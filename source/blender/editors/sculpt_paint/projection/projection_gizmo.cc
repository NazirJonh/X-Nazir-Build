/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D Viewport gizmo for the inline frame of a projected paint-layer row: a box cage in the space
 * the frame's values are stored in -- the controller Empty's when the row names one, else the
 * painted object's -- plus X/Y/Z rotation rings. Moving and scaling the cage writes the row's
 * center and size; a ring writes its Euler component. The Empty's own gizmo moves, turns and
 * scales the whole projection, while this one keeps editing the inline extents inside its space.
 */

#include "MEM_guardedalloc.h"

#include <algorithm>
#include <cmath>

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"

#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "projection_target.hh"

namespace blender::ed::sculpt_paint::projection {

namespace {

struct ProjectionGizmoGroup;

/** One rotation ring: which group it belongs to and which XYZ Euler component it writes. */
struct RotationHandle {
  ProjectionGizmoGroup *group = nullptr;
  int axis = 0;
};

struct ProjectionGizmoGroup {
  wmGizmo *gz_cage = nullptr;
  /** X, Y and Z rotation rings, each in the frame plane perpendicular to its axis. */
  wmGizmo *gz_rot[3] = {nullptr, nullptr, nullptr};
  RotationHandle rot_handle[3];
  PaintLayerRowProjectionTarget target;
  /**
   * The frame extents when the current gesture began. The cage's box size is the `dimensions`
   * property, while its transform modal reports a *scale delta* through the matrix property; the
   * setter multiplies this base by that delta, and the next idle draw folds the result back into
   * `dimensions` and resets the delta to one.
   */
  float base_size[3] = {1.0f, 1.0f, 1.0f};
  /** The sign of each stored extent, so dragging a mirrored frame does not flip it. */
  float base_sign[3] = {1.0f, 1.0f, 1.0f};
};

ProjectionGizmoGroup *projection_gizmo_group(wmGizmoGroup *gzgroup)
{
  return static_cast<ProjectionGizmoGroup *>(gzgroup->customdata);
}

void projection_target_notify(Material *ma)
{
  if (ma == nullptr) {
    return;
  }
  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

/** Whether one of this group's gizmos is the gizmo map's active modal, i.e. a drag is in flight. */
bool projection_gizmo_is_dragging(const bContext *C, const ProjectionGizmoGroup *ggd)
{
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->runtime == nullptr || region->runtime->gizmo_map == nullptr) {
    return false;
  }
  wmGizmo *modal = WM_gizmomap_get_modal(region->runtime->gizmo_map);
  if (modal == nullptr) {
    return false;
  }
  if (modal == ggd->gz_cage) {
    return true;
  }
  for (const wmGizmo *ring : ggd->gz_rot) {
    if (modal == ring) {
      return true;
    }
  }
  return false;
}

void projection_cage_matrix_get(const wmGizmo * /*gz*/,
                                wmGizmoProperty *gz_prop,
                                void *value_p)
{
  float(*matrix)[4] = static_cast<float(*)[4]>(value_p);
  ProjectionGizmoGroup *ggd = static_cast<ProjectionGizmoGroup *>(
      gz_prop->custom_func.user_data);
  ProjectionFrame frame;
  if (ggd != nullptr && ggd->target.read(frame)) {
    /* The box size lives in `dimensions`; the transform property carries the frame's placement
     * and rotation at unit scale, so the modal reports a scale delta rather than a new extent. */
    const float one[3] = {1.0f, 1.0f, 1.0f};
    loc_eul_size_to_mat4(matrix, frame.center, frame.rotation, one);
  }
  else {
    unit_m4(matrix);
  }
}

void projection_cage_matrix_set(const wmGizmo * /*gz*/,
                                wmGizmoProperty *gz_prop,
                                const void *value_p)
{
  const float(*matrix)[4] = static_cast<const float(*)[4]>(value_p);
  ProjectionGizmoGroup *ggd = static_cast<ProjectionGizmoGroup *>(
      gz_prop->custom_func.user_data);
  if (ggd == nullptr) {
    return;
  }

  float loc[3];
  float rot[3][3];
  float scale[3];
  mat4_to_loc_rot_size(loc, rot, scale, matrix);

  ProjectionFrame frame;
  copy_v3_v3(frame.center, loc);
  for (int i = 0; i < 3; i++) {
    frame.size[i] = ggd->base_size[i] * fabsf(scale[i]) * ggd->base_sign[i];
  }
  mat3_normalized_to_eul(frame.rotation, rot);

  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

void projection_rotation_get(const wmGizmo * /*gz*/, wmGizmoProperty *gz_prop, void *value_p)
{
  RotationHandle *handle = static_cast<RotationHandle *>(gz_prop->custom_func.user_data);
  ProjectionFrame frame;
  *static_cast<float *>(value_p) =
      (handle != nullptr && handle->group != nullptr && handle->group->target.read(frame)) ?
          frame.rotation[handle->axis] :
          0.0f;
}

void projection_rotation_set(const wmGizmo * /*gz*/,
                             wmGizmoProperty *gz_prop,
                             const void *value_p)
{
  RotationHandle *handle = static_cast<RotationHandle *>(gz_prop->custom_func.user_data);
  if (handle == nullptr || handle->group == nullptr) {
    return;
  }
  ProjectionGizmoGroup *ggd = handle->group;
  if (ggd->target.write_rotation_axis(handle->axis, *static_cast<const float *>(value_p))) {
    projection_target_notify(ggd->target.material());
  }
}

bool projection_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }
  const View3D *v3d = CTX_wm_view3d(C);
  if (v3d == nullptr || (v3d->gizmo_flag & V3D_GIZMO_HIDE) != 0) {
    return false;
  }
  PaintLayerRowProjectionTarget target;
  return target.resolve(*C);
}

void projection_gizmo_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  ProjectionGizmoGroup *ggd = MEM_new<ProjectionGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<ProjectionGizmoGroup *>(customdata));
  };

  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_3d", true);
  if (gzt_cage == nullptr) {
    return;
  }
  wmGizmo *gz = WM_gizmo_new_ptr(gzt_cage, gzgroup, nullptr);
  ggd->gz_cage = gz;
  if (gz == nullptr) {
    return;
  }

  RNA_enum_set(gz->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE);
  RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_CAGE3D_STYLE_BOX);
  const float dims[3] = {1.0f, 1.0f, 1.0f};
  RNA_float_set_array(gz->ptr, "dimensions", dims);

  /* No `WM_GIZMO_DRAW_HOVER` on purpose: the frame is the only handle the row has in the
   * viewport, so drawing it only while hovered would leave nothing to aim at. */
  ui::theme::get_color_3fv(TH_GIZMO_PRIMARY, gz->color);
  ui::theme::get_color_3fv(TH_GIZMO_HI, gz->color_hi);
  /* The theme lookup fills three components; the cage is an outline and has to be opaque. */
  gz->color[3] = 1.0f;
  gz->color_hi[3] = 1.0f;
  WM_gizmo_set_line_width(gz, 1.5f);
  /* The native cage modal writes through the matrix property on every event; the flag is what
   * gives the whole gesture one undo step. */
  WM_gizmo_set_flag(gz, WM_GIZMO_NEEDS_UNDO, true);

  wmGizmoPropertyFnParams params{};
  params.value_get_fn = projection_cage_matrix_get;
  params.value_set_fn = projection_cage_matrix_set;
  params.user_data = ggd;
  WM_gizmo_target_property_def_func(gz, "matrix", &params);

  const wmGizmoType *gzt_dial = WM_gizmotype_find("GIZMO_GT_dial_3d", true);
  if (gzt_dial == nullptr) {
    return;
  }
  for (int axis = 0; axis < 3; axis++) {
    wmGizmo *ring = WM_gizmo_new_ptr(gzt_dial, gzgroup, nullptr);
    if (ring == nullptr) {
      continue;
    }
    ggd->gz_rot[axis] = ring;
    ggd->rot_handle[axis].group = ggd;
    ggd->rot_handle[axis].axis = axis;
    RNA_enum_set(ring->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_ANGLE_VALUE);
    RNA_boolean_set(ring->ptr, "wrap_angle", true);
    /* The ring is sized by its matrix in world units, not by the screen-space gizmo size. */
    ring->flag |= WM_GIZMO_DRAW_NO_SCALE;
    ui::theme::get_color_3fv(TH_AXIS_X + axis, ring->color);
    ui::theme::get_color_3fv(TH_AXIS_X + axis, ring->color_hi);
    ring->color[3] = 1.0f;
    ring->color_hi[3] = 1.0f;
    WM_gizmo_set_flag(ring, WM_GIZMO_NEEDS_UNDO, true);
    wmGizmoPropertyFnParams ring_params{};
    ring_params.value_get_fn = projection_rotation_get;
    ring_params.value_set_fn = projection_rotation_set;
    ring_params.user_data = &ggd->rot_handle[axis];
    WM_gizmo_target_property_def_func(ring, "offset", &ring_params);
  }
}

