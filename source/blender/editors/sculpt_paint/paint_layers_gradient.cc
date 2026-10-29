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
#include "BKE_material.hh"
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
#include "BLI_uuid.h"
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
  /* A Gradient mask correction is the active item of its row's mask; the row itself may hold no
   * gradient at all. */
  MaterialPaintLayer *layer = BKE_paint_layers_active_mask_item_get(*ma);
  if (layer == nullptr) {
    /* While the Mask is what a stroke paints, only a correction's own line is editable: the base
     * mask has none, so the row's gradient must offer neither its line nor its gizmo for it. */
    const Scene *scene = CTX_data_scene(C);
    const PaintModeSettings *paint_mode = nullptr;
    if (scene != nullptr && scene->toolsettings != nullptr) {
      paint_mode = &scene->toolsettings->paint_mode;
    }
    if (BKE_paint_layers_target_mode(paint_mode) == PaintLayersTargetMode::Mask) {
      return nullptr;
    }
    layer = BKE_paint_layers_active_layer_get(*ma);
  }
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
  r_hit.tri = hit.index;
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

/**
 * The object-space point of \a uv on the active UV map. A point outside every island, like the
 * default line along the border of the UV square, is placed on the nearest island point instead, so
 * the handle of a UV row stays reachable and dragging it onto the surface fixes the line.
 */
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

  /* Barycentric weights of \a point in triangle \a index; false for a degenerate one. */
  const auto triangle_weights = [&](const int index, const float2 &point, float3 &r_weights) {
    const int3 tri = corner_tris[index];
    const float2 a = uv_map[tri.x];
    const float2 b = uv_map[tri.y];
    const float2 c = uv_map[tri.z];
    const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::abs(denominator) <= 1e-12f) {
      return false;
    }
    r_weights.x = ((b.y - c.y) * (point.x - c.x) + (c.x - b.x) * (point.y - c.y)) / denominator;
    r_weights.y = ((c.y - a.y) * (point.x - c.x) + (a.x - c.x) * (point.y - c.y)) / denominator;
    r_weights.z = 1.0f - r_weights.x - r_weights.y;
    return true;
  };
  const auto triangle_position = [&](const int index, const float3 &weights) {
    const int3 tri = corner_tris[index];
    return positions[corner_verts[tri.x]] * weights.x + positions[corner_verts[tri.y]] * weights.y +
           positions[corner_verts[tri.z]] * weights.z;
  };
  const auto triangle_point = [&](const int index, float3 &r_point) {
    float3 weights;
    constexpr float epsilon = -1e-5f;
    if (!triangle_weights(index, uv, weights) || weights.x < epsilon || weights.y < epsilon ||
        weights.z < epsilon)
    {
      return false;
    }
    r_point = triangle_position(index, weights);
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

  /* Outside every island: the closest point of the closest triangle, measured in UV space. */
  float best_distance = FLT_MAX;
  int best_index = -1;
  float3 best_closest(0.0f);
  const float3 uv_point(uv.x, uv.y, 0.0f);
  for (const int index : IndexRange(corner_tris.size())) {
    const int3 tri = corner_tris[index];
    float closest[3];
    closest_on_tri_to_point_v3(closest,
                               uv_point,
                               float3(uv_map[tri.x].x, uv_map[tri.x].y, 0.0f),
                               float3(uv_map[tri.y].x, uv_map[tri.y].y, 0.0f),
                               float3(uv_map[tri.z].x, uv_map[tri.z].y, 0.0f));
    const float distance = math::distance_squared(float3(closest), uv_point);
    if (distance < best_distance) {
      best_distance = distance;
      best_index = index;
      best_closest = float3(closest);
    }
  }
  float3 weights;
  if (best_index < 0 || !triangle_weights(best_index, float2(best_closest.x, best_closest.y), weights))
  {
    r_tri_cache = -1;
    return false;
  }
  r_tri_cache = best_index;
  r_object = triangle_position(best_index, weights);
  return true;
}

