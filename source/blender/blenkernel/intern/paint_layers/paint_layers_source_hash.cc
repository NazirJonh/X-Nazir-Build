/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The content hash of a source material: what a Material row of a layered material compares to know
 * that the maps it baked still describe its source.
 */

#include <cstring>
#include <memory>
#include <mutex>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_resolve.hh"

#include "DNA_ID.h"
#include "DNA_color_types.h"
#include "DNA_genfile.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

namespace blender {

static uint64_t hash_mix(uint64_t h, const uint64_t value)
{
  return h * 1000003u ^ value;
}

static uint64_t hash_float(uint64_t h, const float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return hash_mix(h, bits);
}

/* The source hash is a hash of the source's *content*: topology plus the values that change what
 * the graph evaluates to. It deliberately does not read `runtime->previews_refresh_state`, which
 * any node tree update bumps -- including an update of a *different* tree that only shares a nested
 * group with this one. Reading it made the hash move whenever the layered tree or the bake's
 * localized copy was updated, which drove the editor's "source edited -> rebuild -> re-bake" loop.
 *
 * Included: node types/names, links with socket identifiers and mute state, group IDs, group
 * interfaces (topology), plus node custom1/custom2/custom3/custom4, `node->id`, the node's DNA
 * storage bytes, the default values of every input socket, and the default values of group
 * interface sockets. Excluded: location, parent, selection and UI flags. */

static uint64_t source_hash_string(uint64_t h, const char *text)
{
  if (text == nullptr) {
    return hash_mix(h, 0);
  }
  for (const char *c = text; *c != '\0'; c++) {
    h = hash_mix(h, uint8_t(*c));
  }
  return hash_mix(h, 1);
}

/** Hash the scalar \a data of a socket of \a type, as #bNodeSocket::default_value stores it. */
static uint64_t source_hash_socket_value(uint64_t h,
                                         const eNodeSocketDatatype type,
                                         const void *data)
{
  if (data == nullptr) {
    return hash_mix(h, 0xFFFFFFFFu);
  }
  switch (type) {
    case SOCK_FLOAT:
      return hash_float(h, static_cast<const bNodeSocketValueFloat *>(data)->value);
    case SOCK_INT:
      return hash_mix(h, uint32_t(static_cast<const bNodeSocketValueInt *>(data)->value));
    case SOCK_BOOLEAN:
      return hash_mix(
          h, uint32_t(static_cast<const bNodeSocketValueBoolean *>(data)->value));
    case SOCK_VECTOR: {
      const bNodeSocketValueVector *value = static_cast<const bNodeSocketValueVector *>(data);
      for (const int i : IndexRange(3)) {
        h = hash_float(h, value->value[i]);
      }
      return hash_mix(h, uint32_t(value->dimensions));
    }
    case SOCK_INT_VECTOR: {
      const bNodeSocketValueIntVector *value = static_cast<const bNodeSocketValueIntVector *>(data);
      for (const int i : IndexRange(3)) {
        h = hash_mix(h, uint32_t(value->value[i]));
      }
      return hash_mix(h, uint32_t(value->dimensions));
    }
    case SOCK_RGBA: {
      const bNodeSocketValueRGBA *value = static_cast<const bNodeSocketValueRGBA *>(data);
      for (const int i : IndexRange(4)) {
        h = hash_float(h, value->value[i]);
      }
      return h;
    }
    case SOCK_ROTATION: {
      const bNodeSocketValueRotation *value = static_cast<const bNodeSocketValueRotation *>(data);
      for (const int i : IndexRange(3)) {
        h = hash_float(h, value->value_euler[i]);
      }
      return h;
    }
    case SOCK_STRING:
      return source_hash_string(h, static_cast<const bNodeSocketValueString *>(data)->value);
    case SOCK_MENU:
      return hash_mix(h, uint32_t(static_cast<const bNodeSocketValueMenu *>(data)->value));
    default:
      /* Datablock, shader, geometry and other pointer-backed sockets carry no scalar value. */
      return hash_mix(h, 0xFFFFFFFEu);
  }
}

/**
 * Hash a #CurveMapping's points and ranges. Its DNA bytes only carry pointers to the allocated
 * points, so the raw-byte route would freeze the hash on the allocation address and miss a curve
 * edit. Used by the RGB/Vector/Float Curves shader nodes.
 */
static uint64_t source_hash_curve_mapping(uint64_t h, const CurveMapping &mapping)
{
  h = hash_mix(h, uint32_t(mapping.flag));
  h = hash_mix(h, uint32_t(mapping.cur));
  h = hash_mix(h, uint32_t(mapping.preset));
  h = hash_mix(h, uint32_t(mapping.tone));
  for (const CurveMap &map : mapping.cm) {
    h = hash_mix(h, uint32_t(uint16_t(map.totpoint)));
    h = hash_mix(h, uint32_t(int16_t(map.flag)));
    for (int point = 0; point < map.totpoint && map.curve != nullptr; point++) {
      const CurveMapPoint &pt = map.curve[point];
      h = hash_float(h, pt.x);
      h = hash_float(h, pt.y);
      h = hash_mix(h, uint32_t(uint16_t(pt.flag)));
    }
  }
  for (const int i : IndexRange(3)) {
    h = hash_float(h, mapping.black[i]);
    h = hash_float(h, mapping.white[i]);
    h = hash_float(h, mapping.bwmul[i]);
  }
  return h;
}

/**
 * Hash a node's DNA \a storage, sized through the runtime SDNA by the type's `storagename`.
 *
 * There is no shared hash/compare helper for node storage in the code base; this is the generic
 * route the file read/write code already uses to know the struct. Storage is allocated zeroed and
 * is not reallocated by node updates, so raw bytes are stable across the wrapper rebuilds this
 * hash has to ignore. #CurveMapping is the exception: its bytes hold pointers, so it is hashed by
 * its points instead.
 */
static uint64_t source_hash_node_storage(uint64_t h, const bNode &node)
{
  if (node.storage == nullptr || node.typeinfo == nullptr || node.typeinfo->storagename.empty()) {
    return hash_mix(h, 0);
  }
  if (node.typeinfo->storagename == "CurveMapping") {
    return source_hash_curve_mapping(h, *static_cast<const CurveMapping *>(node.storage));
  }
  const SDNA *sdna = DNA_sdna_current_get();
  const int struct_index = DNA_struct_find_index_without_alias(
      sdna, node.typeinfo->storagename.c_str());
  if (struct_index < 0) {
    return hash_mix(h, 1);
  }
  const int size = DNA_struct_size(sdna, struct_index);
  const uint8_t *bytes = static_cast<const uint8_t *>(node.storage);
  h = hash_mix(h, uint32_t(size));
  for (const int i : IndexRange(size)) {
    h = hash_mix(h, bytes[i]);
  }
  return h;
}


static void source_tree_topology_hash_recursive(const bNodeTree &tree,
                                                Set<const bNodeTree *> &visited,
                                                uint64_t &r_hash);
static void source_tree_values_hash_recursive(const bNodeTree &tree,
                                              Set<const bNodeTree *> &visited,
                                              uint64_t &r_hash);

/** The content hash of \a ma's node tree, or zero when it has none. */
uint64_t BKE_paint_layers_source_material_tree_hash(const Material &ma)
{
  if (ma.nodetree == nullptr) {
    return 0;
  }
  ma.nodetree->ensure_topology_cache();
  Set<const bNodeTree *> visited;
  uint64_t topology_hash = 0;
  source_tree_topology_hash_recursive(*ma.nodetree, visited, topology_hash);
  uint64_t hash = hash_mix(0, topology_hash);
  Set<const bNodeTree *> value_visited;
  source_tree_values_hash_recursive(*ma.nodetree, value_visited, hash);
  return hash;
}

/**
 * A topology-only hash of \a tree and every group it reaches, once each: node types and names, the
 * links with their socket identifiers and muted state, the group IDs and every group interface.
 * Values are deliberately left out -- socket defaults, node storage, custom properties and
 * locations -- so moving a slider does not move this. The source-group wrapper compares it to
 * decide between a rebuild (topology) and a sync of values in place.
 */
static void source_tree_topology_hash_recursive(const bNodeTree &tree,
                                                Set<const bNodeTree *> &visited,
                                                uint64_t &r_hash)
{
  if (!visited.add(&tree)) {
    return;
  }
  auto hash_string = [&](const char *text) {
    if (text == nullptr) {
      r_hash = hash_mix(r_hash, 0);
      return;
    }
    for (const char *c = text; *c != '\0'; c++) {
      r_hash = hash_mix(r_hash, uint8_t(*c));
    }
    r_hash = hash_mix(r_hash, 1);
  };

  for (const bNode *node : tree.all_nodes()) {
    r_hash = hash_mix(r_hash, uint32_t(node->typeinfo != nullptr ? node->type_legacy : 0));
    hash_string(node->typeinfo != nullptr ? node->typeinfo->idname.c_str() : nullptr);
    hash_string(node->name);
    if (node->is_group() && node->id != nullptr) {
      r_hash = hash_mix(r_hash, node->id->session_uid);
    }
  }
  for (const bNodeLink &link : tree.links) {
    if (link.fromnode == nullptr || link.tonode == nullptr || link.fromsock == nullptr ||
        link.tosock == nullptr)
    {
      continue;
    }
    hash_string(link.fromnode->name);
    hash_string(link.fromsock->identifier);
    hash_string(link.tonode->name);
    hash_string(link.tosock->identifier);
    r_hash = hash_mix(r_hash, link.is_muted() ? 1 : 0);
  }
  tree.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    r_hash = hash_mix(r_hash, 1);
    hash_string(socket->identifier);
    hash_string(socket->socket_type);
  }
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    r_hash = hash_mix(r_hash, 2);
    hash_string(socket->identifier);
    hash_string(socket->socket_type);
  }
  for (const bNode *node : tree.all_nodes()) {
    if (!node->is_group() || node->id == nullptr) {
      continue;
    }
    if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
      source_tree_topology_hash_recursive(*group_tree, visited, r_hash);
    }
  }
}

