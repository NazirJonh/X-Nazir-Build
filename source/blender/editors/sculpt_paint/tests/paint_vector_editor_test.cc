/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../paint_vector_editor.hh"

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"

#include "../paint_shape_op_props.hh" /* PAINT_SHAPE_MODAL_* */

#include "WM_types.hh" /* wmEvent, KM_PRESS, LEFTMOUSE, EVT_MODAL_MAP */

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

/** A minimal target for the editor tests: a real #ShapeEditSession and a restamp counter. The
 * write backend is never exercised (the editor does not commit); \a C is null throughout. */
class TestVectorHost : public VectorEditHost {
 public:
  TestVectorHost(Vector<PaintShape> shapes, const float tolerance_px)
      : edit_(std::move(shapes)), tolerance_px_(tolerance_px)
  {
    style_.flag = PAINT_SHAPE_USE_FILL | PAINT_SHAPE_USE_STROKE;
    style_.stroke_width = 8.0f;
    style_.feather = 1.0f;
  }

  VectorDocument &document() override
  {
    return edit_.document();
  }
  ShapeEditSession &edit() override
  {
    return edit_;
  }
  const ShapeStyle &style() const override
  {
    return style_;
  }
  PaintShapeSettings &settings() override
  {
    return settings_;
  }
  float hit_tolerance_px() const override
  {
    return tolerance_px_;
  }
  int ref_tile() const override
  {
    return 1001;
  }
  int2 ref_tile_size() const override
  {
    return int2(1024);
  }
  void restamp(bContext * /*C*/) override
  {
    restamp_count++;
  }
  bool settings_apply(bContext * /*C*/, bool /*is_width*/, float /*value*/) override
  {
    return false;
  }
  void redraw(bContext * /*C*/) override
  {
    redraw_count++;
  }

  int restamp_count = 0;
  int redraw_count = 0;

 private:
  ShapeEditSession edit_;
  ShapeStyle style_;
  PaintShapeSettings settings_{};
  float tolerance_px_ = 0.0f;
};

/** A three-point open polyline: points (0,0), (100,0), (100,100), mean origin (66.667, 33.333). */
static PaintShape test_polyline()
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_POLYLINE;
  ShapeSpline spline;
  spline.is_bezier = false;
  spline.points.append(ShapePoint{float2(0.0f, 0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(ShapePoint{float2(100.0f, 0.0f), float2(0), float2(0), false, 1.0f, false});
  spline.points.append(
      ShapePoint{float2(100.0f, 100.0f), float2(0), float2(0), false, 1.0f, false});
  shape.splines.append(spline);
  return shape;
}

static wmEvent mouse_event(const wmEventType type, const short val, const float2 &p)
{
  wmEvent event{};
  event.type = type;
  event.val = val;
  event.mval[0] = int(p.x);
  event.mval[1] = int(p.y);
  return event;
}

TEST(VectorEditor, PointPickDragUndoAcrossEvents)
{
  TestVectorHost host(Vector<PaintShape>{test_polyline()}, 6.0f);
  ShapeVectorEditor editor;

  /* Press on a control point picks it and starts a point drag. */
  const wmEvent press = mouse_event(LEFTMOUSE, KM_PRESS, float2(100.0f, 0.0f));
  EXPECT_EQ(editor.handle_event(nullptr, press, float2(100.0f, 0.0f), true, host),
            ShapeVectorEditor::Status::Handled);
  EXPECT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::PointDrag);
  EXPECT_EQ(host.restamp_count, 0);

  /* Move drags the picked point and re-stamps the preview once. */
  const wmEvent move = mouse_event(MOUSEMOVE, 0, float2(100.0f, 20.0f));
  EXPECT_EQ(editor.handle_event(nullptr, move, float2(100.0f, 20.0f), true, host),
            ShapeVectorEditor::Status::Handled);
  EXPECT_NEAR(host.edit().active()->splines[0].points[1].co.y, 20.0f, 1e-4);
  EXPECT_EQ(host.restamp_count, 1);

  /* Release keeps the pose. */
  const wmEvent release = mouse_event(LEFTMOUSE, KM_RELEASE, float2(100.0f, 20.0f));
  EXPECT_EQ(editor.handle_event(nullptr, release, float2(100.0f, 20.0f), true, host),
            ShapeVectorEditor::Status::Handled);
  EXPECT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::None);

  /* The session-local undo steps the point drag back. */
  EXPECT_TRUE(editor.undo(nullptr, host));
  EXPECT_NEAR(host.edit().active()->splines[0].points[1].co.y, 0.0f, 1e-4);
}

