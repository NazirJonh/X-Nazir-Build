/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Decal map: the map coordinate of a Decal row per texel of the painted object's UV tile. A Decal
 * lays its frame over the surface (#bke::surface_unfold_build patch around the frame's center) so
 * the content crosses UV seams and cube edges; this map is that patch rasterized into UV space, so
 * a reader needs no triangle walk per texel and reads `q.xy` exactly where Planar would compute it.
 */

#include <cstdint>
#include <memory>

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Material;
struct Object;
struct PaintMaterialCompositeProjection;

/** The most triangles a Decal patch grows to; bounds the cost of a build on a dense mesh. */
inline constexpr int PAINT_MATERIAL_DECAL_MAX_TRIS = 20000;
/* The shader stores a triangle id in the float `position` texel's `z`, which is exact only up to
 * 2^24; a larger patch would read a rounded id and lay a texel with another triangle's frame. */
static_assert(PAINT_MATERIAL_DECAL_MAX_TRIS < (1 << 24));

/** The frame of one patch triangle: what a tangent-space Normal map is read through. */
struct PaintMaterialDecalFrame {
  /**
   * The frame axes in object space, indexed like the projection frame's own axes (`axes[axis]` is the
   * depth axis, `axes[u_index]` / `axes[v_index]` the map's U and V): the unfolded plane's X and Y
   * directions laid on this triangle, and their cross product. The shape
   * #BKE_paint_layers_projection_normal_frame_to_tangent takes as `frame_axes`.
   */
  float axes[3][3] = {};
};

/**
 * One patch triangle as the effect-space resampler reads it: where its corners land in the map
 * coordinate and in the tile's UV, and the bases a tangent-space Normal is carried through. The
 * corners give exact barycentrics in either space, so neither lookup is limited by a grid's
 * resolution.
 */
struct PaintMaterialDecalTri {
  /** The corners' map coordinate, normalized like #PaintMaterialDecalMap::q. */
  float2 q[3] = {};
  /** The corners' tile-local UV. */
  float2 uv[3] = {};
  /** The object-space directions of `q.x` and `q.y` on this triangle (unit, the unfold's). */
  float3 plane_x = float3(0.0f);
  float3 plane_y = float3(0.0f);
  /** The UV tangent basis in object space (unit), and the corner-order face normal. Zero tangent
   * and bitangent where the triangle's UV is degenerate. */
  float3 tangent = float3(0.0f);
  float3 bitangent = float3(0.0f);
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  /** The triangle was laid into the tile: its material matches and its UV is inside the tile. */
  bool in_canvas = false;
};

/** The inputs of a Decal map that are not mesh data. */
struct PaintMaterialDecalParams {
  /** The frame's center in object space; the seed triangle is the closest one to it. */
  float3 center = float3(0.0f);
  /** The frame's axes in object space (the columns of the frame rotation). */
  float3 frame_axes[3] = {
      float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f)};
  /** #eMaterialPaintLayerProjectionAxis: the depth axis; the other two are the map's U and V. */
  int axis = 2;
  /** Half the frame's extent along the map's U and V, in object units: the divisor of `q`. */
  float2 half_size = float2(1.0f);
  /** How far the patch grows from the center, in object units. */
  float radius = 1.0f;
  int max_tris = PAINT_MATERIAL_DECAL_MAX_TRIS;
  /** The destination UDIM tile, 1001 for the base tile. */
  int tile_number = 1001;
};

/**
 * The Decal frame of a transform effect, derived from a UV point instead of placed by the user: the
 * effect's Mirror center or pivot is a UV point, and its frame is the surface around that point.
 */
struct PaintMaterialDecalUvFrame {
  /** The surface point at the UV, in object space. */
  float3 center = float3(0.0f);
  /** The frame's Euler rotation (axis Z): X along the UV's U direction, Y along V, Z the normal. */
  float rotation[3] = {};
  /**
   * Half the frame's extent along U and V in object units: the surface length of one UV unit over
   * two, so that one unit of the effect's step coordinate is one UV unit at the center and an offset
   * typed in UV units moves the content by a comparable distance.
   */
  float2 half_size = float2(0.5f);
  /** The patch radius: the object's bounding box diagonal, so the patch reaches the whole object. */
  float radius = 1.0f;
};

