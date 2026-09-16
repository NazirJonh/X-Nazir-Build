/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The chain-reading half of #BKE_paint_material_layer_edit.hh, shared by every mutator in this
 * module: implemented in `paint_material_layer_chain.cc`, called from
 * `paint_material_layer_edit.cc`, `paint_material_layer_props.cc` and
 * `paint_material_layer_channels.cc`.
 *
 * Every mutation here works the same way -- collect each channel's chain (or forest of chains,
 * for a graph with groups), build a #LayerEditPlan that answers every precondition up front, then
 * rebuild the "what is below me" links from the plan. This header is what lets that shape live in
 * one file while the operations that use it live in four: none of it is specific to any one
 * operation, and duplicating it per file would be the second truth this whole module exists to
 * avoid in the node graph itself.
 */

#include "BKE_paint_material_layer_edit.hh"

#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {

struct Image;
struct Main;
struct Material;
struct CompositeMixNode;
struct bNode;
struct bNodeLink;
struct bNodeSocket;
struct bNodeTree;
enum eNodeSocketInOut : short;

/** The nodes one correction owns in one channel. */
struct ChainCorrection {
  bUUID marker = {};
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  PaintMaterialCorrectionEffect effect = PaintMaterialCorrectionEffect::Paint;
  bNode *mix = nullptr;             /* CorrMix / MCorrMix, or the Normal Combine instance */
  bNode *factor_multiply = nullptr; /* coverage x opacity */
  bNode *over_invert = nullptr;     /* Content only */
  bNode *over_combine = nullptr;    /* Content only */
  bNode *map = nullptr;             /* Image Texture, null when Absent */
  Image *image = nullptr;
};

/** One rung of a channel's chain. The bottom rung is a bare Image Texture and has no sockets. */
struct ChainLayer {
  bNode *node = nullptr;
  bNodeSocket *bottom = nullptr;
  bNodeSocket *top = nullptr;
  /** The socket that feeds whatever sits above this layer. */
  bNodeSocket *output = nullptr;
  /** What modulates this layer: its map's Alpha, a mask, or a constant. */
  bNodeSocket *factor = nullptr;
  /** True when this layer blends a layer group rather than a map. */
  bool is_group = false;
  /** For a group layer, which chain of the forest holds its sub-stack; -1 otherwise. */
  int sub_chain_index = -1;
  Image *image = nullptr;
  /**
   * The corrections hanging on this layer's content input, bottom to top (spec 18 §4.5). Empty
   * when the layer has none, and for a group layer, whose content stays inside its folder.
   */
  Vector<ChainCorrection> content_corrections;
  /** The corrections hanging on this layer's coverage input, bottom to top. */
  Vector<ChainCorrection> mask_corrections;
  /**
   * The Image Texture of the layer's own map, read below the content corrections; null when the
   * layer is Absent in this channel, and for a group layer, whose content is its folder's.
   */
  bNode *base_map = nullptr;

  bool is_mix() const
  {
    return bottom != nullptr;
  }
};

struct ChannelChain {
  int channel = -1;
  bNodeTree *tree = nullptr;
  /** Where the top of the chain plugs in: a Principled input, or a Normal Map's Color. */
  bNodeSocket *terminal = nullptr;
  /**
   * The node #terminal belongs to.
   *
   * Resolved once, while the topology cache is known to be good. Relinking invalidates that cache,
   * so asking a socket for its owner in the middle of a rebuild is an assert waiting to happen.
   */
  bNode *terminal_node = nullptr;
  /** Bottom to top, so index 0 is the bare image and index i is layer i. */
  Vector<ChainLayer> layers;
  /** How many of #layers blend a group. */
  int group_num = 0;
  /** 0 at the top level, one more inside each group. */
  int nesting = 0;
};

/** Where a row lives: which chain of one channel's forest, and which position in it. */
struct ForestPosition {
  int chain_index = -1;
  int layer_index = -1;
};

/** One channel's group instance node for an #Add whose anchor is an empty folder. */
struct AddEmptyGroupInstance {
  bNode *instance = nullptr;
  int channel = -1;
};

