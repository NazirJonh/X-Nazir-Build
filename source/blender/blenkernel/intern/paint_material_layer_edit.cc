/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_material_layer_edit.hh.
 *
 * Every operation here works the same way: collect each channel's chain, check the preconditions
 * across all of them, then rebuild the "what is below me" links from an array. Rebuilding rather
 * than patching is deliberate -- an insert expressed as four unlink/link pairs has four ways to
 * leave the graph half-moved, and the array cannot.
 */

#include "BKE_paint_material_layer_edit.hh"

#include "MEM_guardedalloc.h"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_preview_image.hh"

#include "BLT_translation.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include <utility>

#include <cstdio> /* TEMP-DEBUG [MAT_LAYER]: remove with the prints below. */

#include "paint_material_composite_internal.hh"

namespace blender {

MaterialPaintLayerRuntime &BKE_material_paint_layer_runtime_get(Material &ma)
{
  if (ma.paint_layer_runtime == nullptr) {
    ma.paint_layer_runtime = MEM_new<MaterialPaintLayerRuntime>(__func__);
  }
  return *static_cast<MaterialPaintLayerRuntime *>(ma.paint_layer_runtime);
}

uint64_t BKE_material_paint_layer_revision_get(const Material &ma)
{
  return (ma.paint_layer_runtime != nullptr) ?
             static_cast<const MaterialPaintLayerRuntime *>(ma.paint_layer_runtime)->edit_revision :
             0;
}

namespace {

const char *LAYER_MARKER_PROP = "pbr_paint_layer";
/* The marker that says a node group is a folder of layers; see `08 §2.2`. Kept in step with
 * `paint_material_composite.cc`, which is what reads it back. */
const char *LAYER_GROUP_MARKER_PROP = "pbr_paint_node_group";
const char *LAYER_GROUP_MARKER_VALUE = "LAYER_GROUP";
/* Color tag for layer group folders, stored as IDProperty on the group node. */
const char *LAYER_COLOR_TAG_PROP = "pbr_paint_color_tag";
/* What kind of layer a node is -- #PaintMaterialLayerKind -- on the same nodes the layer marker
 * lives on. A node without one reads as #Paint. */
const char *LAYER_KIND_PROP = "pbr_paint_layer_kind";
/* The colour a Fill layer stands for, as a 4-float array; see
 * #BKE_paint_material_layer_fill_color_get. */
const char *LAYER_FILL_COLOR_PROP = "pbr_paint_fill_color";

/** Matches the reader in `paint_material_layer_model.cc`; see `08 §2.2`, Q2. */
constexpr int LAYER_GROUP_NESTING_MAX = 8;

/**
 * The tail every successful edit in this file shares: the stack moved on, and everything that
 * reads it has to be told.
 *
 * One function rather than the same four calls written out per operation, because the next
 * operation added here will otherwise be the one that forgets a line -- a missed revision bump
 * leaves a reader believing a stack it has already walked is still current, which shows up as a
 * stale list nobody can reproduce. The node tree updates stay at the call sites: which trees an
 * edit touched is the edit's own knowledge, and it is not always \a ma's own tree.
 *
 * \param relations: whether the edit changed a relation between data-blocks -- a node's ID field,
 * a group instance added or removed -- rather than only values inside one tree.
 */
void paint_layer_edit_committed(Main &bmain, Material &ma, const bool relations)
{
  BKE_material_paint_layer_runtime_get(ma).edit_revision++;
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  if (relations) {
    /* The shading component does not refresh the evaluated copy, and a relation edit is exactly
     * what that copy must not miss: images are not copied for evaluation, so its nodes point at
     * the original data-blocks, and one the edit freed would be left dangling there -- read the
     * next time the material compiles. #BKE_main_ensure_invariants tags node tree edits the same
     * way. */
    DEG_id_tag_update(&ma.id, ID_RECALC_SYNC_TO_EVAL);
    /* A relation edit reshapes what the tree's output actually reads (a layer added, moved, or
     * removed relinks the chain), which #BKE_main_ensure_invariants' tree_output_changed_fn tags
     * as #ID_RECALC_NTREE_OUTPUT on the tree itself -- this file's hand-rolled update never does,
     * so without it the viewport keeps the old result until some unrelated redraw (e.g. a
     * workspace switch) happens to force one. */
    if (ma.nodetree != nullptr) {
      DEG_id_tag_update(&ma.nodetree->id, ID_RECALC_NTREE_OUTPUT);
    }
    DEG_relations_tag_update(&bmain);
  }
  BKE_paint_material_composite_cache_invalidate(&ma);
}

/** The tree the group instance \a node opens, or null when it holds none or not a node tree. */
bNodeTree *layer_group_tree_of(const bNode &node)
{
  if (node.id == nullptr || GS(node.id->name) != ID_NT) {
    return nullptr;
  }
  return id_cast<bNodeTree *>(node.id);
}

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

/** The one link arriving at \a socket, or null when there is none or more than one. */
bNodeLink *sole_link_into(bNodeSocket &socket)
{
  const Span<bNodeLink *> links = socket.directly_linked_links();
  if (links.size() != 1 || !links[0]->is_available()) {
    return nullptr;
  }
  return links[0];
}

/**
 * Whether anything at all feeds \a socket.
 *
 * Distinct from #sole_link_into returning null, which also means "more than one link" or "a link
 * that is not available": those are chains this file refuses to rewrite, and reading them as an
 * unlinked bottom would quietly turn a broken chain into a short one.
 */
bool socket_has_link(bNodeSocket &socket)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (link->is_available()) {
      return true;
    }
  }
  return false;
}

/**
 * Walk \a terminal down to the bottom of the chain.
 *
 * Unlike the reader, this refuses reroutes: it is about to rewrite these links, and a reroute is a
 * user's deliberate arrangement that a rebuild would silently discard.
 */
bool chain_collect(bNodeTree &tree,
                   bNodeSocket &terminal,
                   ChannelChain &r_chain,
                   PaintMaterialLayerEditError &r_error)
{
  r_chain.tree = &tree;
  r_chain.terminal = &terminal;
  r_chain.terminal_node = &terminal.owner_node();

  Vector<ChainLayer> top_down;
  bool reached_bottom = false;
  bNodeSocket *socket = &terminal;
  for (int step = 0; step < 64; step++) {
    if (!socket_has_link(*socket)) {
      /* Nothing under the lowest Mix node is the bottom of a uniform chain: it blends over the
       * transparency its own socket holds, which is what lets a layer be put below it later. An
       * unlinked terminal, on the other hand, is a channel that was never a stack. */
      if (top_down.is_empty()) {
        r_error = PaintMaterialLayerEditError::ChainNotPlain;
        return false;
      }
      reached_bottom = true;
      break;
    }
    bNodeLink *link = sole_link_into(*socket);
    if (link == nullptr) {
      /* Linked, but not in a shape this file can rewrite: several links, or a muted one. */
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }
    bNode &from = *link->fromnode;
    if (from.is_reroute()) {
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }

    CompositeMixNode mix;
    if (composite_mix_node_read(from, mix)) {
      /* A result consumed by anything but the layer above cannot be reordered without changing
       * what that other consumer sees. */
      if (link->fromsock->directly_linked_links().size() != 1) {
        r_error = PaintMaterialLayerEditError::ChainIsShared;
        return false;
      }
      ChainLayer layer;
      layer.node = &from;
      layer.bottom = const_cast<bNodeSocket *>(mix.bottom);
      layer.top = const_cast<bNodeSocket *>(mix.top);
      layer.factor = const_cast<bNodeSocket *>(mix.factor);
      layer.output = link->fromsock;
      const bNode *top_source = composite_source_node_shallow(*mix.top);
      layer.is_group = top_source != nullptr && BKE_paint_material_is_layer_group(*top_source);
      if (!layer.is_group) {
        const ImageUser *iuser = nullptr;
        composite_image_from_socket(*mix.top, layer.image, iuser);
      }
      r_chain.group_num += layer.is_group ? 1 : 0;
      top_down.append(layer);
      socket = layer.bottom;
      continue;
    }

    if (from.type_legacy == SH_NODE_TEX_IMAGE) {
      ChainLayer base;
      base.node = &from;
      base.output = link->fromsock;
      base.image = (from.id != nullptr && GS(from.id->name) == ID_IM) ?
                       id_cast<Image *>(from.id) :
                       nullptr;
      top_down.append(base);
      reached_bottom = true;
      break;
    }

    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }

  if (top_down.is_empty() || !reached_bottom) {
    /* Ran out of depth without reaching a bottom. */
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }

  r_chain.layers.clear();
  for (int i = top_down.size() - 1; i >= 0; i--) {
    r_chain.layers.append(top_down[i]);
  }
  return true;
}

/**
 * Every channel of \a ma that resolves to a chain, with the longest one first.
 *
 * A material may wire Base Color and Roughness and leave Metallic constant; that is normal, and
 * only the channels that are actually wired take part in an edit.
 */
bool chains_collect(Material &ma, Vector<ChannelChain> &r_chains, PaintMaterialLayerEditError &r_error)
{
  r_chains.clear();
  if (ma.nodetree == nullptr) {
    r_error = PaintMaterialLayerEditError::NotAStack;
    return false;
  }
  /* Walking a chain reads links and asks sockets who owns them, and a caller that just created or
   * removed a node has left that cache invalid. */
  ma.nodetree->ensure_topology_cache();
  for (const int channel : BKE_paint_material_composite_passes()) {
    const bNodeSocket *terminal = paint_material_channel_socket_find(ma, channel);
    if (terminal == nullptr) {
      continue;
    }
    ChannelChain chain;
    chain.channel = channel;
    PaintMaterialLayerEditError channel_error = PaintMaterialLayerEditError::None;
    if (!chain_collect(
            *ma.nodetree, *const_cast<bNodeSocket *>(terminal), chain, channel_error))
    {
      /* An unwired or procedural channel is not a failure; a broken chain in a channel that *is*
       * wired as a stack is, because a partial edit would desynchronize the channels. */
      if (channel_error == PaintMaterialLayerEditError::ChainIsShared) {
        r_error = channel_error;
        return false;
      }
      continue;
    }
    r_chains.append(std::move(chain));
  }
  if (r_chains.is_empty()) {
    r_error = PaintMaterialLayerEditError::NotAStack;
    return false;
  }
  return true;
}

/**
 * A socket of \a node found by the name a user sees, not by its identifier.
 *
 * Sockets of a group instance and of its Group Output come from the tree interface, and their
 * identifiers are handed out in creation order (`Socket_0`, `Socket_1`, ...) rather than derived
 * from the name. Looking them up by name is what makes "the Result of the Roughness channel" a
 * question this code can ask.
 */
bNodeSocket *socket_find_by_name(bNode &node, const eNodeSocketInOut in_out, const StringRef name)
{
  ListBaseT<bNodeSocket> &sockets = (in_out == SOCK_IN) ? node.inputs : node.outputs;
  for (bNodeSocket &socket : sockets) {
    if (socket.name == name) {
      return &socket;
    }
  }
  return nullptr;
}

/** The socket inside \a group its `Result` for \a channel comes from: the top of the sub-stack. */
bNodeSocket *group_result_socket(const bNode &group, const int channel)
{
  bNodeTree *group_tree = layer_group_tree_of(group);
  if (group_tree == nullptr) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char result_name[64];
  SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);

  group_tree->ensure_topology_cache();
  for (bNode &node : group_tree->nodes) {
    if (node.type_legacy != NODE_GROUP_OUTPUT) {
      continue;
    }
    if (bNodeSocket *result = socket_find_by_name(node, SOCK_IN, result_name)) {
      return result;
    }
  }
  return nullptr;
}

/**
 * Every chain of one channel: the one at the top level, and one more for each group's sub-stack.
 *
 * Collected in the same order the UI model walks the graph -- children before the group row that
 * holds them -- so that the ordinals the two hand out line up, which is what lets an operator say
 * "the layer the user clicked" with a number.
 */
bool chain_forest_collect(bNodeTree &tree,
                          bNodeSocket &terminal,
                          const int channel,
                          Vector<ChannelChain> &r_chains,
                          const int nesting,
                          PaintMaterialLayerEditError &r_error)
{
  if (nesting > LAYER_GROUP_NESTING_MAX) {
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  ChannelChain chain;
  chain.channel = channel;
  chain.nesting = nesting;
  if (!chain_collect(tree, terminal, chain, r_error)) {
    return false;
  }
  /* The sub-stacks are collected first so that the chain they belong to can name them by index
   * once it is appended: a group row knows the chain it opens. */
  for (ChainLayer &layer : chain.layers) {
    if (!layer.is_group) {
      continue;
    }
    const bNode *group = composite_source_node_shallow(*layer.top);
    bNodeSocket *result = (group == nullptr) ? nullptr : group_result_socket(*group, channel);
    bNodeTree *group_tree = (group == nullptr) ? nullptr : layer_group_tree_of(*group);
    if (group_tree == nullptr) {
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }
    /* A group whose Group Output is unlinked is an empty folder: it holds no chain to collect, and
     * it contributes nothing, since an unlinked Result is transparent and its Alpha zero. Its own
     * row stays -- that is what the user drops layers into.
     *
     * The socket itself is always there, since it comes from the group's interface; what says the
     * folder is empty is that nothing feeds it. */
    if (result == nullptr || !socket_has_link(*result)) {
      continue;
    }
    /* Captured after the call, not before: it may append more than one chain -- its own subgroups'
     * as well as its own -- and the one this layer opens is always the last of those, whatever the
     * count. Capturing #r_chains.size() beforehand only holds for a single level of nesting; a
     * group inside a group left the outer one pointing at the inner one's own chain instead of its
     * own, and the outer row was then never found -- while a row on either side of it still was,
     * by coincidence, since a group's own chain and the first one its recursion collects can land
     * on the same index. */
    if (!chain_forest_collect(
            *group_tree, *result, channel, r_chains, nesting + 1, r_error))
    {
      return false;
    }
    layer.sub_chain_index = int(r_chains.size()) - 1;
  }
  r_chains.append(std::move(chain));
  return true;
}

void relink_into(bNodeTree &tree,
                 bNodeSocket &into,
                 bNode &into_node,
                 bNode &from_node,
                 bNodeSocket &from_socket)
{
  tree.ensure_topology_cache();
  for (bNodeLink *link : Vector<bNodeLink *>(into.directly_linked_links())) {
    BKE_ntree_update_tag_link_removed(&tree);
    bke::node_remove_link(&tree, *link);
  }
  bNodeLink &link = bke::node_add_link(tree, from_node, from_socket, into_node, into);
  BKE_ntree_update_tag_link_added(&tree, &link);
}

/** Wire `chain.layers` back up in their current array order, bottom to top. */
void chain_rebuild_links(ChannelChain &chain)
{
  bNodeTree &tree = *chain.tree;
  if (chain.layers.is_empty()) {
    return;
  }
  ChainLayer &bottom = chain.layers.first();
  if (bottom.is_mix()) {
    /* The lowest layer of a uniform chain blends over the transparency its own socket holds, so
     * whatever used to sit under it -- before a move took that layer away -- has to be unwired. */
    tree.ensure_topology_cache();
    for (bNodeLink *link : Vector<bNodeLink *>(bottom.bottom->directly_linked_links())) {
      BKE_ntree_update_tag_link_removed(&tree);
      bke::node_remove_link(&tree, *link);
    }
  }
  for (const int64_t i : chain.layers.index_range().drop_front(1)) {
    ChainLayer &layer = chain.layers[i];
    ChainLayer &below = chain.layers[i - 1];
    relink_into(tree, *layer.bottom, *layer.node, *below.node, *below.output);
  }
  ChainLayer &top = chain.layers.last();
  relink_into(tree, *chain.terminal, *chain.terminal_node, *top.node, *top.output);
}

IDProperty *node_properties_ensure(bNode &node)
{
  if (node.prop == nullptr) {
    IDPropertyTemplate val = {0};
    node.prop = IDP_New(IDP_GROUP, &val, "RNA");
  }
  return node.prop;
}

/** Align every channel's layer `i` with the reference chain's layer `i`. */
bool chains_align(Span<ChannelChain> chains, PaintMaterialLayerEditError &r_error)
{
  const int64_t layer_num = chains.first().layers.size();
  for (const ChannelChain &chain : chains) {
    if (chain.layers.size() != layer_num) {
      /* Matching by marker would let the counts differ, but a stack whose channels disagree is a
       * stack the UI is already drawing wrong; refusing here is the honest answer. */
      r_error = PaintMaterialLayerEditError::ChannelsDisagree;
      return false;
    }
  }
  return true;
}

/**
 * Every chain of every wired channel, in the order the UI model walks them.
 *
 * One entry per channel per nesting level: `[channel 0's sub-stacks..., channel 0's top-level
 * chain, channel 1's ...]`. Which chain a row lives in is answered by #forest_resolve_ordinal.
 */
bool chains_collect_forest(Material &ma,
                           Vector<Vector<ChannelChain>> &r_per_channel,
                           PaintMaterialLayerEditError &r_error)
{
  r_per_channel.clear();
  if (ma.nodetree == nullptr) {
    r_error = PaintMaterialLayerEditError::NotAStack;
    return false;
  }
  ma.nodetree->ensure_topology_cache();
  for (const int channel : BKE_paint_material_composite_passes()) {
    const bNodeSocket *terminal = paint_material_channel_socket_find(ma, channel);
    if (terminal == nullptr) {
      continue;
    }
    Vector<ChannelChain> chains;
    PaintMaterialLayerEditError channel_error = PaintMaterialLayerEditError::None;
    if (!chain_forest_collect(*ma.nodetree,
                              *const_cast<bNodeSocket *>(terminal),
                              channel,
                              chains,
                              0,
                              channel_error))
    {
      if (channel_error == PaintMaterialLayerEditError::ChainIsShared) {
        r_error = channel_error;
        return false;
      }
      continue;
    }
    r_per_channel.append(std::move(chains));
  }
  if (r_per_channel.is_empty()) {
    r_error = PaintMaterialLayerEditError::NotAStack;
    return false;
  }
  return true;
}

/** Where a row lives: which chain of one channel's forest, and which position in it. */
struct ForestPosition {
  int chain_index = -1;
  int layer_index = -1;
};

/**
 * The chain and position \a ordinal names, numbered exactly as the UI model numbers its rows.
 *
 * Rows of the top-level chain are numbered by position; rows inside groups continue from
 * #PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE in the order the walk meets them, which is depth first
 * with a group's children before the group itself.
 */
ForestPosition forest_resolve_ordinal(Span<ChannelChain> chains, const int ordinal)
{
  /* The last chain of a channel's forest is its top level; the rest are sub-stacks. */
  const int top_index = int(chains.size()) - 1;
  if (ordinal < PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE) {
    if (ordinal < 0 || ordinal >= chains[top_index].layers.size()) {
      return {};
    }
    return {top_index, ordinal};
  }

  int nested_seen = 0;
  const int wanted = ordinal - PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE;
  /* Walk the same way the model does: a group's rows come before the group row itself. */
  ForestPosition found;
  auto walk = [&](auto &&self, const int chain_index, const bool count) -> void {
    const ChannelChain &chain = chains[chain_index];
    for (const int64_t index : chain.layers.index_range()) {
      const ChainLayer &layer = chain.layers[index];
      if (layer.is_group && layer.sub_chain_index >= 0) {
        self(self, layer.sub_chain_index, true);
      }
      if (!count || found.chain_index >= 0) {
        continue;
      }
      if (nested_seen == wanted) {
        found = {chain_index, int(index)};
      }
      nested_seen++;
    }
  };
  walk(walk, top_index, false);
  return found;
}

/**
 * The chain of every channel that holds the row \a ordinal names, and the row's index in it.
 *
 * This is what lets an operation act on a layer wherever it lives: the position is resolved per
 * channel through that channel's own forest, and the channels are then checked to agree, exactly
 * as #chains_align checks a flat stack.
 */
bool forest_rows_resolve(Vector<Vector<ChannelChain>> &per_channel,
                         const int ordinal,
                         Vector<ChannelChain *> &r_chains,
                         int &r_layer_index,
                         PaintMaterialLayerEditError &r_error)
{
  r_chains.clear();
  r_layer_index = -1;
  for (Vector<ChannelChain> &chains : per_channel) {
    const ForestPosition position = forest_resolve_ordinal(chains, ordinal);
    if (position.chain_index < 0) {
      r_error = PaintMaterialLayerEditError::IndexOutOfRange;
      return false;
    }
    if (r_layer_index >= 0 && position.layer_index != r_layer_index) {
      /* One channel puts this row in a different place than another: the stack the UI is drawing
       * is not the stack the graph has, and editing either half of it would make that worse. */
      r_error = PaintMaterialLayerEditError::ChannelsDisagree;
      return false;
    }
    r_layer_index = position.layer_index;
    r_chains.append(&chains[position.chain_index]);
  }
  const int64_t layer_num = r_chains.first()->layers.size();
  for (const ChannelChain *chain : r_chains) {
    if (chain->layers.size() != layer_num) {
      r_error = PaintMaterialLayerEditError::ChannelsDisagree;
      return false;
    }
  }
  return true;
}

/**
 * The inverse of #forest_resolve_ordinal: the ordinal the UI model would give the layer at
 * \a target_layer of \a chains[target_chain] -- its plain position if that chain is the forest's
 * top level, otherwise counted the same depth-first, a group's children before the group row
 * itself, way #forest_resolve_ordinal decodes.
 *
 * Used to report where a freshly inserted row landed: an edit builds a row directly, in the chain
 * it belongs to, and has no ordinal for it until this walks the forest to find one.
 */
int forest_ordinal_for_position(Span<ChannelChain> chains,
                                const int target_chain,
                                const int target_layer)
{
  const int top_index = int(chains.size()) - 1;
  if (target_chain == top_index) {
    return target_layer;
  }

  int nested_seen = 0;
  int found_ordinal = -1;
  auto walk = [&](auto &&self, const int chain_index, const bool count) -> void {
    const ChannelChain &chain = chains[chain_index];
    for (const int64_t index : chain.layers.index_range()) {
      const ChainLayer &layer = chain.layers[index];
      if (layer.is_group && layer.sub_chain_index >= 0) {
        self(self, layer.sub_chain_index, true);
      }
      if (!count || found_ordinal >= 0) {
        continue;
      }
      if (chain_index == target_chain && int(index) == target_layer) {
        found_ordinal = PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE + nested_seen;
      }
      nested_seen++;
    }
  };
  walk(walk, top_index, false);
  return found_ordinal;
}

/** One channel's group instance node for an #Add whose anchor is an empty folder. */
struct AddEmptyGroupInstance {
  bNode *instance = nullptr;
  int channel = -1;
};

/**
 * Resolve #PaintMaterialLayerAddParams::anchor_ordinal to the chains a new layer goes into and the
 * index within them.
 *
 * The anchor names a UI row. A plain row -- top level or inside a folder -- takes the new layer
 * directly above itself, in its own chain. A row that is a folder takes it inside, on top of what
 * the folder holds. A folder that holds nothing yet has no chain to resolve: \a r_into_empty_group
 * is set and \a r_empty_group_instances receives its per-channel instance nodes, which is where the
 * mutation wires the first layer's output.
 */
static bool add_anchor_resolve(Vector<Vector<ChannelChain>> &per_channel,
                               const int anchor_ordinal,
                               Vector<ChannelChain *> &r_chains,
                               int &r_insert_index,
                               bool &r_into_empty_group,
                               Vector<AddEmptyGroupInstance> &r_empty_group_instances,
                               PaintMaterialLayerEditError &r_error)
{
  r_chains.clear();
  r_insert_index = -1;
  r_into_empty_group = false;
  r_empty_group_instances.clear();

  auto disagree = [&]() {
    r_error = PaintMaterialLayerEditError::ChannelsDisagree;
    return false;
  };

  for (Vector<ChannelChain> &chains : per_channel) {
    const ForestPosition pos = forest_resolve_ordinal(chains, anchor_ordinal);
    if (pos.chain_index < 0) {
      r_error = PaintMaterialLayerEditError::IndexOutOfRange;
      return false;
    }
    ChainLayer &anchor_layer = chains[pos.chain_index].layers[pos.layer_index];
    if (anchor_layer.is_group && anchor_layer.sub_chain_index < 0) {
      /* An empty folder: nothing to blend over, so the mutation wires a bare map straight to the
       * group's own Result output. */
      if (r_insert_index >= 0 && !r_into_empty_group) {
        return disagree();
      }
      bNode *instance = const_cast<bNode *>(composite_source_node_shallow(*anchor_layer.top));
      if (instance == nullptr) {
        r_error = PaintMaterialLayerEditError::ChainNotPlain;
        return false;
      }
      r_into_empty_group = true;
      r_insert_index = 0;
      r_empty_group_instances.append({instance, chains[pos.chain_index].channel});
      continue;
    }
    ChannelChain &target = anchor_layer.is_group ? chains[anchor_layer.sub_chain_index] :
                                                   chains[pos.chain_index];
    const int insert = anchor_layer.is_group ? int(target.layers.size()) : pos.layer_index + 1;
    if (r_insert_index >= 0 && (r_into_empty_group || insert != r_insert_index)) {
      return disagree();
    }
    r_insert_index = insert;
    r_chains.append(&target);
  }

  if (!r_into_empty_group) {
    const int64_t layer_num = r_chains.first()->layers.size();
    for (const ChannelChain *chain : r_chains) {
      if (chain->layers.size() != layer_num) {
        return disagree();
      }
    }
  }
  return true;
}

/**
 * Whether \a ordinal names a layer of the chain rather than one held inside a group.
 *
 * Ordinals of top-level rows are their position in the chain, so they mean the same thing to the
 * UI and to this file. Rows inside a group are numbered from #PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE
 * instead, because they have no position in this chain at all -- and editing them means walking
 * into the group's own tree, which the operations here do not do yet.
 */
bool ordinal_is_in_chain(const int ordinal, PaintMaterialLayerEditError &r_error)
{
  if (ordinal >= PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE) {
    r_error = PaintMaterialLayerEditError::HasGroups;
    return false;
  }
  return true;
}

/** The socket a Mix node (or the Normal Combine group) hands its result out by. */
bNodeSocket *mix_output_find(bNode &node)
{
  if (BKE_paint_material_is_normal_combine_group(node)) {
    /* A group instance inherits the interface's `Socket_N` identifiers, not its display name;
     * #node_find_socket matches identifiers. See #NORMAL_COMBINE_ID_*. */
    return bke::node_find_socket(
        node, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
  }
  if (bNodeSocket *socket = bke::node_find_socket(node, SOCK_OUT, "Result_Color"_ustr)) {
    return socket;
  }
  /* #SH_NODE_MIX_RGB_LEGACY. */
  return bke::node_find_socket(node, SOCK_OUT, "Color"_ustr);
}

/**
 * Wire \a factor to a fresh Math node in Multiply mode instead of to \a coverage directly: one
 * input takes \a coverage (the layer's own map's Alpha, driving per-pixel "over" compositing the
 * way it always has), the other is left a plain constant at \a initial_opacity -- the layer's own
 * opacity, which #BKE_paint_material_layer_stack_from_material can still point a Value slider at.
 * A bare link into Factor, by contrast, leaves nothing there to edit (`08 §1.4`).
 *
 * \return false, leaving the tree unchanged, only when the node could not be created at all.
 */
static bool layer_factor_coverage_link(bNodeTree &tree,
                                       bNode &factor_node,
                                       bNodeSocket &factor,
                                       bNode &coverage_node,
                                       bNodeSocket &coverage,
                                       const float initial_opacity)
{
  bNode *multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (multiply == nullptr) {
    return false;
  }
  multiply->custom1 = NODE_MATH_MULTIPLY;
  bNodeSocket *value_a = static_cast<bNodeSocket *>(BLI_findlink(&multiply->inputs, 0));
  bNodeSocket *value_b = static_cast<bNodeSocket *>(BLI_findlink(&multiply->inputs, 1));
  bNodeSocket *result = static_cast<bNodeSocket *>(multiply->outputs.first);
  if (value_a == nullptr || value_b == nullptr || result == nullptr) {
    bke::node_remove_node(nullptr, tree, *multiply, true);
    return false;
  }
  static_cast<bNodeSocketValueFloat *>(value_b->default_value)->value = initial_opacity;
  bke::node_position_relative(*multiply, coverage_node, result, coverage);
  bke::node_add_link(tree, coverage_node, coverage, *multiply, *value_a);
  bke::node_add_link(tree, *multiply, *result, factor_node, factor);
  return true;
}

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

/**
 * The map the caller handed over for \a channel, or null when this channel's map is to be created.
 */
static Image *layer_image_given(const PaintMaterialLayerAddParams &params, const int channel)
{
  for (const PaintMaterialLayerChannelImage &given : params.channel_images) {
    if (given.channel == channel) {
      return given.image;
    }
  }
  return nullptr;
}

/**
 * The colour a map of a Fill layer is (re-)filled with; what #layer_image_create starts one as.
 *
 * A scalar channel has one meaningful component, and the fill colour's red carries it; Normal is
 * flat tangent space so a filled Normal layer starts out as "no change".
 */
static void fill_map_color_for(const int channel, const float fill_color[4], float r_color[4])
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  const float value = info.is_color ? 0.0f : fill_color[0];
  r_color[0] = info.is_color ? fill_color[0] : value;
  r_color[1] = info.is_color ? fill_color[1] : value;
  r_color[2] = info.is_color ? fill_color[2] : value;
  r_color[3] = 1.0f;
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    r_color[0] = 0.5f;
    r_color[1] = 0.5f;
    r_color[2] = 1.0f;
  }
}

