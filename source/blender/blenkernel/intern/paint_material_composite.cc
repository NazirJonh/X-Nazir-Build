/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <cstring>
#include <memory>

namespace blender {

/* The image change log the composite cache subscribes to. Brought in wholesale because the switch
 * over #ePartialUpdateCollectResult reads badly with the full qualification on every label. */
using namespace bke::image::partial_update;


Span<int> BKE_paint_material_composite_passes()
{
  /* Base Color first, then the scalars and colours a PBR material is normally authored with, in
   * Principled's own order, then the two roles that are not Principled inputs at all. Ambient
   * Occlusion is baked by the user rather than wired, and a mask belongs to the layer, not to the
   * shader.
   *
   * Height is deliberately absent: it has a descriptor and an identifier, but no part in the
   * Combined preview's shading, and listing it would offer a pass the preview visibly ignores. */
  static const int passes[] = {
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      PAINT_MATERIAL_CHANNEL_METALLIC,
      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
      PAINT_MATERIAL_CHANNEL_SPECULAR,
      PAINT_MATERIAL_CHANNEL_NORMAL,
      PAINT_MATERIAL_CHANNEL_AO,
      PAINT_MATERIAL_CHANNEL_ALPHA,
      PAINT_MATERIAL_CHANNEL_EMISSION,
      PAINT_LAYER_MAP_MASK,
  };
  return Span<int>(passes, ARRAY_SIZE(passes));
}

Span<int> BKE_paint_material_display_passes()
{
  /* Combined leads, as it does in the Compositor. Kept out of #BKE_paint_material_composite_passes
   * because every consumer of that list treats its values as roles -- indexing a per-channel
   * array, looking the value up in the channel descriptor table, resolving a layer map -- and none
   * of those is meaningful for a display mode. A second list means no existing loop has to learn
   * about it. */
  static const int passes[] = {
      PAINT_LAYER_PASS_COMBINED,
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      PAINT_MATERIAL_CHANNEL_METALLIC,
      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
      PAINT_MATERIAL_CHANNEL_SPECULAR,
      PAINT_MATERIAL_CHANNEL_NORMAL,
      PAINT_MATERIAL_CHANNEL_AO,
      PAINT_MATERIAL_CHANNEL_ALPHA,
      PAINT_MATERIAL_CHANNEL_EMISSION,
      PAINT_LAYER_MAP_MASK,
  };
  return Span<int>(passes, ARRAY_SIZE(passes));
}


/* -------------------------------------------------------------------- */
/** \name Evaluation
 * \{ */

/**
 * Layer buffers are required to be byte RGBA.
 *
 * A float buffer is not rejected for lack of a conversion but for lack of a *correct* one: its
 * values are scene-referred, and turning them into the display-referred bytes the composite is
 * made of is a colour management step, not a multiply. Converting the #Image in place, as the
 * only cheap alternative, would also mutate data the compositing caller does not own. A material
 * whose layers are float therefore goes to the bake, which renders through the display pipeline
 * anyway.
 */
static bool composite_ibuf_is_byte_rgba(const ImBuf *ibuf, const int width, const int height)
{
  if (ibuf == nullptr || ibuf->byte_buffer.data == nullptr) {
    return false;
  }
  if (ibuf->x != width || ibuf->y != height) {
    return false;
  }
  return ELEM(ibuf->channels, 0, 4);
}

/** Masks are read as a factor, so a float mask needs no colour transform and is accepted. */
static bool composite_mask_ibuf_is_valid(const ImBuf *ibuf, const int width, const int height)
{
  if (ibuf == nullptr) {
    return false;
  }
  if (ibuf->x != width || ibuf->y != height) {
    return false;
  }
  return ibuf->byte_buffer.data != nullptr || ibuf->float_buffer.data != nullptr;
}

/**
 * Read one straight scene-linear RGBA sample of \a ibuf at (x, y).
 *
 * A byte buffer is straight; a float one is straight-straightened before its colorspace is decoded,
 * and the buffer's own colorspace is what is decoded -- the same path the colour tiles take. A
 * mask or a mask correction is data in practice, so the conversion is a no-op for them; it exists
 * so a map that is somehow tagged sRGB cannot make the CPU and the shader disagree.
 */
static void composite_read_sample_linear(const ImBuf *ibuf,
                                         const int x,
                                         const int y,
                                         float rgba[4])
{
  const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
  const bool four = channels == 4;
  if (ibuf->byte_buffer.data != nullptr) {
    const uchar *pixel = ibuf->byte_data() + (int64_t(y) * ibuf->x + x) * 4;
    rgba[0] = float(pixel[0]) / 255.0f;
    rgba[1] = float(pixel[1]) / 255.0f;
    rgba[2] = float(pixel[2]) / 255.0f;
    rgba[3] = float(pixel[3]) / 255.0f;
  }
  else {
    const float *pixel = ibuf->float_buffer.data + (int64_t(y) * ibuf->x + x) * channels;
    rgba[0] = pixel[0];
    rgba[1] = pixel[1];
    rgba[2] = pixel[2];
    rgba[3] = four ? pixel[3] : 1.0f;
    if (rgba[3] > 0.0f) {
      const float inv = 1.0f / rgba[3];
      rgba[0] *= inv;
      rgba[1] *= inv;
      rgba[2] *= inv;
    }
  }
  const ColorSpace *colorspace = (ibuf->byte_buffer.data != nullptr) ?
                                     ibuf->byte_buffer.colorspace :
                                     ibuf->float_buffer.colorspace;
  if (colorspace != nullptr && !IMB_colormanagement_space_is_data(colorspace) &&
      !IMB_colormanagement_space_is_scene_linear(colorspace))
  {
    IMB_colormanagement_colorspace_to_scene_linear_v4(rgba, false, colorspace);
  }
}

static float mask_factor_at(
    const ImBuf *mask_ibuf, const bool from_alpha, const int x, const int y, const float influence)
{
  if (mask_ibuf == nullptr || influence <= 0.0f) {
    return 1.0f;
  }
  float rgba[4];
  composite_read_sample_linear(mask_ibuf, x, y, rgba);
  const float mask_value = from_alpha ? rgba[3] : (rgba[0] + rgba[1] + rgba[2]) / 3.0f;
  return (1.0f - influence) + influence * clamp_f(mask_value, 0.0f, 1.0f);
}

/**
 * The color and alpha of a mask-correction pixel: the mean of its stored RGB and its stored alpha.
 * The map is stored straight, like every paint-layer map, so the mean already is the coverage
 * colour `C`; there is no un-premultiply divide. The graph reads the same straight bytes, after its
 * texture upload has pre-multiplied and its Divide has undone that, so the two agree texel for
 * texel.
 */
static void correction_mask_coverage_at(const ImBuf *ibuf,
                                        const int x,
                                        const int y,
                                        float &r_gray,
                                        float &r_alpha)
{
  float rgba[4];
  composite_read_sample_linear(ibuf, x, y, rgba);
  r_gray = clamp_f((rgba[0] + rgba[1] + rgba[2]) / 3.0f, 0.0f, 1.0f);
  r_alpha = clamp_f(rgba[3], 0.0f, 1.0f);
}

/**
 * Lay the tangent-space normal \a top over \a bottom, both encoded in [0, 1].
 *
 * The whiteout blend: decode both, add the detail map's slope to the base map's, keep the product
 * of their z, renormalize. \a fac interpolates in normal space rather than on the encoded bytes,
 * so a partial factor tilts the result towards the base normal instead of towards grey.
 */
static void blend_normal_combine(const float bottom[4], const float top[4], float r_rgb[3])
{
  const float3 base = float3(bottom[0], bottom[1], bottom[2]) * 2.0f - 1.0f;
  const float3 detail = float3(top[0], top[1], top[2]) * 2.0f - 1.0f;
  const float3 combined = math::normalize(
      float3(base.x + detail.x, base.y + detail.y, base.z * detail.z));
  copy_v3_v3(r_rgb, combined * 0.5f + 0.5f);
}

/** The scalar `blend_value`'s `ramp_blend` equivalent, for mask corrections. */
static float blend_value_ramp(const float bottom,
                              const float top,
                              const CompositeBlend blend,
                              const float fac)
{
  if (blend == CompositeBlend::NormalCombine) {
    const float bottom_rgba[4] = {bottom, bottom, bottom, 1.0f};
    const float top_rgba[4] = {top, top, top, 1.0f};
    float combined[3];
    blend_normal_combine(bottom_rgba, top_rgba, combined);
    return (combined[0] + combined[1] + combined[2]) / 3.0f;
  }
  float rgba[4] = {bottom, bottom, bottom, 1.0f};
  const float top_rgba[4] = {top, top, top, 1.0f};
  ramp_blend(int(blend), rgba, clamp_f(fac, 0.0f, 1.0f), top_rgba);
  return rgba[0];
}

/**
 * Blend one row of a scene-linear pixel: the shader's own `ramp_blend` for every colour mode, and
 * the whiteout for #CompositeBlend::NormalCombine, which no Mix node expresses.
 *
 * \a top carries the row's coverage in its alpha; Mix mixes it like the node does, the other modes
 * keep the destination's. Nothing is clamped beyond the factor: Add is meant to exceed one, and the
 * encode step is where a byte output is finally clamped. This is the one implementation of the
 * colour formulas; the shader reaches the same `ramp_blend`.
 */
static void blend_row_linear(float dst[4],
                             const float top[4],
                             const CompositeBlend blend,
                             const float factor)
{
  const float fac = clamp_f(factor, 0.0f, 1.0f);
  if (blend == CompositeBlend::NormalCombine) {
    float combined[3];
    blend_normal_combine(dst, top, combined);
    for (const int i : IndexRange(3)) {
      dst[i] = dst[i] * (1.0f - fac) + combined[i] * fac;
    }
    return;
  }
  ramp_blend(int(blend), dst, fac, top);
}

/**
 * The factor a layer that carries corrections blends by, at one pixel (spec 18 §5.3), steps A-C.
 *
 * The coverage starts at the layer's mask, its own alpha, or full. Each content correction
 * accumulates its own coverage the way the engine's "over" pair does -- `a = a + f * (1 - a)` --
 * and mask corrections then blend onto that factor, after #mask_influence, which the base coverage
 * already carries: the mask image establishes the coverage, the corrections sit on top of it. A
 * mask image owns the factor on its own, and what the content corrections accumulate goes to the
 * layer's alpha instead.
 *
 * Shared by #composite_correction_pixel_apply and the mask baker: B is meant to be exactly the
 * factor the composite blends by, so the two must read the same expression.
 *
 * \param offset: the pixel's offset into every layer buffer, which all match the stack dimensions.
 */
/**
 * The straight alpha of a colour sample at (x, y), from a byte or float buffer.
 *
 * A byte buffer is straight already; a float one is premultiplied, so the stored alpha is what
 * unpremultiplied the colour during decode and is the coverage either way.
 */
static float composite_color_alpha_at(const ImBuf *ibuf, const int x, const int y)
{
  if (ibuf == nullptr) {
    return 0.0f;
  }
  const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
  if (ibuf->byte_buffer.data != nullptr) {
    return channels == 4 ? float(ibuf->byte_data()[(int64_t(y) * ibuf->x + x) * 4 + 3]) / 255.0f :
                           1.0f;
  }
  const float *pixel = ibuf->float_buffer.data + (int64_t(y) * ibuf->x + x) * channels;
  return channels == 4 ? pixel[3] : 1.0f;
}

static float composite_correction_pixel_mask_factor(const PaintMaterialCompositeLayer &layer,
                                                    const int x,
                                                    const int y)
{
  /* Two coverages that multiply: the mask (its own image, times a second baked coverage) and the
   * layer's own content coverage (its map alpha, or full). The content corrections accumulate into
   * the second one only, so a mask always clips them: a correction cannot bring coverage in where
   * the mask is zero. With no mask at all, an alpha-driven layer covers by its own alpha -- where
   * an absent base has none, and the corrections are what bring coverage in. */
  float mask_coverage = 1.0f;
  if (layer.mask_ibuf != nullptr) {
    mask_coverage = mask_factor_at(layer.mask_ibuf,
                                   layer.mask_from_alpha && !layer.mask_reads_grey,
                                   x,
                                   y,
                                   layer.mask_influence);
  }
  if (layer.has_coverage_constant) {
    /* A live Material source alpha: the same constant the generator builds for the factor base. */
    mask_coverage *= layer.coverage_constant;
  }
  else if (layer.coverage_ibuf != nullptr) {
    /* A second coverage (a Material layer's source transparency) multiplies the mask. A baked
     * coverage is read as its grey like the generator's chain; a live source alpha map is read as
     * its alpha, the output the generator's factor uses. */
    mask_coverage *= mask_factor_at(
        layer.coverage_ibuf, layer.coverage_from_alpha, x, y, 1.0f);
  }
  float alpha = 1.0f;
  if (layer.color_alpha_coverage && layer.color_ibuf != nullptr) {
    alpha = composite_color_alpha_at(layer.color_ibuf, x, y);
  }
  else if (layer.mask_ibuf == nullptr && layer.mask_from_alpha) {
    alpha = composite_color_alpha_at(layer.color_ibuf, x, y);
  }

  for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.content_corrections) {
    if (!correction.enabled || correction.ibuf == nullptr) {
      continue;
    }
    /* The correction's colour arrives through its own coverage: its map alpha. */
    const float corr_alpha = composite_color_alpha_at(correction.ibuf, x, y);
    /* The correction's coverage reaches the factor by the same Multiply the graph uses. */
    const float fac = clamp_f(correction.opacity * corr_alpha, 0.0f, 1.0f);
    alpha = alpha + fac * (1.0f - alpha);
  }

