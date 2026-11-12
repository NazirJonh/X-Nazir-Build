/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The volume mode of the projection frame gizmo, for the rows whose projection fills the frame's
 * extents (Box, Triplanar, Cube, Cylinder, Sphere): a full box cage for the extents, and -- while
 * the Projection Frame tool is active -- the standard Move, Rotate and Scale handle sets in the
 * sculpt-gizmo style (see #projection_gizmo_axes.cc for the styles they share with
 * view3d_gizmo_sculpt_cursor.cc).
 *
 * Every handle reads and writes the frame through target properties, so a drag is one undo step
 * and the value-only BKE setters stay the only writers. Property drags report offsets from the
 * placement center (#ProjectionGizmoGroup::drag_anchor_center), which keeps the values small
 * enough for an unconstrained arrow to draw at its own value; the custom drags (the plane squares
 * and the center circle) write the frame directly off a state capture in
 * #projection_gizmo_volume_invoke_prepare, and their undo step still comes from
 * #WM_GIZMO_NEEDS_UNDO at the drag's end. The frame values live in the space object's coordinates
 * while the arrow dials report world-space offsets, so the get/set pair divides by the axis's
 * world scale; a space object at rest scale keeps both equal.
 */

#include <algorithm>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_paint_layers.hh"
#include "BKE_screen.hh"

#include "ED_gizmo_library.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "projection_gizmo_intern.hh"

namespace blender::ed::sculpt_paint::projection {

/** The plane square kinds of the three frame planes: the kind that shares a plane's perpendicular
 * axis also shares its theme color (plane 0 is the YZ square, colored X, and so on). */
constexpr int PROJ_AXIS_PLANE_KINDS[3] = {PROJ_AXIS_SCALE_YZ, PROJ_AXIS_SCALE_ZX,
                                          PROJ_AXIS_SCALE_XY};

/* -------------------------------------------------------------------- */
/** \name Shared helpers
 * \{ */

/** Which entry of \a handles is \a gz, or -1: the get/set callbacks are shared per handle set. */
static int projection_handle_index(const wmGizmo *const *handles, const wmGizmo *gz)
{
  for (const int i : IndexRange(3)) {
    if (handles[i] == gz) {
      return i;
    }
  }
  return -1;
}

/** A new handle with the theme color and the group's opacity and undo flag. */
static wmGizmo *projection_volume_handle_new(const wmGizmoType *gzt,
                                             wmGizmoGroup *gzgroup,
                                             const int theme_id)
{
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  if (gz == nullptr) {
    return nullptr;
  }
  ui::theme::get_color_3fv(theme_id, gz->color);
  ui::theme::get_color_3fv(TH_GIZMO_HI, gz->color_hi);
  gz->color[3] = PROJECTION_GIZMO_ALPHA;
  gz->color_hi[3] = PROJECTION_GIZMO_ALPHA;
  WM_gizmo_set_flag(gz, WM_GIZMO_NEEDS_UNDO, true);
  return gz;
}

/**
 * Frame rotation with local +Z along frame axis \a i, at \a origin: the arrows' drag and draw axis
 * and the rotate dials' turn axis, like the depth arrow and the roll dial build theirs.
 */
static void projection_axis_basis(const float rot[3][3],
                                  const int i,
                                  const float origin[3],
                                  float r_mat[4][4])
{
  unit_m4(r_mat);
  /* The columns are the frame axes in the order (i+1, i+2, i), which keeps the basis right-handed
   * with local +Z along frame axis i. */
  const int cols[3] = {(i + 1) % 3, (i + 2) % 3, i};
  for (const int k : IndexRange(3)) {
    for (const int j : IndexRange(3)) {
      r_mat[k][j] = rot[cols[k]][j];
    }
  }
  copy_v3_v3(r_mat[3], origin);
}

/**
 * How many world units one frame-space unit along frame axis \a i spans: the arrow properties are
 * world-sized (the arrow modal measures its drag with viewport rays), while the frame values live
 * in the space object's coordinates. A space object at rest scale returns one.
 */
static float projection_axis_world_scale(const Object &space_ob, const float rot[3][3], const int i)
{
  float axis_local[3];
  projection_frame_axis(rot, i, axis_local);
  float axis_world[3];
  mul_v3_mat3_m4v3(axis_world, space_ob.object_to_world().ptr(), axis_local);
  return std::max(len_v3(axis_world), 1e-6f);
}

/** View-aligned rotation with local +Z along the view direction, at \a origin: the basis of the
 * screen-aligned handles, like the sculpt cursor builds #MAN_AXIS_ROT_C and #MAN_AXIS_TRANS_C. */
static void projection_view_basis(const RegionView3D &rv3d,
                                  const float origin[3],
                                  float r_mat[4][4])
{
  unit_m4(r_mat);
  float x_axis[3], y_axis[3], z_axis[3];
  copy_v3_v3(z_axis, rv3d.viewinv[2]);
  ortho_basis_v3v3_v3(x_axis, y_axis, z_axis);
  copy_v3_v3(r_mat[0], x_axis);
  copy_v3_v3(r_mat[1], y_axis);
  copy_v3_v3(r_mat[2], z_axis);
  copy_v3_v3(r_mat[3], origin);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume cage (scale the extents)
 * \{ */

static void projection_volume_cage_matrix_get(const wmGizmo * /*gz*/,
                                              wmGizmoProperty *gz_prop,
                                              void *value_p)
{
  float(*matrix)[4] = static_cast<float(*)[4]>(value_p);
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  ProjectionFrame frame;
  if (ggd != nullptr && ggd->target.read(frame)) {
    /* Unit scale: the modal reports a scale delta; dimensions carry the absolute size. */
    float rot[3][3];
    BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot);
    projection_frame_basis(rot, frame.center, matrix);
  }
  else {
    unit_m4(matrix);
  }
}

static void projection_volume_cage_matrix_set(const wmGizmo * /*gz*/,
                                              wmGizmoProperty *gz_prop,
                                              const void *value_p)
{
  const float(*matrix)[4] = static_cast<const float(*)[4]>(value_p);
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd == nullptr) {
    return;
  }
  float loc[3];
  float rot[3][3];
  float scale[3];
  mat4_to_loc_rot_size(loc, rot, scale, matrix);
  UNUSED_VARS(loc);

  /* Shift holds the drag uniform: collapse the cage's per-axis factors into one. */
  float factor[3] = {scale[0], scale[1], scale[2]};
  if (ggd->volume_uniform) {
    const float uniform = (scale[0] + scale[1] + scale[2]) / 3.0f;
    factor[0] = uniform;
    factor[1] = uniform;
    factor[2] = uniform;
  }

  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return;
  }
  /* Scaling from the center: the cage modal pivots at the dragged corner, but the stored center
   * never moves for the volume frame. The signs of the stored sizes are kept, like the planar
   * cage does. */
  for (int i = 0; i < 3; i++) {
    frame.size[i] = copysignf(
        std::max(ggd->cage_size_init[i] * fabsf(factor[i]), PAINT_LAYER_PROJECTION_SIZE_MIN),
        ggd->cage_sign_init[i]);
  }
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

/**
 * Shift toggles uniform scaling for the running drag: #gizmo_cage3d_modal has no modifier handling
 * of its own, so the wrapper stashes the state for the set callback and defers the drag itself to
 * the library modal.
 */
static wmOperatorStatus projection_volume_cage_modal(bContext *C,
                                                     wmGizmo *gz,
                                                     const wmEvent *event,
                                                     eWM_GizmoFlagTweak tweak_flag)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz->parent_gzgroup->customdata);
  if (ggd != nullptr) {
    ggd->volume_uniform = (event->modifier & KM_SHIFT) != 0;
  }
  return gz->type->modal(C, gz, event, tweak_flag);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move handles (axes, planes, center)
 * \{ */

static void projection_move_axis_get(const wmGizmo *gz,
                                     wmGizmoProperty *gz_prop,
                                     void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  *static_cast<float *>(value_p) = 0.0f;
  if (ggd == nullptr || ggd->target.space_object() == nullptr) {
    return;
  }
  const int axis = projection_handle_index(ggd->gz_move_axis, gz);
  ProjectionFrame frame;
  if (axis == -1 || !ggd->target.read(frame)) {
    return;
  }
  /* The arrow draws its value along its own world axis, so the frame's local center offset is
   * reported in world units. */
  float rot[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot);
  float axis_local[3];
  projection_frame_axis(rot, axis, axis_local);
  float delta[3];
  sub_v3_v3v3(delta, frame.center, ggd->drag_anchor_center);
  *static_cast<float *>(value_p) = dot_v3v3(delta, axis_local) *
                                   projection_axis_world_scale(*ggd->target.space_object(), rot,
                                                               axis);
}

static void projection_move_axis_set(const wmGizmo *gz,
                                     wmGizmoProperty *gz_prop,
                                     const void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd == nullptr || ggd->target.space_object() == nullptr) {
    return;
  }
  const int axis = projection_handle_index(ggd->gz_move_axis, gz);
  ProjectionFrame frame;
  if (axis == -1 || !ggd->target.read(frame)) {
    return;
  }
  float rot[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot);
  float axis_local[3];
  projection_frame_axis(rot, axis, axis_local);
  const float offset_local = *static_cast<const float *>(value_p) /
                             projection_axis_world_scale(*ggd->target.space_object(), rot, axis);
  madd_v3_v3v3fl(frame.center, ggd->drag_anchor_center, axis_local, offset_local);
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

/**
 * Drag of the plane squares and of the center circle: the cursor ray meets the handle's plane, and
 * the hit moves the frame's center by the same offset. The plane squares clamp to their frame
 * plane, the circle moves freely in the view plane. The state (#move_drag_valid) is captured at
 * #invoke_prepare, so the drag writes a delta onto a still anchor.
 */
static wmOperatorStatus projection_move_drag_modal(bContext *C,
                                                   wmGizmo *gz,
                                                   const wmEvent *event,
                                                   eWM_GizmoFlagTweak /*tweak_flag*/)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz->parent_gzgroup->customdata);
  if (ggd == nullptr || !ggd->move_drag_valid || event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }
  const ARegion *region = CTX_wm_region(C);
  Object *space_ob = ggd->target.space_object();
  if (region == nullptr || space_ob == nullptr) {
    return OPERATOR_RUNNING_MODAL;
  }

  const float4x4 &o2w = space_ob->object_to_world();
  float anchor_world[3];
  mul_v3_m4v3(anchor_world, o2w.ptr(), ggd->move_anchor);

  /* The plane the drag meets: a plane square's own frame plane, or the view plane. */
  float plane_no_world[3];
  if (ggd->move_plane <= 2) {
    float axis_local[3];
    projection_frame_axis(ggd->move_rot, ggd->move_plane, axis_local);
    mul_v3_mat3_m4v3(plane_no_world, o2w.ptr(), axis_local);
    normalize_v3(plane_no_world);
  }
  else {
    const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
    if (rv3d == nullptr) {
      return OPERATOR_RUNNING_MODAL;
    }
    copy_v3_v3(plane_no_world, rv3d->viewinv[2]);
  }

  float plane[4];
  plane_from_point_normal_v3(plane, anchor_world, plane_no_world);
  const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  float co_world[3];
  if (!ED_view3d_win_to_3d_on_plane(region, plane, mval_fl, false, co_world)) {
    return OPERATOR_RUNNING_MODAL;
  }
  float delta_world[3];
  sub_v3_v3v3(delta_world, co_world, anchor_world);
  if (ggd->move_plane <= 2) {
    /* The ray-cast already lands on the plane; drop the numeric residue along its normal. */
    madd_v3_v3fl(delta_world, plane_no_world, -dot_v3v3(delta_world, plane_no_world));
  }

  float delta_local[3];
  float o2w_inv[4][4];
  invert_m4_m4(o2w_inv, o2w.ptr());
  mul_v3_mat3_m4v3(delta_local, o2w_inv, delta_world);

  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return OPERATOR_RUNNING_MODAL;
  }
  copy_v3_v3(frame.center, ggd->move_anchor);
  add_v3_v3(frame.center, delta_local);
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
  return OPERATOR_RUNNING_MODAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rotate handles (axis dials, view ring)
 * \{ */

/**
 * The dial adds its drag angle to the value read at invoke, and that value is captured as the
 * frame's rotation at #invoke_prepare: zero always means "the frame as the drag found it", so the
 * set below turns the captured rotation by the reported angle.
 */
static void projection_rot_get(const wmGizmo * /*gz*/,
                               wmGizmoProperty * /*gz_prop*/,
                               void *value_p)
{
  *static_cast<float *>(value_p) = 0.0f;
}

static void projection_rot_set(const wmGizmo * /*gz*/,
                               wmGizmoProperty *gz_prop,
                               const void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd == nullptr || !ggd->rot_drag_valid) {
    return;
  }
  const float angle = *static_cast<const float *>(value_p);

  /* The turn is composed in the frame's own coordinates: post-multiplying keeps every other frame
   * axis fixed while the dial's axis turns by \a angle. */
  float turn[3][3];
  if (ggd->rot_axis <= 2) {
    float axis_local[3] = {0.0f, 0.0f, 0.0f};
    axis_local[ggd->rot_axis] = 1.0f;
    axis_angle_normalized_to_mat3(turn, axis_local, angle);
  }
  else {
    axis_angle_normalized_to_mat3(turn, ggd->rot_view_axis_frame, angle);
  }
  float rot[3][3];
  mul_m3_m3m3(rot, ggd->rot_init, turn);

  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return;
  }
  BKE_paint_layers_projection_frame_from_matrix(rot, frame.normal, &frame.roll);
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scale handles (axis arrows, planes, uniform annulus)
 * \{ */

static void projection_scale_axis_get(const wmGizmo * /*gz*/,
                                      wmGizmoProperty * /*gz_prop*/,
                                      void *value_p)
{
  /* Always zero: the unconstrained arrow shifts its origin by its value along its axis, and the
   * scale handle keeps a fixed place like the Object Mode scale gizmo instead of riding on the
   * cube's face. The drag's offset from zero is what #projection_scale_axis_set turns into a
   * scale factor. */
  *static_cast<float *>(value_p) = 0.0f;
}

static void projection_scale_axis_set(const wmGizmo *gz,
                                      wmGizmoProperty *gz_prop,
                                      const void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd == nullptr) {
    return;
  }
  const int axis = projection_handle_index(ggd->gz_scale_axis, gz);
  if (axis == -1) {
    return;
  }
  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return;
  }
  /* The offset along the arrow, relative to the tip's distance from the center at the drag start,
   * scales the extent the drag started with. Clamped before the setter: a size_normalize(0) would
   * read back as one and jump. */
  const float factor = (ggd->scale_axis_ref + *static_cast<const float *>(value_p)) /
                       ggd->scale_axis_ref;
  frame.size[axis] = copysignf(
      std::max(ggd->cage_size_init[axis] * fabsf(factor), PAINT_LAYER_PROJECTION_SIZE_MIN),
      ggd->cage_sign_init[axis]);
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

