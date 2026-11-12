/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The projected lookup of a paint layer row: the frame-space position, the per-mode UV math, the
 * facing weight, the frame factor (the two footprint axes, the optional depth slab), and the frame
 * clip factor. Built once per row group and shared by every map the row reads.
 *
 * The coordinate space is the controller object's when the row names one, else the painted
 * object's own object space. The inline center/rotation/size are applied inside that space, so an
 * Empty and the inline values compose instead of replacing one another. The facing weight reads the
 * world normal against a direction the value sync bakes out of the controller's transform (which a
 * live node chain cannot name); without a controller the Vector Transform node names the shaded
 * object instead, so a turned object still faces exactly.
 */

#include <cmath>

#include "BLI_math_vector.h"
#include "BLI_ustring.hh"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint_layers.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_object_types.h"

#include "paint_layers_generate_build_intern.hh"
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"

namespace blender::bke::paint_layers {

namespace {

/* The CPU bake reads the same values from BKE_paint_layers.hh. */
constexpr float INV_TWO_PI = PAINT_LAYER_PROJECTION_INV_TWO_PI;
constexpr float INV_PI = PAINT_LAYER_PROJECTION_INV_PI;
constexpr float EPSILON = PAINT_LAYER_PROJECTION_EPSILON;

bNode *projection_group_input(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (node.is_group_input()) {
      return &node;
    }
  }
  return nullptr;
}

void float_socket_set(bNodeSocket *socket, const float value)
{
  if (socket != nullptr && socket->default_value != nullptr) {
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = value;
  }
}

bNode *projection_value_node(bNodeTree &tree,
                             const float value,
                             Vector<bNode *> &made,
                             layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  if (node == nullptr) {
    return nullptr;
  }
  bNodeSocket *out = socket_out(*node, "Value");
  if (out == nullptr) {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return nullptr;
  }
  float_socket_set(out, value);
  made.append(node);
  grid.place_extra(*node);
  return node;
}

NodeOutput projection_math(bNodeTree &tree,
                           const int operation,
                           const NodeOutput a,
                           const NodeOutput b,
                           const float b_default,
                           Vector<bNode *> &made,
                           layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (node == nullptr) {
    return {};
  }
  node->custom1 = operation;
  bNodeSocket *in_a = socket_in(*node, "Value");
  bNodeSocket *in_b = socket_in(*node, "Value_001");
  bNodeSocket *out = socket_out(*node, "Value");
  if (in_a == nullptr || in_b == nullptr || out == nullptr || a.node == nullptr ||
      a.socket == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  bke::node_add_link(tree, *a.node, *a.socket, *node, *in_a);
  if (b.node != nullptr && b.socket != nullptr) {
    bke::node_add_link(tree, *b.node, *b.socket, *node, *in_b);
  }
  else {
    float_socket_set(in_b, b_default);
  }
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

NodeOutput projection_math_unary(bNodeTree &tree,
                                 const int operation,
                                 const NodeOutput a,
                                 Vector<bNode *> &made,
                                 layout::LayoutGrid &grid)
{
  return projection_math(tree, operation, a, {}, 0.0f, made, grid);
}

NodeOutput projection_affine(bNodeTree &tree,
                             const NodeOutput value,
                             const float mul,
                             const float add,
                             Vector<bNode *> &made,
                             layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (node == nullptr) {
    return {};
  }
  node->custom1 = NODE_MATH_MULTIPLY_ADD;
  bNodeSocket *in_value = socket_in(*node, "Value");
  bNodeSocket *in_mul = socket_in(*node, "Value_001");
  bNodeSocket *in_add = socket_in(*node, "Value_002");
  bNodeSocket *out = socket_out(*node, "Value");
  if (in_value == nullptr || in_mul == nullptr || in_add == nullptr || out == nullptr ||
      value.node == nullptr || value.socket == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  bke::node_add_link(tree, *value.node, *value.socket, *node, *in_value);
  float_socket_set(in_mul, mul);
  float_socket_set(in_add, add);
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

NodeOutput projection_one_minus(bNodeTree &tree,
                                const NodeOutput value,
                                Vector<bNode *> &made,
                                layout::LayoutGrid &grid)
{
  bNode *one = projection_value_node(tree, 1.0f, made, grid);
  if (one == nullptr) {
    return {};
  }
  return projection_math(
      tree, NODE_MATH_SUBTRACT, {one, socket_out(*one, "Value")}, value, 0.0f, made, grid);
}

NodeOutput projection_vector_math(bNodeTree &tree,
                                  const int operation,
                                  const NodeOutput a,
                                  const NodeOutput b,
                                  const float scale,
                                  Vector<bNode *> &made,
                                  layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
  if (node == nullptr) {
    return {};
  }
  node->custom1 = operation;
  bNodeSocket *in_a = socket_in(*node, "Vector");
  bNodeSocket *out = socket_out(*node, "Vector");
  if (in_a == nullptr || out == nullptr || a.node == nullptr || a.socket == nullptr) {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  bke::node_add_link(tree, *a.node, *a.socket, *node, *in_a);
  if (operation == NODE_VECTOR_MATH_SCALE) {
    bNodeSocket *in_scale = socket_in(*node, "Scale");
    if (in_scale == nullptr) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
      return {};
    }
    if (b.node != nullptr && b.socket != nullptr) {
      bke::node_add_link(tree, *b.node, *b.socket, *node, *in_scale);
    }
    else {
      float_socket_set(in_scale, scale);
    }
  }
  else if (b.node != nullptr && b.socket != nullptr) {
    bNodeSocket *in_b = socket_in(*node, "Vector_001");
    if (in_b == nullptr) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
      return {};
    }
    bke::node_add_link(tree, *b.node, *b.socket, *node, *in_b);
  }
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

NodeOutput projection_vector_length(bNodeTree &tree,
                                    const NodeOutput vector,
                                    Vector<bNode *> &made,
                                    layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
  if (node == nullptr) {
    return {};
  }
  node->custom1 = NODE_VECTOR_MATH_LENGTH;
  bNodeSocket *in_vector = socket_in(*node, "Vector");
  bNodeSocket *out = socket_out(*node, "Value");
  if (in_vector == nullptr || out == nullptr || vector.node == nullptr ||
      vector.socket == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  bke::node_add_link(tree, *vector.node, *vector.socket, *node, *in_vector);
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

NodeOutput projection_vector_rotate(bNodeTree &tree,
                                    const NodeOutput vector,
                                    const NodeOutput rotation,
                                    const bool invert,
                                    Vector<bNode *> &made,
                                    layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_ROTATE);
  if (node == nullptr) {
    return {};
  }
  node->custom1 = NODE_VECTOR_ROTATE_TYPE_EULER_XYZ;
  node->custom2 = invert ? 1 : 0;
  /* Keep the Euler XYZ (inverted) order in step with the CPU's `composite_projection_position`,
   * which builds the same matrix with `eul_to_mat3` and transposes it. */
  bNodeSocket *in_vector = socket_in(*node, "Vector");
  bNodeSocket *in_rotation = socket_in(*node, "Rotation");
  bNodeSocket *out = socket_out(*node, "Vector");
  if (in_vector == nullptr || in_rotation == nullptr || out == nullptr ||
      vector.node == nullptr || vector.socket == nullptr || rotation.node == nullptr ||
      rotation.socket == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  bke::node_add_link(tree, *vector.node, *vector.socket, *node, *in_vector);
  bke::node_add_link(tree, *rotation.node, *rotation.socket, *node, *in_rotation);
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

/** The X, Y and Z outputs of one Separate XYZ node. */
struct SeparateOutputs {
  bNode *node = nullptr;
  NodeOutput component[3] = {};
};

SeparateOutputs projection_separate(bNodeTree &tree,
                                    const NodeOutput vector,
                                    Vector<bNode *> &made,
                                    layout::LayoutGrid &grid)
{
  SeparateOutputs result;
  if (vector.node == nullptr || vector.socket == nullptr) {
    return result;
  }
  bNode *node = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
  if (node == nullptr) {
    return result;
  }
  bNodeSocket *in_vector = socket_in(*node, "Vector");
  if (in_vector == nullptr) {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return result;
  }
  bke::node_add_link(tree, *vector.node, *vector.socket, *node, *in_vector);
  const char *names[3] = {"X", "Y", "Z"};
  for (int i = 0; i < 3; i++) {
    bNodeSocket *out = socket_out(*node, names[i]);
    if (out == nullptr) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
      return {};
    }
    result.component[i] = {node, out};
  }
  made.append(node);
  grid.place_extra(*node);
  result.node = node;
  return result;
}

NodeOutput projection_combine_xyz(bNodeTree &tree,
                                  const NodeOutput x,
                                  const NodeOutput y,
                                  const float z,
                                  Vector<bNode *> &made,
                                  layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
  if (node == nullptr) {
    return {};
  }
  bNodeSocket *in_x = socket_in(*node, "X");
  bNodeSocket *in_y = socket_in(*node, "Y");
  bNodeSocket *in_z = socket_in(*node, "Z");
  bNodeSocket *out = socket_out(*node, "Vector");
  if (in_x == nullptr || in_y == nullptr || in_z == nullptr || out == nullptr) {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  if (x.node != nullptr && x.socket != nullptr) {
    bke::node_add_link(tree, *x.node, *x.socket, *node, *in_x);
  }
  if (y.node != nullptr && y.socket != nullptr) {
    bke::node_add_link(tree, *y.node, *y.socket, *node, *in_y);
  }
  float_socket_set(in_z, z);
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

/** One face's local UV in [0, 1]. Face 0 is X, 1 is Y, 2 is Z. */
NodeOutput projection_face_uv(bNodeTree &tree,
                              const int face,
                              const NodeOutput q[3],
                              Vector<bNode *> &made,
                              layout::LayoutGrid &grid)
{
  NodeOutput u;
  NodeOutput v;
  if (face == 0) {
    /* +X: u from -Y, v from +Z. */
    u = projection_affine(tree, q[1], -0.5f, 0.5f, made, grid);
    v = projection_affine(tree, q[2], 0.5f, 0.5f, made, grid);
  }
  else if (face == 1) {
    u = projection_affine(tree, q[0], 0.5f, 0.5f, made, grid);
    v = projection_affine(tree, q[2], 0.5f, 0.5f, made, grid);
  }
  else {
    u = projection_affine(tree, q[0], 0.5f, 0.5f, made, grid);
    v = projection_affine(tree, q[1], 0.5f, 0.5f, made, grid);
  }
  return projection_combine_xyz(tree, u, v, 0.0f, made, grid);
}

/**
 * The floor of the painted object's UV, as the UDIM tile origin a tiled source map reads. Built on
 * first use and shared by every tiled map of the row.
 */
bNode *projection_tile_origin_ensure(bNodeTree &tree,
                                     ProjectionNodes &projection,
                                     Vector<bNode *> &made,
                                     layout::LayoutGrid &grid)
{
  if (projection.tile_origin != nullptr) {
    return projection.tile_origin;
  }
  if (projection.tex_coord == nullptr) {
    return nullptr;
  }
  bNodeSocket *uv = socket_out(*projection.tex_coord, "UV");
  if (uv == nullptr) {
    return nullptr;
  }
  const SeparateOutputs coords = projection_separate(
      tree, {projection.tex_coord, uv}, made, grid);
  if (coords.node == nullptr) {
    return nullptr;
  }
  const NodeOutput floor_x = projection_math_unary(
      tree, NODE_MATH_FLOOR, coords.component[0], made, grid);
  const NodeOutput floor_y = projection_math_unary(
      tree, NODE_MATH_FLOOR, coords.component[1], made, grid);
  const NodeOutput origin = projection_combine_xyz(tree, floor_x, floor_y, 0.0f, made, grid);
  if (origin.node == nullptr) {
    return nullptr;
  }
  projection.tile_origin = origin.node;
  return origin.node;
}

/**
 * Move \a mapped into its UDIM tile before sampling a tiled source: the pattern is wrapped within
 * the tile and the surface UV's tile origin is added back, so the tiled Image Texture selects the
 * tile the painted surface belongs to.
 */
bNode *projection_tile_local_uv(bNodeTree &tree,
                                bNode &mapped,
                                bNode &tile_origin,
                                Vector<bNode *> &made,
                                layout::LayoutGrid &grid)
{
  bNodeSocket *mapped_out = socket_out(mapped, "Vector");
  bNodeSocket *origin_out = socket_out(tile_origin, "Vector");
  if (mapped_out == nullptr || origin_out == nullptr) {
    return &mapped;
  }
  const SeparateOutputs coords = projection_separate(tree, {&mapped, mapped_out}, made, grid);
  if (coords.node == nullptr) {
    return &mapped;
  }
  const NodeOutput frac_x = projection_math_unary(
      tree, NODE_MATH_FRACTION, coords.component[0], made, grid);
  const NodeOutput frac_y = projection_math_unary(
      tree, NODE_MATH_FRACTION, coords.component[1], made, grid);
  const NodeOutput local = projection_combine_xyz(tree, frac_x, frac_y, 0.0f, made, grid);
  if (local.node == nullptr) {
    return &mapped;
  }
  const NodeOutput tiled = projection_vector_math(tree,
                                                  NODE_VECTOR_MATH_ADD,
                                                  local,
                                                  {&tile_origin, origin_out},
                                                  0.0f,
                                                  made,
                                                  grid);
  return (tiled.node != nullptr) ? tiled.node : &mapped;
}

/** `extent - |value|`, as the depth of one axis inside the frame. */
NodeOutput projection_axis_depth(bNodeTree &tree,
                                 const NodeOutput value,
                                 Vector<bNode *> &made,
                                 layout::LayoutGrid &grid)
{
  NodeOutput absolute = projection_math_unary(tree, NODE_MATH_ABSOLUTE, value, made, grid);
  return projection_affine(tree, absolute, -1.0f, PAINT_LAYER_PROJECTION_FRAME_EXTENT, made, grid);
}

/**
 * The smooth 0..1 factor of one frame-space axis: one inside, fading to zero over
 * #PAINT_LAYER_PROJECTION_EDGE_SOFT. The CPU repeats the same two Multiply Add nodes in
 * #BKE_paint_layers_projection_axis_factor and clamps them to 0..1; the graph clamps with the
 * node's own clamp flag for the same reason. Outside the frame the raw factor turns negative, and
 * past the frame's corners two negative footprint factors would multiply to a positive one again,
 * which would lay the map where the row covers nothing instead of showing what is below.
 */
NodeOutput projection_axis_factor(bNodeTree &tree,
                                  const NodeOutput value,
                                  Vector<bNode *> &made,
                                  layout::LayoutGrid &grid)
{
  const NodeOutput depth = projection_axis_depth(tree, value, made, grid);
  NodeOutput factor = projection_affine(
      tree, depth, 1.0f / PAINT_LAYER_PROJECTION_EDGE_SOFT, 0.0f, made, grid);
  if (factor.node != nullptr) {
    factor.node->custom2 = SHD_MATH_CLAMP;
  }
  return factor;
}

/** Whether one frame-space axis reads inside the frame: the same soft edge factor the CPU
 * repeats in #BKE_paint_layers_projection_axis_factor. */
NodeOutput projection_inside_axis(bNodeTree &tree,
                                  const NodeOutput value,
                                  Vector<bNode *> &made,
                                  layout::LayoutGrid &grid)
{
  return projection_axis_factor(tree, value, made, grid);
}

/** The product of two inside factors, as the CPU multiplies them. */
NodeOutput projection_inside_product(bNodeTree &tree,
                                     const NodeOutput a,
                                     const NodeOutput b,
                                     Vector<bNode *> &made,
                                     layout::LayoutGrid &grid)
{
  return projection_math(tree, NODE_MATH_MULTIPLY, a, b, 0.0f, made, grid);
}

/** The box inside over the frame-space position's three axes: the product of the axes' factors,
 * the same chain the CPU repeats in #BKE_paint_layers_projection_frame_factor. */
NodeOutput projection_inside_all(bNodeTree &tree,
                                 const NodeOutput components[3],
                                 Vector<bNode *> &made,
                                 layout::LayoutGrid &grid)
{
  const NodeOutput inside_x = projection_inside_axis(tree, components[0], made, grid);
  const NodeOutput inside_y = projection_inside_axis(tree, components[1], made, grid);
  const NodeOutput inside_z = projection_inside_axis(tree, components[2], made, grid);
  return projection_inside_product(
      tree, projection_inside_product(tree, inside_x, inside_y, made, grid), inside_z, made, grid);
}

/**
 * `value * scale + offset` clamped to 0..1: one half of the facing weight's hermite, with the
 * scale and offset the value sync baked out of the facing limit. The CPU repeats the clamp in
 * #BKE_paint_layers_projection_facing_weight.
 */
NodeOutput projection_facing_hermite_entry(bNodeTree &tree,
                                           const NodeOutput value,
                                           const NodeOutput scale,
                                           const NodeOutput offset,
                                           Vector<bNode *> &made,
                                           layout::LayoutGrid &grid)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (node == nullptr) {
    return {};
  }
  node->custom1 = NODE_MATH_MULTIPLY_ADD;
  node->custom2 = SHD_MATH_CLAMP;
  bNodeSocket *in_value = socket_in(*node, "Value");
  bNodeSocket *in_mul = socket_in(*node, "Value_001");
  bNodeSocket *in_add = socket_in(*node, "Value_002");
  bNodeSocket *out = socket_out(*node, "Value");
  if (in_value == nullptr || in_mul == nullptr || in_add == nullptr || out == nullptr ||
      value.node == nullptr || value.socket == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  bke::node_add_link(tree, *value.node, *value.socket, *node, *in_value);
  if (scale.node != nullptr && scale.socket != nullptr) {
    bke::node_add_link(tree, *scale.node, *scale.socket, *node, *in_mul);
  }
  if (offset.node != nullptr && offset.socket != nullptr) {
    bke::node_add_link(tree, *offset.node, *offset.socket, *node, *in_add);
  }
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

/** `t * t * (3 - 2 * t)`, the smooth half of the facing weight over its clamped entry. */
NodeOutput projection_facing_hermite(bNodeTree &tree,
                                     const NodeOutput t,
                                     Vector<bNode *> &made,
                                     layout::LayoutGrid &grid)
{
  bNode *squared = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *mirror = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *weight = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (squared == nullptr || mirror == nullptr || weight == nullptr) {
    for (bNode *node : {squared, mirror, weight}) {
      if (node != nullptr) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
    }
    return {};
  }
  squared->custom1 = NODE_MATH_MULTIPLY;
  mirror->custom1 = NODE_MATH_MULTIPLY_ADD;
  weight->custom1 = NODE_MATH_MULTIPLY;
  bNodeSocket *squared_a = socket_in(*squared, "Value");
  bNodeSocket *squared_b = socket_in(*squared, "Value_001");
  bNodeSocket *squared_out = socket_out(*squared, "Value");
  bNodeSocket *mirror_a = socket_in(*mirror, "Value");
  bNodeSocket *mirror_mul = socket_in(*mirror, "Value_001");
  bNodeSocket *mirror_add = socket_in(*mirror, "Value_002");
  bNodeSocket *mirror_out = socket_out(*mirror, "Value");
  bNodeSocket *weight_a = socket_in(*weight, "Value");
  bNodeSocket *weight_b = socket_in(*weight, "Value_001");
  bNodeSocket *weight_out = socket_out(*weight, "Value");
  if (squared_a == nullptr || squared_b == nullptr || squared_out == nullptr ||
      mirror_a == nullptr || mirror_mul == nullptr || mirror_add == nullptr ||
      mirror_out == nullptr || weight_a == nullptr || weight_b == nullptr ||
      weight_out == nullptr || t.node == nullptr || t.socket == nullptr)
  {
    for (bNode *node : {squared, mirror, weight}) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return {};
  }
  bke::node_add_link(tree, *t.node, *t.socket, *squared, *squared_a);
  bke::node_add_link(tree, *t.node, *t.socket, *squared, *squared_b);
  bke::node_add_link(tree, *t.node, *t.socket, *mirror, *mirror_a);
  float_socket_set(mirror_mul, -2.0f);
  float_socket_set(mirror_add, 3.0f);
  bke::node_add_link(tree, *squared, *squared_out, *weight, *weight_a);
  bke::node_add_link(tree, *mirror, *mirror_out, *weight, *weight_b);
  made.append(squared);
  made.append(mirror);
  made.append(weight);
  grid.place_extra(*squared);
  grid.place_extra(*mirror);
  grid.place_extra(*weight);
  return {weight, weight_out};
}

/** `mix(1, inside, clip)`: the frame factor falls back to one when the clip toggle is off. */
NodeOutput projection_clip_mix(bNodeTree &tree,
                               const NodeOutput inside,
                               const NodeOutput clip,
                               Vector<bNode *> &made,
                               layout::LayoutGrid &grid)
{
  NodeOutput one_minus = projection_one_minus(tree, clip, made, grid);
  NodeOutput scaled = projection_math(tree, NODE_MATH_MULTIPLY, inside, clip, 0.0f, made, grid);
  return projection_math(tree, NODE_MATH_ADD, scaled, one_minus, 0.0f, made, grid);
}

/** The three normalized face weights of Box and Triplanar from the frame-space normal. */
bool projection_weights_add(bNodeTree &tree,
                            const NodeOutput normal,
                            const bool triplanar,
                            const NodeOutput blend,
                            NodeOutput r_weights[3],
                            Vector<bNode *> &made,
                            layout::LayoutGrid &grid)
{
  SeparateOutputs n = projection_separate(tree, normal, made, grid);
  if (n.node == nullptr) {
    return false;
  }
  NodeOutput abs_n[3];
  for (int i = 0; i < 3; i++) {
    abs_n[i] = projection_math_unary(tree, NODE_MATH_ABSOLUTE, n.component[i], made, grid);
    if (abs_n[i].node == nullptr) {
      return false;
    }
  }
  NodeOutput exponent;
  if (triplanar) {
    /* Hard at blend 0 down to soft at blend 1: a controllable normal blend. The CPU bake takes
     * the same line from #BKE_paint_layers_projection_triplanar_exponent. */
    NodeOutput scaled = projection_math(
        tree,
        NODE_MATH_MULTIPLY,
        blend,
        {},
        -(PAINT_LAYER_PROJECTION_TRIPLANAR_EXPONENT_HARD -
          PAINT_LAYER_PROJECTION_TRIPLANAR_EXPONENT_SOFT),
        made,
        grid);
    exponent = projection_math(
        tree,
        NODE_MATH_ADD,
        scaled,
        {},
        PAINT_LAYER_PROJECTION_TRIPLANAR_EXPONENT_HARD,
        made,
        grid);
  }
  else {
    bNode *value = projection_value_node(tree, PAINT_LAYER_PROJECTION_BOX_EXPONENT, made, grid);
    if (value == nullptr) {
      return false;
    }
    exponent = {value, socket_out(*value, "Value")};
  }
  if (exponent.node == nullptr) {
    return false;
  }
  NodeOutput raw[3];
  for (int i = 0; i < 3; i++) {
    raw[i] = projection_math(tree, NODE_MATH_POWER, abs_n[i], exponent, 1.0f, made, grid);
    if (raw[i].node == nullptr) {
      return false;
    }
  }
  NodeOutput sum = projection_math(tree, NODE_MATH_ADD, raw[0], raw[1], 0.0f, made, grid);
  sum = projection_math(tree, NODE_MATH_ADD, sum, raw[2], 0.0f, made, grid);
  NodeOutput safe = projection_math(tree, NODE_MATH_MAXIMUM, sum, {}, EPSILON, made, grid);
  for (int i = 0; i < 3; i++) {
    r_weights[i] = projection_math(tree, NODE_MATH_DIVIDE, raw[i], safe, 1.0f, made, grid);
    if (r_weights[i].node == nullptr) {
      return false;
    }
  }
  return true;
}

/** The six hard face weights of the Cube atlas from the frame-space position. */
bool projection_cube_weights_add(bNodeTree &tree,
                                 const NodeOutput q[3],
                                 NodeOutput r_weights[6],
                                 Vector<bNode *> &made,
                                 layout::LayoutGrid &grid)
{
  NodeOutput abs_q[3];
  for (int i = 0; i < 3; i++) {
    abs_q[i] = projection_math_unary(tree, NODE_MATH_ABSOLUTE, q[i], made, grid);
  }
  /* An epsilon on the leading axis makes exact ties fall to X, then Y, then Z. */
  const NodeOutput abs_x_eps = projection_affine(tree, abs_q[0], 1.0f, EPSILON, made, grid);
  const NodeOutput abs_y_eps = projection_affine(tree, abs_q[1], 1.0f, EPSILON, made, grid);
  const NodeOutput cmp_x_ge_y = projection_math(
      tree, NODE_MATH_GREATER_THAN, abs_x_eps, abs_q[1], 0.0f, made, grid);
  const NodeOutput cmp_x_ge_z = projection_math(
      tree, NODE_MATH_GREATER_THAN, abs_x_eps, abs_q[2], 0.0f, made, grid);
  const NodeOutput cmp_y_gt_x = projection_math(
      tree, NODE_MATH_GREATER_THAN, abs_q[1], abs_q[0], 0.0f, made, grid);
  const NodeOutput cmp_y_ge_z = projection_math(
      tree, NODE_MATH_GREATER_THAN, abs_y_eps, abs_q[2], 0.0f, made, grid);
  const NodeOutput cmp_z_gt_x = projection_math(
      tree, NODE_MATH_GREATER_THAN, abs_q[2], abs_q[0], 0.0f, made, grid);
  const NodeOutput cmp_z_gt_y = projection_math(
      tree, NODE_MATH_GREATER_THAN, abs_q[2], abs_q[1], 0.0f, made, grid);
  const NodeOutput any[3] = {
      projection_inside_product(tree, cmp_x_ge_y, cmp_x_ge_z, made, grid),
      projection_inside_product(tree, cmp_y_gt_x, cmp_y_ge_z, made, grid),
      projection_inside_product(tree, cmp_z_gt_x, cmp_z_gt_y, made, grid),
  };
  for (int axis = 0; axis < 3; axis++) {
    NodeOutput positive = projection_math(
        tree, NODE_MATH_GREATER_THAN, q[axis], {}, 0.0f, made, grid);
    NodeOutput negative = projection_one_minus(tree, positive, made, grid);
    if (any[axis].node == nullptr || positive.node == nullptr || negative.node == nullptr) {
      return false;
    }
    r_weights[axis * 2 + 0] = projection_inside_product(
        tree, any[axis], positive, made, grid);
    r_weights[axis * 2 + 1] = projection_inside_product(
        tree, any[axis], negative, made, grid);
  }
  return true;
}

}  // namespace

bNodeSocket *PaintLayersTreeBuilder::projection_input_socket(
    bNodeTree &tree,
    const MaterialPaintLayer &row,
    const Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> &inputs)
{
  bNodeTreeInterfaceSocket *const *iface = inputs.lookup_ptr(&row);
  if (iface == nullptr || *iface == nullptr || (*iface)->identifier == nullptr) {
    return nullptr;
  }
  bNode *group_input = projection_group_input(tree);
  if (group_input == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline((*iface)->identifier));
}

ProjectionNodes *PaintLayersTreeBuilder::projection_nodes_ensure(
    bNodeTree &tree,
    const MaterialPaintLayer &row,
    Vector<bNode *> &r_nodes)
{
  if (!BKE_paint_layers_projection_applies(ma_, row)) {
    return nullptr;
  }
  if (ProjectionNodes *found = projection_nodes_.lookup_ptr(&row)) {
    return found;
  }

  bNodeSocket *center_socket = projection_input_socket(tree, row, projection_center_inputs_);
  bNodeSocket *size_socket = projection_input_socket(tree, row, projection_size_inputs_);
  bNodeSocket *rotation_socket = projection_input_socket(
      tree, row, projection_rotation_inputs_);
  bNodeSocket *blend_socket = projection_input_socket(tree, row, projection_blend_inputs_);
  bNodeSocket *clip_socket = projection_input_socket(tree, row, projection_clip_inputs_);
  bNodeSocket *depth_socket = projection_input_socket(tree, row, projection_depth_inputs_);
  bNodeSocket *facing_socket = projection_input_socket(tree, row, projection_facing_inputs_);
  bNodeSocket *facing_dir_socket = projection_input_socket(
      tree, row, projection_facing_dir_inputs_);
  bNodeSocket *aspect_socket = projection_input_socket(tree, row, projection_aspect_inputs_);
  bNodeSocket *tiling_socket = projection_input_socket(tree, row, projection_tiling_inputs_);
  bNode *group_input = projection_group_input(tree);
  if (center_socket == nullptr || size_socket == nullptr || rotation_socket == nullptr ||
      blend_socket == nullptr || clip_socket == nullptr || depth_socket == nullptr ||
      facing_socket == nullptr || facing_dir_socket == nullptr || aspect_socket == nullptr ||
      tiling_socket == nullptr ||
      group_input == nullptr)
  {
    return nullptr;
  }

  Vector<bNode *> made;
  const auto fail = [&]() {
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return static_cast<ProjectionNodes *>(nullptr);
  };

  bNode *tex_coord = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_COORD);
  if (tex_coord == nullptr) {
    return fail();
  }
  if (row.projection_object != nullptr) {
    /* The node owns a user of its own, as any node that names an ID does; the row's user is a
     * separate one, held by the row and released with it. */
    tex_coord->id = &row.projection_object->id;
    id_us_plus(&row.projection_object->id);
  }
  bNodeSocket *object_out = socket_out(*tex_coord, "Object");
  if (object_out == nullptr) {
    made.append(tex_coord);
    return fail();
  }
  made.append(tex_coord);
  grid_.place_extra(*tex_coord);

  const NodeOutput position = {tex_coord, object_out};
  const NodeOutput center_value = {group_input, center_socket};
  const NodeOutput rotation_value = {group_input, rotation_socket};
  const NodeOutput size_value = {group_input, size_socket};

  const NodeOutput centered = projection_vector_math(
      tree, NODE_VECTOR_MATH_SUBTRACT, position, center_value, 0.0f, made, grid_);
  const NodeOutput rotated_position = projection_vector_rotate(
      tree, centered, rotation_value, true, made, grid_);
  /* The Size input is the frame's full extent, the extent the cage shows, so the frame-space
   * coordinate reads the offset over the half of it: the axis is inside while
   * |q| < #PAINT_LAYER_PROJECTION_FRAME_EXTENT. The CPU composite divides by the same half
   * (#composite_projection_position). */
  const NodeOutput half_size = projection_vector_math(
      tree, NODE_VECTOR_MATH_SCALE, size_value, {}, 0.5f, made, grid_);
  const NodeOutput q_unscaled = projection_vector_math(
      tree, NODE_VECTOR_MATH_DIVIDE, rotated_position, half_size, 1.0f, made, grid_);
  /* The image-aspect fit scales the frame's footprint coordinates before anything reads them, so
   * the UV square and the frame factor show the same rectangle. The sync scales the depth axis by
   * one, which leaves the slab untouched. */
  const NodeOutput q = projection_vector_math(
      tree, NODE_VECTOR_MATH_MULTIPLY, q_unscaled, {group_input, aspect_socket}, 0.0f, made, grid_);
  if (half_size.node == nullptr || q.node == nullptr) {
    return fail();
  }
  const SeparateOutputs q_components = projection_separate(tree, q, made, grid_);
  if (q_components.node == nullptr) {
    return fail();
  }

  const int16_t space = row.mapping.space;
  const ProjectionModeInfo &mode = BKE_paint_layers_projection_mode_info(space);
  const int8_t axis = BKE_paint_layers_projection_axis_get(row);
  NodeOutput uv[3];
  NodeOutput weights[3];
  NodeOutput inside;

  /* The frame-space component of the surface normal along the axis: a controller names one object,
   * whose whole rotation a live node chain cannot name, so the value sync bakes the world-space
   * direction the world normal reads against; without a controller the Vector Transform node names
   * the shaded object and the inline rotation applies live. */
  /* The Texture Coordinate "Normal" output is in the shaded object's space, so the facing weight
   * reads the Geometry node's world-space normal instead. */
  bNode *geometry = bke::node_add_static_node(nullptr, tree, SH_NODE_NEW_GEOMETRY);
  if (geometry == nullptr) {
    return fail();
  }
  made.append(geometry);
  grid_.place_extra(*geometry);
  bNodeSocket *world_normal_out = socket_out(*geometry, "Normal");
  if (world_normal_out == nullptr) {
    return fail();
  }
  const NodeOutput normal = {geometry, world_normal_out};
  NodeOutput facing_component;
  if (row.projection_object != nullptr) {
    NodeOutput dot = projection_vector_math(tree,
                                            NODE_VECTOR_MATH_DOT_PRODUCT,
                                            normal,
                                            {group_input, facing_dir_socket},
                                            0.0f,
                                            made,
                                            grid_);
    if (dot.node != nullptr) {
      bNodeSocket *value_out = socket_out(*dot.node, "Value");
      if (value_out != nullptr) {
        dot.socket = value_out;
      }
    }
    facing_component = dot;
  }
  else {
    bNode *to_object = bke::node_add_static_node(nullptr, tree, SH_NODE_VECT_TRANSFORM);
    if (to_object == nullptr) {
      return fail();
    }
    if (NodeShaderVectTransform *storage =
            static_cast<NodeShaderVectTransform *>(to_object->storage))
    {
      storage->type = SHD_VECT_TRANSFORM_TYPE_NORMAL;
      storage->convert_from = SHD_VECT_TRANSFORM_SPACE_WORLD;
      storage->convert_to = SHD_VECT_TRANSFORM_SPACE_OBJECT;
    }
    bNodeSocket *vt_in = socket_in(*to_object, "Vector");
    bNodeSocket *vt_out = socket_out(*to_object, "Vector");
    if (vt_in == nullptr || vt_out == nullptr) {
      made.append(to_object);
      return fail();
    }
    bke::node_add_link(tree, *geometry, *world_normal_out, *to_object, *vt_in);
    made.append(to_object);
    grid_.place_extra(*to_object);
    const NodeOutput n_frame = projection_vector_rotate(
        tree, {to_object, vt_out}, rotation_value, true, made, grid_);
    const SeparateOutputs n_components = projection_separate(tree, n_frame, made, grid_);
    if (n_components.node == nullptr) {
      return fail();
    }
    facing_component = n_components.component[axis];
  }
  if (facing_component.node == nullptr) {
    return fail();
  }
  /* The facing weight from the limit's baked scale and offset: one clamped entry, then the smooth
   * t * t * (3 - 2 t), then the toggle mix. */
  const SeparateOutputs facing = projection_separate(
      tree, {group_input, facing_socket}, made, grid_);
  if (facing.node == nullptr) {
    return fail();
  }
  const NodeOutput facing_entry = projection_facing_hermite_entry(
      tree, facing_component, facing.component[0], facing.component[1], made, grid_);
  const NodeOutput facing_hermite = projection_facing_hermite(tree, facing_entry, made, grid_);
  if (facing_hermite.node == nullptr) {
    return fail();
  }
  const NodeOutput facing_factor = projection_clip_mix(
      tree, facing_hermite, facing.component[2], made, grid_);
  if (facing_factor.node == nullptr) {
    return fail();
  }

  /* The tiling multiplies the UV only: the frame factor reads q itself, so the footprint stays the
   * frame while the map repeats inside it (the extension is set from UV Wrap). */
  const SeparateOutputs tiling = projection_separate(
      tree, {group_input, tiling_socket}, made, grid_);
  if (tiling.node == nullptr) {
    return fail();
  }
  const auto axis_uv = [&](const int u_index, const int v_index) -> NodeOutput {
    const NodeOutput u_tiled = projection_math(tree,
                                               NODE_MATH_MULTIPLY,
                                               q_components.component[u_index],
                                               tiling.component[0],
                                               0.0f,
                                               made,
                                               grid_);
    const NodeOutput v_tiled = projection_math(tree,
                                               NODE_MATH_MULTIPLY,
                                               q_components.component[v_index],
                                               tiling.component[1],
                                               0.0f,
                                               made,
                                               grid_);
    const NodeOutput u = projection_affine(tree, u_tiled, 0.5f, 0.5f, made, grid_);
    const NodeOutput v = projection_affine(tree, v_tiled, 0.5f, 0.5f, made, grid_);
    return projection_combine_xyz(tree, u, v, 0.0f, made, grid_);
  };

  /* The frame factor of one Planar axis pair: the footprint axes always, the depth slab when its
   * toggle asks for it. The facing fade joins after the clip mix, below. */
  const auto planar_factor = [&](const int axis_index,
                                 const int u_index,
                                 const int v_index) -> NodeOutput {
    const NodeOutput u_factor = projection_axis_factor(
        tree, q_components.component[u_index], made, grid_);
    const NodeOutput v_factor = projection_axis_factor(
        tree, q_components.component[v_index], made, grid_);
    NodeOutput factor = projection_math(
        tree, NODE_MATH_MULTIPLY, u_factor, v_factor, 0.0f, made, grid_);
    const NodeOutput depth_factor = projection_clip_mix(
        tree,
        projection_axis_factor(tree, q_components.component[axis_index], made, grid_),
        {group_input, depth_socket},
        made,
        grid_);
    if (factor.node != nullptr && depth_factor.node != nullptr) {
      factor = projection_math(
          tree, NODE_MATH_MULTIPLY, factor, depth_factor, 0.0f, made, grid_);
    }
    return factor;
  };

  /* The frame-space normal for the Box and Triplanar face weights: the world normal rotated by
   * the frame's rotation. A controller names one object whose whole rotation a live node chain
   * cannot name, so the value sync bakes the world direction; without one the Vector Transform
   * names the shaded object and the inline rotation applies live. */
  NodeOutput frame_normal;
  if (row.projection_object == nullptr) {
    frame_normal = projection_vector_rotate(tree, normal, rotation_value, true, made, grid_);
  }
  else {
    frame_normal = normal;
  }
  if (frame_normal.node == nullptr) {
    return fail();
  }

  switch (space) {
    case MA_PAINT_LAYER_MAPPING_SPACE_PLANAR: {
      /* X: u = y, v = z; Y: u = x, v = z; Z: u = x, v = y. */
      const int u_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
      const int v_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 : 2;
      uv[0] = axis_uv(u_index, v_index);
      inside = planar_factor(axis, u_index, v_index);
      break;
    }
    case MA_PAINT_LAYER_MAPPING_SPACE_BOX:
    case MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR: {
      /* Face 0 is X, 1 is Y, 2 is Z; the weights split the frame-space normal between them. */
      for (int face = 0; face < 3; face++) {
        uv[face] = projection_face_uv(tree, face, q_components.component, made, grid_);
      }
      const NodeOutput blend = {group_input, blend_socket};
      if (!projection_weights_add(tree,
                                  frame_normal,
                                  mode.uses_blend,
                                  blend,
                                  weights,
                                  made,
                                  grid_))
      {
        return fail();
      }
      inside = projection_inside_all(tree, q_components.component, made, grid_);
      break;
    }
    case MA_PAINT_LAYER_MAPPING_SPACE_CUBE: {
      /* The atlas layout is the one table row the CPU bake reads too. */
      const ProjectionCubeLayoutInfo &atlas = BKE_paint_layers_projection_cube_layout_info(
          BKE_paint_layers_projection_cube_layout_get(row));
      NodeOutput local[3];
      for (int face = 0; face < 3; face++) {
        local[face] = projection_face_uv(tree, face, q_components.component, made, grid_);
        if (local[face].node == nullptr) {
          return fail();
        }
      }
      NodeOutput cube_weights[6];
      if (!projection_cube_weights_add(
              tree, q_components.component, cube_weights, made, grid_))
      {
        return fail();
      }
      NodeOutput weighted = {};
      for (int face = 0; face < 6; face++) {
        const SeparateOutputs local_uv = projection_separate(
            tree, local[face / 2], made, grid_);
        if (local_uv.node == nullptr) {
          return fail();
        }
        const NodeOutput u = projection_affine(
            tree, local_uv.component[0], atlas.cell_size[0], atlas.origin[face][0], made, grid_);
        const NodeOutput v = projection_affine(
            tree, local_uv.component[1], atlas.cell_size[1], atlas.origin[face][1], made, grid_);
        const NodeOutput cell = projection_combine_xyz(tree, u, v, 0.0f, made, grid_);
        const NodeOutput scaled = projection_vector_math(
            tree, NODE_VECTOR_MATH_SCALE, cell, cube_weights[face], 1.0f, made, grid_);
        if (scaled.node == nullptr) {
          return fail();
        }
        weighted = (weighted.node == nullptr) ?
                       scaled :
                       projection_vector_math(
                           tree, NODE_VECTOR_MATH_ADD, weighted, scaled, 0.0f, made, grid_);
        if (weighted.node == nullptr) {
          return fail();
        }
      }
      uv[0] = weighted;
      inside = projection_inside_all(tree, q_components.component, made, grid_);
      break;
    }
    case MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER: {
      const NodeOutput y_component = q_components.component[(axis ==
                                                             MA_PAINT_LAYER_PROJECTION_AXIS_Z) ?
                                                                1 :
                                                                (axis ==
                                                                 MA_PAINT_LAYER_PROJECTION_AXIS_X) ?
                                                                2 :
                                                                0];
      const NodeOutput x_component = q_components.component[(axis ==
                                                             MA_PAINT_LAYER_PROJECTION_AXIS_Z) ?
                                                                0 :
                                                                (axis ==
                                                                 MA_PAINT_LAYER_PROJECTION_AXIS_X) ?
                                                                1 :
                                                                2];
      const NodeOutput angle = projection_math(
          tree, NODE_MATH_ARCTAN2, y_component, x_component, 0.0f, made, grid_);
      const NodeOutput u = projection_affine(tree, angle, INV_TWO_PI, 0.5f, made, grid_);
      const NodeOutput height = q_components.component[axis];
      const NodeOutput v = projection_affine(tree, height, 0.5f, 0.5f, made, grid_);
      uv[0] = projection_combine_xyz(tree, u, v, 0.0f, made, grid_);
      const NodeOutput radial_a = projection_math(
          tree, NODE_MATH_MULTIPLY, y_component, y_component, 0.0f, made, grid_);
      const NodeOutput radial_b = projection_math(
          tree, NODE_MATH_MULTIPLY, x_component, x_component, 0.0f, made, grid_);
      const NodeOutput radial = projection_math(
          tree, NODE_MATH_ADD, radial_a, radial_b, 0.0f, made, grid_);
      const NodeOutput radial_inside = projection_math(
          tree, NODE_MATH_LESS_THAN, radial, {}, 1.0f, made, grid_);
      const NodeOutput height_inside = projection_inside_axis(tree, height, made, grid_);
      inside = projection_inside_product(
          tree, radial_inside, height_inside, made, grid_);
      break;
    }
    case MA_PAINT_LAYER_MAPPING_SPACE_SPHERE: {
      const NodeOutput length = projection_vector_length(tree, q, made, grid_);
      const NodeOutput safe_length = projection_math(
          tree, NODE_MATH_MAXIMUM, length, {}, EPSILON, made, grid_);
      const int polar_index = axis;
      const int y_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 :
                          (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 2 :
                                                                       0;
      const int x_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 0 :
                          (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 :
                                                                       2;
      const NodeOutput ratio = projection_math(tree,
                                               NODE_MATH_DIVIDE,
                                               q_components.component[polar_index],
                                               safe_length,
                                               1.0f,
                                               made,
                                               grid_);
      const NodeOutput clamped_low = projection_math(
          tree, NODE_MATH_MAXIMUM, ratio, {}, -1.0f, made, grid_);
      const NodeOutput clamped = projection_math(
          tree, NODE_MATH_MINIMUM, clamped_low, {}, 1.0f, made, grid_);
      const NodeOutput polar = projection_math_unary(
          tree, NODE_MATH_ARCCOSINE, clamped, made, grid_);
      const NodeOutput v = projection_affine(tree, polar, INV_PI, 0.0f, made, grid_);
      const NodeOutput angle = projection_math(tree,
                                               NODE_MATH_ARCTAN2,
                                               q_components.component[y_index],
                                               q_components.component[x_index],
                                               0.0f,
                                               made,
                                               grid_);
      const NodeOutput u = projection_affine(tree, angle, INV_TWO_PI, 0.5f, made, grid_);
      uv[0] = projection_combine_xyz(tree, u, v, 0.0f, made, grid_);
      inside = projection_math(tree, NODE_MATH_LESS_THAN, length, {}, 1.0f, made, grid_);
      break;
    }
    default:
      return fail();
  }

  if (uv[0].node == nullptr || inside.node == nullptr) {
    return fail();
  }

  const NodeOutput clip = {group_input, clip_socket};
  NodeOutput clip_factor = projection_clip_mix(tree, inside, clip, made, grid_);
  /* The facing fade joins after the clip mix: the mirrored copy on the far side and the stretched
   * sweep over faces parallel to the axis are artifacts of the direction, not of the frame's
   * bounds, so they hide even while the map repeats. */
  if (clip_factor.node != nullptr) {
    clip_factor = projection_math(
        tree, NODE_MATH_MULTIPLY, clip_factor, facing_factor, 0.0f, made, grid_);
  }
  if (clip_factor.node == nullptr) {
    return fail();
  }

  ProjectionNodes projection;
  projection.tex_coord = tex_coord;
  projection.position = q.node;
  projection.normal = frame_normal.node;
  projection.samples = mode.samples_num;
  for (int i = 0; i < projection.samples; i++) {
    projection.uv[i] = uv[i].node;
    projection.weight[i] = weights[i].node;
  }
  projection.clip = clip_factor.node;

  for (bNode *node : made) {
    r_nodes.append(node);
  }
  projection_nodes_.add(&row, projection);
  return projection_nodes_.lookup_ptr(&row);
}

bNode *PaintLayersTreeBuilder::projection_uv_mapped(bNodeTree &tree,
                                                    const MaterialPaintLayer &row,
                                                    bNode &uv,
                                                    Vector<bNode *> &r_nodes)
{
  /* A projected row follows only its UV wrap; the 2D adjust would move the lookup away from the
   * frame the gizmo shows, so #BKE_paint_layers_mapping_remaps answers false for it. */
  if (!BKE_paint_layers_mapping_remaps(ma_, row)) {
    return &uv;
  }
  bNode *group_input = projection_group_input(tree);
  bNodeSocket *uv_out = socket_out(uv, "Vector");
  if (group_input == nullptr || uv_out == nullptr) {
    return &uv;
  }
  const auto value_socket =
      [&](Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> &inputs) {
        bNodeTreeInterfaceSocket *const *iface = inputs.lookup_ptr(&row);
        if (iface == nullptr || *iface == nullptr || (*iface)->identifier == nullptr) {
          return static_cast<bNodeSocket *>(nullptr);
        }
        return bke::node_find_socket(
            *group_input, SOCK_OUT, UString::from_ptr_noinline((*iface)->identifier));
      };
  const MappingChain chain = mapping_chain_add(tree,
                                               uv,
                                               *uv_out,
                                               group_input,
                                               value_socket(mapping_offset_inputs_),
                                               value_socket(mapping_scale_inputs_),
                                               value_socket(mapping_rotation_inputs_));
  if (chain.mapping == nullptr) {
    return &uv;
  }
  grid_.place_extra(*chain.mapping);
  grid_.place_extra(*chain.rotate);
  r_nodes.append(chain.mapping);
  r_nodes.append(chain.rotate);
  return chain.mapping;
}

NodeOutput PaintLayersTreeBuilder::projection_clip_factor(
    bNodeTree &tree,
    const MaterialPaintLayer &row,
    Vector<bNode *> &r_nodes)
{
  ProjectionNodes *nodes = projection_nodes_ensure(tree, row, r_nodes);
  if (nodes == nullptr || nodes->clip == nullptr) {
    return {};
  }
  bNodeSocket *out = socket_out(*nodes->clip, "Value");
  if (out == nullptr) {
    return {};
  }
  return {nodes->clip, out};
}

static bool projection_map_is_tiled(const bNode &map)
{
  if (map.id == nullptr || GS(map.id->name) != ID_IM) {
    return false;
  }
  const Image *image = id_cast<const Image *>(map.id);
  return image->source == IMA_SRC_TILED;
}

MapRead PaintLayersTreeBuilder::map_read_apply(bNodeTree &tree,
                                               const MaterialPaintLayer &row,
                                               bNode &map,
                                               Vector<bNode *> &r_nodes)
{
  MapRead read;
  read.node = &map;
  read.color = {&map, socket_out(map, "Color")};
  read.alpha = {&map, socket_out(map, "Alpha")};

  if (!BKE_paint_layers_projection_applies(ma_, row)) {
    read.mapped = this->mapping_apply(tree, row, map, r_nodes);
    return read;
  }

  ProjectionNodes *projection = projection_nodes_ensure(tree, row, r_nodes);
  if (projection == nullptr || projection->uv[0] == nullptr) {
    /* The projection nodes could not be made: fall back to the UV path so the row still shows
     * something rather than disappearing. */
    read.mapped = this->mapping_apply(tree, row, map, r_nodes);
    return read;
  }

  const bool tiled_source = projection_map_is_tiled(map);
  /* Only UV Wrap picks the extension, exactly as the CPU resample does (`wrap != CLIP` repeats),
   * so the graph and the Image Editor agree. The frame's own Limit cuts the footprint through the
   * clip factor, which does not need the image to clip as well; forcing Clip here made Repeat do
   * nothing whenever Limit was on, which every Decal is. A one-axis repeat mode repeats both axes
   * here as it does on the CPU. */
  const bool wrap_clip = BKE_paint_layers_mapping_wrap_get(row) ==
                         MA_PAINT_LAYER_MAPPING_WRAP_CLIP;
  const int image_extension = wrap_clip ? SHD_IMAGE_EXTENSION_CLIP : SHD_IMAGE_EXTENSION_REPEAT;
  const auto set_extension = [&](bNode &texture) {
    if (NodeTexImage *storage = static_cast<NodeTexImage *>(texture.storage)) {
      storage->extension = image_extension;
    }
  };
  const auto tiled_uv = [&](bNode &mapped) -> bNode * {
    if (!tiled_source) {
      return &mapped;
    }
    bNode *origin = projection_tile_origin_ensure(tree, *projection, r_nodes, grid_);
    if (origin == nullptr) {
      return &mapped;
    }
    return projection_tile_local_uv(tree, mapped, *origin, r_nodes, grid_);
  };

  if (projection->samples <= 1) {
    bNode *mapped = tiled_uv(*projection_uv_mapped(tree, row, *projection->uv[0], r_nodes));
    bNodeSocket *mapped_out = socket_out(*mapped, "Vector");
    bNodeSocket *vector = socket_in(map, "Vector");
    if (mapped_out == nullptr || vector == nullptr) {
      read.mapped = this->mapping_apply(tree, row, map, r_nodes);
      return read;
    }
    bke::node_add_link(tree, *mapped, *mapped_out, map, *vector);
    set_extension(map);
    read.projected = true;
    return read;
  }

  /* Box and Triplanar: three map copies, one per axis, blended by the normal weights. */
  Vector<bNode *> made;
  bNode *maps[3] = {&map, nullptr, nullptr};
  for (int face = 0; face < 3; face++) {
    bNode *texture = maps[face];
    if (face > 0) {
      texture = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      if (texture == nullptr) {
        return read;
      }
      texture->id = map.id;
      if (texture->id != nullptr) {
        id_us_plus(texture->id);
      }
      if (NodeTexImage *dst = static_cast<NodeTexImage *>(texture->storage)) {
        const NodeTexImage *src = static_cast<const NodeTexImage *>(map.storage);
        if (src != nullptr) {
          dst->interpolation = src->interpolation;
          dst->extension = src->extension;
          dst->projection = SHD_PROJ_FLAT;
          dst->projection_blend = 0.0f;
        }
      }
      made.append(texture);
      grid_.place_extra(*texture);
      maps[face] = texture;
    }
    set_extension(*texture);
    if (socket_in(*texture, "Vector") == nullptr || socket_out(*texture, "Color") == nullptr ||
        socket_out(*texture, "Alpha") == nullptr)
    {
      for (bNode *node : made) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
      return read;
    }
  }

  /* The weights sum to one; wyz splits Y and Z, and the X weight mixes over the result. */
  bNodeSocket *weight_x = socket_out(*projection->weight[0], "Value");
  bNodeSocket *weight_y = socket_out(*projection->weight[1], "Value");
  bNodeSocket *weight_z = socket_out(*projection->weight[2], "Value");
  if (weight_x == nullptr || weight_y == nullptr || weight_z == nullptr) {
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return read;
  }
  const NodeOutput yz_sum = projection_math(tree,
                                            NODE_MATH_ADD,
                                            {projection->weight[1], weight_y},
                                            {projection->weight[2], weight_z},
                                            0.0f,
                                            made,
                                            grid_);
  const NodeOutput yz_safe = projection_math(
      tree, NODE_MATH_MAXIMUM, yz_sum, {}, EPSILON, made, grid_);
  const NodeOutput y_factor = projection_math(tree,
                                              NODE_MATH_DIVIDE,
                                              {projection->weight[1], weight_y},
                                              yz_safe,
                                              1.0f,
                                              made,
                                              grid_);
  bNode *mix_yz = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
  if (mix_yz == nullptr) {
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return read;
  }
  bNodeSocket *mix_yz_a = socket_in(*mix_yz, "A_Color");
  bNodeSocket *mix_yz_b = socket_in(*mix_yz, "B_Color");
  bNodeSocket *mix_yz_fac = socket_in(*mix_yz, "Factor_Float");
  bNodeSocket *mix_yz_out = socket_out(*mix_yz, "Result_Color");
  if (mix_yz_a == nullptr || mix_yz_b == nullptr || mix_yz_fac == nullptr ||
      mix_yz_out == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *mix_yz, true, false);
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return read;
  }
  bke::node_add_link(tree, *maps[2], *socket_out(*maps[2], "Color"), *mix_yz, *mix_yz_a);
  bke::node_add_link(tree, *maps[1], *socket_out(*maps[1], "Color"), *mix_yz, *mix_yz_b);
  if (y_factor.node != nullptr) {
    bke::node_add_link(tree, *y_factor.node, *y_factor.socket, *mix_yz, *mix_yz_fac);
  }
  made.append(mix_yz);
  grid_.place_extra(*mix_yz);

  bNode *mix_xyz = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
  if (mix_xyz == nullptr) {
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return read;
  }
  bNodeSocket *mix_xyz_a = socket_in(*mix_xyz, "A_Color");
  bNodeSocket *mix_xyz_b = socket_in(*mix_xyz, "B_Color");
  bNodeSocket *mix_xyz_fac = socket_in(*mix_xyz, "Factor_Float");
  bNodeSocket *mix_xyz_out = socket_out(*mix_xyz, "Result_Color");
  if (mix_xyz_a == nullptr || mix_xyz_b == nullptr || mix_xyz_fac == nullptr ||
      mix_xyz_out == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *mix_xyz, true, false);
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return read;
  }
  bke::node_add_link(tree, *mix_yz, *mix_yz_out, *mix_xyz, *mix_xyz_a);
  bke::node_add_link(tree, *maps[0], *socket_out(*maps[0], "Color"), *mix_xyz, *mix_xyz_b);
  bke::node_add_link(tree, *projection->weight[0], *weight_x, *mix_xyz, *mix_xyz_fac);
  made.append(mix_xyz);
  grid_.place_extra(*mix_xyz);

  /* The per-face UVs and the map links are only made once every node exists, so a failure above
   * leaves the row's map untouched. */
  for (int face = 0; face < 3; face++) {
    bNode *mapped_uv = tiled_uv(*projection_uv_mapped(tree, row, *projection->uv[face], r_nodes));
    bNodeSocket *mapped_out = socket_out(*mapped_uv, "Vector");
    bNodeSocket *vector = socket_in(*maps[face], "Vector");
    if (mapped_out == nullptr || vector == nullptr) {
      for (bNode *node : made) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
      return read;
    }
    bke::node_add_link(tree, *mapped_uv, *mapped_out, *maps[face], *vector);
  }

  /* Alpha is the weighted sum: the three map alphas times the same normalized weights. */
  NodeOutput alpha_terms[3];
  for (int face = 0; face < 3; face++) {
    alpha_terms[face] = projection_math(tree,
                                        NODE_MATH_MULTIPLY,
                                        {maps[face], socket_out(*maps[face], "Alpha")},
                                        {projection->weight[face],
                                         socket_out(*projection->weight[face], "Value")},
                                        0.0f,
                                        made,
                                        grid_);
    if (alpha_terms[face].node == nullptr) {
      for (bNode *node : made) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
      return read;
    }
  }
  NodeOutput alpha_sum = projection_math(
      tree, NODE_MATH_ADD, alpha_terms[0], alpha_terms[1], 0.0f, made, grid_);
  alpha_sum = projection_math(
      tree, NODE_MATH_ADD, alpha_sum, alpha_terms[2], 0.0f, made, grid_);
  if (alpha_sum.node == nullptr) {
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return read;
  }

  for (bNode *node : made) {
    r_nodes.append(node);
  }
  read.node = mix_xyz;
  read.color = {mix_xyz, mix_xyz_out};
  read.alpha = alpha_sum;
  read.projected = true;
  return read;
}

}  // namespace blender::bke::paint_layers
