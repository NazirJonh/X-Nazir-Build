/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Sculpt Cursor - independent 3D cursor for sculpt mode workflow
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector.h"

#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::cursor {

/* Calculate rotation for sculpt cursor based on surface normal */
static void calc_sculpt_cursor_rotation(const float3 &normal,
                                        eV3DCursorOrient orientation,
                                        float r_quat[4])
{
  float mat[3][3];

  switch (orientation) {
    case V3D_CURSOR_ORIENT_VIEW:
      /* Use view direction - for now just use identity */
      unit_qt(r_quat);
      break;

    case V3D_CURSOR_ORIENT_GEOM:
      /* Build rotation from normal */
      {
        float3 up = {0.0f, 0.0f, 1.0f};

        /* If normal is close to up, use different vector */
        if (fabsf(math::dot(normal, up)) > 0.999f) {
          up = float3{1.0f, 0.0f, 0.0f};
        }

        /* Create coordinate system from normal */
        float3 tangent = math::cross(up, normal);
        tangent = math::normalize(tangent);
        float3 bitangent = math::cross(normal, tangent);

        /* Fill matrix */
        copy_v3_v3(mat[0], tangent);
        copy_v3_v3(mat[1], bitangent);
        copy_v3_v3(mat[2], normal);

        mat3_to_quat(r_quat, mat);
      }
      break;

    case V3D_CURSOR_ORIENT_NONE:
    default:
      unit_qt(r_quat);
      break;
  }
}

/* Invoke: Set cursor position on mouse click */
static wmOperatorStatus sculpt_cursor_set_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  BKE_view_layer_synced_ensure(*CTX_data_main(C), scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  if (!ob || !ob->runtime->sculpt_session) {
    return OPERATOR_CANCELLED;
  }

  SculptSession *ss = ob->runtime->sculpt_session;

  /* Get parameters from RNA */
  bool use_depth = RNA_boolean_get(op->ptr, "use_depth");
  eV3DCursorOrient orientation = eV3DCursorOrient(RNA_enum_get(op->ptr, "orientation"));

  float3 location;
  float3 normal;
  bool hit = false;

  float mval[2];
  mval[0] = float(event->mval[0]);
  mval[1] = float(event->mval[1]);

  /* Try to get position on mesh surface */
  if (use_depth) {
    hit = stroke_get_location_bvh(C, location, mval, false);

    if (hit) {
      /* Get normal at intersection point */
      std::optional<CursorGeometryInfo> cursor_info = cursor_geometry_info_update(
          C, float2(mval), true);

      if (cursor_info.has_value()) {
        normal = cursor_info->normal;
      }
      else {
        /* Fallback normal */
        normal = float3{0.0f, 0.0f, 1.0f};
      }
    }
  }

  if (!hit) {
    /* Fallback: use viewport depth or view plane */
    ARegion *region = CTX_wm_region(C);
    RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

    /* Project cursor onto view plane at object location */
    const View3D *v3d = CTX_wm_view3d(C);
    const float3 depth_pt = ob->object_to_world().location();
    float3 world_loc;
    ED_view3d_win_to_3d(v3d, region, depth_pt, mval, world_loc);
    location = math::transform_point(ob->world_to_object(), world_loc);

    /* Normal = view direction in object space */
    float3 world_normal = float3{rv3d->viewinv[2]};
    negate_v3(world_normal);
    normal = math::normalize(math::transform_direction(ob->world_to_object(), world_normal));
  }

  /* Save position */
  copy_v3_v3(ss->sculpt_cursor_pos, location);

  /* Calculate and save rotation */
  calc_sculpt_cursor_rotation(normal, orientation, ss->sculpt_cursor_rot);

  /* Mark cursor as initialized */
  ss->sculpt_cursor_initialized = true;
  copy_v3_fl3(ss->sculpt_cursor_scale, 1.0f, 1.0f, 1.0f);

  BKE_sculpt_cursor_session_to_storage(*ob, *ss);

  /* Notifier for redraw */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);

  return OPERATOR_FINISHED;
}

/* Poll: Available only in Sculpt Mode */
static bool sculpt_cursor_set_poll(bContext *C)
{
  return sculpt_mode_poll_view3d(C);
}