  float mask_factor = mask_coverage * alpha;
  /* Each mask item lays its straight coverage `C` over the factor with the over formula, in list
   * order with the last entry on top. MIX replaces the factor with `C`, the colour the map stores;
   * MULTIPLY darkens it by `C`, the same `F * (1 - A * op) + F * C * (A * op)` the generator
   * builds. Every other mode reads as MIX. */
  for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.mask_corrections) {
    if (!correction.enabled) {
      continue;
    }
    float gray = 0.0f;
    float corr_alpha = 0.0f;
    if (correction.has_constant_color) {
      gray = (correction.constant_color[0] + correction.constant_color[1] +
              correction.constant_color[2]) /
             3.0f;
      corr_alpha = 1.0f;
    }
    else if (correction.ibuf != nullptr) {
      correction_mask_coverage_at(correction.ibuf, x, y, gray, corr_alpha);
      /* The map is stored straight, like every paint-layer map, so its grey is the colour `C`
       * directly: no un-premultiply here. The graph reaches the same `C` by dividing the texture,
       * which the upload pre-multiplied, by alpha. `mix(F, C, A * op)` is
       * `F * (1 - A * op) + C * A * op`. */
    }
    else {
      continue;
    }
    const float fac = clamp_f(corr_alpha * correction.opacity, 0.0f, 1.0f);
    const float gray_clamped = clamp_f(gray, 0.0f, 1.0f);
    if (correction.blend == CompositeBlend::Multiply) {
      mask_factor = mask_factor * (1.0f - fac) + (mask_factor * gray_clamped) * fac;
    }
    else {
      mask_factor = mask_factor * (1.0f - fac) + gray_clamped * fac;
    }
  }
  return mask_factor;
}

/**
/** Whether \a ibuf is a byte or float RGBA buffer of the stack's dimensions. */
static bool composite_ibuf_is_rgba(const ImBuf *ibuf, const int width, const int height)
{
  if (ibuf == nullptr || ibuf->x != width || ibuf->y != height) {
    return false;
  }
  if (!ELEM(ibuf->channels, 0, 4)) {
    return false;
  }
  return ibuf->byte_buffer.data != nullptr || ibuf->float_buffer.data != nullptr;
}

/** The colorspace name of \a ibuf's own buffer, or null when its buffer names none. */
static const char *composite_buffer_colorspace_name(const ImBuf *ibuf)
{
  const ColorSpace *colorspace = (ibuf->byte_buffer.data != nullptr) ?
                                     ibuf->byte_buffer.colorspace :
                                     ibuf->float_buffer.colorspace;
  if (colorspace == nullptr) {
    return nullptr;
  }
  return IMB_colormanagement_colorspace_get_name(colorspace);
}

/**
 * Decode one tile of \a ibuf into \a dst as straight scene-linear RGBA: `tw * th * 4` floats.
 *
 * A byte buffer is straight already; a float one is premultiplied and is straightened here, before
 * the colorspace conversion, and the conversion is one buffer call per tile rather than a per-pixel
 * one -- what keeps a 4K map cheap. A data or scene-linear space is left as it is.
 */
static void composite_decode_tile(const ImBuf *ibuf,
                                  const char *colorspace_name,
                                  const rcti &tile,
                                  float *dst)
{
  const int tw = BLI_rcti_size_x(&tile);
  const int th = BLI_rcti_size_y(&tile);
  const bool is_float = ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data != nullptr;
  const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
  const bool four = channels == 4;
  for (int ty = 0; ty < th; ty++) {
    const int y = tile.ymin + ty;
    for (int tx = 0; tx < tw; tx++) {
      const int x = tile.xmin + tx;
      float *out = dst + (int64_t(ty) * tw + tx) * 4;
      if (!is_float) {
        const uchar *pixel = ibuf->byte_data() + (int64_t(y) * ibuf->x + x) * 4;
        out[0] = float(pixel[0]) / 255.0f;
        out[1] = float(pixel[1]) / 255.0f;
        out[2] = float(pixel[2]) / 255.0f;
        out[3] = float(pixel[3]) / 255.0f;
      }
      else {
        const float *pixel = ibuf->float_buffer.data + (int64_t(y) * ibuf->x + x) * channels;
        out[0] = pixel[0];
        out[1] = pixel[1];
        out[2] = pixel[2];
        out[3] = four ? pixel[3] : 1.0f;
      }
    }
  }
  if (is_float) {
    /* A float buffer is premultiplied: straighten it before anything reads the colour. */
    for (int64_t i = 0; i < int64_t(tw) * th; i++) {
      const float alpha = dst[i * 4 + 3];
      if (alpha > 0.0f) {
        const float inv = 1.0f / alpha;
        dst[i * 4 + 0] *= inv;
        dst[i * 4 + 1] *= inv;
        dst[i * 4 + 2] *= inv;
      }
    }
  }
  /* The name is resolved by the acquisition path from the buffer it hands over; a stack built by
   * hand (a test) leaves it null, and the buffer's own colorspace is then the right answer. */
  const ColorSpace *colorspace = (colorspace_name != nullptr) ?
                                     IMB_colormanagement_space_get_named(colorspace_name) :
                                     nullptr;
  if (colorspace == nullptr) {
    colorspace = (ibuf->byte_buffer.data != nullptr) ? ibuf->byte_buffer.colorspace :
                                                       ibuf->float_buffer.colorspace;
  }
  if (colorspace != nullptr && !IMB_colormanagement_space_is_data(colorspace) &&
      !IMB_colormanagement_space_is_scene_linear(colorspace))
  {
    IMB_colormanagement_colorspace_to_scene_linear(dst, tw, th, 4, colorspace, false);
  }
}

