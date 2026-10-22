/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLT_translation.hh"

#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_material_composite_internal.hh"

#include <cstring>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Blend Group
 * \{ */

/** Name of the shared Blend group data-block. The leading dot keeps it out of every ID picker
 * and #Node Groups list (see #interface_template_id). NOTE: do not translate. */
static const char *BLEND_TREE_NAME = ".PBR Blend";

/**
 * The IDProperty marking the trees this module recognizes as one of its own groups, told apart by
 * value. The key is shared with the Normal Combine group; only the value says which group this is.
 */
static constexpr const char *BLEND_NODE_GROUP_MARKER_PROP = "pbr_paint_node_group";
/** #BLEND_NODE_GROUP_MARKER_PROP value of the Blend group. */
static constexpr const char *BLEND_MARKER_VALUE = "BLEND";

/** The #BLEND_NODE_GROUP_MARKER_PROP value on \a tree_id, or null when unmarked. */
static const char *blend_group_marker_get(const ID &tree_id)
{
  const IDProperty *properties = IDP_GetProperties(const_cast<ID *>(&tree_id));
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
      properties, BLEND_NODE_GROUP_MARKER_PROP, IDP_STRING);
  return (marker != nullptr) ? IDP_string_get(marker) : nullptr;
}

/** Stamp the shared Blend group tree. */
static void blend_group_marker_set(bNodeTree &group)
{
  IDProperty *properties = IDP_EnsureProperties(&group.id);
  IDPropertyTemplate value = {0};
  value.string.str = const_cast<char *>(BLEND_MARKER_VALUE);
  value.string.len = int(strlen(BLEND_MARKER_VALUE)) + 1;
  value.string.subtype = IDP_STRING_SUB_UTF8;
  IDP_AddToGroup(properties, IDP_New(IDP_STRING, &value, BLEND_NODE_GROUP_MARKER_PROP));
}

bool BKE_paint_material_is_blend_group(const bNode &node)
{
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    return false;
  }
  const char *marker = blend_group_marker_get(*node.id);
  return marker != nullptr && STREQ(marker, BLEND_MARKER_VALUE);
}

/** The group already in \a bmain, or null. Found by its marker, so a rename does not lose it. */
static bNodeTree *blend_group_find(Main &bmain)
{
  for (bNodeTree &ntree : bmain.nodetrees) {
    const char *marker = blend_group_marker_get(ntree.id);
    if (marker != nullptr && STREQ(marker, BLEND_MARKER_VALUE)) {
      return &ntree;
    }
  }
  return nullptr;
}

/**
 * One static blend branch: a `ShaderNodeMix` in RGBA mode set to \a ramp_blend.
 *
 * The settings are the ones #mix_node_add in `paint_layers_generate_build_chain.cc` uses, and the
 * same the CPU's `ramp_blend` assumes: the factor is clamped to 0..1, the result is not, so Add
 * can exceed one and only the byte encode finally clamps.
 */
static bNode *blend_branch_add(bNodeTree &group,
                               const int ramp_blend,
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
  storage->blend_type = ramp_blend;
  storage->clamp_factor = true;
  storage->clamp_result = false;
  node->location[0] = location_x;
  node->location[1] = location_y;
  return node;
}

