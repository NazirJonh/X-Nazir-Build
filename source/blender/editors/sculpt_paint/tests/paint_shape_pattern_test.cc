/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../shapes/paint_shape_pattern.hh"

#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

namespace {

/** A default Curve Pattern with the customer's crop frame and no relief. */
PaintShapeCurvePattern default_pattern(const int resolution)
{
  PaintShapeCurvePattern pattern;
  pattern.resolution = resolution;
  pattern.mode = PAINT_SHAPE_CURVE_PATTERN_FILL;
  pattern.flag = 0;
  return pattern;
}

/** A tile of \a pattern filled from one spline: a cyclic square from tile px (32, 32) to
 * (96, 96) — the middle quarter of the 128 px tile (the crop frame maps curve space
 * [-0.25, 0.25] there). */
PaintShape square_pattern_shape()
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_CURVE;
  ShapeSpline spline;
  spline.cyclic = true;
  spline.is_bezier = false;
  const float2 corners[4] = {{32.0f, 32.0f}, {96.0f, 32.0f}, {96.0f, 96.0f}, {32.0f, 96.0f}};
  for (const float2 &co : corners) {
    ShapePoint point;
    point.co = co;
    point.corner = true;
    spline.points.append(point);
  }
  shape.splines.append(std::move(spline));
  return shape;
}

/** The tile value at a uv of the 0..1 square (the pattern's own parametrization). */
static float4 tile_sample(Span<float> buffer, const int resolution, const float2 uv)
{
  const int x = math::clamp(int(uv.x * resolution), 0, resolution - 1);
  const int y = math::clamp(int(uv.y * resolution), 0, resolution - 1);
  const int64_t idx = (int64_t(y) * resolution + x) * 4;
  return float4(buffer[idx], buffer[idx + 1], buffer[idx + 2], buffer[idx + 3]);
}

}  // namespace

TEST(PaintShapePattern, CropFrameMapsToTile)
{
  /* The square occupies the middle quarter of the tile: the center is covered, the corner is
   * not. */
  const PaintShapeCurvePattern pattern = default_pattern(128);
  const PaintShape shape = square_pattern_shape();
  Array<float> buffer(size_t(128) * 128 * 4, 0.0f);
  shape_curve_pattern_tile_rasterize(shape, pattern, 128.0f, buffer.data());

  const float4 center = tile_sample(buffer, 128, float2(0.5f, 0.5f));
  EXPECT_NEAR(center.x, 1.0f, 1e-3f);
  EXPECT_NEAR(center.w, 1.0f, 1e-3f);

  const float4 corner = tile_sample(buffer, 128, float2(0.05f, 0.05f));
  EXPECT_NEAR(corner.x, 0.0f, 1e-3f);
}

TEST(PaintShapePattern, SdfRelief)
{
  /* With USE_SDF_RELIEF the G channel carries the depth: 1 deep inside, 0 at the edge. */
  PaintShapeCurvePattern pattern = default_pattern(128);
  pattern.flag = PAINT_SHAPE_CURVE_PATTERN_USE_SDF_RELIEF;
  const PaintShape shape = square_pattern_shape();
  Array<float> buffer(size_t(128) * 128 * 4, 0.0f);
  shape_curve_pattern_tile_rasterize(shape, pattern, 128.0f, buffer.data());

  /* The center is ~0.125 of the tile in from the square's edge, well within the 5% relief
   * band of the tile... of the 0.25-wide square, so it saturates to 1 only if the band is
   * smaller; the square's half extent in uv is 0.25, the band is 0.05 -> the center saturates. */
  const float4 center = tile_sample(buffer, 128, float2(0.5f, 0.5f));
  EXPECT_NEAR(center.y, 1.0f, 1e-2f);
  /* Just inside the edge (within the 5%-of-tile relief band) the relief is still climbing
   * (positive but below 1). */
  const float4 near_edge = tile_sample(buffer, 128, float2(0.72f, 0.5f));
  EXPECT_GT(near_edge.y, 0.0f);
  EXPECT_LT(near_edge.y, 1.0f);
  /* Without the flag the G channel follows the coverage (a flat plateau). */
  PaintShapeCurvePattern flat = default_pattern(128);
  Array<float> flat_buffer(size_t(128) * 128 * 4, 0.0f);
  shape_curve_pattern_tile_rasterize(shape, flat, 128.0f, flat_buffer.data());
  const float4 flat_center = tile_sample(flat_buffer, 128, float2(0.5f, 0.5f));
  EXPECT_NEAR(flat_center.y, 1.0f, 1e-3f);
}

