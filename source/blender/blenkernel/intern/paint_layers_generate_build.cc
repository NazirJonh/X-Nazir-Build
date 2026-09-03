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

#include "paint_layers_generate_build_intern.hh"

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
using namespace bke::paint_layers;

/** Whether \a node belongs to \a tree. Used to hold the "wrapper instance lives in the tree it is
 * linked into" invariant; a node only ever belongs to one tree. */
static bool node_belongs_to_tree(const bNodeTree &tree, const bNode &node)
{
  for (const bNode &candidate : tree.nodes) {
    if (&candidate == &node) {
      return true;
    }
  }
  return false;
}

/**
 * Wire every Image Texture the generator created in \a tree to one UV Map node, so the whole stack
 * samples the UV layer \a uv_name names. One shared node per tree is enough: every map in that tree
 * reads the same layer. With no name set nothing is added and the maps keep reading the render UV,
 * the behavior before names existed.
 *
 * The call is idempotent: a preserved group already carries its UV node and linked maps, so a
 * value-only edit that rebuilds the root leaves them alone.
 */
namespace bke::paint_layers {

void generated_uv_maps_wire(bNodeTree &tree, const char *uv_name)
{
  if (uv_name == nullptr || uv_name[0] == '\0') {
    return;
  }
  bNode *uv_node = nullptr;
  bool needs_link = false;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_UVMAP) {
      const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(node.storage);
      if (storage != nullptr && STREQ(storage->uv_map, uv_name)) {
        uv_node = &node;
      }
    }
    else if (node.type_legacy == SH_NODE_TEX_IMAGE) {
      bNodeSocket *vector = socket_in(node, "Vector");
      if (vector != nullptr && vector->link == nullptr) {
        needs_link = true;
      }
    }
  }
  if (!needs_link) {
    return;
  }
  if (uv_node == nullptr) {
    uv_node = bke::node_add_static_node(nullptr, tree, SH_NODE_UVMAP);
    if (uv_node == nullptr) {
      return;
    }
    uv_node->location[0] = -600.0f;
    uv_node->location[1] = -320.0f;
    if (NodeShaderUVMap *storage = static_cast<NodeShaderUVMap *>(uv_node->storage)) {
      BLI_strncpy(storage->uv_map, uv_name, sizeof(storage->uv_map));
    }
  }
  bNodeSocket *uv_out = socket_out(*uv_node, "UV");
  if (uv_out == nullptr) {
    return;
  }
  for (bNode &node : tree.nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE) {
      continue;
    }
    bNodeSocket *vector = socket_in(node, "Vector");
    if (vector != nullptr && vector->link == nullptr) {
      bke::node_add_link(tree, *uv_node, *uv_out, node, *vector);
    }
  }
}

void refresh_layer_group(LayerGroup &group)
{
  if (group.tree == nullptr) {
    return;
  }
  if (group.group_input != nullptr) {
    nodes::update_node_declaration_and_sockets(*group.tree, *group.group_input);
  }
  if (group.group_output != nullptr) {
    nodes::update_node_declaration_and_sockets(*group.tree, *group.group_output);
  }
  if (group.instance != nullptr && group.parent_tree != nullptr) {
    nodes::update_node_declaration_and_sockets(*group.parent_tree, *group.instance);
  }
}

bNodeTreeInterfaceSocket *group_interface_socket_find(LayerGroup &group,
                                                      const char *name,
                                                      const StringRef socket_type,
                                                      const NodeTreeInterfaceSocketFlag flag)
{
  bNodeTreeInterfaceSocket *found = nullptr;
  group.tree->tree_interface.foreach_item([&](bNodeTreeInterfaceItem &item) {
    if (item.item_type != NodeTreeInterfaceItemType::Socket) {
      return true;
    }
    bNodeTreeInterfaceSocket &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(item);
    const bool want_input = (flag & NODE_INTERFACE_SOCKET_INPUT) != 0;
    if (((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0) != want_input) {
      return true;
    }
    if (socket.name == nullptr || !STREQ(socket.name, name)) {
      return true;
    }
    if (socket.socket_type == nullptr || StringRef(socket.socket_type) != socket_type) {
      return true;
    }
    found = &socket;
    return false;
  });
  return found;
}

/* Add an interface socket to a layer group and grow its three nodes to match. A rebuilt group
 * keeps its old interface (see #LayerGroup::used_sockets), so a socket it already carries is
 * reused: that keeps its identifier, and with it the parent's links into the group, stable. */
bNodeTreeInterfaceSocket *layer_group_add_socket(LayerGroup &group,
                                                 const char *base,
                                                 const StringRef socket_type,
                                                 const NodeTreeInterfaceSocketFlag flag)
{
  if (bNodeTreeInterfaceSocket *existing = group_interface_socket_find(
          group, base, socket_type, flag))
  {
    group.used_sockets.add(existing);
    return existing;
  }
  char name[256];
  interface_name_unique(group.tree->tree_interface, base, name, sizeof(name));
  bNodeTreeInterfaceSocket *socket = group.tree->tree_interface.add_socket(
      name, "", socket_type, flag, nullptr);
  if (socket != nullptr) {
    group.used_sockets.add(socket);
    refresh_layer_group(group);
  }
  return socket;
}

/* The value input of \a group that stands in the warm slot \a slot for \a role and \a channel. */
static bNodeTreeInterfaceSocket *group_interface_socket_find_by_slot(LayerGroup &group,
                                                                     const bUUID &slot,
                                                                     const char *role,
                                                                     const int channel,
                                                                     const StringRef socket_type)
{
  bNodeTreeInterfaceSocket *found = nullptr;
  group.tree->tree_interface.foreach_item([&](bNodeTreeInterfaceItem &item) {
    if (item.item_type != NodeTreeInterfaceItemType::Socket) {
      return true;
    }
    bNodeTreeInterfaceSocket &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(item);
    if ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) == 0 || socket.socket_type == nullptr ||
        StringRef(socket.socket_type) != socket_type)
    {
      return true;
    }
    bUUID socket_slot = BLI_uuid_nil();
    const char *socket_role = prop_string_get(socket.properties, INPUT_ROLE_PROP);
    if (socket_role == nullptr || !STREQ(socket_role, role) ||
        prop_int_get(socket.properties, INPUT_CHANNEL_PROP, -1) != channel ||
        !uid_prop_get(socket.properties, INPUT_SLOT_PROP, socket_slot) ||
        !BLI_uuid_equal(socket_slot, slot))
    {
      return true;
    }
    found = &socket;
    return false;
  });
  return found;
}

