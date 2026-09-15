/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The build half of correction layers (spec 18): a correction is a child of a layer, and in one
 * channel it owns a Mix of the same shape the layer's own is, a Multiply of coverage by opacity,
 * and -- for a content section -- a Subtract + Multiply-Add pair accumulating coverage the way an
 * "over" does. Its Mix sits inline between the layer's map (or the correction below it) and the
 * layer's own Mix -- at the layer's map input for a content section, at the coverage input of the
 * layer's Multiply for a mask one.
 *
 * One channel is built per call; the caller collects the channels first and updates the tree
 * once, after the last one, so no half-built correction ever reaches the evaluator.
 */

#include "paint_material_layer_edit_intern.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "DEG_depsgraph.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.hh"

#include <string>
#include <utility>

#include "paint_material_composite_internal.hh"
#include "paint_material_layer_idprops.hh"

namespace blender {

namespace {

/**
 * The coverage a content correction stacked on \a source accumulates over: the map's Alpha, or
 * the over output of the correction already sitting there. Null when \a source is neither, which
 * reads as `a_below` zero rather than as a broken shape.
 */
bNodeSocket *correction_below_coverage_find(bNodeTree &tree, bNode &source)
{
  if (source.type_legacy == SH_NODE_TEX_IMAGE) {
    return bke::node_find_socket(source, SOCK_OUT, "Alpha"_ustr);
  }
  CompositeMixNode mix;
  if (!bke::paint_layer::node_is_correction(source) || !composite_mix_node_read(source, mix)) {
    return nullptr;
  }
  /* Its accumulated coverage leaves by the Multiply-Add of its over pair, which consumes the
   * correction's own coverage Multiply -- the node feeding its Factor. */
  tree.ensure_topology_cache();
  ChainCorrection nodes;
  if (!correction_nodes_read(source, mix, nodes) ||
      nodes.section != PaintMaterialCorrectionSection::Content || nodes.over_combine == nullptr)
  {
    return nullptr;
  }
  return static_cast<bNodeSocket *>(nodes.over_combine->outputs.first);
}

/**
 * The map a layer's content comes from, walked down through any content corrections sitting on
 * it -- their bottom side is the map, not their own Mix. Null when the walk meets anything else:
 * a group instance, an unwired input, a shape this file does not build.
 */
const bNode *layer_map_node_find(const CompositeMixNode &mix)
{
  const bNodeSocket *socket = mix.top;
  for (int step = 0; step < 64; step++) {
    if (socket == nullptr) {
      return nullptr;
    }
    const bNode *source = composite_source_node_shallow(*socket);
    if (source == nullptr) {
      return nullptr;
    }
    if (source->type_legacy == SH_NODE_TEX_IMAGE) {
      return source;
    }
    CompositeMixNode below;
    if (!bke::paint_layer::node_is_correction(*source) ||
        !composite_mix_node_read(*source, below))
    {
      return nullptr;
    }
    socket = below.bottom;
  }
  return nullptr;
}

}  // namespace

bNode *coverage_over_link(bNodeTree &tree,
                          bNode *below_node,
                          bNodeSocket *below_socket,
                          bNode &layer_node,
                          bNodeSocket &layer_alpha,
                          bNodeSocket *&r_output,
                          bNode **r_invert)
{
  r_output = nullptr;
  if (r_invert != nullptr) {
    *r_invert = nullptr;
  }
  BLI_assert(below_socket == nullptr || below_node != nullptr);

  /* `1 - a_below`, and `a_layer * (1 - a_below) + a_below`. */
  bNode *invert = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *combine = (invert != nullptr) ? bke::node_add_static_node(nullptr, tree, SH_NODE_MATH) :
                                         nullptr;
  if (invert == nullptr || combine == nullptr) {
    if (invert != nullptr) {
      bke::node_remove_node(nullptr, tree, *invert, true);
    }
    return nullptr;
  }
  invert->custom1 = NODE_MATH_SUBTRACT;
  combine->custom1 = NODE_MATH_MULTIPLY_ADD;
  bNodeSocket *invert_a = static_cast<bNodeSocket *>(BLI_findlink(&invert->inputs, 0));
  bNodeSocket *invert_b = static_cast<bNodeSocket *>(BLI_findlink(&invert->inputs, 1));
  bNodeSocket *invert_out = static_cast<bNodeSocket *>(invert->outputs.first);
  bNodeSocket *combine_a = static_cast<bNodeSocket *>(BLI_findlink(&combine->inputs, 0));
  bNodeSocket *combine_b = static_cast<bNodeSocket *>(BLI_findlink(&combine->inputs, 1));
  bNodeSocket *combine_c = static_cast<bNodeSocket *>(BLI_findlink(&combine->inputs, 2));
  bNodeSocket *combine_out = static_cast<bNodeSocket *>(combine->outputs.first);
  if (invert_a == nullptr || invert_b == nullptr || invert_out == nullptr ||
      combine_a == nullptr || combine_b == nullptr || combine_c == nullptr ||
      combine_out == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *combine, true);
    bke::node_remove_node(nullptr, tree, *invert, true);
    return nullptr;
  }
  static_cast<bNodeSocketValueFloat *>(invert_a->default_value)->value = 1.0f;
  /* `a_below` is zero, not the 0.5 a Math input defaults to (invariant I1) -- also when it is
   * linked: a muted source (a Disabled base map) is dropped by shader localization, and the
   * consumer then reads its own default value in place of the link. */
  invert_b->default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
  combine_c->default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
  if (below_socket != nullptr) {
    bke::node_add_link(tree, *below_node, *below_socket, *invert, *invert_b);
  }
  bke::node_add_link(tree, layer_node, layer_alpha, *combine, *combine_a);
  bke::node_add_link(tree, *invert, *invert_out, *combine, *combine_b);
  if (below_socket != nullptr) {
    bke::node_add_link(tree, *below_node, *below_socket, *combine, *combine_c);
  }
  r_output = combine_out;
  if (r_invert != nullptr) {
    *r_invert = invert;
  }
  return combine;
}

void correction_over_gate_set(bNodeTree &tree,
                              bNode &factor_multiply,
                              bNode &over_invert,
                              bNode &over_combine,
                              const bool enable)
{
  bNodeSocket *multiply_out = static_cast<bNodeSocket *>(factor_multiply.outputs.first);
  bNodeSocket *combine_a = static_cast<bNodeSocket *>(BLI_findlink(&over_combine.inputs, 0));
  bNodeSocket *invert_a = static_cast<bNodeSocket *>(BLI_findlink(&over_invert.inputs, 0));
  if (multiply_out == nullptr || combine_a == nullptr || invert_a == nullptr) {
    return;
  }
  tree.ensure_topology_cache();
  /* Resolved while the cache is good: the relinks below leave it stale. */
  bNode &combine_owner = combine_a->owner_node();
  bNode &invert_owner = invert_a->owner_node();
  auto feed = [&](bNodeSocket &socket, bNode &owner, const bool linked) {
    tree.ensure_topology_cache();
    for (bNodeLink *link : Vector<bNodeLink *>(socket.directly_linked_links())) {
      BKE_ntree_update_tag_link_removed(&tree);
      bke::node_remove_link(&tree, *link);
    }
    if (linked) {
      bNodeLink &added = bke::node_add_link(tree, factor_multiply, *multiply_out, owner, socket);
      BKE_ntree_update_tag_link_added(&tree, &added);
    }
  };
  if (enable) {
    /* On: the Multiply-Add's A carries the correction's own alpha again, and the Subtract's first
     * input goes back to the `1 - a_below` constant the pair was built with. */
    feed(*combine_a, combine_owner, true);
    feed(*invert_a, invert_owner, false);
    invert_a->default_value_typed<bNodeSocketValueFloat>()->value = 1.0f;
    BKE_ntree_update_tag_socket_property(&tree, invert_a);
  }
  else {
    /* Off (invariant I1): the Multiply-Add's A is unlinked and explicitly zero, so the pair
     * computes `0*b + c = a_below` -- a switched-off row covers nothing. The Multiply's output
     * rides on the Subtract's first input instead, where it cannot reach the result: the one link
     * that keeps the pair findable from the row's own Multiply even when nothing sits below it. */
    feed(*combine_a, combine_owner, false);
    combine_a->default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
    BKE_ntree_update_tag_socket_property(&tree, combine_a);
    feed(*invert_a, invert_owner, true);
  }
}

void correction_row_enabled_apply(bNodeTree &tree, const ChainCorrection &nodes, const bool enable)
{
  if (nodes.mix == nullptr) {
    return;
  }
  SET_FLAG_FROM_TEST(nodes.mix->flag, !enable, NODE_MUTED);
  BKE_ntree_update_tag_node_mute(&tree, nodes.mix);
  if (nodes.over_combine != nullptr && nodes.over_invert != nullptr &&
      nodes.factor_multiply != nullptr)
  {
    correction_over_gate_set(
        tree, *nodes.factor_multiply, *nodes.over_invert, *nodes.over_combine, enable);
  }
  if (nodes.map == nullptr) {
    return;
  }
  /* A muted Mix passes its first *linked* colour input: with nothing under the correction that is
   * its own map. The map mutes with the row, so shader localization drops it and the consumer
   * reads the explicit black the insert gave the base. A content map also stays muted while its
   * channel is Disabled (coverage unlinked); a mask's map is shared by every channel and follows
   * the row alone. */
  bool map_on = enable;
  if (map_on && nodes.section == PaintMaterialCorrectionSection::Content) {
    tree.ensure_topology_cache();
    CompositeMixNode corr;
    map_on = composite_mix_node_read(*nodes.mix, corr) && corr.factor_coverage != nullptr &&
             !corr.factor_coverage->directly_linked_links().is_empty();
  }
  channel_map_mute_set(tree, *nodes.map, map_on);
}

void layer_mask_corrections_sync(bNodeTree &tree, ChainLayer &layer)
{
  if (layer.node == nullptr || layer.mask_corrections.is_empty()) {
    return;
  }
  tree.ensure_topology_cache();
  /* Whether the row puts anything into this channel (spec 18 I2'): its base map is on, or a
   * content correction paints. A shape the base reader does not know -- a folder -- is taken as
   * contributing, the way it was before masks were derived. */
  PaintMaterialLayerChannelState base_state = PaintMaterialLayerChannelState::Enabled;
  const bool base_known = !layer.is_group &&
                          composite_layer_base_state_get(*layer.node, base_state);
  const bool contributes = !base_known || base_state == PaintMaterialLayerChannelState::Enabled ||
                           row_channel_painted_by_corrections(layer);

  const ChainCorrection &lowest = layer.mask_corrections.first();
  CompositeMixNode lowest_mix;
  if (lowest.mix == nullptr || !composite_mix_node_read(*lowest.mix, lowest_mix) ||
      lowest_mix.bottom == nullptr)
  {
    return;
  }
  bNodeSocket &base = const_cast<bNodeSocket &>(*lowest_mix.bottom);
  if (contributes) {
    if (!socket_has_link(base)) {
      /* The coverage the row has without its mask corrections, the order #layer_coverage_restore
       * and #layer_mask_base_consumer agree on: the row's mask image, the content corrections'
       * accumulated coverage, the folder's alpha, the row's own map alpha. */
      bNode *source_node = nullptr;
      bNodeSocket *source_socket = nullptr;
      const bUUID row_marker = bke::paint_layer::marker_get(*layer.node);
      for (bNode &node : tree.nodes) {
        if (node.type_legacy != SH_NODE_TEX_IMAGE || node.id == nullptr ||
            GS(node.id->name) != ID_IM)
        {
          continue;
        }
        const Image &image = *id_cast<const Image *>(node.id);
        if (image.paint_layer_channel == PAINT_LAYER_MAP_MASK &&
            BLI_uuid_equal(image.paint_layer_id, row_marker))
        {
          /* A switched-off mask does not come back as the chain's base; the coverage falls to the
           * next source down, the same as for a row without a mask. */
          if (image.paint_layer_mask_disabled == 0) {
            source_node = &node;
            source_socket = bke::node_find_socket(node, SOCK_OUT, "Color"_ustr);
          }
          break;
        }
      }
      if (source_socket == nullptr && !layer.content_corrections.is_empty() &&
          layer.content_corrections.last().over_combine != nullptr)
      {
        source_node = layer.content_corrections.last().over_combine;
        source_socket = static_cast<bNodeSocket *>(source_node->outputs.first);
      }
      if (source_socket == nullptr && layer.is_group && layer.top != nullptr) {
        if (bNodeLink *top_link = sole_link_into(*layer.top)) {
          source_node = top_link->fromnode;
          source_socket = socket_find_by_name(*source_node, SOCK_OUT, "Alpha");
        }
      }
      if (source_socket == nullptr && layer.base_map != nullptr) {
        source_node = layer.base_map;
        source_socket = bke::node_find_socket(*source_node, SOCK_OUT, "Alpha"_ustr);
      }
      if (source_node != nullptr && source_socket != nullptr) {
        relink_into(tree, base, *lowest.mix, *source_node, *source_socket);
      }
    }
  }
  else {
    /* Nothing of the row's own in this channel (I1): no base under the mask chain, so a mask
     * painted in a blend that adds coverage cannot bring a row back that paints nothing. */
    for (bNodeLink *link : Vector<bNodeLink *>(base.directly_linked_links())) {
      BKE_ntree_update_tag_link_removed(&tree);
      bke::node_remove_link(&tree, *link);
    }
  }

  for (const ChainCorrection &nodes : layer.mask_corrections) {
    tree.ensure_topology_cache();
    CompositeMixNode corr;
    if (nodes.mix == nullptr || nodes.factor_multiply == nullptr ||
        !composite_mix_node_read(*nodes.mix, corr) || corr.factor_coverage == nullptr)
    {
      continue;
    }
    bNodeSocket &coverage = const_cast<bNodeSocket &>(*corr.factor_coverage);
    if (contributes && nodes.map != nullptr) {
      if (!socket_has_link(coverage)) {
        if (bNodeSocket *alpha = bke::node_find_socket(*nodes.map, SOCK_OUT, "Alpha"_ustr)) {
          relink_into(tree, coverage, *nodes.factor_multiply, *nodes.map, *alpha);
        }
      }
    }
    else if (!contributes) {
      /* The same unlinked-and-zero form a Disabled channel keeps (I1): the mask correction's own
       * factor is zero here, and its Mix passes the zero base through. */
      for (bNodeLink *link : Vector<bNodeLink *>(coverage.directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *link);
      }
      coverage.default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
      BKE_ntree_update_tag_socket_property(&tree, &coverage);
    }
  }
}

