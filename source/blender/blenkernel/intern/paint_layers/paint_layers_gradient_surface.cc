/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Per-object UV-to-object-space surfaces for the 2D Image Editor preview.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "BKE_paint_layers_gradient_surface.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_hash.hh"
#include "BLI_implicit_sharing.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

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
 * The generated-coordinate bounding box, from the same texture space the shader's
 * `Texture Coordinate.Generated` uses when the mesh has no #CD_ORCO layer.
 *
 * A zero extent on an axis is widened to one unit around the center, matching Blender's rule where
 * a flat object still maps to the middle of the generated range instead of dividing by zero.
 *
 * \return false when neither the texture space nor the object bounds give a usable box.
 */
bool generated_bbox_get(const Object &object,
                        const Mesh &mesh,
                        float3 &r_location,
                        float3 &r_size)
{
  float texspace_location[3] = {0.0f, 0.0f, 0.0f};
  float texspace_size[3] = {1.0f, 1.0f, 1.0f};
  BKE_mesh_texspace_get(const_cast<Mesh *>(&mesh), texspace_location, texspace_size);
  r_location = float3(texspace_location[0], texspace_location[1], texspace_location[2]);
  r_size = float3(texspace_size[0], texspace_size[1], texspace_size[2]);

  const bool texspace_usable = std::abs(r_size.x) > 1e-8f || std::abs(r_size.y) > 1e-8f ||
                               std::abs(r_size.z) > 1e-8f;
  if (!texspace_usable) {
    const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(&object);
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
  for (int axis = 0; axis < 3; axis++) {
    if (std::abs(r_size[axis]) <= 1e-8f) {
      r_size[axis] = 1.0f;
    }
  }
  return true;
}

/** \a texspace_location / \a texspace_size and \a bbox_valid are the caller's #generated_bbox_get. */
std::shared_ptr<PaintMaterialCompositeSurface> surface_build(const Object &object,
                                                             const Mesh &mesh,
                                                             const char *uv_map_name,
                                                             const int size,
                                                             const uint64_t key,
                                                             const float3 &texspace_location,
                                                             const float3 &texspace_size,
                                                             const bool bbox_valid)
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
   * pre-deformation position and must not be replaced by the deformed one. */
  const float3 *orco = static_cast<const float3 *>(
      CustomData_get_layer(&mesh.vert_data, CD_ORCO));
  if (orco != nullptr && mesh.verts_num > 0) {
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
    return surface;
  }

  MutableSpan<float3> dst_positions = surface->positions.as_mutable_span();
  MutableSpan<uint8_t> dst_valid = surface->valid.as_mutable_span();
  MutableSpan<float3> dst_generated = surface->generated_positions.as_mutable_span();
  const bool has_orco = !dst_generated.is_empty();
  const float inv_size = 1.0f / float(size);

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
      }
    }
  }

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

  /* The texture space is computed once and shared by the key and the build, so the lookup and the
   * build cannot disagree about it. */
  float3 texspace_location(0.0f);
  float3 texspace_size(1.0f);
  const bool bbox_valid = generated_bbox_get(object, mesh, texspace_location, texspace_size);
  const uint64_t key = surface_key(
      object, mesh, uv_map_name, clamped_size, texspace_location, texspace_size);

  SurfaceCache &cache = surface_cache();
  {
    std::lock_guard lock(cache.mutex);
    if (const SurfaceCacheEntry *found = cache.entries.lookup_ptr(key)) {
      return found->surface;
    }
  }

  /* Rasterizing is O(UV area), so it runs outside the lock: other objects' lookups never wait on
   * it. Two threads missing the same key build twice and the first insert wins. */
  std::shared_ptr<PaintMaterialCompositeSurface> built = surface_build(object,
                                                                       mesh,
                                                                       uv_map_name,
                                                                       clamped_size,
                                                                       key,
                                                                       texspace_location,
                                                                       texspace_size,
                                                                       bbox_valid);
  if (!built) {
    return nullptr;
  }

  std::lock_guard lock(cache.mutex);
  if (const SurfaceCacheEntry *found = cache.entries.lookup_ptr(key)) {
    return found->surface;
  }
  if (cache.entries.size() >= surface_cache_capacity && !cache.order.is_empty()) {
    cache.entries.remove(cache.order.first());
    cache.order.remove(0);
  }
  cache.entries.add(key, SurfaceCacheEntry{object.id.session_uid, built});
  cache.order.append(key);
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

}  // namespace blender
