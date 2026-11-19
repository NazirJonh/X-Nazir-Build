/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_blender.hh"
#include "BKE_preferences.h"

#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::bke::tests {

static Vector<std::string> paint_add_favorite_identifiers(const UserDef &userdef)
{
  Vector<std::string> identifiers;
  for (const bUserPaintAddFavorite &favorite : userdef.paint_add_favorites) {
    identifiers.append(favorite.identifier);
  }
  return identifiers;
}

static void expect_paint_add_favorite_order(const UserDef &userdef,
                                            const Span<const char *> expected)
{
  const Vector<std::string> identifiers = paint_add_favorite_identifiers(userdef);
  ASSERT_EQ(identifiers.size(), expected.size());
  for (const int64_t i : identifiers.index_range()) {
    EXPECT_EQ(identifiers[i], expected[i]);
  }
}

TEST(preferences, PaintAddFavoriteAddAppendsInOrderAndDoesNotDuplicate)
{
  UserDef userdef{};

  bUserPaintAddFavorite *alpha = BKE_preferences_paint_add_favorite_add(&userdef, "Alpha");
  ASSERT_NE(alpha, nullptr);
  EXPECT_STREQ(alpha->identifier, "Alpha");

  bUserPaintAddFavorite *beta = BKE_preferences_paint_add_favorite_add(&userdef, "Beta");
  ASSERT_NE(beta, nullptr);
  EXPECT_STREQ(beta->identifier, "Beta");
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 2);
  expect_paint_add_favorite_order(userdef, {"Alpha", "Beta"});

  /* Adding an identifier that is already pinned returns the existing node, it does not duplicate. */
  EXPECT_EQ(BKE_preferences_paint_add_favorite_add(&userdef, "Alpha"), alpha);
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 2);
  expect_paint_add_favorite_order(userdef, {"Alpha", "Beta"});

  EXPECT_TRUE(BKE_preferences_paint_add_favorite_contains(&userdef, "Alpha"));
  EXPECT_TRUE(BKE_preferences_paint_add_favorite_contains(&userdef, "Beta"));
  EXPECT_EQ(BKE_preferences_paint_add_favorite_find(&userdef, "Beta"), beta);
  EXPECT_EQ(BKE_preferences_paint_add_favorite_find(&userdef, "Missing"), nullptr);

  BKE_preferences_paint_add_favorite_clear(&userdef);
}

TEST(preferences, PaintAddFavoriteAddEmptyIdentifierReturnsNullAndChangesNothing)
{
  UserDef userdef{};

  EXPECT_EQ(BKE_preferences_paint_add_favorite_add(&userdef, ""), nullptr);
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 0);
  EXPECT_FALSE(BKE_preferences_paint_add_favorite_contains(&userdef, ""));

  /* The rejection applies just the same when other favorites are already pinned. */
  EXPECT_NE(BKE_preferences_paint_add_favorite_add(&userdef, "Alpha"), nullptr);
  EXPECT_EQ(BKE_preferences_paint_add_favorite_add(&userdef, ""), nullptr);
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 1);
  expect_paint_add_favorite_order(userdef, {"Alpha"});

  BKE_preferences_paint_add_favorite_clear(&userdef);
}

TEST(preferences, PaintAddFavoriteRemoveExistingAndMissing)
{
  UserDef userdef{};

  BKE_preferences_paint_add_favorite_add(&userdef, "Alpha");
  BKE_preferences_paint_add_favorite_add(&userdef, "Beta");
  BKE_preferences_paint_add_favorite_add(&userdef, "Gamma");

  EXPECT_TRUE(BKE_preferences_paint_add_favorite_remove(&userdef, "Beta"));
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 2);
  expect_paint_add_favorite_order(userdef, {"Alpha", "Gamma"});
  EXPECT_FALSE(BKE_preferences_paint_add_favorite_contains(&userdef, "Beta"));

  EXPECT_FALSE(BKE_preferences_paint_add_favorite_remove(&userdef, "Beta"));
  EXPECT_FALSE(BKE_preferences_paint_add_favorite_remove(&userdef, ""));
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 2);
  expect_paint_add_favorite_order(userdef, {"Alpha", "Gamma"});

  BKE_preferences_paint_add_favorite_clear(&userdef);
}

