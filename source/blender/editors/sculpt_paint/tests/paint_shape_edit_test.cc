/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../paint_shape_edit.hh"

#include <algorithm>
#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

static ShapeStyle flat_style()
{
  ShapeStyle style;
  style.flag = PAINT_SHAPE_USE_FILL | PAINT_SHAPE_USE_STROKE;
  style.stroke_width = 8.0f;
  style.feather = 1.0f;
  return style;
}

static PaintShape test_ellipse()
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_ELLIPSE;
  shape.center = float2(60.0f, 60.0f);
  shape.half_size = float2(30.0f, 18.0f);
  return shape;
}

static PaintShape test_line()
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_LINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(10.0f, 50.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(110.0f, 50.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);
  return shape;
}

TEST(ShapeEdit, ContourHitHonorsOutlineNotBounds)
{
  /* The bbox corner of the ellipse is inside the bounds but outside the outline: the contour
   * test misses where the old bbox test grabbed. */
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_ellipse()});

  EXPECT_GE(session.hit_shape(float2(60.0f, 60.0f), style, 2.0f), 0);
  EXPECT_EQ(session.hit_shape(float2(200.0f, 200.0f), style, 2.0f), -1);
  EXPECT_EQ(session.hit_shape(float2(90.0f, 78.0f), style, 2.0f), -1);
  EXPECT_TRUE(session.hit_test(float2(60.0f, 60.0f), style, 2.0f));
}

TEST(ShapeEdit, StrokeOnlyLineHitWithTolerance)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  ShapeEditSession session(Vector<PaintShape>{test_line()});

  EXPECT_GE(session.hit_shape(float2(60.0f, 50.5f), style, 4.0f), 0);
  EXPECT_EQ(session.hit_shape(float2(60.0f, 80.0f), style, 4.0f), -1);
  /* 8 px off a 10 px stroke with feather 1: inside the 4 px tolerance ring, outside without. */
  EXPECT_GE(session.hit_shape(float2(60.0f, 58.0f), style, 4.0f), 0);
  EXPECT_EQ(session.hit_shape(float2(60.0f, 58.0f), style, 0.0f), -1);
}

TEST(ShapeEdit, SessionUndoRedo)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_ellipse()});

  /* Entry 0 is the initial state; a new state is recorded AFTER the change. */
  ASSERT_NE(session.active_for_write(), nullptr);
  shape_translate(*session.active_for_write(), float2(10.0f, 0.0f));
  session.undo_push();
  EXPECT_EQ(session.active()->center.x, 70.0f);

  EXPECT_TRUE(session.undo_pop());
  EXPECT_EQ(session.active()->center.x, 60.0f);
  EXPECT_TRUE(session.redo_can());
  EXPECT_TRUE(session.redo_pop());
  EXPECT_EQ(session.active()->center.x, 70.0f);

  /* A new state pushed after an undo drops the redo branch. */
  EXPECT_TRUE(session.undo_pop());
  shape_translate(*session.active_for_write(), float2(-15.0f, 0.0f));
  session.undo_push();
  EXPECT_EQ(session.active()->center.x, 45.0f);
  EXPECT_FALSE(session.redo_can());
  EXPECT_FALSE(session.redo_pop());

  /* Cancelling a drag restores the start pose without recording a new entry. */
  ASSERT_TRUE(session.drag_begin(float2(45.0f, 60.0f), style, 4.0f));
  ASSERT_TRUE(session.drag_update(float2(65.0f, 60.0f)));
  EXPECT_EQ(session.active()->center.x, 65.0f);
  session.drag_cancel();
  EXPECT_EQ(session.active()->center.x, 45.0f);
  EXPECT_FALSE(session.redo_can());
}

