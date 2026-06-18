/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief Implementation of brush texture caching system for performance optimization.
 */

#include "brush_texture_preview.h"
#include "brush_texture_shaders.h"

#include "DNA_brush_types.h"
#include "DNA_brush_enums.h"
#include "DNA_texture_types.h"
#include "DNA_image_types.h"

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_texture.h"

#include "BLI_hash.hh"
#include "BLI_hash_mm2a.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_threads.hh"
#include "BLI_vector.hh"

#include "GPU_texture.hh"
#include "GPU_shader.hh"
#include "GPU_batch.hh"
#include "GPU_framebuffer.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace blender::ed::interface {

/* -------------------------------------------------------------------- */
/** \name Global Cache Management
 * \{ */

/* Global texture cache instance */
static BrushTextureCacheManager *g_cache_manager = nullptr;
static std::mutex g_cache_mutex;

/* Cache statistics */
struct CacheStats {
  size_t total_entries = 0;
  size_t memory_usage = 0;
  size_t hit_count = 0;
  size_t miss_count = 0;
  size_t eviction_count = 0;
};

/* Forward declarations for static functions */
static uint64_t generate_texture_cache_key(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format);
static uint64_t get_texture_modification_time(const MTex *mtex);
static void cleanup_cache_if_needed(BrushTextureCacheManager *manager, size_t additional_memory);
static blender::gpu::Texture *create_texture_from_mtex(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format);
static ImBuf *create_cpu_buffer_from_mtex(const MTex *mtex, int2 resolution);

static CacheStats g_cache_stats;

/* Cache entry structure */
struct TextureCacheEntry {
  uint64_t hash_key;
  const Tex *source_tex;
  blender::gpu::Texture *gpu_texture;
  ImBuf *cpu_buffer;
  size_t memory_size;
  std::chrono::steady_clock::time_point last_access;
  std::chrono::steady_clock::time_point creation_time;
  int reference_count;
  bool is_dirty;
  bool gpu_uploaded;
  
  /* Texture parameters for validation */
  int2 resolution;
  blender::gpu::TextureFormat format;
  int texture_type;
  uint64_t source_modification_time;
};

/* Cache manager implementation */
struct BrushTextureCacheManager {
  std::unordered_map<uint64_t, std::unique_ptr<TextureCacheEntry>> entries;
  std::mutex cache_mutex;
  size_t max_memory_usage;
  size_t max_entries;
  bool auto_cleanup_enabled;
  std::chrono::seconds cleanup_interval;
  std::chrono::steady_clock::time_point last_cleanup;
  
  BrushTextureCacheManager() {
    max_memory_usage = 256 * 1024 * 1024; /* 256MB default */
    max_entries = 1000;
    auto_cleanup_enabled = true;
    cleanup_interval = std::chrono::seconds(30);
    last_cleanup = std::chrono::steady_clock::now();
  }
};

BrushTextureCacheManager *BKE_brush_texture_cache_manager_get()
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  
  if (!g_cache_manager) {
    g_cache_manager = new BrushTextureCacheManager();
  }
  
  return g_cache_manager;
}

