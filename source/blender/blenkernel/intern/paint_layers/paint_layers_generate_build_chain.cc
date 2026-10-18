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

#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_ustring.hh"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_object_mask.hh"
#include "BKE_paint_material_composite.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_customdata_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "paint_layers_generate_build_intern.hh"
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"
#include "../paint_material_composite_internal.hh"

namespace blender {
namespace bke::paint_layers {

/* Defined with the correction code below, which also builds Math nodes; the mask code needs it
 * first. The default of the last argument is given at the definition. */
static NodeOutput math_node_add(bNodeTree &tree,
                                layout::LayoutGrid &grid,
                                Vector<bNode *> &r_nodes,
                                int operation,
                                NodeOutput a,
                                NodeOutput b,
                                float a_default = 0.0f);

/** A Math node of \a operation over \a a and the constant \a b; defined with the mask code below. */
static bNode *math_constant_node_add(bNodeTree &tree,
                                     layout::LayoutGrid &grid,
                                     Vector<bNode *> &r_nodes,
                                     int operation,
                                     NodeOutput a,
                                     float b);

/** A Mix node in float mode, `mix(A, B, factor)`; defined with the isolated-fold code below. */
static bNode *mix_float_node_add(bNodeTree &tree);

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

NodeOutput PaintLayersTreeBuilder::normal_green_flip_nodes_add(bNodeTree &tree,
                                                               const NodeOutput color,
                                                               Vector<bNode *> &r_nodes)
{
  bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
  bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
  const auto discard = [&]() {
    for (bNode *node : {separate, combine}) {
      if (node != nullptr) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
    }
  };
  if (separate == nullptr || combine == nullptr || color.node == nullptr ||
      color.socket == nullptr)
  {
    discard();
    return {};
  }
  bNodeSocket *sep_vector = socket_in(*separate, "Vector");
  bNodeSocket *sep_x = socket_out(*separate, "X");
  bNodeSocket *sep_y = socket_out(*separate, "Y");
  bNodeSocket *sep_z = socket_out(*separate, "Z");
  bNodeSocket *com_x = socket_in(*combine, "X");
  bNodeSocket *com_y = socket_in(*combine, "Y");
  bNodeSocket *com_z = socket_in(*combine, "Z");
  bNodeSocket *com_out = socket_out(*combine, "Vector");
  if (sep_vector == nullptr || sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
      com_x == nullptr || com_y == nullptr || com_z == nullptr || com_out == nullptr)
  {
    discard();
    return {};
  }
  /* The Subtract is built last: it is the only node that can fail after these two exist, and it
   * removes only itself. */
  Vector<bNode *> flip_nodes;
  const NodeOutput flipped = math_node_add(
      tree, grid_, flip_nodes, NODE_MATH_SUBTRACT, {}, {separate, sep_y}, 1.0f);
  if (flipped.node == nullptr || flipped.socket == nullptr) {
    discard();
    return {};
  }
  grid_.place(*separate, layout::Column::Source, 1);
  grid_.place(*combine, layout::Column::Source, 3);
  bke::node_add_link(tree, *color.node, *color.socket, *separate, *sep_vector);
  bke::node_add_link(tree, *separate, *sep_x, *combine, *com_x);
  bke::node_add_link(tree, *flipped.node, *flipped.socket, *combine, *com_y);
  bke::node_add_link(tree, *separate, *sep_z, *combine, *com_z);
  r_nodes.append(separate);
  for (bNode *node : flip_nodes) {
    r_nodes.append(node);
  }
  r_nodes.append(combine);
  return {combine, com_out};
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
  const auto discard = [&]() { this->nodes_discard(tree, nodes); };

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
    this->mapping_apply(tree, group, *mask, *map, nodes);
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
    alpha = this->mapping_clip_alpha(tree, *mask, {map, map_alpha}, nodes);
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
  if (paint_layer_mask_item_inverted(*mask)) {
    /* `1 - C` on the grey only; the alpha stays what the map says, as on the CPU. */
    grey = math_node_add(tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, grey, 1.0f);
    if (grey.node == nullptr) {
      discard();
      return {};
    }
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

  /* The corrections over the base, in order, each the item's own blend of what is below with its
   * grey by `A * opacity`: `M = blend(M, C, A * op)`. The blend is the shader's `ramp_blend` on a
   * grey colour, the one call the CPU makes, with the scalar linked into a colour input and read
   * back from the result. One that lays nothing yet has no node, as it has no pixel on the CPU. */
  NodeOutput current = {mix, result};
  int row = 6;
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0 || !paint_layer_mask_item_lays(item)) {
      continue;
    }
    if (paint_layer_effect_is_filter(item)) {
      /* `M = lerp(M, f(M), opacity)`: the filter works on the coverage so far, as a grey. */
      NodeOutput filtered = current;
      if (this->filter_lay(tree, group, item, -1, filtered)) {
        current = filtered;
      }
      continue;
    }
    if (paint_layer_kind_is_map_coverage(item)) {
      /* `M = M * lerp(1, match, opacity)`: the operator lays its map's match over the coverage so
       * far. A missing map is `match = 0`; the invert flips the match, as on the CPU. */
      bNodeTreeInterfaceSocket **item_opacity_iface = mask_opacity_inputs_.lookup_ptr(&item);
      bNodeSocket *item_opacity = (item_opacity_iface != nullptr) ?
                                      this->group_input_socket(group, **item_opacity_iface) :
                                      nullptr;
      if (item_opacity == nullptr) {
        discard();
        return {};
      }
      NodeOutput match = this->map_coverage_match_add(tree, group, item, nodes);
      if (match.socket == nullptr) {
        discard();
        return {};
      }
      if (paint_layer_mask_item_inverted(item)) {
        match = math_node_add(tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, match, 1.0f);
        if (match.socket == nullptr) {
          discard();
          return {};
        }
      }
      /* Gradient: `match *= value`, the standard value input of the item; White/Black read one. */
      bNodeTreeInterfaceSocket **value_iface = mask_value_inputs_.lookup_ptr(&item);
      bNodeSocket *item_value = (value_iface != nullptr) ?
                                    this->group_input_socket(group, **value_iface) :
                                    nullptr;
      if (item_value == nullptr) {
        discard();
        return {};
      }
      match = math_node_add(
          tree, grid_, nodes, NODE_MATH_MULTIPLY, match, {group.group_input, item_value});
      if (match.socket == nullptr) {
        discard();
        return {};
      }
      bNode *fold = mix_float_node_add(tree);
      bNodeSocket *fold_a = (fold != nullptr) ? socket_in(*fold, "A_Float") : nullptr;
      bNodeSocket *fold_b = (fold != nullptr) ? socket_in(*fold, "B_Float") : nullptr;
      bNodeSocket *fold_factor = (fold != nullptr) ? socket_in(*fold, "Factor_Float") : nullptr;
      bNodeSocket *fold_result = (fold != nullptr) ? socket_out(*fold, "Result_Float") : nullptr;
      if (fold_a == nullptr || fold_b == nullptr || fold_factor == nullptr ||
          fold_result == nullptr)
      {
        if (fold != nullptr) {
          bke::node_remove_node(nullptr, tree, *fold, true, false);
        }
        discard();
        return {};
      }
      nodes.append(fold);
      grid_.place_extra(*fold);
      if (fold_a->default_value != nullptr) {
        static_cast<bNodeSocketValueFloat *>(fold_a->default_value)->value = 1.0f;
      }
      bke::node_add_link(tree, *match.node, *match.socket, *fold, *fold_b);
      bke::node_add_link(tree, *group.group_input, *item_opacity, *fold, *fold_factor);
      const NodeOutput product = this->multiply_nodes_add(
          tree, current, {fold, fold_result});
      if (product.node == nullptr) {
        discard();
        return {};
      }
      nodes.append(product.node);
      current = product;
      continue;
    }
    const MaterialPaintLayerChannel *item_record = paint_layer_channel_find(
        item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    bNodeTreeInterfaceSocket **item_opacity_iface = mask_opacity_inputs_.lookup_ptr(&item);
    bNodeSocket *item_opacity = (item_opacity_iface != nullptr) ?
                                    this->group_input_socket(group, **item_opacity_iface) :
                                    nullptr;
    if (item_record == nullptr || item_opacity == nullptr) {
      continue;
    }
    NodeOutput item_grey;
    NodeOutput item_alpha;
    if (item_record->image != nullptr) {
      bNode *item_map = this->image_map_node_add(tree, *item_record->image);
      if (item_map == nullptr) {
        discard();
        return {};
      }
      nodes.append(item_map);
      grid_.place(*item_map, layout::Column::Mask, row++);
      this->mapping_apply(tree, group, item, *item_map, nodes);
      bNodeSocket *item_color = socket_out(*item_map, "Color");
      bNodeSocket *item_map_alpha = socket_out(*item_map, "Alpha");
      if (item_color == nullptr || item_map_alpha == nullptr) {
        discard();
        return {};
      }
      item_grey = this->rgb_mean_nodes_add(tree, {item_map, item_color}, nodes);
      if (item_grey.node == nullptr) {
        discard();
        return {};
      }
      item_alpha = this->mapping_clip_alpha(tree, item, {item_map, item_map_alpha}, nodes);
    }
    else {
      bNodeTreeInterfaceSocket **item_value_iface = mask_value_inputs_.lookup_ptr(&item);
      bNodeSocket *item_value = (item_value_iface != nullptr) ?
                                    this->group_input_socket(group, **item_value_iface) :
                                    nullptr;
      if (item_value == nullptr) {
        continue;
      }
      item_grey = {group.group_input, item_value};
    }
    if (paint_layer_mask_item_inverted(item)) {
      /* `1 - C` on the grey only, as the CPU reads it: the alpha stays what the map says. */
      item_grey = math_node_add(tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, item_grey, 1.0f);
      if (item_grey.node == nullptr) {
        discard();
        return {};
      }
    }
    NodeOutput item_weight = {group.group_input, item_opacity};
    if (item_alpha.node != nullptr) {
      item_weight = this->multiply_nodes_add(tree, item_alpha, item_weight);
      if (item_weight.node == nullptr) {
        discard();
        return {};
      }
    }
    bNode *blend = mix_node_add(
        tree, BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(item.blend)), 0.0f, 0.0f);
    if (blend == nullptr) {
      discard();
      return {};
    }
    nodes.append(blend);
    grid_.place(*blend, layout::Column::Mask, row++);
    bNodeSocket *blend_a = socket_in(*blend, "A_Color");
    bNodeSocket *blend_b = socket_in(*blend, "B_Color");
    bNodeSocket *blend_factor = socket_in(*blend, "Factor_Float");
    bNodeSocket *blend_result = socket_out(*blend, "Result_Color");
    if (blend_a == nullptr || blend_b == nullptr || blend_factor == nullptr ||
        blend_result == nullptr || item_weight.socket == nullptr || item_grey.socket == nullptr)
    {
      discard();
      return {};
    }
    bke::node_add_link(tree, *current.node, *current.socket, *blend, *blend_a);
    bke::node_add_link(tree, *item_grey.node, *item_grey.socket, *blend, *blend_b);
    bke::node_add_link(tree, *item_weight.node, *item_weight.socket, *blend, *blend_factor);
    current = {blend, blend_result};
  }
  layout::frame_add(tree, "Mask", nodes);

