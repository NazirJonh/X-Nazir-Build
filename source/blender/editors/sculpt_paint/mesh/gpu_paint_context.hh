/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * GPU Paint Context for sculpt painting optimization.
 * Manages GPU resources for compute shader-based painting.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"

#include "DNA_brush_types.h"

/* Forward declarations. */
struct Brush;
struct MTex;

namespace blender {
struct ImBuf;
struct SculptSession;
}

namespace blender::bke::pbvh::pixels {
struct PackedPixelRow;
struct UVPrimitivePaintInput;
}

namespace blender::ed::sculpt_paint::paint::image {

/* -------------------------------------------------------------------- */
/** \name GPU Paint Data Structures
 * \{ */

/**
 * Pixel row data for GPU shader.
 * Encodes sequential pixels for efficient processing.
 * Must match PaintPixelRow in GPU_paint_shared.hh.
 * Layout: fields ordered by alignment requirements (largest first).
 */
struct GPUPixelRow {
  float2 start_barycentric_coord;
  uint2 start_image_coord;
  uint uv_primitive_index;
  uint num_pixels;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Paint Context Class
 * \{ */

/**
 * Manages GPU resources for compute shader-based sculpt painting.
 *
 * This class handles:
 * - Shader creation and caching
 * - Storage buffer management
 * - Texture bindings
 * - Compute dispatch coordination
 *
 * Brush parameters are passed via push constants for efficiency.
 */
class GPU_PaintContext {
 private:
  /* Shader for compute painting. */
  gpu::Shader *paint_shader_ = nullptr;

  /* Storage buffers for geometry and pixel data. */
  gpu::StorageBuf *pixel_data_ssbo_ = nullptr;
  gpu::StorageBuf *vertex_positions_ssbo_ = nullptr;
  gpu::StorageBuf *triangle_indices_ssbo_ = nullptr;
  gpu::StorageBuf *automask_data_ssbo_ = nullptr;

  /* Brush texture. */
  gpu::Texture *brush_texture_ = nullptr;

  /* Cached data sizes. */
  int num_pixel_rows_ = 0;
  int num_vertices_ = 0;
  int num_triangles_ = 0;

  /* Initialization state. */
  bool is_initialized_ = false;
  bool has_brush_texture_ = false;
  bool has_automask_ = false;

  /* Image dimensions for current paint operation. */
  int2 image_size_ = int2(0);

  /* Brush parameters for push constants. */
  float4 brush_color_ = float4(1.0f);
  float4 secondary_color_ = float4(1.0f);
  float3 brush_location_ = float3(0.0f);
  float brush_radius_ = 1.0f;
  float brush_strength_ = 1.0f;
  float brush_alpha_ = 1.0f;
  float hardness_ = 0.5f;
  float brush_tex_rotation_ = 0.0f;
  float2 brush_tex_size_ = float2(1.0f);
  float2 brush_tex_offset_ = float2(0.0f);
  float texture_sample_bias_ = 0.0f;
  int blend_mode_ = 0;
  int falloff_shape_ = 0;
  int invert_ = 0;

 public:
  GPU_PaintContext() = default;
  ~GPU_PaintContext();

  /* Non-copyable. */
  GPU_PaintContext(const GPU_PaintContext &) = delete;
  GPU_PaintContext &operator=(const GPU_PaintContext &) = delete;

  /**
   * Ensure all GPU resources are initialized.
   * Safe to call multiple times.
   */
  void ensure_resources();

  /**
   * Free all GPU resources.
   */
  void free_resources();

  /**
   * Check if GPU painting is supported for the given image buffer.
   */
  static bool is_gpu_paint_supported(blender::ImBuf *image_buffer);

  /**
   * Update brush parameters from brush data.
   */
  void update_brush_params(const Brush &brush, const SculptSession &ss, bool invert);

  /**
   * Set the brush texture for sampling.
   * Pass nullptr to disable brush texture.
   */
  void set_brush_texture(gpu::Texture *texture, const struct MTex *mtex = nullptr);

  /**
   * Upload pixel row data to GPU.
   * @param pixel_rows The pixel rows to upload.
   * @param uv_primitives Array to convert uv_primitive_index to tri_index.
   * @return The number of rows uploaded.
   */
  int upload_pixel_data(
      const Span<bke::pbvh::pixels::PackedPixelRow> &pixel_rows,
      const Span<bke::pbvh::pixels::UVPrimitivePaintInput> &uv_primitives);

  /**
   * Upload vertex positions and triangle indices.
   */
  void upload_geometry_data(const Span<float3> positions, const Span<int3> triangles);

  /**
   * Upload automasking factors.
   */
  void upload_automask_data(const Span<float> factors);

  /**
   * Dispatch compute shader to paint on target texture.
   * The target texture must be bound as an image.
   */
  void dispatch_paint(gpu::Texture *target_texture);

  /**
   * Dispatch compute shader for specific number of pixel rows.
   */
  void dispatch_paint(gpu::Texture *target_texture, int num_pixel_rows);

  /**
   * Synchronize GPU operations before reading results.
   * Must be called before GPU_texture_read() on the target texture.
   */
  void synchronize();

  /**
   * Check if resources are initialized.
   */
  bool is_initialized() const
  {
    return is_initialized_;
  }

  /**
   * Check if brush texture is set.
   */
  bool has_brush_texture() const
  {
    return has_brush_texture_;
  }

 private:
  /**
   * Create the compute shader.
   */
  void create_shader();

  /**
   * Bind all resources to the shader.
   */
  void bind_resources(int2 image_size, uint num_pixel_rows);

  /**
   * Unbind all resources from the shader.
   */
  void unbind_resources();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Paint Utility Functions
 * \{ */

/**
 * Check if the given brush texture type is GPU-compatible.
 * Returns true for TEX_IMAGE and TEX_BLEND.
 */
bool is_brush_texture_gpu_compatible(const struct MTex *mtex);

/**
 * Create a GPU texture from an image buffer.
 * The texture will be configured for shader read/write.
 * The GPU texture is assigned to ibuf->gpu.texture for ownership.
 */
gpu::Texture *create_paint_texture_from_ibuf(blender::ImBuf *ibuf);

/**
 * Get or create GPU texture for painting.
 * Returns existing texture if compatible, creates new one otherwise.
 */
gpu::Texture *get_or_create_paint_texture(blender::ImBuf *ibuf);

/**
 * Check if ImBuf has a valid GPU texture.
 */
bool has_gpu_texture(const blender::ImBuf *ibuf);

/**
 * Synchronize GPU texture data back to CPU buffer.
 * Used for undo system integration.
 */
void sync_gpu_to_cpu(blender::ImBuf *ibuf);

/**
 * Sync only a dirty region from GPU to CPU.
 * More efficient for large images with small changes.
 */
void sync_gpu_region_to_cpu(blender::ImBuf *ibuf, int x, int y, int width, int height);

/**
 * Prepare ImBuf for GPU painting.
 * Ensures float buffer exists and GPU texture is ready.
 * Returns the GPU texture to paint on, or nullptr on failure.
 */
gpu::Texture *prepare_ibuf_for_gpu_paint(blender::ImBuf *ibuf);

/** \} */

}  // namespace blender::ed::sculpt_paint::paint::image
