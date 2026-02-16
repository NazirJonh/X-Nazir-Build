/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "BLI_vector.hh"

namespace blender {

/* Structs */
struct SpaceUserPref;
struct UserDef;
class bContext;

namespace ui {
class Layout;
}  // namespace ui

void ED_operatortypes_userpref();

Vector<int> ED_userpref_tabs_list(SpaceUserPref *prefs);
bool ED_userpref_tab_has_search_result(SpaceUserPref *sprefs, int index);
void ED_userpref_search_string_set(SpaceUserPref *sprefs, const char *value);
int ED_userpref_search_string_length(SpaceUserPref *sprefs);
const char *ED_userpref_search_string_get(SpaceUserPref *sprefs);

/**
 * Create the asset library tree-view in the given layout.
 * Used by Python template_asset_library_tree_view().
 */
namespace ed::userpref {
void userpref_create_asset_library_tree_view_in_layout(const bContext *C,
                                                        ui::Layout &layout,
                                                        UserDef *userdef);
}  // namespace ed::userpref

}  // namespace blender
