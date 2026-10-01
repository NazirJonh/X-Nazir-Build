/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Serialization of a #PaintShape and its #ShapeStyle into operator properties; see
 * #paint_shape_op_props.hh.
 */

#include <cfloat>
#include <cmath>
#include <climits>
#include <cstdint>

#include "BLI_math_vector_types.hh"

#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "WM_types.hh"

#include "paint_shape.hh"
#include "paint_shape_op_props.hh"

namespace blender::ed::sculpt_paint::shape {


/* -------------------------------------------------------------------- */
/** \name Operator property serialization
 * \{ */

void canvas_tile_to_op_props(wmOperator *op, const CanvasTile &tile)
{
  RNA_int_set(op->ptr, "shape_ref_tile", tile.ref_tile);
  const int tile_size[2] = {tile.ref_tile_size.x, tile.ref_tile_size.y};
  RNA_int_set_array(op->ptr, "shape_ref_tile_size", tile_size);
}

CanvasTile canvas_tile_from_op_props(wmOperator *op)
{
  CanvasTile tile;
  tile.ref_tile = RNA_int_get(op->ptr, "shape_ref_tile");
  int tile_size[2];
  RNA_int_get_array(op->ptr, "shape_ref_tile_size", tile_size);
  /* A hand-edited or stale redo may carry a non-positive size; keep the default then. */
  if (tile_size[0] > 0 && tile_size[1] > 0) {
    tile.ref_tile_size = int2(tile_size[0], tile_size[1]);
  }
  return tile;
}

void shape_to_op_props(wmOperator *op, const PaintShape &shape)
{
  RNA_int_set(op->ptr, "shape_type", int(shape.type));
  const float center[2] = {shape.center.x, shape.center.y};
  RNA_float_set_array(op->ptr, "shape_center", center);
  const float half_size[2] = {shape.half_size.x, shape.half_size.y};
  RNA_float_set_array(op->ptr, "shape_half_size", half_size);
  RNA_float_set(op->ptr, "shape_rotation", shape.rotation);
  const float corner_radius[4] = {shape.corner_radius.x,
                                  shape.corner_radius.y,
                                  shape.corner_radius.z,
                                  shape.corner_radius.w};
  RNA_float_set_array(op->ptr, "shape_corner_radius", corner_radius);
  const float origin[2] = {shape.origin.x, shape.origin.y};
  RNA_float_set_array(op->ptr, "shape_origin", origin);
  RNA_boolean_set(op->ptr, "shape_origin_custom", shape.origin_is_custom);

  RNA_collection_clear(op->ptr, "shape_points");
  for (const int s : shape.splines.index_range()) {
    const ShapeSpline &spline = shape.splines[s];
    for (const int p : spline.points.index_range()) {
      const ShapePoint &point = spline.points[p];
      PointerRNA itemptr;
      RNA_collection_add(op->ptr, "shape_points", &itemptr);
      const float co[2] = {point.co.x, point.co.y};
      RNA_float_set_array(&itemptr, "co", co);
      /* Auto handles are stored as the origin, so they are resolved here: `from_op_props`
       * always creates explicit handles. */
      float2 handle_left = point.handle_left;
      float2 handle_right = point.handle_right;
      if (point.auto_handles && spline.is_bezier) {
        shape_spline_point_handles_get(spline, p, handle_left, handle_right);
      }
      const float handle_left_arr[2] = {handle_left.x, handle_left.y};
      RNA_float_set_array(&itemptr, "handle_left", handle_left_arr);
      const float handle_right_arr[2] = {handle_right.x, handle_right.y};
      RNA_float_set_array(&itemptr, "handle_right", handle_right_arr);
      RNA_boolean_set(&itemptr, "corner", point.corner);
      RNA_float_set(&itemptr, "width", point.width_factor);
      RNA_int_set(&itemptr, "spline", s);
      RNA_boolean_set(&itemptr, "cyclic", spline.cyclic);
      RNA_boolean_set(&itemptr, "is_bezier", spline.is_bezier);
    }
  }
}

std::optional<PaintShape> shape_from_op_props(wmOperator *op)
{
  const int type = RNA_int_get(op->ptr, "shape_type");
  if (type < PAINT_SHAPE_LINE || type > PAINT_SHAPE_CURVE) {
    return std::nullopt;
  }
  PaintShape shape;
  shape.type = ePaintShapeType(type);
  float center[2];
  RNA_float_get_array(op->ptr, "shape_center", center);
  shape.center = float2(center[0], center[1]);
  float half_size[2];
  RNA_float_get_array(op->ptr, "shape_half_size", half_size);
  shape.half_size = float2(half_size[0], half_size[1]);
  shape.rotation = RNA_float_get(op->ptr, "shape_rotation");
  float corner_radius[4];
  RNA_float_get_array(op->ptr, "shape_corner_radius", corner_radius);
  shape.corner_radius = float4(
      corner_radius[0], corner_radius[1], corner_radius[2], corner_radius[3]);
  float origin[2];
  RNA_float_get_array(op->ptr, "shape_origin", origin);
  shape.origin = float2(origin[0], origin[1]);
  shape.origin_is_custom = RNA_boolean_get(op->ptr, "shape_origin_custom");

  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "shape_points");
  if (prop == nullptr) {
    return std::nullopt;
  }