TEST(ShapeEdit, UndoCursorThroughGestures)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_line()});

  const auto p0 = [&]() { return session.active()->splines[0].points[0].co; };

  /* Gesture 1: move. */
  ASSERT_TRUE(session.move_active_begin(float2(60.0f, 50.0f)));
  ASSERT_TRUE(session.drag_update(float2(80.0f, 50.0f)));
  session.drag_end();
  const float2 s1 = p0();

  /* Gesture 2: quarter turn around the moved mean origin. */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Rotate, float2(180.0f, 50.0f), style));
  ASSERT_TRUE(session.transform_update(float2(80.0f, 150.0f)));
  session.transform_end();
  const float2 s2 = p0();

  /* Gesture 3: drag the first point. */
  ASSERT_TRUE(session.point_drag_begin(s2, 8.0f));
  ASSERT_TRUE(session.point_drag_update(s2 + float2(0.0f, 10.0f)));
  session.point_drag_end();
  const float2 s3 = p0();
  EXPECT_NE(s1.x, s2.x);
  EXPECT_NE(s2.y, s3.y);

  /* Three undos step through every intermediate state, three redos return. */
  EXPECT_TRUE(session.undo_pop());
  EXPECT_NEAR(p0().x, s2.x, 1e-4);
  EXPECT_NEAR(p0().y, s2.y, 1e-4);
  EXPECT_TRUE(session.undo_pop());
  EXPECT_NEAR(p0().x, s1.x, 1e-4);
  EXPECT_NEAR(p0().y, s1.y, 1e-4);
  EXPECT_TRUE(session.undo_pop());
  EXPECT_NEAR(p0().x, 10.0f, 1e-4);
  EXPECT_NEAR(p0().y, 50.0f, 1e-4);
  EXPECT_FALSE(session.undo_pop());

  EXPECT_TRUE(session.redo_pop());
  EXPECT_NEAR(p0().x, s1.x, 1e-4);
  EXPECT_TRUE(session.redo_pop());
  EXPECT_NEAR(p0().x, s2.x, 1e-4);
  EXPECT_TRUE(session.redo_pop());
  EXPECT_NEAR(p0().x, s3.x, 1e-4);
  EXPECT_TRUE(session.redo_pop() == false);
}

TEST(ShapeEdit, CancelDoesNotBreakRedo)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_line()});

  ASSERT_TRUE(session.move_active_begin(float2(60.0f, 50.0f)));
  ASSERT_TRUE(session.drag_update(float2(70.0f, 50.0f)));
  session.drag_end();
  EXPECT_TRUE(session.undo_pop());
  EXPECT_TRUE(session.redo_can());

  /* A cancelled drag records nothing and leaves the redo branch intact. */
  ASSERT_TRUE(session.move_active_begin(float2(10.0f, 50.0f)));
  ASSERT_TRUE(session.drag_update(float2(40.0f, 50.0f)));
  session.drag_cancel();
  EXPECT_TRUE(session.redo_can());
  EXPECT_TRUE(session.redo_pop());
  EXPECT_NEAR(session.active()->splines[0].points[0].co.x, 20.0f, 1e-4);
}

TEST(ShapeEdit, RotateParametric)
{
  const ShapeStyle style = flat_style();
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(100.0f, 100.0f);
  rect.half_size = float2(40.0f, 20.0f);
  ShapeEditSession session(Vector<PaintShape>{rect});

  /* The pivot is the bounds center; a quarter turn around it keeps the center. */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Rotate, float2(150.0f, 100.0f), style));
  EXPECT_TRUE(session.is_transforming());
  EXPECT_TRUE(session.transform_update(float2(100.0f, 150.0f)));
  session.transform_end();
  EXPECT_NEAR(session.active()->rotation, float(M_PI) / 2.0f, 1e-5);
  EXPECT_NEAR(session.active()->center.x, 100.0f, 1e-4);
  EXPECT_NEAR(session.active()->center.y, 100.0f, 1e-4);
  EXPECT_TRUE(session.undo_pop());
  EXPECT_NEAR(session.active()->rotation, 0.0f, 1e-6);
}

TEST(ShapeEdit, ScaleParametric)
{
  const ShapeStyle style = flat_style();
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(100.0f, 100.0f);
  rect.half_size = float2(40.0f, 20.0f);
  ShapeEditSession session(Vector<PaintShape>{rect});

  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Scale, float2(150.0f, 100.0f), style));
  EXPECT_TRUE(session.transform_update(float2(200.0f, 100.0f)));
  session.transform_end();
  EXPECT_NEAR(session.active()->half_size.x, 80.0f, 1e-4);
  EXPECT_NEAR(session.active()->half_size.y, 40.0f, 1e-4);
  EXPECT_NEAR(session.active()->center.x, 100.0f, 1e-4);
}

