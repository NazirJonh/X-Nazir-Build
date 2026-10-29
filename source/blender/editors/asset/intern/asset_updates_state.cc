/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Lifetime of the automatic asset updates module: the settings snapshot and the runtime state
 * shared by the scan job, the timer and the UI. See #ED_asset_updates.hh for the general design.
 *
 * The state owns guarded-allocator containers, so it is created lazily and dropped deliberately
 * in #storage_exit() instead of being a file-scope static that would outlive #MEM_exit().
 */

#include "ED_asset_updates.hh"

#include <memory>

#include "DNA_userdef_types.h"

#include "asset_updates_intern.hh"

namespace blender::ed::asset_updates {

namespace {

/** Created on first use, freed by #storage_exit(). */
std::unique_ptr<ModuleState> module_state;

}  // namespace

ModuleState &state()
{
  if (!module_state) {
    module_state = std::make_unique<ModuleState>();
  }
  return *module_state;
}

UpdatesSettings settings_from_userdef(const UserDef &userdef)
{
  UpdatesSettings settings;
  settings.refresh_assets_panel = userdef.asset_updates_assets_panel != 0;
  settings.update_project_resources = userdef.asset_updates_project_resources != 0;
  settings.interval_min = userdef.asset_updates_interval_min > 0 ?
                              userdef.asset_updates_interval_min :
                              1;
  settings.skip_mismatch = userdef.asset_updates_skip_mismatch != 0;
  return settings;
}

void storage_exit()
{
  /* The global timer list is freed with #BLI_timer_free(), but unregistering here keeps the
   * module self-contained. */
  timer_unregister();
  module_state.reset();
}

}  // namespace blender::ed::asset_updates
