/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 * Sculpt Cursor Gizmo - Interactive transform widget for sculpt cursor
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_constants.h"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "DNA_object_types.h"

#include "DNA_theme_types.h"

#include "DNA_userdef_types.h"

#include "DNA_view3d_types.h"

#include "DNA_workspace_types.h"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_transform.hh"
#include "ED_view3d.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "RNA_access.hh"

namespace blender::ed::view3d {

/** Scale of the two-axis planes (inside rotation dials). */
#define SCULPT_CURSOR_SCALE_PLANE_SCALE 0.5f

/* Axis indices. Ranges are kept contiguous per axis type. */
enum SculptCursorAxis {
  SCULPT_CURSOR_SCALE_C = 0,
  SCULPT_CURSOR_SCALE_X,
  SCULPT_CURSOR_SCALE_Y,
  SCULPT_CURSOR_SCALE_Z,
  SCULPT_CURSOR_SCALE_XY,
  SCULPT_CURSOR_SCALE_YZ,
  SCULPT_CURSOR_SCALE_ZX,
  SCULPT_CURSOR_ROT_X,
  SCULPT_CURSOR_ROT_Y,
  SCULPT_CURSOR_ROT_Z,
  SCULPT_CURSOR_ROT_C,
  SCULPT_CURSOR_TRANS_X,
  SCULPT_CURSOR_TRANS_Y,
  SCULPT_CURSOR_TRANS_Z,
  SCULPT_CURSOR_TRANS_C,
  SCULPT_CURSOR_AXIS_LEN,
};

enum SculptCursorAxisType {
  SCULPT_CURSOR_AXES_TRANSLATE = 0,
  SCULPT_CURSOR_AXES_ROTATE,
  SCULPT_CURSOR_AXES_SCALE,
};

struct SculptCursorGizmoGroup {
  wmGizmo *gizmos[SCULPT_CURSOR_AXIS_LEN];
  /** Mode the axis operators are currently bound to, to avoid rebinding on every refresh.
   * -1 until the first bind. */
  int bound_mode;
  /** Handle types shown by the last refresh, see #gizmo_transform_tool_get. */
  int show_flag;

  /**
   * Captured when a handle bound to a Transform operator is pressed: the gizmo axes and the
   * cursor's world rotation (which #createTransSculpt turns into the Transform pivot). While
   * dragging, the pivot's rotation delta is applied to `drag_orient`, so a Rotate drag turns the
   * gizmo drawn in the tool's orientation instead of snapping it to the cursor axes.
   */
  float drag_orient[3][3];
  float drag_pivot_rot_init[4];
  /** View rotation of the last refresh, to re-align a view-oriented gizmo when orbiting. */
  float prev_viewinv[3][3];
};

/* Threshold for hiding translate axes pointing towards the view. */
static const struct {
  float min, max;
} g_axis_range = {0.02f, 0.1f};

/* -------------------------------------------------------------------- */
/** \name Gizmo Setup (based on transform_gizmo_3d.cc)
 * \{ */

static SculptCursorAxisType sculpt_cursor_axis_type_get(const int axis_idx)
{
  if (axis_idx >= SCULPT_CURSOR_TRANS_X && axis_idx <= SCULPT_CURSOR_TRANS_C) {
    return SCULPT_CURSOR_AXES_TRANSLATE;
  }
  if (axis_idx >= SCULPT_CURSOR_ROT_X && axis_idx <= SCULPT_CURSOR_ROT_C) {
    return SCULPT_CURSOR_AXES_ROTATE;
  }
  if ((axis_idx >= SCULPT_CURSOR_SCALE_X && axis_idx <= SCULPT_CURSOR_SCALE_ZX) ||
      axis_idx == SCULPT_CURSOR_SCALE_C)
  {
    return SCULPT_CURSOR_AXES_SCALE;
  }
  return SCULPT_CURSOR_AXES_TRANSLATE;
}

/** Index into the standard `TRANSFORM_OT_*` operators: 0 = translate, 1 = rotate, 2 = scale. */
static int sculpt_cursor_axis_transform_mode(const int axis_idx)
{
  switch (sculpt_cursor_axis_type_get(axis_idx)) {
    case SCULPT_CURSOR_AXES_ROTATE:
      return 1;
    case SCULPT_CURSOR_AXES_SCALE:
      return 2;
    case SCULPT_CURSOR_AXES_TRANSLATE:
    default:
      return 0;
  }
}

/**
 * Set-mode mode for #SCULPT_OT_cursor_transform. Scale handles are Deform-only, so a scale axis
 * falls back to translate -- the handle is hidden in Set mode, so this is never reached anyway.
 */
static int sculpt_cursor_axis_set_mode(const int axis_idx)
{
  return (sculpt_cursor_axis_type_get(axis_idx) == SCULPT_CURSOR_AXES_ROTATE) ? 1 : 0;
}

static void gizmo_line_range(const SculptCursorAxisType axis_type, float *r_start, float *r_end)
{
  float start = 0.2f;
  float end = 1.0f;

  /* Mirrors #transform_gizmo_3d.cc #gizmo_line_range for the combined
   * translate|rotate|scale gizmo this cursor always draws. */
  switch (axis_type) {
    case SCULPT_CURSOR_AXES_TRANSLATE: {
      /* Scale handles occupy the outer range. */
      start = end - 0.125f;
      /* Avoid rotate and translate gizmos overlap. */
      const float rotate_offset = 0.215f;
      start += rotate_offset;
      end += rotate_offset + 0.2f;
      break;
    }
    case SCULPT_CURSOR_AXES_SCALE:
      end -= 0.225f;
      break;
    case SCULPT_CURSOR_AXES_ROTATE:
      break;
  }

  if (r_start) {
    *r_start = start;
  }
  if (r_end) {
    *r_end = end;
  }
}

static void gizmo_setup_axis_matrix(wmGizmo *axis, const int axis_idx)
{
  float matrix[3][3];

  switch (axis_idx) {
    case SCULPT_CURSOR_TRANS_X:
    case SCULPT_CURSOR_SCALE_X:
    case SCULPT_CURSOR_ROT_X:
      copy_v3_fl3(matrix[0], 0.0f, -1.0f, 0.0f);
      copy_v3_fl3(matrix[1], 0.0f, 0.0f, -1.0f);
      copy_v3_fl3(matrix[2], 1.0f, 0.0f, 0.0f);
      break;
    case SCULPT_CURSOR_TRANS_Y:
    case SCULPT_CURSOR_SCALE_Y:
    case SCULPT_CURSOR_ROT_Y:
      copy_v3_fl3(matrix[0], 1.0f, 0.0f, 0.0f);
      copy_v3_fl3(matrix[1], 0.0f, 0.0f, -1.0f);
      copy_v3_fl3(matrix[2], 0.0f, 1.0f, 0.0f);
      break;
    case SCULPT_CURSOR_TRANS_Z:
    case SCULPT_CURSOR_SCALE_Z:
    case SCULPT_CURSOR_ROT_Z:
      copy_v3_fl3(matrix[0], 1.0f, 0.0f, 0.0f);
      copy_v3_fl3(matrix[1], 0.0f, 1.0f, 0.0f);
      copy_v3_fl3(matrix[2], 0.0f, 0.0f, 1.0f);
      break;
    case SCULPT_CURSOR_SCALE_XY:
      copy_v3_fl3(matrix[0], -M_SQRT1_2, M_SQRT1_2, 0.0f);
      copy_v3_fl3(matrix[1], 0.0f, 0.0f, 1.0f);
      copy_v3_fl3(matrix[2], M_SQRT1_2, M_SQRT1_2, 0.0f);
      break;
    case SCULPT_CURSOR_SCALE_YZ:
      copy_v3_fl3(matrix[0], 0.0f, -M_SQRT1_2, M_SQRT1_2);
      copy_v3_fl3(matrix[1], 1.0f, 0.0f, 0.0f);
      copy_v3_fl3(matrix[2], 0, M_SQRT1_2, M_SQRT1_2);
      break;
    case SCULPT_CURSOR_SCALE_ZX:
      copy_v3_fl3(matrix[0], M_SQRT1_2, 0.0f, -M_SQRT1_2);
      copy_v3_fl3(matrix[1], 0.0f, 1.0f, 0.0f);
      copy_v3_fl3(matrix[2], M_SQRT1_2, 0.0f, M_SQRT1_2);
      break;
    default:
      return;
  }

  copy_m4_m3(axis->matrix_offset, matrix);
}

static void gizmo_get_axis_constraint(const int axis_idx, bool r_axis[3])
{
  r_axis[0] = r_axis[1] = r_axis[2] = false;

  switch (axis_idx) {
    case SCULPT_CURSOR_TRANS_X:
    case SCULPT_CURSOR_ROT_X:
    case SCULPT_CURSOR_SCALE_X:
      r_axis[0] = true;
      break;
    case SCULPT_CURSOR_TRANS_Y:
    case SCULPT_CURSOR_ROT_Y:
    case SCULPT_CURSOR_SCALE_Y:
      r_axis[1] = true;
      break;
    case SCULPT_CURSOR_TRANS_Z:
    case SCULPT_CURSOR_ROT_Z:
    case SCULPT_CURSOR_SCALE_Z:
      r_axis[2] = true;
      break;
    case SCULPT_CURSOR_SCALE_XY:
      r_axis[0] = r_axis[1] = true;
      break;
    case SCULPT_CURSOR_SCALE_YZ:
      r_axis[1] = r_axis[2] = true;
      break;
    case SCULPT_CURSOR_SCALE_ZX:
      r_axis[2] = r_axis[0] = true;
      break;
    default:
      break;
  }
}

static void gizmo_setup_draw(wmGizmo *axis, const int axis_idx)
{
  gizmo_setup_axis_matrix(axis, axis_idx);

  const SculptCursorAxisType axis_type = sculpt_cursor_axis_type_get(axis_idx);

  switch (axis_idx) {
    case SCULPT_CURSOR_TRANS_X:
    case SCULPT_CURSOR_TRANS_Y:
    case SCULPT_CURSOR_TRANS_Z: {
      float start, end;
      gizmo_line_range(axis_type, &start, &end);
      mul_v3_v3fl(axis->matrix_offset[3], axis->matrix_offset[2], start);
      RNA_float_set(axis->ptr, "length", end - start);
      RNA_enum_set(axis->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_NORMAL);
      RNA_enum_set(axis->ptr, "draw_options", ED_GIZMO_ARROW_DRAW_FLAG_STEM);
      WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_OFFSET_SCALE, true);
      WM_gizmo_set_line_width(axis, 2.0f);
      break;
    }
    case SCULPT_CURSOR_SCALE_X:
    case SCULPT_CURSOR_SCALE_Y:
    case SCULPT_CURSOR_SCALE_Z: {
      float start, end;
      gizmo_line_range(axis_type, &start, &end);
      mul_v3_v3fl(axis->matrix_offset[3], axis->matrix_offset[2], start);
      RNA_float_set(axis->ptr, "length", end - start);
      RNA_enum_set(axis->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_BOX);
      RNA_enum_set(axis->ptr, "draw_options", ED_GIZMO_ARROW_DRAW_FLAG_STEM);
      WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_OFFSET_SCALE, true);
      WM_gizmo_set_line_width(axis, 2.0f);
      /* Prefer scale handles over rotation dials when overlapping. */
      axis->select_bias = 2.0f;
      break;
    }
    case SCULPT_CURSOR_SCALE_XY:
    case SCULPT_CURSOR_SCALE_YZ:
    case SCULPT_CURSOR_SCALE_ZX:
      RNA_enum_set(axis->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_PLANE);
      RNA_enum_set(axis->ptr, "draw_options", 0);
      RNA_float_set(axis->ptr, "length", SCULPT_CURSOR_SCALE_PLANE_SCALE);
      WM_gizmo_set_line_width(axis, 1.0f);
      axis->select_bias = 2.0f;
      break;
    case SCULPT_CURSOR_SCALE_C:
      RNA_enum_set(axis->ptr, "draw_style", ED_GIZMO_PRIMITIVE_STYLE_ANNULUS);
      RNA_boolean_set(axis->ptr, "draw_inner", false);
      RNA_float_set(axis->ptr, "arc_inner_factor", 6.0f);
      WM_gizmo_set_scale(axis, 0.2f);
      axis->select_bias = -2.0f;
      break;
    case SCULPT_CURSOR_ROT_X:
    case SCULPT_CURSOR_ROT_Y:
    case SCULPT_CURSOR_ROT_Z:
      /* Clip the dial against the geometry behind it, as in #transform_gizmo_3d.cc. */
      RNA_enum_set(axis->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_CLIP);
      RNA_float_set(axis->ptr, "incremental_angle", 0.0f);
      WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_VALUE, true);
      WM_gizmo_set_line_width(axis, 3.0f);
      /* Deprioritize rotation dials vs scale handles when hit regions overlap. */
      axis->select_bias = -2.0f;
      break;
    case SCULPT_CURSOR_ROT_C:
      /* Screen-aligned ring, like #MAN_AXIS_ROT_C in #transform_gizmo_3d.cc. */
      RNA_enum_set(axis->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_NOP);
      RNA_float_set(axis->ptr, "incremental_angle", 0.0f);
      WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_VALUE, true);
      WM_gizmo_set_scale(axis, 1.2f);
      WM_gizmo_set_line_width(axis, 3.0f);
      axis->select_bias = 0.0f;
      break;
    case SCULPT_CURSOR_TRANS_C:
      /* Central circle, like #MAN_AXIS_TRANS_C in #transform_gizmo_3d.cc. */
      RNA_enum_set(axis->ptr, "draw_style", ED_GIZMO_PRIMITIVE_STYLE_CIRCLE);
      RNA_boolean_set(axis->ptr, "draw_inner", false);
      WM_gizmo_set_scale(axis, 0.2f);
      /* Prevent axis gizmos overlapping the center point (see #transform_gizmo_3d.cc #63744). */
      axis->select_bias = 2.0f;
      break;
    default:
      break;
  }
}