/**
 * Apply one enabled layer over the scene-linear \a dst tile (spec 18 §5.3).
 *
 * The colour is decoded out of the layer's own map -- or taken from its constant, already linear --
 * and every content correction is decoded and blended onto it the way the graph routes a
 * correction's map through its own coverage. The factor the row finally blends by is the coverage
 * chain, computed pixel by pixel; a mask image owns it, and the mask corrections sit on top.
 */
/**
 * Render one row onto tile-sized buffers: its straight colour in \a r_color and the coverage it
 * blends by in \a r_factor. The destination is not touched, so a folder can composite the rows it
 * holds through this same code.
 *
 * \param source_override: when non-null, the colour to start from instead of the layer's own map --
 *                         how a folder's isolated result enters the folder's corrections and mask.
 * \param coverage_override: when non-null, the coverage to start from instead of the layer's own
 *                          alpha -- the isolated coverage a folder's contents accumulated, so its
 *                          content corrections fold into that coverage by the over model.
 */
static void composite_layer_render(const PaintMaterialCompositeLayer &layer,
                                   const rcti &tile,
                                   const float *source_override,
                                   const float *coverage_override,
                                   float *r_color,
                                   float *r_factor)
{
  const int tw = BLI_rcti_size_x(&tile);
  const int th = BLI_rcti_size_y(&tile);
  const int64_t count = int64_t(tw) * th;
  const bool has_corrections = !layer.content_corrections.is_empty() ||
                               !layer.mask_corrections.is_empty();

  if (source_override != nullptr) {
    memcpy(r_color, source_override, size_t(count) * 4 * sizeof(float));
  }
  else if (layer.color_ibuf != nullptr) {
    composite_decode_tile(layer.color_ibuf, layer.color_colorspace_name, tile, r_color);
  }
  else {
    for (int64_t i = 0; i < count; i++) {
      if (layer.has_constant_color) {
        copy_v4_v4(r_color + i * 4, layer.constant_color);
      }
      else {
        zero_v4(r_color + i * 4);
      }
    }
  }

  /* A mask is a map like any other: decoded in one tile call, never a colorspace conversion per
   * pixel. Non-Color masks -- the ones this engine creates -- make the call a no-op. */
  Vector<float> mask_storage;
  const float *mask = nullptr;
  if (layer.mask_ibuf != nullptr) {
    mask_storage.resize(count * 4);
    composite_decode_tile(layer.mask_ibuf, layer.mask_colorspace_name, tile, mask_storage.data());
    mask = mask_storage.data();
  }

  /* A second coverage (a Material layer's baked source transparency), decoded the same way and
   * multiplied into the base as its grey, like the per-pixel path and the generator. */
  Vector<float> coverage_storage;
  if (layer.coverage_ibuf != nullptr) {
    coverage_storage.resize(count * 4);
    composite_decode_tile(
        layer.coverage_ibuf, layer.coverage_colorspace_name, tile, coverage_storage.data());
  }
  auto extra_coverage = [&](const int64_t i) -> float {
    if (layer.has_coverage_constant) {
      /* A live Material source alpha, the constant the generator builds. */
      return layer.coverage_constant;
    }
    if (coverage_storage.is_empty()) {
      return 1.0f;
    }
    const float *c = coverage_storage.data() + i * 4;
    if (layer.coverage_from_alpha) {
      return clamp_f(c[3], 0.0f, 1.0f);
    }
    return clamp_f((c[0] + c[1] + c[2]) / 3.0f, 0.0f, 1.0f);
  };

  /* The mask side of the coverage: its own image, times the second baked coverage. A mask always
   * clips the layer, corrections included, so it stays apart from the content side below. */
  auto mask_coverage = [&](const int64_t i) -> float {
    float value_out = 1.0f;
    if (mask != nullptr) {
      const float *m = mask + i * 4;
      const float value = (layer.mask_from_alpha && !layer.mask_reads_grey) ?
                              m[3] :
                              (m[0] + m[1] + m[2]) / 3.0f;
      value_out = (1.0f - layer.mask_influence) +
                  layer.mask_influence * clamp_f(value, 0.0f, 1.0f);
    }
    return value_out * extra_coverage(i);
  };
  /* The content side: a folder's contents decide its coverage, a map's alpha decides a leaf's, and
   * the content corrections build on it. Read before #r_color's alpha is overwritten below. */
  auto content_coverage = [&](const int64_t i) -> float {
    if (coverage_override != nullptr) {
      return coverage_override[i];
    }
    if ((layer.color_alpha_coverage && layer.color_ibuf != nullptr) ||
        (mask == nullptr && layer.mask_from_alpha))
    {
      return clamp_f(r_color[i * 4 + 3], 0.0f, 1.0f);
    }
    return 1.0f;
  };
  auto base_coverage = [&](const int64_t i) -> float {
    return mask_coverage(i) * content_coverage(i);
  };

  Vector<float> alpha_storage;
  float *alpha = nullptr;
  if (has_corrections) {
    alpha_storage.resize(count);
    alpha = alpha_storage.data();
    for (int64_t i = 0; i < count; i++) {
      alpha[i] = content_coverage(i);
    }

    Vector<float> correction_storage;
    for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.content_corrections) {
      if (!correction.enabled) {
        continue;
      }
      const float *correction_pixels = nullptr;
      if (correction.has_constant_color) {
        correction_storage.clear();
      }
      else if (correction.ibuf != nullptr) {
        correction_storage.resize(count * 4);
        composite_decode_tile(
            correction.ibuf, correction.colorspace_name, tile, correction_storage.data());
        correction_pixels = correction_storage.data();
      }
      else {
        continue;
      }
      for (int64_t i = 0; i < count; i++) {
        float corr_alpha = 1.0f;
        const float *corr_rgba = correction.constant_color;
        if (correction_pixels != nullptr) {
          corr_rgba = correction_pixels + i * 4;
          corr_alpha = corr_rgba[3];
        }
        const float fac = clamp_f(correction.opacity * corr_alpha, 0.0f, 1.0f);
        blend_row_linear(r_color + i * 4, corr_rgba, correction.blend, fac);
        alpha[i] = alpha[i] + fac * (1.0f - alpha[i]);
      }
    }
    /* The factor the row blends by: the mask times the coverage the content corrections built, and
     * the mask corrections blend onto that. Each correction is decoded once for the tile, then read
     * per pixel. */
    for (int64_t i = 0; i < count; i++) {
      r_factor[i] = mask_coverage(i) * alpha[i];
      r_color[i * 4 + 3] = r_factor[i];
    }
    for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.mask_corrections) {
      if (!correction.enabled) {
        continue;
      }
      const float *correction_pixels = nullptr;
      float constant_gray = 0.0f;
      if (correction.has_constant_color) {
        constant_gray = (correction.constant_color[0] + correction.constant_color[1] +
                         correction.constant_color[2]) /
                        3.0f;
      }
      else if (correction.ibuf != nullptr) {
        correction_storage.resize(count * 4);
        composite_decode_tile(
            correction.ibuf, correction.colorspace_name, tile, correction_storage.data());
        correction_pixels = correction_storage.data();
      }
      else {
        continue;
      }
      for (int64_t i = 0; i < count; i++) {
        /* Straight coverage laid over the factor; the bytes are read as stored, never divided. */
        float gray = constant_gray;
        float corr_alpha = 1.0f;
        if (correction_pixels != nullptr) {
          const float *c = correction_pixels + i * 4;
          corr_alpha = c[3];
          /* Read straight, exactly as in #composite_correction_pixel_mask_factor's loop. */
          gray = (c[0] + c[1] + c[2]) / 3.0f;
        }
        const float fac = clamp_f(corr_alpha * correction.opacity, 0.0f, 1.0f);
        const float gray_clamped = clamp_f(gray, 0.0f, 1.0f);
        if (correction.blend == CompositeBlend::Multiply) {
          r_factor[i] = r_factor[i] * (1.0f - fac) + (r_factor[i] * gray_clamped) * fac;
        }
        else {
          r_factor[i] = r_factor[i] * (1.0f - fac) + gray_clamped * fac;
        }
      }
    }
    return;
  }

  for (int64_t i = 0; i < count; i++) {
    r_factor[i] = base_coverage(i);
  }
}

/**
 * Accumulate a folder's contents in isolation: pre-multiplied colour in \a r_straight and coverage
 * in \a r_coverage, per the isolated-group model of design §5.
 */
