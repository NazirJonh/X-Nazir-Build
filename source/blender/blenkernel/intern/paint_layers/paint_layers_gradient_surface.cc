/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Per-object UV-to-object-space surfaces for the 2D Image Editor preview.
 */

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>

#include "BKE_paint_layers_gradient_surface.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_hash.hh"
#include "BLI_implicit_sharing.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"
#include "BLI_time.h"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"

namespace blender {

namespace {

/**
 * A surface carries two float3 grids (evaluated position and CD_ORCO) plus the valid mask:
 * 1024² × (2 × 12 + 1) bytes ≈ 26 MB per surface, so both the resolution and the cache are
 * deliberately small. Two entries cover the usual "one object, previous UV map" switch without
 * letting a multi-object scene pin hundreds of MB.
 */
constexpr int surface_cache_capacity = 2;
constexpr int surface_size_min = 256;
constexpr int surface_size_max = 1024;

/**
 * The one-shot surface of a corrections bake follows the canvas it writes, which a 4K map makes
 * 4096²: two float3 grids plus the valid mask, 25 bytes a texel, about 400 MiB next to the bake's
 * own strip buffers. Never cached, so only one is alive at a time.
 */
constexpr int bake_surface_size_min = 256;
constexpr int bake_surface_size_max = 4096;

/**
 * Content revisions are process-global and monotonic: a cache entry may be evicted and rebuilt
 * under the same identity key, and every consumer of the surface needs a value that still tells
 * the two apart. Atomic because surfaces may in principle be built from more than one thread.
 */
std::atomic<uint64_t> next_surface_revision{0};

/** How many vertices #geometry_stamp samples; enough to tell two deformations apart, cheap per redraw. */
constexpr int stamp_sample_num = 64;

/**
 * A change-detecting stamp of one attribute array: where it lives, who shares it and how often it
 * was made mutable. The version moves on every in-place edit (a sculpt stroke, a shape key), the
 * pointers on every rebuild of the evaluated mesh.
 */
uint64_t attribute_stamp(const bke::AttributeAccessor &attributes, const StringRef name)
{
  const bke::GAttributeReader reader = attributes.lookup(name);
  if (!reader) {
    return 0;
  }
  const ImplicitSharingInfo *sharing = reader.sharing_info;
  return get_default_hash(reinterpret_cast<const void *>(sharing),
                          sharing != nullptr ? sharing->version() : int64_t(-1));
}

/**
 * A stamp of the geometry the surface is rasterized from: positions, topology and the UV layer.
 *
 * The evaluated mesh is freed and reallocated on every depsgraph update, and the allocator happily
 * hands the same address back, so pointers alone can repeat for different content. A strided sample
 * of the positions covers that case, and the sharing versions cover an edit in place. This is what
 * lets the cache stay correct without clearing it on every redraw notifier.
 */
uint64_t geometry_stamp(const Mesh &mesh, const StringRef uv_map_name)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  uint64_t stamp = get_default_hash(reinterpret_cast<const void *>(&mesh),
                                    int(mesh.verts_num),
                                    int(mesh.corners_num));
  stamp = get_default_hash(stamp,
                           attribute_stamp(attributes, "position"),
                           attribute_stamp(attributes, ".corner_vert"),
                           attribute_stamp(attributes, uv_map_name));