static int sculpt_cursor_axis_norm_index(const int axis_idx)
{
  switch (axis_idx) {
    case SCULPT_CURSOR_TRANS_X:
    case SCULPT_CURSOR_ROT_X:
    case SCULPT_CURSOR_SCALE_X:
    case SCULPT_CURSOR_SCALE_YZ:
      return 0;
    case SCULPT_CURSOR_TRANS_Y:
    case SCULPT_CURSOR_ROT_Y:
    case SCULPT_CURSOR_SCALE_Y:
    case SCULPT_CURSOR_SCALE_ZX:
      return 1;
    case SCULPT_CURSOR_TRANS_Z:
    case SCULPT_CURSOR_ROT_Z:
    case SCULPT_CURSOR_SCALE_Z:
    case SCULPT_CURSOR_SCALE_XY:
      return 2;
    default:
      return -1;
  }
}

static void gizmo_refresh_from_matrix(wmGizmo *axis,
                                      const int axis_idx,
                                      const float mat[4][4],
                                      const float scale[3])
{
  const SculptCursorAxisType axis_type = sculpt_cursor_axis_type_get(axis_idx);
  const int aidx_norm = sculpt_cursor_axis_norm_index(axis_idx);

  switch (axis_idx) {
    case SCULPT_CURSOR_TRANS_X:
    case SCULPT_CURSOR_TRANS_Y:
    case SCULPT_CURSOR_TRANS_Z:
      copy_m4_m4(axis->matrix_basis, mat);
      break;
    case SCULPT_CURSOR_SCALE_X:
    case SCULPT_CURSOR_SCALE_Y:
    case SCULPT_CURSOR_SCALE_Z:
      copy_m4_m4(axis->matrix_basis, mat);
      if (scale && aidx_norm >= 0) {
        float start, end;
        gizmo_line_range(axis_type, &start, &end);
        RNA_float_set(axis->ptr, "length", (end - start) * scale[aidx_norm]);
      }
      break;
    case SCULPT_CURSOR_SCALE_XY:
    case SCULPT_CURSOR_SCALE_YZ:
    case SCULPT_CURSOR_SCALE_ZX:
      copy_m4_m4(axis->matrix_basis, mat);
      if (scale && aidx_norm >= 0) {
        RNA_float_set(axis->ptr,
                      "length",
                      SCULPT_CURSOR_SCALE_PLANE_SCALE * scale[aidx_norm == 2 ? 0 : aidx_norm + 1]);
      }
      break;
    case SCULPT_CURSOR_SCALE_C:
      WM_gizmo_set_matrix_location(axis, mat[3]);
      if (scale) {
        WM_gizmo_set_scale(axis, 0.2f * scale[0]);
      }
      break;
    case SCULPT_CURSOR_ROT_X:
      copy_m4_m4(axis->matrix_basis, mat);
      orthogonalize_m4(axis->matrix_basis, 0);
      break;
    case SCULPT_CURSOR_ROT_Y:
      copy_m4_m4(axis->matrix_basis, mat);
      orthogonalize_m4(axis->matrix_basis, 1);
      break;
    case SCULPT_CURSOR_ROT_Z:
      copy_m4_m4(axis->matrix_basis, mat);
      orthogonalize_m4(axis->matrix_basis, 2);
      break;
    case SCULPT_CURSOR_ROT_C:
    case SCULPT_CURSOR_TRANS_C:
      /* Screen-aligned widgets: only their location follows the cursor. */
      WM_gizmo_set_matrix_location(axis, mat[3]);
      break;
    default:
      break;
  }
}

