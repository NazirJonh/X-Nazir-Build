/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * TEMPORARY: Sculpt/Paint brush-set filtering for the asset shelf popover.
 *
 * Classifies asset libraries (#bUserAssetLibrary.brush_set) and the Essentials library
 * (#UserDef.essentials_brush_set) as containing sculpt brushes, paint brushes, both or neither,
 * and filters the brush shelf's library selector and asset grid accordingly.
 *
 * The Sculpt/Paint display group the shelf filters for is the group the toolbar itself shows, taken
 * from the Tool System instead of being recomputed here. Three declarations decide it, in order
 * (see #resolve_category):
 *
 * - #bToolRef_Runtime::display_group, the group the toolbar declares the tool is drawn in. Set from
 *   the tool definition (`_tools_sculpt_display` / `_tools_paint_display` and add-on
 *   `bl_display_group`), so built-in and add-on tools are classified identically everywhere.
 * - #bToolRef_Runtime::brush_type for tools and add-ons that declare only a brush type.
 * - For tools shown in both groups the active tool cannot decide, so the group the user last picked
 *   with the toolbar's "Sculpt" / "Paint" buttons is used, stored per workspace in
 *   #bke::WorkSpaceRuntime::sculpt_paint_tool_display.
 *
 * Every one of these lives in the workspace (the tool list of a workspace is
 * #WorkSpace.tools), so two workspaces or windows never interfere.
 *
 * Everything here is meant to be removed wholesale once paint moves to Texture Paint Mode and
 * this stops being needed.
 *
 * Removal checklist:
 * - Delete this module (`.hh` and `.cc`), its test and the CMake entries.
 * - Delete the `ND_ASSET_SHELF_BRUSH_SET` notifier and its two listeners
 *   (#asset_shelf_region_listen, #asset_shelf_popover_listen).
 * - Delete the RNA properties `UserAssetLibrary.brush_set`,
 *   `PreferencesAssetLibraries.essentials_brush_set` and `WorkSpace.sculpt_paint_tool_display`,
 *   plus their UI and the `view3d.sculpt_paint_display_set` operator behind them.
 * - Delete the DNA fields (#eAssetLibraryBrushSet, `bUserAssetLibrary.brush_set`,
 *   `UserDef.essentials_brush_set`), their defaults and the matching versioning block.
 * - Delete #bke::eSculptPaintToolDisplay, #bke::WorkSpaceRuntime::sculpt_paint_tool_display and
 *   the `sculpt_paint_last_*_tool` strings.
 * - Delete #eToolDisplayGroup, #bToolRef_Runtime::display_group and the `display_group` argument of
 *   `WorkSpaceTool.setup()` (only this module and the toolbar use it).
 * - grep: `brush_set_temp`, `essentials_brush_set`, `ASSET_LIBRARY_BRUSH_SET_`,
 *   `required_brush_set`, `ND_ASSET_SHELF_BRUSH_SET`, `sculpt_paint_tool_display`, `display_group`.
 */

#pragma once

#include <cstdint>

#include "BLI_enum_flags.hh"

#include "DNA_workspace_types.h"

namespace blender {

struct AssetShelf;
struct AssetShelfSettings;
struct AssetLibraryReference;
struct UserDef;
struct bContext;
struct bUserAssetLibrary;

namespace asset_system {
class AssetRepresentation;
}  // namespace asset_system

namespace ed::asset::shelf::brush_set_temp {

/** Brush sets a library or a single brush can provide. Used as bit flags: an API may ask for one
 * set or for a combination, #Any (= no bit set) means "no filter". The values must match
 * #eAssetLibraryBrushSet, the persistent form of the same bits. */
enum class Category : int8_t {
  Any = 0,
  Sculpt = 1 << 0,
  Paint = 1 << 1,
};
ENUM_OPERATORS(Category);

/** The display group the brush shelf (#VIEW3D_AST_brush_sculpt) currently shows, or #Any for any
 * other shelf, object mode, or when neither the active tool nor the workspace state says it. */
Category category_from_shelf(const bContext &C, const AssetShelf &shelf);

/** #category_from_shelf() for callers where the context or the shelf may be null. Null-safe. */
Category required_for_shelf(const bContext *C, const AssetShelf *shelf);

/** Groups a brush of type \a sculpt_brush_type belongs to (#eBrushSculptType):
 * #Category::Sculpt or #Category::Paint for a brush exclusive to one display group, both bits for a
 * brush usable from either group, #Category::Any for a type that is not a sculpt brush type at all
 * (e.g. #bToolRef_Runtime::brush_type of a non-brush tool, which is 0 or -1). */
Category brush_type_categories(int sculpt_brush_type);

/** #Category bits a tool declares for the Sculpt/Paint split through
 * #bToolRef_Runtime::display_group: one bit for a group-exclusive tool, both bits for a tool shown
 * in both groups, #Category::Any when the tool declares nothing. */
Category tool_display_category(eToolDisplayGroup display_group);

/** The group to filter for, from the three declarations the Tool System and the workspace keep,
 * in priority order: \a declared_group wins while it names one group, then \a brush_type, then
 * \a stored_group (the user's last explicit choice, for tools shown in both groups). */
Category resolve_category(Category declared_group, int brush_type, Category stored_group);

/** True when \a library may be offered for \a required (#Category::Any = no filter; folders, being
 * containers rather than sources, always pass). */
bool library_passes(const bUserAssetLibrary &library, Category required);
bool essentials_passes(const UserDef &userdef, Category required);
/** Owner-library + per-asset classification check for an asset shown by a shelf filtering for
 * \a category. Covers the merged "All" library: each asset is filtered by the brush-set of the
 * library that owns it, then by its own Sculpt/Paint classification. */
bool asset_passes(const asset_system::AssetRepresentation &asset, Category category);

/** When the shelf's current library is not allowed for \a category, switch it to All (the All view
 * is filtered per asset, so the shelf never shows an empty or wrong set). */
void ensure_library_ref(AssetShelfSettings &settings, Category category);

}  // namespace ed::asset::shelf::brush_set_temp
}  // namespace blender
