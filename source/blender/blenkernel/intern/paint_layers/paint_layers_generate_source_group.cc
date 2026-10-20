/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The group a Material row instantiates to show its source's shader graph live: a copy of the
 * source's nodes without its Principled BSDF and Material Output, with one output per paint
 * channel that a node of the source feeds. The row's chain reads those outputs where a painted row
 * reads a map, so the source's graph is evaluated by the shader in place of a render of it.
 *
 * The group is derived data like the layer groups: owned by the material, rebuilt in place when
 * the source's content moves, deleted when no row uses it. When the source reaches its Principled
 * BSDF through groups, every group on that path is copied privately too, so each level can expose
 * the channels one level up; the groups beside the path are shared, not copied.
 */

#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>

#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_resolve.hh"

#include "DEG_depsgraph.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"

namespace blender::bke::paint_layers {

/** Bumped when what a fill produces changes, so groups made by an older fill are rebuilt once. */
static constexpr uint64_t SOURCE_GROUP_FILL_VERSION = 6;

uint64_t source_group_content_hash(const Material &source, const bool mapped)
{
  uint64_t hash = BKE_paint_layers_source_material_tree_hash_cached(source);
  hash = hash * 1000003u ^ SOURCE_GROUP_FILL_VERSION;
  /* Whether the wrapper carries the row mapping is structural, but it lives in the content hash
   * (with the version) so toggling it refills the group once. Slider values do not enter here, so
   * dragging a mapping value never refills. */
  hash = hash * 1000003u ^ (mapped ? 1u : 0u);
  return hash;
}

uint64_t source_group_structure_hash(const Material &source, const bool mapped)
{
  uint64_t hash = BKE_paint_layers_source_structure_hash_cached(source);
  hash = hash * 1000003u ^ SOURCE_GROUP_FILL_VERSION;
  hash = hash * 1000003u ^ (mapped ? 1u : 0u);
  return hash;
}

/** The node and socket of the source tree that feed an input, or an empty pair. */
struct FedSocket {
  const bNode *node = nullptr;
  const bNodeSocket *socket = nullptr;
};

/**
 * What feeds \a input after Reroutes and muted nodes, when it is a node of the same tree: the copy
 * can only link sockets of the tree it copies, so a graph reached through another Group node reads
 * the map rendered from it instead. Empty when nothing feeds the input.
 */
static FedSocket source_group_follow_source(const bNodeSocket &input)
{
  Vector<const bNode *> group_path;
  const bNodeSocket *fed = BKE_paint_material_source_socket(input, &group_path);
  if (fed == nullptr || !group_path.is_empty()) {
    return {};
  }
  return {&fed->owner_node(), fed};
}

/** The nodes and links of \a src copied into \a dst but for the output nodes and \a skip. */
static bool source_group_copy_nodes(bNodeTree &dst,
                                    const bNodeTree &src,
                                    const bNode *skip,
                                    Map<const bNode *, bNode *> &r_node_map,
                                    Map<const bNodeSocket *, bNodeSocket *> &r_socket_map)
{
  for (const bNode &node : src.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL || &node == skip) {
      continue;
    }
    bNode *copy = bke::node_copy_with_mapping(
        &dst, node, 0, std::nullopt, std::nullopt, r_socket_map);
    if (copy == nullptr) {
      return false;
    }
    r_node_map.add(&node, copy);
  }
  /* The copy does not remap a Frame parent, so a node would keep a pointer into the source tree; a
   * parent outside the copied set becomes none. */
  for (const auto item : r_node_map.items()) {
    item.value->parent = r_node_map.lookup_default(item.key->parent, nullptr);
  }
  for (const bNodeLink &link : src.links) {
    bNode *from = r_node_map.lookup_default(link.fromnode, nullptr);
    bNode *to = r_node_map.lookup_default(link.tonode, nullptr);
    bNodeSocket *from_socket = r_socket_map.lookup_default(link.fromsock, nullptr);
    bNodeSocket *to_socket = r_socket_map.lookup_default(link.tosock, nullptr);
    if (from == nullptr || to == nullptr || from_socket == nullptr || to_socket == nullptr) {
      continue;
    }
    bke::node_add_link(dst, *from, *from_socket, *to, *to_socket);
  }
  for (const auto item : r_node_map.items()) {
    bke::node_declaration_ensure(dst, *item.value);
  }
  return true;
}

