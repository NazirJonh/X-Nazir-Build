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
#include "BLI_string_ref.hh"
#include "BLI_ustring.hh"
#include "BLI_vector.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_source_graph.hh"
#include "BKE_paint_material_resolve.hh"

#include "DNA_ID.h"
#include "DNA_color_types.h"
#include "DNA_genfile.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"

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

/**
 * Hash the reference to \a id the way the stored hashes need it to be stable: name and owning
 * library, never `session_uid`, which is reborn every session and would make a hash stored in the
 * file mismatch after every load. The name is the file-stable identity a data-block keeps, a packed
 * image included; a rename or a re-link from another library moving the hash is the wanted answer,
 * since the graph then reads something else.
 */
static uint64_t source_hash_id_reference(uint64_t h, const ID *id)
{
  if (id == nullptr) {
    return hash_mix(h, 0);
  }
  h = source_hash_string(h, id->name);
  if (id->lib != nullptr) {
    h = source_hash_string(h, id->lib->filepath);
  }
  return h;
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
                                              uint64_t &r_hash,
                                              bool include_sockets);

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
  source_tree_values_hash_recursive(*ma.nodetree, value_visited, hash, true);
  return hash;
}

uint64_t BKE_paint_layers_source_material_structure_hash(const Material &ma)
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
  source_tree_values_hash_recursive(*ma.nodetree, value_visited, hash, false);
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
                                              uint64_t &r_hash,
                                              const bool include_sockets)
{
  if (!visited.add(&tree)) {
    return;
  }
  tree.ensure_interface_cache();
  if (include_sockets) {
    for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
      r_hash = source_hash_interface_socket(r_hash, *socket);
    }
    for (const bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
      r_hash = source_hash_interface_socket(r_hash, *socket);
    }
  }
  for (const bNode *node : tree.all_nodes()) {
    r_hash = hash_mix(r_hash, uint16_t(node->custom1));
    r_hash = hash_mix(r_hash, uint16_t(node->custom2));
    r_hash = hash_float(r_hash, node->custom3);
    r_hash = hash_float(r_hash, node->custom4);
    r_hash = node->id != nullptr ? source_hash_string(r_hash, node->id->name) :
                                   hash_mix(r_hash, 0);
    r_hash = source_hash_node_storage(r_hash, *node);
    if (!include_sockets) {
      continue;
    }
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
      source_tree_values_hash_recursive(*group_tree, visited, r_hash, include_sockets);
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

/** One memoized answer of a source hash, keyed by `ID::session_uid` and valid while the node tree
 * signal it was made from stands. All the maps share #source_hash_memo_mutex() and register with
 * #source_memo_registry(), so one forget clears a material's entry from every one of them. */
class SourceMemoMapBase {
 public:
  virtual ~SourceMemoMapBase() = default;
  virtual void remove(uint32_t session_uid) = 0;
  virtual void clear_and_shrink() = 0;
};

/** Every memo above, so one forget reaches them all. Guarded by #source_hash_memo_mutex(); the
 * memos register themselves from their (mutex-guarded) first use, and entries a memo has not made
 * yet are entries a forget cannot miss. */
Vector<SourceMemoMapBase *> &source_memo_registry()
{
  static Vector<SourceMemoMapBase *> registry;
  return registry;
}

/**
 * \tparam Answer: what the memo keeps beside the signal. \tparam ZeroIsAnAnswer: whether a zero
 * answer is an answer. It is for the channel-graph memo, whose zero is the content "the graph
 * feeds nothing"; the stored-hash memos take the other branch, where a zero is the "no claim"
 * placeholder no answer of theirs may be mistaken for.
 */
template<typename Answer, bool ZeroIsAnAnswer = false> class SourceMemoMap final
    : public SourceMemoMapBase {
 public:
  struct Entry {
    uint64_t signal = 0;
    Answer answer{};
  };

  const Entry *lookup(const uint32_t session_uid, const uint64_t signal) const
  {
    const Entry *entry = map_.lookup_ptr(session_uid);
    if (entry == nullptr || entry->signal != signal) {
      return nullptr;
    }
    if constexpr (!ZeroIsAnAnswer) {
      if (entry->answer == 0) {
        return nullptr;
      }
    }
    return entry;
  }

  void add_overwrite(const uint32_t session_uid, const uint64_t signal, const Answer answer)
  {
    map_.add_overwrite(session_uid, Entry{signal, answer});
  }

  void remove(const uint32_t session_uid) override
  {
    map_.remove(session_uid);
  }

  void clear_and_shrink() override
  {
    map_ = Map<uint32_t, Entry>();
  }

 private:
  Map<uint32_t, Entry> map_;
};

SourceMemoMap<uint64_t> &source_hash_memo()
{
  static SourceMemoMap<uint64_t> memo;
  return memo;
}

/**
 * Guards the memos above. The main thread reads and writes them, and the free of an evaluated
 * copy (which shares the session UID of its original) forgets an entry from wherever the depsgraph
 * frees it. Held around the maps only, never around a walk of the trees.
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
  /** Some level the source group copies reads the active UVs (#paint_layer_source_reads_uv_compute),
   * asked on every redraw by the mapping panel. */
  bool reads_uv = false;
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
    if (const SourceMemoMap<uint64_t>::Entry *memo = source_hash_memo().lookup(ma.id.session_uid,
                                                                               signal))
    {
      return memo->answer;
    }
  }
  const uint64_t content = BKE_paint_layers_source_material_tree_hash(ma);
  std::lock_guard lock(source_hash_memo_mutex());
  source_hash_memo().add_overwrite(ma.id.session_uid, signal, content);
  return content;
}

/* -------------------------------------------------------------------- */
/** \name Normal hash
 *
 * The part of the source a rendered Normal map depends on: everything upstream of the Principled
 * BSDF's Normal and of the Material Output's Displacement, followed through Group nodes (into
 * their output, and back out through their Group Input to the instance's inputs). Moving a colour
 * or a roughness value elsewhere in the source leaves it untouched, so such an edit renders nothing.
 * \{ */

struct NormalHashWalk {
  Set<uint64_t> seen;
  uint64_t hash = 0;
  /** Set when the walk met a node whose result depends on the object or the camera. */
  bool object_dependent = false;
};

static void normal_hash_input(NormalHashWalk &walk,
                              const bNodeSocket &input,
                              Vector<const bNode *> &context);

static void normal_hash_node(NormalHashWalk &walk, const bNode &node)
{
  uint64_t &h = walk.hash;
  h = hash_mix(h, uint32_t(node.type_legacy));
  h = source_hash_string(h, node.name);
  h = hash_mix(h, uint16_t(node.custom1));
  h = hash_mix(h, uint16_t(node.custom2));
  h = hash_float(h, node.custom3);
  h = hash_float(h, node.custom4);
  /* Name and library, not a session uid: the channel-graph hash is stored in the file and compared
   * after a load (see #BKE_paint_layers_source_channel_graph_hash). */
  h = source_hash_id_reference(h, node.id);
  h = source_hash_node_storage(h, node);
  h = hash_mix(h, node.is_muted() ? 1 : 0);
  /* A coarse test on purpose: it names the node kinds a UV-quad bake cannot reproduce faithfully
   * (Texture Coordinate counts although its UV output is safe), so a caller warns rather than
   * promises the pixels. */
  if (ELEM(node.type_legacy,
           SH_NODE_TEX_COORD,
           SH_NODE_NEW_GEOMETRY,
           SH_NODE_OBJECT_INFO,
           SH_NODE_CAMERA,
           SH_NODE_LIGHT_PATH))
  {
    walk.object_dependent = true;
  }
}

static void normal_hash_output(NormalHashWalk &walk,
                               const bNode &node,
                               const bNodeSocket &output,
                               Vector<const bNode *> &context)
{
  /* A route helper the generator minted is not the author's content: a regeneration recreates it
   * (with a fresh name), so hashing it would read every such rebuild as a graph change. The walk
   * follows through it without hashing it, the same transparency the generated chain gives it.
   * The ownership marker alone is not the test -- a duplicate the user made of a helper carries it
   * too (system properties are copied) -- so the minted record answers as well, the same pair
   * #BKE_paint_layers_revert_to_plain removes by. */
  if (bke::paint_layers::node_is_generator_owned(node) &&
      bke::paint_layers::response_node_is_created(node.owner_tree(), node))
  {
    for (const bNodeSocket &input : node.inputs) {
      if (!input.is_directly_linked()) {
        walk.hash = source_hash_socket_value(walk.hash, input.type, input.default_value);
        continue;
      }
      for (const bNodeLink *link : input.directly_linked_links()) {
        if (link->fromnode == nullptr || link->fromsock == nullptr) {
          continue;
        }
        normal_hash_output(walk, *link->fromnode, *link->fromsock, context);
      }
    }
    return;
  }
  const uint64_t key = hash_mix(
      hash_mix(hash_mix(uint64_t(intptr_t(&output)), context.size()),
               uint64_t(intptr_t(context.is_empty() ? nullptr : context.last()))),
      0x6e6f726du);
  if (!walk.seen.add(key)) {
    return;
  }
  walk.hash = source_hash_string(walk.hash, output.identifier);
  if (node.type_legacy == NODE_GROUP_INPUT) {
    if (context.is_empty()) {
      return;
    }
    /* Out of the group: what the instance's input of the same identifier is fed by. */
    const bNode *instance = context.pop_last();
    if (const bNodeSocket *outer = bke::node_find_socket(
            *instance, SOCK_IN, UString::from_ptr_noinline(output.identifier)))
    {
      normal_hash_input(walk, *outer, context);
    }
    context.append(instance);
    return;
  }
  if (node.is_group() && node.id != nullptr) {
    const bNodeTree *group_tree = id_cast<const bNodeTree *>(node.id);
    if (group_tree == nullptr) {
      return;
    }
    group_tree->ensure_topology_cache();
    const bNode *group_output = group_tree->group_output_node();
    if (group_output == nullptr) {
      return;
    }
    /* Into the group: what its output of the same identifier is fed by. */
    if (const bNodeSocket *inner = bke::node_find_socket(
            *group_output, SOCK_IN, UString::from_ptr_noinline(output.identifier)))
    {
      context.append(&node);
      normal_hash_input(walk, *inner, context);
      context.pop_last();
    }
    return;
  }
  normal_hash_node(walk, node);
  for (const bNodeSocket &input : node.inputs) {
    normal_hash_input(walk, input, context);
  }
}

static void normal_hash_input(NormalHashWalk &walk,
                              const bNodeSocket &input,
                              Vector<const bNode *> &context)
{
  walk.hash = source_hash_string(walk.hash, input.identifier);
  if (!input.is_directly_linked()) {
    walk.hash = source_hash_socket_value(walk.hash, input.type, input.default_value);
    return;
  }
  for (const bNodeLink *link : input.directly_linked_links()) {
    if (link->fromnode == nullptr || link->fromsock == nullptr) {
      continue;
    }
    walk.hash = hash_mix(walk.hash, link->is_muted() ? 1 : 0);
    normal_hash_output(walk, *link->fromnode, *link->fromsock, context);
  }
}

