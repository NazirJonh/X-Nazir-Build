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
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLT_translation.hh"

#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_material_blend_group_internal.hh"
#include "paint_layers/paint_material_generator_group_ids.hh"
#include "paint_material_composite_internal.hh"

#include <cstring>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Blend Group
 * \{ */

/** Name of the shared Blend group data-block. The leading dot keeps it out of every ID picker
 * and #Node Groups list (see #interface_template_id). NOTE: do not translate. */
static const char *BLEND_TREE_NAME = ".PBR Blend";

/** Marker value of the Blend group; see #paint_material_group_has_marker. */
static constexpr const char *BLEND_MARKER_VALUE = "BLEND";

/** How many branches the group builds: one per Mix-node blend code, plus one cluster per
 * #eMaterialPaintBlend mode. A group with fewer selects a neighbouring mode silently, so this is
 * what the build checks against. */
constexpr int BLEND_BRANCH_EXPECTED = (MA_RAMP_EXCLUSION - MA_RAMP_BLEND + 1) + 6;

bool BKE_paint_material_is_blend_group(const bNode &node)
{
  return paint_material_group_has_marker(node, BLEND_MARKER_VALUE);
}

bNodeTree *BKE_paint_material_blend_group_ensure(Main &bmain)
{
  if (bNodeTree *existing = paint_material_group_find(bmain, BLEND_MARKER_VALUE)) {
    /* A file saved before the dot scheme carries the bare name; rename it in place. Found by
     * marker, so the name never decides which tree this is. */
    if (!STREQ(existing->id.name + 2, BLEND_TREE_NAME)) {
      BKE_id_rename(bmain, existing->id, BLEND_TREE_NAME);
    }
    /* A group loaded from a file built before the flag existed: set it on its branches so hiding a
     * scrubbed row stays a uniform change. */
    for (bNode &node : existing->nodes) {
      if (node.type_legacy == SH_NODE_MIX) {
        node.flag |= NODE_KEEP_INACTIVE_BRANCH;
      }
    }
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, BLEND_TREE_NAME, "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }
  paint_material_group_marker_set(*group, BLEND_MARKER_VALUE);

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
  /* Past the Mix node's own range: the codes of #eMaterialPaintBlend select the branches built out
   * of several nodes. The gap in between is the normal combine, which is a group of its own. */
  mode_data.max = MA_PAINT_BLEND_DARKER_COLOR;

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
  Vector<BlendBranch> branch_list;
  for (int ramp_blend = MA_RAMP_BLEND; ramp_blend <= MA_RAMP_EXCLUSION; ramp_blend++) {
    const int index = ramp_blend - MA_RAMP_BLEND;
    bNode *branch = blend_group_mix_node_rgba_add(
        *group, ramp_blend, -1200.0f, 1080.0f - index * 120.0f);
    if (branch == nullptr) {
      ok = false;
      break;
    }
    ok = link(group_input, BLEND_GROUP_ID_A, branch, BLEND_NODE_MIX_A) && ok;
    ok = link(group_input, BLEND_GROUP_ID_B, branch, BLEND_NODE_MIX_B) && ok;
    ok = link(group_input, BLEND_GROUP_ID_FACTOR, branch, BLEND_NODE_MIX_FACTOR) && ok;
    branch_list.append(BlendBranch{ramp_blend, branch, BLEND_NODE_MIX_RESULT});
  }

  /* The modes a Mix node has no blend type for, one cluster each, below the ramp range and past the
   * code the normal combine keeps for itself. */
  struct ExtendedBranch {
    int mode;
    int index;
  };
  static constexpr ExtendedBranch extended_branches[] = {
      {MA_PAINT_BLEND_HARD_LIGHT, 0},
      {MA_PAINT_BLEND_VIVID_LIGHT, 1},
      {MA_PAINT_BLEND_PIN_LIGHT, 2},
      {MA_PAINT_BLEND_LINEAR_BURN, 3},
      {MA_PAINT_BLEND_LIGHTER_COLOR, 4},
      {MA_PAINT_BLEND_DARKER_COLOR, 5},
  };
  for (const ExtendedBranch &entry : extended_branches) {
    if (!ok) {
      break;
    }
    BlendBranch branch = blend_branch_extended_add(
        *group,
        *group_input,
        FunctionRef<bool(bNode *, const char *, bNode *, const char *)>(link),
        entry.mode,
        -1200.0f,
        -1600.0f - entry.index * 700.0f);
    if (branch.node == nullptr) {
      ok = false;
      break;
    }
    branch.code = entry.mode;
    branch_list.append(branch);
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
  if (ok && branch_list.size() != BLEND_BRANCH_EXPECTED) {
    /* Every mode of both ranges is built above; a different count means the build stopped early,
     * and a dispatcher missing a mode would silently select the one below it. */
    BLI_assert_msg(false, "The .PBR Blend group built fewer branches than it dispatches");
    ok = false;
  }
  if (!ok) {
    /* A cluster that returned early left its already-added nodes in the tree: the group is dropped
     * whole here, so a half-built group is never handed to a reader or kept in the file. */
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  /* The branch and the selector are different node types, so their output socket has a different
   * identifier. Kept next to the node instead of guessing it from the type each round. */
  float select_x = -900.0f;
  constexpr float select_step_x = 360.0f;
  bNode *accumulator = branch_list[0].node;
  const char *accumulator_output = branch_list[0].output;
  for (const int64_t i : IndexRange::from_begin_end(1, branch_list.size())) {
    if (!ok) {
      break;
    }
    const BlendBranch &branch = branch_list[i];
    const int code = branch_list[i].code;
    bNode *compare = bke::node_add_node(nullptr, *group, "ShaderNodeMath"_ustr);
    if (compare == nullptr) {
      ok = false;
      break;
    }
    compare->custom1 = NODE_MATH_COMPARE;
    compare->custom2 = 0;
    compare->location[0] = select_x;
    compare->location[1] = 120;

    bNodeSocket *compare_code = bke::node_find_socket(
        *compare, SOCK_IN, UString::from_ptr_noinline(BLEND_NODE_MATH_B));
    bNodeSocket *compare_epsilon = bke::node_find_socket(
        *compare, SOCK_IN, UString::from_ptr_noinline(BLEND_NODE_MATH_C));
    if (compare_code == nullptr || compare_code->default_value == nullptr ||
        compare_epsilon == nullptr || compare_epsilon->default_value == nullptr)
    {
      ok = false;
      break;
    }
    static_cast<bNodeSocketValueFloat *>(compare_code->default_value)->value = float(code);
    static_cast<bNodeSocketValueFloat *>(compare_epsilon->default_value)->value = 0.5f;
    /* Mode is an integer socket; the shader tree converts it implicitly to the float the Math node
     * reads, the same conversion the UI offers when the two are linked by hand. */
    ok = link(group_input, BLEND_GROUP_ID_MODE, compare, BLEND_NODE_MATH_A) && ok;

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

    ok = link(compare, BLEND_NODE_MATH_VALUE, selector, BLEND_NODE_LEGACY_MIX_FAC) && ok;
    ok = link(accumulator, accumulator_output, selector, BLEND_NODE_LEGACY_MIX_COLOR1) && ok;
    ok = link(branch.node, branch.output, selector, BLEND_NODE_LEGACY_MIX_COLOR2) && ok;
    accumulator = selector;
    accumulator_output = BLEND_NODE_LEGACY_MIX_COLOR;
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
  /* The factor is the row's opacity and may legitimately be 0 while the row is hidden. Pruning the
   * inactive branch at that value rewrites the shader, so every Mix keeps both branches. */
  for (bNode &node : group->nodes) {
    if (node.type_legacy == SH_NODE_MIX) {
      node.flag |= NODE_KEEP_INACTIVE_BRANCH;
    }
  }

  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/** \} */

}  // namespace blender