TEST(ShapeEdit, RotateSpline)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_line()});

  /* Pivot (60, 50): a quarter turn maps the horizontal line onto the vertical one. */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Rotate, float2(160.0f, 50.0f), style));
  EXPECT_TRUE(session.transform_update(float2(60.0f, 150.0f)));
  session.transform_end();
  const ShapeSpline &spline = session.active()->splines[0];
  ASSERT_EQ(spline.points.size(), 2);
  EXPECT_NEAR(spline.points[0].co.x, 60.0f, 1e-4);
  EXPECT_NEAR(spline.points[0].co.y, 0.0f, 1e-4);
  EXPECT_NEAR(spline.points[1].co.x, 60.0f, 1e-4);
  EXPECT_NEAR(spline.points[1].co.y, 100.0f, 1e-4);
}

TEST(ShapeEdit, ScaleSpline)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_line()});

  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Scale, float2(160.0f, 50.0f), style));
  EXPECT_TRUE(session.transform_update(float2(260.0f, 50.0f)));
  session.transform_end();
  const ShapeSpline &spline = session.active()->splines[0];
  ASSERT_EQ(spline.points.size(), 2);
  EXPECT_NEAR(spline.points[0].co.x, -40.0f, 1e-4);
  EXPECT_NEAR(spline.points[0].co.y, 50.0f, 1e-4);
  EXPECT_NEAR(spline.points[1].co.x, 160.0f, 1e-4);
  EXPECT_NEAR(spline.points[1].co.y, 50.0f, 1e-4);
}

TEST(ShapeEdit, ScaleSplineAxisLock)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_line()});

  /* Pure vertical cursor travel with an X lock changes nothing; doubling along X stretches
   * the horizontal line while its height stays. */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Scale, float2(160.0f, 50.0f), style));
  EXPECT_FALSE(session.transform_update(float2(160.0f, 150.0f), ShapeAxisLock::X));
  EXPECT_TRUE(session.transform_update(float2(260.0f, 50.0f), ShapeAxisLock::X));
  session.transform_end();
  const ShapeSpline &spline = session.active()->splines[0];
  EXPECT_NEAR(spline.points[0].co.x, -40.0f, 1e-4);
  EXPECT_NEAR(spline.points[0].co.y, 50.0f, 1e-4);
  EXPECT_NEAR(spline.points[1].co.x, 160.0f, 1e-4);
  EXPECT_NEAR(spline.points[1].co.y, 50.0f, 1e-4);
}