/**
 * The frame of the surface point at the tile-local \a uv (inside \a tile_number), from mesh data.
 * The triangle holding the UV is found by barycentrics (the nearest one when the UV lies in an
 * empty place), the axes are its UV tangent and bitangent (orthonormalized) and its normal.
 * \param tri_enabled: per corner triangle whether it takes part (empty enables all).
 * \return false when no usable triangle exists.
 */
bool BKE_paint_material_decal_frame_from_uv_mesh(Span<int3> corner_tris,
                                                 Span<int> corner_verts,
                                                 Span<float3> positions,
                                                 Span<float2> uvs,
                                                 Span<bool> tri_enabled,
                                                 int tile_number,
                                                 const float2 &uv,
                                                 PaintMaterialDecalUvFrame &r_frame);

/**
 * #BKE_paint_material_decal_frame_from_uv_mesh on \a object's (evaluated) mesh and render UV map,
 * restricted to the slots that hold \a material (every slot when null). The one place the frame of
 * an effect is derived, so the CPU resample, the bake and the hashes read the same frame.
 */
bool BKE_paint_material_decal_frame_from_uv(const Object *object,
                                            const Material *material,
                                            int tile_number,
                                            const float2 &uv,
                                            PaintMaterialDecalUvFrame &r_frame);

/**
 * How many Decal maps and inverse maps were built (not read from the cache) so far. A caller that
 * compares the counts around a getter call tells a hit from a miss; used by the perf log.
 */
void BKE_paint_material_decal_build_counts_get(int64_t &r_map, int64_t &r_inverse);

/** The identity of a Decal map. No pointers, like #PaintMaterialSurfaceMapKey. */
struct PaintMaterialDecalMapKey {
  uint32_t object_uid = 0;
  int tile_number = 0;
  int width = 0;
  int height = 0;
  int verts_num = 0;
  int faces_num = 0;
  int corners_num = 0;
  /** The mesh's content hash and the matching slots. */
  uint64_t mesh_hash = 0;
  /** The frame: center, axes, extents, radius, depth axis and the patch limit. */
  uint64_t frame_hash = 0;

  bool operator==(const PaintMaterialDecalMapKey &other) const = default;
};

/**
 * The Decal map of one UDIM tile. Row-major, `y * width + x`, texel centre at `(x + 0.5) / width`.
 *
 * Stored as RG32F plus one int per texel (12 bytes, against 16 for an RGBA32F pack): `q` stays full
 * precision, since a 16-bit half steps visibly on a 4K canvas, and the triangle index replaces both
 * the valid flag (it is negative outside the patch) and the Normal hint: the frame of the triangle is
 * exact, where a rotation angle alone would have to be rebuilt per read.
 */
struct PaintMaterialDecalMap {
  PaintMaterialDecalMapKey key;
  int width = 0;
  int height = 0;
  /**
   * The map coordinate, normalized like Planar's `q.xy`: the unfolded plane position over the half
   * extent, so the frame's footprint is `[-1, 1]`. `q.x` is the map's U axis, `q.y` its V axis.
   */
  Array<float2> q;
  /** Per texel the index into #frames, or -1 where the patch does not reach. */
  Array<int> patch_tri;
  /** One frame per patch triangle, in the patch's breadth-first order. */
  Vector<PaintMaterialDecalFrame> frames;
  /** One entry per patch triangle, indexed like #frames: what the inverse map reads. */
  Vector<PaintMaterialDecalTri> tris;
  /** The patch triangles placed, and whether the triangle limit cut the growth short. */
  int patch_tris_num = 0;
  bool truncated = false;
  /** Seconds the build took; zero for a map read from the cache. */
  float build_seconds = 0.0f;

  size_t bytes() const
  {
    return size_t(this->q.size()) * sizeof(float2) + size_t(this->patch_tri.size()) * sizeof(int) +
           size_t(this->frames.size()) * sizeof(PaintMaterialDecalFrame) +
           size_t(this->tris.size()) * sizeof(PaintMaterialDecalTri);
  }
};

