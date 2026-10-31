/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Filter rows: the pointwise filters (invert, hue/saturation, levels, brightness/contrast) a row
 * applies to its colour, each lerped over the row's own factor by the filter's opacity.
 */

#include "BKE_paint_layers_generate.hh"

#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_paint_layers.hh"

#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_layers_generate_build_intern.hh"
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {
namespace bke::paint_layers {

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
    case MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST: {
      /* `lerp(c, max((1 + contrast) * c + brightness - contrast * 0.5, 0), opacity)`: the
       * Brightness/Contrast node's own function, fed from the parameters as inputs so an edit of
       * one is a value and not a different graph. The node leaves alpha untouched. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM) {
        return false;
      }
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        for (bNode *node : nodes) {
          bke::node_remove_node(nullptr, tree, *node, true, false);
        }
        return false;
      };

      bNode *bc = bke::node_add_static_node(nullptr, tree, SH_NODE_BRIGHTCONTRAST);
      if (bc == nullptr) {
        return false;
      }
      nodes.append(bc);
      grid_.place_extra(*bc);
      bNodeSocket *color_in = socket_in(*bc, "Color");
      bNodeSocket *color_out = socket_out(*bc, "Color");
      const char *const param_names[PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM] = {"Bright",
                                                                                   "Contrast"};
      bool linked = color_in != nullptr && color_out != nullptr;
      for (const int i : IndexRange(PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM)) {
        bNodeSocket *param_in = linked ? socket_in(*bc, param_names[i]) : nullptr;
        bNodeSocket *param_out = ((*params)[i] != nullptr) ?
                                     group_input_socket(group, *(*params)[i]) :
                                     nullptr;
        if (param_in == nullptr || param_out == nullptr) {
          linked = false;
          break;
        }
        bke::node_add_link(tree, *group.group_input, *param_out, *bc, *param_in);
      }
      if (!linked) {
        return discard();
      }
      bke::node_add_link(tree, *content.node, *content.socket, *bc, *color_in);

      bNode *mix = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
      bNodeSocket *a = (mix != nullptr) ? socket_in(*mix, "A_Color") : nullptr;
      bNodeSocket *b = (mix != nullptr) ? socket_in(*mix, "B_Color") : nullptr;
      bNodeSocket *fac = (mix != nullptr) ? socket_in(*mix, "Factor_Float") : nullptr;
      bNodeSocket *out = (mix != nullptr) ? socket_out(*mix, "Result_Color") : nullptr;
      if (mix == nullptr || a == nullptr || b == nullptr || fac == nullptr || out == nullptr) {
        if (mix != nullptr) {
          bke::node_remove_node(nullptr, tree, *mix, true, false);
        }
        return discard();
      }
      nodes.append(mix);
      grid_.place_extra(*mix);
      bke::node_add_link(tree, *content.node, *content.socket, *mix, *a);
      bke::node_add_link(tree, *bc, *color_out, *mix, *b);
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

}  // namespace bke::paint_layers
}  // namespace blender
