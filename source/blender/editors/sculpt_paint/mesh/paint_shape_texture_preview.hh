/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * On-screen preview of the Mask Source texture inside a Rectangle / Ellipse shape.
 *
 * Drawn while a Pixel / new shape is dragged out. It places the texture with the one UV matrix the
 * painted coverage uses (#shape_mask_source_uv_matrix), so the preview matches the result.
 */

#include <memory>

#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {
struct bContext;
struct PaintShapeSettings;
}  // namespace blender

namespace blender::gpu {
class Texture;
}

namespace blender::ed::sculpt_paint::shape {

struct PaintShape;
struct ShapeMaskSource;

/**
 * The shape settings the preview reads (toggle and opacity): a live Vector session owns a private
 * copy the header edits, otherwise the shared scene settings apply.
 */
const PaintShapeSettings &shape_texture_preview_settings_get(const bContext *C);

class ShapeTexturePreview {
 public:
  ShapeTexturePreview();
  ~ShapeTexturePreview();
  ShapeTexturePreview(const ShapeTexturePreview &) = delete;
  ShapeTexturePreview &operator=(const ShapeTexturePreview &) = delete;

  /** Drop the resolved source and the GPU texture. */
  void clear();

  /**
   * Resolve the source (reused while the brush inputs are unchanged), build the texture and the
   * clipped polygon for \a shape. \a shape_index must match the index the painted coverage uses
   * (the 3D backend always uses 0).
   * \return true when there is something to draw: Mask mode, a textured Mask Source and a
   * Rectangle / Ellipse shape with the preview enabled.
   */
  bool update(const bContext *C,
              const PaintShape &shape,
              const PaintShapeSettings &settings,
              bool is_3d,
              int shape_index);

  /**
   * Pixel-mode drag overlay: update for \a shape and draw it. Every vertex goes through
   * \a to_region (shape space -> region pixels) and the polygon is subdivided radially, so a
   * perspective (Surface space) mapping bends the texture like the shape instead of shearing it
   * affinely. Only shown while the shape is being dragged out, never for an existing Vector shape.
   */
  void draw_pixel_shape(const bContext *C,
                        const PaintShape &shape,
                        FunctionRef<float2(const float2 &)> to_region,
                        bool is_3d);

 private:
  std::shared_ptr<ShapeMaskSource> source_;
  uint64_t source_key_ = 0;
  gpu::Texture *texture_ = nullptr;
  uint64_t texture_signature_ = 0;
  /** Clipped polygon in the unit frame (shape UV - 0.5) and its texture UVs. */
  Vector<float2> positions_;
  Vector<float2> uvs_;
  float opacity_ = 0.0f;
};

}  // namespace blender::ed::sculpt_paint::shape