static void composite_folder_accumulate(const PaintMaterialCompositeLayer &folder,
                                        const rcti &tile,
                                        float *r_straight,
                                        float *r_coverage)
{
  const int tw = BLI_rcti_size_x(&tile);
  const int th = BLI_rcti_size_y(&tile);
  const int64_t count = int64_t(tw) * th;

  Vector<float> premul(count * 4, 0.0f);
  Vector<float> coverage(count, 0.0f);
  Vector<float> child_color(count * 4);
  Vector<float> child_factor(count);
  Vector<float> sub_color(count * 4);
  Vector<float> sub_coverage(count);

  for (const PaintMaterialCompositeLayer &child : folder.children) {
    if (!child.enabled) {
      continue;
    }
    if (child.is_folder) {
      composite_folder_accumulate(child, tile, sub_color.data(), sub_coverage.data());
      composite_layer_render(child,
                             tile,
                             sub_color.data(),
                             sub_coverage.data(),
                             child_color.data(),
                             child_factor.data());
    }
    else {
      composite_layer_render(
          child, tile, nullptr, nullptr, child_color.data(), child_factor.data());
    }

    for (int64_t i = 0; i < count; i++) {
      const float f = clamp_f(child.opacity * child_factor[i], 0.0f, 1.0f);
      const float a = coverage[i];
      float straight[4];
      if (a > 0.0f) {
        for (const int k : IndexRange(4)) {
          straight[k] = premul[i * 4 + k] / a;
        }
      }
      else {
        zero_v4(straight);
      }
      /* blend_m(S, c) at full factor, then c_eff = lerp(c, blend, a). */
      float blended[4] = {straight[0], straight[1], straight[2], straight[3]};
      blend_row_linear(blended, child_color.data() + i * 4, child.blend, 1.0f);
      const float *c = child_color.data() + i * 4;
      float c_eff[4];
      for (const int k : IndexRange(4)) {
        c_eff[k] = c[k] + (blended[k] - c[k]) * a;
      }
      for (const int k : IndexRange(4)) {
        premul[i * 4 + k] = premul[i * 4 + k] * (1.0f - f) + c_eff[k] * f;
      }
      coverage[i] = a + f * (1.0f - a);
    }
  }

  for (int64_t i = 0; i < count; i++) {
    const float a = coverage[i];
    if (a > 0.0f) {
      for (const int k : IndexRange(4)) {
        r_straight[i * 4 + k] = premul[i * 4 + k] / a;
      }
    }
    else {
      zero_v4(r_straight + i * 4);
    }
    r_coverage[i] = a;
  }
}

static void composite_apply_layer_linear(const PaintMaterialCompositeLayer &layer,
                                         const rcti &tile,
                                         float *dst)
{
  const int tw = BLI_rcti_size_x(&tile);
  const int th = BLI_rcti_size_y(&tile);
  const int64_t count = int64_t(tw) * th;

  Vector<float> color(count * 4);
  Vector<float> factor(count);
  if (layer.is_folder) {
    Vector<float> sub_color(count * 4);
    Vector<float> sub_coverage(count);
    composite_folder_accumulate(layer, tile, sub_color.data(), sub_coverage.data());
    /* The accumulated coverage is the base the folder's own corrections build on, so it is passed
     * in rather than multiplied afterwards (design §5). */
    composite_layer_render(
        layer, tile, sub_color.data(), sub_coverage.data(), color.data(), factor.data());
  }
  else {
    composite_layer_render(layer, tile, nullptr, nullptr, color.data(), factor.data());
  }

  for (int64_t i = 0; i < count; i++) {
    blend_row_linear(
        dst + i * 4, color.data() + i * 4, layer.blend, layer.opacity * factor[i]);
  }
}

static bool composite_stack_validate(const PaintMaterialCompositeStack &stack)
{
  if (stack.width <= 0 || stack.height <= 0 || stack.layers.is_empty()) {
    return false;
  }
  bool any_enabled = false;
  for (const PaintMaterialCompositeLayer &layer : stack.layers) {
    if (!layer.enabled) {
      continue;
    }
    if (layer.is_folder) {
      /* A folder carries no buffers of its own; its contents are what has to be usable. */
      bool children_ok = !layer.children.empty();
      for (const PaintMaterialCompositeLayer &child : layer.children) {
        PaintMaterialCompositeStack child_stack;
        child_stack.width = stack.width;
        child_stack.height = stack.height;
        child_stack.layers.append(child);
        if (!composite_stack_validate(child_stack)) {
          children_ok = false;
          break;
        }
      }
      if (!children_ok) {
        return false;
      }
      any_enabled = true;
      continue;
    }
    const bool has_corrections = !layer.content_corrections.is_empty() ||
                                 !layer.mask_corrections.is_empty();
    if (layer.color_ibuf != nullptr) {
      if (!composite_ibuf_is_rgba(layer.color_ibuf, stack.width, stack.height)) {
        return false;
      }
    }
    else if (!has_corrections && !layer.has_constant_color) {
      /* A layer without a map has nothing to composite unless its content corrections carry it
       * (spec 18 §5.3) or it is a constant; refusing it here is what sends such a stack to the
       * bake. */
      return false;
    }
    if (layer.mask_ibuf != nullptr &&
        !composite_mask_ibuf_is_valid(layer.mask_ibuf, stack.width, stack.height))
    {
      return false;
    }
    if (layer.coverage_ibuf != nullptr &&
        !composite_mask_ibuf_is_valid(layer.coverage_ibuf, stack.width, stack.height))
    {
      return false;
    }
    for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.content_corrections) {
      if (correction.ibuf != nullptr &&
          !composite_ibuf_is_rgba(correction.ibuf, stack.width, stack.height))
      {
        return false;
      }
    }
    for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.mask_corrections) {
      if (correction.ibuf != nullptr &&
          !composite_mask_ibuf_is_valid(correction.ibuf, stack.width, stack.height))
      {
        return false;
      }
    }
    any_enabled = true;
  }
  return any_enabled;
}

/** The rectangle \a region clips to inside `[0, width] x [0, height]`, or empty. */
static bool composite_clip_area(const PaintMaterialCompositeStack &stack,
                                const rcti *region,
                                rcti &r_area)
{
  BLI_rcti_init(&r_area, 0, stack.width, 0, stack.height);
  if (region != nullptr) {
    rcti clipped = *region;
    if (!BLI_rcti_isect(&r_area, &clipped, &r_area)) {
      BLI_rcti_init(&r_area, 0, 0, 0, 0);
      return false;
    }
  }
  return true;
}

/** One 256x256 tile: the scratch a layer is decoded and mixed in, never a full-size copy. */
static constexpr int COMPOSITE_TILE_SIZE = 256;

bool BKE_paint_material_composite_eval_linear(const PaintMaterialCompositeStack &stack,
                                              float *dst,
                                              const rcti *region,
                                              PaintMaterialCompositeEvalStats *r_stats)
{
  if (!composite_stack_validate(stack)) {
    return false;
  }

  rcti area;
  if (!composite_clip_area(stack, region, area)) {
    /* Nothing of the tagged region is inside the buffer; the composite is already correct. */
    if (r_stats != nullptr) {
      *r_stats = {};
    }
    return true;
  }

  const double start_time = BLI_time_now_seconds();
  const int64_t area_width = BLI_rcti_size_x(&area);
  int layers_evaluated = 0;
  Vector<float> tile_storage;

  for (int tile_y = area.ymin; tile_y < area.ymax; tile_y += COMPOSITE_TILE_SIZE) {
    for (int tile_x = area.xmin; tile_x < area.xmax; tile_x += COMPOSITE_TILE_SIZE) {
      rcti tile;
      BLI_rcti_init(&tile,
                    tile_x,
                    min_ii(tile_x + COMPOSITE_TILE_SIZE, area.xmax),
                    tile_y,
                    min_ii(tile_y + COMPOSITE_TILE_SIZE, area.ymax));
      const int tw = BLI_rcti_size_x(&tile);
      const int th = BLI_rcti_size_y(&tile);
      tile_storage.resize(int64_t(tw) * th * 4);
      float *tile_dst = tile_storage.data();

      bool initialized = false;
      for (const PaintMaterialCompositeLayer &layer : stack.layers) {
        if (!layer.enabled) {
          continue;
        }
        const bool has_corrections = !layer.content_corrections.is_empty() ||
                                     !layer.mask_corrections.is_empty();
        if (!initialized) {
          /* A bare bottom is copied rather than blended: it has no Mix node, so it has no blend
           * mode or factor, and blending it over undefined pixels would let them show through
           * wherever it is transparent. A uniform chain's lowest layer has all of those, and
           * blends over the transparency the graph gives it -- so the tile starts from the shared
           * bottom colour and it is blended like any other layer. */
          if (layer.is_bare_base && layer.color_ibuf != nullptr) {
            composite_decode_tile(
                layer.color_ibuf, layer.color_colorspace_name, tile, tile_dst);
          }
          else if (stack.has_bottom_color) {
            for (int64_t i = 0; i < int64_t(tw) * th; i++) {
              copy_v4_v4(tile_dst + i * 4, stack.bottom_color);
            }
          }
          else {
            memset(tile_dst, 0, size_t(int64_t(tw) * th * 4) * sizeof(float));
          }
          initialized = true;
          layers_evaluated++;
          if (layer.is_bare_base && !has_corrections) {
            continue;
          }
        }
        composite_apply_layer_linear(layer, tile, tile_dst);
      }

      /* Copy the tile back into the caller's buffer; only the region is ever touched. */
      for (int row = 0; row < th; row++) {
        float *dst_row = dst + ((int64_t(tile.ymin + row) * stack.width + tile.xmin) * 4);
        memcpy(dst_row, tile_dst + int64_t(row) * tw * 4, size_t(tw) * 4 * sizeof(float));
      }
    }
  }

  if (r_stats != nullptr) {
    r_stats->elapsed_seconds = BLI_time_now_seconds() - start_time;
    r_stats->layers_evaluated = layers_evaluated;
    r_stats->pixels_processed = area_width * BLI_rcti_size_y(&area);
  }
  return true;
}

