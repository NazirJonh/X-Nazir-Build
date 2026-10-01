/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared creation machine of the shape drawing tools: the drag builders (Line / Rect / Ellipse)
 * and the Bézier input (Polyline / Curve Patch), used by both the Image
 * Editor and the 3D Viewport frontends.
 *
 * The gesture only turns presses, drags and modifiers into a #PaintShape; the frontend owns the
 * region, the overlay, the commit and the operator lifecycle. Coordinates are shape-space pixels;
 * the frontend maps its events into that space (region pixels for the 3D viewport, reference-tile
 * pixels for the Image Editor).
 */

#pragma once

#include <functional>

#include "BLI_math_vector_types.hh"

#include "paint_bezier_input.hh"
#include "paint_shape.hh"

namespace blender {

struct ARegion;
struct bContext;
struct wmEvent;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

/** Outcome of one modal event the creation machine processed. */
enum class ShapeCreateResult {
  /** Not relevant; the operator keeps running unchanged. */
  None,
  /** The preview changed; the frontend should redraw. */
  Changed,
  /** The shape is ready; the frontend commits it. */
  Confirmed,
  /** The gesture was abandoned (Esc / a zero-length line). */
  Cancelled,
};

/**
 * The drag and Bézier-input creation machine. One instance per draw: constructed from the tool's
 * type and a style snapshot, seeded with #begin, fed every modal event, then queried for #shape.
 */
class ShapeCreateGesture {
 public:
  ShapeCreateGesture(ePaintShapeType type,
                     const ShapeStyle &style,
                     bezier_input::Mapping input_mapping);

  /**
   * Start the gesture.
   *
   * \param event_p: the invoking event in shape space.
   * \param button_held: true when the invocation is the mouse button still held (LMB press); a
   * keyboard or menu invocation passes false and the drag starts on the first press.
   */
  void begin(ARegion &region, const wmEvent &event, const float2 &event_p, bool button_held);

  /** Feed a modal event; \a event_p is in shape space, \a in_owner whether it is in the owner
   * region (the Bézier input ignores outside mouse motion). */
  ShapeCreateResult handle_event(const wmEvent &event, const float2 &event_p, bool in_owner);

  bool use_input() const
  {
    return use_input_;
  }
  bezier_input::BezierInput &input()
  {
    return input_;
  }
  const bezier_input::BezierInput &input() const
  {
    return input_;
  }

  bool has_shape() const
  {
    return has_shape_;
  }
  /** True once the drag actually moved past the click threshold. The drag preview must wait for
   * this: the invoking press builds a degenerate shape at the press point, and drawing it would
   * flash a spurious outline (e.g. a dash from the region corner) before the first mouse move. */
  bool moved() const
  {
    return moved_;
  }
  const PaintShape &shape() const
  {
    return shape_;
  }
  ePaintShapeType type() const
  {
    return type_;
  }
  const ShapeStyle &style() const
  {
    return style_;
  }
  /** Replace the style snapshot (e.g. after the brush colors were swapped mid-drag). */
  void set_style(const ShapeStyle &style)
  {
    style_ = style;
  }
  float2 cursor_px() const
  {
    return cursor_p_;
  }

  /** The shape built from the Bézier input (input mode, when no drag preview exists). */
  PaintShape shape_from_input() const;

  /** Set the shape-space directions corresponding to one screen pixel along X and Y. */
  void set_drag_axes(const float2 &axis_x, const float2 &axis_y);

  /**
   * Pixel-mode F / Shift+F drag: adjust the stroke width or the overall opacity (the brush
   * strength) while the key is held, like the Vector session and the brush radial control.
   *
   * \param value: current stroke width or overall opacity the drag starts from.
   */
  void begin_value_drag(bool is_width, const int2 &mval, float value);
  /** Apply the drag from the cursor; returns the new width or overall opacity. */
  float update_value_drag(const int2 &mval);
  void end_value_drag();
  bool value_drag_active() const;
  bool value_drag_is_width() const;

 private:
  void update_drag(bool modifier_shift, bool modifier_alt, bool modifier_ctrl);
  PaintShape shape_at_center(const float2 &center) const;

  ePaintShapeType type_;
  ShapeStyle style_;
  bezier_input::Mapping input_mapping_;
  ARegion *owner_region_ = nullptr;

  bool use_input_ = false;
  bezier_input::BezierInput input_;

  bool drag_active_ = false;
  bool moved_ = false;
  bool move_shape_ = false;
  float2 press_p_ = float2(0.0f);
  float2 cursor_p_ = float2(0.0f);
  float2 move_prev_p_ = float2(0.0f);
  int2 press_mval_ = int2(0);

  bool has_shape_ = false;
  PaintShape shape_;

  float2 drag_axis_x_ = float2(1.0f, 0.0f);
  float2 drag_axis_y_ = float2(0.0f, 1.0f);
  float drag_rotation_ = 0.0f;
  bool use_drag_axes_ = false;

  bool value_drag_active_ = false;
  bool value_drag_is_width_ = false;
  float value_drag_start_value_ = 0.0f;
  int2 value_drag_start_mval_ = int2(0);
};

/** Status-bar items of the creation drag / Bézier input, shared by both frontends. */
void shape_status_set_creation(bContext *C, ePaintShapeType type);
void shape_status_set_input(bContext *C);

/**
 * Draw the animated dashed preview outline of \a shape: flatten it in shape space, project every
 * point through \a to_region, and draw. The frontend supplies the projector so the same overlay
 * serves region-pixel (3D) and reference-tile (Image Editor) spaces.
 *
 * \param width_guide: when set, the stroke edges (stroke width and Align of this style) are drawn
 * too, so a width being adjusted shows the band the stroke will cover.
 */
void shape_draw_creation_preview(const PaintShape &shape,
                                 float error_px,
                                 const std::function<float2(const float2 &)> &to_region,
                                 const ShapeStyle *width_guide = nullptr);

}  // namespace blender::ed::sculpt_paint::shape