/**
 * A map for \a channel, in the color space and with the neutral value that channel needs.
 *
 * Mirrors what #BKE_paint_principled_channel_image_ensure creates, so a layer added here and a map
 * created by the first brush stroke are the same kind of thing.
 */
Image *layer_image_create(Main &bmain,
                          const int channel,
                          const PaintMaterialLayerAddParams &params)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));

  char image_name[MAX_ID_NAME - 2];
  if (params.name != nullptr && params.name[0] != '\0') {
    STRNCPY_UTF8(image_name, params.name);
  }
  else {
    SNPRINTF_UTF8(image_name, "%s TexLayer", info.ui_name);
  }

  float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (params.type == PaintMaterialLayerAddType::Fill) {
    fill_map_color_for(channel, params.fill_color, color);
  }

  Image *image = BKE_image_add_generated(&bmain,
                                         params.image_size,
                                         params.image_size,
                                         image_name,
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         color,
                                         false,
                                         !info.is_color,
                                         false);
  if (image != nullptr) {
    image->flag |= IMA_PAINT_CANVAS;
  }
  return image;
}

/**
 * A new, empty layer-group tree: the marker, one `Result` per channel and one shared `Alpha`.
 *
 * The alpha is shared because a layer's coverage is one thing in this model -- the same Factor
 * drives every channel of a layer -- so a group made of those layers has one coverage too.
 */
bNodeTree *layer_group_tree_add(Main &bmain,
                                Span<ChannelChain *> chains,
                                const int from_ordinal,
                                const int to_ordinal)
{
  bNodeTree *group = bke::node_tree_add_tree(&bmain, DATA_("Paint Layer Group"), "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }
  IDProperty *properties = IDP_EnsureProperties(&group->id);
  IDPropertyTemplate value = {0};
  value.string.str = const_cast<char *>(LAYER_GROUP_MARKER_VALUE);
  value.string.len = int(strlen(LAYER_GROUP_MARKER_VALUE)) + 1;
  value.string.subtype = IDP_STRING_SUB_UTF8;
  IDP_AddToGroup(properties, IDP_New(IDP_STRING, &value, LAYER_GROUP_MARKER_PROP));

  for (const ChannelChain *chain : chains) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(chain->channel));
    char result_name[64];
    SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
    group->tree_interface.add_socket(
        result_name, "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  }
  group->tree_interface.add_socket(
      DATA_("Alpha"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);

  bNode *output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  output->location[0] = 400.0f * (to_ordinal - from_ordinal + 1);
  return group;
}

/**
 * Move one channel's layers `from_ordinal + 1 .. to_ordinal` into \a group, and the map of
 * `from_ordinal` with them, leaving the Mix node of `from_ordinal` behind to blend the group in.
 *
 * Nodes are copied into the group and the originals collected in \a r_nodes_to_remove: there is no
 * "move a node to another tree" in the node API, and copying keeps the id-properties -- the layer
 * marker among them -- which is what makes a layer inside a group still the same layer.
 */
/**
 * The map node of every layer in a range, resolved while the topology cache is still good.
 *
 * Creating a node invalidates that cache, and the group is built by creating a great many of them;
 * asking a socket for its links halfway through is the assert this exists to avoid. Empty when a
 * layer of the range has no single map to move.
 */
Vector<bNode *> chain_range_map_nodes(ChannelChain &chain,
                                      const int from_ordinal,
                                      const int to_ordinal)
{
  Vector<bNode *> maps;
  for (const int ordinal : IndexRange(from_ordinal, to_ordinal - from_ordinal + 1)) {
    ChainLayer &layer = chain.layers[ordinal];
    bNodeLink *link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
    if (link == nullptr) {
      return {};
    }
    maps.append(link->fromnode);
  }
  return maps;
}

bool layer_group_fill_channel(Main & /*bmain*/,
                              bNodeTree & /*tree*/,
                              bNodeTree &group,
                              ChannelChain &chain,
                              Span<bNode *> map_nodes,
                              const int from_ordinal,
                              const int to_ordinal,
                              const bool build_alpha,
                              Vector<bNode *> &r_nodes_to_remove)
{
  bNode *output = nullptr;
  for (bNode &node : group.nodes) {
    if (node.type_legacy == NODE_GROUP_OUTPUT) {
      output = &node;
      break;
    }
  }
  if (output == nullptr) {
    return false;
  }

  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(chain.channel));
  char result_name[64];
  SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);

  /* The bottom of the sub-stack is the map the kept Mix node used to blend. */
  if (map_nodes.size() != to_ordinal - from_ordinal + 1) {
    return false;
  }

  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  bNode *below = bke::node_copy_with_mapping(
      &group, *map_nodes.first(), LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
  if (below == nullptr) {
    return false;
  }
  r_nodes_to_remove.append_non_duplicates(map_nodes.first());
  bNodeSocket *below_out = bke::node_find_socket(*below, SOCK_OUT, "Color"_ustr);
  bNodeSocket *alpha_so_far = bke::node_find_socket(*below, SOCK_OUT, "Alpha"_ustr);
  if (below_out == nullptr || alpha_so_far == nullptr) {
    return false;
  }
  bNode *alpha_node = below;
  float location_x = 0.0f;

  for (const int ordinal : IndexRange(from_ordinal + 1, to_ordinal - from_ordinal)) {
    ChainLayer &layer = chain.layers[ordinal];
    bNode *map_source = map_nodes[ordinal - from_ordinal];
    bNode *map_copy = bke::node_copy_with_mapping(
        &group, *map_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
    bNode *mix_copy = bke::node_copy_with_mapping(
        &group, *layer.node, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
    if (map_copy == nullptr || mix_copy == nullptr) {
      return false;
    }
    r_nodes_to_remove.append_non_duplicates(map_source);
    r_nodes_to_remove.append_non_duplicates(layer.node);

    /* A previous pass through this loop may have added links, which invalidates the topology
     * cache #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    group.ensure_topology_cache();
    CompositeMixNode mix;
    bNodeSocket *mix_out = mix_output_find(*mix_copy);
    bNodeSocket *map_color = bke::node_find_socket(*map_copy, SOCK_OUT, "Color"_ustr);
    bNodeSocket *map_alpha = bke::node_find_socket(*map_copy, SOCK_OUT, "Alpha"_ustr);
    if (mix_out == nullptr || map_color == nullptr || map_alpha == nullptr ||
        !composite_mix_node_read(*mix_copy, mix))
    {
      return false;
    }
    location_x += 300.0f;
    mix_copy->location[0] = location_x;
    map_copy->location[0] = location_x - 200.0f;

    bke::node_add_link(
        group, *below, *below_out, *mix_copy, *const_cast<bNodeSocket *>(mix.bottom));
    bke::node_add_link(
        group, *map_copy, *map_color, *mix_copy, *const_cast<bNodeSocket *>(mix.top));
    layer_factor_coverage_link(
        group, *mix_copy, *const_cast<bNodeSocket *>(mix.factor), *map_copy, *map_alpha, 1.0f);
    below = mix_copy;
    below_out = mix_out;

    if (build_alpha) {
      /* Coverage accumulates the way an "over" does: `a = a_below + a_layer * (1 - a_below)`. */
      bNode *invert = bke::node_add_static_node(nullptr, group, SH_NODE_MATH);
      invert->custom1 = NODE_MATH_SUBTRACT;
      bNode *combine = bke::node_add_static_node(nullptr, group, SH_NODE_MATH);
      combine->custom1 = NODE_MATH_MULTIPLY_ADD;
      bNodeSocket *invert_a = static_cast<bNodeSocket *>(BLI_findlink(&invert->inputs, 0));
      bNodeSocket *invert_b = static_cast<bNodeSocket *>(BLI_findlink(&invert->inputs, 1));
      bNodeSocket *invert_out = static_cast<bNodeSocket *>(invert->outputs.first);
      bNodeSocket *combine_a = static_cast<bNodeSocket *>(BLI_findlink(&combine->inputs, 0));
      bNodeSocket *combine_b = static_cast<bNodeSocket *>(BLI_findlink(&combine->inputs, 1));
      bNodeSocket *combine_c = static_cast<bNodeSocket *>(BLI_findlink(&combine->inputs, 2));
      bNodeSocket *combine_out = static_cast<bNodeSocket *>(combine->outputs.first);
      if (invert_a == nullptr || invert_b == nullptr || combine_c == nullptr) {
        return false;
      }
      static_cast<bNodeSocketValueFloat *>(invert_a->default_value)->value = 1.0f;
      invert->location[0] = location_x;
      invert->location[1] = -300.0f;
      combine->location[0] = location_x + 150.0f;
      combine->location[1] = -300.0f;

      bke::node_add_link(group, *alpha_node, *alpha_so_far, *invert, *invert_b);
      bke::node_add_link(group, *map_copy, *map_alpha, *combine, *combine_a);
      bke::node_add_link(group, *invert, *invert_out, *combine, *combine_b);
      bke::node_add_link(group, *alpha_node, *alpha_so_far, *combine, *combine_c);
      alpha_node = combine;
      alpha_so_far = combine_out;
    }
  }

  bNodeSocket *result_in = socket_find_by_name(*output, SOCK_IN, result_name);
  if (result_in == nullptr) {
    return false;
  }
  bke::node_add_link(group, *below, *below_out, *output, *result_in);
  if (build_alpha) {
    if (bNodeSocket *alpha_in = socket_find_by_name(*output, SOCK_IN, "Alpha")) {
      bke::node_add_link(group, *alpha_node, *alpha_so_far, *output, *alpha_in);
    }
  }
  return true;
}

/** Undo everything #layer_nodes_create made, for a transaction that turned out to be impossible. */
void new_layer_nodes_discard(Main &bmain, bNodeTree &tree, MutableSpan<NewLayerNodes> nodes)
{
  for (NewLayerNodes &added : nodes) {
    if (added.mix != nullptr) {
      /* Removed as a user: on the Normal channel this node is an instance of the shared
       * normal-combine group, and the failed transaction has to give that user back. A plain Mix
       * node references nothing, so the flag costs it nothing. */
      bke::node_remove_node(&bmain, tree, *added.mix, true);
    }
    if (added.tex != nullptr) {
      /* Clear the reference first: the node is about to go, and the image right after it. */
      added.tex->id = nullptr;
      bke::node_remove_node(&bmain, tree, *added.tex, false);
    }
    if (added.image != nullptr && added.owns_image) {
      /* A handed-over map is not this transaction's to free here; the add's own exit does. */
      BKE_id_free(&bmain, added.image);
    }
    added = NewLayerNodes{};
  }
}

/** How many levels of layer groups \a tree holds below itself, its own level not counted. */
static int layer_group_depth(bNodeTree &tree, const int guard = 0)
{
  if (guard > LAYER_GROUP_NESTING_MAX) {
    return guard;
  }
  int deepest = 0;
  for (bNode &node : tree.nodes) {
    if (node.id == nullptr || !BKE_paint_material_is_layer_group(node)) {
      continue;
    }
    const int depth = 1 + layer_group_depth(*id_cast<bNodeTree *>(node.id), guard + 1);
    if (depth > deepest) {
      deepest = depth;
    }
  }
  return deepest;
}

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

/** Whether any chain of any channel still ends in a bare Image Texture. */
static bool forest_has_bare_bottom(const Vector<Vector<ChannelChain>> &per_channel)
{
  for (const Vector<ChannelChain> &chains : per_channel) {
    for (const ChannelChain &chain : chains) {
      if (!chain.layers.is_empty() && !chain.layers.first().is_mix()) {
        return true;
      }
    }
  }
  return false;
}

/** The top-level chain of every channel, which is what a flat, nesting-refusing edit works on. */
static void forest_top_chains(Vector<Vector<ChannelChain>> &per_channel,
                              Vector<ChannelChain *> &r_top)
{
  for (Vector<ChannelChain> &chains : per_channel) {
    r_top.append(&chains.last());
  }
}

/**
 * The group tree a moved group row opens, checked the way a move into \a dst_tree needs it to be.
 *
 * A group row is one group with an instance node per channel; what has to agree is the tree those
 * instances open, not the nodes. The move must not land the group inside itself, and must not land
 * it deeper than the readers walk.
 */
static bool moving_group_check(Vector<ChannelChain *> &from_chains,
                        const int from_index,
                        bNodeTree &dst_tree,
                        const int dst_nesting,
                        PaintMaterialLayerEditError &r_error)
{
  bNode *first_instance = nullptr;
  for (ChannelChain *chain : from_chains) {
    ChainLayer &layer = chain->layers[from_index];
    bNodeLink *top_link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
    if (top_link == nullptr) {
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }
    if (first_instance == nullptr) {
      first_instance = top_link->fromnode;
    }
    else if (top_link->fromnode->id != first_instance->id) {
      r_error = PaintMaterialLayerEditError::ChannelsDisagree;
      return false;
    }
  }
  bNodeTree *group_tree = (first_instance != nullptr) ? layer_group_tree_of(*first_instance) :
                                                        nullptr;
  if (group_tree == nullptr || group_tree == &dst_tree ||
      bke::node_tree_contains_tree(*group_tree, dst_tree))
  {
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  if (dst_nesting + 1 + layer_group_depth(*group_tree) > LAYER_GROUP_NESTING_MAX) {
    r_error = PaintMaterialLayerEditError::NestingTooDeep;
    return false;
  }
  return true;
}

/**
 * Whether \a tree is reached from \a from through group instances, directly or deeper.
 *
 * Trees already walked are in \a visited, which may be shared by several walks: a tree that a
 * previous walk fully scanned and did not find \a tree through cannot find it on a second pass
 * either.
 */
static bool tree_reachable_from(const bNodeTree &from,
                                const bNodeTree &tree,
                                Set<const bNodeTree *> &visited)
{
  if (!visited.add(&from)) {
    return false;
  }
  for (const bNode &node : from.nodes) {
    if (node.id == nullptr) {
      continue;
    }
    if (node.id == &tree.id) {
      return true;
    }
    if (bNodeTree *nested = layer_group_tree_of(node)) {
      if (tree_reachable_from(*nested, tree, visited)) {
        return true;
      }
    }
  }
  return false;
}

/**
 * The write right on one tree the edit may write: local, not an override, and not reached from
 * another material's node tree.
 *
 * The raw user count cannot answer the last part -- a group holds one reference per channel
 * instance, so a group only this material uses still counts more than one -- which is why the
 * other materials' trees are walked instead.
 */
static bool tree_write_scope_check(Main &bmain,
                                   const Material &ma,
                                   bNodeTree &tree,
                                   PaintMaterialLayerEditError &r_error)
{
  /* An override is linked as well, so the more specific reason is checked first. */
  if (ID_IS_OVERRIDE_LIBRARY(&tree.id)) {
    r_error = PaintMaterialLayerEditError::TreeIsOverride;
    return false;
  }
  if (!ID_IS_EDITABLE(&tree.id)) {
    r_error = PaintMaterialLayerEditError::TreeNotEditable;
    return false;
  }
  if (&tree.id != &ma.nodetree->id) {
    Set<const bNodeTree *> visited;
    for (const Material &other : bmain.materials) {
      if (&other == &ma || other.nodetree == nullptr) {
        continue;
      }
      if (tree_reachable_from(*other.nodetree, tree, visited)) {
        r_error = PaintMaterialLayerEditError::TreeShared;
        return false;
      }
    }
  }
  return true;
}

/**
 * Collect everything the operation \a op on the row \a ordinal needs, without writing a byte.
 *
 * The one place an edit's possibility is decided. Checks the mutations used to make after their
 * shape conversion -- which already changed the graph by then -- belong here instead, so that a
 * refused edit leaves the graph exactly as it was.
 *
 * \param target_ordinal: the second row of a two-row operation: #Reorder's destination, #Move's
 *   anchor, #GroupMake's range end.
 * \param move_place: how a #Move lands beside or inside its anchor.
 * \param add_params: the add parameters, required for #Add.
 */
static bool layer_edit_plan_build(Main &bmain,
                                  Material &ma,
                                  const int ordinal,
                                  const LayerEditOp op,
                                  LayerEditPlan &r_plan,
                                  PaintMaterialLayerEditError &r_error,
                                  const int target_ordinal = -1,
                                  const PaintMaterialLayerMovePlace move_place =
                                      PaintMaterialLayerMovePlace::Above,
                                  const PaintMaterialLayerAddParams *add_params = nullptr)
{
  r_plan = LayerEditPlan{};

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    r_error = PaintMaterialLayerEditError::NotEditable;
    return false;
  }

  /* An #Add placed at a plain top-level position extends the stack through the flat reader, exactly
   * as the mutation will; an #Add placed relative to a row (which may live inside a folder) and
   * every other operation work on the forest. */
  const bool add_by_anchor = (op == LayerEditOp::Add) && add_params != nullptr &&
                             add_params->anchor_ordinal >= 0;
  if (op == LayerEditOp::Add && !add_by_anchor) {
    BLI_assert(add_params != nullptr);
    Vector<ChannelChain> flat;
    if (!chains_collect(ma, flat, r_error)) {
      if (r_error != PaintMaterialLayerEditError::NotAStack) {
        /* A stack that cannot be rewritten is refused before anything is created. */
        return false;
      }
      /* No stack yet: adding the first layer *is* the plan. The Base Color terminal is the only
       * thing the graph has to already have, and only the bottom position (0) is free -- the first
       * layer is a bare Image Texture, which nothing can be inserted below. */
      if (add_params->ordinal > 0) {
        r_error = PaintMaterialLayerEditError::IndexOutOfRange;
        return false;
      }
      if (paint_material_channel_socket_find(ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
        r_error = PaintMaterialLayerEditError::NoPrincipled;
        return false;
      }
      r_plan.layer_index = 0;
      return true;
    }
    if (!chains_align(flat, r_error)) {
      return false;
    }
    /* Held as a one-channel-per-entry forest so the chains below resolve the same way for every
     * operation; an add only ever touches the top level. */
    r_plan.per_channel.append(std::move(flat));
    /* The shape conversion walks the forest, so its reach is what the forest says: a forest the
     * reader refuses is a conversion that never happens. */
    Vector<Vector<ChannelChain>> forest;
    PaintMaterialLayerEditError forest_error = PaintMaterialLayerEditError::None;
    r_plan.needs_bottom_normalize = chains_collect_forest(ma, forest, forest_error) &&
                                    forest_has_bare_bottom(forest);
  }
  else {
    if (!chains_collect_forest(ma, r_plan.per_channel, r_error)) {
      return false;
    }
    r_plan.needs_bottom_normalize = forest_has_bare_bottom(r_plan.per_channel);
  }
  BLI_assert(op != LayerEditOp::Add || add_params != nullptr);

  Vector<ChannelChain *> top_chains;
  forest_top_chains(r_plan.per_channel, top_chains);

  auto top_layers_num = [&]() {
    return int(top_chains.first()->layers.size());
  };
  auto top_chains_aligned = [&]() {
    const int64_t layer_num = top_chains.first()->layers.size();
    for (ChannelChain *chain : top_chains) {
      if (chain->layers.size() != layer_num) {
        r_error = PaintMaterialLayerEditError::ChannelsDisagree;
        return false;
      }
    }
    return true;
  };
  /* A position-0 edit on a chain whose bottom is still a bare image is a conversion of nodes
   * rather than a relink of links, and the operations refuse it rather than converting as a side
   * effect. */
  auto bottom_is_bare = [](const Vector<ChannelChain *> &chains) {
    return !chains.first()->layers.first().is_mix();
  };
  /* The write right on every tree the operation may write to, checked before the first byte is
   * written. Each chain the operation touches contributes its own -- a row inside a group writes
   * to the group's tree, and a move writes to both the chain it leaves and the one it lands in.
   * When the shape still has to be brought to contract, the trees holding bare bottoms join the
   * set, since the conversion writes there too. */
  auto write_scope_ok = [&]() {
    Vector<bNodeTree *> write_trees;
    for (ChannelChain *chain : r_plan.chains) {
      write_trees.append_non_duplicates(chain->tree);
    }
    for (ChannelChain *chain : r_plan.target_chains) {
      write_trees.append_non_duplicates(chain->tree);
    }
    if (r_plan.needs_bottom_normalize) {
      for (const Vector<ChannelChain> &chains : r_plan.per_channel) {
        for (const ChannelChain &chain : chains) {
          if (!chain.layers.is_empty() && !chain.layers.first().is_mix()) {
            write_trees.append_non_duplicates(chain.tree);
          }
        }
      }
    }
    for (bNodeTree *tree : write_trees) {
      if (!tree_write_scope_check(bmain, ma, *tree, r_error)) {
        return false;
      }
    }
    return true;
  };

  switch (op) {
    case LayerEditOp::Add: {
      if (add_by_anchor) {
        bool into_empty_group = false;
        Vector<AddEmptyGroupInstance> empty_group_instances;
        if (!add_anchor_resolve(r_plan.per_channel,
                                add_params->anchor_ordinal,
                                r_plan.chains,
                                r_plan.layer_index,
                                into_empty_group,
                                empty_group_instances,
                                r_error))
        {
          return false;
        }
        r_plan.target_is_empty_group = into_empty_group;
        if (into_empty_group) {
          r_plan.add_empty_group_instances = std::move(empty_group_instances);
          /* The write right on every empty folder tree the first layer is created in, plus the
           * trees a pending shape conversion touches -- the same set #write_scope_ok would
           * assemble, built here because that lambda reads from #r_plan.chains, which an empty
           * folder leaves empty. */
          Vector<bNodeTree *> write_trees;
          for (const AddEmptyGroupInstance &inst : r_plan.add_empty_group_instances) {
            bNodeTree *group_tree = layer_group_tree_of(*inst.instance);
            if (group_tree == nullptr) {
              r_error = PaintMaterialLayerEditError::ChainNotPlain;
              return false;
            }
            write_trees.append_non_duplicates(group_tree);
          }
          if (r_plan.needs_bottom_normalize) {
            for (const Vector<ChannelChain> &chains : r_plan.per_channel) {
              for (const ChannelChain &chain : chains) {
                if (!chain.layers.is_empty() && !chain.layers.first().is_mix()) {
                  write_trees.append_non_duplicates(chain.tree);
                }
              }
            }
          }
          for (bNodeTree *write_tree : write_trees) {
            if (!tree_write_scope_check(bmain, ma, *write_tree, r_error)) {
              return false;
            }
          }
          return true;
        }
        if (r_plan.layer_index == 0 && bottom_is_bare(r_plan.chains)) {
          r_error = PaintMaterialLayerEditError::IsBottomLayer;
          return false;
        }
        if (r_plan.layer_index > int(r_plan.chains.first()->layers.size())) {
          r_error = PaintMaterialLayerEditError::IndexOutOfRange;
          return false;
        }
        return write_scope_ok();
      }
      if (!top_chains_aligned()) {
        return false;
      }
      if (add_params->ordinal >= 0) {
        if (!ordinal_is_in_chain(add_params->ordinal, r_error)) {
          return false;
        }
        const int insert_at = add_params->ordinal;
        if (insert_at == 0 && bottom_is_bare(top_chains)) {
          /* Going below a bare base would mean turning it into a Mix node, which is a conversion
           * rather than a relink. Below the lowest layer of a uniform chain is just another
           * position. */
          r_error = PaintMaterialLayerEditError::IsBottomLayer;
          return false;
        }
        if (insert_at > top_layers_num()) {
          r_error = PaintMaterialLayerEditError::IndexOutOfRange;
          return false;
        }
      }
      r_plan.chains = top_chains;
      r_plan.layer_index = (add_params->ordinal < 0) ? top_layers_num() : add_params->ordinal;
      return write_scope_ok();
    }

    case LayerEditOp::Remove: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      if (r_plan.chains.first()->layers.size() == 1) {
        /* Removing the only layer would leave the channel unwired, which is a different operation
         * (unassigning the material's texture) than removing a layer from a stack. */
        r_error = PaintMaterialLayerEditError::IsBottomLayer;
        return false;
      }
      return write_scope_ok();
    }

    case LayerEditOp::Rename:
    case LayerEditOp::SetEnabled:
    case LayerEditOp::MaskAdd:
    case LayerEditOp::MaskRemove: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      if (r_plan.layer_index == 0 && bottom_is_bare(r_plan.chains)) {
        /* A bare base has no Mix node to carry a name, a mute or a mask. */
        r_error = PaintMaterialLayerEditError::IsBottomLayer;
        return false;
      }
      return write_scope_ok();
    }

    case LayerEditOp::ChannelImageSet: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      /* A bare base is deliberately accepted here, unlike a rename or a mask: the base is its
       * channel's own map, and setting an image on it is replacing that map. Nothing in this
       * operation needs a Mix node, so #needs_bottom_normalize stays advisory and the mutation
       * ignores it. */
      return write_scope_ok();
    }

    case LayerEditOp::FillColorSet: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      /* A bare base is accepted, the way #ChannelImageSet accepts one: a Fill added as the very
       * first layer of a stack is a bare Image Texture carrying the kind marker itself. Whether
       * the row actually *is* a Fill is the mutation's check, against the marker. */
      return write_scope_ok();
    }

    case LayerEditOp::KindSet: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      /* A bare base is accepted, the way #FillColorSet accepts one: the marker is what makes a
       * bare Image Texture read as anything other than a Paint layer. */
      return write_scope_ok();
    }

    case LayerEditOp::Duplicate: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      if (r_plan.layer_index == 0 && bottom_is_bare(r_plan.chains)) {
        /* A bare base has no Mix node; copying it would have to make one. */
        r_error = PaintMaterialLayerEditError::IsBottomLayer;
        return false;
      }
      return write_scope_ok();
    }

    case LayerEditOp::GroupMake: {
      if (!top_chains_aligned()) {
        return false;
      }
      if (!ordinal_is_in_chain(ordinal, r_error) ||
          !ordinal_is_in_chain(target_ordinal, r_error))
      {
        return false;
      }
      if (ordinal < 0 || target_ordinal >= top_layers_num() || target_ordinal < ordinal) {
        r_error = PaintMaterialLayerEditError::IndexOutOfRange;
        return false;
      }
      if (ordinal == 0 && bottom_is_bare(top_chains)) {
        /* A bare base is the channel's own map: it has no Mix node to become the group's. */
        r_error = PaintMaterialLayerEditError::IsBottomLayer;
        return false;
      }
      /* Every map the group will hold, resolved here: a mapless layer in the range is a refusal,
       * not a half-built group. */
      for (ChannelChain *chain : top_chains) {
        if (chain_range_map_nodes(*chain, ordinal, target_ordinal).is_empty()) {
          r_error = PaintMaterialLayerEditError::ChainNotPlain;
          return false;
        }
      }
      r_plan.chains = top_chains;
      r_plan.layer_index = ordinal;
      r_plan.target_index = target_ordinal;
      return write_scope_ok();
    }

    case LayerEditOp::GroupAdd: {
      if (ordinal < 0) {
        if (!top_chains_aligned()) {
          return false;
        }
        r_plan.chains = top_chains;
        r_plan.target_index = top_layers_num();
      }
      else {
        int layer_index = -1;
        if (!forest_rows_resolve(
                r_plan.per_channel, ordinal, r_plan.chains, layer_index, r_error))
        {
          return false;
        }
        /* Above the row it was given is the position after it in a chain listed bottom to top;
         * below it is the row's own position, which pushes that row up. */
        r_plan.target_index = (move_place == PaintMaterialLayerMovePlace::Below) ?
                                  layer_index :
                                  layer_index + 1;
      }
      if (r_plan.target_index == 0 && bottom_is_bare(r_plan.chains)) {
        /* The same rule #LayerEditOp::Add follows: going below a bare base would mean turning it
         * into a Mix node, which is a conversion rather than a relink. */
        r_error = PaintMaterialLayerEditError::IsBottomLayer;
        return false;
      }
      if (r_plan.target_index < 0 ||
          r_plan.target_index > int(r_plan.chains.first()->layers.size()))
      {
        r_error = PaintMaterialLayerEditError::IndexOutOfRange;
        return false;
      }
      return write_scope_ok();
    }

    case LayerEditOp::Ungroup: {
      if (!top_chains_aligned()) {
        return false;
      }
      if (!ordinal_is_in_chain(ordinal, r_error)) {
        return false;
      }
      if (ordinal < 1 || ordinal >= top_layers_num()) {
        r_error = PaintMaterialLayerEditError::IndexOutOfRange;
        return false;
      }
      for (ChannelChain *chain : top_chains) {
        ChainLayer &keeper = chain->layers[ordinal];
        bNodeLink *top_link = (keeper.top == nullptr) ? nullptr : sole_link_into(*keeper.top);
        if (top_link == nullptr || !BKE_paint_material_is_layer_group(*top_link->fromnode)) {
          r_error = PaintMaterialLayerEditError::NotAStack;
          return false;
        }
      }
      r_plan.chains = top_chains;
      r_plan.layer_index = ordinal;
      return write_scope_ok();
    }

    case LayerEditOp::Reorder:
    case LayerEditOp::Move: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      bNodeTree *empty_group_tree = nullptr;
      if (op == LayerEditOp::Reorder) {
        if (!forest_rows_resolve(r_plan.per_channel,
                                 target_ordinal,
                                 r_plan.target_chains,
                                 r_plan.target_index,
                                 r_error))
        {
          return false;
        }
      }
      else {
        if (!forest_rows_resolve(r_plan.per_channel,
                                 target_ordinal,
                                 r_plan.target_chains,
                                 r_plan.target_index,
                                 r_error))
        {
          return false;
        }
        if (move_place == PaintMaterialLayerMovePlace::Into) {
          /* The anchor is the folder itself, so the destination is the chain it opens -- and a
           * folder with nothing in it opens none yet. */
          const ChainLayer &group_layer = r_plan.target_chains.first()->layers[r_plan.target_index];
          if (!group_layer.is_group) {
            r_error = PaintMaterialLayerEditError::ChainNotPlain;
            return false;
          }
          if (group_layer.sub_chain_index < 0) {
            const bNode *instance = composite_source_node_shallow(*group_layer.top);
            empty_group_tree = (instance == nullptr) ? nullptr : layer_group_tree_of(*instance);
            if (empty_group_tree == nullptr ||
                empty_group_tree == r_plan.chains.first()->tree)
            {
              r_error = PaintMaterialLayerEditError::ChainNotPlain;
              return false;
            }
            r_plan.target_is_empty_group = true;
          }
          else {
            const int anchor_index = r_plan.target_index;
            for (const int64_t i : r_plan.target_chains.index_range()) {
              const int sub_index = r_plan.target_chains[i]->layers[anchor_index].sub_chain_index;
              if (sub_index < 0 || sub_index >= int(r_plan.per_channel[i].size())) {
                r_error = PaintMaterialLayerEditError::ChannelsDisagree;
                return false;
              }
              r_plan.target_chains[i] = &r_plan.per_channel[i][sub_index];
            }
            /* On top of what the group holds. */
            r_plan.target_index = int(r_plan.target_chains.first()->layers.size());
          }
        }
        else {
          int to_index = r_plan.target_index +
                         ((move_place == PaintMaterialLayerMovePlace::Above) ? 1 : 0);
          /* The layer leaves its own chain before it is put back, so every position above it moves
           * down by one; a move into another chain takes nothing out of the destination. */
          if (r_plan.chains.first() == r_plan.target_chains.first() &&
              r_plan.layer_index < to_index)
          {
            to_index--;
          }
          r_plan.target_index = to_index;
        }
      }

      /* The checks #layer_move_apply would make before its first write, made here instead: a move
       * refused for its shape must not leave a converted bottom behind. */
      const bool same_chain = (r_plan.chains.first() == r_plan.target_chains.first());
      for (const int64_t i : r_plan.chains.index_range()) {
        if ((r_plan.chains[i] == r_plan.target_chains[i]) != same_chain) {
          r_error = PaintMaterialLayerEditError::ChannelsDisagree;
          return false;
        }
      }
      if ((r_plan.layer_index == 0 && bottom_is_bare(r_plan.chains)) ||
          (!r_plan.target_is_empty_group && r_plan.target_index == 0 &&
           bottom_is_bare(r_plan.target_chains)))
      {
        r_error = PaintMaterialLayerEditError::IsBottomLayer;
        return false;
      }
      if (r_plan.target_is_empty_group) {
        /* Landing in an empty group: the moved group must not end up inside itself, and not deeper
         * than the readers walk. The destination's nesting is the anchor chain's. */
        if (r_plan.chains.first()->layers[r_plan.layer_index].is_group) {
          if (!moving_group_check(r_plan.chains,
                                  r_plan.layer_index,
                                  *empty_group_tree,
                                  r_plan.target_chains.first()->nesting,
                                  r_error))
          {
            return false;
          }
        }
      }
      else {
        const int to_num = int(r_plan.target_chains.first()->layers.size());
        if (r_plan.target_index > (same_chain ? to_num - 1 : to_num)) {
          r_error = PaintMaterialLayerEditError::IndexOutOfRange;
          return false;
        }
        if (!same_chain && r_plan.chains.first()->layers[r_plan.layer_index].is_group) {
          if (!moving_group_check(r_plan.chains,
                                  r_plan.layer_index,
                                  *r_plan.target_chains.first()->tree,
                                  r_plan.target_chains.first()->nesting,
                                  r_error))
          {
            return false;
          }
        }
      }
      return write_scope_ok();
    }
  }
  BLI_assert_unreachable();
  return false;
}

}  // namespace