/* Operator definition */
void SCULPT_OT_cursor_set(wmOperatorType *ot)
{
  /* Identifiers */
  ot->name = "Set Sculpt Cursor";
  ot->idname = "SCULPT_OT_cursor_set";
  ot->description = "Set the sculpt cursor location and orientation";

  /* API callbacks */
  ot->invoke = sculpt_cursor_set_invoke;
  ot->poll = sculpt_cursor_set_poll;

  /* Flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Properties */
  static const EnumPropertyItem orientation_items[] = {
      {V3D_CURSOR_ORIENT_NONE, "NONE", 0, "None", "Leave orientation unchanged"},
      {V3D_CURSOR_ORIENT_VIEW, "VIEW", 0, "View", "Orient to the viewport"},
      {V3D_CURSOR_ORIENT_XFORM,
       "XFORM",
       0,
       "Transform",
       "Orient to the current transform setting"},
      {V3D_CURSOR_ORIENT_GEOM, "GEOM", 0, "Geometry", "Match the surface normal"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_boolean(ot->srna,
                  "use_depth",
                  true,
                  "Surface Project",
                  "Project cursor onto the surface");

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "orientation",
                                   orientation_items,
                                   V3D_CURSOR_ORIENT_VIEW,
                                   "Orientation",
                                   "The orientation for the cursor rotation");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* ================================================================
 * Operator for Transform sculpt cursor (move/rotate/scale)
 * ================================================================ */

enum eSculptCursorTransformMode {
  SCULPT_CURSOR_TRANSFORM_TRANSLATE = 0,
  SCULPT_CURSOR_TRANSFORM_ROTATE = 1,
  SCULPT_CURSOR_TRANSFORM_SCALE = 2,
};

struct SculptCursorTransformData {
  float3 initial_cursor_pos;
  float4 initial_cursor_rot;
  float3 initial_cursor_scale;

  float initial_world_pos[3];
  float initial_world_mat[4][4];

  float2 initial_mouse;
  bool constraint_axis[3];
};

static void sculpt_cursor_world_matrix_from_state(const Object &ob,
                                                  const float cursor_pos[3],
                                                  const float cursor_rot[4],
                                                  float r_mat[4][4])
{
  float local_rot[3][3];
  quat_to_mat3(local_rot, cursor_rot);

  float ob_rot[3][3];
  copy_m3_m4(ob_rot, ob.object_to_world().ptr());
  normalize_m3(ob_rot);

  float world_rot[3][3];
  mul_m3_m3m3(world_rot, ob_rot, local_rot);
  normalize_m3(world_rot);

  copy_m4_m3(r_mat, world_rot);
  copy_v3_v3(r_mat[3], cursor_pos);
  mul_m4_v3(ob.object_to_world().ptr(), r_mat[3]);
}

static int sculpt_cursor_constraint_axis_index(const bool constraint_axis[3])
{
  int count = 0;
  int axis = -1;
  for (int i = 0; i < 3; i++) {
    if (constraint_axis[i]) {
      axis = i;
      count++;
    }
  }
  return count == 1 ? axis : -1;
}

/**
 * Axis-constrained translation using the same ray/axis intersection as arrow gizmos.
 */
static bool sculpt_cursor_translate_along_axis(const bContext *C,
                                               const float initial_world_pos[3],
                                               const float initial_world_mat[4][4],
                                               const int axis,
                                               const float2 &init_mval,
                                               const float2 &curr_mval,
                                               float r_new_world_pos[3])
{
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  float arrow_co[3], arrow_no[3];
  copy_v3_v3(arrow_co, initial_world_pos);
  normalize_v3_v3(arrow_no, initial_world_mat[axis]);

  struct {
    float2 mval;
    float ray_origin[3], ray_direction[3];
    float location[3];
  } proj[2] = {};

  proj[0].mval = init_mval;
  proj[1].mval = curr_mval;

  int ok = 0;
  for (int j = 0; j < 2; j++) {
    const float mval[2] = {proj[j].mval.x, proj[j].mval.y};
    ED_view3d_win_to_ray(region, mval, proj[j].ray_origin, proj[j].ray_direction);
    if (j == 0) {
      if (RAD2DEGF(acosf(dot_v3v3(proj[j].ray_direction, arrow_no))) < 5.0f) {
        normalize_v3_v3(arrow_no, rv3d->viewinv[1]);
      }
    }

    float arrow_no_proj[3];
    project_plane_v3_v3v3(arrow_no_proj, arrow_no, proj[j].ray_direction);
    if (is_zero_v3(arrow_no_proj)) {
      continue;
    }
    normalize_v3(arrow_no_proj);

    float lambda;
    if (isect_ray_plane_v3_factor(
            arrow_co, arrow_no, proj[j].ray_origin, arrow_no_proj, &lambda))
    {
      madd_v3_v3v3fl(proj[j].location, arrow_co, arrow_no, lambda);
      ok++;
    }
  }

  if (ok != 2) {
    return false;
  }

  float offset[3];
  sub_v3_v3v3(offset, proj[1].location, proj[0].location);
  const float facdir = dot_v3v3(arrow_no, offset) < 0.0f ? -1.0f : 1.0f;
  madd_v3_v3v3fl(r_new_world_pos, initial_world_pos, arrow_no, facdir * len_v3(offset));
  return true;
}

static bool sculpt_cursor_rotate_around_axis(const bContext *C,
                                             const SculptCursorTransformData *data,
                                             const int axis,
                                             const float2 &curr_mval,
                                             float r_new_cursor_rot[4])
{
  ARegion *region = CTX_wm_region(C);

  float axis_vec[3];
  copy_v3_v3(axis_vec, data->initial_world_mat[axis]);
  normalize_v3(axis_vec);

  float dial_plane[4];
  plane_from_point_normal_v3(dial_plane, data->initial_world_pos, axis_vec);

  float proj_init[3], proj_curr[3];
  const float init_mval[2] = {data->initial_mouse.x, data->initial_mouse.y};
  const float curr_mval_fl[2] = {curr_mval.x, curr_mval.y};
  if (!ED_view3d_win_to_3d_on_plane(region, dial_plane, init_mval, false, proj_init)) {
    return false;
  }
  if (!ED_view3d_win_to_3d_on_plane(region, dial_plane, curr_mval_fl, false, proj_curr)) {
    return false;
  }

  sub_v3_v3(proj_init, data->initial_world_pos);
  sub_v3_v3(proj_curr, data->initial_world_pos);

  if (is_zero_v3(proj_init) || is_zero_v3(proj_curr)) {
    return false;
  }

  const float angle = angle_signed_on_axis_v3v3_v3(proj_init, proj_curr, axis_vec);

  float cursor_mat[3][3];
  quat_to_mat3(cursor_mat, data->initial_cursor_rot);
  float axis_local[3];
  copy_v3_v3(axis_local, cursor_mat[axis]);

  float delta_quat[4];
  axis_angle_to_quat(delta_quat, axis_local, angle);
  mul_qt_qtqt(r_new_cursor_rot, delta_quat, data->initial_cursor_rot);
  return true;
}

static int sculpt_cursor_constraint_axis_count(const bool constraint_axis[3])
{
  int count = 0;
  for (int i = 0; i < 3; i++) {
    if (constraint_axis[i]) {
      count++;
    }
  }
  return count;
}

/**
 * Scale the sculpt cursor using the same ray/axis intersection as translate gizmos.
 */
static bool sculpt_cursor_scale_from_drag(const bContext *C,
                                        const SculptCursorTransformData *data,
                                        const float2 &curr_mval,
                                        float r_new_scale[3])
{
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  copy_v3_v3(r_new_scale, data->initial_cursor_scale);

  const int constraint_count = sculpt_cursor_constraint_axis_count(data->constraint_axis);
  const float pixel_size = ED_view3d_pixel_size(rv3d, data->initial_world_pos);
  const float sensitivity = 1.0f / max_ff(pixel_size * 100.0f, 0.0001f);

  if (constraint_count == 0) {
    /* Uniform scale from view-plane distance to cursor center. */
    View3D *v3d = CTX_wm_view3d(C);
    if (!v3d) {
      return false;
    }
    float init_co[3], curr_co[3];
    const float init_mval[2] = {data->initial_mouse.x, data->initial_mouse.y};
    const float curr_mval_fl[2] = {curr_mval.x, curr_mval.y};
    ED_view3d_win_to_3d(v3d, region, data->initial_world_pos, init_mval, init_co);
    ED_view3d_win_to_3d(v3d, region, data->initial_world_pos, curr_mval_fl, curr_co);
    const float init_dist = len_v3v3(init_co, data->initial_world_pos);
    const float curr_dist = len_v3v3(curr_co, data->initial_world_pos);
    if (init_dist < 1e-6f) {
      return false;
    }
    const float factor = curr_dist / init_dist;
    for (int i = 0; i < 3; i++) {
      r_new_scale[i] = max_ff(0.001f, data->initial_cursor_scale[i] * factor);
    }
    return true;
  }

  if (constraint_count == 1) {
    const int axis = sculpt_cursor_constraint_axis_index(data->constraint_axis);
    if (axis == -1) {
      return false;
    }
    float new_world_pos[3];
    if (!sculpt_cursor_translate_along_axis(C,
                                            data->initial_world_pos,
                                            data->initial_world_mat,
                                            axis,
                                            data->initial_mouse,
                                            curr_mval,
                                            new_world_pos))
    {
      return false;
    }
    float delta[3];
    sub_v3_v3v3(delta, new_world_pos, data->initial_world_pos);
    float axis_no[3];
    normalize_v3_v3(axis_no, data->initial_world_mat[axis]);
    const float along = dot_v3v3(axis_no, delta);
    const float factor = 1.0f + along * sensitivity;
    r_new_scale[axis] = max_ff(0.001f, data->initial_cursor_scale[axis] * factor);
    return true;
  }

  /* Two-axis plane scale: apply the same factor to both constrained axes. */
  int axis = -1;
  for (int i = 0; i < 3; i++) {
    if (data->constraint_axis[i]) {
      axis = i;
      break;
    }
  }
  if (axis == -1) {
    return false;
  }
  float new_world_pos[3];
  if (!sculpt_cursor_translate_along_axis(C,
                                          data->initial_world_pos,
                                          data->initial_world_mat,
                                          axis,
                                          data->initial_mouse,
                                          curr_mval,
                                          new_world_pos))
  {
    return false;
  }
  float delta[3];
  sub_v3_v3v3(delta, new_world_pos, data->initial_world_pos);
  float axis_no[3];
  normalize_v3_v3(axis_no, data->initial_world_mat[axis]);
  const float along = dot_v3v3(axis_no, delta);
  const float factor = 1.0f + along * sensitivity;
  for (int i = 0; i < 3; i++) {
    if (data->constraint_axis[i]) {
      r_new_scale[i] = max_ff(0.001f, data->initial_cursor_scale[i] * factor);
    }
  }
  return true;
}

static wmOperatorStatus sculpt_cursor_transform_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || !ob->runtime->sculpt_session) {
    return OPERATOR_CANCELLED;
  }

  SculptSession *ss = ob->runtime->sculpt_session;

  if (!ss->sculpt_cursor_initialized) {
    return OPERATOR_CANCELLED;
  }

  /* Allocate custom data */
  SculptCursorTransformData *data = MEM_new<SculptCursorTransformData>(__func__);
  op->customdata = data;

  /* Store initial state */
  copy_v3_v3(data->initial_cursor_pos, ss->sculpt_cursor_pos);
  copy_qt_qt(data->initial_cursor_rot, ss->sculpt_cursor_rot);
  copy_v3_v3(data->initial_cursor_scale, ss->sculpt_cursor_scale);
  sculpt_cursor_world_matrix_from_state(
      *ob, data->initial_cursor_pos, data->initial_cursor_rot, data->initial_world_mat);
  copy_v3_v3(data->initial_world_pos, data->initial_world_mat[3]);
  data->initial_mouse = float2(float(event->mval[0]), float(event->mval[1]));
  RNA_boolean_get_array(op->ptr, "constraint_axis", data->constraint_axis);

  /* Add modal handler */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus sculpt_cursor_transform_modal(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || !ob->runtime->sculpt_session) {
    return OPERATOR_CANCELLED;
  }

  SculptSession *ss = ob->runtime->sculpt_session;
  SculptCursorTransformData *data = static_cast<SculptCursorTransformData *>(op->customdata);

  eSculptCursorTransformMode mode = eSculptCursorTransformMode(RNA_enum_get(op->ptr, "mode"));

  switch (event->type) {
    case MOUSEMOVE: {
      const float2 current_mouse = float2(float(event->mval[0]), float(event->mval[1]));

      if (mode == SCULPT_CURSOR_TRANSFORM_TRANSLATE) {
        const int axis = sculpt_cursor_constraint_axis_index(data->constraint_axis);
        if (axis != -1) {
          float new_world_pos[3];
          if (sculpt_cursor_translate_along_axis(C,
                                                 data->initial_world_pos,
                                                 data->initial_world_mat,
                                                 axis,
                                                 data->initial_mouse,
                                                 current_mouse,
                                                 new_world_pos))
          {
            float3 world_loc(new_world_pos);
            ss->sculpt_cursor_pos = math::transform_point(ob->world_to_object(), world_loc);
          }
        }
      }
      else if (mode == SCULPT_CURSOR_TRANSFORM_ROTATE) {
        const int axis = sculpt_cursor_constraint_axis_index(data->constraint_axis);
        if (axis != -1) {
          float new_rot[4];
          if (sculpt_cursor_rotate_around_axis(C, data, axis, current_mouse, new_rot)) {
            copy_qt_qt(ss->sculpt_cursor_rot, new_rot);
          }
        }
      }
      else if (mode == SCULPT_CURSOR_TRANSFORM_SCALE) {
        float new_scale[3];
        if (sculpt_cursor_scale_from_drag(C, data, current_mouse, new_scale)) {
          copy_v3_v3(ss->sculpt_cursor_scale, new_scale);
        }
      }

      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
      break;
    }

    case LEFTMOUSE:
      if (RNA_boolean_get(op->ptr, "release_confirm") && event->val == KM_RELEASE) {
        BKE_sculpt_cursor_session_to_storage(*ob, *ss);
        MEM_delete(data);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      break;

    case EVT_RETKEY:
      if (event->val == KM_RELEASE) {
        BKE_sculpt_cursor_session_to_storage(*ob, *ss);
        MEM_delete(data);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      break;

    case EVT_ESCKEY:
    case RIGHTMOUSE:
      /* Cancel: restore initial position */
      copy_v3_v3(ss->sculpt_cursor_pos, data->initial_cursor_pos);
      copy_qt_qt(ss->sculpt_cursor_rot, data->initial_cursor_rot);
      copy_v3_v3(ss->sculpt_cursor_scale, data->initial_cursor_scale);
      BKE_sculpt_cursor_session_to_storage(*ob, *ss);
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
      MEM_delete(data);
      op->customdata = nullptr;
      return OPERATOR_CANCELLED;

    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

void SCULPT_OT_cursor_transform(wmOperatorType *ot)
{
  static const EnumPropertyItem mode_items[] = {
      {SCULPT_CURSOR_TRANSFORM_TRANSLATE, "TRANSLATE", 0, "Translate", ""},
      {SCULPT_CURSOR_TRANSFORM_ROTATE, "ROTATE", 0, "Rotate", ""},
      {SCULPT_CURSOR_TRANSFORM_SCALE, "SCALE", 0, "Scale", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Transform Sculpt Cursor";
  ot->idname = "SCULPT_OT_cursor_transform";
  ot->description = "Transform the sculpt cursor";

  ot->invoke = sculpt_cursor_transform_invoke;
  ot->modal = sculpt_cursor_transform_modal;
  ot->poll = sculpt_cursor_set_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  RNA_def_enum(ot->srna, "mode", mode_items, SCULPT_CURSOR_TRANSFORM_TRANSLATE, "Mode", "");

  RNA_def_boolean_vector(
      ot->srna, "constraint_axis", 3, nullptr, "Constraint Axis", "Constrain movement to axis");
  RNA_def_boolean(ot->srna, "release_confirm", false, "Confirm on Release", "");
}

/* ================================================================
 * Registration
 * ================================================================ */

void ED_operatortypes_sculpt_cursor()
{
  WM_operatortype_append(SCULPT_OT_cursor_set);
  WM_operatortype_append(SCULPT_OT_cursor_transform);
}

}  // namespace blender::ed::sculpt_paint::cursor
