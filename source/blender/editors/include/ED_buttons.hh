/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include <optional>

#include "BLI_vector.hh"

#include "DNA_space_types.h"

namespace blender {

struct ARegion;
struct ScrArea;
struct SpaceProperties;
struct bContext;
struct PointerRNA;
struct PropertyRNA;

namespace ui {
struct Layout;
}  // namespace ui

/**
 * Fills an array with the tab context values for the properties editor. -1 signals a separator.
 *
 * \return The total number of items in the array returned.
 */
Vector<eSpaceButtons_Context> ED_buttons_tabs_list(const SpaceProperties *sbuts,
                                                   bool apply_filter = true);
void ED_buttons_visible_tabs_menu(bContext *C, ui::Layout *layout, void * /*arg*/);
void ED_buttons_navbar_menu(bContext *C, ui::Layout *layout, void * /*arg*/);
bool ED_buttons_tab_has_search_result(SpaceProperties *sbuts, int index);

void ED_buttons_search_string_set(SpaceProperties *sbuts, const char *value);
int ED_buttons_search_string_length(SpaceProperties *sbuts);
const char *ED_buttons_search_string_get(SpaceProperties *sbuts);

bool ED_buttons_should_sync_with_outliner(const bContext *C,
                                          const SpaceProperties *sbuts,
                                          ScrArea *area);
void ED_buttons_set_context(const bContext *C,
                            SpaceProperties *sbuts,
                            PointerRNA *ptr,
                            eSpaceButtons_Context context);

/**
 * Switch the (unpinned) Properties Editor to its Texture tab, with the texture of \a prop on
 * \a ptr selected as the active texture user. No-op when no Properties Editor can show it.
 *
 * This is what the "Show texture in texture tab" button (#uiTemplateTextureShow) does; exposed so
 * an operator that assigns a texture can bring it up the same way.
 */
void ED_buttons_texture_show(bContext *C, PointerRNA *ptr, PropertyRNA *prop);

/** Placeholder panel marking the drop position in the modifier stack. */
#define MODIFIER_DROP_GHOST_PANEL_IDNAME "MOD_PT_drop_ghost"

/**
 * While a Geometry Nodes modifier (local node group or asset) is dragged over \a region, return
 * the modifier stack index the drop would insert at (equal to the modifier count when appending).
 * Used by the modifier panels template to show a placeholder panel at the drop position.
 */
std::optional<int> ED_buttons_modifier_drop_insert_index(const ARegion *region);

}  // namespace blender
