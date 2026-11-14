/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Shared between #BKE_paint_material_blend_group_ensure (the group's interface, its Mix-node
 * branches and its Mode dispatcher, in `paint_material_blend_group.cc`) and the extended-mode
 * branch builders (`paint_material_blend_group_extended.cc`). Private to those two files.
 */

#include "BLI_function_ref.hh"

struct bNode;
struct bNodeTree;

namespace blender {

/** Socket identifiers of the stock node types the branches are built from. A typo here would not
 * break the build: every link and default-set reports a missing socket at run time. Naming them
 * once keeps the identifiers reviewable next to the nodes that use them. These are the identifiers
 * Blender's own node declarations give these sockets. */
constexpr const char *BLEND_NODE_MIX_FACTOR = "Factor_Float";
constexpr const char *BLEND_NODE_MIX_FACTOR_VECTOR = "Factor_Vector";
constexpr const char *BLEND_NODE_MIX_A = "A_Color";
constexpr const char *BLEND_NODE_MIX_B = "B_Color";
constexpr const char *BLEND_NODE_MIX_RESULT = "Result_Color";
constexpr const char *BLEND_NODE_MIX_VECTOR_A = "A_Vector";
constexpr const char *BLEND_NODE_MIX_VECTOR_B = "B_Vector";
constexpr const char *BLEND_NODE_MIX_VECTOR_RESULT = "Result_Vector";
constexpr const char *BLEND_NODE_VECTOR_MATH_A = "Vector";
constexpr const char *BLEND_NODE_VECTOR_MATH_B = "Vector_001";
constexpr const char *BLEND_NODE_VECTOR_MATH_SCALE = "Scale";
constexpr const char *BLEND_NODE_VECTOR_MATH_VALUE = "Value";
/** The vector output of a Vector Math node; its "Value" output is the scalar one, which only the
 * dot product, the length and the distance fill in. */
constexpr const char *BLEND_NODE_VECTOR_MATH_VECTOR = "Vector";
constexpr const char *BLEND_NODE_MATH_A = "Value";
constexpr const char *BLEND_NODE_MATH_B = "Value_001";
constexpr const char *BLEND_NODE_MATH_C = "Value_002";
constexpr const char *BLEND_NODE_MATH_VALUE = "Value";
constexpr const char *BLEND_NODE_SEPARATE_COLOR = "Color";
constexpr const char *BLEND_NODE_SEPARATE_RED = "Red";
constexpr const char *BLEND_NODE_SEPARATE_GREEN = "Green";
constexpr const char *BLEND_NODE_SEPARATE_BLUE = "Blue";
constexpr const char *BLEND_NODE_COMBINE_X = "X";
constexpr const char *BLEND_NODE_COMBINE_Y = "Y";
constexpr const char *BLEND_NODE_COMBINE_Z = "Z";
constexpr const char *BLEND_NODE_COMBINE_VALUE = "Vector";
constexpr const char *BLEND_NODE_LEGACY_MIX_FAC = "Fac";
constexpr const char *BLEND_NODE_LEGACY_MIX_COLOR1 = "Color1";
constexpr const char *BLEND_NODE_LEGACY_MIX_COLOR2 = "Color2";
constexpr const char *BLEND_NODE_LEGACY_MIX_COLOR = "Color";

/** One branch's answer: the Mode code it answers to, the node the dispatcher selects, and the
 * colour output to select from it. */
struct BlendBranch {
  int code;
  bNode *node = nullptr;
  const char *output = nullptr;
};

/** Links \a from's output \a from_socket to \a to's input \a to_socket; false on a missing socket. */
using BlendLinkFn = FunctionRef<bool(bNode *from,
                                     const char *from_socket,
                                     bNode *to,
                                     const char *to_socket)>;

/** A `ShaderNodeMix` in RGBA mode, blending with \a blend_type: the form every branch is built
 * from, and the settings #mix_node_add in `paint_layers_generate_build_chain.cc` uses. */
bNode *blend_group_mix_node_rgba_add(bNodeTree &group,
                                     int blend_type,
                                     float location_x,
                                     float location_y);

/** The constant on \a node's float input \a identifier: a branch's threshold or its full factor. */
bool blend_group_socket_float_default_set(bNode &node, const char *identifier, float value);

/** The constant vector on \a node's input \a identifier. */
bool blend_group_socket_vector_default_set(bNode &node,
                                           const char *identifier,
                                           float x,
                                           float y,
                                           float z);

/**
 * A branch for one of the modes a single Mix node cannot express (#eMaterialPaintBlend).
 *
 * Every one of them is the same shape: a cluster computing the mode at full strength, then a Mix
 * of what is below with that answer by the group's own Factor. That is also how the CPU reads them
 * (`BKE_paint_material_blend_apply` mixes the formula's result in), so the two agree texel for
 * texel -- and the alpha model stays the chain's, since none of these modes is a Mix
 * (#ROLE_BLEND_IS_MIX reads false). An empty result means the cluster could not be built; the
 * caller drops the whole group, so no orphan of a half-built cluster is left behind.
 */
BlendBranch blend_branch_extended_add(bNodeTree &group,
                                      bNode &group_input,
                                      const BlendLinkFn &link,
                                      int mode,
                                      float location_x,
                                      float location_y);

}  // namespace blender
