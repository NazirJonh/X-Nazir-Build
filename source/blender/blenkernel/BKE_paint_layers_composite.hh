/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * \file
 * \ingroup bke
 *
 * The CPU half of a layered material's channel: the description (`Material::paint_layers`) is
 * flattened into the image-layer model the compositor evaluates, with no node graph involved.
 *
 * It exists so the pixels a stroke is judged against and the pixels the render produces come from
 * the same description by construction: both read the rows of the description, the one building a
 * GPU graph and this one a CPU stack.
 */

#include "BKE_paint_material_composite.hh"

namespace blender {

struct Image;
struct ImBuf;
struct Material;
struct MaterialPaintLayer;
struct ReportList;
struct rcti;

/**
 * The layer stack \a channel of \a ma's description resolves to, bottom to top.
 *
 * A row takes part when it carries a live record for the channel: its map becomes an image layer, a
 * flat channel value with some coverage becomes a constant layer. A record without a map and
 * without coverage lays nothing down and is left out. The stack starts from the channel's bottom
 * colour (#BKE_paint_layers_channel_bottom_color) and every row is blended over it with its
 * opacity.
 *
 * Alpha is not one thing. A map's own alpha is its coverage: a texel the painter left transparent
 * shows what is below. A flat value's alpha is never coverage: the row covers by its opacity (and
 * mask) alone, and the value's alpha only does two things -- it decides whether the row takes part
 * (an alpha of 0 lays nothing down) and it travels, as the row's content alpha, in the result's
 * alpha component, mixed in exactly like its colour:
 * `a = a_below * (1 - f) + a_row * f` with `f = opacity * mask`.
 * The result's alpha is therefore "how much of the rows' own content is present", not a coverage
 * of the stack: consumers decide what to do with it (the float export premultiplies by it, a byte
 * export writes it as is). The generated graph's factor follows the same rule.
 *
 * \param describe_only: describe the stack without rendering any Anchor Point slice: every live
 *        reader keeps its descriptor and the maps its slice subscribes to, but gains no
 *        `anchor_point_color`/`anchor_point_coverage`/`anchor_point_slice` buffer. The GPU
 *        Combined preview asks for this, since it captures the slices into its own slots; a CPU
 *        caller (the composite, a bake, the Image Editor) must leave it false. The slice cache is
 *        not populated in this mode.
 * \return false when the channel has no participating row at all.
 */
bool BKE_paint_layers_composite_image_layers(
    const Material &ma,
    int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers,
    bool describe_only = false);

/**
 * Drop every cached Anchor Point slice. The cache owns float canvases of its own (about 0.5 GiB per
 * reader and channel at 4K), so it has to be emptied before the leak check at exit, and with the
 * rest of the session when a file is loaded: its entries name materials of the file going away.
 */
void BKE_paint_layers_slice_cache_free_all();

/**
 * Composite \a channel of \a ma into \a r_dst, which must be byte RGBA of the stack's dimensions.
 *
 * The result is encoded in \a r_dst's own colorspace; a buffer that names none gets the channel's
 * own (sRGB for a colour channel, data for a scalar or Normal one).
 *
 * \param region: when given, only this rectangle is recomputed, exactly as
 *                #BKE_paint_material_composite_eval_linear takes it.
 * \return false when the channel is not compositable from the description (see
 *         #BKE_paint_layers_composite_image_layers).
 */
bool BKE_paint_layers_composite_channel(const Material &ma,
                                        int channel,
                                        ImBuf &r_dst,
                                        const rcti *region = nullptr);

/**
 * Composite \a channel of \a ma into \a dst, for a caller that holds an #Image rather than a bare
 * buffer: the one call an export of a channel's result goes through.
 *
 * A float destination is written as straight scene-linear, premultiplied like every float ImBuf, so
 * it compares one to one with what the shader produced; a byte destination goes through
 * #BKE_paint_layers_composite_channel. The image must already have a buffer of the stack's
 * dimensions; a float buffer must be RGBA.
 *
 * On success \a dst is marked dirty and its display and partial-update state stale, so the caller
 * does not have to.
 *
 * \param region: when given, only this rectangle is written.
 * \return false, with a report, when the channel is not a compositable stack or the destination
 *         has no usable buffer.
 */
bool BKE_paint_layers_composite_image(const Material &ma,
                                      int channel,
                                      Image &dst,
                                      const rcti *region = nullptr,
                                      ReportList *reports = nullptr);

/**
 * The row's mask items as the evaluator reads them, bottom to top: the same description
 * #BKE_paint_layers_composite_image_layers carries per layer, exposed for a caller that samples the
 * mask itself (the strokes, see #PaintLayersMaskSampler).
 *
 * \return false when the row has no mask.
 */
bool BKE_paint_layers_mask_items(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 Vector<PaintMaterialCompositeMaskItem> &r_items,
                                 const bUUID *stop_before = nullptr);

/**
 * Describe the slice \a reader lays in \a channel, without rendering it: the same entry
 * #BKE_paint_layers_slice_render evaluates. A mask target describes the source row's mask stack up
 * to the point, a content target the row's content up to it. The row's own opacity, mask and
 * visibility are cleared: a slice is read straight. \a slice_rect limits nested slices to the
 * rectangle a partial render needs.
 *
 * \param dimensions_only: describe only what the slice is made of, so a caller finding the stack
 * dimensions does not render (or allocate) a single nested slice. The entry must not be evaluated.
 * \param allow_empty_mask: a mask slice with no item is a valid slice (a factor of one), rather
 * than a refused one. The GPU capture plan asks for it so the slot can be cleared to one; a bake
 * still refuses it.
 *
 * \return false when the reference does not resolve or the row cannot be described.
 */
bool BKE_paint_layers_slice_describe(const Material &ma,
                                     const MaterialPaintLayer &reader,
                                     int channel,
                                     PaintMaterialCompositeImageLayer &r_entry,
                                     const int *slice_rect = nullptr,
                                     bool dimensions_only = false,
                                     bool allow_empty_mask = false);

/**
 * Render the slice \a reader lays in \a channel at the source row's resolution. The buffers are
 * reference counted and outlive the slice cache; \a dst_rect limits a partial render to that
 * rectangle, leaving the rest zero, and is never cached. Used by the developer GPU verification.
 */
bool BKE_paint_layers_slice_render(const Material &ma,
                                   const MaterialPaintLayer &reader,
                                   int channel,
                                   std::shared_ptr<ImBuf> *r_color,
                                   std::shared_ptr<ImBuf> *r_coverage,
                                   const int *dst_rect = nullptr,
                                   bool dimensions_only = false);

/**
 * Why the GPU Combined preview cannot lay \a layer the way the CPU composite does, or null when it
 * can. Pure: it reads the description only, so it is testable without a GPU. A live Anchor Point
 * slice is supported (the GPU path captures it into a slot); a *baked* correction with a coverage
 * map is not, because the correction pass has no coverage map of its own.
 *
 * \param mask_slice: \a layer is the description of a mask slice captured for a slot, not a row of
 *        the stack. Such an entry may consist of mask items alone; an ordinary row may not, and a
 *        row without a map or colour is still refused.
 */
const char *BKE_paint_layers_gpu_unsupported_reason(const PaintMaterialCompositeImageLayer &layer,
                                                    bool mask_slice = false);

/* -------------------------------------------------------------------- */
/** \name GPU Anchor Point Capture Plan
 *
 * What the GPU Combined preview has to capture before its channel passes, collected and ordered
 * without a GPU so the rules are testable. The gather only allocates textures and dispatches.
 * \{ */

/** What a capture job produces: a content pair, a mask rendered as content, or a mask slot. */
enum class PaintLayersGpuCaptureKind : uint8_t {
  Content = 0,
  MaskContent,
  MaskItem,
};

/** One slice to capture before the channel passes, with the description it was resolved from. */
struct PaintLayersGpuCaptureJob {
  bUUID reader = {};
  bUUID point = {};
  /** The channel enum the slice is read in, never a combined-stack index. */
  int channel = 0;
  /** The rank of #point, not of the reader. */
  int rank = -1;
  PaintLayersGpuCaptureKind kind = PaintLayersGpuCaptureKind::Content;
  /** How many readers lay this same slot; the gather compares the slot once per point. */
  int reader_count = 1;
  Vector<bUUID> readers;
  /** A mask slice with no items is a factor of one, not a refused slice. */
  bool clear_to_one = false;
  /** Described without rendering: the GPU fills the slot from the description. */
  PaintMaterialCompositeImageLayer entry;
};

/**
 * Collect the Anchor Point slices \a stacks need, recursively, into \a r_jobs.
 *
 * Every job's entry is described once (\a stacks themselves are expected to come from
 * #BKE_paint_layers_composite_image_layers with `describe_only`), and the readers nested inside
 * that entry are collected from it, so a reader below a point still gets a slot even when its own
 * row is hidden or lays nothing. A reader on a hidden/zero-opacity row is collected only through
 * such a nested entry: nothing lays it otherwise.
 *
 * Jobs are deduplicated by (point, kind, channel) and sorted by the rank of the point, so a nested
 * producer is captured before the slice that reads it. A shared primitive point yields one job.
 *
 * \param stacks: one stack per entry of \a channels, same length.
 * \param channels: the channel enum each stack was derived for.
 * \param r_reason: a static string saying why no plan could be built, or null.
 */
bool BKE_paint_layers_gpu_capture_plan(
    const Material &ma,
    Span<const Span<const PaintMaterialCompositeImageLayer>> stacks,
    Span<const int> channels,
    Vector<PaintLayersGpuCaptureJob> &r_jobs,
    const char **r_reason = nullptr);

/** The identity of a GPU slice slot: a point read in one channel enum. */
struct PaintLayersGpuSlotKey {
  bUUID point = {};
  int channel = 0;
};

bool BKE_paint_layers_gpu_slot_key_equal(const PaintLayersGpuSlotKey &a,
                                         const PaintLayersGpuSlotKey &b);

/** The index of the slot for \a point in \a channel, or -1. */
int BKE_paint_layers_gpu_slot_find(Span<const PaintLayersGpuSlotKey> slots,
                                   const bUUID &point,
                                   int channel);

/* -------------------------------------------------------------------- */
/** \name GPU Verification
 * \{ */

/** The outcome of comparing one captured slot with the CPU slice it should hold. */
struct PaintLayersGpuVerifyDiff {
  bool ok = true;
  /** The largest difference of any component of any sample: reported, not judged. */
  float max_diff = 0.0f;
  /** The mean over the samples of each sample's largest component difference: what is judged. */
  float mean_diff = 0.0f;
  bool has_nan = false;
  int64_t samples = 0;
};

/**
 * Compare paired samples of a captured GPU slot and its CPU slice. The two arrays hold
 * \a sample_count samples of \a components floats each, already paired by the caller.
 *
 * Half-float storage and the different sampling at a slice edge mean a per-pixel comparison is
 * meaningless; \a tolerance applies to the mean per-sample difference, 1e-2 by default. A NaN in
 * either array fails the comparison. With no samples \a samples is 0 and nothing was compared.
 */
PaintLayersGpuVerifyDiff BKE_paint_layers_gpu_verify_compare_slot(const float *gpu,
                                                                  const float *cpu,
                                                                  int64_t sample_count,
                                                                  int components,
                                                                  float tolerance = 1e-2f);

/**
 * Owns the textures a GPU capture slot allocated and releases them in its destructor, however far
 * the slot got: one allocation that fails must not leak the one before it. The release function is
 * supplied by the caller, so the guard itself has no GPU dependency and is testable.
 */
class PaintLayersGpuSlotGuard {
 public:
  using ReleaseFn = void (*)(void *release_data, void *resource);

