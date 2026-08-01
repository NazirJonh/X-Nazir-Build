/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "asset_shelf_asset_lists.hh"

#include "BLI_serialize.hh"

#include "testing/testing.h"

namespace blender::ed::asset::shelf::tests {

static ShelfAssetRef make_ref(const std::string &relative_identifier)
{
  ShelfAssetRef ref;
  ref.library_type = ASSET_LIBRARY_LOCAL;
  ref.relative_identifier = relative_identifier;
  return ref;
}

TEST(asset_shelf_asset_lists, record_recent_into_prepends_new_entry)
{
  Vector<ShelfAssetRef> recent;
  record_recent_into(recent, make_ref("brush_a"), SHELF_ASSET_LISTS_RECENT_MAX);
  record_recent_into(recent, make_ref("brush_b"), SHELF_ASSET_LISTS_RECENT_MAX);

  ASSERT_EQ(recent.size(), 2);
  EXPECT_EQ(recent[0].relative_identifier, "brush_b");
  EXPECT_EQ(recent[1].relative_identifier, "brush_a");
}

TEST(asset_shelf_asset_lists, record_recent_into_moves_existing_entry_to_front)
{
  Vector<ShelfAssetRef> recent;
  record_recent_into(recent, make_ref("brush_a"), SHELF_ASSET_LISTS_RECENT_MAX);
  record_recent_into(recent, make_ref("brush_b"), SHELF_ASSET_LISTS_RECENT_MAX);
  record_recent_into(recent, make_ref("brush_a"), SHELF_ASSET_LISTS_RECENT_MAX);

  ASSERT_EQ(recent.size(), 2);
  EXPECT_EQ(recent[0].relative_identifier, "brush_a");
  EXPECT_EQ(recent[1].relative_identifier, "brush_b");
}

TEST(asset_shelf_asset_lists, record_recent_into_trims_to_max_count)
{
  Vector<ShelfAssetRef> recent;
  for (int i = 0; i < 5; i++) {
    record_recent_into(recent, make_ref("brush_" + std::to_string(i)), /*max_count=*/3);
  }

  ASSERT_EQ(recent.size(), 3);
  EXPECT_EQ(recent[0].relative_identifier, "brush_4");
  EXPECT_EQ(recent[1].relative_identifier, "brush_3");
  EXPECT_EQ(recent[2].relative_identifier, "brush_2");
}

TEST(asset_shelf_asset_lists, toggle_favorite_into_adds_then_removes)
{
  VectorSet<ShelfAssetRef> favorites;
  const ShelfAssetRef ref = make_ref("brush_a");

  toggle_favorite_into(favorites, ref);
  EXPECT_TRUE(favorites.contains(ref));

  toggle_favorite_into(favorites, ref);
  EXPECT_FALSE(favorites.contains(ref));
}

TEST(asset_shelf_asset_lists, reorder_favorite_into_moves_entry)
{
  VectorSet<ShelfAssetRef> favorites;
  const ShelfAssetRef ref_a = make_ref("A");
  const ShelfAssetRef ref_b = make_ref("B");
  const ShelfAssetRef ref_c = make_ref("C");
  favorites.add(ref_a);
  favorites.add(ref_b);
  favorites.add(ref_c);

  EXPECT_TRUE(reorder_favorite_into(favorites, ref_b, 0));
  ASSERT_EQ(favorites.size(), 3);
  EXPECT_EQ(favorites[0].relative_identifier, "B");
  EXPECT_EQ(favorites[1].relative_identifier, "A");
  EXPECT_EQ(favorites[2].relative_identifier, "C");
}

TEST(asset_shelf_asset_lists, reorder_favorite_into_noop_when_missing)
{
  VectorSet<ShelfAssetRef> favorites;
  EXPECT_FALSE(reorder_favorite_into(favorites, make_ref("x"), 0));
}

TEST(asset_shelf_asset_lists, reorder_favorite_into_noop_when_same_index)
{
  VectorSet<ShelfAssetRef> favorites;
  const ShelfAssetRef ref_a = make_ref("A");
  favorites.add(ref_a);

  EXPECT_FALSE(reorder_favorite_into(favorites, ref_a, 0));
  EXPECT_EQ(favorites.size(), 1);
}

TEST(asset_shelf_asset_lists, reorder_favorite_into_clamps_index)
{
  VectorSet<ShelfAssetRef> favorites;
  const ShelfAssetRef ref_a = make_ref("A");
  const ShelfAssetRef ref_b = make_ref("B");
  favorites.add(ref_a);
  favorites.add(ref_b);

  EXPECT_TRUE(reorder_favorite_into(favorites, ref_a, 99));
  ASSERT_EQ(favorites.size(), 2);
  EXPECT_EQ(favorites[0].relative_identifier, "B");
  EXPECT_EQ(favorites[1].relative_identifier, "A");
}

TEST(asset_shelf_asset_lists, reorder_favorite_into_five_elements)
{
  VectorSet<ShelfAssetRef> favorites;
  const ShelfAssetRef ref_a = make_ref("A");
  const ShelfAssetRef ref_b = make_ref("B");
  const ShelfAssetRef ref_c = make_ref("C");
  const ShelfAssetRef ref_d = make_ref("D");
  const ShelfAssetRef ref_e = make_ref("E");
  favorites.add(ref_a);
  favorites.add(ref_b);
  favorites.add(ref_c);
  favorites.add(ref_d);
  favorites.add(ref_e);

  EXPECT_TRUE(reorder_favorite_into(favorites, ref_a, 3));
  ASSERT_EQ(favorites.size(), 5);
  EXPECT_EQ(favorites[0].relative_identifier, "B");
  EXPECT_EQ(favorites[1].relative_identifier, "C");
  EXPECT_EQ(favorites[2].relative_identifier, "D");
  EXPECT_EQ(favorites[3].relative_identifier, "A");
  EXPECT_EQ(favorites[4].relative_identifier, "E");
}

TEST(asset_shelf_asset_lists, favorite_index_from_identifier_finds_match)
{
  VectorSet<ShelfAssetRef> favorites;
  favorites.add(make_ref("brush_a"));
  favorites.add(make_ref("brush_b"));
  favorites.add(make_ref("brush_c"));

  EXPECT_EQ(favorite_index_from_identifier(favorites.as_span(), "brush_a"), 0);
  EXPECT_EQ(favorite_index_from_identifier(favorites.as_span(), "brush_b"), 1);
  EXPECT_EQ(favorite_index_from_identifier(favorites.as_span(), "brush_c"), 2);
}

TEST(asset_shelf_asset_lists, favorite_index_from_identifier_returns_minus_one_when_missing)
{
  VectorSet<ShelfAssetRef> favorites;
  favorites.add(make_ref("brush_a"));
  favorites.add(make_ref("brush_b"));

  EXPECT_EQ(favorite_index_from_identifier(favorites.as_span(), "missing"), -1);
  EXPECT_EQ(favorite_index_from_identifier(Span<ShelfAssetRef>(), "brush_a"), -1);
}

TEST(asset_shelf_asset_lists, json_round_trip_preserves_recent_and_favorites)
{
  Map<std::string, ShelfAssetLists> shelves;
  ShelfAssetLists lists;
  lists.recent.append(make_ref("brush_a"));
  lists.recent.append(make_ref("brush_b"));
  lists.favorites.add(make_ref("brush_c"));
  lists.favorites.add(make_ref("brush_d"));
  lists.favorites.add(make_ref("brush_e"));
  shelves.add("VIEW3D_AST_brush_sculpt", std::move(lists));

  const std::shared_ptr<io::serialize::DictionaryValue> json = lists_to_json(shelves);
  const Map<std::string, ShelfAssetLists> round_tripped = lists_from_json(*json);

  ASSERT_TRUE(round_tripped.contains("VIEW3D_AST_brush_sculpt"));
  const ShelfAssetLists &result = round_tripped.lookup("VIEW3D_AST_brush_sculpt");
  ASSERT_EQ(result.recent.size(), 2);
  EXPECT_EQ(result.recent[0].relative_identifier, "brush_a");
  EXPECT_EQ(result.recent[1].relative_identifier, "brush_b");
  ASSERT_EQ(result.favorites.size(), 3);
  EXPECT_EQ(result.favorites[0].relative_identifier, "brush_c");
  EXPECT_EQ(result.favorites[1].relative_identifier, "brush_d");
  EXPECT_EQ(result.favorites[2].relative_identifier, "brush_e");
}

TEST(asset_shelf_asset_lists, blend_filepath_scopes_local_refs_to_their_file)
{
  ShelfAssetRef ref_a = make_ref("Clay");
  ref_a.blend_filepath = "/tmp/a.blend";
  ShelfAssetRef ref_b = make_ref("Clay");
  ref_b.blend_filepath = "/tmp/b.blend";

  /* Same library type and ID name, different .blend file: two distinct local assets. */
  EXPECT_FALSE(ref_a == ref_b);

  Map<std::string, ShelfAssetLists> shelves;
  ShelfAssetLists lists;
  toggle_favorite_into(lists.favorites, ref_a);
  toggle_favorite_into(lists.favorites, ref_b);
  ASSERT_EQ(lists.favorites.size(), 2);
  shelves.add("VIEW3D_AST_brush_sculpt", std::move(lists));

  const std::shared_ptr<io::serialize::DictionaryValue> json = lists_to_json(shelves);
  const Map<std::string, ShelfAssetLists> round_tripped = lists_from_json(*json);

  ASSERT_TRUE(round_tripped.contains("VIEW3D_AST_brush_sculpt"));
  const ShelfAssetLists &result = round_tripped.lookup("VIEW3D_AST_brush_sculpt");
  ASSERT_EQ(result.favorites.size(), 2);
  EXPECT_EQ(result.favorites[0].blend_filepath, "/tmp/a.blend");
  EXPECT_EQ(result.favorites[1].blend_filepath, "/tmp/b.blend");
  EXPECT_TRUE(result.favorites.contains(ref_a));
  EXPECT_TRUE(result.favorites.contains(ref_b));
}

TEST(asset_shelf_asset_lists, shelf_supports_asset_lists_brush_and_image)
{
  EXPECT_TRUE(shelf_supports_asset_lists("VIEW3D_AST_brush_sculpt"));
  EXPECT_TRUE(shelf_supports_asset_lists("VIEW3D_AST_image_texture"));
  EXPECT_FALSE(shelf_supports_asset_lists("VIEW3D_AST_something_else"));
}

TEST(asset_shelf_asset_lists, lists_from_json_skips_malformed_entries)
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

  const Map<std::string, ShelfAssetLists> result = lists_from_json(*root);

  ASSERT_TRUE(result.contains("VIEW3D_AST_brush_sculpt"));
  EXPECT_TRUE(result.lookup("VIEW3D_AST_brush_sculpt").recent.is_empty());
}

}  // namespace blender::ed::asset::shelf::tests
