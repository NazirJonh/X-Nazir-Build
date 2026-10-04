#include "paint_layers_generate_intern.hh"
/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The paint-layer generator: `Material::paint_layers` (the DNA description) becomes a node tree.
 *
 * `paint_layers_tree_build` is the topology half: interface, per-layer nodes and the links between
 * them, with no #Main involved. `BKE_paint_layers_regenerate` is the #Main-side half: it owns the
 * generated group, the instance node in the material's embedded tree, and the routing into the
 * Principled BSDF.
 *
 * Topology and values are separated: the channel chains and the interface are a function of the
 * description's *structure* alone, while the animatable values (`opacity`, `enabled`, a Fill
 * constant) are inputs of each layer group's own interface. Their current values sit on that
 * group's instance node in its parent tree and are copied there by #BKE_paint_layers_values_sync
 * (called from the material evaluation too).
 */

#include "BKE_paint_layers_debug.hh"
#include "BKE_paint_layers_generate.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "IMB_colormanagement.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "paint_layers_intern.hh"

#include "NOD_socket.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "paint_material_composite_internal.hh"


namespace blender {
namespace bke::paint_layers {

/** The `COLOR:<CHANNEL>` / `COVERAGE` role string a wrapper output carries for \a channel. */
const char *source_group_socket_type(const eMaterialPaintChannel channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return "NodeSocketVector";
  }
  return BKE_paint_material_channel_info(channel).is_color ? "NodeSocketColor" : "NodeSocketFloat";
}

/** The longest group path the wrapper walks before refusing; guards corrupt data. */
constexpr int PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH = 8;

/**
 * Add or find an output socket by name, reusing the existing one so its identifier survives. The
 * role is set only when non-null: `COLOR:<CHANNEL>`/`COVERAGE` is the wrapper's outward contract
 * and belongs on the root alone, never on the intermediate group copies.
 */
bNodeTreeInterfaceSocket *source_group_interface_output(bNodeTree &group,
                                                        const char *name,
                                                        const char *socket_type,
                                                        const char *role)
{
  group.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, name)) {
      if (role != nullptr) {
        prop_string_set(socket->properties, PAINT_LAYERS_CUSTOM_ROLE_PROP, role);
      }
      return socket;
    }
  }
  bNodeTreeInterfaceSocket *socket = group.tree_interface.add_socket(
      name, "", socket_type, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  if (socket != nullptr && role != nullptr) {
    prop_string_set(socket->properties, PAINT_LAYERS_CUSTOM_ROLE_PROP, role);
  }
  return socket;
}

bNodeTreeInterfaceSocket *source_group_interface_output_find(bNodeTree &group, const char *name)
{
  group.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, name)) {
      return socket;
    }
  }
  return nullptr;
}

/** One channel the wrapper exposes: a colour channel, or the coverage from Alpha. */
struct SourceGroupChannel {
  int channel;
  /** The Principled input the value is read from ("Base Color", "Alpha", ...). */
  const char *principled_socket;
  /** The display name the root wrapper's output socket takes. */
  const char *ui_name;
  bool coverage;
};

Vector<SourceGroupChannel> source_group_channels(const bNode &principled)
{
  Vector<SourceGroupChannel> channels;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.socket_name == nullptr) {
      continue;
    }
    if (bke::node_find_socket(const_cast<bNode &>(principled),
                              SOCK_IN,
                              UString::from_ptr_noinline(info.socket_name)) != nullptr)
    {
      channels.append({int(info.channel), info.socket_name, info.ui_name, false});
    }
  }
  if (bke::node_find_socket(const_cast<bNode &>(principled), SOCK_IN, "Alpha"_ustr) != nullptr) {
    channels.append({int(PAINT_MATERIAL_CHANNEL_ALPHA), "Alpha", "Coverage", true});
  }
  return channels;
}

/** The deterministic output name of \a spec: the contract name at the root, a private one below. */
void source_group_output_name(const SourceGroupChannel &spec,
                              const bool is_root,
                              char r_name[160])
{
  if (is_root) {
    BLI_snprintf(r_name, 160, "%s", spec.ui_name);
  }
  else {
    BLI_snprintf(r_name, 160, ".PL %s", spec.ui_name);
  }
}

/** The outward role string of \a spec, written into \a r_role. */
const char *source_group_role(const SourceGroupChannel &spec, char r_role[128])
{
  if (spec.coverage) {
    BLI_snprintf(r_role, 128, "COVERAGE");
    return r_role;
  }
  const char *identifier = BKE_paint_layers_custom_channel_identifier(spec.channel);
  BLI_snprintf(r_role, 128, "COLOR:%s", identifier != nullptr ? identifier : "");
  return r_role;
}

/** Copy \a src's default into \a dst when they are the same socket type. */
void socket_default_copy(bNodeSocket &dst, const bNodeSocket &src)
{
  if (dst.default_value == nullptr || src.default_value == nullptr || dst.type != src.type) {
    return;
  }
  switch (src.type) {
    case SOCK_FLOAT:
      *static_cast<bNodeSocketValueFloat *>(dst.default_value) =
          *static_cast<const bNodeSocketValueFloat *>(src.default_value);
      break;
    case SOCK_RGBA:
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(dst.default_value)->value,
                 static_cast<const bNodeSocketValueRGBA *>(src.default_value)->value);
      break;
    case SOCK_VECTOR:
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(dst.default_value)->value,
                 static_cast<const bNodeSocketValueVector *>(src.default_value)->value);
      break;
    default:
      break;
  }
}

/** Copy \a src's nodes and links into \a dst (cleared first), mapping originals to copies. */
bool source_group_copy_nodes(Main &bmain,
                             bNodeTree &dst,
                             const bNodeTree &src,
                             Map<const bNode *, bNode *> &r_node_map)
{
  tree_clear_nodes(bmain, dst);
  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  for (const bNode &node : src.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL) {
      continue;
    }
    bNode *copy = bke::node_copy_with_mapping(
        &dst, node, 0, std::nullopt, std::nullopt, socket_map);
    if (copy == nullptr) {
      return false;
    }
    r_node_map.add(&node, copy);
  }
  /* `node_copy_with_mapping` does not remap `node->parent` (a Frame node), so a copied node would
   * otherwise keep a pointer into the source tree; a later tree copy would walk it and crash. A
   * parent outside the copied set (a skipped Material Output is no parent, but be safe) becomes
   * null. Frame nodes themselves are copied -- only Material Output is skipped -- so nesting,
   * including frames inside frames, is preserved. */
  for (const auto item : r_node_map.items()) {
    item.value->parent = r_node_map.lookup_default(item.key->parent, nullptr);
  }
  for (const bNodeLink &link : src.links) {
    bNode *from = r_node_map.lookup_default(link.fromnode, nullptr);
    bNode *to = r_node_map.lookup_default(link.tonode, nullptr);
    bNodeSocket *from_socket = socket_map.lookup_default(link.fromsock, nullptr);
    bNodeSocket *to_socket = socket_map.lookup_default(link.tosock, nullptr);
    if (from == nullptr || to == nullptr || from_socket == nullptr || to_socket == nullptr) {
      continue;
    }
    bke::node_add_link(dst, *from, *from_socket, *to, *to_socket);
  }
  /* The reference copy path (#ntree_copy_data) ensures every node's declaration once the links are
   * in place; do the same so a copied node's sockets are usable in this pass. */
  for (const auto item : r_node_map.items()) {
    bke::node_declaration_ensure(dst, *item.value);
  }
  return true;
}

/** The Group Output of \a tree, created when the copied tree has none (a material root). */
bNode *source_group_group_output(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (node.is_group_output()) {
      return &node;
    }
  }
  return bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
}

/** A node and socket of the copied wrapper tree, or an empty pair when nothing feeds the input. */
struct SourceGroupSourceSocket {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
  explicit operator bool() const
  {
    return node != nullptr && socket != nullptr;
  }
};

/**
 * The socket feeding \a input after Reroutes and muted nodes, or an empty pair.
 *
 * Mirrors the resolver's own walk (`paint_material_resolve.cc`) without descending group instances:
 * the wrapper can only link sockets of the copied tree it is building, and a group directly feeding
 * the Principled is already handled by #source_group_build_level's path recursion.
 */
SourceGroupSourceSocket source_group_follow_source(const bNodeSocket &input)
{
  const bNodeSocket *current = &input;
  for (int step = 0; step < 64; step++) {
    const Span<const bNodeLink *> links = current->directly_linked_links();
    if (links.is_empty() || !links[0]->is_available() || links[0]->is_muted()) {
      return {};
    }
    const bNodeLink *link = links[0];
    if (link->fromnode->is_reroute()) {
      current = static_cast<const bNodeSocket *>(link->fromnode->inputs.first);
      continue;
    }
    if (link->fromnode->is_muted()) {
      const bNodeLink *internal = nullptr;
      for (const bNodeLink &candidate : link->fromnode->internal_links()) {
        if (candidate.tosock == link->fromsock) {
          internal = &candidate;
          break;
        }
      }
      if (internal == nullptr) {
        return {};
      }
      current = internal->fromsock;
      continue;
    }
    return {link->fromnode, link->fromsock};
  }
  return {};
}

