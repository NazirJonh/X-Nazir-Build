/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Pure math of a row's UV mapping: the affine map from canvas UV to the map coordinate a row
 * reads, and the drags that edit it. No context, no editor types: the Image Editor gizmo and a
 * future 3D adapter share this.
 *
 * The shader and the CPU compositor define the mapping as
 *
 *     sample_uv = scale * (R(rotation) * uv) + offset
 *
 * (rotation applied first, then scale, then the offset), with `uv` in the canvas unit square.
 * The visible footprint of the map on the canvas is the inverse image of that square, a
 * rectangle rotated by -rotation.
 */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

namespace blender::ed::sculpt_paint::uv_mapping {

/** The editable parameters of one row's mapping, by value. */
struct UvMappingXform {
  float2 offset = float2(0.0f);
  float2 scale = float2(1.0f);
  float rotation = 0.0f;
};

/** Which local axes a scale gesture changes. */
struct UvMappingScaleAxes {
  bool x = false;
  bool y = false;
};

/** `A = scale * R(rotation)`: canvas UV -> the coordinate read from the map. */
float3x3 uv_mapping_sample_matrix(const UvMappingXform &xform);

/** `P = A^-1`: the coordinate read from the map -> canvas UV (the visible footprint). */
float3x3 uv_mapping_footprint_matrix(const UvMappingXform &xform);

/** The map coordinate read at canvas point \a canvas_uv: the inverse of #uv_mapping_footprint_point. */
float2 uv_mapping_map_point(const UvMappingXform &xform, const float2 &canvas_uv);

/** Footprint of the map point \a map_uv in canvas UV. */
float2 uv_mapping_footprint_point(const UvMappingXform &xform, const float2 &map_uv);

/** Centre of the map's unit square on the canvas. */
float2 uv_mapping_footprint_center(const UvMappingXform &xform);

/**
 * Move the footprint by \a delta_uv (canvas UV). The mapping offset moves the opposite way,
 * because it is added after the rotation and scale.
 */
UvMappingXform uv_mapping_drag_move(const UvMappingXform &start, const float2 &delta_uv);

/**
 * Rotate the mapping around \a pivot_uv (canvas UV). The footprint turns by +angle while the
 * stored rotation decreases; the offset is recomputed so the pivot stays where it is.
 */
UvMappingXform uv_mapping_drag_rotate(const UvMappingXform &start,
                                      const float2 &pivot_uv,
                                      float delta_angle);

/**
 * Scale the footprint around \a pivot_uv (canvas UV) from a drag that started at
 * \a mouse_start_uv and is now at \a mouse_uv. Both are canvas UV.
 *
 * The drag is read in the cage's own axes (the rectangle's edges, rotated by -rotation), so a
 * corner follows the mouse and a side handle only its own axis. The cage grows while the stored
 * scale shrinks. A sign change is a legal mirror. With \a uniform both axes take the projection
 * of the drag onto the start direction, matching the row's scale lock.
 */
UvMappingXform uv_mapping_drag_scale(const UvMappingXform &start,
                                     const float2 &pivot_uv,
                                     const float2 &mouse_start_uv,
                                     const float2 &mouse_uv,
                                     const UvMappingScaleAxes &axes,
                                     bool uniform);

}  // namespace blender::ed::sculpt_paint::uv_mapping