/** The nodes one channel contributes to a layer being added. */
struct NewLayerNodes {
  int channel = -1;
  bNode *tex = nullptr;
  /** Null only for the bottom layer of a stack being created from nothing. */
  bNode *mix = nullptr;
  Image *image = nullptr;
  /**
   * Whether #image was created by this transaction. A map the caller handed over through
   * #PaintMaterialLayerAddParams::channel_images stays the caller's when the add is refused.
   */
  bool owns_image = true;
};

/** The operations a plan can be built for: one builder, one order of checks. */
enum class LayerEditOp {
  Add,
  Remove,
  Move,
  Reorder,
  Rename,
  SetEnabled,
  Duplicate,
  MaskAdd,
  MaskRemove,
  ChannelImageSet,
  FillColorSet,
  KindSet,
  GroupMake,
  GroupAdd,
  Ungroup,
  /** Add, remove, reorder, rename or toggle one correction of the row at the ordinal. */
  CorrectionEdit,
};

/**
 * Everything a mutator needs to know about the graph, collected without writing a byte to it.
 *
 * Building the plan is the only place that decides whether an edit is possible at all: every
 * refusal a graph can give -- rights, shape, ordinal, nesting -- is answered here, and a mutator
 * that got its plan runs to the end. The checks are made against the graph *as it is*: an edit
 * refused for its shape must not leave the shape conversion behind that the old prologue would
 * already have applied.
 *
 * The chains it holds point into #per_channel and are only valid while that forest is.
 */
struct LayerEditPlan {
  /** Every wired channel's forest: sub-stacks first, the channel's top-level chain last. */
  Vector<Vector<ChannelChain>> per_channel;
  /** The chains the operation's primary row lives in, one per channel. */
  Vector<ChannelChain *> chains;
  /** The primary row's position inside #chains; for #Add, the position to insert at. */
  int layer_index = -1;
  /**
   * The destination of a two-row operation: #Reorder's destination, #Move's anchor (or the chains
   * its Into lands in), #GroupMake's range end, #GroupAdd's insert position.
   */
  Vector<ChannelChain *> target_chains;
  int target_index = -1;
  /** A Move::Into whose anchor group holds nothing yet: there is no chain to insert into. */
  bool target_is_empty_group = false;
  /**
   * An #Add whose anchor is an empty folder: #chains is empty, and these are the folder's
   * per-channel instance nodes the first layer's output is wired to.
   */
  Vector<AddEmptyGroupInstance> add_empty_group_instances;
  /** A bottom somewhere in the forest is a bare image: the shape conversion comes first. */
  bool needs_bottom_normalize = false;
};

/**
 * Tag \a ma edited: bump its paint-layer revision, tag its node tree for a re-evaluation
 * (\a relations when the topology itself changed, not just a value), and invalidate the caches
 * that read the graph's shape.
 */
void paint_layer_edit_committed(Main &bmain, Material &ma, const bool relations);

/** The tree the group instance \a node opens, or null when it holds none or not a node tree. */
bNodeTree *layer_group_tree_of(const bNode &node);

/** The one link arriving at \a socket, or null when there is none or more than one. */
bNodeLink *sole_link_into(bNodeSocket &socket);

/**
 * Whether anything at all feeds \a socket.
 *
 * Distinct from #sole_link_into returning null, which also means "more than one link" or "a link
 * that is not available": those are chains this file refuses to rewrite, and reading them as an
 * unlinked bottom would quietly turn a broken chain into a short one.
 */
bool socket_has_link(bNodeSocket &socket);

/** Walk \a terminal down to the bottom of the chain, filling \a r_chain. */
bool chain_collect(bNodeTree &tree,
                   bNodeSocket &terminal,
                   ChannelChain &r_chain,
                   PaintMaterialLayerEditError &r_error);

/** #chain_collect for every channel of \a ma that resolves to a plain (group-free) chain. */
bool chains_collect(Material &ma,
                      Vector<ChannelChain> &r_chains,
                      PaintMaterialLayerEditError &r_error);

bNodeSocket *socket_find_by_name(bNode &node, const eNodeSocketInOut in_out, const StringRef name);

bNodeSocket *group_result_socket(const bNode &group, const int channel);

/** #chain_collect, recursing into layer groups to build one chain per nesting level. */
bool chain_forest_collect(bNodeTree &tree,
                          bNodeSocket &terminal,
                          const int channel,
                          Vector<ChannelChain> &r_chains,
                          const int nesting,
                          PaintMaterialLayerEditError &r_error);

