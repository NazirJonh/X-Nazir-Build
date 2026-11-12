/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The pure helpers of the volume mode's standard axis handles: line ranges, per-axis colors, the
 * view-facing hiding of translate axes and the per-kind draw styles. Carried over from
 * view3d_gizmo_sculpt_cursor.cc (its "Gizmo Setup" section, which mirrors transform_gizmo_3d.cc)
 * and adapted to the projection frame's own axes; the sculpt cursor keeps its private originals so
 * its behavior cannot drift with this module's needs.
 */

#include "BLI_index_range.hh"
#include "BLI_math_constants.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "DNA_view3d_types.h"

#include "ED_gizmo_library.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "projection_gizmo_intern.hh"

namespace blender::ed::sculpt_paint::projection {

/* -------------------------------------------------------------------- */
/** \name Shared helpers
 * \{ */

/** Scale of the two-axis plane squares (see #projection_axis_draw_setup). */
#define PROJ_GIZMO_PLANE_SCALE 0.7f

/** Line width of the axis handles, as #GIZMO_AXIS_LINE_WIDTH in transform_gizmo.hh. */
#define PROJ_GIZMO_AXIS_LINE_WIDTH 2.0f

/** Threshold for hiding axes pointing towards the view, as in the Transform gizmo: index 1 is the
 * range of the plane squares, whose dot product is flipped. */
static const struct {
  float min, max;
} g_axis_range[2] = {{0.02f, 0.1f}, {0.175f, 0.25f}};

/** Whether \a axis_idx is a two-axis plane square. */
static bool projection_axis_is_plane(const int axis_idx)
{
  return ELEM(axis_idx, PROJ_AXIS_SCALE_XY, PROJ_AXIS_SCALE_YZ, PROJ_AXIS_SCALE_ZX);
}

ProjectionGizmoAxesType projection_axis_type_get(const int axis_idx)
{
  if (axis_idx >= PROJ_AXIS_TRANS_X && axis_idx <= PROJ_AXIS_TRANS_C) {
    return PROJ_AXES_TRANSLATE;
  }
  if (axis_idx >= PROJ_AXIS_ROT_X && axis_idx <= PROJ_AXIS_ROT_C) {
    return PROJ_AXES_ROTATE;
  }
  if (axis_idx >= PROJ_AXIS_SCALE_C && axis_idx <= PROJ_AXIS_SCALE_ZX) {
    return PROJ_AXES_SCALE;
  }
  return PROJ_AXES_TRANSLATE;
}

int projection_axis_norm_index(const int axis_idx)
{
  switch (axis_idx) {
    case PROJ_AXIS_TRANS_X:
    case PROJ_AXIS_ROT_X:
    case PROJ_AXIS_SCALE_X:
    case PROJ_AXIS_SCALE_YZ:
      return 0;
    case PROJ_AXIS_TRANS_Y:
    case PROJ_AXIS_ROT_Y:
    case PROJ_AXIS_SCALE_Y:
    case PROJ_AXIS_SCALE_ZX:
      return 1;
    case PROJ_AXIS_TRANS_Z:
    case PROJ_AXIS_ROT_Z:
    case PROJ_AXIS_SCALE_Z:
    case PROJ_AXIS_SCALE_XY:
      return 2;
    default:
      return -1;
  }
}

void projection_axis_line_range(const ProjectionGizmoAxesType axis_type,
                                const int show_mask,
                                float *r_start,
                                float *r_end)
{
  float start = 0.2f;
  float end = 1.0f;

  /* Mirrors #gizmo_line_range in transform_gizmo_3d.cc: with several sets shown the scale handles
   * sit inside, the rotate dials on the middle radius, the translate arrows outside. */
  switch (axis_type) {
    case PROJ_AXES_TRANSLATE:
      if (show_mask & PROJECTION_GIZMO_SHOW_SCALE) {
        start = end - 0.125f;
      }
      if (show_mask & PROJECTION_GIZMO_SHOW_ROTATE) {
        /* Avoid rotate and translate gizmos overlap. */
        const float rotate_offset = 0.215f;
        start += rotate_offset;
        end += rotate_offset + 0.2f;
      }
      break;
    case PROJ_AXES_SCALE:
      if (show_mask & (PROJECTION_GIZMO_SHOW_TRANSLATE | PROJECTION_GIZMO_SHOW_ROTATE)) {
        end -= 0.225f;
      }
      break;
    case PROJ_AXES_ROTATE:
      break;
  }

  if (r_start != nullptr) {
    *r_start = start;
  }
  if (r_end != nullptr) {
    *r_end = end;
  }
}

void projection_axis_idot_get(const RegionView3D *rv3d,
                              const float world_mat[4][4],
                              float r_idot[3])
{
  float view_vec[3], axis_vec[3];
  ED_view3d_global_to_vector(rv3d, world_mat[3], view_vec);
  for (const int i : IndexRange(3)) {
    normalize_v3_v3(axis_vec, world_mat[i]);
    r_idot[i] = 1.0f - fabsf(dot_v3v3(view_vec, axis_vec));
  }
}

bool projection_axis_is_visible(const float idot[3], const int axis_idx)
{
  const int axis_norm = projection_axis_norm_index(axis_idx);
  /* Rotation rings and the screen-aligned handles never hide. */
  if (axis_norm < 0 || projection_axis_type_get(axis_idx) == PROJ_AXES_ROTATE) {
    return true;
  }
  const bool is_plane = projection_axis_is_plane(axis_idx);
  const float idot_axis = is_plane ? 1.0f - idot[axis_norm] : idot[axis_norm];
  return idot_axis >= g_axis_range[is_plane].min;
}

void projection_axis_color_get(const int axis_idx,
                               const float idot[3],
                               float r_col[4],
                               float r_col_hi[4])
{
  /* Alpha values for normal/highlighted states, as #gizmo_get_axis_color in transform_gizmo_3d.cc. */
  const float alpha = 0.6f;
  const float alpha_hi = 1.0f;
  float alpha_fac = 1.0f;

  /* Everything but the rotation rings fades out as its axis turns towards the view. */
  const int axis_norm = projection_axis_norm_index(axis_idx);
  if (axis_norm >= 0 && projection_axis_type_get(axis_idx) != PROJ_AXES_ROTATE) {
    const bool is_plane = projection_axis_is_plane(axis_idx);
    const float idot_axis = is_plane ? 1.0f - idot[axis_norm] : idot[axis_norm];
    const float idot_min = g_axis_range[is_plane].min;
    const float idot_max = g_axis_range[is_plane].max;
    alpha_fac = (idot_axis > idot_max) ? 1.0f :
                (idot_axis < idot_min) ? 0.0f :
                                         ((idot_axis - idot_min) / (idot_max - idot_min));
  }

  switch (axis_idx) {
    case PROJ_AXIS_TRANS_X:
    case PROJ_AXIS_ROT_X:
    case PROJ_AXIS_SCALE_X:
    case PROJ_AXIS_SCALE_YZ:
      ui::theme::get_color_4fv(TH_AXIS_X, r_col);
      break;
    case PROJ_AXIS_TRANS_Y:
    case PROJ_AXIS_ROT_Y:
    case PROJ_AXIS_SCALE_Y:
    case PROJ_AXIS_SCALE_ZX:
      ui::theme::get_color_4fv(TH_AXIS_Y, r_col);
      break;
    case PROJ_AXIS_TRANS_Z:
    case PROJ_AXIS_ROT_Z:
    case PROJ_AXIS_SCALE_Z:
    case PROJ_AXIS_SCALE_XY:
      ui::theme::get_color_4fv(TH_AXIS_Z, r_col);
      break;
    case PROJ_AXIS_SCALE_C:
    case PROJ_AXIS_ROT_C:
    case PROJ_AXIS_TRANS_C:
      ui::theme::get_color_4fv(TH_GIZMO_VIEW_ALIGN, r_col);
      break;
    default:
      return;
  }

  copy_v4_v4(r_col_hi, r_col);
  r_col[3] = alpha * alpha_fac;
  r_col_hi[3] = alpha_hi * alpha_fac;
}

void projection_axis_plane_matrix_setup(float r_mat[4][4], const int i)
{
  /* The default matrices of #gizmo_3d_setup_default_matrix in transform_gizmo_3d.cc, in frame
   * coordinates: the arrow's local Z points along the diagonal between the two axes the plane
   * spans, and the square (drawn at the end of the arrow's length) sits on it. */
  float mat[3][3];
  switch (i) {
    case 0: /* YZ */
      copy_v3_fl3(mat[0], 0.0f, -float(M_SQRT1_2), float(M_SQRT1_2));
      copy_v3_fl3(mat[1], 1.0f, 0.0f, 0.0f);
      copy_v3_fl3(mat[2], 0.0f, float(M_SQRT1_2), float(M_SQRT1_2));
      break;
    case 1: /* ZX */
      copy_v3_fl3(mat[0], float(M_SQRT1_2), 0.0f, -float(M_SQRT1_2));
      copy_v3_fl3(mat[1], 0.0f, 1.0f, 0.0f);
      copy_v3_fl3(mat[2], float(M_SQRT1_2), 0.0f, float(M_SQRT1_2));
      break;
    default: /* XY */
      copy_v3_fl3(mat[0], -float(M_SQRT1_2), float(M_SQRT1_2), 0.0f);
      copy_v3_fl3(mat[1], 0.0f, 0.0f, 1.0f);
      copy_v3_fl3(mat[2], float(M_SQRT1_2), float(M_SQRT1_2), 0.0f);
      break;
  }
  unit_m4(r_mat);
  copy_m4_m3(r_mat, mat);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw styles
 *
 * One case per handle kind, from #gizmo_setup_draw in view3d_gizmo_sculpt_cursor.cc. The matrices
 * are not set here: place() owns them, since the frame's axes move with the row.
 * \{ */

void projection_axis_draw_setup(wmGizmo *gz, const int axis_idx)
{
  if (gz == nullptr) {
    return;
  }

  /* The same values as #gizmo_3d_setup_draw_default in transform_gizmo_3d.cc. */
  switch (axis_idx) {
    case PROJ_AXIS_TRANS_X:
    case PROJ_AXIS_TRANS_Y:
    case PROJ_AXIS_TRANS_Z:
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_NORMAL);
      WM_gizmo_set_line_width(gz, PROJ_GIZMO_AXIS_LINE_WIDTH);
      break;
    case PROJ_AXIS_SCALE_X:
    case PROJ_AXIS_SCALE_Y:
    case PROJ_AXIS_SCALE_Z:
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_BOX);
      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_ARROW_DRAW_FLAG_STEM);
      WM_gizmo_set_line_width(gz, PROJ_GIZMO_AXIS_LINE_WIDTH);
      break;
    case PROJ_AXIS_SCALE_XY:
    case PROJ_AXIS_SCALE_YZ:
    case PROJ_AXIS_SCALE_ZX:
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_PLANE);
      RNA_enum_set(gz->ptr, "draw_options", 0);
      RNA_float_set(gz->ptr, "length", PROJ_GIZMO_PLANE_SCALE);
      WM_gizmo_set_line_width(gz, 1.0f);
      break;
    case PROJ_AXIS_SCALE_C:
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_PRIMITIVE_STYLE_ANNULUS);
      RNA_boolean_set(gz->ptr, "draw_inner", false);
      /* Use 6 since this is '1.2' if the main scale is 0.2. */
      RNA_float_set(gz->ptr, "arc_inner_factor", 6.0f);
      WM_gizmo_set_scale(gz, 0.2f);
      WM_gizmo_set_line_width(gz, PROJ_GIZMO_AXIS_LINE_WIDTH);
      /* Prevent axis gizmos overlapping the center point (see #transform_gizmo_3d.cc #63744). */
      gz->select_bias = -2.0f;
      break;
    case PROJ_AXIS_ROT_X:
    case PROJ_AXIS_ROT_Y:
    case PROJ_AXIS_ROT_Z:
      /* Clip the dial against the geometry behind it, as in #transform_gizmo_3d.cc. */
      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_CLIP);
      RNA_float_set(gz->ptr, "incremental_angle", 0.0f);
      WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_VALUE, true);
      /* Increased line width for better display. */
      WM_gizmo_set_line_width(gz, PROJ_GIZMO_AXIS_LINE_WIDTH + 1.0f);
      gz->select_bias = 0.0f;
      break;
    case PROJ_AXIS_ROT_C:
      /* Screen-aligned ring, like #MAN_AXIS_ROT_C in #transform_gizmo_3d.cc. */
      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_NOP);
      RNA_float_set(gz->ptr, "incremental_angle", 0.0f);
      WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_VALUE, true);
      WM_gizmo_set_scale(gz, 1.2f);
      WM_gizmo_set_line_width(gz, PROJ_GIZMO_AXIS_LINE_WIDTH);
      gz->select_bias = 0.0f;
      break;
    case PROJ_AXIS_TRANS_C:
      /* Central circle, like #MAN_AXIS_TRANS_C in #transform_gizmo_3d.cc. */
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_PRIMITIVE_STYLE_CIRCLE);
      RNA_boolean_set(gz->ptr, "draw_inner", false);
      WM_gizmo_set_scale(gz, 0.2f);
      WM_gizmo_set_line_width(gz, PROJ_GIZMO_AXIS_LINE_WIDTH);
      /* Prevent axis gizmos overlapping the center point (see #transform_gizmo_3d.cc #63744). */
      gz->select_bias = 2.0f;
      break;
    default:
      break;
  }
}

