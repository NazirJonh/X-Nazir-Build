/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The shared Gradient Factor group: the shader-tree half of the Gradient generator. The CPU
 * composite calls #BKE_paint_layers_gradient_factor, which builds the same arithmetic; every
 * constant and every operation below has a counterpart there.
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_gradient.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_ustring.hh"

#include "BLT_translation.hh"

#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_material_composite_internal.hh"


namespace blender {

/* -------------------------------------------------------------------- */
/** \name Gradient Factor group
 * \{ */

/** Name of the shared group data-block. The leading dot keeps it out of every ID picker. */
static const char *GRADIENT_TREE_NAME = ".PBR Gradient";

/** Marker value of the Gradient group. */
static constexpr const char *GRADIENT_MARKER_VALUE = "GRADIENT";

bool BKE_paint_material_is_gradient_group(const bNode &node)
{
  return paint_material_group_has_marker(node, GRADIENT_MARKER_VALUE);
}

namespace {

/** A node and one of its sockets. */
struct GradientSocket {
  bNode *node = nullptr;
  const char *name = nullptr;
};

class GradientGroupBuilder {
 public:
  explicit GradientGroupBuilder(bNodeTree &group) : group_(group)
  {
    group_input_ = bke::node_add_node(nullptr, group, "NodeGroupInput"_ustr);
    group_output_ = bke::node_add_node(nullptr, group, "NodeGroupOutput"_ustr);
    if (group_input_ == nullptr || group_output_ == nullptr) {
      ok_ = false;
      return;
    }
    group_input_->location[0] = -1800;
    group_input_->location[1] = 0;
  }

  bool ok() const
  {
    return ok_;
  }

  bNode *group_input() const
  {
    return group_input_;
  }

  bNode *group_output() const
  {
    return group_output_;
  }

  /** A Math node with \a operation, at the next free column. */
  bNode *math(const int operation)
  {
    bNode *node = bke::node_add_static_node(nullptr, group_, SH_NODE_MATH);
    if (node == nullptr) {
      ok_ = false;
      return nullptr;
    }
    node->custom1 = operation;
    place(node);
    return node;
  }

  /** A Vector Math node with \a operation. */
  bNode *vector_math(const int operation)
  {
    bNode *node = bke::node_add_node(nullptr, group_, "ShaderNodeVectorMath"_ustr);
    if (node == nullptr) {
      ok_ = false;
      return nullptr;
    }
    node->custom1 = operation;
    place(node);
    return node;
  }

  /** A float Mix node, `mix(A, B, factor)`. */
  bNode *mix_float()
  {
    bNode *node = bke::node_add_node(nullptr, group_, "ShaderNodeMix"_ustr);
    if (node == nullptr || node->storage == nullptr) {
      ok_ = false;
      return nullptr;
    }
    NodeShaderMix *storage = static_cast<NodeShaderMix *>(node->storage);
    storage->data_type = SOCK_FLOAT;
    storage->factor_mode = NODE_MIX_MODE_UNIFORM;
    storage->clamp_factor = true;
    storage->clamp_result = false;
    place(node);
    return node;
  }

  bool link(const GradientSocket from, const GradientSocket to)
  {
    if (from.node == nullptr || to.node == nullptr) {
      ok_ = false;
      return false;
    }
    bNodeSocket *from_socket = bke::node_find_socket(
        *from.node, SOCK_OUT, UString::from_ptr_noinline(from.name));
    bNodeSocket *to_socket = bke::node_find_socket(
        *to.node, SOCK_IN, UString::from_ptr_noinline(to.name));
    if (from_socket == nullptr || to_socket == nullptr) {
      ok_ = false;
      return false;
    }
    bke::node_add_link(group_, *from.node, *from_socket, *to.node, *to_socket);
    return true;
  }

  /** Link the group's input \a identifier into \a to's \a input. */
  bool link_input(const char *identifier, bNode *to, const char *input)
  {
    return link({group_input_, identifier}, {to, input});
  }

