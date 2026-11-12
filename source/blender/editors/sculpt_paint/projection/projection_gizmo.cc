/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D Viewport gizmo for the inline frame of a projected paint-layer row. The group carries the
 * plumbing -- poll, target, placement -- and picks a handle mode from the row's mapping space: the
 * planar frame handles for Planar rows, the volume handles for the rows that fill the
 * frame's extents (see #projection_gizmo_intern.hh). The frame's orientation is the row's
 * normal+roll; the handles read it through #BKE_paint_layers_projection_frame_matrix.
 */

#include <algorithm>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_paint_layers.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "ED_gizmo_library.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "projection_gizmo_intern.hh"

namespace blender::ed::sculpt_paint::projection {
namespace {

ProjectionGizmoGroup *projection_gizmo_group(wmGizmoGroup *gzgroup)
{
  return static_cast<ProjectionGizmoGroup *>(gzgroup->customdata);
}

/**
 * The handle sets the volume mode draws: the Projection Frame tool draws the Move / Rotate / Scale
 * sets the stored #Sculpt::projection_gizmo_set picks, and while any other tool is active the
 * handles step aside -- the cage alone keeps its place, like the planar frame does.
 */
int projection_gizmo_volume_show_mask_get(const bContext *C)
{
  if (!ED_paint_projection_frame_tool_is_active(C)) {
    return 0;
  }
  const Scene *scene = CTX_data_scene(C);
  const Sculpt *sculpt = (scene != nullptr && scene->toolsettings != nullptr) ?
                             scene->toolsettings->sculpt :
                             nullptr;
  if (sculpt == nullptr) {
    return 0;
  }
  switch (sculpt->projection_gizmo_set) {
    case SCULPT_PROJECTION_GIZMO_MOVE:
      return PROJECTION_GIZMO_SHOW_TRANSLATE;
    case SCULPT_PROJECTION_GIZMO_ROTATE:
      return PROJECTION_GIZMO_SHOW_ROTATE;
    case SCULPT_PROJECTION_GIZMO_SCALE:
      return PROJECTION_GIZMO_SHOW_SCALE;
    case SCULPT_PROJECTION_GIZMO_ALL:
    default:
      return PROJECTION_GIZMO_SHOW_ALL;
  }
}

/* -------------------------------------------------------------------- */
/** \name Poll, setup, place
 * \{ */

bool projection_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }
  const View3D *v3d = CTX_wm_view3d(C);
  if (v3d == nullptr || (v3d->gizmo_flag & V3D_GIZMO_HIDE) != 0) {
    return false;
  }
  /* The "Show Gizmo" toggle of the Mapping & Projection panel. */
  const Scene *scene = CTX_data_scene(C);
  const Sculpt *sculpt = (scene != nullptr && scene->toolsettings != nullptr) ?
                             scene->toolsettings->sculpt :
                             nullptr;
  if (sculpt != nullptr && sculpt->projection_gizmo_hide != 0) {
    return false;
  }
  PaintLayerRowProjectionTarget target;
  return target.resolve(*C);
}

void projection_gizmo_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  auto *ggd = MEM_new<ProjectionGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<ProjectionGizmoGroup *>(customdata));
  };

  projection_gizmo_planar_setup(gzgroup, *ggd);
  projection_gizmo_volume_setup(gzgroup, *ggd);
}

bool projection_gizmo_place(const bContext *C, wmGizmoGroup *gzgroup)
{
  auto *ggd = projection_gizmo_group(gzgroup);
  if (ggd == nullptr) {
    return false;
  }
  /* Placement keeps running during a drag so every handle follows the live frame; the mode's
   * place skips only the dragged handle that draws itself from its own drag state. */
  ggd->drag_gz = projection_gizmo_modal_get(C, ggd);
  if (ggd->drag_gz == nullptr) {
    /* No drag is in flight, so the uniform-scale annulus capture of the last one is stale. */
    ggd->uniform_drag_valid = false;
    ggd->volume_uniform = false;
  }
  ProjectionFrame frame;
  if (!ggd->target.resolve(*C) || !ggd->target.read(frame)) {
    projection_gizmo_handles_set_hidden(*ggd, true);
    return false;
  }
  Object *space_ob = ggd->target.space_object();
  if (space_ob == nullptr) {
    projection_gizmo_handles_set_hidden(*ggd, true);
    return false;
  }
  const MaterialPaintLayer *layer = BKE_paint_layers_find(*ggd->target.material(),
                                                         ggd->target.marker());
  if (layer == nullptr) {
    projection_gizmo_handles_set_hidden(*ggd, true);
    return false;
  }

  for (const int i : blender::IndexRange(3)) {
    ggd->base_size[i] = fabsf(frame.size[i]);
    ggd->base_sign[i] = frame.size[i] < 0.0f ? -1.0f : 1.0f;
  }
  float rot[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot);

  /* The mode's place un-hides only its own handles; everything else stays hidden from here. */
  projection_gizmo_handles_set_hidden(*ggd, true);
  if (projection_gizmo_mode_get(*layer) == PROJECTION_GIZMO_VOLUME) {
    /* The cage previews the volume the row projects through: a cylinder or a sphere, else the
     * box that Box, Triplanar and Cube share. */
    switch (BKE_paint_layers_projection_mode_info(layer->mapping.space).cage_style) {
      case PROJECTION_CAGE_CYLINDER:
        ggd->volume_cage_style = ED_GIZMO_CAGE3D_STYLE_CYLINDER;
        break;
      case PROJECTION_CAGE_SPHERE:
        ggd->volume_cage_style = ED_GIZMO_CAGE3D_STYLE_SPHERE;
        break;
      case PROJECTION_CAGE_BOX:
      default:
        ggd->volume_cage_style = ED_GIZMO_CAGE3D_STYLE_BOX;
        break;
    }
    ggd->volume_cage_axis = std::clamp(int(BKE_paint_layers_projection_axis_get(*layer)), 0, 2);
    return projection_gizmo_volume_place(
        C, *ggd, *space_ob, frame, rot, projection_gizmo_volume_show_mask_get(C));
  }
  return projection_gizmo_planar_place(C, *ggd, *space_ob, frame, rot);
}

