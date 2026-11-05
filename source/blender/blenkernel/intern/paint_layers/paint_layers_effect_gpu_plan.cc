/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The pure half of the GPU effect pass (see the "GPU Effect Pass Plan" section of
 * #BKE_paint_layers_composite.hh): the pass plan a filter's kind and parameters decide, the
 * neighbourhood halo the plan implies, the dispatch rectangle each pass of it computes, and the two
 * GPU-only key fingerprints. No GPU and no dispatch lives here, so the GTests read the same numbers
 * the gather runs, and the gather cannot drift from what the tests hold.
 *
 * The kernels the plan describes are ports of `paint_layer_effect_kernels.cc`, kept in that file's
 * order of operations so `BLENDER_PBR_GPU_VERIFY` compares one number. The file is registered in
 * `blenkernel/CMakeLists.txt` beside the rest of the paint_layers intern sources.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_gradient.hh"

#include "paint_layers_composite_intern.hh"
#include "paint_layers_kind.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Texel-exact description
 *
 * Shared by the slice cache's region-exact check (`paint_layers_composite.cc`) and the GPU effect's
 * one below: both narrow a dispatch to a rectangle reported on a map only where the described input
 * remaps no map.
 * \{ */

/**
 * Whether nothing in \a layer moves a texel of a map: no mapping on the row, its corrections or
 * its mask items, and no reader whose own source would have to be looked into. Every filter and
 * blend a slice can hold works on the texel it is given, so with this a rectangle edited in a map
 * is the same rectangle of the slice.
 */
bool anchor_slice_layer_is_texel_exact(const PaintMaterialCompositeImageLayer &layer)
{
  if (layer.mapping.enabled || !BLI_uuid_is_nil(layer.anchor_point_reader)) {
    return false;
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.mapping.enabled || !BLI_uuid_is_nil(correction.anchor_point_reader)) {
      return false;
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    if (item.mapping.enabled || item.coverage_mapping.enabled ||
        !BLI_uuid_is_nil(item.anchor_point_reader))
    {
      return false;
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    if (!anchor_slice_layer_is_texel_exact(child)) {
      return false;
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Effect Pass Plan (Ф2)
 *
 * The pure shape of a GPU effect pass, decided from the effect's kind and parameters alone. The
 * gather runs the plan over a ping-pong scratch pool; a test reads the same numbers to check the
 * pass count, the per-pass roles, the halo and the region arithmetic without a GPU.
 * \{ */

/** The schema integer parameter at the given index, read back from the float the gather packed. */
static int effect_pass_param_int(const float *params, const int index)
{
  return int(params[index] + 0.5f);
}

/** A neighbourhood radius, clamped the way `paint_layer_effect_blur_box` clamps it. */
static int effect_pass_clamp_radius(const int radius, const int width, const int height)
{
  return std::clamp(radius, 1, std::max(width, height));
}

void BKE_paint_layers_effect_pass_plan(eMaterialPaintLayerFilter filter,
                                       const float *params,
                                       const int width,
                                       const int height,
                                       Vector<PaintLayerEffectPass> &r_passes)
{
  r_passes.clear();
  /* One way to append a pass, so every branch below differs only in the kind it names and the
   * fields it sets on it: the roles (which buffers the pass reads) and the reach follow from the
   * kind, and are spelled on the pass right after, where the reason they hold is. */
  const auto add = [&](const ePaintLayerEffectPass kind, const int radius) {
    PaintLayerEffectPass pass;
    pass.kind = kind;
    pass.radius = radius;
    r_passes.append(pass);
  };
  /* The first pass of the plan reads the input canvas -- there is no destination before it, and a
   * host that runs the plan without knowing the kinds has nothing to bind #Previous to yet -- the
   * window passes after it read the destination the pass before them wrote. */
  const auto window_source = [&]() {
    return r_passes.size() == 1 ? ePaintLayerEffectPassSource::Input :
                                  ePaintLayerEffectPassSource::Previous;
  };
  switch (filter) {
    case MA_PAINT_LAYER_FILTER_NOISE: {
      /* A pointwise kernel: it reads its own texel and nothing else. */
      add(ePaintLayerEffectPass::Noise, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      pass.reach = 0;
      /* The mask is meaningless without a reach, and NONE keeps the reach-0 pass out of the halo's
       * and the regions' per-axis fold. */
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      break;
    }
    case MA_PAINT_LAYER_FILTER_BLUR: {
      const int radius = effect_pass_param_int(params, PAINT_LAYER_BLUR_RADIUS);
      const bool box = effect_pass_param_int(params, PAINT_LAYER_BLUR_SHAPE) ==
                       PAINT_LAYER_BLUR_BOX;
      /* Each box pass reads its own clamped radius; a soft blur spends the reach over three pairs. */
      const int pairs = box ? 1 : 3;
      for (int i = 0; i < pairs; i++) {
        const int pass_radius = effect_pass_clamp_radius(
            box ? radius : std::max(radius / 2, 1), width, height);
        add(ePaintLayerEffectPass::BlurH, pass_radius);
        PaintLayerEffectPass &h = r_passes.last();
        h.source = window_source();
        h.aux = ePaintLayerEffectPassAux::None;
        h.reach = h.radius;
        h.axis_mask = PAINT_LAYER_EFFECT_AXIS_X;
        add(ePaintLayerEffectPass::BlurV, pass_radius);
        PaintLayerEffectPass &v = r_passes.last();
        v.source = window_source();
        v.aux = ePaintLayerEffectPassAux::None;
        v.reach = v.radius;
        v.axis_mask = PAINT_LAYER_EFFECT_AXIS_Y;
      }
      break;
    }
    case MA_PAINT_LAYER_FILTER_SHARPEN: {
      const int radius = effect_pass_clamp_radius(
          effect_pass_param_int(params, PAINT_LAYER_SHARPEN_RADIUS), width, height);
      add(ePaintLayerEffectPass::BlurH, radius);
      PaintLayerEffectPass &h = r_passes.last();
      h.source = window_source();
      h.aux = ePaintLayerEffectPassAux::None;
      h.reach = h.radius;
      h.axis_mask = PAINT_LAYER_EFFECT_AXIS_X;
      add(ePaintLayerEffectPass::BlurV, radius);
      PaintLayerEffectPass &v = r_passes.last();
      v.source = window_source();
      v.aux = ePaintLayerEffectPassAux::None;
      v.reach = v.radius;
      v.axis_mask = PAINT_LAYER_EFFECT_AXIS_Y;
      /* The tail puts the raw detail back on top of the blurred colour, clipped below zero: it
       * reads the input canvas for the detail and the blurred copy for the base, and the Sharpen
       * amount rides in #aux0. */
      add(ePaintLayerEffectPass::UnsharpTail, 0);
      PaintLayerEffectPass &tail = r_passes.last();
      tail.source = ePaintLayerEffectPassSource::Input;
      tail.aux = ePaintLayerEffectPassAux::Previous;
      tail.reach = 0;
      /* The mask is meaningless without a reach. */
      tail.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      tail.aux0 = params[PAINT_LAYER_SHARPEN_AMOUNT];
      break;
    }
    case MA_PAINT_LAYER_FILTER_HIGH_PASS: {
      const bool grayscale = effect_pass_param_int(params, PAINT_LAYER_HIGH_PASS_OUTPUT) ==
                             PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE;
      const int radius = effect_pass_clamp_radius(
          effect_pass_param_int(params, PAINT_LAYER_HIGH_PASS_RADIUS), width, height);
      if (grayscale) {
        /* The luminance goes in before the blur, so the detail is the luminance's own; it reads the
         * input canvas, and its result is the buffer the tail's #Luminance source names. */
        add(ePaintLayerEffectPass::Luminance, 0);
        PaintLayerEffectPass &pass = r_passes.last();
        pass.source = ePaintLayerEffectPassSource::Input;
        pass.aux = ePaintLayerEffectPassAux::None;
        pass.reach = 0;
        /* The mask is meaningless without a reach. */
        pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      }
      add(ePaintLayerEffectPass::BlurH, radius);
      PaintLayerEffectPass &h = r_passes.last();
      h.source = window_source();
      h.aux = ePaintLayerEffectPassAux::None;
      h.reach = h.radius;
      h.axis_mask = PAINT_LAYER_EFFECT_AXIS_X;
      add(ePaintLayerEffectPass::BlurV, radius);
      PaintLayerEffectPass &v = r_passes.last();
      v.source = window_source();
      v.aux = ePaintLayerEffectPassAux::None;
      v.reach = v.radius;
      v.axis_mask = PAINT_LAYER_EFFECT_AXIS_Y;
      /* The tail reads the pre-blur original, which lives in the luminance buffer for a grayscale
       * output and in the raw input otherwise, and the blurred copy; the contrast rides in #aux0.
       * The detail always goes through the sRGB curve, whatever the output is. */
      add(ePaintLayerEffectPass::HighPassTail, 0);
      PaintLayerEffectPass &tail = r_passes.last();
      tail.source = grayscale ? ePaintLayerEffectPassSource::Luminance :
                                ePaintLayerEffectPassSource::Input;
      tail.aux = ePaintLayerEffectPassAux::Previous;
      tail.reach = 0;
      /* The mask is meaningless without a reach. */
      tail.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      tail.aux0 = params[PAINT_LAYER_HIGH_PASS_CONTRAST];
      break;
    }
    case MA_PAINT_LAYER_FILTER_ERODE_DILATE: {
      const int radius = effect_pass_clamp_radius(
          effect_pass_param_int(params, PAINT_LAYER_ERODE_DILATE_RADIUS), width, height);
      const bool dilate = effect_pass_param_int(params, PAINT_LAYER_ERODE_DILATE_MODE) ==
                          PAINT_LAYER_ERODE_DILATE_DILATE;
      add(ePaintLayerEffectPass::ExtremumH, radius);
      PaintLayerEffectPass &h = r_passes.last();
      h.maximum = dilate;
      h.source = window_source();
      h.aux = ePaintLayerEffectPassAux::None;
      h.reach = h.radius;
      h.axis_mask = PAINT_LAYER_EFFECT_AXIS_X;
      add(ePaintLayerEffectPass::ExtremumV, radius);
      PaintLayerEffectPass &v = r_passes.last();
      v.maximum = dilate;
      v.source = window_source();
      v.aux = ePaintLayerEffectPassAux::None;
      v.reach = v.radius;
      v.axis_mask = PAINT_LAYER_EFFECT_AXIS_Y;
      break;
    }
    case MA_PAINT_LAYER_FILTER_EDGE_DETECT: {
      /* The 3x3 window is one texel of reach, and the shader case reads no radius: the kind alone
       * tells the kernel what to tap (the Sobel gradient magnitude here). */
      add(ePaintLayerEffectPass::Edge3x3, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      pass.reach = 1;
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      break;
    }
    case MA_PAINT_LAYER_FILTER_EMBOSS: {
      /* The same 3x3 window as the Edge Detect, one texel of reach, and the shader case reads no
       * radius: the kind alone tells the kernel what to tap (the Emboss diagonal lighting here,
       * of which a flat colour is its own result). */
      add(ePaintLayerEffectPass::Emboss3x3, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      pass.reach = 1;
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      break;
    }
    default:
      /* Not a #PAINT_LAYER_KIND_GPU_COMPUTED kind: an empty plan. The caller refuses it first. */
      break;
  }
  const int count = int(r_passes.size());
  for (int i = 0; i < count; i++) {
    r_passes[i].pass_index = i;
    r_passes[i].pass_count = count;
  }
}

int BKE_paint_layers_effect_halo(eMaterialPaintLayerFilter filter,
                                 const float *params,
                                 const int width,
                                 const int height)
{
  /* One rule decides the halo: the plan. Folding its reaches per axis gives the same numbers the
   * kernels' coverage gave, and keeps the halo, the per-pass regions and what Ф3 adds growing from
   * the same numbers, instead of three places reading the same parameters. */
  Vector<PaintLayerEffectPass> passes;
  BKE_paint_layers_effect_pass_plan(filter, params, width, height, passes);
  int reach_x = 0;
  int reach_y = 0;
  for (const PaintLayerEffectPass &pass : passes) {
    if (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_X) {
      reach_x += pass.reach;
    }
    if (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_Y) {
      reach_y += pass.reach;
    }
  }
  return std::max(reach_x, reach_y);
}

/**
 * Grow \a dirty by \a pad_x and \a pad_y and clamp every side to the canvas
 * `[0, width) x [0, height)`. An empty \a dirty stays empty (nothing moved). The one arithmetic the
 * clip and the per-pass regions share, so the two cannot disagree about a pad past an edge.
 */
static void effect_region_clip_axes(const rcti &dirty,
                                    const int pad_x,
                                    const int pad_y,
                                    const int width,
                                    const int height,
                                    rcti &r_out)
{
  if (BLI_rcti_is_empty(&dirty)) {
    BLI_rcti_init(&r_out, 0, 0, 0, 0);
    return;
  }
  r_out = dirty;
  BLI_rcti_pad(&r_out, pad_x, pad_y);
  r_out.xmin = clamp_i(r_out.xmin, 0, width);
  r_out.ymin = clamp_i(r_out.ymin, 0, height);
  r_out.xmax = clamp_i(r_out.xmax, 0, width);
  r_out.ymax = clamp_i(r_out.ymax, 0, height);
}

void BKE_paint_layers_effect_pass_regions(Span<const PaintLayerEffectPass> passes,
                                          const rcti &dirty,
                                          int extra_input_halo,
                                          const int width,
                                          const int height,
                                          MutableSpan<rcti> r_rects)
{
  BLI_assert(r_rects.size() == passes.size());
  if (BLI_rcti_is_empty(&dirty)) {
    /* Nothing moved, nothing to compute: every pass gets an empty rectangle, and the caller renders
     * the whole canvas instead when the region is not exact. */
    for (const int i : r_rects.index_range()) {
      BLI_rcti_init(&r_rects[i], 0, 0, 0, 0);
    }
    return;
  }
  /* The effects that precede this one in the row grew the changed region by their own halo: their
   * re-rendered band is part of what this effect has to redo. */
  const int extra = std::max(extra_input_halo, 0);
  rcti base = dirty;
  BLI_rcti_pad(&base, extra, extra);
  int total_x = 0;
  int total_y = 0;
  for (const PaintLayerEffectPass &pass : passes) {
    if (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_X) {
      total_x += pass.reach;
    }
    if (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_Y) {
      total_y += pass.reach;
    }
  }
  /* The last pass covers the changed region grown by the plan's whole halo and clamped to the
   * canvas: a stale band a neighbourhood smears past a stroke would otherwise never clear out of
   * the cached result. Each pass before it grows the rectangle once more, by the reach the passes
   * after it still read along each axis -- the suffix of the plan behind it -- so the next pass
   * samples a neighbourhood its source has already computed. The fold is per axis: an H pass after
   * a V one widens the X side only, it does not grow the region's height. */
  int suffix_x = total_x;
  int suffix_y = total_y;
  for (const int p : passes.index_range()) {
    const PaintLayerEffectPass &pass = passes[p];
    /* The suffix is what the passes strictly after this one still read, so this pass's own reach
     * leaves it first: the last pass then lands on exactly the halo. */
    if (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_X) {
      suffix_x -= pass.reach;
    }
    if (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_Y) {
      suffix_y -= pass.reach;
    }
    effect_region_clip_axes(
        base, total_x + suffix_x, total_y + suffix_y, width, height, r_rects[p]);
  }
}

bool BKE_paint_layers_effect_region_is_exact(
    const PaintMaterialCompositeImageLayer &input_entry,
    Span<Image *> images,
    const int width,
    const int height)
{
  /* Rule (2): nothing between a map and the canvas remaps it -- no UV mapping and no live Anchor
   * Point reader anywhere in the described input, the same check the slice cache makes. */
  if (!anchor_slice_layer_is_texel_exact(input_entry)) {
    return false;
  }
  /* Rule (1): a rectangle reported on a map is the same rectangle of the input only where the map
   * has the input's own texel grid. A map no edit was marked on is unchanged, whatever its size. */
  for (const Image *image : images) {
    int image_width = 0;
    int image_height = 0;
    BKE_image_pixel_journal_size(*image, image_width, image_height);
    if (image_width > 0 && (image_width != width || image_height != height)) {
      return false;
    }
  }
  return true;
}

void BKE_paint_layers_effect_region_clip(const rcti &dirty,
                                         const int halo,
                                         const int width,
                                         const int height,
                                         rcti &r_out)
{
  /* The clip is the regions' arithmetic with one pad on both axes. */
  effect_region_clip_axes(dirty, max_ii(halo, 0), max_ii(halo, 0), width, height, r_out);
}

/** Fold \a value into the running FNV-1a \a hash, with the blenlib constants. */
static uint64_t effect_hash_u64(uint64_t hash, const uint64_t value)
{
  return fnv1a_mix(hash, value);
}

/** Fold \a value into the running FNV-1a \a hash, with the blenlib constants. */
static uint64_t effect_hash_u32(uint64_t hash, const uint32_t value)
{
  return fnv1a_mix(hash, value);
}

/** Fold the bits of \a value into the running FNV-1a \a hash. */
static uint64_t effect_hash_float(uint64_t hash, const float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return effect_hash_u32(hash, bits);
}

/**
 * Fold the 16 packed bytes of \a uuid into the running FNV-1a \a hash, as two uint64 halves. The
 * fingerprint is only ever compared within a session, so the byte order of the halves is free as
 * long as this file is the only one that folds it.
 */
static uint64_t effect_hash_uuid(uint64_t hash, const bUUID &uuid)
{
  static_assert(sizeof(bUUID) == sizeof(uint64_t[2]), "the fold reads the uuid's packed 16 bytes");
  uint64_t halves[2];
  memcpy(halves, &uuid, sizeof(halves));
  hash = effect_hash_u64(hash, halves[0]);
  return effect_hash_u64(hash, halves[1]);
}

/**
 * Fold a live Anchor Point reader into the running FNV-1a \a hash: the reader itself, what it reads
 * (content or a mask) and the slice hash of what it currently reads. A map version does not carry a
 * live reader's slice, so the reader keys the entry the same way a flat colour does.
 */
static uint64_t effect_hash_reader(uint64_t hash,
                                   const bUUID &reader,
                                   const int8_t anchor_point_target,
                                   const uint64_t anchor_point_hash)
{
  hash = effect_hash_uuid(hash, reader);
  hash = effect_hash_u32(hash, uint32_t(uint8_t(anchor_point_target)));
  return effect_hash_u64(hash, anchor_point_hash);
}

/**
 * Fold everything of a gradient generator that decides its pixels into the running FNV-1a \a hash:
 * the channel's own value ramp and ends, the map size and the surface key of the exact 2D preview,
 * and the payload's line, shape and ramp. A map version does not carry a gradient's ramp, so the
 * gradient keys the entry the same way a flat colour does.
 */
static uint64_t effect_hash_gradient(uint64_t hash, const PaintMaterialCompositeGradient &gradient)
{
  hash = effect_hash_u32(hash, gradient.color_channel ? 1u : 0u);
  hash = effect_hash_u32(hash, gradient.scalar_ramp_valid ? 1u : 0u);
  hash = effect_hash_float(hash, gradient.scalar_start);
  hash = effect_hash_float(hash, gradient.scalar_end);
  hash = effect_hash_u32(hash, uint32_t(gradient.width));
  hash = effect_hash_u32(hash, uint32_t(gradient.height));
  hash = effect_hash_u64(hash, gradient.surface_key);
  hash = effect_hash_u32(hash, gradient.approximate ? 1u : 0u);
  hash = effect_hash_u64(hash, BKE_paint_layers_gradient_ramp_hash(gradient.scalar_ramp));
  hash = effect_hash_u64(hash, BKE_paint_layers_gradient_ramp_hash(gradient.payload.ramp));
  for (const int i : IndexRange(3)) {
    hash = effect_hash_float(hash, gradient.payload.start[i]);
    hash = effect_hash_float(hash, gradient.payload.end[i]);
  }
  hash = effect_hash_float(hash, gradient.payload.midpoint);
  hash = effect_hash_u32(hash, uint32_t(uint8_t(gradient.payload.space)));
  hash = effect_hash_u32(hash, uint32_t(uint8_t(gradient.payload.type)));
  hash = effect_hash_u32(hash, uint32_t(uint8_t(gradient.payload.repeat)));
  hash = effect_hash_u32(hash, uint32_t(uint8_t(gradient.payload.distribution)));
  return hash;
}

uint64_t BKE_paint_layers_effect_chain_hash(
    Span<const PaintMaterialCompositeCorrection> corrections, const bUUID &effect_marker)
{
  /* The same fields as the byte fold it replaces -- each preceding effect's marker and its own
   * parameter hash, in order -- folded with the blenlib helpers. A non-effect correction (a nil
   * marker, a zero hash) adds nothing. */
  uint64_t hash = fnv1a_offset_basis;
  for (const PaintMaterialCompositeCorrection &correction : corrections) {
    if (BLI_uuid_equal(correction.effect_marker, effect_marker)) {
      /* Only the effects strictly before the target reach its input. */
      break;
    }
    if (correction.effect_hash == 0 || BLI_uuid_is_nil(correction.effect_marker)) {
      /* Not an effect correction (a map, a flat colour, a live reader). */
      continue;
    }
    hash = effect_hash_uuid(hash, correction.effect_marker);
    hash = effect_hash_u64(hash, correction.effect_hash);
  }
  return hash;
}

uint64_t BKE_paint_layers_effect_input_scalar_hash(
    const PaintMaterialCompositeImageLayer &input_entry)
{
  uint64_t hash = fnv1a_offset_basis;
  /* The row's own flat colour and adjustment, which no map version carries. */
  hash = effect_hash_u32(hash, input_entry.has_constant_color ? 1u : 0u);
  if (input_entry.has_constant_color) {
    for (const int c : IndexRange(4)) {
      hash = effect_hash_float(hash, input_entry.constant_color[c]);
    }
  }
  hash = effect_hash_u32(hash, uint32_t(input_entry.adjustment_filter));
  if (input_entry.adjustment_filter != 0) {
    for (const float param : input_entry.adjustment_params) {
      hash = effect_hash_float(hash, param);
    }
  }
  /* The corrections below the effect that lay colour: their opacity, blend, filter and flat colour
   * change the pixels the effect reads, but a map's pixels are already covered by its version. */
  for (const PaintMaterialCompositeCorrection &correction : input_entry.content_corrections) {
    if (correction.opacity <= 0.0f) {
      continue;
    }
    hash = effect_hash_float(hash, correction.opacity);
    hash = effect_hash_u32(hash, uint32_t(int(correction.blend)));
    if (correction.has_constant_color) {
      for (const int c : IndexRange(4)) {
        hash = effect_hash_float(hash, correction.constant_color[c]);
      }
    }
    if (correction.filter != 0) {
      hash = effect_hash_u32(hash, uint32_t(correction.filter));
      for (const float param : correction.filter_params) {
        hash = effect_hash_float(hash, param);
      }
    }
    /* A live reader and a gradient of a correction are keyed like the flat colour they stand in
     * for: a map version does not carry a gradient's ramp or a live reader's slice hash, so a
     * change to one alone would otherwise serve a result computed from the old pixels. */
    if (!BLI_uuid_is_nil(correction.anchor_point_reader)) {
      hash = effect_hash_reader(hash,
                                correction.anchor_point_reader,
                                correction.anchor_point_target,
                                correction.anchor_point_hash);
    }
    if (correction.gradient.enabled) {
      hash = effect_hash_gradient(hash, correction.gradient);
    }
  }
  /* The row's own gradient generator and live reader, keyed the same way a flat colour is. The
   * mask items are not read: the effect input description clears them
   * (#BKE_paint_layers_effect_input_describe), so there is nothing here to name. */
  if (input_entry.gradient.enabled) {
    hash = effect_hash_gradient(hash, input_entry.gradient);
  }
  if (!BLI_uuid_is_nil(input_entry.anchor_point_reader)) {
    hash = effect_hash_reader(hash,
                              input_entry.anchor_point_reader,
                              input_entry.anchor_point_target,
                              input_entry.anchor_point_hash);
  }
  return hash;
}

/** \} */

}  // namespace blender
