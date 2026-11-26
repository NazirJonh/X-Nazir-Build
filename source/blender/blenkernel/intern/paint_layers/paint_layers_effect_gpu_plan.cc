/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The pure half of the GPU effect pass (see the "GPU Effect Pass Plan" section of
 * #BKE_paint_layers_composite.hh): the pass plan a filter's kind and parameters decide, the
 * neighbourhood halo the plan implies, the dispatch rectangle each pass of it computes, and the
 * two input-key fingerprints the effect caches share. No GPU and no dispatch lives here, so the
 * GTests read the same numbers the gather runs, and the gather cannot drift from what the tests
 * hold.
 *
 * The kernels the plan describes are ports of `paint_layer_effect_kernels.cc`, kept in that file's
 * order of operations so `BLENDER_PBR_GPU_VERIFY` compares one number. The file is registered in
 * `blenkernel/CMakeLists.txt` beside the rest of the paint_layers intern sources.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_constants.h"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_gradient.hh"

#include "../paint_material_composite_internal.hh"
#include "paint_layers_composite_intern.hh"
#include "paint_layers_effect_params_layout.hh"
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
  /* A projection moves texels as well: it bakes the map through the object's UV layout. */
  if (layer.mapping.enabled || layer.mapping.projection.present ||
      !BLI_uuid_is_nil(layer.anchor_point_reader))
  {
    return false;
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.mapping.enabled || correction.mapping.projection.present ||
        !BLI_uuid_is_nil(correction.anchor_point_reader))
    {
      return false;
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    if (item.mapping.enabled || item.coverage_mapping.enabled ||
        item.mapping.projection.present || item.coverage_mapping.projection.present ||
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
/** \name GPU Effect Pass Plan
 *
 * The pure shape of a GPU effect pass, decided from the effect's kind and parameters alone. The
 * gather runs the plan over a ping-pong scratch pool; a test reads the same numbers to check the
 * pass count, the per-pass roles, the halo and the region arithmetic without a GPU.
 * \{ */

/** The schema integer parameter at the given index, read back from the float the gather packed. */
static int effect_pass_param_int(const float *params, const int index)
{
  /* Rounded to nearest, not `int(x + 0.5f)`: that truncates toward zero, so a negative spread of
   * -3 would read as -2 and the GPU would erode one texel less than the CPU. */
  return int(std::lround(params[index]));
}

/** A neighbourhood radius, clamped the way `paint_layer_effect_blur_box` clamps it. */
static int effect_pass_clamp_radius(const int radius, const int width, const int height)
{
  return std::clamp(radius, 1, std::max(width, height));
}

/* The kinds whose schema carries a colour ahead of scalars read their packed positions through the
 * layout blocks of #paint_layers_effect_params_layout.hh, the one place they are named: the
 * packing lays a colour's three channels across three slots at its place in the schema order, so
 * the scalars after it sit at positions the kind's own enum (which numbers the schema's
 * parameters, one each) does not name. Every scalar before the colour keeps the enum's own index,
 * which is why the plans below read the leading ones through it and the blocks only past it. */

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
  /* Every reach below is in the texel grid of the effect's input, which is the row already moved by
   * its Transform and Mirror effects: those ride the input description, not the pass chain, so a
   * neighbourhood pass never reads a halo measured before the transform. A Transform above the
   * effect therefore grows the region in the transformed grid the shader samples. */
  /* One horizontal window pass and its vertical twin, both of \a radius: each reads the pass before
   * it and reaches \a radius along its own axis, so the regions fold per axis. The source is asked
   * per pass, so the pair's vertical half reads the horizontal one's result. \a maximum is the
   * Extremum pair's direction; the other window pairs leave it at erode's false. \a alpha_from_kernel
   * is set on both halves: the vertical half reads the horizontal one's alpha, so it has to write
   * its own for the pair to spread the content alpha as the CPU's blur passes do. */
  const auto add_axis_pair = [&](const ePaintLayerEffectPass h_kind,
                                 const ePaintLayerEffectPass v_kind,
                                 const int radius,
                                 const bool maximum = false,
                                 const bool alpha_from_kernel = false) {
    add(h_kind, radius);
    PaintLayerEffectPass &h = r_passes.last();
    h.maximum = maximum;
    h.alpha_from_kernel = alpha_from_kernel;
    h.source = window_source();
    h.aux = ePaintLayerEffectPassAux::None;
    h.reach = h.radius;
    h.axis_mask = PAINT_LAYER_EFFECT_AXIS_X;
    add(v_kind, radius);
    PaintLayerEffectPass &v = r_passes.last();
    v.maximum = maximum;
    v.alpha_from_kernel = alpha_from_kernel;
    v.source = window_source();
    v.aux = ePaintLayerEffectPassAux::None;
    v.reach = v.radius;
    v.axis_mask = PAINT_LAYER_EFFECT_AXIS_Y;
  };
  /* The seed field and the flooding steps the distance transforms (a Bevel, a Mask Outline) share:
   * texels whose scalar sits on the seed side of \a threshold stamp their own coordinates, the
   * rest a far point, then the Jump Flooding steps halve the radius from half the canvas down to
   * one, the standard walk that lets every texel hear of a seed anywhere on it. \a seeds_over: the
   * seeds sit at or over the threshold rather than below it -- the Bevel seeds the scalars short
   * of the half, the Mask Outline seeds whichever side its position and Start From Black make the
   * inside. JFA is not a local kernel -- the nearest seed of a texel may sit across the canvas --
   * so every pass of the transform reaches the whole canvas on both axes: the halo and the
   * per-pass regions of these kinds land on the whole canvas, which the clip trims there anyway,
   * and their region incrementality is off. */
  const auto add_jfa_field = [&](const float source_scalar,
                                 const float threshold,
                                 const bool seeds_over) {
    const int whole = std::max(width, height);
    add(ePaintLayerEffectPass::JfaInit, 0);
    PaintLayerEffectPass &init = r_passes.last();
    init.source = ePaintLayerEffectPassSource::Input;
    init.aux = ePaintLayerEffectPassAux::None;
    init.reach = whole;
    init.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
    init.params.x = source_scalar;
    init.params.y = threshold;
    init.params.z = seeds_over ? 1.0f : 0.0f;
    init.params.w = 0.0f;
    for (int step = whole / 2; step >= 1; step /= 2) {
      add(ePaintLayerEffectPass::JfaStep, step);
      PaintLayerEffectPass &jfa = r_passes.last();
      jfa.source = ePaintLayerEffectPassSource::Previous;
      jfa.aux = ePaintLayerEffectPassAux::None;
      jfa.reach = whole;
      jfa.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
    }
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
      /* Each box pass reads its own clamped radius; a soft blur spends the reach over three pairs.
       * The kind spreads the content alpha along with the colour, so every pass of the chain writes
       * the blurred alpha, the vertical half reading the horizontal one's. */
      const int pairs = box ? 1 : 3;
      for (int i = 0; i < pairs; i++) {
        add_axis_pair(ePaintLayerEffectPass::BlurH,
                      ePaintLayerEffectPass::BlurV,
                      effect_pass_clamp_radius(box ? radius : std::max(radius / 2, 1), width, height),
                      false,
                      true);
      }
      break;
    }
    case MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL: {
      /* One pass of one kernel: the line average at the angle, whose reach walks the line the
       * effect's angle names. The angle and the mix ride in the effect's parameters, as the
       * correction pass is given them. */
      add(ePaintLayerEffectPass::BlurDirectional,
          effect_pass_clamp_radius(
              effect_pass_param_int(params, PAINT_LAYER_BLUR_DIRECTIONAL_RADIUS), width, height));
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      /* A diagonal line reaches along both axes the way a square window of the same radius does,
       * so the halo and the region growth read the reach across both. */
      pass.reach = pass.radius;
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      /* The kernel spreads the content alpha along the same line, so the pass writes it. */
      pass.alpha_from_kernel = true;
      break;
    }
    case MA_PAINT_LAYER_FILTER_SHARPEN: {
      const int radius = effect_pass_clamp_radius(
          effect_pass_param_int(params, PAINT_LAYER_SHARPEN_RADIUS), width, height);
      add_axis_pair(ePaintLayerEffectPass::BlurH, ePaintLayerEffectPass::BlurV, radius);
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
      add_axis_pair(ePaintLayerEffectPass::BlurH, ePaintLayerEffectPass::BlurV, radius);
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
      add_axis_pair(ePaintLayerEffectPass::ExtremumH, ePaintLayerEffectPass::ExtremumV, radius, dilate);
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
    case MA_PAINT_LAYER_FILTER_FXAA: {
      /* The same 3x3 window as the Edge Detect, one texel of reach, and the shader case reads no
       * radius: the kind alone tells the kernel what to tap (the luminance edge blend here, of
       * which a flat ring is its own result). */
      add(ePaintLayerEffectPass::Fxaa, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      pass.reach = 1;
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      break;
    }
    case MA_PAINT_LAYER_FILTER_GLOW: {
      /* The CPU kernel (`paint_layer_effect_glow_apply`) builds the glow in a private copy of the
       * buffer: the scene-linear luminance choked by the spread, blurred by the width, screened
       * under the colour at the opacity. The plan lays the same chain over the ping-pong pair, the
       * tail reading the original from the input canvas and the blurred band from the pass before
       * it, so no private copy is kept. */
      const int mode = effect_pass_param_int(params, PAINT_LAYER_GLOW_MODE);
      const int blur_width = effect_pass_param_int(params, PAINT_LAYER_GLOW_WIDTH);
      /* The luminance and the choke are pointwise: the choke clamps the luminance to the glow band,
       * `m = clamp((luma - spread) / max(1 - spread, 1e-6), 0, 1)`, the spread riding in #aux0, and
       * its result is the buffer the blur reads. */
      add(ePaintLayerEffectPass::Luminance, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      pass.reach = 0;
      /* The mask is meaningless without a reach. */
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      add(ePaintLayerEffectPass::Choke, 0);
      PaintLayerEffectPass &choke = r_passes.last();
      choke.source = ePaintLayerEffectPassSource::Previous;
      choke.aux = ePaintLayerEffectPassAux::None;
      choke.reach = 0;
      /* The mask is meaningless without a reach. */
      choke.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      choke.aux0 = params[PAINT_LAYER_GLOW_SPREAD];
      /* One box pass is precise; three soften into a gaussian falloff -- the same radii the CPU's
       * `paint_layer_effect_blur_soft` spends. */
      const bool precise = mode == PAINT_LAYER_GLOW_PRECISE;
      const int pairs = precise ? 1 : 3;
      for (int i = 0; i < pairs; i++) {
        add_axis_pair(ePaintLayerEffectPass::BlurH,
                      ePaintLayerEffectPass::BlurV,
                      effect_pass_clamp_radius(precise ? blur_width : std::max(blur_width / 2, 1),
                                               width,
                                               height));
      }
      /* The tail screens the blurred band under the colour at the opacity, per channel
       * `out += tint * (glow * opacity) * (1 - out)`: a black texel lands on the tint, a lit one
       * keeps its light. The opacity rides in #aux0, the tint in #params (the packing lays a
       * colour's three channels at its place in the schema order), and it computes its own texel
       * only, so it reaches nothing. It lifts the alpha where the screen lands, `max(alpha, m)`, so
       * the glow spreads the row's coverage; the passes before it keep their own alpha. */
      add(ePaintLayerEffectPass::ScreenTail, 0);
      PaintLayerEffectPass &tail = r_passes.last();
      tail.source = ePaintLayerEffectPassSource::Input;
      tail.aux = ePaintLayerEffectPassAux::Previous;
      tail.reach = 0;
      /* The mask is meaningless without a reach. */
      tail.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      tail.alpha_from_kernel = true;
      tail.aux0 = params[PAINT_LAYER_GLOW_OPACITY];
      tail.params.x = params[PAINT_LAYER_GLOW_COLOR + 0];
      tail.params.y = params[PAINT_LAYER_GLOW_COLOR + 1];
      tail.params.z = params[PAINT_LAYER_GLOW_COLOR + 2];
      tail.params.w = 0.0f;
      break;
    }
    case MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL: {
      /* The CPU kernel (`paint_layer_effect_height_to_normal_apply`) is the Sobel gradients of the
       * height's red channel, alpha-weighted to the neutral where the row has no coverage, encoded
       * as the tangent-space normal. One 3x3 pass: it reads one texel past on both axes like the
       * Edge Detect, and the height is a function of the texel, so no pre-pass and no second
       * buffer. */
      const float strength = params[PAINT_LAYER_HEIGHT_TO_NORMAL_STRENGTH];
      float scale = strength;
      if (effect_pass_param_int(params, PAINT_LAYER_HEIGHT_TO_NORMAL_WORLD_UNITS) != 0) {
        /* The multiplier is the physical slope instead: the height delta's centimetres against the
         * texel's own, the texel the wider side of the canvas covers, and the half that turns the
         * Sobel's doubled gradient into that slope. Without a surface the world units name no
         * slope, and the normal stays flat. The canvas size is all this reads, which the plan was
         * given, so the plan can hand the kernel the final number. */
        const float texel_cm = params[PAINT_LAYER_HEIGHT_TO_NORMAL_SURFACE_SIZE] /
                               float(std::max(width, height));
        scale = (texel_cm > 0.0f) ?
                    0.5f * params[PAINT_LAYER_HEIGHT_TO_NORMAL_HEIGHT_DEPTH] / texel_cm :
                    0.0f;
      }
      /* The normal leans against the slope; the DirectX encoding flips the green. #params names the
       * convention -- +1 the OpenGL encoding, -1 the DirectX one -- which the kernel negates into
       * the CPU's own green sign (a flipped green reads `ny = +dh_dv`, an unflipped one `-dh_dv`). */
      const float green_convention = effect_pass_param_int(params,
                                                           PAINT_LAYER_HEIGHT_TO_NORMAL_FLIP_GREEN) != 0 ?
                                         -1.0f :
                                         1.0f;
      add(ePaintLayerEffectPass::HeightToNormal3x3, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      pass.reach = 1;
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      pass.aux0 = scale;
      pass.params.x = green_convention;
      pass.params.y = 0.0f;
      pass.params.z = 0.0f;
      pass.params.w = 0.0f;
      break;
    }
    case MA_PAINT_LAYER_FILTER_PIXELATE: {
      /* The CPU kernel (`paint_layer_effect_pixelate_apply`) cuts the map into \a amount blocks per
       * side, a block at least one texel and the last one of a side shorter. One pass: each texel
       * averages the alpha-weighted colour of the block it belongs to out of the original, so it
       * reads its own block and nothing else -- no neighbourhood, no halo. The amount rides in
       * #aux0, the block count per side, and the kernel cuts the canvas into that many blocks per
       * axis like the CPU does, the last one of a side shorter at the edge. */
      const int pixels = std::max(effect_pass_param_int(params, PAINT_LAYER_PIXELATE_AMOUNT), 1);
      add(ePaintLayerEffectPass::BlockAverage, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::None;
      pass.reach = 0;
      /* The mask is meaningless without a reach. */
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      pass.aux0 = float(pixels);
      break;
    }
    case MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT: {
      /* The CPU kernel (`paint_layer_effect_channel_split_apply`) samples the row's slot map at the
       * texel's own UV, bilinearly, and lays one channel of it as a grey. One pass: the slot map is
       * bound outside the ping-pong pair -- the aux names it, the gather refuses the effect when the
       * description names no slot image -- and the channel index rides in #aux0. A flat colour
       * reads its own texel only, so it reaches nothing. */
      const int channel = std::clamp(
          effect_pass_param_int(params, PAINT_LAYER_CHANNEL_SPLIT_CHANNEL),
          int(PAINT_LAYER_CHANNEL_SPLIT_RED),
          int(PAINT_LAYER_CHANNEL_SPLIT_ALPHA));
      add(ePaintLayerEffectPass::SlotFetch, 0);
      PaintLayerEffectPass &pass = r_passes.last();
      pass.source = ePaintLayerEffectPassSource::Input;
      pass.aux = ePaintLayerEffectPassAux::Slot;
      pass.reach = 0;
      /* The mask is meaningless without a reach. */
      pass.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      pass.aux0 = float(channel);
      break;
    }
    case MA_PAINT_LAYER_FILTER_BEVEL: {
      /* The CPU kernel (`paint_layer_effect_bevel_apply`) seeds every texel whose scalar -- the
       * alpha, or the luminance the source names -- sits below one half and walks the distance to
       * the nearest seed out of them, writing the ramp `clamp(dist / reach, 0, 1)` as a grey into
       * all three channels, the reach the distance parameter, softened by the smooth blur. The
       * plan floods the same field with the JFA passes and reads the ramp off it: the Euclid
       * distance the flood builds is the CPU's own round-corner metric (its 3-4 chamfer walk
       * normalized by a third), so the GPU bevels round whatever the corner type says -- the
       * chessboard walk the Angular corner names is no pass of this plan. A row with no seed is
       * one everywhere on both, the far point of the seed field clamping the ramp to one. */
      const int source = effect_pass_param_int(params, PAINT_LAYER_BEVEL_SOURCE);
      const int distance = effect_pass_param_int(params, PAINT_LAYER_BEVEL_DISTANCE);
      const int smooth = effect_pass_param_int(params, PAINT_LAYER_BEVEL_SMOOTH);
      const int whole = std::max(width, height);
      add_jfa_field(float(source), 0.5f, false);
      /* The ramp reads the field the flooding built: the divisor is the distance parameter, at
       * least one the way the CPU's own is. #params.y is the Outline band's bend, which the ramp
       * mode does not read, and #params.z names the ramp mode. */
      add(ePaintLayerEffectPass::DistEval, 0);
      PaintLayerEffectPass &ramp = r_passes.last();
      ramp.source = ePaintLayerEffectPassSource::Previous;
      ramp.aux = ePaintLayerEffectPassAux::None;
      ramp.reach = whole;
      ramp.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      ramp.params.x = float(std::max(distance, 1));
      ramp.params.y = 0.0f;
      ramp.params.z = 0.0f;
      ramp.params.w = 0.0f;
      /* The smooth blur the CPU lays over the finished ramp, one pair like a box blur's. */
      if (smooth > 0) {
        add_axis_pair(ePaintLayerEffectPass::BlurH,
                      ePaintLayerEffectPass::BlurV,
                      effect_pass_clamp_radius(smooth, width, height));
      }
      break;
    }
    case MA_PAINT_LAYER_FILTER_MASK_OUTLINE: {
      /* The CPU kernel (`paint_layer_effect_mask_outline_apply`) seeds one side of the border the
       * threshold crosses in the grey, walks the distance field out of the seeds, and lays the
       * band `1 - clamp(dist / reach, 0, 1)`, bent toward the smoothstep by the curve shape, on
       * the side opposite the seeds -- softened by the blur, then tinted premultiplied with the
       * band as its own alpha. The plan lays the same chain; the seed pass's own rule seeds the
       * scalar below the threshold, which is the CPU's seed side when Start From Black is on (an
       * exact `grey == threshold` texel apart) or the position is Inside, and the mirrored one
       * when the CPU seeds what crosses the threshold -- the one polarity the seed pass names is
       * the contract here, and no parameter of this plan moves it. The Double position runs the
       * same single chain, the CPU's single-position walk: its second field seeds the other side
       * of the border, which the same seed pass cannot name, so the GPU band lies on one side
       * only. A shape with no texel on the seed side floods nothing, and the band reads as
       * infinitely far -- no band, the CPU's own unseen walk. */
      const int source = effect_pass_param_int(params, PAINT_LAYER_MASK_OUTLINE_SOURCE);
      const float threshold = params[PAINT_LAYER_MASK_OUTLINE_THRESHOLD];
      const int band_width = effect_pass_param_int(params, PAINT_LAYER_MASK_OUTLINE_WIDTH);
      const int blur = effect_pass_param_int(params, PAINT_LAYER_MASK_OUTLINE_BLUR);
      const float shape = params[PAINT_LAYER_MASK_OUTLINE_SHAPE];
      const bool alpha_source = source == PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA;
      const int whole = std::max(width, height);
      /* The seeds are the CPU's inside side, which the position and the Start From Black lay out:
       * inside is the greys at or under the threshold from black, at or over it otherwise, and the
       * Inside position seeds the outside instead -- so the seed side crosses the threshold from
       * above exactly when an Inside position starts from black or an Outside / Double one does
       * not. The Double position still runs the single chain below, its band on one side only. */
      const int position = effect_pass_param_int(params, PAINT_LAYER_MASK_OUTLINE_POSITION);
      const bool start_black = params[PAINT_LAYER_MASK_OUTLINE_START_BLACK] != 0.0f;
      const bool seeds_over = (position == PAINT_LAYER_MASK_OUTLINE_INSIDE) ? start_black :
                                                                            !start_black;
      add_jfa_field(float(source), threshold, seeds_over);
      /* The band's divisor is the width parameter, at least one like the CPU's own; the bend and
       * the band mode ride beside it. */
      add(ePaintLayerEffectPass::DistEval, 0);
      PaintLayerEffectPass &band = r_passes.last();
      band.source = ePaintLayerEffectPassSource::Previous;
      band.aux = ePaintLayerEffectPassAux::None;
      band.reach = whole;
      band.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      band.params.x = float(std::max(band_width, 1));
      band.params.y = shape;
      band.params.z = 1.0f;
      band.params.w = 0.0f;
      /* The blur the CPU lays over the finished band. */
      if (blur > 0) {
        add_axis_pair(ePaintLayerEffectPass::BlurH,
                      ePaintLayerEffectPass::BlurV,
                      effect_pass_clamp_radius(blur, width, height));
      }
      /* The tint tail turns the blurred band into the premultiplied ring: the colour at the band,
       * the band as its own alpha. The colour is white when the shape reads the alpha -- the CPU
       * forces its own white there, the panel hiding the colour and a value left from an earlier
       * luminance source not tinting the ring. */
      add(ePaintLayerEffectPass::BandTintTail, 0);
      PaintLayerEffectPass &tail = r_passes.last();
      tail.source = ePaintLayerEffectPassSource::Previous;
      tail.aux = ePaintLayerEffectPassAux::None;
      tail.reach = 0;
      /* The mask is meaningless without a reach. */
      tail.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      tail.params2.x = alpha_source ? 1.0f : params[PAINT_LAYER_MASK_OUTLINE_COLOR + 0];
      tail.params2.y = alpha_source ? 1.0f : params[PAINT_LAYER_MASK_OUTLINE_COLOR + 1];
      tail.params2.z = alpha_source ? 1.0f : params[PAINT_LAYER_MASK_OUTLINE_COLOR + 2];
      tail.params2.w = 0.0f;
      tail.alpha_from_kernel = true;
      break;
    }
    case MA_PAINT_LAYER_FILTER_DROP_SHADOW: {
      /* The CPU kernel (`paint_layer_effect_drop_shadow_apply`) clones the buffer into the grey
       * silhouette the source names, grows (a positive spread) or shrinks (a negative one) it by
       * the extremum pair, blurs it by the size, then reads the clone back at the offset the
       * angle casts -- past the map's edge there is no silhouette to cast -- and lays the colour
       * premultiplied by `shadow * opacity * under`, the alpha source keeping the shadow off the
       * content (`1 - alpha`), the luminance one laying it wherever the silhouette covers. The
       * plan lays the same chain over the ping-pong pair, the tail reading the original for the
       * under factor and the blurred silhouette through its aux, its own alpha written. */
      const int source = effect_pass_param_int(params, PAINT_LAYER_DROP_SHADOW_SOURCE);
      const float angle_rad = params[PAINT_LAYER_DROP_SHADOW_ANGLE] * float(M_PI) / 180.0f;
      const int distance = effect_pass_param_int(params, PAINT_LAYER_DROP_SHADOW_DISTANCE);
      const int size = effect_pass_param_int(params, PAINT_LAYER_DROP_SHADOW_SIZE);
      const int spread = effect_pass_param_int(params, PAINT_LAYER_DROP_SHADOW_SPREAD);
      const float opacity = params[PAINT_LAYER_DROP_SHADOW_OPACITY];
      add(ePaintLayerEffectPass::Silhouette, 0);
      PaintLayerEffectPass &shape = r_passes.last();
      shape.source = ePaintLayerEffectPassSource::Input;
      shape.aux = ePaintLayerEffectPassAux::None;
      shape.reach = 0;
      /* The mask is meaningless without a reach. */
      shape.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      shape.params.x = float(source);
      shape.params.y = 0.0f;
      shape.params.z = 0.0f;
      shape.params.w = 0.0f;
      /* The spread's extremum pair, dilating when the spread grows the silhouette and eroding
       * when it shrinks it, the radius its absolute value; zero of either leaves it as it is. */
      if (spread != 0) {
        add_axis_pair(ePaintLayerEffectPass::ExtremumH,
                      ePaintLayerEffectPass::ExtremumV,
                      effect_pass_clamp_radius(std::abs(spread), width, height),
                      spread > 0);
      }
      if (size > 0) {
        add_axis_pair(ePaintLayerEffectPass::BlurH,
                      ePaintLayerEffectPass::BlurV,
                      effect_pass_clamp_radius(size, width, height));
      }
      add(ePaintLayerEffectPass::OffsetTintTail, 0);
      PaintLayerEffectPass &tail = r_passes.last();
      tail.source = ePaintLayerEffectPassSource::Input;
      tail.aux = ePaintLayerEffectPassAux::Previous;
      /* The tail reads the blurred silhouette the offset lands on, so its output depends on the
       * plan's input that far back: the reach the cast moves, the larger of the two axes'
       * offsets, rounded half up like the CPU's own. The other axis pads a little wide, which
       * the clip trims. */
      const int offset_x = int(std::floor(std::cos(angle_rad) * float(distance) + 0.5f));
      const int offset_y = int(std::floor(std::sin(angle_rad) * float(distance) + 0.5f));
      tail.reach = std::max(std::abs(offset_x), std::abs(offset_y));
      tail.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      /* The cast's direction and the opacity ride in #params, the tint and the under flag in
       * #params2: one when the alpha source keeps the shadow off the content, zero when the
       * luminance one lays it wherever the silhouette covers. */
      tail.params.x = std::cos(angle_rad);
      tail.params.y = std::sin(angle_rad);
      tail.params.z = float(distance);
      tail.params.w = opacity;
      tail.params2.x = params[PAINT_LAYER_DROP_SHADOW_COLOR + 0];
      tail.params2.y = params[PAINT_LAYER_DROP_SHADOW_COLOR + 1];
      tail.params2.z = params[PAINT_LAYER_DROP_SHADOW_COLOR + 2];
      tail.params2.w = (source == PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA) ? 1.0f : 0.0f;
      tail.alpha_from_kernel = true;
      break;
    }
    case MA_PAINT_LAYER_FILTER_BLUR_SLOPE: {
      /* The CPU kernel (`paint_layer_effect_blur_slope_apply`) builds the driver -- the input's
       * luminance, blurred first when the source mode is the previous input and the blur is on,
       * or the seeded value noise the tilling tiles -- re-graded by the position and the
       * contrast, then walks the gradient's direction over the original: the taps the quality
       * names, spanning the intensity, blended by the mode. The GPU splits it the same way, the
       * walk reading the original through its aux -- the taps are not a pass output, so they grow
       * no region; the gradient the walk follows reads one texel past of the driver on both
       * axes, which is the reach. A flat slope reads as it is on both, the taps of a walk whose
       * driver is flat being the texel's own. */
      const int source_mode = effect_pass_param_int(params, PAINT_LAYER_BLUR_SLOPE_SOURCE_MODE);
      const int source_blur = effect_pass_param_int(params, PAINT_LAYER_BLUR_SLOPE_SOURCE_BLUR);
      const float position = params[PAINT_LAYER_BLUR_SLOPE_SOURCE_POSITION];
      const float contrast = params[PAINT_LAYER_BLUR_SLOPE_SOURCE_CONTRAST];
      const float seed = params[PAINT_LAYER_BLUR_SLOPE_SEED];
      const float tilling = params[PAINT_LAYER_BLUR_SLOPE_SOURCE_TILLING];
      const int quality = effect_pass_param_int(params, PAINT_LAYER_BLUR_SLOPE_QUALITY);
      const int blending = effect_pass_param_int(params, PAINT_LAYER_BLUR_SLOPE_BLENDING);
      const float intensity = params[PAINT_LAYER_BLUR_SLOPE_INTENSITY];
      /* The blur rides only on the input mode: the noise has no detail of its own to soften, the
       * CPU blurring inside the previous-input branch alone. */
      const bool driver_blurred = source_mode == PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT &&
                                  source_blur > 0;
      if (driver_blurred) {
        add_axis_pair(ePaintLayerEffectPass::BlurH,
                      ePaintLayerEffectPass::BlurV,
                      effect_pass_clamp_radius(source_blur, width, height));
      }
      add(ePaintLayerEffectPass::SlopeDriver, 0);
      PaintLayerEffectPass &driver = r_passes.last();
      /* The driver reads the blurred input when the blur pair ran and the input itself when it
       * did not. */
      driver.source = driver_blurred ? ePaintLayerEffectPassSource::Previous :
                                       ePaintLayerEffectPassSource::Input;
      driver.aux = ePaintLayerEffectPassAux::None;
      driver.reach = 0;
      /* The mask is meaningless without a reach. */
      driver.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      /* The mode is the CPU's own enum, the noise zero and the previous input one. */
      driver.aux0 = float(source_mode);
      /* The re-grade and the noise's seed and tiles ride in #params, in the contract's order. */
      driver.params.x = position;
      driver.params.y = contrast;
      driver.params.z = seed;
      driver.params.w = tilling;
      add(ePaintLayerEffectPass::SlopeWalk, std::clamp(quality, 1, 32));
      PaintLayerEffectPass &walk = r_passes.last();
      walk.source = ePaintLayerEffectPassSource::Previous;
      walk.aux = ePaintLayerEffectPassAux::Original;
      walk.reach = 1;
      walk.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      /* The walk's radius is the tap count the quality names, clamped like the CPU's own; the
       * blend mode and the span ride in #params. */
      walk.params.x = float(blending);
      walk.params.y = intensity;
      walk.params.z = 0.0f;
      walk.params.w = 0.0f;
      break;
    }
    case MA_PAINT_LAYER_FILTER_COLOR_MATCH: {
      /* The CPU kernel (`paint_layer_effect_color_match_apply`) moves every texel by the HSL
       * distance between the target colour and the source one -- the input's own alpha-weighted
       * average, or the colour a pick laid over the schema -- with the optional per-texel jitter on
       * top. The average is a statistic of the whole map, so the plan reduces it first: a pyramid of
       * alpha-weighted colour sums, the first level halving the canvas and every step after it
       * halving one more, down to the 1x1 apex the grade reads through its aux. A reduction is no
       * local kernel either -- the sum of any block is fed by the whole map -- so every pass of the
       * chain reaches the whole canvas on both axes and the region incrementality is off, the JFA's
       * own policy: the halo and the per-pass regions of these kinds land on the whole canvas, which
       * the clip trims there anyway. */
      /* The schema packs the two colours across three slots each, so the scalars after them sit at
       * the packed positions the constants above name: the picked flag leads, then the source and
       * the target colours, then the variation switch with its three amounts riding beside it in
       * the effect's own parameters (in the schema order the correction pass is given them). */
      const bool picked = params[PAINT_LAYER_COLOR_MATCH_SOURCE_PICKED] != 0.0f;
      if (picked) {
        /* The picked branch is the CPU's: the frozen contract lays the picked colour in the grade's
         * #params2, whose consumer names that slot the match's second colour while the CPU plays it
         * as the source the match moves *from* -- the one reading this file cannot check against the
         * CPU kernel alone. An empty plan refuses the GPU path the same way an unflagged kind is,
         * and the CPU lays the picked match exactly as it always has. */
        break;
      }
      const int whole = std::max(width, height);
      /* The first level is the canvas halved; a step's radius names the level it makes, and the
       * chain runs while that level still holds a texel -- the last one is the 1x1 apex. */
      add(ePaintLayerEffectPass::ReduceStart, 1);
      PaintLayerEffectPass &start = r_passes.last();
      start.source = ePaintLayerEffectPassSource::Input;
      start.aux = ePaintLayerEffectPassAux::None;
      start.reach = whole;
      start.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      for (int level = 2; (whole >> level) >= 1; level++) {
        add(ePaintLayerEffectPass::ReduceStep, level);
        PaintLayerEffectPass &step = r_passes.last();
        step.source = ePaintLayerEffectPassSource::Previous;
        step.aux = ePaintLayerEffectPassAux::None;
        step.reach = whole;
        step.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      }
      /* The grade reads the original for the colours it moves and the pyramid's apex for the
       * average it moves them by -- the whole-map field the reduce passes left, outside the
       * ping-pong pair, the same role the flood field plays for the Fill Area's passes. The target
       * colour and the variation switch ride in #params, in the schema order; #params2 stays empty,
       * the picked colour's slot the reduce branch does not name. */
      add(ePaintLayerEffectPass::MatchGrade, 0);
      PaintLayerEffectPass &grade = r_passes.last();
      grade.source = ePaintLayerEffectPassSource::Input;
      grade.aux = ePaintLayerEffectPassAux::Field;
      grade.reach = 0;
      /* The mask is meaningless without a reach. */
      grade.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      grade.params.x = params[bke::paint_filter_params::ColorMatch::target_color + 0];
      grade.params.y = params[bke::paint_filter_params::ColorMatch::target_color + 1];
      grade.params.z = params[bke::paint_filter_params::ColorMatch::target_color + 2];
      grade.params.w = params[bke::paint_filter_params::ColorMatch::variation];
      /* The variation's three amounts ride in #params2 -- the shader reads them there, the packed
       * first float4 the correction pass pushes carrying the picked flag and the source colour
       * instead. */
      grade.params2.x = params[bke::paint_filter_params::ColorMatch::variation_hue];
      grade.params2.y = params[bke::paint_filter_params::ColorMatch::variation_chroma];
      grade.params2.z = params[bke::paint_filter_params::ColorMatch::variation_luma];
      grade.params2.w = 0.0f;
      break;
    }
    case MA_PAINT_LAYER_FILTER_FILL_AREA: {
      /* The CPU kernel (`paint_layer_effect_fill_area_apply`) classifies the regions a continuous
       * border closes off -- the border the scalar's threshold crosses, the background the one an
       * open border leaves reachable from the map's edge -- then fills them with the border's own
       * colours diffused inward, or the solid colour the mode names, and softens the result by the
       * blur. The plan floods the same classification: the label field stamps the border and the
       * map's edge the background zero and every other texel its own coordinates, the flooding steps
       * swallow the regions the edge reaches, the diffusion walks the border's colours inward one
       * pass at a time, and the tail writes the fill out with the coverage the flooding named. The
       * flooding is not a local kernel -- a region's label is fed by the whole map -- so the init
       * and the steps reach the whole canvas on both axes and the region incrementality is off, the
       * JFA's own policy; the diffusion reads its four neighbours only, one texel of reach. */
      /* The schema packs the fill colour across three slots, so the blur and the iterations after
       * it sit at the packed positions the layout block names; the scalars before it keep the
       * enum's own index. */
      const int source = effect_pass_param_int(params, PAINT_LAYER_FILL_AREA_SOURCE);
      const float threshold = params[PAINT_LAYER_FILL_AREA_THRESHOLD];
      const bool debug = params[PAINT_LAYER_FILL_AREA_DEBUG] != 0.0f;
      const int uv_border = effect_pass_param_int(params, PAINT_LAYER_FILL_AREA_UV_BORDER);
      const int color_mode = effect_pass_param_int(params, PAINT_LAYER_FILL_AREA_COLOR_MODE);
      const int blur = effect_pass_param_int(params, bke::paint_filter_params::FillArea::blur);
      const int iterations = effect_pass_param_int(
          params, bke::paint_filter_params::FillArea::iterations);
      /* The parameterizations the flood cannot express are the CPU's, each refused with an empty
       * plan the gather falls back on: the debug lays a grey ramp of the CPU's own region numbers,
       * which the flooding's labels do not name; the largest-region classification asks a reduction
       * over the regions' areas, outside this phase's walks (the edge-based one is the flood's to
       * name, the map's edge stamped the background); and the source mode with no iteration has
       * nothing to reach in with -- the CPU clears the layer, which no chain of these passes writes.
       * The solid mode needs no diffusion, so it runs the flood straight into the tail. */
      if (debug || uv_border != PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE ||
          (color_mode == PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE && iterations <= 0))
      {
        break;
      }
      const int whole = std::max(width, height);
      /* The label field's seeding: #params names the scalar the border is read at (the alpha, or
       * the luminance the source names -- a mask item never dispatches here, the mask form refusing
       * the GPU path by its kind) and the threshold the border crosses. The polarity is the CPU's
       * own fixed one: a scalar at or over the threshold is border. */
      add(ePaintLayerEffectPass::FloodInit, 0);
      PaintLayerEffectPass &init = r_passes.last();
      init.source = ePaintLayerEffectPassSource::Input;
      init.aux = ePaintLayerEffectPassAux::None;
      init.reach = whole;
      init.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
      init.params.x = float(source);
      init.params.y = threshold;
      init.params.z = 0.0f;
      init.params.w = 0.0f;
      /* The flooding is the raster sweep pair the shader runs -- one pass walking the rows, one the
       * columns, each a whole line to the thread that owns it -- because a jump flooding step
       * strides past a wall thinner than its radius, and a wall is exactly what a region is. Every
       * pair turns the wavefront once, so sixteen of them flood a background the edge reaches
       * through sixteen turns; a more winding one stays enclosed, the GPU's own bound the CPU's
       * serial walk does not have. The field is global either way -- the reaches stay whole-canvas
       * -- and the mode rides in #params, the radius left empty. */
      constexpr int FILL_AREA_SWEEP_PAIRS = 16;
      for (int sweep = 0; sweep < FILL_AREA_SWEEP_PAIRS; sweep++) {
        for (const float mode : {1.0f, 2.0f}) {
          add(ePaintLayerEffectPass::FloodStep, 0);
          PaintLayerEffectPass &flood = r_passes.last();
          flood.source = ePaintLayerEffectPassSource::Previous;
          flood.aux = ePaintLayerEffectPassAux::None;
          flood.reach = whole;
          flood.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
          flood.params.x = mode;
          flood.params.y = 0.0f;
          flood.params.z = 0.0f;
          flood.params.w = 0.0f;
        }
      }
      /* The diffusion, the source mode's alone: the first step reads the original, which is where
       * the CPU seeds its walk -- the border texels' own (unpremultiplied) colours, the enclosed
       * ones at zero -- and every step after it reads the pass before it, the CPU's ping-pong. The
       * radius names the iteration; the label field rides the aux, so a step keeps its neighbours
       * inside the region they belong to. */
      if (color_mode == PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE) {
        for (int pass = 1; pass <= iterations; pass++) {
          add(ePaintLayerEffectPass::DiffuseStep, pass);
          PaintLayerEffectPass &diffuse = r_passes.last();
          diffuse.source = (pass == 1) ? ePaintLayerEffectPassSource::Input :
                                         ePaintLayerEffectPassSource::Previous;
          diffuse.aux = ePaintLayerEffectPassAux::Field;
          diffuse.reach = 1;
          diffuse.axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
        }
      }
      /* The tail writes the fill out ahead of the blur pair, the CPU's own order: the diffused
       * colour (or the solid one in #params2) on the enclosed texels, everything else empty, so
       * the border's own colours do not smear into the softened edge. The coverage it softens
       * beside the colour is the tail's to compute from the field -- the box window sum of the
       * enclosed indicator, the two axis passes composed -- with the blur riding in #radius; the
       * alpha it writes then survives the pair, whose store carries the centre texel's alpha
       * through and touches the colour only. */
      add(ePaintLayerEffectPass::FillTail, blur);
      PaintLayerEffectPass &tail = r_passes.last();
      tail.source = ePaintLayerEffectPassSource::Previous;
      tail.aux = ePaintLayerEffectPassAux::Field;
      tail.reach = 0;
      /* The mask is meaningless without a reach. */
      tail.axis_mask = PAINT_LAYER_EFFECT_AXIS_NONE;
      tail.params.x = float(color_mode);
      tail.params.y = 0.0f;
      tail.params.z = 0.0f;
      tail.params.w = 0.0f;
      tail.params2.x = params[bke::paint_filter_params::FillArea::color + 0];
      tail.params2.y = params[bke::paint_filter_params::FillArea::color + 1];
      tail.params2.z = params[bke::paint_filter_params::FillArea::color + 2];
      tail.params2.w = 0.0f;
      tail.alpha_from_kernel = true;
      /* The blur the CPU lays over the finished fill: the colour, the tail's alpha riding along. */
      if (blur > 0) {
        add_axis_pair(ePaintLayerEffectPass::BlurH,
                      ePaintLayerEffectPass::BlurV,
                      effect_pass_clamp_radius(blur, width, height));
      }
      break;
    }
    default:
      /* Not a #PAINT_LAYER_KIND_GPU_COMPUTED kind, so an empty plan -- the cases above are every
       * kind the registry flags. What is left refuses the GPU path first: the pointwise filters the
       * preview's apply_filter branch lays instead of this pass chain, and the parameterizations the
       * cases above refuse with their own empty plan (a picked Color Match source, the Fill Area's
       * largest-region classification, its debug ramp and its iteration-less source mode). */
      break;
  }
  const int count = int(r_passes.size());
  for (int i = 0; i < count; i++) {
    r_passes[i].pass_index = i;
    r_passes[i].pass_count = count;
  }
}

/** The plan's halo: its reaches folded per axis, the larger total. Shared by the public halo and
 * the coverage reach so a kind can only be classified one way. */
static int effect_plan_halo(Span<PaintLayerEffectPass> passes)
{
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

int BKE_paint_layers_effect_halo(eMaterialPaintLayerFilter filter,
                                 const float *params,
                                 const int width,
                                 const int height)
{
  /* One rule decides the halo: the plan. Folding its reaches per axis gives the same numbers the
   * kernels' coverage gave, and keeps the halo, the per-pass regions and the reach of the kinds
   * still to come growing from the same numbers, instead of three places reading the same
   * parameters. */
  Vector<PaintLayerEffectPass> passes;
  BKE_paint_layers_effect_pass_plan(filter, params, width, height, passes);
  return effect_plan_halo(passes);
}

EffectReach BKE_paint_layers_effect_reach(eMaterialPaintLayerFilter filter, const float *params)
{
  /* The coverage culling reads this as what the CPU *kernel* depends on, not what a GPU pass chain
   * reads: these kinds have a plan now, but their result at a texel is fed by the whole canvas (a
   * distance field, a flood, a statistic) or by the canvas size (the block grid, the UV a slot is
   * sampled at, the world scale), and a plan evaluated at the one-texel bound below would classify
   * them as pointwise or local and let a stroke elsewhere skip their bake. The kind table names
   * them once (#PAINT_LAYER_KIND_EFFECT_WHOLE_MAP), so a new such kind is one registry flag, not
   * a second list to keep in step with the tests' own. */
  if (paint_layer_effect_whole_map(int16_t(filter))) {
    return EffectReach::WholeMap;
  }
  /* The plan's one-texel size bound only clamps a radius upwards to one, so it classifies every
   * kind without measuring a canvas. An empty plan is a kind the plan does not describe (a
   * whole-map pass such as a Color Match) and never counts as pointwise. */
  Vector<PaintLayerEffectPass> passes;
  BKE_paint_layers_effect_pass_plan(filter, params, 1, 1, passes);
  if (passes.is_empty()) {
    return EffectReach::WholeMap;
  }
  return effect_plan_halo(passes) == 0 ? EffectReach::Pointwise : EffectReach::Neighbourhood;
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
    /* The images come from the description's own maps, which are never null. */
    BLI_assert(image != nullptr);
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

/** Whether \a kind is one whose output at a texel is fed by the whole canvas. */
static bool effect_pass_kind_is_whole_canvas(const ePaintLayerEffectPass kind)
{
  switch (kind) {
    /* The walks and the tails that read what they left. */
    case ePaintLayerEffectPass::ReduceStart:
    case ePaintLayerEffectPass::ReduceStep:
    case ePaintLayerEffectPass::FloodInit:
    case ePaintLayerEffectPass::FloodStep:
    case ePaintLayerEffectPass::DiffuseStep:
    case ePaintLayerEffectPass::FillTail:
    case ePaintLayerEffectPass::JfaInit:
    case ePaintLayerEffectPass::JfaStep:
    case ePaintLayerEffectPass::DistEval:
    /* Kernels that address the canvas itself: a block cut, a shadow offset read past the canvas
     * edge, the slope walk's taps and the reduction's grade. Their filters carry
     * #EffectReach::WholeMap today; the steps are listed so a plan that named one anyway could not
     * narrow the stage, whatever reach it was handed. */
    case ePaintLayerEffectPass::Choke:
    case ePaintLayerEffectPass::ScreenTail:
    case ePaintLayerEffectPass::BlockAverage:
    case ePaintLayerEffectPass::HeightToNormal3x3:
    case ePaintLayerEffectPass::OffsetTintTail:
    case ePaintLayerEffectPass::SlopeWalk:
    case ePaintLayerEffectPass::MatchGrade:
    /* The Drop Shadow's silhouette reads the source by UV over the whole map. */
    case ePaintLayerEffectPass::Silhouette:
      return true;
    default:
      return false;
  }
}

bool BKE_paint_layers_effect_input_region_supported(Span<const PaintLayerEffectPass> passes,
                                                    const EffectReach reach,
                                                    const bool region_exact)
{
  /* The two rules the caller already answers, plus the plan's own: a whole-map kind carries the
   * registry's flag (#EffectReach::WholeMap), and a plan whose walk reads the whole canvas can
   * never be narrowed whatever the registry says. The steps, not the filter kinds, are the guard,
   * so a future whole-canvas pass refuses the region the moment its plan names it. */
  if (!region_exact || reach == EffectReach::WholeMap || passes.is_empty()) {
    return false;
  }
  for (const PaintLayerEffectPass &pass : passes) {
    if (effect_pass_kind_is_whole_canvas(pass.kind)) {
      return false;
    }
  }
  return true;
}

bool BKE_paint_layers_effect_input_region(Span<const PaintLayerEffectPass> passes,
                                          Span<const rcti> pass_rects,
                                          const int width,
                                          const int height,
                                          rcti &r_out)
{
  BLI_rcti_init(&r_out, 0, 0, 0, 0);
  if (passes.is_empty() || pass_rects.size() != passes.size() || width <= 0 || height <= 0) {
    return false;
  }
  /* The union of what each pass reads, grown by that pass's own reach: an H pass grows the X side
   * only, a V pass the Y one, so the two axes of a separable chain fold independently. */
  bool any = false;
  rcti stage;
  BLI_rcti_init(&stage, 0, 0, 0, 0);
  for (const int i : passes.index_range()) {
    const rcti &rect = pass_rects[i];
    if (BLI_rcti_is_empty(&rect)) {
      /* Nothing of this pass has to be computed, so it reads nothing of the input. */
      continue;
    }
    const PaintLayerEffectPass &pass = passes[i];
    const int pad_x = (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_X) != 0 ?
                          std::max(pass.reach, 0) :
                          0;
    const int pad_y = (pass.axis_mask & PAINT_LAYER_EFFECT_AXIS_Y) != 0 ?
                          std::max(pass.reach, 0) :
                          0;
    rcti grown = rect;
    BLI_rcti_pad(&grown, pad_x, pad_y);
    if (!any) {
      stage = grown;
      any = true;
    }
    else {
      BLI_rcti_union(&stage, &grown);
    }
  }
  if (!any) {
    return false;
  }
  /* One texel of margin: a bilinear sample on the stage's last row or column reaches half a texel
   * outside it. */
  BLI_rcti_pad(&stage, 1, 1);
  stage.xmin = clamp_i(stage.xmin, 0, width);
  stage.ymin = clamp_i(stage.ymin, 0, height);
  stage.xmax = clamp_i(stage.xmax, 0, width);
  stage.ymax = clamp_i(stage.ymax, 0, height);
  if (BLI_rcti_is_empty(&stage)) {
    return false;
  }
  r_out = stage;
  return true;
}

/** Fold \a value into the running FNV-1a \a hash, with the blenlib constants. */
static uint64_t effect_hash_u64(uint64_t hash, const uint64_t value)
{
  return fnv1a_mix(hash, value);
}

/** Fold the bits of \a value into the running FNV-1a \a hash. */
static uint64_t effect_hash_float(uint64_t hash, const float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return effect_hash_u64(hash, bits);
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
  hash = effect_hash_u64(hash, uint32_t(uint8_t(anchor_point_target)));
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
  hash = effect_hash_u64(hash, gradient.color_channel ? 1u : 0u);
  hash = effect_hash_u64(hash, gradient.scalar_ramp_valid ? 1u : 0u);
  hash = effect_hash_float(hash, gradient.scalar_start);
  hash = effect_hash_float(hash, gradient.scalar_end);
  hash = effect_hash_u64(hash, uint32_t(gradient.width));
  hash = effect_hash_u64(hash, uint32_t(gradient.height));
  hash = effect_hash_u64(hash, gradient.surface_key);
  hash = effect_hash_u64(hash, gradient.approximate ? 1u : 0u);
  hash = effect_hash_u64(hash, BKE_paint_layers_gradient_ramp_hash(gradient.scalar_ramp));
  hash = effect_hash_u64(hash, BKE_paint_layers_gradient_ramp_hash(gradient.payload.ramp));
  for (const int i : IndexRange(3)) {
    hash = effect_hash_float(hash, gradient.payload.start[i]);
    hash = effect_hash_float(hash, gradient.payload.end[i]);
  }
  hash = effect_hash_float(hash, gradient.payload.midpoint);
  hash = effect_hash_u64(hash, uint32_t(uint8_t(gradient.payload.space)));
  hash = effect_hash_u64(hash, uint32_t(uint8_t(gradient.payload.type)));
  hash = effect_hash_u64(hash, uint32_t(uint8_t(gradient.payload.repeat)));
  hash = effect_hash_u64(hash, uint32_t(uint8_t(gradient.payload.distribution)));
  return hash;
}

uint64_t BKE_paint_layers_effect_chain_hash(
    Span<const PaintMaterialCompositeCorrection> corrections, const bUUID &effect_marker)
{
  /* The same fields as the byte fold it replaces -- each preceding effect's marker and its own
   * parameter hash, in order -- folded with the blenlib helpers. A non-effect correction adds
   * nothing. */
  uint64_t hash = fnv1a_offset_basis;
  for (const PaintMaterialCompositeCorrection &correction : corrections) {
    if (BLI_uuid_equal(correction.effect_marker, effect_marker)) {
      /* Only the effects strictly before the target reach its input. */
      break;
    }
    if (!paint_correction_is_effect(correction)) {
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
  hash = effect_hash_u64(hash, input_entry.has_constant_color ? 1u : 0u);
  if (input_entry.has_constant_color) {
    for (const int c : IndexRange(4)) {
      hash = effect_hash_float(hash, input_entry.constant_color[c]);
    }
  }
  /* A projected input is baked through a frame no map version carries: moving the frame moves the
   * pixels the effect reads, so it keys the result like the flat colour does. */
  hash = composite_mapping_hash(effect_hash_u64(hash, input_entry.mapping.enabled ? 1u : 0u),
                                input_entry.mapping);
  /* The transform steps (Mirror, Transform Mapping effects) move the pixels the effect reads like
   * the frame does, and the stack hash folds them the same way, so a step that moves the CPU key
   * moves this key too. The two keys are built by different hash functions and never compared. */
  for (const PaintMaterialCompositeMapping &step : input_entry.input_transforms) {
    hash = composite_mapping_hash(effect_hash_u64(hash, 1u), step);
  }
  hash = effect_hash_u64(hash, uint32_t(input_entry.adjustment_filter));
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
    hash = effect_hash_u64(hash, uint32_t(int(correction.blend)));
    hash = composite_mapping_hash(effect_hash_u64(hash, correction.mapping.enabled ? 1u : 0u),
                                  correction.mapping);
    if (correction.has_constant_color) {
      for (const int c : IndexRange(4)) {
        hash = effect_hash_float(hash, correction.constant_color[c]);
      }
    }
    if (correction.filter != 0) {
      hash = effect_hash_u64(hash, uint32_t(correction.filter));
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
  /* The slot map a Channel Split's kernel samples itself stands outside the input's maps: no map
   * version of the described input carries its pixels, so its identity keys the entry -- assigning
   * another image must serve a result computed from the new one, the way the CPU effect hash folds
   * it (#paint_layer_effect_hash_mix_slot). The session uid is the fold both keys use. */
  if (input_entry.slot_image != nullptr) {
    hash = effect_hash_u64(hash, input_entry.slot_image->id.session_uid);
  }
  return hash;
}

/** \} */

}  // namespace blender