static void sculpt_cursor_world_matrix_get(const Scene &scene,
                                           const Object &ob,
                                           float r_mat[4][4])
{
  copy_m4_m4(r_mat, sculpt_paint::cursor::world_matrix_get(scene, ob).ptr());
}

static void gizmo_get_idot(const RegionView3D *rv3d,
                           const float world_mat[4][4],
                           float r_idot[3])
{
  float view_vec[3], axis_vec[3];
  ED_view3d_global_to_vector(rv3d, world_mat[3], view_vec);
  for (int i = 0; i < 3; i++) {
    normalize_v3_v3(axis_vec, world_mat[i]);
    r_idot[i] = 1.0f - fabsf(dot_v3v3(view_vec, axis_vec));
  }
}

static bool gizmo_is_translate_visible(const float idot[3], const int axis_idx)
{
  const int axis_norm = sculpt_cursor_axis_norm_index(axis_idx);
  if (axis_norm < 0) {
    return true;
  }
  return idot[axis_norm] >= g_axis_range.min;
}

static void gizmo_get_axis_color(const int axis_idx,
                                 const float idot[3],
                                 float r_col[4],
                                 float r_col_hi[4])
{
  const float alpha = 0.6f;
  const float alpha_hi = 1.0f;
  float alpha_fac = 1.0f;

  if (axis_idx >= SCULPT_CURSOR_TRANS_X && axis_idx <= SCULPT_CURSOR_TRANS_C) {
    const int axis_norm = sculpt_cursor_axis_norm_index(axis_idx);
    if (axis_norm >= 0) {
      const float idot_axis = idot[axis_norm];
      alpha_fac = (idot_axis > g_axis_range.max) ?
                        1.0f :
                        (idot_axis < g_axis_range.min) ?
                        0.0f :
                        ((idot_axis - g_axis_range.min) / (g_axis_range.max - g_axis_range.min));
    }
  }

  switch (axis_idx) {
    case SCULPT_CURSOR_TRANS_X:
    case SCULPT_CURSOR_ROT_X:
    case SCULPT_CURSOR_SCALE_X:
    case SCULPT_CURSOR_SCALE_YZ:
      ui::theme::get_color_4fv(TH_AXIS_X, r_col);
      break;
    case SCULPT_CURSOR_TRANS_Y:
    case SCULPT_CURSOR_ROT_Y:
    case SCULPT_CURSOR_SCALE_Y:
    case SCULPT_CURSOR_SCALE_ZX:
      ui::theme::get_color_4fv(TH_AXIS_Y, r_col);
      break;
    case SCULPT_CURSOR_TRANS_Z:
    case SCULPT_CURSOR_ROT_Z:
    case SCULPT_CURSOR_SCALE_Z:
    case SCULPT_CURSOR_SCALE_XY:
      ui::theme::get_color_4fv(TH_AXIS_Z, r_col);
      break;
    case SCULPT_CURSOR_SCALE_C:
    case SCULPT_CURSOR_ROT_C:
    case SCULPT_CURSOR_TRANS_C:
      ui::theme::get_color_4fv(TH_GIZMO_VIEW_ALIGN, r_col);
      break;
    default:
      return;
  }

  copy_v4_v4(r_col_hi, r_col);
  r_col[3] = alpha * alpha_fac;
  r_col_hi[3] = alpha_hi * alpha_fac;
}

static void gizmo_world_matrix_get(const bContext *C,
                                   const Scene &scene,
                                   Object &ob,
                                   float r_mat[4][4],
                                   int *r_orient_index);

/**
 * Gizmo matrix while a Transform operator runs: the shared world pivot position, and the gizmo axes
 * captured on press turned by the pivot's rotation since then.
 */
static void sculpt_cursor_transform_pivot_world_matrix(const SculptCursorGizmoGroup &ggd,
                                                       const SculptSession &ss,
                                                       float r_mat[4][4])
{
  float rot_init_inv[4], rot_delta[4];
  invert_qt_qt_normalized(rot_init_inv, ggd.drag_pivot_rot_init);
  mul_qt_qtqt(rot_delta, ss.transform_pivot_rot_world, rot_init_inv);

  float delta_mat[3][3], rot[3][3];
  quat_to_mat3(delta_mat, rot_delta);
  mul_m3_m3m3(rot, delta_mat, ggd.drag_orient);

  unit_m4(r_mat);
  copy_m4_m3(r_mat, rot);
  copy_v3_v3(r_mat[3], ss.transform_pivot_pos_world);
}

