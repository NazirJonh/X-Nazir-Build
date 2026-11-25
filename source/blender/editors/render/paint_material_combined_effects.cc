/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * The GPU Combined preview's pass machinery and its effect stage; the split-off half of
 * #paint_material_combined_gather.cc. #CombinedPasses dispatches one pass of each kind over whatever
 * target the caller names, and #combined_preview_gpu_effects_run runs the effects: the walk that
 * collects them in their rows' order, the GPU result cache's loop with each input's size, plan and
 * fingerprints, the input rendered whole at its own resolution, and the pass plan dispatched and
 * published. A CPU-computed kind the GPU path serves from the CPU effect cache's buffer (the
 * node-group filter) is the exception: no plan, no input render -- the buffer
 * #BKE_paint_layers_effect_buffer_result hands over is uploaded whole into the cache texture
 * (#combined_effect_bake_upload) and published like a dispatched result. The dispatch order, the
 * barriers and the texture releases are the ones the gather established; nothing here changes them.
 */

#include "paint_material_combined_effects.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "BKE_image.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_decal_map.hh"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_utils.hh"
#include "BLI_rect.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::ed::material_combined {

/**
 * A draw-thread LRU of GPU entries: oldest first, a byte budget, and the entries the budget evicted
 * parked in `pending_free` until the next call can hand them to the driver. The release hook is the
 * caller's, since the surface and Decal entries free different textures. It holds no mutex: only the
 * draw thread touches these, unlike the CPU stores.
 */
template<typename Entry> struct GpuLruStore {
  Vector<Entry> entries;
  size_t total_bytes = 0;
  Vector<Entry> pending_free;

  void pending_free_apply(void (*release)(Entry &))
  {
    for (Entry &entry : pending_free) {
      release(entry);
    }
    pending_free.clear_and_shrink();
  }

  void free_all(void (*release)(Entry &))
  {
    this->pending_free_apply(release);
    for (Entry &entry : entries) {
      release(entry);
    }
    entries.clear_and_shrink();
    total_bytes = 0;
  }

  /** Make \a index the youngest entry; the returned pointer is valid until the next change. */
  const Entry *touch(const int index)
  {
    Entry touched = std::move(entries[index]);
    entries.remove(index);
    entries.append(std::move(touched));
    return &entries.last();
  }

  /** Take \a index out of the store, leaving its bytes accounted for. */
  Entry take(const int index)
  {
    Entry entry = std::move(entries[index]);
    total_bytes -= entry.bytes;
    entries.remove(index);
    return entry;
  }

  void append(Entry entry)
  {
    total_bytes += entry.bytes;
    entries.append(std::move(entry));
  }

  /** Evict from the front until \a bytes fit; the evicted entries are parked for release. */
  void evict_for(const size_t bytes,
                 const size_t max_bytes,
                 size_t *r_evicted_bytes = nullptr)
  {
    while (!entries.is_empty() && total_bytes + bytes > max_bytes) {
      Entry oldest = std::move(entries[0]);
      total_bytes -= oldest.bytes;
      if (r_evicted_bytes != nullptr) {
        *r_evicted_bytes += oldest.bytes;
      }
      pending_free.append(std::move(oldest));
      entries.remove(0);
    }
  }
};

/**
 * The GPU texture of \a image read through \a image_user (null selects the default), and whether it
 * stores premultiplied colour, by the rule the texture upload used. The upload premultiplies a byte
 * buffer (assumed straight) and straightens a float one (assumed premultiplied) exactly when it
 * stores premultiplied, so dividing by alpha gives the straight colour `composite_decode_tile`
 * decodes. Null when the image has no GPU texture.
 *
 * Always the base tile of a tiled image: the texture is cached once per image, so it cannot follow
 * a tile. #combined_preview_gpu_update refuses a tile other than the base for a tiled stack, so
 * this never serves another tile's plan.
 */
gpu::Texture *gpu_map_texture(Image *image,
                              const ImageUser *image_user,
                              bool &r_premultiplied)
{
  ImageUser iuser;
  if (image_user != nullptr) {
    iuser = *image_user;
  }
  else {
    BKE_imageuser_default(&iuser);
  }
  gpu::Texture *texture = BKE_image_get_gpu_texture(image, &iuser);
  if (texture == nullptr) {
    return nullptr;
  }
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
  r_premultiplied = BKE_image_has_gpu_texture_premultiplied_alpha(image, ibuf);
  BKE_image_release_ibuf(image, ibuf, nullptr);
  return texture;
}

/**
 * Turn a row's UV \a mapping into the flags and values the shaders read, when it applies. The same
 * values the CPU resample reads, the scale through the shared normalization.
 */
static void gpu_mapping_setup(const PaintMaterialCompositeMapping &mapping,
                              int &r_flags,
                              GpuMappingUniforms &r_uniforms)
{
  if (!mapping.enabled) {
    return;
  }
  bool repeat_u = true;
  bool repeat_v = true;
  BKE_paint_layers_mapping_wrap_axes(mapping.wrap, repeat_u, repeat_v);
  r_flags |= gpu_layer_mapped;
  if (repeat_u) {
    r_flags |= gpu_layer_repeat_u;
  }
  if (repeat_v) {
    r_flags |= gpu_layer_repeat_v;
  }
  if (mapping.tangent_normal) {
    r_flags |= gpu_layer_tangent_normal;
  }
  if (mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U)) {
    r_flags |= gpu_layer_mirror_u;
  }
  if (mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V)) {
    r_flags |= gpu_layer_mirror_v;
  }
  /* The direction bits only matter with their axis set; the shader reads them with it. */
  if (mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U) &&
      mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U_NEGATIVE))
  {
    r_flags |= gpu_layer_mirror_u_neg;
  }
  if (mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V) &&
      mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V_NEGATIVE))
  {
    r_flags |= gpu_layer_mirror_v_neg;
  }
  r_uniforms.offset = float2(mapping.offset[0], mapping.offset[1]);
  r_uniforms.scale = float2(BKE_paint_layers_mapping_scale_normalize(mapping.scale[0]),
                            BKE_paint_layers_mapping_scale_normalize(mapping.scale[1]));
  r_uniforms.rotation = float2(std::cos(mapping.rotation), std::sin(mapping.rotation));
  r_uniforms.mirror_center = float2(mapping.mirror_center[0], mapping.mirror_center[1]);
}

/** Upload the mapping values of \a uniforms; the three passes name them alike. */
static void gpu_mapping_uniforms_set(gpu::Shader *shader, const GpuMappingUniforms &uniforms)
{
  GPU_shader_uniform_2fv(shader, "mapping_offset", &uniforms.offset.x);
  GPU_shader_uniform_2fv(shader, "mapping_scale", &uniforms.scale.x);
  GPU_shader_uniform_2fv(shader, "mapping_rotation", &uniforms.rotation.x);
  GPU_shader_uniform_2fv(shader, "mapping_mirror_center", &uniforms.mirror_center.x);
}

GpuInputSteps::GpuInputSteps(Span<PaintMaterialCompositeMapping> steps)
{
  /* The CPU refusal in #BKE_paint_layers_gpu_unsupported_reason keeps a longer list from getting
   * here; the clamp below only protects a release build if that contract ever breaks. */
  BLI_assert(int(steps.size()) <= gpu_input_step_max);
  for (const int index : IndexRange(std::min(int(steps.size()), gpu_input_step_max))) {
    const PaintMaterialCompositeMapping &step = steps[index];
    float4 &a = (index == 0) ? step_0_a : step_1_a;
    float4 &b = (index == 0) ? step_0_b : step_1_b;
    a = float4(step.offset[0],
               step.offset[1],
               BKE_paint_layers_mapping_scale_normalize(step.scale[0]),
               BKE_paint_layers_mapping_scale_normalize(step.scale[1]));
    b = float4(std::cos(step.rotation), std::sin(step.rotation),
               step.mirror_center[0], step.mirror_center[1]);
    const int shift = index * gpu_input_step_bits;
    bool repeat_u = true;
    bool repeat_v = true;
    BKE_paint_layers_mapping_wrap_axes(step.wrap, repeat_u, repeat_v);
    int step_flags = gpu_input_step_mapped;
    if (repeat_u) {
      step_flags |= gpu_input_step_repeat_u;
    }
    if (repeat_v) {
      step_flags |= gpu_input_step_repeat_v;
    }
    if (step.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U)) {
      step_flags |= gpu_input_step_mirror_u;
    }
    if (step.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V)) {
      step_flags |= gpu_input_step_mirror_v;
    }
    if (step.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U) &&
        step.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U_NEGATIVE))
    {
      step_flags |= gpu_input_step_mirror_u_neg;
    }
    if (step.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V) &&
        step.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V_NEGATIVE))
    {
      step_flags |= gpu_input_step_mirror_v_neg;
    }
    flags |= step_flags << shift;
    count++;
  }
}

/** Upload the packed steps; the three passes name them alike. */
static void gpu_input_steps_uniforms_set(gpu::Shader *shader, const GpuInputSteps &steps)
{
  GPU_shader_uniform_4fv(shader, "input_step_0_a", &steps.step_0_a.x);
  GPU_shader_uniform_4fv(shader, "input_step_0_b", &steps.step_0_b.x);
  GPU_shader_uniform_4fv(shader, "input_step_1_a", &steps.step_1_a.x);
  GPU_shader_uniform_4fv(shader, "input_step_1_b", &steps.step_1_b.x);
  GPU_shader_uniform_1i(shader, "input_step_flags", steps.flags);
  GPU_shader_uniform_1i(shader, "input_step_count", steps.count);
}

/** The passes that keep their own placement (mask items, corrections) run with no steps: the row's
 * transform effects move what the row itself lays. */
static const GpuInputSteps gpu_input_steps_none;

/* -------------------------------------------------------------------- */
/** \name Projection surface map
 *
 * The painted surface per canvas texel (#BKE_paint_material_surface_map_get) uploaded as two
 * RGBA32F textures: object-space position (`w` = 1 where covered) and normal. The textures live
 * with the map they were made from, in an LRU store like the BKE cache's: a frame that projects
 * several objects or tiles alternates between the entries instead of replacing them, and the byte
 * budget evicts the least recently used ones. An evicted entry is not freed at once: the passes
 * read the raw pointers of #GpuProjection after the call that filled them, so the eviction parks
 * the entry until the next call lets the driver have it.
 * \{ */

/** What the surface map textures may hold in sum: two 4K RGBA32F maps are 1 GiB, a smaller canvas
 * fits proportionally more. */
constexpr size_t GPU_SURFACE_CACHE_BYTES = size_t(1024) * 1024 * 1024;

/** One uploaded target: the map the textures were made from (its identity answers whether they
 * still hold what a rebuilt map would) and what they were made for, to recognize them while a
 * stroke freezes the map. */
struct GpuSurfaceMapEntry {
  std::shared_ptr<const PaintMaterialSurfaceMap> map;
  gpu::Texture *position = nullptr;
  gpu::Texture *normal = nullptr;
  /** The per-triangle tangent basis the normal's `w` indexes (#PaintMaterialSurfaceMap::tri_basis). */
  gpu::Texture *tri_basis = nullptr;
  uint32_t object_uid = 0;
  uint32_t material_uid = 0;
  int tile_number = 0;
  int canvas = 0;
  /** The bytes of both textures: the budget counts them, not the entries. */
  size_t bytes = 0;
};

static GpuLruStore<GpuSurfaceMapEntry> gpu_surface_cache;

static void gpu_surface_entry_release(GpuSurfaceMapEntry &entry)
{
  if (entry.position != nullptr) {
    GPU_texture_free(entry.position);
  }
  if (entry.normal != nullptr) {
    GPU_texture_free(entry.normal);
  }
  if (entry.tri_basis != nullptr) {
    GPU_texture_free(entry.tri_basis);
  }
  entry.position = nullptr;
  entry.normal = nullptr;
  entry.tri_basis = nullptr;
  entry.map.reset();
}

/** Free the parked evicted entries: a pass that read them has had its own call since, so nothing
 * reads the textures any longer. */
static void gpu_surface_cache_pending_free()
{
  gpu_surface_cache.pending_free_apply(gpu_surface_entry_release);
}

static void gpu_decal_cache_free();

void combined_projection_gpu_free()
{
  gpu_surface_cache.free_all(gpu_surface_entry_release);
  /* The CPU twin is dropped with it: it is the same 0.5 GiB per 4K canvas and is rebuilt on demand. */
  BKE_paint_material_surface_map_free();
  gpu_decal_cache_free();
}

static gpu::Texture *gpu_surface_texture_create(const char *name,
                                                const int canvas,
                                                const Array<float4> &texels,
                                                const gpu::TextureFormat format)
{
  gpu::Texture *texture = GPU_texture_create_2d(name,
                                                canvas,
                                                canvas,
                                                1,
                                                format,
                                                GPU_TEXTURE_USAGE_SHADER_READ,
                                                &texels[0].x);
  return texture;
}

/** The triangle basis table (#PaintMaterialSurfaceMap::tri_basis): full-float, since the basis is
 * unnormalized and the conversion's result depends on its magnitude. */
