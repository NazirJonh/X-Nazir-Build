/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the wand / quick-select region algorithms; see
 * #paint_image_select_region.hh.
 */

#include "paint_image_select_region.hh"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_interp.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

namespace blender::ed::sculpt_paint::image_select {

/* -------------------------------------------------------------------- */
/** \name UV-island map
 * \{ */

bool UVIslandMap::passable(const int2 &from, const int2 &to) const
{
  if (!is_valid()) {
    return true;
  }
  if (overlap_at(from) || overlap_at(to)) {
    /* A pixel claimed by several islands is passable from any of them. */
    return true;
  }
  const int32_t from_island = island_at(from);
  const int32_t to_island = island_at(to);
  if (from_island < 0 && to_island < 0) {
    /* The empty area floods freely; only island borders are walls. */
    return true;
  }
  return from_island == to_island;
}

void uv_island_map_dilate(UVIslandMap &map, const int margin_px)
{
  if (!map.is_valid() || margin_px <= 0) {
    return;
  }
  /* BFS from every island pixel outward, filling only -1 pixels, layer by layer. */
  Vector<int2> frontier;
  for (const int y : IndexRange(map.size.y)) {
    for (const int x : IndexRange(map.size.x)) {
      if (map.island[map.index(x, y)] >= 0) {
        frontier.append(int2(x, y));
      }
    }
  }
  Vector<int2> next;
  for (int layer = 0; layer < margin_px && !frontier.is_empty(); layer++) {
    next.clear();
    for (const int2 &px : frontier) {
      const int2 neighbors[4] = {
          px + int2(1, 0), px - int2(1, 0), px + int2(0, 1), px - int2(0, 1)};
      for (const int2 &n : neighbors) {
        if (n.x < 0 || n.y < 0 || n.x >= map.size.x || n.y >= map.size.y) {
          continue;
        }
        int32_t &dst = map.island[map.index(n.x, n.y)];
        if (dst < 0) {
          dst = map.island[map.index(px.x, px.y)];
          next.append(n);
        }
      }
    }
    std::swap(frontier, next);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Minimax cost map (Magic Wand)
 * \{ */

namespace {

/** 4- or 8-connected neighbor offsets. */
constexpr int2 NEIGHBORS_4[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int2 NEIGHBORS_8[8] = {
    {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

}  // namespace

void minimax_cost_map(const blender::FunctionRef<float(int64_t)> dist_at,
                      const int2 size,
                      const int2 seed,
                      const bool connect_8,
                      const float max_tolerance,
                      const blender::FunctionRef<bool(int2, int2)> passable,
                      const MutableSpan<uint16_t> r_cost)
{
  const int64_t pixel_num = int64_t(size.x) * size.y;
  BLI_assert(r_cost.size() == pixel_num);
  r_cost.fill(COST_UNREACHABLE);
  if (pixel_num == 0 || seed.x < 0 || seed.y < 0 || seed.x >= size.x || seed.y >= size.y) {
    return;
  }

  /* A pixel whose distance exceeds the max tolerance is a wall: the flood never enters it.
   * This bounds the visited area to roughly the tolerance's neighborhood. \a dist_at is only
   * evaluated for visited pixels (at most once per neighbor check). */
  const int max_level = int(std::clamp(max_tolerance, 0.0f, 1.0f) * float(COST_LEVELS));
  auto level_of = [&](const int64_t idx) {
    return std::min(int(dist_at(idx) * float(COST_LEVELS)), COST_LEVELS);
  };

  const int64_t seed_idx = int64_t(seed.y) * size.x + seed.x;
  const int seed_level = level_of(seed_idx);
  if (seed_level > max_level) {
    return;
  }

  /* Bucket queue: every transition's cost is max(parent, pixel) >= the parent's key, so the
   * pop pointer only moves forward and each pixel settles once. */
  Array<Vector<int32_t>> buckets(size_t(COST_LEVELS) + 1);
  r_cost[seed_idx] = uint16_t(seed_level);
  buckets[size_t(seed_level)].append(int32_t(seed_idx));

  const Span<int2> neighbors = connect_8 ? Span<int2>(NEIGHBORS_8, 8) : Span<int2>(NEIGHBORS_4, 4);

  for (int level = seed_level; level <= max_level; level++) {
    Vector<int32_t> &bucket = buckets[size_t(level)];
    while (!bucket.is_empty()) {
      const int32_t idx = bucket.pop_last();
      if (r_cost[idx] < uint16_t(level)) {
        continue; /* A cheaper path already settled this pixel. */
      }
      const int2 px(idx % size.x, idx / size.x);
      for (const int2 &offset : neighbors) {
        const int2 nb = px + offset;
        if (nb.x < 0 || nb.y < 0 || nb.x >= size.x || nb.y >= size.y) {
          continue;
        }
        const int64_t nb_idx = int64_t(nb.y) * size.x + nb.x;
        if (r_cost[nb_idx] != COST_UNREACHABLE && r_cost[nb_idx] <= uint16_t(level)) {
          continue;
        }
        const int nb_level = level_of(nb_idx);
        if (nb_level > max_level) {
          continue; /* Wall. */
        }
        if (passable && !passable(px, nb)) {
          continue;
        }
        const int next = std::max(level, nb_level);
        if (next < r_cost[nb_idx]) {
          r_cost[nb_idx] = uint16_t(next);
          buckets[size_t(next)].append(int32_t(nb_idx));
        }
      }
    }
  }
}

void soft_threshold(const Span<uint16_t> cost,
                    const int2 size,
                    const int2 seed,
                    const float tolerance,
                    const float softness,
                    const MutableSpan<float> r_mask)
{
  const int64_t pixel_num = int64_t(size.x) * size.y;
  BLI_assert(r_mask.size() == pixel_num);
  const float t = tolerance * float(COST_LEVELS);
  const float s = std::max(softness, 0.0f) * float(COST_LEVELS);
  threading::parallel_for(IndexRange(pixel_num), 4096, [&](const IndexRange range) {
    for (const int64_t i : range) {
      const uint16_t c = cost[i];
      float m;
      if (c == COST_UNREACHABLE) {
        m = 0.0f;
      }
      else if (s <= 0.0f) {
        m = (float(c) <= t) ? 1.0f : 0.0f;
      }
      else {
        m = math::clamp((t + s - float(c)) / (2.0f * s), 0.0f, 1.0f);
      }
      r_mask[i] = m;
    }
  });
  /* The clicked pixel belongs to the selection no matter its own distance. */
  if (seed.x >= 0 && seed.y >= 0 && seed.x < size.x && seed.y < size.y) {
    r_mask[int64_t(seed.y) * size.x + seed.x] = 1.0f;
  }
}

void threshold_global(const Span<float> dist,
                      const float tolerance,
                      const MutableSpan<float> r_mask)
{
  BLI_assert(r_mask.size() == dist.size());
  threading::parallel_for(IndexRange(dist.size()), 4096, [&](const IndexRange range) {
    for (const int64_t i : range) {
      r_mask[i] = (dist[i] <= tolerance) ? 1.0f : 0.0f;
    }
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geodesic growth (Quick Selection)
 * \{ */

void geodesic_cost_map(const blender::FunctionRef<float(int2)> step_cost,
                       const int2 size,
                       const int2 seed,
                       const bool connect_8,
                       const float budget,
                       const blender::FunctionRef<bool(int2, int2)> passable,
                       const MutableSpan<float> r_cost)
{
  const int64_t pixel_num = int64_t(size.x) * size.y;
  BLI_assert(r_cost.size() == pixel_num);
  constexpr float UNREACHABLE = std::numeric_limits<float>::max();
  r_cost.fill(UNREACHABLE);
  if (pixel_num == 0 || seed.x < 0 || seed.y < 0 || seed.x >= size.x || seed.y >= size.y) {
    return;
  }

  using Entry = std::pair<float, int64_t>; /* (cost, index), min-heap by cost. */
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;

  const int64_t seed_idx = int64_t(seed.y) * size.x + seed.x;
  r_cost[seed_idx] = 0.0f;
  queue.emplace(0.0f, seed_idx);

  const Span<int2> neighbors = connect_8 ? Span<int2>(NEIGHBORS_8, 8) : Span<int2>(NEIGHBORS_4, 4);

  while (!queue.empty()) {
    const auto [current, idx] = queue.top();
    queue.pop();
    if (current > r_cost[idx]) {
      continue; /* Stale heap entry. */
    }
    const int2 px(int(idx % size.x), int(idx / size.x));
    for (const int2 &offset : neighbors) {
      const int2 nb = px + offset;
      if (nb.x < 0 || nb.y < 0 || nb.x >= size.x || nb.y >= size.y) {
        continue;
      }
      const int64_t nb_idx = int64_t(nb.y) * size.x + nb.x;
      if (passable && !passable(px, nb)) {
        continue;
      }
      /* Diagonal steps are sqrt(2) long, so the grown region is round rather than square. */
      const float length = (offset.x != 0 && offset.y != 0) ? 1.41421356f : 1.0f;
      const float next = current + step_cost(nb) * length;
      if (next > budget || next >= r_cost[nb_idx]) {
        continue;
      }
      r_cost[nb_idx] = next;
      queue.emplace(next, nb_idx);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge map and distance transform
 * \{ */

void sobel_edge_map(const Span<float> data,
                    const int dims,
                    const int2 size,
                    const MutableSpan<float> r_edge)
{
  const int w = size.x, h = size.y;
  BLI_assert(r_edge.size() == int64_t(w) * h);
  if (w < 3 || h < 3) {
    r_edge.fill(0.0f);
    return;
  }
  /* Per-pixel maximum |sobel| over the components, then normalize by the 95th percentile. */
  threading::parallel_for(IndexRange(1, h - 2), 32, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (const int x : IndexRange(1, w - 2)) {
        float max_mag = 0.0f;
        for (const int c : IndexRange(dims)) {
          auto at = [&](const int xx, const int yy) {
            return data[(int64_t(yy) * w + xx) * dims + c];
          };
          const float gx = -at(x - 1, y - 1) - 2.0f * at(x - 1, y) - at(x - 1, y + 1) +
                           at(x + 1, y - 1) + 2.0f * at(x + 1, y) + at(x + 1, y + 1);
          const float gy = -at(x - 1, y - 1) - 2.0f * at(x, y - 1) - at(x + 1, y - 1) +
                           at(x - 1, y + 1) + 2.0f * at(x, y + 1) + at(x + 1, y + 1);
          max_mag = std::max(max_mag, std::sqrt(gx * gx + gy * gy));
        }
        r_edge[int64_t(y) * w + x] = max_mag;
      }
    }
  });
  /* Borders: copy the nearest interior value so the map has no artificial edges. */
  for (const int x : IndexRange(w)) {
    r_edge[x] = r_edge[int64_t(1) * w + x];
    r_edge[int64_t(h - 1) * w + x] = r_edge[int64_t(h - 2) * w + x];
  }
  for (const int y : IndexRange(h)) {
    r_edge[int64_t(y) * w] = r_edge[int64_t(y) * w + 1];
    r_edge[int64_t(y) * w + w - 1] = r_edge[int64_t(y) * w + w - 2];
  }

  /* Normalize by the 95th percentile (sampled). */
  Vector<float> sample;
  const int stride = std::max(1, (w * h) / 4096);
  for (int64_t i = 0; i < int64_t(w) * h; i += stride) {
    sample.append(r_edge[i]);
  }
  if (!sample.is_empty()) {
    const int64_t nth = std::min(sample.size() - 1, int64_t(float(sample.size()) * 0.95f));
    std::nth_element(sample.begin(), sample.begin() + nth, sample.end());
    const float norm = std::max(sample[nth], 1e-6f);
    for (float &value : r_edge) {
      value = std::min(value / norm, 1.0f);
    }
  }
}

namespace {

/* Felzenszwalb & Huttenlocher 1D squared-distance lower envelope. \a v and \a z are the
 * caller-provided work buffers (\a v: n entries, \a z: n + 1 entries). */
void edt_1d(const Span<float> f,
            const MutableSpan<float> d,
            const int n,
            const MutableSpan<int> v,
            const MutableSpan<float> z)
{
  constexpr float INF = 1e20f;
  int k = 0;
  v[0] = 0;
  z[0] = -INF;
  z[1] = INF;
  for (int q = 1; q < n; q++) {
    float s = ((f[q] + float(q * q)) - (f[v[k]] + float(v[k] * v[k]))) /
              (2.0f * float(q) - 2.0f * float(v[k]));
    while (s <= z[k]) {
      k--;
      s = ((f[q] + float(q * q)) - (f[v[k]] + float(v[k] * v[k]))) /
          (2.0f * float(q) - 2.0f * float(v[k]));
    }
    k++;
    v[k] = q;
    z[k] = s;
    z[k + 1] = INF;
  }
  k = 0;
  for (int q = 0; q < n; q++) {
    while (z[k + 1] < float(q)) {
      k++;
    }
    d[q] = (float(q) - float(v[k])) * (float(q) - float(v[k])) + f[v[k]];
  }
}

}  // namespace

void distance_transform_true(const Span<uint8_t> binary,
                             const int2 size,
                             const MutableSpan<float> r_distance)
{
  const int w = size.x, h = size.y;
  BLI_assert(r_distance.size() == int64_t(w) * h);
  if (w <= 0 || h <= 0) {
    return;
  }
  /* Squared-distance grid: 0 on true pixels, a value beyond any real squared distance elsewhere
   * (kept small so float sums stay exact). The envelope must not read and write one buffer. */
  const float far_sq = float(w) * float(w) + float(h) * float(h) + 1.0f;
  Array<float> grid(int64_t(w) * h);
  for (const int64_t i : grid.index_range()) {
    grid[i] = binary[i] ? 0.0f : far_sq;
  }
  const int max_dim = std::max(w, h);
  /* Columns, then rows; each task keeps its own 1D work buffers for its whole range. */
  threading::parallel_for(IndexRange(w), 16, [&](const IndexRange cols) {
    Array<int> v(max_dim);
    Array<float> z(max_dim + 1);
    Array<float> scan(max_dim);
    Array<float> scan_out(max_dim);
    for (const int x : cols) {
      for (const int y : IndexRange(h)) {
        scan[y] = grid[int64_t(y) * w + x];
      }
      edt_1d(scan, scan_out, h, v, z);
      for (const int y : IndexRange(h)) {
        grid[int64_t(y) * w + x] = scan_out[y];
      }
    }
  });
  threading::parallel_for(IndexRange(h), 16, [&](const IndexRange rows) {
    Array<int> v(max_dim);
    Array<float> z(max_dim + 1);
    Array<float> scan_out(max_dim);
    for (const int y : rows) {
      const Span<float> row = grid.as_span().slice(int64_t(y) * w, w);
      edt_1d(row, scan_out, w, v, z);
      MutableSpan<float> row_out = grid.as_mutable_span().slice(int64_t(y) * w, w);
      for (const int x : IndexRange(w)) {
        row_out[x] = scan_out[x];
      }
    }
  });
  for (const int64_t i : r_distance.index_range()) {
    r_distance[i] = std::sqrt(grid[i]);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Post-processing
 * \{ */

void gaussian_blur_barrier(const Span<float> src,
                           const int2 size,
                           const float sigma,
                           const UVIslandMap *barrier,
                           const MutableSpan<float> r_dst)
{
  const int w = size.x, h = size.y;
  BLI_assert(r_dst.size() == int64_t(w) * h);
  if (w <= 0 || h <= 0 || sigma <= 0.0f) {
    r_dst.copy_from(src);
    return;
  }
  const int radius = std::min({int(std::ceil(3.0f * sigma)), (w - 1) / 2, (h - 1) / 2});
  Array<float> kernel(radius * 2 + 1);
  const float sigma2 = 2.0f * sigma * sigma;
  float weight_sum = 0.0f;
  for (const int i : kernel.index_range()) {
    const float d = float(i - radius);
    kernel[i] = std::exp(-d * d / sigma2);
    weight_sum += kernel[i];
  }
  for (float &weight : kernel) {
    weight /= weight_sum;
  }

  /* Two normalized passes; barrier weights drop to 0 and the kernel renormalizes. */
  Array<float> temp(int64_t(w) * h);
  threading::parallel_for(IndexRange(h), 8, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (const int x : IndexRange(w)) {
        const int64_t idx = int64_t(y) * w + x;
        float sum = 0.0f, norm = 0.0f;
        for (int k = -radius; k <= radius; k++) {
          const int xx = x + k;
          if (xx < 0 || xx >= w) {
            continue;
          }
          const int64_t nidx = int64_t(y) * w + xx;
          if (barrier && !barrier->passable(int2(x, y), int2(xx, y))) {
            continue;
          }
          sum += src[nidx] * kernel[k + radius];
          norm += kernel[k + radius];
        }
        temp[idx] = (norm > 0.0f) ? sum / norm : src[idx];
      }
    }
  });
  threading::parallel_for(IndexRange(h), 8, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (const int x : IndexRange(w)) {
        const int64_t idx = int64_t(y) * w + x;
        float sum = 0.0f, norm = 0.0f;
        for (int k = -radius; k <= radius; k++) {
          const int yy = y + k;
          if (yy < 0 || yy >= h) {
            continue;
          }
          const int64_t nidx = int64_t(yy) * w + x;
          if (barrier && !barrier->passable(int2(x, y), int2(x, yy))) {
            continue;
          }
          sum += temp[nidx] * kernel[k + radius];
          norm += kernel[k + radius];
        }
        r_dst[idx] = (norm > 0.0f) ? sum / norm : temp[idx];
      }
    }
  });
}

void mask_fill_holes(MutableSpan<float> mask, const int2 size, const int min_area)
{
  const int w = size.x, h = size.y;
  if (w <= 0 || h <= 0 || min_area <= 0) {
    return;
  }
  const int64_t n = int64_t(w) * h;
  Array<uint8_t> visited(n, 0);
  Vector<int64_t> component;
  Vector<int64_t> stack;
  for (const int64_t start : IndexRange(n)) {
    if (visited[start] || mask[start] >= 0.5f) {
      continue;
    }
    /* Flood one unselected component; remember whether it touches the border. The indices are
     * only collected while the component could still be filled, so a huge background component
     * costs no extra memory (the flood keeps running to mark every pixel visited). */
    component.clear();
    stack.clear();
    stack.append(start);
    visited[start] = 1;
    int64_t count = 0;
    bool touches_border = false;
    while (!stack.is_empty()) {
      const int64_t idx = stack.pop_last();
      count++;
      if (!touches_border && count < int64_t(min_area)) {
        component.append(idx);
      }
      const int x = int(idx % w), y = int(idx / w);
      if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
        touches_border = true;
      }
      const int2 neighbors[4] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
      for (const int2 &nb : neighbors) {
        if (nb.x < 0 || nb.y < 0 || nb.x >= w || nb.y >= h) {
          continue;
        }
        const int64_t nb_idx = int64_t(nb.y) * w + nb.x;
        if (!visited[nb_idx] && mask[nb_idx] < 0.5f) {
          visited[nb_idx] = 1;
          stack.append(nb_idx);
        }
      }
    }
    if (!touches_border && count < int64_t(min_area)) {
      for (const int64_t idx : component) {
        mask[idx] = 1.0f;
      }
    }
  }
}

void mask_grow(MutableSpan<float> mask, const int2 size, const int grow_px)
{
  if (grow_px <= 0) {
    return;
  }
  const int64_t n = int64_t(size.x) * size.y;
  Array<uint8_t> binary(n);
  for (const int64_t i : IndexRange(n)) {
    binary[i] = mask[i] >= 0.5f;
  }
  Array<float> dist(n);
  distance_transform_true(binary, size, dist);
  for (const int64_t i : IndexRange(n)) {
    if (dist[i] <= float(grow_px)) {
      mask[i] = 1.0f;
    }
  }
}

void mask_shrink(MutableSpan<float> mask, const int2 size, const int shrink_px)
{
  if (shrink_px <= 0) {
    return;
  }
  const int64_t n = int64_t(size.x) * size.y;
  Array<uint8_t> binary(n);
  for (const int64_t i : IndexRange(n)) {
    binary[i] = mask[i] < 0.5f;
  }
  Array<float> dist(n);
  distance_transform_true(binary, size, dist);
  for (const int64_t i : IndexRange(n)) {
    if (dist[i] <= float(shrink_px)) {
      mask[i] = 0.0f;
    }
  }
}

void mask_resample(const Span<float> src,
                   const int2 src_size,
                   const MutableSpan<float> r_dst,
                   const int2 dst_size)
{
  BLI_assert(r_dst.size() == int64_t(dst_size.x) * dst_size.y);
  if (src_size.x <= 0 || src_size.y <= 0) {
    r_dst.fill(0.0f);
    return;
  }
  threading::parallel_for(IndexRange(dst_size.y), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (const int x : IndexRange(dst_size.x)) {
        /* Pixel centers through normalized UV coordinates, clamped at the tile border. */
        const float gx = (float(x) + 0.5f) * float(src_size.x) / float(dst_size.x) - 0.5f;
        const float gy = (float(y) + 0.5f) * float(src_size.y) / float(dst_size.y) - 0.5f;
        math::interpolate_bilinear_wrapmode_fl(src.data(),
                                               &r_dst[int64_t(y) * dst_size.x + x],
                                               src_size.x,
                                               src_size.y,
                                               1,
                                               gx,
                                               gy,
                                               math::InterpWrapMode::Extend,
                                               math::InterpWrapMode::Extend);
      }
    }
  });
}

void mask_combine(MutableSpan<float> dst, const Span<float> src, const eSelectOp sel_op)
{
  BLI_assert(dst.size() == src.size());
  for (const int64_t i : dst.index_range()) {
    switch (sel_op) {
      case SEL_OP_SET:
        dst[i] = src[i];
        break;
      case SEL_OP_ADD:
        dst[i] = std::max(dst[i], src[i]);
        break;
      case SEL_OP_SUB:
        dst[i] = std::min(dst[i], 1.0f - src[i]);
        break;
      case SEL_OP_AND:
        dst[i] = std::min(dst[i], src[i]);
        break;
      case SEL_OP_XOR:
        dst[i] = std::abs(dst[i] - src[i]);
        break;
    }
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::image_select
