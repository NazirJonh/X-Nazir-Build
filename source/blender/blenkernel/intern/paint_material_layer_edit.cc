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

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLT_translation.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
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

#include <utility>

#include "paint_material_composite_internal.hh"

namespace blender {

namespace {

const char *LAYER_MARKER_PROP = "pbr_paint_layer";
/* The marker that says a node group is a folder of layers; see `08 §2.2`. Kept in step with
 * `paint_material_composite.cc`, which is what reads it back. */
const char *LAYER_GROUP_MARKER_PROP = "pbr_paint_node_group";
const char *LAYER_GROUP_MARKER_VALUE = "LAYER_GROUP";

/** Matches the reader in `paint_material_layer_model.cc`; see `08 §2.2`, Q2. */
constexpr int LAYER_GROUP_NESTING_MAX = 8;

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
                       reinterpret_cast<Image *>(from.id) :
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
  bNodeTree *group_tree = reinterpret_cast<bNodeTree *>(group.id);
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
    bNodeTree *group_tree = (group == nullptr) ? nullptr :
                                                 reinterpret_cast<bNodeTree *>(group->id);
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
    layer.sub_chain_index = int(r_chains.size());
    if (!chain_forest_collect(
            *group_tree, *result, channel, r_chains, nesting + 1, r_error))
    {
      return false;
    }
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
    return bke::node_find_socket(node, SOCK_OUT, "Result"_ustr);
  }
  if (bNodeSocket *socket = bke::node_find_socket(node, SOCK_OUT, "Result_Color"_ustr)) {
    return socket;
  }
  /* #SH_NODE_MIX_RGB_LEGACY. */
  return bke::node_find_socket(node, SOCK_OUT, "Color"_ustr);
}

/** The nodes one channel contributes to a layer being added. */
struct NewLayerNodes {
  int channel = -1;
  bNode *tex = nullptr;
  /** Null only for the bottom layer of a stack being created from nothing. */
  bNode *mix = nullptr;
  Image *image = nullptr;
};

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
    /* A scalar channel has one meaningful component, and the fill color's red carries it. */
    const float value = info.is_color ? 0.0f : params.fill_color[0];
    color[0] = info.is_color ? params.fill_color[0] : value;
    color[1] = info.is_color ? params.fill_color[1] : value;
    color[2] = info.is_color ? params.fill_color[2] : value;
    color[3] = 1.0f;
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      /* Flat tangent space, so a filled Normal layer starts out as "no change". */
      color[0] = 0.5f;
      color[1] = 0.5f;
      color[2] = 1.0f;
    }
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
                                Span<ChannelChain> chains,
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

  for (const ChannelChain &chain : chains) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(chain.channel));
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
    bke::node_add_link(
        group, *map_copy, *map_alpha, *mix_copy, *const_cast<bNodeSocket *>(mix.factor));
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
      bke::node_remove_node(&bmain, tree, *added.mix, false);
    }
    if (added.tex != nullptr) {
      /* Clear the reference first: the node is about to go, and the image right after it. */
      added.tex->id = nullptr;
      bke::node_remove_node(&bmain, tree, *added.tex, false);
    }
    if (added.image != nullptr) {
      BKE_id_free(&bmain, added.image);
    }
    added = NewLayerNodes{};
  }
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
      /* Assigned directly: user counts of node ID references are recomputed by the tree update. */
      node->id = &group->id;
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
  /* The layer covers what is below it exactly where its own map is opaque, like every other. */
  relink_into(tree, *const_cast<bNodeSocket *>(mix.factor), *mix_node, *image_node, *image_alpha);
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
    BKE_paint_material_layer_markers_ensure(ma);
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
    DEG_relations_tag_update(&bmain);
    BKE_paint_material_composite_cache_invalidate(&ma);
  }
  return changed;
}