/** What a level of the wrapper is built from: the source, and the groups leading to its Principled. */
struct SourceGroupBuild {
  Main &bmain;
  const bNode &principled;
  /** The group instance nodes from the source's tree down to the Principled's own group. */
  const Vector<const bNode *> &path;
  const bUUID &owner_uid;
  const char *source_name;
  /** Whether the row applies its UV mapping, so every level carries the mapping chain. */
  bool mapped;
};

/** One channel a level hands up: the node of the level's copy that feeds it. */
struct SourceGroupOutput {
  int channel;
  bNode *node;
  bNodeSocket *socket;
  const char *name;
  const char *socket_type;
};

/**
 * A new private copy of the group \a orig, with the interface of \a orig so the links into its
 * instance node and its Group Input stay valid, and no body yet. The instance node that takes it
 * is its only user.
 */
static bNodeTree *source_group_path_tree_add(const SourceGroupBuild &build, const bNodeTree &orig)
{
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, ".PL Source %s %s", build.source_name, orig.id.name + 2);
  bNodeTree *tree = bke::node_tree_add_tree(&build.bmain, name, "ShaderNodeTree");
  if (tree == nullptr) {
    return nullptr;
  }
  id_us_min(&tree->id);
  tree_owner_uid_set(*tree, build.owner_uid);
  prop_int_set(tree->id.properties, TREE_SOURCE_PATH_PROP, 1);
  /* The Group Input of the copied body and the instance's sockets are made from the interface, and
   * a fresh tree only has an empty one. */
  tree->tree_interface.free_data();
  tree->tree_interface.copy_data(orig.tree_interface, 0);
  return tree;
}

/** The Group Output \a tree has, or a new one for the material tree the root is copied from. */
static bNode *source_group_group_output(bNodeTree &tree)
{
  tree.ensure_topology_cache();
  if (bNode *existing = const_cast<bNode *>(tree.group_output_node())) {
    return existing;
  }
  return bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
}

/** Expose \a outputs on the interface of \a tree and link them to its Group Output. */
static bool source_group_outputs_wire(bNodeTree &tree, const Span<SourceGroupOutput> outputs)
{
  bNode *group_output = source_group_group_output(tree);
  if (group_output == nullptr) {
    return false;
  }
  /* An output of an earlier fill is reused, so the identifier the instances link into survives a
   * refill that changed no structure; one the source no longer feeds is dropped. */
  tree.ensure_interface_cache();
  Vector<bNodeTreeInterfaceSocket *> existing;
  for (bNodeTreeInterfaceSocket *interface : tree.interface_outputs()) {
    if (prop_int_get(interface->properties, INPUT_CHANNEL_PROP, -1) >= 0) {
      existing.append(interface);
    }
  }
  Vector<bNodeTreeInterfaceSocket *> interfaces;
  for (const SourceGroupOutput &output : outputs) {
    bNodeTreeInterfaceSocket *interface = nullptr;
    for (bNodeTreeInterfaceSocket *candidate : existing) {
      if (prop_int_get(candidate->properties, INPUT_CHANNEL_PROP, -1) == output.channel) {
        interface = candidate;
        break;
      }
    }
    if (interface == nullptr) {
      interface = tree.tree_interface.add_socket(
          output.name, "", output.socket_type, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
      if (interface == nullptr) {
        return false;
      }
      prop_int_set(interface->properties, INPUT_CHANNEL_PROP, output.channel);
    }
    interfaces.append(interface);
  }
  for (bNodeTreeInterfaceSocket *stale : existing) {
    if (!interfaces.contains(stale)) {
      tree.tree_interface.remove_item(reinterpret_cast<bNodeTreeInterfaceItem &>(*stale));
    }
  }
  /* The output node only has the sockets of the interface once the declaration is updated. */
  nodes::update_node_declaration_and_sockets(tree, *group_output);
  for (const int64_t i : outputs.index_range()) {
    bNodeSocket *to = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline(interfaces[i]->identifier));
    if (to == nullptr) {
      return false;
    }
    bke::node_add_link(tree, *outputs[i].node, *outputs[i].socket, *group_output, *to);
  }
  return true;
}