/**
 * The inverse of a Decal map: from a map coordinate `q` back to the tile UV of the surface point
 * the unfold put there. A grid over the patch's `q` bounds holds per cell the index of a patch
 * triangle covering it (#PaintMaterialDecalMap::tris); the reader takes the exact barycentrics of
 * `q` in that triangle (or in a neighbour cell's), so the grid only picks the triangle.
 *
 * Only triangles laid into the tile are entered: a point on a triangle of another material or
 * outside the tile has no texel of the layer's map to read.
 */
struct PaintMaterialDecalInverseMap {
  PaintMaterialDecalMapKey key;
  /** The cell count along the longer side of the bounds. */
  int resolution = 0;
  int width = 0;
  int height = 0;
  float2 q_min = float2(0.0f);
  float2 q_max = float2(0.0f);
  /** Row-major `y * width + x`; -1 where no triangle reaches the cell. */
  Array<int> cell_tri;
  float build_seconds = 0.0f;

  size_t bytes() const
  {
    return size_t(this->cell_tri.size()) * sizeof(int);
  }
};

/**
 * Build the inverse of \a map on a grid of \a resolution cells along the longer side of its
 * bounds. \return false when the map has no triangle laid into the tile.
 */
bool BKE_paint_material_decal_inverse_map_build(const PaintMaterialDecalMap &map,
                                                int resolution,
                                                PaintMaterialDecalInverseMap &r_inverse);

/**
 * The cached inverse of \a map at \a resolution. Keyed by the map's key, so a frame or mesh change
 * (which changes the map's key) builds a new one. Null when the build fails.
 */
std::shared_ptr<const PaintMaterialDecalInverseMap> BKE_paint_material_decal_inverse_map_get(
    const PaintMaterialDecalMap &map, int resolution);

/**
 * The tile UV of the surface point at map coordinate \a q (the triangle's barycentrics of \a q,
 * extrapolated by up to about a cell at the patch border). \return false outside the patch.
 */
bool BKE_paint_material_decal_inverse_lookup(const PaintMaterialDecalMap &map,
                                             const PaintMaterialDecalInverseMap &inverse,
                                             const float2 &q,
                                             float2 &r_uv,
                                             int &r_tri);

/**
 * The map coordinate at the tile UV \a uv: the nearest texel picks the triangle, its barycentrics
 * of \a uv give `q`. \a uv is wrapped into the tile first. \return false where the patch does not
 * reach.
 */
bool BKE_paint_material_decal_forward_lookup(const PaintMaterialDecalMap &map,
                                             const float2 &uv,
                                             float2 &r_q,
                                             int &r_tri);

/**
 * Build a Decal map from mesh data directly (no Object, no cache), for the cache below and tests.
 *
 * \param corner_tris, corner_verts, positions, uvs: the mesh's corner triangles, corner-to-vertex map,
 * (evaluated) vertex positions and the render UV per corner.
 * \param tri_enabled: per corner triangle whether it takes part in the canvas (the material's slots);
 * empty enables all. A disabled triangle is crossed by the patch but not rasterized, and cannot be
 * the seed.
 * \return false when no seed triangle exists (the map is left empty).
 */
bool BKE_paint_material_decal_map_build(const PaintMaterialDecalParams &params,
                                        Span<int3> corner_tris,
                                        Span<int> corner_verts,
                                        Span<float3> positions,
                                        Span<float2> uvs,
                                        Span<bool> tri_enabled,
                                        int width,
                                        int height,
                                        PaintMaterialDecalMap &r_map);

/**
 * The cached Decal map of a Decal row's frame (\a projection: object, material, tile, center,
 * rotation, size, aspect, axis) at \a width by \a height. Object space only: the controller is
 * ignored. An LRU store bounded by the sum of the maps' bytes keeps the entries. Null when the
 * object has no mesh, render UV map, matching slot or triangle.
 *
 * \param radius_scale: grows the patch past the frame's footprint. A Decal row clips to its frame
 * and needs no more; a Decal-space effect moves points inside a square frame, whose corners lie
 * `sqrt(2)` times further than its sides.
 */
std::shared_ptr<const PaintMaterialDecalMap> BKE_paint_material_decal_map_get(
    const PaintMaterialCompositeProjection &projection,
    int width,
    int height,
    float radius_scale = 1.0f);

/** Drop the cached Decal maps, their inverses and the adjacency (mesh change, file close, leaving
 * the paint context). */
void BKE_paint_material_decal_map_free();

}  // namespace blender
