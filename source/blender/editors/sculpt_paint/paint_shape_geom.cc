/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Builders, evaluation and style snapshot of the shape drawing tools; the data model lives in
 * #paint_shape.hh. Pure geometry: the Image-side symmetry and UV/tile mapping live in
 * `mesh/paint_image_shape_symmetry.cc`, the operator property serialization in
 * `paint_shape_op_props.cc`.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_constants.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "BKE_colorband.hh"
#include "BKE_curves.hh"
#include "BKE_colortools.hh"

#include "paint_bezier_input.hh"
#include "paint_shape.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Style
 * \{ */

static float4 to_float4(const float rgba[4])
{
  return float4(rgba[0], rgba[1], rgba[2], rgba[3]);
}

ShapeStyle style_from_settings(const PaintShapeSettings &settings)
{
  ShapeStyle style;
  style.flag = settings.flag;
  style.stroke_align = ePaintShapeStrokeAlign(settings.stroke_align);
  style.cap_type = ePaintShapeCap(settings.cap_type);
  style.join_type = ePaintShapeJoin(settings.join_type);
  style.profile_mode = ePaintShapeProfileMode(settings.profile_mode);
  style.fill_type = ePaintShapeFillType(settings.fill_type);
  style.fill_rule = ePaintShapeFillRule(settings.fill_rule);
  style.dash_cap = ePaintShapeDashCap(settings.dash_cap);
  style.height_blend = ePaintShapeHeightBlend(settings.height_blend);

  style.stroke_width = settings.stroke_width;
  style.feather = settings.feather;
  style.rotation = settings.rotation;
  style.size = float2(settings.size[0], settings.size[1]);
  /* Uniform corners edit a single radius in the UI; replicate it, otherwise the other three
   * would stay at whatever was set while the flag was off. */
  if (settings.flag & PAINT_SHAPE_CORNER_UNIFORM) {
    style.corner_radius = float4(settings.corner_radius[0]);
  }
  else {
    style.corner_radius = float4(settings.corner_radius[0],
                                 settings.corner_radius[1],
                                 settings.corner_radius[2],
                                 settings.corner_radius[3]);
  }
  style.dash_length = settings.dash_length;
  style.gap_length = settings.gap_length;
  style.dash_offset = settings.dash_offset;
  style.height_depth = settings.height_depth;
  style.normal_strength = settings.normal_strength;

  style.stroke_color_picked = to_float4(settings.stroke_color);
  style.fill_color_picked = to_float4(settings.fill_color);
  style.stroke_color = to_scene_linear(style.stroke_color_picked);
  style.fill_color = to_scene_linear(style.fill_color_picked);
  style.stroke_blend = settings.stroke_blend;
  style.fill_blend = settings.fill_blend;
  style.stroke_opacity = settings.stroke_opacity;
  style.fill_opacity = settings.fill_opacity;

  style.fill_profile_width = settings.fill_profile_width;

  /* The profiles are evaluated on the calling (main) thread: the CurveMapping tables must not be
   * touched from the compositor's worker threads. */
  for (const int i : IndexRange(ShapeStyle::PROFILE_TABLE_SIZE)) {
    const float t = (i == 0) ? 0.0f : float(i) / (ShapeStyle::PROFILE_TABLE_SIZE - 1);
    style.stroke_profile_table[i] = settings.stroke_profile ?
                                        BKE_curvemapping_evaluateF(settings.stroke_profile, 0, t) :
                                        1.0f;
    style.fill_profile_table[i] = settings.fill_profile ?
                                      BKE_curvemapping_evaluateF(settings.fill_profile, 0, t) :
                                      1.0f;
  }

  style.use_stroke_ramp = (settings.use_stroke_ramp != 0) && (settings.stroke_ramp != nullptr);
  if (settings.stroke_ramp) {
    for (const int i : IndexRange(ShapeStyle::PROFILE_TABLE_SIZE)) {
      const float t = (i == 0) ? 0.0f : float(i) / (ShapeStyle::PROFILE_TABLE_SIZE - 1);
      float rgba[4];
      BKE_colorband_evaluate(settings.stroke_ramp, t, rgba);
      style.stroke_ramp_table[i] = to_float4(rgba);
    }
  }
  else {
    style.stroke_ramp_table.fill(style.stroke_color);
  }

  style.use_fill_gradient = (settings.fill_type == PAINT_SHAPE_FILL_GRADIENT) &&
                            (settings.fill_gradient != nullptr);
  if (settings.fill_gradient) {
    for (const int i : IndexRange(ShapeStyle::PROFILE_TABLE_SIZE)) {
      const float t = (i == 0) ? 0.0f : float(i) / (ShapeStyle::PROFILE_TABLE_SIZE - 1);
      float rgba[4];
      BKE_colorband_evaluate(settings.fill_gradient, t, rgba);
      style.fill_gradient_table[i] = to_float4(rgba);
    }
  }
  else {
    style.fill_gradient_table.fill(style.fill_color);
  }

  /* Record the selected color source so the shader dispatches once (Texture sources are D4; the
   * flags above stay the source of truth for now). */
  style.stroke_source = style.use_stroke_ramp ? ShapeStrokeSource::Ramp :
                                                ShapeStrokeSource::Solid;
  style.fill_source = style.use_fill_gradient ? ShapeFillSource::Gradient : ShapeFillSource::Solid;

  /* PBR channel values are plain structs: copied verbatim into the snapshot, except the colors,
   * which the settings store as picked (PROP_COLOR_GAMMA) while the channel compositing works in
   * scene linear and converts per buffer, like the part colors above. */
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    style.stroke_channels[i] = settings.stroke_channels[i];
    style.fill_channels[i] = settings.fill_channels[i];
    srgb_to_linearrgb_v3_v3(style.stroke_channels[i].color, settings.stroke_channels[i].color);
    srgb_to_linearrgb_v3_v3(style.fill_channels[i].color, settings.fill_channels[i].color);
  }

  return style;
}

