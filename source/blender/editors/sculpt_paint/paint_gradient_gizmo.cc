/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_gradient_gizmo.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_gradient.hh"
#include "BLI_math_vector.hh"

#include "BKE_colorband.hh"
#include "BKE_paint.hh"
#include "IMB_colormanagement.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

namespace blender::ed::sculpt_paint::gradient_gizmo {

/** Segment count of the handle discs. */
static constexpr int HANDLE_SEGMENTS = 16;

/** Alpha of the ramp-colored parts; a transparent ramp stop must not hide the handles. */
static constexpr float RAMP_OVERLAY_ALPHA = 0.95f;

namespace {

void line_vertex(const uint pos, const float2 &point)
{
  immVertex3f(pos, point.x, point.y, 0.0f);
}

/** A dark fill needs a light ring (and a light one a dark ring) to stay readable on any ramp. */
float4 outline_color_for(const float4 &fill)
{
  const float luma = 0.2126f * fill.x + 0.7152f * fill.y + 0.0722f * fill.z;
  return (luma < 0.3f) ? float4(0.95f, 0.95f, 0.95f, 0.85f) : float4(0.0f, 0.0f, 0.0f, 0.8f);
}

/** One pass of the line through the midpoint marker, in a single color. */
void draw_line_pass(const uint pos,
                    const float2 &start_px,
                    const float2 &end_px,
                    const float2 &mid_px,
                    const float width,
                    const float4 &color)
{
  GPU_line_width(width);
  immUniformColor4f(color.x, color.y, color.z, color.w);
  immBegin(GPU_PRIM_LINE_STRIP, 3);
  line_vertex(pos, start_px);
  line_vertex(pos, mid_px);
  line_vertex(pos, end_px);
  immEnd();
}

/**
 * The line: a dark under-stroke and a core. The core is white for the Color Gradient tool and the
 * row's ramp colors (start, half color, end) for the Gradient Layer gizmo.
 */
void draw_line(const float2 &start_px,
               const float2 &end_px,
               const float2 &mid_px,
               const DrawOptions &options)
{
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  draw_line_pass(pos, start_px, end_px, mid_px, 3.0f, float4(0.0f, 0.0f, 0.0f, 0.55f));
  if (!options.ramp_colors) {
    draw_line_pass(pos, start_px, end_px, mid_px, 1.5f, float4(1.0f, 1.0f, 1.0f, 0.9f));
    immUnbindProgram();
    return;
  }

  /* A white edge under the ramp colors: a dark stop must not make the line disappear against the
   * dark under-stroke. */
  draw_line_pass(pos, start_px, end_px, mid_px, 2.0f, float4(1.0f, 1.0f, 1.0f, 0.55f));
  immUnbindProgram();

  format = immVertexFormat();
  const uint pos_color = GPU_vertformat_attr_add(
      format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint color = GPU_vertformat_attr_add(
      format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_SMOOTH_COLOR);
  GPU_line_width(1.3f);
  immBegin(GPU_PRIM_LINE_STRIP, 3);
  const float2 points[3] = {start_px, mid_px, end_px};
  const float4 colors[3] = {options.color_start, options.color_mid, options.color_end};
  for (const int i : IndexRange(3)) {
    immAttr4f(color, colors[i].x, colors[i].y, colors[i].z, RAMP_OVERLAY_ALPHA);
    line_vertex(pos_color, points[i]);
  }
  immEnd();
  immUnbindProgram();
}

/** A filled triangle with the end color and a contrasting outline, pointing from start to end. */
void draw_direction_arrow(const float2 &start_px, const float2 &end_px, const DrawOptions &options)
{
  const float2 axis = end_px - start_px;
  const float length = math::length(axis);
  if (length < 26.0f) {
    return;
  }
  const float2 dir = axis / length;
  const float2 side(-dir.y, dir.x);
  const float size = math::clamp(length * 0.10f, 5.0f, 10.0f);
  /* Between the midpoint marker and the end handle, where the line still reads as a line. */
  const float2 center = math::interpolate(start_px, end_px, 0.72f);

  const float2 tip = center + dir * (size * 0.6f);
  const float2 base = center - dir * (size * 0.6f);
  const float2 left = base + side * (size * 0.45f);
  const float2 right = base - side * (size * 0.45f);

  const float2 outline_tip = center + dir * (size * 0.82f);
  const float2 outline_base = center - dir * (size * 0.82f);
  const float2 outline_left = outline_base + side * (size * 0.62f);
  const float2 outline_right = outline_base - side * (size * 0.62f);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  GPU_polygon_smooth(true);
  const float4 outline = outline_color_for(options.color_end);
  immUniformColor4f(outline.x, outline.y, outline.z, outline.w);
  immBegin(GPU_PRIM_TRIS, 3);
  line_vertex(pos, outline_tip);
  line_vertex(pos, outline_left);
  line_vertex(pos, outline_right);
  immEnd();

  immUniformColor4f(
      options.color_end.x, options.color_end.y, options.color_end.z, RAMP_OVERLAY_ALPHA);
  immBegin(GPU_PRIM_TRIS, 3);
  line_vertex(pos, tip);
  line_vertex(pos, left);
  line_vertex(pos, right);
  immEnd();
  GPU_polygon_smooth(false);

  immUnbindProgram();
}

/** One small disc per ramp stop, in the stop's color, drawn under the start/end handles. */
void draw_stop_handles(const float2 &start_px,
                       const float2 &end_px,
                       const DrawOptions &options)
{
  if (!options.stop_handles || options.stop_count <= 0) {
    return;
  }
  const float radius = HANDLE_RADIUS * 0.42f;

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  const int count = math::min(options.stop_count, MAX_STOP_HANDLES);
  for (const int i : IndexRange(count)) {
    const float4 color = options.stop_colors[i];
    const float2 point = math::interpolate(
        start_px, end_px, math::clamp(options.stop_positions[i], 0.0f, 1.0f));
    immUniformColor4f(color.x, color.y, color.z, RAMP_OVERLAY_ALPHA);
    imm_draw_circle_fill_3d(pos, point.x, point.y, radius, HANDLE_SEGMENTS);
  }

  GPU_line_width(1.0f);
  for (const int i : IndexRange(count)) {
    const float4 color = options.stop_colors[i];
    const float2 point = math::interpolate(
        start_px, end_px, math::clamp(options.stop_positions[i], 0.0f, 1.0f));
    const float4 outline = outline_color_for(color);
    immUniformColor4f(outline.x, outline.y, outline.z, outline.w);
    imm_draw_circle_wire_3d(pos, point.x, point.y, radius, HANDLE_SEGMENTS);
  }

  immUnbindProgram();
}

/** The start, end and midpoint discs; the start is the gradient's origin and can be larger. */
void draw_handles(const float2 &start_px,
                  const float2 &end_px,
                  const float2 &mid_px,
                  const DrawOptions &options)
{
  const float radius = HANDLE_RADIUS;
  const float start_radius = radius * options.start_radius_scale;
  const float end_radius = radius * 0.6f;
  const float mid_radius = radius * 0.45f;

  const float4 neutral(0.9f, 0.9f, 0.9f, 0.9f);
  /** A transparent ramp stop must still leave a visible handle. */
  const auto ramp_color = [](const float4 &color) {
    return float4(color.x, color.y, color.z, RAMP_OVERLAY_ALPHA);
  };
  const float4 start_color = options.ramp_colors ? ramp_color(options.color_start) : neutral;
  const float4 end_color = options.ramp_colors ? ramp_color(options.color_end) : neutral;
  const float4 mid_color = options.ramp_colors ? ramp_color(options.color_mid) : neutral;

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immUniformColor4f(start_color.x, start_color.y, start_color.z, start_color.w);
  imm_draw_circle_fill_3d(pos, start_px.x, start_px.y, start_radius, HANDLE_SEGMENTS);
  immUniformColor4f(end_color.x, end_color.y, end_color.z, end_color.w);
  imm_draw_circle_fill_3d(pos, end_px.x, end_px.y, end_radius, HANDLE_SEGMENTS);
  if (options.midpoint_handle) {
    immUniformColor4f(mid_color.x, mid_color.y, mid_color.z, mid_color.w);
    imm_draw_circle_fill_3d(pos, mid_px.x, mid_px.y, mid_radius, HANDLE_SEGMENTS);
  }

  GPU_line_width(1.0f);
  const float4 start_outline = outline_color_for(start_color);
  immUniformColor4f(start_outline.x, start_outline.y, start_outline.z, start_outline.w);
  imm_draw_circle_wire_3d(pos, start_px.x, start_px.y, start_radius, HANDLE_SEGMENTS);
  const float4 end_outline = outline_color_for(end_color);
  immUniformColor4f(end_outline.x, end_outline.y, end_outline.z, end_outline.w);
  imm_draw_circle_wire_3d(pos, end_px.x, end_px.y, end_radius, HANDLE_SEGMENTS);
  if (options.midpoint_handle) {
    const float4 mid_outline = outline_color_for(mid_color);
    immUniformColor4f(mid_outline.x, mid_outline.y, mid_outline.z, mid_outline.w);
    imm_draw_circle_wire_3d(pos, mid_px.x, mid_px.y, mid_radius, HANDLE_SEGMENTS);
  }

  immUnbindProgram();
}

}  // namespace

float2 midpoint_position(const float2 &start_px, const float2 &end_px, const float midpoint)
{
  return math::interpolate(start_px, end_px, midpoint);
}

int hit_test(const float2 &start_px,
             const float2 &end_px,
             const float midpoint,
             const float2 &mval,
             const float hit_radius,
             const bool midpoint_handle,
             const float *stop_positions,
             const int stop_count)
{
  /* Nearest marker wins, with each marker's drawn size as its hit radius. Picking the midpoint
   * first, as this used to, made the start handle unreachable on a short gradient: the midpoint
   * marker sits on top of it and every press near the start moved the midpoint instead. */
  struct Handle {
    int id;
    float2 point;
    float radius;
  };
  const Handle handles[3] = {
      {START, start_px, hit_radius},
      {END, end_px, hit_radius},
      {MID, midpoint_position(start_px, end_px, midpoint), hit_radius * 0.75f},
  };
  int best = NONE;
  float best_dist = hit_radius;
  for (const Handle &handle : handles) {
    if (handle.id == MID && !midpoint_handle) {
      continue;
    }
    const float dist = math::distance(handle.point, mval);
    if (dist <= handle.radius && dist <= best_dist) {
      best = handle.id;
      best_dist = dist;
    }
  }
  /* Ramp stops: smaller than the handles, and a strict comparison so a stop sitting on a start or
   * end handle never takes the press away from that handle. */
  if (stop_positions != nullptr) {
    const float stop_radius = hit_radius * 0.55f;
    const int count = math::min(stop_count, MAX_STOP_HANDLES);
    for (const int i : IndexRange(count)) {
      const float2 point = math::interpolate(
          start_px, end_px, math::clamp(stop_positions[i], 0.0f, 1.0f));
      const float dist = math::distance(point, mval);
      if (dist <= stop_radius && dist < best_dist) {
        best = STOP_FIRST + i;
        best_dist = dist;
      }
    }
  }
  return best;
}

void draw(const float2 &start_px,
          const float2 &end_px,
          const float midpoint,
          const DrawOptions &options)
{
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  /* The handles float over the scene; a depth test would be meaningless in pixel space. */
  const GPUDepthTest depth_prev = GPU_depth_test_get();
  GPU_depth_test(GPU_DEPTH_NONE);

  const float2 mid_px = midpoint_position(start_px, end_px, midpoint);
  draw_line(start_px, end_px, mid_px, options);
  if (options.direction_arrow) {
    draw_direction_arrow(start_px, end_px, options);
  }
  draw_stop_handles(start_px, end_px, options);
  draw_handles(start_px, end_px, mid_px, options);

  GPU_depth_test(depth_prev);
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

void apply_ramp(DrawOptions &options,
                const ColorBand &ramp,
                const bool use_grey,
                const bool stop_handles,
                const float midpoint)
{
  options.ramp_colors = true;
  options.color_start = ramp_display_color(ramp, 0.0f, use_grey);
  options.color_mid = ramp_display_color(ramp, 0.5f, use_grey);
  options.color_end = ramp_display_color(ramp, 1.0f, use_grey);

  options.stop_handles = stop_handles;
  options.stop_count = 0;
  if (stop_handles) {
    const int count = math::min(int(ramp.tot), MAX_STOP_HANDLES);
    for (const int i : IndexRange(count)) {
      const CBData &stop = ramp.data[i];
      float linear[3] = {stop.r, stop.g, stop.b};
      if (use_grey) {
        linear[0] = linear[1] = linear[2] = stop.r;
      }
      float display[3];
      IMB_colormanagement_scene_linear_to_srgb_v3(display, linear);
      options.stop_positions[i] = stop_line_position(ramp, i, midpoint);
      options.stop_colors[i] = float4(display[0], display[1], display[2], stop.a);
    }
    options.stop_count = count;
  }
}

float4 ramp_display_color(const ColorBand &ramp, const float t, const bool use_grey)
{
  float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  if (ramp.tot > 0) {
    BKE_colorband_evaluate(&ramp, t, rgba);
  }
  float linear[3] = {rgba[0], rgba[1], rgba[2]};
  if (use_grey) {
    linear[0] = linear[1] = linear[2] = rgba[0];
  }
  float display[3];
  IMB_colormanagement_scene_linear_to_srgb_v3(display, linear);
  /* A value ramp is a scalar the shaders read from the red channel; it has no alpha of its own. */
  return float4(display[0], display[1], display[2], use_grey ? 1.0f : rgba[3]);
}

ColorBand *display_ramp_for_channel(PaintModeSettings &paint_mode,
                                    ColorBand *tool_colour_ramp,
                                    bool &r_use_grey)
{
  r_use_grey = false;
  /* The getter also rejects an out-of-range channel index, a colour channel (which reads the
   * tool's colour ramp) and Normal (a packed tangent no ramp can drive). */
  ColorBand *value_ramp = BKE_paint_gradient_value_ramp_get(
      paint_mode, eMaterialPaintChannel(paint_mode.gradient_gizmo_channel));
  if (value_ramp != nullptr && value_ramp->tot > 0) {
    r_use_grey = true;
    return value_ramp;
  }
  if (tool_colour_ramp != nullptr && tool_colour_ramp->tot > 0) {
    return tool_colour_ramp;
  }
  return nullptr;
}

bool stop_drag_apply(ColorBand &ramp,
                     const int stop,
                     const float2 &start,
                     const float2 &end,
                     const float2 &mval,
                     const float min_len_sq)
{
  const float2 axis = end - start;
  const float len_sq = math::length_squared(axis);
  if (stop < 0 || stop >= ramp.tot || len_sq <= min_len_sq) {
    return false;
  }
  const float t = math::clamp(math::dot(mval - start, axis) / len_sq, 0.0f, 1.0f);
  const float lo = (stop > 0) ? ramp.data[stop - 1].pos : 0.0f;
  const float hi = (stop + 1 < ramp.tot) ? ramp.data[stop + 1].pos : 1.0f;
  ramp.data[stop].pos = math::clamp(t, lo, hi);
  return true;
}

void capture_stop_positions(Vector<GradientStopSnapshot> &snapshots, ColorBand &ramp)
{
  for (const GradientStopSnapshot &snapshot : snapshots) {
    if (snapshot.ramp == &ramp) {
      /* The first drag of a ramp is the one whose "before" a cancel has to restore. */
      return;
    }
  }
  snapshots.append(GradientStopSnapshot{});
  GradientStopSnapshot &snapshot = snapshots.last();
  snapshot.ramp = &ramp;
  snapshot.count = math::min(int(ramp.tot), MAX_STOP_HANDLES);
  for (const int i : IndexRange(snapshot.count)) {
    snapshot.positions[i] = ramp.data[i].pos;
  }
}

void restore_stop_positions(const Span<GradientStopSnapshot> &snapshots)
{
  for (const GradientStopSnapshot &snapshot : snapshots) {
    const int count = math::min(snapshot.count, int(snapshot.ramp->tot));
    for (const int i : IndexRange(count)) {
      snapshot.ramp->data[i].pos = snapshot.positions[i];
    }
  }
}

float stop_line_position(const ColorBand &ramp, const int index, const float midpoint)
{
  if (index < 0 || index >= ramp.tot) {
    return 0.0f;
  }
  return math::gradient_remap_midpoint_inverse(ramp.data[index].pos, midpoint);
}

}  // namespace blender::ed::sculpt_paint::gradient_gizmo
