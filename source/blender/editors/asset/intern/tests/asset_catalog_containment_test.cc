/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_set.hh"

#include "ED_asset_filter.hh"

namespace blender::ed::asset::tests {

TEST(asset_catalog_containment, empty_set_shows_all)
{
  const Set<std::string> empty;
  EXPECT_TRUE(catalog_path_is_in_enabled_set("Textures/Brick", empty, CatalogContainment::Exact));
  EXPECT_TRUE(
      catalog_path_is_in_enabled_set("Textures/Brick", empty, CatalogContainment::IncludeChildren));
  EXPECT_TRUE(catalog_path_is_in_enabled_set("", empty, CatalogContainment::Exact));
}

TEST(asset_catalog_containment, exact_does_not_include_children)
{
  Set<std::string> enabled;
  enabled.add_new("Textures");

  EXPECT_TRUE(catalog_path_is_in_enabled_set("Textures", enabled, CatalogContainment::Exact));
  EXPECT_FALSE(
      catalog_path_is_in_enabled_set("Textures/Brick", enabled, CatalogContainment::Exact));
  EXPECT_FALSE(catalog_path_is_in_enabled_set("Other", enabled, CatalogContainment::Exact));
}

TEST(asset_catalog_containment, include_children_covers_descendants)
{
  Set<std::string> enabled;
  enabled.add_new("Textures");

  EXPECT_TRUE(
      catalog_path_is_in_enabled_set("Textures", enabled, CatalogContainment::IncludeChildren));
  EXPECT_TRUE(catalog_path_is_in_enabled_set(
      "Textures/Brick", enabled, CatalogContainment::IncludeChildren));
  EXPECT_TRUE(catalog_path_is_in_enabled_set(
      "Textures/Brick/Worn", enabled, CatalogContainment::IncludeChildren));
  EXPECT_FALSE(
      catalog_path_is_in_enabled_set("Other", enabled, CatalogContainment::IncludeChildren));
  EXPECT_FALSE(
      catalog_path_is_in_enabled_set("Texture", enabled, CatalogContainment::IncludeChildren));
}

TEST(asset_catalog_containment, unassigned_hidden_when_set_nonempty)
{
  Set<std::string> enabled;
  enabled.add_new("Textures");
  EXPECT_FALSE(catalog_path_is_in_enabled_set("", enabled, CatalogContainment::Exact));
  EXPECT_FALSE(catalog_path_is_in_enabled_set("", enabled, CatalogContainment::IncludeChildren));
}

TEST(asset_catalog_containment, union_of_several_paths)
{
  Set<std::string> enabled;
  enabled.add_new("Textures");
  enabled.add_new("HDRI");

  EXPECT_TRUE(
      catalog_path_is_in_enabled_set("HDRI/Studio", enabled, CatalogContainment::IncludeChildren));
  EXPECT_TRUE(catalog_path_is_in_enabled_set(
      "Textures/Brick", enabled, CatalogContainment::IncludeChildren));
  EXPECT_FALSE(
      catalog_path_is_in_enabled_set("Meshes", enabled, CatalogContainment::IncludeChildren));
}

}  // namespace blender::ed::asset::tests