bNodeSocket *layer_mask_base_consumer(ChainLayer &layer)
{
  if (layer.node == nullptr) {
    return nullptr;
  }
  CompositeMixNode mix;
  if (!composite_mix_node_read(*layer.node, mix) || mix.factor_coverage == nullptr) {
    return nullptr;
  }
  const bNode *coverage_source = composite_source_node_shallow(*mix.factor_coverage);
  if (coverage_source != nullptr && bke::paint_layer::node_is_correction(*coverage_source)) {
    if (bke::paint_layer::correction_section_get(*coverage_source) !=
        PaintMaterialCorrectionSection::Mask)
    {
      /* A content correction's over output already feeds the base; the next one replaces it
       * there, which is how several content sections stack. */
      return const_cast<bNodeSocket *>(mix.factor_coverage);
    }
    /* A mask correction owns the coverage input: the accumulated coverage feeds its base. */
    CompositeMixNode mask_mix;
    return composite_mix_node_read(*coverage_source, mask_mix) ?
               const_cast<bNodeSocket *>(mask_mix.bottom) :
               nullptr;
  }
  if (coverage_source != nullptr && coverage_source->type_legacy == SH_NODE_TEX_IMAGE &&
      coverage_source != layer_map_node_find(mix))
  {
    /* A mask image owns the coverage, and the accumulated coverage goes nowhere: what the mask
     * modulates is the layer's own map, not a correction's. */
    return nullptr;
  }
  return const_cast<bNodeSocket *>(mix.factor_coverage);
}

bool correction_nodes_read(const bNode &node,
                           const CompositeMixNode &corr,
                           ChainCorrection &r_nodes)
{
  r_nodes = ChainCorrection{};
  r_nodes.mix = const_cast<bNode *>(&node);
  r_nodes.marker = bke::paint_layer::marker_get(node);
  r_nodes.section = bke::paint_layer::correction_section_get(node);
  r_nodes.effect = bke::paint_layer::correction_effect_get(node);

  /* The coverage Multiply the correction's Factor hangs on, stamped there by every insert. */
  if (corr.factor == nullptr) {
    return false;
  }
  bNodeLink *factor_link = sole_link_into(*const_cast<bNodeSocket *>(corr.factor));
  if (factor_link == nullptr || factor_link->fromnode->type_legacy != SH_NODE_MATH ||
      NodeMathOperation(factor_link->fromnode->custom1) != NODE_MATH_MULTIPLY)
  {
    return false;
  }
  r_nodes.factor_multiply = factor_link->fromnode;

  /* The correction's own map, when this channel shows one (Absent otherwise): the one Image
   * Texture its top input reads. Read for both sections here, before the Mask early-return below
   * -- a mask correction's own painted map lives on this same input, exactly like a layer's map or
   * a content correction's, and #correction_walk_step refuses any other shape wired there. */
  if (corr.top != nullptr) {
    bNodeLink *map_link = sole_link_into(*const_cast<bNodeSocket *>(corr.top));
    if (map_link != nullptr && map_link->fromnode->type_legacy == SH_NODE_TEX_IMAGE) {
      r_nodes.map = map_link->fromnode;
      r_nodes.image = (r_nodes.map->id != nullptr && GS(r_nodes.map->id->name) == ID_IM) ?
                          id_cast<Image *>(r_nodes.map->id) :
                          nullptr;
    }
  }
  if (r_nodes.section == PaintMaterialCorrectionSection::Mask) {
    /* A mask section limits coverage instead of accumulating it: it owns no over pair. */
    return true;
  }

  /* The Subtract + Multiply-Add pair accumulating coverage the way an "over" does; the pair
   * consumes the coverage Multiply's output, so it is found through that output's consumers. */
  bNodeSocket *multiply_out = static_cast<bNodeSocket *>(r_nodes.factor_multiply->outputs.first);
  if (multiply_out == nullptr) {
    return false;
  }
  bNode *invert = nullptr;
  for (const bNodeLink *link : multiply_out->directly_linked_links()) {
    bNode &consumer = *link->tonode;
    if (consumer.type_legacy == SH_NODE_MATH &&
        NodeMathOperation(consumer.custom1) == NODE_MATH_MULTIPLY_ADD)
    {
      r_nodes.over_combine = &consumer;
      break;
    }
  }
  if (r_nodes.over_combine == nullptr) {
    /* The switched-off row (invariant I1): the Multiply-Add's A input is unlinked and explicitly
     * zero, so the pair hangs off the Multiply only through its gate -- the link riding on the
     * Subtract's first input (#correction_over_gate_set). That Subtract, whose output still feeds
     * the Multiply-Add's second input, is the pair's own. */
    for (const bNodeLink *link : multiply_out->directly_linked_links()) {
      bNode &consumer = *link->tonode;
      if (consumer.type_legacy != SH_NODE_MATH ||
          NodeMathOperation(consumer.custom1) != NODE_MATH_SUBTRACT)
      {
        continue;
      }
      bNodeSocket *invert_out = static_cast<bNodeSocket *>(consumer.outputs.first);
      if (invert_out == nullptr) {
        continue;
      }
      for (const bNodeLink *out_link : invert_out->directly_linked_links()) {
        bNode &combine = *out_link->tonode;
        if (combine.type_legacy != SH_NODE_MATH ||
            NodeMathOperation(combine.custom1) != NODE_MATH_MULTIPLY_ADD)
        {
          continue;
        }
        /* Only the gate shape itself: the Subtract lands on the Multiply-Add's B, and A is the
         * unlinked zero. A hand-wired Subtract into some other Multiply-Add is not the pair. */
        const bNodeSocket *combine_a = static_cast<const bNodeSocket *>(
            BLI_findlink(&combine.inputs, 0));
        const bNodeSocket *combine_b = static_cast<const bNodeSocket *>(
            BLI_findlink(&combine.inputs, 1));
        if (combine_a == nullptr || combine_b == nullptr ||
            !combine_a->directly_linked_links().is_empty() || out_link->tosock != combine_b)
        {
          continue;
        }
        r_nodes.over_combine = &combine;
        invert = &consumer;
        break;
      }
      if (r_nodes.over_combine != nullptr) {
        break;
      }
    }
    if (r_nodes.over_combine == nullptr) {
      return false;
    }
  }
  if (invert == nullptr) {
    /* The pair's Subtract feeds the Multiply-Add's second input, its `1 - a_below`. */
    bNodeSocket *combine_b = static_cast<bNodeSocket *>(
        BLI_findlink(&r_nodes.over_combine->inputs, 1));
    if (combine_b == nullptr || combine_b->directly_linked_links().size() != 1) {
      return false;
    }
    invert = combine_b->directly_linked_links()[0]->fromnode;
    if (invert->type_legacy != SH_NODE_MATH ||
        NodeMathOperation(invert->custom1) != NODE_MATH_SUBTRACT)
    {
      return false;
    }
  }
  r_nodes.over_invert = invert;
  return true;
}

void layer_owned_nodes_collect(const ChainLayer &layer, Vector<bNode *> &r_nodes)
{
  r_nodes.clear();
  if (layer.node == nullptr) {
    return;
  }
  r_nodes.append(layer.node);
  auto add = [&](const bNode *node) {
    if (node != nullptr) {
      r_nodes.append_non_duplicates(const_cast<bNode *>(node));
    }
  };
  /* A group row's content is the folder instance on its map input: it has no #base_map, and
   * without the instance a copied row would blend nothing. */
  if (layer.is_group && layer.top != nullptr) {
    if (bNodeLink *top_link = sole_link_into(*layer.top)) {
      add(top_link->fromnode);
    }
  }

  /* The coverage Multiply the row's Factor hangs on: the row's own opacity lives on it, and the
   * mask and the mask corrections hang off its coverage input. A legacy row links its Factor
   * straight to the coverage source -- there is no Multiply, and the walk below finds what hangs
   * there. */
  CompositeMixNode mix;
  const bool mix_read = composite_mix_node_read(*layer.node, mix);
  if (mix_read && mix.factor != nullptr) {
    if (bNodeLink *factor_link = sole_link_into(*const_cast<bNodeSocket *>(mix.factor))) {
      bNode &feed = *factor_link->fromnode;
      if (feed.type_legacy == SH_NODE_MATH &&
          NodeMathOperation(feed.custom1) == NODE_MATH_MULTIPLY)
      {
        add(&feed);
      }
    }
  }
  /* The row's own map, read below the content corrections. */
  add(layer.base_map);
  for (const Vector<ChainCorrection> *rows :
       {&layer.content_corrections, &layer.mask_corrections})
  {
    for (const ChainCorrection &correction : *rows) {
      add(correction.mix);
      add(correction.factor_multiply);
      add(correction.over_invert);
      add(correction.over_combine);
      add(correction.map);
    }
  }
  /* The row's own mask, when it has one: the Image Texture at the base of the coverage path --
   * below the mask corrections, or straight on the coverage input. The row's own map's alpha and
   * a content correction's over output are coverage sources, not masks, and are already in the
   * set through the map and the corrections. */
  const bNodeSocket *coverage_base = nullptr;
  if (!layer.mask_corrections.is_empty()) {
    CompositeMixNode lowest;
    if (composite_mix_node_read(*layer.mask_corrections.first().mix, lowest)) {
      coverage_base = lowest.bottom;
    }
  }
  else if (mix_read) {
    coverage_base = (mix.factor_coverage != nullptr) ? mix.factor_coverage : mix.factor;
  }
  if (coverage_base != nullptr) {
    if (bNodeLink *feed_link = sole_link_into(*const_cast<bNodeSocket *>(coverage_base))) {
      if (feed_link->fromnode->type_legacy == SH_NODE_TEX_IMAGE) {
        add(feed_link->fromnode);
      }
    }
  }
}

bool layer_owned_nodes_copy(bNodeTree &dst_tree,
                            const Span<bNode *> nodes,
                            Map<const bNodeSocket *, bNodeSocket *> &r_socket_map,
                            Map<const bNode *, bNode *> &r_node_map)
{
  for (bNode *node : nodes) {
    bNode *copy = bke::node_copy_with_mapping(
        &dst_tree, *node, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, r_socket_map);
    if (copy == nullptr) {
      return false;
    }
    r_node_map.add_new(node, copy);
  }
  /* The links inside the set survive the copy; the ones reaching in from outside it -- what the
   * row blends over, mostly -- are the caller's to rebuild or leave off. */
  for (bNode *node : nodes) {
    for (const bNodeSocket *input : node->input_sockets()) {
      for (const bNodeLink *link : input->directly_linked_links()) {
        bNode **from_copy = r_node_map.lookup_ptr(link->fromnode);
        if (from_copy == nullptr) {
          continue;
        }
        bNodeSocket **from_sock = r_socket_map.lookup_ptr(link->fromsock);
        bNodeSocket **to_sock = r_socket_map.lookup_ptr(link->tosock);
        if (from_sock == nullptr || to_sock == nullptr) {
          continue;
        }
        bke::node_add_link(
            dst_tree, **from_copy, **from_sock, *r_node_map.lookup(link->tonode), **to_sock);
      }
    }
  }
  return true;
}

bool layer_owned_node_consumed_by(const Span<bNode *> owned, const bNode &node)
{
  for (const bNodeSocket *output : node.output_sockets()) {
    for (const bNodeLink *link : output->directly_linked_links()) {
      if (!owned.contains(const_cast<bNode *>(link->tonode))) {
        return false;
      }
    }
  }
  return true;
}

