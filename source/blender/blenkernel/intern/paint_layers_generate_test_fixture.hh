/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"

/* The D1 root-hash/reload tests below need the stored root hash accessors. */
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_scene.hh"

#include "paint_material_composite_internal.hh"

#include "NOD_socket.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_time.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <thread>

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_color_types.h"
#include "DNA_colorband_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::bke::tests {

class PaintLayersGenerateTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
    ma = BKE_material_add(bmain, "Layered");
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
    /* The sampler budget is runtime global state; start every test with the check off. */
    BKE_paint_layers_sampler_budget_set(0, 0);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Image *add_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }

  MaterialPaintLayer *add_paint_layer(const char *name, Image *image)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    layer->channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
    layer->channels[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    layer->channels[0].state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    layer->channels[0].image = image;
    layer->channels_num = 1;
    return layer;
  }

  /**
   * Add a Layer-role row directly into \a anchor's own #children -- a Stack correction/mask item
   * (#BKE_paint_layers_add accepts \a place Into there since it is a folder in every sense
   * #BKE_paint_layers_is_folder cares about, regardless of \a anchor's role). Mirrors
   * #add_paint_layer, which always anchors at the top level instead.
   */
  MaterialPaintLayer *add_paint_layer_into(MaterialPaintLayer *anchor,
                                           const char *name,
                                           Image *image)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, anchor, PaintLayerPlace::Into);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  }

  /** Move \a layer's hidden-at mark past #PAINT_LAYERS_COLD_TIER_SECONDS, so the next rebuild may
   * drop it. The mark is set by #BKE_paint_layers_set_enabled; this only ages it, the seam that
   * lets a test model "hidden longer than the tier" without waiting the real tier. */
  void age_cold_mark(MaterialPaintLayer *layer)
  {
    bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_mutable(*ma);
    ASSERT_NE(runtime, nullptr);
    double *since = runtime->hidden_since.lookup_ptr(layer->marker);
    ASSERT_NE(since, nullptr);
    *since = BLI_time_now_seconds() - PAINT_LAYERS_COLD_TIER_SECONDS - 1.0;
  }

  /** Disable \a layer and age its mark, so a rebuild now leaves it out of the graph. */
  void disable_and_age(MaterialPaintLayer *layer)
  {
    ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
    age_cold_mark(layer);
  }

  bNodeTree *make_tree(const char *name)
  {
    return bke::node_tree_add_tree(bmain, name, "ShaderNodeTree");
  }

  /** The layer-tree factory the build requires: an empty tree per row on the fixture's #Main. */
  bNodeTree *layer_tree_for(const MaterialPaintLayer &layer)
  {
    char name[64];
    BLI_snprintf(name,
                 sizeof(name),
                 ".PL Layer %s",
                 layer.name[0] != '\0' ? layer.name : "Layer");
    return make_tree(name);
  }

  /** The subgroup factory a pure build needs: an empty `.PL Mask <row>` tree per row on the
   * fixture's #Main. */
  bNodeTree *subgroup_tree_for(const MaterialPaintLayer &row, StringRef kind)
  {
    if (!STREQ(kind.data(), "mask")) {
      return nullptr;
    }
    char name[64];
    BLI_snprintf(name,
                 sizeof(name),
                 ".PL Mask %s",
                 row.name[0] != '\0' ? row.name : "Layer");
    return make_tree(name);
  }

  /** The `.PL Layer <name>` group in \a bmain, or null. */
  static bNodeTree *layer_tree_find(Main &bmain, const char *layer_name)
  {
    char full[96];
    BLI_snprintf(full, sizeof(full), ".PL Layer %s", layer_name);
    for (bNodeTree &tree : bmain.nodetrees) {
      if (STREQ(tree.id.name + 2, full)) {
        return &tree;
      }
    }
    return nullptr;
  }

  /** The `.PL Folder <name>` group in \a bmain, or null. */
  static bNodeTree *layer_tree_find_folder(Main &bmain, const char *folder_name)
  {
    char full[96];
    BLI_snprintf(full, sizeof(full), ".PL Folder %s", folder_name);
    for (bNodeTree &tree : bmain.nodetrees) {
      if (STREQ(tree.id.name + 2, full)) {
        return &tree;
      }
    }
    return nullptr;
  }

  static int layer_tree_count(Main &bmain)
  {
    int count = 0;
    for (bNodeTree &tree : bmain.nodetrees) {
      if (StringRef(tree.id.name + 2).startswith(".PL Layer ")) {
        count++;
      }
    }
    return count;
  }

  /** Count both `.PL Layer` and `.PL Folder` groups. */
  static int layer_group_tree_count(Main &bmain)
  {
    int count = 0;
    for (bNodeTree &tree : bmain.nodetrees) {
      const StringRef name(tree.id.name + 2);
      if (name.startswith(".PL Layer ") || name.startswith(".PL Folder ")) {
        count++;
      }
    }
    return count;
  }

  /** The `.PL Folder <name>` group in \a bmain, or null. */
  static bNodeTree *folder_tree_find(Main &bmain, const char *folder_name)
  {
    char full[96];
    BLI_snprintf(full, sizeof(full), ".PL Folder %s", folder_name);
    for (bNodeTree &tree : bmain.nodetrees) {
      if (STREQ(tree.id.name + 2, full)) {
        return &tree;
      }
    }
    return nullptr;
  }

  /** The layer group instance of \a group in \a parent, or null. */
  static bNode *group_instance_find(bNodeTree &parent, bNodeTree &group)
  {
    for (bNode &node : parent.nodes) {
      if (node.is_group() && node.id == &group.id) {
        return &node;
      }
    }
    return nullptr;
  }

  /** The layer/folder group instances directly in \a parent, one level down. */
  static void collect_group_instances(bNodeTree &parent, Vector<bNodeTree *> &r_instances)
  {
    for (bNode &node : parent.nodes) {
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        r_instances.append(id_cast<bNodeTree *>(node.id));
      }
    }
  }

  /**
   * #collect_group_instances without a row's packed `.PL Mask` group: those groups are nested
   * under the row's own group and are not part of the folder/row chain a test walks.
   */
  static void collect_chain_group_instances(bNodeTree &parent, Vector<bNodeTree *> &r_instances)
  {
    Vector<bNodeTree *> all;
    collect_group_instances(parent, all);
    for (bNodeTree *group : all) {
      const IDProperty *props = IDP_GetProperties(&group->id);
      if (props != nullptr &&
          IDP_GetPropertyTypeFromGroup(props, "pbr_paint_layers_subkind", IDP_STRING) != nullptr)
      {
        continue;
      }
      r_instances.append(group);
    }
  }

  static bool interface_has_socket(bNodeTree &tree, const char *name, const bool output)
  {
    tree.ensure_interface_cache();
    if (output) {
      for (bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
        if (socket->name != nullptr && STREQ(socket->name, name)) {
          return true;
        }
      }
    }
    else {
      for (bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
        if (socket->name != nullptr && STREQ(socket->name, name)) {
          return true;
        }
      }
    }
    return false;
  }

  /** Count nodes of \a type, descending into nested layer groups so the shape is seen whole. */
  static int count_type(const bNodeTree &tree, const int type)
  {
    int count = 0;
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == type) {
        count++;
      }
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        count += count_type(*reinterpret_cast<const bNodeTree *>(node.id), type);
      }
    }
    return count;
  }

  static int node_count(const bNodeTree &tree)
  {
    int count = 0;
    for (const bNode &node : tree.nodes) {
      UNUSED_VARS(node);
      count++;
    }
    return count;
  }

  /**
   * Visit \a tree's nodes and every node of the layer/folder groups it instances, packed `.PL Mask`
   * groups included; \a fn gets the owning tree with each node so it can read that tree's links.
   * The normal-combine group is skipped like the other recursive helpers.
   */
  template<typename Fn> static void walk_nodes_recursive(bNodeTree &tree, Fn &&fn)
  {
    for (bNode &node : tree.nodes) {
      fn(tree, node);
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        if (bNodeTree *group = id_cast<bNodeTree *>(node.id)) {
          walk_nodes_recursive(*group, fn);
        }
      }
    }
  }

  /** Whether every Image Texture in \a tree (nested layer groups included) reads a UV Map node. */
  static bool every_tex_image_uv_wired(const bNodeTree &tree)
  {
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == SH_NODE_TEX_IMAGE) {
        bNodeSocket *vector = bke::node_find_socket(
            const_cast<bNode &>(node), SOCK_IN, UString::from_ptr_noinline("Vector"));
        if (vector == nullptr || vector->directly_linked_links().is_empty()) {
          return false;
        }
        const bNode *from = vector->directly_linked_links()[0]->fromnode;
        if (from == nullptr || from->type_legacy != SH_NODE_UVMAP) {
          return false;
        }
      }
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        if (!every_tex_image_uv_wired(*reinterpret_cast<const bNodeTree *>(node.id))) {
          return false;
        }
      }
    }
    return true;
  }

  /** Find the first node of \a type, descending into nested layer groups. */
  static bNode *find_type(bNodeTree &tree, const int type)
  {
    for (bNode &node : tree.nodes) {
      if (node.type_legacy == type) {
        return &node;
      }
    }
    for (bNode &node : tree.nodes) {
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        if (bNode *found = find_type(*reinterpret_cast<bNodeTree *>(node.id), type)) {
          return found;
        }
      }
    }
    return nullptr;
  }

  /**
   * Stamp \a value into the first Mix node of \a group, descending nested groups. A preserved
   * group keeps the stamp across a regenerate; a rebuilt one comes back with the field cleared,
   * so this is what proves "the nodes are the same objects" without depending on allocator reuse.
   */
  static bool group_mix_sentinel_set(bNodeTree &group, const float value)
  {
    bNode *mix = find_type(group, SH_NODE_MIX);
    if (mix == nullptr) {
      return false;
    }
    mix->color[1] = value;
    return true;
  }

  static bool group_mix_sentinel_get(bNodeTree &group, const float value)
  {
    bNode *mix = find_type(group, SH_NODE_MIX);
    return mix != nullptr && mix->color[1] == value;
  }

  /** The root generated tree's nodes, in order; equal vectors mean the root was not rebuilt. */
  static Vector<bNode *> root_nodes(bNodeTree &tree)
  {
    Vector<bNode *> nodes;
    for (bNode &node : tree.nodes) {
      nodes.append(&node);
    }
    return nodes;
  }

  static bool same_nodes(const Vector<bNode *> &a, const Vector<bNode *> &b)
  {
    if (a.size() != b.size()) {
      return false;
    }
    for (const int i : a.index_range()) {
      if (a[i] != b[i]) {
        return false;
      }
    }
    return true;
  }

  /**
   * Stamp \a value into \a group's Group Input node. A preserved group keeps the stamp across a
   * regenerate; a rebuilt group clears its nodes (the factory's `tree_clear_nodes`), so the field
   * comes back defaulted. Unlike #group_mix_sentinel_set this needs no Mix node, so it works for a
   * Normal-only row whose blend is the normal-combine group rather than a Mix.
   */
  static bool group_io_sentinel_set(bNodeTree &group, const float value)
  {
    for (bNode &node : group.nodes) {
      if (node.is_group_input()) {
        node.color[1] = value;
        return true;
      }
    }
    return false;
  }

  static bool group_io_sentinel_get(bNodeTree &group, const float value)
  {
    for (bNode &node : group.nodes) {
      if (node.is_group_input()) {
        return node.color[1] == value;
      }
    }
    return false;
  }

  /** Every root link's sockets are still owned by the nodes it records, i.e. nothing dangles. */
  static bool root_links_are_consistent(bNodeTree &tree)
  {
    for (bNodeLink &link : tree.links) {
      if (link.fromsock == nullptr || link.tosock == nullptr || link.fromnode == nullptr ||
          link.tonode == nullptr)
      {
        return false;
      }
      if (&link.fromsock->owner_node() != link.fromnode ||
          &link.tosock->owner_node() != link.tonode)
      {
        return false;
      }
    }
    return true;
  }

  /** Compose Color Alpha nodes in \a tree left without a linked Alpha input, i.e. with no source. */
  static int count_unfed_compose_alpha(bNodeTree &tree)
  {
    int count = 0;
    for (bNode &node : tree.nodes) {
      if (node.type_legacy != SH_NODE_COMPOSE_COLOR_ALPHA) {
        continue;
      }
      bNodeSocket *alpha = bke::node_find_socket(
          node, SOCK_IN, UString::from_ptr_noinline("Alpha"));
      if (alpha == nullptr || alpha->directly_linked_links().is_empty()) {
        count++;
      }
    }
    return count;
  }

  /** An input socket of a layer group's own interface, by name, or null. */
  static bNodeTreeInterfaceSocket *group_input_find(bNodeTree &group, const char *name)
  {
    group.ensure_interface_cache();
    for (bNodeTreeInterfaceSocket *socket : group.interface_inputs()) {
      if (socket->name != nullptr && STREQ(socket->name, name)) {
        return socket;
      }
    }
    return nullptr;
  }

  /** The input socket of \a group's instance in \a parent that stands for interface \a iface. */
  static bNodeSocket *group_instance_input(bNodeTree &parent,
                                           bNodeTree &group,
                                           bNodeTreeInterfaceSocket &iface)
  {
    bNode *instance = group_instance_find(parent, group);
    if (instance == nullptr || iface.identifier == nullptr) {
      return nullptr;
    }
    return bke::node_find_socket(
        *instance, SOCK_IN, UString::from_ptr_noinline(iface.identifier));
  }

  bNode *instance_find()
  {
    if (ma->nodetree == nullptr || ma->paint_layers_tree == nullptr) {
      return nullptr;
    }
    for (bNode &node : ma->nodetree->nodes) {
      if (node.id == &ma->paint_layers_tree->id) {
        return &node;
      }
    }
    return nullptr;
  }

  bNodeTreeInterfaceSocket *interface_input_find(const char *name)
  {
    if (ma->paint_layers_tree == nullptr) {
      return nullptr;
    }
    ma->paint_layers_tree->ensure_interface_cache();
    for (bNodeTreeInterfaceSocket *socket : ma->paint_layers_tree->interface_inputs()) {
      if (socket->name != nullptr && STREQ(socket->name, name)) {
        return socket;
      }
    }
    return nullptr;
  }

  /**
   * The root instance input socket that mirrors the named value (A1): values are written there now,
   * and the layer/folder group sockets are fed from it by links, not written directly.
   */
  bNodeSocket *root_instance_input(const char *name)
  {
    bNode *instance = instance_find();
    bNodeTreeInterfaceSocket *iface = interface_input_find(name);
    if (instance == nullptr || iface == nullptr || iface->identifier == nullptr) {
      return nullptr;
    }
    return bke::node_find_socket(
        *instance, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
  }

  /** Whether \a tree carries a value input (source or mirror) for \a marker and \a role. */
  static bool interface_has_value_for(bNodeTree &tree, const bUUID &marker, const char *role)
  {
    char marker_text[UUID_STRING_SIZE];
    BLI_uuid_format(marker_text, marker);
    tree.ensure_interface_cache();
    for (bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
      if (socket->properties == nullptr) {
        continue;
      }
      const IDProperty *role_prop = IDP_GetPropertyTypeFromGroup(
          socket->properties, "pbr_paint_layers_role", IDP_STRING);
      const IDProperty *marker_prop = IDP_GetPropertyTypeFromGroup(
          socket->properties, "pbr_paint_layers_layer", IDP_STRING);
      if (role_prop != nullptr && STREQ(IDP_string_get(role_prop), role) &&
          marker_prop != nullptr && STREQ(IDP_string_get(marker_prop), marker_text))
      {
        return true;
      }
    }
    return false;
  }

  bNodeTreeInterfaceSocket *interface_output_find(const char *name)
  {
    if (ma->paint_layers_tree == nullptr) {
      return nullptr;
    }
    ma->paint_layers_tree->ensure_interface_cache();
    for (bNodeTreeInterfaceSocket *socket : ma->paint_layers_tree->interface_outputs()) {
      if (socket->name != nullptr && STREQ(socket->name, name)) {
        return socket;
      }
    }
    return nullptr;
  }

  MaterialPaintLayerChannel *add_channel(MaterialPaintLayer &layer,
                                         const eMaterialPaintChannel channel,
                                         Image *image)
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, &layer, channel);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return record;
  }

  /** One channel's bit in #Material::paint_layers_channels. */
  static constexpr uint16_t channel_bit(const eMaterialPaintChannel channel)
  {
    return uint16_t(1) << int(channel);
  }

  /**
   * Add \a extra to the material's channel set, keeping the derived default. A Material row has no
   * channel records, so the channels its source carries beyond the build default set (Specular,
   * Alpha, Emission) have to be opted into to keep the row participating in them.
   */
  void extend_channel_set(const uint16_t extra)
  {
    BKE_paint_layers_channel_set_mask_set(*ma,
                                          BKE_paint_layers_channel_set_mask_get(*ma) | extra);
  }

  /** A source material whose Principled is linked to its output and whose Roughness is constant. */
  Material *add_principled_source(const char *name, const float roughness)
  {
    Material *source = BKE_material_add(bmain, name);
    bNodeTree &ntree = *source->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(ntree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
    bNodeSocket *socket = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
    EXPECT_NE(socket, nullptr);
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = roughness;
    return source;
  }

  static bNode *principled_of(Material &source)
  {
    for (bNode &node : source.nodetree->nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        return &node;
      }
    }
    return nullptr;
  }

  /** The `.PL Layer <name>` group built for a row, via the real finder. */
  bNodeTree *find_row_group(const char *name)
  {
    return layer_tree_find(*bmain, name);
  }

  /** A node's spot in tree space: its own plus every parent frame's own. */
  static float2 node_global_location(const bNode &node)
  {
    float2 global(node.location[0], node.location[1]);
    for (const bNode *parent = node.parent; parent != nullptr; parent = parent->parent) {
      global.x += parent->location[0];
      global.y += parent->location[1];
    }
    return global;
  }

  /** Whether two nodes overlap as 140x160 boxes, the layout grid assumption. */
  static bool nodes_overlap(const bNode &a,
                            const bNode &b,
                            const float width = 140.0f,
                            const float height = 160.0f)
  {
    /* Why global: framed children store their spot relative to the frame, so the check
     * reads them in tree space and frames cannot break the invariant. */
    const float2 ga = node_global_location(a);
    const float2 gb = node_global_location(b);
    const float dx = ga.x - gb.x;
    const float dy = ga.y - gb.y;
    const float adx = dx < 0.0f ? -dx : dx;
    const float ady = dy < 0.0f ? -dy : dy;
    return adx < width && ady < height;
  }

  /**
   * Pairwise overlap check of every layout node in \a tree. Frames and the group
   * input/output are packing only and are skipped; each node is read in its own
   * tree's space because a nested `.PL Mask` group has its own origin. Returns
   * how many nodes were checked, so callers can assert the tree is not empty.
   */
  static int tree_nodes_do_not_overlap(bNodeTree &tree)
  {
    Vector<const bNode *> nodes;
    for (bNode &node : tree.nodes) {
      if (node.type_legacy == NODE_FRAME) {
        continue;
      }
      if (node.is_group_input() || node.is_group_output()) {
        continue;
      }
      nodes.append(&node);
    }
    for (int i = 0; i < nodes.size(); i++) {
      for (int j = i + 1; j < nodes.size(); j++) {
        EXPECT_FALSE(nodes_overlap(*nodes[i], *nodes[j]))
            << "nodes " << i << " (" << nodes[i]->idname << ") and " << j << " ("
            << nodes[j]->idname << ") overlap at (" << nodes[i]->location[0] << ", "
            << nodes[i]->location[1] << ") vs (" << nodes[j]->location[0] << ", "
            << nodes[j]->location[1] << ")";
      }
    }
    return nodes.size();
  }

  /**
   * The densest row for the layout test: a Fill mask (constant Mix only), an
   * Image mask (map plus the four-node grey mean plus Mix and factor) and an
   * Image Effect correction, all on Base Color. Built from the same setters the
   * mask/correction tests above use.
   */
  MaterialPaintLayer *build_row_with_two_masks_and_effect()
  {
    MaterialPaintLayer *row = add_paint_layer("Row", add_image("RowBase"));
    EXPECT_NE(row, nullptr);
    if (row == nullptr) {
      return nullptr;
    }
    MaterialPaintLayer *fill_mask = BKE_paint_layers_mask_add(*ma, row, 0.5f);
    EXPECT_NE(fill_mask, nullptr);
    MaterialPaintLayer *image_mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
    EXPECT_NE(image_mask, nullptr);
    if (image_mask != nullptr) {
      EXPECT_NE(BKE_paint_layers_channel_add(
                    *ma, image_mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                nullptr);
      EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma,
                                                     image_mask,
                                                     PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                     add_image("RowMask")));
      EXPECT_TRUE(BKE_paint_layers_correction_source_set(
          *ma, image_mask, MA_PAINT_LAYER_SOURCE_IMAGE));
    }
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
        *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "RowEffect");
    EXPECT_NE(effect, nullptr);
    if (effect != nullptr) {
      MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
          *ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      EXPECT_NE(record, nullptr);
      if (record != nullptr) {
        record->image = add_image("RowEffectMap");
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
      }
    }
    return row;
  }
};

}  // namespace blender::bke::tests
