/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The footprint of the content an owned effect computes from: the rectangle of the input canvas a
 * bounded projection or mapping lays content in. A bake effect whose kind has a finite reach moves
 * its result only inside the footprint grown by the chain's halo, so a bake that knows the
 * footprint can compute and write that rectangle alone instead of the whole canvas.
 *
 * The geometry lives in #bke::paint_layers::footprint::uv_rect: the preimage of the unit UV square
 * under the composed mapping chain, walked backwards through the same steps
 * `CompositeMappingChain` composes forward, so the two cannot disagree about where a texel lands.
 * Everything that would make the rectangle cut real content away -- a repeat wrap, a projected or
 * Decal read, a generator, a folder, a laid correction without a frame of its own -- refuses the
 * rectangle, and the caller computes the whole canvas as before. A conservative false is always
 * safe; a wrong rectangle never is.
 */

#include <algorithm>
#include <cmath>
#include <utility>

#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_tile.hh"

#include "paint_layers_effect_footprint.hh"
#include "paint_layers_intern.hh"

#include "../paint_material_composite_projection_internal.hh"

namespace blender {

namespace {

/** Union of \a uv into \a r_uv, which carries the first contributor as it is. */
void uv_rect_union(rctf &r_uv, bool &r_have, const rctf &uv)
{
  if (!r_have) {
    r_uv = uv;
    r_have = true;
    return;
  }
  r_uv.xmin = std::min(r_uv.xmin, uv.xmin);
  r_uv.ymin = std::min(r_uv.ymin, uv.ymin);
  r_uv.xmax = std::max(r_uv.xmax, uv.xmax);
  r_uv.ymax = std::max(r_uv.ymax, uv.ymax);
}

/**
 * Whether the described correction lays coverage of its own at a place no frame of this file
 * bounds: a laid correction's content joins the row's, so an unbounded one unbounds the whole
 * input. The caller has already skipped the filter corrections (which lay no coverage of their
 * own, the doc of #PaintMaterialCompositeCorrection::filter); of the laid ones, one whose result
 * is a layer with an alpha of its own grows the row's content by the chain's halo, which the
 * region the caller computes adds anyway -- those carry a map and are bounded by their own frame.
 * One without a map -- a flat value over the whole canvas, an Anchor Point slice whose geometry
 * is not here -- refuses the rectangle.
 */
bool correction_lays_unbounded_content(const PaintMaterialCompositeCorrection &correction)
{
  return correction.image == nullptr;
}

}  // namespace

namespace bke::paint_layers::footprint {

bool uv_rect(const PaintMaterialCompositeMapping &mapping,
             blender::Span<PaintMaterialCompositeMapping> input_steps,
             rctf &r_uv)
{
  /* A projected step has no UV form to invert: the frame's footprint is the mesh's, not a
   * rectangle of the canvas (the v1 boundary -- a projected row keeps the whole canvas). */
  if (mapping.projection.present) {
    return false;
  }
  for (const PaintMaterialCompositeMapping &step : input_steps) {
    if (step.projection.present) {
      return false;
    }
  }
  /* The row's own mapping is what clips the composed lookup at the end: without it (or with an
   * axis it repeats) the chain floors its result, which tiles the content over the whole canvas. */
  if (!mapping.enabled) {
    return false;
  }
  bool repeat_u = false;
  bool repeat_v = false;
  BKE_paint_layers_mapping_wrap_axes(mapping.wrap, repeat_u, repeat_v);
  if (repeat_u || repeat_v) {
    return false;
  }
  /* The description leaves transform effects past the cap out of the chain entirely: a step the
   * walk never sees would move content outside the rectangle computed without it. */
  if (input_steps.size() >= PAINT_MATERIAL_COMPOSITE_INPUT_TRANSFORM_MAX) {
    return false;
  }

  const CompositeMappingChain chain = composite_mapping_chain_build(mapping, input_steps);
  if (chain.steps_num == 0) {
    return false;
  }

  /* The preimage of the unit square, walked backwards through the steps the chain composes
   * forward. One step's affine part inverts in closed form (`S * R * fold` reads the fold's
   * output, so the affine inverse lands in fold space), and a fold's preimage of one point is the
   * point and its reflection about the fold's center -- both fold directions map the same pair
   * onto it (`|x - c| = d` and `c - |x - c| = d` have the same two solutions). Every folding step
   * therefore doubles the candidate corners, and the rectangle of the content is the box around
   * all of them: an earlier step's clipping axis can only cut content away from it, never add. */
  blender::Vector<float2> corners;
  corners.append(float2(0.0f, 0.0f));
  corners.append(float2(1.0f, 0.0f));
  corners.append(float2(1.0f, 1.0f));
  corners.append(float2(0.0f, 1.0f));
  for (int i = chain.steps_num - 1; i >= 0; i--) {
    const CompositeMappingTransform &step = chain.steps[i];
    blender::Vector<float2> preimage;
    preimage.reserve(corners.size() * 4);
    for (const float2 &p : corners) {
      const float dx = p.x - step.offset_u;
      const float dy = p.y - step.offset_v;
      /* `S * R` inverts as `R^T * S^-1`: un-scale per axis first, then un-rotate -- the two do not
       * commute, so each product keeps the axis it was divided by. A zero scale axis was
       * normalized to one, so the division is safe. */
      const float2 q((dx * step.cos_rot) / step.scale_x + (dy * step.sin_rot) / step.scale_y,
                     (-dx * step.sin_rot) / step.scale_x + (dy * step.cos_rot) / step.scale_y);
      preimage.append(q);
      if (step.mirror_u) {
        preimage.append(float2(2.0f * step.mirror_center_u - q.x, q.y));
      }
      if (step.mirror_v) {
        preimage.append(float2(q.x, 2.0f * step.mirror_center_v - q.y));
      }
    }
    corners = std::move(preimage);
  }

  float min_u = corners[0].x;
  float min_v = corners[0].y;
  float max_u = min_u;
  float max_v = min_v;
  for (const float2 &p : corners) {
    min_u = std::min(min_u, p.x);
    min_v = std::min(min_v, p.y);
    max_u = std::max(max_u, p.x);
    max_v = std::max(max_v, p.y);
  }
  if (!(std::isfinite(min_u) && std::isfinite(min_v) && std::isfinite(max_u) &&
        std::isfinite(max_v)))
  {
    /* A degenerate frame (an infinite scale the description kept) bounds nothing. */
    return false;
  }
  BLI_rctf_init(&r_uv, min_u, max_u, min_v, max_v);
  return true;
}

int paint_layer_effect_chain_halo(const Material &ma,
                                  const MaterialPaintLayer &row,
                                  const MaterialPaintLayer &effect,
                                  const int channel,
                                  const int width,
                                  const int height)
{
  int halo = 0;
  /* The same walk the reach guard makes, so the two cannot disagree about what feeds the effect:
   * the effects below it in the list, in order, then the effect itself. */
  for (const MaterialPaintLayer &chain_effect : row.effects) {
    if (!paint_layer_effect_filters(ma, chain_effect, channel)) {
      continue;
    }
    float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM];
    paint_layer_effect_filter_params(chain_effect, params);
    /* At the canvas's own size, the upper bound of every radius clamp: a kernel running on a
     * smaller buffer clamps downwards, never past what this halo grew the region by. */
    halo += BKE_paint_layers_effect_halo(
        eMaterialPaintLayerFilter(chain_effect.kind_subtype), params, width, height);
    if (&chain_effect == &effect) {
      break;
    }
  }
  return halo;
}

bool chain_has_whole_map(const Material &ma,
                         const MaterialPaintLayer &row,
                         const MaterialPaintLayer &effect,
                         const int channel)
{
  for (const MaterialPaintLayer &chain_effect : row.effects) {
    if (!paint_layer_effect_filters(ma, chain_effect, channel)) {
      continue;
    }
    float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM];
    paint_layer_effect_filter_params(chain_effect, params);
    if (BKE_paint_layers_effect_reach(eMaterialPaintLayerFilter(chain_effect.kind_subtype),
                                      params) == EffectReach::WholeMap)
    {
      return true;
    }
    if (&chain_effect == &effect) {
      break;
    }
  }
  return false;
}

}  // namespace bke::paint_layers::footprint

