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

#include <array>

#include "BLI_index_range.hh"
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

bNodeSocket *paint_layers_blend_group_socket(bNode &instance,
                                             const char *name,
                                             const char *identifier)
{
  const UString id = UString::from_ptr_noinline(identifier);
  if (bNodeSocket *socket = bke::node_find_socket(instance, SOCK_IN, id)) {
    return socket;
  }
  if (bNodeSocket *socket = bke::node_find_socket(instance, SOCK_OUT, id)) {
    return socket;
  }
  for (bNodeSocket &socket : instance.inputs) {
    if (STREQ(socket.name, name)) {
      return &socket;
    }
  }
  for (bNodeSocket &socket : instance.outputs) {
    if (STREQ(socket.name, name)) {
      return &socket;
    }
  }
  return nullptr;
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

bool paint_layer_blend_dynamic_eligible(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        const int channel)
{
  /* Normal combines normals in its own group instead of building a Mix node, so neither tier
   * exists for it. */
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      channel == PAINT_MATERIAL_CHANNEL_NORMAL)
  {
    return false;
  }
  /* A mask item's blend is the type of its coverage Mix, which is built by the mask chain and
   * never served by the dynamic blend group; a tier record for it would only force a pointless
   * rebuild when it expires. */
  if (layer.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return false;
  }
  /* An inlined folder builds no row of its own, so its blend has no node to be dynamic. */
  if (BKE_paint_layers_folder_is_pass_through(layer)) {
    return false;
  }
  /* A content correction may lay in fewer channels than its owner: only the channels it really
   * builds blend nodes in are eligible. */
  if (layer.role == MA_PAINT_LAYER_ROLE_EFFECT) {
    return paint_layer_effect_lays(ma, layer, channel);
  }
  return row_participates(ma, layer, channel);
}

uint64_t paint_layer_blend_dynamic_mask(const Material &ma, const MaterialPaintLayer &layer)
{
  uint64_t mask = 0;
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    if (!paint_layer_blend_dynamic_eligible(ma, layer, channel)) {
      continue;
    }
    if (paint_layers_blend_tier_is_dynamic(ma, layer.marker, channel)) {
      mask |= uint64_t(1) << channel;
    }
  }
  return mask;
}

bool paint_layers_blend_any_dynamic(const Material &ma)
{
  bool any = false;
  /* #BKE_paint_layers_foreach visits rows and folders only; a correction of a row builds its own
   * blend node and is walked here explicitly. */
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        if (paint_layer_blend_dynamic_mask(ma, layer) != 0) {
          any = true;
          return false;
        }
        for (const MaterialPaintLayer &effect : layer.effects) {
          if (paint_layer_blend_dynamic_mask(ma, effect) != 0) {
            any = true;
            return false;
          }
        }
        return true;
      });
  return any;
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
  /* The owner is read while the cache is still valid: removing the links below dirties it, and
   * #bNodeSocket::owner_node asserts without it (a Height Bump taking over a Normal Map's link to
   * the Principled Normal is the case that reaches it). */
  bNode &input_node = input.owner_node();
  const Span<bNodeLink *> links = input.directly_linked_links();
  if (!links.is_empty()) {
    if (links[0]->fromnode == &from_node && links[0]->fromsock == &from_socket) {
      return;
    }
    r_report.restored_inputs = true;
  }
  bke::node_remove_socket_links(*ma.nodetree, input);
  bke::node_add_link(*ma.nodetree, from_node, from_socket, input_node, input);
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

/* -------------------------------------------------------------------- */
/** \name Row UV mapping nodes
 *
 * The one place the row transform exists as nodes, shared by the row group and the source-group
 * wrapper so the two cannot drift apart (nor from the CPU resample, which follows the same order).
 * \{ */

bool node_is_mapping_chain(const bNode &node)
{
  return prop_string_get(node.prop, MAPPING_CHAIN_PROP) != nullptr;
}

UvReaders uv_readers_find(bNodeTree &tree)
{
  UvReaders found;
  /* Links are read from the list, not the per-socket caches: a tree under construction (a copy
   * the source group is filling) has stale ones, and a source tree asked from the UI is the same
   * list either way. */
  const auto linked = [&](const bNodeSocket *socket) {
    if (socket == nullptr) {
      return false;
    }
    for (const bNodeLink &link : tree.links) {
      if (link.tosock == socket || link.fromsock == socket) {
        return true;
      }
    }
    return false;
  };
  for (bNode &node : tree.nodes) {
    switch (node.type_legacy) {
      case SH_NODE_TEX_COORD:
        if (linked(socket_out(node, "UV"))) {
          found.sources.append(&node);
        }
        break;
      case SH_NODE_UVMAP: {
        /* A named layer is the source's own and is not what the row mapping moves. */
        const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(node.storage);
        if (storage != nullptr && storage->uv_map[0] == '\0' && linked(socket_out(node, "UV"))) {
          found.sources.append(&node);
        }
        break;
      }
      case SH_NODE_TEX_IMAGE:
        /* An orphan texture is not part of the shader; an open Vector samples the active UV. */
        if ((linked(socket_out(node, "Color")) || linked(socket_out(node, "Alpha"))) &&
            !linked(socket_in(node, "Vector")))
        {
          found.open_images.append(&node);
        }
        break;
      default:
        break;
    }
  }
  return found;
}

