/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared state and mode entry points of the projection frame gizmo. #projection_gizmo.cc carries
 * the group plumbing and picks a mode from the shown row's mapping space; the mode files own the
 * handles of their projection kinds.
 */

#pragma once

#include <cstdint>

#include "WM_types.hh"

#include "projection_target.hh"

namespace blender {
struct bContext;
struct MaterialPaintLayer;
struct Object;
struct RegionView3D;
struct wmGizmo;
struct wmGizmoGroup;
struct wmEvent;
}  // namespace blender

namespace blender::ed::sculpt_paint::projection {

/** Opacity of every handle, so the surface stays readable under the gizmo. */
constexpr float PROJECTION_GIZMO_ALPHA = 0.5f;

/**
 * Floats of the planar surface-height cache key: the painted-to-frame matrix (16), the frame's
 * half extents on X and Y (2), the evaluated mesh's vertex count (1) and its pointer, which takes
 * two floats on a 64-bit build (2).
 */
constexpr int PROJECTION_SURFACE_KEY_SIZE = 21;

/** The two handle sets of the gizmo, chosen by the shown row's mapping space. */
enum ProjectionGizmoMode {
  /** Thin frame cage with roll, depth, direction and a surface-snap move ring. */
  PROJECTION_GIZMO_PLANAR = 0,
  /** Full box cage with axis move/scale handles, for rows that fill a volume. */
  PROJECTION_GIZMO_VOLUME,
};

/** Bits of the volume mode's standard handle sets, shown while the Projection Frame tool is
 * active (which set comes from #Sculpt::projection_gizmo_set). */
enum ProjectionGizmoShowFlag {
  PROJECTION_GIZMO_SHOW_TRANSLATE = (1 << 0),
  PROJECTION_GIZMO_SHOW_ROTATE = (1 << 1),
  PROJECTION_GIZMO_SHOW_SCALE = (1 << 2),
  PROJECTION_GIZMO_SHOW_ALL = PROJECTION_GIZMO_SHOW_TRANSLATE | PROJECTION_GIZMO_SHOW_ROTATE |
                              PROJECTION_GIZMO_SHOW_SCALE,
};

/** Every handle of the group; the mode shows one set and hides the other. */
struct ProjectionGizmoGroup {
  PaintLayerRowProjectionTarget target;

  /* Planar mode, see #projection_gizmo_planar.cc. */
  wmGizmo *gz_cage = nullptr;
  wmGizmo *gz_roll = nullptr;
  wmGizmo *gz_depth = nullptr;
  wmGizmo *gz_direction = nullptr;
  wmGizmo *gz_move = nullptr;

  /* Volume mode, see #projection_gizmo_volume.cc. */
  wmGizmo *gz_volume_cage = nullptr;
  /** Translate arrows along the frame axes, plane squares in the frame planes, and a free
   * view-plane circle at the center; drawn in the sculpt-gizmo style. */
  wmGizmo *gz_move_axis[3] = {nullptr, nullptr, nullptr};
  wmGizmo *gz_move_plane[3] = {nullptr, nullptr, nullptr};
  wmGizmo *gz_move_center = nullptr;
  /** Rotate dials around the frame axes, and the view-aligned ring. */
  wmGizmo *gz_rot_axis[3] = {nullptr, nullptr, nullptr};
  wmGizmo *gz_rot_c = nullptr;
  /** Scale box arrows on the cube faces, scale planes spanning two axes each, and the
   * uniform-scale annulus at the center. */
  wmGizmo *gz_scale_axis[3] = {nullptr, nullptr, nullptr};
  wmGizmo *gz_scale_plane[3] = {nullptr, nullptr, nullptr};
  wmGizmo *gz_scale_uniform = nullptr;

  /** Full extents of the frame (absolute) and the signs of the stored sizes, from the last place. */
  float base_size[3] = {1.0f, 1.0f, 1.0f};
  float base_sign[3] = {1.0f, 1.0f, 1.0f};
  /**
   * The frame center at the last placement. The move arrows report offsets from it, so it only
   * stays put while one of them is dragged (#drag_gz); otherwise it follows the live frame.
   */
  float drag_anchor_center[3] = {0.0f, 0.0f, 0.0f};

  /**
   * The group's gizmo that runs a drag right now, or null. place() keeps running during a drag so
   * every handle follows the live frame, but the dragged handle of a property-driven kind (cage,
   * arrows, dials) draws itself from its own drag state and is left alone.
   */
  wmGizmo *drag_gz = nullptr;

  /**
   * Session uid of the material whose previews are shaded at an octave while a handle is dragged
   * (see #gradient_interactive_begin), or zero. Kept so the mark is cleared by uid when the drag
   * ends or the group goes away, without dereferencing a material the drag outlived.
   */
  uint32_t drag_material_uid = 0;

