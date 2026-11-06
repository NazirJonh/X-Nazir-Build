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
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_colortools.hh"
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
      /* `out_k = c_k + (|c_k - 1| - c_k) * f_k`: Difference against white is `1 - c` on [0, 1] and
       * stays symmetric beyond it, the three amounts ride in as one vector, and the opacity is the
       * final mix's own factor. Every parameter an input of the group, so an edit of one is a value
       * and not a different graph. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_INVERT_PARAM_NUM) {
        return false;
      }
      NodeOutput param[PAINT_LAYER_INVERT_PARAM_NUM];
      for (const int i : IndexRange(PAINT_LAYER_INVERT_PARAM_NUM)) {
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
        nodes_discard(tree, nodes);
        return false;
      };

      /* The complement itself, not a mix towards it: the Difference sits at full factor, so the
       * per-channel amounts -- not the node -- decide how far each channel turns. */
      bNode *diff = mix_node_add(tree, MA_RAMP_DIFF, 0.0f, 0.0f);
      bNodeSocket *a = (diff != nullptr) ? socket_in(*diff, "A_Color") : nullptr;
      bNodeSocket *b = (diff != nullptr) ? socket_in(*diff, "B_Color") : nullptr;
      bNodeSocket *fac = (diff != nullptr) ? socket_in(*diff, "Factor_Float") : nullptr;
      bNodeSocket *diff_out = (diff != nullptr) ? socket_out(*diff, "Result_Color") : nullptr;
      if (a == nullptr || b == nullptr || fac == nullptr || diff_out == nullptr ||
          b->default_value == nullptr || fac->default_value == nullptr)
      {
        if (diff != nullptr) {
          bke::node_remove_node(nullptr, tree, *diff, true, false);
        }
        return false;
      }
      const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(b->default_value)->value, white);
      static_cast<bNodeSocketValueFloat *>(fac->default_value)->value = 1.0f;
      grid_.place_extra(*diff);
      nodes.append(diff);
      bke::node_add_link(tree, *content.node, *content.socket, *diff, *a);

      /* The distance from the colour to its complement, scaled by the amount and added back: a
       * lerp of the colour towards the complement, per channel. */
      const NodeOutput amounts = combine_xyz_node_add(tree, grid_, nodes, param);
      bNode *distance = vector_math_node_add(
          tree, grid_, nodes, NODE_VECTOR_MATH_SUBTRACT, {diff, diff_out}, content);
      bNode *lerped = (distance != nullptr) ?
                          vector_math_node_add(tree,
                                               grid_,
                                               nodes,
                                               NODE_VECTOR_MATH_MULTIPLY_ADD,
                                               {distance, socket_out(*distance, "Vector")},
                                               amounts) :
                          nullptr;
      if (amounts.socket == nullptr || distance == nullptr || lerped == nullptr) {
        return discard();
      }
      bNodeSocket *base_in = socket_in(*lerped, "Vector_002");
      if (base_in == nullptr) {
        return discard();
      }
      bke::node_add_link(tree, *content.node, *content.socket, *lerped, *base_in);

      bNode *mix = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
      bNodeSocket *mix_a = (mix != nullptr) ? socket_in(*mix, "A_Color") : nullptr;
      bNodeSocket *mix_b = (mix != nullptr) ? socket_in(*mix, "B_Color") : nullptr;
      bNodeSocket *mix_fac = (mix != nullptr) ? socket_in(*mix, "Factor_Float") : nullptr;
      bNodeSocket *mix_out = (mix != nullptr) ? socket_out(*mix, "Result_Color") : nullptr;
      if (mix == nullptr || mix_a == nullptr || mix_b == nullptr || mix_fac == nullptr ||
          mix_out == nullptr)
      {
        if (mix != nullptr) {
          bke::node_remove_node(nullptr, tree, *mix, true, false);
        }
        return discard();
      }
      nodes.append(mix);
      grid_.place_extra(*mix);
      bke::node_add_link(tree, *content.node, *content.socket, *mix, *mix_a);
      bke::node_add_link(tree, *lerped, *socket_out(*lerped, "Vector"), *mix, *mix_b);
      bke::node_add_link(tree, *strength.node, *strength.socket, *mix, *mix_fac);
      content = {mix, mix_out};
      layout::frame_add(tree, effect.name, nodes);
      return true;
    }
    case MA_PAINT_LAYER_FILTER_HSL: {
      /* A Hue/Saturation/Value node at full strength, then the vibrance lift, then the opacity:
       * the node's own factor would mix before the vibrance saw the colour, while the CPU applies
       * the whole filter first and blends it by the strength after. At a zero vibrance the lift
       * reads the colour back, so the tree is fixed and an edit of any parameter is a value edit. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_HSL_PARAM_NUM) {
        return false;
      }
      NodeOutput param[PAINT_LAYER_HSL_PARAM_NUM];
      for (const int i : IndexRange(PAINT_LAYER_HSL_PARAM_NUM)) {
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
        nodes_discard(tree, nodes);
        return false;
      };

      bNode *hsv = bke::node_add_static_node(nullptr, tree, SH_NODE_HUE_SAT);
      bNodeSocket *color_in = (hsv != nullptr) ? socket_in(*hsv, "Color") : nullptr;
      bNodeSocket *color_out = (hsv != nullptr) ? socket_out(*hsv, "Color") : nullptr;
      const char *const names[3] = {"Hue", "Saturation", "Value"};
      bool linked = color_in != nullptr && color_out != nullptr;
      for (const int i : IndexRange(3)) {
        bNodeSocket *param_in = linked ? socket_in(*hsv, names[i]) : nullptr;
        if (param_in == nullptr) {
          linked = false;
          break;
        }
        bke::node_add_link(tree, *param[i].node, *param[i].socket, *hsv, *param_in);
      }
      if (!linked) {
        if (hsv != nullptr) {
          bke::node_remove_node(nullptr, tree, *hsv, true, false);
        }
        return false;
      }
      /* The node's own factor stays at its default of one: the mix to the colour it read is the
       * final mix's job. */
      grid_.place_extra(*hsv);
      nodes.append(hsv);
      bke::node_add_link(tree, *content.node, *content.socket, *hsv, *color_in);

      /* The vibrance: `rgb + (rgb - gray) * vibrance * (1 - (max - min) / max)`, the gray the RGB
       * to BW node's own luminance, then the clamp below zero the HSV branch applies. */
      bNode *bw = bke::node_add_static_node(nullptr, tree, SH_NODE_RGBTOBW);
      NodeOutput xyz[3];
      bool lifted = bw != nullptr;
      if (lifted) {
        nodes.append(bw);
        grid_.place_extra(*bw);
        bNodeSocket *bw_color = socket_in(*bw, "Color");
        bNodeSocket *bw_val = socket_out(*bw, "Val");
        lifted = bw_color != nullptr && bw_val != nullptr;
        if (lifted) {
          bke::node_add_link(tree, *hsv, *color_out, *bw, *bw_color);
        }
      }
      if (lifted) {
        lifted = separate_xyz_node_add(tree, grid_, nodes, {hsv, color_out}, xyz);
      }
      const NodeOutput gray = {bw, (bw != nullptr) ? socket_out(*bw, "Val") : nullptr};
      const NodeOutput max_xy = lifted ?
                                    math_node_add(tree, grid_, nodes, NODE_MATH_MAXIMUM, xyz[0], xyz[1]) :
                                    NodeOutput{};
      const NodeOutput max_c = (max_xy.socket != nullptr) ?
                                   math_node_add(tree, grid_, nodes, NODE_MATH_MAXIMUM, max_xy, xyz[2]) :
                                   NodeOutput{};
      const NodeOutput min_xy = lifted ?
                                    math_node_add(tree, grid_, nodes, NODE_MATH_MINIMUM, xyz[0], xyz[1]) :
                                    NodeOutput{};
      const NodeOutput min_c = (min_xy.socket != nullptr) ?
                                   math_node_add(tree, grid_, nodes, NODE_MATH_MINIMUM, min_xy, xyz[2]) :
                                   NodeOutput{};
      const NodeOutput span = (max_c.socket != nullptr && min_c.socket != nullptr) ?
                                  math_node_add(tree, grid_, nodes, NODE_MATH_SUBTRACT, max_c, min_c) :
                                  NodeOutput{};
      /* A black reads as unsaturated: the divide hands back zero where the max is zero, and the
       * difference from the grey shrinks with it. */
      const NodeOutput sat = (span.socket != nullptr) ?
                                 math_node_add(tree, grid_, nodes, NODE_MATH_DIVIDE, span, max_c) :
                                 NodeOutput{};
      const NodeOutput rest = (sat.socket != nullptr) ?
                                  math_node_add(tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, sat, 1.0f) :
                                  NodeOutput{};
      const NodeOutput boost = (rest.socket != nullptr) ?
                                   math_node_add(tree,
                                                 grid_,
                                                 nodes,
                                                 NODE_MATH_MULTIPLY,
                                                 param[PAINT_LAYER_HSL_VIBRANCE],
                                                 rest) :
                                   NodeOutput{};
      NodeOutput gray_components[3] = {gray, gray, gray};
      const NodeOutput gray_vec = lifted ?
                                     combine_xyz_node_add(tree, grid_, nodes, gray_components) :
                                     NodeOutput{};
      bNode *diff = (gray_vec.socket != nullptr) ?
                        vector_math_node_add(tree,
                                             grid_,
                                             nodes,
                                             NODE_VECTOR_MATH_SUBTRACT,
                                             {hsv, color_out},
                                             gray_vec) :
                        nullptr;
      NodeOutput boost_components[3] = {boost, boost, boost};
      const NodeOutput boost_vec = (boost.socket != nullptr) ?
                                       combine_xyz_node_add(tree, grid_, nodes, boost_components) :
                                       NodeOutput{};
      bNode *moved = (diff != nullptr && boost_vec.socket != nullptr) ?
                         vector_math_node_add(tree,
                                              grid_,
                                              nodes,
                                              NODE_VECTOR_MATH_MULTIPLY_ADD,
                                              {diff, socket_out(*diff, "Vector")},
                                              boost_vec) :
                         nullptr;
      if (moved != nullptr) {
        bNodeSocket *moved_base = socket_in(*moved, "Vector_002");
        if (moved_base == nullptr) {
          return discard();
        }
        bke::node_add_link(tree, *hsv, *color_out, *moved, *moved_base);
      }
      bNode *clamped = (moved != nullptr) ?
                           vector_math_node_add(tree,
                                                grid_,
                                                nodes,
                                                NODE_VECTOR_MATH_MAXIMUM,
                                                {moved, socket_out(*moved, "Vector")},
                                                {}) :
                           nullptr;
      if (clamped != nullptr) {
        bNodeSocket *zero_b = socket_in(*clamped, "Vector_001");
        if (zero_b == nullptr || zero_b->default_value == nullptr) {
          return discard();
        }
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(zero_b->default_value)->value, 0.0f);
      }
      if (gray.socket == nullptr || max_xy.socket == nullptr || max_c.socket == nullptr ||
          min_xy.socket == nullptr || min_c.socket == nullptr || span.socket == nullptr ||
          sat.socket == nullptr || rest.socket == nullptr || boost.socket == nullptr ||
          gray_vec.socket == nullptr || diff == nullptr || boost_vec.socket == nullptr ||
          moved == nullptr || clamped == nullptr)
      {
        return discard();
      }

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
      bke::node_add_link(tree, *clamped, *socket_out(*clamped, "Vector"), *mix, *b);
      bke::node_add_link(tree, *strength.node, *strength.socket, *mix, *fac);
      content = {mix, out};
      layout::frame_add(tree, effect.name, nodes);
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
        nodes_discard(tree, nodes);
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

      if (!filter_result_mix_add(tree,
                                 grid_,
                                 nodes,
                                 content,
                                 {mapped, socket_out(*mapped, "Vector")},
                                 strength,
                                 effect.name))
      {
        return discard();
      }
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
        nodes_discard(tree, nodes);
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

      if (!filter_result_mix_add(tree,
                                 grid_,
                                 nodes,
                                 content,
                                 {bc, color_out},
                                 strength,
                                 effect.name))
      {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_COLOR_RAMP: {
      /* `lerp(c, ramp(luma(c)), opacity)`: the RGB to BW and Color Ramp nodes' own functions, with
       * the row's band copied into the node so the generated tree owns no ID reference. */
      const ColorBand *band = BKE_paint_layers_ramp_get(effect);
      if (band == nullptr) {
        return false;
      }
      /* The one input is `affect_alpha`; the ramp itself has none. A mask item has no stop alpha to
       * apply, so it needs no input. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (!in_mask && (params == nullptr || params->size() != PAINT_LAYER_COLOR_RAMP_PARAM_NUM - 1 ||
                       (*params)[PAINT_LAYER_COLOR_RAMP_AFFECT_ALPHA] == nullptr))
      {
        return false;
      }
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      bNode *bw = bke::node_add_static_node(nullptr, tree, SH_NODE_RGBTOBW);
      bNode *ramp = bke::node_add_static_node(nullptr, tree, SH_NODE_VALTORGB);
      if (bw == nullptr || ramp == nullptr || ramp->storage == nullptr) {
        if (bw != nullptr) {
          nodes.append(bw);
        }
        if (ramp != nullptr) {
          nodes.append(ramp);
        }
        return discard();
      }
      nodes.append(bw);
      nodes.append(ramp);
      grid_.place_extra(*bw);
      grid_.place_extra(*ramp);
      *static_cast<ColorBand *>(ramp->storage) = *band;

      bNodeSocket *bw_color = socket_in(*bw, "Color");
      bNodeSocket *bw_val = socket_out(*bw, "Val");
      bNodeSocket *ramp_fac = socket_in(*ramp, "Fac");
      bNodeSocket *ramp_color = socket_out(*ramp, "Color");
      bNodeSocket *ramp_alpha = socket_out(*ramp, "Alpha");
      if (bw_color == nullptr || bw_val == nullptr || ramp_fac == nullptr ||
          ramp_color == nullptr || ramp_alpha == nullptr)
      {
        return discard();
      }
      bke::node_add_link(tree, *content.node, *content.socket, *bw, *bw_color);
      bke::node_add_link(tree, *bw, *bw_val, *ramp, *ramp_fac);

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
      bke::node_add_link(tree, *ramp, *ramp_color, *mix, *b);
      NodeOutput mix_strength = strength;
      if (!in_mask) {
        /* `strength * lerp(1, ramp alpha, affect_alpha)` in the shape `1 + affect * (alpha - 1)`,
         * a fixed tree for either mode: at 0 it is exactly the strength. */
        bNodeSocket *affect = group_input_socket(
            group, *(*params)[PAINT_LAYER_COLOR_RAMP_AFFECT_ALPHA]);
        bNode *less_node = math_constant_node_add(
            tree, grid_, nodes, NODE_MATH_SUBTRACT, {ramp, ramp_alpha}, 1.0f);
        if (affect == nullptr || less_node == nullptr) {
          return discard();
        }
        const NodeOutput alpha_less_one = {less_node, socket_out(*less_node, "Value")};
        const NodeOutput weighted = math_node_add(
            tree, grid_, nodes, NODE_MATH_MULTIPLY, {group.group_input, affect}, alpha_less_one);
        bNode *k_node = (weighted.socket != nullptr) ?
                            math_constant_node_add(
                                tree, grid_, nodes, NODE_MATH_ADD, weighted, 1.0f) :
                            nullptr;
        if (k_node == nullptr) {
          return discard();
        }
        mix_strength = math_node_add(tree,
                                     grid_,
                                     nodes,
                                     NODE_MATH_MULTIPLY,
                                     strength,
                                     {k_node, socket_out(*k_node, "Value")});
        if (mix_strength.socket == nullptr) {
          return discard();
        }
      }
      bke::node_add_link(tree, *mix_strength.node, *mix_strength.socket, *mix, *fac);
      content = {mix, out};
      layout::frame_add(tree, effect.name, nodes);
      return true;
    }
    case MA_PAINT_LAYER_FILTER_POSTERIZE: {
      /* `floor(clamp(c, 0, 1) * (steps - 1) + 0.5) / (steps - 1)` per channel: the quantization the
       * CPU composite applies, with the step count as an input so an edit of it is a value and not
       * a different graph. Vector Math MULTIPLY_ADD carries the scale and the half that turns the
       * floor into a round. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_POSTERIZE_PARAM_NUM ||
          (*params)[PAINT_LAYER_POSTERIZE_STEPS] == nullptr)
      {
        return false;
      }
      bNodeSocket *steps_socket = group_input_socket(group, *(*params)[PAINT_LAYER_POSTERIZE_STEPS]);
      if (steps_socket == nullptr) {
        return false;
      }
      const NodeOutput steps{group.group_input, steps_socket};
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      bNode *k = math_constant_node_add(tree, grid_, nodes, NODE_MATH_SUBTRACT, steps, 1.0f);
      bNode *lower = vector_math_node_add(
          tree, grid_, nodes, NODE_VECTOR_MATH_MAXIMUM, content, {});
      bNode *upper = (lower != nullptr) ?
                         vector_math_node_add(tree,
                                              grid_,
                                              nodes,
                                              NODE_VECTOR_MATH_MINIMUM,
                                              {lower, socket_out(*lower, "Vector")},
                                              {}) :
                         nullptr;
      if (k == nullptr || upper == nullptr) {
        return discard();
      }
      /* The CPU clamps the value before quantizing, so the clamps are set explicitly and the
       * quantized result stays in [0, 1]. */
      bNodeSocket *lower_b = socket_in(*lower, "Vector_001");
      bNodeSocket *upper_b = socket_in(*upper, "Vector_001");
      if (lower_b == nullptr || lower_b->default_value == nullptr || upper_b == nullptr ||
          upper_b->default_value == nullptr)
      {
        return discard();
      }
      zero_v3(static_cast<bNodeSocketValueVector *>(lower_b->default_value)->value);
      copy_v3_fl(static_cast<bNodeSocketValueVector *>(upper_b->default_value)->value, 1.0f);
      bNode *scaled = vector_math_node_add(tree,
                                           grid_,
                                           nodes,
                                           NODE_VECTOR_MATH_MULTIPLY_ADD,
                                           {upper, socket_out(*upper, "Vector")},
                                           {k, socket_out(*k, "Value")});
      bNode *floored = (scaled != nullptr) ?
                           vector_math_node_add(tree,
                                                grid_,
                                                nodes,
                                                NODE_VECTOR_MATH_FLOOR,
                                                {scaled, socket_out(*scaled, "Vector")},
                                                {}) :
                           nullptr;
      if (scaled == nullptr || floored == nullptr) {
        return discard();
      }
      bNodeSocket *offset = socket_in(*scaled, "Vector_002");
      if (offset == nullptr || offset->default_value == nullptr) {
        return discard();
      }
      copy_v3_fl(static_cast<bNodeSocketValueVector *>(offset->default_value)->value, 0.5f);
      bNode *divided = vector_math_node_add(tree,
                                            grid_,
                                            nodes,
                                            NODE_VECTOR_MATH_DIVIDE,
                                            {floored, socket_out(*floored, "Vector")},
                                            {k, socket_out(*k, "Value")});
      if (divided == nullptr) {
        return discard();
      }

      if (!filter_result_mix_add(tree,
                                 grid_,
                                 nodes,
                                 content,
                                 {divided, socket_out(*divided, "Vector")},
                                 strength,
                                 effect.name))
      {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_MATH: {
      /* `op(c, operand_r/g/b)` per channel, the operation and the clamp read from the row when the
       * graph is built: an edit of either is a shape edit and rebuilds the tree, the operands stay
       * value inputs. One scalar Math node per channel, the way Vector Math has no POWER,
       * LOGARITHM or Use Clamp of its own; each node's zero-on-zero-divisor rule is the CPU's. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_MATH_PARAM_NUM ||
          (*params)[PAINT_LAYER_MATH_OPERAND_R] == nullptr ||
          (*params)[PAINT_LAYER_MATH_OPERAND_G] == nullptr ||
          (*params)[PAINT_LAYER_MATH_OPERAND_B] == nullptr)
      {
        return false;
      }
      int node_operation = 0;
      switch (paint_layer_kind_param_int_get(effect, PAINT_LAYER_MATH_OPERATION)) {
        case PAINT_LAYER_MATH_OP_ADD:
          node_operation = NODE_MATH_ADD;
          break;
        case PAINT_LAYER_MATH_OP_SUBTRACT:
          node_operation = NODE_MATH_SUBTRACT;
          break;
        case PAINT_LAYER_MATH_OP_MULTIPLY:
          node_operation = NODE_MATH_MULTIPLY;
          break;
        case PAINT_LAYER_MATH_OP_DIVIDE:
          node_operation = NODE_MATH_DIVIDE;
          break;
        case PAINT_LAYER_MATH_OP_MINIMUM:
          node_operation = NODE_MATH_MINIMUM;
          break;
        case PAINT_LAYER_MATH_OP_MAXIMUM:
          node_operation = NODE_MATH_MAXIMUM;
          break;
        case PAINT_LAYER_MATH_OP_POWER:
          node_operation = NODE_MATH_POWER;
          break;
        case PAINT_LAYER_MATH_OP_LOGARITHM:
          node_operation = NODE_MATH_LOGARITHM;
          break;
        default:
          return false;
      }
      const bool clamped = paint_layer_kind_param_bool_get(effect, PAINT_LAYER_MATH_CLAMP);
      bNodeSocket *operand_sockets[3] = {group_input_socket(group, *(*params)[PAINT_LAYER_MATH_OPERAND_R]),
                                         group_input_socket(group, *(*params)[PAINT_LAYER_MATH_OPERAND_G]),
                                         group_input_socket(group, *(*params)[PAINT_LAYER_MATH_OPERAND_B])};
      if (operand_sockets[0] == nullptr || operand_sockets[1] == nullptr ||
          operand_sockets[2] == nullptr)
      {
        return false;
      }
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      NodeOutput channels[3];
      if (!separate_xyz_node_add(tree, grid_, nodes, content, channels)) {
        return discard();
      }
      NodeOutput operated[3];
      for (const int i : IndexRange(3)) {
        operated[i] = math_node_add(tree,
                                    grid_,
                                    nodes,
                                    node_operation,
                                    channels[i],
                                    {group.group_input, operand_sockets[i]});
        if (operated[i].socket == nullptr) {
          return discard();
        }
        if (clamped) {
          /* The Math node's own Use Clamp: the result folds into [0, 1], what the CPU's clamp
           * reads. Off leaves the flag at its zero default; the flag is part of the node, so the
           * clamp is in the topology hash with the operation. */
          operated[i].node->custom2 = SHD_MATH_CLAMP;
        }
      }
      NodeOutput combined = combine_xyz_node_add(tree, grid_, nodes, operated);
      if (combined.socket == nullptr) {
        return discard();
      }

      if (!filter_result_mix_add(tree, grid_, nodes, content, combined, strength, effect.name)) {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_THRESHOLD: {
      /* `(c > threshold) ? 1 : 0` per channel, the Math node's Greater Than with the threshold as
       * an input, so an edit of it is a value and not a different graph. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_THRESHOLD_PARAM_NUM ||
          (*params)[PAINT_LAYER_THRESHOLD_THRESHOLD] == nullptr)
      {
        return false;
      }
      bNodeSocket *threshold_socket = group_input_socket(
          group, *(*params)[PAINT_LAYER_THRESHOLD_THRESHOLD]);
      if (threshold_socket == nullptr) {
        return false;
      }
      const NodeOutput threshold{group.group_input, threshold_socket};
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      NodeOutput xyz[3];
      if (!separate_xyz_node_add(tree, grid_, nodes, content, xyz)) {
        return discard();
      }
      NodeOutput compared[3];
      for (const int i : IndexRange(3)) {
        compared[i] = math_node_add(tree, grid_, nodes, NODE_MATH_GREATER_THAN, xyz[i], threshold);
        if (compared[i].socket == nullptr) {
          return discard();
        }
      }
      NodeOutput combined = combine_xyz_node_add(tree, grid_, nodes, compared);
      if (combined.socket == nullptr) {
        return discard();
      }

      if (!filter_result_mix_add(tree, grid_, nodes, content, combined, strength, effect.name)) {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_CLAMP: {
      /* `min(max(c, min), max)` per channel, the Math node's Clamp with both bounds as inputs, so
       * an edit of one is a value and not a different graph. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_CLAMP_PARAM_NUM ||
          (*params)[PAINT_LAYER_CLAMP_MIN] == nullptr ||
          (*params)[PAINT_LAYER_CLAMP_MAX] == nullptr)
      {
        return false;
      }
      bNodeSocket *min_socket = group_input_socket(group, *(*params)[PAINT_LAYER_CLAMP_MIN]);
      bNodeSocket *max_socket = group_input_socket(group, *(*params)[PAINT_LAYER_CLAMP_MAX]);
      if (min_socket == nullptr || max_socket == nullptr) {
        return false;
      }
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      bNode *lower = vector_math_node_add(
          tree, grid_, nodes, NODE_VECTOR_MATH_MAXIMUM, content, {group.group_input, min_socket});
      bNode *upper = (lower != nullptr) ?
                         vector_math_node_add(tree,
                                              grid_,
                                              nodes,
                                              NODE_VECTOR_MATH_MINIMUM,
                                              {lower, socket_out(*lower, "Vector")},
                                              {group.group_input, max_socket}) :
                         nullptr;
      if (upper == nullptr) {
        return discard();
      }

      if (!filter_result_mix_add(tree,
                                 grid_,
                                 nodes,
                                 content,
                                 {upper, socket_out(*upper, "Vector")},
                                 strength,
                                 effect.name))
      {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_CURVES: {
      /* `curves(c)` per channel, the row's mapping copied into the node so the generated tree owns
       * no reference. The node's own evaluation — each channel through its curve, no white
       * balancing — is the CPU's, and its Fac stays at one: the opacity mixes outside, like every
       * other filter. */
      const CurveMapping *curve_mapping = BKE_paint_layers_curves_get(effect);
      if (curve_mapping == nullptr) {
        return false;
      }
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      bNode *curves = bke::node_add_static_node(nullptr, tree, SH_NODE_CURVE_RGB);
      if (curves == nullptr || curves->storage == nullptr) {
        if (curves != nullptr) {
          nodes.append(curves);
        }
        return discard();
      }
      nodes.append(curves);
      grid_.place_extra(*curves);
      /* The node's own storage is a default mapping: free its points and take a deep copy of the
       * row's, tables included. */
      BKE_curvemapping_free_data(static_cast<CurveMapping *>(curves->storage));
      BKE_curvemapping_copy_data(static_cast<CurveMapping *>(curves->storage), curve_mapping);
      /* Key the node with the effect's marker, like a blend node carries `B:<marker>:<channel>`:
       * the curves sync finds the node by it and updates its storage in place, so a point edit
       * rebuilds no group. The shape stays out of the topology hash on purpose. */
      char curves_marker[UUID_STRING_SIZE];
      BLI_uuid_format(curves_marker, effect.marker);
      prop_string_set(curves->system_properties, CURVES_NODE_KEY_PROP, curves_marker);

      bNodeSocket *color_in = socket_in(*curves, "Color");
      bNodeSocket *color_out = socket_out(*curves, "Color");
      if (color_in == nullptr || color_out == nullptr) {
        return discard();
      }
      bke::node_add_link(tree, *content.node, *content.socket, *curves, *color_in);

      if (!filter_result_mix_add(
              tree, grid_, nodes, content, {curves, color_out}, strength, effect.name))
      {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_COLOR_BALANCE: {
      /* `pow(max(c * A + B, 0), E)` per channel: the folded triples the CPU composite reads, fed
       * from three Vector inputs of the group so an edit of one is a value and not a different
       * graph. The method the payload carries is not visible here — both fold into the same A, B
       * and E — and the MULTIPLY_ADD carries the linear part, the MAXIMUM the clamp at zero, and
       * one scalar POWER per component the exponent from its own triple. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_COLOR_BALANCE_TRIPLE_NUM ||
          (*params)[0] == nullptr || (*params)[1] == nullptr || (*params)[2] == nullptr)
      {
        return false;
      }
      bNodeSocket *a_socket = group_input_socket(group, *(*params)[0]);
      bNodeSocket *b_socket = group_input_socket(group, *(*params)[1]);
      bNodeSocket *e_socket = group_input_socket(group, *(*params)[2]);
      if (a_socket == nullptr || b_socket == nullptr || e_socket == nullptr) {
        return false;
      }
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      /* `c * A + B`: the second operand the helper links is A, the third is linked here. */
      bNode *scaled = vector_math_node_add(tree,
                                           grid_,
                                           nodes,
                                           NODE_VECTOR_MATH_MULTIPLY_ADD,
                                           content,
                                           {group.group_input, a_socket});
      if (scaled == nullptr) {
        return discard();
      }
      bNodeSocket *scaled_offset = socket_in(*scaled, "Vector_002");
      if (scaled_offset == nullptr) {
        return discard();
      }
      bke::node_add_link(tree, *group.group_input, *b_socket, *scaled, *scaled_offset);

      /* `max(., 0)`: the CPU composite and the shader clamp before the power, so a negative base
       * never reaches it. */
      bNode *lower = vector_math_node_add(tree,
                                          grid_,
                                          nodes,
                                          NODE_VECTOR_MATH_MAXIMUM,
                                          {scaled, socket_out(*scaled, "Vector")},
                                          {});
      if (lower == nullptr) {
        return discard();
      }
      bNodeSocket *lower_zero = socket_in(*lower, "Vector_001");
      if (lower_zero == nullptr || lower_zero->default_value == nullptr) {
        return discard();
      }
      zero_v3(static_cast<bNodeSocketValueVector *>(lower_zero->default_value)->value);

      NodeOutput clamped[3];
      if (!separate_xyz_node_add(tree, grid_, nodes, {lower, socket_out(*lower, "Vector")}, clamped))
      {
        return discard();
      }
      /* The exponents from their own triple, split once for all three channels. */
      NodeOutput exponents[3];
      if (!separate_xyz_node_add(tree, grid_, nodes, {group.group_input, e_socket}, exponents)) {
        return discard();
      }
      NodeOutput powered[3];
      for (const int i : IndexRange(3)) {
        powered[i] = math_node_add(tree, grid_, nodes, NODE_MATH_POWER, clamped[i], exponents[i]);
        if (powered[i].socket == nullptr) {
          return discard();
        }
      }
      NodeOutput combined = combine_xyz_node_add(tree, grid_, nodes, powered);
      if (combined.socket == nullptr) {
        return discard();
      }

      if (!filter_result_mix_add(tree, grid_, nodes, content, combined, strength, effect.name)) {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_EXPOSURE: {
      /* `lerp(c, (c * 2^exposure + offset)^(1 / gamma), opacity)`: the function the CPU composite
       * applies, every parameter an input of the group so an edit of one is a value and not a
       * different graph. The stop scale is a power of a constant two, and the Gamma node leaves a
       * non-positive value as it is -- which is also what keeps a colour above one above one at the
       * default gamma, so the filter brightens an HDR map instead of clipping what it holds. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_EXPOSURE_PARAM_NUM) {
        return false;
      }
      NodeOutput param[PAINT_LAYER_EXPOSURE_PARAM_NUM];
      for (const int i : IndexRange(PAINT_LAYER_EXPOSURE_PARAM_NUM)) {
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
        nodes_discard(tree, nodes);
        return false;
      };

      const NodeOutput two = value_node_add(tree, grid_, nodes, 2.0f);
      const NodeOutput scale = math_node_add(
          tree, grid_, nodes, NODE_MATH_POWER, two, param[PAINT_LAYER_EXPOSURE_EXPOSURE]);
      const NodeOutput inv_gamma = math_node_add(
          tree, grid_, nodes, NODE_MATH_DIVIDE, {}, param[PAINT_LAYER_EXPOSURE_GAMMA], 1.0f);
      if (scale.socket == nullptr || inv_gamma.socket == nullptr) {
        return discard();
      }
      bNode *shifted = vector_math_node_add(
          tree, grid_, nodes, NODE_VECTOR_MATH_MULTIPLY_ADD, content, scale);
      bNodeSocket *offset_in = (shifted != nullptr) ? socket_in(*shifted, "Vector_002") : nullptr;
      if (offset_in == nullptr) {
        return discard();
      }
      bke::node_add_link(tree,
                         *param[PAINT_LAYER_EXPOSURE_OFFSET].node,
                         *param[PAINT_LAYER_EXPOSURE_OFFSET].socket,
                         *shifted,
                         *offset_in);

      bNode *gamma = bke::node_add_static_node(nullptr, tree, SH_NODE_GAMMA);
      if (gamma == nullptr) {
        return discard();
      }
      nodes.append(gamma);
      grid_.place_extra(*gamma);
      bNodeSocket *gamma_color = socket_in(*gamma, "Color");
      bNodeSocket *gamma_exponent = socket_in(*gamma, "Gamma");
      bNodeSocket *gamma_out = socket_out(*gamma, "Color");
      if (gamma_color == nullptr || gamma_exponent == nullptr || gamma_out == nullptr) {
        return discard();
      }
      bke::node_add_link(
          tree, *shifted, *socket_out(*shifted, "Vector"), *gamma, *gamma_color);
      bke::node_add_link(
          tree, *inv_gamma.node, *inv_gamma.socket, *gamma, *gamma_exponent);

      if (!filter_result_mix_add(tree,
                                 grid_,
                                 nodes,
                                 content,
                                 {gamma, gamma_out},
                                 strength,
                                 effect.name))
      {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_DEBAND: {
      /* Two White Noise lookups of the same texel, one seed apart, make the triangular pair: their
       * difference is the signed noise of one quantization step, which the Deband lays under the
       * colour and the Dither under a rounding to whole levels. The shader hashes the UV and the
       * CPU composite the texel it walks, so the two agree to a tolerance and no closer -- which
       * is also why the combined pass falls back to the CPU, where the texel has coordinates. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_DEBAND_PARAM_NUM ||
          (*params)[PAINT_LAYER_DEBAND_SEED] == nullptr ||
          (*params)[PAINT_LAYER_DEBAND_BITS] == nullptr)
      {
        return false;
      }
      const NodeOutput seed{group.group_input,
                            group_input_socket(group, *(*params)[PAINT_LAYER_DEBAND_SEED])};
      const NodeOutput bits{group.group_input,
                            group_input_socket(group, *(*params)[PAINT_LAYER_DEBAND_BITS])};
      if (seed.socket == nullptr || bits.socket == nullptr) {
        return false;
      }
      /* The mode picks the nodes below, so it is read here and an edit of it rebuilds the tree
       * (it is part of the topology hash). */
      const int mode = int(paint_layer_kind_param_get(effect, PAINT_LAYER_DEBAND_MODE));
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      bNode *texcoord = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_COORD);
      if (texcoord != nullptr) {
        nodes.append(texcoord);
        grid_.place_extra(*texcoord);
      }
      bNode *noise_nodes[2] = {bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_WHITE_NOISE),
                               bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_WHITE_NOISE)};
      for (bNode *noise_node : noise_nodes) {
        if (noise_node != nullptr) {
          nodes.append(noise_node);
          grid_.place_extra(*noise_node);
        }
      }
      if (texcoord == nullptr || noise_nodes[0] == nullptr || noise_nodes[1] == nullptr) {
        return discard();
      }
      /* Four dimensions: the position carries the texel and the W the pass's own seed. */
      noise_nodes[0]->custom1 = 4;
      noise_nodes[1]->custom1 = 4;
      bNodeSocket *texcoord_out = socket_out(*texcoord, "Generated");
      bNode *seed_b_node = math_constant_node_add(tree, grid_, nodes, NODE_MATH_ADD, seed, 1.0f);
      if (texcoord_out == nullptr || seed_b_node == nullptr) {
        return discard();
      }
      const NodeOutput seeds[2] = {seed, {seed_b_node, socket_out(*seed_b_node, "Value")}};
      NodeOutput noise_value[2];
      for (const int i : IndexRange(2)) {
        bNodeSocket *vector_in = socket_in(*noise_nodes[i], "Vector");
        bNodeSocket *w_in = socket_in(*noise_nodes[i], "W");
        bNodeSocket *value_out = socket_out(*noise_nodes[i], "Value");
        if (vector_in == nullptr || w_in == nullptr || value_out == nullptr ||
            seeds[i].socket == nullptr)
        {
          return discard();
        }
        bke::node_add_link(tree, *texcoord, *texcoord_out, *noise_nodes[i], *vector_in);
        bke::node_add_link(tree, *seeds[i].node, *seeds[i].socket, *noise_nodes[i], *w_in);
        noise_value[i] = {noise_nodes[i], value_out};
      }

      /* The signed noise of one step, and the step itself: `levels = 2^bits - 1`. */
      const NodeOutput diff = math_node_add(
          tree, grid_, nodes, NODE_MATH_SUBTRACT, noise_value[0], noise_value[1]);
      const NodeOutput two = value_node_add(tree, grid_, nodes, 2.0f);
      const NodeOutput pow2 = math_node_add(tree, grid_, nodes, NODE_MATH_POWER, two, bits);
      bNode *levels_node = (pow2.socket != nullptr) ?
                               math_constant_node_add(
                                   tree, grid_, nodes, NODE_MATH_SUBTRACT, pow2, 1.0f) :
                               nullptr;
      if (diff.socket == nullptr || levels_node == nullptr) {
        return discard();
      }
      const NodeOutput levels{levels_node, socket_out(*levels_node, "Value")};
      const NodeOutput diff_xyz[3] = {diff, diff, diff};
      const NodeOutput levels_xyz[3] = {levels, levels, levels};
      const NodeOutput diff_vec = combine_xyz_node_add(tree, grid_, nodes, diff_xyz);
      const NodeOutput levels_vec = combine_xyz_node_add(tree, grid_, nodes, levels_xyz);
      if (diff_vec.socket == nullptr || levels_vec.socket == nullptr) {
        return discard();
      }

      bNode *quantized = nullptr;
      if (mode == PAINT_LAYER_DEBAND_DITHER) {
        /* `floor(c * levels + noise + 0.5) / levels`: the Posterize chain with the noise beside
         * the half. */
        bNode *scaled = vector_math_node_add(
            tree, grid_, nodes, NODE_VECTOR_MATH_MULTIPLY_ADD, content, levels_vec);
        bNodeSocket *noise_in = (scaled != nullptr) ? socket_in(*scaled, "Vector_002") : nullptr;
        if (noise_in == nullptr) {
          return discard();
        }
        bke::node_add_link(tree, *diff_vec.node, *diff_vec.socket, *scaled, *noise_in);
        bNode *shifted = vector_math_node_add(tree,
                                              grid_,
                                              nodes,
                                              NODE_VECTOR_MATH_ADD,
                                              {scaled, socket_out(*scaled, "Vector")},
                                              {});
        bNodeSocket *half = (shifted != nullptr) ? socket_in(*shifted, "Vector_001") : nullptr;
        if (half == nullptr || half->default_value == nullptr) {
          return discard();
        }
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(half->default_value)->value, 0.5f);
        bNode *floored = vector_math_node_add(tree,
                                              grid_,
                                              nodes,
                                              NODE_VECTOR_MATH_FLOOR,
                                              {shifted, socket_out(*shifted, "Vector")},
                                              {});
        quantized = (floored != nullptr) ?
                        vector_math_node_add(tree,
                                             grid_,
                                             nodes,
                                             NODE_VECTOR_MATH_DIVIDE,
                                             {floored, socket_out(*floored, "Vector")},
                                             levels_vec) :
                        nullptr;
      }
      else {
        /* `c + noise * 1.5 / levels`: the triangular pair reaches one and a half steps either way. */
        const NodeOutput scale = math_node_add(
            tree, grid_, nodes, NODE_MATH_DIVIDE, {}, levels, 1.5f);
        const NodeOutput scale_xyz[3] = {scale, scale, scale};
        const NodeOutput scale_vec = (scale.socket != nullptr) ?
                                         combine_xyz_node_add(tree, grid_, nodes, scale_xyz) :
                                         NodeOutput{};
        bNode *shifted = (scale_vec.socket != nullptr) ?
                             vector_math_node_add(tree,
                                                  grid_,
                                                  nodes,
                                                  NODE_VECTOR_MATH_MULTIPLY_ADD,
                                                  diff_vec,
                                                  scale_vec) :
                             nullptr;
        bNodeSocket *base_in = (shifted != nullptr) ? socket_in(*shifted, "Vector_002") : nullptr;
        if (base_in == nullptr) {
          return discard();
        }
        bke::node_add_link(tree, *content.node, *content.socket, *shifted, *base_in);
        quantized = shifted;
      }
      if (quantized == nullptr) {
        return discard();
      }
      /* The CPU clamps the result into [0, 1], so the clamps are set explicitly. */
      bNode *lower = vector_math_node_add(tree,
                                          grid_,
                                          nodes,
                                          NODE_VECTOR_MATH_MAXIMUM,
                                          {quantized, socket_out(*quantized, "Vector")},
                                          {});
      bNode *upper = (lower != nullptr) ?
                         vector_math_node_add(tree,
                                              grid_,
                                              nodes,
                                              NODE_VECTOR_MATH_MINIMUM,
                                              {lower, socket_out(*lower, "Vector")},
                                              {}) :
                         nullptr;
      bNodeSocket *lower_b = (lower != nullptr) ? socket_in(*lower, "Vector_001") : nullptr;
      bNodeSocket *upper_b = (upper != nullptr) ? socket_in(*upper, "Vector_001") : nullptr;
      if (lower_b == nullptr || lower_b->default_value == nullptr || upper_b == nullptr ||
          upper_b->default_value == nullptr)
      {
        return discard();
      }
      zero_v3(static_cast<bNodeSocketValueVector *>(lower_b->default_value)->value);
      copy_v3_fl(static_cast<bNodeSocketValueVector *>(upper_b->default_value)->value, 1.0f);

      if (!filter_result_mix_add(tree,
                                 grid_,
                                 nodes,
                                 content,
                                 {upper, socket_out(*upper, "Vector")},
                                 strength,
                                 effect.name))
      {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_NORMAL_CONVERT: {
      /* The mode's conversion over the encoded channels, the optional normalize on top, and the
       * opacity last. Both the mode and the normalize are read when the graph is built -- an edit
       * of either is a shape edit (the enum hash) and the tree is rebuilt -- while the CPU
       * composite and the graph carry the same constants, so the pair stays the one broken line. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != PAINT_LAYER_NORMAL_CONVERT_PARAM_NUM) {
        return false;
      }
      const int mode = int(paint_layer_kind_param_get(effect, PAINT_LAYER_NORMAL_CONVERT_MODE));
      const bool normalize =
          int(paint_layer_kind_param_get(effect, PAINT_LAYER_NORMAL_CONVERT_NORMALIZE)) != 0;
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };
      /* The scalar Math helpers, null-propagating so a chain checks its result once at the end. */
      const auto constant_op = [&](const int operation, const NodeOutput a, const float b) {
        bNode *node = (a.socket != nullptr) ?
                          math_constant_node_add(tree, grid_, nodes, operation, a, b) :
                          nullptr;
        return NodeOutput{node, (node != nullptr) ? socket_out(*node, "Value") : nullptr};
      };
      const auto binary_op = [&](const int operation, const NodeOutput a, const NodeOutput b) {
        return (a.socket != nullptr && b.socket != nullptr) ?
                   math_node_add(tree, grid_, nodes, operation, a, b) :
                   NodeOutput{};
      };
      /* One channel through the IEC 61966-2.1 broken line: the linear piece under the threshold,
       * the power piece over it, mixed by the comparison. The power reads a non-negative base, so
       * a value past the range cannot make it NaN (the CPU leaves a negative one on the linear
       * piece, which is the one place the pair differs, and only outside [0, 1]). */
      const auto srgb_channel = [&](const NodeOutput x, const bool encode) {
        const NodeOutput base = constant_op(NODE_MATH_MAXIMUM, x, 0.0f);
        NodeOutput low_piece;
        NodeOutput high_piece;
        float threshold = 0.0f;
        if (encode) {
          low_piece = constant_op(NODE_MATH_MULTIPLY, base, 12.92f);
          const NodeOutput exponent = value_node_add(tree, grid_, nodes, 1.0f / 2.4f);
          const NodeOutput powered = binary_op(NODE_MATH_POWER, base, exponent);
          high_piece = constant_op(
              NODE_MATH_SUBTRACT, constant_op(NODE_MATH_MULTIPLY, powered, 1.055f), 0.055f);
          threshold = 0.0031308f;
        }
        else {
          low_piece = constant_op(NODE_MATH_DIVIDE, base, 12.92f);
          const NodeOutput ratio = constant_op(
              NODE_MATH_DIVIDE, constant_op(NODE_MATH_ADD, base, 0.055f), 1.055f);
          const NodeOutput exponent = value_node_add(tree, grid_, nodes, 2.4f);
          high_piece = binary_op(NODE_MATH_POWER, ratio, exponent);
          threshold = 0.04045f;
        }
        const NodeOutput threshold_value = value_node_add(tree, grid_, nodes, threshold);
        const NodeOutput under = binary_op(NODE_MATH_LESS_THAN, base, threshold_value);
        const NodeOutput over = (under.socket != nullptr) ?
                                    math_node_add(
                                        tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, under, 1.0f) :
                                    NodeOutput{};
        return binary_op(NODE_MATH_ADD,
                         binary_op(NODE_MATH_MULTIPLY, low_piece, under),
                         binary_op(NODE_MATH_MULTIPLY, high_piece, over));
      };

      NodeOutput converted;
      if (ELEM(mode, PAINT_LAYER_NORMAL_CONVERT_DX_TO_GL, PAINT_LAYER_NORMAL_CONVERT_GL_TO_DX)) {
        /* `g = 1 - g`: the one flip, whichever way the convention goes. */
        NodeOutput xyz[3];
        if (!separate_xyz_node_add(tree, grid_, nodes, content, xyz)) {
          return discard();
        }
        const NodeOutput flipped = math_node_add(
            tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, xyz[1], 1.0f);
        const NodeOutput out_xyz[3] = {xyz[0], flipped, xyz[2]};
        converted = (flipped.socket != nullptr) ?
                        combine_xyz_node_add(tree, grid_, nodes, out_xyz) :
                        NodeOutput{};
      }
      else if (mode == PAINT_LAYER_NORMAL_CONVERT_REBUILD_Z) {
        /* `v = 2c - 1`, `z = sqrt(max(1 - x² - y², 0))`, `b = (z + 1) / 2`: the unit length the
         * two channels leave. */
        bNode *decoded = vector_math_node_add(
            tree, grid_, nodes, NODE_VECTOR_MATH_MULTIPLY_ADD, content, {});
        bNodeSocket *two = (decoded != nullptr) ? socket_in(*decoded, "Vector_001") : nullptr;
        bNodeSocket *minus = (decoded != nullptr) ? socket_in(*decoded, "Vector_002") : nullptr;
        if (two == nullptr || two->default_value == nullptr || minus == nullptr ||
            minus->default_value == nullptr)
        {
          return discard();
        }
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(two->default_value)->value, 2.0f);
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(minus->default_value)->value, -1.0f);
        NodeOutput xyz[3];
        if (!separate_xyz_node_add(
                tree, grid_, nodes, {decoded, socket_out(*decoded, "Vector")}, xyz))
        {
          return discard();
        }
        const NodeOutput xx = binary_op(NODE_MATH_MULTIPLY, xyz[0], xyz[0]);
        const NodeOutput yy = binary_op(NODE_MATH_MULTIPLY, xyz[1], xyz[1]);
        const NodeOutput sum = binary_op(NODE_MATH_ADD, xx, yy);
        const NodeOutput rest = (sum.socket != nullptr) ?
                                    math_node_add(
                                        tree, grid_, nodes, NODE_MATH_SUBTRACT, {}, sum, 1.0f) :
                                    NodeOutput{};
        const NodeOutput root = constant_op(
            NODE_MATH_SQRT, constant_op(NODE_MATH_MAXIMUM, rest, 0.0f), 0.0f);
        const NodeOutput encoded = constant_op(
            NODE_MATH_ADD, constant_op(NODE_MATH_MULTIPLY, root, 0.5f), 0.5f);
        const NodeOutput out_xyz[3] = {xyz[0], xyz[1], encoded};
        converted = (encoded.socket != nullptr) ?
                        combine_xyz_node_add(tree, grid_, nodes, out_xyz) :
                        NodeOutput{};
      }
      else if (mode == PAINT_LAYER_NORMAL_CONVERT_CLEAR_Z) {
        /* The two-channel form: the blue dropped, red and green as they are. */
        NodeOutput xyz[3];
        if (!separate_xyz_node_add(tree, grid_, nodes, content, xyz)) {
          return discard();
        }
        const NodeOutput zero = value_node_add(tree, grid_, nodes, 0.0f);
        const NodeOutput out_xyz[3] = {xyz[0], xyz[1], zero};
        converted = (zero.socket != nullptr) ? combine_xyz_node_add(tree, grid_, nodes, out_xyz) :
                                               NodeOutput{};
      }
      else {
        NodeOutput xyz[3];
        if (!separate_xyz_node_add(tree, grid_, nodes, content, xyz)) {
          return discard();
        }
        const bool encode = mode == PAINT_LAYER_NORMAL_CONVERT_LINEAR_TO_SRGB;
        NodeOutput out_xyz[3];
        for (const int k : IndexRange(3)) {
          out_xyz[k] = srgb_channel(xyz[k], encode);
          if (out_xyz[k].socket == nullptr) {
            return discard();
          }
        }
        converted = combine_xyz_node_add(tree, grid_, nodes, out_xyz);
      }
      if (converted.socket == nullptr) {
        return discard();
      }
      if (normalize) {
        /* The result decoded, brought back to unit length and encoded again: the one step the
         * modes share, and the one the CPU floors at a tiny length. */
        bNode *decoded = vector_math_node_add(
            tree, grid_, nodes, NODE_VECTOR_MATH_MULTIPLY_ADD, converted, {});
        bNode *brought = (decoded != nullptr) ?
                             vector_math_node_add(tree,
                                                  grid_,
                                                  nodes,
                                                  NODE_VECTOR_MATH_NORMALIZE,
                                                  {decoded, socket_out(*decoded, "Vector")},
                                                  {}) :
                             nullptr;
        bNode *encoded = (brought != nullptr) ?
                             vector_math_node_add(tree,
                                                  grid_,
                                                  nodes,
                                                  NODE_VECTOR_MATH_MULTIPLY_ADD,
                                                  {brought, socket_out(*brought, "Vector")},
                                                  {}) :
                             nullptr;
        bNodeSocket *two = (decoded != nullptr) ? socket_in(*decoded, "Vector_001") : nullptr;
        bNodeSocket *minus = (decoded != nullptr) ? socket_in(*decoded, "Vector_002") : nullptr;
        bNodeSocket *half = (encoded != nullptr) ? socket_in(*encoded, "Vector_001") : nullptr;
        bNodeSocket *shift = (encoded != nullptr) ? socket_in(*encoded, "Vector_002") : nullptr;
        if (two == nullptr || minus == nullptr || half == nullptr || shift == nullptr ||
            two->default_value == nullptr || minus->default_value == nullptr ||
            half->default_value == nullptr || shift->default_value == nullptr)
        {
          return discard();
        }
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(two->default_value)->value, 2.0f);
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(minus->default_value)->value, -1.0f);
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(half->default_value)->value, 0.5f);
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(shift->default_value)->value, 0.5f);
        converted = {encoded, socket_out(*encoded, "Vector")};
      }

      if (!filter_result_mix_add(tree, grid_, nodes, content, converted, strength, effect.name)) {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_CHANNEL_MIXER: {
      /* `clamp(dot(row_k, c) + constant_k, 0, 1)` per output channel: the folded matrix the CPU
       * composite reads (monochrome already in the rows), fed from four Vector inputs of the group
       * -- the three rows and the constants -- so an edit of any of them is a value and not a
       * different graph. The clamp is explicit: the fold has none and the addon's result is clipped
       * into [0, 1]. */
      Vector<bNodeTreeInterfaceSocket *> *params = effect_param_inputs_.lookup_ptr(&effect);
      if (params == nullptr || params->size() != 4) {
        return false;
      }
      bNodeSocket *input_sockets[4];
      for (const int i : IndexRange(4)) {
        input_sockets[i] = ((*params)[i] != nullptr) ? group_input_socket(group, *(*params)[i]) :
                                                       nullptr;
        if (input_sockets[i] == nullptr) {
          return false;
        }
      }
      Vector<bNode *> nodes;
      const auto discard = [&]() {
        nodes_discard(tree, nodes);
        return false;
      };

      /* The constants split once for the three channels. */
      NodeOutput constants[3];
      if (!separate_xyz_node_add(
              tree, grid_, nodes, {group.group_input, input_sockets[3]}, constants))
      {
        return discard();
      }
      NodeOutput sums[3];
      for (const int k : IndexRange(3)) {
        bNode *dot = vector_math_node_add(tree,
                                          grid_,
                                          nodes,
                                          NODE_VECTOR_MATH_DOT_PRODUCT,
                                          content,
                                          {group.group_input, input_sockets[k]});
        bNodeSocket *dot_out = (dot != nullptr) ? socket_out(*dot, "Value") : nullptr;
        if (dot_out == nullptr) {
          return discard();
        }
        sums[k] = math_node_add(tree, grid_, nodes, NODE_MATH_ADD, {dot, dot_out}, constants[k]);
        if (sums[k].socket == nullptr) {
          return discard();
        }
      }
      const NodeOutput combined = combine_xyz_node_add(tree, grid_, nodes, sums);
      if (combined.socket == nullptr) {
        return discard();
      }

      bNode *lower = vector_math_node_add(
          tree, grid_, nodes, NODE_VECTOR_MATH_MAXIMUM, combined, {});
      bNode *upper = (lower != nullptr) ?
                         vector_math_node_add(tree,
                                              grid_,
                                              nodes,
                                              NODE_VECTOR_MATH_MINIMUM,
                                              {lower, socket_out(*lower, "Vector")},
                                              {}) :
                         nullptr;
      bNodeSocket *lower_b = (lower != nullptr) ? socket_in(*lower, "Vector_001") : nullptr;
      bNodeSocket *upper_b = (upper != nullptr) ? socket_in(*upper, "Vector_001") : nullptr;
      if (lower_b == nullptr || lower_b->default_value == nullptr || upper_b == nullptr ||
          upper_b->default_value == nullptr)
      {
        return discard();
      }
      zero_v3(static_cast<bNodeSocketValueVector *>(lower_b->default_value)->value);
      copy_v3_fl(static_cast<bNodeSocketValueVector *>(upper_b->default_value)->value, 1.0f);

      if (!filter_result_mix_add(tree,
                                 grid_,
                                 nodes,
                                 content,
                                 {upper, socket_out(*upper, "Vector")},
                                 strength,
                                 effect.name))
      {
        return discard();
      }
      return true;
    }
    case MA_PAINT_LAYER_FILTER_COLOR_ID:
    case MA_PAINT_LAYER_FILTER_UV_ISLAND:
      /* Map-coverage operators only exist in a mask, where #map_coverage_match_add builds them. */
      break;
    default:
      /* A CPU-computed effect is left out of the live graph: as a correction #correction_lay never
       * hands it here, and as a mask item the grey it computes is read like a map once its bake
       * exists -- until then the item lays nothing, so false is its answer too. The registry
       * decides, so a new one needs no case here. */
      BLI_assert(!(channel >= 0 && paint_layer_effect_is_cpu_computed(effect)));
      break;
  }
  return false;
}

}  // namespace bke::paint_layers
}  // namespace blender
