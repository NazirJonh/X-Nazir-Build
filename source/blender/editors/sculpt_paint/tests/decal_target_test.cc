/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstring>
#include <string>

#include "BLI_string_utf8.h"

#include "../decal/decal_target.hh"

namespace blender::ed::sculpt_paint::decal::tests {

TEST(decal_row_name, round_trips_decal_and_channel)
{
  char name[64];
  decal_row_name_make("Scratch", "Base Color", name);
  EXPECT_STREQ(name, "Scratch \xC2\xB7 Base Color");

  char decal[64];
  ASSERT_TRUE(decal_name_of_row_name(name, decal));
  EXPECT_STREQ(decal, "Scratch");
}

TEST(decal_row_name, a_decal_name_with_the_separator_is_read_back_whole)
{
  char name[64];
  decal_row_name_make("a \xC2\xB7 b", "Normal", name);

  char decal[64];
  ASSERT_TRUE(decal_name_of_row_name(name, decal));
  EXPECT_STREQ(decal, "a \xC2\xB7 b");
}

TEST(decal_row_name, an_empty_decal_reads_as_decal)
{
  char name[64];
  decal_row_name_make("", "Roughness", name);

  char decal[64];
  ASSERT_TRUE(decal_name_of_row_name(name, decal));
  EXPECT_STREQ(decal, "Decal");
}

TEST(decal_row_name, a_name_without_a_decal_part_is_not_a_decal_row)
{
  char decal[64];
  EXPECT_FALSE(decal_name_of_row_name("Plain Layer", decal));
  EXPECT_FALSE(decal_name_of_row_name(" \xC2\xB7 Base Color", decal));
}

TEST(decal_row_name, a_long_decal_is_cut_on_a_character_boundary_and_keeps_its_channel)
{
  /* 40 two-byte characters: 80 bytes, past the 63 the name holds. */
  std::string long_decal;
  for (int i = 0; i < 40; i++) {
    long_decal += "\xC3\xA9";
  }
  char name[64];
  decal_row_name_make(long_decal.c_str(), "Base Color", name);
  EXPECT_LT(strlen(name), size_t(64));
  EXPECT_EQ(BLI_str_utf8_invalid_strip(name, strlen(name)), 0) << "no character is cut in half";

  char decal[64];
  ASSERT_TRUE(decal_name_of_row_name(name, decal));
  EXPECT_EQ(std::string(name).rfind("Base Color"), strlen(name) - strlen("Base Color"));
  EXPECT_EQ(long_decal.rfind(decal, 0), size_t(0)) << "the cut decal is a prefix of the original";
}

}  // namespace blender::ed::sculpt_paint::decal::tests
