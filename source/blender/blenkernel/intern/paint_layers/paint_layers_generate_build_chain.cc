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

#include <algorithm>
#include <array>

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

bNode *PaintLayersTreeBuilder::image_map_node_add(bNodeTree &tree, Image &image)
{
  bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  if (map == nullptr) {
    return nullptr;
  }
  map->id = &image.id;
  id_us_plus(&image.id);
  return map;
}

NodeOutput PaintLayersTreeBuilder::rgb_mean_nodes_add(bNodeTree &tree,
                                                      const NodeOutput color,
                                                      Vector<bNode *> &r_nodes)
{
  bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
  bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  const std::array<bNode *, 4> created = {separate, add_xy, add_z, divide};
  /* Nothing is linked yet, so a node that cannot be completed is simply removed again. */
  const auto discard = [&]() {
    for (bNode *node : created) {
      if (node != nullptr) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
    }
  };
  if (separate == nullptr || add_xy == nullptr || add_z == nullptr || divide == nullptr ||
      color.node == nullptr || color.socket == nullptr)
  {
    discard();
    return {};
  }

  bNodeSocket *sep_vector = socket_in(*separate, "Vector");
  bNodeSocket *sep_x = socket_out(*separate, "X");
  bNodeSocket *sep_y = socket_out(*separate, "Y");
  bNodeSocket *sep_z = socket_out(*separate, "Z");
  bNodeSocket *xy_a = socket_in(*add_xy, "Value");
  bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
  bNodeSocket *xy_out = socket_out(*add_xy, "Value");
  bNodeSocket *z_a = socket_in(*add_z, "Value");
  bNodeSocket *z_b = socket_in(*add_z, "Value_001");
  bNodeSocket *z_out = socket_out(*add_z, "Value");
  bNodeSocket *d_a = socket_in(*divide, "Value");
  bNodeSocket *d_b = socket_in(*divide, "Value_001");
  bNodeSocket *d_out = socket_out(*divide, "Value");
  const std::array<const bNodeSocket *, 13> sockets = {sep_vector,
                                                       sep_x,
                                                       sep_y,
                                                       sep_z,
                                                       xy_a,
                                                       xy_b,
                                                       xy_out,
                                                       z_a,
                                                       z_b,
                                                       z_out,
                                                       d_a,
                                                       d_b,
                                                       d_out};
  if (std::find(sockets.begin(), sockets.end(), nullptr) != sockets.end()) {
    discard();
    return {};
  }

  add_xy->custom1 = NODE_MATH_ADD;
  add_z->custom1 = NODE_MATH_ADD;
  divide->custom1 = NODE_MATH_DIVIDE;
  grid_.place(*separate, layout::Column::Mask, 1);
  grid_.place(*add_xy, layout::Column::Mask, 2);
  grid_.place(*add_z, layout::Column::Mask, 3);
  grid_.place(*divide, layout::Column::Mask, 4);
  bke::node_add_link(tree, *color.node, *color.socket, *separate, *sep_vector);
  bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
  bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
  bke::node_add_link(tree, *add_xy, *xy_out, *add_z, *z_a);
  bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
  bke::node_add_link(tree, *add_z, *z_out, *divide, *d_a);
  if (d_b->default_value != nullptr) {
    static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
  }
  for (bNode *node : created) {
    r_nodes.append(node);
  }
  return {divide, d_out};
}

