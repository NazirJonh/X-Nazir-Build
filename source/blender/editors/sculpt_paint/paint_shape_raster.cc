/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shape SDF rasterizer; see #paint_shape_raster.hh.
 *
 * Spline shapes are flattened into dense polylines and turned into a segment list with an
 * acceleration grid (#BUCKET_SIZE pixel cells). Per pixel, the distance to the nearest segment
 * drives the stroke coverage (with alignment shift, variable width, caps and dashes) and the
 * fill coverage; the inside/outside sign of closed loops comes from a per-row winding sweep,
 * which keeps holes (opposite-wound contours) working. Parametric rectangles and ellipses use
 * their analytic SDF directly.
 */

#include "paint_shape_raster.hh"

#include <algorithm>
#include <cmath>
#include <utility>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

namespace blender::ed::sculpt_paint::shape {

/** Acceleration grid cell size, in pixels. */
constexpr int BUCKET_SIZE = 32;
/** Anti-aliased transition width of dash ends and butt/square caps, in pixels. */
constexpr float PATH_AA = 1.0f;

/** Miter joins longer than this multiple of the half width fall back to a bevel (the SVG miter
 * limit), so a very sharp corner does not shoot a spike far past the outline. */
constexpr float MITER_LIMIT = 4.0f;
/** Distances below this are treated as exactly on the outline. */
constexpr float ON_EDGE_EPSILON = 1e-9f;

/** Anti-aliased coverage of a region \a dist_inside pixels deep, over a transition band \a aa. */
static float aa_coverage(const float dist_inside, const float aa)
{
  if (aa <= 1e-5f) {
    return dist_inside >= 0.0f ? 1.0f : 0.0f;
  }
  return math::clamp(dist_inside / aa + 0.5f, 0.0f, 1.0f);
}

/** Rounded box SDF with four different corner radii (TL, TR, BR, BL), exact. */
static float sd_rounded_box(const float2 &p, const float2 &half, const float4 radius)
{
  float r;
  if (p.x >= 0.0f) {
    r = (p.y >= 0.0f) ? radius.y : radius.z; /* TR : BR */
  }
  else {
    r = (p.y >= 0.0f) ? radius.x : radius.w; /* TL : BL */
  }
  r = std::min(r, std::min(half.x, half.y));
  const float2 q = math::abs(p) - half + float2(r);
  /* `- r` is what makes the zero level the rounded box of half-size `half`: without it the whole
   * shape shrinks by `r` and the straight sides move inward as the radius grows. */
  return std::min(std::max(q.x, q.y), 0.0f) + math::length(math::max(q, float2(0.0f))) - r;
}

/**
 * Ellipse SDF: the zero level is exact, offsets use the first-order gradient approximation
 * (smooth and, for paint-sized ellipses, accurate to a fraction of a pixel).
 */
static float sd_ellipse(const float2 &p, const float2 &ab)
{
  if (ab.x <= 0.0f || ab.y <= 0.0f) {
    return 1e9f;
  }
  const float2 q = p / ab;
  const float f = math::length(q);
  if (f < 1e-6f) {
    return -std::min(ab.x, ab.y);
  }
  const float2 grad = q / (ab * f);
  return (f - 1.0f) / math::length(grad);
}

/** \a p transformed into the parametric shape's local (unrotated, centered) frame. */
static float2 parametric_local(const PaintShape &shape, const float2 &p)
{
  const float2 rel = p - shape.center;
  const float c = math::cos(-shape.rotation);
  const float s = math::sin(-shape.rotation);
  return float2(rel.x * c - rel.y * s, rel.x * s + rel.y * c);
}

/** Signed distance of \a p to a parametric shape (negative inside). */
static float sd_parametric(const PaintShape &shape, const float2 &p)
{
  const float2 local = parametric_local(shape, p);
  if (shape.type == PAINT_SHAPE_RECT) {
    return sd_rounded_box(local, shape.half_size, shape.corner_radius);
  }
  return sd_ellipse(local, shape.half_size);
}

/* -------------------------------------------------------------------- */
/** \name Per-shape geometry
 * \{ */

struct PolyInfo {
  /** Total arc length of the polyline. */
  float length = 0.0f;
  bool cyclic = false;
};

struct NearestHit {
  float dist = FLT_MAX;
  int seg = -1;
  /** Parameter along the segment, 0..1. */
  float t = 0.0f;
  float2 closest = float2(0.0f);
};

struct ShapeGeom {
  /** The shape evaluates its analytic SDF and skips the segment machinery (the parametric
   * Rect/Ellipse). */
  bool is_parametric = false;
  PaintShape parametric_shape;
  /** #ShapeRasterOutputs::ShapeUV comes from the parametric frame (rotated, normalized by
   * half_size) rather than the geometry bbox. True whenever the source shape is an analytic-SDF
   * Rect/Ellipse, even after it was densified for #ShapeRasterOutputs::StrokeS. */
  bool uv_from_parametric = false;
  /** Geometry bbox used for #ShapeRasterOutputs::ShapeUV of spline shapes. */
  float2 uv_min = float2(0.0f);
  float2 uv_max = float2(0.0f);

  /* Dense segments of every spline, with the width factor interpolated at the endpoints and the
   * arc length at the segment start (within its polyline). */
  Vector<float2> seg_a;
  Vector<float2> seg_b;
  Vector<float> seg_wf0;
  Vector<float> seg_wf1;
  Vector<float> seg_s0;
  Vector<float> seg_len;
  Vector<int> seg_poly;
  /* Neighbors of each segment along its polyline (-1 at an open end); the corner between a segment
   * and its next one is where a Miter / Bevel join is built. */
  Vector<int> seg_prev;
  Vector<int> seg_next;
  Vector<PolyInfo> polys;
  /* Segments of cyclic polylines only: these drive the fill and the winding sweep. */
  Vector<int> closed_segs;

  bool has_closed = false;
  /** Evaluation margin: distances beyond it use the saturating far path. */
  float margin = 0.0f;

  /* Segment acceleration grid over the raster region, in shape-space pixels. */
  float2 grid_origin = float2(0.0f);
  int grid_w = 0;
  int grid_h = 0;
  Array<Vector<int>> cells;

  /* Shape-space bounds, expanded by the evaluation margin; pixels outside skip evaluation. */
  rctf bounds;
};

static ShapeGeom shape_geometry_build(const PaintShape &shape,
                                       const ShapeStyle &style,
                                       const rctf &domain,
                                       const ShapeRasterOutputs outputs)
{
  ShapeGeom geom;

  /* The analytic SDF carries no arc length, so StrokeS forces a parametric Rect/Ellipse through
   * the flattened-polyline path (StrokeS is opt-in, so the coverage stays analytic otherwise). */
  const bool force_polyline =
      shape.has_analytic_sdf() &&
      (uint8_t(outputs) & uint8_t(ShapeRasterOutputs::StrokeS)) != 0;
  if (shape.has_analytic_sdf()) {
    /* ShapeUV keeps using the parametric frame even when StrokeS densifies the coverage. */
    geom.parametric_shape = shape;
    geom.uv_from_parametric = true;
  }

  if (shape.has_analytic_sdf() && !force_polyline) {
    geom.is_parametric = true;
    /* Conservative round bounds around the center. */
    const float margin = style.stroke_width + style.feather + style.fill_profile_width + 2.0f;
    const float radius = math::length(shape.half_size) + margin;
    geom.bounds.xmin = shape.center.x - radius;
    geom.bounds.ymin = shape.center.y - radius;
    geom.bounds.xmax = shape.center.x + radius;
    geom.bounds.ymax = shape.center.y + radius;
    return geom;
  }

  float max_width_factor = 1.0f;
  for (const ShapeSpline &spline : shape.splines) {
    for (const ShapePoint &point : spline.points) {
      max_width_factor = std::max(max_width_factor, math::abs(point.width_factor));
    }
  }
  /* A miter tip reaches up to the miter limit times the half width from its vertex. */
  const float join_scale = style.join_type == PAINT_SHAPE_JOIN_MITER ? MITER_LIMIT : 1.0f;
  const float stroke_extent = style.use_stroke() ?
                                  style.stroke_width * max_width_factor * join_scale +
                                      style.feather + PATH_AA + 2.0f :
                                  0.0f;
  const float fill_extent =
      (style.use_fill() && style.use_profile()) ? style.fill_profile_width + style.feather + 2.0f :
                                                  style.feather + 2.0f;
  geom.margin = std::max(stroke_extent, fill_extent);

  const Vector<ShapePolyline> outlines = shape_flatten(shape, 1.0f);
  geom.polys.reserve(outlines.size());
  for (const ShapePolyline &poly : outlines) {
    PolyInfo info;
    info.length = poly.points.is_empty() ? 0.0f : poly.arc_len.last();
    info.cyclic = poly.cyclic;
    const int poly_index = geom.polys.append_and_get_index(info);
    geom.has_closed = geom.has_closed || poly.cyclic;

    int first_seg = -1;
    int last_seg = -1;
    for (const int i : IndexRange(poly.points.size() - 1)) {
      const float2 a = poly.points[i];
      const float2 b = poly.points[i + 1];
      const float len = math::distance(a, b);
      if (len < 1e-6f) {
        continue;
      }
      const int seg_index = geom.seg_a.size();
      geom.seg_prev.append(last_seg);
      geom.seg_next.append(-1);
      if (last_seg >= 0) {
        geom.seg_next[last_seg] = seg_index;
      }
      else {
        first_seg = seg_index;
      }
      last_seg = seg_index;
      geom.seg_a.append(a);
      geom.seg_b.append(b);
      geom.seg_wf0.append(poly.width[i]);
      geom.seg_wf1.append(poly.width[i + 1]);
      geom.seg_s0.append(poly.arc_len[i]);
      geom.seg_len.append(len);
      geom.seg_poly.append(poly_index);
      if (poly.cyclic) {
        geom.closed_segs.append(geom.seg_a.size() - 1);
      }
    }
    /* A closed outline has no open ends: its last segment joins back to the first. */
    if (poly.cyclic && first_seg >= 0 && last_seg > first_seg) {
      geom.seg_next[last_seg] = first_seg;
      geom.seg_prev[first_seg] = last_seg;
    }
  }

  if (geom.seg_a.is_empty()) {
    geom.bounds.xmin = geom.bounds.ymin = 0.0f;
    geom.bounds.xmax = geom.bounds.ymax = 0.0f;
    return geom;
  }

  BLI_rctf_init_minmax(&geom.bounds);
  for (const int i : geom.seg_a.index_range()) {
    BLI_rctf_do_minmax_v(&geom.bounds, geom.seg_a[i]);
    BLI_rctf_do_minmax_v(&geom.bounds, geom.seg_b[i]);
  }
  if (!geom.uv_from_parametric) {
    /* The raw geometry bbox drives ShapeUV; the margin below is only for evaluation. */
    geom.uv_min = float2(geom.bounds.xmin, geom.bounds.ymin);
    geom.uv_max = float2(geom.bounds.xmax, geom.bounds.ymax);
  }
  geom.bounds.xmin -= geom.margin;
  geom.bounds.ymin -= geom.margin;
  geom.bounds.xmax += geom.margin;
  geom.bounds.ymax += geom.margin;

  /* Acceleration grid over the domain's shape-space extent. */
  geom.grid_origin = float2(domain.xmin, domain.ymin);
  const float domain_w = domain.xmax - domain.xmin;
  const float domain_h = domain.ymax - domain.ymin;
  geom.grid_w = std::max(1, int(domain_w) / BUCKET_SIZE + 1);
  geom.grid_h = std::max(1, int(domain_h) / BUCKET_SIZE + 1);
  geom.cells.reinitialize(geom.grid_w * geom.grid_h);

  const int max_cx = geom.grid_w - 1;
  const int max_cy = geom.grid_h - 1;
  for (const int si : geom.seg_a.index_range()) {
    /* Registration margin: everything that can influence a pixel within the evaluation margin of
     * the segment must find the segment in its own cell. Kept at or above the evaluation margin
     * so "far" pixels never fall into a cell that does hold the nearest segment. */
    const float wf = std::max(math::abs(geom.seg_wf0[si]), math::abs(geom.seg_wf1[si]));
    float reg_margin = std::max(geom.margin,
                                style.stroke_width * wf * join_scale + style.feather + PATH_AA +
                                    1.0f);
    if (style.use_fill() && style.use_profile()) {
      reg_margin = std::max(reg_margin, style.fill_profile_width + 1.0f);
    }
    const float2 &a = geom.seg_a[si];
    const float2 &b = geom.seg_b[si];
    const float2 lo = math::min(a, b) - float2(reg_margin);
    const float2 hi = math::max(a, b) + float2(reg_margin);
    if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(hi.x) ||
        !std::isfinite(hi.y))
    {
      continue;
    }
    /* Clamp in float before the int conversion: far-off symmetry copies can exceed the int
     * range, and an out-of-range conversion yields INT_MIN, inverting the cell range. */
    auto cell_index = [](const float co, const float origin, const int max_index) {
      return int(std::clamp((co - origin) / float(BUCKET_SIZE), 0.0f, float(max_index)));
    };
    const int cx0 = cell_index(lo.x, geom.grid_origin.x, max_cx);
    const int cy0 = cell_index(lo.y, geom.grid_origin.y, max_cy);
    const int cx1 = cell_index(hi.x, geom.grid_origin.x, max_cx);
    const int cy1 = cell_index(hi.y, geom.grid_origin.y, max_cy);
    for (const int cy : IndexRange(cy0, cy1 - cy0 + 1)) {
      for (const int cx : IndexRange(cx0, cx1 - cx0 + 1)) {
        geom.cells[cy * geom.grid_w + cx].append(si);
      }
    }
  }

  return geom;
}

/** Nearest segment to \a p within its grid cell. Exact for pixels within the registration
 * margin of a segment -- which is all the rasterizer ever evaluates closely. */
static void nearest_segment_lookup(const ShapeGeom &geom, const float2 &p, NearestHit &r_hit)
{
  r_hit.seg = -1;
  r_hit.dist = FLT_MAX;
  if (geom.grid_w <= 0 || geom.grid_h <= 0) {
    return;
  }
  const int cx = std::clamp(int((p.x - geom.grid_origin.x) / BUCKET_SIZE), 0, geom.grid_w - 1);
  const int cy = std::clamp(int((p.y - geom.grid_origin.y) / BUCKET_SIZE), 0, geom.grid_h - 1);
  const Vector<int> &cell = geom.cells[cy * geom.grid_w + cx];
  if (cell.is_empty()) {
    return;
  }
  for (const int si : cell) {
    const float2 a = geom.seg_a[si];
    const float2 b = geom.seg_b[si];
    const float2 ab = b - a;
    const float len_sq = math::length_squared(ab);
    float t = 0.0f;
    if (len_sq > 1e-12f) {
      t = math::clamp(math::dot(p - a, ab) / len_sq, 0.0f, 1.0f);
    }
    const float2 closest = a + ab * t;
    const float dist_sq = math::length_squared(p - closest);
    if (dist_sq < r_hit.dist) {
      r_hit.dist = dist_sq;
      r_hit.seg = si;
      r_hit.t = t;
      r_hit.closest = closest;
    }
  }
  r_hit.dist = math::sqrt(r_hit.dist);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke joins
 * \{ */

/** Signed distance to a convex polygon of either winding (negative inside). */
static float sd_convex_polygon(const Span<float2> points, const float2 &p)
{
  float dist_sq = FLT_MAX;
  bool any_negative = false;
  bool any_positive = false;
  for (const int i : points.index_range()) {
    const float2 a = points[i];
    const float2 ab = points[(i + 1) % points.size()] - a;
    const float len_sq = math::length_squared(ab);
    const float t = len_sq > 1e-12f ? math::clamp(math::dot(p - a, ab) / len_sq, 0.0f, 1.0f) :
                                      0.0f;
    dist_sq = std::min(dist_sq, math::length_squared(p - (a + ab * t)));
    const float side = ab.x * (p.y - a.y) - ab.y * (p.x - a.x);
    any_negative = any_negative || side < 0.0f;
    any_positive = any_positive || side > 0.0f;
  }
  const float dist = math::sqrt(dist_sq);
  return (any_negative && any_positive) ? dist : -dist;
}

/**
 * Signed distance to the stroke of the segments around \a p, built from butt-ended segment
 * rectangles plus a corner wedge at every polyline vertex, instead of the round-everywhere distance
 * to the nearest segment. The wedge is the join: a bevel triangle, or the miter quad up to the
 * miter limit. Open ends get the cap shape (round adds a circle, square extends the rectangle).
 */
static float stroke_join_sd(const ShapeGeom &geom,
                            const ShapeStyle &style,
                            const float2 &p,
                            const float halfw)
{
  const int cx = std::clamp(int((p.x - geom.grid_origin.x) / BUCKET_SIZE), 0, geom.grid_w - 1);
  const int cy = std::clamp(int((p.y - geom.grid_origin.y) / BUCKET_SIZE), 0, geom.grid_h - 1);
  float sd = FLT_MAX;
  for (const int si : geom.cells[cy * geom.grid_w + cx]) {
    const float2 a = geom.seg_a[si];
    const float2 b = geom.seg_b[si];
    const float len = geom.seg_len[si];
    const float2 dir = (b - a) / len;
    const float2 normal(-dir.y, dir.x);
    const bool has_prev = geom.seg_prev[si] >= 0;
    const int next = geom.seg_next[si];

    /* A joined end overlaps the corner wedge a little: the rectangle and the wedge share their edge,
     * where both distances are zero, so the union of their anti-aliased edges would leave a
     * half-covered seam. The overshoot lies inside the wedge or the neighboring segment. */
    const float overlap = PATH_AA + style.feather + 0.5f;
    const float ext0 = has_prev ? overlap :
                                  (style.cap_type == PAINT_SHAPE_CAP_SQUARE ? halfw : 0.0f);
    const float ext1 = next >= 0 ? overlap :
                                   (style.cap_type == PAINT_SHAPE_CAP_SQUARE ? halfw : 0.0f);
    const float u = math::dot(p - a, dir);
    const float v = math::dot(p - a, normal);
    const float dx = std::max(-u - ext0, u - len - ext1);
    const float dy = math::abs(v) - halfw;
    sd = std::min(sd,
                  math::length(float2(std::max(dx, 0.0f), std::max(dy, 0.0f))) +
                      std::min(std::max(dx, dy), 0.0f));

    if (style.cap_type == PAINT_SHAPE_CAP_ROUND) {
      if (!has_prev) {
        sd = std::min(sd, math::distance(p, a) - halfw);
      }
      if (next < 0) {
        sd = std::min(sd, math::distance(p, b) - halfw);
      }
    }
    if (next < 0) {
      continue;
    }

    /* The corner at b, on the outer side of the turn. */
    const float2 dir1 = (geom.seg_b[next] - geom.seg_a[next]) / geom.seg_len[next];
    const float turn = dir.x * dir1.y - dir.y * dir1.x;
    if (math::abs(turn) < 1e-4f && math::dot(dir, dir1) > 0.0f) {
      continue;
    }
    const float side = turn > 0.0f ? 1.0f : -1.0f;
    const float2 n0 = float2(dir.y, -dir.x) * side;
    const float2 n1 = float2(dir1.y, -dir1.x) * side;
    const float2 o0 = b + n0 * halfw;
    const float2 o1 = b + n1 * halfw;
    float2 wedge[4] = {b, o0, o1, o1};
    int wedge_num = 3;
    if (style.join_type == PAINT_SHAPE_JOIN_MITER) {
      float2 mid = n0 + n1;
      const float mid_len = math::length(mid);
      if (mid_len > 1e-4f) {
        mid /= mid_len;
        const float cos_half = math::dot(mid, n0);
        if (cos_half > 1.0f / MITER_LIMIT) {
          wedge[2] = b + mid * (halfw / cos_half);
          wedge[3] = o1;
          wedge_num = 4;
        }
      }
    }
    sd = std::min(sd, sd_convex_polygon(Span<float2>(wedge, wedge_num), p));
  }
  return sd;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pixel evaluation
 * \{ */

struct PixelCoverage {
  float fill = 0.0f;
  float stroke = 0.0f;
  float stroke_t = 0.0f;
  float fill_d = 0.0f;
  float2 stroke_dir = float2(0.0f);
  /** Unit gradient of #fill_d, pointing inward (toward the fill interior). */
  float2 fill_dir = float2(0.0f);
  float stroke_s = 0.0f;
  float stroke_len = 0.0f;
  float2 shape_uv = float2(0.0f);
};

/**
 * Evaluate one shape at \a p.
 *
 * \param inside: winding at \a p (tracked by the caller's per-row sweep), only read for shapes
 * with closed loops.
 * \param want: which quantities to compute (decoded from the #ShapeRasterOutputs mask once, by
 * the caller); unrequested distance/direction fields stay zero, and the base coverage they
 * derive from needs its own flag (#Fill for #FillD/#FillDir, #Stroke for #StrokeT/#StrokeDir).
 */
static void shape_eval_pixel(const ShapeGeom &geom,
                             const ShapeStyle &style,
                             const float2 &p,
                             const bool inside,
                             PixelCoverage &r_cov,
                             const ShapeRasterWants &want)
{
  r_cov = {};

  const bool use_fill = want.fill && style.use_fill() && (geom.is_parametric || geom.has_closed);
  const bool use_stroke = want.stroke && style.use_stroke() && style.stroke_width > 0.0f;
  if ((!use_fill && !use_stroke && !want.shape_uv) || !BLI_rctf_isect_pt_v(&geom.bounds, p)) {
    return;
  }

  if (want.shape_uv) {
    if (geom.uv_from_parametric) {
      const float2 local = parametric_local(geom.parametric_shape, p);
      const float2 half = geom.parametric_shape.half_size;
      r_cov.shape_uv = float2(half.x > 1e-6f ? (local.x + half.x) / (2.0f * half.x) : 0.5f,
                              half.y > 1e-6f ? (local.y + half.y) / (2.0f * half.y) : 0.5f);
    }
    else {
      const float2 size = geom.uv_max - geom.uv_min;
      r_cov.shape_uv = float2(size.x > 1e-6f ? (p.x - geom.uv_min.x) / size.x : 0.0f,
                              size.y > 1e-6f ? (p.y - geom.uv_min.y) / size.y : 0.0f);
    }
  }

  if (geom.is_parametric) {
    const float d = sd_parametric(geom.parametric_shape, p);
    if (use_fill) {
      r_cov.fill = aa_coverage(-d, style.feather);
      if (want.fill_d) {
        r_cov.fill_d = std::max(0.0f, -d);
      }
      /* Gradient of the inward distance: the negative of the SDF gradient (the SDF grows
       * outward, fill_d grows inward). */
      if (want.fill_dir && d < -ON_EDGE_EPSILON) {
        const float e = 0.25f;
        const float2 grad(
            sd_parametric(geom.parametric_shape, p + float2(e, 0.0f)) -
                sd_parametric(geom.parametric_shape, p - float2(e, 0.0f)),
            sd_parametric(geom.parametric_shape, p + float2(0.0f, e)) -
                sd_parametric(geom.parametric_shape, p - float2(0.0f, e)));
        const float len = math::length(grad);
        if (len > 1e-9f) {
          r_cov.fill_dir = -grad / (2.0f * e * len);
        }
      }
    }
    if (use_stroke) {
      const float halfw = style.stroke_width * 0.5f;
      float shift = 0.0f;
      if (style.stroke_align == PAINT_SHAPE_STROKE_ALIGN_INSIDE) {
        shift = halfw;
      }
      else if (style.stroke_align == PAINT_SHAPE_STROKE_ALIGN_OUTSIDE) {
        shift = -halfw;
      }
      const float dc = d + shift;
      r_cov.stroke = aa_coverage(halfw - math::abs(dc), style.feather);
      if (want.stroke_t) {
        r_cov.stroke_t = math::clamp(math::abs(dc) / halfw, 0.0f, 1.0f);
      }
      /* Gradient of the analytic SDF, oriented away from the stroke centerline. */
      if (want.stroke_dir && math::abs(dc) > ON_EDGE_EPSILON) {
        const float e = 0.25f;
        const float2 grad(
            sd_parametric(geom.parametric_shape, p + float2(e, 0.0f)) -
                sd_parametric(geom.parametric_shape, p - float2(e, 0.0f)),
            sd_parametric(geom.parametric_shape, p + float2(0.0f, e)) -
                sd_parametric(geom.parametric_shape, p - float2(0.0f, e)));
        const float len = math::length(grad);
        if (len > 1e-9f) {
          const float sdc = (dc > 0.0f) ? 1.0f : (dc < 0.0f ? -1.0f : 0.0f);
          r_cov.stroke_dir = grad * (sdc / (2.0f * e * len));
        }
      }
    }
    return;
  }

  NearestHit hit;
  nearest_segment_lookup(geom, p, hit);
  const bool near = (hit.seg >= 0) && (hit.dist <= geom.margin);

  if (use_fill) {
    if (near) {
      const float d_signed = inside ? -hit.dist : hit.dist;
      r_cov.fill = aa_coverage(-d_signed, style.feather);
      if (want.fill_d) {
        r_cov.fill_d = std::max(0.0f, -d_signed);
      }
      /* The distance gradient points away from the nearest edge point, which inside the shape is
       * the inward direction fill_d grows along (the same #g the stroke direction uses). */
      if (want.fill_dir && inside && hit.dist > ON_EDGE_EPSILON) {
        r_cov.fill_dir = (p - hit.closest) / hit.dist;
      }
    }
    else if (inside) {
      r_cov.fill = 1.0f;
      if (want.fill_d) {
        r_cov.fill_d = geom.margin;
      }
    }
  }

  if (!use_stroke || !near) {
    return;
  }

  const float2 a = geom.seg_a[hit.seg];
  const float2 b = geom.seg_b[hit.seg];
  const float wf = math::interpolate(geom.seg_wf0[hit.seg], geom.seg_wf1[hit.seg], hit.t);
  const float w = style.stroke_width * math::abs(wf);
  if (w <= 0.0f) {
    return;
  }
  const float halfw = w * 0.5f;

  /* Signed distance: closed shapes use the winding sign, open ones the side of the line. */
  float sign;
  if (geom.has_closed) {
    sign = inside ? -1.0f : 1.0f;
  }
  else {
    const float2 tangent = b - a;
    const float cross = tangent.x * (p.y - hit.closest.y) - tangent.y * (p.x - hit.closest.x);
    sign = (cross >= 0.0f) ? 1.0f : -1.0f;
  }

  /* Alignment shift moves the stroke band relative to the outline. */
  float shift = 0.0f;
  if (style.stroke_align == PAINT_SHAPE_STROKE_ALIGN_INSIDE) {
    shift = halfw;
  }
  else if (style.stroke_align == PAINT_SHAPE_STROKE_ALIGN_OUTSIDE) {
    shift = -halfw;
  }
  const float dc = sign * hit.dist + shift;

  float cov = aa_coverage(halfw - math::abs(dc), style.feather);
  if (style.join_type != PAINT_SHAPE_JOIN_ROUND && style.stroke_align == PAINT_SHAPE_STROKE_ALIGN_CENTER)
  {
    /* The distance to the nearest segment always rounds a corner; Miter / Bevel rebuild the
     * coverage from the segment bodies and the corner wedges. Inside / Outside alignment keeps the
     * offset distance field (a wedge would have to be offset with it). */
    cov = aa_coverage(-stroke_join_sd(geom, style, p, halfw), style.feather);
  }
  const float s = geom.seg_s0[hit.seg] + hit.t * geom.seg_len[hit.seg];
  const PolyInfo &poly = geom.polys[geom.seg_poly[hit.seg]];

  if (want.stroke_s) {
    /* The seam of a cyclic polyline is its first flattened point: wrap the doubled end point
     * (s == length) back to 0, so the coordinate is single-valued in [0, length). */
    float s_out = s;
    if (poly.cyclic && poly.length > 1e-9f) {
      s_out = std::fmod(s, poly.length);
      if (s_out < 0.0f) {
        s_out += poly.length;
      }
    }
    r_cov.stroke_s = s_out;
    r_cov.stroke_len = poly.length;
  }

  if (!poly.cyclic) {
    /* Cap handling at the two path ends, keyed off the nearest point's arc length. */
    const float length = poly.length;
    if (style.cap_type == PAINT_SHAPE_CAP_BUTT) {
      cov *= aa_coverage(s, PATH_AA) * aa_coverage(length - s, PATH_AA);
    }
    else if (style.cap_type == PAINT_SHAPE_CAP_SQUARE) {
      cov *= aa_coverage(s + halfw, PATH_AA) * aa_coverage(length + halfw - s, PATH_AA);
    }
    /* PAINT_SHAPE_CAP_ROUND: the segment SDF already rounds the ends. */
  }

  if (style.use_dash()) {
    const float period = style.dash_length + style.gap_length;
    if (period > 0.0f && style.dash_length > 0.0f) {
      float u = std::fmod(s + style.dash_offset, period);
      if (u < 0.0f) {
        u += period;
      }
      /* TODO: shape the dash ends by #ShapeStyle::dash_cap (round caps extend past the dash). */
      cov *= aa_coverage(std::min(u, style.dash_length - u), PATH_AA);
    }
  }

  r_cov.stroke = cov;
  if (want.stroke_t) {
    r_cov.stroke_t = math::clamp(math::abs(dc) / halfw, 0.0f, 1.0f);
  }
  if (want.stroke_dir && hit.dist > ON_EDGE_EPSILON) {
    /* Gradient of the across-stroke distance |dc|: away from the stroke's centerline. */
    const float2 g = (p - hit.closest) / hit.dist;
    const float sdc = (dc > 0.0f) ? 1.0f : (dc < 0.0f ? -1.0f : 0.0f);
    r_cov.stroke_dir = g * (sdc * sign);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Winding sweep
 * \{ */

/** Crossings of one pixel row with the closed segments, sorted by x. */
static void row_crossings_collect(const ShapeGeom &geom,
                                  const float ycen,
                                  Vector<std::pair<float, int>> &r_crossings)
{
  r_crossings.clear();
  for (const int si : geom.closed_segs) {
    const float2 &a = geom.seg_a[si];
    const float2 &b = geom.seg_b[si];
    if ((a.y <= ycen) == (b.y <= ycen)) {
      continue;
    }
    const float x = a.x + (ycen - a.y) * (b.x - a.x) / (b.y - a.y);
    r_crossings.append({x, (b.y > a.y) ? 1 : -1});
  }
  std::sort(
      r_crossings.begin(), r_crossings.end(), [](const auto &ca, const auto &cb) {
        return ca.first < cb.first;
      });
}

/**
 * Advance the winding sweep to \a xcen. \a r_cursor and \a r_winding carry over between
 * the pixels of one row (visited in ascending x); reset both per row.
 */
static int winding_advance(const std::pair<float, int> *crossings,
                           const int crossings_num,
                           int &r_cursor,
                           int &r_winding,
                           const float xcen)
{
  while (r_cursor < crossings_num && crossings[r_cursor].first <= xcen) {
    r_winding += crossings[r_cursor].second;
    r_cursor++;
  }
  return r_winding;
}

/** Inside test of a winding count under the style's fill rule. */
static bool winding_inside(const int winding, const ePaintShapeFillRule fill_rule)
{
  if (fill_rule == PAINT_SHAPE_FILL_EVENODD) {
    /* Direction sums and crossing counts agree mod 2, so the signed sum decides parity. */
    return (std::abs(winding) % 2) != 0;
  }
  return winding != 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rasterization
 * \{ */

struct ShapeRasterizer::State {
  Vector<ShapeGeom> geoms;
  /* A copy: the caller-owned style may not outlive a cached rasterizer. */
  ShapeStyle style;
  float2 tile_offset_px = float2(0.0f);
  ShapeRasterOutputs outputs = SHAPE_RASTER_OUTPUTS_ALL;
  /** The mask above, decoded once (see #ShapeRasterWants). */
  ShapeRasterWants wants;
};

ShapeRasterizer::ShapeRasterizer(const Span<PaintShape> shapes,
                                 const ShapeStyle &style,
                                 const rcti &grid_rect,
                                 const float2 &tile_offset_px,
                                 const ShapeRasterOutputs outputs)
{
  state_ = std::make_unique<State>();
  state_->style = style;
  state_->tile_offset_px = tile_offset_px;
  state_->outputs = outputs;
  state_->wants = ShapeRasterWants(outputs);
  rctf domain;
  BLI_rctf_init(&domain,
                tile_offset_px.x + float(grid_rect.xmin),
                tile_offset_px.x + float(grid_rect.xmax),
                tile_offset_px.y + float(grid_rect.ymin),
                tile_offset_px.y + float(grid_rect.ymax));
  for (const PaintShape &shape : shapes) {
    if (shape.is_empty()) {
      continue;
    }
    state_->geoms.append(shape_geometry_build(shape, style, domain, outputs));
  }
}

ShapeRasterizer::~ShapeRasterizer() = default;
ShapeRasterizer::ShapeRasterizer(ShapeRasterizer &&other) noexcept = default;
ShapeRasterizer &ShapeRasterizer::operator=(ShapeRasterizer &&other) noexcept = default;

ShapeCoverage ShapeRasterizer::rasterize(const rcti &rect) const
{
  const ShapeStyle &style = state_->style;
  const float2 &tile_offset_px = state_->tile_offset_px;
  const Span<ShapeGeom> geoms = state_->geoms;

  ShapeCoverage cov;
  cov.rect = rect;
  /* Half-open rect: [xmin, xmax) x [ymin, ymax). */
  const int w = rect.xmax - rect.xmin;
  const int h = rect.ymax - rect.ymin;
  if (w <= 0 || h <= 0 || geoms.is_empty()) {
    return cov;
  }
  const int64_t pixel_num = int64_t(w) * h;
  /* Only the requested outputs own a buffer; the evaluator skips the rest, which keeps the
   * expensive parametric direction/distance fields out of pure Canvas rasterization. */
  const ShapeRasterWants &want = state_->wants;
  if (want.fill) {
    cov.fill = Array<float>(pixel_num, 0.0f);
  }
  if (want.stroke) {
    cov.stroke = Array<float>(pixel_num, 0.0f);
  }
  if (want.stroke_t) {
    cov.stroke_t = Array<float>(pixel_num, 0.0f);
  }
  if (want.stroke_dir) {
    cov.stroke_dir = Array<float2>(pixel_num, float2(0.0f));
  }
  if (want.fill_d) {
    cov.fill_d = Array<float>(pixel_num, 0.0f);
  }
  if (want.fill_dir) {
    cov.fill_dir = Array<float2>(pixel_num, float2(0.0f));
  }
  if (want.stroke_s) {
    cov.stroke_s = Array<float>(pixel_num, 0.0f);
    cov.stroke_len = Array<float>(pixel_num, 0.0f);
  }
  if (want.shape_uv) {
    cov.shape_uv = Array<float2>(pixel_num, float2(0.0f));
  }

  threading::parallel_for(IndexRange(h), 16, [&](const IndexRange rows) {
    /* Per-block scratch: one crossing list per shape, reused for every row. */
    Array<Vector<std::pair<float, int>>> crossings(geoms.size());
    Array<int> cursors(geoms.size());
    Array<int> windings(geoms.size());

    for (const int y : rows) {
      /* \a y is relative to the region; the shape-space scan line adds the region offset. */
      const float ycen = tile_offset_px.y + rect.ymin + y + 0.5f;

      for (const int gi : geoms.index_range()) {
        const ShapeGeom &geom = geoms[gi];
        if (!geom.is_parametric && geom.has_closed) {
          row_crossings_collect(geom, ycen, crossings[gi]);
        }
        else {
          crossings[gi].clear();
        }
        cursors[gi] = 0;
        windings[gi] = 0;
      }

      for (const int x : IndexRange(w)) {
        const float xcen = tile_offset_px.x + rect.xmin + x + 0.5f;
        const float2 p(xcen, ycen);

        PixelCoverage acc;
        float acc_uv_weight = -1.0f;
        for (const int gi : geoms.index_range()) {
          const ShapeGeom &geom = geoms[gi];
          bool inside = false;
          if (!geom.is_parametric && geom.has_closed) {
            inside = winding_inside(winding_advance(crossings[gi].data(),
                                                    int(crossings[gi].size()),
                                                    cursors[gi],
                                                    windings[gi],
                                                    xcen),
                                    style.fill_rule);
          }

          PixelCoverage part;
          shape_eval_pixel(geom, style, p, inside, part, state_->wants);
          /* Symmetry copies combine with the max coverage; the across-stroke data comes from the
           * most covered copy. */
          if (part.fill > acc.fill) {
            acc.fill = part.fill;
            acc.fill_d = part.fill_d;
            acc.fill_dir = part.fill_dir;
          }
          if (part.stroke > acc.stroke) {
            acc.stroke = part.stroke;
            acc.stroke_t = part.stroke_t;
            acc.stroke_dir = part.stroke_dir;
            acc.stroke_s = part.stroke_s;
            acc.stroke_len = part.stroke_len;
          }
          const float part_w = std::max(part.fill, part.stroke);
          if (part_w > acc_uv_weight) {
            acc_uv_weight = part_w;
            acc.shape_uv = part.shape_uv;
          }
        }

        const int64_t idx = int64_t(y) * w + x;
        if (want.fill) {
          cov.fill[idx] = acc.fill;
        }
        if (want.stroke) {
          cov.stroke[idx] = acc.stroke;
        }
        if (want.stroke_t) {
          cov.stroke_t[idx] = acc.stroke_t;
        }
        if (want.stroke_dir) {
          cov.stroke_dir[idx] = acc.stroke_dir;
        }
        if (want.fill_d) {
          cov.fill_d[idx] = acc.fill_d;
        }
        if (want.fill_dir) {
          cov.fill_dir[idx] = acc.fill_dir;
        }
        if (want.stroke_s) {
          cov.stroke_s[idx] = acc.stroke_s;
          cov.stroke_len[idx] = acc.stroke_len;
        }
        if (want.shape_uv) {
          cov.shape_uv[idx] = acc.shape_uv;
        }
      }
    }
  });

  return cov;
}

ShapeCoverage shape_rasterize(const Span<PaintShape> shapes,
                              const ShapeStyle &style,
                              const rcti &rect,
                              const float2 &tile_offset_px,
                              const ShapeRasterOutputs outputs)
{
  ShapeRasterizer rasterizer(shapes, style, rect, tile_offset_px, outputs);
  return rasterizer.rasterize(rect);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
