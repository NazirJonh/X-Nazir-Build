/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * The periodic timer of the automatic asset updates. A global #BLI_timer callback: it fires from
 * #wm_event_do_notifiers() on every main-loop iteration, independent of open windows or spaces,
 * so the feature keeps working while the user only paints in the 3D View. Registered as
 * persistent so file loads keep it alive (a file load drops all non-persistent timers); it reads
 * the current main database and settings on every fire, so it needs no per-database state. Use
 * #timer_sync() to register, re-arm or drop it.
 */

#include "ED_asset_updates.hh"

#include <cstdint>

#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_global.hh"
#include "BKE_main.hh"

#include "BLI_timer.h"

#include "asset_updates_intern.hh"

namespace blender::ed::asset_updates {

static uintptr_t updates_timer_uuid()
{
  static const char uuid = 0;
  return uintptr_t(&uuid);
}

static double updates_timer_callback(uintptr_t /*uuid*/, void * /*user_data*/)
{
  const UpdatesSettings settings = settings_from_userdef(U);
  if (!settings.refresh_assets_panel && !settings.update_project_resources) {
    /* Disabled (programmatically -- the preference update callback normally unregisters the
     * timer already). A negative return value removes the timer. */
    state().timer_interval = 0.0;
    return -1.0;
  }

  const Main *bmain = G_MAIN;
  wmWindowManager *wm = bmain ? static_cast<wmWindowManager *>(bmain->wm.first) : nullptr;
  if (wm != nullptr && !scan_job_running(*wm)) {
    if (settings.refresh_assets_panel) {
      refresh_visible_asset_browsers(*wm);
    }
    if (settings.update_project_resources) {
      scan_job_start(*wm, static_cast<wmWindow *>(wm->windows.first), ScanMode::Automatic);
    }
  }
  /* When a scan is still running, this tick is skipped and retried at the next interval. */
  return double(settings.interval_min) * 60.0;
}

void timer_unregister()
{
  BLI_timer_unregister(updates_timer_uuid());
  state().timer_interval = 0.0;
}

void timer_sync()
{
  const UpdatesSettings settings = settings_from_userdef(U);
  const bool wanted = settings.refresh_assets_panel || settings.update_project_resources;
  const double interval = double(settings.interval_min) * 60.0;

  if (!wanted) {
    timer_unregister();
    return;
  }
  if (BLI_timer_is_registered(updates_timer_uuid())) {
    if (state().timer_interval == interval) {
      return;
    }
    /* The interval changed: re-arm the timer so it applies to the next run. */
    timer_unregister();
  }
  BLI_timer_register(updates_timer_uuid(),
                     updates_timer_callback,
                     nullptr,
                     nullptr,
                     interval,
                     /*persistent=*/true);
  state().timer_interval = interval;
}

}  // namespace blender::ed::asset_updates
