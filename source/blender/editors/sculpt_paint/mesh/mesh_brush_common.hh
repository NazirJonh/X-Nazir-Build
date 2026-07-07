/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_bit_span.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_subdiv_ccg.hh"

#include "DNA_brush_enums.h"

#include "sculpt_intern.hh"

namespace blender {

/**
 * This file contains common operations useful for the implementation of various different brush
 * tools. The design goals of the API are to always operate on more than one data element at a
 * time, to avoid unnecessary branching for constants, favor cache-friendly access patterns, enable
 * use of SIMD, and provide opportunities to avoid work where possible.
 *
 * API function arguments should favor passing raw data references rather than general catch-all
 * storage structs in order to clarify the scope of each function, structure the work around the
 * required data, and limit redundant data storage.
 *
 * Many functions calculate "factors" which describe how strong the brush influence should be
 * between 0 and 1. Most functions multiply with the existing factor value rather than assigning a
 * new value from scratch.
 */

struct BMesh;
struct BMVert;
struct BMFace;
struct Brush;
struct Mesh;
struct Object;
struct Sculpt;
struct SculptSession;
struct SubdivCCG;
struct SubdivCCGCoord;
namespace bke {
class AttributeAccessor;
}
namespace bke::pbvh {
class Node;
class Tree;
}  // namespace bke::pbvh

namespace ed::sculpt_paint {
struct StrokeCache;

namespace auto_mask {
struct Cache;
};

void filter_translations(MutableSpan<float3> translations, Span<float> factors);
void scale_translations(MutableSpan<float3> translations, Span<float> factors);
void scale_translations(MutableSpan<float3> translations, float factor);
void scale_factors(MutableSpan<float> factors, float strength);
void scale_factors(MutableSpan<float> factors, Span<float> strengths);
void translations_from_offset_and_factors(const float3 &offset,
                                          Span<float> factors,
                                          MutableSpan<float3> r_translations);

/**
 * For brushes that calculate an averaged new position instead of generating a new translation
 * vector.
 */
void translations_from_new_positions(Span<float3> new_positions,
                                     Span<int> verts,
                                     Span<float3> old_positions,
                                     MutableSpan<float3> translations);
void translations_from_new_positions(Span<float3> new_positions,
                                     Span<float3> old_positions,
                                     MutableSpan<float3> translations);

/** Gather data from an array aligned with all geometry vertices. */
template<typename T> void gather_data_mesh(Span<T> src, Span<int> indices, MutableSpan<T> dst)
{
  /* #exec_mode::serial because this is called from tasks with TLS that don't use isolation. */
  array_utils::gather(src, indices, dst, exec_mode::serial);
}
template<typename T>
MutableSpan<T> gather_data_mesh(const Span<T> src, const Span<int> indices, Vector<T> &dst)
{
  dst.resize(indices.size());
  gather_data_mesh(src, indices, dst.as_mutable_span());
  return dst;
}
template<typename T>
void gather_data_grids(const SubdivCCG &subdiv_ccg,
                       Span<T> src,
                       Span<int> grids,
                       MutableSpan<T> node_data);
template<typename T>
MutableSpan<T> gather_data_grids(const SubdivCCG &subdiv_ccg,
                                 const Span<T> src,
                                 const Span<int> grids,
                                 Vector<T> &dst)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  dst.resize(grids.size() * key.grid_area);
  gather_data_grids(subdiv_ccg, src, grids, dst.as_mutable_span());
  return dst;
}

template<typename T>
void gather_data_bmesh(Span<T> src, const Set<BMVert *, 0> &verts, MutableSpan<T> node_data);
template<typename T>
MutableSpan<T> gather_data_bmesh(const Span<T> src, const Set<BMVert *, 0> &verts, Vector<T> &dst)
{
  dst.resize(verts.size());
  gather_data_bmesh(src, verts, dst.as_mutable_span());
  return dst;
}

/** Scatter data from an array of the node's data to the referenced geometry vertices. */
template<typename T> void scatter_data_mesh(Span<T> src, Span<int> indices, MutableSpan<T> dst)
{
  /* #exec_mode::serial because this is called from tasks with TLS that don't use isolation. */
  array_utils::scatter(src, indices, dst, exec_mode::serial);
}
template<typename T>
void scatter_data_grids(const SubdivCCG &subdiv_ccg,
                        Span<T> node_data,
                        Span<int> grids,
                        MutableSpan<T> dst);
template<typename T>
void scatter_data_bmesh(Span<T> node_data, const Set<BMVert *, 0> &verts, MutableSpan<T> dst);

/** Fill the output array with all positions in the geometry referenced by the indices. */
inline MutableSpan<float3> gather_grids_positions(const SubdivCCG &subdiv_ccg,
                                                  const Span<int> grids,
                                                  Vector<float3> &positions)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  positions.resize(key.grid_area * grids.size());
  gather_data_grids(subdiv_ccg, subdiv_ccg.positions.as_span(), grids, positions);
  return positions;
}
void gather_bmesh_positions(const Set<BMVert *, 0> &verts, MutableSpan<float3> positions);
inline MutableSpan<float3> gather_bmesh_positions(const Set<BMVert *, 0> &verts,
                                                  Vector<float3> &positions)
{
  positions.resize(verts.size());
  gather_bmesh_positions(verts, positions.as_mutable_span());
  return positions;
}

