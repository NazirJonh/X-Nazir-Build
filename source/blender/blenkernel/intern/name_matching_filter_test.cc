/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_name_matching.hh"
#include "DNA_userdef_types.h"
#include "MEM_guardedalloc.h"
#include "testing/testing.h"

namespace blender::tests {

TEST(name_match_filter_state, is_active_requires_enabled_and_ids)
{
  NameMatchFilterState state;
  EXPECT_FALSE(BKE_name_match_filter_is_active(state));
  state.enabled = true;
  EXPECT_FALSE(BKE_name_match_filter_is_active(state));
  state.active_map_type_ids.add("NORMAL");
  EXPECT_TRUE(BKE_name_match_filter_is_active(state));
}

TEST(name_match_filter_state, clear_keeps_enabled)
{
  NameMatchFilterState state;
  state.enabled = true;
  state.active_map_type_ids.add("NORMAL");
  BKE_name_match_filter_clear_selection(state);
  EXPECT_TRUE(state.enabled);
  EXPECT_TRUE(state.active_map_type_ids.is_empty());
}

TEST(name_match_filter_state, toggle_map_type)
{
  NameMatchFilterState state;
  BKE_name_match_filter_toggle_map_type(state, "BASE_COLOR");
  EXPECT_TRUE(BKE_name_match_filter_map_type_is_active(state, "BASE_COLOR"));
  BKE_name_match_filter_toggle_map_type(state, "BASE_COLOR");
  EXPECT_FALSE(BKE_name_match_filter_map_type_is_active(state, "BASE_COLOR"));
}

TEST(name_matching, map_type_identifier_rejects_comma)
{
  UserDef userdef = {};
  EXPECT_EQ(BKE_name_matching_map_type_add(&userdef, "Bad", "A,B", 0), nullptr);
  bUserNameMatchMapType *ok = BKE_name_matching_map_type_add(&userdef, "Ok", "OK_ID", 0);
  ASSERT_NE(ok, nullptr);
  BKE_name_matching_map_type_identifier_set(&userdef, ok, "STILL,BAD");
  EXPECT_STREQ(ok->identifier, "OK_ID"); /* unchanged */
  BKE_name_matching_userdef_free(&userdef);
}

TEST(name_match_filter_state, asset_passes_empty_or_disabled)
{
  UserDef userdef = {};
  BKE_name_matching_userdef_ensure_defaults(&userdef);
  NameMatchFilterState state;
  EXPECT_TRUE(BKE_name_match_filter_asset_passes(state, userdef, "Wood_Normal", {}));
  state.enabled = true;
  EXPECT_TRUE(BKE_name_match_filter_asset_passes(state, userdef, "Wood_Normal", {}));
  state.active_map_type_ids.add("NORMAL");
  EXPECT_TRUE(BKE_name_match_filter_asset_passes(state, userdef, "Wood_Normal", {}));
  EXPECT_FALSE(BKE_name_match_filter_asset_passes(state, userdef, "Wood_BaseColor", {}));
  BKE_name_matching_userdef_free(&userdef);
}

}  // namespace blender::tests