TEST(ShapeEdit, PointDragMovesHandlesWithPoint)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_POLYLINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(100.0f, 0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(
      ShapePoint{float2(100.0f, 100.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);
  ShapeEditSession session(Vector<PaintShape>{shape});

  ASSERT_TRUE(session.point_drag_begin(float2(102.0f, 2.0f), 8.0f));
  EXPECT_TRUE(session.is_point_dragging());
  /* The grab delta rides along, so the point lands at the cursor plus the grab offset. */
  EXPECT_TRUE(session.point_drag_update(float2(120.0f, 10.0f)));
  EXPECT_NEAR(session.active()->splines[0].points[1].co.x, 118.0f, 1e-4);
  EXPECT_NEAR(session.active()->splines[0].points[1].co.y, 8.0f, 1e-4);
  session.point_drag_end();
  EXPECT_TRUE(session.undo_pop());
  EXPECT_NEAR(session.active()->splines[0].points[1].co.x, 100.0f, 1e-4);

  EXPECT_FALSE(session.point_drag_begin(float2(500.0f, 500.0f), 8.0f));
}

TEST(ShapeEdit, DefaultOriginIsMeanNotBoundsCenter)
{
  /* A right triangle: the mean of the points is (66.667, 33.333), while the bounds center is
   * (50, 50). */
  PaintShape shape;
  shape.type = PAINT_SHAPE_POLYLINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(0.0f, 0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(100.0f, 0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(
      ShapePoint{float2(100.0f, 100.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);

  const float2 origin = shape_origin_default(shape);
  EXPECT_NEAR(origin.x, 200.0f / 3.0f, 1e-3);
  EXPECT_NEAR(origin.y, 100.0f / 3.0f, 1e-3);
  /* Non-custom: the effective origin is the same mean. */
  EXPECT_NEAR(shape_effective_origin(shape).x, 200.0f / 3.0f, 1e-3);
  EXPECT_NEAR(shape_effective_origin(shape).y, 100.0f / 3.0f, 1e-3);
}

TEST(ShapeEdit, AppendShapeIsUndoable)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session;
  session.append_shape(test_ellipse());
  session.append_shape(test_line());
  EXPECT_EQ(session.shapes_num(), 2);
  session.active_set(1);

  /* A cancelled gesture on the appended shape leaves both in place. */
  ASSERT_TRUE(session.move_active_begin(float2(60.0f, 50.0f)));
  ASSERT_TRUE(session.drag_update(float2(80.0f, 50.0f)));
  session.drag_cancel();
  EXPECT_EQ(session.shapes_num(), 2);

  /* Undo removes exactly the appended second shape. */
  EXPECT_TRUE(session.undo_pop());
  EXPECT_EQ(session.shapes_num(), 1);
}

TEST(ShapeEdit, ActiveChangeIsNotUndone)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session;
  session.append_shape(test_ellipse());
  session.append_shape(test_line());
  session.active_set(1);

  /* Commit a move of the second shape. */
  ASSERT_TRUE(session.move_active_begin(float2(60.0f, 50.0f)));
  ASSERT_TRUE(session.drag_update(float2(70.0f, 50.0f)));
  session.drag_end();

  /* Switching the active shape is live state, not history: undoing the move keeps it. */
  session.active_set(0);
  EXPECT_TRUE(session.undo_pop());
  EXPECT_EQ(session.active_index(), 0);
}

TEST(ShapeEdit, RotateAroundCustomOrigin)
{
  const ShapeStyle style = flat_style();
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(100.0f, 100.0f);
  rect.half_size = float2(40.0f, 20.0f);
  /* A custom pivot left of the center: the rotation must use it, not the bounds center. */
  rect.origin = float2(60.0f, 100.0f);
  rect.origin_is_custom = true;
  ShapeEditSession session(Vector<PaintShape>{rect});

  /* Quarter turn around (60, 100): the center (100, 100) moves to (60, 140). */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Rotate, float2(160.0f, 100.0f), style));
  EXPECT_TRUE(session.transform_update(float2(60.0f, 160.0f)));
  session.transform_end();
  EXPECT_NEAR(session.active()->rotation, float(M_PI) / 2.0f, 1e-5);
  EXPECT_NEAR(session.active()->center.x, 60.0f, 1e-4);
  EXPECT_NEAR(session.active()->center.y, 140.0f, 1e-4);
  /* The custom origin is the pivot, so it stays put. */
  EXPECT_NEAR(session.active()->origin.x, 60.0f, 1e-4);
  EXPECT_NEAR(session.active()->origin.y, 100.0f, 1e-4);
}

TEST(ShapeEdit, HandleAtPrefersActivePointThenOrigin)
{
  const ShapeStyle style = flat_style();
  ShapeEditSession session(Vector<PaintShape>{test_line()});
  /* test_line: points (10,50) and (110,50); the mean origin is (60,50). */
  ASSERT_EQ(session.active_index(), 0);

  const ShapeHandle at_point = session.handle_at(float2(10.0f, 50.0f), 4.0f, style);
  EXPECT_EQ(at_point.type, ShapeHandleType::Point);
  EXPECT_EQ(at_point.shape_index, 0);
  EXPECT_EQ(at_point.spline_index, 0);
  EXPECT_EQ(at_point.point_index, 0);

  /* The origin sits on the contour (midpoint of the line), but wins over the move handle. */
  const ShapeHandle at_origin = session.handle_at(float2(60.0f, 50.0f), 4.0f, style);
  EXPECT_EQ(at_origin.type, ShapeHandleType::Origin);
  EXPECT_EQ(at_origin.shape_index, 0);

  /* Away from point and origin, the contour grabs the whole shape. */
  const ShapeHandle at_move = session.handle_at(float2(40.0f, 50.0f), 4.0f, style);
  EXPECT_EQ(at_move.type, ShapeHandleType::Move);
  EXPECT_EQ(at_move.shape_index, 0);

  const ShapeHandle at_none = session.handle_at(float2(500.0f, 500.0f), 4.0f, style);
  EXPECT_EQ(at_none.type, ShapeHandleType::None);
  EXPECT_EQ(at_none.shape_index, -1);
}

TEST(ShapeEdit, PointThenOriginAcrossGestures)
{
  const ShapeStyle style = flat_style();
  PaintShape shape;
  shape.type = PAINT_SHAPE_POLYLINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(0.0f, 0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(100.0f, 0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(
      ShapePoint{float2(100.0f, 100.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);
  ShapeEditSession session(Vector<PaintShape>{shape});

  /* Step 1: the point handle wins at a control point. */
  const ShapeHandle h = session.handle_at(float2(100.0f, 0.0f), 6.0f, style);
  ASSERT_EQ(h.type, ShapeHandleType::Point);
  ASSERT_EQ(h.spline_index, 0);
  ASSERT_EQ(h.point_index, 1);

  /* Step 2: drag that point and commit one undo step. */
  ASSERT_TRUE(session.point_drag_begin(float2(100.0f, 0.0f), 6.0f));
  ASSERT_TRUE(session.point_drag_update(float2(100.0f, 20.0f)));
  session.point_drag_end();
  EXPECT_NEAR(session.active()->splines[0].points[1].co.y, 20.0f, 1e-4);

  /* Step 3: undo restores the point. */
  ASSERT_TRUE(session.undo_pop());
  EXPECT_NEAR(session.active()->splines[0].points[1].co.y, 0.0f, 1e-4);

  /* Step 4: the origin (mean of the points) is selectable and is not shadowed by a point. */
  const float2 origin = shape_effective_origin(*session.active());
  const ShapeHandle oh = session.handle_at(origin, 6.0f, style);
  EXPECT_EQ(oh.type, ShapeHandleType::Origin);

  /* Step 5: an origin-anchored quarter turn uses that origin as its pivot. */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Rotate, float2(origin.x + 10.0f, origin.y), style));
  ASSERT_TRUE(session.transform_update(float2(origin.x, origin.y + 10.0f)));
  session.transform_end();
  /* (0,0) rotates a quarter turn around (66.667, 33.333) to (100, -33.333). */
  EXPECT_NEAR(session.active()->splines[0].points[0].co.x, 100.0f, 1e-3);
  EXPECT_NEAR(session.active()->splines[0].points[0].co.y, -33.333f, 1e-3);
  /* The mean equals the pivot, so it does not move under the rotation. */
  EXPECT_NEAR(shape_effective_origin(*session.active()).x, origin.x, 1e-3);
  EXPECT_NEAR(shape_effective_origin(*session.active()).y, origin.y, 1e-3);

  /* Step 6: undo the rotation; the point returns. */
  ASSERT_TRUE(session.undo_pop());
  EXPECT_NEAR(session.active()->splines[0].points[0].co.x, 0.0f, 1e-3);
  EXPECT_NEAR(session.active()->splines[0].points[0].co.y, 0.0f, 1e-3);
}

TEST(ShapeEdit, CurvePatchItemSurvivesDocumentUndo)
{
  /* R9.2: a CurvePatch item lives in #VectorDocument, so its undo history is the document's. Only
   * the user-editable fields (control_curve, params) are snapshotted. */
  VectorItem item;
  item.type = VectorItemType::CurvePatch;
  item.params.radius = 1.0f;
  item.params.stamp_seed = 42;

  VectorDocument doc;
  vector_document_set_items(doc, {std::move(item)});
  ASSERT_EQ(doc.items.size(), 1);
  EXPECT_EQ(doc.items[0].type, VectorItemType::CurvePatch);
  EXPECT_FLOAT_EQ(doc.items[0].params.radius, 1.0f);

  doc.items[0].params.radius = 2.0f;
  vector_document_undo_push(doc);
  doc.items[0].params.radius = 9.0f;
  vector_document_undo_push(doc);

  ASSERT_TRUE(vector_document_undo_back(doc));
  EXPECT_FLOAT_EQ(doc.items[0].params.radius, 2.0f);
  EXPECT_EQ(doc.items[0].params.stamp_seed, 42);
  ASSERT_TRUE(vector_document_undo_back(doc));
  EXPECT_FLOAT_EQ(doc.items[0].params.radius, 1.0f);
  /* Back at the start of an untrimmed history: nothing more to undo. */
  EXPECT_FALSE(vector_document_undo_back(doc));

  ASSERT_TRUE(vector_document_undo_forward(doc));
  EXPECT_FLOAT_EQ(doc.items[0].params.radius, 2.0f);
  ASSERT_TRUE(vector_document_undo_forward(doc));
  EXPECT_FLOAT_EQ(doc.items[0].params.radius, 9.0f);
}

TEST(ShapeEdit, CurvePatchUndoBackDoesNotCancelWhenTrimmed)
{
  /* Curve Patch semantics: once the oldest step was trimmed, "back at 0" reports success (there is
   * simply nothing earlier) instead of "cancel the patch". */
  VectorItem item;
  item.type = VectorItemType::CurvePatch;

  VectorDocument doc;
  vector_document_set_items(doc, {std::move(item)});
  doc.undo_anchor_trimmed = true;
  EXPECT_TRUE(vector_document_undo_back(doc));
}

TEST(ShapeEdit, RotateAboutCustomOriginKeepsOriginPivot)
{
  /* A custom origin is the pivot: rotating about it leaves the origin, carries the center around
   * it and accumulates the parametric rotation. */
  PaintShape ellipse = test_ellipse();
  ellipse.origin = float2(0.0f, 0.0f);
  ellipse.origin_is_custom = true;

  shape_rotate_about(ellipse, shape_effective_origin(ellipse), float(M_PI) / 2.0f);

  EXPECT_NEAR(ellipse.origin.x, 0.0f, 1e-4);
  EXPECT_NEAR(ellipse.origin.y, 0.0f, 1e-4);
  /* (60, 60) turned a quarter turn about the origin becomes (-60, 60). */
  EXPECT_NEAR(ellipse.center.x, -60.0f, 1e-4);
  EXPECT_NEAR(ellipse.center.y, 60.0f, 1e-4);
  EXPECT_NEAR(ellipse.rotation, float(M_PI) / 2.0f, 1e-5);
}

TEST(ShapeEdit, RotateAboutDefaultOriginKeepsCenter)
{
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(100.0f, 100.0f);
  rect.half_size = float2(40.0f, 20.0f);

  shape_rotate_about(rect, shape_effective_origin(rect), float(M_PI) / 2.0f);

  EXPECT_NEAR(rect.center.x, 100.0f, 1e-4);
  EXPECT_NEAR(rect.center.y, 100.0f, 1e-4);
  EXPECT_NEAR(rect.rotation, float(M_PI) / 2.0f, 1e-5);
}

TEST(ShapeEdit, RotateSplineAboutMean)
{
  PaintShape line = test_line();
  shape_rotate_about(line, shape_effective_origin(line), float(M_PI));

  ASSERT_EQ(line.splines.size(), 1);
  /* The ends swap around the (60, 50) midpoint. */
  EXPECT_NEAR(line.splines[0].points[0].co.x, 110.0f, 1e-4);
  EXPECT_NEAR(line.splines[0].points[0].co.y, 50.0f, 1e-4);
  EXPECT_NEAR(line.splines[0].points[1].co.x, 10.0f, 1e-4);
  EXPECT_NEAR(line.splines[0].points[1].co.y, 50.0f, 1e-4);
}

TEST(ShapeEdit, SetSizeKeepsCenterOriginAndClampsCorners)
{
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(50.0f, 50.0f);
  rect.half_size = float2(100.0f, 100.0f);
  rect.corner_radius = float4(80.0f);
  rect.origin = float2(50.0f, 50.0f);
  rect.origin_is_custom = true;

  shape_set_size(rect, float2(40.0f, 20.0f));

  EXPECT_NEAR(rect.half_size.x, 20.0f, 1e-4);
  EXPECT_NEAR(rect.half_size.y, 10.0f, 1e-4);
  EXPECT_NEAR(rect.center.x, 50.0f, 1e-4);
  EXPECT_NEAR(rect.center.y, 50.0f, 1e-4);
  EXPECT_NEAR(rect.origin.x, 50.0f, 1e-4);
  /* Corners clamp to min(half) = 10. */
  EXPECT_NEAR(rect.corner_radius.x, 10.0f, 1e-4);
  EXPECT_NEAR(rect.corner_radius.w, 10.0f, 1e-4);

  /* A spline shape ignores the size setter. */
  PaintShape line = test_line();
  shape_set_size(line, float2(10.0f, 10.0f));
  EXPECT_EQ(line.splines.size(), 1);
}

TEST(ShapeEdit, SetSizeAxisIndependent)
{
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.half_size = float2(50.0f, 30.0f);

  /* Editing one axis leaves the other untouched (the settings carry both, so the unchanged Y
   * round-trips exactly). */
  shape_set_size(rect, float2(80.0f, 60.0f));
  EXPECT_NEAR(rect.half_size.x, 40.0f, 1e-4);
  EXPECT_NEAR(rect.half_size.y, 30.0f, 1e-4);
}

TEST(ShapeEdit, ScaleFreeParametricAxisIndependent)
{
  const ShapeStyle style = flat_style();
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(100.0f, 100.0f);
  rect.half_size = float2(40.0f, 20.0f);
  ShapeEditSession session(Vector<PaintShape>{rect});

  /* Grab the right-middle handle at (140, 100) and double X; Y has no grab extent, so it stays. */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::ScaleFree, float2(140.0f, 100.0f), style));
  EXPECT_TRUE(session.transform_update(float2(180.0f, 100.0f)));
  session.transform_end();
  EXPECT_NEAR(session.active()->half_size.x, 80.0f, 1e-4);
  EXPECT_NEAR(session.active()->half_size.y, 20.0f, 1e-4);
  EXPECT_NEAR(session.active()->center.x, 100.0f, 1e-4);
}

TEST(ShapeEdit, ScaleFreeFollowsLocalAxes)
{
  const ShapeStyle style = flat_style();
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(0.0f, 0.0f);
  rect.half_size = float2(10.0f, 5.0f);
  /* A quarter turn puts the shape's local +X along the world +Y. */
  rect.rotation = float(M_PI) / 2.0f;
  ShapeEditSession session(Vector<PaintShape>{rect});

  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::ScaleFree, float2(0.0f, 10.0f), style));
  EXPECT_TRUE(session.transform_update(float2(0.0f, 20.0f)));
  session.transform_end();
  /* The drag scales the local X (its own width) alone. */
  EXPECT_NEAR(session.active()->half_size.x, 20.0f, 1e-4);
  EXPECT_NEAR(session.active()->half_size.y, 5.0f, 1e-4);
}

