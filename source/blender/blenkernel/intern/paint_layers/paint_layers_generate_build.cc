/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The root of the generated tree: the interface, one group per row, and the chain of those groups
 * for every channel the description wires. The nodes inside a row's group are built in
 * `paint_layers_generate_build_chain.cc`.
 */

#include "BKE_paint_layers_generate.hh"

#include <array>
#include <atomic>
#include <utility>

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_material_composite.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "paint_layers_generate_build_intern.hh"
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {
namespace bke::paint_layers {

void refresh_layer_group(LayerGroup &group)
{
  if (group.tree == nullptr) {
    return;
  }
  if (group.group_input != nullptr) {
    nodes::update_node_declaration_and_sockets(*group.tree, *group.group_input);
  }
  if (group.group_output != nullptr) {
    nodes::update_node_declaration_and_sockets(*group.tree, *group.group_output);
  }
  if (group.instance != nullptr && group.parent_tree != nullptr) {
    nodes::update_node_declaration_and_sockets(*group.parent_tree, *group.instance);
  }
}

bNodeTreeInterfaceSocket *layer_group_socket_find(const LayerGroup &group,
                                                  const char *name,
                                                  const StringRef socket_type,
                                                  const bool input)
{
  if (group.tree == nullptr || name == nullptr) {
    return nullptr;
  }
  bNodeTreeInterfaceSocket *found = nullptr;
  group.tree->tree_interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
    if (item.item_type != NodeTreeInterfaceItemType::Socket) {
      return true;
    }
    const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
    if (((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0) != input) {
      return true;
    }
    if (socket.name != nullptr && STREQ(socket.name, name) && socket.socket_type != nullptr &&
        StringRef(socket.socket_type) == socket_type)
    {
      found = const_cast<bNodeTreeInterfaceSocket *>(&socket);
      return false;
    }
    return true;
  });
  return found;
}

bNodeTreeInterfaceSocket *layer_group_add_socket(LayerGroup &group,
                                                 const char *base,
                                                 const StringRef socket_type,
                                                 const NodeTreeInterfaceSocketFlag flag)
{
  /* Find before adding: a group whose interface was kept across a rebuild hands the same socket
   * back, so re-running the build never grows the interface with a duplicate. */
  if (bNodeTreeInterfaceSocket *existing = layer_group_socket_find(
          group, base, socket_type, (flag & NODE_INTERFACE_SOCKET_INPUT) != 0))
  {
    return existing;
  }
  char name[256];
  interface_name_unique(group.tree->tree_interface, base, name, sizeof(name));
  bNodeTreeInterfaceSocket *socket = group.tree->tree_interface.add_socket(
      name, "", socket_type, flag, nullptr);
  if (socket != nullptr) {
    refresh_layer_group(group);
  }
  return socket;
}

bNodeTreeInterfaceSocket *layer_group_value_input(LayerGroup &group,
                                                  const char *base,
                                                  const StringRef socket_type,
                                                  const char *role,
                                                  const bUUID &marker,
                                                  const int channel)
{
  bNodeTreeInterfaceSocket *socket = layer_group_add_socket(
      group, base, socket_type, NODE_INTERFACE_SOCKET_INPUT);
  if (socket == nullptr) {
    return nullptr;
  }
  uid_prop_set(socket->properties, INPUT_MARKER_PROP, marker);
  prop_string_set(socket->properties, INPUT_ROLE_PROP, role);
  prop_int_set(socket->properties, INPUT_CHANNEL_PROP, channel);
  return socket;
}

bNodeSocket *PaintLayersTreeBuilder::group_input_socket(
    const LayerGroup &group, const bNodeTreeInterfaceSocket &iface) const
{
  if (group.group_input == nullptr || iface.identifier == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      *group.group_input, SOCK_OUT, UString::from_ptr_noinline(iface.identifier));
}

/** Write a default into \a iface and into the instance socket that was built from it. */
template<typename SocketValueT, typename WriteFn>
static void group_input_default_write(const LayerGroup &group,
                                      bNodeTreeInterfaceSocket &iface,
                                      const WriteFn &write)
{
  if (iface.socket_data != nullptr) {
    write(*static_cast<SocketValueT *>(iface.socket_data));
  }
  /* The instance was declared before the value was known, and an unlinked group input reads the
   * default of the instance's own socket. */
  if (group.instance != nullptr && iface.identifier != nullptr) {
    bNodeSocket *socket = bke::node_find_socket(
        *group.instance, SOCK_IN, UString::from_ptr_noinline(iface.identifier));
    if (socket != nullptr && socket->default_value != nullptr) {
      write(*static_cast<SocketValueT *>(socket->default_value));
    }
  }
}

void group_input_default_set(const LayerGroup &group,
                             bNodeTreeInterfaceSocket &iface,
                             const float value)
{
  group_input_default_write<bNodeSocketValueFloat>(
      group, iface, [&](bNodeSocketValueFloat &socket_value) { socket_value.value = value; });
}

void group_input_default_set(const LayerGroup &group,
                             bNodeTreeInterfaceSocket &iface,
                             const float value[4])
{
  group_input_default_write<bNodeSocketValueRGBA>(
      group, iface, [&](bNodeSocketValueRGBA &socket_value) {
        copy_v4_v4(socket_value.value, value);
      });
}

void group_input_vector_default_set(const LayerGroup &group,
                                    bNodeTreeInterfaceSocket &iface,
                                    const float value[3])
{
  group_input_default_write<bNodeSocketValueVector>(
      group, iface, [&](bNodeSocketValueVector &socket_value) {
        copy_v3_v3(socket_value.value, value);
      });
}

void PaintLayersTreeBuilder::mapping_inputs_create(LayerGroup &group,
                                                   const MaterialPaintLayer &row,
                                                   const char *label)
{
  /* The one predicate the topology hash builds by, so the interface and the hash agree. */
  if (!BKE_paint_layers_mapping_remaps(ma_, row)) {
    return;
  }
  struct MappingInput {
    const char *suffix;
    const char *role;
    float value[3];
    Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> &store;
  };
  /* The mapping chain reads three Vectors: the offset in x, y; the scale in x, y; the angle in
   * z (fed to the Vector Rotate in front of the Mapping node). */
  const MappingInput inputs[3] = {
      {"Offset",
       ROLE_MAPPING_OFFSET,
       {row.mapping.offset[0], row.mapping.offset[1], 0.0f},
       mapping_offset_inputs_},
      {"Scale",
       ROLE_MAPPING_SCALE,
       {BKE_paint_layers_mapping_scale_normalize(row.mapping.scale[0]),
        BKE_paint_layers_mapping_scale_normalize(row.mapping.scale[1]),
        1.0f},
       mapping_scale_inputs_},
      {"Rotation",
       ROLE_MAPPING_ROTATION,
       {0.0f, 0.0f, row.mapping.rotation},
       mapping_rotation_inputs_},
  };
  for (const MappingInput &input : inputs) {
    char base[200];
    SNPRINTF(base, "%s Mapping %s", label, input.suffix);
    bNodeTreeInterfaceSocket *socket = layer_group_value_input(
        group, base, "NodeSocketVector", input.role, row.marker, -1);
    if (socket != nullptr) {
      group_input_vector_default_set(group, *socket, input.value);
      input.store.add(&row, socket);
    }
  }
}

bNode *PaintLayersTreeBuilder::uv_coordinate_ensure(bNodeTree &tree)
{
  /* One source per tree: every Mapping of the group reads the same `UV` output. */
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_TEX_COORD) {
      return &node;
    }
  }
  bNode *coord = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_COORD);
  if (coord != nullptr) {
    grid_.place_extra(*coord);
  }
  return coord;
}

bNodeSocket *PaintLayersTreeBuilder::gradient_coordinate_socket(
    bNode &coord, const MaterialPaintLayerGradient &gradient)
{
  switch (eMaterialPaintLayerGradientSpace(gradient.space)) {
    case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
      return socket_out(coord, "Object");
    case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
      return socket_out(coord, "Generated");
    case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
      break;
  }
  /* An unknown space is the safe UV default, not a discarded row. */
  return socket_out(coord, "UV");
}

/** The socket of \a group_input that stands for \a iface, or null. */
static bNodeSocket *mapping_group_input_socket(bNode *group_input,
                                               const bNodeTreeInterfaceSocket *const *iface)
{
  if (group_input == nullptr || iface == nullptr || *iface == nullptr ||
      (*iface)->identifier == nullptr)
  {
    return nullptr;
  }
  return bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline((*iface)->identifier));
}