static gpu::Texture *gpu_surface_tri_basis_texture_create(const Array<float4> &basis)
{
  const int height = int(basis.size() / PAINT_MATERIAL_SURFACE_TRI_BASIS_WIDTH);
  if (height <= 0) {
    return nullptr;
  }
  return GPU_texture_create_2d("combined_surface_tri_basis",
                               PAINT_MATERIAL_SURFACE_TRI_BASIS_WIDTH,
                               height,
                               1,
                               gpu::TextureFormat::SFLOAT_32_32_32_32,
                               GPU_TEXTURE_USAGE_SHADER_READ,
                               &basis[0].x);
}

/** The bytes the GPU holds for \a map: the two canvas textures and the triangle basis. */
static size_t gpu_surface_map_bytes(const PaintMaterialSurfaceMap &map, const int canvas)
{
  return size_t(canvas) * size_t(canvas) * sizeof(float[4]) * 2 +
         size_t(map.tri_basis.size()) * sizeof(float4);
}

/* -------------------------------------------------------------------- */
/* Decal map
 *
 * A Decal row's unfolded patch (#BKE_paint_material_decal_map_get) on the GPU, as two textures:
 * - a canvas-sized RGBA32F "position" `(q.x, q.y, triangle id, 1)`, with all zeros where the patch
 *   does not reach. It takes the place of the surface position texture in the shader's
 *   projection read, so `p_to_q` (a diagonal aspect matrix for a Decal) yields exactly the CPU's
 *   `q` and a texel outside the patch is the transparent one the surface map's `w = 0` already is.
 *   Full-float: `q` steps visibly in a half on a 4K canvas, and a triangle id is exact in a float
 *   up to 2^24 (the patch holds at most #PAINT_MATERIAL_DECAL_MAX_TRIS). One texture instead of an
 *   RG32F plus an R32I saves a sampler in every pass and the shader's integer-sampler path, for
 *   4 bytes a texel more (256 MiB against 192 MiB at 4K).
 * - the frames table: three RGBA32F texels per patch triangle (its three axes), a few hundred KiB.
 *
 * The entries are keyed by the BKE map's key, so a rebuilt-but-identical map reuses its textures.
 * A gizmo drag makes a new key per frame: the byte budget evicts the old ones, parked like the
 * surface entries until the next call. */

constexpr size_t GPU_DECAL_CACHE_BYTES = size_t(768) * 1024 * 1024;
/** The frames table's row width in texels; a multiple of three, so a triangle's texels never
 * straddle a row. The shader reads the width back with textureSize. */
constexpr int GPU_DECAL_FRAME_TABLE_WIDTH = 1536;
/* A patch triangle's three axes share one row: a width that is not a multiple of three would
 * straddle the row end and the shader's `tri * 3` indexing would read the wrong texels. */
static_assert(GPU_DECAL_FRAME_TABLE_WIDTH % 3 == 0);
/** Rows of the canvas texture uploaded per call, so the staging copy stays a few MiB. */
constexpr int GPU_DECAL_UPLOAD_ROWS = 64;

struct GpuDecalMapEntry {
  PaintMaterialDecalMapKey key;
  gpu::Texture *position = nullptr;
  gpu::Texture *frames = nullptr;
  size_t bytes = 0;
};

static GpuLruStore<GpuDecalMapEntry> gpu_decal_cache;

static void gpu_decal_entry_release(GpuDecalMapEntry &entry)
{
  if (entry.position != nullptr) {
    GPU_texture_free(entry.position);
  }
  if (entry.frames != nullptr) {
    GPU_texture_free(entry.frames);
  }
  entry.position = nullptr;
  entry.frames = nullptr;
}

static void gpu_decal_cache_pending_free()
{
  gpu_decal_cache.pending_free_apply(gpu_decal_entry_release);
}

static void gpu_decal_cache_free()
{
  gpu_decal_cache.free_all(gpu_decal_entry_release);
  /* The CPU twin goes with it, as the surface map's does. */
  BKE_paint_material_decal_map_free();
}

/** Upload \a map into \a entry's textures; false when a texture could not be allocated. */
static bool gpu_decal_entry_upload(const PaintMaterialDecalMap &map,
                                   const int canvas,
                                   GpuDecalMapEntry &entry)
{
  entry.position = GPU_texture_create_2d("combined_decal_position",
                                         canvas,
                                         canvas,
                                         1,
                                         gpu::TextureFormat::SFLOAT_32_32_32_32,
                                         GPU_TEXTURE_USAGE_SHADER_READ,
                                         nullptr);
  const int64_t tris_num = map.frames.size();
  const int64_t texels_num = std::max<int64_t>(tris_num * 3, 3);
  const int rows_num = int((texels_num + GPU_DECAL_FRAME_TABLE_WIDTH - 1) /
                           GPU_DECAL_FRAME_TABLE_WIDTH);
  Array<float4> table(int64_t(rows_num) * GPU_DECAL_FRAME_TABLE_WIDTH, float4(0.0f));
  for (const int64_t tri : IndexRange(tris_num)) {
    for (const int axis : IndexRange(3)) {
      const float *v = map.frames[tri].axes[axis];
      table[tri * 3 + axis] = float4(v[0], v[1], v[2], 0.0f);
    }
  }
  entry.frames = GPU_texture_create_2d("combined_decal_frames",
                                       GPU_DECAL_FRAME_TABLE_WIDTH,
                                       rows_num,
                                       1,
                                       gpu::TextureFormat::SFLOAT_32_32_32_32,
                                       GPU_TEXTURE_USAGE_SHADER_READ,
                                       &table[0].x);
  if (entry.position == nullptr || entry.frames == nullptr) {
    return false;
  }
  Array<float4> strip(int64_t(GPU_DECAL_UPLOAD_ROWS) * canvas);
  for (int y0 = 0; y0 < canvas; y0 += GPU_DECAL_UPLOAD_ROWS) {
    const int rows = std::min(GPU_DECAL_UPLOAD_ROWS, canvas - y0);
    /* Every row of the strip is its own texels, so the fill runs in parallel: a 4K canvas is 16M
     * texels on the draw thread. */
    threading::parallel_for(IndexRange(rows), 8, [&](const IndexRange row_range) {
      for (const int row : row_range) {
        for (const int x : IndexRange(canvas)) {
          const int64_t index = int64_t(y0 + row) * canvas + x;
          const int tri = map.patch_tri[index];
          strip[int64_t(row) * canvas + x] = tri >= 0 ? float4(map.q[index].x,
                                                               map.q[index].y,
                                                               float(tri),
                                                               1.0f) :
                                                         float4(0.0f);
        }
      }
    });
    GPU_texture_update_sub(
        entry.position, GPU_DATA_FLOAT, &strip[0].x, 0, y0, 0, canvas, rows, 0);
  }
  return true;
}

static size_t gpu_decal_map_bytes(const PaintMaterialDecalMap &map, const int canvas)
{
  return size_t(canvas) * size_t(canvas) * sizeof(float4) +
         (size_t(map.frames.size()) * 3 + GPU_DECAL_FRAME_TABLE_WIDTH) * sizeof(float4);
}

/**
 * The textures of \a projection's Decal map at \a canvas, uploaded or reused, as the youngest
 * entry. While a stroke runs the last entry of the same object, tile and size stands in: a sculpt
 * step moves vertices every dab and the patch would be rebuilt at each.
 */
