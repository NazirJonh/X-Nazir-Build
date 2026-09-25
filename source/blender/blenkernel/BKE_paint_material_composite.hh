/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Flattens a layer stack -- a chain of Mix nodes over Image Textures -- into one buffer.
 *
 * This is the interactive half of the pair that turns a material into a single buffer; the other
 * is the bake in `ED_material_bake.hh`. Which one applies is not a user choice but a property of
 * the graph, decided by #BKE_paint_material_source_resolve and by whether
 * #BKE_paint_material_composite_stack_from_material can express the channel as a layer stack:
 *
 * - A plain stack of image layers composites here, on the CPU, in milliseconds, and can be
 *   refreshed for a single rectangle. That is what makes a stroke show its own result on the
 *   composite canvas within a frame.
 * - Anything else -- procedural nodes, math, ramps -- has to go through the bake, which is
 *   general but costs a shader compile and an EEVEE job, and can only ever be recomputed whole.
 *
 * The two must never both be considered authoritative for the same channel: whenever a stack is
 * derived here the composite is the answer, and the bake is not consulted.
 *
 * Free of GPU and of #Object, like the resolver it builds on.
 */

#include <cstdint>
#include <vector>

#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_paint_material_enums.hh"

#include "DNA_scene_types.h"

namespace blender {

struct ImBuf;
struct Image;
struct Main;
struct bNode;
struct bNodeTree;
struct ImageUser;
struct Material;
struct rcti;

/**
 * One correction of a layer, as data-blocks: an image blended onto the layer's colour (a content
 * section) or onto its coverage (a mask one), below the layer's own blend.
 *
 * A correction has no buffer of its own outside the channel it was built for, so a stack derived
 * for one channel carries that channel's corrections only -- a correction with no map in this
 * channel is Absent here: #image null, and the evaluator skips it.
 */
struct PaintMaterialCompositeCorrection {
  Image *image = nullptr; /* null: Absent in this channel -> skipped */
  /**
   * The correction reads a MESH_MAP atlas rather than a painted map.
   *
   * A geometry map may be baked at another resolution than the channel's painted maps, so the CPU
   * resamples it onto the reference grid instead of rejecting the stack; #mesh_map_scalar makes a
   * scalar atlas (AO, Curvature, Edge) contribute its R.
   */
  bool mesh_map = false;
  bool mesh_map_scalar = false;
  /** A MESH_MAP mask item reads the atlas R as its coverage; a Paint mask reads the mean. */
  bool mesh_map_mask_reads_red = false;
  /**
   * The correction's own map node's #ImageUser, or null. Owned by the material: copy it before
   * acquiring a buffer, since acquisition writes to it.
   */
  const ImageUser *iuser = nullptr;
  /** A Fill-effect correction's flat colour, when it carries no map; see
   * #PaintMaterialCompositeImageLayer.constant_color. */
  float constant_color[4] = {};
  bool has_constant_color = false;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  /** On in this channel: the row is on and its coverage here is not the switched-off form. */
  bool enabled = true;
  /**
   * The row itself is on (its Mix is not muted), whatever this channel's coverage says. A stack
   * derived for another channel by tag (AO) starts from this rather than from #enabled, since the
   * reference channel's coverage says nothing about the other channel's map.
   */
  bool row_enabled = true;
  /** The correction's identity, shared by every channel's nodes for it (spec 18 AO: map lookup by
   * tag). */
  bUUID marker = {};
};

/** One layer of a stack, as data-blocks. This is what a material resolves to. */
struct PaintMaterialCompositeImageLayer {
  Image *color_image = nullptr;
  /**
   * The image is a MESH_MAP atlas rather than one of the channel's painted maps.
   *
   * A geometry map may be baked at another resolution than the channel's painted maps; the CPU
   * composite resamples it onto the channel's reference grid (#resampled_color) rather than
   * rejecting the stack, matching the graph's filtered UV sampling.
   */
  bool is_mesh_map = false;
  /** A scalar atlas (AO, Curvature, Edge) contributes its R spread across the RGB. */
  bool is_mesh_map_scalar = false;
  /**
   * The image node's own #ImageUser, or null. Owned by the material: copy it before acquiring a
   * buffer, since acquisition writes to it.
   */
  const ImageUser *color_iuser = nullptr;
  /** Modulates #opacity per pixel. Null when the layer's factor is a plain value. */
  Image *mask_image = nullptr;
  const ImageUser *mask_iuser = nullptr;
  /**
   * The flat colour the layer paints with when it has no map, in the channel's own space.
   *
   * A Fill layer and a channel with no map both reduce to a constant; carrying it here rather than
   * manufacturing a one-pixel image keeps the CPU path free of sampling and gives masks and
   * corrections (phase 2) the same constant layer to stand on.
   */
  float constant_color[4] = {};
  bool has_constant_color = false;
  /**
   * The mask is the image's alpha rather than its colour.
   *
   * Which output of the Image Texture the factor was taken from, and not a detail: a layer stack
   * routes the layer's own alpha into the factor, and reading its colour there instead would
   * modulate every layer by its own brightness.
   */
  bool mask_from_alpha = false;
  /**
   * With #mask_from_alpha, read #mask_image's value as its grey (the mean of RGB) rather than its
   * alpha. Only the value read changes, not how the factor is built: a layer mask is painted black
   * and white, and a brush writes colour, never alpha.
   */
  bool mask_reads_grey = false;
  /**
   * The layer's own map alpha multiplies into the factor base, so a texel the map leaves
   * transparent shows what is below instead of covering it with the map's (black) color. Set for a
   * plain Paint layer's channel map; a mask or a baked map keeps its own coverage.
   */
  bool color_alpha_coverage = false;
  /**
   * Whether the generated chain tracks a content alpha for this row (F2-C1/F2-C5).
   *
   * True for a Paint/Fill/Custom row on a channel whose generated chain carries one, or a folder of
   * such rows; false for a Material row and for the Normal channel, whose fourth component is the
   * colour chain's own. When set, the evaluator lays the content corrections over this alpha --
   * `a = a + fac * (1 - a)` -- instead of over the coverage, so a partially transparent row stays
   * partially transparent under a correction. See #BKE_paint_material_channel_tracks_content_alpha.
   */
  bool tracks_content_alpha = false;
  /**
   * A second coverage multiplied into the factor base with the mask, read as its grey: a Material
   * layer's source transparency, baked into a map. Kept apart from #mask_image so the user's own
   * mask stays live on top of it and editing that mask never re-bakes the source.
   */
  Image *coverage_image = nullptr;
  /** The coverage map's own #ImageUser, or null (the baked coverage and plain maps use null). */
  const ImageUser *coverage_iuser = nullptr;
  /**
   * A flat coverage used instead of #coverage_image when the source's alpha is live: the same
   * constant the generator builds for the active Material row's factor.
   */
  float coverage_constant = 1.0f;
  bool has_coverage_constant = false;
  /**
   * The coverage is read as #coverage_image's alpha rather than its grey. Set for a live source
   * alpha map: the generator reads that map's Alpha output as the factor.
   */
  bool coverage_from_alpha = false;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  /** How much of #mask_image applies; 0 ignores the mask entirely. */
  float mask_influence = 1.0f;
  bool enabled = true;
  /**
   * The row's identity, read from the marker its Mix node carries.
   *
   * Carried separately from #color_image because the image can be tagged with anything the user
   * chose, while the marker is the stable name a later reader -- the mask baker -- looks the row up
   * by to find the layer whose coverage it has to compute. Nil for a bare base, which has no Mix
   * node to carry one.
   */
  bUUID marker = {};
  /**
   * The layer is a bare Image Texture wired straight into the channel, not a blended layer.
   *
   * Such a bottom is copied rather than blended, because it has no Mix node and therefore no blend
   * mode, opacity or factor of its own. A uniform chain has none of these: its lowest layer blends
   * over transparency like every other layer, and is composited the same way.
   */
  bool is_bare_base = false;
  /**
   * The corrections blended onto the layer's colour, bottom to top (spec 18 §4.5). May carry the
   * layer on their own, which is why #color_image may be null: a layer Absent in this channel
   * still paints through the content corrections it holds.
   */
  Vector<PaintMaterialCompositeCorrection> content_corrections;
  /** The corrections blended onto the layer's coverage, bottom to top. */
  Vector<PaintMaterialCompositeCorrection> mask_corrections;
  /**
   * A folder: #children are composited in isolation over transparency, and the result is laid over
   * what is below with the row's own blend, opacity and mask (design §5). A folder carries no maps
   * of its own; its participation in a channel is the union of its children's.
   */
  bool is_folder = false;
  /* std::vector, not blender::Vector: a self-referential member needs a container that accepts an
   * incomplete type, which is exactly the case std::vector allows for. */
  std::vector<PaintMaterialCompositeImageLayer> children;
};

/** One correction of a layer, as buffers. This is what the evaluator reads. */
struct PaintMaterialCompositeCorrectionBuffer {
  ImBuf *ibuf = nullptr;
  /**
   * Name of #ibuf's buffer colorspace, resolved once at acquisition, or null for scene linear.
   *
   * Taken from the buffer the evaluator actually reads -- `byte_buffer.colorspace` for a byte map,
   * `float_buffer.colorspace` for a float one -- never from the #Image setting, which can disagree
   * with the buffer that was handed over.
   */
  const char *colorspace_name = nullptr;
  /** See #PaintMaterialCompositeCorrection.mesh_map / .mesh_map_scalar. */
  bool is_mesh_map = false;
  bool is_mesh_map_scalar = false;
  bool mesh_map_mask_reads_red = false;
  float constant_color[4] = {};
  bool has_constant_color = false;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  bool enabled = true;
};

/** One layer of a stack, as buffers. This is what the evaluator reads. */
struct PaintMaterialCompositeLayer {
  /**
   * Byte or float RGBA, matching the stack dimensions. Null when the layer is Absent in this
   * channel and its content corrections are what it paints with.
   */
  ImBuf *color_ibuf = nullptr;
  ImBuf *mask_ibuf = nullptr;
  /** See #PaintMaterialCompositeImageLayer.is_mesh_map. */
  bool is_mesh_map = false;
  bool is_mesh_map_scalar = false;
  /** Colorspace names of #color_ibuf / #mask_ibuf's buffers; see
   * #PaintMaterialCompositeCorrectionBuffer.colorspace_name. */
  const char *color_colorspace_name = nullptr;
  const char *mask_colorspace_name = nullptr;
  /** See #PaintMaterialCompositeImageLayer.constant_color. */
  float constant_color[4] = {};
  bool has_constant_color = false;
  /** See #PaintMaterialCompositeImageLayer.mask_from_alpha. */
  bool mask_from_alpha = false;
  /** See #PaintMaterialCompositeImageLayer.mask_reads_grey. */
  bool mask_reads_grey = false;
  /** See #PaintMaterialCompositeImageLayer.color_alpha_coverage. */
  bool color_alpha_coverage = false;
  /** See #PaintMaterialCompositeImageLayer.tracks_content_alpha. */
  bool tracks_content_alpha = false;
  /** See #PaintMaterialCompositeImageLayer.coverage_image; with its buffer's colorspace. */
  ImBuf *coverage_ibuf = nullptr;
  const char *coverage_colorspace_name = nullptr;
  /** See #PaintMaterialCompositeImageLayer.has_coverage_constant / .coverage_constant. */
  float coverage_constant = 1.0f;
  bool has_coverage_constant = false;
  /** See #PaintMaterialCompositeImageLayer.coverage_from_alpha. */
  bool coverage_from_alpha = false;
  /** See #PaintMaterialCompositeImageLayer.coverage_iuser. */
  const ImageUser *coverage_iuser = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  float mask_influence = 1.0f;
  bool enabled = true;
  /** See #PaintMaterialCompositeImageLayer.is_bare_base. */
  bool is_bare_base = false;
  /** The corrections blended onto the colour, bottom to top; see
   * #PaintMaterialCompositeImageLayer.content_corrections. */
  Vector<PaintMaterialCompositeCorrectionBuffer> content_corrections;
  /** The corrections blended onto the coverage, bottom to top. */
  Vector<PaintMaterialCompositeCorrectionBuffer> mask_corrections;
  /** See #PaintMaterialCompositeImageLayer.is_folder. */
  bool is_folder = false;
  std::vector<PaintMaterialCompositeLayer> children;
};

/** Layers bottom to top: index 0 is composited first and everything else lands on top of it. */
struct PaintMaterialCompositeStack {
  Vector<PaintMaterialCompositeLayer> layers;
  int width = 0;
  int height = 0;
  /**
   * The value the composite starts from, before the first row: #BKE_paint_layers_channel_bottom_color
   * for a description-driven stack. When #has_bottom_color is false the buffer starts transparent,
   * the graph-as-truth reading. See #PaintMaterialCompositeImageLayer.constant_color.
   */
  float bottom_color[4] = {};
  bool has_bottom_color = false;
};

struct PaintMaterialCompositeEvalStats {
  double elapsed_seconds = 0.0;
  int layers_evaluated = 0;
  int64_t pixels_processed = 0;
};

/**
 * Whether the generated chain of \a channel tracks a content alpha (F2-C1): every channel the image
 * canvas can resolve a map for, except Normal, whose row blend is a three-component normal combine.
 * The CPU stack builder and the generator both gate on this, so they agree on which rows carry a
 * content alpha and which leave their fourth component to the colour chain.
 */
bool BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel channel);