static wmOperatorStatus sculpt_cursor_gizmo_modal(bContext *C,
                                                  wmGizmo *widget,
                                                  const wmEvent *event,
                                                  eWM_GizmoFlagTweak /*tweak_flag*/)
{
  if (ELEM(event->type, TIMER, INBETWEEN_MOUSEMOVE)) {
    return OPERATOR_RUNNING_MODAL;
  }

  wmGizmoGroup *gzgroup = widget->parent_gzgroup;
  if (!gzgroup) {
    return OPERATOR_RUNNING_MODAL;
  }

  SculptCursorGizmoGroup *ggd = static_cast<SculptCursorGizmoGroup *>(gzgroup->customdata);
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  SculptSession *ss = (ob && ob->runtime) ? ob->runtime->sculpt_session : nullptr;
  if (!ss || !scene || !sculpt_paint::cursor::is_enabled(*scene)) {
    return OPERATOR_RUNNING_MODAL;
  }

  /* The widget is bound to one of the standard Transform operators only in Deform mode. Detect
   * that from the operator type rather than the cached mode so the behavior is correct even if the
   * group has not re-bound yet. */
  bool is_transform = false;
  for (const wmGizmoOpElem &gzop : widget->op_data) {
    if (gzop.type && STRPREFIX(gzop.type->idname, "TRANSFORM_OT_")) {
      is_transform = true;
      break;
    }
  }

  float world_mat[4][4];
  const float *scale = nullptr;
  if (is_transform) {
    /* Follow the running Transform operator: it drives the shared world pivot position/rotation
     * and the pivot scale every step (see `sculpt_transform.cc`), so mirroring them keeps the
     * gizmo moving with the deformation instead of staying pinned at the cursor, giving the user
     * feedback on how far they dragged. The cursor itself is written once by the Transform
     * session's end/cancel handlers (see `sculpt_transform.cc`), not from this draw-time
     * callback. */
    sculpt_cursor_transform_pivot_world_matrix(*ggd, *ss, world_mat);
    scale = ss->pivot_scale;
  }
  else {
    gizmo_world_matrix_get(C, *scene, *ob, world_mat, nullptr);
  }
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = region ? static_cast<RegionView3D *>(region->regiondata) : nullptr;
  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    if (wmGizmo *gz = ggd->gizmos[i]) {
      gizmo_refresh_from_matrix(gz, i, world_mat, scale);
      if (rv3d && (i == SCULPT_CURSOR_ROT_C || i == SCULPT_CURSOR_TRANS_C)) {
        WM_gizmo_set_matrix_rotation_from_z_axis(gz, rv3d->viewinv[2]);
      }
    }
  }

  if (region) {
    ED_region_tag_redraw_editor_overlays(region);
  }

  return OPERATOR_RUNNING_MODAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gizmo Group Callbacks
 * \{ */

/** True while the active viewport tool is the sculpt 3D Cursor tool itself. */
static bool gizmo_tool_is_cursor(const bContext *C)
{
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  return tref && tref->idname && STREQ(tref->idname, "builtin.sculpt_cursor");
}

/** Bits of #SculptCursorAxisType the active tool shows. */
enum {
  SCULPT_CURSOR_SHOW_TRANSLATE = (1 << SCULPT_CURSOR_AXES_TRANSLATE),
  SCULPT_CURSOR_SHOW_ROTATE = (1 << SCULPT_CURSOR_AXES_ROTATE),
  SCULPT_CURSOR_SHOW_SCALE = (1 << SCULPT_CURSOR_AXES_SCALE),
  SCULPT_CURSOR_SHOW_ALL = SCULPT_CURSOR_SHOW_TRANSLATE | SCULPT_CURSOR_SHOW_ROTATE |
                           SCULPT_CURSOR_SHOW_SCALE,
};

/**
 * With the cursor enabled it replaces the Transform gizmo of the Move/Rotate/Scale/Transform
 * tools (see `transform_gizmo_3d.cc`). There it shows only that tool's handles and always deforms
 * the mesh, since that is what those tools are for.
 *
 * \return false when the active tool is not a transform tool.
 */
static bool gizmo_transform_tool_get(const bContext *C, int *r_show_flag)
{
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  const char *idname = tref ? tref->idname : nullptr;
  if (!idname) {
    return false;
  }
  if (STREQ(idname, "builtin.move")) {
    *r_show_flag = SCULPT_CURSOR_SHOW_TRANSLATE;
  }
  else if (STREQ(idname, "builtin.rotate")) {
    *r_show_flag = SCULPT_CURSOR_SHOW_ROTATE;
  }
  else if (STREQ(idname, "builtin.scale")) {
    *r_show_flag = SCULPT_CURSOR_SHOW_SCALE;
  }
  else if (STREQ(idname, "builtin.transform")) {
    *r_show_flag = SCULPT_CURSOR_SHOW_ALL;
  }
  else {
    return false;
  }
  return true;
}

/** Mode the handles act in: the stored cursor mode, or Deform inside a transform tool. */
static sculpt_paint::cursor::SculptCursorMode gizmo_effective_mode(const bContext *C,
                                                                   const Scene &scene)
{
  int show_flag;
  if (gizmo_transform_tool_get(C, &show_flag)) {
    return sculpt_paint::cursor::SculptCursorMode::Deform;
  }
  return sculpt_paint::cursor::mode_get(scene);
}

/**
 * Orientation of a transform tool's gizmo: the header's Transform Orientation for that tool, as the
 * regular Transform gizmo uses (see #calc_gizmo_stats). "Cursor" refers to the sculpt cursor here,
 * which is also what the cursor's own tool and the brushes use.
 *
 * \return false when the gizmo keeps the sculpt cursor's axes.
 */
static bool gizmo_tool_orientation_get(const bContext *C,
                                       Object &ob,
                                       float r_rot[3][3],
                                       int *r_orient_index)
{
  int show_flag;
  if (!gizmo_transform_tool_get(C, &show_flag)) {
    return false;
  }
  Scene *scene = CTX_data_scene(C);
  if (!scene) {
    return false;
  }

  int twtype = 0;
  SET_FLAG_FROM_TEST(
      twtype, show_flag & SCULPT_CURSOR_SHOW_TRANSLATE, V3D_GIZMO_SHOW_OBJECT_TRANSLATE);
  SET_FLAG_FROM_TEST(twtype, show_flag & SCULPT_CURSOR_SHOW_ROTATE, V3D_GIZMO_SHOW_OBJECT_ROTATE);
  SET_FLAG_FROM_TEST(twtype, show_flag & SCULPT_CURSOR_SHOW_SCALE, V3D_GIZMO_SHOW_OBJECT_SCALE);
  const int orient_index = BKE_scene_orientation_get_index_from_flag(scene, twtype);
  if (orient_index == V3D_ORIENT_CURSOR) {
    return false;
  }

  const ARegion *region = CTX_wm_region(C);
  const RegionView3D *rv3d = region ? static_cast<const RegionView3D *>(region->regiondata) :
                                      nullptr;
  ed::transform::calc_orientation_from_type_ex(*CTX_data_main(C),
                                               scene,
                                               CTX_data_view_layer(C),
                                               CTX_wm_view3d(C),
                                               rv3d,
                                               &ob,
                                               nullptr,
                                               orient_index,
                                               scene->toolsettings->transform_pivot_point,
                                               r_rot);
  /* Gimbal and sheared local axes are not orthogonal (see #calc_orientation_from_type_ex). */
  orthogonalize_m3(r_rot, 2);
  normalize_m3(r_rot);
  *r_orient_index = orient_index;
  return true;
}

/**
 * World matrix the gizmo is drawn with: always located at the sculpt cursor, oriented by the
 * active transform tool's orientation or else by the cursor itself.
 *
 * \param r_orient_index: The tool orientation used, or -1 for the cursor's axes. May be null.
 */
static void gizmo_world_matrix_get(const bContext *C,
                                   const Scene &scene,
                                   Object &ob,
                                   float r_mat[4][4],
                                   int *r_orient_index)
{
  sculpt_cursor_world_matrix_get(scene, ob, r_mat);

  float rot[3][3];
  int orient_index = -1;
  if (gizmo_tool_orientation_get(C, ob, rot, &orient_index)) {
    float location[3];
    copy_v3_v3(location, r_mat[3]);
    /* #copy_m4_m3 also resets the translation. */
    copy_m4_m3(r_mat, rot);
    copy_v3_v3(r_mat[3], location);
  }
  if (r_orient_index) {
    *r_orient_index = orient_index;
  }
}

static bool gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || !(ob->mode & OB_MODE_SCULPT)) {
    return false;
  }

  const Scene *scene = CTX_data_scene(C);
  if (!ob->runtime->sculpt_session || !scene) {
    return false;
  }

  /* The cursor drives other tools only while enabled, but its own tool keeps the (dimmed) gizmo
   * visible so a handle click can enable it. */
  if (!sculpt_paint::cursor::is_enabled(*scene) && !gizmo_tool_is_cursor(C)) {
    return false;
  }

  /* Hidden together with the "3D Cursor" viewport overlay toggle. */
  const View3D *v3d = CTX_wm_view3d(C);
  if (!v3d || (v3d->flag2 & V3D_HIDE_OVERLAYS) ||
      (v3d->overlay.flag & V3D_OVERLAY_HIDE_CURSOR))
  {
    return false;
  }

  return true;
}

static void gizmo_axis_color_set(wmGizmo *gz, const int axis_idx)
{
  float color[4], color_hi[4];
  float idot[3] = {1.0f, 1.0f, 1.0f};
  gizmo_get_axis_color(axis_idx, idot, color, color_hi);
  WM_gizmo_set_color(gz, color);
  WM_gizmo_set_color_highlight(gz, color_hi);
}

static void gizmo_setup_operator(wmGizmo *gz,
                                 const int axis_idx,
                                 wmOperatorType *ot,
                                 const int mode)
{
  if (ot == nullptr) {
    return;
  }

  bool constraint_axis[3];
  gizmo_get_axis_constraint(axis_idx, constraint_axis);

  PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot, nullptr);
  RNA_enum_set(ptr, "mode", mode);
  RNA_boolean_set_array(ptr, "constraint_axis", constraint_axis);
  RNA_boolean_set(ptr, "release_confirm", true);
}

/**
 * Deform mode: bind the axis to the standard Transform operator so the mesh is deformed around
 * the sculpt cursor (which #createTransSculpt uses as the transform pivot), instead of moving the
 * cursor itself.
 */
static void gizmo_setup_operator_transform(wmGizmo *gz, const int axis_idx)
{
  static const char *op_idnames[] = {
      "TRANSFORM_OT_translate",
      "TRANSFORM_OT_rotate",
      "TRANSFORM_OT_resize",
  };
  const int transform_mode = sculpt_cursor_axis_transform_mode(axis_idx);
  wmOperatorType *ot = WM_operatortype_find(op_idnames[transform_mode], true);
  if (ot == nullptr) {
    return;
  }

  bool constraint_axis[3];
  gizmo_get_axis_constraint(axis_idx, constraint_axis);

  PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot, nullptr);
  PropertyRNA *prop = RNA_struct_find_property(ptr, "constraint_axis");
  if (prop && (constraint_axis[0] || constraint_axis[1] || constraint_axis[2])) {
    RNA_property_boolean_set_array(ptr, prop, constraint_axis);
  }
  RNA_boolean_set(ptr, "release_confirm", true);
}

