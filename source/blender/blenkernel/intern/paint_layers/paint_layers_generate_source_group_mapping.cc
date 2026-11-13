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

#include <string>
#include <utility>

#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
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
 * Add or find one of the wrapper's row-mapping value inputs. Found by role before anything is
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

/** The wrapper input role of one transform effect's mapping values: the row's own role with the
 * effect's marker appended, so an effect's inputs never collide with the row's own. */
std::string source_group_effect_mapping_role(const char *base, const MaterialPaintLayer &effect)
{
  char marker_str[UUID_STRING_SIZE];
  BLI_uuid_format(marker_str, effect.marker);
  return std::string(base) + "_" + marker_str;
}

/** The wrapper interface input of \a effect's mapping value \a base role, or null. */
static bNodeTreeInterfaceSocket *source_group_effect_mapping_input_find(
    bNodeTree &tree, const char *base, const MaterialPaintLayer &effect)
{
  const std::string role = source_group_effect_mapping_role(base, effect);
  return source_group_mapping_input_find(tree, role.c_str());
}

/** The wrapper's row-mapping value inputs -- the row's own, and one set per transform effect in
 * \a effects (a Transform Mapping carries offset, scale and rotation; a Mirror its fold center;
 * the axes are chain shape and never inputs) -- in the order the chains read them. */
