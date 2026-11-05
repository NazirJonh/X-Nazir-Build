/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_paint_types.hh"

#include "testing/testing.h"

namespace blender {

/* The PBR stroke randomness draws one factor per dab (#bke::PaintRuntime::strength_random_value)
 * with #bke::random_reduction_factor. The formula is shared by the stroke code and both engines,
 * so the tests pin the contract every consumer relies on: randomness only ever lowers the strength
 * (the factor stays in [1 - amount, 1]) and an amount of 0 leaves it untouched. */

TEST(paint_strength_random, zero_amount_keeps_strength)
{
  /* The feature is off: every draw must return exactly 1.0, no matter the random value. */
  for (const float random : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 123.0f, -4.0f}) {
    EXPECT_EQ(bke::random_reduction_factor(0.0f, random), 1.0f);
  }
}

TEST(paint_strength_random, bounds_of_full_amount)
{
  /* amount = 1 spans the whole reduction range: the first draw (random = 0) keeps the strength,
   * the last one (random = 1) silences the dab entirely. */
  EXPECT_EQ(bke::random_reduction_factor(1.0f, 0.0f), 1.0f);
  EXPECT_EQ(bke::random_reduction_factor(1.0f, 1.0f), 0.0f);
}

TEST(paint_strength_random, interpolated_values)
{
  EXPECT_NEAR(bke::random_reduction_factor(0.8f, 0.5f), 0.6f, 1e-6);
  EXPECT_NEAR(bke::random_reduction_factor(0.25f, 0.5f), 0.875f, 1e-6);
  EXPECT_NEAR(bke::random_reduction_factor(0.5f, 0.1f), 0.95f, 1e-6);
}

TEST(paint_strength_random, never_exceeds_the_strength)
{
  /* For every amount the factor is capped at 1: a dab can only get weaker, never stronger. */
  for (const float amount : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
    for (const float random : {0.0f, 0.125f, 0.5f, 0.875f, 1.0f}) {
      EXPECT_LE(bke::random_reduction_factor(amount, random), 1.0f);
    }
  }
}

TEST(paint_strength_random, stays_within_amount_range)
{
  /* The reduction is bounded by the amount: the factor never drops below 1 - amount, so a small
   * amount cannot mute the brush. */
  for (const float amount : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
    for (const float random : {0.0f, 0.125f, 0.5f, 0.875f, 1.0f}) {
      EXPECT_GE(bke::random_reduction_factor(amount, random), 1.0f - amount);
    }
  }
}

TEST(paint_strength_random, monotonic_in_random)
{
  /* A larger draw only lowers the factor, so the sequence has no upward steps. */
  for (const float amount : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
    float previous = 2.0f;
    for (int i = 0; i <= 10; i++) {
      const float factor = bke::random_reduction_factor(amount, i / 10.0f);
      EXPECT_LE(factor, previous);
      previous = factor;
    }
  }
}

}  // namespace blender