MappingChain mapping_chain_add(bNodeTree &tree,
                               bNode &uv_node,
                               bNodeSocket &uv_out,
                               bNode *values_node,
                               bNodeSocket *offset,
                               bNodeSocket *scale,
                               bNodeSocket *rotation)
{
  bNode *rotate = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_ROTATE);
  bNode *mapping = bke::node_add_static_node(nullptr, tree, SH_NODE_MAPPING);
  bNodeSocket *rotate_vector = (rotate != nullptr) ? socket_in(*rotate, "Vector") : nullptr;
  bNodeSocket *rotate_rotation = (rotate != nullptr) ? socket_in(*rotate, "Rotation") : nullptr;
  bNodeSocket *rotate_out = (rotate != nullptr) ? socket_out(*rotate, "Vector") : nullptr;
  bNodeSocket *map_vector = (mapping != nullptr) ? socket_in(*mapping, "Vector") : nullptr;
  bNodeSocket *map_location = (mapping != nullptr) ? socket_in(*mapping, "Location") : nullptr;
  bNodeSocket *map_scale = (mapping != nullptr) ? socket_in(*mapping, "Scale") : nullptr;
  if (rotate_vector == nullptr || rotate_rotation == nullptr || rotate_out == nullptr ||
      map_vector == nullptr || map_location == nullptr || map_scale == nullptr)
  {
    if (rotate != nullptr) {
      bke::node_remove_node(nullptr, tree, *rotate, true, false);
    }
    if (mapping != nullptr) {
      bke::node_remove_node(nullptr, tree, *mapping, true, false);
    }
    return {};
  }
  /* A Vector Rotate in front of the Mapping node gives `offset + scale * (R * uv)`: the Mapping
   * node alone would scale first and shear the pattern when the scale is not uniform. Point
   * mapping keeps only the scale and the offset; the rotation is the rotate node's. */
  rotate->custom1 = NODE_VECTOR_ROTATE_TYPE_EULER_XYZ;
  mapping->custom1 = NODE_MAPPING_TYPE_POINT;
  prop_string_set(rotate->prop, MAPPING_CHAIN_PROP, "rotate");
  prop_string_set(mapping->prop, MAPPING_CHAIN_PROP, "mapping");
  bke::node_add_link(tree, uv_node, uv_out, *rotate, *rotate_vector);
  bke::node_add_link(tree, *rotate, *rotate_out, *mapping, *map_vector);
  if (values_node != nullptr) {
    if (offset != nullptr) {
      bke::node_add_link(tree, *values_node, *offset, *mapping, *map_location);
    }
    if (scale != nullptr) {
      bke::node_add_link(tree, *values_node, *scale, *mapping, *map_scale);
    }
    if (rotation != nullptr) {
      bke::node_add_link(tree, *values_node, *rotation, *rotate, *rotate_rotation);
    }
  }
  return {rotate, mapping};
}