bNode *PaintLayersTreeBuilder::mapping_node_ensure(bNodeTree &tree,
                                                   const MaterialPaintLayer &row,
                                                   Vector<bNode *> &r_nodes)
{
  if (!BKE_paint_layers_mapping_remaps(ma_, row)) {
    return nullptr;
  }
  if (bNode **found = mapping_nodes_.lookup_ptr(&row)) {
    return *found;
  }
  bNode *coord = this->uv_coordinate_ensure(tree);
  bNodeSocket *uv = (coord != nullptr) ? socket_out(*coord, "UV") : nullptr;
  if (uv == nullptr) {
    return nullptr;
  }
  /* The values ride the row group's inputs; an input missing here leaves the socket at its
   * identity default instead. */
  bNode *group_input = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.is_group_input()) {
      group_input = &node;
      break;
    }
  }
  const auto value_socket =
      [&](Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> &inputs) {
        bNodeTreeInterfaceSocket *const *iface = inputs.lookup_ptr(&row);
        return mapping_group_input_socket(group_input, iface);
      };
  const MappingChain chain = mapping_chain_add(tree,
                                               *coord,
                                               *uv,
                                               group_input,
                                               value_socket(mapping_offset_inputs_),
                                               value_socket(mapping_scale_inputs_),
                                               value_socket(mapping_rotation_inputs_));
  if (chain.mapping == nullptr) {
    return nullptr;
  }
  grid_.place_extra(*chain.mapping);
  grid_.place_extra(*chain.rotate);
  mapping_nodes_.add(&row, chain.mapping);
  r_nodes.append(chain.mapping);
  r_nodes.append(chain.rotate);
  return chain.mapping;
}

bool PaintLayersTreeBuilder::mapping_apply(bNodeTree &tree,
                                           const MaterialPaintLayer &row,
                                           bNode &map,
                                           Vector<bNode *> &r_nodes)
{
  if (!BKE_paint_layers_mapping_remaps(ma_, row)) {
    return false;
  }
  bNodeSocket *vector = socket_in(map, "Vector");
  if (vector == nullptr) {
    return false;
  }
  bNode *mapping = this->mapping_node_ensure(tree, row, r_nodes);
  if (mapping == nullptr) {
    return false;
  }
  bNodeSocket *mapping_out = socket_out(*mapping, "Vector");
  if (mapping_out == nullptr) {
    return false;
  }
  if (NodeTexImage *storage = static_cast<NodeTexImage *>(map.storage)) {
    storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
  }
  bke::node_add_link(tree, *mapping, *mapping_out, map, *vector);
  return true;
}

/** A Math node of \a operation placed on the grid; null when it cannot be made. */
static bNode *clip_math_add(bNodeTree &tree, layout::LayoutGrid &grid, const int operation)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (node != nullptr) {
    node->custom1 = operation;
    grid.place_extra(*node);
  }
  return node;
}

NodeOutput PaintLayersTreeBuilder::mapping_clip_factor(bNodeTree &tree,
                                                       const MaterialPaintLayer &row,
                                                       Vector<bNode *> &r_nodes,
                                                       const bool create_mapping)
{
  bool repeat_u = true;
  bool repeat_v = true;
  BKE_paint_layers_mapping_wrap_axes(BKE_paint_layers_mapping_wrap_get(row), repeat_u, repeat_v);
  if ((repeat_u && repeat_v) || !BKE_paint_layers_mapping_remaps(ma_, row)) {
    return {};
  }
  if (const NodeOutput *cached = mapping_clip_masks_.lookup_ptr(&row)) {
    return *cached;
  }

  /* An axis is inside the unit square exactly when `floor(a) == 0`; Compare with an epsilon of
   * one half reads that without a pair of range tests, the CPU's `0 <= a < 1`. Everything made
   * is kept apart until the factor is complete, so a failure leaves nothing behind. */
  Vector<bNode *> made;
  const auto fail = [&]() {
    for (bNode *node : made) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
    return NodeOutput{};
  };
  bNode *mapping = nullptr;
  if (bNode **found = mapping_nodes_.lookup_ptr(&row)) {
    mapping = *found;
  }
  else if (create_mapping) {
    mapping = this->mapping_node_ensure(tree, row, r_nodes);
  }
  if (mapping == nullptr) {
    return {};
  }
  bNodeSocket *mapping_out = socket_out(*mapping, "Vector");
  bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
  if (separate != nullptr) {
    made.append(separate);
  }
  bNodeSocket *separate_in = (separate != nullptr) ? socket_in(*separate, "Vector") : nullptr;
  if (mapping_out == nullptr || separate_in == nullptr) {
    return fail();
  }
  grid_.place_extra(*separate);
  bke::node_add_link(tree, *mapping, *mapping_out, *separate, *separate_in);
  const bool clipped[2] = {!repeat_u, !repeat_v};
  const char *axis_names[2] = {"X", "Y"};
  NodeOutput inside;
  for (const int axis : IndexRange(2)) {
    if (!clipped[axis]) {
      continue;
    }
    bNode *floor_node = clip_math_add(tree, grid_, NODE_MATH_FLOOR);
    if (floor_node != nullptr) {
      made.append(floor_node);
    }
    bNode *compare = clip_math_add(tree, grid_, NODE_MATH_COMPARE);
    if (compare != nullptr) {
      made.append(compare);
    }
    bNodeSocket *from = socket_out(*separate, axis_names[axis]);
    bNodeSocket *floor_in = (floor_node != nullptr) ? socket_in(*floor_node, "Value") : nullptr;
    bNodeSocket *floor_out = (floor_node != nullptr) ? socket_out(*floor_node, "Value") : nullptr;
    bNodeSocket *compare_in = (compare != nullptr) ? socket_in(*compare, "Value") : nullptr;
    bNodeSocket *compare_target = (compare != nullptr) ? socket_in(*compare, "Value_001") : nullptr;
    bNodeSocket *compare_epsilon = (compare != nullptr) ? socket_in(*compare, "Value_002") :
                                                          nullptr;
    bNodeSocket *compare_out = (compare != nullptr) ? socket_out(*compare, "Value") : nullptr;
    if (from == nullptr || floor_in == nullptr || floor_out == nullptr || compare_in == nullptr ||
        compare_target == nullptr || compare_epsilon == nullptr || compare_out == nullptr ||
        compare_target->default_value == nullptr || compare_epsilon->default_value == nullptr)
    {
      return fail();
    }
    static_cast<bNodeSocketValueFloat *>(compare_target->default_value)->value = 0.0f;
    static_cast<bNodeSocketValueFloat *>(compare_epsilon->default_value)->value = 0.5f;
    bke::node_add_link(tree, *separate, *from, *floor_node, *floor_in);
    bke::node_add_link(tree, *floor_node, *floor_out, *compare, *compare_in);
    if (inside.node == nullptr) {
      inside = {compare, compare_out};
      continue;
    }
    /* Both axes clip: inside is the product of the two tests. */
    bNode *both = clip_math_add(tree, grid_, NODE_MATH_MULTIPLY);
    if (both != nullptr) {
      made.append(both);
    }
    bNodeSocket *both_a = (both != nullptr) ? socket_in(*both, "Value") : nullptr;
    bNodeSocket *both_b = (both != nullptr) ? socket_in(*both, "Value_001") : nullptr;
    bNodeSocket *both_out = (both != nullptr) ? socket_out(*both, "Value") : nullptr;
    if (both_a == nullptr || both_b == nullptr || both_out == nullptr) {
      return fail();
    }
    bke::node_add_link(tree, *inside.node, *inside.socket, *both, *both_a);
    bke::node_add_link(tree, *compare, *compare_out, *both, *both_b);
    inside = {both, both_out};
  }
  if (inside.node == nullptr) {
    return fail();
  }
  mapping_clip_masks_.add(&row, inside);
  for (bNode *node : made) {
    r_nodes.append(node);
  }
  return inside;
}

NodeOutput PaintLayersTreeBuilder::mapping_clip_alpha(bNodeTree &tree,
                                                      const MaterialPaintLayer &row,
                                                      const NodeOutput alpha,
                                                      Vector<bNode *> &r_nodes)
{
  if (alpha.node == nullptr || alpha.socket == nullptr ||
      !BKE_paint_layers_mapping_remaps(ma_, row))
  {
    return alpha;
  }
  const NodeOutput mask = this->mapping_clip_factor(tree, row, r_nodes, false);
  if (mask.node == nullptr || mask.socket == nullptr) {
    return alpha;
  }

  bNode *product = clip_math_add(tree, grid_, NODE_MATH_MULTIPLY);
  bNodeSocket *product_a = (product != nullptr) ? socket_in(*product, "Value") : nullptr;
  bNodeSocket *product_b = (product != nullptr) ? socket_in(*product, "Value_001") : nullptr;
  bNodeSocket *product_out = (product != nullptr) ? socket_out(*product, "Value") : nullptr;
  if (product_a == nullptr || product_b == nullptr || product_out == nullptr) {
    if (product != nullptr) {
      bke::node_remove_node(nullptr, tree, *product, true, false);
    }
    return alpha;
  }
  bke::node_add_link(tree, *alpha.node, *alpha.socket, *product, *product_a);
  bke::node_add_link(tree, *mask.node, *mask.socket, *product, *product_b);
  r_nodes.append(product);
  return {product, product_out};
}