/** Resolve the row and place the cage; returns false (and hides it) when nothing projects. */
bool projection_gizmo_place(const bContext *C, wmGizmoGroup *gzgroup)
{
  ProjectionGizmoGroup *ggd = projection_gizmo_group(gzgroup);
  if (ggd == nullptr || ggd->gz_cage == nullptr) {
    return false;
  }
  /* The native modals drive the matrices between events; resetting them from the row here would
   * fight the gesture. */
  if (projection_gizmo_is_dragging(C, ggd)) {
    return true;
  }
  ProjectionFrame frame;
  if (!ggd->target.resolve(*C) || !ggd->target.read(frame) ||
      ggd->target.space_object() == nullptr)
  {
    ggd->gz_cage->flag |= WM_GIZMO_HIDDEN;
    for (wmGizmo *ring : ggd->gz_rot) {
      if (ring != nullptr) {
        ring->flag |= WM_GIZMO_HIDDEN;
      }
    }
    return false;
  }
  ggd->gz_cage->flag &= ~WM_GIZMO_HIDDEN;
  for (wmGizmo *ring : ggd->gz_rot) {
    if (ring != nullptr) {
      ring->flag &= ~WM_GIZMO_HIDDEN;
    }
  }

  /* The box size is `dimensions`; the matrix carries only the placement and rotation at unit
   * scale, so the modal's scale output is a delta the matrix setter applies to the recorded base
   * extents. */
  for (int i = 0; i < 3; i++) {
    const float extent = fabsf(frame.size[i]);
    ggd->base_size[i] = extent;
    ggd->base_sign[i] = frame.size[i] < 0.0f ? -1.0f : 1.0f;
  }
  RNA_float_set_array(ggd->gz_cage->ptr, "dimensions", ggd->base_size);
  /* The frame's values are stored in the controller's space when the row names one, so the whole
   * gizmo is placed through that object's transform and the drop of a drag lands where the panel
   * would put it. */
  const Object *ob = ggd->target.space_object();
  copy_m4_m4(ggd->gz_cage->matrix_basis, ob->object_to_world().ptr());
  const float one[3] = {1.0f, 1.0f, 1.0f};
  loc_eul_size_to_mat4(ggd->gz_cage->matrix_offset, frame.center, frame.rotation, one);

  /* Each ring lies in the frame plane perpendicular to its Euler axis, so dragging it turns that
   * axis. The ring's local X gives the angle reference; the target property is the row's Euler
   * component. */
  float rot[3][3];
  eul_to_mat3(rot, frame.rotation);
  const float max_size = std::max(
      0.1f, std::max(ggd->base_size[0], std::max(ggd->base_size[1], ggd->base_size[2])));
  for (int axis = 0; axis < 3; axis++) {
    wmGizmo *ring = ggd->gz_rot[axis];
    if (ring == nullptr) {
      continue;
    }
    const int x_axis = (axis + 1) % 3;
    const int y_axis = (axis + 2) % 3;
    float basis[4][4];
    unit_m4(basis);
    copy_v3_v3(basis[0], rot[x_axis]);
    copy_v3_v3(basis[1], rot[y_axis]);
    copy_v3_v3(basis[2], rot[axis]);
    copy_v3_v3(basis[3], frame.center);
    mul_m4_m4m4(ring->matrix_basis, ob->object_to_world().ptr(), basis);
    const float radius = max_size * (0.72f + 0.08f * float(axis));
    unit_m4(ring->matrix_offset);
    for (int i = 0; i < 3; i++) {
      ring->matrix_offset[i][i] = radius;
    }
  }
  return true;
}

void projection_gizmo_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  projection_gizmo_place(C, gzgroup);
}

void projection_gizmo_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  projection_gizmo_place(C, gzgroup);
}

}  // namespace

void projection_gizmo_type_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Paint Layer Projection";
  gzgt->idname = "VIEW3D_GGT_paint_layer_projection";

  /* A box cage needs the 3D hit test (`gizmo_window_project_3d`) but not depth testing: a handle
   * the surface hides must still be reachable. */
  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL |
               WM_GIZMOGROUPTYPE_3D;
  gzgt->poll = projection_gizmo_poll;
  gzgt->setup = projection_gizmo_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = projection_gizmo_refresh;
  gzgt->draw_prepare = projection_gizmo_draw_prepare;
}

}  // namespace blender::ed::sculpt_paint::projection

namespace blender {

void ED_view3d_projection_gizmo_setup(wmGizmoGroupType *gzgt)
{
  ed::sculpt_paint::projection::projection_gizmo_type_setup(gzgt);
}

}  // namespace blender
