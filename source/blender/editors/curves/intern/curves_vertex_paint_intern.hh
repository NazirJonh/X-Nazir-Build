/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>
#include <optional>

#include "BLI_color.hh"
#include "BLI_math_color.hh"
#include "BKE_attribute.hh"
#include "DNA_brush_types.h"
#include "IMB_imbuf.hh"

#include "curves_paint_intern.hh"

struct bContext;

namespace blender::ed::sculpt_paint {

using ColorGeometry4f = blender::ColorGeometry4f;
using ColorPaint4f = blender::ColorPaint4f;

/**
 * Base class for curves vertex paint operations with color-specific utilities.
 * Inherits common paint functionality from CurvesPaintOperationBase.
 * Provides color-specific functionality for all vertex paint brush types (Draw, Blur, Average, Smear, Replace).
 */
class CurvesVertexPaintOperationBase : public CurvesPaintOperationBase {
 protected:
  static constexpr const char *ATTR_VERTEX_COLOR = "vertex_color";

  /* ----- Vertex paint specific settings ----- */
  ColorPaint4f brush_color{0.0f, 0.0f, 0.0f, 0.0f};
  IMB_BlendMode blend_mode{IMB_BLEND_MIX};
  bool paint_stroke_points{true};
  bool paint_fill{false};
  std::optional<bke::SpanAttributeWriter<ColorGeometry4f>> vertex_colors_writer_;

 public:
  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override;
  void on_stroke_done(const bContext &C) override;

  /* ----- Vertex paint specific utilities ----- */

  /**
   * Get current color of a point from vertex color attribute.
   * @param point_index Index of the curve point
   * @return Color value (RGBA)
   */
  ColorGeometry4f get_point_color(int point_index);

  /**
   * Set color of a point in vertex color attribute.
   * @param point_index Index of the curve point
   * @param color Color value (RGBA)
   */
  void set_point_color(int point_index, const ColorGeometry4f &color);

  /**
   * Blend two colors using the specified blend mode and influence factor.
   * @param src Source color to blend
   * @param dst Destination color to blend into
   * @param influence Blend factor (0.0 - 1.0)
   * @return Blended color
   */
  ColorPaint4f blend_colors(const ColorPaint4f &src, const ColorPaint4f &dst, float influence);

 protected:
  /* ----- Override to apply color to points ----- */

  /**
   * Override to apply vertex color to points collected in brush.
   * Default implementation applies brush color to all points.
   */
  void apply_operation_to_point(const CurvesBrushPoint &point) override;

  /**
   * Initialize vertex paint mode (ensure color attribute exists).
   */
  void init_paint_mode(const bContext &C) override;

  void finalize_paint_mode(const bContext &C) override;

  /**
   * Apply color to a specific point with given influence.
   * @param point_index Index of the curve point
   * @param color Color to apply (RGBA)
   * @param influence Brush influence factor (0.0 - 1.0)
   */
  virtual void apply_color_to_point(int point_index, const ColorPaint4f &color, float influence);
};

/**
 * Factory functions for different vertex paint operations.
 * Create appropriate operation based on brush type.
 */

/** Create a Draw vertex paint operation. */
std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_draw_operation(
    const BrushStrokeMode &stroke_mode);

/** Create a Blur vertex paint operation. */
std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_blur_operation();

/** Create an Average vertex paint operation. */
std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_average_operation();

/** Create a Smear vertex paint operation. */
std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_smear_operation();

/** Create a Replace vertex paint operation. */
std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_replace_operation();

/**
 * Mode enter/exit functions for vertex paint mode.
 */
void curves_vertex_paint_mode_enter(struct bContext *C);
void curves_vertex_paint_mode_exit(struct bContext *C);

bool curves_vertex_paint_poll(bContext *C);
bool curves_vertex_paint_mode_poll(bContext *C);

void curves_vertex_paint_ensure_color_attribute(Object *ob);

}  // namespace blender::ed::sculpt_paint

/**
 * Register vertex paint operators.
 * Called from ED_operatortypes_curves() to register all vertex paint operators.
 */
void ED_operatortypes_curves_vertex_paint();
