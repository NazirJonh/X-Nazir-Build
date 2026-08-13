/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_preferences.h"

#include "DNA_userdef_types.h"

#include "BLI_listbase.h"

namespace blender::bke::tests {

TEST(preferences, AssetLibraryDeepestContainingPathPrefersNestedLibrary)
{
  UserDef userdef{};

  bUserAssetLibrary *parent = BKE_preferences_asset_library_add(
      &userdef, "Parent", "/tmp/libs/parent");
  bUserAssetLibrary *nested = BKE_preferences_asset_library_add(
      &userdef, "Nested", "/tmp/libs/parent/nested");
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(nested, nullptr);

  /* First-match walks the list in insertion order, so the parent wins for a nested path. */
  EXPECT_EQ(BKE_preferences_asset_library_containing_path(&userdef, "/tmp/libs/parent/nested"),
            parent);
  EXPECT_EQ(
      BKE_preferences_asset_library_deepest_containing_path(&userdef, "/tmp/libs/parent/nested"),
      nested);

  EXPECT_EQ(BKE_preferences_asset_library_deepest_containing_path(&userdef, "/tmp/libs/parent"),
            parent);
  EXPECT_EQ(BKE_preferences_asset_library_deepest_containing_path(&userdef, "/tmp/other"),
            nullptr);

  BKE_preferences_asset_library_remove(&userdef, nested);
  BKE_preferences_asset_library_remove(&userdef, parent);
}

}  // namespace blender::bke::tests
