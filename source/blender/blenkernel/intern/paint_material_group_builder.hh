/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The node arithmetic the shared Paint Layers groups are written with. The Gradient's ramp position
 * and the compound generators' patterns are all of a kind: a column of Math and Vector Math nodes, a
 * few Mix nodes to switch between them, and one float the group's caller reads. Each group used to
 * own its own builder for that, which meant a second place to fix whenever the shape of a chain
 * changed; this is the one place.
 *
 * Every method here has a counterpart in the CPU composite that mirrors its group, statement for
 * statement. A helper that does not match a Math node's own evaluation is a formula the render and
 * the bake can disagree about, so the operations are the node's, in the order the node reads them.
 */

#pragma once

#include <algorithm>
#include <array>

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "BLI_index_range.hh"
#include "BLI_span.hh"
#include "BLI_string_utf8.h"
#include "BLI_ustring.hh"

#include "BLT_translation.hh"

#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "NOD_socket.hh"

namespace blender {

struct bNodeTree;

/** A node of the group under construction and one of its sockets, named by identifier. */
struct GroupSocket {
  bNode *node = nullptr;
  const char *socket = nullptr;
};

/** One float input of a shared group: its name in the editor, its default value and its range. */
struct GroupFloatInput {
  const char *name;
  float value;
  float min;
  float max;
};

/**
 * `smoothstep(lo, hi, x)` as #PaintMaterialGroupBuilder::smooth_step builds it out of Math nodes:
 * the guarded divide, the clamp, the Hermite curve, in that order. The CPU mirrors of the compound
 * groups call this so that the two spellings of the threshold stay one formula.
 */
inline float paint_material_group_smooth_step(const float x, const float lo, const float hi)
{
  const float range = hi - lo;
  const float safe = std::max(range, 1e-6f);
  float u = (x - lo) / safe;
  u = std::min(std::max(u, 0.0f), 1.0f);
  const float squared = u * u;
  const float twice = u * 2.0f;
  const float curve = 3.0f - twice;
  return squared * curve;
}

/**
 * The interface of a generator group: a vector Coordinate, one float per \a floats, then the float
 * Factor output. The `Socket_N` identifiers follow from this order, which is the contract the row
 * builder and the sync read from #paint_material_composite_internal.hh -- a socket inserted in the
 * middle would renumber every socket after it, in every file that holds the group.
 */
inline bool paint_material_group_interface_add(bNodeTree &group,
                                               const char *coordinate_name,
                                               Span<const GroupFloatInput> floats,
                                               const char *factor_name)
{
  if (group.tree_interface.add_socket(DATA_(coordinate_name),
                                      "",
                                      "NodeSocketVector",
                                      NODE_INTERFACE_SOCKET_INPUT,
                                      nullptr) == nullptr)
  {
    return false;
  }
  for (const GroupFloatInput &input : floats) {
    bNodeTreeInterfaceSocket *socket = group.tree_interface.add_socket(
        DATA_(input.name), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
    if (socket == nullptr) {
      return false;
    }
    if (socket->socket_data != nullptr) {
      auto &data = *static_cast<bNodeSocketValueFloat *>(socket->socket_data);
      data.value = input.value;
      data.min = input.min;
      data.max = input.max;
    }
  }
  return group.tree_interface.add_socket(DATA_(factor_name),
                                         "",
                                         "NodeSocketFloat",
                                         NODE_INTERFACE_SOCKET_OUTPUT,
                                         nullptr) != nullptr;
}

/**
 * Builds one shared generator group: a group input and output, then the chain between them. Every
 * adder places its node in the next column, so a finished group reads left to right; a failure sets
 * #ok_ false and the caller throws the whole tree away.
 */
class PaintMaterialGroupBuilder {
 public:
  explicit PaintMaterialGroupBuilder(bNodeTree &group) : group_(group)
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

  /** A node of \a idname with its default storage, at the next free column. */
  bNode *add_node(const char *idname)
  {
    bNode *node = bke::node_add_node(nullptr, group_, UString::from_ptr_noinline(idname));
    if (node == nullptr) {
      ok_ = false;
      return nullptr;
    }
    place(node);
    return node;
  }

  /** A static node of legacy \a type, at the next free column. */
  bNode *add_static_node(const int type)
  {
    bNode *node = bke::node_add_static_node(nullptr, group_, type);
    if (node == nullptr) {
      ok_ = false;
      return nullptr;
    }
    place(node);
    return node;
  }

  /** A Math node with \a operation, at the next free column. */
  bNode *math(const int operation)
  {
    bNode *node = add_static_node(SH_NODE_MATH);
    if (node == nullptr) {
      return nullptr;
    }
    node->custom1 = operation;
    return node;
  }

  /** A Vector Math node with \a operation. */
  bNode *vector_math(const int operation)
  {
    bNode *node = add_node("ShaderNodeVectorMath");
    if (node == nullptr) {
      return nullptr;
    }
    node->custom1 = operation;
    return node;
  }

  /** A float Mix node, `mix(A, B, factor)`. */
  bNode *mix_float()
  {
    bNode *node = add_node("ShaderNodeMix");
    if (node == nullptr || node->storage == nullptr) {
      ok_ = false;
      return nullptr;
    }
    NodeShaderMix *storage = static_cast<NodeShaderMix *>(node->storage);
    storage->data_type = SOCK_FLOAT;
    storage->factor_mode = NODE_MIX_MODE_UNIFORM;
    storage->clamp_factor = true;
    storage->clamp_result = false;
    return node;
  }

  /**
   * A Separate XYZ over \a vector; \a r_xyz receives its three components. The 2D coordinate a
   * generator reads has Z = 0, which a pattern that only uses X and Y never looks at.
   */
  bool separate_xyz(const GroupSocket vector, GroupSocket r_xyz[3])
  {
    return this->separate(SH_NODE_SEPXYZ, "Vector", {"X", "Y", "Z"}, vector, r_xyz);
  }

  /**
   * A Separate Color over \a color; \a r_rgb receives its three channels. A Noise node's Color
   * output is three independent noises of the same coordinate, which is what a weave's jitter is
   * made of.
   */
  bool separate_color(const GroupSocket color, GroupSocket r_rgb[3])
  {
    return this->separate(SH_NODE_SEPARATE_COLOR, "Color", {"Red", "Green", "Blue"}, color, r_rgb);
  }

  /** A Combine XYZ of \a x, \a y and \a z. */
  GroupSocket combine_xyz(const GroupSocket x, const GroupSocket y, const GroupSocket z)
  {
    bNode *node = add_static_node(SH_NODE_COMBXYZ);
    if (node == nullptr) {
      return {};
    }
    if (socket(node, SOCK_IN, "X") == nullptr || socket(node, SOCK_IN, "Y") == nullptr ||
        socket(node, SOCK_IN, "Z") == nullptr || socket(node, SOCK_OUT, "Vector") == nullptr)
    {
      ok_ = false;
      return {};
    }
    link(x, {node, "X"});
    link(y, {node, "Y"});
    link(z, {node, "Z"});
    if (!ok_) {
      return {};
    }
    return vector(node);
  }

  /**
   * A Voronoi node fixed to the distance to the cell edge over Euclidean cells, which is the only
   * feature a scratch is. The sockets of a node whose storage says it reads nothing are dropped
   * here, exactly as the generator does before it links a pattern's parameters.
   */
  bNode *voronoi_edge(const bool is_2d)
  {
    bNode *node = add_static_node(SH_NODE_TEX_VORONOI);
    if (node == nullptr || node->storage == nullptr) {
      ok_ = false;
      return nullptr;
    }
    NodeTexVoronoi &tex = *static_cast<NodeTexVoronoi *>(node->storage);
    tex.dimensions = is_2d ? 2 : 3;
    tex.feature = SHD_VORONOI_DISTANCE_TO_EDGE;
    tex.distance = SHD_VORONOI_EUCLIDEAN;
    /* The node's own default, and the value the CPU composite of a scratch passes: an option of its
     * own would only let the two pick different ranges for the distance. */
    tex.normalize = false;
    nodes::update_node_declaration_and_sockets(group_, *node);
    if (socket(node, SOCK_IN, "Vector") == nullptr || socket(node, SOCK_OUT, "Distance") == nullptr)
    {
      ok_ = false;
      return nullptr;
    }
    return node;
  }

  /**
   * A Noise node of \a type, normalized or not, in \a is_2d dimensions. Both choices are storage,
   * not sockets: a group holds them for every row that instantiates it, which is why the generators
   * whose look depends on them are the ones a single node makes.
   */
  bNode *noise(const bool is_2d, const int type, const bool normalize)
  {
    bNode *node = add_static_node(SH_NODE_TEX_NOISE);
    if (node == nullptr || node->storage == nullptr) {
      ok_ = false;
      return nullptr;
    }
    NodeTexNoise &tex = *static_cast<NodeTexNoise *>(node->storage);
    tex.dimensions = is_2d ? 2 : 3;
    tex.type = uint8_t(type);
    tex.normalize = normalize ? 1 : 0;
    nodes::update_node_declaration_and_sockets(group_, *node);
    if (socket(node, SOCK_IN, "Vector") == nullptr || socket(node, SOCK_OUT, "Fac") == nullptr) {
      ok_ = false;
      return nullptr;
    }
    return node;
  }

  bool link(const GroupSocket from, const GroupSocket to)
  {
    if (from.node == nullptr || to.node == nullptr) {
      ok_ = false;
      return false;
    }
    bNodeSocket *from_socket = socket(from.node, SOCK_OUT, from.socket);
    bNodeSocket *to_socket = socket(to.node, SOCK_IN, to.socket);
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

  /** Link a socket of a node to the group's output \a identifier. */
  bool link_output(const GroupSocket from, const char *identifier)
  {
    return link(from, {group_output_, identifier});
  }

  /** Set a float input's default; a missing socket is a build error. */
  bool set_float(bNode *node, const char *input, const float value)
  {
    if (node == nullptr) {
      ok_ = false;
      return false;
    }
    bNodeSocket *sock = socket(node, SOCK_IN, input);
    if (sock == nullptr || sock->default_value == nullptr) {
      ok_ = false;
      return false;
    }
    static_cast<bNodeSocketValueFloat *>(sock->default_value)->value = value;
    return true;
  }

  /** `a op b` where both are outputs of nodes, one input each. */
  bNode *binary(const int operation,
                const GroupSocket a,
                const GroupSocket b,
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
                      const GroupSocket a,
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
                      const GroupSocket b,
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
  bNode *compare(const GroupSocket value, const float target, const float epsilon = 0.5f)
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
  bNode *mix(const GroupSocket a, const GroupSocket b, const GroupSocket factor)
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
  static GroupSocket value(const bNode *node)
  {
    return {const_cast<bNode *>(node), "Value"};
  }

  static GroupSocket vector(const bNode *node)
  {
    return {const_cast<bNode *>(node), "Vector"};
  }

  /** The output of a float Mix node. */
  static GroupSocket result(const bNode *node)
  {
    return {const_cast<bNode *>(node), "Result_Float"};
  }

  /** The output of a texture node's float answer. */
  static GroupSocket fac(const bNode *node)
  {
    return {const_cast<bNode *>(node), "Fac"};
  }

  /** `min(max(x, 0), 1)` -- the clamp a smooth step starts from, in the node's own two operations. */
  GroupSocket clamp01(const GroupSocket x)
  {
    bNode *lo = binary_const(NODE_MATH_MAXIMUM, x, 0.0f);
    if (lo == nullptr) {
      return {};
    }
    bNode *hi = binary_const(NODE_MATH_MINIMUM, value(lo), 1.0f);
    return value(hi);
  }

  /** `1 - x`. */
  GroupSocket invert(const GroupSocket x)
  {
    return value(const_binary(NODE_MATH_SUBTRACT, 1.0f, x));
  }

  /**
   * The threshold every compound generator ends with: the position of \a x between \a lo and \a hi,
   * clamped, through the Hermite curve `u * u * (3 - 2 * u)`. There is no smooth-step operation in
   * a Math node, so the curve is made of the ones there are, in this order -- and the CPU mirror
   * evaluates them in this order too.
   */
  GroupSocket smooth_step(const GroupSocket x, const GroupSocket lo, const GroupSocket hi)
  {
    bNode *range = binary(NODE_MATH_SUBTRACT, hi, lo);
    bNode *safe = binary_const(NODE_MATH_MAXIMUM, value(range), 1e-6f);
    bNode *shifted = binary(NODE_MATH_SUBTRACT, x, lo);
    bNode *position = binary(NODE_MATH_DIVIDE, value(shifted), value(safe));
    if (!ok_) {
      return {};
    }
    const GroupSocket u = clamp01(value(position));
    bNode *squared = binary(NODE_MATH_MULTIPLY, u, u);
    bNode *twice = binary_const(NODE_MATH_MULTIPLY, u, 2.0f);
    bNode *curve = const_binary(NODE_MATH_SUBTRACT, 3.0f, value(twice));
    return value(binary(NODE_MATH_MULTIPLY, value(squared), value(curve)));
  }

  /** `a - b` of two sockets, as a socket. */
  GroupSocket subtract(const GroupSocket a, const GroupSocket b)
  {
    return value(binary(NODE_MATH_SUBTRACT, a, b));
  }

  /** `a * b` of two sockets, as a socket. */
  GroupSocket multiply(const GroupSocket a, const GroupSocket b)
  {
    return value(binary(NODE_MATH_MULTIPLY, a, b));
  }

  /** `a + b` of two sockets, as a socket. */
  GroupSocket add(const GroupSocket a, const GroupSocket b)
  {
    return value(binary(NODE_MATH_ADD, a, b));
  }

  /** `a / b` of two sockets, as a socket. */
  GroupSocket divide(const GroupSocket a, const GroupSocket b)
  {
    return value(binary(NODE_MATH_DIVIDE, a, b));
  }

  /** `floor(x)`. */
  GroupSocket floor(const GroupSocket x)
  {
    bNode *node = math(NODE_MATH_FLOOR);
    if (node == nullptr) {
      return {};
    }
    link(x, {node, "Value"});
    return value(node);
  }

  /** The integer remainder of \a x over \a period, as the Floored Modulo node computes it. */
  GroupSocket floored_modulo(const GroupSocket x, const float period)
  {
    bNode *node = binary_const(NODE_MATH_FLOORED_MODULO, x, period);
    return value(node);
  }

  /** Put the group output one column after the last node added. */
  void place_output()
  {
    this->place(group_output_);
  }

 private:
  /** One of the two Separate nodes: \a type, its single input and its three outputs. */
  bool separate(const int type,
                const char *input,
                const std::array<const char *, 3> outputs,
                const GroupSocket source,
                GroupSocket r_components[3])
  {
    bNode *node = add_static_node(type);
    if (node == nullptr) {
      return false;
    }
    if (socket(node, SOCK_IN, input) == nullptr || socket(node, SOCK_OUT, outputs[0]) == nullptr ||
        socket(node, SOCK_OUT, outputs[1]) == nullptr ||
        socket(node, SOCK_OUT, outputs[2]) == nullptr)
    {
      ok_ = false;
      return false;
    }
    if (!link(source, {node, input})) {
      return false;
    }
    for (const int i : IndexRange(3)) {
      r_components[i] = {node, outputs[i]};
    }
    return true;
  }

  static bNodeSocket *socket(bNode *node, const eNodeSocketInOut in_out, const char *identifier)
  {
    return bke::node_find_socket(*node, in_out, UString::from_ptr_noinline(identifier));
  }

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

}  // namespace blender