bNodeTreeInterfaceSocket *layer_group_value_input(LayerGroup &group,
                                                 const char *base,
                                                 const StringRef socket_type,
                                                 const char *role,
                                                 const bUUID &marker,
                                                 const int channel,
                                                 const bUUID &slot)
{
  bNodeTreeInterfaceSocket *socket = nullptr;
  if (!BLI_uuid_is_nil(slot)) {
    /* An item in a warm slot builds into the slot's sockets, whatever it is called: the spare and
     * the real item that takes its place then have one and the same interface. A slot never falls
     * back to the name, or a replacement spare would find the sockets the real item now owns. */
    socket = group_interface_socket_find_by_slot(group, slot, role, channel, socket_type);
    if (socket != nullptr) {
      group.used_sockets.add(socket);
    }
    else {
      char name[256];
      interface_name_unique(group.tree->tree_interface, base, name, sizeof(name));
      socket = group.tree->tree_interface.add_socket(
          name, "", socket_type, NODE_INTERFACE_SOCKET_INPUT, nullptr);
      if (socket != nullptr) {
        group.used_sockets.add(socket);
        refresh_layer_group(group);
      }
    }
  }
  else {
    socket = layer_group_add_socket(group, base, socket_type, NODE_INTERFACE_SOCKET_INPUT);
  }
  if (socket == nullptr) {
    return nullptr;
  }
  uid_prop_set(socket->properties, INPUT_SLOT_PROP, slot);
  uid_prop_set(socket->properties, INPUT_MARKER_PROP, marker);
  prop_string_set(socket->properties, INPUT_ROLE_PROP, role);
  prop_int_set(socket->properties, INPUT_CHANNEL_PROP, channel);
  return socket;
}

std::string value_key(const bUUID &marker, const char *role, const int channel)
{
  char marker_text[UUID_STRING_SIZE];
  BLI_uuid_format(marker_text, marker);
  char key[UUID_STRING_SIZE + 128];
  BLI_snprintf(key, sizeof(key), "%s|%s|%d", marker_text, role, channel);
  return std::string(key);
}