  const int points_num = RNA_collection_length(op->ptr, "shape_points");
  RNA_PROP_BEGIN (op->ptr, itemptr, prop) {
    ShapePoint point;
    float co[2], handle_left[2], handle_right[2];
    RNA_float_get_array(&itemptr, "co", co);
    RNA_float_get_array(&itemptr, "handle_left", handle_left);
    RNA_float_get_array(&itemptr, "handle_right", handle_right);
    point.co = float2(co[0], co[1]);
    point.handle_left = float2(handle_left[0], handle_left[1]);
    point.handle_right = float2(handle_right[0], handle_right[1]);
    point.corner = RNA_boolean_get(&itemptr, "corner");
    point.width_factor = RNA_float_get(&itemptr, "width");
    point.auto_handles = false;

    const int spline_index = RNA_int_get(&itemptr, "spline");
    /* Every spline owns at least one point, so a valid index is below the point count; this keeps a
     * corrupt value from allocating an unbounded spline list. */
    if (spline_index < 0 || spline_index >= points_num) {
      continue;
    }
    while (shape.splines.size() <= spline_index) {
      shape.splines.append(ShapeSpline{});
    }
    ShapeSpline &spline = shape.splines[spline_index];
    if (spline.points.is_empty()) {
      /* The spline-level flags repeat on every point; read them once. */
      spline.cyclic = RNA_boolean_get(&itemptr, "cyclic");
      spline.is_bezier = RNA_boolean_get(&itemptr, "is_bezier");
    }
    spline.points.append(point);
  }
  RNA_PROP_END;

  /* No spline data and nothing parametric to draw: the properties hold no usable shape. */
  if (shape.splines.is_empty() && !shape.is_parametric()) {
    return std::nullopt;
  }
  return shape;
}

void style_to_op_props(wmOperator *op, const ShapeStyle &style)
{
  RNA_int_set(op->ptr, "style_flag", style.flag);
  RNA_float_set(op->ptr, "stroke_width", style.stroke_width);
  RNA_float_set(op->ptr, "feather", style.feather);
  RNA_int_set(op->ptr, "stroke_align", int(style.stroke_align));
  RNA_int_set(op->ptr, "cap_type", int(style.cap_type));
  RNA_int_set(op->ptr, "join_type", int(style.join_type));
  RNA_int_set(op->ptr, "dash_cap", int(style.dash_cap));
  RNA_float_set(op->ptr, "dash_length", style.dash_length);
  RNA_float_set(op->ptr, "gap_length", style.gap_length);
  RNA_float_set(op->ptr, "dash_offset", style.dash_offset);
  RNA_int_set(op->ptr, "fill_type", int(style.fill_type));
  RNA_int_set(op->ptr, "fill_rule", int(style.fill_rule));
  RNA_float_set(op->ptr, "fill_profile_width", style.fill_profile_width);
  RNA_int_set(op->ptr, "height_blend", int(style.height_blend));
  RNA_float_set(op->ptr, "height_depth", style.height_depth);
  RNA_float_set(op->ptr, "normal_strength", style.normal_strength);
  RNA_boolean_set(op->ptr, "use_stroke_ramp", style.use_stroke_ramp);
  RNA_boolean_set(op->ptr, "use_fill_gradient", style.use_fill_gradient);
  RNA_int_set(op->ptr, "stroke_blend", style.stroke_blend);
  RNA_int_set(op->ptr, "fill_blend", style.fill_blend);
  RNA_float_set(op->ptr, "stroke_opacity", style.stroke_opacity);
  RNA_float_set(op->ptr, "fill_opacity", style.fill_opacity);
  /* The picked (sRGB) form is stored, so the Redo panel edits it like the settings' colors; the
   * scene-linear forms are derived back in `from_op_props`. */
  const float stroke_color[4] = {style.stroke_color_picked.x,
                                 style.stroke_color_picked.y,
                                 style.stroke_color_picked.z,
                                 style.stroke_color_picked.w};
  RNA_float_set_array(op->ptr, "stroke_color", stroke_color);
  const float fill_color[4] = {style.fill_color_picked.x,
                               style.fill_color_picked.y,
                               style.fill_color_picked.z,
                               style.fill_color_picked.w};
  RNA_float_set_array(op->ptr, "fill_color", fill_color);
}

