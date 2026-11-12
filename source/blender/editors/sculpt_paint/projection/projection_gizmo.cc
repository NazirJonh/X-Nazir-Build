/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D Viewport gizmo for the inline frame of a projected paint-layer row: a box cage in the space
 * the frame's values are stored in -- the controller Empty's when the row names one, else the
 * painted object's -- plus a roll ring. Moving and scaling the cage writes the row's center and
 * size; the ring writes its roll around the projection direction. The Empty's own gizmo moves, turns and
 * scales the whole projection, while this one keeps editing the inline extents inside its space.
 */

#include "MEM_guardedalloc.h"

#include <algorithm>
#include <cmath>

#include "BLI_math_matrix.h"
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

struct ProjectionGizmoGroup {
  wmGizmo *gz_cage = nullptr;
  /** The roll ring, in the frame's XY plane around its +Z axis. */
  wmGizmo *gz_roll = nullptr;
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
  return modal == ggd->gz_cage || (ggd->gz_roll != nullptr && modal == ggd->gz_roll);
}

/**
 * The frame's placement at unit scale: the basis the generated graph and the CPU composite use
 * (#BKE_paint_layers_projection_frame_matrix) with the center as translation.
 */
void projection_frame_matrix(const ProjectionFrame &frame, float r_matrix[4][4])
{
  float rot[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot);
  copy_m4_m3(r_matrix, rot);
  copy_v3_v3(r_matrix[3], frame.center);
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
    projection_frame_matrix(frame, matrix);
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
  BKE_paint_layers_projection_frame_from_matrix(rot, frame.normal, &frame.roll);

  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

void projection_roll_get(const wmGizmo * /*gz*/, wmGizmoProperty *gz_prop, void *value_p)
{
  ProjectionGizmoGroup *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  ProjectionFrame frame;
  *static_cast<float *>(value_p) = (ggd != nullptr && ggd->target.read(frame)) ? frame.roll : 0.0f;
}

void projection_roll_set(const wmGizmo * /*gz*/, wmGizmoProperty *gz_prop, const void *value_p)
{
  ProjectionGizmoGroup *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd != nullptr && ggd->target.write_roll(*static_cast<const float *>(value_p))) {
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
  wmGizmo *ring = WM_gizmo_new_ptr(gzt_dial, gzgroup, nullptr);
  if (ring == nullptr) {
    return;
  }
  ggd->gz_roll = ring;
  RNA_enum_set(ring->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_ANGLE_VALUE);
  RNA_boolean_set(ring->ptr, "wrap_angle", true);
  /* The ring is sized by its matrix in world units, not by the screen-space gizmo size. */
  ring->flag |= WM_GIZMO_DRAW_NO_SCALE;
  ui::theme::get_color_3fv(TH_AXIS_Z, ring->color);
  ui::theme::get_color_3fv(TH_AXIS_Z, ring->color_hi);
  ring->color[3] = 1.0f;
  ring->color_hi[3] = 1.0f;
  WM_gizmo_set_flag(ring, WM_GIZMO_NEEDS_UNDO, true);
  wmGizmoPropertyFnParams ring_params{};
  ring_params.value_get_fn = projection_roll_get;
  ring_params.value_set_fn = projection_roll_set;
  ring_params.user_data = ggd;
  WM_gizmo_target_property_def_func(ring, "offset", &ring_params);
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
    if (ggd->gz_roll != nullptr) {
      ggd->gz_roll->flag |= WM_GIZMO_HIDDEN;
    }
    return false;
  }
  ggd->gz_cage->flag &= ~WM_GIZMO_HIDDEN;
  if (ggd->gz_roll != nullptr) {
    ggd->gz_roll->flag &= ~WM_GIZMO_HIDDEN;
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
  projection_frame_matrix(frame, ggd->gz_cage->matrix_offset);

  /* The roll ring lies in the frame's XY plane, so dragging it turns the frame around its +Z axis;
   * the target property is the row's roll. */
  if (ggd->gz_roll != nullptr) {
    float basis[4][4];
    projection_frame_matrix(frame, basis);
    mul_m4_m4m4(ggd->gz_roll->matrix_basis, ob->object_to_world().ptr(), basis);
    const float max_size = std::max(
        0.1f, std::max(ggd->base_size[0], std::max(ggd->base_size[1], ggd->base_size[2])));
    const float radius = max_size * 0.72f;
    unit_m4(ggd->gz_roll->matrix_offset);
    for (int i = 0; i < 3; i++) {
      ggd->gz_roll->matrix_offset[i][i] = radius;
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
