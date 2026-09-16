/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Reads a material's paint layer graph into the shapes the mutators in
 * `paint_material_layer_edit.cc`, `paint_material_layer_props.cc` and
 * `paint_material_layer_channels.cc` all build on: a channel's chain of Mix nodes
 * (#ChannelChain), a channel's forest of chains when groups are involved, and the
 * #LayerEditPlan every one of those files' operations is built from. See
 * `paint_material_layer_edit_intern.hh` for the shared vocabulary this file implements.
 *
 * Every operation elsewhere works the same way: collect each channel's chain, check the
 * preconditions across all of them here, then rebuild the "what is below me" links from an
 * array. Rebuilding rather than patching is deliberate -- an insert expressed as four
 * unlink/link pairs has four ways to leave the graph half-moved, and the array cannot.
 */

#include "paint_material_layer_edit_intern.hh"
#include "paint_material_layer_mask_bake_intern.hh"

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
#include "BKE_paint_material_mask_bake.hh"
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
#include "BLI_uuid.h"
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

#include "paint_material_composite_internal.hh"
#include "paint_material_layer_idprops.hh"

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
  const MaterialPaintLayerRuntime *runtime =
      static_cast<const MaterialPaintLayerRuntime *>(ma.paint_layer_runtime);
  return (runtime != nullptr) ? runtime->edit_revision : 0;
}

void paint_layer_socket_default_color(const bNodeSocket &socket, float r_color[4])
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

/** Matches the readers in `paint_material_layer_edit.cc` and `paint_material_layer_model.cc`;
 * see `08 §2.2`, Q2. */
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
  /* The baked masks the same edit moved the graph under; an anchor is rebuilt from the same stack
   * the composite reads, so the two are invalidated together. */
  BKE_paint_material_mask_bake_cache_invalidate(&ma);
  /* Bake now, on the main thread this commit runs on: B feeds the shader, and a draw between this
   * edit and the next ensure would sample a freshly anchored row's still-empty texture. */
  BKE_paint_material_mask_bake_ensure(bmain, ma, false);
}

/** The tree the group instance \a node opens, or null when it holds none or not a node tree. */
bNodeTree *layer_group_tree_of(const bNode &node)
{
  if (node.id == nullptr || GS(node.id->name) != ID_NT) {
    return nullptr;
  }
  return id_cast<bNodeTree *>(node.id);
}

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
 * One step of the correction walk inside #correction_chain_read: the node feeding \a socket must
 * be a correction of \a section whose links are the shape #correction_channel_insert builds, and
 * its base socket -- what it blends over -- is returned in \a r_next.
 */
