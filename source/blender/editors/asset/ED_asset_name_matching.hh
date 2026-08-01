/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Keeps the name-match map-type IDs stored by the grid hosts in sync with the Preferences.
 */

#pragma once

#include "BLI_string_ref.hh"

struct Main;

namespace blender::ed::asset {

/**
 * Re-point (\a new_id non-empty) or drop (\a new_id empty) every stored selection of the map type
 * \a old_id, in the asset shelves and image grids of \a bmain, the popup shelves and the live
 * runtime grid state.
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

}  // namespace blender::ed::asset
