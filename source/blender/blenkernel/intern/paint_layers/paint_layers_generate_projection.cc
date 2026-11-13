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

#include "BLI_index_range.hh"
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

/** `value * mul + add` with all three operands linked: both the tiling multiplier and its
 * center-keeping offset come from the row's tiling value, which no constant-affine node carries. */
NodeOutput projection_multiply_add(bNodeTree &tree,
                                   const NodeOutput value,
                                   const NodeOutput mul,
                                   const NodeOutput add,
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
      value.node == nullptr || value.socket == nullptr || mul.node == nullptr ||
      mul.socket == nullptr || add.node == nullptr || add.socket == nullptr)
  {
    bke::node_remove_node(nullptr, tree, *node, true, false);
    return {};
  }
  bke::node_add_link(tree, *value.node, *value.socket, *node, *in_value);
  bke::node_add_link(tree, *mul.node, *mul.socket, *node, *in_mul);
  bke::node_add_link(tree, *add.node, *add.socket, *node, *in_add);
  made.append(node);
  grid.place_extra(*node);
  return {node, out};
}

/**
 * One face's local UV in [0, 1], tiled around the face center: the map repeats `tiling` times
 * inside the frame's footprint on the face, matching the CPU composite's
 * `0.5 + (uv - 0.5) * tiling`. Face 0 is X, 1 is Y, 2 is Z.
 */
NodeOutput projection_face_uv_tiled(bNodeTree &tree,
                                    const int face,
                                    const NodeOutput q[3],
                                    const SeparateOutputs &tiling,
                                    Vector<bNode *> &made,
                                    layout::LayoutGrid &grid)
{
  const NodeOutput uv = projection_face_uv(tree, face, q, made, grid);
  if (uv.node == nullptr) {
    return {};
  }
  const SeparateOutputs local = projection_separate(tree, uv, made, grid);
  if (local.node == nullptr) {
    return {};
  }
  /* The offset reads `0.5 - 0.5 * tiling`, which keeps the face center fixed while the tiling
   * scales the UV away from it. */
  const NodeOutput offset_u = projection_affine(
      tree, tiling.component[0], -0.5f, 0.5f, made, grid);
  const NodeOutput offset_v = projection_affine(
      tree, tiling.component[1], -0.5f, 0.5f, made, grid);
  if (offset_u.node == nullptr || offset_v.node == nullptr) {
    return {};
  }
  const NodeOutput u = projection_multiply_add(
      tree, local.component[0], tiling.component[0], offset_u, made, grid);
  const NodeOutput v = projection_multiply_add(
      tree, local.component[1], tiling.component[1], offset_v, made, grid);
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

/* -------------------------------------------------------------------- */
/** \name Projection nodes build steps
 *
 * The steps of #PaintLayersTreeBuilder::projection_nodes_ensure, run in the order the nodes are
 * made. A step that returns false leaves its partial nodes in `made`, which the caller removes.
 * \{ */

/** The state one projection build threads through its steps. */
struct ProjectionBuild {
  bNodeTree &tree;
  const MaterialPaintLayer &row;
  const ProjectionSockets &sockets;
  Vector<bNode *> &made;
  layout::LayoutGrid &grid;

  int16_t space = 0;
  int8_t axis = 0;

  bNode *tex_coord = nullptr;
  NodeOutput rotation_value;
  /** The frame-space coordinate and its components. */
  NodeOutput q;
  SeparateOutputs q_components;
  /** The world-space normal and the normal Box and Triplanar weigh. */
  NodeOutput normal;
  NodeOutput frame_normal;
  NodeOutput facing_factor;
  SeparateOutputs tiling;

  NodeOutput uv[3];
  NodeOutput weights[3];
  NodeOutput inside;
  /** Cube: whether to make the picked cell's local UV, for the transform effects' steps. */
  bool cube_local_wanted = false;
  NodeOutput cube_local;
};

/** Add a static node and record it in \a made at once, so a failing step removes it too. */
bNode *projection_node_track(bNodeTree &tree, const int type, Vector<bNode *> &made)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, type);
  if (node != nullptr) {
    made.append(node);
  }
  return node;
}