/**
 * Fill \a layer's correction lists and #base_map by descending its content and coverage inputs
 * (spec 18 §4.5 p.2). The layer must be a Mix layer this file reads; a group layer keeps nothing
 * to read here.
 */
bool correction_chain_read(ChainLayer &layer, PaintMaterialLayerEditError &r_error);

/** Relink \a into's link so it comes from \a from_socket on \a from_node instead. */
void relink_into(bNodeTree &tree,
                 bNodeSocket &into,
                 bNode &into_node,
                 bNode &from_node,
                 bNodeSocket &from_socket);

/** Point \a chain's terminal at its bottom layer, dropping every layer above it. */
void chain_base_apply(ChannelChain &chain);

/** Relink every layer of \a chain to the one below it, bottom to top. */
void chain_rebuild_links(ChannelChain &chain);

/** Whether every chain of \a chains has the same layer count. */
bool chains_align(Span<ChannelChain> chains, PaintMaterialLayerEditError &r_error);

/**
 * Spec 18 §4.5 p.3: the corrections of the row at \a layer_index must be the same UUID sequence
 * per section, with the same section and effect, in every chain. False with
 * #PaintMaterialLayerEditError::ChannelsDisagree when they are not; true when there is no resolved
 * row to compare (\a chains empty or \a layer_index outside it).
 */
bool layer_corrections_agree(Span<ChannelChain *> chains,
                             int layer_index,
                             PaintMaterialLayerEditError &r_error);

/** #chain_forest_collect for every channel of \a ma, one forest per channel. */
bool chains_collect_forest(Material &ma,
                           Vector<Vector<ChannelChain>> &r_per_channel,
                           PaintMaterialLayerEditError &r_error);

/**
 * The chain and position \a ordinal names, numbered exactly as the UI model numbers its rows.
 *
 * Rows of the top-level chain are numbered by position; rows inside groups continue from
 * #PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE in the order the walk meets them, which is depth first
 * with a group's children before the group itself.
 */
ForestPosition forest_resolve_ordinal(Span<ChannelChain> chains, const int ordinal);

/** #forest_resolve_ordinal, materialized into the chain pointers a mutator writes through. */
bool forest_rows_resolve(Vector<Vector<ChannelChain>> &per_channel,
                         const int ordinal,
                         Vector<ChannelChain *> &r_chains,
                         int &r_layer_index,
                         PaintMaterialLayerEditError &r_error);

/** The inverse of #forest_resolve_ordinal: the ordinal the UI model would give a row. */
int forest_ordinal_for_position(Span<ChannelChain> chains,
                                const int target_chain,
                                const int target_layer);

/**
 * Resolve #PaintMaterialLayerAddParams::anchor_ordinal to the chains a new layer goes into and
 * the index within them.
 */
bool add_anchor_resolve(Vector<Vector<ChannelChain>> &per_channel,
                        const int anchor_ordinal,
                        Vector<ChannelChain *> &r_chains,
                        int &r_insert_index,
                        bool &r_into_empty_group,
                        Vector<AddEmptyGroupInstance> &r_empty_group_instances,
                        PaintMaterialLayerEditError &r_error);

bool ordinal_is_in_chain(const int ordinal, PaintMaterialLayerEditError &r_error);

bNodeSocket *mix_output_find(bNode &node);

bNode *layer_factor_coverage_link(bNodeTree &tree,
                                  bNode &factor_node,
                                  bNodeSocket &factor,
                                  bNode &coverage_node,
                                  bNodeSocket &coverage,
                                  const float initial_opacity);

bNode *layer_factor_absent_link(bNodeTree &tree,
                                bNode &factor_node,
                                bNodeSocket &factor,
                                const float opacity);

/**
 * Accumulate coverage the way an "over" does, `a = a_below + a_layer * (1 - a_below)`, as a
 * Subtract + Multiply-Add pair. \a below_socket null means nothing below (a_below = 0).
 * \return the Multiply-Add node; \a r_output its Value output, \a r_invert when given the
 * Subtract of the pair, whose node a later edit has to know to remove the whole thing.
 */
bNode *coverage_over_link(bNodeTree &tree,
                          bNode *below_node,
                          bNodeSocket *below_socket,
                          bNode &layer_node,
                          bNodeSocket &layer_alpha,
                          bNodeSocket *&r_output,
                          bNode **r_invert = nullptr);

