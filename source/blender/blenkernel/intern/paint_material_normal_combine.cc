/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLT_translation.hh"

#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include <cstring>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Normal Combine Group
 * \{ */

/** Name of the shared Normal Combine group data-block. NOTE: do not translate. */
static const char *NORMAL_COMBINE_TREE_NAME = "PBR Normal Combine";

namespace {

/**
 * The IDProperty marking the trees this module recognizes as one of its own groups, told apart by
 * value. Kept local here: the Normal Combine group is the only group the new path marks.
 */
constexpr const char *NODE_GROUP_MARKER_PROP = "pbr_paint_node_group";
/** #NODE_GROUP_MARKER_PROP value of the Normal Combine group. */
constexpr const char *NORMAL_COMBINE_MARKER_VALUE = "NORMAL_COMBINE";

/** The #NODE_GROUP_MARKER_PROP value on \a tree_id, or null when unmarked. */
const char *node_tree_group_marker_get(const ID &tree_id)
{
  const IDProperty *properties = IDP_GetProperties(const_cast<ID *>(&tree_id));
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
      properties, NODE_GROUP_MARKER_PROP, IDP_STRING);
  return (marker != nullptr) ? IDP_string_get(marker) : nullptr;
}

/** Stamp the shared Normal Combine group tree. */
void normal_combine_tree_marker_set(bNodeTree &group)
{
  IDProperty *properties = IDP_EnsureProperties(&group.id);
  IDPropertyTemplate value = {0};
  value.string.str = const_cast<char *>(NORMAL_COMBINE_MARKER_VALUE);
  value.string.len = int(strlen(NORMAL_COMBINE_MARKER_VALUE)) + 1;
  value.string.subtype = IDP_STRING_SUB_UTF8;
  IDP_AddToGroup(properties, IDP_New(IDP_STRING, &value, NODE_GROUP_MARKER_PROP));
}

}  // namespace

bool BKE_paint_material_is_normal_combine_group(const bNode &node)
{
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    return false;
  }
  const char *marker = node_tree_group_marker_get(*node.id);
  return marker != nullptr && STREQ(marker, NORMAL_COMBINE_MARKER_VALUE);
}

/** The group already in \a bmain, or null. Found by its marker, so a rename does not lose it. */
static bNodeTree *normal_combine_group_find(Main &bmain)
{
  for (bNodeTree &ntree : bmain.nodetrees) {
    const char *marker = node_tree_group_marker_get(ntree.id);
    if (marker != nullptr && STREQ(marker, NORMAL_COMBINE_MARKER_VALUE)) {
      return &ntree;
    }
  }
  return nullptr;
}

/** A Vector Math node set to \a operation. */
static bNode *normal_combine_vector_math_add(bNodeTree &group,
                                             const int operation,
                                             const float2 location)
{
  bNode *node = bke::node_add_node(nullptr, group, "ShaderNodeVectorMath"_ustr);
  node->custom1 = operation;
  node->location[0] = location.x;
  node->location[1] = location.y;
  return node;
}

/** Sets the second and third Vector inputs of a Multiply Add, which decode or encode a normal. */
static void normal_combine_range_map_set(bNode &node, const float scale, const float offset)
{
  bNodeSocket *scale_socket = bke::node_find_socket(node, SOCK_IN, "Vector_001"_ustr);
  bNodeSocket *offset_socket = bke::node_find_socket(node, SOCK_IN, "Vector_002"_ustr);
  copy_v3_fl(static_cast<bNodeSocketValueVector *>(scale_socket->default_value)->value, scale);
  copy_v3_fl(static_cast<bNodeSocketValueVector *>(offset_socket->default_value)->value, offset);
}