  const Span<float3> positions = mesh.vert_positions();
  const int step = std::max<int>(1, int(positions.size()) / stamp_sample_num);
  for (int i = 0; i < positions.size(); i += step) {
    stamp = get_default_hash(stamp, positions[i].x, positions[i].y, positions[i].z);
  }
  return stamp;
}

/**
 * The identity of a surface: which object, which UV map, which resolution and which geometry.
 */
uint64_t surface_key(const Object &object,
                     const Mesh &mesh,
                     const char *uv_map_name,
                     const int size,
                     const float3 &texspace_location,
                     const float3 &texspace_size)
{
  const StringRef uv_name = uv_map_name != nullptr ? uv_map_name : "";
  uint64_t key = get_default_hash(uint64_t(object.id.session_uid), uv_name, size);
  key = get_default_hash(key, geometry_stamp(mesh, uv_name));
  key = get_default_hash(key, texspace_location.x, texspace_location.y, texspace_location.z);
  key = get_default_hash(key, texspace_size.x, texspace_size.y, texspace_size.z);
  return key;
}

/**
 * The identity a surface is keyed on, with the texture space that key was hashed with. The
 * texture space is computed once and shared by the key and the build, so a lookup and the build it
 * misses cannot disagree about it.
 */
struct SurfaceIdentity {
  uint64_t key = 0;
  float3 texspace_location = float3(0.0f);
  float3 texspace_size = float3(1.0f);
  bool bbox_valid = false;
};

static SurfaceIdentity surface_identity(const Object &object,
                                        const Mesh &mesh,
                                        const char *uv_map_name,
                                        const int clamped_size)
{
  SurfaceIdentity identity;
  identity.bbox_valid = BKE_paint_layers_gradient_generated_box_get(
      &object, mesh, identity.texspace_location, identity.texspace_size);
  identity.key = surface_key(object,
                             mesh,
                             uv_map_name,
                             clamped_size,
                             identity.texspace_location,
                             identity.texspace_size);
  return identity;
}

/**
 * Grow every rasterized island outward by \a passes texels, one ring a pass: each empty texel takes
 * the position of its nearest valid neighbour, and the writes of a pass only become sources in the
 * next one, so the result does not depend on the traversal order.
 *
 * A written map is read with bilinear filtering and mipmaps; an empty texel beside an island would
 * otherwise blend into it as a dark or transparent seam. Edge neighbours go before diagonal ones,
 * which keeps a straight island border straight. Each pass is bounded by the current valid box, so
 * the cost follows the grown area, not the whole canvas.
 *
 * \return how many texels the padding filled, so the caller can keep the surface's island count
 *         without a second pass over the mask.
 */
static int64_t surface_pad_islands(MutableSpan<float3> positions,
                                   MutableSpan<float3> generated_positions,
                                   MutableSpan<uint8_t> valid,
                                   const int width,
                                   const int height,
                                   const int passes,
                                   int valid_min_x,
                                   int valid_min_y,
                                   int valid_max_x,
                                   int valid_max_y)
{
  /* Four edge neighbours first, then the four diagonals. */
  constexpr int offsets[8][2] = {
      {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  const bool copy_generated = !generated_positions.is_empty();
  struct TexelCopy {
    int dst;
    int src;
  };
  Vector<TexelCopy> copies;
  int64_t filled = 0;
  for (int pass = 0; pass < passes; pass++) {
    copies.clear();
    const int x_min = std::max(0, valid_min_x - 1);
    const int x_max = std::min(width - 1, valid_max_x + 1);
    const int y_min = std::max(0, valid_min_y - 1);
    const int y_max = std::min(height - 1, valid_max_y + 1);
    for (int y = y_min; y <= y_max; y++) {
      for (int x = x_min; x <= x_max; x++) {
        const int index = y * width + x;
        if (valid[index] != 0) {
          continue;
        }
        for (const auto &offset : offsets) {
          const int nx = x + offset[0];
          const int ny = y + offset[1];
          if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
            continue;
          }
          const int neighbour = ny * width + nx;
          if (valid[neighbour] != 0) {
            copies.append({index, neighbour});
            break;
          }
        }
      }
    }
    if (copies.is_empty()) {
      break;
    }
    filled += int64_t(copies.size());
    for (const TexelCopy &copy : copies) {
      positions[copy.dst] = positions[copy.src];
      if (copy_generated) {
        generated_positions[copy.dst] = generated_positions[copy.src];
      }
      valid[copy.dst] = 1;
      const int dst_x = copy.dst % width;
      const int dst_y = copy.dst / width;
      valid_min_x = std::min(valid_min_x, dst_x);
      valid_max_x = std::max(valid_max_x, dst_x);
      valid_min_y = std::min(valid_min_y, dst_y);
      valid_max_y = std::max(valid_max_y, dst_y);
    }
  }
  return filled;
}

/**
 * Rasterize \a mesh's render-active UV map into a surface. \a texspace_location, \a texspace_size
 * and \a bbox_valid come from the caller's box lookup, shared with the key the surface is cached
 * under.
 *
 * \param padding_texels: island growth for a map read with filtering (#surface_pad_islands).
 * \param store_generated: keep the #CD_ORCO sample per texel, which only a Generated Gradient
 *                         reads; without it the texture-space box is the generated coordinate.
 */
std::shared_ptr<PaintMaterialCompositeSurface> surface_build(const Mesh &mesh,
                                                             const char *uv_map_name,
                                                             const int size,
                                                             const uint64_t key,
                                                             const float3 &texspace_location,
                                                             const float3 &texspace_size,
                                                             const bool bbox_valid,
                                                             const int padding_texels,
                                                             const bool store_generated)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  const auto uv_attr = attributes.lookup<float2>(uv_map_name, bke::AttrDomain::Corner);
  if (!uv_attr) {
    return nullptr;
  }
  const VArraySpan<float2> uvs(*uv_attr);

  auto surface = std::make_shared<PaintMaterialCompositeSurface>();
  surface->width = size;
  surface->height = size;
  surface->key = key;
  surface->revision = ++next_surface_revision;

  const int64_t texel_count = int64_t(size) * size;
  surface->positions = Vector<float3>(texel_count, float3(0.0f));
  surface->valid = Vector<uint8_t>(texel_count, uint8_t(0));

  /* CD_ORCO is what EEVEE's `Generated` output reads. It is absent on an undeformed mesh, where
   * the texspace bbox below produces the same coordinate; on a deformed mesh it holds the
   * pre-deformation position and must not be replaced by the deformed one. Only a Generated
   * Gradient row reads it, so a bake without one skips the third grid. */
  const float3 *orco = static_cast<const float3 *>(
      CustomData_get_layer(&mesh.vert_data, CD_ORCO));
  if (store_generated && orco != nullptr && mesh.verts_num > 0) {
    surface->generated_positions = Vector<float3>(texel_count, float3(0.0f));
  }

  if (bbox_valid) {
    surface->generated_bbox_valid = true;
    surface->generated_bbox_min = texspace_location - texspace_size;
    surface->generated_bbox_max = texspace_location + texspace_size;
  }

  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();
  if (positions.is_empty() || corner_verts.is_empty() || corner_tris.is_empty()) {
    /* No geometry to rasterize: the surface covers nothing, which is a counted zero and not an
     * unknown. */
    surface->valid_texel_count = 0;
    return surface;
  }

  MutableSpan<float3> dst_positions = surface->positions.as_mutable_span();
  MutableSpan<uint8_t> dst_valid = surface->valid.as_mutable_span();
  MutableSpan<float3> dst_generated = surface->generated_positions.as_mutable_span();
  const bool has_orco = !dst_generated.is_empty();
  const float inv_size = 1.0f / float(size);
  /* The box the islands grew into, so a padding pass walks it instead of the whole canvas. */
  int valid_min_x = size;
  int valid_min_y = size;
  int valid_max_x = -1;
  int valid_max_y = -1;
  /* Counted as the texels are marked, so no reader has to walk the mask: the largest surfaces are
   * built for a bake, and the 2D preview asks this of every redraw. */
  int64_t valid_count = 0;

  for (const int3 &tri : corner_tris) {
    const int c0 = tri.x, c1 = tri.y, c2 = tri.z;
    if (c0 < 0 || c1 < 0 || c2 < 0 || c0 >= corner_verts.size() ||
        c1 >= corner_verts.size() || c2 >= corner_verts.size())
    {
      continue;
    }
    const float2 uv0 = uvs[c0];
    const float2 uv1 = uvs[c1];
    const float2 uv2 = uvs[c2];
    if (!std::isfinite(uv0.x) || !std::isfinite(uv0.y) || !std::isfinite(uv1.x) ||
        !std::isfinite(uv1.y) || !std::isfinite(uv2.x) || !std::isfinite(uv2.y))
    {
      continue;
    }

    const float2 tri_min = math::min(math::min(uv0, uv1), uv2);
    const float2 tri_max = math::max(math::max(uv0, uv1), uv2);

    /* Clamp before the float-to-int conversion: an authored UV may be arbitrarily far outside the
     * square, and only the part inside it can address a texel. */
    const float tri_min_x = std::clamp(tri_min.x, 0.0f, 1.0f);
    const float tri_max_x = std::clamp(tri_max.x, 0.0f, 1.0f);
    const float tri_min_y = std::clamp(tri_min.y, 0.0f, 1.0f);
    const float tri_max_y = std::clamp(tri_max.y, 0.0f, 1.0f);
    if (tri_max.x <= 0.0f || tri_max.y <= 0.0f || tri_min.x >= 1.0f || tri_min.y >= 1.0f) {
      continue;
    }

    /* The texel range whose centers can fall inside the triangle, clipped to the square. */
    const int x_min = std::max(0, int(std::floor(tri_min_x * float(size))));
    const int x_max = std::min(size - 1, int(std::ceil(tri_max_x * float(size))) - 1);
    const int y_min = std::max(0, int(std::floor(tri_min_y * float(size))));
    const int y_max = std::min(size - 1, int(std::ceil(tri_max_y * float(size))) - 1);
    if (x_min > x_max || y_min > y_max) {
      continue;
    }

    /* Barycentric coordinates of the texel centers, recomputed per texel. */
    const float denominator = (uv1.y - uv2.y) * (uv0.x - uv2.x) +
                              (uv2.x - uv1.x) * (uv0.y - uv2.y);
    if (std::abs(denominator) <= 1e-12f) {
      continue;
    }
    const float inv_denominator = 1.0f / denominator;
    const int v0 = corner_verts[c0];
    const int v1 = corner_verts[c1];
    const int v2 = corner_verts[c2];
    const float3 p0 = positions[v0];
    const float3 p1 = positions[v1];
    const float3 p2 = positions[v2];
    const float3 o0 = has_orco ? orco[v0] : float3(0.0f);
    const float3 o1 = has_orco ? orco[v1] : float3(0.0f);
    const float3 o2 = has_orco ? orco[v2] : float3(0.0f);
    constexpr float edge_epsilon = -1e-6f;

    for (int y = y_min; y <= y_max; y++) {
      const float py = (float(y) + 0.5f) * inv_size;
      for (int x = x_min; x <= x_max; x++) {
        const float px = (float(x) + 0.5f) * inv_size;
        const float w0 = ((uv1.y - uv2.y) * (px - uv2.x) + (uv2.x - uv1.x) * (py - uv2.y)) *
                         inv_denominator;
        const float w1 = ((uv2.y - uv0.y) * (px - uv2.x) + (uv0.x - uv2.x) * (py - uv2.y)) *
                         inv_denominator;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < edge_epsilon || w1 < edge_epsilon || w2 < edge_epsilon) {
          continue;
        }
        const int64_t index = int64_t(y) * size + x;
        dst_positions[index] = p0 * w0 + p1 * w1 + p2 * w2;
        if (has_orco) {
          dst_generated[index] = o0 * w0 + o1 * w1 + o2 * w2;
        }
        dst_valid[index] = 1;
        valid_count++;
        valid_min_x = std::min(valid_min_x, x);
        valid_max_x = std::max(valid_max_x, x);
        valid_min_y = std::min(valid_min_y, y);
        valid_max_y = std::max(valid_max_y, y);
      }
    }
  }