/**
 * Expose the Normal of \a principled the way the chain and the bake read it: as the encoded
 * tangent-space map. A Normal Map node contributes its Color input, which is already encoded (its
 * own output is the decoded vector). Nothing is exposed for any other source or when the Normal
 * Map's colour is a constant: the map rendered from the source stands in.
 */
static void source_group_normal_output_add(const bNode &principled,
                                           bNodeTree &tree,
                                           const Map<const bNode *, bNode *> &node_map,
                                           const Map<const bNodeSocket *, bNodeSocket *> &socket_map,
                                           const bool mapped,
                                           Vector<SourceGroupOutput> &r_outputs)
{
  const bNodeSocket *input = bke::node_find_socket(principled, SOCK_IN, "Normal"_ustr);
  if (input == nullptr) {
    return;
  }
  const FedSocket fed = source_group_follow_source(*input);
  if (fed.node == nullptr) {
    return;
  }
  const char *name = "Normal";
  const char *type = "NodeSocketVector";
  if (fed.node->type_legacy == SH_NODE_NORMAL_MAP) {
    const bNodeSocket *color = bke::node_find_socket(*fed.node, SOCK_IN, "Color"_ustr);
    const FedSocket color_fed = (color != nullptr) ? source_group_follow_source(*color) :
                                                      FedSocket{};
    bNode *node = node_map.lookup_default(color_fed.node, nullptr);
    bNodeSocket *socket = socket_map.lookup_default(color_fed.socket, nullptr);
    if (node == nullptr || socket == nullptr) {
      return;
    }
    if (mapped && source_group_color_reads_mapped_uv(tree, *node)) {
      /* The row mapping moved the texture's read point, so the encoded vectors follow it: remap
       * them with this level's mapping rotation and scale-sign before exposing the Color. A
       * colour that does not read the moved UV (Generated coordinates, a named UV Map) stays. */
      bNode *group_input = source_group_group_input_ensure(tree);
      if (group_input != nullptr) {
        bNodeSocket *rotation = source_group_mapping_input_socket(
            tree, *group_input, SOURCE_GROUP_ROLE_MAPPING_ROTATION);
        bNodeSocket *scale = source_group_mapping_input_socket(
            tree, *group_input, SOURCE_GROUP_ROLE_MAPPING_SCALE);
        Vector<bNode *> made;
        bNode *encode_node = nullptr;
        if (rotation != nullptr && scale != nullptr) {
          if (bNodeSocket *remapped = normal_remap_nodes_add(
                  tree, *node, *socket, group_input, rotation, group_input, scale, made, encode_node))
          {
            node = encode_node;
            socket = remapped;
          }
        }
      }
    }
    r_outputs.append({int(PAINT_MATERIAL_CHANNEL_NORMAL), node, socket, name, type});
    return;
  }
  /* Any other source (a Bump for one) is left to the map rendered from it: such a normal comes
   * from screen-space derivatives, so evaluated live it changes with the camera instead of being a
   * property of the surface. */
}

/**
 * Copy \a orig into \a tree and expose the paint channels the source feeds: at the level of the
 * Principled BSDF straight from the nodes that feed its inputs, above it from the instance of the
 * private copy of the next group on the path, which exposes them the same way.
 */