void BKE_brush_texture_cache_manager_free()
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  
  if (g_cache_manager) {
    /* Free all cache entries */
    for (auto &pair : g_cache_manager->entries) {
      TextureCacheEntry *entry = pair.second.get();
      if (entry->gpu_texture) {
        GPU_texture_free(entry->gpu_texture);
      }
      if (entry->cpu_buffer) {
        IMB_freeImBuf(entry->cpu_buffer);
      }
    }
    
    delete g_cache_manager;
    g_cache_manager = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache Key Generation
 * \{ */

static uint64_t generate_texture_cache_key(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format)
{
  if (!mtex || !mtex->tex) {
    return 0;
  }
  
  uint64_t key = 0;
  
  /* Include texture pointer */
  key = blender::get_default_hash(reinterpret_cast<uintptr_t>(mtex->tex)) ^ key;
  
  /* Include texture parameters */
  key = blender::get_default_hash(mtex->tex->type) ^ key;
  key = blender::get_default_hash(resolution.x) ^ key;
  key = blender::get_default_hash(resolution.y) ^ key;
  key = blender::get_default_hash(static_cast<int>(format)) ^ key;
  
  /* Include mapping parameters */
  key = blender::get_default_hash(*reinterpret_cast<const int*>(&mtex->size[0])) ^ key;
  key = blender::get_default_hash(*reinterpret_cast<const int*>(&mtex->size[1])) ^ key;
  key = blender::get_default_hash(*reinterpret_cast<const int*>(&mtex->ofs[0])) ^ key;
  key = blender::get_default_hash(*reinterpret_cast<const int*>(&mtex->ofs[1])) ^ key;
  key = blender::get_default_hash(*reinterpret_cast<const int*>(&mtex->rot)) ^ key;
  
  /* Include blend parameters */
  key = blender::get_default_hash(mtex->blendtype) ^ key;
  key = blender::get_default_hash(*reinterpret_cast<const int*>(&mtex->colfac)) ^ key;
  
  /* For image textures, include image modification time */
  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    key = blender::get_default_hash(mtex->tex->ima->id.recalc) ^ key;
  }
  
  return key;
}

static uint64_t get_texture_modification_time(const MTex *mtex)
{
  if (!mtex || !mtex->tex) {
    return 0;
  }
  
  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    return mtex->tex->ima->id.recalc;
  }
  
  return mtex->tex->id.recalc;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache Operations
 * \{ */

blender::gpu::Texture *BKE_brush_texture_cache_get(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format)
{
  if (mtex) {
    if (mtex->tex) {
    }
  }
  
  if (!mtex || !mtex->tex) {
    return nullptr;
  }
  
  
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  uint64_t cache_key = generate_texture_cache_key(mtex, resolution, format);
  uint64_t mod_time = get_texture_modification_time(mtex);
  
  
  auto it = manager->entries.find(cache_key);
  if (it != manager->entries.end()) {
    TextureCacheEntry *entry = it->second.get();
    
    
    /* Check if entry is still valid */
    if (entry->source_modification_time == mod_time && !entry->is_dirty) {
      /* Update access time and reference count */
      entry->last_access = std::chrono::steady_clock::now();
      entry->reference_count++;
      
      g_cache_stats.hit_count++;
      
      /* Ensure GPU texture is uploaded */
      if (!entry->gpu_uploaded && entry->cpu_buffer) {
        if (entry->gpu_texture) {
          GPU_texture_free(entry->gpu_texture);
        }
        
        entry->gpu_texture = GPU_texture_create_2d(
            "cached_brush_texture",
            entry->cpu_buffer->x,
            entry->cpu_buffer->y,
            1,
            format,
            GPU_TEXTURE_USAGE_SHADER_READ,
            nullptr
        );
        
        if (entry->cpu_buffer->float_buffer.data) {
          GPU_texture_update(entry->gpu_texture, GPU_DATA_FLOAT, entry->cpu_buffer->float_buffer.data);
        } else if (entry->cpu_buffer->byte_buffer.data) {
          GPU_texture_update(entry->gpu_texture, GPU_DATA_UBYTE, entry->cpu_buffer->byte_buffer.data);
        }
        
        entry->gpu_uploaded = (entry->gpu_texture != nullptr);
      }
      
      return entry->gpu_texture;
    }
    else {
      /* Entry is outdated, remove it */
      if (entry->gpu_texture) {
        GPU_texture_free(entry->gpu_texture);
      }
      if (entry->cpu_buffer) {
        IMB_freeImBuf(entry->cpu_buffer);
      }
      
      g_cache_stats.memory_usage -= entry->memory_size;
      g_cache_stats.total_entries--;
      manager->entries.erase(it);
    }
  }
  else {
  }
  
  g_cache_stats.miss_count++;
  
  /* Create new cache entry */
  blender::gpu::Texture *gpu_texture = create_texture_from_mtex(mtex, resolution, format);
  if (!gpu_texture) {
    return nullptr;
  }
  
  /* Create CPU buffer for caching */
  ImBuf *cpu_buffer = create_cpu_buffer_from_mtex(mtex, resolution);
  
  /* Calculate memory usage */
  size_t memory_size = resolution.x * resolution.y * 4 * sizeof(float); /* Assume RGBA float */
  if (cpu_buffer) {
    memory_size += cpu_buffer->x * cpu_buffer->y * 4 * (cpu_buffer->float_buffer.data ? sizeof(float) : sizeof(char));
  }
  
  /* Check memory limits and cleanup if necessary */
  cleanup_cache_if_needed(manager, memory_size);
  
  /* Create cache entry */
  auto entry = std::make_unique<TextureCacheEntry>();
  entry->hash_key = cache_key;
  entry->gpu_texture = gpu_texture;
  entry->cpu_buffer = cpu_buffer;
  entry->memory_size = memory_size;
  entry->last_access = std::chrono::steady_clock::now();
  entry->creation_time = entry->last_access;
  entry->reference_count = 1;
  entry->is_dirty = false;
  entry->gpu_uploaded = true;
  entry->resolution = resolution;
  entry->format = format;
  entry->texture_type = mtex->tex->type;
  entry->source_modification_time = mod_time;
  entry->source_tex = mtex->tex;
  
  /* Add to cache */
  manager->entries[cache_key] = std::move(entry);
  
  /* Update statistics */
  g_cache_stats.total_entries++;
  g_cache_stats.memory_usage += memory_size;
  
  return gpu_texture;
}

void BKE_brush_texture_cache_release(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format)
{
  if (!mtex || !mtex->tex) {
    return;
  }
  
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  uint64_t cache_key = generate_texture_cache_key(mtex, resolution, format);
  
  auto it = manager->entries.find(cache_key);
  if (it != manager->entries.end()) {
    TextureCacheEntry *entry = it->second.get();
    if (entry->reference_count > 0) {
      entry->reference_count--;
    }
  }
}

void BKE_brush_texture_cache_invalidate(const MTex *mtex)
{
  if (mtex) {
  }
  
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  /* If no texture, clear all cache entries to prevent fallback textures */
  if (!mtex || !mtex->tex) {
    int cleared_count = 0;
    for (auto &pair : manager->entries) {
      TextureCacheEntry *entry = pair.second.get();
      if (entry->gpu_texture) {
        GPU_texture_free(entry->gpu_texture);
      }
      if (entry->cpu_buffer) {
        IMB_freeImBuf(entry->cpu_buffer);
      }
      cleared_count++;
    }
    manager->entries.clear();
    g_cache_stats.total_entries = 0;
    g_cache_stats.memory_usage = 0;
    return;
  }
  
  const uint64_t mod_time = get_texture_modification_time(mtex);
  
  /* Mark only matching entries as dirty when modification time changed. */
  int invalidated_count = 0;
  for (auto &pair : manager->entries) {
    TextureCacheEntry *entry = pair.second.get();
    if (entry->source_tex != mtex->tex) {
      continue;
    }
    if (entry->source_modification_time == mod_time) {
      continue;
    }
    entry->source_modification_time = mod_time;
    if (!entry->is_dirty) {
      entry->is_dirty = true;
      invalidated_count++;
    }
  }
  
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache Cleanup and Maintenance
 * \{ */

static void cleanup_cache_if_needed(BrushTextureCacheManager *manager, size_t additional_memory)
{
  /* Check if cleanup is needed */
  bool needs_cleanup = false;
  
  if (g_cache_stats.memory_usage + additional_memory > manager->max_memory_usage) {
    needs_cleanup = true;
  }
  
  if (g_cache_stats.total_entries >= manager->max_entries) {
    needs_cleanup = true;
  }
  
  auto now = std::chrono::steady_clock::now();
  if (manager->auto_cleanup_enabled && 
      (now - manager->last_cleanup) > manager->cleanup_interval) {
    needs_cleanup = true;
  }
  
  if (!needs_cleanup) {
    return;
  }
  
  /* Collect entries for removal */
  Vector<uint64_t> entries_to_remove;
  
  for (auto &pair : manager->entries) {
    TextureCacheEntry *entry = pair.second.get();
    
    /* Remove entries with zero references that haven't been accessed recently */
    if (entry->reference_count == 0) {
      auto time_since_access = now - entry->last_access;
      if (time_since_access > std::chrono::minutes(5)) {
        entries_to_remove.append(pair.first);
      }
    }
    
    /* Remove dirty entries */
    if (entry->is_dirty) {
      entries_to_remove.append(pair.first);
    }
  }
  
  /* If we still need more space, remove oldest entries */
  if (g_cache_stats.memory_usage + additional_memory > manager->max_memory_usage) {
    Vector<std::pair<std::chrono::steady_clock::time_point, uint64_t>> entries_by_age;
    
    for (auto &pair : manager->entries) {
      if (std::find(entries_to_remove.begin(), entries_to_remove.end(), pair.first) == entries_to_remove.end()) {
        entries_by_age.append({pair.second->last_access, pair.first});
      }
    }
    
    /* Sort by access time (oldest first) */
    std::sort(entries_by_age.begin(), entries_by_age.end());
    
    /* Remove oldest entries until we have enough space */
    size_t memory_to_free = (g_cache_stats.memory_usage + additional_memory) - manager->max_memory_usage;
    size_t freed_memory = 0;
    
    for (auto &age_pair : entries_by_age) {
      if (freed_memory >= memory_to_free) {
        break;
      }
      
      uint64_t key = age_pair.second;
      auto it = manager->entries.find(key);
      if (it != manager->entries.end()) {
        freed_memory += it->second->memory_size;
        entries_to_remove.append(key);
      }
    }
  }
  
  /* Remove selected entries */
  for (uint64_t key : entries_to_remove) {
    auto it = manager->entries.find(key);
    if (it != manager->entries.end()) {
      TextureCacheEntry *entry = it->second.get();
      
      if (entry->gpu_texture) {
        GPU_texture_free(entry->gpu_texture);
      }
      if (entry->cpu_buffer) {
        IMB_freeImBuf(entry->cpu_buffer);
      }
      
      g_cache_stats.memory_usage -= entry->memory_size;
      g_cache_stats.total_entries--;
      g_cache_stats.eviction_count++;
      
      manager->entries.erase(it);
    }
  }
  
  manager->last_cleanup = now;
}

void BKE_brush_texture_cache_cleanup()
{
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  cleanup_cache_if_needed(manager, 0);
}

void BKE_brush_texture_cache_clear()
{
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  /* Free all entries */
  for (auto &pair : manager->entries) {
    TextureCacheEntry *entry = pair.second.get();
    if (entry->gpu_texture) {
      GPU_texture_free(entry->gpu_texture);
    }
    if (entry->cpu_buffer) {
      IMB_freeImBuf(entry->cpu_buffer);
    }
  }
  
  manager->entries.clear();
  
  /* Reset statistics */
  g_cache_stats.total_entries = 0;
  g_cache_stats.memory_usage = 0;
  g_cache_stats.eviction_count = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Creation Helpers
 * \{ */

static blender::gpu::Texture *create_texture_from_mtex(const MTex *mtex, int2 resolution, blender::gpu::TextureFormat format)
{
  if (!mtex || !mtex->tex) {
    return nullptr;
  }
  
  
  /* Check if this is an image texture with valid image data */
  bool is_image_texture = (mtex->tex->type == TEX_IMAGE && mtex->tex->ima);
  
  if (is_image_texture) {
    
    /* Load image texture using the correct ImageUser from the texture */
    ImBuf *ibuf = BKE_image_acquire_ibuf(mtex->tex->ima, &mtex->tex->iuser, nullptr);
    if (!ibuf) {
      return nullptr;
    }
    
    
    /* Check if this is a texture atlas or single texture */
    if (ibuf->x > resolution.x * 2 || ibuf->y > resolution.y * 2) {
    }
    
    /* Resize if necessary */
    ImBuf *resized_ibuf = ibuf;
    if (ibuf->x != resolution.x || ibuf->y != resolution.y) {
      resized_ibuf = IMB_dupImBuf(ibuf);
      IMB_scale(resized_ibuf, resolution.x, resolution.y, IMBScaleFilter::Bilinear, false);
    }
    
    /* Create GPU texture */
    const float *texture_data = nullptr;
    float *converted_data = nullptr; /* Track converted data for cleanup */
    gpu::TextureFormat gpu_format = gpu::TextureFormat::UNORM_8_8_8_8;
    eGPUDataFormat data_format = GPU_DATA_FLOAT;
    
    if (resized_ibuf->float_buffer.data) {
      texture_data = resized_ibuf->float_buffer.data;
    }
    else if (resized_ibuf->byte_buffer.data) {
      /* Convert byte data to float RGBA to avoid driver issues with layout expectations. */
      const size_t pixel_count = size_t(resized_ibuf->x) * size_t(resized_ibuf->y);
      const size_t data_size = pixel_count * 4;
      converted_data = MEM_new_array_uninitialized<float>(data_size, "brush_texture_float");
      if (!converted_data) {
        if (resized_ibuf != ibuf) {
          IMB_freeImBuf(resized_ibuf);
        }
        BKE_image_release_ibuf(mtex->tex->ima, ibuf, nullptr);
        return nullptr;
      }
      const uchar *byte_data = resized_ibuf->byte_buffer.data;
      for (size_t i = 0; i < data_size; i++) {
        converted_data[i] = float(byte_data[i]) / 255.0f;
      }
      texture_data = converted_data;
      data_format = GPU_DATA_FLOAT;
    }
    else {
      if (resized_ibuf != ibuf) {
        IMB_freeImBuf(resized_ibuf);
      }
      BKE_image_release_ibuf(mtex->tex->ima, ibuf, nullptr);
      return nullptr;
    }
    
    
    blender::gpu::Texture *gpu_texture = GPU_texture_create_2d(
        "brush_texture_from_image",
        resolution.x,
        resolution.y,
        1,
        gpu_format,
        GPU_TEXTURE_USAGE_SHADER_READ,
        texture_data);
    
    
    /* Cleanup */
    if (converted_data) {
      MEM_delete(converted_data);
    }
    if (resized_ibuf != ibuf) {
      IMB_freeImBuf(resized_ibuf);
    }
    BKE_image_release_ibuf(mtex->tex->ima, ibuf, nullptr);
    
    return gpu_texture;
  }
  else {
    
    /* For image textures without valid image data, return nullptr to prevent placeholder */
    if (mtex->tex->type == TEX_IMAGE && !mtex->tex->ima) {
      return nullptr;
    }
    
    /* Only create procedural texture for actual procedural texture types */
    if (mtex->tex->type != TEX_IMAGE) {
      
      /* Create procedural texture */
      /* This would require implementing texture evaluation */
      /* For now, create a placeholder texture */
      size_t data_size = resolution.x * resolution.y * 4;
      float *data = MEM_new_array_zeroed<float>(data_size, "procedural_texture_data");
      
      /* Generate simple procedural pattern */
      for (int y = 0; y < resolution.y; y++) {
        for (int x = 0; x < resolution.x; x++) {
          int idx = (y * resolution.x + x) * 4;
          float u = float(x) / resolution.x;
          float v = float(y) / resolution.y;
          
          /* Simple checkerboard pattern */
          bool checker = ((int(u * 8) + int(v * 8)) % 2) == 0;
          float value = checker ? 1.0f : 0.0f;
          
          data[idx + 0] = value; /* R */
          data[idx + 1] = value; /* G */
          data[idx + 2] = value; /* B */
          data[idx + 3] = 1.0f;  /* A */
        }
      }
      
      blender::gpu::Texture *gpu_texture = GPU_texture_create_2d(
          "brush_texture_procedural",
          resolution.x,
          resolution.y,
          1,
          format,
          GPU_TEXTURE_USAGE_SHADER_READ,
          data
      );
      
      MEM_delete(data);
      return gpu_texture;
    }
    
    /* If we get here, it's an image texture without image data - return nullptr */
    return nullptr;
  }
}

static ImBuf *create_cpu_buffer_from_mtex(const MTex *mtex, int2 resolution)
{
  if (!mtex || !mtex->tex) {
    return nullptr;
  }
  
  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    /* Load and resize image */
    ImBuf *ibuf = BKE_image_acquire_ibuf(mtex->tex->ima, nullptr, nullptr);
    if (!ibuf) {
      return nullptr;
    }
    
    ImBuf *result = IMB_dupImBuf(ibuf);
    if (ibuf->x != resolution.x || ibuf->y != resolution.y) {
      IMB_scale(result, resolution.x, resolution.y, IMBScaleFilter::Bilinear, false);
    }
    
    BKE_image_release_ibuf(mtex->tex->ima, ibuf, nullptr);
    return result;
  }
  
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache Statistics and Configuration
 * \{ */

void BKE_brush_texture_cache_get_stats(BrushTextureCacheStats *stats)
{
  if (!stats) {
    return;
  }
  
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  stats->total_entries = g_cache_stats.total_entries;
  stats->memory_usage = g_cache_stats.memory_usage;
  stats->hit_count = g_cache_stats.hit_count;
  stats->miss_count = g_cache_stats.miss_count;
  stats->eviction_count = g_cache_stats.eviction_count;
  stats->hit_ratio = (g_cache_stats.hit_count + g_cache_stats.miss_count > 0) ?
      float(g_cache_stats.hit_count) / (g_cache_stats.hit_count + g_cache_stats.miss_count) : 0.0f;
}

void BKE_brush_texture_cache_set_memory_limit(size_t max_memory_bytes)
{
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  manager->max_memory_usage = max_memory_bytes;
  
  /* Trigger cleanup if we're over the new limit */
  if (g_cache_stats.memory_usage > max_memory_bytes) {
    cleanup_cache_if_needed(manager, 0);
  }
}

void BKE_brush_texture_cache_set_entry_limit(size_t max_entries)
{
  BrushTextureCacheManager *manager = BKE_brush_texture_cache_manager_get();
  std::lock_guard<std::mutex> lock(manager->cache_mutex);
  
  manager->max_entries = max_entries;
  
  /* Trigger cleanup if we're over the new limit */
  if (g_cache_stats.total_entries > max_entries) {
    cleanup_cache_if_needed(manager, 0);
  }
}

/** \} */

} // namespace blender::ed::interface