/** Fill the output array with all normals in the grids referenced by the indices. */
void gather_grids_normals(const SubdivCCG &subdiv_ccg,
                          Span<int> grids,
                          MutableSpan<float3> normals);
void gather_bmesh_normals(const Set<BMVert *, 0> &verts, MutableSpan<float3> normals);

/**
 * Common set of mesh attributes used by a majority of brushes when calculating influence.
 */
struct MeshAttributeData {
  /* Point Domain */
  VArraySpan<float> mask;
  VArraySpan<bool> hide_vert;

  /* Face Domain */
  VArraySpan<bool> hide_poly;
  VArraySpan<int> face_sets;

  explicit MeshAttributeData(const Mesh &mesh);
};

void calc_factors_common_mesh(const Depsgraph &depsgraph,
                              const Brush &brush,
                              const Object &object,
                              const MeshAttributeData &attribute_data,
                              Span<float3> positions,
                              Span<float3> vert_normals,
                              const bke::pbvh::MeshNode &node,
                              Vector<float> &r_factors,
                              Vector<float> &r_distances);
void calc_factors_common_mesh_indexed(const Depsgraph &depsgraph,
                                      const Brush &brush,
                                      const Object &object,
                                      const MeshAttributeData &attribute_data,
                                      Span<float3> vert_positions,
                                      Span<float3> vert_normals,
                                      const bke::pbvh::MeshNode &node,
                                      Vector<float> &r_factors,
                                      Vector<float> &r_distances);
void calc_factors_common_mesh_indexed(const Depsgraph &depsgraph,
                                      const Brush &brush,
                                      const Object &object,
                                      const MeshAttributeData &attribute_data,
                                      Span<float3> vert_positions,
                                      Span<float3> vert_normals,
                                      const bke::pbvh::MeshNode &node,
                                      MutableSpan<float> factors,
                                      MutableSpan<float> distances);
void calc_factors_common_grids(const Depsgraph &depsgraph,
                               const Brush &brush,
                               const Object &object,
                               Span<float3> positions,
                               const bke::pbvh::GridsNode &node,
                               Vector<float> &r_factors,
                               Vector<float> &r_distances);
void calc_factors_common_bmesh(const Depsgraph &depsgraph,
                               const Brush &brush,
                               const Object &object,
                               Span<float3> positions,
                               bke::pbvh::BMeshNode &node,
                               Vector<float> &r_factors,
                               Vector<float> &r_distances);
void calc_factors_common_from_orig_data_mesh(const Depsgraph &depsgraph,
                                             const Brush &brush,
                                             const Object &object,
                                             const MeshAttributeData &attribute_data,
                                             Span<float3> positions,
                                             Span<float3> normals,
                                             const bke::pbvh::MeshNode &node,
                                             Vector<float> &r_factors,
                                             Vector<float> &r_distances);