static void gizmo_dial_matrixbasis_calc(const ARegion *region,
                                        const float axis[3],
                                        const float center_global[3],
                                        const float mval_init[2],
                                        float r_mat_basis[4][4])
{
  plane_from_point_normal_v3(r_mat_basis[2], center_global, axis);
  copy_v3_v3(r_mat_basis[3], center_global);

  if (ED_view3d_win_to_3d_on_plane(region, r_mat_basis[2], mval_init, false, r_mat_basis[1])) {
    sub_v3_v3(r_mat_basis[1], center_global);
    normalize_v3(r_mat_basis[1]);
    cross_v3_v3v3(r_mat_basis[0], r_mat_basis[1], r_mat_basis[2]);
  }
  else {
    /* The plane and the mouse direction are parallel.
     * Calculate a matrix orthogonal to the axis. */
    ortho_basis_v3v3_v3(r_mat_basis[0], r_mat_basis[1], r_mat_basis[2]);
  }

  r_mat_basis[0][3] = 0.0f;
  r_mat_basis[1][3] = 0.0f;
  r_mat_basis[2][3] = 0.0f;
  r_mat_basis[3][3] = 1.0f;
}

/**
 * Mirrors #gizmo_3d_draw_invoke in `transform_gizmo_3d.cc`: show only the dragged handle
 * (translate keeps its arrows as visual reference), face the screen-aligned widgets to the view,
 * and align the rotation dials to the initial mouse position.
 */
static void gizmo_modal_draw_setup(const bContext *C,
                                   SculptCursorGizmoGroup *ggd,
                                   wmGizmo *gz,
                                   const int axis_idx,
                                   const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  const SculptCursorAxisType axis_type = sculpt_cursor_axis_type_get(axis_idx);

  /* Display only the active gizmo. */
  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    if (wmGizmo *other = ggd->gizmos[i]) {
      WM_gizmo_set_flag(other, WM_GIZMO_HIDDEN, other != gz);
    }
  }

  if (axis_type == SCULPT_CURSOR_AXES_TRANSLATE) {
    /* Arrows are used for visual reference, so keep all visible. */
    for (int i = SCULPT_CURSOR_TRANS_X; i <= SCULPT_CURSOR_TRANS_Z; i++) {
      if (wmGizmo *axis = ggd->gizmos[i]) {
        WM_gizmo_set_flag(axis, WM_GIZMO_HIDDEN, false);
      }
    }
  }

  if (axis_idx == SCULPT_CURSOR_ROT_C || axis_idx == SCULPT_CURSOR_TRANS_C) {
    WM_gizmo_set_matrix_rotation_from_z_axis(gz, rv3d->viewinv[2]);
  }

  if (axis_type == SCULPT_CURSOR_AXES_ROTATE && axis_idx != SCULPT_CURSOR_ROT_C) {
    const float mval[2] = {float(event->mval[0]), float(event->mval[1])};
    float mat[3][3];
    mul_m3_m4m4(mat, gz->matrix_basis, gz->matrix_offset);
    gizmo_dial_matrixbasis_calc(region, mat[2], gz->matrix_basis[3], mval, gz->matrix_offset);

    copy_m3_m4(mat, gz->matrix_basis);
    invert_m3(mat);
    mul_m4_m3m4(gz->matrix_offset, mat, gz->matrix_offset);
    zero_v3(gz->matrix_offset[3]);
  }
}

/**
 * Deform mode: feed the sculpt cursor's world orientation to the standard Transform operator before
 * it starts, so an axis constraint (and the rotation/resize planes) follows the cursor's own axes
 * instead of the scene's default orientation. Mirrors #WIDGETGROUP_gizmo_invoke_prepare in
 * `transform_gizmo_3d.cc`, but the orientation is the cursor matrix rather than the tool's.
 *
 * The Set-mode cursor operator transforms the cursor in its own local axes, so it is left alone.
 * Clicking any handle while the cursor is disabled enables it first.
 */
static void gizmo_invoke_prepare(const bContext *C,
                                 wmGizmoGroup *gzgroup,
                                 wmGizmo *gz,
                                 const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  if (!ob || !scene) {
    return;
  }

  /* Clicking any handle of the dimmed (disabled) gizmo enables the cursor first, then performs the
   * handle's action as usual. */
  if (!sculpt_paint::cursor::is_enabled(*scene)) {
    if (Sculpt *sculpt = scene->toolsettings ? scene->toolsettings->sculpt : nullptr) {
      sculpt->sculpt_cursor_flag |= SCULPT_CURSOR_ENABLED;
      WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    }
  }

  SculptCursorGizmoGroup *ggd = static_cast<SculptCursorGizmoGroup *>(gzgroup->customdata);
  int axis_idx = -1;
  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    if (ggd->gizmos[i] == gz) {
      axis_idx = i;
      break;
    }
  }
  if (axis_idx == -1) {
    return;
  }

  gizmo_modal_draw_setup(C, ggd, gz, axis_idx, event);

  wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
  if (!gzop || !gzop->type || !STRPREFIX(gzop->type->idname, "TRANSFORM_OT_")) {
    return;
  }

  SculptSession *ss = ob->runtime->sculpt_session;
  if (!ss) {
    return;
  }

  /* Capture the drag reference for #sculpt_cursor_transform_pivot_world_matrix. #createTransSculpt
   * seeds the pivot rotation from the cursor's world rotation. */
  float world_mat[4][4];
  int orient_index = -1;
  gizmo_world_matrix_get(C, *scene, *ob, world_mat, &orient_index);
  copy_m3_m4(ggd->drag_orient, world_mat);
  float cursor_rot[3][3];
  copy_m3_m4(cursor_rot, sculpt_paint::cursor::world_matrix_get(*scene, *ob).ptr());
  mat3_normalized_to_quat(ggd->drag_pivot_rot_init, cursor_rot);

  /* The screen-aligned ring always uses the view orientation. */
  if (axis_idx == SCULPT_CURSOR_ROT_C) {
    RNA_enum_set(&gzop->ptr, "orient_type", V3D_ORIENT_VIEW);
    if (PropertyRNA *prop = RNA_struct_find_property(&gzop->ptr, "orient_matrix")) {
      RNA_property_unset(&gzop->ptr, prop);
    }
    return;
  }

  /* Pass the exact axes the gizmo is drawn with. The type only labels the matrix (redo panel);
   * the cursor's own axes are reported as the "Cursor" orientation. */
  float orient_matrix[3][3];
  copy_m3_m4(orient_matrix, world_mat);
  RNA_float_set_array(&gzop->ptr, "orient_matrix", &orient_matrix[0][0]);
  RNA_enum_set(
      &gzop->ptr, "orient_matrix_type", orient_index >= 0 ? orient_index : V3D_ORIENT_CURSOR);

  /* Leave `orient_type` unset so `transform_generics.cc` treats the matrix as the orientation,
   * matching how the Transform gizmo stores its custom orientation. */
  if (PropertyRNA *prop_orient_type = RNA_struct_find_property(&gzop->ptr, "orient_type")) {
    RNA_property_unset(&gzop->ptr, prop_orient_type);
  }
}

/** Rebind every axis gizmo to the operator set matching \a mode. */
static void gizmo_group_bind_operators(SculptCursorGizmoGroup *ggd,
                                       const sculpt_paint::cursor::SculptCursorMode mode)
{
  wmOperatorType *ot_cursor = WM_operatortype_find("SCULPT_OT_cursor_transform", true);
  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    wmGizmo *gz = ggd->gizmos[i];
    if (gz == nullptr) {
      continue;
    }
    if (mode == sculpt_paint::cursor::SculptCursorMode::Deform) {
      gizmo_setup_operator_transform(gz, i);
    }
    else {
      gizmo_setup_operator(gz, i, ot_cursor, sculpt_cursor_axis_set_mode(i));
    }
  }
  ggd->bound_mode = int(mode);
}