/**
 * Composite \a stack bottom to top into \a composite_ibuf, which must be byte RGBA of the stack's
 * dimensions.
 *
 * \param region: when given, only this rectangle is recomputed and the rest of \a composite_ibuf
 *                is left as it was. Clipped to the buffer. This is the whole point of compositing
 *                on the CPU: a dab touches a few thousand pixels, and refreshing only those keeps
 *                the cost of a stroke independent of the canvas resolution.
 * \return false when the stack is empty, dimensions disagree, or a buffer is unusable.
 */
bool BKE_paint_material_composite_eval(const PaintMaterialCompositeStack &stack,
                                       ImBuf *composite_ibuf,
                                       const rcti *region = nullptr,
                                       PaintMaterialCompositeEvalStats *r_stats = nullptr);

/**
 * The one implementation of the stack's formulas: composite \a stack into \a dst as straight
 * scene-linear float RGBA, `width * height * 4` values.
 *
 * Every source sample is first decoded out of its own buffer's colorspace (see the layers'
 * `*_colorspace_name`), then the rows are mixed in scene linear the way the shader mixes them; a
 * premultiplied float buffer is straightened before it is read. No encoding happens here -- that is
 * #BKE_paint_material_composite_eval's job, and the bake's when it writes a map.
 *
 * \param region: when given, only this rectangle is written and the rest of \a dst is left as it
 *                was. Same contract as the byte evaluation.
 */
