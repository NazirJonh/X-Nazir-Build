/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 * Sculpt Cursor Gizmo - Interactive transform widget for sculpt cursor
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_constants.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_screen.hh"

#include "DNA_object_types.h"

#include "DNA_view3d_types.h"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"

namespace blender::ed::view3d {

/** Scale of the two-axis planes (inside rotation dials). */
#define SCULPT_CURSOR_SCALE_PLANE_SCALE 0.5f
/** Scale box arrow range (inside #DIAL_WIDTH rotation rings). */
#define SCULPT_CURSOR_SCALE_START 0.15f
#define SCULPT_CURSOR_SCALE_END 0.55f

/* Axis indices matching #transform_gizmo_3d.cc layout. */
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
  SCULPT_CURSOR_TRANS_X,
  SCULPT_CURSOR_TRANS_Y,
  SCULPT_CURSOR_TRANS_Z,
  SCULPT_CURSOR_AXIS_LEN,
};

enum SculptCursorAxisType {
  SCULPT_CURSOR_AXES_TRANSLATE = 0,
  SCULPT_CURSOR_AXES_ROTATE,
  SCULPT_CURSOR_AXES_SCALE,
};

struct SculptCursorGizmoGroup {
  wmGizmo *gizmos[SCULPT_CURSOR_AXIS_LEN];
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
  if (axis_idx >= SCULPT_CURSOR_TRANS_X && axis_idx <= SCULPT_CURSOR_TRANS_Z) {
    return SCULPT_CURSOR_AXES_TRANSLATE;
  }
  if (axis_idx >= SCULPT_CURSOR_ROT_X && axis_idx <= SCULPT_CURSOR_ROT_Z) {
    return SCULPT_CURSOR_AXES_ROTATE;
  }
  if (axis_idx >= SCULPT_CURSOR_SCALE_X && axis_idx <= SCULPT_CURSOR_SCALE_ZX) {
    return SCULPT_CURSOR_AXES_SCALE;
  }
  if (axis_idx == SCULPT_CURSOR_SCALE_C) {
    return SCULPT_CURSOR_AXES_SCALE;
  }
  return SCULPT_CURSOR_AXES_TRANSLATE;
}

static void gizmo_line_range(const SculptCursorAxisType axis_type, float *r_start, float *r_end)
{
  float start = 0.2f;
  float end = 1.0f;

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
      start = SCULPT_CURSOR_SCALE_START;
      end = SCULPT_CURSOR_SCALE_END;
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
  ARRAY_SET_ITEMS(r_axis, 0, 0, 0);

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
      RNA_enum_set(axis->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_NOP);
      RNA_float_set(axis->ptr, "incremental_angle", 0.0f);
      WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_VALUE, true);
      WM_gizmo_set_line_width(axis, 3.0f);
      /* Deprioritize rotation dials vs scale handles when hit regions overlap. */
      axis->select_bias = -2.0f;
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
    default:
      break;
  }
}

static void sculpt_cursor_world_matrix_get(const Object &ob,
                                           const SculptSession &ss,
                                           float r_mat[4][4])
{
  float local_rot[3][3];
  quat_to_mat3(local_rot, ss.sculpt_cursor_rot);

  float ob_rot[3][3];
  copy_m3_m4(ob_rot, ob.object_to_world().ptr());
  normalize_m3(ob_rot);

  float world_rot[3][3];
  mul_m3_m3m3(world_rot, ob_rot, local_rot);
  normalize_m3(world_rot);

  unit_m4(r_mat);
  copy_m4_m3(r_mat, world_rot);
  copy_v3_v3(r_mat[3], ss.sculpt_cursor_pos);
  mul_m4_v3(ob.object_to_world().ptr(), r_mat[3]);
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

  if (axis_idx >= SCULPT_CURSOR_TRANS_X && axis_idx <= SCULPT_CURSOR_TRANS_Z) {
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
      ui::theme::get_color_4fv(TH_GIZMO_VIEW_ALIGN, r_col);
      break;
    default:
      return;
  }

  copy_v4_v4(r_col_hi, r_col);
  r_col[3] = alpha * alpha_fac;
  r_col_hi[3] = alpha_hi * alpha_fac;
}