void PaintLayersTreeBuilder::nodes_discard(bNodeTree &tree, Span<bNode *> nodes)
{
  /* A removed Mapping or clip factor must not stay in the lookups, or a retry of the row would link
   * to freed memory. */
  Vector<const MaterialPaintLayer *> stale;
  for (const auto item : mapping_nodes_.items()) {
    if (nodes.contains(item.value)) {
      stale.append(item.key);
    }
  }
  for (const MaterialPaintLayer *row : stale) {
    mapping_nodes_.remove(row);
  }
  stale.clear();
  for (const auto item : mapping_clip_masks_.items()) {
    if (nodes.contains(item.value.node)) {
      stale.append(item.key);
    }
  }
  for (const MaterialPaintLayer *row : stale) {
    mapping_clip_masks_.remove(row);
  }
  for (bNode *node : nodes) {
    bke::node_remove_node(nullptr, tree, *node, true, false);
  }
}

NodeOutput PaintLayersTreeBuilder::normal_remap_add(bNodeTree &tree,
                                                    const MaterialPaintLayer &row,
                                                    const NodeOutput color,
                                                    Vector<bNode *> *r_nodes)
{
  bNode **found = mapping_nodes_.lookup_ptr(&row);
  if (found == nullptr || *found == nullptr || color.node == nullptr || color.socket == nullptr) {
    return color;
  }
  bNode &mapping = **found;
  /* Rotation and Scale come from the very sockets feeding the row's chain. Links are read from
   * the tree because `bNodeSocket::link` can be stale. The rotation feeds the Vector Rotate that
   * sits before the Mapping node, not the Mapping node itself. */
  const bNodeSocket *mapping_vector = socket_in(mapping, "Vector");
  const bNodeSocket *mapping_scale = socket_in(mapping, "Scale");
  bNode *chain_rotate = nullptr;
  for (bNodeLink &link : tree.links) {
    if (link.tosock == mapping_vector) {
      chain_rotate = link.fromnode;
    }
  }
  const bNodeSocket *mapping_rotation = (chain_rotate != nullptr) ?
                                            socket_in(*chain_rotate, "Rotation") :
                                            nullptr;
  NodeOutput rotation;
  NodeOutput scale;
  for (bNodeLink &link : tree.links) {
    if (link.tosock == mapping_rotation) {
      rotation = {link.fromnode, link.fromsock};
    }
    else if (link.tosock == mapping_scale) {
      scale = {link.fromnode, link.fromsock};
    }
  }

  Vector<bNode *> made;
  bNode *encode_node = nullptr;
  bNodeSocket *encoded = normal_remap_nodes_add(tree,
                                                *color.node,
                                                *color.socket,
                                                rotation.node,
                                                rotation.socket,
                                                scale.node,
                                                scale.socket,
                                                made,
                                                encode_node);
  if (encoded == nullptr) {
    return color;
  }
  for (bNode *node : made) {
    grid_.place_extra(*node);
    if (r_nodes != nullptr) {
      r_nodes->append(node);
    }
  }
  return {encode_node, encoded};
}

void PaintLayersTreeBuilder::gradient_line_inputs_create(LayerGroup &group,
                                                         const MaterialPaintLayer &row,
                                                         const char *label)
{
  if (row.gradient == nullptr) {
    return;
  }
  char base[200];
  SNPRINTF(base, "%s Gradient Start", label);
  bNodeTreeInterfaceSocket *start = layer_group_value_input(
      group, base, "NodeSocketVector", ROLE_GRADIENT_START, row.marker, -1);
  if (start != nullptr) {
    const float value[3] = {row.gradient->start[0],
                            row.gradient->start[1],
                            BKE_paint_layers_gradient_line_z(*row.gradient, row.gradient->start[2])};
    group_input_vector_default_set(group, *start, value);
    gradient_start_inputs_.add(&row, start);
  }
  SNPRINTF(base, "%s Gradient End", label);
  bNodeTreeInterfaceSocket *end = layer_group_value_input(
      group, base, "NodeSocketVector", ROLE_GRADIENT_END, row.marker, -1);
  if (end != nullptr) {
    const float value[3] = {row.gradient->end[0],
                            row.gradient->end[1],
                            BKE_paint_layers_gradient_line_z(*row.gradient, row.gradient->end[2])};
    group_input_vector_default_set(group, *end, value);
    gradient_end_inputs_.add(&row, end);
  }
  SNPRINTF(base, "%s Gradient Midpoint", label);
  bNodeTreeInterfaceSocket *midpoint = layer_group_value_input(
      group, base, "NodeSocketFloat", ROLE_GRADIENT_MIDPOINT, row.marker, -1);
  if (midpoint != nullptr) {
    group_input_default_set(
        group, *midpoint, BKE_paint_layers_gradient_midpoint_effective(*row.gradient));
    gradient_midpoint_inputs_.add(&row, midpoint);
  }
  SNPRINTF(base, "%s Gradient Type", label);
  bNodeTreeInterfaceSocket *type = layer_group_value_input(
      group, base, "NodeSocketFloat", ROLE_GRADIENT_TYPE, row.marker, -1);
  if (type != nullptr) {
    group_input_default_set(group, *type, float(row.gradient->type));
    gradient_type_inputs_.add(&row, type);
  }
  SNPRINTF(base, "%s Gradient Repeat", label);
  bNodeTreeInterfaceSocket *repeat = layer_group_value_input(
      group, base, "NodeSocketFloat", ROLE_GRADIENT_REPEAT, row.marker, -1);
  if (repeat != nullptr) {
    group_input_default_set(group, *repeat, float(row.gradient->repeat));
    gradient_repeat_inputs_.add(&row, repeat);
  }
}

