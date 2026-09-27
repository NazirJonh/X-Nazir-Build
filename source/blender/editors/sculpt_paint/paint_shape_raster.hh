/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Signed-distance-field rasterizer of the shape drawing tools.
 *
 * Rasterization is pure geometry: it produces per-pixel coverage of the fill and of the stroke
 * (dashes, caps and variable widths included) plus the across-stroke coordinate and the distance
 * gradient the profile/height/normal compositing samples. Color mixing, profile application and
 * the selection mask belong to the compositor (`paint_image_shape_composite.hh`).
 */

#pragma once

#include <memory>

#include "BLI_array.hh"
#include "BLI_enum_flags.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"

#include "paint_shape.hh"

namespace blender::ed::sculpt_paint::shape {

/** Per-pixel coverage of one rasterization region (a pixel rect of one UDIM tile). */
struct ShapeCoverage {
  /** Pixel rect of the tile the buffers are laid out over, half-open [xmin, xmax). */
  rcti rect = {};

  /** 0..1 fill coverage. */
  Array<float> fill;
  /** 0..1 stroke coverage (dash, caps and variable width included). */
  Array<float> stroke;
  /**
   * Across-stroke coordinate: 0 at the stroke's centerline, 1 at its edge (the shift of the
   * stroke alignment included). Zero wherever the stroke coverage is zero.
   */
  Array<float> stroke_t;
  /**
   * Unit gradient of the across-stroke distance: points away from the stroke's centerline, so
   * the height profile slopes down along it. Zero wherever the stroke coverage is zero.
   */
  Array<float2> stroke_dir;
  /** Distance inward from the fill's edge in pixels (saturates at the evaluation margin). */
  Array<float> fill_d;
  /** Unit gradient of #fill_d per pixel, pointing inward; zero where the fill coverage is zero. */
  Array<float2> fill_dir;

  /**
   * Along-stroke coordinate: arc length from the START of the stroke's flattened polyline to the
   * closest centerline point, in shape pixels. For a cyclic polyline the seam is its first
   * flattened point and the value wraps into [0, #stroke_len). Zero where the stroke coverage is
   * zero.
   */
  Array<float> stroke_s;
  /**
   * Total arc length of the polyline #stroke_s is measured on -- the denominator of the
   * along-stroke texture coordinate `stroke_s / stroke_len` (each polyline restarts at 0). Zero
   * where the stroke coverage is zero.
   */
  Array<float> stroke_len;
  /**
   * Local coordinates of the shape in [0, 1]^2. For Rect/Ellipse it is the rotated, centered
   * parametric frame normalized by #PaintShape::half_size (the frame corners map to (0,0) and
   * (1,1)); for spline shapes it is the geometry's axis-aligned bbox. Zero outside every shape.
   */
  Array<float2> shape_uv;

  int width() const
  {
    return rect.xmax - rect.xmin;
  }
  int height() const
  {
    return rect.ymax - rect.ymin;
  }
  int64_t index(const int x, const int y) const
  {
    return int64_t(y - rect.ymin) * width() + (x - rect.xmin);
  }
};

/** Which quantities rasterization computes (the expensive gradient/distance outputs are
 * skipped unless a caller needs them for Height, Normal or fill-profile shading). */
enum class ShapeRasterOutputs : uint8_t {
  Fill = (1 << 0),
  Stroke = (1 << 1),
  StrokeT = (1 << 2),
  StrokeDir = (1 << 3),
  FillD = (1 << 4),
  FillDir = (1 << 5),
  /** Along-stroke arc length and the polyline's full length (texture along the stroke). Opt-in
   * (request #Stroke alongside it); it forces Rect/Ellipse through the flattened-polyline path
   * (their analytic SDF carries no arc length). */
  StrokeS = (1 << 6),
  /** Local shape coordinates in [0, 1]^2 (bbox or parametric frame; texture fill). Opt-in. */
  ShapeUV = (1 << 7),
};
ENUM_OPERATORS(ShapeRasterOutputs);

/** The default output set: coverage and the gradients/distance shading reads. The along-stroke
 * (#StrokeS) and local-UV (#ShapeUV) outputs are deliberately NOT in here -- a caller that does
 * not ask for them gets no buffer and no computation, and the 3D texel path (which uses this
 * constant) never pays for them. */
constexpr ShapeRasterOutputs SHAPE_RASTER_OUTPUTS_ALL = ShapeRasterOutputs(
    uint8_t(ShapeRasterOutputs::Fill) | uint8_t(ShapeRasterOutputs::Stroke) |
    uint8_t(ShapeRasterOutputs::StrokeT) | uint8_t(ShapeRasterOutputs::StrokeDir) |
    uint8_t(ShapeRasterOutputs::FillD) | uint8_t(ShapeRasterOutputs::FillDir));

/** The decoded #ShapeRasterOutputs mask, computed once from it and passed down: the evaluation
 * paths read plain booleans instead of re-testing the bits per shape and per pixel. */
struct ShapeRasterWants {
  bool fill = false;
  bool stroke = false;
  bool stroke_t = false;
  bool stroke_dir = false;
  bool fill_d = false;
  bool fill_dir = false;
  bool stroke_s = false;
  bool shape_uv = false;