TEST(preferences, PaintAddFavoriteMoveEdgePositionsReturnFalseAndKeepOrder)
{
  UserDef userdef{};

  BKE_preferences_paint_add_favorite_add(&userdef, "Alpha");
  BKE_preferences_paint_add_favorite_add(&userdef, "Beta");
  BKE_preferences_paint_add_favorite_add(&userdef, "Gamma");

  /* A move that would leave the favorite where it already is reports false and changes nothing. */
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Alpha", PAINT_ADD_FAVORITE_MOVE_UP));
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Alpha", PAINT_ADD_FAVORITE_MOVE_TOP));
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Gamma", PAINT_ADD_FAVORITE_MOVE_DOWN));
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Gamma", PAINT_ADD_FAVORITE_MOVE_BOTTOM));
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Missing", PAINT_ADD_FAVORITE_MOVE_UP));
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 3);
  expect_paint_add_favorite_order(userdef, {"Alpha", "Beta", "Gamma"});

  /* With a single favorite clamped targets equal the own index, so every direction is a no-op. */
  BKE_preferences_paint_add_favorite_remove(&userdef, "Alpha");
  BKE_preferences_paint_add_favorite_remove(&userdef, "Gamma");
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Beta", PAINT_ADD_FAVORITE_MOVE_UP));
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Beta", PAINT_ADD_FAVORITE_MOVE_DOWN));
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Beta", PAINT_ADD_FAVORITE_MOVE_TOP));
  EXPECT_FALSE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Beta", PAINT_ADD_FAVORITE_MOVE_BOTTOM));
  expect_paint_add_favorite_order(userdef, {"Beta"});

  BKE_preferences_paint_add_favorite_clear(&userdef);
}

TEST(preferences, PaintAddFavoriteMoveUpAndDownSwapWithNeighbor)
{
  UserDef userdef{};

  BKE_preferences_paint_add_favorite_add(&userdef, "Alpha");
  bUserPaintAddFavorite *beta = BKE_preferences_paint_add_favorite_add(&userdef, "Beta");
  BKE_preferences_paint_add_favorite_add(&userdef, "Gamma");

  EXPECT_TRUE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Beta", PAINT_ADD_FAVORITE_MOVE_UP));
  expect_paint_add_favorite_order(userdef, {"Beta", "Alpha", "Gamma"});
  /* Moves reorder the existing nodes instead of recreating them. */
  EXPECT_EQ(BKE_preferences_paint_add_favorite_find(&userdef, "Beta"), beta);

  EXPECT_TRUE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Beta", PAINT_ADD_FAVORITE_MOVE_DOWN));
  expect_paint_add_favorite_order(userdef, {"Alpha", "Gamma", "Beta"});

  BKE_preferences_paint_add_favorite_clear(&userdef);
}

TEST(preferences, PaintAddFavoriteMoveTopAndBottomJumpToEnds)
{
  UserDef userdef{};

  BKE_preferences_paint_add_favorite_add(&userdef, "Alpha");
  BKE_preferences_paint_add_favorite_add(&userdef, "Beta");
  BKE_preferences_paint_add_favorite_add(&userdef, "Gamma");
  expect_paint_add_favorite_order(userdef, {"Alpha", "Beta", "Gamma"});

  EXPECT_TRUE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Gamma", PAINT_ADD_FAVORITE_MOVE_TOP));
  expect_paint_add_favorite_order(userdef, {"Gamma", "Alpha", "Beta"});

  EXPECT_TRUE(
      BKE_preferences_paint_add_favorite_move(&userdef, "Gamma", PAINT_ADD_FAVORITE_MOVE_BOTTOM));
  expect_paint_add_favorite_order(userdef, {"Alpha", "Beta", "Gamma"});

  BKE_preferences_paint_add_favorite_clear(&userdef);
}

TEST(preferences, PaintAddFavoriteClearEmptiesTheList)
{
  UserDef userdef{};

  BKE_preferences_paint_add_favorite_add(&userdef, "Alpha");
  BKE_preferences_paint_add_favorite_add(&userdef, "Beta");
  BKE_preferences_paint_add_favorite_clear(&userdef);

  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 0);
  EXPECT_FALSE(BKE_preferences_paint_add_favorite_contains(&userdef, "Alpha"));
  EXPECT_EQ(BKE_preferences_paint_add_favorite_find(&userdef, "Beta"), nullptr);

  /* The list stays usable after a clear. */
  BKE_preferences_paint_add_favorite_add(&userdef, "Gamma");
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 1);
  expect_paint_add_favorite_order(userdef, {"Gamma"});

  BKE_preferences_paint_add_favorite_clear(&userdef);
}

