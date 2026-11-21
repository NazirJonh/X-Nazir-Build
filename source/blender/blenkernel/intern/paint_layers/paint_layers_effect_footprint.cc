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

#include "BKE_paint.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_tile.hh"

#include "paint_layers_composite_intern.hh"
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

/**
 * The pixel rectangle of \a uv on a \a width by \a height canvas, shared by every
 * UV-rectangle-to-region conversion in this file so the footprint callers agree about the pixels a
 * mapping bounds. The evaluator clips at texel centres `((x + 0.5) / W, (y + 0.5) / H)`, so a
 * texel carries the content when its centre lies in the UV rectangle; the \a pad absorbs the
 * half-texel centre offset and the rounding at the frame's edge. The max edges are exclusive: the
 * whole canvas reads back as `xmax == width`, `ymax == height`.
 *
 * False when the frame misses the canvas entirely: the caller keeps the whole canvas rather than
 * treat an empty region as "nothing to bake" (v1).
 */
bool uv_rect_to_region(const rctf &uv, const int width, const int height, const int pad, rcti &r_region)
{
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
    return false;
  }
  BLI_rcti_init(&r_region, x0, x1, y0, y1);
  return true;
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

bool row_source_region(const Material &ma,
                       const MaterialPaintLayer &row,
                       const PaintLayerTileContext &context,
                       const int size,
                       rcti &r_region)
{
  /* A folder's bake composites its children, which carry mappings of their own: the parent's read
   * chain does not bound what they lay. */
  if (!row.children.is_empty()) {
    return false;
  }
  /* A row the planner keeps live is never baked: its own mapping is applied by the graph when the
   * map is read, not laid into the map, so the described chain would bound the live read rather
   * than the map's content. The same blocked test the planner's candidate check reads. */
  if (BKE_paint_layers_bake_blocked(ma, row)) {
    return false;
  }
  /* A frame off the UV plane (a projection kind, a Decal frame) reads the surface or the Decal
   * map, which the described mapping does not carry: the described numbers would bound nothing. */
  if (row.mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_UV) {
    return false;
  }
  /* The mapping is common to the row, so the channel it is described on only decides which of the
   * row's own maps the entry names. Prefer a channel that carries the row's own map: the authoring
   * policy can leave empty records for channels the row never got a map for, and the described
   * entry is what proves the row lays content. */
  int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  if (row.channels != nullptr && row.channels_num > 0) {
    channel = int(row.channels[0].channel);
    for (int index = 0; index < row.channels_num; index++) {
      if (row.channels[index].image != nullptr) {
        channel = int(row.channels[index].channel);
        break;
      }
    }
  }
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }

  /* Per channel the bake renders: a correction that lays its own maps reads through its own frame,
   * which the row's chain does not bound, and a whole-map kind anywhere in the chain makes the
   * bake a function of the whole canvas (v1 refuses instead of unioning laid frames). A CPU effect
   * that filters nothing lays nothing in the bake either, so it refuses nothing here. The halo the
   * chain's filters reach grows the rectangle at the end; the largest channel's is the one every
   * channel's result stays inside. */
  const MaterialPaintLayer *last_effect = static_cast<const MaterialPaintLayer *>(row.effects.last);
  int max_halo = 0;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const int effect_channel = int(info.channel);
    for (const MaterialPaintLayer &effect : row.effects) {
      if (!paint_layer_effect_is_cpu_computed(effect) &&
          !paint_layer_effect_filters(ma, effect, effect_channel) &&
          paint_layer_effect_lays(ma, effect, effect_channel))
      {
        return false;
      }
    }
    if (last_effect != nullptr) {
      if (chain_has_whole_map(ma, row, *last_effect, effect_channel)) {
        return false;
      }
      max_halo = std::max(
          max_halo,
          paint_layer_effect_chain_halo(ma, row, *last_effect, effect_channel, size, size));
    }
  }

  /* The same described entry the row's bake render reads from, at the dimensions call the render
   * makes first: its own mapping, its transform effects' steps and its corrections, described from
   * the live source, never from the maps being updated. */
  PaintMaterialCompositeImageLayer entry;
  const int effective_tile = BKE_paint_layers_tile_resolve(
      context.tile_number, nullptr, context.painted_object);
  if (!composite_image_layer_build(ma,
                                   row,
                                   channel,
                                   1.0f,
                                   /*isolating=*/false,
                                   /*ignore_own_bake=*/true,
                                   entry,
                                   effective_tile,
                                   /*stop_before=*/nullptr,
                                   /*slice_rect=*/nullptr,
                                   /*dimensions_only=*/true,
                                   /*describe_only=*/true,
                                   /*context_surface=*/nullptr,
                                   /*height_won_cache=*/nullptr,
                                   paint_layer_bake_scope(row, false),
                                   context.painted_object))
  {
    return false;
  }
  /* The same refusals the effect footprint makes about a described row: a reader lays the slice it
   * points at, and a generator, a Mesh Map or a folder's children cover the canvas by rules that
   * are not a frame's. */
  if (!BLI_uuid_is_nil(entry.anchor_point_reader) || entry.gradient.enabled ||
      entry.is_mesh_map || !entry.children.empty())
  {
    return false;
  }
  /* The row's own content -- a Fill's flat colour, a map's texels, the source's rendered map -- is
   * what the chain bounds. A row that lays nothing of its own bounds nothing here. The mask and
   * the opacity only cut the content away, so the rectangle stays an upper bound of where it can
   * lie. */
  if (!entry.has_constant_color && entry.color_image == nullptr) {
    return false;
  }
  rctf uv;
  if (!uv_rect(entry.mapping, entry.input_transforms, uv)) {
    return false;
  }
  if (!uv_rect_to_region(uv, size, size, 1, r_region)) {
    /* The frame misses the canvas entirely. */
    return false;
  }
  if (max_halo > 0) {
    /* The bake render lays the chain's filters' results over the row's content, and a
     * neighbourhood kernel reaches past the content by its own halo: the maps' content ends at the
     * footprint grown by the chain's reach, so that is what the caller may render alone. */
    BKE_paint_layers_effect_region_clip(r_region, max_halo, size, size, r_region);
  }
  return true;
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

  /* The one-texel pad absorbs the half-texel centre offset and the rounding at the frame's edge;
   * the rest of the conversion is the shared one. */
  constexpr int pad = 1;
  return uv_rect_to_region(uv, width, height, pad, r_region);
}

/** \} */

}  // namespace blender
