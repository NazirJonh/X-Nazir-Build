/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_clone_source.hh"

#include <cfloat>
#include <climits>

#include "DNA_brush_types.h"
/* #CloneChannelTarget holds an #ImageUser by value, so the complete type is needed here. */
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BLI_kdopbvh.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_image.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_view2d.hh"

#include "paint_clone_2d.hh"

#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint::clone {

/* -------------------------------------------------------------------- */
/** \name Source points (runtime-only, not DNA).
 * \{ */

/** Source points of the session, keyed on #ID.session_uid. Cleared on file load. */
static Map<uint32_t, CloneSourcePoint> &clone_source_points()
{
  static Map<uint32_t, CloneSourcePoint> points;
  return points;
}

bool clone_source_point_is_valid(const CloneSourcePoint &source, const Object &ob)
{
  if (!source.is_valid) {
    return false;
  }
  if (source.object_session_uid != ob.id.session_uid) {
    return false;
  }
  if (source.mesh_data_ptr != id_cast<const Mesh *>(ob.data)) {
    return false;
  }
  if (source.material_slot != ob.actcol) {
    return false;
  }
  return true;
}

void clone_source_point_set(const Object &ob,
                            const float3 &co_object,
                            const float2 &uv,
                            const CloneSurfaceFrame &frame,
                            const StringRef uv_layer_name,
                            const int material_slot)
{
  CloneSourcePoint point;
  point.co_object = co_object;
  point.uv = uv;
  point.frame = frame;
  point.is_valid = true;
  point.object_session_uid = ob.id.session_uid;
  point.mesh_data_ptr = id_cast<const Mesh *>(ob.data);
  point.uv_layer_name = uv_layer_name;
  point.material_slot = material_slot;
  clone_source_points().add_overwrite(ob.id.session_uid, std::move(point));
}

CloneSourcePoint *clone_source_point_get_for_write(const Object *ob)
{
  if (ob == nullptr) {
    return nullptr;
  }
  CloneSourcePoint *point = clone_source_points().lookup_ptr(ob->id.session_uid);
  if (point == nullptr || !clone_source_point_is_valid(*point, *ob)) {
    return nullptr;
  }
  return point;
}

const CloneSourcePoint *clone_source_point_get(const Object *ob)
{
  return clone_source_point_get_for_write(const_cast<Object *>(ob));
}

void clone_source_point_reset(const Object *ob)
{
  if (ob != nullptr) {
    clone_source_points().remove(ob->id.session_uid);
  }
}

void clone_source_points_clear_all()
{
  clone_source_points().clear();
}

static void clone_source_points_on_file_load(Main * /*bmain*/,
                                             PointerRNA ** /*pointers*/,
                                             int /*pointers_num*/,
                                             void * /*arg*/)
{
  /* Every source names an object of the file being replaced. Session uids are not reused, so the
   * records could never match again -- they would only sit in the map for the rest of the run. */
  clone_source_points_clear_all();
}

void clone_source_points_callbacks_register()
{
  /* The store is a list node: adding it twice would splice the callback list into a cycle. Guard
   * here rather than at the call site, so the function stays safe for any caller. */
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;

  static bCallbackFuncStore load_pre_cb{};
  load_pre_cb.func = clone_source_points_on_file_load;
  load_pre_cb.alloc = false;
  BKE_callback_add(&load_pre_cb, BKE_CB_EVT_LOAD_PRE);
}

