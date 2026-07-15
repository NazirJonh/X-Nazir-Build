/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "asset_shelf_brush_lists.hh"

#include "BLI_serialize.hh"

#include "testing/testing.h"

namespace blender::ed::asset::shelf::tests {

static BrushAssetRef make_ref(const std::string &relative_identifier)
{
  BrushAssetRef ref;
  ref.library_type = ASSET_LIBRARY_LOCAL;
  ref.relative_identifier = relative_identifier;
  return ref;
}

TEST(asset_shelf_brush_lists, record_recent_into_prepends_new_entry)
{
  Vector<BrushAssetRef> recent;
  record_recent_into(recent, make_ref("brush_a"), BRUSH_ASSET_LISTS_RECENT_MAX);
  record_recent_into(recent, make_ref("brush_b"), BRUSH_ASSET_LISTS_RECENT_MAX);

  ASSERT_EQ(recent.size(), 2);
  EXPECT_EQ(recent[0].relative_identifier, "brush_b");
  EXPECT_EQ(recent[1].relative_identifier, "brush_a");
}

TEST(asset_shelf_brush_lists, record_recent_into_moves_existing_entry_to_front)
{
  Vector<BrushAssetRef> recent;
  record_recent_into(recent, make_ref("brush_a"), BRUSH_ASSET_LISTS_RECENT_MAX);
  record_recent_into(recent, make_ref("brush_b"), BRUSH_ASSET_LISTS_RECENT_MAX);
  record_recent_into(recent, make_ref("brush_a"), BRUSH_ASSET_LISTS_RECENT_MAX);

  ASSERT_EQ(recent.size(), 2);
  EXPECT_EQ(recent[0].relative_identifier, "brush_a");
  EXPECT_EQ(recent[1].relative_identifier, "brush_b");
}

TEST(asset_shelf_brush_lists, record_recent_into_trims_to_max_count)
{
  Vector<BrushAssetRef> recent;
  for (int i = 0; i < 5; i++) {
    record_recent_into(recent, make_ref("brush_" + std::to_string(i)), /*max_count=*/3);
  }

  ASSERT_EQ(recent.size(), 3);
  EXPECT_EQ(recent[0].relative_identifier, "brush_4");
  EXPECT_EQ(recent[1].relative_identifier, "brush_3");
  EXPECT_EQ(recent[2].relative_identifier, "brush_2");
}

TEST(asset_shelf_brush_lists, toggle_favorite_into_adds_then_removes)
{
  VectorSet<BrushAssetRef> favorites;
  const BrushAssetRef ref = make_ref("brush_a");

  toggle_favorite_into(favorites, ref);
  EXPECT_TRUE(favorites.contains(ref));

  toggle_favorite_into(favorites, ref);
  EXPECT_FALSE(favorites.contains(ref));
}

TEST(asset_shelf_brush_lists, json_round_trip_preserves_recent_and_favorites)
{
  Map<std::string, ShelfBrushLists> shelves;
  ShelfBrushLists lists;
  lists.recent.append(make_ref("brush_a"));
  lists.recent.append(make_ref("brush_b"));
  lists.favorites.add(make_ref("brush_c"));
  shelves.add("VIEW3D_AST_brush_sculpt", std::move(lists));

  const std::shared_ptr<io::serialize::DictionaryValue> json = lists_to_json(shelves);
  const Map<std::string, ShelfBrushLists> round_tripped = lists_from_json(*json);

  ASSERT_TRUE(round_tripped.contains("VIEW3D_AST_brush_sculpt"));
  const ShelfBrushLists &result = round_tripped.lookup("VIEW3D_AST_brush_sculpt");
  ASSERT_EQ(result.recent.size(), 2);
  EXPECT_EQ(result.recent[0].relative_identifier, "brush_a");
  EXPECT_EQ(result.recent[1].relative_identifier, "brush_b");
  EXPECT_TRUE(result.favorites.contains(make_ref("brush_c")));
}

TEST(asset_shelf_brush_lists, blend_filepath_scopes_local_refs_to_their_file)
{
  BrushAssetRef ref_a = make_ref("Clay");
  ref_a.blend_filepath = "/tmp/a.blend";
  BrushAssetRef ref_b = make_ref("Clay");
  ref_b.blend_filepath = "/tmp/b.blend";

  /* Same library type and ID name, different .blend file: two distinct local assets. */
  EXPECT_FALSE(ref_a == ref_b);

  Map<std::string, ShelfBrushLists> shelves;
  ShelfBrushLists lists;
  toggle_favorite_into(lists.favorites, ref_a);
  toggle_favorite_into(lists.favorites, ref_b);
  ASSERT_EQ(lists.favorites.size(), 2);
  shelves.add("VIEW3D_AST_brush_sculpt", std::move(lists));

  const std::shared_ptr<io::serialize::DictionaryValue> json = lists_to_json(shelves);
  const Map<std::string, ShelfBrushLists> round_tripped = lists_from_json(*json);

  ASSERT_TRUE(round_tripped.contains("VIEW3D_AST_brush_sculpt"));
  const ShelfBrushLists &result = round_tripped.lookup("VIEW3D_AST_brush_sculpt");
  ASSERT_EQ(result.favorites.size(), 2);
  EXPECT_EQ(result.favorites[0].blend_filepath, "/tmp/a.blend");
  EXPECT_EQ(result.favorites[1].blend_filepath, "/tmp/b.blend");
  EXPECT_TRUE(result.favorites.contains(ref_a));
  EXPECT_TRUE(result.favorites.contains(ref_b));
}

TEST(asset_shelf_brush_lists, lists_from_json_skips_malformed_entries)
{
  auto root = std::make_shared<io::serialize::DictionaryValue>();
  root->append_int("version", 1);
  const std::shared_ptr<io::serialize::DictionaryValue> shelves_dict = root->append_dict(
      "shelves");
  const std::shared_ptr<io::serialize::DictionaryValue> shelf_dict = shelves_dict->append_dict(
      "VIEW3D_AST_brush_sculpt");
  const std::shared_ptr<io::serialize::ArrayValue> recent_array = shelf_dict->append_array(
      "recent");
  /* Missing "relative_identifier" -- must be skipped, not crash. */
  const std::shared_ptr<io::serialize::DictionaryValue> malformed_entry =
      recent_array->append_dict();
  malformed_entry->append_int("asset_library_type", int64_t(ASSET_LIBRARY_LOCAL));

  const Map<std::string, ShelfBrushLists> result = lists_from_json(*root);

  ASSERT_TRUE(result.contains("VIEW3D_AST_brush_sculpt"));
  EXPECT_TRUE(result.lookup("VIEW3D_AST_brush_sculpt").recent.is_empty());
}

}  // namespace blender::ed::asset::shelf::tests