/**
 * One scale plane scales the two axes it spans by one factor: the drag distance from the handle's
 * screen position, divided by the distance at the drag start. The plane's own axis keeps its size.
 */
static wmOperatorStatus projection_scale_plane_modal(bContext * /*C*/,
                                                     wmGizmo *gz,
                                                     const wmEvent *event,
                                                     eWM_GizmoFlagTweak /*tweak_flag*/)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz->parent_gzgroup->customdata);
  if (ggd == nullptr || !ggd->scale_plane_valid || event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }
  const float2 mval_init = {float(ggd->scale_plane_mval_init[0]),
                            float(ggd->scale_plane_mval_init[1])};
  const float2 mval_now = {float(event->mval[0]), float(event->mval[1])};
  const float dist_init = std::max(math::distance(mval_init, float2(ggd->scale_plane_center_px)),
                                   4.0f);
  const float factor = math::distance(mval_now, float2(ggd->scale_plane_center_px)) / dist_init;

  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return OPERATOR_RUNNING_MODAL;
  }
  for (int i = 0; i < 3; i++) {
    if (i == ggd->scale_plane_axis) {
      continue;
    }
    frame.size[i] = copysignf(
        std::max(fabsf(ggd->scale_plane_size_init[i]) * factor, PAINT_LAYER_PROJECTION_SIZE_MIN),
        ggd->scale_plane_size_init[i]);
  }
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
  return OPERATOR_RUNNING_MODAL;
}

