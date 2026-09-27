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
using namespace bke::paint_layers;

/**
 * One layer as the chain builder sees it: its source socket, and where its opacity comes from.
 *
 * The owner nodes are carried alongside the sockets because `bNodeSocket::owner_node()` reads a
 * runtime pointer that is only filled by the topology cache; the cache is stale while the tree is
 * being built, so asking a freshly created socket for its node would answer null.
 */
struct ChainLayer {
  const MaterialPaintLayer *layer = nullptr;
  bNode *source_node = nullptr;
  bNodeSocket *source = nullptr;
  /** The opacity interface input, before any per-pixel mask is folded in. */
  bNode *opacity_node = nullptr;
  bNodeSocket *opacity = nullptr;
  /** What the Mix/Combine factor links from: the opacity, or opacity times a mask map's alpha. */
  bNode *factor_node = nullptr;
  bNodeSocket *factor = nullptr;
  /** Float chain for content alpha (Base Color only). Null when not tracked (treat as 1.0). */
  bNode *content_alpha_node = nullptr;
  bNodeSocket *content_alpha = nullptr;
};

/** A chain's result plus, for a folder's contents, the coverage it accumulated. */
struct ChainResult {
  ChainLayer chain;
  /** The node owning #coverage; null for the root chain, which carries no coverage. */
  bNode *coverage_node = nullptr;
  bNodeSocket *coverage = nullptr;
  /** Scalar content alpha, parallel to coverage. Null when no Paint leaf contributes alpha. */
  bNode *content_alpha_node = nullptr;
  bNodeSocket *content_alpha = nullptr;
};

/**
 * Where a single row's nodes go: the tree and the group input its values are read from. A row's
 * body always builds inside its own layer group, so that group input carries the row's value inputs
 * (session 10e).
 */
struct RowTarget {
  bNodeTree *tree = nullptr;
  bNode *group_input = nullptr;
  float location_x = 0.0f;
  float location_y = 0.0f;
};

/** One built row: its source and factor, or invalid when the row takes part in nothing here. */
struct RowResult {
  bool valid = false;
  ChainLayer current;
  /** For a folder row, the coverage its contents accumulated; null otherwise. */
  bNode *folder_coverage_node = nullptr;
  bNodeSocket *folder_coverage = nullptr;
  /** For a folder row, the content alpha its contents accumulated; null otherwise. */
  bNode *folder_content_alpha_node = nullptr;
  bNodeSocket *folder_content_alpha = nullptr;
  /** True when the row was built inside its own layer group, not in the parent. */
  bool grouped = false;
  bNode *group_instance = nullptr;
  /** The instance sockets the parent chains through: Below in, Color/Coverage/Blend/Result out. */
  bNodeSocket *group_below = nullptr;
  bNodeSocket *group_color = nullptr;
  bNodeSocket *group_coverage = nullptr;
  bNodeSocket *group_content_alpha = nullptr;
  bNodeSocket *group_blend = nullptr;
  bNodeSocket *group_result = nullptr;
};

/** A leaf's or folder's own `.PL …` node group, its nodes, and its instance in the parent tree. */
struct LayerGroup {
  bNodeTree *tree = nullptr;
  bNode *group_input = nullptr;
  bNode *group_output = nullptr;
  bNode *instance = nullptr;
  bNodeTree *parent_tree = nullptr;
  /** The factory handed back a tree whose topology hash already matches: do not rebuild it. */
  bool unchanged = false;
  /**
   * The interface sockets this build created or reused. A rebuilt group's interface is not cleared
   * (so its socket identifiers, and the root's links into it, stay stable); anything not touched
   * this build is removed once the group is fully built.
   */
  Set<bNodeTreeInterfaceSocket *> used_sockets;
};
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

}  // namespace bke::paint_layers