/* -------------------------------------------------------------------- */
/** \name Public: the input footprint of an owned effect
 * \{ */

bool BKE_paint_layers_effect_input_footprint_region(const Material &ma,
                                                    const MaterialPaintLayer &row,
                                                    const MaterialPaintLayer &effect,
                                                    const int channel,
                                                    const PaintLayerTileContext &context,
                                                    rcti &r_region)
{
  /* A whole-map kind anywhere in the chain below the effect (it included) makes the result a
   * function of the whole canvas -- a flood's labels or a reduction's sums cannot be narrowed to
   * the footprint's rectangle. The walk stops at the effect itself: the effects above it read its
   * result, they do not feed it. */
  if (bke::paint_layers::footprint::chain_has_whole_map(ma, row, effect, channel)) {
    return false;
  }

  /* Described only: the geometry is in the mappings, and no nested slice or effect buffer is
   * needed to see them. The same call the input hash makes. */
  PaintMaterialCompositeImageLayer entry;
  if (!BKE_paint_layers_effect_input_describe(ma,
                                              row,
                                              effect,
                                              channel,
                                              /*dimensions_only=*/true,
                                              /*describe_only=*/true,
                                              entry,
                                              context))
  {
    return false;
  }
  int width = 0;
  int height = 0;
  bool from_slot = false;
  if (!paint_layer_effect_dimensions_resolve(
          blender::Span<PaintMaterialCompositeImageLayer>(&entry, 1),
          effect,
          context.tile_number,
          width,
          height,
          from_slot) ||
      width <= 0 || height <= 0 || from_slot)
  {
    /* No canvas of its own (a slot-fed input): nothing here bounds where the result lands. */
    return false;
  }

  bool have_uv = false;
  rctf uv = {};
  /* A reader's slice, a generator's pattern and a mesh-map atlas cover the canvas by rules that
   * are not a frame's; a folder's children may or may not move with the folder's own mapping, so
   * a bounded claim could cut real content away. Each refuses the rectangle (v1). */
  if (!BLI_uuid_is_nil(entry.anchor_point_reader) || entry.gradient.enabled ||
      entry.is_mesh_map || !entry.children.empty())
  {
    return false;
  }

  /* The row's own content -- a Fill's flat colour, a painted map's texels -- is what the chain
   * bounds. A row without own content bounds nothing here; the corrections decide below. */
  if (entry.has_constant_color || entry.color_image != nullptr) {
    rctf own_uv;
    if (!bke::paint_layers::footprint::uv_rect(entry.mapping, entry.input_transforms, own_uv)) {
      return false;
    }
    uv_rect_union(uv, have_uv, own_uv);
  }

  for (const PaintMaterialCompositeCorrection &correction : entry.content_corrections) {
    if (correction.filter != 0) {
      /* A filter blends over the row's colour and lays no coverage of its own; its result stays
       * inside the row's footprint grown by the chain's halo. */
      continue;
    }
    if (correction_lays_unbounded_content(correction)) {
      return false;
    }
    rctf laid_uv;
    if (!bke::paint_layers::footprint::uv_rect(correction.mapping, {}, laid_uv)) {
      return false;
    }
    uv_rect_union(uv, have_uv, laid_uv);
  }
  /* The mask items and the row's factor only cut the content away; the rectangle stays an upper
   * bound of where content can lie. */
  if (!have_uv) {
    /* No contributor found a frame: the content's bounds are unknown here (an empty row with
     * nothing laid over it, for one), so the caller keeps the whole canvas. */
    return false;
  }

  /* The evaluator clips at texel centres `((x + 0.5) / W, (y + 0.5) / H)`, so a texel carries the
   * content when its centre lies in the UV rectangle; the one-texel pad absorbs the half-texel
   * centre offset and the rounding at the frame's edge. */
  constexpr int pad = 1;
  /* Clamped before the float-to-int conversion: a mapping with a tiny scale can put the preimage
   * far outside the int range, and converting an out-of-range float is undefined behavior. */
  const auto clamp_texel = [](const double value, const int limit) {
    return int(std::clamp(value, 0.0, double(limit)));
  };
  const int x0 = clamp_texel(std::floor(double(uv.xmin) * width) - pad, width);
  const int x1 = clamp_texel(std::ceil(double(uv.xmax) * width) + pad, width);
  const int y0 = clamp_texel(std::floor(double(uv.ymin) * height) - pad, height);
  const int y1 = clamp_texel(std::ceil(double(uv.ymax) * height) + pad, height);
  if (x0 >= x1 || y0 >= y1) {
    /* The frame misses the canvas entirely. The caller keeps the whole canvas rather than treat
     * an empty region as "nothing to bake" (v1). */
    return false;
  }
  BLI_rcti_init(&r_region, x0, x1, y0, y1);
  return true;
}

/** \} */

}  // namespace blender