/**
 * Uniform scale from the annulus at the center: the drag distance from the handle's screen
 * position, divided by the distance at the drag start, scales every extent by one factor. The
 * primitive gizmo carries no target property, so the modal writes the frame directly; the undo
 * step still comes from #WM_GIZMO_NEEDS_UNDO at the drag's end.
 */
static wmOperatorStatus projection_scale_uniform_modal(bContext * /*C*/,
                                                       wmGizmo *gz,
                                                       const wmEvent *event,
                                                       eWM_GizmoFlagTweak /*tweak_flag*/)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz->parent_gzgroup->customdata);
  if (ggd == nullptr || !ggd->uniform_drag_valid || event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }
  const float2 mval_init = {float(ggd->uniform_mval_init[0]), float(ggd->uniform_mval_init[1])};
  const float2 mval_now = {float(event->mval[0]), float(event->mval[1])};
  const float dist_init = std::max(math::distance(mval_init, float2(ggd->uniform_center_px)),
                                   4.0f);
  const float factor = math::distance(mval_now, float2(ggd->uniform_center_px)) / dist_init;

  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return OPERATOR_RUNNING_MODAL;
  }
  for (int i = 0; i < 3; i++) {
    frame.size[i] = copysignf(
        std::max(fabsf(ggd->uniform_size_init[i]) * factor, PAINT_LAYER_PROJECTION_SIZE_MIN),
        ggd->uniform_size_init[i]);
  }
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
  return OPERATOR_RUNNING_MODAL;
}

