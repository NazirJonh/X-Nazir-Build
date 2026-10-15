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

/** A Mix node in float mode: `mix(A, B, factor)`, the coverage fold of an isolated row. */
static bNode *mix_float_node_add(bNodeTree &tree)
{
  bNode *mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX);
  if (mix == nullptr) {
    return nullptr;
  }
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(mix->storage);
  storage->data_type = SOCK_FLOAT;
  storage->factor_mode = NODE_MIX_MODE_UNIFORM;
  storage->clamp_factor = true;
  storage->clamp_result = false;
  return mix;
}

RowResult PaintLayersTreeBuilder::build_row(const MaterialPaintLayer &layer,
                                            LayerGroup &group,
                                            const int channel,
                                            const bool isolated)
{
  bNodeTree &tree = *group.tree;
  bNode *group_input = group.group_input;
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  /* A folder owns no record: its source is the isolated result of what it holds. */
  const bool folder = BKE_paint_layers_is_folder(layer);
  if (record == nullptr && !folder) {
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

  /* Inside a folder the contract carries the coverage next to the colour. */
  bNodeTreeInterfaceSocket *below_coverage_iface = nullptr;
  bNodeTreeInterfaceSocket *result_coverage_iface = nullptr;
  bNodeSocket *below_coverage = nullptr;
  bNodeSocket *result_coverage_out = nullptr;
  if (isolated) {
    SNPRINTF(socket_name, "Below Coverage %s", info.ui_name);
    below_coverage_iface = layer_group_add_socket(
        group, socket_name, "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT);
    SNPRINTF(socket_name, "Result Coverage %s", info.ui_name);
    result_coverage_iface = layer_group_add_socket(
        group, socket_name, "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT);
    if (below_coverage_iface == nullptr || result_coverage_iface == nullptr) {
      return {};
    }
    below_coverage = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(below_coverage_iface->identifier));
    result_coverage_out = bke::node_find_socket(
        *group.group_output, SOCK_IN, UString::from_ptr_noinline(result_coverage_iface->identifier));
    if (below_coverage == nullptr || result_coverage_out == nullptr) {
      return {};
    }
  }

  /* A folder takes the straightened result of its contents and the coverage they accumulated. */
  bNodeTreeInterfaceSocket *folder_color_iface = nullptr;
  bNodeTreeInterfaceSocket *folder_coverage_iface = nullptr;
  bNodeSocket *folder_color = nullptr;
  bNodeSocket *folder_coverage = nullptr;
  if (folder) {
    SNPRINTF(socket_name, "Folder Color %s", info.ui_name);
    folder_color_iface = layer_group_add_socket(
        group, socket_name, "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT);
    SNPRINTF(socket_name, "Folder Coverage %s", info.ui_name);
    folder_coverage_iface = layer_group_add_socket(
        group, socket_name, "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT);
    if (folder_color_iface == nullptr || folder_coverage_iface == nullptr) {
      return {};
    }
    folder_color = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(folder_color_iface->identifier));
    folder_coverage = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(folder_coverage_iface->identifier));
    if (folder_color == nullptr || folder_coverage == nullptr) {
      return {};
    }
  }

  /* The row's source: its map, or the constant the row's group takes as an input. */
  bNode *source_node = nullptr;
  bNodeSocket *source = nullptr;
  bNode *map_node = nullptr;
  Vector<bNode *> source_nodes;
  if (folder) {
    source_node = group_input;
    source = folder_color;
  }
  else if (record->image != nullptr) {
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

  /* A folder covers by the coverage its contents accumulated: where nothing of them reaches, the
   * folder shows what is below however opaque its own factor is. */
  if (folder) {
    if (factor == nullptr) {
      factor_node = group_input;
      factor = folder_coverage;
    }
    else {
      const NodeOutput product = this->multiply_nodes_add(
          tree, {factor_node, factor}, {group_input, folder_coverage});
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
  if (!isolated) {
    blend_inputs_link(tree,
                      blend,
                      {group_input, below},
                      {source_node, source},
                      {factor_node, factor});
    bke::node_add_link(tree, *blend.node, *blend.result, *group.group_output, *result_out);
    Vector<bNode *> blend_nodes;
    blend_nodes.append(blend.node);
    layout::frame_add(tree, "Blend", blend_nodes);
  }
  else {
    /* Inside a folder `below` is the pre-multiplied accumulation `P` and `below_coverage` its
     * coverage `a`. With `S = P / a` the straight colour so far and `f` this row's factor:
     *   c_eff = lerp(c, blend(S, c), a)   (where nothing is below yet the row is its own colour)
     *   P'    = lerp(P, c_eff, f)
     *   a'    = lerp(a, 1, f)
     * which is what the CPU's isolated accumulation computes row by row. */
    bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
    bNode *c_eff = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
    bNode *premul = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
    bNode *coverage = mix_float_node_add(tree);
    if (divide == nullptr || c_eff == nullptr || premul == nullptr || coverage == nullptr) {
      return {};
    }
    divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
    grid_.place(*divide, layout::Column::Output, 7);
    grid_.place(*c_eff, layout::Column::Output, 8);
    grid_.place(*premul, layout::Column::Output, 9);
    grid_.place(*coverage, layout::Column::Output, 10);

    bNodeSocket *divide_a = socket_in(*divide, "Vector");
    bNodeSocket *divide_b = socket_in(*divide, "Vector_001");
    bNodeSocket *divide_out = socket_out(*divide, "Vector");
    bNodeSocket *eff_a = socket_in(*c_eff, "A_Color");
    bNodeSocket *eff_b = socket_in(*c_eff, "B_Color");
    bNodeSocket *eff_fac = socket_in(*c_eff, "Factor_Float");
    bNodeSocket *eff_out = socket_out(*c_eff, "Result_Color");
    bNodeSocket *premul_a = socket_in(*premul, "A_Color");
    bNodeSocket *premul_b = socket_in(*premul, "B_Color");
    bNodeSocket *premul_fac = socket_in(*premul, "Factor_Float");
    bNodeSocket *premul_out = socket_out(*premul, "Result_Color");
    bNodeSocket *cover_a = socket_in(*coverage, "A_Float");
    bNodeSocket *cover_b = socket_in(*coverage, "B_Float");
    bNodeSocket *cover_fac = socket_in(*coverage, "Factor_Float");
    bNodeSocket *cover_out = socket_out(*coverage, "Result_Float");
    const std::array<const bNodeSocket *, 16> sockets = {divide_a,
                                                         divide_b,
                                                         divide_out,
                                                         eff_a,
                                                         eff_b,
                                                         eff_fac,
                                                         eff_out,
                                                         premul_a,
                                                         premul_b,
                                                         premul_fac,
                                                         premul_out,
                                                         cover_a,
                                                         cover_b,
                                                         cover_fac,
                                                         cover_out,
                                                         below_coverage};
    if (std::find(sockets.begin(), sockets.end(), nullptr) != sockets.end()) {
      return {};
    }

    bke::node_add_link(tree, *group_input, *below, *divide, *divide_a);
    bke::node_add_link(tree, *group_input, *below_coverage, *divide, *divide_b);
    /* The row blends over the straightened accumulation at full factor; the factor is applied
     * after, to the whole colour. */
    blend_inputs_link(tree, blend, {divide, divide_out}, {source_node, source}, {});
    bke::node_add_link(tree, *source_node, *source, *c_eff, *eff_a);
    bke::node_add_link(tree, *blend.node, *blend.result, *c_eff, *eff_b);
    bke::node_add_link(tree, *group_input, *below_coverage, *c_eff, *eff_fac);

    bke::node_add_link(tree, *group_input, *below, *premul, *premul_a);
    bke::node_add_link(tree, *c_eff, *eff_out, *premul, *premul_b);
    bke::node_add_link(tree, *group_input, *below_coverage, *coverage, *cover_a);
    static_cast<bNodeSocketValueFloat *>(cover_b->default_value)->value = 1.0f;
    if (factor != nullptr) {
      bke::node_add_link(tree, *factor_node, *factor, *premul, *premul_fac);
      bke::node_add_link(tree, *factor_node, *factor, *coverage, *cover_fac);
    }
    else {
      /* No factor at all: the row covers fully. */
      static_cast<bNodeSocketValueFloat *>(premul_fac->default_value)->value = 1.0f;
      static_cast<bNodeSocketValueFloat *>(cover_fac->default_value)->value = 1.0f;
    }
    bke::node_add_link(tree, *premul, *premul_out, *group.group_output, *result_out);
    bke::node_add_link(
        tree, *coverage, *cover_out, *group.group_output, *result_coverage_out);

    Vector<bNode *> blend_nodes = {blend.node, divide, c_eff, premul, coverage};
    layout::frame_add(tree, "Isolated Blend", blend_nodes);
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
  if (isolated) {
    result.below_coverage = bke::node_find_socket(
        *group.instance, SOCK_IN, UString::from_ptr_noinline(below_coverage_iface->identifier));
    result.result_coverage = bke::node_find_socket(
        *group.instance, SOCK_OUT, UString::from_ptr_noinline(result_coverage_iface->identifier));
    if (result.below_coverage == nullptr || result.result_coverage == nullptr) {
      return {};
    }
  }
  if (folder) {
    result.folder_color = bke::node_find_socket(
        *group.instance, SOCK_IN, UString::from_ptr_noinline(folder_color_iface->identifier));
    result.folder_coverage = bke::node_find_socket(
        *group.instance, SOCK_IN, UString::from_ptr_noinline(folder_coverage_iface->identifier));
    if (result.folder_color == nullptr || result.folder_coverage == nullptr) {
      return {};
    }
  }
  return result;
}

RowResult PaintLayersTreeBuilder::row_link_reused(const MaterialPaintLayer &layer,
                                                  LayerGroup &group,
                                                  const int channel,
                                                  const bool isolated)
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
  /* The same extra sockets build_row adds, found by the names it gives them. */
  const auto instance_input = [&](const char *prefix, const StringRef type, const bool input)
      -> bNodeSocket * {
    SNPRINTF(socket_name, "%s %s", prefix, info.ui_name);
    bNodeTreeInterfaceSocket *iface = layer_group_socket_find(group, socket_name, type, input);
    if (iface == nullptr) {
      return nullptr;
    }
    return bke::node_find_socket(
        *group.instance, input ? SOCK_IN : SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
  };
  if (isolated) {
    result.below_coverage = instance_input("Below Coverage", "NodeSocketFloat", true);
    result.result_coverage = instance_input("Result Coverage", "NodeSocketFloat", false);
    if (result.below_coverage == nullptr || result.result_coverage == nullptr) {
      return {};
    }
  }
  if (BKE_paint_layers_is_folder(layer)) {
    result.folder_color = instance_input("Folder Color", "NodeSocketColor", true);
    result.folder_coverage = instance_input("Folder Coverage", "NodeSocketFloat", true);
    if (result.folder_color == nullptr || result.folder_coverage == nullptr) {
      return {};
    }
  }
  return result;
}

}  // namespace bke::paint_layers
}  // namespace blender