void calc_factors_common_from_orig_data_grids(const Depsgraph &depsgraph,
                                              const Brush &brush,
                                              const Object &object,
                                              Span<float3> positions,
                                              Span<float3> normals,
                                              const bke::pbvh::GridsNode &node,
                                              Vector<float> &r_factors,
                                              Vector<float> &r_distances);
void calc_factors_common_from_orig_data_bmesh(const Depsgraph &depsgraph,
                                              const Brush &brush,
                                              const Object &object,
                                              Span<float3> positions,
                                              Span<float3> normals,
                                              bke::pbvh::BMeshNode &node,
                                              Vector<float> &r_factors,
                                              Vector<float> &r_distances);

/**
 * Calculate initial influence factors based on vertex visibility.
 */
void fill_factor_from_hide(Span<bool> hide_vert, Span<int> verts, MutableSpan<float> r_factors);
void fill_factor_from_hide(const SubdivCCG &subdiv_ccg,
                           Span<int> grids,
                           MutableSpan<float> r_factors);
void fill_factor_from_hide(const Set<BMVert *, 0> &verts, MutableSpan<float> r_factors);

/**
 * Calculate initial influence factors based on vertex visibility and masking.
 */
void fill_factor_from_hide_and_mask(Span<bool> hide_vert,
                                    Span<float> mask,
                                    Span<int> verts,
                                    MutableSpan<float> r_factors);
void fill_factor_from_hide_and_mask(const SubdivCCG &subdiv_ccg,
                                    Span<int> grids,
                                    MutableSpan<float> r_factors);
void fill_factor_from_hide_and_mask(const BMesh &bm,
                                    const Set<BMVert *, 0> &verts,
                                    MutableSpan<float> r_factors);

/**
 * Disable brush influence when vertex normals point away from the view.
 */
void calc_front_face(const float3 &view_normal, Span<float3> normals, MutableSpan<float> factors);
void calc_front_face(const float3 &view_normal,
                     Span<float3> vert_normals,
                     Span<int> verts,
                     MutableSpan<float> factors);
void calc_front_face(const float3 &view_normal,
                     const SubdivCCG &subdiv_ccg,
                     Span<int> grids,
                     MutableSpan<float> factors);
void calc_front_face(const float3 &view_normal,
                     const Set<BMVert *, 0> &verts,
                     MutableSpan<float> factors);
void calc_front_face(const float3 &view_normal,
                     const Set<BMFace *, 0> &faces,
                     MutableSpan<float> factors);

/**
 * When the 3D view's clipping planes are enabled, brushes shouldn't have any effect on vertices
 * outside of the planes, because they're not visible. This function disables the factors for those
 * vertices.
 */
void filter_region_clip_factors(const SculptSession &ss,
                                Span<float3> vert_positions,
                                Span<int> verts,
                                MutableSpan<float> factors);
void filter_region_clip_factors(const SculptSession &ss,
                                Span<float3> positions,
                                MutableSpan<float> factors);

/**
 * True when \a ob's own #Object.scale is anisotropic (its axes differ from one another), using
 * the same tolerance as the "Object has non-uniform scale" stroke-start warning
 * (`sculpt_ops.cc`). Combined with #StrokeCache.multi_object_stroke to decide when the
 * non-uniform-scale correction helpers below should engage: a multi-object stroke always
 * engages them (every object in the group needs consistent treatment even if this particular one
 * happens to be uniformly scaled), while a single-object stroke only engages them when this
 * object's own scale actually is anisotropic -- a plain or uniformly-scaled single object stays
 * bit-exact. Declared here but defined in `sculpt.cc` (like #non_uniform_scale_compensation
 * below) since it dereferences #Object, which this header only forward-declares.
 */
bool object_has_non_uniform_scale(const Object &ob);

