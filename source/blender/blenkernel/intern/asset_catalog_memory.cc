/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_asset_catalog_memory.hh"

#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BKE_preferences.h"

#include "MEM_guardedalloc.h"

#include "BLO_read_write.hh"

namespace blender {

static bool uuid_is_nil(const bUUID &uuid)
{
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&uuid);
  for (size_t i = 0; i < sizeof(bUUID); i++) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

static void catalog_id_set_free(ListBaseT<AssetCatalogUUIDLink> &list)
{
  while (AssetCatalogUUIDLink *link = static_cast<AssetCatalogUUIDLink *>(BLI_pophead(&list))) {
    MEM_delete(link);
  }
}

static void catalog_id_set_duplicate(ListBaseT<AssetCatalogUUIDLink> &dst,
                                      const ListBaseT<AssetCatalogUUIDLink> &src)
{
  for (const AssetCatalogUUIDLink &link : src) {
    AssetCatalogUUIDLink *copy = MEM_new<AssetCatalogUUIDLink>(__func__);
    copy->catalog_id = link.catalog_id;
    BLI_addtail(&dst, copy);
  }
}

void BKE_asset_catalog_memory_list_free_single(bUserAssetCatalogMemory &entry)
{
  catalog_id_set_free(entry.catalog_id_set);
}

void BKE_asset_catalog_memory_list_free(ListBaseT<bUserAssetCatalogMemory> &list)
{
  while (bUserAssetCatalogMemory *entry = static_cast<bUserAssetCatalogMemory *>(
             BLI_pophead(&list)))
  {
    BKE_asset_catalog_memory_list_free_single(*entry);
    MEM_delete(entry);
  }
}

void BKE_asset_catalog_memory_list_duplicate(ListBaseT<bUserAssetCatalogMemory> &dst,
                                              const ListBaseT<bUserAssetCatalogMemory> &src)
{
  for (const bUserAssetCatalogMemory &entry : src) {
    bUserAssetCatalogMemory *copy = MEM_new<bUserAssetCatalogMemory>(__func__);
    STRNCPY(copy->library_identifier, entry.library_identifier);
    STRNCPY(copy->domain, entry.domain);
    copy->mode = entry.mode;
    copy->single_catalog_id = entry.single_catalog_id;
    catalog_id_set_duplicate(copy->catalog_id_set, entry.catalog_id_set);
    BLI_addtail(&dst, copy);
  }
}

void BKE_asset_catalog_memory_entry_sanitize(bUserAssetCatalogMemory &entry)
{
  if (entry.mode < ASSET_CATALOG_MEMORY_ALL || entry.mode > ASSET_CATALOG_MEMORY_FAVORITES) {
    entry.mode = ASSET_CATALOG_MEMORY_ALL;
  }

  /* Torn list head (one of first/last null, the other not) -- reset without walking or freeing;
   * the pointers cannot be trusted enough to iterate or MEM_delete through. */
  if ((entry.catalog_id_set.first == nullptr) != (entry.catalog_id_set.last == nullptr)) {
    entry.catalog_id_set = {nullptr, nullptr};
    return;
  }

  ListBaseT<AssetCatalogUUIDLink> sanitized = {nullptr, nullptr};
  while (AssetCatalogUUIDLink *link = static_cast<AssetCatalogUUIDLink *>(
             BLI_pophead(&entry.catalog_id_set)))
  {
    if (uuid_is_nil(link->catalog_id)) {
      MEM_delete(link);
      continue;
    }
    BLI_addtail(&sanitized, link);
  }
  entry.catalog_id_set = sanitized;
}

void BKE_asset_catalog_memory_list_blend_write(BlendWriter *writer,
                                                const ListBaseT<bUserAssetCatalogMemory> &list)
{
  for (const bUserAssetCatalogMemory &entry : list) {
    writer->write_struct(&entry);
    writer->write_struct_list(&entry.catalog_id_set);
  }
}

void BKE_asset_catalog_memory_list_blend_read_data(BlendDataReader *reader,
                                                    ListBaseT<bUserAssetCatalogMemory> &list)
{
  BLO_read_struct_list(reader, bUserAssetCatalogMemory, &list);
  for (bUserAssetCatalogMemory &entry : list.items_mutable()) {
    BLO_read_struct_list(reader, AssetCatalogUUIDLink, &entry.catalog_id_set);
    BKE_asset_catalog_memory_entry_sanitize(entry);
  }
}

bUserAssetCatalogMemory::bUserAssetCatalogMemory(const bUserAssetCatalogMemory &other)
{
  STRNCPY(library_identifier, other.library_identifier);
  STRNCPY(domain, other.domain);
  mode = other.mode;
  single_catalog_id = other.single_catalog_id;
  catalog_id_set_duplicate(catalog_id_set, other.catalog_id_set);
}

bUserAssetCatalogMemory &bUserAssetCatalogMemory::operator=(const bUserAssetCatalogMemory &other)
{
  if (this == &other) {
    return *this;
  }
  catalog_id_set_free(catalog_id_set);
  STRNCPY(library_identifier, other.library_identifier);
  STRNCPY(domain, other.domain);
  mode = other.mode;
  single_catalog_id = other.single_catalog_id;
  catalog_id_set_duplicate(catalog_id_set, other.catalog_id_set);
  return *this;
}

bUserAssetCatalogMemory::~bUserAssetCatalogMemory()
{
  catalog_id_set_free(catalog_id_set);
}

static bUserAssetCatalogMemory *catalog_memory_find(UserDef *userdef,
                                                     const char *library_identifier,
                                                     StringRef domain)
{
  for (bUserAssetCatalogMemory &entry : userdef->catalog_memory) {
    if (STREQ(entry.library_identifier, library_identifier) && domain == entry.domain) {
      return &entry;
    }
  }
  return nullptr;
}

static const bUserAssetCatalogMemory *catalog_memory_find(const UserDef *userdef,
                                                           const char *library_identifier,
                                                           StringRef domain)
{
  return catalog_memory_find(const_cast<UserDef *>(userdef), library_identifier, domain);
}

static bUserAssetCatalogMemory *catalog_memory_ensure(UserDef *userdef,
                                                       const char *library_identifier,
                                                       StringRef domain)
{
  if (bUserAssetCatalogMemory *existing = catalog_memory_find(userdef, library_identifier, domain))
  {
    return existing;
  }
  bUserAssetCatalogMemory *entry = MEM_new<bUserAssetCatalogMemory>(__func__);
  STRNCPY(entry->library_identifier, library_identifier);
  domain.copy_utf8_truncated(entry->domain, sizeof(entry->domain));
  BLI_addtail(&userdef->catalog_memory, entry);
  return entry;
}

std::optional<bUUID> BKE_asset_catalog_memory_get_single(const UserDef *userdef,
                                                          const AssetLibraryReference &library_ref,
                                                          StringRef domain)
{
  const char *identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                              &library_ref);
  const bUserAssetCatalogMemory *entry = catalog_memory_find(userdef, identifier, domain);
  if (!entry || entry->mode != ASSET_CATALOG_MEMORY_SINGLE) {
    return std::nullopt;
  }
  return entry->single_catalog_id;
}