void PaintLayersTreeBuilder::build()
{
  /* The factory is the single path: without it a layer has nowhere to put its nodes. */
  BLI_assert_msg(bool(ctx_.layer_tree_get), "paint_layers_tree_build needs a layer-tree factory");
  if (!ctx_.layer_tree_get) {
    return;
  }

  /* Debug-only: a row's section has to match the list it sits in (effects vs mask_stack), or the
   * generator, the CPU compositor and the bake would disagree about what it is. */
  BKE_paint_layers_assert_consistent(ma_);
  /* Which channels the description wires at all. A channel nothing participates in gets no output,
   * so the material keeps whatever the user had on that Principled input. */
  cache_ = ctx_.regen_cache;
  wired_channels_ = paint_layers_wired_channels(ma_, cache_);

  /* The build body below (and its two recursive lambdas) still reads the state under the names it
   * used as locals; every one of them is bound to the builder member that now owns it. */
  const Material &ma = ma_;
  bNodeTree &tree = tree_;
  const Vector<int> &wired_channels = wired_channels_;
  if (wired_channels.is_empty()) {
    return;
  }

  bNodeTreeInterface &interface = tree.tree_interface;

  /* Interface sockets are created before the group input/output nodes, so those nodes get their
   * sockets immediately and can be linked right away. */

  /* One output per wired channel. */
  auto &result_outputs = result_outputs_;
  for (const int channel : wired_channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    char result_name[64];
    SNPRINTF(result_name, "Result %s", info.ui_name);
    bNodeTreeInterfaceSocket *socket = interface.add_socket(
        result_name, "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    if (socket != nullptr) {
      result_outputs.add(channel, socket);
    }
  }
  bNode *&group_input = group_input_;
  bNode *&group_output = group_output_;
  group_input = bke::node_add_node(nullptr, tree, "NodeGroupInput"_ustr);
  group_output = bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
  if (group_input == nullptr || group_output == nullptr) {
    return;
  }
  group_input->location[0] = -400;
  group_output->location[0] = 600;

  /* The root target: a row's own group is created before the row is built, so the root target is
   * only the starting scope; its own group node carries the row's values. */
  RowTarget &target = root_target_;
  target.tree = &tree;
  target.group_input = group_input;

  auto &layer_groups = layer_groups_;

  float location_y = 0.0f;
  for (const int channel : wired_channels) {
    bNodeTreeInterfaceSocket *const *result_iface = result_outputs.lookup_ptr(channel);
    if (result_iface == nullptr || (*result_iface)->identifier == nullptr) {
      continue;
    }
    bNodeSocket *result_socket = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline((*result_iface)->identifier));
    if (result_socket == nullptr) {
      continue;
    }

    /* The bottom of every chain is a constant rather than the first layer's own map: a chain that
     * started at a bare image would have no Mix node of its own, and the layer's opacity could not
     * be applied at all. Normal starts from a flat tangent-space normal, everything else from
     * transparent. */
    bNode *bottom_node = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
    if (bottom_node == nullptr) {
      continue;
    }
    bottom_node->location[0] = 0.0f;
    bottom_node->location[1] = location_y;
    /* The shared channel table: the CPU composite starts from the same value, so a partially
     * covered row fades towards the same colour on both sides. */
    float bottom_color[4];
    BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
    if (bNodeSocket *bottom_color_socket = socket_out(*bottom_node, "Color")) {
      if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(
              bottom_color_socket->default_value))
      {
        copy_v4_v4(value->value, bottom_color);
      }
    }
    ChainLayer previous;
    previous.source_node = bottom_node;
    previous.source = socket_out(*bottom_node, "Color");
    float location_x = 180.0f;

    /* Content alpha is tracked for every map channel except Normal (F2-C1): the leaf supplies it,
     * the isolating folder divides the premultiplied accumulation by coverage, and the final Result
     * composes it back. A channel outside that set leaves the chain null, so nothing is built and
     * nothing changes. */
    const bool track_content_alpha =
        BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel));

    PaintLayersChainBuilder chain(*this, location_x, location_y);
    ChainResult built = chain.build_list(ma.paint_layers, previous, false, target, channel);
    previous = built.chain;

    /* A partial factor interpolates the combine's encoded output against the base, which shortens
     * the tangent-space vector; one final decode-normalize-encode restores a unit normal, matching
     * what the shader's Normal Map node hands the BSDF and what a CPU export has to write. */
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && previous.source != nullptr) {
      auto add_vector_math = [&](const int operation, const float scale, const float offset) {
        bNode *node = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
        if (node == nullptr) {
          return node;
        }
        node->custom1 = operation;
        if (operation == NODE_VECTOR_MATH_MULTIPLY_ADD) {
          for (const char *name : {"Vector_001", "Vector_002"}) {
            bNodeSocket *socket = socket_in(*node, name);
            if (socket == nullptr || socket->default_value == nullptr) {
              continue;
            }
            const float value = (STREQ(name, "Vector_001")) ? scale : offset;
            copy_v3_fl(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, value);
          }
        }
        return node;
      };
      bNode *decode = add_vector_math(NODE_VECTOR_MATH_MULTIPLY_ADD, 2.0f, -1.0f);
      bNode *normalize = add_vector_math(NODE_VECTOR_MATH_NORMALIZE, 0.0f, 0.0f);
      bNode *encode = add_vector_math(NODE_VECTOR_MATH_MULTIPLY_ADD, 0.5f, 0.5f);
      if (decode != nullptr && normalize != nullptr && encode != nullptr) {
        bNodeSocket *decode_in = socket_in(*decode, "Vector");
        bNodeSocket *decode_out = socket_out(*decode, "Vector");
        bNodeSocket *normalize_in = socket_in(*normalize, "Vector");
        bNodeSocket *normalize_out = socket_out(*normalize, "Vector");
        bNodeSocket *encode_in = socket_in(*encode, "Vector");
        bNodeSocket *encode_out = socket_out(*encode, "Vector");
        if (decode_in != nullptr && decode_out != nullptr && normalize_in != nullptr &&
            normalize_out != nullptr && encode_in != nullptr && encode_out != nullptr)
        {
          bke::node_add_link(
              tree, *previous.source_node, *previous.source, *decode, *decode_in);
          bke::node_add_link(tree, *decode, *decode_out, *normalize, *normalize_in);
          bke::node_add_link(tree, *normalize, *normalize_out, *encode, *encode_in);
          previous.source_node = encode;
          previous.source = encode_out;
        }
      }
    }

    /* The Result's alpha is the tracked content alpha, composed over the chain's colour once: the
     * colour's own alpha was cut by the Vector divides, and stays cut. Without a tracked alpha
     * the source links directly as before. */
    if (track_content_alpha && previous.content_alpha != nullptr && previous.source != nullptr) {
      bNode *compose = bke::node_add_static_node(nullptr, tree, SH_NODE_COMPOSE_COLOR_ALPHA);
      bNodeSocket *comp_color = (compose != nullptr) ? socket_in(*compose, "Color") : nullptr;
      bNodeSocket *comp_alpha = (compose != nullptr) ? socket_in(*compose, "Alpha") : nullptr;
      bNodeSocket *comp_out = (compose != nullptr) ? socket_out(*compose, "Color") : nullptr;
      if (comp_out != nullptr) {
        compose->location[0] = 420.0f;
        compose->location[1] = location_y;
        bke::node_add_link(
            tree, *previous.source_node, *previous.source, *compose, *comp_color);
        bke::node_add_link(
            tree, *previous.content_alpha_node, *previous.content_alpha, *compose, *comp_alpha);
        previous.source_node = compose;
        previous.source = comp_out;
      }
    }

    if (previous.source != nullptr) {
      bke::node_add_link(
          tree, *previous.source_node, *previous.source, *group_output, *result_socket);
    }
    location_y += 240.0f;
  }

  /* Every value lives on the group that owns it, but only the root instance in the material's
   * embedded tree may be written during evaluation. Relay each value up to the root through the
   * interfaces it passes: a row's own group input is fed from its parent scope (a folder group's
   * group input, or the root's), and that scope is fed from its own parent in turn. The root ends up
   * carrying one mirror per value in the whole stack, fed by a link from the root's group input, so
   * #values_sync only ever writes the root instance's sockets -- never a socket inside a nested
   * tree, which is another #ID on its own.
   *
   * A scope (the root, or a folder's group) is handled in three passes, so it is O(V) in its number
   * of mirrors rather than O(V^2): collect the group instances and, depth first, fill each group's
   * own interface with mirrors for its children; then create every mirror the scope is missing,
   * finding existing ones through a key -> socket map built once; finally refresh the group input and
   * each instance once and lay the links. The links are idempotent because a nested folder tree is
   * shared by the scratch build and the real rebuild; a link that points at the wrong source (a
   * stale socket) is replaced. */
  Map<const bNodeTree *, LayerGroup *> group_by_tree;
  for (const auto &item : layer_groups.items()) {
    if (item.value->tree != nullptr) {
      group_by_tree.add(item.value->tree, item.value);
    }
  }
  mirror_scope(tree, *group_input, group_by_tree);

  prune_layer_group_sockets();

  wire_generated_uv_maps();
}

