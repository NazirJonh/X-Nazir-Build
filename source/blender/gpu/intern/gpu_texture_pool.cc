/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "fmt/format.h"

#include "BKE_global.hh"
#include "BLI_string.h"

#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "gpu_backend.hh"
#include "gpu_context_private.hh"
#include "gpu_texture_pool_private.hh"

namespace blender::gpu {

/** The parked-texture budget every pool reads; 0 (the default) keeps them unlimited. */
static std::atomic<uint64_t> g_texture_pool_budget_bytes = 0;

bool texture_pool_mem_log_enabled()
{
  /* Read once: the switch cannot change under a running Blender, and every pool's reset probes it.
   */
  static const bool enabled = []() {
    const char *value = getenv("BLENDER_PAINT_MEM_LOG");
    return value != nullptr && value[0] != '\0';
  }();
  return enabled;
}

void texture_pool_log_sample(TexturePoolLogState &state,
                             const char *pool,
                             const uint64_t parked_bytes,
                             const uint64_t budget_bytes,
                             const int64_t pool_count)
{
  if (!texture_pool_mem_log_enabled()) {
    return;
  }
  /* A frame's reset usually changes nothing: log only a new sample, not every draw. */
  if (state.valid && state.parked_bytes == parked_bytes && state.budget_bytes == budget_bytes &&
      state.pool_count == pool_count)
  {
    return;
  }
  state = {parked_bytes, budget_bytes, pool_count, true};
  fprintf(stderr,
          "PAINT_MEM texpool pool=%s parked_bytes=%lld budget_bytes=%lld pool_count=%lld\n",
          pool,
          (long long)parked_bytes,
          (long long)budget_bytes,
          (long long)pool_count);
}

TexturePool &TexturePool::get()
{
  BLI_assert(GPU_context_active_get() != nullptr);
  return *unwrap(GPU_context_active_get())->texture_pool;
}

TexturePoolImpl::~TexturePoolImpl()
{
  for (TextureHandle tex : acquired_) {
    GPU_texture_free(tex.texture);
  }
  for (TextureHandle tex : pool_) {
    GPU_texture_free(tex.texture);
  }
}

Texture *TexturePoolImpl::acquire_texture_impl(int3 extent,
                                               int mip_len,
                                               GPUTextureType type,
                                               TextureFormat format,
                                               eGPUTextureUsage usage,
                                               const char * /* name */)
{
  /* Determine actual mipmap depth. */
  int mip_len_max = 1 + floorf(log2f(std::max({extent.x, extent.y, extent.z})));
  mip_len = min_ii(mip_len, mip_len_max);

  /* Append format view flag for all pool textures. */
  /* TODO(not_mark): this flag can likely be removed from TexturePoolImpl entirely. */
  usage |= GPU_TEXTURE_USAGE_FORMAT_VIEW;

  /* Search pool for compatible available texture first. */
  int64_t match_index = -1;
  for (uint64_t i : pool_.index_range()) {
    Texture *tex = pool_[i].texture;

    auto tex_args = std::tuple(tex->format_get(),
                               tex->type_get(),
                               tex->width_get(),
                               tex->height_get(),
                               tex->depth_get(),
                               tex->mip_count());
    if (std::tie(format, type, UNPACK3(extent), mip_len) != tex_args) {
      continue;
    }

    eGPUTextureUsage tex_usage = tex->usage_get();
    if ((tex_usage & usage) == usage) {
      /* If a texture has overlapping usage flags, keep its index for now. */
      match_index = i;
    }
    if (tex_usage == usage) {
      /* If a texture has matching usage flags, there is no better candidate. */
      match_index = i;
      break;
    }
  }

  /* If a compatible pool texture was found, acquire and return it. */
  if (match_index != -1) {
    TextureHandle handle = {pool_[match_index].texture};
    acquired_.add(handle);
    parked_bytes_ -= GPU_texture_vram_size_get(handle.texture);
    pool_.remove_and_reorder(match_index);
    return handle.texture;
  }

  /* Generate debug label name, if one isn't passed in `name`. TexturePoolImpl ignores the
   * name argument, as returned textures do not shadow/view/abstract the underlying texture. */
  std::string name_str;
  if (G.debug & G_DEBUG_GPU) {
    name_str = fmt::format("TexFromPool_{}", pool_.size());
  }

  /* Otherwise, allocate a new texture of the specified type. */
  TextureHandle handle = {GPUBackend::get()->texture_alloc(name_str.c_str())};
  handle.texture->usage_set(usage);
  bool init_result = false;
  switch (type) {
    case GPU_TEXTURE_1D:
    case GPU_TEXTURE_1D_ARRAY:
      init_result = handle.texture->init_1D(extent.x, extent.y, mip_len, format);
      break;
    case GPU_TEXTURE_2D:
    case GPU_TEXTURE_2D_ARRAY:
      init_result = handle.texture->init_2D(extent.x, extent.y, extent.z, mip_len, format);
      break;
    case GPU_TEXTURE_3D:
      init_result = handle.texture->init_3D(extent.x, extent.y, extent.z, mip_len, format);
      break;
    case GPU_TEXTURE_CUBE:
    case GPU_TEXTURE_CUBE_ARRAY:
      init_result = handle.texture->init_cubemap(extent.x, extent.y, mip_len, format);
      break;
    default:
      BLI_assert_unreachable();
      break;
  }
  UNUSED_VARS_NDEBUG(init_result);
  BLI_assert(init_result);

  acquired_.add(handle);
  return handle.texture;
}  // acquire_texture_impl

void TexturePoolImpl::enforce_pool_budget(const size_t incoming_bytes)
{
  const uint64_t budget = g_texture_pool_budget_bytes.load(std::memory_order_relaxed);
  if (budget == 0) {
    return;
  }
  /* Evict the largest parked textures first, the order `reset()` uses: the small ones are the cheap
   * re-allocations the multi-cycle parking exists to avoid (#113024), the 4K-scale ones are the
   * gigabytes worth giving up. `incoming_bytes` is not in `pool_` yet, so it is only an additive
   * term here; the caller frees it if even an empty pool cannot fit it. */
  while (parked_bytes_ + incoming_bytes > budget) {
    int64_t largest = -1;
    size_t largest_bytes = 0;
    for (const int64_t i : pool_.index_range()) {
      const size_t bytes = GPU_texture_vram_size_get(pool_[i].texture);
      if (bytes > largest_bytes) {
        largest_bytes = bytes;
        largest = i;
      }
    }
    if (largest == -1) {
      break;
    }
    parked_bytes_ -= largest_bytes;
    GPU_texture_free(pool_[largest].texture);
    pool_.remove_and_reorder(largest);
  }
}

void TexturePoolImpl::release_texture(Texture *tex)
{
  BLI_assert_msg(acquired_.contains({tex}),
                 "Unacquired texture passed to TexturePool::release_texture()");
  acquired_.remove({tex});

  /* Park the texture only if the parked total still fits the budget; a release that would push it
   * over evicts the largest parked textures first and, if even an empty pool cannot take this one,
   * frees it instead, so a pass's scratch does not outlive the pass that used it. */
  const size_t bytes = GPU_texture_vram_size_get(tex);
  const uint64_t budget = g_texture_pool_budget_bytes.load(std::memory_order_relaxed);
  if (budget != 0) {
    enforce_pool_budget(bytes);
    /* Soft limit: a concurrent budget change can land between the enforcement and this check, and
     * `reset()` enforces the new value again. */
    if (parked_bytes_ + bytes > budget) {
      GPU_texture_free(tex);
      return;
    }
  }

  pool_.append({tex});
  parked_bytes_ += bytes;
}

void TexturePoolImpl::offset_users_count(Texture *tex, int offset)
{
  BLI_assert_msg(acquired_.contains({tex}),
                 "Unacquired texture passed to TexturePool::offset_users_count()");
  int users_count = acquired_.lookup_key({tex}).users_count;
  acquired_.add_overwrite({tex, users_count + offset, 0});
}

void TexturePoolImpl::reset(bool force_free)
{
#ifndef NDEBUG
  /* Iterate acquired textures, and ensure the internal counter equals 0; otherwise
   * this indicates a missing `::retain()` or `::release()`. */
  for (const TextureHandle &tex : acquired_) {
    BLI_assert_msg(tex.users_count == 0,
                   "Missing texture release/retain. Likely TextureFromPool::release(), "
                   "TextureFromPool::retain() or TexturePool::release_texture().");
  }
#endif

  /* Reverse iterate pool textures, to make sure we only reorder known good handles. */
  for (int i = pool_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = pool_[i];
    if (tex.unused_cycles_count >= max_unused_cycles_ || force_free) {
      parked_bytes_ -= GPU_texture_vram_size_get(tex.texture);
      GPU_texture_free(tex.texture);
      pool_.remove_and_reorder(i);
    }
    else {
      tex.unused_cycles_count++;
    }
  }

  /* Enforce the budget through the same largest-first eviction a release uses, so the two paths
   * cannot disagree about what is given up first (see #113024). */
  enforce_pool_budget(0);

  /* One probe per reset (a draw's / a frame's boundary), not per release, and only when the sample
   * changed: logging every parked texture would flood a 4K session with thousands of lines a
   * frame. */
  texture_pool_log_sample(log_state_,
                          "impl",
                          parked_bytes_,
                          g_texture_pool_budget_bytes.load(std::memory_order_relaxed),
                          int64_t(pool_.size()));
}

}  // namespace blender::gpu

void GPU_texture_pool_set_budget(const uint64_t bytes)
{
  blender::gpu::g_texture_pool_budget_bytes.store(bytes, std::memory_order_relaxed);
}

uint64_t GPU_texture_pool_budget_get()
{
  return blender::gpu::g_texture_pool_budget_bytes.load(std::memory_order_relaxed);
}