bool BKE_paint_material_composite_eval_linear(const PaintMaterialCompositeStack &stack,
                                              float *dst,
                                              const rcti *region = nullptr,
                                              PaintMaterialCompositeEvalStats *r_stats = nullptr);

/**
 * Acquire every layer's buffer, evaluate, and release the locks again.
 *
 * The layer images are acquired for the duration of the evaluation only. Nothing is written back
 * to them, so a caller holding no lock of its own is safe here.
 */
bool BKE_paint_material_composite_eval_images(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    ImBuf *composite_ibuf,
    const rcti *region = nullptr,
    PaintMaterialCompositeEvalStats *r_stats = nullptr,
    const float bottom_color[4] = nullptr);

/**
 * Acquire the layer buffers, evaluate through #BKE_paint_material_composite_eval_linear, and
 * release them: the same call as #BKE_paint_material_composite_eval_images, but leaving the result
 * as scene-linear floats for a caller that will encode it itself (a bake, an export, a test that
 * compares against the shader's own linear space).
 */
bool BKE_paint_material_composite_eval_images_linear(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    float *dst,
    const rcti *region = nullptr,
    PaintMaterialCompositeEvalStats *r_stats = nullptr,
    const float bottom_color[4] = nullptr);

/**
 * Compute \a row_marker's mask factor for every pixel of \a region into \a dst_ibuf's RGB
 * (alpha 1), using the same math as the composite.
 *
 * The row is the layer of \a image_layers whose #PaintMaterialCompositeImageLayer::marker equals
 * \a row_marker. \a dst_ibuf must be byte RGBA of the stack's dimensions. This is what the mask
 * baker writes into B: the per-pixel coverage the row's mask-correction chain produces, which the
 * CPU composite applies as the layer's blend factor. Sharing the computation is what keeps B and
 * the composite from drifting apart.
 *
 * \param region: when given, only this rectangle is written and the rest of \a dst_ibuf is left as
 *                it was. Clipped to the buffer.
 * \return false when the row is not in the stack, or a buffer is unusable.
 */