/** Captured when the annulus drag starts: the handle's screen position and the frame sizes. */
static void projection_scale_uniform_invoke_prepare(const bContext *C,
                                                    ProjectionGizmoGroup &ggd,
                                                    const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  ProjectionFrame frame;
  if (region == nullptr || !ggd.target.read(frame)) {
    return;
  }
  float final_matrix[4][4];
  WM_gizmo_calc_matrix_final(ggd.gz_scale_uniform, final_matrix);
  float center_px[2] = {0.0f, 0.0f};
  if (ED_view3d_project_float_global(region, final_matrix[3], center_px, V3D_PROJ_TEST_NOP) !=
      V3D_PROJ_RET_OK)
  {
    return;
  }
  copy_v2_v2(ggd.uniform_center_px, center_px);
  copy_v2_v2_int(ggd.uniform_mval_init, event->mval);
  copy_v3_v3(ggd.uniform_size_init, frame.size);
  ggd.uniform_drag_valid = true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invoke prepare
 * \{ */

void projection_gizmo_volume_invoke_prepare(const bContext *C,
                                            ProjectionGizmoGroup &ggd,
                                            const wmGizmo *gz,
                                            const wmEvent *event)
{
  /* Every drag start invalidates the previous capture: when the one below fails, the modal must
   * not apply the stale anchor and sizes of an older drag. The Shift flag joins it, so a
   * modifier held at the end of one drag cannot leak into the next. */
  ggd.uniform_drag_valid = false;
  ggd.volume_uniform = false;
  ggd.rot_drag_valid = false;
  ggd.move_drag_valid = false;
  ggd.scale_plane_valid = false;
  ggd.move_plane = -1;
  ggd.scale_plane_axis = 0;
  ggd.rot_axis = 0;

  const ARegion *region = CTX_wm_region(C);
  ProjectionFrame frame;
  if (region == nullptr || !ggd.target.read(frame)) {
    return;
  }
  float rot[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot);

  /* --- Rotate dials: the frame rotation at the drag start, and the axis it turns around. --- */
  const int rot_axis = (gz != nullptr) ? projection_handle_index(ggd.gz_rot_axis, gz) : -1;
  if (rot_axis != -1) {
    ggd.rot_axis = rot_axis;
    copy_m3_m3(ggd.rot_init, rot);
    ggd.rot_drag_valid = true;
    return;
  }
  if (gz == ggd.gz_rot_c) {
    /* The view ring turns the frame around the view direction, expressed in frame coordinates at
     * the drag start. */
    const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
    if (rv3d == nullptr) {
      return;
    }
    copy_m3_m3(ggd.rot_init, rot);
    float inv_rot[3][3], view_frame[3];
    invert_m3_m3(inv_rot, rot);
    mul_v3_m3v3(view_frame, inv_rot, rv3d->viewinv[2]);
    if (normalize_v3(view_frame) > 0.0f) {
      copy_v3_v3(ggd.rot_view_axis_frame, view_frame);
      ggd.rot_axis = 3;
      ggd.rot_drag_valid = true;
    }
    return;
  }

  /* --- Free-move drags: the plane squares and the center circle. --- */
  const int move_plane = (gz != nullptr) ? projection_handle_index(ggd.gz_move_plane, gz) : -1;
  if (move_plane != -1) {
    ggd.move_plane = move_plane;
  }
  else if (gz == ggd.gz_move_center) {
    ggd.move_plane = 3;
  }
  if (ggd.move_plane != -1) {
    copy_v3_v3(ggd.move_anchor, frame.center);
    copy_m3_m3(ggd.move_rot, rot);
    ggd.move_drag_valid = true;
    return;
  }

  /* --- Scale arrows: the tip's world distance from the center, the unit of the drag offset. --- */
  const int scale_axis = (gz != nullptr) ? projection_handle_index(ggd.gz_scale_axis, gz) : -1;
  if (scale_axis != -1) {
    float final_matrix[4][4];
    WM_gizmo_calc_matrix_final(gz, final_matrix);
    float line_start, line_end;
    projection_axis_line_range(PROJ_AXES_SCALE, ggd.show_mask, &line_start, &line_end);
    ggd.scale_axis_ref = std::max(line_end * len_v3(final_matrix[2]), 1e-6f);
    return;
  }

  /* --- Scale planes: the handle's screen position and the frame sizes. --- */
  const int scale_plane = (gz != nullptr) ? projection_handle_index(ggd.gz_scale_plane, gz) : -1;
  if (scale_plane != -1) {
    ggd.scale_plane_axis = scale_plane;
    float final_matrix[4][4];
    WM_gizmo_calc_matrix_final(gz, final_matrix);
    float center_px[2] = {0.0f, 0.0f};
    if (ED_view3d_project_float_global(region, final_matrix[3], center_px, V3D_PROJ_TEST_NOP) !=
        V3D_PROJ_RET_OK)
    {
      return;
    }
    copy_v2_v2(ggd.scale_plane_center_px, center_px);
    copy_v2_v2_int(ggd.scale_plane_mval_init, event->mval);
    copy_v3_v3(ggd.scale_plane_size_init, frame.size);
    ggd.scale_plane_valid = true;
    return;
  }

  /* --- The uniform-scale annulus keeps its own capture. --- */
  if (gz == ggd.gz_scale_uniform) {
    projection_scale_uniform_invoke_prepare(C, ggd, event);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Setup and place
 * \{ */

/**
 * Bind the target properties of the property-driven volume handles. Called once at setup and
 * again on every placement: the definition runs the gizmo's property update, which re-syncs the
 * handle's own draw offset with what the get callbacks report after a re-placement. The plane
 * squares, the center circle and the annulus carry no property: their custom modals write the
 * frame directly.
 */
static void projection_volume_properties_define(ProjectionGizmoGroup &ggd)
{
  wmGizmoPropertyFnParams move_params{};
  move_params.value_get_fn = projection_move_axis_get;
  move_params.value_set_fn = projection_move_axis_set;
  move_params.user_data = &ggd;

  wmGizmoPropertyFnParams rot_params{};
  rot_params.value_get_fn = projection_rot_get;
  rot_params.value_set_fn = projection_rot_set;
  rot_params.user_data = &ggd;

  wmGizmoPropertyFnParams scale_params{};
  scale_params.value_get_fn = projection_scale_axis_get;
  scale_params.value_set_fn = projection_scale_axis_set;
  scale_params.user_data = &ggd;

  const auto define = [](wmGizmo *gz, const char *idname, const wmGizmoPropertyFnParams &params) {
    if (gz != nullptr) {
      WM_gizmo_target_property_def_func(gz, idname, &params);
    }
  };
  for (const int i : IndexRange(3)) {
    define(ggd.gz_move_axis[i], "offset", move_params);
    define(ggd.gz_rot_axis[i], "offset", rot_params);
    define(ggd.gz_scale_axis[i], "offset", scale_params);
  }
  define(ggd.gz_rot_c, "offset", rot_params);
}

void projection_gizmo_volume_setup(wmGizmoGroup *gzgroup, ProjectionGizmoGroup &ggd)
{
  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_3d", true);
  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_3d", true);
  const wmGizmoType *gzt_dial = WM_gizmotype_find("GIZMO_GT_dial_3d", true);
  const wmGizmoType *gzt_prim = WM_gizmotype_find("GIZMO_GT_primitive_3d", true);

  /* --- Cage: full extents, per-axis scale, no translate (the Move handles move the center). --- */
  if (gzt_cage != nullptr) {
    wmGizmo *gz = projection_volume_handle_new(gzt_cage, gzgroup, TH_GIZMO_PRIMARY);
    ggd.gz_volume_cage = gz;
    if (gz != nullptr) {
      RNA_enum_set(gz->ptr, "transform", ED_GIZMO_CAGE_XFORM_FLAG_SCALE);
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_CAGE3D_STYLE_BOX);
      /* The volume's edges are a neutral dashed black and white line, readable on any surface. */
      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_CAGE_DRAW_FLAG_DASHED_OUTLINE);
      const float dims[3] = {1.0f, 1.0f, 1.0f};
      RNA_float_set_array(gz->ptr, "dimensions", dims);
      WM_gizmo_set_line_width(gz, 1.5f);
      WM_gizmo_set_fn_custom_modal(gz, projection_volume_cage_modal);
      wmGizmoPropertyFnParams params{};
      params.value_get_fn = projection_volume_cage_matrix_get;
      params.value_set_fn = projection_volume_cage_matrix_set;
      params.user_data = &ggd;
      WM_gizmo_target_property_def_func(gz, "matrix", &params);
    }
  }

  if (gzt_arrow == nullptr || gzt_dial == nullptr || gzt_prim == nullptr) {
    return;
  }

  const int axis_themes[3] = {TH_AXIS_X, TH_AXIS_Y, TH_AXIS_Z};

  /* --- Move arrows along the frame axes, from the center. --- */
  for (const int i : IndexRange(3)) {
    wmGizmo *gz = projection_volume_handle_new(gzt_arrow, gzgroup, axis_themes[i]);
    ggd.gz_move_axis[i] = gz;
    projection_axis_draw_setup(gz, PROJ_AXIS_TRANS_X + i);
  }

  /* --- Move squares in the frame planes, dragging in the plane they span. --- */
  for (const int i : IndexRange(3)) {
    wmGizmo *gz = projection_volume_handle_new(gzt_arrow, gzgroup, axis_themes[i]);
    ggd.gz_move_plane[i] = gz;
    projection_axis_draw_setup(gz, PROJ_AXIS_PLANE_KINDS[i]);
    if (gz != nullptr) {
      WM_gizmo_set_fn_custom_modal(gz, projection_move_drag_modal);
    }
  }

  /* --- Free move circle at the center. --- */
  ggd.gz_move_center = projection_volume_handle_new(gzt_prim, gzgroup, TH_GIZMO_PRIMARY);
  projection_axis_draw_setup(ggd.gz_move_center, PROJ_AXIS_TRANS_C);
  if (ggd.gz_move_center != nullptr) {
    WM_gizmo_set_fn_custom_modal(ggd.gz_move_center, projection_move_drag_modal);
  }

  /* --- Rotate dials around the frame axes, and the view-aligned ring. --- */
  for (const int i : IndexRange(3)) {
    wmGizmo *gz = projection_volume_handle_new(gzt_dial, gzgroup, axis_themes[i]);
    ggd.gz_rot_axis[i] = gz;
    projection_axis_draw_setup(gz, PROJ_AXIS_ROT_X + i);
  }
  ggd.gz_rot_c = projection_volume_handle_new(gzt_dial, gzgroup, TH_GIZMO_VIEW_ALIGN);
  projection_axis_draw_setup(ggd.gz_rot_c, PROJ_AXIS_ROT_C);

  /* --- Scale box arrows on the cube faces. --- */
  for (const int i : IndexRange(3)) {
    wmGizmo *gz = projection_volume_handle_new(gzt_arrow, gzgroup, axis_themes[i]);
    ggd.gz_scale_axis[i] = gz;
    projection_axis_draw_setup(gz, PROJ_AXIS_SCALE_X + i);
  }

  /* --- Scale planes spanning two axes each. --- */
  for (const int i : IndexRange(3)) {
    wmGizmo *gz = projection_volume_handle_new(gzt_arrow, gzgroup, axis_themes[i]);
    ggd.gz_scale_plane[i] = gz;
    projection_axis_draw_setup(gz, PROJ_AXIS_PLANE_KINDS[i]);
    if (gz != nullptr) {
      WM_gizmo_set_fn_custom_modal(gz, projection_scale_plane_modal);
    }
  }

  /* --- Uniform scale annulus at the center. --- */
  ggd.gz_scale_uniform = projection_volume_handle_new(gzt_prim, gzgroup, TH_GIZMO_PRIMARY);
  projection_axis_draw_setup(ggd.gz_scale_uniform, PROJ_AXIS_SCALE_C);
  if (ggd.gz_scale_uniform != nullptr) {
    WM_gizmo_set_fn_custom_modal(ggd.gz_scale_uniform, projection_scale_uniform_modal);
  }

  projection_volume_properties_define(ggd);
}

bool projection_gizmo_volume_place(const bContext *C,
                                   ProjectionGizmoGroup &ggd,
                                   const Object &space_ob,
                                   const ProjectionFrame &frame,
                                   const float rot[3][3],
                                   const int show_mask)
{
  if (ggd.gz_volume_cage == nullptr) {
    return false;
  }
  /* The move arrows report offsets from the placement center, so while one is dragged the anchor
   * stays put from the drag's first event to its last. Any other drag moves the frame under the
   * handles, and the anchor follows it to keep the idle arrows' reported offsets at zero. */
  const bool arrow_dragged = ggd.drag_gz != nullptr &&
                             projection_handle_index(ggd.gz_move_axis, ggd.drag_gz) != -1;
  if (!arrow_dragged) {
    copy_v3_v3(ggd.drag_anchor_center, frame.center);
  }
  /* The dragged handle of these kinds draws from its own drag state (the cage modal's scaled
   * matrix_offset, an arrow's value from the fixed anchor, a dial's angle) and is left as it is.
   * The plane squares, the center circle and the scale handles with a custom modal are placed
   * live: their drags read captured state, not the gizmo matrix. */
  const auto drag_owns_matrix = [&ggd](const wmGizmo *gz) {
    return gz != nullptr && gz == ggd.drag_gz && gz != ggd.gz_move_center &&
           gz != ggd.gz_scale_uniform && projection_handle_index(ggd.gz_move_plane, gz) == -1 &&
           projection_handle_index(ggd.gz_scale_plane, gz) == -1;
  };

  const ARegion *region = CTX_wm_region(C);
  const RegionView3D *rv3d = (region != nullptr) ?
                                 static_cast<const RegionView3D *>(region->regiondata) :
                                 nullptr;

  /* The frame's world matrix at its center: the screen-facing hiding and the colors of the
   * translate arrows read the axes against the view from it. */
  float frame_basis[4][4];
  projection_frame_basis(rot, frame.center, frame_basis);
  float frame_world[4][4];
  mul_m4_m4m4(frame_world, space_ob.object_to_world().ptr(), frame_basis);
  float idot[3] = {1.0f, 1.0f, 1.0f};
  if (rv3d != nullptr) {
    projection_axis_idot_get(rv3d, frame_world, idot);
  }

  ggd.show_mask = show_mask;
  const bool show_translate = (show_mask & PROJECTION_GIZMO_SHOW_TRANSLATE) != 0;
  const bool show_rotate = (show_mask & PROJECTION_GIZMO_SHOW_ROTATE) != 0;
  const bool show_scale = (show_mask & PROJECTION_GIZMO_SHOW_SCALE) != 0;

  const auto set_hidden = [](wmGizmo *gz, const bool hidden) {
    if (gz != nullptr) {
      SET_FLAG_FROM_TEST(gz->flag, hidden, WM_GIZMO_HIDDEN);
    }
  };
  const auto set_color = [&idot](wmGizmo *gz, const int axis_idx) {
    if (gz == nullptr) {
      return;
    }
    float col[4], col_hi[4];
    projection_axis_color_get(axis_idx, idot, col, col_hi);
    copy_v4_v4(gz->color, col);
    copy_v4_v4(gz->color_hi, col_hi);
  };

  /* --- Cage: dimensions = the full extents. --- */
  ggd.gz_volume_cage->flag &= ~WM_GIZMO_HIDDEN;
  RNA_enum_set(ggd.gz_volume_cage->ptr, "draw_style", ggd.volume_cage_style);
  RNA_int_set(ggd.gz_volume_cage->ptr, "shape_axis", ggd.volume_cage_axis);
  if (!drag_owns_matrix(ggd.gz_volume_cage)) {
    RNA_float_set_array(ggd.gz_volume_cage->ptr, "dimensions", ggd.base_size);
    copy_m4_m4(ggd.gz_volume_cage->matrix_basis, space_ob.object_to_world().ptr());
    projection_frame_basis(rot, frame.center, ggd.gz_volume_cage->matrix_offset);
  }

  /* The frame's full basis, in the space object's coordinates: the plane squares and the annulus
   * are oriented with it. */
  for (const int i : IndexRange(3)) {
    float basis[4][4];
    projection_axis_basis(rot, i, frame.center, basis);

    /* Which handles show for which sets follows #gizmo_is_axis_visible in transform_gizmo_3d.cc:
     * the plane squares only belong to a lone Move or Scale set, and axes facing the view hide. */
    const bool show_move_plane = show_translate && !show_rotate;
    const bool show_scale_plane = show_scale && !show_translate && !show_rotate;

    /* Move arrow: screen-sized on its frame axis, dragged by a world-unit offset. */
    if (wmGizmo *gz = ggd.gz_move_axis[i]) {
      set_hidden(gz, !show_translate || !projection_axis_is_visible(idot, PROJ_AXIS_TRANS_X + i));
      projection_axis_draw_setup_for_mask(gz, PROJ_AXIS_TRANS_X + i, show_mask);
      if (!drag_owns_matrix(gz)) {
        mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), basis);
      }
      set_color(gz, PROJ_AXIS_TRANS_X + i);
    }

    /* Move square: on the diagonal between the two axes of its plane, at the frame center. */
    if (wmGizmo *gz = ggd.gz_move_plane[i]) {
      set_hidden(gz,
                 !show_move_plane || !projection_axis_is_visible(idot, PROJ_AXIS_PLANE_KINDS[i]));
      mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), frame_basis);
      projection_axis_plane_matrix_setup(gz->matrix_offset, i);
      set_color(gz, PROJ_AXIS_PLANE_KINDS[i]);
    }

    /* Rotate dial: around its frame axis, at the center. */
    if (wmGizmo *gz = ggd.gz_rot_axis[i]) {
      set_hidden(gz, !show_rotate);
      projection_axis_draw_setup_for_mask(gz, PROJ_AXIS_ROT_X + i, show_mask);
      if (!drag_owns_matrix(gz)) {
        mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), basis);
      }
      set_color(gz, PROJ_AXIS_ROT_X + i);
    }

    /* Scale box arrow: fixed screen size on its frame axis, like the Object Mode scale gizmo. */
    if (wmGizmo *gz = ggd.gz_scale_axis[i]) {
      set_hidden(gz, !show_scale || !projection_axis_is_visible(idot, PROJ_AXIS_SCALE_X + i));
      projection_axis_draw_setup_for_mask(gz, PROJ_AXIS_SCALE_X + i, show_mask);
      if (!drag_owns_matrix(gz)) {
        mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), basis);
      }
      set_color(gz, PROJ_AXIS_SCALE_X + i);
    }

    /* Scale square: on the diagonal between the two axes it scales. */
    if (wmGizmo *gz = ggd.gz_scale_plane[i]) {
      set_hidden(gz,
                 !show_scale_plane || !projection_axis_is_visible(idot, PROJ_AXIS_PLANE_KINDS[i]));
      mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), frame_basis);
      projection_axis_plane_matrix_setup(gz->matrix_offset, i);
      set_color(gz, PROJ_AXIS_PLANE_KINDS[i]);
    }
  }

  /* Screen-aligned handles: the view ring of Rotate and the free move circle, both at the center.
   * The annulus keeps its own frame-plane orientation. */
  if (rv3d != nullptr) {
    float view_basis[4][4];
    projection_view_basis(*rv3d, frame.center, view_basis);
    if (wmGizmo *gz = ggd.gz_rot_c) {
      set_hidden(gz, !show_rotate);
      if (!drag_owns_matrix(gz)) {
        mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), view_basis);
      }
      set_color(gz, PROJ_AXIS_ROT_C);
    }
    if (wmGizmo *gz = ggd.gz_move_center) {
      set_hidden(gz, !show_translate);
      mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), view_basis);
      set_color(gz, PROJ_AXIS_TRANS_C);
    }
  }
  else {
    set_hidden(ggd.gz_rot_c, true);
    set_hidden(ggd.gz_move_center, true);
  }

  if (wmGizmo *gz = ggd.gz_scale_uniform) {
    /* The uniform annulus is a Scale-only handle: with Move shown its center belongs to the
     * move circle. */
    set_hidden(gz, !show_scale || show_translate);
    mul_m4_m4m4(gz->matrix_basis, space_ob.object_to_world().ptr(), frame_basis);
    set_color(gz, PROJ_AXIS_SCALE_C);
  }

  /* The definition re-syncs every handle's own draw state, which the running drag must not see. */
  if (ggd.drag_gz == nullptr) {
    projection_volume_properties_define(ggd);
  }
  return true;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::projection
