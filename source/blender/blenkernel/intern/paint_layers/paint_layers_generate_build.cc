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

#include <utility>

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

bNodeTreeInterfaceSocket *layer_group_add_socket(LayerGroup &group,
                                                 const char *base,
                                                 const StringRef socket_type,
                                                 const NodeTreeInterfaceSocketFlag flag)
{
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

void PaintLayersTreeBuilder::create_value_inputs(LayerGroup &group, const MaterialPaintLayer &layer)
{
  const char *layer_name = layer.name[0] != '\0' ? layer.name : "Layer";
  for (const int channel : wired_channels_) {
    if (!leaf_participates(layer, channel)) {
      continue;
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    char base[200];
    SNPRINTF(base, "%s %s Opacity", layer_name, info.ui_name);
    bNodeTreeInterfaceSocket *opacity = layer_group_value_input(
        group, base, "NodeSocketFloat", ROLE_OPACITY, layer.marker, channel);
    if (opacity != nullptr) {
      group_input_default_set(group, *opacity, BKE_paint_layers_effective_opacity(layer));
      opacity_inputs_.lookup_or_add_default(&layer).add(channel, opacity);
    }

    const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
    if (record == nullptr || record->image != nullptr) {
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
  }
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
  /* The factory hands out an empty tree: the Group Input/Output nodes are this build's. */
  group.group_input = bke::node_add_node(nullptr, *group_tree, "NodeGroupInput"_ustr);
  group.group_output = bke::node_add_node(nullptr, *group_tree, "NodeGroupOutput"_ustr);
  if (group.group_input == nullptr || group.group_output == nullptr) {
    return nullptr;
  }
  group.group_input->location[0] = 0.0f;
  group.group_output->location[0] = float(int(layout::Column::Extra) + 1) * layout::kColumnWidth;
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
  create_value_inputs(group, layer);
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
  group_output_ = bke::node_add_node(nullptr, tree_, "NodeGroupOutput"_ustr);
  if (group_output_ == nullptr) {
    return false;
  }
  group_output_->location[0] = 600;
  return true;
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

NodeOutput PaintLayersTreeBuilder::channel_chain_build(const int channel,
                                                       const NodeOutput below,
                                                       const float location_y,
                                                       float &location_x)
{
  NodeOutput end = below;
  for (const MaterialPaintLayer &layer : ma_.paint_layers) {
    if (!leaf_participates(layer, channel)) {
      continue;
    }
    LayerGroup *group = layer_group_ensure(layer, location_x, location_y);
    if (group == nullptr) {
      continue;
    }
    const RowResult row = build_row(layer, *group, channel);
    if (!row.valid) {
      continue;
    }
    bke::node_add_link(tree_, *end.node, *end.socket, *row.instance, *row.below);
    end = {row.instance, row.result};
    location_x += 180.0f;
  }
  return end;
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
    NodeOutput end = this->channel_chain_build(channel, bottom, location_y, location_x);
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && end.node != bottom.node) {
      end = this->normal_renormalize_add(end, location_x, location_y);
    }
    bke::node_add_link(tree_, *end.node, *end.socket, *group_output_, *result_socket);
    location_y += 240.0f;
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
