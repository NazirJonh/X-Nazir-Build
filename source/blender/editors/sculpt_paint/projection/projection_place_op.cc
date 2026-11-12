/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Modal operator that places the Planar projection frame on the painted surface by a viewport
 * click: raycasts against the evaluated mesh, sets center + orientation, and lets the user drag
 * size before committing.
 */

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_index_range.hh"
#include "BLI_utildefines.h"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_event_types.hh"

#include "BKE_bvhutils.hh"

#include "projection_gizmo_intern.hh"
#include "projection_target.hh"

namespace blender::ed::sculpt_paint::projection {

/* -------------------------------------------------------------------- */
/** \name Place Projection operator state
 * \{ */

struct PlaceProjectionState {
  PaintLayerRowProjectionTarget target;
  ProjectionFrame original;
  float hit_center[3] = {0.0f, 0.0f, 0.0f};
  float hit_normal[3] = {0.0f, 0.0f, 1.0f};
  float size[3] = {1.0f, 1.0f, 1.0f};
  float roll = 0.0f;
  float roll_at_r_press = 0.0f;
  int click_mval[2] = {0, 0};
  bool placed = false;
  bool dragging_size = false;
  bool dragging_roll = false;
  float pixel_size = 0.01f;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/** Transform a normal vector from local to world using transpose(inverse(M3)). */
static float3 normal_local_to_world(const float4x4 &o2w, const float3 &local_no)
{
  return math::normalize(math::transpose(math::invert(float3x3(o2w))) * local_no);
}

/** Transform a normal vector from world to a frame object's local space using transpose(M3(o2w)). */
static float3 normal_world_to_local(const float4x4 &o2w, const float3 &world_no)
{
  return math::normalize(math::transpose(float3x3(o2w)) * world_no);
}

static void place_free(wmOperator *op)
{
  auto *data = static_cast<PlaceProjectionState *>(op->customdata);
  if (data != nullptr) {
    MEM_delete(data);
  }
  op->customdata = nullptr;
}

static void place_cancel(bContext * /*C*/, wmOperator *op)
{
  place_free(op);
}

static void place_notify(Material *ma)
{
  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

static void place_update_status(bContext *C, const PlaceProjectionState &data)
{
  const float roll_deg = data.roll * (180.0f / float(M_PI));
  const std::string text = fmt::format(
      "LMB: place / drag: size | R: roll | Ctrl: step | Esc/RMB: cancel | "
      "Size: {:.3f} | Roll: {:.1f}{}",
      data.size[0],
      roll_deg,
      "\xc2\xb0");
  ED_workspace_status_text(C, text.c_str());
}

static void place_write(PlaceProjectionState &data)
{
  Material *ma = data.target.material();
  if (ma == nullptr) {
    return;
  }
  ProjectionFrame frame;
  copy_v3_v3(frame.center, data.hit_center);
  copy_v3_v3(frame.size, data.size);
  copy_v3_v3(frame.normal, data.hit_normal);
  frame.roll = data.roll;
  data.target.write(frame);
  place_notify(ma);
}

/** Find the 3D Viewport region under the cursor. Returns the View3D, RegionView3D, and
 * region-relative coordinates. */
static bool place_find_view3d(const bContext *C,
                              const wmEvent &event,
                              const ARegion *&r_region,
                              const View3D *&r_v3d,
                              RegionView3D *&r_rv3d,
                              float r_mval[2])
{
  wmWindow *win = CTX_wm_window(C);
  ScrArea *area = nullptr;
  const ARegion *region = ED_screen_area_region_under_cursor(
      win, SPACE_VIEW3D, RGN_TYPE_WINDOW, event.xy, &area);
  if (region == nullptr || area == nullptr) {
    return false;
  }
  r_v3d = static_cast<View3D *>(area->spacedata.first);
  r_rv3d = static_cast<RegionView3D *>(region->regiondata);
  r_region = region;
  r_mval[0] = float(event.xy[0] - region->winrct.xmin);
  r_mval[1] = float(event.xy[1] - region->winrct.ymin);
  return true;
}

/** Raycast the active object's evaluated mesh. Returns true with world-space position and normal
 * on hit. */
static bool place_raycast(const bContext *C,
                          const ARegion *region,
                          const View3D *v3d,
                          const float mval[2],
                          Object *ob,
                          float r_world_loc[3],
                          float r_world_no[3])
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  float ray_start[3], ray_dir[3];
  if (!ED_view3d_win_to_ray_clipped(depsgraph, region, v3d, mval, ray_start, ray_dir, true)) {
    return false;
  }
  Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
  if (ob_eval == nullptr || ob_eval->type != OB_MESH) {
    return false;
  }
  Mesh *me_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (me_eval == nullptr) {
    return false;
  }
  /* Transform ray to object space. */
  const float4x4 &o2w = ob_eval->object_to_world();
  const float4x4 w2o = math::invert(o2w);
  float3 local_start = math::transform_point(w2o, float3(ray_start));
  float3 local_dir = math::normalize(math::transform_direction(w2o, float3(ray_dir)));

  bke::BVHTreeFromMesh tree_data = me_eval->bvh_corner_tris_no_hidden();
  if (tree_data.tree == nullptr) {
    return false;
  }
  BVHTreeRayHit hit{};
  hit.index = -1;
  hit.dist = BVH_RAYCAST_DIST_MAX;
  const float start_f[3] = {local_start.x, local_start.y, local_start.z};
  const float dir_f[3] = {local_dir.x, local_dir.y, local_dir.z};
  BLI_bvhtree_ray_cast(
      tree_data.tree, start_f, dir_f, 0.0f, &hit, tree_data.raycast_callback, &tree_data);
  if (hit.index < 0) {
    return false;
  }
  /* hit.co and hit.no are in object space; transform to world. */
  const float3 world_loc = math::transform_point(o2w, float3(hit.co));
  const float3 world_no = normal_local_to_world(o2w, float3(hit.no));
  copy_v3_v3(r_world_loc, world_loc);
  copy_v3_v3(r_world_no, world_no);
  return true;
}

/** Convert a world point and normal into the frame's space (controller or object). */
static void place_world_to_frame(const bContext *C,
                                 Object *space_ob,
                                 const float world_loc[3],
                                 const float world_no[3],
                                 float r_center[3],
                                 float r_normal[3])
{
  if (space_ob != nullptr) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    Object *sp_eval = DEG_get_evaluated(depsgraph, space_ob);
    if (sp_eval != nullptr) {
      const float4x4 w2o = sp_eval->world_to_object();
      const float3 lp = math::transform_point(w2o, float3(world_loc));
      copy_v3_v3(r_center, lp);
      /* Normal: world→frame uses transpose(M3(o2w)). */
      const float3 ln = normal_world_to_local(sp_eval->object_to_world(), float3(world_no));
      copy_v3_v3(r_normal, ln);
      return;
    }
  }
  copy_v3_v3(r_center, world_loc);
  copy_v3_v3(r_normal, world_no);
}

/** Fallback: intersect ray with the view plane through the 3D cursor. */
static bool place_plane_fallback(const bContext *C,
                                 const ARegion *region,
                                 const View3D *v3d,
                                 const RegionView3D *rv3d,
                                 const float mval[2],
                                 float r_center[3],
                                 float r_normal[3])
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  float ray_start[3], ray_dir[3];
  if (!ED_view3d_win_to_ray_clipped(depsgraph, region, v3d, mval, ray_start, ray_dir, false)) {
    return false;
  }
  Scene *scene = CTX_data_scene(C);
  const float *cursor_loc = scene->cursor.location;
  /* The camera's +Z axis points toward the observer, matching the frame +Z convention. */
  const float view_n[3] = {rv3d->viewinv[2][0], rv3d->viewinv[2][1], rv3d->viewinv[2][2]};
  /* Plane through cursor, normal = view direction. */
  float plane[4];
  plane[0] = view_n[0];
  plane[1] = view_n[1];
  plane[2] = view_n[2];
  plane[3] = -dot_v3v3(view_n, cursor_loc);
  float lambda;
  if (!isect_ray_plane_v3(ray_start, ray_dir, plane, &lambda, true)) {
    return false;
  }
  copy_v3_v3(r_center, ray_start);
  madd_v3_v3fl(r_center, ray_dir, lambda);
  copy_v3_v3(r_normal, view_n);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator callbacks
 * \{ */

static bool place_poll(bContext *C)
{
  PaintLayerRowProjectionTarget target;
  return target.resolve(*C);
}

static wmOperatorStatus place_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  auto *data = MEM_new<PlaceProjectionState>(__func__);
  if (!data->target.resolve(*C)) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }
  if (!data->target.read(data->original)) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }
  copy_v3_v3(data->hit_center, data->original.center);
  copy_v3_v3(data->size, data->original.size);
  copy_v3_v3(data->hit_normal, data->original.normal);
  data->roll = data->original.roll;
  data->pixel_size = 0.01f;

  /* Default size from object bbox. */
  Object *ob = data->target.object();
  if (ob != nullptr) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
    if (ob_eval != nullptr) {
      const auto bounds = BKE_object_boundbox_get(ob_eval);
      if (bounds.has_value()) {
        const float3 extent = bounds->max - bounds->min;
        const float max_ext = std::max({extent.x, extent.y, extent.z});
        const float default_sz = max_ext * 0.25f;
        copy_v3_fl(data->size, default_sz);
      }
    }
  }

  op->customdata = data;
  WM_event_add_modal_handler(C, op);
  place_update_status(C, *data);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus place_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto *data = static_cast<PlaceProjectionState *>(op->customdata);

  /* Cancel. */
  if ((event->type == EVT_ESCKEY || (event->type == RIGHTMOUSE && event->val == KM_PRESS))) {
    data->target.write(data->original);
    Material *ma = data->target.material();
    if (ma != nullptr) {
      place_notify(ma);
    }
    ED_workspace_status_text(C, nullptr);
    place_free(op);
    return OPERATOR_CANCELLED;
  }

  /* LMB press: place. */
  if (event->type == LEFTMOUSE && event->val == KM_PRESS && !data->dragging_roll) {
    const ARegion *region = nullptr;
    const View3D *v3d = nullptr;
    RegionView3D *rv3d = nullptr;
    float mval[2];
    if (!place_find_view3d(C, *event, region, v3d, rv3d, mval)) {
      return OPERATOR_RUNNING_MODAL;
    }
    Object *ob = data->target.object();
    Object *space_ob = data->target.space_object();
    float world_loc[3], world_no[3];
    bool got_hit = false;
    if (ob != nullptr && place_raycast(C, region, v3d, mval, ob, world_loc, world_no)) {
      got_hit = true;
    }
    else if (place_plane_fallback(C, region, v3d, rv3d, mval, world_loc, world_no)) {
      got_hit = true;
    }
    if (got_hit) {
      place_world_to_frame(C, space_ob, world_loc, world_no, data->hit_center, world_no);
      BKE_paint_layers_projection_orientation_from_surface(
          world_no, data->hit_normal, &data->roll);
      data->roll_at_r_press = data->roll;
    }
    data->placed = true;
    data->dragging_size = true;
    copy_v2_v2_int(data->click_mval, event->xy);
    /* Compute pixel size at the hit point for drag scaling. */
    {
      Object *px_ob = (space_ob != nullptr) ? space_ob : ob;
      const float4x4 &o2w = px_ob->object_to_world();
      const float3 world_center = math::transform_point(o2w, float3(data->hit_center));
      data->pixel_size = ED_view3d_pixel_size(rv3d, world_center);
    }
    place_write(*data);
    place_update_status(C, *data);
    return OPERATOR_RUNNING_MODAL;
  }

  /* LMB release: finish. */
  if (event->type == LEFTMOUSE && event->val == KM_RELEASE && data->dragging_size) {
    data->dragging_size = false;
    ED_workspace_status_text(C, nullptr);
    place_free(op);
    return OPERATOR_FINISHED;
  }

  /* R key: enter/exit roll mode. */
  if (event->type == EVT_RKEY && event->val == KM_PRESS) {
    data->dragging_roll = true;
    data->dragging_size = false;
    data->roll_at_r_press = data->roll;
    copy_v2_v2_int(data->click_mval, event->xy);
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == EVT_RKEY && event->val == KM_RELEASE) {
    data->dragging_roll = false;
    if (data->placed) {
      ED_workspace_status_text(C, nullptr);
      place_free(op);
      return OPERATOR_FINISHED;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  /* Mouse move. */
  if (event->type == MOUSEMOVE) {
    if (data->dragging_size) {
      const int dx = event->xy[0] - data->click_mval[0];
      const int dy = event->xy[1] - data->click_mval[1];
      const float pixel_dist = std::sqrt(float(dx * dx + dy * dy));
      const float world_radius = pixel_dist * data->pixel_size;
      const float sz = std::max(2.0f * world_radius, PAINT_LAYER_PROJECTION_SIZE_MIN);
      data->size[0] = sz;
      data->size[1] = sz;
      /* Z (depth) stays at default. */
      place_write(*data);
      place_update_status(C, *data);
    }
    if (data->dragging_roll) {
      const int dx = event->xy[0] - data->click_mval[0];
      float new_roll = data->roll_at_r_press + float(dx) * 0.01f;
      if (event->modifier & KM_CTRL) {
        constexpr float step = float(M_PI) / 12.0f; /* 15 degrees. */
        new_roll = std::round(new_roll / step) * step;
      }
      data->roll = new_roll;
      place_write(*data);
      place_update_status(C, *data);
    }
    return OPERATOR_RUNNING_MODAL;
  }

  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_layer_projection_place(wmOperatorType *ot)
{
  ot->name = "Place Projection on Surface";
  ot->idname = "PAINT_OT_layer_projection_place";
  ot->description = "Set the projection frame by clicking on the painted surface";
  ot->invoke = place_invoke;
  ot->modal = place_modal;
  ot->poll = place_poll;
  ot->cancel = place_cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/* -------------------------------------------------------------------- */
/** \name Flip Projection operator
 * \{ */

static wmOperatorStatus paint_layer_projection_flip_exec(bContext *C, wmOperator * /*op*/)
{
  PaintLayerRowProjectionTarget target;
  if (!target.resolve(*C)) {
    return OPERATOR_CANCELLED;
  }
  ProjectionFrame frame;
  if (!target.read(frame)) {
    return OPERATOR_CANCELLED;
  }
  /* Flip: negate the normal and mirror the roll. The reference axes for -normal are (-x_ref, y_ref),
   * so rolled_x' = -x and y' = y is achieved at roll' = -roll; roll + pi would flip the image
   * upside-down. Compute through the matrix for correctness: columns (-x, y, -n). */
  float rot[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot);
  /* Negate columns 0 (x) and 2 (z), keep column 1 (y). */
  for (const int i : IndexRange(3)) {
    rot[i][0] = -rot[i][0];
    rot[i][2] = -rot[i][2];
  }
  float new_normal[3];
  float new_roll;
  BKE_paint_layers_projection_frame_from_matrix(rot, new_normal, &new_roll);
  copy_v3_v3(frame.normal, new_normal);
  frame.roll = new_roll;
  target.write(frame);
  Material *ma = target.material();
  if (ma != nullptr) {
    DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_projection_flip(wmOperatorType *ot)
{
  ot->name = "Flip Projection Direction";
  ot->idname = "PAINT_OT_layer_projection_flip";
  ot->description = "Reverse the projection normal, adjusting roll to keep the image orientation";
  ot->exec = paint_layer_projection_flip_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move Projection operator
 * \{ */

struct MoveProjectionState {
  PaintLayerRowProjectionTarget target;
  ProjectionFrame original;
  ProjectionFrame frame;
};

static void move_free(wmOperator *op)
{
  auto *data = static_cast<MoveProjectionState *>(op->customdata);
  if (data != nullptr) {
    MEM_delete(data);
  }
  op->customdata = nullptr;
}

static void move_cancel(bContext * /*C*/, wmOperator *op)
{
  move_free(op);
}

static wmOperatorStatus move_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  auto *data = MEM_new<MoveProjectionState>(__func__);
  if (!data->target.resolve(*C) || !data->target.read(data->original)) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }
  data->frame = data->original;
  op->customdata = data;
  WM_event_add_modal_handler(C, op);
  ED_workspace_status_text(
      C, "Move along the surface | Alt: keep orientation | Shift: in the frame plane | Esc/RMB: cancel");
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus move_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto *data = static_cast<MoveProjectionState *>(op->customdata);

  if (event->type == EVT_ESCKEY || (event->type == RIGHTMOUSE && event->val == KM_PRESS)) {
    data->target.write(data->original);
    if (Material *ma = data->target.material()) {
      place_notify(ma);
    }
    ED_workspace_status_text(C, nullptr);
    move_free(op);
    return OPERATOR_CANCELLED;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    ED_workspace_status_text(C, nullptr);
    move_free(op);
    return OPERATOR_FINISHED;
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  const ARegion *region = nullptr;
  const View3D *v3d = nullptr;
  RegionView3D *rv3d = nullptr;
  float mval[2];
  if (!place_find_view3d(C, *event, region, v3d, rv3d, mval)) {
    return OPERATOR_RUNNING_MODAL;
  }
  Object *ob = data->target.object();
  Object *space_ob = data->target.space_object();
  if (ob == nullptr || space_ob == nullptr) {
    return OPERATOR_RUNNING_MODAL;
  }

  float world_loc[3], world_no[3];
  bool hit = false;
  if ((event->modifier & KM_SHIFT) != 0) {
    /* Free move inside the plane of the frame, through its current center. */
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    const Object *space_eval = DEG_get_evaluated(depsgraph, space_ob);
    float ray_start[3], ray_dir[3];
    if (space_eval != nullptr &&
        ED_view3d_win_to_ray_clipped(depsgraph, region, v3d, mval, ray_start, ray_dir, false))
    {
      const float4x4 &o2w = space_eval->object_to_world();
      const float3 plane_co = math::transform_point(o2w, float3(data->frame.center));
      const float3 plane_no = normal_local_to_world(o2w, float3(data->frame.normal));
      float plane[4];
      plane_from_point_normal_v3(plane, plane_co, plane_no);
      float lambda;
      if (isect_ray_plane_v3(ray_start, ray_dir, plane, &lambda, false) && std::isfinite(lambda)) {
        madd_v3_v3v3fl(world_loc, ray_start, ray_dir, lambda);
        copy_v3_v3(world_no, plane_no);
        hit = true;
      }
    }
  }
  else {
    hit = place_raycast(C, region, v3d, mval, ob, world_loc, world_no);
  }
  if (!hit) {
    /* A miss keeps the last good placement instead of jumping. */
    return OPERATOR_RUNNING_MODAL;
  }

  float center[3];
  float frame_no[3];
  place_world_to_frame(C, space_ob, world_loc, world_no, center, frame_no);
  copy_v3_v3(data->frame.center, center);
  /* Like a Shrinkwrap controller, the frame turns to the face it lands on, so it can cross an edge
   * onto another face. Alt keeps the orientation, and Shift moves in the frame's own plane. */
  const bool keep_orientation = (event->modifier & (KM_ALT | KM_SHIFT)) != 0;
  if (!keep_orientation && normalize_v3(frame_no) > 0.0f) {
    /* Carry the frame over by the shortest turn between the two normals, so the roll the user set
     * survives the move instead of snapping back to the reference up. */
    float old_rot[3][3], turn[3][3], new_rot[3][3];
    BKE_paint_layers_projection_frame_matrix(
        data->frame.normal, data->frame.roll, old_rot);
    float old_no[3];
    copy_v3_v3(old_no, old_rot[2]);
    rotation_between_vecs_to_mat3(turn, old_no, frame_no);
    mul_m3_m3m3(new_rot, turn, old_rot);
    BKE_paint_layers_projection_frame_from_matrix(
        new_rot, data->frame.normal, &data->frame.roll);
  }
  data->target.write(data->frame);
  if (Material *ma = data->target.material()) {
    place_notify(ma);
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_layer_projection_move(wmOperatorType *ot)
{
  ot->name = "Move Projection on Surface";
  ot->idname = "PAINT_OT_layer_projection_move";
  ot->description = "Drag the projection frame along the painted surface";
  ot->invoke = move_invoke;
  ot->modal = move_modal;
  ot->poll = place_poll;
  ot->cancel = move_cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projection Frame tool
 *
 * The tool that shows the volume mode's standard Move / Rotate / Scale handles. Like the Color
 * Gradient tool follows Gradient rows, it follows the shown row: a row whose projection fills the
 * frame's extents switches to it and remembers the tool the user held, and any other shown row
 * hands that tool back while the Projection Frame tool is still the one held. See
 * #ED_paint_projection_frame_tool_activate_if_needed (projection_ops.cc) for the call sites.
 * \{ */

static bool projection_frame_tool_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || (ob->mode & OB_MODE_SCULPT) == 0) {
    return false;
  }
  PaintLayerRowProjectionTarget target;
  if (!target.resolve(*C)) {
    return false;
  }
  const MaterialPaintLayer *layer = BKE_paint_layers_find(*target.material(), target.marker());
  return layer != nullptr && projection_gizmo_mode_get(*layer) == PROJECTION_GIZMO_VOLUME;
}

static wmOperatorStatus projection_frame_tool_exec(bContext *C, wmOperator * /*op*/)
{
  PaintLayerRowProjectionTarget target;
  if (!target.resolve(*C)) {
    return OPERATOR_CANCELLED;
  }
  Material *ma = target.material();
  const MaterialPaintLayer *layer = BKE_paint_layers_find(*ma, target.marker());
  if (layer == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* The button is an explicit request, so a call that already found the tool active still counts
   * as one: the volume branch above runs either way. */
  if (!ED_paint_projection_frame_tool_activate_if_needed(C, *ma, layer)) {
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_projection_frame_tool(wmOperatorType *ot)
{
  ot->name = "Edit Projection Frame";
  ot->idname = "PAINT_OT_projection_frame_tool";
  ot->description = "Switch to the Projection Frame tool to edit the shown row's projection frame "
                    "with the standard handles";
  ot->exec = projection_frame_tool_exec;
  ot->poll = projection_frame_tool_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/** \} */

}  // namespace blender::ed::sculpt_paint::projection
