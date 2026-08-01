/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "interface_grid_view_settings_utils.hh"
#include "testing/testing.h"

namespace blender::ui::grid_settings::tests {

TEST(grid_view_settings, catalogs_round_trip_plain)
{
  Set<std::string> in;
  in.add("Characters");
  in.add("Environment/Rocks");
  Set<std::string> out;
  for (const std::string &s : catalogs_split(catalogs_join(in))) {
    out.add(s);
  }
  EXPECT_EQ(out.size(), 2);
  EXPECT_TRUE(out.contains("Characters"));
  EXPECT_TRUE(out.contains("Environment/Rocks"));
}

TEST(grid_view_settings, catalogs_round_trip_with_comma_and_backslash)
{
  Set<std::string> in;
  in.add("Props, Big");  /* contains a comma */
  in.add("Back\\slash"); /* contains a backslash */
  Set<std::string> out;
  for (const std::string &s : catalogs_split(catalogs_join(in))) {
    out.add(s);
  }
  EXPECT_EQ(out.size(), 2);
  EXPECT_TRUE(out.contains("Props, Big"));
  EXPECT_TRUE(out.contains("Back\\slash"));
}

TEST(grid_view_settings, catalogs_split_skips_empty_tokens)
{
  const Vector<std::string> out = catalogs_split(",A,,B,");
  EXPECT_EQ(out.size(), 2);
  EXPECT_EQ(out[0], "A");
  EXPECT_EQ(out[1], "B");
}

TEST(grid_view_settings, catalogs_split_tokens_deduplicate_without_ub)
{
  /* A malformed/duplicated string must merge under Set::add() (the fix replaces Set::add_new,
   * which is UB on a repeated key). */
  Set<std::string> out;
  for (std::string &s : catalogs_split("A,A")) {
    out.add(std::move(s));
  }
  EXPECT_EQ(out.size(), 1);
}

TEST(grid_view_settings, catalogs_by_library_round_trip_empty)
{
  Map<std::string, Set<std::string>> in;
  const std::string joined = catalogs_by_library_join(in);
  EXPECT_EQ(joined, "");
  const Map<std::string, Set<std::string>> out = catalogs_by_library_split(joined);
  EXPECT_EQ(out.size(), 0);
}

TEST(grid_view_settings, catalogs_by_library_round_trip_multi)
{
  Map<std::string, Set<std::string>> in;
  in.add("local", {"Characters", "Environment/Rocks"});
  in.add("MyLibrary", {"Props, Big"});
  /* A library with no active filter is never written (matches "empty = show all"). */
  in.add("essentials", {});

  const std::string joined = catalogs_by_library_join(in);
  const Map<std::string, Set<std::string>> out = catalogs_by_library_split(joined);

  EXPECT_EQ(out.size(), 2);
  ASSERT_TRUE(out.contains("local"));
  EXPECT_EQ(out.lookup("local").size(), 2);
  EXPECT_TRUE(out.lookup("local").contains("Characters"));
  EXPECT_TRUE(out.lookup("local").contains("Environment/Rocks"));
  ASSERT_TRUE(out.contains("MyLibrary"));
  EXPECT_TRUE(out.lookup("MyLibrary").contains("Props, Big"));
  EXPECT_FALSE(out.contains("essentials"));
}

TEST(grid_view_settings, catalogs_by_library_split_skips_malformed_segment)
{
  /* A segment with no unit separator (corrupt/hand-edited data) is skipped rather than crashing
   * or silently eating the rest of the string. */
  const std::string malformed = std::string("local") + '\x1f' + "Characters" + '\x1e' + "bogus";
  const Map<std::string, Set<std::string>> out = catalogs_by_library_split(malformed);
  EXPECT_EQ(out.size(), 1);
  EXPECT_TRUE(out.contains("local"));
}

TEST(grid_view_settings, name_match_map_types_join_split_round_trip)
{
  Set<std::string> in;
  in.add("BASE_COLOR");
  in.add("NORMAL");
  Set<std::string> out;
  for (std::string &s : split_comma_separated(name_match_map_types_join(in))) {
    out.add(std::move(s));
  }
  EXPECT_EQ(out.size(), 2);
  EXPECT_TRUE(out.contains("BASE_COLOR"));
  EXPECT_TRUE(out.contains("NORMAL"));
}

}  // namespace blender::ui::grid_settings::tests
