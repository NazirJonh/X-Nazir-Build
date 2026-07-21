/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Multi-viewport context for a live Curve Patch edit: keep the session's `ViewContext` pointed
 * at the viewport under the cursor, and attach the modal handler to windows opened after the
 * session started.
 */

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "ED_paint_curve_draw.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "mesh/sculpt_intern.hh"
#include "paint_curve_patch_edit_intern.hh"
#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint {

void curve_patch_sync_view_context(bContext *C,
                                   ScrArea *area,
                                   ARegion *region,
                                   CurvePatchSession &patch)
{
  if (!area || !region) {
    return;
  }
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, region);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  patch.view_context = ED_view3d_viewcontext_init(C, depsgraph);
  Object *ob = CTX_data_active_object(C);
  if (ob && ob->runtime->sculpt_session && ob->runtime->sculpt_session->cache) {
    ob->runtime->sculpt_session->cache->vc = &patch.view_context;
  }
}

/**
 * Called from outside the modal (the overlay-redraw poll) to catch windows opened after the
 * session started -- `curve_patch_edit_modal()` re-runs
 * #WM_event_add_modal_handler_all_windows() itself on every tick too, but this covers the gap
 * before the next event reaches it. Discovers the running #SCULPT_OT_curve_patch_edit instance
 * by type: there is always at most one active session, so a type-based search (unlike an
 * instance-match idempotency check) is the right tool here -- there is no already-known
 * `wmOperator *` to compare against yet.
 */
void ED_paint_curve_patch_modal_handlers_ensure(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (!ob || !ob->runtime->sculpt_session || !ob->runtime->sculpt_session->curve_patch_session) {
    return;
  }
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm) {
    return;
  }
  const wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_curve_patch_edit", false);
  if (!ot) {
    return;
  }
  wmOperator *op = nullptr;
  for (wmWindow &win : wm->windows) {
    op = WM_operator_find_modal_by_type(&win, ot);
    if (op) {
      break;
    }
  }
  if (!op) {
    return;
  }
  WM_event_add_modal_handler_all_windows(C, op, SPACE_VIEW3D, RGN_TYPE_WINDOW);
}

}  // namespace blender::ed::sculpt_paint