void paint_layers_tree_build(const Material &ma,
                             bNodeTree &tree,
                             const PaintLayersBuildContext &ctx)
{
  /* The factory is the single path: without it a layer has nowhere to put its nodes. */
  BLI_assert_msg(bool(ctx.layer_tree_get), "paint_layers_tree_build needs a layer-tree factory");
  if (!ctx.layer_tree_get) {
    return;
  }

  /* Debug-only: a row's section has to match the list it sits in (effects vs mask_stack), or the
   * generator, the CPU compositor and the bake would disagree about what it is. */
  BKE_paint_layers_assert_consistent(ma);
  /* Which channels the description wires at all. A channel nothing participates in gets no output,
   * so the material keeps whatever the user had on that Principled input. */
  const PaintLayersRegenCache *const cache = ctx.regen_cache;
  const Vector<int> wired_channels = paint_layers_wired_channels(ma, cache);
  if (wired_channels.is_empty()) {
    return;
  }

  bNodeTreeInterface &interface = tree.tree_interface;

  /* Interface sockets are created before the group input/output nodes, so those nodes get their
   * sockets immediately and can be linked right away. */

  /* Values are interface inputs of the group that owns them (session 10e): a row's opacity and Fill
   * constant, and each effect's and mask item's opacity and constant. They are created on the owning
   * layer's own group when it is ensured; these maps let the row builder find the socket. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> opacity_inputs;
  Map<const MaterialPaintLayerChannel *, bNodeTreeInterfaceSocket *> fill_inputs;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> correction_opacity_inputs;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> correction_fill_inputs;
  /* A Hybrid Material row's live constant per channel (ТЗ-26): the source's default value is a
   * value, not topology, so it travels through a group input like opacity and Fill, instead of
   * being baked into an RGB node's default. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> live_constant_inputs;
  /* The same, for an Effect correction with source Material (see #resolve_row_material_source). */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> correction_live_constant_inputs;


  /* One output per wired channel. */
  Map<int, bNodeTreeInterfaceSocket *> result_outputs;
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
  bNode *group_input = bke::node_add_node(nullptr, tree, "NodeGroupInput"_ustr);
  bNode *group_output = bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
  if (group_input == nullptr || group_output == nullptr) {
    return;
  }
  group_input->location[0] = -400;
  group_output->location[0] = 600;

  /* The root target: a row's own group is created before the row is built, so the root target is
   * only the starting scope; its own group node carries the row's values. */
  RowTarget target;
  target.tree = &tree;
  target.group_input = group_input;

  /* A layer's or folder's group is created once per layer, across channels: one group holds every
   * channel's Below/Color/Coverage/Blend/Result and the value mirrors the row uses. The groups are
   * stored by pointer so a nested group's #LayerGroup::parent stays valid as the map grows. */
  Map<const MaterialPaintLayer *, LayerGroup *> layer_groups;
  Vector<std::unique_ptr<LayerGroup>> layer_group_storage;

  /* A Material row in SourceGroup mode instantiates its source wrapper once per row, across all
   * channels: every channel's chain reads one output of the same instance node. */
  Map<const MaterialPaintLayer *, bNode *> source_group_instances;
  Map<const MaterialPaintLayer *, bNodeTree *> source_group_trees;
  auto source_group_instance_get = [&](const MaterialPaintLayer &layer, bNodeTree &tree) -> bNode * {
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
  };

  /**
   * What \a row shows in \a channel when its source is Material or Node Group: the same
   * Baked/Hybrid/SourceGroup resolution a Layer row's own channel content uses. One helper for
   * both, so an Effect correction with source Material/Node Group behaves exactly as the Layer row
   * of the same kind -- a live constant, a live map, or the wrapper's `COLOR:<CHANNEL>` output.
   * \a substituted mirrors the caller's own gate: a row already replaced by its bake never asks the
   * wrapper for an instance (row_is_substituted excludes Material, so this only affects Node Group).
   */
  struct RowMaterialSource {
    PaintLayerMaterialMode mode = PaintLayerMaterialMode::Baked;
    bool live_constant = false;
    float live_value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool live_map = false;
    Image *live_map_image = nullptr;
    const ImageUser *live_map_iuser = nullptr;
    bNode *source_group_instance = nullptr;
    bNodeTree *source_group_tree = nullptr;
    bNodeSocket *source_group_socket = nullptr;
  };
  auto resolve_row_material_source = [&](const MaterialPaintLayer &row,
                                         const int channel,
                                         bNodeTree &row_tree,
                                         const bool substituted) -> RowMaterialSource {
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
  };

  auto refresh_layer_group = [&](LayerGroup &group) {
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
  };

  /** An existing interface socket of \a group with \a name, \a socket_type and direction, or null. */
  auto group_interface_socket_find = [&](LayerGroup &group,
                                         const char *name,
                                         const StringRef socket_type,
                                         const NodeTreeInterfaceSocketFlag flag)
      -> bNodeTreeInterfaceSocket * {
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
  };

  /* Add an interface socket to a layer group and grow its three nodes to match. A rebuilt group
   * keeps its old interface (see #LayerGroup::used_sockets), so a socket it already carries is
   * reused: that keeps its identifier, and with it the parent's links into the group, stable. */
  auto layer_group_add_socket = [&](LayerGroup &group,
                                    const char *base,
                                    const StringRef socket_type,
                                    const NodeTreeInterfaceSocketFlag flag)
      -> bNodeTreeInterfaceSocket * {
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
  };

  /** Add a value input to \a group's interface, tagged so values_sync can find it. */
  auto layer_group_value_input = [&](LayerGroup &group,
                                     const char *base,
                                     const StringRef socket_type,
                                     const char *role,
                                     const bUUID &marker,
                                     const int channel) -> bNodeTreeInterfaceSocket * {
    bNodeTreeInterfaceSocket *socket = layer_group_add_socket(
        group, base, socket_type, NODE_INTERFACE_SOCKET_INPUT);
    if (socket == nullptr) {
      return nullptr;
    }
    uid_prop_set(socket->properties, INPUT_MARKER_PROP, marker);
    prop_string_set(socket->properties, INPUT_ROLE_PROP, role);
    prop_int_set(socket->properties, INPUT_CHANNEL_PROP, channel);
    return socket;
  };

  /* The values of \a layer live on its own group's interface (session 10e): a row's opacity and
   * Fill constant, and each effect's and mask item's opacity and constant. The set is decided by
   * topology alone, so editing a value never changes it. */
  auto create_value_inputs = [&](LayerGroup &group, const MaterialPaintLayer &layer) {
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
            group, base, "NodeSocketFloat", ROLE_CORRECTION_OPACITY, correction.marker, channel);
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
          PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      if (socket != nullptr && socket->socket_data != nullptr) {
        float color[4];
        BKE_paint_layers_correction_constant(
            correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
        copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->socket_data)->value, color);
      }
      correction_fill_inputs.add(&correction, socket);
    };
    for (const MaterialPaintLayer &effect :
         layer.effects)
    {
      add_correction(effect, false);
    }
    for (const MaterialPaintLayer &mask_item :
         layer.mask_stack)
    {
      add_correction(mask_item, true);
    }
  };

  auto layer_group_ensure = [&](const MaterialPaintLayer &layer,
                                bNodeTree &parent_tree) -> LayerGroup * {
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
  };

  /** The parent-side view of a preserved group's contract for \a channel, without rebuilding it. */
  auto row_from_unchanged_group = [&](LayerGroup &group,
                                      const MaterialPaintLayer &layer,
                                      const int channel) -> RowResult {
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
  };

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

    std::function<ChainResult(const ListBaseT<MaterialPaintLayer> &, ChainLayer, bool, const RowTarget &, int)>
        build_list = [&](const ListBaseT<MaterialPaintLayer> &list,
                         ChainLayer previous,
                         const bool premul,
                         const RowTarget &parent_target,
                         const int channel) -> ChainResult {
      /* Shadows the enclosing per-channel loop's own `channel` for this whole call and everything
       * it recurses into (including the nested #build_row below): a Stack mask's subtree builds in
       * its own fixed `mask_channel` (or Base Color, for the Alpha convention), never in whichever
       * channel the owner row happens to be generating right now -- see the two call sites below
       * that pass something other than the plain `channel` they were handed. #track_content_alpha
       * is re-derived from this parameter for the same reason: Normal is the only channel that does
       * not track it, and a mask fixed to a non-Normal channel must track it even while the owner
       * itself is generating its own Normal row. */
      const bool track_content_alpha =
          BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel));
      /* This list builds in the tree it is handed: the root, or a folder's own group. */
      bNodeTree &tree = *parent_target.tree;
      bNode *coverage_node = nullptr;
      bNodeSocket *coverage = nullptr;
      bNode *content_alpha_node = nullptr;
      bNodeSocket *content_alpha = nullptr;
      if (premul) {
        bNode *zero = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        if (zero != nullptr) {
          coverage_node = zero;
          coverage = socket_out(*zero, "Value");
          if (coverage != nullptr && coverage->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(coverage->default_value)->value = 0.0f;
          }
        }
      }
      auto build_row = [&](const MaterialPaintLayer *layer,
                           const RowTarget &target,
                           const bool substituted,
                           Image *baked_color,
                           const bool premul,
                           LayerGroup *layer_group) -> RowResult {
        /* A row a past rebuild left out of the graph contributes nothing. */
        if (row_is_removed(ma, *layer)) {
          return {};
        }
        /* The aliases keep the row body unchanged; for a leaf target points at the row's own group,
         * where the value socket mirrors each root input the row uses. */
        bNodeTree &tree = *target.tree;
        bNode *group_input = target.group_input;
        /* The row's values are inputs of its own group; its Group Input node exposes them. */
        auto group_input_socket =
            [&](const bNodeTreeInterfaceSocket &iface) -> bNodeSocket * {
          if (group_input == nullptr || iface.identifier == nullptr) {
            return nullptr;
          }
          return bke::node_find_socket(
              *group_input, SOCK_OUT, UString::from_ptr_noinline(iface.identifier));
        };
        const float location_x = target.location_x;
        const float location_y = target.location_y;
        /* A Custom and a Material row alike have no generated subtree: a valid bake substitutes
         * above, and without one there is no channel record and the row drops out below. */

        /* Set while THIS row is built and it is itself a folder: its source is the sub-chain's
         * straight result, and its factor is multiplied by the sub-chain's coverage. These are
         * local to one #build_row call (not the enclosing #build_list's shared state): a
         * correction's own children -- and an isolating folder's own children -- recurse back into
         * #build_row for a different #layer, and a shared copy would carry that nested call's
         * folder bookkeeping right past its own return, into a completely unrelated row's coverage
         * multiply near this call's end (see the crash this fixed: a Stack correction's own row
         * inherited a grouped child's leftover folder-coverage node, in the child's tree, and linked
         * it into this row's tree). */
        bNode *folder_source_node = nullptr;
        bNodeSocket *folder_source = nullptr;
        bNode *folder_coverage_node = nullptr;
        bNodeSocket *folder_coverage = nullptr;
        bNode *folder_content_alpha_node = nullptr;
        bNodeSocket *folder_content_alpha = nullptr;
        /* The active Material row's live value for this channel, when it has one. A live constant
         * needs no sampler; a live map is the source's own texture. Both count as the row taking
         * part even without a channel record, so the early drop below must see them. */
        const RowMaterialSource row_source = resolve_row_material_source(
            *layer, channel, tree, substituted);
        float live_value[4];
        copy_v4_v4(live_value, row_source.live_value);
        Image *live_map_image = row_source.live_map_image;
        const ImageUser *live_map_iuser = row_source.live_map_iuser;
        const bool live_constant = row_source.live_constant;
        const bool live_map = row_source.live_map;
        /* A Material row whose whole source graph goes through the wrapper group. Its instance is
         * created once per row and reused for every channel. */
        const PaintLayerMaterialMode material_mode = row_source.mode;
        bNode *source_group_instance = row_source.source_group_instance;
        bNodeTree *source_group_tree = row_source.source_group_tree;
        bNodeSocket *source_group_socket = row_source.source_group_socket;

        if (!substituted && BKE_paint_layers_is_folder(*layer)) {
          /* A folder: its contents are built in isolation -- pre-multiplied colour and coverage --
           * and then its own row lays the isolated result over what is below (design §5). */
          bNode *p_zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
          bNodeSocket *p_zero_out = (p_zero != nullptr) ? socket_out(*p_zero, "Color") : nullptr;
          if (p_zero_out == nullptr) {
            return {};
          }
          if (p_zero_out->default_value != nullptr) {
            copy_v4_fl(
                static_cast<bNodeSocketValueRGBA *>(p_zero_out->default_value)->value, 0.0f);
          }
          ChainLayer sub_previous;
          sub_previous.source_node = p_zero;
          sub_previous.source = p_zero_out;
          ChainResult sub = build_list(layer->children, sub_previous, true, target, channel);
          if (sub.chain.source == nullptr || sub.coverage == nullptr) {
            return {};
          }
          /* S_folder = P / a; the Vector Math divide is per channel and safe (0 on zero), so an
           * empty folder answers zero and covers nothing. */
          bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
          bNodeSocket *div_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
          bNodeSocket *div_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") : nullptr;
          bNodeSocket *div_out = (divide != nullptr) ? socket_out(*divide, "Vector") : nullptr;
          if (div_out == nullptr) {
            return {};
          }
          divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
          divide->location[0] = location_x;
          divide->location[1] = location_y - 480.0f;
          bke::node_add_link(tree, *sub.chain.source_node, *sub.chain.source, *divide, *div_a);
          bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *divide, *div_b);
          folder_source_node = divide;
          folder_source = div_out;
          folder_coverage_node = sub.coverage_node;
          folder_coverage = sub.coverage;
          /* The content alpha is accumulated beside the premultiplied colour, so it is straightened
           * the same way: S_alpha = P_alpha / a. A null accumulation means no Paint leaf tracked an
           * alpha here, so the chain stays null and no Content Alpha socket is built. */
          folder_content_alpha_node = nullptr;
          folder_content_alpha = nullptr;
          if (track_content_alpha && sub.content_alpha != nullptr) {
            bNode *alpha_divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNodeSocket *ad_a = (alpha_divide != nullptr) ? socket_in(*alpha_divide, "Value") :
                                                            nullptr;
            bNodeSocket *ad_b = (alpha_divide != nullptr) ? socket_in(*alpha_divide, "Value_001") :
                                                            nullptr;
            bNodeSocket *ad_out = (alpha_divide != nullptr) ? socket_out(*alpha_divide, "Value") :
                                                              nullptr;
            if (ad_out != nullptr) {
              alpha_divide->custom1 = NODE_MATH_DIVIDE;
              alpha_divide->location[0] = location_x;
              alpha_divide->location[1] = location_y - 400.0f;
              bke::node_add_link(
                  tree, *sub.content_alpha_node, *sub.content_alpha, *alpha_divide, *ad_a);
              bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *alpha_divide, *ad_b);
              folder_content_alpha_node = alpha_divide;
              folder_content_alpha = ad_out;
            }
          }
        }
        else if (!substituted && !BKE_paint_layers_is_folder(*layer) &&
                 !leaf_participates(ma, *layer, channel) && !live_constant && !live_map &&
                 source_group_instance == nullptr)
        {
          if (!paint_layer_channel_present(ma, *layer, channel) &&
              layer->source == MA_PAINT_LAYER_SOURCE_NODE_GROUP &&
              custom_bake_missing_warn_once(ma, *layer))
          {
            fprintf(stderr,
                    "Paint layers: Custom layer '%s' has no bake yet and is skipped until one "
                    "lands\n",
                    layer->name);
          }
          return {};
        }

      ChainLayer current;
      current.layer = layer;
      if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
              opacity_inputs.lookup_ptr(layer))
      {
        if (bNodeTreeInterfaceSocket **opacity_iface = opacity_by_channel->lookup_ptr(channel)) {
          current.opacity_node = group_input;
          current.opacity = group_input_socket(**opacity_iface);
        }
      }

      /* The row's own channel map, when it has one: its alpha is the row's per-pixel coverage, the
       * same read #composite_image_layers_build sets up with #color_alpha_coverage. */
      bNode *leaf_map_node = nullptr;
      if (substituted) {
        bNode *baked_color_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        bNode *baked_coverage_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        if (baked_color_node == nullptr || baked_coverage_node == nullptr) {
          return {};
        }
        baked_color_node->id = &baked_color->id;
        id_us_plus(&baked_color->id);
        baked_color_node->location[0] = location_x;
        baked_color_node->location[1] = location_y;
        /* A substituted row's bake stores content alpha in the same Image Texture Alpha output as a
         * live Paint/Fill map (both write IMA_ALPHA_STRAIGHT, #bake_image_ensure), so it starts the
         * content-alpha chain here exactly as the live leaf does at its own Image Texture node. Not
         * gated by kind: Custom has no live path and only reaches the generator through this branch,
         * and Material never reaches it at all (its bake substitutes a whole subtree, not a row). */
        if (track_content_alpha) {
          current.content_alpha_node = baked_color_node;
          current.content_alpha = socket_out(*baked_color_node, "Alpha");
        }
        baked_coverage_node->id = &layer->bake->coverage->id;
        id_us_plus(&layer->bake->coverage->id);
        baked_coverage_node->location[0] = location_x - 90.0f;
        baked_coverage_node->location[1] = location_y - 160.0f;
        current.source_node = baked_color_node;
        current.source = socket_out(*baked_color_node, "Color");
        /* The coverage map stores the scalar in grey RGB (alpha = 1, mask-correction convention);
         * the factor is the mean of the three channels, exactly as the CPU reads it. */
        bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
        bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (separate != nullptr && add_xy != nullptr && add_z != nullptr && divide != nullptr) {
          add_xy->custom1 = NODE_MATH_ADD;
          add_z->custom1 = NODE_MATH_ADD;
          divide->custom1 = NODE_MATH_DIVIDE;
          bNodeSocket *sep_vector = socket_in(*separate, "Vector");
          bNodeSocket *sep_x = socket_out(*separate, "X");
          bNodeSocket *sep_y = socket_out(*separate, "Y");
          bNodeSocket *sep_z = socket_out(*separate, "Z");
          bNodeSocket *xy_a = socket_in(*add_xy, "Value");
          bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
          bNodeSocket *z_a = socket_in(*add_z, "Value");
          bNodeSocket *z_b = socket_in(*add_z, "Value_001");
          bNodeSocket *d_a = socket_in(*divide, "Value");
          bNodeSocket *d_b = socket_in(*divide, "Value_001");
          if (sep_vector == nullptr || sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
              xy_a == nullptr || xy_b == nullptr || z_a == nullptr || z_b == nullptr ||
              d_a == nullptr || d_b == nullptr)
          {
            return {};
          }
          bke::node_add_link(tree,
                             *baked_coverage_node,
                             *socket_out(*baked_coverage_node, "Color"),
                             *separate,
                             *sep_vector);
          bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
          bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
          bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
          bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
          bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
          if (d_b->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
          }
          current.opacity_node = divide;
          current.opacity = socket_out(*divide, "Value");
        }
      }
      else {
        /* A constant answers first: it needs no sampler, so a channel the resolver calls Constant
         * never falls through to an Image result. */
        Image *image = (live_constant || live_map || source_group_instance != nullptr) ?
                           nullptr :
                           paint_layer_channel_image(ma, *layer, channel);
        if (live_constant) {
          /* The active Material row shows its source's live constant rather than its baked map.
           * The value lives in another material, so it is not topology (ТЗ-26): it is read from
           * this row's own group input, filled by #create_value_inputs and kept current by
           * #values_sync_socket, exactly like a row's own Fill constant. */
          bNodeTreeInterfaceSocket *live_constant_iface = nullptr;
          if (Map<int, bNodeTreeInterfaceSocket *> *live_constant_by_channel =
                  live_constant_inputs.lookup_ptr(layer))
          {
            if (bNodeTreeInterfaceSocket **found = live_constant_by_channel->lookup_ptr(channel)) {
              live_constant_iface = *found;
            }
          }
          bNodeSocket *constant_out = (live_constant_iface != nullptr) ?
                                          group_input_socket(*live_constant_iface) :
                                          nullptr;
          if (constant_out == nullptr) {
            return {};
          }
          current.source_node = group_input;
          current.source = constant_out;
          /* A Material row's transparency is the Alpha input, never the channel's own value: the
           * Principled ignores Base Color's RGBA alpha. It tracks no content alpha of its own --
           * the material alpha already fed `layer_factor` -- so the Result alpha keeps the chain's
           * blended value, exactly as the CPU computes it. */
        }
        else if (live_map) {
          /* The row shows the source's own texture. Its sampling settings travel with it; no Divide
           * is built here, exactly like the row's own map branch below -- the Image Texture node
           * handles a non-data texture's un-premultiply itself. */
          bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
          if (map == nullptr) {
            return {};
          }
          map->id = &live_map_image->id;
          id_us_plus(&live_map_image->id);
          map->location[0] = location_x;
          map->location[1] = location_y;
          if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
            if (live_map_iuser != nullptr) {
              dst->iuser = *live_map_iuser;
            }
            MaterialSourceResolve resolve_local;
            const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(
                layer->material, cache, resolve_local);
            const bNode *src_node = resolve.images[channel].node;
            if (const NodeTexImage *src_storage =
                    (src_node != nullptr) ? static_cast<const NodeTexImage *>(src_node->storage) :
                                            nullptr)
            {
              dst->interpolation = src_storage->interpolation;
              dst->extension = src_storage->extension;
              dst->projection = src_storage->projection;
            }
          }
          current.source_node = map;
          current.source = socket_out(*map, "Color");
          leaf_map_node = map;
          /* A Material row's transparency is the source's Alpha input, not the channel map's own
           * alpha (the Principled's Base Color reads RGB only). It tracks no content alpha of its
           * own: the material alpha already fed `layer_factor`, and leaving the chain null keeps
           * the Result alpha the chain's blended value, exactly as the CPU computes it. */
        }
        else if (source_group_instance != nullptr) {
          /* The whole source graph goes through the wrapper's COLOR:<CHANNEL> output. No map, so
           * content coverage for this row comes from the wrapper's COVERAGE output below. */
          if (source_group_socket == nullptr) {
            PL_DEBUG_PRINTF(
                "paint layers: row '%s' channel %d: wrapper has no COLOR output, row dropped\n",
                layer->name,
                channel);
            return {};
          }
          current.source_node = source_group_instance;
          current.source = source_group_socket;
        }
        else if (folder_source != nullptr) {
          /* The folder's own row: its source is the isolated sub-chain, not a map. */
          current.source_node = folder_source_node;
          current.source = folder_source;
          current.content_alpha_node = folder_content_alpha_node;
          current.content_alpha = folder_content_alpha;
        }
        else if (image != nullptr) {
          bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
          if (map == nullptr) {
            return {};
          }
          map->id = &image->id;
          id_us_plus(&image->id);
          map->location[0] = location_x;
          map->location[1] = location_y;
          /* A MESH_MAP atlas is sampled with Extend (clamp to the edge), the mode the CPU's own
           * bilinear resample uses, so a differently sized atlas agrees at its borders. A painted
           * map keeps its Repeat default. Read through the shared resolver so the two sides cannot
           * disagree about whether the row is a mesh map at all. */
          const bool mesh_map = layer->source == MA_PAINT_LAYER_SOURCE_MESH_MAP &&
                                paint_layer_mesh_map_image(ma, *layer) == image;
          if (mesh_map) {
            if (NodeTexImage *storage = static_cast<NodeTexImage *>(map->storage)) {
              storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
            }
          }
          current.source_node = map;
          current.source = socket_out(*map, "Color");
          /* A scalar atlas (AO, Curvature, Edge) stores its value in R; spread it across RGB so a
           * colour channel reads it as grey and the CPU reads the same. An RGB atlas (Normal, IDs)
           * is wired as it is. */
          if (mesh_map && paint_layer_mesh_map_is_scalar(layer->mesh_map_type)) {
            bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
            bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
            if (separate != nullptr && combine != nullptr) {
              bNodeSocket *sep_vector = socket_in(*separate, "Vector");
              bNodeSocket *sep_x = socket_out(*separate, "X");
              bNodeSocket *combine_x = socket_in(*combine, "X");
              bNodeSocket *combine_y = socket_in(*combine, "Y");
              bNodeSocket *combine_z = socket_in(*combine, "Z");
              if (sep_vector != nullptr && sep_x != nullptr && combine_x != nullptr &&
                  combine_y != nullptr && combine_z != nullptr)
              {
                separate->location[0] = location_x + 80.0f;
                separate->location[1] = location_y;
                combine->location[0] = location_x + 160.0f;
                combine->location[1] = location_y;
                bke::node_add_link(tree, *map, *current.source, *separate, *sep_vector);
                bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_x);
                bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_y);
                bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_z);
                current.source_node = combine;
                current.source = socket_out(*combine, "Vector");
              }
            }
          }
          leaf_map_node = map;
          /* A Paint or Fill map carries its content alpha in the Image Texture Alpha output; it
           * starts the content-alpha chain here rather than being read back out of the Color's own
           * alpha. Custom stays outside since its bake substitutes above instead of reaching this
           * branch.
           *
           * A Material row never takes its content alpha from the channel map: the Principled's
           * transparency is the Alpha input, and a real bake map is opaque. It tracks no content
           * alpha at all, so the Result alpha stays the chain's blended value like the CPU's. */
          if (track_content_alpha &&
              (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer &&
              ELEM(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)))
          {
            current.content_alpha_node = map;
            current.content_alpha = socket_out(*map, "Alpha");
          }
        }
        else {
          const MaterialPaintLayerChannel *entry = paint_layer_channel_find(*layer, channel);
          if (bNodeTreeInterfaceSocket **fill_iface = fill_inputs.lookup_ptr(entry)) {
            current.opacity_node = group_input;
            current.source_node = group_input;
            current.source = group_input_socket(**fill_iface);
          }
          /* A Paint or Fill constant's alpha is the constant colour's `.a`, the same number the RGB
           * Fill input carries; it is frozen into a Value so the content-alpha chain has a scalar
           * leaf. Custom and Material leaves (F2-C3/C4) supply no such constant. */
          if (track_content_alpha &&
              (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer &&
              ELEM(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) &&
              current.source != nullptr)
          {
            float constant[4];
            paint_layer_channel_constant(*layer, channel, constant);
            bNode *alpha_value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
            bNodeSocket *alpha_out = (alpha_value != nullptr) ? socket_out(*alpha_value, "Value") :
                                                                nullptr;
            if (alpha_value != nullptr && alpha_out != nullptr &&
                alpha_out->default_value != nullptr)
            {
              alpha_value->location[0] = location_x - 90.0f;
              alpha_value->location[1] = location_y - 40.0f;
              static_cast<bNodeSocketValueFloat *>(alpha_out->default_value)->value = constant[3];
              current.content_alpha_node = alpha_value;
              current.content_alpha = alpha_out;
            }
          }
        }
      }
      if (current.source == nullptr) {
        return {};
      }

      /* The grey of \a image (the mean of RGB) as a new node chain, the way the CPU reads a mask
       * item and a coverage map (#PaintMaterialCompositeImageLayer::mask_reads_grey,
       * ::coverage_image): masks are painted black and white, and a brush writes colour, not alpha. */
      auto grey_of_map = [&](Image &image,
                             const float offset_y) -> std::pair<bNode *, bNodeSocket *> {
        bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
        bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (map == nullptr || separate == nullptr || add_xy == nullptr || add_z == nullptr ||
            divide == nullptr)
        {
          return {nullptr, nullptr};
        }
        map->id = &image.id;
        id_us_plus(&image.id);
        map->location[0] = location_x - 90.0f;
        map->location[1] = location_y + offset_y;
        add_xy->custom1 = NODE_MATH_ADD;
        add_z->custom1 = NODE_MATH_ADD;
        divide->custom1 = NODE_MATH_DIVIDE;
        bNodeSocket *map_color = socket_out(*map, "Color");
        bNodeSocket *sep_vector = socket_in(*separate, "Vector");
        bNodeSocket *sep_x = socket_out(*separate, "X");
        bNodeSocket *sep_y = socket_out(*separate, "Y");
        bNodeSocket *sep_z = socket_out(*separate, "Z");
        bNodeSocket *xy_a = socket_in(*add_xy, "Value");
        bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
        bNodeSocket *z_a = socket_in(*add_z, "Value");
        bNodeSocket *z_b = socket_in(*add_z, "Value_001");
        bNodeSocket *d_a = socket_in(*divide, "Value");
        bNodeSocket *d_b = socket_in(*divide, "Value_001");
        if (map_color == nullptr || sep_vector == nullptr || sep_x == nullptr ||
            sep_y == nullptr || sep_z == nullptr || xy_a == nullptr || xy_b == nullptr ||
            z_a == nullptr || z_b == nullptr || d_a == nullptr || d_b == nullptr)
        {
          return {nullptr, nullptr};
        }
        bke::node_add_link(tree, *map, *map_color, *separate, *sep_vector);
        bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
        bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
        bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
        bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
        bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
        if (d_b->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
        }
        return {divide, socket_out(*divide, "Value")};
      };

      /* A Material/Node Group correction's own coverage: the same Baked/Hybrid/SourceGroup
       * precedence #resolve_row_material_source uses for content, but always read on the Alpha
       * channel and ending at the wrapper's dedicated COVERAGE output (SourceGroup) or the
       * correction's own bake coverage (Baked) -- mirrors `layer_factor` below (~2276-2356)
       * exactly, with \a row standing in for `*layer`. One helper for a content Effect (its own
       * coverage multiplier) and a Mask Item (its coverage when `mask_channel` is not Alpha), so
       * the two cannot disagree about what "the source's coverage" means. */
      auto resolve_correction_coverage =
          [&](const MaterialPaintLayer &row) -> std::pair<bNode *, bNodeSocket *> {
        if (row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
          /* A Node Group has no live path at all (it is Baked-only); its "coverage" is its own
           * bake coverage, the same map a substituted row's factor would read. */
          if (row.bake != nullptr && row.bake->coverage != nullptr) {
            return grey_of_map(*row.bake->coverage, -240.0f);
          }
          return {nullptr, nullptr};
        }
        const RowMaterialSource alpha_source = resolve_row_material_source(
            row, PAINT_MATERIAL_CHANNEL_ALPHA, tree, false);
        if (alpha_source.live_constant) {
          bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
          bNodeSocket *value_out = (value != nullptr) ? socket_out(*value, "Value") : nullptr;
          if (value != nullptr && value_out != nullptr && value_out->default_value != nullptr) {
            value->location[0] = location_x - 90.0f;
            value->location[1] = location_y - 240.0f;
            static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value =
                alpha_source.live_value[0];
            return {value, value_out};
          }
          return {nullptr, nullptr};
        }
        if (alpha_source.live_map) {
          bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
          bNodeSocket *map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
          if (map != nullptr && map_alpha != nullptr) {
            map->id = &alpha_source.live_map_image->id;
            id_us_plus(&alpha_source.live_map_image->id);
            map->location[0] = location_x - 90.0f;
            map->location[1] = location_y - 240.0f;
            if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
              if (alpha_source.live_map_iuser != nullptr) {
                dst->iuser = *alpha_source.live_map_iuser;
              }
            }
            return {map, map_alpha};
          }
          return {nullptr, nullptr};
        }
        if (alpha_source.source_group_instance != nullptr &&
            alpha_source.source_group_tree != nullptr)
        {
          bNodeSocket *coverage_out = source_group_output(*alpha_source.source_group_tree,
                                                           *alpha_source.source_group_instance,
                                                           PAINT_MATERIAL_CHANNEL_ALPHA,
                                                           true);
          if (coverage_out != nullptr) {
            return {alpha_source.source_group_instance, coverage_out};
          }
        }
        if (row.bake != nullptr && row.bake->coverage != nullptr) {
          return grey_of_map(*row.bake->coverage, -240.0f);
        }
        return {nullptr, nullptr};
      };

      /* The factor base the mask stack builds on: one, times -- for a Material layer -- its
       * source's coverage (what the source's own transparency baked into), so a mask on a
       * transparent source limits it further and never re-bakes it. */
      bNode *layer_factor_node = nullptr;
      bNodeSocket *layer_factor_socket = nullptr;
      float live_alpha[4];
      Image *live_alpha_image = nullptr;
      const ImageUser *live_alpha_iuser = nullptr;
      if (!substituted && BKE_paint_layers_material_live_constant(
                              ma, *layer, PAINT_MATERIAL_CHANNEL_ALPHA, live_alpha, cache))
      {
        /* The source's alpha is a constant too, so the coverage stays live with it. When the
         * source's alpha is not constant while its other channels are, the coverage keeps its last
         * bake -- a bounded divergence: only channels that can be shown live are. */
        bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        bNodeSocket *value_out = (value != nullptr) ? socket_out(*value, "Value") : nullptr;
        if (value != nullptr && value_out != nullptr && value_out->default_value != nullptr) {
          value->location[0] = location_x - 90.0f;
          value->location[1] = location_y - 320.0f;
          static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value = live_alpha[0];
          layer_factor_node = value;
          layer_factor_socket = value_out;
        }
      }
      else if (!substituted &&
               BKE_paint_layers_material_live_image(ma,
                                                    *layer,
                                                    PAINT_MATERIAL_CHANNEL_ALPHA,
                                                    &live_alpha_image,
                                                    &live_alpha_iuser,
                                                    cache))
      {
        /* The source's alpha is a live texture: the factor is that map's Alpha output, the same
         * output the CPU reads as its coverage. */
        bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        bNodeSocket *map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
        if (map != nullptr && map_alpha != nullptr) {
          map->id = &live_alpha_image->id;
          id_us_plus(&live_alpha_image->id);
          map->location[0] = location_x - 90.0f;
          map->location[1] = location_y - 320.0f;
          if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
            if (live_alpha_iuser != nullptr) {
              dst->iuser = *live_alpha_iuser;
            }
            MaterialSourceResolve resolve_local;
            const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(
                layer->material, cache, resolve_local);
            const bNode *src_node = resolve.images[PAINT_MATERIAL_CHANNEL_ALPHA].node;
            if (const NodeTexImage *src_storage =
                    (src_node != nullptr) ? static_cast<const NodeTexImage *>(src_node->storage) :
                                            nullptr)
            {
              dst->interpolation = src_storage->interpolation;
              dst->extension = src_storage->extension;
              dst->projection = src_storage->projection;
            }
          }
          layer_factor_node = map;
          layer_factor_socket = map_alpha;
        }
      }
      else if (!substituted && source_group_instance != nullptr && source_group_tree != nullptr) {
        /* The wrapper's own COVERAGE output is the row's factor. Without one it behaves like a
         * source with no live alpha: the baked coverage stands in. */
        bNodeSocket *coverage_out = source_group_output(
            *source_group_tree, *source_group_instance, PAINT_MATERIAL_CHANNEL_ALPHA, true);
        if (coverage_out != nullptr) {
          layer_factor_node = source_group_instance;
          layer_factor_socket = coverage_out;
        }
        else if (layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer->bake != nullptr &&
                 layer->bake->coverage != nullptr)
        {
          std::tie(layer_factor_node, layer_factor_socket) = grey_of_map(*layer->bake->coverage,
                                                                         -320.0f);
        }
      }
      else if (!substituted && layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL &&
               layer->bake != nullptr && layer->bake->coverage != nullptr)
      {
        std::tie(layer_factor_node, layer_factor_socket) = grey_of_map(*layer->bake->coverage,
                                                                       -320.0f);
      }
      /* The row's own content coverage, kept apart from the mask: an unpainted texel of a fresh map
       * is transparent black and must show the rows below, and the content corrections raise this
       * coverage. The mask multiplies it in afterwards, so a mask always clips a correction.
       *
       * A Material row has no such coverage of its own: the Principled's Base Color reads RGB only,
       * so the channel map's alpha (a real bake is opaque) must not be folded into the factor -- the
       * material's transparency already arrived as `layer_factor` from the Alpha input. */
      bNode *content_cov_node = nullptr;
      bNodeSocket *content_cov = nullptr;
      if (leaf_map_node != nullptr &&
          !ELEM(layer->source, MA_PAINT_LAYER_SOURCE_MATERIAL, MA_PAINT_LAYER_SOURCE_MESH_MAP))
      {
        content_cov = socket_out(*leaf_map_node, "Alpha");
        content_cov_node = (content_cov != nullptr) ? leaf_map_node : nullptr;
      }
      /* The per-pixel coverage the mask stack builds on: the Material source coverage when there is
       * one, one otherwise. Null until an item needs it. */
      auto ensure_factor_base = [&]() {
        if (layer_factor_socket != nullptr) {
          return;
        }
        bNode *white = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
        if (white != nullptr) {
          white->location[0] = location_x - 60.0f;
          white->location[1] = location_y - 320.0f;
          bNodeSocket *white_out = socket_out(*white, "Color");
          if (white_out != nullptr && white_out->default_value != nullptr) {
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[0] = 1.0f;
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[1] = 1.0f;
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[2] = 1.0f;
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[3] = 1.0f;
          }
          layer_factor_node = white;
          layer_factor_socket = white_out;
        }
      };

      /* Content corrections adjust the colour the row paints with, before its own blend. Only the
       * exact subset the CPU composites: a Paint correction backed by a map, or a Fill correction
       * carrying a constant. On the Normal channel a content Paint correction rides the same
       * Normal Combine the row does, while a content Fill correction (a constant normal) is left
       * out here too, so the two never disagree about which rows contributed. */
      if (!substituted) {
        const bool normal_channel = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
        for (const MaterialPaintLayer *effect : BKE_paint_layers_effects(*layer)) {
          const MaterialPaintLayer &correction = *effect;
          if (!ELEM(correction.source,
                    MA_PAINT_LAYER_SOURCE_IMAGE,
                    MA_PAINT_LAYER_SOURCE_CONSTANT,
                    MA_PAINT_LAYER_SOURCE_MESH_MAP,
                    MA_PAINT_LAYER_SOURCE_MATERIAL,
                    MA_PAINT_LAYER_SOURCE_NODE_GROUP,
                    MA_PAINT_LAYER_SOURCE_STACK))
          {
            continue;
          }
          const bool fill = BKE_paint_layers_source_type(correction) ==
                            PaintLayerSourceType::Constant;
          /* A constant normal makes no sense; the CPU skips this correction as well. */
          if (normal_channel && fill) {
            continue;
          }
          bNodeSocket *correction_opacity = nullptr;
          if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
                  correction_opacity_inputs.lookup_ptr(&correction))
          {
            if (bNodeTreeInterfaceSocket **opacity_iface =
                    opacity_by_channel->lookup_ptr(channel))
            {
              correction_opacity = group_input_socket(**opacity_iface);
            }
          }
          bNode *correction_source = nullptr;
          bNodeSocket *correction_color = nullptr;
          bNodeSocket *correction_alpha = nullptr;
          /* The node owning the colour socket: the group input for a Fill, the map for a Paint. */
          bNode *correction_color_node = nullptr;
          /* A Material correction's own coverage, exactly like a Layer row of that source
           * (#BKE_paint_layers_material_lives_from_source's caller reads `layer_factor` the same
           * way): the source's Alpha input decides its transparency, never the content channel's own
           * map alpha (a Material row has no coverage of its own -- see the comment on `content_cov`
           * below). Populated only for a Material correction; every other kind keeps using
           * `correction_alpha`, its own content map's alpha. */
          bNode *correction_material_coverage_node = nullptr;
          bNodeSocket *correction_material_coverage_socket = nullptr;
          /* Whether the effect's map is read as colour data. The Image Texture node only
           * un-premultiplies a non-data texture, so a data map needs the chain's own Divide (the
           * same rule the mask chain follows). */
          bool content_map_is_data = false;
          if (fill) {
            if (bNodeTreeInterfaceSocket **fill_iface =
                    correction_fill_inputs.lookup_ptr(&correction))
            {
              correction_color = group_input_socket(**fill_iface);
              correction_color_node = group_input;
            }
            if (correction_color == nullptr) {
              continue;
            }
          }
          else if (correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
            /* An Effect correction with source Material behaves exactly as a Layer row of that
             * source: the same Baked/Hybrid/SourceGroup resolution, through the one helper both
             * share. */
            const RowMaterialSource row_source = resolve_row_material_source(
                correction, channel, tree, false);
            if (row_source.live_constant) {
              if (Map<int, bNodeTreeInterfaceSocket *> *live_by_channel =
                      correction_live_constant_inputs.lookup_ptr(&correction))
              {
                if (bNodeTreeInterfaceSocket **live_iface = live_by_channel->lookup_ptr(channel)) {
                  correction_color = group_input_socket(**live_iface);
                  correction_color_node = group_input;
                }
              }
              if (correction_color == nullptr) {
                continue;
              }
            }
            else if (row_source.live_map) {
              bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
              if (map == nullptr) {
                continue;
              }
              map->id = &row_source.live_map_image->id;
              id_us_plus(&row_source.live_map_image->id);
              map->location[0] = location_x;
              map->location[1] = location_y - 160.0f;
              if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
                if (row_source.live_map_iuser != nullptr) {
                  dst->iuser = *row_source.live_map_iuser;
                }
              }
              correction_color = socket_out(*map, "Color");
              correction_alpha = socket_out(*map, "Alpha");
              correction_color_node = map;
              correction_source = map;
              if (correction_color == nullptr) {
                continue;
              }
            }
            else if (row_source.source_group_instance != nullptr &&
                     row_source.source_group_socket != nullptr)
            {
              /* The whole source graph goes through the wrapper's COLOR:<CHANNEL> output, exactly
               * like a Layer row in SourceGroup mode. */
              correction_color = row_source.source_group_socket;
              correction_color_node = row_source.source_group_instance;
            }
            else {
              /* Baked: the correction's own external bake, read through the same resolver a Layer
               * row's Baked mode uses. */
              Image *correction_image = paint_layer_channel_image(ma, correction, channel);
              if (correction_image == nullptr) {
                continue;
              }
              content_map_is_data = IMB_colormanagement_space_name_is_data(
                  correction_image->colorspace_settings.name);
              correction_source = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
              if (correction_source == nullptr) {
                continue;
              }
              correction_source->id = &correction_image->id;
              id_us_plus(&correction_image->id);
              correction_source->location[0] = location_x;
              correction_source->location[1] = location_y - 160.0f;
              correction_color = socket_out(*correction_source, "Color");
              correction_alpha = socket_out(*correction_source, "Alpha");
              correction_color_node = correction_source;
              if (correction_color == nullptr) {
                continue;
              }
            }
            /* The correction's own coverage: the same Baked/Hybrid/SourceGroup precedence
             * #resolve_row_material_source uses for content, but read on the Alpha channel and
             * ending at the wrapper's dedicated COVERAGE output (SourceGroup) or the correction's
             * own bake coverage (Baked) -- mirrors `layer_factor` above (~2276-2356) exactly, with
             * `correction` standing in for `*layer`. */
            const RowMaterialSource alpha_source = resolve_row_material_source(
                correction, PAINT_MATERIAL_CHANNEL_ALPHA, tree, false);
            if (alpha_source.live_constant) {
              bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
              bNodeSocket *value_out = (value != nullptr) ? socket_out(*value, "Value") : nullptr;
              if (value != nullptr && value_out != nullptr && value_out->default_value != nullptr) {
                value->location[0] = location_x - 90.0f;
                value->location[1] = location_y - 240.0f;
                static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value =
                    alpha_source.live_value[0];
                correction_material_coverage_node = value;
                correction_material_coverage_socket = value_out;
              }
            }
            else if (alpha_source.live_map) {
              bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
              bNodeSocket *map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
              if (map != nullptr && map_alpha != nullptr) {
                map->id = &alpha_source.live_map_image->id;
                id_us_plus(&alpha_source.live_map_image->id);
                map->location[0] = location_x - 90.0f;
                map->location[1] = location_y - 240.0f;
                if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
                  if (alpha_source.live_map_iuser != nullptr) {
                    dst->iuser = *alpha_source.live_map_iuser;
                  }
                }
                correction_material_coverage_node = map;
                correction_material_coverage_socket = map_alpha;
              }
            }
            else if (alpha_source.source_group_instance != nullptr &&
                     alpha_source.source_group_tree != nullptr)
            {
              bNodeSocket *coverage_out = source_group_output(*alpha_source.source_group_tree,
                                                               *alpha_source.source_group_instance,
                                                               PAINT_MATERIAL_CHANNEL_ALPHA,
                                                               true);
              if (coverage_out != nullptr) {
                correction_material_coverage_node = alpha_source.source_group_instance;
                correction_material_coverage_socket = coverage_out;
              }
              else if (correction.bake != nullptr && correction.bake->coverage != nullptr) {
                std::tie(correction_material_coverage_node, correction_material_coverage_socket) =
                    grey_of_map(*correction.bake->coverage, -240.0f);
              }
            }
            else if (correction.bake != nullptr && correction.bake->coverage != nullptr) {
              std::tie(correction_material_coverage_node, correction_material_coverage_socket) =
                  grey_of_map(*correction.bake->coverage, -240.0f);
            }
          }
          else if (correction.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
            /* A Node Group correction is Baked-only, exactly like a Layer row of that source: no
             * live path exists, so a correction with no valid bake yet takes no part (mirrors the
             * Custom row's own #custom_bake_missing_warn_once skip). */
            Image *correction_baked = nullptr;
            if (!BKE_paint_layers_bake_substitute(ma, correction, channel, &correction_baked) &&
                !BKE_paint_layers_bake_substitute_custom(
                    ma, correction, channel, &correction_baked, nullptr))
            {
              continue;
            }
            if (correction_baked == nullptr) {
              continue;
            }
            correction_source = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
            if (correction_source == nullptr) {
              continue;
            }
            correction_source->id = &correction_baked->id;
            id_us_plus(&correction_baked->id);
            correction_source->location[0] = location_x;
            correction_source->location[1] = location_y - 160.0f;
            correction_color = socket_out(*correction_source, "Color");
            correction_alpha = socket_out(*correction_source, "Alpha");
            correction_color_node = correction_source;
            if (correction_color == nullptr) {
              continue;
            }
          }
          else if (correction.source == MA_PAINT_LAYER_SOURCE_STACK) {
            /* A Stack correction composites its own children in isolation, exactly like a Layer
             * folder composites its own (#build_list over `layer->children` above, ~1957-1993):
             * accumulate premultiplied colour and coverage starting from transparent, then the
             * same Vector Math Divide straightens the result to `S = P / a`. The straight colour
             * is this correction's content; the coverage stands in for a Material correction's own
             * Alpha input below (`correction_material_coverage_*`), never a content map's alpha --
             * a Stack correction has no map of its own, only its children's accumulated result. */
            bNode *p_zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
            bNodeSocket *p_zero_out = (p_zero != nullptr) ? socket_out(*p_zero, "Color") : nullptr;
            if (p_zero_out == nullptr) {
              continue;
            }
            if (p_zero_out->default_value != nullptr) {
              copy_v4_fl(static_cast<bNodeSocketValueRGBA *>(p_zero_out->default_value)->value,
                         0.0f);
            }
            ChainLayer sub_previous;
            sub_previous.source_node = p_zero;
            sub_previous.source = p_zero_out;
            ChainResult sub = build_list(correction.children, sub_previous, true, target, channel);
            if (sub.chain.source == nullptr || sub.coverage == nullptr) {
              continue;
            }
            bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
            bNodeSocket *div_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
            bNodeSocket *div_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") : nullptr;
            bNodeSocket *div_out = (divide != nullptr) ? socket_out(*divide, "Vector") : nullptr;
            if (div_out == nullptr) {
              continue;
            }
            divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
            divide->location[0] = location_x;
            divide->location[1] = location_y - 160.0f;
            bke::node_add_link(tree, *sub.chain.source_node, *sub.chain.source, *divide, *div_a);
            bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *divide, *div_b);
            correction_color = div_out;
            correction_color_node = divide;
            correction_material_coverage_node = sub.coverage_node;
            correction_material_coverage_socket = sub.coverage;
          }
          else {
            const bool correction_mesh_map = correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP;
            Image *correction_image = paint_layer_channel_image(ma, correction, channel);
            if (correction_image == nullptr) {
              continue;
            }
            /* Maps from files saved before #IMA_GPU_LINEAR_PREMUL existed, or assigned by hand,
             * get it here; its texture is rebuilt because the storage format changes. A MESH_MAP
             * atlas is a material-owned image, not a paint map, so it is left alone. */
            if (!correction_mesh_map && (correction_image->flag & IMA_GPU_LINEAR_PREMUL) == 0) {
              correction_image->flag |= IMA_GPU_LINEAR_PREMUL;
              BKE_image_free_gputextures(correction_image);
            }
            content_map_is_data = !correction_mesh_map &&
                                  IMB_colormanagement_space_name_is_data(
                                      correction_image->colorspace_settings.name);
            correction_source = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
            if (correction_source != nullptr) {
              correction_source->id = &correction_image->id;
              id_us_plus(&correction_image->id);
              correction_source->location[0] = location_x;
              correction_source->location[1] = location_y - 160.0f;
              if (correction_mesh_map) {
                if (NodeTexImage *storage = static_cast<NodeTexImage *>(
                        correction_source->storage))
                {
                  storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
                }
              }
              correction_color = socket_out(*correction_source, "Color");
              correction_alpha = socket_out(*correction_source, "Alpha");
              correction_color_node = correction_source;
              /* A scalar atlas spreads its R across RGB, matching the row and the CPU. */
              if (correction_mesh_map &&
                  paint_layer_mesh_map_is_scalar(correction.mesh_map_type))
              {
                bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
                bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
                if (separate != nullptr && combine != nullptr) {
                  bNodeSocket *sep_vector = socket_in(*separate, "Vector");
                  bNodeSocket *sep_x = socket_out(*separate, "X");
                  bNodeSocket *combine_x = socket_in(*combine, "X");
                  bNodeSocket *combine_y = socket_in(*combine, "Y");
                  bNodeSocket *combine_z = socket_in(*combine, "Z");
                  if (sep_vector != nullptr && sep_x != nullptr && combine_x != nullptr &&
                      combine_y != nullptr && combine_z != nullptr)
                  {
                    separate->location[0] = location_x + 80.0f;
                    separate->location[1] = location_y - 160.0f;
                    combine->location[0] = location_x + 160.0f;
                    combine->location[1] = location_y - 160.0f;
                    bke::node_add_link(
                        tree, *correction_source, *correction_color, *separate, *sep_vector);
                    bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_x);
                    bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_y);
                    bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_z);
                    correction_color_node = combine;
                    correction_color = socket_out(*combine, "Vector");
                  }
                }
              }
            }
            if (correction_color == nullptr) {
              continue;
            }
          }
          /* On the Normal channel the correction is the same Normal Combine the row itself uses;
           * elsewhere the row's own blend mode is a MixRGB. */
          bNode *correction_mix = nullptr;
          bNodeSocket *mix_color1 = nullptr;
          bNodeSocket *mix_color2 = nullptr;
          bNodeSocket *mix_fac = nullptr;
          bNodeSocket *mix_out = nullptr;
          if (normal_channel) {
            if (ctx.normal_combine_group == nullptr || tree.typeinfo == nullptr ||
                tree.typeinfo->group_idname == nullptr)
            {
              continue;
            }
            correction_mix = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
            if (correction_mix == nullptr) {
              continue;
            }
            correction_mix->id = &ctx.normal_combine_group->id;
            id_us_plus(&ctx.normal_combine_group->id);
            correction_mix->location[0] = location_x + 120.0f;
            correction_mix->location[1] = location_y;
            /* A hand-assigned group only grows its instance sockets once its declaration is built;
             * this instantiates them from the group's interface without needing #Main or a whole
             * tree update. */
            nodes::update_node_declaration_and_sockets(tree, *correction_mix);
            mix_color1 = bke::node_find_socket(
                *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
            mix_color2 = bke::node_find_socket(
                *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
            mix_fac = bke::node_find_socket(
                *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
            mix_out = bke::node_find_socket(
                *correction_mix, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
          }
          else {
            correction_mix = mix_node_add(tree,
                                          BKE_paint_layers_blend_to_ramp(
                                              eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(correction, channel))),
                                          location_x + 120.0f,
                                          location_y);
            if (correction_mix == nullptr) {
              continue;
            }
            mix_color1 = socket_in(*correction_mix, "A_Color");
            mix_color2 = socket_in(*correction_mix, "B_Color");
            mix_fac = socket_in(*correction_mix, "Factor_Float");
            mix_out = socket_out(*correction_mix, "Result_Color");
          }
          STRNCPY_UTF8(correction_mix->label, correction.name);
          if (mix_color1 == nullptr || mix_color2 == nullptr || mix_fac == nullptr ||
              mix_out == nullptr)
          {
            /* The sockets are the node's own declaration; a missing one is a build error. */
            BLI_assert_msg(false, "blend node sockets were not declared");
            continue;
          }
          bke::node_add_link(
              tree, *current.source_node, *current.source, *correction_mix, *mix_color1);
          /* A data map reaches the chain as `C * A` (the upload pre-multiplied it and the Image
           * Texture node leaves a data texture alone); divide by the map's alpha to get `C`, so the
           * coverage is applied once, exactly as the CPU reads the straight bytes. A non-data map
           * is already straight: the node un-premultiplied it, so nothing is built. */
          if (!fill && content_map_is_data && correction_source != nullptr &&
              correction_alpha != nullptr)
          {
            bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
            bNode *straighten = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
            if (combine != nullptr && straighten != nullptr) {
              bNodeSocket *combine_x = socket_in(*combine, "X");
              bNodeSocket *combine_y = socket_in(*combine, "Y");
              bNodeSocket *combine_z = socket_in(*combine, "Z");
              bNodeSocket *combine_out = socket_out(*combine, "Vector");
              bNodeSocket *vector_in = socket_in(*straighten, "Vector");
              bNodeSocket *divisor_in = socket_in(*straighten, "Vector_001");
              bNodeSocket *vector_out = socket_out(*straighten, "Vector");
              if (combine_x != nullptr && combine_y != nullptr && combine_z != nullptr &&
                  combine_out != nullptr && vector_in != nullptr && divisor_in != nullptr &&
                  vector_out != nullptr)
              {
                straighten->custom1 = NODE_VECTOR_MATH_DIVIDE;
                combine->location[0] = location_x;
                combine->location[1] = location_y - 240.0f;
                straighten->location[0] = location_x + 40.0f;
                straighten->location[1] = location_y - 160.0f;
                bke::node_add_link(
                    tree, *correction_source, *correction_alpha, *combine, *combine_x);
                bke::node_add_link(
                    tree, *correction_source, *correction_alpha, *combine, *combine_y);
                bke::node_add_link(
                    tree, *correction_source, *correction_alpha, *combine, *combine_z);
                bke::node_add_link(
                    tree, *correction_source, *correction_color, *straighten, *vector_in);
                bke::node_add_link(
                    tree, *combine, *combine_out, *straighten, *divisor_in);
                correction_color_node = straighten;
                correction_color = vector_out;
              }
            }
          }
          /* The colour arrives from a group input (Fill) or a map (Paint). */
          if (correction_color_node != nullptr) {
            bke::node_add_link(
                tree, *correction_color_node, *correction_color, *correction_mix, *mix_color2);
          }
          bNodeSocket *correction_coverage_socket = nullptr;
          bNode *correction_coverage_node = nullptr;
          if (correction_opacity != nullptr && group_input != nullptr) {
            /* A Material or Stack correction's per-pixel coverage is its own Alpha-channel
             * resolution or its subtree's accumulated coverage (`correction_material_coverage_*`,
             * built above like `layer_factor`), never the content channel's own map alpha -- neither
             * has a content map of its own (see the comment on `content_cov`). Every other kind uses
             * its content map's alpha (`correction_alpha`) the way it always did. A Fill correction's
             * factor is its opacity alone, with no per-pixel term to scale it; a Material or Stack
             * correction with none of its own (Baked with no bake coverage yet, or an empty subtree)
             * behaves the same way for this purpose. */
            const bool material_correction = ELEM(
                correction.source, MA_PAINT_LAYER_SOURCE_MATERIAL, MA_PAINT_LAYER_SOURCE_STACK);
            bNodeSocket *coverage_map_socket = material_correction ? correction_material_coverage_socket :
                                                                     correction_alpha;
            bNode *coverage_map_node = material_correction ? correction_material_coverage_node :
                                                              correction_source;
            if (fill || coverage_map_socket == nullptr || coverage_map_node == nullptr) {
              /* A flat correction's factor is its opacity; it covers fully. */
              correction_coverage_socket = correction_opacity;
              correction_coverage_node = group_input;
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
            }
            else {
              bNode *correction_factor = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (correction_factor == nullptr) {
                continue;
              }
              correction_factor->custom1 = NODE_MATH_MULTIPLY;
              correction_factor->location[0] = location_x + 40.0f;
              correction_factor->location[1] = location_y + 160.0f;
              bNodeSocket *factor_value = socket_in(*correction_factor, "Value");
              bNodeSocket *factor_coverage = socket_in(*correction_factor, "Value_001");
              bNodeSocket *factor_out = socket_out(*correction_factor, "Value");
              if (factor_value == nullptr || factor_coverage == nullptr || factor_out == nullptr) {
                continue;
              }
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *correction_factor, *factor_value);
              bke::node_add_link(
                  tree, *coverage_map_node, *coverage_map_socket, *correction_factor, *factor_coverage);
              bke::node_add_link(
                  tree, *correction_factor, *factor_out, *correction_mix, *mix_fac);
              correction_coverage_socket = factor_out;
              correction_coverage_node = correction_factor;
            }
          }
          /* A content correction changes the colour and, by the over model, the coverage the row
           * lays with: coverage = coverage + f * (1 - coverage). The base is the row's own
           * coverage: a folder's isolated result (design §5), or a leaf's mask map. A leaf with no
           * mask map covers fully, so there the update is the identity and nothing is built. */
          if (correction_coverage_socket != nullptr && correction_coverage_node != nullptr) {
            bNode *coverage_base_node = nullptr;
            bNodeSocket *coverage_base_socket = nullptr;
            const bool folder = BKE_paint_layers_is_folder(*layer);
            if (folder) {
              coverage_base_node = folder_coverage_node;
              coverage_base_socket = folder_coverage;
            }
            else {
              /* A leaf with no map alpha covers fully, so the update is the identity and nothing
               * is built; the mask is not part of this base, it multiplies the result below. */
              coverage_base_node = content_cov_node;
              coverage_base_socket = content_cov;
            }
            if (coverage_base_node != nullptr && coverage_base_socket != nullptr) {
              bNode *one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (one_minus != nullptr && scaled != nullptr && joined != nullptr) {
                one_minus->custom1 = NODE_MATH_SUBTRACT;
                scaled->custom1 = NODE_MATH_MULTIPLY;
                joined->custom1 = NODE_MATH_ADD;
                one_minus->location[0] = location_x + 150.0f;
                one_minus->location[1] = location_y - 420.0f;
                scaled->location[0] = location_x + 230.0f;
                scaled->location[1] = location_y - 420.0f;
                joined->location[0] = location_x + 310.0f;
                joined->location[1] = location_y - 420.0f;
                bNodeSocket *om_a = socket_in(*one_minus, "Value");
                bNodeSocket *om_b = socket_in(*one_minus, "Value_001");
                bNodeSocket *sc_a = socket_in(*scaled, "Value");
                bNodeSocket *sc_b = socket_in(*scaled, "Value_001");
                bNodeSocket *jo_a = socket_in(*joined, "Value");
                bNodeSocket *jo_b = socket_in(*joined, "Value_001");
                if (om_a != nullptr && om_b != nullptr && sc_a != nullptr && sc_b != nullptr &&
                    jo_a != nullptr && jo_b != nullptr)
                {
                  if (om_a->default_value != nullptr) {
                    static_cast<bNodeSocketValueFloat *>(om_a->default_value)->value = 1.0f;
                  }
                  bke::node_add_link(
                      tree, *coverage_base_node, *coverage_base_socket, *one_minus, *om_b);
                  bke::node_add_link(
                      tree, *correction_coverage_node, *correction_coverage_socket, *scaled, *sc_a);
                  bke::node_add_link(tree,
                                     *one_minus,
                                     *socket_out(*one_minus, "Value"),
                                     *scaled,
                                     *sc_b);
                  bke::node_add_link(
                      tree, *coverage_base_node, *coverage_base_socket, *joined, *jo_a);
                  bke::node_add_link(
                      tree, *scaled, *socket_out(*scaled, "Value"), *joined, *jo_b);
                  if (folder) {
                    folder_coverage_node = joined;
                    folder_coverage = socket_out(*joined, "Value");
                  }
                  else {
                    content_cov_node = joined;
                    content_cov = socket_out(*joined, "Value");
                  }
                }
              }
            }
          }
          /* The content alpha follows the colour that was just corrected: a content correction
           * raises the row's alpha by the over model, `a = a + fac * (1 - a)`, exactly as the CPU
           * folds it while it blends the correction in (#composite_layer_render). The fac is the
           * same `opacity * A` the coverage update above uses. A row that tracks no content alpha
           * (Material, Normal, a channel outside the image-paint set) builds nothing. */
          if (track_content_alpha && current.content_alpha != nullptr &&
              current.content_alpha_node != nullptr &&
              correction_coverage_socket != nullptr && correction_coverage_node != nullptr)
          {
            bNode *alpha_one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNode *alpha_scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNode *alpha_joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            if (alpha_one_minus != nullptr && alpha_scaled != nullptr && alpha_joined != nullptr) {
              bNodeSocket *aom_a = socket_in(*alpha_one_minus, "Value");
              bNodeSocket *aom_b = socket_in(*alpha_one_minus, "Value_001");
              bNodeSocket *asc_a = socket_in(*alpha_scaled, "Value");
              bNodeSocket *asc_b = socket_in(*alpha_scaled, "Value_001");
              bNodeSocket *ajo_a = socket_in(*alpha_joined, "Value");
              bNodeSocket *ajo_b = socket_in(*alpha_joined, "Value_001");
              if (aom_a != nullptr && aom_b != nullptr && asc_a != nullptr && asc_b != nullptr &&
                  ajo_a != nullptr && ajo_b != nullptr)
              {
                alpha_one_minus->custom1 = NODE_MATH_SUBTRACT;
                alpha_scaled->custom1 = NODE_MATH_MULTIPLY;
                alpha_joined->custom1 = NODE_MATH_ADD;
                alpha_one_minus->location[0] = location_x + 150.0f;
                alpha_one_minus->location[1] = location_y - 480.0f;
                alpha_scaled->location[0] = location_x + 230.0f;
                alpha_scaled->location[1] = location_y - 480.0f;
                alpha_joined->location[0] = location_x + 310.0f;
                alpha_joined->location[1] = location_y - 480.0f;
                if (aom_a->default_value != nullptr) {
                  static_cast<bNodeSocketValueFloat *>(aom_a->default_value)->value = 1.0f;
                }
                bke::node_add_link(tree,
                                   *current.content_alpha_node,
                                   *current.content_alpha,
                                   *alpha_one_minus,
                                   *aom_b);
                bke::node_add_link(tree,
                                   *correction_coverage_node,
                                   *correction_coverage_socket,
                                   *alpha_scaled,
                                   *asc_a);
                bke::node_add_link(tree,
                                   *alpha_one_minus,
                                   *socket_out(*alpha_one_minus, "Value"),
                                   *alpha_scaled,
                                   *asc_b);
                bke::node_add_link(tree,
                                   *current.content_alpha_node,
                                   *current.content_alpha,
                                   *alpha_joined,
                                   *ajo_a);
                bke::node_add_link(tree,
                                   *alpha_scaled,
                                   *socket_out(*alpha_scaled, "Value"),
                                   *alpha_joined,
                                   *ajo_b);
                current.content_alpha_node = alpha_joined;
                current.content_alpha = socket_out(*alpha_joined, "Value");
              }
            }
          }
          current.source_node = correction_mix;
          current.source = mix_out;
        }
      }

      /* The layer factor is the mask times the content coverage; either alone when the other is
       * absent. */
      if (content_cov != nullptr && content_cov_node != nullptr) {
        if (layer_factor_socket == nullptr) {
          layer_factor_node = content_cov_node;
          layer_factor_socket = content_cov;
        }
        else {
          bNode *product = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          bNodeSocket *p_a = (product != nullptr) ? socket_in(*product, "Value") : nullptr;
          bNodeSocket *p_b = (product != nullptr) ? socket_in(*product, "Value_001") : nullptr;
          bNodeSocket *p_out = (product != nullptr) ? socket_out(*product, "Value") : nullptr;
          if (p_a != nullptr && p_b != nullptr && p_out != nullptr) {
            product->custom1 = NODE_MATH_MULTIPLY;
            product->location[0] = location_x + 350.0f;
            product->location[1] = location_y - 420.0f;
            bke::node_add_link(tree, *layer_factor_node, *layer_factor_socket, *product, *p_a);
            bke::node_add_link(tree, *content_cov_node, *content_cov, *product, *p_b);
            layer_factor_node = product;
            layer_factor_socket = p_out;
          }
        }
      }

      /* Mask items are a coverage stack over the row factor: each item lays its own coverage
       * `F = F_below * (1 - A * op) + C * op`, where `C` is the mean of the map's raw Color (a
       * Non-Color map reads un-premultiplied, so this is the pre-multiplied color) and `A` its
       * Alpha, and `op` the row's own opacity. MULTIPLY lays `F * C` instead. Later list entries
       * lay over earlier ones. Every other blend mode reads as MIX; the opacity is row-level, not
       * per channel. */
      if (current.opacity != nullptr)
      {
        const bool has_mask_corrections = !BKE_paint_layers_mask_items(*layer).is_empty();
        if (has_mask_corrections) {
          ensure_factor_base();
          bNode *factor_node = layer_factor_node;
          bNodeSocket *factor_socket = layer_factor_socket;
          for (const MaterialPaintLayer *mask_item : BKE_paint_layers_mask_items(*layer)) {
            const MaterialPaintLayer &correction = *mask_item;
            const bool fill = BKE_paint_layers_source_type(correction) ==
                              PaintLayerSourceType::Constant;
            const bool mask_material_or_group = ELEM(correction.source,
                                                      MA_PAINT_LAYER_SOURCE_MATERIAL,
                                                      MA_PAINT_LAYER_SOURCE_NODE_GROUP,
                                                      MA_PAINT_LAYER_SOURCE_STACK);
            /* A Normal mask channel has no visual meaning to reduce to one number (it is a
             * tangent-space direction, not a scalar or a colour); the item is quietly skipped,
             * exactly like a Node Group correction with no bake yet. */
            if (mask_material_or_group &&
                correction.mask_channel == PAINT_MATERIAL_CHANNEL_NORMAL)
            {
              continue;
            }
            bNodeSocket *correction_opacity = nullptr;
            if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
                    correction_opacity_inputs.lookup_ptr(&correction))
            {
              if (bNodeTreeInterfaceSocket **opacity_iface =
                      opacity_by_channel->lookup_ptr(channel))
              {
                correction_opacity = group_input_socket(**opacity_iface);
              }
            }
            /* The raw Color whose mean is the correction's color `C`: the Fill constant or the
             * map's Color output read as-is. */
            bNode *gray_source_node = nullptr;
            bNodeSocket *gray_source_color = nullptr;
            bNode *correction_map = nullptr;
            bNodeSocket *correction_alpha = nullptr;
            /* Whether the map is read as colour data. The Image Texture node only un-premultiplies
             * a premultiplied texture when its colorspace is not data (node_shader_tex_image.cc),
             * so only a data map still needs the chain's own Divide. */
            bool mask_map_is_data = false;
            bool mask_map_is_mesh = false;
            /* A Material/Node Group mask's own coverage (its source's Alpha), independent of the
             * grey it lays over the factor -- reused as-is when `mask_channel` is Alpha (the grey
             * itself is this number) or as the item's Fac multiplier otherwise. */
            bNode *correction_coverage_node = nullptr;
            bNodeSocket *correction_coverage_socket = nullptr;
            if (fill) {
              if (bNodeTreeInterfaceSocket **fill_iface =
                      correction_fill_inputs.lookup_ptr(&correction))
              {
                gray_source_color = group_input_socket(**fill_iface);
                gray_source_node = group_input;
              }
              if (gray_source_color == nullptr) {
                continue;
              }
            }
            else if (mask_material_or_group) {
              const bool mask_alpha_channel = correction.mask_channel ==
                                              PAINT_MATERIAL_CHANNEL_ALPHA;
              if (correction.source == MA_PAINT_LAYER_SOURCE_STACK) {
                /* A Stack mask item reads its own children's accumulated result, exactly like a
                 * Stack content correction and a Layer folder's own children (#build_list,
                 * mirrors ~1957-1993 and the content correction's own Stack branch above): straight
                 * colour + coverage, starting from transparent. The subtree's coverage is this
                 * item's Fac multiplier on every channel (below) and, on the Alpha channel, the
                 * grey itself -- there is no separate content map to read.
                 *
                 * The subtree builds in a fixed channel, never in the owner's current `channel`: on
                 * the Alpha convention it always reads Base Color (exactly like every other Paint
                 * mask -- #paint_layer_mask_correction_image always reads Base Color too), and
                 * otherwise it reads `mask_channel` itself. Both are constant across every channel
                 * the owner row generates, or the same mask would answer differently depending on
                 * which of the owner's own channels happens to be built at the time -- the bug
                 * #stack_mask_fixed_channel_applies_to_every_owner_channel_matches_the_cpu and
                 * #stack_mask_alpha_reads_base_color_on_every_owner_channel_matches_the_cpu regress. */
                const int mask_build_channel = mask_alpha_channel ?
                                                   int(PAINT_MATERIAL_CHANNEL_BASE_COLOR) :
                                                   int(correction.mask_channel);
                bNode *p_zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
                bNodeSocket *p_zero_out = (p_zero != nullptr) ? socket_out(*p_zero, "Color") :
                                                                nullptr;
                if (p_zero_out == nullptr) {
                  continue;
                }
                if (p_zero_out->default_value != nullptr) {
                  copy_v4_fl(
                      static_cast<bNodeSocketValueRGBA *>(p_zero_out->default_value)->value, 0.0f);
                }
                ChainLayer sub_previous;
                sub_previous.source_node = p_zero;
                sub_previous.source = p_zero_out;
                ChainResult sub = build_list(
                    correction.children, sub_previous, true, target, mask_build_channel);
                if (sub.chain.source == nullptr || sub.coverage == nullptr) {
                  continue;
                }
                bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
                bNodeSocket *div_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
                bNodeSocket *div_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") :
                                                           nullptr;
                bNodeSocket *div_out = (divide != nullptr) ? socket_out(*divide, "Vector") :
                                                             nullptr;
                if (div_out == nullptr) {
                  continue;
                }
                divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
                divide->location[0] = location_x - 220.0f;
                divide->location[1] = location_y - 320.0f;
                bke::node_add_link(
                    tree, *sub.chain.source_node, *sub.chain.source, *divide, *div_a);
                bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *divide, *div_b);
                correction_coverage_node = sub.coverage_node;
                correction_coverage_socket = sub.coverage;
                if (!mask_alpha_channel) {
                  const bool mask_is_color = BKE_paint_material_channel_info(
                                                  eMaterialPaintChannel(correction.mask_channel))
                                                  .is_color;
                  if (mask_is_color) {
                    bNode *rgbtobw = bke::node_add_static_node(nullptr, tree, SH_NODE_RGBTOBW);
                    bNodeSocket *bw_in = (rgbtobw != nullptr) ? socket_in(*rgbtobw, "Color") :
                                                                nullptr;
                    bNodeSocket *bw_out = (rgbtobw != nullptr) ? socket_out(*rgbtobw, "Val") :
                                                                 nullptr;
                    if (bw_in == nullptr || bw_out == nullptr) {
                      continue;
                    }
                    rgbtobw->location[0] = location_x - 140.0f;
                    rgbtobw->location[1] = location_y - 320.0f;
                    bke::node_add_link(tree, *divide, *div_out, *rgbtobw, *bw_in);
                    gray_source_node = rgbtobw;
                    gray_source_color = bw_out;
                  }
                  else {
                    bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
                    bNodeSocket *sep_vector = (separate != nullptr) ?
                                                  socket_in(*separate, "Vector") :
                                                  nullptr;
                    bNodeSocket *sep_x = (separate != nullptr) ? socket_out(*separate, "X") :
                                                                 nullptr;
                    if (sep_vector == nullptr || sep_x == nullptr) {
                      continue;
                    }
                    bke::node_add_link(tree, *divide, *div_out, *separate, *sep_vector);
                    gray_source_node = separate;
                    gray_source_color = sep_x;
                  }
                }
              }
              else if (mask_alpha_channel) {
                /* The channel itself is the source's coverage: no separate grey/coverage split. */
                std::tie(correction_coverage_node, correction_coverage_socket) =
                    resolve_correction_coverage(correction);
                if (correction_coverage_node == nullptr || correction_coverage_socket == nullptr) {
                  continue;
                }
              }
              else {
                const bool mask_is_color = BKE_paint_material_channel_info(
                                                eMaterialPaintChannel(correction.mask_channel))
                                                .is_color;
                bNode *value_node = nullptr;
                bNodeSocket *value_socket = nullptr;
                if (correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
                  const RowMaterialSource row_source = resolve_row_material_source(
                      correction, correction.mask_channel, tree, false);
                  if (row_source.live_constant) {
                    if (Map<int, bNodeTreeInterfaceSocket *> *live_by_channel =
                            correction_live_constant_inputs.lookup_ptr(&correction))
                    {
                      if (bNodeTreeInterfaceSocket **live_iface =
                              live_by_channel->lookup_ptr(correction.mask_channel))
                      {
                        value_socket = group_input_socket(**live_iface);
                        value_node = group_input;
                      }
                    }
                  }
                  else if (row_source.live_map) {
                    bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
                    if (map != nullptr) {
                      map->id = &row_source.live_map_image->id;
                      id_us_plus(&row_source.live_map_image->id);
                      map->location[0] = location_x - 220.0f;
                      map->location[1] = location_y - 320.0f;
                      if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
                        if (row_source.live_map_iuser != nullptr) {
                          dst->iuser = *row_source.live_map_iuser;
                        }
                      }
                      value_socket = socket_out(*map, "Color");
                      value_node = map;
                    }
                  }
                  else if (row_source.source_group_instance != nullptr &&
                           row_source.source_group_socket != nullptr)
                  {
                    value_socket = row_source.source_group_socket;
                    value_node = row_source.source_group_instance;
                  }
                  else {
                    Image *mask_image = paint_layer_channel_image(
                        ma, correction, correction.mask_channel);
                    if (mask_image != nullptr) {
                      bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
                      if (map != nullptr) {
                        map->id = &mask_image->id;
                        id_us_plus(&mask_image->id);
                        map->location[0] = location_x - 220.0f;
                        map->location[1] = location_y - 320.0f;
                        value_socket = socket_out(*map, "Color");
                        value_node = map;
                      }
                    }
                  }
                }
                else {
                  /* Node Group: Baked-only, like the content correction of the same source. */
                  Image *mask_baked = nullptr;
                  if (BKE_paint_layers_bake_substitute(
                          ma, correction, correction.mask_channel, &mask_baked) ||
                      BKE_paint_layers_bake_substitute_custom(
                          ma, correction, correction.mask_channel, &mask_baked, nullptr))
                  {
                    if (mask_baked != nullptr) {
                      bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
                      if (map != nullptr) {
                        map->id = &mask_baked->id;
                        id_us_plus(&mask_baked->id);
                        map->location[0] = location_x - 220.0f;
                        map->location[1] = location_y - 320.0f;
                        value_socket = socket_out(*map, "Color");
                        value_node = map;
                      }
                    }
                  }
                }
                if (value_socket == nullptr || value_node == nullptr) {
                  continue;
                }
                if (mask_is_color) {
                  bNode *rgbtobw = bke::node_add_static_node(nullptr, tree, SH_NODE_RGBTOBW);
                  bNodeSocket *bw_in = (rgbtobw != nullptr) ? socket_in(*rgbtobw, "Color") :
                                                              nullptr;
                  bNodeSocket *bw_out = (rgbtobw != nullptr) ? socket_out(*rgbtobw, "Val") :
                                                               nullptr;
                  if (bw_in == nullptr || bw_out == nullptr) {
                    continue;
                  }
                  rgbtobw->location[0] = location_x - 140.0f;
                  rgbtobw->location[1] = location_y - 320.0f;
                  bke::node_add_link(tree, *value_node, *value_socket, *rgbtobw, *bw_in);
                  gray_source_node = rgbtobw;
                  gray_source_color = bw_out;
                }
                else {
                  /* A scalar channel's map or constant stores its value spread across R=G=B
                   * (#socket_default_to_constant's SOCK_FLOAT case, and every scalar bake/AOV);
                   * Separate X reads it directly, exactly like a MESH_MAP scalar atlas. */
                  bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
                  bNodeSocket *sep_vector = (separate != nullptr) ? socket_in(*separate, "Vector") :
                                                                    nullptr;
                  bNodeSocket *sep_x = (separate != nullptr) ? socket_out(*separate, "X") : nullptr;
                  if (sep_vector == nullptr || sep_x == nullptr) {
                    continue;
                  }
                  bke::node_add_link(tree, *value_node, *value_socket, *separate, *sep_vector);
                  gray_source_node = separate;
                  gray_source_color = sep_x;
                }
                /* This item's own Fac multiplier: the source's coverage, resolved once more (its
                 * own Alpha channel, independent of `mask_channel`). */
                std::tie(correction_coverage_node, correction_coverage_socket) =
                    resolve_correction_coverage(correction);
              }
            }
            else {
              Image *correction_image = paint_layer_mask_correction_image(
                  ma, correction, channel);
              if (correction_image == nullptr) {
                continue;
              }
              mask_map_is_data = IMB_colormanagement_space_name_is_data(
                  correction_image->colorspace_settings.name);
              correction_map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
              if (correction_map == nullptr) {
                continue;
              }
              correction_map->id = &correction_image->id;
              id_us_plus(&correction_image->id);
              correction_map->location[0] = location_x - 220.0f;
              correction_map->location[1] = location_y - 320.0f;
              /* A MESH_MAP mask item reads the atlas R, the same value the CPU reads, and is
               * sampled Extend like every other atlas read. */
              mask_map_is_mesh = correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP;
              if (mask_map_is_mesh) {
                if (NodeTexImage *storage = static_cast<NodeTexImage *>(correction_map->storage)) {
                  storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
                }
              }
              correction_alpha = socket_out(*correction_map, "Alpha");
              gray_source_color = socket_out(*correction_map, "Color");
              gray_source_node = correction_map;
            }
            bNode *correction_gray_node = nullptr;
            bNodeSocket *correction_gray = nullptr;
            if (mask_material_or_group) {
              /* Already reduced to one number above: the coverage itself (Alpha channel) or the
               * RGBTOBW/Separate-X result (colour/scalar channel) -- no further mean/Divide here. */
              if (correction.mask_channel == PAINT_MATERIAL_CHANNEL_ALPHA) {
                correction_gray_node = correction_coverage_node;
                correction_gray = correction_coverage_socket;
              }
              else {
                correction_gray_node = gray_source_node;
                correction_gray = gray_source_color;
              }
            }
            else if (mask_map_is_mesh) {
              /* A scalar atlas: its R is the coverage directly; no mean, no Divide (the atlas is
               * Non-Color and its alpha is ignored). */
              bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
              if (separate == nullptr) {
                continue;
              }
              bNodeSocket *sep_x = socket_out(*separate, "X");
              bNodeSocket *sep_vector = socket_in(*separate, "Vector");
              if (sep_x == nullptr || sep_vector == nullptr) {
                continue;
              }
              bke::node_add_link(
                  tree, *gray_source_node, *gray_source_color, *separate, *sep_vector);
              correction_gray_node = separate;
              correction_gray = sep_x;
            }
            else {
              bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
              bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (separate == nullptr || add_xy == nullptr || add_z == nullptr ||
                  divide == nullptr)
              {
                continue;
              }
              bNodeSocket *sep_x = socket_out(*separate, "X");
              bNodeSocket *sep_y = socket_out(*separate, "Y");
              bNodeSocket *sep_z = socket_out(*separate, "Z");
              bNodeSocket *sep_vector = socket_in(*separate, "Vector");
              add_xy->custom1 = NODE_MATH_ADD;
              add_z->custom1 = NODE_MATH_ADD;
              divide->custom1 = NODE_MATH_DIVIDE;
              bNodeSocket *xy_a = socket_in(*add_xy, "Value");
              bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
              bNodeSocket *z_a = socket_in(*add_z, "Value");
              bNodeSocket *z_b = socket_in(*add_z, "Value_001");
              bNodeSocket *d_a = socket_in(*divide, "Value");
              bNodeSocket *d_b = socket_in(*divide, "Value_001");
              if (sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
                  sep_vector == nullptr || xy_a == nullptr || xy_b == nullptr ||
                  z_a == nullptr || z_b == nullptr || d_a == nullptr || d_b == nullptr)
              {
                continue;
              }
              bke::node_add_link(
                  tree, *gray_source_node, *gray_source_color, *separate, *sep_vector);
              bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
              bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
              bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
              bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
              bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
              if (d_b->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
              }
              correction_gray_node = divide;
              correction_gray = socket_out(*divide, "Value");
            }
            bNode *correction_mix = mix_node_add(
                tree, MA_RAMP_BLEND, location_x + 60.0f, location_y - 320.0f);
            if (correction_mix == nullptr || correction_gray == nullptr ||
                correction_gray_node == nullptr || factor_socket == nullptr)
            {
              continue;
            }
            bNodeSocket *mix_color1 = socket_in(*correction_mix, "A_Color");
            bNodeSocket *mix_color2 = socket_in(*correction_mix, "B_Color");
            bNodeSocket *mix_fac = socket_in(*correction_mix, "Factor_Float");
            bNodeSocket *mix_out = socket_out(*correction_mix, "Result_Color");
            if (mix_color1 == nullptr || mix_color2 == nullptr || mix_fac == nullptr ||
                mix_out == nullptr)
            {
              continue;
            }
            bke::node_add_link(tree, *factor_node, *factor_socket, *correction_mix, *mix_color1);
            /* B is what the factor mixes towards: the Fill constant or the map's straightened
             * grey. MULTIPLY mixes towards `F * C` instead, so the item darkens the factor by its
             * grey rather than replacing it with the grey; every other mode reads as MIX. */
            bNode *b_node = correction_gray_node;
            bNodeSocket *b_socket = correction_gray;
            /* A Material/Node Group mask never premultiplies: its grey is either the source's own
             * coverage (Alpha channel) or a value/RGBTOBW read straight from a live constant,
             * live texture or bake, none of which the texture-upload premultiply touches -- only a
             * user-painted Image/MeshMap map needs the straighten-by-A step below. */
            if (!fill && !mask_material_or_group) {
              if (correction_alpha == nullptr || correction_map == nullptr) {
                continue;
              }
              if (mask_map_is_data && !mask_map_is_mesh) {
                /* The map is stored straight, but the texture upload pre-multiplied its bytes by A,
                 * so the sampled grey already carries A once; mixing it by `A * op` would apply A
                 * twice. The Image Texture node leaves a data texture pre-multiplied, so the chain
                 * straightens it: `mix(F, C / A, A * op)` is `F * (1 - A * op) + C * A * op`. Where
                 * A is 0 the factor is 0 too, and Math Divide yields 0 there. */
                bNode *straighten = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
                if (straighten == nullptr) {
                  continue;
                }
                straighten->custom1 = NODE_MATH_DIVIDE;
                straighten->location[0] = location_x - 20.0f;
                straighten->location[1] = location_y - 380.0f;
                bNodeSocket *straighten_value = socket_in(*straighten, "Value");
                bNodeSocket *straighten_alpha = socket_in(*straighten, "Value_001");
                bNodeSocket *straighten_out = socket_out(*straighten, "Value");
                if (straighten_value == nullptr || straighten_alpha == nullptr ||
                    straighten_out == nullptr)
                {
                  continue;
                }
                bke::node_add_link(
                    tree, *correction_gray_node, *correction_gray, *straighten, *straighten_value);
                bke::node_add_link(
                    tree, *correction_map, *correction_alpha, *straighten, *straighten_alpha);
                b_node = straighten;
                b_socket = straighten_out;
              }
              /* A non-data map is already straight: the Image Texture node un-premultiplied it,
               * because the chain also reads the Alpha output, so no Divide is built. */
            }
            if (correction.blend == MA_PAINT_LAYER_BLEND_MULTIPLY) {
              bNode *multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (multiply == nullptr) {
                continue;
              }
              multiply->custom1 = NODE_MATH_MULTIPLY;
              multiply->location[0] = location_x;
              multiply->location[1] = location_y - 420.0f;
              bNodeSocket *mul_a = socket_in(*multiply, "Value");
              bNodeSocket *mul_b = socket_in(*multiply, "Value_001");
              bNodeSocket *mul_out = socket_out(*multiply, "Value");
              if (mul_a == nullptr || mul_b == nullptr || mul_out == nullptr) {
                continue;
              }
              bke::node_add_link(tree, *factor_node, *factor_socket, *multiply, *mul_a);
              bke::node_add_link(tree, *b_node, *b_socket, *multiply, *mul_b);
              b_node = multiply;
              b_socket = mul_out;
            }
            bke::node_add_link(tree, *b_node, *b_socket, *correction_mix, *mix_color2);
            /* Fac is `A * op`: the map alpha times the correction row's own opacity input. The
             * per-channel sockets all carry the same row value, so the current channel's is
             * enough. A Fill covers fully, so its factor is the opacity alone. */
            if (correction_opacity == nullptr || group_input == nullptr) {
              continue;
            }
            /* A Material/Node Group mask on its own Alpha channel is the coverage itself, so its
             * Fac is the opacity alone (the coordinator's decision: squaring the same number as
             * both the grey and its own multiplier would be wrong) -- flat, exactly like Fill and
             * a MESH_MAP atlas. On every other channel its Fac multiplies by the source's own
             * coverage (`correction_coverage_*`, resolved above), never a texture's own alpha. */
            const bool mask_alpha_flat = mask_material_or_group &&
                                         correction.mask_channel == PAINT_MATERIAL_CHANNEL_ALPHA;
            if (fill || mask_map_is_mesh || mask_alpha_flat) {
              /* A Fill covers fully; a MESH_MAP atlas' alpha is ignored (spec M2 §1), so its factor
               * is the item's opacity alone and no Alpha multiply is built. */
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
            }
            else if (mask_material_or_group) {
              if (correction_coverage_node == nullptr || correction_coverage_socket == nullptr) {
                /* No coverage of its own yet (Baked with no bake coverage, say): flat, exactly
                 * like the row-level factor falls back to full coverage in the same state. */
                bke::node_add_link(
                    tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
              }
              else {
                bNode *fac_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
                if (fac_multiply == nullptr) {
                  continue;
                }
                fac_multiply->custom1 = NODE_MATH_MULTIPLY;
                bNodeSocket *fac_value = socket_in(*fac_multiply, "Value");
                bNodeSocket *fac_coverage = socket_in(*fac_multiply, "Value_001");
                bNodeSocket *fac_out = socket_out(*fac_multiply, "Value");
                if (fac_value == nullptr || fac_coverage == nullptr || fac_out == nullptr) {
                  continue;
                }
                bke::node_add_link(
                    tree, *group_input, *correction_opacity, *fac_multiply, *fac_value);
                bke::node_add_link(tree,
                                   *correction_coverage_node,
                                   *correction_coverage_socket,
                                   *fac_multiply,
                                   *fac_coverage);
                bke::node_add_link(tree, *fac_multiply, *fac_out, *correction_mix, *mix_fac);
              }
            }
            else {
              if (correction_alpha == nullptr || correction_map == nullptr) {
                continue;
              }
              bNode *fac_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (fac_multiply == nullptr) {
                continue;
              }
              fac_multiply->custom1 = NODE_MATH_MULTIPLY;
              bNodeSocket *fac_value = socket_in(*fac_multiply, "Value");
              bNodeSocket *fac_coverage = socket_in(*fac_multiply, "Value_001");
              bNodeSocket *fac_out = socket_out(*fac_multiply, "Value");
              if (fac_value == nullptr || fac_coverage == nullptr || fac_out == nullptr) {
                continue;
              }
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *fac_multiply, *fac_value);
              bke::node_add_link(
                  tree, *correction_map, *correction_alpha, *fac_multiply, *fac_coverage);
              bke::node_add_link(tree, *fac_multiply, *fac_out, *correction_mix, *mix_fac);
            }
            layer_factor_node = correction_mix;
            layer_factor_socket = mix_out;
            /* The next item lays over this one: its base is this item's result. */
            factor_node = correction_mix;
            factor_socket = mix_out;
          }
        }
      }

      /* The factor is the correction/mask chain times the row's own opacity, or the opacity alone
       * when there is neither a mask map nor a correction. */
      if (layer_factor_socket != nullptr && current.opacity != nullptr &&
          current.opacity_node != nullptr)
      {
        bNode *opacity_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (opacity_multiply != nullptr) {
          opacity_multiply->custom1 = NODE_MATH_MULTIPLY;
          opacity_multiply->location[0] = location_x + 200.0f;
          opacity_multiply->location[1] = location_y - 320.0f;
          bNodeSocket *value_a = socket_in(*opacity_multiply, "Value");
          bNodeSocket *value_b = socket_in(*opacity_multiply, "Value_001");
          bNodeSocket *value_out = socket_out(*opacity_multiply, "Value");
          if (value_a != nullptr && value_b != nullptr && value_out != nullptr) {
            bke::node_add_link(
                tree, *current.opacity_node, *current.opacity, *opacity_multiply, *value_a);
            bke::node_add_link(
                tree, *layer_factor_node, *layer_factor_socket, *opacity_multiply, *value_b);
            current.factor_node = opacity_multiply;
            current.factor = value_out;
          }
        }
      }
      else {
        current.factor_node = current.opacity_node;
        current.factor = current.opacity;
      }

      /* A folder's row blends by its own factor times the coverage its contents accumulated. */
      if (folder_coverage_node != nullptr && folder_coverage != nullptr) {
        bNode *coverage_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (coverage_multiply != nullptr) {
          coverage_multiply->custom1 = NODE_MATH_MULTIPLY;
          coverage_multiply->location[0] = location_x + 150.0f;
          coverage_multiply->location[1] = location_y - 400.0f;
          bNodeSocket *cv_a = socket_in(*coverage_multiply, "Value");
          bNodeSocket *cv_b = socket_in(*coverage_multiply, "Value_001");
          bNodeSocket *cv_out = socket_out(*coverage_multiply, "Value");
          if (cv_a != nullptr && cv_b != nullptr && cv_out != nullptr) {
            if (current.factor != nullptr && current.factor_node != nullptr) {
              bke::node_add_link(
                  tree, *current.factor_node, *current.factor, *coverage_multiply, *cv_a);
            }
            else if (cv_a->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(cv_a->default_value)->value = 1.0f;
            }
            bke::node_add_link(
                tree, *folder_coverage_node, *folder_coverage, *coverage_multiply, *cv_b);
            current.factor_node = coverage_multiply;
            current.factor = cv_out;
          }
        }
        }

        RowResult result;

        /* A leaf in its own group: expose the row's colour, coverage and the two blends the parent
         * may chain through. Below is the group's blend base; Color and Coverage are its straight
         * result; Blend is the row blended at factor one (only the parent's premul chain uses it);
         * Result is the row laid over Below. */
        if (layer_group != nullptr && current.source != nullptr) {
          const MaterialPaintChannelInfo &channel_info = BKE_paint_material_channel_info(
              eMaterialPaintChannel(channel));
          auto add_group_socket = [&](const char *kind,
                                      const StringRef type,
                                      const NodeTreeInterfaceSocketFlag flag)
              -> bNodeTreeInterfaceSocket * {
            char base[192];
            SNPRINTF(base, "%s %s", kind, channel_info.ui_name);
            return layer_group_add_socket(*layer_group, base, type, flag);
          };
          bNodeTreeInterfaceSocket *below_iface = add_group_socket(
              "Below", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT);
          bNodeTreeInterfaceSocket *color_iface = add_group_socket(
              "Color", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
          bNodeTreeInterfaceSocket *coverage_iface = add_group_socket(
              "Coverage", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT);
          bNodeTreeInterfaceSocket *blend_iface = add_group_socket(
              "Blend", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
          bNodeTreeInterfaceSocket *result_iface = add_group_socket(
              "Result", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
          /* A private scalar output for the content alpha. Only when this row tracks one (a Base
           * Color Paint leaf or a folder with tracked content); absent otherwise, so groups for
           * untracked rows keep the exact contract they had. */
          bNodeTreeInterfaceSocket *content_alpha_iface = nullptr;
          if (current.content_alpha != nullptr) {
            content_alpha_iface = add_group_socket(
                "Content Alpha", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT);
            if (content_alpha_iface == nullptr) {
              return {};
            }
          }
          if (below_iface == nullptr || color_iface == nullptr || coverage_iface == nullptr ||
              blend_iface == nullptr || result_iface == nullptr)
          {
            return {};
          }
          bNodeSocket *below = bke::node_find_socket(
              *group_input, SOCK_OUT, UString::from_ptr_noinline(below_iface->identifier));
          bNodeSocket *color_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(color_iface->identifier));
          bNodeSocket *coverage_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(coverage_iface->identifier));
          bNodeSocket *blend_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(blend_iface->identifier));
          bNodeSocket *result_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(result_iface->identifier));
          bNodeSocket *content_alpha_out = nullptr;
          if (content_alpha_iface != nullptr) {
            content_alpha_out = bke::node_find_socket(
                *layer_group->group_output,
                SOCK_IN,
                UString::from_ptr_noinline(content_alpha_iface->identifier));
          }
          if (below == nullptr || color_out == nullptr || coverage_out == nullptr ||
              blend_out == nullptr || result_out == nullptr ||
              (content_alpha_iface != nullptr && content_alpha_out == nullptr))
          {
            return {};
          }
          bke::node_add_link(tree, *current.source_node, *current.source,
                             *layer_group->group_output, *color_out);
          if (current.factor != nullptr && current.factor_node != nullptr) {
            bke::node_add_link(tree, *current.factor_node, *current.factor,
                               *layer_group->group_output, *coverage_out);
          }
          else if (coverage_out->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(coverage_out->default_value)->value = 1.0f;
          }
          if (content_alpha_out != nullptr) {
            bke::node_add_link(tree,
                               *current.content_alpha_node,
                               *current.content_alpha,
                               *layer_group->group_output,
                               *content_alpha_out);
          }

          /* blend(Below, Color) at factor one; the parent feeds Below from its own chain. */
          auto add_row_blend = [&](bNodeSocket *factor,
                                   const float factor_default)
              -> std::pair<bNode *, bNodeSocket *> {
            if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
                tree.typeinfo != nullptr && tree.typeinfo->group_idname != nullptr)
            {
              bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
              if (combine == nullptr) {
                return {nullptr, nullptr};
              }
              combine->id = &ctx.normal_combine_group->id;
              id_us_plus(&ctx.normal_combine_group->id);
              combine->location[0] = location_x;
              combine->location[1] = location_y;
              nodes::update_node_declaration_and_sockets(tree, *combine);
              bNodeSocket *a_in = bke::node_find_socket(
                  *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
              bNodeSocket *b_in = bke::node_find_socket(
                  *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
              bNodeSocket *f_in = bke::node_find_socket(
                  *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
              bNodeSocket *r_out = bke::node_find_socket(
                  *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
              if (a_in == nullptr || b_in == nullptr || f_in == nullptr || r_out == nullptr) {
                return {nullptr, nullptr};
              }
              bke::node_add_link(tree, *group_input, *below, *combine, *a_in);
              bke::node_add_link(
                  tree, *current.source_node, *current.source, *combine, *b_in);
              if (factor != nullptr) {
                bke::node_add_link(tree, *current.factor_node, *factor, *combine, *f_in);
              }
              else if (f_in->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = factor_default;
              }
              return {combine, r_out};
            }
            bNode *mix = mix_node_add(tree,
                                      BKE_paint_layers_blend_to_ramp(
                                          eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*layer, channel))),
                                      location_x,
                                      location_y);
            if (mix == nullptr) {
              return {nullptr, nullptr};
            }
            bNodeSocket *a_in = socket_in(*mix, "A_Color");
            bNodeSocket *b_in = socket_in(*mix, "B_Color");
            bNodeSocket *f_in = socket_in(*mix, "Factor_Float");
            bNodeSocket *r_out = socket_out(*mix, "Result_Color");
            if (a_in == nullptr || b_in == nullptr || f_in == nullptr || r_out == nullptr) {
              return {nullptr, nullptr};
            }
            bke::node_add_link(tree, *group_input, *below, *mix, *a_in);
            bke::node_add_link(tree, *current.source_node, *current.source, *mix, *b_in);
            if (factor != nullptr) {
              bke::node_add_link(tree, *current.factor_node, *factor, *mix, *f_in);
            }
            else if (f_in->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = factor_default;
            }
            return {mix, r_out};
          };

          auto [result_node, result_source] = add_row_blend(current.factor, 1.0f);
          if (result_source == nullptr) {
            return {};
          }
          bke::node_add_link(
              tree, *result_node, *result_source, *layer_group->group_output, *result_out);
          if (premul) {
            auto [blend_node, blend_source] = add_row_blend(nullptr, 1.0f);
            if (blend_source != nullptr) {
              bke::node_add_link(
                  tree, *blend_node, *blend_source, *layer_group->group_output, *blend_out);
            }
          }

          result.grouped = true;
          result.group_instance = layer_group->instance;
          result.group_below = bke::node_find_socket(
              *layer_group->instance, SOCK_IN, UString::from_ptr_noinline(below_iface->identifier));
          result.group_color = bke::node_find_socket(
              *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(color_iface->identifier));
          result.group_coverage = bke::node_find_socket(
              *layer_group->instance,
              SOCK_OUT,
              UString::from_ptr_noinline(coverage_iface->identifier));
          result.group_blend = bke::node_find_socket(
              *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(blend_iface->identifier));
          result.group_result = bke::node_find_socket(
              *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(result_iface->identifier));
          if (content_alpha_iface != nullptr) {
            result.group_content_alpha = bke::node_find_socket(
                *layer_group->instance,
                SOCK_OUT,
                UString::from_ptr_noinline(content_alpha_iface->identifier));
          }
        }

        result.valid = true;
        result.current = current;
        result.folder_coverage_node = folder_coverage_node;
        result.folder_coverage = folder_coverage;
        result.folder_content_alpha_node = folder_content_alpha_node;
        result.folder_content_alpha = folder_content_alpha;
        return result;
      };
      for (const MaterialPaintLayer &layer_ref :
           list)
      {
        const MaterialPaintLayer *layer = &layer_ref;
        if (BKE_paint_layers_folder_is_pass_through(ma, *layer))
        {
          if (row_is_removed(ma, *layer)) {
            /* The over-budget pass dropped this hidden folder; its inlined subtree goes with it. */
            continue;
          }
          /* A Pass Through folder is expanded in place: its children are built straight into the
           * parent's chain with the parent's own premul mode, exactly as if the folder were not
           * there. This is what keeps the generated shader identical when a row is moved into or
           * out of such a folder.
           *
           * Why inlining and not an isolated sub-chain: EEVEE compiles a material as one shader, so
           * any structural difference in the generated graph recompiles it; with a live Material row
           * carrying a large source graph that is seconds of stall. The isolated folder always edits
           * that structure (its own group instance, the P/a divide, the coverage overlay), while the
           * inlined form does not, so EEVEE can take the unchanged pass from its cache.
           *
           * Why it is correct: over is associative under Normal/Mix at full opacity with no mask,
           * correction or bake, so `over(below, P/a, a)` of the isolated folder equals laying the
           * children over `below` one after another. The predicate rejects every case where that
           * fails (opacity below one with several children in particular); a hidden Pass Through
           * folder scales each descendant's factor to zero through its value input, which is exact
           * and stays a value edit.
           *
           * What it costs and what to check when refactoring: switching a folder's mode is one
           * rebuild and one EEVEE compile, which is acceptable. The win depends on the build
           * producing byte-identical node and link order before and after a move; if it stops doing
           * so the compile returns silently. The CPU compositor and #BKE_paint_layers_bake_render_node
           * must keep giving the GPU's result -- see `composite_image_layers_build`, which flattens
           * the same folder. Alternatives considered and rejected: reserving correction slots (does
           * not avoid the code change) and an always-present opacity input (complicates every chain
           * for this rare case). */
          ChainResult sub = build_list(layer->children, previous, premul, parent_target, channel);
          previous = sub.chain;
          coverage_node = sub.coverage_node;
          coverage = sub.coverage;
          content_alpha_node = sub.content_alpha_node;
          content_alpha = sub.content_alpha;
          continue;
        }
        Image *baked_color = nullptr;
        const bool substituted = row_channel_substituted(ma, *layer, channel, &baked_color);
        const bool is_folder = BKE_paint_layers_is_folder(*layer);

        RowTarget row_target = parent_target;
        row_target.location_x = location_x;
        row_target.location_y = location_y;
        LayerGroup *layer_group = nullptr;
        /* Every row gets its own group: a bake-substituted row, a folder that takes part, or a leaf
         * that paints something here. The same helper feeds the root hash, so the root and the build
         * never disagree about which instances exist. */
        const bool group_this = layer_row_has_group(ma, *layer, channel, cache);
        if (group_this) {
          layer_group = layer_group_ensure(*layer, *parent_target.tree);
        }
        RowResult row;
        if (layer_group != nullptr && layer_group->unchanged) {
          /* The group's topology is current: keep its nodes and interface and chain the existing
           * instance. */
          row = row_from_unchanged_group(*layer_group, *layer, channel);
        }
        else {
          if (layer_group != nullptr) {
            row_target.tree = layer_group->tree;
            row_target.group_input = layer_group->group_input;
          }
          row = build_row(layer, row_target, substituted, baked_color, premul, layer_group);
        }
        if (!row.valid) {
          continue;
        }
        ChainLayer current = row.current;
        bNode *folder_coverage_node = row.folder_coverage_node;
        bNodeSocket *folder_coverage = row.folder_coverage;
        if (row.grouped) {
          /* The parent chains the group through its instance: Color and Coverage are the row's
           * straight result, while Below/Blend/Result are the instance's own sockets. */
          current.source_node = row.group_instance;
          current.source = row.group_color;
          current.factor_node = row.group_instance;
          current.factor = row.group_coverage;
          /* The row's straight content alpha comes from the instance's Content Alpha output when
           * the group tracked one; an absent output leaves the chain untracked (treat as 1.0).
           * The inner-tree socket is never kept here, it lives in another tree. */
          current.content_alpha_node = (row.group_content_alpha != nullptr) ? row.group_instance :
                                                                              nullptr;
          current.content_alpha = row.group_content_alpha;
        }

      if (premul) {
        /* The isolated-group accumulation (design §5): S = P/a, c_eff = lerp(c, blend(S, c), a),
         * P = P(1-f) + c_eff*f, a = a(1-f) + f. */
        bNode *s_divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
        bNodeSocket *sd_a = (s_divide != nullptr) ? socket_in(*s_divide, "Vector") : nullptr;
        bNodeSocket *sd_b = (s_divide != nullptr) ? socket_in(*s_divide, "Vector_001") : nullptr;
        bNodeSocket *sd_out = (s_divide != nullptr) ? socket_out(*s_divide, "Vector") : nullptr;
        if (sd_out == nullptr || coverage_node == nullptr || coverage == nullptr) {
          continue;
        }
        s_divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
        s_divide->location[0] = location_x;
        s_divide->location[1] = location_y + 120.0f;
        bke::node_add_link(tree, *previous.source_node, *previous.source, *s_divide, *sd_a);
        bke::node_add_link(tree, *coverage_node, *coverage, *s_divide, *sd_b);

        /* blend_m(S, c) at full factor. */
        bNode *row_blend = nullptr;
        bNodeSocket *row_blend_out = nullptr;
        if (row.grouped) {
          /* S = P/a feeds the group's Below; its Blend output is blend_m(S, Color) at factor one. */
          if (row.group_below != nullptr) {
            bke::node_add_link(tree, *s_divide, *sd_out, *row.group_instance, *row.group_below);
          }
          row_blend = row.group_instance;
          row_blend_out = row.group_blend;
        }
        else if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
            tree.typeinfo != nullptr && tree.typeinfo->group_idname != nullptr)
        {
          bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
          if (combine != nullptr) {
            combine->id = &ctx.normal_combine_group->id;
            id_us_plus(&ctx.normal_combine_group->id);
            combine->location[0] = location_x;
            combine->location[1] = location_y - 120.0f;
            nodes::update_node_declaration_and_sockets(tree, *combine);
            bNodeSocket *a_in = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
            bNodeSocket *b_in = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
            bNodeSocket *f_in = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
            bNodeSocket *r_out = bke::node_find_socket(
                *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
            if (a_in != nullptr && b_in != nullptr && f_in != nullptr && r_out != nullptr) {
              if (f_in->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = 1.0f;
              }
              bke::node_add_link(tree, *s_divide, *sd_out, *combine, *a_in);
              bke::node_add_link(tree, *current.source_node, *current.source, *combine, *b_in);
              row_blend = combine;
              row_blend_out = r_out;
            }
          }
        }
        else {
          bNode *mix = mix_node_add(tree,
                                    BKE_paint_layers_blend_to_ramp(
                                        eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*current.layer, channel))),
                                    location_x,
                                    location_y - 120.0f);
          if (mix != nullptr) {
            bNodeSocket *fac = socket_in(*mix, "Factor_Float");
            bNodeSocket *color1 = socket_in(*mix, "A_Color");
            bNodeSocket *color2 = socket_in(*mix, "B_Color");
            bNodeSocket *color_out = socket_out(*mix, "Result_Color");
            if (fac != nullptr && color1 != nullptr && color2 != nullptr && color_out != nullptr) {
              if (fac->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(fac->default_value)->value = 1.0f;
              }
              bke::node_add_link(tree, *s_divide, *sd_out, *mix, *color1);
              bke::node_add_link(tree, *current.source_node, *current.source, *mix, *color2);
              row_blend = mix;
              row_blend_out = color_out;
            }
          }
        }
        if (row_blend == nullptr || row_blend_out == nullptr) {
          continue;
        }

        /* c_eff = lerp(c, blend, a). */
        bNode *c_eff = mix_node_add(tree, MA_RAMP_BLEND, location_x + 120.0f, location_y - 240.0f);
        bNodeSocket *eff_a = (c_eff != nullptr) ? socket_in(*c_eff, "A_Color") : nullptr;
        bNodeSocket *eff_b = (c_eff != nullptr) ? socket_in(*c_eff, "B_Color") : nullptr;
        bNodeSocket *eff_f = (c_eff != nullptr) ? socket_in(*c_eff, "Factor_Float") : nullptr;
        bNodeSocket *eff_out = (c_eff != nullptr) ? socket_out(*c_eff, "Result_Color") : nullptr;
        if (eff_out == nullptr) {
          continue;
        }
        bke::node_add_link(tree, *current.source_node, *current.source, *c_eff, *eff_a);
        bke::node_add_link(tree, *row_blend, *row_blend_out, *c_eff, *eff_b);
        bke::node_add_link(tree, *coverage_node, *coverage, *c_eff, *eff_f);

        /* P = lerp(P, c_eff, f). */
        bNode *p_mix = mix_node_add(tree, MA_RAMP_BLEND, location_x + 240.0f, location_y);
        bNodeSocket *pm_a = (p_mix != nullptr) ? socket_in(*p_mix, "A_Color") : nullptr;
        bNodeSocket *pm_b = (p_mix != nullptr) ? socket_in(*p_mix, "B_Color") : nullptr;
        bNodeSocket *pm_f = (p_mix != nullptr) ? socket_in(*p_mix, "Factor_Float") : nullptr;
        bNodeSocket *pm_out = (p_mix != nullptr) ? socket_out(*p_mix, "Result_Color") : nullptr;
        if (pm_out == nullptr) {
          continue;
        }
        bke::node_add_link(tree, *previous.source_node, *previous.source, *p_mix, *pm_a);
        bke::node_add_link(tree, *c_eff, *eff_out, *p_mix, *pm_b);
        if (current.factor != nullptr && current.factor_node != nullptr) {
          bke::node_add_link(tree, *current.factor_node, *current.factor, *p_mix, *pm_f);
        }
        /* The content alpha beside the colour's own alpha: S_a = P_a / a through a Math divide
         * (safe on zero, next to the Vector divide above), then c_eff_a and P_a like the colour
         * path: c_eff_a = c_a at Mix, folded with the straight alpha below otherwise, and
         * P_a = P_a + (c_eff_a - P_a) * f. An untracked row counts as opaque; with no tracked
         * alpha anywhere nothing is built and the chain stays null. */
        bNode *content_pa_node = previous.content_alpha_node;
        bNodeSocket *content_pa = previous.content_alpha;
        if (track_content_alpha &&
            (previous.content_alpha != nullptr || current.content_alpha != nullptr))
        {
          bNode *below_node = previous.content_alpha_node;
          bNodeSocket *below = previous.content_alpha;
          if (below == nullptr) {
            below_node = coverage_node;
            below = coverage;
          }
          bNode *row_alpha_node = current.content_alpha_node;
          bNodeSocket *row_a = current.content_alpha;
          bNode *row_one = nullptr;
          if (row_a == nullptr) {
            row_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
            row_a = (row_one != nullptr) ? socket_out(*row_one, "Value") : nullptr;
            if (row_a != nullptr && row_a->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(row_a->default_value)->value = 1.0f;
            }
            row_alpha_node = row_one;
          }
          bNode *factor_one = nullptr;
          bNode *factor_node = current.factor_node;
          bNodeSocket *factor = current.factor;
          if (factor == nullptr) {
            factor_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
            factor = (factor_one != nullptr) ? socket_out(*factor_one, "Value") : nullptr;
            if (factor != nullptr && factor->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(factor->default_value)->value = 1.0f;
            }
            factor_node = factor_one;
          }
          bNode *alpha_div = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          bNodeSocket *ad_a = (alpha_div != nullptr) ? socket_in(*alpha_div, "Value") : nullptr;
          bNodeSocket *ad_b = (alpha_div != nullptr) ? socket_in(*alpha_div, "Value_001") :
                                                       nullptr;
          bNodeSocket *ad_out = (alpha_div != nullptr) ? socket_out(*alpha_div, "Value") : nullptr;
          const int row_ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
              BKE_paint_layers_channel_blend_effective(*current.layer, channel)));
          if (below != nullptr && below_node != nullptr && row_a != nullptr &&
              row_alpha_node != nullptr && factor != nullptr && factor_node != nullptr &&
              ad_out != nullptr)
          {
            alpha_div->custom1 = NODE_MATH_DIVIDE;
            alpha_div->location[0] = location_x + 240.0f;
            alpha_div->location[1] = location_y - 520.0f;
            bke::node_add_link(tree, *below_node, *below, *alpha_div, *ad_a);
            bke::node_add_link(tree, *coverage_node, *coverage, *alpha_div, *ad_b);
            /* c_eff_a: the row's own alpha at Mix, folded with the straight alpha below when the
             * row's blend leaves alpha (every mode but Mix). */
            bNode *ceff_node = row_alpha_node;
            bNodeSocket *ceff = row_a;
            if (row_ramp != MA_RAMP_BLEND) {
              bNode *csub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *cmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *cadd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNodeSocket *cs_a = (csub != nullptr) ? socket_in(*csub, "Value") : nullptr;
              bNodeSocket *cs_b = (csub != nullptr) ? socket_in(*csub, "Value_001") : nullptr;
              bNodeSocket *cm_a = (cmul != nullptr) ? socket_in(*cmul, "Value") : nullptr;
              bNodeSocket *cm_b = (cmul != nullptr) ? socket_in(*cmul, "Value_001") : nullptr;
              bNodeSocket *ca_a = (cadd != nullptr) ? socket_in(*cadd, "Value") : nullptr;
              bNodeSocket *ca_b = (cadd != nullptr) ? socket_in(*cadd, "Value_001") : nullptr;
              bNodeSocket *cs_out = (csub != nullptr) ? socket_out(*csub, "Value") : nullptr;
              bNodeSocket *cm_out = (cmul != nullptr) ? socket_out(*cmul, "Value") : nullptr;
              bNodeSocket *ca_out = (cadd != nullptr) ? socket_out(*cadd, "Value") : nullptr;
              if (cs_out != nullptr && cm_out != nullptr && ca_out != nullptr) {
                csub->custom1 = NODE_MATH_SUBTRACT;
                cmul->custom1 = NODE_MATH_MULTIPLY;
                cadd->custom1 = NODE_MATH_ADD;
                bke::node_add_link(tree, *alpha_div, *ad_out, *csub, *cs_a);
                bke::node_add_link(tree, *row_alpha_node, *row_a, *csub, *cs_b);
                bke::node_add_link(tree, *csub, *cs_out, *cmul, *cm_a);
                bke::node_add_link(tree, *coverage_node, *coverage, *cmul, *cm_b);
                bke::node_add_link(tree, *row_alpha_node, *row_a, *cadd, *ca_a);
                bke::node_add_link(tree, *cmul, *cm_out, *cadd, *ca_b);
                ceff_node = cadd;
                ceff = ca_out;
              }
            }
            /* P_a = P_a + (c_eff_a - P_a) * f. */
            bNode *psub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNode *pmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNode *padd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNodeSocket *ps_a = (psub != nullptr) ? socket_in(*psub, "Value") : nullptr;
            bNodeSocket *ps_b = (psub != nullptr) ? socket_in(*psub, "Value_001") : nullptr;
            bNodeSocket *pm_a2 = (pmul != nullptr) ? socket_in(*pmul, "Value") : nullptr;
            bNodeSocket *pm_b2 = (pmul != nullptr) ? socket_in(*pmul, "Value_001") : nullptr;
            bNodeSocket *pa_a = (padd != nullptr) ? socket_in(*padd, "Value") : nullptr;
            bNodeSocket *pa_b = (padd != nullptr) ? socket_in(*padd, "Value_001") : nullptr;
            bNodeSocket *ps_out = (psub != nullptr) ? socket_out(*psub, "Value") : nullptr;
            bNodeSocket *pm_out2 = (pmul != nullptr) ? socket_out(*pmul, "Value") : nullptr;
            bNodeSocket *pa_out = (padd != nullptr) ? socket_out(*padd, "Value") : nullptr;
            if (ps_out != nullptr && pm_out2 != nullptr && pa_out != nullptr) {
              psub->custom1 = NODE_MATH_SUBTRACT;
              pmul->custom1 = NODE_MATH_MULTIPLY;
              padd->custom1 = NODE_MATH_ADD;
              bke::node_add_link(tree, *ceff_node, *ceff, *psub, *ps_a);
              bke::node_add_link(tree, *below_node, *below, *psub, *ps_b);
              bke::node_add_link(tree, *psub, *ps_out, *pmul, *pm_a2);
              bke::node_add_link(tree, *factor_node, *factor, *pmul, *pm_b2);
              bke::node_add_link(tree, *below_node, *below, *padd, *pa_a);
              bke::node_add_link(tree, *pmul, *pm_out2, *padd, *pa_b);
              content_pa_node = padd;
              content_pa = pa_out;
            }
          }
        }
        previous = {current.layer,
                    p_mix,
                    pm_out,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    content_pa_node,
                    content_pa};
        content_alpha_node = content_pa_node;
        content_alpha = content_pa;

        /* a = a + f*(1-a). */
        bNode *one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNodeSocket *om_a = (one_minus != nullptr) ? socket_in(*one_minus, "Value") : nullptr;
        bNodeSocket *om_b = (one_minus != nullptr) ? socket_in(*one_minus, "Value_001") : nullptr;
        bNodeSocket *sc_a = (scaled != nullptr) ? socket_in(*scaled, "Value") : nullptr;
        bNodeSocket *sc_b = (scaled != nullptr) ? socket_in(*scaled, "Value_001") : nullptr;
        bNodeSocket *jo_a = (joined != nullptr) ? socket_in(*joined, "Value") : nullptr;
        bNodeSocket *jo_b = (joined != nullptr) ? socket_in(*joined, "Value_001") : nullptr;
        if (om_b == nullptr || sc_a == nullptr || sc_b == nullptr || jo_a == nullptr ||
            jo_b == nullptr)
        {
          continue;
        }
        one_minus->custom1 = NODE_MATH_SUBTRACT;
        scaled->custom1 = NODE_MATH_MULTIPLY;
        joined->custom1 = NODE_MATH_ADD;
        if (om_a != nullptr && om_a->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(om_a->default_value)->value = 1.0f;
        }
        bke::node_add_link(tree, *coverage_node, *coverage, *one_minus, *om_b);
        if (current.factor != nullptr && current.factor_node != nullptr) {
          bke::node_add_link(tree, *current.factor_node, *current.factor, *scaled, *sc_a);
        }
        else if (sc_a->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(sc_a->default_value)->value = 1.0f;
        }
        bke::node_add_link(tree, *one_minus, *socket_out(*one_minus, "Value"), *scaled, *sc_b);
        bke::node_add_link(tree, *coverage_node, *coverage, *joined, *jo_a);
        bke::node_add_link(tree, *scaled, *socket_out(*scaled, "Value"), *joined, *jo_b);
        coverage_node = joined;
        coverage = socket_out(*joined, "Value");
      }
      else if (row.grouped) {
        /* The group's Result already blends the row over Below; the parent only feeds Below. */
        if (previous.source != nullptr && previous.source_node != nullptr &&
            row.group_below != nullptr)
        {
          bke::node_add_link(tree,
                             *previous.source_node,
                             *previous.source,
                             *row.group_instance,
                             *row.group_below);
        }
        /* The parent's own content chain lays the group's straight alpha over the alpha below,
         * at Mix; any other blend leaves the alpha below alone, like the colour path. */
        bNode *below_content_node = previous.content_alpha_node;
        bNodeSocket *below_content = previous.content_alpha;
        /* An untracked row counts as opaque, like the isolated chain's `row_one`: a MESH_MAP or
         * Material leaf exposes no Content Alpha output, yet the CPU still blends the row's full
         * coverage by the row factor, so the chain has to see a constant one here. */
        bNode *row_one = nullptr;
        bNodeSocket *row_content = current.content_alpha;
        bNode *row_content_node = current.content_alpha_node;
        if (track_content_alpha && row_content == nullptr) {
          row_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
          row_content = (row_one != nullptr) ? socket_out(*row_one, "Value") : nullptr;
          if (row_content != nullptr && row_content->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(row_content->default_value)->value = 1.0f;
          }
          row_content_node = row_one;
        }
        if (track_content_alpha && row_content != nullptr && row_content_node != nullptr)
        {
          if (below_content == nullptr) {
            bNode *bottom_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
            below_content = (bottom_one != nullptr) ? socket_out(*bottom_one, "Value") : nullptr;
            if (below_content != nullptr && below_content->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(below_content->default_value)->value = 1.0f;
            }
            below_content_node = bottom_one;
          }
          const int group_ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
              BKE_paint_layers_channel_blend_effective(*current.layer, channel)));
          if (below_content != nullptr && group_ramp == MA_RAMP_BLEND) {
            bNode *gsub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNode *gmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNode *gadd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
            bNodeSocket *gs_a = (gsub != nullptr) ? socket_in(*gsub, "Value") : nullptr;
            bNodeSocket *gs_b = (gsub != nullptr) ? socket_in(*gsub, "Value_001") : nullptr;
            bNodeSocket *gm_a = (gmul != nullptr) ? socket_in(*gmul, "Value") : nullptr;
            bNodeSocket *gm_b = (gmul != nullptr) ? socket_in(*gmul, "Value_001") : nullptr;
            bNodeSocket *ga_a = (gadd != nullptr) ? socket_in(*gadd, "Value") : nullptr;
            bNodeSocket *ga_b = (gadd != nullptr) ? socket_in(*gadd, "Value_001") : nullptr;
            bNodeSocket *gs_out = (gsub != nullptr) ? socket_out(*gsub, "Value") : nullptr;
            bNodeSocket *gm_out = (gmul != nullptr) ? socket_out(*gmul, "Value") : nullptr;
            bNodeSocket *ga_out = (gadd != nullptr) ? socket_out(*gadd, "Value") : nullptr;
            if (gs_out != nullptr && gm_out != nullptr && ga_out != nullptr) {
              gsub->custom1 = NODE_MATH_SUBTRACT;
              gmul->custom1 = NODE_MATH_MULTIPLY;
              gadd->custom1 = NODE_MATH_ADD;
              bke::node_add_link(
                  tree, *row_content_node, *row_content, *gsub, *gs_a);
              bke::node_add_link(tree, *below_content_node, *below_content, *gsub, *gs_b);
              bke::node_add_link(tree, *gsub, *gs_out, *gmul, *gm_a);
              if (current.factor != nullptr && current.factor_node != nullptr) {
                bke::node_add_link(
                    tree, *current.factor_node, *current.factor, *gmul, *gm_b);
              }
              else if (gm_b != nullptr && gm_b->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(gm_b->default_value)->value = 1.0f;
              }
              bke::node_add_link(tree, *below_content_node, *below_content, *gadd, *ga_a);
              bke::node_add_link(tree, *gmul, *gm_out, *gadd, *ga_b);
              below_content_node = gadd;
              below_content = ga_out;
            }
          }
        }
        previous = {current.layer,
                    row.group_instance,
                    row.group_result,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    below_content_node,
                    below_content};
        content_alpha_node = below_content_node;
        content_alpha = below_content;
      }
      else {
        bool combined = false;
        if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
            tree.typeinfo != nullptr && tree.typeinfo->group_idname != nullptr)
        {
          bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
          if (combine != nullptr) {
            combine->id = &ctx.normal_combine_group->id;
            id_us_plus(&ctx.normal_combine_group->id);
            combine->location[0] = location_x;
            combine->location[1] = location_y;
            /* A hand-assigned group only grows its instance sockets once its declaration is built;
             * this instantiates them from the group's interface without needing #Main or a whole
             * tree update. */
            nodes::update_node_declaration_and_sockets(tree, *combine);
            bNodeSocket *socket_a = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
            bNodeSocket *socket_b = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
            bNodeSocket *factor = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
            bNodeSocket *result = bke::node_find_socket(
                *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
            if (socket_a == nullptr || socket_b == nullptr || factor == nullptr ||
                result == nullptr)
            {
              /* The sockets are the group's own interface; a missing one is a build error, not a
               * shape to paper over with a Mix that would silently flatten the relief. */
              BLI_assert_msg(false, "Normal Combine instance sockets were not instantiated");
              continue;
            }
            bke::node_add_link(
                tree, *previous.source_node, *previous.source, *combine, *socket_a);
            bke::node_add_link(
                tree, *current.source_node, *current.source, *combine, *socket_b);
            if (current.factor != nullptr && current.factor_node != nullptr) {
              bke::node_add_link(tree, *current.factor_node, *current.factor, *combine, *factor);
            }
            previous = {current.layer,
                        combine,
                        result,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        previous.content_alpha_node,
                        previous.content_alpha};
            combined = true;
          }
        }
        if (!combined) {
          bNode *mix = mix_node_add(tree,
                                    BKE_paint_layers_blend_to_ramp(
                                        eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*current.layer, channel))),
                                    location_x,
                                    location_y);
          if (mix != nullptr) {
            STRNCPY_UTF8(mix->label, current.layer->name);
            bNodeSocket *fac = socket_in(*mix, "Factor_Float");
            bNodeSocket *color1 = socket_in(*mix, "A_Color");
            bNodeSocket *color2 = socket_in(*mix, "B_Color");
            bNodeSocket *color_out = socket_out(*mix, "Result_Color");
            if (fac != nullptr && color1 != nullptr && color2 != nullptr && color_out != nullptr) {
              bke::node_add_link(tree, *previous.source_node, *previous.source, *mix, *color1);
              bke::node_add_link(tree, *current.source_node, *current.source, *mix, *color2);
              if (current.factor != nullptr && current.factor_node != nullptr) {
                bke::node_add_link(tree, *current.factor_node, *current.factor, *mix, *fac);
              }
              /* The straight content chain mirrors the Mix: lay the row's alpha over the alpha
               * below at Mix, keep the alpha below for any other blend. */
              bNode *mix_below_node = previous.content_alpha_node;
              bNodeSocket *mix_below = previous.content_alpha;
              previous = {current.layer,
                          mix,
                          color_out,
                          nullptr,
                          nullptr,
                          nullptr,
                          nullptr,
                          mix_below_node,
                          mix_below};
              if (track_content_alpha && current.content_alpha != nullptr &&
                  current.content_alpha_node != nullptr)
              {
                const int mix_ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
                    BKE_paint_layers_channel_blend_effective(*current.layer, channel)));
                if (mix_below == nullptr) {
                  bNode *mix_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
                  mix_below = (mix_one != nullptr) ? socket_out(*mix_one, "Value") : nullptr;
                  if (mix_below != nullptr && mix_below->default_value != nullptr) {
                    static_cast<bNodeSocketValueFloat *>(mix_below->default_value)->value = 1.0f;
                  }
                  mix_below_node = mix_one;
                }
                if (mix_below != nullptr && mix_ramp == MA_RAMP_BLEND) {
                  bNode *msub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
                  bNode *mmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
                  bNode *madd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
                  bNodeSocket *ms_a = (msub != nullptr) ? socket_in(*msub, "Value") : nullptr;
                  bNodeSocket *ms_b = (msub != nullptr) ? socket_in(*msub, "Value_001") : nullptr;
                  bNodeSocket *mm_a = (mmul != nullptr) ? socket_in(*mmul, "Value") : nullptr;
                  bNodeSocket *mm_b = (mmul != nullptr) ? socket_in(*mmul, "Value_001") : nullptr;
                  bNodeSocket *ma_a = (madd != nullptr) ? socket_in(*madd, "Value") : nullptr;
                  bNodeSocket *ma_b = (madd != nullptr) ? socket_in(*madd, "Value_001") : nullptr;
                  bNodeSocket *ms_out = (msub != nullptr) ? socket_out(*msub, "Value") : nullptr;
                  bNodeSocket *mm_out = (mmul != nullptr) ? socket_out(*mmul, "Value") : nullptr;
                  bNodeSocket *ma_out = (madd != nullptr) ? socket_out(*madd, "Value") : nullptr;
                  if (ms_out != nullptr && mm_out != nullptr && ma_out != nullptr) {
                    msub->custom1 = NODE_MATH_SUBTRACT;
                    mmul->custom1 = NODE_MATH_MULTIPLY;
                    madd->custom1 = NODE_MATH_ADD;
                    bke::node_add_link(
                        tree, *current.content_alpha_node, *current.content_alpha, *msub, *ms_a);
                    bke::node_add_link(tree, *mix_below_node, *mix_below, *msub, *ms_b);
                    bke::node_add_link(tree, *msub, *ms_out, *mmul, *mm_a);
                    if (current.factor != nullptr && current.factor_node != nullptr) {
                      bke::node_add_link(
                          tree, *current.factor_node, *current.factor, *mmul, *mm_b);
                    }
                    else if (mm_b != nullptr && mm_b->default_value != nullptr) {
                      static_cast<bNodeSocketValueFloat *>(mm_b->default_value)->value = 1.0f;
                    }
                    bke::node_add_link(tree, *mix_below_node, *mix_below, *madd, *ma_a);
                    bke::node_add_link(tree, *mmul, *mm_out, *madd, *ma_b);
                    mix_below_node = madd;
                    mix_below = ma_out;
                  }
                }
                previous.content_alpha_node = mix_below_node;
                previous.content_alpha = mix_below;
                content_alpha_node = mix_below_node;
                content_alpha = mix_below;
              }
            }
          }
        }
      }
      location_x += 180.0f;
      }
      return {previous, coverage_node, coverage, content_alpha_node, content_alpha};
    };
    ChainResult built = build_list(ma.paint_layers, previous, false, target, channel);
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
  auto value_key = [](const bUUID &marker, const char *role, const int channel) -> std::string {
    char marker_text[UUID_STRING_SIZE];
    BLI_uuid_format(marker_text, marker);
    char key[UUID_STRING_SIZE + 128];
    BLI_snprintf(key, sizeof(key), "%s|%s|%d", marker_text, role, channel);
    return std::string(key);
  };
  std::function<void(bNodeTree &, bNode &)> mirror_scope =
      [&](bNodeTree &scope_tree, bNode &scope_group_input) {
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
            mirror_scope(*group_tree, *child_group_input);
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
          mirrors.add(value_key(marker, role, prop_int_get(candidate->properties,
                                                           INPUT_CHANNEL_PROP,
                                                           -1)),
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
            if (BKE_paint_layers_find(const_cast<Material &>(ma), marker) == nullptr) {
              continue;
            }
            const int channel = prop_int_get(iface->properties, INPUT_CHANNEL_PROP, -1);
            const std::string key = value_key(marker, role, channel);
            bNodeTreeInterfaceSocket *scope_iface = mirrors.lookup_default(key, nullptr);
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
      };
  mirror_scope(tree, *group_input);

  /* A rebuilt group kept its old interface so its socket identifiers -- and the parent's links
   * into them -- stay stable; drop the sockets this build no longer uses so the interface matches
   * what a fresh build would produce (and its signature, which the root hash reads, is honest). */
  for (const auto &item : layer_groups.items()) {
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

  /* Every Image Texture the build created samples the one UV layer the material names. Each layer
   * group is a separate tree, so each that holds a map gets its own UV Map node; a tree with no map
   * gets none. The root is handled the same way. */
  const char *const uv_name = BKE_paint_layers_uv_map_name(ma);
  generated_uv_maps_wire(tree, uv_name);
  for (const auto &item : layer_groups.items()) {
    if (item.value->tree != nullptr) {
      generated_uv_maps_wire(*item.value->tree, uv_name);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Regenerate
 * \{ */

}  // namespace blender