bool paint_layer_gradient_uv_on_tri_plane(const bContext &C,
                                          Object &ob,
                                          const Mesh &mesh,
                                          const int tri_index,
                                          const float2 &mval,
                                          float2 &r_uv)
{
  const Span<int3> corner_tris = mesh.corner_tris();
  if (tri_index < 0 || tri_index >= corner_tris.size()) {
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
  float ray_start[3];
  float ray_direction[3];
  if (!paint_layer_gradient_object_ray(C, ob, mval, ray_start, ray_direction)) {
    return false;
  }

  const int3 tri = corner_tris[tri_index];
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = mesh.vert_positions();
  const float3 &p0 = positions[corner_verts[tri.x]];
  const float3 &p1 = positions[corner_verts[tri.y]];
  const float3 &p2 = positions[corner_verts[tri.z]];

  float plane[4];
  if (normal_tri_v3(plane, p0, p1, p2) == 0.0f) {
    return false;
  }
  plane[3] = -math::dot(float3(plane), p0);
  float lambda;
  if (!isect_ray_plane_v3(ray_start, ray_direction, plane, &lambda, false)) {
    return false;
  }
  const float3 hit = float3(ray_start) + float3(ray_direction) * lambda;

  /* The weights are not clamped: past the triangle the UV continues along the same mapping, which
   * is what lets the line leave the island and the mesh. */
  float weights[3];
  interp_weights_tri_v3(weights, p0, p1, p2, hit);
  r_uv = uv_map[tri.x] * weights[0] + uv_map[tri.y] * weights[1] + uv_map[tri.z] * weights[2];
  return std::isfinite(r_uv.x) && std::isfinite(r_uv.y);
}

bool paint_layer_gradient_object_point(const Object &ob,
                                       const Mesh &mesh,
                                       const MaterialPaintLayerGradient &gradient,
                                       const float3 &point,
                                       int &r_tri_cache,
                                       float3 &r_object)
{
  switch (eMaterialPaintLayerGradientSpace(gradient.space)) {
    case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
      r_object = point;
      return true;
    case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
      return BKE_paint_layers_gradient_generated_to_object(&ob, mesh, point, r_object);
    case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
    default:
      return paint_layer_gradient_uv_to_object(
          mesh, float2(point.x, point.y), r_tri_cache, r_object);
  }
}

bool paint_layer_gradient_world_point(const Object &ob,
                                      const Mesh &mesh,
                                      const MaterialPaintLayerGradient &gradient,
                                      const float3 &point,
                                      int &r_tri_cache,
                                      float3 &r_world)
{
  float3 object_point;
  if (!paint_layer_gradient_object_point(ob, mesh, gradient, point, r_tri_cache, object_point)) {
    return false;
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

namespace {

bool paint_layer_gradient_operator_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  Material *ma = (ob != nullptr) ? BKE_paint_layers_active_material_get(ob) : nullptr;
  return ma != nullptr && ID_IS_EDITABLE(&ma->id);
}

/**
 * The row or mask item an operator's buttons were drawn for. The panel can show an item that is
 * not the active one (the base mask while Mask mode edits a layer), so the caller passes its
 * stable marker; without one, the active item is used.
 */
bool paint_layer_gradient_operator_target(bContext *C,
                                          wmOperator *op,
                                          Material *&r_material,
                                          MaterialPaintLayer *&r_layer)
{
  Object *active_object = CTX_data_active_object(C);
  r_material = (active_object != nullptr) ?
                   BKE_paint_layers_active_material_get(active_object) :
                   nullptr;
  if (r_material == nullptr || !ID_IS_EDITABLE(&r_material->id)) {
    BKE_report(op->reports, RPT_WARNING, "Select a layered material first");
    return false;
  }

  r_layer = nullptr;
  char marker_buffer[64] = {};
  RNA_string_get(op->ptr, "marker", marker_buffer);
  bUUID marker;
  if (marker_buffer[0] != '\0' && BLI_uuid_parse_string(&marker, marker_buffer)) {
    r_layer = BKE_paint_layers_find(*r_material, marker);
  }
  if (r_layer == nullptr) {
    r_layer = BKE_paint_layers_active_mask_item_get(*r_material);
    if (r_layer == nullptr) {
      r_layer = BKE_paint_layers_active_layer_get(*r_material);
    }
  }
  if (r_layer == nullptr || !BKE_paint_layers_gradient_supported(*r_layer) ||
      r_layer->gradient == nullptr)
  {
    BKE_report(op->reports, RPT_WARNING, "Select a Gradient layer first");
    return false;
  }
  return true;
}

wmOperatorStatus paint_layer_gradient_space_set_exec(bContext *C, wmOperator *op)
{
  Object *active_object = CTX_data_active_object(C);
  Material *ma = nullptr;
  MaterialPaintLayer *layer = nullptr;
  if (!paint_layer_gradient_operator_target(C, op, ma, layer)) {
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintLayerGradientSpace space = eMaterialPaintLayerGradientSpace(
      RNA_enum_get(op->ptr, "space"));
  if (layer->gradient->space == space) {
    return OPERATOR_CANCELLED;
  }

  /* The active object is the one the handles are shown on. A material may be used by several
   * objects, and the active one may not use it at all: then there is no surface to measure the
   * line against, and the numbers stay as they are. */
  const bool uses_material = active_object != nullptr && active_object->type == OB_MESH &&
                             BKE_object_material_slot_find_index(active_object, ma) >= 0;
  const Mesh *mesh = uses_material ? paint_layer_gradient_mesh_eval(*C, *active_object) : nullptr;

  bool converted = false;
  if (!BKE_paint_layers_gradient_convert_space(
          *ma, *layer, uses_material ? active_object : nullptr, mesh, space, converted))
  {
    BKE_report(op->reports, RPT_ERROR, "Could not change the gradient space");
    return OPERATOR_CANCELLED;
  }

  if (!converted) {
    if (!uses_material) {
      BKE_report(op->reports,
                 RPT_WARNING,
                 "The active object does not use this material: the line numbers were not "
                 "converted");
    }
    else {
      BKE_report(op->reports,
                 RPT_WARNING,
                 "No surface point could be found: the line numbers were not converted");
    }
  }

  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  ED_undo_push(C, "Set Gradient Space");
  return OPERATOR_FINISHED;
}

const EnumPropertyItem *paint_layer_gradient_space_items()
{
  static const EnumPropertyItem items[] = {
      {MA_PAINT_LAYER_GRADIENT_SPACE_UV, "UV", 0, "UV", "The active UV map; Z is ignored"},
      {MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT,
       "OBJECT",
       0,
       "Object",
       "Object-space coordinates, in object units"},
      {MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED,
       "GENERATED",
       0,
       "Generated",
       "Generated coordinates, normalized to the object bounding box"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return items;
}

const EnumPropertyItem *paint_layer_gradient_value_channel_items()
{
  static const EnumPropertyItem items[] = {
      {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metal", "Metallic value ramp"},
      {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Rough", "Roughness value ramp"},
      {PAINT_MATERIAL_CHANNEL_ALPHA, "ALPHA", 0, "Alpha", "Alpha value ramp"},
      {PAINT_MATERIAL_CHANNEL_AO, "AO", 0, "AO", "Ambient occlusion value ramp"},
      {PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Spec", "Specular value ramp"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return items;
}

const EnumPropertyItem *paint_layer_gradient_gizmo_channel_items()
{
  static const EnumPropertyItem items[] = {
      {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", "Colour ramp"},
      {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metal", "Value ramp"},
      {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Rough", "Value ramp"},
      {PAINT_MATERIAL_CHANNEL_ALPHA, "ALPHA", 0, "Alpha", "Value ramp"},
      {PAINT_MATERIAL_CHANNEL_AO, "AO", 0, "AO", "Value ramp"},
      {PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emit", "Colour ramp"},
      {PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Spec", "Value ramp"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return items;
}

wmOperatorStatus paint_layer_gradient_set_gizmo_channel_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  MaterialPaintLayer *layer = nullptr;
  if (!paint_layer_gradient_operator_target(C, op, ma, layer)) {
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  if (!BKE_paint_layers_gradient_set_gizmo_channel(*ma, *layer, channel)) {
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

wmOperatorStatus paint_layer_gradient_sync_channel_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  MaterialPaintLayer *layer = nullptr;
  if (!paint_layer_gradient_operator_target(C, op, ma, layer)) {
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  if (!BKE_paint_layers_gradient_sync_channel_ramp(*ma, *layer, channel)) {
    BKE_report(op->reports, RPT_WARNING, "This channel has no value ramp to sync");
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  ED_undo_push(C, "Sync Gradient Value Ramp");
  return OPERATOR_FINISHED;
}

}  // namespace

void PAINT_OT_paint_layer_gradient_space_set(wmOperatorType *ot)
{
  ot->name = "Set Gradient Space";
  ot->idname = "PAINT_OT_paint_layer_gradient_space_set";
  ot->description =
      "Switch the Gradient's space, keeping its line on the same place of the active object's "
      "surface";

  ot->exec = paint_layer_gradient_space_set_exec;
  ot->poll = paint_layer_gradient_operator_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "space",
               paint_layer_gradient_space_items(),
               MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT,
               "Space",
               "Space the gradient's line is defined in");

  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "marker",
                                     nullptr,
                                     64,
                                     "Marker",
                                     "Stable identity of the row or mask item the buttons edit");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

void PAINT_OT_paint_layer_gradient_set_gizmo_channel(wmOperatorType *ot)
{
  ot->name = "Set Gradient Gizmo Channel";
  ot->idname = "PAINT_OT_paint_layer_gradient_set_gizmo_channel";
  ot->description = "Show and move this channel's ramp on the Gradient handles";

  ot->exec = paint_layer_gradient_set_gizmo_channel_exec;
  ot->poll = paint_layer_gradient_operator_poll;

  ot->flag = OPTYPE_REGISTER;

  RNA_def_enum(ot->srna,
               "channel",
               paint_layer_gradient_gizmo_channel_items(),
               PAINT_MATERIAL_CHANNEL_BASE_COLOR,
               "Channel",
               "Channel whose ramp the handles show");

  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "marker",
                                     nullptr,
                                     64,
                                     "Marker",
                                     "Stable identity of the row or mask item the buttons edit");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

void PAINT_OT_paint_layer_gradient_sync_channel(wmOperatorType *ot)
{
  ot->name = "Sync Gradient Value Ramp";
  ot->idname = "PAINT_OT_paint_layer_gradient_sync_channel";
  ot->description =
      "Give the channel's value ramp the same stop count and positions as the Base Color ramp";

  ot->exec = paint_layer_gradient_sync_channel_exec;
  ot->poll = paint_layer_gradient_operator_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "channel",
               paint_layer_gradient_value_channel_items(),
               PAINT_MATERIAL_CHANNEL_METALLIC,
               "Channel",
               "Scalar channel whose value ramp is synced");

  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "marker",
                                     nullptr,
                                     64,
                                     "Marker",
                                     "Stable identity of the row or mask item the buttons edit");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender
