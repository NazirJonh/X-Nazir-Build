/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Gradient Layer tool. The active Gradient row is procedural: this operator draws its line in
 * the space the row is authored in (UV, object or generated) and writes the start/end into the row.
 * The generated shader nodes follow through the ordinary value sync, so the viewport and the Image
 * Editor see the same ramp without a map being painted.
 */

#include <cfloat>
#include <cmath>

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"

#include "BLI_index_range.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"
#include "BLI_virtual_array.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_layers_gradient.hh"

namespace blender {

/** The active layered material and its active Gradient row, or null. */
Material *paint_layer_gradient_target(const bContext *C, MaterialPaintLayer **r_layer)
{
  *r_layer = nullptr;
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return nullptr;
  }
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr || !ID_IS_EDITABLE(&ma->id)) {
    return nullptr;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_active_layer_get(*ma);
  if (layer == nullptr || !BKE_paint_layers_gradient_supported(*layer) ||
      layer->gradient == nullptr)
  {
    return nullptr;
  }
  *r_layer = layer;
  return ma;
}

/**
 * The view ray under \a mval, transformed into \a ob's object space. The ray direction is
 * normalized.
 */
static bool paint_layer_gradient_object_ray(const bContext &C,
                                            Object &ob,
                                            const float2 &mval,
                                            float r_start[3],
                                            float r_direction[3])
{
  const ARegion *region = CTX_wm_region(&C);
  const View3D *v3d = CTX_wm_view3d(&C);
  if (region == nullptr || v3d == nullptr) {
    return false;
  }
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
  const float mval_xy[2] = {mval.x, mval.y};
  float ray_start[3];
  float ray_normal[3];
  if (!ED_view3d_win_to_ray_clipped(
          depsgraph, region, v3d, mval_xy, ray_start, ray_normal, true))
  {
    return false;
  }
  float imat[4][4];
  if (!invert_m4_m4(imat, ob.object_to_world().ptr())) {
    return false;
  }
  mul_v3_m4v3(r_start, imat, ray_start);
  copy_v3_v3(r_direction, ray_normal);
  mul_mat3_m4_v3(imat, r_direction);
  return normalize_v3(r_direction) != 0.0f;
}

const Mesh *paint_layer_gradient_mesh_eval(const bContext &C, const Object &ob)
{
  const Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(&C);
  if (depsgraph == nullptr) {
    return nullptr;
  }
  const Object *ob_eval = DEG_get_evaluated<Object>(depsgraph, &ob);
  return ob_eval != nullptr ? BKE_object_get_evaluated_mesh(ob_eval) : nullptr;
}

/**
 * Raycast \a ob's evaluated mesh under \a mval, the geometry the material is shaded on. The BVH is
 * built in object space, so the hit position and normal need no further transform. The UV is
 * interpolated at the hit triangle from the render-active UV map.
 */
static bool paint_layer_gradient_hit_on_mesh(const bContext &C,
                                             Object &ob,
                                             const float2 &mval,
                                             PaintLayerGradientHit &r_hit)
{
  const Mesh *mesh = paint_layer_gradient_mesh_eval(C, ob);
  if (mesh == nullptr) {
    return false;
  }
  float ray_start_ob[3];
  float ray_normal_ob[3];
  if (!paint_layer_gradient_object_ray(C, ob, mval, ray_start_ob, ray_normal_ob)) {
    return false;
  }

  bke::BVHTreeFromMesh tree_data = mesh->bvh_corner_tris_no_hidden();
  if (tree_data.tree == nullptr) {
    return false;
  }
  BVHTreeRayHit hit{};
  hit.index = -1;
  hit.dist = BVH_RAYCAST_DIST_MAX;
  BLI_bvhtree_ray_cast(tree_data.tree,
                       ray_start_ob,
                       ray_normal_ob,
                       0.0f,
                       &hit,
                       tree_data.raycast_callback,
                       &tree_data);
  if (hit.index < 0) {
    return false;
  }

  r_hit.position = float3(hit.co);
  r_hit.valid = true;

  float normal[3];
  copy_v3_v3(normal, hit.no);
  if (normalize_v3(normal) != 0.0f) {
    r_hit.normal = float3(normal);
    r_hit.has_normal = true;
  }

  const Span<int3> corner_tris = mesh->corner_tris();
  if (hit.index >= corner_tris.size()) {
    return true;
  }
  const int3 tri = corner_tris[hit.index];
  const bke::AttributeReader<float2> uv_attribute = mesh->attributes().lookup<float2>(
      BKE_paint_layers_gradient_uv_map_name(*mesh), bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return true;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  if (uv_map.is_empty()) {
    return true;
  }
  const float3 bary = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
      mesh->vert_positions(), mesh->corner_verts(), tri, float3(hit.co));
  r_hit.uv = bke::mesh_surface_sample::sample_corner_attribute_with_bary_coords(bary, tri, uv_map);
  r_hit.has_uv = true;
  return true;
}

