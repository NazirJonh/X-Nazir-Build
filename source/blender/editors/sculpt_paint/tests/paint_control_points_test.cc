/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "ED_control_points.hh"

#include <cfloat>

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DNA_curve_types.h"

#include "ED_paint_curve_draw.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::control_points::tests {

/** A hand-built source: every point is described by value, nothing is derived. */
class TestSource : public Source {
 public:
  struct Point {
    float2 position;
    float2 left;
    float2 right;
    int8_t type_left = BEZIER_HANDLE_FREE;
    int8_t type_right = BEZIER_HANDLE_FREE;
    uint8_t selection = 0;
    bool left_visible = true;
    bool right_visible = true;
  };

  Vector<Point> points;

  int points_num() const override
  {
    return int(points.size());
  }
  float2 point_position(const int point) const override
  {
    return points[point].position;
  }
  void point_handles(const int point, float2 &r_left, float2 &r_right) const override
  {
    r_left = points[point].left;
    r_right = points[point].right;
  }
  int8_t handle_type(const int point, const bool left) const override
  {
    return left ? points[point].type_left : points[point].type_right;
  }
  uint8_t selection(const int point) const override
  {
    return points[point].selection;
  }
  bool handle_visible(const int point, const bool left) const override
  {
    return left ? points[point].left_visible : points[point].right_visible;
  }
  int segments_num() const override
  {
    return 0;
  }
  void segment_polyline(const int /*segment*/, Vector<float2> & /*r_out*/) const override {}
};

static TestSource::Point point_at(const float2 position)
{
  TestSource::Point point;
  point.position = position;
  point.left = position - float2(20.0f, 0.0f);
  point.right = position + float2(20.0f, 0.0f);
  return point;
}

static TestSource single_point_source()
{
  TestSource source;
  source.points.append(point_at(float2(50.0f, 50.0f)));
  return source;
}

TEST(ControlPoints, PickPointClosestPartWinsAndPivotKeepsTies)
{
  const TestSource source = single_point_source();

  const Pick on_pivot = pick_point(source, float2(50.0f, 50.0f), 10.0f, Metric::Manhattan);
  EXPECT_EQ(on_pivot.point, 0);
  EXPECT_EQ(on_pivot.part, Part::Point);

  const Pick on_left = pick_point(source, float2(32.0f, 50.0f), 10.0f, Metric::Manhattan);
  EXPECT_EQ(on_left.point, 0);
  EXPECT_EQ(on_left.part, Part::Left);

  const Pick on_right = pick_point(source, float2(69.0f, 51.0f), 10.0f, Metric::Manhattan);
  EXPECT_EQ(on_right.part, Part::Right);

  const Pick miss = pick_point(source, float2(50.0f, 90.0f), 10.0f, Metric::Manhattan);
  EXPECT_EQ(miss.point, -1);
  EXPECT_EQ(miss.part, Part::None);
}

TEST(ControlPoints, PickPointIgnorePivotRedirectsToTheNearerHandle)
{
  const TestSource source = single_point_source();

  /* Slightly left of the pivot: the pivot still wins the hit, then the tangent tool redirects it
   * to the nearer (left) handle. */
  const Pick redirected = pick_point(
      source, float2(49.0f, 50.0f), 10.0f, Metric::Manhattan, /*ignore_pivot=*/true);
  EXPECT_EQ(redirected.point, 0);
  EXPECT_EQ(redirected.part, Part::Left);

  const Pick plain = pick_point(source, float2(49.0f, 50.0f), 10.0f, Metric::Manhattan);
  EXPECT_EQ(plain.part, Part::Point);
}

TEST(ControlPoints, PickPointSkipsHiddenHandles)
{
  TestSource source = single_point_source();
  source.points[0].left_visible = false;

  /* On the hidden left knob: only the pivot (20 px away) and the right handle are candidates. */
  const Pick hidden = pick_point(source, float2(30.0f, 50.0f), 5.0f, Metric::Manhattan);
  EXPECT_EQ(hidden.point, -1);

  const Pick visible = pick_point(source, float2(70.0f, 50.0f), 5.0f, Metric::Manhattan);
  EXPECT_EQ(visible.part, Part::Right);
}

TEST(ControlPoints, PickPointMetricChangesTheReach)
{
  const TestSource source = single_point_source();
  /* (7, 7) off the pivot: Manhattan 14, Euclidean ~9.9. */
  const float2 pos(57.0f, 57.0f);
  EXPECT_EQ(pick_point(source, pos, 12.0f, Metric::Manhattan).point, -1);
  EXPECT_EQ(pick_point(source, pos, 12.0f, Metric::Euclidean).part, Part::Point);
}

