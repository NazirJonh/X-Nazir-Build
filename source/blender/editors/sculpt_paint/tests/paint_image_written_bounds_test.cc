/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The one convention of a written-pixel rectangle: #WrittenBounds accumulates the texels a writer
 * stored and reports them exclusive, as the partial-update log and the undo tiles take them.
 */

#include "../paint_image_written_bounds.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::tests {

TEST(paint_image_written_bounds, an_empty_union_has_no_rectangle)
{
  const WrittenBounds bounds;
  EXPECT_TRUE(bounds.is_empty());
}

TEST(paint_image_written_bounds, one_texel_is_a_one_by_one_exclusive_rectangle)
{
  WrittenBounds bounds;
  bounds.add(3, 5);
  ASSERT_FALSE(bounds.is_empty());
  const rcti rect = bounds.as_exclusive_rcti();
  EXPECT_EQ(rect.xmin, 3);
  EXPECT_EQ(rect.xmax, 4);
  EXPECT_EQ(rect.ymin, 5);
  EXPECT_EQ(rect.ymax, 6);
}

TEST(paint_image_written_bounds, adjacent_texels_share_the_shared_edge)
{
  WrittenBounds bounds;
  bounds.add(0, 0);
  bounds.add(1, 0);
  const rcti rect = bounds.as_exclusive_rcti();
  EXPECT_EQ(rect.xmin, 0);
  EXPECT_EQ(rect.xmax, 2) << "the second texel is [1, 2), so the union's max is exclusive";
  EXPECT_EQ(rect.ymin, 0);
  EXPECT_EQ(rect.ymax, 1);
}

TEST(paint_image_written_bounds, merging_two_unions_keeps_the_far_corners)
{
  WrittenBounds first;
  first.add(2, 3);
  first.add(4, 6);
  WrittenBounds second;
  second.add(1, 8);
  second.add(7, 1);

  first.merge(second);
  const rcti rect = first.as_exclusive_rcti();
  EXPECT_EQ(rect.xmin, 1);
  EXPECT_EQ(rect.xmax, 8);
  EXPECT_EQ(rect.ymin, 1);
  EXPECT_EQ(rect.ymax, 9);
}

TEST(paint_image_written_bounds, merging_an_empty_union_changes_nothing)
{
  WrittenBounds bounds;
  bounds.add(5, 7);
  const WrittenBounds empty;
  bounds.merge(empty);
  ASSERT_FALSE(bounds.is_empty());
  const rcti rect = bounds.as_exclusive_rcti();
  EXPECT_EQ(rect.xmin, 5);
  EXPECT_EQ(rect.xmax, 6);
  EXPECT_EQ(rect.ymin, 7);
  EXPECT_EQ(rect.ymax, 8);
}

TEST(paint_image_written_bounds, merging_into_an_empty_union_takes_the_other)
{
  WrittenBounds bounds;
  WrittenBounds other;
  other.add(9, 2);
  bounds.merge(other);
  ASSERT_FALSE(bounds.is_empty());
  const rcti rect = bounds.as_exclusive_rcti();
  EXPECT_EQ(rect.xmin, 9);
  EXPECT_EQ(rect.xmax, 10);
  EXPECT_EQ(rect.ymin, 2);
  EXPECT_EQ(rect.ymax, 3);
}

}  // namespace blender::ed::sculpt_paint::tests