static uint64_t source_normal_hash(const Material &ma)
{
  if (ma.nodetree == nullptr) {
    return 0;
  }
  ma.nodetree->ensure_topology_cache();
  NormalHashWalk walk;
  Vector<const bNode *> context;
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason, &group_path);
  if (principled != nullptr) {
    principled->owner_tree().ensure_topology_cache();
    for (const bNode *instance : group_path) {
      instance->owner_tree().ensure_topology_cache();
      context.append(instance);
    }
    if (const bNodeSocket *normal = bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr)) {
      normal_hash_input(walk, *normal, context);
    }
  }
  context.clear();
  for (const bNode *node : ma.nodetree->all_nodes()) {
    if (node->type_legacy != SH_NODE_OUTPUT_MATERIAL) {
      continue;
    }
    if (const bNodeSocket *displacement = bke::node_find_socket(
            *node, SOCK_IN, "Displacement"_ustr))
    {
      normal_hash_input(walk, *displacement, context);
    }
  }
  return walk.hash == 0 ? 1 : walk.hash;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Channel-graph hash
 *
 * The part of the source the material's own CPU cache depends on: what feeds each
 * `Source <channel>` input of the generated instance. The walk is the Normal one at heart --
 * upstream from a socket, through groups, muted links and reroutes -- but it starts at the
 * instance's Source inputs and follows the generator's route helpers through without hashing them,
 * so a regeneration that recreates them does not read as a graph change. Nothing else of the tree
 * takes part: the stack's own rows, the viewer nodes and the routing downstream of the instance
 * are outside the walk, which is why editing the stack leaves the cache's validity alone.
 * \{ */

static uint64_t source_channel_graph_hash(const Material &ma, bool *r_object_dependent)
{
  if (r_object_dependent != nullptr) {
    *r_object_dependent = false;
  }
  if (ma.nodetree == nullptr) {
    return 0;
  }
  ma.nodetree->ensure_topology_cache();
  bNode *instance = bke::paint_layers::instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr || ma.paint_layers_tree == nullptr) {
    return 0;
  }
  NormalHashWalk walk;
  Vector<const bNode *> context;
  /* The Source inputs the interface carries are matched the way the wiring matches them: the
   * interface socket by name, the instance socket by its identifier (see #instance_input_find). */
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    char source_name[64];
    bke::paint_layers::source_input_name_get(info, source_name);
    const bNodeSocket *input = bke::paint_layers::instance_input_find(
        *ma.paint_layers_tree, *instance, source_name);
    if (input == nullptr || !input->is_directly_linked()) {
      /* An unlinked Source input reads its neutral constant -- the graph feeds that channel
       * nothing, there is nothing to cache, and claiming it in the hash would leave a material no
       * bake can satisfy. Only what the graph actually feeds is the content. */
      continue;
    }
    /* The input's identity is part of the content: a stack that stops wiring a channel drops its
     * Source input, and the graph the cache was baked from is no longer the one the stack reads. */
    walk.hash = source_hash_string(walk.hash, input->identifier);
    normal_hash_input(walk, *input, context);
  }
  if (r_object_dependent != nullptr) {
    *r_object_dependent = walk.object_dependent;
  }
  /* Zero is a valid content here: the graph feeds no adopted channel, no cache belongs to it, and
   * the stored hash reads zero with it (see #Material::paint_layers_source_hash). */
  return walk.hash;
}

/** \} */

/** The channel-graph memo keeps the object-dependence the walk met beside the content. */
struct SourceChannelGraphAnswer {
  uint64_t content = 0;
  bool object_dependent = false;
};

/** Guarded by #source_hash_memo_mutex(), like the full hash's memo. */
static SourceMemoMap<uint64_t> &source_normal_hash_memo()
{
  static SourceMemoMap<uint64_t> memo;
  return memo;
}

/** Guarded by #source_hash_memo_mutex(), like the full hash's memo. */
static SourceMemoMap<uint64_t> &source_structure_hash_memo()
{
  static SourceMemoMap<uint64_t> memo;
  return memo;
}

/** Guarded by #source_hash_memo_mutex(), like the full hash's memo. Zero is a content here
 * (the graph feeds nothing), so the map keeps a zero answer instead of recomputing it. */
static SourceMemoMap<SourceChannelGraphAnswer, true> &source_channel_graph_memo()
{
  static SourceMemoMap<SourceChannelGraphAnswer, true> memo;
  return memo;
}

uint64_t BKE_paint_layers_source_structure_hash_cached(const Material &ma)
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
    if (const SourceMemoMap<uint64_t>::Entry *entry = source_structure_hash_memo().lookup(
            ma.id.session_uid, signal))
    {
      return entry->answer;
    }
  }
  const uint64_t content = BKE_paint_layers_source_material_structure_hash(ma);
  std::lock_guard lock(source_hash_memo_mutex());
  source_structure_hash_memo().add_overwrite(ma.id.session_uid, signal, content);
  return content;
}

