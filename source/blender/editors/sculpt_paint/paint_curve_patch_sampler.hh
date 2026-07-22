/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Target-agnostic half of the Curve Patch re-stamp: given an element, decide whether the patch
 * reaches it and with what magnitude. Knows nothing about what the caller then writes -- relief
 * displaces positions, color mixes into a color attribute, and both consume the same samples.
 */

#pragma once

#include <cstdint>
#include <optional>

#include "BLI_bit_vector.hh"
#include "BLI_index_mask.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "DNA_scene_enums.h"

namespace blender {
struct Brush;
struct CCGKey;
struct ImagePool;
struct Object;
struct SubdivCCG;
}  // namespace blender

namespace blender::bke {
struct CurvePatchGeometry;
}

namespace blender::bke::pbvh {
class Tree;
}

namespace blender::ed::sculpt_paint {

struct CurvePatchItem;
struct CurvePatchTextureBinding;

/**
 * The four values the sampler needs from a stroke, as data rather than as a live `StrokeCache`.
 *
 * All four are produced by the symmetry machinery (`do_symmetrical_brush_actions()` /
 * `cache_calc_brushdata_symm()`), never by user input -- which is why a headless caller can fill
 * this in without synthesizing a whole stroke. Taking them as data is also what lets this module
 * stop including `mesh/sculpt_intern.hh`, a neighbouring module's private header.
 */
struct CurvePatchStrokeContext {
  ePaintSymmetryFlags mirror_symmetry_pass = ePaintSymmetryFlags(0);
  int radial_symmetry_pass = 0;
  float4x4 symm_rot_mat_inv = float4x4::identity();
  /** Signed brush strength for this pass, already folded through the Add/Subtract direction. */
  float bstrength = 0.0f;
};

/**
 * Map a world-space position into the canonical (non-mirrored, non-rotated) frame the patch was
 * built in -- the same technique `filter_region_clip_factors()` uses to compare a real vertex
 * against pass-0-defined data.
 *
 * Shared by the sampler and both cull helpers: they must agree, or culling drops elements the
 * sampler would have accepted. Applies to direction vectors as well as points, which is what lets
 * the sampler canonicalize a vertex normal through it -- `symm_rot_mat_inv` is a rotation, so
 * transforming a normal as a point is exact here.
 */
float3 curve_patch_canonicalize(const CurvePatchStrokeContext &ctx, const float3 &co);

/**
 * Radius of the tube around the control polyline that the relief can possibly reach.
 *
 * The `2.5x` factor is generous margin for the bounding-sphere approximation the culls make and for
 * slightly-stale node bounds; at the tube boundary the relief has tapered to ~0 anyway, so nothing
 * visible is ever culled. `ribbon_end_margin` is added because the ribbon's end extension reaches
 * that far past the polyline's own ends, and a node holding only the overhang of an end stamp would
 * otherwise be dropped before the per-element test could claim it.
 *
 * Both cull helpers derive their reach from THIS -- they were two verbatim copies of the
 * expression, and the sampler's own acceptance radius is what they have to stay conservative
 * against.
 *
 * Takes the geometry rather than the session so that the agreement between it and
 * #curve_patch_max_radius can be pinned down by a test without standing up a sculpt session.
 */
float curve_patch_cull_tube_radius(const bke::CurvePatchGeometry &geometry, float max_radius);

/**
 * Pre-patch state the sampler reads. An effect that mutates geometry (relief) serves positions
 * from its own snapshot so the sampler never sees its own output; an effect that does not
 * (color) leaves #orig_positions null, because the live arrays are already pristine.
 */
struct CurvePatchSourceGeometry {
  /** Live element positions, indexed by the same flat scheme the effect snapshots with. */
  Span<float3> positions;
  /** Normals used for the surface-orientation cull. */
  Span<float3> normals;
  /** Snapshot consulted before #positions, or null when the target does not move geometry. */
  const Map<int, float3> *orig_positions = nullptr;
  /**
   * Whether the index the sampler is called with is a mesh vertex index. True for every target
   * whose elements ARE mesh vertices (relief, color); false when the spans above describe
   * something else entirely -- an image target derives one entry per PIXEL, so its indices run
   * `[0, chunk_size)` and name nothing in mesh-vertex space.
   *
   * Defaults to true so that the mesh-vertex targets keep their existing behavior; only a source
   * that knows its indices are foreign has to say so. See #CurvePatchSampler::sample, which uses
   * this to decide whether `CurvePatchGeometry::surface.vert_normals` -- a per-mesh-vertex array --
   * may be indexed at all.
   */
  bool indices_are_mesh_verts = true;
};

/** What the curve contributes at one element. */
struct CurvePatchSample {
  /** Pre-patch position, resolved through the snapshot. Effects that write geometry use it as
   * their write base; it must survive from phase 1 because a later symmetry pass would otherwise
   * read a position an earlier pass already wrote. */
  float3 orig;
  /** Signed magnitude: texture value folded with every falloff, the sculpt mask and
   * `StrokeCache::bstrength`. */
  float value;
  /** Claim weight for the cross-symmetry-pass blend. */
  float weight;
  /** RGBA of the brush texture sampled at the same `(u, s)` as #value. Stays `{1, 1, 1, 1}` when
   * the brush has no texture (`brush.mtex.tex == nullptr`), matching the initializer the sampler
   * already used for its local `tex_rgba`/`sample_rgba` buffers before the null-texture guard.
   *
   * `ColorEffect` and `ImageColorEffect` use only the ALPHA, to modulate their mix -- both paint
   * the brush's primary color, so the RGB is carried but not yet read, reserved for real
   * RGBA-texture support. `ReliefEffect` ignores the field entirely, so its existing aggregate
   * initializers (`{orig, height, weight}`) keep working via the default here. The alpha is the
   * texture's own alpha (an image texture with transparency reports < 1), not a mix weight by
   * itself. */
  float4 tex_color{1.0f, 1.0f, 1.0f, 1.0f};
};

class CurvePatchSampler {
 public:
  CurvePatchSampler(const CurvePatchItem &item,
                    const CurvePatchTextureBinding &texture,
                    const CurvePatchStrokeContext &ctx,
                    const Brush &brush,
                    const CurvePatchSourceGeometry &source,
                    Span<float> mask,
                    /* A reference rather than a pointer on purpose: a sampler built for a brush
                     * WITH a texture and no pool dereferenced null here, while a brush without one
                     * passed -- a defect no ordinary check catches. See
                     * #SculptSession::tex_pool_ensure. */
                    ImagePool &tex_pool);

