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

#include <cstring>
#include <mutex>

#include "BLI_assert.h"
#include "BLI_index_range.hh"
#include "BLI_task.hh"
#include "BLI_threads.h"
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
 * The input description of \a effect's grey, and the canvas it lays on: the entry the owner row
 * describes with the mask stopped before the item, or -- for a kind that reads an Anchor Point's
 * slice -- the empty entry (that grey has no row description at all, the slice's own canvas only)
 * and the slice's dimensions. #mask_effect_dimensions is the size question alone; the regional
 * gate additionally needs \a r_entry, which the anchor-slice grey never reaches.
 */
static bool mask_effect_input_entry(const Material &ma,
                                    const MaterialPaintLayer &row,
                                    const MaterialPaintLayer &effect,
                                    const int tile_number,
                                    const PaintLayerSurfaceRef painted_object,
                                    PaintMaterialCompositeImageLayer &r_entry,
                                    int &r_width,
                                    int &r_height)
{
  if (paint_layer_kind_reads_anchor_point(effect)) {
    /* The grey of a kind that reads a slice (a Normal to Mask) is the slice's own canvas: the
     * owner row's maps say nothing about it, and a mask over a row with no map in the colour
     * channel would size it to one texel. The entry stays undescribed: the slice is not the row's
     * input, and the region path refuses this kind before it would read one. */
    PaintMaterialCompositeImageLayer slice_entry;
    int slice_width = 0;
    int slice_height = 0;
    if (BKE_paint_layers_slice_describe(
            ma,
            effect,
            paint_layer_effect_input_channel(effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            slice_entry,
            nullptr,
            /*dimensions_only=*/true,
            /*allow_empty_mask=*/false,
            PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}) &&
        BKE_paint_material_composite_stack_dimensions(
            Span<PaintMaterialCompositeImageLayer>(&slice_entry, 1), slice_width, slice_height))
    {
      r_width = slice_width;
      r_height = slice_height;
      return true;
    }
    return false;
  }
  if (!composite_image_layer_build(ma,
                                   row,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   1.0f,
                                   false,
                                   true,
                                   r_entry,
                                   tile_number,
                                   &effect.marker,
                                   nullptr,
                                   /*dimensions_only=*/true,
                                   /*describe_only=*/false,
                                   nullptr,
                                   nullptr,
                                   PaintLayerBakeScope::WholeRow,
                                   painted_object))
  {
    return false;
  }
  /* A mask with no map of its own and a kind that reads a slot takes the slot's size. */
  bool from_slot = false;
  return paint_layer_effect_dimensions_resolve(Span<PaintMaterialCompositeImageLayer>(&r_entry, 1),
                                               effect,
                                               tile_number,
                                               r_width,
                                               r_height,
                                               from_slot);
}

/**
 * The grey canvas the mask effect of \a row lays on: the row's own dimensions on \a tile_number,
 * described with the mask stopped before the effect. The stop is what keeps the walk from
 * re-entering the effect's own describe -- a plain dimensions walk of the row reaches the item and
 * would ask for the grey that is being sized, for ever. The canvas follows the row's maps, which
 * no mask item carries.
 */
static bool mask_effect_dimensions(const Material &ma,
                                   const MaterialPaintLayer &row,
                                   const MaterialPaintLayer &effect,
                                   const int tile_number,
                                   const PaintLayerSurfaceRef painted_object,
                                   int &r_width,
                                   int &r_height)
{
  PaintMaterialCompositeImageLayer entry;
  return mask_effect_input_entry(
      ma, row, effect, tile_number, painted_object, entry, r_width, r_height);
}

/** The key of \a effect's entry: the effect's own hash and the mask channel marker folded into it,
 * with the mask built up to the item in the chain half of the key -- the same field a GPU key
 * carries the row's preceding effects in, so one scheme keys both (M6 reads it as is). */
static EffectCacheKey mask_effect_key_get(const Material &ma,
                                          const MaterialPaintLayer &row,
                                          const MaterialPaintLayer &effect,
                                          const int tile_number,
                                          const PaintLayerSurfaceRef painted_object)
{
  EffectCacheKey key{ma.id.session_uid,
                     effect.marker,
                     PAINT_LAYER_EFFECT_CHANNEL_MASK,
                     tile_number,
                     paint_layer_effect_hash(effect, PAINT_LAYER_EFFECT_CHANNEL_MASK)};
  if (paint_layer_kind_reads_anchor_point(effect)) {
    /* A kind whose grey reads an Anchor Point's slice (a Normal to Mask) keys the entry by the
     * slice's own description hash, not by the mask built up to it (the input images the
     * staleness walk collects follow it, see #BKE_paint_layers_mask_effect_input_images), and the
     * slice hash already folds the source's projection, so the prefix projection half stays
     * zero. */
    const int input_channel = paint_layer_effect_input_channel(effect,
                                                               PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    key.key_extras.chain_hash = paint_layer_anchor_point_slice_hash(ma, effect, input_channel);
    /* A projected source reads the surface of the object the grey is computed for: the slice hash
     * holds the frame but not the surface, so one source projected over two objects would share an
     * entry. Zero for a source that does not project. */
    const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, effect);
    key.key_extras.input_scalar_hash = (ref.source_row != nullptr) ?
                                           paint_layer_row_projection_hash(ma,
                                                                           *ref.source_row,
                                                                           input_channel,
                                                                           tile_number,
                                                                           painted_object) :
                                           0;
  }
  else {
    key.key_extras.chain_hash = paint_layer_mask_prefix_hash(ma, row, effect);
    /* The prefix hash above sees an item's mapping but not its projection frame or the surface the
     * frame is read over, which the grey depends on. Zero for a mask with no projected item, so the
     * key of every other mask is the one it always had. */
    key.key_extras.input_scalar_hash = paint_layer_mask_prefix_projection_hash(
        ma, row, effect, tile_number, painted_object);
  }
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
/** \name GPU provider
 * \{ */

/* The editors' GPU answer for a mask-stack effect, registered while their GPU pipeline can serve
 * one and null whenever it cannot. Reads happen during a composite; the plain pointer keeps no
 * guard for that, the same terms the owned-effect provider keeps (#paint_layers_composite.cc). */
static PaintLayerMaskEffectGpuProvider g_mask_effect_gpu_provider = nullptr;

void BKE_paint_layers_mask_effect_gpu_provider_set(PaintLayerMaskEffectGpuProvider provider)
{
  /* The contract the header pins, the owned-effect hook's own terms: registered on the main
   * thread before the first bake's worker starts reading the hook. */
  BLI_assert(BLI_thread_is_main());
  g_mask_effect_gpu_provider = provider;
}

PaintLayerMaskEffectGpuProvider BKE_paint_layers_mask_effect_gpu_provider_get()
{
  return g_mask_effect_gpu_provider;
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

/**
 * Resample the grey in \a buffer through the transform \a step, in place. The resample filters
 * premultiplied, so a texel the step reads from outside a clipped edge, or from nothing, lands as
 * no coverage rather than as a transparent hole the item's blend would see through to the mask
 * below: the item says what the mask is there, and a moved mask has no coverage past its edge.
 */
static void mask_effect_transform_apply(ImBuf &buffer, const PaintMaterialCompositeMapping &step)
{
  ImBuf *moved = BKE_paint_material_composite_buffer_remap(
      &buffer, buffer.x, buffer.y, Span<PaintMaterialCompositeMapping>(&step, 1));
  if (moved == nullptr || moved->float_data() == nullptr) {
    /* The identity, or a source that could not be read: the grey stays as it is. */
    if (moved != nullptr) {
      IMB_freeImBuf(moved);
    }
    return;
  }
  const float *in = moved->float_data();
  float *out = buffer.float_data_for_write();
  const int64_t texels = int64_t(buffer.x) * buffer.y;
  for (const int64_t i : IndexRange(texels)) {
    out[i * 4 + 0] = in[i * 4 + 0];
    out[i * 4 + 1] = in[i * 4 + 0];
    out[i * 4 + 2] = in[i * 4 + 0];
    out[i * 4 + 3] = 1.0f;
  }
  IMB_freeImBuf(moved);
}

/** Whether the mask item \a effect is a kind whose grey reads an Anchor Point's slice, and the
 * reference resolves to a content slice: the grey's input is another layer's slice rather than
 * the grey the mask has built up to the item. A broken reference answers false, and the item
 * lays nothing -- the same answer a CPU effect whose input cannot render gives. */
static bool mask_effect_reads_anchor_slice(const Material &ma, const MaterialPaintLayer &effect)
{
  if (!paint_layer_kind_reads_anchor_point(effect)) {
    return false;
  }
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, effect);
  return ref.issue == PaintLayerAnchorPointIssue::None &&
         ref.target == PaintLayerAnchorPointTarget::Content;
}

/** Fill \a color, the item's canvas-sized grey buffer, with the input channel's color of the
 * slice the Anchor Point item \a effect references: the kernel's whole input. The slice renders
 * at the source row's own resolution, so the canvas samples it nearest when the two differ -- the
 * same stretch a bake map takes its grey by (see #bake_mask_effect_row_write). */
static bool mask_effect_anchor_slice_input(const Material &ma,
                                           const MaterialPaintLayer &effect,
                                           const int tile_number,
                                           const PaintLayerSurfaceRef painted_object,
                                           const int width,
                                           const int height,
                                           std::shared_ptr<ImBuf> &color)
{
  const int input_channel = paint_layer_effect_input_channel(effect,
                                                             PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  std::shared_ptr<ImBuf> slice_color;
  std::shared_ptr<ImBuf> slice_coverage;
  if (!BKE_paint_layers_slice_render(ma,
                                     effect,
                                     eMaterialPaintChannel(input_channel),
                                     &slice_color,
                                     &slice_coverage,
                                     nullptr,
                                     /*dimensions_only=*/false,
                                     PaintLayerTileContext{.painted_object = painted_object,
                                                           .tile_number = tile_number}))
  {
    return false;
  }
  if (slice_color == nullptr || slice_color->float_data() == nullptr) {
    return false;
  }
  const float *src = slice_color->float_data();
  const int src_width = slice_color->x;
  const int src_height = slice_color->y;
  float *pixels = color->float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      const int src_y = paint_layer_nearest_resample_texel(y, height, src_height);
      const float *src_row = src + int64_t(src_y) * src_width * 4;
      float *dst_row = pixels + int64_t(y) * width * 4;
      for (const int x : IndexRange(width)) {
        const int src_x = paint_layer_nearest_resample_texel(x, width, src_width);
        const float *texel = src_row + int64_t(src_x) * 4;
        dst_row[int64_t(x) * 4 + 0] = texel[0];
        dst_row[int64_t(x) * 4 + 1] = texel[1];
        dst_row[int64_t(x) * 4 + 2] = texel[2];
        dst_row[int64_t(x) * 4 + 3] = 1.0f;
      }
    }
  });
  return true;
}