uint64_t BKE_paint_layers_source_normal_hash_cached(const Material &ma)
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
    if (const SourceMemoMap<uint64_t>::Entry *entry = source_normal_hash_memo().lookup(
            ma.id.session_uid, signal))
    {
      return entry->answer;
    }
  }
  const uint64_t content = source_normal_hash(ma);
  std::lock_guard lock(source_hash_memo_mutex());
  source_normal_hash_memo().add_overwrite(ma.id.session_uid, signal, content);
  return content;
}

uint64_t BKE_paint_layers_source_channel_graph_hash(const Material &ma)
{
  return source_channel_graph_hash(ma, nullptr);
}

/** The memoized answer for \a ma's channel graph, recomputed when the signal moved. */
static SourceChannelGraphAnswer source_channel_graph_memo_entry(const Material &ma)
{
  SourceChannelGraphAnswer none;
  if (ma.nodetree == nullptr) {
    return none;
  }
  ma.nodetree->ensure_topology_cache();
  Set<const bNodeTree *> visited;
  uint64_t signal = 0;
  source_signal_recursive(*ma.nodetree, visited, signal);
  {
    std::lock_guard lock(source_hash_memo_mutex());
    if (const SourceMemoMap<SourceChannelGraphAnswer, true>::Entry *entry =
            source_channel_graph_memo().lookup(ma.id.session_uid, signal))
    {
      return entry->answer;
    }
  }
  SourceChannelGraphAnswer answer;
  answer.content = source_channel_graph_hash(ma, &answer.object_dependent);
  std::lock_guard lock(source_hash_memo_mutex());
  source_channel_graph_memo().add_overwrite(ma.id.session_uid, signal, answer);
  return answer;
}

uint64_t BKE_paint_layers_source_channel_graph_hash_cached(const Material &ma)
{
  return source_channel_graph_memo_entry(ma).content;
}

int BKE_paint_layers_source_graph_feeds_find(const Material &ma,
                                             MutableSpan<PaintLayersSourceChannelFeed> r_feeds)
{
  BLI_assert(r_feeds.size() >= PAINT_MATERIAL_CHANNEL_NUM);
  int found = 0;
  if (ma.nodetree == nullptr) {
    return 0;
  }
  ma.nodetree->ensure_topology_cache();
  bNode *instance = bke::paint_layers::instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr || ma.paint_layers_tree == nullptr) {
    return 0;
  }
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!bke::paint_layers::source_channel_adoptable(int(info.channel))) {
      continue;
    }
    char source_name[64];
    bke::paint_layers::source_input_name_get(info, source_name);
    const bNodeSocket *socket = bke::paint_layers::instance_input_find(
        *ma.paint_layers_tree, *instance, source_name);
    if (socket == nullptr || !socket->is_directly_linked()) {
      continue;
    }
    r_feeds[found++] = PaintLayersSourceChannelFeed{info.channel, socket};
  }
  return found;
}

bool BKE_paint_layers_source_graph_object_dependent(const Material &ma)
{
  return source_channel_graph_memo_entry(ma).object_dependent;
}

void BKE_paint_layers_source_memo_free_all()
{
  {
    std::lock_guard lock(source_hash_memo_mutex());
    for (SourceMemoMapBase *memo : source_memo_registry()) {
      memo->clear_and_shrink();
    }
  }
  std::lock_guard lock(source_resolve_memo_mutex());
  source_resolve_memo() = Map<const bNodeTree *, SourceResolveMemo>();
}

void BKE_paint_layers_source_material_hash_forget(const Material &ma)
{
  {
    std::lock_guard lock(source_hash_memo_mutex());
    /* Every memo the file keeps, through the one registry: a new memo registers itself there,
     * so the forget cannot grow a hole by missing it. */
    for (SourceMemoMapBase *memo : source_memo_registry()) {
      memo->remove(ma.id.session_uid);
    }
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
  entry.reads_uv = paint_layer_source_reads_uv_compute(source);
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

bool BKE_paint_layers_source_reads_uv(const Material &source)
{
  return source_resolve_entry(source).reads_uv;
}

/** \} */

}  // namespace blender
