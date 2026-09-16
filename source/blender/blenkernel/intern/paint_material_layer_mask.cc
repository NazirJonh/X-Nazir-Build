/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * A paint layer's mask: adding it, removing it, and switching it on or off without losing it. The
 * mask is one image for the whole row, tagged as such (#Image::paint_layer_channel), and every
 * channel of the row wires the same node into its coverage. See #BKE_paint_material_layer_edit.hh
 * for the public API and `paint_material_layer_mask_intern.hh` for the facts the other mutators
 * read.
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
#include "BKE_paint_material_layer_model.hh"
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
#include "paint_material_layer_mask_intern.hh"

namespace blender {

Image *paint_layer_mask_image_find(Main &bmain, const bUUID &marker)
{
  for (Image &image : bmain.images) {
    if (image.paint_layer_channel == PAINT_LAYER_MAP_MASK &&
        BLI_uuid_equal(image.paint_layer_id, marker))
    {
      return &image;
    }
  }
  return nullptr;
}

bNode *layer_mask_node_find(bNodeTree &tree, const bUUID &marker)
{
  for (bNode &node : tree.nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE || node.id == nullptr || GS(node.id->name) != ID_IM)
    {
      continue;
    }
    const Image &image = *id_cast<const Image *>(node.id);
    if (image.paint_layer_channel == PAINT_LAYER_MAP_MASK &&
        BLI_uuid_equal(image.paint_layer_id, marker))
    {
      return &node;
    }
  }
  return nullptr;
}

bool paint_layer_mask_is_enabled(const Image &image)
{
  return image.paint_layer_mask_disabled == 0;
}

/**
 * The socket the row's mask feeds when it is on: the base of the lowest mask correction when the
 * row has any (spec 18 §4.3), the coverage input of the Factor Multiply otherwise. Null when the
 * row's Factor is not the shape this module builds.
 */
static bNodeSocket *layer_mask_base_socket(ChainLayer &layer, CompositeMixNode &r_mix)
{
  if (!layer.mask_corrections.is_empty()) {
    const ChainCorrection &lowest = layer.mask_corrections.first();
    CompositeMixNode lowest_mix;
    if (lowest.mix == nullptr || !composite_mix_node_read(*lowest.mix, lowest_mix) ||
        lowest_mix.bottom == nullptr)
    {
      return nullptr;
    }
    return const_cast<bNodeSocket *>(lowest_mix.bottom);
  }
  if (layer.node == nullptr || !composite_mix_node_read(*layer.node, r_mix) ||
      r_mix.factor == nullptr)
  {
    return nullptr;
  }
  if (r_mix.factor_opacity != nullptr) {
    return const_cast<bNodeSocket *>(r_mix.factor_coverage);
  }
  return const_cast<bNodeSocket *>(r_mix.factor);
}

void layer_coverage_source_without_mask(const ChainLayer &layer,
                                        bNode *&r_node,
                                        bNodeSocket *&r_socket)
{
  r_node = nullptr;
  r_socket = nullptr;
  if (!layer.content_corrections.is_empty() &&
      layer.content_corrections.last().over_combine != nullptr)
  {
    r_node = layer.content_corrections.last().over_combine;
    r_socket = static_cast<bNodeSocket *>(r_node->outputs.first);
    return;
  }
  if (layer.is_group && layer.top != nullptr) {
    if (bNodeLink *top_link = sole_link_into(*layer.top)) {
      r_node = top_link->fromnode;
      r_socket = socket_find_by_name(*r_node, SOCK_OUT, "Alpha");
      if (r_socket != nullptr) {
        return;
      }
      r_node = nullptr;
    }
  }
  if (layer.base_map != nullptr) {
    r_node = layer.base_map;
    r_socket = bke::node_find_socket(*r_node, SOCK_OUT, "Alpha"_ustr);
  }
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
    if (!layer.mask_corrections.is_empty()) {
      /* The mask becomes the base of the mask-correction chain (spec 18 §4.3): it feeds what the
       * lowest mask correction blends over, exactly where the coverage the row had without the
       * mask used to sit, so the mask defines the coverage the corrections then shape. A channel
       * whose chain base is unlinked keeps the explicit-zero form (I1) -- the mask is picked up
       * when the channel gets coverage. */
      const ChainCorrection &lowest = layer.mask_corrections.first();
      CompositeMixNode lowest_mix;
      if (lowest.mix == nullptr || !composite_mix_node_read(*lowest.mix, lowest_mix) ||
          lowest_mix.bottom == nullptr)
      {
        continue;
      }
      bNodeSocket &bottom = *const_cast<bNodeSocket *>(lowest_mix.bottom);
      if (!socket_has_link(bottom)) {
        continue;
      }
      relink_into(*chain->tree, bottom, *lowest.mix, *tex, *color);
      continue;
    }
    if (mix.factor_opacity != nullptr) {
      if (composite_mix_coverage_off(mix)) {
        /* Absent or Disabled here: the mask must not become this channel's coverage (I1). It is
         * picked up when the channel is switched on. */
        continue;
      }
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
  /* The masks whose nodes go below, for the orphan check at the end: a generated blank nothing
   * reads anymore must not keep answering as the row's mask to the model and the paint canvas. */
  Vector<Image *> mask_images;
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
    if (!layer.mask_corrections.is_empty()) {
      /* The mask corrections stay; what goes is the mask image at the chain's base, and what
       * comes back is the coverage the row had without the mask -- the content corrections'
       * accumulated coverage, or the row's own map's alpha. */
      const ChainCorrection &lowest = layer.mask_corrections.first();
      CompositeMixNode lowest_mix;
      if (lowest.mix == nullptr || !composite_mix_node_read(*lowest.mix, lowest_mix) ||
          lowest_mix.bottom == nullptr)
      {
        continue;
      }
      bNodeSocket &bottom = *const_cast<bNodeSocket *>(lowest_mix.bottom);
      bNodeLink *mask_link = sole_link_into(bottom);
      if (mask_link == nullptr) {
        /* Nothing feeds the chain's base here -- the Absent form: no mask in this channel. */
        continue;
      }
      bNode *mask_node = mask_link->fromnode;
      if (mask_node->type_legacy != SH_NODE_TEX_IMAGE || mask_node->id == nullptr ||
          GS(mask_node->id->name) != ID_IM)
      {
        continue;
      }
      Image &mask_image = *id_cast<Image *>(mask_node->id);
      /* The row's own mask is the tagged one; the map's alpha (or a correction's over output) at
         the base is the no-mask shape, not a mask to remove. */
      if (mask_image.paint_layer_channel != PAINT_LAYER_MAP_MASK ||
          !BLI_uuid_equal(mask_image.paint_layer_id,
                          BKE_paint_material_layer_marker_get(*layer.node)))
      {
        continue;
      }
      bNode *base_node = nullptr;
      bNodeSocket *base_socket = nullptr;
      if (!layer.content_corrections.is_empty()) {
        const ChainCorrection &top = layer.content_corrections.last();
        if (top.over_combine != nullptr) {
          base_node = top.over_combine;
          base_socket = static_cast<bNodeSocket *>(base_node->outputs.first);
        }
      }
      if (base_node == nullptr && layer.base_map != nullptr) {
        base_node = layer.base_map;
        base_socket = bke::node_find_socket(*base_node, SOCK_OUT, "Alpha"_ustr);
      }
      /* Counted before the relink: the Color output's remaining links say whether the node is
         the last reader's, the same rule the plain path below follows. */
      if (mask_link->fromsock->directly_linked_links().size() == 1) {
        mask_nodes.append_non_duplicates({chain.tree, mask_node});
        mask_images.append_non_duplicates(&mask_image);
      }
      if (base_node != nullptr && base_socket != nullptr) {
        relink_into(*chain.tree, bottom, *lowest.mix, *base_node, *base_socket);
      }
      else {
        for (bNodeLink *link : Vector<bNodeLink *>(bottom.directly_linked_links())) {
          BKE_ntree_update_tag_link_removed(chain.tree);
          bke::node_remove_link(chain.tree, *link);
        }
      }
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
    /* What covers the row without its mask: the content corrections' accumulated coverage, a
     * folder's alpha, or the row's own map's alpha -- the same order #layer_mask_corrections_sync
     * and #BKE_paint_material_layer_mask_set_enabled restore. Resolved before deciding what "the
     * mask" even is: a row whose coverage already comes from this source -- the default shape now,
     * not just the masked one -- has no mask to remove, and mistaking that source for one would
     * delete it. */
    bNode *base_node = nullptr;
    bNodeSocket *base_socket = nullptr;
    layer_coverage_source_without_mask(layer, base_node, base_socket);
    if (base_node != nullptr && mask_link->fromnode == base_node &&
        mask_link->fromsock == base_socket)
    {
      continue;
    }
    if (mask_link->fromsock->directly_linked_links().size() == 1) {
      mask_nodes.append_non_duplicates({chain.tree, mask_link->fromnode});
    }
    if (base_node == nullptr || base_socket == nullptr) {
      /* Nothing to restore coverage from; leaving it unlinked is still a layer without a mask. */
      for (bNodeLink *link : Vector<bNodeLink *>(coverage_socket->directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(chain.tree);
        bke::node_remove_link(chain.tree, *link);
      }
      continue;
    }
    /* The base node is taken from the helper rather than from a link read after the previous
     * channel's relink, which already invalidated the topology cache #owner_node asserts on. */
    if (mix.factor_opacity != nullptr) {
      relink_into(*chain.tree, *coverage_socket, coverage_owner, *base_node, *base_socket);
    }
    else {
      /* No opacity to preserve here -- wrap the restored coverage in a fresh Multiply so the
       * layer keeps an editable one going forward. */
      layer_factor_coverage_link(
          *chain.tree, *layer.node, *coverage_socket, *base_node, *base_socket, 1.0f);
    }
  }

  Set<bNodeTree *> touched_trees;
  const bUUID remove_marker = BKE_paint_material_layer_marker_get(
      *plan.chains.first()->layers[layer_index].node);
  /* Every node the two loops queued is a mask Image Texture: its image is the row's mask and has to
   * go with it. Collected from the node queue rather than per branch so the plain path and the
   * correction path cannot drift -- an image left tagged and user-less keeps answering as the row's
   * mask to the stack model's maps-by-tag pass, so the mask would look like it was never removed.
   * The tag is checked, not just the node's type: the plain path's node is a coverage source this
   * file did not create, and an image that only looks like a map must not be freed here. */
  for (const std::pair<bNodeTree *, bNode *> &entry : mask_nodes) {
    if (entry.second->id == nullptr || GS(entry.second->id->name) != ID_IM) {
      continue;
    }
    Image &image = *id_cast<Image *>(entry.second->id);
    if (image.paint_layer_channel == PAINT_LAYER_MAP_MASK &&
        BLI_uuid_equal(image.paint_layer_id, remove_marker))
    {
      mask_images.append_non_duplicates(&image);
    }
  }
  for (const std::pair<bNodeTree *, bNode *> &entry : mask_nodes) {
    bke::node_remove_node(&bmain, *entry.first, *entry.second, true);
    BKE_ntree_update_tag_node_removed(entry.first);
    touched_trees.add(entry.first);
  }

  /* A switched-off mask has no link for the loops above to follow (#BKE_paint_material_layer_
   * mask_set_enabled); its nodes go too, found by tag, so a remove on a switched-off mask leaves
   * nothing behind either. */
  {
    for (ChannelChain *chain_ptr : plan.chains) {
      ChannelChain &chain = *chain_ptr;
      bNode *mask_node = layer_mask_node_find(*chain.tree, remove_marker);
      if (mask_node == nullptr) {
        continue;
      }
      /* The mask may still be linked in a channel this pass skipped (Disabled, I1) -- a reader
       * besides the row's coverage is not something this remove owns. */
      bNodeSocket *mask_color = bke::node_find_socket(*mask_node, SOCK_OUT, "Color"_ustr);
      const bool still_read = mask_color != nullptr &&
                              !mask_color->directly_linked_links().is_empty();
      if (still_read) {
        for (bNodeLink *link : Vector<bNodeLink *>(mask_color->directly_linked_links())) {
          BKE_ntree_update_tag_link_removed(chain.tree);
          bke::node_remove_link(chain.tree, *link);
        }
      }
      Image &mask_image = *id_cast<Image *>(mask_node->id);
      if (mask_image.id.us <= 1 && mask_image.source == IMA_SRC_GENERATED) {
        mask_images.append_non_duplicates(&mask_image);
      }
      bke::node_remove_node(&bmain, *chain.tree, *mask_node, true);
      BKE_ntree_update_tag_node_removed(chain.tree);
      touched_trees.add(chain.tree);
    }
  }

  for (Image *image : mask_images) {
    /* The node removal gave the mask's user back; a generated blank nothing reads anymore is
     * freed the way every other exit in this module disposes of its own orphans -- left tagged
     * and user-less it would still reach the stack model's maps-by-tag pass. */
    if (image->id.us == 0 && image->source == IMA_SRC_GENERATED) {
      BKE_id_free(&bmain, image);
    }
  }
  mask_images.clear();
  for (bNodeTree *touched : touched_trees) {
    if (touched != ma.nodetree) {
      BKE_ntree_update_after_single_tree_change(bmain, *touched);
    }
  }

  /* The mask is gone, so the row's mask corrections read as applying again: the mask-correction
   * sync re-evaluates what the row puts in and re-links the chain's base and each correction's
   * coverage, the way #BKE_paint_material_layer_mask_set_enabled does. Without it a row whose mask
   * was switched off keeps its chain zeroed, and a row that was baked keeps an anchor built for a
   * mask that no longer exists. */
  for (ChannelChain *chain : plan.chains) {
    layer_mask_corrections_sync(
        bmain, ma, *chain->tree, chain->layers[layer_index], chain->channel);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_mask_set_enabled(Main &bmain,
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

  /* 1. Preflight: the row exists and a bare base is refused, without writing a byte. The
   * #MaskRemove plan is the right set of checks: a row that can lose its mask can switch it off. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskRemove, plan, error)) {
    return fail(error);
  }
  const int layer_index = plan.layer_index;

  /* The mask is one image for the whole row, found by tag rather than by link -- a switched-off
   * mask has none. */
  const bUUID row_marker = BKE_paint_material_layer_marker_get(
      *plan.chains.first()->layers[layer_index].node);
  Image *mask_image = paint_layer_mask_image_find(bmain, row_marker);
  if (mask_image == nullptr) {
    /* Corrections alone are not a mask to switch: what they shape is the row's coverage either
     * way, and each of them has its own toggle. */
    return fail(PaintMaterialLayerEditError::MaskNotFound);
  }
  if (paint_layer_mask_is_enabled(*mask_image) == enable) {
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  }

  /* 2. Mutation: from here, a refusal is impossible. The flag is written before the links so the
   * readers that pick the mask up by tag (#layer_coverage_restore, #layer_mask_corrections_sync)
   * already see the state the links below express. */
  mask_image->paint_layer_mask_disabled = enable ? 0 : 1;

  Set<bNodeTree *> touched_trees;
  for (ChannelChain *chain_ptr : plan.chains) {
    ChannelChain &chain = *chain_ptr;
    ChainLayer &layer = chain.layers[layer_index];
    chain.tree->ensure_topology_cache();
    CompositeMixNode mix;
    bNodeSocket *base = layer_mask_base_socket(layer, mix);
    if (base == nullptr) {
      continue;
    }
    /* The mask's node in this tree, linked or not. */
    bNode *mask_node = layer_mask_node_find(*chain.tree, row_marker);
    if (mask_node == nullptr) {
      continue;
    }
    bNodeSocket *mask_color = bke::node_find_socket(*mask_node, SOCK_OUT, "Color"_ustr);
    if (mask_color == nullptr) {
      continue;
    }
    if (!enable) {
      bNodeLink *mask_link = sole_link_into(*base);
      if (mask_link == nullptr || mask_link->fromnode != mask_node) {
        /* The base the reader resolved is not the socket the mask actually feeds -- a shape it
         * read differently than the writer left it (an opacity Multiply the detector no longer
         * recognizes, or a stale base under a mask correction). Find the mask's own link on this
         * row's Mix instead of trusting the base, so a switched-off mask cannot stay wired while
         * the flag -- and the row's icon -- already say off. */
        for (bNodeLink *link : Vector<bNodeLink *>(mask_color->directly_linked_links())) {
          if (link->fromnode == mask_node && link->tonode == layer.node) {
            base = link->tosock;
            mask_link = link;
            break;
          }
        }
      }
      if (mask_link == nullptr || mask_link->fromnode != mask_node) {
        /* Not this channel's coverage -- a channel switched off (I1) or one that never wired the
         * mask in; nothing to take out. */
        continue;
      }
      bNode *source_node = nullptr;
      bNodeSocket *source_socket = nullptr;
      layer_coverage_source_without_mask(layer, source_node, source_socket);
      if (source_node != nullptr && source_socket != nullptr) {
        relink_into(*chain.tree, *base, base->owner_node(), *source_node, *source_socket);
      }
      else {
        for (bNodeLink *link : Vector<bNodeLink *>(base->directly_linked_links())) {
          BKE_ntree_update_tag_link_removed(chain.tree);
          bke::node_remove_link(chain.tree, *link);
        }
      }
      if (!layer.mask_corrections.is_empty()) {
        /* The corrections shape the mask: a switched-off mask leaves them nothing to shape, so
         * re-sync turns their coverage off (and back on when the mask comes back). */
        layer_mask_corrections_sync(bmain, ma, *chain.tree, layer, chain.channel);
      }
      touched_trees.add(chain.tree);
      continue;
    }
    /* The mask sits at the base of the row's mask-correction chain, so its own socket is that
     * chain's bottom, not the layer Mix's factor. The layer's factor shape (#mix) is only read
     * for a row without mask corrections; reading it here would dereference a default-constructed
     * mix and wire the mask into the wrong node. */
    const bool at_correction_base = !layer.mask_corrections.is_empty();
    if (!at_correction_base && mix.factor_opacity != nullptr && composite_mix_coverage_off(mix)) {
      /* Absent or Disabled here: the mask must not become this channel's coverage (I1). It is
       * picked up when the channel is switched on. */
      continue;
    }
    if (at_correction_base || mix.factor_opacity != nullptr) {
      relink_into(*chain.tree, *base, base->owner_node(), *mask_node, *mask_color);
    }
    else {
      /* A bare constant Factor: wrap the mask in a fresh Multiply the way #BKE_paint_material_
       * layer_mask_add does, so the row keeps an editable opacity. */
      const float initial_opacity = static_cast<const bNodeSocketValueFloat *>(
                                        mix.factor->default_value)
                                        ->value;
      layer_factor_coverage_link(*chain.tree,
                                 *layer.node,
                                 *base,
                                 *mask_node,
                                 *mask_color,
                                 initial_opacity);
    }
    if (!layer.mask_corrections.is_empty()) {
      /* The corrections shape the mask, so a switched-off mask leaves them no pixels to shape:
       * re-sync turns their coverage off (and back on when the mask comes back). */
      layer_mask_corrections_sync(bmain, ma, *chain.tree, layer, chain.channel);
    }
    touched_trees.add(chain.tree);
  }

  for (bNodeTree *touched : touched_trees) {
    BKE_ntree_update_after_single_tree_change(bmain, *touched);
  }
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_stack_contains_mask(Main &bmain, Material &ma, const Image &image)
{
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, ma, entries)) {
    return false;
  }
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr) == &image) {
      return true;
    }
  }
  return false;
}

}  // namespace blender
