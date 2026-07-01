/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <memory>
#include <optional>

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_time.h"

#include "AS_asset_library.hh"

#include "BKE_asset.hh"

#include "BLO_read_write.hh"

#include "DNA_asset_types.h"

#include "MEM_guardedalloc.h"

namespace blender {

/* #AssetWeakReference -------------------------------------------- */

AssetWeakReference::AssetWeakReference() = default;

AssetWeakReference::AssetWeakReference(const AssetWeakReference &other)
    : asset_library_type(other.asset_library_type),
      asset_library_identifier(BLI_strdup_null(other.asset_library_identifier)),
      relative_asset_identifier(BLI_strdup_null(other.relative_asset_identifier))
{
}

AssetWeakReference::AssetWeakReference(AssetWeakReference &&other)
    : asset_library_type(other.asset_library_type),
      asset_library_identifier(other.asset_library_identifier),
      relative_asset_identifier(other.relative_asset_identifier)
{
  other.asset_library_type = eAssetLibraryType{}; /* Not a valid type. */
  other.asset_library_identifier = nullptr;
  other.relative_asset_identifier = nullptr;
}

AssetWeakReference::~AssetWeakReference()
{
  MEM_delete(asset_library_identifier);
  MEM_delete(relative_asset_identifier);
}

AssetWeakReference &AssetWeakReference::operator=(const AssetWeakReference &other)
{
  if (this == &other) {
    return *this;
  }
  std::destroy_at(this);
  new (this) AssetWeakReference(other);
  return *this;
}

AssetWeakReference &AssetWeakReference::operator=(AssetWeakReference &&other)
{
  if (this == &other) {
    return *this;
  }
  std::destroy_at(this);
  new (this) AssetWeakReference(std::move(other));
  return *this;
}

bool operator==(const AssetWeakReference &a, const AssetWeakReference &b)
{
  if (a.asset_library_type != b.asset_library_type) {
    return false;
  }

  if (a.asset_library_type == ASSET_LIBRARY_CUSTOM) {
    const char *a_lib_idenfifier = a.asset_library_identifier ? a.asset_library_identifier : "";
    const char *b_lib_idenfifier = b.asset_library_identifier ? b.asset_library_identifier : "";
    if (BLI_path_cmp_normalized(a_lib_idenfifier, b_lib_idenfifier) != 0) {
      return false;
    }
  }
  const char *a_asset_idenfifier = a.relative_asset_identifier ? a.relative_asset_identifier : "";
  const char *b_asset_idenfifier = b.relative_asset_identifier ? b.relative_asset_identifier : "";
  if (BLI_path_cmp_normalized(a_asset_idenfifier, b_asset_idenfifier) != 0) {
    return false;
  }
  return true;
}

AssetWeakReference AssetWeakReference::make_reference(const asset_system::AssetLibrary &library,
                                                      const StringRef library_relative_identifier)
{
  AssetWeakReference weak_ref{};

  BLI_assert_msg(
      !(library.library_type() == ASSET_LIBRARY_CUSTOM && library.name().is_empty()),
      "Custom asset libraries should have a name set, otherwise weak references will not work");

  weak_ref.asset_library_type = library.library_type();
  StringRefNull name = library.name();
  if (!name.is_empty()) {
    weak_ref.asset_library_identifier = BLI_strdupn(name.c_str(), name.size());
  }

  weak_ref.relative_asset_identifier = BLI_strdupn(library_relative_identifier.data(),
                                                   library_relative_identifier.size());

  return weak_ref;
}

void BKE_asset_weak_reference_write(BlendWriter *writer, const AssetWeakReference *weak_ref)
{
  writer->write_struct(weak_ref);
  writer->write_string(weak_ref->asset_library_identifier);
  writer->write_string(weak_ref->relative_asset_identifier);
}

void BKE_asset_weak_reference_read(BlendDataReader *reader, AssetWeakReference *weak_ref)
{
  BLO_read_string(reader, &weak_ref->asset_library_identifier);
  BLO_read_string(reader, &weak_ref->relative_asset_identifier);
}

void BKE_asset_catalog_path_list_free(ListBaseT<AssetCatalogPathLink> &catalog_path_list)
{
  for (AssetCatalogPathLink &catalog_path : catalog_path_list.items_mutable()) {
    MEM_delete(catalog_path.path);
    BLI_freelinkN(&catalog_path_list, &catalog_path);
  }
  BLI_assert(catalog_path_list.is_empty());
}

ListBaseT<AssetCatalogPathLink> BKE_asset_catalog_path_list_duplicate(
    const ListBaseT<AssetCatalogPathLink> &catalog_path_list)
{
  ListBaseT<AssetCatalogPathLink> duplicated_list = {nullptr};

  for (AssetCatalogPathLink &catalog_path : catalog_path_list) {
    AssetCatalogPathLink *copied_path = MEM_new<AssetCatalogPathLink>(__func__);
    copied_path->path = BLI_strdup(catalog_path.path);

    BLI_addtail(&duplicated_list, copied_path);
  }

  return duplicated_list;
}

void BKE_asset_catalog_path_list_blend_write(
    BlendWriter *writer, const ListBaseT<AssetCatalogPathLink> &catalog_path_list)
{
  for (const AssetCatalogPathLink &catalog_path : catalog_path_list) {
    writer->write_struct(&catalog_path);
    writer->write_string(catalog_path.path);
  }
}

void BKE_asset_catalog_path_list_blend_read_data(
    BlendDataReader *reader, ListBaseT<AssetCatalogPathLink> &catalog_path_list)
{
  BLO_read_struct_list(reader, AssetCatalogPathLink, &catalog_path_list);
  for (AssetCatalogPathLink &catalog_path : catalog_path_list) {
    BLO_read_string(reader, &catalog_path.path);
  }
}

bool BKE_asset_catalog_path_list_has_path(const ListBaseT<AssetCatalogPathLink> &catalog_path_list,
                                          const char *catalog_path)
{
  return BLI_findstring_ptr(
             &catalog_path_list, catalog_path, offsetof(AssetCatalogPathLink, path)) != nullptr;
}

void BKE_asset_catalog_path_list_add_path(ListBaseT<AssetCatalogPathLink> &catalog_path_list,
                                          const char *catalog_path)
{
  AssetCatalogPathLink *new_path = MEM_new<AssetCatalogPathLink>(__func__);
  new_path->path = BLI_strdup(catalog_path);
  BLI_addtail(&catalog_path_list, new_path);
}

/* #AssetCatalogState ------------------------------------------------ */

void BKE_asset_catalog_state_list_free(ListBaseT<AssetCatalogState> &catalog_state_list)
{
  for (AssetCatalogState &state : catalog_state_list.items_mutable()) {
    MEM_delete(state.path);
    BLI_freelinkN(&catalog_state_list, &state);
  }
  BLI_assert(catalog_state_list.is_empty());
}

void BKE_asset_catalog_state_list_duplicate(ListBaseT<AssetCatalogState> &dest_list,
                                            const ListBaseT<AssetCatalogState> &src_list)
{
  dest_list.clear_no_delete();

  for (const AssetCatalogState &src_state : src_list) {
    AssetCatalogState *new_state = MEM_new<AssetCatalogState>(__func__);
    new_state->path = BLI_strdup(src_state.path);
    new_state->last_used = src_state.last_used;
    new_state->is_collapsed = src_state.is_collapsed;
    BLI_addtail(&dest_list, new_state);
  }
}

void BKE_asset_catalog_state_list_blend_write(
    BlendWriter *writer, const ListBaseT<AssetCatalogState> &catalog_state_list)
{
  for (const AssetCatalogState &state : catalog_state_list) {
    writer->write_struct(&state);
    writer->write_string(state.path);
  }
}

void BKE_asset_catalog_state_list_blend_read_data(BlendDataReader *reader,
                                                  ListBaseT<AssetCatalogState> &catalog_state_list)
{
  BLO_read_struct_list(reader, AssetCatalogState, &catalog_state_list);
  for (AssetCatalogState &state : catalog_state_list.items_mutable()) {
    BLO_read_string(reader, &state.path);
  }
}

void BKE_asset_catalog_state_set_collapsed(ListBaseT<AssetCatalogState> &catalog_state_list,
                                           const char *catalog_path,
                                           const bool collapsed)
{
  if (!catalog_path || !catalog_path[0]) {
    return;
  }

  for (AssetCatalogState &state : catalog_state_list.items_mutable()) {
    if (state.path && STREQ(state.path, catalog_path)) {
      state.is_collapsed = collapsed;
      state.last_used = uint32_t(BLI_time_now_seconds());
      return;
    }
  }

  AssetCatalogState *new_state = MEM_new<AssetCatalogState>(__func__);
  new_state->path = BLI_strdup(catalog_path);
  new_state->is_collapsed = collapsed;
  new_state->last_used = uint32_t(BLI_time_now_seconds());
  BLI_addtail(&catalog_state_list, new_state);
}

std::optional<bool> BKE_asset_catalog_state_get_collapsed(
    const ListBaseT<AssetCatalogState> &catalog_state_list, const char *catalog_path)
{
  if (!catalog_path || !catalog_path[0]) {
    return std::nullopt;
  }

  for (const AssetCatalogState &state : catalog_state_list) {
    if (state.path && STREQ(state.path, catalog_path)) {
      return bool(state.is_collapsed);
    }
  }

  return std::nullopt;
}

void BKE_asset_catalog_state_cleanup_old(ListBaseT<AssetCatalogState> &catalog_state_list,
                                         const uint32_t max_age_seconds)
{
  const uint32_t now = uint32_t(BLI_time_now_seconds());
  for (AssetCatalogState &state : catalog_state_list.items_mutable()) {
    if ((now - state.last_used) > max_age_seconds) {
      MEM_delete(state.path);
      BLI_freelinkN(&catalog_state_list, &state);
    }
  }
}

}  // namespace blender