  /** Set a float input's default; a missing socket is a build error. */
  bool set_float(bNode *node, const char *input, const float value)
  {
    if (node == nullptr) {
      ok_ = false;
      return false;
    }
    bNodeSocket *socket = bke::node_find_socket(
        *node, SOCK_IN, UString::from_ptr_noinline(input));
    if (socket == nullptr || socket->default_value == nullptr) {
      ok_ = false;
      return false;
    }
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = value;
    return true;
  }

  /** `a op b` where both are outputs of nodes, one input each. */
  bNode *binary(const int operation,
                const GradientSocket a,
                const GradientSocket b,
                const char *a_input = "Value",
                const char *b_input = "Value_001")
  {
    bNode *node = math(operation);
    if (node == nullptr) {
      return nullptr;
    }
    link(a, {node, a_input});
    link(b, {node, b_input});
    return node;
  }

  /** `a op constant` with the constant in the second input. */
  bNode *binary_const(const int operation,
                      const GradientSocket a,
                      const float b,
                      const char *a_input = "Value",
                      const char *b_input = "Value_001")
  {
    bNode *node = math(operation);
    if (node == nullptr) {
      return nullptr;
    }
    link(a, {node, a_input});
    set_float(node, b_input, b);
    return node;
  }

  /** `constant op a` with the constant in the first input. */
  bNode *const_binary(const int operation,
                      const float a,
                      const GradientSocket b,
                      const char *a_input = "Value",
                      const char *b_input = "Value_001")
  {
    bNode *node = math(operation);
    if (node == nullptr) {
      return nullptr;
    }
    set_float(node, a_input, a);
    link(b, {node, b_input});
    return node;
  }

  /** A Compare node: one when \a value is within \a epsilon of \a target. */
  bNode *compare(const GradientSocket value, const float target, const float epsilon = 0.5f)
  {
    bNode *node = math(NODE_MATH_COMPARE);
    if (node == nullptr) {
      return nullptr;
    }
    link(value, {node, "Value"});
    set_float(node, "Value_001", target);
    set_float(node, "Value_002", epsilon);
    return node;
  }

  /** A float mix of \a a and \a b by \a factor. */
  bNode *mix(const GradientSocket a, const GradientSocket b, const GradientSocket factor)
  {
    bNode *node = mix_float();
    if (node == nullptr) {
      return nullptr;
    }
    link(a, {node, "A_Float"});
    link(b, {node, "B_Float"});
    link(factor, {node, "Factor_Float"});
    return node;
  }

  /** A Math or Vector Math socket output; Vector Math subtracts on "Vector", the rest on "Value". */
  static GradientSocket value(const bNode *node)
  {
    return {const_cast<bNode *>(node), "Value"};
  }

  static GradientSocket vector(const bNode *node)
  {
    return {const_cast<bNode *>(node), "Vector"};
  }

  /** The output of a float Mix node. */
  static GradientSocket result(const bNode *node)
  {
    return {const_cast<bNode *>(node), "Result_Float"};
  }

  /** Put the group output one column after the last node added. */
  void place_output()
  {
    this->place(group_output_);
  }

 private:
  void place(bNode *node)
  {
    node->location[0] = 400.0f + float(column_++) * 180.0f;
    node->location[1] = 0.0f;
  }