TEST(preferences, PaintAddFavoriteRemovePtrTakesTheFavoriteItself)
{
  UserDef userdef{};

  BKE_preferences_paint_add_favorite_add(&userdef, "Alpha");
  bUserPaintAddFavorite *beta = BKE_preferences_paint_add_favorite_add(&userdef, "Beta");
  BKE_preferences_paint_add_favorite_add(&userdef, "Gamma");

  /* The pointer an RNA collection hands back unpin that favorite directly. */
  EXPECT_TRUE(BKE_preferences_paint_add_favorite_remove_ptr(&userdef, beta));
  expect_paint_add_favorite_order(userdef, {"Alpha", "Gamma"});
  EXPECT_FALSE(BKE_preferences_paint_add_favorite_contains(&userdef, "Beta"));

  /* A pointer that is not in this list (a favorite of another UserDef, or the invalidated wrapper
   * of one removed already) is refused, not freed. */
  UserDef other{};
  bUserPaintAddFavorite *foreign = BKE_preferences_paint_add_favorite_add(&other, "Foreign");
  EXPECT_FALSE(BKE_preferences_paint_add_favorite_remove_ptr(&userdef, foreign));
  EXPECT_EQ(BLI_listbase_count(&userdef.paint_add_favorites), 2);
  EXPECT_EQ(BLI_listbase_count(&other.paint_add_favorites), 1);

  BKE_preferences_paint_add_favorite_clear(&userdef);
  BKE_preferences_paint_add_favorite_clear(&other);
}

TEST(preferences, PaintAddFavoriteSyncReplacesLikeTheUserMenus)
{
  UserDef userdef_dst{};
  UserDef userdef_src{};

  BKE_preferences_paint_add_favorite_add(&userdef_dst, "Running");
  BKE_preferences_paint_add_favorite_add(&userdef_src, "Saved");
  BKE_preferences_paint_add_favorite_add(&userdef_src, "SavedSecond");

  /* The source is the configuration to become: the running pins go, the source's order arrives. */
  BKE_blender_userdef_sync_from(&userdef_dst, &userdef_src, USER_SYNC_FAVORITES);
  expect_paint_add_favorite_order(userdef_dst, {"Saved", "SavedSecond"});
  /* The nodes moved, not copied: the source's list is emptied by the move. */
  EXPECT_EQ(BLI_listbase_count(&userdef_src.paint_add_favorites), 0);

  /* An empty source (Load Factory Settings reads one) empties the running favorites, rather than
   * merging the pins over nothing. */
  BKE_blender_userdef_sync_from(&userdef_dst, &userdef_src, USER_SYNC_FAVORITES);
  EXPECT_EQ(BLI_listbase_count(&userdef_dst.paint_add_favorites), 0);

  BKE_blender_userdef_data_free(&userdef_dst, false);
  BKE_blender_userdef_data_free(&userdef_src, false);
}

TEST(preferences, PaintAddFavoriteSyncOnlyWithCorrectFlag)
{
  UserDef userdef_dst{};
  UserDef userdef_src{};

  BKE_preferences_paint_add_favorite_add(&userdef_dst, "Original");
  BKE_preferences_paint_add_favorite_add(&userdef_src, "Source");

  /* Sync without #USER_SYNC_FAVORITES must not touch the favorites. */
  BKE_blender_userdef_sync_from(&userdef_dst, &userdef_src, USER_SYNC_THEME | USER_SYNC_PATHS);
  expect_paint_add_favorite_order(userdef_dst, {"Original"});
  expect_paint_add_favorite_order(userdef_src, {"Source"});

  /* Sync with #USER_SYNC_FAVORITES replaces the list. */
  BKE_blender_userdef_sync_from(&userdef_dst, &userdef_src, USER_SYNC_FAVORITES);
  expect_paint_add_favorite_order(userdef_dst, {"Source"});
  /* The nodes moved, not copied: the source's list ends up empty. */
  EXPECT_EQ(BLI_listbase_count(&userdef_src.paint_add_favorites), 0);

  BKE_blender_userdef_data_free(&userdef_dst, false);
  BKE_blender_userdef_data_free(&userdef_src, false);
}

}  // namespace blender::bke::tests
