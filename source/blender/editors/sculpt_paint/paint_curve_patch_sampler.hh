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