static void gizmogroup_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  SculptCursorGizmoGroup *ggd = MEM_new<SculptCursorGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  ggd->bound_mode = -1;
  ggd->show_flag = SCULPT_CURSOR_SHOW_ALL;
  unit_m3(ggd->drag_orient);
  unit_qt(ggd->drag_pivot_rot_init);
  unit_m3(ggd->prev_viewinv);

  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_3d", true);
  const wmGizmoType *gzt_dial = WM_gizmotype_find("GIZMO_GT_dial_3d", true);
  const wmGizmoType *gzt_prim = WM_gizmotype_find("GIZMO_GT_primitive_3d", true);

  /* Order matches #transform_gizmo_3d.cc for correct depth sorting. */
  ggd->gizmos[SCULPT_CURSOR_SCALE_C] = WM_gizmo_new_ptr(gzt_prim, gzgroup, nullptr);

  for (int i = SCULPT_CURSOR_SCALE_X; i <= SCULPT_CURSOR_SCALE_ZX; i++) {
    ggd->gizmos[i] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, nullptr);
  }

  for (int i = SCULPT_CURSOR_ROT_X; i <= SCULPT_CURSOR_ROT_Z; i++) {
    ggd->gizmos[i] = WM_gizmo_new_ptr(gzt_dial, gzgroup, nullptr);
  }
  ggd->gizmos[SCULPT_CURSOR_ROT_C] = WM_gizmo_new_ptr(gzt_dial, gzgroup, nullptr);

  ggd->gizmos[SCULPT_CURSOR_TRANS_C] = WM_gizmo_new_ptr(gzt_prim, gzgroup, nullptr);
  for (int i = SCULPT_CURSOR_TRANS_X; i <= SCULPT_CURSOR_TRANS_Z; i++) {
    ggd->gizmos[i] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, nullptr);
  }

  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    wmGizmo *gz = ggd->gizmos[i];
    if (!gz) {
      continue;
    }
    gizmo_setup_draw(gz, i);
    gizmo_axis_color_set(gz, i);
    WM_gizmo_set_fn_custom_modal(gz, sculpt_cursor_gizmo_modal);
  }

  sculpt_paint::cursor::SculptCursorMode mode = sculpt_paint::cursor::SculptCursorMode::Set;
  if (const Scene *scene = C ? CTX_data_scene(C) : nullptr) {
    mode = gizmo_effective_mode(C, *scene);
  }
  gizmo_group_bind_operators(ggd, mode);
}

static void gizmogroup_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  SculptCursorGizmoGroup *ggd = static_cast<SculptCursorGizmoGroup *>(gzgroup->customdata);

  const auto hide_all = [&]() {
    for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
      if (wmGizmo *gz = ggd->gizmos[i]) {
        WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
      }
    }
  };

  Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  if (!ob || !ob->runtime->sculpt_session || !scene) {
    hide_all();
    return;
  }

  SculptSession *ss = ob->runtime->sculpt_session;

  /* Rebind the axis operators when the tool switched between Set and Deform. The mode is honored
   * in every tool so the viewport buttons work regardless of the active tool. */
  const sculpt_paint::cursor::SculptCursorMode mode = gizmo_effective_mode(C, *scene);
  if (ggd->bound_mode != int(mode)) {
    gizmo_group_bind_operators(ggd, mode);
  }
  int show_flag = SCULPT_CURSOR_SHOW_ALL;
  gizmo_transform_tool_get(C, &show_flag);
  ggd->show_flag = show_flag;

  float world_mat[4][4];
  const float *scale = nullptr;
  /* While a Transform operator is running, its `filter_cache` exists and the shared world pivot is
   * driven every step; mirror it so the gizmo follows the deformation. Otherwise the gizmo sits at
   * the placed cursor. */
  RegionView3D *rv3d = nullptr;
  if (ARegion *region = CTX_wm_region(C)) {
    rv3d = static_cast<RegionView3D *>(region->regiondata);
  }
  if (ggd->bound_mode == int(sculpt_paint::cursor::SculptCursorMode::Deform) &&
      ss->filter_cache != nullptr)
  {
    sculpt_cursor_transform_pivot_world_matrix(*ggd, *ss, world_mat);
    scale = ss->pivot_scale;
  }
  else {
    gizmo_world_matrix_get(C, *scene, *ob, world_mat, nullptr);
    /* Keep the drag reference current while idle, so a Transform started from the keyboard (not
     * through a handle, see #gizmo_invoke_prepare) also turns the gizmo correctly. */
    copy_m3_m4(ggd->drag_orient, world_mat);
    float cursor_rot[3][3];
    copy_m3_m4(cursor_rot, sculpt_paint::cursor::world_matrix_get(*scene, *ob).ptr());
    mat3_normalized_to_quat(ggd->drag_pivot_rot_init, cursor_rot);
  }
  if (rv3d) {
    copy_m3_m4(ggd->prev_viewinv, rv3d->viewinv);
  }

  float idot[3] = {1.0f, 1.0f, 1.0f};
  const bool is_modal = WM_gizmo_group_is_modal(gzgroup);
  if (!is_modal && rv3d) {
    gizmo_get_idot(rv3d, world_mat, idot);
  }
  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    wmGizmo *gz = ggd->gizmos[i];
    if (!gz) {
      continue;
    }

    bool hide = false;
    if (!is_modal && i >= SCULPT_CURSOR_TRANS_X && i <= SCULPT_CURSOR_TRANS_Z) {
      hide = !gizmo_is_translate_visible(idot, i);
    }
    /* Center uniform scale is hidden when translate handles are shown (see #transform_gizmo_3d). */
    if (i == SCULPT_CURSOR_SCALE_C) {
      hide = true;
    }
    /* Scale handles deform the mesh around the cursor, so they are Deform-only. */
    if (ggd->bound_mode != int(sculpt_paint::cursor::SculptCursorMode::Deform) &&
        i >= SCULPT_CURSOR_SCALE_X && i <= SCULPT_CURSOR_SCALE_ZX)
    {
      hide = true;
    }
    /* Inside a transform tool only that tool's handles are shown, like its own gizmo. */
    if (!(show_flag & (1 << sculpt_cursor_axis_type_get(i)))) {
      hide = true;
    }

    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, hide);
    if (!hide) {
      gizmo_refresh_from_matrix(gz, i, world_mat, scale);
      /* Screen-aligned widgets face the viewport (see #gizmo_3d_draw_invoke). */
      if (rv3d && (i == SCULPT_CURSOR_ROT_C || i == SCULPT_CURSOR_TRANS_C)) {
        WM_gizmo_set_matrix_rotation_from_z_axis(gz, rv3d->viewinv[2]);
      }
    }
  }
}

static void gizmogroup_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  SculptCursorGizmoGroup *ggd = static_cast<SculptCursorGizmoGroup *>(gzgroup->customdata);

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  if (!ob || !ob->runtime->sculpt_session || !scene) {
    return;
  }

  const bool enabled = sculpt_paint::cursor::is_enabled(*scene);

  /* Switching tools does not necessarily refresh this group, which is not owned by any tool; when
   * the handles or their operators no longer match the active tool, request a refresh. */
  if (!WM_gizmo_group_is_modal(gzgroup)) {
    int show_flag = SCULPT_CURSOR_SHOW_ALL;
    gizmo_transform_tool_get(C, &show_flag);
    if (show_flag != ggd->show_flag || ggd->bound_mode != int(gizmo_effective_mode(C, *scene))) {
      WM_gizmomap_tag_refresh(gzgroup->parent_gzmap);
    }
  }

  float world_mat[4][4];
  int orient_index = -1;
  gizmo_world_matrix_get(C, *scene, *ob, world_mat, &orient_index);

  /* Orbiting does not refresh the group, but the View orientation follows the view, so re-apply
   * it the way #WIDGETGROUP_gizmo_draw_prepare does. */
  if (!WM_gizmo_group_is_modal(gzgroup) && orient_index == V3D_ORIENT_VIEW) {
    float viewinv_m3[3][3];
    copy_m3_m4(viewinv_m3, rv3d->viewinv);
    if (!equals_m3m3(viewinv_m3, ggd->prev_viewinv)) {
      gizmogroup_refresh(C, gzgroup);
    }
  }

  float idot[3];
  gizmo_get_idot(rv3d, world_mat, idot);

  const bool is_modal = WM_gizmo_group_is_modal(gzgroup);

  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    wmGizmo *gz = ggd->gizmos[i];
    if (!gz) {
      continue;
    }
    if (gz->flag & WM_GIZMO_HIDDEN) {
      continue;
    }

    /* Orbiting the view does not trigger a refresh, so the screen-aligned widgets are re-aligned
     * on every draw, as in #WIDGETGROUP_gizmo_draw_prepare. While dragging, the modal callback owns
     * their matrices. */
    if (!is_modal && ELEM(i, SCULPT_CURSOR_ROT_C, SCULPT_CURSOR_TRANS_C)) {
      WM_gizmo_set_matrix_location(gz, world_mat[3]);
      WM_gizmo_set_matrix_rotation_from_z_axis(gz, rv3d->viewinv[2]);
    }

    float color[4], color_hi[4];
    gizmo_get_axis_color(i, idot, color, color_hi);
    if (!enabled) {
      /* Dim the disabled cursor, but keep the hover highlight at full strength so the widget
       * reads as interactive and a handle click can enable it. */
      color[3] *= 0.25f;
    }
    WM_gizmo_set_color(gz, color);
    WM_gizmo_set_color_highlight(gz, color_hi);
  }
}