bNodeTree *BKE_paint_material_blend_group_ensure(Main &bmain)
{
  if (bNodeTree *existing = blend_group_find(bmain)) {
    /* A file saved before the dot scheme carries the bare name; rename it in place. Found by
     * marker, so the name never decides which tree this is. */
    if (!STREQ(existing->id.name + 2, BLEND_TREE_NAME)) {
      BKE_id_rename(bmain, existing->id, BLEND_TREE_NAME);
    }
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, BLEND_TREE_NAME, "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }
  blend_group_marker_set(*group);

  /* Interface identifiers are handed out in creation order: `Socket_0` .. `Socket_4`, see the
   * `BLEND_GROUP_ID_*` constants in `paint_material_composite_internal.hh`. */
  bNodeTreeInterfaceSocket *factor = group->tree_interface.add_socket(
      DATA_("Factor"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *a = group->tree_interface.add_socket(
      DATA_("A"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *b = group->tree_interface.add_socket(
      DATA_("B"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *mode = group->tree_interface.add_socket(
      DATA_("Mode"), "", "NodeSocketInt", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *result = group->tree_interface.add_socket(
      DATA_("Result"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  if (factor == nullptr || factor->socket_data == nullptr || a == nullptr || b == nullptr ||
      mode == nullptr || mode->socket_data == nullptr || result == nullptr)
  {
    /* The interface is what every reader of the group asks for by these fixed identifiers. A
     * failure here is a build error; leaving a half-built group behind would make the generator
     * fail later, far from the cause. */
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  auto &factor_data = *static_cast<bNodeSocketValueFloat *>(factor->socket_data);
  factor_data.subtype = PROP_FACTOR;
  factor_data.min = 0.0f;
  factor_data.max = 1.0f;
  factor_data.value = 1.0f;
  auto &mode_data = *static_cast<bNodeSocketValueInt *>(mode->socket_data);
  mode_data.value = 0;
  mode_data.min = MA_RAMP_BLEND;
  mode_data.max = MA_RAMP_EXCLUSION;

  bNode *group_input = bke::node_add_node(nullptr, *group, "NodeGroupInput"_ustr);
  bNode *group_output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  if (group_input == nullptr || group_output == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  group_input->location[0] = -1600;
  group_input->location[1] = 0;
  group_output->location[0] = 5200;
  group_output->location[1] = 0;

  bool ok = true;
  auto link = [&](bNode *from, const char *from_socket, bNode *to, const char *to_socket) -> bool {
    if (from == nullptr || to == nullptr) {
      return false;
    }
    bNodeSocket *from_sock = bke::node_find_socket(
        *from, SOCK_OUT, UString::from_ptr_noinline(from_socket));
    bNodeSocket *to_sock = bke::node_find_socket(*to, SOCK_IN, UString::from_ptr_noinline(to_socket));
    if (from_sock == nullptr || to_sock == nullptr) {
      return false;
    }
    bke::node_add_link(*group, *from, *from_sock, *to, *to_sock);
    return true;
  };

  /* Every mode is built once, side by side, and all of them read the same two colours and factor.
   * The branches are static: a mode change only moves the comparison below, it never rebuilds a
   * node, which is what keeps the group shared and the shader cache stable. */
  constexpr int branch_count = MA_RAMP_EXCLUSION - MA_RAMP_BLEND + 1;
  bNode *branches[branch_count] = {};
  for (int ramp_blend = MA_RAMP_BLEND; ramp_blend <= MA_RAMP_EXCLUSION; ramp_blend++) {
    const int index = ramp_blend - MA_RAMP_BLEND;
    bNode *branch = blend_branch_add(*group, ramp_blend, -1200.0f, 1080.0f - index * 120.0f);
    if (branch == nullptr) {
      ok = false;
      break;
    }
    branches[index] = branch;
    ok = link(group_input, BLEND_GROUP_ID_A, branch, "A_Color") && ok;
    ok = link(group_input, BLEND_GROUP_ID_B, branch, "B_Color") && ok;
    ok = link(group_input, BLEND_GROUP_ID_FACTOR, branch, "Factor_Float") && ok;
  }

  /* Pick the branch the Mode input names. A Math COMPARE against each code makes a 0/1 factor, and
   * a Mix (Legacy) cross-fades between the branch chosen so far and the next one.
   *
   * Why the legacy MixRGB rather than another ShaderNodeMix: EEVEE's
   * `ntree_shader_disconnect_inactive_mix_branches()` in `nodes/shader/node_shader_tree.cc`
   * disconnects the inactive branch of an #SH_NODE_MIX whose factor is a constant zero. That turns
   * each group instance into a different graph shape, so EEVEE has to specialize and recompile the
   * shader for every blend mode. #SH_NODE_MIX_RGB_LEGACY is not touched by that pass, so every mode
   * shares one compiled shader: measured as zero recompiles when only Mode changes, with the GLSL
   * identical to the equivalent linked-factor Mix chain.
   *
   * If upstream ever removes the legacy node, this dispatcher is the only place that has to change:
   * the fallback is a chain of #SH_NODE_MIX nodes whose factors stay linked (Combine Color + RGB to
   * BW), never a constant, which keeps the disconnect pass from firing. */
  bNode *accumulator = branches[0];
  /* The branch and the selector are different node types, so their output socket has a different
   * identifier. Kept next to the node instead of guessing it from the type each round. */
  const char *accumulator_output = "Result_Color";
  float select_x = -900.0f;
  constexpr float select_step_x = 360.0f;
  for (int ramp_blend = MA_RAMP_BLEND + 1; ok && ramp_blend <= MA_RAMP_EXCLUSION; ramp_blend++) {
    bNode *compare = bke::node_add_node(nullptr, *group, "ShaderNodeMath"_ustr);
    if (compare == nullptr) {
      ok = false;
      break;
    }
    compare->custom1 = NODE_MATH_COMPARE;
    compare->custom2 = 0;
    compare->location[0] = select_x;
    compare->location[1] = 120;

    bNodeSocket *compare_code = bke::node_find_socket(*compare, SOCK_IN, "Value_001"_ustr);
    bNodeSocket *compare_epsilon = bke::node_find_socket(*compare, SOCK_IN, "Value_002"_ustr);
    if (compare_code == nullptr || compare_code->default_value == nullptr ||
        compare_epsilon == nullptr || compare_epsilon->default_value == nullptr)
    {
      ok = false;
      break;
    }
    static_cast<bNodeSocketValueFloat *>(compare_code->default_value)->value = float(ramp_blend);
    static_cast<bNodeSocketValueFloat *>(compare_epsilon->default_value)->value = 0.5f;
    /* Mode is an integer socket; the shader tree converts it implicitly to the float the Math node
     * reads, the same conversion the UI offers when the two are linked by hand. */
    ok = link(group_input, BLEND_GROUP_ID_MODE, compare, "Value") && ok;

    bNode *selector = bke::node_add_node(nullptr, *group, "ShaderNodeMixRGB"_ustr);
    if (selector == nullptr) {
      ok = false;
      break;
    }
    /* The legacy node's own blend is a plain Mix: its Color output is `lerp(Color1, Color2, Fac)`,
     * and the branch's operation is already baked into Color2. The result is left unclamped. */
    selector->custom1 = MA_RAMP_BLEND;
    selector->custom2 = 0;
    selector->location[0] = select_x + 160.0f;
    selector->location[1] = 0;

    ok = link(compare, "Value", selector, "Fac") && ok;
    ok = link(accumulator, accumulator_output, selector, "Color1") && ok;
    ok = link(branches[ramp_blend - MA_RAMP_BLEND], "Result_Color", selector, "Color2") && ok;
    accumulator = selector;
    accumulator_output = "Color";
    select_x += select_step_x;
  }

  ok = ok && link(accumulator, accumulator_output, group_output, BLEND_GROUP_ID_RESULT);
  if (!ok) {
    /* An unregistered node type or a changed socket declaration must fail here, where the cause is
     * visible, rather than leaving a group every reader would misread. */
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  /* The Mix nodes' `data_type` and the Math nodes' `operation` were set on the storage after the
   * nodes were created, so their socket declarations are still the defaults. Tag the whole tree and
   * run the update now: without this the group instantiates into a shader with the wrong sockets
   * and the node inliner asserts when EEVEE compiles a material using it. */
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/** \} */

}  // namespace blender