static const GpuDecalMapEntry *gpu_decal_entry_get(const PaintMaterialCompositeProjection &projection,
                                                   const int canvas)
{
  gpu_decal_cache_pending_free();
  const uint32_t object_uid = projection.object->id.session_uid;
  const auto touch = [&](const int index) -> const GpuDecalMapEntry * {
    return gpu_decal_cache.touch(index);
  };
  if (BKE_paint_layers_bake_stroke_active_get()) {
    for (int i = gpu_decal_cache.entries.size() - 1; i >= 0; i--) {
      const PaintMaterialDecalMapKey &key = gpu_decal_cache.entries[i].key;
      if (key.object_uid == object_uid && key.tile_number == projection.tile_number &&
          key.width == canvas)
      {
        return touch(i);
      }
    }
  }
  bke::paint_perf::ScopedTimer perf_map("proj.gpu.decal_map");
  const std::shared_ptr<const PaintMaterialDecalMap> map = BKE_paint_material_decal_map_get(
      projection, canvas, canvas);
  if (map == nullptr) {
    gpu_refuse("a Decal row has no unfolded patch");
    return nullptr;
  }
  for (const int i : gpu_decal_cache.entries.index_range()) {
    if (gpu_decal_cache.entries[i].key == map->key) {
      perf_map.note("reused canvas=%d", canvas);
      return touch(i);
    }
  }
  const size_t bytes = gpu_decal_map_bytes(*map, canvas);
  gpu_decal_cache.evict_for(bytes, GPU_DECAL_CACHE_BYTES);
  GpuDecalMapEntry entry;
  entry.key = map->key;
  entry.bytes = bytes;
  if (!gpu_decal_entry_upload(*map, canvas, entry)) {
    gpu_decal_entry_release(entry);
    gpu_refuse("the Decal map textures could not be allocated");
    return nullptr;
  }
  /* The upload is read by the dispatch that follows. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  gpu_decal_cache.append(std::move(entry));
  perf_map.note("uploaded canvas=%d tris=%d", canvas, int(map->frames.size()));
  return &gpu_decal_cache.entries.last();
}

bool combined_projection_gpu_make(const PaintMaterialCompositeProjection &projection,
                                  const PaintMaterialCompositeMapping &mapping,
                                  GpuProjection &r_projection)
{
  /* The row's Mirror folds the map coordinate inside the projection (the CPU resample and the
   * generated graph do), and the projected GPU read clears the plain mapping's fold flags: it has
   * no such step yet, so the CPU path reads the row rather than the GPU showing it unmirrored. */
  if (mapping.mirror_axes != 0) {
    return gpu_refuse("a projected row with a Mirror has no GPU step yet");
  }
  const Object *object = projection.object;
  const Material *material = projection.material;
  if (object == nullptr || material == nullptr) {
    return gpu_refuse("a projected row has no painted object");
  }
  /* The canvas the stack is described at (#composite_channel_describe): the map is cut at it, and
   * a screen texel reads the map texel under its canvas UV. */
  const int canvas = material->paint_layers_image_size > 0 ? int(material->paint_layers_image_size) :
                                                             4096;
  const uint32_t object_uid = object->id.session_uid;
  const uint32_t material_uid = material->id.session_uid;
  /* The evicted entries of the previous call are let go of here: the passes between the two calls
   * have had their dispatch, so the parked textures are read by no one any longer. */
  gpu_surface_cache_pending_free();
  int entry_index = -1;
  for (const int i : gpu_surface_cache.entries.index_range()) {
    const GpuSurfaceMapEntry &entry = gpu_surface_cache.entries[i];
    if (entry.object_uid == object_uid && entry.material_uid == material_uid &&
        entry.tile_number == projection.tile_number && entry.canvas == canvas)
    {
      entry_index = i;
      break;
    }
  }
  const bool same_target = entry_index != -1;
  /* During a stroke the mesh moves every dab (sculpt) and the map would be rebuilt at each: the
   * last upload stands in until the stroke is over, and the first frame after it rebuilds. */
  const bool frozen = same_target && BKE_paint_layers_bake_stroke_active_get();
  if (frozen) {
    /* Still the youngest: the stroke keeps standing on this entry. */
    gpu_surface_cache.touch(entry_index);
    entry_index = gpu_surface_cache.entries.size() - 1;
  }
  else {
    bke::paint_perf::ScopedTimer perf_map("proj.gpu.surface_map");
    /* The texels are asked for only when they are about to be uploaded: a cache entry whose texels
     * went to the GPU earlier is released, and a hit on it needs nothing but its identity. */
    std::shared_ptr<const PaintMaterialSurfaceMap> map = BKE_paint_material_surface_map_get(
        object, material, canvas, canvas, projection.tile_number, /*need_texels=*/false);
    if (map != nullptr && map->texels_released &&
        (!same_target || map != gpu_surface_cache.entries[entry_index].map))
    {
      /* The GPU copy of this map is gone (the budget evicted its entry): rebuild with texels. */
      map = BKE_paint_material_surface_map_get(
          object, material, canvas, canvas, projection.tile_number, /*need_texels=*/true);
    }
    if (map == nullptr && !same_target) {
      return gpu_refuse("a projected row's object has no surface map");
    }
    size_t evicted_bytes = 0;
    if (map == nullptr) {
      /* The evaluated mesh can be missing for a moment after a mode switch or an edit: the previous
       * map stands in rather than a 500 ms CPU composite, and the next frame rebuilds. */
      perf_map.note("kept previous map canvas=%d", canvas);
    }
    else if (same_target && map == gpu_surface_cache.entries[entry_index].map) {
      /* The textures are this map's already: the entry is only made the youngest one. */
      gpu_surface_cache.touch(entry_index);
      entry_index = gpu_surface_cache.entries.size() - 1;
      perf_map.note("reused canvas=%d", canvas);
    }
    else if (same_target) {
      /* The map was rebuilt since the textures were made (the mesh moved outside a stroke): the
       * entry keeps its place in the store, its textures are remade from the new map. */
      GpuSurfaceMapEntry entry = gpu_surface_cache.take(entry_index);
      /* The old textures are parked, not freed: a pass of this frame may still read them. */
      GpuSurfaceMapEntry stale;
      stale.position = entry.position;
      stale.normal = entry.normal;
      stale.tri_basis = entry.tri_basis;
      gpu_surface_cache.pending_free.append(std::move(stale));
      entry.position = nullptr;
      entry.normal = nullptr;
      entry.tri_basis = nullptr;
      entry.map.reset();
      /* Both textures are 32-bit float. The position keeps full precision: it is divided by the
       * frame size and a texel of error is a visible shift. The normal is the interpolated, not
       * normalized, corner normal the CPU bake reads, and the shader's facing fade and Box weights
       * depend on its length. Normalizing it or packing it into a narrower (snorm or half float)
       * format would change those results, and 16-bit snorm texture usage as a compute-shader
       * image is not guaranteed on every driver, so it is stored unmodified. */
      entry.position = gpu_surface_texture_create(
          "combined_surface_position",
          canvas,
          map->position,
          gpu::TextureFormat::SFLOAT_32_32_32_32);
      entry.normal = gpu_surface_texture_create(
          "combined_surface_normal", canvas, map->normal, gpu::TextureFormat::SFLOAT_32_32_32_32);
      /* The normal's `w` is a triangle id into this table (the tangent basis of a projected Normal
       * row): one 32-byte pair per triangle, instead of a basis per texel. */
      entry.tri_basis = gpu_surface_tri_basis_texture_create(map->tri_basis);
      if (entry.position == nullptr || entry.normal == nullptr || entry.tri_basis == nullptr) {
        gpu_surface_entry_release(entry);
        return gpu_refuse("the surface map textures could not be allocated");
      }
      /* The table's size follows the mesh, so the entry's bytes are counted again. */
      entry.bytes = gpu_surface_map_bytes(*map, canvas);
      /* The upload is read by the dispatch that follows. */
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
      entry.map = map;
      /* The textures hold the texels now: the CPU copy (0.5 GiB at 4K) is no longer read. */
      BKE_paint_material_surface_map_texels_release(map);
      /* The entry is made the youngest one. */
      gpu_surface_cache.append(std::move(entry));
      entry_index = gpu_surface_cache.entries.size() - 1;
      perf_map.note("uploaded canvas=%d", canvas);
    }
    else {
      /* A target the store does not hold: make room from the front, parking the evicted entries
       * for the next call to free, and upload the new one. */
      const size_t bytes = gpu_surface_map_bytes(*map, canvas);
      gpu_surface_cache.evict_for(bytes, GPU_SURFACE_CACHE_BYTES, &evicted_bytes);
      GpuSurfaceMapEntry entry;
      entry.map = map;
      entry.object_uid = object_uid;
      entry.material_uid = material_uid;
      entry.tile_number = projection.tile_number;
      entry.canvas = canvas;
      entry.bytes = bytes;
      /* Both textures are 32-bit float. The position keeps full precision: it is divided by the
       * frame size and a texel of error is a visible shift. The normal is the interpolated, not
       * normalized, corner normal the CPU bake reads, and the shader's facing fade and Box weights
       * depend on its length. Normalizing it or packing it into a narrower (snorm or half float)
       * format would change those results, and 16-bit snorm texture usage as a compute-shader
       * image is not guaranteed on every driver, so it is stored unmodified. */
      entry.position = gpu_surface_texture_create(
          "combined_surface_position",
          canvas,
          map->position,
          gpu::TextureFormat::SFLOAT_32_32_32_32);
      entry.normal = gpu_surface_texture_create(
          "combined_surface_normal", canvas, map->normal, gpu::TextureFormat::SFLOAT_32_32_32_32);
      entry.tri_basis = gpu_surface_tri_basis_texture_create(map->tri_basis);
      if (entry.position == nullptr || entry.normal == nullptr || entry.tri_basis == nullptr) {
        gpu_surface_entry_release(entry);
        return gpu_refuse("the surface map textures could not be allocated");
      }
      /* The upload is read by the dispatch that follows. */
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
      /* The textures hold the texels now: the CPU copy (0.5 GiB at 4K) is no longer read. */
      BKE_paint_material_surface_map_texels_release(map);
      gpu_surface_cache.append(std::move(entry));
      entry_index = gpu_surface_cache.entries.size() - 1;
      if (evicted_bytes == 0) {
        perf_map.note("uploaded canvas=%d", canvas);
      }
      else {
        perf_map.note("uploaded canvas=%d evicted=%u MiB", canvas, uint32_t(evicted_bytes >> 20));
      }
    }
  }
  const GpuSurfaceMapEntry &entry = gpu_surface_cache.entries[entry_index];
  r_projection.position = entry.position;
  r_projection.normal = entry.normal;
  r_projection.tri_basis = entry.tri_basis;
  r_projection.canvas_size = int2(canvas);

  /* The frame, as `composite_projection_position` and the bake's texel callback read it. */
  float frame_rot[3][3];
  eul_to_mat3(frame_rot, projection.rotation);
  float inv_rot[3][3];
  transpose_m3_m3(inv_rot, frame_rot);

  /* The painted mesh is in its own space; a controller Empty moves the frame into its own, the
   * point by the two matrices and the normal by their inverse transposes (`composite_projection_raster`). */
  float4x4 p_to_controller = float4x4::identity();
  float3x3 n_to_controller = float3x3::identity();
  /* A Decal lives in the painted object's space: the controller is left out of its frame, as
   * #composite_resample_projected leaves it out. */
  const bool is_decal = projection.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL;
  if (!is_decal && projection.controller != nullptr && projection.controller != object) {
    const float4x4 object_to_world = object->object_to_world();
    const float4x4 world_to_controller = projection.controller->world_to_object();
    p_to_controller = world_to_controller * object_to_world;
    n_to_controller = math::transpose(math::invert(float3x3(world_to_controller))) *
                      math::transpose(math::invert(float3x3(object_to_world)));
  }

  /* `q = S R^-1 (p - center)`: S folds the half extent and the image-aspect fit, which the CPU
   * applies in two steps. */
  float4x4 rotation = float4x4::identity();
  float3x3 rotation_3x3 = float3x3::identity();
  for (const int column : IndexRange(3)) {
    for (const int row : IndexRange(3)) {
      rotation[column][row] = inv_rot[column][row];
      rotation_3x3[column][row] = inv_rot[column][row];
    }
  }
  float4x4 translation = float4x4::identity();
  translation[3] = float4(-projection.center[0], -projection.center[1], -projection.center[2], 1.0f);
  float4x4 scale = float4x4::identity();
  for (const int i : IndexRange(3)) {
    const float half_size = BKE_paint_layers_projection_size_normalize(projection.size[i]) * 0.5f;
    scale[i][i] = projection.aspect[i] / half_size;
  }
  r_projection.p_to_q = scale * rotation * translation * p_to_controller;
  if (is_decal) {
    /* The "position" texture holds the unfolded `q` already (over the half extent): the point
     * matrix only places it on the map's U and V axes and applies the image-aspect fit, the depth
     * axis staying zero, as the CPU's Decal texel does. The triangle id in `z` is dropped. */
    const GpuDecalMapEntry *decal = gpu_decal_entry_get(projection, canvas);
    if (decal == nullptr) {
      return false;
    }
    const int axis_index = clamp_i(
        projection.axis, MA_PAINT_LAYER_PROJECTION_AXIS_X, MA_PAINT_LAYER_PROJECTION_AXIS_Z);
    const int u_index = (axis_index == 0) ? 1 : 0;
    const int v_index = (axis_index == 2) ? 1 : 2;
    r_projection.p_to_q = float4x4::zero();
    r_projection.p_to_q[0][u_index] = projection.aspect[u_index];
    r_projection.p_to_q[1][v_index] = projection.aspect[v_index];
    r_projection.p_to_q[3][3] = 1.0f;
    r_projection.position = decal->position;
    r_projection.decal_frames = decal->frames;
  }

  const float3x3 n_to_frame = rotation_3x3 * n_to_controller;
  r_projection.n_to_frame = float4x4::identity();
  for (const int column : IndexRange(3)) {
    r_projection.n_to_frame[column] = float4(n_to_frame[column], 0.0f);
  }
  /* The two halves of that normal matrix the tangent-space Normal conversion needs apart: the
   * frame's rotation, and the object-to-controller normal matrix its triangle basis and surface
   * normal move through (identity without a controller, as the CPU skips it). */
  /* The frame's rotation itself, not the `R^-1` that `rotation` holds for the point: the Normal
   * conversion carries a vector out of the frame, like the CPU helper (`R * n_frame`). */
  r_projection.frame_rot = float4x4::identity();
  for (const int column : IndexRange(3)) {
    for (const int row : IndexRange(3)) {
      r_projection.frame_rot[column][row] = frame_rot[column][row];
    }
  }
  r_projection.n_to_controller = float4x4::identity();
  for (const int column : IndexRange(3)) {
    r_projection.n_to_controller[column] = float4(n_to_controller[column], 0.0f);
  }

  const ProjectionModeInfo &mode = BKE_paint_layers_projection_mode_info(projection.space);
  if (projection.space == MA_PAINT_LAYER_MAPPING_SPACE_CUBE) {
    r_projection.mode = gpu_proj_cube;
  }
  else if (projection.space == MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER) {
    r_projection.mode = gpu_proj_cylinder;
  }
  else if (projection.space == MA_PAINT_LAYER_MAPPING_SPACE_SPHERE) {
    r_projection.mode = gpu_proj_sphere;
  }
  else if (mode.samples_num > 1) {
    r_projection.mode = gpu_proj_box;
  }
  else {
    r_projection.mode = gpu_proj_planar;
  }
  r_projection.axis = clamp_i(
      projection.axis, MA_PAINT_LAYER_PROJECTION_AXIS_X, MA_PAINT_LAYER_PROJECTION_AXIS_Z);
  r_projection.flags = (projection.clip ? gpu_proj_clip : 0) |
                       (!is_decal && projection.depth ? gpu_proj_depth : 0) |
                       (!is_decal && projection.facing ? gpu_proj_facing : 0) |
                       (is_decal ? gpu_proj_decal : 0) |
                       (mapping.enabled ? gpu_proj_uv_adjust : 0) |
                       (mapping.wrap == MA_PAINT_LAYER_MAPPING_WRAP_CLIP ? gpu_proj_wrap_clip : 0);
  r_projection.params_0 = float4(
      projection.tiling[0], projection.tiling[1], projection.facing_scale, projection.facing_offset);
  const float exponent = mode.uses_blend ?
                             BKE_paint_layers_projection_triplanar_exponent(projection.blend) :
                             PAINT_LAYER_PROJECTION_BOX_EXPONENT;
  r_projection.params_1 = float4(
      exponent, PAINT_LAYER_PROJECTION_FRAME_EXTENT, PAINT_LAYER_PROJECTION_EDGE_SOFT, 0.0f);
  const ProjectionCubeLayoutInfo &atlas = BKE_paint_layers_projection_cube_layout_info(
      projection.cube_layout);
  r_projection.atlas_0 = float4(
      atlas.origin[0][0], atlas.origin[0][1], atlas.origin[1][0], atlas.origin[1][1]);
  r_projection.atlas_1 = float4(
      atlas.origin[2][0], atlas.origin[2][1], atlas.origin[3][0], atlas.origin[3][1]);
  r_projection.atlas_2 = float4(
      atlas.origin[4][0], atlas.origin[4][1], atlas.origin[5][0], atlas.origin[5][1]);
  r_projection.atlas_cell = float2(atlas.cell_size[0], atlas.cell_size[1]);
  return true;
}

/**
 * Read a correction's or mask item's map through \a mapping's projection instead of the canvas UV,
 * as the row's map is (#composite_mapped_remap is the CPU twin for all three): the frame and the
 * surface map into \a r_frame, the projected bit into \a r_flags. The UV adjust stays in the
 * mapping uniforms (the shader applies it per tap); the fold, repeat axes and tangent-space flip
 * belong to the plain mapping, which a projection replaces. False, with the reason reported, when
 * the frame cannot be laid.
 */
static bool gpu_projection_apply(const PaintMaterialCompositeMapping &mapping,
                                 int &r_flags,
                                 GpuProjection &r_frame,
                                 const char *what)
{
  bke::paint_perf::ScopedTimer perf("proj.gpu.frame");
  if (!combined_projection_gpu_make(mapping.projection, mapping, r_frame)) {
    return false;
  }
  if (mapping.projection.source_tiled) {
    /* A tiled source's GPU texture is not one tile the projected UV could wrap in. */
    return gpu_refuse("a projected map is tiled, which the GPU path does not lay");
  }
  r_flags |= gpu_layer_projected;
  r_flags &= ~(gpu_layer_mapped | gpu_layer_repeat_u | gpu_layer_repeat_v |
               gpu_layer_tangent_normal | gpu_layer_mirror_u | gpu_layer_mirror_v |
               gpu_layer_mirror_u_neg | gpu_layer_mirror_v_neg);
  perf.note("%s", what);
  return true;
}

