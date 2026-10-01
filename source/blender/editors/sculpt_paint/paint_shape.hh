/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Data model of the shape drawing tools (Rectangle/Ellipse) for the Image Editor.
 *
 * A #PaintShape is the serializable description of what to paint; a #ShapeStyle is the
 * pointer-free snapshot of #PaintShapeSettings that controls how. The pipeline is: build a
 * #PaintShape (from a drag, a click or Python), evaluate it into dense outlines
 * (#shape_flatten), rasterize (`paint_shape_raster.hh`) and composite into the canvas/PBR
 * targets (`paint_image_shape_composite.hh`).
 *
 * Coordinates are pixels of the reference UDIM tile of the shapes' canvas (#CanvasTile), so stroke
 * widths and pixel snapping are well-defined; the tile a shape actually lands on is just a pixel
 * offset, and a target tile of another resolution gets the shapes scaled (#shapes_scale_to_target).
 *
 * This header is pure geometry: the image-side helpers (canvas symmetry, UV/tile mapping) live
 * in `mesh/paint_image_shape.hh`, the rasterizer in `paint_shape_raster.hh` and the operator
 * property serialization in `paint_shape_op_props.hh`.
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "BLI_math_color.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

namespace blender::ed::sculpt_paint::bezier_input {
class BezierInput;
struct BezierInputPoint;
}  // namespace blender::ed::sculpt_paint::bezier_input

namespace blender::ed::sculpt_paint::shape {

/**
 * The UDIM tile (and its pixel resolution) the pixel coordinates of a set of 2D shapes refer to.
 * It belongs to the canvas the shapes live in, not to each shape, so every consumer takes it next
 * to the shapes.
 */
struct CanvasTile {
  int ref_tile = 1001;
  int2 ref_tile_size = int2(1024);
};

/** The RNA colors are picked in sRGB (#PROP_COLOR_GAMMA) like the brush color; the compositing
 * works in scene linear and converts per buffer, so the snapshots store the converted form.
 * One shared definition: the style snapshot and the op-property overlay must not drift apart on
 * the sRGB-to-linear rule. */
inline float4 to_scene_linear(const float4 &picked)
{
  float4 linear = picked;
  srgb_to_linearrgb_v3_v3(linear, linear);
  return linear;
}

/** One control point of a #ShapeSpline, in reference-tile pixels. */
struct ShapePoint {
  float2 co = float2(0.0f);
  float2 handle_left = float2(0.0f);
  float2 handle_right = float2(0.0f);
  /** The segment leading into this point is a straight line. */
  bool corner = false;
  /** Stroke width multiplier at this point, interpolated along the spline. */
  float width_factor = 1.0f;
  /** Evaluate the handles from the neighbors (Catmull-Rom) instead of using the stored ones. */
  bool auto_handles = true;
};

struct ShapeSpline {
  Vector<ShapePoint> points;
  bool cyclic = false;
  /** Evaluate with Bézier handles; otherwise a plain polyline through the points. */
  bool is_bezier = false;
};

/** Traits of one #ePaintShapeType value: the single table behind #PaintShape::is_parametric(),
 * #PaintShape::has_analytic_sdf() and the open-outline check (`shape_is_open` in
 * `paint_shape_geom.cc`). A new shape type adds its row here; the switch has no default, so the
 * compiler flags every lookup that has not learned the new value yet. */
struct ShapeTypeTraits {
  /** The outline is generated from the parametric fields instead of the splines. */
  bool parametric;
  /** The rasterizer can use its analytic SDF instead of the flattened-polyline path. */
  bool analytic_sdf;
  /** The outline is open: it owns no fill interior. */
  bool open;
};

constexpr ShapeTypeTraits shape_type_traits(const ePaintShapeType type)
{
  switch (type) {
    case PAINT_SHAPE_LINE:
      return {false, false, true};
    case PAINT_SHAPE_POLYLINE:
      return {false, false, true};
    case PAINT_SHAPE_RECT:
      return {true, true, false};
    case PAINT_SHAPE_ELLIPSE:
      return {true, true, false};
    case PAINT_SHAPE_CURVE:
      return {false, false, true};
  }
  /* Not reachable with the current #ePaintShapeType values; reached only if the enum grew without
   * a new row above. */
  return {false, false, false};
}

struct PaintShape {
  ePaintShapeType type = PAINT_SHAPE_RECT;

  /* Parametric data of Rect/Ellipse; used when #splines is empty. */
  float2 center = float2(0.0f);
  /** Half extents. */
  float2 half_size = float2(0.0f);
  /** Rotation around #center, radians (the shape's own Angle). */
  float rotation = 0.0f;
  /** Corner radii: top-left, top-right, bottom-right, bottom-left. */
  float4 corner_radius = float4(0.0f);

  /** Spline data of symmetry copies that cannot stay parametric. */
  Vector<ShapeSpline> splines;

  /**
   * Custom pivot of the shape, in reference-tile pixels. Only meaningful while #origin_is_custom
   * is true; it moves and scales with the shape.
   */
  float2 origin = float2(0.0f);
  bool origin_is_custom = false;

  /** No outline to draw (splines-based shape without splines). */
  bool is_empty() const
  {
    return splines.is_empty() && !is_parametric();
  }

  /** The outline is generated from the parametric fields instead of the splines: Rect/Ellipse
   * with no spline data. */
  bool is_parametric() const
  {
    return splines.is_empty() && shape_type_traits(type).parametric;
  }

  /** The rasterizer can use its analytic SDF (Rect/Ellipse without spline data). */
  bool has_analytic_sdf() const
  {
    return splines.is_empty() && shape_type_traits(type).analytic_sdf;
  }
};

/** Stroke color source (Texture is TODO). */
enum class ShapeStrokeSource : int8_t {
  /** #ShapeStyle::stroke_color / #ShapeStyle::stroke_channels. */
  Solid = 0,
  /** #ShapeStyle::stroke_ramp_table sampled along the across-stroke coordinate. */
  Ramp = 1,
  /** A brush/asset texture mapped along the stroke (TODO). */
  Texture = 2,
};

/** Fill color source (Texture is TODO). */
enum class ShapeFillSource : int8_t {
  /** #ShapeStyle::fill_color / #ShapeStyle::fill_channels. */
  Solid = 0,
  /** #ShapeStyle::fill_gradient_table over the shape bbox. */
  Gradient = 1,
  /** A brush/asset texture over the shape (TODO). */
  Texture = 2,
};

/** Pointer-free snapshot of #PaintShapeSettings (profiles evaluated to tables). */
struct ShapeStyle {
  static constexpr int PROFILE_TABLE_SIZE = 256;

  int flag = PAINT_SHAPE_USE_FILL;
  ePaintShapeStrokeAlign stroke_align = PAINT_SHAPE_STROKE_ALIGN_CENTER;
  ePaintShapeCap cap_type = PAINT_SHAPE_CAP_ROUND;
  ePaintShapeJoin join_type = PAINT_SHAPE_JOIN_ROUND;
  ePaintShapeProfileMode profile_mode = PAINT_SHAPE_PROFILE_COVERAGE;
  ePaintShapeFillType fill_type = PAINT_SHAPE_FILL_SOLID;
  ePaintShapeFillRule fill_rule = PAINT_SHAPE_FILL_NONZERO;
  ePaintShapeDashCap dash_cap = PAINT_SHAPE_DASH_CAP_ROUND;
  ePaintShapeHeightBlend height_blend = PAINT_SHAPE_HEIGHT_ADD;

  float stroke_width = 8.0f;
  float feather = 1.0f;
  /** Rotation of the shape around its center, radians (Pixel-mode default placement). */
  float rotation = 0.0f;
  /** Pixel-mode default Rect/Ellipse size. */
  float2 size = float2(256.0f);
  float4 corner_radius = float4(0.0f);
  float dash_length = 16.0f;
  float gap_length = 16.0f;
  float dash_offset = 0.0f;
  float height_depth = 1.0f;
  float normal_strength = 1.0f;

  /** Stroke color, scene linear (converted once from the picked sRGB). */
  float4 stroke_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
  float4 fill_color = float4(1.0f, 1.0f, 1.0f, 1.0f);
  /** The colors as picked (sRGB): used directly for data (non-color) byte images. */
  float4 stroke_color_picked = float4(0.0f, 0.0f, 0.0f, 1.0f);
  float4 fill_color_picked = float4(1.0f, 1.0f, 1.0f, 1.0f);
  short stroke_blend = 0;
  short fill_blend = 0;
  float stroke_opacity = 1.0f;
  float fill_opacity = 1.0f;

  /** Stroke profile over the across-stroke coordinate t (0 = line center, 1 = outer edge). */
  std::array<float, PROFILE_TABLE_SIZE> stroke_profile_table{};
  /** Fill profile over the inward distance t (0 = fill edge, 1 = #fill_profile_width inwards). */
  std::array<float, PROFILE_TABLE_SIZE> fill_profile_table{};
  float fill_profile_width = 8.0f;

  /** Color ramp along the stroke profile (Canvas mode), sampled at #PROFILE_TABLE_SIZE steps. */
  std::array<float4, PROFILE_TABLE_SIZE> stroke_ramp_table{};
  bool use_stroke_ramp = false;
  /** Fill gradient ramp (GRADIENT fill), sampled at #PROFILE_TABLE_SIZE steps. */
  std::array<float4, PROFILE_TABLE_SIZE> fill_gradient_table{};
  bool use_fill_gradient = false;

  /** Which source feeds each part's color. Derived from the flags above by
   * #style_from_settings / #style_from_op_props; model-only (not DNA), the shader has one switch
   * over it, and D4 fills the Texture branches. */
  ShapeStrokeSource stroke_source = ShapeStrokeSource::Solid;
  ShapeFillSource fill_source = ShapeFillSource::Solid;

  /** PBR channel values per part, copied from the DNA settings (already pointer-free). */
  std::array<PaintShapeChannelValue, PAINT_MATERIAL_CHANNEL_NUM> stroke_channels{};
  std::array<PaintShapeChannelValue, PAINT_MATERIAL_CHANNEL_NUM> fill_channels{};

  bool use_fill() const
  {
    return (flag & PAINT_SHAPE_USE_FILL) != 0;
  }
  bool use_stroke() const
  {
    return (flag & PAINT_SHAPE_USE_STROKE) != 0;
  }
  bool use_dash() const
  {
    return (flag & PAINT_SHAPE_USE_DASH) != 0;
  }
  bool use_from_center() const
  {
    return (flag & PAINT_SHAPE_FROM_CENTER) != 0;
  }
  bool use_keep_aspect() const
  {
    return (flag & PAINT_SHAPE_KEEP_ASPECT) != 0;
  }
  bool use_profile() const
  {
    return (flag & PAINT_SHAPE_USE_PROFILE) != 0;
  }
  bool normal_flip_y() const
  {
    return (flag & PAINT_SHAPE_NORMAL_FLIP_Y) != 0;
  }
  bool use_stroke_screen_space() const
  {
    return (flag & PAINT_SHAPE_STROKE_SCREEN_SPACE) != 0;
  }
  bool use_channels_override() const
  {
    return (flag & PAINT_SHAPE_CHANNELS_OVERRIDE) != 0;
  }
  bool height_normal_link() const
  {
    return (flag & PAINT_SHAPE_HEIGHT_NORMAL_LINK) != 0;
  }
  bool profile_affects_coverage() const
  {
    return use_profile() && ELEM(profile_mode, PAINT_SHAPE_PROFILE_COVERAGE, PAINT_SHAPE_PROFILE_BOTH);
  }
  bool profile_affects_height() const
  {
    return use_profile() && ELEM(profile_mode, PAINT_SHAPE_PROFILE_HEIGHT, PAINT_SHAPE_PROFILE_BOTH);
  }

  /** Linear sample of the stroke profile at \a t in [0, 1]. */
  float stroke_profile_sample(float t) const;
  /** Linear sample of the fill profile at \a t in [0, 1]. */
  float fill_profile_sample(float t) const;
  /** Linear sample of the stroke color ramp at \a t in [0, 1]. */
  float4 stroke_ramp_sample(float t) const;
  /** Linear sample of the fill gradient at \a t in [0, 1]. */
  float4 fill_gradient_sample(float t) const;
};

/** Evaluate the DNA settings into a pointer-free snapshot. Main thread only (it evaluates the
 * profile curves into tables). */
ShapeStyle style_from_settings(const PaintShapeSettings &settings);

/**
 * Resolve \a style for \a shapes: when any shape is open (it has a non-cyclic spline), the stroke
 * is forced on with a minimum width, since an open outline has no fill to draw.
 */
ShapeStyle style_resolve_for_shapes(Span<PaintShape> shapes, ShapeStyle style);

/** Dense evaluated outline of a shape part: points, cumulative arc lengths, width factors.
 * Cyclic outlines repeat their first point at the end so the loop is explicit. */
struct ShapePolyline {
  Vector<float2> points;
  /** Cumulative arc length at each point (points[0] = 0). */
  Vector<float> arc_len;
  /** Stroke width multiplier at each point. */
  Vector<float> width;
  bool cyclic = false;
};

/** Evaluate \a shape into dense outlines (parametric shapes analytically, splines by
 * subdivision), in reference-tile pixels. */
void shape_flatten(const PaintShape &shape, float max_error_px, Vector<ShapePolyline> &r_out);
Vector<ShapePolyline> shape_flatten(const PaintShape &shape, float max_error_px);

/** Bounding box of \a shape in reference-tile pixels, expanded by the stroke width, feather and
 * the profile extent so a rasterizer region never clips the shape. */
rctf shape_bounds_calc(const PaintShape &shape, const ShapeStyle &style);

/* -------------------------------------------------------------------- */
/** \name Builders
 * \{ */

/** Straight line from \a p0 to \a p1. */
PaintShape shape_line(const float2 &p0, const float2 &p1);

/** Axis-aligned rectangle from two drag corners (or center + corner with \a from_center). */
PaintShape shape_rect_from_drag(const float2 &p0,
                                const float2 &p1,
                                bool from_center,
                                bool keep_aspect,
                                const ShapeStyle &style);

/** Ellipse from two drag positions (corner anchors, or center + extent with \a from_center). */
PaintShape shape_ellipse_from_drag(const float2 &p0, const float2 &p1, bool from_center, bool keep_aspect);

/** Default-size rectangle centered at \a center (a click without a drag in Pixel mode). */
PaintShape shape_rect_at_center(const float2 &center, const ShapeStyle &style);

/** Default-size ellipse centered at \a center (a click without a drag in Pixel mode). */
PaintShape shape_ellipse_at_center(const float2 &center, const ShapeStyle &style);

/**
 * The outline placed with the shared Bézier input as a #PAINT_SHAPE_CURVE (Bézier handles,
 * closed through the first point) or #PAINT_SHAPE_POLYLINE (straight segments).
 */
PaintShape shape_from_bezier_input(const bezier_input::BezierInput &input, ePaintShapeType type);

/**
 * Resolve the handles of point \a index: the stored ones when the point owns them, Catmull-Rom
 * auto handles otherwise (mirroring the live Bézier input).
 */
void shape_spline_point_handles_get(const ShapeSpline &spline,
                                    int index,
                                    float2 &r_handle_left,
                                    float2 &r_handle_right);

/** \} */

/** Translate \a shape by \a delta_px in reference-tile pixels. A custom #PaintShape::origin
 * moves with it. */
void shape_translate(PaintShape &shape, const float2 &delta_px);

/** Rotate \a shape around \a pivot by \a dangle radians. A parametric Rect/Ellipse accumulates
 * #PaintShape::rotation (and moves its #center around the pivot); a spline shape rotates its
 * control points and handles. */
void shape_rotate_about(PaintShape &shape, const float2 &pivot, float dangle);

/** Set the full width/height of a parametric Rect/Ellipse from \a size, keeping its center and
 * origin and re-clamping the rectangle's corner radii to the new half size. A no-op for any
 * spline shape. */
void shape_set_size(PaintShape &shape, const float2 &size);

/** Clamp a parametric rectangle's corner radii to its half size, so adjacent radii never overlap.
 * The single clamp rule shared by the builders and #shape_set_size. A no-op for any other shape
 * type. */
void shape_clamp_corner_radius(PaintShape &shape);

/* -------------------------------------------------------------------- */
/** \name Target resolution
 * \{ */

/**
 * \a shapes scaled from the reference-tile pixels by \a scale (the target tile's resolution over
 * the reference one): centers, half sizes, corner radii and the spline points with their handles
 * scale per axis. The caller derives the scaled #CanvasTile from the target tile size, which is
 * the reference size times \a scale.
 *
 * A rotated parametric shape under an axis-asymmetric scale skews in a way the parametric data
 * cannot express; such shapes are densified into polylines first (like the non-affine symmetry
 * maps), so their scaled result is exact.
 */
Vector<PaintShape> shapes_scale_to_target(Span<PaintShape> shapes, const float2 &scale);

/**
 * \a style with the pixel-based widths (stroke, feather, dash, fill falloff) scaled by the
 * average of \a scale's factors: the geometry above scales per axis while the widths carry one
 * factor, which is exact for uniform scales and the accepted approximation otherwise. The
 * feather never scales below one pixel (a deliberately smaller user value is kept), so the AA
 * edge stays smooth on downscaled targets.
 */
ShapeStyle style_scale_to_target(const ShapeStyle &style, const float2 &scale);

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