bool BKE_paint_material_composite_eval(const PaintMaterialCompositeStack &stack,
                                       ImBuf *composite_ibuf,
                                       const rcti *region,
                                       PaintMaterialCompositeEvalStats *r_stats)
{
  if (!composite_ibuf_is_byte_rgba(composite_ibuf, stack.width, stack.height)) {
    return false;
  }
  if (!composite_stack_validate(stack)) {
    return false;
  }

  Vector<float> linear(int64_t(stack.width) * stack.height * 4);
  if (!BKE_paint_material_composite_eval_linear(stack, linear.data(), region, r_stats)) {
    return false;
  }

  rcti area;
  if (!composite_clip_area(stack, region, area)) {
    return true;
  }

  /* The color encoding the byte output is written in: the buffer's own colorspace, so the preview
   * and a saved map are the same pixels. Null (a plain ImBuf) leaves the scene-linear values as
   * they are, which is what a test comparing one raw pixel wants. */
  const char *colorspace_name = composite_buffer_colorspace_name(composite_ibuf);
  const ColorSpace *colorspace = (colorspace_name != nullptr) ?
                                     IMB_colormanagement_space_get_named(colorspace_name) :
                                     nullptr;
  const bool encode = colorspace != nullptr && !IMB_colormanagement_space_is_data(colorspace) &&
                      !IMB_colormanagement_space_is_scene_linear(colorspace);

  uchar *pixels = composite_ibuf->byte_data_for_write();
  const int64_t area_width = BLI_rcti_size_x(&area);
  for (int y = area.ymin; y < area.ymax; y++) {
    float *row = linear.data() + (int64_t(y) * stack.width + area.xmin) * 4;
    if (encode) {
      IMB_colormanagement_scene_linear_to_colorspace(
          row, int(area_width), 1, 4, colorspace);
    }
    uchar *out = pixels + (int64_t(y) * stack.width + area.xmin) * 4;
    for (int64_t i = 0; i < area_width * 4; i++) {
      out[i] = uchar(clamp_i(int(row[i] * 255.0f + 0.5f), 0, 255));
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Layer Acquisition
 * \{ */

struct CompositeImageLock {
  Image *image = nullptr;
  ImBuf *ibuf = nullptr;
  void *lock = nullptr;
};

static ImBuf *composite_image_acquire(Image *image,
                                      const ImageUser *iuser,
                                      Vector<CompositeImageLock> &r_locks)
{
  if (image == nullptr) {
    return nullptr;
  }
  /* Acquiring writes to the #ImageUser, and the material's copy is not this code's to mutate. */
  ImageUser iuser_local;
  if (iuser != nullptr) {
    iuser_local = *iuser;
  }
  else {
    BKE_imageuser_default(&iuser_local);
  }

  CompositeImageLock entry;
  entry.image = image;
  entry.ibuf = BKE_image_acquire_ibuf(image, &iuser_local, &entry.lock);
  if (entry.ibuf == nullptr) {
    return nullptr;
  }
  r_locks.append(entry);
  return entry.ibuf;
}

static void composite_images_release(Span<CompositeImageLock> locks)
{
  for (const CompositeImageLock &entry : locks) {
    BKE_image_release_ibuf(entry.image, entry.ibuf, entry.lock);
  }
}

/** Acquire one image-layer's buffers into \a r_layer, recursing into a folder's children. */
static bool composite_layer_build(const PaintMaterialCompositeImageLayer &image_layer,
                                  Vector<CompositeImageLock> &r_locks,
                                  PaintMaterialCompositeLayer &r_layer)
{
  r_layer.blend = image_layer.blend;
  r_layer.opacity = image_layer.opacity;
  r_layer.mask_influence = image_layer.mask_influence;
  r_layer.mask_from_alpha = image_layer.mask_from_alpha;
  r_layer.mask_reads_grey = image_layer.mask_reads_grey;
  r_layer.color_alpha_coverage = image_layer.color_alpha_coverage;
  r_layer.is_bare_base = image_layer.is_bare_base;
  r_layer.is_folder = image_layer.is_folder;
  copy_v4_v4(r_layer.constant_color, image_layer.constant_color);
  r_layer.has_constant_color = image_layer.has_constant_color;
  r_layer.coverage_constant = image_layer.coverage_constant;
  r_layer.has_coverage_constant = image_layer.has_coverage_constant;
  r_layer.coverage_from_alpha = image_layer.coverage_from_alpha;
  r_layer.coverage_iuser = image_layer.coverage_iuser;

  if (image_layer.color_image != nullptr) {
    r_layer.color_ibuf = composite_image_acquire(
        image_layer.color_image, image_layer.color_iuser, r_locks);
    if (r_layer.color_ibuf == nullptr) {
      return false;
    }
    /* The buffer's own colorspace, not the Image setting: the two can disagree, and the evaluator
     * has to decode what it is actually handed. */
    r_layer.color_colorspace_name = composite_buffer_colorspace_name(r_layer.color_ibuf);
  }
  if (image_layer.mask_image != nullptr) {
    r_layer.mask_ibuf = composite_image_acquire(
        image_layer.mask_image, image_layer.mask_iuser, r_locks);
    if (r_layer.mask_ibuf == nullptr) {
      return false;
    }
    r_layer.mask_colorspace_name = composite_buffer_colorspace_name(r_layer.mask_ibuf);
  }
  if (image_layer.coverage_image != nullptr) {
    r_layer.coverage_ibuf = composite_image_acquire(
        image_layer.coverage_image, image_layer.coverage_iuser, r_locks);
    if (r_layer.coverage_ibuf == nullptr) {
      return false;
    }
    r_layer.coverage_colorspace_name = composite_buffer_colorspace_name(r_layer.coverage_ibuf);
  }
  for (const PaintMaterialCompositeCorrection &correction : image_layer.content_corrections) {
    PaintMaterialCompositeCorrectionBuffer buffer;
    buffer.ibuf = composite_image_acquire(correction.image, correction.iuser, r_locks);
    if (buffer.ibuf == nullptr && correction.image != nullptr) {
      return false;
    }
    if (buffer.ibuf != nullptr) {
      buffer.colorspace_name = composite_buffer_colorspace_name(buffer.ibuf);
    }
    buffer.blend = correction.blend;
    copy_v4_v4(buffer.constant_color, correction.constant_color);
    buffer.has_constant_color = correction.has_constant_color;
    buffer.opacity = correction.opacity;
    buffer.enabled = correction.enabled;
    r_layer.content_corrections.append(buffer);
  }
  for (const PaintMaterialCompositeCorrection &correction : image_layer.mask_corrections) {
    PaintMaterialCompositeCorrectionBuffer buffer;
    buffer.ibuf = composite_image_acquire(correction.image, correction.iuser, r_locks);
    if (buffer.ibuf == nullptr && correction.image != nullptr) {
      return false;
    }
    if (buffer.ibuf != nullptr) {
      buffer.colorspace_name = composite_buffer_colorspace_name(buffer.ibuf);
    }
    buffer.blend = correction.blend;
    copy_v4_v4(buffer.constant_color, correction.constant_color);
    buffer.has_constant_color = correction.has_constant_color;
    buffer.opacity = correction.opacity;
    buffer.enabled = correction.enabled;
    r_layer.mask_corrections.append(buffer);
  }
  for (const PaintMaterialCompositeImageLayer &child : image_layer.children) {
    PaintMaterialCompositeLayer child_layer;
    if (!composite_layer_build(child, r_locks, child_layer)) {
      return false;
    }
    r_layer.children.push_back(child_layer);
  }
  return true;
}

/**
 * Acquire the buffers of \a image_layers into \a r_stack, so the evaluator can read them without
 * touching an image's own cache mid-evaluation.
 *
 * \param only_marker: when given, only the layer carrying that identity is built. The mask baker
 *                     reads one row's coverage and nothing else, and acquiring the rest of the
 *                     stack would hold locks on images the factor never reads.
 */
static bool composite_stack_build(Span<PaintMaterialCompositeImageLayer> image_layers,
                                  Vector<CompositeImageLock> &r_locks,
                                  PaintMaterialCompositeStack &r_stack,
                                  const bUUID *only_marker = nullptr)
{
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, r_stack.width, r_stack.height))
  {
    return false;
  }
  for (const PaintMaterialCompositeImageLayer &image_layer : image_layers) {
    if (!image_layer.enabled) {
      continue;
    }
    if (only_marker != nullptr && !BLI_uuid_equal(image_layer.marker, *only_marker)) {
      continue;
    }
    const bool has_corrections = !image_layer.content_corrections.is_empty() ||
                                 !image_layer.mask_corrections.is_empty();
    if (!image_layer.is_folder && image_layer.color_image == nullptr && !has_corrections &&
        !image_layer.has_constant_color)
    {
      continue;
    }
    PaintMaterialCompositeLayer layer;
    if (!composite_layer_build(image_layer, r_locks, layer)) {
      return false;
    }
    r_stack.layers.append(layer);
  }
  return !r_stack.layers.is_empty();
}

bool BKE_paint_material_composite_eval_images(Span<PaintMaterialCompositeImageLayer> image_layers,
                                              ImBuf *composite_ibuf,
                                              const rcti *region,
                                              PaintMaterialCompositeEvalStats *r_stats,
                                              const float bottom_color[4])
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  bool ok = composite_stack_build(image_layers, locks, stack);
  if (ok && bottom_color != nullptr) {
    copy_v4_v4(stack.bottom_color, bottom_color);
    stack.has_bottom_color = true;
  }
  if (ok) {
    ok = BKE_paint_material_composite_eval(stack, composite_ibuf, region, r_stats);
  }
  composite_images_release(locks);
  return ok;
}

bool BKE_paint_material_composite_eval_images_linear(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    float *dst,
    const rcti *region,
    PaintMaterialCompositeEvalStats *r_stats,
    const float bottom_color[4])
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  bool ok = composite_stack_build(image_layers, locks, stack);
  if (ok && bottom_color != nullptr) {
    copy_v4_v4(stack.bottom_color, bottom_color);
    stack.has_bottom_color = true;
  }
  if (ok) {
    ok = BKE_paint_material_composite_eval_linear(stack, dst, region, r_stats);
  }
  composite_images_release(locks);
  return ok;
}

bool BKE_paint_material_composite_eval_row_mask(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const bUUID &row_marker,
    ImBuf *dst_ibuf,
    const rcti *region)
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  /* Only the row is built: its mask factor depends on nothing the other rows composite, and
   * building them would acquire buffers the factor never reads. The dimensions still come from the
   * whole stack, so B is validated against the same rectangle the composite uses. */
  const bool built = composite_stack_build(image_layers, locks, stack, &row_marker);

  bool ok = false;
  if (built && stack.layers.size() == 1 &&
      composite_ibuf_is_byte_rgba(dst_ibuf, stack.width, stack.height) &&
      composite_stack_validate(stack))
  {
    const PaintMaterialCompositeLayer &layer = stack.layers.first();
    rcti area;
    BLI_rcti_init(&area, 0, stack.width, 0, stack.height);
    bool have_area = true;
    if (region != nullptr) {
      rcti clipped = *region;
      if (!BLI_rcti_isect(&area, &clipped, &area)) {
        /* Nothing of the tagged region is inside the buffer; B is already correct. */
        have_area = false;
      }
    }
    if (have_area) {
      const int64_t row_stride = int64_t(stack.width) * 4;
      const int64_t area_width = BLI_rcti_size_x(&area);
      const IndexRange rows(area.ymin, BLI_rcti_size_y(&area));
      uchar *dst_pixels = dst_ibuf->byte_data_for_write();
      threading::parallel_for(rows, 64, [&](const IndexRange range) {
        for (const int64_t y : range) {
          const int64_t row_offset = y * row_stride;
          for (const int64_t x : IndexRange(area.xmin, area_width)) {
            const int64_t offset = row_offset + x * 4;
            const float mask_factor = composite_correction_pixel_mask_factor(
                layer, int(x), int(y));
            /* A scalar mask: the same value on all three colour channels, opaque. */
            const uchar gray = uchar(clamp_i(int(mask_factor * 255.0f + 0.5f), 0, 255));
            dst_pixels[offset + 0] = gray;
            dst_pixels[offset + 1] = gray;
            dst_pixels[offset + 2] = gray;
            dst_pixels[offset + 3] = 255;
          }
        }
      });
    }
    ok = true;
  }

  composite_images_release(locks);
  return ok;
}

bool BKE_paint_material_composite_eval_row_content(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const bUUID &row_marker,
    float *r_color_rgba,
    float *r_coverage_gray,
    const rcti *region)
{
  if (r_color_rgba == nullptr || r_coverage_gray == nullptr) {
    return false;
  }
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  const bool built = composite_stack_build(image_layers, locks, stack, &row_marker);
  bool ok = false;
  if (built && stack.layers.size() == 1 && composite_stack_validate(stack)) {
    const PaintMaterialCompositeLayer &layer = stack.layers.first();
    rcti area;
    if (!composite_clip_area(stack, region, area)) {
      composite_images_release(locks);
      return false;
    }
    const int64_t count = int64_t(BLI_rcti_size_x(&area)) * BLI_rcti_size_y(&area);
    Vector<float> color(count * 4);
    Vector<float> factor(count);
    if (layer.is_folder) {
      Vector<float> sub_color(count * 4);
      Vector<float> sub_coverage(count);
      composite_folder_accumulate(layer, area, sub_color.data(), sub_coverage.data());
      composite_layer_render(
          layer, area, sub_color.data(), sub_coverage.data(), color.data(), factor.data());
    }
    else {
      composite_layer_render(layer, area, nullptr, nullptr, color.data(), factor.data());
    }
    for (int64_t i = 0; i < count; i++) {
      r_color_rgba[i * 4 + 0] = color[i * 4 + 0];
      r_color_rgba[i * 4 + 1] = color[i * 4 + 1];
      r_color_rgba[i * 4 + 2] = color[i * 4 + 2];
      r_color_rgba[i * 4 + 3] = 1.0f;
      r_coverage_gray[i] = clamp_f(layer.opacity * factor[i], 0.0f, 1.0f);
    }
    ok = true;
  }
  composite_images_release(locks);
  return ok;
}

/**
 * The image whose buffer answers the bottom layer's size and colorspace: the layer's own map, or
 * -- when the layer is Absent here and its corrections carry it -- the first correction map.
 */
static Image *composite_bottom_layer_size_image(const PaintMaterialCompositeImageLayer &layer,
                                                const ImageUser *&r_iuser)
{
  if (layer.is_folder) {
    /* A folder has no map of its own: its size comes from its contents. */
    for (const PaintMaterialCompositeImageLayer &child : layer.children) {
      if (Image *image = composite_bottom_layer_size_image(child, r_iuser)) {
        return image;
      }
    }
    return nullptr;
  }
  if (layer.color_image != nullptr) {
    r_iuser = layer.color_iuser;
    return layer.color_image;
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.image != nullptr) {
      r_iuser = correction.iuser;
      return correction.image;
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
    if (correction.image != nullptr) {
      r_iuser = correction.iuser;
      return correction.image;
    }
  }
  return nullptr;
}

/**
 * Dimensions and byte colorspace of the bottom-most enabled layer.
 *
 * The colorspace is reported alongside the size because the composite has to inherit it rather
 * than take the default: the layers of a Roughness or a Normal channel are Non-Color, and a
 * composite that claimed sRGB instead would be display-transformed on its way to the screen while
 * the very same layer, opened on its own, would not. The two would then disagree about the pixels
 * a stroke is being judged against.
 *
 * \param r_byte_colorspace: name owned by the colour management configuration, so it outlives the
 *                           acquisition it is read from. Null when the layer has no byte buffer,
 *                           which the evaluator rejects anyway.
 */
static bool composite_stack_bottom_layer_info(Span<PaintMaterialCompositeImageLayer> image_layers,
                                              int &r_width,
                                              int &r_height,
                                              const char **r_byte_colorspace)
{
  r_width = 0;
  r_height = 0;
  if (r_byte_colorspace != nullptr) {
    *r_byte_colorspace = nullptr;
  }
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    if (!layer.enabled) {
      continue;
    }
    const ImageUser *size_iuser = nullptr;
    Image *size_image = composite_bottom_layer_size_image(layer, size_iuser);
    if (size_image == nullptr) {
      continue;
    }
    Vector<CompositeImageLock> locks;
    const ImBuf *ibuf = composite_image_acquire(size_image, size_iuser, locks);
    if (ibuf != nullptr) {
      r_width = ibuf->x;
      r_height = ibuf->y;
      if (r_byte_colorspace != nullptr) {
        *r_byte_colorspace = IMB_colormanagement_get_byte_colorspace(ibuf);
      }
    }
    composite_images_release(locks);
    return r_width > 0 && r_height > 0;
  }
  return false;
}

bool BKE_paint_material_composite_stack_dimensions(
    Span<PaintMaterialCompositeImageLayer> image_layers, int &r_width, int &r_height)
{
  return composite_stack_bottom_layer_info(image_layers, r_width, r_height, nullptr);
}

