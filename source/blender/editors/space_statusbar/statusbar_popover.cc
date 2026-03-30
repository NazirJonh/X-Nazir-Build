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

#include "RNA_access.hh"

#include "wm.hh"

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
  if (info->description1 && info->description1[0]) {
    layout.label(info->description1, ICON_INFO);
  }
  if (info->description2 && info->description2[0]) {
    layout.label(info->description2, ICON_NONE);
  }
  if (info->warning_message1 && info->warning_message1[0]) {
    layout.label(info->warning_message1, ICON_ERROR);
  }
  if (info->warning_message2 && info->warning_message2[0]) {
    layout.label(info->warning_message2, ICON_NONE);
  }

  /* Credits */
  if (info->credits && info->credits[0]) {
    layout.separator();
    layout.label(info->credits, ICON_HEART);
  }

  /* Feedback button */
  layout.separator();
  
  /* Tutorial videos link */
  {
    ui::Layout &link_row = layout.row(false);
    link_row.alignment_set(ui::LayoutAlign::Left);
    link_row.label(RPT_("Watch tutorial videos:"), ICON_NONE);
    link_row.link("https://www.youtube.com/@XNazirBuild", "X-Nazir Build YouTube Channel", ICON_URL);
  }

  layout.separator();

  /* Create feedback button with URL operator using C++ Layout API */
  PointerRNA op_ptr = layout.op("WM_OT_url_open",
                                ">>>Support and Send FEEDBACK!<<<",
                                ICON_FUND,
                                wm::OpCallContext::InvokeDefault,
                                UI_ITEM_NONE);
  
  /* Set the URL property */
  RNA_string_set(&op_ptr, "url", "https://xnazirbuildfeedback.carrd.co/");
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
