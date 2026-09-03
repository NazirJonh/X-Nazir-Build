/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "ED_asset_catalog.hh"
#include "ED_asset_name_matching.hh"
#include "BKE_name_matching.hh"
#include "BLI_string_ref.hh"
#include "DNA_userdef_types.h"

#include <string>

namespace blender::ed::asset::tests {

TEST(asset_catalog, RootNameSanitize)
{
  std::string sanitized;

  /* Empty string */
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize("", sanitized), CatalogNameValidateResult::Empty);
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize("   ", sanitized), CatalogNameValidateResult::Empty);

  /* Invalid characters (reserved names) */
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize(".", sanitized), CatalogNameValidateResult::InvalidChars);
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize("..", sanitized), CatalogNameValidateResult::InvalidChars);

  /* Invalid characters (path separators) */
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize("cat/alog", sanitized), CatalogNameValidateResult::InvalidChars);
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize("cat\\alog", sanitized), CatalogNameValidateResult::InvalidChars);

  /* Valid names */
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize("My Catalog", sanitized), CatalogNameValidateResult::Ok);
  EXPECT_EQ(sanitized, "My Catalog");

  /* Valid names with leading/trailing spaces */
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize("  My Catalog  ", sanitized), CatalogNameValidateResult::Ok);
  EXPECT_EQ(sanitized, "My Catalog");

  /* Valid, containing single dot */
  EXPECT_EQ(ED_asset_catalog_root_name_sanitize(".catalog", sanitized), CatalogNameValidateResult::Ok);
  EXPECT_EQ(sanitized, ".catalog");
}

TEST(asset_name_matching, GuessMapTypeIdentifier)
{
  UserDef userdef = {};
  bUserNameMatchMapType *normal = BKE_name_matching_map_type_add(
      &userdef, "Normal", "NORMAL", 0);
  ASSERT_NE(normal, nullptr);
  ASSERT_NE(BKE_name_matching_token_add(normal, "normal"), nullptr);

  EXPECT_EQ(ED_asset_name_matching_guess_map_type_identifier(userdef, "Brick_Normal.png"),
            "NORMAL");
  EXPECT_TRUE(
      ED_asset_name_matching_guess_map_type_identifier(userdef, "Brick_BaseColor.png").empty());

  bUserNameMatchMapType *roughness = BKE_name_matching_map_type_add(
      &userdef, "Roughness", "ROUGHNESS", 0);
  ASSERT_NE(roughness, nullptr);
  ASSERT_NE(BKE_name_matching_token_add(roughness, "normal"), nullptr);
  EXPECT_TRUE(
      ED_asset_name_matching_guess_map_type_identifier(userdef, "Brick_Normal.png").empty());

  BKE_name_matching_userdef_free(&userdef);
}

}  // namespace blender::ed::asset::tests