TEST(ControlPoints, PickPointPicksTheClosestOfSeveralPoints)
{
  TestSource source;
  source.points.append(point_at(float2(50.0f, 50.0f)));
  source.points.append(point_at(float2(150.0f, 50.0f)));
  const Pick pick = pick_point(source, float2(148.0f, 50.0f), 10.0f, Metric::Euclidean);
  EXPECT_EQ(pick.point, 1);
  EXPECT_EQ(pick.part, Part::Point);
}

TEST(ControlPoints, HandlesBuildMapsLeftAndRightConsistently)
{
  /* Guards the left / right convention of the Source interface: `left == true` is the left
   * handle for the type AND for the visibility. */
  TestSource source = single_point_source();
  source.points[0].type_left = BEZIER_HANDLE_AUTO;
  source.points[0].type_right = BEZIER_HANDLE_VECTOR;
  source.points[0].left_visible = false;

  BuildOptions options;
  options.compute_colors = false;
  PaintCurveScreenHandles handles;
  handles_build(source, options, handles);

  ASSERT_EQ(handles.points.size(), 1);
  const PaintCurveHandleDrawData &hd = handles.points[0];
  EXPECT_EQ(hd.h1, BEZIER_HANDLE_AUTO);
  EXPECT_EQ(hd.h2, BEZIER_HANDLE_VECTOR);
  EXPECT_FALSE(hd.show_handle_left);
  EXPECT_TRUE(hd.show_handle_right);
  EXPECT_EQ(hd.handle_left, float2(30.0f, 50.0f));
  EXPECT_EQ(hd.handle_right, float2(70.0f, 50.0f));
}

TEST(ControlPoints, HandlesBuildSelectionOfThePivotHighlightsBothHandles)
{
  TestSource source = single_point_source();
  source.points[0].selection = SEL_F2;

  BuildOptions options;
  options.compute_colors = false;
  PaintCurveScreenHandles handles;
  handles_build(source, options, handles);
  EXPECT_TRUE(handles.points[0].selected_center);
  EXPECT_TRUE(handles.points[0].selected_left);
  EXPECT_TRUE(handles.points[0].selected_right);

  source.points[0].selection = SEL_F1;
  handles_build(source, options, handles);
  EXPECT_FALSE(handles.points[0].selected_center);
  EXPECT_TRUE(handles.points[0].selected_left);
  EXPECT_FALSE(handles.points[0].selected_right);
}

TEST(ControlPoints, HandlesBuildHoverMarksOnlyThePivotUnderTheCursor)
{
  TestSource source = single_point_source();

  BuildOptions options;
  options.compute_colors = false;
  options.hover_point = true;
  options.cursor = float2(51.0f, 50.0f);
  PaintCurveScreenHandles handles;
  handles_build(source, options, handles);
  EXPECT_TRUE(handles.points[0].hovered_center);

  /* A hover over a handle knob does not mark the pivot. */
  options.cursor = float2(31.0f, 50.0f);
  handles_build(source, options, handles);
  EXPECT_FALSE(handles.points[0].hovered_center);
}

TEST(ControlPoints, HandleIsCollapsedOnlyWhenOnThePivot)
{
  EXPECT_TRUE(handle_is_collapsed(float2(5.0f, 5.0f), float2(5.0f, 5.0f)));
  EXPECT_FALSE(handle_is_collapsed(float2(5.0f, 5.0f), float2(5.1f, 5.0f)));
}

TEST(ControlPoints, PartToSelectionFlag)
{
  EXPECT_EQ(part_to_sel_flag(Part::Left), SEL_F1);
  EXPECT_EQ(part_to_sel_flag(Part::Point), SEL_F2);
  EXPECT_EQ(part_to_sel_flag(Part::Right), SEL_F3);
  EXPECT_EQ(part_to_sel_flag(Part::None), 0);
}

