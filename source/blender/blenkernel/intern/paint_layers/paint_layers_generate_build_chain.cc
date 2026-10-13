/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The nodes of one row of one channel, built inside the row's own group: its source (the channel's
 * map, or the constant input), its factor (opacity times the map's own alpha) and the blend over
 * what the group's Below input carries.
 */

#include "BKE_paint_layers_generate.hh"

#include "BLI_string.h"
#include "BLI_ustring.hh"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "paint_layers_generate_build_intern.hh"
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"
#include "../paint_material_composite_internal.hh"

namespace blender {
namespace bke::paint_layers {

NodeOutput PaintLayersTreeBuilder::multiply_nodes_add(bNodeTree &tree,
                                                      const NodeOutput a,
                                                      const NodeOutput b)
{
  bNode *product = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (product == nullptr) {
    return {};
  }
  bNodeSocket *a_in = socket_in(*product, "Value");
  bNodeSocket *b_in = socket_in(*product, "Value_001");
  bNodeSocket *out = socket_out(*product, "Value");
  if (a_in == nullptr || b_in == nullptr || out == nullptr || a.node == nullptr ||
      b.node == nullptr || a.socket == nullptr || b.socket == nullptr)
  {
    return {};
  }
  product->custom1 = NODE_MATH_MULTIPLY;
  /* Why grid: the opacity fold sits in Output past the source. */
  grid_.place(*product, layout::Column::Output, 3);
  bke::node_add_link(tree, *a.node, *a.socket, *product, *a_in);
  bke::node_add_link(tree, *b.node, *b.socket, *product, *b_in);
  Vector<bNode *> product_nodes;
  product_nodes.append(product);
  layout::frame_add(tree, "Opacity x Factor", product_nodes);
  return {product, out};
}

BlendNode PaintLayersTreeBuilder::blend_node_add(bNodeTree &tree,
                                                 const int channel,
                                                 const int ramp_blend)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx_.normal_combine_group != nullptr &&
      tree.typeinfo != nullptr && tree.typeinfo->group_idname != nullptr)
  {
    bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
    if (combine != nullptr) {
      combine->id = &ctx_.normal_combine_group->id;
      id_us_plus(&ctx_.normal_combine_group->id);
      /* Why extra: the combine instance owns no named grid slot per channel. */
      grid_.place_extra(*combine);
      /* A hand-assigned group only grows its instance sockets once its declaration is built;
       * this instantiates them from the group's interface without needing #Main or a whole
       * tree update. */
      nodes::update_node_declaration_and_sockets(tree, *combine);
      BlendNode blend;
      blend.node = combine;
      blend.a = bke::node_find_socket(
          *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
      blend.b = bke::node_find_socket(
          *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
      blend.factor = bke::node_find_socket(
          *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
      blend.result = bke::node_find_socket(
          *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
      if (blend.a == nullptr || blend.b == nullptr || blend.factor == nullptr ||
          blend.result == nullptr)
      {
        /* The sockets are the group's own interface; a missing one is a build error, not a shape
         * to paper over with a Mix that would silently flatten the relief. */
        BLI_assert_msg(false, "Normal Combine instance sockets were not instantiated");
        return {};
      }
      return blend;
    }
  }

  bNode *mix = mix_node_add(tree, ramp_blend, 0.0f, 0.0f);
  if (mix == nullptr) {
    return {};
  }
  /* Why grid: the row's own Mix sits in the free Output rows. */
  grid_.place(*mix, layout::Column::Output, 6);
  BlendNode blend;
  blend.node = mix;
  blend.a = socket_in(*mix, "A_Color");
  blend.b = socket_in(*mix, "B_Color");
  blend.factor = socket_in(*mix, "Factor_Float");
  blend.result = socket_out(*mix, "Result_Color");
  if (blend.a == nullptr || blend.b == nullptr || blend.factor == nullptr ||
      blend.result == nullptr)
  {
    return {};
  }
  return blend;
}

void blend_inputs_link(bNodeTree &tree,
                       const BlendNode &blend,
                       const NodeOutput below,
                       const NodeOutput source,
                       const NodeOutput factor)
{
  bke::node_add_link(tree, *below.node, *below.socket, *blend.node, *blend.a);
  bke::node_add_link(tree, *source.node, *source.socket, *blend.node, *blend.b);
  if (factor.socket != nullptr) {
    bke::node_add_link(tree, *factor.node, *factor.socket, *blend.node, *blend.factor);
  }
  else if (blend.factor->default_value != nullptr) {
    /* No opacity input and no map alpha: the row covers fully. */
    static_cast<bNodeSocketValueFloat *>(blend.factor->default_value)->value = 1.0f;
  }
}

RowResult PaintLayersTreeBuilder::build_row(const MaterialPaintLayer &layer,
                                            LayerGroup &group,
                                            const int channel)
{
  bNodeTree &tree = *group.tree;
  bNode *group_input = group.group_input;
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  if (record == nullptr) {
    return {};
  }

  /* The group's contract for this channel: what is below, and the row laid over it. */
  char socket_name[192];
  SNPRINTF(socket_name, "Below %s", info.ui_name);
  bNodeTreeInterfaceSocket *below_iface = layer_group_add_socket(
      group, socket_name, "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT);
  SNPRINTF(socket_name, "Result %s", info.ui_name);
  bNodeTreeInterfaceSocket *result_iface = layer_group_add_socket(
      group, socket_name, "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
  if (below_iface == nullptr || result_iface == nullptr) {
    return {};
  }
  bNodeSocket *below = bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline(below_iface->identifier));
  bNodeSocket *result_out = bke::node_find_socket(
      *group.group_output, SOCK_IN, UString::from_ptr_noinline(result_iface->identifier));
  if (below == nullptr || result_out == nullptr) {
    return {};
  }

  /* The row's source: its map, or the constant the row's group takes as an input. */
  bNode *source_node = nullptr;
  bNodeSocket *source = nullptr;
  bNode *map_node = nullptr;
  Vector<bNode *> source_nodes;
  if (record->image != nullptr) {
    Image *image = record->image;
    bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    if (map == nullptr) {
      return {};
    }
    map->id = &image->id;
    id_us_plus(&image->id);
    /* Why grid: the row source opens the Source column at the origin. */
    grid_.place(*map, layout::Column::Source, 0);
    source_nodes.append(map);
    source_node = map;
    source = socket_out(*map, "Color");
    map_node = map;
  }
  else {
    bNodeTreeInterfaceSocket *const *fill_iface = nullptr;
    if (Map<int, bNodeTreeInterfaceSocket *> *by_channel = fill_inputs_.lookup_ptr(&layer)) {
      fill_iface = by_channel->lookup_ptr(channel);
    }
    if (fill_iface == nullptr) {
      return {};
    }
    source_node = group_input;
    source = group_input_socket(group, **fill_iface);
  }
  if (source_node == nullptr || source == nullptr) {
    return {};
  }
  if (!source_nodes.is_empty()) {
    layout::frame_add(tree, "Source", source_nodes);
  }

  /* The factor is the row's opacity times the map's own alpha: a texel the map leaves transparent
   * shows what is below. A constant covers by its opacity alone. */
  bNode *factor_node = nullptr;
  bNodeSocket *factor = nullptr;
  if (Map<int, bNodeTreeInterfaceSocket *> *by_channel = opacity_inputs_.lookup_ptr(&layer)) {
    if (bNodeTreeInterfaceSocket **opacity_iface = by_channel->lookup_ptr(channel)) {
      factor_node = group_input;
      factor = group_input_socket(group, **opacity_iface);
    }
  }
  if (map_node != nullptr) {
    bNodeSocket *map_alpha = socket_out(*map_node, "Alpha");
    if (factor == nullptr) {
      factor_node = map_node;
      factor = map_alpha;
    }
    else if (map_alpha != nullptr) {
      const NodeOutput product = this->multiply_nodes_add(
          tree, {factor_node, factor}, {map_node, map_alpha});
      if (product.node != nullptr) {
        factor_node = product.node;
        factor = product.socket;
      }
    }
  }

  /* The blend over Below. Normal combines two tangent-space maps rather than interpolating their
   * encoded values, which would flatten the relief; everything else uses the row's own blend. */
  const BlendNode blend = this->blend_node_add(
      tree, channel, BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(layer.blend)));
  if (blend.node == nullptr) {
    return {};
  }
  blend_inputs_link(tree,
                    blend,
                    {group_input, below},
                    {source_node, source},
                    {factor_node, factor});
  bke::node_add_link(tree, *blend.node, *blend.result, *group.group_output, *result_out);
  {
    Vector<bNode *> blend_nodes;
    blend_nodes.append(blend.node);
    layout::frame_add(tree, "Blend", blend_nodes);
  }

  RowResult result;
  result.valid = true;
  result.instance = group.instance;
  result.below = bke::node_find_socket(
      *group.instance, SOCK_IN, UString::from_ptr_noinline(below_iface->identifier));
  result.result = bke::node_find_socket(
      *group.instance, SOCK_OUT, UString::from_ptr_noinline(result_iface->identifier));
  if (result.below == nullptr || result.result == nullptr) {
    return {};
  }
  return result;
}

}  // namespace bke::paint_layers
}  // namespace blender
