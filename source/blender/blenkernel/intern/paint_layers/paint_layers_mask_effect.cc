/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * A CPU-computed effect (a Blur, a Noise) as an item of a row's mask stack: the effect reads the
 * grey the mask has built up to it -- the base mask and the items before it, laid the way the
 * composite lays them -- and lays its result back as one grey, with the item's own invert, blend
 * and opacity applied by the reader. The live graph cannot express it, so the result is computed
 * into a buffer and cached the same way a row's effect is (#EffectCacheKey), keyed with the mask
 * channel marker so a mask item and a colour channel of one row never share an entry.
 *
 * The cache stands beside the row effect cache (#paint_layers_composite.cc) and shares its
 * machinery: the key, the staleness snapshot of the input's maps and the LRU store.
 */

#include <mutex>

#include "BLI_assert.h"
#include "BLI_index_range.hh"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers_composite_intern.hh"
#include "paint_layers_effect_cache_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Mask effect cache
 * \{ */

/** One cached mask effect: the result buffer, the maps the input was rendered from, the size and
 * the byte cost, keyed by #EffectCacheKey. */
struct MaskEffectCacheEntry {
  EffectCacheKey key;
  int width = 0;
  int height = 0;
  std::shared_ptr<ImBuf> color;
  /** The maps the input was rendered from, with the versions they had. */
  Vector<PaintLayerEffectSource> synced;
  size_t bytes = 0;
};

/** The same budget policy as the row effect cache: the working set of a couple of recent effects at
 * 4K, an entry evicted between two evaluations computed whole again. */
constexpr size_t PAINT_LAYER_MASK_EFFECT_CACHE_BYTES = size_t(1024) * 1024 * 1024;

static size_t g_paint_mask_effect_cache_budget = PAINT_LAYER_MASK_EFFECT_CACHE_BYTES;

static PaintLayerEffectCacheStore<MaskEffectCacheEntry> g_paint_mask_effect_cache;

/** Guards the container and the byte count only; never held while an effect computes, so the
 * input's own nested describes cannot deadlock on it. */
static std::mutex g_paint_mask_effect_cache_mutex;

/** An opaque RGBA float buffer of \a width x \a height, owned by the returned shared pointer. */
static std::shared_ptr<ImBuf> mask_effect_buffer_new(const int width, const int height)
{
  ImBuf *ibuf = IMB_allocImBuf(width, height, ImBufFlags::FloatData);
  if (ibuf == nullptr) {
    return {};
  }
  return std::shared_ptr<ImBuf>(ibuf, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

/**
 * The grey canvas the mask effect of \a row lays on: the row's own dimensions, described with the
 * mask stopped before the effect. The stop is what keeps the walk from re-entering the effect's
 * own describe -- a plain dimensions walk of the row reaches the item and would ask for the grey
 * that is being sized, for ever. The canvas follows the row's maps, which no mask item carries.
 */
static bool mask_effect_dimensions(const Material &ma,
                                   const MaterialPaintLayer &row,
                                   const MaterialPaintLayer &effect,
                                   int &r_width,
                                   int &r_height)
{
  PaintMaterialCompositeImageLayer entry;
  if (!composite_image_layer_build(ma,
                                   row,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   1.0f,
                                   false,
                                   true,
                                   entry,
                                   &effect.marker,
                                   nullptr,
                                   /*dimensions_only=*/true,
                                   /*describe_only=*/false))
  {
    return false;
  }
  return BKE_paint_material_composite_stack_dimensions(
             Span<PaintMaterialCompositeImageLayer>(&entry, 1), r_width, r_height) &&
         r_width > 0 && r_height > 0;
}

/** The key of \a effect's entry: the effect's own hash and the mask channel marker folded into it,
 * with the mask built up to the item in the chain half of the key -- the same field a GPU key
 * carries the row's preceding effects in, so one scheme keys both (M6 reads it as is). */
static EffectCacheKey mask_effect_key_get(const Material &ma,
                                          const MaterialPaintLayer &row,
                                          const MaterialPaintLayer &effect)
{
  EffectCacheKey key{ma.id.session_uid,
                     effect.marker,
                     PAINT_LAYER_EFFECT_CHANNEL_MASK,
                     paint_layer_effect_hash(effect, PAINT_LAYER_EFFECT_CHANNEL_MASK)};
  key.gpu_extras.chain_hash = paint_layer_mask_prefix_hash(ma, row, effect);
  return key;
}

void paint_layer_mask_effect_cache_clear()
{
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  const auto no_release = [](MaskEffectCacheEntry &) {};
  g_paint_mask_effect_cache.free_all(no_release);
}

void BKE_paint_layers_mask_effect_cache_free_all()
{
  paint_layer_mask_effect_cache_clear();
}

void BKE_paint_layers_mask_effect_cache_free_material(const Material &ma)
{
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  const auto no_release = [](MaskEffectCacheEntry &) {};
  g_paint_mask_effect_cache.free_material(ma.id.session_uid, no_release);
}

size_t paint_layer_mask_effect_cache_bytes()
{
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  return g_paint_mask_effect_cache.total_bytes;
}

int paint_layer_mask_effect_cache_entry_count()
{
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  return int(g_paint_mask_effect_cache.entries.size());
}

void paint_layer_mask_effect_cache_set_budget(const size_t bytes)
{
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  g_paint_mask_effect_cache_budget = bytes;
}

size_t paint_layer_mask_effect_cache_budget()
{
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  return g_paint_mask_effect_cache_budget;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compute
 * \{ */

/**
 * Bring the kernel's result back to one grey when the kind's kernel can leave a grey input
 * colored -- the kind table says which (#PAINT_LAYER_KIND_MASK_NEEDS_LUMA), so a new kind cannot
 * lay a colored mask silently. The monochrome Noise's delta is one value added to all three
 * channels, so it stays grey and is left alone -- except the salt and pepper, whose per-channel
 * draw the monochrome switch does not reach.
 */
static void mask_effect_grey_normalize(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  if (!paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_MASK_NEEDS_LUMA)) {
    /* A Blur, a Sharpen, a High Pass, an Edge Detect, an Emboss, an Erode / Dilate and a Bevel
     * keep the three channels equal: a grey in, a grey out. */
    return;
  }
  if (eMaterialPaintLayerFilter(effect.kind_subtype) == MA_PAINT_LAYER_FILTER_NOISE) {
    const bool monochrome = paint_layer_kind_param_bool_get(effect, PAINT_LAYER_NOISE_MONOCHROME);
    const int type = paint_layer_kind_param_int_get(effect, PAINT_LAYER_NOISE_TYPE);
    if (monochrome && type != PAINT_LAYER_NOISE_SALT_PEPPER) {
      return;
    }
  }
  paint_layer_effect_to_luminance(buffer);
}

/** Render the input of \a effect -- the mask built up to it -- and apply its kernel. */
static bool paint_layer_mask_effect_compute(const Material &ma,
                                            const MaterialPaintLayer &row,
                                            const MaterialPaintLayer &effect,
                                            std::shared_ptr<ImBuf> &r_color,
                                            Vector<PaintLayerEffectSource> &r_synced)
{
  bke::paint_perf::ScopedTimer perf_total("mask_effect.cpu.total");
  int width = 0;
  int height = 0;
  if (!mask_effect_dimensions(ma, row, effect, width, height)) {
    return false;
  }
  std::shared_ptr<ImBuf> color = mask_effect_buffer_new(width, height);
  if (color == nullptr) {
    return false;
  }
  {
    bke::paint_perf::ScopedTimer perf_input("mask_effect.cpu.input");
    /* The same grey a slice before this item would show: the base and the items before it, laid by
     * their own opacity, blend and invert, independent of the row's visibility -- the anchor
     * slice's own policy. The buffer is already canvas grey, so the item samples in with no map. */
    PaintLayersMaskSampler sampler(ma, row, &effect.marker, false);
    sampler.tile_acquire(0, width, height);
    float *pixels = color->float_data_for_write();
    for (const int y : IndexRange(height)) {
      for (const int x : IndexRange(width)) {
        const float grey = sampler.sample(0, x, y);
        const int64_t index = int64_t(y) * width + x;
        pixels[index * 4 + 0] = grey;
        pixels[index * 4 + 1] = grey;
        pixels[index * 4 + 2] = grey;
        pixels[index * 4 + 3] = 1.0f;
      }
    }
    perf_input.note("kind=%d size=%dx%d", int(effect.kind_subtype), width, height);
  }
  /* The kernel reads the buffer the block above filled and leaves the alpha alone. */
  paint_layer_effect_kernel_apply(*color, effect);
  mask_effect_grey_normalize(*color, effect);
  /* The versions are taken after the render: an edit that landed while it ran is read next time. */
  Vector<Image *> images;
  BKE_paint_layers_mask_effect_input_images(ma, row, effect, images);
  r_synced = map_sources_snapshot(images);
  r_color = std::move(color);
  perf_total.note("kind=%d size=%dx%d", int(effect.kind_subtype), width, height);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cached result
 * \{ */

std::shared_ptr<ImBuf> paint_layer_mask_effect_buffer_get(const Material &ma,
                                                          const MaterialPaintLayer &row,
                                                          const MaterialPaintLayer &effect,
                                                          const bool describe_only,
                                                          Vector<Image *> *r_images)
{
  /* The re-entrancy guard: the describes the grey's own walk needs -- its input's, and the canvas
   * size -- stop before the item, but any other describe of the row reaches the item's branch and
   * would ask for the grey that is being computed, for ever. One that lays nothing is the answer
   * then, the way an input that cannot be rendered is.
   *
   * Nothing in the effect's own walk stops late enough to land here: a describe that does is
   * `composite_image_layer_build` describing the row from outside the walk (its branch for the
   * item asks for the grey mid-compute). The empty answer keeps that frame up, but an empty grey
   * can then be cached higher up the stack, so a Debug build fails loudly where the cause shows. */
  static thread_local Vector<const MaterialPaintLayer *> computing;
  if (computing.contains(&effect)) {
    BLI_assert_msg(0, "mask effect grey asked for while it computes");
    return {};
  }
  computing.append(&effect);
  /* The pop rides the scope, every exit and ones added later included: a return without it would
   * leave the marker in `computing` for good, and the element would answer nothing from then on. */
  struct ComputingPop {
    Vector<const MaterialPaintLayer *> &stack;
    ~ComputingPop()
    {
      stack.remove_last();
    }
  } computing_pop{computing};

  int width = 0;
  int height = 0;
  if (!mask_effect_dimensions(ma, row, effect, width, height)) {
    return {};
  }
  Vector<Image *> images;
  BKE_paint_layers_mask_effect_input_images(ma, row, effect, images);
  if (r_images != nullptr) {
    *r_images = images;
  }
  if (describe_only) {
    return {};
  }

  const EffectCacheKey key = mask_effect_key_get(ma, row, effect);
  return paint_layer_effect_cache_get_or_compute(
      g_paint_mask_effect_cache,
      g_paint_mask_effect_cache_mutex,
      key,
      width,
      height,
      images,
      g_paint_mask_effect_cache_budget,
      [&](std::shared_ptr<ImBuf> &r_color, Vector<PaintLayerEffectSource> &r_synced) {
        return paint_layer_mask_effect_compute(ma, row, effect, r_color, r_synced);
      });
}

void paint_layer_mask_effect_cache_drop(const Material &ma,
                                        const MaterialPaintLayer &row,
                                        const MaterialPaintLayer &effect)
{
  const EffectCacheKey key = mask_effect_key_get(ma, row, effect);
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  const auto no_release = [](MaskEffectCacheEntry &) {};
  g_paint_mask_effect_cache.drop_rekeyed(key, no_release);
}

/** \} */

}  // namespace blender