bUUID BKE_paint_material_layer_marker_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return BLI_uuid_nil();
  }
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(node.prop, LAYER_MARKER_PROP, IDP_STRING);
  if (marker == nullptr) {
    return BLI_uuid_nil();
  }
  bUUID uuid = BLI_uuid_nil();
  if (!BLI_uuid_parse_string(&uuid, IDP_string_get(marker))) {
    return BLI_uuid_nil();
  }
  return uuid;
}

void BKE_paint_material_layer_marker_set(bNode &node, const bUUID &layer_id)
{
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, layer_id);

  IDProperty *properties = node_properties_ensure(node);
  IDProperty *marker = IDP_GetPropertyTypeFromGroup(properties, LAYER_MARKER_PROP, IDP_STRING);
  if (marker != nullptr) {
    IDP_AssignString(marker, formatted);
    return;
  }
  IDP_AddToGroup(properties, IDP_NewString(formatted, LAYER_MARKER_PROP));
}

int BKE_paint_material_layer_color_tag_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return -1;
  }
  const IDProperty *color_tag = IDP_GetPropertyTypeFromGroup(
      node.prop, LAYER_COLOR_TAG_PROP, IDP_INT);
  if (color_tag == nullptr) {
    return -1;
  }
  return IDP_int_get(color_tag);
}

void BKE_paint_material_layer_color_tag_set(bNode &node, const int color_tag)
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *tag_prop = IDP_GetPropertyTypeFromGroup(properties, LAYER_COLOR_TAG_PROP, IDP_INT);
  if (tag_prop != nullptr) {
    IDP_int_set(tag_prop, color_tag);
    return;
  }
  IDP_AddToGroup(properties, IDP_NewInt(color_tag, LAYER_COLOR_TAG_PROP));
}

PaintMaterialLayerKind BKE_paint_material_layer_kind_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return PaintMaterialLayerKind::Paint;
  }
  const IDProperty *kind = IDP_GetPropertyTypeFromGroup(node.prop, LAYER_KIND_PROP, IDP_INT);
  if (kind == nullptr) {
    return PaintMaterialLayerKind::Paint;
  }
  return static_cast<PaintMaterialLayerKind>(IDP_int_get(kind));
}

void BKE_paint_material_layer_kind_set(bNode &node, const PaintMaterialLayerKind kind)
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *kind_prop = IDP_GetPropertyTypeFromGroup(properties, LAYER_KIND_PROP, IDP_INT);
  if (kind_prop != nullptr) {
    IDP_int_set(kind_prop, int8_t(kind));
    return;
  }
  IDP_AddToGroup(properties, IDP_NewInt(int8_t(kind), LAYER_KIND_PROP));
}

bool BKE_paint_material_layer_fill_color_get(const bNode &node, float r_color[4])
{
  if (node.prop == nullptr) {
    return false;
  }
  const IDProperty *fill = IDP_GetPropertyTypeFromGroup(
      node.prop, LAYER_FILL_COLOR_PROP, IDP_ARRAY);
  if (fill == nullptr || fill->subtype != IDP_FLOAT || fill->len != 4) {
    return false;
  }
  copy_v4_v4(r_color,
             static_cast<const float *>(IDP_array_voidp_get(const_cast<IDProperty *>(fill))));
  return true;
}

void BKE_paint_material_layer_fill_color_set(bNode &node, const float color[4])
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *fill = IDP_GetPropertyTypeFromGroup(properties, LAYER_FILL_COLOR_PROP, IDP_ARRAY);
  if (fill != nullptr && fill->subtype == IDP_FLOAT && fill->len == 4) {
    copy_v4_v4(static_cast<float *>(IDP_array_voidp_get(fill)), color);
    return;
  }
  if (fill != nullptr) {
    /* A stale array of the wrong shape is replaced rather than reused. */
    IDP_RemoveFromGroup(properties, fill);
    IDP_FreeProperty(fill);
  }
  IDP_AddToGroup(properties,
                 bke::idprop::create(LAYER_FILL_COLOR_PROP, Span<float>(color, 4)).release());
}

const char *BKE_paint_material_layer_edit_error_message(const PaintMaterialLayerEditError error)
{
  switch (error) {
    case PaintMaterialLayerEditError::None:
      return "";
    case PaintMaterialLayerEditError::NotEditable:
      return "Material or node tree is linked or overridden";
    case PaintMaterialLayerEditError::NotAStack:
      return "Material is not a paint layer stack";
    case PaintMaterialLayerEditError::IndexOutOfRange:
      return "No such paint layer";
    case PaintMaterialLayerEditError::IsBottomLayer:
      return "The bottom layer has nothing to blend with and cannot be moved";
    case PaintMaterialLayerEditError::ChainIsShared:
      return "A layer's result is used elsewhere in the node tree";
    case PaintMaterialLayerEditError::ChainNotPlain:
      return "The node chain contains nodes that are not paint layers";
    case PaintMaterialLayerEditError::ChannelsDisagree:
      return "The material's channels have different numbers of layers";
    case PaintMaterialLayerEditError::NoPrincipled:
      return "The material has no Principled BSDF to build a paint layer stack on";
    case PaintMaterialLayerEditError::CreationFailed:
      return "The paint layer's image or nodes could not be created";
    case PaintMaterialLayerEditError::HasGroups:
      return "Layers inside a group cannot be edited yet; ungroup it first";
    case PaintMaterialLayerEditError::NestingTooDeep:
      return "Groups cannot be nested any deeper";
    case PaintMaterialLayerEditError::TreeNotEditable:
      return "The layer group's node tree is linked and cannot be edited";
    case PaintMaterialLayerEditError::TreeIsOverride:
      return "The layer group's node tree is a library override and cannot be edited";
    case PaintMaterialLayerEditError::TreeShared:
      return "The layer group's node tree is used by another material";
    case PaintMaterialLayerEditError::ChannelHasUnsupportedSource:
      return "A needed channel is wired to nodes that are not paint layers; refusing to rewire it";
  }
  return "";
}

/**
 * The Mix node a layer of \a channel blends with, freshly created in \a tree.
 *
 * The Normal channel is the exception the whole file makes: tangent-space maps do not blend
 * component-wise, so the engine's own group does it instead of a Mix node.
 */
static bNode *layer_mix_node_create(Main &bmain, bNodeTree &tree, const int channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    bNodeTree *group = BKE_paint_material_normal_combine_group_ensure(bmain);
    if (group == nullptr) {
      return nullptr;
    }
    bNode *node = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
    if (node != nullptr) {
      node->id = &group->id;
      /* Unlike a layer's own map, this group is shared: #..._group_ensure hands the same tree to
       * every Normal layer in the file. Each instance is a user of its own, and a tree that
       * counted only the first would drop to zero when that first layer is removed -- while every
       * other instance still points at it. Nothing recomputes these counts between a file read
       * and a file write. */
      id_us_plus(node->id);
      /* A group assigned by hand only grows its sockets once the updater is told the node's
       * group changed; a plain tree update does not notice. */
      BKE_ntree_update_tag_node_property(&tree, node);
    }
    return node;
  }
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX);
  if (node == nullptr) {
    return nullptr;
  }
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(node->storage);
  storage->data_type = SOCK_RGBA;
  storage->factor_mode = NODE_MIX_MODE_UNIFORM;
  storage->blend_type = MA_RAMP_BLEND;
  return node;
}

/**
 * Give \a chain's bare Image Texture bottom a Mix node of its own.
 *
 * The chain is re-collected by the caller afterwards: creating a node invalidates the topology
 * cache, and a group instance does not even have its sockets until the tree has been updated.
 */
static bool chain_bottom_convert(Main &bmain,
                                 ChannelChain &chain,
                                 PaintMaterialLayerEditError &r_error)
{
  bNodeTree &tree = *chain.tree;
  bNode *image_node = chain.layers.first().node;
  const int channel = chain.channel;

  bNode *mix_node = layer_mix_node_create(bmain, tree, channel);
  if (mix_node == nullptr) {
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  /* The chain the caller collected described the tree before the node existed. */
  ChannelChain fresh;
  fresh.channel = channel;
  if (!chain_collect(tree, *chain.terminal, fresh, r_error)) {
    bke::node_remove_node(&bmain, tree, *mix_node, true);
    return false;
  }

  CompositeMixNode mix;
  bNodeSocket *mix_out = mix_output_find(*mix_node);
  bNodeSocket *image_color = bke::node_find_socket(*image_node, SOCK_OUT, "Color"_ustr);
  bNodeSocket *image_alpha = bke::node_find_socket(*image_node, SOCK_OUT, "Alpha"_ustr);
  if (mix_out == nullptr || image_color == nullptr || image_alpha == nullptr ||
      !composite_mix_node_read(*mix_node, mix) || fresh.layers.first().node != image_node)
  {
    bke::node_remove_node(&bmain, tree, *mix_node, true);
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }

  /* Whatever read the bare image now reads the Mix node, and the image becomes what that Mix
   * blends over transparency -- the shape every other layer already has. */
  if (fresh.layers.size() > 1) {
    ChainLayer &above = fresh.layers[1];
    relink_into(tree, *above.bottom, *above.node, *mix_node, *mix_out);
  }
  else {
    relink_into(tree, *fresh.terminal, *fresh.terminal_node, *mix_node, *mix_out);
  }
  relink_into(tree, *const_cast<bNodeSocket *>(mix.top), *mix_node, *image_node, *image_color);
  /* The layer covers what is below it exactly where its own map is opaque, like every other --
   * and, since this Mix node is newly created, keeps a real opacity alongside that coverage from
   * the start, rather than a bare link with nothing left to edit. */
  if (!layer_factor_coverage_link(tree,
                                  *mix_node,
                                  *const_cast<bNodeSocket *>(mix.factor),
                                  *image_node,
                                  *image_alpha,
                                  1.0f))
  {
    bke::node_remove_node(&bmain, tree, *mix_node, true);
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }
  bke::node_position_relative(*mix_node, *image_node, mix_out, *image_color);

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  return true;
}

bool BKE_paint_material_layer_bottom_normalize(Main &bmain, Material &ma)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return false;
  }

  /* Write scope, checked up front: the conversion writes wherever a bare bottom lives, including
   * inside group trees, and a linked or shared group is never half-converted. */
  {
    Vector<Vector<ChannelChain>> scope_forest;
    if (chains_collect_forest(ma, scope_forest, error)) {
      for (const Vector<ChannelChain> &chains : scope_forest) {
        for (const ChannelChain &chain : chains) {
          if (!chain.layers.is_empty() && !chain.layers.first().is_mix() &&
              !tree_write_scope_check(bmain, ma, *chain.tree, error))
          {
            return false;
          }
        }
      }
    }
  }

  bool changed = false;
  /* One conversion per pass: it rewrites links and adds a node, which the collected chains of
   * every channel describe from before. The count of chains bounds the loop. */
  for (int guard = 0; guard < 64; guard++) {
    Vector<Vector<ChannelChain>> per_channel;
    if (!chains_collect_forest(ma, per_channel, error)) {
      break;
    }
    ChannelChain *bare = nullptr;
    for (Vector<ChannelChain> &chains : per_channel) {
      for (ChannelChain &chain : chains) {
        if (!chain.layers.is_empty() && !chain.layers.first().is_mix()) {
          bare = &chain;
          break;
        }
      }
      if (bare != nullptr) {
        break;
      }
    }
    if (bare == nullptr) {
      break;
    }
    if (!chain_bottom_convert(bmain, *bare, error)) {
      break;
    }
    changed = true;
  }

  if (changed) {
    paint_layer_edit_committed(bmain, ma, true);
  }
  return changed;
}

bool BKE_paint_material_layer_markers_ensure(Material &ma)
{
  /* The markers are id-properties of the Mix nodes of the top-level chains, all of which live in
   * the material's own node tree; a tree that may not be written gets no markers. */
  if (ma.nodetree == nullptr || !ID_IS_EDITABLE(&ma.nodetree->id) ||
      ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return false;
  }
  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  if (!chains_collect(ma, chains, error) || !chains_align(chains, error)) {
    return false;
  }

  const int64_t layer_num = chains.first().layers.size();
  for (const int64_t i : IndexRange(layer_num)) {
    if (!chains.first().layers[i].is_mix()) {
      /* The bottom layer is a bare image; it has no Mix node to carry a marker. */
      continue;
    }
    /* An existing marker wins, so that a stack edited before keeps its identity. */
    bUUID layer_id = BLI_uuid_nil();
    for (const ChannelChain &chain : chains) {
      const bUUID candidate = BKE_paint_material_layer_marker_get(*chain.layers[i].node);
      if (!BLI_uuid_is_nil(candidate)) {
        layer_id = candidate;
        break;
      }
    }
    if (BLI_uuid_is_nil(layer_id)) {
      layer_id = BLI_uuid_generate_random();
    }
    for (const ChannelChain &chain : chains) {
      BKE_paint_material_layer_marker_set(*chain.layers[i].node, layer_id);
    }
  }
  return true;
}

