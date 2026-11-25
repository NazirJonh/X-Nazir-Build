/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_texture_pool.hh"
#include "gpu_texture_private.hh"

namespace blender::gpu {

/**
 * The VRAM a texture allocation holds, mip chain and array layers included. Shared by the pools
 * that park individual textures (TexturePoolImpl, GLTexturePool, MTLTexturePool); the Vulkan pool
 * budgets whole unused allocations instead and measures those by `allocation_info.size`.
 */
size_t texture_pool_allocation_bytes(const Texture *tex);

/**
 * Whether `BLENDER_PAINT_MEM_LOG` is set, read once. Gates the per-reset `PAINT_MEM texpool` probe
 * of every pool; a no-op without the variable.
 */
bool texture_pool_mem_log_enabled();

/** Last `PAINT_MEM texpool` sample of one pool, so an unchanged reset logs nothing. */
struct TexturePoolLogState {
  uint64_t parked_bytes = 0;
  uint64_t budget_bytes = 0;
  int64_t pool_count = -1;
  bool valid = false;
};

/**
 * Print one `PAINT_MEM texpool` line for \a pool when the sample changed since the last call, or
 * on the first sample. A no-op without `BLENDER_PAINT_MEM_LOG`.
 */
void texture_pool_log_sample(TexturePoolLogState &state,
                             const char *pool,
                             uint64_t parked_bytes,
                             uint64_t budget_bytes,
                             int64_t pool_count);

/**
 * TexturePool base class, which connects `acquire_texture_*` variants to
 * virtual `acquire_texture_impl` which backend implementations implement.
 */
class TexturePoolBase : public TexturePool {
 protected:
  virtual Texture *acquire_texture_impl(int3 extent,
                                        int mip_len,
                                        GPUTextureType type,
                                        TextureFormat format,
                                        eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL,
                                        const char *name = nullptr) = 0;

 public:
  Texture *acquire_texture_1d(int extent,
                              int mip_len,
                              TextureFormat format,
                              eGPUTextureUsage usage,
                              const char *name = nullptr) override
  {
    return acquire_texture_impl({extent, 0, 0}, mip_len, GPU_TEXTURE_1D, format, usage, name);
  }
  Texture *acquire_texture_1d_array(int extent,
                                    int layer_len,
                                    int mip_len,
                                    TextureFormat format,
                                    eGPUTextureUsage usage,
                                    const char *name = nullptr) override
  {
    return acquire_texture_impl(
        {extent, layer_len, 0}, mip_len, GPU_TEXTURE_1D_ARRAY, format, usage, name);
  }
  Texture *acquire_texture_2d(int2 extent,
                              int mip_len,
                              TextureFormat format,
                              eGPUTextureUsage usage,
                              const char *name = nullptr) override
  {
    return acquire_texture_impl(
        {extent.x, extent.y, 0}, mip_len, GPU_TEXTURE_2D, format, usage, name);
  }
  Texture *acquire_texture_2d_array(int2 extent,
                                    int layer_len,
                                    int mip_len,
                                    TextureFormat format,
                                    eGPUTextureUsage usage,
                                    const char *name = nullptr) override
  {
    return acquire_texture_impl(
        {extent.x, extent.y, layer_len}, mip_len, GPU_TEXTURE_2D_ARRAY, format, usage, name);
  }
  Texture *acquire_texture_3d(int3 extent,
                              int mip_len,
                              TextureFormat format,
                              eGPUTextureUsage usage,
                              const char *name = nullptr) override
  {
    return acquire_texture_impl(extent, mip_len, GPU_TEXTURE_3D, format, usage, name);
  }
  Texture *acquire_texture_cube(int extent,
                                int mip_len,
                                TextureFormat format,
                                eGPUTextureUsage usage,
                                const char *name = nullptr) override
  {
    return acquire_texture_impl(
        {extent, extent, 0}, mip_len, GPU_TEXTURE_CUBE, format, usage, name);
  }
  Texture *acquire_texture_cube_array(int extent,
                                      int layer_len,
                                      int mip_len,
                                      TextureFormat format,
                                      eGPUTextureUsage usage,
                                      const char *name = nullptr) override
  {
    return acquire_texture_impl(
        {extent, extent, layer_len}, mip_len, GPU_TEXTURE_CUBE_ARRAY, format, usage, name);
  }
};

/**
 * Old texture pool implementation, to be used while a backend-specific
 * implementation is not yet available.
 */
class TexturePoolImpl : public TexturePoolBase {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  /* Internal packet for texture, which supports set insertion. */
  struct TextureHandle {
    Texture *texture;
    /* Counter to track texture acquire/retain mismatches in `acquire_`.  */
    int users_count = 1;
    /* Counter to track the number of unused cycles before deallocation in `pool_`. */
    int unused_cycles_count = 0;

    /* We use the pointer as hash/comparator, as a texture cannot be acquired twice. */
    uint64_t hash() const
    {
      return get_default_hash(texture);
    }

    bool operator==(const TextureHandle &o) const
    {
      return texture == o.texture;
    }
  };

  /* Pool of textures ready to be reused. */
  Vector<TextureHandle> pool_;
  /* Set of textures currently in use. */
  Set<TextureHandle> acquired_;
  /* VRAM of every texture in `pool_`, kept in step with the append/remove/reuse paths so the budget
   * is checked in O(1) instead of re-walking every parked mip chain per release. */
  size_t parked_bytes_ = 0;
  /** Last diagnostic sample, so a reset that changes nothing logs nothing. */
  TexturePoolLogState log_state_;

  /* Free the largest parked textures until `parked_bytes_ + incoming_bytes` fits the live budget.
   * The same order `reset()` uses; leaves `parked_bytes_` honest, never touches `incoming`. */
  void enforce_pool_budget(size_t incoming_bytes);

 protected:
  Texture *acquire_texture_impl(int3 extent,
                                int mip_len,
                                GPUTextureType type,
                                TextureFormat format,
                                eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL,
                                const char *name = nullptr) override;

 public:
  ~TexturePoolImpl() override;

  void release_texture(Texture *tex) override;

  void reset(bool force_free = false) override;

  void offset_users_count(Texture *tex, int offset) override;
};

}  // namespace blender::gpu
