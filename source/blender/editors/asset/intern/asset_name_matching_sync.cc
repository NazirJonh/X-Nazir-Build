/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "ED_asset_name_matching.hh"

#include "BKE_main.hh"
#include "BKE_name_matching.hh"

#include "BLI_listbase.h"
#include "BLI_name_matching.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "ED_asset_shelf.hh"
#include "ED_image_grid.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include <cstddef>
#include <string>

namespace blender::ed::asset {

/**
 * Apply the rename/removal to one host's #AssetNameMatchIdLink list. Guarded like every other
 * traversal of these lists: an implausible head is left untouched rather than dereferenced.
 */
static bool name_match_id_list_replace(ListBaseT<AssetNameMatchIdLink> &list,
                                       const StringRef old_id,
                                       const StringRef new_id)
{
  if (!BLI_listbase_head_is_plausible(&list)) {
    return false;
  }
  bool changed = false;
  const std::string new_id_str = new_id;
  for (AssetNameMatchIdLink &link : list.items_mutable()) {
    if (old_id != link.id) {
      continue;
    }
    changed = true;
    if (new_id.is_empty()) {
      BLI_freelinkN(&list, &link);
      continue;
    }
    /* The target may already be selected (rename onto an existing selection): keep one link. */
    if (BLI_findstring(&list, new_id_str.c_str(), offsetof(AssetNameMatchIdLink, id)) != nullptr) {
      BLI_freelinkN(&list, &link);
      continue;
    }
    STRNCPY_UTF8(link.id, new_id_str.c_str());
  }
  return changed;
}

static void name_match_id_set_replace(Set<std::string> &ids,
                                      const StringRef old_id,
                                      const StringRef new_id)
{
  if (!ids.remove_as(old_id)) {
    return;
  }
  if (!new_id.is_empty()) {
    ids.add(std::string(new_id));
  }
}

void name_match_map_type_id_replace(Main &bmain, const StringRef old_id, const StringRef new_id)
{
  if (old_id.is_empty() || old_id == new_id) {
    return;
  }

  bool file_browser_changed = false;

  for (bScreen &screen : bmain.screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype == SPACE_VIEW3D) {
          View3D &v3d = reinterpret_cast<View3D &>(sl);
          for (ImageGridSlotDNA *slot : {&v3d.image_grid, &v3d.image_grid_mask}) {
            name_match_id_list_replace(slot->filter_name_match_map_types, old_id, new_id);
          }
          /* The runtime state is seeded from DNA once and never re-synced, so an open grid would
           * keep filtering by the old ID until the file is reloaded (see
           * #image_grid_foreach_live_library_ref for the same reasoning). */
          image_grid::image_grid_foreach_live_name_match_ids(
              image_grid::ImageGridOwner::from(v3d), [&](Set<std::string> &ids) {
                name_match_id_set_replace(ids, old_id, new_id);
              });
        }

        if (sl.spacetype == SPACE_FILE) {
          SpaceFile &sfile = reinterpret_cast<SpaceFile &>(sl);
          if (sfile.browse_mode == FILE_BROWSE_MODE_ASSETS && sfile.asset_params != nullptr) {
            if (name_match_id_list_replace(
                    sfile.asset_params->filter_name_match_map_types, old_id, new_id))
            {
              file_browser_changed = true;
            }
          }
        }

        /* Mirrors #foreach_library_reference()'s walk: the active space's regions live on
         * #ScrArea, every other space's on its own #SpaceLink. */
        ListBaseT<ARegion> *regionbase = (&sl == area.spacedata.first) ? &area.regionbase :
                                                                        &sl.regionbase;
        for (ARegion &region : *regionbase) {
          if (region.regiontype != RGN_TYPE_ASSET_SHELF) {
            continue;
          }
          RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(
              region);
          if (!shelf_regiondata) {
            continue;
          }
          for (AssetShelf &shelf : shelf_regiondata->shelves) {
            name_match_id_list_replace(
                shelf.settings.filter_name_match_map_types, old_id, new_id);
          }
        }
      }
    }
  }

  shelf::popup_shelves_foreach_settings([&](AssetShelfSettings &settings) {
    name_match_id_list_replace(settings.filter_name_match_map_types, old_id, new_id);
  });

  /* Prefs rename/remove only sends #NC_WINDOW (redraw). Shelves read DNA when filtering, but the
   * Asset Browser pushes name-match state into the filelist during #file_refresh, which needs
   * #ND_SPACE_FILE_PARAMS. */
  if (file_browser_changed) {
    WM_main_add_notifier(NC_SPACE | ND_SPACE_FILE_PARAMS, nullptr);
  }
}

std::string ED_asset_name_matching_guess_map_type_identifier(const UserDef &userdef,
                                                             const StringRef filename)
{
  const std::string normalized = BLI_name_matching_normalize_asset_name(filename);
  if (normalized.empty()) {
    return {};
  }

  const bUserNameMatchMapType *matched = nullptr;
  for (const bUserNameMatchMapType &map_type : userdef.name_match_map_types) {
    /* Collect tokens for this map type. */
    Vector<StringRef> tokens;
    for (const bUserNameMatchToken &token : map_type.tokens) {
      tokens.append(token.value);
    }
    if (tokens.is_empty()) {
      continue;
    }
    if (BLI_name_matching_map_type_matches_name(normalized, tokens.as_span())) {
      if (matched != nullptr) {
        /* More than one map type matches — ambiguous, return nothing. */
        return {};
      }
      matched = &map_type;
    }
  }
  return matched ? std::string(matched->identifier) : std::string{};
}

}  // namespace blender::ed::asset
