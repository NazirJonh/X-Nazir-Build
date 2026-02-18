/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Share between interface_region_*.cc files.
 */

#pragma once

#include "BLI_string_ref.hh"

namespace blender {

struct ARegion;
struct bContext;
struct bScreen;

namespace ui {

/* interface_region_menu_popup.cc */

uint popup_menu_hash(StringRef str);

/* interface_regions.cc */

ARegion *region_temp_add(bScreen *screen);
void region_temp_remove(bContext *C, bScreen *screen, ARegion *region);

/* interface_region_tooltip.cc */

/**
 * Update text in an existing simple tooltip region without recreation.
 * Returns true if the tooltip was successfully updated.
 * Only works for tooltips with a single TIP_STYLE_NORMAL field.
 */
bool tooltip_region_update_text(ARegion *region, const char *text);

}  // namespace ui
}  // namespace blender
