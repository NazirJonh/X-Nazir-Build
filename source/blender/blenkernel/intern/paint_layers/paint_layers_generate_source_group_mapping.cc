/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The row's UV mapping inside a source-group wrapper: the three value inputs of every copied
 * level, the implicit UV wiring, the Vector Rotate + Mapping chain in front of each UV reader and
 * the relay of the values down the path of groups. The fill in `paint_layers_generate_source_group.cc`
 * calls these per level; the node transform itself comes from #mapping_chain_add, shared with the
 * row group, and what counts as a UV reader from #uv_readers_find, shared with the predicate.
 *
 * Nodes of a tree under construction have no topology cache, so nothing here asks a socket for its
 * owner node or its link state: owners are passed explicitly and links are read from the list.
 */

#include <utility>

#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_vector.hh"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint_layers.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"

namespace blender::bke::paint_layers {

/** The Group Input of \a tree, created when the copied tree has none. */
bNode *source_group_group_input_ensure(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (node.is_group_input()) {
      return &node;
    }
  }
  return bke::node_add_node(nullptr, tree, "NodeGroupInput"_ustr);
}

/** Find the wrapper interface input whose role names it, whatever it is called. */
bNodeTreeInterfaceSocket *source_group_mapping_input_find(bNodeTree &tree, const char *role)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    const char *socket_role = prop_string_get(socket->properties, INPUT_ROLE_PROP);
    if (socket_role != nullptr && STREQ(socket_role, role)) {
      return socket;
    }
  }
  return nullptr;
}

/**
 * Add or find one of the wrapper's three row-mapping value inputs. Found by role before anything is
 * added, so a refill keeps the socket identifiers an instance links into. \a default_value is the
 * identity the Mapping reads while no row links the input.
 */