void projection_gizmo_invoke_prepare(const bContext *C,
                                     wmGizmoGroup *gzgroup,
                                     wmGizmo *gz,
                                     const wmEvent *event)
{
  auto *ggd = projection_gizmo_group(gzgroup);
  if (ggd != nullptr) {
    /* The last placement ran with the frame the drag starts from. */
    copy_v3_v3(ggd->cage_size_init, ggd->base_size);
    copy_v3_v3(ggd->cage_sign_init, ggd->base_sign);
    ggd->cage_surface_init = ggd->surface_height;
    projection_gizmo_volume_invoke_prepare(C, *ggd, gz, event);
  }
}

void projection_gizmo_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  projection_gizmo_place(C, gzgroup);
}

void projection_gizmo_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  projection_gizmo_place(C, gzgroup);
}

/** \} */

}  // namespace

/** Tag the row's material so the viewport and the editors re-read the frame values. */
void projection_target_notify(Material *ma)
{
  if (ma == nullptr) {
    return;
  }
  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

wmGizmo *projection_gizmo_modal_get(const bContext *C, const ProjectionGizmoGroup *ggd)
{
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->runtime == nullptr || region->runtime->gizmo_map == nullptr) {
    return nullptr;
  }
  wmGizmo *modal = WM_gizmomap_get_modal(region->runtime->gizmo_map);
  if (modal == nullptr) {
    return nullptr;
  }
  if (ELEM(modal,
           ggd->gz_cage,
           ggd->gz_roll,
           ggd->gz_depth,
           ggd->gz_volume_cage,
           ggd->gz_move_center,
           ggd->gz_rot_c,
           ggd->gz_scale_uniform))
  {
    return modal;
  }
  for (const int i : blender::IndexRange(3)) {
    if (ELEM(modal,
             ggd->gz_move_axis[i],
             ggd->gz_move_plane[i],
             ggd->gz_rot_axis[i],
             ggd->gz_scale_axis[i],
             ggd->gz_scale_plane[i]))
    {
      return modal;
    }
  }
  return nullptr;
}

void projection_gizmo_handles_set_hidden(ProjectionGizmoGroup &ggd, const bool hidden)
{
  const auto set_hidden = [hidden](wmGizmo *gz) {
    if (gz != nullptr) {
      SET_FLAG_FROM_TEST(gz->flag, hidden, WM_GIZMO_HIDDEN);
    }
  };
  set_hidden(ggd.gz_cage);
  set_hidden(ggd.gz_roll);
  set_hidden(ggd.gz_depth);
  set_hidden(ggd.gz_direction);
  set_hidden(ggd.gz_move);
  set_hidden(ggd.gz_volume_cage);
  set_hidden(ggd.gz_move_center);
  set_hidden(ggd.gz_rot_c);
  set_hidden(ggd.gz_scale_uniform);
  for (const int i : blender::IndexRange(3)) {
    set_hidden(ggd.gz_move_axis[i]);
    set_hidden(ggd.gz_move_plane[i]);
    set_hidden(ggd.gz_rot_axis[i]);
    set_hidden(ggd.gz_scale_axis[i]);
    set_hidden(ggd.gz_scale_plane[i]);
  }
}

ProjectionGizmoMode projection_gizmo_mode_get(const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_projection_mode_info(layer.mapping.space).is_volume ?
             PROJECTION_GIZMO_VOLUME :
             PROJECTION_GIZMO_PLANAR;
}

void projection_frame_basis(const float rot[3][3], const float origin[3], float r_mat[4][4])
{
  /* `rot[axis][component]`: row \a i of the 3x3 is frame axis \a i, which is also row \a i of a
   * Blender 4x4 (`mat[axis][component]`), so the rows copy straight across. */
  unit_m4(r_mat);
  for (const int i : IndexRange(3)) {
    for (const int j : IndexRange(3)) {
      r_mat[i][j] = rot[i][j];
    }
    r_mat[3][i] = origin[i];
  }
}

void projection_frame_axis(const float rot[3][3], const int i, float r_axis[3])
{
  copy_v3_v3(r_axis, rot[i]);
}

void projection_gizmo_type_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Paint Layer Projection";
  gzgt->idname = "VIEW3D_GGT_paint_layer_projection";
  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL |
               WM_GIZMOGROUPTYPE_3D;
  gzgt->poll = projection_gizmo_poll;
  gzgt->setup = projection_gizmo_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = projection_gizmo_refresh;
  gzgt->draw_prepare = projection_gizmo_draw_prepare;
  gzgt->invoke_prepare = projection_gizmo_invoke_prepare;
}

}  // namespace blender::ed::sculpt_paint::projection

namespace blender {

void ED_view3d_projection_gizmo_setup(wmGizmoGroupType *gzgt)
{
  ed::sculpt_paint::projection::projection_gizmo_type_setup(gzgt);
}

}  // namespace blender