/** Extend \a hash with everything about one correction that changes the composited pixels. */
static uint64_t composite_correction_hash(uint64_t hash,
                                          const PaintMaterialCompositeCorrection &correction)
{
  /* The marker hashed field by field: it is the correction's identity, so a map re-tagged to a
   * different correction must not keep serving the old composite. */
  const bUUID &marker = correction.marker;
  uint64_t node_bytes = 0;
  for (const int i : IndexRange(6)) {
    node_bytes |= uint64_t(marker.node[i]) << (8 * (5 - i));
  }
  hash = get_default_hash(hash,
                          marker.time_low,
                          uint64_t(marker.time_mid) << 16 | marker.time_hi_and_version,
                          uint64_t(marker.clock_seq_hi_and_reserved) << 8 |
                              marker.clock_seq_low,
                          node_bytes);
  /* Session UID rather than a pointer, like the layers' own maps: a freed image's address can
   * come back as a different one. */
  return get_default_hash(hash,
                          correction.image != nullptr ? correction.image->id.session_uid : 0,
                          int(correction.blend),
                          correction.enabled,
                          correction.opacity);
}

uint64_t BKE_paint_material_composite_stack_hash(
    Span<PaintMaterialCompositeImageLayer> image_layers)
{
  uint64_t hash = get_default_hash(image_layers.size());
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    /* Mix the running hash multiplicatively before each layer: #get_default_hash folds its
     * arguments with XOR, which is order-independent, so two layers swapped would otherwise hash
     * the same and a reorder would not invalidate the composite. */
    hash *= 0x100000001b3ULL;
    /* Session UIDs rather than pointers: a freed image's address can come back as a different
     * one, and the hash is the only thing standing between that and a stale composite. */
    hash = get_default_hash(hash,
                            layer.color_image != nullptr ? layer.color_image->id.session_uid : 0,
                            layer.mask_image != nullptr ? layer.mask_image->id.session_uid : 0);
    hash = get_default_hash(hash,
                            int(layer.blend),
                            layer.enabled,
                            layer.mask_from_alpha,
                            layer.opacity,
                            layer.mask_influence);
    /* Split rather than appended: #get_default_hash mixes a fixed number of values at once. */
    hash = get_default_hash(hash,
                            layer.is_bare_base,
                            layer.mask_reads_grey,
                            layer.coverage_image != nullptr ?
                                layer.coverage_image->id.session_uid :
                                0,
                            layer.color_alpha_coverage);
    /* A live constant is not backed by an image, so its value has to be hashed explicitly or the
     * cached composite would not follow a source slider. */
    hash = get_default_hash(hash,
                            layer.has_constant_color,
                            layer.constant_color[0],
                            layer.constant_color[1],
                            layer.constant_color[2],
                            layer.constant_color[3]);
    hash = get_default_hash(
        hash, layer.has_coverage_constant, layer.coverage_constant, layer.coverage_from_alpha);
    /* A correction changes the composite like any other layer input, and so belongs in the hash
     * that decides whether the whole stack has to be re-flattened. */
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      hash = composite_correction_hash(hash, correction);
    }
    for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
      hash = composite_correction_hash(hash, correction);
    }
  }
  return hash;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Composite Cache
 *
 * Keyed by the material's #ID.session_uid, like the bake cache and for the same reason: a freed
 * material hands its address to the next one, and a pointer-keyed cache would then serve the old
 * composite for the new material.
 *
 * Main thread only. Everything that reaches this -- the image editor's buffer acquisition, a
 * stroke's region tag, an image edit -- runs there, and unlike the bake there is no worker
 * writing results back.
 * \{ */

struct CompositeCacheKey {
  uint32_t material_session_uid = 0;
  int channel = 0;

  uint64_t hash() const
  {
    return get_default_hash(this->material_session_uid, this->channel);
  }

  friend bool operator==(const CompositeCacheKey &a, const CompositeCacheKey &b)
  {
    return a.material_session_uid == b.material_session_uid && a.channel == b.channel;
  }
};

/**
 * Owning handles for the two resources a cache entry holds.
 *
 * By value rather than as raw pointers freed by hand, because the entry is destroyed from four
 * places -- eviction, a failed evaluation, a per-material drop and the teardown -- and every one
 * of them used to have to remember both. A path added later that forgets is a leak that nothing
 * reports.
 */
struct ImBufDeleter {
  void operator()(ImBuf *ibuf) const
  {
    IMB_freeImBuf(ibuf);
  }
};
using ImBufPtr = std::unique_ptr<ImBuf, ImBufDeleter>;

struct PartialUpdateUserDeleter {
  void operator()(PartialUpdateUser *user) const
  {
    BKE_image_partial_update_free(user);
  }
};
using PartialUpdateUserPtr = std::unique_ptr<PartialUpdateUser, PartialUpdateUserDeleter>;

struct CompositeCacheEntry {
  ImBufPtr ibuf;
  int width = 0;
  int height = 0;
  uint64_t stack_hash = 0;
  /** The whole buffer has to be recomputed. Set on creation, and whenever a region is unknown. */
  bool dirty_full = true;
  /** Bounding rectangle of the pixels tagged since the last evaluation. */
  rcti dirty_region = {0, 0, 0, 0};
  /** Images the composite read, so an edit to one can be reported without knowing the stack. */
  Vector<uint32_t> image_session_uids;
  /**
   * One partial-update subscription per source image, keyed by #ID.session_uid.
   *
   * The image records what changed in it, per tile, whoever caused the change; polling that is
   * what makes this cache independent of anyone remembering to tag it. More to the point, it is
   * what stops a blanket tag from discarding a precise one: painting tags the image ID on every
   * dab, and the depsgraph flush that follows used to reach #image_changed and mark the whole
   * composite dirty -- re-flattening the entire stack for a dab that had already been reported
   * exactly.
   *
   * Never holds an #Image pointer. The images arrive with every `cache_ensure` call, so a poll
   * always has a fresh one, and a cache outliving an ID it pointed at would be a crash rather than
   * a stale pixel.
   */
  Map<uint32_t, PartialUpdateUserPtr> partial_update_users;
  /**
   * Taken from a counter shared by the whole cache, so a consumer holding a copy of the pixels can
   * tell it is old.
   *
   * Shared rather than per entry because a consumer compares one number over time, not per
   * material and channel: the Image Editor switching from one composited pass to another is
   * looking at a different buffer, and two entries counting from one of their own would hand it
   * the same revision for both.
   */
  uint64_t revision = 0;
  /** Monotonic counter used to evict the least recently used entry. */
  int64_t last_use = 0;
};

/** A composite is one buffer per material and channel, so this is a handful of entries at most;
 * the budget only exists to bound a pathological case, not to be managed. */
static constexpr int64_t COMPOSITE_CACHE_BUDGET_BYTES = 256 * 1024 * 1024;

/**
 * The one composite cache of the session.
 *
 * A single object rather than three loose globals, so that the counters cannot drift from the
 * entries they belong to and so that everything the cache owns is reached from one place.
 *
 * Deliberately not stored on #Main or on #Material, which is where derived data normally lives:
 * the dependency this cache exists to answer runs the wrong way. An edit reports "these pixels of
 * this image changed", and the cache is the only thing that knows which materials read that image;
 * per-material storage could not answer it without a walk over every material, from call sites --
 * a paint stroke, an image edit -- that have no #Main to walk. What per-material lifetime would
 * have bought is instead paid for explicitly, by
 * #BKE_paint_material_composite_cache_free_material.
 *
 * Main thread only. Everything that reaches it -- the image editor's buffer acquisition, a
 * stroke's region tag, an image edit -- runs there, and unlike the bake there is no worker writing
 * results back.
 */
struct CompositeCache {
  Map<CompositeCacheKey, CompositeCacheEntry> entries;
  /** Monotonic, and only ever compared: the source of #CompositeCacheEntry.last_use. */
  int64_t use_counter = 0;
  /** Never reset: a consumer compares revisions over time, across entries that come and go. */
  uint64_t revision_counter = 0;
};

static CompositeCache g_cache;

static int64_t composite_entry_size_in_bytes(const CompositeCacheEntry &entry)
{
  return int64_t(entry.width) * entry.height * 4;
}

/**
 * Drop the buffer and the subscriptions of \a entry while keeping the entry itself.
 *
 * Only for a resize, which needs a new buffer of a new size and -- because the subscriptions go
 * with it -- a fresh set of them, whose first poll asks for the full rebuild a resize needs
 * anyway. Removing an entry from the cache needs no call: the handles free themselves.
 */
static void composite_entry_reset(CompositeCacheEntry &entry)
{
  entry.ibuf.reset();
  entry.partial_update_users.clear();
}

/** Evict least recently used entries until the cache fits the budget, never the one just made. */
static void composite_cache_enforce_budget(const CompositeCacheKey &keep)
{
  int64_t total = 0;
  for (const CompositeCacheEntry &entry : g_cache.entries.values()) {
    total += composite_entry_size_in_bytes(entry);
  }
  while (total > COMPOSITE_CACHE_BUDGET_BYTES) {
    const CompositeCacheKey *oldest_key = nullptr;
    int64_t oldest_use = INT64_MAX;
    for (const auto item : g_cache.entries.items()) {
      if (item.key == keep) {
        continue;
      }
      if (item.value.last_use < oldest_use) {
        oldest_use = item.value.last_use;
        oldest_key = &item.key;
      }
    }
    if (oldest_key == nullptr) {
      break;
    }
    const CompositeCacheKey key = *oldest_key;
    total -= composite_entry_size_in_bytes(g_cache.entries.lookup(key));
    g_cache.entries.remove(key);
  }
}

/**
 * The images one layer is read from: its maps, then its corrections'.
 *
 * The cache subscribes to each image's partial-update log through this one list, so a correction's
 * own map reports its edits exactly the way a layer's map does. Deduplicated, since a layer that
 * masks itself by its own map names the same image twice and one subscription per image is all a
 * poll can use.
 */
static Vector<Image *> composite_layer_images(const PaintMaterialCompositeImageLayer &layer)
{
  Vector<Image *> images;
  if (layer.color_image != nullptr) {
    images.append_non_duplicates(layer.color_image);
  }
  if (layer.mask_image != nullptr) {
    images.append_non_duplicates(layer.mask_image);
  }
  if (layer.coverage_image != nullptr) {
    images.append_non_duplicates(layer.coverage_image);
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.image != nullptr) {
      images.append_non_duplicates(correction.image);
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
    if (correction.image != nullptr) {
      images.append_non_duplicates(correction.image);
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    for (Image *image : composite_layer_images(child)) {
      images.append_non_duplicates(image);
    }
  }
  return images;
}

static void composite_entry_image_dependencies_set(
    CompositeCacheEntry &entry, Span<PaintMaterialCompositeImageLayer> image_layers)
{
  entry.image_session_uids.clear();
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    for (Image *image : composite_layer_images(layer)) {
      entry.image_session_uids.append_non_duplicates(image->id.session_uid);
    }
  }

  /* A layer removed from the stack stops being watched, or the entry keeps an allocation and a
   * poll per frame for an image it no longer reads. */
  Vector<uint32_t> stale;
  for (const uint32_t uid : entry.partial_update_users.keys()) {
    if (!entry.image_session_uids.contains(uid)) {
      stale.append(uid);
    }
  }
  for (const uint32_t uid : stale) {
    entry.partial_update_users.remove(uid);
  }
}

ImBuf *BKE_paint_material_composite_cache_ensure(
    const Material &ma,
    const eMaterialPaintChannel channel,
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const uint64_t stack_hash,
    uint64_t *r_revision,
    PaintMaterialCompositeEvalStats *r_stats,
    rcti *r_changed_region)
{
  if (r_changed_region != nullptr) {
    /* Initialized before any early return, so that "nothing was recomputed" is never confused with
     * "the caller forgot to look". */
    BLI_rcti_init(r_changed_region, 0, 0, 0, 0);
  }

  int width = 0;
  int height = 0;
  const char *byte_colorspace = nullptr;
  if (!composite_stack_bottom_layer_info(image_layers, width, height, &byte_colorspace)) {
    return nullptr;
  }

  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = int(channel);

  CompositeCacheEntry &entry = g_cache.entries.lookup_or_add_default(key);
  entry.last_use = ++g_cache.use_counter;

  const bool size_changed = entry.ibuf == nullptr || entry.width != width ||
                            entry.height != height;
  if (size_changed) {
    composite_entry_reset(entry);
    entry.ibuf.reset(IMB_allocImBuf(uint(width), uint(height), ImBufFlags::ByteData));
    if (entry.ibuf == nullptr) {
      g_cache.entries.remove(key);
      return nullptr;
    }
    entry.ibuf->channels = 4;
    entry.width = width;
    entry.height = height;
  }

  /* What the source images say changed since the last call.
   *
   * Polled rather than reported: a caller that edits pixels no longer has to remember to tell this
   * cache, and -- the reason this exists -- a blanket ID tag from an unrelated subsystem can no
   * longer overwrite a precise report with "everything". Placed after the reallocation above so a
   * resize, which drops the subscriptions with the buffer, is followed by fresh ones whose first
   * poll asks for the full rebuild a resize needs anyway. */
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    for (Image *image : composite_layer_images(layer)) {
      PartialUpdateUser *user =
          entry.partial_update_users
              .lookup_or_add_cb(
                  image->id.session_uid,
                  [&]() { return PartialUpdateUserPtr(BKE_image_partial_update_create(image)); })
              .get();

      switch (BKE_image_partial_update_collect_changes(image, user)) {
        case ePartialUpdateCollectResult::FullUpdateNeeded:
          /* A brand new subscription lands here too, which is right: nothing of this image has
           * been flattened yet. */
          entry.dirty_full = true;
          break;
        case ePartialUpdateCollectResult::NoChangesDetected:
          break;
        case ePartialUpdateCollectResult::PartialChangesDetected: {
          PartialUpdateRegion change;
          while (BKE_image_partial_update_get_next_change(user, &change) ==
                 ePartialUpdateIterResult::ChangeAvailable)
          {
            /* A layer stack cannot be tiled, so a change reported for any tile but the first would
             * land at the wrong place in a single-tile buffer. Give up precision rather than put
             * pixels somewhere they do not belong. */
            if (change.tile_number != 1001) {
              entry.dirty_full = true;
              break;
            }
            if (BLI_rcti_is_empty(&entry.dirty_region)) {
              entry.dirty_region = change.region;
            }
            else {
              BLI_rcti_union(&entry.dirty_region, &change.region);
            }
          }
          break;
        }
      }
    }
  }

  /* The partial-update log reports whole tiles, which can reach past a buffer smaller than one
   * tile. A layer stack cannot be tiled, so clip the report to the buffer: every consumer of the
   * echoed region -- and the echoed region itself -- must stay within the pixels that exist. */
  if (!entry.dirty_full && !BLI_rcti_is_empty(&entry.dirty_region)) {
    const rcti bounds = {0, entry.width, 0, entry.height};
    rcti clipped;
    if (BLI_rcti_isect(&bounds, &entry.dirty_region, &clipped)) {
      entry.dirty_region = clipped;
    }
    else {
      BLI_rcti_init(&entry.dirty_region, 0, 0, 0, 0);
    }
  }

  const bool rebuild_all = size_changed || entry.stack_hash != stack_hash || entry.dirty_full;
  const bool rebuild_region = !rebuild_all && !BLI_rcti_is_empty(&entry.dirty_region);
  if (rebuild_all) {
    /* Only with the stack: the bottom layer decides the colorspace, and a region refresh cannot
     * have changed which layer that is. Non-Color layers -- Roughness, Metallic, a normal map --
     * must not be handed on as sRGB, or the composite is display-transformed on its way to the
     * screen while the layer it is made of is not. */
    BLI_assert(byte_colorspace != nullptr);
    IMB_colormanagement_assign_byte_colorspace(entry.ibuf.get(), byte_colorspace);
  }

  if (rebuild_all || rebuild_region) {
    const rcti *region = rebuild_region ? &entry.dirty_region : nullptr;
    if (!BKE_paint_material_composite_eval_images(image_layers, entry.ibuf.get(), region, r_stats))
    {
      g_cache.entries.remove(key);
      return nullptr;
    }
    if (r_changed_region != nullptr) {
      /* The caller derives its own pixels from these and needs to refresh no more than what really
       * moved; a full rebuild is reported as the whole buffer rather than as "everything", so one
       * rectangle type covers both cases. Read before #dirty_region is reset below. */
      if (rebuild_all) {
        BLI_rcti_init(r_changed_region, 0, entry.ibuf->x, 0, entry.ibuf->y);
      }
      else {
        *r_changed_region = entry.dirty_region;
      }
    }
    entry.stack_hash = stack_hash;
    entry.dirty_full = false;
    entry.revision = ++g_cache.revision_counter;
    BLI_rcti_init(&entry.dirty_region, 0, 0, 0, 0);
    composite_entry_image_dependencies_set(entry, image_layers);
  }

  if (r_revision != nullptr) {
    *r_revision = entry.revision;
  }
  composite_cache_enforce_budget(key);
  return entry.ibuf.get();
}

