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

TEST(name_match_filter_resolve, synthetic_map_tag_or_with_tokens)
{
  UserDef userdef = {};
  BKE_name_matching_userdef_ensure_defaults(&userdef);

  NameMatchFilterState state;
  state.enabled = true;
  state.active_map_type_ids.add("NORMAL");

  /* Asset tagged map:normal (lower-case) must pass NORMAL filter via synthetic tag. */
  {
    Vector<StringRef> meta_lower = {"map:normal"};
    EXPECT_TRUE(
        BKE_name_match_filter_asset_passes(state, userdef, "no_tokens_here", meta_lower));
  }

  /* Asset tagged MAP:NORMAL (upper-case) must also pass. */
  {
    Vector<StringRef> meta_upper = {"MAP:NORMAL"};
    EXPECT_TRUE(
        BKE_name_match_filter_asset_passes(state, userdef, "no_tokens_here", meta_upper));
  }

  /* Wrong map type tag must NOT pass NORMAL-only filter. */
  {
    Vector<StringRef> meta_rough = {"map:ROUGHNESS"};
    EXPECT_FALSE(
        BKE_name_match_filter_asset_passes(state, userdef, "no_tokens_here", meta_rough));
  }

  /* With BASE_COLOR also active, either tag passes. */
  state.active_map_type_ids.add("BASE_COLOR");
  {
    Vector<StringRef> meta_upper = {"MAP:NORMAL"};
    EXPECT_TRUE(
        BKE_name_match_filter_asset_passes(state, userdef, "no_tokens_here", meta_upper));
  }

  /* Filename BaseColor token must NOT satisfy NORMAL-only filter. */
  {
    NameMatchFilterState normal_only;
    normal_only.enabled = true;
    normal_only.active_map_type_ids.add("NORMAL");
    EXPECT_FALSE(
        BKE_name_match_filter_asset_passes(normal_only, userdef, "Wood_BaseColor.png", {}));
  }

  BKE_name_matching_userdef_free(&userdef);
}

TEST(name_match_filter_resolve, unknown_active_id_adds_nothing)
{
  UserDef userdef = {};
  BKE_name_matching_userdef_ensure_defaults(&userdef);
  NameMatchFilterState state;
  state.enabled = true;
  state.active_map_type_ids.add("NOT_A_REAL_MAP");
  NameMatchResolvedFilter resolved = BKE_name_match_filter_resolve(state, userdef);
  EXPECT_TRUE(resolved.token_lists.is_empty());
  EXPECT_TRUE(resolved.filter_tags.is_empty());
  EXPECT_FALSE(resolved.active);
  BKE_name_matching_userdef_free(&userdef);
}

}  // namespace blender::tests