static bool source_group_level_build(const SourceGroupBuild &build,
                                     bNodeTree &tree,
                                     const bNodeTree &orig,
                                     const int depth)
{
  orig.ensure_topology_cache();
  const bool innermost = depth == int(build.path.size());
  Map<const bNode *, bNode *> node_map;
  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  if (!source_group_copy_nodes(
          tree, orig, innermost ? &build.principled : nullptr, node_map, socket_map))
  {
    return false;
  }

  if (build.mapped) {
    /* Every level: the three mapping inputs, the implicit UV wiring and the chain in front of
     * every UV output. Path copies are fresh, so a mapped build always starts from no inputs. */
    source_group_mapping_prepare(tree);
  }
  else if (depth == 0) {
    /* The root's interface survives a refill, so inputs left by an earlier mapped build go now. */
    source_group_mapping_interface_remove(tree);
  }

  Vector<SourceGroupOutput> outputs;
  if (innermost) {
    const auto output_add = [&](const int channel,
                                const char *socket_name,
                                const char *ui_name,
                                const char *socket_type) {
      const bNodeSocket *input = bke::node_find_socket(
          build.principled, SOCK_IN, UString::from_ptr_noinline(socket_name));
      if (input == nullptr) {
        return;
      }
      const FedSocket fed = source_group_follow_source(*input);
      bNode *node = node_map.lookup_default(fed.node, nullptr);
      bNodeSocket *socket = socket_map.lookup_default(fed.socket, nullptr);
      if (node == nullptr || socket == nullptr) {
        return;
      }
      outputs.append({channel, node, socket, ui_name, socket_type});
    };
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (info.socket_name == nullptr || info.channel == PAINT_MATERIAL_CHANNEL_NORMAL ||
          info.channel == PAINT_MATERIAL_CHANNEL_ALPHA)
      {
        continue;
      }
      output_add(int(info.channel),
                 info.socket_name,
                 info.ui_name,
                 info.is_color ? "NodeSocketColor" : "NodeSocketFloat");
    }
    output_add(int(PAINT_MATERIAL_CHANNEL_ALPHA), "Alpha", "Coverage", "NodeSocketFloat");
    /* A chain inserted at a parent level may feed this level's Normal through a group input, so
     * the row's `mapped` flag (not only this level's local chain) decides the remap. */
    source_group_normal_output_add(
        build.principled, tree, node_map, socket_map, build.mapped, outputs);
  }
  else {
    const bNode *orig_group = build.path[depth];
    bNode *instance = node_map.lookup_default(orig_group, nullptr);
    if (instance == nullptr || orig_group->id == nullptr || GS(orig_group->id->name) != ID_NT) {
      return false;
    }
    const bNodeTree &orig_child = *id_cast<const bNodeTree *>(orig_group->id);
    bNodeTree *child = source_group_path_tree_add(build, orig_child);
    if (child == nullptr) {
      return false;
    }
    if (instance->id != nullptr) {
      id_us_min(instance->id);
    }
    instance->id = &child->id;
    id_us_plus(&child->id);
    if (!source_group_level_build(build, *child, orig_child, depth + 1)) {
      return false;
    }
    /* The copy is a tree of its own: its sockets and interface cache are brought current here, as
     * the ones the generator hands out are. */
    BKE_ntree_update_tag_all(child);
    BKE_ntree_update_after_single_tree_change(build.bmain, *child);
    /* The instance only sees the outputs the child gained after a declaration update. */
    nodes::update_node_declaration_and_sockets(tree, *instance);
    if (build.mapped) {
      /* Relay this level's mapping values into the child path instance, the way the row relays
       * its values into the wrapper. */
      bNode *group_input = source_group_group_input_ensure(tree);
      if (group_input != nullptr) {
        nodes::update_node_declaration_and_sockets(tree, *group_input);
        source_group_mapping_instance_wire(tree, *group_input, *instance);
      }
    }
    child->ensure_interface_cache();
    for (const bNodeTreeInterfaceSocket *interface : child->interface_outputs()) {
      const int channel = prop_int_get(interface->properties, INPUT_CHANNEL_PROP, -1);
      if (channel < 0 || interface->identifier == nullptr) {
        continue;
      }
      bNodeSocket *socket = bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(interface->identifier));
      if (socket != nullptr) {
        outputs.append({channel, instance, socket, interface->name, interface->socket_type});
      }
    }
  }
  if (outputs.is_empty()) {
    return false;
  }
  if (depth == 0) {
    /* Which channels the wrapper exposes: a channel missing here is read from its render, or not
     * at all when none is shown. */
    printf("[paint-layers source group] '%s' exposes channels:", build.source_name);
    for (const SourceGroupOutput &output : outputs) {
      printf(" %d", output.channel);
    }
    printf("\n");
  }
  return source_group_outputs_wire(tree, outputs);
}

