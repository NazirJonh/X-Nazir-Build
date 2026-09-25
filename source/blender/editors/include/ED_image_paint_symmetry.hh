/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Canvas-space symmetry of 2D texture painting (#ImagePaintSettings::symmetry_type): one
 * definition of the copies shared by brush strokes, selection gestures and the overlay.
 */

#pragma once

#include <optional>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

struct Image;
struct SpaceImage;

namespace blender::ed::image_paint_symmetry {

struct CanvasSymmetry {
  eImagePaint_SymmetryType type = IMAGE_PAINT_SYMMETRY_TYPE_LINE;
  float2 pivot = float2(0.5f);
  float angle = 0.0f;
  float radius = 0.25f;
  float width = 0.2f;
  int parallel_count = 1;

  float2 direction() const;
  float2 normal() const;

  /** Number of copies besides the original. */
  int copies_num() const;
  /** UV of the \a copy-th copy of \a uv. */
  float2 apply(int copy, const float2 &uv) const;
  /** Local isotropic scale of the mapping at \a uv (1 except for the circle inversion). */
  float scale_at(const float2 &uv) const;
  /**
   * #scale_at clamped for sizing brush dabs, so a stroke near the inversion center does not
   * flood the canvas (or vanish far from it).
   */
  float dab_scale_at(const float2 &uv) const;
  /**
   * UV Jacobian of the mapping at \a uv: how the copy moves when the original moves. A reflection
   * for the line and the circle, the identity for the parallel copies.
   */
  float2x2 jacobian(const float2 &uv) const;
  /** Whether the copies are mirror images (reversed orientation) rather than translations. */
  bool is_reflection() const;
};

/**
 * Canvas symmetry of \a settings when it is enabled and affects \a affect
 * (#IMAGE_PAINT_SYMMETRY_LINE_AFFECT_BRUSH or #IMAGE_PAINT_SYMMETRY_LINE_AFFECT_SELECTION).
 */
std::optional<CanvasSymmetry> from_settings(const ToolSettings &tool_settings,
                                            eImagePaint_SymmetryLineFlag affect);

/** Whether the 2D Canvas symmetry mode is in effect: it replaces the mesh (3D) symmetry. */
bool canvas_mode_active(const ToolSettings &tool_settings);
/** The symmetry as currently set, regardless of the enabled and affect flags. */
CanvasSymmetry from_settings_unconditional(const ImagePaintSettings &settings);

/**
 * \a uv_polygon mapped by the \a copy-th copy. The circle inversion bends straight edges into
 * arcs, so edges are subdivided before mapping. Empty when the copy is unbounded (a polygon
 * around the inversion center).
 */
Vector<float2> apply_polygon(const CanvasSymmetry &symmetry, int copy, Span<float2> uv_polygon);

/** UV origin (bottom-left corner) of the active UDIM tile of \a ima. */
float2 active_tile_origin(const Image *ima);

/**
 * Parameter range `[r_t0, r_t1]` of the line `pivot + t * direction` inside the unit tile at
 * \a tile_origin. \return false when the line misses the tile.
 */
bool line_clip_to_tile(const float2 &pivot,
                       const float2 &direction,
                       const float2 &tile_origin,
                       float &r_t0,
                       float &r_t1);

/* -------------------------------------------------------------------- */
/** \name Interactive editing session
 *
 * The handles are only drawn while #PAINT_OT_image_symmetry_edit runs.
 * \{ */

enum class EditHandle {
  None,
  /** Pivot or circle center: moves the whole symmetry. */
  Pivot,
  /** Line ends: rotate and set the displayed length. */
  EndA,
  EndB,
  /** Circle radius, or the spacing of the parallel copies. */
  Extent,
};

/** UV positions of the handles, shared by the overlay and the hit-testing of the session. */
struct EditHandles {
  float2 pivot;
  /** Ends of the displayed line (after #ImagePaintSettings::symmetry_line_length). */
  float2 end_a, end_b;
  bool has_ends = false;
  float2 extent;
  bool has_extent = false;
};
EditHandles edit_handles_calc(const ImagePaintSettings &settings, const float2 &tile_origin);

/** The Image Editor currently editing the symmetry, if any. */
bool edit_session_active(const SpaceImage *sima);
/** The handle under the cursor or being dragged, for highlighting. */
EditHandle edit_session_hot_handle();

/** \} */

}  // namespace blender::ed::image_paint_symmetry
