/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_clone_stroke.hh"

#include <algorithm>
#include <cfloat>

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_kdopbvh.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_view3d.hh"

#include "IMB_imbuf_types.hh"

#include "paint_intern.hh"

#include "mesh/paint_area_plane_2d.hh"
#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint::clone {

/**
 * Surface-to-UV scale of the hit triangle: sqrt(uv_area / face_area), in UV units per object
 * unit. Turns the brush radius (object space) into a UV-space footprint radius. Falls back to
 * the active UV map when the material canvas does not name one (same policy as
 * #clone_pick_uv). Returns 0 for degenerate triangles / missing UVs.
 */
static float clone_tri_uv_scale(const Mesh *mesh_eval,
                                const ePaintCanvasSource canvas_mode,
                                Object *ob_for_material,
                                const int tri_index)
{
  const Span<int3> tris = mesh_eval->corner_tris();
  const int3 &tri = tris[tri_index];
  const Span<int> corner_verts = mesh_eval->corner_verts();
  const Span<float3> positions = mesh_eval->vert_positions();
  const float co_area = area_tri_v3(positions[corner_verts[tri[0]]],
                                    positions[corner_verts[tri[1]]],
                                    positions[corner_verts[tri[2]]]);
  if (co_area <= 0.0f) {
    return 0.0f;
  }

  const VArraySpan<float2> uv_map = clone_uv_map_get(
      *mesh_eval, canvas_mode, ob_for_material, tri_index);
  if (uv_map.is_empty()) {
    return 0.0f;
  }

  const float2 uv0 = uv_map[tri[0]];
  const float2 uv1 = uv_map[tri[1]];
  const float2 uv2 = uv_map[tri[2]];
  const float uv_area = area_tri_v2(uv0, uv1, uv2);
  return math::sqrt(uv_area / co_area);
}

CloneStrokeRuntime *clone_stroke_runtime_ensure(CloneStrokeRuntime *&owner,
                                                Main *bmain,
                                                const CloneSourcePoint &source,
                                                const Material &ma,
                                                const Brush *brush,
                                                const int visible_material_channels)
{
  if (owner == nullptr) {
    CloneStrokeRuntime *runtime = MEM_new<CloneStrokeRuntime>(__func__);
    runtime->source = source;
    runtime->material = &ma;
    runtime->brush = brush;
    runtime->targets = clone_stroke_targets_build(*bmain, ma, visible_material_channels);
    if (!runtime->targets.is_valid()) {
      MEM_delete(runtime);
      return nullptr;
    }
    owner = runtime;
    return runtime;
  }
  if (owner->material != &ma) {
    CloneStrokeTargets rebuilt = clone_stroke_targets_build(
        *bmain, ma, visible_material_channels);
    if (!rebuilt.is_valid()) {
      return nullptr;
    }
    /* Move-assign releases the previous material's ImBuf locks. */
    owner->targets = std::move(rebuilt);
    owner->material = &ma;
  }
  return owner;
}

void clone_stroke_runtime_free(CloneStrokeRuntime *&owner)
{
  MEM_delete(owner);
  owner = nullptr;
}

bool clone_dab_is_symmetry_duplicate(const CloneStrokeRuntime &runtime,
                                     const float2 &dest_uv,
                                     const float uv_radius)
{
  if (!runtime.main_pass_dest_uv_valid) {
    return false;
  }
  return math::distance(dest_uv, runtime.main_pass_dest_uv) < uv_radius;
}

/* -------------------------------------------------------------------- */
/** \name Projection-paint hooks.
 * \{ */