  ShapeRasterWants() = default;
  explicit ShapeRasterWants(const ShapeRasterOutputs outputs)
      : fill((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::Fill)) != 0),
        stroke((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::Stroke)) != 0),
        stroke_t((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::StrokeT)) != 0),
        stroke_dir((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::StrokeDir)) != 0),
        fill_d((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::FillD)) != 0),
        fill_dir((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::FillDir)) != 0),
        stroke_s((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::StrokeS)) != 0),
        shape_uv((uint8_t(outputs) & uint8_t(ShapeRasterOutputs::ShapeUV)) != 0)
  {
  }
};

/**
 * Rasterize the (already symmetry-expanded) \a shapes over the pixel rect \a rect of one UDIM
 * tile.
 *
 * \param tile_offset_px: shape-space pixel coordinates of the tile's (0, 0) pixel corner; the
 * shape-space position of the tile pixel (x, y) is `tile_offset_px + (x + 0.5, y + 0.5)`. For
 * the reference tile itself the offset is zero.
 *
 * Evaluation happens on shared worker threads; the caller must not hold locks that conflict
 * with them.
 */
ShapeCoverage shape_rasterize(Span<PaintShape> shapes,
                              const ShapeStyle &style,
                              const rcti &rect,
                              const float2 &tile_offset_px,
                              ShapeRasterOutputs outputs = SHAPE_RASTER_OUTPUTS_ALL);

/**
 * Reusable rasterization of one tile: the segment geometry and acceleration grid are built once
 * for \a grid_rect, then sub-rectangles (strips) can be rasterized repeatedly without paying the
 * geometry build again — the compositor drives the strip loop to keep peak memory low.
 *
 * Only the requested \a outputs are computed and allocated in #ShapeCoverage; unrequested
 * buffers stay empty. The distance/direction outputs derive from the base coverage, so callers
 * include #Fill alongside #FillD/#FillDir and #Stroke alongside #StrokeT/#StrokeDir.
 */
class ShapeRasterizer {
 public:
  ShapeRasterizer(Span<PaintShape> shapes,
                  const ShapeStyle &style,
                  const rcti &grid_rect,
                  const float2 &tile_offset_px,
                  ShapeRasterOutputs outputs = SHAPE_RASTER_OUTPUTS_ALL);
  ~ShapeRasterizer();
  ShapeRasterizer(ShapeRasterizer &&other) noexcept;
  ShapeRasterizer &operator=(ShapeRasterizer &&other) noexcept;

  /** Rasterize \a rect (half-open, inside \a grid_rect) into a fresh coverage. */
  ShapeCoverage rasterize(const rcti &rect) const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

/** Coverage of a single 2D point: the same quantities the rasterizer writes per pixel. */
struct ShapeSample {
  float fill = 0.0f;
  float stroke = 0.0f;
  float stroke_t = 0.0f;
  float fill_d = 0.0f;
  float2 stroke_dir = float2(0.0f);
  /**
   * Unit gradient of #fill_d: points inward, toward the fill interior (the direction the fill
   * profile's coordinate grows). Zero wherever the fill coverage is zero.
   */
  float2 fill_dir = float2(0.0f);
  /** Along-stroke arc length and full polyline length; see #ShapeCoverage::stroke_s /
   * #ShapeCoverage::stroke_len. */
  float stroke_s = 0.0f;
  float stroke_len = 0.0f;
  /** Local shape coordinates in [0, 1]^2; see #ShapeCoverage::shape_uv. */
  float2 shape_uv = float2(0.0f);
};

/**
 * Point coverage of the (already symmetry-expanded) \a shapes over \a domain (shape-space
 * pixels): the 3D path samples projections of mesh vertices and texels through it, and the 2D
 * path uses it for contour hit-testing. Thread-safe after construction.
 */
class ShapeEvaluator {
 public:
  ShapeEvaluator(Span<PaintShape> shapes,
                 const ShapeStyle &style,
                 const rctf &domain,
                 ShapeRasterOutputs outputs = SHAPE_RASTER_OUTPUTS_ALL);
  ~ShapeEvaluator();
  ShapeEvaluator(ShapeEvaluator &&other) noexcept;
  ShapeEvaluator &operator=(ShapeEvaluator &&other) noexcept;

  /** Coverage at the shape-space point \a p. */
  ShapeSample sample(const float2 &p) const;
  /** Coverage of every point in \a points (same order). Thread-parallel. */
  void sample_many(Span<float2> points, MutableSpan<ShapeSample> r_samples) const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace blender::ed::sculpt_paint::shape
