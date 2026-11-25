/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * A `gpu::TextureFromPool` is a wrapper around backend specific texture objects whose usage is
 * transient and can be shared between parts of an engine or across several parts of blender.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "GPU_texture.hh"

namespace blender::gpu {

class TexturePool {
 public:
  virtual ~TexturePool() = default;

  /* Return the texture pool from the active GPUContext.
   * Only valid if a context is active. */
  static TexturePool &get();

  /* Acquire a texture of a particular type from the pool. */
  virtual Texture *acquire_texture_1d(int extent,
                                      int mip_len,
                                      TextureFormat format,
                                      eGPUTextureUsage usage,
                                      const char *name = nullptr) = 0;
  virtual Texture *acquire_texture_1d_array(int extent,
                                            int layer_len,
                                            int mip_len,
                                            TextureFormat format,
                                            eGPUTextureUsage usage,
                                            const char *name = nullptr) = 0;
  virtual Texture *acquire_texture_2d(int2 extent,
                                      int mip_len,
                                      TextureFormat format,
                                      eGPUTextureUsage usage,
                                      const char *name = nullptr) = 0;
  virtual Texture *acquire_texture_2d_array(int2 extent,
                                            int layer_len,
                                            int mip_len,
                                            TextureFormat format,
                                            eGPUTextureUsage usage,
                                            const char *name = nullptr) = 0;
  virtual Texture *acquire_texture_3d(int3 extent,
                                      int mip_len,
                                      TextureFormat format,
                                      eGPUTextureUsage usage,
                                      const char *name = nullptr) = 0;
  virtual Texture *acquire_texture_cube(int extent,
                                        int mip_len,
                                        TextureFormat format,
                                        eGPUTextureUsage usage,
                                        const char *name = nullptr) = 0;
  virtual Texture *acquire_texture_cube_array(int extent,
                                              int layer_len,
                                              int mip_len,
                                              TextureFormat format,
                                              eGPUTextureUsage usage,
                                              const char *name = nullptr) = 0;

  /* Release the texture back into the pool so it can be reused. */
  virtual void release_texture(Texture *tex) = 0;

  /* Validate acquired texture counters and release unused textures.
   * If `force_free` is true, free unused texture memory inside the pool. */
  virtual void reset(bool force_free = false) = 0;

  /* Modify the internal counter of an acquired texture.
   * Used by `TextureFromPool::retain()` in `DRW_gpu_wrapper.hh`. */
  virtual void offset_users_count(Texture *tex, int offset) = 0;
};

}  // namespace blender::gpu

/* The VRAM each GPU context's texture pool may park for reuse, in bytes; 0 (the default) keeps
 * the pools unlimited. The one value every pool reads, but it bounds each pool separately: a pool
 * lives per GPU context, so N contexts may park up to N times the budget together. A release that
 * would push a pool's parked total over the budget evicts its largest parked textures first and,
 * if the texture still cannot fit, frees it instead of parking it; `reset()` evicts the same way.
 * The budget only bounds *parked* textures: textures a pass still holds are untouched. Gives the
 * pool a ceiling so heavy viewport scratch (a 4K paint stack's effect buffers) cannot park
 * gigabytes for `max_unused_cycles` frames after the pass that used them. */
void GPU_texture_pool_set_budget(uint64_t bytes);
uint64_t GPU_texture_pool_budget_get();