bool correction_channel_insert(Main &bmain,
                               ChannelChain &chain,
                               ChainLayer &layer,
                               PaintMaterialCorrectionSection section,
                               PaintMaterialCorrectionEffect effect,
                               const bUUID &marker,
                               ChainCorrection &r_nodes)
{
  r_nodes = ChainCorrection{};
  if (chain.tree == nullptr || layer.node == nullptr) {
    return false;
  }
  bNodeTree &tree = *chain.tree;
  /* Every read below walks links; a caller that just built a node has left the cache stale. */
  tree.ensure_topology_cache();

  /* Everything is resolved before the first node exists, so a refusal here leaves the tree
   * exactly as it was -- the transaction shape every edit in this module shares. */
  CompositeMixNode layer_mix;
  if (!composite_mix_node_read(*layer.node, layer_mix)) {
    return false;
  }
  /* D7: a content section hangs off the layer's own content stack, which a group keeps inside
   * its folder; a mask section limits the folder's result and is allowed. */
  if (layer.is_group && section == PaintMaterialCorrectionSection::Content) {
    return false;
  }
  /* A content section sits on the layer's map input; a mask one on the coverage input of the
   * layer's own Multiply, which only the per-channel shape has. */
  bNodeSocket *top_socket = (section == PaintMaterialCorrectionSection::Content) ?
                                const_cast<bNodeSocket *>(layer_mix.top) :
                                const_cast<bNodeSocket *>(layer_mix.factor_coverage);
  if (top_socket == nullptr) {
    return false;
  }
  bNodeLink *old_link = sole_link_into(*top_socket);
  if (old_link == nullptr && socket_has_link(*top_socket)) {
    /* Several feeds is a shape this file never rewrites, as everywhere else in the module. */
    return false;
  }
  bNode *old_source = (old_link != nullptr) ? old_link->fromnode : nullptr;
  bNodeSocket *old_from_socket = (old_link != nullptr) ? old_link->fromsock : nullptr;
  /* The Multiply the coverage input belongs to, resolved while the cache is still good: the
   * nodes created below invalidate it, and the relink asks for the owner afterwards. */
  bNode *coverage_owner = (section == PaintMaterialCorrectionSection::Mask) ?
                              &top_socket->owner_node() :
                              nullptr;
  bNode *below_node = nullptr;
  bNodeSocket *below_socket = nullptr;
  bNodeSocket *consumer = nullptr;
  bNode *consumer_owner = nullptr;
  if (section == PaintMaterialCorrectionSection::Content) {
    consumer = layer_mask_base_consumer(layer);
    if (consumer != nullptr) {
      consumer_owner = &consumer->owner_node();
    }
    if (old_source != nullptr) {
      below_node = old_source;
      below_socket = correction_below_coverage_find(tree, *old_source);
    }
  }

  /* The writes. A refusal from here is a creation failure, and undoes its own nodes. */
  bNode *mix = layer_mix_node_create(bmain, tree, chain.channel);
  if (mix == nullptr) {
    return false;
  }
  if (chain.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* The correction's Mix in the Normal channel is a Normal Combine instance, whose sockets only
     * the updater instantiates; a plain Mix node carries its sockets from birth. */
    BKE_ntree_update_after_single_tree_change(bmain, tree);
  }
  tree.ensure_topology_cache();
  CompositeMixNode corr;
  bNodeSocket *mix_out = mix_output_find(*mix);
  if (mix_out == nullptr || !composite_mix_node_read(*mix, corr) || corr.factor == nullptr ||
      corr.bottom == nullptr)
  {
    bke::node_remove_node(&bmain, tree, *mix, true);
    return false;
  }
  if (mix->type_legacy == SH_NODE_MIX && corr.bottom->type == SOCK_RGBA) {
    /* Black, the colour the compositor starts a correction stack from. Read whenever nothing
     * feeds the base -- an Absent base, or a Disabled one whose muted map shader localization
     * drops -- where the Mix default's grey would tint the corrections above. */
    const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    copy_v4_v4(const_cast<bNodeSocket *>(corr.bottom)
                   ->default_value_typed<bNodeSocketValueRGBA>()
                   ->value,
               black);
  }
  /* Absent in this channel: the coverage input of the correction's own Multiply stays unlinked
   * and explicitly zero, the shape every layer keeps its Value slider on (invariant I1). */
  bNode *multiply = layer_factor_absent_link(
      tree, *mix, *const_cast<bNodeSocket *>(corr.factor), 1.0f);
  bNodeSocket *multiply_out = (multiply != nullptr) ?
                                  static_cast<bNodeSocket *>(multiply->outputs.first) :
                                  nullptr;
  bNode *over_combine = nullptr;
  bNode *over_invert = nullptr;
  bNodeSocket *over_out = nullptr;
  if (multiply_out != nullptr && section == PaintMaterialCorrectionSection::Content) {
    over_combine = coverage_over_link(
        tree, below_node, below_socket, *multiply, *multiply_out, over_out, &over_invert);
  }
  if (multiply == nullptr || multiply_out == nullptr ||
      (section == PaintMaterialCorrectionSection::Content &&
       (over_combine == nullptr || over_invert == nullptr || over_out == nullptr)))
  {
    if (multiply != nullptr) {
      bke::node_remove_node(&bmain, tree, *multiply, true);
    }
    bke::node_remove_node(&bmain, tree, *mix, true);
    return false;
  }

  /* Whatever fed the section's socket now feeds the correction's base instead. */
  if (old_source != nullptr) {
    bke::node_add_link(
        tree, *old_source, *old_from_socket, *mix, *const_cast<bNodeSocket *>(corr.bottom));
  }
  if (section == PaintMaterialCorrectionSection::Content) {
    if (consumer != nullptr) {
      /* The accumulated coverage replaces whatever fed the mask base -- the layer's own map
       * alpha, or the over output of the correction below (spec 18 §4.1a). */
      relink_into(tree, *consumer, *consumer_owner, *over_combine, *over_out);
    }
    relink_into(tree, *top_socket, *layer.node, *mix, *mix_out);
  }
  else {
    relink_into(tree, *top_socket, *coverage_owner, *mix, *mix_out);
  }

  /* The correction hangs left of the row it corrects; a content section's over pair sits below
   * it, laid out the way the group fill lays a row's alpha pair out. */
  if (section == PaintMaterialCorrectionSection::Content) {
    bke::node_position_relative(*mix, *layer.node, mix_out, *top_socket);
    over_invert->location[0] = mix->location[0];
    over_invert->location[1] = mix->location[1] - 300.0f;
    over_combine->location[0] = mix->location[0] + 150.0f;
    over_combine->location[1] = mix->location[1] - 300.0f;
  }
  else {
    bke::node_position_relative(*mix, *coverage_owner, mix_out, *top_socket);
  }

  /* One identity for the correction's nodes, stamped the way a layer's Mix carries its marker;
   * the label stays empty -- the row is named by the UI model, not by the graph. */
  bke::paint_layer::marker_set(*mix, marker);
  bke::paint_layer::kind_set(*mix, PaintMaterialLayerKind::Correction);
  bke::paint_layer::correction_section_set(*mix, section);
  bke::paint_layer::correction_effect_set(*mix, effect);

  r_nodes.marker = marker;
  r_nodes.section = section;
  r_nodes.effect = effect;
  r_nodes.mix = mix;
  r_nodes.factor_multiply = multiply;
  r_nodes.over_invert = over_invert;
  r_nodes.over_combine = over_combine;
  /* Absent in this channel: no map and no image (spec 18 §4.1a). */
  r_nodes.map = nullptr;
  r_nodes.image = nullptr;
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Correction edits (spec 18 §4.1, §4.3, §6.1)
 *
 * The public half of this file: add, remove, reorder, rename and toggle a correction of one layer
 * row, in every channel at once, the way every other edit in this module works -- preflight, then
 * shape, then a mutation that cannot refuse.
 * \{ */

namespace {

/** The correction of \a section carrying \a marker in \a layer, or null when there is none. */
const ChainCorrection *correction_nodes_find(const ChainLayer &layer,
                                             const PaintMaterialCorrectionSection section,
                                             const bUUID &marker)
{
  const Vector<ChainCorrection> &rows = (section == PaintMaterialCorrectionSection::Content) ?
                                             layer.content_corrections :
                                             layer.mask_corrections;
  for (const ChainCorrection &nodes : rows) {
    if (BLI_uuid_equal(nodes.marker, marker)) {
      return &nodes;
    }
  }
  return nullptr;
}

/** The correction list of \a layer that \a section names. */
Vector<ChainCorrection> &correction_rows_of(ChainLayer &layer,
                                            const PaintMaterialCorrectionSection section)
{
  return (section == PaintMaterialCorrectionSection::Content) ? layer.content_corrections :
                                                                layer.mask_corrections;
}

/**
 * The row of the stack model that carries \a correction: its owner layer's ordinal, or -1 when no
 * correction anywhere in the stack does.
 *
 * The model is what the UI shows, so it is also what answers "which row is this correction on";
 * the mutators re-read the graph themselves, and the plan re-checks the channel agreement the
 * model presumes. The section, the position and the effect of the row are handed back through the
 * optional out-parameters, the same walk answering all three.
 */
int correction_owner_ordinal_find(const Main &bmain,
                                  const Material &ma,
                                  const bUUID &correction,
                                  PaintMaterialCorrectionSection *r_section,
                                  int *r_index,
                                  PaintMaterialCorrectionEffect *r_effect = nullptr)
{
  if (ma.nodetree != nullptr) {
    ma.nodetree->ensure_topology_cache();
  }
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, ma, entries)) {
    return -1;
  }
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    for (const int section_i : IndexRange(2)) {
      const auto section = PaintMaterialCorrectionSection(section_i);
      const Vector<PaintMaterialLayerCorrectionEntry> &rows =
          (section == PaintMaterialCorrectionSection::Content) ? entry.content_corrections :
                                                                 entry.mask_corrections;
      for (const int64_t i : rows.index_range()) {
        if (BLI_uuid_equal(rows[i].marker, correction)) {
          if (r_section != nullptr) {
            *r_section = section;
          }
          if (r_index != nullptr) {
            *r_index = int(i);
          }
          if (r_effect != nullptr) {
            *r_effect = rows[i].effect;
          }
          return entry.ordinal;
        }
      }
    }
  }
  return -1;
}

/** The first image of \a maps, in no particular order -- any role will do (spec 18 §6.1). */
Image *map_size_image_find(const Map<int, Image *> &maps)
{
  for (const auto &item : maps.items()) {
    if (item.value != nullptr) {
      return item.value;
    }
  }
  return nullptr;
}

/**
 * The preflight every edit of an existing correction shares: the row carrying \a correction, and
 * the plan for it, without writing a byte. A row carrying a correction already has the Mix a bare
 * base lacks, so the bottom conversion other edits run first never applies here.
 */
bool correction_plan_resolve(Main &bmain,
                             Material &ma,
                             const bUUID &correction,
                             LayerEditPlan &r_plan,
                             PaintMaterialCorrectionSection &r_section,
                             int &r_owner,
                             int *r_index,
                             PaintMaterialLayerEditError &r_error,
                             PaintMaterialCorrectionEffect *r_effect = nullptr)
{
  r_owner = correction_owner_ordinal_find(bmain, ma, correction, &r_section, r_index, r_effect);
  if (r_owner < 0) {
    r_error = PaintMaterialLayerEditError::CorrectionNotFound;
    return false;
  }
  return layer_edit_plan_build(
      bmain, ma, r_owner, LayerEditOp::CorrectionEdit, r_plan, r_error, int(r_section));
}

}  // namespace

