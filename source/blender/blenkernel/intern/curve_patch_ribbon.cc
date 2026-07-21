/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Ribbon -> LUT parameterization for Curve Patch. Geometry stages adapted from the Roll stroke
 * method's per-dab implementation (`paint_stroke_roll.cc`) -- see the header for what differs and
 * why the module keeps its own copy instead of sharing Roll's dab-scoped code paths.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>

#include "BKE_curve_patch.hh"

#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

namespace blender::bke {

void CurvePatchRibbonLut::clear()
{
  res = 0;
  uv.clear();
  dist_sq.clear();
  row.clear();
  uv2.clear();
  dist_sq2.clear();
  row2.clear();
  bb_min = float2(0.0f);
  inv_extent = float2(0.0f);
  axis_x = float3(0.0f);
  axis_y = float3(0.0f);
  v_threshold = 0.0f;
  ready = false;
  source_hash = 0;
}

/* -------------------------------------------------------------------- */
/** \name Border Self-Intersection Collapse
 * \{ */

/**
 * Fix inner-border self-intersections at sharp turns.
 *
 * The border curves (at the strip's half-width from the center) may self-intersect on the inner
 * side of tight turns where the radius of curvature is smaller than the half-width. Finds where
 * the border polyline actually crosses itself (2D segment vs segment test in the ribbon plane),
 * then collapses all loop vertices to that crossing point. Overlapping quads become degenerate
 * triangles fanning from the crossing point -- the cross-lines rotate to point toward it. Long
 * gentle turns collapse only the sharpest `3 * R` arc window centered on the apex vertex.
 *
 * `R` is the largest half-width along the curve: used for the loop-vertex validation radius (with
 * a 20% margin) and the partial-collapse arc budget.
 *
 * `cyclic` marks a closed curve, whose border's first and last vertices are the same point. That
 * makes the loop's own join look exactly like a crossing (the two segments meeting there share an
 * endpoint, so the segment test reports a hit), and the validation below cannot reject it: it asks
 * whether every vertex of the supposed loop lies within `R` of the center curve, which on a closed
 * curve is true of the entire border. Left alone, the whole strip collapses onto that point. See
 * the skip in the candidate scan.
 */
static void ribbon_fix_border_self_intersections(const Span<float3> poly,
                                                 const float3 &axis_x,
                                                 const float3 &axis_y,
                                                 const float R,
                                                 Vector<float3> &border,
                                                 const bool cyclic)
{
  const int count = int(poly.size());
  if (count < 4) {
    return;
  }

  Vector<float2> b2d(count);
  for (int k = 0; k < count; k++) {
    b2d[k] = float2(math::dot(border[k], axis_x), math::dot(border[k], axis_y));
  }

  /* Uniform grid over the segment start points, so the crossing search only visits segments that
   * can actually reach segment `i`.
   *
   * Two segments cross only if their start points are no farther apart than the sum of their
   * lengths, so a cell of twice the longest segment makes a 3x3 cell neighborhood a conservative
   * candidate set. The exhaustive scan this replaces tested every later segment for every segment:
   * quadratic in the TESSELLATED point count, which is where an interactive re-stamp of a long
   * curve spent most of its time (a Roll hand-off resamples to many control points, and each of
   * those is subdivided again by the curve's evaluation resolution). */
  const int seg_num = count - 1;
  float max_seg_len = 0.0f;
  float2 grid_min(FLT_MAX), grid_max(-FLT_MAX);
  for (int k = 0; k < seg_num; k++) {
    max_seg_len = std::max(max_seg_len, math::distance(b2d[k], b2d[k + 1]));
    grid_min = math::min(grid_min, b2d[k]);
    grid_max = math::max(grid_max, b2d[k]);
  }
  const float2 grid_extent = math::max(grid_max - grid_min, float2(1e-6f));
  const float cell_size = std::max(2.0f * max_seg_len, 1e-6f);
  /* Capped so a curve whose segments are tiny relative to its extent cannot allocate a huge grid;
   * a coarser grid only widens the candidate set, it never misses a crossing. */
  const int grid_x = std::clamp(int(grid_extent.x / cell_size) + 1, 1, 512);
  const int grid_y = std::clamp(int(grid_extent.y / cell_size) + 1, 1, 512);
  const float2 grid_scale(float(grid_x) / grid_extent.x, float(grid_y) / grid_extent.y);
  auto cell_x_of = [&](const float2 p) {
    return std::clamp(int((p.x - grid_min.x) * grid_scale.x), 0, grid_x - 1);
  };
  auto cell_y_of = [&](const float2 p) {
    return std::clamp(int((p.y - grid_min.y) * grid_scale.y), 0, grid_y - 1);
  };

  /* Counting sort of the segment indices into cells. */
  Vector<int> cell_start(grid_x * grid_y + 1, 0);
  Vector<int> cell_segs(seg_num);
  for (int k = 0; k < seg_num; k++) {
    cell_start[cell_y_of(b2d[k]) * grid_x + cell_x_of(b2d[k]) + 1]++;
  }
  for (int c = 0; c < grid_x * grid_y; c++) {
    cell_start[c + 1] += cell_start[c];
  }
  {
    Vector<int> cursor(cell_start);
    for (int k = 0; k < seg_num; k++) {
      cell_segs[cursor[cell_y_of(b2d[k]) * grid_x + cell_x_of(b2d[k])]++] = k;
    }
  }

  Vector<float3> orig(border);
  Vector<int> candidates;
  int skip_until = -1;

  for (int i = 0; i < count - 1; i++) {
    if (i <= skip_until) {
      continue;
    }
    const float2 d1 = b2d[i + 1] - b2d[i];
    if (math::dot(d1, d1) < 1e-8f) {
      continue;
    }

    /* Gather the nearby later segments, highest index first. The original scan walked `j` downward
     * from the far end and stopped at its first hit, so it always collapsed the WIDEST loop through
     * `i`; sorting descending here keeps that choice identical. */
    candidates.clear();
    const int cx = cell_x_of(b2d[i]);
    const int cy = cell_y_of(b2d[i]);
    for (int oy = std::max(0, cy - 1); oy <= std::min(grid_y - 1, cy + 1); oy++) {
      for (int ox = std::max(0, cx - 1); ox <= std::min(grid_x - 1, cx + 1); ox++) {
        const int c = oy * grid_x + ox;
        for (int t = cell_start[c]; t < cell_start[c + 1]; t++) {
          if (cell_segs[t] >= i + 2) {
            candidates.append(cell_segs[t]);
          }
        }
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](const int a, const int b) { return a > b; });

    for (const int j : candidates) {
      /* On a closed curve the last segment and the first are neighbours THROUGH the join, exactly
       * like any consecutive pair -- which the candidate gather already excludes via
       * `cell_segs[t] >= i + 2`. That rule just cannot see across the wrap. Without this skip the
       * shared join vertex reads as a crossing and collapses the whole strip. */
      if (cyclic && i == 0 && j == count - 2) {
        continue;
      }
      const float2 d2 = b2d[j + 1] - b2d[j];
      if (math::dot(d2, d2) < 1e-8f) {
        continue;
      }

      const float2 ac = b2d[j] - b2d[i];
      const float denom = d1.x * d2.y - d1.y * d2.x;
      if (std::abs(denom) < 1e-8f) {
        continue;
      }
      const float s = (ac.x * d2.y - ac.y * d2.x) / denom;
      const float t = (ac.x * d1.y - ac.y * d1.x) / denom;

      if (s >= 0.0f && s <= 1.0f && t >= 0.0f && t <= 1.0f) {
        bool all_inside = true;
        const int m_lo = std::max(0, i - 2);
        const int m_hi = std::min(count - 2, j + 2);
        const float R_sq = (R * 1.2f) * (R * 1.2f); /* 20% margin for inclined borders. */
        /* Validate all loop vertices are within R of the center polyline. Also find the apex =
         * vertex with sharpest curvature on the border (largest turning angle between consecutive
         * segments). */
        int apex_idx = (i + 1 + j) / 2; /* Default to midpoint. */
        float max_turn_angle = -1.0f;
        for (int k = i + 1; k <= j; k++) {
          float min_dist_sq = FLT_MAX;
          for (int m = m_lo; m <= m_hi; m++) {
            const float3 ab = poly[m + 1] - poly[m];
            const float ab_dot = math::dot(ab, ab);
            const float tp = (ab_dot > 1e-12f) ?
                                 std::clamp(math::dot(orig[k] - poly[m], ab) / ab_dot, 0.0f, 1.0f) :
                                 0.0f;
            const float3 proj = math::interpolate(poly[m], poly[m + 1], tp);
            min_dist_sq = std::min(min_dist_sq, math::distance_squared(orig[k], proj));
          }
          if (min_dist_sq >= R_sq) {
            all_inside = false;
            break;
          }
          /* Curvature: angle between incoming and outgoing border segments at vertex k (in the 2D
           * projection). Higher = sharper turn. */
          if (k > i + 1 && k < j) {
            const float2 seg_prev = b2d[k] - b2d[k - 1];
            const float2 seg_next = b2d[k + 1] - b2d[k];
            const float lp = math::length(seg_prev);
            const float ln = math::length(seg_next);
            if (lp > 1e-7f && ln > 1e-7f) {
              const float cos_a = math::dot(seg_prev, seg_next) / (lp * ln);
              const float turn = 1.0f - std::clamp(cos_a, -1.0f, 1.0f);
              if (turn > max_turn_angle) {
                max_turn_angle = turn;
                apex_idx = k;
              }
            }
          }
        }
        if (!all_inside) {
          continue;
        }

        /* Standard merge point from the segment intersection. */
        const float3 pa = orig[i] + (orig[i + 1] - orig[i]) * s;
        const float3 pb = orig[j] + (orig[j + 1] - orig[j]) * t;
        const float3 cross_pt = (pa + pb) * 0.5f;

        int collapse_lo = i + 1;
        int collapse_hi = j;
        float3 merge_pt = cross_pt;

        /* If the intersection loop arc length exceeds 3R along the center polyline, only collapse
         * the sharpest portion centered on the apex. Remaining overlap vertices are linearly
         * interpolated between the intersection endpoints and the merge point. */
        const float max_collapse_arc = 3.0f * R;
        float span_arc = 0.0f;
        for (int k = i; k < j; k++) {
          span_arc += math::distance(poly[k], poly[k + 1]);
        }
        const bool is_partial = span_arc > max_collapse_arc && (j - i) > 3;
        if (is_partial) {
          /* Center the window on the apex using arc-length balance: grow outward from the apex in
           * both directions, spending half the budget on each side. */
          const float half_arc = max_collapse_arc * 0.5f;
          collapse_lo = apex_idx;
          collapse_hi = apex_idx;
          float arc_lo = 0.0f, arc_hi = 0.0f;
          while (collapse_lo > i + 1 || collapse_hi < j) {
            const bool can_lo = (collapse_lo > i + 1) && (arc_lo <= arc_hi);
            const bool can_hi = (collapse_hi < j) && (arc_hi <= arc_lo);
            if (can_lo) {
              const float seg = math::distance(poly[collapse_lo - 1], poly[collapse_lo]);
              if (arc_lo + seg > half_arc) {
                break;
              }
              collapse_lo--;
              arc_lo += seg;
            }
            else if (can_hi) {
              const float seg = math::distance(poly[collapse_hi], poly[collapse_hi + 1]);
              if (arc_hi + seg > half_arc) {
                break;
              }
              collapse_hi++;
              arc_hi += seg;
            }
            else {
              break;
            }
          }
          merge_pt = (orig[collapse_lo] + orig[collapse_hi]) * 0.5f;
        }

        for (int k = collapse_lo; k <= collapse_hi; k++) {
          border[k] = merge_pt;
        }
        if (is_partial) {
          /* Linearly interpolate border vertices between the real intersection endpoints and the
           * merge point, evenly spacing the overlap zone instead of leaving it overlapping. */
          const int n_before = collapse_lo - (i + 1);
          for (int k = 0; k < n_before; k++) {
            const float t2 = float(k + 1) / float(n_before + 1);
            border[i + 1 + k] = math::interpolate(orig[i + 1], merge_pt, t2);
          }
          const int n_after = j - collapse_hi;
          for (int k = 0; k < n_after; k++) {
            const float t2 = float(k + 1) / float(n_after + 1);
            border[collapse_hi + 1 + k] = math::interpolate(merge_pt, orig[j], t2);
          }
        }
        /* Skip the entire original intersection span so the uncollapsed outer portions are not
         * re-tested by later iterations. */
        skip_until = j;
        break;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid Subdivision + Smoothing
 * \{ */

/**
 * Catmull-Clark subdivision of the ribbon grid, `rows x cols` -> `(2*rows-1) x (2*cols-1)`.
 * `pin_border_columns` selects the boundary rule; everything else is shared.
 *
 * Pinned (the first pass): every original vertex keeps its exact position except the center column,
 * which takes the 1D boundary rule ALONG the curve so cross-strip segments can curve near merge
 * points, and every vertical edge point is a plain midpoint. The borders therefore carry the
 * self-intersection collapse geometry through unchanged.
 *
 * Standard (the second pass): only the border columns stay pinned; interior vertices take the full
 * CC vertex rule and the first/last row takes the 1D rule ACROSS the strip.
 */
static void ribbon_subdivide(Vector<float3> &grid_pos,
                             Vector<float2> &grid_uv,
                             int &rows,
                             int &cols,
                             const bool pin_border_columns)
{
  const int oR = rows, oC = cols;
  const int nR = 2 * oR - 1;
  const int nC = 2 * oC - 1;
  const int fR = oR - 1;
  const int fC = oC - 1;

  auto oi = [oC](int r, int c) { return r * oC + c; };
  auto ni = [nC](int r, int c) { return r * nC + c; };

  /* Face points (average of the 4 corners of each quad). */
  Vector<float3> fp(fR * fC);
  Vector<float2> fu(fR * fC);
  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < fC; c++) {
      fp[r * fC + c] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)] +
                                grid_pos[oi(r + 1, c)] + grid_pos[oi(r + 1, c + 1)]);
      fu[r * fC + c] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)] +
                                grid_uv[oi(r + 1, c)] + grid_uv[oi(r + 1, c + 1)]);
    }
  }

  Vector<float3> np(nR * nC);
  Vector<float2> nu(nR * nC);

  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < fC; c++) {
      np[ni(2 * r + 1, 2 * c + 1)] = fp[r * fC + c];
      nu[ni(2 * r + 1, 2 * c + 1)] = fu[r * fC + c];
    }
  }

  /* Horizontal edge points (even row, odd col). Interior: average of endpoints + adjacent face
   * points. Boundary (first/last row): simple midpoint. Same in both passes. */
  for (int r = 0; r < oR; r++) {
    for (int c = 0; c < fC; c++) {
      if (r == 0 || r == oR - 1) {
        np[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)]);
        nu[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)]);
      }
      else {
        np[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)] +
                                            fp[(r - 1) * fC + c] + fp[r * fC + c]);
        nu[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)] +
                                            fu[(r - 1) * fC + c] + fu[r * fC + c]);
      }
    }
  }

  /* Vertical edge points (odd row, even col). Pinned keeps every one of them a plain midpoint, so
   * the center vertical edges stay on the curve polyline and the borders keep the collapse
   * geometry. Standard applies the CC interior rule away from the border columns. */
  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < oC; c++) {
      const bool border_col = (c == 0 || c == oC - 1);
      if (pin_border_columns || border_col) {
        np[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_pos[oi(r, c)] + grid_pos[oi(r + 1, c)]);
        nu[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_uv[oi(r, c)] + grid_uv[oi(r + 1, c)]);
      }
      else {
        np[ni(2 * r + 1, 2 * c)] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r + 1, c)] +
                                            fp[r * fC + c - 1] + fp[r * fC + c]);
        nu[ni(2 * r + 1, 2 * c)] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r + 1, c)] +
                                            fu[r * fC + c - 1] + fu[r * fC + c]);
      }
    }
  }

  /* Original vertex points (even row, even col) -- the two passes differ most here. */
  for (int r = 0; r < oR; r++) {
    for (int c = 0; c < oC; c++) {
      if (pin_border_columns) {
        /* `c == 1` rather than "not a border column": the pinned pass runs only on the 3-column
         * seed grid (`rows x 3` -> `(2*rows-1) x 5`), where the two are the same thing. Spelling it
         * literally keeps this branch identical to the pass it replaces at every grid size. */
        if (c == 1 && r > 0 && r < oR - 1) {
          np[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_pos[oi(r - 1, c)] +
                                                  6.0f * grid_pos[oi(r, c)] +
                                                  grid_pos[oi(r + 1, c)]);
          nu[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_uv[oi(r - 1, c)] +
                                                  6.0f * grid_uv[oi(r, c)] +
                                                  grid_uv[oi(r + 1, c)]);
        }
        else {
          np[ni(2 * r, 2 * c)] = grid_pos[oi(r, c)];
          nu[ni(2 * r, 2 * c)] = grid_uv[oi(r, c)];
        }
        continue;
      }

      if (c == 0 || c == oC - 1) {
        np[ni(2 * r, 2 * c)] = grid_pos[oi(r, c)];
        nu[ni(2 * r, 2 * c)] = grid_uv[oi(r, c)];
      }
      else if (r == 0 || r == oR - 1) {
        np[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_pos[oi(r, c - 1)] + 6.0f * grid_pos[oi(r, c)] +
                                                grid_pos[oi(r, c + 1)]);
        nu[ni(2 * r, 2 * c)] = (1.0f / 8.0f) *
                               (grid_uv[oi(r, c - 1)] + 6.0f * grid_uv[oi(r, c)] +
                                grid_uv[oi(r, c + 1)]);
      }
      else {
        const float3 Q = 0.25f * (fp[(r - 1) * fC + (c - 1)] + fp[(r - 1) * fC + c] +
                                  fp[r * fC + (c - 1)] + fp[r * fC + c]);
        const float2 Qu = 0.25f * (fu[(r - 1) * fC + (c - 1)] + fu[(r - 1) * fC + c] +
                                   fu[r * fC + (c - 1)] + fu[r * fC + c]);
        const float3 Rp = 0.25f * (0.5f * (grid_pos[oi(r, c - 1)] + grid_pos[oi(r, c)]) +
                                   0.5f * (grid_pos[oi(r, c + 1)] + grid_pos[oi(r, c)]) +
                                   0.5f * (grid_pos[oi(r - 1, c)] + grid_pos[oi(r, c)]) +
                                   0.5f * (grid_pos[oi(r + 1, c)] + grid_pos[oi(r, c)]));
        const float2 Ru = 0.25f * (0.5f * (grid_uv[oi(r, c - 1)] + grid_uv[oi(r, c)]) +
                                   0.5f * (grid_uv[oi(r, c + 1)] + grid_uv[oi(r, c)]) +
                                   0.5f * (grid_uv[oi(r - 1, c)] + grid_uv[oi(r, c)]) +
                                   0.5f * (grid_uv[oi(r + 1, c)] + grid_uv[oi(r, c)]));
        np[ni(2 * r, 2 * c)] = (Q + 2.0f * Rp + grid_pos[oi(r, c)]) / 4.0f;
        nu[ni(2 * r, 2 * c)] = (Qu + 2.0f * Ru + grid_uv[oi(r, c)]) / 4.0f;
      }
    }
  }

  grid_pos = std::move(np);
  grid_uv = std::move(nu);
  rows = nR;
  cols = nC;
}

