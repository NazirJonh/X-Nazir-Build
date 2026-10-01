/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "../paint_shape_render.hh"

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

static ShapeStyle flat_style()
{
  ShapeStyle style;
  style.stroke_profile_table.fill(1.0f);
  style.fill_profile_table.fill(1.0f);
  style.stroke_ramp_table.fill(float4(1.0f, 1.0f, 1.0f, 1.0f));
  style.feather = 1.0f;
  return style;
}

static double integrate(const Array<float> &buffer)
{
  double sum = 0.0;
  for (const float value : buffer) {
    sum += double(value);
  }
  return sum;
}

static ShapeCoverage rasterize(const PaintShape &shape,
                               const ShapeStyle &style,
                               const rcti &rect)
{
  return shape_rasterize(Span<PaintShape>(&shape, 1), style, rect, float2(0.0f));
}

TEST(ShapeRaster, CircleFillArea)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(60.0f, 60.0f);
  shape.half_size = float2(50.0f, 50.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;

  rcti rect;
  BLI_rcti_init(&rect, 0, 119, 0, 119);
  const double area = integrate(rasterize(shape, style, rect).fill);
  const double expected = M_PI * 50.0 * 50.0;
  EXPECT_NEAR(area, expected, expected * 0.01);
}

TEST(ShapeRaster, RoundedRectFillArea)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_RECT;
  shape.center = float2(110.0f, 70.0f);
  shape.half_size = float2(100.0f, 50.0f);
  shape.corner_radius = float4(30.0f, 30.0f, 30.0f, 30.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;

  rcti rect;
  BLI_rcti_init(&rect, 0, 219, 0, 139);
  const double area = integrate(rasterize(shape, style, rect).fill);
  const double expected = 200.0 * 100.0 - (4.0 - M_PI) * 30.0 * 30.0;
  EXPECT_NEAR(area, expected, expected * 0.01);
}

TEST(ShapeRaster, EllipseFillArea)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(70.0f, 50.0f);
  shape.half_size = float2(50.0f, 20.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;

  rcti rect;
  BLI_rcti_init(&rect, 0, 139, 0, 99);
  const double area = integrate(rasterize(shape, style, rect).fill);
  const double expected = M_PI * 50.0 * 20.0;
  EXPECT_NEAR(area, expected, expected * 0.01);
}