/**
 * Approximates world-space isotropy for a local-space NORMAL/direction vector (built from
 * cross-products of a surface normal, or averaged into an area normal) under the object's own
 * non-uniform scale, using #StrokeCache.scale (the inverse-transpose-style `max_scale /
 * object_scale_axis` compensation seeded once per stroke in #stroke_cache_init). Gated on
 * #StrokeCache.non_uniform_scale_active, so an object with uniform (or no) scale keeps its
 * previous, unchanged behavior.
 *
 * Do NOT use this for position differences / falloff distances — those transform by the
 * object's scale directly (the opposite relationship), see #position_scale_normalized.
 *
 * This returns the corrected but *unnormalized* vector; when the result is consumed as a unit
 * orientation vector (a basis axis, a rotation axis, a projection-plane normal) use
 * #scale_normalized_unit instead so uniformly-scaled strokes stay bit-exact.
 */
inline float3 scale_normalized(const StrokeCache &cache, const float3 &v)
{
  return cache.non_uniform_scale_active ? v * cache.scale : v;
}

/**
 * Unit-length variant of #scale_normalized for a raw local NORMAL/direction that is consumed as a
 * unit orientation vector. When the correction is active it applies the non-uniform-scale
 * correction and re-normalizes; otherwise it returns #v untouched — no extra #math::normalize is
 * introduced, so the result stays bit-identical to the pre-correction behavior.
 */
inline float3 scale_normalized_unit(const StrokeCache &cache, const float3 &v)
{
  return cache.non_uniform_scale_active ? math::normalize(v * cache.scale) : v;
}

/**
 * Approximates world-space isotropy for a local-space POSITION DIFFERENCE (a falloff distance,
 * a slide direction derived from `location - position`) under the object's own non-uniform
 * scale, using #StrokeCache.position_scale (`ob.scale[axis] / mat4_to_scale(world matrix)`,
 * seeded once per stroke in #stroke_cache_init). Gated on #StrokeCache.non_uniform_scale_active.
 *
 * Do NOT use this for normals/orientation vectors — see #scale_normalized instead.
 */
inline float3 position_scale_normalized(const StrokeCache &cache, const float3 &v)
{
  return cache.non_uniform_scale_active ? v * cache.position_scale : v;
}

/**
 * Per-axis compensation making a local-space displacement/direction look isotropic in world
 * space despite the object's own non-uniform #Object.scale: the largest axis is left unscaled,
 * smaller axes are scaled up to match it. Only the axis *ratios* matter for direction
 * correctness (a uniform post-multiply cancels out under `normalize()`). This is the same
 * formula #StrokeCache.scale is seeded with once per stroke in #stroke_cache_init; use that
 * cached value instead when a #StrokeCache is available (e.g. via #scale_normalized).
 */
float3 non_uniform_scale_compensation(const Object &ob);

/**
 * Per-axis compensation making a local-space POSITION DIFFERENCE match a true isotropic
 * world-space distance, given that #StrokeCache.radius/#SculptSession.cursor_radius were derived
 * by dividing a world/screen radius by the single isotropic scalar `mat4_to_scale(world matrix)`
 * (#paint_calc_object_space_radius). This is `ob.scale[axis] / mat4_to_scale(ob)` — the opposite
 * relationship from #non_uniform_scale_compensation, which corrects normals via the
 * inverse-transpose rule. This is the same formula #StrokeCache.position_scale is seeded with
 * once per stroke in #stroke_cache_init; use that cached value instead when a #StrokeCache is
 * available (e.g. via #position_scale_normalized).
 */
float3 position_scale_compensation(const Object &ob);

/**
 * A snapshot of an object's world transform used to evaluate #BKE_kelvinlet_* (Elastic Deform,
 * Snake Hook's kelvinlet mode, the Transform tool's Elastic mode) in world space instead of the
 * object's own local space. #BKE_kelvinlet_* has no scale awareness of its own -- it measures
 * real Euclidean distances/directions on whatever raw `float[3]` values it is given, so
 * evaluating it directly on local-space coordinates gives an anisotropically distorted result
 * whenever the object's own #Object.scale is non-uniform. Round-tripping through world space
 * (transform in, call kelvinlet unmodified, transform the displacement back out) fixes this
 * without touching `kelvinlet.cc`. See #kelvinlet_world_transform_init.
 */