NodeOutput PaintLayersTreeBuilder::mask_factor_ensure(bNodeTree &tree,
                                                      LayerGroup &group,
                                                      const MaterialPaintLayer &layer)
{
  if (const NodeOutput *cached = mask_factors_.lookup_ptr(&layer)) {
    return *cached;
  }
  const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer);
  bNodeTreeInterfaceSocket **opacity_iface = mask_opacity_inputs_.lookup_ptr(&layer);
  if (mask == nullptr || opacity_iface == nullptr) {
    return {};
  }
  bNodeSocket *opacity = this->group_input_socket(group, **opacity_iface);
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (opacity == nullptr || record == nullptr) {
    return {};
  }

  /* Every node this function adds, so that a failure part way leaves nothing half built behind. */
  Vector<bNode *> nodes;
  const auto discard = [&]() {
    for (bNode *node : nodes) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
  };

  /* The grey of the item and, for a map, its own alpha: the CPU composite reads the mean of the
   * stored RGB and the texel's alpha, so a texel the mask has not been painted on (transparent)
   * leaves the row visible. */
  NodeOutput grey;
  NodeOutput alpha;
  if (record->image != nullptr) {
    bNode *map = this->image_map_node_add(tree, *record->image);
    if (map == nullptr) {
      return {};
    }
    nodes.append(map);
    grid_.place(*map, layout::Column::Mask, 0);
    bNodeSocket *map_color = socket_out(*map, "Color");
    bNodeSocket *map_alpha = socket_out(*map, "Alpha");
    if (map_color == nullptr || map_alpha == nullptr) {
      discard();
      return {};
    }
    grey = this->rgb_mean_nodes_add(tree, {map, map_color}, nodes);
    if (grey.node == nullptr) {
      discard();
      return {};
    }
    alpha = {map, map_alpha};
  }
  else {
    bNodeTreeInterfaceSocket **value_iface = mask_value_inputs_.lookup_ptr(&layer);
    bNodeSocket *value = (value_iface != nullptr) ? this->group_input_socket(group, **value_iface) :
                                                    nullptr;
    if (value == nullptr) {
      return {};
    }
    grey = {group.group_input, value};
  }

  /* `F = mix(1, C, A * opacity)`: the Multiply item over a factor of one. */
  NodeOutput weight = {group.group_input, opacity};
  if (alpha.node != nullptr) {
    weight = this->multiply_nodes_add(tree, alpha, weight);
    if (weight.node == nullptr) {
      discard();
      return {};
    }
  }
  bNode *mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX);
  if (mix == nullptr) {
    discard();
    return {};
  }
  nodes.append(mix);
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(mix->storage);
  storage->data_type = SOCK_FLOAT;
  storage->factor_mode = NODE_MIX_MODE_UNIFORM;
  storage->clamp_factor = true;
  storage->clamp_result = false;
  grid_.place(*mix, layout::Column::Mask, 5);
  bNodeSocket *factor_in = socket_in(*mix, "Factor_Float");
  bNodeSocket *a_in = socket_in(*mix, "A_Float");
  bNodeSocket *b_in = socket_in(*mix, "B_Float");
  bNodeSocket *result = socket_out(*mix, "Result_Float");
  if (factor_in == nullptr || a_in == nullptr || b_in == nullptr || result == nullptr ||
      weight.socket == nullptr || grey.socket == nullptr)
  {
    discard();
    return {};
  }
  if (a_in->default_value != nullptr) {
    static_cast<bNodeSocketValueFloat *>(a_in->default_value)->value = 1.0f;
  }
  bke::node_add_link(tree, *weight.node, *weight.socket, *mix, *factor_in);
  bke::node_add_link(tree, *grey.node, *grey.socket, *mix, *b_in);
  layout::frame_add(tree, "Mask", nodes);

  const NodeOutput out = {mix, result};
  mask_factors_.add(&layer, out);
  return out;
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
    bNode *map = this->image_map_node_add(tree, *record->image);
    if (map == nullptr) {
      return {};
    }
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

  /* The row's mask lays its own factor over the row's, the same for every channel. */
  const NodeOutput mask = this->mask_factor_ensure(tree, group, layer);
  if (mask.node != nullptr) {
    if (factor == nullptr) {
      factor_node = mask.node;
      factor = mask.socket;
    }
    else {
      const NodeOutput product = this->multiply_nodes_add(tree, {factor_node, factor}, mask);
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

RowResult PaintLayersTreeBuilder::row_link_reused(const MaterialPaintLayer & /*layer*/,
                                                  LayerGroup &group,
                                                  const int channel)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char socket_name[192];
  SNPRINTF(socket_name, "Below %s", info.ui_name);
  bNodeTreeInterfaceSocket *below_iface = layer_group_socket_find(
      group, socket_name, "NodeSocketColor", true);
  SNPRINTF(socket_name, "Result %s", info.ui_name);
  bNodeTreeInterfaceSocket *result_iface = layer_group_socket_find(
      group, socket_name, "NodeSocketColor", false);
  if (below_iface == nullptr || result_iface == nullptr) {
    return {};
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
