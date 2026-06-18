/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief Implementation of brush texture pattern generation.
 */

#include "brush_texture_preview_api.h"
#include "brush_texture_shaders.h"

#include "DNA_brush_types.h"
#include "DNA_brush_enums.h"
#include "DNA_texture_types.h"
#include "DNA_node_types.h"

#include "BKE_brush.hh"
#include "BKE_texture.h"
#include "BKE_node_legacy_types.hh"


#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_rand_c.hh"
#include "BLI_rect.hh"
#include "BLI_vector.hh"
#include "BLI_listbase.hh"

#include <cmath>

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "GPU_batch.hh"
#include "GPU_framebuffer.hh"
#include "GPU_shader.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_uniform_buffer.hh"
#include "GPU_immediate.hh"

#include "MEM_guardedalloc.h"

namespace blender::ed::interface {

/* Forward declarations */
static bool render_texture_element(BrushTexturePreview *preview, const TextureElement &element);

/* -------------------------------------------------------------------- */
/** \name Pattern Generation
 * \{ */

/* Forward declarations */
static bool generate_stroke_pattern(BrushTexturePreview *preview, const Brush *brush);
static bool generate_random_pattern(BrushTexturePreview *preview, const Brush *brush);
static bool generate_grid_pattern(BrushTexturePreview *preview, const Brush *brush);
static bool generate_radial_pattern(BrushTexturePreview *preview, const Brush *brush);
static bool generate_spiral_pattern(BrushTexturePreview *preview, const Brush *brush);

bool BKE_brush_texture_pattern_generate(BrushTexturePreview *preview, const Brush *brush)
{
  if (!preview || !brush) {
    return false;
  }
  
  /* Clear existing pattern elements */
  preview->pattern_elements.clear();
  
  /* Generate pattern based on distribution type */
  switch (preview->pattern_params.distribution_type) {
    case BRUSH_TEX_DIST_STROKE:
      return generate_stroke_pattern(preview, brush);
    case BRUSH_TEX_DIST_RANDOM:
      return generate_random_pattern(preview, brush);
    case BRUSH_TEX_DIST_GRID:
      return generate_grid_pattern(preview, brush);
    case BRUSH_TEX_DIST_RADIAL:
      return generate_radial_pattern(preview, brush);
    case BRUSH_TEX_DIST_SPIRAL:
      return generate_spiral_pattern(preview, brush);
    default:
      return generate_stroke_pattern(preview, brush);
  }
}

static float2 pixel_to_ndc(float x, float y, int width, int height)
{
  return float2(2.0f * x / float(width) - 1.0f, 2.0f * y / float(height) - 1.0f);
}

static bool generate_stroke_pattern(BrushTexturePreview *preview, const Brush *brush)
{
  const int width = preview->preview_size.x;
  const int height = preview->preview_size.y;
  const float preview_size = min_ff(float(width), float(height)) * 0.7f;
  const float center_x = float(width) * 0.5f;
  const float center_y = float(height) * 0.5f;

  const float stroke_angle = preview->stroke_params.angle;
  const float pattern_spacing = preview->stroke_params.spacing;

  const float preview_width = preview_size * 2.0f;
  const float preview_height = preview_size;
  float footprint_size = min_ff(float(brush->size), preview_height * 0.8f);
  footprint_size = max_ff(footprint_size, 16.0f);

  float spacing_clamped = max_ff(pattern_spacing, 1.0f);
  spacing_clamped = min_ff(spacing_clamped, 500.0f);
  const float spacing_normalized = (spacing_clamped - 1.0f) / (500.0f - 1.0f);
  int max_elements = int(30.0f - (30.0f - 3.0f) * spacing_normalized);
  max_elements = max_ii(max_elements, 3);
  max_elements = min_ii(max_elements, 30);

  float step_distance = footprint_size * (spacing_clamped / 100.0f);
  step_distance = max_ff(step_distance, footprint_size * 0.1f);
  if (max_elements > 1) {
    step_distance = (preview_width * 0.8f) / float(max_elements - 1);
  }

  const float start_x = center_x - (preview_width * 0.4f);
  const float base_amplitude = preview_height * 0.15f;
  const float max_amplitude = preview_height * 0.4f;
  const float periods = 1.5f;

  RNG *rng = BLI_rng_new(0);

  const bool use_jitter = (brush->flag & BRUSH_ABSOLUTE_JITTER) ? (brush->jitter_absolute != 0) :
                                                                  (brush->jitter != 0.0f);
  const float jitter_amount = use_jitter ? ((brush->flag & BRUSH_ABSOLUTE_JITTER) ?
                                                float(brush->jitter_absolute) :
                                                brush->jitter) :
                                           0.0f;

  const bool use_random = (brush->mtex.brush_angle_mode & MTEX_ANGLE_RANDOM) != 0;
  const float random_angle = brush->mtex.random_angle;

  const float size_factor = brush->size / 100.0f;
  const float base_element_size = (preview_size * size_factor) * 0.3f;
  const float max_element_size = (preview_size * size_factor) * 0.8f;
  const float min_element_size = max_ff(base_element_size * 0.5f, 15.0f);

  for (int i = 0; i < max_elements; i++) {
    float x = start_x + float(i) * step_distance;
    const float progress = (max_elements > 1) ? float(i) / float(max_elements - 1) : 0.0f;
    const float sine_phase = progress * periods * 2.0f * float(M_PI);
    const float current_amplitude = base_amplitude + (max_amplitude - base_amplitude) * progress;
    float y = center_y + current_amplitude * sinf(sine_phase);

    if (use_jitter) {
      float rand_pos[2];
      do {
        rand_pos[0] = BLI_rng_get_float(rng) - 0.5f;
        rand_pos[1] = BLI_rng_get_float(rng) - 0.5f;
      } while ((rand_pos[0] * rand_pos[0] + rand_pos[1] * rand_pos[1]) > 0.25f);

      const float jitter_radius = (brush->flag & BRUSH_ABSOLUTE_JITTER) ?
                                      jitter_amount :
                                      preview_size * 0.1f * jitter_amount;
      x += 2.0f * rand_pos[0] * jitter_radius;
      y += 2.0f * rand_pos[1] * jitter_radius;
    }

    float element_angle = stroke_angle;
    if (use_random && random_angle > 0.0f) {
      const float random_factor = BLI_rng_get_float(rng);
      element_angle += -random_angle / 2.0f + random_angle * random_factor;
    }

    float element_size = base_element_size + (max_element_size - base_element_size) * progress;
    element_size = max_ff(element_size, min_element_size);

    TextureElement element;
    element.position = pixel_to_ndc(x, y, width, height);
    element.size = float2(
        element_size / float(width) * 2.0f,
        element_size / float(height) * 2.0f);
    element.rotation = element_angle;
    element.color = float3(1.0f, 1.0f, 1.0f);
    element.opacity = brush->alpha;
    element.uv_offset = float2(0.0f, 0.0f);
    element.uv_scale = float2(1.0f, 1.0f);
    element.blend_mode = brush->mtex.tex ?
                             BKE_brush_texture_convert_blend_mode(brush->mtex.blendtype) :
                             BRUSH_TEX_BLEND_MIX;

    preview->pattern_elements.append(element);
  }

  BLI_rng_free(rng);
  return true;
}

static bool generate_random_pattern(BrushTexturePreview *preview, const Brush *brush)
{
  printf("[DEBUG] generate_random_pattern: Starting with seed %u\n", preview->pattern_params.random_seed);
  
  RNG *rng = BLI_rng_new(preview->pattern_params.random_seed);
  
  int element_count = preview->pattern_params.element_count;
  float density = preview->pattern_params.density;
  
  /* Adjust element count based on density */
  element_count = int(element_count * density);
  
  printf("[DEBUG] generate_random_pattern: Generating %d elements (density %.2f)\n", element_count, density);
  
  for (int i = 0; i < element_count; i++) {
    TextureElement element;
    
    /* Random position */
    element.position = float2(
        BLI_rng_get_float(rng) * 2.0f - 1.0f,
        BLI_rng_get_float(rng) * 2.0f - 1.0f
    );
    
    /* Size variation */
    float size_factor = BLI_rng_get_float(rng) * 
        (preview->pattern_params.size_variation.y - preview->pattern_params.size_variation.x) + 
        preview->pattern_params.size_variation.x;
    element.size = float2(size_factor, size_factor);
    
    /* Rotation variation */
    element.rotation = BLI_rng_get_float(rng) * preview->pattern_params.rotation_variation;
    
    /* Color and opacity from brush */
    element.color = float3(1.0f, 1.0f, 1.0f);
    element.opacity = brush->alpha;
    
    /* UV coordinates for texture sampling */
    element.uv_offset = float2(
        BLI_rng_get_float(rng),
        BLI_rng_get_float(rng)
    );
    element.uv_scale = float2(1.0f, 1.0f);
    
    /* Blend mode from brush texture */
    if (brush->mtex.tex) {
      element.blend_mode = BKE_brush_texture_convert_blend_mode(brush->mtex.blendtype);
    } else {
      element.blend_mode = BRUSH_TEX_BLEND_MIX;
    }
    
    preview->pattern_elements.append(element);
  }
  
  BLI_rng_free(rng);
  
  printf("[DEBUG] generate_random_pattern: Generated %d elements\n", int(preview->pattern_elements.size()));
  
  return true;
}

static bool generate_grid_pattern(BrushTexturePreview *preview, const Brush *brush)
{
  int grid_size = int(sqrt(preview->pattern_params.element_count));
  float step = 2.0f / grid_size;
  
  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      TextureElement element;
      
      /* Grid position */
      element.position = float2(
          -1.0f + (x + 0.5f) * step,
          -1.0f + (y + 0.5f) * step
      );
      
      /* Uniform size */
      element.size = float2(step * 0.8f, step * 0.8f);
      element.rotation = 0.0f;
      
      /* Color and opacity from brush */
      element.color = float3(1.0f, 1.0f, 1.0f);
      element.opacity = brush->alpha;
      
      /* UV coordinates */
      element.uv_offset = float2(float(x) / grid_size, float(y) / grid_size);
      element.uv_scale = float2(1.0f / grid_size, 1.0f / grid_size);
      
      /* Blend mode */
      if (brush->mtex.tex) {
        element.blend_mode = BKE_brush_texture_convert_blend_mode(brush->mtex.blendtype);
      } else {
        element.blend_mode = BRUSH_TEX_BLEND_MIX;
      }
      
      preview->pattern_elements.append(element);
    }
  }
  
  return true;
}