  mask_factors_.add(&layer, current);
  return current;
}

/** A Math node of \a operation over \a a and the constant \a b; null when it cannot be made. */
static bNode *math_constant_node_add(bNodeTree &tree,
                                     layout::LayoutGrid &grid,
                                     Vector<bNode *> &r_nodes,
                                     const int operation,
                                     const NodeOutput a,
                                     const float b)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (node == nullptr) {
    return nullptr;
  }
  bNodeSocket *a_in = socket_in(*node, "Value");
  bNodeSocket *b_in = socket_in(*node, "Value_001");
  if (a_in == nullptr || b_in == nullptr || a.socket == nullptr || b_in->default_value == nullptr) {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return nullptr;
  }
  node->custom1 = operation;
  grid.place_extra(*node);
  bke::node_add_link(tree, *a.node, *a.socket, *node, *a_in);
  static_cast<bNodeSocketValueFloat *>(b_in->default_value)->value = b;
  r_nodes.append(node);
  return node;
}

/** A Value node holding \a value; the null output when it cannot be made. */
static NodeOutput value_node_add(bNodeTree &tree,
                                 layout::LayoutGrid &grid,
                                 Vector<bNode *> &r_nodes,
                                 const float value)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNodeSocket *out = (node != nullptr) ? socket_out(*node, "Value") : nullptr;
  if (out == nullptr || out->default_value == nullptr) {
    if (node != nullptr) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return {};
  }
  static_cast<bNodeSocketValueFloat *>(out->default_value)->value = value;
  grid.place_extra(*node);
  r_nodes.append(node);
  return {node, out};
}

/**
 * A Separate XYZ node over the vector \a vector. \a r_xyz receives its three outputs; false, with
 * nothing left in the tree, when the node cannot be made.
 */
static bool separate_xyz_node_add(bNodeTree &tree,
                                  layout::LayoutGrid &grid,
                                  Vector<bNode *> &r_nodes,
                                  const NodeOutput vector,
                                  NodeOutput r_xyz[3])
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_SEPXYZ);
  if (node == nullptr) {
    return false;
  }
  bNodeSocket *vector_in = socket_in(*node, "Vector");
  bNodeSocket *outs[3] = {socket_out(*node, "X"), socket_out(*node, "Y"), socket_out(*node, "Z")};
  if (vector_in == nullptr || outs[0] == nullptr || outs[1] == nullptr || outs[2] == nullptr ||
      vector.socket == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return false;
  }
  grid.place_extra(*node);
  r_nodes.append(node);
  bke::node_add_link(tree, *vector.node, *vector.socket, *node, *vector_in);
  for (const int i : IndexRange(3)) {
    r_xyz[i] = {node, outs[i]};
  }
  return true;
}

/**
 * A Vector Math node of \a operation whose first input is \a a and, when \a b carries a socket, its
 * second is \a b; null when it cannot be made.
 */
static bNode *vector_math_node_add(bNodeTree &tree,
                                   layout::LayoutGrid &grid,
                                   Vector<bNode *> &r_nodes,
                                   const int operation,
                                   const NodeOutput a,
                                   const NodeOutput b)
{
  bNode *node = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
  bNodeSocket *a_in = (node != nullptr) ? socket_in(*node, "Vector") : nullptr;
  bNodeSocket *b_in = (node != nullptr) ? socket_in(*node, "Vector_001") : nullptr;
  if (node == nullptr || a_in == nullptr || b_in == nullptr || a.socket == nullptr) {
    if (node != nullptr) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return nullptr;
  }
  node->custom1 = operation;
  grid.place_extra(*node);
  bke::node_add_link(tree, *a.node, *a.socket, *node, *a_in);
  if (b.socket != nullptr) {
    bke::node_add_link(tree, *b.node, *b.socket, *node, *b_in);
  }
  r_nodes.append(node);
  return node;
}

/** The island scalar of a UV Island map: its red channel, clamped to 0..1 as the CPU reads it. */
static NodeOutput uv_island_match_add(bNodeTree &tree,
                                      layout::LayoutGrid &grid,
                                      Vector<bNode *> &r_nodes,
                                      const NodeOutput map_color)
{
  NodeOutput xyz[3];
  if (!separate_xyz_node_add(tree, grid, r_nodes, map_color, xyz)) {
    return {};
  }
  /* A byte data map is already within range, this only guards a float one. */
  bNode *lower = math_constant_node_add(tree, grid, r_nodes, NODE_MATH_MAXIMUM, xyz[0], 0.0f);
  if (lower == nullptr) {
    return {};
  }
  bNode *upper = math_constant_node_add(
      tree, grid, r_nodes, NODE_MATH_MINIMUM, {lower, socket_out(*lower, "Value")}, 1.0f);
  return (upper != nullptr) ? NodeOutput{upper, socket_out(*upper, "Value")} : NodeOutput{};
}

/** The max-channel distance `max(|a.r-b.r|, |a.g-b.g|, |a.b-b.b|)` of two colours, as the CPU. */
static NodeOutput color_distance_add(bNodeTree &tree,
                                     layout::LayoutGrid &grid,
                                     Vector<bNode *> &r_nodes,
                                     const NodeOutput a,
                                     const NodeOutput b)
{
  bNode *diff = vector_math_node_add(tree, grid, r_nodes, NODE_VECTOR_MATH_SUBTRACT, a, b);
  bNodeSocket *diff_out = (diff != nullptr) ? socket_out(*diff, "Vector") : nullptr;
  if (diff_out == nullptr) {
    return {};
  }
  bNode *abs_node = vector_math_node_add(
      tree, grid, r_nodes, NODE_VECTOR_MATH_ABSOLUTE, {diff, diff_out}, {});
  bNodeSocket *abs_out = (abs_node != nullptr) ? socket_out(*abs_node, "Vector") : nullptr;
  if (abs_out == nullptr) {
    return {};
  }
  NodeOutput xyz[3];
  if (!separate_xyz_node_add(tree, grid, r_nodes, {abs_node, abs_out}, xyz)) {
    return {};
  }
  const NodeOutput dist_xy = math_node_add(tree, grid, r_nodes, NODE_MATH_MAXIMUM, xyz[0], xyz[1]);
  if (dist_xy.socket == nullptr) {
    return {};
  }
  return math_node_add(tree, grid, r_nodes, NODE_MATH_MAXIMUM, dist_xy, xyz[2]);
}

/**
 * `1 - smoothstep(tolerance, falloff_end, dist)`: one inside the tolerance and zero past the falloff,
 * which is the CPU's match. Map Range does the ramp, with its output flipped.
 */
