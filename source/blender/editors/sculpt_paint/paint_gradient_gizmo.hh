/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The screen-space line and handles shared by the Color Gradient tool's interactive session and
 * the Gradient Layer's persistent gizmo. The callers own the state and the projection; this only
 * draws the same shapes and picks the same handles, so both tools look and feel identical.
 */

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_colorband_types.h"

struct PaintModeSettings;

namespace blender::ed::sculpt_paint::gradient_gizmo {

/** What #hit_test found under the cursor. */
enum Handle {
  NONE = 0,
  START = 1,
  END = 2,
  MID = 3,
  /** Ramp stop markers follow: `STOP_FIRST + i` for stop \a i. */
  STOP_FIRST = 4,
};

/** Most stops a #DrawOptions can carry, matching #MAXCOLORBAND. */
constexpr int MAX_STOP_HANDLES = 32;

/** Whether \a handle is one of the ramp stop markers. */
inline bool is_stop_handle(const int handle)
{
  return handle >= STOP_FIRST;
}

/** Radius of the drawn handle discs, in region pixels. */
constexpr float HANDLE_RADIUS = 10.0f;

/** Screen position of the midpoint marker: along the handle line, at the halfway color point. */
float2 midpoint_position(const float2 &start_px, const float2 &end_px, float midpoint);

/**
 * The handle under \a mval, or #NONE. The nearest drawn marker wins. \a hit_radius widens the
 * pick area around the drawn discs without changing their size; the Gradient Layer gizmo uses a
 * larger one than the Color Gradient's interactive session.
 */
int hit_test(const float2 &start_px,
             const float2 &end_px,
             float midpoint,
             const float2 &mval,
             float hit_radius = HANDLE_RADIUS,
             bool midpoint_handle = true,
             const float *stop_positions = nullptr,
             int stop_count = 0);

/**
 * Optional styling for #draw. The Color Gradient uses the neutral overlay colors; the Gradient
 * Layer passes its row's ramp so the line, the handles and the direction arrow read as the
 * gradient the row actually blends.
 */
struct DrawOptions {
  /** Color the line and the handles from #color_start / #color_mid / #color_end. */
  bool ramp_colors = false;
  /** Samples of the row's ramp at 0, 0.5 and 1, in straight scene-linear RGBA. */
  float4 color_start = float4(1.0f);
  float4 color_mid = float4(1.0f);
  float4 color_end = float4(1.0f);
  /** Radius scale of the start disc; the end and midpoint markers keep their sizes. */
  float start_radius_scale = 0.6f;
  /** Draw a filled arrowhead on the line, pointing from the start towards the end. */
  bool direction_arrow = false;
  /** Draw the midpoint marker: the Ramp distribution of a Gradient Layer has no midpoint to move. */
  bool midpoint_handle = true;
  /** Draw one marker per ramp stop at `start + (end - start) * position`. */
  bool stop_handles = false;
  int stop_count = 0;
  /** Ramp stop positions (0..1) and their display-space colors. */
  float stop_positions[MAX_STOP_HANDLES] = {};
  float4 stop_colors[MAX_STOP_HANDLES] = {};
};

/**
 * Draw the line, its handles and (when #DrawOptions::stop_handles is set) one marker per ramp
 * stop, in region pixels. The caller sets a pixel-space projection (a `REGION_DRAW_POST_PIXEL`
 * callback or a 2D gizmo). Saves and restores the GPU state it changes.
 */
void draw(const float2 &start_px,
          const float2 &end_px,
          float midpoint,
          const DrawOptions &options = {});

/**
 * Fill \a options from \a ramp the way the Gradient Layer's gizmo does: the line and the handle
 * discs take the ramp's colors at 0, 0.5 and 1, and \a stop_handles adds one marker per ramp stop.
 * \a use_grey collapses every sampled color to its red channel, so a value ramp reads as grey.
 * The colors are converted from scene-linear to display space the way brush colors are.
 * \a midpoint is the gradient's effective midpoint: it does not affect the sampled colors, but it
 * bends where the stop markers land on the line (see #stop_line_position).
 */
void apply_ramp(DrawOptions &options,
                const ColorBand &ramp,
                bool use_grey,
                bool stop_handles,
                float midpoint);

/**
 * The line parameter of ramp stop \a index: the inverse of the midpoint bend, so a marker drawn
 * there shows the color the paint reads at that line position. With a centered midpoint this is
 * the stop's own position.
 */
float stop_line_position(const ColorBand &ramp, int index, float midpoint);

/**
 * The ramp a gradient tool's gizmo shows and edits: the grey value ramp of the channel selected in
 * #PaintModeSettings::gradient_gizmo_channel when it has one, else \a tool_colour_ramp (null when
 * there is none). A colour channel and Normal never have a value ramp
 * (see #BKE_paint_gradient_value_ramp_get), and an out-of-range channel index falls back to the
 * colour ramp as well.
 *
 * \a r_use_grey is set when the returned ramp is a value ramp: a scalar, drawn as the grey of its
 * red channel.
 */
ColorBand *display_ramp_for_channel(PaintModeSettings &paint_mode,
                                    ColorBand *tool_colour_ramp,
                                    bool &r_use_grey);

/**
 * The ramp's color at \a t, converted from scene-linear to display space the way brush colors are.
 * \a use_grey collapses the color to a grey of the ramp's red channel, which is how a value ramp
 * reads; such a ramp carries no transparency of its own, so it is returned opaque.
 */
float4 ramp_display_color(const ColorBand &ramp, float t, bool use_grey);

/**
 * Slide ramp stop \a stop to the point of the start -> end line under \a mval. The stop is clamped
 * between its neighbours, so the sorted order -- and with it the handle index -- survives the
 * drag.
 *
 * \a start, \a end and \a mval are in one space: the Color Gradient passes region pixels, the
 * Image Editor's gradient its UVs. \a min_len_sq is the shortest usable line in that space's
 * units. Returns false, writing nothing, for an out-of-range stop or a line too short to project
 * onto.
 */
bool stop_drag_apply(ColorBand &ramp,
                     int stop,
                     const float2 &start,
                     const float2 &end,
                     const float2 &mval,
                     float min_len_sq = 0.0f);

/**
 * The stop positions one ramp had before a stop drag edited them. Both gradient tools keep one
 * snapshot per ramp their drags touched, so a cancel can put every one of them back.
 */
struct GradientStopSnapshot {
  ColorBand *ramp = nullptr;
  int count = 0;
  float positions[MAX_STOP_HANDLES] = {};
};

/**
 * Record \a ramp's stop positions into \a snapshots, appending an entry unless that ramp already
 * has one: the first snapshot of a ramp is what a cancel restores. The callers run this when a
 * stop drag starts, not when the session starts -- the gizmo channel can be switched mid-session,
 * and the next drag then edits another ramp.
 */
void capture_stop_positions(Vector<GradientStopSnapshot> &snapshots, ColorBand &ramp);

/** Put the stop positions of every recorded ramp back (see #capture_stop_positions). */
void restore_stop_positions(const Span<GradientStopSnapshot> &snapshots);

}  // namespace blender::ed::sculpt_paint::gradient_gizmo
