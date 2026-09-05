/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_rect.h"

namespace blender::tests {

static rcti make_rcti(const int xmin, const int xmax, const int ymin, const int ymax)
{
  rcti rect;
  BLI_rcti_init(&rect, xmin, xmax, ymin, ymax);
  return rect;
}

/* -------------------------------------------------------------------- */
/** \name Union Exactness
 * \{ */

TEST(rct, union_is_exact_containment)
{
  const rcti outer = make_rcti(0, 10, 0, 10);
  const rcti inner = make_rcti(2, 8, 2, 8);
  EXPECT_TRUE(BLI_rcti_union_is_exact(&outer, &inner));
  EXPECT_TRUE(BLI_rcti_union_is_exact(&inner, &outer));
  EXPECT_TRUE(BLI_rcti_union_is_exact(&outer, &outer));
}

TEST(rct, union_is_exact_when_stacked_on_a_shared_axis)
{
  /* Same x extent, meeting on y: the union is the tall rectangle and claims nothing extra. */
  const rcti lower = make_rcti(0, 10, 0, 5);
  const rcti upper = make_rcti(0, 10, 5, 12);
  EXPECT_TRUE(BLI_rcti_union_is_exact(&lower, &upper));

  /* Same y extent, meeting on x. */
  const rcti left = make_rcti(0, 5, 0, 10);
  const rcti right = make_rcti(5, 12, 0, 10);
  EXPECT_TRUE(BLI_rcti_union_is_exact(&left, &right));

  /* Overlapping on the shared axis is still exact. */
  const rcti overlapping = make_rcti(0, 10, 3, 12);
  EXPECT_TRUE(BLI_rcti_union_is_exact(&lower, &overlapping));
}

TEST(rct, union_is_not_exact_when_they_only_share_a_corner_region)
{
  /* The case the predicate exists for: these overlap, but their bounding box also claims two
   * corners that neither rectangle covers. */
  const rcti a = make_rcti(0, 10, 0, 10);
  const rcti b = make_rcti(5, 15, 5, 15);
  EXPECT_FALSE(BLI_rcti_union_is_exact(&a, &b));
  EXPECT_FALSE(BLI_rcti_union_is_exact(&b, &a));
}

TEST(rct, union_is_not_exact_when_a_shared_axis_leaves_a_gap)
{
  /* Same x extent, but disjoint on y with a gap between them: the union fills the gap. */
  const rcti lower = make_rcti(0, 10, 0, 4);
  const rcti upper = make_rcti(0, 10, 6, 12);
  EXPECT_FALSE(BLI_rcti_union_is_exact(&lower, &upper));
}

TEST(rct, union_is_not_exact_for_disjoint_rectangles)
{
  const rcti a = make_rcti(0, 4, 0, 4);
  const rcti b = make_rcti(10, 14, 10, 14);
  EXPECT_FALSE(BLI_rcti_union_is_exact(&a, &b));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Difference Bounds
 * \{ */

TEST(rct, difference_bounds_disjoint_keeps_the_whole_rectangle)
{
  const rcti a = make_rcti(0, 10, 0, 10);
  const rcti b = make_rcti(20, 30, 20, 30);
  rcti result;
  EXPECT_TRUE(BLI_rcti_difference_bounds(&a, &b, &result));
  EXPECT_EQ(result.xmin, 0);
  EXPECT_EQ(result.xmax, 10);
  EXPECT_EQ(result.ymin, 0);
  EXPECT_EQ(result.ymax, 10);
}

TEST(rct, difference_bounds_fully_covered_is_empty)
{
  const rcti a = make_rcti(2, 8, 2, 8);
  const rcti b = make_rcti(0, 10, 0, 10);
  rcti result;
  EXPECT_TRUE(BLI_rcti_difference_bounds(&a, &b, &result));
  EXPECT_TRUE(BLI_rcti_is_empty(&result));
}

TEST(rct, difference_bounds_trims_along_a_fully_spanned_axis)
{
  const rcti a = make_rcti(0, 10, 0, 10);

  /* A band across the bottom: the remainder is the top strip, exactly. */
  const rcti bottom = make_rcti(0, 10, 0, 4);
  rcti result;
  EXPECT_TRUE(BLI_rcti_difference_bounds(&a, &bottom, &result));
  EXPECT_EQ(result.ymin, 4);
  EXPECT_EQ(result.ymax, 10);
  EXPECT_EQ(result.xmin, 0);
  EXPECT_EQ(result.xmax, 10);

  /* A band across the top. */
  const rcti top = make_rcti(0, 10, 6, 10);
  EXPECT_TRUE(BLI_rcti_difference_bounds(&a, &top, &result));
  EXPECT_EQ(result.ymin, 0);
  EXPECT_EQ(result.ymax, 6);

  /* A band down the left. */
  const rcti left = make_rcti(0, 4, 0, 10);
  EXPECT_TRUE(BLI_rcti_difference_bounds(&a, &left, &result));
  EXPECT_EQ(result.xmin, 4);
  EXPECT_EQ(result.xmax, 10);

  /* A band down the right. */
  const rcti right = make_rcti(6, 10, 0, 10);
  EXPECT_TRUE(BLI_rcti_difference_bounds(&a, &right, &result));
  EXPECT_EQ(result.xmin, 0);
  EXPECT_EQ(result.xmax, 6);
}

TEST(rct, difference_bounds_is_a_superset_when_the_remainder_is_not_a_rectangle)
{
  const rcti a = make_rcti(0, 10, 0, 10);

  /* Strictly inside: "everything but the middle" has no rectangular form. */
  const rcti middle = make_rcti(3, 7, 3, 7);
  rcti result;
  EXPECT_FALSE(BLI_rcti_difference_bounds(&a, &middle, &result));
  EXPECT_EQ(result.xmin, 0);
  EXPECT_EQ(result.xmax, 10);
  EXPECT_EQ(result.ymin, 0);
  EXPECT_EQ(result.ymax, 10);

  /* A corner bite: the remainder is L-shaped. */
  const rcti corner = make_rcti(0, 4, 0, 4);
  EXPECT_FALSE(BLI_rcti_difference_bounds(&a, &corner, &result));
  EXPECT_EQ(result.xmin, 0);
  EXPECT_EQ(result.xmax, 10);
  EXPECT_EQ(result.ymin, 0);
  EXPECT_EQ(result.ymax, 10);
}

TEST(rct, difference_bounds_of_an_empty_rectangle_is_empty)
{
  const rcti a = make_rcti(0, 0, 0, 0);
  const rcti b = make_rcti(0, 10, 0, 10);
  rcti result;
  EXPECT_TRUE(BLI_rcti_difference_bounds(&a, &b, &result));
  EXPECT_TRUE(BLI_rcti_is_empty(&result));
}

/** \} */

}  // namespace blender::tests