  /** Read-only and thread-safe; `thread_id` indexes the texture pool's per-thread slot. */
  std::optional<CurvePatchSample> sample(int idx, int thread_id) const;

 private:
  const CurvePatchItem &item_;
  const CurvePatchTextureBinding &texture_;
  const CurvePatchStrokeContext &ctx_;
  const Brush &brush_;
  CurvePatchSourceGeometry source_;
  Span<float> mask_;
  ImagePool &tex_pool_;
  float total_length_;
  float2 mtex_size_;
  float2 mtex_ofs_;
};

/** Largest world-space half-width anywhere on the curve, scaled by the ribbon radius. Takes the
 * geometry rather than the session for the same reason #curve_patch_cull_tube_radius does. */
float curve_patch_max_radius(const bke::CurvePatchGeometry &geometry);

/** Drop nodes whose bounds fall entirely outside the falloff tube. `query_mask` is the caller's
 * `calc_brush_node_mask()` result. */
IndexMask curve_patch_cull_nodes(const CurvePatchItem &item,
                                 const CurvePatchStrokeContext &ctx,
                                 const bke::pbvh::Tree &pbvh,
                                 const IndexMask &query_mask,
                                 float max_radius,
                                 IndexMaskMemory &memory);

/** Multires only: cull individual grids within the surviving nodes the same way. */
BitVector<> curve_patch_cull_grids(const CurvePatchItem &item,
                                   const CurvePatchStrokeContext &ctx,
                                   const bke::pbvh::Tree &pbvh,
                                   const SubdivCCG &subdiv_ccg,
                                   const CCGKey &key,
                                   Span<float3> positions,
                                   const IndexMask &node_mask,
                                   float max_radius);

}  // namespace blender::ed::sculpt_paint