bool BKE_paint_material_layer_correction_add(Main &bmain,
                                             Material &ma,
                                             const int layer_ordinal,
                                             const PaintMaterialCorrectionSection section,
                                             const PaintMaterialCorrectionEffect effect,
                                             const char *name,
                                             bUUID *r_correction,
                                             PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists, the material is writable, and a group row refuses a content
   * section -- all decided without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(
          bmain, ma, layer_ordinal, LayerEditOp::CorrectionEdit, plan, error, int(section)))
  {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * correction is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(
              bmain, ma, layer_ordinal, LayerEditOp::CorrectionEdit, plan, error, int(section)))
      {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  const bUUID marker = BLI_uuid_generate_random();

  struct WrappedCoverage {
    ChannelChain *chain = nullptr;
    bNode *multiply = nullptr;
    bNode *coverage_node = nullptr;
    bNodeSocket *coverage_socket = nullptr;
  };
  Vector<WrappedCoverage> wrapped;
  Vector<std::pair<ChannelChain *, ChainCorrection>> inserted;
  /* A creation that fails partway leaves the graph as it found it: the corrections already in are
   * closed over again, and a wrapped coverage goes back to the bare link it was. */
  auto rollback = [&](const PaintMaterialLayerEditError reason) {
    for (int64_t i = inserted.size() - 1; i >= 0; i--) {
      ChannelChain &chain = *inserted[i].first;
      correction_channel_remove(bmain, chain, chain.layers[plan.layer_index], inserted[i].second);
    }
    for (const WrappedCoverage &wrap : wrapped) {
      bNodeTree &tree = *wrap.chain->tree;
      tree.ensure_topology_cache();
      ChainLayer &layer = wrap.chain->layers[plan.layer_index];
      CompositeMixNode mix;
      if (layer.node != nullptr && composite_mix_node_read(*layer.node, mix) &&
          mix.factor != nullptr)
      {
        relink_into(tree,
                    *const_cast<bNodeSocket *>(mix.factor),
                    *layer.node,
                    *wrap.coverage_node,
                    *wrap.coverage_socket);
      }
      bke::node_remove_node(&bmain, tree, *wrap.multiply, true);
    }
    for (ChannelChain *chain : plan.chains) {
      BKE_ntree_update_after_single_tree_change(bmain, *chain->tree);
    }
    return fail(reason);
  };

  /* A group row's coverage is a bare link to the folder's Alpha -- the shape #GroupMake leaves. A
   * mask section hangs on the coverage input of a Multiply, so the link is wrapped in one first:
   * coverage by the folder's own alpha, opacity one, which renders exactly as before (spec 18
   * §4.3). */
  if (section == PaintMaterialCorrectionSection::Mask) {
    for (ChannelChain *chain : plan.chains) {
      chain->tree->ensure_topology_cache();
      ChainLayer &layer = chain->layers[plan.layer_index];
      CompositeMixNode mix;
      if (layer.node == nullptr || !composite_mix_node_read(*layer.node, mix) ||
          mix.factor == nullptr || mix.factor_coverage != nullptr)
      {
        continue;
      }
      bNodeLink *factor_link = sole_link_into(*const_cast<bNodeSocket *>(mix.factor));
      if (factor_link == nullptr) {
        continue;
      }
      /* Captured before the link goes away: the removal frees it. */
      bNode &coverage_node = *factor_link->fromnode;
      bNodeSocket &coverage_socket = *factor_link->fromsock;
      /* The bare link goes away first -- adding a link never replaces one: the Multiply takes its
       * place and reads the same alpha, so what the row covers is unchanged. */
      BKE_ntree_update_tag_link_removed(chain->tree);
      bke::node_remove_link(chain->tree, *factor_link);
      bNode *multiply = layer_factor_coverage_link(*chain->tree,
                                                   *layer.node,
                                                   *const_cast<bNodeSocket *>(mix.factor),
                                                   coverage_node,
                                                   coverage_socket,
                                                   1.0f);
      if (multiply == nullptr) {
        /* Put the bare link back before the rollback reads the rows wrapped so far. */
        bke::node_add_link(*chain->tree,
                           coverage_node,
                           coverage_socket,
                           *layer.node,
                           *const_cast<bNodeSocket *>(mix.factor));
        return rollback(PaintMaterialLayerEditError::CreationFailed);
      }
      wrapped.append({chain, multiply, &coverage_node, &coverage_socket});
    }
  }

  /* Every wired channel gets the correction, Mix channels first and Normal last: the Normal
   * channel's correction is a Normal Combine instance, and the tree is brought up to date before
   * that insert joins it, so it lands on a consistent graph. The nodes each insert built are kept:
   * the plan's chain rows were read before any of them existed. */
  ChannelChain *normal_chain = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      normal_chain = chain;
      continue;
    }
    ChainCorrection nodes;
    if (!correction_channel_insert(
            bmain, *chain, chain->layers[plan.layer_index], section, effect, marker, nodes))
    {
      return rollback(PaintMaterialLayerEditError::CreationFailed);
    }
    if (name != nullptr) {
      STRNCPY_UTF8(nodes.mix->label, name);
    }
    inserted.append({chain, nodes});
  }
  if (normal_chain != nullptr) {
    BKE_ntree_update_after_single_tree_change(bmain, *normal_chain->tree);
    ChainCorrection nodes;
    if (!correction_channel_insert(
            bmain, *normal_chain, normal_chain->layers[plan.layer_index], section, effect, marker, nodes))
    {
      return rollback(PaintMaterialLayerEditError::CreationFailed);
    }
    if (name != nullptr) {
      STRNCPY_UTF8(nodes.mix->label, name);
    }
    inserted.append({normal_chain, nodes});
  }

  if (section == PaintMaterialCorrectionSection::Mask) {
    /* A mask has no per-channel choice (spec 18 §4.3): one transparent map, shown by every
     * channel's correction from the start. */
    int width = 0;
    int height = 0;
    if (!BKE_paint_material_layer_map_size_get(bmain, ma, layer_ordinal, width, height) ||
        width <= 0 || height <= 0)
    {
      width = 1024;
      height = 1024;
    }
    char mask_name[MAX_ID_NAME - 2];
    SNPRINTF_UTF8(
        mask_name, "%s Mask", (name != nullptr && name[0] != '\0') ? name : "Correction");
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Image *mask = BKE_image_add_generated(&bmain,
                                         width,
                                         height,
                                         mask_name,
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         transparent,
                                         false,
                                         true,
                                         false);
    if (mask == nullptr) {
      return rollback(PaintMaterialLayerEditError::CreationFailed);
    }
    /* The same tags a layer's mask carries: the paint-canvas view of the ID browser and the
     * composite reader that assembles a mask channel from the maps themselves key off them. */
    mask->flag |= IMA_PAINT_CANVAS;
    mask->paint_layer_id = marker;
    mask->paint_layer_channel = PAINT_LAYER_MAP_MASK;

    bNodeTree &tree = *plan.chains.first()->tree;
    bNode *tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    bNodeSocket *tex_color = (tex != nullptr) ?
                                 bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr) :
                                 nullptr;
    bNodeSocket *tex_alpha = (tex != nullptr) ?
                                 bke::node_find_socket(*tex, SOCK_OUT, "Alpha"_ustr) :
                                 nullptr;
    if (tex == nullptr || tex_color == nullptr || tex_alpha == nullptr) {
      if (tex != nullptr) {
        bke::node_remove_node(&bmain, tree, *tex, true);
      }
      BKE_id_free(&bmain, mask);
      return rollback(PaintMaterialLayerEditError::CreationFailed);
    }
    tex->id = &mask->id;

    const ChainCorrection *first_nodes = nullptr;
    bNodeSocket *first_top = nullptr;
    for (const std::pair<ChannelChain *, ChainCorrection> &item : inserted) {
      const ChainCorrection &nodes = item.second;
      if (nodes.mix == nullptr || nodes.factor_multiply == nullptr) {
        continue;
      }
      /* Every correction the insert built lives in this tree, and a previous channel's links
       * invalidate the topology cache this read walks. */
      tree.ensure_topology_cache();
      CompositeMixNode corr;
      if (!composite_mix_node_read(*nodes.mix, corr) || corr.top == nullptr ||
          corr.factor_coverage == nullptr)
      {
        continue;
      }
      /* The map's Color is what the correction blends towards, its Alpha what covers: the same
       * reading the stack model and the compositor give a correction's map. */
      bke::node_add_link(
          tree, *tex, *tex_color, *nodes.mix, *const_cast<bNodeSocket *>(corr.top));
      bke::node_add_link(tree,
                         *tex,
                         *tex_alpha,
                         *nodes.factor_multiply,
                         *const_cast<bNodeSocket *>(corr.factor_coverage));
      if (first_nodes == nullptr) {
        first_nodes = &nodes;
        first_top = const_cast<bNodeSocket *>(corr.top);
      }
    }
    if (first_nodes != nullptr) {
      bke::node_position_relative(*tex, *first_nodes->mix, tex_color, *first_top);
    }
  }

  if (section == PaintMaterialCorrectionSection::Mask) {
    /* The shared map shows in every channel, the ones the row puts nothing into included: there
     * its mask chain takes the off form (spec 18 §4.3). Read again -- the plan's rows predate the
     * correction. */
    BKE_ntree_update_after_single_tree_change(bmain, *plan.chains.first()->tree);
    LayerEditPlan fresh;
    PaintMaterialLayerEditError fresh_error = PaintMaterialLayerEditError::None;
    if (layer_edit_plan_build(bmain,
                              ma,
                              layer_ordinal,
                              LayerEditOp::CorrectionEdit,
                              fresh,
                              fresh_error,
                              int(section)))
    {
      for (ChannelChain *chain : fresh.chains) {
        layer_mask_corrections_sync(*chain->tree, chain->layers[fresh.layer_index]);
      }
    }
  }

  if (r_correction != nullptr) {
    *r_correction = marker;
  }
  BKE_ntree_update_after_single_tree_change(bmain, *plan.chains.first()->tree);
  if (plan.chains.first()->tree != ma.nodetree) {
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  }
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

void correction_channel_remove(Main &bmain,
                               ChannelChain &chain,
                               ChainLayer & /*layer*/,
                               const ChainCorrection &nodes)
{
  bNodeTree &tree = *chain.tree;
  tree.ensure_topology_cache();

  CompositeMixNode corr;
  if (nodes.mix == nullptr || !composite_mix_node_read(*nodes.mix, corr) ||
      corr.bottom == nullptr)
  {
    /* Not the shape this module builds: removing it would guess at what moves where. */
    return;
  }
  bNodeSocket *mix_out = mix_output_find(*nodes.mix);

  /* What the correction blended over -- its base -- and, for a content section, what accumulated
   * coverage under it (`a_below`, read off the Subtract's second input). Both are carried over to
   * the consumers of the correction's own results, which is what closes the chain over it. */
  bNode *base_node = nullptr;
  bNodeSocket *base_socket = nullptr;
  if (bNodeLink *base_link = sole_link_into(*const_cast<bNodeSocket *>(corr.bottom))) {
    base_node = base_link->fromnode;
    base_socket = base_link->fromsock;
  }
  bNode *below_node = nullptr;
  bNodeSocket *below_socket = nullptr;
  if (nodes.over_invert != nullptr) {
    const bNodeSocket *invert_b = static_cast<const bNodeSocket *>(
        BLI_findlink(&nodes.over_invert->inputs, 1));
    if (invert_b != nullptr) {
      if (bNodeLink *below_link = sole_link_into(*const_cast<bNodeSocket *>(invert_b))) {
        below_node = below_link->fromnode;
        below_socket = below_link->fromsock;
      }
    }
  }

  /* The correction's map node goes with it when every link leaving the map lands on a node this
   * removal takes out anyway. Resolved before the first node is removed, since removal drops the
   * links this count reads. */
  bool map_is_sole_user = false;
  if (nodes.map != nullptr) {
    int outgoing = 0;
    int consumed = 0;
    for (const bNodeSocket *output : nodes.map->output_sockets()) {
      for (const bNodeLink *link : output->directly_linked_links()) {
        outgoing++;
        if (link->tonode == nodes.mix || link->tonode == nodes.factor_multiply ||
            link->tonode == nodes.over_invert || link->tonode == nodes.over_combine)
        {
          consumed++;
        }
      }
    }
    map_is_sole_user = outgoing > 0 && outgoing == consumed;
  }

  /* The consumers keep reading the same inputs from whatever was below the correction. The cache
   * is ensured per call: relinking one socket's feed invalidates it for the next. */
  auto redirect = [&](bNodeSocket &consumed, bNode *from_node, bNodeSocket *from_socket) {
    tree.ensure_topology_cache();
    for (bNodeLink *link : Vector<bNodeLink *>(consumed.directly_linked_links())) {
      if (from_node != nullptr && from_socket != nullptr) {
        relink_into(tree, *link->tosock, *link->tonode, *from_node, *from_socket);
      }
      else {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *link);
      }
    }
  };
  if (mix_out != nullptr) {
    redirect(*mix_out, base_node, base_socket);
  }
  if (nodes.section == PaintMaterialCorrectionSection::Content && nodes.over_combine != nullptr) {
    if (bNodeSocket *over_out = static_cast<bNodeSocket *>(nodes.over_combine->outputs.first)) {
      redirect(*over_out, below_node, below_socket);
    }
  }

  /* The nodes go after the relinks: removal detaches whatever links are left. */
  Vector<bNode *> to_remove;
  to_remove.append_non_duplicates(nodes.mix);
  to_remove.append_non_duplicates(nodes.factor_multiply);
  to_remove.append_non_duplicates(nodes.over_invert);
  to_remove.append_non_duplicates(nodes.over_combine);
  if (map_is_sole_user && nodes.map != nullptr) {
    to_remove.append_non_duplicates(nodes.map);
  }
  for (bNode *node : to_remove) {
    bke::node_remove_node(&bmain, tree, *node, true);
    BKE_ntree_update_tag_node_removed(&tree);
  }
}