bool source_group_fill(Main &bmain, bNodeTree &wrapper, const Material &source, const bool mapped)
{
  /* The interface stays: the instances of the group link into its outputs, and a refill that
   * changes no structure must not move them. */
  tree_clear_nodes(bmain, wrapper);
  const bUUID owner_uid = tree_owner_uid_get(wrapper);
  const auto done = [&](const bool ok) {
    if (!ok) {
      tree_clear(bmain, wrapper);
    }
    /* The copies the cleared body held, and those of a failed build, have no instance any more. */
    source_group_paths_prune(bmain, owner_uid);
    return ok;
  };
  if (source.nodetree == nullptr) {
    return done(false);
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(source, reason, &group_path);
  if (principled == nullptr || int(group_path.size()) > SOURCE_GROUP_MAX_DEPTH) {
    return done(false);
  }
  const SourceGroupBuild build{
      bmain, *principled, group_path, owner_uid, source.id.name + 2, mapped};
  return done(source_group_level_build(build, wrapper, *source.nodetree, 0));
}

void source_group_structure_hash_set(bNodeTree &wrapper, const uint64_t hash)
{
  tree_hash_set(wrapper, TREE_SOURCE_STRUCTURE_LOW_PROP, TREE_SOURCE_STRUCTURE_HIGH_PROP, hash);
}

bool source_group_structure_hash_get(bNodeTree &wrapper, uint64_t &r_hash)
{
  return tree_hash_get(
      wrapper, TREE_SOURCE_STRUCTURE_LOW_PROP, TREE_SOURCE_STRUCTURE_HIGH_PROP, r_hash);
}

/** Give the sockets of \a copy the default values of the ones of \a src, found by identifier. */
template<typename SocketList>
static bool source_group_node_values_sync(bNodeTree &tree,
                                          bNode &copy,
                                          const SocketList &src_sockets,
                                          const eNodeSocketInOut in_out)
{
  for (const bNodeSocket &src_socket : src_sockets) {
    bNodeSocket *socket = bke::node_find_socket(
        copy, in_out, UString::from_ptr_noinline(src_socket.identifier));
    if (socket == nullptr || socket->type != src_socket.type) {
      return false;
    }
    if (socket->default_value == nullptr || src_socket.default_value == nullptr) {
      continue;
    }
    node_socket_copy_default_value_data(
        eNodeSocketDatatype(socket->type), socket->default_value, src_socket.default_value);
    BKE_ntree_update_tag_socket_property(&tree, socket);
  }
  return true;
}

/** The counterpart of #source_group_level_build: values only, along the same path of groups. */
static bool source_group_level_sync(bNodeTree &tree,
                                    const bNodeTree &orig,
                                    const bNode &principled,
                                    const Vector<const bNode *> &path,
                                    const int depth,
                                    Vector<bNodeTree *> &r_trees)
{
  const bool innermost = depth == int(path.size());
  r_trees.append(&tree);
  for (const bNode &node : orig.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL || (innermost && &node == &principled)) {
      continue;
    }
    bNode *copy = bke::node_find_node_by_name(tree, node.name);
    if (copy == nullptr || copy->type_legacy != node.type_legacy ||
        !source_group_node_values_sync(tree, *copy, node.inputs, SOCK_IN) ||
        !source_group_node_values_sync(tree, *copy, node.outputs, SOCK_OUT))
    {
      return false;
    }
  }
  if (innermost) {
    return true;
  }
  const bNode *orig_group = path[depth];
  bNode *instance = bke::node_find_node_by_name(tree, orig_group->name);
  if (instance == nullptr || instance->id == nullptr || orig_group->id == nullptr ||
      GS(orig_group->id->name) != ID_NT)
  {
    return false;
  }
  return source_group_level_sync(*id_cast<bNodeTree *>(instance->id),
                                 *id_cast<const bNodeTree *>(orig_group->id),
                                 principled,
                                 path,
                                 depth + 1,
                                 r_trees);
}