void PaintLayersTreeBuilder::create_value_inputs(LayerGroup &group,
                                                 const MaterialPaintLayer &layer)
{
  const char *layer_name = layer.name[0] != '\0' ? layer.name : "Layer";
  /* The Height record's own scale, one input for the whole row: the height modes' chain multiplies
   * the record's distance from the neutral level by it, and a scale edit is then a value. A row
   * built from its bake lays the same chain over its baked map, so the input follows the bake. */
  if (BKE_paint_layers_channel_height_blend_effective(layer, PAINT_MATERIAL_CHANNEL_HEIGHT) !=
          MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT &&
      (row_participates(ma_, layer, PAINT_MATERIAL_CHANNEL_HEIGHT) ||
       paint_layer_row_baked_channel(ma_, layer, PAINT_MATERIAL_CHANNEL_HEIGHT, nullptr)))
  {
    char base[200];
    SNPRINTF(base, "%s Height Scale", layer_name);
    bNodeTreeInterfaceSocket *scale = layer_group_value_input(
        group,
        base,
        "NodeSocketFloat",
        ROLE_HEIGHT_SCALE,
        layer.marker,
        PAINT_MATERIAL_CHANNEL_HEIGHT);
    if (scale != nullptr) {
      group_input_default_set(
          group,
          *scale,
          BKE_paint_layers_channel_height_scale(layer, PAINT_MATERIAL_CHANNEL_HEIGHT));
      height_scale_inputs_.add(&layer, scale);
    }
  }
  if (paint_layer_row_substituted(ma_, layer)) {
    /* A row built from its bake has one input per channel it has a map in: its visibility. The
     * opacity, the mask and the corrections are inside the bake, and its own object mask is applied
     * by the nodes that read the object, not by an input. */
    for (const int channel : wired_channels_) {
      if (!paint_layer_row_baked_channel(ma_, layer, channel, nullptr)) {
        continue;
      }
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      char base[200];
      SNPRINTF(base, "%s %s Opacity", layer_name, info.ui_name);
      bNodeTreeInterfaceSocket *opacity = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_OPACITY, layer.marker, channel);
      if (opacity != nullptr) {
        prop_int_set(opacity->properties, INPUT_SUBSTITUTED_PROP, 1);
        group_input_default_set(
            group,
            *opacity,
            ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) *
                forms_.lookup_default(&layer, {}).pass_through_scale);
        opacity_inputs_.lookup_or_add_default(&layer).add(channel, opacity);
      }
    }
    /* The alpha model input follows the row's participation, not only its baked channels: a channel
     * the bake has no map for still builds a row from the live description. */
    for (const int channel : wired_channels_) {
      if (row_participates(ma_, layer, channel)) {
        this->blend_mix_input_create(group, layer, channel, layer_name);
      }
    }
    return;
  }
  this->mapping_inputs_create(group, layer, layer_name);
  const bool generates = paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES);
  if (generates) {
    /* The gradient's line and shape are values of the row, shared by all of its channels. */
    this->gradient_line_inputs_create(group, layer, layer_name);
  }
  for (const int channel : wired_channels_) {
    if (!row_participates(ma_, layer, channel)) {
      continue;
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    char base[200];
    SNPRINTF(base, "%s %s Opacity", layer_name, info.ui_name);
    bNodeTreeInterfaceSocket *opacity = layer_group_value_input(
        group, base, "NodeSocketFloat", ROLE_OPACITY, layer.marker, channel);
    if (opacity != nullptr) {
      group_input_default_set(group,
                              *opacity,
                              BKE_paint_layers_channel_opacity_effective(layer, channel) *
                                  forms_.lookup_default(&layer, {}).pass_through_scale);
      opacity_inputs_.lookup_or_add_default(&layer).add(channel, opacity);
    }
    /* The alpha model of the channel: a value, so a blend edit never rewires the alpha chain. */
    this->blend_mix_input_create(group, layer, channel, layer_name);

    if (generates) {
      /* Both a colour and a scalar channel read a ColorRamp built from the description; neither
       * needs a value input beyond the shared factor. */
      continue;
    }

    if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
      /* A Material row has no record: a plain value of its source is an input, filled from the
       * source and kept current by the value sync, so editing it in the source never rebuilds. */
      const PaintLayerMaterialSource source = paint_layer_material_channel_source(
          ma_, layer, channel);
      if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
        SNPRINTF(base, "%s %s Source", layer_name, info.ui_name);
        bNodeTreeInterfaceSocket *live = layer_group_value_input(
            group, base, "NodeSocketColor", ROLE_LIVE_CONSTANT, layer.marker, channel);
        if (live != nullptr) {
          group_input_default_set(group, *live, source.constant);
          live_constant_inputs_.lookup_or_add_default(&layer).add(channel, live);
        }
      }
      continue;
    }
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
    /* An empty base builds a flat zero instead of its record's value, so it has no fill input. */
    if (record == nullptr || record->image != nullptr ||
        paint_layer_row_empty_base(ma_, layer, channel))
    {
      continue;
    }
    /* A channel with no map: its flat value is an input of the row, so a change to it is a value
     * and not a different shape of graph. */
    SNPRINTF(base, "%s %s", layer_name, info.ui_name);
    bNodeTreeInterfaceSocket *fill = layer_group_value_input(
        group, base, "NodeSocketColor", ROLE_FILL, layer.marker, channel);
    if (fill != nullptr) {
      group_input_default_set(group, *fill, record->value);
      fill_inputs_.lookup_or_add_default(&layer).add(channel, fill);
    }
    /* The constant's alpha is the content alpha the row lays, which the colour socket cannot hand
     * over: a scalar of its own, on the channels that track one. */
    if (BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel))) {
      SNPRINTF(base, "%s %s Alpha", layer_name, info.ui_name);
      bNodeTreeInterfaceSocket *fill_alpha = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_FILL_ALPHA, layer.marker, channel);
      if (fill_alpha != nullptr) {
        group_input_default_set(group, *fill_alpha, record->value[3]);
        fill_alpha_inputs_.lookup_or_add_default(&layer).add(channel, fill_alpha);
      }
    }
  }

  /* The coverage of a Material row: its source's Alpha, when that is a plain value, is an input of
   * the group like the colour values are. */
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(ma_, layer);
    bool any = false;
    for (const int channel : wired_channels_) {
      any |= row_participates(ma_, layer, channel);
    }
    if (any && coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
      char base[200];
      SNPRINTF(base, "%s Source Coverage", layer_name);
      bNodeTreeInterfaceSocket *live = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_LIVE_COVERAGE, layer.marker, -1);
      if (live != nullptr) {
        group_input_default_set(group, *live, coverage.constant);
        live_coverage_inputs_.add(&layer, live);
      }
    }
  }

  /* A content correction has its own opacity per channel it lays in, and a Fill correction its
   * colour while it has no map there: an input like a Fill row's, so editing it is a value and not
   * a different shape. The colour's alpha is no input: a Fill covers by its opacity alone, so it
   * never reaches the graph. */
  /* A filter's parameters are inputs of the group, one set for all the channels it acts in: they
   * are values, so editing one is not a different shape of graph. */
  const auto filter_params_create = [&](const MaterialPaintLayer &filter, const bool mask_item) {
    const PaintLayerKindInfo *kind = paint_layer_kind_of(filter);
    if (!paint_layer_effect_is_filter(filter) || kind == nullptr || kind->params_num == 0) {
      return;
    }
    /* A filter in the mask works on its one coverage, in no channel. */
    bool acts = mask_item;
    for (const int channel : wired_channels_) {
      acts |= paint_layer_effect_filters(ma_, filter, channel);
    }
    for (int i = 0; acts && i < kind->params_num; i++) {
      char base[200];
      SNPRINTF(base, "%s %s %s", layer_name, paint_layer_effect_label(filter), kind->params[i].label);
      bNodeTreeInterfaceSocket *param = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_PARAM, filter.marker, -1);
      if (param != nullptr) {
        prop_int_set(param->properties, INPUT_PARAM_PROP, i);
        group_input_default_set(group, *param, paint_layer_kind_param_get(filter, i));
      }
      /* Kept even when null, so the builder sees the set is incomplete and builds nothing. */
      effect_param_inputs_.lookup_or_add_default(&filter).append(param);
    }
  };
  if (paint_layer_row_is_adjustment(layer)) {
    filter_params_create(layer, false);
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    filter_params_create(effect, false);
    char effect_label[224];
    SNPRINTF(effect_label, "%s %s", layer_name, paint_layer_effect_label(effect));
    this->mapping_inputs_create(group, effect, effect_label);
    const bool effect_generates = paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_GENERATES);
    if (effect_generates) {
      this->gradient_line_inputs_create(group, effect, effect_label);
    }
    for (const int channel : wired_channels_) {
      const bool filters = paint_layer_effect_filters(ma_, effect, channel);
      if (!filters && !paint_layer_effect_lays(ma_, effect, channel)) {
        continue;
      }
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      char base[200];
      SNPRINTF(base,
               "%s %s %s Opacity",
               layer_name,
               paint_layer_effect_label(effect),
               info.ui_name);
      bNodeTreeInterfaceSocket *opacity = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_OPACITY, effect.marker, channel);
      if (opacity != nullptr) {
        group_input_default_set(group,
                                *opacity,
                                BKE_paint_layers_channel_opacity_effective(effect, channel) *
                                    forms_.lookup_default(&effect, {}).pass_through_scale);
        effect_opacity_inputs_.lookup_or_add_default(&effect).add(channel, opacity);
      }
      if (filters) {
        /* A filter has no colour of its own: its opacity is its only input. */
        continue;
      }
      if (effect_generates) {
        /* Both a colour and a scalar channel read a ColorRamp built from the description, exactly
         * as a Gradient row's do. */
        continue;
      }
      if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_READS_SOURCE)) {
        /* A correction that reads a material has no record: a plain value of its source is an
         * input, as a Material row's is, filled from the source and kept current by the value sync
         * so editing it in the source never rebuilds. */
        const PaintLayerMaterialSource source = paint_layer_material_channel_source(
            ma_, effect, channel);
        if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
          SNPRINTF(
              base, "%s %s %s Source", layer_name, paint_layer_effect_label(effect), info.ui_name);
          bNodeTreeInterfaceSocket *live = layer_group_value_input(
              group, base, "NodeSocketColor", ROLE_LIVE_CONSTANT, effect.marker, channel);
          if (live != nullptr) {
            group_input_default_set(group, *live, source.constant);
            live_constant_inputs_.lookup_or_add_default(&effect).add(channel, live);
          }
        }
        continue;
      }
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(effect, channel);
      if (!paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_FLAT_VALUE) ||
          record->image != nullptr)
      {
        continue;
      }
      SNPRINTF(base, "%s %s %s", layer_name, paint_layer_effect_label(effect), info.ui_name);
      bNodeTreeInterfaceSocket *fill = layer_group_value_input(
          group, base, "NodeSocketColor", ROLE_FILL, effect.marker, channel);
      if (fill != nullptr) {
        group_input_default_set(group, *fill, record->value);
        fill_inputs_.lookup_or_add_default(&effect).add(channel, fill);
      }
    }
    if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_READS_SOURCE)) {
      /* The transparency of the source, when a plain value, is one input for all the channels the
       * correction lays in. */
      bool lays = false;
      for (const int channel : wired_channels_) {
        lays |= paint_layer_effect_lays(ma_, effect, channel);
      }
      const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(ma_,
                                                                                       effect);
      if (lays && coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
        char base[200];
        SNPRINTF(base, "%s %s Source Coverage", layer_name, paint_layer_effect_label(effect));
        bNodeTreeInterfaceSocket *live = layer_group_value_input(
            group, base, "NodeSocketFloat", ROLE_LIVE_COVERAGE, effect.marker, -1);
        if (live != nullptr) {
          group_input_default_set(group, *live, coverage.constant);
          live_coverage_inputs_.add(&effect, live);
        }
      }
    }
  }

  /* The base mask is one for every channel of the row, so its inputs exist once per group. */
  if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
    char base[200];
    SNPRINTF(base, "%s Mask", layer_name);
    this->mapping_inputs_create(group, *mask, base);
    SNPRINTF(base, "%s Mask Opacity", layer_name);
    bNodeTreeInterfaceSocket *opacity = layer_group_value_input(
        group, base, "NodeSocketFloat", ROLE_MASK_OPACITY, layer.marker, -1);
    if (opacity != nullptr) {
      group_input_default_set(group, *opacity, BKE_paint_layers_effective_opacity(*mask));
      mask_opacity_inputs_.add(&layer, opacity);
    }
    SNPRINTF(base, "%s Mask Enabled", layer_name);
    bNodeTreeInterfaceSocket *enabled = layer_group_value_input(
        group, base, "NodeSocketFloat", ROLE_MASK_ENABLED, layer.marker, -1);
    if (enabled != nullptr) {
      group_input_default_set(
          group, *enabled, (mask->flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f);
      mask_enabled_inputs_.add(&layer, enabled);
    }
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        *mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (record != nullptr && record->image == nullptr) {
      /* A map is read by an Image Texture; the constant is only an input while there is none. */
      SNPRINTF(base, "%s Mask", layer_name);
      bNodeTreeInterfaceSocket *value = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_MASK_VALUE, layer.marker, -1);
      if (value != nullptr) {
        group_input_default_set(group, *value, paint_layer_mask_constant(*record));
        mask_value_inputs_.add(&layer, value);
      }
    }
  }

  /* A mask correction has an opacity of its own and, while it has no map, its grey: inputs like the
   * base's, named by the correction's marker so the value sync finds the item they belong to. */
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0 || !paint_layer_mask_item_lays(ma_, item)) {
      continue;
    }
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    const char *item_name = item.name[0] != '\0' ? item.name : "Mask Correction";
    char base[200];
    SNPRINTF(base, "%s %s Opacity", layer_name, item_name);
    bNodeTreeInterfaceSocket *opacity = layer_group_value_input(
        group, base, "NodeSocketFloat", ROLE_MASK_OPACITY, item.marker, -1);
    if (opacity != nullptr) {
      group_input_default_set(group, *opacity, BKE_paint_layers_effective_opacity(item));
      mask_opacity_inputs_.add(&item, opacity);
    }
    filter_params_create(item, true);
    SNPRINTF(base, "%s %s", layer_name, item_name);
    this->mapping_inputs_create(group, item, base);
    const bool item_generates = paint_layer_kind_has_flag(item, PAINT_LAYER_KIND_GENERATES);
    if (item_generates) {
      this->gradient_line_inputs_create(group, item, item_name);
    }
    if (paint_layer_kind_has_flag(item, PAINT_LAYER_KIND_READS_SOURCE)) {
      /* A mask correction that reads a material has no record: a plain value of its source's Base
       * Color is an input, and so is a plain transparency, as for a Material row. */
      const int source_channel = paint_layer_mask_source_channel(item);
      const PaintLayerMaterialSource source = paint_layer_material_channel_source(
          ma_, item, source_channel);
      if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
        SNPRINTF(base, "%s %s Source", layer_name, item_name);
        bNodeTreeInterfaceSocket *live = layer_group_value_input(
            group,
            base,
            "NodeSocketColor",
            ROLE_LIVE_CONSTANT,
            item.marker,
            source_channel);
        if (live != nullptr) {
          group_input_default_set(group, *live, source.constant);
          live_constant_inputs_.lookup_or_add_default(&item).add(source_channel, live);
        }
      }
      const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(ma_, item);
      if (coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
        SNPRINTF(base, "%s %s Source Coverage", layer_name, item_name);
        bNodeTreeInterfaceSocket *live = layer_group_value_input(
            group, base, "NodeSocketFloat", ROLE_LIVE_COVERAGE, item.marker, -1);
        if (live != nullptr) {
          group_input_default_set(group, *live, coverage.constant);
          live_coverage_inputs_.add(&item, live);
        }
      }
      continue;
    }
    if (paint_layer_kind_is_map_coverage(item)) {
      /* The operator's tolerance and softness: values of its kind's schema, like a filter's. */
      const PaintLayerKindInfo *kind = paint_layer_kind_of(item);
      for (int i = 0; kind != nullptr && i < kind->params_num; i++) {
        char param_base[200];
        SNPRINTF(param_base, "%s %s %s", layer_name, item_name, kind->params[i].label);
        bNodeTreeInterfaceSocket *param = layer_group_value_input(
            group, param_base, "NodeSocketFloat", ROLE_PARAM, item.marker, -1);
        if (param != nullptr) {
          prop_int_set(param->properties, INPUT_PARAM_PROP, i);
          group_input_default_set(group, *param, paint_layer_kind_param_get(item, i));
        }
        /* Kept even when null, so the builder sees the set is incomplete and builds nothing. */
        effect_param_inputs_.lookup_or_add_default(&item).append(param);
      }
      /* Every colour is a value input: adding or removing one changes the count and rebuilds, but
       * editing a colour or its tick moves in place. */
      Vector<IdColorInputs> &inputs = id_color_inputs_.lookup_or_add_default(&item);
      int color_index = 0;
      for (const MaterialPaintLayerIdColor &color : item.id_colors) {
        IdColorInputs entry;
        char color_base[200];
        SNPRINTF(color_base, "%s %s Colour %d Enabled", layer_name, item_name, color_index);
        entry.enabled = layer_group_value_input(
            group, color_base, "NodeSocketFloat", ROLE_ID_COLOR_ENABLED, item.marker, -1);
        if (entry.enabled != nullptr) {
          prop_int_set(entry.enabled->properties, INPUT_ID_COLOR_PROP, color_index);
          group_input_default_set(group,
                                  *entry.enabled,
                                  (color.flag & MA_PAINT_LAYER_ID_COLOR_ENABLED) != 0 ? 1.0f :
                                                                                        0.0f);
        }
        SNPRINTF(color_base, "%s %s Colour %d", layer_name, item_name, color_index);
        entry.color = layer_group_value_input(
            group, color_base, "NodeSocketColor", ROLE_ID_COLOR, item.marker, -1);
        if (entry.color != nullptr) {
          prop_int_set(entry.color->properties, INPUT_ID_COLOR_PROP, color_index);
          float value[4] = {color.color[0], color.color[1], color.color[2], 1.0f};
          group_input_default_set(group, *entry.color, value);
        }
        if (paint_layer_mask_item_color_id_advanced(item)) {
          SNPRINTF(color_base, "%s %s Colour %d Tolerance", layer_name, item_name, color_index);
          entry.tolerance = layer_group_value_input(
              group, color_base, "NodeSocketFloat", ROLE_ID_COLOR_TOLERANCE, item.marker, -1);
          if (entry.tolerance != nullptr) {
            prop_int_set(entry.tolerance->properties, INPUT_ID_COLOR_PROP, color_index);
            group_input_default_set(group, *entry.tolerance, color.tolerance);
          }
        }
        inputs.append(entry);
        color_index++;
      }
      /* The record's value is the Gradient output scale, not a constant mask. */
      if (record != nullptr) {
        /* The output scale: one for White/Black, the item's gradient value in Gradient mode. It is
         * always built, so switching the mode or moving the value rebuilds nothing. */
        SNPRINTF(base, "%s %s Output", layer_name, item_name);
        bNodeTreeInterfaceSocket *value = layer_group_value_input(
            group, base, "NodeSocketFloat", ROLE_MASK_VALUE, item.marker, -1);
        if (value != nullptr) {
          group_input_default_set(
              group, *value, BKE_paint_layers_mask_item_output_value(item));
          mask_value_inputs_.add(&item, value);
        }
      }
      continue;
    }
    if (!item_generates && record != nullptr && record->image == nullptr) {
      SNPRINTF(base, "%s %s", layer_name, item_name);
      bNodeTreeInterfaceSocket *value = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_MASK_VALUE, item.marker, -1);
      if (value != nullptr) {
        group_input_default_set(group, *value, paint_layer_mask_constant(*record));
        mask_value_inputs_.add(&item, value);
      }
    }
  }
}