static NodeOutput falloff_match_add(bNodeTree &tree,
                                    layout::LayoutGrid &grid,
                                    Vector<bNode *> &r_nodes,
                                    const NodeOutput dist,
                                    const NodeOutput tolerance,
                                    const NodeOutput falloff_end)
{
  bNode *range = bke::node_add_static_node(nullptr, tree, SH_NODE_MAP_RANGE);
  bNodeSocket *value = (range != nullptr) ? socket_in(*range, "Value") : nullptr;
  bNodeSocket *from_min = (range != nullptr) ? socket_in(*range, "From Min") : nullptr;
  bNodeSocket *from_max = (range != nullptr) ? socket_in(*range, "From Max") : nullptr;
  bNodeSocket *to_min = (range != nullptr) ? socket_in(*range, "To Min") : nullptr;
  bNodeSocket *to_max = (range != nullptr) ? socket_in(*range, "To Max") : nullptr;
  bNodeSocket *result = (range != nullptr) ? socket_out(*range, "Result") : nullptr;
  if (value == nullptr || from_min == nullptr || from_max == nullptr || to_min == nullptr ||
      to_max == nullptr || result == nullptr || to_min->default_value == nullptr ||
      to_max->default_value == nullptr)
  {
    if (range != nullptr) {
      bke::node_remove_node(nullptr, tree, *range, true, false);
    }
    return {};
  }
  if (NodeMapRange *storage = static_cast<NodeMapRange *>(range->storage)) {
    storage->data_type = CD_PROP_FLOAT;
    storage->interpolation_type = NODE_MAP_RANGE_SMOOTHSTEP;
    storage->clamp = 1;
  }
  grid.place_extra(*range);
  r_nodes.append(range);
  bke::node_add_link(tree, *dist.node, *dist.socket, *range, *value);
  bke::node_add_link(tree, *tolerance.node, *tolerance.socket, *range, *from_min);
  bke::node_add_link(tree, *falloff_end.node, *falloff_end.socket, *range, *from_max);
  static_cast<bNodeSocketValueFloat *>(to_min->default_value)->value = 1.0f;
  static_cast<bNodeSocketValueFloat *>(to_max->default_value)->value = 0.0f;
  return {range, result};
}

NodeOutput PaintLayersTreeBuilder::map_coverage_match_add(bNodeTree &tree,
                                                          LayerGroup &group,
                                                          const MaterialPaintLayer &item,
                                                          Vector<bNode *> &r_nodes)
{
  /* A constant zero is `match = 0` for a missing map or an empty colour list: the operator still
   * takes part and restricts everything, exactly as the CPU reads it. */
  if (!paint_layer_map_coverage_has_map(item)) {
    return value_node_add(tree, grid_, r_nodes, 0.0f);
  }
  const MaterialPaintLayerChannel *record = paint_layer_map_coverage_record(item);

  bNode *map = this->image_map_node_add(tree, *record->image);
  if (map == nullptr) {
    return {};
  }
  r_nodes.append(map);
  grid_.place_extra(*map);
  this->mapping_apply(tree, group, item, *map, r_nodes);
  /* The ID map is a lookup, not a gradient: nearest texel, so a texel matches only its own colour,
   * as the picker and the CPU read it. */
  if (NodeTexImage *storage = static_cast<NodeTexImage *>(map->storage)) {
    storage->interpolation = SHD_INTERP_CLOSEST;
  }
  bNodeSocket *map_color_socket = socket_out(*map, "Color");
  if (map_color_socket == nullptr) {
    return {};
  }
  const NodeOutput map_color = {map, map_color_socket};

  if (item.kind_subtype == MA_PAINT_LAYER_FILTER_UV_ISLAND) {
    return uv_island_match_add(tree, grid_, r_nodes, map_color);
  }

  /* Color ID: Simple reads the item's shared tolerance and softness; Advanced gives every colour
   * its own tolerance and a hard edge. */
  const bool advanced = paint_layer_mask_item_color_id_advanced(item);
  const auto param_input = [&](const int index) -> bNodeSocket * {
    Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&item);
    if (params == nullptr || index >= params->size() || (*params)[index] == nullptr) {
      return nullptr;
    }
    return this->group_input_socket(group, *(*params)[index]);
  };
  NodeOutput tol;
  NodeOutput falloff_end;
  if (!advanced) {
    bNodeSocket *tol_socket = param_input(PAINT_LAYER_COVERAGE_TOLERANCE);
    bNodeSocket *soft_socket = param_input(PAINT_LAYER_COVERAGE_SOFTNESS);
    if (tol_socket == nullptr || soft_socket == nullptr) {
      return {};
    }
    tol = {group.group_input, tol_socket};
    const NodeOutput soft = {group.group_input, soft_socket};
    /* A zero softness is a hard edge on the CPU; Map Range divides by the range, so floor it to a
     * step far below one byte, which keeps the two edges apart without changing a byte result. */
    bNode *soft_floor = math_constant_node_add(
        tree, grid_, r_nodes, NODE_MATH_MAXIMUM, soft, 1.0f / 65536.0f);
    if (soft_floor == nullptr) {
      return {};
    }
    falloff_end = math_node_add(
        tree, grid_, r_nodes, NODE_MATH_ADD, tol, {soft_floor, socket_out(*soft_floor, "Value")});
    if (falloff_end.socket == nullptr) {
      return {};
    }
  }

  Vector<IdColorInputs> *inputs = id_color_inputs_.lookup_ptr(&item);
  NodeOutput match;
  for (const IdColorInputs &entry : (inputs != nullptr) ? inputs->as_span() : Span<IdColorInputs>()) {
    bNodeSocket *color_socket = (entry.color != nullptr) ?
                                    this->group_input_socket(group, *entry.color) :
                                    nullptr;
    bNodeSocket *enabled_socket = (entry.enabled != nullptr) ?
                                      this->group_input_socket(group, *entry.enabled) :
                                      nullptr;
    if (color_socket == nullptr || enabled_socket == nullptr) {
      return {};
    }
    NodeOutput entry_tol = tol;
    NodeOutput entry_falloff_end = falloff_end;
    if (advanced) {
      bNodeSocket *entry_tol_socket = (entry.tolerance != nullptr) ?
                                          this->group_input_socket(group, *entry.tolerance) :
                                          nullptr;
      if (entry_tol_socket == nullptr) {
        return {};
      }
      entry_tol = {group.group_input, entry_tol_socket};
      /* A hard edge: Map Range needs two distinct ends, so add a step far below one byte. */
      bNode *end = math_constant_node_add(
          tree, grid_, r_nodes, NODE_MATH_ADD, entry_tol, 1.0f / 65536.0f);
      if (end == nullptr) {
        return {};
      }
      entry_falloff_end = {end, socket_out(*end, "Value")};
    }
    const NodeOutput dist = color_distance_add(
        tree, grid_, r_nodes, map_color, {group.group_input, color_socket});
    if (dist.socket == nullptr) {
      return {};
    }
    const NodeOutput entry_match = falloff_match_add(
        tree, grid_, r_nodes, dist, entry_tol, entry_falloff_end);
    if (entry_match.socket == nullptr) {
      return {};
    }
    /* A disabled colour contributes nothing, so it neither matches nor lifts the maximum. */
    const NodeOutput gate = this->multiply_nodes_add(
        tree, entry_match, {group.group_input, enabled_socket});
    if (gate.node == nullptr) {
      return {};
    }
    r_nodes.append(gate.node);
    match = (match.socket == nullptr) ?
                gate :
                math_node_add(tree, grid_, r_nodes, NODE_MATH_MAXIMUM, match, gate);
    if (match.socket == nullptr) {
      return {};
    }
  }
  return (match.socket != nullptr) ? match : value_node_add(tree, grid_, r_nodes, 0.0f);
}