  PaintLayersGpuSlotGuard() = default;
  PaintLayersGpuSlotGuard(const PaintLayersGpuSlotGuard &) = delete;
  PaintLayersGpuSlotGuard &operator=(const PaintLayersGpuSlotGuard &) = delete;
  PaintLayersGpuSlotGuard(PaintLayersGpuSlotGuard &&other) noexcept;
  PaintLayersGpuSlotGuard &operator=(PaintLayersGpuSlotGuard &&other) noexcept;
  ~PaintLayersGpuSlotGuard();

  void set_release(ReleaseFn fn, void *release_data);
  /** Take ownership of \a resource; null is ignored. */
  void own(void *resource);
  /** Release everything now; the destructor does this otherwise. */
  void release();

  int64_t size() const
  {
    return int64_t(resources_.size());
  }

 private:
  ReleaseFn release_ = nullptr;
  void *release_data_ = nullptr;
  Vector<void *> resources_;
};

/** \} */

/**
 * A CPU sampler of one row's effective mask, for the paint strokes.
 *
 * The row's mask stack is described once through the same reader the composite uses
 * (#BKE_paint_layers_mask_items), and its maps are acquired per UDIM tile through the same decode
 * and mapping the composite acquires them with. #sample returns the scalar factor `M` the composite
 * lays the row's coverage under (#BKE_paint_material_mask_factor_eval), so a stroke cannot hide what
 * the render shows: one reading of the mask rules for the canvas and for the render.
 *
 * A row with no mask items is empty and samples as one. A map that cannot be read is treated as an
 * absent one, as in the composite: a laid item falls back to its constant, a map-driven operator to
 * `match = 0`.
 */
class PaintLayersMaskSampler {
 public:
  PaintLayersMaskSampler(const Material &ma,
                         const MaterialPaintLayer &layer,
                         const bUUID *stop_before = nullptr);
  ~PaintLayersMaskSampler();

