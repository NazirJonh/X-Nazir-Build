/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief API for brush texture preview system integration.
 */

#pragma once

#include "brush_texture_preview.h"

struct bContext;
struct ImBuf;
struct bNodeTree;

namespace blender::ed::interface {

/* -------------------------------------------------------------------- */
/** \name Brush Texture Preview Management
 * \{ */

/**
 * Create a new brush texture preview context.
 * Initializes all necessary data structures and GPU resources.
 *
 * \param brush: Source brush for texture data
 * \param preview_size: Dimensions of the preview area
 * \return: Newly allocated BrushTexturePreview or nullptr on failure
 */
BrushTexturePreview *BKE_brush_texture_preview_create(const Brush *brush, int2 preview_size);

/**
 * Free brush texture preview and all associated resources.
 * Cleans up GPU resources, caches, and memory allocations.
 *
 * \param preview: Preview context to free
 */
void BKE_brush_texture_preview_free(BrushTexturePreview *preview);

/**
 * Update brush texture preview with new brush data.
 * Handles cache invalidation and resource updates.
 *
 * \param preview: Preview context to update
 * \param brush: Updated brush data
 * \return: True if update was successful
 */
bool BKE_brush_texture_preview_update(BrushTexturePreview *preview, const Brush *brush);

/**
 * Check if preview needs regeneration due to brush changes.
 *
 * \param preview: Preview context to check
 * \param brush: Current brush state
 * \return: True if preview is outdated
 */
bool BKE_brush_texture_preview_is_dirty(const BrushTexturePreview *preview, const Brush *brush);

/**
 * Return a preview owned by \a owner (an opaque key, typically the UI region), creating it on
 * first use and recreating it when the brush or preview size changes. The returned preview is
 * already updated for the current brush state. The registry retains ownership; callers must not
 * free the result. All cached previews are released by #BKE_brush_texture_preview_free_all().
 *
 * \param owner: Opaque key that scopes the preview lifetime (e.g. the #ARegion pointer).
 * \param brush: Current brush.
 * \param preview_size: Pixel dimensions of the preview area.
 * \param stroke_angle: Brush texture angle for the stroke preview.
 * \param stroke_spacing: Brush spacing for the stroke preview.
 */
BrushTexturePreview *BKE_brush_texture_preview_ensure(const void *owner,
                                                      const Brush *brush,
                                                      int2 preview_size,
                                                      float stroke_angle,
                                                      float stroke_spacing);

/**
 * Free every preview created through #BKE_brush_texture_preview_ensure(). Must be called while the
 * GPU context is still valid (i.e. from #GPU_exit()).
 */
void BKE_brush_texture_preview_free_all();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Cache Management
 * \{ */

/**
 * Initialize texture cache from MTex data.
 * Loads texture data and creates GPU resources.
 *
 * \param cache: Cache structure to initialize
 * \param mtex: Source texture data
 * \return: True if initialization was successful
 */
bool BKE_brush_texture_cache_init(TextureCache *cache, const MTex *mtex);

/**
 * Update texture cache if source data has changed.
 * Checks modification times and reloads if necessary.
 *
 * \param cache: Cache to update
 * \param mtex: Current texture data
 * \return: True if cache was updated
 */
bool BKE_brush_texture_cache_update(TextureCache *cache, const MTex *mtex);

/**
 * Free texture cache resources.
 * Cleans up GPU textures and resets cache state.
 *
 * \param cache: Cache to free
 */
void BKE_brush_texture_cache_free(TextureCache *cache);

/**
 * Validate texture cache against source data.
 *
 * \param cache: Cache to validate
 * \param mtex: Source texture data
 * \return: True if cache is valid
 */
bool BKE_brush_texture_cache_is_valid(const TextureCache *cache, const MTex *mtex);

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Context Management
 * \{ */

/**
 * Initialize GPU rendering context.
 * Creates shaders, framebuffers, and GPU resources.
 *
 * \param gpu_context: GPU context to initialize
 * \param viewport_size: Rendering viewport dimensions
 * \return: True if initialization was successful
 */
bool BKE_brush_texture_gpu_context_init(BrushTextureGPUContext *gpu_context, int2 viewport_size);

/**
 * Free GPU rendering context.
 * Cleans up all GPU resources and shaders.
 *
 * \param gpu_context: GPU context to free
 */
void BKE_brush_texture_gpu_context_free(BrushTextureGPUContext *gpu_context);

/**
 * Resize GPU context for new viewport dimensions.
 *
 * \param gpu_context: GPU context to resize
 * \param new_size: New viewport dimensions
 * \return: True if resize was successful
 */
bool BKE_brush_texture_gpu_context_resize(BrushTextureGPUContext *gpu_context, int2 new_size);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pattern Generation
 * \{ */

/**
 * Generate texture element pattern based on brush settings.
 * Creates positioned texture elements for rendering.
 *
 * \param preview: Preview context
 * \param brush: Source brush for pattern parameters
 * \return: True if pattern generation was successful
 */
bool BKE_brush_texture_pattern_generate(BrushTexturePreview *preview, const Brush *brush);

/**
 * Clear existing pattern and reset element array.
 *
 * \param preview: Preview context to clear
 */
void BKE_brush_texture_pattern_clear(BrushTexturePreview *preview);

/**
 * Update pattern parameters without full regeneration.
 *
 * \param preview: Preview context
 * \param density: Pattern density factor
 * \param distribution: Distribution type
 * \return: True if update was successful
 */
bool BKE_brush_texture_pattern_update_params(BrushTexturePreview *preview, 
                                           float density, 
                                           eBrushTextureDistribution distribution);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node System Integration
 * \{ */

/**
 * Sample texture using node system for advanced texture processing.
 * Integrates with Blender's node-based texture system.
 *
 * \param mtex: Texture data with potential node tree
 * \param co: Texture coordinates (0.0 to 1.0)
 * \param result: Output color result
 * \return: True if sampling was successful
 */
bool BKE_brush_texture_sample_nodes(const MTex *mtex, const float2 &co, float result[4]);

/**
 * Check if texture uses node-based processing.
 *
 * \param mtex: Texture data to check
 * \return: True if texture uses nodes
 */
bool BKE_brush_texture_uses_nodes(const MTex *mtex);

/**
 * Initialize node-based texture sampling context.
 *
 * \param mtex: Texture data
 * \return: Opaque context pointer or nullptr on failure
 */
void *BKE_brush_texture_nodes_context_create(const MTex *mtex);

/**
 * Free node-based texture sampling context.
 *
 * \param context: Context to free
 */
void BKE_brush_texture_nodes_context_free(void *context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rendering and Display
 * \{ */

/**
 * Render brush texture preview to GPU framebuffer.
 * Main rendering function that combines all texture elements.
 *
 * \param preview: Preview context
 * \return: True if rendering was successful
 */
bool BKE_brush_texture_preview_render(BrushTexturePreview *preview);

/**
 * Draw rendered preview texture into a UI rectangle.
 */
bool BKE_brush_texture_preview_draw_ui(const BrushTexturePreview *preview,
                                       const struct rcti *rect,
                                       float zoom_factor);

/**
 * Get rendered preview as ImBuf for CPU-side processing.
 *
 * \param preview: Preview context
 * \return: ImBuf with rendered preview or nullptr on failure
 */
struct ImBuf *BKE_brush_texture_preview_get_ibuf(BrushTexturePreview *preview);

/**
 * Attach material for material-bound brush previews (future stub).
 */
void BKE_brush_texture_preview_set_material(BrushTexturePreview *preview, const Material *material);

/**
 * Clear material reference.
 */
void BKE_brush_texture_preview_clear_material(BrushTexturePreview *preview);

/**
 * Update shader uniforms with current preview parameters.
 *
 * \param preview: Preview context
 * \param uniforms: Uniform data structure to fill
 */
void BKE_brush_texture_preview_update_uniforms(const BrushTexturePreview *preview, 
                                             BrushTextureShaderUniforms *uniforms);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

/**
 * Convert MTex blend mode to preview blend mode.
 *
 * \param mtex_blend: MTex blend type
 * \return: Corresponding preview blend mode
 */
eBrushTextureBlendMode BKE_brush_texture_convert_blend_mode(int mtex_blend);

/**
 * Get texture resolution from MTex data.
 *
 * \param mtex: Texture data
 * \return: Texture resolution or default if unavailable
 */
int2 BKE_brush_texture_get_resolution(const MTex *mtex);

/**
 * Check if texture data is valid for preview generation.
 *
 * \param mtex: Texture data to validate
 * \return: True if texture is valid
 */
bool BKE_brush_texture_is_valid(const MTex *mtex);

/**
 * Get texture modification time for cache validation.
 *
 * \param mtex: Texture data
 * \return: Modification timestamp
 */
uint64_t BKE_brush_texture_get_update_time(const MTex *mtex);

/** \} */

} // namespace blender::ed::interface