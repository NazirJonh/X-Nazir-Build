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

void PaintLayersTreeBuilder::create_value_inputs(LayerGroup &group, const MaterialPaintLayer &layer)
{
  const char *layer_name = layer.name[0] != '\0' ? layer.name : "Layer";
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
                              BKE_paint_layers_effective_opacity(layer) *
                                  forms_.lookup_default(&layer, {}).pass_through_scale);
      opacity_inputs_.lookup_or_add_default(&layer).add(channel, opacity);
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

  /* A content correction has its own opacity per channel it lays in, and a Fill correction its
   * colour while it has no map there: an input like a Fill row's, so editing it is a value and not
   * a different shape. The colour's alpha is no input: a Fill covers by its opacity alone, so it
   * never reaches the graph. */
  for (const MaterialPaintLayer &effect : layer.effects) {
    for (const int channel : wired_channels_) {
      if (!paint_layer_effect_lays(ma_, effect, channel)) {
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
                                BKE_paint_layers_effective_opacity(effect) *
                                    forms_.lookup_default(&effect, {}).pass_through_scale);
        effect_opacity_inputs_.lookup_or_add_default(&effect).add(channel, opacity);
      }
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(effect, channel);
      if (effect.source != MA_PAINT_LAYER_SOURCE_CONSTANT || record->image != nullptr) {
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
  }

  /* The base mask is one for every channel of the row, so its inputs exist once per group. */
  if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
    char base[200];
    SNPRINTF(base, "%s Mask Opacity", layer_name);
    bNodeTreeInterfaceSocket *opacity = layer_group_value_input(
        group, base, "NodeSocketFloat", ROLE_MASK_OPACITY, layer.marker, -1);
    if (opacity != nullptr) {
      group_input_default_set(group, *opacity, BKE_paint_layers_effective_opacity(*mask));
      mask_opacity_inputs_.add(&layer, opacity);
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
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0 || !paint_layer_mask_item_lays(item)) {
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
    if (record != nullptr && record->image == nullptr) {
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
    group.group_input = bke::node_add_node(nullptr, *group_tree, "NodeGroupInput"_ustr);
    group.group_output = bke::node_add_node(nullptr, *group_tree, "NodeGroupOutput"_ustr);
    if (group.group_input == nullptr || group.group_output == nullptr) {
      return nullptr;
    }
    group.group_input->location[0] = 0.0f;
    group.group_output->location[0] = float(int(layout::Column::Extra) + 1) * layout::kColumnWidth;
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
    const bool track_alpha = BKE_paint_material_channel_tracks_content_alpha(
        eMaterialPaintChannel(channel));
    if (BKE_paint_layers_is_folder(layer)) {
      bNode *p_zero = zero_node_add(tree_, true, location_x, location_y - 140.0f);
      bNode *a_zero = zero_node_add(tree_, false, location_x, location_y - 260.0f);
      /* The content alpha is accumulated pre-multiplied beside the colour, from zero. */
      bNode *pa_zero = track_alpha ? zero_node_add(tree_, false, location_x, location_y - 380.0f) :
                                     nullptr;
      /* A folder that cannot be built leaves no half of it behind in the tree. */
      const auto discard = [&](bNode *extra) {
        for (bNode *node : {p_zero, a_zero, pa_zero, extra}) {
          if (node != nullptr) {
            bke::node_remove_node(nullptr, tree_, *node, true, false);
          }
        }
      };
      if (p_zero == nullptr || a_zero == nullptr || (track_alpha && pa_zero == nullptr)) {
        discard(nullptr);
        continue;
      }
      ChainEnd inner_start;
      inner_start.color = {p_zero, socket_out(*p_zero, "Color")};
      inner_start.coverage = {a_zero, socket_out(*a_zero, "Value")};
      if (pa_zero != nullptr) {
        inner_start.alpha = {pa_zero, socket_out(*pa_zero, "Value")};
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

    LayerGroup *group = layer_group_ensure(layer, location_x, location_y);
    if (group == nullptr) {
      continue;
    }
    /* A group the factory kept already holds the row: only its instance is re-linked, so its nodes
     * are never rebuilt and a value-only pass touches nothing. */
    const bool reused = ctx_.layer_unchanged && ctx_.layer_unchanged(layer);
    const RowResult row = reused ? row_link_reused(layer, *group, channel, isolated) :
                                   build_row(layer, *group, channel, isolated);
    if (!row.valid) {
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
}

}  // namespace bke::paint_layers

void BKE_paint_layers_tree_build(const Material &ma,
                                 bNodeTree &tree,
                                 const PaintLayersBuildContext &ctx)
{
  bke::paint_layers::PaintLayersTreeBuilder(ma, tree, ctx).build();
}

}  // namespace blender