bool PaintLayersTreeBuilder::layer_group_io_nodes_add(LayerGroup &group)
{
  group.group_input = bke::node_add_node(nullptr, *group.tree, "NodeGroupInput"_ustr);
  group.group_output = bke::node_add_node(nullptr, *group.tree, "NodeGroupOutput"_ustr);
  if (group.group_input == nullptr || group.group_output == nullptr) {
    return false;
  }
  group.group_input->location[0] = 0.0f;
  group.group_output->location[0] = float(int(layout::Column::Extra) + 1) * layout::kColumnWidth;
  return true;
}

LayerGroup *PaintLayersTreeBuilder::layer_group_ensure(const MaterialPaintLayer &layer,
                                                       const float location_x,
                                                       const float location_y)
{
  if (LayerGroup **found = layer_groups_.lookup_ptr(&layer)) {
    return *found;
  }
  bNodeTree *group_tree = ctx_.layer_tree_get(layer);
  if (group_tree == nullptr || tree_.typeinfo == nullptr || tree_.typeinfo->group_idname == nullptr)
  {
    return nullptr;
  }
  layer_group_storage_.append(std::make_unique<LayerGroup>());
  LayerGroup &group = *layer_group_storage_.last();
  layer_groups_.add(&layer, &group);
  group.tree = group_tree;
  group.parent_tree = &tree_;
  const bool reused = ctx_.layer_unchanged && ctx_.layer_unchanged(layer);
  if (reused) {
    /* The factory kept this group: its Group Input/Output nodes and value inputs are already in
     * place, so find them instead of adding a second set on top. */
    for (bNode &node : group_tree->nodes) {
      if (node.is_group_input()) {
        group.group_input = &node;
      }
      else if (node.is_group_output()) {
        group.group_output = &node;
      }
    }
    if (group.group_input == nullptr || group.group_output == nullptr) {
      return nullptr;
    }
  }
  else {
    /* A fresh (or rebuilt) tree: the Group Input/Output nodes are this build's. */
    if (!layer_group_io_nodes_add(group)) {
      return nullptr;
    }
  }
  bNode *instance = bke::node_add_node(nullptr, tree_, tree_.typeinfo->group_idname);
  if (instance == nullptr) {
    return nullptr;
  }
  instance->id = &group_tree->id;
  /* The factory tree has no users; assigning the id makes this instance its only one. */
  id_us_plus(&group_tree->id);
  STRNCPY_UTF8(instance->label, layer.name);
  instance->location[0] = location_x;
  instance->location[1] = location_y;
  group.instance = instance;
  refresh_layer_group(group);
  if (!reused) {
    create_value_inputs(group, layer);
  }
  return &group;
}

