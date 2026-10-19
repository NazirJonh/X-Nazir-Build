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
#include <optional>

#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
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
static constexpr uint64_t SOURCE_GROUP_FILL_VERSION = 3;

uint64_t source_group_content_hash(const Material &source)
{
  uint64_t hash = BKE_paint_layers_source_material_tree_hash_cached(source);
  hash = hash * 1000003u ^ SOURCE_GROUP_FILL_VERSION;
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
 * own output is the decoded vector); any other source, a Bump with relief for one, is encoded
 * through `0.5 * n + 0.5`, the single Vector Math the bake uses (`vector_encode_node_add` in
 * `render_material_bake.cc`), so a live row and its render see one source the same way. Nothing is
 * exposed when the Normal is not fed by a node of this tree or the Normal Map's colour is a
 * constant: the map rendered from the source stands in.
 */
static void source_group_normal_output_add(bNodeTree &tree,
                                           const bNode &principled,
                                           const Map<const bNode *, bNode *> &node_map,
                                           const Map<const bNodeSocket *, bNodeSocket *> &socket_map,
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
    if (node != nullptr && socket != nullptr) {
      r_outputs.append({int(PAINT_MATERIAL_CHANNEL_NORMAL), node, socket, name, type});
    }
    return;
  }
  bNode *source_node = node_map.lookup_default(fed.node, nullptr);
  bNodeSocket *source_socket = socket_map.lookup_default(fed.socket, nullptr);
  bNode *encode = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
  if (source_node == nullptr || source_socket == nullptr || encode == nullptr) {
    return;
  }
  encode->custom1 = NODE_VECTOR_MATH_MULTIPLY_ADD;
  bNodeSocket *encode_in = bke::node_find_socket(*encode, SOCK_IN, "Vector"_ustr);
  bNodeSocket *multiplier = bke::node_find_socket(*encode, SOCK_IN, "Vector_001"_ustr);
  bNodeSocket *addend = bke::node_find_socket(*encode, SOCK_IN, "Vector_002"_ustr);
  bNodeSocket *encode_out = bke::node_find_socket(*encode, SOCK_OUT, "Vector"_ustr);
  if (encode_in == nullptr || multiplier == nullptr || addend == nullptr ||
      encode_out == nullptr)
  {
    return;
  }
  for (bNodeSocket *socket : {multiplier, addend}) {
    if (socket->default_value != nullptr) {
      copy_v3_fl(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, 0.5f);
    }
  }
  bke::node_add_link(tree, *source_node, *source_socket, *encode, *encode_in);
  r_outputs.append({int(PAINT_MATERIAL_CHANNEL_NORMAL), encode, encode_out, name, type});
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
    source_group_normal_output_add(tree, build.principled, node_map, socket_map, outputs);
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
  return source_group_outputs_wire(tree, outputs);
}

bool source_group_fill(Main &bmain, bNodeTree &wrapper, const Material &source)
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
  const SourceGroupBuild build{bmain, *principled, group_path, owner_uid, source.id.name + 2};
  return done(source_group_level_build(build, wrapper, *source.nodetree, 0));
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

}  // namespace blender::bke::paint_layers
