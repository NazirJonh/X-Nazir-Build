/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Operators and status reporting of the automatic asset updates. The popover UI itself
 * (#FILE_PT_asset_updates) is drawn by Python in bl_ui/space_filebrowser.py, which reads the
 * preferences properties and the last check status getters below (exposed to Python through the
 * `AssetUpdatesStatus` RNA struct, see `bpy.context.window_manager.asset_updates`).
 */

#include "ED_asset_updates.hh"

#include <cstddef>

#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_vector.hh"

#include "ED_asset_list.hh"
#include "ED_fileselect.hh"

#include "WM_api.hh"

#include "BLT_translation.hh"

#include "asset_library_reference.hh"
#include "asset_updates_intern.hh"

namespace blender::ed::asset_updates {

/* -------------------------------------------------------------------- */
/** \name Asset panel refresh
 * \{ */

void refresh_visible_asset_browsers(wmWindowManager &wm)
{
  Vector<AssetLibraryReference> refs;
  for (wmWindow &win : wm.windows) {
    const bScreen *screen = WM_window_get_active_screen(&win);
    if (screen == nullptr) {
      continue;
    }
    for (const ScrArea &area : screen->areabase) {
      if (area.spacetype != SPACE_FILE) {
        continue;
      }
      const SpaceFile *sfile = static_cast<const SpaceFile *>(area.spacedata.first);
      if (sfile == nullptr || !ED_fileselect_is_asset_browser(sfile)) {
        continue;
      }
      const FileAssetSelectParams *params = ED_fileselect_get_asset_params(sfile);
      if (params == nullptr) {
        continue;
      }
      if (!refs.contains(params->asset_library_ref)) {
        refs.append(params->asset_library_ref);
      }
    }
  }

  for (const AssetLibraryReference &ref : refs) {
    /* Same as #ASSET_OT_library_refresh: update the on-disk image index of custom libraries and
     * clear the stored lists so the next read job picks up the changes. */
    ed::asset::list::library_refresh_from_disk(ref, &wm);
  }

  if (!refs.is_empty()) {
    WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST_READING, nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Manual update operators
 * \{ */

static bool updates_operator_poll(bContext *C)
{
  const SpaceFile *sfile = CTX_wm_space_file(C);
  return sfile != nullptr && ED_fileselect_is_asset_browser(sfile);
}

static wmOperatorStatus updates_assets_panel_exec(bContext *C, wmOperator * /*op*/)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return OPERATOR_CANCELLED;
  }
  refresh_visible_asset_browsers(*wm);
  return OPERATOR_FINISHED;
}

static void ASSET_OT_updates_panel_refresh(wmOperatorType *ot)
{
  ot->name = "Update Assets Panel";
  ot->idname = "ASSET_OT_updates_panel_refresh";
  ot->description = "Check asset files for changes and update them in the Assets panel";
  ot->poll = updates_operator_poll;
  ot->exec = updates_assets_panel_exec;
}

static wmOperatorStatus updates_resources_exec(bContext *C, wmOperator *op)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (scan_job_running(*wm)) {
    BKE_report(op->reports, RPT_WARNING, "An update check is already running");
    return OPERATOR_CANCELLED;
  }
  if (scan_job_start(*wm, CTX_wm_window(C), ScanMode::Manual)) {
    /* The full hashes (and any reloads) are reported once the background job finishes. */
    BKE_report(op->reports, RPT_INFO, "Checking the external files of the project resources");
  }
  else {
    BKE_report(op->reports, RPT_INFO, "No external project resource files to check");
  }
  return OPERATOR_FINISHED;
}

static void ASSET_OT_updates_resources_refresh(wmOperatorType *ot)
{
  ot->name = "Update Resources Used in Project";
  ot->idname = "ASSET_OT_updates_resources_refresh";
  ot->description =
      "Check the external files of resources used in the project (layer stack included) for "
      "changes, and reload them";
  /* No poll: the check works on the whole database, so the operator stays available outside the
   * asset browser too (e.g. for custom keymaps or menus). */
  ot->exec = updates_resources_exec;
}

void operatortypes()
{
  WM_operatortype_append(ASSET_OT_updates_panel_refresh);
  WM_operatortype_append(ASSET_OT_updates_resources_refresh);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Last check status
 *
 * Human readable status of the last completed scan, read by the `AssetUpdatesStatus` RNA struct
 * that the #FILE_PT_asset_updates popover shows. The formatting stays here because the timestamps
 * are #BLI_time_now_seconds() values (a monotonic clock), which Python cannot interpret.
 * \{ */

void last_check_ago_get(char *r_text, size_t text_maxlen)
{
  const LastScanInfo &last_scan = state().last_scan;
  if (last_scan.timestamp <= 0.0) {
    BLI_strncpy(r_text, IFACE_("Not checked yet"), text_maxlen);
    return;
  }
  const double elapsed_min = (BLI_time_now_seconds() - last_scan.timestamp) / 60.0;
  if (elapsed_min < 1.0) {
    BLI_strncpy(r_text, IFACE_("Just now"), text_maxlen);
  }
  else {
    BLI_snprintf(r_text, text_maxlen, IFACE_("%d min ago"), int(elapsed_min));
  }
}

void last_check_summary_get(char *r_text, size_t text_maxlen)
{
  const LastScanInfo &last_scan = state().last_scan;
  if (last_scan.timestamp <= 0.0) {
    r_text[0] = '\0';
    return;
  }
  if (last_scan.deferred_num > 0) {
    /* Deferred images were left for the next automatic pass, not skipped by the user. */
    BLI_snprintf(r_text,
                 text_maxlen,
                 IFACE_("%d checked, %d reloaded, %d skipped, %d missing, %d deferred"),
                 last_scan.checked_num,
                 last_scan.reloaded_num,
                 last_scan.skipped_num,
                 last_scan.missing_num,
                 last_scan.deferred_num);
    return;
  }
  BLI_snprintf(r_text,
               text_maxlen,
               IFACE_("%d checked, %d reloaded, %d skipped, %d missing"),
               last_scan.checked_num,
               last_scan.reloaded_num,
               last_scan.skipped_num,
               last_scan.missing_num);
}

/** \} */

}  // namespace blender::ed::asset_updates
