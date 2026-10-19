/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Helpers shared by the paint-layer generator's translation units: ID property access, socket
 * lookup, which channels a description wires, and the routing of the generated group into the
 * material's Principled BSDF.
 */

#include "paint_layers_generate_intern.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_resolve.hh"

#include "paint_layers_intern.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

namespace blender::bke::paint_layers {

/* -------------------------------------------------------------------- */
/** \name ID property access
 * \{ */

IDProperty *properties_ensure(IDProperty *&properties)
{
  if (properties == nullptr) {
    IDPropertyTemplate val = {0};
    properties = IDP_New(IDP_GROUP, &val, "RNA");
  }
  return properties;
}

void prop_string_set(IDProperty *&properties, const char *key, const char *value)
{
  IDProperty *group = properties_ensure(properties);
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(group, key, IDP_STRING);
  if (prop != nullptr) {
    IDP_AssignString(prop, value);
    return;
  }
  IDP_AddToGroup(group, IDP_NewString(value, key));
}

void prop_int_set(IDProperty *&properties, const char *key, const int value)
{
  IDProperty *group = properties_ensure(properties);
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(group, key, IDP_INT);
  if (prop != nullptr) {
    IDP_int_set(prop, value);
    return;
  }
  IDP_AddToGroup(group, IDP_NewInt(value, key));
}

const char *prop_string_get(const IDProperty *properties, const char *key)
{
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(properties, key, IDP_STRING);
  return (prop != nullptr) ? IDP_string_get(prop) : nullptr;
}

int prop_int_get(const IDProperty *properties, const char *key, const int fallback)
{
  if (properties == nullptr) {
    return fallback;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(properties, key, IDP_INT);
  return (prop != nullptr) ? IDP_int_get(prop) : fallback;
}

void uid_prop_set(IDProperty *&properties, const char *key, const bUUID &uid)
{
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, uid);
  prop_string_set(properties, key, formatted);
}

bool uid_prop_get(const IDProperty *properties, const char *key, bUUID &r_uid)
{
  const char *formatted = prop_string_get(properties, key);
  if (formatted == nullptr) {
    return false;
  }
  return BLI_uuid_parse_string(&r_uid, formatted);
}

bool source_group_row_get(const bNodeTree &tree, bUUID &r_marker)
{
  return uid_prop_get(tree.id.properties, TREE_SOURCE_ROW_PROP, r_marker);
}

void source_group_row_set(bNodeTree &tree, const bUUID &marker)
{
  uid_prop_set(tree.id.properties, TREE_SOURCE_ROW_PROP, marker);
}

bUUID tree_owner_uid_get(const bNodeTree &tree)
{
  bUUID uid = BLI_uuid_nil();
  uid_prop_get(tree.id.properties, TREE_OWNER_PROP, uid);
  return uid;
}

void tree_owner_uid_set(bNodeTree &tree, const bUUID &uid)
{
  uid_prop_set(tree.id.properties, TREE_OWNER_PROP, uid);
}

void instance_uid_set(bNode &node, const bUUID &uid)
{
  uid_prop_set(node.prop, INSTANCE_OWNER_PROP, uid);
}

bool layer_tree_marker_get(const bNodeTree &tree, bUUID &r_marker)
{
  return uid_prop_get(tree.id.properties, TREE_LAYER_PROP, r_marker);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Small helpers
 * \{ */

bNodeSocket *socket_out(bNode &node, const StringRefNull name)
{
  return bke::node_find_socket(node, SOCK_OUT, UString::from_ptr_noinline(name.c_str()));
}

bNodeSocket *socket_in(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_IN, UString::from_ptr_noinline(name));
}

/**
 * A `ShaderNodeMix` in RGBA mode: the shader's `ramp_blend`, with the factor clamped to 0..1 and
 * the result left unclamped -- exactly the CPU's `ramp_blend`, so Add can exceed one and the byte
 * encode is what finally clamps.
 */
bNode *mix_node_add(bNodeTree &tree,
                    const int ramp_blend,
                    const float location_x,
                    const float location_y)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX);
  if (node == nullptr) {
    return nullptr;
  }
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(node->storage);
  storage->data_type = SOCK_RGBA;
  storage->factor_mode = NODE_MIX_MODE_UNIFORM;
  storage->blend_type = ramp_blend;
  storage->clamp_factor = true;
  storage->clamp_result = false;
  node->location[0] = location_x;
  node->location[1] = location_y;
  return node;
}

