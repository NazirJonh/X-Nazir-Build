/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Keeps the name-match map-type IDs stored by the grid hosts in sync with the Preferences.
 */

#pragma once

#include "DNA_listBase.h"
#include "DNA_asset_types.h"

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "BKE_name_matching.hh"

#include <string>

struct BlendDataReader;
struct BlendWriter;
struct bContext;
struct FileAssetSelectParams;
struct Main;
struct UserDef;

namespace blender::ed::asset {

/**
 * Re-point (\a new_id non-empty) or drop (\a new_id empty) every stored selection of the map type
 * \a old_id, in the asset shelves, Asset Browser (#FileAssetSelectParams), and image grids of
 * \a bmain, the popup shelves and the live runtime grid state.
 *
 * Without this, removing or renaming a map type in the Preferences leaves hosts holding an ID that
 * no longer resolves: #BKE_name_match_filter_resolve skips it, and a selection that resolves to
 * nothing silently shows *all* assets, which reads as the filter turning itself off.
 *
 * \note The Generic Grid API's IDProperty storage is owned by arbitrary (Python) callers and is not
 * reachable from #Main, so it keeps the resolve-time behavior. Only hosts that Blender itself owns
 * are pruned here.
 */
void name_match_map_type_id_replace(Main &bmain, StringRef old_id, StringRef new_id);

/**
 * Guess the map-type identifier from a filename by testing each map type's tokens against the
 * normalized asset name. Returns the #bUserNameMatchMapType::identifier of the single matching
 * map type, or an empty string when zero or more than one map types match.
 *
 * Intended for prefilling the per-file map-type identifier in the image-import-mark dialog so users
 * don't have to assign obvious matches by hand.
 */
std::string ED_asset_name_matching_guess_map_type_identifier(const UserDef &userdef,
                                                             StringRef filename);

void ED_asset_browser_name_match_map_types_free(ListBaseT<AssetNameMatchIdLink> &list);
void ED_asset_browser_name_match_map_types_copy(ListBaseT<AssetNameMatchIdLink> &dst,
                                                const ListBaseT<AssetNameMatchIdLink> &src);
void ED_asset_browser_name_match_map_types_blend_write(BlendWriter *writer,
                                                       const ListBaseT<AssetNameMatchIdLink> &list);
void ED_asset_browser_name_match_map_types_blend_read(BlendDataReader *reader,
                                                      ListBaseT<AssetNameMatchIdLink> &list);

void ED_asset_browser_name_match_notify(const bContext *C);

bool ED_asset_browser_name_match_filter_enabled(const FileAssetSelectParams &params);
void ED_asset_browser_name_match_filter_set_enabled(FileAssetSelectParams &params, bool enabled);
void ED_asset_browser_name_match_map_type_toggle(FileAssetSelectParams &params,
                                                 StringRef identifier);
void ED_asset_browser_name_match_clear_selection(FileAssetSelectParams &params);

/**
 * Asset Browser filelist visibility for one entry after catalog/online checks.
 * Only Images are affected by name matching; non-Image entries always pass through, even while
 * the filter is enabled.
 * \return true if the entry may remain visible (other filters still AND later).
 */
bool ED_asset_browser_name_match_entry_visible(bool filter_enabled,
                                               bool is_image_asset,
                                               bool stored_map_type_ids_nonempty,
                                               const NameMatchResolvedFilter &resolved,
                                               StringRef asset_name,
                                               Span<StringRef> metadata_tag_names);

void ED_asset_browser_name_match_panel_register();

}  // namespace blender::ed::asset
