/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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

#include "paint_curve_patch_document.hh"
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
 * `BKE_brush_curve_strength()` inside the sampler builds the table lazily for the CUSTOM preset,
 * and the gather phase of every effect runs in parallel -- a lazy init inside a worker would race.
 * Doing it here, once per pass, is what makes those reads safe.
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
                                              const CurvePatchItem &item,
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
      item, ctx, pbvh, cursor_sample_result.node_mask, max_radius, memory);
}

/**
 * Fold one pass's contribution into the cross-pass accumulator and return the running composite.
 *
 * Two different overlaps meet at one element, and they are not the same operation:
 * - The passes of ONE patch (mirror / radial symmetry) can each claim it. Their contributions
 *   average, weighted by each pass's own falloff, so the pass whose write ran last does not leave a
 *   hard seam where only one side "won".
 * - A patch stamped AFTER another (the session stamps from the bottom of the curve list up) lies
 *   over it. It is composited with the "over" operator, so it hides what is below it exactly as far
 *   as its own coverage says and lets the rest show through. This is what makes a point's opacity
 *   behave as opacity: a transparent stretch of the upper curve contributes nothing and the lower
 *   curve stays visible, instead of being averaged down.
 *
 * `premultiplied` is the pass's payload in `xyz` (a height, a color already multiplied by its
 * coverage, ...) and its coverage in `w`, in [0, 1]. `weight` weights the pass within its own
 * patch. The patch is identified by #CurvePatchApplyState::patch_rank, so the passes of one patch
 * must run back to back.
 *
 * `key` is whatever index the effect's snapshot is keyed by -- a mesh vertex or a color-domain
 * element. A sole patch gets back exactly its own weighted average, which is what the accumulator
 * returned before patches could overlap.
 */
inline float4 curve_patch_blend_over(CurvePatchOverAccum &accum,
                                     const int patch_rank,
                                     const float weight,
                                     const float4 &premultiplied)
{
  if (accum.patch_rank != patch_rank) {
    if (accum.patch_rank >= 0 && accum.current_weight > 0.0f) {
      const float4 previous = accum.current / accum.current_weight;
      accum.below = previous + accum.below * (1.0f - previous.w);
    }
    accum.current = float4(0.0f);
    accum.current_weight = 0.0f;
    accum.patch_rank = patch_rank;
  }
  accum.current += premultiplied * weight;
  accum.current_weight += weight;
  /* A pass with no weight (a fully transparent stretch) claims nothing: what is below stays. */
  const float4 current = accum.current_weight > 0.0f ? accum.current / accum.current_weight :
                                                       float4(0.0f);
  return current + accum.below * (1.0f - current.w);
}

/** The same for an element kept in the restamp's shared `Map`, keyed like the effect's snapshot. */
inline float4 curve_patch_blend_over(CurvePatchApplyState &apply,
                                     const int key,
                                     const float weight,
                                     const float4 &premultiplied)
{
  return curve_patch_blend_over(
      apply.pass_weight_accum.lookup_or_add_default(key), apply.patch_rank, weight, premultiplied);
}

/**
 * #curve_patch_blend_over for a single scalar (a relief height, a mask value), whose coverage is
 * the pass weight itself. Returns the composited scalar.
 */
inline float curve_patch_blend_across_passes(CurvePatchApplyState &apply,
                                             const int key,
                                             const float weight,
                                             const float value)
{
  return curve_patch_blend_over(
             apply, key, weight, float4(value, 0.0f, 0.0f, std::clamp(weight, 0.0f, 1.0f)))
      .x;
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
 * The texture's own alpha further attenuates the factor, so a partially-transparent texel paints
 * at partial strength -- the same outcome a per-dab brush gets by multiplying the dab's alpha by
 * the texture's. `has_texture` is needed because `CurvePatchSample::tex_color` falls back to
 * `{1, 1, 1, 1}` when no texture is assigned, which is indistinguishable from an opaque sample.
 *
 * `strength` is the brush's Strength (`BKE_brush_alpha_get()`). Unlike a relief patch, whose
 * Strength rides in through `bstrength` and thus through `blended`, a color/image patch does NOT
 * get it that way: `brush_strength()` deliberately omits `alpha` for `SCULPT_BRUSH_TYPE_PAINT`
 * (`mesh/sculpt.cc`), because the ordinary paint pipeline applies it as a separate final
 * multiplier instead (`sculpt_paint_color.cc`'s `buffer_color * alpha`). Fold it in here so the
 * Strength slider scales a color/image patch exactly as it scales an ordinary paint stroke; the
 * modal editor already re-stamps when the slider changes
 * (`CurvePatchEditOpData::last_synced_alpha`).
 */
/**
 * The RGB one sample paints.
 *
 * The sampler already returns the ribbon texture's full RGBA in #CurvePatchSample::tex_color,
 * sampled at the ribbon's own `(u, v)`. Only the alpha used to be read, so a ribbon with a color
 * texture assigned painted a flat brush color and the texture was invisible in the result.
 *
 * When a texture is assigned its color IS the paint color -- the same rule the raster PBR engine
 * follows ("a source, or the texture itself, is the color; the Color slider does not multiply",
 * `mesh/paint_image_2d.cc`). With no texture the caller's own color is returned unchanged.
 *
 * \param tex_valid: #CurvePatchSample::tex_valid, i.e. whether a texture was actually evaluated
 * AT THIS ELEMENT. Deliberately per-sample: a brush-wide "has a texture somewhere" test answers
 * a different question, and answering this one with it paints an opaque white ribbon wherever the
 * branch that ran had no texture of its own.
 *
 * Only callers that write a COLOR may use this. A scalar material channel carries its value in
 * #CurvePatchSample::value already, and Normal writes a packed direction that a texture's RGB
 * would corrupt.
 */
inline float3 curve_patch_paint_color(const float3 &base_color,
                                      const float4 &tex_color,
                                      const bool tex_valid)
{
  return tex_valid ? float3(tex_color) : base_color;
}

/** \param tex_valid: see #curve_patch_paint_color -- an unevaluated texture contributes no alpha
 * either, or a textureless stamp would fade the ribbon by its identity alpha of 1 (harmless) while
 * a textureless SINGLE ribbon would do the same for a texture the user never assigned. */
inline float curve_patch_color_mix_factor(const float blended,
                                          const float4 &tex_color,
                                          const bool tex_valid,
                                          const float strength)
{
  const float source_alpha = tex_valid ? tex_color.w : 1.0f;
  return std::clamp(blended, 0.0f, 1.0f) * source_alpha * strength;
}

/**
 * Record the nodes one pass wrote, in both of the sets that track them.
 *
 * `last_restamp_nodes` describes only the latest re-stamp and is what the NEXT restore reverts;
 * `all_touched_nodes` accumulates over the patch's whole life and is what the commit-time undo
 * step is pushed over. `set_bits()` ORs in place: the orchestrator sizes and clears the former
 * before the first pass of a re-stamp and only ever sizes the latter, so neither may be cleared
 * here.
 */
inline void curve_patch_record_touched_nodes(CurvePatchApplyState &apply,
                                             const IndexMask &tag_mask)
{
  tag_mask.set_bits(apply.last_restamp_nodes);
  tag_mask.set_bits(apply.all_touched_nodes);
}

}  // namespace blender::ed::sculpt_paint