NodeOutput PaintLayersTreeBuilder::object_mask_visible_ensure(bNodeTree &tree,
                                                              const MaterialPaintLayer &layer)
{
  if ((layer.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) == 0) {
    return {};
  }
  if (const NodeOutput *cached = object_mask_visible_.lookup_ptr(&layer)) {
    return *cached;
  }

  /* Every node this function adds, so that a failure part way leaves nothing half built behind. */
  Vector<bNode *> nodes;
  const auto discard = [&]() {
    this->nodes_discard(tree, nodes);
    return NodeOutput{};
  };
  /* Math over \a a and the constant \a b, as a #NodeOutput. */
  const auto with_constant = [&](const int operation, const NodeOutput a, const float b) {
    bNode *node = math_constant_node_add(tree, grid_, nodes, operation, a, b);
    return (node != nullptr) ? NodeOutput{node, socket_out(*node, "Value")} : NodeOutput{};
  };

  /* A missing record reads 0 ("nothing hidden"), which is why records store exclusions. The record
   * is resolved per object by the Attribute node, so one tree serves every object using the
   * material. */
  bNode *attribute = bke::node_add_static_node(nullptr, tree, SH_NODE_ATTRIBUTE);
  if (attribute == nullptr) {
    return discard();
  }
  nodes.append(attribute);
  grid_.place_extra(*attribute);
  NodeShaderAttribute *attribute_data = static_cast<NodeShaderAttribute *>(attribute->storage);
  attribute_data->type = SHD_ATTRIBUTE_OBJECT;
  STRNCPY(attribute_data->name,
          BKE_paint_layer_object_mask_attribute_path(ma_, layer).c_str());
  NodeOutput excluded = {attribute, socket_out(*attribute, "Fac")};
  if (excluded.socket == nullptr) {
    return discard();
  }

  if (layer.object_mask_type == MA_PAINT_LAYER_OBJECT_MASK_UV_TILES) {
    /* UDIM standard: 10 columns per row; the mask covers FIRST..FIRST+NUM-1. The UV is the one the
     * layer's maps read, before any row mapping, because tiles are a property of the mesh. */
    constexpr float udim_columns = float(bke::PAINT_LAYER_OBJECT_MASK_UDIM_COLUMNS);
    constexpr float tile_last_exclusive = float(bke::PAINT_LAYER_OBJECT_MASK_TILE_NUM) - 0.5f;
    bNode *coord = this->uv_coordinate_ensure(tree);
    bNodeSocket *uv = (coord != nullptr) ? socket_out(*coord, "UV") : nullptr;
    bNode *separate = bke::node_add_static_node(nullptr, tree, SH_NODE_SEPXYZ);
    if (uv == nullptr || separate == nullptr) {
      return discard();
    }
    nodes.append(separate);
    grid_.place_extra(*separate);
    bke::node_add_link(tree, *coord, *uv, *separate, *socket_in(*separate, "Vector"));

    const NodeOutput column = with_constant(
        NODE_MATH_FLOOR, {separate, socket_out(*separate, "X")}, 0.0f);
    const NodeOutput row = with_constant(
        NODE_MATH_FLOOR, {separate, socket_out(*separate, "Y")}, 0.0f);
    const NodeOutput row_start = with_constant(NODE_MATH_MULTIPLY, row, udim_columns);
    const NodeOutput index = math_node_add(
        tree, grid_, nodes, NODE_MATH_ADD, column, row_start, 0.0f);
    /* The bit is `floor(bits / 2^index) mod 2`, exact for 24 bits in a float. */
    const NodeOutput power = math_node_add(
        tree, grid_, nodes, NODE_MATH_POWER, {}, index, 2.0f);
    const NodeOutput shifted = math_node_add(
        tree, grid_, nodes, NODE_MATH_DIVIDE, excluded, power, 0.0f);
    const NodeOutput floored = with_constant(NODE_MATH_FLOOR, shifted, 0.0f);
    const NodeOutput bit = with_constant(NODE_MATH_FLOORED_MODULO, floored, 2.0f);
    /* Tiles outside FIRST..FIRST+NUM-1 are never hidden: the column is 0..9 and the index
     * 0..NUM-1. */
    const NodeOutput column_below = with_constant(NODE_MATH_LESS_THAN, column, udim_columns);
    const NodeOutput column_above = with_constant(NODE_MATH_GREATER_THAN, column, -0.5f);
    const NodeOutput index_below = with_constant(
        NODE_MATH_LESS_THAN, index, tile_last_exclusive);
    const NodeOutput index_above = with_constant(NODE_MATH_GREATER_THAN, index, -0.5f);
    const NodeOutput column_ok = math_node_add(
        tree, grid_, nodes, NODE_MATH_MULTIPLY, column_below, column_above, 0.0f);
    const NodeOutput index_ok = math_node_add(
        tree, grid_, nodes, NODE_MATH_MULTIPLY, index_below, index_above, 0.0f);
    const NodeOutput in_range = math_node_add(
        tree, grid_, nodes, NODE_MATH_MULTIPLY, column_ok, index_ok, 0.0f);
    excluded = math_node_add(tree, grid_, nodes, NODE_MATH_MULTIPLY, bit, in_range, 0.0f);
    /* The Math helper reads a missing first operand as its default, so a part that failed has to
     * be caught here rather than left to link a wrong chain. */
    for (const NodeOutput &part : {column, row, row_start, index, power, shifted, floored, bit,
                                   column_below, column_above, index_below, index_above,
                                   column_ok, index_ok, in_range, excluded})
    {
      if (part.socket == nullptr) {
        return discard();
      }
    }
  }

  /* `visible = 1 - excluded`. */
  const NodeOutput visible = math_node_add(
      tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, excluded, 1.0f);
  if (visible.socket == nullptr) {
    return discard();
  }
  layout::frame_add(tree, "Object Mask", nodes);
  object_mask_visible_.add(&layer, visible);
  return visible;
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

/**
 * A Math node of \a operation over \a a and \a b: a null socket of \a a reads \a a_default instead.
 * Empty when the node cannot be made, in which case nothing is left behind.
 */
static NodeOutput math_node_add(bNodeTree &tree,
                                layout::LayoutGrid &grid,
                                Vector<bNode *> &r_nodes,
                                const int operation,
                                const NodeOutput a,
                                const NodeOutput b,
                                const float a_default)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (node == nullptr) {
    return {};
  }
  bNodeSocket *a_in = socket_in(*node, "Value");
  bNodeSocket *b_in = socket_in(*node, "Value_001");
  bNodeSocket *out = socket_out(*node, "Value");
  if (a_in == nullptr || b_in == nullptr || out == nullptr || b.socket == nullptr) {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  node->custom1 = operation;
  grid.place_extra(*node);
  if (a.socket != nullptr) {
    bke::node_add_link(tree, *a.node, *a.socket, *node, *a_in);
  }
  else if (a_in->default_value != nullptr) {
    static_cast<bNodeSocketValueFloat *>(a_in->default_value)->value = a_default;
  }
  bke::node_add_link(tree, *b.node, *b.socket, *node, *b_in);
  r_nodes.append(node);
  return {node, out};
}

bool PaintLayersTreeBuilder::filter_lay(bNodeTree &tree,
                                        LayerGroup &group,
                                        const MaterialPaintLayer &effect,
                                        const int channel,
                                        NodeOutput &content,
                                        const bool full_strength)
{
  /* A negative channel is a filter in a mask: it works on the mask's one coverage, in no channel. */
  const bool in_mask = channel < 0;
  if (content.socket == nullptr || (!in_mask && !paint_layer_effect_filters(ma_, effect, channel)))
  {
    return false;
  }
  /* How much of the filter's result replaces the colour. A correction has an opacity of its own; an
   * adjustment is applied in full and the row's own factor and blend lay it over the stack. */
  NodeOutput strength;
  if (in_mask) {
    bNodeTreeInterfaceSocket **mask_opacity = mask_opacity_inputs_.lookup_ptr(&effect);
    bNodeSocket *opacity = (mask_opacity != nullptr) ?
                               group_input_socket(group, **mask_opacity) :
                               nullptr;
    if (opacity == nullptr) {
      return false;
    }
    strength = {group.group_input, opacity};
  }
  else if (full_strength) {
    bNode *one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
    bNodeSocket *one_out = (one != nullptr) ? socket_out(*one, "Value") : nullptr;
    if (one_out == nullptr || one_out->default_value == nullptr) {
      if (one != nullptr) {
        bke::node_remove_node(nullptr, tree, *one, true, false);
      }
      return false;
    }
    static_cast<bNodeSocketValueFloat *>(one_out->default_value)->value = 1.0f;
    grid_.place_extra(*one);
    strength = {one, one_out};
  }
  else {
    Map<int, bNodeTreeInterfaceSocket *> *by_channel = effect_opacity_inputs_.lookup_ptr(&effect);
    if (by_channel == nullptr) {
      return false;
    }
    bNodeTreeInterfaceSocket **opacity_iface = by_channel->lookup_ptr(channel);
    bNodeSocket *opacity = (opacity_iface != nullptr) ?
                               group_input_socket(group, **opacity_iface) :
                               nullptr;
    if (opacity == nullptr) {
      return false;
    }
    strength = {group.group_input, opacity};
  }

  switch (eMaterialPaintLayerFilter(effect.kind_subtype)) {
    case MA_PAINT_LAYER_FILTER_INVERT: {
      /* `lerp(c, |c - 1|, opacity)`: Difference against white is `1 - c` on [0, 1] and stays
       * symmetric beyond it, which is the formula the CPU composite applies. */
      bNode *mix = mix_node_add(tree, MA_RAMP_DIFF, 0.0f, 0.0f);
      bNodeSocket *a = (mix != nullptr) ? socket_in(*mix, "A_Color") : nullptr;
      bNodeSocket *b = (mix != nullptr) ? socket_in(*mix, "B_Color") : nullptr;
      bNodeSocket *fac = (mix != nullptr) ? socket_in(*mix, "Factor_Float") : nullptr;
      bNodeSocket *out = (mix != nullptr) ? socket_out(*mix, "Result_Color") : nullptr;
      if (a == nullptr || b == nullptr || fac == nullptr || out == nullptr ||
          b->default_value == nullptr)
      {
        if (mix != nullptr) {
          bke::node_remove_node(nullptr, tree, *mix, true, false);
        }
        return false;
      }
      const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(b->default_value)->value, white);
      grid_.place_extra(*mix);
      bke::node_add_link(tree, *content.node, *content.socket, *mix, *a);
      bke::node_add_link(tree, *strength.node, *strength.socket, *mix, *fac);
      content = {mix, out};
      Vector<bNode *> framed;
      framed.append(mix);
      layout::frame_add(tree, effect.name, framed);
      return true;
    }
    case MA_PAINT_LAYER_FILTER_HSL: {
      /* One Hue/Saturation/Value node: the parameters feed its inputs and the filter's opacity its
       * own factor, so the node mixes the result with the colour it read. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_HSL_PARAM_NUM) {
        return false;
      }
      bNode *hsv = bke::node_add_static_node(nullptr, tree, SH_NODE_HUE_SAT);
      bNodeSocket *color_in = (hsv != nullptr) ? socket_in(*hsv, "Color") : nullptr;
      bNodeSocket *fac_in = (hsv != nullptr) ? socket_in(*hsv, "Fac") : nullptr;
      bNodeSocket *color_out = (hsv != nullptr) ? socket_out(*hsv, "Color") : nullptr;
      const char *const names[PAINT_LAYER_HSL_PARAM_NUM] = {"Hue", "Saturation", "Value"};
      bool linked = color_in != nullptr && fac_in != nullptr && color_out != nullptr;
      for (const int i : IndexRange(PAINT_LAYER_HSL_PARAM_NUM)) {
        bNodeSocket *param_in = linked ? socket_in(*hsv, names[i]) : nullptr;
        bNodeSocket *param_out = ((*params)[i] != nullptr) ?
                                     group_input_socket(group, *(*params)[i]) :
                                     nullptr;
        if (param_in == nullptr || param_out == nullptr) {
          linked = false;
          break;
        }
        bke::node_add_link(tree, *group.group_input, *param_out, *hsv, *param_in);
      }
      if (!linked) {
        if (hsv != nullptr) {
          bke::node_remove_node(nullptr, tree, *hsv, true, false);
        }
        return false;
      }
      grid_.place_extra(*hsv);
      bke::node_add_link(tree, *content.node, *content.socket, *hsv, *color_in);
      bke::node_add_link(tree, *strength.node, *strength.socket, *hsv, *fac_in);
      content = {hsv, color_out};
      Vector<bNode *> framed;
      framed.append(hsv);
      layout::frame_add(tree, effect.name, framed);
      return true;
    }
    case MA_PAINT_LAYER_FILTER_LEVELS: {
      /* `lerp(c, ob + clamp(c * k + m, 0, 1)^(1 / gamma) * (ow - ob), opacity)` with
       * `k = 1 / (iw - ib)` and `m = -ib * k`: the function the CPU composite applies, built from
       * the parameters as inputs so an edit of one is a value and not a different graph. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_LEVELS_PARAM_NUM) {
        return false;
      }
      NodeOutput param[PAINT_LAYER_LEVELS_PARAM_NUM];
      for (const int i : IndexRange(PAINT_LAYER_LEVELS_PARAM_NUM)) {
        if ((*params)[i] == nullptr) {
          return false;
        }
        param[i] = {group.group_input, group_input_socket(group, *(*params)[i])};
        if (param[i].socket == nullptr) {
          return false;
        }
      }

      Vector<bNode *> nodes;
      const auto discard = [&]() {
        for (bNode *node : nodes) {
          bke::node_remove_node(nullptr, tree, *node, true, false);
        }
        return false;
      };
      const NodeOutput span = math_node_add(tree,
                                            grid_,
                                            nodes,
                                            NODE_MATH_SUBTRACT,
                                            param[PAINT_LAYER_LEVELS_IN_WHITE],
                                            param[PAINT_LAYER_LEVELS_IN_BLACK]);
      const NodeOutput k = math_node_add(tree, grid_, nodes, NODE_MATH_DIVIDE, {}, span, 1.0f);
      const NodeOutput black_k = math_node_add(
          tree, grid_, nodes, NODE_MATH_MULTIPLY, param[PAINT_LAYER_LEVELS_IN_BLACK], k);
      const NodeOutput m = math_node_add(tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, black_k, 0.0f);
      const NodeOutput inv_gamma = math_node_add(
          tree, grid_, nodes, NODE_MATH_DIVIDE, {}, param[PAINT_LAYER_LEVELS_GAMMA], 1.0f);
      const NodeOutput out_span = math_node_add(tree,
                                                grid_,
                                                nodes,
                                                NODE_MATH_SUBTRACT,
                                                param[PAINT_LAYER_LEVELS_OUT_WHITE],
                                                param[PAINT_LAYER_LEVELS_OUT_BLACK]);
      if (span.node == nullptr || k.node == nullptr || black_k.node == nullptr ||
          m.node == nullptr || inv_gamma.node == nullptr || out_span.node == nullptr)
      {
        return discard();
      }

      /* A Vector Math node whose first input is \a a; the others are linked or left at their
       * defaults by the caller. A float output feeding a vector input is splat to all three. */
      const auto vector_node = [&](const int operation, const NodeOutput a) -> bNode * {
        bNode *node = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
        bNodeSocket *vec = (node != nullptr) ? socket_in(*node, "Vector") : nullptr;
        if (vec == nullptr || a.socket == nullptr) {
          if (node != nullptr) {
            bke::node_remove_node(nullptr, tree, *node, true, false);
          }
          return nullptr;
        }
        node->custom1 = operation;
        grid_.place_extra(*node);
        bke::node_add_link(tree, *a.node, *a.socket, *node, *vec);
        nodes.append(node);
        return node;
      };
      const auto link_to = [&](bNode &node, const char *name, const NodeOutput from) {
        bNodeSocket *socket = socket_in(node, name);
        if (socket == nullptr || from.socket == nullptr) {
          return false;
        }
        bke::node_add_link(tree, *from.node, *from.socket, node, *socket);
        return true;
      };

      bNode *scaled = vector_node(NODE_VECTOR_MATH_MULTIPLY_ADD, content);
      if (scaled == nullptr || !link_to(*scaled, "Vector_001", k) ||
          !link_to(*scaled, "Vector_002", m))
      {
        return discard();
      }
      bNode *lower = vector_node(NODE_VECTOR_MATH_MAXIMUM, {scaled, socket_out(*scaled, "Vector")});
      bNode *upper = (lower != nullptr) ?
                         vector_node(NODE_VECTOR_MATH_MINIMUM,
                                     {lower, socket_out(*lower, "Vector")}) :
                         nullptr;
      bNodeSocket *upper_limit = (upper != nullptr) ? socket_in(*upper, "Vector_001") : nullptr;
      if (upper_limit == nullptr || upper_limit->default_value == nullptr) {
        return discard();
      }
      copy_v3_fl(static_cast<bNodeSocketValueVector *>(upper_limit->default_value)->value, 1.0f);

      bNode *gamma = bke::node_add_static_node(nullptr, tree, SH_NODE_GAMMA);
      if (gamma == nullptr) {
        return discard();
      }
      nodes.append(gamma);
      grid_.place_extra(*gamma);
      bNodeSocket *gamma_color = socket_in(*gamma, "Color");
      bNodeSocket *gamma_out = socket_out(*gamma, "Color");
      if (gamma_color == nullptr || gamma_out == nullptr ||
          !link_to(*gamma, "Gamma", inv_gamma))
      {
        return discard();
      }
      bke::node_add_link(tree, *upper, *socket_out(*upper, "Vector"), *gamma, *gamma_color);

      bNode *mapped = vector_node(NODE_VECTOR_MATH_MULTIPLY_ADD, {gamma, gamma_out});
      if (mapped == nullptr || !link_to(*mapped, "Vector_001", out_span) ||
          !link_to(*mapped, "Vector_002", param[PAINT_LAYER_LEVELS_OUT_BLACK]))
      {
        return discard();
      }

      bNode *mix = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
      bNodeSocket *a = (mix != nullptr) ? socket_in(*mix, "A_Color") : nullptr;
      bNodeSocket *b = (mix != nullptr) ? socket_in(*mix, "B_Color") : nullptr;
      bNodeSocket *fac = (mix != nullptr) ? socket_in(*mix, "Factor_Float") : nullptr;
      bNodeSocket *out = (mix != nullptr) ? socket_out(*mix, "Result_Color") : nullptr;
      if (out == nullptr || a == nullptr || b == nullptr || fac == nullptr) {
        if (mix != nullptr) {
          bke::node_remove_node(nullptr, tree, *mix, true, false);
        }
        return discard();
      }
      nodes.append(mix);
      grid_.place_extra(*mix);
      bke::node_add_link(tree, *content.node, *content.socket, *mix, *a);
      bke::node_add_link(tree, *mapped, *socket_out(*mapped, "Vector"), *mix, *b);
      bke::node_add_link(tree, *strength.node, *strength.socket, *mix, *fac);
      content = {mix, out};
      layout::frame_add(tree, effect.name, nodes);
      return true;
    }
    case MA_PAINT_LAYER_FILTER_COLOR_ID:
    case MA_PAINT_LAYER_FILTER_UV_ISLAND:
      /* Map-coverage operators only exist in a mask, where #map_coverage_match_add builds them. */
      break;
  }
  return false;
}