/**
 * The whole-row on/off gate of a content correction's over pair (spec 18 §4.1). On: the
 * Multiply-Add's A input carries the coverage Multiply's output, the Subtract's first input keeps
 * its `1 - a_below` constant. Off: the Multiply-Add's A is unlinked and explicitly zero --
 * `combine = 0*b + c = a_below` -- so the row covers nothing, and the Multiply's output rides on
 * the Subtract's first input instead. The pair is never expressed by muting its nodes: a muted
 * Math node passes its first connected input, which for the Multiply-Add is the correction's own
 * alpha. The gate link also keeps the pair discoverable from the Multiply while it is off, even
 * for a row with nothing below it; #correction_nodes_read reads it back.
 */
void correction_over_gate_set(bNodeTree &tree,
                              bNode &factor_multiply,
                              bNode &over_invert,
                              bNode &over_combine,
                              const bool enable);

/** Spec 18 §4.3: socket that consumes the layer's content coverage (the "mask base"), or null
 * when a mask image owns it and the accumulated coverage goes nowhere. */
bNodeSocket *layer_mask_base_consumer(ChainLayer &layer);

/**
 * Switch one correction's nodes in one channel on or off as a whole row (spec 18 §4.1): the mute
 * on its Mix, the gate of its over pair (#correction_over_gate_set), and the mute of its map --
 * shader localization drops a muted map, so a muted Mix with nothing below reads the explicit
 * black base instead of the map it would otherwise pass through. A content map also stays muted
 * while its channel is Disabled.
 */
void correction_row_enabled_apply(bNodeTree &tree, const ChainCorrection &nodes, bool enable);

/**
 * Bring \a layer's mask corrections in this channel to the form what the row puts into the channel
 * implies (spec 18 §4.3, I1, I2'). Where the base map is on or a content correction paints, the
 * mask chain sits on the row's coverage and each mask correction reads its map's alpha; where the
 * row puts nothing in, the chain's base and every mask correction's coverage are unlinked and
 * zero, so no mask blend can raise the coverage of a row that paints nothing. A mask has no
 * per-channel switch of its own: this is its whole per-channel state.
 *
 * This is also what owns the row's mask-bake anchor in \a channel: with mask corrections present
 * it ensures one exists (B feeds coverage, the live chain parks on the anchor); when the last one
 * is gone it takes the anchor off and points coverage back at the live chain. A row with no graph
 * (#ChainLayer::node null) is a no-op, and so is a shape this channel's graph cannot read.
 *
 * The anchor is only installed when \a ma's CPU composite can actually reproduce the row in \a
 * channel; a row it cannot flatten -- a folder whose mask corrections limit the folder as a whole,
 * a channel with an unsupported blend -- keeps its live chain instead of sampling a B nothing
 * writes.
 */
void layer_mask_corrections_sync(Main &bmain,
                                 Material &ma,
                                 bNodeTree &tree,
                                 ChainLayer &layer,
                                 int channel);

/** Whether a content correction of the row paints in this channel: it owns a map, and neither its
 * Mix nor its map is muted. Drives the I2' coverage link and the last-enabled-channel check; a
 * mask correction brings no pixels of its own and never counts. */
bool row_channel_painted_by_corrections(const ChainLayer &layer);

Image *layer_image_given(const PaintMaterialLayerAddParams &params, const int channel);

void fill_map_color_for(const int channel, const float fill_color[4], float r_color[4]);

/**
 * Overwrite every pixel of \a image with \a color, and tell the readers the pixels moved.
 *
 * \a color is in the same convention #BKE_image_add_generated takes, so a refill and a fresh map
 * agree on what a colour means.
 */
void image_fill_flat(Image &image, const float color[4]);

Image *layer_image_create(Main &bmain,
                          const int channel,
                          const PaintMaterialLayerAddParams &params);

bNodeTree *layer_group_tree_add(Main &bmain,
                                Span<ChannelChain *> chains,
                                const int from_ordinal,
                                const int to_ordinal);

/**
 * The Mix node a layer of \a channel blends with, freshly created in \a tree.
 *
 * The Normal channel is the exception the whole file makes: tangent-space maps do not blend
 * component-wise, so the engine's own group does it instead of a Mix node.
 */