bool BKE_paint_material_composite_eval_row_mask(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const bUUID &row_marker,
    ImBuf *dst_ibuf,
    const rcti *region = nullptr);

/**
 * The content and coverage of the row \a row_marker, the pair a bake of that row stores: the
 * straight scene-linear colour before its own blend over what is below, and the factor its Mix
 * would take (`opacity x mask x corrections` for a leaf, the folder's own opacity/mask times its
 * children's accumulated coverage for a folder).
 *
 * \a r_color_rgba and \a r_coverage_gray are sized to the evaluated rectangle: the stack's
 * `width * height * 4` and `width * height` values when \a region is null, otherwise the region's
 * `w * h * 4` and `w * h` values. This is the one place the isolated-group formula is asked for a
 * row's content, shared by the bake planner and #BKE_paint_layers_bake_render_node.
 *
 * \param region: when given, only this rectangle of the row is rendered and the outputs describe it
 *                alone, so a partial re-bake does not composite pixels it will not write. The
 *                region is clipped to the stack; an empty intersection produces no output and
 *                returns false.
 */
bool BKE_paint_material_composite_eval_row_content(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const bUUID &row_marker,
    float *r_color_rgba,
    float *r_coverage_gray,
    const rcti *region = nullptr);

/**
 * Dimensions of the composite, taken from the bottom-most enabled layer.
 *
 * Layers of differing sizes are not composited: a stack whose layers disagree is rejected by the
 * evaluator rather than resampled, since a resample would silently change the pixels a stroke is
 * about to be compared against.
 *
 * \return false when no enabled layer has a readable buffer.
 */