bool PaintLayersTreeBuilder::result_interface_create()
{
  /* Interface sockets are created before the group output node, so that node gets its sockets
   * immediately and can be linked right away. */
  bNodeTreeInterface &interface = tree_.tree_interface;
  for (const int channel : wired_channels_) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    char result_name[64];
    SNPRINTF(result_name, "Result %s", info.ui_name);
    bNodeTreeInterfaceSocket *socket = interface.add_socket(
        result_name, "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    if (socket != nullptr) {
      result_outputs_.add(channel, socket);
    }
  }
  /* The active row's mask, exposed next to the channels for the viewport's Mask pass. It is linked
   * after the channels (the active row's group may be built by any of them). */
  mask_output_ = interface.add_socket(
      "Result Mask", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  group_output_ = bke::node_add_node(nullptr, tree_, "NodeGroupOutput"_ustr);
  if (group_output_ == nullptr) {
    return false;
  }
  group_output_->location[0] = 600;
  return true;
}

void PaintLayersTreeBuilder::blend_mix_input_create(LayerGroup &group,
                                                    const MaterialPaintLayer &layer,
                                                    const int channel,
                                                    const char *layer_name)
{
  if (!BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel))) {
    return;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char base[200];
  SNPRINTF(base, "%s %s Is Mix", layer_name, info.ui_name);
  bNodeTreeInterfaceSocket *input = layer_group_value_input(
      group, base, "NodeSocketFloat", ROLE_BLEND_IS_MIX, layer.marker, channel);
  if (input == nullptr) {
    return;
  }
  const int blend = BKE_paint_layers_channel_blend_effective(layer, channel);
  const bool is_mix = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(blend)) ==
                      MA_RAMP_BLEND;
  group_input_default_set(group, *input, is_mix ? 1.0f : 0.0f);
  blend_mix_inputs_.lookup_or_add_default(&layer).add(channel, input);
}

NodeOutput PaintLayersTreeBuilder::channel_bottom_add(const int channel, const float location_y)
{
  /* The bottom of every chain is a constant rather than the first layer's own map: a chain that
   * started at a bare image would have no Mix node of its own, and the layer's opacity could not
   * be applied at all. The shared channel table is what the CPU composite starts from too, so a
   * partially covered row fades towards the same colour on both sides. */
  bNode *bottom_node = bke::node_add_static_node(nullptr, tree_, SH_NODE_RGB);
  if (bottom_node == nullptr) {
    return {};
  }
  bottom_node->location[0] = 0.0f;
  bottom_node->location[1] = location_y;
  bNodeSocket *bottom_out = socket_out(*bottom_node, "Color");
  if (bottom_out == nullptr) {
    return {};
  }
  float bottom_color[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
  if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(bottom_out->default_value))
  {
    copy_v4_v4(value->value, bottom_color);
  }
  return {bottom_node, bottom_out};
}

ChainEnd PaintLayersTreeBuilder::channel_chain_build(const int channel,
                                                     const ChainEnd below,
                                                     const float location_y,
                                                     float &location_x)
{
  return this->list_build(ma_.paint_layers, below, channel, location_y, location_x, false);
}

/**
 * A Vector Math node of \a operation at (\a x, \a y). A Multiply Add gets \a scale and \a offset
 * as its second and third vectors, which is how it decodes or encodes a normal.
 */
static bNode *vector_math_add(bNodeTree &tree,
                              const int operation,
                              const float x,
                              const float y,
                              const float scale = 0.0f,
                              const float offset = 0.0f)
{
  bNode *node = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
  if (node == nullptr) {
    return nullptr;
  }
  node->custom1 = operation;
  node->location[0] = x;
  node->location[1] = y;
  if (operation == NODE_VECTOR_MATH_MULTIPLY_ADD) {
    const std::pair<const char *, float> constants[] = {{"Vector_001", scale},
                                                         {"Vector_002", offset}};
    for (const auto &[name, value] : constants) {
      bNodeSocket *socket = socket_in(*node, name);
      if (socket != nullptr && socket->default_value != nullptr) {
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, value);
      }
    }
  }
  return node;
}

/**
 * A constant node holding zero: an RGB node (\a is_color) or a Value node. Null when it cannot be
 * made.
 */
static bNode *zero_node_add(bNodeTree &tree, const bool is_color, const float x, const float y)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, is_color ? SH_NODE_RGB : SH_NODE_VALUE);
  bNodeSocket *out = (node != nullptr) ? socket_out(*node, is_color ? "Color" : "Value") : nullptr;
  if (out == nullptr) {
    return nullptr;
  }
  if (out->default_value != nullptr) {
    if (is_color) {
      zero_v4(static_cast<bNodeSocketValueRGBA *>(out->default_value)->value);
    }
    else {
      static_cast<bNodeSocketValueFloat *>(out->default_value)->value = 0.0f;
    }
  }
  node->location[0] = x;
  node->location[1] = y;
  return node;
}

static std::atomic<int> g_kept_group_rebuild_count = 0;

int paint_layers_kept_group_rebuild_count()
{
  return g_kept_group_rebuild_count.load();
}

void paint_layers_kept_group_rebuild_count_reset()
{
  g_kept_group_rebuild_count = 0;
}

/**
 * Whether an Anchor Point is part of \a layer: the row is a reader, or one of its corrections or
 * mask items is a marker or a reader. Only such a row can need its kept group rebuilt for a tap or
 * a reader input the group predates; any other row keeps the plain skip of a channel it cannot lay.
 */
static bool row_has_anchor_point(const MaterialPaintLayer &layer)
{
  if (paint_layer_kind_reads_anchor_point(layer)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (paint_layer_kind_reads_anchor_point(effect) ||
        paint_layer_kind_is_anchor_point_marker(effect))
    {
      return true;
    }
  }
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if (paint_layer_kind_reads_anchor_point(item) || paint_layer_kind_is_anchor_point_marker(item))
    {
      return true;
    }
  }
  return false;
}