/**
 * Add a Vector Math Multiply-Add encoding a signed vector into the [0, 1] range a normal map stores:
 * `0.5 * n + 0.5`. This is the same single node the bake's `vector_encode_node_add` uses
 * (`render_material_bake.cc`); copied rather than shared because that file lives in the editors
 * layer, which blenkernel cannot include. The caller routes the source into `Vector` and reads
 * `Vector`.
 */
bNode *source_group_normal_encode_node_add(bNodeTree &tree)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
  if (node == nullptr) {
    return nullptr;
  }
  node->custom1 = NODE_VECTOR_MATH_MULTIPLY_ADD;
  bNodeSocket *multiplier = bke::node_find_socket(*node, SOCK_IN, "Vector_001"_ustr);
  bNodeSocket *addend = bke::node_find_socket(*node, SOCK_IN, "Vector_002"_ustr);
  if (multiplier == nullptr || addend == nullptr) {
    return nullptr;
  }
  for (bNodeSocket *socket : {multiplier, addend}) {
    if (socket->default_value != nullptr) {
      copy_v3_fl(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, 0.5f);
    }
  }
  return node;
}

/**
 * Which UV readers of a source the row mapping remaps. A Texture Coordinate always; a UV Map node
 * with no layer or the owner's layer; an Attribute (Geometry) node naming the owner's layer (read
 * on `Vector`, which carries the UV). \a remap_all
 * (#MA_PAINT_LAYERS_REMAP_ALL_UV) widens the last two to every UV Map node and every Attribute
 * (Geometry) node with a non-empty name. Part of the wrapper's topology hash.
 */
struct SourceGroupUvPolicy {
  const char *uv_name;
  bool remap_all;
};

bNode *source_group_group_input_ensure(bNodeTree &tree);
bNodeTreeInterfaceSocket *source_group_mapping_input_find(bNodeTree &tree, const char *role);

/**
 * Feed \a out_in the source's Normal in the format the bake reads and the Normal chain expects: the
 * encoded tangent-space map. A Normal Map contributes its Color input (already encoded, its own
 * output is decoded); any other source -- a Bump with relief, a computed normal -- is encoded
 * through `0.5 * n + 0.5`. This mirrors the Normal branch of `channel_bake_source_socket` in
 * `render_material_bake.cc`, so a live SourceGroup row and its bake see one source the same way.
 *
 * The output stays a `NodeSocketVector`: the encoded value is three components and the consumer
 * decodes it; changing the interface type would move the wrapper's topology hash for no benefit.
 */
bool source_group_wire_normal(bNodeTree &tree,
                                     bNode &group_output,
                                     bNodeSocket &out_in,
                                     const bNodeSocket &principled_normal,
                                     const bool mapped)
{
  const SourceGroupSourceSocket source = source_group_follow_source(principled_normal);
  if (!source) {
    return false;
  }
  if (source.node->type_legacy == SH_NODE_NORMAL_MAP) {
    bNodeSocket *color = bke::node_find_socket(*source.node, SOCK_IN, "Color"_ustr);
    if (color == nullptr) {
      return false;
    }
    const SourceGroupSourceSocket color_source = source_group_follow_source(*color);
    if (color_source) {
      bNode *from_node = color_source.node;
      bNodeSocket *from_socket = color_source.socket;
      if (mapped) {
        /* The row mapping moved the texture's read point, so the encoded vectors follow it, fed
         * by this level's mapping inputs. Limitation: the correction applies to whatever reaches
         * the Normal Map's Color, so a source mixing several textures (each behind its own
         * Mapping) gets one common rotation/flip; the Bump branch below needs none. */
        bNode *group_input = source_group_group_input_ensure(tree);
        nodes::update_node_declaration_and_sockets(tree, *group_input);
        auto input_out = [&](const char *role) -> std::pair<bNode *, bNodeSocket *> {
          bNodeTreeInterfaceSocket *iface = source_group_mapping_input_find(tree, role);
          if (iface == nullptr || iface->identifier == nullptr) {
            return {nullptr, nullptr};
          }
          bNodeSocket *out = bke::node_find_socket(
              *group_input, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
          return {out != nullptr ? group_input : nullptr, out};
        };
        bNode *remap_node = nullptr;
        bNodeSocket *remap_out = normal_remap_nodes_add(
            tree,
            *color_source.node,
            *color_source.socket,
            input_out(SOURCE_GROUP_ROLE_MAPPING_ROTATION),
            input_out(SOURCE_GROUP_ROLE_MAPPING_SCALE),
            color_source.node->location[0] + 160.0f,
            color_source.node->location[1] - 240.0f,
            remap_node);
        from_node = remap_node;
        from_socket = remap_out;
      }
      bke::node_add_link(tree, *from_node, *from_socket, group_output, out_in);
      return true;
    }
    /* An unlinked Color is the encoded constant; carry its rgb to the vector output. */
    if (color->default_value != nullptr && out_in.default_value != nullptr) {
      const float *rgba = static_cast<const bNodeSocketValueRGBA *>(color->default_value)->value;
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(out_in.default_value)->value, rgba);
      return true;
    }
    return false;
  }
  bNode *encode = source_group_normal_encode_node_add(tree);
  bNodeSocket *encode_in = (encode != nullptr) ?
                               bke::node_find_socket(*encode, SOCK_IN, "Vector"_ustr) :
                               nullptr;
  bNodeSocket *encode_out = (encode != nullptr) ?
                                bke::node_find_socket(*encode, SOCK_OUT, "Vector"_ustr) :
                                nullptr;
  if (encode_in == nullptr || encode_out == nullptr) {
    return false;
  }
  bke::node_add_link(tree, *source.node, *source.socket, *encode, *encode_in);
  bke::node_add_link(tree, *encode, *encode_out, group_output, out_in);
  return true;
}

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
 * Add or find one of the wrapper's three row-mapping value inputs (ТЗ 2.2). The interface is
 * found by role before anything is added, so a rebuild keeps the socket identifiers an instance
 * of the wrapper links into; the role marker is also what keeps the value sync from copying a
 * source interface input over it. \a default_value is the identity the Mapping reads while no
 * row links the input.
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