  /**
   * Cage state captured at #invoke_prepare: the cage modal reports its scale relative to the
   * matrix it started from, so the set callbacks multiply these start values, not the live frame
   * (which place() keeps updating during the drag).
   */
  float cage_size_init[3] = {1.0f, 1.0f, 1.0f};
  float cage_sign_init[3] = {1.0f, 1.0f, 1.0f};
  float cage_surface_init = 0.0f;

  /**
   * World distance from the frame center to the dragged scale arrow's tip at #invoke_prepare. The
   * arrow stays at a fixed screen size like the Object Mode scale gizmo, and its drag offset
   * relative to this distance gives the scale factor (applied to #cage_size_init).
   */
  float scale_axis_ref = 1.0f;

  /** The handle sets of the last volume placement (bits of #ProjectionGizmoShowFlag). */
  int show_mask = 0;

  /** The cage's preview shape (an #ED_GIZMO_CAGE3D_STYLE_* value: box, cylinder or sphere) and the
   * cylinder's axis (0..2), from the shown row. */
  int volume_cage_style = 0;
  int volume_cage_axis = 2;

  /**
   * Height of the painted surface above the frame plane inside the frame's footprint, in frame
   * units. Planar display only: the handles are drawn there instead of on the stored center plane,
   * which may lie inside the mesh or in the air. Display only, the stored center never changes.
   */
  float surface_height = 0.0f;
  /** What #surface_height was computed from, so it is only recomputed when that changes. */
  float surface_key[PROJECTION_SURFACE_KEY_SIZE] = {};
  bool surface_key_valid = false;

  /**
   * Shift is held during the running volume-cage drag: the set callback collapses the cage's
   * per-axis factors into one, scaling the cube uniformly from its center. Set by the cage's
   * custom modal, the only place the drag's event reaches.
   */
  bool volume_uniform = false;

  /** State of the uniform-scale annulus drag, captured at #invoke_prepare. */
  bool uniform_drag_valid = false;
  int uniform_mval_init[2] = {0, 0};
  float uniform_center_px[2] = {0.0f, 0.0f};
  float uniform_size_init[3] = {1.0f, 1.0f, 1.0f};

