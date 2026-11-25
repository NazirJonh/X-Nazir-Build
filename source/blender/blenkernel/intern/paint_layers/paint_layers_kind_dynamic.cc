/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The dynamic kind schemas: the `PaintLayerKindInfo` the kinds flagged
 * #PAINT_LAYER_KIND_DYNAMIC_SCHEMA answer with, built from the definition the row reads rather
 * than from the registry table.
 *
 * A node-group kind's parameters are its tree's interface sockets: every input that is not one of
 * the convention's reserved names (`BKE_paint_layers_node_group.hh`) is a parameter, typed and
 * ranged from the socket itself. A chain's parameters are the aggregated schemas of the
 * definition's effect rows, keyed so an instance can carry a value per member. The descriptor
 * copies the static entry's flags, roles and name -- everything true of every instance -- and
 * replaces the schema alone.
 *
 * The cache follows the memo pattern of the source graph's hashes (`paint_layers_source_hash.cc`):
 * entries are keyed on the definition's `ID::session_uid` and the row kind, stand while the
 * definition's update signal does, and are forgotten wholesale on the definition edits that slip
 * past the signal. A descriptor is immutable once published, and the publication is append-only:
 * a rebuilt identity retires the previous descriptor under the same key instead of replacing it,
 * and a retired (or forgotten) one stays allocated until
 * #BKE_paint_layers_node_group_forget_all drops the whole cache on the file-load post. A reader
 * that walked out of the lookup with a pointer into a descriptor therefore keeps a live allocation
 * for as long as it holds it: no entry is ever freed under a reader, so the readers (the parameter
 * accessors, the packing, the RNA) walk the descriptor without a lock. Refcounting the descriptors
 * instead was rejected: the lookups are the hot path every parameter read takes, and the retire
 * window -- the handful of descriptors between two file loads -- costs a few kilobytes of memory.
 *
 * NOTE: The append-only guarantee (no-free-under-reader) holds only on the main thread. All
 * callers of descriptor lookups (the composite, the RNA, the packing, shader-bake image_get) run
 * on the main thread exclusively. A descriptor accessed from another thread must not be assumed to
 * stay allocated -- the main thread may retire/rebuild it at any time. If parallel bake or
 * background compositing is added in the future, descriptor access must be guarded by a generation
 * check or a lock.
 */

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8_symbols.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_node_group.hh"

#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/** The range an un-ranged socket reads as: wide enough that a stored value never fights the
 * clamp, and a value outside it is a script's own doing. */
constexpr float node_group_param_unranged_min = -10000.0f;
constexpr float node_group_param_unranged_max = 10000.0f;

static uint64_t node_group_hash_mix(uint64_t hash, const uint64_t value)
{
  hash = (hash ^ value) * 0x9E3779B97F4A7C15ULL;
  hash ^= hash >> 29;
  return hash;
}

/* -------------------------------------------------------------------- */
/** \name The tree update signal
 *
 * A cheap per-tree number that every tree update raises
 * (#bNodeTreeRuntime::content_revision, bumped in `node_tree_update.cc`'s update), folded
 * recursively through the group nodes, so an edit of a nested tree moves the outer one's signal
 * too. An entry keyed on a session uid stands while the signal it was made from stands. The
 * counter is this module's own rather than the preview state's: the preview flag belongs to the
 * preview system, and a refactor there must not silently stop the definition memos from noticing
 * an edit.
 * \{ */

static uint64_t node_group_tree_signal_recursive(const bNodeTree &tree,
                                                 Set<const bNodeTree *> &visited)
{
  if (!visited.add(&tree)) {
    return 0;
  }
  /* A tree read before its runtime is allocated (an imported or embedded definition) has no
   * signal of its own; the interface hash is then recomputed on every ask rather than read from
   * a stale memo. */
  if (tree.runtime == nullptr) {
    return 0;
  }
  uint64_t signal = node_group_hash_mix(uint64_t(tree.runtime->content_revision),
                                        uint64_t(tree.runtime->output_topology_hash));
  for (const bNode *node : tree.all_nodes()) {
    if (!node->is_group() || node->id == nullptr) {
      continue;
    }
    if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
      signal = node_group_hash_mix(signal,
                                   node_group_tree_signal_recursive(*group_tree, visited));
    }
  }
  return signal;
}

