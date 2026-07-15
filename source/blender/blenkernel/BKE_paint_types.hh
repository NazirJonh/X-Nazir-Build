/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"
#include "BLI_utility_mixins.hh"

namespace blender {

/** \file
 * \ingroup bke
 */

namespace ocio {
class ColorSpace;
}
struct AssetWeakReference;
enum class PaintMode : int8_t {
  Sculpt = 0,
  /** Vertex color. */
  Vertex = 1,
  Weight = 2,
  /** 3D view (projection painting). */
  Texture3D = 3,
  /** Image space (2D painting). */
  Texture2D = 4,
  GPencil = 6,
  /* Grease Pencil Vertex Paint */
  VertexGPencil = 7,
  SculptGPencil = 8,
  WeightGPencil = 9,
  /** Curves. */
  SculptCurves = 10,

  /** Keep last. */
  /* TODO: Shift the ordering so that invalid is first so that zero-initialization makes sense. */
  Invalid = 11,
};

namespace bke {
struct PaintRuntime : NonCopyable, NonMovable {
  bool initialized = false;
  uint16_t ob_mode = 0;
  PaintMode paint_mode = PaintMode::Invalid;
  AssetWeakReference *previous_active_brush_reference = nullptr;

  /**
   * Sticky "Use Texture Overlay" state for this session (not saved to file). Set whenever the
   * user toggles #BRUSH_OVERLAY_PRIMARY or edits #Brush::texture_overlay_alpha on the active
   * brush, then reapplied to whichever brush becomes active next in #BKE_paint_brush_set.
   */
  bool session_use_texture_overlay = false;
  int session_texture_overlay_alpha = 100;

  float2 last_rake = float2(0.0f, 0.0f);
  float last_rake_angle = 0.0f;

  int last_stroke_valid = false;
  float3 average_stroke_accum = float3(0.0f, 0.0f, 0.0f);
  int average_stroke_counter = 0;

  /**
   * How much brush should be rotated in the view plane, 0 means x points right, y points up.
   * The convention is that the brush's _negative_ Y axis points in the tangent direction (of the
   * mouse curve, Bezier curve, etc.)
   */
  float brush_rotation = 0.0f;
  float brush_rotation_sec = 0.0f;

  /*******************************************************************************
   * All data below are used to communicate with cursor drawing and tex sampling *
   *******************************************************************************/

  bool draw_anchored = false;
  int anchored_size = 0;

  /**
   * Normalization factor due to accumulated value of curve along spacing.
   * Calculated when brush spacing changes to dampen strength of stroke
   * if space attenuation is used.
   */
  float overlap_factor = 0.0f;
  bool draw_inverted = false;
  /** Check is there an ongoing stroke right now. */
  bool stroke_active = false;

  /**
   * Store last location of stroke or whether the mesh was hit.
   * Valid only while stroke is active.
   */
  float3 last_location = float3(0.0f, 0.0f, 0.0f);
  bool last_hit = false;

  float2 anchored_initial_mouse = float2(0.0f, 0.0f);

  /**
   * Radius of brush, pre-multiplied with pressure.
   * In case of anchored brushes contains the anchored radius.
   */
  float pixel_radius = 0.0f;
  float initial_pixel_radius = 0.0f;
  float start_pixel_radius = 0.0f;

  /** Evaluated size pressure value */
  float size_pressure_value = 0.0f;

  /** Position of mouse, used to sample the texture. */
  float2 tex_mouse = float2(0.0f, 0.0f);

  /** Position of mouse, used to sample the mask texture. */
  float2 mask_tex_mouse = float2(0.0f, 0.0f);

  /**
   * Scale applied to the texture sampling coordinates so that a non-square image texture keeps its
   * aspect ratio instead of being stretched to fill the square brush footprint. Only meaningful
   * when #MTEX_MAPPING_PRESERVE_ASPECT is enabled, and is `1` otherwise. Cached because computing
   * it requires acquiring the image buffer, which is far too expensive to do per sample.
   */
  float2 tex_aspect_correction = float2(1.0f);
  float2 mask_tex_aspect_correction = float2(1.0f);

  /** ColorSpace cache to avoid locking up during sampling. */
  bool do_linear_conversion = false;
  const ocio::ColorSpace *colorspace = nullptr;

  /** WM Paint cursor. */
  void *paint_cursor = nullptr;

  PaintRuntime();
  ~PaintRuntime();
};
};  // namespace bke

}  // namespace blender