ChainEnd PaintLayersTreeBuilder::list_build(const ListBaseT<MaterialPaintLayer> &list,
                                            ChainEnd end,
                                            const int channel,
                                            const float location_y,
                                            float &location_x,
                                            const bool isolated)
{
  for (const MaterialPaintLayer &layer : list) {
    if (!row_participates(ma_, layer, channel)) {
      continue;
    }
    if (failed_rows_.contains(&layer)) {
      /* The rebuild of this row failed on an earlier channel: the group holds no contract for
       * this one, and ensuring it again would only add a second instance to the root (or reach a
       * group whose Group Input/Output never got created). Checked before anything of the row is
       * built, so a folder's children are not built again to be thrown away. */
      continue;
    }

    /* A Pass Through folder is expanded in place: its rows are chained straight onto the parent's
     * chain, in the parent's own form, as if the folder were not there. Over is associative for the
     * Mix blend at full weight, so this equals the isolated result the predicate stands for, without
     * its group, divide and coverage nodes. A hidden one scales its rows' opacity to zero instead
     * (#PaintLayerForm::pass_through_scale), which keeps hiding a value edit. */
    if (BKE_paint_layers_folder_is_pass_through(layer)) {
      end = this->list_build(layer.children, end, channel, location_y, location_x, isolated);
      continue;
    }

    /* A folder composites what it holds in isolation: the children are chained first, over nothing
     * (pre-multiplied colour `P` and coverage `a`, both zero), and the folder row then lays the
     * straightened result `S = P / a` over what is below by its own factor times `a`. The divide is
     * safe at zero, so a folder nothing of which covers reads as no colour and no coverage. */
    NodeOutput folder_color;
    NodeOutput folder_coverage;
    NodeOutput folder_alpha;
    /* The folder's own zero/divide nodes in the root tree: kept so a failed row rebuild can drop
     * them instead of leaving orphans beside the chain. */
    bNode *folder_p_zero = nullptr;
    bNode *folder_a_zero = nullptr;
    bNode *folder_pa_zero = nullptr;
    const bool track_alpha = BKE_paint_material_channel_tracks_content_alpha(
        eMaterialPaintChannel(channel));
    /* A folder whose bake stands in builds nothing of what it holds: its row reads the bake. */
    if (BKE_paint_layers_is_folder(layer) && !paint_layer_row_baked_channel(ma_, layer, channel, nullptr))
    {
      folder_p_zero = zero_node_add(tree_, true, location_x, location_y - 140.0f);
      folder_a_zero = zero_node_add(tree_, false, location_x, location_y - 260.0f);
      /* The content alpha is accumulated pre-multiplied beside the colour, from zero. */
      folder_pa_zero = track_alpha ? zero_node_add(tree_, false, location_x, location_y - 380.0f) :
                                     nullptr;
      /* A folder that cannot be built leaves no half of it behind in the tree. */
      const auto discard = [&](bNode *extra) {
        for (bNode *node : {folder_p_zero, folder_a_zero, folder_pa_zero, extra}) {
          if (node != nullptr) {
            bke::node_remove_node(nullptr, tree_, *node, true, false);
          }
        }
        folder_p_zero = folder_a_zero = folder_pa_zero = nullptr;
      };
      if (folder_p_zero == nullptr || folder_a_zero == nullptr ||
          (track_alpha && folder_pa_zero == nullptr))
      {
        discard(nullptr);
        continue;
      }
      ChainEnd inner_start;
      inner_start.color = {folder_p_zero, socket_out(*folder_p_zero, "Color")};
      inner_start.coverage = {folder_a_zero, socket_out(*folder_a_zero, "Value")};
      if (folder_pa_zero != nullptr) {
        inner_start.alpha = {folder_pa_zero, socket_out(*folder_pa_zero, "Value")};
      }
      const ChainEnd inner = this->list_build(
          layer.children, inner_start, channel, location_y, location_x, true);
      bNode *divide = vector_math_add(
          tree_, NODE_VECTOR_MATH_DIVIDE, location_x, location_y - 140.0f);
      bNodeSocket *divide_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
      bNodeSocket *divide_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") : nullptr;
      bNodeSocket *divide_out = (divide != nullptr) ? socket_out(*divide, "Vector") : nullptr;
      if (divide_out == nullptr || inner.color.socket == nullptr ||
          inner.coverage.socket == nullptr)
      {
        discard(divide);
        continue;
      }
      bke::node_add_link(tree_, *inner.color.node, *inner.color.socket, *divide, *divide_a);
      bke::node_add_link(tree_, *inner.coverage.node, *inner.coverage.socket, *divide, *divide_b);
      folder_color = {divide, divide_out};
      folder_coverage = inner.coverage;
      /* The straight alpha of what the folder holds: `S_a = P_a / a`, zero where nothing covers. */
      if (track_alpha && inner.alpha.socket != nullptr) {
        bNode *alpha_divide = bke::node_add_static_node(nullptr, tree_, SH_NODE_MATH);
        bNodeSocket *alpha_a = (alpha_divide != nullptr) ? socket_in(*alpha_divide, "Value") :
                                                           nullptr;
        bNodeSocket *alpha_b = (alpha_divide != nullptr) ? socket_in(*alpha_divide, "Value_001") :
                                                           nullptr;
        bNodeSocket *alpha_out = (alpha_divide != nullptr) ? socket_out(*alpha_divide, "Value") :
                                                             nullptr;
        if (alpha_out != nullptr) {
          alpha_divide->custom1 = NODE_MATH_DIVIDE;
          alpha_divide->location[0] = location_x;
          alpha_divide->location[1] = location_y - 320.0f;
          bke::node_add_link(
              tree_, *inner.alpha.node, *inner.alpha.socket, *alpha_divide, *alpha_a);
          bke::node_add_link(
              tree_, *inner.coverage.node, *inner.coverage.socket, *alpha_divide, *alpha_b);
          folder_alpha = {alpha_divide, alpha_out};
        }
      }
      location_x += 180.0f;
    }

    /* Folder divide/zero nodes live in the root tree and are linked into the instance below. If the
     * row is dropped they would stay as orphans: remove them the same way a failed folder build
     * discards its half-built nodes. Do not touch folder_coverage: that socket belongs to a child.
     */
    const auto discard_folder_nodes = [&]() {
      for (bNode *node :
           {folder_color.node, folder_alpha.node, folder_p_zero, folder_a_zero, folder_pa_zero})
      {
        if (node != nullptr) {
          bke::node_remove_node(nullptr, tree_, *node, true, false);
        }
      }
      folder_color = {};
      folder_coverage = {};
      folder_alpha = {};
      folder_p_zero = folder_a_zero = folder_pa_zero = nullptr;
    };
    /* Leave the row out of the chain for the rest of the pass. \a invalidate also makes the next
     * pass rebuild its group from the description, for a group that is broken and not just empty
     * of this channel. */
    const auto drop_row = [&](const bool invalidate) {
      failed_rows_.add(&layer);
      if (invalidate && ctx_.layer_invalidate) {
        ctx_.layer_invalidate(layer);
      }
      discard_folder_nodes();
    };
    const bool already_rebuilt = rebuilt_rows_.contains(&layer);
    LayerGroup *group = layer_group_ensure(layer, location_x, location_y);
    if (group == nullptr) {
      /* A kept group without its Group Input/Output cannot describe the row: let the next pass
       * build the group from its description instead of keeping a broken one forever. */
      drop_row(true);
      continue;
    }
    /* A group the factory kept already holds the row: only its instance is re-linked, so its nodes
     * are never rebuilt and a value-only pass touches nothing. A group torn down and rebuilt
     * earlier in this pass holds the channels built before it; a channel that was not built yet is
     * built now, the rest only re-link their instance sockets.
     *
     * Asked only now: the factory decides whether the group is unchanged while it hands the group
     * out (#layer_group_ensure), so asking earlier answers "changed" for the first channel of every
     * row and builds a second set of nodes into a group that is kept. */
    const bool reused = ctx_.layer_unchanged && ctx_.layer_unchanged(layer);
    RowResult row{};
    if (already_rebuilt) {
      if (group->instance == nullptr) {
        /* The rebuild of this row failed on an earlier channel, or left no instance: the group in
         * the root holds nothing to grow here. */
        drop_row(false);
        continue;
      }
      const Set<int> *built = built_channels_.lookup_ptr(&layer);
      const bool channel_built = built != nullptr && built->contains(channel);
      row = channel_built ? row_link_reused(layer, *group, channel, isolated) :
                            build_row(layer, *group, channel, isolated);
      if (!row.valid) {
        /* The rebuild left no contract for this channel: let the next pass rebuild the group from
         * its description rather than keep a group that can never grow it. */
        drop_row(true);
        continue;
      }
    }
    else if (reused) {
      row = row_link_reused(layer, *group, channel, isolated);
      if (!row.valid && !row_has_anchor_point(layer)) {
        /* Not a row an Anchor Point touches: the kept group simply does not lay this channel (a
         * channel the source supplies nothing for gives no sockets), and that has always meant
         * skipping it. Tearing the group down for it would drop the row's other channels too. */
        continue;
      }
      if (!row.valid) {
        /* A kept group whose instance cannot be re-linked predates an input this row now needs.
         * Rebuild it from an empty tree (the interface stays) rather than adding a second set
         * beside the kept nodes, or dropping the row silently. */
        g_kept_group_rebuild_count.fetch_add(1);
        if (ctx_.bmain != nullptr) {
          bool group_ok = false;
          row = row_rebuild_kept(layer, *group, channel, isolated, group_ok);
          rebuilt_rows_.add(&layer);
          if (!row.valid && group_ok) {
            /* The group is whole again and this channel is one the row cannot lay: the plain skip
             * of a channel, not a broken group. */
            discard_folder_nodes();
            continue;
          }
        }
        else {
          /* No Main: cannot rebuild. The group record stays (removing it would let the next
           * channel's ensure make a second group beside this one, losing the channels already
           * chained through it), but its instance leaves the root: a row dropped in this pass is
           * rebuilt from its description on the next one through #layer_invalidate. */
          rebuilt_rows_.add(&layer);
          if (group->instance != nullptr) {
            bke::node_remove_node(nullptr, tree_, *group->instance, true, false);
            group->instance = nullptr;
          }
        }
        if (!row.valid) {
          drop_row(true);
          continue;
        }
      }
    }
    else {
      row = build_row(layer, *group, channel, isolated);
    }
    if (!row.valid) {
      discard_folder_nodes();
      continue;
    }
    bke::node_add_link(tree_, *end.color.node, *end.color.socket, *row.instance, *row.below);
    if (isolated && row.below_coverage != nullptr && end.coverage.socket != nullptr) {
      bke::node_add_link(
          tree_, *end.coverage.node, *end.coverage.socket, *row.instance, *row.below_coverage);
    }
    if (folder_color.socket != nullptr && row.folder_color != nullptr &&
        row.folder_coverage != nullptr)
    {
      bke::node_add_link(
          tree_, *folder_color.node, *folder_color.socket, *row.instance, *row.folder_color);
      bke::node_add_link(tree_,
                         *folder_coverage.node,
                         *folder_coverage.socket,
                         *row.instance,
                         *row.folder_coverage);
    }
    if (row.below_alpha != nullptr && end.alpha.socket != nullptr) {
      bke::node_add_link(
          tree_, *end.alpha.node, *end.alpha.socket, *row.instance, *row.below_alpha);
    }
    if (row.folder_alpha != nullptr && folder_alpha.socket != nullptr) {
      bke::node_add_link(
          tree_, *folder_alpha.node, *folder_alpha.socket, *row.instance, *row.folder_alpha);
    }
    end.color = {row.instance, row.result};
    end.coverage = (isolated && row.result_coverage != nullptr) ?
                       NodeOutput{row.instance, row.result_coverage} :
                       NodeOutput{};
    end.alpha = (row.result_alpha != nullptr) ? NodeOutput{row.instance, row.result_alpha} :
                                                NodeOutput{};
    location_x += 180.0f;
  }
  return end;
}