bool BKE_paint_material_composite_stack_dimensions(
    Span<PaintMaterialCompositeImageLayer> image_layers, int &r_width, int &r_height);

/**
 * The shader node group that lays one tangent-space normal map over another, created on demand.
 *
 * Exists so that a normal stack can be *authored* in a way the compositor is able to reproduce.
 * A plain Mix node interpolates encoded normals, which is a legitimate "replace" but flattens
 * relief when it is meant to accumulate; the correct operation is #CompositeBlend::NormalCombine,
 * and nothing in a stock node graph expresses it. Rather than guess that some Mix node "really
 * means" a normal combine -- which would make the composite disagree with the render -- the
 * engine offers this group and recognizes it again by an ID property, so the two agree by
 * construction whichever of them a viewer is looking at.
 *
 * Its inputs are `A` (below), `B` (the layer) and `Factor`; its output is `Result`. The group is
 * shared: a second call returns the one already in \a bmain.
 */
bNodeTree *BKE_paint_material_normal_combine_group_ensure(Main &bmain);

/** Whether \a node is an instance of #BKE_paint_material_normal_combine_group_ensure's group. */
bool BKE_paint_material_is_normal_combine_group(const bNode &node);

/**
 * The passes a layered material offers, in the order they are listed to the user.
 *
 * Values are #eMaterialPaintChannel plus #PAINT_LAYER_MAP_MASK. A fixed list rather than one
 * derived from the material: a pass is something the material *can* have, and the user picks it
 * before the maps behind it necessarily exist. Whether a given pass resolves to anything is
 * answered by #BKE_paint_material_composite_stack_from_material.
 */
Span<int> BKE_paint_material_composite_passes();