bNodeSocket *normal_remap_nodes_add(bNodeTree &tree,
                                    bNode &color_node,
                                    bNodeSocket &color_socket,
                                    bNode *rotation_node,
                                    bNodeSocket *rotation,
                                    bNode *scale_node,
                                    bNodeSocket *scale,
                                    Vector<bNode *> &r_made,
                                    bNode *&r_encode)
{
  bNode *decode = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
  bNode *sign = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
  bNode *flip = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
  bNode *rotate = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_ROTATE);
  bNode *encode = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
  const std::array<bNode *, 5> created = {decode, sign, flip, rotate, encode};
  const auto discard = [&]() -> bNodeSocket * {
    for (bNode *node : created) {
      if (node != nullptr) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
    }
    return nullptr;
  };
  if (decode == nullptr || sign == nullptr || flip == nullptr || rotate == nullptr ||
      encode == nullptr)
  {
    return discard();
  }
  decode->custom1 = NODE_VECTOR_MATH_MULTIPLY_ADD;
  encode->custom1 = NODE_VECTOR_MATH_MULTIPLY_ADD;
  sign->custom1 = NODE_VECTOR_MATH_SIGN;
  flip->custom1 = NODE_VECTOR_MATH_MULTIPLY;
  /* The UVs go through `S * R`, so a tangent normal needs its covariant counterpart
   * `R^T * S`: flip by the sign of the scale (its magnitude does not turn a direction), then
   * rotate back. */
  rotate->custom1 = NODE_VECTOR_ROTATE_TYPE_EULER_XYZ;
  rotate->custom2 = 1; /* Invert: rotation undone. */
  const auto set_madd = [](bNode &node, const float multiplier, const float addend) {
    const char *names[2] = {"Vector_001", "Vector_002"};
    const float values[2] = {multiplier, addend};
    for (const int i : IndexRange(2)) {
      bNodeSocket *socket = socket_in(node, names[i]);
      if (socket != nullptr && socket->default_value != nullptr) {
        copy_v3_fl(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, values[i]);
      }
    }
  };
  set_madd(*decode, 2.0f, -1.0f);
  set_madd(*encode, 0.5f, 0.5f);

  bNodeSocket *decode_in = socket_in(*decode, "Vector");
  bNodeSocket *decode_out = socket_out(*decode, "Vector");
  bNodeSocket *sign_in = socket_in(*sign, "Vector");
  bNodeSocket *sign_out = socket_out(*sign, "Vector");
  bNodeSocket *flip_a = socket_in(*flip, "Vector");
  bNodeSocket *flip_b = socket_in(*flip, "Vector_001");
  bNodeSocket *flip_out = socket_out(*flip, "Vector");
  bNodeSocket *rotate_in = socket_in(*rotate, "Vector");
  bNodeSocket *rotate_rotation = socket_in(*rotate, "Rotation");
  bNodeSocket *rotate_out = socket_out(*rotate, "Vector");
  bNodeSocket *encode_in = socket_in(*encode, "Vector");
  bNodeSocket *encode_out = socket_out(*encode, "Vector");
  if (decode_in == nullptr || decode_out == nullptr || sign_in == nullptr || sign_out == nullptr ||
      flip_a == nullptr || flip_b == nullptr || flip_out == nullptr || rotate_in == nullptr ||
      rotate_rotation == nullptr || rotate_out == nullptr || encode_in == nullptr ||
      encode_out == nullptr)
  {
    return discard();
  }
  for (bNode *node : created) {
    r_made.append(node);
  }
  /* The owner nodes come from the caller: a socket's own `owner_node()` needs a topology cache
   * that a tree under construction does not have. */
  if (rotation != nullptr && rotation_node != nullptr) {
    bke::node_add_link(tree, *rotation_node, *rotation, *rotate, *rotate_rotation);
  }
  if (scale != nullptr && scale_node != nullptr) {
    bke::node_add_link(tree, *scale_node, *scale, *sign, *sign_in);
  }
  else if (sign_in->default_value != nullptr) {
    /* An unlinked scale is the Mapping's identity default. */
    copy_v3_fl(static_cast<bNodeSocketValueVector *>(sign_in->default_value)->value, 1.0f);
  }
  bke::node_add_link(tree, color_node, color_socket, *decode, *decode_in);
  bke::node_add_link(tree, *decode, *decode_out, *flip, *flip_a);
  bke::node_add_link(tree, *sign, *sign_out, *flip, *flip_b);
  bke::node_add_link(tree, *flip, *flip_out, *rotate, *rotate_in);
  bke::node_add_link(tree, *rotate, *rotate_out, *encode, *encode_in);
  r_encode = encode;
  return encode_out;
}

void mapping_input_relay(bNodeTree &tree,
                         bNode &group_input,
                         const bNodeTreeInterfaceSocket *from_iface,
                         bNode &instance,
                         const char *role)
{
  if (from_iface == nullptr || from_iface->identifier == nullptr || instance.id == nullptr ||
      GS(instance.id->name) != ID_NT)
  {
    return;
  }
  bNodeTree &child = *id_cast<bNodeTree *>(instance.id);
  const bNodeTreeInterfaceSocket *child_iface = source_group_mapping_input_find(child, role);
  if (child_iface == nullptr || child_iface->identifier == nullptr) {
    return;
  }
  bNodeSocket *from = bke::node_find_socket(
      group_input, SOCK_OUT, UString::from_ptr_noinline(from_iface->identifier));
  bNodeSocket *to = bke::node_find_socket(
      instance, SOCK_IN, UString::from_ptr_noinline(child_iface->identifier));
  if (from == nullptr || to == nullptr) {
    return;
  }
  for (const bNodeLink &link : tree.links) {
    if (link.tosock == to) {
      return;
    }
  }
  bke::node_add_link(tree, group_input, *from, instance, *to);
}

/** \} */

}  // namespace blender::bke::paint_layers
