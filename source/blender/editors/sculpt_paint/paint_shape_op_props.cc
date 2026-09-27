/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Serialization of a #PaintShape and its #ShapeStyle into operator properties; see
 * #paint_shape_op_props.hh.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>

#include "BLI_math_constants.h"
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
  tile.ref_tile_size = int2(tile_size[0], tile_size[1]);
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
  /* Generator parameters of a parametric Polygon/Star/Arc. */
  RNA_int_set(op->ptr, "shape_polygon_sides", shape.polygon_sides);
  RNA_float_set(op->ptr, "shape_star_inner_ratio", shape.star_inner_ratio);
  RNA_float_set(op->ptr, "shape_arc_start", shape.arc_start);
  RNA_float_set(op->ptr, "shape_arc_end", shape.arc_end);
  RNA_int_set(op->ptr, "shape_arc_mode", int(shape.arc_mode));

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
  if (type < PAINT_SHAPE_LINE || type > PAINT_SHAPE_ARC) {
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
  /* Generator parameters of a parametric Polygon/Star/Arc. */
  shape.polygon_sides = std::max(3, RNA_int_get(op->ptr, "shape_polygon_sides"));
  shape.star_inner_ratio = RNA_float_get(op->ptr, "shape_star_inner_ratio");
  shape.arc_start = RNA_float_get(op->ptr, "shape_arc_start");
  shape.arc_end = RNA_float_get(op->ptr, "shape_arc_end");
  shape.arc_mode = ePaintShapeArcMode(RNA_int_get(op->ptr, "shape_arc_mode"));

  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "shape_points");
  if (prop == nullptr) {
    return std::nullopt;
  }

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
  RNA_int_set(op->ptr, "polygon_sides", style.polygon_sides);
  RNA_float_set(op->ptr, "star_inner_ratio", style.star_inner_ratio);
  RNA_float_set(op->ptr, "arc_start", style.arc_start);
  RNA_float_set(op->ptr, "arc_end", style.arc_end);
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
  r_style.polygon_sides = short(RNA_int_get(op->ptr, "polygon_sides"));
  r_style.star_inner_ratio = RNA_float_get(op->ptr, "star_inner_ratio");
  r_style.arc_start = RNA_float_get(op->ptr, "arc_start");
  r_style.arc_end = RNA_float_get(op->ptr, "arc_end");
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
                     int(PAINT_SHAPE_ARC),
                     "Shape Type",
                     "",
                     0,
                     int(PAINT_SHAPE_ARC));
  RNA_def_property_flag(prop, PROP_HIDDEN);

  /* Upper bound matches UDIM (`IMA_UDIM_MAX`): rows beyond the first ten are valid. */
  prop = RNA_def_int(ot->srna, "shape_ref_tile", 1001, 1001, 2000, "Reference Tile", "", 1001, 2000);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_int_array(ot->srna,
                           "shape_ref_tile_size",
                           2,
                           nullptr,
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

  prop = RNA_def_int(
      ot->srna, "shape_polygon_sides", 6, 3, 1024, "Polygon Sides", "", 3, 1024);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float(
      ot->srna, "shape_star_inner_ratio", 0.5f, 0.0f, 1.0f, "Star Inner Ratio", "", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float(
      ot->srna, "shape_arc_start", 0.0f, -FLT_MAX, FLT_MAX, "Arc Start", "", -FLT_MAX, FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float(ot->srna,
                       "shape_arc_end",
                       float(M_TAU),
                       -FLT_MAX,
                       FLT_MAX,
                       "Arc End",
                       "",
                       -FLT_MAX,
                       FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_int(ot->srna, "shape_arc_mode", 0, 0, 16, "Arc Mode", "", 0, 16);
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
  RNA_def_property_flag(prop, PROP_HIDDEN);
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
  prop = RNA_def_int(ot->srna, "polygon_sides", 6, 3, 64, "Sides", "", 3, 64);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(
      ot->srna, "star_inner_ratio", 0.5f, 0.0f, 1.0f, "Inner Ratio", "", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(ot->srna,
                       "arc_start",
                       0.0f,
                       -M_PI * 2.0f,
                       M_PI * 2.0f,
                       "Arc Start",
                       "",
                       -M_PI * 2.0f,
                       M_PI * 2.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(ot->srna,
                       "arc_end",
                       float(M_TAU),
                       -M_PI * 2.0f,
                       M_PI * 2.0f,
                       "Arc End",
                       "",
                       -M_PI * 2.0f,
                       M_PI * 2.0f);
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

/* -------------------------------------------------------------------- */
/** \name Space serialization
 *
 * The 3D shape's frozen view (its #ShapeSpaceDesc) is stored with the operator so `exec` and
 * Redo (F9) rebuild the exact same projection instead of reading the live viewport. The matrices
 * are flat float arrays in the #float4x4 memory order (column-major).
 * \{ */

void space_to_op_props(wmOperator *op, const ShapeSpaceDesc &desc)
{
  RNA_int_set(op->ptr, "space_type", int(desc.type));
  RNA_int_set(op->ptr, "space_ref_tile", desc.ref_tile);
  const int ref_tile_size[2] = {desc.ref_tile_size.x, desc.ref_tile_size.y};
  RNA_int_set_array(op->ptr, "space_ref_tile_size", ref_tile_size);
  RNA_boolean_set(op->ptr, "space_is_persp", desc.is_persp);
  const int win_size[2] = {desc.win_size.x, desc.win_size.y};
  RNA_int_set_array(op->ptr, "space_win_size", win_size);
  RNA_float_set(op->ptr, "space_clip_start", desc.clip_start);
  RNA_float_set(op->ptr, "space_clip_end", desc.clip_end);
  float persmat[16];
  memcpy(persmat, desc.persmat.ptr(), sizeof(persmat));
  RNA_float_set_array(op->ptr, "space_persmat", persmat);
  float viewinv[16];
  memcpy(viewinv, desc.viewinv.ptr(), sizeof(viewinv));
  RNA_float_set_array(op->ptr, "space_viewinv", viewinv);

  /* SurfaceAnchored (R8). */
  const float anchor_co[3] = {desc.anchor.co.x, desc.anchor.co.y, desc.anchor.co.z};
  RNA_float_set_array(op->ptr, "space_anchor_co", anchor_co);
  const float anchor_normal[3] = {
      desc.anchor.normal.x, desc.anchor.normal.y, desc.anchor.normal.z};
  RNA_float_set_array(op->ptr, "space_anchor_normal", anchor_normal);
  const float anchor_tangent[3] = {
      desc.anchor.tangent.x, desc.anchor.tangent.y, desc.anchor.tangent.z};
  RNA_float_set_array(op->ptr, "space_anchor_tangent", anchor_tangent);
  const float anchor_uv[2] = {desc.anchor.surface_uv.x, desc.anchor.surface_uv.y};
  RNA_float_set_array(op->ptr, "space_anchor_surface_uv", anchor_uv);
  RNA_int_set(op->ptr, "space_anchor_tri", desc.anchor.tri);
  /* The exact restore cache (Redo / F9): the triangle's corner indices and the barycentric weights
   * of the anchor on it. `tri` alone cannot restore (the corner check would fail after a
   * replay), and the UV fallback is ambiguous on mirrored / overlapping UVs. */
  const int anchor_tri_corners[3] = {
      desc.anchor.tri_corners.x, desc.anchor.tri_corners.y, desc.anchor.tri_corners.z};
  RNA_int_set_array(op->ptr, "space_anchor_tri_corners", anchor_tri_corners);
  const float anchor_bary[3] = {desc.anchor.bary.x, desc.anchor.bary.y, desc.anchor.bary.z};
  RNA_float_set_array(op->ptr, "space_anchor_bary", anchor_bary);
  RNA_boolean_set(op->ptr, "space_anchor_has_uv", desc.anchor.has_surface_uv);
  RNA_float_set(op->ptr, "space_anchor_px_per_unit", desc.anchor.px_per_unit);
  RNA_float_set(op->ptr, "space_anchor_max_depth", desc.anchor.max_depth);
  RNA_string_set(op->ptr, "space_surface_uv_map", desc.anchor.surface_uv_map);
}

bool space_from_op_props(wmOperator *op, ShapeSpaceDesc &r_desc)
{
  const int type = RNA_int_get(op->ptr, "space_type");
  if (type < 0) {
    return false;
  }
  r_desc.type = ShapeSpaceType(type);
  r_desc.ref_tile = RNA_int_get(op->ptr, "space_ref_tile");
  int ref_tile_size[2];
  RNA_int_get_array(op->ptr, "space_ref_tile_size", ref_tile_size);
  r_desc.ref_tile_size = int2(ref_tile_size[0], ref_tile_size[1]);
  int win_size[2];
  RNA_int_get_array(op->ptr, "space_win_size", win_size);
  r_desc.win_size = int2(win_size[0], win_size[1]);
  r_desc.is_persp = RNA_boolean_get(op->ptr, "space_is_persp");
  r_desc.clip_start = RNA_float_get(op->ptr, "space_clip_start");
  r_desc.clip_end = RNA_float_get(op->ptr, "space_clip_end");
  float persmat[16];
  RNA_float_get_array(op->ptr, "space_persmat", persmat);
  memcpy(r_desc.persmat.ptr(), persmat, sizeof(persmat));
  float viewinv[16];
  RNA_float_get_array(op->ptr, "space_viewinv", viewinv);
  memcpy(r_desc.viewinv.ptr(), viewinv, sizeof(viewinv));

  /* SurfaceAnchored (R8). */
  float anchor_co[3];
  RNA_float_get_array(op->ptr, "space_anchor_co", anchor_co);
  r_desc.anchor.co = float3(anchor_co[0], anchor_co[1], anchor_co[2]);
  float anchor_normal[3];
  RNA_float_get_array(op->ptr, "space_anchor_normal", anchor_normal);
  r_desc.anchor.normal = float3(anchor_normal[0], anchor_normal[1], anchor_normal[2]);
  float anchor_tangent[3];
  RNA_float_get_array(op->ptr, "space_anchor_tangent", anchor_tangent);
  r_desc.anchor.tangent = float3(anchor_tangent[0], anchor_tangent[1], anchor_tangent[2]);
  float anchor_uv[2];
  RNA_float_get_array(op->ptr, "space_anchor_surface_uv", anchor_uv);
  r_desc.anchor.surface_uv = float2(anchor_uv[0], anchor_uv[1]);
  r_desc.anchor.tri = RNA_int_get(op->ptr, "space_anchor_tri");
  int anchor_tri_corners[3];
  RNA_int_get_array(op->ptr, "space_anchor_tri_corners", anchor_tri_corners);
  r_desc.anchor.tri_corners = int3(anchor_tri_corners[0], anchor_tri_corners[1], anchor_tri_corners[2]);
  float anchor_bary[3];
  RNA_float_get_array(op->ptr, "space_anchor_bary", anchor_bary);
  r_desc.anchor.bary = float3(anchor_bary[0], anchor_bary[1], anchor_bary[2]);
  r_desc.anchor.has_surface_uv = RNA_boolean_get(op->ptr, "space_anchor_has_uv");
  r_desc.anchor.px_per_unit = RNA_float_get(op->ptr, "space_anchor_px_per_unit");
  r_desc.anchor.max_depth = RNA_float_get(op->ptr, "space_anchor_max_depth");
  RNA_string_get(op->ptr, "space_surface_uv_map", r_desc.anchor.surface_uv_map);
  return true;
}

void space_op_properties_register(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Presence marker: a -1 default means a fresh invocation has no stored space. */
  prop = RNA_def_int(ot->srna,
                     "space_type",
                     -1,
                     -1,
                     int(ShapeSpaceType::SurfaceAnchored),
                     "Space Type",
                     "",
                     -1,
                     int(ShapeSpaceType::SurfaceAnchored));
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_int(
      ot->srna, "space_ref_tile", 1001, 1001, 2000, "Space Reference Tile", "", 1001, 2000);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_int_array(ot->srna,
                           "space_ref_tile_size",
                           2,
                           nullptr,
                           1,
                           65536,
                           "Space Reference Tile Size",
                           "",
                           1,
                           65536);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_boolean(ot->srna, "space_is_persp", false, "Space Perspective", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_int_array(
      ot->srna, "space_win_size", 2, nullptr, 0, 65536, "Space Window Size", "", 0, 65536);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float(ot->srna,
                       "space_clip_start",
                       0.0f,
                       -FLT_MAX,
                       FLT_MAX,
                       "Space Clip Start",
                       "",
                       -FLT_MAX,
                       FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float(ot->srna,
                       "space_clip_end",
                       1000.0f,
                       -FLT_MAX,
                       FLT_MAX,
                       "Space Clip End",
                       "",
                       -FLT_MAX,
                       FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float_array(ot->srna,
                             "space_persmat",
                             16,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Space Projection Matrix",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_float_array(ot->srna,
                             "space_viewinv",
                             16,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Space View Inverse",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  /* SurfaceAnchored anchor (R8). */
  prop = RNA_def_float_array(ot->srna,
                             "space_anchor_co",
                             3,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Anchor Point",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "space_anchor_normal",
                             3,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Anchor Normal",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "space_anchor_tangent",
                             3,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Anchor Tangent",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "space_anchor_surface_uv",
                             2,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Anchor Surface UV",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(
      ot->srna, "space_anchor_tri", -1, -1, INT_MAX, "Anchor Triangle", "", -1, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int_array(ot->srna,
                           "space_anchor_tri_corners",
                           3,
                           nullptr,
                           -1,
                           INT_MAX,
                           "Anchor Triangle Corners",
                           "",
                           -1,
                           INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "space_anchor_bary",
                             3,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Anchor Barycentric",
                             "",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_boolean(ot->srna, "space_anchor_has_uv", false, "Anchor Has UV", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(ot->srna,
                       "space_anchor_px_per_unit",
                       1.0f,
                       1e-6f,
                       FLT_MAX,
                       "Anchor Pixels Per Unit",
                       "",
                       1e-6f,
                       FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(ot->srna,
                       "space_anchor_max_depth",
                       0.0f,
                       0.0f,
                       FLT_MAX,
                       "Anchor Max Depth",
                       "",
                       0.0f,
                       FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_string(ot->srna,
                        "space_surface_uv_map",
                        nullptr,
                        64,
                        "Anchor UV Map",
                        "UV map the anchor was sampled through (empty: active)");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
