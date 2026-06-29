/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_multires_grid_resample.hh"
#include "BKE_ccg.hh"

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include <algorithm>

namespace blender::bke {

int grid_totelem(const int grids_num, const int level)
{
  const int64_t gs = CCG_grid_size(level);
  return int(int64_t(grids_num) * gs * gs);
}

Array<float3> grid_subsample(const Span<float3> src,
                             const int src_level,
                             const int dst_level,
                             const int grids_num)
{
  if (dst_level >= src_level) {
    return Array<float3>(src);
  }
  const int src_gs = CCG_grid_size(src_level);
  const int dst_gs = CCG_grid_size(dst_level);
  const int step = (src_gs - 1) / (dst_gs - 1); /* Power of two (nested grids). */
  const int64_t src_area = int64_t(src_gs) * src_gs;
  const int64_t dst_area = int64_t(dst_gs) * dst_gs;

  Array<float3> dst(int64_t(grids_num) * dst_area);
  threading::parallel_for(IndexRange(grids_num), 16, [&](const IndexRange range) {
    for (const int g : range) {
      const int64_t so = int64_t(g) * src_area;
      const int64_t doff = int64_t(g) * dst_area;
      for (int y = 0; y < dst_gs; y++) {
        for (int x = 0; x < dst_gs; x++) {
          dst[doff + int64_t(y) * dst_gs + x] = src[so + int64_t(y * step) * src_gs + x * step];
        }
      }
    }
  });
  return dst;
}

Array<float3> grid_upsample(const Span<float3> src,
                            const int src_level,
                            const int dst_level,
                            const int grids_num)
{
  if (dst_level <= src_level) {
    return Array<float3>(src);
  }
  const int src_gs = CCG_grid_size(src_level);
  const int dst_gs = CCG_grid_size(dst_level);
  const int ratio = (dst_gs - 1) / (src_gs - 1); /* Power of two. */
  const int64_t src_area = int64_t(src_gs) * src_gs;
  const int64_t dst_area = int64_t(dst_gs) * dst_gs;

  Array<float3> dst(int64_t(grids_num) * dst_area);
  threading::parallel_for(IndexRange(grids_num), 16, [&](const IndexRange range) {
    for (const int g : range) {
      const int64_t so = int64_t(g) * src_area;
      const int64_t doff = int64_t(g) * dst_area;
      for (int Y = 0; Y < dst_gs; Y++) {
        const int y0 = Y / ratio;
        const int y1 = std::min(y0 + 1, src_gs - 1);
        const float ty = float(Y - y0 * ratio) / float(ratio);
        for (int X = 0; X < dst_gs; X++) {
          const int x0 = X / ratio;
          const int x1 = std::min(x0 + 1, src_gs - 1);
          const float tx = float(X - x0 * ratio) / float(ratio);
          const float3 v00 = src[so + int64_t(y0) * src_gs + x0];
          const float3 v10 = src[so + int64_t(y0) * src_gs + x1];
          const float3 v01 = src[so + int64_t(y1) * src_gs + x0];
          const float3 v11 = src[so + int64_t(y1) * src_gs + x1];
          const float3 a = math::interpolate(v00, v10, tx);
          const float3 b = math::interpolate(v01, v11, tx);
          dst[doff + int64_t(Y) * dst_gs + X] = math::interpolate(a, b, ty);
        }
      }
    }
  });
  return dst;
}

}  // namespace blender::bke