  if (padding_texels > 0 && valid_max_x >= valid_min_x) {
    /* The padding marks texels valid too: they carry a position, and a sampler that reads them gets
     * the island edge instead of nothing. They are part of the count for the same reason. */
    valid_count += surface_pad_islands(dst_positions,
                                       dst_generated,
                                       dst_valid,
                                       size,
                                       size,
                                       padding_texels,
                                       valid_min_x,
                                       valid_min_y,
                                       valid_max_x,
                                       valid_max_y);
  }
  surface->valid_texel_count = valid_count;

  return surface;
}

struct SurfaceCacheEntry {
  /** The object the surface belongs to, so a per-object clear needs no key to be reversible. */
  uint32_t object_session_uid = 0;
  std::shared_ptr<const PaintMaterialCompositeSurface> surface;
};

struct SurfaceCache {
  std::mutex mutex;
  Map<uint64_t, SurfaceCacheEntry> entries;
  /** Keys oldest first, so a full cache drops one entry instead of all of them. */
  Vector<uint64_t> order;
};

SurfaceCache &surface_cache()
{
  static SurfaceCache cache;
  return cache;
}

}  // namespace

std::shared_ptr<const PaintMaterialCompositeSurface> BKE_paint_layers_gradient_surface_get(
    const Object &object, const Mesh &mesh, const char *uv_map_name, const int size)
{
  if (uv_map_name == nullptr || uv_map_name[0] == '\0') {
    return nullptr;
  }
  const int clamped_size = std::clamp(size, surface_size_min, surface_size_max);
  const SurfaceIdentity identity = surface_identity(object, mesh, uv_map_name, clamped_size);

  SurfaceCache &cache = surface_cache();
  {
    std::lock_guard lock(cache.mutex);
    if (const SurfaceCacheEntry *found = cache.entries.lookup_ptr(identity.key)) {
      return found->surface;
    }
  }

  /* Rasterizing is O(UV area), so it runs outside the lock: other objects' lookups never wait on
   * it. Two threads missing the same key build twice and the first insert wins. */
  std::shared_ptr<PaintMaterialCompositeSurface> built = surface_build(mesh,
                                                                       uv_map_name,
                                                                       clamped_size,
                                                                       identity.key,
                                                                       identity.texspace_location,
                                                                       identity.texspace_size,
                                                                       identity.bbox_valid,
                                                                       0,
                                                                       true);
  if (!built) {
    return nullptr;
  }

  std::lock_guard lock(cache.mutex);
  if (const SurfaceCacheEntry *found = cache.entries.lookup_ptr(identity.key)) {
    return found->surface;
  }
  if (cache.entries.size() >= surface_cache_capacity && !cache.order.is_empty()) {
    cache.entries.remove(cache.order.first());
    cache.order.remove(0);
  }
  cache.entries.add(identity.key, SurfaceCacheEntry{object.id.session_uid, built});
  cache.order.append(identity.key);
  return built;
}

