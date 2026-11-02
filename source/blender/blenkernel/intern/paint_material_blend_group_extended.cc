/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The node clusters of the ".PBR Blend" group for the modes a single Mix node cannot express
 * (#eMaterialPaintBlend), split out of `paint_material_blend_group.cc` where only the group's
 * interface, its Mix-node branches and its Mode dispatcher live.
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_node.hh"

#include "BLI_index_range.hh"
#include "BLI_ustring.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "paint_material_blend_group_internal.hh"
#include "paint_material_composite_internal.hh"

namespace blender {

bNode *blend_group_mix_node_rgba_add(bNodeTree &group,
                                     const int blend_type,
                                     const float location_x,
                                     const float location_y)
{
  bNode *node = bke::node_add_node(nullptr, group, "ShaderNodeMix"_ustr);
  if (node == nullptr || node->storage == nullptr) {
    return nullptr;
  }
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(node->storage);
  storage->data_type = SOCK_RGBA;
  storage->factor_mode = NODE_MIX_MODE_UNIFORM;
  storage->blend_type = blend_type;
  storage->clamp_factor = true;
  storage->clamp_result = false;
  node->location[0] = location_x;
  node->location[1] = location_y;
  return node;
}

static bNode *math_node_add(bNodeTree &group,
                            const int operation,
                            const float location_x,
                            const float location_y)
{
  bNode *node = bke::node_add_node(nullptr, group, "ShaderNodeMath"_ustr);
  if (node == nullptr) {
    return nullptr;
  }
  node->custom1 = operation;
  node->location[0] = location_x;
  node->location[1] = location_y;
  return node;
}

static bNode *vector_math_node_add(bNodeTree &group,
                                   const int operation,
                                   const float location_x,
                                   const float location_y)
{
  bNode *node = bke::node_add_node(nullptr, group, "ShaderNodeVectorMath"_ustr);
  if (node == nullptr) {
    return nullptr;
  }
  node->custom1 = operation;
  node->location[0] = location_x;
  node->location[1] = location_y;
  return node;
}

bool blend_group_socket_float_default_set(bNode &node,
                                          const char *identifier,
                                          const float value)
{
  bNodeSocket *socket = bke::node_find_socket(
      node, SOCK_IN, UString::from_ptr_noinline(identifier));
  if (socket == nullptr || socket->type != SOCK_FLOAT || socket->default_value == nullptr) {
    return false;
  }
  static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = value;
  return true;
}

bool blend_group_socket_vector_default_set(bNode &node,
                                           const char *identifier,
                                           const float x,
                                           const float y,
                                           const float z)
{
  bNodeSocket *socket = bke::node_find_socket(
      node, SOCK_IN, UString::from_ptr_noinline(identifier));
  if (socket == nullptr || socket->type != SOCK_VECTOR || socket->default_value == nullptr) {
    return false;
  }
  auto &value = *static_cast<bNodeSocketValueVector *>(socket->default_value);
  value.value[0] = x;
  value.value[1] = y;
  value.value[2] = z;
  return true;
}

BlendBranch blend_branch_extended_add(bNodeTree &group,
                                      bNode &group_input,
                                      const BlendLinkFn &link,
                                      const int mode,
                                      const float location_x,
                                      const float location_y)
{
  bNode *full = nullptr;
  const char *full_output = nullptr;
  float x = location_x;
  float y = location_y;
  constexpr float step = 200.0f;

  switch (mode) {
    case MA_PAINT_BLEND_HARD_LIGHT: {
      /* Overlay is the base deciding which half of the curve; exchanging the two colours makes the
       * layer decide, which is Hard Light. */
      bNode *swap = blend_group_mix_node_rgba_add(group, MA_RAMP_OVERLAY, x, y);
      if (swap == nullptr ||
          !link(&group_input, BLEND_GROUP_ID_B, swap, BLEND_NODE_MIX_A) ||
          !link(&group_input, BLEND_GROUP_ID_A, swap, BLEND_NODE_MIX_B) ||
          !blend_group_socket_float_default_set(*swap, BLEND_NODE_MIX_FACTOR, 1.0f))
      {
        return {};
      }
      full = swap;
      full_output = BLEND_NODE_MIX_RESULT;
      break;
    }
    case MA_PAINT_BLEND_LINEAR_BURN: {
      /* `below + layer - 1`, unclamped: the encode at the end of the chain is what brings a byte
       * result back inside 0..1. */
      bNode *sum = vector_math_node_add(group, NODE_VECTOR_MATH_ADD, x, y);
      x += step;
      bNode *shift = vector_math_node_add(group, NODE_VECTOR_MATH_SUBTRACT, x, y);
      if (sum == nullptr || shift == nullptr ||
          !link(&group_input,
                BLEND_GROUP_ID_A,
                sum,
                BLEND_NODE_VECTOR_MATH_A) ||
          !link(&group_input,
                BLEND_GROUP_ID_B,
                sum,
                BLEND_NODE_VECTOR_MATH_B) ||
          !link(sum, BLEND_NODE_VECTOR_MATH_VALUE, shift, BLEND_NODE_VECTOR_MATH_A) ||
          !blend_group_socket_vector_default_set(
              *shift, BLEND_NODE_VECTOR_MATH_B, 1.0f, 1.0f, 1.0f))
      {
        return {};
      }
      full = shift;
      full_output = BLEND_NODE_VECTOR_MATH_VALUE;
      break;
    }
    case MA_PAINT_BLEND_VIVID_LIGHT:
    case MA_PAINT_BLEND_PIN_LIGHT: {
      /* Two halves, chosen per channel on the layer crossing 0.5: the soft half runs the layer at
       * twice its value, the hard half at twice its value less one. */
      const bool vivid = mode == MA_PAINT_BLEND_VIVID_LIGHT;
      const int soft_blend = vivid ? MA_RAMP_BURN : MA_RAMP_DARK;
      const int hard_blend = vivid ? MA_RAMP_DODGE : MA_RAMP_LIGHT;

      bNode *doubled = vector_math_node_add(group, NODE_VECTOR_MATH_SCALE, x, y);
      x += step;
      bNode *shifted = vector_math_node_add(group, NODE_VECTOR_MATH_SUBTRACT, x, y);
      x += step;
      bNode *soft = blend_group_mix_node_rgba_add(group, soft_blend, x, y);
      x += step;
      bNode *hard = blend_group_mix_node_rgba_add(group, hard_blend, x, y);
      x += step;
      bNode *layer_rgb = nullptr;
      bNode *mask = nullptr;
      if (doubled == nullptr || shifted == nullptr || soft == nullptr || hard == nullptr) {
        return {};
      }
      if (!link(&group_input,
                BLEND_GROUP_ID_B,
                doubled,
                BLEND_NODE_VECTOR_MATH_A) ||
          !blend_group_socket_float_default_set(*doubled, BLEND_NODE_VECTOR_MATH_SCALE, 2.0f) ||
          !link(doubled, BLEND_NODE_VECTOR_MATH_VALUE, shifted, BLEND_NODE_VECTOR_MATH_A) ||
          !blend_group_socket_vector_default_set(
              *shifted, BLEND_NODE_VECTOR_MATH_B, 1.0f, 1.0f, 1.0f))
      {
        return {};
      }
      /* Both halves at full strength; the choice below is what weights them. */
      if (!blend_group_socket_float_default_set(*soft, BLEND_NODE_MIX_FACTOR, 1.0f) ||
          !blend_group_socket_float_default_set(*hard, BLEND_NODE_MIX_FACTOR, 1.0f) ||
          !link(&group_input, BLEND_GROUP_ID_A, soft, BLEND_NODE_MIX_A) ||
          !link(doubled, BLEND_NODE_VECTOR_MATH_VALUE, soft, BLEND_NODE_MIX_B) ||
          !link(&group_input, BLEND_GROUP_ID_A, hard, BLEND_NODE_MIX_A) ||
          !link(shifted, BLEND_NODE_VECTOR_MATH_VALUE, hard, BLEND_NODE_MIX_B))
      {
        return {};
      }
      /* The mask: one compare per channel of the layer against 0.5, gathered into the vector the
       * per-element Mix reads. */
      layer_rgb = bke::node_add_node(nullptr, group, "ShaderNodeSeparateColor"_ustr);
      if (layer_rgb == nullptr) {
        return {};
      }
      layer_rgb->location[0] = location_x;
      layer_rgb->location[1] = y - 260.0f;
      if (!link(&group_input,
                BLEND_GROUP_ID_B,
                layer_rgb,
                BLEND_NODE_SEPARATE_COLOR)) {
        return {};
      }
      mask = bke::node_add_node(nullptr, group, "ShaderNodeCombineXYZ"_ustr);
      if (mask == nullptr) {
        return {};
      }
      mask->location[0] = x;
      mask->location[1] = y - 260.0f;
      const char *components[3] = {
          BLEND_NODE_SEPARATE_RED, BLEND_NODE_SEPARATE_GREEN, BLEND_NODE_SEPARATE_BLUE};
      const char *axes[3] = {BLEND_NODE_COMBINE_X, BLEND_NODE_COMBINE_Y, BLEND_NODE_COMBINE_Z};
      float compare_x = x - step;
      for (const int i : IndexRange(3)) {
        bNode *compare = math_node_add(
            group, NODE_MATH_LESS_THAN, compare_x, y - 260.0f - i * 90.0f);
        compare_x -= step;
        if (compare == nullptr || !link(layer_rgb, components[i], compare, BLEND_NODE_MATH_A) ||
            !blend_group_socket_float_default_set(*compare, BLEND_NODE_MATH_B, 0.5f) ||
            !link(compare, BLEND_NODE_MATH_VALUE, mask, axes[i]))
        {
          return {};
        }
      }
      /* A factor of one takes the second colour, so the half the mask selects is the one wired to
       * B: the soft branch where the layer is under a half. */
      bNode *choice = blend_group_mix_node_rgba_add(group, MA_RAMP_BLEND, x, y);
      if (choice == nullptr) {
        return {};
      }
      static_cast<NodeShaderMix *>(choice->storage)->factor_mode = NODE_MIX_MODE_NON_UNIFORM;
      if (!link(mask, BLEND_NODE_COMBINE_VALUE, choice, BLEND_NODE_MIX_FACTOR_VECTOR) ||
          !link(hard, BLEND_NODE_MIX_RESULT, choice, BLEND_NODE_MIX_A) ||
          !link(soft, BLEND_NODE_MIX_RESULT, choice, BLEND_NODE_MIX_B))
      {
        return {};
      }
      full = choice;
      full_output = BLEND_NODE_MIX_RESULT;
      break;
    }
    case MA_PAINT_BLEND_LIGHTER_COLOR:
    case MA_PAINT_BLEND_DARKER_COLOR: {
      /* The whole colour with the greater (or lesser) RGB sum wins -- a decision about the colour,
       * not about each channel, so a single compare drives one uniform Mix. */
      bNode *sum_below = vector_math_node_add(group, NODE_VECTOR_MATH_DOT_PRODUCT, x, y);
      x += step;
      bNode *sum_layer = vector_math_node_add(group, NODE_VECTOR_MATH_DOT_PRODUCT, x, y);
      x += step;
      bNode *compare = math_node_add(
          group,
          mode == MA_PAINT_BLEND_LIGHTER_COLOR ? NODE_MATH_GREATER_THAN : NODE_MATH_LESS_THAN,
          x,
          y);
      x += step;
      bNode *choice = blend_group_mix_node_rgba_add(group, MA_RAMP_BLEND, x, y);
      if (sum_below == nullptr || sum_layer == nullptr || compare == nullptr || choice == nullptr) {
        return {};
      }
      if (!blend_group_socket_vector_default_set(
              *sum_below, BLEND_NODE_VECTOR_MATH_B, 1.0f, 1.0f, 1.0f) ||
          !blend_group_socket_vector_default_set(
              *sum_layer, BLEND_NODE_VECTOR_MATH_B, 1.0f, 1.0f, 1.0f) ||
          !link(&group_input,
                BLEND_GROUP_ID_A,
                sum_below,
                BLEND_NODE_VECTOR_MATH_A) ||
          !link(&group_input,
                BLEND_GROUP_ID_B,
                sum_layer,
                BLEND_NODE_VECTOR_MATH_A))
      {
        return {};
      }
      if (!link(sum_layer, BLEND_NODE_VECTOR_MATH_VALUE, compare, BLEND_NODE_MATH_A) ||
          !link(sum_below, BLEND_NODE_VECTOR_MATH_VALUE, compare, BLEND_NODE_MATH_B) ||
          !link(compare, BLEND_NODE_MATH_VALUE, choice, BLEND_NODE_MIX_FACTOR) ||
          !link(&group_input, BLEND_GROUP_ID_A, choice, BLEND_NODE_MIX_A) ||
          !link(&group_input, BLEND_GROUP_ID_B, choice, BLEND_NODE_MIX_B))
      {
        return {};
      }
      full = choice;
      full_output = BLEND_NODE_MIX_RESULT;
      break;
    }
    default:
      return {};
  }

  /* The formula applied by the row's own factor, over what is below. */
  bNode *apply = blend_group_mix_node_rgba_add(group, MA_RAMP_BLEND, x + step, location_y);
  if (apply == nullptr ||
      !link(&group_input, BLEND_GROUP_ID_A, apply, BLEND_NODE_MIX_A) ||
      !link(&group_input, BLEND_GROUP_ID_FACTOR, apply, BLEND_NODE_MIX_FACTOR) ||
      !link(full, full_output, apply, BLEND_NODE_MIX_B))
  {
    return {};
  }
  return {mode, apply, BLEND_NODE_MIX_RESULT};
}

}  // namespace blender