bNode *PaintLayersTreeBuilder::source_group_instance_get(const MaterialPaintLayer &layer,
                                                         bNodeTree &tree)
{
  const PaintLayersBuildContext &ctx = ctx_;
  auto &source_group_instances = source_group_instances_;
  auto &source_group_trees = source_group_trees_;
  if (bNode **found = source_group_instances.lookup_ptr(&layer)) {
    return *found;
  }
  bNode *instance = nullptr;
  const char *failure = nullptr;
  if (!ctx.source_group_get) {
    failure = "no-factory";
  }
  else if (layer.material == nullptr) {
    failure = "no-wrapper";
  }
  else if (tree.typeinfo == nullptr || tree.typeinfo->group_idname == nullptr) {
    failure = "add-failed";
  }
  else {
    bNodeTree *wrapper = ctx.source_group_get(*layer.material);
    if (wrapper == nullptr) {
      failure = "no-wrapper";
    }
    else {
      instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (instance == nullptr) {
        failure = "add-failed";
      }
      else {
        instance->id = &wrapper->id;
        id_us_plus(&wrapper->id);
        STRNCPY_UTF8(instance->label, layer.name);
        /* The instance sockets come from the wrapper's interface; build them now so the channel
         * chains can link into the COLOR/COVERAGE outputs during this same pass. */
        nodes::update_node_declaration_and_sockets(tree, *instance);
        source_group_trees.add(&layer, wrapper);
      }
    }
  }
  if (instance == nullptr) {
    PL_DEBUG_PRINTF("paint layers: row '%s': no wrapper instance (%s)\n",
                    layer.name,
                    (failure != nullptr) ? failure : "unknown");
  }
  source_group_instances.add(&layer, instance);
  return instance;
}

/**
 * What \a row shows in \a channel when its source is Material or Node Group: the same
 * Baked/Hybrid/SourceGroup resolution a Layer row's own channel content uses. One helper for
 * both, so an Effect correction with source Material/Node Group behaves exactly as the Layer row
 * of the same kind -- a live constant, a live map, or the wrapper's `COLOR:<CHANNEL>` output.
 * \a substituted mirrors the caller's own gate: a row already replaced by its bake never asks the
 * wrapper for an instance (row_is_substituted excludes Material, so this only affects Node Group).
 */
RowMaterialSource PaintLayersTreeBuilder::resolve_row_material_source(const MaterialPaintLayer &row,
                                                                      const int channel,
                                                                      bNodeTree &row_tree,
                                                                      const bool substituted)
{
  const Material &ma = ma_;
  const PaintLayersRegenCache *const cache = cache_;
  auto &source_group_trees = source_group_trees_;
  RowMaterialSource out;
  out.live_constant = BKE_paint_layers_material_live_constant(
      ma, row, channel, out.live_value, cache);
  out.live_map = !out.live_constant && BKE_paint_layers_material_live_image(
                                           ma, row, channel, &out.live_map_image, &out.live_map_iuser, cache);
  out.mode = (row.source == MA_PAINT_LAYER_SOURCE_MATERIAL) ?
                BKE_paint_layers_material_mode(ma, row, cache) :
                PaintLayerMaterialMode::Baked;
  /* See the doc comment on the call site this was extracted from: the gate keeps an Unavailable
   * channel from looking live through the wrapper's always-present `COLOR:<CHANNEL>` socket, and
   * keeps a substituted row from asking the wrapper for an instance at all. */
  if (!substituted && out.mode == PaintLayerMaterialMode::SourceGroup &&
      material_source_group_channel(ma, row, channel, cache))
  {
    out.source_group_instance = source_group_instance_get(row, row_tree);
    out.source_group_tree = source_group_trees.lookup_default(&row, nullptr);
    if (out.source_group_instance != nullptr && out.source_group_tree != nullptr) {
      out.source_group_socket = source_group_output(
          *out.source_group_tree, *out.source_group_instance, channel, false);
    }
    BLI_assert(out.source_group_instance == nullptr ||
               node_belongs_to_tree(row_tree, *out.source_group_instance));
  }
  return out;
}

