/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_name_matching.hh"

#include "testing/testing.h"

namespace blender::tests {

TEST(name_matching, normalize_strips_duplicate_then_extension)
{
  EXPECT_EQ(BLI_name_matching_normalize_asset_name("Wood_Normal.png.001"), "Wood_Normal");
  EXPECT_EQ(BLI_name_matching_normalize_asset_name("Hero_N.exr"), "Hero_N");
}

TEST(name_matching, segment_prefix_postfix)
{
  EXPECT_TRUE(BLI_name_matching_token_matches("T_Hero_N", "N"));
  EXPECT_TRUE(BLI_name_matching_token_matches("N_Rock", "N"));
  EXPECT_TRUE(BLI_name_matching_token_matches("rock_normal_4k", "normal"));
  EXPECT_TRUE(BLI_name_matching_token_matches("BaseColor", "BaseColor"));
}

TEST(name_matching, north_wall_rejects_normal_default_tokens)
{
  const char *tokens[] = {"N", "nor", "norm", "normal"};
  for (const char *t : tokens) {
    EXPECT_FALSE(BLI_name_matching_token_matches("north_wall", t)) << t;
  }
}

TEST(name_matching, case_insensitive)
{
  EXPECT_TRUE(BLI_name_matching_token_matches("hero_basecolor", "BaseColor"));
}

TEST(name_matching, include_or_and_empty_passthrough)
{
  /* Empty actives → pass. */
  EXPECT_TRUE(BLI_name_matching_asset_passes_include_filter("x", {}, {}, {}));

  const StringRef normal_tokens[] = {"N", "nor", "norm", "normal"};
  const Span<StringRef> map_lists_match[] = {Span(normal_tokens)};
  EXPECT_TRUE(BLI_name_matching_asset_passes_include_filter(
      "Wood_Normal.png.001", {}, Span(map_lists_match), {}));

  const StringRef roughness_tokens[] = {"R", "rough", "roughness"};
  const Span<StringRef> map_lists_no_match[] = {Span(roughness_tokens)};
  EXPECT_FALSE(BLI_name_matching_asset_passes_include_filter(
      "Wood_Normal.png.001", {}, Span(map_lists_no_match), {}));

  /* OR: map type misses, but active filter tag matches metadata. */
  const StringRef metadata_tags[] = {"tileable"};
  const StringRef active_tags[] = {"tileable"};
  EXPECT_TRUE(BLI_name_matching_asset_passes_include_filter(
      "Wood_Normal.png.001", Span(metadata_tags), Span(map_lists_no_match), Span(active_tags)));

  /* OR: filter tag segment-matches normalized name. */
  const StringRef active_name_tags[] = {"Wood"};
  EXPECT_TRUE(BLI_name_matching_asset_passes_include_filter(
      "Wood_Normal.png.001", {}, Span(map_lists_no_match), Span(active_name_tags)));
}

TEST(name_matching, empty_token_never_matches)
{
  EXPECT_FALSE(BLI_name_matching_token_matches("anything", ""));
}

}  // namespace blender::tests