/**
 * The kernel of the mask item \a effect over the ready grey \a buffer: the GPU provider when it can
 * serve the kind on this thread and \a allow_gpu, the CPU kernel otherwise, then the grey normalize
 * (#mask_effect_grey_normalize) for the kinds the kind table flags. Returns "gpu" or "cpu" for the
 * caller's log, or null when the CPU kernel answered false (see
 * #paint_layer_effect_kernel_apply) -- the full compute and the regional patch share this, so the
 * normalize and the provider gates are one place.
 *
 * \param allow_gpu: false for a cropped buffer (the regional patch). The mask provider builds its
 *        pass plan at the buffer's own size and some plans -- a flood, a distance field, a
 *        reduction -- reach by the canvas dimensions, so the full-canvas GPU answer is not the
 *        cropped one. The CPU kernel is the function the kind table vouches as local
 *        (#PAINT_LAYER_KIND_EFFECT_REGION_EXACT), and that is what a crop may run.
 */
static const char *mask_effect_kernel_apply(ImBuf &buffer,
                                            const MaterialPaintLayer &effect,
                                            const int tile_number,
                                            const bool allow_gpu)
{
  /* The GPU provider is asked first, the way a bake asks the owned-effect one: a grey the screen's
   * pipeline already computes saves the CPU kernel. The gates are the provider's own terms -- it
   * needs the main thread's GPU context, which a worker thread has not -- and a refusal (no
   * context, the item's pass failed, a kind it does not serve) falls through to the CPU kernel,
   * which answers the same grey as before. The normalize runs on either path: the provider lays
   * the raw kernel result, and the kinds the kind table flags fold back to one grey here. */
  const PaintLayerMaskEffectGpuProvider provider = BKE_paint_layers_mask_effect_gpu_provider_get();
  const char *path = "cpu";
  bool gpu_done = false;
  if (allow_gpu && provider != nullptr && BKE_paint_layers_effect_is_gpu_computed(effect) &&
      BLI_thread_is_main())
  {
    bke::paint_perf::ScopedTimer perf_gpu("mask_effect.gpu");
    /* The input and the result are the one buffer: the provider reads the grey whole into a
     * texture before its dispatches write (see #PaintLayerMaskEffectGpuProvider), so the aliased
     * run is safe. */
    gpu_done = provider(effect, buffer, buffer);
    perf_gpu.note("kind=%d done=%s", int(effect.kind_subtype), gpu_done ? "yes" : "fallback");
  }
  if (!gpu_done) {
    /* A kernel that answers false -- a node-group filter with no backend, or one asked off the
     * main thread -- leaves no answer to cache: null tells the caller to keep the entry
     * unrendered, so a bake worker's ask never files the grey back as the item's output. */
    if (!paint_layer_effect_kernel_apply(buffer, effect, /*for_mask=*/true, tile_number)) {
      return nullptr;
    }
  }
  else {
    path = "gpu";
  }
  mask_effect_grey_normalize(buffer, effect);
  return path;
}