static bool correction_walk_step(const bNodeSocket &at,
                                 const PaintMaterialCorrectionSection section,
                                 ChainCorrection &r_correction,
                                 const bNodeSocket *&r_next,
                                 PaintMaterialLayerEditError &r_error)
{
  bNodeLink *link = sole_link_into(const_cast<bNodeSocket &>(at));
  if (link == nullptr) {
    /* Linked, but not in a shape this file can rewrite: several links, or a muted one. */
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  bNode &from = *link->fromnode;
  CompositeMixNode corr_mix;
  if (!composite_mix_node_read(from, corr_mix) || corr_mix.bottom == nullptr) {
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  if (!correction_nodes_read(from, corr_mix, r_correction) || r_correction.section != section) {
    /* A correction of the other section, or one whose links are not the shape this module
     * builds: neither belongs on this path of the layer. */
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  if (link->fromsock->directly_linked_links().size() != 1) {
    /* A result consumed by anything but the layer (or correction) above cannot be rewritten
     * without changing what that other consumer sees. */
    r_error = PaintMaterialLayerEditError::ChainIsShared;
    return false;
  }
  if (socket_has_link(const_cast<bNodeSocket &>(*corr_mix.top)) && r_correction.map == nullptr) {
    /* The correction's map input is fed, and not by the one Image Texture a map is. */
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  r_next = corr_mix.bottom;
  return true;
}

/**
 * Reverse \a top_down into \a r_list: the walk meets the topmost correction first, and the lists
 * read bottom to top, like #ChannelChain.layers does.
 */
static void correction_list_reverse(const Vector<ChainCorrection> &top_down,
                                    Vector<ChainCorrection> &r_list)
{
  for (const int64_t i : top_down.index_range()) {
    r_list.append(top_down[top_down.size() - 1 - i]);
  }
}

bool correction_chain_read(ChainLayer &layer, PaintMaterialLayerEditError &r_error)
{
  layer.content_corrections.clear();
  layer.mask_corrections.clear();
  layer.base_map = nullptr;
  layer.image = nullptr;
  if (layer.node == nullptr) {
    return true;
  }
  CompositeMixNode layer_mix;
  if (!composite_mix_node_read(*layer.node, layer_mix) || layer_mix.top == nullptr) {
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }

  /* The content stack: the corrections hanging on the layer's map input, down to the layer's own
   * map -- or to nothing, which is the layer being Absent in this channel.
   *
   * The walk follows links, not #composite_source_node_shallow: a muted node must not stop the
   * reading of the shape, because a muted map is exactly the Disabled state the channel state
   * has to report under the corrections. Reroutes are refused, the way the layer walk refuses
   * them: this file is about to rewrite these links. */
  Vector<ChainCorrection> top_down;
  bool base_resolved = false;
  const bNodeSocket *socket = layer_mix.top;
  for (int step = 0; step < 64 && socket != nullptr; step++) {
    if (!socket_has_link(const_cast<bNodeSocket &>(*socket))) {
      base_resolved = true;
      break;
    }
    bNodeLink *link = sole_link_into(const_cast<bNodeSocket &>(*socket));
    if (link == nullptr) {
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }
    bNode &from = *link->fromnode;
    if (from.type_legacy == SH_NODE_TEX_IMAGE) {
      layer.base_map = &from;
      layer.image = (from.id != nullptr && GS(from.id->name) == ID_IM) ?
                        id_cast<Image *>(from.id) :
                        nullptr;
      base_resolved = true;
      break;
    }
    if (BKE_paint_material_is_layer_group(from)) {
      /* A group row's map is the folder's Result: the rows it holds live inside the folder, and
       * the row itself carries no content corrections (D7). The base is the instance itself. */
      base_resolved = true;
      break;
    }
    if (!bke::paint_layer::node_is_correction(from)) {
      /* Anything else on the map input -- a reroute, a hand-wired node -- is read the way it was
       * before corrections existed: no correction to collect, and the image it resolves to, if
       * any. Refusing it here would lock every edit of the whole material. */
      const ImageUser *iuser = nullptr;
      composite_image_from_socket(*socket, layer.image, iuser);
      base_resolved = true;
      break;
    }
    ChainCorrection correction;
    const bNodeSocket *next = nullptr;
    if (!correction_walk_step(*socket,
                              PaintMaterialCorrectionSection::Content,
                              correction,
                              next,
                              r_error))
    {
      return false;
    }
    top_down.append(correction);
    socket = next;
  }
  if (!base_resolved) {
    /* Ran out of depth without reaching the map. */
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  correction_list_reverse(top_down, layer.content_corrections);

  /* The mask chain: the corrections limiting what covers the layer, down to the coverage source
   * the layer would have without them -- its own map's alpha, a mask image, or the over output
   * of a content correction. None of these is a base map of its own, so the walk stops there and
   * #base_map stays on the content stack's answer. */
  top_down.clear();
  bool coverage_resolved = (layer_mix.factor_coverage == nullptr);
  /* A baked coverage reads B, but the corrections that shape it stay on the anchor's live input:
   * the model has to list those corrections, so the walk starts where they are. With no bake the
   * helper returns the coverage socket itself, so the reading below is the same as it always was.
   */
  socket = (layer_mix.factor_coverage != nullptr) ?
               mask_bake_live_top_socket(*layer_mix.factor_coverage) :
               nullptr;
  for (int step = 0; step < 64 && socket != nullptr; step++) {
    if (!socket_has_link(const_cast<bNodeSocket &>(*socket))) {
      coverage_resolved = true;
      break;
    }
    bNodeLink *link = sole_link_into(const_cast<bNodeSocket &>(*socket));
    if (link == nullptr) {
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }
    bNode &from = *link->fromnode;
    if (from.type_legacy == SH_NODE_TEX_IMAGE ||
        (from.type_legacy == SH_NODE_MATH &&
         NodeMathOperation(from.custom1) == NODE_MATH_MULTIPLY_ADD))
    {
      coverage_resolved = true;
      break;
    }
    if (BKE_paint_material_is_layer_group(from) || !bke::paint_layer::node_is_correction(from)) {
      /* The folder's own Alpha, or any other coverage source a row had before corrections
       * existed: where the mask chain ends, read as it always was. */
      coverage_resolved = true;
      break;
    }
    ChainCorrection correction;
    const bNodeSocket *next = nullptr;
    if (!correction_walk_step(*socket,
                              PaintMaterialCorrectionSection::Mask,
                              correction,
                              next,
                              r_error))
    {
      return false;
    }
    top_down.append(correction);
    socket = next;
  }
  if (!coverage_resolved) {
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  correction_list_reverse(top_down, layer.mask_corrections);
  bool coverage_baked = false;
  if (layer_mix.factor_coverage != nullptr) {
    Image *baked_image = nullptr;
    bNode *baked_node = nullptr;
    coverage_baked = mask_bake_coverage_is_baked(
        *layer_mix.factor_coverage, baked_image, baked_node);
  }
  MASK_BAKE_TRACE("chain_read node=%p content=%zu mask=%zu coverage_baked=%d\n",
                  static_cast<void *>(layer.node),
                  size_t(layer.content_corrections.size()),
                  size_t(layer.mask_corrections.size()),
                  int(coverage_baked));
  return true;
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
    /* A correction hangs on a layer's own inputs -- its map, or its coverage -- never between
     * two layers. Corrections are Mix nodes, so this has to be tested before the layer read
     * below mistakes one for a layer. */
    if (bke::paint_layer::node_is_correction(from)) {
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
      /* The corrections hanging on the row's own inputs are read with it, group rows included:
       * a mask section limits the folder's result and hangs on the row's own coverage Multiply
       * (spec 18 §4.3), and an edit moving the row has to move them too. #correction_chain_read
       * stops at the instance for a group, so the folder's insides are not walked here. */
      if (!correction_chain_read(layer, r_error)) {
        return false;
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
bool chains_collect(Material &ma,
                      Vector<ChannelChain> &r_chains,
                      PaintMaterialLayerEditError &r_error)
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

/**
 * Put the hidden base under \a chain: the unlinked bottom input of its lowest Mix takes the value
 * the chain's terminal would have on its own -- the Principled input's default, or a flat normal
 * through the Normal Map. A layer with no map in this channel, or a switched-off one, then shows
 * exactly what the material showed before the stack existed.
 *
 * Copied on every structural edit rather than synced live: once the chain feeds the terminal, its
 * default value is no longer editable from the UI (see the spec's base contract).
 *
 * Only a top-level chain has a base; inside a folder the bottom stays transparent, so a folder
 * contributes nothing where its layers contribute nothing.
 */
void chain_base_apply(ChannelChain &chain)
{
  if (chain.layers.is_empty() || chain.terminal == nullptr || chain.terminal_node == nullptr ||
      chain.terminal_node->type_legacy == NODE_GROUP_OUTPUT)
  {
    return;
  }
  ChainLayer &bottom = chain.layers.first();
  if (!bottom.is_mix()) {
    return;
  }
  float base[4];
  paint_layer_socket_default_color(*chain.terminal, base);
  bNodeSocket &input = *bottom.bottom;
  switch (input.type) {
    case SOCK_RGBA:
      copy_v4_v4(input.default_value_typed<bNodeSocketValueRGBA>()->value, base);
      break;
    case SOCK_VECTOR:
      copy_v3_v3(input.default_value_typed<bNodeSocketValueVector>()->value, base);
      break;
    case SOCK_FLOAT:
      input.default_value_typed<bNodeSocketValueFloat>()->value = base[0];
      break;
    default:
      return;
  }
  BKE_ntree_update_tag_socket_property(chain.tree, &input);
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
  chain_base_apply(chain);
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

/** Spec 18 §4.5 p.3: same UUID sequence per section, same section and effect, in every chain. */
bool layer_corrections_agree(Span<ChannelChain *> chains,
                             const int layer_index,
                             PaintMaterialLayerEditError &r_error)
{
  if (chains.is_empty() || layer_index < 0 || layer_index >= chains.first()->layers.size()) {
    /* No resolved row to compare -- an add at the top of a stack, an empty folder. The
     * corrections an operation would touch do not exist yet. */
    return true;
  }
  const ChainLayer &reference = chains.first()->layers[layer_index];
  auto sections_disagree = [](const Span<ChainCorrection> reference_sections,
                              const Span<ChainCorrection> sections) {
    if (reference_sections.size() != sections.size()) {
      return true;
    }
    for (const int64_t i : reference_sections.index_range()) {
      if (!BLI_uuid_equal(reference_sections[i].marker, sections[i].marker) ||
          reference_sections[i].section != sections[i].section ||
          reference_sections[i].effect != sections[i].effect)
      {
        return true;
      }
    }
    return false;
  };
  for (const ChannelChain *chain : chains.drop_front(1)) {
    if (layer_index >= chain->layers.size()) {
      r_error = PaintMaterialLayerEditError::ChannelsDisagree;
      return false;
    }
    const ChainLayer &layer = chain->layers[layer_index];
    if (sections_disagree(reference.content_corrections, layer.content_corrections) ||
        sections_disagree(reference.mask_corrections, layer.mask_corrections))
    {
      /* One channel's correction stack drifted from the reference's: the rows no longer say the
       * same thing in every channel, and an edit would move one half of the disagreement. */
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

/**
 * Resolve #PaintMaterialLayerAddParams::anchor_ordinal to the chains a new layer goes into and the
 * index within them.
 *
 * The anchor names a UI row. A plain row -- top level or inside a folder -- takes the new layer
 * directly above itself, in its own chain. A row that is a folder takes it inside, on top of what
 * the folder holds. A folder that holds nothing yet has no chain to resolve: \a r_into_empty_group
 * is set and \a r_empty_group_instances receives its per-channel instance nodes, which is where
 * the
 * mutation wires the first layer's output.
 */
bool add_anchor_resolve(Vector<Vector<ChannelChain>> &per_channel,
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
 * \return the Multiply, or null -- leaving the tree unchanged -- only when it could not be
 * created.
 */
bNode *layer_factor_coverage_link(bNodeTree &tree,
                                         bNode &factor_node,
                                         bNodeSocket &factor,
                                         bNode &coverage_node,
                                         bNodeSocket &coverage,
                                         const float initial_opacity)
{
  bNode *multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (multiply == nullptr) {
    return nullptr;
  }
  multiply->custom1 = NODE_MATH_MULTIPLY;
  bNodeSocket *value_a = static_cast<bNodeSocket *>(BLI_findlink(&multiply->inputs, 0));
  bNodeSocket *value_b = static_cast<bNodeSocket *>(BLI_findlink(&multiply->inputs, 1));
  bNodeSocket *result = static_cast<bNodeSocket *>(multiply->outputs.first);
  if (value_a == nullptr || value_b == nullptr || result == nullptr) {
    bke::node_remove_node(nullptr, tree, *multiply, true);
    return nullptr;
  }
  static_cast<bNodeSocketValueFloat *>(value_b->default_value)->value = initial_opacity;
  bke::node_position_relative(*multiply, coverage_node, result, coverage);
  bke::node_add_link(tree, coverage_node, coverage, *multiply, *value_a);
  bke::node_add_link(tree, *multiply, *result, factor_node, factor);
  return multiply;
}

/**
 * The Absent shape of a layer's Factor in one channel: the same Multiply every layer has, with its
 * coverage input unlinked and explicitly zero. A Math input defaults to 0.5, so leaving it merely
 * unlinked would blend half of nothing over the rows below (invariant I1).
 *
 * \return the Multiply, or null -- leaving the tree unchanged -- when it could not be created.
 */
bNode *layer_factor_absent_link(bNodeTree &tree,
                                       bNode &factor_node,
                                       bNodeSocket &factor,
                                       const float opacity)
{
  bNode *multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (multiply == nullptr) {
    return nullptr;
  }
  multiply->custom1 = NODE_MATH_MULTIPLY;
  bNodeSocket *coverage = static_cast<bNodeSocket *>(BLI_findlink(&multiply->inputs, 0));
  bNodeSocket *value_b = static_cast<bNodeSocket *>(BLI_findlink(&multiply->inputs, 1));
  bNodeSocket *result = static_cast<bNodeSocket *>(multiply->outputs.first);
  if (coverage == nullptr || value_b == nullptr || result == nullptr) {
    bke::node_remove_node(nullptr, tree, *multiply, true);
    return nullptr;
  }
  coverage->default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
  value_b->default_value_typed<bNodeSocketValueFloat>()->value = opacity;
  bke::node_position_relative(*multiply, factor_node, nullptr, factor);
  bke::node_add_link(tree, *multiply, *result, factor_node, factor);
  return multiply;
}

/**
 * The map the caller handed over for \a channel, or null when this channel's map is to be created.
 */
Image *layer_image_given(const PaintMaterialLayerAddParams &params, const int channel)
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
void fill_map_color_for(const int channel, const float fill_color[4], float r_color[4])
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
  if (params.kind == PaintMaterialLayerKind::Fill) {
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
  bke::paint_layer::layer_group_tree_marker_set(*group);

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
 * The Mix node a layer of \a channel blends with, freshly created in \a tree.
 *
 * The Normal channel is the exception the whole file makes: tangent-space maps do not blend
 * component-wise, so the engine's own group does it instead of a Mix node.
 */
bNode *layer_mix_node_create(Main &bmain, bNodeTree &tree, const int channel)
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
    /* The row's own map is the one read below its content corrections; its top socket shows the
     * topmost correction when it has any. A group row has no map of its own -- its instance is
     * what moves. */
    if (!layer.is_group && layer.base_map != nullptr) {
      maps.append(layer.base_map);
      continue;
    }
    bNodeLink *link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
    /* An Absent row has no map. The entry stays as a null so the caller keeps one map per row in
     * the range; #layer_group_fill_channel builds such a row inside the folder with an
     * explicit-zero coverage. */
    maps.append((link != nullptr) ? link->fromnode : nullptr);
  }
  return maps;
}

bool layer_group_fill_channel(Main &bmain,
                              bNodeTree & /*tree*/,
                              bNodeTree &group,
                              ChannelChain &chain,
                              Span<bNode *> map_nodes,
                              const int from_ordinal,
                              const int to_ordinal,
                              const bUUID &keeper_copy_marker,
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
  ChainLayer &keeper = chain.layers[from_ordinal];
  const bool keeper_has_corrections = !keeper.content_corrections.is_empty() ||
                                      !keeper.mask_corrections.is_empty();

  bNode *below = nullptr;
  bNodeSocket *below_out = nullptr;
  bNodeSocket *alpha_so_far = nullptr;
  bNode *alpha_node = nullptr;
  float location_x = 0.0f;

  if (keeper_has_corrections) {
    /* The kept row moves whole: its map becomes the sub-stack's bottom and its corrections hang
     * on a copy of its own Mix -- the row they belong to inside the folder. The kept Mix outside
     * stays, and blends the group instance as the folder's own row. A channel that shows no map
     * for the row (Absent) brings the corrections over nothing: they keep the explicit-zero base
     * they had, and the folder's alpha starts from nothing. */
    /* A kept folder row has no map: its instance is what the folder's alpha starts from. Read
     * while the outer tree's topology cache is still good. */
    bNodeLink *keeper_instance_link = (keeper.is_group && keeper.top != nullptr) ?
                                          sole_link_into(*keeper.top) :
                                          nullptr;
    bNode *keeper_instance = (keeper_instance_link != nullptr) ? keeper_instance_link->fromnode :
                                                                 nullptr;
    Vector<bNode *> owned;
    /* The copy must carry the canonical (unbaked) row: the anchor and its B are outside the owned
     * set, so a baked keeper would move into the folder without its coverage. */
    mask_bake_unbake_row(bmain, keeper, chain.channel);
    layer_owned_nodes_collect(keeper, owned);
    Map<const bNode *, bNode *> node_map;
    if (!layer_owned_nodes_copy(group, owned, socket_map, node_map)) {
      return false;
    }
    bNode *mix_copy = node_map.lookup(keeper.node);
    if (mix_copy == nullptr) {
      return false;
    }
    for (bNode *node : owned) {
      if (node != keeper.node) {
        /* The kept Mix itself stays outside; everything else it owns is in the folder now. */
        r_nodes_to_remove.append_non_duplicates(node);
      }
    }
    if (keeper.base_map != nullptr) {
      below = node_map.lookup(keeper.base_map);
      r_nodes_to_remove.append_non_duplicates(map_nodes.first());
      if (below == nullptr) {
        return false;
      }
      below_out = bke::node_find_socket(*below, SOCK_OUT, "Color"_ustr);
      alpha_so_far = bke::node_find_socket(*below, SOCK_OUT, "Alpha"_ustr);
      if (below_out == nullptr || alpha_so_far == nullptr) {
        return false;
      }
      alpha_node = below;
    }
    else if (keeper_instance != nullptr) {
      if (bNode *const *instance_copy = node_map.lookup_ptr(keeper_instance)) {
        alpha_node = *instance_copy;
        alpha_so_far = socket_find_by_name(**instance_copy, SOCK_OUT, "Alpha");
      }
    }
    bNodeSocket *mix_copy_out = mix_output_find(*mix_copy);
    if (mix_copy_out == nullptr) {
      return false;
    }
    /* The copy is the row the corrections hang on, and the one the rows above blend over. */
    location_x += 300.0f;
    const float delta_x = location_x - mix_copy->location[0];
    for (bNode *copy : node_map.values()) {
      copy->location[0] += delta_x;
    }
    BKE_paint_material_layer_marker_set(*mix_copy, keeper_copy_marker);
    /* The folder's alpha starts from what the kept row covers: its coverage input carries the
     * content corrections' accumulated coverage and its mask chain, which the map's alpha alone
     * would miss. On a baked row that input reads B, which stays outside the group, so the live
     * top is resolved on the original row and the source mapped through the copy. Unlinked, the
     * row covers nothing here and the alpha starts from nothing. */
    group.ensure_topology_cache();
    if (bNodeSocket *live_top = paint_layer_live_coverage_socket(keeper)) {
      /* A coverage with no feed is the row covering nothing here: the alpha starts from nothing,
       * the same form the old coverage-input read produced. */
      bNode *copy_node = nullptr;
      bNodeSocket *copy_socket = nullptr;
      if (bNodeLink *coverage_link = sole_link_into(*live_top)) {
        bNode **found_node = node_map.lookup_ptr(coverage_link->fromnode);
        bNodeSocket **found_socket = socket_map.lookup_ptr(coverage_link->fromsock);
        copy_node = (found_node != nullptr) ? *found_node : nullptr;
        copy_socket = (found_socket != nullptr) ? *found_socket : nullptr;
      }
      alpha_node = copy_node;
      alpha_so_far = copy_socket;
    }
    below = mix_copy;
    below_out = mix_copy_out;
  }
  else if (map_nodes.first() != nullptr) {
    below = bke::node_copy_with_mapping(
        &group, *map_nodes.first(), LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
    if (below == nullptr) {
      return false;
    }
    r_nodes_to_remove.append_non_duplicates(map_nodes.first());
    below_out = bke::node_find_socket(*below, SOCK_OUT, "Color"_ustr);
    alpha_so_far = bke::node_find_socket(*below, SOCK_OUT, "Alpha"_ustr);
    if (below_out == nullptr || alpha_so_far == nullptr) {
      return false;
    }
    alpha_node = below;
  }
  else {
    /* The kept row is Absent in this channel: it has no map to move in, so the folder holds
     * nothing for it and the outer Mix blends an empty group -- the same coverage the row had
     * outside. */
    below = nullptr;
    below_out = nullptr;
    alpha_so_far = nullptr;
    alpha_node = nullptr;
  }

  for (const int ordinal : IndexRange(from_ordinal + 1, to_ordinal - from_ordinal)) {
    ChainLayer &layer = chain.layers[ordinal];
    bNode *map_source = map_nodes[ordinal - from_ordinal];
    bNode *map_copy = nullptr;
    bNode *mix_copy = nullptr;
    bNodeSocket *mix_out = nullptr;
    bNodeSocket *map_alpha = nullptr;
    CompositeMixNode mix;
    /* What the row covers, for the folder's alpha: a plain row's map alpha, or -- for a row with
     * corrections -- whatever feeds its coverage input. */
    bool row_has_corrections = false;
    bNode *row_alpha_node = nullptr;
    bNodeSocket *row_alpha_socket = nullptr;

    if (!layer.content_corrections.is_empty() || !layer.mask_corrections.is_empty()) {
      /* The row moves whole (spec 18 §4.5): its map, its corrections, its coverage Multiply and
       * its mask come over with their links, and only what it blends over -- the row below --
       * is wired here. A channel that shows no map for the row brings the corrections over
       * nothing, the way the kept row's own Absent form does. */
      Vector<bNode *> owned;
      /* Same as the kept row above: unbake before collecting so the live chain, coverage and all,
       * is what moves into the folder. */
      mask_bake_unbake_row(bmain, layer, chain.channel);
      layer_owned_nodes_collect(layer, owned);
      Map<const bNode *, bNode *> node_map;
      if (!layer_owned_nodes_copy(group, owned, socket_map, node_map)) {
        return false;
      }
      /* A folder row's "map" is its instance, which the owned set carries for it. */
      map_copy = layer.is_group ? node_map.lookup_default(map_source, nullptr) :
                 (layer.base_map != nullptr) ? node_map.lookup(layer.base_map) :
                                               nullptr;
      mix_copy = node_map.lookup(layer.node);
      if (mix_copy == nullptr || ((layer.is_group || layer.base_map != nullptr) && map_copy == nullptr))
      {
        return false;
      }
      for (bNode *node : owned) {
        r_nodes_to_remove.append_non_duplicates(node);
      }
      /* The copies' links, added below, invalidate the topology cache
       * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
      group.ensure_topology_cache();
      mix_out = mix_output_find(*mix_copy);
      /* A group instance's sockets carry generated identifiers; its Alpha is found by name. */
      map_alpha = (map_copy == nullptr) ? nullptr :
                  layer.is_group        ? socket_find_by_name(*map_copy, SOCK_OUT, "Alpha") :
                                          bke::node_find_socket(*map_copy, SOCK_OUT, "Alpha"_ustr);
      if (mix_out == nullptr || (map_copy != nullptr && map_alpha == nullptr) ||
          !composite_mix_node_read(*mix_copy, mix))
      {
        return false;
      }
      row_has_corrections = true;
      /* The row's live coverage top, not its coverage input: a baked row reads B there, and B
       * stays outside the group. Resolve the source on the original row and map it through the
       * copy that just moved in. */
      if (bNodeSocket *live_top = paint_layer_live_coverage_socket(layer)) {
        if (bNodeLink *coverage_link = sole_link_into(*live_top)) {
          bNode **copy_node = node_map.lookup_ptr(coverage_link->fromnode);
          bNodeSocket **copy_socket = socket_map.lookup_ptr(coverage_link->fromsock);
          row_alpha_node = (copy_node != nullptr) ? *copy_node : nullptr;
          row_alpha_socket = (copy_socket != nullptr) ? *copy_socket : nullptr;
        }
      }
      location_x += 300.0f;
      const float delta_x = location_x - mix_copy->location[0];
      for (bNode *copy : node_map.values()) {
        copy->location[0] += delta_x;
      }
      bke::node_add_link(
          group, *below, *below_out, *mix_copy, *const_cast<bNodeSocket *>(mix.bottom));
      below = mix_copy;
      below_out = mix_out;
    }
    else if (map_source != nullptr) {
      map_copy = bke::node_copy_with_mapping(
          &group, *map_source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
      mix_copy = bke::node_copy_with_mapping(
          &group, *layer.node, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
      if (map_copy == nullptr || mix_copy == nullptr) {
        return false;
      }
      r_nodes_to_remove.append_non_duplicates(map_source);
      r_nodes_to_remove.append_non_duplicates(layer.node);

      /* A previous pass through this loop may have added links, which invalidates the topology
       * cache #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
      group.ensure_topology_cache();
      mix_out = mix_output_find(*mix_copy);
      bNodeSocket *map_color = bke::node_find_socket(*map_copy, SOCK_OUT, "Color"_ustr);
      map_alpha = bke::node_find_socket(*map_copy, SOCK_OUT, "Alpha"_ustr);
      if (mix_out == nullptr || map_color == nullptr || map_alpha == nullptr ||
          !composite_mix_node_read(*mix_copy, mix))
      {
        return false;
      }
      location_x += 300.0f;
      mix_copy->location[0] = location_x;
      map_copy->location[0] = location_x - 200.0f;

      if (below != nullptr) {
        bke::node_add_link(
            group, *below, *below_out, *mix_copy, *const_cast<bNodeSocket *>(mix.bottom));
      }
      bke::node_add_link(
          group, *map_copy, *map_color, *mix_copy, *const_cast<bNodeSocket *>(mix.top));
      layer_factor_coverage_link(
          group, *mix_copy, *const_cast<bNodeSocket *>(mix.factor), *map_copy, *map_alpha, 1.0f);
      below = mix_copy;
      below_out = mix_out;
    }
    else {
      /* The row is Absent in this channel: copy its Mix alone, leave its map input unlinked and its
       * coverage explicitly zero -- the same form #ensure_mirror_chain builds for a mirrored row,
       * so the folder's chain reads the row as absent too. */
      mix_copy = bke::node_copy_with_mapping(
          &group, *layer.node, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
      if (mix_copy == nullptr) {
        return false;
      }
      r_nodes_to_remove.append_non_duplicates(layer.node);
      group.ensure_topology_cache();
      mix_out = mix_output_find(*mix_copy);
      if (mix_out == nullptr || !composite_mix_node_read(*mix_copy, mix)) {
        return false;
      }
      if (below != nullptr) {
        bke::node_add_link(
            group, *below, *below_out, *mix_copy, *const_cast<bNodeSocket *>(mix.bottom));
      }
      if (layer_factor_absent_link(
              group, *mix_copy, *const_cast<bNodeSocket *>(mix.factor), 1.0f) == nullptr)
      {
        return false;
      }
      location_x += 300.0f;
      mix_copy->location[0] = location_x;
      below = mix_copy;
      below_out = mix_out;
    }

    if (!row_has_corrections) {
      row_alpha_node = map_copy;
      row_alpha_socket = map_alpha;
    }
    if (build_alpha && row_alpha_node != nullptr && row_alpha_socket != nullptr) {
      /* Coverage accumulates the way an "over" does: `a = a_below + a_layer * (1 - a_below)`.
       * The pair hangs one row below the row being added, at the x the loop is tracking -- the
       * layout is this loop's own bookkeeping, so it stays here rather than in the shared
       * helper, which the corrections build with a different one. A row that covers nothing in
       * this channel brings no alpha to accumulate. */
      bNode *invert = nullptr;
      bNodeSocket *combine_out = nullptr;
      bNode *combine = coverage_over_link(group,
                                          alpha_node,
                                          alpha_so_far,
                                          *row_alpha_node,
                                          *row_alpha_socket,
                                          combine_out,
                                          &invert);
      if (combine == nullptr || invert == nullptr || combine_out == nullptr) {
        return false;
      }
      invert->location[0] = location_x;
      invert->location[1] = -300.0f;
      combine->location[0] = location_x + 150.0f;
      combine->location[1] = -300.0f;
      alpha_node = combine;
      alpha_so_far = combine_out;
    }
  }

  bNodeSocket *result_in = socket_find_by_name(*output, SOCK_IN, result_name);
  if (result_in == nullptr) {
    return false;
  }
  /* An empty group -- every row of the range Absent in this channel -- keeps its Result unlinked,
   * which the instance reads as transparent: exactly what the range contributed outside. */
  if (below != nullptr) {
    bke::node_add_link(group, *below, *below_out, *output, *result_in);
  }
  if (build_alpha && alpha_node != nullptr && alpha_so_far != nullptr) {
    if (bNodeSocket *alpha_in = socket_find_by_name(*output, SOCK_IN, "Alpha")) {
      bke::node_add_link(group, *alpha_node, *alpha_so_far, *output, *alpha_in);
    }
  }
  return true;
}

/** Undo everything #layer_nodes_create made, for a transaction that turned out impossible. */
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
int layer_group_depth(bNodeTree &tree, int guard)
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

/** Whether any chain of any channel still ends in a bare Image Texture. */
bool forest_has_bare_bottom(const Vector<Vector<ChannelChain>> &per_channel)
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
void forest_top_chains(Vector<Vector<ChannelChain>> &per_channel,
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
bool moving_group_check(Vector<ChannelChain *> &from_chains,
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
bool tree_reachable_from(const bNodeTree &from,
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
bool tree_write_scope_check(Main &bmain,
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
 *   anchor, #GroupMake's range end. #CorrectionEdit carries the correction section the edit aims
 *   at, as an int.
 * \param move_place: how a #Move lands beside or inside its anchor.
 * \param add_params: the add parameters, required for #Add.
 */
bool layer_edit_plan_build(Main &bmain,
                                  Material &ma,
                                  const int ordinal,
                                  const LayerEditOp op,
                                  LayerEditPlan &r_plan,
                                  PaintMaterialLayerEditError &r_error,
                                  const int target_ordinal,
                                  const PaintMaterialLayerMovePlace move_place,
                                  const PaintMaterialLayerAddParams *add_params)
{
  r_plan = LayerEditPlan{};

  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    r_error = PaintMaterialLayerEditError::NotEditable;
    return false;
  }

  /* An #Add placed at a plain top-level position extends the stack through the flat reader,
   * exactly
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
     * operation; an add only ever touches the top level. Splitting them matters: #forest_top_chains
     * takes the last chain of each entry, so folding every channel into one entry would leave the
     * plan with a single channel and the layer would be added to that one only. */
    for (ChannelChain &chain : flat) {
      Vector<ChannelChain> one_channel;
      one_channel.append(std::move(chain));
      r_plan.per_channel.append(std::move(one_channel));
    }
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
  /* The corrections a row carries are part of what every channel has to agree on, the way its
   * position is: a stack whose channels drifted apart under spec 18 §4.1a is refused before the
   * first byte moves. Runs wherever #r_plan.chains and #r_plan.layer_index name a resolved row;
   * the guards inside #layer_corrections_agree pass the operations that resolve none. */
  auto plan_ready = [&]() {
    if (!layer_corrections_agree(r_plan.chains, r_plan.layer_index, r_error)) {
      return false;
    }
    return write_scope_ok();
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
        return plan_ready();
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
      return plan_ready();
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
      return plan_ready();
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
      return plan_ready();
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
      return plan_ready();
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
      return plan_ready();
    }

    case LayerEditOp::KindSet: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      /* A bare base is accepted, the way #FillColorSet accepts one: the marker is what makes a
       * bare Image Texture read as anything other than a Paint layer. */
      return plan_ready();
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
      return plan_ready();
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
      /* A row absent in a channel has no map there, and a missing map is not a refusal: the fill
       * builds it inside the folder as an explicit-zero row, the same form the channel already
       * showed outside it. Only a row that is not a Mix at all is broken, and the plan's shape
       * checks have already refused that. */
      r_plan.chains = top_chains;
      r_plan.layer_index = ordinal;
      r_plan.target_index = target_ordinal;
      return plan_ready();
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
      return plan_ready();
    }

    case LayerEditOp::Ungroup: {
      if (!top_chains_aligned()) {
        return false;
      }
      if (!ordinal_is_in_chain(ordinal, r_error)) {
        return false;
      }
      if (ordinal < 0 || ordinal >= top_layers_num()) {
        r_error = PaintMaterialLayerEditError::IndexOutOfRange;
        return false;
      }
      if (ordinal == 0) {
        /* A folder can sit at the very bottom: its keeper Mix reverts to blending the inner
         * bottom, which is the same thing ungroup does anywhere else. A bare base at the bottom
         * has no Mix to blend the folder and is refused exactly as before. */
        ChainLayer &bottom_keeper = top_chains.first()->layers[0];
        bNodeLink *bottom_top_link = (bottom_keeper.top == nullptr) ?
                                         nullptr :
                                         sole_link_into(*bottom_keeper.top);
        if (bottom_top_link == nullptr ||
            !BKE_paint_material_is_layer_group(*bottom_top_link->fromnode))
        {
          r_error = PaintMaterialLayerEditError::IndexOutOfRange;
          return false;
        }
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
      return plan_ready();
    }

    case LayerEditOp::CorrectionEdit: {
      if (!forest_rows_resolve(
              r_plan.per_channel, ordinal, r_plan.chains, r_plan.layer_index, r_error))
      {
        return false;
      }
      /* The correction section the edit aims at, carried the way a two-row operation carries its
       * destination row. */
      const auto section = PaintMaterialCorrectionSection(target_ordinal);
      /* A bare base has no Mix node to hang a correction on: the mutator converts it first and
       * plans again, the way an add does for a position the conversion opens up. */
      const bool bare_base = (r_plan.layer_index == 0 && bottom_is_bare(r_plan.chains));
      if (!bare_base && r_plan.chains.first()->layers[r_plan.layer_index].is_group &&
          section == PaintMaterialCorrectionSection::Content)
      {
        /* D7: a group's content stack lives inside its folder, so there is nothing on the group
         * row itself for a content correction to hang on. A mask section limits the folder's
         * result and is allowed. */
        r_error = PaintMaterialLayerEditError::CorrectionNotAllowedOnGroup;
        return false;
      }
      return plan_ready();
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
          const ChainLayer &group_layer =
              r_plan.target_chains.first()->layers[r_plan.target_index];
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
      return plan_ready();
    }
  }
  BLI_assert_unreachable();
  return false;
}

bool BKE_paint_material_layer_bake_endpoint_resolve(Material &ma,
                                                    const bUUID &marker,
                                                    const int channel,
                                                    bNodeSocket **r_color_socket,
                                                    bNodeSocket **r_mask_socket)
{
  *r_color_socket = nullptr;
  *r_mask_socket = nullptr;
  if (ma.nodetree == nullptr || BLI_uuid_is_nil(marker)) {
    /* A nil marker would match every unmarked node -- a bare base among them -- and no row the
     * stack model hands out carries one. */
    return false;
  }
  /* The collection warms the topology cache before it walks, so a caller that just rewrote a link
   * is safe to read. Not a paint stack refuses here like it refuses every reader. */
  Vector<Vector<ChannelChain>> per_channel;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  if (!chains_collect_forest(ma, per_channel, error)) {
    return false;
  }
  for (const Vector<ChannelChain> &forest : per_channel) {
    if (forest.is_empty() || forest.last().channel != channel) {
      continue;
    }
    for (const ChannelChain &chain : forest) {
      if (chain.tree != ma.nodetree) {
        /* The rows of a folder live in the group's own tree, and nothing tracks the instance
         * nodes that route to it: a bake attached at the material tree has no endpoint to reach
         * them by, so those rows are refused rather than resolved to an unroutable socket. */
        continue;
      }
      for (const ChainLayer &layer : chain.layers) {
        if (layer.node == nullptr ||
            !BLI_uuid_equal(marker, BKE_paint_material_layer_marker_get(*layer.node)))
        {
          continue;
        }
        bNodeSocket *color = mix_output_find(*layer.node);
        if (color == nullptr) {
          /* The row is in the chain, but not in a shape a bake can render. */
          return false;
        }
        *r_color_socket = color;
        /* The coverage a bake renders as the row's alpha is the output feeding the Factor input,
         * not the input itself: a bake attaches output sockets. An unlinked Factor is a constant,
         * which no socket renders -- the bake's alpha stays opaque, the shape a full-coverage
         * constant stands for. */
        if (layer.factor != nullptr) {
          if (bNodeLink *factor_link = sole_link_into(*layer.factor)) {
            *r_mask_socket = factor_link->fromsock;
          }
        }
        return true;
      }
    }
  }
  return false;
}

}  // namespace blender