float2 clone_dab_center_uv_get(CloneSourcePoint &source,
                               const float2 &dest_uv,
                               const float3 &dest_co_object,
                               const int8_t clone_mode)
{
  if (clone_mode != CLONE_MODE_RELATIVE) {
    return dest_uv;
  }
  if (!source.anchor_valid) {
    source.anchor_uv = dest_uv;
    source.anchor_co_object = dest_co_object;
    source.anchor_valid = true;
  }
  return source.anchor_uv;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2D source point.
 * \{ */

std::optional<Clone2DSource> clone_2d_source_get(const ImagePaintSettings &settings)
{
  if ((settings.clone_source_flag & IMAGE_PAINT_CLONE_SOURCE_SET) == 0) {
    return std::nullopt;
  }
  Clone2DSource source;
  source.uv = float2(settings.clone_source_uv);
  source.anchor_uv = float2(settings.clone_source_anchor_uv);
  source.anchor_valid = (settings.clone_source_flag & IMAGE_PAINT_CLONE_SOURCE_ANCHOR_SET) != 0;
  return source;
}

void clone_2d_source_set(ImagePaintSettings &settings, const float2 &uv)
{
  copy_v2_v2(settings.clone_source_uv, uv);
  copy_v2_fl(settings.clone_source_anchor_uv, 0.0f);
  /* Setting a source drops the Relative offset: it is measured from the source, so the old one
   * would put the stamp at an offset the user never chose. */
  settings.clone_source_flag = IMAGE_PAINT_CLONE_SOURCE_SET;
}

float2 clone_2d_dab_center_uv_get(ImagePaintSettings &settings,
                                  const float2 &dest_uv,
                                  const int8_t clone_mode)
{
  if (clone_mode != CLONE_MODE_RELATIVE) {
    return dest_uv;
  }
  if ((settings.clone_source_flag & IMAGE_PAINT_CLONE_SOURCE_ANCHOR_SET) == 0) {
    copy_v2_v2(settings.clone_source_anchor_uv, dest_uv);
    settings.clone_source_flag |= IMAGE_PAINT_CLONE_SOURCE_ANCHOR_SET;
  }
  return float2(settings.clone_source_anchor_uv);
}

void clone_2d_source_reset(ImagePaintSettings &settings)
{
  copy_v2_fl(settings.clone_source_uv, 0.0f);
  copy_v2_fl(settings.clone_source_anchor_uv, 0.0f);
  settings.clone_source_flag = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source picking (3D Viewport, BVH raycast like color picker).
 * \{ */

float2 clone_pick_uv(const Mesh *mesh_eval,
                     const ePaintCanvasSource canvas_mode,
                     Object *ob_for_material,
                     const int tri_index,
                     const float3 &bary_coord)
{
  const Span<int3> tris = mesh_eval->corner_tris();
  const VArraySpan<float2> uv_map = clone_uv_map_get(
      *mesh_eval, canvas_mode, ob_for_material, tri_index);
  return bke::mesh_surface_sample::sample_corner_attribute_with_bary_coords(
      bary_coord, tris[tri_index], uv_map);
}

bool clone_pick_face(ViewContext *vc,
                     const int mval[2],
                     int *r_tri_index,
                     int *r_face_index,
                     float3 *r_bary_coord,
                     float r_co[3],
                     float r_no[3],
                     float3 *r_co_object,
                     const Mesh &mesh)
{
  if (mesh.faces_num == 0) {
    return false;
  }
  float3 start_world;
  float3 end_world;
  ED_view3d_win_to_segment_clipped(
      vc->depsgraph, vc->region, vc->v3d, float2(mval[0], mval[1]), start_world, end_world, true);

  const float4x4 &world_to_object = vc->obact->world_to_object();
  const float3 start_object = math::transform_point(world_to_object, start_world);
  const float3 end_object = math::transform_point(world_to_object, end_world);

  bke::BVHTreeFromMesh mesh_bvh = mesh.bvh_corner_tris();
  BVHTreeRayHit ray_hit;
  ray_hit.dist = FLT_MAX;
  ray_hit.index = -1;
  BLI_bvhtree_ray_cast(mesh_bvh.tree,
                       start_object,
                       math::normalize(end_object - start_object),
                       0.0f,
                       &ray_hit,
                       mesh_bvh.raycast_callback,
                       &mesh_bvh);
  if (ray_hit.index == -1) {
    return false;
  }
  *r_bary_coord = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
      mesh.vert_positions(),
      mesh.corner_verts(),
      mesh.corner_tris()[ray_hit.index],
      ray_hit.co);
  *r_tri_index = ray_hit.index;
  *r_face_index = mesh.corner_tri_faces()[ray_hit.index];
  *r_co_object = float3(ray_hit.co);
  const float3 co_world = math::transform_point(vc->obact->object_to_world(),
                                                float3(ray_hit.co));
  copy_v3_v3(r_co, co_world);
  const float3 no_world = math::transform_direction(vc->obact->object_to_world(),
                                                    float3(ray_hit.no));
  copy_v3_v3(r_no, math::normalize(no_world));
  return true;
}

/* Dyntopo is unsupported. A BMesh on the sculpt session means it is active. */
bool clone_dyntopo_active(const Object *ob)
{
  if (ob == nullptr || ob->runtime == nullptr) {
    return false;
  }
  const SculptSession *session = ob->runtime->sculpt_session;
  return session != nullptr && session->bm != nullptr;
}

bool clone_poll_basic(bContext *C)
{
  if (G.background) {
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  if (scene == nullptr || ob == nullptr || ob->data == nullptr) {
    return false;
  }
  if (ob->type != OB_MESH) {
    return false;
  }
  if (clone_dyntopo_active(ob)) {
    return false;
  }
  return true;
}

static wmOperatorStatus clone_source_set_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  /* Image Editor pick first: the canvas UV comes straight from the region's view2d and no mesh
   * is involved, so this branch must run before the 3D path requires an active object. */
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima != nullptr) {
    ARegion *region = CTX_wm_region(C);
    if (region == nullptr || scene == nullptr) {
      return OPERATOR_CANCELLED;
    }
    int mval[2];
    RNA_int_get_array(op->ptr, "location", mval);
    if (!clone_2d_source_set_from_region(scene->toolsettings->imapaint, *region, mval, op)) {
      return OPERATOR_CANCELLED;
    }
    WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
    BKE_report(op->reports, RPT_INFO, "Clone source set");
    return OPERATOR_FINISHED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Object *ob = CTX_data_active_object(C);
  if (scene == nullptr || ob == nullptr) {
    return OPERATOR_CANCELLED;
  }

  int mval[2];
  RNA_int_get_array(op->ptr, "location", mval);

  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  if (vc.v3d == nullptr || vc.region == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Clone source requires the 3D Viewport");
    return OPERATOR_CANCELLED;
  }

  Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (mesh_eval == nullptr || mesh_eval->uv_map_names().is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "Clone source requires a mesh with UVs");
    return OPERATOR_CANCELLED;
  }
  Material *ma = BKE_object_material_get(ob_eval, ob_eval->actcol);
  if (ma == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Clone source requires a material");
    return OPERATOR_CANCELLED;
  }

  int tri_index = -1;
  int face_index = -1;
  float3 bary_coord;
  float co_arr[3] = {0.0f, 0.0f, 0.0f};
  float no_arr[3] = {0.0f, 0.0f, 1.0f};
  float3 co_object(0.0f);
  const VArray<bool> hide_poly = *mesh_eval->attributes().lookup_or_default<bool>(
      ".hide_poly", bke::AttrDomain::Face, false);
  const bool hit = clone_pick_face(&vc,
                                   mval,
                                   &tri_index,
                                   &face_index,
                                   &bary_coord,
                                   co_arr,
                                   no_arr,
                                   &co_object,
                                   *mesh_eval) &&
                   !hide_poly[face_index];
  if (!hit) {
    BKE_report(op->reports, RPT_WARNING, "Clone source: no surface under the cursor");
    return OPERATOR_CANCELLED;
  }

  const float2 uv = clone_pick_uv(mesh_eval,
                                  ePaintCanvasSource(scene->toolsettings->imapaint.mode),
                                  ob_eval,
                                  tri_index,
                                  bary_coord);

  /* The frame of the picked triangle in the view as it stands NOW. Everything the stamp does with
   * orientation is measured against this snapshot, so it must be taken here and never refreshed
   * -- see #CloneSourcePoint.frame. */
  const ePaintCanvasSource canvas_mode = ePaintCanvasSource(scene->toolsettings->imapaint.mode);
  CloneSurfaceFrame source_frame;
  const bool has_view = vc.rv3d != nullptr;
  clone_surface_frame_build(*mesh_eval,
                            clone_uv_map_get(*mesh_eval, canvas_mode, ob_eval, tri_index),
                            tri_index,
                            has_view ? vc.region : nullptr,
                            has_view ? ED_view3d_ob_project_mat_get(vc.rv3d, ob_eval) :
                                       float4x4::identity(),
                            clone_view_right_get(vc, *ob),
                            source_frame);

  const StringRef uv_name = mesh_eval->active_uv_map_name();
  clone_source_point_set(*ob, co_object, uv, source_frame, uv_name, ob->actcol);

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  BKE_report(op->reports, RPT_INFO, "Clone source set");
  return OPERATOR_FINISHED;
}

static wmOperatorStatus clone_source_set_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  if (region != nullptr && event != nullptr) {
    RNA_int_set_array(op->ptr, "location", event->mval);
  }
  return clone_source_set_exec(C, op);
}

static bool clone_source_set_poll(bContext *C)
{
  if (CTX_wm_space_image(C) != nullptr) {
    if (!ED_image_tools_paint_poll(C)) {
      return false;
    }
    const Paint *paint = BKE_paint_get_active_from_context(C);
    const Brush *brush = (paint != nullptr) ? BKE_paint_brush_for_read(paint) : nullptr;
    if (brush == nullptr || brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_CLONE) {
      CTX_wm_operator_poll_msg_set(C, "Active brush is not a Clone brush");
      return false;
    }
    if (!clone_2d_tool_active(C)) {
      /* The legacy Clone tool shares the brush; only the Clone Stamp tool picks a source, or a
       * stray Shift+Click would silently switch the legacy brush over to stamping. */
      CTX_wm_operator_poll_msg_set(C, "Clone Stamp tool is not active");
      return false;
    }
    return true;
  }
  if (!clone_poll_basic(C)) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if (clone_dyntopo_active(ob)) {
    CTX_wm_operator_poll_msg_set(C, "Clone Stamp is not available in dynamic topology mode");
    return false;
  }
  return true;
}

void PAINT_OT_clone_source_set(wmOperatorType *ot)
{
  ot->name = "Set Clone Stamp Source";
  ot->idname = "PAINT_OT_clone_source_set";
  ot->description =
      "Set the point the Clone Stamp brush copies from (Shift+LMB in the 3D Viewport or the "
      "Image Editor)";

  ot->exec = clone_source_set_exec;
  ot->invoke = clone_source_set_invoke;
  ot->poll = clone_source_set_poll;

  /* One-shot picker, no modal loop (avoids long-running parallel modal).
   *
   * No OPTYPE_UNDO, matching #PAINT_OT_sample_color -- the other Shift+Click picker that writes
   * into data. Picking is setting a tool up, not editing the file, and an undo step per pick would
   * bury the strokes around it. The 3D source is session state, so nothing to undo there; the 2D
   * source lives in #ImagePaintSettings, so a global undo step taken before the pick does revert
   * it -- the same thing that happens to any other tool setting. */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_int_vector(
      ot->srna, "location", 2, nullptr, 0, INT_MAX, "Location", "", 0, 16384);
  RNA_def_property_flag(prop, (PROP_SKIP_SAVE | PROP_HIDDEN));
}

static wmOperatorStatus clone_source_reset_exec(bContext *C, wmOperator * /*op*/)
{
  if (CTX_wm_space_image(C) != nullptr) {
    Scene *scene = CTX_data_scene(C);
    if (scene == nullptr) {
      return OPERATOR_CANCELLED;
    }
    clone_2d_source_reset(scene->toolsettings->imapaint);
    WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
    return OPERATOR_FINISHED;
  }
  Object *ob = CTX_data_active_object(C);
  clone_source_point_reset(ob);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  return OPERATOR_FINISHED;
}

static bool clone_source_reset_poll(bContext *C)
{
  if (CTX_wm_space_image(C) != nullptr) {
    return ED_image_tools_paint_poll(C);
  }
  return clone_poll_basic(C);
}

void PAINT_OT_clone_source_reset(wmOperatorType *ot)
{
  ot->name = "Reset Clone Stamp Source";
  ot->idname = "PAINT_OT_clone_source_reset";
  ot->description = "Clear the Clone Stamp source point and its Relative offset";

  ot->exec = clone_source_reset_exec;
  ot->poll = clone_source_reset_poll;

  ot->flag = OPTYPE_REGISTER;
}

bool clone_2d_source_set_from_region(ImagePaintSettings &settings,
                                     ARegion &region,
                                     const int mval[2],
                                     wmOperator *op)
{
  float2 uv;
  ui::view2d_region_to_view(&region.v2d, float(mval[0]), float(mval[1]), &uv[0], &uv[1]);
  if (!(uv[0] >= 0.0f && uv[0] <= 1.0f && uv[1] >= 0.0f && uv[1] <= 1.0f)) {
    /* The core sampler wraps UVs, so a source outside the canvas would silently read texels from
     * the wrapped position; refuse the pick instead. */
    if (op != nullptr) {
      BKE_report(op->reports, RPT_INFO, "Clone Stamp source must be inside the canvas");
    }
    return false;
  }
  clone_2d_source_set(settings, uv);
  return true;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