/** Render the input of \a effect -- the mask built up to it on \a tile_number -- and apply its
 * kernel. */
static bool paint_layer_mask_effect_compute(const Material &ma,
                                            const MaterialPaintLayer &row,
                                            const MaterialPaintLayer &effect,
                                            const int tile_number,
                                            const PaintLayerSurfaceRef painted_object,
                                            std::shared_ptr<ImBuf> &r_color,
                                            Vector<PaintLayerEffectSource> &r_synced)
{
  bke::paint_perf::ScopedTimer perf_total("mask_effect.cpu.total");
  int width = 0;
  int height = 0;
  if (!mask_effect_dimensions(ma, row, effect, tile_number, painted_object, width, height)) {
    return false;
  }
  std::shared_ptr<ImBuf> color = mask_effect_buffer_new(width, height);
  if (color == nullptr) {
    return false;
  }
  {
    bke::paint_perf::ScopedTimer perf_input("mask_effect.cpu.input");
    if (paint_layer_kind_reads_anchor_point(effect)) {
      /* An Anchor Point kind reads the input channel of the layer its reference names, not the
       * grey the mask has built up to it: the slice is the kernel's whole input, and the coverage
       * it would bring does not exist (a tangent normal tracks no content alpha). A reference that
       * is broken or names a mask point has no content slice to convert, and the item lays nothing
       * rather than running the kernel over the mask's own grey. */
      if (!mask_effect_reads_anchor_slice(ma, effect) ||
          !mask_effect_anchor_slice_input(
              ma, effect, tile_number, painted_object, width, height, color))
      {
        return false;
      }
    }
    else {
      /* The same grey a slice before this item would show: the base and the items before it, laid
       * by their own opacity, blend and invert, independent of the row's visibility -- the anchor
       * slice's own policy. The buffer is already canvas grey, so the item samples in with no
       * map. The tile is the one the grey is computed for: the stack's tiled maps are acquired on
       * it. */
      PaintLayersMaskSampler sampler(ma, row, &effect.marker, false, painted_object);
      sampler.tile_acquire(tile_number, width, height);
      float *pixels = color->float_data_for_write();
      /* #tile_acquire decodes those maps into the sampler and is not thread safe, so the one tile
       * runs here, on the calling thread, before the rows below sample it in parallel -- the order
       * the mask merge keeps (#paint_layers_target.cc), and the one the class documents.
       * #sample mutates nothing (see #PaintLayersMaskSampler::sample): it reads the acquired tile's
       * pointers and the described stack's buffers, and every evaluator it reaches is a per-texel
       * function of those, so any number of threads may call it at once and the fill runs by rows. */
      threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
        for (const int y : range) {
          for (const int x : IndexRange(width)) {
            const float grey = sampler.sample(tile_number, x, y);
            const int64_t index = int64_t(y) * width + x;
            pixels[index * 4 + 0] = grey;
            pixels[index * 4 + 1] = grey;
            pixels[index * 4 + 2] = grey;
            pixels[index * 4 + 3] = 1.0f;
          }
        }
      });
    }
    perf_input.note("kind=%d size=%dx%d", int(effect.kind_subtype), width, height);
  }
  /* The path the grey took past the fill, for the total's note: a transform effect and a CPU
   * kernel are both the CPU, a served provider is the GPU. */
  const char *compute_path = "cpu";
  PaintMaterialCompositeMapping transform_step;
  if (paint_layer_mask_transform_step_get(effect, transform_step)) {
    /* A transform effect moves the grey instead of correcting it. */
    mask_effect_transform_apply(*color, transform_step);
  }
  else {
    /* The kernel reads the buffer the block above filled and leaves the alpha alone. A kernel
     * that answers false -- a node-group filter with no backend, or one asked off the main
     * thread -- leaves no answer to cache: the caller keeps the entry unrendered, so a bake
     * worker's ask never files the grey back as the item's output. */
    compute_path = mask_effect_kernel_apply(*color, effect, tile_number, /*allow_gpu=*/true);
    if (compute_path == nullptr) {
      return false;
    }
  }
  /* The versions are taken after the render: an edit that landed while it ran is read next time. */
  Vector<Image *> images;
  BKE_paint_layers_mask_effect_input_images(ma, row, effect, images);
  r_synced = map_sources_snapshot(images);
  r_color = std::move(color);
  perf_total.note(
      "kind=%d size=%dx%d path=%s", int(effect.kind_subtype), width, height, compute_path);
  return true;
}