static bNodeTreeInterfaceSocket *source_group_mapping_input_ensure(bNodeTree &tree,
                                                                  const char *name,
                                                                  const char *role,
                                                                  const float default_value[3])
{
  if (bNodeTreeInterfaceSocket *found = source_group_mapping_input_find(tree, role)) {
    return found;
  }
  bNodeTreeInterfaceSocket *socket = tree.tree_interface.add_socket(
      name, "", "NodeSocketVector", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  if (socket == nullptr) {
    return nullptr;
  }
  prop_string_set(socket->properties, INPUT_ROLE_PROP, role);
  if (socket->socket_data != nullptr) {
    copy_v3_v3(static_cast<bNodeSocketValueVector *>(socket->socket_data)->value, default_value);
  }
  return socket;
}

/** The wrapper's three row-mapping value inputs, in the order the chain reads them. */
static bool source_group_mapping_interface_ensure(bNodeTree &tree)
{
  static const float zero[3] = {0.0f, 0.0f, 0.0f};
  static const float one[3] = {1.0f, 1.0f, 1.0f};
  char name[64];
  SNPRINTF(name, "%sOffset", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  if (source_group_mapping_input_ensure(tree, name, SOURCE_GROUP_ROLE_MAPPING_OFFSET, zero) ==
      nullptr)
  {
    return false;
  }
  SNPRINTF(name, "%sScale", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  if (source_group_mapping_input_ensure(tree, name, SOURCE_GROUP_ROLE_MAPPING_SCALE, one) ==
      nullptr)
  {
    return false;
  }
  SNPRINTF(name, "%sRotation", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  if (source_group_mapping_input_ensure(tree, name, SOURCE_GROUP_ROLE_MAPPING_ROTATION, zero) ==
      nullptr)
  {
    return false;
  }
  return true;
}

/**
 * Drop the three row-mapping inputs again once the row applies no mapping. The root's interface
 * survives a refill, so inputs left by an earlier mapped build would otherwise stay. Path copies
 * need no such step: their interface is copied fresh from the source group.
 */
void source_group_mapping_interface_remove(bNodeTree &tree)
{
  const char *roles[3] = {SOURCE_GROUP_ROLE_MAPPING_OFFSET,
                          SOURCE_GROUP_ROLE_MAPPING_SCALE,
                          SOURCE_GROUP_ROLE_MAPPING_ROTATION};
  bool removed = false;
  for (const char *role : roles) {
    if (bNodeTreeInterfaceSocket *socket = source_group_mapping_input_find(tree, role)) {
      tree.tree_interface.remove_item(reinterpret_cast<bNodeTreeInterfaceItem &>(*socket));
      removed = true;
    }
  }
  if (!removed) {
    return;
  }
  /* The Group Input only loses the sockets once its declaration is updated. */
  for (bNode &node : tree.nodes) {
    if (node.is_group_input()) {
      nodes::update_node_declaration_and_sockets(tree, node);
    }
  }
}

/** The Group Input socket of the wrapper interface input named by \a role. */
bNodeSocket *source_group_mapping_input_socket(bNodeTree &tree,
                                                     bNode &group_input,
                                                     const char *role)
{
  bNodeTreeInterfaceSocket *interface = source_group_mapping_input_find(tree, role);
  if (interface == nullptr || interface->identifier == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      group_input, SOCK_OUT, UString::from_ptr_noinline(interface->identifier));
}

/**
 * With no UV layer named, an Image Texture with an open Vector reads the object's active UV. Feed
 * those open vectors from one Texture Coordinate `UV` output here, so the mapping insertion then
 * finds it. Links are read from the tree, whose per-socket caches this build has not refreshed.
 */
static void source_group_implicit_uv_wire(bNodeTree &tree)
{
  bNode *tex_coord = nullptr;
  bNodeSocket *uv_out = nullptr;
  for (bNode *texture : uv_readers_find(tree).open_images) {
    bNodeSocket *vector = socket_in(*texture, "Vector");
    if (vector == nullptr) {
      continue;
    }
    if (tex_coord == nullptr) {
      tex_coord = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_COORD);
      if (tex_coord == nullptr) {
        return;
      }
      uv_out = socket_out(*tex_coord, "UV");
      if (uv_out == nullptr) {
        return;
      }
      tex_coord->location[0] = texture->location[0] - 200.0f;
      tex_coord->location[1] = texture->location[1];
    }
    bke::node_add_link(tree, *tex_coord, *uv_out, *texture, *vector);
  }
}

/**
 * Whether \a image reads a UV that the row mapping moved: its Vector is open (the implicit wiring
 * feeds it from the mapped Texture Coordinate) or its upstream chain passes through a generated
 * mapping chain node. A texture on Generated/Object coordinates or on a named UV Map that the
 * insertion left alone stays outside and keeps the source's own extension. A Vector fed by the
 * Group Input comes from the parent level, whose chain has moved it.
 */
static bool image_texture_reads_mapped_uv(const bNodeTree &tree, const bNode &image)
{
  const bNodeSocket *vector = socket_in(const_cast<bNode &>(image), "Vector");
  const bNodeSocket *socket = vector;
  for (int depth = 0; depth < 16 && socket != nullptr; depth++) {
    /* Links are read from the list: the per-socket caches of a copied tree are stale. */
    const bNodeLink *link = nullptr;
    for (const bNodeLink &candidate : tree.links) {
      if (candidate.tosock == socket) {
        link = &candidate;
        break;
      }
    }
    if (link == nullptr) {
      /* An open Vector of the texture itself reads the active UV; an open one further up does not. */
      return depth == 0;
    }
    const bNode &node = *link->fromnode;
    if (node_is_mapping_chain(node) || node.is_group_input()) {
      return true;
    }
    if (node.type_legacy == SH_NODE_MAPPING || node.type_legacy == SH_NODE_VECTOR_ROTATE) {
      socket = socket_in(const_cast<bNode &>(node), "Vector");
      continue;
    }
    return false;
  }
  return false;
}

/** Tiling semantics for image textures whose UV the mapped chain actually moves. */
static void source_group_image_textures_repeat(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE || !image_texture_reads_mapped_uv(tree, node)) {
      continue;
    }
    if (NodeTexImage *storage = static_cast<NodeTexImage *>(node.storage)) {
      storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
    }
  }
}

/**
 * Insert the row mapping in front of every UV output that feeds anything: each Texture Coordinate
 * node and every UV Map node with no layer named (the active UV; other named layers are the
 * source's own and stay untouched). The chain is `Vector Rotate` then `Mapping`, so the UVs go
 * through `S * R * uv + offset`: rotate first, then scale and offset. The rotation comes from the
 * wrapper's rotation input, the offset and scale from its other two; the Mapping's own Rotation
 * stays at its identity default. The outgoing links are read from #bNodeTree::links, not the stale
 * per-socket caches, and re-pointed through the Mapping. Generated nodes carry the dot-prefixed
 * names so the value sync never matches them against a source node.
 */
static int source_group_mapping_insert(bNodeTree &tree, bNode &group_input)
{
  int inserted = 0;
  bNodeSocket *offset_out = source_group_mapping_input_socket(
      tree, group_input, SOURCE_GROUP_ROLE_MAPPING_OFFSET);
  bNodeSocket *scale_out = source_group_mapping_input_socket(
      tree, group_input, SOURCE_GROUP_ROLE_MAPPING_SCALE);
  bNodeSocket *rotation_out = source_group_mapping_input_socket(
      tree, group_input, SOURCE_GROUP_ROLE_MAPPING_ROTATION);
  if (offset_out == nullptr || scale_out == nullptr || rotation_out == nullptr) {
    return 0;
  }

  /* Collect first: the nodes the insertion adds would invalidate an iteration over `tree.nodes`. */
  const Vector<bNode *> uv_sources = uv_readers_find(tree).sources;

  for (bNode *source : uv_sources) {
    bNodeSocket *uv_out = socket_out(*source, "UV");
    if (uv_out == nullptr) {
      continue;
    }
    Vector<std::pair<bNode *, bNodeSocket *>> readers;
    for (bNodeLink &link : tree.links) {
      if (link.fromnode == source && link.fromsock == uv_out && link.is_available()) {
        readers.append({link.tonode, link.tosock});
      }
    }
    if (readers.is_empty()) {
      continue;
    }

    const MappingChain chain = mapping_chain_add(
        tree, *source, *uv_out, &group_input, offset_out, scale_out, rotation_out);
    if (chain.mapping == nullptr) {
      return inserted;
    }
    bNode *rotate = chain.rotate;
    bNode *mapping = chain.mapping;
    bNodeSocket *map_out = socket_out(*mapping, "Vector");
    if (map_out == nullptr) {
      return inserted;
    }
    rotate->location[0] = source->location[0] + 80.0f;
    rotate->location[1] = source->location[1];
    mapping->location[0] = source->location[0] + 160.0f;
    mapping->location[1] = source->location[1];

    /* Re-point the readers: remove-and-add keeps the tree's own bookkeeping honest. */
    for (const std::pair<bNode *, bNodeSocket *> &reader : readers) {
      bNodeLink *stale = nullptr;
      for (bNodeLink &link : tree.links) {
        if (link.fromnode == source && link.fromsock == uv_out && link.tonode == reader.first &&
            link.tosock == reader.second)
        {
          stale = &link;
          break;
        }
      }
      if (stale != nullptr) {
        bke::node_remove_link(&tree, *stale);
      }
      bke::node_add_link(tree, *mapping, *map_out, *reader.first, *reader.second);
    }
    inserted++;
  }
  return inserted;
}

/** Whether \a tree or a group it instances carries a generated row-mapping chain. */
static bool source_group_mapping_present_at(const bNodeTree &tree, const int depth)
{
  if (depth > SOURCE_GROUP_MAX_DEPTH) {
    return false;
  }
  for (const bNode &node : tree.nodes) {
    if (node_is_mapping_chain(node)) {
      return true;
    }
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
      const bNodeTree &child = *id_cast<const bNodeTree *>(node.id);
      if (source_group_mapping_present_at(child, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

bool source_group_mapping_present(const bNodeTree &tree)
{
  return source_group_mapping_present_at(tree, 0);
}

/**
 * The row-mapping half of a mapped level: inputs, implicit UVs, Repeat, and the chain. Returns
 * whether a chain was actually inserted, so a source with no UV reader is not treated as mapped by
 * the clip factor or the Normal remap.
 */
bool source_group_mapping_prepare(bNodeTree &tree)
{
  source_group_implicit_uv_wire(tree);
  if (!source_group_mapping_interface_ensure(tree)) {
    return false;
  }
  bNode *group_input = source_group_group_input_ensure(tree);
  if (group_input == nullptr) {
    return false;
  }
  nodes::update_node_declaration_and_sockets(tree, *group_input);
  const int inserted = source_group_mapping_insert(tree, *group_input);
  if (inserted > 0) {
    /* Tiling semantics only where the UV actually moves; a texture on Generated/Object
     * coordinates keeps the source's own extension. */
    source_group_image_textures_repeat(tree);
  }
  return inserted > 0;
}

/**
 * Relay the parent's three mapping inputs into the child path \a instance, each side found by role
 * on its own interface. An unlinked input leaves the child's Mapping at its identity default.
 */
void source_group_mapping_instance_wire(bNodeTree &tree,
                                               bNode &group_input,
                                               bNode &instance)
{
  const char *roles[3] = {SOURCE_GROUP_ROLE_MAPPING_OFFSET,
                          SOURCE_GROUP_ROLE_MAPPING_SCALE,
                          SOURCE_GROUP_ROLE_MAPPING_ROTATION};
  for (const char *role : roles) {
    mapping_input_relay(
        tree, group_input, source_group_mapping_input_find(tree, role), instance, role);
  }
}

/**
 * Whether the colour fed by \a node of \a tree follows the UV the row mapping moved: an Image
 * Texture by its own Vector chain, a group by the chain inside it, anything else (a mix of
 * textures) by whether this level carries a chain at all.
 */
bool source_group_color_reads_mapped_uv(const bNodeTree &tree, const bNode &node)
{
  if (node.type_legacy == SH_NODE_TEX_IMAGE) {
    return image_texture_reads_mapped_uv(tree, node);
  }
  if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
    return source_group_mapping_present(*id_cast<const bNodeTree *>(node.id));
  }
  return source_group_mapping_present(tree);
}

}  // namespace blender::bke::paint_layers
