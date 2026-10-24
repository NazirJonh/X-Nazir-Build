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

namespace blender::ed::sculpt_paint::gradient_gizmo {

/** What #hit_test found under the cursor. */
enum Handle {
  NONE = 0,
  START = 1,
  END = 2,
  MID = 3,
};

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
             float hit_radius = HANDLE_RADIUS);

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
};

/**
 * Draw the line and its three handles, in region pixels. The caller sets a pixel-space projection
 * (a `REGION_DRAW_POST_PIXEL` callback or a 2D gizmo). Saves and restores the GPU state it
 * changes.
 */
void draw(const float2 &start_px,
          const float2 &end_px,
          float midpoint,
          const DrawOptions &options = {});

}  // namespace blender::ed::sculpt_paint::gradient_gizmo