/** \} */

/* Refresh (and so re-bind the operators) when the cursor's ToolSettings change. */
static void gizmogroup_message_subscribe(const bContext *C,
                                         wmGizmoGroup *gzgroup,
                                         wmMsgBus *mbus)
{
  ARegion *region = CTX_wm_region(C);

  wmMsgSubscribeValue msg_sub_value_gz_tag_refresh{};
  msg_sub_value_gz_tag_refresh.owner = region;
  msg_sub_value_gz_tag_refresh.user_data = gzgroup->parent_gzmap;
  msg_sub_value_gz_tag_refresh.notify = WM_gizmo_do_msg_notify_tag_refresh;

  WM_msg_subscribe_rna_anon_prop(mbus, Sculpt, use_sculpt_cursor, &msg_sub_value_gz_tag_refresh);
  WM_msg_subscribe_rna_anon_prop(
      mbus, Sculpt, sculpt_cursor_gizmo, &msg_sub_value_gz_tag_refresh);
  /* Transform tools draw the gizmo in the header's Transform Orientation. */
  WM_msg_subscribe_rna_anon_type(mbus, TransformOrientationSlot, &msg_sub_value_gz_tag_refresh);
  WM_msg_subscribe_rna_anon_prop(mbus, Sculpt, sculpt_cursor_mode, &msg_sub_value_gz_tag_refresh);
  WM_msg_subscribe_rna_anon_prop(mbus, Sculpt, pin_sculpt_cursor, &msg_sub_value_gz_tag_refresh);
  WM_msg_subscribe_rna_anon_prop(
      mbus, Sculpt, use_shared_sculpt_cursor, &msg_sub_value_gz_tag_refresh);
  WM_msg_subscribe_rna_anon_prop(
      mbus, Sculpt, use_sculpt_cursor_proportional, &msg_sub_value_gz_tag_refresh);
  WM_msg_subscribe_rna_anon_prop(
      mbus, Sculpt, use_sculpt_cursor_projected, &msg_sub_value_gz_tag_refresh);
}

void VIEW3D_GGT_sculpt_cursor(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Sculpt Cursor Gizmo";
  gzgt->idname = "VIEW3D_GGT_sculpt_cursor";

  gzgt->flag = WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP | WM_GIZMOGROUPTYPE_3D |
               WM_GIZMOGROUPTYPE_DELAY_REFRESH_FOR_TWEAK;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = gizmo_poll;
  gzgt->setup = gizmogroup_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = gizmogroup_refresh;
  gzgt->draw_prepare = gizmogroup_draw_prepare;
  gzgt->invoke_prepare = gizmo_invoke_prepare;
  gzgt->message_subscribe = gizmogroup_message_subscribe;
}

/* -------------------------------------------------------------------- */
/** \name Sculpt Cursor Viewport Buttons
 *
 * The Set/Deform and Pin buttons are plain `GIZMO_GT_button_2d` widgets, which are designed for
 * screen-space (non-3D) gizmo groups: their hit-test compares the region mouse position directly
 * against `matrix_basis[3]` (see #gizmo_button2d_test_select) and their scale is a plain UI-pixel
 * value (the #WM_GIZMOGROUPTYPE_SCALE path of #wm_gizmo_calculate_scale). Putting them in the 3D
 * cursor group would both mis-size them and break clicking, so they live in their own group whose
 * position is the cursor's projected screen coordinate.
 * \{ */

struct SculptCursorButtonsGizmoGroup {
  /** Two variants per slot (off/on). `GIZMO_GT_button_2d` caches its icon on the first draw, so
   * switching the icon is done by toggling visibility between the two variants. */
  wmGizmo *mode_button[2];
  wmGizmo *pin_button[2];
  wmGizmo *global_button[2];
  /** Decorative outline layers drawn on top of the buttons (not selectable). */
  wmGizmo *mode_outline;
  wmGizmo *pin_outline;
  wmGizmo *global_outline;
};

static bool sculpt_cursor_buttons_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  const Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  /* Like the gizmo handles, the buttons are usable from any tool while the cursor is on. */
  if (!ob || !ob->runtime->sculpt_session || !scene || !sculpt_paint::cursor::is_enabled(*scene))
  {
    return false;
  }

  /* Hidden together with the "3D Cursor" viewport overlay toggle. */
  const View3D *v3d = CTX_wm_view3d(C);
  if (!v3d || (v3d->flag2 & V3D_HIDE_OVERLAYS) ||
      (v3d->overlay.flag & V3D_OVERLAY_HIDE_CURSOR))
  {
    return false;
  }

  return true;
}

static wmGizmo *sculpt_cursor_screen_button_new(const wmGizmoType *gzt,
                                                wmGizmoGroup *gzgroup,
                                                wmOperatorType *ot,
                                                const char *data_path,
                                                const int icon)
{
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  /* Screen-space group: `scale_final` is `scale_basis * UI_SCALE_FAC`, i.e. UI pixels. */
  gz->scale_basis = 14.0f;
  gz->flag |= WM_GIZMO_DRAW_OFFSET_SCALE;
  RNA_enum_set(gz->ptr, "icon", icon);
  RNA_enum_set(gz->ptr, "draw_options",
               ED_GIZMO_BUTTON_SHOW_OUTLINE | ED_GIZMO_BUTTON_SHOW_BACKDROP);
  RNA_boolean_set(gz->ptr, "show_drag", false);
  /* Each button draws its own round backdrop; use the panel theme colors so it reads like a
   * regular UI panel chip. */
  ui::theme::get_color_4fv(TH_PANEL_BACK, gz->color);
  ui::theme::get_color_4fv(TH_PANEL_HEADER, gz->color_hi);

  if (ot != nullptr) {
    /* The buttons drive the ToolSettings directly through `wm.context_toggle` /
     * `wm.context_cycle_enum`, so their state and the top-bar settings are the same value. */
    PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot, nullptr);
    RNA_string_set(ptr, "data_path", data_path);
    /* `wm.context_cycle_enum` only toggles a two-item enum when it wraps. */
    if (RNA_struct_find_property(ptr, "wrap")) {
      RNA_boolean_set(ptr, "wrap", true);
    }
  }
  return gz;
}

/**
 * Decorative circle outline drawn over a button. `GIZMO_GT_button_2d` draws its own outline in the
 * same color as the fill (so it is invisible); this separate, non-selectable layer adds a readable
 * border in a contrasting theme color.
 */
static wmGizmo *sculpt_cursor_screen_outline_new(const wmGizmoType *gzt, wmGizmoGroup *gzgroup)
{
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  gz->scale_basis = 14.0f;
  gz->flag |= WM_GIZMO_DRAW_OFFSET_SCALE | WM_GIZMO_HIDDEN_SELECT;
  gz->line_width = 1.5f;
  RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_BACKDROP);
  RNA_float_set(gz->ptr, "backdrop_fill_alpha", 0.0f);
  ui::theme::get_color_4fv(TH_PANEL_OUTLINE, gz->color);
  copy_v4_v4(gz->color_hi, gz->color);
  return gz;
}

