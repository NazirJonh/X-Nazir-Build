/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_material_layer_edit.hh: adding, moving, reordering, grouping, ungrouping,
 * duplicating and removing a paint layer or a layer group -- the operations that shuffle rows
 * around in the stack, as opposed to the ones that change one row's own properties
 * (`paint_material_layer_props.cc`) or a row's per-channel wiring
 * (`paint_material_layer_channels.cc`). All three share the chain-reading infrastructure in
 * `paint_material_layer_edit_intern.hh`, implemented in `paint_material_layer_chain.cc`.
 *
 * Every operation here works the same way: build a #LayerEditPlan, which checks the
 * preconditions across every channel up front, then rebuild the "what is below me" links from an
 * array. Rebuilding rather than patching is deliberate -- an insert expressed as four unlink/link
 * pairs has four ways to leave the graph half-moved, and the array cannot.
 */

#include "paint_material_layer_edit_intern.hh"

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

#include "paint_material_composite_internal.hh"
#include "paint_material_layer_idprops.hh"

namespace blender {

/** Matches the readers in `paint_material_layer_chain.cc` and `paint_material_layer_model.cc`;
 * see `08 §2.2`, Q2. */
constexpr int LAYER_GROUP_NESTING_MAX = 8;

bUUID BKE_paint_material_layer_marker_get(const bNode &node)
{
  return bke::paint_layer::marker_get(node);
}

void BKE_paint_material_layer_marker_set(bNode &node, const bUUID &layer_id)
{
  bke::paint_layer::marker_set(node, layer_id);
}

int BKE_paint_material_layer_color_tag_get(const bNode &node)
{
  return bke::paint_layer::color_tag_get(node);
}

void BKE_paint_material_layer_color_tag_set(bNode &node, const int color_tag)
{
  bke::paint_layer::color_tag_set(node, color_tag);
}

PaintMaterialLayerKind BKE_paint_material_layer_kind_get(const bNode &node)
{
  return bke::paint_layer::kind_get(node);
}

void BKE_paint_material_layer_kind_set(bNode &node, const PaintMaterialLayerKind kind)
{
  bke::paint_layer::kind_set(node, kind);
}

bool BKE_paint_material_layer_fill_color_get(const bNode &node, float r_color[4])
{
  return bke::paint_layer::fill_color_get(node, r_color);
}

void BKE_paint_material_layer_fill_color_set(bNode &node, const float color[4])
{
  bke::paint_layer::fill_color_set(node, color);
}

const char *BKE_paint_material_layer_edit_error_message(const PaintMaterialLayerEditError error)
{
  switch (error) {
    case PaintMaterialLayerEditError::None:
      return "";
    case PaintMaterialLayerEditError::NotEditable:
      return N_("Material or node tree is linked or overridden");
    case PaintMaterialLayerEditError::NotAStack:
      return N_("Material is not a paint layer stack");
    case PaintMaterialLayerEditError::IndexOutOfRange:
      return N_("No such paint layer");
    case PaintMaterialLayerEditError::IsBottomLayer:
      return N_("The bottom layer has nothing to blend with and cannot be moved");
    case PaintMaterialLayerEditError::ChainIsShared:
      return N_("A layer's result is used elsewhere in the node tree");
    case PaintMaterialLayerEditError::ChainNotPlain:
      return N_("The node chain contains nodes that are not paint layers");
    case PaintMaterialLayerEditError::ChannelsDisagree:
      return N_("The material's channels have different numbers of layers");
    case PaintMaterialLayerEditError::NoPrincipled:
      return N_("The material has no Principled BSDF to build a paint layer stack on");
    case PaintMaterialLayerEditError::CreationFailed:
      return N_("The paint layer's image or nodes could not be created");
    case PaintMaterialLayerEditError::HasGroups:
      return N_("Layers inside a group cannot be edited yet; ungroup it first");
    case PaintMaterialLayerEditError::NestingTooDeep:
      return N_("Groups cannot be nested any deeper");
    case PaintMaterialLayerEditError::TreeNotEditable:
      return N_("The layer group's node tree is linked and cannot be edited");
    case PaintMaterialLayerEditError::TreeIsOverride:
      return N_("The layer group's node tree is a library override and cannot be edited");
    case PaintMaterialLayerEditError::TreeShared:
      return N_("The layer group's node tree is used by another material");
    case PaintMaterialLayerEditError::ChannelHasUnsupportedSource:
      return N_("A needed channel is wired to nodes that are not paint layers; refusing to rewire "
               "it");
    case PaintMaterialLayerEditError::ChannelNotToggleable:
      return N_("A folder's channels follow the layers inside it");
    case PaintMaterialLayerEditError::LastEnabledChannel:
      return N_("A layer keeps at least one channel switched on; hide the layer instead");
    case PaintMaterialLayerEditError::CorrectionNotFound:
      return N_("No correction with this identity is in the stack");
    case PaintMaterialLayerEditError::CorrectionSectionMismatch:
      return N_("Mask corrections have no channel switches; they follow the layer's channels");
    case PaintMaterialLayerEditError::CorrectionNotAllowedOnGroup:
      return N_("Content corrections are not allowed on a layer group");
    case PaintMaterialLayerEditError::CorrectionChainNotPlain:
      return N_("A correction's nodes are not the shape the paint layer stack builds");
    case PaintMaterialLayerEditError::GroupHasMaskCorrections:
      return N_("Remove the folder's mask corrections before ungrouping it");
    case PaintMaterialLayerEditError::MaskNotFound:
      return N_("The layer has no mask");
  }
  return "";
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
  /* The maps the caller handed over are this call's from the start. The ones a surviving node
   * shows
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
    /* Channels that have drifted out of step -- a stray row in one of them, mostly -- refuse
     * every add; bringing them back to the row structure the UI draws is what lets the gesture
     * through. */
    if (error != PaintMaterialLayerEditError::ChannelsDisagree ||
        !BKE_paint_material_layer_channels_realign(bmain, ma, &error) ||
        !layer_edit_plan_build(bmain, ma,
                               params.ordinal,
                               LayerEditOp::Add,
                               plan,
                               error,
                               -1,
                               PaintMaterialLayerMovePlace::Above,
                               &params))
    {
      return fail(error);
    }
  }
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
    /* Nothing to blend over but the hidden base: the first layer is a normal Mix row from the
     * start, so it has a marker, an opacity and a mute like every other row and nothing has to
     * convert it later. */
    Image *image = layer_image_given(params, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (image == nullptr) {
      image = layer_image_create(bmain, PAINT_MATERIAL_CHANNEL_BASE_COLOR, params);
    }
    if (image == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    /* From here on the map is the base add's, whatever it returns: it keeps the map on its node,
     * or frees it with its refusal. Either way this call must not release it again. */
    given_used.add(image);
    const PaintMaterialLayerChannelImage base_map = {PAINT_MATERIAL_CHANNEL_BASE_COLOR, image};
    int base_ordinal = -1;
    if (!BKE_paint_material_layer_add_material_base(
            bmain, ma, Span(&base_map, 1), params.kind, &base_ordinal, &error))
    {
      /* Not `fail`: it forgets which maps are used and would free this one a second time. The
       * other handed-over maps are still this call's to release. */
      given_release();
      if (r_error != nullptr) {
        *r_error = error;
      }
      return false;
    }
    if (params.kind == PaintMaterialLayerKind::Fill) {
      /* The colour lives on the marker, not only in the pixels: a painted-over fill map cannot
       * be asked what it was filled with. Set on the row's own nodes rather than through the
       * ordinal API: the row was just built, so there is nothing to plan or refuse, and no
       * second commit to count. */
      const bUUID layer_id = image->paint_layer_id;
      for (bNode &node : ma.nodetree->nodes) {
        if (BLI_uuid_equal(BKE_paint_material_layer_marker_get(node), layer_id)) {
          BKE_paint_material_layer_fill_color_set(node, params.fill_color);
        }
      }
    }
    if (params.name != nullptr && params.name[0] != '\0') {
      PaintMaterialLayerEditError rename_error = PaintMaterialLayerEditError::None;
      if (!BKE_paint_material_layer_rename(bmain, ma, base_ordinal, params.name, &rename_error)) {
        /* A row built a moment ago in an editable tree has nothing a rename could refuse. */
        BLI_assert_unreachable();
      }
    }
    Vector<ChannelChain> chains;
    if (chains_collect(ma, chains, error)) {
      for (ChannelChain &chain : chains) {
        chain_base_apply(chain);
      }
    }
    return succeed(base_ordinal);
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
  /* A new row has a map only where it is given one -- a Material layer's baked channels -- or, for
   * an empty Paint or Fill layer, in Base Color. Every other channel is Absent: the Mix and its
   * Multiply keep the row's place in the chain, but nothing is painted there until the user turns
   * the channel on. */
  const auto channel_enabled = [&](const int channel) {
    if (!params.channel_images.is_empty()) {
      return layer_image_given(params, channel) != nullptr;
    }
    return channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  };
  for (const int channel : new_channels) {
    NewLayerNodes nodes;
    nodes.channel = channel;
    if (channel_enabled(channel)) {
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
    }
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      /* Tangent-space maps do not blend component-wise; the engine's own group does it
       * properly. */
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
    bNodeSocket *tex_color = (nodes.tex == nullptr) ?
                                 nullptr :
                                 bke::node_find_socket(*nodes.tex, SOCK_OUT, "Color"_ustr);
    bNodeSocket *tex_alpha = (nodes.tex == nullptr) ?
                                 nullptr :
                                 bke::node_find_socket(*nodes.tex, SOCK_OUT, "Alpha"_ustr);
    const bool mix_read = composite_mix_node_read(*nodes.mix, mix);
    if (output == nullptr || !mix_read ||
        (nodes.tex != nullptr && (tex_color == nullptr || tex_alpha == nullptr)))
    {
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
    if (entry.tex != nullptr) {
      bke::node_add_link(tree, *entry.tex, *entry.tex_color, *entry.layer.node, *entry.layer.top);
      layer_factor_coverage_link(
          tree, *entry.layer.node, *entry.factor, *entry.tex, *entry.tex_alpha, 1.0f);
    }
    else {
      layer_factor_absent_link(tree, *entry.layer.node, *entry.factor, 1.0f);
    }
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
    if (entry.tex != nullptr) {
      bke::node_position_relative(
          *entry.tex, *entry.layer.node, entry.tex_color, *entry.layer.top);
    }
    BKE_paint_material_layer_marker_set(*entry.layer.node, layer_id);
    if (params.kind != PaintMaterialLayerKind::Paint) {
      /* The kind lives on the marker, not only in the pixels: a row of baked maps reads as
       * Material rather than as a Paint layer whose maps happen to carry a bake link. */
      BKE_paint_material_layer_kind_set(*entry.layer.node, params.kind);
      if (params.kind == PaintMaterialLayerKind::Fill) {
        BKE_paint_material_layer_fill_color_set(*entry.layer.node, params.fill_color);
      }
    }
    if (entry.layer.image != nullptr) {
      entry.layer.image->paint_layer_id = layer_id;
    }
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
    /* Moving a layer from one tree/chain to another tree/chain (e.g. into/out of a group, or
     * between groups). */
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
      /**
       * A row with corrections moves as one owned unit (spec 18 §4.5): its map, its coverage
       * Multiply, its mask and every node of its corrections, copied into the destination and
       * taken out of the source together. #map_source and #mask_source name single nodes and are
       * not used when this is set.
       */
      bool has_corrections = false;
      Vector<bNode *> owned;
      /** The row's own map, below its content corrections; null when the channel shows none. */
      bNode *base_map_source = nullptr;
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
      m.base_map_source = layer.base_map;

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
      /* A folder row carries its mask corrections along too: its owned set holds its instance. */
      if (!layer.content_corrections.is_empty() || !layer.mask_corrections.is_empty()) {
        m.has_corrections = true;
        layer_owned_nodes_collect(layer, m.owned);
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
      /** Filled for an owned set (a row with corrections); empty for the single-node copies. */
      Map<const bNode *, bNode *> node_map;
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
       * #BKE_paint_material_layer_group_make -- so what has to agree is the tree they open, not
       * the
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

      if (m.has_corrections) {
        /* The owned set comes over with its internal links -- the map into the corrections and
         * the Mix, a mask onto the coverage it drives -- so nothing here rewires them; the only
         * feed left is the row's bottom, which the destination chain's rebuild does. */
        Map<const bNode *, bNode *> node_map;
        if (!layer_owned_nodes_copy(*dst_tree, m.owned, socket_map, node_map)) {
          for (bNode *copy : node_map.values()) {
            created_in_dst.append(copy);
          }
          discard_copies();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        CopiedChannelNodes c;
        c.mix_copy = node_map.lookup(m.mix_source);
        c.map_copy = (m.base_map_source != nullptr) ? node_map.lookup(m.base_map_source) : nullptr;
        c.image = m.image;
        c.node_map = std::move(node_map);
        for (bNode *copy : c.node_map.values()) {
          created_in_dst.append(copy);
        }
        copies.append(c);
        continue;
      }

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
      if (!c.node_map.is_empty()) {
        /* The owned set brought its map, mask, coverage Multiply and correction links along; the
         * row's own handles are resolved here for the destination chain's entry. */
        if (mix_out == nullptr || !composite_mix_node_read(*c.mix_copy, mix)) {
          discard_copies();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        c.mix_bottom = const_cast<bNodeSocket *>(mix.bottom);
        c.mix_top = const_cast<bNodeSocket *>(mix.top);
        c.mix_factor = const_cast<bNodeSocket *>(mix.factor);
        c.mix_output = mix_out;
        continue;
      }
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
      if (m.has_corrections) {
        /* The corrections are children of the row (spec 18 §4.5): the whole owned set goes with
         * it, not just the Mix and the nodes its own sockets happen to show. */
        for (bNode *node : m.owned) {
          nodes_to_remove_from_src.append_non_duplicates(node);
        }
      }
      else {
        nodes_to_remove_from_src.append_non_duplicates(m.mix_source);
        nodes_to_remove_from_src.append_non_duplicates(m.map_source);
        if (m.mask_source != nullptr) {
          nodes_to_remove_from_src.append_non_duplicates(m.mask_source);
        }
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
      /* The row's own map is the copied base map, read below its corrections; the corrections
       * themselves are re-read from the copied links with the next collection. */
      if (!c.node_map.is_empty() && moving_nodes[i].base_map_source != nullptr) {
        layer.base_map = c.node_map.lookup(moving_nodes[i].base_map_source);
      }

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
    /**
     * A row with corrections moves as one owned unit (spec 18 §4.5); see the same field in
     * #layer_move_apply, which shares this shape.
     */
    bool has_corrections = false;
    Vector<bNode *> owned;
    /** The row's own map, below its content corrections; null when the channel shows none. */
    bNode *base_map_source = nullptr;
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
    m.base_map_source = layer.base_map;
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
    /* A folder row carries its mask corrections along too: its owned set holds its instance. */
    if (!layer.content_corrections.is_empty() || !layer.mask_corrections.is_empty()) {
      m.has_corrections = true;
      layer_owned_nodes_collect(layer, m.owned);
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
    /** Filled for an owned set (a row with corrections); empty for the single-node copies. */
    Map<const bNode *, bNode *> node_map;
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

    if (m.has_corrections) {
      /* The owned set comes over with its internal links -- the map into the corrections and the
       * Mix, a mask onto the coverage it drives. Only the row's bottom stays unwired: inside the
       * group it blends over transparency. */
      Map<const bNode *, bNode *> node_map;
      if (!layer_owned_nodes_copy(*group_tree, m.owned, socket_map, node_map)) {
        for (bNode *copy : node_map.values()) {
          created.append(copy);
        }
        discard();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      c.mix_copy = node_map.lookup(m.mix_source);
      c.node_map = std::move(node_map);
      for (bNode *copy : c.node_map.values()) {
        created.append(copy);
      }
      copies.append(c);
      continue;
    }

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

  /* Sockets of the copies -- a group instance among them -- only exist once the tree is
   * updated. */
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

    if (!c.node_map.is_empty()) {
      /* The owned set brought the map, the mask, the coverage Multiply and the correction links
       * along. The row's result becomes what the folder hands out, and what the folder covers is
       * what drives the row's coverage -- the same feed the single-node path takes the raw mask
       * or map alpha from. */
      if (mix_out == nullptr || !composite_mix_node_read(*c.mix_copy, mix) ||
          mix.factor == nullptr)
      {
        discard();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      bNodeSocket *result = socket_find_by_name(*group_output, SOCK_IN, result_name);
      if (result == nullptr) {
        discard();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      /* The bottom stays unlinked: inside the group this layer blends over transparency, which
       * is what the group composites on. */
      bke::node_add_link(*group_tree, *c.mix_copy, *mix_out, *group_output, *result);
      if (i == 0) {
        if (bNodeSocket *alpha_in = socket_find_by_name(*group_output, SOCK_IN, "Alpha")) {
          if (bNodeLink *coverage_link = sole_link_into(*const_cast<bNodeSocket *>(
                  (mix.factor_coverage != nullptr) ? mix.factor_coverage : mix.factor)))
          {
            bke::node_add_link(*group_tree,
                               *coverage_link->fromnode,
                               *coverage_link->fromsock,
                               *group_output,
                               *alpha_in);
          }
        }
      }
      continue;
    }

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
    if (m.has_corrections) {
      /* The corrections are children of the row (spec 18 §4.5): the whole owned set goes with
       * it, not just the Mix and the nodes its own sockets happen to show. */
      for (bNode *node : m.owned) {
        nodes_to_remove.append_non_duplicates(node);
      }
    }
    else {
      nodes_to_remove.append_non_duplicates(m.mix_source);
      nodes_to_remove.append_non_duplicates(m.map_source);
      if (m.mask_source != nullptr) {
        nodes_to_remove.append_non_duplicates(m.mask_source);
      }
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
  const bUUID moved_marker = (from_node != nullptr) ?
                                     BKE_paint_material_layer_marker_get(*from_node) :
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
        if (chains_collect_forest(ma, fresh_per_channel, ignore) &&
            !fresh_per_channel.is_empty())
        {
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
  /* A kept row that carries corrections gets a copy of its own Mix inside the folder -- the row
   * its corrections hang on there. One identity for it, shared by every channel's copy, the way
   * every row carries one marker across its channels. */
  const bUUID keeper_copy_marker = BLI_uuid_generate_random();
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
                                  keeper_copy_marker,
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
   * material's own for a top-level row, or a nested group's own for one already inside a
   * folder. */
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
                                forest_ordinal_for_position(
                                    per_channel.first(), chain_index, insert_at);
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
  for (ChannelChain *chain : plan.chains) {
    if (!chain->layers[plan.layer_index].mask_corrections.is_empty()) {
      /* A folder's mask corrections shape its result as a whole; spliced out, the rows have no
       * result left for them to shape, and moving them onto one row would draw something else. */
      return fail(PaintMaterialLayerEditError::GroupHasMaskCorrections);
    }
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
    /* Set when the sub-stack's bottom row IS the kept Mix's own content stack -- a folder made
     * from a corrections-bearing row (spec 18 §4.5). Its nodes re-attach to the kept Mix, and no
     * new row is spliced in for it. */
    bool bottom_merged = false;
    /* For a merged bottom row: the copied sockets the kept Mix re-adopts its content and its
     * coverage from. */
    bNode *keeper_top_node = nullptr;
    bNodeSocket *keeper_top_socket = nullptr;
    bNode *keeper_factor_node = nullptr;
    bNodeSocket *keeper_factor_socket = nullptr;
    Image *keeper_image = nullptr;
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

      if (index == 0 && !inner_layer.is_mix()) {
        /* The bottom of the sub-stack is a bare map; it goes back onto the group's Mix node. */
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
        layer.output = bke::node_find_socket(*copy, SOCK_OUT, "Color"_ustr);
        layer.factor = bke::node_find_socket(*copy, SOCK_OUT, "Alpha"_ustr);
        if (layer.output == nullptr || layer.factor == nullptr) {
          discard();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        out.layers.append(layer);
        continue;
      }

      const bool has_corrections = !inner_layer.content_corrections.is_empty() ||
                                   !inner_layer.mask_corrections.is_empty();
      if (index == 0 && has_corrections) {
        /* The sub-stack's bottom row is the kept Mix's own content stack -- the folder was made
         * from a corrections-bearing row. Its owned nodes come back out and re-attach to the kept
         * Mix; the Mix the folder held a copy of is not needed, the kept one serves. */
        Vector<bNode *> owned;
        layer_owned_nodes_collect(inner_layer, owned);
        owned.remove_first_occurrence_and_reorder(inner_layer.node);
        Map<const bNode *, bNode *> node_map;
        if (!layer_owned_nodes_copy(tree, owned, socket_map, node_map)) {
          for (bNode *node_copy : node_map.values()) {
            created.append(node_copy);
          }
          discard();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        for (bNode *node_copy : node_map.values()) {
          created.append(node_copy);
        }
        /* What the copy read its content and its coverage from -- a correction's result, or the
           map, and the coverage Multiply -- is what the kept Mix reads once the row is out. */
        bNodeLink *top_feed = (inner_layer.top != nullptr) ?
                                  sole_link_into(*const_cast<bNodeSocket *>(inner_layer.top)) :
                                  nullptr;
        bNodeLink *factor_feed = (inner_layer.factor != nullptr) ?
                                     sole_link_into(
                                         *const_cast<bNodeSocket *>(inner_layer.factor)) :
                                     nullptr;
        if (top_feed == nullptr || factor_feed == nullptr ||
            node_map.lookup_ptr(top_feed->fromnode) == nullptr ||
            node_map.lookup_ptr(factor_feed->fromnode) == nullptr ||
            socket_map.lookup_ptr(top_feed->fromsock) == nullptr ||
            socket_map.lookup_ptr(factor_feed->fromsock) == nullptr)
        {
          discard();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        out.bottom_merged = true;
        out.keeper_top_node = node_map.lookup(top_feed->fromnode);
        out.keeper_top_socket = socket_map.lookup(top_feed->fromsock);
        out.keeper_factor_node = node_map.lookup(factor_feed->fromnode);
        out.keeper_factor_socket = socket_map.lookup(factor_feed->fromsock);
        out.keeper_image = inner_layer.image;
        continue;
      }

      bNode *copy = nullptr;
      if (has_corrections) {
        /* The row's corrections are spliced out with it (spec 18 §4.5): the owned set comes over
         * with its links, and only what the row blends over is left to the chain's rebuild. */
        Vector<bNode *> owned;
        layer_owned_nodes_collect(inner_layer, owned);
        Map<const bNode *, bNode *> node_map;
        if (!layer_owned_nodes_copy(tree, owned, socket_map, node_map)) {
          for (bNode *node_copy : node_map.values()) {
            created.append(node_copy);
          }
          discard();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        for (bNode *node_copy : node_map.values()) {
          created.append(node_copy);
        }
        copy = node_map.lookup(inner_layer.node);
      }
      else {
        copy = bke::node_copy_with_mapping(&tree,
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
      }

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

    if (out.bottom_merged) {
      /* The kept Mix re-adopts the content stack that came out of the folder: it reads the
       * corrections' result and its coverage Multiply again, exactly as it did before grouping. */
      relink_into(
          tree, *keeper.top, *keeper.node, *out.keeper_top_node, *out.keeper_top_socket);
      relink_into(tree,
                  *keeper.factor,
                  *keeper.node,
                  *out.keeper_factor_node,
                  *out.keeper_factor_socket);
      keeper.image = out.keeper_image;
      bke::node_position_relative(
          *out.keeper_top_node, *keeper.node, out.keeper_top_socket, *keeper.top);
    }
    else {
      ChainLayer &bottom = out.layers.first();

      /* The group's Mix node goes back to blending a map, which is what it did before grouping. */
      relink_into(tree, *keeper.top, *keeper.node, *bottom.node, *bottom.output);
      relink_into(tree, *keeper.factor, *keeper.node, *bottom.node, *bottom.factor);
      keeper.image = bottom.image;
      bke::node_position_relative(*bottom.node, *keeper.node, bottom.output, *keeper.top);
    }

    /* The two relinks above, and a previous channel's, invalidate the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    tree.ensure_topology_cache();

    /* Every layer above the bottom one is spliced into the outer chain, in order. */
    int insert_at = plan.layer_index + 1;
    const int64_t splice_from = out.bottom_merged ? 0 : 1;
    for (const int64_t index : out.layers.index_range().drop_front(splice_from)) {
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
    restored_num = int(out.layers.size()) + (out.bottom_merged ? 1 : 0);
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

    /* The folder's mask corrections belong to the group row, not to the tree it opens: the tree
     * copy does not carry them, so they are copied onto the new row once it exists. */
    Vector<PaintMaterialCorrectionRef> mask_refs;
    for (const ChainCorrection &correction : source_layer.mask_corrections) {
      mask_refs.append({ma.id.session_uid, correction.marker});
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
    if (!mask_refs.is_empty()) {
      Vector<bUUID> created_corrections;
      if (!BKE_paint_material_layer_corrections_copy(
              bmain, mask_refs, ma, group_ordinal, created_corrections, nullptr, &error))
      {
        /* The duplicate stands; the corrections that could not follow go with it in the caller's
         * undo step. */
        return fail(error);
      }
    }
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
    /** Filled for an owned set (a row with corrections); empty for the single-node copies. */
    Map<const bNode *, bNode *> node_map;
  };
  /* The maps deep-copied for the duplicate, by their source: a copy that shared the original's
   * maps would be the same layer listed twice -- and a mask correction's shared map must stay one
   * map across the channels, so the copies are made once per source image. */
  Map<Image *, Image *> copied_images;
  auto image_copy_get = [&](Image &source) -> Image * {
    if (Image *const *found = copied_images.lookup_ptr(&source)) {
      return *found;
    }
    Image *copy = id_cast<Image *>(BKE_id_copy(&bmain, &source.id));
    if (copy != nullptr) {
      copied_images.add_new(&source, copy);
    }
    return copy;
  };
  /* One new marker per source marker (spec 18 §4.5): a duplicate's corrections are new rows, not
   * aliases of the originals, and every channel's copy of one correction shares its marker. */
  struct MarkerCopy {
    bUUID source;
    bUUID copy;
  };
  Vector<MarkerCopy> correction_markers;
  struct CorrectionMapCopy {
    bUUID source_marker;
    Image *image = nullptr;
  };
  Vector<CorrectionMapCopy> correction_map_copies;
  /* Copies of the row's own mask, re-tagged with the duplicate's identity once it is minted. */
  Vector<Image *> row_mask_copies;
  /* The first node shown a copy takes the user the copy was created with; each further node -- a
   * mask correction's map is one node per channel -- adds one of its own. The node's user of the
   * original goes back either way. */
  Set<Image *> assigned_copies;
  auto image_copy_assign = [&](bNode &node, Image &copy) {
    id_us_min(node.id);
    node.id = &copy.id;
    if (!assigned_copies.add(&copy)) {
      id_us_plus(&copy.id);
    }
  };
  Vector<DuplicatedLayer> copies;
  auto discard = [&]() {
    for (DuplicatedLayer &copy : copies) {
      if (!copy.node_map.is_empty()) {
        /* The owned set's copies hold real users of their own -- the originals' images, a shared
         * group's tree -- and the removal is what hands those back. */
        for (bNode *node_copy : copy.node_map.values()) {
          bke::node_remove_node(&bmain, tree, *node_copy, true);
        }
        continue;
      }
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
    for (Image *image : copied_images.values()) {
      /* A generated blank no copied node ended up showing goes the way the add's own exits free
       * theirs; one a node took over was already given back by the removal above. */
      if (image->id.us == 0) {
        BKE_id_free(&bmain, image);
      }
    }
    copied_images.clear();
  };

  for (ChannelChain *chain_ptr : plan.chains) {
    ChannelChain &chain = *chain_ptr;
    ChainLayer &source = chain.layers[layer_index];
    DuplicatedLayer copy;
    copy.chain = &chain;

    const bool has_corrections = !source.content_corrections.is_empty() ||
                                 !source.mask_corrections.is_empty();
    if (has_corrections) {
      /* The corrections are duplicated with the row (spec 18 §4.5): the owned set comes over with
       * its links, the maps it shows become new Image data-blocks, and the corrections get new
       * identities. */
      Vector<bNode *> owned;
      layer_owned_nodes_collect(source, owned);
      Map<const bNodeSocket *, bNodeSocket *> socket_map;
      Map<const bNode *, bNode *> node_map;
      if (!layer_owned_nodes_copy(tree, owned, socket_map, node_map)) {
        copy.node_map = std::move(node_map);
        copies.append(copy);
        discard();
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      copy.mix = node_map.lookup(source.node);
      copy.tex = (source.base_map != nullptr) ? node_map.lookup(source.base_map) : nullptr;
      if (copy.tex != nullptr) {
        Image *image_copy = (source.image != nullptr) ? image_copy_get(*source.image) : nullptr;
        if (image_copy == nullptr) {
          copy.node_map = std::move(node_map);
          copies.append(copy);
          discard();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        /* The copied texture node still points at the original map, with the user
         * #node_copy_with_mapping gave it, and is about to stop: without this the original keeps
         * a user it no longer has. The copy needs no matching #id_us_plus -- a freshly created ID
         * already carries one user, and this node is it. */
        image_copy_assign(*copy.tex, *image_copy);
        copy.image = image_copy;
      }
      for (const Vector<ChainCorrection> *rows :
           {&source.content_corrections, &source.mask_corrections})
      {
        for (const ChainCorrection &correction : *rows) {
          if (correction.map == nullptr || correction.image == nullptr) {
            /* Absent in this channel: the copied correction stays Absent the same way. */
            continue;
          }
          Image *image_copy = image_copy_get(*correction.image);
          if (image_copy == nullptr) {
            copy.node_map = std::move(node_map);
            copies.append(copy);
            discard();
            return fail(PaintMaterialLayerEditError::CreationFailed);
          }
          image_copy_assign(*node_map.lookup(correction.map), *image_copy);
          correction_map_copies.append({correction.marker, image_copy});
        }
      }
      /* The row's own mask travels in the owned set too; a duplicate still showing it would paint
       * into the original's mask. */
      const bUUID source_marker = BKE_paint_material_layer_marker_get(*source.node);
      for (bNode *node : owned) {
        if (node->type_legacy != SH_NODE_TEX_IMAGE || node == source.base_map ||
            node->id == nullptr || GS(node->id->name) != ID_IM)
        {
          continue;
        }
        Image &image = *id_cast<Image *>(node->id);
        if (image.paint_layer_channel != PAINT_LAYER_MAP_MASK ||
            !BLI_uuid_equal(image.paint_layer_id, source_marker))
        {
          continue;
        }
        Image *mask_copy = image_copy_get(image);
        if (mask_copy == nullptr) {
          copy.node_map = std::move(node_map);
          copies.append(copy);
          discard();
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        image_copy_assign(*node_map.lookup(node), *mask_copy);
        row_mask_copies.append_non_duplicates(mask_copy);
      }
      for (const Vector<ChainCorrection> *rows :
           {&source.content_corrections, &source.mask_corrections})
      {
        for (const ChainCorrection &correction : *rows) {
          if (correction.mix == nullptr) {
            continue;
          }
          bUUID new_marker = BLI_uuid_nil();
          for (const MarkerCopy &marker : correction_markers) {
            if (BLI_uuid_equal(marker.source, correction.marker)) {
              new_marker = marker.copy;
              break;
            }
          }
          if (BLI_uuid_is_nil(new_marker)) {
            new_marker = BLI_uuid_generate_random();
            correction_markers.append({correction.marker, new_marker});
          }
          bke::paint_layer::marker_set(*node_map.lookup(correction.mix), new_marker);
        }
      }
      copy.node_map = std::move(node_map);
      copies.append(copy);
      continue;
    }

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

  /* A correction's map carries its identity the way a layer's own maps carry the row's: the
   * copied maps are re-tagged with the duplicate's regenerated markers. */
  for (const CorrectionMapCopy &map_copy : correction_map_copies) {
    for (const MarkerCopy &marker : correction_markers) {
      if (BLI_uuid_equal(marker.source, map_copy.source_marker)) {
        map_copy.image->paint_layer_id = marker.copy;
        break;
      }
    }
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
    /* The map copy is only looked at on the single-node path; an owned set carries its own links
     * and needs none of the rewiring. */
    bNodeSocket *tex_color = (copy.tex != nullptr) ?
                                 bke::node_find_socket(*copy.tex, SOCK_OUT, "Color"_ustr) :
                                 nullptr;
    bNodeSocket *tex_alpha = (copy.tex != nullptr) ?
                                 bke::node_find_socket(*copy.tex, SOCK_OUT, "Alpha"_ustr) :
                                 nullptr;
    if (chain == nullptr || output == nullptr ||
        (copy.tex != nullptr && (tex_color == nullptr || tex_alpha == nullptr)) ||
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

    if (copy.node_map.is_empty()) {
      bke::node_add_link(tree, *copy.tex, *tex_color, *copy.mix, *layer.top);
      layer_factor_coverage_link(
          tree, *copy.mix, *const_cast<bNodeSocket *>(mix.factor), *copy.tex, *tex_alpha, 1.0f);
    }
    else {
      /* The owned set is already wired: the map reads into the corrections and the corrections
         into the Mix, and the coverage Multiply the source row had came along with its opacity. */
      layer.base_map = copy.tex;
    }
    bke::node_position_relative(*copy.mix, *chain->terminal_node, output, *chain->terminal);
    if (copy.node_map.is_empty()) {
      bke::node_position_relative(*copy.tex, *copy.mix, tex_color, *layer.top);
    }

    chain->layers.insert(insert_at, layer);
    chain_rebuild_links(*chain);
    /* A copy is a different layer, so it gets an identity of its own rather than the
     * original's. */
    BKE_paint_material_layer_marker_set(*copy.mix, layer_id);
    if (copy.image != nullptr) {
      copy.image->paint_layer_id = layer_id;
    }
  }

  for (Image *mask : row_mask_copies) {
    mask->paint_layer_id = layer_id;
  }
  /* A channel with no graph of its own (AO) holds a correction's map by its tag alone, with no
   * node the owned copy could carry it through: the duplicate's corrections get deep copies
   * tagged with their new identity, each held by its creation user the way an enabled AO map is. */
  for (const MarkerCopy &marker : correction_markers) {
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (info.socket_name != nullptr) {
        continue;
      }
      Image *source_map = correction_tagged_map_find(bmain, marker.source, info.channel);
      if (source_map == nullptr) {
        continue;
      }
      if (Image *map_copy = id_cast<Image *>(BKE_id_copy(&bmain, &source_map->id))) {
        map_copy->paint_layer_id = marker.copy;
      }
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
    /** Every node the row owns, for a row with corrections; empty otherwise. */
    Vector<bNode *> owned;
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
    ResolvedRemoval entry;
    entry.chain = chain_ptr;
    entry.top_link = top_link;
    entry.bare_base = bare_base;
    entry.map_is_sole_user = map_is_sole_user;
    if (!removed.content_corrections.is_empty() || !removed.mask_corrections.is_empty()) {
      layer_owned_nodes_collect(removed, entry.owned);
    }
    resolved.append(std::move(entry));
  }
  /* A mask correction's map is one node every channel's correction reads, so a map is kept only
   * when something outside *every* channel's owned set reads it -- decided now, while the links
   * are the ones the sets were read from -- and each node is queued once. */
  Vector<bNode *> all_owned;
  for (const ResolvedRemoval &entry : resolved) {
    for (bNode *node : entry.owned) {
      all_owned.append_non_duplicates(node);
    }
  }
  Set<bNode *> kept_maps;
  for (bNode *node : all_owned) {
    if (node->type_legacy == SH_NODE_TEX_IMAGE && !layer_owned_node_consumed_by(all_owned, *node))
    {
      kept_maps.add(node);
    }
  }
  Set<bNode *> removal_queued;

  for (const ResolvedRemoval &entry : resolved) {
    ChannelChain &chain = *entry.chain;
    if (entry.bare_base) {
      /* Removing a bare base: the layer above becomes the new bottom, and since it has nothing
       * left to blend over, the Image Texture it carried takes the base's place. A uniform chain
       * needs none of this -- its next layer down already blends over transparency. */
      if (entry.top_link == nullptr) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      ChainLayer &above = chain.layers[1];
      nodes_to_remove.append({chain.tree, chain.layers[0].node});
      if (!above.content_corrections.is_empty() || !above.mask_corrections.is_empty()) {
        /* The row above keeps its map as the new base; its corrections, its coverage Multiply and
         * its mask go with the Mix that carried them (spec 18 §4.5). */
        if (above.base_map == nullptr) {
          return fail(PaintMaterialLayerEditError::ChainNotPlain);
        }
        for (bNode *node : entry.owned) {
          if (node == above.base_map) {
            /* The map stays: it is the new bottom the chain reads. */
            continue;
          }
          if (removal_queued.add(node)) {
            nodes_to_remove.append({chain.tree, node});
          }
        }
        ChainLayer new_base;
        new_base.node = above.base_map;
        new_base.output = bke::node_find_socket(*above.base_map, SOCK_OUT, "Color"_ustr);
        new_base.image = above.image;
        if (new_base.output == nullptr) {
          return fail(PaintMaterialLayerEditError::ChainNotPlain);
        }
        chain.layers.remove(0);
        chain.layers[0] = new_base;
      }
      else {
        ChainLayer new_base;
        new_base.node = entry.top_link->fromnode;
        new_base.output = entry.top_link->fromsock;
        new_base.image = chain.layers[1].image;
        nodes_to_remove.append({chain.tree, chain.layers[1].node});
        chain.layers.remove(0);
        chain.layers[0] = new_base;
      }
    }
    else {
      ChainLayer &removed = chain.layers[layer_index];
      if (!removed.content_corrections.is_empty() || !removed.mask_corrections.is_empty()) {
        /* The corrections are children of the row (spec 18 §4.5): the whole owned set goes with
         * it. A map some hand-wired consumer outside the set still reads stays, the same rule
         * that keeps a map shared with another row below. */
        for (bNode *node : entry.owned) {
          if (kept_maps.contains(node)) {
            continue;
          }
          if (removal_queued.add(node)) {
            nodes_to_remove.append({chain.tree, node});
          }
        }
      }
      else {
        nodes_to_remove.append({chain.tree, chain.layers[layer_index].node});
        /* The map that only this layer read goes with it; one shared with another layer stays. */
        if (entry.map_is_sole_user) {
          nodes_to_remove.append({chain.tree, entry.top_link->fromnode});
        }
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
