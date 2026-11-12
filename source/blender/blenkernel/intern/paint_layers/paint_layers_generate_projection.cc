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

constexpr float INV_TWO_PI = 0.15915494309189535f;
constexpr float INV_PI = 0.3183098861837907f;
constexpr float EPSILON = 1e-6f;

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
 * #BKE_paint_layers_projection_axis_factor, so the frame factor has no hard seam on either side.
 */
NodeOutput projection_axis_factor(bNodeTree &tree,
                                  const NodeOutput value,
                                  Vector<bNode *> &made,
                                  layout::LayoutGrid &grid)
{
  const NodeOutput depth = projection_axis_depth(tree, value, made, grid);
  return projection_affine(tree, depth, 1.0f / PAINT_LAYER_PROJECTION_EDGE_SOFT, 0.0f, made, grid);
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
  bNodeSocket *clip_socket = projection_input_socket(tree, row, projection_clip_inputs_);
  bNodeSocket *depth_socket = projection_input_socket(tree, row, projection_depth_inputs_);
  bNodeSocket *facing_socket = projection_input_socket(tree, row, projection_facing_inputs_);
  bNodeSocket *facing_dir_socket = projection_input_socket(
      tree, row, projection_facing_dir_inputs_);
  bNodeSocket *aspect_socket = projection_input_socket(tree, row, projection_aspect_inputs_);
  bNode *group_input = projection_group_input(tree);
  if (center_socket == nullptr || size_socket == nullptr || rotation_socket == nullptr ||
      clip_socket == nullptr || depth_socket == nullptr || facing_socket == nullptr ||
      facing_dir_socket == nullptr || aspect_socket == nullptr || group_input == nullptr)
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
  const NodeOutput q_unscaled = projection_vector_math(
      tree, NODE_VECTOR_MATH_DIVIDE, rotated_position, size_value, 1.0f, made, grid_);
  /* The image-aspect fit scales the frame's footprint coordinates before anything reads them, so
   * the UV square and the frame factor show the same rectangle. The sync scales the depth axis by
   * one, which leaves the slab untouched. */
  const NodeOutput q = projection_vector_math(
      tree, NODE_VECTOR_MATH_MULTIPLY, q_unscaled, {group_input, aspect_socket}, 0.0f, made, grid_);
  if (q.node == nullptr) {
    return fail();
  }
  const SeparateOutputs q_components = projection_separate(tree, q, made, grid_);
  if (q_components.node == nullptr) {
    return fail();
  }

  const int16_t space = row.mapping.space;
  const int8_t axis = BKE_paint_layers_projection_axis_get(row);
  NodeOutput uv;
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

  const auto axis_uv = [&](const int u_index, const int v_index) -> NodeOutput {
    const NodeOutput u = projection_affine(
        tree, q_components.component[u_index], 0.5f, 0.5f, made, grid_);
    const NodeOutput v = projection_affine(
        tree, q_components.component[v_index], 0.5f, 0.5f, made, grid_);
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

  switch (space) {
    case MA_PAINT_LAYER_MAPPING_SPACE_PLANAR: {
      /* X: u = y, v = z; Y: u = x, v = z; Z: u = x, v = y. */
      const int u_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
      const int v_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 : 2;
      uv = axis_uv(u_index, v_index);
      inside = planar_factor(axis, u_index, v_index);
      break;
    }
    default:
      return fail();
  }

  if (uv.node == nullptr || inside.node == nullptr) {
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
  projection.uv = uv.node;
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
  if (!BKE_paint_layers_mapping_applies(ma_, row)) {
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
  if (projection == nullptr || projection->uv == nullptr) {
    /* The projection nodes could not be made: fall back to the UV path so the row still shows
     * something rather than disappearing. */
    read.mapped = this->mapping_apply(tree, row, map, r_nodes);
    return read;
  }

  const bool tiled_source = projection_map_is_tiled(map);
  /* A clipped frame must not repeat its map past the frame edge either: Ucupaint's decal keeps its
   * source on Clip for the same reason, and the factor's smooth edge then meets a clipped image
   * instead of a tiled one. */
  const bool wrap_clip = BKE_paint_layers_mapping_wrap_get(row) ==
                             MA_PAINT_LAYER_MAPPING_WRAP_CLIP ||
                         BKE_paint_layers_projection_clip_get(row);
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

  bNode *mapped = tiled_uv(*projection_uv_mapped(tree, row, *projection->uv, r_nodes));
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

}  // namespace blender::bke::paint_layers