struct KelvinletWorldTransform {
  float4x4 to_world = float4x4::identity();
  float4x4 to_local = float4x4::identity();
  float3x3 to_world_normal = float3x3::identity();
};

/**
 * Build a #KelvinletWorldTransform from \a ob's current world transform. Computes `to_local =
 * invert(to_world)` fresh from #Object.object_to_world -- does NOT read or write
 * #Object.runtime.world_to_object, so this is safe to call from a threaded PBVH node loop (unlike
 * relying on the cached runtime field the way #calc_brush_area_texture_mat does, which explicitly
 * refreshes it before reading and must not be mutated concurrently from multiple threads).
 * Intended to be called ONCE per brush step, before iterating vertices/nodes -- the object's
 * transform is assumed static for the stroke's duration (the same assumption
 * #StrokeCache.scale/#StrokeCache.position_scale already make). Declared here but defined in
 * `sculpt.cc` (like #object_has_non_uniform_scale above) since it dereferences #Object, which
 * this header only forward-declares.
 */
KelvinletWorldTransform kelvinlet_world_transform_init(const Object &ob);

/** Transform a local-space POSITION (#BKE_kelvinlet_*'s `elem_orig_co`/`brush_location`) to world space. */
inline float3 kelvinlet_position_to_world(const KelvinletWorldTransform &transform, const float3 &p)
{
  return math::transform_point(transform.to_world, p);
}

/** Transform a local-space DIRECTION (#BKE_kelvinlet_grab*'s `brush_delta`) to world space. */
inline float3 kelvinlet_direction_to_world(const KelvinletWorldTransform &transform, const float3 &v)
{
  return math::transform_direction(transform.to_world, v);
}

/**
 * Transform a local-space NORMAL (#BKE_kelvinlet_scale/#BKE_kelvinlet_twist's `surface_normal`) to
 * world space via the inverse-transpose rule -- see #non_uniform_scale_compensation for why
 * normals need a different transform law than positions/directions.
 */
inline float3 kelvinlet_normal_to_world(const KelvinletWorldTransform &transform, const float3 &n)
{
  return math::normalize(math::transform_direction(transform.to_world_normal, n));
}

/**
 * Transform a world-space DISPLACEMENT (a #BKE_kelvinlet_* return value) back to local space.
 * Displacements are directions, not positions -- use the object's inverse transform's linear
 * part, not a position round-trip.
 */
inline float3 kelvinlet_direction_to_local(const KelvinletWorldTransform &transform, const float3 &v)
{
  return math::transform_direction(transform.to_local, v);
}

/**
 * Calculate distances based on the distance from the brush cursor and various other settings.
 * Also ignore vertices that are too far from the cursor.
 */
void calc_brush_distances(const SculptSession &ss,
                          Span<float3> vert_positions,
                          Span<int> vert,
                          eBrushFalloffShape falloff_shape,
                          MutableSpan<float> r_distances);
void calc_brush_distances(const SculptSession &ss,
                          Span<float3> positions,
                          eBrushFalloffShape falloff_shape,
                          MutableSpan<float> r_distances);
void calc_brush_distances_squared(const SculptSession &ss,
                                  Span<float3> positions,
                                  Span<int> verts,
                                  eBrushFalloffShape falloff_shape,
                                  MutableSpan<float> r_distances);
void calc_brush_distances_squared(const SculptSession &ss,
                                  Span<float3> positions,
                                  eBrushFalloffShape falloff_shape,
                                  MutableSpan<float> r_distances);

/** Set the factor to zero for all distances greater than the radius. */
void filter_distances_with_radius(float radius, Span<float> distances, MutableSpan<float> factors);

/**
 * Calculate distances based on a "square" brush tip falloff and ignore vertices that are too far
 * away.
 */