std::shared_ptr<const PaintMaterialCompositeSurface> BKE_paint_layers_gradient_surface_build(
    const Object &object,
    const Mesh &mesh,
    const char *uv_map_name,
    const int size,
    const int padding_texels,
    const bool need_generated)
{
  if (uv_map_name == nullptr || uv_map_name[0] == '\0') {
    return nullptr;
  }
  /* Built for the one caller that holds it, for as long as the bake runs: the canvas it follows
   * may be larger than any preview surface, so nothing of it enters the cache. */
  const int clamped_size = std::clamp(size, bake_surface_size_min, bake_surface_size_max);
  const SurfaceIdentity identity = surface_identity(object, mesh, uv_map_name, clamped_size);
  const double time_start = BLI_time_now_seconds();
  std::shared_ptr<PaintMaterialCompositeSurface> built = surface_build(mesh,
                                                                       uv_map_name,
                                                                       clamped_size,
                                                                       identity.key,
                                                                       identity.texspace_location,
                                                                       identity.texspace_size,
                                                                       identity.bbox_valid,
                                                                       padding_texels,
                                                                       need_generated);
  /* A diagnostic for the sizes that make a Merge stall: the rasterization is single-threaded and
   * a 4096 surface is hundreds of MiB next to the bake's own strips. Kept on by default like the
   * other `[paint-layers ...]` traces, and only for the sizes worth measuring. */
  if (built && clamped_size >= 2048) {
    const double seconds = BLI_time_now_seconds() - time_start;
    const double surface_mib = double(built->positions.size()) * double(sizeof(float3)) /
                               (1024.0 * 1024.0);
    const double generated_mib = double(built->generated_positions.size()) *
                                 double(sizeof(float3)) / (1024.0 * 1024.0);
    printf("[paint-layers bake surface] size=%d padding=%d time=%.3fs surface=%.1fMiB "
           "generated=%.1fMiB peak=%.1fMiB\n",
           clamped_size,
           padding_texels,
           seconds,
           surface_mib,
           generated_mib,
           double(MEM_get_peak_memory()) / (1024.0 * 1024.0));
  }
  return built;
}

