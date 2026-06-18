/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 * \brief GPU shader create infos for the brush texture preview rendering.
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "GPU_shader_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Texture element shader
 * \{ */

GPU_SHADER_INTERFACE_INFO(brush_texture_iface)
SMOOTH(float2, texCoord_interp)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(brush_texture_element)
VERTEX_IN(0, float2, pos)
VERTEX_IN(1, float2, texcoord)
VERTEX_IN(2, float, element_rotation)
VERTEX_OUT(brush_texture_iface)
FRAGMENT_OUT(0, float4, fragColor)
PUSH_CONSTANT(float4x4, ModelViewProjectionMatrix)
PUSH_CONSTANT(float4x4, u_texture_transform)
PUSH_CONSTANT(float2, u_element_center)
PUSH_CONSTANT(float2, u_element_scale)
PUSH_CONSTANT(float, u_element_rotation)
PUSH_CONSTANT(float4, u_color_tint)
PUSH_CONSTANT(float, u_opacity)
PUSH_CONSTANT(int, u_blend_mode)
PUSH_CONSTANT(float2, u_texture_scale)
PUSH_CONSTANT(float2, u_texture_offset)
PUSH_CONSTANT(float, u_texture_rotation)
SAMPLER(0, sampler2D, u_texture)
SAMPLER(1, sampler2D, u_mask_texture)
VERTEX_SOURCE("editors_interface_brush_brush_texture_vertex.glsl")
FRAGMENT_SOURCE("editors_interface_brush_brush_texture_fragment.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend shader
 * \{ */

GPU_SHADER_CREATE_INFO(brush_blend_advanced)
ADDITIONAL_INFO(gpu_fullscreen)
FRAGMENT_OUT(0, float4, fragColor)
PUSH_CONSTANT(float4, blend_params)
SAMPLER(0, sampler2D, u_base_texture)
SAMPLER(1, sampler2D, u_blend_texture)
SAMPLER(2, sampler2D, u_mask_texture)
FRAGMENT_SOURCE("editors_interface_brush_brush_blend_fragment.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compute shader
 * \{ */

GPU_SHADER_CREATE_INFO(brush_texture_compute)
LOCAL_GROUP_SIZE(8, 8, 1)
PUSH_CONSTANT(float4, pattern_params)
PUSH_CONSTANT(float4, viewport_data)
SAMPLER(0, sampler2D, u_input_texture)
IMAGE(0, SFLOAT_32_32_32_32, write, image2D, u_output_image)
COMPUTE_SOURCE("editors_interface_brush_brush_pattern_compute.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/** \} */