TEST(ShapeRaster, DashCoverageHalf)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_LINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(20.0f, 60.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(220.0f, 60.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 10.0f;
  style.cap_type = PAINT_SHAPE_CAP_ROUND;

  rcti rect;
  BLI_rcti_init(&rect, 0, 239, 0, 119);

  ShapeStyle full_style = style;
  ShapeStyle dashed_style = style;
  dashed_style.flag |= PAINT_SHAPE_USE_DASH;
  dashed_style.dash_length = 25.0f;
  dashed_style.gap_length = 25.0f;

  const double full = integrate(rasterize(shape, full_style, rect).stroke);
  const double dashed = integrate(rasterize(shape, dashed_style, rect).stroke);
  EXPECT_GT(full, 0.0);
  const double ratio = dashed / full;
  EXPECT_NEAR(ratio, 0.5, 0.06);
}

TEST(ShapeRaster, StrokeCenterlineTZero)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_LINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(10.0f, 50.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(110.0f, 50.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 10.0f;

  rcti rect;
  BLI_rcti_init(&rect, 0, 119, 0, 99);
  const ShapeCoverage cov = rasterize(shape, style, rect);

  /* A pixel whose center sits 0.5 px off the centerline: the across-stroke coordinate must be
   * near zero and the coverage near full. */
  const int64_t idx = cov.index(60, 50);
  EXPECT_GT(cov.stroke[idx], 0.9f);
  EXPECT_LT(cov.stroke_t[idx], 0.25f);
}

TEST(ShapeRaster, AlignInsideKeepsOutsideClean)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_RECT;
  shape.center = float2(60.0f, 60.0f);
  shape.half_size = float2(50.0f, 50.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 20.0f;
  style.stroke_align = PAINT_SHAPE_STROKE_ALIGN_INSIDE;

  rcti rect;
  BLI_rcti_init(&rect, 0, 119, 0, 119);
  const ShapeCoverage cov = rasterize(shape, style, rect);

  /* 15 px outside the outline: an inside-aligned stroke never reaches it. */
  const int64_t outside = cov.index(125, 60);
  EXPECT_LT(cov.stroke[outside], 1e-4f);
  /* 2.5 px inside the outline: well within the inside band. */
  const int64_t inside = cov.index(108, 60);
  EXPECT_GT(cov.stroke[inside], 0.5f);
}

TEST(ShapeRaster, SymmetryCopiesCombineByMax)
{
  /* Two identical ellipses: the combined fill equals one ellipse. */
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(60.0f, 60.0f);
  shape.half_size = float2(30.0f, 30.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;

  rcti rect;
  BLI_rcti_init(&rect, 0, 119, 0, 119);
  const Array<PaintShape> shapes = {shape, shape};
  const ShapeCoverage cov = shape_rasterize(shapes.as_span(), style, rect, float2(0.0f));
  const double area = integrate(cov.fill);
  const double expected = M_PI * 30.0 * 30.0;
  EXPECT_NEAR(area, expected, expected * 0.02);
}

TEST(ShapeGeom, ScaleToTargetResolutionIndependent)
{
  /* The same shape rasterized into a tile of twice the reference resolution covers four times
   * the pixels around the doubled centroid: the painted shape is resolution independent, which
   * is what PBR channel maps of a different size than the canvas rely on. */
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(64.0f, 64.0f);
  shape.half_size = float2(30.0f, 30.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  style.stroke_width = 10.0f;

  rcti small_rect;
  BLI_rcti_init(&small_rect, 0, 127, 0, 127);
  const ShapeCoverage small_cov = shape_rasterize(
      Span<PaintShape>(&shape, 1), style, small_rect, float2(0.0f));

  const Vector<PaintShape> scaled = shapes_scale_to_target(Span<PaintShape>(&shape, 1),
                                                           float2(2.0f));
  ASSERT_EQ(scaled.size(), 1);
  EXPECT_NEAR(scaled[0].center.x, 128.0f, 1e-4);
  EXPECT_NEAR(scaled[0].center.y, 128.0f, 1e-4);
  EXPECT_NEAR(scaled[0].half_size.x, 60.0f, 1e-4);
  const ShapeStyle scaled_style = style_scale_to_target(style, float2(2.0f));
  EXPECT_NEAR(scaled_style.stroke_width, 20.0f, 1e-5);
  EXPECT_NEAR(scaled_style.feather, style.feather * 2.0f, 1e-5);

  rcti big_rect;
  BLI_rcti_init(&big_rect, 0, 255, 0, 255);
  const ShapeCoverage big_cov = shape_rasterize(scaled.as_span(),
                                                scaled_style,
                                                big_rect,
                                                float2(0.0f));

  const double small_area = integrate(small_cov.fill);
  const double big_area = integrate(big_cov.fill);
  /* Each resolution against its own analytic area: the doubled radius in the doubled tile is
   * the same UV-space shape covering exactly four times the pixels. */
  const double small_expected = M_PI * 30.0 * 30.0;
  const double big_expected = M_PI * 60.0 * 60.0;
  EXPECT_NEAR(small_area, small_expected, small_expected * 0.02);
  EXPECT_NEAR(big_area, big_expected, big_expected * 0.02);

  const auto centroid = [](const ShapeCoverage &cov) {
    double x = 0.0;
    double y = 0.0;
    double weight_sum = 0.0;
    for (const int py : IndexRange(cov.rect.ymin, cov.rect.ymax - cov.rect.ymin)) {
      for (const int px : IndexRange(cov.rect.xmin, cov.rect.xmax - cov.rect.xmin)) {
        const double weight = double(cov.fill[cov.index(px, py)]);
        x += weight * (double(px) + 0.5);
        y += weight * (double(py) + 0.5);
        weight_sum += weight;
      }
    }
    return float2(float(x / weight_sum), float(y / weight_sum));
  };
  const float2 small_centroid = centroid(small_cov);
  const float2 big_centroid = centroid(big_cov);
  EXPECT_NEAR(big_centroid.x, small_centroid.x * 2.0f, 0.5f);
  EXPECT_NEAR(big_centroid.y, small_centroid.y * 2.0f, 0.5f);
}

TEST(ShapeRaster, OffsetRegionMatchesOriginRegion)
{
  /* The same circle rasterized over a region that does not start at the tile corner: the region
   * offset must land the shape where it belongs and the buffers must stay inside their bounds
   * (a negative index here would corrupt the heap). */
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(260.0f, 260.0f);
  shape.half_size = float2(50.0f, 50.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;

  rcti offset_rect;
  BLI_rcti_init(&offset_rect, 200, 320, 200, 320);
  const double area = integrate(rasterize(shape, style, offset_rect).fill);
  const double expected = M_PI * 50.0 * 50.0;
  EXPECT_NEAR(area, expected, expected * 0.01);
}

TEST(ShapeEvaluator, MatchesRasterAtPixelCenters)
{
  /* The point evaluator must agree with the grid rasterizer at pixel centers. */
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(60.0f, 60.0f);
  shape.half_size = float2(30.0f, 18.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL | PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 8.0f;

  rcti rect;
  BLI_rcti_init(&rect, 20, 100, 20, 100);
  const ShapeCoverage cov = rasterize(shape, style, rect);

  rctf domain;
  BLI_rctf_init(&domain, 20.0f, 100.0f, 20.0f, 100.0f);
  const ShapeEvaluator evaluator(Span<PaintShape>(&shape, 1), style, domain);
  for (const int y : IndexRange(rect.ymin, rect.ymax - rect.ymin)) {
    for (const int x : IndexRange(rect.xmin, rect.xmax - rect.xmin)) {
      const ShapeSample sample = evaluator.sample(float2(x + 0.5f, y + 0.5f));
      const int64_t idx = cov.index(x, y);
      EXPECT_NEAR(sample.fill, cov.fill[idx], 1e-4) << "fill at (" << x << ", " << y << ")";
      EXPECT_NEAR(sample.stroke, cov.stroke[idx], 1e-4) << "stroke at (" << x << ", " << y << ")";
      EXPECT_NEAR(sample.stroke_t, cov.stroke_t[idx], 1e-4) << "t at (" << x << ", " << y << ")";
      EXPECT_NEAR(sample.fill_d, cov.fill_d[idx], 1e-4) << "fill_d at (" << x << ", " << y << ")";
    }
  }
}

TEST(ShapeEvaluator, EvenOddHole)
{
  /* Outer square plus inner square: non-zero fills both, even-odd leaves a hole. */
  auto square_spline = [](const float2 &lo, const float2 &hi) {
    ShapeSpline spline;
    spline.is_bezier = false;
    spline.cyclic = true;
    for (const float2 &co : {lo, float2(hi.x, lo.y), hi, float2(lo.x, hi.y)}) {
      ShapePoint point;
      point.co = co;
      point.corner = true;
      point.auto_handles = false;
      spline.points.append(point);
    }
    return spline;
  };
  PaintShape shape;
  shape.type = PAINT_SHAPE_POLYLINE;
  shape.splines.append(square_spline(float2(10.0f, 10.0f), float2(90.0f, 90.0f)));
  shape.splines.append(square_spline(float2(30.0f, 30.0f), float2(70.0f, 70.0f)));

  rctf domain;
  BLI_rctf_init(&domain, 0.0f, 100.0f, 0.0f, 100.0f);
  const PaintShape shapes[] = {shape};

  ShapeStyle nonzero = flat_style();
  nonzero.flag = PAINT_SHAPE_USE_FILL;
  nonzero.fill_rule = PAINT_SHAPE_FILL_NONZERO;
  const ShapeEvaluator eval_nonzero(Span<PaintShape>(shapes, 1), nonzero, domain);
  EXPECT_GT(eval_nonzero.sample(float2(50.0f, 50.0f)).fill, 0.9f);
  EXPECT_GT(eval_nonzero.sample(float2(20.0f, 20.0f)).fill, 0.9f);
  EXPECT_LT(eval_nonzero.sample(float2(5.0f, 5.0f)).fill, 0.1f);

  ShapeStyle evenodd = flat_style();
  evenodd.flag = PAINT_SHAPE_USE_FILL;
  evenodd.fill_rule = PAINT_SHAPE_FILL_EVENODD;
  const ShapeEvaluator eval_evenodd(Span<PaintShape>(shapes, 1), evenodd, domain);
  EXPECT_LT(eval_evenodd.sample(float2(50.0f, 50.0f)).fill, 0.1f);
  EXPECT_GT(eval_evenodd.sample(float2(20.0f, 20.0f)).fill, 0.9f);
}

TEST(ShapeGeom, AutoHandlesResolveMatchesFlatten)
{
  /* Serialization resolves auto handles to explicit ones (`shape_to_op_props`): the resolved
   * shape must flatten identically to the auto one, while the old buggy form (origin handles
   * with `auto_handles = false`) pulls the curve towards the origin. */
  PaintShape auto_shape;
  auto_shape.type = PAINT_SHAPE_CURVE;
  ShapeSpline spline;
  spline.is_bezier = true;
  spline.cyclic = false;
  ShapePoint p0, p1, p2;
  p0.co = float2(10.0f, 10.0f);
  p0.auto_handles = true;
  p1.co = float2(50.0f, 80.0f);
  p1.auto_handles = true;
  p2.co = float2(90.0f, 10.0f);
  p2.auto_handles = true;
  spline.points.append(p0);
  spline.points.append(p1);
  spline.points.append(p2);
  auto_shape.splines.append(spline);

  const Vector<ShapePolyline> auto_flat = shape_flatten(auto_shape, 0.5f);
  ASSERT_FALSE(auto_flat.is_empty());
  ASSERT_GT(auto_flat[0].points.size(), 2);

  PaintShape resolved_shape = auto_shape;
  for (ShapeSpline &resolved_spline : resolved_shape.splines) {
    for (const int i : resolved_spline.points.index_range()) {
      ShapePoint &point = resolved_spline.points[i];
      if (point.auto_handles) {
        shape_spline_point_handles_get(auto_shape.splines[0], i, point.handle_left, point.handle_right);
        point.auto_handles = false;
      }
    }
  }
  /* The middle auto handles leave the origin. */
  EXPECT_GT(math::length_squared(resolved_shape.splines[0].points[1].handle_left -
                                float2(0.0f)),
            1.0f);

  const Vector<ShapePolyline> resolved_flat = shape_flatten(resolved_shape, 0.5f);
  ASSERT_EQ(resolved_flat.size(), auto_flat.size());
  ASSERT_EQ(resolved_flat[0].points.size(), auto_flat[0].points.size());
  for (const int i : resolved_flat[0].points.index_range()) {
    EXPECT_NEAR(resolved_flat[0].points[i].x, auto_flat[0].points[i].x, 1e-4);
    EXPECT_NEAR(resolved_flat[0].points[i].y, auto_flat[0].points[i].y, 1e-4);
  }

  PaintShape buggy_shape = auto_shape;
  for (ShapeSpline &buggy_spline : buggy_shape.splines) {
    for (ShapePoint &point : buggy_spline.points) {
      point.auto_handles = false;
    }
  }
  const Vector<ShapePolyline> buggy_flat = shape_flatten(buggy_shape, 0.5f);
  ASSERT_EQ(buggy_flat[0].points.size(), auto_flat[0].points.size());
  bool differs = false;
  for (const int i : buggy_flat[0].points.index_range()) {
    if (math::distance(buggy_flat[0].points[i], auto_flat[0].points[i]) > 1.0f) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs);
}

TEST(ShapeRaster, SubsetOutputsLeaveUnrequestedBuffersEmpty)
{
  /* Canvas-style flags skip the direction/distance buffers but keep identical base coverage. */
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(60.0f, 60.0f);
  shape.half_size = float2(30.0f, 18.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL | PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 8.0f;

  rcti rect;
  BLI_rcti_init(&rect, 20, 100, 20, 100);

  const ShapeRasterOutputs canvas_outputs = ShapeRasterOutputs::Fill | ShapeRasterOutputs::Stroke |
                                            ShapeRasterOutputs::StrokeT;
  ShapeRasterizer rasterizer(Span<PaintShape>(&shape, 1), style, rect, float2(0.0f), canvas_outputs);
  const ShapeCoverage cov = rasterizer.rasterize(rect);
  EXPECT_TRUE(cov.stroke_dir.is_empty());
  EXPECT_TRUE(cov.fill_d.is_empty());
  EXPECT_TRUE(cov.fill_dir.is_empty());
  EXPECT_FALSE(cov.fill.is_empty());
  EXPECT_FALSE(cov.stroke.is_empty());
  EXPECT_FALSE(cov.stroke_t.is_empty());

  const ShapeCoverage full = rasterize(shape, style, rect);
  EXPECT_EQ(cov.fill.size(), full.fill.size());
  for (int64_t i = 0; i < cov.fill.size(); i++) {
    EXPECT_NEAR(cov.fill[i], full.fill[i], 1e-6);
    EXPECT_NEAR(cov.stroke[i], full.stroke[i], 1e-6);
    EXPECT_NEAR(cov.stroke_t[i], full.stroke_t[i], 1e-6);
  }
}

TEST(ShapeRaster, LineStrokeSMonotone)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_LINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(10.0f, 50.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(110.0f, 50.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 10.0f;

  rcti rect;
  BLI_rcti_init(&rect, 0, 120, 40, 61);
  const ShapeRasterOutputs outputs = ShapeRasterOutputs::Stroke | ShapeRasterOutputs::StrokeS;
  const ShapeCoverage cov = shape_rasterize(
      Span<PaintShape>(&shape, 1), style, rect, float2(0.0f), outputs);

  /* Along the centerline the along-stroke coordinate grows with x and equals the distance from
   * the line start. */
  float prev_s = -1.0f;
  for (int x = 15; x <= 105; x += 10) {
    const int64_t idx = cov.index(x, 50);
    EXPECT_GT(cov.stroke[idx], 0.0f);
    EXPECT_GE(cov.stroke_s[idx], prev_s - 1e-3f);
    prev_s = cov.stroke_s[idx];
  }
  const int64_t mid = cov.index(60, 50);
  EXPECT_NEAR(cov.stroke_s[mid], 50.0f, 1.5f);
  EXPECT_NEAR(cov.stroke_len[mid], 100.0f, 1e-3f);
}

TEST(ShapeRaster, EllipseStrokeSMonotoneAndLength)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(0.0f, 0.0f);
  shape.half_size = float2(50.0f, 40.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 6.0f;

  rctf domain;
  BLI_rctf_init(&domain, -70.0f, 70.0f, -70.0f, 70.0f);
  const ShapeRasterOutputs outputs = ShapeRasterOutputs::Stroke | ShapeRasterOutputs::StrokeS;
  const ShapeEvaluator eval(Span<PaintShape>(&shape, 1), style, domain, outputs);

  /* The ellipse is densified for StrokeS; the coordinate grows with the parameter angle (the
   * flatten starts at the +x axis, counter-clockwise) and the full length is the perimeter. */
  const float a = 50.0f;
  const float b = 40.0f;
  float prev_s = -1.0f;
  float len = 0.0f;
  for (int i = 1; i <= 30; i++) {
    const float angle = 2.0f * float(M_PI) * (float(i) / 32.0f);
    const float2 p(a * math::cos(angle), b * math::sin(angle));
    const ShapeSample sample = eval.sample(p);
    EXPECT_GT(sample.stroke, 0.0f);
    EXPECT_GE(sample.stroke_s, prev_s - 0.01f);
    prev_s = sample.stroke_s;
    len = sample.stroke_len;
  }
  const float perimeter = float(M_PI) *
                          (3.0f * (a + b) - math::sqrt((3.0f * a + b) * (a + 3.0f * b)));
  EXPECT_NEAR(len, perimeter, perimeter * 0.01f);
}

TEST(ShapeRaster, RectShapeUVCornersAndRotation)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_RECT;
  shape.center = float2(50.0f, 50.0f);
  shape.half_size = float2(40.0f, 30.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;

  rctf domain;
  BLI_rctf_init(&domain, -100.0f, 100.0f, -100.0f, 100.0f);
  const ShapeRasterOutputs outputs = ShapeRasterOutputs::Fill | ShapeRasterOutputs::ShapeUV;

  /* The parametric frame corners map to (0,0) and (1,1). */
  const ShapeEvaluator eval(Span<PaintShape>(&shape, 1), style, domain, outputs);
  const ShapeSample lo = eval.sample(float2(10.0f, 20.0f));
  const ShapeSample hi = eval.sample(float2(90.0f, 80.0f));
  EXPECT_NEAR(lo.shape_uv.x, 0.0f, 1e-4);
  EXPECT_NEAR(lo.shape_uv.y, 0.0f, 1e-4);
  EXPECT_NEAR(hi.shape_uv.x, 1.0f, 1e-4);
  EXPECT_NEAR(hi.shape_uv.y, 1.0f, 1e-4);

  /* A quarter turn folds into the local frame: the local top-right corner (40, 30) lands at
   * world (20, 90) and the bottom-left (-40, -30) at (80, 10). */
  shape.rotation = float(M_PI) / 2.0f;
  const ShapeEvaluator eval_rot(Span<PaintShape>(&shape, 1), style, domain, outputs);
  const ShapeSample tr = eval_rot.sample(float2(20.0f, 90.0f));
  EXPECT_NEAR(tr.shape_uv.x, 1.0f, 1e-3);
  EXPECT_NEAR(tr.shape_uv.y, 1.0f, 1e-3);
  const ShapeSample bl = eval_rot.sample(float2(80.0f, 10.0f));
  EXPECT_NEAR(bl.shape_uv.x, 0.0f, 1e-3);
  EXPECT_NEAR(bl.shape_uv.y, 0.0f, 1e-3);
}

TEST(ShapeRaster, UnrequestedStrokeSAndShapeUVAreEmpty)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(60.0f, 60.0f);
  shape.half_size = float2(50.0f, 40.0f);

  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL | PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 8.0f;

  rcti rect;
  BLI_rcti_init(&rect, 0, 119, 0, 119);
  const ShapeRasterOutputs outputs = ShapeRasterOutputs::Fill | ShapeRasterOutputs::Stroke;
  ShapeRasterizer rasterizer(Span<PaintShape>(&shape, 1), style, rect, float2(0.0f), outputs);
  const ShapeCoverage cov = rasterizer.rasterize(rect);
  EXPECT_TRUE(cov.stroke_s.is_empty());
  EXPECT_TRUE(cov.stroke_len.is_empty());
  EXPECT_TRUE(cov.shape_uv.is_empty());
}

}  // namespace blender::ed::sculpt_paint::shape
