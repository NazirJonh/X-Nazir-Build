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

#include <cstdint>
#include <memory>
#include <utility>

#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_color_types.h"
#include "DNA_colorband_types.h"

#include "BKE_paint_layers_tile.hh"
#include "BKE_paint_material_composite.hh"

namespace blender {

struct Image;
struct ImBuf;
struct Material;
struct MaterialPaintLayer;
struct Object;
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
 * (opacities, masks, the rows below) can change between passes and nothing here is keyed on it.
 */
struct PaintLayersHeightWonKey {
  const MaterialPaintLayer *row = nullptr;
  bool isolating = false;
  int tile_number = PAINT_TILE_NUMBER_BASE;

  uint64_t hash() const
  {
    return get_default_hash(this->row, this->isolating, this->tile_number);
  }

  friend bool operator==(const PaintLayersHeightWonKey &a, const PaintLayersHeightWonKey &b)
  {
    return a.row == b.row && a.isolating == b.isolating && a.tile_number == b.tile_number;
  }
};

/**
 * The rendered masks one build pass shares, keyed on #PaintLayersHeightWonKey: the row, whether
 * the build was isolating and the UDIM tile the maps were read on, the three things the mask
 * depends on besides the pass. A caller that describes a single channel has nothing to share and
 * leaves it out.
 */
struct PaintLayersHeightWonCache {
  /** The rendered mask of a row of an isolating or a top-level build; a null entry is "no mask". */
  Map<PaintLayersHeightWonKey, std::shared_ptr<ImBuf>> masks;
  /**
   * The rows of this pass whose mask could not be rendered for a size mismatch and which have been
   * said out loud already, keyed like #masks. The mask is rendered once per row and pass, but the
   * diagnostic is worth exactly one line per pass: a stack that compares through a resample the
   * graph has no node for is worth knowing about once, not once per channel.
   */
  Set<PaintLayersHeightWonKey> size_mismatch_reported;
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
 *        `anchor_point_color`/`anchor_point_coverage`/`grey_buffer` buffer. The GPU
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
    PaintLayersHeightWonCache *height_won_cache = nullptr,
    int tile_number = PAINT_TILE_NUMBER_BASE,
    PaintLayerSurfaceRef painted_object = nullptr);

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
 * Drop every cached mask-effect grey (a CPU-computed kind as a mask item). The cache owns float
 * canvases of its own, so it has to be emptied before the leak check at exit, and with the rest of
 * the session when a file is loaded: its entries name materials of the file going away.
 */
void BKE_paint_layers_mask_effect_cache_free_all();

/** Drop the cached mask-effect greys of \a ma, keyed by its session UID like the slices are. */
void BKE_paint_layers_mask_effect_cache_free_material(const Material &ma);

/**
 * The byte budget of a paint cache whose built-in default is \a default_bytes, as
 * #UserDef::paint_layers_cache_budget_mb chooses it: zero (unset) keeps the default, any other
 * value shares that many MiB over every paint cache proportionally to its default, so a smaller
 * value scales every ceiling down together. An entry larger than its cache's budget is not
 * cached, which is what makes a small value safe: it trades recompute for memory.
 */
int64_t BKE_paint_layers_cache_budget_bytes(int64_t default_bytes);

/**
 * The built-in default of the paint cache named \a id in the budget table (the Anchor Point slices,
 * the effect and mask-effect results, the GPU effect results, the stroke corrections, the composite
 * and the Combined preview), or zero for an unknown name. Lets a cache whose default lives outside
 * blenkernel -- the editors' stroke correction cache -- read the one table rather than mirror the
 * number.
 */
int64_t BKE_paint_layers_cache_budget_default_bytes(const char *id);

/**
 * Apply #UserDef::paint_layers_cache_budget_mb to every cache in the budget table: the slice,
 * effect, mask-effect and GPU effect caches, the stroke corrections (which read the preference when
 * they evict, so are left alone here), the composite and the Combined preview. Called when the
 * preferences load and when the preference changes; with the preference unset the caches fall back
 * to their built-in defaults.
 */
void BKE_paint_layers_cache_budgets_sync_from_userdef();

/**
 * Print every paint cache's used and budget bytes to stderr, one `PAINT_MEM caches` line each and
 * a `PAINT_MEM caches_total` sum, when `BLENDER_PAINT_MEM_LOG` is set; a no-op otherwise. \a where
 * names the caller (the bake commit, a manual probe) and is echoed on each line. For diagnosing
 * the 4K texture-memory footprint, never on a pixel path. Main thread only: the sizes are read
 * without the caches' locks, so a probe must not run on a bake worker.
 */
void BKE_paint_layers_cache_debug_log(const char *where);

/**
 * The grey the CPU-computed mask effect \a effect lays on its owner row \a row, on \a tile_number:
 * the mask effect cache's buffer, computed when no fresh one stands. The buffer is a scalar grey --
 * one float per texel, `channels = 1` -- which the readers take as the mask factor. A Normal to
 * Mask item's grey is the kernel's output over the Normal channel of the slice its Anchor Point
 * references; the item's own invert, blend and opacity are laid by the reader of the grey, not by
 * it.
 *
 * \return null when \a effect is not a mask item of \a row, is no CPU-computed kind, or its input
 * cannot be rendered (a Normal to Mask with an Anchor Point that does not resolve).
 */
std::shared_ptr<ImBuf> BKE_paint_layers_mask_effect_grey(const Material &ma,
                                                         const MaterialPaintLayer &row,
                                                         const MaterialPaintLayer &effect,
                                                         const int tile_number,
                                                         PaintLayerSurfaceRef painted_object =
                                                             nullptr);

/**
 * The parameters of a Normal to Mask item, read through its kind schema: the mode validated into
 * the range the kernel answers for (a file may carry anything), and the two numbers its ramp cuts
 * with. The one read for the kernel, the effect hash and the preview session, so a typo the
 * string getters would answer with a silent zero cannot reach any of them.
 */
struct PaintLayerNormalToMaskParams {
  /**
   * The field the normal rebuilds into: a `PaintLayerNormalToMaskMode` code (0 Height, 1 Edges,
   * 2 Slope). The range is validated here, so no consumer re-checks it.
   */
  int mode = 0;
  /** Where the ramp centers, in the field's normalized [0, 1]. */
  float threshold = 0.5f;
  /** How wide the ramp opens around the position; zero is a hard cut. */
  float softness = 0.1f;
};

/**
 * The parameters of the Normal to Mask item \a item into \a r_params. False for anything that is
 * not the kind (an unknown or foreign kind reads no schema of this one).
 */
bool BKE_paint_layers_normal_to_mask_params_get(const MaterialPaintLayer &item,
                                                PaintLayerNormalToMaskParams &r_params);

/**
 * The field the Normal to Mask \a params name, rebuilt from the encoded tangent normal \a slice
 * carries (RGBA float, the encoding the Normal channel reads): one value per texel, plus the
 * robust [0, 1] window the field maps into before its cut. The Height mode is a Poisson solve,
 * so this is the half a preview caches: the cut is cheap, the solve is not. A slice whose float
 * buffer is missing computes nothing and leaves \a r_field empty.
 */
void BKE_paint_layers_normal_to_mask_field_compute(const ImBuf &slice,
                                                   const PaintLayerNormalToMaskParams &params,
                                                   blender::Vector<float> &r_field,
                                                   float &r_lo,
                                                   float &r_hi);

/**
 * The cut of the Normal to Mask: the field \a field -- computed over \a buffer's own size, with
 * the window \a lo, \a hi it was normalized by -- through the ramp \a params name, written over
 * the RGB of \a buffer in place. Alpha is untouched. The grey is one formula with the kernel's
 * (#paint_layer_normal_to_mask_ramp_apply), so a preview cannot drift from what the bake lays.
 */
void BKE_paint_layers_normal_to_mask_cut_apply(ImBuf &buffer,
                                               const blender::Span<float> field,
                                               const float lo,
                                               const float hi,
                                               const PaintLayerNormalToMaskParams &params);

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
                                        const rcti *region = nullptr,
                                        int tile_number = PAINT_TILE_NUMBER_BASE,
                                        PaintLayerSurfaceRef painted_object = nullptr);

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
                                      ReportList *reports = nullptr,
                                      int tile_number = PAINT_TILE_NUMBER_BASE,
                                      PaintLayerSurfaceRef painted_object = nullptr);

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
                                 const bUUID *stop_before = nullptr,
                                 int tile_number = PAINT_TILE_NUMBER_BASE,
                                 PaintLayerSurfaceRef painted_object = nullptr);

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
                                                   &context_surface = nullptr,
                                               PaintLayerSurfaceRef painted_object = nullptr);

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
 * \param context: the object a projected row or mask item reads its surface from, and the UDIM
 * tile the slice describes: every map of the source row is read on that tile, the way the
 * composite reads the row on it. An unset tile resolves through the object (see
 * #BKE_paint_layers_tile_resolve).
 *
 * \return false when the reference does not resolve or the row cannot be described.
 */
bool BKE_paint_layers_slice_describe(const Material &ma,
                                     const MaterialPaintLayer &reader,
                                     int channel,
                                     PaintMaterialCompositeImageLayer &r_entry,
                                     const int *slice_rect = nullptr,
                                     bool dimensions_only = false,
                                     bool allow_empty_mask = false,
                                     const PaintLayerTileContext &context = {});

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
 * \param context: the object a projected row, correction or mask item of the input reads its
 *        surface from (null refuses the projection), and the UDIM tile the input describes: every
 *        map of the row is read on that tile, the way the composite reads the row on it. An unset
 *        tile resolves through the object.
 * \return false when the row cannot be described.
 */
bool BKE_paint_layers_effect_input_describe(const Material &ma,
                                            const MaterialPaintLayer &row,
                                            const MaterialPaintLayer &effect,
                                            int channel,
                                            bool dimensions_only,
                                            bool describe_only,
                                            PaintMaterialCompositeImageLayer &r_entry,
                                            const PaintLayerTileContext &context = {});

/**
 * Every map the described effect \a layer renders from, once each and none of them null. The effect
 * owns no map of its own, so a stroke on one of these is what makes its result stale -- the CPU
 * cache compares their versions, and so does the GPU one.
 */
void BKE_paint_layers_effect_input_images(const PaintMaterialCompositeImageLayer &layer,
                                          Vector<Image *> &r_images);

/**
 * The result of the CPU-computed \a effect on its row's content in \a channel, exactly as the CPU
 * effect cache holds it for the composite and the bake: a premultiplied, scene-linear RGBA float
 * buffer at the input's own size, the kernel's answer written over the input it rendered. The row
 * the effect hangs on is found by its marker, the way the GPU preview's job walk does.
 *
 * This is the one public read of the CPU effect cache's buffer, so the GPU Combined preview can
 * publish a CPU-computed kind's answer as a GPU result: uploading this buffer is parity by
 * construction -- the pixels are the kernel's own output, not a re-computation of it.
 *
 * \param effect: a filter effect row (#MA_PAINT_LAYER_ROLE_EFFECT) a #PAINT_LAYER_KIND_NEEDS_BAKE
 *        kind computes; anything else answers null, as does an effect no row of \a ma carries or
 *        whose input cannot be rendered. A source kind's answer is its bake map, not this buffer.
 * \param r_width, r_height: the buffer's dimensions, the input's own.
 * \return the cached buffer, computed on a miss. The cache's mutex is never held while an effect
 *         computes, so the compute may render nested slices and nested effects of its own; a caller
 *         on the main thread (the preview's gather runs there) is the safe one, matching the terms
 *         the bake job computes under.
 */
std::shared_ptr<ImBuf> BKE_paint_layers_effect_buffer_result(const Material &ma,
                                                             const MaterialPaintLayer &effect,
                                                             int channel,
                                                             int tile_number,
                                                             int &r_width,
                                                             int &r_height);

/**
 * Describe the input of a Smart Filter chain's GPU-computed \a member: the chain's content up to
 * \a chain on \a row, with the definition members before \a member folded in (the prefix). The
 * member itself is deliberately not named: the description is what its kernel reads, and naming it
 * would make the GPU input render apply the member to its own (not yet rendered) result. The GPU
 * job's input render and the member's CPU kernel both read this one description, so the pixels they
 * work from cannot drift. Same terms as #BKE_paint_layers_effect_input_describe.
 */
bool BKE_paint_layers_chain_member_input_describe(const Material &ma,
                                                  const MaterialPaintLayer &row,
                                                  const MaterialPaintLayer &chain,
                                                  const MaterialPaintLayer &member,
                                                  int channel,
                                                  bool dimensions_only,
                                                  bool describe_only,
                                                  PaintMaterialCompositeImageLayer &r_entry,
                                                  const PaintLayerTileContext &context = {});

/**
 * The result of the GPU-computed \a member of the Smart Filter chain instance \a chain on its
 * owner row's content in \a channel, exactly as the CPU effect cache holds it under the member's
 * composite marker for the composite and the bake: the twin of #BKE_paint_layers_effect_buffer_result
 * for a member, whose key names the composite marker the unfolding built (the chain row's and the
 * member's markers hashed together) rather than a DNA marker, and whose input folds the members
 * before this one. A node-group member reads the instance's exposed values, which live on the
 * chain row; the kernel gets the member's own properties with those overrides folded in.
 *
 * \param row: the stack row the chain hangs on.
 * \param chain: the chain row itself (#MA_PAINT_LAYER_FILTER_CHAIN).
 * \param member: the definition member, a CPU-computed kind (#PAINT_LAYER_KIND_NEEDS_BAKE); a
 *        pointwise or non-effect member answers null, as does a chain row of another kind.
 * \param r_width, r_height: the buffer's dimensions, the input's own.
 * \return the cached buffer, computed on a miss, under the same main-thread terms
 *         #BKE_paint_layers_effect_buffer_result states.
 */
std::shared_ptr<ImBuf> BKE_paint_layers_chain_member_effect_buffer_result(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &chain,
    const MaterialPaintLayer &member,
    int channel,
    int tile_number,
    int &r_width,
    int &r_height);

/**
 * The input transforms the transform effects (a Transform Mapping, a Mirror) of \a row lay over
 * its own content: one entry per enabled effect whose own #MaterialPaintLayerMapping remaps, in
 * the effects list's order, each with #enabled set and no projection, and a no-op effect left
 * out. Each effect reads its own mapping -- a Transform Mapping the offset, rotation, scale and
 * wrap; a Mirror the fold's axes and center -- so the mapping panel, the gizmo and the value sync
 * are the ones every mapped row shares. \a channel decides whether the steps carry the
 * tangent-Normal flag, exactly as a row mapping's description does. The one place that reads the
 * transform kinds' mappings, so the CPU composite, the generated graph and the GPU preview cannot
 * disagree on what a transform effect says. A correction or a mask item keeps its own placement:
 * the transform moves what the row itself lays, not what is stamped over it.
 *
 * An effect in its own Decal frame (#BKE_paint_layers_effect_decal_applies) carries that frame as
 * its step's projection (space Decal, on \a painted_object's surface in \a tile_number); every Decal
 * step of the row carries the first one's frame. Without \a painted_object such a step is left out.
 */
void BKE_paint_layers_input_transforms_get(const Material &ma,
                                           const MaterialPaintLayer &row,
                                           int channel,
                                           Vector<PaintMaterialCompositeMapping> &r_steps,
                                           int tile_number = PAINT_TILE_NUMBER_BASE,
                                           PaintLayerSurfaceRef painted_object = nullptr);

/**
 * Render the slice \a reader lays in \a channel on the context's tile, at the source row's
 * resolution for that tile. The buffers are reference counted and outlive the slice cache; \a
 * dst_rect limits a partial render to that rectangle, leaving the rest zero, and is never cached.
 * Each tile is its own cache entry. Used by the developer GPU verification.
 */
bool BKE_paint_layers_slice_render(const Material &ma,
                                   const MaterialPaintLayer &reader,
                                   int channel,
                                   std::shared_ptr<ImBuf> *r_color,
                                   std::shared_ptr<ImBuf> *r_coverage,
                                   const int *dst_rect = nullptr,
                                   bool dimensions_only = false,
                                   const PaintLayerTileContext &context = {});

/**
 * Whether the combined preview computes \a effect itself, in an effect pass of its own whose result
 * the GPU effect cache holds. #BKE_paint_layers_gpu_unsupported_reason refuses the stack for a
 * CPU-computed effect this answers false for, and the gather queues a pass only for the ones it answers
 * true for, so the refusal, the queue and the shader's own cases stay one rule.
 */
bool BKE_paint_layers_effect_is_gpu_computed(const MaterialPaintLayer &effect);

/**
 * Whether the GPU Combined preview serves this CPU-computed \a effect from the CPU effect cache's
 * buffer instead of an effect pass of its own (#BKE_paint_layers_effect_buffer_result hands the
 * buffer over). The one CPU-computed kind accepted past #BKE_paint_layers_gpu_unsupported_reason's
 * correction refusal and queued by the gather's effect stage, so the refusal, the queue and the
 * kinds stay one rule. The node-group filter, whose kernel runs over the same described input the
 * cache renders, is that kind.
 */
bool BKE_paint_layers_effect_is_precomputed_result(const MaterialPaintLayer &effect);

/**
 * Whether the precomputed \a effect can actually answer where it sits: the shader-bake backend is
 * registered and the row still names its definition tree. Without either, the kernel leaves its
 * buffer unchanged and an upload would publish a wrong "no-op" as the result, so the effect stage
 * refuses the job and the CPU preview answers -- which skips the answer the same way. Pure: it
 * reads the hooks' registration, not the GPU, so it is testable without a context.
 */
bool BKE_paint_layers_effect_node_group_can_compute(const MaterialPaintLayer &effect);

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

/**
 * Describe \a layer alone, as a whole row, in \a channel for the GPU: the same entry
 * #BKE_paint_layers_bake_render_node renders on the CPU, but described only (maps stay images, no
 * nested slice is rendered). False under the same refusals as that function (a generator the bake
 * cannot evaluate, an Object or Generated pattern, which need a surface the GPU path has no use
 * for), and when the row has nothing in \a channel.
 */
bool BKE_paint_layers_gpu_row_entry(const Material &ma,
                                    const MaterialPaintLayer &layer,
                                    int channel,
                                    PaintMaterialCompositeImageLayer &r_entry,
                                    PaintLayerSurfaceRef painted_object = nullptr);

/* -------------------------------------------------------------------- */
/** \name GPU Effect Pass Plan
 *
 * The shape a GPU effect pass takes, decided from the effect's kind and parameters alone -- no GPU,
 * no dispatch, so the gather and the tests read one rule. A #PAINT_LAYER_KIND_GPU_COMPUTED effect is
 * a list of one or more passes: a pointwise kernel (a Noise) is one pass, a separable one (a Blur) is
 * a pass per axis, an unsharp-style one (a Sharpen, a High Pass) adds a luminance pre-pass and a
 * combine tail that reads the original and the blurred copy. The gather runs the plan in order over a
 * ping-pong pair; a #PaintLayerEffectPass names one dispatch of it. A future reduction pass (a Color
 * Match's average) is one more kind here, with no rewrite of what is below.
 * \{ */

/**
 * One dispatch of an effect's GPU pass plan. The values mirror the shader's `COMBINED_PASS_*`
 * defines one to one; the gather static-asserts the mirror, so renumbering here without the shader
 * fails the build rather than dispatching the wrong kernel.
 */
enum class ePaintLayerEffectPass : uint8_t {
  /** The whole Noise kernel: one pointwise pass over the input. */
  Noise = 0,
  /** Scene-linear luminance to all three channels: a High Pass grayscale pre-pass. */
  Luminance,
  /** One horizontal box pass: window `[max(i-r,0), min(i+r,size-1)]`, divide by `hi-lo+1`. */
  BlurH,
  /** One vertical box pass, over the result of #BlurH. */
  BlurV,
  /** One horizontal min/max pass (a #PAINT_LAYER_KIND of Erode / Dilate). */
  ExtremumH,
  /** One vertical min/max pass, over the result of #ExtremumH. */
  ExtremumV,
  /** The Sobel 3x3 gradient magnitude of an Edge Detect: taps clamped to the canvas edge. */
  Edge3x3,
  /** The Emboss diagonal lighting 3x3: a flat colour is its own result. */
  Emboss3x3,
  /** The Sharpen combine tail: the raw detail back on top of the colour, clipped below zero. */
  UnsharpTail,
  /** The High Pass combine tail: the detail against the mid grey, through the sRGB curve. */
  HighPassTail,
  /** The line average at an angle of a Blur Directional: taps clamped to the canvas edge, the
   * mix read off the effect's parameters. */
  BlurDirectional,
  /** The single-pass luminance-driven edge blend of an FXAA: the nine lumas name the edge, the
   * gradient names the direction, and the pair of samples along it blend. */
  Fxaa,
  /** The Glow's luminance choke: the spread clamps the luminance buffer to the glow band, a
   * pointwise pass over a #Luminance result. */
  Choke,
  /** The Glow's screen tail: the blurred choke lights the original colour through the screen
   * formula, reading the blurred band through its aux. */
  ScreenTail,
  /** The Pixelate block average: one pass, each texel averages the alpha-weighted block it
   * belongs to out of the original; the amount -- the block count per side, the canvas cut into it
   * per axis like the CPU's -- comes through `aux0`. */
  BlockAverage,
  /** The Height To Normal Sobel: one 3x3 pass over the height's red channel with the alpha weight
   * to the neutral, encoded GL or DX; the world scale comes through `aux0` and the green
   * convention (+1 the OpenGL encoding, -1 the DirectX one) through `params.x`. */
  HeightToNormal3x3,
  /** The Channel Split slot fetch: one pass, a bilinear sample of the slot map named by the aux
   * at the texel's own UV, one channel taken as grey; the channel index comes through `aux0`. */
  SlotFetch,
  /** The distance transform's seed field: texels whose scalar sits below the threshold stamp their
   * own coordinates, the rest stamp a far point; the JFA passes refine the field from here. */
  JfaInit,
  /** One Jump Flooding step: each texel keeps the nearest seed its `radius`-neighbourhood reports,
   * the radius halving every pass until one. */
  JfaStep,
  /** The distance field's read: the Euclid distance to the seed field turns into the Bevel's ramp
   * or the Mask Outline's band, per `params`; the band writes its own alpha. */
  DistEval,
  /** The Drop Shadow's silhouette: the source the effect names (its alpha or its luminance) as
   * one grey, the spread and the blur working on it afterwards. */
  Silhouette,
  /** The Drop Shadow's combine tail: the blurred silhouette read at the shadow's offset -- past
   * the canvas it reads zero, not the clamped edge -- tinted under the original colour, its own
   * alpha written. */
  OffsetTintTail,
  /** The Blur Slope's driver: the luminance of the (blurred) input, or the value noise, re-graded
   * by the position and contrast -- one grey whose gradient the walk follows. */
  SlopeDriver,
  /** The Blur Slope's walk: the gradient of the driver names the direction, the span names the
   * reach, and the taps of the original blend by the mode (average, minimum, maximum). */
  SlopeWalk,
  /** The Mask Outline's tint tail: the blurred band turns into the premultiplied colour with the
   * band as its own alpha. */
  BandTintTail,
  /** The Color Match reduction's first level: the alpha-weighted colour sum of every 2x2 block of
   * the input, into a float32 pyramid level the steps halve down to one texel. */
  ReduceStart,
  /** One Color Match reduction step: the sum of the four texels the level above names, into the
   * half-size level; the `radius` names which level this is. */
  ReduceStep,
  /** The Color Match's grade: the average the 1x1 pyramid texel carries (its aux) drives the
   * pointwise hue / chroma / luma match of the original, the variation seeded by the texel hash. */
  MatchGrade,
  /** The Fill Area's label field: the border and the canvas edge stamp the background label zero,
   * every other texel its own coordinates; the flooding steps refine it from here. */
  FloodInit,
  /** One Fill Area flooding step: each texel keeps the smallest label its `radius`-neighbourhood
   * reports, the background label swallowing every region the edge reaches. */
  FloodStep,
  /** One Fill Area diffusion step: the average of the four neighbours inside the same region, one
   * pass of the CPU's iterations; the `radius` names which one. */
  DiffuseStep,
  /** The Fill Area's write tail: the diffused colour goes out with the coverage the flooding
   * named, the layer of its own the effect leaves. */
  FillTail,
};

/** Which buffer a pass's `input_tx` reads. The gather binds the texture the role names. */
enum class ePaintLayerEffectPassSource : uint8_t {
  /** The effect's rendered input canvas. */
  Input = 0,
  /** The destination of the pass before this one. */
  Previous,
  /** The luminance buffer a grayscale High Pass keeps from its pre-pass. */
  Luminance,
};

/** Which buffer a pass's `aux_tx` reads; the passes that read no second buffer name none. */
enum class ePaintLayerEffectPassAux : uint8_t {
  None = 0,
  /** The blurred copy the plan built before this pass (a combine tail's). */
  Previous,
  /** The effect's rendered input canvas, the same texture #ePaintLayerEffectPassSource::Input
   * names. The Blur Slope's walk reads it for its taps, and the tail of a future reduction effect
   * (a Color Match's grade) will too. */
  Original,
  /** The slot map a Channel Split reads: a texture the host binds for the pass, outside the
   * effect's own ping-pong buffers. */
  Slot,
  /** The refined label field a Fill Area's diffusion reads: the float32 texture the flooding
   * passes left, outside the ping-pong pair. */
  Field,
};

/** Which axes a pass's window reaches along, its share of the halo and the region growth. */
enum ePaintLayerEffectPassAxis : uint8_t {
  PAINT_LAYER_EFFECT_AXIS_NONE = 0,
  PAINT_LAYER_EFFECT_AXIS_X = 1 << 0,
  PAINT_LAYER_EFFECT_AXIS_Y = 1 << 1,
  PAINT_LAYER_EFFECT_AXIS_XY = PAINT_LAYER_EFFECT_AXIS_X | PAINT_LAYER_EFFECT_AXIS_Y,
};

/** One pass of the plan: which kernel, which buffers, at what radius, with the parameters it reads. */
struct PaintLayerEffectPass {
  ePaintLayerEffectPass kind = ePaintLayerEffectPass::Noise;
  /** The texture the shader's `input_tx` reads for this pass. */
  ePaintLayerEffectPassSource source = ePaintLayerEffectPassSource::Input;
  /** The texture the shader's `aux_tx` reads, or #ePaintLayerEffectPassAux::None. */
  ePaintLayerEffectPassAux aux = ePaintLayerEffectPassAux::None;
  /** The window half-reach in texels, already clamped the way the CPU kernel clamps it. */
  int radius = 0;
  /** #ExtremumH / #ExtremumV: dilate (maximum) when true, erode (minimum) when false. */
  bool maximum = false;
  /**
   * How far past an edited texel this pass's own output depends on its input, in texels: the
   * window passes reach their #radius, a 3x3 reaches one, a pointwise pass and a tail reach
   * nothing. The regions and the halo read this, not #radius, so a pass that only reaches along
   * one axis widens only that axis (an H pass after a V one does not grow the region's height).
   */
  int reach = 0;
  /** The axes #reach applies along, a #ePaintLayerEffectPassAxis bit set. */
  uint8_t axis_mask = PAINT_LAYER_EFFECT_AXIS_XY;
  /** #UnsharpTail / #HighPassTail: the Sharpen amount or the High Pass contrast, in the effect's
   * own units. */
  float aux0 = 0.0f;
  /**
   * Extra per-pass scalars the kernel reads as a float4: the Glow tail's tint colour, the Height
   * To Normal's green convention, whatever a future pass kind needs beyond #radius / #maximum /
   * #aux0. The host pushes it as the shader's `pass_params` unchanged.
   */
  float4 params = {0.0f, 0.0f, 0.0f, 0.0f};
  /**
   * The second per-pass float4, for the tails that need a colour and a flag side by side with
   * #params (the Drop Shadow's offset and opacity there, its tint here). The host pushes it as the
   * shader's `pass_params2` unchanged.
   */
  float4 params2 = {0.0f, 0.0f, 0.0f, 0.0f};
  /**
   * Whether this pass computes the alpha it writes instead of carrying the source's: the tails of
   * the effects whose result is a layer of its own (the Mask Outline's band, the Drop Shadow's
   * weight) name their alpha from the kernel, everything else keeps the standard rule. The host
   * pushes it as the shader's `alpha_from_kernel` unchanged.
   */
  bool alpha_from_kernel = false;
  /** Its position in the plan, so the host can tell the last pass (which writes the cache). */
  int pass_index = 0;
  int pass_count = 0;
};

/**
 * The passes a filter computes through, in order, from its kind and its parameters in the schema
 * order (the same order the gather packs into `filter_params`). Pointwise kinds give one pass;
 * separable kinds give one per axis; unsharp kinds give the blur passes plus a combine tail. Each
 * pass names its own sources, so the gather runs the plan without knowing the kinds: the first pass
 * reads #ePaintLayerEffectPassSource::Input, the window passes read the pass before them, a tail
 * reads the original (a Sharpen) or the luminance buffer (a grayscale High Pass) and the blurred
 * copy. A kind that is not #PAINT_LAYER_KIND_GPU_COMPUTED gives an empty plan -- the caller refuses
 * it before this runs. Parameters are read as the shader reads them: integers stored as floats,
 * rounded back.
 */
void BKE_paint_layers_effect_pass_plan(eMaterialPaintLayerFilter filter,
                                       const float *params,
                                       const int width,
                                       const int height,
                                       Vector<PaintLayerEffectPass> &r_passes);

/**
 * The halo -- the reach in input texels of an edit past the edited rectangle that a neighbourhood
 * kernel reads. Grows the dirty region of the cache so a partial dispatch covers everything the
 * kernel reads. Computed as the pass plan's convolution: the reaches fold per axis (an H pass adds
 * to the X reach only, a V pass to the Y one) and the halo is the larger axis total, which keeps
 * the same numbers the CPU passes' coverage gave -- a box blur reaches `r`, a soft blur three
 * passes of `max(r/2, 1)`, a Sharpen / High Pass reach their blur `r`, an Erode / Dilate reach `r`,
 * a 3x3 kernel reaches one. `params` is read in the schema order for the filter and clamped as the
 * kernel clamps it; a pointwise kind (a Noise) has no halo and gives zero.
 */
int BKE_paint_layers_effect_halo(eMaterialPaintLayerFilter filter,
                                 const float *params,
                                 const int width,
                                 const int height);

/**
 * The parameters of a mask-stack \a effect in the schema order the pass plan reads them by, with the
 * `for_mask` source override the CPU mask kernel applies: the shape-reading kinds (Drop Shadow, Mask
 * Outline, Fill Area, Bevel) take their border from the luminance, because a mask's grey carries no
 * alpha of its own. The packing is the one #paint_layer_effect_filter_params answers -- a colour lays
 * its three channels across three consecutive slots at its place in the schema order -- so a caller
 * outside BKE (the editors' mask GPU provider) reads the same numbers without duplicating the
 * schema.
 */
void BKE_paint_layers_effect_mask_filter_params(
    const MaterialPaintLayer &effect,
    float (&r_params)[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM]);

/** How far an edit of an effect's input reaches in the effect's output. */
enum class EffectReach : int8_t {
  /** One output texel reads one input texel: an edit moves the result inside the edited rect. */
  Pointwise,
  /** A neighbourhood kernel reads past the edited texel, so its result moves around it too. */
  Neighbourhood,
  /** No pass plan describes the kind (a whole-map pass such as a Color Match): the result spans
   * the whole map. */
  WholeMap,
};

/**
 * \a filter's reach with \a params in the schema order, from the same pass plan
 * #BKE_paint_layers_effect_pass_plan and #BKE_paint_layers_effect_halo read. The one answer for the
 * bake's coverage culling: a pointwise effect's bake cannot move past an edited rectangle, while a
 * neighbourhood or whole-map one may. A kind the plan does not describe is #EffectReach::WholeMap,
 * which keeps every region; a plan whose every pass has no reach is #EffectReach::Pointwise. The
 * plan's size arguments only bound its radius clamp, which clamps upwards to one, so the answer
 * does not depend on a canvas and is computed from a one-texel bound.
 */
EffectReach BKE_paint_layers_effect_reach(eMaterialPaintLayerFilter filter, const float *params);

/**
 * The dispatch rectangle of every pass of \a passes, in order, into \a r_rects (one entry per
 * pass). The last pass's rectangle is the input's changed region grown by the plan's whole halo and
 * clamped to the canvas -- the cached result has to be correct there, or the stale band a
 * neighbourhood smears past a stroke never clears. A pass before that writes one rectangle more,
 * by the reach the passes after it still read along each axis (an H pass grows the X side only), so
 * the next pass samples a neighbourhood its source has already computed. \a extra_input_halo grows
 * the changed region first, by the halo of the effects that precede this one in the row: their
 * results are part of this effect's input, so their own halo band re-rendered last frame is part of
 * what this one has to redo. An empty \a dirty gives empty rectangles (nothing moved, nothing to
 * compute); the caller renders the whole canvas instead when the region is not exact. Pure, so the
 * grow / clamp arithmetic is testable without a GPU.
 */
void BKE_paint_layers_effect_pass_regions(Span<const PaintLayerEffectPass> passes,
                                          const rcti &dirty,
                                          int extra_input_halo,
                                          const int width,
                                          const int height,
                                          MutableSpan<rcti> r_rects);

/**
 * Whether a rectangle reported on the input's maps can narrow the effect's dispatch: the described
 * effect's `input_entry` remaps no map (no UV mapping, no live Anchor Point reader between a map and
 * the canvas), and no map an edit was marked on has another size than the input. Both rules as the
 * slice cache checks them (see #anchor_slice_region_is_exact); anything else is a full render.
 */
bool BKE_paint_layers_effect_region_is_exact(const PaintMaterialCompositeImageLayer &input_entry,
                                             Span<Image *> images,
                                             int width,
                                             int height);

/**
 * The dispatch rectangle a pass computes: `dirty` grown by `halo`, clamped to the canvas
 * `[0, width) x [0, height)`. An empty `dirty` stays empty (nothing moved); a full canvas stays
 * full. Pure, so the intersection / grow / clamp arithmetic is testable without a GPU.
 */
void BKE_paint_layers_effect_region_clip(const rcti &dirty,
                                         int halo,
                                         int width,
                                         int height,
                                         rcti &r_out);

/**
 * The rectangle of an owned effect's \a width x \a height result that a map writer may lay from
 * it, the answer #paint_layer_owned_effect_result_compute gives its callers: \a changed_region
 * grown by the halo of \a effect's filter and clamped to the canvas, or the whole canvas when no
 * region is offered, it is empty, or it is not in the input's own texel grid
 * (#BKE_paint_layers_effect_region_is_exact). Shared with the worker job's regional write, which
 * computes an effect's result off the main thread and has to clip the band it writes the same way.
 */
void BKE_paint_layers_effect_valid_region(const Material &ma,
                                          const MaterialPaintLayer &row,
                                          const MaterialPaintLayer &effect,
                                          int channel,
                                          int tile_number,
                                          const rcti *changed_region,
                                          int width,
                                          int height,
                                          rcti &r_valid_region);

/**
 * Whether the content the CPU-computed \a effect reads on \a row is bounded to a rectangle of the
 * input's canvas, and the rectangle in input pixels (an exclusive `x0, y0, x1, y1`, padded one
 * texel past the frame's edge). The rectangle is the footprint the row's bounded UV mapping lays
 * its content in: the preimage of the unit square under the composed chain of the transform
 * effects' steps and the row's own mapping, which a finite kernel's result stays inside once the
 * chain's halo is added. A bake effect with such a footprint can compute and write that rectangle
 * alone -- the result outside it is a function of the input outside it, which the pixels the
 * previous bake left there already answer.
 *
 * False when nothing here bounds the content, and the caller computes the whole canvas: a mapping
 * that is off or repeats an axis, a projected or Decal step, a generator, an Anchor Point reader,
 * a mesh-map atlas, a folder, a laid correction without a frame of its own, a whole-map kind in
 * the chain below the effect, an input with no canvas of its own. A conservative false is always
 * safe; a wrong rectangle never is.
 *
 * \param context: the tile context the effect's input is computed under, the same one the caller
 *        passes the compute (the bake describes and computes on #PAINT_TILE_NUMBER_BASE).
 * \param r_input_width / r_input_height: when given, the input's own canvas the rectangle is in.
 *        The CPU effects filter at that resolution, so a caller writing a `canvas`-sized map scales
 *        the rectangle and the chain's halo by it (`canvas / input`): a two-texel radius of an
 *        8-square input reaches 512 texels of a 2048 map.
 */
bool BKE_paint_layers_effect_input_footprint_region(const Material &ma,
                                                    const MaterialPaintLayer &row,
                                                    const MaterialPaintLayer &effect,
                                                    int channel,
                                                    const PaintLayerTileContext &context,
                                                    rcti &r_region,
                                                    int *r_input_width = nullptr,
                                                    int *r_input_height = nullptr);

/**
 * Whether \a correction stands for an effect row's result: the description builder names the effect
 * row it was made from in #PaintMaterialCompositeCorrection::effect_marker and leaves the marker nil
 * for every other kind (a map, a flat colour, a live reader). The one predicate every reader of the
 * corrections uses, so none of them re-derives "is this an effect" from the fields.
 */
bool paint_correction_is_effect(const PaintMaterialCompositeCorrection &correction);

/**
 * A fingerprint of the effects that precede the effect_marker in a row's content corrections, in
 * order: each one's marker and its own parameter hash. The result of an earlier effect is part of
 * the input of a later one, so a change to an earlier effect -- its parameters or its place in the
 * order -- must re-key the later effect's cache entry, in the CPU row cache and the GPU cache
 * alike. The effect hash of a single effect does not know about the rest of the row; this
 * fingerprint is that missing half. Corrections that are not effects (their marker is nil) carry
 * no fingerprint here.
 */
uint64_t BKE_paint_layers_effect_chain_hash(
    Span<const PaintMaterialCompositeCorrection> corrections, const bUUID &effect_marker);

/**
 * A fingerprint of the described effect input's non-map scalar content -- the row's flat colour, its
 * adjustment filter and parameters, the filters, colours and opacities of the corrections below
 * the effect, and every gradient generator's payload and Anchor Point reader's slice hash in the
 * same description, and the input's transform steps (Mirror, Transform Mapping): a map version
 * does not carry those, and a change to one alone would otherwise serve a result computed from the
 * old pixels. The slot map an effect's kernel samples itself (a Channel Split's,
 * #PaintMaterialCompositeImageLayer::slot_image) joins by identity, its pixels being outside every
 * map version the description carries too. Together with the map versions it names everything a
 * re-render would read; it keys a cache entry beyond the effect's own hash, in the CPU row cache
 * and the GPU cache alike. Pure: it reads the description only, so it is testable without a GPU.
 */
uint64_t BKE_paint_layers_effect_input_scalar_hash(
    const PaintMaterialCompositeImageLayer &input_entry);

/** \} */

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
 * The two fingerprints that key an effect cache entry beyond the effect's own hash. Both are read
 * from the description alone and both are zero when they describe nothing (a lone effect on a
 * plain map input). They are shared by every effect cache -- the CPU row cache, the CPU mask cache
 * and the GPU cache -- since all three key on the same described input: the row cache fills both
 * from the description it computes from, the GPU cache fills both for its jobs, and the mask
 * effect cache (CPU) fills the chain half with the mask built up to the item.
 */
struct PaintLayerEffectKeyExtras {
  /** The ordered fingerprint of the effects that precede this one in its row, whose results feed
   * its input (see #BKE_paint_layers_effect_chain_hash). For a mask effect item, the hash of the
   * mask built up to it. */
  uint64_t chain_hash = 0;
  /** The fingerprint of the input's non-map scalar content (see
   * #BKE_paint_layers_effect_input_scalar_hash). */
  uint64_t input_scalar_hash = 0;
};

bool operator==(const PaintLayerEffectKeyExtras &a, const PaintLayerEffectKeyExtras &b);

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
 * \param key_extras: the two fingerprints of the input beyond the effect's own parameters -- an
 *        ordered hash of the effects that precede this one in the row (whose results feed this
 *        effect's input), and a hash of the input's non-map scalar content (a flat colour, an
 *        adjustment, a filter's parameters, transform steps) that no map version carries. They
 *        key the entry beyond the CPU effect hash, which knows only this effect's own parameters;
 *        leave both zero for an effect with neither. The publish below must pass the same value,
 *        so a hit reads on identical content.
 */
PaintLayerEffectGpuState BKE_paint_layers_effect_gpu_cache_get(const Material &ma,
                                                               const MaterialPaintLayer &effect,
                                                               int channel,
                                                               int width,
                                                               int height,
                                                               Span<Image *> images,
                                                               const PaintLayerEffectKeyExtras &key_extras,
                                                               const PaintLayerEffectGpuHooks &hooks,
                                                               rcti *r_dirty_region,
                                                               void **r_resource,
                                                               int tile_number = PAINT_TILE_NUMBER_AUTO);

/**
 * The same cache, keyed explicitly: \a effect_marker and \a effect_hash name the key's effect half
 * instead of the row's own marker and hash. The Smart Filter chain member a description carries as
 * an effect correction is no DNA row of the material -- its key names the composite marker the
 * unfolding built and the instance-unique hash the correction carries -- so the GPU job walk keys
 * a member's entry through this form, with the values it read from the correction, and the CPU
 * effect cache (which keyed the same member under the same marker and hash) and this one agree on
 * what the member's answer is a function of.
 */
PaintLayerEffectGpuState BKE_paint_layers_effect_gpu_cache_get_keyed(
    const Material &ma,
    const bUUID &effect_marker,
    uint64_t effect_hash,
    int channel,
    int width,
    int height,
    Span<Image *> images,
    const PaintLayerEffectKeyExtras &key_extras,
    const PaintLayerEffectGpuHooks &hooks,
    rcti *r_dirty_region,
    void **r_resource,
    int tile_number = PAINT_TILE_NUMBER_AUTO);

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
                                               Span<Image *> images,
                                               const PaintLayerEffectKeyExtras &key_extras,
                                               int tile_number = PAINT_TILE_NUMBER_AUTO);

/**
 * The publish above, keyed explicitly: the pair a #BKE_paint_layers_effect_gpu_cache_get_keyed ask
 * was keyed under, so a hit reads on identical content. A Smart Filter chain member's job publishes
 * through this form, with the correction's composite marker and instance-unique hash.
 */
void BKE_paint_layers_effect_gpu_cache_publish_keyed(const Material &ma,
                                                     const bUUID &effect_marker,
                                                     uint64_t effect_hash,
                                                     int channel,
                                                     int width,
                                                     int height,
                                                     Span<Image *> images,
                                                     const PaintLayerEffectKeyExtras &key_extras,
                                                     int tile_number = PAINT_TILE_NUMBER_AUTO);

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
/** \name Owned Effect GPU Provider
 *
 * The one hook the bake takes into the GPU half. The editors register the combined preview's GPU
 * effect pipeline here, and the bake of an effect that owns its result asks it before it falls back
 * to the CPU compute, so a bake reads the result the screen already keeps instead of paying the
 * whole-canvas CPU render and kernel.
 * \{ */

/**
 * Computes an owned effect's result the way the combined preview's effect pass does. The result is a
 * float RGBA #ImBuf at the input's size with the CPU effect buffer's semantics: premultiplied by the
 * input's content alpha, and a kind flagged #PAINT_LAYER_KIND_RESULT_HAS_ALPHA writes its own alpha.
 * Null is a refusal (no GPU context, the effect's pass failed) and sends the caller to the CPU path.
 *
 * \param changed_region: the rectangle the caller (a bake that knows the stroke's region) offers
 * for a regional dispatch, in the input's texel grid, or null for the whole canvas. The texture is
 * valid as a whole once the provider returns; the valid rectangle a map writer needs -- the band
 * the dispatch actually changed, grown by the halo -- is filled by the caller
 * (#paint_layer_owned_effect_result_compute) with the same arithmetic the CPU path answers with.
 *
 * \param r_crop: when non-null, the provider may read back only the valid band instead of the whole
 * canvas and fills \a r_crop with the band and the canvas it sits in; leaving it empty means the
 * buffer is the whole canvas, as the \a nullptr caller always gets. The bake's precompute is the one
 * caller that passes it, and only for a map that already stood -- see
 * #bake_job_owned_effects_gpu_precompute.
 *
 * The result lives in the combined preview's own GPU effect cache, keyed the way the preview keys
 * it -- the effect hash plus #PaintLayerEffectKeyExtras and the maps' versions -- so a bake and
 * the screen share one computed result. The provider is asked on the main thread with a current GPU
 * context only; the bake job's worker has neither and stays on the CPU path.
 *
 * The contract the two registration functions below pin: the hook is set on the main thread, before
 * the first bake starts its worker -- the job's precompute and the sync bake both read it after the
 * editors have registered, so a registration that races a bake is a caller's bug the plain pointer
 * does not guard; the provider is always called on the main thread with a GPU context it may have
 * to enable itself; a null answer -- or no provider at all -- is the CPU fallback, never an error.
 */
struct PaintLayerOwnedEffectGpuCrop {
  /** The band the returned buffer holds, in the full canvas' texel coordinates. */
  rcti rect = {};
  /** The canvas the band sits in: the dims a whole-canvas read would have had. */
  int full_width = 0;
  int full_height = 0;
};

using PaintLayerOwnedEffectGpuProvider =
    std::shared_ptr<ImBuf> (*)(const Material &ma,
                               const MaterialPaintLayer &row,
                               const MaterialPaintLayer &effect,
                               const int channel,
                               const int tile_number,
                               const rcti *changed_region,
                               PaintLayerOwnedEffectGpuCrop *r_crop);

/**
 * Register \a provider as the GPU answer for owned effects. Every call replaces the previous one,
 * and null takes the hook away again -- the state before the editors register, in which every bake
 * takes the CPU path. Main thread only, and before the first bake's worker starts reading the hook:
 * the registration is a plain pointer, so an edit that races a bake is the caller's own to keep out.
 */
void BKE_paint_layers_owned_effect_gpu_provider_set(PaintLayerOwnedEffectGpuProvider provider);

/** The provider currently registered, or null. */
PaintLayerOwnedEffectGpuProvider BKE_paint_layers_owned_effect_gpu_provider_get();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask Effect GPU Provider
 *
 * The one hook a mask-stack effect (a Blur, a Noise, an item of a row's mask stack) takes into the
 * GPU half. The editors register the combined preview's GPU effect pipeline here, and the mask
 * effect's compute asks it before it runs the CPU kernel, so a mask item's grey is laid by the same
 * pass the screen keeps instead of the serial-per-texel CPU kernel.
 * \{ */

/**
 * Computes the kernel of a mask-stack effect over a grey canvas. The provider receives the grey the
 * mask has built up to the effect -- RGB holding the same grey in all three channels, alpha one, at
 * the mask canvas' size -- and fills \a r_result with the kernel's raw result: no normalize, no
 * blend of the item's own invert/opacity (the reader lays those), and no luminance fold -- the
 * caller runs the grey normalize after, for the kinds the kind table flags
 * #PAINT_LAYER_KIND_MASK_NEEDS_LUMA.
 *
 * \param effect: the mask item whose kernel to run; the provider reads its own parameters only
 *        (a filter's settings, a Noise's seed), the way the CPU kernel does.
 * \param grey_input: the grey read in. \a r_result may alias it -- the compute fills one buffer
 *        and hands it twice -- and that is safe for the pipeline this hook expects: the input is
 *        read whole into a texture before any dispatch writes, so every texel's source is fixed
 *        before the first write lands. A provider that cannot keep that order (or has no current
 *        context, or the effect's pass fails) returns false and sends the caller to the CPU kernel,
 *        rather than writing a buffer whose input it read half of.
 * \param r_result: the caller-allocated result buffer, the same size as \a grey_input; the provider
 *        fills all of it -- RGB as grey, alpha one, the shape the CPU kernel leaves.
 *
 * False is a refusal and falls back to the CPU path (the registered provider names its stage in
 * the `BLENDER_PBR_PERF` log). The main thread and a
 * current GPU context are the provider's own responsibility, the same terms the owned-effect
 * provider works on: the hook is set on the main thread, before the first bake starts its worker,
 * and the provider is always called on the main thread with a context it may have to enable itself.
 */
using PaintLayerMaskEffectGpuProvider = bool (*)(const MaterialPaintLayer &effect,
                                                 const ImBuf &grey_input,
                                                 ImBuf &r_result);

/**
 * Register \a provider as the GPU answer for mask-stack effects. Every call replaces the previous
 * one, and null takes the hook away again -- the state before the editors register, in which every
 * mask effect takes the CPU path. Reads happen during a composite; the plain pointer keeps no guard
 * for that, the same terms the owned-effect provider keeps. Main thread only, and before the first
 * bake starts its worker.
 */
void BKE_paint_layers_mask_effect_gpu_provider_set(PaintLayerMaskEffectGpuProvider provider);

/** The provider currently registered, or null. */
PaintLayerMaskEffectGpuProvider BKE_paint_layers_mask_effect_gpu_provider_get();

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
 * \param context: the object a projected row in a slice reads its surface from (without it the
 *        slice's projection has no object and the GPU path refuses it), and the UDIM tile the
 *        stacks describe and the slots capture for; each tile is its own plan and its own slots.
 *        An unset tile resolves through the object.
 */
bool BKE_paint_layers_gpu_capture_plan(
    const Material &ma,
    Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
    Span<int> channels,
    Vector<PaintLayersGpuCaptureJob> &r_jobs,
    const char **r_reason = nullptr,
    const PaintLayerTileContext &context = {});

/** The identity of a GPU slice slot: a point read in one channel enum, on one UDIM tile. */
struct PaintLayersGpuSlotKey {
  bUUID point = {};
  int channel = 0;
  /** The tile the slot's slice was captured for; each tile is its own slot. */
  int tile_number = PAINT_TILE_NUMBER_BASE;
};

bool BKE_paint_layers_gpu_slot_key_equal(const PaintLayersGpuSlotKey &a,
                                         const PaintLayersGpuSlotKey &b);

/**
 * The index of the slot for \a point in \a channel on \a tile_number, or -1. An unset tile reads
 * as the base one, the tile the slots of a caller that named none were captured for.
 */
int BKE_paint_layers_gpu_slot_find(Span<PaintLayersGpuSlotKey> slots,
                                   const bUUID &point,
                                   int channel,
                                   int tile_number = PAINT_TILE_NUMBER_AUTO);

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
                         const bool hidden_with_base = true,
                         PaintLayerSurfaceRef painted_object = nullptr);
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
