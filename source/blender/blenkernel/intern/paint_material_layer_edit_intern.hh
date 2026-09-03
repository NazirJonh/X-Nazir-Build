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
struct bNode;
struct bNodeLink;
struct bNodeSocket;
struct bNodeTree;
enum eNodeSocketInOut : short;

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

/** The twelve operations a plan can be built for: one builder, one order of checks. */
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

Image *layer_image_given(const PaintMaterialLayerAddParams &params, const int channel);

void fill_map_color_for(const int channel, const float fill_color[4], float r_color[4]);

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
 * empty when any layer in the range has no single map to move.
 */
Vector<bNode *> chain_range_map_nodes(ChannelChain &chain,
                                      const int from_ordinal,
                                      const int to_ordinal);

bool layer_group_fill_channel(Main & /*bmain*/,
                              bNodeTree & /*tree*/,
                              bNodeTree &group,
                              ChannelChain &chain,
                              Span<bNode *> map_nodes,
                              const int from_ordinal,
                              const int to_ordinal,
                              const bool build_alpha,
                              Vector<bNode *> &r_nodes_to_remove);

void new_layer_nodes_discard(Main &bmain, bNodeTree &tree, MutableSpan<NewLayerNodes> nodes);

/** How many levels of layer groups \a tree holds below itself, its own level not counted. */
int layer_group_depth(bNodeTree &tree, int guard = 0);

/** Whether any chain of any channel still ends in a bare Image Texture. */
bool forest_has_bare_bottom(const Vector<Vector<ChannelChain>> &per_channel);

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
 * take a second row or extra arguments; every other operation ignores them.
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