/** Hash a group interface socket's default value, typed through its socket typeinfo. */
static uint64_t source_hash_interface_socket(uint64_t h, const bNodeTreeInterfaceSocket &socket)
{
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  if (typeinfo == nullptr) {
    return hash_mix(h, 0xFFFFFFFDu);
  }
  return source_hash_socket_value(h, typeinfo->type, socket.socket_data);
}

/**
 * The value half of the content hash: node custom fields, `node->id`, DNA storage and the default
 * values of every input and interface socket of \a tree and every group it reaches, once each.
 * Locations, parents, selection and UI flags are left out on purpose.
 */
static void source_tree_values_hash_recursive(const bNodeTree &tree,
                                              Set<const bNodeTree *> &visited,
                                              uint64_t &r_hash)
{
  if (!visited.add(&tree)) {
    return;
  }
  tree.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    r_hash = source_hash_interface_socket(r_hash, *socket);
  }
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    r_hash = source_hash_interface_socket(r_hash, *socket);
  }
  for (const bNode *node : tree.all_nodes()) {
    r_hash = hash_mix(r_hash, uint16_t(node->custom1));
    r_hash = hash_mix(r_hash, uint16_t(node->custom2));
    r_hash = hash_float(r_hash, node->custom3);
    r_hash = hash_float(r_hash, node->custom4);
    r_hash = node->id != nullptr ? source_hash_string(r_hash, node->id->name) :
                                   hash_mix(r_hash, 0);
    r_hash = source_hash_node_storage(r_hash, *node);
    for (const bNodeSocket &socket : node->inputs) {
      r_hash = source_hash_socket_value(r_hash, socket.type, socket.default_value);
    }
    /* Output defaults carry the value of constant nodes such as Value and RGB. */
    for (const bNodeSocket &socket : node->outputs) {
      r_hash = source_hash_socket_value(r_hash, socket.type, socket.default_value);
    }
  }
  for (const bNode *node : tree.all_nodes()) {
    if (!node->is_group() || node->id == nullptr) {
      continue;
    }
    if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
      source_tree_values_hash_recursive(*group_tree, visited, r_hash);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Memo
 *
 * The content hash walks every node of the source and every group it reaches, and the main thread
 * asks for it on every scene update for every Material row. The cheap answer is a signal the node
 * tree update bumps on any change: while it is the same the content is the same, and the walk is
 * skipped. The signal may move without the content moving (a different tree sharing a group), which
 * only costs one walk.
 * \{ */

static void source_signal_recursive(const bNodeTree &tree,
                                    Set<const bNodeTree *> &visited,
                                    uint64_t &r_signal)
{
  if (!visited.add(&tree)) {
    return;
  }
  r_signal = hash_mix(r_signal, tree.runtime->previews_refresh_state);
  r_signal = hash_mix(r_signal, tree.runtime->output_topology_hash);
  for (const bNode *node : tree.all_nodes()) {
    if (!node->is_group() || node->id == nullptr) {
      continue;
    }
    if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
      source_signal_recursive(*group_tree, visited, r_signal);
    }
  }
}

namespace {

struct SourceHashMemo {
  uint64_t signal = 0;
  uint64_t content = 0;
};

Map<uint32_t, SourceHashMemo> &source_hash_memo()
{
  static Map<uint32_t, SourceHashMemo> memo;
  return memo;
}

/**
 * Guards #source_hash_memo(). The main thread reads and writes it, and the free of an evaluated
 * copy (which shares the session UID of its original) forgets an entry from wherever the depsgraph
 * frees it. Held around the map only, never around a walk of the trees.
 */
std::mutex &source_hash_memo_mutex()
{
  static std::mutex mutex;
  return mutex;
}

/**
 * What the resolver found in a source, by node tree address: an evaluated copy has trees (and
 * images) of its own, and the pointers inside the answer must be the ones of the tree it was made
 * from. The same signal as above says when the answer is out of date.
 */
struct SourceResolveMemo {
  uint64_t signal = 0;
  std::shared_ptr<const MaterialSourceResolve> resolve;
  /** The Principled BSDF is reached through at most #SOURCE_GROUP_MAX_DEPTH groups, so its graph
   * can be copied into a group. */
  bool group_wrappable = false;
};

Map<const bNodeTree *, SourceResolveMemo> &source_resolve_memo()
{
  static Map<const bNodeTree *, SourceResolveMemo> memo;
  return memo;
}

std::mutex &source_resolve_memo_mutex()
{
  static std::mutex mutex;
  return mutex;
}

}  // namespace

uint64_t BKE_paint_layers_source_material_tree_hash_cached(const Material &ma)
{
  if (ma.nodetree == nullptr) {
    return 0;
  }
  ma.nodetree->ensure_topology_cache();
  Set<const bNodeTree *> visited;
  uint64_t signal = 0;
  source_signal_recursive(*ma.nodetree, visited, signal);
  {
    std::lock_guard lock(source_hash_memo_mutex());
    if (const SourceHashMemo *memo = source_hash_memo().lookup_ptr(ma.id.session_uid)) {
      if (memo->signal == signal && memo->content != 0) {
        return memo->content;
      }
    }
  }
  const uint64_t content = BKE_paint_layers_source_material_tree_hash(ma);
  std::lock_guard lock(source_hash_memo_mutex());
  source_hash_memo().add_overwrite(ma.id.session_uid, SourceHashMemo{signal, content});
  return content;
}

void BKE_paint_layers_source_material_hash_forget(const Material &ma)
{
  {
    std::lock_guard lock(source_hash_memo_mutex());
    source_hash_memo().remove(ma.id.session_uid);
  }
  if (ma.nodetree != nullptr) {
    std::lock_guard lock(source_resolve_memo_mutex());
    source_resolve_memo().remove(ma.nodetree);
  }
}

/** The memoized answer for \a source, made when the signal moved. */
static SourceResolveMemo source_resolve_entry(const Material &source)
{
  if (source.nodetree == nullptr) {
    SourceResolveMemo none;
    none.resolve = std::make_shared<const MaterialSourceResolve>(
        BKE_paint_material_source_resolve(&source));
    return none;
  }
  source.nodetree->ensure_topology_cache();
  Set<const bNodeTree *> visited;
  uint64_t signal = 0;
  source_signal_recursive(*source.nodetree, visited, signal);
  {
    std::lock_guard lock(source_resolve_memo_mutex());
    if (const SourceResolveMemo *memo = source_resolve_memo().lookup_ptr(source.nodetree)) {
      if (memo->signal == signal) {
        return *memo;
      }
    }
  }
  SourceResolveMemo entry;
  entry.signal = signal;
  entry.resolve = std::make_shared<const MaterialSourceResolve>(
      BKE_paint_material_source_resolve(&source));
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  entry.group_wrappable = BKE_paint_material_principled_find(source, reason, &group_path) !=
                              nullptr &&
                          group_path.size() <= SOURCE_GROUP_MAX_DEPTH;
  std::lock_guard lock(source_resolve_memo_mutex());
  source_resolve_memo().add_overwrite(source.nodetree, entry);
  return entry;
}

std::shared_ptr<const MaterialSourceResolve> BKE_paint_layers_source_resolve(
    const Material &source)
{
  return source_resolve_entry(source).resolve;
}

bool BKE_paint_layers_source_group_wrappable(const Material &source)
{
  return source_resolve_entry(source).group_wrappable;
}

/** \} */

}  // namespace blender
