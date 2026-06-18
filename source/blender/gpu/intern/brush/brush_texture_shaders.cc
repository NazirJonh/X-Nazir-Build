/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief Implementation of GPU shader management for brush texture preview rendering.
 */

#include "brush_texture_shaders.h"

#include "GPU_shader.hh"
#include "GPU_context.hh"
#include "GPU_texture.hh"

namespace blender::ed::interface {

/* -------------------------------------------------------------------- */
/** \name Shader Management
 * \{ */

static blender::gpu::Shader *g_texture_element_shader = nullptr;
static blender::gpu::Shader *g_texture_preview_shader = nullptr;
static blender::gpu::Shader *g_blend_advanced_shader = nullptr;
static blender::gpu::Shader *g_texture_compute_shader = nullptr;

blender::gpu::Shader *BKE_brush_texture_shader_get(eBrushTextureShaderType shader_type)
{
  switch (shader_type) {
    case BRUSH_SHADER_TEXTURE_ELEMENT:
      if (!g_texture_element_shader) {
        g_texture_element_shader = GPU_shader_create_from_info_name("brush_texture_element");
        if (!g_texture_element_shader) {
          return nullptr;
        }
      }
      return g_texture_element_shader;
    case BRUSH_SHADER_TEXTURE_PREVIEW:
      if (!g_texture_preview_shader) {
        g_texture_preview_shader = GPU_shader_create_from_info_name("brush_texture_preview");
        if (!g_texture_preview_shader) {
          return nullptr;
        }
      }
      return g_texture_preview_shader;
      
    case BRUSH_SHADER_BLEND_ADVANCED:
      if (!g_blend_advanced_shader) {
        g_blend_advanced_shader = GPU_shader_create_from_info_name("brush_blend_advanced");
        if (!g_blend_advanced_shader) {
          return nullptr;
        }
      }
      return g_blend_advanced_shader;
      
    case BRUSH_SHADER_PATTERN_COMPUTE:
      if (!g_texture_compute_shader) {
        g_texture_compute_shader = GPU_shader_create_from_info_name("brush_texture_compute");
        if (!g_texture_compute_shader) {
          return nullptr;
        }
      }
      return g_texture_compute_shader;
      
    default:
      return nullptr;
  }
}

void BKE_brush_texture_shaders_free()
{
  if (g_texture_element_shader) {
    GPU_shader_free(g_texture_element_shader);
    g_texture_element_shader = nullptr;
  }

  if (g_texture_preview_shader) {
    GPU_shader_free(g_texture_preview_shader);
    g_texture_preview_shader = nullptr;
  }
  
  if (g_blend_advanced_shader) {
    GPU_shader_free(g_blend_advanced_shader);
    g_blend_advanced_shader = nullptr;
  }
  
  if (g_texture_compute_shader) {
    GPU_shader_free(g_texture_compute_shader);
    g_texture_compute_shader = nullptr;
  }
}

bool BKE_brush_texture_shader_is_valid(blender::gpu::Shader *shader)
{
  return shader != nullptr;
}

const char *BKE_brush_texture_shader_get_error(blender::gpu::Shader *shader)
{
  if (!shader) {
    return "Shader is null";
  }
  
  return "No error";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Uniforms
 * \{ */

void BKE_brush_texture_shader_set_element_params(
    blender::gpu::Shader *shader,
    float pos_x, float pos_y, float rotation, float scale
)
{
  if (!shader) {
    return;
  }
  
  float element_params[4] = {pos_x, pos_y, rotation, scale};
  GPU_shader_uniform_4fv(shader, "element_params", element_params);
}

void BKE_brush_texture_shader_set_viewport_data(
    blender::gpu::Shader *shader,
    float width, float height, float aspect_ratio, float pixel_scale
)
{
  if (!shader) {
    return;
  }
  
  float viewport_data[4] = {width, height, aspect_ratio, pixel_scale};
  GPU_shader_uniform_4fv(shader, "viewport_data", viewport_data);
}

void BKE_brush_texture_shader_set_brush_color(
    blender::gpu::Shader *shader,
    float r, float g, float b, float a
)
{
  if (!shader) {
    return;
  }
  
  float brush_color[4] = {r, g, b, a};
  GPU_shader_uniform_4fv(shader, "brush_color", brush_color);
}

void BKE_brush_texture_shader_set_blend_params(
    blender::gpu::Shader *shader,
    float blend_factor, float blend_mode, float opacity, float contrast
)
{
  if (!shader) {
    return;
  }
  
  float blend_params[4] = {blend_factor, blend_mode, opacity, contrast};
  GPU_shader_uniform_4fv(shader, "blend_params", blend_params);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Textures
 * \{ */

void BKE_brush_texture_shader_bind_texture(
    blender::gpu::Shader *shader,
    blender::gpu::Texture *texture,
    int binding
)
{
  if (!shader || !texture) {
    return;
  }
  
  GPU_texture_bind(texture, binding);
}

void BKE_brush_texture_shader_bind_textures(
    blender::gpu::Shader *shader,
    blender::gpu::Texture *texture,
    blender::gpu::Texture *mask_texture,
    blender::gpu::Texture *blend_texture
)
{
  if (!shader) {
    return;
  }
  
  if (texture) {
    GPU_texture_bind(texture, 0);
  }
  
  if (mask_texture) {
    GPU_texture_bind(mask_texture, 1);
  }
  
  if (blend_texture) {
    GPU_texture_bind(blend_texture, 2);
  }
}

/** \} */

}  // namespace blender::ed::interface

/* C API */
extern "C" {

GPUShader *BKE_brush_texture_shader_get_c(eBrushTextureShaderTypeC shader_type)
{
  blender::ed::interface::eBrushTextureShaderType cpp_type;
  switch (shader_type) {
    case BRUSH_SHADER_TEXTURE_ELEMENT_C:
      cpp_type = blender::ed::interface::BRUSH_SHADER_TEXTURE_ELEMENT;
      break;
    case BRUSH_SHADER_BLEND_ADVANCED_C:
      cpp_type = blender::ed::interface::BRUSH_SHADER_BLEND_ADVANCED;
      break;
    case BRUSH_SHADER_PATTERN_COMPUTE_C:
      cpp_type = blender::ed::interface::BRUSH_SHADER_PATTERN_COMPUTE;
      break;
    default:
      return nullptr;
  }
  
  return (GPUShader*)blender::ed::interface::BKE_brush_texture_shader_get(cpp_type);
}

void BKE_brush_texture_shaders_free_c()
{
  blender::ed::interface::BKE_brush_texture_shaders_free();
}

bool BKE_brush_texture_shader_is_valid_c(GPUShader *shader)
{
  return blender::ed::interface::BKE_brush_texture_shader_is_valid((blender::gpu::Shader*)shader);
}

const char *BKE_brush_texture_shader_get_error_c(GPUShader *shader)
{
  return blender::ed::interface::BKE_brush_texture_shader_get_error((blender::gpu::Shader*)shader);
}

void BKE_brush_texture_shader_set_element_params_c(
    GPUShader *shader,
    float pos_x, float pos_y, float rotation, float scale
)
{
  blender::ed::interface::BKE_brush_texture_shader_set_element_params(
    (blender::gpu::Shader*)shader, pos_x, pos_y, rotation, scale
  );
}

void BKE_brush_texture_shader_set_viewport_data_c(
    GPUShader *shader,
    float width, float height, float aspect_ratio, float pixel_scale
)
{
  blender::ed::interface::BKE_brush_texture_shader_set_viewport_data(
    (blender::gpu::Shader*)shader, width, height, aspect_ratio, pixel_scale
  );
}

void BKE_brush_texture_shader_set_brush_color_c(
    GPUShader *shader,
    float r, float g, float b, float a
)
{
  blender::ed::interface::BKE_brush_texture_shader_set_brush_color(
    (blender::gpu::Shader*)shader, r, g, b, a
  );
}

void BKE_brush_texture_shader_set_blend_params_c(
    GPUShader *shader,
    float blend_factor, float blend_mode, float opacity, float contrast
)
{
  blender::ed::interface::BKE_brush_texture_shader_set_blend_params(
    (blender::gpu::Shader*)shader, blend_factor, blend_mode, opacity, contrast
  );
}

void BKE_brush_texture_shader_bind_texture_c(
    GPUShader *shader,
    void *texture,
    int binding
)
{
  blender::ed::interface::BKE_brush_texture_shader_bind_texture(
    (blender::gpu::Shader*)shader, (blender::gpu::Texture*)texture, binding
  );
}

void BKE_brush_texture_shader_bind_textures_c(
    GPUShader *shader,
    void *texture,
    void *mask_texture,
    void *blend_texture
)
{
  blender::ed::interface::BKE_brush_texture_shader_bind_textures(
    (blender::gpu::Shader*)shader, 
    (blender::gpu::Texture*)texture, 
    (blender::gpu::Texture*)mask_texture, 
    (blender::gpu::Texture*)blend_texture
  );
}

}  // extern "C"