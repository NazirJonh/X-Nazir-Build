/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "gpu_interface_infos.hh"
#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Compute Shader Create Info
 * \{ */

GPU_SHADER_CREATE_INFO(gpu_paint_compute_base)
LOCAL_GROUP_SIZE(1)
/* Target image for painting. */
IMAGE(0, SFLOAT_32_32_32_32, read_write, image2D, target_image)
/* Brush texture (optional). */
SAMPLER(0, sampler2D, brush_texture)
/* Vertex positions for triangle vertices. */
STORAGE_BUF(0, read, float3, vertex_positions[])
/* Triangle indices. */
STORAGE_BUF(1, read, uint3, triangles[])
/* Automasking factors (optional). */
STORAGE_BUF(2, read, float, automask_factors[])
/* Pixel rows for processing - each row uses 2 uint4:
 * [0]: barycentric.x (as float bits), barycentric.y (as float bits), image_coord.x, image_coord.y
 * [1]: uv_primitive_index, num_pixels, padding, padding */
STORAGE_BUF(3, read, uint4, pixel_rows[])
/* Push constants for brush parameters. */
PUSH_CONSTANT(int2, image_size)
PUSH_CONSTANT(int, num_pixel_rows)
PUSH_CONSTANT(float3, brush_location)
PUSH_CONSTANT(float, brush_radius)
PUSH_CONSTANT(float, brush_strength)
PUSH_CONSTANT(float, brush_alpha)
PUSH_CONSTANT(float, hardness)
PUSH_CONSTANT(float, brush_tex_rotation)
PUSH_CONSTANT(float2, brush_tex_size)
PUSH_CONSTANT(float2, brush_tex_offset)
PUSH_CONSTANT(float, texture_sample_bias)
PUSH_CONSTANT(int, blend_mode)
PUSH_CONSTANT(int, falloff_shape)
PUSH_CONSTANT(int, invert)
PUSH_CONSTANT(int, has_brush_texture)
PUSH_CONSTANT(int, has_automask)
PUSH_CONSTANT(float4, brush_color)
PUSH_CONSTANT(float4, secondary_color)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(gpu_paint_compute)
ADDITIONAL_INFO(gpu_paint_compute_base)
COMPUTE_SOURCE("gpu_paint_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/** \} */
