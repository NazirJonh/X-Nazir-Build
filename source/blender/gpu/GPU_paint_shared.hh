/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_shader_shared_utils.hh"

/* -------------------------------------------------------------------- */
/** \name Paint Shared Structures
 * \{ */

/**
 * Packed pixel row for efficient GPU processing.
 * Represents a contiguous run of pixels on a single triangle face.
 * Layout: fields ordered by alignment requirements (largest first).
 */
struct [[host_shared]] PaintPixelRow {
  /** Barycentric coordinates for the starting pixel. */
  float2 start_barycentric_coord;
  /** Starting image coordinate (x, y). */
  uint2 start_image_coord;
  /** Index into triangles array. */
  uint uv_primitive_index;
  /** Number of pixels in this row. */
  uint num_pixels;
};

/**
 * Brush parameters passed to the shader via push constants.
 */
struct [[host_shared]] PaintBrushParams {
  float4 brush_color;
  float4 secondary_color;
  packed_float3 brush_location;
  float brush_radius;
  int blend_mode;
  int falloff_shape;
  float hardness;
  float rotation;
  int invert;
  float strength;
  float alpha;
  float texture_sample_bias;
  int _pad0;
  int _pad1;
  int _pad2;
  int _pad3;
};

/** \} */
