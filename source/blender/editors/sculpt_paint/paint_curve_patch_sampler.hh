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
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender {
struct Brush;
struct CCGKey;
struct ImagePool;
struct Object;
struct SubdivCCG;
}  // namespace blender

namespace blender::bke::pbvh {
class Tree;
}

namespace blender::ed::sculpt_paint {

struct CurvePatchCache;
struct StrokeCache;

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
   * this to decide whether `CurvePatchCache::surface.vert_normals` -- a per-mesh-vertex array --
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
   * `ColorEffect` reads the RGB as its paint color (replacing `BKE_brush_color_get` when a texture
   * is assigned) and uses the alpha to modulate the mix; `ReliefEffect` ignores this field, so its
   * existing aggregate initializers (`{orig, height, weight}`) keep working via the default here.
   * The alpha is the texture's own alpha (an image texture with transparency reports < 1), not a
   * mix weight by itself. */
  float4 tex_color{1.0f, 1.0f, 1.0f, 1.0f};
};

class CurvePatchSampler {
 public:
  CurvePatchSampler(const CurvePatchCache &patch,
                    const StrokeCache &cache,
                    const Brush &brush,
                    const CurvePatchSourceGeometry &source,
                    Span<float> mask,
                    ImagePool *tex_pool);

  /** Read-only and thread-safe; `thread_id` indexes the texture pool's per-thread slot. */
  std::optional<CurvePatchSample> sample(int idx, int thread_id) const;

 private:
  const CurvePatchCache &patch_;
  const StrokeCache &cache_;
  const Brush &brush_;
  CurvePatchSourceGeometry source_;
  Span<float> mask_;
  ImagePool *tex_pool_;
  float total_length_;
  float2 mtex_size_;
  float2 mtex_ofs_;
};

/** Largest world-space half-width anywhere on the curve, scaled by the ribbon radius. */
float curve_patch_max_radius(const CurvePatchCache &patch);

/** Drop nodes whose bounds fall entirely outside the falloff tube. `query_mask` is the caller's
 * `calc_brush_node_mask()` result. */
IndexMask curve_patch_cull_nodes(const CurvePatchCache &patch,
                                 const StrokeCache &cache,
                                 const bke::pbvh::Tree &pbvh,
                                 const IndexMask &query_mask,
                                 float max_radius,
                                 IndexMaskMemory &memory);

/** Multires only: cull individual grids within the surviving nodes the same way. */
BitVector<> curve_patch_cull_grids(const CurvePatchCache &patch,
                                   const StrokeCache &cache,
                                   const bke::pbvh::Tree &pbvh,
                                   const SubdivCCG &subdiv_ccg,
                                   const CCGKey &key,
                                   Span<float3> positions,
                                   const IndexMask &node_mask,
                                   float max_radius);

}  // namespace blender::ed::sculpt_paint