bNode *layer_mix_node_create(Main &bmain, bNodeTree &tree, const int channel);

/**
 * The single map node feeding each layer of \a chain from \a from_ordinal to \a to_ordinal, or
 * empty when any layer in the range has no single map to move. Read through #ChainLayer::base_map,
 * which sits below the row's content corrections -- its top socket shows the topmost correction
 * when it has any, not its map.
 */
Vector<bNode *> chain_range_map_nodes(ChannelChain &chain,
                                      const int from_ordinal,
                                      const int to_ordinal);

/**
 * Move one channel's rows `from_ordinal + 1 .. to_ordinal` -- and the map of `from_ordinal` with
 * them -- into \a group, leaving the Mix node of `from_ordinal` behind to blend the group in. A
 * kept row that carries corrections moves whole: its map, its corrections and every node between
 * them go in as one unit, topped by a copy of the kept Mix itself (the row the corrections hang
 * on inside the folder); \a keeper_copy_marker is the identity that copy gets, minted once by the
 * caller so every channel's copy is the same row.
 *
 * Nodes are copied into the group and the originals collected in \a r_nodes_to_remove: there is
 * no "move a node to another tree" in the node API, and copying keeps the id-properties -- the
 * layer marker and a correction's among them -- which is what makes a layer inside a group still
 * the same layer.
 */
bool layer_group_fill_channel(Main &bmain,
                              bNodeTree &tree,
                              bNodeTree &group,
                              ChannelChain &chain,
                              Span<bNode *> map_nodes,
                              const int from_ordinal,
                              const int to_ordinal,
                              const bUUID &keeper_copy_marker,
                              const bool build_alpha,
                              Vector<bNode *> &r_nodes_to_remove);

/**
 * Insert a correction on top of \a section's inner stack of \a layer in \a chain, Absent in this
 * channel (no map, coverage unlinked and zero -- spec 18 §4.1a). Stamps marker/kind/section/effect
 * on the Mix node. Does not update the tree; the caller does, once, after every channel.
 */
bool correction_channel_insert(Main &bmain,
                               ChannelChain &chain,
                               ChainLayer &layer,
                               PaintMaterialCorrectionSection section,
                               PaintMaterialCorrectionEffect effect,
                               const bUUID &marker,
                               ChainCorrection &r_nodes);

/**
 * Take one correction's nodes out of one channel, relinking what was below to what was above:
 * the layer's stack reads as if the correction had never been there. For a content section that
 * also means carrying the correction's `a_below` source (the base alpha, or the over output of
 * the correction under it) over to the consumer of its accumulated-coverage output, so the Over
 * chain of the corrections left stays intact. The correction's map node goes with it when
 * nothing besides this correction reads it.
 *
 * Does not update the tree; the caller does, once, after every channel.
 */
void correction_channel_remove(Main &bmain,
                               ChannelChain &chain,
                               ChainLayer &layer,
                               const ChainCorrection &nodes);

/**
 * The map node of one channel's Multiply form switched on or off: the muted node only spares
 * the sampler, the coverage input's explicit zero -- the caller's clear -- is what makes the
 * channel contribute nothing (invariant I1). The one mute toggle a layer row's channel and a
 * correction's channel share; written once so the two cannot drift.
 */
void channel_map_mute_set(bNodeTree &tree, bNode &map, bool enable);

/**
 * The image tagged as \a channel's map of the correction carrying \a marker, found across
 * #Main's images the way the stack model finds a correction's maps -- a correction's map does
 * not need a node of its own in \a channel's graph (AO) to be its map. Null when none is.
 */
Image *correction_tagged_map_find(Main &bmain, const bUUID &marker, int channel);

/**
 * The nodes one correction's links wire together, read back from the Mix \a corr of \a node: its
 * stamped identity, the coverage Multiply feeding it, and -- for a Content section -- the
 * Subtract + Multiply-Add over pair, plus the correction's own map. False when the links are not
 * the shape #correction_channel_insert builds.
 */
bool correction_nodes_read(const bNode &node,
                           const CompositeMixNode &corr,
                           ChainCorrection &r_nodes);