template<typename T>
void calc_brush_cube_distances(const Brush &brush,
                               const Span<T> positions,
                               const MutableSpan<float> r_distances);

/**
 * Scale the distances based on the brush radius and the cached "hardness" setting, which increases
 * the strength of the effect for vertices towards the outside of the radius.
 */
void apply_hardness_to_distances(float radius, float hardness, MutableSpan<float> distances);
inline void apply_hardness_to_distances(const StrokeCache &cache,
                                        const MutableSpan<float> distances)
{
  apply_hardness_to_distances(cache.radius, cache.hardness, distances);
}

/**
 * Modify the factors based on distances to the brush cursor, using various brush settings.
 */
void calc_brush_strength_factors(const StrokeCache &cache,
                                 const Brush &brush,
                                 Span<float> distances,
                                 MutableSpan<float> factors);

/**
 * Modify brush influence factors to include sampled texture values.
 */
void calc_brush_texture_factors(const SculptSession &ss,
                                const Brush &brush,
                                Span<float3> vert_positions,
                                Span<int> vert,
                                MutableSpan<float> factors);
void calc_brush_texture_factors(const SculptSession &ss,
                                const Brush &brush,
                                Span<float3> positions,
                                MutableSpan<float> factors);

/**
 * Many brushes end up calculating translations from the original positions. Instead of applying
 * these directly to the modified values, it's helpful to process them separately to easily
 * calculate various effects like clipping. After they are processed, this function can be used to
 * simply add them to the final vertex positions.
 */
void apply_translations(Span<float3> translations, Span<int> verts, MutableSpan<float3> positions);
void apply_translations(Span<float3> translations, Span<int> grids, SubdivCCG &subdiv_ccg);
void apply_translations(Span<float3> translations, const Set<BMVert *, 0> &verts);

/** Align the translations with plane normal. */
void project_translations(MutableSpan<float3> translations, const float3 &plane);

/**
 * Cancel out translations already applied over the course of the operation from the new
 * translations. This is used for tools that calculate new positions based on the original
 * positions for the entirety of an operation. Conceptually this is the same as resetting the
 * positions before each step of the operation, but combining that into the same loop should be
 * preferable for performance.
 */
void reset_translations_to_original(MutableSpan<float3> translations,
                                    Span<float3> positions,
                                    Span<float3> orig_positions);

/**
 * Rotate translations to account for rotations from procedural deformation.
 *
 * \todo Don't invert `deform_imats` on object evaluation. Instead just invert them on-demand in
 * brush implementations. This would be better because only the inversions required for affected
 * vertices would be necessary.
 */
void apply_crazyspace_to_translations(Span<float3x3> deform_imats,
                                      Span<int> verts,
                                      MutableSpan<float3> translations);

/**
 * Modify translations based on sculpt mode axis locking and mirroring clipping.
 */
void clip_and_lock_translations(const Sculpt &sd,
                                const SculptSession &ss,
                                Span<float3> positions,
                                Span<int> verts,
                                MutableSpan<float3> translations);
void clip_and_lock_translations(const Sculpt &sd,
                                const SculptSession &ss,
                                Span<float3> positions,
                                MutableSpan<float3> translations);

/**
 * Creates OffsetIndices based on each node's unique vertex count, allowing for easy slicing of a
 * new array.
 */
OffsetIndices<int> create_node_vert_offsets(Span<bke::pbvh::MeshNode> nodes,
                                            const IndexMask &node_mask,
                                            Array<int> &node_data);
OffsetIndices<int> create_node_vert_offsets(const CCGKey &key,
                                            Span<bke::pbvh::GridsNode> nodes,
                                            const IndexMask &node_mask,
                                            Array<int> &node_data);
OffsetIndices<int> create_node_vert_offsets_bmesh(Span<bke::pbvh::BMeshNode> nodes,
                                                  const IndexMask &node_mask,
                                                  Array<int> &node_data);

/**
 * Find vertices connected to the indexed vertices across faces. Neighbors connected across hidden
 * faces are skipped.
 *
 * See #calc_vert_neighbors_interior for a version that does extra filtering for boundary vertices.
 */