bNodeTree *BKE_paint_material_normal_combine_group_ensure(Main &bmain)
{
  if (bNodeTree *existing = normal_combine_group_find(bmain)) {
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, NORMAL_COMBINE_TREE_NAME, "ShaderNodeTree");
  normal_combine_tree_marker_set(*group);

  /* Interface identifiers are handed out in creation order: `Socket_0` .. `Socket_3`. */
  group->tree_interface.add_socket(
      DATA_("A"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  group->tree_interface.add_socket(
      DATA_("B"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *factor = group->tree_interface.add_socket(
      DATA_("Factor"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  auto &factor_data = *static_cast<bNodeSocketValueFloat *>(factor->socket_data);
  factor_data.subtype = PROP_FACTOR;
  factor_data.min = 0.0f;
  factor_data.max = 1.0f;
  factor_data.value = 1.0f;
  group->tree_interface.add_socket(
      DATA_("Result"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);

  bNode *group_input = bke::node_add_node(nullptr, *group, "NodeGroupInput"_ustr);
  group_input->location[0] = -800;
  bNode *group_output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  group_output->location[0] = 400;

  /* Decode both maps out of [0, 1] and into vectors. */
  bNode *decode_a = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY_ADD, float2(-600.0f, 120.0f));
  normal_combine_range_map_set(*decode_a, 2.0f, -1.0f);
  bNode *decode_b = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY_ADD, float2(-600.0f, -120.0f));
  normal_combine_range_map_set(*decode_b, 2.0f, -1.0f);

  /* Whiteout: the slopes add, the two Z multiply. */
  bNode *slopes = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_ADD, float2(-400.0f, 120.0f));
  bNode *depths = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY, float2(-400.0f, -120.0f));

  bNode *split_slopes = bke::node_add_node(nullptr, *group, "ShaderNodeSeparateXYZ"_ustr);
  split_slopes->location[0] = -220;
  split_slopes->location[1] = 120;
  bNode *split_depths = bke::node_add_node(nullptr, *group, "ShaderNodeSeparateXYZ"_ustr);
  split_depths->location[0] = -220;
  split_depths->location[1] = -120;
  bNode *rebuilt = bke::node_add_node(nullptr, *group, "ShaderNodeCombineXYZ"_ustr);
  rebuilt->location[0] = -60;

  bNode *normalize = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_NORMALIZE, float2(100.0f, 0.0f));
  bNode *encode = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY_ADD, float2(240.0f, 0.0f));
  normal_combine_range_map_set(*encode, 0.5f, 0.5f);

  /* The factor interpolates between the layer below and the combined result, so that a masked
   * layer fades out to what it covers rather than to a flat normal. */
  bNode *mix = bke::node_add_node(nullptr, *group, "ShaderNodeMix"_ustr);
  auto &mix_storage = *static_cast<NodeShaderMix *>(mix->storage);
  mix_storage.data_type = SOCK_RGBA;
  mix->location[0] = 240;
  mix->location[1] = 180;

  auto link = [&](bNode &from, const char *from_socket, bNode &to, const char *to_socket) {
    bke::node_add_link(
        *group,
        from,
        *bke::node_find_socket(from, SOCK_OUT, UString::from_ptr_noinline(from_socket)),
        to,
        *bke::node_find_socket(to, SOCK_IN, UString::from_ptr_noinline(to_socket)));
  };

  link(*group_input, "Socket_0", *decode_a, "Vector");
  link(*group_input, "Socket_1", *decode_b, "Vector");
  link(*decode_a, "Vector", *slopes, "Vector");
  link(*decode_b, "Vector", *slopes, "Vector_001");
  link(*decode_a, "Vector", *depths, "Vector");
  link(*decode_b, "Vector", *depths, "Vector_001");
  link(*slopes, "Vector", *split_slopes, "Vector");
  link(*depths, "Vector", *split_depths, "Vector");
  link(*split_slopes, "X", *rebuilt, "X");
  link(*split_slopes, "Y", *rebuilt, "Y");
  link(*split_depths, "Z", *rebuilt, "Z");
  link(*rebuilt, "Vector", *normalize, "Vector");
  link(*normalize, "Vector", *encode, "Vector");
  link(*group_input, "Socket_0", *mix, "A_Color");
  link(*encode, "Vector", *mix, "B_Color");
  link(*group_input, "Socket_2", *mix, "Factor_Float");
  link(*mix, "Result_Color", *group_output, "Socket_3");

  /* The Mix node's `data_type` and the Vector Math nodes' `operation` were set on the storage
   * after the nodes were created, so their socket declarations are still the defaults. Tag the
   * whole tree and run the update now: without this the group instantiates into a shader with
   * the wrong sockets and the node inliner asserts when EEVEE compiles a material using it. */
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/** \} */

}  // namespace blender
