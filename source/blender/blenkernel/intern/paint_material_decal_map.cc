/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The Decal map: a patch of the surface unfolded around a frame's center, rasterized into the
 * painted object's UV tile. See #PaintMaterialDecalMap.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>

#include "BKE_attribute.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_decal_map.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_surface_unfold.hh"

#include "paint_lru_cache.hh"

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_geom.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_string_ref.hh"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph_query.hh"

#include "paint_material_composite_projection_internal.hh"

namespace blender {

static constexpr int decal_udim_first = 1001;
static constexpr int decal_udim_row_len = 10;
/* The slack of the closed tile-border test and of the inside test of a barycentric weight, the
 * values the surface map's rasterizer uses. */
static constexpr float decal_border_epsilon = 1e-5f;
static constexpr int decal_band_max = 32;

/* Counted so a caller can tell a cache hit from a build by comparing the counts around its call,
 * without the getters growing an out-parameter. */
static std::atomic<int64_t> g_decal_map_builds{0};
static std::atomic<int64_t> g_decal_inverse_builds{0};

void BKE_paint_material_decal_build_counts_get(int64_t &r_map, int64_t &r_inverse)
{
  r_map = g_decal_map_builds.load();
  r_inverse = g_decal_inverse_builds.load();
}

/* -------------------------------------------------------------------- */
/** \name Frames
 * \{ */

/**
 * The frame of one unfolded triangle: the object-space directions of the plane's +X and +Y (from the
 * corners' object-space edges and their plane edges, an isometry, so the pair is orthonormal), laid
 * on the frame's axes. \a flip_v mirrors Y, see #decal_map_build_impl.
 */
static PaintMaterialDecalFrame decal_tri_frame(const bke::SurfaceUnfoldTri &tri,
                                               const int axis,
                                               const int u_index,
                                               const int v_index,
                                               const bool flip_v,
                                               const PaintMaterialDecalParams &params)
{
  PaintMaterialDecalFrame frame;
  const float2 p1 = tri.plane[1] - tri.plane[0];
  const float2 p2 = tri.plane[2] - tri.plane[0];
  const float det = p1.x * p2.y - p1.y * p2.x;
  const float3 e1 = tri.co[1] - tri.co[0];
  const float3 e2 = tri.co[2] - tri.co[0];
  float3 tangent = (e1 * p2.y - e2 * p1.y) / det;
  float3 bitangent = (e2 * p1.x - e1 * p2.x) / det;
  float3 depth = math::cross(tangent, bitangent);
  if (std::abs(det) < 1e-20f || math::length_squared(tangent) < 1e-20f ||
      math::length_squared(bitangent) < 1e-20f || math::length_squared(depth) < 1e-20f)
  {
    /* A degenerate triangle covers no texel; the frame of the seed's own axes keeps the entry sane. */
    for (const int i : IndexRange(3)) {
      copy_v3_v3(frame.axes[i], params.frame_axes[i]);
    }
    return frame;
  }
  tangent = math::normalize(tangent);
  bitangent = math::normalize(bitangent);
  if (flip_v) {
    bitangent = -bitangent;
  }
  /* The projection frame is right-handed (`X x Y = Z`): U x V is the depth axis, up to the sign
   * the two remaining axes' order gives (Y reads X and Z). */
  depth = math::normalize(math::cross(tangent, bitangent)) * ((axis == 1) ? -1.0f : 1.0f);
  copy_v3_v3(frame.axes[u_index], tangent);
  copy_v3_v3(frame.axes[v_index], bitangent);
  copy_v3_v3(frame.axes[axis], depth);
  return frame;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Build
 * \{ */

struct DecalSeed {
  float dist_sq = std::numeric_limits<float>::max();
  int tri = -1;
  float3 point = float3(0.0f);
};

/** The enabled triangle closest to \a center. Ties go to the lower index, so it is deterministic. */
static DecalSeed decal_seed_find(const float3 &center,
                                 const Span<int3> corner_tris,
                                 const Span<int> corner_verts,
                                 const Span<float3> positions,
                                 const Span<bool> tri_enabled)
{
  return threading::parallel_reduce(
      IndexRange(corner_tris.size()),
      2048,
      DecalSeed(),
      [&](const IndexRange range, DecalSeed best) {
        for (const int tri_index : range) {
          if (!tri_enabled.is_empty() && !tri_enabled[tri_index]) {
            continue;
          }
          const int3 tri = corner_tris[tri_index];
          const float3 a = positions[corner_verts[tri[0]]];
          const float3 b = positions[corner_verts[tri[1]]];
          const float3 c = positions[corner_verts[tri[2]]];
          float3 point;
          closest_on_tri_to_point_v3(point, center, a, b, c);
          const float dist_sq = math::distance_squared(point, center);
          if (dist_sq < best.dist_sq) {
            best.dist_sq = dist_sq;
            best.tri = tri_index;
            best.point = point;
          }
        }
        return best;
      },
      [](const DecalSeed &a, const DecalSeed &b) {
        if (a.tri < 0) {
          return b;
        }
        if (b.tri < 0) {
          return a;
        }
        if (b.dist_sq < a.dist_sq || (b.dist_sq == a.dist_sq && b.tri < a.tri)) {
          return b;
        }
        return a;
      });
}

/** A patch triangle ready to rasterize: its tile-local UV corners and the canvas box they cover. */
struct DecalRasterTri {
  int patch_index = 0;
  float uv[3][2] = {};
  int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

static bool decal_map_build_impl(const PaintMaterialDecalParams &params,
                                 const Span<int3> corner_tris,
                                 const Span<int> corner_verts,
                                 const Span<float3> positions,
                                 const Span<float2> uvs,
                                 const Span<bool> tri_enabled,
                                 const bke::SurfaceUnfoldAdjacency &adjacency,
                                 const int width,
                                 const int height,
                                 PaintMaterialDecalMap &r_map)
{
  const double start_time = BLI_time_now_seconds();
  r_map.width = width;
  r_map.height = height;
  r_map.q = Array<float2>();
  r_map.patch_tri = Array<int>();
  r_map.frames.clear();
  r_map.tris.clear();
  r_map.patch_tris_num = 0;
  r_map.truncated = false;
  if (width <= 0 || height <= 0 || corner_tris.is_empty()) {
    return false;
  }

  const int axis = std::clamp(params.axis, 0, 2);
  const int u_index = (axis == 0) ? 1 : 0;
  const int v_index = (axis == 2) ? 1 : 2;

  const DecalSeed seed = decal_seed_find(
      params.center, corner_tris, corner_verts, positions, tri_enabled);
  if (seed.tri < 0) {
    return false;
  }
  const double seed_time = BLI_time_now_seconds();

  /* The unfold's X is the frame's map-U axis laid in the seed's plane, its Y is `N x X` (`N` the
   * corner-order normal). A seed that faces away from the frame (or a mesh whose winding is
   * inverted) would turn the map around Y relative to Planar, which keeps U and V on the frame's
   * axes whichever side it is seen from; flipping V restores that, so a flat surface reads exactly
   * as Planar does. */
  const int3 seed_corners = corner_tris[seed.tri];
  const float3 seed_a = positions[corner_verts[seed_corners[0]]];
  const float3 seed_b = positions[corner_verts[seed_corners[1]]];
  const float3 seed_c = positions[corner_verts[seed_corners[2]]];
  const float3 seed_normal = math::normalize(math::cross(seed_b - seed_a, seed_c - seed_a));
  const float3 tangent = params.frame_axes[u_index];
  float3 plane_x = tangent - seed_normal * math::dot(tangent, seed_normal);
  if (math::length_squared(plane_x) < 1e-12f) {
    plane_x = seed_b - seed_a;
  }
  plane_x = math::normalize(plane_x);
  const bool flip_v = math::dot(math::cross(seed_normal, plane_x), params.frame_axes[v_index]) <
                      0.0f;

  bke::SurfaceUnfoldPatch patch;
  if (!bke::surface_unfold_build(corner_tris,
                                 corner_verts,
                                 positions,
                                 adjacency,
                                 seed.tri,
                                 seed.point,
                                 tangent,
                                 std::max(params.radius, 1e-5f),
                                 std::max(params.max_tris, 1),
                                 patch))
  {
    return false;
  }
  const double unfold_time = BLI_time_now_seconds();
  r_map.patch_tris_num = int(patch.tris.size());
  r_map.truncated = patch.truncated;

  const float2 inv_half(params.half_size.x != 0.0f ? 1.0f / params.half_size.x : 1.0f,
                        params.half_size.y != 0.0f ? 1.0f / params.half_size.y : 1.0f);
  const float v_sign = flip_v ? -1.0f : 1.0f;
  Array<std::array<float2, 3>> tri_q(patch.tris.size());
  r_map.frames.reserve(patch.tris.size());
  for (const int64_t i : patch.tris.index_range()) {
    const bke::SurfaceUnfoldTri &tri = patch.tris[i];
    for (const int k : IndexRange(3)) {
      tri_q[i][k] = float2(tri.plane[k].x * inv_half.x, tri.plane[k].y * v_sign * inv_half.y);
    }
    r_map.frames.append(decal_tri_frame(tri, axis, u_index, v_index, flip_v, params));
  }

  /* The table the effect-space resampler reads: the corners in both spaces and the bases a
   * tangent-space Normal is carried through. `in_canvas` is set below, where the triangle is laid. */
  r_map.tris.resize(patch.tris.size());
  for (const int64_t i : patch.tris.index_range()) {
    const bke::SurfaceUnfoldTri &tri = patch.tris[i];
    PaintMaterialDecalTri &entry = r_map.tris[i];
    const int3 corners = corner_tris[tri.tri_index];
    for (const int k : IndexRange(3)) {
      entry.q[k] = tri_q[i][k];
      entry.uv[k] = (corners[k] >= 0 && corners[k] < uvs.size()) ? uvs[corners[k]] : float2(0.0f);
    }
    entry.plane_x = float3(r_map.frames[i].axes[u_index]);
    entry.plane_y = float3(r_map.frames[i].axes[v_index]);
    const float3 e1 = tri.co[1] - tri.co[0];
    const float3 e2 = tri.co[2] - tri.co[0];
    const float3 face_normal = math::cross(e1, e2);
    entry.normal = math::length_squared(face_normal) > 1e-20f ? math::normalize(face_normal) :
                                                                float3(r_map.frames[i].axes[axis]);
    const float2 duv1 = entry.uv[1] - entry.uv[0];
    const float2 duv2 = entry.uv[2] - entry.uv[0];
    const float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (std::abs(det) > 1e-12f) {
      /* The true UV gradient, so a mirrored island keeps its bitangent's direction: the basis the
       * map's tangent-space vectors are stored in. */
      const float3 tangent = (e1 * duv2.y - e2 * duv1.y) / det;
      const float3 bitangent = (e2 * duv1.x - e1 * duv2.x) / det;
      if (math::length_squared(tangent) > 1e-20f && math::length_squared(bitangent) > 1e-20f) {
        entry.tangent = math::normalize(tangent);
        entry.bitangent = math::normalize(bitangent);
      }
    }
  }

  /* One bake is one UDIM tile; a triangle crossing the tile border is left out, as the surface
   * map's rasterizer does. */
  const int tile = (params.tile_number >= decal_udim_first) ? params.tile_number : decal_udim_first;
  const int tile_x = (tile - decal_udim_first) % decal_udim_row_len;
  const int tile_y = (tile - decal_udim_first) / decal_udim_row_len;
  const auto in_tile = [&](const float2 &uv) {
    return uv.x >= float(tile_x) - decal_border_epsilon &&
           uv.x <= float(tile_x + 1) + decal_border_epsilon &&
           uv.y >= float(tile_y) - decal_border_epsilon &&
           uv.y <= float(tile_y + 1) + decal_border_epsilon;
  };
  /* The table's UVs are tile-local, the space the layer's map of this tile is read in. */
  for (PaintMaterialDecalTri &entry : r_map.tris) {
    for (const int k : IndexRange(3)) {
      entry.uv[k] -= float2(float(tile_x), float(tile_y));
    }
  }

  Vector<DecalRasterTri> raster_tris;
  raster_tris.reserve(patch.tris.size());
  for (const int64_t i : patch.tris.index_range()) {
    const int tri_index = patch.tris[i].tri_index;
    if (!tri_enabled.is_empty() && !tri_enabled[tri_index]) {
      continue;
    }
    const int3 corners = corner_tris[tri_index];
    float2 uv[3];
    bool inside = true;
    for (const int k : IndexRange(3)) {
      if (corners[k] < 0 || corners[k] >= uvs.size() || !in_tile(uvs[corners[k]])) {
        inside = false;
        break;
      }
      uv[k] = uvs[corners[k]] - float2(float(tile_x), float(tile_y));
    }
    if (!inside) {
      continue;
    }
    const float min_u = std::min({uv[0].x, uv[1].x, uv[2].x});
    const float max_u = std::max({uv[0].x, uv[1].x, uv[2].x});
    const float min_v = std::min({uv[0].y, uv[1].y, uv[2].y});
    const float max_v = std::max({uv[0].y, uv[1].y, uv[2].y});
    if (max_u < 0.0f || max_v < 0.0f || min_u >= 1.0f || min_v >= 1.0f) {
      continue;
    }
    DecalRasterTri raster;
    raster.patch_index = int(i);
    for (const int k : IndexRange(3)) {
      raster.uv[k][0] = uv[k].x;
      raster.uv[k][1] = uv[k].y;
    }
    raster.x0 = clamp_i(int(floorf(min_u * float(width))), 0, width - 1);
    raster.x1 = clamp_i(int(ceilf(max_u * float(width))), 0, width - 1);
    raster.y0 = clamp_i(int(floorf(min_v * float(height))), 0, height - 1);
    raster.y1 = clamp_i(int(ceilf(max_v * float(height))), 0, height - 1);
    raster_tris.append(raster);
    r_map.tris[i].in_canvas = true;
  }

  const int64_t texels_num = int64_t(width) * height;
  r_map.q = Array<float2>(texels_num, float2(0.0f));
  r_map.patch_tri = Array<int>(texels_num, -1);
  Array<uint8_t> island(texels_num, uint8_t(0));
  const int band_num = std::min(decal_band_max, height);
  const int band_rows = (height + band_num - 1) / band_num;
  Array<int4> band_boxes(band_num, int4(width, height, -1, -1));

  /* Bands own whole canvas rows, so two tasks never write the same texel. Every band walks the
   * patch in its order, so where triangles overlap (a saddle, or UV islands that share space) the
   * first placement wins, the unfold's own rule. */
  threading::parallel_for(IndexRange(band_num), 1, [&](const IndexRange bands) {
    for (const int band : bands) {
      const int band_y0 = band * band_rows;
      const int band_y1 = std::min(band_y0 + band_rows, height) - 1;
      int4 &band_box = band_boxes[band];
      for (const DecalRasterTri &raster : raster_tris) {
        const int y0 = std::max(raster.y0, band_y0);
        const int y1 = std::min(raster.y1, band_y1);
        if (y0 > y1) {
          continue;
        }
        const std::array<float2, 3> &corner_q = tri_q[raster.patch_index];
        for (int y = y0; y <= y1; y++) {
          for (int x = raster.x0; x <= raster.x1; x++) {
            const int64_t index = int64_t(y) * width + x;
            if (r_map.patch_tri[index] >= 0) {
              continue;
            }
            const float co[2] = {(float(x) + 0.5f) / float(width),
                                 (float(y) + 0.5f) / float(height)};
            float w01[3];
            barycentric_weights_v2(raster.uv[0], raster.uv[1], raster.uv[2], co, w01);
            const float w0 = w01[0];
            const float w1 = w01[1];
            const float w2 = 1.0f - w0 - w1;
            if (!std::isfinite(w0) || !std::isfinite(w1) || !std::isfinite(w2)) {
              continue;
            }
            if (w0 < -decal_border_epsilon || w1 < -decal_border_epsilon ||
                w2 < -decal_border_epsilon)
            {
              continue;
            }
            r_map.patch_tri[index] = raster.patch_index;
            r_map.q[index] = corner_q[0] * w0 + corner_q[1] * w1 + corner_q[2] * w2;
            island[index] = 1;
            band_box = int4(std::min(band_box.x, x),
                            std::min(band_box.y, y),
                            std::max(band_box.z, x),
                            std::max(band_box.w, y));
          }
        }
      }
    }
  });

  /* The surface map's padding rule: the island's border grows by the texels bilinear filtering and
   * the first mip levels reach, so a seam texel never blends with an empty one. */
  int2 box_min(width, height);
  int2 box_max(-1, -1);
  for (const int4 &box : band_boxes) {
    if (box.z < box.x) {
      continue;
    }
    box_min = math::min(box_min, int2(box.x, box.y));
    box_max = math::max(box_max, int2(box.z, box.w));
  }
  if (box_max.x >= box_min.x) {
    BKE_paint_composite_pad_islands(
        island,
        width,
        height,
        BKE_paint_composite_island_padding(std::max(width, height)),
        box_min,
        box_max,
        [&](const int64_t dst, const int64_t src) {
          r_map.q[dst] = r_map.q[src];
          r_map.patch_tri[dst] = r_map.patch_tri[src];
        });
  }

  const double end_time = BLI_time_now_seconds();
  r_map.build_seconds = float(end_time - start_time);
  /* On by default, like the other builds' measurements: a Decal drag rebuilds this per step. */
  bke::paint_perf::printf_always(
      "decal_map build %.3f ms | %dx%d tile %d patch %d tris%s raster %d tris | %.1f MiB "
      "(seed %.2f, unfold %.2f, raster+pad %.2f ms)\n",
      (end_time - start_time) * 1000.0,
      width,
      height,
      tile,
      r_map.patch_tris_num,
      r_map.truncated ? " (truncated)" : "",
      int(raster_tris.size()),
      double(r_map.bytes()) / (1024.0 * 1024.0),
      (seed_time - start_time) * 1000.0,
      (unfold_time - seed_time) * 1000.0,
      (end_time - unfold_time) * 1000.0);
  return true;
}

bool BKE_paint_material_decal_map_build(const PaintMaterialDecalParams &params,
                                        const Span<int3> corner_tris,
                                        const Span<int> corner_verts,
                                        const Span<float3> positions,
                                        const Span<float2> uvs,
                                        const Span<bool> tri_enabled,
                                        const int width,
                                        const int height,
                                        PaintMaterialDecalMap &r_map)
{
  const bke::SurfaceUnfoldAdjacency adjacency = bke::surface_unfold_adjacency_build(corner_tris,
                                                                                    corner_verts);
  return decal_map_build_impl(
      params, corner_tris, corner_verts, positions, uvs, tri_enabled, adjacency, width, height, r_map);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache
 * \{ */

/** What the store holds in sum: a 4K map is about 200 MiB, so a few frames of a drag fit. */
constexpr size_t DECAL_MAP_CACHE_BYTES = size_t(768) * 1024 * 1024;

/** The adjacency is per topology and costs a sort of every edge: kept across the steps of a drag. */
struct DecalAdjacencyKey {
  uint32_t object_uid = 0;
  uint64_t mesh_hash = 0;
  int tris_num = 0;

  bool operator==(const DecalAdjacencyKey &other) const
  {
    return object_uid == other.object_uid && mesh_hash == other.mesh_hash &&
           tris_num == other.tris_num;
  }
};

struct DecalAdjacencyValue {
  std::shared_ptr<const bke::SurfaceUnfoldAdjacency> adjacency;

  /** Unbudgeted: the entry cap alone bounds this store. */
  size_t bytes() const
  {
    return 0;
  }
};

static bke::LruByteCache<PaintMaterialDecalMapKey, PaintMaterialDecalMap> g_decal_map_cache(
    DECAL_MAP_CACHE_BYTES);
/* Four topologies: a drag moves no geometry, and a rebuilt mesh drops the old one. */
static bke::LruByteCache<DecalAdjacencyKey, DecalAdjacencyValue> g_decal_adjacency_cache(
    std::numeric_limits<size_t>::max(), 4);

/** The slots of \a object that hold \a material (every slot when null), by original ID. */
static int decal_slots_match(const Object &object,
                             const Material *material,
                             MutableSpan<bool> r_matches)
{
  const Material *material_orig = (material != nullptr) ? DEG_get_original(material) : nullptr;
  int matched = 0;
  for (int slot = 0; slot < object.totcol; slot++) {
    const Material *slot_material = BKE_object_material_get(const_cast<Object *>(&object),
                                                            short(slot + 1));
    if (material_orig == nullptr ||
        (slot_material != nullptr && DEG_get_original(slot_material) == material_orig))
    {
      r_matches[slot] = true;
      matched++;
    }
  }
  return matched;
}

std::shared_ptr<const PaintMaterialDecalMap> BKE_paint_material_decal_map_get(
    const PaintMaterialCompositeProjection &projection,
    const int width,
    const int height,
    const float radius_scale)
{
  const Object *object = projection.object;
  const Mesh *mesh_ptr = composite_projection_mesh(object);
  if (mesh_ptr == nullptr || width <= 0 || height <= 0 || object->totcol <= 0) {
    return nullptr;
  }
  const Mesh &mesh = *mesh_ptr;
  const StringRefNull uv_name = BKE_paint_layers_gradient_uv_map_name(mesh);
  if (uv_name.is_empty()) {
    return nullptr;
  }
  const bke::AttributeAccessor attributes = mesh.attributes();
  const auto uv_attr = attributes.lookup<float2>(uv_name.c_str(), bke::AttrDomain::Corner);
  if (!uv_attr) {
    return nullptr;
  }
  const VArraySpan<float2> uvs(*uv_attr);
  Array<bool> slot_matches(object->totcol, false);
  if (decal_slots_match(*object, projection.material, slot_matches) == 0) {
    return nullptr;
  }
  const VArraySpan<int> material_indices = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);
  const Span<int3> corner_tris = mesh.corner_tris();
  if (corner_tris.is_empty()) {
    return nullptr;
  }

  /* The frame in the painted object's space; the controller is ignored by design. */
  PaintMaterialDecalParams params;
  float frame_rot[3][3];
  eul_to_mat3(frame_rot, projection.rotation);
  for (const int i : IndexRange(3)) {
    params.frame_axes[i] = float3(frame_rot[i]);
  }
  params.axis = std::clamp(int(projection.axis), 0, 2);
  const int u_index = (params.axis == 0) ? 1 : 0;
  const int v_index = (params.axis == 2) ? 1 : 2;
  params.center = float3(projection.center);
  params.half_size = float2(
      BKE_paint_layers_projection_size_normalize(projection.size[u_index]) * 0.5f,
      BKE_paint_layers_projection_size_normalize(projection.size[v_index]) * 0.5f);
  /* The aspect fit shrinks the footprint along an axis whose factor is above one and grows it where
   * it is below: the patch has to reach the footprint's larger reach. */
  const float aspect_u = std::max(std::abs(projection.aspect[u_index]), 1e-3f);
  const float aspect_v = std::max(std::abs(projection.aspect[v_index]), 1e-3f);
  params.radius = std::max(std::abs(params.half_size.x) / aspect_u,
                           std::abs(params.half_size.y) / aspect_v) *
                  std::max(radius_scale, 1e-3f);
  if (projection.patch_radius > 0.0f) {
    /* A frame derived from a UV point has a size that is the UV's scale, not the patch's reach: its
     * radius is the object's own and independent of the frame's extents. */
    params.radius = projection.patch_radius;
  }
  params.max_tris = PAINT_MATERIAL_DECAL_MAX_TRIS;
  params.tile_number = projection.tile_number;

  /* No pointers: the evaluated and the original copy of one object find the same entry. */
  PaintMaterialDecalMapKey key;
  key.object_uid = object->id.session_uid;
  key.tile_number = projection.tile_number;
  key.width = width;
  key.height = height;
  key.verts_num = mesh.verts_num;
  key.faces_num = mesh.faces_num;
  key.corners_num = mesh.corners_num;
  uint64_t mesh_hash = composite_mesh_content_hash(mesh);
  for (const bool match : slot_matches) {
    mesh_hash = get_default_hash(mesh_hash, match);
  }
  key.mesh_hash = mesh_hash;
  uint64_t frame_hash = get_default_hash(params.center.x, params.center.y, params.center.z);
  for (const int i : IndexRange(3)) {
    frame_hash = get_default_hash(
        frame_hash, params.frame_axes[i].x, params.frame_axes[i].y, params.frame_axes[i].z);
  }
  frame_hash = get_default_hash(
      frame_hash, params.half_size.x, params.half_size.y, params.radius, params.axis);
  frame_hash = get_default_hash(frame_hash, params.max_tris);
  key.frame_hash = frame_hash;

  /* The build runs outside the store's lock: callers on several threads (the bake of each channel,
   * the previews) ask for the same map at once, and the store lets one build while the others wait
   * for its result. */
  return g_decal_map_cache.get_or_build(
      key, [&]() -> std::shared_ptr<const PaintMaterialDecalMap> {
        const uint64_t topology_hash = composite_mesh_content_hash(mesh);
        const DecalAdjacencyKey adjacency_key{
            key.object_uid, topology_hash, int(corner_tris.size())};
        const std::shared_ptr<const bke::SurfaceUnfoldAdjacency> adjacency =
            g_decal_adjacency_cache
                .get_or_build(adjacency_key,
                              [&]() -> std::shared_ptr<const DecalAdjacencyValue> {
                                auto value = std::make_shared<DecalAdjacencyValue>();
                                value->adjacency =
                                    std::make_shared<const bke::SurfaceUnfoldAdjacency>(
                                        bke::surface_unfold_adjacency_build(corner_tris,
                                                                            mesh.corner_verts()));
                                return value;
                              })
                ->adjacency;

        /* A triangle whose face does not use the row's material is crossed by the patch but not
         * laid. */
        Array<int> tri_faces(corner_tris.size());
        bke::mesh::corner_tris_calc_face_indices(mesh.faces(), tri_faces);
        Array<bool> tri_enabled(corner_tris.size(), false);
        for (const int64_t i : corner_tris.index_range()) {
          const int material_index = material_indices[tri_faces[i]];
          tri_enabled[i] = material_index >= 0 && material_index < object->totcol &&
                           slot_matches[material_index];
        }

        auto map = std::make_shared<PaintMaterialDecalMap>();
        map->key = key;
        if (!decal_map_build_impl(params,
                                  corner_tris,
                                  mesh.corner_verts(),
                                  mesh.vert_positions(),
                                  uvs,
                                  tri_enabled,
                                  *adjacency,
                                  width,
                                  height,
                                  *map))
        {
          return std::shared_ptr<const PaintMaterialDecalMap>(nullptr);
        }
        g_decal_map_builds++;
        return std::shared_ptr<const PaintMaterialDecalMap>(std::move(map));
      });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Inverse Map
 * \{ */

/** The barycentric weights of \a p in the 2D triangle \a a, \a b, \a c; false when degenerate. */
static bool decal_barycentric(
    const float2 &a, const float2 &b, const float2 &c, const float2 &p, float3 &r_w)
{
  const float2 v0 = b - a;
  const float2 v1 = c - a;
  const float2 v2 = p - a;
  const float det = v0.x * v1.y - v0.y * v1.x;
  if (std::abs(det) < 1e-20f) {
    return false;
  }
  const float w1 = (v2.x * v1.y - v2.y * v1.x) / det;
  const float w2 = (v0.x * v2.y - v0.y * v2.x) / det;
  r_w = float3(1.0f - w1 - w2, w1, w2);
  return std::isfinite(r_w.x) && std::isfinite(r_w.y) && std::isfinite(r_w.z);
}

/**
 * How far \a p lies outside the triangle, along the outward normal of the edge it is furthest
 * past; zero inside. In the units of the triangle's own space.
 */
static float decal_outside_distance(const float2 corners[3], const float2 &p)
{
  const float2 &a = corners[0];
  const float2 &b = corners[1];
  const float2 &c = corners[2];
  const float orientation = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  const float sign = orientation < 0.0f ? -1.0f : 1.0f;
  float outside = 0.0f;
  for (const int k : IndexRange(3)) {
    const float2 &e0 = corners[k];
    const float2 &e1 = corners[(k + 1) % 3];
    const float2 edge = e1 - e0;
    const float length = math::length(edge);
    if (length < 1e-20f) {
      continue;
    }
    /* Positive on the inner side for a counter-clockwise triangle; the sign turns a clockwise one
     * (a mirrored island) the same way. */
    const float inner = sign * (edge.x * (p.y - e0.y) - edge.y * (p.x - e0.x)) / length;
    outside = std::max(outside, -inner);
  }
  return outside;
}

bool BKE_paint_material_decal_inverse_map_build(const PaintMaterialDecalMap &map,
                                                const int resolution,
                                                PaintMaterialDecalInverseMap &r_inverse)
{
  const double start_time = BLI_time_now_seconds();
  r_inverse.key = map.key;
  r_inverse.resolution = resolution;
  r_inverse.width = 0;
  r_inverse.height = 0;
  r_inverse.cell_tri = Array<int>();
  if (resolution <= 0) {
    return false;
  }
  float2 q_min(std::numeric_limits<float>::max());
  float2 q_max(-std::numeric_limits<float>::max());
  int laid_num = 0;
  for (const PaintMaterialDecalTri &tri : map.tris) {
    if (!tri.in_canvas) {
      continue;
    }
    laid_num++;
    for (const int k : IndexRange(3)) {
      q_min = math::min(q_min, tri.q[k]);
      q_max = math::max(q_max, tri.q[k]);
    }
  }
  if (laid_num == 0) {
    return false;
  }
  const float2 extent = math::max(q_max - q_min, float2(1e-6f));
  const float longer = std::max(extent.x, extent.y);
  const int width = std::max(1, int(std::ceil(float(resolution) * extent.x / longer)));
  const int height = std::max(1, int(std::ceil(float(resolution) * extent.y / longer)));
  r_inverse.q_min = q_min;
  r_inverse.q_max = q_min + extent;
  r_inverse.width = width;
  r_inverse.height = height;
  r_inverse.cell_tri = Array<int>(int64_t(width) * height, -1);
  const float2 cell_size(extent.x / float(width), extent.y / float(height));

  /* Bands own whole rows, so two tasks never write one cell; each walks the patch in its
   * breadth-first order, so where the unfold overlaps itself the first placement wins, as in the
   * forward map. */
  const int band_num = std::min(decal_band_max, height);
  const int band_rows = (height + band_num - 1) / band_num;
  threading::parallel_for(IndexRange(band_num), 1, [&](const IndexRange bands) {
    for (const int band : bands) {
      const int band_y0 = band * band_rows;
      const int band_y1 = std::min(band_y0 + band_rows, height) - 1;
      for (const int64_t tri_index : map.tris.index_range()) {
        const PaintMaterialDecalTri &tri = map.tris[tri_index];
        if (!tri.in_canvas) {
          continue;
        }
        const float2 lo = math::min(math::min(tri.q[0], tri.q[1]), tri.q[2]);
        const float2 hi = math::max(math::max(tri.q[0], tri.q[1]), tri.q[2]);
        const int x0 = std::clamp(int(std::floor((lo.x - q_min.x) / cell_size.x)), 0, width - 1);
        const int x1 = std::clamp(int(std::floor((hi.x - q_min.x) / cell_size.x)), 0, width - 1);
        const int y0 = std::max(
            std::clamp(int(std::floor((lo.y - q_min.y) / cell_size.y)), 0, height - 1), band_y0);
        const int y1 = std::min(
            std::clamp(int(std::floor((hi.y - q_min.y) / cell_size.y)), 0, height - 1), band_y1);
        for (int y = y0; y <= y1; y++) {
          for (int x = x0; x <= x1; x++) {
            const int64_t index = int64_t(y) * width + x;
            if (r_inverse.cell_tri[index] >= 0) {
              continue;
            }
            const float2 center = q_min + float2((float(x) + 0.5f) * cell_size.x,
                                                 (float(y) + 0.5f) * cell_size.y);
            float3 w;
            if (!decal_barycentric(tri.q[0], tri.q[1], tri.q[2], center, w)) {
              continue;
            }
            if (w.x < -decal_border_epsilon || w.y < -decal_border_epsilon ||
                w.z < -decal_border_epsilon)
            {
              continue;
            }
            r_inverse.cell_tri[index] = int(tri_index);
          }
        }
      }
    }
  });
  /* A triangle smaller than a cell can miss every cell center: its centroid's cell takes it when
   * nothing else did, so the reader's neighbour search can still find it. */
  for (const int64_t tri_index : map.tris.index_range()) {
    const PaintMaterialDecalTri &tri = map.tris[tri_index];
    if (!tri.in_canvas) {
      continue;
    }
    const float2 centroid = (tri.q[0] + tri.q[1] + tri.q[2]) / 3.0f;
    const int x = std::clamp(int(std::floor((centroid.x - q_min.x) / cell_size.x)), 0, width - 1);
    const int y = std::clamp(
        int(std::floor((centroid.y - q_min.y) / cell_size.y)), 0, height - 1);
    int &cell = r_inverse.cell_tri[int64_t(y) * width + x];
    if (cell < 0) {
      cell = int(tri_index);
    }
  }

  const double end_time = BLI_time_now_seconds();
  r_inverse.build_seconds = float(end_time - start_time);
  /* On by default like the forward build's: a frame drag rebuilds both per step. */
  bke::paint_perf::printf_always(
      "decal_inverse_map build %.3f ms | %dx%d cells (res %d) | %d tris | %.1f MiB\n",
      (end_time - start_time) * 1000.0,
      width,
      height,
      resolution,
      laid_num,
      double(r_inverse.bytes()) / (1024.0 * 1024.0));
  return true;
}

bool BKE_paint_material_decal_inverse_lookup(const PaintMaterialDecalMap &map,
                                             const PaintMaterialDecalInverseMap &inverse,
                                             const float2 &q,
                                             float2 &r_uv,
                                             int &r_tri)
{
  if (inverse.width <= 0 || inverse.height <= 0) {
    return false;
  }
  const float2 extent = inverse.q_max - inverse.q_min;
  const float2 cell_size(extent.x / float(inverse.width), extent.y / float(inverse.height));
  const int cx = int(std::floor((q.x - inverse.q_min.x) / cell_size.x));
  const int cy = int(std::floor((q.y - inverse.q_min.y) / cell_size.y));
  /* One cell of slack around the bounds: the border extrapolation below reaches that far. */
  if (cx < -1 || cy < -1 || cx > inverse.width || cy > inverse.height) {
    return false;
  }
  /* The cell keeps one triangle; on a dense patch \a q may sit in a neighbour of it, so the eight
   * neighbours' triangles are tried too. The first containing one wins; otherwise the closest one
   * within a cell extrapolates, which keeps the patch border from fraying by a cell. */
  int best_tri = -1;
  float best_outside = std::numeric_limits<float>::max();
  const float tolerance = math::length(cell_size);
  /* The center cell first: its triangle is the likeliest. */
  constexpr int offsets[3] = {0, -1, 1};
  for (const int oy : offsets) {
    for (const int ox : offsets) {
      const int x = cx + ox;
      const int y = cy + oy;
      if (x < 0 || y < 0 || x >= inverse.width || y >= inverse.height) {
        continue;
      }
      const int tri_index = inverse.cell_tri[int64_t(y) * inverse.width + x];
      if (tri_index < 0 || tri_index == best_tri) {
        continue;
      }
      const float outside = decal_outside_distance(map.tris[tri_index].q, q);
      if (outside < best_outside) {
        best_outside = outside;
        best_tri = tri_index;
        if (outside <= 0.0f) {
          break;
        }
      }
    }
    if (best_outside <= 0.0f) {
      break;
    }
  }
  if (best_tri < 0 || best_outside > tolerance) {
    return false;
  }
  const PaintMaterialDecalTri &tri = map.tris[best_tri];
  float3 w;
  if (!decal_barycentric(tri.q[0], tri.q[1], tri.q[2], q, w)) {
    return false;
  }
  r_uv = tri.uv[0] * w.x + tri.uv[1] * w.y + tri.uv[2] * w.z;
  r_tri = best_tri;
  return true;
}

bool BKE_paint_material_decal_forward_lookup(const PaintMaterialDecalMap &map,
                                             const float2 &uv,
                                             float2 &r_q,
                                             int &r_tri)
{
  if (map.width <= 0 || map.height <= 0 || map.patch_tri.is_empty()) {
    return false;
  }
  const float2 wrapped(uv.x - std::floor(uv.x), uv.y - std::floor(uv.y));
  const int x = std::clamp(int(wrapped.x * float(map.width)), 0, map.width - 1);
  const int y = std::clamp(int(wrapped.y * float(map.height)), 0, map.height - 1);
  const int tri_index = map.patch_tri[int64_t(y) * map.width + x];
  if (tri_index < 0) {
    return false;
  }
  if (tri_index >= map.tris.size()) {
    r_q = map.q[int64_t(y) * map.width + x];
    r_tri = tri_index;
    return true;
  }
  const PaintMaterialDecalTri &tri = map.tris[tri_index];
  float3 w;
  if (!decal_barycentric(tri.uv[0], tri.uv[1], tri.uv[2], wrapped, w)) {
    r_q = map.q[int64_t(y) * map.width + x];
  }
  else {
    r_q = tri.q[0] * w.x + tri.q[1] * w.y + tri.q[2] * w.z;
  }
  r_tri = tri_index;
  return true;
}

/** Inverses are small next to their forward maps (an int per cell, at most 2048 squared). */
constexpr int DECAL_INVERSE_CACHE_ENTRIES = 4;

struct DecalInverseKey {
  PaintMaterialDecalMapKey map_key;
  int resolution = 0;

  bool operator==(const DecalInverseKey &other) const
  {
    return map_key == other.map_key && resolution == other.resolution;
  }
};

static bke::LruByteCache<DecalInverseKey, PaintMaterialDecalInverseMap> g_decal_inverse_cache(
    std::numeric_limits<size_t>::max(), DECAL_INVERSE_CACHE_ENTRIES);

std::shared_ptr<const PaintMaterialDecalInverseMap> BKE_paint_material_decal_inverse_map_get(
    const PaintMaterialDecalMap &map, const int resolution)
{
  return g_decal_inverse_cache.get_or_build(
      DecalInverseKey{map.key, resolution},
      [&]() -> std::shared_ptr<const PaintMaterialDecalInverseMap> {
        auto inverse = std::make_shared<PaintMaterialDecalInverseMap>();
        if (!BKE_paint_material_decal_inverse_map_build(map, resolution, *inverse)) {
          return nullptr;
        }
        g_decal_inverse_builds++;
        return std::shared_ptr<const PaintMaterialDecalInverseMap>(std::move(inverse));
      });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame From UV
 * \{ */

struct DecalUvHit {
  float distance = std::numeric_limits<float>::max();
  int tri = -1;
};

bool BKE_paint_material_decal_frame_from_uv_mesh(const Span<int3> corner_tris,
                                                 const Span<int> corner_verts,
                                                 const Span<float3> positions,
                                                 const Span<float2> uvs,
                                                 const Span<bool> tri_enabled,
                                                 const int tile_number,
                                                 const float2 &uv,
                                                 PaintMaterialDecalUvFrame &r_frame)
{
  if (corner_tris.is_empty() || positions.is_empty()) {
    return false;
  }
  const int tile = (tile_number >= decal_udim_first) ? tile_number : decal_udim_first;
  const float2 point = uv + float2(float((tile - decal_udim_first) % decal_udim_row_len),
                                   float((tile - decal_udim_first) / decal_udim_row_len));
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    return false;
  }

  const auto tri_uvs = [&](const int tri_index, float2 r_uv[3]) {
    for (const int k : IndexRange(3)) {
      const int corner = corner_tris[tri_index][k];
      if (corner < 0 || corner >= uvs.size()) {
        return false;
      }
      r_uv[k] = uvs[corner];
    }
    return true;
  };

  /* The triangle that holds the UV, else the nearest one: a Mirror center dragged into an empty
   * place of the tile still has a frame, on the closest part of the surface. Ties go to the lower
   * index, so the frame is deterministic. */
  const DecalUvHit hit = threading::parallel_reduce(
      IndexRange(corner_tris.size()),
      2048,
      DecalUvHit(),
      [&](const IndexRange range, DecalUvHit best) {
        for (const int tri_index : range) {
          if (!tri_enabled.is_empty() && !tri_enabled[tri_index]) {
            continue;
          }
          float2 corners[3];
          float3 weights;
          if (!tri_uvs(tri_index, corners) ||
              !decal_barycentric(corners[0], corners[1], corners[2], point, weights))
          {
            continue;
          }
          const float distance = decal_outside_distance(corners, point);
          if (distance < best.distance) {
            best.distance = distance;
            best.tri = tri_index;
          }
        }
        return best;
      },
      [](const DecalUvHit &a, const DecalUvHit &b) {
        if (a.tri < 0) {
          return b;
        }
        if (b.tri < 0) {
          return a;
        }
        if (b.distance < a.distance || (b.distance == a.distance && b.tri < a.tri)) {
          return b;
        }
        return a;
      });
  if (hit.tri < 0) {
    return false;
  }

  float2 uv_corners[3];
  tri_uvs(hit.tri, uv_corners);
  float3 weights;
  if (!decal_barycentric(uv_corners[0], uv_corners[1], uv_corners[2], point, weights)) {
    return false;
  }
  /* A point past the triangle takes the clamped weights: its nearest place on the surface. */
  weights = math::max(weights, float3(0.0f));
  const float weight_sum = weights.x + weights.y + weights.z;
  if (weight_sum < 1e-12f) {
    return false;
  }
  weights /= weight_sum;

  const int3 corners = corner_tris[hit.tri];
  const float3 co0 = positions[corner_verts[corners[0]]];
  const float3 co1 = positions[corner_verts[corners[1]]];
  const float3 co2 = positions[corner_verts[corners[2]]];
  const float2 duv1 = uv_corners[1] - uv_corners[0];
  const float2 duv2 = uv_corners[2] - uv_corners[0];
  const float det = duv1.x * duv2.y - duv1.y * duv2.x;
  const float3 e1 = co1 - co0;
  const float3 e2 = co2 - co0;
  /* The surface's derivatives per UV unit: their lengths are the surface length of one UV unit. */
  const float3 dp_du = (e1 * duv2.y - e2 * duv1.y) / det;
  const float3 dp_dv = (e2 * duv1.x - e1 * duv2.x) / det;
  const float length_u = math::length(dp_du);
  if (!(length_u > 1e-12f) || !std::isfinite(length_u)) {
    return false;
  }
  const float3 axis_u = dp_du / length_u;
  /* Orthogonalized, so the frame is a rotation; the bitangent keeps its direction, so a mirrored
   * island keeps V where the UV has it. */
  const float3 v_rest = dp_dv - axis_u * math::dot(axis_u, dp_dv);
  const float length_v_rest = math::length(v_rest);
  if (!(length_v_rest > 1e-12f) || !std::isfinite(length_v_rest)) {
    return false;
  }
  const float3 axis_v = v_rest / length_v_rest;
  const float3 axis_n = math::cross(axis_u, axis_v);

  float rot[3][3];
  copy_v3_v3(rot[0], axis_u);
  copy_v3_v3(rot[1], axis_v);
  copy_v3_v3(rot[2], axis_n);
  mat3_to_eul(r_frame.rotation, rot);
  r_frame.center = co0 * weights.x + co1 * weights.y + co2 * weights.z;
  r_frame.half_size = float2(0.5f * length_u, 0.5f * math::length(dp_dv));

  float3 bounds_min(std::numeric_limits<float>::max());
  float3 bounds_max(-std::numeric_limits<float>::max());
  for (const float3 &co : positions) {
    bounds_min = math::min(bounds_min, co);
    bounds_max = math::max(bounds_max, co);
  }
  r_frame.radius = std::max(math::length(bounds_max - bounds_min), 1e-4f);
  return true;
}

/** The frames kept per UV point: a row has one, a few rows or tiles are alive at a time. */
constexpr int DECAL_FRAME_CACHE_ENTRIES = 8;

struct DecalFrameKey {
  uint64_t hash = 0;
  float2 uv;
  int tile_number = 0;

  bool operator==(const DecalFrameKey &other) const
  {
    return hash == other.hash && uv.x == other.uv.x && uv.y == other.uv.y &&
           tile_number == other.tile_number;
  }
};

struct DecalFrameCacheEntry {
  /** Whether the search found a usable triangle; a miss is remembered too. */
  bool valid = false;
  PaintMaterialDecalUvFrame frame;

  /** Unbudgeted: the entry cap alone bounds this store. */
  size_t bytes() const
  {
    return sizeof(*this);
  }
};

static bke::LruByteCache<DecalFrameKey, DecalFrameCacheEntry> g_decal_frame_cache(
    std::numeric_limits<size_t>::max(), DECAL_FRAME_CACHE_ENTRIES);

bool BKE_paint_material_decal_frame_from_uv(const Object *object,
                                            const Material *material,
                                            const int tile_number,
                                            const float2 &uv,
                                            PaintMaterialDecalUvFrame &r_frame)
{
  const Mesh *mesh_ptr = composite_projection_mesh(object);
  if (mesh_ptr == nullptr || object->totcol <= 0) {
    return false;
  }
  const Mesh &mesh = *mesh_ptr;
  const StringRefNull uv_name = BKE_paint_layers_gradient_uv_map_name(mesh);
  if (uv_name.is_empty()) {
    return false;
  }
  const bke::AttributeAccessor attributes = mesh.attributes();
  const auto uv_attr = attributes.lookup<float2>(uv_name.c_str(), bke::AttrDomain::Corner);
  if (!uv_attr) {
    return false;
  }
  const VArraySpan<float2> uvs(*uv_attr);
  Array<bool> slot_matches(object->totcol, false);
  if (decal_slots_match(*object, material, slot_matches) == 0) {
    return false;
  }

  /* Every describe of an effect asks for the frame of the same UV point while the mesh stands still
   * (an interactive drag of an unrelated value, a bake after a preview), and the triangle search is
   * linear in the mesh: the last few answers are kept. The key holds no pointers, like the maps'. */
  uint64_t key = composite_mesh_content_hash(mesh);
  for (const bool match : slot_matches) {
    key = get_default_hash(key, match);
  }
  key = get_default_hash(key, object->id.session_uid, tile_number, StringRef(uv_name));
  key = get_default_hash(key, uv.x, uv.y, mesh.corners_num);
  const DecalFrameKey frame_key{key, uv, tile_number};
  const std::shared_ptr<const DecalFrameCacheEntry> entry = g_decal_frame_cache.get_or_build(
      frame_key, [&]() -> std::shared_ptr<const DecalFrameCacheEntry> {
        const VArraySpan<int> material_indices = *attributes.lookup_or_default<int>(
            "material_index", bke::AttrDomain::Face, 0);
        const Span<int3> corner_tris = mesh.corner_tris();
        /* The same enabled set the Decal map uses, so the frame's seed triangle is one the map
         * lays. */
        Array<int> tri_faces(corner_tris.size());
        bke::mesh::corner_tris_calc_face_indices(mesh.faces(), tri_faces);
        Array<bool> tri_enabled(corner_tris.size(), false);
        for (const int64_t i : corner_tris.index_range()) {
          const int material_index = material_indices[tri_faces[i]];
          tri_enabled[i] = material_index >= 0 && material_index < object->totcol &&
                           slot_matches[material_index];
        }
        auto built = std::make_shared<DecalFrameCacheEntry>();
        built->valid = BKE_paint_material_decal_frame_from_uv_mesh(corner_tris,
                                                                   mesh.corner_verts(),
                                                                   mesh.vert_positions(),
                                                                   uvs,
                                                                   tri_enabled,
                                                                   tile_number,
                                                                   uv,
                                                                   built->frame);
        return built;
      });
  r_frame = entry->frame;
  return entry->valid;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Free
 * \{ */

void BKE_paint_material_decal_map_free()
{
  g_decal_map_cache.clear();
  g_decal_inverse_cache.clear();
  g_decal_frame_cache.clear();
  g_decal_adjacency_cache.clear();
}

/** \} */

}  // namespace blender
