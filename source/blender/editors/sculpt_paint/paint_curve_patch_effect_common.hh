/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Machinery every Curve Patch effect repeats: turning the live `StrokeCache` into the sampler's
 * data-only stroke context, the node-mask query, the lazy falloff-curve init, the cross-pass blend
 * and the touched-node bookkeeping.
 *
 * This header, unlike `paint_curve_patch_effect.hh`, deliberately DOES reach into sculpt internals
 * -- the effects genuinely work with `StrokeCache` and the Paint BVH, and pretending otherwise
 * would only push the same includes into three files instead of one. What it exists to protect is
 * `paint_curve_patch_sampler.{hh,cc}`, which after this no longer sees any of it.
 */

#include <algorithm>

#include "DNA_brush_types.h"

#include "BKE_colortools.hh"

#include "BLI_bit_vector.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "paint_curve_patch_sampler.hh"
#include "paint_curve_patch_session.hh"

#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/**
 * Snapshot the four stroke values the sampler needs out of the live `StrokeCache`.
 *
 * Call once per symmetry pass, at the top of `apply_pass()` -- NOT once per re-stamp. Every field
 * here is rewritten by `do_symmetrical_brush_actions()` / `cache_calc_brushdata_symm()` between
 * passes, so a context captured before them would describe pass 0 for every pass.
 */
inline CurvePatchStrokeContext curve_patch_stroke_context_from_cache(const StrokeCache &cache)
{
  CurvePatchStrokeContext ctx;
  ctx.mirror_symmetry_pass = cache.mirror_symmetry_pass;
  ctx.radial_symmetry_pass = cache.radial_symmetry_pass;
  ctx.symm_rot_mat_inv = cache.symm_rot_mat_inv;
  ctx.bstrength = cache.bstrength;
  return ctx;
}

/**
 * Build the falloff curve's lookup table before any worker thread can read it.
 *
 * `BKE_brush_curve_strength()` inside the sampler builds the table lazily for the CUSTOM preset, and
 * the gather phase of every effect runs in parallel -- a lazy init inside a worker would race. Doing
 * it here, once per pass, is what makes those reads safe.
 */
inline void curve_patch_effect_ensure_falloff_curve(const Brush &brush)
{
  if (brush.curve_distance_falloff) {
    BKE_curvemapping_init(brush.curve_distance_falloff);
  }
}

/**
 * The Paint BVH nodes one pass has to walk: the brush's own encompassing-sphere query, narrowed to
 * the falloff tube.
 *
 * The query alone is only a conservative superset -- on a long curve over a broad surface it holds
 * many nodes whose every vertex the sampler would reject -- so both steps belong together and no
 * effect should perform one without the other. `memory` must outlive the returned mask.
 */
inline IndexMask curve_patch_effect_node_mask(const Depsgraph &depsgraph,
                                              Object &ob,
                                              const Brush &brush,
                                              const CurvePatchSession &patch,
                                              const CurvePatchStrokeContext &ctx,
                                              const bke::pbvh::Tree &pbvh,
                                              const float max_radius,
                                              IndexMaskMemory &memory)
{
  /* Local: the query mask is consumed by the cull below and never escapes, whereas the culled mask
   * this returns is built into the caller's `memory`. */
  IndexMaskMemory query_memory;
  const brushes::CursorSampleResult cursor_sample_result = calc_brush_node_mask(
      depsgraph, ob, brush, query_memory);
  return curve_patch_cull_nodes(
      patch, ctx, pbvh, cursor_sample_result.node_mask, max_radius, memory);
}

/**
 * Fold one pass's contribution into the cross-pass accumulator and report the running weighted
 * average.
 *
 * A patch straddling a mirror or radial symmetry plane can have both the direct and the mirrored
 * pass claim the same element. Without this the pass whose write ran last would simply overwrite
 * the earlier one, leaving a hard seam where only one side "won". Weighting by each pass's own
 * falloff (rather than a plain average) makes the blend fall off to whichever pass dominates away
 * from the overlap, converging to that pass's own value alone.
 *
 * `key` is whatever index the effect's snapshot is keyed by -- a mesh vertex, a color-domain
 * element, or the image effect's packed tile-and-offset id.
 */
inline float curve_patch_blend_across_passes(CurvePatchApplyState &apply,
                                             const int key,
                                             const float weight,
                                             const float value)
{
  float2 &accum = apply.pass_weight_accum.lookup_or_add(key, float2(0.0f, 0.0f));
  accum.x += weight;
  accum.y += weight * value;
  return accum.y / accum.x;
}

/**
 * Turn a blended magnitude into a color mix factor.
 *
 * Deliberately NOT shared with `ReliefEffect`, which has no `abs()` anywhere: a negative
 * `bstrength` (the Subtract direction, or a CUSTOM falloff curve dipping below zero) legitimately
 * carves relief inward, whereas a negative mix factor has no meaning. Clamping here is what makes
 * the brush's Add/Subtract toggle a no-op on a color patch -- unify the two and the negative
 * heights break.
 *
 * The texture's own alpha further attenuates the factor, so a partially-transparent texel paints at
 * partial strength -- the same outcome a per-dab brush gets by multiplying the dab's alpha by the
 * texture's. `has_texture` is needed because `CurvePatchSample::tex_color` falls back to
 * `{1, 1, 1, 1}` when no texture is assigned, which is indistinguishable from an opaque sample.
 */
inline float curve_patch_color_mix_factor(const float blended,
                                          const float4 &tex_color,
                                          const bool has_texture)
{
  const float source_alpha = has_texture ? tex_color.w : 1.0f;
  return std::clamp(blended, 0.0f, 1.0f) * source_alpha;
}

/**
 * Record the nodes one pass wrote, in both of the sets that track them.
 *
 * `last_restamp_nodes` describes only the latest re-stamp and is what the NEXT restore reverts;
 * `all_touched_nodes` accumulates over the patch's whole life and is what the commit-time undo step
 * is pushed over. `set_bits()` ORs in place: the orchestrator sizes and clears the former before the
 * first pass of a re-stamp and only ever sizes the latter, so neither may be cleared here.
 */
inline void curve_patch_record_touched_nodes(CurvePatchApplyState &apply, const IndexMask &tag_mask)
{
  tag_mask.set_bits(apply.last_restamp_nodes);
  tag_mask.set_bits(apply.all_touched_nodes);
}

}  // namespace blender::ed::sculpt_paint