/* The values of \a layer live on its own group's interface (session 10e): a row's opacity and
 * Fill constant, and each effect's and mask item's opacity and constant. The set is decided by
 * topology alone, so editing a value never changes it. */
void PaintLayersTreeBuilder::create_value_inputs(LayerGroup &group,
                                                 const MaterialPaintLayer &layer)
{
  const Material &ma = ma_;
  const PaintLayersRegenCache *const cache = cache_;
  const Vector<int> &wired_channels = wired_channels_;
  auto &opacity_inputs = opacity_inputs_;
  auto &fill_inputs = fill_inputs_;
  auto &correction_opacity_inputs = correction_opacity_inputs_;
  auto &correction_fill_inputs = correction_fill_inputs_;
  auto &live_constant_inputs = live_constant_inputs_;
  auto &correction_live_constant_inputs = correction_live_constant_inputs_;
  /* A substituted row's values are inside its bake; a group input would apply them twice. */
  if (row_is_substituted(ma, layer)) {
    return;
  }
  for (const int channel : wired_channels) {
    if (!layer_subtree_has_channel(ma, layer, channel, cache)) {
      continue;
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    char base[200];
    SNPRINTF(base,
             "%s %s Opacity",
             layer.name[0] != '\0' ? layer.name : "Layer",
             info.ui_name);
    bNodeTreeInterfaceSocket *socket = layer_group_value_input(
        group, base, "NodeSocketFloat", ROLE_OPACITY, layer.marker, channel);
    if (socket == nullptr) {
      continue;
    }
    if (socket->socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(socket->socket_data)->value =
          BKE_paint_layers_channel_opacity_effective(layer, channel) *
          pass_through_scale_of(ma, layer, cache);
    }
    opacity_inputs.lookup_or_add_default(&layer).add(channel, socket);
  }
  for (const int channel : wired_channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    if (!paint_layer_channel_present(ma, layer, channel) ||
        paint_layer_channel_image(ma, layer, channel) != nullptr)
    {
      continue;
    }
    if (!(BKE_paint_layers_role(layer) == PaintLayerRole::Layer &&
          BKE_paint_layers_kind_info(layer.source).uses_fill_color))
    {
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
      if (record == nullptr || record->value[3] <= 0.0f) {
        continue;
      }
    }
    char base[160];
    SNPRINTF(base,
             "%s %s",
             layer.name[0] != '\0' ? layer.name : "Layer",
             info.ui_name);
    bNodeTreeInterfaceSocket *socket = layer_group_value_input(
        group, base, "NodeSocketColor", ROLE_FILL, layer.marker, channel);
    if (socket == nullptr) {
      continue;
    }
    if (socket->socket_data != nullptr) {
      float color[4];
      paint_layer_channel_constant(layer, channel, color);
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->socket_data)->value, color);
    }
    fill_inputs.add(paint_layer_channel_find(layer, channel), socket);
  }
  /* A Hybrid Material row's live constant (ТЗ-26): the value lives on another material's node
   * tree, so it cannot travel through #BKE_paint_layers_custom_properties_sync like a row's own
   * Fill constant. It gets its own group input instead, filled here and kept current by
   * #values_sync_socket, so a source edit reaches the row without rebuilding its group. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    for (const int channel : wired_channels) {
      float live_value[4];
      if (!BKE_paint_layers_material_live_constant(ma, layer, channel, live_value, cache)) {
        continue;
      }
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      char base[200];
      SNPRINTF(base,
               "%s %s Source",
               layer.name[0] != '\0' ? layer.name : "Layer",
               info.ui_name);
      bNodeTreeInterfaceSocket *socket = layer_group_value_input(
          group, base, "NodeSocketColor", ROLE_LIVE_CONSTANT, layer.marker, channel);
      if (socket == nullptr) {
        continue;
      }
      if (socket->socket_data != nullptr) {
        copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->socket_data)->value, live_value);
      }
      live_constant_inputs.lookup_or_add_default(&layer).add(channel, socket);
    }
  }
  auto add_correction = [&](const MaterialPaintLayer &correction, const bool mask_item) {
    /* A spare, and the real item that took its place, build into the same interface sockets. */
    const bUUID warm_slot = paint_layers_warm_slot_of(ma, layer.marker, correction.marker);
    for (const int channel : wired_channels) {
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      char base[224];
      SNPRINTF(base,
               "%s %s %s Opacity",
               layer.name[0] != '\0' ? layer.name : "Layer",
               correction.name[0] != '\0' ? correction.name : "Correction",
               info.ui_name);
      bNodeTreeInterfaceSocket *socket = layer_group_value_input(
          group,
          base,
          "NodeSocketFloat",
          ROLE_CORRECTION_OPACITY,
          correction.marker,
          channel,
          warm_slot);
      if (socket == nullptr) {
        continue;
      }
      if (socket->socket_data != nullptr) {
        static_cast<bNodeSocketValueFloat *>(socket->socket_data)->value =
            mask_item ? BKE_paint_layers_effective_opacity(correction) :
                        BKE_paint_layers_channel_opacity_effective(correction, channel);
      }
      correction_opacity_inputs.lookup_or_add_default(&correction).add(channel, socket);
    }
    /* An Effect correction with source Material gets the same per-channel live-constant input a
     * Layer row of that source gets (ТЗ-26): the value lives on another material's node tree, so
     * it travels through a group input kept current by #values_sync_socket. #ROLE_LIVE_CONSTANT
     * is reused as-is -- #values_sync_socket looks its row up by marker, and #BKE_paint_layers_find
     * walks the whole tree, so a correction's marker resolves to the correction itself. */
    if (!mask_item && correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
      for (const int channel : wired_channels) {
        float live_value[4];
        if (!BKE_paint_layers_material_live_constant(ma, correction, channel, live_value, cache)) {
          continue;
        }
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(channel));
        char live_base[224];
        SNPRINTF(live_base,
                 "%s %s %s Source",
                 layer.name[0] != '\0' ? layer.name : "Layer",
                 correction.name[0] != '\0' ? correction.name : "Correction",
                 info.ui_name);
        bNodeTreeInterfaceSocket *live_socket = layer_group_value_input(
            group, live_base, "NodeSocketColor", ROLE_LIVE_CONSTANT, correction.marker, channel);
        if (live_socket == nullptr) {
          continue;
        }
        if (live_socket->socket_data != nullptr) {
          copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(live_socket->socket_data)->value,
                    live_value);
        }
        correction_live_constant_inputs.lookup_or_add_default(&correction).add(channel,
                                                                               live_socket);
      }
    }
    /* A Mask Item only ever reads its own single `mask_channel`, never every wired channel of
     * the owner row -- one group input, not one per channel. Not needed for Alpha: there the
     * mask's grey is the coverage itself, built straight into the tree, never a group input. */
    if (mask_item && correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL &&
        correction.mask_channel != PAINT_MATERIAL_CHANNEL_ALPHA)
    {
      const int channel = correction.mask_channel;
      float live_value[4];
      if (BKE_paint_layers_material_live_constant(ma, correction, channel, live_value, cache)) {
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(channel));
        char live_base[224];
        SNPRINTF(live_base,
                 "%s %s %s Source",
                 layer.name[0] != '\0' ? layer.name : "Layer",
                 correction.name[0] != '\0' ? correction.name : "Correction",
                 info.ui_name);
        bNodeTreeInterfaceSocket *live_socket = layer_group_value_input(
            group, live_base, "NodeSocketColor", ROLE_LIVE_CONSTANT, correction.marker, channel);
        if (live_socket != nullptr) {
          if (live_socket->socket_data != nullptr) {
            copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(live_socket->socket_data)->value,
                      live_value);
          }
          correction_live_constant_inputs.lookup_or_add_default(&correction).add(channel,
                                                                                 live_socket);
        }
      }
    }
    if (BKE_paint_layers_source_type(correction) != PaintLayerSourceType::Constant) {
      return;
    }
    char base[200];
    SNPRINTF(base,
             "%s %s Fill",
             layer.name[0] != '\0' ? layer.name : "Layer",
             correction.name[0] != '\0' ? correction.name : "Correction");
    bNodeTreeInterfaceSocket *socket = layer_group_value_input(
        group,
        base,
        "NodeSocketColor",
        ROLE_CORRECTION_FILL,
        correction.marker,
        PAINT_MATERIAL_CHANNEL_BASE_COLOR,
        warm_slot);
    if (socket != nullptr && socket->socket_data != nullptr) {
      float color[4];
      BKE_paint_layers_correction_constant(
          correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->socket_data)->value, color);
    }
    correction_fill_inputs.add(&correction, socket);
  };
  for (const MaterialPaintLayer *effect : paint_layers_build_effects(ma_, layer)) {
    if (!paint_layers_warm_defers_item(ma_, layer, *effect, false)) {
      add_correction(*effect, false);
    }
  }
  for (const MaterialPaintLayer *mask_item : paint_layers_build_mask_items(ma_, layer)) {
    if (!paint_layers_warm_defers_item(ma_, layer, *mask_item, true)) {
      add_correction(*mask_item, true);
    }
  }
}

