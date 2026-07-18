/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Ribbon -> LUT parameterization for Curve Patch. Geometry stages adapted from the Roll stroke
 * method's per-dab implementation (`paint_stroke_roll.cc`) -- see the header for what differs and
 * why the module keeps its own copy instead of sharing Roll's dab-scoped code paths.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "paint_curve_patch_ribbon.hh"
#include "paint_curve_patch_spline.hh"

#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"

namespace blender::ed::sculpt_paint {

void CurvePatchRibbonLut::clear()
{
  res = 0;
  uv.clear();
  dist_sq.clear();
  row.clear();
  bb_min = float2(0.0f);
  inv_extent = float2(0.0f);
  axis_x = float3(0.0f);
  axis_y = float3(0.0f);
  v_threshold = 0.0f;
  ready = false;
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
 */
static void ribbon_fix_border_self_intersections(const Span<float3> poly,
                                                 const float3 &axis_x,
                                                 const float3 &axis_y,
                                                 const float R,
                                                 Vector<float3> &border)
{
  const int count = int(poly.size());
  if (count < 4) {
    return;
  }

  Vector<float2> b2d(count);
  for (int k = 0; k < count; k++) {
    b2d[k] = float2(math::dot(border[k], axis_x), math::dot(border[k], axis_y));
  }

  const int max_loop = count - 1;
  Vector<float3> orig(border);
  int skip_until = -1;

  for (int i = 0; i < count - 1; i++) {
    if (i <= skip_until) {
      continue;
    }
    const float2 d1 = b2d[i + 1] - b2d[i];
    if (math::dot(d1, d1) < 1e-8f) {
      continue;
    }

    const int j_max = std::min(count - 2, i + max_loop);
    for (int j = j_max; j >= i + 2; j--) {
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
 * First Catmull-Clark pass with all ORIGINAL vertices pinned except the center column: the
 * borders keep the exact collapse geometry, only the NEW intermediate positions get CC-averaged
 * and the center column gets the 1D boundary rule so cross-strip segments can curve near merge
 * points. `rows x 3` -> `(2*rows-1) x 5`.
 */
static void ribbon_subdivide_pinned(Vector<float3> &grid_pos,
                                    Vector<float2> &grid_uv,
                                    int &rows,
                                    int &cols)
{
  const int oR = rows;
  const int oC = cols; /* 3 */
  const int nR = 2 * oR - 1;
  const int nC = 2 * oC - 1; /* 5 */
  const int fR = oR - 1;
  const int fC = oC - 1; /* 2 */

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
   * points. Boundary (first/last row): simple midpoint. */
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

  /* Vertical edge points (odd row, even col): simple midpoints everywhere (center vertical edges
   * stay on the curve polyline; borders keep the collapse geometry). */
  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < oC; c++) {
      np[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_pos[oi(r, c)] + grid_pos[oi(r + 1, c)]);
      nu[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_uv[oi(r, c)] + grid_uv[oi(r + 1, c)]);
    }
  }

  /* Original vertex points (even row, even col). Center column interior: CC boundary rule (1D
   * smoothing along the curve) so cross-strip segments can curve near merge points. Everything
   * else: pinned. */
  for (int r = 0; r < oR; r++) {
    for (int c = 0; c < oC; c++) {
      if (c == 1 && r > 0 && r < oR - 1) {
        np[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_pos[oi(r - 1, c)] +
                                                6.0f * grid_pos[oi(r, c)] + grid_pos[oi(r + 1, c)]);
        nu[ni(2 * r, 2 * c)] = (1.0f / 8.0f) *
                               (grid_uv[oi(r - 1, c)] + 6.0f * grid_uv[oi(r, c)] +
                                grid_uv[oi(r + 1, c)]);
      }
      else {
        np[ni(2 * r, 2 * c)] = grid_pos[oi(r, c)];
        nu[ni(2 * r, 2 * c)] = grid_uv[oi(r, c)];
      }
    }
  }

  grid_pos = std::move(np);
  grid_uv = std::move(nu);
  rows = nR;
  cols = nC;
}

/**
 * Standard Catmull-Clark subdivision on a `rows x cols` quad grid. Produces
 * `(2*rows-1) x (2*cols-1)` output. Border columns are pinned to preserve the self-intersection
 * collapse geometry; all other vertices use the standard CC rules.
 */
static void ribbon_subdivide_standard(Vector<float3> &grid_pos,
                                      Vector<float2> &grid_uv,
                                      int &rows,
                                      int &cols)
{
  const int oR = rows, oC = cols;
  const int nR = 2 * oR - 1;
  const int nC = 2 * oC - 1;
  const int fR = oR - 1;
  const int fC = oC - 1;

  auto oi = [oC](int r, int c) { return r * oC + c; };
  auto ni = [nC](int r, int c) { return r * nC + c; };

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

  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < oC; c++) {
      if (c == 0 || c == oC - 1) {
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

  for (int r = 0; r < oR; r++) {
    for (int c = 0; c < oC; c++) {
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
  constexpr int smooth_iters = 10;
  constexpr float mix = 0.5f;
  const int grid_total = rows * cols;
  Vector<float3> buf_p(grid_total);
  Vector<float2> buf_u(grid_total);
  float3 *src_p = grid_pos.data(), *dst_p = buf_p.data();
  float2 *src_u = grid_uv.data(), *dst_u = buf_u.data();
  for (int iter = 0; iter < smooth_iters; iter++) {
    memcpy(dst_p, src_p, sizeof(float3) * grid_total);
    memcpy(dst_u, src_u, sizeof(float2) * grid_total);
    for (int r = 1; r < rows - 1; r++) {
      for (int c = 1; c < cols - 1; c++) {
        const int idx = r * cols + c;
        const float3 avg_p = 0.25f * (src_p[(r - 1) * cols + c] + src_p[(r + 1) * cols + c] +
                                      src_p[r * cols + c - 1] + src_p[r * cols + c + 1]);
        const float2 avg_u = 0.25f * (src_u[(r - 1) * cols + c] + src_u[(r + 1) * cols + c] +
                                      src_u[r * cols + c - 1] + src_u[r * cols + c + 1]);
        dst_p[idx] = (1.0f - mix) * src_p[idx] + mix * avg_p;
        dst_u[idx] = (1.0f - mix) * src_u[idx] + mix * avg_u;
      }
    }
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

void curve_patch_ribbon_build(const CurvePatchSpline &spline,
                              const float brush_radius,
                              CurvePatchRibbonLut &r_lut)
{
  r_lut.clear();
  if (spline.is_empty() || brush_radius <= 0.0f) {
    return;
  }

  const Span<float3> poly = spline.poly_3d.as_span();
  const Span<float3> tangents = spline.tangents_3d.as_span();
  const int count = int(poly.size());
  const bool have_radii = spline.radii.size() == poly.size();
  const float3 plane_normal = math::normalize(spline.plane_normal);

  /* Per-vertex world half-width; `R_max` drives the collapse validation and the LUT resolution. */
  float R_max = 0.0f;
  Vector<float> halfwidths(count);
  for (int i = 0; i < count; i++) {
    const float w = have_radii ? std::max(spline.radii[i] * brush_radius, brush_radius * 0.01f) :
                                 brush_radius;
    halfwidths[i] = w;
    R_max = std::max(R_max, w);
  }
  if (R_max <= 0.0f) {
    return;
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
  for (int i = 0; i < count; i++) {
    const float3 &T = tangents[i];
    float3 B = math::cross(T, plane_normal);
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
    border_left[i] = poly[i] + B * halfwidths[i];
    border_right[i] = poly[i] - B * halfwidths[i];
  }

  ribbon_fix_border_self_intersections(poly, axis_x, axis_y, R_max, border_left);
  ribbon_fix_border_self_intersections(poly, axis_x, axis_y, R_max, border_right);

  /* Seed grid: one row per polyline vertex, columns = {right, center, left}. V = raw arc
   * length. */
  int rows = count;
  int cols = 3;
  Vector<float3> grid_pos(rows * cols);
  Vector<float2> grid_uv(rows * cols);
  for (int k = 0; k < count; k++) {
    const float v = spline.lengths_3d[k];
    grid_pos[k * 3 + 0] = border_right[k];
    grid_pos[k * 3 + 1] = poly[k];
    grid_pos[k * 3 + 2] = border_left[k];
    grid_uv[k * 3 + 0] = float2(-1.0f, v);
    grid_uv[k * 3 + 1] = float2(0.0f, v);
    grid_uv[k * 3 + 2] = float2(1.0f, v);
  }

  ribbon_subdivide_pinned(grid_pos, grid_uv, rows, cols);
  ribbon_subdivide_standard(grid_pos, grid_uv, rows, cols);
  ribbon_laplacian_smooth(grid_pos, grid_uv, rows, cols);

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
   * than ~15% of the largest half-width so the across-strip coordinate keeps sub-strip precision
   * on long thin curves, clamped to keep the per-restamp fill cost bounded. */
  const float max_extent = std::max(ext.x, ext.y);
  const int res = std::clamp(int(max_extent / (0.15f * R_max)) + 1, 128, 512);
  const float2 inv_ext(float(res) / ext.x, float(res) / ext.y);

  const int lut_total = res * res;
  r_lut.res = res;
  r_lut.uv.reinitialize(lut_total);
  r_lut.dist_sq.reinitialize(lut_total);
  r_lut.row.reinitialize(lut_total);
  for (int i = 0; i < lut_total; i++) {
    r_lut.uv[i] = float2(FLT_MAX, 0.0f);
    r_lut.dist_sq[i] = FLT_MAX;
    r_lut.row[i] = -1;
  }

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
          bool write;
          if (existing_row < 0) {
            write = true;
          }
          else if (r - existing_row <= 2) {
            /* Same stretch of the curve (rows iterate low to high, so the existing row is never
             * ahead of `r`): the better bilinear fit wins. */
            write = (dsq < r_lut.dist_sq[li]);
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
             * Prefer the leg that covers the pixel more centrally instead. |u| varies smoothly
             * across the ribbon, so the resulting branch boundary is a smooth curve (the two legs'
             * medial axis) rather than a pixel-quantized age boundary, and neither leg can steal
             * pixels it only barely reaches. */
            write = std::abs(cand_uv.x) < std::abs(r_lut.uv[li].x);
          }
          if (write) {
            r_lut.dist_sq[li] = dsq;
            r_lut.row[li] = r;
            r_lut.uv[li] = cand_uv;
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

bool CurvePatchRibbonLut::sample(const float3 &co, float2 &r_uv) const
{
  if (!ready) {
    return false;
  }
  const int RES = res;
  const float2 query = float2(math::dot(co, axis_x), math::dot(co, axis_y));
  const float2 fc = (query - bb_min) * inv_extent;
  if (UNLIKELY(!std::isfinite(fc.x) || !std::isfinite(fc.y) || fc.x < -0.5f ||
               fc.x > float(RES) - 0.5f || fc.y < -0.5f || fc.y > float(RES) - 0.5f))
  {
    return false;
  }

  const float fx = std::clamp(fc.x - 0.5f, 0.0f, float(RES - 2));
  const float fy = std::clamp(fc.y - 0.5f, 0.0f, float(RES - 2));
  const int ix = std::min(int(fx), RES - 2);
  const int iy = std::min(int(fy), RES - 2);
  const float tx = fx - float(ix);
  const float ty = fy - float(iy);

  float2 uv00 = uv[iy * RES + ix];
  float2 uv10 = uv[iy * RES + ix + 1];
  float2 uv01 = uv[(iy + 1) * RES + ix];
  float2 uv11 = uv[(iy + 1) * RES + ix + 1];

  /* Fill never-rasterized pixels from the nearest valid neighbor so the bilinear sample does not
   * blend against garbage; reject outright when all four are empty (query is off the ribbon). */
  constexpr float INVALID = FLT_MAX * 0.5f;
  const bool b00 = uv00.x >= INVALID;
  const bool b10 = uv10.x >= INVALID;
  const bool b01 = uv01.x >= INVALID;
  const bool b11 = uv11.x >= INVALID;
  if (UNLIKELY(b00 || b10 || b01 || b11)) {
    if (b00 && b10 && b01 && b11) {
      return false;
    }
    const float2 fill_uv = !b00 ? uv00 : !b10 ? uv10 : !b01 ? uv01 : uv11;
    if (b00) {
      uv00 = fill_uv;
    }
    if (b10) {
      uv10 = fill_uv;
    }
    if (b01) {
      uv01 = fill_uv;
    }
    if (b11) {
      uv11 = fill_uv;
    }
  }

  const float2 uvs[4] = {uv00, uv10, uv01, uv11};
  const float weights[4] = {
      (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};

  /* Overlap discontinuity: where the curve doubles back, the 4 sampled pixels can straddle the
   * boundary between the two legs, and blending V across it would fabricate a curve position that
   * lies on neither leg. */
  const float v_min = std::min({uv00.y, uv10.y, uv01.y, uv11.y});
  const float v_max = std::max({uv00.y, uv10.y, uv01.y, uv11.y});
  if (UNLIKELY(v_max - v_min > v_threshold)) {
    /* Anchor on the best-fitting pixel, then interpolate only across the neighbors that agree with
     * it -- the ones belonging to the same leg -- renormalizing their bilinear weights.
     *
     * Deliberately not a plain nearest-neighbor pick (which is what Roll does here): snapping the
     * whole 2x2 neighborhood to one pixel quantizes the result to the LUT grid, and along a
     * boundary that runs across many pixels that reads as a staircase in the relief. Averaging the
     * agreeing subset keeps the mapping continuous within each leg while still refusing to blend
     * ACROSS legs. */
    const float ds[4] = {dist_sq[iy * RES + ix],
                         dist_sq[iy * RES + ix + 1],
                         dist_sq[(iy + 1) * RES + ix],
                         dist_sq[(iy + 1) * RES + ix + 1]};
    int anchor = 0;
    for (int k = 1; k < 4; k++) {
      if (ds[k] < ds[anchor]) {
        anchor = k;
      }
    }
    float2 accum(0.0f);
    float weight_sum = 0.0f;
    for (int k = 0; k < 4; k++) {
      if (std::abs(uvs[k].y - uvs[anchor].y) <= v_threshold) {
        accum += uvs[k] * weights[k];
        weight_sum += weights[k];
      }
    }
    r_uv = weight_sum > 1e-6f ? accum / weight_sum : uvs[anchor];
    return true;
  }

  r_uv = uvs[0] * weights[0] + uvs[1] * weights[1] + uvs[2] * weights[2] + uvs[3] * weights[3];
  return true;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