void clone_pbr_proj_dab(const bContext *C,
                        CloneStrokeRuntime *&owner,
                        const float mouse[2],
                        const float pressure,
                        const bool eraser)
{
  if (C == nullptr) {
    return;
  }
  if (eraser) {
    return; /* No erase semantic for clone (matches the sculpt path). */
  }
  Scene *scene = CTX_data_scene(C);
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  if (scene == nullptr || bmain == nullptr || ob == nullptr || ob->data == nullptr) {
    return;
  }
  if (ob->type != OB_MESH) {
    return;
  }
  if (clone_dyntopo_active(ob)) {
    return; /* dyntopo blocked */
  }
  const CloneSourcePoint *source = clone_source_point_get(ob);
  if (source == nullptr) {
    return; /* No PBR source: legacy single-image clone path runs alone. */
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  View3D *v3d = CTX_wm_view3d(C);
  ARegion *region = CTX_wm_region(C);
  if (depsgraph == nullptr || v3d == nullptr || region == nullptr) {
    return; /* Projection paint is 3D Viewport only. */
  }
  ViewContext vc = ED_view3d_viewcontext_init(const_cast<bContext *>(C), depsgraph);
  Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (mesh_eval == nullptr) {
    return;
  }
  const int mval[2] = {int(mouse[0]), int(mouse[1])};
  int tri_index = -1;
  int face_index = -1;
  float3 bary_coord;
  float co_arr[3] = {0.0f, 0.0f, 0.0f};
  float no_arr[3] = {0.0f, 0.0f, 1.0f};
  float3 co_object(0.0f);
  if (!clone_pick_face(&vc,
                       mval,
                       &tri_index,
                       &face_index,
                       &bary_coord,
                       co_arr,
                       no_arr,
                       &co_object,
                       *mesh_eval))
  {
    return;
  }
  const float2 dest_uv = clone_pick_uv(mesh_eval,
                                       ePaintCanvasSource(scene->toolsettings->imapaint.mode),
                                       ob_eval,
                                       tri_index,
                                       bary_coord);

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  if (ma == nullptr) {
    return;
  }
  /* The source UV was captured in a specific UV map; if the active map changed since, the
   * offset would be applied in a different coordinate space. */
  if (!source->uv_layer_name.empty() && mesh_eval->active_uv_map_name() != source->uv_layer_name)
  {
    return;
  }
  Brush *brush = BKE_paint_brush(paint);

  /* Brush footprint in UV units, from the brush radius in canvas pixels divided by the active
   * canvas width. The stamp stays geometrically identical across layers/channels because the
   * radius is carried in UV space, not in per-image pixels. */
  int canvas_w = 0;
  if (ma->texpaintslot != nullptr) {
    Image *slot_image = ma->texpaintslot[ma->paint_active_slot].ima;
    if (slot_image != nullptr) {
      int canvas_h = 0;
      BKE_image_get_size(slot_image, nullptr, &canvas_w, &canvas_h);
    }
  }
  if (canvas_w <= 0) {
    canvas_w = 1024;
  }
  const float uv_radius = (BKE_brush_radius_get(paint, brush)) / float(canvas_w);
  if (uv_radius <= 0.0f) {
    return;
  }

  /* NOTE: undo is owned by the outer ImagePaintStroke (push_begin at stroke start,
   * push_end at done). We must NOT open a nested transaction here. */
  CloneStrokeRuntime *runtime = clone_stroke_runtime_ensure(
      owner, bmain, *source, *ma, brush, paint->visible_material_channels);
  if (runtime == nullptr) {
    return;
  }
  /* Only the destination frame is rebuilt here; the source half was frozen when the source was
   * picked, which is what lets a viewport rotation reach the stamp. */
  CloneDabTransform transform;
  if (vc.rv3d != nullptr) {
    transform = clone_dab_transform_build(*mesh_eval,
                                          ePaintCanvasSource(scene->toolsettings->imapaint.mode),
                                          ob_eval,
                                          source->frame,
                                          tri_index,
                                          region,
                                          ED_view3d_ob_project_mat_get(vc.rv3d, ob_eval),
                                          clone_view_right_get(vc, *ob));
  }

  /* Writable, because Relative fixes its anchor on the first dab. */
  CloneSourcePoint *source_mut = clone_source_point_get_for_write(ob);
  if (source_mut == nullptr) {
    return;
  }
  const float2 dab_center_uv = clone_dab_center_uv_get(
      *source_mut, dest_uv, co_object, paint->clone_mode);

  const float strength = std::clamp(pressure, 0.0f, 1.0f);
  clone_stroke_apply_dab(runtime,
                         dest_uv,
                         dab_center_uv,
                         transform,
                         uv_radius,
                         paint->visible_material_channels,
                         strength);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt-stroke hooks.
 * \{ */

/**
 * Resolve dab-center UV from an object-space surface location (no ViewContext needed).
 *
 * A nearest-surface query, not a ray cast: #StrokeCache.sculpt_normal is only refreshed for the
 * brush types #sculpt_brush_needs_normal lists, and CLONE is not one of them, so a cast along it
 * would leave from a zero direction and miss on every dab. The dab center already lies on the
 * surface, so the closest triangle is the one the stroke is over.
 */
static bool clone_pick_face_at_location(const Mesh &mesh,
                                        const float3 &location,
                                        int *r_tri_index,
                                        float3 *r_bary_coord)
{
  if (mesh.faces_num == 0) {
    return false;
  }
  bke::BVHTreeFromMesh mesh_bvh = mesh.bvh_corner_tris();
  if (mesh_bvh.tree == nullptr) {
    return false;
  }
  BVHTreeNearest nearest;
  nearest.index = -1;
  nearest.dist_sq = FLT_MAX;
  BLI_bvhtree_find_nearest(
      mesh_bvh.tree, location, &nearest, mesh_bvh.nearest_callback, &mesh_bvh);
  if (nearest.index == -1) {
    return false;
  }
  *r_bary_coord = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
      mesh.vert_positions(), mesh.corner_verts(), mesh.corner_tris()[nearest.index], nearest.co);
  *r_tri_index = nearest.index;
  return true;
}

void sculpt_clone_dab_apply(const Depsgraph &depsgraph,
                            const Sculpt &sd,
                            Object &ob,
                            const Brush &brush,
                            PaintModeSettings &settings,
                            CloneStrokeRuntime *&owner)
{
  if (ob.type != OB_MESH) {
    return;
  }
  SculptSession *ss = (ob.runtime != nullptr) ? ob.runtime->sculpt_session : nullptr;
  if (ss == nullptr || ss->cache == nullptr) {
    return;
  }
  if (ss->bm != nullptr) {
    return; /* Dyntopo is unsupported. */
  }
  const StrokeCache &cache = *ss->cache;
  if (cache.toggle_settings.invert) {
    return; /* No erase semantic for clone. */
  }
  const bool is_main_pass = cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0;
  if (!ELEM(settings.canvas_source, PAINT_CANVAS_SOURCE_MATERIAL, PAINT_CANVAS_SOURCE_IMAGE)) {
    return; /* Clone paints images; attribute canvases are a clean no-op. */
  }
  const CloneSourcePoint *source = clone_source_point_get(&ob);
  if (source == nullptr) {
    return;
  }
  Main *bmain = DEG_get_bmain(&depsgraph);
  if (bmain == nullptr) {
    return;
  }
  Object *ob_eval = DEG_get_evaluated(&depsgraph, &ob);
  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (mesh_eval == nullptr || mesh_eval->uv_map_names().is_empty()) {
    return;
  }
  Material *ma = BKE_object_material_get(&ob, ob.actcol);
  if (ma == nullptr) {
    return;
  }
  /* The source UV was captured in a specific UV map; if the active map changed since, the
   * offset would be applied in a different coordinate space. */
  if (!source->uv_layer_name.empty() && mesh_eval->active_uv_map_name() != source->uv_layer_name)
  {
    return;
  }

  /* #StrokeCache.location_symm is this pass's own dab center: the main pass's point for the main
   * pass, its mirror for each mirrored one. Reading #location instead is what used to force every
   * mirrored pass to be dropped -- they would all have re-stamped the main pass's spot. */
  int tri_index = -1;
  float3 bary_coord;
  if (!clone_pick_face_at_location(*mesh_eval, cache.location_symm, &tri_index, &bary_coord)) {
    return;
  }
  const float2 dest_uv = clone_pick_uv(
      mesh_eval, settings.canvas_source, &ob, tri_index, bary_coord);

  /* Brush radius (object space) -> UV-space footprint via the hit triangle's density. */
  const float uv_scale = clone_tri_uv_scale(mesh_eval, settings.canvas_source, &ob, tri_index);
  if (uv_scale <= 0.0f) {
    return;
  }
  const float uv_radius = cache.radius * uv_scale;

  /* NOTE: undo is owned by the outer sculpt stroke (image-undo when
   * sculpt_brush_uses_image_canvas(); CLONE is registered there). Never nest. */
  CloneStrokeRuntime *runtime = clone_stroke_runtime_ensure(
      owner, bmain, *source, *ma, &brush, sd.paint.visible_material_channels);
  if (runtime == nullptr) {
    return;
  }
  /* Only the destination frame is rebuilt here; the source half was frozen when the source was
   * picked, which is what lets a viewport rotation reach the stamp. #StrokeCache already carries
   * the view in this object's space, refreshed per stroke. A cache with no view context (a
   * scripted stroke) leaves the region null, which #clone_dab_transform_build answers with the
   * plain UV-locked stamp. */
  const ARegion *dab_region = cache.vc != nullptr ? cache.vc->region : nullptr;
  CloneDabTransform transform = clone_dab_transform_build(*mesh_eval,
                                                          settings.canvas_source,
                                                          &ob,
                                                          source->frame,
                                                          tri_index,
                                                          dab_region,
                                                          cache.projection_mat,
                                                          cache.view_right);

  if (is_main_pass) {
    runtime->main_pass_dest_uv = dest_uv;
    runtime->main_pass_dest_uv_valid = true;
    /* Frozen for the mirrored passes of every later dab of the stroke: their sampling map is this
     * transform composed with the mirror, see below. */
    runtime->main_pass_transform = transform;
    const VArraySpan<float2> uv_map = clone_uv_map_get(
        *mesh_eval, settings.canvas_source, &ob, tri_index);
    runtime->main_pass_transform_valid = clone_tri_uv_jacobian(
        *mesh_eval, uv_map, tri_index, runtime->main_pass_dp_du, runtime->main_pass_dp_dv);
  }
  else {
    /* A mirror pass stamps the mirror image of the main pass, which is the main pass's own map
     * composed with the mirror Jacobian -- NOT this triangle's map. On a symmetric mesh the
     * mirrored location's parametrization maps UV to screen the same way the main one does, so
     * its transform alone carries the source's direction into both halves unchanged and the
     * mirrored stroke runs parallel to the original instead of reflected. The frames below
     * (destination tangent basis for the Normal channel) stay this hit's own; only the two
     * sampling maps are replaced. Radial passes are pure rotations and keep the built transform.
     */
    float3 mirror_dp_du = float3(0.0f), mirror_dp_dv = float3(0.0f);
    float2x2 jacobian;
    const VArraySpan<float2> uv_map = clone_uv_map_get(
        *mesh_eval, settings.canvas_source, &ob, tri_index);
    if (cache.mirror_symmetry_pass != 0 && runtime->main_pass_transform_valid &&
        clone_tri_uv_jacobian(*mesh_eval, uv_map, tri_index, mirror_dp_du, mirror_dp_dv) &&
        symmetry_uv_jacobian(runtime->main_pass_dp_du,
                             runtime->main_pass_dp_dv,
                             mirror_dp_du,
                             mirror_dp_dv,
                             cache.mirror_symmetry_pass,
                             jacobian))
    {
      CloneDabTransform mirrored = runtime->main_pass_transform;
      if (clone_symmetry_transform_apply(mirrored, jacobian)) {
        transform.dest_uv_to_source_uv = mirrored.dest_uv_to_source_uv;
        transform.dest_uv_to_footprint_dir = mirrored.dest_uv_to_footprint_dir;
      }
    }
    if (clone_dab_is_symmetry_duplicate(*runtime, dest_uv, uv_radius)) {
      return;
    }
  }

  /* Writable, because Relative fixes its anchor on the first dab -- and only the main pass may
   * fix it, or a mirrored location would define the offset the whole stroke then works from. */
  CloneSourcePoint *source_mut = clone_source_point_get_for_write(&ob);
  if (source_mut == nullptr) {
    return;
  }
  if (!is_main_pass && sd.paint.clone_mode == CLONE_MODE_RELATIVE && !source_mut->anchor_valid) {
    return;
  }
  const float2 dab_center_uv = clone_dab_center_uv_get(
      *source_mut, dest_uv, cache.location, sd.paint.clone_mode);

  /* bstrength carries pressure/overlap/feather (PAINT formula); the Strength slider
   * (alpha) is folded in here since our direct write has no separate composite stage. */
  const float strength = std::clamp(cache.bstrength * BKE_brush_alpha_get(&sd.paint, &brush),
                                    0.0f,
                                    1.0f);
  clone_stroke_apply_dab(runtime,
                         dest_uv,
                         dab_center_uv,
                         transform,
                         uv_radius,
                         sd.paint.visible_material_channels,
                         strength);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