/**
 * The sampled point under \a mval. The Image Editor is UV-only; the 3D View raycasts the active
 * mesh and reports object-space position, normal and the active UV map coordinate.
 */
bool paint_layer_gradient_hit_at_mouse(bContext &C,
                                       const MaterialPaintLayerGradient &gradient,
                                       const float2 &mval,
                                       PaintLayerGradientHit &r_hit)
{
  r_hit = PaintLayerGradientHit();
  if (CTX_wm_space_image(&C) != nullptr) {
    if (gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
      return false;
    }
    const ARegion *region = CTX_wm_region(&C);
    if (region == nullptr) {
      return false;
    }
    float2 uv;
    ui::view2d_region_to_view(&region->v2d, mval.x, mval.y, &uv.x, &uv.y);
    r_hit.uv = uv;
    r_hit.position = float3(uv.x, uv.y, 0.0f);
    r_hit.has_uv = true;
    r_hit.valid = true;
    return true;
  }
  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || ob->type != OB_MESH) {
    return false;
  }
  return paint_layer_gradient_hit_on_mesh(C, *ob, mval, r_hit);
}

/** Convert a sampled hit into the row's own space. */
bool paint_layer_gradient_point_from_hit(const MaterialPaintLayerGradient &gradient,
                                         const Object *object,
                                         const Mesh *mesh,
                                         const PaintLayerGradientHit &hit,
                                         float3 &r_point)
{
  if (!hit.valid) {
    return false;
  }
  switch (gradient.space) {
    case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
      if (!hit.has_uv) {
        return false;
      }
      r_point = float3(hit.uv.x, hit.uv.y, 0.0f);
      return true;
    case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
      r_point = hit.position;
      return true;
    case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
      if (mesh == nullptr) {
        return false;
      }
      return BKE_paint_layers_gradient_generated_from_object(
          object, *mesh, hit.position, r_point);
  }
  return false;
}

bool paint_layer_gradient_point_at_mouse(bContext &C,
                                         const MaterialPaintLayerGradient &gradient,
                                         Object *ob,
                                         const Mesh *mesh,
                                         const float2 &mval,
                                         const PaintLayerGradientHit *plane_source,
                                         float3 &r_point)
{
  PaintLayerGradientHit hit;
  if (paint_layer_gradient_hit_at_mouse(C, gradient, mval, hit) && hit.valid &&
      paint_layer_gradient_point_from_hit(gradient, ob, mesh, hit, r_point))
  {
    return true;
  }
  /* The cursor is off the mesh: the tangent plane of the earlier surface point still gives a point
   * in the row's space. A UV coordinate cannot be recovered that way. */
  if (plane_source == nullptr || ob == nullptr || mesh == nullptr ||
      gradient.space == MA_PAINT_LAYER_GRADIENT_SPACE_UV)
  {
    return false;
  }
  PaintLayerGradientHit fallback;
  if (!paint_layer_gradient_plane_hit(C, *ob, *plane_source, mval, fallback.position)) {
    return false;
  }
  fallback.valid = true;
  return paint_layer_gradient_point_from_hit(gradient, ob, mesh, fallback, r_point);
}

/**
 * Fallback for a drag whose end misses the mesh: intersect the end view ray with the tangent plane
 * of the first hit. The result is an object-space surface point.
 */
bool paint_layer_gradient_plane_hit(const bContext &C,
                                    Object &ob,
                                    const PaintLayerGradientHit &start_hit,
                                    const float2 &mval,
                                    float3 &r_object_point)
{
  if (!start_hit.valid || !start_hit.has_normal) {
    return false;
  }
  float ray_start[3];
  float ray_direction[3];
  if (!paint_layer_gradient_object_ray(C, ob, mval, ray_start, ray_direction)) {
    return false;
  }
  float plane[4];
  copy_v3_v3(plane, start_hit.normal);
  plane[3] = -dot_v3v3(start_hit.normal, start_hit.position);
  float lambda;
  if (!isect_ray_plane_v3(ray_start, ray_direction, plane, &lambda, true)) {
    return false;
  }
  r_object_point = start_hit.position + float3(ray_direction) * lambda;
  return true;
}

bool paint_layer_gradient_active(const bContext *C)
{
  MaterialPaintLayer *layer = nullptr;
  return paint_layer_gradient_target(C, &layer) != nullptr && layer != nullptr;
}

/** The object-space point of \a uv on the active UV map, or false outside its islands. */
static bool paint_layer_gradient_uv_to_object(const Mesh &mesh,
                                              const float2 &uv,
                                              int &r_tri_cache,
                                              float3 &r_object)
{
  const Span<int3> corner_tris = mesh.corner_tris();
  if (corner_tris.is_empty()) {
    return false;
  }
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      BKE_paint_layers_gradient_uv_map_name(mesh), bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return false;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  if (uv_map.is_empty()) {
    return false;
  }
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = mesh.vert_positions();

  auto triangle_point = [&](const int index, float3 &r_point) -> bool {
    const int3 tri = corner_tris[index];
    const float2 a = uv_map[tri.x];
    const float2 b = uv_map[tri.y];
    const float2 c = uv_map[tri.z];
    const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::abs(denominator) <= 1e-12f) {
      return false;
    }
    const float w0 = ((b.y - c.y) * (uv.x - c.x) + (c.x - b.x) * (uv.y - c.y)) / denominator;
    const float w1 = ((c.y - a.y) * (uv.x - c.x) + (a.x - c.x) * (uv.y - c.y)) / denominator;
    const float w2 = 1.0f - w0 - w1;
    constexpr float epsilon = -1e-5f;
    if (w0 < epsilon || w1 < epsilon || w2 < epsilon) {
      return false;
    }
    r_point = positions[corner_verts[tri.x]] * w0 + positions[corner_verts[tri.y]] * w1 +
              positions[corner_verts[tri.z]] * w2;
    return true;
  };

  /* The triangle a previous call found usually still covers the point, which makes the per-frame
   * projection constant time; a UV edit or another row falls back to the scan. */
  if (r_tri_cache >= 0 && r_tri_cache < corner_tris.size() &&
      triangle_point(r_tri_cache, r_object))
  {
    return true;
  }
  for (const int index : IndexRange(corner_tris.size())) {
    if (index == r_tri_cache) {
      continue;
    }
    if (triangle_point(index, r_object)) {
      r_tri_cache = index;
      return true;
    }
  }
  r_tri_cache = -1;
  return false;
}

bool paint_layer_gradient_world_point(const Object &ob,
                                      const Mesh &mesh,
                                      const MaterialPaintLayerGradient &gradient,
                                      const float3 &point,
                                      int &r_tri_cache,
                                      float3 &r_world)
{
  float3 object_point;
  switch (eMaterialPaintLayerGradientSpace(gradient.space)) {
    case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
      object_point = point;
      break;
    case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
      if (!BKE_paint_layers_gradient_generated_to_object(&ob, mesh, point, object_point)) {
        return false;
      }
      break;
    case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
    default:
      if (!paint_layer_gradient_uv_to_object(
              mesh, float2(point.x, point.y), r_tri_cache, object_point))
      {
        return false;
      }
      break;
  }
  r_world = math::transform_point(ob.object_to_world(), object_point);
  return true;
}

namespace {

bool paint_layer_gradient_poll(bContext *C)
{
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  MaterialPaintLayer *layer = nullptr;
  Material *ma = paint_layer_gradient_target(C, &layer);
  if (ma == nullptr || layer == nullptr) {
    return false;
  }
  if (const SpaceImage *sima = CTX_wm_space_image(C)) {
    return sima->mode == SI_MODE_PAINT &&
           layer->gradient->space == MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  }
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH || CTX_wm_view3d(C) == nullptr) {
    return false;
  }
  return (ob->mode & (OB_MODE_SCULPT | OB_MODE_TEXTURE_PAINT)) != 0;
}

/** Defined below; #exec restores a live gesture's line before cancelling. */
void paint_layer_gradient_restore(bContext *C, wmOperator *op);

/** Cancel \a exec, putting the line of a live gesture back first. A direct call does nothing. */
wmOperatorStatus paint_layer_gradient_exec_cancelled(bContext *C, wmOperator *op)
{
  if (op->customdata != nullptr) {
    paint_layer_gradient_restore(C, op);
  }
  return OPERATOR_CANCELLED;
}

wmOperatorStatus paint_layer_gradient_exec(bContext *C, wmOperator *op)
{
  MaterialPaintLayer *layer = nullptr;
  Material *ma = paint_layer_gradient_target(C, &layer);
  if (ma == nullptr || layer == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Select a Gradient layer first");
    return paint_layer_gradient_exec_cancelled(C, op);
  }
  const MaterialPaintLayerGradient &gradient = *layer->gradient;
  const float2 start(float(RNA_int_get(op->ptr, "xstart")),
                     float(RNA_int_get(op->ptr, "ystart")));
  const float2 end(float(RNA_int_get(op->ptr, "xend")), float(RNA_int_get(op->ptr, "yend")));

  PaintLayerGradientHit start_hit;
  if (!paint_layer_gradient_hit_at_mouse(*C, gradient, start, start_hit) || !start_hit.valid) {
    return paint_layer_gradient_exec_cancelled(C, op);
  }

  Object *ob = CTX_data_active_object(C);
  const Mesh *mesh = (ob != nullptr && ob->type == OB_MESH) ? paint_layer_gradient_mesh_eval(*C, *ob) :
                                                             nullptr;

  float3 start_point;
  if (!paint_layer_gradient_point_from_hit(gradient, ob, mesh, start_hit, start_point)) {
    return paint_layer_gradient_exec_cancelled(C, op);
  }

  float3 end_point;
  if (!paint_layer_gradient_point_at_mouse(
          *C, gradient, ob, mesh, end, &start_hit, end_point))
  {
    return paint_layer_gradient_exec_cancelled(C, op);
  }

  /* The straight-line gesture exposes a flip toggle; swapping the endpoints reverses the ramp
   * direction of a linear gradient. A radial gradient's start is its centre, so swapping would move
   * the centre instead of flipping it: ignore flip there. */
  if (gradient.type == MA_PAINT_LAYER_GRADIENT_LINEAR && RNA_boolean_get(op->ptr, "flip")) {
    const float3 tmp = start_point;
    start_point = end_point;
    end_point = tmp;
  }
  const float start_array[3] = {start_point.x, start_point.y, start_point.z};
  const float end_array[3] = {end_point.x, end_point.y, end_point.z};
  if (equals_v3v3(start_array, end_array)) {
    return paint_layer_gradient_exec_cancelled(C, op);
  }
  if (!BKE_paint_layers_gradient_set_line(*ma, *layer, start_array, end_array)) {
    return paint_layer_gradient_exec_cancelled(C, op);
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  /* A live gesture pushes its single undo step when the modal ends; a direct exec owns its own. */
  if (op->customdata == nullptr) {
    ED_undo_push(C, "Set Gradient Line");
  }
  return OPERATOR_FINISHED;
}

/** Put the row's line back after a cancelled gesture. */
void paint_layer_gradient_restore(bContext *C, wmOperator *op)
{
  MaterialPaintLayer *layer = nullptr;
  Material *ma = paint_layer_gradient_target(C, &layer);
  if (ma == nullptr || layer == nullptr) {
    return;
  }
  float start[3];
  float end[3];
  RNA_float_get_array(op->ptr, "original_start", start);
  RNA_float_get_array(op->ptr, "original_end", end);
  if (BKE_paint_layers_gradient_set_line(*ma, *layer, start, end)) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
    if (ARegion *region = CTX_wm_region(C)) {
      ED_region_tag_redraw(region);
    }
  }
}