TEST(PaintShapePattern, WrapCrossingSeams)
{
  /* A stroke ending just short of the top border wraps: with WRAP_CROSSING the opposite (bottom)
   * border pixel is covered by the wrapped copy, without the flag it stays empty. */
  const int resolution = 128;
  PaintShapeCurvePattern wrapped = default_pattern(resolution);
  wrapped.mode = PAINT_SHAPE_CURVE_PATTERN_STROKE;
  wrapped.line_width = 0.05f;
  wrapped.flag = PAINT_SHAPE_CURVE_PATTERN_WRAP_CROSSING;

  PaintShape shape;
  shape.type = PAINT_SHAPE_CURVE;
  ShapeSpline spline;
  spline.cyclic = false;
  spline.is_bezier = false;
  /* A vertical line ending ~2.5 px short of the top border (tile px: x = 64, y up to 126.7). */
  ShapePoint a;
  a.co = float2(64.0f, 89.6f);
  a.corner = true;
  ShapePoint b;
  b.co = float2(64.0f, 126.7f);
  b.corner = true;
  spline.points.append(a);
  spline.points.append(b);
  shape.splines.append(std::move(spline));

  Array<float> buffer(size_t(resolution) * resolution * 4, 0.0f);
  shape_curve_pattern_tile_rasterize(shape, wrapped, float(resolution), buffer.data());
  /* The bottom border pixel on the line's x (uv y ~ 0) is covered by the wrap of the top end. */
  const float4 bottom = tile_sample(buffer, resolution, float2(0.5f, 0.01f));
  EXPECT_NEAR(bottom.x, 1.0f, 1e-2f);

  /* Without the flag the same pixel stays empty. */
  PaintShapeCurvePattern unwrapped = wrapped;
  unwrapped.flag = 0;
  Array<float> unwrapped_buffer(size_t(resolution) * resolution * 4, 0.0f);
  shape_curve_pattern_tile_rasterize(shape, unwrapped, float(resolution), unwrapped_buffer.data());
  const float4 bottom_unwrapped = tile_sample(unwrapped_buffer, resolution, float2(0.5f, 0.01f));
  EXPECT_NEAR(bottom_unwrapped.x, 0.0f, 1e-2f);
}

TEST(PaintShapePattern, StrokeRidgeRelief)
{
  /* The STROKE mode's relief peaks at the centerline (a rounded ridge). */
  const int resolution = 128;
  PaintShapeCurvePattern pattern = default_pattern(resolution);
  pattern.mode = PAINT_SHAPE_CURVE_PATTERN_STROKE;
  pattern.line_width = 0.1f;
  pattern.flag = PAINT_SHAPE_CURVE_PATTERN_USE_SDF_RELIEF;

  PaintShape shape;
  shape.type = PAINT_SHAPE_CURVE;
  ShapeSpline spline;
  spline.cyclic = false;
  spline.is_bezier = false;
  /* A horizontal line across the tile's middle (tile px). */
  ShapePoint a;
  a.co = float2(12.8f, 64.0f);
  a.corner = true;
  ShapePoint b;
  b.co = float2(115.2f, 64.0f);
  b.corner = true;
  spline.points.append(a);
  spline.points.append(b);
  shape.splines.append(std::move(spline));

  Array<float> buffer(size_t(resolution) * resolution * 4, 0.0f);
  shape_curve_pattern_tile_rasterize(shape, pattern, float(resolution), buffer.data());
  const float4 ridge = tile_sample(buffer, resolution, float2(0.5f, 0.5f));
  EXPECT_NEAR(ridge.x, 1.0f, 1e-2f); /* covered */
  EXPECT_NEAR(ridge.y, 1.0f, 1e-2f); /* the ridge crest */

  const float4 flank = tile_sample(buffer, resolution, float2(0.5f, 0.54f));
  EXPECT_LT(flank.y, ridge.y); /* the relief falls off across the stroke */
}

}  // namespace blender::ed::sculpt_paint::shape