static bool source_group_mapping_interface_ensure(bNodeTree &tree,
                                                  const Span<const MaterialPaintLayer *> effects)
{
  static const float zero[3] = {0.0f, 0.0f, 0.0f};
  static const float one[3] = {1.0f, 1.0f, 1.0f};
  static const float center_default[3] = {0.5f, 0.5f, 0.0f};
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
  SNPRINTF(name, "%sMirror Center", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  if (source_group_mapping_input_ensure(
          tree, name, SOURCE_GROUP_ROLE_MAPPING_MIRROR_CENTER, center_default) == nullptr)
  {
    return false;
  }
  for (const MaterialPaintLayer *effect : effects) {
    if (effect == nullptr) {
      continue;
    }
    char effect_name[160];
    SNPRINTF(effect_name, "%s%s ", SOURCE_GROUP_MAPPING_INPUT_PREFIX, effect->name);
    if (effect->kind_subtype == MA_PAINT_LAYER_FILTER_MIRROR) {
      const std::string role = source_group_effect_mapping_role(
          SOURCE_GROUP_ROLE_MAPPING_MIRROR_CENTER, *effect);
      if (source_group_mapping_input_ensure(
              tree, effect_name, role.c_str(), center_default) == nullptr)
      {
        return false;
      }
      continue;
    }
    char label[192];
    const std::string offset_role = source_group_effect_mapping_role(
        SOURCE_GROUP_ROLE_MAPPING_OFFSET, *effect);
    SNPRINTF(label, "%sOffset", effect_name);
    if (source_group_mapping_input_ensure(tree, label, offset_role.c_str(), zero) == nullptr) {
      return false;
    }
    const std::string scale_role = source_group_effect_mapping_role(
        SOURCE_GROUP_ROLE_MAPPING_SCALE, *effect);
    SNPRINTF(label, "%sScale", effect_name);
    if (source_group_mapping_input_ensure(tree, label, scale_role.c_str(), one) == nullptr) {
      return false;
    }
    const std::string rotation_role = source_group_effect_mapping_role(
        SOURCE_GROUP_ROLE_MAPPING_ROTATION, *effect);
    SNPRINTF(label, "%sRotation", effect_name);
    if (source_group_mapping_input_ensure(tree, label, rotation_role.c_str(), zero) == nullptr) {
      return false;
    }
  }
  return true;
}

/**
 * Drop the row-mapping inputs again once the row applies no mapping -- the row's own and any a
 * transform effect left behind, found by their role prefix. The root's interface survives a
 * refill, so inputs left by an earlier mapped build would otherwise stay. Path copies need no such
 * step: their interface is copied fresh from the source group.
 */
void source_group_mapping_interface_remove(bNodeTree &tree)
{
  tree.ensure_interface_cache();
  Vector<bNodeTreeInterfaceSocket *> stale;
  for (bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    const char *socket_role = prop_string_get(socket->properties, INPUT_ROLE_PROP);
    if (socket_role != nullptr && BLI_str_startswith(socket_role, "source_mapping_")) {
      stale.append(socket);
    }
  }
  bool removed = !stale.is_empty();
  for (bNodeTreeInterfaceSocket *socket : stale) {
    tree.tree_interface.remove_item(reinterpret_cast<bNodeTreeInterfaceItem &>(*socket));
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
 * source's own and stay untouched). The chain is the row's Mirror fold when \a mirror_axes folds
 * an axis, then `Vector Rotate` then `Mapping`, so the UVs go through the fold in the canvas UV
 * and then `S * R * uv + offset`. The transform effects of \a effects sit ahead of the row's own
 * chain, first in the list closest to the UV reader, each driven by its own wrapper inputs: a
 * Transform Mapping by its offset, scale and rotation, a Mirror by its center with the axes baked
 * as constants. The rotation comes from the wrapper's rotation input, the offset and scale from
 * its other two and the fold center from its fourth; the Mapping's own Rotation stays at its
 * identity default. The outgoing links are read from #bNodeTree::links, not the stale per-socket
 * caches, and re-pointed through the Mapping. Generated nodes carry the dot-prefixed names so the
 * value sync never matches them against a source node.
 */
static int source_group_mapping_insert(bNodeTree &tree,
                                       bNode &group_input,
                                       const int16_t mirror_axes,
                                       const Span<const MaterialPaintLayer *> effects)
{
  int inserted = 0;
  bNodeSocket *offset_out = source_group_mapping_input_socket(
      tree, group_input, SOURCE_GROUP_ROLE_MAPPING_OFFSET);
  bNodeSocket *scale_out = source_group_mapping_input_socket(
      tree, group_input, SOURCE_GROUP_ROLE_MAPPING_SCALE);
  bNodeSocket *rotation_out = source_group_mapping_input_socket(
      tree, group_input, SOURCE_GROUP_ROLE_MAPPING_ROTATION);
  bNodeSocket *center_out = (mirror_axes != 0) ? source_group_mapping_input_socket(
                                                     tree, group_input,
                                                     SOURCE_GROUP_ROLE_MAPPING_MIRROR_CENTER) :
                                                 nullptr;
  if (offset_out == nullptr || scale_out == nullptr || rotation_out == nullptr ||
      (mirror_axes != 0 && center_out == nullptr))
  {
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

    /* The effects first, in list order: each chain reads the previous one's output, so a fold's
     * side signs and a clip factor of a later step are evaluated where that step's fold acts.
     * Everything one reader made is removed together when a later chain fails. */
    Vector<bNode *> made;
    const auto discard_made = [&]() {
      for (bNode *node : made) {
        bke::node_remove_node(nullptr, tree, *node, true, false);
      }
      return inserted;
    };
    bNode *uv_node = source;
    bNodeSocket *uv_socket = uv_out;
    float place_x = source->location[0] + 80.0f;
    bool effects_failed = false;
    for (const MaterialPaintLayer *effect : effects) {
      if (effect == nullptr) {
        continue;
      }
      const bool mirror = effect->kind_subtype == MA_PAINT_LAYER_FILTER_MIRROR;
      const int16_t axes = mirror ? BKE_paint_layers_mapping_mirror_get(*effect) : int16_t(0);
      /* The input sockets are found through the Group Input by role; a missing one leaves the
       * effect out rather than failing the whole wrapper. */
      bNodeTreeInterfaceSocket *center_iface = (mirror) ? source_group_effect_mapping_input_find(
                                                            tree,
                                                            SOURCE_GROUP_ROLE_MAPPING_MIRROR_CENTER,
                                                            *effect) :
                                                          nullptr;
      bNodeTreeInterfaceSocket *offset_iface = (!mirror) ? source_group_effect_mapping_input_find(
                                                             tree,
                                                             SOURCE_GROUP_ROLE_MAPPING_OFFSET,
                                                             *effect) :
                                                           nullptr;
      bNodeTreeInterfaceSocket *scale_iface = (!mirror) ? source_group_effect_mapping_input_find(
                                                            tree,
                                                            SOURCE_GROUP_ROLE_MAPPING_SCALE,
                                                            *effect) :
                                                          nullptr;
      bNodeTreeInterfaceSocket *rotation_iface = (!mirror) ?
                                                     source_group_effect_mapping_input_find(
                                                         tree,
                                                         SOURCE_GROUP_ROLE_MAPPING_ROTATION,
                                                         *effect) :
                                                     nullptr;
      bNodeSocket *effect_center = (center_iface != nullptr) ?
                                       source_group_mapping_input_socket(
                                           tree, group_input, center_iface->identifier) :
                                       nullptr;
      bNodeSocket *effect_offset = (offset_iface != nullptr) ?
                                       source_group_mapping_input_socket(
                                           tree, group_input, offset_iface->identifier) :
                                       nullptr;
      bNodeSocket *effect_scale = (scale_iface != nullptr) ?
                                      source_group_mapping_input_socket(
                                          tree, group_input, scale_iface->identifier) :
                                      nullptr;
      bNodeSocket *effect_rotation = (rotation_iface != nullptr) ?
                                         source_group_mapping_input_socket(
                                             tree, group_input, rotation_iface->identifier) :
                                         nullptr;
      if ((!mirror && (effect_offset == nullptr || effect_scale == nullptr ||
                       effect_rotation == nullptr)) ||
          (mirror && effect_center == nullptr))
      {
        /* The inputs a relay never wired would read identity defaults; leave the effect out. */
        continue;
      }
      const MappingChain chain = mapping_chain_add(tree,
                                                   *uv_node,
                                                   *uv_socket,
                                                   &group_input,
                                                   effect_offset,
                                                   effect_scale,
                                                   effect_rotation,
                                                   &group_input,
                                                   effect_center,
                                                   axes);
      if (chain.mapping == nullptr) {
        effects_failed = true;
        break;
      }
      made.append(chain.rotate);
      made.append(chain.mapping);
      for (bNode *fold : chain.fold_nodes) {
        made.append(fold);
      }
      /* Each chain sits on the source's line, after the ones before it. */
      for (bNode *fold : chain.fold_nodes) {
        fold->location[0] = place_x;
        fold->location[1] = source->location[1];
        place_x += 80.0f;
      }
      chain.rotate->location[0] = place_x;
      chain.rotate->location[1] = source->location[1];
      place_x += 80.0f;
      chain.mapping->location[0] = place_x;
      chain.mapping->location[1] = source->location[1];
      place_x += 80.0f;
      uv_node = chain.mapping;
      uv_socket = socket_out(*chain.mapping, "Vector");
      if (uv_socket == nullptr) {
        effects_failed = true;
        break;
      }
    }
    if (effects_failed) {
      return discard_made();
    }

    const MappingChain chain = mapping_chain_add(tree,
                                                 *uv_node,
                                                 *uv_socket,
                                                 &group_input,
                                                 offset_out,
                                                 scale_out,
                                                 rotation_out,
                                                 &group_input,
                                                 center_out,
                                                 mirror_axes);
    if (chain.mapping == nullptr) {
      return discard_made();
    }
    bNode *rotate = chain.rotate;
    bNode *mapping = chain.mapping;
    bNodeSocket *map_out = socket_out(*mapping, "Vector");
    if (map_out == nullptr) {
      for (bNode *fold : chain.fold_nodes) {
        bke::node_remove_node(nullptr, tree, *fold, true, false);
      }
      bke::node_remove_node(nullptr, tree, *mapping, true, false);
      bke::node_remove_node(nullptr, tree, *rotate, true, false);
      return discard_made();
    }
    /* The fold's nodes sit between the UV source and the rotate; place them on the same line. */
    for (bNode *fold : chain.fold_nodes) {
      fold->location[0] = place_x;
      fold->location[1] = source->location[1];
      place_x += 80.0f;
    }
    rotate->location[0] = place_x;
    rotate->location[1] = source->location[1];
    mapping->location[0] = place_x + 80.0f;
    mapping->location[1] = source->location[1];
    made.append(rotate);
    made.append(mapping);
    for (bNode *fold : chain.fold_nodes) {
      made.append(fold);
    }

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
 * The row-mapping half of a mapped level: inputs (the row's own and one set per transform
 * effect), implicit UVs, Repeat, and the chains. Returns whether a chain was actually inserted,
 * so a source with no UV reader is not treated as mapped by the clip factor or the Normal remap.
 * \a mirror_axes names the axes the row's Mirror folds; the chain then reads the fold center from
 * the wrapper's fourth input. \a effects are the row's transform effects whose mapping remaps, in
 * list order, each with its own inputs and its chain ahead of the row's own.
 */
bool source_group_mapping_prepare(bNodeTree &tree,
                                  const int16_t mirror_axes,
                                  const Span<const MaterialPaintLayer *> effects)
{
  source_group_implicit_uv_wire(tree);
  if (!source_group_mapping_interface_ensure(tree, effects)) {
    return false;
  }
  bNode *group_input = source_group_group_input_ensure(tree);
  if (group_input == nullptr) {
    return false;
  }
  nodes::update_node_declaration_and_sockets(tree, *group_input);
  const int inserted = source_group_mapping_insert(tree, *group_input, mirror_axes, effects);
  if (inserted > 0) {
    /* Tiling semantics only where the UV actually moves; a texture on Generated/Object
     * coordinates keeps the source's own extension. */
    source_group_image_textures_repeat(tree);
  }
  return inserted > 0;
}

/**
 * Relay the parent's row-mapping inputs into the child path \a instance, each side found by role
 * on its own interface: the row's own four, and every transform effect's set, found by their
 * shared role prefix. An unlinked input leaves the child's Mapping at its identity default.
 */
void source_group_mapping_instance_wire(bNodeTree &tree,
                                               bNode &group_input,
                                               bNode &instance)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    const char *socket_role = prop_string_get(socket->properties, INPUT_ROLE_PROP);
    if (socket_role == nullptr || !BLI_str_startswith(socket_role, "source_mapping_")) {
      continue;
    }
    mapping_input_relay(
        tree, group_input, socket, instance, socket_role);
  }
}

/**
 * The Mirror fold's side-sign output of \a tree's mapping chain, or null when the level folds
 * nothing. \a r_node, when asked, takes the socket's owner node. Found by the chain marker, not by
 * name: the generated nodes carry dot-prefixed names the value sync must not match against a
 * source node.
 */
bNodeSocket *source_group_side_sign_find(bNodeTree &tree, bNode **r_node)
{
  for (bNode &node : tree.nodes) {
    const char *tag = prop_string_get(node.prop, MAPPING_CHAIN_PROP);
    if (tag != nullptr && STREQ(tag, "mirror_side")) {
      if (r_node != nullptr) {
        *r_node = &node;
      }
      return socket_out(node, "Vector");
    }
  }
  if (r_node != nullptr) {
    *r_node = nullptr;
  }
  return nullptr;
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