GroupedSpan<int> calc_vert_neighbors(OffsetIndices<int> faces,
                                     Span<int> corner_verts,
                                     GroupedSpan<int> vert_to_face,
                                     Span<bool> hide_poly,
                                     Span<int> verts,
                                     Vector<int> &r_offset_data,
                                     Vector<int> &r_data);
GroupedSpan<int> calc_vert_neighbors(const SubdivCCG &subdiv_ccg,
                                     Span<int> grids,
                                     Vector<int> &r_offset_data,
                                     Vector<int> &r_data);
GroupedSpan<BMVert *> calc_vert_neighbors(Set<BMVert *, 0> verts,
                                          Vector<int> &r_offset_data,
                                          Vector<BMVert *> &r_data);

/**
 * Find vertices connected to the indexed vertices across faces. Neighbors connected across hidden
 * faces are skipped. For boundary vertices (stored in the \a boundary_verts argument), only
 * include other boundary vertices. Corner vertices are skipped entirely and will not have neighbor
 * information populated.
 */
GroupedSpan<int> calc_vert_neighbors_interior(OffsetIndices<int> faces,
                                              Span<int> corner_verts,
                                              GroupedSpan<int> vert_to_face,
                                              BitSpan boundary_verts,
                                              const Set<OrderedEdge> &boundary_edges,
                                              Span<bool> hide_poly,
                                              Span<int> verts,
                                              Vector<int> &r_offset_data,
                                              Vector<int> &r_data);
GroupedSpan<int> calc_vert_neighbors_interior(OffsetIndices<int> faces,
                                              Span<int> corner_verts,
                                              GroupedSpan<int> vert_to_face,
                                              BitSpan boundary_verts,
                                              const Set<OrderedEdge> &boundary_edges,
                                              Span<bool> hide_poly,
                                              Span<int> verts,
                                              Span<float> factors,
                                              Vector<int> &r_offset_data,
                                              Vector<int> &r_data);
void calc_vert_neighbors_interior(OffsetIndices<int> faces,
                                  Span<int> corner_verts,
                                  BitSpan boundary_verts,
                                  const Set<OrderedEdge> &boundary_edges,
                                  const SubdivCCG &subdiv_ccg,
                                  Span<int> grids,
                                  MutableSpan<Vector<SubdivCCGCoord>> result);
void calc_vert_neighbors_interior(const Set<BMVert *, 0> &verts,
                                  MutableSpan<Vector<BMVert *>> result);

/** Find the translation from each vertex position to the closest point on the plane. */
void calc_translations_to_plane(Span<float3> vert_positions,
                                Span<int> verts,
                                const float4 &plane,
                                MutableSpan<float3> translations);
void calc_translations_to_plane(Span<float3> positions,
                                const float4 &plane,
                                MutableSpan<float3> translations);

/** Ignores verts outside of a symmetric area defined by a pivot point. */
void filter_verts_outside_symmetry_area(Span<float3> positions,
                                        const float3 &pivot,
                                        ePaintSymmetryFlags symm,
                                        MutableSpan<float> factors);

/** Ignore points that fall below the "plane trim" threshold for the brush. */
void filter_plane_trim_limit_factors(const Brush &brush,
                                     const StrokeCache &cache,
                                     Span<float3> translations,
                                     MutableSpan<float> factors);

/** Ignore points below the plane. */
void filter_below_plane_factors(Span<float3> vert_positions,
                                Span<int> verts,
                                const float4 &plane,
                                MutableSpan<float> factors);
void filter_below_plane_factors(Span<float3> positions,
                                const float4 &plane,
                                MutableSpan<float> factors);

/* Ignore points above the plane. */
void filter_above_plane_factors(Span<float3> vert_positions,
                                Span<int> verts,
                                const float4 &plane,
                                MutableSpan<float> factors);
void filter_above_plane_factors(Span<float3> positions,
                                const float4 &plane,
                                MutableSpan<float> factors);

}  // namespace ed::sculpt_paint

}  // namespace blender