/** A unique interface name for \a base, so a renamed or duplicated layer never collides. */
void interface_name_unique(const bNodeTreeInterface &interface,
                           const char *base,
                           char *r_buffer,
                           const size_t buffer_size)
{
  auto name_taken = [&](const char *name) {
    bool taken = false;
    interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
      if (item.item_type == NodeTreeInterfaceItemType::Socket) {
        const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
        if (socket.name != nullptr && STREQ(socket.name, name)) {
          taken = true;
          return false;
        }
      }
      return true;
    });
    return taken;
  };
  BLI_snprintf(r_buffer, buffer_size, "%s", base);
  for (int suffix = 2; name_taken(r_buffer); suffix++) {
    BLI_snprintf(r_buffer, buffer_size, "%s %d", base, suffix);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Participation
 * \{ */

bool leaf_participates(const Material &ma, const MaterialPaintLayer &layer, const int channel)
{
  return paint_layer_row_participates(ma, layer, channel);
}

bool row_participates(const Material &ma, const MaterialPaintLayer &layer, const int channel)
{
  /* A row whose bake stands in takes part through the bake's map and nothing else, whatever it
   * holds: the generator builds a source from that map alone, and so does the CPU composite. */
  if (paint_layer_row_substituted(ma, layer)) {
    return paint_layer_row_baked_channel(ma, layer, channel, nullptr);
  }
  if (!BKE_paint_layers_is_folder(layer)) {
    return leaf_participates(ma, layer, channel);
  }
  /* A folder takes part through what it holds, and through the corrections laid over its result. */
  if (paint_layer_effects_lay(ma, layer, channel)) {
    return true;
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (row_participates(ma, child, channel)) {
      return true;
    }
  }
  return false;
}

static void form_map_add(const ListBaseT<MaterialPaintLayer> &list,
                         const PaintLayerForm &form,
                         PaintLayerFormMap &r_forms)
{
  for (const MaterialPaintLayer &row : list) {
    r_forms.add(&row, form);
    /* A correction is built into its row's group and scaled like it. */
    for (const MaterialPaintLayer &effect : row.effects) {
      r_forms.add(&effect, form);
    }
    if (!BKE_paint_layers_is_folder(row)) {
      continue;
    }
    PaintLayerForm inner = form;
    if (BKE_paint_layers_folder_is_pass_through(row)) {
      /* A hidden Pass Through folder has no row of its own to carry its visibility, so it reaches
       * its rows as a zero factor on their opacity. */
      if ((row.flag & MA_PAINT_LAYER_ENABLED) == 0) {
        inner.pass_through_scale = 0.0f;
      }
    }
    else {
      inner.in_isolating_folder = true;
    }
    form_map_add(row.children, inner, r_forms);
  }
}

PaintLayerFormMap paint_layers_form_map(const Material &ma)
{
  PaintLayerFormMap forms;
  form_map_add(ma.paint_layers, {}, forms);
  return forms;
}

Vector<int> paint_layers_wired_channels(const Material &ma)
{
  Vector<int> wired;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    /* A folder owns no records, so a channel is wired exactly when some leaf, at any depth, takes
     * part in it. */
    bool wires = false;
    BKE_paint_layers_foreach(
        ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
          wires = leaf_participates(ma, layer, int(info.channel));
          return !wires;
        });
    if (wires) {
      wired.append(int(info.channel));
    }
  }
  return wired;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generated trees
 * \{ */

bNode *instance_find(const Material &ma, const bUUID &owner_uid)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    bUUID node_uid = BLI_uuid_nil();
    if (uid_prop_get(node.prop, INSTANCE_OWNER_PROP, node_uid) &&
        BLI_uuid_equal(node_uid, owner_uid))
    {
      return &node;
    }
  }
  return nullptr;
}

