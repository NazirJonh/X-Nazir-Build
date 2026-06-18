/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief Implementation of brush texture preview API.
 */

#include "brush_texture_preview_api.h"
#include "brush_texture_shaders.h"

#include "DNA_brush_types.h"
#include "DNA_brush_enums.h"
#include "DNA_texture_types.h"
#include "DNA_image_types.h"
#include "DNA_node_types.h"

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_texture.h"

#include "RE_texture.h"

#include "NOD_texture.h"

#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "GPU_batch.hh"
#include "GPU_framebuffer.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include <algorithm>
#include <cmath>

namespace blender::ed::interface {

/* Registry of live previews keyed by an opaque owner (the UI region). Keeping one preview per
 * owner avoids recreating GPU resources when several regions show different brushes, and lets all
 * resources be released together from #BKE_brush_texture_preview_free_all() at GPU shutdown. */
static Map<const void *, BrushTexturePreview *> g_preview_registry;

/* -------------------------------------------------------------------- */
/** \name Brush Texture Preview Management
 * \{ */

BrushTexturePreview *BKE_brush_texture_preview_create(const Brush *brush, int2 preview_size)
{
  printf("[DEBUG] BKE_brush_texture_preview_create: Creating preview for brush %p, size %dx%d\n",
         (const void*)brush, preview_size.x, preview_size.y);
  
  if (!brush) {
    printf("[DEBUG] BKE_brush_texture_preview_create: ERROR - Null brush\n");
    return nullptr;
  }
  
  printf("[DEBUG] BKE_brush_texture_preview_create: Allocating BrushTexturePreview\n");

  /* Allocate and ensure critical aggregates are initialized. */
  BrushTexturePreview *preview = MEM_new<BrushTexturePreview>("BrushTexturePreview");
  preview->main_texture_cache = {};
  preview->mask_texture_cache = {};
  preview->gpu_context = {};
  preview->preview_size = int2(0, 0);
  preview->pattern_elements.clear();
  preview->elements.clear();
  preview->state = {};
  
  preview->brush = brush;
  preview->preview_size = preview_size;
  
  /* Initialize pattern parameters with defaults */
  preview->pattern_params.element_count = 64;
  preview->pattern_params.distribution_type = BRUSH_TEX_DIST_STROKE;
  /* Fixed seed so the preview pattern is stable across redraws (no flicker). */
  preview->pattern_params.random_seed = 0u;
  preview->pattern_params.density = 1.0f;
  preview->pattern_params.size_variation = float2(0.8f, 1.2f);
  preview->pattern_params.rotation_variation = M_PI;

  preview->stroke_params.angle = 0.0f;
  preview->stroke_params.spacing = 25.0f;
  
  /* Initialize transform parameters from brush */
  preview->transform.scale = float2(1.0f, 1.0f);
  preview->transform.rotation = 0.0f;
  preview->transform.offset = float2(0.0f, 0.0f);
  preview->transform.mapping_mode = BRUSH_TEX_MAP_TILED;
  
  /* Initialize render parameters */
  preview->render_params.blend_mode = BRUSH_TEX_BLEND_MIX;
  preview->render_params.opacity = 1.0f;
  preview->render_params.color_tint = float3(1.0f, 1.0f, 1.0f);
  preview->render_params.contrast = 1.0f;
  preview->render_params.brightness = 0.0f;
  
  /* Initialize state */
  preview->state.last_brush_update = 0;
  preview->state.is_valid = false;
  preview->state.gpu_initialized = false;
  preview->state.pattern_dirty = true;
  preview->state.texture_dirty = true;
  
  /* Initialize GPU context */
  if (!BKE_brush_texture_gpu_context_init(&preview->gpu_context, preview_size)) {
    MEM_delete(preview);
    return nullptr;
  }
  
  /* Initialize texture caches */
  BKE_brush_texture_cache_init(&preview->main_texture_cache, &brush->mtex);
  BKE_brush_texture_cache_init(&preview->mask_texture_cache, &brush->mask_mtex);
  
  return preview;
}

void BKE_brush_texture_preview_free(BrushTexturePreview *preview)
{
  if (!preview) {
    return;
  }
  
  /* Free GPU context */
  BKE_brush_texture_gpu_context_free(&preview->gpu_context);
  
  /* Free texture caches */
  BKE_brush_texture_cache_free(&preview->main_texture_cache);
  BKE_brush_texture_cache_free(&preview->mask_texture_cache);
  
  /* Free preview structure */
  MEM_delete(preview);
}

bool BKE_brush_texture_preview_update(BrushTexturePreview *preview, const Brush *brush)
{
  printf("[DEBUG] BKE_brush_texture_preview_update: Starting update (preview=%p, brush=%p)\n",
         (void*)preview, (const void*)brush);
  
  if (!preview || !brush) {
    printf("[DEBUG] BKE_brush_texture_preview_update: ERROR - Invalid parameters\n");
    return false;
  }

  /* Check if brush has changed */
  uint64_t current_time = BKE_brush_texture_get_update_time(&brush->mtex);
  printf("[DEBUG] BKE_brush_texture_preview_update: current_time=%llu, last_update=%llu\n",
         (unsigned long long)current_time, (unsigned long long)preview->state.last_brush_update);
  
  if (current_time != preview->state.last_brush_update) {
    printf("[DEBUG] BKE_brush_texture_preview_update: Brush changed, marking dirty\n");
    preview->brush = brush;
    preview->state.last_brush_update = current_time;
    preview->state.texture_dirty = true;
    preview->state.pattern_dirty = true;
    preview->state.is_valid = false;
  }

  /* If material reference changed or marked dirty, force refresh. */
  if (preview->state.material_dirty) {
    printf("[DEBUG] BKE_brush_texture_preview_update: Material dirty, marking for refresh\n");
    preview->state.texture_dirty = true;
    preview->state.pattern_dirty = true;
    preview->state.material_dirty = false;
  }
  
  /* Update texture caches if needed */
  if (preview->state.texture_dirty) {
    printf("[DEBUG] BKE_brush_texture_preview_update: Updating texture caches\n");
    const bool main_ok = BKE_brush_texture_cache_update(&preview->main_texture_cache, &brush->mtex);
    BKE_brush_texture_cache_update(&preview->mask_texture_cache, &brush->mask_mtex);
    printf("[DEBUG] BKE_brush_texture_preview_update: Main texture OK=%d, GPU texture=%p\n",
           main_ok, (void*)preview->main_texture_cache.gpu_texture);
    /* Keep dirty if we failed to produce a GPU texture so the next update can retry. */
    preview->state.texture_dirty = !(main_ok && preview->main_texture_cache.gpu_texture);
  }
  
  /* Regenerate pattern if needed */
  if (preview->state.pattern_dirty) {
    printf("[DEBUG] BKE_brush_texture_preview_update: Regenerating pattern\n");
    BKE_brush_texture_pattern_generate(preview, brush);
    preview->state.pattern_dirty = false;
  }
  
  preview->state.is_valid = true;
  printf("[DEBUG] BKE_brush_texture_preview_update: Update complete, valid=%d\n", preview->state.is_valid);
  return true;
}

bool BKE_brush_texture_preview_is_dirty(const BrushTexturePreview *preview, const Brush *brush)
{
  if (!preview || !brush) {
    return true;
  }
  
  uint64_t current_time = BKE_brush_texture_get_update_time(&brush->mtex);
  return current_time != preview->state.last_brush_update || !preview->state.is_valid;
}

BrushTexturePreview *BKE_brush_texture_preview_ensure(const void *owner,
                                                      const Brush *brush,
                                                      int2 preview_size,
                                                      float stroke_angle,
                                                      float stroke_spacing)
{
  if (!owner || !brush) {
    return nullptr;
  }

  preview_size = int2(max_ii(preview_size.x, 1), max_ii(preview_size.y, 1));

  BrushTexturePreview *preview = g_preview_registry.lookup_default(owner, nullptr);

  /* Recreate the GPU resources only when the brush or the size changes. Content changes (texture,
   * pattern) are handled by #BKE_brush_texture_preview_update() below. */
  if (preview && (preview->brush != brush || preview->preview_size != preview_size)) {
    BKE_brush_texture_preview_free(preview);
    g_preview_registry.remove(owner);
    preview = nullptr;
  }

  if (!preview) {
    preview = BKE_brush_texture_preview_create(brush, preview_size);
    if (!preview) {
      return nullptr;
    }
    g_preview_registry.add_overwrite(owner, preview);
  }

  if (preview->stroke_params.angle != stroke_angle ||
      preview->stroke_params.spacing != stroke_spacing)
  {
    preview->stroke_params.angle = stroke_angle;
    preview->stroke_params.spacing = stroke_spacing;
    preview->state.pattern_dirty = true;
  }

  BKE_brush_texture_preview_update(preview, brush);
  return preview;
}

void BKE_brush_texture_preview_free_all()
{
  for (BrushTexturePreview *preview : g_preview_registry.values()) {
    BKE_brush_texture_preview_free(preview);
  }
  g_preview_registry.clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Cache Management
 * \{ */

bool BKE_brush_texture_cache_init(TextureCache *cache, const MTex *mtex)
{
  if (!cache || !mtex || !mtex->tex) {
    return false;
  }
  
  /* Initialize cache structure */
  cache->gpu_texture = nullptr;
  cache->source_texture = mtex->tex;
  cache->source_image = nullptr;
  cache->last_update = 0;
  cache->resolution = int2(256, 256); /* Default resolution */
  cache->format = int(gpu::TextureFormat::UNORM_8_8_8_8); /* RGBA8 */
  cache->is_valid = false;
  cache->gpu_initialized = false;
  
  /* Get image reference for image textures */
  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    cache->source_image = mtex->tex->ima;
    cache->resolution = BKE_brush_texture_get_resolution(mtex);
  }
  
  return true;
}

bool BKE_brush_texture_cache_update(TextureCache *cache, const MTex *mtex)
{
  if (!cache || !mtex) {
    return false;
  }

  /* mtex->tex may be cleared between init and update; guard all dereferences. */
  if (!mtex->tex) {
    cache->is_valid = false;
    cache->gpu_initialized = false;
    return false;
  }
  
  /* Check if cache is still valid */
  if (BKE_brush_texture_cache_is_valid(cache, mtex)) {
    return true;
  }
  
  /* Free existing GPU texture */
  if (cache->gpu_texture) {
    GPU_texture_free(cache->gpu_texture);
    cache->gpu_texture = nullptr;
  }
  
  /* Update cache data */
  cache->source_texture = mtex->tex;
  cache->last_update = BKE_brush_texture_get_update_time(mtex);

  const int size = std::max(256, std::max(cache->resolution.x, cache->resolution.y));
  cache->resolution = int2(size, size);

  uchar *buffer = MEM_new_array_uninitialized<uchar>(size * size * 4, "brush_preview_tex");
  if (!buffer) {
    cache->is_valid = false;
    cache->gpu_initialized = false;
    return false;
  }

  ImagePool *pool = BKE_image_pool_new();
  const float rotation = -mtex->rot;

  bNodeTreeExec *tex_exec = nullptr;
  if (mtex->tex->nodetree) {
    tex_exec = ntreeTexBeginExecTree(mtex->tex->nodetree);
  }

  for (int j = 0; j < size; j++) {
    for (int i = 0; i < size; i++) {
      const int index = (j * size + i) * 4;
      float x = (float(i) / float(size) - 0.5f) * 2.0f;
      float y = (float(j) / float(size) - 0.5f) * 2.0f;
      const float len = sqrtf(x * x + y * y);

      if (len <= 1.0f) {
        if (fabsf(rotation) > 0.001f) {
          const float angle = atan2f(y, x) + rotation;
          x = len * cosf(angle);
          y = len * sinf(angle);
        }

        const float co[3] = {x, y, 0.0f};
        float intensity;
        float rgba[4];
        const bool has_rgb = RE_texture_evaluate(
            mtex, co, 0, pool, false, false, &intensity, rgba);

        if (has_rgb) {
          buffer[index] = uchar(clamp_f(rgba[0], 0.0f, 1.0f) * 255.0f);
          buffer[index + 1] = uchar(clamp_f(rgba[1], 0.0f, 1.0f) * 255.0f);
          buffer[index + 2] = uchar(clamp_f(rgba[2], 0.0f, 1.0f) * 255.0f);
          buffer[index + 3] = uchar(clamp_f(rgba[3], 0.0f, 1.0f) * 255.0f);
        }
        else {
          /* Alpha/strength textures: match paint cursor inversion. */
          CLAMP(intensity, 0.0f, 1.0f);
          const uchar val = uchar(255.0f - intensity * 255.0f);
          buffer[index] = val;
          buffer[index + 1] = val;
          buffer[index + 2] = val;
          buffer[index + 3] = 255;
        }
      }
      else {
        buffer[index] = 0;
        buffer[index + 1] = 0;
        buffer[index + 2] = 0;
        buffer[index + 3] = 0;
      }
    }
  }

  if (tex_exec) {
    ntreeTexEndExecTree(tex_exec);
  }
  if (pool) {
    BKE_image_pool_free(pool);
  }

  cache->gpu_texture = GPU_texture_create_2d("brush_texture_cache",
                                           size,
                                           size,
                                           1,
                                           gpu::TextureFormat::UNORM_8_8_8_8,
                                           GPU_TEXTURE_USAGE_SHADER_READ,
                                           nullptr);
  if (cache->gpu_texture) {
    GPU_texture_update(cache->gpu_texture, GPU_DATA_UBYTE, buffer);
  }

  MEM_delete(buffer);
  
  cache->is_valid = (cache->gpu_texture != nullptr);
  cache->gpu_initialized = cache->is_valid;
  
  return cache->is_valid;
}

void BKE_brush_texture_cache_free(TextureCache *cache)
{
  if (!cache) {
    return;
  }
  
  if (cache->gpu_texture) {
    GPU_texture_free(cache->gpu_texture);
    cache->gpu_texture = nullptr;
  }
  
  cache->is_valid = false;
  cache->gpu_initialized = false;
}

bool BKE_brush_texture_cache_is_valid(const TextureCache *cache, const MTex *mtex)
{
  if (!cache || !mtex || !cache->is_valid) {
    return false;
  }
  
  /* Check if source texture has changed */
  if (cache->source_texture != mtex->tex) {
    return false;
  }
  
  /* Check modification time */
  uint64_t current_time = BKE_brush_texture_get_update_time(mtex);
  return current_time == cache->last_update;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Context Management
 * \{ */

bool BKE_brush_texture_gpu_context_init(BrushTextureGPUContext *gpu_context, int2 viewport_size)
{
  if (!gpu_context) {
    return false;
  }
  
  /* Initialize context structure */
  gpu_context->texture_shader = nullptr;
  gpu_context->blend_shader = nullptr;
  gpu_context->quad_batch = nullptr;
  gpu_context->framebuffer = nullptr;
  gpu_context->render_target = nullptr;
  gpu_context->depth_buffer = nullptr;
  gpu_context->dummy_mask_texture = nullptr;
  gpu_context->viewport_size = viewport_size;
  gpu_context->initialized = false;
  gpu_context->needs_update = true;

  /* Create shaders using shader create info (force immediate compilation). */
  /* Use brush_texture_element info (shared GLSL, stable pipeline). */
  const GPUShaderCreateInfo *tex_info = GPU_shader_create_info_get("brush_texture_element");
  if (tex_info) {
    gpu_context->texture_shader = GPU_shader_create_from_info(tex_info);
  }
  const GPUShaderCreateInfo *blend_info = GPU_shader_create_info_get("brush_blend_advanced");
  if (blend_info) {
    gpu_context->blend_shader = GPU_shader_create_from_info(blend_info);
  }

  if (!gpu_context->texture_shader || !gpu_context->blend_shader) {
    BKE_brush_texture_gpu_context_free(gpu_context);
    return false;
  }
  
  /* Create quad batch using modern API */
  gpu::VertBuf *vbo = GPU_vertbuf_calloc();
  GPUVertFormat format;
  GPU_vertformat_clear(&format);
  /* Attribute names must match GPU_SHADER_CREATE_INFO: pos / texcoord / element_rotation. */
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  GPU_vertformat_attr_add(&format, "texcoord", gpu::VertAttrType::SFLOAT_32_32);
  GPU_vertformat_attr_add(&format, "element_rotation", gpu::VertAttrType::SFLOAT_32);
  GPU_vertbuf_init_with_format(*vbo, format);
  GPU_vertbuf_data_alloc(*vbo, 4);
  
  /* Quad vertices */
  float vertices[4][5] = {
      {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f}, /* Bottom-left */
      { 1.0f, -1.0f, 1.0f, 0.0f, 0.0f}, /* Bottom-right */
      { 1.0f,  1.0f, 1.0f, 1.0f, 0.0f}, /* Top-right */
      {-1.0f,  1.0f, 0.0f, 1.0f, 0.0f}, /* Top-left */
  };
  
  for (int i = 0; i < 4; i++) {
    GPU_vertbuf_vert_set(vbo, i, vertices[i]);
  }
  
  /* Create index buffer */
  gpu::IndexBuf *ibuf = GPU_indexbuf_calloc();
  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, 2, 4);
  GPU_indexbuf_add_tri_verts(&builder, 0, 1, 2);
  GPU_indexbuf_add_tri_verts(&builder, 0, 2, 3);
  GPU_indexbuf_build_in_place(&builder, ibuf);
  
  gpu_context->quad_batch = GPU_batch_create_ex(
      GPU_PRIM_TRIS, vbo, ibuf, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);

  /* Create render targets */
  gpu_context->render_target = GPU_texture_create_2d(
      "brush_preview_target",
      viewport_size.x,
      viewport_size.y,
      1,
      gpu::TextureFormat::UNORM_8_8_8_8,
      GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ,
      nullptr);
  
  /* Set proper sampler parameters for render_target */
  if (gpu_context->render_target) {
    GPU_texture_filter_mode(gpu_context->render_target, false); /* No filtering for pixel-perfect display */
    GPU_texture_extend_mode(gpu_context->render_target, GPU_SAMPLER_EXTEND_MODE_EXTEND); /* Clamp to edge */
  }

  gpu_context->depth_buffer = GPU_texture_create_2d(
      "brush_preview_depth",
      viewport_size.x,
      viewport_size.y,
      1,
      gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
      GPU_TEXTURE_USAGE_ATTACHMENT,
      nullptr);

  /* Create framebuffer */
  gpu_context->framebuffer = GPU_framebuffer_create("brush_preview_fb");
  GPU_framebuffer_texture_attach(gpu_context->framebuffer, gpu_context->render_target, 0, 0);
  GPU_framebuffer_texture_attach(gpu_context->framebuffer, gpu_context->depth_buffer, 0, 0);
  
  if (!GPU_framebuffer_check_valid(gpu_context->framebuffer, nullptr)) {
    BKE_brush_texture_gpu_context_free(gpu_context);
    return false;
  }
  
  /* 1x1 white texture bound to the mask sampler when the brush has no mask. Owned by this
   * context so it is freed together with the framebuffer that uses it. */
  {
    float data[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    gpu_context->dummy_mask_texture = GPU_texture_create_2d("brush_dummy_mask",
                                                            1,
                                                            1,
                                                            1,
                                                            gpu::TextureFormat::UNORM_8_8_8_8,
                                                            GPU_TEXTURE_USAGE_SHADER_READ,
                                                            data);
  }

  gpu_context->initialized = true;
  return true;
}

void BKE_brush_texture_gpu_context_free(BrushTextureGPUContext *gpu_context)
{
  if (!gpu_context) {
    return;
  }
  
  if (gpu_context->texture_shader) {
    GPU_shader_free(gpu_context->texture_shader);
  }
  if (gpu_context->blend_shader) {
    GPU_shader_free(gpu_context->blend_shader);
  }
  if (gpu_context->quad_batch) {
    GPU_batch_discard(gpu_context->quad_batch);
  }
  if (gpu_context->framebuffer) {
    GPU_framebuffer_free(gpu_context->framebuffer);
  }
  if (gpu_context->render_target) {
    GPU_texture_free(gpu_context->render_target);
  }
  if (gpu_context->depth_buffer) {
    GPU_texture_free(gpu_context->depth_buffer);
  }
  if (gpu_context->dummy_mask_texture) {
    GPU_texture_free(gpu_context->dummy_mask_texture);
    gpu_context->dummy_mask_texture = nullptr;
  }

  gpu_context->initialized = false;
}

bool BKE_brush_texture_gpu_context_resize(BrushTextureGPUContext *gpu_context, int2 new_size)
{
  if (!gpu_context || !gpu_context->initialized) {
    return false;
  }
  
  if (gpu_context->viewport_size.x == new_size.x && gpu_context->viewport_size.y == new_size.y) {
    return true;
  }
  
  /* Free old render targets */
  if (gpu_context->render_target) {
    GPU_texture_free(gpu_context->render_target);
  }
  if (gpu_context->depth_buffer) {
    GPU_texture_free(gpu_context->depth_buffer);
  }
  
  /* Create new render targets */
  gpu_context->render_target = GPU_texture_create_2d(
      "brush_preview_target",
      new_size.x,
      new_size.y,
      1,
      gpu::TextureFormat::UNORM_8_8_8_8,
      GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ,
      nullptr);
  
  /* Set proper sampler parameters for render_target */
  if (gpu_context->render_target) {
    GPU_texture_filter_mode(gpu_context->render_target, false); /* No filtering for pixel-perfect display */
    GPU_texture_extend_mode(gpu_context->render_target, GPU_SAMPLER_EXTEND_MODE_EXTEND); /* Clamp to edge */
  }
  
  gpu_context->depth_buffer = GPU_texture_create_2d(
      "brush_preview_depth", 
      new_size.x, 
      new_size.y, 
      1, 
      gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
      GPU_TEXTURE_USAGE_ATTACHMENT,
      nullptr);
  
  /* Update framebuffer */
  GPU_framebuffer_texture_attach(gpu_context->framebuffer, gpu_context->render_target, 0, 0);
  GPU_framebuffer_texture_attach(gpu_context->framebuffer, gpu_context->depth_buffer, 0, 0);
  
  gpu_context->viewport_size = new_size;
  gpu_context->needs_update = true;
  
  return GPU_framebuffer_check_valid(gpu_context->framebuffer, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material integration stubs
 * \{ */

void BKE_brush_texture_preview_set_material(BrushTexturePreview *preview, const Material *material)
{
  if (!preview) {
    return;
  }

  preview->material = material;
  preview->state.material_dirty = true;
}

void BKE_brush_texture_preview_clear_material(BrushTexturePreview *preview)
{
  if (!preview) {
    return;
  }

  preview->material = nullptr;
  preview->state.material_dirty = true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

eBrushTextureBlendMode BKE_brush_texture_convert_blend_mode(int mtex_blend)
{
  switch (mtex_blend) {
    case MTEX_BLEND: return BRUSH_TEX_BLEND_MIX;
    case MTEX_MUL: return BRUSH_TEX_BLEND_MULTIPLY;
    case MTEX_SCREEN: return BRUSH_TEX_BLEND_SCREEN;
    case MTEX_OVERLAY: return BRUSH_TEX_BLEND_OVERLAY;
    case MTEX_ADD: return BRUSH_TEX_BLEND_ADD;
    case MTEX_SUB: return BRUSH_TEX_BLEND_SUBTRACT;
    default: return BRUSH_TEX_BLEND_MIX;
  }
}

int2 BKE_brush_texture_get_resolution(const MTex *mtex)
{
  if (!mtex || !mtex->tex) {
    return int2(256, 256);
  }
  
  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    ImBuf *ibuf = BKE_image_acquire_ibuf(mtex->tex->ima, nullptr, nullptr);
    if (ibuf) {
      int2 resolution = int2(ibuf->x, ibuf->y);
      BKE_image_release_ibuf(mtex->tex->ima, ibuf, nullptr);
      return resolution;
    }
  }
  
  return int2(256, 256);
}

bool BKE_brush_texture_is_valid(const MTex *mtex)
{
  return mtex && mtex->tex && (mtex->tex->type == TEX_IMAGE ? mtex->tex->ima != nullptr : true);
}

uint64_t BKE_brush_texture_get_update_time(const MTex *mtex)
{
  if (!mtex || !mtex->tex) {
    return 0;
  }
  
  /* For image textures, use image modification time */
  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    return mtex->tex->ima->id.recalc;
  }
  
  /* For procedural textures, use texture modification time */
  return mtex->tex->id.recalc;
}

/** \} */

} // namespace blender::ed::interface