static bool generate_radial_pattern(BrushTexturePreview *preview, const Brush *brush)
{
  int rings = int(sqrt(preview->pattern_params.element_count / 4));
  
  for (int ring = 0; ring < rings; ring++) {
    float radius = float(ring + 1) / rings;
    int elements_in_ring = std::max(1, int(2 * M_PI * radius * preview->pattern_params.element_count / 10));
    
    for (int i = 0; i < elements_in_ring; i++) {
      TextureElement element;
      
      /* Radial position */
      float angle = (2.0f * M_PI * i) / elements_in_ring;
      element.position = float2(
          radius * cos(angle),
          radius * sin(angle)
      );
      
      /* Size decreases with radius */
      float size_factor = 1.0f - radius * 0.5f;
      element.size = float2(size_factor * 0.1f, size_factor * 0.1f);
      element.rotation = angle;
      
      /* Color and opacity */
      element.color = float3(1.0f, 1.0f, 1.0f);
      element.opacity = brush->alpha * (1.0f - radius * 0.3f);
      
      /* UV coordinates */
      element.uv_offset = float2(
          (element.position.x + 1.0f) * 0.5f,
          (element.position.y + 1.0f) * 0.5f
      );
      element.uv_scale = float2(1.0f, 1.0f);
      
      /* Blend mode */
      if (brush->mtex.tex) {
        element.blend_mode = BKE_brush_texture_convert_blend_mode(brush->mtex.blendtype);
      } else {
        element.blend_mode = BRUSH_TEX_BLEND_MIX;
      }
      
      preview->pattern_elements.append(element);
    }
  }
  
  return true;
}

