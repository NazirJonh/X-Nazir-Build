/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The Height channel's own blend: the delta-space chain of static nodes a row's Height record
 * builds instead of a Mix, and the won mask a Compare record publishes to the row's other
 * channels.
 */

#include "BKE_paint_layers_generate.hh"

#include "BLI_ustring.hh"

#include "CLG_log.h"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "paint_layers_generate_build_intern.hh"
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"

namespace blender {

static CLG_LogRef LOG = {"bke.paint_layers"};

namespace bke::paint_layers {

NodeOutput PaintLayersTreeBuilder::height_scalar_decode(bNodeTree &tree,
                                                        Vector<bNode *> &r_nodes,
                                                        const NodeOutput color)
{
  if (color.socket == nullptr) {
    return {};
  }
  if (color.socket->type == SOCK_FLOAT) {
    return color;
  }
  bNode *separate = bke::node_add_static_node(nullptr, tree, SH_NODE_SEPARATE_COLOR);
  bNodeSocket *in = (separate != nullptr) ? socket_in(*separate, "Color") : nullptr;
  bNodeSocket *red = (separate != nullptr) ? socket_out(*separate, "Red") : nullptr;
  if (separate == nullptr || in == nullptr || red == nullptr) {
    if (separate != nullptr) {
      bke::node_remove_node(nullptr, tree, *separate, true, false);
    }
    return {};
  }
  grid_.place_extra(*separate);
  bke::node_add_link(tree, *color.node, *color.socket, *separate, *in);
  r_nodes.append(separate);
  return {separate, red};
}

NodeOutput PaintLayersTreeBuilder::height_blend_chain_add(bNodeTree &tree,
                                                          LayerGroup &group,
                                                          const MaterialPaintLayer &row,
                                                          const NodeOutput below,
                                                          const NodeOutput source,
                                                          const NodeOutput factor,
                                                          const int height_blend)
{
  /* A missing node is a failed build, not a row without height: the caller drops the row from the
   * chain either way, and the note names it so the drop is visible in the log. */
  const auto fail = [&]() -> NodeOutput {
    CLOG_ERROR(
        &LOG, "Could not build the Height blend chain of row '%s': a node is missing", row.name);
    return {};
  };
  if (below.socket == nullptr || source.socket == nullptr) {
    return fail();
  }
  Vector<bNode *> nodes;
  /* The heights are scalars carried on colour sockets: the red of a Separate Color is the value
   * every component of a grey height map holds, the one channel the CPU reads too. */
  const NodeOutput below_value = this->height_scalar_decode(tree, nodes, below);
  const NodeOutput source_value = this->height_scalar_decode(tree, nodes, source);
  if (below_value.socket == nullptr || source_value.socket == nullptr) {
    return fail();
  }

  /* `d_below = below - neutral` and `d_layer = (source - neutral) * scale`, the delta space every
   * mode works in. The scale is the row's input, a Value constant only for a degraded build the
   * value sync has no input for yet. */
  bNode *d_below = math_constant_node_add(
      tree, grid_, nodes, NODE_MATH_SUBTRACT, below_value, PAINT_HEIGHT_NEUTRAL);
  bNode *delta = math_constant_node_add(
      tree, grid_, nodes, NODE_MATH_SUBTRACT, source_value, PAINT_HEIGHT_NEUTRAL);
  if (d_below == nullptr || delta == nullptr) {
    return fail();
  }
  const NodeOutput delta_out = {delta, socket_out(*delta, "Value")};
  NodeOutput scale = {};
  if (bNodeTreeInterfaceSocket **iface = height_scale_inputs_.lookup_ptr(&row)) {
    scale = {group.group_input, this->group_input_socket(group, **iface)};
  }
  else {
    scale = value_node_add(tree,
                           grid_,
                           nodes,
                           BKE_paint_layers_channel_height_scale(row,
                                                                 PAINT_MATERIAL_CHANNEL_HEIGHT));
  }
  NodeOutput d_layer_out = {};
  if (scale.socket != nullptr) {
    d_layer_out = math_node_add(tree, grid_, nodes, NODE_MATH_MULTIPLY, delta_out, scale, 0.0f);
  }
  if (d_layer_out.socket == nullptr) {
    return fail();
  }
  const NodeOutput d_below_out = {d_below, socket_out(*d_below, "Value")};

  /* The mode's step. A float Mix is `lerp(a, b, fac)` with the factor clamped and the result not,
   * exactly the CPU's `blend_row_height`; Add and Subtract are the Math pair; Compare is the
   * Ucupaint group node for node: `won = d_below < d_layer` (a LESS_THAN), the step a mix by
   * `won * fac`. */
  const auto float_mix = [&](const NodeOutput fac) -> NodeOutput {
    bNode *mix = mix_float_node_add(tree);
    bNodeSocket *a_in = (mix != nullptr) ? socket_in(*mix, "A_Float") : nullptr;
    bNodeSocket *b_in = (mix != nullptr) ? socket_in(*mix, "B_Float") : nullptr;
    bNodeSocket *fac_in = (mix != nullptr) ? socket_in(*mix, "Factor_Float") : nullptr;
    bNodeSocket *out = (mix != nullptr) ? socket_out(*mix, "Result_Float") : nullptr;
    if (mix == nullptr || a_in == nullptr || b_in == nullptr || fac_in == nullptr ||
        out == nullptr)
    {
      if (mix != nullptr) {
        bke::node_remove_node(nullptr, tree, *mix, true, false);
      }
      return {};
    }
    grid_.place_extra(*mix);
    bke::node_add_link(tree, *d_below_out.node, *d_below_out.socket, *mix, *a_in);
    bke::node_add_link(tree, *d_layer_out.node, *d_layer_out.socket, *mix, *b_in);
    if (fac.socket != nullptr) {
      bke::node_add_link(tree, *fac.node, *fac.socket, *mix, *fac_in);
    }
    else if (fac_in->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(fac_in->default_value)->value = 1.0f;
    }
    nodes.append(mix);
    return {mix, out};
  };
  NodeOutput stepped;
  if (height_blend == MA_PAINT_LAYER_HEIGHT_BLEND_MIX) {
    stepped = float_mix(factor);
  }
  else if (height_blend == MA_PAINT_LAYER_HEIGHT_BLEND_ADD ||
           height_blend == MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT)
  {
    /* `d_out = d_below ± fac * d_layer` -- the weighted delta as one Math pair. */
    NodeOutput weighted = {};
    if (factor.socket != nullptr) {
      weighted = math_node_add(tree, grid_, nodes, NODE_MATH_MULTIPLY, d_layer_out, factor, 0.0f);
    }
    else {
      weighted = d_layer_out;
    }
    if (weighted.socket == nullptr) {
      return fail();
    }
    stepped = math_node_add(tree,
                            grid_,
                            nodes,
                            height_blend == MA_PAINT_LAYER_HEIGHT_BLEND_ADD ? NODE_MATH_ADD :
                                                                              NODE_MATH_SUBTRACT,
                            d_below_out,
                            weighted,
                            0.0f);
  }
  else if (height_blend == MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE) {
    NodeOutput won = math_node_add(
        tree, grid_, nodes, NODE_MATH_LESS_THAN, d_below_out, d_layer_out, 0.0f);
    if (won.socket == nullptr) {
      return fail();
    }
    NodeOutput fac = won;
    if (factor.socket != nullptr) {
      fac = math_node_add(tree, grid_, nodes, NODE_MATH_MULTIPLY, won, factor, 0.0f);
      if (fac.socket == nullptr) {
        return fail();
      }
    }
    stepped = float_mix(fac);
    /* The mask gates every other channel of the row; the Height pass runs first (see #build). */
    height_won_.add(&row, won);
  }
  if (stepped.socket == nullptr) {
    return fail();
  }

  /* Encode back: the whole colour carries the scalar, the way the CPU writes one value to all
   * three components. */
  bNode *encoded = math_constant_node_add(
      tree, grid_, nodes, NODE_MATH_ADD, stepped, PAINT_HEIGHT_NEUTRAL);
  if (encoded == nullptr) {
    return fail();
  }
  layout::frame_add(tree, "Height Blend", nodes);
  return {encoded, socket_out(*encoded, "Value")};
}

}  // namespace bke::paint_layers
}  // namespace blender