  PaintLayersMaskSampler(const PaintLayersMaskSampler &) = delete;
  PaintLayersMaskSampler &operator=(const PaintLayersMaskSampler &) = delete;

  /** Whether the row has any mask item to apply. */
  bool is_empty() const;

  /**
   * Acquire the maps of UDIM \a tile_number, the painted map's tile being \a ref_width x
   * \a ref_height. Repeating an already acquired tile is free. Not thread safe: acquire every tile
   * on one thread before the threads that call #sample start.
   */
  void tile_acquire(int tile_number, int ref_width, int ref_height);

  /**
   * The mask factor at texel (\a x, \a y) of UDIM \a tile_number; 1 for a tile that was not
   * acquired. Read only, so any number of threads may call it once the tiles are acquired.
   */
  float sample(int tile_number, int x, int y) const;

 private:
  struct Tile {
    int tile_number = -1;
    int width = 0;
    int height = 0;
    /** Owned decoded buffers per item (mask map / coverage map), or null where the item has none. */
    Vector<ImBuf *> items;
    Vector<ImBuf *> coverage;
    /** Pointers into the buffers above, for the evaluator; null where a buffer is absent. */
    Vector<const float *> item_samples;
    Vector<const float *> coverage_samples;
  };

  Vector<PaintMaterialCompositeMaskItem> items_;
  Vector<PaintMaterialCompositeMaskBuffer> buffers_;
  Vector<Tile> tiles_;
};

}  // namespace blender
