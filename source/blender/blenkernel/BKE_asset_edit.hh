/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

/**
 * Editing of datablocks from asset libraries.
 *
 * Asset blend files are linked into the global main database, with the asset
 * datablock itself and its dependencies. These datablocks remain linked but
 * are marked as editable.
 *
 * User edited asset datablocks are written to individual blend files per
 * asset. These blend files include any datablock dependencies and packaged
 * image files.
 *
 * This way the blend file can be easily saved, reloaded and deleted.
 *
 * This mechanism is currently only used for brush assets.
 */

#include <optional>
#include <string>

#include "BLI_string_ref.hh"

#include "DNA_ID_enums.h"

namespace blender {

struct bUserAssetLibrary;
struct AssetWeakReference;
struct ID;
struct Main;
struct ReportList;

namespace bke {

/** Get datablock from weak reference, loading the blend file as needed. */
ID *asset_edit_id_from_weak_reference(Main &global_main,
                                      ID_Type id_type,
                                      const AssetWeakReference &weak_ref);

/** Get asset weak reference from ID. */
std::optional<AssetWeakReference> asset_edit_weak_reference_from_id(const ID &id);

/**
 * Map a local asset weak reference (#ASSET_LIBRARY_LOCAL) to the weak reference of the source
 * asset it was made local from, using the datablock's stored library weak reference. Returns
 * nullopt when the reference is not local, the datablock is missing, or it has no source asset.
 *
 * Used so a datablock localized from an asset (e.g. a brush localized to receive a texture) is
 * still recognized as that source asset, for active-asset highlighting in the asset shelf.
 */
std::optional<AssetWeakReference> asset_edit_local_to_source_weak_reference(
    Main &global_main, const AssetWeakReference &local_weak_ref);

/** Asset editing operations. */

bool asset_edit_id_is_editable(const ID &id);
bool asset_edit_id_is_writable(const ID &id);

std::optional<std::string> asset_edit_id_save_as(Main &global_main,
                                                 const ID &id,
                                                 StringRefNull name,
                                                 const bUserAssetLibrary &user_library,
                                                 AssetWeakReference &r_weak_ref,
                                                 ReportList &reports);

bool asset_edit_id_save(Main &global_main, const ID &id, ReportList &reports);
/**
 * Relink the asset from the library. This causes the ID to be re-allocated, so its address
 * changes. Even in case of failure to reload the asset, \a id will be deleted.
 * \return the new address of the reloaded \a id.
 */
ID *asset_edit_id_revert(Main &global_main, ID &id, ReportList &reports);
bool asset_edit_id_delete(Main &global_main, ID &id, ReportList &reports);

/** Find a local copy of the asset. */
ID *asset_edit_id_find_local(Main &global_main, ID &id);
/**
 * Ensure a local copy of the asset and its direct and indirect dependencies exists. Dependencies
 * where #ID_TYPE_SUPPORTS_ASSET_EDITABLE() fails will not be made local and will be cleared.
 */
ID *asset_edit_id_ensure_local(Main &global_main, ID &id);

}  // namespace bke
}  // namespace blender