bool source_group_values_sync(Main &bmain, bNodeTree &wrapper, const Material &source)
{
  if (source.nodetree == nullptr) {
    return false;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(source, reason, &group_path);
  if (principled == nullptr || int(group_path.size()) > SOURCE_GROUP_MAX_DEPTH) {
    return false;
  }
  Vector<bNodeTree *> trees;
  if (!source_group_level_sync(wrapper, *source.nodetree, *principled, group_path, 0, trees)) {
    return false;
  }
  for (bNodeTree *tree : trees) {
    BKE_ntree_update_after_single_tree_change(bmain, *tree);
    DEG_id_tag_update(&tree->id, ID_RECALC_SYNC_TO_EVAL);
  }
  return true;
}

void source_group_paths_prune(Main &bmain, const bUUID &owner_uid)
{
  /* Deleting a copy frees its instance nodes, which leaves the copy below it without a user, so
   * the passes peel one level each. */
  for (int pass = 0; pass <= SOURCE_GROUP_MAX_DEPTH; pass++) {
    Vector<bNodeTree *> orphans;
    for (bNodeTree &tree : bmain.nodetrees) {
      if (prop_int_get(tree.id.properties, TREE_SOURCE_PATH_PROP, 0) != 0 &&
          BLI_uuid_equal(tree_owner_uid_get(tree), owner_uid) && ID_REAL_USERS(&tree.id) <= 0)
      {
        orphans.append(&tree);
      }
    }
    if (orphans.is_empty()) {
      break;
    }
    for (bNodeTree *tree : orphans) {
      tree_clear_nodes(bmain, *tree);
      BKE_id_delete(&bmain, tree);
    }
  }
}

bNodeSocket *source_group_output(const bNodeTree &wrapper, bNode &instance, const int channel)
{
  wrapper.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *interface : wrapper.interface_outputs()) {
    if (prop_int_get(interface->properties, INPUT_CHANNEL_PROP, -1) == channel &&
        interface->identifier != nullptr)
    {
      return bke::node_find_socket(
          instance, SOCK_OUT, UString::from_ptr_noinline(interface->identifier));
    }
  }
  return nullptr;
}

bool source_group_normal_live(const Material &source)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(source, reason, &group_path);
  if (principled == nullptr) {
    return false;
  }
  const bNodeSocket *input = bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr);
  if (input == nullptr) {
    return false;
  }
  const FedSocket fed = source_group_follow_source(*input);
  if (fed.node == nullptr || fed.node->type_legacy != SH_NODE_NORMAL_MAP) {
    return false;
  }
  const bNodeSocket *color = bke::node_find_socket(*fed.node, SOCK_IN, "Color"_ustr);
  return color != nullptr && source_group_follow_source(*color).node != nullptr;
}

}  // namespace blender::bke::paint_layers