bool style_from_op_props(wmOperator *op, ShapeStyle &r_style)
{
  /* Presence marker: a fresh invocation never stored a style (`style_flag`'s RNA minimum is -1,
   * its default, so an unset property is never clamped into the valid range). */
  const int flag = RNA_int_get(op->ptr, "style_flag");
  if (flag < 0) {
    return false;
  }
  r_style.flag = flag;
  r_style.stroke_width = RNA_float_get(op->ptr, "stroke_width");
  r_style.feather = RNA_float_get(op->ptr, "feather");
  r_style.stroke_align = ePaintShapeStrokeAlign(RNA_int_get(op->ptr, "stroke_align"));
  r_style.cap_type = ePaintShapeCap(RNA_int_get(op->ptr, "cap_type"));
  r_style.join_type = ePaintShapeJoin(RNA_int_get(op->ptr, "join_type"));
  r_style.dash_cap = ePaintShapeDashCap(RNA_int_get(op->ptr, "dash_cap"));
  r_style.dash_length = RNA_float_get(op->ptr, "dash_length");
  r_style.gap_length = RNA_float_get(op->ptr, "gap_length");
  r_style.dash_offset = RNA_float_get(op->ptr, "dash_offset");
  r_style.fill_type = ePaintShapeFillType(RNA_int_get(op->ptr, "fill_type"));
  r_style.fill_rule = ePaintShapeFillRule(RNA_int_get(op->ptr, "fill_rule"));
  r_style.fill_profile_width = RNA_float_get(op->ptr, "fill_profile_width");
  r_style.height_blend = ePaintShapeHeightBlend(RNA_int_get(op->ptr, "height_blend"));
  r_style.height_depth = RNA_float_get(op->ptr, "height_depth");
  r_style.normal_strength = RNA_float_get(op->ptr, "normal_strength");
  r_style.use_stroke_ramp = RNA_boolean_get(op->ptr, "use_stroke_ramp");
  r_style.use_fill_gradient = RNA_boolean_get(op->ptr, "use_fill_gradient");
  /* Keep the source selection in sync with the replayed flags (otherwise F9 would drop a
   * stroke ramp / fill gradient). */
  r_style.stroke_source = r_style.use_stroke_ramp ? ShapeStrokeSource::Ramp :
                                                    ShapeStrokeSource::Solid;
  r_style.fill_source = r_style.use_fill_gradient ? ShapeFillSource::Gradient :
                                                    ShapeFillSource::Solid;
  r_style.stroke_blend = short(RNA_int_get(op->ptr, "stroke_blend"));
  r_style.fill_blend = short(RNA_int_get(op->ptr, "fill_blend"));
  r_style.stroke_opacity = RNA_float_get(op->ptr, "stroke_opacity");
  r_style.fill_opacity = RNA_float_get(op->ptr, "fill_opacity");

  float stroke_color[4];
  RNA_float_get_array(op->ptr, "stroke_color", stroke_color);
  r_style.stroke_color_picked = float4(
      stroke_color[0], stroke_color[1], stroke_color[2], stroke_color[3]);
  r_style.stroke_color = to_scene_linear(r_style.stroke_color_picked);
  float fill_color[4];
  RNA_float_get_array(op->ptr, "fill_color", fill_color);
  r_style.fill_color_picked = float4(
      fill_color[0], fill_color[1], fill_color[2], fill_color[3]);
  r_style.fill_color = to_scene_linear(r_style.fill_color_picked);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator property registration
 * \{ */

void shape_op_properties_register(wmOperatorType *ot)
{
  PropertyRNA *prop;

  prop = RNA_def_int(ot->srna,
                     "shape_type",
                     int(PAINT_SHAPE_RECT),
                     0,
                     int(PAINT_SHAPE_CURVE),
                     "Shape Type",
                     "",
                     0,
                     int(PAINT_SHAPE_CURVE));
  RNA_def_property_flag(prop, PROP_HIDDEN);

  /* Upper bound matches UDIM (`IMA_UDIM_MAX`): rows beyond the first ten are valid. */
  prop = RNA_def_int(ot->srna, "shape_ref_tile", 1001, 1001, 2000, "Reference Tile", "", 1001, 2000);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  static const int default_tile_size[2] = {1024, 1024};
  prop = RNA_def_int_array(ot->srna,
                           "shape_ref_tile_size",
                           2,
                           default_tile_size,
                           1,
                           65536,
                           "Reference Tile Size",
                           "",
                           1,
                           65536);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float_array(ot->srna,
                             "shape_center",
                             2,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Center",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float_array(ot->srna,
                             "shape_half_size",
                             2,
                             nullptr,
                             0.0f,
                             FLT_MAX,
                             "Half Size",
                             "",
                             0.0f,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float(ot->srna,
                       "shape_rotation",
                       0.0f,
                       -FLT_MAX,
                       FLT_MAX,
                       "Rotation",
                       "",
                       -FLT_MAX,
                       FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float_array(ot->srna,
                             "shape_corner_radius",
                             4,
                             nullptr,
                             0.0f,
                             FLT_MAX,
                             "Corner Radius",
                             "",
                             0.0f,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float_array(ot->srna,
                             "shape_origin",
                             2,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Origin",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_boolean(ot->srna, "shape_origin_custom", false, "Origin Custom", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_collection_runtime(ot->srna, "shape_points", RNA_OperatorShapePoint, "Points", "");
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
}

void style_op_properties_register(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Presence marker: `style_from_op_props` rejects the -1 default of a fresh invocation. */
  prop = RNA_def_int(ot->srna, "style_flag", -1, -1, INT_MAX, "Style Flags", "", -1, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  /* The Redo (F9) panel exposes the appearance tweaks; the rest of the style replays hidden. */
  prop = RNA_def_float(
      ot->srna, "stroke_width", 8.0f, 0.0f, FLT_MAX, "Stroke Width", "", 0.0f, 10000.0f);
  prop = RNA_def_float(ot->srna, "feather", 1.0f, 0.0f, FLT_MAX, "Feather", "", 0.0f, 100.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "stroke_align", 0, 0, 2, "Align", "", 0, 2);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "cap_type", 0, 0, 2, "Cap", "", 0, 2);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "join_type", 0, 0, 2, "Join", "", 0, 2);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "dash_cap", 0, 0, 2, "Dash Cap", "", 0, 2);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "dash_length", 16.0f, 0.0f, FLT_MAX, "Dash Length", "", 0.0f, 10000.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "gap_length", 16.0f, 0.0f, FLT_MAX, "Gap Length", "", 0.0f, 10000.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "dash_offset", 0.0f, -FLT_MAX, FLT_MAX, "Dash Offset", "", -10000.0f, 10000.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "fill_type", 0, 0, 2, "Fill Type", "", 0, 2);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "fill_rule", 0, 0, 1, "Fill Rule", "", 0, 1);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "fill_profile_width", 8.0f, 0.0f, FLT_MAX, "Fill Falloff", "", 0.0f, 10000.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "height_blend", 0, 0, 3, "Height Blend", "", 0, 3);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "height_depth", 1.0f, 0.0f, 100.0f, "Height Depth", "", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "normal_strength", 1.0f, 0.0f, 10.0f, "Normal Strength", "", 0.0f, 10.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_boolean(ot->srna, "use_stroke_ramp", false, "Use Stroke Ramp", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_boolean(ot->srna, "use_fill_gradient", false, "Use Fill Gradient", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "stroke_blend", 0, 0, INT16_MAX, "Stroke Blend", "", 0, INT16_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna, "fill_blend", 0, 0, INT16_MAX, "Fill Blend", "", 0, INT16_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "stroke_opacity", 1.0f, 0.0f, 1.0f, "Stroke Opacity", "", 0.0f, 1.0f);
  prop = RNA_def_float(ot->srna, "fill_opacity", 1.0f, 0.0f, 1.0f, "Fill Opacity", "", 0.0f, 1.0f);

  prop = RNA_def_float_color(
      ot->srna, "stroke_color", 4, nullptr, 0.0f, FLT_MAX, "Stroke Color", "", 0.0f, 1.0f);
  RNA_def_property_subtype(prop, PROP_COLOR_GAMMA);
  prop = RNA_def_float_color(
      ot->srna, "fill_color", 4, nullptr, 0.0f, FLT_MAX, "Fill Color", "", 0.0f, 1.0f);
  RNA_def_property_subtype(prop, PROP_COLOR_GAMMA);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
