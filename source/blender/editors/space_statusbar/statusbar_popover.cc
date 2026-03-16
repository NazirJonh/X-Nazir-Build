/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spstatusbar
 *
 * Popover panel for experimental build information.
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BKE_experimental_build.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Experimental Build Info Popover Panel
 * \{ */

/**
 * Draw function for the experimental build info popover panel.
 * This panel displays information about the custom build.
 * Uses the centralized BKE_experimental_build_info_get() API.
 */
static void statusbar_experimental_build_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  /* Get build info from centralized source */
  const ExperimentalBuildInfo *info = BKE_experimental_build_info_get();

  /* Main title with alert icon */
  layout.label(info->build_name, ICON_ERROR);

  /* Separator */
  layout.separator();

  /* Build creator information */
  std::string author_text = std::string("Created by: ") + info->author;
  layout.label(author_text, ICON_USER);

  /* Build date */
  if (info->build_date && info->build_date[0]) {
    std::string date_text = std::string("Build date: ") + info->build_date;
    layout.label(date_text, ICON_TIME);
  }

  /* Additional information section */
  layout.separator();
  layout.label(info->description, ICON_INFO);
  layout.label(info->warning_message, ICON_ERROR);

  /* Credits */
  if (info->credits && info->credits[0]) {
    layout.separator();
    layout.label(info->credits, ICON_HEART);
  }

  /* Future: Add more build-specific information here */
  /* Future: Add buttons, links, etc. */
}

/**
 * Poll function for the popover panel.
 * Returns true if the panel should be visible.
 */
static bool statusbar_experimental_build_panel_poll(const bContext * /*C*/, PanelType * /*pt*/)
{
  /* Always show this panel - it's non-toggleable */
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panel Registration
 * \{ */

/**
 * Register the experimental build info popover panel.
 * This must be called during space type initialization.
 *
 * \param art: The region type to register the panel with.
 */
void statusbar_experimental_build_panel_register(ARegionType *art)
{
  /* Check if panel type already exists in global registry.
   * Uses global paneltype registry to allow usage as popover. */
  if (WM_paneltype_find("STATUSBAR_PT_experimental_build", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>("statusbar experimental build panel");

  /* Panel identification */
  STRNCPY_UTF8(pt->idname, "STATUSBAR_PT_experimental_build");
  STRNCPY_UTF8(pt->label, N_("About Build"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Information about this experimental build");

  /* Panel callbacks */
  pt->draw = statusbar_experimental_build_panel_draw;
  pt->poll = statusbar_experimental_build_panel_poll;

  /* Panel settings */
  pt->flag = PANEL_TYPE_NO_HEADER;  /* No header for cleaner popover look */
  pt->ui_units_x = 20;  /* Width of the popover */

  /* Add to region type's panel list */
  BLI_addtail(&art->paneltypes, pt);

  /* Add to global panel type registry for popover access */
  WM_paneltype_add(pt);
}

/** \} */

}  // namespace blender