  /**
   * Rotate drag state, captured at #invoke_prepare: the frame rotation the drag turns from, and
   * the frame axis the dial turns around (0..2), or 3 for the view ring (whose axis is
   * #rot_view_axis_frame, the view direction in frame coordinates at the drag start).
   */
  bool rot_drag_valid = false;
  float rot_init[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
  int rot_axis = 0;
  float rot_view_axis_frame[3] = {0.0f, 0.0f, 1.0f};

  /** Free-move drag state (the plane squares and the center circle), captured at #invoke_prepare:
   * the anchor center and the frame rotation it was captured with. */
  bool move_drag_valid = false;
  float move_anchor[3] = {0.0f, 0.0f, 0.0f};
  float move_rot[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
  /** Which handle runs the free-move drag: 0..2 a plane square (normal = that frame axis), 3 the
   * center circle (view plane). */
  int move_plane = -1;

  /** Scale-plane drag state, captured at #invoke_prepare like the uniform annulus's. */
  bool scale_plane_valid = false;
  /** The frame axis the dragged plane is perpendicular to; it scales the other two axes. */
  int scale_plane_axis = 0;
  float scale_plane_center_px[2] = {0.0f, 0.0f};
  int scale_plane_mval_init[2] = {0, 0};
  float scale_plane_size_init[3] = {1.0f, 1.0f, 1.0f};
};

/** The handle set a row's mapping space draws: the volume kinds, else the planar frame. */
ProjectionGizmoMode projection_gizmo_mode_get(const MaterialPaintLayer &layer);

/* -------------------------------------------------------------------- */
/** \name Standard axis handles (see #projection_gizmo_axes.cc)
 *
 * Pure helpers of the sculpt-gizmo handle set, carried over from view3d_gizmo_sculpt_cursor.cc
 * and adapted to the frame's own axes. The sculpt cursor's behavior is unchanged: this is a copy,
 * not a shared extraction, so its file keeps no dependency on the projection module.
 * \{ */

/** The three standard axis sets, matching the order of #ProjectionGizmoShowFlag bits. */
enum ProjectionGizmoAxesType {
  PROJ_AXES_TRANSLATE = 0,
  PROJ_AXES_ROTATE = 1,
  PROJ_AXES_SCALE = 2,
};

/** The standard handle kinds, named like the sculpt cursor's #SculptCursorAxis (minus its
 * screen-space frame, which this gizmo does not draw). */
enum ProjectionGizmoAxis {
  PROJ_AXIS_SCALE_C = 0,
  PROJ_AXIS_SCALE_X,
  PROJ_AXIS_SCALE_Y,
  PROJ_AXIS_SCALE_Z,
  PROJ_AXIS_SCALE_XY,
  PROJ_AXIS_SCALE_YZ,
  PROJ_AXIS_SCALE_ZX,
  PROJ_AXIS_ROT_X,
  PROJ_AXIS_ROT_Y,
  PROJ_AXIS_ROT_Z,
  PROJ_AXIS_ROT_C,
  PROJ_AXIS_TRANS_X,
  PROJ_AXIS_TRANS_Y,
  PROJ_AXIS_TRANS_Z,
  PROJ_AXIS_TRANS_C,
};

/** The set \a axis_idx belongs to. */
ProjectionGizmoAxesType projection_axis_type_get(int axis_idx);

/**
 * The frame axis \a axis_idx follows for its theme color: the axis it moves or scales, or the one
 * a plane is perpendicular to.
 */
int projection_axis_norm_index(int axis_idx);

/** Start and end of one handle's line on its axis, in gizmo units, like the Transform gizmo's
 * ranges for the sets in \a show_mask (bits of #ProjectionGizmoShowFlag). */
void projection_axis_line_range(ProjectionGizmoAxesType axis_type,
                                int show_mask,
                                float *r_start,
                                float *r_end);

/** The set-independent style of the standard handle \a gz for \a axis_idx, as the Transform gizmo
 * sets it up once. The gizmo's type must already match the kind. */
void projection_axis_draw_setup(wmGizmo *gz, int axis_idx);

/** What the Transform gizmo re-applies when its shown sets change: the line range and options of
 * the arrows and the select bias of the dials, for the sets in \a show_mask. */
void projection_axis_draw_setup_for_mask(wmGizmo *gz, int axis_idx, int show_mask);

/** How much each frame axis lies across the view: 1 for an axis the view looks across, 0 for one
 * it looks along, from the frame's world matrix \a world_mat. */
void projection_axis_idot_get(const RegionView3D *rv3d,
                              const float world_mat[4][4],
                              float r_idot[3]);

/** Whether the handle \a axis_idx stays visible for \a idot: axes that face the view (and planes
 * seen edge-on) are hidden, like in the Transform gizmo. Rotation rings always stay. */
bool projection_axis_is_visible(const float idot[3], int axis_idx);

/** Theme colors of \a axis_idx, with the Transform gizmo's alphas, faded by how aligned its axis
 * is with the view. */
void projection_axis_color_get(int axis_idx,
                               const float idot[3],
                               float r_col[4],
                               float r_col_hi[4]);

/**
 * The matrix_offset of a plane square for the plane perpendicular to frame axis \a i, as the
 * Transform gizmo orients its plane handles: the square sits on the diagonal between the two axes
 * it spans. Meant to go with a matrix_basis that carries the frame's orientation.
 */
void projection_axis_plane_matrix_setup(float r_mat[4][4], int i);

/** \} */

/** Tag the row's material so the viewport and the editors re-read the frame values. */
void projection_target_notify(Material *ma);

/** Hide or show every handle of the group. */
void projection_gizmo_handles_set_hidden(ProjectionGizmoGroup &ggd, bool hidden);

/** The group's gizmo that is the gizmo map's active modal, i.e. a drag is in flight, or null. */
wmGizmo *projection_gizmo_modal_get(const bContext *C, const ProjectionGizmoGroup *ggd);

/** Create the planar mode's handles. Safe to call when some gizmo types are missing. */
void projection_gizmo_planar_setup(wmGizmoGroup *gzgroup, ProjectionGizmoGroup &ggd);

/** Place the planar mode's handles for \a frame; un-hides only its own set. */
bool projection_gizmo_planar_place(const bContext *C,
                                   ProjectionGizmoGroup &ggd,
                                   const Object &space_ob,
                                   const ProjectionFrame &frame,
                                   const float rot[3][3]);

/** Create the volume mode's handles. Safe to call when some gizmo types are missing. */
void projection_gizmo_volume_setup(wmGizmoGroup *gzgroup, ProjectionGizmoGroup &ggd);

/**
 * Place the volume mode's handles for \a frame; un-hides only its own set. The cage is always
 * shown; the standard Move / Rotate / Scale sets follow \a show_mask (bits of
 * #ProjectionGizmoShowFlag), which is all-zero unless the Projection Frame tool is active.
 */
bool projection_gizmo_volume_place(const bContext *C,
                                   ProjectionGizmoGroup &ggd,
                                   const Object &space_ob,
                                   const ProjectionFrame &frame,
                                   const float rot[3][3],
                                   int show_mask);

/** Capture the uniform-scale annulus's drag state when its drag starts. */
void projection_gizmo_volume_invoke_prepare(const bContext *C,
                                            ProjectionGizmoGroup &ggd,
                                            const wmGizmo *gz,
                                            const wmEvent *event);

/**
 * The frame rotation at \a origin: rotation in the 3x3 part, translation in column 3, so a gizmo
 * basis carries the frame's orientation and a position in the space object's coordinates.
 */
void projection_frame_basis(const float rot[3][3], const float origin[3], float r_mat[4][4]);

/** The frame axis \a i in the parent space: column \a i of the frame matrix. */
void projection_frame_axis(const float rot[3][3], int i, float r_axis[3]);

}  // namespace blender::ed::sculpt_paint::projection