LayerGroup *PaintLayersTreeBuilder::layer_group_ensure(const MaterialPaintLayer &layer,
                                                       bNodeTree &parent_tree)
{
  const PaintLayersBuildContext &ctx = ctx_;
  auto &layer_groups = layer_groups_;
  auto &layer_group_storage = layer_group_storage_;
  if (LayerGroup **found = layer_groups.lookup_ptr(&layer)) {
    return *found;
  }
  if (!ctx.layer_tree_get) {
    return nullptr;
  }
  bNodeTree *group_tree = ctx.layer_tree_get(layer);
  if (group_tree == nullptr || parent_tree.typeinfo == nullptr ||
      parent_tree.typeinfo->group_idname == nullptr)
  {
    return nullptr;
  }
  layer_group_storage.append(std::make_unique<LayerGroup>());
  LayerGroup &group = *layer_group_storage.last();
  layer_groups.add(&layer, &group);
  group.tree = group_tree;
  group.parent_tree = &parent_tree;
  group.unchanged = ctx.layer_tree_unchanged && ctx.layer_tree_unchanged(layer);
  /* A preserved group still holds its Group Input/Output nodes; a fresh or rebuilt one is empty
   * (the factory cleared it), so create them only when they are missing. */
  for (bNode &node : group_tree->nodes) {
    if (node.is_group_input()) {
      group.group_input = &node;
    }
    else if (node.is_group_output()) {
      group.group_output = &node;
    }
  }
  if (group.group_input == nullptr) {
    group.group_input = bke::node_add_node(nullptr, *group_tree, "NodeGroupInput"_ustr);
  }
  if (group.group_output == nullptr) {
    group.group_output = bke::node_add_node(nullptr, *group_tree, "NodeGroupOutput"_ustr);
  }
  bNode *instance = bke::node_add_node(nullptr, parent_tree, parent_tree.typeinfo->group_idname);
  if (instance != nullptr) {
    instance->id = &group_tree->id;
    /* The factory tree has no users; assigning the id makes this instance its only one. */
    id_us_plus(&group_tree->id);
    STRNCPY_UTF8(instance->label, layer.name);
    group.instance = instance;
  }
  refresh_layer_group(group);
  /* A preserved group already carries its values; only a rebuilt one re-creates them. */
  if (!group.unchanged) {
    create_value_inputs(group, layer);
  }
  return &group;
}