/**
 * Keep gizmo widgets aligned with the sculpt cursor while dragging.
 */
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
  SculptSession *ss = ob ? ob->runtime->sculpt_session : nullptr;
  if (!ss || !ss->sculpt_cursor_initialized) {
    return OPERATOR_RUNNING_MODAL;
  }

  float world_mat[4][4];
  sculpt_cursor_world_matrix_get(*ob, *ss, world_mat);

  const float *scale = ss->sculpt_cursor_scale;
  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    if (wmGizmo *gz = ggd->gizmos[i]) {
      gizmo_refresh_from_matrix(gz, i, world_mat, scale);
    }
  }

  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw_editor_overlays(region);
  }

  return OPERATOR_RUNNING_MODAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gizmo Group Callbacks
 * \{ */

static bool gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  ScrArea *area = CTX_wm_area(C);
  const bToolRef *tref = area ? area->runtime.tool : nullptr;
  if (!tref || !STREQ(tref->idname, "builtin.sculpt_cursor")) {
    return false;
  }

  Object *ob = CTX_data_active_object(C);
  if (!ob || !(ob->mode & OB_MODE_SCULPT)) {
    return false;
  }

  SculptSession *ss = ob->runtime->sculpt_session;
  if (!ss || !ss->sculpt_cursor_initialized) {
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
  bool constraint_axis[3];
  gizmo_get_axis_constraint(axis_idx, constraint_axis);

  PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot, nullptr);
  RNA_enum_set(ptr, "mode", mode);
  RNA_boolean_set_array(ptr, "constraint_axis", constraint_axis);
  RNA_boolean_set(ptr, "release_confirm", true);
}

static void gizmogroup_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  SculptCursorGizmoGroup *ggd = MEM_new<SculptCursorGizmoGroup>(__func__);
  gzgroup->customdata = ggd;

  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_3d", true);
  const wmGizmoType *gzt_dial = WM_gizmotype_find("GIZMO_GT_dial_3d", true);
  const wmGizmoType *gzt_prim = WM_gizmotype_find("GIZMO_GT_primitive_3d", true);

  wmOperatorType *ot_transform = WM_operatortype_find("SCULPT_OT_cursor_transform", true);

  /* Order matches #transform_gizmo_3d.cc for correct depth sorting. */
  ggd->gizmos[SCULPT_CURSOR_SCALE_C] = WM_gizmo_new_ptr(gzt_prim, gzgroup, nullptr);

  for (int i = SCULPT_CURSOR_SCALE_X; i <= SCULPT_CURSOR_SCALE_ZX; i++) {
    ggd->gizmos[i] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, nullptr);
  }

  for (int i = SCULPT_CURSOR_ROT_X; i <= SCULPT_CURSOR_ROT_Z; i++) {
    ggd->gizmos[i] = WM_gizmo_new_ptr(gzt_dial, gzgroup, nullptr);
  }

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

    const SculptCursorAxisType axis_type = sculpt_cursor_axis_type_get(i);
    switch (axis_type) {
      case SCULPT_CURSOR_AXES_TRANSLATE:
        gizmo_setup_operator(gz, i, ot_transform, 0);
        break;
      case SCULPT_CURSOR_AXES_ROTATE:
        gizmo_setup_operator(gz, i, ot_transform, 1);
        break;
      case SCULPT_CURSOR_AXES_SCALE:
        gizmo_setup_operator(gz, i, ot_transform, 2);
        break;
    }
  }
}

static void gizmogroup_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  SculptCursorGizmoGroup *ggd = static_cast<SculptCursorGizmoGroup *>(gzgroup->customdata);

  Object *ob = CTX_data_active_object(C);
  if (!ob || !ob->runtime->sculpt_session) {
    return;
  }

  SculptSession *ss = ob->runtime->sculpt_session;

  if (!ss->sculpt_cursor_initialized) {
    for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
      if (wmGizmo *gz = ggd->gizmos[i]) {
        WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
      }
    }
    return;
  }

  float world_mat[4][4];
  sculpt_cursor_world_matrix_get(*ob, *ss, world_mat);

  float idot[3] = {1.0f, 1.0f, 1.0f};
  const bool is_modal = WM_gizmo_group_is_modal(gzgroup);
  if (!is_modal) {
    if (ARegion *region = CTX_wm_region(C)) {
      RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
      gizmo_get_idot(rv3d, world_mat, idot);
    }
  }

  const float *scale = ss->sculpt_cursor_scale;
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

    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, hide);
    if (!hide) {
      gizmo_refresh_from_matrix(gz, i, world_mat, scale);
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
  if (!ob || !ob->runtime->sculpt_session || !ob->runtime->sculpt_session->sculpt_cursor_initialized)
  {
    return;
  }

  SculptSession *ss = ob->runtime->sculpt_session;
  float world_mat[4][4];
  sculpt_cursor_world_matrix_get(*ob, *ss, world_mat);

  float idot[3];
  gizmo_get_idot(rv3d, world_mat, idot);

  for (int i = 0; i < SCULPT_CURSOR_AXIS_LEN; i++) {
    wmGizmo *gz = ggd->gizmos[i];
    if (!gz || (gz->flag & WM_GIZMO_HIDDEN)) {
      continue;
    }

    float color[4], color_hi[4];
    gizmo_get_axis_color(i, idot, color, color_hi);
    WM_gizmo_set_color(gz, color);
    WM_gizmo_set_color_highlight(gz, color_hi);
  }
}

/** \} */

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
}

}  // namespace blender::ed::view3d
