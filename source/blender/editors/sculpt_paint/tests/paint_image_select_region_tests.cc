/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <limits>

#include "../selection/paint_image_select_region.hh"

#include "../selection/paint_image_select_source.hh"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::image_select::tests {

/* 32x32 buffer for the connectivity and barrier tests. */
static constexpr int2 TEST_SIZE = int2(32, 32);

/** Adapter for the lazy-distance #minimax_cost_map over a full distance buffer. */
struct DistAt {
  Span<float> dist;
  float operator()(const int64_t i) const
  {
    return dist[i];
  }
};

/** Plain flood fill (stack based) with a hard tolerance, the reference the minimax map must
 * match. */
static Array<float> reference_flood_fill(Span<float> dist,
                                         const int2 size,
                                         const int2 seed,
                                         const float tolerance,
                                         const bool connect_8,
                                         const blender::FunctionRef<bool(int2, int2)> passable)
{
  Array<float> mask(size.x * size.y, 0.0f);
  Vector<int64_t> stack;
  const int64_t seed_idx = int64_t(seed.y) * size.x + seed.x;
  mask[seed_idx] = 1.0f;
  stack.append(seed_idx);
  static const int2 neighbors4[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  static const int2 neighbors8[8] = {
      {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
  const Span<int2> neighbors = connect_8 ? Span<int2>(neighbors8, 8) : Span<int2>(neighbors4, 4);
  while (!stack.is_empty()) {
    const int64_t idx = stack.pop_last();
    const int x = int(idx % size.x), y = int(idx / size.x);
    for (const int2 &offset : neighbors) {
      const int2 nb = int2(x, y) + offset;
      if (nb.x < 0 || nb.y < 0 || nb.x >= size.x || nb.y >= size.y) {
        continue;
      }
      const int64_t nb_idx = int64_t(nb.y) * size.x + nb.x;
      if (mask[nb_idx] > 0.0f || dist[nb_idx] > tolerance) {
        continue;
      }
      if (passable && !passable(int2(x, y), nb)) {
        continue;
      }
      mask[nb_idx] = 1.0f;
      stack.append(nb_idx);
    }
  }
  return mask;
}

TEST(ImageSelectRegion, MinimaxMatchesFloodFill)
{
  RandomNumberGenerator rng(20260926);
  const int64_t n = TEST_SIZE.x * TEST_SIZE.y;
  Array<float> dist(n);
  for (float &value : dist) {
    value = rng.get_float();
  }

  Array<float> dist_quantized(n);
  for (const int64_t i : IndexRange(n)) {
    dist_quantized[i] = float(int(dist[i] * float(COST_LEVELS))) / float(COST_LEVELS);
  }

  for (const int tolerance_step : IndexRange(10)) {
    const float tolerance = (tolerance_step + 1) / 10.0f;
    Array<uint16_t> cost(n);
    minimax_cost_map(DistAt{dist}, TEST_SIZE, int2(0, 0), false, 1.0f, nullptr, cost);
    Array<float> minimax_mask(n);
    soft_threshold(cost, TEST_SIZE, int2(0, 0), tolerance, 0.0f, minimax_mask);
    /* The minimax map quantizes distances to #COST_LEVELS levels, so the reference floods the
     * quantized distances. */
    Array<float> flood = reference_flood_fill(
        dist_quantized, TEST_SIZE, int2(0, 0), tolerance, false, nullptr);
    for (const int64_t i : IndexRange(n)) {
      ASSERT_NEAR(minimax_mask[i], flood[i], 1e-6f);
    }
  }
}

TEST(ImageSelectRegion, BarrierIsNotCrossed)
{
  /* A wall of high distance in the middle; the flood from the left must not reach the right. */
  const int64_t n = TEST_SIZE.x * TEST_SIZE.y;
  Array<float> dist(n, 0.0f);
  for (const int y : IndexRange(TEST_SIZE.y)) {
    dist[int64_t(y) * TEST_SIZE.x + 16] = 1.0f;
  }
  const auto barrier = [&](const int2 from, const int2 to) {
    /* Only pixels left of the wall are passable (a stand-in for the UV-island test). */
    return from.x < 16 && to.x < 16;
  };

  Array<uint16_t> cost(n);
  minimax_cost_map(DistAt{dist}, TEST_SIZE, int2(0, 0), true, 1.0f, barrier, cost);
  for (const int y : IndexRange(TEST_SIZE.y)) {
    EXPECT_EQ(cost[int64_t(y) * TEST_SIZE.x + 17], COST_UNREACHABLE);
  }
  EXPECT_EQ(cost[int64_t(0) * TEST_SIZE.x + 15], 0);
}

TEST(ImageSelectRegion, Connectivity4vs8)
{
  /* A single diagonal chain: 4-connectivity never leaves the seed, 8-connectivity follows it. */
  const int64_t n = TEST_SIZE.x * TEST_SIZE.y;
  Array<float> dist(n, 1.0f);
  for (const int i : IndexRange(TEST_SIZE.x)) {
    dist[int64_t(i) * TEST_SIZE.x + i] = 0.0f;
  }

  Array<uint16_t> cost4(n);
  minimax_cost_map(DistAt{dist}, TEST_SIZE, int2(0, 0), false, 0.5f, nullptr, cost4);
  Array<uint16_t> cost8(n);
  minimax_cost_map(DistAt{dist}, TEST_SIZE, int2(0, 0), true, 0.5f, nullptr, cost8);

  EXPECT_EQ(cost4[int64_t(TEST_SIZE.x - 1) * TEST_SIZE.x + TEST_SIZE.x - 1], COST_UNREACHABLE);
  EXPECT_NE(cost8[int64_t(TEST_SIZE.x - 1) * TEST_SIZE.x + TEST_SIZE.x - 1], COST_UNREACHABLE);
}

TEST(ImageSelectRegion, FillHoles)
{
  /* A selected ring around a small unselected hole: the hole fills. A big hole stays. */
  const int2 size(33, 33);
  const int64_t n = size.x * size.y;
  Array<float> mask(n, 1.0f);
  /* Small hole: 3x3 near the corner (away from the border, away from the big hole). */
  for (const int dy : IndexRange(3)) {
    for (const int dx : IndexRange(3)) {
      mask[int64_t(26 + dy) * size.x + 26 + dx] = 0.0f;
    }
  }
  /* Big hole: 15x15, away from the border but too large to fill. */
  for (const int dy : IndexRange(15)) {
    for (const int dx : IndexRange(15)) {
      mask[int64_t(5 + dy) * size.x + 5 + dx] = 0.0f;
    }
  }
  mask_fill_holes(mask, size, 10);

  EXPECT_FLOAT_EQ(mask[int64_t(27) * size.x + 27], 1.0f); /* Small hole filled. */
  EXPECT_FLOAT_EQ(mask[int64_t(12) * size.x + 12], 0.0f); /* Big hole survives. */
  EXPECT_FLOAT_EQ(mask[int64_t(16) * size.x + 0], 1.0f);  /* Selected area untouched. */
}

TEST(ImageSelectRegion, DistanceTransformAgainstBruteForce)
{
  const int2 size(17, 13);
  const int64_t n = size.x * size.y;
  RandomNumberGenerator rng(42);
  Array<uint8_t> binary(n);
  for (uint8_t &value : binary) {
    value = rng.get_float() < 0.15f;
  }
  binary[int64_t(6) * size.x + 8] = 1; /* Guarantee at least one true pixel. */

  Array<float> dist(n);
  distance_transform_true(binary, size, dist);
  for (const int y : IndexRange(size.y)) {
    for (const int x : IndexRange(size.x)) {
      float best = std::numeric_limits<float>::max();
      for (const int ty : IndexRange(size.y)) {
        for (const int tx : IndexRange(size.x)) {
          if (binary[int64_t(ty) * size.x + tx]) {
            const float dx = float(x - tx), dy = float(y - ty);
            best = std::min(best, std::sqrt(dx * dx + dy * dy));
          }
        }
      }
      ASSERT_NEAR(dist[int64_t(y) * size.x + x], best, 1e-3f);
    }
  }
}

TEST(ImageSelectRegion, ResampleKeepsArea)
{
  /* A half-filled mask resampled 2x keeps its area within 1% (a circle would be exact; a
   * straight half is exact too, the bilinear error concentrates on the boundary). */
  const int2 src_size(64, 64);
  Array<float> src(src_size.x * src_size.y, 0.0f);
  for (const int y : IndexRange(src_size.y)) {
    for (const int x : IndexRange(src_size.x)) {
      if (x < src_size.x / 2) {
        src[int64_t(y) * src_size.x + x] = 1.0f;
      }
    }
  }
  const int2 dst_size(128, 128);
  Array<float> dst(dst_size.x * dst_size.y);
  mask_resample(src, src_size, dst, dst_size);

  double sum = 0.0;
  for (const float value : dst) {
    sum += value;
  }
  /* Half of the destination must be selected; bilinear interpolation of an exact step has no
   * area error, only boundary softness. */
  EXPECT_NEAR(sum / (dst_size.x * dst_size.y), 0.5, 0.01);
}

TEST(ImageSelectRegion, NormalMetricAngle)
{
  FeatureImage feature;
  feature.size = int2(1, 1);
  feature.dims = 3;
  feature.is_normal = true;
  feature.metric = SourceMetric::NormalAngle;
  feature.data.reinitialize(3);
  feature.data[0] = 1.0f;
  feature.data[1] = 0.0f;
  feature.data[2] = 0.0f;

  const float same[3] = {1.0f, 0.0f, 0.0f};
  EXPECT_NEAR(feature_distance(feature, feature.data.data(), same), 0.0f, 1e-6f);

  /* 90 degrees apart: distance = 90/180 = 0.5. */
  const float rotated[3] = {0.0f, 0.0f, 1.0f};
  EXPECT_NEAR(feature_distance(feature, feature.data.data(), rotated), 0.5f, 1e-5f);

  /* Opposite: 1.0. */
  const float flipped[3] = {-1.0f, 0.0f, 0.0f};
  EXPECT_NEAR(feature_distance(feature, feature.data.data(), flipped), 1.0f, 1e-5f);
}

TEST(ImageSelectRegion, SobelStaysInsideTheBuffer)
{
  /* A vertical step edge in the middle of a non-square buffer: the edge lights up, flat areas and
   * the last row/column (which used to be read past the end) stay finite and in 0..1. */
  const int2 size(9, 6);
  Array<float> data(size.x * size.y, 0.0f);
  for (const int y : IndexRange(size.y)) {
    for (const int x : IndexRange(size.x / 2, size.x - size.x / 2)) {
      data[int64_t(y) * size.x + x] = 1.0f;
    }
  }
  Array<float> edge(size.x * size.y);
  sobel_edge_map(data, 1, size, edge);
  for (const float value : edge) {
    ASSERT_TRUE(std::isfinite(value));
    ASSERT_GE(value, 0.0f);
    ASSERT_LE(value, 1.0f);
  }
  EXPECT_FLOAT_EQ(edge[int64_t(3) * size.x + 0], 0.0f);
  EXPECT_GT(edge[int64_t(3) * size.x + size.x / 2], 0.5f);
}

TEST(ImageSelectRegion, GrowAndShrink)
{
  const int2 size(21, 21);
  Array<float> mask(size.x * size.y, 0.0f);
  mask[int64_t(10) * size.x + 10] = 1.0f;
  mask_grow(mask, size, 3);
  EXPECT_FLOAT_EQ(mask[int64_t(10) * size.x + 13], 1.0f);
  EXPECT_FLOAT_EQ(mask[int64_t(10) * size.x + 14], 0.0f);
  /* Euclidean, not a square: the corner of the 3x3 box is farther than 3. */
  EXPECT_FLOAT_EQ(mask[int64_t(13) * size.x + 13], 0.0f);

  mask_shrink(mask, size, 2);
  EXPECT_FLOAT_EQ(mask[int64_t(10) * size.x + 10], 1.0f);
  EXPECT_FLOAT_EQ(mask[int64_t(10) * size.x + 12], 0.0f);
}

TEST(ImageSelectRegion, GaussianWithBarrierDoesNotLeak)
{
  const int2 size(9, 9);
  const int64_t n = size.x * size.y;
  /* A step: left column of the center row is 1, right column 0; a vertical barrier between
   * columns 4 and 5 keeps the blur from mixing them. */
  Array<float> src(n, 0.0f);
  for (const int y : IndexRange(size.y)) {
    for (const int x : IndexRange(5)) {
      src[int64_t(y) * size.x + x] = 1.0f;
    }
  }
  UVIslandMap barrier;
  barrier.size = size;
  barrier.island.reinitialize(n);
  barrier.island.fill(0);
  barrier.overlap.reinitialize(n);
  barrier.overlap.fill(0);
  for (const int y : IndexRange(size.y)) {
    for (const int x : IndexRange(size.x)) {
      barrier.island[int64_t(y) * size.x + x] = (x < 5) ? 0 : 1;
    }
  }
  barrier.island_num = 2;

  Array<float> dst(n);
  gaussian_blur_barrier(src, size, 2.0f, &barrier, dst);
  /* The pixel right of the wall must stay clean; the pixel left of it must stay saturated. */
  EXPECT_GT(dst[int64_t(4) * size.x + 4], 0.999f);
  EXPECT_LT(dst[int64_t(4) * size.x + 5], 0.001f);
}

}  // namespace blender::ed::sculpt_paint::image_select::tests