void BKE_paint_layers_gradient_surface_cache_clear()
{
  SurfaceCache &cache = surface_cache();
  std::lock_guard lock(cache.mutex);
  cache.entries.clear();
  cache.order.clear();
}

void BKE_paint_layers_gradient_surface_cache_clear_for_object(
    const uint32_t object_session_uid)
{
  SurfaceCache &cache = surface_cache();
  std::lock_guard lock(cache.mutex);
  cache.order.remove_if([&](const uint64_t key) {
    return cache.entries.lookup(key).object_session_uid == object_session_uid;
  });
  Vector<uint64_t> keys;
  for (const auto &item : cache.entries.items()) {
    if (item.value.object_session_uid == object_session_uid) {
      keys.append(item.key);
    }
  }
  for (const uint64_t key : keys) {
    cache.entries.remove(key);
  }
}

StringRefNull BKE_paint_layers_gradient_uv_map_name(const Mesh &mesh)
{
  /* `Texture Coordinate.UV` reads the render-active/default map, not the editable active one. */
  const StringRefNull name = mesh.default_uv_map_name();
  return name.is_empty() ? mesh.active_or_default_uv_map_name() : name;
}

bool BKE_paint_layers_gradient_generated_box_get(const Object *object,
                                                 const Mesh &mesh,
                                                 float3 &r_location,
                                                 float3 &r_size)
{
  float texspace_location[3] = {0.0f, 0.0f, 0.0f};
  float texspace_size[3] = {1.0f, 1.0f, 1.0f};
  BKE_mesh_texspace_get(const_cast<Mesh *>(&mesh), texspace_location, texspace_size);
  r_location = float3(texspace_location);
  r_size = float3(texspace_size);

  const bool texspace_usable = std::abs(r_size.x) > 1e-8f || std::abs(r_size.y) > 1e-8f ||
                               std::abs(r_size.z) > 1e-8f;
  if (!texspace_usable) {
    const std::optional<Bounds<float3>> bounds = object != nullptr ?
                                                     BKE_object_boundbox_get(object) :
                                                     std::nullopt;
    if (!bounds) {
      return false;
    }
    r_location = (bounds->min + bounds->max) * 0.5f;
    r_size = (bounds->max - bounds->min) * 0.5f;
    if (std::abs(r_size.x) <= 1e-8f && std::abs(r_size.y) <= 1e-8f &&
        std::abs(r_size.z) <= 1e-8f)
    {
      return false;
    }
  }

  /* A flat or degenerate axis gets a full unit of extent, so a generated coordinate exists. */
  for (const int axis : IndexRange(3)) {
    if (std::abs(r_size[axis]) <= 1e-8f) {
      r_size[axis] = 1.0f;
    }
  }
  return true;
}

