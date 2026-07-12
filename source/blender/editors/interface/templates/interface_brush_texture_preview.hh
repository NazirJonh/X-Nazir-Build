/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief Off-screen GPU rendering of the brush-texture stroke preview widget.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {
struct Brush;
struct Image;
struct MTex;
struct Tex;
struct rcti;
}  // namespace blender

namespace blender::gpu {
class Batch;
class FrameBuffer;
class Texture;
}  // namespace blender::gpu

namespace blender::ed::interface {

/**
 * Off-screen renderer for the brush-texture stroke preview. One instance is cached per UI region
 * (see #ensure). It evaluates the brush texture into a GPU texture, renders a row of textured
 * stamps into an off-screen target, then blits that target into the widget rectangle.
 */
class BrushStrokePreview {
  /* One brush stamp placed along the previewed stroke. */
  struct Element {
    float2 center;
    float2 scale;
    float rotation;
    float2 uv_scale;
    float2 uv_offset;
    float3 color;
    float opacity;
    int blend_mode;
  };

  /* GPU texture cached from an #MTex (main or mask), with invalidation bookkeeping. */
  struct TextureCache {
    blender::gpu::Texture *gpu_texture = nullptr;
    const Tex *source_texture = nullptr;
    const Image *source_image = nullptr;
    uint64_t last_update = 0;
    int2 resolution = {0, 0};
    bool is_valid = false;
  };

  const Brush *brush_ = nullptr;
  int2 preview_size_ = {0, 0};
  float stroke_angle_ = 0.0f;
  float stroke_spacing_ = 0.0f;

  TextureCache main_cache_;
  TextureCache mask_cache_;

  blender::gpu::Batch *quad_batch_ = nullptr;
  blender::gpu::FrameBuffer *framebuffer_ = nullptr;
  blender::gpu::Texture *render_target_ = nullptr;
  blender::gpu::Texture *depth_buffer_ = nullptr;
  blender::gpu::Texture *dummy_mask_texture_ = nullptr;

  blender::Vector<Element> elements_;
  bool initialized_ = false;

 public:
  BrushStrokePreview() = default;
  ~BrushStrokePreview();
  BrushStrokePreview(const BrushStrokePreview &) = delete;
  BrushStrokePreview &operator=(const BrushStrokePreview &) = delete;

  /** Return the preview owned by \a owner, (re)creating it when brush or size changes. */
  static BrushStrokePreview *ensure(const void *owner,
                                    const Brush *brush,
                                    int2 size,
                                    float stroke_angle,
                                    float stroke_spacing);

  /** Free every cached preview and the shared shader. Must run while the GPU context is valid. */
  static void free_all();

  /** Render the textured stamps into the off-screen target. */
  bool render();

  /** Blit the rendered target into \a rect. */
  bool draw(const rcti *rect, float zoom_factor) const;

 private:
  bool gpu_context_init();
  void gpu_context_free();
  bool update();
  void render_element(const Element &element);
  void generate_stroke_layout();

  static bool cache_is_valid(const TextureCache &cache, const MTex *mtex);
  static bool cache_init(TextureCache &cache, const MTex *mtex);
  static bool cache_update(TextureCache &cache, const MTex *mtex);
  static void cache_free(TextureCache &cache);
};

}  // namespace blender::ed::interface