void projection_axis_draw_setup_for_mask(wmGizmo *gz, const int axis_idx, const int show_mask)
{
  if (gz == nullptr) {
    return;
  }
  const ProjectionGizmoAxesType axis_type = projection_axis_type_get(axis_idx);

  /* The same values as #gizmo_3d_setup_draw_from_twtype in transform_gizmo_3d.cc. */
  switch (axis_idx) {
    case PROJ_AXIS_TRANS_X:
    case PROJ_AXIS_TRANS_Y:
    case PROJ_AXIS_TRANS_Z:
      RNA_enum_set(gz->ptr,
                   "draw_options",
                   (show_mask & (PROJECTION_GIZMO_SHOW_ROTATE | PROJECTION_GIZMO_SHOW_SCALE)) ?
                       0 :
                       ED_GIZMO_ARROW_DRAW_FLAG_STEM);
      ATTR_FALLTHROUGH;
    case PROJ_AXIS_SCALE_X:
    case PROJ_AXIS_SCALE_Y:
    case PROJ_AXIS_SCALE_Z: {
      float start, end;
      projection_axis_line_range(axis_type, show_mask, &start, &end);
      /* The arrow's local axes are already the frame axis (see the place functions), so the
       * line start lies along local Z. */
      zero_v3(gz->matrix_offset[3]);
      gz->matrix_offset[3][2] = start;
      RNA_float_set(gz->ptr, "length", end - start);
      WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_OFFSET_SCALE, true);
      break;
    }
    case PROJ_AXIS_ROT_X:
    case PROJ_AXIS_ROT_Y:
    case PROJ_AXIS_ROT_Z:
      /* Deprioritize rotation dials vs scale handles when hit regions overlap. */
      gz->select_bias = ((show_mask & PROJECTION_GIZMO_SHOW_SCALE) &&
                         (show_mask & PROJECTION_GIZMO_SHOW_ROTATE)) ?
                            -2.0f :
                            0.0f;
      break;
    default:
      break;
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::projection