static bool generate_spiral_pattern(BrushTexturePreview *preview, const Brush *brush)
{
  int element_count = preview->pattern_params.element_count;
  float spiral_turns = 3.0f;
  
  for (int i = 0; i < element_count; i++) {
    TextureElement element;
    
    /* Spiral position */
    float t = float(i) / element_count;
    float angle = t * spiral_turns * 2.0f * M_PI;
    float radius = t;
    
    element.position = float2(
        radius * cos(angle),
        radius * sin(angle)
    );
    
    /* Size decreases along spiral */
    float size_factor = 1.0f - t * 0.7f;
    element.size = float2(size_factor * 0.08f, size_factor * 0.08f);
    element.rotation = angle;
    
    /* Color and opacity */
    element.color = float3(1.0f, 1.0f, 1.0f);
    element.opacity = brush->alpha * (1.0f - t * 0.2f);
    
    /* UV coordinates */
    element.uv_offset = float2(
        (element.position.x + 1.0f) * 0.5f,
        (element.position.y + 1.0f) * 0.5f
    );
    element.uv_scale = float2(1.0f, 1.0f);
    
    /* Blend mode */
    if (brush->mtex.tex) {
      element.blend_mode = BKE_brush_texture_convert_blend_mode(brush->mtex.blendtype);
    } else {
      element.blend_mode = BRUSH_TEX_BLEND_MIX;
    }
    
    preview->pattern_elements.append(element);
  }
  
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rendering Functions
 * \{ */

bool BKE_brush_texture_preview_render(BrushTexturePreview *preview)
{
  if (!preview || !preview->gpu_context.initialized) {
    printf("[DEBUG] BKE_brush_texture_preview_render: Invalid preview or uninitialized GPU context\n");
    return false;
  }

  printf("[DEBUG] BKE_brush_texture_preview_render: Starting render with %d elements\n", int(preview->pattern_elements.size()));

  if (!preview->main_texture_cache.gpu_texture) {
    printf("[DEBUG] BKE_brush_texture_preview_render: No main texture in cache\n");
    return false;
  }
  
  BrushTextureGPUContext *gpu_ctx = &preview->gpu_context;

  /* Save caller's framebuffer and viewport so we can restore them afterwards.
   * GPU_framebuffer_restore() only reverts to the default back-buffer, which
   * breaks Blender's offscreen UI pipeline. */
  gpu::FrameBuffer *prev_fb = GPU_framebuffer_active_get();
  int prev_viewport[4];
  GPU_viewport_size_get_i(prev_viewport);

  /* Bind framebuffer */
  GPU_framebuffer_bind(gpu_ctx->framebuffer);
  GPU_viewport(0, 0, gpu_ctx->viewport_size.x, gpu_ctx->viewport_size.y);

  printf("[DEBUG] BKE_brush_texture_preview_render: Framebuffer bound, viewport %dx%d\n",
         gpu_ctx->viewport_size.x, gpu_ctx->viewport_size.y);
  
  /* Clear render target */
  const blender::double4 clear_color = blender::double4(0.0, 0.0, 0.0, 0.0);
  GPU_framebuffer_clear_color(gpu_ctx->framebuffer, clear_color);
  GPU_framebuffer_clear_depth(gpu_ctx->framebuffer, 1.0f);
  
  /* Setup render state */
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_NONE);
  
  printf("[DEBUG] BKE_brush_texture_preview_render: Rendering %d elements\n", int(preview->pattern_elements.size()));
  
  /* Render each texture element */
  int rendered_count = 0;
  for (const TextureElement &element : preview->pattern_elements) {
    if (render_texture_element(preview, element)) {
      rendered_count++;
    }
  }
  
  printf("[DEBUG] BKE_brush_texture_preview_render: Successfully rendered %d/%d elements\n", 
         rendered_count, int(preview->pattern_elements.size()));
  
  /* Restore render state */
  GPU_blend(GPU_BLEND_NONE);
  
  /* Read a sample pixel to verify render actually wrote data */
  float render_sample[4] = {0};
  GPU_framebuffer_read_color(gpu_ctx->framebuffer, 
                             gpu_ctx->viewport_size.x / 2, 
                             gpu_ctx->viewport_size.y / 2, 
                             1, 1, 4, 0, GPU_DATA_FLOAT, static_cast<void *>(render_sample));
  
  printf("[DEBUG] BKE_brush_texture_preview_render: Center pixel RGBA = [%.3f, %.3f, %.3f, %.3f]\n",
         render_sample[0], render_sample[1], render_sample[2], render_sample[3]);
  
  /* Restore the caller's framebuffer and viewport.
   * Must be done before the memory barrier so subsequent UI draw calls go to
   * the correct offscreen buffer with the correct viewport. */
  if (prev_fb) {
    GPU_framebuffer_bind(prev_fb);
  }
  else {
    GPU_framebuffer_restore();
  }
  GPU_viewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

  /* Ensure written color is visible to subsequent sampling. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);

  return true;
}

static bool render_texture_element(BrushTexturePreview *preview, const TextureElement &element)
{
  BrushTextureGPUContext *gpu_ctx = &preview->gpu_context;
  
  if (!gpu_ctx->quad_batch) {
    printf("[DEBUG] render_texture_element: No quad batch\n");
    return false;
  }

  if (!gpu_ctx->texture_shader) {
    printf("[DEBUG] render_texture_element: No texture shader\n");
    return false;
  }

  if (!BKE_brush_texture_shader_is_valid(gpu_ctx->texture_shader)) {
    printf("[DEBUG] render_texture_element: Shader is invalid\n");
    return false;
  }


  if (!preview->main_texture_cache.gpu_texture) {
    printf("[DEBUG] render_texture_element: No main texture\n");
    return false;
  }

  /* Validate batch before binding the shader. */
  if (!gpu_ctx->quad_batch) {
    printf("[DEBUG] render_texture_element: Batch validation failed\n");
    return false;
  }


  /* Assign shader to batch so GPU_batch_draw uses it. */
  GPU_batch_set_shader(gpu_ctx->quad_batch, gpu_ctx->texture_shader);
  const char *shader_name = GPU_shader_get_name(gpu_ctx->texture_shader);
  if (!shader_name) {
    return false;
  }
  
  /* Setup push-constant uniforms expected by the shader. */
  const float4x4 mvp = float4x4::identity();
  const float4x4 tex_xform = float4x4::identity();

  GPU_shader_uniform_mat4(
      gpu_ctx->texture_shader, "ModelViewProjectionMatrix", (const float(*)[4])mvp.ptr());
  GPU_shader_uniform_mat4(
      gpu_ctx->texture_shader, "u_texture_transform", (const float(*)[4])tex_xform.ptr());

  GPU_shader_uniform_2f(gpu_ctx->texture_shader, "u_element_center", element.position.x, element.position.y);
  GPU_shader_uniform_2f(gpu_ctx->texture_shader, "u_element_scale", element.size.x, element.size.y);
  GPU_shader_uniform_1f(gpu_ctx->texture_shader, "u_element_rotation", element.rotation);
  GPU_shader_uniform_4f(gpu_ctx->texture_shader, "u_color_tint", element.color.x, element.color.y, element.color.z, 1.0f);
  GPU_shader_uniform_1f(gpu_ctx->texture_shader, "u_opacity", element.opacity);
  GPU_shader_uniform_1i(gpu_ctx->texture_shader, "u_blend_mode", element.blend_mode);
  GPU_shader_uniform_2f(gpu_ctx->texture_shader, "u_texture_scale", element.uv_scale.x, element.uv_scale.y);
  GPU_shader_uniform_2f(gpu_ctx->texture_shader, "u_texture_offset", element.uv_offset.x, element.uv_offset.y);
  GPU_shader_uniform_1f(gpu_ctx->texture_shader, "u_texture_rotation", 0.0f);

  /* Bind shader first so texture bindings apply to correct descriptor set. */
  GPU_shader_bind(gpu_ctx->texture_shader);

  /* Query sampler bindings to avoid binding to nonexistent slots. */
  const int tex_binding = GPU_shader_get_sampler_binding(gpu_ctx->texture_shader, "u_texture");
  const int mask_binding = GPU_shader_get_sampler_binding(gpu_ctx->texture_shader, "u_mask_texture");

  /* Bind textures using reported bindings. */
  if (tex_binding >= 0 && preview->main_texture_cache.gpu_texture) {
    GPU_texture_bind(preview->main_texture_cache.gpu_texture, tex_binding);
  }

  /* Fall back to the context's 1x1 white texture so the mask sampler is always bound. */
  blender::gpu::Texture *mask_tex = preview->mask_texture_cache.gpu_texture ?
                                        preview->mask_texture_cache.gpu_texture :
                                        gpu_ctx->dummy_mask_texture;
  if (mask_binding >= 0 && mask_tex) {
    GPU_texture_bind(mask_tex, mask_binding);
  }

  /* Setup blend mode */
  switch (element.blend_mode) {
    case BRUSH_TEX_BLEND_MIX:
      GPU_blend(GPU_BLEND_ALPHA);
      break;
    case BRUSH_TEX_BLEND_MULTIPLY:
      GPU_blend(GPU_BLEND_MULTIPLY);
      break;
    case BRUSH_TEX_BLEND_SCREEN:
      GPU_blend(GPU_BLEND_ADDITIVE);
      break;
    case BRUSH_TEX_BLEND_ADD:
      GPU_blend(GPU_BLEND_ADDITIVE);
      break;
    case BRUSH_TEX_BLEND_SUBTRACT:
      GPU_blend(GPU_BLEND_SUBTRACT);
      break;
    default:
      GPU_blend(GPU_BLEND_ALPHA);
      break;
  }
  
  /* Render quad */
  GPU_batch_draw(gpu_ctx->quad_batch);
  
  printf("[DEBUG] render_texture_element: Element drawn at (%.2f, %.2f) size (%.2f, %.2f)\n",
         element.position.x, element.position.y, element.size.x, element.size.y);
  
  /* Keep textures bound.
   * Vulkan descriptor tracking asserts when a descriptor slot is marked unused after a draw.
   * Leaving the bindings intact avoids transient Unused states while the shader stays bound. */
  
  return true;
}

bool BKE_brush_texture_preview_render_to_image(BrushTexturePreview *preview, ImBuf **r_ibuf)
{
  if (!preview || !r_ibuf) {
    return false;
  }
  
  printf("[DEBUG] BKE_brush_texture_preview_render_to_image: Starting render to image\n");
  
  /* Render to framebuffer */
  if (!BKE_brush_texture_preview_render(preview)) {
    printf("[DEBUG] BKE_brush_texture_preview_render_to_image: Render failed\n");
    return false;
  }
  
  /* Read pixels from GPU */
  BrushTextureGPUContext *gpu_ctx = &preview->gpu_context;
  int width = gpu_ctx->viewport_size.x;
  int height = gpu_ctx->viewport_size.y;
  
  printf("[DEBUG] BKE_brush_texture_preview_render_to_image: Creating ImBuf %dx%d\n", width, height);
  
  /* Create image buffer */
  *r_ibuf = IMB_allocImBuf(width, height, ImBufFlags::FloatData);
  if (!*r_ibuf) {
    printf("[DEBUG] BKE_brush_texture_preview_render_to_image: ImBuf allocation failed\n");
    return false;
  }

  /* Read pixels */
  GPU_framebuffer_bind(gpu_ctx->framebuffer);
  GPU_framebuffer_read_color(gpu_ctx->framebuffer,
                             0,
                             0,
                             width,
                             height,
                             4,
                             0,
                             GPU_DATA_FLOAT,
                             (*r_ibuf)->float_data_for_write());
  GPU_framebuffer_restore();
  
  printf("[DEBUG] BKE_brush_texture_preview_render_to_image: Image readback complete\n");
  
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node System Integration
 * \{ */

bool BKE_brush_texture_node_tree_evaluate(const bNodeTree *ntree, 
                                         const BrushTextureNodeContext *context,
                                         BrushTextureNodeResult *result)
{
  if (!ntree || !context || !result) {
    return false;
  }
  
  /* Initialize result */
  result->output_texture = nullptr;
  result->output_mask = nullptr;
  result->has_color_output = false;
  result->has_alpha_output = false;
  result->evaluation_time = 0.0;
  
  printf("[DEBUG] BKE_brush_texture_node_tree_evaluate: Evaluating node tree\n");
  
  /* Find output nodes */
  bNode *output_node = nullptr;
  for (bNode &node : ntree->nodes) {
    /* Check if this is an output node by name pattern */
    if (node.name[0] != '\0' &&
        (strstr(node.name, "Output") || strstr(node.name, "output")))
    {
      output_node = &node;
      printf("[DEBUG] Found output node: %s\n", node.name);
      break;
    }
  }

  if (!output_node) {
    /* If no output node found, just use the first node */
    output_node = static_cast<bNode *>(ntree->nodes.first);
    if (!output_node) {
      printf("[DEBUG] BKE_brush_texture_node_tree_evaluate: No nodes in tree\n");
      return false;
    }
    printf("[DEBUG] Using first node as output: %s\n",
           output_node->name[0] != '\0' ? output_node->name : "unnamed");
  }
  
  /* Evaluate node tree */
  /* This is a simplified implementation - actual node evaluation would require */
  /* integration with Blender's node evaluation system */
  
  /* For now, create a placeholder result */
  result->has_color_output = true;
  result->has_alpha_output = true;
  result->evaluation_time = context->time;
  
  return true;
}

bool BKE_brush_texture_node_context_init(BrushTextureNodeContext *context,
                                        const Brush *brush,
                                        float2 uv_coord,
                                        float pressure,
                                        float time)
{
  if (!context || !brush) {
    return false;
  }
  
  context->brush = brush;
  context->uv_coord = uv_coord;
  context->pressure = pressure;
  context->time = time;
  context->brush_size = brush->size;
  context->brush_strength = brush->alpha;
  context->brush_angle = brush->mtex.rot;
  
  /* Initialize texture coordinates */
  context->texture_coord = uv_coord;
  
  /* Apply texture transformations */
  if (brush->mtex.tex) {
    /* Apply scaling */
    context->texture_coord.x *= brush->mtex.size[0];
    context->texture_coord.y *= brush->mtex.size[1];
    
    /* Apply offset */
    context->texture_coord.x += brush->mtex.ofs[0];
    context->texture_coord.y += brush->mtex.ofs[1];
    
    /* Apply rotation */
    if (brush->mtex.rot != 0.0f) {
      float cos_rot = cos(brush->mtex.rot);
      float sin_rot = sin(brush->mtex.rot);
      float x = context->texture_coord.x;
      float y = context->texture_coord.y;
      
      context->texture_coord.x = x * cos_rot - y * sin_rot;
      context->texture_coord.y = x * sin_rot + y * cos_rot;
    }
  }
  
  return true;
}

void BKE_brush_texture_node_context_free(BrushTextureNodeContext *context)
{
  if (!context) {
    return;
  }
  
  /* Currently no dynamic allocation in context, so nothing to free */
  context->brush = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Display and UI Integration
 * \{ */

bool BKE_brush_texture_preview_draw_ui(const BrushTexturePreview *preview, 
                                      const rcti *rect,
                                      [[maybe_unused]] float zoom_factor)
{
  if (!preview || !rect) {
    return false;
  }
  
  const int display_width = BLI_rcti_size_x(rect);
  const int display_height = BLI_rcti_size_y(rect);
  
  if (!preview->gpu_context.render_target) {
    return false;
  }

  int old_scissor[4] = {0};
  GPU_scissor_get(old_scissor);
  
  GPU_scissor_test(true);
  GPU_scissor(rect->xmin, rect->ymin, display_width, display_height);

  /* Checkerboard background for transparency. */
  {
    const float checker_colors[2][4] = {
        {0.8f, 0.8f, 0.8f, 1.0f},
        {0.6f, 0.6f, 0.6f, 1.0f},
    };
    const int checker_size = 8;

    GPU_blend(GPU_BLEND_NONE);
    GPUVertFormat *bg_format = immVertexFormat();
    const uint bg_pos = GPU_vertformat_attr_add(
        bg_format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    const uint bg_col = GPU_vertformat_attr_add(
        bg_format, "color", blender::gpu::VertAttrType::SFLOAT_32_32_32_32);

    immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);
    const int cells_x = (display_width + checker_size - 1) / checker_size;
    const int cells_y = (display_height + checker_size - 1) / checker_size;
    immBegin(GPU_PRIM_TRIS, cells_x * cells_y * 6);

    for (int y = 0; y < display_height; y += checker_size) {
      for (int x = 0; x < display_width; x += checker_size) {
        const int checker_idx = ((x / checker_size) + (y / checker_size)) % 2;
        const float *color = checker_colors[checker_idx];
        const float x1 = float(rect->xmin + x);
        const float y1 = float(rect->ymin + y);
        const float x2 = float(min_ii(rect->xmin + x + checker_size, rect->xmax));
        const float y2 = float(min_ii(rect->ymin + y + checker_size, rect->ymax));

        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x1, y1);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x2, y1);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x2, y2);

        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x1, y1);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x2, y2);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x1, y2);
      }
    }
    immEnd();
    immUnbindProgram();
  }
  
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);
  GPU_face_culling(GPU_CULL_NONE);
  GPU_color_mask(true, true, true, true);

  /* Draw the rendered texture into the widget rect.
   * Use the existing UI matrix (which correctly accounts for scroll offset and
   * maps region pixel coords to NDC). A custom ortho is NOT used here because
   * it would fill the entire viewport rather than the widget area. */
  GPUVertFormat *format = immVertexFormat();
  const uint attr_pos = GPU_vertformat_attr_add(
      format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  const uint attr_tex = GPU_vertformat_attr_add(
      format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  const GPUSamplerState sampler_state = {GPU_SAMPLER_FILTERING_LINEAR,
                                          GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                                          GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                                          GPU_SAMPLER_CUSTOM_ICON,
                                          GPU_SAMPLER_STATE_TYPE_PARAMETERS};
  immBindTextureSampler("image", preview->gpu_context.render_target, sampler_state);

  GPU_blend(GPU_BLEND_ALPHA_PREMULT);

  const float x1 = float(rect->xmin);
  const float y1 = float(rect->ymin);
  const float x2 = float(rect->xmax);
  const float y2 = float(rect->ymax);

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(attr_tex, 0.0f, 0.0f);
  immVertex2f(attr_pos, x1, y1);
  immAttr2f(attr_tex, 1.0f, 0.0f);
  immVertex2f(attr_pos, x2, y1);
  immAttr2f(attr_tex, 1.0f, 1.0f);
  immVertex2f(attr_pos, x2, y2);
  immAttr2f(attr_tex, 0.0f, 1.0f);
  immVertex2f(attr_pos, x1, y2);
  immEnd();

  GPU_texture_unbind(preview->gpu_context.render_target);
  GPU_blend(GPU_BLEND_NONE);
  GPU_depth_mask(true);
  immUnbindProgram();
  
  GPU_scissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
  
  return true;
}

bool BKE_brush_texture_preview_get_pixel_info(const BrushTexturePreview *preview,
                                             float2 uv_coord,
                                             BrushTexturePixelInfo *pixel_info)
{
  if (!preview || !pixel_info) {
    return false;
  }
  
  /* Initialize pixel info */
  pixel_info->color = float4(0.0f, 0.0f, 0.0f, 0.0f);
  pixel_info->alpha = 0.0f;
  pixel_info->uv_coord = uv_coord;
  pixel_info->is_valid = false;
  
  /* Sample texture at UV coordinate */
  if (preview->main_texture_cache.gpu_texture) {
    /* This would require GPU texture sampling - simplified for now */
    pixel_info->color = float4(1.0f, 1.0f, 1.0f, 1.0f);
  }
  
  /* Count overlapping elements */
  for (const TextureElement &element : preview->pattern_elements) {
    float2 element_uv = (uv_coord - element.position) / element.size;
    if (element_uv.x >= -0.5f && element_uv.x <= 0.5f && 
        element_uv.y >= -0.5f && element_uv.y <= 0.5f) {
      pixel_info->is_valid = true;
    }
  }
  
  return true;
}

/** \} */

} // namespace blender::ed::interface