TEST(ShapeEdit, ScaleFreeEdgeHandleUsesLocalAxis)
{
  const ShapeStyle style = flat_style();
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(0.0f, 10.0f);
  rect.half_size = float2(10.0f, 5.0f);
  /* Local +X points along world +Y; a custom origin is the pivot. */
  rect.rotation = float(M_PI) / 2.0f;
  rect.origin = float2(0.0f, 0.0f);
  rect.origin_is_custom = true;
  ShapeEditSession session(Vector<PaintShape>{rect});

  /* The local +X side handle is at center + R*(half.x, 0) = (0, 20). */
  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::ScaleFree, float2(0.0f, 20.0f), style, ShapeScaleAxis::X));
  EXPECT_TRUE(session.transform_update(float2(0.0f, 40.0f)));
  session.transform_end();
  EXPECT_NEAR(session.active()->half_size.x, 20.0f, 1e-4);
  EXPECT_NEAR(session.active()->half_size.y, 5.0f, 1e-4);
  /* The center orbits the origin along the local X (world +Y). */
  EXPECT_NEAR(session.active()->center.x, 0.0f, 1e-4);
  EXPECT_NEAR(session.active()->center.y, 20.0f, 1e-4);
}

TEST(ShapeEdit, ScaleAxisLockUsesGlobalAxes)
{
  const ShapeStyle style = flat_style();
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(0.0f, 10.0f);
  rect.half_size = float2(10.0f, 5.0f);
  rect.rotation = float(M_PI) / 2.0f;
  rect.origin = float2(0.0f, 0.0f);
  rect.origin_is_custom = true;
  ShapeEditSession session(Vector<PaintShape>{rect});

  ASSERT_TRUE(session.transform_begin(
      ShapeTransformMode::Scale, float2(10.0f, 10.0f), style));
  EXPECT_TRUE(session.transform_update(float2(20.0f, 10.0f), ShapeAxisLock::X));
  session.transform_end();
  /* The X lock scales along the GLOBAL X, so the center keeps its world position (a local frame
   * would orbit it around the origin). */
  EXPECT_NEAR(session.active()->center.x, 0.0f, 1e-4);
  EXPECT_NEAR(session.active()->center.y, 10.0f, 1e-4);
  EXPECT_NEAR(session.active()->half_size.x, 20.0f, 1e-4);
}

}  // namespace blender::ed::sculpt_paint::shape