/** The wrapper's three row-mapping value inputs, in the order the Mapping node reads them. */
static void source_group_mapping_interface_ensure(bNodeTree &tree)
{
  static const float zero[3] = {0.0f, 0.0f, 0.0f};
  static const float one[3] = {1.0f, 1.0f, 1.0f};
  char name[64];
  SNPRINTF(name, "%sOffset", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  source_group_mapping_input_ensure(tree, name, SOURCE_GROUP_ROLE_MAPPING_OFFSET, zero);
  SNPRINTF(name, "%sScale", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  source_group_mapping_input_ensure(tree, name, SOURCE_GROUP_ROLE_MAPPING_SCALE, one);
  SNPRINTF(name, "%sRotation", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  source_group_mapping_input_ensure(tree, name, SOURCE_GROUP_ROLE_MAPPING_ROTATION, zero);
}

/**
 * Drop the three row-mapping inputs again once no row applies a mapping, and re-declare the Group
 * Input so it loses their sockets. Instances of the wrapper are re-declared by the regeneration
 * (`refresh_generated_instances`), and rows that linked into them were rebuilt with the mapping
 * flag, so no link is left dangling.
 */
static void source_group_mapping_interface_remove(bNodeTree &tree)
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
  for (bNode &node : tree.nodes) {
    if (node.is_group_input()) {
      nodes::update_node_declaration_and_sockets(tree, node);
    }
  }
}

/**
 * Whether \a tree -- transitively through the group instances it instantiates -- reads the
 * object's UVs: a Texture Coordinate or UV Map node, an Image Texture with an open Vector input
 * (which samples the active UV), or a group that does any of that. This is what decides whether
 * a shared source group has to be copied privately (ТЗ 2.3): a group without UV readers stays
 * shared, its sampling is coordinate-driven and a mapping in front would be wrong.
 */
static bool source_group_attribute_names_uv(const bNode &node, const SourceGroupUvPolicy &uv)
{
  if (node.type_legacy != SH_NODE_ATTRIBUTE) {
    return false;
  }
  const NodeShaderAttribute *attr = static_cast<const NodeShaderAttribute *>(node.storage);
  if (attr == nullptr || attr->type != SHD_ATTRIBUTE_GEOMETRY || attr->name[0] == '\0') {
    return false;
  }
  return uv.remap_all || (uv.uv_name != nullptr && STREQ(attr->name, uv.uv_name));
}

static bool source_group_uv_map_is_remapped(const bNode &node, const SourceGroupUvPolicy &uv)
{
  if (node.type_legacy != SH_NODE_UVMAP) {
    return false;
  }
  const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(node.storage);
  if (storage == nullptr) {
    return false;
  }
  /* A UV Map node with no layer named reads the object's active UV, the very layer the owner's
   * name selects, so it is the row's to remap like a named match. */
  return uv.remap_all || storage->uv_map[0] == '\0' ||
         (uv.uv_name != nullptr && STREQ(storage->uv_map, uv.uv_name));
}

static bool source_group_tree_reads_uv(bNodeTree &tree, const SourceGroupUvPolicy &uv, const int depth)
{
  /* The depth guard keeps a cyclic or corrupted source from recursing forever; it matches the
   * path depth the wrapper itself accepts, so anything deeper was refused before this ran. */
  if (depth > PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH) {
    return false;
  }
  tree.ensure_topology_cache();
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_TEX_COORD || node.type_legacy == SH_NODE_UVMAP ||
        source_group_attribute_names_uv(node, uv))
    {
      return true;
    }
    if (node.type_legacy == SH_NODE_TEX_IMAGE) {
      const bNodeSocket *vector = bke::node_find_socket(node, SOCK_IN, "Vector"_ustr);
      if (vector != nullptr && !vector->is_directly_linked()) {
        return true;
      }
    }
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
      bNodeTree *child = reinterpret_cast<bNodeTree *>(node.id);
      if (child != nullptr && source_group_tree_reads_uv(*child, uv, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Insert one Mapping (Point) node in front of every UV output that feeds anything: each Texture
 * Coordinate node and the owner's UV Map node (matched by name -- a source's own other UV layers
 * are not the row's mapping to remap). The outgoing links are re-pointed through the new node,
 * read from #bNodeTree::links, not the stale per-socket caches; a source's own Mapping ends up
 * *behind* the row's one, so the row's mapping applies on top of it. The node feeds its
 * Location/Rotation/Scale from the tree's Group Input through the wrapper's mapping inputs, and
 * is named with the dot prefix so the value sync never matches it against a source node.
 */
static void source_group_mapping_insert(bNodeTree &tree, const SourceGroupUvPolicy &uv)
{
  bNode *group_input = source_group_group_input_ensure(tree);
  nodes::update_node_declaration_and_sockets(tree, *group_input);
  const float offset_default[3] = {0.0f, 0.0f, 0.0f};
  const float scale_default[3] = {1.0f, 1.0f, 1.0f};
  char name[64];
  SNPRINTF(name, "%sOffset", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  bNodeTreeInterfaceSocket *offset_iface = source_group_mapping_input_ensure(
      tree, name, SOURCE_GROUP_ROLE_MAPPING_OFFSET, offset_default);
  SNPRINTF(name, "%sScale", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  bNodeTreeInterfaceSocket *scale_iface = source_group_mapping_input_ensure(
      tree, name, SOURCE_GROUP_ROLE_MAPPING_SCALE, scale_default);
  SNPRINTF(name, "%sRotation", SOURCE_GROUP_MAPPING_INPUT_PREFIX);
  bNodeTreeInterfaceSocket *rotation_iface = source_group_mapping_input_ensure(
      tree, name, SOURCE_GROUP_ROLE_MAPPING_ROTATION, offset_default);
  if (offset_iface == nullptr || scale_iface == nullptr || rotation_iface == nullptr ||
      offset_iface->identifier == nullptr || scale_iface->identifier == nullptr ||
      rotation_iface->identifier == nullptr)
  {
    return;
  }
  bNodeSocket *offset_out = bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline(offset_iface->identifier));
  bNodeSocket *scale_out = bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline(scale_iface->identifier));
  bNodeSocket *rotation_out = bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline(rotation_iface->identifier));
  if (offset_out == nullptr || scale_out == nullptr || rotation_out == nullptr) {
    return;
  }
  for (bNode &node : tree.nodes) {
    const bool is_tex_coord = node.type_legacy == SH_NODE_TEX_COORD;
    const bool is_owner_uv_map = source_group_uv_map_is_remapped(node, uv);
    /* The Attribute node carries the UV on `Vector`, not on `UV`; its Color/Fac/Alpha outputs are
     * the attribute's own data and stay untouched. */
    const bool is_uv_attribute = source_group_attribute_names_uv(node, uv);
    if (!is_tex_coord && !is_owner_uv_map && !is_uv_attribute) {
      continue;
    }
    bNodeSocket *uv_out = socket_out(node, is_uv_attribute ? "Vector" : "UV");
    if (uv_out == nullptr) {
      continue;
    }
    /* The links are read from the tree itself, like #texture_vector_link_mapped does: the
     * per-socket link cache is refreshed by a tree update this build has not run. */
    Vector<std::pair<bNode *, bNodeSocket *>> readers;
    for (bNodeLink &link : tree.links) {
      if (link.fromnode == &node && link.fromsock == uv_out && link.is_available()) {
        readers.append({link.tonode, link.tosock});
      }
    }
    if (readers.is_empty()) {
      continue;
    }
    bNode *mapping = bke::node_add_static_node(nullptr, tree, SH_NODE_MAPPING);
    if (mapping == nullptr) {
      return;
    }
    mapping->custom1 = NODE_MAPPING_TYPE_POINT;
    STRNCPY_UTF8(mapping->name, SOURCE_GROUP_MAPPING_NODE_NAME);
    bke::node_unique_name(tree, *mapping);
    mapping->location[0] = node.location[0] + 80.0f;
    mapping->location[1] = node.location[1];
    bNodeSocket *map_vector = socket_in(*mapping, "Vector");
    bNodeSocket *map_out = socket_out(*mapping, "Vector");
    bNodeSocket *map_location = socket_in(*mapping, "Location");
    bNodeSocket *map_scale = socket_in(*mapping, "Scale");
    bNodeSocket *map_rotation = socket_in(*mapping, "Rotation");
    if (map_vector == nullptr || map_out == nullptr || map_location == nullptr ||
        map_scale == nullptr || map_rotation == nullptr)
    {
      return;
    }
    bke::node_add_link(tree, node, *uv_out, *mapping, *map_vector);
    bke::node_add_link(tree, *group_input, *offset_out, *mapping, *map_location);
    bke::node_add_link(tree, *group_input, *scale_out, *mapping, *map_scale);
    bke::node_add_link(tree, *group_input, *rotation_out, *mapping, *map_rotation);
    /* Re-point the readers: remove-and-add keeps the tree's own bookkeeping honest, which
     * assigning `tonode`/`tosock` in place would bypass. */
    for (const std::pair<bNode *, bNodeSocket *> &reader : readers) {
      bNodeLink *stale = nullptr;
      for (bNodeLink &link : tree.links) {
        if (link.fromnode == &node && link.fromsock == uv_out && link.tonode == reader.first &&
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
  }
}

/**
 * With no UV layer named, #generated_uv_maps_wire leaves the open Vector of every Image Texture
 * alone, and #source_group_mapping_insert only remaps Texture Coordinate and named UV Map nodes,
 * so an implicitly-UV texture would sit outside the row mapping. Feed those textures from a
 * Texture Coordinate `UV` output here, so the mapping insertion then finds it. Links are read from
 * the tree, like #generated_uv_maps_wire does.
 */
static void source_group_implicit_uv_wire(bNodeTree &tree)
{
  bNode *tex_coord = nullptr;
  bNodeSocket *uv_out = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE) {
      continue;
    }
    bNodeSocket *vector = socket_in(node, "Vector");
    if (vector == nullptr) {
      continue;
    }
    bool linked = false;
    for (const bNodeLink &link : tree.links) {
      if (link.tosock == vector) {
        linked = true;
        break;
      }
    }
    if (linked) {
      continue;
    }
    if (tex_coord == nullptr) {
      std::tie(tex_coord, uv_out) = generated_uv_map_ensure(tree, "");
      if (tex_coord == nullptr || uv_out == nullptr) {
        return;
      }
    }
    bke::node_add_link(tree, *tex_coord, *uv_out, node, *vector);
  }
}

/**
 * The row-mapping half of a mapped level: the three interface inputs, the implicit-UV wiring for
 * an unnamed layer, and the Mapping nodes in front of every UV output. One place for the wrapper
 * root, its path copies and the private copies.
 */
static void source_group_mapping_prepare(bNodeTree &tree, const SourceGroupUvPolicy &uv)
{
  if (uv.uv_name == nullptr || uv.uv_name[0] == '\0') {
    source_group_implicit_uv_wire(tree);
  }
  source_group_mapping_interface_ensure(tree);
  source_group_mapping_insert(tree, uv);
}

/**
 * Wire the wrapper instance's mapping inputs from the row group's own: the three values travel
 * the same relay every other value does, from the root instance down through each interface.
 * Idempotent: the instance is shared by the row's channels, and a second wiring must add no
 * link. An unlinked input (the row carries no mapping, or its inputs were never created) leaves
 * the wrapper's Mapping at its identity defaults.
 */
static void source_group_mapping_instance_wire(bNodeTree &tree,
                                               bNode &group_input,
                                               bNode &instance)
{
  /* The two sides are different trees with their own identifiers: the source is the parent's
   * interface input, the target the child's (the instance's group), each found by role. */
  if (instance.id == nullptr || GS(instance.id->name) != ID_NT) {
    return;
  }
  bNodeTree &child = *id_cast<bNodeTree *>(instance.id);
  const char *roles[3] = {SOURCE_GROUP_ROLE_MAPPING_OFFSET,
                          SOURCE_GROUP_ROLE_MAPPING_SCALE,
                          SOURCE_GROUP_ROLE_MAPPING_ROTATION};
  for (const char *role : roles) {
    const bNodeTreeInterfaceSocket *iface = source_group_mapping_input_find(tree, role);
    const bNodeTreeInterfaceSocket *child_iface = source_group_mapping_input_find(child, role);
    if (iface == nullptr || iface->identifier == nullptr || child_iface == nullptr ||
        child_iface->identifier == nullptr)
    {
      continue;
    }
    bNodeSocket *src = bke::node_find_socket(
        group_input, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
    bNodeSocket *dst = bke::node_find_socket(
        instance, SOCK_IN, UString::from_ptr_noinline(child_iface->identifier));
    if (src == nullptr || dst == nullptr) {
      continue;
    }
    bool wired = false;
    for (bNodeLink &link : tree.links) {
      if (link.tosock == dst) {
        wired = true;
        break;
      }
    }
    if (!wired) {
      bke::node_add_link(tree, group_input, *src, instance, *dst);
    }
  }
}

/**
 * The private (copy-on-write) copy of the shared source group \a orig (ТЗ 2.3): a group whose
 * tree reads the owner's UVs anywhere must not be shared with other users of the source, so it
 * is rebuilt here like a path copy -- private nodes, the owner's UV wiring, the row mapping in
 * front of the UV outputs and its three inputs on the interface, then the same treatment for the
 * groups it instantiates. Null when \a orig owns no UV reader at any depth: the group stays
 * shared, exactly as the original material built it.
 */
static bNodeTree *source_group_private_copy_ensure(Main &bmain,
                                                   const bNodeTree &orig,
                                                   const int depth,
                                                   const bUUID &owner_uid,
                                                   const int source_uid,
                                                   const char *source_name,
                                                   const SourceGroupUvPolicy &uv,
                                                   const bool mapped)
{
  if (depth > PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH) {
    return nullptr;
  }
  if (!source_group_tree_reads_uv(const_cast<bNodeTree &>(orig), uv, 0)) {
    return nullptr;
  }
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, ".PL Source %s %s", source_name, orig.id.name + 2);
  bNodeTree *copy = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
  if (copy == nullptr) {
    return nullptr;
  }
  /* The instance is the only user; the copy owns its own reference like a path copy does. */
  id_us_min(&copy->id);
  tree_owner_uid_set(*copy, owner_uid);
  prop_int_set(copy->id.properties, TREE_SOURCE_PROP, source_uid);
  copy->tree_interface.free_data();
  /* The copy keeps the original's interface so the other nodes of the parent copy keep their
   * links; `copy_data` carries `next_uid` over, so the mapping inputs added after it take fresh
   * identifiers. */
  copy->tree_interface.copy_data(orig.tree_interface, 0);
  Map<const bNode *, bNode *> node_map;
  if (!source_group_copy_nodes(bmain, *copy, orig, node_map)) {
    BKE_id_free(&bmain, copy);
    return nullptr;
  }
  copy->ensure_topology_cache();
  generated_uv_maps_wire(*copy, uv.uv_name);
  if (mapped) {
    source_group_mapping_prepare(*copy, uv);
  }
  /* The groups this copy instantiates follow the same rule; a shared group below a private one
   * is copied too, so the mapping reaches every UV reader the group feeds. */
  for (const auto &item : node_map.items()) {
    const bNode *orig_node = item.key;
    bNode *copy_node = item.value;
    if (!copy_node->is_group() || orig_node->id == nullptr ||
        GS(orig_node->id->name) != ID_NT)
    {
      continue;
    }
    const bNodeTree *orig_child = reinterpret_cast<const bNodeTree *>(orig_node->id);
    bNodeTree *child_copy = source_group_private_copy_ensure(bmain,
                                                             *orig_child,
                                                             depth + 1,
                                                             owner_uid,
                                                             source_uid,
                                                             source_name,
                                                             uv,
                                                             mapped);
    if (child_copy == nullptr) {
      continue;
    }
    if (copy_node->id != nullptr) {
      id_us_min(copy_node->id);
    }
    copy_node->id = &child_copy->id;
    id_us_plus(&child_copy->id);
    nodes::update_node_declaration_and_sockets(*copy, *copy_node);
    if (mapped) {
      bNode *group_input = source_group_group_input_ensure(*copy);
      nodes::update_node_declaration_and_sockets(*copy, *group_input);
      source_group_mapping_instance_wire(*copy, *group_input, *copy_node);
    }
  }
  return copy;
}

/** Expose \a channels in \a tree, reading them straight from \a principled. */
bool source_group_wire_principled(bNodeTree &tree,
                                  bNode &principled,
                                  const Vector<SourceGroupChannel> &channels,
                                  const bool is_root,
                                  const bool mapped)
{
  bNode *group_output = source_group_group_output(tree);
  if (group_output == nullptr) {
    return false;
  }
  Vector<bNodeTreeInterfaceSocket *> ifaces;
  for (const SourceGroupChannel &spec : channels) {
    char name[160];
    source_group_output_name(spec, is_root, name);
    char role[128];
    ifaces.append(source_group_interface_output(
        tree,
        name,
        source_group_socket_type(eMaterialPaintChannel(spec.channel)),
        is_root ? source_group_role(spec, role) : nullptr));
  }
  nodes::update_node_declaration_and_sockets(tree, *group_output);
  for (const int i : channels.index_range()) {
    if (ifaces[i] == nullptr || ifaces[i]->identifier == nullptr) {
      continue;
    }
    bNodeSocket *out_in = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline(ifaces[i]->identifier));
    if (out_in == nullptr) {
      continue;
    }
    const bNodeSocket *input = bke::node_find_socket(
        principled, SOCK_IN, UString::from_ptr_noinline(channels[i].principled_socket));
    if (input == nullptr) {
      continue;
    }
    /* The declaration update above and every link added by an earlier channel invalidate the
     * topology cache that `directly_linked_links` reads, so it is rebuilt before each read. */
    tree.ensure_topology_cache();
    const Span<const bNodeLink *> links = input->directly_linked_links();
    if (links.is_empty()) {
      socket_default_copy(*out_in, *input);
    }
    else if (channels[i].channel == int(PAINT_MATERIAL_CHANNEL_NORMAL)) {
      /* The Normal leaves as the encoded map the bake and the chain share, never as the decoded
       * vector the Principled input carries. */
      source_group_wire_normal(tree, *group_output, *out_in, *input, mapped);
    }
    else {
      bke::node_add_link(tree, *links[0]->fromnode, *links[0]->fromsock, *group_output, *out_in);
    }
  }
  return true;
}

/** Expose \a channels in \a tree from \a instance, whose group is \a child. */
bool source_group_wire_instance(bNodeTree &tree,
                                bNode &instance,
                                bNodeTree &child,
                                const Vector<SourceGroupChannel> &channels,
                                const bool is_root)
{
  bNode *group_output = source_group_group_output(tree);
  if (group_output == nullptr) {
    return false;
  }
  Vector<bNodeTreeInterfaceSocket *> ifaces;
  for (const SourceGroupChannel &spec : channels) {
    char name[160];
    source_group_output_name(spec, is_root, name);
    char role[128];
    ifaces.append(source_group_interface_output(
        tree,
        name,
        source_group_socket_type(eMaterialPaintChannel(spec.channel)),
        is_root ? source_group_role(spec, role) : nullptr));
  }
  nodes::update_node_declaration_and_sockets(tree, *group_output);
  for (const int i : channels.index_range()) {
    if (ifaces[i] == nullptr || ifaces[i]->identifier == nullptr) {
      continue;
    }
    char child_name[160];
    source_group_output_name(channels[i], false, child_name);
    bNodeTreeInterfaceSocket *child_iface = source_group_interface_output_find(child, child_name);
    if (child_iface == nullptr || child_iface->identifier == nullptr) {
      continue;
    }
    bNodeSocket *instance_out = bke::node_find_socket(
        instance, SOCK_OUT, UString::from_ptr_noinline(child_iface->identifier));
    bNodeSocket *out_in = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline(ifaces[i]->identifier));
    if (instance_out != nullptr && out_in != nullptr) {
      bke::node_add_link(tree, instance, *instance_out, *group_output, *out_in);
    }
  }
  return true;
}

/**
 * Rebuild \a tree as a copy of \a orig_tree and expose \a channels through it. For every group on
 * \a path the shared original group in the copy is replaced by a private copy (marked with
 * #TREE_SOURCE_PROP so #source_groups_prune collects it), the recursion descends, and each level
 * propagates the child's new outputs up to its own Group Output. Only \a is_root carries the
 * `COLOR:<CHANNEL>`/`COVERAGE` roles.
 *
 * With \a mapped, every level puts the row mapping in front of its UV outputs (ТЗ 2.2) and
 * carries the three mapping inputs on its interface; shared groups off the path whose trees read
 * UVs are copied privately (ТЗ 2.3) so the mapping reaches them too.
 */
bool source_group_build_level(Main &bmain,
                              bNodeTree &tree,
                              const bNodeTree &orig_tree,
                              const Vector<const bNode *> &path,
                              const int depth,
                              const bNode *principled,
                              const Vector<SourceGroupChannel> &channels,
                              const bool is_root,
                              const bUUID &owner_uid,
                              const int source_uid,
                              const char *source_name,
                              const SourceGroupUvPolicy &uv,
                              const bool mapped)
{
  Map<const bNode *, bNode *> node_map;
  if (!source_group_copy_nodes(bmain, tree, orig_tree, node_map)) {
    return false;
  }
  /* `source_group_copy_nodes` lays the copied links down but does not build the link runtime;
   * `source_group_wire_principled` reads `directly_linked_links`, so without this the copied
   * Principled's source is not seen and the Group Output keeps the socket defaults instead of the
   * values the group is fed. */
  tree.ensure_topology_cache();
  /* Every Image Texture the wrapper copied samples the owner's named UV layer: an already-wired
   * Vector is left alone, an open one is linked to the shared UV Map node. */
  generated_uv_maps_wire(tree, uv.uv_name);
  if (mapped) {
    /* The row mapping in front of the UV outputs (Texture Coordinate, the owner's UV Map and the
     * implicitly wired Image Textures), driven by this level's three interface inputs. The root's
     * interface survives a rebuild because the inputs are found by role; a path copy's interface
     * was copied from its original and the inputs are added on top here. */
    source_group_mapping_prepare(tree, uv);
  }
  else if (is_root) {
    /* The root's interface survives a rebuild, so inputs left by an earlier mapped build go now.
     * Path copies need no such step: their interface is copied fresh from the original. */
    source_group_mapping_interface_remove(tree);
  }
  /* Only a mapped wrapper copies shared groups off the path (copy-on-write, on every level, the
   * Principled's own included): one whose tree reads UVs gets its own private copy so the mapping
   * reaches it. Without the mapping nothing in them changes, so they stay shared with the original
   * material exactly as before. The group the path descends through is skipped here -- it is
   * handled by the recursion below. */
  const bNode *path_node = (depth < int(path.size())) ? path[depth] : nullptr;
  for (const auto &item : node_map.items()) {
    const bNode *orig_node = item.key;
    bNode *copy_node = item.value;
    if (!mapped || orig_node == path_node || !copy_node->is_group() || orig_node->id == nullptr ||
        GS(orig_node->id->name) != ID_NT)
    {
      continue;
    }
    const bNodeTree *orig_child_shared = reinterpret_cast<const bNodeTree *>(orig_node->id);
    if (orig_child_shared == nullptr) {
      continue;
    }
    bNodeTree *private_copy = source_group_private_copy_ensure(bmain,
                                                               *orig_child_shared,
                                                               depth + 1,
                                                               owner_uid,
                                                               source_uid,
                                                               source_name,
                                                               uv,
                                                               mapped);
    if (private_copy == nullptr) {
      continue;
    }
    if (copy_node->id != nullptr) {
      id_us_min(copy_node->id);
    }
    copy_node->id = &private_copy->id;
    id_us_plus(&private_copy->id);
    nodes::update_node_declaration_and_sockets(tree, *copy_node);
    if (mapped) {
      bNode *group_input = source_group_group_input_ensure(tree);
      nodes::update_node_declaration_and_sockets(tree, *group_input);
      source_group_mapping_instance_wire(tree, *group_input, *copy_node);
    }
  }
  if (depth >= int(path.size())) {
    bNode *copied_principled = node_map.lookup_default(principled, nullptr);
    if (copied_principled == nullptr) {
      return false;
    }
    return source_group_wire_principled(tree, *copied_principled, channels, is_root, mapped);
  }
  const bNode *orig_group = path[depth];
  bNode *instance = node_map.lookup_default(orig_group, nullptr);
  if (instance == nullptr || orig_group->id == nullptr || GS(orig_group->id->name) != ID_NT) {
    return false;
  }
  const bNodeTree *orig_child = reinterpret_cast<const bNodeTree *>(orig_group->id);
  if (orig_child == nullptr) {
    return false;
  }
  char child_name[MAX_ID_NAME - 2];
  SNPRINTF(child_name, ".PL Source %s %s", source_name, orig_child->id.name + 2);
  bNodeTree *child = bke::node_tree_add_tree(&bmain, child_name, "ShaderNodeTree");
  if (child == nullptr) {
    return false;
  }
  /* The copy's Group Input and the parent instance's input sockets are built from the group's
   * interface, and a fresh tree has only an empty one. Copy the source group's before the body is
   * built, or the copied Group Input has no sockets, the links into it are lost and the Principled
   * falls back to its defaults. The copy is an ID of its own that references any pointer default
   * value (Image/Object/Material) itself, so it takes its own user reference: flag 0, as
   * `ntree_copy_data` does. `free_data` drops the empty interface the fresh tree was born with
   * first; `copy_data` re-tags the item cache itself. */
  child->tree_interface.free_data();
  child->tree_interface.copy_data(orig_child->tree_interface, 0);
  /* The instance is the only user; the copy starts owning its own text-less tree. */
  id_us_min(&child->id);
  tree_owner_uid_set(*child, owner_uid);
  prop_int_set(child->id.properties, TREE_SOURCE_PROP, source_uid);
  if (instance->id != nullptr) {
    id_us_min(instance->id);
  }
  instance->id = &child->id;
  id_us_plus(&child->id);
  if (!source_group_build_level(bmain,
                                *child,
                                *orig_child,
                                path,
                                depth + 1,
                                principled,
                                channels,
                                false,
                                owner_uid,
                                source_uid,
                                source_name,
                                uv,
                                mapped))
  {
    return false;
  }
  /* The child gained its outputs; the instance only sees them after a declaration update. */
  nodes::update_node_declaration_and_sockets(tree, *instance);
  if (mapped) {
    /* The parent relays its own three mapping inputs into the child instance, the same way it
     * relays a row's value inputs through the group interfaces. */
    bNode *group_input = source_group_group_input_ensure(tree);
    nodes::update_node_declaration_and_sockets(tree, *group_input);
    source_group_mapping_instance_wire(tree, *group_input, *instance);
  }
  return source_group_wire_instance(tree, *instance, *child, channels, is_root);
}

/**
 * Rebuild \a group's body as a copy of \a source's tree with a Group Output in place of every
 * Material Output, then expose the Principled's channels, walking \a path through any nested
 * groups. The interface is left in place (outputs are reused by name), so a rebuild keeps the
 * socket identifiers an instance would link into.
 */
bool source_group_build(Main &bmain,
                        bNodeTree &group,
                        Material &owner,
                        const Material &source,
                        const bNode *principled,
                        const Vector<const bNode *> &path,
                        const int source_uid,
                        const bool mapped)
{
  source.nodetree->ensure_topology_cache();
  const Vector<SourceGroupChannel> channels = source_group_channels(*principled);
  return source_group_build_level(bmain,
                                  group,
                                  *source.nodetree,
                                  path,
                                  0,
                                  principled,
                                  channels,
                                  true,
                                  owner.paint_layers_owner_uid,
                                  source_uid,
                                  source.id.name + 2,
                                  SourceGroupUvPolicy{BKE_paint_layers_uv_map_name(owner),
                                                      (owner.paint_layers_flag &
                                                       MA_PAINT_LAYERS_REMAP_ALL_UV) != 0},
                                  mapped);
}

/** Delete the wrapper trees of \a owner whose source is no longer referenced by any row. */
void source_groups_prune(Main &bmain, const Material &owner)
{
  Set<uint32_t> referenced;
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(owner, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer->material != nullptr) {
      referenced.add(layer->material->id.session_uid);
    }
  }
  Vector<bNodeTree *> orphans;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (!BLI_uuid_equal(tree_owner_uid_get(tree), owner.paint_layers_owner_uid)) {
      continue;
    }
    const int source_uid = prop_int_get(tree.id.properties, TREE_SOURCE_PROP, 0);
    if (source_uid == 0 || referenced.contains(uint32_t(source_uid))) {
      continue;
    }
    orphans.append(&tree);
  }
  /* Several passes: deleting a wrapper frees its instance nodes, which drops the user count of the
   * path group copies and lets the next pass delete those too. */
  for (int pass = 0; pass < PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH && !orphans.is_empty(); pass++) {
    Vector<bNodeTree *> remaining;
    bool deleted = false;
    for (bNodeTree *tree : orphans) {
      if (ID_REAL_USERS(&tree->id) <= 0) {
        BKE_id_delete(&bmain, tree);
        deleted = true;
      }
      else {
        remaining.append(tree);
      }
    }
    orphans = std::move(remaining);
    if (!deleted) {
      break;
    }
  }
}

}  // namespace bke::paint_layers
using namespace bke::paint_layers;

