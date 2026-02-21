/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of GPU Paint Context for sculpt painting optimization.
 */

#include "gpu_paint_context.hh"

#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.hh"
#include "BLI_utility_mixins.hh"

#include "DNA_object_types.h"

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh_pixels.hh"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

/* For StrokeCache definition. */
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::paint::image {

/* Import StrokeCache from parent namespace. */
using blender::ed::sculpt_paint::StrokeCache;

/* -------------------------------------------------------------------- */
/** \name Constructor / Destructor
 * \{ */

GPU_PaintContext::~GPU_PaintContext()
{
  free_resources();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resource Management
 * \{ */

void GPU_PaintContext::ensure_resources()
{
  if (is_initialized_) {
    return;
  }

  create_shader();

  is_initialized_ = true;
}

void GPU_PaintContext::free_resources()
{
  if (paint_shader_) {
    GPU_shader_free(paint_shader_);
    paint_shader_ = nullptr;
  }

  if (pixel_data_ssbo_) {
    GPU_storagebuf_free(pixel_data_ssbo_);
    pixel_data_ssbo_ = nullptr;
  }

  if (vertex_positions_ssbo_) {
    GPU_storagebuf_free(vertex_positions_ssbo_);
    vertex_positions_ssbo_ = nullptr;
  }

  if (triangle_indices_ssbo_) {
    GPU_storagebuf_free(triangle_indices_ssbo_);
    triangle_indices_ssbo_ = nullptr;
  }

  if (automask_data_ssbo_) {
    GPU_storagebuf_free(automask_data_ssbo_);
    automask_data_ssbo_ = nullptr;
  }

  /* Note: brush_texture_ is not owned by this class. */

  is_initialized_ = false;
  num_pixel_rows_ = 0;
  num_vertices_ = 0;
  num_triangles_ = 0;
  has_brush_texture_ = false;
  has_automask_ = false;
}

void GPU_PaintContext::create_shader()
{
  paint_shader_ = GPU_shader_create_from_info_name("gpu_paint_compute");

  /* Debug: verify shader creation. */
  static bool shader_debug_printed = false;
  if (!shader_debug_printed) {
    if (paint_shader_) {
      printf("GPU Paint: Shader 'gpu_paint_compute' created successfully\n");
    } else {
      printf("GPU Paint: FAILED to create shader 'gpu_paint_compute' - will use CPU path\n");
    }
    shader_debug_printed = true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Support Check
 * \{ */

bool GPU_PaintContext::is_gpu_paint_supported(blender::ImBuf *image_buffer)
{
  if (image_buffer == nullptr) {
    return false;
  }

  /* Check texture size limits. */
  const int max_texture_size = GPU_max_texture_size();
  if (image_buffer->x > max_texture_size || image_buffer->y > max_texture_size) {
    return false;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data Upload
 * \{ */

void GPU_PaintContext::update_brush_params(const Brush &brush,
                                           const blender::SculptSession &ss,
                                           bool invert)
{
  /* Store brush params for push constants during dispatch. */
  brush_color_ = float4(brush.rgb[0], brush.rgb[1], brush.rgb[2], 1.0f);
  secondary_color_ = float4(brush.secondary_rgb[0],
                            brush.secondary_rgb[1],
                            brush.secondary_rgb[2],
                            1.0f);

  /* Brush location and radius. */
  if (ss.cache) {
    brush_location_ = float3(ss.cache->location_symm);
    brush_radius_ = ss.cache->radius;
    brush_strength_ = ss.cache->bstrength;
    invert_ = (invert || ss.cache->invert) ? 1 : 0;

    /* Debug output for brush params. */
    static bool debug_printed = false;
    if (!debug_printed) {
      printf("GPU Paint Brush: location=(%.3f, %.3f, %.3f), radius=%.3f, "
             "strength=%.3f, hardness=%.3f\n",
             brush_location_.x,
             brush_location_.y,
             brush_location_.z,
             brush_radius_,
             brush_strength_,
             brush.hardness);
      debug_printed = true;
    }
  }
  else {
    brush_location_ = float3(ss.cursor_location);
    brush_radius_ = ss.cursor_radius;
    brush_strength_ = 1.0f;
    invert_ = invert ? 1 : 0;
  }

  /* Alpha. */
  brush_alpha_ = brush.alpha;

  /* Blend mode. */
  blend_mode_ = brush.blend;

  /* Falloff. */
  falloff_shape_ = brush.falloff_shape;
  hardness_ = brush.hardness;
  brush_tex_rotation_ = 0.0f; /* TODO: Get from texture. */
  brush_tex_size_ = float2(1.0f);
  brush_tex_offset_ = float2(0.0f);
  texture_sample_bias_ = 0.0f;
}

void GPU_PaintContext::set_brush_texture(gpu::Texture *texture, const MTex * /*mtex*/)
{
  brush_texture_ = texture;
  has_brush_texture_ = (texture != nullptr);

  /* TODO: Update brush_tex_* params from MTex parameters. */
}

int GPU_PaintContext::upload_pixel_data(
    const Span<bke::pbvh::pixels::PackedPixelRow> &pixel_rows,
    const Span<bke::pbvh::pixels::UVPrimitivePaintInput> &uv_primitives)
{
  if (pixel_rows.is_empty()) {
    return 0;
  }

  /* Pack data into uint4 format (2 uint4 per row):
   * [0]: barycentric.x (float bits), barycentric.y (float bits), image_coord.x, image_coord.y
   * [1]: tri_index, num_pixels, delta_bary_u (float bits), delta_bary_v (float bits)
   *
   * delta_barycentric_coord_u is needed to interpolate position for each pixel in the row.
   */
  Vector<uint4> packed_data;
  packed_data.reserve(pixel_rows.size() * 2);

  /* Debug: verify first few packed values. */
  static int pack_debug_counter = 0;

  for (const auto &row : pixel_rows) {
    float2 barycentric = float2(row.start_barycentric_coord);
    uint2 image_coord = uint2(row.start_image_coordinate);

    /* Get delta barycentric from UVPrimitivePaintInput. */
    const auto &uv_prim = uv_primitives[row.uv_primitive_index];
    float2 delta_bary = uv_prim.delta_barycentric_coord_u;

    /* Debug: print first row's packed data. */
    if (pack_debug_counter < 3 && &row == &pixel_rows[0]) {
      printf("GPU Pack: bary=(%.4f,%.4f) delta_bary=(%.6f,%.6f) tri=%d npix=%u\n",
             barycentric.x, barycentric.y,
             delta_bary.x, delta_bary.y,
             uv_prim.tri_index, row.num_pixels);
      pack_debug_counter++;
    }

    /* Pack barycentric coordinates as float bits. */
    uint bary_x, bary_y;
    memcpy(&bary_x, &barycentric.x, sizeof(float));
    memcpy(&bary_y, &barycentric.y, sizeof(float));

    /* Pack delta barycentric as float bits. */
    uint delta_bary_u, delta_bary_v;
    memcpy(&delta_bary_u, &delta_bary.x, sizeof(float));
    memcpy(&delta_bary_v, &delta_bary.y, sizeof(float));

    /* First uint4: barycentric + image coords. */
    packed_data.append(uint4(bary_x, bary_y, image_coord.x, image_coord.y));

    /* Second uint4: tri_index + pixel count + delta barycentric.
     * CRITICAL: uv_primitive_index is an index into uv_primitives array,
     * not a direct triangle index. We must convert it here. */
    uint tri_index = uv_prim.tri_index;
    packed_data.append(uint4(tri_index, row.num_pixels, delta_bary_u, delta_bary_v));
  }

  /* Create or resize storage buffer.
   * Always recreate if size changed to ensure buffer is large enough. */
  const size_t required_size = packed_data.size() * sizeof(uint4);
  const int required_rows = pixel_rows.size();

  if (pixel_data_ssbo_ == nullptr || required_rows != num_pixel_rows_)
  {
    if (pixel_data_ssbo_) {
      GPU_storagebuf_free(pixel_data_ssbo_);
    }
    pixel_data_ssbo_ = GPU_storagebuf_create_ex(
        required_size, nullptr, GPU_USAGE_DYNAMIC, "GPUPixelData");
    num_pixel_rows_ = required_rows;
  }

  /* Upload data. */
  GPU_storagebuf_update(pixel_data_ssbo_, packed_data.data());

  return pixel_rows.size();
}

void GPU_PaintContext::upload_geometry_data(const Span<float3> positions,
                                            const Span<int3> triangles)
{
  /* Upload vertex positions. */
  if (!positions.is_empty()) {
    const size_t positions_size = positions.size() * sizeof(float3);

    if (vertex_positions_ssbo_ == nullptr ||
        static_cast<int>(positions.size()) != num_vertices_)
    {
      if (vertex_positions_ssbo_) {
        GPU_storagebuf_free(vertex_positions_ssbo_);
      }
      vertex_positions_ssbo_ = GPU_storagebuf_create_ex(
          positions_size, nullptr, GPU_USAGE_STATIC, "GPUVertexPositions");
      num_vertices_ = positions.size();
    }

    GPU_storagebuf_update(vertex_positions_ssbo_, positions.data());
  }

  /* Upload triangle indices. */
  if (!triangles.is_empty()) {
    const size_t triangles_size = triangles.size() * sizeof(int3);

    if (triangle_indices_ssbo_ == nullptr ||
        static_cast<int>(triangles.size()) != num_triangles_)
    {
      if (triangle_indices_ssbo_) {
        GPU_storagebuf_free(triangle_indices_ssbo_);
      }
      triangle_indices_ssbo_ = GPU_storagebuf_create_ex(
          triangles_size, nullptr, GPU_USAGE_STATIC, "GPUTriangleIndices");
      num_triangles_ = triangles.size();
    }

    GPU_storagebuf_update(triangle_indices_ssbo_, triangles.data());
  }
}

void GPU_PaintContext::upload_automask_data(const Span<float> factors)
{
  if (factors.is_empty()) {
    has_automask_ = false;
    return;
  }

  has_automask_ = true;

  const size_t factors_size = factors.size() * sizeof(float);

  if (automask_data_ssbo_) {
    GPU_storagebuf_free(automask_data_ssbo_);
  }
  automask_data_ssbo_ = GPU_storagebuf_create_ex(
      factors_size, factors.data(), GPU_USAGE_STATIC, "GPUAutomaskData");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compute Dispatch
 * \{ */

void GPU_PaintContext::bind_resources(int2 image_size, uint num_pixel_rows)
{
  if (!paint_shader_) {
    return;
  }

  /* Bind storage buffers. */
  if (pixel_data_ssbo_) {
    GPU_storagebuf_bind(pixel_data_ssbo_, 3);
  }

  if (vertex_positions_ssbo_) {
    GPU_storagebuf_bind(vertex_positions_ssbo_, 0);
  }

  if (triangle_indices_ssbo_) {
    GPU_storagebuf_bind(triangle_indices_ssbo_, 1);
  }

  if (automask_data_ssbo_ && has_automask_) {
    GPU_storagebuf_bind(automask_data_ssbo_, 2);
  }

  /* Bind brush texture. */
  if (brush_texture_ && has_brush_texture_) {
    GPU_texture_bind(brush_texture_, 0);
  }

  /* Set push constants. */
  GPU_shader_uniform_2iv(paint_shader_, "image_size", image_size);
  GPU_shader_uniform_1i(paint_shader_, "num_pixel_rows", int(num_pixel_rows));
  GPU_shader_uniform_3fv(paint_shader_, "brush_location", brush_location_);
  GPU_shader_uniform_1f(paint_shader_, "brush_radius", brush_radius_);
  GPU_shader_uniform_1f(paint_shader_, "brush_strength", brush_strength_);
  GPU_shader_uniform_1f(paint_shader_, "brush_alpha", brush_alpha_);
  GPU_shader_uniform_1f(paint_shader_, "hardness", hardness_);
  GPU_shader_uniform_1f(paint_shader_, "brush_tex_rotation", brush_tex_rotation_);
  GPU_shader_uniform_2fv(paint_shader_, "brush_tex_size", brush_tex_size_);
  GPU_shader_uniform_2fv(paint_shader_, "brush_tex_offset", brush_tex_offset_);
  GPU_shader_uniform_1f(paint_shader_, "texture_sample_bias", texture_sample_bias_);
  GPU_shader_uniform_1i(paint_shader_, "blend_mode", blend_mode_);
  GPU_shader_uniform_1i(paint_shader_, "falloff_shape", falloff_shape_);
  GPU_shader_uniform_1i(paint_shader_, "invert", invert_);
  GPU_shader_uniform_1i(paint_shader_, "has_brush_texture", has_brush_texture_ ? 1 : 0);
  GPU_shader_uniform_1i(paint_shader_, "has_automask", has_automask_ ? 1 : 0);
  GPU_shader_uniform_4fv(paint_shader_, "brush_color", brush_color_);
  GPU_shader_uniform_4fv(paint_shader_, "secondary_color", secondary_color_);
}

void GPU_PaintContext::unbind_resources()
{
  if (pixel_data_ssbo_) {
    GPU_storagebuf_unbind(pixel_data_ssbo_);
  }

  if (vertex_positions_ssbo_) {
    GPU_storagebuf_unbind(vertex_positions_ssbo_);
  }

  if (triangle_indices_ssbo_) {
    GPU_storagebuf_unbind(triangle_indices_ssbo_);
  }

  if (automask_data_ssbo_) {
    GPU_storagebuf_unbind(automask_data_ssbo_);
  }

  if (brush_texture_) {
    GPU_texture_unbind(brush_texture_);
  }
}

void GPU_PaintContext::dispatch_paint(gpu::Texture *target_texture)
{
  dispatch_paint(target_texture, num_pixel_rows_);
}

void GPU_PaintContext::dispatch_paint(gpu::Texture *target_texture, int num_pixel_rows)
{
  if (!paint_shader_ || target_texture == nullptr || num_pixel_rows <= 0) {
    printf("GPU Paint: dispatch_paint early return - shader=%p tex=%p rows=%d\n",
           paint_shader_, target_texture, num_pixel_rows);
    return;
  }

  /* Get texture dimensions. */
  int width = GPU_texture_width(target_texture);
  int height = GPU_texture_height(target_texture);
  int2 image_size = int2(width, height);

  GPU_shader_bind(paint_shader_);

  /* Bind target texture as image.
   * NOTE: Use the slot number directly from shader info (IMAGE slot 5).
   * GPU_shader_get_sampler_binding doesn't work for IMAGE declarations. */
  const int kImageSlot = 5;
  GPU_texture_image_bind(target_texture, kImageSlot);

  /* Bind all other resources. */
  bind_resources(image_size, num_pixel_rows);

  /* Dispatch compute shader.
   * Each workgroup processes one pixel row. */
  GPU_compute_dispatch(paint_shader_, num_pixel_rows, 1, 1);

  /* Unbind resources. */
  unbind_resources();
  GPU_texture_image_unbind(target_texture);
  GPU_shader_unbind();
}

void GPU_PaintContext::synchronize()
{
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

bool is_brush_texture_gpu_compatible(const MTex *mtex)
{
  if (mtex == nullptr || mtex->tex == nullptr) {
    return true; /* No texture is compatible. */
  }

  switch (mtex->tex->type) {
    case TEX_IMAGE:
    case TEX_BLEND:
      return true;
    default:
      return false; /* Procedural textures need CPU fallback. */
  }
}

bool has_gpu_texture(const blender::ImBuf *ibuf)
{
  return ibuf && ibuf->gpu.texture;
}

gpu::Texture *create_paint_texture_from_ibuf(blender::ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return nullptr;
  }

  /* Check if GPU texture already exists and is compatible. */
  if (ibuf->gpu.texture) {
    return ibuf->gpu.texture;
  }

  /* Determine format based on buffer type. */
  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL | GPU_TEXTURE_USAGE_SHADER_READ |
                           GPU_TEXTURE_USAGE_SHADER_WRITE;

  gpu::Texture *texture = GPU_texture_create_2d(
      "paint_texture", ibuf->x, ibuf->y, 1, gpu::TextureFormat::SFLOAT_32_32_32_32, usage, nullptr);

  if (texture && ibuf->float_buffer.data) {
    GPU_texture_update(texture, GPU_DATA_FLOAT, ibuf->float_buffer.data);
  }

  /* Assign GPU texture to ImBuf for ownership. */
  if (texture) {
    IMB_assign_gpu_texture(ibuf, texture);
  }

  return texture;
}

void sync_gpu_to_cpu(blender::ImBuf *ibuf)
{
  if (ibuf == nullptr || ibuf->gpu.texture == nullptr) {
    return;
  }

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  float *output_buffer = static_cast<float *>(
      GPU_texture_read(ibuf->gpu.texture, GPU_DATA_FLOAT, 0));

  if (output_buffer) {
    IMB_assign_float_buffer(ibuf, output_buffer, IB_TAKE_OWNERSHIP);
  }
}

void sync_gpu_region_to_cpu(blender::ImBuf *ibuf, int /*x*/, int /*y*/, int /*width*/, int /*height*/)
{
  if (ibuf == nullptr || ibuf->gpu.texture == nullptr) {
    return;
  }

  /* TODO: Implement partial texture read when available.
   * For now, do full sync. */
  sync_gpu_to_cpu(ibuf);
}

gpu::Texture *get_or_create_paint_texture(blender::ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return nullptr;
  }

  /* Return existing texture if available. */
  if (ibuf->gpu.texture) {
    return ibuf->gpu.texture;
  }

  /* Create new texture. */
  return create_paint_texture_from_ibuf(ibuf);
}

gpu::Texture *prepare_ibuf_for_gpu_paint(blender::ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return nullptr;
  }

  /* Ensure float buffer exists. */
  if (ibuf->float_buffer.data == nullptr && ibuf->byte_buffer.data != nullptr) {
    IMB_float_from_byte(ibuf);
  }

  /* Still no float buffer? Cannot proceed. */
  if (ibuf->float_buffer.data == nullptr) {
    return nullptr;
  }

  /* Get or create GPU texture. */
  return get_or_create_paint_texture(ibuf);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::paint::image