bool PaintLayersTreeBuilder::correction_lay(bNodeTree &tree,
                                            LayerGroup &group,
                                            const MaterialPaintLayer &effect,
                                            const int channel,
                                            NodeOutput &content,
                                            NodeOutput &coverage,
                                            NodeOutput *content_alpha)
{
  if (paint_layer_effect_is_filter(effect)) {
    /* A filter lays no coverage and no content alpha: those stay as the row has them. */
    return this->filter_lay(tree, group, effect, channel, content);
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(effect, channel);
  Map<int, bNodeTreeInterfaceSocket *> *by_channel = effect_opacity_inputs_.lookup_ptr(&effect);
  /* A Fill correction with no map in the channel is a flat colour, an input of the group; every
   * other correction reads its map. */
  const bool flat = record != nullptr && record->image == nullptr &&
                    paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_FLAT_VALUE);
  if (record == nullptr || (!flat && record->image == nullptr) || by_channel == nullptr ||
      content.socket == nullptr)
  {
    return false;
  }
  bNodeTreeInterfaceSocket **opacity_iface = by_channel->lookup_ptr(channel);
  bNodeSocket *opacity = (opacity_iface != nullptr) ? group_input_socket(group, **opacity_iface) :
                                                      nullptr;
  bNode *map = nullptr;
  bNodeSocket *map_color = nullptr;
  bNodeSocket *map_alpha = nullptr;
  if (flat) {
    Map<int, bNodeTreeInterfaceSocket *> *fill_by_channel = fill_inputs_.lookup_ptr(&effect);
    bNodeTreeInterfaceSocket **fill_iface = (fill_by_channel != nullptr) ?
                                                fill_by_channel->lookup_ptr(channel) :
                                                nullptr;
    if (fill_iface == nullptr || opacity == nullptr) {
      return false;
    }
    map_color = group_input_socket(group, **fill_iface);
    if (map_color == nullptr) {
      return false;
    }
  }
  else {
    map = this->image_map_node_add(tree, *record->image);
    map_color = (map != nullptr) ? socket_out(*map, "Color") : nullptr;
    map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
    if (opacity == nullptr || map_color == nullptr || map_alpha == nullptr) {
      if (map != nullptr) {
        bke::node_remove_node(nullptr, tree, *map, true, false);
      }
      return false;
    }
    grid_.place_extra(*map);
  }
  Vector<bNode *> nodes;
  if (map != nullptr) {
    nodes.append(map);
  }
  /* A Fill correction's own map reads through the correction's Mapping while it applies. */
  const bool mapped = map != nullptr && this->mapping_apply(tree, group, effect, *map, nodes);
  /* Nothing of the correction is linked to the chain until it is complete, so a failure removes
   * what was made and leaves the row as it was. */
  const auto discard = [&]() {
    this->nodes_discard(tree, nodes);
    return false;
  };
  /* The map is read as stored, like a row's own map: a colour map is un-premultiplied by the node,
   * and a data map follows the same rule a row's does, so the two cannot disagree. */
  NodeOutput color = {flat ? group.group_input : map, map_color};
  if (mapped && channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* The Mapping moved only the read point; the tangent-space vectors follow it. */
    color = this->normal_remap_add(tree, effect, color, &nodes);
  }
  NodeOutput alpha = {map, map_alpha};
  if (mapped) {
    /* A clipping UV wrap takes the part outside the square out of the map's alpha. */
    alpha = this->mapping_clip_alpha(tree, effect, alpha, nodes);
  }

  /* The factor sits in its own `Opacity x Factor` frame, so it stays out of this correction's. */
  bNode *factor_node_for_frame = nullptr;
  const auto frame = [&]() {
    Vector<bNode *> framed;
    for (bNode *node : nodes) {
      if (node != factor_node_for_frame) {
        framed.append(node);
      }
    }
    layout::frame_add(tree, effect.name, framed);
  };
  /* The correction's own factor: its opacity times the map's alpha. A flat colour covers by its
   * opacity alone, as a Fill row does. */
  NodeOutput factor = {group.group_input, opacity};
  if (!flat) {
    factor = this->multiply_nodes_add(tree, {group.group_input, opacity}, alpha);
    if (factor.node == nullptr) {
      return discard();
    }
    nodes.append(factor.node);
    factor_node_for_frame = factor.node;
  }

  /* The correction raises the row's content alpha by the over model, `a = a + f * (1 - a)`,
   * independently of the colour: the Mix below would lerp the alpha component instead. */
  NodeOutput alpha_joined;
  if (content_alpha != nullptr && content_alpha->socket != nullptr) {
    const NodeOutput alpha_one_minus = math_node_add(
        tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, *content_alpha, 1.0f);
    const NodeOutput alpha_scaled = math_node_add(
        tree, grid_, nodes, NODE_MATH_MULTIPLY, factor, alpha_one_minus);
    alpha_joined = math_node_add(
        tree, grid_, nodes, NODE_MATH_ADD, *content_alpha, alpha_scaled);
    if (alpha_joined.node == nullptr) {
      return discard();
    }
  }

  const BlendNode blend = this->blend_node_add(
      tree, channel, BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(effect.blend)));
  if (blend.node == nullptr) {
    return discard();
  }
  nodes.append(blend.node);

  if (coverage.socket == nullptr) {
    /* The row covers fully, so the correction is a plain blend by its factor. */
    blend_inputs_link(tree, blend, content, color, factor);
    content = {blend.node, blend.result};
    if (alpha_joined.node != nullptr) {
      *content_alpha = alpha_joined;
    }
    frame();
    return true;
  }

  /* The row has coverage `a` of its own, which may be partial or zero. A texel it does not cover
   * has no colour of its own, so the correction takes over there instead of mixing with the empty
   * (black) one:
   *   c_eff = lerp(c, blend(S, c), a)
   *   a'    = a + f * (1 - a)
   *   S'    = lerp(S, c_eff, f / a')
   * which is what the CPU composite computes per pixel, and equals the plain blend by f where
   * `a` is one. */
  NodeOutput below = content;
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* The Normal Combine decodes its first input as a tangent normal, and where the row has no
     * content its colour is the map's empty black, which decodes to `(-1, -1, -1)`: the flat normal
     * stands in by the coverage, as in the CPU. */
    bNode *flat = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
    if (flat != nullptr) {
      nodes.append(flat);
    }
    bNodeSocket *flat_a = (flat != nullptr) ? socket_in(*flat, "A_Color") : nullptr;
    bNodeSocket *flat_b = (flat != nullptr) ? socket_in(*flat, "B_Color") : nullptr;
    bNodeSocket *flat_fac = (flat != nullptr) ? socket_in(*flat, "Factor_Float") : nullptr;
    bNodeSocket *flat_out = (flat != nullptr) ? socket_out(*flat, "Result_Color") : nullptr;
    if (flat_a == nullptr || flat_b == nullptr || flat_fac == nullptr || flat_out == nullptr ||
        flat_a->default_value == nullptr)
    {
      return discard();
    }
    grid_.place_extra(*flat);
    const float flat_normal[4] = {0.5f, 0.5f, 1.0f, 1.0f};
    copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(flat_a->default_value)->value, flat_normal);
    bke::node_add_link(tree, *content.node, *content.socket, *flat, *flat_b);
    bke::node_add_link(tree, *coverage.node, *coverage.socket, *flat, *flat_fac);
    below = {flat, flat_out};
  }
  /* The blend at full weight; the weight is applied once, below, over the coverage. */
  blend_inputs_link(tree, blend, below, color, {});

  bNode *c_eff = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
  bNode *lay = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
  for (bNode *node : {c_eff, lay}) {
    if (node != nullptr) {
      nodes.append(node);
      grid_.place_extra(*node);
    }
  }
  bNodeSocket *eff_a = (c_eff != nullptr) ? socket_in(*c_eff, "A_Color") : nullptr;
  bNodeSocket *eff_b = (c_eff != nullptr) ? socket_in(*c_eff, "B_Color") : nullptr;
  bNodeSocket *eff_fac = (c_eff != nullptr) ? socket_in(*c_eff, "Factor_Float") : nullptr;
  bNodeSocket *eff_out = (c_eff != nullptr) ? socket_out(*c_eff, "Result_Color") : nullptr;
  bNodeSocket *lay_a = (lay != nullptr) ? socket_in(*lay, "A_Color") : nullptr;
  bNodeSocket *lay_b = (lay != nullptr) ? socket_in(*lay, "B_Color") : nullptr;
  bNodeSocket *lay_fac = (lay != nullptr) ? socket_in(*lay, "Factor_Float") : nullptr;
  bNodeSocket *lay_out = (lay != nullptr) ? socket_out(*lay, "Result_Color") : nullptr;
  if (eff_a == nullptr || eff_b == nullptr || eff_fac == nullptr || eff_out == nullptr ||
      lay_a == nullptr || lay_b == nullptr || lay_fac == nullptr || lay_out == nullptr)
  {
    return discard();
  }
  bke::node_add_link(tree, *color.node, *color.socket, *c_eff, *eff_a);
  bke::node_add_link(tree, *blend.node, *blend.result, *c_eff, *eff_b);
  bke::node_add_link(tree, *coverage.node, *coverage.socket, *c_eff, *eff_fac);

  /* `a' = a + f * (1 - a)`, then the weight of the correction in the colour, `f / a'`. */
  const NodeOutput one_minus = math_node_add(
      tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, coverage, 1.0f);
  const NodeOutput scaled = math_node_add(
      tree, grid_, nodes, NODE_MATH_MULTIPLY, factor, one_minus);
  const NodeOutput joined = math_node_add(tree, grid_, nodes, NODE_MATH_ADD, coverage, scaled);
  const NodeOutput weight = math_node_add(tree, grid_, nodes, NODE_MATH_DIVIDE, factor, joined);
  if (one_minus.node == nullptr || scaled.node == nullptr || joined.node == nullptr ||
      weight.node == nullptr)
  {
    return discard();
  }
  bke::node_add_link(tree, *content.node, *content.socket, *lay, *lay_a);
  bke::node_add_link(tree, *c_eff, *eff_out, *lay, *lay_b);
  bke::node_add_link(tree, *weight.node, *weight.socket, *lay, *lay_fac);

  content = {lay, lay_out};
  coverage = joined;
  if (alpha_joined.node != nullptr) {
    *content_alpha = alpha_joined;
  }
  frame();
  return true;
}