/** The in-plane axis indices of a Cylinder or Sphere around \a axis: the angle is `atan2(y, x)`. */
void projection_angle_axes(const int8_t axis, int &r_x_index, int &r_y_index)
{
  r_y_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 :
              (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 2 :
                                                           0;
  r_x_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 0 :
              (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 :
                                                           2;
}

/** The controller-space position, as the frame-space coordinate `q`. */
bool projection_frame_space_build(ProjectionBuild &b)
{
  bNode *tex_coord = projection_node_track(b.tree, SH_NODE_TEX_COORD, b.made);
  if (tex_coord == nullptr) {
    return false;
  }
  if (b.row.projection_object != nullptr) {
    tex_coord->id = &b.row.projection_object->id;
    id_us_plus(&b.row.projection_object->id);
  }
  bNodeSocket *object_out = socket_out(*tex_coord, "Object");
  if (object_out == nullptr) {
    return false;
  }
  b.grid.place_extra(*tex_coord);
  b.tex_coord = tex_coord;

  const NodeOutput position = {tex_coord, object_out};
  const NodeOutput center_value = {b.sockets.group_input, b.sockets.center};
  const NodeOutput size_value = {b.sockets.group_input, b.sockets.size};
  b.rotation_value = {b.sockets.group_input, b.sockets.rotation};

  const NodeOutput centered = projection_vector_math(
      b.tree, NODE_VECTOR_MATH_SUBTRACT, position, center_value, 0.0f, b.made, b.grid);
  const NodeOutput rotated_position = projection_vector_rotate(
      b.tree, centered, b.rotation_value, true, b.made, b.grid);
  /* The Size input is the frame's full extent, the extent the cage shows, so the frame-space
   * coordinate reads the offset over the half of it: the axis is inside while
   * |q| < #PAINT_LAYER_PROJECTION_FRAME_EXTENT. The CPU composite divides by the same half
   * (#composite_projection_position). */
  const NodeOutput half_size = projection_vector_math(
      b.tree, NODE_VECTOR_MATH_SCALE, size_value, {}, 0.5f, b.made, b.grid);
  const NodeOutput q_unscaled = projection_vector_math(
      b.tree, NODE_VECTOR_MATH_DIVIDE, rotated_position, half_size, 1.0f, b.made, b.grid);
  /* The image-aspect fit scales the frame's footprint coordinates before anything reads them, so
   * the UV square and the frame factor show the same rectangle. The sync scales the depth axis by
   * one, which leaves the slab untouched. */
  b.q = projection_vector_math(b.tree,
                               NODE_VECTOR_MATH_MULTIPLY,
                               q_unscaled,
                               {b.sockets.group_input, b.sockets.aspect},
                               0.0f,
                               b.made,
                               b.grid);
  if (half_size.node == nullptr || b.q.node == nullptr) {
    return false;
  }
  b.q_components = projection_separate(b.tree, b.q, b.made, b.grid);
  return b.q_components.node != nullptr;
}

/**
 * The world-space normal: the Texture Coordinate "Normal" output is in the shaded object's space,
 * so the facing weight and the Box and Triplanar face weights read the Geometry node instead.
 */
bool projection_world_normal_build(ProjectionBuild &b)
{
  bNode *geometry = projection_node_track(b.tree, SH_NODE_NEW_GEOMETRY, b.made);
  if (geometry == nullptr) {
    return false;
  }
  b.grid.place_extra(*geometry);
  bNodeSocket *world_normal_out = socket_out(*geometry, "Normal");
  if (world_normal_out == nullptr) {
    return false;
  }
  b.normal = {geometry, world_normal_out};
  return true;
}

/**
 * The frame-space component of the surface normal along the axis: a controller names one object,
 * whose whole rotation a live node chain cannot name, so the value sync bakes the world-space
 * direction the world normal reads against; without a controller the Vector Transform node names
 * the shaded object and the inline rotation applies live. Empty node on failure.
 */
NodeOutput projection_facing_component_build(ProjectionBuild &b)
{
  if (b.row.projection_object != nullptr) {
    NodeOutput dot = projection_vector_math(b.tree,
                                            NODE_VECTOR_MATH_DOT_PRODUCT,
                                            b.normal,
                                            {b.sockets.group_input, b.sockets.facing_dir},
                                            0.0f,
                                            b.made,
                                            b.grid);
    if (dot.node != nullptr) {
      bNodeSocket *value_out = socket_out(*dot.node, "Value");
      if (value_out != nullptr) {
        dot.socket = value_out;
      }
    }
    return dot;
  }
  bNode *to_object = projection_node_track(b.tree, SH_NODE_VECT_TRANSFORM, b.made);
  if (to_object == nullptr) {
    return {};
  }
  if (NodeShaderVectTransform *storage = static_cast<NodeShaderVectTransform *>(to_object->storage))
  {
    storage->type = SHD_VECT_TRANSFORM_TYPE_NORMAL;
    storage->convert_from = SHD_VECT_TRANSFORM_SPACE_WORLD;
    storage->convert_to = SHD_VECT_TRANSFORM_SPACE_OBJECT;
  }
  bNodeSocket *vt_in = socket_in(*to_object, "Vector");
  bNodeSocket *vt_out = socket_out(*to_object, "Vector");
  if (vt_in == nullptr || vt_out == nullptr) {
    return {};
  }
  bke::node_add_link(b.tree, *b.normal.node, *b.normal.socket, *to_object, *vt_in);
  b.grid.place_extra(*to_object);
  const NodeOutput n_frame = projection_vector_rotate(
      b.tree, {to_object, vt_out}, b.rotation_value, true, b.made, b.grid);
  const SeparateOutputs n_components = projection_separate(b.tree, n_frame, b.made, b.grid);
  if (n_components.node == nullptr) {
    return {};
  }
  return n_components.component[b.axis];
}

/**
 * The facing fade. The volume kinds receive the map from every direction, so a facing built on one
 * frame axis would blank the faces that axis runs across: the facing fade is a Planar input, and
 * the volume kinds build no facing nodes at all. The CPU composite skips the weight for them too.
 */
bool projection_facing_build(ProjectionBuild &b)
{
  if (BKE_paint_layers_projection_mode_info(b.space).is_volume) {
    return true;
  }
  const NodeOutput facing_component = projection_facing_component_build(b);
  if (facing_component.node == nullptr) {
    return false;
  }
  /* The facing weight from the limit's baked scale and offset: one clamped entry, then the
   * smooth t * t * (3 - 2 t), then the toggle mix. */
  const SeparateOutputs facing = projection_separate(
      b.tree, {b.sockets.group_input, b.sockets.facing}, b.made, b.grid);
  if (facing.node == nullptr) {
    return false;
  }
  const NodeOutput facing_entry = projection_facing_hermite_entry(
      b.tree, facing_component, facing.component[0], facing.component[1], b.made, b.grid);
  const NodeOutput facing_hermite = projection_facing_hermite(
      b.tree, facing_entry, b.made, b.grid);
  if (facing_hermite.node == nullptr) {
    return false;
  }
  b.facing_factor = projection_clip_mix(
      b.tree, facing_hermite, facing.component[2], b.made, b.grid);
  return b.facing_factor.node != nullptr;
}

/**
 * The tiling multiplies the UV only: the frame factor reads q itself, so the footprint stays the
 * frame while the map repeats inside it (the extension is set from UV Wrap).
 */
bool projection_tiling_build(ProjectionBuild &b)
{
  b.tiling = projection_separate(
      b.tree, {b.sockets.group_input, b.sockets.tiling}, b.made, b.grid);
  return b.tiling.node != nullptr;
}

/**
 * The frame-space normal for the Box and Triplanar face weights: the world normal rotated by the
 * frame's rotation. A controller names one object whose whole rotation a live node chain cannot
 * name, so the value sync bakes the world direction; without one the Vector Transform names the
 * shaded object and the inline rotation applies live.
 */
bool projection_frame_normal_build(ProjectionBuild &b)
{
  if (b.row.projection_object == nullptr) {
    b.frame_normal = projection_vector_rotate(
        b.tree, b.normal, b.rotation_value, true, b.made, b.grid);
  }
  else {
    b.frame_normal = b.normal;
  }
  return b.frame_normal.node != nullptr;
}

/** The tiled UV of one Planar axis pair. */
NodeOutput projection_planar_uv(ProjectionBuild &b, const int u_index, const int v_index)
{
  const NodeOutput u_tiled = projection_math(b.tree,
                                             NODE_MATH_MULTIPLY,
                                             b.q_components.component[u_index],
                                             b.tiling.component[0],
                                             0.0f,
                                             b.made,
                                             b.grid);
  const NodeOutput v_tiled = projection_math(b.tree,
                                             NODE_MATH_MULTIPLY,
                                             b.q_components.component[v_index],
                                             b.tiling.component[1],
                                             0.0f,
                                             b.made,
                                             b.grid);
  const NodeOutput u = projection_affine(b.tree, u_tiled, 0.5f, 0.5f, b.made, b.grid);
  const NodeOutput v = projection_affine(b.tree, v_tiled, 0.5f, 0.5f, b.made, b.grid);
  return projection_combine_xyz(b.tree, u, v, 0.0f, b.made, b.grid);
}

/**
 * The frame factor of one Planar axis pair: the footprint axes always, the depth slab when its
 * toggle asks for it. The facing fade joins after the clip mix.
 */
NodeOutput projection_planar_factor(ProjectionBuild &b, const int u_index, const int v_index)
{
  const NodeOutput u_factor = projection_axis_factor(
      b.tree, b.q_components.component[u_index], b.made, b.grid);
  const NodeOutput v_factor = projection_axis_factor(
      b.tree, b.q_components.component[v_index], b.made, b.grid);
  NodeOutput factor = projection_math(
      b.tree, NODE_MATH_MULTIPLY, u_factor, v_factor, 0.0f, b.made, b.grid);
  const NodeOutput depth_factor = projection_clip_mix(
      b.tree,
      projection_axis_factor(b.tree, b.q_components.component[b.axis], b.made, b.grid),
      {b.sockets.group_input, b.sockets.depth},
      b.made,
      b.grid);
  if (factor.node != nullptr && depth_factor.node != nullptr) {
    factor = projection_math(
        b.tree, NODE_MATH_MULTIPLY, factor, depth_factor, 0.0f, b.made, b.grid);
  }
  return factor;
}

bool projection_planar_build(ProjectionBuild &b)
{
  /* X: u = y, v = z; Y: u = x, v = z; Z: u = x, v = y. */
  const int u_index = (b.axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
  const int v_index = (b.axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 : 2;
  b.uv[0] = projection_planar_uv(b, u_index, v_index);
  b.inside = projection_planar_factor(b, u_index, v_index);
  return true;
}

/**
 * Face 0 is X, 1 is Y, 2 is Z; the weights split the frame-space normal between them. The tiling
 * wraps each face's UV around the face center, exactly as the CPU composite does.
 */
bool projection_box_build(ProjectionBuild &b, const bool triplanar)
{
  for (int face = 0; face < 3; face++) {
    b.uv[face] = projection_face_uv_tiled(
        b.tree, face, b.q_components.component, b.tiling, b.made, b.grid);
  }
  const NodeOutput blend = {b.sockets.group_input, b.sockets.blend};
  if (!projection_weights_add(
          b.tree, b.frame_normal, triplanar, blend, b.weights, b.made, b.grid))
  {
    return false;
  }
  b.inside = projection_inside_all(b.tree, b.q_components.component, b.made, b.grid);
  return true;
}

/** The atlas layout is the one table row the CPU bake reads too. */
bool projection_cube_build(ProjectionBuild &b)
{
  const ProjectionCubeLayoutInfo &atlas = BKE_paint_layers_projection_cube_layout_info(
      BKE_paint_layers_projection_cube_layout_get(b.row));
  NodeOutput local[3];
  for (int face = 0; face < 3; face++) {
    local[face] = projection_face_uv(b.tree, face, b.q_components.component, b.made, b.grid);
    if (local[face].node == nullptr) {
      return false;
    }
  }
  NodeOutput cube_weights[6];
  if (!projection_cube_weights_add(b.tree, b.q_components.component, cube_weights, b.made, b.grid))
  {
    return false;
  }
  NodeOutput weighted = {};
  for (int face = 0; face < 6; face++) {
    const SeparateOutputs local_uv = projection_separate(b.tree, local[face / 2], b.made, b.grid);
    if (local_uv.node == nullptr) {
      return false;
    }
    const NodeOutput u = projection_affine(
        b.tree, local_uv.component[0], atlas.cell_size[0], atlas.origin[face][0], b.made, b.grid);
    const NodeOutput v = projection_affine(
        b.tree, local_uv.component[1], atlas.cell_size[1], atlas.origin[face][1], b.made, b.grid);
    const NodeOutput cell = projection_combine_xyz(b.tree, u, v, 0.0f, b.made, b.grid);
    const NodeOutput scaled = projection_vector_math(
        b.tree, NODE_VECTOR_MATH_SCALE, cell, cube_weights[face], 1.0f, b.made, b.grid);
    if (scaled.node == nullptr) {
      return false;
    }
    weighted = (weighted.node == nullptr) ?
                   scaled :
                   projection_vector_math(
                       b.tree, NODE_VECTOR_MATH_ADD, weighted, scaled, 0.0f, b.made, b.grid);
    if (weighted.node == nullptr) {
      return false;
    }
  }
  b.uv[0] = weighted;
  if (b.cube_local_wanted) {
    /* The picked face's local UV: the hard weights make the pair sum of an axis one on exactly that
     * axis, so the sum over the three axes is the local UV of the selected cell. */
    NodeOutput selected = {};
    for (int axis = 0; axis < 3; axis++) {
      const NodeOutput pair = projection_math(b.tree,
                                              NODE_MATH_ADD,
                                              cube_weights[axis * 2],
                                              cube_weights[axis * 2 + 1],
                                              0.0f,
                                              b.made,
                                              b.grid);
      const NodeOutput scaled = projection_vector_math(
          b.tree, NODE_VECTOR_MATH_SCALE, local[axis], pair, 1.0f, b.made, b.grid);
      if (scaled.node == nullptr) {
        return false;
      }
      selected = (selected.node == nullptr) ?
                     scaled :
                     projection_vector_math(
                         b.tree, NODE_VECTOR_MATH_ADD, selected, scaled, 0.0f, b.made, b.grid);
      if (selected.node == nullptr) {
        return false;
      }
    }
    b.cube_local = selected;
  }
  b.inside = projection_inside_all(b.tree, b.q_components.component, b.made, b.grid);
  return true;
}

bool projection_cylinder_build(ProjectionBuild &b)
{
  int x_index;
  int y_index;
  projection_angle_axes(b.axis, x_index, y_index);
  const NodeOutput y_component = b.q_components.component[y_index];
  const NodeOutput x_component = b.q_components.component[x_index];
  const NodeOutput angle = projection_math(
      b.tree, NODE_MATH_ARCTAN2, y_component, x_component, 0.0f, b.made, b.grid);
  const NodeOutput u = projection_affine(b.tree, angle, INV_TWO_PI, 0.5f, b.made, b.grid);
  const NodeOutput height = b.q_components.component[b.axis];
  const NodeOutput v = projection_affine(b.tree, height, 0.5f, 0.5f, b.made, b.grid);
  b.uv[0] = projection_combine_xyz(b.tree, u, v, 0.0f, b.made, b.grid);
  const NodeOutput radial_a = projection_math(
      b.tree, NODE_MATH_MULTIPLY, y_component, y_component, 0.0f, b.made, b.grid);
  const NodeOutput radial_b = projection_math(
      b.tree, NODE_MATH_MULTIPLY, x_component, x_component, 0.0f, b.made, b.grid);
  const NodeOutput radial = projection_math(
      b.tree, NODE_MATH_ADD, radial_a, radial_b, 0.0f, b.made, b.grid);
  const NodeOutput radial_inside = projection_math(
      b.tree, NODE_MATH_LESS_THAN, radial, {}, 1.0f, b.made, b.grid);
  const NodeOutput height_inside = projection_inside_axis(b.tree, height, b.made, b.grid);
  b.inside = projection_inside_product(b.tree, radial_inside, height_inside, b.made, b.grid);
  return true;
}

bool projection_sphere_build(ProjectionBuild &b)
{
  const NodeOutput length = projection_vector_length(b.tree, b.q, b.made, b.grid);
  const NodeOutput safe_length = projection_math(
      b.tree, NODE_MATH_MAXIMUM, length, {}, EPSILON, b.made, b.grid);
  const int polar_index = b.axis;
  int x_index;
  int y_index;
  projection_angle_axes(b.axis, x_index, y_index);
  const NodeOutput ratio = projection_math(b.tree,
                                           NODE_MATH_DIVIDE,
                                           b.q_components.component[polar_index],
                                           safe_length,
                                           1.0f,
                                           b.made,
                                           b.grid);
  const NodeOutput clamped_low = projection_math(
      b.tree, NODE_MATH_MAXIMUM, ratio, {}, -1.0f, b.made, b.grid);
  const NodeOutput clamped = projection_math(
      b.tree, NODE_MATH_MINIMUM, clamped_low, {}, 1.0f, b.made, b.grid);
  const NodeOutput polar = projection_math_unary(
      b.tree, NODE_MATH_ARCCOSINE, clamped, b.made, b.grid);
  const NodeOutput v = projection_affine(b.tree, polar, INV_PI, 0.0f, b.made, b.grid);
  const NodeOutput angle = projection_math(b.tree,
                                           NODE_MATH_ARCTAN2,
                                           b.q_components.component[y_index],
                                           b.q_components.component[x_index],
                                           0.0f,
                                           b.made,
                                           b.grid);
  const NodeOutput u = projection_affine(b.tree, angle, INV_TWO_PI, 0.5f, b.made, b.grid);
  b.uv[0] = projection_combine_xyz(b.tree, u, v, 0.0f, b.made, b.grid);
  b.inside = projection_math(b.tree, NODE_MATH_LESS_THAN, length, {}, 1.0f, b.made, b.grid);
  return true;
}

/** The per-space UVs and the inside factor. */
bool projection_space_build(ProjectionBuild &b)
{
  bool ok = false;
  switch (b.space) {
    case MA_PAINT_LAYER_MAPPING_SPACE_PLANAR:
      ok = projection_planar_build(b);
      break;
    case MA_PAINT_LAYER_MAPPING_SPACE_BOX:
    case MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR:
      ok = projection_box_build(b, BKE_paint_layers_projection_mode_info(b.space).uses_blend);
      break;
    case MA_PAINT_LAYER_MAPPING_SPACE_CUBE:
      ok = projection_cube_build(b);
      break;
    case MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER:
      ok = projection_cylinder_build(b);
      break;
    case MA_PAINT_LAYER_MAPPING_SPACE_SPHERE:
      ok = projection_sphere_build(b);
      break;
    case MA_PAINT_LAYER_MAPPING_SPACE_DECAL:
      /* The graph cannot walk triangles, so it does not read the Decal map: the row's nodes keep
       * Planar's structure (so the topology and the map count stay stable) but the inside factor is
       * zeroed, which makes the row contribute nothing to the graph. Falling back to a UV read of
       * the map would show the content in the wrong place. The Decal itself is laid by the CPU
       * composite and the GPU combined preview, which read the unfolded patch. */
      ok = projection_planar_build(b);
      if (ok) {
        b.inside = projection_math(b.tree, NODE_MATH_MULTIPLY, b.inside, {}, 0.0f, b.made, b.grid);
      }
      break;
    default:
      break;
  }
  return ok && b.uv[0].node != nullptr && b.inside.node != nullptr;
}

/**
 * The frame clip factor: `mix(1, inside, clip)`, then the facing fade. The mirrored copy on the far
 * side and the stretched sweep over faces parallel to the axis are artifacts of the direction, not
 * of the frame's bounds, so the fade hides them even while the map repeats. The volume kinds build
 * no facing, so their frame factor is the clip mix alone.
 */
NodeOutput projection_frame_clip_build(ProjectionBuild &b)
{
  const NodeOutput clip = {b.sockets.group_input, b.sockets.clip};
  NodeOutput clip_factor = projection_clip_mix(b.tree, b.inside, clip, b.made, b.grid);
  if (clip_factor.node != nullptr && b.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL) {
    /* Limit off would let the zeroed inside factor mix back to one; a Decal row stays out of the
     * graph whatever the flags say. */
    clip_factor = projection_math(
        b.tree, NODE_MATH_MULTIPLY, clip_factor, {}, 0.0f, b.made, b.grid);
  }
  if (clip_factor.node != nullptr && b.facing_factor.node != nullptr) {
    clip_factor = projection_math(
        b.tree, NODE_MATH_MULTIPLY, clip_factor, b.facing_factor, 0.0f, b.made, b.grid);
  }
  return clip_factor;
}

/** \} */

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

ProjectionSockets PaintLayersTreeBuilder::projection_sockets_get(bNodeTree &tree,
                                                                 const MaterialPaintLayer &row)
{
  ProjectionSockets sockets;
  sockets.center = projection_input_socket(tree, row, projection_center_inputs_);
  sockets.size = projection_input_socket(tree, row, projection_size_inputs_);
  sockets.rotation = projection_input_socket(tree, row, projection_rotation_inputs_);
  sockets.blend = projection_input_socket(tree, row, projection_blend_inputs_);
  sockets.clip = projection_input_socket(tree, row, projection_clip_inputs_);
  sockets.depth = projection_input_socket(tree, row, projection_depth_inputs_);
  sockets.facing = projection_input_socket(tree, row, projection_facing_inputs_);
  sockets.facing_dir = projection_input_socket(tree, row, projection_facing_dir_inputs_);
  sockets.aspect = projection_input_socket(tree, row, projection_aspect_inputs_);
  sockets.tiling = projection_input_socket(tree, row, projection_tiling_inputs_);
  sockets.group_input = projection_group_input(tree);
  return sockets;
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

  const ProjectionSockets sockets = this->projection_sockets_get(tree, row);
  if (!sockets.valid()) {
    return nullptr;
  }

  Vector<bNode *> made;
  ProjectionBuild build{tree, row, sockets, made, grid_};
  build.space = row.mapping.space;
  build.axis = BKE_paint_layers_projection_axis_get(row);
  build.cube_local_wanted = build.space == MA_PAINT_LAYER_MAPPING_SPACE_CUBE &&
                            !BKE_paint_layers_active_uv_transform_effects(ma_, row).is_empty();

  /* The order is the node creation order of the graph: the frame-space position, the world normal,
   * the facing fade, the tiling, the frame normal, then the per-space UVs. */
  const bool built = projection_frame_space_build(build) && projection_world_normal_build(build) &&
                     projection_facing_build(build) && projection_tiling_build(build) &&
                     projection_frame_normal_build(build) && projection_space_build(build);
  const NodeOutput clip_factor = built ? projection_frame_clip_build(build) : NodeOutput();
  if (clip_factor.node == nullptr) {
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return nullptr;
  }

  ProjectionNodes projection;
  projection.tex_coord = build.tex_coord;
  projection.position = build.q.node;
  projection.normal = build.frame_normal.node;
  projection.cube_local = build.cube_local.node;
  projection.samples = BKE_paint_layers_projection_mode_info(build.space).samples_num;
  for (int i = 0; i < projection.samples; i++) {
    projection.uv[i] = build.uv[i].node;
    projection.weight[i] = build.weights[i].node;
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
  /* The row's own 2D adjust, the CPU's #composite_projection_uv_wrap: it follows the effects' steps
   * (#projection_steps_apply). A row without a remapping mapping follows only its UV wrap. */
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

/** The Group Input socket of \a effect's value input in \a inputs, or null. */
static bNodeSocket *projection_effect_value_socket(
    bNode *group_input,
    const Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> &inputs,
    const MaterialPaintLayer &effect)
{
  if (group_input == nullptr) {
    return nullptr;
  }
  bNodeTreeInterfaceSocket *const *iface = inputs.lookup_ptr(&effect);
  if (iface == nullptr || *iface == nullptr || (*iface)->identifier == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline((*iface)->identifier));
}

PaintLayersTreeBuilder::ProjectionSteps PaintLayersTreeBuilder::projection_steps_apply(
    bNodeTree &tree, const MaterialPaintLayer &row, bNode &uv, Vector<bNode *> &r_nodes)
{
  ProjectionSteps result;
  result.uv_node = &uv;
  /* Cube reads atlas cells, so the steps run on the picked cell's local UV (#ProjectionNodes::
   * cube_local) and the result returns to the atlas below, the CPU's per-tap local-cell handling. */
  const bool is_cube = row.mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_CUBE;
  bNode *cube_local = nullptr;
  if (is_cube) {
    const ProjectionNodes *cube_nodes = projection_nodes_.lookup_ptr(&row);
    cube_local = (cube_nodes != nullptr) ? cube_nodes->cube_local : nullptr;
    if (cube_local == nullptr) {
      return result;
    }
  }
  const Vector<const MaterialPaintLayer *, 4> effects =
      BKE_paint_layers_active_uv_transform_effects(ma_, row);
  bNode *group_input = projection_group_input(tree);
  bNode &start = is_cube ? *cube_local : uv;
  bNodeSocket *uv_out = socket_out(start, "Vector");
  if (effects.is_empty() || group_input == nullptr || uv_out == nullptr) {
    return result;
  }
  NodeOutput source = {&start, uv_out};
  for (const MaterialPaintLayer *effect : effects) {
    bNodeSocket *center_socket = projection_effect_value_socket(
        group_input, mapping_mirror_center_inputs_, *effect);
    const int16_t mirror_axes = (center_socket != nullptr) ?
                                    BKE_paint_layers_mapping_mirror_get(*effect) :
                                    int16_t(0);
    const MappingChain chain = mapping_chain_add(
        tree,
        *source.node,
        *source.socket,
        group_input,
        projection_effect_value_socket(group_input, mapping_offset_inputs_, *effect),
        projection_effect_value_socket(group_input, mapping_scale_inputs_, *effect),
        projection_effect_value_socket(group_input, mapping_rotation_inputs_, *effect),
        group_input,
        center_socket,
        mirror_axes);
    bNodeSocket *chain_out = (chain.mapping != nullptr) ? socket_out(*chain.mapping, "Vector") :
                                                           nullptr;
    if (chain_out == nullptr) {
      continue;
    }
    grid_.place_extra(*chain.mapping);
    grid_.place_extra(*chain.rotate);
    for (bNode *fold : chain.fold_nodes) {
      grid_.place_extra(*fold);
      r_nodes.append(fold);
    }
    r_nodes.append(chain.mapping);
    r_nodes.append(chain.rotate);
    source = {chain.mapping, chain_out};

    /* The step clips on its own output, the coordinate the next step (or the row's adjust) reads. */
    const NodeOutput inside = this->mapping_clip_inside_add(tree, *effect, source, r_nodes);
    if (inside.node != nullptr) {
      result.clip = (result.clip.node != nullptr) ?
                        projection_math(
                            tree, NODE_MATH_MULTIPLY, result.clip, inside, 0.0f, r_nodes, grid_) :
                        inside;
    }
    ProjectionSteps::Step step;
    step.effect = effect;
    if (chain.side_sign != nullptr) {
      step.side_sign = {chain.side_sign_node, chain.side_sign};
    }
    result.steps.append(step);
  }
  if (is_cube && source.node != cube_local) {
    /* Back to the atlas: `atlas + (stepped - local) * cell`, per component, where `atlas` is the
     * un-stepped atlas UV the row would have read. This is the CPU's `origin + stepped * cell`
     * (the atlas UV is `origin + local * cell`), and the row's UV adjust follows it as before. */
    const ProjectionCubeLayoutInfo &layout_info = BKE_paint_layers_projection_cube_layout_info(
        BKE_paint_layers_projection_cube_layout_get(row));
    const SeparateOutputs atlas_parts = projection_separate(
        tree, {&uv, socket_out(uv, "Vector")}, r_nodes, grid_);
    const SeparateOutputs local_parts = projection_separate(
        tree, {cube_local, socket_out(*cube_local, "Vector")}, r_nodes, grid_);
    const SeparateOutputs stepped_parts = projection_separate(tree, source, r_nodes, grid_);
    NodeOutput atlas_stepped[2];
    for (int c = 0; c < 2; c++) {
      NodeOutput moved;
      if (atlas_parts.node != nullptr && local_parts.node != nullptr &&
          stepped_parts.node != nullptr)
      {
        const NodeOutput delta = projection_math(tree,
                                                 NODE_MATH_SUBTRACT,
                                                 stepped_parts.component[c],
                                                 local_parts.component[c],
                                                 0.0f,
                                                 r_nodes,
                                                 grid_);
        const NodeOutput scaled = projection_affine(
            tree, delta, layout_info.cell_size[c], 0.0f, r_nodes, grid_);
        moved = projection_math(
            tree, NODE_MATH_ADD, atlas_parts.component[c], scaled, 0.0f, r_nodes, grid_);
      }
      if (moved.node == nullptr) {
        /* The atlas could not be rebuilt: the steps are left out, as the Cube read was before. */
        result.uv_node = &uv;
        result.clip = {};
        result.steps.clear();
        return result;
      }
      atlas_stepped[c] = moved;
    }
    const NodeOutput atlas = projection_combine_xyz(
        tree, atlas_stepped[0], atlas_stepped[1], 0.0f, r_nodes, grid_);
    if (atlas.node == nullptr) {
      result.uv_node = &uv;
      result.clip = {};
      result.steps.clear();
      return result;
    }
    result.uv_node = atlas.node;
    return result;
  }
  result.uv_node = source.node;
  return result;
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
                                               Vector<bNode *> &r_nodes,
                                               const int channel)
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

  /* The transform effects' covariant counterpart on a sampled tangent normal, the CPU's
   * #CompositeMappingChain::remap_tangent_normal: the steps' inverses in reverse list order, before
   * the frame-to-tangent conversion. */
  bNode *steps_group_input = projection_group_input(tree);
  const auto steps_normal_remap = [&](NodeOutput color,
                                      const ProjectionSteps &steps,
                                      Vector<bNode *> &made) -> NodeOutput {
    for (const int index : steps.steps.index_range()) {
      const ProjectionSteps::Step &step = steps.steps[steps.steps.size() - 1 - index];
      if (color.node == nullptr || color.socket == nullptr) {
        break;
      }
      bNode *encode = nullptr;
      bNodeSocket *encoded = normal_remap_nodes_add(
          tree,
          *color.node,
          *color.socket,
          steps_group_input,
          projection_effect_value_socket(steps_group_input, mapping_rotation_inputs_, *step.effect),
          steps_group_input,
          projection_effect_value_socket(steps_group_input, mapping_scale_inputs_, *step.effect),
          step.side_sign.node,
          step.side_sign.socket,
          made,
          encode);
      if (encoded != nullptr) {
        color = {encode, encoded};
      }
    }
    return color;
  };
  /* A step's wrap-Clip takes the part outside its coordinate's unit square out of the alpha. */
  const auto steps_clip_alpha = [&](const NodeOutput alpha,
                                    const ProjectionSteps &steps,
                                    Vector<bNode *> &made) -> NodeOutput {
    if (steps.clip.node == nullptr || alpha.node == nullptr) {
      return alpha;
    }
    const NodeOutput clipped = projection_math(
        tree, NODE_MATH_MULTIPLY, alpha, steps.clip, 0.0f, made, grid_);
    return (clipped.node != nullptr) ? clipped : alpha;
  };

  if (projection->samples <= 1) {
    const ProjectionSteps steps = this->projection_steps_apply(
        tree, row, *projection->uv[0], r_nodes);
    bNode *mapped = tiled_uv(*projection_uv_mapped(tree, row, *steps.uv_node, r_nodes));
    bNodeSocket *mapped_out = socket_out(*mapped, "Vector");
    bNodeSocket *vector = socket_in(map, "Vector");
    if (mapped_out == nullptr || vector == nullptr) {
      read.mapped = this->mapping_apply(tree, row, map, r_nodes);
      return read;
    }
    bke::node_add_link(tree, *mapped, *mapped_out, map, *vector);
    set_extension(map);
    read.projected = true;
    read.alpha = steps_clip_alpha(read.alpha, steps, r_nodes);
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
        row.mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_PLANAR && !steps.steps.is_empty())
    {
      /* Cylinder and Sphere have no frame conversion; the steps still turn the sampled vectors. */
      Vector<bNode *> made;
      const NodeOutput remapped = steps_normal_remap(read.color, steps, made);
      for (bNode *node : made) {
        grid_.place_extra(*node);
        r_nodes.append(node);
      }
      read.color = remapped;
    }
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
        row.mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_PLANAR)
    {
      /* The sampled vectors read in the frame's own basis: re-express them in the surface tangent
       * basis, the one conversion the CPU bake runs (see
       * #BKE_paint_layers_projection_normal_frame_to_tangent). The record's own convention is read
       * here rather than after the conversion, which a caller would apply too late for. */
      bool directx = false;
      for (const int i : IndexRange(row.channels_num)) {
        if (row.channels[i].channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
          directx = (row.channels[i].flag & MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX) != 0;
          break;
        }
      }
      Vector<bNode *> made;
      bNode *encode = nullptr;
      NodeOutput color_in = {&map, socket_out(map, "Color")};
      if (directx && color_in.socket != nullptr) {
        color_in = this->normal_green_flip_nodes_add(tree, color_in, made);
      }
      if (color_in.socket == nullptr) {
        return read;
      }
      color_in = steps_normal_remap(color_in, steps, made);
      const ProjectionSockets sockets = this->projection_sockets_get(tree, row);
      bNodeSocket *converted = normal_frame_tangent_nodes_add(
          tree, *color_in.node, *color_in.socket, sockets.group_input, sockets.rotation, made, encode);
      if (converted != nullptr) {
        for (bNode *node : made) {
          grid_.place_extra(*node);
          r_nodes.append(node);
        }
        read.color = {encode, converted};
      }
    }
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
  bke::node_add_link(tree, *projection->weight[0], *weight_x, *mix_xyz, *mix_xyz_fac);
  made.append(mix_xyz);
  grid_.place_extra(*mix_xyz);

  /* The per-face UVs and the map links are only made once every node exists, so a failure above
   * leaves the row's map untouched. */
  /* Every face takes the same steps over its own UV, before the blend by the normal weights, as the
   * CPU applies them per face tap. A face's clip rides its alpha and its normal remap its color,
   * the weights then mix both. */
  NodeOutput face_color[3];
  NodeOutput face_alpha[3];
  for (int face = 0; face < 3; face++) {
    const ProjectionSteps steps = this->projection_steps_apply(
        tree, row, *projection->uv[face], r_nodes);
    bNode *mapped_uv = tiled_uv(*projection_uv_mapped(tree, row, *steps.uv_node, r_nodes));
    bNodeSocket *mapped_out = socket_out(*mapped_uv, "Vector");
    bNodeSocket *vector = socket_in(*maps[face], "Vector");
    if (mapped_out == nullptr || vector == nullptr) {
      for (bNode *node : made) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
      return read;
    }
    bke::node_add_link(tree, *mapped_uv, *mapped_out, *maps[face], *vector);
    face_color[face] = {maps[face], socket_out(*maps[face], "Color")};
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && !steps.steps.is_empty()) {
      const int remap_start = int(made.size());
      face_color[face] = steps_normal_remap(face_color[face], steps, made);
      for (const int i : IndexRange(remap_start, int(made.size()) - remap_start)) {
        grid_.place_extra(*made[i]);
      }
    }
    face_alpha[face] = steps_clip_alpha(
        {maps[face], socket_out(*maps[face], "Alpha")}, steps, made);
  }
  bke::node_add_link(tree, *face_color[2].node, *face_color[2].socket, *mix_yz, *mix_yz_a);
  bke::node_add_link(tree, *face_color[1].node, *face_color[1].socket, *mix_yz, *mix_yz_b);
  bke::node_add_link(tree, *face_color[0].node, *face_color[0].socket, *mix_xyz, *mix_xyz_b);

  /* Alpha is the weighted sum: the three map alphas times the same normalized weights. */
  NodeOutput alpha_terms[3];
  for (int face = 0; face < 3; face++) {
    alpha_terms[face] = projection_math(tree,
                                        NODE_MATH_MULTIPLY,
                                        face_alpha[face],
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
