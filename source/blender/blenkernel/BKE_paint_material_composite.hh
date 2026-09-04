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
#include <optional>
#include <string>

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

#include "RNA_types.hh"

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
 * How a layer combines with what is below it.
 *
 * Deliberately a short list: it is the set of Mix node blend modes that
 * #BKE_paint_material_composite_stack_from_material knows how to reproduce byte-exactly. A chain
 * using any other mode is not expressible as a stack and falls to the bake instead of being
 * approximated here.
 */
enum class CompositeBlend : int8_t {
  Mix = 0,
  Multiply,
  Overlay,
  Add,
  /**
   * Combine two tangent-space normal maps, rather than blending their encoded bytes.
   *
   * Encoded normals are not colours: averaging two of them channel by channel flattens the
   * relief instead of laying one over the other, which is why every layer stack that supports
   * normals has an operation of its own for it. This is the whiteout blend -- the detail map's
   * slope added to the base map's, renormalized -- which is what "overlay"/"add" means for a
   * normal layer.
   *
   * Nothing in a plain Mix chain selects this: a Mix node really does interpolate the encoded
   * values, and reproducing it any other way would make the composite disagree with the render.
   * It exists for the graph shapes that genuinely combine normals.
   */
  NormalCombine,
};

/** One layer of a stack, as data-blocks. This is what a material resolves to. */
struct PaintMaterialCompositeImageLayer {
  Image *color_image = nullptr;
  /**
   * The image node's own #ImageUser, or null. Owned by the material: copy it before acquiring a
   * buffer, since acquisition writes to it.
   */
  const ImageUser *color_iuser = nullptr;
  /** Modulates #opacity per pixel. Null when the layer's factor is a plain value. */
  Image *mask_image = nullptr;
  const ImageUser *mask_iuser = nullptr;
  /**
   * The mask is the image's alpha rather than its colour.
   *
   * Which output of the Image Texture the factor was taken from, and not a detail: a layer stack
   * routes the layer's own alpha into the factor, and reading its colour there instead would
   * modulate every layer by its own brightness.
   */
  bool mask_from_alpha = false;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  /** How much of #mask_image applies; 0 ignores the mask entirely. */
  float mask_influence = 1.0f;
  bool enabled = true;
  /**
   * The layer is a bare Image Texture wired straight into the channel, not a blended layer.
   *
   * Such a bottom is copied rather than blended, because it has no Mix node and therefore no blend
   * mode, opacity or factor of its own. A uniform chain has none of these: its lowest layer blends
   * over transparency like every other layer, and is composited the same way.
   */
  bool is_bare_base = false;
};

/**
 * Where the ordinals of layers held inside a group start.
 *
 * Rows at the top level are numbered by their position in the channel chain, so an ordinal from
 * the UI names the same layer to the graph editor. Rows inside a group have no position in that
 * chain at all, so they are numbered from a range of their own: the number stays unique for the
 * tree store, and an ordinal at or above this base is recognizable as "inside a group" by code
 * that can only act on the chain.
 */
constexpr int PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE = 1024;

/**
 * One paint layer as it appears in a UI. Unlike #PaintMaterialCompositeImageLayer, this preserves
 * the node identity and editable socket while remaining independent from the pixel evaluator.
 */
struct PaintMaterialLayerStackEntry {
  int16_t ordinal = 0;
  int32_t node_id = 0;
  const bNodeTree *owner_tree = nullptr;
  uint32_t material_sid = 0;
  uint32_t tree_sid = 0;
  int depth = 0;
  int32_t parent_node_id = 0;
  bool is_group = false;
  /** The row is a bare Image Texture wired straight into the channel, not a blended layer. */
  bool is_bare_base = false;
  /**
   * The layer's identity, nil for a bare base (it has no Mix node to carry one). Stable across an
   * edit that moves the row -- unlike #ordinal, which is only ever a position -- so a caller that
   * has to recognize the same row again after one, such as the Outliner keeping it open or
   * selected, can key off this instead.
   */
  bUUID marker = {};
  std::string name;
  /**
   * The layer's own name as editable storage, or null when the row has none.
   *
   * #name is what the row reads as, which falls back to the layer's map or its node when the user
   * has set no name; this is the #bNode::label that name is set through, handed out so that a UI
   * text field can type straight into it rather than into a copy that the next rebuild discards.
   * Null for a bare base, which has no Mix node to carry a label, and for an unsupported row.
   */
  char *label = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  bool enabled = true;
  bool has_mask = false;
  bool supported = true;
  const char *unsupported_reason = nullptr;
  std::optional<PointerRNA> factor_prop;
  std::optional<PointerRNA> blend_prop;
  Map<int, Image *> channel_images;
};