/**
 * Whether the live line still equals the line the gesture started with. A failed end point
 * restores the original line, so the gesture finishes without leaving an undo step.
 */
bool paint_layer_gradient_line_matches_original(bContext *C, wmOperator *op)
{
  MaterialPaintLayer *layer = nullptr;
  Material *ma = paint_layer_gradient_target(C, &layer);
  if (ma == nullptr || layer == nullptr) {
    return true;
  }
  float original_start[3];
  float original_end[3];
  RNA_float_get_array(op->ptr, "original_start", original_start);
  RNA_float_get_array(op->ptr, "original_end", original_end);
  return equals_v3v3(layer->gradient->start, original_start) &&
         equals_v3v3(layer->gradient->end, original_end);
}

wmOperatorStatus paint_layer_gradient_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  const wmOperatorStatus ret = WM_gesture_straightline_modal(C, op, event);
  if (ret & OPERATOR_CANCELLED) {
    /* Straightline can cancel for more than an explicit Esc, e.g. a degenerate final rect. Put the
     * original line back so a cancelled gesture never leaves its last live preview behind. */
    paint_layer_gradient_restore(C, op);
  }
  else if ((ret & OPERATOR_FINISHED) && !paint_layer_gradient_line_matches_original(C, op)) {
    ED_undo_push(C, "Set Gradient Line");
  }
  return ret;
}

/** External teardown (window close, file load): put the row back and free the gesture. */
void paint_layer_gradient_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata == nullptr) {
    return;
  }
  paint_layer_gradient_restore(C, op);
  WM_gesture_straightline_cancel(C, op);
}

wmOperatorStatus paint_layer_gradient_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  MaterialPaintLayer *layer = nullptr;
  Material *ma = paint_layer_gradient_target(C, &layer);
  if (ma == nullptr || layer == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Select a Gradient layer first");
    return OPERATOR_CANCELLED;
  }
  /* Kept in the operator so a cancelled drag can put the row back without an undo step. */
  RNA_float_set_array(op->ptr, "original_start", layer->gradient->start);
  RNA_float_set_array(op->ptr, "original_end", layer->gradient->end);
  return WM_gesture_straightline_invoke(C, op, event);
}

}  // namespace

void PAINT_OT_paint_layer_gradient(wmOperatorType *ot)
{
  ot->name = "Paint Layer Gradient";
  ot->idname = "PAINT_OT_paint_layer_gradient";
  ot->description =
      "Draw the line of the active Gradient layer in its space; the layer stays procedural and "
      "live";

  ot->invoke = paint_layer_gradient_invoke;
  ot->exec = paint_layer_gradient_exec;
  ot->modal = paint_layer_gradient_modal;
  ot->cancel = paint_layer_gradient_cancel;
  ot->poll = paint_layer_gradient_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  PropertyRNA *prop = RNA_def_float_vector(ot->srna,
                                           "original_start",
                                           3,
                                           nullptr,
                                           -FLT_MAX,
                                           FLT_MAX,
                                           "Original Start",
                                           "The row's line before the gesture, for cancelling",
                                           -FLT_MAX,
                                           FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_float_vector(ot->srna,
                              "original_end",
                              3,
                              nullptr,
                              -FLT_MAX,
                              FLT_MAX,
                              "Original End",
                              "The row's line before the gesture, for cancelling",
                              -FLT_MAX,
                              FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
}

}  // namespace blender