void BKE_asset_catalog_memory_set_single(UserDef *userdef,
                                          const AssetLibraryReference &library_ref,
                                          StringRef domain,
                                          bUUID catalog_id)
{
  const char *identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                              &library_ref);
  bUserAssetCatalogMemory *entry = catalog_memory_ensure(userdef, identifier, domain);
  entry->mode = ASSET_CATALOG_MEMORY_SINGLE;
  entry->single_catalog_id = catalog_id;
  userdef->runtime.is_dirty = true;
}

void BKE_asset_catalog_memory_set_all(UserDef *userdef,
                                       const AssetLibraryReference &library_ref,
                                       StringRef domain)
{
  const char *identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                              &library_ref);
  /* ALL is the implicit default for an absent entry (see #BKE_asset_catalog_memory_get_mode) --
   * do not create one just to store it; that would grow #UserDef.catalog_memory by one row for
   * every "show all" click on a library that was never otherwise narrowed. */
  bUserAssetCatalogMemory *entry = catalog_memory_find(userdef, identifier, domain);
  if (!entry) {
    return;
  }
  entry->mode = ASSET_CATALOG_MEMORY_ALL;
  userdef->runtime.is_dirty = true;
}

Vector<bUUID> BKE_asset_catalog_memory_get_set(const UserDef *userdef,
                                                const AssetLibraryReference &library_ref,
                                                StringRef domain)
{
  const char *identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                              &library_ref);
  const bUserAssetCatalogMemory *entry = catalog_memory_find(userdef, identifier, domain);
  Vector<bUUID> result;
  if (!entry || entry->mode != ASSET_CATALOG_MEMORY_SET) {
    return result;
  }
  for (const AssetCatalogUUIDLink &link : entry->catalog_id_set) {
    result.append(link.catalog_id);
  }
  return result;
}

void BKE_asset_catalog_memory_set_set(UserDef *userdef,
                                       const AssetLibraryReference &library_ref,
                                       StringRef domain,
                                       Span<bUUID> catalog_ids)
{
  const char *identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                              &library_ref);
  bUserAssetCatalogMemory *entry = catalog_memory_ensure(userdef, identifier, domain);
  catalog_id_set_free(entry->catalog_id_set);
  for (const bUUID &catalog_id : catalog_ids) {
    AssetCatalogUUIDLink *link = MEM_new<AssetCatalogUUIDLink>(__func__);
    link->catalog_id = catalog_id;
    BLI_addtail(&entry->catalog_id_set, link);
  }
  entry->mode = ASSET_CATALOG_MEMORY_SET;
  userdef->runtime.is_dirty = true;
}

void BKE_asset_catalog_memory_set_mode(UserDef *userdef,
                                        const AssetLibraryReference &library_ref,
                                        StringRef domain,
                                        eAssetCatalogMemoryMode mode)
{
  const char *identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                              &library_ref);
  bUserAssetCatalogMemory *entry = catalog_memory_ensure(userdef, identifier, domain);
  /* Only the mode field changes -- single_catalog_id/catalog_id_set are left exactly as they
   * were, per the rev-4 independence rule (Image Grid regression guard, see the test above). */
  entry->mode = mode;
  userdef->runtime.is_dirty = true;
}

eAssetCatalogMemoryMode BKE_asset_catalog_memory_get_mode(const UserDef *userdef,
                                                           const AssetLibraryReference &library_ref,
                                                           StringRef domain)
{
  const char *identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                              &library_ref);
  const bUserAssetCatalogMemory *entry = catalog_memory_find(userdef, identifier, domain);
  return entry ? entry->mode : ASSET_CATALOG_MEMORY_ALL;
}

void BKE_asset_catalog_memory_rename_library(UserDef *userdef,
                                              const char *old_identifier,
                                              const char *new_identifier)
{
  if (!userdef || !old_identifier || !new_identifier || !old_identifier[0] ||
      !new_identifier[0] || STREQ(old_identifier, new_identifier))
  {
    return;
  }
  bool renamed = false;
  for (bUserAssetCatalogMemory &entry : userdef->catalog_memory) {
    if (STREQ(entry.library_identifier, old_identifier)) {
      STRNCPY(entry.library_identifier, new_identifier);
      renamed = true;
    }
  }
  if (renamed) {
    userdef->runtime.is_dirty = true;
  }
}

}  // namespace blender