TEST(VectorEditor, ContourPressMovesWholeShape)
{
  TestVectorHost host(Vector<PaintShape>{test_polyline()}, 6.0f);
  ShapeVectorEditor editor;

  /* A press on a segment (away from point and origin) grabs the whole shape. */
  const wmEvent press = mouse_event(LEFTMOUSE, KM_PRESS, float2(40.0f, 0.0f));
  ASSERT_EQ(editor.handle_event(nullptr, press, float2(40.0f, 0.0f), true, host),
            ShapeVectorEditor::Status::Handled);
  ASSERT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::MovePick);

  const wmEvent move = mouse_event(MOUSEMOVE, 0, float2(50.0f, 10.0f));
  EXPECT_EQ(editor.handle_event(nullptr, move, float2(50.0f, 10.0f), true, host),
            ShapeVectorEditor::Status::Handled);
  EXPECT_NEAR(host.edit().active()->splines[0].points[0].co.x, 10.0f, 1e-4);
  EXPECT_NEAR(host.edit().active()->splines[0].points[0].co.y, 10.0f, 1e-4);
}

TEST(VectorEditor, OriginDragMovesMarkerWithoutRestamp)
{
  TestVectorHost host(Vector<PaintShape>{test_polyline()}, 6.0f);
  ShapeVectorEditor editor;
  const float2 origin = shape_effective_origin(*host.edit().active());

  /* Press on the marker starts an origin drag. */
  const wmEvent press = mouse_event(LEFTMOUSE, KM_PRESS, origin);
  EXPECT_EQ(editor.handle_event(nullptr, press, origin, true, host),
            ShapeVectorEditor::Status::Handled);
  EXPECT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::OriginDrag);

  /* Moving the marker redraws the overlay but must NOT re-stamp the composited preview. */
  const float2 moved = origin + float2(20.0f, 0.0f);
  const wmEvent move = mouse_event(MOUSEMOVE, 0, moved);
  EXPECT_EQ(editor.handle_event(nullptr, move, moved, true, host),
            ShapeVectorEditor::Status::Handled);
  EXPECT_EQ(host.restamp_count, 0);
  EXPECT_EQ(host.redraw_count, 1);
  EXPECT_TRUE(host.edit().active()->origin_is_custom);
  EXPECT_NEAR(shape_effective_origin(*host.edit().active()).x, moved.x, 1e-4);

  /* Release records one undo step; undoing restores the computed origin. */
  const wmEvent release = mouse_event(LEFTMOUSE, KM_RELEASE, moved);
  editor.handle_event(nullptr, release, moved, true, host);
  EXPECT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::None);
  EXPECT_TRUE(editor.undo(nullptr, host));
  EXPECT_FALSE(host.edit().active()->origin_is_custom);
}

TEST(VectorEditor, OriginDragRotateUndoAcrossSteps)
{
  TestVectorHost host(Vector<PaintShape>{test_polyline()}, 6.0f);
  ShapeVectorEditor editor;
  const float2 origin0 = shape_effective_origin(*host.edit().active());

  /* Step 1: drag the origin. */
  wmEvent press = mouse_event(LEFTMOUSE, KM_PRESS, origin0);
  editor.handle_event(nullptr, press, origin0, true, host);
  const float2 origin1 = origin0 + float2(20.0f, 0.0f);
  wmEvent move = mouse_event(MOUSEMOVE, 0, origin1);
  editor.handle_event(nullptr, move, origin1, true, host);
  wmEvent release = mouse_event(LEFTMOUSE, KM_RELEASE, origin1);
  editor.handle_event(nullptr, release, origin1, true, host);
  ASSERT_TRUE(host.edit().active()->origin_is_custom);

  /* Step 2: a quarter turn around the new origin. Park the cursor away from the pivot first so
   * the rotation has a start direction. */
  const float2 rot_start = origin1 + float2(30.0f, 0.0f);
  const wmEvent park = mouse_event(MOUSEMOVE, 0, rot_start);
  editor.handle_event(nullptr, park, rot_start, true, host);

  wmEvent rkey{};
  rkey.type = EVT_MODAL_MAP;
  rkey.val = short(PAINT_SHAPE_MODAL_ROTATE);
  ASSERT_EQ(editor.handle_event(nullptr, rkey, rot_start, true, host),
            ShapeVectorEditor::Status::Handled);
  ASSERT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::Rotate);
  const float2 rot_end = origin1 + float2(0.0f, 30.0f);
  const wmEvent rot_move = mouse_event(MOUSEMOVE, 0, rot_end);
  editor.handle_event(nullptr, rot_move, rot_end, true, host);
  const wmEvent rot_release = mouse_event(LEFTMOUSE, KM_RELEASE, rot_end);
  editor.handle_event(nullptr, rot_release, rot_end, true, host);

  /* The pivot is the custom origin, so the rotation keeps it. */
  EXPECT_NEAR(shape_effective_origin(*host.edit().active()).x, origin1.x, 1e-4);

  /* Undo once reverts the rotation, keeping the custom origin. */
  EXPECT_TRUE(editor.undo(nullptr, host));
  EXPECT_TRUE(host.edit().active()->origin_is_custom);

  /* Undo again reverts the origin drag. */
  EXPECT_TRUE(editor.undo(nullptr, host));
  EXPECT_FALSE(host.edit().active()->origin_is_custom);
}