/** One layer of a stack, as buffers. This is what the evaluator reads. */
struct PaintMaterialCompositeLayer {
  /** Byte or float RGBA, matching the stack dimensions. */
  ImBuf *color_ibuf = nullptr;
  ImBuf *mask_ibuf = nullptr;
  /** See #PaintMaterialCompositeImageLayer.mask_from_alpha. */
  bool mask_from_alpha = false;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  float mask_influence = 1.0f;
  bool enabled = true;
  /** See #PaintMaterialCompositeImageLayer.is_bare_base. */
  bool is_bare_base = false;
};

/** Layers bottom to top: index 0 is composited first and everything else lands on top of it. */
struct PaintMaterialCompositeStack {
  Vector<PaintMaterialCompositeLayer> layers;
  int width = 0;
  int height = 0;
};

struct PaintMaterialCompositeEvalStats {
  double elapsed_seconds = 0.0;
  int layers_evaluated = 0;
  int64_t pixels_processed = 0;
};

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
 * Acquire every layer's buffer, evaluate, and release the locks again.
 *
 * The layer images are acquired for the duration of the evaluation only. Nothing is written back
 * to them, so a caller holding no lock of its own is safe here.
 */
bool BKE_paint_material_composite_eval_images(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    ImBuf *composite_ibuf,
    const rcti *region = nullptr,
    PaintMaterialCompositeEvalStats *r_stats = nullptr);

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
 * The layer stack \a channel of \a ma is built from, or false when it is not a layer stack.
 *
 * Walks the same graph, through the same reroutes, muted nodes and group instances, as
 * #BKE_paint_material_source_resolve: the chain of Mix nodes feeding the channel's Principled
 * input, each mixing an Image Texture over what is below it. A single Image Texture is a stack of
 * one. Anything the walk does not recognize -- an unsupported blend mode, a factor that is neither
 * a value nor an image, a node that is not a Mix -- makes the whole channel non-compositable, and
 * the caller falls back to the bake.
 *
 * Unlike the bake, this needs no group path: it only reads data-block pointers out of the nodes it
 * finds, and never has to route a socket back out to the root tree.
 *
 * A channel with no graph to walk -- Ambient Occlusion has no Principled input at all, and
 * #PAINT_LAYER_MAP_MASK is not a channel -- is instead assembled from the layers themselves: the
 * order, blending and masking come from the channel that does have a chain, and each layer's map
 * for \a channel is found by #Image.paint_layer_id and #Image.paint_layer_channel. That is the
 * whole reason those two fields exist; a user who bakes an AO map per layer has no node link that
 * could express the same thing.
 *
 * Cheap enough for a redraw, like the resolver: it allocates only \a r_layers and touches no
 * pixels.
 */
bool BKE_paint_material_composite_stack_from_material(
    const Main &bmain,
    const Material &ma,
    int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers);

/**
 * Build the paint-layer UI model from all channels of \a ma's node graph, bottom to top.
 *
 * This is a structure-only traversal: it does not acquire image buffers and is not a conversion of
 * the evaluator's model, which intentionally lacks node and tree identity.
 */
bool BKE_paint_material_layer_stack_from_material(
    const Main &bmain, const Material &ma, Vector<PaintMaterialLayerStackEntry> &r_entries);

/**
 * Whether \a ma resolves to a paint layer stack at all.
 *
 * Answers the same question as #BKE_paint_material_layer_stack_from_material without aggregating
 * the channels or reaching into #Main, for callers that only need to decide whether to list the
 * material -- the Outliner object overview asks this once per object on every rebuild.
 */
bool BKE_paint_material_has_layer_stack(const Material &ma);

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
 * Whether \a node is a paint layer group: a folder of layers, composited on transparency.
 *
 * Such a group is an ordinary layer as far as the stack around it is concerned -- its `Result` is
 * the top socket of the Mix node above it and its `Alpha` that node's factor -- and a sub-stack as
 * far as the stack inside it is concerned. See `08 §2.2`.
 */
bool BKE_paint_material_is_layer_group(const bNode &node);

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
 * The maps of the paint layer \a layer_id, indexed by role (channel, or #PAINT_LAYER_MAP_MASK).
 *
 * A channel wired into \a ma is answered from its stack: the layer is already identified there, so
 * its map for that channel needs no #Image.paint_layer_channel tag and works for any material a
 * stroke can paint. The tag answers the rest -- Ambient Occlusion and the mask, which no node link
 * mentions.
 *
 * Entries the layer has no map for stay null, which is what lets the canvas list show a channel
 * the active layer does not author yet without pretending it is selectable.
 */
void BKE_paint_material_layer_maps_get(const Main &bmain,
                                       const Material &ma,
                                       const bUUID &layer_id,
                                       MutableSpan<Image *> r_maps);

/**
 * Hash of everything about \a image_layers that changes the composited pixels except the pixels
 * themselves -- which images, in which order, with which blend, opacity and mask.
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
