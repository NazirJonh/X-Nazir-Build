/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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

#include "paint_curve_patch_profile.hh"

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
 * Inverse of #curve_patch_canonicalize for a DIRECTION: maps a vector expressed in the patch's
 * canonical frame back into the space the current symmetry pass actually paints in.
 *
 * Needed because everything the patch geometry reports (#CurvePatchSample::patch_axis_u and
 * friends) lives in the canonical frame, while the surface data a consumer compares it against --
 * triangle normals, tangents -- does not.
 */
float3 curve_patch_decanonicalize_dir(const CurvePatchStrokeContext &ctx, const float3 &dir);

/**
 * Radius of the tube around the control polyline that the relief can possibly reach.
 *
 * The `2.5x` factor is generous margin for the bounding-sphere approximation the culls make and
 * for slightly-stale node bounds; at the tube boundary the relief has tapered to ~0 anyway, so
 * nothing visible is ever culled. `ribbon_end_margin` is added because the ribbon's end extension
 * reaches that far past the polyline's own ends, and a node holding only the overhang of an end
 * stamp would otherwise be dropped before the per-element test could claim it.
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
   * this to decide whether `CurvePatchGeometry::surface.vert_normals` -- a per-mesh-vertex array
   * -- may be indexed at all.
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
   * Every color-writing effect reads both the RGB (an assigned texture IS the paint color, see
   * #curve_patch_paint_color) and the alpha, which attenuates the mix -- but only where
   * #tex_valid says the sample is real. `ReliefEffect` ignores the field entirely, so its
   * existing aggregate initializers (`{orig, height, weight}`) keep working via the default here.
   * The alpha is the texture's own alpha (an image texture with transparency reports < 1), not a
   * mix weight by itself. */
  float4 tex_color{1.0f, 1.0f, 1.0f, 1.0f};
  /** Whether #tex_color came from an actual texture evaluation at THIS element, as opposed to
   * standing at its identity default.
   *
   * A binding-wide "does the brush have a texture anywhere" answer cannot substitute for this.
   * The ribbon in SINGLE mode samples `brush.mtex` while the Stamps list holds the assigned
   * textures (and vice versa), and a SINGLE-mode stamp with no texture wins its merge carrying
   * the identity `{1, 1, 1, 1}`. Treating those as "the texture is white" paints an opaque WHITE
   * ribbon over the user's color -- which is worse than ignoring the texture. */
  bool tex_valid = false;
  /** Where this element sits in the PATCH's own parametrization: `u` across the ribbon, `v`
   * along it (swapped when #CurvePatchParams::swap_axis is set), or the winning stamp's local
   * coordinates in STAMPS mode. Before the texture Size / Offset transform, so a consumer
   * applies its own.
   *
   * This is what lets a texture be oriented ALONG THE CURVE instead of through the brush's
   * view/area mapping: the ribbon's own zone textures have always been sampled here, and a
   * PBR channel source has to reach the same coordinates or it would sit still while the curve
   * turns. Valid only when #patch_uv_valid is set -- a caller that samples outside the ribbon
   * or stamp branches has no such frame to offer. */
  float2 patch_uv{0.0f, 0.0f};
  bool patch_uv_valid = false;
  /** World-space directions in which #patch_uv.x and #patch_uv.y increase, reported in the SAME
   * space as the source geometry (the sampler undoes its own symmetry canonicalization first).
   * Valid together with #patch_uv_valid.
   *
   * A consumer that only reads a color from #patch_uv can ignore these, but a NORMAL map cannot:
   * its RGB encodes a direction relative to the frame the map was authored in, so writing it into
   * a surface's own tangent-space map requires knowing where that frame points. Without them a
   * normal decal would keep facing one fixed way while the curve -- and the color texture with
   * it -- turns underneath. */
  float3 patch_axis_u{1.0f, 0.0f, 0.0f};
  float3 patch_axis_v{0.0f, 1.0f, 0.0f};
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

#if CURVE_PATCH_PROFILING
  /** DEBUG-cpatch: why `branch_relief()` threw a branch away. The rejections plus the accepted
   * remainder sum to `branch_calls`, which is what says WHICH test is worth making cheaper or
   * hoisting ahead of the spline evaluations. */
  struct BranchFunnel {
    int64_t branch_calls = 0;
    int64_t rej_radius = 0;
    int64_t rej_normal_dist = 0;
    int64_t rej_falloff = 0;
    int64_t rej_endpoint = 0;
    int64_t rej_s_range = 0;
    int64_t rej_end_falloff = 0;
    /** Stamp-layout and texture-zone rejections past the geometric funnel above. */
    int64_t rej_late = 0;

    void add(const BranchFunnel &other);
  };
  const BranchFunnel &dbg_branch_funnel() const
  {
    return dbg_branch_funnel_;
  }
  int64_t dbg_reached_lut() const
  {
    return dbg_reached_lut_;
  }
  int64_t dbg_reached_relief() const
  {
    return dbg_reached_relief_;
  }
  int64_t dbg_tex_evals() const
  {
    return dbg_tex_evals_;
  }
#endif

 private:
  const CurvePatchItem &item_;
  const CurvePatchTextureBinding &texture_;
  const CurvePatchStrokeContext &ctx_;
  const Brush &brush_;
  CurvePatchSourceGeometry source_;
  Span<float> mask_;
  ImagePool &tex_pool_;
  float total_length_;
  /** `radius_at(0.0f)` / `radius_at(total_length_)`, cached because a non-square endpoint's Smooth
   * end-falloff extension reads them on every vertex, and both are curve-invariant. */
  float start_endpoint_radius_;
  float end_endpoint_radius_;
  float2 mtex_size_;
  float2 mtex_ofs_;
#if CURVE_PATCH_PROFILING
  /* DEBUG-cpatch-image funnel counters. `mutable` because `sample()` is `const`; no atomics needed
   * because a sampler is constructed per chunk and a chunk is processed by a single thread. */
  mutable BranchFunnel dbg_branch_funnel_;
  mutable int64_t dbg_reached_lut_ = 0;
  mutable int64_t dbg_reached_relief_ = 0;
  mutable int64_t dbg_tex_evals_ = 0;
#endif
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