/**
 * The passes the Image Editor offers as *display* modes, in the order they are listed.
 *
 * #BKE_paint_material_composite_passes plus #PAINT_LAYER_PASS_COMBINED at the front. A second list
 * rather than a widened one because every consumer of the role list treats its values as roles --
 * indexing a per-channel array, looking the value up in the channel descriptor table, resolving a
 * layer map -- and none of that is meaningful for a display mode. Only code that populates a
 * selector or cycles the shown pass should use this one.
 *
 * The two lists are kept in sync by hand and a test asserts that they agree.
 */
Span<int> BKE_paint_material_display_passes();

/**
 * Hash of everything about \a image_layers that changes the composited pixels except the pixels
 * themselves -- which images, in which order, with which blend, opacity and mask, the layers'
 * corrections included.
 *
 * Image *contents* are deliberately not in here; there is no content version to hash. An edit to a
 * layer's pixels is found instead by #BKE_paint_material_composite_cache_ensure, which polls each
 * source image's partial-update log.
 */
uint64_t BKE_paint_material_composite_stack_hash(
    Span<PaintMaterialCompositeImageLayer> image_layers);

/**
 * The composite of \a channel of \a ma, recomputing whatever part of it is out of date.
 *
 * Synchronous: unlike a bake this is a few milliseconds, and a whole-buffer rebuild only happens
 * when the stack itself changed. A stroke's own edits are refreshed as rectangles.
 *
 * Pixel changes are detected, not reported: the cache subscribes to each source image's
 * partial-update log and polls it here. A caller that edits a layer's pixels therefore has nothing
 * to tell this cache, and -- the reason it works that way -- a caller that tags the image ID for
 * unrelated reasons can no longer turn a known rectangle into "the whole canvas".
 * #BKE_paint_material_composite_cache_invalidate remains for the other kind of change: the stack's
 * own description.
 *
 * The returned buffer is owned by the cache and stays valid until the next call for a different
 * material or a #BKE_paint_material_composite_cache_free_all. Do not free it.
 *
 * \param r_revision: incremented every time the pixels are recomputed, so a consumer that uploads
 *                    them somewhere -- the image editor's GPU texture -- can tell whether its copy
 *                    is still current without comparing buffers. Never zero for a live composite.
 * \param r_changed_region: the rectangle recomputed by this call, the whole buffer when it was
 *                          rebuilt entirely, and empty when nothing was. A consumer that derives
 *                          its own pixels from this composite -- the Combined preview -- needs it
 *                          to keep its own refresh proportional to the edit.
 * \return null when the stack cannot be composited, which is the caller's cue to fall back.
 */
ImBuf *BKE_paint_material_composite_cache_ensure(
    const Material &ma,
    eMaterialPaintChannel channel,
    Span<PaintMaterialCompositeImageLayer> image_layers,
    uint64_t stack_hash,
    uint64_t *r_revision = nullptr,
    PaintMaterialCompositeEvalStats *r_stats = nullptr,
    rcti *r_changed_region = nullptr);

/**
 * Mark the whole composite of \a ma out of date, or of every material when \a ma is null.
 *
 * Marks rather than drops, for the same reason the bake cache does: a caller asking for the buffer
 * in between must get the previous pixels rather than nothing.
 *
 * Called wherever the material itself changed -- its node tree was edited -- and, with a null
 * material, after an undo step that may have moved pixels under the cache without any of the
 * per-image tags being able to report it.
 */
void BKE_paint_material_composite_cache_invalidate(const Material *ma);

/**
 * Drop every cached composite of \a ma, freeing its buffers.
 *
 * Called when the material is freed. The cache is keyed on #ID.session_uid, so an entry left
 * behind would never be looked up again and would hold its buffer until the budget happened to
 * evict it; this is what a cache stored on the material itself would have got for free, and the
 * price of keeping it in one place instead -- see the note on #CompositeCache.
 */
void BKE_paint_material_composite_cache_free_material(const Material &ma);

/** Drop every cached composite. For teardown and for file load. */
void BKE_paint_material_composite_cache_free_all();

/** Whether a composite of \a channel of \a ma is currently cached. Exists for tests. */
bool BKE_paint_material_composite_cache_contains(const Material &ma,
                                                 eMaterialPaintChannel channel);

}  // namespace blender