/** The parent-side view of a preserved group's contract for \a channel, without rebuilding it. */
RowResult PaintLayersTreeBuilder::row_from_unchanged_group(LayerGroup &group,
                                                           const MaterialPaintLayer &layer,
                                                           const int channel)
{
  RowResult result;
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  group.tree->ensure_interface_cache();
  auto instance_socket = [&](const char *kind, const bool output) -> bNodeSocket * {
    char name[192];
    SNPRINTF(name, "%s %s", kind, info.ui_name);
    for (bNodeTreeInterfaceSocket *iface : (output ? group.tree->interface_outputs() :
                                                      group.tree->interface_inputs()))
    {
      if (iface->name != nullptr && iface->identifier != nullptr && STREQ(iface->name, name)) {
        return bke::node_find_socket(
            *group.instance,
            output ? SOCK_OUT : SOCK_IN,
            UString::from_ptr_noinline(iface->identifier));
      }
    }
    return nullptr;
  };
  result.group_instance = group.instance;
  result.group_below = instance_socket("Below", false);
  result.group_color = instance_socket("Color", true);
  result.group_coverage = instance_socket("Coverage", true);
  result.group_blend = instance_socket("Blend", true);
  result.group_result = instance_socket("Result", true);
  /* Optional: only present when the group tracked a content alpha (Base Color Paint content). */
  result.group_content_alpha = instance_socket("Content Alpha", true);
  result.current.layer = &layer;
  result.grouped = true;
  result.valid = result.group_instance != nullptr && result.group_below != nullptr &&
                 result.group_color != nullptr && result.group_coverage != nullptr &&
                 result.group_result != nullptr;
  return result;
}

void PaintLayersTreeBuilder::mirror_scope(
    bNodeTree &scope_tree,
    bNode &scope_group_input,
    Map<const bNodeTree *, LayerGroup *> &group_by_tree)
{
/* (a) The scope's group instances, and depth first: a folder's interface is filled with its
 * own children's mirrors before it is mirrored into its parent. */
Vector<bNode *> group_nodes;
for (bNode &node : scope_tree.nodes) {
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    continue;
  }
  bNodeTree *group_tree = id_cast<bNodeTree *>(node.id);
  if (group_tree == nullptr) {
    continue;
  }
  /* Only the generated layer/folder groups carry value inputs; a source wrapper or the
   * Normal Combine group must not be walked or have its interface touched. */
  bUUID layer_marker = BLI_uuid_nil();
  if (!uid_prop_get(group_tree->id.properties, TREE_LAYER_PROP, layer_marker)) {
    continue;
  }
  group_nodes.append(&node);
  bNode *child_group_input = nullptr;
  for (bNode &candidate : group_tree->nodes) {
    if (candidate.is_group_input()) {
      child_group_input = &candidate;
      break;
    }
  }
  if (child_group_input != nullptr) {
    mirror_scope(*group_tree, *child_group_input, group_by_tree);
  }
}
if (group_nodes.is_empty()) {
  return;
}

/* (b) The mirrors this scope already carries, by key, so a rebuild reuses their
 * identifiers. Scanned once instead of once per value. */
Map<std::string, bNodeTreeInterfaceSocket *> mirrors;
scope_tree.ensure_interface_cache();
for (bNodeTreeInterfaceSocket *candidate : scope_tree.interface_inputs()) {
  if (prop_int_get(candidate->properties, INPUT_MIRROR_PROP, 0) == 0) {
    continue;
  }
  const char *role = prop_string_get(candidate->properties, INPUT_ROLE_PROP);
  bUUID marker = BLI_uuid_nil();
  if (role == nullptr ||
      !uid_prop_get(candidate->properties, INPUT_MARKER_PROP, marker))
  {
    continue;
  }
  mirrors.add(value_key(value_slot_or_marker(candidate->properties, marker),
                        role,
                        prop_int_get(candidate->properties, INPUT_CHANNEL_PROP, -1)),
              candidate);
}