bool BKE_paint_layers_gradient_generated_from_object(const Object *object,
                                                     const Mesh &mesh,
                                                     const float3 &position,
                                                     float3 &r_generated)
{
  float3 location;
  float3 size;
  if (!BKE_paint_layers_gradient_generated_box_get(object, mesh, location, size)) {
    return false;
  }
  r_generated = (position - location) / size * 0.5f + 0.5f;
  return true;
}

bool BKE_paint_layers_gradient_generated_to_object(const Object *object,
                                                   const Mesh &mesh,
                                                   const float3 &generated,
                                                   float3 &r_position)
{
  float3 location;
  float3 size;
  if (!BKE_paint_layers_gradient_generated_box_get(object, mesh, location, size)) {
    return false;
  }
  r_position = location + (generated * 2.0f - 1.0f) * size;
  return true;
}

namespace {

/** Barycentric weights of \a point inside the UV triangle \a a, \a b, \a c. */
bool uv_triangle_weights(const float2 &a,
                         const float2 &b,
                         const float2 &c,
                         const float2 &point,
                         float3 &r_weights)
{
  const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
  if (std::abs(denominator) <= 1e-12f) {
    return false;
  }
  r_weights.x = ((b.y - c.y) * (point.x - c.x) + (c.x - b.x) * (point.y - c.y)) / denominator;
  r_weights.y = ((c.y - a.y) * (point.x - c.x) + (a.x - c.x) * (point.y - c.y)) / denominator;
  r_weights.z = 1.0f - r_weights.x - r_weights.y;
  return true;
}

/** The UV map and geometry the two conversions read, or an empty span when either is missing. */
struct GradientMeshUv {
  VArraySpan<float2> uv;
  Span<float3> positions;
  Span<int> corner_verts;
  Span<int3> corner_tris;
  bool valid = false;
};

GradientMeshUv gradient_mesh_uv(const Mesh &mesh)
{
  GradientMeshUv data;
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      BKE_paint_layers_gradient_uv_map_name(mesh), bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return data;
  }
  data.uv = VArraySpan<float2>(*uv_attribute);
  data.positions = mesh.vert_positions();
  data.corner_verts = mesh.corner_verts();
  data.corner_tris = mesh.corner_tris();
  data.valid = !data.uv.is_empty() && !data.positions.is_empty() &&
               !data.corner_verts.is_empty() && !data.corner_tris.is_empty();
  return data;
}

}  // namespace