NodeOutput PaintLayersTreeBuilder::normal_renormalize_add(const NodeOutput from,
                                                          const float location_x,
                                                          const float location_y)
{
  bNode *decode = vector_math_add(
      tree_, NODE_VECTOR_MATH_MULTIPLY_ADD, location_x, location_y, 2.0f, -1.0f);
  bNode *normalize = vector_math_add(
      tree_, NODE_VECTOR_MATH_NORMALIZE, location_x + 180.0f, location_y);
  bNode *encode = vector_math_add(
      tree_, NODE_VECTOR_MATH_MULTIPLY_ADD, location_x + 360.0f, location_y, 0.5f, 0.5f);
  if (decode == nullptr || normalize == nullptr || encode == nullptr) {
    return from;
  }
  bNodeSocket *decode_in = socket_in(*decode, "Vector");
  bNodeSocket *decode_out = socket_out(*decode, "Vector");
  bNodeSocket *normalize_in = socket_in(*normalize, "Vector");
  bNodeSocket *normalize_out = socket_out(*normalize, "Vector");
  bNodeSocket *encode_in = socket_in(*encode, "Vector");
  bNodeSocket *encode_out = socket_out(*encode, "Vector");
  if (decode_in == nullptr || decode_out == nullptr || normalize_in == nullptr ||
      normalize_out == nullptr || encode_in == nullptr || encode_out == nullptr)
  {
    return from;
  }
  bke::node_add_link(tree_, *from.node, *from.socket, *decode, *decode_in);
  bke::node_add_link(tree_, *decode, *decode_out, *normalize, *normalize_in);
  bke::node_add_link(tree_, *normalize, *normalize_out, *encode, *encode_in);
  return {encode, encode_out};
}

void PaintLayersTreeBuilder::build()
{
  /* The factory is the single path: without it a layer has nowhere to put its nodes. */
  BLI_assert_msg(bool(ctx_.layer_tree_get),
                 "BKE_paint_layers_tree_build needs a layer-tree factory");
  if (!ctx_.layer_tree_get) {
    return;
  }
  /* Which channels the description wires at all. A channel nothing participates in gets no
   * output, so the material keeps whatever the user had on that Principled input. */
  wired_channels_ = paint_layers_wired_channels(ma_);
  if (wired_channels_.is_empty() || !this->result_interface_create()) {
    return;
  }

  float location_y = 0.0f;
  for (const int lane : wired_channels_.index_range()) {
    const int channel = wired_channels_[lane];
    /* Why one lane per channel: every channel's chain is built into the same row groups, so its
     * nodes must not share the grid cells of the channel before it. */
    layout::LayoutGrid::LaneScope lane_scope(grid_, lane);
    bNodeTreeInterfaceSocket *const *result_iface = result_outputs_.lookup_ptr(channel);
    if (result_iface == nullptr || (*result_iface)->identifier == nullptr) {
      continue;
    }
    bNodeSocket *result_socket = bke::node_find_socket(
        *group_output_, SOCK_IN, UString::from_ptr_noinline((*result_iface)->identifier));
    if (result_socket == nullptr) {
      continue;
    }

    const NodeOutput bottom = this->channel_bottom_add(channel, location_y);
    if (bottom.node == nullptr) {
      continue;
    }
    float location_x = 180.0f;
    ChainEnd start;
    start.color = bottom;
    /* The content alpha starts from the channel's bottom alpha, as the CPU composite does. */
    if (BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel))) {
      bNode *alpha_bottom = bke::node_add_static_node(nullptr, tree_, SH_NODE_VALUE);
      bNodeSocket *alpha_out = (alpha_bottom != nullptr) ? socket_out(*alpha_bottom, "Value") :
                                                          nullptr;
      if (alpha_out != nullptr) {
        float bottom_color[4];
        BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
        if (alpha_out->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(alpha_out->default_value)->value = bottom_color[3];
        }
        alpha_bottom->location[0] = 0.0f;
        alpha_bottom->location[1] = location_y - 120.0f;
        start.alpha = {alpha_bottom, alpha_out};
      }
    }
    ChainEnd chain_end = this->channel_chain_build(channel, start, location_y, location_x);
    NodeOutput end = chain_end.color;
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && end.node != bottom.node) {
      end = this->normal_renormalize_add(end, location_x, location_y);
    }
    /* The result's fourth component is the tracked content alpha, composed over the colour once:
     * the colour chain's own alpha was cut by the divides of the isolating folders. Where nothing
     * is tracked the colour links straight as before. */
    if (chain_end.alpha.socket != nullptr && end.node != bottom.node) {
      bNode *compose = bke::node_add_static_node(nullptr, tree_, SH_NODE_COMPOSE_COLOR_ALPHA);
      bNodeSocket *compose_color = (compose != nullptr) ? socket_in(*compose, "Color") : nullptr;
      bNodeSocket *compose_alpha = (compose != nullptr) ? socket_in(*compose, "Alpha") : nullptr;
      bNodeSocket *compose_out = (compose != nullptr) ? socket_out(*compose, "Color") : nullptr;
      if (compose_color != nullptr && compose_alpha != nullptr && compose_out != nullptr) {
        compose->location[0] = location_x + 180.0f;
        compose->location[1] = location_y;
        bke::node_add_link(tree_, *end.node, *end.socket, *compose, *compose_color);
        bke::node_add_link(
            tree_, *chain_end.alpha.node, *chain_end.alpha.socket, *compose, *compose_alpha);
        end = {compose, compose_out};
      }
      else if (compose != nullptr) {
        bke::node_remove_node(nullptr, tree_, *compose, true, false);
      }
    }
    bke::node_add_link(tree_, *end.node, *end.socket, *group_output_, *result_socket);
    location_y += 240.0f;
  }

  this->root_mask_output_link();
}

void PaintLayersTreeBuilder::root_mask_output_link()
{
  if (mask_output_ == nullptr || mask_output_->identifier == nullptr || group_output_ == nullptr) {
    return;
  }
  bNodeSocket *out = bke::node_find_socket(
      *group_output_, SOCK_IN, UString::from_ptr_noinline(mask_output_->identifier));
  if (out == nullptr) {
    return;
  }
  for (const auto item : layer_groups_.items()) {
    if (!BLI_uuid_equal(item.key->marker, ma_.active_layer_marker)) {
      continue;
    }
    LayerGroup *group = item.value;
    if (group == nullptr || group->instance == nullptr || group->tree == nullptr) {
      return;
    }
    bNodeSocket *mask_out = instance_output_find(*group->tree, *group->instance, "Mask");
    if (mask_out != nullptr) {
      bke::node_add_link(tree_, *group->instance, *mask_out, *group_output_, *out);
    }
    return;
  }
}

}  // namespace bke::paint_layers

void BKE_paint_layers_tree_build(const Material &ma,
                                 bNodeTree &tree,
                                 const PaintLayersBuildContext &ctx)
{
  bke::paint_layers::PaintLayersTreeBuilder(ma, tree, ctx).build();
}

}  // namespace blender
