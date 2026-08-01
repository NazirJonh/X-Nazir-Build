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

}  // namespace blender::ui::grid_settings::tests