  bNodeTree &group_;
  bNode *group_input_ = nullptr;
  bNode *group_output_ = nullptr;
  int column_ = 0;
  bool ok_ = true;
};

}  // namespace

bNodeTree *BKE_paint_material_gradient_group_ensure(Main &bmain)
{
  if (bNodeTree *existing = paint_material_group_find(bmain, GRADIENT_MARKER_VALUE)) {
    if (!STREQ(existing->id.name + 2, GRADIENT_TREE_NAME)) {
      BKE_id_rename(bmain, existing->id, GRADIENT_TREE_NAME);
    }
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, GRADIENT_TREE_NAME, "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }
  paint_material_group_marker_set(*group, GRADIENT_MARKER_VALUE);

  /* The identifier stays `Socket_0` for the groups already in files; the name says what the socket
   * really carries: a 3D coordinate, with UV rows passing Z = 0. */
  bNodeTreeInterfaceSocket *uv = group->tree_interface.add_socket(
      DATA_("Coordinate"), "", "NodeSocketVector", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *start = group->tree_interface.add_socket(
      DATA_("Start"), "", "NodeSocketVector", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *end = group->tree_interface.add_socket(
      DATA_("End"), "", "NodeSocketVector", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *midpoint = group->tree_interface.add_socket(
      DATA_("Midpoint"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *type = group->tree_interface.add_socket(
      DATA_("Type"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *repeat = group->tree_interface.add_socket(
      DATA_("Repeat"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *factor = group->tree_interface.add_socket(
      DATA_("Factor"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  if (uv == nullptr || start == nullptr || end == nullptr || midpoint == nullptr ||
      type == nullptr || repeat == nullptr || factor == nullptr)
  {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  if (midpoint->socket_data != nullptr) {
    auto &data = *static_cast<bNodeSocketValueFloat *>(midpoint->socket_data);
    data.value = 0.5f;
    data.min = math::gradient_midpoint_min;
    data.max = math::gradient_midpoint_max;
  }
  if (type->socket_data != nullptr) {
    auto &data = *static_cast<bNodeSocketValueFloat *>(type->socket_data);
    data.value = 0.0f;
    data.min = 0.0f;
    data.max = 1.0f;
  }
  if (repeat->socket_data != nullptr) {
    auto &data = *static_cast<bNodeSocketValueFloat *>(repeat->socket_data);
    data.value = 0.0f;
    data.min = 0.0f;
    data.max = 2.0f;
  }

  GradientGroupBuilder builder(*group);
  if (!builder.ok()) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  /* d = End - Start; v = Coordinate - Start. The group is 3D; a UV row hands it (u, v, 0). */
  bNode *d = builder.vector_math(NODE_VECTOR_MATH_SUBTRACT);
  builder.link_input(GRADIENT_GROUP_ID_END, d, "Vector");
  builder.link_input(GRADIENT_GROUP_ID_START, d, "Vector_001");
  bNode *v = builder.vector_math(NODE_VECTOR_MATH_SUBTRACT);
  builder.link_input(GRADIENT_GROUP_ID_COORDINATE, v, "Vector");
  builder.link_input(GRADIENT_GROUP_ID_START, v, "Vector_001");

  bNode *dot_vd = builder.vector_math(NODE_VECTOR_MATH_DOT_PRODUCT);
  builder.link({v, "Vector"}, {dot_vd, "Vector"});
  builder.link({d, "Vector"}, {dot_vd, "Vector_001"});
  bNode *dot_dd = builder.vector_math(NODE_VECTOR_MATH_DOT_PRODUCT);
  builder.link({d, "Vector"}, {dot_dd, "Vector"});
  builder.link({d, "Vector"}, {dot_dd, "Vector_001"});
  bNode *len_v = builder.vector_math(NODE_VECTOR_MATH_LENGTH);
  builder.link({v, "Vector"}, {len_v, "Vector"});
  bNode *len_d = builder.vector_math(NODE_VECTOR_MATH_LENGTH);
  builder.link({d, "Vector"}, {len_d, "Vector"});

  /* Degenerate lines read as the start of the ramp, exactly as the CPU's guard does. */
  bNode *safe_sq = builder.binary_const(NODE_MATH_MAXIMUM, {dot_dd, "Value"}, 1e-12f);
  bNode *safe_len = builder.binary_const(NODE_MATH_MAXIMUM, {len_d, "Value"}, 1e-6f);
  bNode *linear = builder.binary(NODE_MATH_DIVIDE, {dot_vd, "Value"}, {safe_sq, "Value"});
  bNode *radial = builder.binary(NODE_MATH_DIVIDE, {len_v, "Value"}, {safe_len, "Value"});

  bNode *type_is_radial = builder.compare({builder.group_input(), GRADIENT_GROUP_ID_TYPE}, 1.0f);
  bNode *type_mix = builder.mix(
      builder.value(linear), builder.value(radial), builder.value(type_is_radial));
  /* A degenerate line reads as the start of the ramp on the CPU; the same 0/1 gate here. */
  bNode *line_valid = builder.binary_const(NODE_MATH_GREATER_THAN, {dot_dd, "Value"}, 1e-12f);
  const GradientSocket t_type = builder.value(
      builder.binary(NODE_MATH_MULTIPLY, builder.result(type_mix), {line_valid, "Value"}));

  /* Repeat: None clamps, Repeat is `frac`, Reflect is `pingpong(t, 1)`. */
  bNode *clamped_lo = builder.binary_const(NODE_MATH_MAXIMUM, t_type, 0.0f);
  bNode *clamped = builder.binary_const(NODE_MATH_MINIMUM, {clamped_lo, "Value"}, 1.0f);
  bNode *fract = builder.math(NODE_MATH_FRACTION);
  builder.link(t_type, {fract, "Value"});
  bNode *pingpong = builder.binary_const(NODE_MATH_PINGPONG, t_type, 1.0f);
  bNode *is_repeat = builder.compare({builder.group_input(), GRADIENT_GROUP_ID_REPEAT}, 1.0f);
  bNode *repeat_mix = builder.mix(builder.value(clamped),
                                  builder.value(fract),
                                  builder.value(is_repeat));
  bNode *is_reflect = builder.compare({builder.group_input(), GRADIENT_GROUP_ID_REPEAT}, 2.0f);
  bNode *repeat_final = builder.mix(
      builder.result(repeat_mix), builder.value(pingpong), builder.value(is_reflect));
  const GradientSocket t_rep = builder.result(repeat_final);

  /* Midpoint: the two cubic Hermite segments of the CPU's remap. */
  bNode *mid_lo = builder.binary_const(NODE_MATH_MAXIMUM,
                                       {builder.group_input(), GRADIENT_GROUP_ID_MIDPOINT},
                                       math::gradient_midpoint_min);
  bNode *mid = builder.binary_const(
      NODE_MATH_MINIMUM, {mid_lo, "Value"}, math::gradient_midpoint_max);

  /* First segment: t <= mid, u = t / mid. */
  bNode *u1 = builder.binary(NODE_MATH_DIVIDE, t_rep, {mid, "Value"});
  bNode *u12 = builder.binary(NODE_MATH_MULTIPLY, {u1, "Value"}, {u1, "Value"});
  bNode *u13 = builder.binary(NODE_MATH_MULTIPLY, {u12, "Value"}, {u1, "Value"});
  bNode *t1a = builder.binary_const(NODE_MATH_MULTIPLY, {u13, "Value"}, 0.5f);
  bNode *t1b = builder.binary_const(NODE_MATH_MULTIPLY, {u12, "Value"}, 1.0f);
  bNode *t1c = builder.binary_const(NODE_MATH_MULTIPLY, {u1, "Value"}, 0.5f);
  bNode *t1_sub = builder.binary(NODE_MATH_SUBTRACT, {t1a, "Value"}, {t1b, "Value"});
  bNode *t1_sum = builder.binary(NODE_MATH_ADD, {t1_sub, "Value"}, {t1c, "Value"});
  bNode *t1d = builder.binary_const(NODE_MATH_MULTIPLY, {u12, "Value"}, 1.5f);
  bNode *t1e = builder.binary_const(NODE_MATH_MULTIPLY, {u13, "Value"}, 1.0f);
  bNode *t1_hi = builder.binary(NODE_MATH_SUBTRACT, {t1d, "Value"}, {t1e, "Value"});
  bNode *t1_join = builder.binary(NODE_MATH_SUBTRACT, {u13, "Value"}, {u12, "Value"});
  bNode *t1_mid = builder.binary(NODE_MATH_MULTIPLY, {t1_join, "Value"}, {mid, "Value"});
  bNode *first_a = builder.binary(NODE_MATH_ADD, {t1_sum, "Value"}, {t1_hi, "Value"});
  bNode *first = builder.binary(NODE_MATH_ADD, {first_a, "Value"}, {t1_mid, "Value"});

  /* Second segment: t > mid, u = (t - mid) / (1 - mid). */
  bNode *im = builder.const_binary(NODE_MATH_SUBTRACT, 1.0f, {mid, "Value"});
  bNode *t_shift = builder.binary(NODE_MATH_SUBTRACT, t_rep, {mid, "Value"});
  bNode *u2 = builder.binary(NODE_MATH_DIVIDE, {t_shift, "Value"}, {im, "Value"});
  bNode *u22 = builder.binary(NODE_MATH_MULTIPLY, {u2, "Value"}, {u2, "Value"});
  bNode *u23 = builder.binary(NODE_MATH_MULTIPLY, {u22, "Value"}, {u2, "Value"});
  bNode *s2a = builder.binary_const(NODE_MATH_MULTIPLY, {u23, "Value"}, 1.0f);
  bNode *s2b = builder.binary_const(NODE_MATH_MULTIPLY, {u22, "Value"}, 1.5f);
  bNode *s2c = builder.binary(NODE_MATH_SUBTRACT, {s2a, "Value"}, {s2b, "Value"});
  bNode *s2d = builder.binary_const(NODE_MATH_ADD, {s2c, "Value"}, 0.5f);
  bNode *q2 = builder.binary_const(NODE_MATH_MULTIPLY, {u23, "Value"}, 1.0f);
  bNode *q3 = builder.binary_const(NODE_MATH_MULTIPLY, {u22, "Value"}, 2.0f);
  bNode *q4 = builder.binary(NODE_MATH_SUBTRACT, {q2, "Value"}, {q3, "Value"});
  bNode *q5 = builder.binary(NODE_MATH_ADD, {q4, "Value"}, {u2, "Value"});
  bNode *s2e = builder.binary(NODE_MATH_MULTIPLY, {q5, "Value"}, {im, "Value"});
  bNode *r1 = builder.binary_const(NODE_MATH_MULTIPLY, {u22, "Value"}, 3.0f);
  bNode *r2 = builder.binary_const(NODE_MATH_MULTIPLY, {u23, "Value"}, 2.0f);
  bNode *s2f = builder.binary(NODE_MATH_SUBTRACT, {r1, "Value"}, {r2, "Value"});
  bNode *p1 = builder.binary(NODE_MATH_SUBTRACT, {u23, "Value"}, {u22, "Value"});
  bNode *s2g = builder.binary_const(NODE_MATH_MULTIPLY, {p1, "Value"}, 0.5f);
  bNode *second_a = builder.binary(NODE_MATH_ADD, {s2d, "Value"}, {s2e, "Value"});
  bNode *second_b = builder.binary(NODE_MATH_ADD, {second_a, "Value"}, {s2f, "Value"});
  bNode *second = builder.binary(NODE_MATH_ADD, {second_b, "Value"}, {s2g, "Value"});

  bNode *before_mid = builder.math(NODE_MATH_LESS_THAN);
  builder.link(t_rep, {before_mid, "Value"});
  builder.link({mid, "Value"}, {before_mid, "Value_001"});
  /* A Mix node returns B at factor one, so the first segment is B: it applies before the midpoint. */
  bNode *factor_node = builder.mix(
      builder.value(second), builder.value(first), builder.value(before_mid));

  if (!builder.ok() || factor_node == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  if (!builder.link({factor_node, "Result_Float"},
                    {builder.group_output(), GRADIENT_GROUP_ID_FACTOR}))
  {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  builder.place_output();

  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/** \} */

}  // namespace blender