void tree_clear_nodes(Main &bmain, bNodeTree &tree)
{
  Vector<bNode *> nodes;
  for (bNode &node : tree.nodes) {
    nodes.append(&node);
  }
  for (bNode *node : nodes) {
    bke::node_remove_node(&bmain, tree, *node, true);
  }
}

void tree_clear(Main &bmain, bNodeTree &tree)
{
  tree_clear_nodes(bmain, tree);
  tree.tree_interface.clear_items();
}

void layer_trees_copy(Main *bmain,
                      bNodeTree &tree,
                      const bUUID &src_owner,
                      const bUUID &dst_owner)
{
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), src_owner)) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, marker) && !source_group_row_get(*group, marker)) {
      continue;
    }
    bNodeTree *group_copy = id_cast<bNodeTree *>(BKE_id_copy(bmain, &group->id));
    if (group_copy == nullptr) {
      continue;
    }
    /* #BKE_id_copy hands back a reference of its own; the node below is the user. */
    id_us_min(&group_copy->id);
    id_us_min(node.id);
    node.id = &group_copy->id;
    id_us_plus(&group_copy->id);
    tree_owner_uid_set(*group_copy, dst_owner);
    layer_trees_copy(bmain, *group_copy, src_owner, dst_owner);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Routing into the material
 * \{ */

/** The Material Output node of \a ma's embedded tree, or null. */
bNode *material_output_find(Material &ma)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL) {
      return &node;
    }
  }
  return nullptr;
}

/** The Principled input a channel routes into, or null when there is none. */
bNodeSocket *principled_channel_socket(Material &ma, const int channel)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(info.socket_name));
}

/** The first node of \a type in \a ma's embedded tree, or a new one at (\a x, \a y). */
static bNode *node_of_type_ensure(Material &ma, const int type, const float x, const float y)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == type) {
      return &node;
    }
  }
  bNode *node = bke::node_add_static_node(nullptr, *ma.nodetree, type);
  if (node != nullptr) {
    node->location[0] = x;
    node->location[1] = y;
  }
  return node;
}

/** A Normal Map node in \a ma, created when missing. */
bNode *normal_map_ensure(Material &ma)
{
  return node_of_type_ensure(ma, SH_NODE_NORMAL_MAP, 400.0f, -400.0f);
}

/** A Bump node in \a ma, created when missing. */
bNode *bump_ensure(Material &ma)
{
  return node_of_type_ensure(ma, SH_NODE_BUMP, 400.0f, -560.0f);
}

void principled_ensure(Material &ma, PaintLayersRegenerateReport &r_report)
{
  if (ma.nodetree == nullptr) {
    return;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  if (BKE_paint_material_principled_find(ma, reason) != nullptr) {
    return;
  }
  bNode *output = material_output_find(ma);
  if (output == nullptr) {
    output = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_OUTPUT_MATERIAL);
    if (output == nullptr) {
      r_report.no_principled = true;
      return;
    }
    output->location[0] = 300.0f;
  }
  bNodeSocket *surface = socket_in(*output, "Surface");
  if (surface == nullptr) {
    r_report.no_principled = true;
    return;
  }
  /* A Surface taken by something other than a Principled is the user's graph: never replaced. The
   * tree was just updated and may have lost its topology cache. */
  ma.nodetree->ensure_topology_cache();
  if (!surface->directly_linked_links().is_empty()) {
    r_report.no_principled = true;
    return;
  }
  bNode *principled = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_BSDF_PRINCIPLED);
  if (principled == nullptr) {
    r_report.no_principled = true;
    return;
  }
  principled->location[0] = 0.0f;
  principled->location[1] = 0.0f;
  bke::node_add_link(
      *ma.nodetree, *principled, *socket_out(*principled, "BSDF"), *output, *surface);
  r_report.created_principled = true;
}

void input_link_restore(Material &ma,
                        bNodeSocket &input,
                        bNode &from_node,
                        bNodeSocket &from_socket,
                        PaintLayersRegenerateReport &r_report)
{
  /* Every link added or removed below dirties the topology cache, and the next call (the following
   * channel, or the second link of one) reads `directly_linked_links` through it. */
  ma.nodetree->ensure_topology_cache();
  const Span<bNodeLink *> links = input.directly_linked_links();
  if (!links.is_empty()) {
    if (links[0]->fromnode == &from_node && links[0]->fromsock == &from_socket) {
      return;
    }
    r_report.restored_inputs = true;
  }
  bke::node_remove_socket_links(*ma.nodetree, input);
  bke::node_add_link(*ma.nodetree, from_node, from_socket, input.owner_node(), input);
}