static uint64_t node_group_tree_signal(const bNodeTree &tree)
{
  Set<const bNodeTree *> visited;
  return node_group_tree_signal_recursive(tree, visited);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The descriptor cache (declared here, used by the hash forgets below)
 * \{ */

struct DynamicKindDescriptor {
  /** The definition hash the descriptor was built from. */
  uint64_t definition_hash = 0;
  /**
   * The cheap token the cache validates against: the tree's interface hash for a node-group kind,
   * the definition's topology generation plus its members' definition hashes for a chain (see
   * #dynamic_kind_chain_identity). Kept apart from #definition_hash because a chain's hash also
   * folds its members' storage keys and schemas, which the lookup must not walk again.
   */
  uint64_t identity = 0;
  /** The static entry's flags, roles and name, with the schema replaced. */
  PaintLayerKindInfo info;
  /** The schema the info points into; owning. */
  Vector<PaintLayerKindParam> params;
  /**
   * The storage the parameter names and labels of a dynamic schema point into. A deque keeps its
   * elements' addresses stable as it grows, so the `const char *` the schema hands out stay valid
   * for the descriptor's whole life.
   */
  std::deque<std::string> strings;
  /**
   * The choice lists of the schema's menu parameters: each menu socket's runtime items, owned
   * whole (the interface socket shares them with the tree, and the descriptor may outlive the
   * socket data it read them from only in shape -- it revalidates by identity, never by pointer).
   * The `const PaintLayerKindEnumItem *` the schema hands out point into these, whose addresses
   * are stable once a list is appended.
   */
  std::deque<Vector<PaintLayerKindEnumItem>> enum_items;

  /** Keep \a text alive with the descriptor and answer the pointer the schema stores. */
  const char *intern(StringRef text)
  {
    strings.emplace_back(text.data(), size_t(text.size()));
    return strings.back().c_str();
  }
};

static Map<uint64_t, std::unique_ptr<DynamicKindDescriptor>> &dynamic_kind_descriptor_map();
static Vector<std::unique_ptr<DynamicKindDescriptor>> &dynamic_kind_descriptor_retired();
static void dynamic_kind_descriptor_retire(const uint64_t key);
static std::mutex &node_group_memo_mutex();

/** The cache key of one descriptor: the definition's session uid and the row kind, so two kinds
 * that share one tree (a source row and a filter row of it) build separate descriptors. */
static uint64_t dynamic_kind_descriptor_key(const uint32_t session_uid, const int16_t subtype)
{
  return (uint64_t(session_uid) << 32) | uint64_t(uint16_t(subtype));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The interface hash
 *
 * The identity of what the tree exposes: every interface item's position, and every socket's
 * direction, name, type, default, range and structure. Value edits inside the nodes do not move
 * it -- the values live on the rows by then -- and an interface edit moves it as soon as the
 * tree update has run. Memoized against the tree's signal, the way the source graph's hashes are.
 * \{ */

static uint64_t node_group_socket_data_hash(const bNodeTreeInterfaceSocket &socket)
{
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  const eNodeSocketDatatype type = (typeinfo != nullptr) ? typeinfo->type : SOCK_FLOAT;
  uint64_t hash = uint64_t(type);
  switch (type) {
    case SOCK_FLOAT: {
      if (const bNodeSocketValueFloat *value = static_cast<const bNodeSocketValueFloat *>(
              socket.socket_data))
      {
        hash = node_group_hash_mix(
            hash,
            get_default_hash(value->value, value->min, value->max, float(value->subtype)));
      }
      break;
    }
    case SOCK_INT: {
      if (const bNodeSocketValueInt *value = static_cast<const bNodeSocketValueInt *>(
              socket.socket_data))
      {
        hash = node_group_hash_mix(
            hash,
            get_default_hash(float(value->value),
                             float(value->min),
                             float(value->max),
                             float(value->subtype)));
      }
      break;
    }
    case SOCK_VECTOR: {
      if (const bNodeSocketValueVector *value = static_cast<const bNodeSocketValueVector *>(
              socket.socket_data))
      {
        hash = node_group_hash_mix(hash,
                                   get_default_hash(value->value[0],
                                                    value->value[1],
                                                    value->value[2],
                                                    float(value->dimensions)));
        hash = node_group_hash_mix(hash, get_default_hash(value->min, value->max));
      }
      break;
    }
    case SOCK_RGBA: {
      if (const bNodeSocketValueRGBA *value = static_cast<const bNodeSocketValueRGBA *>(
              socket.socket_data))
      {
        hash = node_group_hash_mix(
            hash,
            get_default_hash(value->value[0], value->value[1], value->value[2], value->value[3]));
      }
      break;
    }
    case SOCK_BOOLEAN: {
      if (const bNodeSocketValueBoolean *value = static_cast<const bNodeSocketValueBoolean *>(
              socket.socket_data))
      {
        hash = node_group_hash_mix(hash, uint64_t(value->value));
      }
      break;
    }
    case SOCK_MENU: {
      /* The menu is its item list as much as its default: both are the schema the row's dynamic
       * kind answers with, so a choice added, renamed or made default moves the hash. */
      if (const bNodeSocketValueMenu *value = static_cast<const bNodeSocketValueMenu *>(
              socket.socket_data))
      {
        hash = node_group_hash_mix(hash, uint64_t(value->value));
        if (value->enum_items != nullptr) {
          hash = node_group_hash_mix(hash, uint64_t(value->enum_items->items.size()));
          for (const bke::RuntimeNodeEnumItem &item : value->enum_items->items) {
            hash = node_group_hash_mix(hash,
                                       get_default_hash(item.identifier,
                                                        StringRef(item.name),
                                                        StringRef(item.description)));
          }
        }
      }
      break;
    }
    default:
      break;
  }
  if (socket.properties != nullptr) {
    hash = node_group_hash_mix(hash, uint64_t(socket.properties->type));
  }
  return hash;
}

static uint64_t node_group_interface_hash_recursive(const bNodeTreeInterfacePanel &panel,
                                                    uint64_t hash)
{
  hash = node_group_hash_mix(hash, uint64_t(panel.items_num));
  for (const bNodeTreeInterfaceItem *item : panel.items()) {
    if (item->item_type == NodeTreeInterfaceItemType::Panel) {
      const bNodeTreeInterfacePanel &child = *reinterpret_cast<const bNodeTreeInterfacePanel *>(item);
      hash = node_group_hash_mix(hash, get_default_hash(StringRef(child.name)));
      hash = node_group_interface_hash_recursive(child, hash);
      continue;
    }
    if (item->item_type != NodeTreeInterfaceItemType::Socket) {
      continue;
    }
    const bNodeTreeInterfaceSocket &socket = *reinterpret_cast<const bNodeTreeInterfaceSocket *>(item);
    hash = node_group_hash_mix(hash, get_default_hash(StringRef(socket.name),
                                                        StringRef(socket.socket_type)));
    hash = node_group_hash_mix(hash, get_default_hash(StringRef(socket.identifier)));
    hash = node_group_hash_mix(
        hash,
        uint64_t(socket.flag & (NODE_INTERFACE_SOCKET_INPUT | NODE_INTERFACE_SOCKET_OUTPUT)));
    hash = node_group_hash_mix(hash, node_group_socket_data_hash(socket));
  }
  return hash;
}

struct NodeGroupInterfaceMemo {
  uint64_t signal = 0;
  uint64_t hash = 0;
};

static Map<uint32_t, NodeGroupInterfaceMemo> &node_group_interface_memo()
{
  static Map<uint32_t, NodeGroupInterfaceMemo> memo;
  return memo;
}

static std::mutex &node_group_memo_mutex()
{
  static std::mutex mutex;
  return mutex;
}

uint64_t BKE_paint_layers_node_group_interface_hash(const bNodeTree *tree)
{
  if (tree == nullptr) {
    return 0;
  }
  /* A tree read before its runtime is allocated (an imported or embedded definition) has no
   * signal of its own: the memo cannot tell one state of it from another, so the hash is computed
   * every time instead of being stored under a signal no edit would ever move. */
  if (tree->runtime == nullptr) {
    return node_group_interface_hash_recursive(tree->tree_interface.root_panel, 0);
  }
  const uint64_t signal = node_group_tree_signal(*tree);
  {
    std::lock_guard lock(node_group_memo_mutex());
    if (const NodeGroupInterfaceMemo *memo = node_group_interface_memo().lookup_ptr(
            tree->id.session_uid))
    {
      if (memo->signal == signal) {
        return memo->hash;
      }
    }
  }
  const uint64_t hash = node_group_interface_hash_recursive(tree->tree_interface.root_panel, 0);
  std::lock_guard lock(node_group_memo_mutex());
  node_group_interface_memo().add_overwrite(tree->id.session_uid,
                                            NodeGroupInterfaceMemo{signal, hash});
  return hash;
}

void BKE_paint_layers_node_group_forget(const bNodeTree *tree)
{
  if (tree == nullptr) {
    return;
  }
  std::lock_guard lock(node_group_memo_mutex());
  node_group_interface_memo().remove(tree->id.session_uid);
  /* The interface may have moved without the tree update having run yet (an RNA edit before the
   * update), so the descriptors built from the old interface go with the memo -- retired rather
   * than freed, because a reader may still be walking one (the file header's append-only rule). */
  for (const uint64_t subtype : {uint64_t(0),
                                 uint64_t(uint16_t(MA_PAINT_LAYER_FILTER_NODE_GROUP)),
                                 uint64_t(uint16_t(MA_PAINT_LAYER_FILTER_CHAIN)),
                                 uint64_t(uint16_t(MA_PAINT_LAYER_GENERATOR_NODE_GROUP))})
  {
    dynamic_kind_descriptor_retire(dynamic_kind_descriptor_key(tree->id.session_uid,
                                                               int16_t(subtype)));
  }
}

void BKE_paint_layers_node_group_forget_all()
{
  /* The one point a descriptor dies: the file-load post, where the previous file's Main is gone
   * and no reader can hold a pointer into a descriptor any more. */
  std::lock_guard lock(node_group_memo_mutex());
  node_group_interface_memo().clear();
  dynamic_kind_descriptor_map().clear();
  dynamic_kind_descriptor_retired().clear();
}

void BKE_paint_layers_node_group_forget_chain(const uint32_t material_session_uid)
{
  /* A chain's descriptor is keyed on the definition material's session uid, not on a tree's, so
   * the tree's forget does not reach it: the material's own free does. Retired rather than freed
   * for the same reason the tree forget retires -- a reader may still be walking the aggregate
   * schema. */
  std::lock_guard lock(node_group_memo_mutex());
  dynamic_kind_descriptor_retire(
      dynamic_kind_descriptor_key(material_session_uid, int16_t(MA_PAINT_LAYER_FILTER_CHAIN)));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Validation
 * \{ */

/** The sockets the top-level definition exposes. A nested group's interface is internal to the
 * graph and says nothing about what the row's tree answers, so only the root tree is read here. */
static void node_group_shape_scan_interface(const bNodeTree &tree,
                                            PaintLayerNodeGroupShape &shape)
{
  tree.tree_interface.root_panel.foreach_item(
      [&](const bNodeTreeInterfaceItem &item) {
        if (item.item_type != NodeTreeInterfaceItemType::Socket) {
          return true;
        }
        const bNodeTreeInterfaceSocket &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
        const bool is_output = (socket.flag & NODE_INTERFACE_SOCKET_OUTPUT) != 0;
        const bool is_input = (socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0;
        if (is_output) {
          if (StringRef(socket.name) == PAINT_NG_SOCKET_RESULT) {
            shape.has_result = true;
          }
          else if (StringRef(socket.name) == PAINT_NG_SOCKET_FAC) {
            shape.has_fac = true;
          }
          else if (StringRef(socket.name) == PAINT_NG_SOCKET_COLOR) {
            shape.has_color = true;
          }
          else if (StringRef(socket.name) == PAINT_NG_SOCKET_ALPHA) {
            shape.has_alpha = true;
          }
          else if (StringRef(socket.name) != PAINT_NG_SOCKET_COORDINATE) {
            for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
              if (StringRef(socket.name) == info.ui_name) {
                shape.has_channel_output = true;
                break;
              }
            }
          }
        }
        if (is_input) {
          if (StringRef(socket.name) == PAINT_NG_SOCKET_INPUT) {
            shape.has_input = true;
          }
          else if (StringRef(socket.name) == PAINT_NG_SOCKET_UV) {
            shape.has_uv = true;
          }
          else if (StringRef(socket.name) == PAINT_NG_SOCKET_MASK) {
            shape.has_mask = true;
          }
          else if (StringRef(socket.name) == PAINT_NG_SOCKET_COORDINATE) {
            shape.has_coordinate = true;
          }
        }
        return true;
      },
      /*include_self=*/false);
}

/** The graph's nodes, nested groups included: a neighborhood or object-dependent node anywhere in
 * the graph prices the whole answer, because the bake evaluates the graph as one material. */
static void node_group_shape_scan_nodes(const bNodeTree &tree,
                                        Set<const bNodeTree *> &visited,
                                        PaintLayerNodeGroupShape &shape)
{
  if (!visited.add(&tree)) {
    return;
  }
  for (const bNode *node : tree.all_nodes()) {
    switch (node->type_legacy) {
      case SH_NODE_BEVEL:
      case SH_NODE_AMBIENT_OCCLUSION:
        shape.neighborhood_nodes = true;
        break;
      case SH_NODE_OBJECT_INFO:
      case SH_NODE_CAMERA:
      case SH_NODE_LIGHT_PATH:
        shape.object_dependent = true;
        break;
      default:
        break;
    }
    if (node->is_group() && node->id != nullptr) {
      if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
        node_group_shape_scan_nodes(*group_tree, visited, shape);
      }
    }
  }
}

PaintLayerNodeGroupShape BKE_paint_layers_node_group_shape(const bNodeTree *tree)
{
  PaintLayerNodeGroupShape shape;
  if (tree == nullptr) {
    return shape;
  }
  node_group_shape_scan_interface(*tree, shape);
  Set<const bNodeTree *> visited;
  node_group_shape_scan_nodes(*tree, visited, shape);
  return shape;
}

static bool node_group_tree_is_shader(const bNodeTree *tree)
{
  return tree != nullptr && tree->typeinfo != nullptr && tree->typeinfo->type == NTREE_SHADER;
}

/** Whether the interface names \a name as a Color socket in the asked direction. */
static bool node_group_interface_socket_is_color(const bNodeTree &tree,
                                                 const StringRef name,
                                                 const bool want_output)
{
  bool is_color = false;
  tree.tree_interface.root_panel.foreach_item(
      [&](const bNodeTreeInterfaceItem &item) {
        if (item.item_type != NodeTreeInterfaceItemType::Socket) {
          return true;
        }
        const bNodeTreeInterfaceSocket &socket =
            reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
        const bool is_output = (socket.flag & NODE_INTERFACE_SOCKET_OUTPUT) != 0;
        if (is_output != want_output || StringRef(socket.name) != name) {
          return true;
        }
        is_color = StringRef(socket.socket_type) == "NodeSocketColor";
        return false;
      },
      /*include_self=*/false);
  return is_color;
}

bool BKE_paint_layers_node_group_supports_filter(const bNodeTree *tree)
{
  if (!node_group_tree_is_shader(tree)) {
    return false;
  }
  const PaintLayerNodeGroupShape shape = BKE_paint_layers_node_group_shape(tree);
  return shape.has_input && shape.has_result &&
         node_group_interface_socket_is_color(*tree, PAINT_NG_SOCKET_INPUT, false) &&
         node_group_interface_socket_is_color(*tree, PAINT_NG_SOCKET_RESULT, true);
}

bool BKE_paint_layers_node_group_supports_source(const bNodeTree *tree)
{
  if (!node_group_tree_is_shader(tree)) {
    return false;
  }
  const PaintLayerNodeGroupShape shape = BKE_paint_layers_node_group_shape(tree);
  /* The one-answer contract is `Result`; a tree that names its paint channels instead is the
   * per-channel form, and both are source trees (#BKE_paint_layers_node_group_source_channels
   * reads the named outputs). */
  return shape.has_result || shape.has_channel_output;
}

bool BKE_paint_layers_node_group_supports_generator(const bNodeTree *tree)
{
  if (!node_group_tree_is_shader(tree)) {
    return false;
  }
  const PaintLayerNodeGroupShape shape = BKE_paint_layers_node_group_shape(tree);
  return shape.has_coordinate && shape.has_fac;
}

bool BKE_paint_layers_node_group_reaches_whole_map(const MaterialPaintLayer &row)
{
  /* Only the tree-reading kinds ask: a source row, or a filter of the node-group subtype in the
   * filter source. The subtype alone is ambiguous (a Threshold filter is 10). */
  if (row.source != MA_PAINT_LAYER_SOURCE_NODE_GROUP &&
      !(row.source == MA_PAINT_LAYER_SOURCE_FILTER &&
        row.kind_subtype == MA_PAINT_LAYER_FILTER_NODE_GROUP))
  {
    return false;
  }
  const PaintLayerNodeGroupShape shape = BKE_paint_layers_node_group_shape(row.kind_tree);
  return shape.neighborhood_nodes;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parameters from the interface
 *
 * Every input that is not one of the reserved names is a parameter. The storage key is the
 * socket's identifier -- stable across a rename -- and the label is its name, so a parameter
 * renamed in the tree keeps the instance's value. A socket this build cannot answer (a menu, an
 * image, a geometry) is counted, not added: the descriptor stays a schema the readers can walk,
 * and the socket's own default rides with the group node.
 * \{ */

/** Whether \a name is one of the reserved inputs the kinds read themselves. */
static bool node_group_input_is_reserved(const StringRef name, const bool is_generator)
{
  if (is_generator) {
    return name == PAINT_NG_SOCKET_COORDINATE;
  }
  return name == PAINT_NG_SOCKET_INPUT || name == PAINT_NG_SOCKET_UV ||
         name == PAINT_NG_SOCKET_MASK;
}

static bool node_group_params_from_interface(const bNodeTree &tree,
                                             const bool is_generator,
                                             DynamicKindDescriptor &r_descriptor,
                                             int &r_unsupported)
{
  r_unsupported = 0;
  tree.tree_interface.root_panel.foreach_item(
      [&](const bNodeTreeInterfaceItem &item) {
        if (item.item_type != NodeTreeInterfaceItemType::Socket) {
          return true;
        }
        const bNodeTreeInterfaceSocket &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
        if ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) == 0) {
          return true;
        }
        if (node_group_input_is_reserved(socket.name, is_generator)) {
          return true;
        }
        const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
        const eNodeSocketDatatype type = (typeinfo != nullptr) ? typeinfo->type : SOCK_FLOAT;
        PaintLayerKindParam param = {};
        bool keep = false;
        switch (type) {
          case SOCK_FLOAT: {
            if (const bNodeSocketValueFloat *value =
                    static_cast<const bNodeSocketValueFloat *>(socket.socket_data))
            {
              param.type = PaintLayerKindParamType::Float;
              param.default_value = value->value;
              /* An un-ranged socket (min == max) reads as an unclamped one. */
              param.min = (value->min < value->max) ? value->min : node_group_param_unranged_min;
              param.max = (value->min < value->max) ? value->max : node_group_param_unranged_max;
              keep = true;
            }
            break;
          }
          case SOCK_INT: {
            if (const bNodeSocketValueInt *value = static_cast<const bNodeSocketValueInt *>(
                    socket.socket_data))
            {
              param.type = PaintLayerKindParamType::Int;
              param.default_value = float(value->value);
              param.min = float(value->min);
              param.max = float(value->max);
              keep = true;
            }
            break;
          }
          case SOCK_BOOLEAN: {
            if (const bNodeSocketValueBoolean *value =
                    static_cast<const bNodeSocketValueBoolean *>(socket.socket_data))
            {
              param.type = PaintLayerKindParamType::Bool;
              param.default_value = value->value != 0 ? 1.0f : 0.0f;
              param.min = 0.0f;
              param.max = 1.0f;
              keep = true;
            }
            break;
          }
          case SOCK_RGBA: {
            if (const bNodeSocketValueRGBA *value = static_cast<const bNodeSocketValueRGBA *>(
                    socket.socket_data))
            {
              param.type = PaintLayerKindParamType::Color;
              copy_v4_v4(param.default_color, value->value);
              param.min = 0.0f;
              param.max = 1.0f;
              keep = true;
            }
            break;
          }
          case SOCK_VECTOR: {
            if (const bNodeSocketValueVector *value =
                    static_cast<const bNodeSocketValueVector *>(socket.socket_data))
            {
              /* The default rides the #PaintLayerKindParam::default_color slots -- the ones the
               * vector accessors read (#kind_param_ensure_vector and its get) -- the way a
               * Color's four ride there whole; without it every vector parameter read as
               * (0, 0, 0) whatever the socket said. A socket of fewer than three dimensions
               * stays a parameter: its components keep their values and the missing ones read
               * zero, the way a stored short array would read back. */
              param.type = PaintLayerKindParamType::Vector;
              zero_v3(param.default_color);
              for (const int i : IndexRange(std::min(int(value->dimensions), 3))) {
                param.default_color[i] = value->value[i];
              }
              param.min = (value->min < value->max) ? value->min : node_group_param_unranged_min;
              param.max = (value->min < value->max) ? value->max : node_group_param_unranged_max;
              keep = true;
            }
            break;
          }
          case SOCK_MENU: {
            /* A menu switch is a parameter the panel answers from the socket's own item list: the
             * runtime items the interface socket shares, read by their persistent int identifiers
             * (what the socket stores and what the instance binds). A menu without items -- one
             * the tree has not resolved -- stays unsupported, its default riding the group node
             * as before. */
            if (const bNodeSocketValueMenu *value = static_cast<const bNodeSocketValueMenu *>(
                    socket.socket_data))
            {
              if (value->enum_items != nullptr && !value->enum_items->items.is_empty()) {
                /* A deque keeps the list's address stable as more menus are added: the schema's
                 * `enum_items` pointer answers into it for the descriptor's whole life. */
                Vector<PaintLayerKindEnumItem> &items = r_descriptor.enum_items.emplace_back();
                int min_identifier = value->enum_items->items.first().identifier;
                int max_identifier = min_identifier;
                for (const bke::RuntimeNodeEnumItem &item : value->enum_items->items) {
                  /* The RNA enum identifier has to be unique and stable: the menu's persistent
                   * value is the one, the display name is the label. Two items may share a name. */
                  const std::string id_text = std::to_string(item.identifier);
                  items.append({item.identifier,
                                r_descriptor.intern(id_text),
                                r_descriptor.intern(item.name)});
                  min_identifier = std::min(min_identifier, item.identifier);
                  max_identifier = std::max(max_identifier, item.identifier);
                }
                param.type = PaintLayerKindParamType::Enum;
                param.default_value = float(value->value);
                /* The clamps the stored value rides read the identifier range, so a stored choice
                 * survives the accessor round-trip whatever identifiers the tree picked. */
                param.min = float(min_identifier);
                param.max = float(max_identifier);
                param.enum_items = items.data();
                param.enum_items_num = int(items.size());
                keep = true;
              }
            }
            break;
          }
          default:
            break;
        }
        if (keep) {
          param.name = r_descriptor.intern(socket.identifier);
          param.label = r_descriptor.intern(socket.name);
          r_descriptor.params.append(param);
        }
        else {
          r_unsupported += 1;
        }
        return true;
      },
      /*include_self=*/false);
  return r_unsupported == 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The descriptor cache
 *
 * One descriptor per (definition session uid, row kind): the static entry copied, the schema
 * replaced. A definition edit changes the definition hash; a rebuild publishes a new descriptor
 * and retires the previous one rather than replacing it, and a reader holding the old descriptor
 * keeps reading a live allocation until it drops the pointer, so no lock follows the reader out
 * of the lookup. The retired ones stay allocated until the wholesale clear on the file-load post
 * (see the file header for why append-only rather than refcounting).
 * \{ */

static Map<uint64_t, std::unique_ptr<DynamicKindDescriptor>> &dynamic_kind_descriptor_map()
{
  static Map<uint64_t, std::unique_ptr<DynamicKindDescriptor>> map;
  return map;
}

/**
 * The descriptors a rebuild or a forget has taken out of the lookup. They stay allocated here --
 * never freed at the retire point -- because a reader may still be walking one; the whole set
 * dies with the map on #BKE_paint_layers_node_group_forget_all, which runs on the file-load post
 * where no reader can hold a descriptor. No eviction besides that (no LRU): the set is bounded
 * by the session's definition edits, each a few kilobytes, and an eviction that could free under
 * a reader is exactly the hazard the append-only model exists to rule out.
 */
static Vector<std::unique_ptr<DynamicKindDescriptor>> &dynamic_kind_descriptor_retired()
{
  static Vector<std::unique_ptr<DynamicKindDescriptor>> retired;
  return retired;
}

/** Publish \a descriptor under \a key, retiring whatever stood there. The caller holds the memo
 * mutex. */
static void dynamic_kind_descriptor_publish(const uint64_t key,
                                            std::unique_ptr<DynamicKindDescriptor> descriptor)
{
  Map<uint64_t, std::unique_ptr<DynamicKindDescriptor>> &map = dynamic_kind_descriptor_map();
  /* The append-only publication: the old entry is moved out, not freed, so a reader that walked
   * out of an earlier lookup with its `info` pointer keeps a live allocation behind it. */
  if (std::unique_ptr<DynamicKindDescriptor> *entry = map.lookup_ptr(key)) {
    dynamic_kind_descriptor_retired().append(std::move(*entry));
    map.remove(key);
  }
  map.add_new(key, std::move(descriptor));
}

/** Retire the entry under \a key without freeing it (see #dynamic_kind_descriptor_retired). The
 * caller holds the memo mutex; a missing key is a no-op. */
static void dynamic_kind_descriptor_retire(const uint64_t key)
{
  Map<uint64_t, std::unique_ptr<DynamicKindDescriptor>> &map = dynamic_kind_descriptor_map();
  if (std::unique_ptr<DynamicKindDescriptor> *entry = map.lookup_ptr(key)) {
    dynamic_kind_descriptor_retired().append(std::move(*entry));
    map.remove(key);
  }
}

/** The number of descriptors the cache holds keyed and live, the retired ones aside: a test's
 * window into a cache that is otherwise this translation unit's own static. */
int paint_layer_kind_dynamic_descriptor_count()
{
  std::lock_guard lock(node_group_memo_mutex());
  return int(dynamic_kind_descriptor_map().size());
}

static const PaintLayerKindInfo *dynamic_kind_static_info(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_find(row.source, row.kind_subtype);
  return (info != nullptr && (info->flags & PAINT_LAYER_KIND_DYNAMIC_SCHEMA) != 0) ? info : nullptr;
}

/**
 * The definition a dynamic kind reads: the tree of a node-group kind, the material of a chain.
 * Null when the row carries none (the definition is gone), which the lookup answers with the
 * static entry.
 */
static const bNodeTree *dynamic_kind_tree_of(const MaterialPaintLayer &row)
{
  /* The subtype only names a kind beside its source: 46 is the node-group filter and 10 the
   * node-group generator inside their own sources, while a filter of subtype 10 is the
   * Threshold. */
  if (row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP ||
      (row.source == MA_PAINT_LAYER_SOURCE_FILTER &&
       row.kind_subtype == MA_PAINT_LAYER_FILTER_NODE_GROUP) ||
      (row.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
       row.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP))
  {
    return row.kind_tree;
  }
  return nullptr;
}

static const Material *dynamic_kind_chain_of(const MaterialPaintLayer &row)
{
  if (row.source == MA_PAINT_LAYER_SOURCE_FILTER &&
      row.kind_subtype == MA_PAINT_LAYER_FILTER_CHAIN)
  {
    return row.material;
  }
  return nullptr;
}

/**
 * Build the schema of a node-group kind: the interface's parameters, in interface order. A
 * generator kind opens its schema with the shared placement prefix and closes it with the ramp,
 * the way every static generator's schema does, so the placement readers read the same slots the
 * DNA codes name. The static entry answers for everything else.
 */
static void dynamic_kind_descriptor_build_node_group(const PaintLayerKindInfo &static_info,
                                                     const bNodeTree &tree,
                                                     const bool is_generator,
                                                     DynamicKindDescriptor &r_descriptor)
{
  r_descriptor.info = static_info;
  if (is_generator) {
    /* The placement schema is a const span owned by the kind table, copied into the descriptor's
     * own storage (the `Span<const T>` overload of `extend` does not exist). */
    const Span<const PaintLayerKindParam> placement = paint_layer_generator_placement_params();
    r_descriptor.params.extend(placement.data(), placement.size());
  }
  /* The `Alpha` output is the filter's answer to what its result covers by: a tree that exposes
   * one lays a layer with an alpha of its own (the Drop Shadow's contract), so the descriptor
   * carries #PAINT_LAYER_KIND_RESULT_HAS_ALPHA and every reader of the flag -- the effect
   * compute, the correction description, the graph's alpha raise, the bake maps' alpha keep --
   * answers for the kind without a branch of its own. Only the filter takes it. A source tree's
   * alpha is its coverage already: the map the source lays covers by its own alpha channel, so a
   * separate answer would only double what the stack reads. A generator has no alpha to lay -- the
   * ramp tail's coverage is the whole answer. */
  if (!is_generator && static_info.source == MA_PAINT_LAYER_SOURCE_FILTER &&
      static_info.subtype == MA_PAINT_LAYER_FILTER_NODE_GROUP)
  {
    const PaintLayerNodeGroupShape shape = BKE_paint_layers_node_group_shape(&tree);
    if (shape.has_alpha) {
      r_descriptor.info.flags |= PAINT_LAYER_KIND_RESULT_HAS_ALPHA;
    }
  }
  int unsupported = 0;
  node_group_params_from_interface(tree, is_generator, r_descriptor, unsupported);
  if (is_generator) {
    r_descriptor.params.append(*paint_layer_generator_ramp_param());
  }
  r_descriptor.definition_hash = BKE_paint_layers_node_group_interface_hash(&tree);
  r_descriptor.identity = r_descriptor.definition_hash;
  r_descriptor.info.params = r_descriptor.params.data();
  r_descriptor.info.params_num = int(r_descriptor.params.size());
}

/**
 * The cheap identity token of a chain definition: the definition's topology generation, every
 * member's marker (the member set and its order) and the member's own definition hash. The
 * generation alone misses a member that is itself a dynamic kind (a node-group filter inside the
 * chain): editing that member's tree moves the member's schema -- which is part of the aggregate
 * schema the descriptor answers -- without touching the chain definition's rows, so the fold is
 * what makes the cache notice. A static member answers 0 and mixes nothing in for it. The markers
 * are folded beside it because a definition material usually has no generated stack tree: its
 * topology generation is 0, and without the member set a script that added or removed a member in
 * place would keep the old aggregate schema.
 *
 * The walk is the one #dynamic_kind_descriptor_build_chain takes -- the definition's top level --
 * so a hand-made nested row cannot move the identity without moving the schema.
 *
 * The lookup recomputes this before consulting the cache, so it stays a walk over the definition's
 * rows and their memoized tree hashes -- microseconds, not the schema rebuild the identity guards.
 */
static uint64_t dynamic_kind_chain_identity(const Material &definition)
{
  const uint64_t generation =
      uint64_t(BKE_material_paint_layers_topology_generation(definition));
  uint64_t identity = node_group_hash_mix(0, generation);
  for (const MaterialPaintLayer &member : definition.paint_layers) {
    if (member.role != MA_PAINT_LAYER_ROLE_EFFECT) {
      continue;
    }
    char member_key[UUID_STRING_SIZE];
    BLI_uuid_format(member_key, member.marker);
    identity = node_group_hash_mix(identity, get_default_hash(StringRef(member_key)));
    identity = node_group_hash_mix(identity, BKE_paint_layers_node_group_definition_hash(member));
  }
  return identity;
}

std::string paint_layer_chain_member_param_key(const bUUID &member_marker,
                                               const StringRefNull param_name)
{
  char member_key[UUID_STRING_SIZE];
  BLI_uuid_format(member_key, member_marker);
  return std::string(member_key) + ":" + param_name.c_str();
}

/**
 * Build the schema of a chain: every kind parameter of every effect row of the definition, in
 * stack order, keyed by member so an instance carries a value per member. The definition's
 * topology generation answers for the shape; a member's kind may itself be a dynamic one.
 */
static void dynamic_kind_descriptor_build_chain(const PaintLayerKindInfo &static_info,
                                                const Material &definition,
                                                DynamicKindDescriptor &r_descriptor)
{
  r_descriptor.info = static_info;
  const uint64_t generation =
      uint64_t(BKE_material_paint_layers_topology_generation(definition));
  uint64_t hash = node_group_hash_mix(0, generation);
  /* The same walk the composite's unfolding reads -- the definition's top level alone. The
   * folder walk a #BKE_paint_layers_foreach would take disagrees with it for a definition with
   * nested rows: the schema would offer parameters the unfolding never lays corrections for.
   * The Smart Filter save blocks folders, so the two agree on conforming definitions; this walk
   * keeps them agreeing on hand-made ones too. */
  for (const MaterialPaintLayer &member : definition.paint_layers) {
    if (member.role != MA_PAINT_LAYER_ROLE_EFFECT) {
      continue;
    }
    char member_key[UUID_STRING_SIZE];
    BLI_uuid_format(member_key, member.marker);
    hash = node_group_hash_mix(hash, get_default_hash(StringRef(member_key)));
    /* A member that is itself a dynamic kind (a node-group filter inside the chain) reads a
     * definition of its own, whose edits move the member's schema without moving the chain
     * definition's topology generation. Its definition hash folds in here, so editing that
     * nested tree rebuilds the chain descriptor too -- the aggregate schema is the member's
     * schema, whatever the member reads it from. A static member answers 0 and moves nothing. */
    hash = node_group_hash_mix(hash, BKE_paint_layers_node_group_definition_hash(member));
    const PaintLayerKindInfo *member_info = paint_layer_kind_of(member);
    if (member_info == nullptr) {
      continue;
    }
    for (const int i : IndexRange(member_info->params_num)) {
      const PaintLayerKindParam &param = member_info->params[i];
      /* Payload parameters stay the member's whole (v1): the instance carries no override for a
       * ramp, the curves, a color balance or a channel mixer, so the aggregate schema does not
       * offer a slot without a reader. The unfolding passes the member's payload through. */
      if (ELEM(param.type,
               PaintLayerKindParamType::Ramp,
               PaintLayerKindParamType::Curves,
               PaintLayerKindParamType::ColorBalance,
               PaintLayerKindParamType::ChannelMixer))
      {
        continue;
      }
      PaintLayerKindParam entry = {};
      const std::string storage_key = paint_layer_chain_member_param_key(member.marker,
                                                                         param.name);
      const std::string label = std::string(member.name) + " " + BLI_STR_UTF8_MIDDLE_DOT + " " +
                                param.label;
      entry.name = r_descriptor.intern(storage_key);
      entry.label = r_descriptor.intern(label);
      entry.default_value = param.default_value;
      entry.min = param.min;
      entry.max = param.max;
      entry.type = param.type;
      /* A member that is itself a dynamic kind carries its menu items in its own descriptor:
       * borrowing the pointer would dangle the first time the member's tree was forgotten while
       * the chain's identity stood (a member's identity moves with the chain's, but the copy
       * makes the chain descriptor's answer independent of the member's lifetime, which is what
       * an immutable-published cache promises its readers). */
      if (param.enum_items != nullptr && param.enum_items_num > 0) {
        Vector<PaintLayerKindEnumItem> &items = r_descriptor.enum_items.emplace_back();
        for (const int item_i : IndexRange(param.enum_items_num)) {
          const PaintLayerKindEnumItem &src = param.enum_items[item_i];
          items.append({src.value, r_descriptor.intern(src.identifier),
                        r_descriptor.intern(src.label)});
        }
        entry.enum_items = items.data();
        entry.enum_items_num = int(items.size());
      }
      copy_v4_v4(entry.default_color, param.default_color);
      r_descriptor.params.append(entry);
    }
  }
  r_descriptor.definition_hash = hash;
  r_descriptor.identity = dynamic_kind_chain_identity(definition);
  r_descriptor.info.params = r_descriptor.params.data();
  r_descriptor.info.params_num = int(r_descriptor.params.size());
}

static const PaintLayerKindInfo *dynamic_kind_descriptor_lookup(
    const MaterialPaintLayer &row, const PaintLayerKindInfo &static_info)
{
  Map<uint64_t, std::unique_ptr<DynamicKindDescriptor>> &map = dynamic_kind_descriptor_map();

  if (const bNodeTree *tree = dynamic_kind_tree_of(row)) {
    const bool is_generator = row.source == MA_PAINT_LAYER_SOURCE_GENERATOR;
    const uint64_t key = dynamic_kind_descriptor_key(tree->id.session_uid, row.kind_subtype);
    const uint64_t definition_hash = BKE_paint_layers_node_group_interface_hash(tree);
    {
      std::lock_guard lock(node_group_memo_mutex());
      if (const std::unique_ptr<DynamicKindDescriptor> *entry = map.lookup_ptr(key)) {
        if ((*entry)->identity == definition_hash) {
          return &(*entry)->info;
        }
      }
    }
    auto descriptor = std::make_unique<DynamicKindDescriptor>();
    dynamic_kind_descriptor_build_node_group(static_info, *tree, is_generator, *descriptor);
    const PaintLayerKindInfo *info = &descriptor->info;
    std::lock_guard lock(node_group_memo_mutex());
    /* The identity mismatch retires the standing descriptor instead of overwriting it: the reader
     * of the old one keeps its live allocation (the file header's append-only rule). */
    dynamic_kind_descriptor_publish(key, std::move(descriptor));
    return info;
  }
  if (const Material *definition = dynamic_kind_chain_of(row)) {
    const uint64_t key = dynamic_kind_descriptor_key(definition->id.session_uid,
                                                     row.kind_subtype);
    /* The identity is the generation plus every dynamic member's own definition hash, so an edit
     * of a member's tree retires the cached aggregate with it. Computed before the lock: the
     * member hashes take the memo mutex below, which this lock is not re-entrant against. */
    const uint64_t identity = dynamic_kind_chain_identity(*definition);
    {
      std::lock_guard lock(node_group_memo_mutex());
      if (const std::unique_ptr<DynamicKindDescriptor> *entry = map.lookup_ptr(key)) {
        if ((*entry)->identity == identity) {
          return &(*entry)->info;
        }
      }
    }
    auto descriptor = std::make_unique<DynamicKindDescriptor>();
    dynamic_kind_descriptor_build_chain(static_info, *definition, *descriptor);
    const PaintLayerKindInfo *info = &descriptor->info;
    std::lock_guard lock(node_group_memo_mutex());
    dynamic_kind_descriptor_publish(key, std::move(descriptor));
    return info;
  }
  /* The definition is gone: the row is a valid kind with an empty schema, the way a file from a
   * newer build reads. The consumers skip it; the UI reports the missing definition. */
  return &static_info;
}

const PaintLayerKindInfo *paint_layer_kind_dynamic_of(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *static_info = dynamic_kind_static_info(row);
  if (static_info == nullptr) {
    return nullptr;
  }
  return dynamic_kind_descriptor_lookup(row, *static_info);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Definition hash
 * \{ */

uint64_t BKE_paint_layers_node_group_definition_hash(const MaterialPaintLayer &row)
{
  if (const bNodeTree *tree = dynamic_kind_tree_of(row)) {
    /* The schema is the interface, but the answer is the graph: a node value, a link or a node
     * added inside the tree moves the tree's content revision without moving the interface, and
     * the bake and the effect cache must retire with it. The signal is the revision the tree
     * update raises, folded with the topology hash and with every nested group tree's own signal
     * (#node_group_tree_signal), so one mix answers the interface, the graph and the values -- an
     * interface edit also moves the revision, which only costs the memo a recompute it already
     * pays.
     *
     * The tree's identity joins the fold: two different trees can share an interface and a signal
     * (a duplicate edited the same number of times), and a row re-pointed at the other one must
     * make the caches keyed on this hash miss instead of serving the previous tree's pixels. */
    return node_group_hash_mix(
        node_group_hash_mix(
            node_group_hash_mix(uint64_t(row.kind_subtype), uint64_t(tree->id.session_uid)),
            BKE_paint_layers_node_group_interface_hash(tree)),
        node_group_tree_signal(*tree));
  }
  if (const Material *definition = dynamic_kind_chain_of(row)) {
    /* The definition's identity joins for the same reason: two chain materials with the same
     * member shape answer different pixels, and a row re-pointed at one of them must miss. */
    return node_group_hash_mix(
        node_group_hash_mix(uint64_t(row.kind_subtype), uint64_t(definition->id.session_uid)),
        uint64_t(BKE_material_paint_layers_topology_generation(*definition)));
  }
  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resync
 * \{ */

void BKE_paint_layers_node_group_rows_resync(Main &bmain, const Span<bNodeTree *> trees)
{
  if (trees.is_empty()) {
    return;
  }
  Set<uint32_t> tree_uids;
  for (const bNodeTree *tree : trees) {
    if (tree != nullptr) {
      tree_uids.add(tree->id.session_uid);
    }
  }
  if (tree_uids.is_empty()) {
    return;
  }
  const auto row_reads_a_tree = [&](const MaterialPaintLayer &row) {
    if (row.kind_tree == nullptr || !tree_uids.contains(row.kind_tree->id.session_uid)) {
      return false;
    }
    /* The chain reads its definition material; a tree on a chain row is one nothing reads. */
    return row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP ||
           (row.source == MA_PAINT_LAYER_SOURCE_FILTER &&
            row.kind_subtype == MA_PAINT_LAYER_FILTER_NODE_GROUP) ||
           (row.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
            row.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP);
  };
  for (Material &ma : bmain.materials) {
    if (!BKE_paint_layers_is_layered(ma)) {
      continue;
    }
    bool touched = false;
    const auto resync_row = [&](MaterialPaintLayer &row) {
      if (!row_reads_a_tree(row)) {
        return;
      }
      /* A socket the tree gained gets its default stored; one it lost keeps its stored value,
       * which a reader treats as the default while no schema entry names it. */
      paint_layer_kind_params_init(row);
      touched = true;
    };
    BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) {
      resync_row(layer);
      for (MaterialPaintLayer &effect : layer.effects) {
        resync_row(effect);
      }
      for (MaterialPaintLayer &item : layer.mask_stack) {
        resync_row(item);
      }
      return true;
    });
    if (touched) {
      /* The interface the schema came from moved, which is a shape edit: the graphs the rows are
       * built into rebuild, and the bakes go stale by their own hashes. */
      BKE_paint_layers_tag_edited(ma);
    }
  }
}

/** \} */

}  // namespace blender