static float profile_table_sample(const std::array<float, ShapeStyle::PROFILE_TABLE_SIZE> &table,
                                  const float t)
{
  const float x = math::clamp(t, 0.0f, 1.0f) * (ShapeStyle::PROFILE_TABLE_SIZE - 1);
  const int i = int(x);
  const int j = std::min(i + 1, ShapeStyle::PROFILE_TABLE_SIZE - 1);
  const float f = x - float(i);
  return table[i] * (1.0f - f) + table[j] * f;
}

float ShapeStyle::stroke_profile_sample(const float t) const
{
  return profile_table_sample(stroke_profile_table, t);
}

float ShapeStyle::fill_profile_sample(const float t) const
{
  return profile_table_sample(fill_profile_table, t);
}

float4 ShapeStyle::stroke_ramp_sample(const float t) const
{
  const float x = math::clamp(t, 0.0f, 1.0f) * (PROFILE_TABLE_SIZE - 1);
  const int i = int(x);
  const int j = std::min(i + 1, PROFILE_TABLE_SIZE - 1);
  const float f = x - float(i);
  return stroke_ramp_table[i] * (1.0f - f) + stroke_ramp_table[j] * f;
}

float4 ShapeStyle::fill_gradient_sample(const float t) const
{
  const float x = math::clamp(t, 0.0f, 1.0f) * (PROFILE_TABLE_SIZE - 1);
  const int i = int(x);
  const int j = std::min(i + 1, PROFILE_TABLE_SIZE - 1);
  const float f = x - float(i);
  return fill_gradient_table[i] * (1.0f - f) + fill_gradient_table[j] * f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spline evaluation
 * \{ */

/**
 * Resolve the handles of point #index: the stored ones when the point owns them, Catmull-Rom
 * auto handles otherwise (mirroring the live Bézier input, so an untouched outline evaluates
 * identically to its preview).
 */
void shape_spline_point_handles_get(const ShapeSpline &spline,
                                    const int index,
                                    float2 &r_handle_left,
                                    float2 &r_handle_right)
{
  const ShapePoint &point = spline.points[index];
  if (!point.auto_handles) {
    r_handle_left = point.handle_left;
    r_handle_right = point.handle_right;
    return;
  }
  const int count = spline.points.size();
  if (count < 2) {
    r_handle_left = point.co;
    r_handle_right = point.co;
    return;
  }

  const int last = count - 1;
  const bool closed = spline.cyclic;
  const float2 prev_co = (index > 0) ? spline.points[index - 1].co :
                                       (closed ? spline.points[last].co :
                                                 point.co * 2.0f - spline.points[1].co);
  const float2 next_co = (index < last) ? spline.points[index + 1].co :
                                          (closed ? spline.points[0].co :
                                                    point.co * 2.0f - spline.points[last - 1].co);
  /* Catmull-Rom style auto handles: a third of the centered tangent. */
  const float2 diff = next_co - prev_co;
  if (math::length_squared(diff) < 1e-20f) {
    r_handle_left = point.co;
    r_handle_right = point.co;
    return;
  }
  r_handle_right = point.co + diff * (1.0f / 6.0f);
  r_handle_left = point.co - diff * (1.0f / 6.0f);
}

static void polyline_finalize(ShapePolyline &poly, const bool cyclic)
{
  poly.cyclic = cyclic;
  poly.arc_len.reinitialize(poly.points.size());
  float length = 0.0f;
  poly.arc_len[0] = 0.0f;
  for (const int i : IndexRange(1, poly.points.size() - 1)) {
    length += math::distance(poly.points[i - 1], poly.points[i]);
    poly.arc_len[i] = length;
  }
}

static void spline_flatten(const ShapeSpline &spline, const float max_error, ShapePolyline &r_out)
{
  r_out = {};
  const int count = spline.points.size();
  if (count == 0) {
    return;
  }
  if (count == 1) {
    r_out.points.append(spline.points[0].co);
    r_out.width.append(spline.points[0].width_factor);
    polyline_finalize(r_out, false);
    return;
  }

  const int segment_count = spline.cyclic ? count : count - 1;
  for (const int segment : IndexRange(segment_count)) {
    const int i = segment;
    const int j = (segment + 1) % count;
    const ShapePoint &pi = spline.points[i];
    const ShapePoint &pj = spline.points[j];
    /* A corner flag makes the incoming segment a straight line; non-Bézier splines are entirely
     * straight. */
    const bool straight = pj.corner || !spline.is_bezier;

    int steps = 1;
    float2 handle_right_i = pi.co;
    float2 handle_left_j = pj.co;
    if (!straight) {
      float2 handle_left_i, handle_right_j;
      shape_spline_point_handles_get(spline, i, handle_left_i, handle_right_i);
      shape_spline_point_handles_get(spline, j, handle_left_j, handle_right_j);
      /* Degenerate handles collapse the segment to a line, like the live input. */
      if (handle_right_i != pi.co || handle_left_j != pj.co) {
        const float control_length = math::distance(pi.co, handle_right_i) +
                                     math::distance(handle_right_i, handle_left_j) +
                                     math::distance(handle_left_j, pj.co);
        steps = std::clamp(int(control_length / max_error) + 1, 4, 256);
      }
    }

    /* The segment points at t = step / steps, from the same evaluation the Curves use. */
    Vector<float2> bezier_points;
    if (!straight) {
      bezier_points.resize(steps);
      bke::curves::bezier::evaluate_segment(
          pi.co, handle_right_i, handle_left_j, pj.co, bezier_points.as_mutable_span());
    }

    for (const int step : IndexRange(steps)) {
      const float t = float(step) / float(steps);
      const float2 p = straight ? math::interpolate(pi.co, pj.co, t) : bezier_points[step];
      r_out.points.append(p);
      r_out.width.append(math::interpolate(pi.width_factor, pj.width_factor, t));
    }
  }

  if (spline.cyclic) {
    /* Close the loop explicitly: the first point repeats at the end. */
    r_out.points.append(spline.points[0].co);
    r_out.width.append(spline.points[0].width_factor);
  }
  else {
    /* Every segment skipped its end point (the next segment starts there); the last segment has
     * no successor, so its end point is appended here. */
    const ShapePoint &last = spline.points.last();
    r_out.points.append(last.co);
    r_out.width.append(last.width_factor);
  }
  polyline_finalize(r_out, spline.cyclic);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parametric evaluation
 * \{ */

static float2 local_to_shape(const PaintShape &shape, const float2 &local)
{
  const float c = math::cos(shape.rotation);
  const float s = math::sin(shape.rotation);
  return shape.center + float2(local.x * c - local.y * s, local.x * s + local.y * c);
}

static void rect_flatten(const PaintShape &shape, const float max_error, ShapePolyline &r_out)
{
  r_out = {};
  const float2 half = shape.half_size;
  /* Corner radius order: TL, TR, BR, BL, clamped to the half size so adjacent radii never
   * overlap. */
  const float rmax = std::min(half.x, half.y);
  const float radius[4] = {std::min(shape.corner_radius.x, rmax),
                           std::min(shape.corner_radius.y, rmax),
                           std::min(shape.corner_radius.z, rmax),
                           std::min(shape.corner_radius.w, rmax)};

  /* Counter-clockwise walk: top edge, TR corner, right edge, BR corner, bottom edge, BL corner,
   * left edge, TL corner. */
  auto straight = [&](const float2 &from, const float2 &to) {
    const float length = math::distance(from, to);
    const int steps = std::max(1, int(length / max_error) + 1);
    for (const int step : IndexRange(steps)) {
      r_out.points.append(
          local_to_shape(shape, math::interpolate(from, to, float(step) / float(steps))));
      r_out.width.append(1.0f);
    }
  };
  auto arc = [&](const float2 &center, const float radius, const float start, const float sweep) {
    if (radius <= 0.0f) {
      r_out.points.append(local_to_shape(shape, center));
      r_out.width.append(1.0f);
      return;
    }
    const int steps = std::max(2, int((math::abs(sweep) * radius) / max_error) + 1);
    for (const int step : IndexRange(steps)) {
      const float angle = start + sweep * (float(step) / float(steps));
      r_out.points.append(
          local_to_shape(shape, center + radius * float2(math::cos(angle), math::sin(angle))));
      r_out.width.append(1.0f);
    }
  };

  straight(float2(-half.x + radius[0], half.y), float2(half.x - radius[1], half.y));
  arc(float2(half.x - radius[1], half.y - radius[1]), radius[1], 0.5f * M_PI, -0.5f * M_PI);
  straight(float2(half.x, half.y - radius[1]), float2(half.x, -half.y + radius[2]));
  arc(float2(half.x - radius[2], -half.y + radius[2]), radius[2], 0.0f, -0.5f * M_PI);
  straight(float2(half.x - radius[2], -half.y), float2(-half.x + radius[3], -half.y));
  arc(float2(-half.x + radius[3], -half.y + radius[3]), radius[3], 1.5f * M_PI, -0.5f * M_PI);
  straight(float2(-half.x, -half.y + radius[3]), float2(-half.x, half.y - radius[0]));
  arc(float2(-half.x + radius[0], half.y - radius[0]), radius[0], M_PI, -0.5f * M_PI);

  /* Close the loop explicitly. */
  r_out.points.append(r_out.points[0]);
  r_out.width.append(1.0f);
  polyline_finalize(r_out, true);
}

static void ellipse_flatten(const PaintShape &shape, const float max_error, ShapePolyline &r_out)
{
  r_out = {};
  /* Ramanujan's perimeter approximation, good enough to pick the sample count. */
  const float a = shape.half_size.x;
  const float b = shape.half_size.y;
  const float perimeter = M_PI * (3.0f * (a + b) - math::sqrt((3.0f * a + b) * (a + 3.0f * b)));
  const int steps = std::clamp(int(perimeter / max_error) + 1, 16, 512);
  for (const int step : IndexRange(steps)) {
    const float angle = 2.0f * M_PI * (float(step) / float(steps));
    r_out.points.append(
        local_to_shape(shape, shape.half_size * float2(math::cos(angle), math::sin(angle))));
    r_out.width.append(1.0f);
  }
  /* Close the loop explicitly. */
  r_out.points.append(r_out.points[0]);
  r_out.width.append(1.0f);
  polyline_finalize(r_out, true);
}

void shape_flatten(const PaintShape &shape, const float max_error_px, Vector<ShapePolyline> &r_out)
{
  r_out.clear();
  if (shape.has_analytic_sdf()) {
    ShapePolyline poly;
    if (shape.type == PAINT_SHAPE_RECT) {
      rect_flatten(shape, max_error_px, poly);
    }
    else {
      ellipse_flatten(shape, max_error_px, poly);
    }
    r_out.append(std::move(poly));
    return;
  }
  for (const ShapeSpline &spline : shape.splines) {
    ShapePolyline poly;
    spline_flatten(spline, max_error_px, poly);
    if (!poly.points.is_empty()) {
      r_out.append(std::move(poly));
    }
  }
}

Vector<ShapePolyline> shape_flatten(const PaintShape &shape, const float max_error_px)
{
  Vector<ShapePolyline> out;
  shape_flatten(shape, max_error_px, out);
  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bounds
 * \{ */

rctf shape_bounds_calc(const PaintShape &shape, const ShapeStyle &style)
{
  rctf bounds;
  BLI_rctf_init_minmax(&bounds);
  for (const ShapePolyline &poly : shape_flatten(shape, 1.0f)) {
    for (const float2 &co : poly.points) {
      BLI_rctf_do_minmax_v(&bounds, co);
    }
  }

  if (bounds.xmin > bounds.xmax) {
    /* Degenerate (empty) shape. */
    BLI_rctf_init(&bounds, 0.0f, 0.0f, 0.0f, 0.0f);
    return bounds;
  }

  /* The stroke width follows the per-point width factors (symmetry copies of the circle
   * inversion can scale them far past 1). */
  float max_width_factor = 1.0f;
  for (const ShapeSpline &spline : shape.splines) {
    for (const ShapePoint &point : spline.points) {
      max_width_factor = std::max(max_width_factor, math::abs(point.width_factor));
    }
  }

  float margin = style.feather;
  if (style.use_stroke()) {
    /* Generous across all stroke alignments: the SDF region is clipped by it, never cut. */
    /* A miter tip reaches up to 4 half widths (the raster's miter limit) past its vertex. */
    const float join_scale = style.join_type == PAINT_SHAPE_JOIN_MITER ? 4.0f : 1.0f;
    margin += style.stroke_width * max_width_factor * join_scale + style.feather;
  }
  if (style.use_fill() && style.use_profile()) {
    margin += style.fill_profile_width * 0.5f;
  }
  bounds.xmin -= margin;
  bounds.ymin -= margin;
  bounds.xmax += margin;
  bounds.ymax += margin;
  return bounds;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Builders
 * \{ */

PaintShape shape_line(const float2 &p0, const float2 &p1)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_LINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.cyclic = false;
  ShapePoint a;
  a.co = p0;
  a.auto_handles = false;
  spline.points.append(a);
  ShapePoint b;
  b.co = p1;
  b.auto_handles = false;
  spline.points.append(b);
  shape.splines.append(std::move(spline));
  return shape;
}

static void drag_extents(const float2 &p0,
                         const float2 &p1,
                         const bool from_center,
                         const bool keep_aspect,
                         float2 &r_center,
                         float2 &r_half)
{
  if (from_center) {
    r_center = p0;
    /* \a p1 is a corner: its distance to the center is already a half extent. */
    r_half = math::abs(p1 - p0);
  }
  else {
    r_center = (p0 + p1) * 0.5f;
    r_half = math::abs(p1 - p0) * 0.5f;
  }
  if (keep_aspect) {
    const float extent = std::max(r_half.x, r_half.y);
    r_half = float2(extent);
  }
}

PaintShape shape_rect_from_drag(const float2 &p0,
                                const float2 &p1,
                                const bool from_center,
                                const bool keep_aspect,
                                const ShapeStyle &style)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_RECT;
  drag_extents(p0, p1, from_center, keep_aspect, shape.center, shape.half_size);
  shape.rotation = style.rotation;
  const float rmax = std::min(shape.half_size.x, shape.half_size.y);
  shape.corner_radius = float4(std::min(style.corner_radius.x, rmax),
                               std::min(style.corner_radius.y, rmax),
                               std::min(style.corner_radius.z, rmax),
                               std::min(style.corner_radius.w, rmax));
  return shape;
}

PaintShape shape_ellipse_from_drag(const float2 &p0,
                                   const float2 &p1,
                                   const bool from_center,
                                   const bool keep_aspect)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  drag_extents(p0, p1, from_center, keep_aspect, shape.center, shape.half_size);
  return shape;
}

PaintShape shape_rect_at_center(const float2 &center, const ShapeStyle &style)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_RECT;
  shape.center = center;
  shape.half_size = style.size * 0.5f;
  shape.rotation = style.rotation;
  const float rmax = std::min(shape.half_size.x, shape.half_size.y);
  shape.corner_radius = float4(std::min(style.corner_radius.x, rmax),
                               std::min(style.corner_radius.y, rmax),
                               std::min(style.corner_radius.z, rmax),
                               std::min(style.corner_radius.w, rmax));
  return shape;
}

PaintShape shape_ellipse_at_center(const float2 &center, const ShapeStyle &style)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = center;
  shape.half_size = style.size * 0.5f;
  shape.rotation = style.rotation;
  return shape;
}

PaintShape shape_from_bezier_input(const bezier_input::BezierInput &input, const ePaintShapeType type)
{
  PaintShape shape;
  shape.type = type;
  ShapeSpline spline;
  spline.cyclic = input.is_closed();
  spline.is_bezier = (type == PAINT_SHAPE_CURVE);

  /* The first point's corner flag is the closing segment (last point back to first): carry the
   * Ctrl-at-close choice into the committed shape so ctrl-closing a Curve Patch flattens straight,
   * exactly like the live input previewed it. */
  const bool closing_straight = spline.cyclic && input.closed_straight();
  for (const int point_index : input.points().index_range()) {
    const bezier_input::BezierInputPoint &point = input.points()[point_index];
    ShapePoint out;
    out.co = point.co;
    out.corner = point.straight_prev || !spline.is_bezier ||
                 (point_index == 0 && closing_straight);
    if (spline.is_bezier && point.handles_set) {
      out.handle_left = point.handle_left;
      out.handle_right = point.handle_right;
      out.auto_handles = false;
    }
    spline.points.append(out);
  }

  if (spline.points.size() >= 2) {
    shape.splines.append(std::move(spline));
  }
  return shape;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Translation
 * \{ */

void shape_translate(PaintShape &shape, const float2 &delta_px)
{
  /* A custom origin is pinned to the shape and travels with it; a default one is derived from
   * the points, so translating them moves it implicitly. */
  if (shape.origin_is_custom) {
    shape.origin += delta_px;
  }
  if (shape.is_parametric()) {
    shape.center += delta_px;
    return;
  }
  for (ShapeSpline &spline : shape.splines) {
    for (ShapePoint &point : spline.points) {
      point.co += delta_px;
      point.handle_left += delta_px;
      point.handle_right += delta_px;
    }
  }
}

/** Rotate \a v around \a pivot by the angle with cosine \a c and sine \a s. */
static float2 rotate_point_around(const float2 &v, const float2 &pivot, const float c, const float s)
{
  const float2 rel = v - pivot;
  return pivot + float2(rel.x * c - rel.y * s, rel.x * s + rel.y * c);
}

void shape_rotate_about(PaintShape &shape, const float2 &pivot, const float dangle)
{
  const float c = math::cos(dangle);
  const float s = math::sin(dangle);
  if (shape.is_parametric()) {
    shape.center = rotate_point_around(shape.center, pivot, c, s);
    shape.rotation += dangle;
    return;
  }
  for (ShapeSpline &spline : shape.splines) {
    for (ShapePoint &point : spline.points) {
      point.co = rotate_point_around(point.co, pivot, c, s);
      point.handle_left = rotate_point_around(point.handle_left, pivot, c, s);
      point.handle_right = rotate_point_around(point.handle_right, pivot, c, s);
    }
  }
}

void shape_clamp_corner_radius(PaintShape &shape)
{
  if (shape.type != PAINT_SHAPE_RECT || !shape.is_parametric()) {
    return;
  }
  const float rmax = std::min(shape.half_size.x, shape.half_size.y);
  shape.corner_radius = float4(std::min(shape.corner_radius.x, rmax),
                               std::min(shape.corner_radius.y, rmax),
                               std::min(shape.corner_radius.z, rmax),
                               std::min(shape.corner_radius.w, rmax));
}

void shape_set_size(PaintShape &shape, const float2 &size)
{
  if (!shape.has_analytic_sdf()) {
    return;
  }
  shape.half_size = float2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)) * 0.5f;
  shape_clamp_corner_radius(shape);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Target resolution
 * \{ */

Vector<PaintShape> shapes_scale_to_target(const Span<PaintShape> shapes, const float2 &scale)
{
  Vector<PaintShape> scaled;
  scaled.reserve(shapes.size());
  const float scale_avg = 0.5f * (scale.x + scale.y);
  for (const PaintShape &shape : shapes) {
    /* A rotated parametric shape skews under an axis-asymmetric scale, which scaling center and
     * half size cannot express: densify it into polylines first and scale the points, the same
     * fallback the non-affine symmetry maps use. */
    const bool densify = shape.is_parametric() && shape.rotation != 0.0f &&
                         scale.x != scale.y;
    PaintShape copy = shape;
    if (densify) {
      /* The flatten error is set in reference pixels; dividing by the larger factor keeps it
       * within half a pixel of the target resolution. */
      const Vector<ShapePolyline> outlines = shape_flatten(
          shape, 0.5f / std::max(scale.x, scale.y));
      copy.splines.clear();
      for (const ShapePolyline &outline : outlines) {
        ShapeSpline spline;
        spline.is_bezier = false;
        spline.cyclic = outline.cyclic;
        for (const int i : outline.points.index_range()) {
          ShapePoint point;
          point.co = outline.points[i];
          point.width_factor = outline.width[i];
          spline.points.append(point);
        }
        if (spline.points.size() >= 2) {
          copy.splines.append(std::move(spline));
        }
      }
      if (copy.splines.is_empty()) {
        continue;
      }
    }
    copy.center *= scale;
    copy.half_size *= scale;
    copy.corner_radius *= scale_avg;
    if (copy.origin_is_custom) {
      copy.origin *= scale;
    }
    for (ShapeSpline &spline : copy.splines) {
      for (ShapePoint &point : spline.points) {
        point.co *= scale;
        point.handle_left *= scale;
        point.handle_right *= scale;
      }
    }
    scaled.append(std::move(copy));
  }
  return scaled;
}

ShapeStyle style_scale_to_target(const ShapeStyle &style, const float2 &scale)
{
  ShapeStyle scaled = style;
  const float scale_avg = 0.5f * (scale.x + scale.y);
  scaled.stroke_width *= scale_avg;
  /* AA needs a band of at least a pixel to stay smooth: never scale the feather below one,
   * while a deliberately smaller user value keeps its choice. */
  scaled.feather = std::max(scaled.feather, std::min(style.feather, 1.0f));
  scaled.dash_length *= scale_avg;
  scaled.gap_length *= scale_avg;
  scaled.dash_offset *= scale_avg;
  scaled.fill_profile_width *= scale_avg;
  return scaled;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Open-shape style resolution
 * \{ */

/** Whether \a shape is an open outline: it has a spline that is not cyclic. A parametric
 * Rect/Ellipse owns no spline and is always closed. */
static bool shape_is_open(const PaintShape &shape)
{
  if (shape.is_parametric()) {
    return shape_type_traits(shape.type).open;
  }
  for (const ShapeSpline &spline : shape.splines) {
    if (!spline.cyclic) {
      return true;
    }
  }
  return false;
}

ShapeStyle style_resolve_for_shapes(const Span<PaintShape> shapes, ShapeStyle style)
{
  bool any_open = false;
  for (const PaintShape &shape : shapes) {
    if (shape_is_open(shape)) {
      any_open = true;
      break;
    }
  }
  if (!any_open) {
    return style;
  }
  /* An open shape has no interior to fill, so the stroke is the only thing it can draw: force
   * it on (with a minimum width) even when the user turned Stroke off, so it is never invisible.
   * Closed shapes keep the user's fill-only choice. */
  style.flag |= PAINT_SHAPE_USE_STROKE;
  if (style.stroke_width <= 0.0f) {
    style.stroke_width = 1.0f;
  }
  return style;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