/** Upload \a frame to the mask or correction pass; a pass that is not projected gets the identity. */
static void gpu_projection_uniforms_set(gpu::Shader *shader, const GpuProjection &frame)
{
  GPU_shader_uniform_mat4(shader, "proj_p_to_q", frame.p_to_q.ptr());
  GPU_shader_uniform_mat4(shader, "proj_n_to_frame", frame.n_to_frame.ptr());
  GPU_shader_uniform_4fv(shader, "proj_params_0", &frame.params_0.x);
  GPU_shader_uniform_4fv(shader, "proj_params_1", &frame.params_1.x);
  GPU_shader_uniform_4fv(shader, "proj_atlas_0", &frame.atlas_0.x);
  GPU_shader_uniform_4fv(shader, "proj_atlas_1", &frame.atlas_1.x);
  GPU_shader_uniform_4fv(shader, "proj_atlas_2", &frame.atlas_2.x);
  GPU_shader_uniform_2fv(shader, "proj_atlas_cell", &frame.atlas_cell.x);
  GPU_shader_uniform_2iv(shader, "proj_canvas_size", &frame.canvas_size.x);
  GPU_shader_uniform_1i(shader, "proj_mode", frame.mode);
  GPU_shader_uniform_1i(shader, "proj_axis", frame.axis);
  GPU_shader_uniform_1i(shader, "proj_flags", frame.flags);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name CombinedPasses
 * \{ */

CombinedPasses::CombinedPasses(gpu::TexturePool &pool,
                               gpu::Texture *dummy,
                               gpu::Texture *slot_dummy,
                               gpu::Shader *layer_shader,
                               gpu::Shader *mask_shader,
                               gpu::Shader *correction_shader,
                               gpu::Shader *folder_shader,
                               gpu::Shader *effect_shader,
                               Vector<gpu::Texture *> &filter_luts,
                               Vector<GpuSliceSlot> &slice_slots,
                               Vector<GpuMaskSlot> &mask_slots,
                               Vector<GpuEffectResult> &effect_results,
                               eGPUTextureUsage accum_usage)
    : pool(pool),
      dummy(dummy),
      slot_dummy(slot_dummy),
      layer_shader(layer_shader),
      mask_shader(mask_shader),
      correction_shader(correction_shader),
      folder_shader(folder_shader),
      effect_shader(effect_shader),
      filter_luts(filter_luts),
      slice_slots(slice_slots),
      mask_slots(mask_slots),
      effect_results(effect_results),
      accum_usage(accum_usage)
{
}

bool CombinedPasses::color_ramp_is_constant(const ColorBand &band)
{
  return band.color_mode == COLBAND_BLEND_RGB && band.ipotype == COLBAND_INTERP_CONSTANT;
}

gpu::Texture *CombinedPasses::filter_lut_acquire(const int filter_kind,
                                                 const ColorBand *ramp,
                                                 const CurveMapping *curves)
{
  Vector<float4> texels;
  int width = 0;
  int height = 0;
  if (filter_kind == MA_PAINT_LAYER_FILTER_COLOR_RAMP) {
    if (ramp == nullptr || ramp->tot == 0) {
      return nullptr;
    }
    paint_layer_color_ramp_lut_build(*ramp, texels, width);
    height = 1;
  }
  else if (filter_kind == MA_PAINT_LAYER_FILTER_CURVES) {
    if (curves == nullptr) {
      return nullptr;
    }
    paint_layer_curves_lut_build(*curves, texels, width, height);
  }
  else {
    return dummy;
  }
  gpu::Texture *lut = pool.acquire_texture_2d(
      int2(width, height),
      1,
      gpu::TextureFormat::SFLOAT_32_32_32_32,
      eGPUTextureUsage(GPU_TEXTURE_USAGE_SHADER_READ),
      "combined_filter_lut");
  if (lut == nullptr) {
    return nullptr;
  }
  GPU_texture_update(lut, GPU_DATA_FLOAT, texels.data());
  /* The upload is read by the dispatch that follows; the tests upload-and-read the same way. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  filter_luts.append(lut);
  return lut;
}

float CombinedPasses::map_lod(CombinedTarget &t,
                              gpu::Texture *map,
                              const int flags,
                              const float2 &mapping_scale)
{
  if (map == nullptr || (flags & gpu_layer_alpha_coverage) != 0 || (flags & gpu_layer_slot) != 0) {
    /* A painted map stores straight colour with black where it is transparent, and its mip
     * levels average that black in: a stroke edge seen from afar would come out several times
     * darker than the CPU composite reads it. The base level has no such bleed. A slice slot is
     * screen-space and has no mip levels at all. */
    return 0.0f;
  }
  const float2 map_size(GPU_texture_width(map), GPU_texture_height(map));
  const float3x3 &to_uv = t.texel_to_uv;
  float texels_per_pixel = std::max(
      math::length(float2(to_uv[0].x, to_uv[0].y) * map_size),
      math::length(float2(to_uv[1].x, to_uv[1].y) * map_size));
  if ((flags & gpu_layer_mapped) != 0) {
    /* A scaled mapping reads the map denser or sparser than the canvas does. */
    texels_per_pixel *= std::max(std::abs(mapping_scale.x), std::abs(mapping_scale.y));
  }
  return std::max(0.0f, std::log2(std::max(texels_per_pixel, 1e-6f)));
}

bool CombinedPasses::lay(CombinedTarget &t,
                         gpu::Texture *dest,
                         gpu::Texture *map,
                         const int flags,
                         const float4 &layer_color,
                         const float4 &init_color,
                         const float opacity,
                         const GpuMappingUniforms &mapping,
                         const GpuInputSteps &input_steps,
                         const int blend,
                         gpu::Texture *mask,
                         const int filter_kind,
                         const float *filter_params,
                         const ColorBand *filter_ramp,
                         const CurveMapping *filter_curves,
                         gpu::Texture *slot_coverage,
                         const GpuProjection *projection)
{
  /* A row that is not projected still uploads the frame values (identity) and binds the
   * placeholder, so the shared pass never reads stale ones. */
  const GpuProjection no_projection;
  const GpuProjection &frame = projection != nullptr ? *projection : no_projection;
  gpu::Texture *proj_position = frame.position != nullptr ? frame.position : dummy;
  gpu::Texture *proj_normal = frame.normal != nullptr ? frame.normal : dummy;
  gpu::Texture *proj_tri_basis = frame.tri_basis != nullptr ? frame.tri_basis : dummy;
  gpu::Texture *proj_decal_frames = frame.decal_frames != nullptr ? frame.decal_frames : dummy;
  gpu::Texture *sampled = map != nullptr ? map : dummy;
  gpu::Texture *mask_sampled = mask != nullptr ? mask : dummy;
  gpu::Texture *slot_cov_sampled = slot_coverage != nullptr ? slot_coverage : dummy;
  const float lod = map_lod(t, map, flags, mapping.scale);
  GPU_shader_bind(layer_shader);
  GPU_shader_uniform_mat4(layer_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(layer_shader, "texture_size", &t.size.x);
  GPU_shader_uniform_4fv(layer_shader, "layer_color", &layer_color.x);
  GPU_shader_uniform_4fv(layer_shader, "init_color", &init_color.x);
  gpu_mapping_uniforms_set(layer_shader, mapping);
  GPU_shader_uniform_mat4(layer_shader, "proj_p_to_q", frame.p_to_q.ptr());
  GPU_shader_uniform_mat4(layer_shader, "proj_n_to_frame", frame.n_to_frame.ptr());
  GPU_shader_uniform_mat4(layer_shader, "proj_frame_rot", frame.frame_rot.ptr());
  GPU_shader_uniform_mat4(layer_shader, "proj_n_to_controller", frame.n_to_controller.ptr());
  GPU_shader_uniform_4fv(layer_shader, "proj_params_0", &frame.params_0.x);
  GPU_shader_uniform_4fv(layer_shader, "proj_params_1", &frame.params_1.x);
  GPU_shader_uniform_4fv(layer_shader, "proj_atlas_0", &frame.atlas_0.x);
  GPU_shader_uniform_4fv(layer_shader, "proj_atlas_1", &frame.atlas_1.x);
  GPU_shader_uniform_4fv(layer_shader, "proj_atlas_2", &frame.atlas_2.x);
  GPU_shader_uniform_2fv(layer_shader, "proj_atlas_cell", &frame.atlas_cell.x);
  GPU_shader_uniform_2iv(layer_shader, "proj_canvas_size", &frame.canvas_size.x);
  GPU_shader_uniform_1i(layer_shader, "proj_mode", frame.mode);
  GPU_shader_uniform_1i(layer_shader, "proj_axis", frame.axis);
  GPU_shader_uniform_1i(layer_shader, "proj_flags", frame.flags);
  gpu_input_steps_uniforms_set(layer_shader, input_steps);
  GPU_shader_uniform_1f(layer_shader, "opacity", opacity);
  GPU_shader_uniform_1f(layer_shader, "lod", lod);
  /* The scene luminance the F0 map encoding reads a specular colour by: the coefficients the RGB
   * to BW nodes and the CPU helper use, with `w` carrying the F0 a level of one is pre-divided, so
   * the layer pass multiplies by one value. */
  float luma_coefficients[3];
  IMB_colormanagement_get_luminance_coefficients(luma_coefficients);
  const float4 map_luma(luma_coefficients[0],
                        luma_coefficients[1],
                        luma_coefficients[2],
                        1.0f / SPECULAR_LEVEL_F0_FULL);
  GPU_shader_uniform_4fv(layer_shader, "map_luma", &map_luma.x);
  GPU_shader_uniform_1i(layer_shader, "flags", flags);
  GPU_shader_uniform_1i(layer_shader, "blend", blend);
  GPU_shader_uniform_1i(layer_shader, "filter_kind", filter_kind);
  const float4 adjustment_0 = filter_params != nullptr ?
                                  float4(filter_params[0], filter_params[1], filter_params[2], filter_params[3]) :
                                  float4(0.0f);
  float4 adjustment_1 = filter_params != nullptr ?
                            float4(filter_params[4], filter_params[5], filter_params[6], filter_params[7]) :
                            float4(0.0f);
  /* A Color Balance's exponent triple; zero reads as a disabled filter's unused parameter. */
  float4 adjustment_2 = filter_params != nullptr ?
                            float4(filter_params[8],
                                   filter_params[9],
                                   filter_params[10],
                                   filter_params[11]) :
                            float4(0.0f);
  if (filter_kind == MA_PAINT_LAYER_FILTER_COLOR_RAMP && filter_ramp != nullptr) {
    /* The band's CONSTANT interpolation, which the shader reads from `filter_params_1.x`. */
    adjustment_1.x = color_ramp_is_constant(*filter_ramp) ? 1.0f : 0.0f;
  }
  /* The shader applies a layer filter only on an adjustment that is not reading a row already
   * filtered: the row-use pass would otherwise build a table it never samples. */
  const bool applies_filter = (flags & gpu_layer_adjustment) != 0 &&
                              (flags & gpu_layer_row_use) == 0;
  gpu::Texture *filter_lut = applies_filter ?
                                 filter_lut_acquire(filter_kind, filter_ramp, filter_curves) :
                                 dummy;
  if (filter_lut == nullptr) {
    return gpu_refuse("a filter LUT could not be built");
  }
  GPU_shader_uniform_4fv(layer_shader, "filter_params_0", &adjustment_0.x);
  GPU_shader_uniform_4fv(layer_shader, "filter_params_1", &adjustment_1.x);
  GPU_shader_uniform_4fv(layer_shader, "filter_params_2", &adjustment_2.x);

  /* The previous pass wrote this accumulator through an image. */
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(layer_shader, "layer_tx"));
  GPU_texture_bind(mask_sampled, GPU_shader_get_sampler_binding(layer_shader, "mask_tx"));
  GPU_texture_bind(slot_cov_sampled,
                   GPU_shader_get_sampler_binding(layer_shader, "slot_cov_tx"));
  GPU_texture_bind(filter_lut, GPU_shader_get_sampler_binding(layer_shader, "filter_lut_tx"));
  GPU_texture_bind(proj_position, GPU_shader_get_sampler_binding(layer_shader, "proj_pos_tx"));
  GPU_texture_bind(proj_normal, GPU_shader_get_sampler_binding(layer_shader, "proj_nrm_tx"));
  GPU_texture_bind(proj_tri_basis, GPU_shader_get_sampler_binding(layer_shader, "proj_tri_tx"));
  GPU_texture_bind(proj_decal_frames,
                   GPU_shader_get_sampler_binding(layer_shader, "proj_decal_tx"));
  GPU_texture_image_bind(dest, GPU_shader_get_sampler_binding(layer_shader, "accum_img"));
  GPU_texture_image_bind(t.row_color,
                         GPU_shader_get_sampler_binding(layer_shader, "row_color_img"));
  GPU_texture_image_bind(t.row_factor,
                         GPU_shader_get_sampler_binding(layer_shader, "row_factor_img"));
  GPU_compute_dispatch(layer_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(t.row_factor);
  GPU_texture_image_unbind(t.row_color);
  GPU_texture_image_unbind(dest);
  GPU_texture_unbind(proj_decal_frames);
  GPU_texture_unbind(proj_tri_basis);
  GPU_texture_unbind(proj_normal);
  GPU_texture_unbind(proj_position);
  GPU_texture_unbind(filter_lut);
  GPU_texture_unbind(slot_cov_sampled);
  GPU_texture_unbind(mask_sampled);
  GPU_texture_unbind(sampled);
  dispatches++;
  return true;
}

bool CombinedPasses::mask_step(CombinedTarget &t,
                               const int mode,
                               gpu::Texture *map,
                               const int flags,
                               const float constant_gray,
                               const float opacity,
                               const float influence,
                               const int blend,
                               const int filter,
                               const float *filter_params,
                               const ColorBand *filter_ramp,
                               const CurveMapping *filter_curves,
                               const PaintMaterialCompositeMapping *mapping,
                               gpu::Texture *slot_dst,
                               gpu::Texture *mask_source,
                               const float contrast)
{
  gpu::Texture *sampled = map != nullptr ? map : dummy;
  int step_flags = flags;
  GpuMappingUniforms mapping_uniforms;
  GpuProjection projection_frame;
  if (mapping != nullptr && map != nullptr) {
    gpu_mapping_setup(*mapping, step_flags, mapping_uniforms);
    if (mapping->projection.present &&
        !gpu_projection_apply(*mapping, step_flags, projection_frame, "mask"))
    {
      return false;
    }
  }
  float4 params_0(0.0f);
  float4 params_1(0.0f);
  /* A Color Balance's exponent triple; zero reads as a disabled filter's unused parameter. */
  float4 params_2(0.0f);
  if (filter_params != nullptr) {
    params_0 = float4(filter_params[0], filter_params[1], filter_params[2], filter_params[3]);
    params_1 = float4(filter_params[4], filter_params[5], filter_params[6], filter_params[7]);
    params_2 = float4(filter_params[8], filter_params[9], filter_params[10], filter_params[11]);
  }
  if (filter == MA_PAINT_LAYER_FILTER_COLOR_RAMP && filter_ramp != nullptr) {
    params_1.x = color_ramp_is_constant(*filter_ramp) ? 1.0f : 0.0f;
  }
  gpu::Texture *filter_lut = filter_lut_acquire(filter, filter_ramp, filter_curves);
  if (filter_lut == nullptr) {
    return gpu_refuse("a filter LUT could not be built");
  }
  /* A mask map is painted like a layer's: its base level, for the same reason. */
  const float lod = 0.0f;
  GPU_shader_bind(mask_shader);
  GPU_shader_uniform_mat4(mask_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(mask_shader, "texture_size", &t.size.x);
  gpu_mapping_uniforms_set(mask_shader, mapping_uniforms);
  gpu_projection_uniforms_set(mask_shader, projection_frame);
  gpu_input_steps_uniforms_set(mask_shader, gpu_input_steps_none);
  GPU_shader_uniform_4fv(mask_shader, "filter_params_0", &params_0.x);
  GPU_shader_uniform_4fv(mask_shader, "filter_params_1", &params_1.x);
  GPU_shader_uniform_4fv(mask_shader, "filter_params_2", &params_2.x);
  GPU_shader_uniform_1f(mask_shader, "constant_gray", constant_gray);
  GPU_shader_uniform_1f(mask_shader, "opacity", opacity);
  GPU_shader_uniform_1f(mask_shader, "contrast", contrast);
  GPU_shader_uniform_1f(mask_shader, "influence", influence);
  GPU_shader_uniform_1f(mask_shader, "lod", lod);
  GPU_shader_uniform_1i(mask_shader, "flags", step_flags);
  GPU_shader_uniform_1i(mask_shader, "mode", mode);
  GPU_shader_uniform_1i(mask_shader, "blend", blend);
  GPU_shader_uniform_1i(mask_shader, "filter_kind", filter);

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  gpu::Texture *mask_target = mask_source != nullptr ? mask_source : t.mask_accum;
  GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(mask_shader, "mask_tx"));
  GPU_texture_bind(filter_lut, GPU_shader_get_sampler_binding(mask_shader, "filter_lut_tx"));
  gpu::Texture *proj_position = projection_frame.position != nullptr ? projection_frame.position : dummy;
  gpu::Texture *proj_normal = projection_frame.normal != nullptr ? projection_frame.normal : dummy;
  GPU_texture_bind(proj_position, GPU_shader_get_sampler_binding(mask_shader, "proj_pos_tx"));
  GPU_texture_bind(proj_normal, GPU_shader_get_sampler_binding(mask_shader, "proj_nrm_tx"));
  GPU_texture_image_bind(mask_target, GPU_shader_get_sampler_binding(mask_shader, "mask_img"));
  GPU_texture_image_bind(slot_dst != nullptr ? slot_dst : slot_dummy,
                         GPU_shader_get_sampler_binding(mask_shader, "slot_img"));
  GPU_compute_dispatch(mask_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(slot_dst != nullptr ? slot_dst : slot_dummy);
  GPU_texture_image_unbind(mask_target);
  GPU_texture_unbind(proj_normal);
  GPU_texture_unbind(proj_position);
  GPU_texture_unbind(filter_lut);
  GPU_texture_unbind(sampled);
  dispatches++;
  return true;
}

bool CombinedPasses::correction_step(CombinedTarget &t,
                                     const PaintMaterialCompositeCorrection &correction,
                                     const bool tracks_alpha,
                                     gpu::Texture *slot_color,
                                     gpu::Texture *slot_coverage,
                                     gpu::Texture *effect_result)
{
  gpu::Texture *map = nullptr;
  int step_flags = tracks_alpha ? gpu_layer_tracks_alpha : 0;
  if (effect_result != nullptr) {
    /* A CPU-computed effect the GPU computed: the result texture is the same buffer the CPU cache
     * hands the evaluator, at the same size and in the same texel space, so the shader's effect
     * branch stands in for the correction's map and for its filter at once. */
    map = effect_result;
    step_flags |= gpu_effect_result;
  }
  else if (slot_color != nullptr) {
    /* A live Anchor Point slice: screen-space, no mapping, coverage in the paired slot. */
    map = slot_color;
    step_flags |= gpu_layer_slot;
  }
  else if (correction.filter == 0 && correction.image != nullptr) {
    bool premultiplied = false;
    map = gpu_map_texture(correction.image, correction.iuser, premultiplied);
    if (map == nullptr) {
      return false;
    }
    step_flags |= gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
  }
  GpuMappingUniforms mapping_uniforms;
  GpuProjection projection_frame;
  if (map != nullptr && slot_color == nullptr && effect_result == nullptr) {
    gpu_mapping_setup(correction.mapping, step_flags, mapping_uniforms);
    /* A mesh map is never projected: the CPU reads it through the mesh, not the frame. */
    if (correction.mapping.projection.present && !correction.is_mesh_map &&
        !gpu_projection_apply(correction.mapping, step_flags, projection_frame, "correction"))
    {
      return false;
    }
  }
  gpu::Texture *sampled = (map != nullptr && effect_result == nullptr) ? map : dummy;
  gpu::Texture *effect_sampled = effect_result != nullptr ? effect_result : dummy;
  gpu::Texture *slot_sampled = slot_coverage != nullptr ? slot_coverage : dummy;
  const float4 constant_color(correction.constant_color);
  const float4 params_0(correction.filter_params[0],
                        correction.filter_params[1],
                        correction.filter_params[2],
                        correction.filter_params[3]);
  float4 params_1(correction.filter_params[4],
                  correction.filter_params[5],
                  correction.filter_params[6],
                  correction.filter_params[7]);
  /* A Color Balance's exponent triple; zero reads as a disabled filter's unused parameter. */
  const float4 params_2(correction.filter_params[8],
                        correction.filter_params[9],
                        correction.filter_params[10],
                        correction.filter_params[11]);
  if (correction.filter == MA_PAINT_LAYER_FILTER_COLOR_RAMP && correction.filter_ramp != nullptr)
  {
    params_1.x = color_ramp_is_constant(*correction.filter_ramp) ? 1.0f : 0.0f;
  }
  gpu::Texture *filter_lut = filter_lut_acquire(
      int(correction.filter), correction.filter_ramp, correction.filter_curves);
  if (filter_lut == nullptr) {
    return gpu_refuse("a filter LUT could not be built");
  }
  /* A correction map is painted like a layer's: its base level, for the same reason. */
  const float lod = 0.0f;
  GPU_shader_bind(correction_shader);
  GPU_shader_uniform_mat4(correction_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(correction_shader, "texture_size", &t.size.x);
  GPU_shader_uniform_4fv(correction_shader, "constant_color", &constant_color.x);
  gpu_mapping_uniforms_set(correction_shader, mapping_uniforms);
  gpu_projection_uniforms_set(correction_shader, projection_frame);
  gpu_input_steps_uniforms_set(correction_shader, gpu_input_steps_none);
  GPU_shader_uniform_4fv(correction_shader, "filter_params_0", &params_0.x);
  GPU_shader_uniform_4fv(correction_shader, "filter_params_1", &params_1.x);
  GPU_shader_uniform_4fv(correction_shader, "filter_params_2", &params_2.x);
  GPU_shader_uniform_1f(correction_shader, "opacity", correction.opacity);
  GPU_shader_uniform_1f(correction_shader, "lod", lod);
  GPU_shader_uniform_1i(correction_shader, "flags", step_flags);
  GPU_shader_uniform_1i(correction_shader, "blend", int(correction.blend));
  GPU_shader_uniform_1i(correction_shader, "filter_kind", int(correction.filter));

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(correction_shader, "correction_tx"));
  GPU_texture_bind(effect_sampled,
                   GPU_shader_get_sampler_binding(correction_shader, "effect_tx"));
  GPU_texture_bind(slot_sampled,
                   GPU_shader_get_sampler_binding(correction_shader, "slot_cov_tx"));
  GPU_texture_bind(filter_lut,
                   GPU_shader_get_sampler_binding(correction_shader, "filter_lut_tx"));
  gpu::Texture *proj_position = projection_frame.position != nullptr ? projection_frame.position : dummy;
  gpu::Texture *proj_normal = projection_frame.normal != nullptr ? projection_frame.normal : dummy;
  GPU_texture_bind(proj_position,
                   GPU_shader_get_sampler_binding(correction_shader, "proj_pos_tx"));
  GPU_texture_bind(proj_normal,
                   GPU_shader_get_sampler_binding(correction_shader, "proj_nrm_tx"));
  GPU_texture_image_bind(t.row_color,
                         GPU_shader_get_sampler_binding(correction_shader, "row_color_img"));
  GPU_texture_image_bind(t.row_factor,
                         GPU_shader_get_sampler_binding(correction_shader, "row_factor_img"));
  GPU_compute_dispatch(correction_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(t.row_factor);
  GPU_texture_image_unbind(t.row_color);
  GPU_texture_unbind(proj_normal);
  GPU_texture_unbind(proj_position);
  GPU_texture_unbind(filter_lut);
  GPU_texture_unbind(slot_sampled);
  GPU_texture_unbind(effect_sampled);
  GPU_texture_unbind(sampled);
  dispatches++;
  return true;
}

void CombinedPasses::folder_step(CombinedTarget &t,
                                 const int mode,
                                 const int level,
                                 const int flags,
                                 const float opacity,
                                 const int blend,
                                 gpu::Texture *mask)
{
  gpu::Texture *mask_sampled = mask != nullptr ? mask : dummy;
  GPU_shader_bind(folder_shader);
  GPU_shader_uniform_mat4(folder_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(folder_shader, "texture_size", &t.size.x);
  GPU_shader_uniform_1f(folder_shader, "opacity", opacity);
  GPU_shader_uniform_1i(folder_shader, "flags", flags);
  GPU_shader_uniform_1i(folder_shader, "blend", blend);
  GPU_shader_uniform_1i(folder_shader, "mode", mode);

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(mask_sampled, GPU_shader_get_sampler_binding(folder_shader, "mask_tx"));
  GPU_texture_image_bind(t.row_color, GPU_shader_get_sampler_binding(folder_shader, "row_color_img"));
  GPU_texture_image_bind(t.row_factor,
                         GPU_shader_get_sampler_binding(folder_shader, "row_factor_img"));
  GPU_texture_image_bind(t.folder_premul[level],
                         GPU_shader_get_sampler_binding(folder_shader, "premul_img"));
  GPU_texture_image_bind(t.folder_cover[level],
                         GPU_shader_get_sampler_binding(folder_shader, "cover_img"));
  GPU_compute_dispatch(folder_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(t.folder_cover[level]);
  GPU_texture_image_unbind(t.folder_premul[level]);
  GPU_texture_image_unbind(t.row_factor);
  GPU_texture_image_unbind(t.row_color);
  GPU_texture_unbind(mask_sampled);
  dispatches++;
}

bool CombinedPasses::folder_levels_ensure(CombinedTarget &t, const int level)
{
  if (level < 0 || level >= gpu_folder_levels_max) {
    return false;
  }
  if (t.folder_premul[level] == nullptr) {
    t.folder_premul[level] = pool.acquire_texture_2d(
        t.size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_folder_premul");
  }
  if (t.folder_cover[level] == nullptr) {
    t.folder_cover[level] = pool.acquire_texture_2d(
        t.size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_folder_cover");
  }
  return t.folder_premul[level] != nullptr && t.folder_cover[level] != nullptr;
}

GpuSliceSlot *CombinedPasses::slice_slot_find(const bUUID &point, const int channel)
{
  for (GpuSliceSlot &slot : slice_slots) {
    if (BKE_paint_layers_gpu_slot_key_equal({slot.point, slot.channel}, {point, channel})) {
      return &slot;
    }
  }
  return nullptr;
}

GpuMaskSlot *CombinedPasses::mask_slot_find(const bUUID &point)
{
  for (GpuMaskSlot &slot : mask_slots) {
    if (BLI_uuid_equal(slot.point, point)) {
      return &slot;
    }
  }
  return nullptr;
}

gpu::Texture *CombinedPasses::effect_result_find(const bUUID &marker, const int channel)
{
  for (const GpuEffectResult &result : effect_results) {
    if (BLI_uuid_equal(result.marker, marker) && result.channel == channel) {
      return result.texture;
    }
  }
  return nullptr;
}

bool CombinedPasses::mask_build(CombinedTarget &t,
                                const PaintMaterialCompositeImageLayer &layer,
                                bool &r_has_mask)
{
  r_has_mask = layer.mask_image != nullptr;
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    r_has_mask |= item.opacity > 0.0f;
  }
  if (!r_has_mask) {
    return true;
  }
  if (!mask_step(t,
                 gpu_mask_init,
                 nullptr,
                 0,
                 1.0f,
                 1.0f,
                 1.0f,
                 int(CompositeBlend::Mix),
                 0,
                 nullptr,
                 nullptr,
                 nullptr,
                 nullptr))
  {
    return false;
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    if (item.opacity <= 0.0f) {
      continue;
    }
    int item_flags = item.invert ? gpu_mask_invert : 0;
    if (item.map_coverage != 0) {
      /* The match accumulates over the passes; a missing or unreadable map leaves it at zero, the
       * full restriction the CPU composite gives. */
      if (!mask_step(t,
                     gpu_mask_cov_begin,
                     nullptr,
                     0,
                     1.0f,
                     1.0f,
                     1.0f,
                     int(CompositeBlend::Mix),
                     0,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr))
      {
        return false;
      }
      bool premultiplied = false;
      gpu::Texture *coverage_map = item.coverage_image != nullptr ?
                                       gpu_map_texture(
                                           item.coverage_image, item.coverage_iuser, premultiplied) :
                                       nullptr;
      if (coverage_map != nullptr) {
        const int map_flags = gpu_layer_has_map | gpu_map_nearest |
                              (premultiplied ? gpu_layer_premultiplied : 0);
        if (item.map_coverage == MA_PAINT_LAYER_FILTER_UV_ISLAND) {
          if (!mask_step(t,
                         gpu_mask_cov_scalar,
                         coverage_map,
                         map_flags,
                         1.0f,
                         1.0f,
                         1.0f,
                         int(CompositeBlend::Mix),
                         0,
                         nullptr,
                         nullptr,
                         nullptr,
                         &item.coverage_mapping))
          {
            return false;
          }
        }
        else if (item.map_coverage == MA_PAINT_LAYER_FILTER_COLOR_ID) {
          const float shared_tol = std::max(0.0f, item.coverage_tolerance);
          const float shared_soft = std::max(0.0f, item.coverage_softness);
          for (const PaintMaterialCompositeIdColor &entry : item.coverage_colors) {
            if (!entry.enabled) {
              continue;
            }
            /* Advanced gives each colour its own tolerance and a hard edge. */
            const float params[8] = {entry.color[0],
                                     entry.color[1],
                                     entry.color[2],
                                     entry.own_tolerance ? std::max(0.0f, entry.tolerance) : shared_tol,
                                     entry.own_tolerance ? 0.0f : shared_soft,
                                     0.0f,
                                     0.0f,
                                     0.0f};
            if (!mask_step(t,
                           gpu_mask_cov_color,
                           coverage_map,
                           map_flags,
                           1.0f,
                           1.0f,
                           1.0f,
                           int(CompositeBlend::Mix),
                           0,
                           params,
                           nullptr,
                           nullptr,
                           &item.coverage_mapping))
            {
              return false;
            }
          }
        }
      }
      if (!mask_step(t,
                     gpu_mask_cov_end,
                     nullptr,
                     item_flags,
                     1.0f,
                     item.opacity,
                     item.coverage_output,
                     int(CompositeBlend::Mix),
                     0,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     item.contrast))
      {
        return false;
      }
      continue;
    }
    gpu::Texture *item_map = nullptr;
    const bool live_reader = !BLI_uuid_is_nil(item.anchor_point_reader) &&
                             item.image == nullptr && item.filter == 0;
    if (live_reader) {
      if (item.anchor_point_target == int8_t(PaintLayerAnchorPointTarget::Mask)) {
        GpuMaskSlot *slot = mask_slot_find(item.anchor_point_point);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point mask slice has no captured slot");
        }
        item_map = slot->texture;
      }
      else {
        GpuSliceSlot *slot = slice_slot_find(item.anchor_point_point,
                                             item.anchor_point_read_channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point Content slice has no captured slot");
        }
        /* A content slice read as a mask is its coverage, which the coverage slot holds. */
        item_map = slot->coverage;
      }
      item_flags |= gpu_mask_slot;
    }
    else if (item.filter == 0 && item.image != nullptr) {
      bool premultiplied = false;
      item_map = gpu_map_texture(item.image, item.iuser, premultiplied);
      if (item_map == nullptr) {
        return gpu_refuse("a mask map has no GPU texture");
      }
      item_flags |= gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
    }
    if (!mask_step(t,
                   gpu_mask_item,
                   item_map,
                   item_flags,
                   item.constant_gray,
                   item.opacity,
                   1.0f,
                   int(item.blend),
                   int(item.filter),
                   item.filter_params,
                   item.filter_ramp,
                   item.filter_curves,
                   (live_reader || item.is_mesh_map) ? nullptr : &item.mapping,
                   nullptr,
                   nullptr,
                   item.contrast))
    {
      return false;
    }
  }
  if (layer.mask_image != nullptr) {
    bool premultiplied = false;
    gpu::Texture *mask_map = gpu_map_texture(layer.mask_image, layer.mask_iuser, premultiplied);
    if (mask_map == nullptr) {
      return gpu_refuse("a row's mask map has no GPU texture");
    }
    int map_flags = gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
    if (layer.mask_from_alpha) {
      map_flags |= gpu_mask_from_alpha;
    }
    /* A source coverage map reads through the row's mapping, projection included, as the CPU does;
     * without the tangent-space flip, which would corrupt a coverage scalar. */
    PaintMaterialCompositeMapping coverage_mapping = layer.mapping;
    coverage_mapping.tangent_normal = false;
    if (!mask_step(t,
                   gpu_mask_map,
                   mask_map,
                   map_flags,
                   1.0f,
                   1.0f,
                   layer.mask_influence,
                   int(CompositeBlend::Mix),
                   0,
                   nullptr,
                   nullptr,
                   nullptr,
                   layer.mapping.enabled ? &coverage_mapping : nullptr))
    {
      return false;
    }
  }
  return true;
}

bool CombinedPasses::row_emit(CombinedTarget &t,
                              const PaintMaterialCompositeImageLayer &layer,
                              const int channel,
                              const int level,
                              gpu::Texture *capture_color,
                              gpu::Texture *capture_coverage)
{
  if (!layer.enabled || layer.opacity <= 0.0f) {
    /* The composite skips it as well. */
    return true;
  }
  const bool in_folder = level >= 0;

  /* The corrections that change anything. */
  bool has_corrections = false;
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.opacity <= 0.0f) {
      continue;
    }
    has_corrections |= correction.filter != 0 || correction.image != nullptr ||
                       correction.has_constant_color ||
                       !BLI_uuid_is_nil(correction.anchor_point_reader);
  }

  /* Where the row's colour and coverage are decided: the row textures for a folder, for a row in
   * a folder (its parent reads them there) and for a row with corrections; the map itself
   * otherwise. A capture reads the row textures even when nothing else would. */
  bool row_in_textures = layer.is_folder || in_folder || has_corrections ||
                         capture_color != nullptr;
  int flags = 0;
  gpu::Texture *map = nullptr;
  GpuSliceSlot *reader_slot = nullptr;
  if (!BLI_uuid_is_nil(layer.anchor_point_reader) && layer.color_image == nullptr) {
    /* A live reader row: its slice is in a captured slot. A baked reader has a colour map and
     * no slot. */
    /* Slots are keyed by the channel enum, which only equals the index in the shaded set for the
     * first channels. */
    reader_slot = slice_slot_find(layer.anchor_point_point, channel);
    if (reader_slot == nullptr) {
      return gpu_refuse("an Anchor Point row slice has no captured slot");
    }
  }
  float4 layer_color(layer.constant_color);
  GpuMappingUniforms mapping_uniforms;
  GpuProjection projection_frame;
  const GpuProjection *projection = nullptr;
  /* Outside the row branch: the final lay below reads it as well. */
  const GpuInputSteps input_steps(layer.input_transforms);
  bool has_mask = false;

  if (layer.is_folder) {
    const int own_level = level + 1;
    if (!folder_levels_ensure(t, own_level)) {
      return gpu_refuse("folders are nested too deep for the GPU path");
    }
    folder_step(t, gpu_folder_init, own_level, 0, 1.0f, int(CompositeBlend::Mix), nullptr);
    for (const PaintMaterialCompositeImageLayer &child : layer.children) {
      if (!row_emit(t, child, channel, own_level, nullptr, nullptr)) {
        return false;
      }
    }
    /* After the children: the mask accumulator is theirs until they are laid. */
    if (!mask_build(t, layer, has_mask)) {
      return false;
    }
    folder_step(t, gpu_folder_resolve, own_level, 0, 1.0f, int(CompositeBlend::Mix), nullptr);
  }
  else {
    if (!mask_build(t, layer, has_mask)) {
      return false;
    }
    /* A row covers by its own alpha when it says so and no mask map takes the alpha's place. */
    const bool own_alpha = layer.mask_image == nullptr && layer.mask_from_alpha;
    if (reader_slot != nullptr) {
      map = reader_slot->color;
      flags |= gpu_layer_slot;
    }
    else if (layer.color_image != nullptr) {
      bool premultiplied = false;
      map = gpu_map_texture(layer.color_image, layer.color_iuser, premultiplied);
      if (map == nullptr) {
        return gpu_refuse("a layer map has no GPU texture");
      }
      flags |= gpu_layer_has_map;
      if (layer.normal_directx) {
        flags |= gpu_layer_normal_directx;
      }
      switch (eMaterialPaintLayerMapEncoding(layer.map_encoding)) {
        case MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS:
          flags |= gpu_layer_map_glossiness;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA:
          flags |= gpu_layer_map_glossiness_alpha;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_F0:
          flags |= gpu_layer_map_f0;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_F0_GLOSSINESS_ALPHA:
          flags |= gpu_layer_map_f0_glossiness_alpha;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_CHANNEL:
          break;
      }
      if (premultiplied) {
        flags |= gpu_layer_premultiplied;
      }
      if (layer.color_alpha_coverage || own_alpha) {
        flags |= gpu_layer_alpha_coverage;
      }
    }
    else if (own_alpha) {
      /* A flat colour covers by its own alpha when the row says so. */
      flags |= gpu_layer_alpha_coverage;
    }
    if (layer.empty_base) {
      flags |= gpu_layer_empty_base;
    }
    if (layer.adjustment_filter != 0) {
      flags |= gpu_layer_adjustment;
    }
    if (map != nullptr && reader_slot == nullptr) {
      gpu_mapping_setup(layer.mapping, flags, mapping_uniforms);
      if (layer.mapping.projection.present) {
        /* A projected row is addressed by its frame: the UV adjust rides in the same uniforms but
         * the fold and the repeat axes belong to the plain mapping, which a projection replaces
         * (the CPU bake never applies them to it). The tangent-space flag stays: the projected read
         * remaps and converts the Normal's vectors itself, as the CPU bake does. */
        if (!combined_projection_gpu_make(
                layer.mapping.projection, layer.mapping, projection_frame))
        {
          return false;
        }
        projection = &projection_frame;
        flags |= gpu_layer_projected;
        flags &= ~(gpu_layer_mapped | gpu_layer_repeat_u | gpu_layer_repeat_v |
                   gpu_layer_mirror_u | gpu_layer_mirror_v | gpu_layer_mirror_u_neg |
                   gpu_layer_mirror_v_neg);
      }
    }
    if (row_in_textures) {
      if (!lay(t,
               t.dest,
               map,
               flags | gpu_layer_row_store,
               layer_color,
               float4(0.0f),
               1.0f,
               mapping_uniforms,
               input_steps,
               int(CompositeBlend::Mix),
               nullptr,
               int(layer.adjustment_filter),
               layer.adjustment_params,
               layer.adjustment_ramp,
               layer.adjustment_curves,
               reader_slot != nullptr ? reader_slot->coverage : nullptr,
               projection))
      {
        return false;
      }
    }
  }

  if (row_in_textures) {
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      if (correction.opacity <= 0.0f) {
        continue;
      }
      const bool live_reader = !BLI_uuid_is_nil(correction.anchor_point_reader) &&
                               correction.image == nullptr &&
                               !correction.has_constant_color && correction.filter == 0;
      if (correction.filter == 0 && correction.image == nullptr &&
          !correction.has_constant_color && !live_reader)
      {
        continue;
      }
      gpu::Texture *slot_color = nullptr;
      gpu::Texture *slot_coverage = nullptr;
      if (live_reader) {
        GpuSliceSlot *slot = slice_slot_find(correction.anchor_point_point, channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point correction slice has no captured slot");
        }
        slot_color = slot->color;
        slot_coverage = slot->coverage;
      }
      gpu::Texture *effect_result = nullptr;
      if (paint_correction_is_effect(correction)) {
        /* A CPU-computed effect: the pass that computes it ran before the stacks, and its texture is
         * the correction's content now. Nothing found means this frame has no result for the effect
         * -- the row it sits on was not walked, or a captured slice holds a second effect -- so the
         * CPU path has to shade the whole stack rather than drop the effect. */
        effect_result = effect_result_find(correction.effect_marker, channel);
        if (effect_result == nullptr) {
          return gpu_refuse("a GPU effect has no computed result for this channel");
        }
      }
      if (!correction_step(
              t, correction, layer.tracks_content_alpha, slot_color, slot_coverage, effect_result))
      {
        return gpu_refuse("a correction map has no GPU texture");
      }
    }
  }

  if (capture_color != nullptr) {
    /* The row's content before its own opacity, mask and blend: what a reader of this point lays.
     * The row textures hold the corrections already, so only a straight copy is needed. */
    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_UPDATE);
    GPU_texture_copy(capture_color, t.row_color);
    /* The coverage slot is grey in rgb and opaque in alpha; the mask store pass does that from
     * the row factor's `x`. */
    if (!mask_step(t,
                   gpu_mask_slot_store,
                   nullptr,
                   0,
                   1.0f,
                   1.0f,
                   1.0f,
                   int(CompositeBlend::Mix),
                   0,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr,
                   capture_coverage,
                   t.row_factor))
    {
      return false;
    }
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
    return true;
  }

  const int mask_flags = has_mask ? gpu_layer_has_mask : 0;
  if (in_folder) {
    folder_step(t,
                gpu_folder_blend,
                level,
                mask_flags,
                layer.opacity,
                int(layer.blend),
                has_mask ? t.mask_accum : nullptr);
    return true;
  }
  if (!lay(t,
           t.dest,
           row_in_textures ? nullptr : map,
           (row_in_textures ? gpu_layer_row_use : flags) | mask_flags,
           layer_color,
           float4(0.0f),
           layer.opacity,
           mapping_uniforms,
           input_steps,
           int(layer.blend),
           has_mask ? t.mask_accum : nullptr,
           int(layer.adjustment_filter),
           layer.adjustment_params,
           layer.adjustment_ramp,
           layer.adjustment_curves,
           reader_slot != nullptr ? reader_slot->coverage : nullptr,
           projection))
  {
    return false;
  }
  return true;
}

void CombinedPasses::effect_pass_dispatch(const EffectDispatchArgs &args,
                                          const float4 &luma_coefficients)
{
  const GpuEffectJob &job = args.job;
  const rcti &rect = args.rect;
  const int2 origin(rect.xmin, rect.ymin);
  const int2 rect_size(rect.xmax - rect.xmin, rect.ymax - rect.ymin);
  const int2 groups = math::divide_ceil(rect_size, int2(16));
  if (groups.x <= 0 || groups.y <= 0) {
    return;
  }
  GPU_shader_bind(effect_shader);
  GPU_shader_uniform_2iv(effect_shader, "texture_size", &job.input_size.x);
  GPU_shader_uniform_2iv(effect_shader, "dispatch_origin", &origin.x);
  const int2 end(rect.xmax, rect.ymax);
  GPU_shader_uniform_2iv(effect_shader, "dispatch_end", &end.x);
  GPU_shader_uniform_1i(effect_shader, "pass_kind", int(args.pass.kind));
  GPU_shader_uniform_1i(effect_shader, "radius", args.pass.radius);
  GPU_shader_uniform_1i(effect_shader, "maximum", args.pass.maximum ? 1 : 0);
  GPU_shader_uniform_1f(effect_shader, "aux0", args.pass.aux0);
  GPU_shader_uniform_4fv(effect_shader, "pass_params", &args.pass.params.x);
  GPU_shader_uniform_4fv(effect_shader, "pass_params2", &args.pass.params2.x);
  GPU_shader_uniform_1i(effect_shader, "alpha_from_kernel", args.pass.alpha_from_kernel ? 1 : 0);
  GPU_shader_uniform_4fv(effect_shader, "filter_params_0", job.correction->filter_params);
  GPU_shader_uniform_4fv(effect_shader, "luma_coefficients", &luma_coefficients.x);
  GPU_shader_uniform_1i(effect_shader, "tracks_alpha", job.tracks_alpha ? 1 : 0);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(args.src, GPU_shader_get_sampler_binding(effect_shader, "input_tx"));
  GPU_texture_bind(args.aux != nullptr ? args.aux : dummy,
                   GPU_shader_get_sampler_binding(effect_shader, "aux_tx"));
  GPU_texture_image_bind(args.dst,
                         GPU_shader_get_sampler_binding(effect_shader, "result_img"));
  /* The reduction and the flooding write their float32 texture through the field slot, whose
   * declaration names the 32F format the 16F result slot cannot take; the result slot still gets a
   * valid 16F binding those kernels leave alone. */
  /* A pass that writes no field still binds the placeholder: the slot belongs to the whole shader. */
  gpu::Texture *field_bound = (args.field_dst != nullptr) ? args.field_dst : job.field_placeholder;
  if (field_bound != nullptr) {
    GPU_texture_image_bind(field_bound,
                           GPU_shader_get_sampler_binding(effect_shader, "field_img"));
  }
  GPU_compute_dispatch(effect_shader, uint(groups.x), uint(groups.y), 1);
  if (field_bound != nullptr) {
    GPU_texture_image_unbind(field_bound);
  }
  GPU_texture_image_unbind(args.dst);
  GPU_texture_unbind(args.src);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  dispatches++;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Effect Stage
 * \{ */

/** The GPU effect cache's acquire hook: a canvas of the shape the frame's own accumulators have --
 * half float, read by the correction pass and written by the effect pass. HOST_READ joins the two:
 * the owned-effect bake provider reads an entry back into a CPU buffer, and the read-back usage
 * flag has to be named when the texture is made. The cache keeps one reference to what this
 * returns; a frame holding it across a dispatch takes one of its own. */
static void *effect_gpu_acquire(void * /*data*/, const int width, const int height)
{
  gpu::Texture *texture = GPU_texture_create_2d(
      "combined_effect_result",
      width,
      height,
      1,
      gpu::TextureFormat::SFLOAT_16_16_16_16,
      eGPUTextureUsage(GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE |
                       GPU_TEXTURE_USAGE_HOST_READ),
      nullptr);
  if (texture == nullptr) {
    return nullptr;
  }
  /* The result is read at the display's resolution, so a canvas texel is usually covered by several
   * screen pixels and filtered between them, exactly as a painted map is. */
  GPU_texture_filter_mode(texture, true);
  return texture;
}

/** The cache's release hook: drop its reference, which destroys the texture unless a frame still
 * holds one. */
static void effect_gpu_release(void * /*data*/, void *resource)
{
  if (resource != nullptr) {
    GPU_texture_free(static_cast<gpu::Texture *>(resource));
  }
}

/** The hooks the GPU effect cache works through for this module's textures. */
static const PaintLayerEffectGpuHooks effect_gpu_hooks = {
    nullptr,
    effect_gpu_acquire,
    effect_gpu_release,
};

/**
 * Whether \a entry's pixels depend on the painted surface anywhere a CPU compute without the object
 * describes differently: a projection on the row, a correction, a mask item or a nested child; a
 * transform step in its own Decal frame (the only steps that carry one); a live Anchor Point reader
 * or a baked one's coverage; or a gradient whose exact answer is keyed on the surface. The baked
 * job's CPU answer (#BKE_paint_layers_effect_buffer_result) is computed with no object, and a
 * projected read without one lays transparent rather than at the wrong UV -- uploading that would
 * publish pixels the described input does not hold. The flat shapes a filter usually reads -- a
 * painted map, a flat colour, a Material row's own bake maps, a plain transform step -- describe
 * the same without a surface and stay on the fast path.
 */
static bool combined_effect_input_needs_surface(const PaintMaterialCompositeImageLayer &entry)
{
  if (entry.mapping.projection.present || !BLI_uuid_is_nil(entry.anchor_point_reader)) {
    return true;
  }
  if (entry.gradient.enabled && entry.gradient.surface_key != 0) {
    return true;
  }
  for (const PaintMaterialCompositeMapping &step : entry.input_transforms) {
    if (step.projection.present) {
      return true;
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : entry.content_corrections) {
    if (correction.mapping.projection.present ||
        !BLI_uuid_is_nil(correction.anchor_point_reader) ||
        correction.anchor_point_coverage_image != nullptr ||
        (correction.gradient.enabled && correction.gradient.surface_key != 0))
    {
      return true;
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : entry.mask_items) {
    if (item.mapping.projection.present || item.coverage_mapping.projection.present ||
        !BLI_uuid_is_nil(item.anchor_point_reader))
    {
      return true;
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : entry.children) {
    if (combined_effect_input_needs_surface(child)) {
      return true;
    }
  }
  return false;
}

/**
 * Fill the baked job's cache texture with the CPU effect cache's buffer: the node-group filter's
 * kernel runs on the CPU (the shader-bake backend renders the user's tree over the described
 * input), and the buffer that comes back is the answer -- the same pixels the CPU composite mixes
 * and the bake writes. Uploading it is therefore parity by construction, not an approximation.
 *
 * The gather runs on the main thread, the only thread the effect cache computes on (a bake job
 * computes there too, and the cache's mutex is never held while one computes), so the compute this
 * may trigger -- the input rendered whole, nested slices included -- is safe here. A surface-
 * dependent input never reaches here: the cache loop refused it before the job was queued to
 * render (#combined_effect_input_needs_surface), so the compute below sees a description that reads
 * the same without the object.
 *
 * The buffer is premultiplied, scene-linear RGBA float with the content alpha in the fourth
 * component, one row per scanline from the bottom up; the texture is the RGBA half-float canvas
 * #effect_gpu_acquire made at the input's size, which the correction pass's effect branch reads and
 * straightens exactly as `composite_decode_tile` does on the CPU. `GPU_texture_update` converts the
 * float rows to half floats in place -- no colourspace step, no row flip, the convention the image
 * textures upload by. The whole buffer goes up whatever the cache named as moved: the kernel is
 * pointwise, so a partial upload would save nothing worth the path.
 *
 * \return false with the reason reported when the CPU answer cannot be had or does not match the
 *         texture the cache handed out.
 */
bool combined_effect_bake_upload(const Material &ma,
                                 const GpuEffectJob &job,
                                 const int tile_number)
{
  int width = 0;
  int height = 0;
  /* The CPU effect cache's buffer, keyed under the same marker the GPU entry is: a direct effect's
   * DNA marker, a chain member's composite one -- the member asks through the prefix-aware getter,
   * whose input folds the members before it exactly as the described input above does, so the two
   * sizes agree the same way the direct effect's do. */
  std::shared_ptr<ImBuf> buffer = (job.correction->effect_member != nullptr) ?
                                      BKE_paint_layers_chain_member_effect_buffer_result(
                                          ma,
                                          *job.row,
                                          *job.correction->effect_chain,
                                          *job.effect,
                                          job.channel,
                                          tile_number,
                                          width,
                                          height) :
                                      BKE_paint_layers_effect_buffer_result(
                                          ma, *job.effect, job.channel, tile_number, width, height);
  if (buffer == nullptr || buffer->float_buffer.data == nullptr) {
    return gpu_refuse("a node-group filter has no CPU result to upload");
  }
  if (width != job.input_size.x || height != job.input_size.y ||
      GPU_texture_width(job.result) != width || GPU_texture_height(job.result) != height)
  {
    /* The CPU cache resolves the input's size through the same describe path the GPU key was built
     * from, so this should not happen; a mismatch would smear one grid onto another. */
    return gpu_refuse("a node-group filter result does not match its input size");
  }
  GPU_texture_update(job.result, GPU_DATA_FLOAT, buffer->float_buffer.data);
  /* The upload lands before anything reads it: the correction pass samples the texture this same
     frame, and the capture path barriers the same way (GPU_BARRIER_TEXTURE_UPDATE for the host
     write, the fetch bit for the sampler that reads it after). */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
  /* The versions are stamped after the upload, the way the dispatch path stamps them after its
   * passes: an edit that landed while the CPU computed is newer than this entry and costs one more
   * update rather than a result that never clears. The hashes are the ones the entry was keyed on
   * above, so the publish finds it -- a chain member's through the keyed form, the composite
   * marker and the instance-unique hash its correction carries. */
  if (job.correction->effect_member != nullptr) {
    BKE_paint_layers_effect_gpu_cache_publish_keyed(ma,
                                                    job.correction->effect_marker,
                                                    job.correction->effect_hash,
                                                    job.channel,
                                                    job.input_size.x,
                                                    job.input_size.y,
                                                    job.images,
                                                    PaintLayerEffectKeyExtras{job.chain_hash,
                                                                                 job.input_scalar_hash},
                                                    tile_number);
  }
  else {
    BKE_paint_layers_effect_gpu_cache_publish(ma,
                                              *job.effect,
                                              job.channel,
                                              job.input_size.x,
                                              job.input_size.y,
                                              job.images,
                                              PaintLayerEffectKeyExtras{job.chain_hash,
                                                                           job.input_scalar_hash},
                                              tile_number);
  }
  return true;
}

bool combined_preview_gpu_effects_run(
    const Material &ma,
    Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
    Span<int> channel_enums,
    Span<bool> has_stack,
    int wanted_channel,
    CombinedPasses &passes,
    Vector<GpuEffectJob> &r_jobs,
    const Object *painted_object,
    const int tile_number,
    const rcti *effects_region,
    const bUUID *effect_limit)
{
  /* GPU effects: the content each effect reads, then the effect's own passes into the cache. This
   * runs after the Anchor Point captures -- a content render may read their slots -- and before the
   * channels' stacks, which mix the results in as corrections. A row may carry several effects:
   * they are collected in the row's own order, and each one's input (described up to its marker)
   * holds the results of the ones before it, so they are rendered and published in that order
   * below. */
  const auto effect_jobs_walk = [&](auto &&self,
                                   const PaintMaterialCompositeImageLayer &layer,
                                   const int channel) -> void {
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      /* An effect correction is the one kind that carries a parameter hash of its own, and the
       * refusal above already dropped any effect the preview cannot compute itself. */
      if (correction.opacity <= 0.0f || !paint_correction_is_effect(correction)) {
        continue;
      }
      GpuEffectJob job;
      job.correction = &correction;
      job.channel = channel;
      /* The effects before this one in the same row feed its input: their markers and parameter
       * hashes order-dependently re-key this entry. */
      job.chain_hash = BKE_paint_layers_effect_chain_hash(
          Span<const PaintMaterialCompositeCorrection>(
              layer.content_corrections.data(), int64_t(layer.content_corrections.size())),
          correction.effect_marker);
      r_jobs.append(std::move(job));
      /* A bake names the one effect it wants: the ones after it in this row are warmed for a frame
       * the bake never reads, at a main-thread dispatch each, so the walk stops at the marker. The
       * preview passes no marker and walks the row whole. */
      if (effect_limit != nullptr && BLI_uuid_equal(correction.effect_marker, *effect_limit)) {
        return;
      }
    }
    for (const PaintMaterialCompositeImageLayer &child : layer.children) {
      self(self, child, channel);
    }
  };
  for (const int i : IndexRange(stacks.size())) {
    if (!has_stack[i] || (wanted_channel >= 0 && i != wanted_channel)) {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : stacks[i]) {
      effect_jobs_walk(effect_jobs_walk, layer, channel_enums[i]);
    }
  }
  for (GpuEffectJob &job : r_jobs) {
    if (job.correction->effect_member != nullptr) {
      /* A Smart Filter chain's GPU-computed member: the correction carries the pair the marker
       * cannot name -- a composite marker is no row of the material, so the lookup below would
       * refuse it. The member answers the kind flags and the definition tree exactly as a direct
       * effect's row does, and the chain row resolves the owner the same lookup the CPU cache's
       * caller runs. */
      job.effect = job.correction->effect_member;
      job.row = BKE_paint_layers_correction_owner(ma, *job.correction->effect_chain);
    }
    else {
      /* The description names the effect by its marker but not the row it sits on, and the input
       * has to be described from both: find them in the material the same way the CPU cache's
       * caller does. */
      job.effect = BKE_paint_layers_find(ma, job.correction->effect_marker);
    }
    if (job.effect == nullptr) {
      return gpu_refuse("a GPU effect row cannot be resolved");
    }
    if (!BKE_paint_layers_effect_is_gpu_computed(*job.effect)) {
      /* A kind with no effect pass of its own is refused, with the one exception: the node-group
       * filter, whose CPU kernel answers through the shader-bake backend over the same described
       * input the effect cache renders, so the CPU cache's buffer *is* the result and the GPU path
       * publishes it instead of dispatching. A chain member of the same kind takes the same road:
       * it was never a direct effect, but the predicate reads the member row's own kind, and its
       * own plan is empty exactly like the direct one's -- the baked treatment is per member. */
      if (!BKE_paint_layers_effect_is_precomputed_result(*job.effect)) {
        return gpu_refuse("a GPU effect row cannot be resolved");
      }
      /* Without the backend or the definition tree the kernel leaves its buffer unchanged, and
       * uploading that would publish a wrong "no-op" as the effect's result: refuse to the CPU
       * preview, which skips the answer the same way. The parity reason lives on the predicate
       * (#BKE_paint_layers_effect_node_group_can_compute), which reads the member row's own
       * definition tree -- the tree the definition copy carried at save time. */
      if (!BKE_paint_layers_effect_node_group_can_compute(*job.effect)) {
        return gpu_refuse("a node-group filter has no shader-bake backend");
      }
      job.baked_result = true;
    }
    if (job.row == nullptr) {
      job.row = BKE_paint_layers_correction_owner(ma, *job.effect);
    }
    if (job.row == nullptr) {
      return gpu_refuse("a GPU effect has no owner row");
    }
  }

  /* The cache first, and each input's size, plan and fingerprints with it: a hit costs no dispatch,
   * and the versions of the input's maps are read as the frame sees them. The input is described once
   * here (describe-only, no slice rendered) and the same description is re-used by the render below. */
  bke::paint_perf::ScopedTimer perf_cache("gpu_effect.cache");
  int effect_hits = 0;
  /* The effects of one row chain through each other's results, and the walk collected them in
   * the row's own order, so the halo of the ones before this job is a running sum that resets
   * when the row changes. */
  const MaterialPaintLayer *chain_row = nullptr;
  int chain_channel = -1;
  int chain_halo = 0;
  for (GpuEffectJob &job : r_jobs) {
    /* The input is described once here (describe-only, no slice rendered) and the same description
     * is re-used by the render below. A Smart Filter chain member's input is the owner's content up
     * to the chain with the members before it folded in -- the same prefix-aware describe the CPU
     * cache computes the member from, so the two backends lay the member over the same canvas. */
    const bool described = (job.correction->effect_member != nullptr) ?
                               BKE_paint_layers_chain_member_input_describe(
                                   ma,
                                   *job.row,
                                   *job.correction->effect_chain,
                                   *job.effect,
                                   job.channel,
                                   /*dimensions_only=*/false,
                                   /*describe_only=*/true,
                                   job.input_entry,
                                   PaintLayerTileContext{.painted_object = painted_object,
                                                         .tile_number = tile_number}) :
                               BKE_paint_layers_effect_input_describe(
                                   ma,
                                   *job.row,
                                   *job.effect,
                                   job.channel,
                                   /*dimensions_only=*/false,
                                   /*describe_only=*/true,
                                   job.input_entry,
                                   PaintLayerTileContext{.painted_object = painted_object,
                                                         .tile_number = tile_number});
    if (!described) {
      return gpu_refuse("an effect input cannot be described");
    }
    int width = 0;
    int height = 0;
    if (!BKE_paint_material_composite_stack_dimensions(
            Span<PaintMaterialCompositeImageLayer>(&job.input_entry, 1), width, height) ||
        width <= 0 || height <= 0)
    {
      return gpu_refuse("an effect input has no dimensions");
    }
    job.input_size = int2(width, height);
    /* The jobs run channel by channel, so the same row comes round again for the next channel: its
     * chain starts over there as well, or the halo of the previous channel's effects would widen
     * every region of the next. */
    if (job.row != chain_row || job.channel != chain_channel) {
      chain_row = job.row;
      chain_channel = job.channel;
      chain_halo = 0;
    }
    job.predecessor_halo = chain_halo;
    BKE_paint_layers_effect_input_images(job.input_entry, job.images);
    /* The pass plan, the neighbourhood halo, the scalar fingerprint of the input, and whether the
     * region the cache reports can narrow the dispatch -- all decided from the description. A
     * baked job builds no plan: its kind has no effect pass (the node-group filter), so there is
     * nothing for the plan to describe and nothing for the slot rule to check. */
    const float *params = job.correction->filter_params;
    job.input_scalar_hash = BKE_paint_layers_effect_input_scalar_hash(job.input_entry);
    if (job.baked_result) {
      /* The CPU answer this job uploads is computed without the painted surface, so a description
       * whose pixels depend on one would publish what the described input does not hold: the
       * surface-dependent shapes keep the kind with the CPU preview, the way the bake computes them
       * per object. */
      if (combined_effect_input_needs_surface(job.input_entry)) {
        return gpu_refuse("a node-group filter input reads the surface the CPU answer cannot");
      }
      /* The kernel is a function of one texel -- the node group evaluates each pixel on its own --
       * so the result moves only inside an edited rectangle: no halo of its own, and none passed on
       * to the jobs after this one in the row. The whole-canvas upload below covers whatever the
       * cache named as moved, which is more than a pointwise kernel needs and never less. */
      job.halo = 0;
    }
    else {
      job.halo = BKE_paint_layers_effect_halo(
          eMaterialPaintLayerFilter(job.correction->filter), params, width, height);
      BKE_paint_layers_effect_pass_plan(
          eMaterialPaintLayerFilter(job.correction->filter), params, width, height, job.passes);
      if (job.passes.is_empty()) {
        if (bke::paint_perf::enabled()) {
          /* Which kind and which parameters the plan refused: a parametric refusal (a picked Color
           * Match, a Fill Area debug) and a plan the packing starved look alike otherwise. */
          printf("[PBR-PERF] gpu_effect.noplan kind=%d params=%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g\n",
                 int(job.correction->filter),
                 params[0], params[1], params[2], params[3], params[4], params[5], params[6],
                 params[7], params[8], params[9], params[10], params[11]);
          fflush(stdout);
        }
        return gpu_refuse("a GPU effect has no pass plan");
      }
      /* A plan that names a slot fetch but no slot image would sample nothing at dispatch, and a
       * result cached under such a description cannot be trusted either, so the effect -- hit or
       * miss -- is refused to the CPU path. */
      for (const PaintLayerEffectPass &pass : job.passes) {
        if (pass.aux == ePaintLayerEffectPassAux::Slot && job.input_entry.slot_image == nullptr) {
          return gpu_refuse("a Channel Split effect names no slot image");
        }
      }
    }
    chain_halo += job.halo;
    void *resource = nullptr;
    /* A direct effect's entry is keyed on its row's marker and hash; a chain member's on the
     * composite marker and the instance-unique hash its correction carries -- the same key the CPU
     * cache filed the member's answer under. */
    const PaintLayerEffectGpuState state =
        (job.correction->effect_member != nullptr) ?
            BKE_paint_layers_effect_gpu_cache_get_keyed(ma,
                                                        job.correction->effect_marker,
                                                        job.correction->effect_hash,
                                                        job.channel,
                                                        width,
                                                        height,
                                                        job.images,
                                                        PaintLayerEffectKeyExtras{
                                                            job.chain_hash,
                                                            job.input_scalar_hash},
                                                        effect_gpu_hooks,
                                                        &job.region,
                                                        &resource,
                                                        tile_number) :
            BKE_paint_layers_effect_gpu_cache_get(ma,
                                                  *job.effect,
                                                  job.channel,
                                                  width,
                                                  height,
                                                  job.images,
                                                  PaintLayerEffectKeyExtras{
                                                      job.chain_hash,
                                                      job.input_scalar_hash},
                                                  effect_gpu_hooks,
                                                  &job.region,
                                                  &resource,
                                                  tile_number);
    if (state == PaintLayerEffectGpuState::Failed || resource == nullptr) {
      return gpu_refuse("a GPU effect texture could not be allocated");
    }
    job.result = static_cast<gpu::Texture *>(resource);
    /* A reference of the frame's own: the cache may evict the entry -- or replace it, the effect
     * having been edited -- while these passes still bind its texture. */
    GPU_texture_ref(job.result);
    job.needs_render = state == PaintLayerEffectGpuState::Render;
    job.region_exact = BKE_paint_layers_effect_region_is_exact(
        job.input_entry, job.images, width, height);
    /* The caller's rectangle of source texels it already knows moved (the bake's subscription, in
     * the input's own texels) joins the cache's dirty region. It can only extend the band, never
     * shrink it: the texture is shared with the screen and the publish below stamps it valid as a
     * whole, so a render has to cover everything the cache itself named as moved. A Hit carries no
     * dispatch, and a fresh or whole-canvas dirty stays whole whatever joins it, so the rectangle
     * never costs more than the caller's own view of the change; where the description is not
     * region exact the dispatch runs whole regardless (`rect_for` below). */
    if (job.needs_render && effects_region != nullptr && !BLI_rcti_is_empty(effects_region)) {
      /* Clamped to the canvas first: the caller's rectangle names texels of the sources, which the
       * region-exact rule above holds to the input's own grid, and a band outside it narrows
       * nothing. */
      rcti clamped = *effects_region;
      rcti canvas_rect;
      BLI_rcti_init(&canvas_rect, 0, width, 0, height);
      if (BLI_rcti_isect(&clamped, &canvas_rect, &clamped) && !BLI_rcti_is_empty(&clamped)) {
        BLI_rcti_union(&job.region, &clamped);
      }
    }
    passes.effect_results.append(GpuEffectResult{
        .marker = job.correction->effect_marker, .channel = job.channel, .texture = job.result});
    if (!job.needs_render) {
      effect_hits++;
    }
  }
  perf_cache.note("effects=%d hit=%d render=%d budget=%d MiB region=%s%dx%d",
                  int(r_jobs.size()),
                  effect_hits,
                  int(r_jobs.size()) - effect_hits,
                  int(BKE_paint_layers_effect_gpu_cache_budget() / (1024 * 1024)),
                  /* The caller's own rectangle, when the bake handed one in; 0x0 is its absence. */
                  effects_region != nullptr ? "provider=" : "",
                  effects_region != nullptr ? BLI_rcti_size_x(effects_region) : 0,
                  effects_region != nullptr ? BLI_rcti_size_y(effects_region) : 0);

  /* The render of the collected jobs -- each one's input laid, its pass plan dispatched over
   * that input and the publish into the GPU result cache -- lives in
   * paint_material_combined_effect_pass.cc, beside the buffer routing both dispatchers share. */
  if (!combined_effect_jobs_render(ma, passes, r_jobs, effects_region, tile_number)) {
    return false;
  }

  return true;
}

/** \} */

}  // namespace blender::ed::material_combined
