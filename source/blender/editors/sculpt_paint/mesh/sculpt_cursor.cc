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
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"
#include "BKE_unit.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_numinput.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_transform.hh"
#include "ED_view3d.hh"

#include "UI_interface_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::cursor {

/* Calculate rotation for sculpt cursor based on the requested orientation. */
static void calc_sculpt_cursor_rotation(bContext *C,
                                        const float3 &normal,
                                        eV3DCursorOrient orientation,
                                        float r_quat[4])
{
  float mat[3][3];

  switch (orientation) {
    case V3D_CURSOR_ORIENT_VIEW: {
      /* Match the viewport, like `ED_view3d_cursor3d_position_rotation`. */
      ARegion *region = CTX_wm_region(C);
      RegionView3D *rv3d = region ? static_cast<RegionView3D *>(region->regiondata) : nullptr;
      if (rv3d) {
        copy_qt_qt(r_quat, rv3d->viewquat);
        r_quat[0] *= -1.0f;
      }
      else {
        unit_qt(r_quat);
      }
      break;
    }

    case V3D_CURSOR_ORIENT_XFORM: {
      ed::transform::calc_orientation_from_type(C, mat);
      mat3_to_quat(r_quat, mat);
      break;
    }

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

/* -------------------------------------------------------------------- */
/** \name Public cursor state API (shared with the viewport gizmo)
 * \{ */

bool is_enabled(const Scene &scene)
{
  const Sculpt *sculpt = scene.toolsettings ? scene.toolsettings->sculpt : nullptr;
  return sculpt && (sculpt->sculpt_cursor_flag & SCULPT_CURSOR_ENABLED);
}

SculptCursorMode mode_get(const Scene &scene)
{
  const Sculpt *sculpt = scene.toolsettings ? scene.toolsettings->sculpt : nullptr;
  return (sculpt && sculpt->sculpt_cursor_mode == SCULPT_CURSOR_MODE_DEFORM) ?
             SculptCursorMode::Deform :
             SculptCursorMode::Set;
}

bool pin_get(const Scene &scene)
{
  const Sculpt *sculpt = scene.toolsettings ? scene.toolsettings->sculpt : nullptr;
  return sculpt && (sculpt->sculpt_cursor_flag & SCULPT_CURSOR_PIN);
}

bool is_shared(const Scene &scene)
{
  const Sculpt *sculpt = scene.toolsettings ? scene.toolsettings->sculpt : nullptr;
  return sculpt && (sculpt->sculpt_cursor_flag & SCULPT_CURSOR_SHARED);
}

CursorState state_get(const Scene &scene, const Object &ob)
{
  CursorState state;
  if (is_shared(scene)) {
    /* The shared cursor lives on the scene 3D cursor in world space. Convert both the location and
     * the rotation into the object's local space so every object in a multi-object sculpt session
     * resolves the exact same world transform. */
    state.location = math::transform_point(ob.world_to_object(), float3(scene.cursor.location));

    float ob_rot[3][3];
    copy_m3_m4(ob_rot, ob.object_to_world().ptr());
    normalize_m3(ob_rot);
    float ob_rot_inv[3][3];
    invert_m3_m3(ob_rot_inv, ob_rot);

    float cursor_rot[3][3];
    copy_m3_m3(cursor_rot, scene.cursor.matrix<float3x3>().ptr());

    float local_rot[3][3];
    mul_m3_m3m3(local_rot, ob_rot_inv, cursor_rot);

    float quat[4];
    mat3_to_quat(quat, local_rot);
    copy_qt_qt(state.rotation, quat);
  }
  else {
    copy_v3_v3(state.location, ob.sculpt_cursor_location);
    copy_v4_v4(state.rotation, ob.sculpt_cursor_rotation);
  }
  return state;
}

/** Write an object-space \a state to the shared scene 3D cursor, converted to world space. */
static void state_set_shared(Scene &scene, const Object &ob, const CursorState &state)
{
  copy_v3_v3(scene.cursor.location,
             math::transform_point(ob.object_to_world(), float3(state.location)));

  float local_rot[3][3];
  quat_to_mat3(local_rot, state.rotation);
  float ob_rot[3][3];
  copy_m3_m4(ob_rot, ob.object_to_world().ptr());
  normalize_m3(ob_rot);
  float world_rot[3][3];
  mul_m3_m3m3(world_rot, ob_rot, local_rot);

  float quat[4];
  mat3_to_quat(quat, world_rot);
  scene.cursor.set_rotation(math::Quaternion(quat[0], quat[1], quat[2], quat[3]), true);

  DEG_id_tag_update(&scene.id, ID_RECALC_SYNC_TO_EVAL);
}

void state_set(Scene &scene, Object &ob, const CursorState &state)
{
  copy_v3_v3(ob.sculpt_cursor_location, state.location);
  copy_v4_v4(ob.sculpt_cursor_rotation, state.rotation);
  ob.sculpt_cursor_initialized = 1;

  DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL);
  if (is_shared(scene)) {
    /* Mirror the full transform (location and rotation) back to the shared scene 3D cursor so it
     * stays identical for every object following it. */
    state_set_shared(scene, ob, state);
  }
}

void shared_cursor_sync_to_scene(Scene &scene, const Object &ob)
{
  /* Adopt the object's own stored sculpt cursor. Reading through #state_get would be wrong here:
   * in shared mode it already resolves back from the scene cursor, which is exactly what we are
   * about to initialize. */
  CursorState state;
  copy_v3_v3(state.location, ob.sculpt_cursor_location);
  copy_v4_v4(state.rotation, ob.sculpt_cursor_rotation);
  state_set_shared(scene, ob, state);
}

float4x4 world_matrix_get(const Scene &scene, const Object &ob)
{
  const CursorState state = state_get(scene, ob);

  float local_rot[3][3];
  quat_to_mat3(local_rot, state.rotation);

  float ob_rot[3][3];
  copy_m3_m4(ob_rot, ob.object_to_world().ptr());
  normalize_m3(ob_rot);

  float world_rot[3][3];
  mul_m3_m3m3(world_rot, ob_rot, local_rot);
  normalize_m3(world_rot);

  float4x4 mat = float4x4::identity();
  copy_m4_m3(mat.ptr(), world_rot);
  mat.location() = math::transform_point(ob.object_to_world(), float3(state.location));
  return mat;
}

float4x4 symmetry_cursor_to_world(const Scene &scene, const Object &symm_reference_ob)
{
  const Sculpt *sculpt = scene.toolsettings ? scene.toolsettings->sculpt : nullptr;
  if (sculpt == nullptr || sculpt->symmetry_cursor_source == SCULPT_SYMM_CURSOR_OBJECT) {
    return scene.cursor.matrix<float4x4>();
  }
  return world_matrix_get(scene, symm_reference_ob);
}

/** \} */

/**
 * Fill the operator's unset `use_depth`/`orientation` from the 3D Cursor tool's stored settings.
 *
 * The tool reference is read without activating the tool, so placing the cursor through the global
 * Shift+RMB shortcut in any other tool still honors the options shown in the cursor tool's header
 * (same approach as #view3d_cursor3d_tool_orientation_get for the scene 3D cursor).
 */
static void sculpt_cursor_set_props_from_tool(bContext *C, wmOperator *op)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  if (!workspace || !bmain || !scene || !view_layer) {
    return;
  }

  bToolKey tkey{};
  tkey.space_type = SPACE_VIEW3D;
  tkey.mode = WM_toolsystem_mode_from_spacetype(*bmain, scene, view_layer, nullptr, SPACE_VIEW3D);
  bToolRef *tref = WM_toolsystem_ref_find(workspace, &tkey);
  if (!tref) {
    return;
  }

  PointerRNA tool_ptr;
  if (!WM_toolsystem_ref_properties_get_from_operator_for_tool(
          tref, "builtin.sculpt_cursor", op->type, &tool_ptr))
  {
    return;
  }

  for (const char *name : {"orientation", "use_depth"}) {
    PropertyRNA *prop = RNA_struct_find_property(op->ptr, name);
    PropertyRNA *tool_prop = RNA_struct_find_property(&tool_ptr, name);
    if (!prop || !tool_prop || RNA_property_is_set(op->ptr, prop) ||
        !RNA_property_is_set(&tool_ptr, tool_prop))
    {
      continue;
    }
    if (RNA_property_type(prop) == PROP_ENUM) {
      RNA_property_enum_set(op->ptr, prop, RNA_property_enum_get(&tool_ptr, tool_prop));
    }
    else {
      RNA_property_boolean_set(op->ptr, prop, RNA_property_boolean_get(&tool_ptr, tool_prop));
    }
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

  sculpt_cursor_set_props_from_tool(C, op);

  /* Get parameters from RNA */
  bool use_depth = RNA_boolean_get(op->ptr, "use_depth");
  eV3DCursorOrient orientation = eV3DCursorOrient(RNA_enum_get(op->ptr, "orientation"));

  float3 location;
  float3 normal;
  bool hit = false;

  const float2 mval(float(event->mval[0]), float(event->mval[1]));

  /* Try to get position on mesh surface */
  if (use_depth) {
    Object *hit_ob = nullptr;
    std::optional<CursorGeometryInfo> cursor_info = cursor_geometry_info_update(
        C, mval, true, &hit_ob);

    if (cursor_info.has_value()) {
      /* The surface point/normal come back in the HIT object's local space. In multi-object sculpt
       * the hit object may be a non-active one, so route them through world space and store the
       * result in the active object's space (how the rest of the code consumes the cursor). */
      Object &src_ob = hit_ob ? *hit_ob : *ob;
      const float3 world_location = math::transform_point(src_ob.object_to_world(),
                                                          cursor_info->location);
      location = math::transform_point(ob->world_to_object(), world_location);

      const float3 world_normal = math::normalize(
          math::transform_direction(src_ob.object_to_world(), cursor_info->normal));
      normal = math::normalize(math::transform_direction(ob->world_to_object(), world_normal));
      hit = true;
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

  /* Save position and orientation. */
  CursorState state;
  state.location = location;
  calc_sculpt_cursor_rotation(C, normal, orientation, state.rotation);
  state_set(*scene, *ob, state);

  /* Placing the cursor also turns it on; this is the entry point for a scene that has never used
   * the cursor, before the top-bar On/Off toggle is available. */
  Sculpt *sculpt = scene->toolsettings ? scene->toolsettings->sculpt : nullptr;
  if (sculpt && !(sculpt->sculpt_cursor_flag & SCULPT_CURSOR_ENABLED)) {
    sculpt->sculpt_cursor_flag |= SCULPT_CURSOR_ENABLED;
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  }

  /* Notifier for redraw */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

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
  /* The cursor is object data outside the sculpt undo step, so an undo would leave an empty step;
   * match the regular 3D cursor (`VIEW3D_OT_cursor3d`), which is not undoable either. */
  ot->flag = OPTYPE_REGISTER;

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
 * Operator for Transform sculpt cursor (move/rotate)
 * ================================================================ */

enum eSculptCursorTransformMode {
  SCULPT_CURSOR_TRANSFORM_TRANSLATE = 0,
  SCULPT_CURSOR_TRANSFORM_ROTATE = 1,
};

struct SculptCursorTransformData {
  /** Cursor state when the drag started. */
  CursorState initial;
  /** Cursor state as currently dragged. */
  CursorState current;

  float initial_world_pos[3];
  float initial_world_mat[4][4];

  float2 initial_mouse;
  float2 last_mouse;
  bool constraint_axis[3];
};

static int sculpt_cursor_constraint_axis_index(const bool constraint_axis[3]);

static bool sculpt_cursor_rotate_angle_from_axis(const bContext *C,
                                                 const SculptCursorTransformData *data,
                                                 const float axis_vec[3],
                                                 const float2 &curr_mval,
                                                 float *r_angle);

static float sculpt_cursor_rotation_snap_increment(const bContext *C, int modifier);
static float sculpt_cursor_snap_angle(float angle, float snap_increment);

static void sculpt_cursor_dist_to_str(char *r_str,
                                      const int str_maxncpy,
                                      const double val,
                                      const UnitSettings &unit)
{
  BKE_unit_value_as_string_scaled(
      r_str, str_maxncpy, val, 4 * -1, B_UNIT_LENGTH, unit, false, true);
}

static const char *sculpt_cursor_constraint_axis_name(const int axis)
{
  BLI_assert(axis >= 0 && axis < 3);
  static const char *axis_names[] = {IFACE_("X"), IFACE_("Y"), IFACE_("Z")};
  return axis_names[axis];
}

static void sculpt_cursor_transform_header_clear(bContext *C)
{
  ED_area_status_text(CTX_wm_area(C), nullptr);
}

static float sculpt_cursor_rotation_snap_increment(const bContext *C, int modifier)
{
  if ((modifier & KM_CTRL) == 0) {
    return 0.0f;
  }
  const ToolSettings *ts = CTX_data_tool_settings(C);
  float increment = ts ? ts->snap_angle_increment_3d : DEG2RADF(5.0f);
  if ((modifier & KM_SHIFT) != 0) {
    increment = ts ? ts->snap_angle_increment_3d_precision : DEG2RADF(1.0f);
  }
  if (increment <= 0.0f) {
    increment = DEG2RADF(5.0f);
  }
  return increment;
}

static float sculpt_cursor_snap_angle(float angle, float snap_increment)
{
  if (snap_increment <= 0.0f) {
    return angle;
  }
  return roundf(angle / snap_increment) * snap_increment;
}

static void sculpt_cursor_transform_header_update(bContext *C,
                                                  wmOperator *op,
                                                  const Object &ob,
                                                  const CursorState &state,
                                                  const SculptCursorTransformData &data,
                                                  const float2 *curr_mval = nullptr,
                                                  float snap_increment = 0.0f)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return;
  }

  const Scene *scene = CTX_data_scene(C);
  const UnitSettings &unit = scene->unit;
  char str[UI_MAX_DRAW_STR];
  str[0] = '\0';

  const eSculptCursorTransformMode mode = eSculptCursorTransformMode(RNA_enum_get(op->ptr, "mode"));
  const int axis = sculpt_cursor_constraint_axis_index(data.constraint_axis);

  switch (mode) {
    case SCULPT_CURSOR_TRANSFORM_TRANSLATE: {
      float3 world_pos = state.location;
      mul_m4_v3(ob.object_to_world().ptr(), world_pos);

      float world_delta[3];
      sub_v3_v3v3(world_delta, world_pos, data.initial_world_pos);

      if (axis != -1) {
        float axis_no[3];
        normalize_v3_v3(axis_no, data.initial_world_mat[axis]);
        const float along = dot_v3v3(axis_no, world_delta);

        char val_str[NUM_STR_REP_LEN];
        char dist_str[NUM_STR_REP_LEN];
        sculpt_cursor_dist_to_str(val_str, sizeof(val_str), double(along), unit);
        sculpt_cursor_dist_to_str(dist_str, sizeof(dist_str), double(fabsf(along)), unit);

        char axis_text[64];
        SNPRINTF_UTF8(axis_text,
                      IFACE_(" along %s"),
                      sculpt_cursor_constraint_axis_name(axis));
        BLI_snprintf_utf8(str, sizeof(str), "D: %s (%s)%s", val_str, dist_str, axis_text);
      }
      else {
        char dvec_str[3][NUM_STR_REP_LEN];
        char dist_str[NUM_STR_REP_LEN];
        for (int i = 0; i < 3; i++) {
          sculpt_cursor_dist_to_str(dvec_str[i], sizeof(dvec_str[i]), double(world_delta[i]), unit);
        }
        sculpt_cursor_dist_to_str(dist_str, sizeof(dist_str), double(len_v3(world_delta)), unit);
        BLI_snprintf_utf8(str,
                          sizeof(str),
                          "Dx: %s   Dy: %s   Dz: %s (%s)",
                          dvec_str[0],
                          dvec_str[1],
                          dvec_str[2],
                          dist_str);
      }
      break;
    }
    case SCULPT_CURSOR_TRANSFORM_ROTATE: {
      float angle = 0.0f;
      if (curr_mval != nullptr) {
        if (axis != -1) {
          sculpt_cursor_rotate_angle_from_axis(
              C, &data, data.initial_world_mat[axis], *curr_mval, &angle);
        }
        else {
          ARegion *region = CTX_wm_region(C);
          RegionView3D *rv3d = region ? static_cast<RegionView3D *>(region->regiondata) : nullptr;
          if (rv3d) {
            sculpt_cursor_rotate_angle_from_axis(C, &data, rv3d->viewinv[2], *curr_mval, &angle);
          }
        }
        angle = sculpt_cursor_snap_angle(angle, snap_increment);
      }

      if (axis != -1) {
        char axis_text[64];
        SNPRINTF_UTF8(axis_text,
                      IFACE_(" along %s"),
                      sculpt_cursor_constraint_axis_name(axis));
        BLI_snprintf_utf8(str, sizeof(str), IFACE_("Rotation: %.2f%s"), RAD2DEGF(angle), axis_text);
      }
      else {
        BLI_snprintf_utf8(str, sizeof(str), IFACE_("Rotation: %.2f"), RAD2DEGF(angle));
      }
      break;
    }
  }

  ED_area_status_text(area, str);
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

static bool sculpt_cursor_rotate_angle_from_axis(const bContext *C,
                                                 const SculptCursorTransformData *data,
                                                 const float axis_vec_in[3],
                                                 const float2 &curr_mval,
                                                 float *r_angle)
{
  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return false;
  }

  float axis_vec[3];
  normalize_v3_v3(axis_vec, axis_vec_in);

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

  *r_angle = angle_signed_on_axis_v3v3_v3(proj_init, proj_curr, axis_vec);
  return true;
}

static bool sculpt_cursor_rotate_around_axis(const bContext *C,
                                             const SculptCursorTransformData *data,
                                             const int axis,
                                             const float2 &curr_mval,
                                             float r_new_cursor_rot[4],
                                             float snap_increment = 0.0f)
{
  float angle;
  if (!sculpt_cursor_rotate_angle_from_axis(
          C, data, data->initial_world_mat[axis], curr_mval, &angle))
  {
    return false;
  }
  angle = sculpt_cursor_snap_angle(angle, snap_increment);

  float cursor_mat[3][3];
  quat_to_mat3(cursor_mat, data->initial.rotation);
  float axis_local[3];
  copy_v3_v3(axis_local, cursor_mat[axis]);

  float delta_quat[4];
  axis_angle_to_quat(delta_quat, axis_local, angle);
  mul_qt_qtqt(r_new_cursor_rot, delta_quat, data->initial.rotation);
  return true;
}

/**
 * Screen-aligned ring: rotate the cursor around the viewport's view axis (world space), unlike the
 * local-axis dials. The world-space delta is conjugated back into object space.
 */
static bool sculpt_cursor_rotate_around_view_axis(const bContext *C,
                                                  const Object &ob,
                                                  const SculptCursorTransformData *data,
                                                  const float2 &curr_mval,
                                                  float r_new_cursor_rot[4],
                                                  float snap_increment = 0.0f)
{
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = region ? static_cast<RegionView3D *>(region->regiondata) : nullptr;
  if (!rv3d) {
    return false;
  }

  float angle;
  if (!sculpt_cursor_rotate_angle_from_axis(C, data, rv3d->viewinv[2], curr_mval, &angle)) {
    return false;
  }
  angle = sculpt_cursor_snap_angle(angle, snap_increment);

  float ob_rot[3][3];
  copy_m3_m4(ob_rot, ob.object_to_world().ptr());
  normalize_m3(ob_rot);

  float local_rot[3][3];
  quat_to_mat3(local_rot, data->initial.rotation);
  float world_rot[3][3];
  mul_m3_m3m3(world_rot, ob_rot, local_rot);

  float delta[3][3];
  axis_angle_to_mat3(delta, rv3d->viewinv[2], angle);

  float new_world_rot[3][3];
  mul_m3_m3m3(new_world_rot, delta, world_rot);

  float ob_rot_inv[3][3];
  invert_m3_m3(ob_rot_inv, ob_rot);
  float new_local_rot[3][3];
  mul_m3_m3m3(new_local_rot, ob_rot_inv, new_world_rot);

  mat3_to_quat(r_new_cursor_rot, new_local_rot);
  return true;
}

static wmOperatorStatus sculpt_cursor_transform_finish(bContext *C, wmOperator *op)
{
  SculptCursorTransformData *data = static_cast<SculptCursorTransformData *>(op->customdata);
  Object *ob = CTX_data_active_object(C);
  if (ob) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
  sculpt_cursor_transform_header_clear(C);
  MEM_delete(data);
  op->customdata = nullptr;
  return OPERATOR_FINISHED;
}

/**
 * Unconstrained translation (the center circle): move the cursor freely in the plane through its
 * initial world position parallel to the screen.
 */
static bool sculpt_cursor_translate_free(const bContext *C,
                                         const SculptCursorTransformData *data,
                                         const float2 &curr_mval,
                                         float r_new_world_pos[3])
{
  ARegion *region = CTX_wm_region(C);
  const View3D *v3d = CTX_wm_view3d(C);
  if (!region || !v3d) {
    return false;
  }
  const float init_mval[2] = {data->initial_mouse.x, data->initial_mouse.y};
  const float curr_mval_fl[2] = {curr_mval.x, curr_mval.y};
  float init_co[3], curr_co[3];
  ED_view3d_win_to_3d(v3d, region, data->initial_world_pos, init_mval, init_co);
  ED_view3d_win_to_3d(v3d, region, data->initial_world_pos, curr_mval_fl, curr_co);
  float offset[3];
  sub_v3_v3v3(offset, curr_co, init_co);
  add_v3_v3v3(r_new_world_pos, data->initial_world_pos, offset);
  return true;
}

static wmOperatorStatus sculpt_cursor_transform_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  if (!ob || !ob->runtime->sculpt_session || !scene) {
    return OPERATOR_CANCELLED;
  }

  if (!is_enabled(*scene)) {
    return OPERATOR_CANCELLED;
  }

  /* Allocate custom data */
  SculptCursorTransformData *data = MEM_new<SculptCursorTransformData>(__func__);
  op->customdata = data;

  /* Store initial state */
  data->initial = state_get(*scene, *ob);
  data->current = data->initial;
  copy_m4_m4(data->initial_world_mat, world_matrix_get(*scene, *ob).ptr());
  copy_v3_v3(data->initial_world_pos, data->initial_world_mat[3]);
  data->initial_mouse = float2(float(event->mval[0]), float(event->mval[1]));
  data->last_mouse = data->initial_mouse;
  RNA_boolean_get_array(op->ptr, "constraint_axis", data->constraint_axis);

  sculpt_cursor_transform_header_update(C, op, *ob, data->current, *data);

  /* Add modal handler */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus sculpt_cursor_transform_modal(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  if (!ob || !ob->runtime->sculpt_session || !scene) {
    sculpt_cursor_transform_header_clear(C);
    return OPERATOR_CANCELLED;
  }

  SculptCursorTransformData *data = static_cast<SculptCursorTransformData *>(op->customdata);

  eSculptCursorTransformMode mode = eSculptCursorTransformMode(RNA_enum_get(op->ptr, "mode"));

  switch (event->type) {
    case MOUSEMOVE: {
      const float2 current_mouse = float2(float(event->mval[0]), float(event->mval[1]));
      data->last_mouse = current_mouse;
      const float snap_increment = (mode == SCULPT_CURSOR_TRANSFORM_ROTATE) ?
                                       sculpt_cursor_rotation_snap_increment(
                                           C, event->modifier) :
                                       0.0f;

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
            data->current.location = math::transform_point(ob->world_to_object(),
                                                            float3(new_world_pos));
          }
        }
        else {
          /* Center circle: free move in the screen-parallel plane. */
          float new_world_pos[3];
          if (sculpt_cursor_translate_free(C, data, current_mouse, new_world_pos)) {
            data->current.location = math::transform_point(ob->world_to_object(),
                                                            float3(new_world_pos));
          }
        }
      }
      else if (mode == SCULPT_CURSOR_TRANSFORM_ROTATE) {
        const int axis = sculpt_cursor_constraint_axis_index(data->constraint_axis);
        if (axis != -1) {
          float new_rot[4];
          if (sculpt_cursor_rotate_around_axis(
                  C, data, axis, current_mouse, new_rot, snap_increment))
          {
            copy_qt_qt(data->current.rotation, new_rot);
          }
        }
        else {
          /* Screen-aligned ring: rotate around the viewport's view axis. */
          float new_rot[4];
          if (sculpt_cursor_rotate_around_view_axis(
                  C, *ob, data, current_mouse, new_rot, snap_increment))
          {
            copy_qt_qt(data->current.rotation, new_rot);
          }
        }
      }

      state_set(*scene, *ob, data->current);
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
      sculpt_cursor_transform_header_update(
          C, op, *ob, data->current, *data, &current_mouse, snap_increment);
      break;
    }

    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY:
    case EVT_LEFTSHIFTKEY:
    case EVT_RIGHTSHIFTKEY: {
      if (mode != SCULPT_CURSOR_TRANSFORM_ROTATE) {
        break;
      }
      /* Toggling Ctrl/Shift mid-drag re-snaps at the stored mouse position,
       * same as rotation in other Blender tools. */
      int modifier = event->modifier;
      if (event->val == KM_PRESS) {
        if (event->type == EVT_LEFTCTRLKEY || event->type == EVT_RIGHTCTRLKEY) {
          modifier |= KM_CTRL;
        }
        if (event->type == EVT_LEFTSHIFTKEY || event->type == EVT_RIGHTSHIFTKEY) {
          modifier |= KM_SHIFT;
        }
      }
      else if (event->val == KM_RELEASE) {
        if (event->type == EVT_LEFTCTRLKEY || event->type == EVT_RIGHTCTRLKEY) {
          modifier &= ~KM_CTRL;
        }
        if (event->type == EVT_LEFTSHIFTKEY || event->type == EVT_RIGHTSHIFTKEY) {
          modifier &= ~KM_SHIFT;
        }
      }
      const float snap_increment = sculpt_cursor_rotation_snap_increment(C, modifier);
      const int axis = sculpt_cursor_constraint_axis_index(data->constraint_axis);
      if (axis != -1) {
        float new_rot[4];
        if (sculpt_cursor_rotate_around_axis(
                C, data, axis, data->last_mouse, new_rot, snap_increment))
        {
          copy_qt_qt(data->current.rotation, new_rot);
        }
      }
      else {
        float new_rot[4];
        if (sculpt_cursor_rotate_around_view_axis(
                C, *ob, data, data->last_mouse, new_rot, snap_increment))
        {
          copy_qt_qt(data->current.rotation, new_rot);
        }
      }

      state_set(*scene, *ob, data->current);
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
      sculpt_cursor_transform_header_update(
          C, op, *ob, data->current, *data, &data->last_mouse, snap_increment);
      break;
    }

    case LEFTMOUSE:
      if (RNA_boolean_get(op->ptr, "release_confirm") && event->val == KM_RELEASE) {
        return sculpt_cursor_transform_finish(C, op);
      }
      break;

    case EVT_RETKEY:
      if (event->val == KM_RELEASE) {
        return sculpt_cursor_transform_finish(C, op);
      }
      break;

    case EVT_ESCKEY:
    case RIGHTMOUSE:
      /* Cancel: restore initial state */
      state_set(*scene, *ob, data->initial);
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
      sculpt_cursor_transform_header_clear(C);
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
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Transform Sculpt Cursor";
  ot->idname = "SCULPT_OT_cursor_transform";
  ot->description = "Transform the sculpt cursor";

  ot->invoke = sculpt_cursor_transform_invoke;
  ot->modal = sculpt_cursor_transform_modal;
  ot->poll = sculpt_cursor_set_poll;

  /* Not undoable: see #SCULPT_OT_cursor_set. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_BLOCKING;

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
