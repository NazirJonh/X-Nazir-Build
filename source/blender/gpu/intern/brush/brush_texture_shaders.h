/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief GPU shader management for brush texture preview rendering.
 */

#pragma once

#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_batch.hh"

namespace blender::ed::interface {

/* -------------------------------------------------------------------- */
/** \name Shader Types
 * \{ */

enum eBrushTextureShaderType {
  BRUSH_SHADER_TEXTURE_ELEMENT = 0,
  BRUSH_SHADER_TEXTURE_PREVIEW,
  BRUSH_SHADER_BLEND_ADVANCED,
  BRUSH_SHADER_PATTERN_COMPUTE,
  BRUSH_SHADER_MAX
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Management
 * \{ */

/**
 * Get shader by type, creating it if necessary.
 * \param shader_type: Type of shader to retrieve
 * \return GPU shader or nullptr on failure
 */
blender::gpu::Shader *BKE_brush_texture_shader_get(eBrushTextureShaderType shader_type);

/**
 * Free all cached shaders.
 */
void BKE_brush_texture_shaders_free();

/**
 * Check if shader is valid
 */
bool BKE_brush_texture_shader_is_valid(blender::gpu::Shader *shader);

/**
 * Get shader error message
 */
const char *BKE_brush_texture_shader_get_error(blender::gpu::Shader *shader);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Uniforms
 * \{ */

/**
 * Set element parameters uniform
 */
void BKE_brush_texture_shader_set_element_params(
    blender::gpu::Shader *shader,
    float pos_x, float pos_y, float rotation, float scale
);

/**
 * Set viewport data uniform
 */
void BKE_brush_texture_shader_set_viewport_data(
    blender::gpu::Shader *shader,
    float width, float height, float aspect_ratio, float pixel_scale
);

/**
 * Set brush color uniform
 */
void BKE_brush_texture_shader_set_brush_color(
    blender::gpu::Shader *shader,
    float r, float g, float b, float a
);

/**
 * Set blend parameters uniform
 */
void BKE_brush_texture_shader_set_blend_params(
    blender::gpu::Shader *shader,
    float blend_factor, float blend_mode, float opacity, float contrast
);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Textures
 * \{ */

/**
 * Bind texture to shader
 */
void BKE_brush_texture_shader_bind_texture(
    blender::gpu::Shader *shader,
    blender::gpu::Texture *texture,
    int binding
);

/**
 * Bind multiple textures to shader
 */
void BKE_brush_texture_shader_bind_textures(
    blender::gpu::Shader *shader,
    blender::gpu::Texture *texture,
    blender::gpu::Texture *mask_texture,
    blender::gpu::Texture *blend_texture = nullptr
);

/** \} */

}  // namespace blender::ed::interface

/* C API */
extern "C" {

/* Forward declarations for C API */
typedef struct GPUShader GPUShader;

/* Shader type enum for C API */
typedef enum {
  BRUSH_SHADER_TEXTURE_ELEMENT_C = 0,
  BRUSH_SHADER_BLEND_ADVANCED_C,
  BRUSH_SHADER_PATTERN_COMPUTE_C,
  BRUSH_SHADER_MAX_C
} eBrushTextureShaderTypeC;

GPUShader *BKE_brush_texture_shader_get_c(eBrushTextureShaderTypeC shader_type);
void BKE_brush_texture_shaders_free_c();
bool BKE_brush_texture_shader_is_valid_c(GPUShader *shader);
const char *BKE_brush_texture_shader_get_error_c(GPUShader *shader);

void BKE_brush_texture_shader_set_element_params_c(
    GPUShader *shader,
    float pos_x, float pos_y, float rotation, float scale
);

void BKE_brush_texture_shader_set_viewport_data_c(
    GPUShader *shader,
    float width, float height, float aspect_ratio, float pixel_scale
);

void BKE_brush_texture_shader_set_brush_color_c(
    GPUShader *shader,
    float r, float g, float b, float a
);

void BKE_brush_texture_shader_set_blend_params_c(
    GPUShader *shader,
    float blend_factor, float blend_mode, float opacity, float contrast
);

void BKE_brush_texture_shader_bind_texture_c(
    GPUShader *shader,
    void *texture,
    int binding
);

void BKE_brush_texture_shader_bind_textures_c(
    GPUShader *shader,
    void *texture,
    void *mask_texture,
    void *blend_texture
);

}  // extern "C"