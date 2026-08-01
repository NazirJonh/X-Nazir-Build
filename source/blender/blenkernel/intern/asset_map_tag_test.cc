/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_asset.hh"

#include "BLI_listbase_iterator.hh"
#include "DNA_asset_types.h"
#include "MEM_guardedalloc.h"
#include "testing/testing.h"

namespace blender::tests {

TEST(asset_map_tag, is_map_tag_prefix_case_insensitive)
{
  EXPECT_TRUE(BKE_asset_metadata_tag_is_map_tag("map:NORMAL"));
  EXPECT_TRUE(BKE_asset_metadata_tag_is_map_tag("MAP:NORMAL"));
  EXPECT_TRUE(BKE_asset_metadata_tag_is_map_tag("map:foo:bar"));
  EXPECT_TRUE(BKE_asset_metadata_tag_is_map_tag("map:"));
  EXPECT_FALSE(BKE_asset_metadata_tag_is_map_tag("Normal"));
  EXPECT_FALSE(BKE_asset_metadata_tag_is_map_tag("mapping"));
  EXPECT_FALSE(BKE_asset_metadata_tag_is_map_tag(""));
}

TEST(asset_map_tag, clear_removes_variants_keeps_others)
{
  AssetMetaData *meta = BKE_asset_metadata_create();
  BKE_asset_metadata_tag_add(meta, "user");
  BKE_asset_metadata_tag_add(meta, "MAP:NORMAL");
  BKE_asset_metadata_tag_add(meta, "map:foo:bar");
  BKE_asset_metadata_map_tags_clear(meta);
  int count = 0;
  for (AssetTag &tag : meta->tags) {
    count++;
    EXPECT_STREQ(tag.name, "user");
  }
  EXPECT_EQ(count, 1);
  BKE_asset_metadata_free(&meta);
}

TEST(asset_map_tag, ensure_writes_canonical)
{
  AssetMetaData *meta = BKE_asset_metadata_create();
  AssetTag *tag = BKE_asset_metadata_map_tag_ensure(meta, "BASE_COLOR");
  ASSERT_NE(tag, nullptr);
  EXPECT_STREQ(tag->name, "map:BASE_COLOR");
  EXPECT_EQ(BKE_asset_metadata_map_tag_ensure(meta, ""), nullptr);
  BKE_asset_metadata_free(&meta);
}

TEST(asset_map_tag, ensure_is_idempotent)
{
  AssetMetaData *meta = BKE_asset_metadata_create();
  AssetTag *first = BKE_asset_metadata_map_tag_ensure(meta, "NORMAL");
  AssetTag *second = BKE_asset_metadata_map_tag_ensure(meta, "NORMAL");
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(meta->tot_tags, 1);
  BKE_asset_metadata_free(&meta);
}

}  // namespace blender::tests
