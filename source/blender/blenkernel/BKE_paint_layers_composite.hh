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

#include <memory>
#include <utility>

#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "DNA_color_types.h"
#include "DNA_colorband_types.h"

#include "BKE_paint_material_composite.hh"

namespace blender {

struct Image;
struct ImBuf;
struct Material;
struct MaterialPaintLayer;
struct ReportList;
struct rcti;

/**
 * The Compare masks one build pass renders, shared by the channels that read them.
 *
 * A row's Compare Height record gates every channel of the row, so a pass that describes all of
 * them -- the Combined preview gathers eight -- would otherwise render the same mask once per
 * channel: a full evaluation of the rows below plus the row's own content each time, and the
 * mask is channel-independent by construction.
 *
 * An entry is good for the pass that filled it and no longer: the description it was rendered from
 * (opacities, masks, the rows below) can change between passes and nothing here is keyed on it. The
 * key is the row and whether the build was isolating, the two things the mask depends on besides
 * the pass. A caller that describes a single channel has nothing to share and leaves it out.
 */
struct PaintLayersHeightWonCache {
  /** The rendered mask of a row of an isolating or a top-level build; a null entry is "no mask". */
  Map<std::pair<const MaterialPaintLayer *, bool>, std::shared_ptr<ImBuf>> masks;
  /**
   * The rows of this pass whose mask could not be rendered for a size mismatch and which have been
   * said out loud already, keyed like #masks. The mask is rendered once per row and pass, but the
   * diagnostic is worth exactly one line per pass: a stack that compares through a resample the
   * graph has no node for is worth knowing about once, not once per channel.
   */
  Set<std::pair<const MaterialPaintLayer *, bool>> size_mismatch_reported;
};

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
 * \param surface: a Gradient row in Object or Generated space needs the per-object surface to
 *        turn a texel into the coordinate the shader reads. \a surface supplies it for the exact
 *        preview; when it is empty -- or, for a Generated row, carries neither an orco grid nor a
 *        texture-space box -- the layer keeps `approximate = true` and the evaluator uses the
 *        texel's own UV. Editing the row makes no difference: the surface is keyed on the object
 *        and its geometry, not on the line, so a drag cannot make it stale. A UV Gradient never
 *        uses a surface. The layer holds the shared pointer, so its lifetime is extended even if
 *        the caller's cache replaces its own entry.
 * \param height_won_cache: where the Compare masks of this pass are shared with the pass's other
 *        channels (#PaintLayersHeightWonCache); null renders each mask this call needs and keeps
 *        it nowhere, which is what a single-channel caller wants.
 * \return false when the channel has no participating row at all.
 */
bool BKE_paint_layers_composite_image_layers(
    const Material &ma,
    int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers,
    bool describe_only = false,
    std::shared_ptr<const PaintMaterialCompositeSurface> surface = nullptr,
    PaintLayersHeightWonCache *height_won_cache = nullptr);

/**
 * Drop every cached Anchor Point slice. The cache owns float canvases of its own (about 0.5 GiB per
 * reader and channel at 4K), so it has to be emptied before the leak check at exit, and with the
 * rest of the session when a file is loaded: its entries name materials of the file going away.
 */
void BKE_paint_layers_slice_cache_free_all();

/**
 * Drop the cached slices of \a ma. Entries are keyed by the material's session UID, which is never
 * reused, so those of a freed material could not be hit again and would hold their canvases until
 * the byte budget evicted them.
 */
void BKE_paint_layers_slice_cache_free_material(const Material &ma);

/**
 * Drop every cached CPU-computed-effect buffer (a Blur's or a Noise's result). The cache owns float
 * canvases of its own (256 MiB per effect at 4K), so it has to be emptied before the leak check at
 * exit, and with the rest of the session when a file is loaded: its entries name materials of the
 * file going away.
 */
void BKE_paint_layers_effect_cache_free_all();

/** Drop the cached effect buffers of \a ma, keyed by its session UID like the slices are. */
void BKE_paint_layers_effect_cache_free_material(const Material &ma);

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
 * A switched-off base hides the whole stack, corrections included: nothing is described and the
 * mask reads as one, exactly as the row displays. The items stay in the row's data, so switching
 * the base back restores them; a slice reads the stack straight instead (see
 * #BKE_paint_layers_slice_describe).
 *
 * \return false when the row has no mask.
 */
bool BKE_paint_layers_mask_items(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 Vector<PaintMaterialCompositeMaskItem> &r_items,
                                 const bUUID *stop_before = nullptr);

/**
 * Whether the correction \a effect lays in \a channel with the plain blend: Mix, or the combine
 * Normal means by it. Anything else reads the colour below it, which a bake of the correction on its
 * own does not have.
 */
bool BKE_paint_layers_correction_blend_is_normal(const MaterialPaintLayer &effect, int channel);

/**
 * Describe \a corrections (bottom to top, one row's, none a filter or an Anchor Point) as one row
 * kept only for them, so that rendering it with #BKE_paint_material_composite_eval_row_content gives
 * what they lay together, over nothing: their straight colour and the coverage they bring.
 *
 * \param size: the side of the square canvas, for a set that owns no map to say it.
 * \param iuser: the ImageUser naming \a tile_number, given to every map; it must outlive the row. A
 * correction whose map has no such tile lays nothing on it and is left out.
 * \param context_surface: the surface an Object or a Generated Gradient correction reads the texel's
 * coordinate through, the way the 2D preview does (#ED_space_image_gradient_preview_sync). A UV
 * Gradient needs none; a 3D one without a surface falls back to the approximate view, so a bake
 * passes what #BKE_paint_layers_corrections_bake was handed.
 * \return false when no correction lays anything in \a channel on that tile.
 */
bool BKE_paint_layers_corrections_row_describe(const Material &ma,
                                               Span<MaterialPaintLayer *> corrections,
                                               int channel,
                                               int size,
                                               const ImageUser *iuser,
                                               int tile_number,
                                               PaintMaterialCompositeImageLayer &r_row,
                                               const std::shared_ptr<const PaintMaterialCompositeSurface>
                                                   &context_surface = nullptr);

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
 * Describe the **input** of a CPU-computed \a effect: the content \a row holds up to the effect's
 * marker -- its map or flat colour, the corrections below the effect, its children when it is a
 * folder -- and none of the effect itself. The row's own opacity, mask, object mask and blend are
 * cleared from the entry: the effect reads the content alone, and its mask and opacity are laid live
 * over the result, exactly as #BKE_paint_layers_bake_render_node stores them for a row that carries
 * an effect.
 *
 * This is the one description the CPU effect cache renders from (#paint_layer_effect_compute) and
 * the GPU effect pass lays from, so the two cannot disagree about what an effect is computed on.
 *
 * \param dimensions_only: describe only what the input is made of, so a caller finding the size does
 *        not render (or allocate) a single nested slice. The entry must not be evaluated.
 * \param describe_only: keep every reader's descriptor and the maps it subscribes to, but render no
 *        Anchor Point slice and compute no nested effect buffer: the GPU path lays the slices from
 *        its own slots and the nested effects from its own cache. A CPU caller must leave it false.
 * \return false when the row cannot be described.
 */
bool BKE_paint_layers_effect_input_describe(const Material &ma,
                                            const MaterialPaintLayer &row,
                                            const MaterialPaintLayer &effect,
                                            int channel,
                                            bool dimensions_only,
                                            bool describe_only,
                                            PaintMaterialCompositeImageLayer &r_entry);

/**
 * Every map the described effect \a layer renders from, once each and none of them null. The effect
 * owns no map of its own, so a stroke on one of these is what makes its result stale -- the CPU
 * cache compares their versions, and so does the GPU one.
 */
void BKE_paint_layers_effect_input_images(const PaintMaterialCompositeImageLayer &layer,
                                          Vector<Image *> &r_images);

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
 * Whether the combined preview computes \a effect itself, in an effect pass of its own whose result
 * the GPU effect cache holds. #BKE_paint_layers_gpu_unsupported_reason refuses the stack for a
 * CPU-computed effect this answers false for, and the gather queues a pass only for the ones it answers
 * true for, so the refusal, the queue and the shader's own cases stay one rule.
 */
bool BKE_paint_layers_effect_is_gpu_computed(const MaterialPaintLayer &effect);

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
/** \name GPU Effect Result Cache
 *
 * The GPU twin of the CPU effect cache, for the kinds the registry marks
 * #PAINT_LAYER_KIND_GPU_COMPUTED: the combined preview computes the effect in a pass of its own and
 * keeps the result in a texture, so a stroke on the row's colour costs a dispatch instead of the
 * whole-canvas render and kernel the CPU path runs on the main thread. What the cache holds is a
 * texture of the **input's** dimensions and texel space (a Noise is a function of the texel index,
 * and a bake at another size is a resample of it), in straight scene-linear RGBA on the half float
 * the preview's own accumulators use.
 *
 * The texture is an untyped pointer and the acquire and release steps are hooks the caller supplies,
 * the way #PaintLayersGpuSlotGuard supplies its release function: the keying, the byte budget, the
 * eviction order and the staleness rule are then testable without a GPU context, and the cache itself
 * keeps no `GPU_` dependency.
 *
 * A resource the cache hands out is owned by the cache, and the pointer is valid only while the cache
 * holds its entry: another #BKE_paint_layers_effect_gpu_cache_get (a re-key, another size, a budget
 * eviction) drops it again, and so do #BKE_paint_layers_effect_gpu_cache_free_all and
 * #BKE_paint_layers_effect_gpu_cache_free_material. A caller that keeps the texture past that takes
 * a reference of its own through the resource type it handed the hooks for -- `GPU_texture_ref` for
 * a `gpu::Texture` -- and releases it the same way, so an eviction cannot pull the texture out from
 * under a frame that is still laying it.
 *
 * One thread renders. The mutex guards the container and is held across the hooks, never across a
 * dispatch: a caller with real hooks asks on the thread that owns the current GPU context, and every
 * texture it got is laid on that same thread.
 * \{ */

/** A map an effect's input was rendered from, with the version of its pixels at that time. */
struct PaintLayerEffectSource {
  uint32_t image_uid = 0;
  uint64_t version = 0;
};

/** How the GPU effect cache makes and drops the texture it keeps for one entry. Both hooks run with
 * the cache's mutex held and must not re-enter it: no call to the cache API from a hook. */
struct PaintLayerEffectGpuHooks {
  void *data = nullptr;
  /** Make an RGBA half-float canvas of \a width x \a height readable and writable by a shader.
   * Null means the GPU cannot take the effect at all, which the caller reads as #Failed. */
  void *(*acquire)(void *data, int width, int height) = nullptr;
  /** Drop the cache's reference to a texture it made. */
  void (*release)(void *data, void *resource) = nullptr;
};

/** What the cache answered about one effect's GPU result. */
enum class PaintLayerEffectGpuState : uint8_t {
  /** The texture holds the result of the current parameters over the current pixels. */
  Hit = 0,
  /** \a r_resource names a texture to render into: a new one, or one whose input moved. */
  Render = 1,
  /** No texture could be made; the CPU path has to serve this effect. */
  Failed = 2,
};

/**
 * The texture holding \a effect's GPU result, asking for one when the cache has none or the input
 * has moved since it was last computed.
 *
 * \param width, height: the dimensions of the effect's **input**, which are the dimensions of the
 *        result and the texel space the kernel works in.
 * \param images: every map the input renders from (see #BKE_paint_layers_effect_input_images); an
 *        edit of one of these is what makes the entry stale.
 * \param r_dirty_region: when given, the rectangle of the input that has changed since the entry was
 *        published, in input texels -- the whole canvas for a fresh entry or a change that is not one
 *        rectangle. A neighbourhood kernel re-renders that rectangle grown by its halo; a caller that
 *        narrows a dispatch with it must still satisfy the two rules the cache cannot see, that the
 *        maps' texel grid is the input's and that nothing between a map and the canvas remaps it (the
 *        slice cache checks both, see #anchor_slice_region_is_exact). Until it does, the whole texture
 *        is re-rendered and the rectangle is only carried.
 * \param r_resource: the texture, valid for #Hit and #Render.
 */
PaintLayerEffectGpuState BKE_paint_layers_effect_gpu_cache_get(const Material &ma,
                                                               const MaterialPaintLayer &effect,
                                                               int channel,
                                                               int width,
                                                               int height,
                                                               Span<Image *> images,
                                                               const PaintLayerEffectGpuHooks &hooks,
                                                               rcti *r_dirty_region,
                                                               void **r_resource);

/**
 * Record that the texture of this effect's entry now holds its result over \a images at the versions
 * they have now. Call it after the render, the way the CPU cache snapshots the sources afterwards: an
 * edit that landed while the pass ran is then seen as newer than the entry and costs one more update
 * rather than a result that never matches the screen. Nothing is released here, so the hooks are not
 * named: an entry that went away while the frame was rendering (a budget eviction, a re-key) simply
 * stays unpublished, and the next frame asks for it again.
 */
void BKE_paint_layers_effect_gpu_cache_publish(const Material &ma,
                                               const MaterialPaintLayer &effect,
                                               int channel,
                                               int width,
                                               int height,
                                               Span<Image *> images);

/**
 * Drop every cached effect texture. The cache owns textures of its own, so it has to be emptied while
 * the GPU backend is alive, on the main thread -- the same place the CPU effect cache is dropped --
 * and its entries name materials of the file going away. A current context is not required of the
 * caller: a backend given a texture while none is current parks it on its own orphan list and frees
 * it from the next context that runs, the same terms the image and paint-cursor GPU textures are
 * freed on.
 */
void BKE_paint_layers_effect_gpu_cache_free_all();

/** Drop the cached effect textures of \a ma, keyed by its session UID like the CPU ones are. */
void BKE_paint_layers_effect_gpu_cache_free_material(const Material &ma);

/** The GPU effect cache's byte budget currently in force, for the gather's perf reporting. */
size_t BKE_paint_layers_effect_gpu_cache_budget();

/** \} */

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
    Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
    Span<int> channels,
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
int BKE_paint_layers_gpu_slot_find(Span<PaintLayersGpuSlotKey> slots,
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
 *
 * \param hidden_with_base: false reads the stack straight for a slice (see
 *        #BKE_paint_layers_slice_describe); the default hides the whole stack together with a
 *        switched-off base, the way the row's own mask displays.
 */
class PaintLayersMaskSampler {
 public:
  PaintLayersMaskSampler(const Material &ma,
                         const MaterialPaintLayer &layer,
                         const bUUID *stop_before = nullptr,
                         const bool hidden_with_base = true);
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

/**
 * Build the table the Combined preview's shader reads in place of \a curve_mapping. The mapping's
 * tables must be built (#BKE_curvemapping_init) when this runs.
 *
 * Row 0 is `BKE_curvemapping_table_RGBA`: one texel per table entry, with the R, G, B and combined
 * curves in its components. Row 1 holds the data the GLSL node reads as uniforms: texel 0 the start
 * slopes, 1 the end slopes, 2 the range minimums and 3 the range dividers, each four values
 * (#CM_TOT). \a r_width is the table's entry count (at least four, for row 1).
 */
void paint_layer_curves_lut_build(const CurveMapping &curve_mapping,
                                  Vector<float4> &r_texels,
                                  int &r_width,
                                  int &r_height);

/**
 * Build the table the Combined preview's shader reads in place of \a band. \a r_texels holds
 * `r_width` samples of `BKE_colorband_evaluate(band, i / (r_width - 1))`, the stop's alpha in `.w`.
 * The interpolation and colour mode are already part of the evaluation.
 */
void paint_layer_color_ramp_lut_build(const ColorBand &band,
                                      Vector<float4> &r_texels,
                                      int &r_width);

}  // namespace blender