/**
 * Every node the row \a layer owns in one channel: its Mix, the coverage Multiply its Factor
 * hangs on, its base map, its own mask when one sits on the coverage path, and every node of
 * every correction hanging on it -- the whole unit a move, a duplicate or a removal has to carry
 * along (spec 18 §4.5). What feeds the row from outside -- the row below it -- is not part of the
 * set. Clears \a r_nodes first; a row the channel shows nothing of (a bare base) leaves it empty.
 */
void layer_owned_nodes_collect(const ChainLayer &layer, Vector<bNode *> &r_nodes);

/**
 * Copy the owned \a nodes into \a dst_tree, preserving every link whose both ends are in the set:
 * the map into the corrections and the row's Mix, a correction's over pair, a mask onto the
 * coverage it drives. Feeds from outside the set -- the row below -- stay unlinked; the caller
 * wires those. Fills \a r_socket_map for every copied socket and \a r_node_map for every copied
 * node, so the caller can resolve the copies of the row's own handles (#ChainLayer::node and
 * #base_map, a correction's Mix). False when a node could not be copied; the caller then removes
 * what was copied so far, which the two maps name.
 *
 * The source tree's topology cache must be current when this runs. Copying into the very tree the
 * nodes live in is fine: a copy only adds nodes, and the cached link spans of the old nodes keep
 * describing them.
 */
bool layer_owned_nodes_copy(bNodeTree &dst_tree,
                            Span<bNode *> nodes,
                            Map<const bNodeSocket *, bNodeSocket *> &r_socket_map,
                            Map<const bNode *, bNode *> &r_node_map);

/**
 * Whether every link leaving \a node lands on a node of \a owned -- vacuously true when nothing
 * consumes it -- so that removing \a owned can take \a node along without breaking a consumer
 * outside the set. Consulted for the maps of a set being removed; the Mix, the Multiply and the
 * corrections' own nodes go regardless.
 */
bool layer_owned_node_consumed_by(Span<bNode *> owned, const bNode &node);

void new_layer_nodes_discard(Main &bmain, bNodeTree &tree, MutableSpan<NewLayerNodes> nodes);

/** How many levels of layer groups \a tree holds below itself, its own level not counted. */
int layer_group_depth(bNodeTree &tree, int guard = 0);

/** Whether any chain of any channel still ends in a bare Image Texture. */
bool forest_has_bare_bottom(const Vector<Vector<ChannelChain>> &per_channel);

/**
 * Bring every wired channel back to the row structure the stack UI draws (the first wired
 * channel's): a channel holding extra rows has them trimmed off the top, one missing rows has
 * its whole chain re-mirrored from the reference. Flat top-level chains only; a stack whose
 * rows live inside groups is refused unchanged.
 *
 * Repairs the drifted graphs every other edit refuses with
 * #PaintMaterialLayerEditError::ChannelsDisagree; a no-op on an aligned stack.
 */
bool BKE_paint_material_layer_channels_realign(Main &bmain,
                                               Material &ma,
                                               PaintMaterialLayerEditError *r_error);

void forest_top_chains(Vector<Vector<ChannelChain>> &per_channel,
                       Vector<ChannelChain *> &r_top);

bool moving_group_check(Vector<ChannelChain *> &from_chains,
                        const int from_index,
                        bNodeTree &dst_tree,
                        const int dst_nesting,
                        PaintMaterialLayerEditError &r_error);

bool tree_reachable_from(const bNodeTree &from,
                         const bNodeTree &tree,
                         Set<const bNodeTree *> &visited);

bool tree_write_scope_check(Main &bmain,
                            const Material &ma,
                            bNodeTree &tree,
                            PaintMaterialLayerEditError &r_error);

/**
 * Build the plan for \a op on the row at \a ordinal of \a ma, checking every precondition the
 * operation has.
 *
 * \a target_ordinal, \a move_place and \a add_params are meaningful only to the operations that
 * take a second row or extra arguments (#CorrectionEdit carries the correction section the edit
 * aims at, as an int); every other operation ignores them.
 */
bool layer_edit_plan_build(Main &bmain,
                           Material &ma,
                           const int ordinal,
                           const LayerEditOp op,
                           LayerEditPlan &r_plan,
                           PaintMaterialLayerEditError &r_error,
                           const int target_ordinal = -1,
                           const PaintMaterialLayerMovePlace move_place =
                               PaintMaterialLayerMovePlace::Above,
                           const PaintMaterialLayerAddParams *add_params = nullptr);

}  // namespace blender
