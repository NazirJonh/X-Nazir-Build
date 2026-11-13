/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The live UV <-> Decal switch of the Transform Selection tools. Lives apart from the shared
 * floating session (`paint_image_select_floating.cc`), which must not know the concrete tools: this
 * is the one place that reaches into Move, Transform and the 3D Viewport Transform.
 */

#include "BLI_listbase.h"

#include "BKE_context.hh"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_paint.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_image_select_intern.hh"
#include "paint_image_select_move_intern.hh"
#include "paint_image_select_preview_view3d.hh"
#include "paint_image_select_transform_intern.hh"
#include "paint_image_select_view3d.hh"

namespace blender {

/**
 * Run \a sima's session's own live switch toward \a want_decal and refresh what shows it. This
 * runs from the property update or from the end of a gesture, never from a draw callback, so the
 * preview rebuild may free GPU resources.
 * \return true when the session switched.
 */
static bool image_editor_live_decal_apply(bContext *C, SpaceImage *sima, const bool want_decal)
{
  PaintSelectFloatingSession *session = image_select_session_active(sima);
  if (session == nullptr) {
    return false;
  }
  bool changed = false;
  if (session->tool == PaintSelectTool::Transform) {
    changed = image_select_transform_live_decal_set(C, sima, want_decal);
  }
  else if (session->tool == PaintSelectTool::Move) {
    changed = image_select_move_live_decal_set(C, sima, want_decal);
  }
  if (changed) {
    /* The Viewports show the preview the switch rebuilt. */
    if (const PaintSelectFloatingSession *live = image_select_session_active(sima)) {
      ed::sculpt_paint::image_select_preview_view3d_tag_redraw(C, *live);
    }
    /* Redraws every region of the Image Editors (the 2D preview draws the quad or the patch). */
    WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
  }
  return changed;
}

void image_select_floating_pending_mode_apply(bContext *C, SpaceImage *sima)
{
  PaintSelectFloatingSession *session = image_select_session_active(sima);
  if (C == nullptr || session == nullptr || session->pending_decal < 0) {
    return;
  }
  const bool want_decal = session->pending_decal == 1;
  /* Cleared first: the live switch re-stores it when a gesture still runs. */
  session->pending_decal = -1;
  image_editor_live_decal_apply(C, sima, want_decal);
}

void ed::sculpt_paint::image_select_view3d_pending_mode_apply(bContext *C)
{
  ed::sculpt_paint::PaintSelectView3DFloatingSession *session =
      ed::sculpt_paint::image_select_view3d_session_active();
  if (C == nullptr || session == nullptr || session->pending_decal < 0 || session->is_dragging) {
    return;
  }
  const bool want_decal = session->pending_decal == 1;
  session->pending_decal = -1;
  /* Each switch acts only on a live session of its own tool. */
  ed::sculpt_paint::image_select_transform3d_mode_changed(C, want_decal);
  ed::sculpt_paint::image_select_move3d_mode_changed(C, want_decal);
}

void ED_image_paint_select_transform_mode_changed(bContext *C)
{
  if (C == nullptr) {
    return;
  }
  const Scene *scene = CTX_data_scene(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  if (scene == nullptr || scene->toolsettings == nullptr || wm == nullptr) {
    return;
  }
  const bool want_decal = scene->toolsettings->imapaint.selection_transform_mode ==
                          IMAGE_PAINT_SELECT_TRANSFORM_DECAL;

  /* Every Image Editor with a live Move / Transform session. A running gesture defers its switch
   * (see #image_select_floating_pending_mode_apply). */
  for (wmWindow &win : wm->windows) {
    bScreen *screen = WM_window_get_active_screen(&win);
    if (screen == nullptr) {
      continue;
    }
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype != SPACE_IMAGE || area.spacedata.first == nullptr) {
        continue;
      }
      SpaceImage *sima = static_cast<SpaceImage *>(area.spacedata.first);
      image_editor_live_decal_apply(C, sima, want_decal);
    }
  }

  ed::sculpt_paint::image_select_transform3d_mode_changed(C, want_decal);
  ed::sculpt_paint::image_select_move3d_mode_changed(C, want_decal);
}

}  // namespace blender