bool BKE_paint_material_layer_correction_remove(Main &bmain,
                                                Material &ma,
                                                const bUUID &correction,
                                                PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the correction exists, on the row the plan resolves, in every channel. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    return fail(error);
  }
  for (ChannelChain *chain : plan.chains) {
    if (correction_nodes_find(chain->layers[plan.layer_index], section, correction) == nullptr) {
      /* The plan checked that the channels agree; a missing row here is a shape this module does
       * not rewrite, not something to half-remove. */
      return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
    }
  }
  /* 2. Mutation: from here, a refusal is impossible. */
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[plan.layer_index];
    Vector<ChainCorrection> &rows = correction_rows_of(layer, section);
    for (const int64_t i : rows.index_range()) {
      if (!BLI_uuid_equal(rows[i].marker, correction)) {
        continue;
      }
      const ChainCorrection nodes = rows[i];
      /* Out of the row's list before its nodes go: the mask sync below reads the list. */
      rows.remove(i);
      correction_channel_remove(bmain, *chain, layer, nodes);
      break;
    }
    /* A content correction that painted here may have been all the row put into the channel. */
    layer_mask_corrections_sync(*chain->tree, layer);
  }
  /* A channel with no graph of its own (AO) holds the correction's map by its tag and the user
   * its creation gave it: both go, so the map neither reads as the removed row's nor stays saved
   * with nothing using it. */
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.socket_name != nullptr) {
      continue;
    }
    if (Image *map = correction_tagged_map_find(bmain, correction, info.channel)) {
      map->paint_layer_id = bUUID{};
      map->paint_layer_channel = PAINT_LAYER_MAP_NONE;
      id_us_min(&map->id);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, *plan.chains.first()->tree);
  if (plan.chains.first()->tree != ma.nodetree) {
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  }
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_correction_reorder(Main &bmain,
                                                 Material &ma,
                                                 const bUUID &correction,
                                                 const int new_index,
                                                 PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the correction exists, the material is writable, the channels agree. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  int index = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, &index, error)) {
    return fail(error);
  }
  const int row_num = int(
      correction_rows_of(plan.chains.first()->layers[plan.layer_index], section).size());
  if (new_index < 0 || new_index >= row_num) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  if (new_index == index) {
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  }
  /* Every socket the rewiring below reads, checked in every channel before the first link moves:
   * a shape refused halfway through would leave the section's links half rebuilt. */
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[plan.layer_index];
    const Vector<ChainCorrection> &rows = correction_rows_of(layer, section);
    if (rows.size() != row_num || index >= rows.size()) {
      return fail(PaintMaterialLayerEditError::ChannelsDisagree);
    }
    chain->tree->ensure_topology_cache();
    for (const ChainCorrection &corr : rows) {
      CompositeMixNode mix;
      if (corr.mix == nullptr || !composite_mix_node_read(*corr.mix, mix) ||
          mix.bottom == nullptr || mix_output_find(*corr.mix) == nullptr)
      {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      if (section == PaintMaterialCorrectionSection::Content &&
          (corr.over_invert == nullptr || corr.over_combine == nullptr ||
           BLI_findlink(&corr.over_invert->inputs, 1) == nullptr ||
           BLI_findlink(&corr.over_combine->inputs, 2) == nullptr ||
           corr.over_combine->outputs.first == nullptr))
      {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
    }
    if (section == PaintMaterialCorrectionSection::Content) {
      if (layer.top == nullptr) {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
    }
    else {
      CompositeMixNode layer_mix;
      if (layer.node == nullptr || !composite_mix_node_read(*layer.node, layer_mix) ||
          layer_mix.factor_coverage == nullptr)
      {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
    }
  }
  /* 2. Mutation: from here, a refusal is impossible -- the checks above cover every socket the
   * loop reads. No node is created or removed; only the links between the section's corrections
   * are rebuilt, in the new order. */

  for (ChannelChain *chain : plan.chains) {
    bNodeTree &tree = *chain->tree;
    ChainLayer &layer = chain->layers[plan.layer_index];
    Vector<ChainCorrection> &rows = correction_rows_of(layer, section);
    if (index >= rows.size()) {
      return fail(PaintMaterialLayerEditError::ChannelsDisagree);
    }
    tree.ensure_topology_cache();

    /* The sources the section's bottom sat on belong to the section, not to the row that happened
     * to hold them: read them before the list is reordered. */
    bNode *section_base_node = nullptr;
    bNodeSocket *section_base_socket = nullptr;
    {
      CompositeMixNode mix;
      if (rows[0].mix == nullptr || !composite_mix_node_read(*rows[0].mix, mix) ||
          mix.bottom == nullptr)
      {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      if (bNodeLink *link = sole_link_into(*const_cast<bNodeSocket *>(mix.bottom))) {
        section_base_node = link->fromnode;
        section_base_socket = link->fromsock;
      }
    }
    bNode *section_below_node = nullptr;
    bNodeSocket *section_below_socket = nullptr;
    if (section == PaintMaterialCorrectionSection::Content) {
      const bNodeSocket *invert_b = (rows[0].over_invert != nullptr) ?
                                        static_cast<const bNodeSocket *>(
                                            BLI_findlink(&rows[0].over_invert->inputs, 1)) :
                                        nullptr;
      if (invert_b != nullptr) {
        if (bNodeLink *link = sole_link_into(*const_cast<bNodeSocket *>(invert_b))) {
          section_below_node = link->fromnode;
          section_below_socket = link->fromsock;
        }
      }
    }

    ChainCorrection moved = rows[index];
    rows.remove(index);
    rows.insert(new_index, moved);

    /* Drop every feed of \a into, then take it from \a from_node when there is one -- the same
     * rewrite #relink_into does, with "nothing below" as a state of its own. */
    auto relink_or_clear = [&](bNodeSocket &into,
                               bNode &into_node,
                               bNode *from_node,
                               bNodeSocket *from_socket) {
      tree.ensure_topology_cache();
      for (bNodeLink *link : Vector<bNodeLink *>(into.directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *link);
      }
      if (from_node != nullptr && from_socket != nullptr) {
        bNodeLink &added = bke::node_add_link(tree, *from_node, *from_socket, into_node, into);
        BKE_ntree_update_tag_link_added(&tree, &added);
      }
    };

    for (const int64_t i : rows.index_range()) {
      ChainCorrection &corr = rows[i];
      CompositeMixNode mix;
      tree.ensure_topology_cache();
      if (corr.mix == nullptr || !composite_mix_node_read(*corr.mix, mix) ||
          mix.bottom == nullptr)
      {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      bNodeSocket *bottom = const_cast<bNodeSocket *>(mix.bottom);
      if (i > 0) {
        ChainCorrection &below = rows[i - 1];
        bNodeSocket *below_out = mix_output_find(*below.mix);
        if (below_out == nullptr) {
          return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
        }
        relink_or_clear(*bottom, *corr.mix, below.mix, below_out);
        if (section != PaintMaterialCorrectionSection::Content) {
          continue;
        }
        /* The Over chain follows the rows: what accumulated below this one is what the one under
         * it accumulates now. */
        const bNodeSocket *invert_b = (corr.over_invert != nullptr) ?
                                          static_cast<const bNodeSocket *>(
                                              BLI_findlink(&corr.over_invert->inputs, 1)) :
                                          nullptr;
        const bNodeSocket *combine_c = (corr.over_combine != nullptr) ?
                                           static_cast<const bNodeSocket *>(
                                               BLI_findlink(&corr.over_combine->inputs, 2)) :
                                           nullptr;
        bNodeSocket *below_over_out = (below.over_combine != nullptr) ?
                                          static_cast<bNodeSocket *>(
                                              below.over_combine->outputs.first) :
                                          nullptr;
        if (invert_b == nullptr || combine_c == nullptr || below_over_out == nullptr) {
          return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
        }
        relink_or_clear(*const_cast<bNodeSocket *>(invert_b),
                        *corr.over_invert,
                        below.over_combine,
                        below_over_out);
        relink_or_clear(*const_cast<bNodeSocket *>(combine_c),
                        *corr.over_combine,
                        below.over_combine,
                        below_over_out);
        continue;
      }
      /* The bottom of the section keeps what the whole stack of corrections sits on. */
      relink_or_clear(*bottom, *corr.mix, section_base_node, section_base_socket);
      if (section != PaintMaterialCorrectionSection::Content) {
        continue;
      }
      const bNodeSocket *invert_b = (corr.over_invert != nullptr) ?
                                        static_cast<const bNodeSocket *>(
                                            BLI_findlink(&corr.over_invert->inputs, 1)) :
                                        nullptr;
      const bNodeSocket *combine_c = (corr.over_combine != nullptr) ?
                                         static_cast<const bNodeSocket *>(
                                             BLI_findlink(&corr.over_combine->inputs, 2)) :
                                         nullptr;
      if (invert_b == nullptr || combine_c == nullptr) {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      relink_or_clear(*const_cast<bNodeSocket *>(invert_b),
                      *corr.over_invert,
                      section_below_node,
                      section_below_socket);
      relink_or_clear(*const_cast<bNodeSocket *>(combine_c),
                      *corr.over_combine,
                      section_below_node,
                      section_below_socket);
    }

    /* The topmost row of the section is the one the layer reads: its Mix feeds the layer's content
     * or coverage input, and its accumulated coverage goes where the insert would have put it. */
    ChainCorrection &top = rows.last();
    bNodeSocket *top_out = mix_output_find(*top.mix);
    if (top_out == nullptr) {
      return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
    }
    if (section == PaintMaterialCorrectionSection::Content) {
      if (layer.top == nullptr) {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      relink_or_clear(*layer.top, *layer.node, top.mix, top_out);
      tree.ensure_topology_cache();
      bNodeSocket *consumer = layer_mask_base_consumer(layer);
      if (consumer == nullptr) {
        continue;
      }
      bNode &consumer_owner = consumer->owner_node();
      bNodeSocket *top_over_out = (top.over_combine != nullptr) ?
                                      static_cast<bNodeSocket *>(top.over_combine->outputs.first) :
                                      nullptr;
      if (top_over_out == nullptr) {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      relink_or_clear(*consumer, consumer_owner, top.over_combine, top_over_out);
    }
    else {
      tree.ensure_topology_cache();
      CompositeMixNode layer_mix;
      if (!composite_mix_node_read(*layer.node, layer_mix) ||
          layer_mix.factor_coverage == nullptr)
      {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      bNodeSocket &coverage = const_cast<bNodeSocket &>(*layer_mix.factor_coverage);
      bNode &coverage_owner = coverage.owner_node();
      relink_or_clear(coverage, coverage_owner, top.mix, top_out);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, *plan.chains.first()->tree);
  if (plan.chains.first()->tree != ma.nodetree) {
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  }
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_correction_set_enabled(Main &bmain,
                                                     Material &ma,
                                                     const bUUID &correction,
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

  /* 1. Preflight: the correction exists, on the row the plan resolves, in every channel. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    return fail(error);
  }
  for (ChannelChain *chain : plan.chains) {
    const ChainCorrection *nodes = correction_nodes_find(
        chain->layers[plan.layer_index], section, correction);
    if (nodes == nullptr || nodes->mix == nullptr) {
      return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
    }
  }
  /* 2. Mutation: from here, a refusal is impossible. The row switches as a whole (spec 18 §4.1):
   * its Mix and maps mute and its over pair closes its gate (#correction_row_enabled_apply). */
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[plan.layer_index];
    correction_row_enabled_apply(
        *chain->tree, *correction_nodes_find(layer, section, correction), enable);
    if (section == PaintMaterialCorrectionSection::Content) {
      /* A content row going on or off can start or end what the row puts into the channel. */
      layer_mask_corrections_sync(*chain->tree, layer);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, *plan.chains.first()->tree);
  if (plan.chains.first()->tree != ma.nodetree) {
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  }
  /* The Shading component is NO_COW_TAG_ON_UPDATE: #ID_RECALC_SHADING alone never re-copies the
   * evaluated material, so the mute flag GPU compilation reads would stay on whatever it was at
   * the last relation sync -- the same reason a layer's toggle commits with relations. */
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_correction_opacity_set(Main &bmain,
                                                     Material &ma,
                                                     const bUUID &correction,
                                                     const float opacity,
                                                     PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the correction exists, on the row the plan resolves, and every channel's own
   * coverage-multiply constant is found, before a byte is written. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    return fail(error);
  }
  Vector<bNodeSocket *> targets;
  for (ChannelChain *chain : plan.chains) {
    const ChainCorrection *nodes = correction_nodes_find(
        chain->layers[plan.layer_index], section, correction);
    CompositeMixNode mix;
    if (nodes == nullptr || nodes->mix == nullptr ||
        !composite_mix_node_read(*nodes->mix, mix) || mix.factor_opacity == nullptr)
    {
      return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
    }
    targets.append(const_cast<bNodeSocket *>(mix.factor_opacity));
  }

  /* 2. Mutation: from here, a refusal is impossible. Every channel's copy of the correction takes
   * the same strength, the way #BKE_paint_material_layer_correction_set_enabled keeps Enabled
   * shared; only the correction's per-channel Blending Mode stays independent. */
  for (const int i : plan.chains.index_range()) {
    bNodeSocket &socket = *targets[i];
    socket.default_value_typed<bNodeSocketValueFloat>()->value = opacity;
    BKE_ntree_update_tag_socket_property(plan.chains[i]->tree, &socket);
  }

  BKE_ntree_update_after_single_tree_change(bmain, *plan.chains.first()->tree);
  if (plan.chains.first()->tree != ma.nodetree) {
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  }
  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_correction_rename(Main &bmain,
                                                Material &ma,
                                                const bUUID &correction,
                                                const StringRef name,
                                                PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the correction exists, on the row the plan resolves. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    return fail(error);
  }
  /* 2. Mutation: the name a user sees is the label of the correction's Mix nodes, set in every
   * channel at once. Copied first: the reference may point into a node label being rewritten. */
  const std::string name_copy(name);
  for (ChannelChain *chain : plan.chains) {
    const ChainCorrection *nodes = correction_nodes_find(
        chain->layers[plan.layer_index], section, correction);
    if (nodes == nullptr || nodes->mix == nullptr) {
      return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
    }
    STRNCPY_UTF8(nodes->mix->label, name_copy.c_str());
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

int BKE_paint_material_layer_correction_owner_ordinal(
    Main &bmain,
    Material &ma,
    const bUUID &correction,
    PaintMaterialCorrectionSection *r_section,
    int *r_index)
{
  return correction_owner_ordinal_find(bmain, ma, correction, r_section, r_index);
}

bool BKE_paint_material_layer_map_size_get(Main &bmain,
                                           Material &ma,
                                           const int layer_ordinal,
                                           int &r_width,
                                           int &r_height)
{
  /* The stack model reads links; a caller that just built a node has left the cache stale. */
  if (ma.nodetree != nullptr) {
    ma.nodetree->ensure_topology_cache();
  }
  const PaintMaterialLayerStackEntry *entry = nullptr;
  Vector<PaintMaterialLayerStackEntry> entries;
  if (BKE_paint_material_layer_stack_from_material(bmain, ma, entries)) {
    for (const PaintMaterialLayerStackEntry &candidate : entries) {
      if (candidate.ordinal == layer_ordinal) {
        entry = &candidate;
        break;
      }
    }
  }
  if (entry != nullptr) {
    /* The layer's own maps first, any role; then the maps its corrections show (spec 18 §6.1). */
    if (Image *image = map_size_image_find(entry->channel_images)) {
      BKE_image_get_size(image, nullptr, &r_width, &r_height);
      return true;
    }
    for (const Vector<PaintMaterialLayerCorrectionEntry> *rows :
         {&entry->content_corrections, &entry->mask_corrections})
    {
      for (const PaintMaterialLayerCorrectionEntry &correction : *rows) {
        if (Image *image = map_size_image_find(correction.channel_images)) {
          BKE_image_get_size(image, nullptr, &r_width, &r_height);
          return true;
        }
      }
    }
  }
  /* The stack's own dimensions: what the Base Color channel composites at is the size a new map
   * has to fit. */
  Vector<PaintMaterialCompositeImageLayer> image_layers;
  if (!BKE_paint_material_composite_stack_from_material(
          bmain, ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image_layers))
  {
    return false;
  }
  return BKE_paint_material_composite_stack_dimensions(image_layers, r_width, r_height);
}

bool row_channel_painted_by_corrections(const ChainLayer &layer)
{
  /* Content corrections only: a mask correction shapes the coverage the row already has and
   * brings no pixels of its own, so it cannot keep a switched-off channel painting. */
  for (const ChainCorrection &nodes : layer.content_corrections) {
    if (nodes.image == nullptr || nodes.mix == nullptr || nodes.map == nullptr) {
      continue;
    }
    if ((nodes.mix->flag & NODE_MUTED) != 0 || (nodes.map->flag & NODE_MUTED) != 0) {
      continue;
    }
    return true;
  }
  return false;
}

PaintMaterialLayerChannelState BKE_paint_material_layer_correction_channel_state_get(
    Main &bmain, Material &ma, const bUUID &correction, const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return PaintMaterialLayerChannelState::Absent;
  }
  /* The owner row first: a marker nothing carries reads as Absent, like every other refusal of
   * this getter. Its section tells the correction's lists apart. */
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  if (correction_owner_ordinal_find(bmain, ma, correction, &section, nullptr) < 0) {
    return PaintMaterialLayerChannelState::Absent;
  }
  if (BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).socket_name == nullptr) {
    /* A channel with no graph of its own (AO): the tagged map is the whole state. */
    return (correction_tagged_map_find(bmain, correction, channel) != nullptr) ?
               PaintMaterialLayerChannelState::Enabled :
               PaintMaterialLayerChannelState::Absent;
  }
  /* The graph channels read their own nodes: the correction's Mix in the chain of the channel
   * asked for, through the one rule every state reader shares. A forest the reader refuses, a
   * row without the correction, and a shape it cannot read are all Absent. The marker is what
   * says which row is the correction's, so the walk needs no ordinal to agree on. */
  Vector<Vector<ChannelChain>> per_channel;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  if (!chains_collect_forest(ma, per_channel, error)) {
    return PaintMaterialLayerChannelState::Absent;
  }
  for (const Vector<ChannelChain> &forest : per_channel) {
    if (forest.is_empty() || forest.last().channel != channel) {
      continue;
    }
    for (const ChannelChain &chain : forest) {
      for (const ChainLayer &layer : chain.layers) {
        const ChainCorrection *nodes = correction_nodes_find(layer, section, correction);
        if (nodes == nullptr || nodes->mix == nullptr) {
          continue;
        }
        chain.tree->ensure_topology_cache();
        CompositeMixNode corr;
        PaintMaterialLayerChannelState state = PaintMaterialLayerChannelState::Absent;
        if (!composite_mix_node_read(*nodes->mix, corr) ||
            !composite_mix_channel_state_get(corr, state))
        {
          return PaintMaterialLayerChannelState::Absent;
        }
        return state;
      }
    }
  }
  return PaintMaterialLayerChannelState::Absent;
}

bool BKE_paint_material_layer_correction_channel_enabled_set(Main &bmain,
                                                            Material &ma,
                                                            const bUUID &correction,
                                                            const int channel,
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
  auto succeed = [&]() {
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  };

  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));

  /* 1. Preflight: the correction exists, on the row its plan resolves. */
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  PaintMaterialCorrectionEffect effect = PaintMaterialCorrectionEffect::Paint;
  const int owner = correction_owner_ordinal_find(
      bmain, ma, correction, &section, nullptr, &effect);
  if (owner < 0) {
    return fail(PaintMaterialLayerEditError::CorrectionNotFound);
  }
  if (section == PaintMaterialCorrectionSection::Mask) {
    if (effect == PaintMaterialCorrectionEffect::Paint) {
      /* A painted mask has no per-channel choice (spec 18 §4.3): its form in each channel follows
       * whether the row puts anything there (#layer_mask_corrections_sync), not a switch of its
       * own. */
      return fail(PaintMaterialLayerEditError::CorrectionSectionMismatch);
    }
    /* A Fill mask picks its one grayscale channel here -- the first user-picked channel a mask
     * correction has (spec 18 §4.3). The single-channel form is the invariant: a second channel is
     * refused while this correction is wired on another one, before the target channel's nodes are
     * touched. What the pick makes of the mask's own per-channel form stays
     * #layer_mask_corrections_sync's to say. */
    if (enable) {
      for (const int other : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
        if (other == channel) {
          continue;
        }
        if (BKE_paint_material_layer_correction_channel_state_get(bmain, ma, correction, other) !=
            PaintMaterialLayerChannelState::Absent)
        {
          return fail(PaintMaterialLayerEditError::CorrectionSectionMismatch);
        }
      }
    }
  }

  /* A channel with no graph of its own (AO) has no nodes to build and no Disabled form to keep
   * a map in: the tagged map is the whole state. Switching off untags -- the pixels stay in the
   * data-block, left to the usual user-count rules -- which is what makes the channel read
   * Absent again. */
  if (info.socket_name == nullptr) {
    Image *map = correction_tagged_map_find(bmain, correction, channel);
    if ((map != nullptr) == enable) {
      return succeed();
    }
    if (enable) {
      int width = 0;
      int height = 0;
      if (!BKE_paint_material_layer_map_size_get(bmain, ma, owner, width, height) || width <= 0 ||
          height <= 0)
      {
        width = 1024;
        height = 1024;
      }
      char map_name[MAX_ID_NAME - 2];
      SNPRINTF_UTF8(map_name, "%s Correction", info.ui_name);
      const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      map = BKE_image_add_generated(&bmain,
                                    width,
                                    height,
                                    map_name,
                                    32,
                                    false,
                                    IMA_GENTYPE_BLANK,
                                    transparent,
                                    false,
                                    !info.is_color,
                                    false);
      if (map == nullptr) {
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      /* The same tags a layer's map carries: the paint-canvas view of the ID browser and the
       * stack model that assembles a correction's maps key off them. */
      map->flag |= IMA_PAINT_CANVAS;
      map->paint_layer_id = correction;
      map->paint_layer_channel = channel;
    }
    else {
      /* The map's only user is the one its creation gave it -- no node holds it -- so that user
       * goes with the tag: the map is left orphaned, recoverable by undo, rather than saved
       * forever under a tag nothing reads. */
      map->paint_layer_id = bUUID{};
      map->paint_layer_channel = PAINT_LAYER_MAP_NONE;
      id_us_min(&map->id);
    }
    paint_layer_edit_committed(bmain, ma, true);
    return succeed();
  }

  /* 2. The plan, the way every correction edit resolves its row. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(
          bmain, ma, owner, LayerEditOp::CorrectionEdit, plan, error, int(section)))
  {
    return fail(error);
  }
  /* 3. Shape: the conversion changes the chains, so the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(
              bmain, ma, owner, LayerEditOp::CorrectionEdit, plan, error, int(section)))
      {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }

  /* 4. The channel's chain. One the material has yet to wire gets it first -- corrections
   * included, the ensure sees to that -- and the plan is read again, since the topology changed
   * under it. */
  ChannelChain *target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  if (target == nullptr) {
    const int one[1] = {channel};
    if (!BKE_paint_material_layer_channels_ensure(bmain, ma, Span<int>(one, 1), &error)) {
      return fail(error);
    }
    if (!layer_edit_plan_build(
            bmain, ma, owner, LayerEditOp::CorrectionEdit, plan, error, int(section)))
    {
      BLI_assert_unreachable();
      return fail(error);
    }
    for (ChannelChain *chain : plan.chains) {
      if (chain->channel == channel) {
        target = chain;
      }
    }
    if (target == nullptr) {
      BLI_assert_unreachable();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
  }

  /* 5. Mutate: the target channel's own nodes are what carry the correction. */
  bNodeTree &tree = *target->tree;
  tree.ensure_topology_cache();
  ChainLayer &layer = target->layers[plan.layer_index];
  Vector<ChainCorrection> &rows = (section == PaintMaterialCorrectionSection::Content) ?
                                      layer.content_corrections :
                                      layer.mask_corrections;
  ChainCorrection *nodes = nullptr;
  for (ChainCorrection &candidate : rows) {
    if (BLI_uuid_equal(candidate.marker, correction)) {
      nodes = &candidate;
      break;
    }
  }
  if (nodes == nullptr || nodes->mix == nullptr) {
    return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
  }
  CompositeMixNode corr;
  if (!composite_mix_node_read(*nodes->mix, corr)) {
    return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
  }
  PaintMaterialLayerChannelState state = PaintMaterialLayerChannelState::Absent;
  if (!composite_mix_channel_state_get(corr, state)) {
    return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
  }
  if ((state == PaintMaterialLayerChannelState::Enabled) == enable) {
    return succeed();
  }

  if (state == PaintMaterialLayerChannelState::Absent) {
    /* 6. Absent -> Enabled: the map and its node, sized and color-managed like the base's own
     * would be, transparent -- a correction that shows nothing contributes nothing until it is
     * painted. */
    int width = 0;
    int height = 0;
    if (!BKE_paint_material_layer_map_size_get(bmain, ma, owner, width, height) || width <= 0 ||
        height <= 0)
    {
      width = 1024;
      height = 1024;
    }
    char map_name[MAX_ID_NAME - 2];
    SNPRINTF_UTF8(map_name, "%s Correction", info.ui_name);
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    /* A mask's map is grayscale, non-color data, whatever channel it was reached through. */
    const bool is_data = (section == PaintMaterialCorrectionSection::Mask) ? true : !info.is_color;
    Image *map = BKE_image_add_generated(&bmain,
                                         width,
                                         height,
                                         map_name,
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         transparent,
                                         false,
                                         is_data,
                                         false);
    if (map == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    map->flag |= IMA_PAINT_CANVAS;
    map->paint_layer_id = correction;
    /* A mask's own map carries the mask role rather than the channel's: the one map every
     * channel's correction reads is found by it (spec 18 §4.3). */
    map->paint_layer_channel = (section == PaintMaterialCorrectionSection::Mask) ?
                                   PAINT_LAYER_MAP_MASK :
                                   channel;
    bNode *map_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    bNodeSocket *map_color = (map_node != nullptr) ?
                                 bke::node_find_socket(*map_node, SOCK_OUT, "Color"_ustr) :
                                 nullptr;
    bNodeSocket *map_alpha = (map_node != nullptr) ?
                                 bke::node_find_socket(*map_node, SOCK_OUT, "Alpha"_ustr) :
                                 nullptr;
    if (map_node == nullptr || map_color == nullptr || map_alpha == nullptr ||
        corr.top == nullptr || corr.factor_coverage == nullptr ||
        nodes->factor_multiply == nullptr)
    {
      if (map_node != nullptr) {
        bke::node_remove_node(&bmain, tree, *map_node, false);
      }
      BKE_id_free(&bmain, map);
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    /* The image's fresh user becomes the node's, the way a layer's map hands its own over. */
    map_node->id = &map->id;
    /* The map's Color is what the correction blends towards, its Alpha what covers: the
     * coverage input of the correction's own Multiply. */
    bke::node_add_link(
        tree, *map_node, *map_color, *nodes->mix, *const_cast<bNodeSocket *>(corr.top));
    bke::node_position_relative(
        *map_node, *nodes->mix, map_color, *const_cast<bNodeSocket *>(corr.top));
    bke::node_add_link(tree,
                       *map_node,
                       *map_alpha,
                       *nodes->factor_multiply,
                       *const_cast<bNodeSocket *>(corr.factor_coverage));
    /* The chain's row was read before the map existed; the I2' test below reads the same
     * entry, so it learns the map here. */
    nodes->map = map_node;
    nodes->image = map;
    if ((nodes->mix->flag & NODE_MUTED) != 0) {
      /* The row is switched off: its maps stay muted with it (#correction_row_enabled_apply). */
      channel_map_mute_set(tree, *map_node, false);
    }
  }
  else if (nodes->map == nullptr) {
    return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
  }
  else {
    /* 7. Enabled <-> Disabled: the map stays, the coverage moves -- the same Multiply-form
     * toggle a layer row's channel goes through. Only a content section gets here, whose map is
     * this channel's own node. */
    const bool map_is_per_channel = (section == PaintMaterialCorrectionSection::Content);
    if (enable) {
      if (map_is_per_channel && (nodes->mix->flag & NODE_MUTED) == 0) {
        /* A switched-off row keeps its maps muted until the row itself comes back on. */
        channel_map_mute_set(tree, *nodes->map, true);
      }
      bNodeSocket *map_alpha = bke::node_find_socket(*nodes->map, SOCK_OUT, "Alpha"_ustr);
      if (map_alpha == nullptr || corr.factor_coverage == nullptr) {
        return fail(PaintMaterialLayerEditError::CorrectionChainNotPlain);
      }
      bNodeSocket &coverage = const_cast<bNodeSocket &>(*corr.factor_coverage);
      relink_into(tree, coverage, *nodes->factor_multiply, *nodes->map, *map_alpha);
    }
    else {
      /* Coverage off in this channel (I1): no link, and an explicit zero rather than Math's
       * 0.5. The muted map only spares the sampler. */
      bNodeSocket &coverage = const_cast<bNodeSocket &>(*corr.factor_coverage);
      for (bNodeLink *link : Vector<bNodeLink *>(coverage.directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *link);
      }
      coverage.default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
      BKE_ntree_update_tag_socket_property(&tree, &coverage);
      if (map_is_per_channel) {
        channel_map_mute_set(tree, *nodes->map, false);
      }
    }
  }

  /* 8. Spec 18 I2': the row keeps painting through its content corrections when its base is
   * Absent or Disabled here -- but only while the coverage they accumulate reaches the row's
   * own Multiply. Enabling a channel relinks it in where nothing owns the input; disabling
   * takes the link back out once no correction contributes anymore. A mask owns the coverage
   * input and is never touched: a mask correction keeps the feed, and a mask image makes the
   * consumer null. */
  tree.ensure_topology_cache();
  CompositeMixNode row_mix;
  if (section == PaintMaterialCorrectionSection::Content && layer.node != nullptr &&
      !layer.content_corrections.is_empty() && composite_mix_node_read(*layer.node, row_mix) &&
      row_mix.factor_coverage != nullptr)
  {
    bNodeSocket &coverage = const_cast<bNodeSocket &>(*row_mix.factor_coverage);
    if (enable) {
      if (!socket_has_link(coverage) && layer.mask_corrections.is_empty() &&
          row_channel_painted_by_corrections(layer))
      {
        const ChainCorrection &top = layer.content_corrections.last();
        bNodeSocket *over_out = (top.over_combine != nullptr) ?
                                    static_cast<bNodeSocket *>(top.over_combine->outputs.first) :
                                    nullptr;
        bNodeSocket *consumer = layer_mask_base_consumer(layer);
        if (over_out != nullptr && consumer != nullptr) {
          relink_into(tree, *consumer, consumer->owner_node(), *top.over_combine, *over_out);
        }
      }
    }
    else if (!row_channel_painted_by_corrections(layer)) {
      PaintMaterialLayerChannelState base_state = PaintMaterialLayerChannelState::Absent;
      if (composite_layer_base_state_get(*layer.node, base_state) &&
          base_state != PaintMaterialLayerChannelState::Enabled)
      {
        if (bNodeLink *feed = sole_link_into(coverage)) {
          bool from_over = false;
          for (const ChainCorrection &candidate : layer.content_corrections) {
            if (candidate.over_combine == feed->fromnode) {
              from_over = true;
              break;
            }
          }
          if (from_over) {
            BKE_ntree_update_tag_link_removed(&tree);
            bke::node_remove_link(&tree, *feed);
            coverage.default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
            BKE_ntree_update_tag_socket_property(&tree, &coverage);
          }
        }
      }
    }
  }

  /* What the row puts into this channel may have just started or ended: its mask corrections
   * follow (spec 18 §4.3). */
  layer_mask_corrections_sync(tree, layer);

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  if (&tree != ma.nodetree) {
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  }
  paint_layer_edit_committed(bmain, ma, true);
  return succeed();
}

/* -------------------------------------------------------------------- */
/** \name Correction copy and scale (spec 18 §4.5, D8)
 *
 * The copy is the one correction edit that reaches past one material: it reads the source's row
 * through the stack model, writes the target's through the same mutators every other edit here
 * uses, and deep-copies the maps the way a duplicate deep-copies a layer's. The scale is the tail
 * a resize owes its corrections: a layer moved to a new resolution takes their maps along.
 * \{ */

namespace {

/** The ramp blend a #CompositeBlend is written through, or -1 when no Mix blend carries it. */
int blend_ramp_for(const CompositeBlend blend)
{
  switch (blend) {
    case CompositeBlend::Mix:
      return MA_RAMP_BLEND;
    case CompositeBlend::Multiply:
      return MA_RAMP_MULT;
    case CompositeBlend::Overlay:
      return MA_RAMP_OVERLAY;
    case CompositeBlend::Add:
      return MA_RAMP_ADD;
    default:
      /* A Normal Combine has no Mix blend to carry: its channel's nodes carry no blend_type. */
      return -1;
  }
}

/** The correction row carrying \a marker in \a entries, with its section, or null. */
PaintMaterialLayerCorrectionEntry *correction_model_row_find(
    Vector<PaintMaterialLayerStackEntry> &entries,
    const bUUID &marker,
    PaintMaterialCorrectionSection *r_section)
{
  for (PaintMaterialLayerStackEntry &entry : entries) {
    for (const int section_i : IndexRange(2)) {
      const auto section = PaintMaterialCorrectionSection(section_i);
      Vector<PaintMaterialLayerCorrectionEntry> &rows =
          (section == PaintMaterialCorrectionSection::Content) ? entry.content_corrections :
                                                                entry.mask_corrections;
      for (PaintMaterialLayerCorrectionEntry &correction : rows) {
        if (BLI_uuid_equal(correction.marker, marker)) {
          if (r_section != nullptr) {
            *r_section = section;
          }
          return &correction;
        }
      }
    }
  }
  return nullptr;
}

/**
 * Deep-copy \a source_image over the map of role \a role that the correction \a marker shows in
 * \a target -- the transparent one its add or enable just created -- re-tagged and scaled to the
 * size that map has. The nodes showing the old map are pointed at the copy; a map with no node of
 * its own (a channelless channel's) is replaced by tag alone.
 */
bool correction_map_copy_onto(Main &bmain,
                              Material &target,
                              const bUUID &marker,
                              const PaintMaterialCorrectionSection section,
                              const int role,
                              Image &source_image,
                              PaintMaterialCorrectionCopyReport *r_report,
                              PaintMaterialLayerEditError &r_error)
{
  /* The map the add or enable just created: its size is what the target's maps have, and its one
   * user -- the node showing it, or nobody for a channelless channel -- is what the copy takes
   * over. */
  Image *fresh = correction_tagged_map_find(bmain, marker, role);
  if (fresh == nullptr) {
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }

  /* The nodes showing it, with their trees: found through the corrections' own map inputs, which
   * is where a map node sits whether the section hangs on content or on coverage. */
  Map<bNode *, bNodeTree *> showing;
  Vector<Vector<ChannelChain>> per_channel;
  PaintMaterialLayerEditError read_error = PaintMaterialLayerEditError::None;
  if (!chains_collect_forest(target, per_channel, read_error)) {
    /* The graph the target's own edits just built does not read back: replacing the map would
     * guess at where it shows, so the copy stops here and the caller's undo takes it back. */
    r_error = PaintMaterialLayerEditError::CorrectionChainNotPlain;
    return false;
  }
  for (const Vector<ChannelChain> &forest : per_channel) {
    for (const ChannelChain &chain : forest) {
      for (const ChainLayer &layer : chain.layers) {
        const ChainCorrection *nodes = correction_nodes_find(layer, section, marker);
        if (nodes == nullptr || nodes->mix == nullptr) {
          continue;
        }
        chain.tree->ensure_topology_cache();
        CompositeMixNode corr;
        if (!composite_mix_node_read(*nodes->mix, corr) || corr.top == nullptr) {
          continue;
        }
        bNodeLink *feed = sole_link_into(*const_cast<bNodeSocket *>(corr.top));
        if (feed == nullptr || feed->fromnode->type_legacy != SH_NODE_TEX_IMAGE ||
            feed->fromnode->id != &fresh->id)
        {
          continue;
        }
        /* One shared node per correction in every channel (a mask's especially): point at it
         * once, whatever number of channels read it. */
        showing.add_overwrite(feed->fromnode, chain.tree);
      }
    }
  }

  Image *copy = id_cast<Image *>(BKE_id_copy(&bmain, &source_image.id));
  if (copy == nullptr) {
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }
  /* The same tags the target's own maps carry: the paint-canvas view and the stack model key off
   * them. */
  copy->paint_layer_id = marker;
  copy->paint_layer_channel = role;

  int fresh_width = 0;
  int fresh_height = 0;
  BKE_image_get_size(fresh, nullptr, &fresh_width, &fresh_height);
  int copy_width = 0;
  int copy_height = 0;
  BKE_image_get_size(copy, nullptr, &copy_width, &copy_height);
  if (fresh_width > 0 && fresh_height > 0 &&
      (copy_width != fresh_width || copy_height != fresh_height))
  {
    /* A copy between materials arrives at the source's resolution; the target's is what its own
     * maps -- and its correction rows -- are sized to (spec 18 D8). */
    if (BKE_image_scale(copy, fresh_width, fresh_height, nullptr)) {
      if (r_report != nullptr) {
        r_report->scaled_maps++;
      }
    }
  }

  for (const auto &item : showing.items()) {
    /* The node held the fresh map's one user; the copy's own creation user takes its place, the
     * way every map node in this module takes the image it shows. */
    id_us_min(&fresh->id);
    item.key->id = &copy->id;
    BKE_ntree_update_after_single_tree_change(bmain, *item.value);
  }
  if (showing.is_empty()) {
    /* Nobody shows the fresh map: a channelless channel's map is held by nothing but its tag, so
     * its creation user is given back here rather than by a node. */
    id_us_min(&fresh->id);
  }
  if (fresh->id.us == 0) {
    BKE_id_free(&bmain, fresh);
  }
  return true;
}

}  // namespace

bool BKE_paint_material_layer_corrections_copy(Main &bmain,
                                               Span<PaintMaterialCorrectionRef> sources,
                                               Material &target,
                                               const int target_layer_ordinal,
                                               Vector<bUUID> &r_created,
                                               PaintMaterialCorrectionCopyReport *r_report,
                                               PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  bool wrote = false;
  for (const PaintMaterialCorrectionRef &ref : sources) {
    /* 1. The source: the material by session identity, then its model row for the correction.
     * Both misses skip the ref -- a caller can name a correction a concurrently edited Main no
     * longer has, and one stale row must not take the others down with it. */
    Material *source = id_cast<Material *>(
        BKE_libblock_find_session_uid(&bmain, ID_MA, ref.material_session_uid));
    if (source == nullptr) {
      if (r_report != nullptr) {
        r_report->skipped_missing++;
      }
      continue;
    }
    if (source->nodetree != nullptr) {
      source->nodetree->ensure_topology_cache();
    }
    Vector<PaintMaterialLayerStackEntry> source_entries;
    if (!BKE_paint_material_layer_stack_from_material(bmain, *source, source_entries)) {
      if (r_report != nullptr) {
        r_report->skipped_missing++;
      }
      continue;
    }
    PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
    const PaintMaterialLayerCorrectionEntry *source_corr = correction_model_row_find(
        source_entries, ref.correction, &section);
    if (source_corr == nullptr) {
      if (r_report != nullptr) {
        r_report->skipped_missing++;
      }
      continue;
    }

    /* 2. Preflight the target row for the section, before anything is written: a content section
     * on a group row has nothing on the group row to hang on, and skips like a source miss. */
    LayerEditPlan plan;
    if (!layer_edit_plan_build(bmain,
                               target,
                               target_layer_ordinal,
                               LayerEditOp::CorrectionEdit,
                               plan,
                               error,
                               int(section)))
    {
      if (error == PaintMaterialLayerEditError::CorrectionNotAllowedOnGroup) {
        if (r_report != nullptr) {
          r_report->skipped_group++;
        }
        continue;
      }
      return fail(error);
    }
    wrote = true;

    /* 3. The parent channels: every enabled source channel the target has not wired gets its
     * chain first, so the correction below lands in it like in every other -- copying the
     * correction is what includes the channel (spec 18 §4.1). */
    Vector<int> wired_channels;
    BKE_paint_material_layer_channels_wired(target, wired_channels);
    for (const auto &item : source_corr->channel_images.items()) {
      const int channel = item.key;
      if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
        continue;
      }
      if (BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).socket_name == nullptr) {
        /* A channel with no graph of its own (AO) needs no chain: the tagged map below is the
         * whole state there. */
        continue;
      }
      if ((source_corr->disabled_channels_mask & (uint32_t(1) << channel)) != 0 ||
          wired_channels.contains(channel))
      {
        /* A Disabled channel's chain, when its copied map needs one, is built by its own enable
         * below; the report names the channels this copy turns on. */
        continue;
      }
      const int one[1] = {channel};
      if (!BKE_paint_material_layer_channels_ensure(bmain, target, Span<int>(one, 1), &error)) {
        return fail(error);
      }
      wired_channels.append(channel);
      if (r_report != nullptr) {
        r_report->enabled_parent_channels.append_non_duplicates(channel);
      }
    }

    /* 4. The correction itself: Absent in every wired channel, named like the source. */
    bUUID new_marker = BLI_uuid_nil();
    const std::string &name = source_corr->name;
    if (!BKE_paint_material_layer_correction_add(bmain,
                                                 target,
                                                 target_layer_ordinal,
                                                 section,
                                                 source_corr->effect,
                                                 name.empty() ? nullptr : name.c_str(),
                                                 &new_marker,
                                                 &error))
    {
      return fail(error);
    }

    /* 5. The maps. Every channel the source shows a map for is switched on here and takes a deep
     * copy of the source's map, scaled to the target's map size; the channels the source keeps
     * switched off go off afterwards, which is what keeps their copied maps -- a Disabled channel
     * holds its map, an Absent one has none to hold. */
    if (section == PaintMaterialCorrectionSection::Mask) {
      /* A mask has no per-channel choice (spec 18 §4.3): one shared map, tagged
       * #PAINT_LAYER_MAP_MASK, shown by every channel's correction. */
      Image *source_map = source_corr->channel_images.lookup_default(PAINT_LAYER_MAP_MASK,
                                                                     nullptr);
      if (source_map != nullptr &&
          !correction_map_copy_onto(bmain,
                                    target,
                                    new_marker,
                                    section,
                                    PAINT_LAYER_MAP_MASK,
                                    *source_map,
                                    r_report,
                                    error))
      {
        return fail(error);
      }
    }
    else {
      for (const auto &item : source_corr->channel_images.items()) {
        const int channel = item.key;
        if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
          continue;
        }
        if (!BKE_paint_material_layer_correction_channel_enabled_set(
                bmain, target, new_marker, channel, true, &error))
        {
          return fail(error);
        }
        if (!correction_map_copy_onto(bmain,
                                      target,
                                      new_marker,
                                      section,
                                      channel,
                                      *item.value,
                                      r_report,
                                      error))
        {
          return fail(error);
        }
      }
      for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
        if ((source_corr->disabled_channels_mask & (uint32_t(1) << channel)) == 0) {
          continue;
        }
        if (!BKE_paint_material_layer_correction_channel_enabled_set(
                bmain, target, new_marker, channel, false, &error))
        {
          return fail(error);
        }
      }
    }

    /* 6. The row's own parameters, written through the same RNA the UI edits them with: the
     * source model's blend and opacity, onto every channel's nodes of the copy. */
    Vector<PaintMaterialLayerStackEntry> target_entries;
    if (BKE_paint_material_layer_stack_from_material(bmain, target, target_entries)) {
      PaintMaterialLayerCorrectionEntry *target_corr = correction_model_row_find(target_entries,
                                                                                new_marker,
                                                                                nullptr);
      if (target_corr != nullptr) {
        const int ramp = blend_ramp_for(source_corr->blend);
        if (ramp >= 0) {
          for (const auto &item : target_corr->channel_blend_props.items()) {
            RNA_enum_set(&item.value, "blend_type", ramp);
          }
        }
        for (const auto &item : target_corr->channel_factor_props.items()) {
          RNA_float_set(&item.value, "value", source_corr->opacity * 100.0f);
        }
      }
    }

    /* 7. The row's on/off state: a muted source arrives muted. */
    if (!source_corr->enabled &&
        !BKE_paint_material_layer_correction_set_enabled(bmain, target, new_marker, false, &error))
    {
      return fail(error);
    }

    r_created.append(new_marker);
  }

  if (wrote) {
    /* Nodes' ID fields changed hands and new images joined the file: a relations commit, the way
     * every edit that rewires ends. */
    if (target.nodetree != nullptr) {
      BKE_ntree_update_after_single_tree_change(bmain, *target.nodetree);
    }
    paint_layer_edit_committed(bmain, target, true);
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_corrections_scale(Main &bmain,
                                                Material &ma,
                                                const int layer_ordinal,
                                                const int width,
                                                const int height)
{
  if (width <= 0 || height <= 0) {
    return false;
  }
  /* The stack model reads links; a caller that just rewrote a node has left the cache stale. */
  if (ma.nodetree != nullptr) {
    ma.nodetree->ensure_topology_cache();
  }
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, ma, entries)) {
    return false;
  }
  const PaintMaterialLayerStackEntry *entry = nullptr;
  for (const PaintMaterialLayerStackEntry &candidate : entries) {
    if (candidate.ordinal == layer_ordinal) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    return false;
  }

  /* Every map the row's corrections show, all channels and both sections, Disabled ones included:
   * the map is the pixels, and a Disabled channel's map is what re-enabling it brings back. */
  bool scaled = false;
  Set<Image *> seen;
  for (const Vector<PaintMaterialLayerCorrectionEntry> *rows :
       {&entry->content_corrections, &entry->mask_corrections})
  {
    for (const PaintMaterialLayerCorrectionEntry &correction : *rows) {
      for (const auto &item : correction.channel_images.items()) {
        Image *image = item.value;
        if (image == nullptr || !seen.add(image)) {
          continue;
        }
        int map_width = 0;
        int map_height = 0;
        BKE_image_get_size(image, nullptr, &map_width, &map_height);
        if (map_width == width && map_height == height) {
          continue;
        }
        if (BKE_image_scale(image, width, height, nullptr)) {
          /* The same full-update mark the layer's own resize gives its maps: a viewport holding
           * the old pixels has no finer granularity to go on. */
          BKE_image_partial_update_mark_full_update(image);
          scaled = true;
        }
      }
    }
  }

  if (scaled) {
    /* The pixels moved; what shows them is the stack's composite of the row, rebuilt the way
     * every edit of this module tells its readers. */
    paint_layer_edit_committed(bmain, ma, false);
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Correction per-channel flat value
 *
 * The correction-side half of the per-channel value picker: a Fill correction's map in one channel
 * is re-filled with the flat colour the picker stands for there, and the raw colour is recorded on
 * the correction's own Mix node -- the record the getter reads back, the way a Fill layer's marker
 * holds the colour the layer stands for.
 * \{ */

namespace {

/**
 * The write half of the per-channel value API: re-fill the map of \a channel on the correction
 * \a correction with the flat colour \a color stands for there, and -- when \a record_marker --
 * record the raw \a color on the correction's own Mix for
 * #BKE_paint_material_layer_correction_channel_value_get to read.
 *
 * Shared by the apply and the preview so the two cannot drift; what the preview leaves out is the
 * recording, nothing else.
 */
bool correction_channel_value_write(Main &bmain,
                                    Material &ma,
                                    const bUUID &correction,
                                    const int channel,
                                    const float color[4],
                                    const bool record_marker,
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

  /* 1. Preflight: the correction exists, on the row its plan resolves. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    return fail(error);
  }

  /* 2. The channel addressed: a correction the channel does not show -- no chain carries the row
   * in it, or the correction has no map of its own there -- behaves as out of range. */
  ChannelChain *target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  if (target == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  const ChainCorrection *nodes = correction_nodes_find(
      target->layers[plan.layer_index], section, correction);
  if (nodes == nullptr || nodes->image == nullptr || nodes->mix == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 3. Mutation: re-fill this correction's own map, then record the value on its Mix node. */
  float map_color[4];
  fill_map_color_for(channel, color, map_color);
  image_fill_flat(*nodes->image, map_color);
  if (record_marker) {
    bke::paint_layer::channel_value_set(*nodes->mix, channel, color);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

}  // namespace

bool BKE_paint_material_layer_correction_channel_value_get(Main &bmain,
                                                           Material &ma,
                                                           const bUUID &correction,
                                                           const int channel,
                                                           float r_color[4])
{
  /* The same plan the writes build: a correction this module cannot resolve has nothing to
   * read. */
  LayerEditPlan plan;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    return false;
  }
  for (const ChannelChain *chain : plan.chains) {
    if (chain->channel != channel) {
      continue;
    }
    const ChainCorrection *nodes = correction_nodes_find(
        chain->layers[plan.layer_index], section, correction);
    if (nodes == nullptr || nodes->mix == nullptr) {
      /* Not wired on this channel: no Mix carries the recorded value. */
      return false;
    }
    /* The Mix's own record, not the pixels: a painted-over map cannot be asked what colour it was
     * filled with. */
    return bke::paint_layer::channel_value_get(*nodes->mix, channel, r_color);
  }
  return false;
}

bool BKE_paint_material_layer_correction_channel_image_assigned_get(Main &bmain,
                                                                   Material &ma,
                                                                   const bUUID &correction,
                                                                   const int channel)
{
  /* The same plan the writes build: a correction this module cannot resolve has nothing to
   * read. */
  LayerEditPlan plan;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  int owner = -1;
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    return false;
  }
  for (const ChannelChain *chain : plan.chains) {
    if (chain->channel != channel) {
      continue;
    }
    const ChainCorrection *nodes = correction_nodes_find(
        chain->layers[plan.layer_index], section, correction);
    if (nodes == nullptr || nodes->mix == nullptr) {
      /* Not wired on this channel: no Mix carries the record. */
      return false;
    }
    /* The record, not the graph: an assigned image carries the correction's tag like a generated
     * map. */
    return bke::paint_layer::channel_image_assigned_get(*nodes->mix, channel);
  }
  return false;
}

bool BKE_paint_material_layer_correction_channel_value_preview(Main &bmain,
                                                               Material &ma,
                                                               const bUUID &correction,
                                                               const int channel,
                                                               const float color[4],
                                                               PaintMaterialLayerEditError
                                                                   *r_error)
{
  return correction_channel_value_write(bmain, ma, correction, channel, color, false, r_error);
}

bool BKE_paint_material_layer_correction_channel_value_apply(Main &bmain,
                                                             Material &ma,
                                                             const bUUID &correction,
                                                             const int channel,
                                                             const float color[4],
                                                             PaintMaterialLayerEditError *r_error)
{
  return correction_channel_value_write(bmain, ma, correction, channel, color, true, r_error);
}

bool BKE_paint_material_layer_correction_channel_image_set(Main &bmain,
                                                           Material &ma,
                                                           const bUUID &correction,
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

  /* 1. Preflight: the correction exists, on the row its plan resolves. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  PaintMaterialCorrectionEffect effect = PaintMaterialCorrectionEffect::Paint;
  int owner = -1;
  if (!correction_plan_resolve(
          bmain, ma, correction, plan, section, owner, nullptr, error, &effect))
  {
    return fail(error);
  }
  if (section == PaintMaterialCorrectionSection::Mask) {
    if (effect == PaintMaterialCorrectionEffect::Paint) {
      /* A painted mask's map is synced (#layer_mask_corrections_sync), not user-assigned
       * (spec 18 §4.3). */
      return fail(PaintMaterialLayerEditError::CorrectionSectionMismatch);
    }
    /* A Fill mask shows one grayscale map on its one wired channel (spec 18 §4.3): the channel
     * asked for must be the one this correction is wired on. An unwired one is the multi-channel
     * attempt the single-channel form refuses, and so is any channel once several read wired --
     * neither leaves a second grayscale map for the mask to show. */
    if (BKE_paint_material_layer_correction_channel_state_get(bmain, ma, correction, channel) ==
        PaintMaterialLayerChannelState::Absent)
    {
      return fail(PaintMaterialLayerEditError::CorrectionSectionMismatch);
    }
    for (const int other : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      if (other == channel) {
        continue;
      }
      if (BKE_paint_material_layer_correction_channel_state_get(bmain, ma, correction, other) !=
          PaintMaterialLayerChannelState::Absent)
      {
        return fail(PaintMaterialLayerEditError::CorrectionSectionMismatch);
      }
    }
  }

  /* 2. The channel addressed: a correction the channel does not show -- no chain carries the row
   * in it, or the correction has no map of its own there -- behaves as out of range. Creating and
   * wiring channels is #BKE_paint_material_layer_correction_channel_enabled_set's job. */
  ChannelChain *target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  if (target == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  const ChainCorrection *nodes = correction_nodes_find(
      target->layers[plan.layer_index], section, correction);
  if (nodes == nullptr || nodes->map == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 3. Mutation: from here, a refusal is impossible. The map node owns exactly one user of the
   * image it shows: gaining one costs the replacement, and what the replacement orphans -- a
   * generated blank nobody else holds -- is freed at once instead of lingering in the file until
   * a purge. */
  bNodeTree &tree = *target->tree;
  bNode &map_node = *nodes->map;
  auto orphan_check = [&bmain](Image *previous) {
    if (previous != nullptr) {
      id_us_min(&previous->id);
      if (previous->id.us == 0 && previous->source == IMA_SRC_GENERATED) {
        BKE_id_free(&bmain, previous);
      }
    }
  };
  /* The role the assigned image carries: a mask's one map keeps the mask role every reader of a
   * mask map keys on, whatever channel it was assigned through (spec 18 §4.3). */
  const int map_role = (section == PaintMaterialCorrectionSection::Mask) ? PAINT_LAYER_MAP_MASK :
                                                                           channel;

  Image *previous = (map_node.id != nullptr && GS(map_node.id->name) == ID_IM) ?
                        id_cast<Image *>(map_node.id) :
                        nullptr;
  map_node.id = &image.id;
  if (previous != &image) {
    /* The node's user moves from the old image to the new one. */
    id_us_plus(&image.id);
    /* What the model lists as the map is read off the tag, so a replaced image that survives --
     * someone else still holds it -- loses the tag with the node. */
    if (previous != nullptr && BLI_uuid_equal(previous->paint_layer_id, correction) &&
        previous->paint_layer_channel == map_role)
    {
      previous->paint_layer_id = bUUID{};
      previous->paint_layer_channel = PAINT_LAYER_MAP_NONE;
    }
    orphan_check(previous);
  }
  /* The tags every map of a correction carries: the stack model that assembles a correction's
   * maps reads them back. */
  image.paint_layer_id = correction;
  image.paint_layer_channel = map_role;
  /* The graph cannot tell this assignment from a generated map -- both carry the correction's tag
   * -- so the record the UI reads is written here and cleared by the unlink. A channel with no
   * graph of its own (AO) has no Mix to carry the record on. */
  if (nodes->mix != nullptr) {
    bke::paint_layer::channel_image_assigned_set(*nodes->mix, channel, true);
  }

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

bool BKE_paint_material_layer_correction_channel_unlink(Main &bmain,
                                                        Material &ma,
                                                        const bUUID &correction,
                                                        const int channel,
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

  /* 1. Preflight: the correction exists, on the row its plan resolves. */
  LayerEditPlan plan;
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  PaintMaterialCorrectionEffect effect = PaintMaterialCorrectionEffect::Paint;
  int owner = -1;
  if (!correction_plan_resolve(
          bmain, ma, correction, plan, section, owner, nullptr, error, &effect))
  {
    return fail(error);
  }

  /* 2. The channel addressed: a correction the channel does not show -- no chain carries the row
   * in it, or the correction has no map of its own there -- behaves as out of range. */
  ChannelChain *target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  if (target == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  const ChainCorrection *nodes = correction_nodes_find(
      target->layers[plan.layer_index], section, correction);
  if (nodes == nullptr || nodes->image == nullptr || nodes->mix == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 3. The value the map gets back: the last one recorded on the correction's Mix, or the neutral
   * one a map of the channel starts at. The neutral value is recorded like an applied one, so the
   * picker reads back what the map holds. */
  float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bke::paint_layer::channel_value_get(*nodes->mix, channel, value);
  float map_color[4];
  fill_map_color_for(channel, value, map_color);

  /* 4. A painted mask correction owns its one map outright: nothing can have replaced it, so
   * unlinking re-fills the shared pixels in place and touches no link. */
  if (section == PaintMaterialCorrectionSection::Mask &&
      effect == PaintMaterialCorrectionEffect::Paint)
  {
    if (!BLI_uuid_equal(nodes->image->paint_layer_id, correction) ||
        nodes->image->paint_layer_channel != PAINT_LAYER_MAP_MASK)
    {
      /* Not the map this correction created: nothing this module builds can wire one. */
      return fail(PaintMaterialLayerEditError::CorrectionSectionMismatch);
    }
    image_fill_flat(*nodes->image, map_color);
    bke::paint_layer::channel_value_set(*nodes->mix, channel, value);
    bke::paint_layer::channel_image_assigned_set(*nodes->mix, channel, false);
    paint_layer_edit_committed(bmain, ma, false);
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  }

  /* 5. Mutation: a fresh map of the correction's own takes the channel over, created the way the
   * channel-enable path creates one. Whatever the channel showed is never written to: an image
   * assigned through #BKE_paint_material_layer_correction_channel_image_set carries the
   * correction's tag like a generated one, so owning cannot be read back off the graph, and
   * refilling it in place might destroy a dropped image's pixels. A Fill mask correction takes
   * the same road -- an assignment can have replaced its one shared grayscale map, tagged as its
   * own like every map of a correction -- and gives the shared node a fresh grayscale map. */
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  int width = 0;
  int height = 0;
  if (!BKE_paint_material_layer_map_size_get(bmain, ma, owner, width, height) || width <= 0 ||
      height <= 0)
  {
    width = 1024;
    height = 1024;
  }
  char map_name[MAX_ID_NAME - 2];
  SNPRINTF_UTF8(map_name, "%s Correction", info.ui_name);
  const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  /* A mask's map is grayscale, non-color data, whatever channel it was reached through. */
  const bool is_data = (section == PaintMaterialCorrectionSection::Mask) ? true : !info.is_color;
  Image *fresh = BKE_image_add_generated(&bmain,
                                        width,
                                        height,
                                        map_name,
                                        32,
                                        false,
                                        IMA_GENTYPE_BLANK,
                                        transparent,
                                        false,
                                        is_data,
                                        false);
  if (fresh == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  /* The paint-canvas flag every map the creation paths make carries; the tags are the wiring
   * below's own to apply. */
  fresh->flag |= IMA_PAINT_CANVAS;
  if (!BKE_paint_material_layer_correction_channel_image_set(
          bmain, ma, correction, channel, *fresh, &error))
  {
    /* Never wired: the fresh map is still only the creation's user, this call's to free. */
    BKE_id_free(&bmain, fresh);
    return fail(error);
  }
  /* The map node's user is the one the image-set took; the one the creation gave is the extra. */
  id_us_min(&fresh->id);

  /* 6. The wiring replaced the map, so the plan is read again before the pixels and the record
   * are written -- the pattern every edit that relinks first follows. */
  plan = LayerEditPlan();
  if (!correction_plan_resolve(bmain, ma, correction, plan, section, owner, nullptr, error)) {
    BLI_assert_unreachable();
    return fail(error);
  }
  target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  nodes = nullptr;
  if (target != nullptr) {
    nodes = correction_nodes_find(target->layers[plan.layer_index], section, correction);
  }
  if (nodes == nullptr || nodes->mix == nullptr) {
    BLI_assert_unreachable();
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  image_fill_flat(*fresh, map_color);
  bke::paint_layer::channel_value_set(*nodes->mix, channel, value);
  bke::paint_layer::channel_image_assigned_set(*nodes->mix, channel, false);

  /* The relation edit was #BKE_paint_material_layer_correction_channel_image_set's to commit; the
   * refill and the record are values inside one tree. */
  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

/** \} */

}  // namespace blender