static void sculpt_cursor_buttons_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  SculptCursorButtonsGizmoGroup *ggd = MEM_new<SculptCursorButtonsGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  for (int i = 0; i < 2; i++) {
    ggd->mode_button[i] = nullptr;
    ggd->pin_button[i] = nullptr;
    ggd->global_button[i] = nullptr;
  }
  ggd->mode_outline = nullptr;
  ggd->pin_outline = nullptr;
  ggd->global_outline = nullptr;

  const wmGizmoType *gzt_button = WM_gizmotype_find("GIZMO_GT_button_2d", true);
  wmOperatorType *ot_toggle = WM_operatortype_find("WM_OT_context_toggle", true);
  wmOperatorType *ot_cycle = WM_operatortype_find("WM_OT_context_cycle_enum", true);
  if (gzt_button == nullptr || ot_toggle == nullptr || ot_cycle == nullptr) {
    return;
  }

  const char *mode_path = "tool_settings.sculpt.sculpt_cursor_mode";
  const char *pin_path = "tool_settings.sculpt.pin_sculpt_cursor";
  const char *shared_path = "tool_settings.sculpt.use_shared_sculpt_cursor";

  /* Two icon variants per slot; the active one is chosen in #sculpt_cursor_buttons_draw_prepare. */
  ggd->mode_button[0] = sculpt_cursor_screen_button_new(
      gzt_button, gzgroup, ot_cycle, mode_path, ICON_PIVOT_CURSOR);
  ggd->mode_button[1] = sculpt_cursor_screen_button_new(
      gzt_button, gzgroup, ot_cycle, mode_path, ICON_STICKY_UVS_DISABLE);
  ggd->pin_button[0] = sculpt_cursor_screen_button_new(
      gzt_button, gzgroup, ot_toggle, pin_path, ICON_UNPINNED);
  ggd->pin_button[1] = sculpt_cursor_screen_button_new(
      gzt_button, gzgroup, ot_toggle, pin_path, ICON_PINNED);
  ggd->global_button[0] = sculpt_cursor_screen_button_new(
      gzt_button, gzgroup, ot_toggle, shared_path, ICON_GHOST_DISABLED);
  ggd->global_button[1] = sculpt_cursor_screen_button_new(
      gzt_button, gzgroup, ot_toggle, shared_path, ICON_GHOST_ENABLED);

  /* Created last so they are drawn on top of the buttons. */
  ggd->mode_outline = sculpt_cursor_screen_outline_new(gzt_button, gzgroup);
  ggd->pin_outline = sculpt_cursor_screen_outline_new(gzt_button, gzgroup);
  ggd->global_outline = sculpt_cursor_screen_outline_new(gzt_button, gzgroup);
}

/** Style a button as active (the theme's toggle-button color) or inactive (panel color). */
static void sculpt_cursor_button_set_active(wmGizmo *gz, const bool active)
{
  if (active) {
    const bTheme *btheme = ui::theme::theme_get();
    if (btheme) {
      const uiWidgetColors &wcol = btheme->tui.wcol_toggle;
      rgba_uchar_to_float(gz->color, wcol.inner_sel);
      rgba_uchar_to_float(gz->color_hi, wcol.inner_sel);
      return;
    }
  }
  float color[4];
  ui::theme::get_color_4fv(TH_PANEL_BACK, color);
  copy_v4_v4(gz->color, color);
  ui::theme::get_color_4fv(TH_PANEL_HEADER, color);
  copy_v4_v4(gz->color_hi, color);
}

static void sculpt_cursor_buttons_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  SculptCursorButtonsGizmoGroup *ggd = static_cast<SculptCursorButtonsGizmoGroup *>(
      gzgroup->customdata);

  wmGizmo *slots[3][2] = {
      {ggd->mode_button[0], ggd->mode_button[1]},
      {ggd->pin_button[0], ggd->pin_button[1]},
      {ggd->global_button[0], ggd->global_button[1]},
  };
  wmGizmo *outlines[3] = {ggd->mode_outline, ggd->pin_outline, ggd->global_outline};

  const auto hide = [&]() {
    for (int i = 0; i < 3; i++) {
      for (int v = 0; v < 2; v++) {
        if (slots[i][v]) {
          WM_gizmo_set_flag(slots[i][v], WM_GIZMO_HIDDEN, true);
        }
      }
      if (outlines[i]) {
        WM_gizmo_set_flag(outlines[i], WM_GIZMO_HIDDEN, true);
      }
    }
  };

  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  if (!region || !ob || !ob->runtime->sculpt_session || !scene) {
    hide();
    return;
  }

  if (!sculpt_paint::cursor::is_enabled(*scene)) {
    hide();
    return;
  }

  float world_mat[4][4];
  sculpt_cursor_world_matrix_get(*scene, *ob, world_mat);

  /* While a cursor handle is dragged the buttons fade out so the result stays visible. In Deform
   * mode the cursor itself is only written when the Transform session ends, so follow the live
   * Transform pivot the handles are drawn at (see #sculpt_cursor_gizmo_modal) instead. */
  bool is_dragging = false;
  if (const wmGizmo *modal_gz = region->runtime->gizmo_map ?
                                    WM_gizmomap_get_modal(region->runtime->gizmo_map) :
                                    nullptr)
  {
    if (modal_gz->parent_gzgroup &&
        STREQ(modal_gz->parent_gzgroup->type->idname, "VIEW3D_GGT_sculpt_cursor"))
    {
      is_dragging = true;
      const wmGizmoOpElem *gzop = WM_gizmo_operator_get(const_cast<wmGizmo *>(modal_gz), 0);
      if (gzop && gzop->type && STRPREFIX(gzop->type->idname, "TRANSFORM_OT_")) {
        copy_v3_v3(world_mat[3], ob->runtime->sculpt_session->transform_pivot_pos_world);
      }
    }
  }
  const float drag_alpha = 0.25f;

  float co[2];
  if (ED_view3d_project_float_global(region, world_mat[3], co, V3D_PROJ_TEST_CLIP_NEAR) !=
      V3D_PROJ_RET_OK)
  {
    hide();
    return;
  }

  const sculpt_paint::cursor::SculptCursorMode mode = sculpt_paint::cursor::mode_get(*scene);
  const bool pin = sculpt_paint::cursor::pin_get(*scene);
  const bool global = sculpt_paint::cursor::is_shared(*scene);

  /* Which variant (off=0 / on=1) to show per slot, and whether that slot reads as "active". */
  const int active_variant[3] = {
      mode == sculpt_paint::cursor::SculptCursorMode::Deform ? 1 : 0,
      pin ? 1 : 0,
      global ? 1 : 0,
  };
  const bool slot_on[3] = {active_variant[0] == 1, pin, global};

  /* A single row of buttons above the gizmo: [mode] [pin] [shared]. The cursor gizmo handles
   * extend roughly `U.gizmo_size` UI pixels from the center, so keep the row above them. */
  const float spacing = 34.0f * UI_SCALE_FAC;
  const float y_off = (max_ff(float(U.gizmo_size), 1.0f) * 1.6f + 18.0f) * UI_SCALE_FAC;
  for (int i = 0; i < 3; i++) {
    const float x = co[0] + (float(i) - 1.0f) * spacing;
    const float y = co[1] + y_off;
    for (int v = 0; v < 2; v++) {
      wmGizmo *gz = slots[i][v];
      if (!gz) {
        continue;
      }
      gz->matrix_basis[3][0] = x;
      gz->matrix_basis[3][1] = y;
      gz->matrix_basis[3][2] = 0.0f;
      const bool visible = (v == active_variant[i]);
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, !visible);
      if (visible) {
        sculpt_cursor_button_set_active(gz, slot_on[i]);
        if (is_dragging) {
          gz->color[3] *= drag_alpha;
          gz->color_hi[3] *= drag_alpha;
        }
      }
    }
    if (outlines[i]) {
      outlines[i]->matrix_basis[3][0] = x;
      outlines[i]->matrix_basis[3][1] = y;
      outlines[i]->matrix_basis[3][2] = 0.0f;
      ui::theme::get_color_4fv(TH_PANEL_OUTLINE, outlines[i]->color);
      if (is_dragging) {
        outlines[i]->color[3] *= drag_alpha;
      }
      copy_v4_v4(outlines[i]->color_hi, outlines[i]->color);
      WM_gizmo_set_flag(outlines[i], WM_GIZMO_HIDDEN, false);
    }
  }
}

/** \} */

void VIEW3D_GGT_sculpt_cursor_buttons(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Sculpt Cursor Buttons";
  gzgt->idname = "VIEW3D_GGT_sculpt_cursor_buttons";

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_SCALE |
               WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = sculpt_cursor_buttons_poll;
  gzgt->setup = sculpt_cursor_buttons_setup;
  gzgt->draw_prepare = sculpt_cursor_buttons_draw_prepare;
}

}  // namespace blender::ed::view3d
