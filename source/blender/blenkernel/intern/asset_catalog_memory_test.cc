/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_asset_catalog_memory.hh"
#include "BKE_preferences.h"

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include <cstring>
#include <optional>

namespace blender::bke::tests {

static AssetCatalogUUIDLink *add_uuid_link(ListBaseT<AssetCatalogUUIDLink> &list, uint32_t seed)
{
  AssetCatalogUUIDLink *link = MEM_new<AssetCatalogUUIDLink>(__func__);
  memset(&link->catalog_id, 0, sizeof(link->catalog_id));
  memcpy(&link->catalog_id, &seed, sizeof(seed));
  BLI_addtail(&list, link);
  return link;
}

TEST(asset_catalog_memory, ListFreeEmpty)
{
  ListBaseT<bUserAssetCatalogMemory> list = {nullptr, nullptr};
  BKE_asset_catalog_memory_list_free(list);
  EXPECT_EQ(list.first, nullptr);
  EXPECT_EQ(list.last, nullptr);
}

TEST(asset_catalog_memory, ListFreeFreesSetEntries)
{
  ListBaseT<bUserAssetCatalogMemory> list = {nullptr, nullptr};
  bUserAssetCatalogMemory *entry = MEM_new<bUserAssetCatalogMemory>(__func__);
  STRNCPY(entry->library_identifier, "local");
  STRNCPY(entry->domain, "id_browser");
  entry->mode = ASSET_CATALOG_MEMORY_SET;
  add_uuid_link(entry->catalog_id_set, 1);
  add_uuid_link(entry->catalog_id_set, 2);
  BLI_addtail(&list, entry);

  BKE_asset_catalog_memory_list_free(list);
  EXPECT_EQ(list.first, nullptr);
}

TEST(asset_catalog_memory, DuplicateDeepCopiesSet)
{
  ListBaseT<bUserAssetCatalogMemory> src = {nullptr, nullptr};
  bUserAssetCatalogMemory *entry = MEM_new<bUserAssetCatalogMemory>(__func__);
  STRNCPY(entry->library_identifier, "local");
  STRNCPY(entry->domain, "image_grid");
  entry->mode = ASSET_CATALOG_MEMORY_SET;
  add_uuid_link(entry->catalog_id_set, 42);
  BLI_addtail(&src, entry);

  ListBaseT<bUserAssetCatalogMemory> dst = {nullptr, nullptr};
  BKE_asset_catalog_memory_list_duplicate(dst, src);

  ASSERT_NE(dst.first, nullptr);
  bUserAssetCatalogMemory *dup = static_cast<bUserAssetCatalogMemory *>(dst.first);
  EXPECT_STREQ(dup->library_identifier, "local");
  EXPECT_EQ(dup->mode, ASSET_CATALOG_MEMORY_SET);
  ASSERT_NE(dup->catalog_id_set.first, nullptr);
  /* Deep copy: freeing the source must not affect the duplicate. */
  BKE_asset_catalog_memory_list_free(src);
  ASSERT_NE(dup->catalog_id_set.first, nullptr);

  BKE_asset_catalog_memory_list_free(dst);
}

TEST(asset_catalog_memory, SanitizeUnknownModeFallsBackToAll)
{
  ListBaseT<bUserAssetCatalogMemory> list = {nullptr, nullptr};
  bUserAssetCatalogMemory *entry = MEM_new<bUserAssetCatalogMemory>(__func__);
  STRNCPY(entry->library_identifier, "local");
  STRNCPY(entry->domain, "asset_browser");
  entry->mode = eAssetCatalogMemoryMode(99); /* Out of range. */
  BLI_addtail(&list, entry);

  BKE_asset_catalog_memory_entry_sanitize(*entry);
  EXPECT_EQ(entry->mode, ASSET_CATALOG_MEMORY_ALL);

  BKE_asset_catalog_memory_list_free(list);
}

TEST(asset_catalog_memory, SanitizeDropsNilUUIDEntries)
{
  bUserAssetCatalogMemory entry;
  STRNCPY(entry.library_identifier, "local");
  STRNCPY(entry.domain, "id_browser");
  entry.mode = ASSET_CATALOG_MEMORY_SET;
  add_uuid_link(entry.catalog_id_set, 0); /* Nil UUID -- all-zero. */
  add_uuid_link(entry.catalog_id_set, 7); /* Valid. */

  BKE_asset_catalog_memory_entry_sanitize(entry);

  int count = 0;
  for (const AssetCatalogUUIDLink &link : entry.catalog_id_set) {
    (void)link;
    count++;
  }
  EXPECT_EQ(count, 1);

  BKE_asset_catalog_memory_list_free_single(entry);
}

static AssetLibraryReference local_library_ref()
{
  AssetLibraryReference ref{};
  ref.type = ASSET_LIBRARY_LOCAL;
  return ref;
}

TEST(asset_catalog_memory, GetSingleAbsentEntryReturnsAllMode)
{
  UserDef userdef = {};
  EXPECT_EQ(BKE_asset_catalog_memory_get_mode(&userdef, local_library_ref(), "asset_browser"),
            ASSET_CATALOG_MEMORY_ALL);
  EXPECT_EQ(BKE_asset_catalog_memory_get_single(&userdef, local_library_ref(), "asset_browser"),
            std::nullopt);
  BKE_asset_catalog_memory_list_free(userdef.catalog_memory);
}

TEST(asset_catalog_memory, SetSingleThenGetRoundTrips)
{
  UserDef userdef = {};
  bUUID catalog_id;
  memset(&catalog_id, 0, sizeof(catalog_id));
  reinterpret_cast<uint8_t *>(&catalog_id)[0] = 7;

  BKE_asset_catalog_memory_set_single(&userdef, local_library_ref(), "asset_browser", catalog_id);

  EXPECT_EQ(BKE_asset_catalog_memory_get_mode(&userdef, local_library_ref(), "asset_browser"),
            ASSET_CATALOG_MEMORY_SINGLE);
  const std::optional<bUUID> got = BKE_asset_catalog_memory_get_single(
      &userdef, local_library_ref(), "asset_browser");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(memcmp(&*got, &catalog_id, sizeof(bUUID)), 0);

  BKE_asset_catalog_memory_list_free(userdef.catalog_memory);
}

TEST(asset_catalog_memory, ModeSwitchDoesNotClearSetPayload_ImageGridGuard)
{
  /* Regression test for the rev-3->rev-4 fix: switching to RECENT/FAVORITES must never wipe a
   * previously saved catalog set, matching view3d_image_shelf_sync.cc's existing guard. */
  UserDef userdef = {};
  bUUID catalog_id;
  memset(&catalog_id, 0, sizeof(catalog_id));
  reinterpret_cast<uint8_t *>(&catalog_id)[0] = 3;

  BKE_asset_catalog_memory_set_set(
      &userdef, local_library_ref(), "image_grid", Span<bUUID>(&catalog_id, 1));
  ASSERT_EQ(BKE_asset_catalog_memory_get_set(&userdef, local_library_ref(), "image_grid").size(),
            1);

  AssetLibraryReference all_ref{};
  all_ref.type = ASSET_LIBRARY_ALL;
  BKE_asset_catalog_memory_set_mode(&userdef, all_ref, "image_grid", ASSET_CATALOG_MEMORY_RECENT);
  EXPECT_EQ(BKE_asset_catalog_memory_get_mode(&userdef, all_ref, "image_grid"),
            ASSET_CATALOG_MEMORY_RECENT);

  /* The library-A set must be untouched -- it lives under a different library_identifier key
   * than the "all"-keyed Recent mode entry, and even under the same key, set_mode must not
   * touch catalog_id_set. */
  const Vector<bUUID> set_after = BKE_asset_catalog_memory_get_set(
      &userdef, local_library_ref(), "image_grid");
  EXPECT_EQ(set_after.size(), 1);

  BKE_asset_catalog_memory_list_free(userdef.catalog_memory);
}

TEST(asset_catalog_memory, RenameLibraryMovesEntry)
{
  UserDef userdef = {};
  bUUID catalog_id;
  memset(&catalog_id, 0, sizeof(catalog_id));
  reinterpret_cast<uint8_t *>(&catalog_id)[0] = 9;
  AssetLibraryReference custom_ref{};
  custom_ref.type = ASSET_LIBRARY_CUSTOM;
  STRNCPY(custom_ref.custom_library_name, "old_name");

  /* Directly exercise the identifier-keyed API without going through library resolution --
   * BKE_asset_catalog_memory_rename_library operates on raw identifier strings, same as the
   * existing BKE_preferences_asset_browser_settings_rename_library it mirrors. */
  bUserAssetCatalogMemory *entry = MEM_new<bUserAssetCatalogMemory>(__func__);
  STRNCPY(entry->library_identifier, "old_name");
  STRNCPY(entry->domain, "asset_browser");
  entry->mode = ASSET_CATALOG_MEMORY_SINGLE;
  entry->single_catalog_id = catalog_id;
  BLI_addtail(&userdef.catalog_memory, entry);

  BKE_asset_catalog_memory_rename_library(&userdef, "old_name", "new_name");

  bool found_new = false;
  for (const bUserAssetCatalogMemory &e : userdef.catalog_memory) {
    if (STREQ(e.library_identifier, "new_name")) {
      found_new = true;
    }
    EXPECT_FALSE(STREQ(e.library_identifier, "old_name"));
  }
  EXPECT_TRUE(found_new);

  BKE_asset_catalog_memory_list_free(userdef.catalog_memory);
}

}  // namespace blender::bke::tests