TEST(PolylineClosest, ArcLengthAndVertexParameterDiffer)
{
  /* Two segments of length 10 and 30. The closest point (10, 20) sits 2/3 into the second one:
   * the arc-length fraction is 30 / 40, the vertex-index parameter (1 + 2/3) / 2. */
  const Vector<float2> polyline = {float2(0.0f, 0.0f), float2(10.0f, 0.0f), float2(10.0f, 30.0f)};
  PaintCurvePolylineHit hit;
  ASSERT_TRUE(ED_paint_curve_polyline_closest(polyline, float2(10.0f, 20.0f), hit));
  EXPECT_NEAR(hit.dist_sq, 0.0f, 1e-6f);
  EXPECT_NEAR(hit.point.x, 10.0f, 1e-5f);
  EXPECT_NEAR(hit.point.y, 20.0f, 1e-5f);
  EXPECT_NEAR(hit.tangent.x, 0.0f, 1e-5f);
  EXPECT_NEAR(hit.tangent.y, 1.0f, 1e-5f);
  EXPECT_NEAR(hit.arc_t, 0.75f, 1e-5f);
  EXPECT_NEAR(hit.param_t, (1.0f + 2.0f / 3.0f) / 2.0f, 1e-5f);
}

TEST(PolylineClosest, ClampsToTheEndPointsAndReportsTheDistance)
{
  const Vector<float2> polyline = {float2(0.0f, 0.0f), float2(10.0f, 0.0f)};
  PaintCurvePolylineHit hit;
  ASSERT_TRUE(ED_paint_curve_polyline_closest(polyline, float2(-3.0f, 4.0f), hit));
  EXPECT_NEAR(hit.dist_sq, 25.0f, 1e-5f);
  EXPECT_NEAR(hit.arc_t, 0.0f, 1e-6f);
  EXPECT_NEAR(hit.param_t, 0.0f, 1e-6f);
}

TEST(PolylineClosest, TooShortOrDegeneratePolylines)
{
  PaintCurvePolylineHit hit;
  EXPECT_FALSE(ED_paint_curve_polyline_closest(Vector<float2>{float2(1.0f)}, float2(0.0f), hit));
  EXPECT_EQ(ED_paint_curve_polyline_distance_sq(Vector<float2>{}, float2(0.0f)), FLT_MAX);

  /* Coincident vertices: no division by zero, zero length leaves the parameters at 0. */
  const Vector<float2> collapsed = {float2(2.0f, 2.0f), float2(2.0f, 2.0f)};
  ASSERT_TRUE(ED_paint_curve_polyline_closest(collapsed, float2(5.0f, 2.0f), hit));
  EXPECT_NEAR(hit.dist_sq, 9.0f, 1e-5f);
  EXPECT_EQ(hit.arc_t, 0.0f);
}

TEST(PolylineClosest, AtParamIsTheInverseOfTheVertexParameter)
{
  const Vector<float2> polyline = {float2(0.0f, 0.0f), float2(10.0f, 0.0f), float2(10.0f, 30.0f)};
  PaintCurvePolylineHit hit;
  ASSERT_TRUE(ED_paint_curve_polyline_closest(polyline, float2(11.0f, 20.0f), hit));

  float2 point, tangent;
  ASSERT_TRUE(ED_paint_curve_polyline_at_param(polyline, hit.param_t, point, tangent));
  EXPECT_NEAR(point.x, hit.point.x, 1e-4f);
  EXPECT_NEAR(point.y, hit.point.y, 1e-4f);
  EXPECT_NEAR(tangent.y, 1.0f, 1e-5f);

  /* The ends clamp, and a parameter outside 0..1 does too. */
  ASSERT_TRUE(ED_paint_curve_polyline_at_param(polyline, 2.0f, point, tangent));
  EXPECT_NEAR(point.y, 30.0f, 1e-4f);
  EXPECT_FALSE(ED_paint_curve_polyline_at_param(Vector<float2>{}, 0.5f, point, tangent));
}

TEST(ControlPoints, PickSegmentChoosesTheNearestPolyline)
{
  Vector<PaintCurveSegmentDrawData> segments(2);
  segments[0].polyline = {float2(0.0f, 0.0f), float2(100.0f, 0.0f)};
  segments[1].polyline = {float2(0.0f, 50.0f), float2(100.0f, 50.0f)};

  int index = -1;
  float t = -1.0f;
  float dist_sq = -1.0f;
  ASSERT_TRUE(pick_segment(segments, float2(25.0f, 45.0f), 10.0f, index, t, &dist_sq));
  EXPECT_EQ(index, 1);
  EXPECT_NEAR(t, 0.25f, 1e-5f);
  EXPECT_NEAR(dist_sq, 25.0f, 1e-4f);

  EXPECT_FALSE(pick_segment(segments, float2(25.0f, 25.0f), 10.0f, index, t));
  EXPECT_EQ(index, -1);
}

}  // namespace blender::ed::sculpt_paint::control_points::tests