bool BKE_paint_material_layer_markers_ensure(Material &ma)
{
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
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };
  auto succeed = [&](const int ordinal) {
    if (r_ordinal != nullptr) {
      *r_ordinal = ordinal;
    }
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
    DEG_relations_tag_update(&bmain);
    BKE_paint_material_composite_cache_invalidate(&ma);
    return true;
  };

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  bNodeTree &tree = *ma.nodetree;

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  Vector<ChannelChain> chains;
  const bool has_stack = chains_collect(ma, chains, error) && chains_align(chains, error);
  if (!has_stack && error != PaintMaterialLayerEditError::NotAStack) {
    /* A stack that cannot be rewritten is refused before anything is created. */
    return fail(error);
  }

  Vector<NewLayerNodes> added;

  if (!has_stack) {
    /* Nothing to blend over yet: the first layer is a bare Image Texture on Base Color. The other
     * channels get their maps when a brush first writes to them. */
    const bNodeSocket *terminal = paint_material_channel_socket_find(
        ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (terminal == nullptr) {
      return fail(PaintMaterialLayerEditError::NoPrincipled);
    }
    if (params.ordinal > 0) {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }

    /* Resolved before anything is created: adding a node invalidates the topology cache, and the
     * owner of a socket cannot be asked for once it is gone. */
    tree.ensure_topology_cache();
    bNode &terminal_node = const_cast<bNode &>(terminal->owner_node());

    NewLayerNodes base;
    base.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    base.image = layer_image_create(bmain, base.channel, params);
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
    bke::node_position_relative(
        *base.tex, terminal_node, nullptr, *const_cast<bNodeSocket *>(terminal));
    relink_into(
        tree, *const_cast<bNodeSocket *>(terminal), terminal_node, *base.tex, *color);
    return succeed(0);
  }

  if (params.ordinal >= 0 && !ordinal_is_in_chain(params.ordinal, error)) {
    return fail(error);
  }
  const int64_t layer_num = chains.first().layers.size();
  const int insert_at = (params.ordinal < 0) ? int(layer_num) : params.ordinal;
  if (insert_at == 0 && !chains.first().layers.first().is_mix()) {
    /* Going below a bare base would mean turning it into a Mix node, which is a conversion rather
     * than a relink. Below the lowest layer of a uniform chain is just another position. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }
  if (insert_at > layer_num) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* Create every node first. Group nodes get their sockets from a tree update, so nothing may be
   * linked before that update has run. */
  for (const ChannelChain &chain : chains) {
    NewLayerNodes nodes;
    nodes.channel = chain.channel;
    nodes.image = layer_image_create(bmain, chain.channel, params);
    if (nodes.image == nullptr) {
      added.append(nodes);
      new_layer_nodes_discard(bmain, tree, added);
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    nodes.tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    nodes.tex->id = &nodes.image->id;
    if (chain.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      /* Tangent-space maps do not blend component-wise; the engine's own group does it properly. */
      bNodeTree *group = BKE_paint_material_normal_combine_group_ensure(bmain);
      nodes.mix = (group == nullptr) ?
                      nullptr :
                      bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (nodes.mix != nullptr) {
        /* Assigned directly, as the texture nodes are: user counts of node ID references are
         * recomputed by the tree update below. */
        nodes.mix->id = &group->id;
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

  /* Sockets of a group instance only exist once the tree has been updated. The new nodes are not
   * linked to anything yet, so the chains this invalidates are re-read below. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  chains.clear();
  if (!chains_collect(ma, chains, error) || !chains_align(chains, error)) {
    new_layer_nodes_discard(bmain, tree, added);
    return fail(error);
  }

  /* Resolve every socket before touching a link, so a node type that turned out not to match the
   * Mix contract cannot leave half a layer behind. */
  struct ResolvedLayer {
    ChannelChain *chain = nullptr;
    ChainLayer layer;
    bNode *tex = nullptr;
    bNodeSocket *tex_color = nullptr;
    bNodeSocket *tex_alpha = nullptr;
    bNodeSocket *factor = nullptr;
  };
  Vector<ResolvedLayer> resolved;
  for (NewLayerNodes &nodes : added) {
    ChannelChain *chain = nullptr;
    for (ChannelChain &candidate : chains) {
      if (candidate.channel == nodes.channel) {
        chain = &candidate;
        break;
      }
    }
    CompositeMixNode mix;
    bNodeSocket *output = mix_output_find(*nodes.mix);
    bNodeSocket *tex_color = bke::node_find_socket(*nodes.tex, SOCK_OUT, "Color"_ustr);
    bNodeSocket *tex_alpha = bke::node_find_socket(*nodes.tex, SOCK_OUT, "Alpha"_ustr);
    if (chain == nullptr || output == nullptr || tex_color == nullptr || tex_alpha == nullptr ||
        !composite_mix_node_read(*nodes.mix, mix))
    {
      new_layer_nodes_discard(bmain, tree, added);
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    ResolvedLayer entry;
    entry.chain = chain;
    entry.tex = nodes.tex;
    entry.factor = const_cast<bNodeSocket *>(mix.factor);
    entry.layer.node = nodes.mix;
    entry.layer.bottom = const_cast<bNodeSocket *>(mix.bottom);
    entry.layer.top = const_cast<bNodeSocket *>(mix.top);
    entry.layer.output = output;
    entry.layer.image = nodes.image;
    entry.tex_color = tex_color;
    entry.tex_alpha = tex_alpha;
    resolved.append(entry);
  }

  /* One identity for the whole layer, so a later reorder can move it in every channel at once. */
  BKE_paint_material_layer_markers_ensure(ma);
  const bUUID layer_id = BLI_uuid_generate_random();

  for (ResolvedLayer &entry : resolved) {
    bke::node_add_link(tree, *entry.tex, *entry.tex_color, *entry.layer.node, *entry.layer.top);
    /* The stack drives a layer's factor from its own Alpha: an unpainted texel must not cover
     * what is below it. */
    bke::node_add_link(tree, *entry.tex, *entry.tex_alpha, *entry.layer.node, *entry.factor);
    bke::node_position_relative(*entry.layer.node,
                                *entry.chain->terminal_node,
                                entry.layer.output,
                                *entry.chain->terminal);
    bke::node_position_relative(*entry.tex, *entry.layer.node, entry.tex_color, *entry.layer.top);
    entry.chain->layers.insert(insert_at, entry.layer);
    chain_rebuild_links(*entry.chain);
    BKE_paint_material_layer_marker_set(*entry.layer.node, layer_id);
    if (entry.layer.image != nullptr) {
      entry.layer.image->paint_layer_id = layer_id;
    }
  }

  return succeed(insert_at);
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
    const int depth = 1 + layer_group_depth(*reinterpret_cast<bNodeTree *>(node.id), guard + 1);
    if (depth > deepest) {
      deepest = depth;
    }
  }
  return deepest;
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

      if (layer.factor != nullptr) {
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
      bNodeTree *group_tree = reinterpret_cast<bNodeTree *>(moving_nodes.first().map_source->id);
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
          bke::node_add_link(*dst_tree, *c.mask_copy, *mask_color, *c.mix_copy, *c.mix_factor);
        }
      }
      else {
        bke::node_add_link(*dst_tree, *c.map_copy, *map_alpha, *c.mix_copy, *c.mix_factor);
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
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
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
  bNodeTree *group_tree = reinterpret_cast<bNodeTree *>(const_cast<ID *>(instance->id));
  if (group_tree == src_tree) {
    return fail(PaintMaterialLayerEditError::ChainNotPlain);
  }

  /* Everything the graph is asked about is read before the first node is created: creating one
   * invalidates the topology cache the socket walks assert on. */
  struct MovingChannelNodes {
    bNode *mix_source = nullptr;
    bNode *map_source = nullptr;
    bNode *mask_source = nullptr;
    Image *image = nullptr;
    bool is_muted = false;
    int channel = 0;
  };
  Vector<MovingChannelNodes> moving_nodes;
  for (ChannelChain *chain : from_chains) {
    ChainLayer &layer = chain->layers[from_index];
    if (layer.is_group) {
      /* A folder inside an empty folder would have to build a chain out of a group instance, which
       * is a different shape than the one this builds. */
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
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
    if (layer.factor != nullptr) {
      bNodeLink *factor_link = sole_link_into(*layer.factor);
      if (factor_link != nullptr && factor_link->fromnode != m.map_source) {
        m.mask_source = factor_link->fromnode;
      }
    }
    moving_nodes.append(m);
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
    CompositeMixNode mix;
    bNodeSocket *mix_out = mix_output_find(*c.mix_copy);
    bNodeSocket *map_color = bke::node_find_socket(*c.map_copy, SOCK_OUT, "Color"_ustr);
    bNodeSocket *map_alpha = bke::node_find_socket(*c.map_copy, SOCK_OUT, "Alpha"_ustr);
    if (mix_out == nullptr || map_color == nullptr || map_alpha == nullptr ||
        !composite_mix_node_read(*c.mix_copy, mix))
    {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(moving_nodes[i].channel));
    char result_name[64];
    SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
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
    bke::node_add_link(*group_tree,
                       (c.mask_copy != nullptr) ? *c.mask_copy : *c.map_copy,
                       *coverage,
                       *c.mix_copy,
                       *const_cast<bNodeSocket *>(mix.factor));
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
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

/**
 * The forest of \a ma with the row \a from_ordinal resolved in it, ready for a move.
 *
 * Both entry points below start the same way, and the markers have to be written before the chains
 * are collected: they are id-properties, so they leave the collected chains describing the same
 * nodes.
 */
static bool layer_move_prepare(Main &bmain,
                               Material &ma,
                               const int from_ordinal,
                               Vector<Vector<ChannelChain>> &r_per_channel,
                               Vector<ChannelChain *> &r_from_chains,
                               int &r_from_index,
                               PaintMaterialLayerEditError &r_error)
{
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    r_error = PaintMaterialLayerEditError::NotEditable;
    return false;
  }
  /* A move needs every chain to have the same shape, so that "below the lowest layer" is a place
   * at all; a chain still ending in a bare image is brought to the current contract first. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  BKE_paint_material_layer_markers_ensure(ma);
  return chains_collect_forest(ma, r_per_channel, r_error) &&
         forest_rows_resolve(r_per_channel, from_ordinal, r_from_chains, r_from_index, r_error);
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

  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> from_chains;
  Vector<ChannelChain *> to_chains;
  int from_index = -1;
  int to_index = -1;
  if (!layer_move_prepare(bmain, ma, from_ordinal, per_channel, from_chains, from_index, error) ||
      !forest_rows_resolve(per_channel, to_ordinal, to_chains, to_index, error))
  {
    return fail(error);
  }
  return layer_move_apply(bmain, ma, from_chains, from_index, to_chains, to_index, r_error);
}

bool BKE_paint_material_layer_move(Main &bmain,
                                   Material &ma,
                                   const int from_ordinal,
                                   const int anchor_ordinal,
                                   const PaintMaterialLayerMovePlace place,
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

  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> from_chains;
  Vector<ChannelChain *> anchor_chains;
  int from_index = -1;
  int anchor_index = -1;
  if (!layer_move_prepare(bmain, ma, from_ordinal, per_channel, from_chains, from_index, error) ||
      !forest_rows_resolve(per_channel, anchor_ordinal, anchor_chains, anchor_index, error))
  {
    return fail(error);
  }

  if (place == PaintMaterialLayerMovePlace::Into) {
    /* The anchor is the folder itself, so the destination is the chain it opens -- and a folder
     * with nothing in it opens none yet, which is what #layer_move_into_empty_group builds. */
    const ChainLayer &group_layer = anchor_chains.first()->layers[anchor_index];
    if (!group_layer.is_group) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    if (group_layer.sub_chain_index < 0) {
      return layer_move_into_empty_group(
          bmain, ma, from_chains, from_index, anchor_chains, anchor_index, r_error);
    }
    Vector<ChannelChain *> to_chains;
    for (const int64_t i : anchor_chains.index_range()) {
      const int sub_index = anchor_chains[i]->layers[anchor_index].sub_chain_index;
      if (sub_index < 0 || sub_index >= per_channel[i].size()) {
        return fail(PaintMaterialLayerEditError::ChannelsDisagree);
      }
      to_chains.append(&per_channel[i][sub_index]);
    }
    /* On top of what the group holds. */
    const int to_index = int(to_chains.first()->layers.size());
    return layer_move_apply(bmain, ma, from_chains, from_index, to_chains, to_index, r_error);
  }

  int to_index = anchor_index + ((place == PaintMaterialLayerMovePlace::Above) ? 1 : 0);
  /* The layer leaves its own chain before it is put back, so every position above it moves down by
   * one; a move into another chain takes nothing out of the destination. */
  if (from_chains.first() == anchor_chains.first() && from_index < to_index) {
    to_index--;
  }
  if (to_index == from_index && from_chains.first() == anchor_chains.first()) {
    return true;
  }
  return layer_move_apply(bmain, ma, from_chains, from_index, anchor_chains, to_index, r_error);
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  bNodeTree &tree = *ma.nodetree;

  Vector<ChannelChain> chains;
  if (!chains_collect(ma, chains, error) || !chains_align(chains, error)) {
    return fail(error);
  }
  if (!ordinal_is_in_chain(from_ordinal, error) || !ordinal_is_in_chain(to_ordinal, error)) {
    return fail(error);
  }
  const int64_t layer_num = chains.first().layers.size();
  if (from_ordinal < 0 || to_ordinal >= layer_num || to_ordinal < from_ordinal) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  if (from_ordinal == 0 && !chains.first().layers.first().is_mix()) {
    /* A bare base is the channel's own map: it has no Mix node to become the group's, and without
     * one the group would have nothing to blend itself into the stack with. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }

  /* Every map the group will hold, resolved before the first node is created: creating one
   * invalidates the topology cache, and there is no reading links after that. */
  Vector<Vector<bNode *>> map_nodes_per_chain;
  for (ChannelChain &chain : chains) {
    Vector<bNode *> maps = chain_range_map_nodes(chain, from_ordinal, to_ordinal);
    if (maps.is_empty()) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    map_nodes_per_chain.append(std::move(maps));
  }

  bNodeTree *group_tree = layer_group_tree_add(bmain, chains, from_ordinal, to_ordinal);
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

  for (const int64_t chain_index : chains.index_range()) {
    ChannelChain &chain = chains[chain_index];
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
                                  from_ordinal,
                                  to_ordinal,
                                  chain_index == 0,
                                  nodes_to_remove))
    {
      discard_group();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
  }

  /* Sockets of the instances only exist once the tree has been updated. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  for (const int64_t chain_index : chains.index_range()) {
    ChannelChain &chain = chains[chain_index];
    bNode &instance = *instances[chain_index];
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(chain.channel));
    char result_name[64];
    SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);

    bNodeSocket *result = socket_find_by_name(instance, SOCK_OUT, result_name);
    bNodeSocket *alpha = socket_find_by_name(instance, SOCK_OUT, "Alpha");
    ChainLayer &keeper = chain.layers[from_ordinal];
    if (result == nullptr || alpha == nullptr || keeper.top == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    /* The kept Mix node stops blending a map and starts blending the group. */
    relink_into(tree, *keeper.top, *keeper.node, instance, *result);
    relink_into(tree, *keeper.factor, *keeper.node, instance, *alpha);
    bke::node_position_relative(instance, *keeper.node, result, *keeper.top);

    /* Everything above the kept layer, up to the top of the range, is inside the group now. */
    chain.layers.remove(from_ordinal + 1, to_ordinal - from_ordinal);
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
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
  if (r_ordinal != nullptr) {
    *r_ordinal = from_ordinal;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_group_add(Main &bmain,
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  bNodeTree &tree = *ma.nodetree;

  Vector<ChannelChain> chains;
  if (!chains_collect(ma, chains, error) || !chains_align(chains, error)) {
    return fail(error);
  }
  if (ordinal >= 0 && !ordinal_is_in_chain(ordinal, error)) {
    return fail(error);
  }
  const int layer_num = int(chains.first().layers.size());
  /* Above the row it was given, which is the position after it in a chain listed bottom to top. */
  const int insert_at = (ordinal < 0) ? layer_num : ordinal + 1;
  if (insert_at < 1 || insert_at > layer_num) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* The group holds nothing yet, so its Group Output stays unlinked: an unlinked Result is
   * transparent and its Alpha is zero, which is exactly what an empty folder contributes. */
  bNodeTree *group_tree = layer_group_tree_add(bmain, chains, insert_at, insert_at);
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

  for (const ChannelChain &chain : chains) {
    NewGroupNodes nodes;
    nodes.channel = chain.channel;
    nodes.instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
    if (nodes.instance != nullptr) {
      /* Assigned directly, as #BKE_paint_material_layer_group_make does: user counts of node ID
       * references are recomputed by the tree update below. */
      nodes.instance->id = &group_tree->id;
      id_us_plus(&group_tree->id);
    }
    nodes.mix = layer_mix_node_create(bmain, tree, chain.channel);
    added.append(nodes);
    if (nodes.instance == nullptr || nodes.mix == nullptr) {
      discard();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
  }

  /* Sockets of the instances only exist once the tree has been updated, and the chains collected
   * above describe the tree from before those nodes existed. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();
  chains.clear();
  if (!chains_collect(ma, chains, error) || !chains_align(chains, error)) {
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
    for (ChannelChain &candidate : chains) {
      if (candidate.channel == nodes.channel) {
        chain = &candidate;
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
    bke::node_add_link(tree, *entry.instance, *entry.alpha, *entry.layer.node, *entry.factor);
    bke::node_position_relative(
        *entry.instance, *entry.layer.node, entry.result, *entry.layer.top);
    entry.chain->layers.insert(insert_at, entry.layer);
    chain_rebuild_links(*entry.chain);
    BKE_paint_material_layer_marker_set(*entry.layer.node, layer_id);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
  if (r_ordinal != nullptr) {
    *r_ordinal = insert_at;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  bNodeTree &tree = *ma.nodetree;

  Vector<ChannelChain> chains;
  if (!chains_collect(ma, chains, error) || !chains_align(chains, error)) {
    return fail(error);
  }
  if (!ordinal_is_in_chain(ordinal, error)) {
    return fail(error);
  }
  const int64_t layer_num = chains.first().layers.size();
  if (ordinal < 1 || ordinal >= layer_num) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

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
  for (ChannelChain &chain : chains) {
    ChainLayer &keeper = chain.layers[ordinal];
    bNodeLink *top_link = (keeper.top == nullptr) ? nullptr : sole_link_into(*keeper.top);
    if (top_link == nullptr || !BKE_paint_material_is_layer_group(*top_link->fromnode)) {
      return fail(PaintMaterialLayerEditError::NotAStack);
    }
    instances.append(top_link->fromnode);
  }

  for (const int64_t chain_index : chains.index_range()) {
    ChannelChain &chain = chains[chain_index];
    bNode &instance = *instances[chain_index];
    bNodeTree *group_tree = reinterpret_cast<bNodeTree *>(instance.id);
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
    ChainLayer &keeper = chain.layers[ordinal];
    ChainLayer &bottom = out.layers.first();

    /* The group's Mix node goes back to blending a map, which is what it did before grouping. */
    relink_into(tree, *keeper.top, *keeper.node, *bottom.node, *bottom.output);
    relink_into(tree, *keeper.factor, *keeper.node, *bottom.node, *bottom.factor);
    keeper.image = bottom.image;
    bke::node_position_relative(*bottom.node, *keeper.node, bottom.output, *keeper.top);

    /* Every layer above the bottom one is spliced into the outer chain, in order. */
    int insert_at = ordinal + 1;
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
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  bNodeTree &tree = *ma.nodetree;

  Vector<ChannelChain> chains;
  if (!chains_collect(ma, chains, error) || !chains_align(chains, error)) {
    return fail(error);
  }
  if (!ordinal_is_in_chain(ordinal, error)) {
    return fail(error);
  }
  const int64_t layer_num = chains.first().layers.size();
  if (ordinal < 0 || ordinal >= layer_num) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  if (ordinal == 0 && !chains.first().layers.first().is_mix()) {
    /* A bare base has no Mix node; copying it would have to make one, which is the conversion
     * #IsBottomLayer stands for everywhere else here. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
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

  for (ChannelChain &chain : chains) {
    ChainLayer &source = chain.layers[ordinal];
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
      copy.image = reinterpret_cast<Image *>(BKE_id_copy(&bmain, &source.image->id));
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
  const int insert_at = ordinal + 1;
  for (DuplicatedLayer &copy : copies) {
    ChannelChain *chain = nullptr;
    for (ChannelChain &candidate : fresh_chains) {
      if (candidate.channel == copy.chain->channel) {
        chain = &candidate;
        break;
      }
    }
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
    bke::node_add_link(
        tree, *copy.tex, *tex_alpha, *copy.mix, *const_cast<bNodeSocket *>(mix.factor));
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
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> chains;
  int layer_index = -1;
  if (!chains_collect_forest(ma, per_channel, error) ||
      !forest_rows_resolve(per_channel, ordinal, chains, layer_index, error))
  {
    return fail(error);
  }
  if (layer_index == 0 && !chains.first()->layers.first().is_mix()) {
    /* A bare base has no Mix node to carry this. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }
  /* The mask node goes in the tree the layer lives in, which is the group's for a nested row. */
  bNodeTree &tree = *chains.first()->tree;

  /* One mask for the layer, shared by every channel: a layer is one thing, and a mask that
   * differed per channel would be several. */
  char mask_name[MAX_ID_NAME - 2];
  const char *layer_label = chains.first()->layers[layer_index].node->label;
  SNPRINTF_UTF8(mask_name, "%s Mask", (layer_label[0] != 0) ? layer_label : "Layer");
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *mask = BKE_image_add_generated(&bmain,
                                        image_size,
                                        image_size,
                                        mask_name,
                                        32,
                                        false,
                                        IMA_GENTYPE_BLANK,
                                        white,
                                        false,
                                        true,
                                        false);
  if (mask == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  mask->flag |= IMA_PAINT_CANVAS;
  mask->paint_layer_id = BKE_paint_material_layer_marker_get(
      *chains.first()->layers[layer_index].node);
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

  for (ChannelChain *chain : chains) {
    ChainLayer &layer = chain->layers[layer_index];
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.factor == nullptr) {
      continue;
    }
    relink_into(*chain->tree, *const_cast<bNodeSocket *>(mix.factor), *layer.node, *tex, *color);
  }
  ChainLayer &masked = chains.first()->layers[layer_index];
  bke::node_position_relative(*tex, *masked.node, color, *masked.top);

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> chains;
  int layer_index = -1;
  if (!chains_collect_forest(ma, per_channel, error) ||
      !forest_rows_resolve(per_channel, ordinal, chains, layer_index, error))
  {
    return fail(error);
  }
  if (layer_index == 0 && !chains.first()->layers.first().is_mix()) {
    /* A bare base has no Mix node to carry this. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }
  bNodeTree &tree = *chains.first()->tree;

  Vector<std::pair<bNodeTree *, bNode *>> mask_nodes;
  for (ChannelChain *chain_ptr : chains) {
    ChannelChain &chain = *chain_ptr;
    ChainLayer &layer = chain.layers[layer_index];
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.factor == nullptr) {
      continue;
    }
    bNodeLink *mask_link = sole_link_into(*const_cast<bNodeSocket *>(mix.factor));
    if (mask_link == nullptr) {
      continue;
    }
    if (mask_link->fromsock->directly_linked_links().size() == 1) {
      mask_nodes.append_non_duplicates({chain.tree, mask_link->fromnode});
    }
    /* Coverage goes back to the layer's own map, which is where it comes from for a layer that
     * never had a mask. */
    bNodeSocket *factor = const_cast<bNodeSocket *>(mix.factor);
    bNodeLink *map_link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
    bNodeSocket *alpha = (map_link == nullptr) ?
                             nullptr :
                             bke::node_find_socket(*map_link->fromnode, SOCK_OUT, "Alpha"_ustr);
    if (alpha == nullptr) {
      /* Nothing to restore the factor from; leaving it unlinked is still a layer without a mask. */
      for (bNodeLink *link : Vector<bNodeLink *>(factor->directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(chain.tree);
        bke::node_remove_link(chain.tree, *link);
      }
      continue;
    }
    /* The map's node is taken from the link rather than from the socket: relinking the previous
     * channel already invalidated the topology cache #owner_node asserts on. */
    relink_into(*chain.tree, *factor, *layer.node, *map_link->fromnode, *alpha);
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
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);
  if (name == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> chains;
  int layer_index = -1;
  if (!chains_collect_forest(ma, per_channel, error) ||
      !forest_rows_resolve(per_channel, ordinal, chains, layer_index, error))
  {
    return fail(error);
  }
  if (layer_index == 0 && !chains.first()->layers.first().is_mix()) {
    /* A bare base has no Mix node to carry this. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }

  for (ChannelChain *chain : chains) {
    STRNCPY_UTF8(chain->layers[layer_index].node->label, name);
  }

  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  BKE_paint_material_composite_cache_invalidate(&ma);
  UNUSED_VARS(bmain);
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);

  /* Resolved through the forest rather than one flat chain, so a layer inside a group can be
   * switched off where it lives. */
  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> chains;
  int layer_index = -1;
  if (!chains_collect_forest(ma, per_channel, error) ||
      !forest_rows_resolve(per_channel, ordinal, chains, layer_index, error))
  {
    return fail(error);
  }
  if (layer_index == 0 && !chains.first()->layers.first().is_mix()) {
    /* A bare base is the channel's own map: muting it would unwire the channel rather than hide a
     * layer. The lowest layer of a uniform chain has a Mix node and mutes like any other. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }

  for (ChannelChain *chain : chains) {
    bNode &node = *chain->layers[layer_index].node;
    SET_FLAG_FROM_TEST(node.flag, !enable, NODE_MUTED);
    BKE_ntree_update_tag_node_mute(chain->tree, &node);
  }
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  BKE_paint_material_composite_cache_invalidate(&ma);
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

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Every mutation works on a chain of the current shape: a bottom that is still a bare image is
   * wrapped in a Mix node first, so the lowest row is a layer like any other. */
  BKE_paint_material_layer_bottom_normalize(bmain, ma);

  Vector<Vector<ChannelChain>> per_channel;
  Vector<ChannelChain *> chains;
  int layer_index = -1;
  if (!chains_collect_forest(ma, per_channel, error) ||
      !forest_rows_resolve(per_channel, ordinal, chains, layer_index, error))
  {
    return fail(error);
  }
  const int64_t layer_num = chains.first()->layers.size();
  if (layer_num == 1) {
    /* Removing the only layer would leave the channel unwired, which is a different operation
     * (unassigning the material's texture) than removing a layer from a stack. */
    return fail(PaintMaterialLayerEditError::IsBottomLayer);
  }

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
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  DEG_relations_tag_update(&bmain);
  BKE_paint_material_composite_cache_invalidate(&ma);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

}  // namespace blender