/**
 * 2D Laplacian smoothing: pin border columns, smooth everything between them (including the
 * center). Curves the cross-strip segments near collapse points like a subdivision surface.
 * Double-buffered to avoid per-iteration allocation.
 */
static void ribbon_laplacian_smooth(Vector<float3> &grid_pos,
                                    Vector<float2> &grid_uv,
                                    const int rows,
                                    const int cols)
{
  if (rows < 3 || cols < 3) {
    return;
  }
  constexpr int smooth_iters = 10;
  constexpr float mix = 0.5f;
  const int grid_total = rows * cols;
  Vector<float3> buf_p(grid_total);
  Vector<float2> buf_u(grid_total);
  float3 *src_p = grid_pos.data(), *dst_p = buf_p.data();
  float2 *src_u = grid_uv.data(), *dst_u = buf_u.data();
  for (int iter = 0; iter < smooth_iters; iter++) {
    /* Carry over the pinned boundary only -- the loop below writes every interior cell, so copying
     * the whole grid every iteration moved megabytes per re-stamp to no effect. */
    for (int c = 0; c < cols; c++) {
      const int last = (rows - 1) * cols + c;
      dst_p[c] = src_p[c];
      dst_u[c] = src_u[c];
      dst_p[last] = src_p[last];
      dst_u[last] = src_u[last];
    }
    for (int r = 1; r < rows - 1; r++) {
      const int right = r * cols + cols - 1;
      dst_p[r * cols] = src_p[r * cols];
      dst_u[r * cols] = src_u[r * cols];
      dst_p[right] = src_p[right];
      dst_u[right] = src_u[right];
    }
    threading::parallel_for(IndexRange(1, rows - 2), 64, [&](const IndexRange range) {
      for (const int64_t r : range) {
        for (int c = 1; c < cols - 1; c++) {
          const int idx = int(r) * cols + c;
          const float3 avg_p = 0.25f * (src_p[idx - cols] + src_p[idx + cols] + src_p[idx - 1] +
                                        src_p[idx + 1]);
          const float2 avg_u = 0.25f * (src_u[idx - cols] + src_u[idx + cols] + src_u[idx - 1] +
                                        src_u[idx + 1]);
          dst_p[idx] = (1.0f - mix) * src_p[idx] + mix * avg_p;
          dst_u[idx] = (1.0f - mix) * src_u[idx] + mix * avg_u;
        }
      }
    });
    std::swap(src_p, dst_p);
    std::swap(src_u, dst_u);
  }
  if (src_p != grid_pos.data()) {
    memcpy(grid_pos.data(), src_p, sizeof(float3) * grid_total);
    memcpy(grid_uv.data(), src_u, sizeof(float2) * grid_total);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Build + Sample
 * \{ */

/** Hash of everything #curve_patch_ribbon_build reads, so an unchanged curve can skip the rebuild.
 * Deliberately O(n) over the tessellated polyline: still orders of magnitude below the build. */
static uint64_t ribbon_source_hash(const CurvePatchSpline &spline,
                                   const float brush_radius,
                                   const bool high_quality,
                                   const float end_margin_start,
                                   const float end_margin_end,
                                   const Span<float3> binormals)
{
  uint64_t hash = 1469598103934665603ull; /* FNV-1a offset basis. */
  auto mix = [&hash](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    hash = (hash ^ uint64_t(bits)) * 1099511628211ull;
  };
  hash ^= uint64_t(spline.poly_3d.size()) * 1099511628211ull;
  /* `cyclic` is a build input in its own right (it suppresses the join's false self-intersection),
   * so it belongs in the hash even though closing a curve also changes `poly_3d`. */
  hash ^= uint64_t(spline.cyclic) * 1099511628211ull;
  for (const float3 &p : spline.poly_3d) {
    mix(p.x);
    mix(p.y);
    mix(p.z);
  }
  for (const float radius : spline.radii) {
    mix(radius);
  }
  mix(brush_radius);
  mix(spline.plane_normal.x);
  mix(spline.plane_normal.y);
  mix(spline.plane_normal.z);
  /* Without this the commit-time high-quality rebuild would be skipped as a cache hit against the
   * interactive table, which is exactly the table it needs to replace. */
  hash = (hash ^ uint64_t(high_quality ? 1 : 0)) * 1099511628211ull;
  /* The end extension changes the strip's geometry without touching any of the spline data hashed
   * above, so a margin change would otherwise be served from the stale table -- switching between
   * Ribbon and Stamps mode, or moving the Jitter slider, would leave the ends unextended. */
  mix(end_margin_start);
  mix(end_margin_end);
  /* The binormals arrive from outside and change independently of everything hashed above (the
   * smoothed normal field follows the surface snapshot, not the polyline), so without them a
   * changed field would be served from a stale table. */
  hash ^= uint64_t(binormals.size()) * 1099511628211ull;
  for (const float3 &b : binormals) {
    mix(b.x);
    mix(b.y);
    mix(b.z);
  }
  return hash;
}

void CurvePatchRibbonGrid::clear()
{
  positions.clear_and_shrink();
  uv.clear_and_shrink();
  rows = 0;
  cols = 0;
  axis_x = float3(1.0f, 0.0f, 0.0f);
  axis_y = float3(0.0f, 1.0f, 0.0f);
  max_halfwidth = 0.0f;
}

bool curve_patch_ribbon_grid_build(const CurvePatchSpline &spline,
                                   const float brush_radius,
                                   const Span<float3> binormals,
                                   const float end_margin_start,
                                   const float end_margin_end,
                                   CurvePatchRibbonGrid &r_grid)
{
  r_grid.clear();
  if (spline.is_empty() || brush_radius <= 0.0f) {
    return false;
  }

  const bool have_radii = spline.radii.size() == spline.poly_3d.size();

  /* End extension. Prepending/appending one extrapolated sample at each end has to happen HERE,
   * before anything below reads the polyline: the half-widths, the two border curves, the
   * self-intersection collapse, both subdivision passes, the smoothing and the rasterization all
   * walk the same index space, so extending afterwards would leave them disagreeing about which
   * index is which. Local copies rather than mutating the spline: `spline` is shared, const, and
   * everything else (the stamp layout, `evaluate()`, the node cull) must keep seeing the curve's
   * true ends.
   *
   * A cyclic curve is never extended -- it has no ends, and appending a sample past the join would
   * make the strip overlap itself there and re-introduce the false self-intersection the `cyclic`
   * flag exists to suppress. */
  const bool extend = (end_margin_start > 0.0f || end_margin_end > 0.0f) && !spline.cyclic &&
                      spline.poly_3d.size() >= 2 &&
                      spline.lengths_3d.size() == spline.poly_3d.size();
  Vector<float3> poly_ext, tangents_ext;
  Vector<float> lengths_ext, radii_ext;
  if (extend) {
    const int n = int(spline.poly_3d.size());
    poly_ext.reserve(n + 2);
    tangents_ext.reserve(n + 2);
    lengths_ext.reserve(n + 2);
    if (have_radii) {
      radii_ext.reserve(n + 2);
    }
    /* The extrapolated positions follow the END TANGENTS, so the extension continues the curve's
     * direction instead of the last segment's chord. Its radius is copied from the end sample so
     * the strip keeps its width all the way out, and its arc length simply continues past the ends
     * -- negative at the start, which is why `v` may no longer be assumed non-negative. */
    if (end_margin_start > 0.0f) {
      poly_ext.append(spline.poly_3d.first() - spline.tangents_3d.first() * end_margin_start);
      tangents_ext.append(spline.tangents_3d.first());
      lengths_ext.append(-end_margin_start);
      if (have_radii) {
        radii_ext.append(spline.radii.first());
      }
    }
    poly_ext.extend(spline.poly_3d.as_span());
    tangents_ext.extend(spline.tangents_3d.as_span());
    lengths_ext.extend(spline.lengths_3d.as_span());
    if (have_radii) {
      radii_ext.extend(spline.radii.as_span());
    }
    if (end_margin_end > 0.0f) {
      poly_ext.append(spline.poly_3d.last() + spline.tangents_3d.last() * end_margin_end);
      tangents_ext.append(spline.tangents_3d.last());
      lengths_ext.append(spline.lengths_3d.last() + end_margin_end);
      if (have_radii) {
        radii_ext.append(spline.radii.last());
      }
    }
  }

  const Span<float3> poly = extend ? poly_ext.as_span() : spline.poly_3d.as_span();
  const Span<float3> tangents = extend ? tangents_ext.as_span() : spline.tangents_3d.as_span();
  const Span<float> lengths = extend ? lengths_ext.as_span() : spline.lengths_3d.as_span();
  const Span<float> radii = extend ? radii_ext.as_span() : spline.radii.as_span();
  const int count = int(poly.size());
  const float3 plane_normal = math::normalize(spline.plane_normal);

  /* The binormals live in the same index space as the polyline, so extending the ends has to extend
   * them too -- by copying the outermost value, exactly as the radii do. */
  Vector<float3> binormals_ext;
  if (!binormals.is_empty() && extend) {
    binormals_ext.reserve(count);
    if (end_margin_start > 0.0f) {
      binormals_ext.append(binormals.first());
    }
    binormals_ext.extend(binormals);
    if (end_margin_end > 0.0f) {
      binormals_ext.append(binormals.last());
    }
  }
  const Span<float3> binormals_use = binormals.is_empty() ?
                                         Span<float3>() :
                                         (extend ? binormals_ext.as_span() : binormals);
  const bool have_binormals = binormals_use.size() == count;

  /* Per-vertex world half-width; `R_max` drives the collapse validation and the LUT resolution. */
  float R_max = 0.0f;
  Vector<float> halfwidths(count);
  for (int i = 0; i < count; i++) {
    const float w = have_radii ? std::max(radii[i] * brush_radius, brush_radius * 0.01f) :
                                 brush_radius;
    halfwidths[i] = w;
    R_max = std::max(R_max, w);
  }
  if (R_max <= 0.0f) {
    return false;
  }

  /* In-plane projection axes. Mirrors Roll's axis construction so the collapse detection and the
   * LUT share one frame. */
  float3 axis_x = math::cross(plane_normal, float3(0.0f, 0.0f, 1.0f));
  if (math::length_squared(axis_x) < 1e-6f) {
    axis_x = math::cross(plane_normal, float3(1.0f, 0.0f, 0.0f));
  }
  axis_x = math::normalize(axis_x);
  const float3 axis_y = math::normalize(math::cross(plane_normal, axis_x));

  /* Border curves. `B = cross(T, plane_normal)` so that the `+B` side carries U = +1: the old
   * parameterization exposed `u = dot(offset, cross(T, plane_normal)) / radius` (the negated
   * lateral), and the texture orientation must not flip with this rework. */
  Vector<float3> border_left(count);  /* +B side, U = +1. */
  Vector<float3> border_right(count); /* -B side, U = -1. */
  /* World half-widths actually used on each side after the cyclic inward cap. Seed the grid's `u`
   * from these so `u * ribbon_radius` still reconstructs the true lateral distance when the two
   * sides differ. */
  Vector<float> half_pos(count);
  Vector<float> half_neg(count);

  /* Closed-curve centroid, used to cap the INWARD half-width so a brush thicker than the loop
   * cannot push the inner border past the medial axis. Without that, opposite legs of the strip
   * overlap in the interior, write conflicting `(u, s)` into the same LUT cells, and the relief
   * turns into pixel-scale spikes (Close Curve with a large radius). The outward side keeps the
   * full brush radius. For cyclic polylines the last sample repeats the first -- skip it so the
   * join is not double-counted. */
  float3 loop_centroid(0.0f);
  if (spline.cyclic && count >= 3) {
    const int unique_count = count - 1;
    for (int i = 0; i < unique_count; i++) {
      loop_centroid += poly[i];
    }
    loop_centroid /= float(unique_count);
  }

  for (int i = 0; i < count; i++) {
    const float3 &T = tangents[i];
    float3 B;
    if (have_binormals) {
      B = binormals_use[i];
      const float blen = math::length(B);
      B = blen > 1e-7f ? B / blen : math::cross(T, plane_normal);
    }
    else {
      B = math::cross(T, plane_normal);
      const float blen = math::length(B);
      if (blen > 1e-7f) {
        B /= blen;
      }
      else {
        /* Degenerate: tangent parallel to the plane normal. */
        B = math::cross(T, float3(0.0f, 0.0f, 1.0f));
        if (math::length_squared(B) < 1e-12f) {
          B = math::cross(T, float3(1.0f, 0.0f, 0.0f));
        }
        B = math::normalize(B);
      }
    }
    half_pos[i] = halfwidths[i];
    half_neg[i] = halfwidths[i];
    if (spline.cyclic && count >= 3) {
      const float3 to_centroid = loop_centroid - poly[i];
      const float dist_in = math::length(to_centroid);
      if (dist_in > 1e-8f) {
        /* Leave a small hole so opposite legs never meet; 5% is well below LUT pixel size for
         * typical brush radii and keeps the max-|height| merge from having to arbitrate a
         * near-tie across the whole interior. */
        const float max_inward = dist_in * 0.95f;
        if (math::dot(B, to_centroid) > 0.0f) {
          half_pos[i] = std::min(half_pos[i], max_inward);
        }
        else {
          half_neg[i] = std::min(half_neg[i], max_inward);
        }
      }
    }
    border_left[i] = poly[i] + B * half_pos[i];
    border_right[i] = poly[i] - B * half_neg[i];
  }

  ribbon_fix_border_self_intersections(poly, axis_x, axis_y, R_max, border_left, spline.cyclic);
  ribbon_fix_border_self_intersections(poly, axis_x, axis_y, R_max, border_right, spline.cyclic);

  /* Seed grid: one row per polyline vertex, columns = {right, center, left}. V = raw arc
   * length. `u` is normalized by the *nominal* half-width (`halfwidths`), not the possibly
   * capped side length, so a capped inward border sits at `|u| < 1` and the sampler's
   * `u * ribbon_radius` reconstruction stays in world units. */
  int rows = count;
  int cols = 3;
  Vector<float3> grid_pos(rows * cols);
  Vector<float2> grid_uv(rows * cols);
  for (int k = 0; k < count; k++) {
    const float v = lengths[k];
    const float inv_w = halfwidths[k] > 1e-8f ? 1.0f / halfwidths[k] : 0.0f;
    grid_pos[k * 3 + 0] = border_right[k];
    grid_pos[k * 3 + 1] = poly[k];
    grid_pos[k * 3 + 2] = border_left[k];
    grid_uv[k * 3 + 0] = float2(-half_neg[k] * inv_w, v);
    grid_uv[k * 3 + 1] = float2(0.0f, v);
    grid_uv[k * 3 + 2] = float2(half_pos[k] * inv_w, v);
  }

  ribbon_subdivide(grid_pos, grid_uv, rows, cols, /*pin_border_columns=*/true);
  ribbon_subdivide(grid_pos, grid_uv, rows, cols, /*pin_border_columns=*/false);
  ribbon_laplacian_smooth(grid_pos, grid_uv, rows, cols);

  r_grid.positions = std::move(grid_pos);
  r_grid.uv = std::move(grid_uv);
  r_grid.rows = rows;
  r_grid.cols = cols;
  r_grid.axis_x = axis_x;
  r_grid.axis_y = axis_y;
  r_grid.max_halfwidth = R_max;
  return true;
}

void curve_patch_ribbon_build(const CurvePatchSpline &spline,
                              const float brush_radius,
                              CurvePatchRibbonLut &r_lut,
                              const bool high_quality,
                              const float end_margin_start,
                              const float end_margin_end,
                              const Span<float3> binormals)
{
  /* Nothing the ribbon depends on has changed, so the existing LUT is still exact. The modal editor
   * re-stamps on events that never touch the curve (strength slider, Length mode, Repeats count),
   * and those otherwise paid for a full rebuild each time. */
  const uint64_t source_hash = ribbon_source_hash(
      spline, brush_radius, high_quality, end_margin_start, end_margin_end, binormals);
  if (r_lut.ready && r_lut.source_hash == source_hash) {
    return;
  }
  /* Deliberately not #clear: the pixel arrays are reused below when the resolution is unchanged.
   * `ready` stays false until the build completes, so a bail-out leaves the LUT unusable rather
   * than stale. */
  r_lut.ready = false;
  r_lut.source_hash = source_hash;

  CurvePatchRibbonGrid grid;
  if (!curve_patch_ribbon_grid_build(
          spline, brush_radius, binormals, end_margin_start, end_margin_end, grid))
  {
    return;
  }

  /* Named locally so everything below reads exactly as it did while the strip was built here. */
  const Span<float3> grid_pos = grid.positions;
  const Span<float2> grid_uv = grid.uv;
  const int rows = grid.rows;
  const int cols = grid.cols;
  const float3 axis_x = grid.axis_x;
  const float3 axis_y = grid.axis_y;
  const float R_max = grid.max_halfwidth;

  /* Project the grid into the ribbon plane. */
  const int total = rows * cols;
  Vector<float2> grid_pos_2d(total);
  float2 bb_min(FLT_MAX), bb_max(-FLT_MAX);
  for (int i = 0; i < total; i++) {
    const float2 p(math::dot(grid_pos[i], axis_x), math::dot(grid_pos[i], axis_y));
    grid_pos_2d[i] = p;
    bb_min = math::min(bb_min, p);
    bb_max = math::max(bb_max, p);
  }
  {
    const float2 margin = (bb_max - bb_min) * 0.1f;
    bb_min -= margin;
    bb_max += margin;
  }
  const float2 ext = bb_max - bb_min;
  if (!(ext.x > 1e-10f) || !(ext.y > 1e-10f)) {
    return;
  }

  /* Adaptive resolution: unlike Roll (whose LUT only covers a ~5-radius window around the dab and
   * gets away with a small fixed size), this LUT spans the WHOLE curve. Aim for pixels no larger
   * than a fraction of the largest half-width so the across-strip coordinate keeps sub-strip
   * precision on long thin curves, clamped to keep the per-restamp fill cost bounded.
   *
   * The high-quality pass roughly doubles the density: its supersampled relief places samples a few
   * percent of a strip-width apart, and the interactive table's pixels are coarser than that. */
  const float pixel_fraction = high_quality ? 0.075f : 0.15f;
  const int res_cap = high_quality ? 1024 : 512;
  const float max_extent = std::max(ext.x, ext.y);
  /* Quantized to 64-pixel steps: dragging a control point changes the curve's extent slightly on
   * every event, and an exact resolution would therefore drift by a pixel or two each time and
   * force the arrays below to be reallocated for no benefit. */
  const int res_target = int(max_extent / (pixel_fraction * R_max)) + 1;
  const int res = std::clamp((res_target + 63) / 64 * 64, 128, res_cap);
  const float2 inv_ext(float(res) / ext.x, float(res) / ext.y);

  const int lut_total = res * res;
  /* Reuse the previous allocation whenever the resolution is unchanged. An interactive drag
   * rebuilds the LUT on every mouse event, and releasing then re-acquiring several megabytes each
   * time is pure overhead -- only the reset below is actually needed. */
  if (r_lut.uv.size() != lut_total) {
    r_lut.uv.reinitialize(lut_total);
    r_lut.dist_sq.reinitialize(lut_total);
    r_lut.row.reinitialize(lut_total);
    r_lut.uv2.reinitialize(lut_total);
    r_lut.dist_sq2.reinitialize(lut_total);
    r_lut.row2.reinitialize(lut_total);
  }
  r_lut.res = res;
  threading::parallel_for(IndexRange(lut_total), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      r_lut.uv[i] = float2(FLT_MAX, 0.0f);
      r_lut.dist_sq[i] = FLT_MAX;
      r_lut.row[i] = -1;
      r_lut.uv2[i] = float2(FLT_MAX, 0.0f);
      r_lut.dist_sq2[i] = FLT_MAX;
      r_lut.row2[i] = -1;
    }
  });

  auto cross2d = [](const float2 a, const float2 b) { return a.x * b.y - a.y * b.x; };

  /* Rasterize every grid quad into the LUT via bilinear inverse: for a pixel-center Q inside quad
   * (P00, P10, P01, P11) solve `Q = (1-u)(1-v)P00 + u(1-v)P10 + (1-u)v P01 + uv P11` -- a
   * quadratic in v, then a linear solve for u. When multiple quads overlap (at self-intersection
   * collapses), earlier rows win unless the competing row is adjacent (within 2), in which case
   * the better bilinear fit wins -- keeps overlapping branches at sharp turns from flickering per
   * pixel. */
  for (int r = 0; r < rows - 1; r++) {
    for (int c = 0; c < cols - 1; c++) {
      const float2 P00 = grid_pos_2d[r * cols + c];
      const float2 P10 = grid_pos_2d[r * cols + c + 1];
      const float2 P01 = grid_pos_2d[(r + 1) * cols + c];
      const float2 P11 = grid_pos_2d[(r + 1) * cols + c + 1];

      const float2 qmin = math::min(math::min(P00, P10), math::min(P01, P11));
      const float2 qmax = math::max(math::max(P00, P10), math::max(P01, P11));
      const int px_lo = std::max(0, int((qmin.x - bb_min.x) * inv_ext.x));
      const int px_hi = std::min(res - 1, int((qmax.x - bb_min.x) * inv_ext.x) + 1);
      const int py_lo = std::max(0, int((qmin.y - bb_min.y) * inv_ext.y));
      const int py_hi = std::min(res - 1, int((qmax.y - bb_min.y) * inv_ext.y) + 1);

      const float2 a = P10 - P00;
      const float2 b = P01 - P00;
      const float2 tw = P11 - P10 - P01 + P00; /* Twist term. */
      const float A_coeff = cross2d(b, tw);

      for (int py = py_lo; py <= py_hi; py++) {
        for (int px = px_lo; px <= px_hi; px++) {
          const float2 query = bb_min + float2(px + 0.5f, py + 0.5f) / float(res) * ext;
          const float2 d = query - P00;

          const float B_coeff = cross2d(b, a) - cross2d(d, tw);
          const float C_coeff = -cross2d(d, a);

          float v;
          if (std::abs(A_coeff) < 1e-8f) {
            v = (std::abs(B_coeff) > 1e-8f) ? -C_coeff / B_coeff : 0.5f;
          }
          else {
            const float disc = B_coeff * B_coeff - 4.0f * A_coeff * C_coeff;
            if (disc < 0.0f) {
              continue;
            }
            const float sq = sqrtf(disc);
            const float v1 = (-B_coeff + sq) / (2.0f * A_coeff);
            const float v2 = (-B_coeff - sq) / (2.0f * A_coeff);
            const float e1 = std::max(0.0f, std::max(-v1, v1 - 1.0f));
            const float e2 = std::max(0.0f, std::max(-v2, v2 - 1.0f));
            v = (e1 <= e2) ? v1 : v2;
          }
          if (v < -0.01f || v > 1.01f) {
            continue;
          }
          v = std::clamp(v, 0.0f, 1.0f);

          const float2 dv = a + v * tw;
          float u;
          if (std::abs(dv.x) > std::abs(dv.y)) {
            u = (std::abs(dv.x) > 1e-8f) ? (d.x - v * b.x) / dv.x : 0.5f;
          }
          else {
            u = (std::abs(dv.y) > 1e-8f) ? (d.y - v * b.y) / dv.y : 0.5f;
          }
          if (u < -0.01f || u > 1.01f) {
            continue;
          }
          u = std::clamp(u, 0.0f, 1.0f);

          const float2 pnt = (1 - u) * (1 - v) * P00 + u * (1 - v) * P10 + (1 - u) * v * P01 +
                             u * v * P11;
          const float dsq = math::distance_squared(query, pnt);

          const float2 &UV00 = grid_uv[r * cols + c];
          const float2 &UV10 = grid_uv[r * cols + c + 1];
          const float2 &UV01 = grid_uv[(r + 1) * cols + c];
          const float2 &UV11 = grid_uv[(r + 1) * cols + c + 1];
          const float2 cand_uv = (1 - u) * (1 - v) * UV00 + u * (1 - v) * UV10 +
                                 (1 - u) * v * UV01 + u * v * UV11;

          const int li = py * res + px;
          const int existing_row = r_lut.row[li];
          /* Rows iterate low to high, so a recorded row is never ahead of `r`; "within 2 rows"
           * therefore means "the same stretch of the curve as this candidate". */
          auto same_stretch = [r](const int other) { return other >= 0 && r - other <= 2; };
          auto store_primary = [&]() {
            r_lut.dist_sq[li] = dsq;
            r_lut.row[li] = r;
            r_lut.uv[li] = cand_uv;
          };
          auto store_secondary = [&]() {
            r_lut.dist_sq2[li] = dsq;
            r_lut.row2[li] = r;
            r_lut.uv2[li] = cand_uv;
          };

          if (existing_row < 0) {
            store_primary();
          }
          else if (same_stretch(existing_row)) {
            /* Same stretch as the primary: the better bilinear fit wins. */
            if (dsq < r_lut.dist_sq[li]) {
              store_primary();
            }
          }
          else {
            /* Two DISTANT stretches of a self-approaching curve competing for the same pixel --
             * where the curve doubles back on itself and the two ribbons overlap in projection.
             *
             * Roll arbitrates this by age (keep the older row) because its LUT only spans a few
             * radii around the current dab, so far-apart rows barely ever compete and temporal
             * stability of the live stroke matters more. Curve Patch's LUT spans the WHOLE curve,
             * which makes age an arbitrary winner: the earlier leg's outer EDGE (|u| ~ 1) would
             * claim pixels that the later leg covers through its CENTER (|u| ~ 0), report a
             * near-edge across-strip coordinate for them, and so drive `BKE_brush_curve_strength()`
             * to ~0 -- punching a relief-free hole into the strip that should have been fully
             * covered, bounded by a staircase along the LUT's own pixel grid.
             *
             * Rank the two legs by which covers the pixel more centrally instead. |u| varies
             * smoothly across the ribbon, so the ranking (and hence the boundary where it flips)
             * varies smoothly too, rather than following a pixel-quantized age boundary, and
             * neither leg can steal pixels it only barely reaches.
             *
             * Near-ties need hysteresis: on a thick closed loop opposite legs meet with almost
             * equal |u|, and a bare `<` flip-flops primary per LUT pixel -- neighboring mesh
             * vertices then sample wildly different `s` and the relief explodes into spikes.
             * Require a clear centrality win; when still tied, prefer the smaller arc length as a
             * stable deterministic break. The loser is kept as SECONDARY so the relief can still
             * merge both legs by max |height|. */
            constexpr float u_hysteresis = 0.05f;
            auto beats = [&](const float2 &incumbent_uv, const float incumbent_dsq) {
              const float cand_abs_u = std::abs(cand_uv.x);
              const float inc_abs_u = std::abs(incumbent_uv.x);
              if (cand_abs_u < inc_abs_u - u_hysteresis) {
                return true;
              }
              if (std::abs(cand_abs_u - inc_abs_u) <= u_hysteresis) {
                return cand_uv.y < incumbent_uv.y ||
                       (cand_uv.y == incumbent_uv.y && dsq < incumbent_dsq);
              }
              return false;
            };
            if (beats(r_lut.uv[li], r_lut.dist_sq[li])) {
              /* Candidate is the more central leg: demote the incumbent, promote the candidate. */
              r_lut.dist_sq2[li] = r_lut.dist_sq[li];
              r_lut.row2[li] = r_lut.row[li];
              r_lut.uv2[li] = r_lut.uv[li];
              store_primary();
            }
            else if (r_lut.row2[li] < 0) {
              store_secondary();
            }
            else if (same_stretch(r_lut.row2[li])) {
              if (dsq < r_lut.dist_sq2[li]) {
                store_secondary();
              }
            }
            else if (beats(r_lut.uv2[li], r_lut.dist_sq2[li])) {
              /* A third leg reaches this pixel: keep the two most central ones. */
              store_secondary();
            }
          }
        }
      }
    }
  }

  r_lut.bb_min = bb_min;
  r_lut.inv_extent = inv_ext;
  r_lut.axis_x = axis_x;
  r_lut.axis_y = axis_y;
  r_lut.v_threshold = 0.5f * brush_radius;
  r_lut.ready = true;
}

int CurvePatchRibbonLut::sample(const float3 &co, float2 r_uv[2]) const
{
  if (!ready) {
    return 0;
  }
  const int RES = res;
  const float2 query = float2(math::dot(co, axis_x), math::dot(co, axis_y));
  const float2 fc = (query - bb_min) * inv_extent;
  if (UNLIKELY(!std::isfinite(fc.x) || !std::isfinite(fc.y) || fc.x < -0.5f ||
               fc.x > float(RES) - 0.5f || fc.y < -0.5f || fc.y > float(RES) - 0.5f))
  {
    return 0;
  }

  const float fx = std::clamp(fc.x - 0.5f, 0.0f, float(RES - 2));
  const float fy = std::clamp(fc.y - 0.5f, 0.0f, float(RES - 2));
  const int ix = std::min(int(fx), RES - 2);
  const int iy = std::min(int(fy), RES - 2);
  const float tx = fx - float(ix);
  const float ty = fy - float(iy);

  const int pixels[4] = {
      iy * RES + ix, iy * RES + ix + 1, (iy + 1) * RES + ix, (iy + 1) * RES + ix + 1};
  const float pixel_weights[4] = {
      (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};

  /* Collect every stretch of the curve recorded across the 2x2 neighborhood: both slots of all
   * four pixels. Which slot a given stretch occupies is NOT consistent between pixels -- the
   * primary/secondary ranking flips across the two stretches' medial axis -- so the slots are
   * pooled here and re-grouped by arc length below rather than being read positionally. */
  constexpr float INVALID = FLT_MAX * 0.5f;
  struct Candidate {
    float2 uv;
    float weight;
    float dist_sq;
  };
  Candidate candidates[8];
  int candidate_num = 0;
  for (int k = 0; k < 4; k++) {
    if (uv[pixels[k]].x < INVALID) {
      candidates[candidate_num++] = {uv[pixels[k]], pixel_weights[k], dist_sq[pixels[k]]};
    }
    if (uv2[pixels[k]].x < INVALID) {
      candidates[candidate_num++] = {uv2[pixels[k]], pixel_weights[k], dist_sq2[pixels[k]]};
    }
  }
  if (UNLIKELY(candidate_num == 0)) {
    /* Nothing rasterized anywhere nearby: the query is off the ribbon. */
    return 0;
  }

  /* Group the candidates into stretches by arc length, most confidently-fit stretch first.
   *
   * Within one group the candidates are averaged with their bilinear weights (renormalized), which
   * keeps the mapping smooth and continuous inside each stretch. Across groups nothing is blended:
   * averaging two stretches' arc lengths would fabricate a curve position lying on neither. This
   * replaces Roll's nearest-neighbor fallback, which snapped the whole neighborhood to one pixel
   * and so quantized the result to the LUT grid -- visible as a staircase along any boundary that
   * runs across many pixels. */
  bool grouped[8] = {};
  int branch_num = 0;
  for (int attempt = 0; attempt < 4 && branch_num < 2; attempt++) {
    int anchor = -1;
    for (int k = 0; k < candidate_num; k++) {
      if (!grouped[k] && (anchor < 0 || candidates[k].dist_sq < candidates[anchor].dist_sq)) {
        anchor = k;
      }
    }
    if (anchor < 0) {
      break;
    }
    float2 accum(0.0f);
    float weight_sum = 0.0f;
    for (int k = 0; k < candidate_num; k++) {
      if (!grouped[k] && std::abs(candidates[k].uv.y - candidates[anchor].uv.y) <= v_threshold) {
        grouped[k] = true;
        accum += candidates[k].uv * candidates[k].weight;
        weight_sum += candidates[k].weight;
      }
    }
    /* A group can carry zero total weight when it only occupies pixels the query sits exactly on
     * the far edge of. It contributes nothing, so skip it and let the next attempt find the next
     * stretch rather than spending one of the two output slots on it. */
    if (weight_sum > 1e-6f) {
      r_uv[branch_num] = accum / weight_sum;
      branch_num++;
    }
  }
  return branch_num;
}

/** \} */

}  // namespace blender::bke