struct MirrorLink {
  bNode *instance;
  bNodeTreeInterfaceSocket *group_iface;
  bNodeTreeInterfaceSocket *scope_iface;
};
Vector<MirrorLink> links;
for (bNode *node : group_nodes) {
  bNodeTree *group_tree = id_cast<bNodeTree *>(node->id);
  /* The recursion above may have grown group_tree's interface; refresh the instance once. */
  nodes::update_node_declaration_and_sockets(scope_tree, *node);
  group_tree->ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : group_tree->interface_inputs()) {
    const char *role = prop_string_get(iface->properties, INPUT_ROLE_PROP);
    if (role == nullptr) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!uid_prop_get(iface->properties, INPUT_MARKER_PROP, marker)) {
      continue;
    }
    /* A socket whose layer or correction no longer exists is stale: a rebuilt group keeps
     * its old interface and its own stale-socket prune has not run yet, so it must not be
     * mirrored up (that would leak the value into every parent scope and keep the socket
     * alive). Mirrors for values that do still exist resolve here. */
    if (BKE_paint_layers_find(const_cast<Material &>(ma_), marker) == nullptr &&
        !paint_layers_warm_item_present(ma_, marker))
    {
      continue;
    }
    const int channel = prop_int_get(iface->properties, INPUT_CHANNEL_PROP, -1);
    /* A value in a warm slot is keyed by the slot, so the real item that takes the spare's place
     * finds the spare's mirror and the root's interface stays as it was. */
    const bUUID slot = value_slot_or_marker(iface->properties, marker);
    const std::string key = value_key(slot, role, channel);
    bNodeTreeInterfaceSocket *scope_iface = mirrors.lookup_default(key, nullptr);
    if (scope_iface != nullptr) {
      uid_prop_set(scope_iface->properties, INPUT_MARKER_PROP, marker);
      uid_prop_set(scope_iface->properties,
                   INPUT_SLOT_PROP,
                   BLI_uuid_equal(slot, marker) ? BLI_uuid_nil() : slot);
    }
    if (scope_iface == nullptr) {
      const char *base = (iface->name != nullptr) ? iface->name : "Value";
      char name[256];
      interface_name_unique(scope_tree.tree_interface, base, name, sizeof(name));
      scope_iface = scope_tree.tree_interface.add_socket(
          name, "", iface->socket_type, NODE_INTERFACE_SOCKET_INPUT, nullptr);
      if (scope_iface == nullptr) {
        continue;
      }
      prop_int_set(scope_iface->properties, INPUT_MIRROR_PROP, 1);
      uid_prop_set(scope_iface->properties, INPUT_MARKER_PROP, marker);
      uid_prop_set(scope_iface->properties,
                   INPUT_SLOT_PROP,
                   BLI_uuid_equal(slot, marker) ? BLI_uuid_nil() : slot);
      prop_string_set(scope_iface->properties, INPUT_ROLE_PROP, role);
      prop_int_set(scope_iface->properties, INPUT_CHANNEL_PROP, channel);
      mirrors.add(key, scope_iface);
    }
    /* A mirror on a folder group's interface is a socket the build must keep: register it
     * so the stale-socket prune below does not drop it on the next rebuild. */
    if (LayerGroup **owner_group = group_by_tree.lookup_ptr(&scope_tree)) {
      (*owner_group)->used_sockets.add(scope_iface);
    }
    if (iface->identifier != nullptr) {
      links.append({node, iface, scope_iface});
    }
  }
}

/* (c) Refresh the scope's group input once, then lay every link. */
nodes::update_node_declaration_and_sockets(scope_tree, scope_group_input);
for (const MirrorLink &link : links) {
  bNodeSocket *src = bke::node_find_socket(scope_group_input,
                                           SOCK_OUT,
                                           UString::from_ptr_noinline(
                                               link.scope_iface->identifier));
  bNodeSocket *dst = bke::node_find_socket(
      *link.instance, SOCK_IN, UString::from_ptr_noinline(link.group_iface->identifier));
  if (src == nullptr || dst == nullptr) {
    continue;
  }
  if (dst->link != nullptr && dst->link->fromsock == src) {
    /* Already the right link: keep it, so the pass stays idempotent. */
    continue;
  }
  if (dst->link != nullptr) {
    /* A stale link (the mirror was rebuilt with a new source socket): drop it. */
    bke::node_remove_link(&scope_tree, *dst->link);
  }
  bke::node_add_link(scope_tree, scope_group_input, *src, *link.instance, *dst);
}
}

void PaintLayersTreeBuilder::prune_layer_group_sockets()
{
/* A rebuilt group kept its old interface so its socket identifiers -- and the parent's links
 * into them -- stay stable; drop the sockets this build no longer uses so the interface matches
 * what a fresh build would produce (and its signature, which the root hash reads, is honest). */
for (const auto &item : layer_groups_.items()) {
  LayerGroup &group = *item.value;
  if (group.unchanged || group.tree == nullptr) {
    continue;
  }
  Vector<bNodeTreeInterfaceSocket *> stale;
  group.tree->tree_interface.foreach_item([&](bNodeTreeInterfaceItem &iface_item) {
    if (iface_item.item_type != NodeTreeInterfaceItemType::Socket) {
      return true;
    }
    auto &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(iface_item);
    if (!group.used_sockets.contains(&socket)) {
      stale.append(&socket);
    }
    return true;
  });
  for (bNodeTreeInterfaceSocket *socket : stale) {
    group.tree->tree_interface.remove_item(
        reinterpret_cast<bNodeTreeInterfaceItem &>(*socket));
  }
  if (!stale.is_empty()) {
    refresh_layer_group(group);
  }
}
}

void PaintLayersTreeBuilder::wire_generated_uv_maps()
{
/* Every Image Texture the build created samples the one UV layer the material names. Each layer
 * group is a separate tree, so each that holds a map gets its own UV Map node; a tree with no map
 * gets none. The root is handled the same way. */
const char *const uv_name = BKE_paint_layers_uv_map_name(ma_);
generated_uv_maps_wire(tree_, uv_name);
for (const auto &item : layer_groups_.items()) {
  if (item.value->tree != nullptr) {
    generated_uv_maps_wire(*item.value->tree, uv_name);
  }
}
}

}  // namespace bke::paint_layers

void paint_layers_tree_build(const Material &ma,
                             bNodeTree &tree,
                             const PaintLayersBuildContext &ctx)
{
  bke::paint_layers::PaintLayersTreeBuilder builder(ma, tree, ctx);
  builder.build();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Regenerate
 * \{ */

}  // namespace blender

