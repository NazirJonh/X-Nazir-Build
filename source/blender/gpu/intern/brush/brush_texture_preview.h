/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief Brush texture preview system structures and definitions.
 */

#pragma once

#include "DNA_brush_types.h"
#include "DNA_texture_types.h"
#include "DNA_image_types.h"

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "GPU_texture.hh"
#include "GPU_shader.hh"
#include "GPU_batch.hh"
#include "GPU_framebuffer.hh"
#include "GPU_uniform_buffer.hh"

struct ImBuf;
struct Material;

namespace blender::ed::interface {

/* Forward declarations */
struct BrushTexturePreview;
struct TextureElement;
struct TextureCache;

/**
 * Texture element representing a single texture instance in the preview.
 * Replaces the simple rectangle pattern with actual texture rendering.
 */
struct TextureElement {
  /** Position in preview space (0.0 to 1.0) */
  float2 position;
  
  /** Individual rotation angle in radians */
  float rotation;
  
  /** Scale factor for this element */
  float2 scale;
  
  /** Texture coordinates offset */
  float2 tex_offset;
  
  /** Alpha/opacity for blending */
  float alpha;
  
  /** Texture ID reference (for future multi-texture support) */
  int texture_id;
  
  /** Size of the texture element */
  float2 size;
  
  /** Color tint for the element */
  float3 color;
  
  /** Opacity/alpha value */
  float opacity;
  
  /** UV coordinates offset for texture sampling */
  float2 uv_offset;
  
  /** UV coordinates scale for texture sampling */
  float2 uv_scale;
  
  /** Blend mode for this element */
  int blend_mode;
  
  /** GPU-specific data pointer */
  void *gpu_data;
};

/**
 * Texture cache for efficient GPU texture management.
 * Handles texture loading, caching, and GPU resource management.
 */
struct TextureCache {
  /** Cached GPU texture from MTex */
  blender::gpu::Texture *gpu_texture;
  
  /** Source texture pointer for cache validation */
  const Tex *source_texture;
  
  /** Source image pointer for image textures */
  const Image *source_image;
  
  /** Last modification time for cache invalidation */
  uint64_t last_update;
  
  /** Texture resolution */
  int2 resolution;
  
  /** Texture format info */
  int format;
  
  /** Cache validity flag */
  bool is_valid;
  
  /** GPU resource initialization flag */
  bool gpu_initialized;
};

/**
 * GPU rendering context for brush texture preview.
 * Manages GPU resources, shaders, and rendering pipeline.
 */
struct BrushTextureGPUContext {
  /** Shader for texture rendering */
  blender::gpu::Shader *texture_shader;
  
  /** Shader for texture blending */
  blender::gpu::Shader *blend_shader;
  
  /** GPU batch for quad rendering */
  blender::gpu::Batch *quad_batch;
  
  /** Framebuffer for offscreen rendering */
  blender::gpu::FrameBuffer *framebuffer;
  
  /** Render target texture */
  blender::gpu::Texture *render_target;
  
  /** Depth buffer */
  blender::gpu::Texture *depth_buffer;

  /** 1x1 white texture bound to the mask sampler when the brush has no mask texture.
   * Owned per-context so its lifetime matches the framebuffer it is used with. */
  blender::gpu::Texture *dummy_mask_texture;

  /** Viewport dimensions */
  int2 viewport_size;
  
  /** GPU context initialization flag */
  bool initialized;
  
  /** Render dirty flag */
  bool needs_update;
};

/**
 * Main brush texture preview structure.
 * Manages the complete texture preview system including pattern generation,
 * texture sampling, GPU rendering, and caching.
 */
struct BrushTexturePreview {
  /** Source brush reference */
  const Brush *brush;
  /** Optional material reference for future material-bound brushes. */
  const Material *material;
  
  /** Texture elements array */
  blender::Vector<TextureElement> elements;
  
  /** Pattern elements for texture distribution */
  blender::Vector<TextureElement> pattern_elements;
  
  /** Texture cache for main texture */
  TextureCache main_texture_cache;
  
  /** Texture cache for mask texture */
  TextureCache mask_texture_cache;
  
  /** GPU rendering context */
  BrushTextureGPUContext gpu_context;
  
  /** Preview dimensions */
  int2 preview_size;
  
  /** Pattern generation parameters */
  struct {
    /** Number of texture elements */
    int element_count;
    
    /** Pattern distribution type */
    int distribution_type;
    
    /** Random seed for pattern generation */
    uint32_t random_seed;
    
    /** Pattern density */
    float density;
    
    /** Size variation range */
    float2 size_variation;
    
    /** Rotation variation range */
    float rotation_variation;
  } pattern_params;
  
  /** Transform parameters from brush settings */
  struct {
    /** Global scale */
    float2 scale;
    
    /** Global rotation */
    float rotation;
    
    /** Global offset */
    float2 offset;
    
    /** Texture mapping mode */
    int mapping_mode;
  } transform;

  /** Stroke preview parameters (angle and spacing from the UI template). */
  struct {
    float angle;
    float spacing;
  } stroke_params;
  
  /** Rendering parameters */
  struct {
    /** Blend mode for texture mixing */
    int blend_mode;
    
    /** Overall opacity */
    float opacity;
    
    /** Color tint */
    float3 color_tint;
    
    /** Contrast adjustment */
    float contrast;
    
    /** Brightness adjustment */
    float brightness;

    /** Reserved for material channel routing (future). */
    int material_channel_mask;
  } render_params;
  
  /** Cache and state management */
  struct {
    /** Last brush modification time */
    uint64_t last_brush_update;
    
    /** Preview validity flag */
    bool is_valid;
    