TEST(VectorEditor, OriginResetCommandIsUndoable)
{
  TestVectorHost host(Vector<PaintShape>{test_polyline()}, 6.0f);
  ShapeVectorEditor editor;
  const float2 origin0 = shape_effective_origin(*host.edit().active());

  /* Make the origin custom. */
  wmEvent press = mouse_event(LEFTMOUSE, KM_PRESS, origin0);
  editor.handle_event(nullptr, press, origin0, true, host);
  const float2 origin1 = origin0 + float2(15.0f, 5.0f);
  wmEvent move = mouse_event(MOUSEMOVE, 0, origin1);
  editor.handle_event(nullptr, move, origin1, true, host);
  wmEvent release = mouse_event(LEFTMOUSE, KM_RELEASE, origin1);
  editor.handle_event(nullptr, release, origin1, true, host);
  ASSERT_TRUE(host.edit().active()->origin_is_custom);

  /* The modal reset command drops it and must not re-stamp the preview. */
  const int restamp_before = host.restamp_count;
  wmEvent reset{};
  reset.type = EVT_MODAL_MAP;
  reset.val = short(PAINT_SHAPE_MODAL_ORIGIN_RESET);
  EXPECT_EQ(editor.handle_event(nullptr, reset, origin1, true, host),
            ShapeVectorEditor::Status::Handled);
  EXPECT_FALSE(host.edit().active()->origin_is_custom);
  EXPECT_EQ(host.restamp_count, restamp_before);

  /* One undo brings the custom origin back. */
  EXPECT_TRUE(editor.undo(nullptr, host));
  EXPECT_TRUE(host.edit().active()->origin_is_custom);
}

TEST(VectorEditor, PressOutsideFinishes)
{
  TestVectorHost host(Vector<PaintShape>{test_polyline()}, 6.0f);
  ShapeVectorEditor editor;
  const wmEvent press = mouse_event(LEFTMOUSE, KM_PRESS, float2(500.0f, 500.0f));
  EXPECT_EQ(editor.handle_event(nullptr, press, float2(500.0f, 500.0f), true, host),
            ShapeVectorEditor::Status::Finished);
}

TEST(VectorEditor, StartMovePickUsesHandle)
{
  TestVectorHost host(Vector<PaintShape>{test_polyline()}, 6.0f);
  ShapeVectorEditor editor;

  /* The invoke path picks a control point and starts a point drag. */
  EXPECT_TRUE(editor.start_move_pick(nullptr, float2(100.0f, 0.0f), host));
  EXPECT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::PointDrag);
  editor.cancel_gesture(nullptr, host);

  /* The origin starts an origin drag. */
  const float2 origin = shape_effective_origin(*host.edit().active());
  EXPECT_TRUE(editor.start_move_pick(nullptr, origin, host));
  EXPECT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::OriginDrag);
  editor.cancel_gesture(nullptr, host);
  EXPECT_EQ(editor.gesture(), ShapeVectorEditor::Gesture::None);

  /* Nothing there: not owned. */
  EXPECT_FALSE(editor.start_move_pick(nullptr, float2(500.0f, 500.0f), host));
}

}  // namespace blender::ed::sculpt_paint::shape