/**
 * The regional update the mask effect cache runs instead of #paint_layer_mask_effect_compute when a
 * live entry's maps moved by one rectangle: the same structure the row's effect patch
 * (`paint_layer_effect_region_compute`) uses, so the two caches cannot drift. The grey is sampled
 * over \a maps_region grown by the halo twice -- the neighbourhood every texel of the interior reads
 * -- the whole kernel run over that small buffer, and the interior (\a maps_region grown once,
 * clipped) copied into a private copy of \a cached. \a cached stays untouched: a frame still laying
 * it keeps its pixels.
 *
 * The sampler decodes the stack's maps into its own tile and is not thread safe, so it is acquired
 * once here, on the calling thread, before the fill reads it -- the order the full compute keeps.
 * The small buffer holds only the region, so a texel of it samples the canvas texel it stands for.
 *
 * False refuses the patch -- a kind the gate names (a whole-canvas or non-local kernel, a transform
 * item is not a kernel at all), an anchor-slice grey (its input is another layer's slice, not the
 * row's grey), a remapping between the maps and the canvas, an empty or whole-canvas region -- and
 * the caller falls back to the full compute. The versions are taken after the small render, the
 * same rule the full compute stamps by.
 */
static bool mask_effect_region_compute(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &effect,
    const int tile_number,
    const PaintLayerSurfaceRef painted_object,
    const PaintMaterialCompositeImageLayer &input_entry,
    const Span<Image *> images,
    const std::shared_ptr<ImBuf> &cached,
    const rcti &maps_region,
    std::shared_ptr<ImBuf> &r_patched,
    Vector<PaintLayerEffectSource> &r_patched_synced,
    rcti &r_region_valid)
{
  bke::paint_perf::ScopedTimer perf_total("mask_effect.cpu.region");
  const int width = cached->x;
  const int height = cached->y;
  if (!paint_layer_effect_region_update_supported(effect) ||
      paint_layer_kind_reads_anchor_point(effect) ||
      !BKE_paint_layers_effect_region_is_exact(input_entry, images, width, height))
  {
    return false;
  }

  float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
  paint_layer_effect_filter_params(effect, params);
  const int halo = BKE_paint_layers_effect_halo(
      eMaterialPaintLayerFilter(effect.kind_subtype), params, width, height);
  rcti valid;
  BKE_paint_layers_effect_region_clip(maps_region, halo, width, height, valid);
  if (BLI_rcti_is_empty(&valid)) {
    return false;
  }
  if (valid.xmin == 0 && valid.ymin == 0 && valid.xmax >= width && valid.ymax >= height) {
    /* The halo of a whole-canvas kind grew the region past the canvas: the patch would re-sample
     * and re-kernel everything, which the full compute does without the clone and the copy. */
    return false;
  }
  /* The small buffer covers the valid region grown by the halo once more: the neighbourhood every
   * texel of the interior reads stays inside it. */
  rcti region_small;
  BKE_paint_layers_effect_region_clip(valid, halo, width, height, region_small);
  const int region_width = BLI_rcti_size_x(&region_small);
  const int region_height = BLI_rcti_size_y(&region_small);
  if (region_width <= 0 || region_height <= 0) {
    return false;
  }
  std::shared_ptr<ImBuf> small = mask_effect_buffer_new(region_width, region_height);
  if (small == nullptr) {
    return false;
  }
  {
    bke::paint_perf::ScopedTimer perf_input("mask_effect.cpu.region.input");
    PaintLayersMaskSampler sampler(ma, row, &effect.marker, false, painted_object);
    sampler.tile_acquire(tile_number, width, height);
    float *pixels = small->float_data_for_write();
    /* #tile_acquire is not thread safe, so the one tile is decoded here before the fill reads it
     * in parallel -- the order the full compute keeps (#paint_layer_mask_effect_compute). #sample
     * mutates nothing, so any number of threads may call it at once. */
    threading::parallel_for(IndexRange(region_height), 64, [&](const IndexRange range) {
      for (const int y : range) {
        for (const int x : IndexRange(region_width)) {
          const float grey = sampler.sample(
              tile_number, region_small.xmin + x, region_small.ymin + y);
          const int64_t index = int64_t(y) * region_width + x;
          pixels[index * 4 + 0] = grey;
          pixels[index * 4 + 1] = grey;
          pixels[index * 4 + 2] = grey;
          pixels[index * 4 + 3] = 1.0f;
        }
      }
    });
    perf_input.note("kind=%d size=%dx%d region=%d,%d-%d,%d",
                    int(effect.kind_subtype),
                    region_width,
                    region_height,
                    region_small.xmin,
                    region_small.ymin,
                    region_small.xmax,
                    region_small.ymax);
  }
  /* The whole kernel over the whole small buffer: the kind's own reading of the buffer's texel
   * grid is the local one the gate above vouched for, and the interior it writes is the interior
   * the copy below takes. The CPU kernel alone -- a cropped buffer has not the canvas the mask GPU
   * provider builds its plan for (see #mask_effect_kernel_apply). A kernel with no answer (a
   * node-group filter without a backend) refuses the patch, and the caller runs the full compute. */
  if (mask_effect_kernel_apply(*small, effect, tile_number, /*allow_gpu=*/false) == nullptr) {
    return false;
  }

  std::shared_ptr<ImBuf> patched = paint_layer_anchor_slice_buffer_clone(*cached);
  if (patched == nullptr) {
    return false;
  }
  const float *src = small->float_buffer.data;
  float *dst = patched->float_data_for_write();
  for (const int y : IndexRange(valid.ymin, BLI_rcti_size_y(&valid))) {
    const float *src_row = src +
                           (int64_t(y - region_small.ymin) * region_width +
                            int64_t(valid.xmin - region_small.xmin)) *
                               4;
    float *dst_row = dst + (int64_t(y) * width + valid.xmin) * 4;
    memcpy(dst_row, src_row, size_t(BLI_rcti_size_x(&valid)) * sizeof(float[4]));
  }
  /* The versions are taken after the render: an edit that landed while it ran is read next time. */
  r_patched_synced = map_sources_snapshot(images);
  r_patched = std::move(patched);
  r_region_valid = valid;
  perf_total.note("kind=%d size=%dx%d region=%d,%d-%d,%d",
                  int(effect.kind_subtype),
                  width,
                  height,
                  valid.xmin,
                  valid.ymin,
                  valid.xmax,
                  valid.ymax);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cached result
 * \{ */

std::shared_ptr<ImBuf> paint_layer_mask_effect_buffer_get(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &effect,
    const bool describe_only,
    Vector<Image *> *r_images,
    const int tile_number,
    const PaintLayerSurfaceRef painted_object,
    const rcti *changed_region,
    rcti *r_valid_region)
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
      /* The guard is a thread-local static, so its destructor runs after the leak check: an
       * emptied vector would still hold its capacity and be reported as a leak (176 bytes here).
       * Release the storage the moment the stack empties -- the common case, no nested compute. */
      if (stack.is_empty()) {
        stack.clear_and_shrink();
      }
    }
  } computing_pop{computing};

  PaintMaterialCompositeImageLayer input_entry;
  int width = 0;
  int height = 0;
  if (!mask_effect_input_entry(
          ma, row, effect, tile_number, painted_object, input_entry, width, height))
  {
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

  const EffectCacheKey key = mask_effect_key_get(ma, row, effect, tile_number, painted_object);
  std::shared_ptr<ImBuf> color = paint_layer_effect_cache_get_or_compute_region(
      g_paint_mask_effect_cache,
      g_paint_mask_effect_cache_mutex,
      key,
      width,
      height,
      images,
      g_paint_mask_effect_cache_budget,
      [&](std::shared_ptr<ImBuf> &r_color, Vector<PaintLayerEffectSource> &r_synced) {
        return paint_layer_mask_effect_compute(
            ma, row, effect, tile_number, painted_object, r_color, r_synced);
      },
      changed_region,
      r_valid_region,
      [&](const std::shared_ptr<ImBuf> &cached,
          const rcti &maps_region,
          std::shared_ptr<ImBuf> &r_patched,
          Vector<PaintLayerEffectSource> &r_patched_synced,
          rcti &r_region_valid) {
        return mask_effect_region_compute(ma,
                                          row,
                                          effect,
                                          tile_number,
                                          painted_object,
                                          input_entry,
                                          images,
                                          cached,
                                          maps_region,
                                          r_patched,
                                          r_patched_synced,
                                          r_region_valid);
      });
  if (color != nullptr && r_valid_region != nullptr && changed_region != nullptr &&
      !BLI_rcti_is_empty(changed_region))
  {
    /* The same rule the row's effect writer applies (#paint_layer_effect_buffer_get): the answer is
     * current as a whole whichever way the cycle answered, so the band the caller's map is missing
     * -- the offered band united with the one the cycle named, grown by the halo the kernel reads
     * past it -- can be laid from it. A band the maps reach through a remapping is not the input's
     * own texel grid, and no band can be narrowed there: the whole canvas answers instead. */
    if (!paint_layer_kind_reads_anchor_point(effect) &&
        BKE_paint_layers_effect_region_is_exact(input_entry, images, width, height))
    {
      rcti united = *r_valid_region;
      BLI_rcti_union(&united, changed_region);
      float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
      paint_layer_effect_filter_params(effect, params);
      const int halo = BKE_paint_layers_effect_halo(
          eMaterialPaintLayerFilter(effect.kind_subtype), params, width, height);
      BKE_paint_layers_effect_region_clip(united, halo, width, height, *r_valid_region);
    }
    else {
      BLI_rcti_init(r_valid_region, 0, width, 0, height);
    }
  }
  return color;
}

void paint_layer_mask_effect_cache_drop(const Material &ma,
                                        const MaterialPaintLayer &row,
                                        const MaterialPaintLayer &effect,
                                        const int tile_number,
                                        const PaintLayerSurfaceRef painted_object)
{
  const EffectCacheKey key = mask_effect_key_get(ma, row, effect, tile_number, painted_object);
  std::lock_guard<std::mutex> lock(g_paint_mask_effect_cache_mutex);
  const auto no_release = [](MaskEffectCacheEntry &) {};
  g_paint_mask_effect_cache.drop_instance(key, no_release);
}

std::shared_ptr<ImBuf> BKE_paint_layers_mask_effect_grey(const Material &ma,
                                                         const MaterialPaintLayer &row,
                                                         const MaterialPaintLayer &effect,
                                                         const int tile_number,
                                                         const PaintLayerSurfaceRef painted_object)
{
  if (effect.role != MA_PAINT_LAYER_ROLE_MASK_ITEM ||
      !paint_layer_mask_item_bakes_effect(effect))
  {
    return {};
  }
  return paint_layer_mask_effect_buffer_get(
      ma, row, effect, /*describe_only=*/false, nullptr, tile_number, painted_object);
}

/** \} */

}  // namespace blender