KindEmitResult PaintLayersTreeBuilder::row_source_emit(const RowEmitContext &ctx)
{
  bNodeTree &tree = ctx.tree;
  LayerGroup &group = ctx.group;
  bNode *group_input = group.group_input;
  KindEmitResult out;

  if (ctx.folder) {
    /* The straightened result of what the folder holds. */
    out.color = {group_input, ctx.folder_color};
    out.content_alpha = {group_input, ctx.folder_alpha};
  }
  else if (ctx.adjustment) {
    /* The filter at full strength over what is below; the row's factor and blend lay it. A filter
     * changes the colour and nothing else: the alpha it lays is the one below. */
    NodeOutput filtered = {group_input, ctx.below};
    if (!this->filter_lay(tree, group, ctx.layer, ctx.channel, filtered, true)) {
      return {};
    }
    out.color = filtered;
    out.content_alpha = {group_input, ctx.below_alpha};
  }
  else if (ctx.empty_base) {
    /* An empty base: the corrections replace the colour wherever they cover, so its own is a flat
     * zero with a coverage of zero. */
    bNode *zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
    bNodeSocket *zero_out = (zero != nullptr) ? socket_out(*zero, "Color") : nullptr;
    if (zero_out == nullptr) {
      return {};
    }
    if (zero_out->default_value != nullptr) {
      zero_v4(static_cast<bNodeSocketValueRGBA *>(zero_out->default_value)->value);
    }
    grid_.place(*zero, layout::Column::Source, 0);
    out.color = {zero, zero_out};

    bNode *no_coverage = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
    bNodeSocket *no_coverage_out = (no_coverage != nullptr) ? socket_out(*no_coverage, "Value") :
                                                              nullptr;
    if (no_coverage_out == nullptr) {
      return {};
    }
    if (no_coverage_out->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(no_coverage_out->default_value)->value = 0.0f;
    }
    grid_.place(*no_coverage, layout::Column::Source, 1);
    out.coverage = {no_coverage, no_coverage_out};
    out.content_alpha = out.coverage;
  }
  else if (ctx.record != nullptr && ctx.record->image != nullptr) {
    /* A map covers by its own alpha, so a texel it leaves transparent shows what is below. */
    bNode *map = this->image_map_node_add(tree, *ctx.record->image);
    if (map == nullptr) {
      return {};
    }
    /* Why grid: the row source opens the Source column at the origin. */
    grid_.place(*map, layout::Column::Source, 0);
    Vector<bNode *> source_nodes;
    source_nodes.append(map);
    out.color = {map, socket_out(*map, "Color")};
    const bool mapped = this->mapping_apply(tree, group, ctx.layer, *map, source_nodes);
    if (ctx.channel == PAINT_MATERIAL_CHANNEL_NORMAL && out.color.socket != nullptr &&
        (ctx.record->flag & MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX) != 0)
    {
      /* A DirectX map has its green pointing down; the Normal Map node reads OpenGL, so the green
       * is flipped in nodes the user can see in the Shader Editor. Alpha stays the map's. Done
       * before the remap below, which expects OpenGL vectors, as the composite and the GPU
       * preview do. */
      const NodeOutput flipped = this->normal_green_flip_nodes_add(
          tree, out.color, source_nodes);
      if (flipped.node != nullptr) {
        out.color = flipped;
      }
    }
    if (mapped && ctx.channel == PAINT_MATERIAL_CHANNEL_NORMAL && out.color.socket != nullptr) {
      /* The Mapping moved only the read point; the tangent-space vectors follow it. */
      out.color = this->normal_remap_add(tree, ctx.layer, out.color, &source_nodes);
    }
    if (bNodeSocket *map_alpha = socket_out(*map, "Alpha")) {
      /* A clipping UV wrap takes the part outside the square out of the coverage, and with it the
       * content alpha, which starts from the coverage. */
      out.coverage = this->mapping_clip_alpha(tree, ctx.layer, {map, map_alpha}, source_nodes);
      out.content_alpha = out.coverage;
    }
    layout::frame_add(tree, "Source", source_nodes);
  }
  else {
    /* The constant the row's group takes as an input; it covers fully, and its alpha is an input
     * of its own. */
    bNodeTreeInterfaceSocket *const *fill_iface = nullptr;
    if (Map<int, bNodeTreeInterfaceSocket *> *by_channel = fill_inputs_.lookup_ptr(&ctx.layer)) {
      fill_iface = by_channel->lookup_ptr(ctx.channel);
    }
    if (fill_iface == nullptr) {
      return {};
    }
    out.color = {group_input, group_input_socket(group, **fill_iface)};
    if (Map<int, bNodeTreeInterfaceSocket *> *alpha_by_channel = fill_alpha_inputs_.lookup_ptr(
            &ctx.layer))
    {
      if (bNodeTreeInterfaceSocket **alpha_iface = alpha_by_channel->lookup_ptr(ctx.channel)) {
        out.content_alpha = {group_input, group_input_socket(group, **alpha_iface)};
      }
    }
  }

  if (out.color.socket == nullptr) {
    return {};
  }
  /* A channel that tracks a content alpha needs the kind to have one; the rest has none. */
  if (!ctx.track_alpha) {
    out.content_alpha = {};
  }
  else if (out.content_alpha.socket == nullptr) {
    return {};
  }
  return out;
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
  /* A Paint row with no map and no value alpha takes part only for the corrections laid over it:
   * its own content covers nothing. */
  const bool empty_base = !folder && paint_layer_row_empty_base(ma_, layer, channel);
  /* An adjustment changes the stack below it, so it has no source of its own. Inside an isolating
   * folder the accumulation is pre-multiplied and partly covered, which a filter of the colour
   * cannot read; the row is left out there, as the CPU composite leaves it out. */
  const bool adjustment = paint_layer_adjustment_acts(ma_, layer, channel);
  if (adjustment && isolated) {
    return {};
  }
  if (record == nullptr && !folder && !empty_base && !adjustment) {
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

  /* The content alpha of a channel that tracks one rides next to the colour: what is below, and the
   * row laid over it. Inside a folder it is pre-multiplied like the colour (`P_a`), and a folder
   * takes the straightened alpha of what it holds. See #BKE_paint_material_channel_tracks_content_alpha
   * for what the quantity is for. */
  const bool track_alpha = BKE_paint_material_channel_tracks_content_alpha(
      eMaterialPaintChannel(channel));
  bNodeTreeInterfaceSocket *below_alpha_iface = nullptr;
  bNodeTreeInterfaceSocket *result_alpha_iface = nullptr;
  bNodeTreeInterfaceSocket *folder_alpha_iface = nullptr;
  bNodeSocket *below_alpha = nullptr;
  bNodeSocket *result_alpha_out = nullptr;
  bNodeSocket *folder_alpha = nullptr;
  if (track_alpha) {
    SNPRINTF(socket_name, "Below Content Alpha %s", info.ui_name);
    below_alpha_iface = layer_group_add_socket(
        group, socket_name, "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT);
    SNPRINTF(socket_name, "Result Content Alpha %s", info.ui_name);
    result_alpha_iface = layer_group_add_socket(
        group, socket_name, "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT);
    if (below_alpha_iface == nullptr || result_alpha_iface == nullptr) {
      return {};
    }
    below_alpha = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(below_alpha_iface->identifier));
    result_alpha_out = bke::node_find_socket(
        *group.group_output, SOCK_IN, UString::from_ptr_noinline(result_alpha_iface->identifier));
    if (folder) {
      SNPRINTF(socket_name, "Folder Content Alpha %s", info.ui_name);
      folder_alpha_iface = layer_group_add_socket(
          group, socket_name, "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT);
      if (folder_alpha_iface == nullptr) {
        return {};
      }
      folder_alpha = bke::node_find_socket(
          *group_input, SOCK_OUT, UString::from_ptr_noinline(folder_alpha_iface->identifier));
    }
    if (below_alpha == nullptr || result_alpha_out == nullptr ||
        (folder && folder_alpha == nullptr))
    {
      return {};
    }
  }

  /* What the row's own kind lays: its colour, the coverage that colour has and, in a channel that
   * tracks one, its content alpha. The one place that tells the kinds apart for a row. */
  RowEmitContext emit_context{tree,
                              group,
                              layer,
                              channel,
                              folder,
                              adjustment,
                              empty_base,
                              track_alpha,
                              record,
                              below,
                              below_alpha,
                              folder_color,
                              folder_alpha};
  const KindEmitResult emitted = this->row_source_emit(emit_context);
  if (emitted.color.socket == nullptr) {
    return {};
  }
  bNode *source_node = emitted.color.node;
  bNodeSocket *source = emitted.color.socket;
  NodeOutput row_coverage = emitted.coverage;
  NodeOutput content_alpha = emitted.content_alpha;

  /* Content corrections lay their maps over the colour before the row's own blend, and raise the
   * coverage the row lays with. A folder's colour is the straightened result of what it holds and
   * its coverage what that accumulated, so the correction works on those two exactly as it does on
   * a row's map and alpha. */
  NodeOutput folder_cover = {group_input, folder_coverage};
  {
    NodeOutput content = {source_node, source};
    NodeOutput &coverage_in_out = folder ? folder_cover : row_coverage;
    for (const MaterialPaintLayer &effect : layer.effects) {
      this->correction_lay(tree,
                           group,
                           effect,
                           channel,
                           content,
                           coverage_in_out,
                           track_alpha ? &content_alpha : nullptr);
    }
    source_node = content.node;
    source = content.socket;
  }

  /* The factor is the row's opacity times its content coverage. A constant covers by its opacity
   * alone. */
  bNode *factor_node = nullptr;
  bNodeSocket *factor = nullptr;
  if (Map<int, bNodeTreeInterfaceSocket *> *by_channel = opacity_inputs_.lookup_ptr(&layer)) {
    if (bNodeTreeInterfaceSocket **opacity_iface = by_channel->lookup_ptr(channel)) {
      factor_node = group_input;
      factor = group_input_socket(group, **opacity_iface);
    }
  }
  if (row_coverage.socket != nullptr) {
    if (factor == nullptr) {
      factor_node = row_coverage.node;
      factor = row_coverage.socket;
    }
    else {
      const NodeOutput product = this->multiply_nodes_add(
          tree, {factor_node, factor}, row_coverage);
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
      factor_node = folder_cover.node;
      factor = folder_cover.socket;
    }
    else {
      const NodeOutput product = this->multiply_nodes_add(
          tree, {factor_node, factor}, folder_cover);
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

  /* The object mask is one more factor, the same place the row's mask acts: a hidden row is exactly
   * a row at zero opacity. The CPU composite knows no object, so it stays as the visible case. */
  const NodeOutput object_visible = this->object_mask_visible_ensure(tree, layer);
  if (object_visible.node != nullptr) {
    if (factor == nullptr) {
      factor_node = object_visible.node;
      factor = object_visible.socket;
    }
    else {
      const NodeOutput product = this->multiply_nodes_add(
          tree, {factor_node, factor}, object_visible);
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

  /* The content alpha follows the colour by the same model, as a scalar: a Mix row lays its alpha
   * by the row's factor, every other blend keeps the alpha below, exactly as the CPU's `ramp_blend`
   * treats the fourth component. Inside a folder it is accumulated pre-multiplied like `P`, with
   * `S_a = P_a / a` where a blend other than Mix reads the straight alpha below. */
  if (track_alpha) {
    const bool mix_alpha = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(layer.blend)) ==
                           MA_RAMP_BLEND;
    Vector<bNode *> alpha_nodes;
    /* `mix(a, b, fac)` as a float Mix node; an empty \a fac covers fully. */
    const auto alpha_mix = [&](const NodeOutput a,
                               const NodeOutput b,
                               const NodeOutput fac) -> NodeOutput {
      bNode *node = mix_float_node_add(tree);
      bNodeSocket *a_in = (node != nullptr) ? socket_in(*node, "A_Float") : nullptr;
      bNodeSocket *b_in = (node != nullptr) ? socket_in(*node, "B_Float") : nullptr;
      bNodeSocket *fac_in = (node != nullptr) ? socket_in(*node, "Factor_Float") : nullptr;
      bNodeSocket *out = (node != nullptr) ? socket_out(*node, "Result_Float") : nullptr;
      if (a_in == nullptr || b_in == nullptr || fac_in == nullptr || out == nullptr ||
          a.socket == nullptr || b.socket == nullptr)
      {
        if (node != nullptr) {
          bke::node_remove_node(nullptr, tree, *node, true, false);
        }
        return {};
      }
      grid_.place_extra(*node);
      bke::node_add_link(tree, *a.node, *a.socket, *node, *a_in);
      bke::node_add_link(tree, *b.node, *b.socket, *node, *b_in);
      if (fac.socket != nullptr) {
        bke::node_add_link(tree, *fac.node, *fac.socket, *node, *fac_in);
      }
      else if (fac_in->default_value != nullptr) {
        static_cast<bNodeSocketValueFloat *>(fac_in->default_value)->value = 1.0f;
      }
      alpha_nodes.append(node);
      return {node, out};
    };
    const NodeOutput below_a = {group_input, below_alpha};
    const NodeOutput row_factor = {factor_node, factor};
    NodeOutput result_a;
    if (!isolated) {
      result_a = mix_alpha ? alpha_mix(below_a, content_alpha, row_factor) : below_a;
    }
    else {
      const NodeOutput below_cov = {group_input, below_coverage};
      NodeOutput lay_a = content_alpha;
      if (!mix_alpha) {
        /* c_eff_a = lerp(c_a, S_a, a): the straight alpha below shows where the row covers. */
        const NodeOutput straight_a = math_node_add(
            tree, grid_, alpha_nodes, NODE_MATH_DIVIDE, below_a, below_cov);
        lay_a = alpha_mix(content_alpha, straight_a, below_cov);
      }
      result_a = alpha_mix(below_a, lay_a, row_factor);
    }
    if (result_a.socket == nullptr) {
      return {};
    }
    bke::node_add_link(
        tree, *result_a.node, *result_a.socket, *group.group_output, *result_alpha_out);
    if (!alpha_nodes.is_empty()) {
      layout::frame_add(tree, "Content Alpha", alpha_nodes);
    }
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
  if (track_alpha) {
    result.below_alpha = bke::node_find_socket(
        *group.instance, SOCK_IN, UString::from_ptr_noinline(below_alpha_iface->identifier));
    result.result_alpha = bke::node_find_socket(
        *group.instance, SOCK_OUT, UString::from_ptr_noinline(result_alpha_iface->identifier));
    if (result.below_alpha == nullptr || result.result_alpha == nullptr) {
      return {};
    }
    if (folder) {
      result.folder_alpha = bke::node_find_socket(
          *group.instance, SOCK_IN, UString::from_ptr_noinline(folder_alpha_iface->identifier));
      if (result.folder_alpha == nullptr) {
        return {};
      }
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
  if (BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel))) {
    result.below_alpha = instance_input("Below Content Alpha", "NodeSocketFloat", true);
    result.result_alpha = instance_input("Result Content Alpha", "NodeSocketFloat", false);
    if (result.below_alpha == nullptr || result.result_alpha == nullptr) {
      return {};
    }
    if (BKE_paint_layers_is_folder(layer)) {
      result.folder_alpha = instance_input("Folder Content Alpha", "NodeSocketFloat", true);
      if (result.folder_alpha == nullptr) {
        return {};
      }
    }
  }
  return result;
}

}  // namespace bke::paint_layers
}  // namespace blender