    /** GPU resources initialized */
    bool gpu_initialized;
    
    /** Pattern needs regeneration */
    bool pattern_dirty;
    
    /** Texture cache needs update */
    bool texture_dirty;

    /** Material needs re-fetch (future). */
    bool material_dirty;
  } state;
};

/**
 * Shader uniform data structure for GPU rendering.
 * Packed data sent to GPU shaders for texture rendering.
 */
struct BrushTextureShaderUniforms {
  /** Main transformation matrix for the brush texture */
  float transform_matrix[16];
  
  /** Texture coordinate transformation matrix */
  float tex_transform[16];
  
  /** Element transformation matrix */
  float element_transform[16];
  
  /** Texture transformation matrix */
  float texture_transform[16];
  
  /** Color tinting parameters */
  float color_tint[3];
  float opacity;
  float contrast;
  float brightness;
  int blend_mode;
  
  /** Texture sampling parameters */
  float tex_params[4]; // [scale_x, scale_y, rotation, offset]
  
  /** Viewport and rendering data */
  float viewport_size[2];
  float viewport_data[4]; // [width, height, aspect_ratio, pixel_scale]
};

/**
 * Node system context for brush texture evaluation.
 * Contains all necessary data for node-based texture processing.
 */
struct BrushTextureNodeContext {
  /** Source brush reference */
  const Brush *brush;
  
  /** UV coordinates for sampling */
  float2 uv_coord;
  
  /** Brush pressure value */
  float pressure;
  
  /** Animation/evaluation time */
  float time;
  
  /** Brush size at evaluation point */
  float brush_size;
  
  /** Brush strength/alpha */
  float brush_strength;
  
  /** Brush rotation angle */
  float brush_angle;
  
  /** Transformed texture coordinates */
  float2 texture_coord;
};

/**
 * Result structure for node-based texture evaluation.
 * Contains the output data from node tree evaluation.
 */
struct BrushTextureNodeResult {
  /** Output texture (if any) */
  blender::gpu::Texture *output_texture;
  
  /** Output mask texture (if any) */
  blender::gpu::Texture *output_mask;
  
  /** Whether the result has color output */
  bool has_color_output;
  
  /** Whether the result has alpha output */
  bool has_alpha_output;
  
  /** Time when evaluation was performed */
  float evaluation_time;
};

/**
 * Pixel information structure for texture preview queries.
 * Contains detailed information about a specific pixel in the preview.
 */
struct BrushTexturePixelInfo {
  /** Color value at the pixel */
  float4 color;
  
  /** Alpha/mask value */
  float alpha;
  
  /** Texture coordinates */
  float2 uv_coord;
  
  /** Whether the pixel is valid */
  bool is_valid;
};

/* Pattern distribution types */
enum eBrushTextureDistribution {
  BRUSH_TEX_DIST_REGULAR = 0,
  BRUSH_TEX_DIST_RANDOM = 1,
  BRUSH_TEX_DIST_POISSON = 2,
  BRUSH_TEX_DIST_HEXAGONAL = 3,
  BRUSH_TEX_DIST_GRID = 4,
  BRUSH_TEX_DIST_RADIAL = 5,
  BRUSH_TEX_DIST_SPIRAL = 6,
  BRUSH_TEX_DIST_STROKE = 7,
};

/* Texture mapping modes */
enum eBrushTextureMappingMode {
  BRUSH_TEX_MAP_TILED = 0,
  BRUSH_TEX_MAP_STENCIL = 1,
  BRUSH_TEX_MAP_3D = 2,
  BRUSH_TEX_MAP_RANDOM = 3,
};

/* Blend modes for texture mixing */
enum eBrushTextureBlendMode {
  BRUSH_TEX_BLEND_MIX = 0,
  BRUSH_TEX_BLEND_MULTIPLY = 1,
  BRUSH_TEX_BLEND_SCREEN = 2,
  BRUSH_TEX_BLEND_OVERLAY = 3,
  BRUSH_TEX_BLEND_ADD = 4,
  BRUSH_TEX_BLEND_SUBTRACT = 5,
};

/* -------------------------------------------------------------------- */
/** \name Cache Management Functions
 * \{ */

/**
 * Statistics for brush texture cache performance monitoring.
 */
struct BrushTextureCacheStats {
  size_t total_entries;
  size_t memory_usage;
  size_t hit_count;
  size_t miss_count;
  size_t eviction_count;
  float hit_ratio;
};

/**
 * Get the global brush texture cache manager instance.
 */
struct BrushTextureCacheManager *BKE_brush_texture_cache_manager_get();

/**
 * Free the global brush texture cache manager.
 */
void BKE_brush_texture_cache_manager_free();

/**
 * Get a cached GPU texture for the given MTex configuration.
 */
blender::gpu::Texture *BKE_brush_texture_cache_get(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format);

/**
 * Release a reference to a cached texture.
 */
void BKE_brush_texture_cache_release(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format);

/**
 * Invalidate cache entries for a specific texture.
 */
void BKE_brush_texture_cache_invalidate(const MTex *mtex);

/**
 * Perform cache cleanup to free unused entries.
 */
void BKE_brush_texture_cache_cleanup();

/**
 * Clear all cache entries.
 */
void BKE_brush_texture_cache_clear();

/**
 * Get cache statistics.
 */
void BKE_brush_texture_cache_get_stats(BrushTextureCacheStats *stats);

/**
 * Set the maximum memory usage for the cache.
 */
void BKE_brush_texture_cache_set_memory_limit(size_t max_memory_bytes);

/**
 * Set the maximum number of entries in the cache.
 */
void BKE_brush_texture_cache_set_entry_limit(size_t max_entries);

/** \} */

} // namespace blender::ed::interface
