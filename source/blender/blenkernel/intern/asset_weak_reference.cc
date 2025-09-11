/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <memory>

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_vector.hh"

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

  const char *a_lib_idenfifier = a.asset_library_identifier ? a.asset_library_identifier : "";
  const char *b_lib_idenfifier = b.asset_library_identifier ? b.asset_library_identifier : "";
  if (BLI_path_cmp_normalized(a_lib_idenfifier, b_lib_idenfifier) != 0) {
    return false;
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

void BKE_asset_catalog_path_list_free(ListBaseT<AssetCatalogState> &catalog_path_list)
{
  for (AssetCatalogState &catalog_path : catalog_path_list.items_mutable()) {
    MEM_delete(catalog_path.path);
    BLI_freelinkN(&catalog_path_list, &catalog_path);
  }
  BLI_assert(BLI_listbase_is_empty(&catalog_path_list));
}

ListBaseT<AssetCatalogState> BKE_asset_catalog_path_list_duplicate(
    const ListBaseT<AssetCatalogState> &catalog_path_list)
{
  ListBaseT<AssetCatalogState> duplicated_list = {nullptr, nullptr};

  for (const AssetCatalogState &catalog_path : catalog_path_list) {
    AssetCatalogState *copied_path = MEM_new<AssetCatalogState>(__func__);
    copied_path->path = BLI_strdup(catalog_path.path);
    copied_path->path_hash = catalog_path.path_hash;
    copied_path->is_collapsed = catalog_path.is_collapsed;
    copied_path->last_used = catalog_path.last_used;

    BLI_addtail(&duplicated_list, copied_path);
  }

  return duplicated_list;
}

void BKE_asset_catalog_path_list_blend_write(BlendWriter *writer,
                                             const ListBaseT<AssetCatalogState> &catalog_path_list)
{
  for (const AssetCatalogState &catalog_path : catalog_path_list) {
    writer->write_struct(&catalog_path);
    writer->write_string(catalog_path.path);
  }
}

void BKE_asset_catalog_path_list_blend_read_data(BlendDataReader *reader,
                                                 ListBaseT<AssetCatalogState> &catalog_path_list)
{
  BLO_read_struct_list(reader, AssetCatalogState, &catalog_path_list);
  for (AssetCatalogState &catalog_path : catalog_path_list.items_mutable()) {
    BLO_read_string(reader, &catalog_path.path);
    /* The _pad_ptr field is structural padding and must not carry a stale address after load. */
    catalog_path._pad_ptr = nullptr;
  }
}

bool BKE_asset_catalog_path_list_has_path(const ListBaseT<AssetCatalogState> &catalog_path_list,
                                          const char *catalog_path)
{
  return BLI_findstring_ptr(&catalog_path_list, catalog_path, offsetof(AssetCatalogState, path)) !=
         nullptr;
}

void BKE_asset_catalog_path_list_add_path(ListBaseT<AssetCatalogState> &catalog_path_list,
                                          const char *catalog_path)
{
  AssetCatalogState *new_path = MEM_new<AssetCatalogState>(__func__);
  new_path->path = BLI_strdup(catalog_path);
  new_path->is_collapsed = true;
  BLI_addtail(&catalog_path_list, new_path);
}

bool BKE_asset_catalog_path_is_collapsed(const ListBaseT<AssetCatalogState> &catalog_path_list,
                                         const char *catalog_path)
{
  if (!catalog_path || !catalog_path[0]) {
    return true;
  }

  for (const AssetCatalogState &collapsed_state : catalog_path_list) {
    if (collapsed_state.path && STREQ(collapsed_state.path, catalog_path)) {
      return collapsed_state.is_collapsed;
    }
  }

  /* Default to collapsed state for new catalogs */
  return true;
}

/* Asset catalog state management functions */

void BKE_asset_catalog_state_set_collapsed(ListBaseT<AssetCatalogState> &catalog_state_list,
                                           const char *catalog_path,
                                           bool collapsed)
{
  if (!catalog_path || !catalog_path[0]) {
    return;
  }

  const uint64_t path_hash = BLI_hash_string(catalog_path);

  /* Find existing entry using hash optimization */
  for (AssetCatalogState &state : catalog_state_list.items_mutable()) {
    if (state.path && state.path_hash == path_hash && STREQ(state.path, catalog_path)) {
      state.is_collapsed = collapsed;
      state.last_used = uint32_t(BLI_time_now_seconds());
      return;
    }
  }

  /* Create new entry if not found */
  AssetCatalogState *new_state = MEM_new<AssetCatalogState>(__func__);
  new_state->path = BLI_strdup(catalog_path);
  new_state->path_hash = path_hash;
  new_state->is_collapsed = collapsed;
  new_state->last_used = uint32_t(BLI_time_now_seconds());
  BLI_addtail(&catalog_state_list, new_state);
}

bool BKE_asset_catalog_state_get_collapsed(const ListBaseT<AssetCatalogState> &catalog_state_list,
                                           const char *catalog_path,
                                           bool default_state)
{
  if (!catalog_path || !catalog_path[0]) {
    return default_state;
  }

  const uint64_t path_hash = BLI_hash_string(catalog_path);

  for (const AssetCatalogState &state : catalog_state_list) {
    if (state.path && state.path_hash == path_hash && STREQ(state.path, catalog_path)) {
      return state.is_collapsed;
    }
  }

  return default_state;
}

void BKE_asset_catalog_state_toggle_collapsed(ListBaseT<AssetCatalogState> &catalog_state_list,
                                              const char *catalog_path,
                                              bool default_state)
{
  const bool current_state = BKE_asset_catalog_state_get_collapsed(
      catalog_state_list, catalog_path, default_state);
  BKE_asset_catalog_state_set_collapsed(catalog_state_list, catalog_path, !current_state);
}

void BKE_asset_catalog_state_list_free(ListBaseT<AssetCatalogState> &catalog_state_list)
{
  for (AssetCatalogState &state : catalog_state_list.items_mutable()) {
    MEM_delete(state.path);
    BLI_freelinkN(&catalog_state_list, &state);
  }
  BLI_assert(BLI_listbase_is_empty(&catalog_state_list));
}

void BKE_asset_catalog_state_list_duplicate(ListBaseT<AssetCatalogState> &dest_list,
                                            const ListBaseT<AssetCatalogState> &src_list)
{
  BLI_listbase_clear(&dest_list);

  for (const AssetCatalogState &src_state : src_list) {
    AssetCatalogState *new_state = MEM_new<AssetCatalogState>(__func__);
    new_state->path = BLI_strdup(src_state.path);
    new_state->path_hash = src_state.path_hash;
    new_state->is_collapsed = src_state.is_collapsed;
    new_state->last_used = src_state.last_used;
    BLI_addtail(&dest_list, new_state);
  }
}

void BKE_asset_catalog_state_cleanup_old(ListBaseT<AssetCatalogState> &catalog_state_list,
                                         int max_entries)
{
  const int current_count = BLI_listbase_count(&catalog_state_list);
  if (current_count <= max_entries) {
    return;
  }

  blender::Vector<AssetCatalogState *> states;
  states.reserve(current_count);

  for (AssetCatalogState &state : catalog_state_list.items_mutable()) {
    states.append(&state);
  }

  /* Sort oldest-first so the entries to evict are at the front. */
  std::sort(
      states.begin(), states.end(), [](const AssetCatalogState *a, const AssetCatalogState *b) {
        return a->last_used < b->last_used;
      });

  const int to_remove = current_count - max_entries;
  const uint32_t current_time = uint32_t(BLI_time_now_seconds());
  const uint32_t recent_threshold = 7 * 24 * 60 * 60; /* 7 days */

  for (int i = 0; i < to_remove; i++) {
    AssetCatalogState *state = states[i];
    /* Skip recently used entries so active catalogs are never evicted. */
    if ((current_time - state->last_used) < recent_threshold) {
      continue;
    }
    MEM_delete(state->path);
    BLI_freelinkN(&catalog_state_list, state);
  }
}

}  // namespace blender