bool BKE_paint_material_layer_add(Main &bmain,
                                  Material &ma,
                                  const PaintMaterialLayerAddParams &params,
                                  int *r_ordinal,
                                  PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  /* The maps the caller handed over are this call's from the start. The ones a surviving node shows
   * are recorded as they are assigned; every exit frees the rest, so no path -- a refusal in the
   * preflight, a node that could not be created, a channel the layer is not added to -- can leave
   * one in the file with its creation user and nothing showing it. */
  Set<Image *> given_used;
  auto given_release = [&]() {
    Set<Image *> released;
    for (const PaintMaterialLayerChannelImage &given : params.channel_images) {
      if (given.image != nullptr && !given_used.contains(given.image) &&
          released.add(given.image))
      {
        BKE_id_free(&bmain, given.image);
      }
    }
  };
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    /* Nodes that showed a handed-over map were discarded before this, so none of them is used. */
    given_used.clear();
    given_release();
    if (r_error != nullptr) {
      *r_error = reason;
    }
    /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
    printf("[MAT_LAYER]   layer_add FAIL err=%d\n", int(reason));
    fflush(stdout);
    return false;
  };
  auto succeed = [&](const int ordinal) {
    given_release();
    if (r_ordinal != nullptr) {
      *r_ordinal = ordinal;
    }
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
    paint_layer_edit_committed(bmain, ma, true);
    return true;
  };

  /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
  printf("[MAT_LAYER]   layer_add enter type=%d ordinal=%d anchor=%d channel_images=%d\n",
         int(params.type), params.ordinal, params.anchor_ordinal,
         int(params.channel_images.size()));
  fflush(stdout);

  /* 1. Preflight: whether a layer can be added, and where -- decided without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma,
                             params.ordinal,
                             LayerEditOp::Add,
                             plan,
                             error,
                             -1,
                             PaintMaterialLayerMovePlace::Above,
                             &params))
  {
    /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
    printf("[MAT_LAYER]   layer_add plan_build FAIL err=%d\n", int(error));
    fflush(stdout);
    return fail(error);
  }
  /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
  printf("[MAT_LAYER]   layer_add plan ok per_channel=%d chains=%d needs_norm=%d empty_group=%d\n",
         int(plan.per_channel.size()), int(plan.chains.size()),
         int(plan.needs_bottom_normalize), int(plan.target_is_empty_group));
  fflush(stdout);
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * add is known to happen. The conversion changes the chains, so the plan is read again
   * afterwards -- its ordinals survive, its pointers do not. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma,
                                 params.ordinal,
                                 LayerEditOp::Add,
                                 plan,
                                 error,
                                 -1,
                                 PaintMaterialLayerMovePlace::Above,
                                 &params))
      {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */

  const bool add_by_anchor = params.anchor_ordinal >= 0;
  const bool into_empty_group = plan.target_is_empty_group;
  /* The first layer of an empty folder is created in that folder's own node tree; a layer added
   * next to a row that lives inside a folder is created in that row's tree; every other add is in
   * the material's own tree. */
  bNodeTree &tree = into_empty_group ?
                        *layer_group_tree_of(*plan.add_empty_group_instances.first().instance) :
                        (plan.chains.is_empty() ? *ma.nodetree : *plan.chains.first()->tree);
  Vector<NewLayerNodes> added;

  if (plan.per_channel.is_empty()) {
    /* Nothing to blend over yet: the first layer is a bare Image Texture on Base Color. The other
     * channels get their maps when a brush first writes to them. */
    const bNodeSocket *terminal = paint_material_channel_socket_find(
        ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (terminal == nullptr) {
      return fail(PaintMaterialLayerEditError::NoPrincipled);
    }

    /* Resolved before anything is created: adding a node invalidates the topology cache, and the
     * owner of a socket cannot be asked for once it is gone. */
    tree.ensure_topology_cache();
    bNode &terminal_node = const_cast<bNode &>(terminal->owner_node());

    NewLayerNodes base;
    base.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    if (Image *given = layer_image_given(params, base.channel)) {
      base.image = given;
      base.owns_image = false;
      /* The base carries no layer marker, so its map carries no layer identity either -- the
       * same rule #BKE_paint_material_layer_channel_image_set applies to a base's map. */
      base.image->paint_layer_id = bUUID{};
    }
    else {
      base.image = layer_image_create(bmain, base.channel, params);
    }
    if (base.image == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    base.tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    added.append(base);

    bNodeSocket *color = bke::node_find_socket(*base.tex, SOCK_OUT, "Color"_ustr);
    if (color == nullptr) {
      new_layer_nodes_discard(bmain, tree, added);
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    /* Only assign the image once the node is known to be usable, so a failed setup cannot leave a
     * node pointing at an ID about to be freed. */
    base.tex->id = &base.image->id;
    if (!base.owns_image) {
      given_used.add(base.image);
    }
    if (params.type == PaintMaterialLayerAddType::Fill) {
      /* The first layer of a stack is a bare Image Texture with no Mix node; the kind marker lives
       * on the node the row reads as, which here is the texture itself. */
      BKE_paint_material_layer_kind_set(*base.tex, PaintMaterialLayerKind::Fill);
      BKE_paint_material_layer_fill_color_set(*base.tex, params.fill_color);
    }
    bke::node_position_relative(
        *base.tex, terminal_node, nullptr, *const_cast<bNodeSocket *>(terminal));
    relink_into(
        tree, *const_cast<bNodeSocket *>(terminal), terminal_node, *base.tex, *color);
    return succeed(0);
  }

  int insert_at = plan.layer_index;

  /* The channels the new layer is created for: one per chain it is inserted into, or -- for an
   * empty folder, which has no chain yet -- one per folder instance. */
  Vector<int> new_channels;
  if (into_empty_group) {
    for (const AddEmptyGroupInstance &inst : plan.add_empty_group_instances) {
      new_channels.append(inst.channel);
    }
  }
  else {
    for (const ChannelChain *chain_ptr : plan.chains) {
      new_channels.append(chain_ptr->channel);
    }
  }

  /* Create every node first. Group nodes get their sockets from a tree update, so nothing may be
   * linked before that update has run. */
  for (const int channel : new_channels) {
    NewLayerNodes nodes;
    nodes.channel = channel;
    if (Image *given = layer_image_given(params, channel)) {
      nodes.image = given;
      nodes.owns_image = false;
      /* A handed-over map is a layer canvas just like one #layer_image_create makes; the ID
       * browser's paint-canvas view keys off this flag together with #paint_layer_id. */
      nodes.image->flag |= IMA_PAINT_CANVAS;
    }
    else {
      nodes.image = layer_image_create(bmain, channel, params);
    }
    if (nodes.image == nullptr) {
      added.append(nodes);
      new_layer_nodes_discard(bmain, tree, added);
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    nodes.tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    nodes.tex->id = &nodes.image->id;
    if (!nodes.owns_image) {
      /* Released again by `fail` if the add is refused after this point. */
      given_used.add(nodes.image);
    }
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      /* Tangent-space maps do not blend component-wise; the engine's own group does it properly. */
      bNodeTree *group = BKE_paint_material_normal_combine_group_ensure(bmain);
      nodes.mix = (group == nullptr) ?
                      nullptr :
                      bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (nodes.mix != nullptr) {
        nodes.mix->id = &group->id;
        /* Shared between every Normal layer in the file, so each instance is a user of its own --
         * see #layer_mix_node_create, which creates the same node for the same reason. A texture
         * node needs no such call: its map is created for that one node and comes with the user
         * a fresh data-block carries. */
        id_us_plus(nodes.mix->id);
        /* Tell the updater the node's group changed, or the pass below leaves it socketless. */
        BKE_ntree_update_tag_node_property(&tree, nodes.mix);
      }
    }
    else {
      nodes.mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX);
      NodeShaderMix *storage = static_cast<NodeShaderMix *>(nodes.mix->storage);
      storage->data_type = SOCK_RGBA;
      storage->factor_mode = NODE_MIX_MODE_UNIFORM;
      storage->blend_type = MA_RAMP_BLEND;
    }
    added.append(nodes);
    if (nodes.mix == nullptr) {
      new_layer_nodes_discard(bmain, tree, added);
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    if (params.name != nullptr && params.name[0] != '\0') {
      STRNCPY_UTF8(nodes.mix->label, params.name);
    }
  }

  /* Sockets of a group instance (a Normal layer's combine group) only exist after this update. The
   * new nodes are not linked yet, so the chains this invalidates are re-read below. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  /* The chains the new nodes attach to, re-read now that any group instances have their sockets: a
   * flat re-read for a top-level add, a forest re-read for one placed relative to a row. An empty
   * folder has no chain -- its instance nodes carry the Result socket the first layer feeds. */
  Vector<ChannelChain> flat_chains;
  Vector<Vector<ChannelChain>> forest;
  Vector<ChannelChain *> target_chains;
  Vector<AddEmptyGroupInstance> fresh_instances;
  if (add_by_anchor) {
    bool fresh_into_empty = false;
    if (!chains_collect_forest(ma, forest, error) ||
        !add_anchor_resolve(forest,
                            params.anchor_ordinal,
                            target_chains,
                            insert_at,
                            fresh_into_empty,
                            fresh_instances,
                            error))
    {
      new_layer_nodes_discard(bmain, tree, added);
      return fail(error);
    }
  }
  else {
    if (!chains_collect(ma, flat_chains, error) || !chains_align(flat_chains, error)) {
      new_layer_nodes_discard(bmain, tree, added);
      return fail(error);
    }
    for (ChannelChain &chain : flat_chains) {
      target_chains.append(&chain);
    }
  }

  /* Resolve every socket before touching a link, so a node type that turned out not to match the
   * Mix contract cannot leave half a layer behind. */
  struct ResolvedLayer {
    /** The chain the layer is inserted into; null for the first layer of an empty folder. */
    ChannelChain *chain = nullptr;
    /** The folder's Result socket the layer feeds, for the first layer of an empty folder. */
    bNodeSocket *group_result = nullptr;
    bNode *group_result_node = nullptr;
    ChainLayer layer;
    bNode *tex = nullptr;
    bNodeSocket *tex_color = nullptr;
    bNodeSocket *tex_alpha = nullptr;
    bNodeSocket *factor = nullptr;
  };
  Vector<ResolvedLayer> resolved;
  for (NewLayerNodes &nodes : added) {
    CompositeMixNode mix;
    bNodeSocket *output = mix_output_find(*nodes.mix);
    bNodeSocket *tex_color = bke::node_find_socket(*nodes.tex, SOCK_OUT, "Color"_ustr);
    bNodeSocket *tex_alpha = bke::node_find_socket(*nodes.tex, SOCK_OUT, "Alpha"_ustr);
    const bool mix_read = composite_mix_node_read(*nodes.mix, mix);
    /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
    printf("[MAT_LAYER]   layer_add resolve ch=%d combine=%d output=%d texc=%d texa=%d "
           "mix_read=%d bottom=%d top=%d factor=%d\n",
           nodes.channel,
           int(BKE_paint_material_is_normal_combine_group(*nodes.mix)),
           int(output != nullptr), int(tex_color != nullptr), int(tex_alpha != nullptr),
           int(mix_read), int(mix.bottom != nullptr), int(mix.top != nullptr),
           int(mix.factor != nullptr));
    if (output == nullptr || !mix_read) {
      printf("[MAT_LAYER]   layer_add mix node='%s' id=%p type=%d in=[",
             nodes.mix->name, (void *)nodes.mix->id, int(nodes.mix->type_legacy));
      for (bNodeSocket &s : nodes.mix->inputs) {
        printf("%s/%s ", s.identifier, s.name);
      }
      printf("] out=[");
      for (bNodeSocket &s : nodes.mix->outputs) {
        printf("%s/%s ", s.identifier, s.name);
      }
      printf("]\n");
    }
    fflush(stdout);
    if (output == nullptr || tex_color == nullptr || tex_alpha == nullptr || !mix_read) {
      new_layer_nodes_discard(bmain, tree, added);
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    ResolvedLayer entry;
    entry.tex = nodes.tex;
    entry.factor = const_cast<bNodeSocket *>(mix.factor);
    entry.layer.node = nodes.mix;
    entry.layer.bottom = const_cast<bNodeSocket *>(mix.bottom);
    entry.layer.top = const_cast<bNodeSocket *>(mix.top);
    entry.layer.output = output;
    entry.layer.image = nodes.image;
    entry.tex_color = tex_color;
    entry.tex_alpha = tex_alpha;
    if (into_empty_group) {
      bNode *instance = nullptr;
      for (const AddEmptyGroupInstance &inst : fresh_instances) {
        if (inst.channel == nodes.channel) {
          instance = inst.instance;
          break;
        }
      }
      bNodeSocket *result = (instance == nullptr) ? nullptr :
                                                    group_result_socket(*instance, nodes.channel);
      if (result == nullptr) {
        new_layer_nodes_discard(bmain, tree, added);
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      entry.group_result = result;
      entry.group_result_node = &result->owner_node();
    }
    else {
      for (ChannelChain *candidate : target_chains) {
        if (candidate->channel == nodes.channel) {
          entry.chain = candidate;
          break;
        }
      }
      if (entry.chain == nullptr) {
        new_layer_nodes_discard(bmain, tree, added);
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
    }
    resolved.append(entry);
  }

  /* One identity for the whole layer, so a later reorder can move it in every channel at once. */
  BKE_paint_material_layer_markers_ensure(ma);
  const bUUID layer_id = BLI_uuid_generate_random();

  for (ResolvedLayer &entry : resolved) {
    bke::node_add_link(tree, *entry.tex, *entry.tex_color, *entry.layer.node, *entry.layer.top);
    /* The stack drives a layer's factor from its own Alpha: an unpainted texel must not cover
     * what is below it. A Multiply between the two keeps a real, editable opacity alongside that
     * coverage from the start, rather than a bare link with nothing left for a Value slider. */
    layer_factor_coverage_link(
        tree, *entry.layer.node, *entry.factor, *entry.tex, *entry.tex_alpha, 1.0f);
    if (into_empty_group) {
      /* The first layer of the folder blends over the transparency its own bottom socket holds,
       * and its output becomes what the folder contributes -- its Result. */
      bke::node_position_relative(*entry.layer.node,
                                  *entry.group_result_node,
                                  entry.layer.output,
                                  *entry.group_result);
      relink_into(tree,
                  *entry.group_result,
                  *entry.group_result_node,
                  *entry.layer.node,
                  *entry.layer.output);
    }
    else {
      bke::node_position_relative(*entry.layer.node,
                                  *entry.chain->terminal_node,
                                  entry.layer.output,
                                  *entry.chain->terminal);
      entry.chain->layers.insert(insert_at, entry.layer);
      chain_rebuild_links(*entry.chain);
    }
    bke::node_position_relative(*entry.tex, *entry.layer.node, entry.tex_color, *entry.layer.top);
    BKE_paint_material_layer_marker_set(*entry.layer.node, layer_id);
    if (params.type == PaintMaterialLayerAddType::Fill) {
      /* The kind and the colour live on the marker, not only in the pixels: a painted-over fill
       * map cannot be asked what it was filled with. */
      BKE_paint_material_layer_kind_set(*entry.layer.node, PaintMaterialLayerKind::Fill);
      BKE_paint_material_layer_fill_color_set(*entry.layer.node, params.fill_color);
    }
    if (entry.layer.image != nullptr) {
      entry.layer.image->paint_layer_id = layer_id;
    }
  }

  /* TEMP-DEBUG [MAT_LAYER]: remove before merge. Dump what each new layer's Mix node actually
   * ended up wired to, to settle whether bottom/top got swapped or the factor isn't what the
   * coverage link should have produced. */
  {
    tree.ensure_topology_cache();
    for (const ResolvedLayer &entry : resolved) {
      bNodeLink *bottom_link = (entry.layer.bottom == nullptr) ?
                                   nullptr :
                                   sole_link_into(*entry.layer.bottom);
      bNodeLink *top_link = (entry.layer.top == nullptr) ? nullptr :
                                                            sole_link_into(*entry.layer.top);
      CompositeMixNode mix_dbg;
      composite_mix_node_read(*entry.layer.node, mix_dbg);
      const float factor_default = (mix_dbg.factor != nullptr &&
                                    mix_dbg.factor->default_value != nullptr) ?
                                       static_cast<const bNodeSocketValueFloat *>(
                                           mix_dbg.factor->default_value)
                                           ->value :
                                       -1.0f;
      bNodeLink *factor_link = (mix_dbg.factor == nullptr) ?
                                   nullptr :
                                   sole_link_into(*const_cast<bNodeSocket *>(mix_dbg.factor));
      printf("[MAT_LAYER]   layer_add wired ch=%d mix='%s' bottom<-'%s' top<-'%s' "
             "factor_default=%.3f factor<-'%s'\n",
             entry.chain != nullptr ? entry.chain->channel : -1,
             entry.layer.node->name,
             bottom_link != nullptr ? bottom_link->fromnode->name : "(none)",
             top_link != nullptr ? top_link->fromnode->name : "(none)",
             double(factor_default),
             factor_link != nullptr ? factor_link->fromnode->name : "(none)");
      /* The Factor dump above only shows what feeds the Mix's Factor socket (the Math node
       * itself); it never looked inside that Math node to check which of its two inputs the
       * coverage (this layer's own Alpha) actually landed on, or what the other one holds. */
      if (factor_link != nullptr && factor_link->fromnode->type_legacy == SH_NODE_MATH) {
        bNode *math_node = factor_link->fromnode;
        bNodeSocket *value_a = static_cast<bNodeSocket *>(BLI_findlink(&math_node->inputs, 0));
        bNodeSocket *value_b = static_cast<bNodeSocket *>(BLI_findlink(&math_node->inputs, 1));
        bNodeLink *a_link = (value_a == nullptr) ? nullptr : sole_link_into(*value_a);
        bNodeLink *b_link = (value_b == nullptr) ? nullptr : sole_link_into(*value_b);
        const float a_default = (value_a != nullptr) ?
                                    static_cast<const bNodeSocketValueFloat *>(
                                        value_a->default_value)
                                        ->value :
                                    -1.0f;
        const float b_default = (value_b != nullptr) ?
                                    static_cast<const bNodeSocketValueFloat *>(
                                        value_b->default_value)
                                        ->value :
                                    -1.0f;
        printf("[MAT_LAYER]     math='%s' op=%d a_default=%.3f a<-'%s' b_default=%.3f b<-'%s'\n",
               math_node->name,
               math_node->custom1,
               double(a_default),
               a_link != nullptr ? a_link->fromnode->name : "(none)",
               double(b_default),
               b_link != nullptr ? b_link->fromnode->name : "(none)");
      }
      /* Whether the chain's actual terminal (the Principled input, or whatever a Normal Map's
       * Color feeds) ended up reading from this new layer at all -- #chain_rebuild_links is
       * supposed to relink it, but nothing upstream of this dump has verified that it did. */
      if (entry.chain != nullptr && entry.chain->terminal != nullptr) {
        bNodeLink *terminal_link = sole_link_into(*entry.chain->terminal);
        printf("[MAT_LAYER]   layer_add terminal ch=%d terminal_node='%s' terminal<-'%s' "
               "(expected '%s')\n",
               entry.chain->channel,
               entry.chain->terminal_node != nullptr ? entry.chain->terminal_node->name : "?",
               terminal_link != nullptr ? terminal_link->fromnode->name : "(none)",
               entry.layer.node->name);
      }
    }
    fflush(stdout);
  }

  if (&tree != ma.nodetree) {
    /* The new layer's links live in a folder's own node tree; `succeed` updates the material
     * tree, which the folder's output now feeds a changed value into. */
    BKE_ntree_update_after_single_tree_change(bmain, tree);
  }

  if (!add_by_anchor) {
    return succeed(insert_at);
  }
  /* The row just added reads as a #PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE-based ordinal when it
   * landed inside a folder. Its Mix node carries the layer's marker, so a fresh forest read finds
   * where it sits without re-deriving the anchor -- which now names a different, shifted row. */
  Vector<Vector<ChannelChain>> final_forest;
  if (chains_collect_forest(ma, final_forest, error) && !final_forest.is_empty()) {
    const Vector<ChannelChain> &channel_forest = final_forest.first();
    for (const int64_t chain_index : channel_forest.index_range()) {
      const ChannelChain &chain = channel_forest[chain_index];
      for (const int64_t layer_index : chain.layers.index_range()) {
        const bNode *node = chain.layers[layer_index].node;
        if (node != nullptr &&
            BLI_uuid_equal(BKE_paint_material_layer_marker_get(*node), layer_id))
        {
          const int ordinal = forest_ordinal_for_position(
              channel_forest, int(chain_index), int(layer_index));
          return succeed(ordinal >= 0 ? ordinal : insert_at);
        }
      }
    }
  }
  return succeed(insert_at);
}

/**
 * Move the layer at \a from_index of \a from_chains to \a to_index of \a to_chains.
 *
 * Shared by the two ways a caller can name the destination -- by the position the layer should end
 * up at, and by a row to land next to -- because everything past resolving that position is the
 * same work: within one chain the links are rewritten, across chains the nodes themselves have to
 * be copied into the other tree.
 *
 * \a to_index is a position in the destination chain with the moved layer already taken out of it,
 * so the callers resolve any removal shift before handing it over.
 */
static bool layer_move_apply(Main &bmain,
                             Material &ma,
                             Vector<ChannelChain *> &from_chains,
                             const int from_index,
                             Vector<ChannelChain *> &to_chains,
                             const int to_index,
                             PaintMaterialLayerEditError *r_error)
{
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  const bool same_chain = (from_chains.first() == to_chains.first());
  for (const int64_t index : from_chains.index_range()) {
    if ((from_chains[index] == to_chains[index]) != same_chain) {
      return fail(PaintMaterialLayerEditError::ChannelsDisagree);
    }
  }

  /* In a uniform chain the lowest layer is a Mix like any other, so it can be moved and something
   * can be put under it. A chain that still ends in a bare Image Texture has no such place:
   * swapping that image with a Mix layer would mean turning one into the other. */
  if ((from_index == 0 && !from_chains.first()->layers.first().is_mix()) ||
      (to_index == 0 && !to_chains.first()->layers.first().is_mix()))
  {
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }

  /* Within one chain the layer is taken out before it goes back in, so the last position it can
   * take is the one the top layer holds; across chains it can also land above the top one. */
  const int to_num = int(to_chains.first()->layers.size());
  if (to_index > (same_chain ? to_num - 1 : to_num)) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  if (same_chain) {
    for (ChannelChain *chain : from_chains) {
      ChainLayer moved = chain->layers[from_index];
      chain->layers.remove(from_index);
      chain->layers.insert(to_index, moved);
      chain_rebuild_links(*chain);
    }
  }
  else {
    /* Moving a layer from one tree/chain to another tree/chain (e.g. into/out of a group, or between groups). */
    bNodeTree *src_tree = from_chains.first()->tree;
    bNodeTree *dst_tree = to_chains.first()->tree;

    /* A group layer takes its top from one group instance that every channel shares -- the group
     * hands each channel its own `Result <Channel>` out of the same node -- so that node is copied
     * once, while a plain layer has a map of its own per channel. */
    const bool moving_group = from_chains.first()->layers[from_index].is_group;

    /* Collect map node, mix node and mask node for each channel before modifying graph. */
    struct MovingChannelNodes {
      bNode *mix_source = nullptr;
      bNode *map_source = nullptr;
      bNode *mask_source = nullptr;
      Image *image = nullptr;
      bool is_muted = false;
      /** The opacity to carry over; see how #mask_source is resolved below. */
      float opacity = 1.0f;
    };
    Vector<MovingChannelNodes> moving_nodes;

    for (ChannelChain *chain : from_chains) {
      ChainLayer &layer = chain->layers[from_index];
      MovingChannelNodes m;
      m.mix_source = layer.node;
      m.image = layer.image;
      m.is_muted = (layer.node != nullptr) && (layer.node->flag & NODE_MUTED);

      bNodeLink *top_link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
      if (top_link == nullptr) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      m.map_source = top_link->fromnode;

      CompositeMixNode mix;
      if (layer.node == nullptr || !composite_mix_node_read(*layer.node, mix)) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      if (mix.factor_opacity != nullptr) {
        /* Coverage and opacity coexist: what might be an explicit mask lives on the Multiply's
         * coverage input, not on Factor itself -- which now always resolves to the Multiply node,
         * never to an actual mask, whether the layer has one or not. */
        m.opacity =
            static_cast<const bNodeSocketValueFloat *>(mix.factor_opacity->default_value)->value;
        bNodeLink *coverage_link = sole_link_into(
            *const_cast<bNodeSocket *>(mix.factor_coverage));
        if (coverage_link != nullptr && coverage_link->fromnode != m.map_source) {
          m.mask_source = coverage_link->fromnode;
        }
      }
      else if (layer.factor != nullptr) {
        /* Legacy shape, nothing to separate an opacity from. */
        bNodeLink *factor_link = sole_link_into(*layer.factor);
        if (factor_link != nullptr && factor_link->fromnode != m.map_source) {
          m.mask_source = factor_link->fromnode;
        }
      }
      moving_nodes.append(m);
    }

    /* Copy nodes to destination tree. */
    struct CopiedChannelNodes {
      bNode *mix_copy = nullptr;
      bNode *map_copy = nullptr;
      bNode *mask_copy = nullptr;
      Image *image = nullptr;
      bNodeSocket *mix_bottom = nullptr;
      bNodeSocket *mix_top = nullptr;
      bNodeSocket *mix_factor = nullptr;
      bNodeSocket *mix_output = nullptr;
    };
    Vector<CopiedChannelNodes> copies;
    Vector<bNode *> created_in_dst;

    auto discard_copies = [&]() {
      /* The copies took references of their own -- an Image, a group's node tree -- so undoing
       * them has to hand those back. */
      for (bNode *node : created_in_dst) {
        bke::node_remove_node(&bmain, *dst_tree, *node, true);
      }
      created_in_dst.clear();
    };

    if (moving_group) {
      /* A group row is one group with an instance node per channel -- that is how it is built, see
       * #BKE_paint_material_layer_group_make -- so what has to agree is the tree they open, not the
       * nodes themselves. Each instance is copied with its own channel, like any other map. */
      for (const MovingChannelNodes &m : moving_nodes) {
        if (m.map_source == nullptr || m.map_source->id != moving_nodes.first().map_source->id) {
          return fail(PaintMaterialLayerEditError::ChannelsDisagree);
        }
      }
      /* A group cannot hold itself: the instance would end up inside the very tree it opens. */
      bNodeTree *group_tree = layer_group_tree_of(*moving_nodes.first().map_source);
      if (group_tree == nullptr ||
          group_tree == dst_tree ||
          bke::node_tree_contains_tree(*group_tree, *dst_tree))
      {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      /* Landing deeper than the readers walk would leave a stack they refuse to read at all. */
      if (to_chains.first()->nesting + 1 + layer_group_depth(*group_tree) >
          LAYER_GROUP_NESTING_MAX)
      {
        return fail(PaintMaterialLayerEditError::NestingTooDeep);
      }
    }

    for (const int64_t i : from_chains.index_range()) {
      const MovingChannelNodes &m = moving_nodes[i];
      Map<const bNodeSocket *, bNodeSocket *> socket_map;

      bNode *map_copy = bke::node_copy_with_mapping(
          dst_tree, *m.map_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
      bNode *mix_copy = bke::node_copy_with_mapping(
          dst_tree, *m.mix_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
      if (map_copy == nullptr || mix_copy == nullptr) {
        discard_copies();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      created_in_dst.append(map_copy);
      created_in_dst.append(mix_copy);

      bNode *mask_copy = nullptr;
      if (m.mask_source != nullptr) {
        mask_copy = bke::node_copy_with_mapping(
            dst_tree, *m.mask_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
        if (mask_copy == nullptr) {
          discard_copies();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        created_in_dst.append(mask_copy);
      }

      CopiedChannelNodes c;
      c.mix_copy = mix_copy;
      c.map_copy = map_copy;
      c.mask_copy = mask_copy;
      c.image = m.image;
      if (m.is_muted) {
        mix_copy->flag |= NODE_MUTED;
      }
      copies.append(c);
    }

    /* Update destination tree so socket pointers of copied nodes become valid. */
    BKE_ntree_update_after_single_tree_change(bmain, *dst_tree);
    dst_tree->ensure_topology_cache();

    for (const int64_t i : to_chains.index_range()) {
      CopiedChannelNodes &c = copies[i];
      /* A previous channel's links, added below, invalidate the topology cache
       * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
      dst_tree->ensure_topology_cache();
      CompositeMixNode mix;
      bNodeSocket *mix_out = mix_output_find(*c.mix_copy);
      /* A group instance hands each channel a `Result <Channel>` of its own and one shared Alpha,
       * where a map has the Color and Alpha of the image it reads. */
      bNodeSocket *map_color = nullptr;
      bNodeSocket *map_alpha = nullptr;
      if (moving_group) {
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(to_chains[i]->channel));
        char result_name[64];
        SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
        map_color = socket_find_by_name(*c.map_copy, SOCK_OUT, result_name);
        map_alpha = socket_find_by_name(*c.map_copy, SOCK_OUT, "Alpha");
      }
      else {
        map_color = bke::node_find_socket(*c.map_copy, SOCK_OUT, "Color"_ustr);
        map_alpha = bke::node_find_socket(*c.map_copy, SOCK_OUT, "Alpha"_ustr);
      }
      if (mix_out == nullptr || map_color == nullptr || map_alpha == nullptr ||
          !composite_mix_node_read(*c.mix_copy, mix))
      {
        discard_copies();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      c.mix_bottom = const_cast<bNodeSocket *>(mix.bottom);
      c.mix_top = const_cast<bNodeSocket *>(mix.top);
      c.mix_factor = const_cast<bNodeSocket *>(mix.factor);
      c.mix_output = mix_out;

      bke::node_add_link(*dst_tree, *c.map_copy, *map_color, *c.mix_copy, *c.mix_top);
      if (c.mask_copy != nullptr) {
        bNodeSocket *mask_color = bke::node_find_socket(*c.mask_copy, SOCK_OUT, "Color"_ustr);
        if (mask_color != nullptr) {
          layer_factor_coverage_link(*dst_tree,
                                     *c.mix_copy,
                                     *c.mix_factor,
                                     *c.mask_copy,
                                     *mask_color,
                                     moving_nodes[i].opacity);
        }
      }
      else {
        layer_factor_coverage_link(*dst_tree,
                                   *c.mix_copy,
                                   *c.mix_factor,
                                   *c.map_copy,
                                   *map_alpha,
                                   moving_nodes[i].opacity);
      }
    }

    /* Remove layer from source chains and rebuild source links. */
    Vector<bNode *> nodes_to_remove_from_src;
    for (const int64_t i : from_chains.index_range()) {
      ChannelChain *chain = from_chains[i];
      const MovingChannelNodes &m = moving_nodes[i];
      nodes_to_remove_from_src.append_non_duplicates(m.mix_source);
      nodes_to_remove_from_src.append_non_duplicates(m.map_source);
      if (m.mask_source != nullptr) {
        nodes_to_remove_from_src.append_non_duplicates(m.mask_source);
      }
      chain->layers.remove(from_index);
      chain_rebuild_links(*chain);
    }

    for (bNode *node : nodes_to_remove_from_src) {
      bke::node_remove_node(&bmain, *src_tree, *node, true);
    }
    BKE_ntree_update_tag_node_removed(src_tree);

    /* Insert new layer into destination chains and rebuild destination links. */
    for (const int64_t i : to_chains.index_range()) {
      ChannelChain *chain = to_chains[i];
      CopiedChannelNodes &c = copies[i];
      ChainLayer layer;
      layer.node = c.mix_copy;
      layer.bottom = c.mix_bottom;
      layer.top = c.mix_top;
      layer.factor = c.mix_factor;
      layer.output = c.mix_output;
      layer.image = c.image;
      layer.is_group = moving_group;

      chain->layers.insert(to_index, layer);
      chain_rebuild_links(*chain);
    }

    BKE_ntree_update_after_single_tree_change(bmain, *src_tree);
    if (dst_tree != src_tree) {
      BKE_ntree_update_after_single_tree_change(bmain, *dst_tree);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

/**
 * Move the layer at \a from_index into a group that holds nothing yet.
 *
 * The group's Group Output is unlinked -- that is what makes it empty (`08 §1.3`) -- so there is no
 * chain to insert into: this builds the first one. The layer's nodes are copied into the group's
 * tree, its Mix keeps its unlinked bottom, which is now the bottom of the group's own chain, and
 * its result becomes what the group hands out.
 */
static bool layer_move_into_empty_group(Main &bmain,
                                        Material &ma,
                                        Vector<ChannelChain *> &from_chains,
                                        const int from_index,
                                        Vector<ChannelChain *> &group_chains,
                                        const int group_index,
                                        PaintMaterialLayerEditError *r_error)
{
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  bNodeTree *src_tree = from_chains.first()->tree;
  const bNode *instance = composite_source_node_shallow(
      *group_chains.first()->layers[group_index].top);
  if (instance == nullptr || instance->id == nullptr) {
    return fail(PaintMaterialLayerEditError::ChainNotPlain);
  }
  bNodeTree *group_tree = layer_group_tree_of(*instance);
  if (group_tree == nullptr || group_tree == src_tree) {
    return fail(PaintMaterialLayerEditError::ChainNotPlain);
  }

  /* A folder moved into an empty one is one shared group tree with one instance node per
   * channel, the same shape #BKE_paint_material_layer_group_add builds -- not a map with a Color
   * and an Alpha, which the rest of this function otherwise assumes. */
  const bool moving_group = from_chains.first()->layers[from_index].is_group;

  /* Everything the graph is asked about is read before the first node is created: creating one
   * invalidates the topology cache the socket walks assert on. */
  struct MovingChannelNodes {
    bNode *mix_source = nullptr;
    bNode *map_source = nullptr;
    bNode *mask_source = nullptr;
    Image *image = nullptr;
    bool is_muted = false;
    int channel = 0;
    /** The opacity to carry over; see how #mask_source is resolved below. */
    float opacity = 1.0f;
  };
  Vector<MovingChannelNodes> moving_nodes;
  for (ChannelChain *chain : from_chains) {
    ChainLayer &layer = chain->layers[from_index];
    MovingChannelNodes m;
    m.channel = chain->channel;
    m.mix_source = layer.node;
    m.image = layer.image;
    m.is_muted = (layer.node != nullptr) && (layer.node->flag & NODE_MUTED);
    bNodeLink *top_link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
    if (top_link == nullptr) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    m.map_source = top_link->fromnode;
    CompositeMixNode mix;
    if (layer.node == nullptr || !composite_mix_node_read(*layer.node, mix)) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    if (mix.factor_opacity != nullptr) {
      /* Coverage and opacity coexist: what might be an explicit mask lives on the Multiply's
       * coverage input, not on Factor itself -- which now always resolves to the Multiply node,
       * never to an actual mask, whether the layer has one or not. */
      m.opacity =
          static_cast<const bNodeSocketValueFloat *>(mix.factor_opacity->default_value)->value;
      bNodeLink *coverage_link = sole_link_into(*const_cast<bNodeSocket *>(mix.factor_coverage));
      if (coverage_link != nullptr && coverage_link->fromnode != m.map_source) {
        m.mask_source = coverage_link->fromnode;
      }
    }
    else if (layer.factor != nullptr) {
      /* Legacy shape, nothing to separate an opacity from. */
      bNodeLink *factor_link = sole_link_into(*layer.factor);
      if (factor_link != nullptr && factor_link->fromnode != m.map_source) {
        m.mask_source = factor_link->fromnode;
      }
    }
    moving_nodes.append(m);
  }

  if (moving_group) {
    /* The instance differs per channel, but every one of them has to point at the same group
     * tree, the way #BKE_paint_material_layer_group_add builds it -- otherwise the folder being
     * moved is not the shape this code assumes. */
    for (const MovingChannelNodes &m : moving_nodes) {
      if (m.map_source == nullptr || m.map_source->id != moving_nodes.first().map_source->id) {
        return fail(PaintMaterialLayerEditError::ChannelsDisagree);
      }
    }
    bNodeTree *moved_group_tree = layer_group_tree_of(*moving_nodes.first().map_source);
    /* A group cannot hold itself, directly or through one of its own folders. */
    if (moved_group_tree == nullptr || moved_group_tree == group_tree ||
        bke::node_tree_contains_tree(*moved_group_tree, *group_tree))
    {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    /* Landing deeper than the readers walk would leave a stack they refuse to read at all. */
    if (group_chains.first()->nesting + 1 + layer_group_depth(*moved_group_tree) >
        LAYER_GROUP_NESTING_MAX)
    {
      return fail(PaintMaterialLayerEditError::NestingTooDeep);
    }
  }

  struct CopiedChannelNodes {
    bNode *mix_copy = nullptr;
    bNode *map_copy = nullptr;
    bNode *mask_copy = nullptr;
  };
  Vector<CopiedChannelNodes> copies;
  Vector<bNode *> created;
  auto discard = [&]() {
    for (bNode *node : created) {
      bke::node_remove_node(&bmain, *group_tree, *node, true);
    }
    created.clear();
  };

  for (const MovingChannelNodes &m : moving_nodes) {
    Map<const bNodeSocket *, bNodeSocket *> socket_map;
    CopiedChannelNodes c;
    c.map_copy = bke::node_copy_with_mapping(
        group_tree, *m.map_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
    c.mix_copy = bke::node_copy_with_mapping(
        group_tree, *m.mix_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
    if (c.map_copy == nullptr || c.mix_copy == nullptr) {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    created.append(c.map_copy);
    created.append(c.mix_copy);
    if (m.mask_source != nullptr) {
      c.mask_copy = bke::node_copy_with_mapping(
          group_tree, *m.mask_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
      if (c.mask_copy == nullptr) {
        discard();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      created.append(c.mask_copy);
    }
    if (m.is_muted) {
      c.mix_copy->flag |= NODE_MUTED;
    }
    copies.append(c);
  }

  /* Sockets of the copies -- a group instance among them -- only exist once the tree is updated. */
  BKE_ntree_update_after_single_tree_change(bmain, *group_tree);
  group_tree->ensure_topology_cache();

  bNode *group_output = nullptr;
  for (bNode &node : group_tree->nodes) {
    if (node.type_legacy == NODE_GROUP_OUTPUT) {
      group_output = &node;
      break;
    }
  }
  if (group_output == nullptr) {
    discard();
    return fail(PaintMaterialLayerEditError::ChainNotPlain);
  }

  for (const int64_t i : moving_nodes.index_range()) {
    CopiedChannelNodes &c = copies[i];
    /* A previous channel's links, added below, invalidate the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    group_tree->ensure_topology_cache();
    CompositeMixNode mix;
    bNodeSocket *mix_out = mix_output_find(*c.mix_copy);
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(moving_nodes[i].channel));
    char result_name[64];
    SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
    /* A group instance hands each channel a `Result <Channel>` of its own and one shared Alpha,
     * where a map has the Color and Alpha of the image it reads. A group instance's sockets come
     * from its tree interface and are identified by an auto-generated string ("Socket_0", ...)
     * rather than by the name shown in the UI, so those two are looked up by name instead. */
    bNodeSocket *map_color = moving_group ?
                                socket_find_by_name(*c.map_copy, SOCK_OUT, result_name) :
                                bke::node_find_socket(*c.map_copy, SOCK_OUT, "Color"_ustr);
    bNodeSocket *map_alpha = moving_group ?
                                socket_find_by_name(*c.map_copy, SOCK_OUT, "Alpha") :
                                bke::node_find_socket(*c.map_copy, SOCK_OUT, "Alpha"_ustr);
    if (mix_out == nullptr || map_color == nullptr || map_alpha == nullptr ||
        !composite_mix_node_read(*c.mix_copy, mix))
    {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    bNodeSocket *result = socket_find_by_name(*group_output, SOCK_IN, result_name);
    if (result == nullptr) {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }

    bke::node_add_link(
        *group_tree, *c.map_copy, *map_color, *c.mix_copy, *const_cast<bNodeSocket *>(mix.top));
    bNodeSocket *coverage = map_alpha;
    if (c.mask_copy != nullptr) {
      if (bNodeSocket *mask_color = bke::node_find_socket(*c.mask_copy, SOCK_OUT, "Color"_ustr)) {
        coverage = mask_color;
      }
    }
    layer_factor_coverage_link(*group_tree,
                               *c.mix_copy,
                               *const_cast<bNodeSocket *>(mix.factor),
                               (c.mask_copy != nullptr) ? *c.mask_copy : *c.map_copy,
                               *coverage,
                               moving_nodes[i].opacity);
    /* The bottom stays unlinked: inside the group this layer blends over transparency, which is
     * what the group composites on. */
    bke::node_add_link(*group_tree, *c.mix_copy, *mix_out, *group_output, *result);

    /* How much the group covers what is below it is how much its one layer does. Written from the
     * first channel: the group hands out a single Alpha, shared by all of them. */
    if (i == 0) {
      if (bNodeSocket *alpha_in = socket_find_by_name(*group_output, SOCK_IN, "Alpha")) {
        bke::node_add_link(*group_tree,
                           (c.mask_copy != nullptr) ? *c.mask_copy : *c.map_copy,
                           *coverage,
                           *group_output,
                           *alpha_in);
      }
    }
  }

  /* The layer is gone from the chain it came out of, and its nodes with it. */
  Vector<bNode *> nodes_to_remove;
  for (const int64_t i : from_chains.index_range()) {
    const MovingChannelNodes &m = moving_nodes[i];
    nodes_to_remove.append_non_duplicates(m.mix_source);
    nodes_to_remove.append_non_duplicates(m.map_source);
    if (m.mask_source != nullptr) {
      nodes_to_remove.append_non_duplicates(m.mask_source);
    }
    from_chains[i]->layers.remove(from_index);
    chain_rebuild_links(*from_chains[i]);
  }
  for (bNode *node : nodes_to_remove) {
    bke::node_remove_node(&bmain, *src_tree, *node, true);
  }
  BKE_ntree_update_tag_node_removed(src_tree);

  BKE_ntree_update_after_single_tree_change(bmain, *src_tree);
  BKE_ntree_update_after_single_tree_change(bmain, *group_tree);
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_reorder(Main &bmain,
                                      Material &ma,
                                      const int from_ordinal,
                                      const int to_ordinal,
                                      PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  if (from_ordinal == to_ordinal) {
    return true;
  }

  /* 1. Preflight: whether the layer can move, decided without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(
          bmain, ma, from_ordinal, LayerEditOp::Reorder, plan, error, to_ordinal))
  {
    return fail(error);
  }
  /* 2. Shape and identity: a move reads markers, so they are handed out before the rows are
   * resolved for good; a conversion changes the chains, so the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    BKE_paint_material_layer_bottom_normalize(bmain, ma);
  }
  BKE_paint_material_layer_markers_ensure(ma);
  if (plan.needs_bottom_normalize) {
    if (!layer_edit_plan_build(
            bmain, ma, from_ordinal, LayerEditOp::Reorder, plan, error, to_ordinal))
    {
      BLI_assert_unreachable();
      return fail(error);
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  return layer_move_apply(bmain,
                          ma,
                          plan.chains,
                          plan.layer_index,
                          plan.target_chains,
                          plan.target_index,
                          r_error);
}

bool BKE_paint_material_layer_move(Main &bmain,
                                   Material &ma,
                                   const int from_ordinal,
                                   const int anchor_ordinal,
                                   const PaintMaterialLayerMovePlace place,
                                   int *r_ordinal,
                                   PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  if (from_ordinal == anchor_ordinal && place != PaintMaterialLayerMovePlace::Into) {
    return true;
  }

  /* 1. Preflight: whether the layer can move, and where it would land, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(
          bmain, ma, from_ordinal, LayerEditOp::Move, plan, error, anchor_ordinal, place))
  {
    return fail(error);
  }
  /* 2. Shape and identity: a move reads markers, so they are handed out before the rows are
   * resolved for good; a conversion changes the chains, so the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    BKE_paint_material_layer_bottom_normalize(bmain, ma);
  }
  BKE_paint_material_layer_markers_ensure(ma);
  if (plan.needs_bottom_normalize) {
    if (!layer_edit_plan_build(
            bmain, ma, from_ordinal, LayerEditOp::Move, plan, error, anchor_ordinal, place))
    {
      BLI_assert_unreachable();
      return fail(error);
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */

  /* The marker survives every move here, and finding the row again by it afterwards is simpler
   * and more robust than working out its new ordinal from what each branch below does to the
   * graph -- especially landing in a folder that was empty, which has no chain of its own yet for
   * an ordinal to be read from. */
  const bNode *from_node = plan.chains.first()->layers[plan.layer_index].node;
  const bUUID moved_marker = (from_node != nullptr) ? BKE_paint_material_layer_marker_get(*from_node) :
                                                      BLI_uuid_nil();
  auto finish = [&](const bool ok) {
    if (!ok) {
      return false;
    }
    if (r_ordinal != nullptr) {
      *r_ordinal = -1;
      if (!BLI_uuid_is_nil(moved_marker)) {
        Vector<Vector<ChannelChain>> fresh_per_channel;
        PaintMaterialLayerEditError ignore = PaintMaterialLayerEditError::None;
        if (chains_collect_forest(ma, fresh_per_channel, ignore) && !fresh_per_channel.is_empty()) {
          const Vector<ChannelChain> &forest0 = fresh_per_channel.first();
          for (const int64_t chain_index : forest0.index_range()) {
            const ChannelChain &chain = forest0[chain_index];
            for (const int64_t layer_index : chain.layers.index_range()) {
              const ChainLayer &layer = chain.layers[layer_index];
              if (layer.node != nullptr &&
                  BLI_uuid_equal(BKE_paint_material_layer_marker_get(*layer.node), moved_marker))
              {
                const int ordinal = forest_ordinal_for_position(
                    forest0, int(chain_index), int(layer_index));
                *r_ordinal = (ordinal >= 0) ? ordinal : int(layer_index);
                break;
              }
            }
            if (*r_ordinal >= 0) {
              break;
            }
          }
        }
      }
    }
    return true;
  };

  if (place == PaintMaterialLayerMovePlace::Into) {
    if (plan.target_is_empty_group) {
      return finish(layer_move_into_empty_group(bmain,
                                                ma,
                                                plan.chains,
                                                plan.layer_index,
                                                plan.target_chains,
                                                plan.target_index,
                                                r_error));
    }
    return finish(layer_move_apply(bmain,
                                   ma,
                                   plan.chains,
                                   plan.layer_index,
                                   plan.target_chains,
                                   plan.target_index,
                                   r_error));
  }

  if (plan.target_index == plan.layer_index &&
      plan.chains.first() == plan.target_chains.first())
  {
    return true;
  }
  return finish(layer_move_apply(bmain,
                                 ma,
                                 plan.chains,
                                 plan.layer_index,
                                 plan.target_chains,
                                 plan.target_index,
                                 r_error));
}

bool BKE_paint_material_layer_group_make(Main &bmain,
                                         Material &ma,
                                         const int from_ordinal,
                                         const int to_ordinal,
                                         int *r_ordinal,
                                         PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the range, the shape and every map it will hold, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(
          bmain, ma, from_ordinal, LayerEditOp::GroupMake, plan, error, to_ordinal))
  {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * grouping is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(
              bmain, ma, from_ordinal, LayerEditOp::GroupMake, plan, error, to_ordinal))
      {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  bNodeTree &tree = *ma.nodetree;
  const int from = plan.layer_index;
  const int to = plan.target_index;

  /* Every map the group will hold, resolved before the first node is created: creating one
   * invalidates the topology cache, and there is no reading links after that. */
  Vector<Vector<bNode *>> map_nodes_per_chain;
  for (ChannelChain *chain : plan.chains) {
    Vector<bNode *> maps = chain_range_map_nodes(*chain, from, to);
    if (maps.is_empty()) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    map_nodes_per_chain.append(std::move(maps));
  }

  bNodeTree *group_tree = layer_group_tree_add(bmain, plan.chains, from, to);
  if (group_tree == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }

  /* One instance per channel, all of them pointing at the one group tree: the channels are
   * separate chains, but they are the same group. */
  Vector<bNode *> instances;
  Vector<bNode *> nodes_to_remove;
  /* Freeing the group tree while a node still points at it leaves that pointer dangling, and the
   * next thing to read it trips the ID type assert rather than reporting the refusal. So the
   * instances go first, and only a tree nothing references is freed. */
  auto discard_group = [&]() {
    for (bNode *instance : instances) {
      instance->id = nullptr;
      bke::node_remove_node(&bmain, tree, *instance, false);
    }
    instances.clear();
    BKE_id_free(&bmain, group_tree);
  };

  for (const int64_t chain_index : plan.chains.index_range()) {
    ChannelChain &chain = *plan.chains[chain_index];
    bNode *instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
    if (instance == nullptr) {
      discard_group();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    instance->id = &group_tree->id;
    id_us_plus(&group_tree->id);
    instances.append(instance);

    if (!layer_group_fill_channel(bmain,
                                  tree,
                                  *group_tree,
                                  chain,
                                  map_nodes_per_chain[chain_index],
                                  from,
                                  to,
                                  chain_index == 0,
                                  nodes_to_remove))
    {
      discard_group();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
  }

  /* #node_tree_add_tree returned the tree with one user of its own; the instances above are the
   * real users, one per channel. Without this the count stays one too high, and an ungroup or a
   * remove that drops every instance leaves the tree at #us == 1 -- an orphan nothing recognizes
   * as one, kept in #Main until the next file write recomputes the counts. */
  id_us_min(&group_tree->id);

  /* Sockets of the instances only exist once the tree has been updated. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  for (const int64_t chain_index : plan.chains.index_range()) {
    ChannelChain &chain = *plan.chains[chain_index];
    bNode &instance = *instances[chain_index];
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(chain.channel));
    char result_name[64];
    SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);

    bNodeSocket *result = socket_find_by_name(instance, SOCK_OUT, result_name);
    bNodeSocket *alpha = socket_find_by_name(instance, SOCK_OUT, "Alpha");
    ChainLayer &keeper = chain.layers[from];
    if (result == nullptr || alpha == nullptr || keeper.top == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    /* The kept Mix node stops blending a map and starts blending the group. */
    relink_into(tree, *keeper.top, *keeper.node, instance, *result);
    relink_into(tree, *keeper.factor, *keeper.node, instance, *alpha);
    bke::node_position_relative(instance, *keeper.node, result, *keeper.top);

    /* Everything above the kept layer, up to the top of the range, is inside the group now. */
    chain.layers.remove(from + 1, to - from);
    keeper.image = nullptr;
    chain_rebuild_links(chain);
  }

  for (bNode *node : nodes_to_remove) {
    bke::node_remove_node(&bmain, tree, *node, true);
  }
  BKE_ntree_update_tag_node_removed(&tree);

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  /* The sub-stack was built with the range's own bottom map at the bottom, which is the old shape:
   * a bare image, with no Mix node of its own and therefore no blend mode, opacity or mute. Giving
   * the group's chain the same shape as every other one is what makes its lowest row a layer. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  BKE_paint_material_layer_markers_ensure(ma);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_ordinal != nullptr) {
    *r_ordinal = from;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_group_add(Main &bmain,
                                        Material &ma,
                                        const int ordinal,
                                        const PaintMaterialLayerMovePlace place,
                                        int *r_ordinal,
                                        PaintMaterialLayerEditError *r_error,
                                        bNodeTree **r_group_tree)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: where the empty folder goes, without writing a byte. An explicit ordinal names a
   * chain wherever it lives -- the material's own tree for a top-level row, or a group's own tree
   * for one already inside a folder; -1 means the top of the top-level stack. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(
          bmain, ma, ordinal, LayerEditOp::GroupAdd, plan, error, -1, place))
  {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * folder is known to be made; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(
              bmain, ma, ordinal, LayerEditOp::GroupAdd, plan, error, -1, place))
      {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */

  /* The group is created next to the row it was given, in the same tree that row lives in -- the
   * material's own for a top-level row, or a nested group's own for one already inside a folder. */
  bNodeTree &tree = *plan.chains.first()->tree;
  int insert_at = plan.target_index;

  /* The group holds nothing yet, so its Group Output stays unlinked: an unlinked Result is
   * transparent and its Alpha is zero, which is exactly what an empty folder contributes. */
  bNodeTree *group_tree = layer_group_tree_add(bmain, plan.chains, insert_at, insert_at);
  if (group_tree == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }

  struct NewGroupNodes {
    int channel = 0;
    bNode *instance = nullptr;
    bNode *mix = nullptr;
  };
  Vector<NewGroupNodes> added;
  /* Freeing the group tree while a node still points at it leaves that pointer dangling, so the
   * instances go first and only a tree nothing references is freed. */
  auto discard = [&]() {
    for (NewGroupNodes &nodes : added) {
      if (nodes.instance != nullptr) {
        nodes.instance->id = nullptr;
        bke::node_remove_node(&bmain, tree, *nodes.instance, false);
      }
      if (nodes.mix != nullptr) {
        bke::node_remove_node(&bmain, tree, *nodes.mix, true);
      }
    }
    added.clear();
    BKE_id_free(&bmain, group_tree);
  };

  for (const ChannelChain *chain : plan.chains) {
    NewGroupNodes nodes;
    nodes.channel = chain->channel;
    nodes.instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
    if (nodes.instance != nullptr) {
      nodes.instance->id = &group_tree->id;
      id_us_plus(&group_tree->id);
    }
    nodes.mix = layer_mix_node_create(bmain, tree, chain->channel);
    added.append(nodes);
    if (nodes.instance == nullptr || nodes.mix == nullptr) {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
  }

  /* #node_tree_add_tree returned the tree with one user of its own; the instances above are the
   * real users, one per channel. See #BKE_paint_material_layer_group_make for what the surplus
   * user costs on an ungroup. */
  id_us_min(&group_tree->id);

  /* Sockets of the instances only exist once the tree has been updated, and the chains resolved
   * above describe the tree from before those nodes existed -- so the row this group belongs next
   * to is found again, fresh. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  auto resolve_fresh_target = [&](Vector<Vector<ChannelChain>> &per_channel,
                                  Vector<ChannelChain *> &r_chains,
                                  int &r_insert_at) {
    if (!chains_collect_forest(ma, per_channel, error)) {
      return false;
    }
    if (ordinal < 0) {
      for (Vector<ChannelChain> &forest : per_channel) {
        r_chains.append(&forest.last());
      }
      if (r_chains.is_empty()) {
        error = PaintMaterialLayerEditError::NotAStack;
        return false;
      }
      const int64_t layer_num = r_chains.first()->layers.size();
      for (ChannelChain *chain : r_chains) {
        if (chain->layers.size() != layer_num) {
          error = PaintMaterialLayerEditError::ChannelsDisagree;
          return false;
        }
      }
      r_insert_at = int(layer_num);
      return true;
    }
    int layer_index = -1;
    if (!forest_rows_resolve(per_channel, ordinal, r_chains, layer_index, error)) {
      return false;
    }
    /* Above the row it was given is the position after it in a chain listed bottom to top; below
     * it is the row's own position. The preflight has already refused the one place this cannot
     * express -- under a bare base, which would have to be converted first. */
    r_insert_at = (place == PaintMaterialLayerMovePlace::Below) ? layer_index : layer_index + 1;
    return true;
  };

  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> chains;
  if (!resolve_fresh_target(per_channel, chains, insert_at)) {
    discard();
    return fail(error);
  }

  /* Everything is resolved before a single link moves, the same transaction shape as layer_add. */
  struct ResolvedGroup {
    ChannelChain *chain = nullptr;
    ChainLayer layer;
    bNode *instance = nullptr;
    bNodeSocket *result = nullptr;
    bNodeSocket *alpha = nullptr;
    bNodeSocket *factor = nullptr;
  };
  Vector<ResolvedGroup> resolved;
  for (NewGroupNodes &nodes : added) {
    ChannelChain *chain = nullptr;
    for (ChannelChain *candidate : chains) {
      if (candidate->channel == nodes.channel) {
        chain = candidate;
        break;
      }
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(nodes.channel));
    char result_name[64];
    SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);

    CompositeMixNode mix;
    bNodeSocket *output = mix_output_find(*nodes.mix);
    bNodeSocket *result = socket_find_by_name(*nodes.instance, SOCK_OUT, result_name);
    bNodeSocket *alpha = socket_find_by_name(*nodes.instance, SOCK_OUT, "Alpha");
    if (chain == nullptr || output == nullptr || result == nullptr || alpha == nullptr ||
        !composite_mix_node_read(*nodes.mix, mix))
    {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    ResolvedGroup entry;
    entry.chain = chain;
    entry.instance = nodes.instance;
    entry.result = result;
    entry.alpha = alpha;
    entry.factor = const_cast<bNodeSocket *>(mix.factor);
    entry.layer.node = nodes.mix;
    entry.layer.bottom = const_cast<bNodeSocket *>(mix.bottom);
    entry.layer.top = const_cast<bNodeSocket *>(mix.top);
    entry.layer.factor = entry.factor;
    entry.layer.output = output;
    entry.layer.is_group = true;
    resolved.append(entry);
  }

  /* One identity for the whole row, so a later move takes it in every channel at once. */
  BKE_paint_material_layer_markers_ensure(ma);
  const bUUID layer_id = BLI_uuid_generate_random();

  for (ResolvedGroup &entry : resolved) {
    /* Outside the group it is indistinguishable from a layer's map: Result feeds the Mix, Alpha
     * covers it -- and an empty group's Alpha is zero, so the folder shows nothing. */
    bke::node_add_link(tree, *entry.instance, *entry.result, *entry.layer.node, *entry.layer.top);
    layer_factor_coverage_link(
        tree, *entry.layer.node, *entry.factor, *entry.instance, *entry.alpha, 1.0f);
    bke::node_position_relative(
        *entry.instance, *entry.layer.node, entry.result, *entry.layer.top);
    entry.chain->layers.insert(insert_at, entry.layer);
    chain_rebuild_links(*entry.chain);
    BKE_paint_material_layer_marker_set(*entry.layer.node, layer_id);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_group_tree != nullptr) {
    *r_group_tree = group_tree;
  }
  if (r_ordinal != nullptr) {
    /* Named the same way the outliner numbers its rows: a plain position at the top level, or one
     * counted depth-first through the forest for a row that landed inside a group. */
    const int chain_index = (per_channel.is_empty() || resolved.is_empty()) ?
                                -1 :
                                int(resolved.first().chain - per_channel.first().data());
    const int new_ordinal = (chain_index < 0) ?
                                -1 :
                                forest_ordinal_for_position(per_channel.first(), chain_index, insert_at);
    *r_ordinal = (new_ordinal >= 0) ? new_ordinal : insert_at;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_group_material_add(Main &bmain,
                                                 Material &ma,
                                                 Material &source_material,
                                                 const int ordinal,
                                                 const PaintMaterialLayerMovePlace place,
                                                 int *r_ordinal,
                                                 PaintMaterialLayerEditError *r_error)
{
  int new_ordinal = -1;
  if (!BKE_paint_material_layer_group_add(bmain, ma, ordinal, place, &new_ordinal, r_error)) {
    return false;
  }
  if (r_ordinal != nullptr) {
    *r_ordinal = new_ordinal;
  }
  /* The row reads as the material it stands for. */
  if (!BKE_paint_material_layer_rename(
          bmain, ma, new_ordinal, source_material.id.name + 2, r_error))
  {
    return false;
  }

  /* The group the new row holds its layers in is found again through the stack the reader hands
   * out -- its #PaintMaterialLayerStackEntry::group_tree is that tree, the same one the reader
   * will ask for the material. */
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, ma, entries)) {
    return false;
  }
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.ordinal != new_ordinal) {
      continue;
    }
    if (entry.group_tree == nullptr) {
      return false;
    }
    BKE_paint_material_layer_group_material_set(const_cast<ID &>(entry.group_tree->id),
                                                &source_material);
    return true;
  }
  return false;
}

bool BKE_paint_material_layer_group_ungroup(Main &bmain,
                                            Material &ma,
                                            const int ordinal,
                                            int *r_layer_num,
                                            PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row must be a top-level one whose Mix blends a layer group, decided without
   * writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Ungroup, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * ungrouping is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Ungroup, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  bNodeTree &tree = *ma.nodetree;

  /* Everything is copied out of the group first; only then are links moved, so a group that turns
   * out to be malformed halfway through leaves the stack as it was. */
  struct UngroupedChannel {
    ChannelChain *chain = nullptr;
    bNode *instance = nullptr;
    /* Bottom first: index 0 is the map that goes back onto the group's own Mix node. */
    Vector<ChainLayer> layers;
  };
  Vector<UngroupedChannel> unpacked;
  Vector<bNode *> created;
  auto discard = [&]() {
    for (bNode *node : created) {
      bke::node_remove_node(&bmain, tree, *node, false);
    }
    created.clear();
  };

  /* Which group instance each channel's row blends, resolved before anything is copied: copying a
   * node invalidates the topology cache these links are read through. */
  Vector<bNode *> instances;
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &keeper = chain->layers[plan.layer_index];
    bNodeLink *top_link = (keeper.top == nullptr) ? nullptr : sole_link_into(*keeper.top);
    if (top_link == nullptr || !BKE_paint_material_is_layer_group(*top_link->fromnode)) {
      return fail(PaintMaterialLayerEditError::NotAStack);
    }
    instances.append(top_link->fromnode);
  }

  for (const int64_t chain_index : plan.chains.index_range()) {
    ChannelChain &chain = *plan.chains[chain_index];
    bNode &instance = *instances[chain_index];
    bNodeTree *group_tree = layer_group_tree_of(instance);
    if (group_tree == nullptr) {
      discard();
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }

    /* The sub-stack, read with the same walker the outer chains use: inside the group it is an
     * ordinary chain that happens to end at a Group Output. */
    group_tree->ensure_topology_cache();
    bNodeSocket *result_in = nullptr;
    for (bNode &node : group_tree->nodes) {
      if (node.type_legacy != NODE_GROUP_OUTPUT) {
        continue;
      }
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(chain.channel));
      char result_name[64];
      SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
      result_in = socket_find_by_name(node, SOCK_IN, result_name);
      if (result_in != nullptr) {
        break;
      }
    }
    ChannelChain inner;
    inner.channel = chain.channel;
    if (result_in == nullptr || !chain_collect(*group_tree, *result_in, inner, error)) {
      discard();
      return fail(error == PaintMaterialLayerEditError::None ?
                      PaintMaterialLayerEditError::ChainNotPlain :
                      error);
    }

    UngroupedChannel out;
    out.chain = &chain;
    out.instance = &instance;
    for (const int64_t index : inner.layers.index_range()) {
      ChainLayer &inner_layer = inner.layers[index];
      Map<const bNodeSocket *, bNodeSocket *> socket_map;
      bNode *copy = bke::node_copy_with_mapping(&tree,
                                                *inner_layer.node,
                                                LIB_ID_COPY_DEFAULT,
                                                std::nullopt,
                                                std::nullopt,
                                                socket_map);
      if (copy == nullptr) {
        discard();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      created.append(copy);

      ChainLayer layer;
      layer.node = copy;
      layer.image = inner_layer.image;
      if (index == 0) {
        /* The bottom of the sub-stack is a bare map; it goes back onto the group's Mix node. */
        layer.output = bke::node_find_socket(*copy, SOCK_OUT, "Color"_ustr);
        layer.factor = bke::node_find_socket(*copy, SOCK_OUT, "Alpha"_ustr);
        if (layer.output == nullptr || layer.factor == nullptr) {
          discard();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
      }
      out.layers.append(layer);
    }
    unpacked.append(std::move(out));
  }

  /* The copies exist but nothing points at them yet; sockets of copied Mix nodes are resolved
   * after the update, the same way layer_add does it. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  Vector<bNode *> nodes_to_remove;
  int restored_num = 0;
  for (UngroupedChannel &out : unpacked) {
    ChannelChain &chain = *out.chain;
    ChainLayer &keeper = chain.layers[plan.layer_index];
    ChainLayer &bottom = out.layers.first();

    /* The group's Mix node goes back to blending a map, which is what it did before grouping. */
    relink_into(tree, *keeper.top, *keeper.node, *bottom.node, *bottom.output);
    relink_into(tree, *keeper.factor, *keeper.node, *bottom.node, *bottom.factor);
    keeper.image = bottom.image;
    bke::node_position_relative(*bottom.node, *keeper.node, bottom.output, *keeper.top);

    /* The two relinks above, and a previous channel's, invalidate the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    tree.ensure_topology_cache();

    /* Every layer above the bottom one is spliced into the outer chain, in order. */
    int insert_at = plan.layer_index + 1;
    for (const int64_t index : out.layers.index_range().drop_front(1)) {
      ChainLayer &layer = out.layers[index];
      CompositeMixNode mix;
      bNodeSocket *output = mix_output_find(*layer.node);
      if (output == nullptr || !composite_mix_node_read(*layer.node, mix)) {
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      layer.bottom = const_cast<bNodeSocket *>(mix.bottom);
      layer.top = const_cast<bNodeSocket *>(mix.top);
      layer.factor = const_cast<bNodeSocket *>(mix.factor);
      layer.output = output;
      chain.layers.insert(insert_at++, layer);
    }
    restored_num = int(out.layers.size());
    chain_rebuild_links(chain);
    nodes_to_remove.append_non_duplicates(out.instance);
  }

  for (bNode *node : nodes_to_remove) {
    bke::node_remove_node(&bmain, tree, *node, true);
  }
  BKE_ntree_update_tag_node_removed(&tree);

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_layer_num != nullptr) {
    *r_layer_num = restored_num;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_duplicate(Main &bmain,
                                        Material &ma,
                                        const int ordinal,
                                        int *r_ordinal,
                                        PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and is not a bare base, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Duplicate, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * duplication is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Duplicate, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  bNodeTree &tree = *ma.nodetree;
  const int layer_index = plan.layer_index;

  ChainLayer &source_layer = plan.chains.first()->layers[layer_index];
  if (source_layer.is_group) {
    bNodeLink *source_link = sole_link_into(*source_layer.top);
    bNodeTree *source_group =
        (source_link != nullptr) ? layer_group_tree_of(*source_link->fromnode) : nullptr;
    if (source_group == nullptr) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }

    bNodeTree *group_copy = bke::node_tree_copy_tree(&bmain, *source_group);
    if (group_copy == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    for (bNode &node : group_copy->nodes) {
      if (node.id == nullptr || GS(node.id->name) != ID_IM) {
        continue;
      }
      Image *image_copy = id_cast<Image *>(BKE_id_copy(&bmain, node.id));
      if (image_copy == nullptr) {
        BKE_id_free(&bmain, group_copy);
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      /* #node_tree_copy_tree gave the original map a user through this node, and the node is
       * about to stop pointing at it: without this the original keeps a user it no longer has,
       * for the rest of the session. The copy needs no matching #id_us_plus -- a freshly created
       * ID already carries one user, and this node is it, the same way every other map node in
       * this file takes the image it was created with. */
      id_us_min(node.id);
      node.id = &image_copy->id;
    }
    struct MarkerCopy {
      bUUID source;
      bUUID copy;
    };
    Vector<MarkerCopy> markers;
    for (bNode &node : group_copy->nodes) {
      const bUUID source_marker = BKE_paint_material_layer_marker_get(node);
      if (BLI_uuid_is_nil(source_marker)) {
        continue;
      }
      bUUID copy_marker = BLI_uuid_nil();
      for (const MarkerCopy &marker : markers) {
        if (BLI_uuid_equal(marker.source, source_marker)) {
          copy_marker = marker.copy;
          break;
        }
      }
      if (BLI_uuid_is_nil(copy_marker)) {
        copy_marker = BLI_uuid_generate_random();
        markers.append({source_marker, copy_marker});
      }
      BKE_paint_material_layer_marker_set(node, copy_marker);
      if (node.id != nullptr && GS(node.id->name) == ID_IM) {
        id_cast<Image *>(node.id)->paint_layer_id = copy_marker;
      }
    }

    int group_ordinal = -1;
    bNodeTree *empty_group = nullptr;
    if (!BKE_paint_material_layer_group_add(bmain,
                                            ma,
                                            ordinal,
                                            PaintMaterialLayerMovePlace::Above,
                                            &group_ordinal,
                                            r_error,
                                            &empty_group))
    {
      BKE_id_free(&bmain, group_copy);
      return false;
    }
    /* The empty group is in the stack from here on; every way out of the checks below has to
     * leave it as it found them, which the "never partially applied" contract promises. */
    auto rollback = [&]() {
      /* Removing the empty group puts the stack back; the tree it owns outlived its nodes, and
       * the caller that took the group out is the one that deletes it. */
      if (BKE_paint_material_layer_remove(bmain, ma, group_ordinal, nullptr)) {
        BKE_id_delete(&bmain, &empty_group->id);
      }
      BKE_id_free(&bmain, group_copy);
    };

    Vector<Vector<ChannelChain>> fresh_forest;
    Vector<ChannelChain *> fresh_chains;
    int fresh_index = -1;
    if (!chains_collect_forest(ma, fresh_forest, error) ||
        !forest_rows_resolve(fresh_forest, group_ordinal, fresh_chains, fresh_index, error))
    {
      rollback();
      return fail(error);
    }
    Vector<bNode *> new_instances;
    for (ChannelChain *chain : fresh_chains) {
      bNodeLink *link = sole_link_into(*chain->layers[fresh_index].top);
      if (link == nullptr || !BKE_paint_material_is_layer_group(*link->fromnode)) {
        rollback();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      new_instances.append(link->fromnode);
    }
    for (bNode *instance : new_instances) {
      id_us_min(&empty_group->id);
      instance->id = &group_copy->id;
      id_us_plus(&group_copy->id);
    }
    /* The tree is this call's own creation and holds nothing but its own nodes, so no other
     * data-block can reference it; a plain delete is the honest way to let it go. */
    BKE_id_delete(&bmain, &empty_group->id);
    BKE_ntree_update_after_single_tree_change(bmain, tree);
    BKE_ntree_update_after_single_tree_change(bmain, *group_copy);
    paint_layer_edit_committed(bmain, ma, true);
    if (r_ordinal != nullptr) {
      *r_ordinal = group_ordinal;
    }
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  }

  /* Everything is created before a single link moves, the same transaction shape as layer_add. */
  struct DuplicatedLayer {
    ChannelChain *chain = nullptr;
    bNode *mix = nullptr;
    bNode *tex = nullptr;
    Image *image = nullptr;
  };
  Vector<DuplicatedLayer> copies;
  auto discard = [&]() {
    for (DuplicatedLayer &copy : copies) {
      if (copy.mix != nullptr) {
        bke::node_remove_node(&bmain, tree, *copy.mix, false);
      }
      if (copy.tex != nullptr) {
        copy.tex->id = nullptr;
        bke::node_remove_node(&bmain, tree, *copy.tex, false);
      }
      if (copy.image != nullptr) {
        BKE_id_free(&bmain, copy.image);
      }
    }
    copies.clear();
  };

  for (ChannelChain *chain_ptr : plan.chains) {
    ChannelChain &chain = *chain_ptr;
    ChainLayer &source = chain.layers[layer_index];
    DuplicatedLayer copy;
    copy.chain = &chain;

    bNodeLink *top_link = sole_link_into(*source.top);
    if (top_link == nullptr) {
      copies.append(copy);
      discard();
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    if (source.image != nullptr) {
      /* A copy that shared the original's maps would be the same layer listed twice. */
      copy.image = id_cast<Image *>(BKE_id_copy(&bmain, &source.image->id));
      if (copy.image == nullptr) {
        copies.append(copy);
        discard();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
    }
    Map<const bNodeSocket *, bNodeSocket *> socket_map;
    copy.tex = bke::node_copy_with_mapping(
        &tree, *top_link->fromnode, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
    copy.mix = bke::node_copy_with_mapping(
        &tree, *source.node, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
    if (copy.tex == nullptr || copy.mix == nullptr) {
      copies.append(copy);
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    if (copy.image != nullptr) {
      /* The copied texture node still points at the original map, with the user
       * #node_copy_with_mapping gave it, and is about to stop: without this the original keeps a
       * user it no longer has. The copy needs no matching #id_us_plus -- see the group branch
       * above, and #BKE_paint_material_layer_add, which assigns its fresh maps the same way. */
      id_us_min(copy.tex->id);
      copy.tex->id = &copy.image->id;
    }
    copies.append(copy);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  /* The chains were read before the copies existed, and creating nodes moved the tree on. */
  Vector<ChannelChain> fresh_chains;
  if (!chains_collect(ma, fresh_chains, error) || !chains_align(fresh_chains, error)) {
    discard();
    return fail(error);
  }

  const bUUID layer_id = BLI_uuid_generate_random();
  const int insert_at = layer_index + 1;
  for (DuplicatedLayer &copy : copies) {
    ChannelChain *chain = nullptr;
    for (ChannelChain &candidate : fresh_chains) {
      if (candidate.channel == copy.chain->channel) {
        chain = &candidate;
        break;
      }
    }
    /* A previous channel's links, added below, invalidate the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    tree.ensure_topology_cache();
    CompositeMixNode mix;
    bNodeSocket *output = mix_output_find(*copy.mix);
    bNodeSocket *tex_color = bke::node_find_socket(*copy.tex, SOCK_OUT, "Color"_ustr);
    bNodeSocket *tex_alpha = bke::node_find_socket(*copy.tex, SOCK_OUT, "Alpha"_ustr);
    if (chain == nullptr || output == nullptr || tex_color == nullptr || tex_alpha == nullptr ||
        !composite_mix_node_read(*copy.mix, mix))
    {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }

    ChainLayer layer;
    layer.node = copy.mix;
    layer.bottom = const_cast<bNodeSocket *>(mix.bottom);
    layer.top = const_cast<bNodeSocket *>(mix.top);
    layer.output = output;
    layer.image = copy.image;

    bke::node_add_link(tree, *copy.tex, *tex_color, *copy.mix, *layer.top);
    layer_factor_coverage_link(
        tree, *copy.mix, *const_cast<bNodeSocket *>(mix.factor), *copy.tex, *tex_alpha, 1.0f);
    bke::node_position_relative(*copy.mix, *chain->terminal_node, output, *chain->terminal);
    bke::node_position_relative(*copy.tex, *copy.mix, tex_color, *layer.top);

    chain->layers.insert(insert_at, layer);
    chain_rebuild_links(*chain);
    /* A copy is a different layer, so it gets an identity of its own rather than the original's. */
    BKE_paint_material_layer_marker_set(*copy.mix, layer_id);
    if (copy.image != nullptr) {
      copy.image->paint_layer_id = layer_id;
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_ordinal != nullptr) {
    *r_ordinal = insert_at;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_mask_add(Main &bmain,
                                       Material &ma,
                                       const int ordinal,
                                       const float initial_color[4],
                                       const int image_size,
                                       PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and a bare base is refused, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskAdd, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * mask is known to be added; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskAdd, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  const int layer_index = plan.layer_index;
  /* The mask node goes in the tree the layer lives in, which is the group's for a nested row. */
  bNodeTree &tree = *plan.chains.first()->tree;

  /* One mask for the layer, shared by every channel: a layer is one thing, and a mask that
   * differed per channel would be several. */
  char mask_name[MAX_ID_NAME - 2];
  const char *layer_label = plan.chains.first()->layers[layer_index].node->label;
  SNPRINTF_UTF8(mask_name, "%s Mask", (layer_label[0] != 0) ? layer_label : "Layer");
  Image *mask = BKE_image_add_generated(&bmain,
                                        image_size,
                                        image_size,
                                        mask_name,
                                        32,
                                        false,
                                        IMA_GENTYPE_BLANK,
                                        initial_color,
                                        false,
                                        true,
                                        false);
  if (mask == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  mask->flag |= IMA_PAINT_CANVAS;
  mask->paint_layer_id = BKE_paint_material_layer_marker_get(
      *plan.chains.first()->layers[layer_index].node);
  mask->paint_layer_channel = PAINT_LAYER_MAP_MASK;

  bNode *tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  bNodeSocket *color = bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr);
  if (color == nullptr) {
    bke::node_remove_node(&bmain, tree, *tex, false);
    BKE_id_free(&bmain, mask);
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  tex->id = &mask->id;
  tree.ensure_topology_cache();

  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[layer_index];
    /* A previous channel's relink, below, invalidates the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    chain->tree->ensure_topology_cache();
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.factor == nullptr) {
      continue;
    }
    if (mix.factor_opacity != nullptr) {
      /* Coverage and the layer's own opacity already coexist: swap only what feeds the coverage
       * side, so the opacity the user may already have set survives adding a mask. */
      bNodeSocket &coverage = const_cast<bNodeSocket &>(*mix.factor_coverage);
      bNode &multiply = const_cast<bNode &>(coverage.owner_node());
      relink_into(*chain->tree, coverage, multiply, *tex, *color);
    }
    else {
      /* Either a bare constant -- carried over as the new Multiply's opacity -- or an old-style
       * mask with nothing to carry over; either way the layer keeps an editable opacity from here
       * on, wrapped fresh around the new mask. */
      float initial_opacity = 1.0f;
      if (BKE_paint_material_source_socket(*mix.factor) == nullptr) {
        initial_opacity =
            static_cast<const bNodeSocketValueFloat *>(mix.factor->default_value)->value;
      }
      layer_factor_coverage_link(*chain->tree,
                                 *layer.node,
                                 *const_cast<bNodeSocket *>(mix.factor),
                                 *tex,
                                 *color,
                                 initial_opacity);
    }
  }
  ChainLayer &masked = plan.chains.first()->layers[layer_index];
  bke::node_position_relative(*tex, *masked.node, color, *masked.top);

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_mask_remove(Main &bmain,
                                          Material &ma,
                                          const int ordinal,
                                          PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and a bare base is refused, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskRemove, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * mask is known to be removed; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskRemove, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  const int layer_index = plan.layer_index;
  bNodeTree &tree = *plan.chains.first()->tree;

  Vector<std::pair<bNodeTree *, bNode *>> mask_nodes;
  for (ChannelChain *chain_ptr : plan.chains) {
    ChannelChain &chain = *chain_ptr;
    ChainLayer &layer = chain.layers[layer_index];
    /* A previous channel's relink, below, invalidates the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    chain.tree->ensure_topology_cache();
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.factor == nullptr) {
      continue;
    }
    /* Coverage and opacity coexist: what is being removed lives on the Multiply's coverage
     * input, and its opacity constant is left exactly as it was. An old-style layer with nothing
     * to separate an opacity from is masked straight on #factor instead, same as always. */
    bNodeSocket *coverage_socket = (mix.factor_opacity != nullptr) ?
                                       const_cast<bNodeSocket *>(mix.factor_coverage) :
                                       const_cast<bNodeSocket *>(mix.factor);
    bNode &coverage_owner = const_cast<bNode &>(coverage_socket->owner_node());
    bNodeLink *mask_link = sole_link_into(*coverage_socket);
    if (mask_link == nullptr) {
      continue;
    }
    /* Coverage goes back to the layer's own map, which is where it comes from for a layer that
     * never had a mask. Resolved before deciding what "the mask" even is: a layer whose coverage
     * already comes straight from its own map -- the default shape now, not just the masked one --
     * has no mask to remove, and mistaking its own Image Texture node for one would delete it. */
    bNodeLink *map_link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
    if (map_link != nullptr && mask_link->fromnode == map_link->fromnode) {
      continue;
    }
    if (mask_link->fromsock->directly_linked_links().size() == 1) {
      mask_nodes.append_non_duplicates({chain.tree, mask_link->fromnode});
    }
    bNodeSocket *alpha = (map_link == nullptr) ?
                             nullptr :
                             bke::node_find_socket(*map_link->fromnode, SOCK_OUT, "Alpha"_ustr);
    if (alpha == nullptr) {
      /* Nothing to restore coverage from; leaving it unlinked is still a layer without a mask. */
      for (bNodeLink *link : Vector<bNodeLink *>(coverage_socket->directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(chain.tree);
        bke::node_remove_link(chain.tree, *link);
      }
      continue;
    }
    /* The map's node is taken from the link rather than from the socket: relinking the previous
     * channel already invalidated the topology cache #owner_node asserts on. */
    if (mix.factor_opacity != nullptr) {
      relink_into(*chain.tree, *coverage_socket, coverage_owner, *map_link->fromnode, *alpha);
    }
    else {
      /* No opacity to preserve here -- wrap the restored coverage in a fresh Multiply so the
       * layer keeps an editable one going forward. */
      layer_factor_coverage_link(
          *chain.tree, *layer.node, *coverage_socket, *map_link->fromnode, *alpha, 1.0f);
    }
  }

  Set<bNodeTree *> touched_trees;
  for (const std::pair<bNodeTree *, bNode *> &entry : mask_nodes) {
    bke::node_remove_node(&bmain, *entry.first, *entry.second, true);
    BKE_ntree_update_tag_node_removed(entry.first);
    touched_trees.add(entry.first);
  }
  for (bNodeTree *touched : touched_trees) {
    if (touched != ma.nodetree) {
      BKE_ntree_update_after_single_tree_change(bmain, *touched);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

void BKE_paint_material_layer_channels_wired(Material &ma, Vector<int> &r_channels)
{
  r_channels.clear();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  Vector<ChannelChain> chains;
  if (!chains_collect(ma, chains, error)) {
    return;
  }
  for (const ChannelChain &chain : chains) {
    r_channels.append(chain.channel);
  }
}

namespace {

/**
 * Whether the Principled input of \a channel carries a user link right now.
 *
 * A missing socket (older Principled without this input) reads as unlinked: the caller asked
 * for this channel, so a missing socket skips rather than denies (same as the forest reader,
 * which simply never reports such a channel).
 */
bool ensure_principled_input_is_linked(Material &ma, const int channel)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return false;
  }
  bNodeSocket *socket = socket_find_by_name(
      const_cast<bNode &>(*principled), SOCK_IN, StringRef(info.socket_name));
  if (socket == nullptr) {
    return false;
  }
  return socket_has_link(*socket);
}

/** The Principled input of \a channel, or null when there is no Principled or no such input. */
bNodeSocket *ensure_principled_input_find(Material &ma, const int channel)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return nullptr;
  }
  return socket_find_by_name(
      const_cast<bNode &>(*principled), SOCK_IN, StringRef(info.socket_name));
}

/** A socket's own default as a fill color: scalars ride the red channel, like fills do. */
void ensure_socket_default_color(const bNodeSocket &socket, float r_color[4])
{
  switch (socket.type) {
    case SOCK_FLOAT: {
      const float value = static_cast<const bNodeSocketValueFloat *>(socket.default_value)->value;
      r_color[0] = value;
      r_color[1] = value;
      r_color[2] = value;
      r_color[3] = 1.0f;
      break;
    }
    case SOCK_RGBA: {
      const float *value = static_cast<const bNodeSocketValueRGBA *>(socket.default_value)->value;
      r_color[0] = value[0];
      r_color[1] = value[1];
      r_color[2] = value[2];
      r_color[3] = value[3];
      break;
    }
    case SOCK_VECTOR: {
      const float *value = static_cast<const bNodeSocketValueVector *>(socket.default_value)
                               ->value;
      r_color[0] = value[0];
      r_color[1] = value[1];
      r_color[2] = value[2];
      r_color[3] = 1.0f;
      break;
    }
    default:
      r_color[0] = 0.0f;
      r_color[1] = 0.0f;
      r_color[2] = 0.0f;
      r_color[3] = 1.0f;
      break;
  }
}

/** Size of the map \a ref shows, so a neutral mirror stays a drop-in tile. Falls back silently. */
void ensure_ref_image_size(Image *ref, int &r_size_x, int &r_size_y)
{
  if (ref == nullptr) {
    return;
  }
  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ref, &iuser, &lock);
  if (ibuf == nullptr) {
    return;
  }
  if (ibuf->x > 0 && ibuf->y > 0) {
    r_size_x = ibuf->x;
    r_size_y = ibuf->y;
  }
  BKE_image_release_ibuf(ref, ibuf, lock);
}

/**
 * A neutral map for \a channel: transparent, so a row fed by it contributes nothing -- except
 * the bottom of a top-level chain, which is opaque and carries \a opaque_color (the constant
 * the free Principled input used to supply on its own).
 */
Image *ensure_neutral_image_create(Main &bmain,
                                   const int channel,
                                   const int size_x,
                                   const int size_y,
                                   const bool opaque,
                                   const float opaque_color[4])
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char image_name[MAX_ID_NAME - 2];
  SNPRINTF_UTF8(image_name, "%s Neutral", info.ui_name);
  float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* Flat tangent space; the alpha still decides coverage, like on every channel. */
    color[0] = 0.5f;
    color[1] = 0.5f;
    color[2] = 1.0f;
  }
  if (opaque) {
    if (opaque_color != nullptr && channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
      color[0] = opaque_color[0];
      color[1] = opaque_color[1];
      color[2] = opaque_color[2];
    }
    color[3] = 1.0f;
  }
  Image *image = BKE_image_add_generated(&bmain,
                                         size_x,
                                         size_y,
                                         image_name,
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         color,
                                         false,
                                         !info.is_color,
                                         false);
  if (image != nullptr) {
    image->flag |= IMA_PAINT_CANVAS;
  }
  return image;
}

/** How #ensure_discard releases one created node. */
enum class EnsureNodeKind : int8_t {
  /** Plain node with no ID users of its own (Mix, Normal Map). */
  Plain = 0,
  /** Group instance this transaction gave a user (`id_us_plus`): give it back. */
  Instance,
  /** Image Texture showing a transaction image: clear the reference before the image goes. */
  Texture,
};

struct EnsureCreated {
  bNodeTree *tree = nullptr;
  bNode *node = nullptr;
  EnsureNodeKind kind = EnsureNodeKind::Plain;
};

/**
 * Undo one channel's node creation. Interface sockets added to group trees stay behind:
 * unlinked they are transparent, which is exactly what an empty Result contributes.
 */
void ensure_discard(Main &bmain, Vector<EnsureCreated> &r_created, Vector<Image *> &r_images)
{
  for (const EnsureCreated &created : r_created) {
    if (created.node == nullptr || created.tree == nullptr) {
      continue;
    }
    if (created.kind == EnsureNodeKind::Instance && created.node->id != nullptr) {
      id_us_min(created.node->id);
      created.node->id = nullptr;
    }
    if (created.kind == EnsureNodeKind::Texture) {
      created.node->id = nullptr;
    }
    bke::node_remove_node(&bmain, *created.tree, *created.node, false);
  }
  r_created.clear();
  for (Image *image : r_images) {
    if (image != nullptr) {
      BKE_id_free(&bmain, image);
    }
  }
  r_images.clear();
}

/** One built row: the nodes a reference row mirrors into. */
struct EnsureBuiltRow {
  bNode *tex = nullptr;
  Image *image = nullptr;
  bNode *mix = nullptr;
  bNode *instance = nullptr;
  bool is_group = false;
};

/** Where a mirrored chain feeds: resolved fresh after the tree update, never cached across one. */
struct EnsureTerminal {
  bNode *node = nullptr;
  eNodeSocketInOut in_out = SOCK_IN;
  const char *socket_name = nullptr;
};

/** The Group Output node of \a group_tree, or null when the folder lost it. */
bNode *ensure_group_output_find(bNodeTree &group_tree)
{
  for (bNode &node : group_tree.nodes) {
    if (node.type_legacy == NODE_GROUP_OUTPUT) {
      return &node;
    }
  }
  return nullptr;
}

/**
 * The `Result <Channel>` input of \a group_tree's output, adding the interface socket first
 * when this channel reaches the folder for the first time.
 */
bool ensure_group_result_socket(Main &bmain,
                                bNodeTree &group_tree,
                                const int channel,
                                PaintMaterialLayerEditError &r_error)
{
  bNode *output = ensure_group_output_find(group_tree);
  if (output == nullptr) {
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char result_name[64];
  SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
  if (socket_find_by_name(*output, SOCK_IN, StringRef(result_name)) != nullptr) {
    return true;
  }
  if (group_tree.tree_interface.add_socket(
          result_name, "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr) == nullptr)
  {
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }
  BKE_ntree_update_after_single_tree_change(bmain, group_tree);
  group_tree.ensure_topology_cache();
  return true;
}

/**
 * Build one chain mirroring \a ref_chain's rows for \a channel, bottom to top.
 *
 * Plain rows get a neutral map (transparent, except an opaque socket-default bottom on a
 * top-level chain) blended by a fresh Mix node; group rows get a fresh instance of the same
 * folder blended by a keeper Mix. Every Mix carries its reference row's marker -- minted onto
 * the reference itself when an old stack never got one -- so the row keeps one identity in
 * every channel at once.
 */
static bool ensure_mirror_chain(Main &bmain,
                                bNodeTree &tree,
                                const int channel,
                                const ChannelChain &ref_chain,
                                const EnsureTerminal &terminal,
                                const bool bottom_opaque,
                                const float bottom_color[4],
                                Vector<EnsureCreated> &r_created,
                                Vector<Image *> &r_images,
                                int &io_fallback_size,
                                PaintMaterialLayerEditError &r_error)
{
  /* 1. Create every node first: group instances only grow their sockets on a tree update. */
  Vector<EnsureBuiltRow> rows;
  for (const int64_t j : ref_chain.layers.index_range()) {
    /* The caller (and a previous iteration) may have left the tree's topology cache dirty;
     * #composite_source_node_shallow below reads a reference socket's links. */
    tree.ensure_topology_cache();

    const ChainLayer &ref_layer = ref_chain.layers[j];
    if (!ref_layer.is_mix() || ref_layer.node == nullptr || ref_layer.top == nullptr) {
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }
    bUUID row_id = BKE_paint_material_layer_marker_get(*ref_layer.node);
    if (BLI_uuid_is_nil(row_id)) {
      row_id = BLI_uuid_generate_random();
      BKE_paint_material_layer_marker_set(*ref_layer.node, row_id);
    }

    EnsureBuiltRow row;
    row.is_group = ref_layer.is_group;
    if (!ref_layer.is_group) {
      int size_x = io_fallback_size, size_y = io_fallback_size;
      ensure_ref_image_size(ref_layer.image, size_x, size_y);
      io_fallback_size = size_x;
      const bool opaque = bottom_opaque && j == 0;
      row.image = ensure_neutral_image_create(
          bmain, channel, size_x, size_y, opaque, opaque ? bottom_color : nullptr);
      if (row.image == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      r_images.append(row.image);
      row.image->paint_layer_id = row_id;
      row.tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      if (row.tex == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      row.tex->id = &row.image->id;
      r_created.append({&tree, row.tex, EnsureNodeKind::Texture});
    }
    else {
      const bNode *ref_instance = composite_source_node_shallow(*ref_layer.top);
      if (ref_instance == nullptr || ref_instance->id == nullptr || tree.typeinfo == nullptr ||
          tree.typeinfo->group_idname == nullptr)
      {
        r_error = PaintMaterialLayerEditError::ChainNotPlain;
        return false;
      }
      row.instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (row.instance == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      row.instance->id = ref_instance->id;
      id_us_plus(row.instance->id);
      r_created.append({&tree, row.instance, EnsureNodeKind::Instance});
    }
    row.mix = layer_mix_node_create(bmain, tree, channel);
    if (row.mix == nullptr) {
      r_error = PaintMaterialLayerEditError::CreationFailed;
      return false;
    }
    r_created.append({&tree, row.mix, EnsureNodeKind::Plain});
    if (ref_layer.node->label[0] != '\0') {
      STRNCPY_UTF8(row.mix->label, ref_layer.node->label);
    }
    if ((ref_layer.node->flag & NODE_MUTED) != 0) {
      row.mix->flag |= NODE_MUTED;
    }
    BKE_paint_material_layer_marker_set(*row.mix, row_id);
    rows.append(row);
  }

  /* 2. Sockets exist only after the update; resolve everything before touching a link. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  bNodeSocket *terminal_socket = socket_find_by_name(
      *terminal.node, terminal.in_out, StringRef(terminal.socket_name));
  if (terminal_socket == nullptr) {
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }

  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char result_name[64];
  SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);

  ChannelChain built;
  built.tree = &tree;
  built.channel = channel;
  built.terminal = terminal_socket;
  built.terminal_node = terminal.node;
  for (EnsureBuiltRow &row : rows) {
    /* The previous iteration wired a link and added a coverage Multiply node, which leaves the
     * topology cache dirty; #composite_mix_node_read walks the Factor's links, so it needs the
     * cache rebuilt from the graph as it stands now. */
    tree.ensure_topology_cache();

    CompositeMixNode mix_prm;
    bNodeSocket *output = mix_output_find(*row.mix);
    const bool mix_read = (output == nullptr) ?
                              false :
                              composite_mix_node_read(*row.mix, mix_prm);
    if (output == nullptr || !mix_read) {
      r_error = PaintMaterialLayerEditError::CreationFailed;
      return false;
    }
    ChainLayer layer;
    layer.node = row.mix;
    layer.bottom = const_cast<bNodeSocket *>(mix_prm.bottom);
    layer.top = const_cast<bNodeSocket *>(mix_prm.top);
    layer.factor = const_cast<bNodeSocket *>(mix_prm.factor);
    layer.output = output;
    layer.image = row.image;
    if (!row.is_group) {
      bNodeSocket *tex_color = bke::node_find_socket(*row.tex, SOCK_OUT, "Color"_ustr);
      bNodeSocket *tex_alpha = bke::node_find_socket(*row.tex, SOCK_OUT, "Alpha"_ustr);
      if (tex_color == nullptr || tex_alpha == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      bke::node_add_link(tree, *row.tex, *tex_color, *row.mix, *layer.top);
      if (!layer_factor_coverage_link(
              tree, *row.mix, *layer.factor, *row.tex, *tex_alpha, 1.0f))
      {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      bke::node_position_relative(*row.tex, *row.mix, tex_color, *layer.top);
    }
    else {
      /* The folder gained its Result socket up front; a missing one now means the interface
       * and the instances disagree. */
      bNodeSocket *result = socket_find_by_name(*row.instance, SOCK_OUT, result_name);
      bNodeSocket *alpha = socket_find_by_name(*row.instance, SOCK_OUT, "Alpha");
      if (result == nullptr || alpha == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      bke::node_add_link(tree, *row.instance, *result, *row.mix, *layer.top);
      if (!layer_factor_coverage_link(
              tree, *row.mix, *layer.factor, *row.instance, *alpha, 1.0f))
      {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      layer.is_group = true;
      bke::node_position_relative(*row.instance, *row.mix, result, *layer.top);
    }
    bke::node_position_relative(*row.mix, *terminal.node, layer.output, *terminal_socket);
    built.layers.append(layer);
  }

  /* 3. Wire bottom-up; the terminal takes the top, like every other chain. */
  chain_rebuild_links(built);
  return true;
}

/**
 * Migrate one missing channel into \a ref_forest's shape: primary wiring at the Principled
 * input (a fresh Normal Map for Normal), a `Result` interface socket on every folder, and a
 * mirrored chain in every tree. Either the whole channel lands or nothing does.
 */
static bool ensure_migrate_channel(Main &bmain,
                                   Material &ma,
                                   const int channel,
                                   const Vector<ChannelChain> &ref_forest,
                                   PaintMaterialLayerEditError &r_error)
{
  Vector<EnsureCreated> created;
  Vector<Image *> images;
  auto fail = [&](const PaintMaterialLayerEditError error) {
    ensure_discard(bmain, created, images);
    r_error = error;
    return false;
  };

  /* Folders this channel reaches for the first time need their Result socket up front:
   * the outer instances only grow it on a tree update. */
  Vector<bNodeTree *> group_trees;
  Set<bNodeTree *> group_seen;
  for (const ChannelChain &ref_chain : ref_forest) {
    for (const ChainLayer &ref_layer : ref_chain.layers) {
      if (!ref_layer.is_group || ref_layer.top == nullptr) {
        continue;
      }
      const bNode *ref_instance = composite_source_node_shallow(*ref_layer.top);
      bNodeTree *group_tree = (ref_instance == nullptr) ? nullptr :
                                                            layer_group_tree_of(*ref_instance);
      if (group_tree == nullptr) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      if (group_seen.add(group_tree)) {
        group_trees.append(group_tree);
      }
    }
  }

  /* The Normal chain starts one node earlier, at a fresh Normal Map: the Principled Normal
   * input carries an already transformed vector no stack can be recovered from. */
  bNode *normal_map = nullptr;
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    normal_map = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_NORMAL_MAP);
    if (normal_map == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    created.append({ma.nodetree, normal_map, EnsureNodeKind::Plain});
    bNodeSocket *map_color = bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr);
    bNodeSocket *map_normal = bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr);
    bNodeSocket *principled_normal = ensure_principled_input_find(ma, channel);
    if (map_color == nullptr || map_normal == nullptr || principled_normal == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    bNode &principled = const_cast<bNode &>(principled_normal->owner_node());
    bke::node_add_link(*ma.nodetree, *normal_map, *map_normal, principled, *principled_normal);
    bke::node_position_relative(*normal_map, principled, map_normal, *principled_normal);
  }

  for (bNodeTree *group_tree : group_trees) {
    if (!ensure_group_result_socket(bmain, *group_tree, channel, r_error)) {
      ensure_discard(bmain, created, images);
      return false;
    }
  }

  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  const int top_index = int(ref_forest.size()) - 1;
  int fallback_size = 1024;
  for (const int64_t k : ref_forest.index_range()) {
    const ChannelChain &ref_chain = ref_forest[k];
    EnsureTerminal terminal;
    float bottom_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool bottom_opaque = false;
    char result_name[64] = "";
    if (k == top_index) {
      /* The bottom keeps showing what the free input used to supply on its own. */
      bottom_opaque = true;
      if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
        bottom_color[0] = 0.5f;
        bottom_color[1] = 0.5f;
        bottom_color[2] = 1.0f;
        bottom_color[3] = 1.0f;
        terminal.node = normal_map;
        terminal.in_out = SOCK_IN;
        terminal.socket_name = "Color";
      }
      else {
        bNodeSocket *input = ensure_principled_input_find(ma, channel);
        if (input == nullptr) {
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        ensure_socket_default_color(*input, bottom_color);
        terminal.node = &const_cast<bNode &>(input->owner_node());
        terminal.in_out = SOCK_IN;
        terminal.socket_name = info.socket_name;
      }
    }
    else {
      bNode *output = ensure_group_output_find(*ref_chain.tree);
      if (output == nullptr) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
      terminal.node = output;
      terminal.in_out = SOCK_IN;
      terminal.socket_name = result_name;
    }
    if (!ensure_mirror_chain(bmain,
                             *ref_chain.tree,
                             channel,
                             ref_chain,
                             terminal,
                             bottom_opaque,
                             bottom_color,
                             created,
                             images,
                             fallback_size,
                             r_error))
    {
      ensure_discard(bmain, created, images);
      return false;
    }
  }
  return true;
}

}  // namespace

bool BKE_paint_material_layer_channels_ensure(Main &bmain,
                                              Material &ma,
                                              Span<int> channels,
                                              PaintMaterialLayerEditError *r_error)
{
  auto fail = [&](const PaintMaterialLayerEditError error) {
    if (r_error != nullptr) {
      *r_error = error;
    }
    /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
    printf("[MAT_LAYER]   channels_ensure FAIL err=%d\n", int(error));
    fflush(stdout);
    return false;
  };
  /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
  printf("[MAT_LAYER]   channels_ensure enter channels=%d\n", int(channels.size()));
  fflush(stdout);
  /* Channels with no Principled input (Custom, Height, AO) can never be wired: asking for
   * them is meaningless, so they drop out before the forest is even read. */
  Vector<int> work;
  for (const int channel : channels) {
    if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
    if (BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).socket_name == nullptr) {
      continue;
    }
    work.append_non_duplicates(channel);
  }
  if (work.is_empty()) {
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  }
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Empty material takes path E (#BKE_paint_material_layer_add_material_base); there is no
   * forest to mirror here. */
  Vector<Vector<ChannelChain>> forest;
  PaintMaterialLayerEditError forest_error = PaintMaterialLayerEditError::None;
  if (!chains_collect_forest(ma, forest, forest_error)) {
    return fail(forest_error);
  }
  /* The stack the other operations already refuse to rewrite is refused here too, before a
   * byte is written. */
  {
    const int64_t top_len = forest.first().last().layers.size();
    for (const Vector<ChannelChain> &per_channel : forest) {
      if (per_channel.last().layers.size() != top_len) {
        return fail(PaintMaterialLayerEditError::ChannelsDisagree);
      }
    }
  }
  {
    Set<const bNodeTree *> scope_seen;
    const auto scope_check = [&](bNodeTree &tree) {
      if (scope_seen.add(&tree)) {
        PaintMaterialLayerEditError scope_error = PaintMaterialLayerEditError::None;
        if (!tree_write_scope_check(bmain, ma, tree, scope_error)) {
          forest_error = scope_error;
          return false;
        }
      }
      return true;
    };
    if (!scope_check(*ma.nodetree)) {
      return fail(forest_error);
    }
    for (const ChannelChain &ref_chain : forest.first()) {
      for (const ChainLayer &ref_layer : ref_chain.layers) {
        if (!ref_layer.is_group || ref_layer.top == nullptr) {
          continue;
        }
        const bNode *ref_instance = composite_source_node_shallow(*ref_layer.top);
        bNodeTree *group_tree = (ref_instance == nullptr) ? nullptr :
                                                          layer_group_tree_of(*ref_instance);
        if (group_tree == nullptr) {
          return fail(PaintMaterialLayerEditError::ChainNotPlain);
        }
        if (!scope_check(*group_tree)) {
          return fail(forest_error);
        }
      }
    }
  }
  auto have_channel = [&](const Vector<Vector<ChannelChain>> &look, const int channel) {
    for (const Vector<ChannelChain> &per_channel : look) {
      if (!per_channel.is_empty() && per_channel.last().channel == channel) {
        return true;
      }
    }
    return false;
  };
  for (const int channel : work) {
    if (have_channel(forest, channel)) {
      continue;
    }
    /* A link that is not a stack is someone's deliberate graph: replacing it would silently
     * change what the material looks like. */
    if (ensure_principled_input_is_linked(ma, channel)) {
      return fail(PaintMaterialLayerEditError::ChannelHasUnsupportedSource);
    }
  }
  /* Bare bottoms cannot lend a marker to their mirror: wrap them first, the way every other
   * operation does once its preflight has passed. */
  if (forest_has_bare_bottom(forest)) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
    }
    if (!chains_collect_forest(ma, forest, forest_error)) {
      return fail(forest_error);
    }
  }
  for (const int channel : work) {
    if (have_channel(forest, channel)) {
      continue;
    }
    if (!ensure_migrate_channel(bmain, ma, channel, forest.first(), forest_error)) {
      /* #ensure_migrate_channel unwinds its own channel, but a channel migrated on an earlier
       * pass of this loop stays in the graph (as does a `Result` socket it added to a folder).
       * The operator that drives this is #OPTYPE_UNDO, so a single undo step still takes the
       * whole partial migration back; a self-contained rollback across channels is a follow-up. */
      return fail(forest_error);
    }
    /* The new channel invalidates the collected topology: read the forest again so the next
     * channel mirrors a graph as it is, with the markers just minted still on it. */
    if (!chains_collect_forest(ma, forest, forest_error)) {
      return fail(forest_error);
    }
  }
  BKE_paint_material_layer_markers_ensure(ma);
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

/**
 * Path E: the first layer of an empty material, built directly as one normalized Mix row
 * per baked channel -- never as bare images, which carry no marker and would leave the row
 * without one identity across its channels.
 *
 * Takes \a baked_maps over like #BKE_paint_material_layer_add does: shown maps keep their
 * fresh user on the node, and every other map -- all of them on refusal -- is freed.
 */
bool BKE_paint_material_layer_add_material_base(Main &bmain,
                                                Material &ma,
                                                Span<PaintMaterialLayerChannelImage> baked_maps,
                                                int *r_ordinal,
                                                PaintMaterialLayerEditError *r_error)
{
  Set<Image *> given_used;
  auto given_release = [&]() {
    Set<Image *> released;
    for (const PaintMaterialLayerChannelImage &given : baked_maps) {
      if (given.image != nullptr && !given_used.contains(given.image) &&
          released.add(given.image))
      {
        BKE_id_free(&bmain, given.image);
      }
    }
  };
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    given_used.clear();
    given_release();
    if (r_error != nullptr) {
      *r_error = reason;
    }
    /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
    printf("[MAT_LAYER]   add_material_base FAIL err=%d\n", int(reason));
    fflush(stdout);
    return false;
  };

  /* TEMP-DEBUG [MAT_LAYER]: remove before merge. */
  printf("[MAT_LAYER]   add_material_base enter maps=%d\n", int(baked_maps.size()));
  fflush(stdout);

  if (baked_maps.is_empty()) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Path E is only for an empty canvas: a stack already there takes the ensure+add route. */
  {
    Vector<Vector<ChannelChain>> existing;
    PaintMaterialLayerEditError existing_error = PaintMaterialLayerEditError::None;
    if (chains_collect_forest(ma, existing, existing_error)) {
      return fail(PaintMaterialLayerEditError::ChannelsDisagree);
    }
    if (existing_error != PaintMaterialLayerEditError::NotAStack) {
      return fail(existing_error);
    }
  }
  ChannelUnavailableReason principled_reason = ChannelUnavailableReason::None;
  if (BKE_paint_material_principled_find(ma, principled_reason) == nullptr) {
    return fail(PaintMaterialLayerEditError::NoPrincipled);
  }
  /* Validate everything up front: channel ids, duplicates, and free Principled inputs. A
   * linked input here is someone's graph, not an empty canvas -- same policy as ensure. */
  Set<int> seen_channels;
  for (const PaintMaterialLayerChannelImage &given : baked_maps) {
    if (given.image == nullptr || given.channel < 0 ||
        given.channel >= PAINT_MATERIAL_CHANNEL_NUM ||
        BKE_paint_material_channel_info(eMaterialPaintChannel(given.channel)).socket_name ==
            nullptr)
    {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
    if (!seen_channels.add(given.channel)) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    if (ensure_principled_input_is_linked(ma, given.channel)) {
      return fail(PaintMaterialLayerEditError::ChannelHasUnsupportedSource);
    }
  }

  bNodeTree &tree = *ma.nodetree;
  if (tree.typeinfo == nullptr || tree.typeinfo->group_idname == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }

  /* One identity for the whole row, restamped onto the baked maps the way layer_add does:
   * the bake's own layer id only ever lived in the bake link. */
  const bUUID layer_id = BLI_uuid_generate_random();

  struct BaseRow {
    int channel = -1;
    bNode *tex = nullptr;
    bNode *mix = nullptr;
  };
  Vector<BaseRow> rows;
  Vector<EnsureCreated> created;
  auto fail_nodes = [&](const PaintMaterialLayerEditError reason) {
    Vector<Image *> owned;
    ensure_discard(bmain, created, owned);
    return fail(reason);
  };

  /* Normal starts one node earlier, at its own Normal Map (same as ensure's primary). */
  bNode *normal_map = nullptr;
  for (const PaintMaterialLayerChannelImage &given : baked_maps) {
    if (given.channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
      continue;
    }
    normal_map = bke::node_add_static_node(nullptr, tree, SH_NODE_NORMAL_MAP);
    if (normal_map == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    created.append({&tree, normal_map, EnsureNodeKind::Plain});
    bNodeSocket *map_color = bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr);
    bNodeSocket *map_normal = bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr);
    bNodeSocket *principled_normal = ensure_principled_input_find(ma, given.channel);
    if (map_color == nullptr || map_normal == nullptr || principled_normal == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    bNode &principled = const_cast<bNode &>(principled_normal->owner_node());
    bke::node_add_link(tree, *normal_map, *map_normal, principled, *principled_normal);
    bke::node_position_relative(*normal_map, principled, map_normal, *principled_normal);
  }

  for (const PaintMaterialLayerChannelImage &given : baked_maps) {
    BaseRow row;
    row.channel = given.channel;
    row.tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    if (row.tex == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    row.tex->id = &given.image->id;
    given_used.add(given.image);
    created.append({&tree, row.tex, EnsureNodeKind::Texture});
    row.mix = layer_mix_node_create(bmain, tree, given.channel);
    if (row.mix == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    created.append({&tree, row.mix, EnsureNodeKind::Plain});
    BKE_paint_material_layer_marker_set(*row.mix, layer_id);
    given.image->paint_layer_id = layer_id;
    /* A baked map goes on to be a layer canvas like any map #layer_image_create makes; the ID
     * browser's paint-canvas view keys off this flag together with #paint_layer_id. */
    given.image->flag |= IMA_PAINT_CANVAS;
    rows.append(row);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);

  for (BaseRow &row : rows) {
    /* The previous iteration wired links and added a coverage Multiply node; rebuild the
     * topology cache before #composite_mix_node_read walks the Factor's links again. */
    tree.ensure_topology_cache();

    CompositeMixNode mix_prm;
    bNodeSocket *output = mix_output_find(*row.mix);
    bNodeSocket *tex_color = bke::node_find_socket(*row.tex, SOCK_OUT, "Color"_ustr);
    bNodeSocket *tex_alpha = bke::node_find_socket(*row.tex, SOCK_OUT, "Alpha"_ustr);
    if (output == nullptr || tex_color == nullptr || tex_alpha == nullptr ||
        !composite_mix_node_read(*row.mix, mix_prm))
    {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    /* #CompositeMixNode hands out const sockets; links need mutable ones, like the layer
     * structs everywhere else in this file. */
    bNodeSocket *mix_top = const_cast<bNodeSocket *>(mix_prm.top);
    bNodeSocket *mix_factor = const_cast<bNodeSocket *>(mix_prm.factor);
    bNodeSocket *terminal = nullptr;
    bNode *terminal_node = nullptr;
    if (row.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      if (normal_map == nullptr) {
        return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
      }
      terminal = bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr);
      terminal_node = normal_map;
    }
    else {
      terminal = ensure_principled_input_find(ma, row.channel);
      terminal_node = (terminal == nullptr) ?
                          nullptr :
                          &const_cast<bNode &>(terminal->owner_node());
    }
    if (terminal == nullptr || terminal_node == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    bke::node_add_link(tree, *row.tex, *tex_color, *row.mix, *mix_top);
    if (!layer_factor_coverage_link(
            tree, *row.mix, *mix_factor, *row.tex, *tex_alpha, 1.0f))
    {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    bke::node_position_relative(*row.tex, *row.mix, tex_color, *mix_top);
    bke::node_position_relative(*row.mix, *terminal_node, output, *terminal);
    relink_into(tree, *terminal, *terminal_node, *row.mix, *output);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  given_release();
  if (r_ordinal != nullptr) {
    *r_ordinal = 0;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

Image *BKE_paint_material_layer_neutral_image_create(Main &bmain,
                                                     const int channel,
                                                     const int size)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || size <= 0) {
    return nullptr;
  }
  return ensure_neutral_image_create(bmain, channel, size, size, false, nullptr);
}

bool BKE_paint_material_layer_channel_image_set(Main &bmain,
                                                Material &ma,
                                                const int ordinal,
                                                const int channel,
                                                Image &image,
                                                PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 1. Preflight: the row exists and the material is writable, decided without writing a byte.
   * A bare base is accepted -- see the plan's case for why this operation is the one exception. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::ChannelImageSet, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: nothing to convert, and #plan.needs_bottom_normalize is ignored on purpose -- a
   * bare base takes the image as its own map, a Mix layer takes it through its existing top
   * socket. The chain of the one channel asked for is what gets touched. */
  ChannelChain *target_chain = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target_chain = chain;
      break;
    }
  }
  if (target_chain == nullptr) {
    /* A material may wire Base Color and leave Roughness constant: a channel with no stack has
     * no map of this layer for the image to land in. */
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  ChainLayer &layer = target_chain->layers[plan.layer_index];
  bNodeTree &tree = *target_chain->tree;

  /* The map node owns exactly one user of the image it shows: gaining one costs the replacement,
   * and what the replacement orphans -- a generated blank nobody else holds -- is freed at once
   * instead of lingering in the file until a purge. */
  auto orphan_check = [&bmain](Image *previous) {
    if (previous != nullptr) {
      id_us_min(&previous->id);
      if (previous->id.us == 0 && previous->source == IMA_SRC_GENERATED) {
        BKE_id_free(&bmain, previous);
      }
    }
  };

  if (!layer.is_mix()) {
    /* The bare base is the channel's own Image Texture, so the assignment is that node's image.
     * It carries no layer marker, and the image it takes loses any stale one with it: an image
     * tagged as some other layer's map must not stay tagged once it becomes the base. */
    if (layer.node->id == nullptr || GS(layer.node->id->name) != ID_IM) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    Image *previous = id_cast<Image *>(layer.node->id);
    layer.node->id = &image.id;
    if (previous != &image) {
      id_us_plus(&image.id);
      orphan_check(previous);
    }
    image.paint_layer_id = bUUID{};
  }
  else {
    tree.ensure_topology_cache();
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.top == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    const bUUID layer_marker = BKE_paint_material_layer_marker_get(*layer.node);
    /* The map this layer already shows, when it has one: a single Image Texture feeding the map
     * input and tagged as this layer's. Reusing it keeps the node the user may have moved or
     * renamed; anything else feeding the input is replaced by a fresh map node. */
    bNodeSocket &top = const_cast<bNodeSocket &>(*mix.top);
    bNodeLink *map_link = sole_link_into(top);
    bNode *map_node = (map_link != nullptr) ? map_link->fromnode : nullptr;
    bool reuse = (map_node != nullptr) && map_node->type_legacy == SH_NODE_TEX_IMAGE &&
                 (map_node->id == nullptr ||
                  (GS(map_node->id->name) == ID_IM &&
                   BLI_uuid_equal(id_cast<Image *>(map_node->id)->paint_layer_id, layer_marker)));
    if (reuse) {
      Image *previous = (map_node->id != nullptr && GS(map_node->id->name) == ID_IM) ?
                            id_cast<Image *>(map_node->id) :
                            nullptr;
      map_node->id = &image.id;
      if (previous != &image) {
        /* The node's user moves from the old image to the new one. */
        id_us_plus(&image.id);
        orphan_check(previous);
      }
    }
    else {
      bNode *tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      bNodeSocket *color = bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr);
      if (color == nullptr) {
        bke::node_remove_node(&bmain, tree, *tex, false);
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      tex->id = &image.id;
      id_us_plus(&image.id);
      bke::node_position_relative(*tex, *layer.node, nullptr, *const_cast<bNodeSocket *>(mix.top));
      if (map_link != nullptr) {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *map_link);
      }
      bke::node_add_link(tree, *tex, *color, *layer.node, *const_cast<bNodeSocket *>(mix.top));
    }
    image.paint_layer_id = layer_marker;
  }
  image.paint_layer_channel = channel;

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  /* The map may sit in a folder's own node tree, whose evaluated copy is separate from the
   * material's; see #paint_layer_edit_committed for why it has to be refreshed. */
  if (&tree != ma.nodetree) {
    DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
  }
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

/**
 * Overwrite every pixel of \a image with \a color, and tell the readers the pixels moved.
 *
 * \a color is in the same convention #BKE_image_add_generated takes, so a refill and a fresh map
 * agree on what a colour means.
 */
static void image_fill_flat(Image &image, const float color[4])
{
  /* A generated map nobody has painted is rebuilt from its tile's colour whenever its buffer is
   * dropped -- a file reload, a memory purge -- so that colour has to move with the pixels, or the
   * refill silently reverts to the colour the layer was created with. A map that has been painted
   * is saved from its buffer instead, and the refill makes it dirty like any other edit. */
  const bool regenerates = image.source == IMA_SRC_GENERATED && !BKE_image_is_dirty(&image);
  if (regenerates) {
    if (ImageTile *tile = BKE_image_get_tile(&image, 0)) {
      copy_v4_v4(tile->gen_color, color);
    }
  }

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &iuser, &lock);
  if (ibuf != nullptr) {
    /* The same colour-space handling the generator applies (see `add_ibuf_for_tile`): a byte
     * buffer takes the colour as given, a float buffer of a colour map takes it linearized. */
    if (uint8_t *bytes = ibuf->byte_data_for_write()) {
      BKE_image_buf_fill_color(bytes, nullptr, ibuf->x, ibuf->y, color);
    }
    if (float *floats = ibuf->float_data_for_write()) {
      float float_color[4];
      if (IMB_colormanagement_space_name_is_data(image.colorspace_settings.name)) {
        copy_v4_v4(float_color, color);
      }
      else {
        srgb_to_linearrgb_v4(float_color, color);
      }
      BKE_image_buf_fill_color(nullptr, floats, ibuf->x, ibuf->y, float_color);
    }
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    if (!regenerates) {
      BKE_image_mark_dirty(&image, ibuf);
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }
  /* The Outliner row and every other consumer of the ID's preview icon draw the cached
   * thumbnail: without clearing it here they keep showing the pre-fill colour until something
   * else happens to invalidate it. The next draw re-renders it from the new pixels (as a job),
   * so both picker ticks and the bake refresh the row preview. */
  if (image.preview != nullptr) {
    BKE_previewimg_clear(image.preview);
  }
  BKE_image_partial_update_mark_full_update(&image);
}

bool BKE_paint_material_layer_fill_color_apply(Main &bmain,
                                               Material &ma,
                                               const int ordinal,
                                               const float color[4],
                                               PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists, the material is writable, and every channel's node for the row
   * carries the Fill kind -- all decided without writing a byte. Re-filling a painted layer would
   * silently destroy work, so the kind check runs across all channels before the first pixel. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return fail(error);
  }
  for (const ChannelChain *chain : plan.chains) {
    const ChainLayer &layer = chain->layers[plan.layer_index];
    if (BKE_paint_material_layer_kind_get(*layer.node) != PaintMaterialLayerKind::Fill) {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
  }
  /* 2. Shape: nothing to convert. A bare base takes the fill as its own map, exactly the way
   * #layer_image_create made it; a Mix layer takes it through the map its top socket shows. */
  /* 3. Mutation: re-fill every wired channel's map, then record the colour on the marker. */
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[plan.layer_index];
    /* The colour is recorded in every channel, like the kind: a reader may look at any of them. */
    BKE_paint_material_layer_fill_color_set(*layer.node, color);
    if (layer.image == nullptr) {
      /* A channel the layer never got a map for behaves as unwired for this layer. */
      continue;
    }
    float map_color[4];
    fill_map_color_for(chain->channel, color, map_color);
    image_fill_flat(*layer.image, map_color);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_fill_color_preview(Main &bmain,
                                                 Material &ma,
                                                 const int ordinal,
                                                 const float color[4],
                                                 PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight, exactly like #BKE_paint_material_layer_fill_color_apply: the row exists, the
   * material is writable, and every channel's node for the row carries the Fill kind -- all
   * decided without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return fail(error);
  }
  for (const ChannelChain *chain : plan.chains) {
    const ChainLayer &layer = chain->layers[plan.layer_index];
    if (BKE_paint_material_layer_kind_get(*layer.node) != PaintMaterialLayerKind::Fill) {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
  }
  /* 2. Shape: nothing to convert, like the apply. */
  /* 3. Mutation: re-fill every wired channel's map, deliberately recording nothing on the
   * layer's marker -- the marker is what the layer *stands for*, and only the dialog's exec
   * (the bake) moves it. The revision bump and cache invalidation below are still needed so
   * the stack reader and the compositor see the new pixels. */
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[plan.layer_index];
    if (layer.image == nullptr) {
      /* A channel the layer never got a map for behaves as unwired for this layer. */
      continue;
    }
    float map_color[4];
    fill_map_color_for(chain->channel, color, map_color);
    image_fill_flat(*layer.image, map_color);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_kind_set(Main &bmain,
                                       Material &ma,
                                       const int ordinal,
                                       const PaintMaterialLayerKind kind,
                                       PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and the material is writable, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::KindSet, plan, error)) {
    return fail(error);
  }
  /* 2. Mutation: the kind marker is written on the layer's node in every channel at once, so the
   * row reads as one kind no matter which channel a reader looks at. */
  for (ChannelChain *chain : plan.chains) {
    BKE_paint_material_layer_kind_set(*chain->layers[plan.layer_index].node, kind);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_rename(Main &bmain,
                                     Material &ma,
                                     const int ordinal,
                                     const char *name,
                                     PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  if (name == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 1. Preflight: the row exists and a bare base is refused, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Rename, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * rename is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Rename, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. The name a user sees is the label of the
   * layer's Mix nodes, set in every channel at once. */
  for (ChannelChain *chain : plan.chains) {
    STRNCPY_UTF8(chain->layers[plan.layer_index].node->label, name);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_set_enabled(Main &bmain,
                                          Material &ma,
                                          const int ordinal,
                                          const bool enable,
                                          PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and a bare base is refused, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::SetEnabled, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * toggle is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::SetEnabled, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. Muting the layer's Mix nodes is what the UI
   * model reads back as "disabled", in every channel at once. */
  for (ChannelChain *chain : plan.chains) {
    bNode &node = *chain->layers[plan.layer_index].node;
    SET_FLAG_FROM_TEST(node.flag, !enable, NODE_MUTED);
    BKE_ntree_update_tag_node_mute(chain->tree, &node);
  }
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  /* The Shading component is NO_COW_TAG_ON_UPDATE: ID_RECALC_SHADING alone never re-copies the
   * evaluated material, so the mute flag GPU compilation reads would stay on whatever it was at
   * the last relation sync. #SYNC_TO_EVAL is what actually refreshes it, the same fix already
   * applied where a node's ID field changes (see #paint_layer_edit_committed). */
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_remove(Main &bmain,
                                     Material &ma,
                                     const int ordinal,
                                     PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and the stack keeps more than one layer, without writing a
   * byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Remove, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * removal is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Remove, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  Vector<ChannelChain *> &chains = plan.chains;
  const int layer_index = plan.layer_index;

  /* The tree each node is in is recorded now: relinking invalidates the topology cache that
   * #bNode::owner_tree reads, and a layer inside a group lives in the group's tree. */
  Vector<std::pair<bNodeTree *, bNode *>> nodes_to_remove;

  /** What one channel's removal needs, resolved before any link is rewritten. */
  struct ResolvedRemoval {
    ChannelChain *chain = nullptr;
    bNodeLink *top_link = nullptr;
    bool bare_base = false;
    bool map_is_sole_user = false;
  };
  Vector<ResolvedRemoval> resolved;
  for (ChannelChain *chain_ptr : chains) {
    ChannelChain &chain = *chain_ptr;
    const bool bare_base = (layer_index == 0 && !chain.layers.first().is_mix());
    /* Read while the topology cache still describes this tree: the rebuild below rewrites links,
     * and the next channel's turn would then ask a socket for links it no longer has. */
    ChainLayer &removed = chain.layers[bare_base ? 1 : layer_index];
    bNodeLink *top_link = (removed.top == nullptr) ? nullptr : sole_link_into(*removed.top);
    const bool map_is_sole_user = top_link != nullptr &&
                                  top_link->fromsock->directly_linked_links().size() == 1;
    resolved.append({chain_ptr, top_link, bare_base, map_is_sole_user});
  }

  for (const ResolvedRemoval &entry : resolved) {
    ChannelChain &chain = *entry.chain;
    if (entry.bare_base) {
      /* Removing a bare base: the layer above becomes the new bottom, and since it has nothing
       * left to blend over, the Image Texture it carried takes the base's place. A uniform chain
       * needs none of this -- its next layer down already blends over transparency. */
      if (entry.top_link == nullptr) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      ChainLayer new_base;
      new_base.node = entry.top_link->fromnode;
      new_base.output = entry.top_link->fromsock;
      new_base.image = chain.layers[1].image;
      nodes_to_remove.append({chain.tree, chain.layers[0].node});
      nodes_to_remove.append({chain.tree, chain.layers[1].node});
      chain.layers.remove(0);
      chain.layers[0] = new_base;
    }
    else {
      nodes_to_remove.append({chain.tree, chain.layers[layer_index].node});
      /* The map that only this layer read goes with it; one shared with another layer stays. */
      if (entry.map_is_sole_user) {
        nodes_to_remove.append({chain.tree, entry.top_link->fromnode});
      }
      chain.layers.remove(layer_index);
    }
    chain_rebuild_links(chain);
  }

  Set<bNodeTree *> touched_trees;
  for (const std::pair<bNodeTree *, bNode *> &entry : nodes_to_remove) {
    bke::node_remove_node(&bmain, *entry.first, *entry.second, true);
    BKE_ntree_update_tag_node_removed(entry.first);
    touched_trees.add(entry.first);
  }
  for (bNodeTree *touched : touched_trees) {
    if (touched != ma.nodetree) {
      BKE_ntree_update_after_single_tree_change(bmain, *touched);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

}  // namespace blender