bool BKE_paint_layers_gradient_uv_to_object(const Mesh &mesh,
                                            const float2 &uv,
                                            float3 &r_object)
{
  const GradientMeshUv data = gradient_mesh_uv(mesh);
  if (!data.valid) {
    return false;
  }

  const auto triangle_position = [&](const int index, const float3 &weights) {
    const int3 tri = data.corner_tris[index];
    return data.positions[data.corner_verts[tri.x]] * weights.x +
           data.positions[data.corner_verts[tri.y]] * weights.y +
           data.positions[data.corner_verts[tri.z]] * weights.z;
  };

  /* Inside an island the exact triangle wins. */
  constexpr float inside_epsilon = -1e-5f;
  for (const int index : IndexRange(data.corner_tris.size())) {
    const int3 tri = data.corner_tris[index];
    float3 weights;
    if (!uv_triangle_weights(
            data.uv[tri.x], data.uv[tri.y], data.uv[tri.z], uv, weights))
    {
      continue;
    }
    if (weights.x >= inside_epsilon && weights.y >= inside_epsilon &&
        weights.z >= inside_epsilon)
    {
      r_object = triangle_position(index, weights);
      return true;
    }
  }

  /* Outside every island: the closest point of the closest triangle, measured in UV space. */
  float best_distance = FLT_MAX;
  int best_index = -1;
  float3 best_weights(0.0f);
  const float3 uv_point(uv.x, uv.y, 0.0f);
  for (const int index : IndexRange(data.corner_tris.size())) {
    const int3 tri = data.corner_tris[index];
    float closest[3];
    closest_on_tri_to_point_v3(closest,
                               uv_point,
                               float3(data.uv[tri.x].x, data.uv[tri.x].y, 0.0f),
                               float3(data.uv[tri.y].x, data.uv[tri.y].y, 0.0f),
                               float3(data.uv[tri.z].x, data.uv[tri.z].y, 0.0f));
    const float distance = math::distance_squared(float3(closest), uv_point);
    float3 weights;
    if (distance >= best_distance ||
        !uv_triangle_weights(data.uv[tri.x],
                             data.uv[tri.y],
                             data.uv[tri.z],
                             float2(closest[0], closest[1]),
                             weights))
    {
      continue;
    }
    best_distance = distance;
    best_index = index;
    best_weights = weights;
  }
  if (best_index < 0) {
    return false;
  }
  r_object = triangle_position(best_index, best_weights);
  return true;
}

bool BKE_paint_layers_gradient_object_to_uv(const Mesh &mesh,
                                            const float3 &object,
                                            float2 &r_uv)
{
  const GradientMeshUv data = gradient_mesh_uv(mesh);
  if (!data.valid) {
    return false;
  }

  float best_distance = FLT_MAX;
  int best_index = -1;
  float3 best_point(0.0f);
  for (const int index : IndexRange(data.corner_tris.size())) {
    const int3 tri = data.corner_tris[index];
    const float3 &p0 = data.positions[data.corner_verts[tri.x]];
    const float3 &p1 = data.positions[data.corner_verts[tri.y]];
    const float3 &p2 = data.positions[data.corner_verts[tri.z]];
    float closest[3];
    closest_on_tri_to_point_v3(closest, object, p0, p1, p2);
    const float distance = math::distance_squared(float3(closest), object);
    if (distance < best_distance) {
      best_distance = distance;
      best_index = index;
      best_point = float3(closest);
    }
  }
  if (best_index < 0) {
    return false;
  }

  const int3 tri = data.corner_tris[best_index];
  const float3 &p0 = data.positions[data.corner_verts[tri.x]];
  const float3 &p1 = data.positions[data.corner_verts[tri.y]];
  const float3 &p2 = data.positions[data.corner_verts[tri.z]];
  float normal[3];
  if (normal_tri_v3(normal, p0, p1, p2) == 0.0f) {
    return false;
  }
  float weights[3];
  interp_weights_tri_v3(weights, p0, p1, p2, best_point);
  r_uv = data.uv[tri.x] * weights[0] + data.uv[tri.y] * weights[1] + data.uv[tri.z] * weights[2];
  return std::isfinite(r_uv.x) && std::isfinite(r_uv.y);
}

}  // namespace blender