void BKE_paint_material_composite_cache_invalidate(const Material *ma)
{
  if (ma == nullptr) {
    for (CompositeCacheEntry &entry : g_cache.entries.values()) {
      entry.dirty_full = true;
    }
    return;
  }
  const uint32_t session_uid = ma->id.session_uid;
  for (auto item : g_cache.entries.items()) {
    if (item.key.material_session_uid == session_uid) {
      item.value.dirty_full = true;
    }
  }
}

void BKE_paint_material_composite_cache_free_material(const Material &ma)
{
  if (g_cache.entries.is_empty()) {
    /* This runs from #ID free, so it is on the path of every material in every file ever loaded,
     * almost none of which was ever composited. */
    return;
  }
  const uint32_t session_uid = ma.id.session_uid;
  Vector<CompositeCacheKey> dead_keys;
  for (auto item : g_cache.entries.items()) {
    if (item.key.material_session_uid == session_uid) {
      dead_keys.append(item.key);
    }
  }
  /* Collected first: removing from the map while iterating it would invalidate the iteration. */
  for (const CompositeCacheKey &key : dead_keys) {
    g_cache.entries.remove(key);
  }
}

void BKE_paint_material_composite_cache_free_all()
{
  /* Clearing is the whole teardown: every entry owns its buffer and its subscriptions outright. */
  g_cache.entries.clear();
}

bool BKE_paint_material_composite_cache_contains(const Material &ma,
                                                 const eMaterialPaintChannel channel)
{
  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = int(channel);
  return g_cache.entries.contains(key);
}

/** \} */

}  // namespace blender