/* -------------------------------------------------------------------- */
/** \name Source Group Value Sync
 *
 * A value edit in the source updates the existing wrapper in place: a rebuild would replace the
 * private path copies, recompile the shader and lose the generated group's identity. Only a
 * topology edit rebuilds. Nodes match by name and sockets by identifier, both of which the copy
 * preserves; groups off the path to the Principled are shared with the source and need no sync.
 * \{ */

/** Copy one scalar socket value; true when it changed. Void pointers because both #bNodeSocket and
 * #bNodeTreeInterfaceSocket store the same #bNodeSocketValue* behind them. */
static bool source_group_value_copy(const eNodeSocketDatatype type, void *dst_data, const void *src_data)
{
  if (dst_data == nullptr || src_data == nullptr) {
    return false;
  }
  switch (type) {
    case SOCK_FLOAT: {
      bNodeSocketValueFloat &dst = *static_cast<bNodeSocketValueFloat *>(dst_data);
      const bNodeSocketValueFloat &src = *static_cast<const bNodeSocketValueFloat *>(src_data);
      if (dst.value == src.value && dst.subtype == src.subtype) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_INT: {
      bNodeSocketValueInt &dst = *static_cast<bNodeSocketValueInt *>(dst_data);
      const bNodeSocketValueInt &src = *static_cast<const bNodeSocketValueInt *>(src_data);
      if (dst.value == src.value && dst.subtype == src.subtype) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_BOOLEAN: {
      bNodeSocketValueBoolean &dst = *static_cast<bNodeSocketValueBoolean *>(dst_data);
      const bNodeSocketValueBoolean &src = *static_cast<const bNodeSocketValueBoolean *>(src_data);
      if (dst.value == src.value) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_VECTOR: {
      bNodeSocketValueVector &dst = *static_cast<bNodeSocketValueVector *>(dst_data);
      const bNodeSocketValueVector &src = *static_cast<const bNodeSocketValueVector *>(src_data);
      if (dst.dimensions == src.dimensions && equals_v3v3(dst.value, src.value)) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_INT_VECTOR: {
      bNodeSocketValueIntVector &dst = *static_cast<bNodeSocketValueIntVector *>(dst_data);
      const bNodeSocketValueIntVector &src =
          *static_cast<const bNodeSocketValueIntVector *>(src_data);
      if (dst.dimensions == src.dimensions && dst.value[0] == src.value[0] &&
          dst.value[1] == src.value[1] && dst.value[2] == src.value[2])
      {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_RGBA: {
      bNodeSocketValueRGBA &dst = *static_cast<bNodeSocketValueRGBA *>(dst_data);
      const bNodeSocketValueRGBA &src = *static_cast<const bNodeSocketValueRGBA *>(src_data);
      if (equals_v4v4(dst.value, src.value)) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_ROTATION: {
      bNodeSocketValueRotation &dst = *static_cast<bNodeSocketValueRotation *>(dst_data);
      const bNodeSocketValueRotation &src = *static_cast<const bNodeSocketValueRotation *>(src_data);
      if (equals_v3v3(dst.value_euler, src.value_euler)) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_STRING: {
      bNodeSocketValueString &dst = *static_cast<bNodeSocketValueString *>(dst_data);
      const bNodeSocketValueString &src = *static_cast<const bNodeSocketValueString *>(src_data);
      if (STREQ(dst.value, src.value)) {
        return false;
      }
      BLI_strncpy(dst.value, src.value, sizeof(dst.value));
      return true;
    }
    case SOCK_MENU: {
      /* Only the chosen value; the rest of the struct is a runtime pointer to the enum items. */
      bNodeSocketValueMenu &dst = *static_cast<bNodeSocketValueMenu *>(dst_data);
      const bNodeSocketValueMenu &src = *static_cast<const bNodeSocketValueMenu *>(src_data);
      if (dst.value == src.value) {
        return false;
      }
      dst.value = src.value;
      return true;
    }
    default:
      return false;
  }
}

/** Copy \a src's non-structural `node->id` into \a dst for a leaf node; true when it changed. A
 * group's `id` is the path copy the wrapper owns, never the source's group, so it is left alone. */
static bool source_group_node_id_copy(bNode &dst, const bNode &src)
{
  if (src.is_group() || dst.id == src.id) {
    return false;
  }
  if (dst.id != nullptr) {
    id_us_min(dst.id);
  }
  dst.id = src.id;
  if (dst.id != nullptr) {
    id_us_plus(dst.id);
  }
  return true;
}

/** Copy \a src's node storage into \a dst through the type's own copy and free callbacks, so a
 * #CurveMapping and the like keep their API allocation. True when it changed. */
static bool source_group_node_storage_copy(bNodeTree &dst_tree, bNode &dst, const bNode &src)
{
  if (src.typeinfo == nullptr || src.typeinfo->copyfunc == nullptr) {
    return false;
  }
  /* Compare first: a node whose storage already matches must not be freed and re-copied, or the
   * sync would tag it (and its tree) on every value edit of a sibling. */
  if (BKE_paint_layers_source_node_storage_hash(dst) ==
      BKE_paint_layers_source_node_storage_hash(src))
  {
    return false;
  }
  if (src.storage == nullptr) {
    if (dst.storage == nullptr) {
      return false;
    }
    if (src.typeinfo->freefunc != nullptr) {
      src.typeinfo->freefunc(&dst);
    }
    dst.storage = nullptr;
    return true;
  }
  if (dst.storage != nullptr) {
    if (src.typeinfo->freefunc == nullptr) {
      /* The old storage cannot be released, so a copy would leak it: leave it as it was. */
      return false;
    }
    src.typeinfo->freefunc(&dst);
    dst.storage = nullptr;
  }
  src.typeinfo->copyfunc(&dst_tree, &dst, &src);
  return dst.storage != nullptr;
}

/** Refresh the wrapper's channel constants from an already-synced copied Principled. A channel
 * whose Principled input is linked is fed through that link and needs no default; an unlinked one
 * is read from the Group Output's default, which the build copied once and the sync must renew. */
static bool source_group_refresh_channel_defaults(bNodeTree &dst_tree,
                                                  bNode &principled_copy,
                                                  const Vector<SourceGroupChannel> &channels,
                                                  const bool is_root)
{
  bool changed = false;
  bNode *group_output = source_group_group_output(dst_tree);
  if (group_output == nullptr) {
    return false;
  }
  dst_tree.ensure_topology_cache();
  for (const SourceGroupChannel &spec : channels) {
    char name[160];
    source_group_output_name(spec, is_root, name);
    bNodeTreeInterfaceSocket *iface = source_group_interface_output_find(dst_tree, name);
    if (iface == nullptr || iface->identifier == nullptr) {
      continue;
    }
    bNodeSocket *out_in = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
    bNodeSocket *input = bke::node_find_socket(
        principled_copy, SOCK_IN, UString::from_ptr_noinline(spec.principled_socket));
    if (out_in == nullptr || input == nullptr || !input->directly_linked_links().is_empty()) {
      continue;
    }
    if (source_group_value_copy(out_in->type, out_in->default_value, input->default_value)) {
      BKE_ntree_update_tag_node_property(&dst_tree, group_output);
      changed = true;
    }
  }
  return changed;
}

/** Whether \a socket is one of the wrapper's own row-mapping value inputs: their values belong to
 * the rows, so the value sync must never copy a source interface input over them. */
static bool source_group_input_is_mapping(const bNodeTreeInterfaceSocket &socket)
{
  const char *role = prop_string_get(socket.properties, INPUT_ROLE_PROP);
  return role != nullptr && (STREQ(role, SOURCE_GROUP_ROLE_MAPPING_OFFSET) ||
                             STREQ(role, SOURCE_GROUP_ROLE_MAPPING_SCALE) ||
                             STREQ(role, SOURCE_GROUP_ROLE_MAPPING_ROTATION));
}

/**
 * Copy every value of \a src_tree into the matching nodes of \a dst_tree, once each, then recurse
 * into the private copies of the groups on the path to the Principled. Matching is by node name and
 * socket identifier, which #source_group_copy_nodes preserves. Returns the number of nodes changed.
 */
static int source_group_values_sync_tree(Main &bmain,
                                         const bNodeTree &src_tree,
                                         bNodeTree &dst_tree,
                                         const bNode *principled,
                                         const Vector<SourceGroupChannel> &channels,
                                         const bool is_root)
{
  int synced = 0;
  bool tree_changed = false;
  Map<StringRefNull, bNode *> by_name;
  for (bNode &node : dst_tree.nodes) {
    by_name.add(node.name, &node);
  }
  for (const bNode *src : src_tree.all_nodes()) {
    bNode *dst = by_name.lookup_default(src->name, nullptr);
    if (dst == nullptr || dst->type_legacy != src->type_legacy) {
      continue;
    }
    bool self_changed = false;
    if (dst->custom1 != src->custom1) {
      dst->custom1 = src->custom1;
      self_changed = true;
    }
    if (dst->custom2 != src->custom2) {
      dst->custom2 = src->custom2;
      self_changed = true;
    }
    if (dst->custom3 != src->custom3) {
      dst->custom3 = src->custom3;
      self_changed = true;
    }
    if (dst->custom4 != src->custom4) {
      dst->custom4 = src->custom4;
      self_changed = true;
    }
    self_changed |= source_group_node_id_copy(*dst, *src);
    self_changed |= source_group_node_storage_copy(dst_tree, *dst, *src);
    for (const bNodeSocket &src_sock : src->inputs) {
      bNodeSocket *dst_sock = bke::node_find_socket(
          *dst, SOCK_IN, src_sock.identifier_ustr());
      if (dst_sock != nullptr &&
          source_group_value_copy(dst_sock->type, dst_sock->default_value, src_sock.default_value))
      {
        self_changed = true;
      }
    }
    for (const bNodeSocket &src_sock : src->outputs) {
      bNodeSocket *dst_sock = bke::node_find_socket(
          *dst, SOCK_OUT, src_sock.identifier_ustr());
      if (dst_sock != nullptr &&
          source_group_value_copy(dst_sock->type, dst_sock->default_value, src_sock.default_value))
      {
        self_changed = true;
      }
    }
    if (self_changed) {
      BKE_ntree_update_tag_node_property(&dst_tree, dst);
      synced++;
      tree_changed = true;
    }
    if (src == principled) {
      /* The channel constants the wrapper reads are copies of this Principled's inputs. */
      self_changed |= source_group_refresh_channel_defaults(dst_tree, *dst, channels, is_root);
    }
    if (src->is_group() && dst->is_group() && src->id != nullptr && dst->id != nullptr &&
        src->id != dst->id)
    {
      const bNodeTree *src_child = id_cast<const bNodeTree *>(src->id);
      bNodeTree *dst_child = id_cast<bNodeTree *>(dst->id);
      if (src_child != nullptr && dst_child != nullptr) {
        synced += source_group_values_sync_tree(
            bmain, *src_child, *dst_child, principled, channels, false);
      }
    }
  }
  /* Group interface inputs move too; the wrapper's own outputs are its channel contract and are
   * never touched. A missing identifier means the interface changed and a rebuild handles it. */
  src_tree.ensure_interface_cache();
  dst_tree.ensure_interface_cache();
  Map<StringRefNull, bNodeTreeInterfaceSocket *> iface_by_id;
  for (bNodeTreeInterfaceSocket *socket : dst_tree.interface_inputs()) {
    if (socket->identifier != nullptr) {
      iface_by_id.add(socket->identifier, socket);
    }
  }
  for (const bNodeTreeInterfaceSocket *src_socket : src_tree.interface_inputs()) {
    if (src_socket->identifier == nullptr) {
      continue;
    }
    bNodeTreeInterfaceSocket *dst_socket = iface_by_id.lookup_default(src_socket->identifier,
                                                                      nullptr);
    const bke::bNodeSocketType *src_type =
        src_socket->socket_typeinfo();
    if (dst_socket != nullptr && src_type != nullptr &&
        !source_group_input_is_mapping(*dst_socket))
    {
      if (source_group_value_copy(
              src_type->type, dst_socket->socket_data, src_socket->socket_data))
      {
        tree_changed = true;
      }
    }
  }
  /* Only a tree that actually changed is re-declared and pushed to its evaluated copy; a sync that
   * matched every value leaves the tree (and its users) untouched. */
  if (tree_changed) {
    BKE_ntree_update_after_single_tree_change(bmain, dst_tree);
    DEG_id_tag_update(&dst_tree.id, ID_RECALC_SYNC_TO_EVAL);
  }
  return synced;
}

/** \} */

const char *BKE_paint_layers_source_group_refusal_name(
    const PaintLayersSourceGroupRefusal refusal)
{
  switch (refusal) {
    case PaintLayersSourceGroupRefusal::None:
      return "none";
    case PaintLayersSourceGroupRefusal::NoNodeTree:
      return "no-node-tree";
    case PaintLayersSourceGroupRefusal::NoPrincipled:
      return "no-principled";
    case PaintLayersSourceGroupRefusal::PrincipledInGroup:
      return "principled-in-group";
    case PaintLayersSourceGroupRefusal::SelfReference:
      return "self-reference";
    case PaintLayersSourceGroupRefusal::BuildFailed:
      return "build-failed";
    case PaintLayersSourceGroupRefusal::TooManyTextures:
      return "too-many-textures";
  }
  return "unknown";
}

bool BKE_paint_layers_source_group_build_failed_get(const Material &owner,
                                                    const MaterialPaintLayer &layer)
{
  const bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_get(owner);
  if (runtime == nullptr) {
    return false;
  }
  for (const bUUID &other : runtime->source_group_build_failed) {
    if (BLI_uuid_equal(other, layer.marker)) {
      return true;
    }
  }
  return false;
}

void BKE_paint_layers_source_group_build_failed_set(Material &owner,
                                                    const MaterialPaintLayer &layer,
                                                    const bool failed)
{
  bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_mutable(owner);
  if (runtime == nullptr) {
    if (!failed) {
      return;
    }
    runtime = &bke::paint_layers_runtime_ensure(owner);
  }
  for (int i = 0; i < runtime->source_group_build_failed.size();) {
    if (BLI_uuid_equal(runtime->source_group_build_failed[i], layer.marker)) {
      runtime->source_group_build_failed.remove(i);
    }
    else {
      i++;
    }
  }
  if (failed) {
    runtime->source_group_build_failed.append(layer.marker);
  }
}

/**
 * The Main-free refusal behind a Baked row: the same checks #BKE_paint_layers_source_group_ensure
 * runs before it touches #Main, so the UI names the same reason the regeneration reports. A live
 * row needs no probe: its wrapper was built or is not needed, and the sampler fallback already
 * reports Baked through #BKE_paint_layers_material_mode.
 */
static PaintLayersSourceGroupRefusal material_row_refusal_probe(const Material &ma,
                                                                const MaterialPaintLayer &layer)
{
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return PaintLayersSourceGroupRefusal::None;
  }
  if (BKE_paint_layers_material_forced_bake(ma, layer)) {
    return PaintLayersSourceGroupRefusal::TooManyTextures;
  }
  const Material *source = layer.material;
  if (source == nullptr) {
    return PaintLayersSourceGroupRefusal::None;
  }
  if (source == &ma) {
    return PaintLayersSourceGroupRefusal::SelfReference;
  }
  if (source->nodetree == nullptr) {
    return PaintLayersSourceGroupRefusal::NoNodeTree;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  if (BKE_paint_material_principled_find(*source, reason, &group_path) == nullptr) {
    return PaintLayersSourceGroupRefusal::NoPrincipled;
  }
  if (int(group_path.size()) > PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH) {
    return PaintLayersSourceGroupRefusal::PrincipledInGroup;
  }
  return PaintLayersSourceGroupRefusal::None;
}

PaintLayerMaterialLiveStatus BKE_paint_layers_material_live_status(
    const Material &ma,
    const MaterialPaintLayer &layer,
    PaintLayersSourceGroupRefusal *r_refusal,
    const PaintLayersRegenCache *cache)
{
  const PaintLayerMaterialMode mode = BKE_paint_layers_material_mode(ma, layer, cache);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  if (mode == PaintLayerMaterialMode::Baked) {
    refusal = material_row_refusal_probe(ma, layer);
  }
  else if (mode == PaintLayerMaterialMode::SourceGroup &&
           BKE_paint_layers_source_group_build_failed_get(ma, layer))
  {
    /* The live wrapper was refused when it was last built. The refusal only exists inside a
     * regeneration report otherwise, which is discarded, so the Main-free status would call the
     * row Live and the UI would never show that it did not build. */
    refusal = PaintLayersSourceGroupRefusal::BuildFailed;
  }
  if (r_refusal != nullptr) {
    *r_refusal = refusal;
  }
  if (refusal != PaintLayersSourceGroupRefusal::None) {
    return PaintLayerMaterialLiveStatus::Refused;
  }
  if (mode == PaintLayerMaterialMode::Baked) {
    return PaintLayerMaterialLiveStatus::Baked;
  }
  /* Live because the bake cannot be shown yet, while the row is out of the active chain: the maps
   * are still being rendered (or stale), so what is shown is the source until they land. */
  if (layer.bake != nullptr && layer.bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
      !BKE_paint_layers_bake_row_is_deferred(ma, layer) &&
      !BKE_paint_layers_material_bake_ready(ma, layer))
  {
    return PaintLayerMaterialLiveStatus::Baking;
  }
  return PaintLayerMaterialLiveStatus::Live;
}

bNodeTree *BKE_paint_layers_source_group_ensure(Main &bmain,
                                                 Material &owner,
                                                 Material &source,
                                                 PaintLayersSourceGroupRefusal &r_refusal,
                                                 bool *r_changed,
                                                 bool *r_values_synced,
                                                 const bool mapped)
{
  if (r_changed != nullptr) {
    *r_changed = false;
  }
  if (r_values_synced != nullptr) {
    *r_values_synced = false;
  }
  r_refusal = PaintLayersSourceGroupRefusal::None;
  if (&source == &owner) {
    r_refusal = PaintLayersSourceGroupRefusal::SelfReference;
    return nullptr;
  }
  if (source.nodetree == nullptr) {
    r_refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
    return nullptr;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(source, reason, &group_path);
  if (principled == nullptr) {
    r_refusal = PaintLayersSourceGroupRefusal::NoPrincipled;
    return nullptr;
  }
  if (int(group_path.size()) > PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH) {
    r_refusal = PaintLayersSourceGroupRefusal::PrincipledInGroup;
    return nullptr;
  }

  if (BLI_uuid_is_nil(owner.paint_layers_owner_uid)) {
    owner.paint_layers_owner_uid = BLI_uuid_generate_random();
  }
  PL_HASH_CALLER("generate_source_group");
  const uint64_t source_values = BKE_paint_layers_source_material_tree_hash(source);
  uint64_t source_topology = BKE_paint_layers_source_material_topology_hash(source);
  /* The owner's UV layer name decides the UV Map wiring inside the wrapper, so it is part of what
   * the wrapper is built from: a change rebuilds it, the same name keeps it. */
  topology_hash_string(source_topology, BKE_paint_layers_uv_map_name(owner));
  /* The remap-all flag changes which UV readers get a Mapping, so it is part of the topology too. */
  if ((owner.paint_layers_flag & MA_PAINT_LAYERS_REMAP_ALL_UV) != 0) {
    /* Mixed only when set, so the flag-off hash stays what older files stored. */
    source_topology = topology_hash_mix(source_topology, 2);
  }
  /* Whether the row mapping is applied at all (ТЗ 2.2): on means the wrapper carries Mapping
   * nodes and the three value inputs, off means none of them. Toggling the mapping on any row of
   * this source therefore rebuilds the wrapper once. */
  source_topology = topology_hash_mix(source_topology, mapped ? 1 : 0);
  const int source_uid = int(source.id.session_uid);
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, ".PL Source %s", source.id.name + 2);

  /* The cache key is (owner, source): two owners wrapping one source get their own groups. */
  bNodeTree *existing = nullptr;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (!BLI_uuid_equal(tree_owner_uid_get(tree), owner.paint_layers_owner_uid)) {
      continue;
    }
    if (prop_int_get(tree.id.properties, TREE_SOURCE_PROP, 0) != source_uid) {
      continue;
    }
    existing = &tree;
    break;
  }
  if (existing != nullptr) {
    uint64_t stored_topology = 0;
    uint64_t stored_values = 0;
    const bool have_topology = tree_hash_get(
        *existing, TREE_SOURCE_HASH_LOW_PROP, TREE_SOURCE_HASH_HIGH_PROP, stored_topology);
    const bool have_values = tree_hash_get(
        *existing, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, stored_values);
    if (have_topology && stored_topology == source_topology) {
      if (have_values && stored_values == source_values) {
        return existing;
      }
      /* Same topology, new values: copy them into the existing copies. No rebuild, so the wrapper
       * keeps its ID, its nodes and its generated interface, and the shader is not recompiled. */
      const Vector<SourceGroupChannel> channels = source_group_channels(*principled);
      [[maybe_unused]] const int synced = source_group_values_sync_tree(
          bmain, *source.nodetree, *existing, principled, channels, true);
      tree_hash_set(
          *existing, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, source_values);
      DEG_id_tag_update(&owner.id, ID_RECALC_SYNC_TO_EVAL);
      PL_DEBUG_PRINTF("paint layers: source group '%s' for owner '%s' values synced nodes=%d\n",
                      source.id.name + 2,
                      owner.id.name + 2,
                      synced);
      if (r_values_synced != nullptr) {
        *r_values_synced = true;
      }
      return existing;
    }
    /* The rebuild replaces the copy of every group on the path with a fresh one, so the previous
     * copies lose their only user (the instance node that pointed at them) and would leak. Collect
     * them now, before the build; delete the emptied ones once it is done. */
    Vector<bNodeTree *> old_copies;
    for (bNodeTree &tree : bmain.nodetrees) {
      if (&tree == existing ||
          !BLI_uuid_equal(tree_owner_uid_get(tree), owner.paint_layers_owner_uid) ||
          prop_int_get(tree.id.properties, TREE_SOURCE_PROP, 0) != source_uid)
      {
        continue;
      }
      old_copies.append(&tree);
    }
    if (!source_group_build(bmain, *existing, owner, source, principled, group_path, source_uid, mapped)) {
      r_refusal = PaintLayersSourceGroupRefusal::BuildFailed;
      return nullptr;
    }
    tree_hash_set(*existing, TREE_SOURCE_HASH_LOW_PROP, TREE_SOURCE_HASH_HIGH_PROP, source_topology);
    tree_hash_set(
        *existing, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, source_values);
    if (!STREQ(existing->id.name + 2, name)) {
      BKE_id_rename(bmain, existing->id, name);
    }
    /* Several passes: deleting a copy frees the instance nodes inside it, which drops the user of
     * the next copy down and lets the following pass delete that one too. A new copy is still
     * referenced by its instance, so its real users keep it. */
    int removed_copies = 0;
    for (int pass = 0; pass < PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH && !old_copies.is_empty(); pass++) {
      Vector<bNodeTree *> remaining;
      bool deleted = false;
      for (bNodeTree *copy : old_copies) {
        if (ID_REAL_USERS(&copy->id) <= 0) {
          BKE_id_delete(&bmain, copy);
          deleted = true;
          removed_copies++;
        }
        else {
          remaining.append(copy);
        }
      }
      old_copies = std::move(remaining);
      if (!deleted) {
        break;
      }
    }
    PL_DEBUG_PRINTF(
        "paint layers: source group '%s' for owner '%s' %s hash=%llx path_depth=%d "
        "removed_copies=%d\n",
        source.id.name + 2,
        owner.id.name + 2,
        "rebuilt",
        static_cast<unsigned long long>(source_topology),
        int(group_path.size()),
        removed_copies);
    if (r_changed != nullptr) {
      *r_changed = true;
    }
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }
  /* The only user will be the instance node, which adds its own reference when assigned. */
  id_us_min(&group->id);
  tree_owner_uid_set(*group, owner.paint_layers_owner_uid);
  prop_int_set(group->id.properties, TREE_SOURCE_PROP, source_uid);
  tree_hash_set(*group, TREE_SOURCE_HASH_LOW_PROP, TREE_SOURCE_HASH_HIGH_PROP, source_topology);
  tree_hash_set(
      *group, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, source_values);
  if (!source_group_build(bmain, *group, owner, source, principled, group_path, source_uid, mapped)) {
    BKE_id_free(&bmain, group);
    r_refusal = PaintLayersSourceGroupRefusal::BuildFailed;
    return nullptr;
  }
  PL_DEBUG_PRINTF(
      "paint layers: source group '%s' for owner '%s' %s hash=%llx path_depth=%d "
      "removed_copies=%d\n",
      source.id.name + 2,
      owner.id.name + 2,
      "created",
      static_cast<unsigned long long>(source_topology),
      int(group_path.size()),
      0);
  if (r_changed != nullptr) {
    *r_changed = true;
  }
  return group;
}

}  // namespace blender