bNodeSocket *instance_output_find(bNodeTree &tree, bNode &instance, const char *result_name)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, result_name) && iface->identifier != nullptr) {
      return bke::node_find_socket(
          instance, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
    }
  }
  return nullptr;
}

void wire_instance_to_material(Material &ma,
                               bNodeTree &tree,
                               bNode &instance,
                               PaintLayersRegenerateReport &r_report)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled_const = BKE_paint_material_principled_find(ma, reason);
  if (principled_const == nullptr) {
    r_report.no_principled = true;
    return;
  }
  bNode &principled = const_cast<bNode &>(*principled_const);

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.channel == PAINT_MATERIAL_CHANNEL_CUSTOM ||
        info.channel == PAINT_MATERIAL_CHANNEL_AO)
    {
      continue;
    }
    char result_name[64];
    SNPRINTF(result_name, "Result %s", info.ui_name);
    bNodeSocket *instance_out = instance_output_find(tree, instance, result_name);
    if (instance_out == nullptr) {
      continue;
    }

    if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      bNode *normal_map = normal_map_ensure(ma);
      if (normal_map == nullptr) {
        continue;
      }
      /* The node was just created on a tree whose topology cache predates it.
       * #input_link_restore reads a socket's owner through that cache, so refresh it before the
       * new node's sockets are used. */
      ma.nodetree->ensure_topology_cache();
      bNodeSocket *map_color = socket_in(*normal_map, "Color");
      bNodeSocket *map_normal = socket_out(*normal_map, "Normal");
      bNodeSocket *principled_normal = socket_in(principled, "Normal");
      if (map_color == nullptr || map_normal == nullptr || principled_normal == nullptr) {
        continue;
      }
      input_link_restore(ma, *map_color, instance, *instance_out, r_report);
      input_link_restore(ma, *principled_normal, *normal_map, *map_normal, r_report);
      continue;
    }
    if (info.channel == PAINT_MATERIAL_CHANNEL_HEIGHT) {
      bNode *bump = bump_ensure(ma);
      if (bump == nullptr) {
        continue;
      }
      /* See the Normal branch: the freshly created node is not in the topology cache yet. */
      ma.nodetree->ensure_topology_cache();
      bNodeSocket *bump_height = socket_in(*bump, "Height");
      bNodeSocket *bump_normal_out = socket_out(*bump, "Normal");
      bNodeSocket *principled_normal = socket_in(principled, "Normal");
      if (bump_height == nullptr || bump_normal_out == nullptr || principled_normal == nullptr) {
        continue;
      }
      input_link_restore(ma, *bump_height, instance, *instance_out, r_report);
      /* Height rides over the Normal stack: the Bump's Normal input takes whatever currently feeds
       * the Principled Normal, be that the Normal Map or a hand-built graph. */
      bNodeSocket *bump_normal_in = socket_in(*bump, "Normal");
      if (bump_normal_in != nullptr) {
        /* The Height link just made dirtied the cache that this read goes through. */
        ma.nodetree->ensure_topology_cache();
        const Span<bNodeLink *> normal_links = principled_normal->directly_linked_links();
        /* On a regenerate the Principled Normal is already fed by this Bump: its Normal input must
         * keep the user's chain, linking it to its own output would form a cycle. */
        if (normal_links.size() == 1 && normal_links[0]->fromnode != bump) {
          input_link_restore(ma,
                             *bump_normal_in,
                             *normal_links[0]->fromnode,
                             *normal_links[0]->fromsock,
                             r_report);
        }
      }
      input_link_restore(ma, *principled_normal, *bump, *bump_normal_out, r_report);
      continue;
    }

    bNodeSocket *target = principled_channel_socket(ma, info.channel);
    if (target == nullptr) {
      continue;
    }
    input_link_restore(ma, *target, instance, *instance_out, r_report);
  }
}

/** \} */

}  // namespace blender::bke::paint_layers
