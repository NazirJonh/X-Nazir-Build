/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Per-stroke-method state inside an Image Paint stroke.
 *
 * `PaintOperation` is the general 2D painting stroke. A stroke method that needs to record
 * something across the whole gesture used to add its own fields and its own `if
 * (brush->stroke_method == ...)` branches to it -- Curve Patch alone reached eight such places,
 * spread across three unrelated responsibilities (recording, simplification, hand-off). A second
 * method doing the same would double that, and every field would be carried by ordinary strokes
 * that never use it.
 *
 * One `std::unique_ptr<ImageStrokeMethodHook>` replaces all of it. A method that needs nothing
 * creates none, and the stroke sees a null pointer.
 *
 * \note This fits a method whose relationship with the stroke is ONE-WAY -- record the gesture,
 * then act when it ends. It deliberately does NOT fit `BRUSH_STROKE_ROLL`, whose spline is read
 * back per-vertex from `sculpt_apply_texture()` while dabs are being applied; that one needs its
 * state extracted with forwarding accessors instead. Do not widen this interface to cover it.
 */

#include "BLI_math_vector_types.hh"

namespace blender {
struct bContext;
}

namespace blender::ed::sculpt_paint {

class ImageStrokeMethodHook {
 public:
  virtual ~ImageStrokeMethodHook() = default;

  /**
   * True when the stroke must NOT put real dabs on the canvas, because the gesture is being
   * recorded rather than painted. Rolling such dabs back afterwards is not equivalent: the
   * restore leaves a visible remnant baked under whatever is drawn next.
   */
  virtual bool suppresses_dabs() const
  {
    return false;
  }

  /** One dab of the gesture. `uv` is canonical image UV, already converted by the caller. */
  virtual void on_dab(const float2 &uv, float pressure) = 0;

  /**
   * The stroke ended.
   *
   * Returns true when the hook took over the stroke's in-flight image undo step -- the caller
   * must then NOT close it, because the step either was aborted or now belongs to something with
   * a longer life than this stroke.
   *
   * `stroke_started` distinguishes a real gesture from a press with no movement, which the
   * sparse per-dab sampling would otherwise accept silently.
   */
  virtual bool on_stroke_end(bContext &C, bool is_cancel, bool stroke_started) = 0;
};

}  // namespace blender::ed::sculpt_paint
