/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"

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
#include "BKE_paint_material_resolve.hh"
#include "BKE_scene.hh"

#include "paint_material_composite_internal.hh"

#include "NOD_socket.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include <algorithm>
#include <array>
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
};

TEST_F(PaintLayersGenerateTest, color_chain_builds_one_mix_over_two_maps)
{
  Image *bottom = add_image("Bottom");
  Image *top = add_image("Top");
  add_paint_layer("Bottom", bottom);
  add_paint_layer("Top", top);

  bNodeTree *tree = make_tree("PBR Layers A");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  /* One Color channel is wired here (Base Color). The chain starts from a transparent constant, so
   * the two maps produce two Mix nodes. The Group Output and its wiring are the #Main-side
   * regenerate step's job, not this pure build. */
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), 2);
  EXPECT_EQ(count_type(*tree, SH_NODE_MIX), 2);
}

TEST_F(PaintLayersGenerateTest, build_is_deterministic)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));

  bNodeTree *first = make_tree("PBR Layers First");
  bNodeTree *second = make_tree("PBR Layers Second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  paint_layers_tree_build(*ma, *first, ctx);
  paint_layers_tree_build(*ma, *second, ctx);

  EXPECT_EQ(count_type(*first, SH_NODE_TEX_IMAGE), count_type(*second, SH_NODE_TEX_IMAGE));
  EXPECT_EQ(count_type(*first, SH_NODE_MIX),
            count_type(*second, SH_NODE_MIX));
}

TEST_F(PaintLayersGenerateTest, regenerate_builds_group_instance_and_principled)
{
  add_paint_layer("Bottom", add_image("Bottom"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_TRUE(report.created_principled);

  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  ASSERT_NE(interface_output_find("Result Base Color"), nullptr);

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
  EXPECT_FALSE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN);
}

TEST_F(PaintLayersGenerateTest, authored_fill_wires_the_default_channels)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  /* The raw constructor adds no channels; the authored policy is what gives them. */
  EXPECT_EQ(fill->channels_num, 0);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  EXPECT_EQ(fill->channels_num, 3);
  /* Idempotent: a second call adds nothing. */
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  EXPECT_EQ(fill->channels_num, 3);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  for (const char *socket_name : {"Base Color", "Metallic", "Roughness"}) {
    bNodeSocket *socket = bke::node_find_socket(
        const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(socket_name));
    ASSERT_NE(socket, nullptr) << socket_name;
    EXPECT_FALSE(socket->directly_linked_links().is_empty()) << socket_name;
  }
  /* The channels that would change the whole material's transparency or glow are not default. */
  for (const char *socket_name : {"Alpha", "Emission Color"}) {
    bNodeSocket *socket = bke::node_find_socket(
        const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(socket_name));
    if (socket != nullptr) {
      EXPECT_TRUE(socket->directly_linked_links().is_empty()) << socket_name;
    }
  }
}

TEST_F(PaintLayersGenerateTest, authored_paint_participates_but_covers_nothing)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *paint);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The channel is wired (the row participates), but the row has no constant to lay down, so the
   * chain has no map or Mix of its own for it. */
  ASSERT_NE(interface_output_find("Result Base Color"), nullptr);
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
  /* Only the bottom map and no fill input for the paint row. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
}

TEST_F(PaintLayersGenerateTest, uv_map_name_wires_every_image_texture)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNode *uv = find_type(*ma->paint_layers_tree, SH_NODE_UVMAP);
  ASSERT_NE(uv, nullptr);
  const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(uv->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_STREQ(storage->uv_map, "UVMap");
  EXPECT_TRUE(every_tex_image_uv_wired(*ma->paint_layers_tree));
}

TEST_F(PaintLayersGenerateTest, uv_map_no_name_keeps_graph_unchanged)
{
  add_paint_layer("Bottom", add_image("Bottom"));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_UVMAP), 0);
}

TEST_F(PaintLayersGenerateTest, uv_map_name_change_rebuilds_same_name_keeps_group)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_io_sentinel_set(*group, 0.5f));

  /* The same name: the group's topology hash is unchanged, so its nodes are preserved. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group_io_sentinel_get(*group, 0.5f));

  /* A new name is topology: the group is rebuilt and its UV Map node carries the new name. */
  BLI_strncpy(ma->paint_layers_uv_map, "Other", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNode *uv = find_type(*ma->paint_layers_tree, SH_NODE_UVMAP);
  ASSERT_NE(uv, nullptr);
  const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(uv->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_STREQ(storage->uv_map, "Other");
}

TEST_F(PaintLayersGenerateTest, uv_map_node_adds_no_sampler)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int with_name = BKE_paint_layers_sampler_count(*ma);

  BLI_strncpy(ma->paint_layers_uv_map, "", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int without_name = BKE_paint_layers_sampler_count(*ma);

  EXPECT_EQ(with_name, without_name);
}

TEST_F(PaintLayersGenerateTest, regenerate_is_idempotent)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *tree = ma->paint_layers_tree;
  const int images = count_type(*tree, SH_NODE_TEX_IMAGE);
  const int mixes = count_type(*tree, SH_NODE_MIX);
  const int instances = node_count(*ma->nodetree);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), images);
  EXPECT_EQ(count_type(*tree, SH_NODE_MIX), mixes);
  EXPECT_EQ(node_count(*ma->nodetree), instances);
}

TEST_F(PaintLayersGenerateTest, folder_builds_an_isolated_subchain)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The child's map is emitted, and the folder builds its isolated accumulation and overlay on top
   * of it (design §5): the sub-chain's Mix nodes, the P/a divide, the coverage chain. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  EXPECT_GE(count_type(*ma->paint_layers_tree, SH_NODE_MIX), 2);
  EXPECT_GE(count_type(*ma->paint_layers_tree, SH_NODE_MATH), 1);
  EXPECT_NE(interface_output_find("Result Base Color"), nullptr);
}

TEST_F(PaintLayersGenerateTest, fill_constant_has_no_map)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  const float color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  copy_v4_v4(layer->fill_color, color);
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 0);
  /* The Fill constant is an input of the layer's own group, not of the root. */
  bNodeTree *fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(fill_tree, nullptr);
  ASSERT_NE(group_input_find(*fill_tree, "Fill Roughness"), nullptr);
  ASSERT_NE(interface_output_find("Result Roughness"), nullptr);

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  EXPECT_FALSE(roughness->directly_linked_links().is_empty());
}

TEST_F(PaintLayersGenerateTest, values_sync_writes_opacity_and_enabled)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  BKE_paint_layers_set_opacity(*ma, layer, 0.37f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The value lives on the layer group's instance in the root generated tree. */
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "Bottom Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*ma->paint_layers_tree, *group, *iface);
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.37f);

  BKE_paint_layers_set_enabled(*ma, layer, false);
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);
}

TEST_F(PaintLayersGenerateTest, copy_does_not_share_generated_tree)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->paint_layers_tree, nullptr);
  EXPECT_NE(copy->paint_layers_tree, ma->paint_layers_tree);
  EXPECT_FALSE(BLI_uuid_equal(copy->paint_layers_owner_uid, ma->paint_layers_owner_uid));

  bNode *copy_instance = nullptr;
  for (bNode &node : copy->nodetree->nodes) {
    if (node.id == &copy->paint_layers_tree->id) {
      copy_instance = &node;
      break;
    }
  }
  EXPECT_NE(copy_instance, nullptr);

  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, foreign_tree_is_replaced_not_overwritten)
{
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
  bNodeTree *foreign = make_tree("Foreign Tree");
  ASSERT_NE(foreign, nullptr);
  ma->paint_layers_tree = foreign;
  add_paint_layer("Bottom", add_image("Bottom"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.replaced_foreign_tree);
  EXPECT_NE(ma->paint_layers_tree, foreign);
  EXPECT_EQ(count_type(*foreign, SH_NODE_TEX_IMAGE), 0);
}

TEST_F(PaintLayersGenerateTest, tagged_regenerate_is_the_single_scheduling_point)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  /* The mutator only marked the description stale; nothing generated it yet. */
  EXPECT_EQ(ma->paint_layers_tree, nullptr);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_paint_layers_regenerate_tagged(*bmain);
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  bNodeTree *tree = ma->paint_layers_tree;

  /* Nothing is pending: a second pass is a no-op and does not rebuild the tree. */
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_tree, tree);

  /* A new edit marks it stale again, and only the scheduling point rebuilds it. */
  add_paint_layer("Top", add_image("Top"));
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), 2);
}

TEST_F(PaintLayersGenerateTest, mask_item_map_builds_its_chain)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &l) { return layer_tree_for(l); };
  ctx.layer_tree_get = layer_tree_factory;

  /* The same layer built without a mask first, so the item's own nodes are measured as a delta. */
  bNodeTree *without_mask = make_tree("Without Mask");
  ASSERT_NE(without_mask, nullptr);
  paint_layers_tree_build(*ma, *without_mask, ctx);
  const int math_without_mask = count_type(*without_mask, SH_NODE_MATH);
  const int mix_without_mask = count_type(*without_mask, SH_NODE_MIX);

  MaterialPaintLayer *item_const = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item_const, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_add(*ma, item_const, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item_const, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Mask")));
  /* A map is read only while the item is a Paint one. */
  ASSERT_TRUE(
      BKE_paint_layers_correction_source_set(*ma, item_const, MA_PAINT_LAYER_SOURCE_IMAGE));
  bNodeTree *with_mask = make_tree("With Mask Map");
  ASSERT_NE(with_mask, nullptr);
  paint_layers_tree_build(*ma, *with_mask, ctx);

  /* The layer's map and the mask item's map. */
  EXPECT_EQ(count_type(*with_mask, SH_NODE_TEX_IMAGE), 2);
  /* The item adds its grey chain and a Mix. */
  EXPECT_GT(count_type(*with_mask, SH_NODE_MATH), math_without_mask);
  EXPECT_GT(count_type(*with_mask, SH_NODE_MIX), mix_without_mask);
}

TEST_F(PaintLayersGenerateTest, constant_mask_item_adds_no_map)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &l) { return layer_tree_for(l); };
  ctx.layer_tree_get = layer_tree_factory;

  bNodeTree *without_mask = make_tree("Without Mask");
  ASSERT_NE(without_mask, nullptr);
  paint_layers_tree_build(*ma, *without_mask, ctx);
  const int mix_without_mask = count_type(*without_mask, SH_NODE_MIX);

  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 0.5f), nullptr);
  bNodeTree *with_mask = make_tree("With Constant Mask");
  ASSERT_NE(with_mask, nullptr);
  paint_layers_tree_build(*ma, *with_mask, ctx);

  /* A constant item adds no map, only its Mix. */
  EXPECT_EQ(count_type(*with_mask, SH_NODE_TEX_IMAGE), 1);
  EXPECT_GT(count_type(*with_mask, SH_NODE_MIX), mix_without_mask);
}

TEST_F(PaintLayersGenerateTest, content_correction_adds_a_mix_over_the_map)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("Correction");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Two layer maps and the correction map, each layer and the correction with one Mix. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 3);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_MIX), 3);
}

/** Give \a image the data colorspace, so a correction map is read as colour data. */
static void make_generate_image_data(Image &image)
{
  BLI_strncpy(image.colorspace_settings.name,
              IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA),
              sizeof(image.colorspace_settings.name));
}

/**
 * F2-C5: a content correction whose map is read as colour data reaches the chain pre-multiplied, so
 * the generator straightens it with a CombineXYZ plus a Vector Math divide. A colour-space map is
 * un-premultiplied by the Image Texture node itself, so no such nodes are built.
 */
TEST_F(PaintLayersGenerateTest, content_correction_data_map_builds_a_straighten_divide)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrectionData");
  make_generate_image_data(*record->image);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_COMBXYZ), 1);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_VECTOR_MATH), 1);
}

TEST_F(PaintLayersGenerateTest, content_correction_color_map_builds_no_straighten_divide)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrectionColor");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_COMBXYZ), 0);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_VECTOR_MATH), 0);
}

TEST_F(PaintLayersGenerateTest, fill_effect_correction_builds_a_constant_mix)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, correction, green));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The layer's map and the channel's bottom constant; the Fill correction's colour comes from a
   * group input, not a node, and each row contributes a Mix. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_RGB), 1);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_MIX), 2);
}

TEST_F(PaintLayersGenerateTest, scene_graph_update_runs_the_scheduling_point)
{
  /* The paths without an event loop -- a script's depsgraph.update(), a background render -- reach
   * the scheduling point through BKE_scene_graph_update_tagged, not through the WM. */
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ASSERT_NE(scene, nullptr);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  ASSERT_NE(view_layer, nullptr);
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
  ASSERT_NE(depsgraph, nullptr);

  add_paint_layer("Bottom", add_image("Bottom"));
  EXPECT_EQ(ma->paint_layers_tree, nullptr);

  BKE_scene_graph_update_tagged(depsgraph, bmain);
  EXPECT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersGenerateTest, regenerate_tagged_is_main_thread_only)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  EXPECT_EQ(ma->paint_layers_tree, nullptr);

  std::thread worker([this]() { BKE_paint_layers_regenerate_tagged(*bmain); });
  worker.join();

  /* Rule K-1: a worker thread must not create IDs or write node trees into Main. */
  EXPECT_EQ(ma->paint_layers_tree, nullptr);
}

TEST_F(PaintLayersGenerateTest, localized_copy_is_not_regenerated)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *tree = ma->paint_layers_tree;
  ASSERT_NE(tree, nullptr);
  const int images = count_type(*tree, SH_NODE_TEX_IMAGE);

  Material *copy = id_cast<Material *>(BKE_id_copy_ex(bmain,
                                                      &ma->id,
                                                      nullptr,
                                                      LIB_ID_COPY_DEFAULT | LIB_ID_CREATE_LOCAL |
                                                          LIB_ID_CREATE_NO_USER_REFCOUNT));
  ASSERT_NE(copy, nullptr);
  /* A localized copy shares the tree; regenerating it would rewrite the original. */
  ASSERT_EQ(copy->paint_layers_tree, tree);
  copy->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;

  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), images);
  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, set_enabled_is_a_value_edit)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_paint_layers_values_sync(*ma);
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "Bottom Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*ma->paint_layers_tree, *group, *iface);
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);
}

TEST_F(PaintLayersGenerateTest, constant_mask_value_is_a_value_edit)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.5f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  const float quarter[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, item, quarter));
  /* A constant item's colour is a group input: value-only, no rebuild. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0);
}

TEST_F(PaintLayersGenerateTest, correction_opacity_is_synced_without_regen)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, correction, green));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  /* Changing the value does not mark the topology stale. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.25f));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_paint_layers_values_sync(*ma);
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "Bottom C Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*ma->paint_layers_tree, *group, *iface);
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.25f);
}

TEST_F(PaintLayersGenerateTest, mask_correction_builds_a_factor_chain)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
  ASSERT_NE(correction, nullptr);
  /* A mask correction lays coverage with the over formula; no blend mode takes part. */
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("MaskCorrection");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The chain builds and updates without a malformed graph, and the channel is still wired. */
  EXPECT_NE(interface_output_find("Result Base Color"), nullptr);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 3);
}

TEST_F(PaintLayersGenerateTest, pure_build_instantiates_normal_combine_with_links)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  /* A content Paint correction on the Normal channel is a second Combine instance. */
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  add_channel(*correction, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Correction"));

  bNodeTree *tree = make_tree("PBR Layers Normal");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(*bmain);
  ASSERT_NE(ctx.normal_combine_group, nullptr);
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  /* No #Main reachable from the build itself: the group sockets come from the node declaration. */
  paint_layers_tree_build(*ma, *tree, ctx);

  /* The combines live inside the layer groups now, so walk the tree recursively. */
  int combines = 0;
  auto visit = [&](auto &self, bNodeTree &t) -> void {
    t.ensure_topology_cache();
    for (bNode &node : t.nodes) {
      if (BKE_paint_material_is_normal_combine_group(node)) {
        combines++;
        bNodeSocket *a = bke::node_find_socket(
            node, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
        ASSERT_NE(a, nullptr);
        EXPECT_FALSE(a->directly_linked_links().is_empty());
      }
      else if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
        self(self, *reinterpret_cast<bNodeTree *>(node.id));
      }
    }
  };
  visit(visit, *tree);
  /* One Combine for the row itself, one for its content correction. */
  EXPECT_EQ(combines, 2);
}

TEST_F(PaintLayersGenerateTest, pure_build_matches_regenerate_topology)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_channel(*bottom, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  add_channel(*top, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("TopNormal"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
  ASSERT_NE(correction, nullptr);
  add_channel(*correction, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("MaskCorr"));

  PaintLayersBuildContext ctx;
  ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(*bmain);
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  bNodeTree *pure = make_tree("PBR Layers Pure");
  ASSERT_NE(pure, nullptr);
  paint_layers_tree_build(*ma, *pure, ctx);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  auto idname_multiset = [](const bNodeTree &tree) {
    Vector<std::string> names;
    for (const bNode &node : tree.nodes) {
      names.append(std::string(node.idname));
    }
    std::sort(names.begin(), names.end());
    return names;
  };
  const Vector<std::string> pure_names = idname_multiset(*pure);
  const Vector<std::string> regen_names = idname_multiset(*ma->paint_layers_tree);
  EXPECT_EQ(pure_names, regen_names);
}

TEST_F(PaintLayersGenerateTest, leaf_group_has_the_layer_contract)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom, nullptr);
  ASSERT_NE(top, nullptr);

  for (bNodeTree *group : {bottom, top}) {
    EXPECT_TRUE(interface_has_socket(*group, "Below Base Color", false));
    EXPECT_TRUE(interface_has_socket(*group, "Color Base Color", true));
    EXPECT_TRUE(interface_has_socket(*group, "Coverage Base Color", true));
    EXPECT_TRUE(interface_has_socket(*group, "Blend Base Color", true));
    EXPECT_TRUE(interface_has_socket(*group, "Result Base Color", true));
    /* The tree is marked as that layer's own group. */
    const IDProperty *props = IDP_GetProperties(&group->id);
    ASSERT_NE(props, nullptr);
    EXPECT_NE(IDP_GetPropertyTypeFromGroup(props, "pbr_paint_layers_layer_tree", IDP_STRING),
              nullptr);
  }

  /* One instance per layer, and nothing else, in the root tree. */
  int instances = 0;
  for (bNode &node : ma->paint_layers_tree->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        (node.id == &bottom->id || node.id == &top->id))
    {
      instances++;
    }
  }
  EXPECT_EQ(instances, 2);
}

TEST_F(PaintLayersGenerateTest, regenerate_reuses_layer_trees)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom, nullptr);
  ASSERT_NE(top, nullptr);
  EXPECT_EQ(layer_tree_count(*bmain), 2);
  EXPECT_EQ(ID_REAL_USERS(&bottom->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&top->id), 1);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom);
  EXPECT_EQ(layer_tree_find(*bmain, "Top"), top);
  EXPECT_EQ(layer_tree_count(*bmain), 2);
  EXPECT_EQ(ID_REAL_USERS(&bottom->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&top->id), 1);
}

TEST_F(PaintLayersGenerateTest, removed_layer_drops_its_tree)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(layer_tree_find(*bmain, "Bottom"), nullptr);
  ASSERT_NE(top_tree, nullptr);

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, bottom));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), nullptr);
  EXPECT_EQ(layer_tree_count(*bmain), 1);
  EXPECT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
}

TEST_F(PaintLayersGenerateTest, renamed_layer_keeps_its_tree)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);

  ASSERT_TRUE(BKE_paint_layers_rename(*ma, bottom, "Renamed"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Renamed"), bottom_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), nullptr);
}

TEST_F(PaintLayersGenerateTest, copied_material_owns_its_layer_trees)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  bNodeTree *original_root = ma->paint_layers_tree;
  const int original_images = count_type(*original_root, SH_NODE_TEX_IMAGE);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->paint_layers_tree, nullptr);
  EXPECT_NE(copy->paint_layers_tree, original_root);

  /* The copy's layer tree is its own, marked with the copy's owner. */
  bNodeTree *copy_bottom = nullptr;
  for (bNode &node : copy->paint_layers_tree->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        node.id != &copy->paint_layers_tree->id)
    {
      copy_bottom = id_cast<bNodeTree *>(node.id);
    }
  }
  ASSERT_NE(copy_bottom, nullptr);
  EXPECT_NE(copy_bottom, bottom_tree);
  const IDProperty *props = IDP_GetProperties(&copy_bottom->id);
  ASSERT_NE(props, nullptr);
  const IDProperty *owner = IDP_GetPropertyTypeFromGroup(
      props, "pbr_paint_layers_owner", IDP_STRING);
  ASSERT_NE(owner, nullptr);
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, copy->paint_layers_owner_uid);
  EXPECT_STREQ(IDP_string_get(owner), formatted);

  /* Regenerating the copy leaves the original's tree and groups untouched. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *copy));
  EXPECT_EQ(ma->paint_layers_tree, original_root);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  EXPECT_EQ(count_type(*original_root, SH_NODE_TEX_IMAGE), original_images);

  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, folder_group_holds_its_children)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "ChildA", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *child_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "ChildB", folder, PaintLayerPlace::Into);
  ASSERT_NE(child_a, nullptr);
  ASSERT_NE(child_b, nullptr);
  add_channel(*child_a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ChildA"));
  add_channel(*child_b, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ChildB"));
  /* Opacity below one keeps the folder isolating; a default folder would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  bNodeTree *child_a_tree = layer_tree_find(*bmain, "ChildA");
  bNodeTree *child_b_tree = layer_tree_find(*bmain, "ChildB");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_NE(child_a_tree, nullptr);
  ASSERT_NE(child_b_tree, nullptr);

  /* The folder exposes the same contract as a leaf. */
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Below Base Color", false));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Color Base Color", true));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Coverage Base Color", true));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Blend Base Color", true));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Result Base Color", true));

  /* Children instantiated inside the folder, the folder in the root, and no child in the root. */
  EXPECT_NE(group_instance_find(*folder_tree, *child_a_tree), nullptr);
  EXPECT_NE(group_instance_find(*folder_tree, *child_b_tree), nullptr);
  EXPECT_NE(group_instance_find(*ma->paint_layers_tree, *folder_tree), nullptr);
  EXPECT_EQ(group_instance_find(*ma->paint_layers_tree, *child_a_tree), nullptr);
  EXPECT_EQ(group_instance_find(*ma->paint_layers_tree, *child_b_tree), nullptr);
  EXPECT_EQ(layer_group_tree_count(*bmain), 3);
}

TEST_F(PaintLayersGenerateTest, regenerate_reuses_folder_trees)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating; a default folder would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  bNodeTree *child_tree = layer_tree_find(*bmain, "Child");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_NE(child_tree, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), folder_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Child"), child_tree);
  EXPECT_EQ(layer_group_tree_count(*bmain), 2);
  EXPECT_EQ(ID_REAL_USERS(&folder_tree->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&child_tree->id), 1);
}

TEST_F(PaintLayersGenerateTest, removed_folder_drops_its_subtree)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating; a default folder would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(folder_tree_find(*bmain, "Folder"), nullptr);
  ASSERT_NE(layer_tree_find(*bmain, "Child"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(layer_tree_find(*bmain, "Child"), nullptr);
  EXPECT_EQ(layer_group_tree_count(*bmain), 0);
}

TEST_F(PaintLayersGenerateTest, copied_material_owns_nested_trees)
{
  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Leaf", inner, PaintLayerPlace::Into);
  add_channel(*leaf, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Leaf"));
  /* Opacity below one keeps both folders isolating; default folders would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *outer_tree = folder_tree_find(*bmain, "Outer");
  bNodeTree *inner_tree = folder_tree_find(*bmain, "Inner");
  bNodeTree *leaf_tree = layer_tree_find(*bmain, "Leaf");
  ASSERT_NE(outer_tree, nullptr);
  ASSERT_NE(inner_tree, nullptr);
  ASSERT_NE(leaf_tree, nullptr);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);

  Vector<bNodeTree *> level1;
  collect_group_instances(*copy->paint_layers_tree, level1);
  ASSERT_EQ(level1.size(), 1);
  bNodeTree *copy_outer = level1[0];
  Vector<bNodeTree *> level2;
  collect_group_instances(*copy_outer, level2);
  ASSERT_EQ(level2.size(), 1);
  bNodeTree *copy_inner = level2[0];
  Vector<bNodeTree *> level3;
  collect_group_instances(*copy_inner, level3);
  ASSERT_EQ(level3.size(), 1);
  bNodeTree *copy_leaf = level3[0];

  EXPECT_NE(copy_outer, outer_tree);
  EXPECT_NE(copy_inner, inner_tree);
  EXPECT_NE(copy_leaf, leaf_tree);
  const IDProperty *props = IDP_GetProperties(&copy_leaf->id);
  ASSERT_NE(props, nullptr);
  EXPECT_NE(IDP_GetPropertyTypeFromGroup(props, "pbr_paint_layers_layer_tree", IDP_STRING),
            nullptr);

  /* Regenerating the copy leaves the original's nested trees alone. */
  const int original_images = count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *copy));
  EXPECT_EQ(folder_tree_find(*bmain, "Outer"), outer_tree);
  EXPECT_EQ(folder_tree_find(*bmain, "Inner"), inner_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Leaf"), leaf_tree);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), original_images);

  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, baked_row_is_its_own_group)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*layer);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("BakedColor");
  bake->coverage = add_image("BakedCoverage");
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  Image *substituted = nullptr;
  ASSERT_TRUE(BKE_paint_layers_bake_substitute(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &substituted));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(interface_has_socket(*group, "Below Base Color", false));
  EXPECT_TRUE(interface_has_socket(*group, "Result Base Color", true));
  EXPECT_NE(group_instance_find(*ma->paint_layers_tree, *group), nullptr);
  /* The bake's colour and coverage stand in: two Image Texture nodes, not the live map. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 2);
}

TEST_F(PaintLayersGenerateTest, root_holds_only_instances_and_chain_ends)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Base Color only: the root keeps the group I/O, the channel's bottom constant and the group
   * instances. A Normal channel would add its decode-normalize-encode Vector Math at the end.
   * F2-B adds the Base Color content-alpha chain (Value/Math) and its final Compose Color Alpha
   * before the Result output. */
  for (bNode &node : ma->paint_layers_tree->nodes) {
    const bool allowed = node.is_group_input() || node.is_group_output() || node.is_group() ||
                         node.type_legacy == SH_NODE_RGB || node.type_legacy == SH_NODE_VALUE ||
                         node.type_legacy == SH_NODE_MATH ||
                         node.type_legacy == SH_NODE_COMPOSE_COLOR_ALPHA;
    EXPECT_TRUE(allowed) << node.idname;
  }
}

TEST_F(PaintLayersGenerateTest, unchanged_layer_tree_is_not_rebuilt)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom, nullptr);
  const int nodes_before = node_count(*bottom);
  Vector<bNode *> pointers_before;
  for (bNode &node : bottom->nodes) {
    pointers_before.append(&node);
  }
  ASSERT_TRUE(group_mix_sentinel_set(*bottom, 0.125f));

  /* A regenerate without any edit must leave the layer group's nodes alone. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_after = layer_tree_find(*bmain, "Bottom");
  ASSERT_EQ(bottom_after, bottom);
  EXPECT_EQ(node_count(*bottom_after), nodes_before);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_after, 0.125f));
  Vector<bNode *> pointers_after;
  for (bNode &node : bottom_after->nodes) {
    pointers_after.append(&node);
  }
  ASSERT_EQ(pointers_after.size(), pointers_before.size());
  for (const int i : pointers_before.index_range()) {
    EXPECT_EQ(pointers_after[i], pointers_before[i]);
  }
}

TEST_F(PaintLayersGenerateTest, value_edit_does_not_rebuild_any_group)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(fill_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*fill_tree, 0.5f));

  /* Opacity and a Fill constant are group inputs: they change values, not topology. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, bottom, 0.42f));
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, green));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Fill"), fill_tree);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*fill_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, map_change_rebuilds_only_its_layer)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, bottom, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("BottomReplaced")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  ASSERT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
  /* The map change rebuilds Bottom's group (the stamp is gone) but not Top's. */
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, correction_add_rebuilds_only_its_layer)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Correction")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  ASSERT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
  /* The new effect changes Bottom's topology only. */
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, folder_mask_keeps_the_root_and_syncs)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating, so adding the mask only grows its interface. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *child_tree = layer_tree_find(*bmain, "Child");
  ASSERT_NE(child_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*child_tree, 0.25f));

  /* A mask on the folder grows the folder's own interface; the root must be kept. */
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, folder, 0.5f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  /* The mask's value input lives on the folder group, and the child group is untouched. */
  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_tree, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(
      *folder_tree, "Folder Mask Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *instance_in = group_instance_input(*root, *folder_tree, *iface);
  ASSERT_NE(instance_in, nullptr);
  EXPECT_TRUE(group_mix_sentinel_get(*child_tree, 0.25f));
}

TEST_F(PaintLayersGenerateTest, root_interface_has_no_value_inputs)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  /* The root carries only Result outputs; values live on the layer groups (session 10e). */
  ma->paint_layers_tree->ensure_interface_cache();
  EXPECT_TRUE(ma->paint_layers_tree->interface_inputs().is_empty());
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Result Base Color", true));
  EXPECT_FALSE(interface_has_socket(*ma->paint_layers_tree, "Below Base Color", false));
}

TEST_F(PaintLayersGenerateTest, layer_group_owns_its_value_inputs)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  BKE_paint_layers_set_opacity(*ma, layer, 0.5f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *opacity = group_input_find(*group, "Bottom Base Color Opacity");
  ASSERT_NE(opacity, nullptr);
  ASSERT_NE(opacity->properties, nullptr);
  const IDProperty *role = IDP_GetPropertyTypeFromGroup(
      opacity->properties, "pbr_paint_layers_role", IDP_STRING);
  ASSERT_NE(role, nullptr);
  EXPECT_STREQ(IDP_string_get(role), "opacity");
  const IDProperty *channel = IDP_GetPropertyTypeFromGroup(
      opacity->properties, "pbr_paint_layers_channel", IDP_INT);
  ASSERT_NE(channel, nullptr);
  EXPECT_EQ(IDP_int_get(channel), PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NE(IDP_GetPropertyTypeFromGroup(
                opacity->properties, "pbr_paint_layers_layer", IDP_STRING),
            nullptr);
}

TEST_F(PaintLayersGenerateTest, values_sync_writes_to_the_instance)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.42f));
  BKE_paint_layers_values_sync(*ma);

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "Bottom Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*ma->paint_layers_tree, *group, *iface);
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.42f);
}

TEST_F(PaintLayersGenerateTest, values_sync_writes_to_a_nested_instance)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating, so the child instance lives in its tree. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, child, 0.33f));
  BKE_paint_layers_values_sync(*ma);

  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  bNodeTree *child_tree = layer_tree_find(*bmain, "Child");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_NE(child_tree, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*child_tree, "Child Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  /* The child's value lives on its instance inside the folder's tree, not in the root. */
  bNodeSocket *socket = group_instance_input(*folder_tree, *child_tree, *iface);
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.33f);
}

TEST_F(PaintLayersGenerateTest, reorder_rebuilds_no_group)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  /* Swapping the rows changes only the root's links, never a layer group's topology. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, bottom, 1));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  ASSERT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, map_change_keeps_the_root)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));

  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, bottom, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("BottomReplaced")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  /* The changed group's nodes are new; the root was left alone. */
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
}

TEST_F(PaintLayersGenerateTest, correction_add_keeps_the_root)
{
  /* The effect's value inputs now live on the owner layer's own group (session 10e), so adding an
   * effect rebuilds only that group. The root's nodes, links and interface are untouched. */
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(
      BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The root is kept; only the owner's group was rebuilt. */
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
  /* The correction's value input lives on the owner's group and was synced onto its instance. */
  bNode *instance = group_instance_find(*root, *bottom_tree);
  ASSERT_NE(instance, nullptr);
  bool found_value_input = false;
  bottom_tree->ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : bottom_tree->interface_inputs()) {
    if (socket->name != nullptr && STREQ(socket->name, "Bottom C Base Color Opacity")) {
      found_value_input = true;
      EXPECT_NE(bke::node_find_socket(
                    *instance, SOCK_IN, UString::from_ptr_noinline(socket->identifier)),
                nullptr);
    }
  }
  EXPECT_TRUE(found_value_input);
}

TEST_F(PaintLayersGenerateTest, reorder_rebuilds_the_root)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, bottom, 1));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
  /* Only the root's links changed; the groups' contents are untouched. */
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, layer_add_rebuilds_the_root)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));

  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
  /* The neighbour's group is untouched. */
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
}

TEST_F(PaintLayersGenerateTest, channel_add_rebuilds_the_root)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  /* A new channel changes every layer's hash and the root's interface. */
  ASSERT_NE(add_channel(*bottom, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("BottomNormal")),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_FALSE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, value_edit_rebuilds_nothing)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, bottom, 0.37f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
}

/**
 * TZ-26 note: this test used to assert the live constant was built as an #SH_NODE_RGB node with
 * the value baked into its default (`EXPECT_GE(count_type(*group, SH_NODE_RGB), 1)`). The value is
 * now a group input instead (#create_value_inputs, role #ROLE_LIVE_CONSTANT / "live_constant"), so
 * it travels through #BKE_paint_layers_values_sync like opacity and Fill rather than being baked
 * into the row's topology -- this is the change TZ-26 asks for, so the assertion is rewritten to
 * check for the new mechanism (the interface socket and its value) instead of the old one.
 */
TEST_F(PaintLayersGenerateTest, live_material_constant_builds_a_group_input)
{
  Material *source = add_principled_source("LiveSource", 0.42f);
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* A baked map for the row, so the non-live path has something to show. */
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("SourceBake")));
  BKE_paint_layers_bake_finalize(*ma, *row);
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  /* Live: the source's constant is a group input, no RGB node baked into the topology, and the
   * row's baked map is not read. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 0);
  bNodeTreeInterfaceSocket *live_input = group_input_find(*group, "Source Roughness Source");
  ASSERT_NE(live_input, nullptr);
  ASSERT_NE(live_input->properties, nullptr);
  const IDProperty *role = IDP_GetPropertyTypeFromGroup(
      live_input->properties, "pbr_paint_layers_role", IDP_STRING);
  ASSERT_NE(role, nullptr);
  EXPECT_STREQ(IDP_string_get(role), "live_constant");
  ASSERT_NE(live_input->socket_data, nullptr);
  const float *value = static_cast<bNodeSocketValueRGBA *>(live_input->socket_data)->value;
  EXPECT_NEAR(value[0], 0.42f, 1e-4f);

  /* Leaving the row rebuilds its group on the baked map. */
  BKE_paint_layers_active_set(*ma, bottom->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 1);
}

/**
 * TZ-26: this test used to be named `live_material_constant_is_topology_but_keeps_the_root` and
 * asserted the opposite of what it now checks -- moving the source's constant used to rebuild the
 * row's own group (the stamp was gone) while leaving the root alone. The live constant is a value
 * now (#ROLE_LIVE_CONSTANT group input, synced by #values_sync_socket), not topology, so the fix
 * this TZ asks for is exactly that the row's group must survive the edit too; the name and the body
 * are rewritten together to match.
 */
TEST_F(PaintLayersGenerateTest, live_material_constant_edit_syncs_without_rebuild)
{
  Material *source = add_principled_source("LiveSource", 0.1f);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.125f));

  /* Move the source's constant: it is now a value the row's group reads through a group input, not
   * topology, so it must sync in place. */
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.9f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Neither the row's group nor the root was rebuilt: the sentinel and the node lists survive. */
  ASSERT_EQ(layer_tree_find(*bmain, "Source"), group);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));

  /* The new value reached the row's group input through the sync at the end of regeneration. */
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "Source Roughness Source");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*ma->paint_layers_tree, *group, *iface);
  ASSERT_NE(socket, nullptr);
  const float *value = static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value;
  EXPECT_NEAR(value[0], 0.9f, 1e-4f);
}

TEST_F(PaintLayersGenerateTest, material_row_participation_is_stable_across_focus)
{
  Material *source = add_principled_source("StableSource", 0.3f);
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.25f));

  auto output_names = [](bNodeTree &tree) {
    tree.ensure_interface_cache();
    Vector<std::string> names;
    for (bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
      if (socket->name != nullptr) {
        names.append(std::string(socket->name));
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  };
  const Vector<std::string> outputs_active = output_names(*root);

  /* Leave the row: it has no baked maps, so the source constant still carries it. The channel set,
   * the root hash and the row's own topology must all be the same as while it was active. */
  BKE_paint_layers_active_set(*ma, bottom->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_EQ(output_names(*root), outputs_active);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.25f));
}

TEST_F(PaintLayersGenerateTest, live_material_image_uses_the_source_texture)
{
  Material *source = add_principled_source("LiveImageSource", 0.3f);
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  Image *source_map = add_image("LiveSourceTexture");
  bNode *texture = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  texture->id = &source_map->id;
  bke::node_add_link(*source->nodetree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  NodeTexImage *source_storage = static_cast<NodeTexImage *>(texture->storage);
  ASSERT_NE(source_storage, nullptr);
  source_storage->projection = SHD_PROJ_FLAT;
  source_storage->interpolation = SHD_INTERP_CLOSEST;
  source_storage->extension = SHD_IMAGE_EXTENSION_CLIP;

  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* A baked map for Base Color, so the non-live path would show that instead. */
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("LiveSourceBaked")));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  bNode *map = find_type(*group, SH_NODE_TEX_IMAGE);
  ASSERT_NE(map, nullptr);
  /* The source's own texture, not the baked map, and its sampling settings travelled with it. */
  EXPECT_EQ(map->id, &source_map->id);
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(map->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->interpolation, SHD_INTERP_CLOSEST);
  EXPECT_EQ(storage->extension, SHD_IMAGE_EXTENSION_CLIP);
}

namespace {

/** The `pbr_paint_layers_role` of output \a name in \a group, or null. */
const char *output_role(bNodeTree &group, const char *name)
{
  group.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->name == nullptr || !STREQ(socket->name, name) || socket->properties == nullptr) {
      continue;
    }
    const IDProperty *role = IDP_GetPropertyTypeFromGroup(
        socket->properties, "pbr_custom_role", IDP_STRING);
    if (role != nullptr) {
      return IDP_string_get(role);
    }
  }
  return nullptr;
}

/** The output interface identifiers of \a group, sorted; equal sets mean a preserved interface. */
Vector<std::string> output_identifiers(bNodeTree &group)
{
  group.ensure_interface_cache();
  Vector<std::string> ids;
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->identifier != nullptr) {
      ids.append(std::string(socket->identifier));
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

/** The wrapper tree named `.PL Source <name>`, or null. */
bNodeTree *source_wrapper_find(Main &bmain, const char *source_name)
{
  char full[128];
  BLI_snprintf(full, sizeof(full), ".PL Source %s", source_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, full)) {
      return &tree;
    }
  }
  return nullptr;
}

/**
 * A source whose Principled lives inside a nested group and whose Base Color is a Noise graph, so
 * the mode is SourceGroup but the wrapper factory refuses it (PrincipledInGroup).
 */
bNodeTreeInterfaceSocket *tree_interface_output_find(bNodeTree &tree, const char *name)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, name)) {
      return socket;
    }
  }
  return nullptr;
}

/** A group tree holding a Principled (Base Color from Noise) feeding a "Surface" output. */
bNodeTree *make_principled_group(Main &bmain, const char *tree_name)
{
  bNodeTree *group = bke::node_tree_add_tree(&bmain, tree_name, "ShaderNodeTree");
  group->tree_interface.add_socket(
      "Surface", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNode *principled = bke::node_add_static_node(nullptr, *group, SH_NODE_BSDF_PRINCIPLED);
  bNode *noise = bke::node_add_static_node(nullptr, *group, SH_NODE_TEX_NOISE);
  bNode *output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  bke::node_add_link(*group,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  bNodeTreeInterfaceSocket *surface = tree_interface_output_find(*group, "Surface");
  BLI_assert(surface != nullptr);
  bke::node_add_link(
      *group,
      *principled,
      *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
      *output,
      *bke::node_find_socket(*output, SOCK_IN, UString::from_ptr_noinline(surface->identifier)));
  return group;
}

/** Wrap \a inner in a new group that passes its "Surface" through. */
bNodeTree *wrap_principled_group(Main &bmain, const char *tree_name, bNodeTree &inner)
{
  bNodeTree *outer = bke::node_tree_add_tree(&bmain, tree_name, "ShaderNodeTree");
  outer->tree_interface.add_socket(
      "Surface", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNode *instance = bke::node_add_node(nullptr, *outer, inner.typeinfo->group_idname);
  instance->id = &inner.id;
  id_us_plus(&inner.id);
  nodes::update_node_declaration_and_sockets(*outer, *instance);
  bNode *output = bke::node_add_node(nullptr, *outer, "NodeGroupOutput"_ustr);
  BKE_ntree_update_tag_all(outer);
  BKE_ntree_update_after_single_tree_change(bmain, *outer);
  bNodeTreeInterfaceSocket *outer_surface = tree_interface_output_find(*outer, "Surface");
  bNodeTreeInterfaceSocket *inner_surface = tree_interface_output_find(inner, "Surface");
  BLI_assert(outer_surface != nullptr && inner_surface != nullptr);
  bke::node_add_link(
      *outer,
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(inner_surface->identifier)),
      *output,
      *bke::node_find_socket(
          *output, SOCK_IN, UString::from_ptr_noinline(outer_surface->identifier)));
  return outer;
}

/** A source whose Principled is \a depth groups deep, with a Noise on Base Color. */
Material *make_nested_principled_source_depth(Main &bmain, const char *name, const int depth)
{
  bNodeTree *group = make_principled_group(bmain, "NestedSourceBase");
  for (int i = 1; i < depth; i++) {
    char tree_name[64];
    BLI_snprintf(tree_name, sizeof(tree_name), "NestedSourceLevel%d", i);
    group = wrap_principled_group(bmain, tree_name, *group);
  }
  Material *source = BKE_material_add(&bmain, name);
  bNode *instance = bke::node_add_node(nullptr, *source->nodetree, group->typeinfo->group_idname);
  instance->id = &group->id;
  id_us_plus(&group->id);
  nodes::update_node_declaration_and_sockets(*source->nodetree, *instance);
  bNode *output_node = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_OUTPUT_MATERIAL);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);
  bNodeTreeInterfaceSocket *surface = tree_interface_output_find(*group, "Surface");
  BLI_assert(surface != nullptr);
  bke::node_add_link(
      *source->nodetree,
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(surface->identifier)),
      *output_node,
      *bke::node_find_socket(*output_node, SOCK_IN, "Surface"_ustr));
  return source;
}

Material *make_nested_principled_source(Main &bmain, const char *name)
{
  return make_nested_principled_source_depth(bmain, name, 1);
}

/** The output interface names of \a tree, sorted; equal sets mean a preserved interface. */
Vector<std::string> output_names(bNodeTree &tree)
{
  tree.ensure_interface_cache();
  Vector<std::string> names;
  for (bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    if (socket->name != nullptr) {
      names.append(std::string(socket->name));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

/** How many source wrapper or path-copy trees live in \a bmain. */
int source_wrapper_tree_count(Main &bmain)
{
  int count = 0;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (StringRef(tree.id.name + 2).startswith(".PL Source ")) {
      count++;
    }
  }
  return count;
}

/** Every source wrapper or path-copy tree in \a bmain. */
Vector<bNodeTree *> source_wrapper_trees(Main &bmain)
{
  Vector<bNodeTree *> trees;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (StringRef(tree.id.name + 2).startswith(".PL Source ")) {
      trees.append(&tree);
    }
  }
  return trees;
}

/** Whether every node of \a tree whose parent is set has that parent inside the same tree. */
bool parents_are_local(const bNodeTree &tree)
{
  for (const bNode &node : tree.nodes) {
    if (node.parent == nullptr) {
      continue;
    }
    bool found = false;
    for (const bNode &candidate : tree.nodes) {
      if (&candidate == node.parent) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

/** Give \a source's Base Color a Noise graph, so its row goes to SourceGroup mode. */
void source_set_noise_base_color(Main &bmain, Material &source)
{
  bNode *principled = nullptr;
  for (bNode &node : source.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  BLI_assert(principled != nullptr);
  bNode *noise = bke::node_add_static_node(nullptr, *source.nodetree, SH_NODE_TEX_NOISE);
  bke::node_add_link(*source.nodetree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  BKE_ntree_update_tag_all(source.nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source.nodetree);
}

/** The node of \a tree named \a name, or null; a copied wrapper preserves node names. */
bNode *node_by_name(bNodeTree &tree, const StringRefNull name)
{
  for (bNode &node : tree.nodes) {
    if (node.name == name) {
      return &node;
    }
  }
  return nullptr;
}

/** The node feeding the Vector input of \a tex, or null when nothing is linked. */
bNode *vector_source(bNode &tex)
{
  bNodeSocket *vector = bke::node_find_socket(tex, SOCK_IN, "Vector"_ustr);
  if (vector == nullptr) {
    return nullptr;
  }
  const Span<const bNodeLink *> links = vector->directly_linked_links();
  if (links.is_empty()) {
    return nullptr;
  }
  return links[0]->fromnode;
}

/** The UV layer name a UV Map node reads, or "". */
const char *uv_node_layer(const bNode &node)
{
  const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(node.storage);
  return (storage != nullptr) ? storage->uv_map : "";
}

}  // namespace

TEST_F(PaintLayersGenerateTest, source_group_uv_wires_unlinked_textures_only)
{
  Material *source = add_principled_source("UvWrapSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  bNodeTree &src_tree = *source->nodetree;

  /* A texture with no Vector link must sample the named layer; one already wired to its own UV Map
   * node is left alone. */
  bNode *tex_open = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_linked = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *src_uv = bke::node_add_static_node(nullptr, src_tree, SH_NODE_UVMAP);
  ASSERT_NE(tex_open, nullptr);
  ASSERT_NE(tex_linked, nullptr);
  ASSERT_NE(src_uv, nullptr);
  BLI_strncpy(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map,
              "SourceUV",
              sizeof(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map));
  bke::node_add_link(src_tree,
                     *src_uv,
                     *bke::node_find_socket(*src_uv, SOCK_OUT, "UV"_ustr),
                     *tex_linked,
                     *bke::node_find_socket(*tex_linked, SOCK_IN, "Vector"_ustr));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  /* The links the wrapper build added are only visible after its runtime cache is rebuilt. */
  group->ensure_topology_cache();

  bNode *copied_open = node_by_name(*group, tex_open->name);
  bNode *copied_linked = node_by_name(*group, tex_linked->name);
  ASSERT_NE(copied_open, nullptr);
  ASSERT_NE(copied_linked, nullptr);
  bNode *open_source = vector_source(*copied_open);
  bNode *linked_source = vector_source(*copied_linked);
  ASSERT_NE(open_source, nullptr);
  ASSERT_NE(linked_source, nullptr);
  EXPECT_EQ(open_source->type_legacy, SH_NODE_UVMAP);
  EXPECT_STREQ(uv_node_layer(*open_source), "UVMap");
  /* The already-wired texture keeps its own UV Map node untouched. */
  EXPECT_EQ(linked_source->type_legacy, SH_NODE_UVMAP);
  EXPECT_STREQ(uv_node_layer(*linked_source), "SourceUV");

  /* The source material itself is never written to: its open texture stays open. */
  EXPECT_EQ(vector_source(*tex_open), nullptr);
}

TEST_F(PaintLayersGenerateTest, source_group_uv_name_change_rebuilds_the_wrapper)
{
  Material *source = add_principled_source("UvRebuildSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  bNode *tex = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex, nullptr);

  BLI_strncpy(ma->paint_layers_uv_map, "First", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();
  bNode *first_uv = vector_source(*node_by_name(*group, tex->name));
  ASSERT_NE(first_uv, nullptr);
  EXPECT_STREQ(uv_node_layer(*first_uv), "First");

  /* The same name keeps the wrapper and its nodes. */
  bNodeTree *again = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  EXPECT_EQ(again, group);
  group->ensure_topology_cache();
  EXPECT_EQ(vector_source(*node_by_name(*group, tex->name)), first_uv);

  /* A new name is topology: the wrapper rebuilds and the UV Map node names the new layer. */
  BLI_strncpy(ma->paint_layers_uv_map, "Second", sizeof(ma->paint_layers_uv_map));
  bNodeTree *rebuilt = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  EXPECT_EQ(rebuilt, group);
  group->ensure_topology_cache();
  bNode *second_uv = vector_source(*node_by_name(*group, tex->name));
  ASSERT_NE(second_uv, nullptr);
  EXPECT_STREQ(uv_node_layer(*second_uv), "Second");
}

/* GUARD: an empty UV name adds no node and leaves textures open. */
TEST_F(PaintLayersGenerateTest, source_group_uv_empty_name_adds_no_node)
{
  Material *source = add_principled_source("UvEmptySource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  bNode *tex = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex, nullptr);

  BLI_strncpy(ma->paint_layers_uv_map, "", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();
  EXPECT_EQ(vector_source(*node_by_name(*group, tex->name)), nullptr);
}

TEST_F(PaintLayersGenerateTest, source_group_wraps_principled_channels)
{
  Material *source = add_principled_source("WrapSource", 0.3f);
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  bNodeTree &source_tree = *source->nodetree;
  bNode *noise = bke::node_add_static_node(nullptr, source_tree, SH_NODE_TEX_NOISE);
  bNode *ramp = bke::node_add_static_node(nullptr, source_tree, SH_NODE_VALTORGB);
  ASSERT_NE(noise, nullptr);
  ASSERT_NE(ramp, nullptr);
  /* The chain the wrapper has to bring along: Noise -> ColorRamp -> Base Color. */
  bke::node_add_link(source_tree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *ramp,
                     *bke::node_find_socket(*ramp, SOCK_IN, "Fac"_ustr));
  bke::node_add_link(source_tree,
                     *ramp,
                     *bke::node_find_socket(*ramp, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  EXPECT_STREQ(group->id.name + 2, ".PL Source WrapSource");
  /* The Material Output is replaced by the group's own contract. */
  EXPECT_EQ(count_type(*group, SH_NODE_OUTPUT_MATERIAL), 0);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_NOISE), 1);
  EXPECT_EQ(count_type(*group, SH_NODE_VALTORGB), 1);
  EXPECT_STREQ(output_role(*group, "Base Color"), "COLOR:BASE_COLOR");
  EXPECT_STREQ(output_role(*group, "Roughness"), "COLOR:ROUGHNESS");
  /* The Principled has an Alpha input, so coverage is exposed too. */
  EXPECT_STREQ(output_role(*group, "Coverage"), "COVERAGE");
}

TEST_F(PaintLayersGenerateTest, source_group_is_cached_and_rebuilt_in_place)
{
  Material *source = add_principled_source("CacheSource", 0.3f);
  PaintLayersSourceGroupRefusal refusal;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> nodes_before = root_nodes(*group);
  const Vector<std::string> ids_before = output_identifiers(*group);

  /* Unchanged source: the very same tree and nodes come back. */
  bNodeTree *again = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  EXPECT_EQ(again, group);
  EXPECT_TRUE(same_nodes(nodes_before, root_nodes(*group)));

  /* A source edit moves the tree hash: rebuilt in place, same pointer, same interface ids. */
  ASSERT_NE(bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_NOISE), nullptr);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);
  bNodeTree *rebuilt = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  EXPECT_EQ(rebuilt, group);
  EXPECT_NE(count_type(*group, SH_NODE_TEX_NOISE), 0);
  EXPECT_EQ(output_identifiers(*group), ids_before);
}

TEST_F(PaintLayersGenerateTest, source_group_refusals)
{
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;

  /* No Principled at all. */
  Material *flat = BKE_material_add(bmain, "FlatSource");
  refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_source_group_ensure(*bmain, *ma, *flat, refusal), nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::NoPrincipled);

  /* The owner cannot wrap itself. */
  refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_source_group_ensure(*bmain, *ma, *ma, refusal), nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::SelfReference);

  /* A Principled buried deeper than the supported path limit is still refused. */
  Material *deep = make_nested_principled_source_depth(*bmain, "DeepNestedSource", 9);
  refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_source_group_ensure(*bmain, *ma, *deep, refusal), nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::PrincipledInGroup);
}

TEST_F(PaintLayersGenerateTest, source_groups_are_per_owner)
{
  Material *owner_b = BKE_material_add(bmain, "OwnerB");
  Material *source = add_principled_source("SharedSource", 0.3f);
  PaintLayersSourceGroupRefusal refusal;
  bNodeTree *group_a = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  bNodeTree *group_b = BKE_paint_layers_source_group_ensure(*bmain, *owner_b, *source, refusal);
  ASSERT_NE(group_a, nullptr);
  ASSERT_NE(group_b, nullptr);
  EXPECT_NE(group_a, group_b);
}

TEST_F(PaintLayersGenerateTest, source_group_instantiates_one_wrapper_node)
{
  Material *source = add_principled_source("WrapSourceNode", 0.3f);
  source_set_noise_base_color(*bmain, *source);

  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.source_group_refusal, PaintLayersSourceGroupRefusal::None);

  bNodeTree *wrapper = source_wrapper_find(*bmain, "WrapSourceNode");
  ASSERT_NE(wrapper, nullptr);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);

  /* Exactly one wrapper instance, and no baked Image node for this row. */
  int instances = 0;
  bool base_color_linked = false;
  for (bNode &node : group->nodes) {
    if (node.is_group() && node.id == &wrapper->id) {
      instances++;
    }
  }
  EXPECT_EQ(instances, 1);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 0);
  /* Its COLOR:BASE_COLOR output reaches the row's chain. */
  wrapper->ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : wrapper->interface_outputs()) {
    if (iface->identifier == nullptr) {
      continue;
    }
    const IDProperty *role = iface->properties != nullptr ?
                                 IDP_GetPropertyTypeFromGroup(
                                     iface->properties, "pbr_custom_role", IDP_STRING) :
                                 nullptr;
    if (role != nullptr && STREQ(IDP_string_get(role), "COLOR:BASE_COLOR")) {
      bNode *instance = nullptr;
      for (bNode &node : group->nodes) {
        if (node.is_group() && node.id == &wrapper->id) {
          instance = &node;
        }
      }
      ASSERT_NE(instance, nullptr);
      bNodeSocket *out = bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
      ASSERT_NE(out, nullptr);
      base_color_linked = !out->directly_linked_links().is_empty();
    }
  }
  EXPECT_TRUE(base_color_linked);
}

TEST_F(PaintLayersGenerateTest, source_group_refusal_falls_back_to_baked)
{
  /* Too deep to wrap: the factory refuses and the row falls back to its baked maps. */
  Material *source = make_nested_principled_source_depth(*bmain, "NestedWrapSource", 9);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  Image *baked = add_image("NestedBaked");
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.source_group_refusal, PaintLayersSourceGroupRefusal::PrincipledInGroup);

  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  /* The row did not get a wrapper instance; it reads its baked map. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 1);
}

TEST_F(PaintLayersGenerateTest,
       source_group_mode_change_keeps_the_root_when_channels_are_untracked)
{
  /* F2-C4a gives every tracked map channel a content-alpha output, so a Material row whose channel
   * set includes one changes the row group's interface when it moves between Hybrid and SourceGroup
   * and the root is rebuilt (see the tracked-channel test below). This guards the other half: a row
   * whose only channel is untracked builds no content alpha in either mode, so the interface holds
   * and the root is kept. */
  Material *source = add_principled_source("UntrackedModeSource", 0.3f);
  bNodeTree &source_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);

  /* Every channel #channel_tracks_content_alpha would track is made Unavailable: the resolver
   * rejects a tiled (UDIM) image, so linking each of those Principled inputs to one shared tiled
   * Image Texture keeps them out of the wired set in both modes. */
  auto add_flat_image_node = [&](Image &image) -> bNode * {
    bNode *node = bke::node_add_static_node(nullptr, source_tree, SH_NODE_TEX_IMAGE);
    if (node == nullptr) {
      return nullptr;
    }
    node->id = &image.id;
    id_us_plus(&image.id);
    NodeTexImage *storage = static_cast<NodeTexImage *>(node->storage);
    storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
    storage->interpolation = SHD_INTERP_LINEAR;
    storage->projection = SHD_PROJ_FLAT;
    return node;
  };
  auto find_socket = [](bNode &node, const eNodeSocketInOut in_out, const char *name) {
    return bke::node_find_socket(node, in_out, UString::from_ptr_noinline(name));
  };

  Image *unsampleable = add_image("UntrackedModeSuppress");
  unsampleable->source = IMA_SRC_TILED;
  bNode *suppressor = add_flat_image_node(*unsampleable);
  ASSERT_NE(suppressor, nullptr);
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.socket_name == nullptr || info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      continue;
    }
    if (bNodeSocket *input = find_socket(*principled, SOCK_IN, info.socket_name)) {
      bke::node_add_link(source_tree,
                         *suppressor,
                         *find_socket(*suppressor, SOCK_OUT, "Color"),
                         *principled,
                         *input);
    }
  }

  /* The one channel left is Normal, through a Bump with no Height: the resolver answers the flat
   * constant there, so the row is Hybrid on a live constant. Normal is not tracked, so neither mode
   * builds a content-alpha output for it. */
  bNode *bump = bke::node_add_static_node(nullptr, source_tree, SH_NODE_BUMP);
  ASSERT_NE(bump, nullptr);
  bke::node_add_link(source_tree,
                     *bump,
                     *find_socket(*bump, SOCK_OUT, "Normal"),
                     *principled,
                     *find_socket(*principled, SOCK_IN, "Normal"));
  BKE_ntree_update_tag_all(&source_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, source_tree);

  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  /* Hybrid on an untracked channel: no content-alpha output on the row group. */
  EXPECT_FALSE(interface_has_socket(*group, "Content Alpha Normal", true));
  ASSERT_TRUE(group_io_sentinel_set(*group, 0.125f));

  /* A Height source turns the Bump into a graph the CPU cannot reproduce: Normal moves to
   * SourceGroup. The channel set is unchanged and neither mode tracks content alpha, so the row
   * group rebuilds while the root is kept. */
  bNode *noise = bke::node_add_static_node(nullptr, source_tree, SH_NODE_TEX_NOISE);
  ASSERT_NE(noise, nullptr);
  bke::node_add_link(source_tree,
                     *noise,
                     *find_socket(*noise, SOCK_OUT, "Fac"),
                     *bump,
                     *find_socket(*bump, SOCK_IN, "Height"));
  BKE_ntree_update_tag_all(&source_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, source_tree);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_EQ(layer_tree_find(*bmain, "Source"), group);
  EXPECT_FALSE(group_io_sentinel_get(*group, 0.125f));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
}

TEST_F(PaintLayersGenerateTest,
       source_group_mode_change_keeps_the_root_on_tracked_channels)
{
  /* Base Color is a tracked channel, but a Material row's transparency is its Alpha input, so the
   * row never exports a "Content Alpha Base Color" output in either mode. Constant -> Noise
   * therefore moves the row to SourceGroup without moving the group's interface: the row's group
   * rebuilds, the root is kept. The complementary guard to
   * source_group_mode_change_keeps_the_root_when_channels_are_untracked. */
  Material *source = add_principled_source("TrackedModeSource", 0.3f);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  /* A Material row tracks no content alpha, even on a tracked channel. */
  EXPECT_FALSE(interface_has_socket(*group, "Content Alpha Base Color", true));
  ASSERT_TRUE(group_io_sentinel_set(*group, 0.125f));

  /* Constant -> Noise moves the row to SourceGroup: its group rebuilds, the root does not. */
  source_set_noise_base_color(*bmain, *source);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_EQ(layer_tree_find(*bmain, "Source"), group);
  EXPECT_FALSE(group_io_sentinel_get(*group, 0.125f));
  EXPECT_FALSE(interface_has_socket(*group, "Content Alpha Base Color", true));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));

  /* The kept root is internally sound: every link still touches sockets its own endpoints own, and
   * every Compose Color Alpha that survived is fed (the Base Color Paint row below still tracks
   * content alpha). */
  root->ensure_topology_cache();
  EXPECT_TRUE(root_links_are_consistent(*root));
  EXPECT_EQ(count_unfed_compose_alpha(*root), 0);
}

TEST_F(PaintLayersGenerateTest, source_group_row_stays_on_baked_maps_on_cpu)
{
  Material *source = add_principled_source("CpuSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  Image *baked = add_image("CpuBaked");
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeImageLayer *found = nullptr;
  for (const PaintMaterialCompositeImageLayer &layer : layers) {
    if (BLI_uuid_equal(layer.marker, row->marker)) {
      found = &layer;
    }
  }
  ASSERT_NE(found, nullptr);
  /* The CPU stayed on the baked map: no live constant and no live source map. */
  EXPECT_EQ(found->color_image, baked);
  EXPECT_FALSE(found->has_constant_color);
}

TEST_F(PaintLayersGenerateTest, report_records_source_group_material_row)
{
  Material *source = add_principled_source("ReportSgSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "ReportRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(report.material_rows.size(), 1);
  EXPECT_TRUE(BLI_uuid_equal(report.material_rows[0].marker, row->marker));
  EXPECT_STREQ(report.material_rows[0].name, "ReportRow");
  EXPECT_EQ(report.material_rows[0].mode, PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(report.material_rows[0].wrapper_built);
  EXPECT_EQ(report.material_rows[0].refusal, PaintLayersSourceGroupRefusal::None);
}

TEST_F(PaintLayersGenerateTest, report_records_hybrid_material_row)
{
  Material *source = add_principled_source("ReportHybridSource", 0.3f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "HybridRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(report.material_rows.size(), 1);
  EXPECT_EQ(report.material_rows[0].mode, PaintLayerMaterialMode::Hybrid);
  EXPECT_FALSE(report.material_rows[0].wrapper_built);
  EXPECT_EQ(report.material_rows[0].refusal, PaintLayersSourceGroupRefusal::None);
}

TEST_F(PaintLayersGenerateTest, report_records_baked_material_row)
{
  Material *source = BKE_material_add(bmain, "ReportBakedSource");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "BakedRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(report.material_rows.size(), 1);
  EXPECT_EQ(report.material_rows[0].mode, PaintLayerMaterialMode::Baked);
  EXPECT_FALSE(report.material_rows[0].wrapper_built);
}

TEST_F(PaintLayersGenerateTest, source_group_wraps_principled_in_one_group)
{
  Material *source = make_nested_principled_source(*bmain, "WrapNestedSource");
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.source_group_refusal, PaintLayersSourceGroupRefusal::None);

  bNodeTree *wrapper = source_wrapper_find(*bmain, "WrapNestedSource");
  ASSERT_NE(wrapper, nullptr);
  EXPECT_STREQ(output_role(*wrapper, "Base Color"), "COLOR:BASE_COLOR");
  /* The nested Noise graph travels into the wrapper through the path propagation. */
  EXPECT_GE(count_type(*wrapper, SH_NODE_TEX_NOISE), 1);

  /* A second, unchanged call hands back the same tree and rebuilds nothing. */
  const Vector<bNode *> nodes_before = root_nodes(*wrapper);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *again = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  EXPECT_EQ(again, wrapper);
  EXPECT_TRUE(same_nodes(nodes_before, root_nodes(*wrapper)));
}

TEST_F(PaintLayersGenerateTest, source_group_wraps_principled_two_levels_deep)
{
  Material *source = make_nested_principled_source_depth(*bmain, "WrapNested2Source", 2);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.source_group_refusal, PaintLayersSourceGroupRefusal::None);
  bNodeTree *wrapper = source_wrapper_find(*bmain, "WrapNested2Source");
  ASSERT_NE(wrapper, nullptr);
  EXPECT_STREQ(output_role(*wrapper, "Base Color"), "COLOR:BASE_COLOR");
  EXPECT_GE(count_type(*wrapper, SH_NODE_TEX_NOISE), 1);
}

TEST_F(PaintLayersGenerateTest, source_group_leaves_the_original_groups_alone)
{
  Material *source = make_nested_principled_source(*bmain, "UntouchedNestedSource");
  bNodeTree *original = nullptr;
  for (bNode &node : source->nodetree->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
      original = id_cast<bNodeTree *>(node.id);
    }
  }
  ASSERT_NE(original, nullptr);
  const Vector<std::string> names_before = output_names(*original);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  ASSERT_NE(BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal), nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  /* The user's own nested group keeps exactly the interface it had. */
  EXPECT_EQ(output_names(*original), names_before);
}

TEST_F(PaintLayersGenerateTest, source_groups_prune_removes_the_whole_path)
{
  Material *source = make_nested_principled_source(*bmain, "PruneNestedSource");
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_GT(source_wrapper_tree_count(*bmain), 1);

  /* Dropping the row orphans the wrapper and every path copy; the next pass removes them all. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(source_wrapper_tree_count(*bmain), 0);
}

TEST_F(PaintLayersGenerateTest, source_group_remaps_frame_parents)
{
  /* The user's material: nodes grouped into Frames, the Principled inside a nested group. */
  Material *source = make_nested_principled_source(*bmain, "FrameParentSource");
  bNodeTree &source_tree = *source->nodetree;
  bNode *group_node = nullptr;
  for (bNode &node : source_tree.nodes) {
    if (node.is_group() && node.id != nullptr) {
      group_node = &node;
    }
  }
  ASSERT_NE(group_node, nullptr);
  bNode *frame = bke::node_add_static_node(nullptr, source_tree, NODE_FRAME);
  ASSERT_NE(frame, nullptr);
  group_node->parent = frame;
  bNodeTree *nested = id_cast<bNodeTree *>(group_node->id);
  ASSERT_NE(nested, nullptr);
  bNode *inner_frame = bke::node_add_static_node(nullptr, *nested, NODE_FRAME);
  ASSERT_NE(inner_frame, nullptr);
  for (bNode &node : nested->nodes) {
    if (!node.is_frame()) {
      node.parent = inner_frame;
    }
  }

  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  const Vector<bNodeTree *> wrappers = source_wrapper_trees(*bmain);
  ASSERT_GT(wrappers.size(), 1);
  for (bNodeTree *wrapper : wrappers) {
    EXPECT_TRUE(parents_are_local(*wrapper)) << wrapper->id.name + 2;
  }
  ASSERT_NE(source_wrapper_find(*bmain, "FrameParentSource"), nullptr);
  EXPECT_GE(count_type(*source_wrapper_find(*bmain, "FrameParentSource"), NODE_FRAME), 1);
}

TEST_F(PaintLayersGenerateTest, source_group_copy_survives_a_tree_copy)
{
  /* The depsgraph copies the wrappers as node trees; ntree_copy_data walks node->parent, so a
   * stale pointer would crash there. */
  Material *source = make_nested_principled_source(*bmain, "CopyRegressionSource");
  bNodeTree &source_tree = *source->nodetree;
  bNode *group_node = nullptr;
  for (bNode &node : source_tree.nodes) {
    if (node.is_group() && node.id != nullptr) {
      group_node = &node;
    }
  }
  ASSERT_NE(group_node, nullptr);
  bNode *frame = bke::node_add_static_node(nullptr, source_tree, NODE_FRAME);
  group_node->parent = frame;

  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (bNodeTree *wrapper : source_wrapper_trees(*bmain)) {
    bNodeTree *copy = id_cast<bNodeTree *>(
        BKE_id_copy_ex(bmain, &wrapper->id, nullptr, LIB_ID_COPY_DEFAULT));
    ASSERT_NE(copy, nullptr) << wrapper->id.name + 2;
    EXPECT_TRUE(parents_are_local(*copy));
    BKE_id_free(bmain, copy);
  }
}

/** A source whose Principled is inside a nested group and whose group node sits in a Frame, like a
 * user's file. The relation tracking reads the material, not the graph's shape. */
Material *make_frame_principled_source(Main &bmain, const char *name)
{
  Material *source = make_nested_principled_source(bmain, name);
  bNodeTree &tree = *source->nodetree;
  bNode *group_node = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.is_group() && node.id != nullptr) {
      group_node = &node;
      break;
    }
  }
  if (group_node != nullptr) {
    bNode *frame = bke::node_add_static_node(nullptr, tree, NODE_FRAME);
    if (frame != nullptr) {
      group_node->parent = frame;
    }
  }
  return source;
}

TEST_F(PaintLayersGenerateTest, relations_rebuild_when_the_source_material_set_changes)
{
  /* (a) No Material rows: the first regeneration creates the generated tree and rebuilds relations
   * for that alone; a second, no-op regeneration must leave them alone. */
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.relations_changed);

  /* (b) A Material row gaining a source is a relation change; repeating it is not. */
  Material *source_a = make_frame_principled_source(*bmain, "RelationSourceA");
  MaterialPaintLayer *row_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "SourceA", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row_a, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row_a, source_a));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.relations_changed);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.relations_changed);

  /* (c) Swapping the source material is a change. */
  Material *source_b = make_frame_principled_source(*bmain, "RelationSourceB");
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row_a, source_b));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.relations_changed);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.relations_changed);

  /* (d) A second row reading the other source, then a reorder: the set is the same, so relations
   * must not be rebuilt. */
  MaterialPaintLayer *row_a2 = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "SourceA2", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row_a2, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row_a2, source_a));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.relations_changed);
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, row_a, 0));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.relations_changed);

  /* (e) Losing the last source row forces the rebuild that drops the source from the graph. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, row_a));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.relations_changed);
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, row_a2));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.relations_changed);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.relations_changed);
}

/** The `pbr_paint_layers_source` marker of \a tree, or 0 when it has none. */
static int tree_source_uid(const bNodeTree &tree)
{
  if (tree.id.properties == nullptr) {
    return 0;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      tree.id.properties, "pbr_paint_layers_source", IDP_INT);
  return (prop != nullptr) ? IDP_int_get(prop) : 0;
}

/** The wrapper output role string of \a socket, or null. */
static const char *socket_role(const bNodeTreeInterfaceSocket &socket)
{
  const IDProperty *role = socket.properties != nullptr ?
                               IDP_GetPropertyTypeFromGroup(
                                   socket.properties, "pbr_custom_role", IDP_STRING) :
                               nullptr;
  return (role != nullptr) ? IDP_string_get(role) : nullptr;
}

TEST_F(PaintLayersGenerateTest, source_group_reports_created_then_reused)
{
  Material *source = make_frame_principled_source(*bmain, "EnsureChangedSource");
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bool changed = false;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  EXPECT_TRUE(changed);

  changed = true;
  bNodeTree *again = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed);
  EXPECT_EQ(again, wrapper);
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersGenerateTest, source_group_rebuild_replaces_path_copies)
{
  Material *source = make_frame_principled_source(*bmain, "RebuildSource");
  /* The one nested group the Principled lives in, the group on the wrapper's path. */
  bNodeTree *inner = nullptr;
  for (bNode &node : source->nodetree->nodes) {
    if (node.is_group() && node.id != nullptr) {
      inner = id_cast<bNodeTree *>(node.id);
    }
  }
  ASSERT_NE(inner, nullptr);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bool changed = false;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_TRUE(changed);

  /* The wrapper plus exactly one path copy carry this source's marker. */
  const uint32_t source_uid = source->id.session_uid;
  Vector<bNodeTree *> before;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (&tree != wrapper && tree_source_uid(tree) == int(source_uid)) {
      before.append(&tree);
    }
  }
  ASSERT_EQ(before.size(), 1);
  bNodeTree *old_copy = before[0];

  /* Move the source hash: a new node inside the nested group. */
  ASSERT_NE(bke::node_add_static_node(nullptr, *inner, SH_NODE_TEX_NOISE), nullptr);
  BKE_ntree_update_tag_all(inner);
  BKE_ntree_update_after_single_tree_change(*bmain, *inner);

  changed = false;
  bNodeTree *rebuilt = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed);
  EXPECT_EQ(rebuilt, wrapper);
  EXPECT_TRUE(changed);

  Vector<bNodeTree *> after;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (tree_source_uid(tree) == int(source_uid)) {
      after.append(&tree);
    }
  }
  EXPECT_EQ(after.size(), 2);
  EXPECT_FALSE(after.contains(old_copy));
}

TEST_F(PaintLayersGenerateTest, active_source_group_row_instantiates_and_links)
{
  Material *source = make_frame_principled_source(*bmain, "ActiveSourceGroup");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(report.relations_changed);

  bNodeTree *wrapper = source_wrapper_find(*bmain, "ActiveSourceGroup");
  ASSERT_NE(wrapper, nullptr);
  bNodeTree *layer_tree = layer_tree_find(*bmain, "Source");
  ASSERT_NE(layer_tree, nullptr);
  layer_tree->ensure_topology_cache();
  wrapper->ensure_interface_cache();

  int instances = 0;
  int linked_color_outputs = 0;
  for (bNode &node : layer_tree->nodes) {
    if (!node.is_group() || node.id != &wrapper->id) {
      continue;
    }
    instances++;
    for (bNodeTreeInterfaceSocket *iface : wrapper->interface_outputs()) {
      const char *role = (iface != nullptr) ? socket_role(*iface) : nullptr;
      if (role == nullptr || !StringRef(role).startswith("COLOR:") || iface->identifier == nullptr)
      {
        continue;
      }
      bNodeSocket *out = bke::node_find_socket(
          node, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
      if (out != nullptr && !out->directly_linked_links().is_empty()) {
        linked_color_outputs++;
      }
    }
  }
  EXPECT_EQ(instances, 1);
  EXPECT_GE(linked_color_outputs, 1);
}

/**
 * A source whose Principled sits in a nested group fed through the group's interface: the root's
 * RGB and Value nodes reach the group inputs through Reroutes, Metallic is an instance value, and
 * the group output feeds the Material Output. The earlier helpers fed the Principled constants, so
 * a group interface lost in the wrapper copy stayed invisible.
 */
Material *make_interface_wired_source(Main &bmain, const char *name)
{
  bNodeTree *group = bke::node_tree_add_tree(&bmain, "InterfaceWiredGroup", "ShaderNodeTree");
  bNodeTreeInterface &iface = group->tree_interface;
  bNodeTreeInterfaceSocket *in_color = iface.add_socket(
      "Color", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *in_roughness = iface.add_socket(
      "Roughness", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *in_metallic = iface.add_socket(
      "Metallic", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *out_bsdf = iface.add_socket(
      "BSDF", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  iface.tag_items_changed();
  static_cast<bNodeSocketValueFloat *>(in_metallic->socket_data)->value = 0.7f;

  bNode *group_input = bke::node_add_node(nullptr, *group, "NodeGroupInput"_ustr);
  bNode *group_output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  bNode *principled = bke::node_add_static_node(nullptr, *group, SH_NODE_BSDF_PRINCIPLED);
  nodes::update_node_declaration_and_sockets(*group, *group_input);
  nodes::update_node_declaration_and_sockets(*group, *group_output);
  bke::node_add_link(*group,
                     *group_input,
                     *bke::node_find_socket(
                         *group_input, SOCK_OUT, UString::from_ptr_noinline(in_color->identifier)),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  bke::node_add_link(
      *group,
      *group_input,
      *bke::node_find_socket(
          *group_input, SOCK_OUT, UString::from_ptr_noinline(in_roughness->identifier)),
      *principled,
      *bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr));
  bke::node_add_link(
      *group,
      *group_input,
      *bke::node_find_socket(
          *group_input, SOCK_OUT, UString::from_ptr_noinline(in_metallic->identifier)),
      *principled,
      *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));
  bke::node_add_link(*group,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *group_output,
                     *bke::node_find_socket(
                         *group_output, SOCK_IN, UString::from_ptr_noinline(out_bsdf->identifier)));
  bNode *group_frame = bke::node_add_static_node(nullptr, *group, NODE_FRAME);
  for (bNode &node : group->nodes) {
    if (!node.is_frame()) {
      node.parent = group_frame;
    }
  }
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);

  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &tree = *source->nodetree;
  bNode *instance = bke::node_add_node(nullptr, tree, group->typeinfo->group_idname);
  instance->id = &group->id;
  id_us_plus(&group->id);
  nodes::update_node_declaration_and_sockets(tree, *instance);
  /* A full update before the links: it gives the instance's input sockets their default values,
   * which only then exist to be written (the source's Metallic rides the interface default). */
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);

  bNode *rgb = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  const float rgb_color[4] = {0.2f, 0.5f, 0.9f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value, rgb_color);
  bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNodeSocket *value_out = bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr);
  static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value = 0.3f;

  bNode *reroute_color = bke::node_add_static_node(nullptr, tree, NODE_REROUTE);
  bNode *reroute_roughness = bke::node_add_static_node(nullptr, tree, NODE_REROUTE);
  bke::node_add_link(tree,
                     *rgb,
                     *rgb_out,
                     *reroute_color,
                     *bke::node_find_socket(*reroute_color, SOCK_IN, "Input"_ustr));
  bke::node_add_link(
      tree,
      *reroute_color,
      *bke::node_find_socket(*reroute_color, SOCK_OUT, "Output"_ustr),
      *instance,
      *bke::node_find_socket(*instance, SOCK_IN, UString::from_ptr_noinline(in_color->identifier)));
  bke::node_add_link(tree,
                     *value,
                     *value_out,
                     *reroute_roughness,
                     *bke::node_find_socket(*reroute_roughness, SOCK_IN, "Input"_ustr));
  bke::node_add_link(
      tree,
      *reroute_roughness,
      *bke::node_find_socket(*reroute_roughness, SOCK_OUT, "Output"_ustr),
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_IN, UString::from_ptr_noinline(in_roughness->identifier)));
  /* Metallic stays unlinked; the group interface carries its 0.7 into the instance default. */

  bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(
      tree,
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(out_bsdf->identifier)),
      *output,
      *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));

  bNode *root_frame = bke::node_add_static_node(nullptr, tree, NODE_FRAME);
  for (bNode &node : tree.nodes) {
    if (!node.is_frame()) {
      node.parent = root_frame;
    }
  }
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);
  return source;
}

TEST_F(PaintLayersGenerateTest, source_group_copy_keeps_the_group_interface)
{
  Material *source = make_interface_wired_source(*bmain, "WiredSource");
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bool changed = false;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  EXPECT_TRUE(changed);

  /* The wrapper's group instance points at the copy of the nested source group. */
  bNode *instance = nullptr;
  for (bNode &node : wrapper->nodes) {
    if (node.is_group() && node.id != nullptr) {
      instance = &node;
    }
  }
  ASSERT_NE(instance, nullptr);
  bNodeTree *child = id_cast<bNodeTree *>(instance->id);
  ASSERT_NE(child, nullptr);
  child->ensure_topology_cache();
  wrapper->ensure_topology_cache();

  /* The copied interface kept the source group's input sockets. */
  bNodeTreeInterfaceSocket *color_iface = group_input_find(*child, "Color");
  bNodeTreeInterfaceSocket *rough_iface = group_input_find(*child, "Roughness");
  bNodeTreeInterfaceSocket *metal_iface = group_input_find(*child, "Metallic");
  ASSERT_NE(color_iface, nullptr);
  ASSERT_NE(rough_iface, nullptr);
  ASSERT_NE(metal_iface, nullptr);

  /* Group Input -> Principled links survived in the copy. */
  bNode *group_input = nullptr;
  bNode *principled = nullptr;
  for (bNode &node : child->nodes) {
    if (node.is_group_input()) {
      group_input = &node;
    }
    else if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
    }
  }
  ASSERT_NE(group_input, nullptr);
  ASSERT_NE(principled, nullptr);
  struct LinkPair {
    bNodeTreeInterfaceSocket *iface;
    const char *principled_socket;
  };
  const LinkPair pairs[] = {
      {color_iface, "Base Color"}, {rough_iface, "Roughness"}, {metal_iface, "Metallic"}};
  for (const LinkPair &pair : pairs) {
    bNodeSocket *out = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(pair.iface->identifier));
    ASSERT_NE(out, nullptr);
    EXPECT_FALSE(out->directly_linked_links().is_empty());
    bNodeSocket *in = bke::node_find_socket(
        *principled, SOCK_IN, UString::from_ptr_noinline(pair.principled_socket));
    ASSERT_NE(in, nullptr);
    EXPECT_FALSE(in->directly_linked_links().is_empty());
  }

  /* The copied Group Output is fed from those links, not left on its socket defaults. Its private
   * output names are the `".PL <channel>"` form, since only the root carries the public names. */
  bNode *group_output = nullptr;
  for (bNode &node : child->nodes) {
    if (node.is_group_output()) {
      group_output = &node;
    }
  }
  ASSERT_NE(group_output, nullptr);
  for (const char *name : {".PL Base Color", ".PL Roughness", ".PL Metallic"}) {
    bNodeSocket *out = bke::node_find_enabled_socket(*group_output, SOCK_IN, name);
    ASSERT_NE(out, nullptr) << name;
    EXPECT_FALSE(out->directly_linked_links().is_empty()) << name;
  }

  /* The wrapper instance kept its input sockets: Color/Roughness stay linked through the copied
   * Reroutes, Metallic keeps the instance value from the source. */
  bNodeSocket *inst_color = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(color_iface->identifier));
  bNodeSocket *inst_rough = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(rough_iface->identifier));
  bNodeSocket *inst_metal = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(metal_iface->identifier));
  ASSERT_NE(inst_color, nullptr);
  ASSERT_NE(inst_rough, nullptr);
  ASSERT_NE(inst_metal, nullptr);
  EXPECT_FALSE(inst_color->directly_linked_links().is_empty());
  EXPECT_FALSE(inst_rough->directly_linked_links().is_empty());
  EXPECT_TRUE(inst_metal->directly_linked_links().is_empty());
  ASSERT_NE(inst_metal->default_value, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(inst_metal->default_value)->value, 0.7f);
}

/** The source's root RGB node, for a value edit that leaves topology alone. */
static bNode *source_rgb_node(Material &source)
{
  for (bNode &node : source.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_RGB) {
      return &node;
    }
  }
  return nullptr;
}

TEST_F(PaintLayersGenerateTest, active_material_child_keeps_its_folder_unbaked)
{
  Material *source = make_interface_wired_source(*bmain, "ActiveBakeSource");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(material, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, material, source));
  ASSERT_NE(BKE_paint_layers_add(
                *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "PaintChild", folder, PaintLayerPlace::Into),
            nullptr);

  /* A heavy folder cache, so the synchronous planner and the wmJob path would both take it. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, PAINT_LAYERS_HEAVY_BAKE_SIZE));
  BKE_paint_layers_active_set(*ma, material->marker);

  /* Move a value in the source: the folder's hash moves (it covers the child's source tree),
   * but the active child's chain stays live. */
  bNode *rgb = source_rgb_node(*source);
  ASSERT_NE(rgb, nullptr);
  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  ASSERT_NE(rgb_out, nullptr);
  static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value[0] = 0.77f;

  /* ALWAYS. */
  bool changed = false;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* AUTO + heavy. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_AUTO));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, PAINT_LAYERS_HEAVY_BAKE_SIZE));
  changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* Leaving the row: a row outside the folder becomes active, so the folder is no longer
   * deferred and the heavy planner may queue it again. */
  MaterialPaintLayer *outside = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Outside", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(outside, nullptr);
  BKE_paint_layers_active_set(*ma, outside->marker);
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));
}

TEST_F(PaintLayersGenerateTest, sync_planner_still_bakes_a_plain_row)
{
  MaterialPaintLayer *layer = add_paint_layer("Plain", add_image("Plain"));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *layer, 4));
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *layer));

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

/**
 * Build a source shaped like a real material: the Principled inside a group whose interface names
 * the channels the wrapper routes, a second group off the path to it that the wrapper's copy also
 * instances (the "shared" group), a Frame the group instance is parented to, and a Reroute.
 *
 * \param r_shared receives the off-path group, \a r_on_path the group holding the Principled.
 */
static Material *make_hash_source(Main &bmain,
                                  bNodeTree **r_shared,
                                  bNodeTree **r_on_path)
{
  bNodeTree *on_path = make_principled_group(bmain, "HashInner");
  on_path->tree_interface.add_socket(
      "Roughness", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  on_path->tree_interface.add_socket(
      "Specular", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  BKE_ntree_update_tag_all(on_path);
  BKE_ntree_update_after_single_tree_change(bmain, *on_path);

  bNodeTree *shared = bke::node_tree_add_tree(&bmain, "HashShared", "ShaderNodeTree");
  shared->tree_interface.add_socket(
      "SharedOut", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNode *shared_value = bke::node_add_static_node(nullptr, *shared, SH_NODE_VALUE);
  BKE_ntree_update_tag_all(shared);
  BKE_ntree_update_after_single_tree_change(bmain, *shared);
  bNode *shared_output = bke::node_add_node(nullptr, *shared, "NodeGroupOutput"_ustr);
  bNodeTreeInterfaceSocket *shared_out = tree_interface_output_find(*shared, "SharedOut");
  bke::node_add_link(*shared,
                     *shared_value,
                     *bke::node_find_socket(*shared_value, SOCK_OUT, "Value"_ustr),
                     *shared_output,
                     *bke::node_find_socket(
                         *shared_output, SOCK_IN, UString::from_ptr_noinline(shared_out->identifier)));

  Material *source = BKE_material_add(&bmain, "HashSource");
  bNode *on_path_instance = bke::node_add_node(
      nullptr, *source->nodetree, on_path->typeinfo->group_idname);
  on_path_instance->id = &on_path->id;
  id_us_plus(&on_path->id);
  nodes::update_node_declaration_and_sockets(*source->nodetree, *on_path_instance);
  bNode *shared_instance = bke::node_add_node(
      nullptr, *source->nodetree, shared->typeinfo->group_idname);
  shared_instance->id = &shared->id;
  id_us_plus(&shared->id);
  nodes::update_node_declaration_and_sockets(*source->nodetree, *shared_instance);
  bNode *output_node = bke::node_add_static_node(
      nullptr, *source->nodetree, SH_NODE_OUTPUT_MATERIAL);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);
  bNodeTreeInterfaceSocket *surface = tree_interface_output_find(*on_path, "Surface");
  bke::node_add_link(*source->nodetree,
                     *on_path_instance,
                     *bke::node_find_socket(
                         *on_path_instance, SOCK_OUT, UString::from_ptr_noinline(surface->identifier)),
                     *output_node,
                     *bke::node_find_socket(*output_node, SOCK_IN, "Surface"_ustr));

  /* A Frame and a Reroute, and a value node parented to the frame: none of the parent/location/
   * selection state may enter the hash. */
  bNode *frame = bke::node_add_static_node(nullptr, *source->nodetree, NODE_FRAME);
  bNode *value = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_VALUE);
  value->parent = frame;
  bNode *reroute = bke::node_add_static_node(nullptr, *source->nodetree, NODE_REROUTE);
  bke::node_add_link(*source->nodetree,
                     *value,
                     *bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr),
                     *reroute,
                     *bke::node_find_socket(*reroute, SOCK_IN, "Input"_ustr));
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);

  *r_shared = shared;
  *r_on_path = on_path;
  return source;
}

/**
 * A source-material hash that follows content, not update state. A refresh of previews on shared
 * nested groups -- what regenerating the layered tree or a localized bake copy does -- must not
 * move it, or the editor's "source edited -> rebuild -> re-bake" loop never ends. A value edit
 * anywhere reachable, including a group off the path to the Principled, must move it.
 */
TEST_F(PaintLayersGenerateTest, source_hash_ignores_preview_refresh_but_follows_values)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "HashRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(source_wrapper_find(*bmain, "HashSource"), nullptr);

  const uint64_t before = BKE_paint_layers_source_material_tree_hash(*source);
  ASSERT_NE(before, 0u);

  /* Every nested group's previews counter moves; the content is untouched. */
  BKE_material_make_node_previews_dirty(source);
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash(*source), before);

  /* A value edit inside the off-path shared group is content, and must move the hash. */
  bNode *shared_value = nullptr;
  for (bNode &node : shared->nodes) {
    if (node.type_legacy == SH_NODE_VALUE) {
      shared_value = &node;
    }
  }
  ASSERT_NE(shared_value, nullptr);
  bNodeSocket *shared_out = bke::node_find_socket(*shared_value, SOCK_OUT, "Value"_ustr);
  ASSERT_NE(shared_out, nullptr);
  static_cast<bNodeSocketValueFloat *>(shared_out->default_value)->value = 0.8125f;
  BKE_ntree_update_tag_all(shared);
  BKE_ntree_update_after_single_tree_change(*bmain, *shared);
  EXPECT_NE(BKE_paint_layers_source_material_tree_hash(*source), before);

  /* An interface default on the group holding the Principled is content too. */
  const uint64_t after_value = BKE_paint_layers_source_material_tree_hash(*source);
  on_path->ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : on_path->interface_inputs()) {
    if (socket->name != nullptr && STREQ(socket->name, "Roughness")) {
      static_cast<bNodeSocketValueFloat *>(socket->socket_data)->value = 0.25f;
    }
  }
  BKE_ntree_update_tag_all(on_path);
  BKE_ntree_update_after_single_tree_change(*bmain, *on_path);
  EXPECT_NE(BKE_paint_layers_source_material_tree_hash(*source), after_value);
}

/**
 * A Material row's bake is its source material rendered into maps; the row's mask is composited
 * live over them. Drawing on the mask must not invalidate the source maps, and re-subscribing after
 * a bake must not look like a change by itself (a fresh partial-update user answers
 * FullUpdateNeeded on its first collect).
 */
TEST_F(PaintLayersGenerateTest, material_row_subscription_ignores_its_mask)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);

  Material *layered = BKE_material_add(bmain, "MaskLayered");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_MATERIAL, "MaskRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*layered, row, source));

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*layered, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  Image *mask_map = add_image("MaterialRowMaskMap");
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *layered, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));

  ASSERT_NE(BKE_paint_layers_bake_ensure(*row), nullptr);
  BKE_paint_layers_bake_finalize(*layered, *row);
  BKE_paint_layers_bake_notice_changes(*layered);
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*layered));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*layered, *row));

  /* A mask pixel edit is content for the live graph, not for the source maps. */
  BKE_image_partial_update_mark_full_update(mask_map);
  BKE_paint_layers_bake_notice_changes(*layered);
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*layered));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*layered, *row));

  /* A second subscribe/notice cycle, with no edits in between, stays clean. */
  BKE_paint_layers_bake_finalize(*layered, *row);
  BKE_paint_layers_bake_notice_changes(*layered);
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*layered));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*layered, *row));
}

/** A Paint row's own channel map is what its bake shows, so editing it must invalidate the row. */
TEST_F(PaintLayersGenerateTest, paint_row_subscription_sees_its_own_map_edit)
{
  Material *layered = BKE_material_add(bmain, "PaintSub");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_IMAGE, "PaintRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  Image *map = add_image("PaintRowMap");
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *layered, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_NE(BKE_paint_layers_bake_ensure(*row), nullptr);

  BKE_paint_layers_bake_finalize(*layered, *row);
  BKE_paint_layers_bake_notice_changes(*layered);
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*layered));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*layered, *row));

  BKE_image_partial_update_mark_full_update(map);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*layered, *row));
  BKE_paint_layers_bake_notice_changes(*layered);
  EXPECT_TRUE(BKE_paint_layers_bake_stale_get(*layered));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*layered, *row));
}

/** The private path copy of the on-path group: the wrapper tree that holds the copied Principled. */
static bNodeTree *source_group_path_copy(Main &bmain, const bNodeTree &wrapper)
{
  for (bNodeTree *tree : source_wrapper_trees(bmain)) {
    if (tree == &wrapper) {
      continue;
    }
    for (const bNode &node : tree->nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        return tree;
      }
    }
  }
  return nullptr;
}

/**
 * A value edit inside a group on the path to the Principled must sync into the existing wrapper:
 * same tree, same nodes, new value, `r_values_synced`, no rebuild and no topology change.
 */
TEST_F(PaintLayersGenerateTest, source_group_value_edit_syncs_in_place)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bool changed = false;
  bool synced = false;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed, &synced);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  EXPECT_TRUE(changed);
  EXPECT_FALSE(synced);
  const Vector<bNode *> nodes_before = root_nodes(*wrapper);

  bNode *src_principled = find_type(*on_path, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(src_principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*src_principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.123f;
  BKE_ntree_update_tag_all(on_path);
  BKE_ntree_update_after_single_tree_change(*bmain, *on_path);

  changed = true;
  synced = false;
  bNodeTree *again = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed, &synced);
  EXPECT_EQ(again, wrapper);
  EXPECT_FALSE(changed);
  EXPECT_TRUE(synced);
  EXPECT_TRUE(same_nodes(nodes_before, root_nodes(*wrapper)));

  bNodeTree *path_copy = source_group_path_copy(*bmain, *wrapper);
  ASSERT_NE(path_copy, nullptr);
  bNode *copy_principled = find_type(*path_copy, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(copy_principled, nullptr);
  bNodeSocket *copy_roughness = bke::node_find_socket(
      *copy_principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(copy_roughness, nullptr);
  EXPECT_FLOAT_EQ(
      static_cast<bNodeSocketValueFloat *>(copy_roughness->default_value)->value, 0.123f);
}

/**
 * A sync tags only the trees it changed. Editing a value in the innermost path copy must not move
 * the root wrapper's update state; a sync that matches every value must not touch any tree.
 */
TEST_F(PaintLayersGenerateTest, source_group_sync_tags_only_changed_trees)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(wrapper, nullptr);
  bNodeTree *path_copy = source_group_path_copy(*bmain, *wrapper);
  ASSERT_NE(path_copy, nullptr);
  const Vector<bNode *> root_nodes_before = root_nodes(*wrapper);

  /* Nothing changed: the ensure returns early and no tree is re-declared. */
  const uint32_t root_before = wrapper->runtime->previews_refresh_state;
  const uint32_t copy_before = path_copy->runtime->previews_refresh_state;
  ASSERT_EQ(BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal), wrapper);
  EXPECT_EQ(wrapper->runtime->previews_refresh_state, root_before);
  EXPECT_EQ(path_copy->runtime->previews_refresh_state, copy_before);

  /* The value lives in the path copy; only that tree is re-declared. */
  bNode *src_principled = find_type(*on_path, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(src_principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*src_principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.456f;
  BKE_ntree_update_tag_all(on_path);
  BKE_ntree_update_after_single_tree_change(*bmain, *on_path);

  bool changed = false;
  bool synced = false;
  ASSERT_EQ(BKE_paint_layers_source_group_ensure(
                *bmain, *ma, *source, refusal, &changed, &synced),
            wrapper);
  EXPECT_TRUE(synced);
  /* Only the tree whose node changed is re-declared; the root keeps its nodes. (The node-tree
   * update reaches the root as a user, but never rebuilds it.) */
  EXPECT_TRUE(same_nodes(root_nodes_before, root_nodes(*wrapper)));
  EXPECT_NE(path_copy->runtime->previews_refresh_state, copy_before);
}

/** Storage values travel too: a ColorRamp point and an RGB Curves point sync into the copy. */
TEST_F(PaintLayersGenerateTest, source_group_value_edit_syncs_storage)
{
  Material *source = add_principled_source("StorageSource", 0.3f);
  bNode *ramp = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_VALTORGB);
  bNode *curves = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_CURVE_RGB);
  ASSERT_NE(ramp, nullptr);
  ASSERT_NE(curves, nullptr);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(wrapper, nullptr);

  ColorBand *band = static_cast<ColorBand *>(ramp->storage);
  ASSERT_NE(band, nullptr);
  band->data[0].r = 0.11f;
  band->data[0].pos = 0.25f;
  CurveMapping *mapping = static_cast<CurveMapping *>(curves->storage);
  ASSERT_NE(mapping, nullptr);
  mapping->cm[0].curve[0].y = 0.42f;
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);

  bool changed = false;
  bool synced = false;
  ASSERT_EQ(BKE_paint_layers_source_group_ensure(
                *bmain, *ma, *source, refusal, &changed, &synced),
            wrapper);
  EXPECT_FALSE(changed);
  EXPECT_TRUE(synced);

  bNode *copy_ramp = nullptr;
  bNode *copy_curves = nullptr;
  for (bNode &node : wrapper->nodes) {
    if (STREQ(node.name, ramp->name)) {
      copy_ramp = &node;
    }
    if (STREQ(node.name, curves->name)) {
      copy_curves = &node;
    }
  }
  ASSERT_NE(copy_ramp, nullptr);
  ASSERT_NE(copy_curves, nullptr);
  ColorBand *copy_band = static_cast<ColorBand *>(copy_ramp->storage);
  ASSERT_NE(copy_band, nullptr);
  EXPECT_FLOAT_EQ(copy_band->data[0].r, 0.11f);
  EXPECT_FLOAT_EQ(copy_band->data[0].pos, 0.25f);
  CurveMapping *copy_mapping = static_cast<CurveMapping *>(copy_curves->storage);
  ASSERT_NE(copy_mapping, nullptr);
  EXPECT_FLOAT_EQ(copy_mapping->cm[0].curve[0].y, 0.42f);
}

/** Swapping the image of an Image Texture node keeps the source and copy user counts in step. */
TEST_F(PaintLayersGenerateTest, source_group_value_edit_syncs_image)
{
  Material *source = add_principled_source("ImageSource", 0.3f);
  bNode *principled = find_type(*source->nodetree, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(principled, nullptr);
  Image *map_a = add_image("SyncImageA");
  Image *map_b = add_image("SyncImageB");
  bNode *texture = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(texture, nullptr);
  texture->id = &map_a->id;
  id_us_plus(&map_a->id);
  bke::node_add_link(*source->nodetree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(wrapper, nullptr);

  const int users_a_after_build = int(map_a->id.us);
  const int users_b_before = int(map_b->id.us);

  id_us_min(&map_a->id);
  texture->id = &map_b->id;
  id_us_plus(&map_b->id);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);

  bool changed = false;
  bool synced = false;
  ASSERT_EQ(BKE_paint_layers_source_group_ensure(
                *bmain, *ma, *source, refusal, &changed, &synced),
            wrapper);
  EXPECT_FALSE(changed);
  EXPECT_TRUE(synced);

  bNode *copy_texture = nullptr;
  for (bNode &node : wrapper->nodes) {
    if (STREQ(node.name, texture->name)) {
      copy_texture = &node;
    }
  }
  ASSERT_NE(copy_texture, nullptr);
  EXPECT_EQ(copy_texture->id, &map_b->id);
  /* One new reference from the copy, one for the source's own node that now points at it. */
  EXPECT_EQ(int(map_b->id.us), users_b_before + 2);
  /* Both the source's and the copy's old references to `map_a` are released. */
  EXPECT_EQ(int(map_a->id.us), users_a_after_build - 2);
}

/** A topology edit still rebuilds: a new node moves the wrapper's nodes and reports `r_changed`. */
TEST_F(PaintLayersGenerateTest, source_group_topology_edit_rebuilds)
{
  Material *source = add_principled_source("TopoSource", 0.3f);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bool changed = false;
  bool synced = false;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed, &synced);
  ASSERT_NE(wrapper, nullptr);
  const Vector<bNode *> nodes_before = root_nodes(*wrapper);

  ASSERT_NE(bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_NOISE), nullptr);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);

  changed = false;
  synced = true;
  ASSERT_EQ(BKE_paint_layers_source_group_ensure(
                *bmain, *ma, *source, refusal, &changed, &synced),
            wrapper);
  EXPECT_TRUE(changed);
  EXPECT_FALSE(synced);
  EXPECT_FALSE(same_nodes(nodes_before, root_nodes(*wrapper)));
  EXPECT_NE(count_type(*wrapper, SH_NODE_TEX_NOISE), 0);
}

/** After a sync the channel the wrapper exposes reads the source's new value (a constant here). */
TEST_F(PaintLayersGenerateTest, source_group_sync_updates_channel_constant)
{
  Material *source = add_principled_source("ChannelSource", 0.3f);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *wrapper = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(wrapper, nullptr);

  bNode *principled = find_type(*source->nodetree, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.77f;
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);

  ASSERT_NE(BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal), nullptr);

  bNodeTreeInterfaceSocket *iface = nullptr;
  wrapper->ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : wrapper->interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, "Roughness")) {
      iface = socket;
    }
  }
  ASSERT_NE(iface, nullptr);
  bNode *group_output = nullptr;
  for (bNode &node : wrapper->nodes) {
    if (node.is_group_output()) {
      group_output = &node;
    }
  }
  ASSERT_NE(group_output, nullptr);
  bNodeSocket *out_in = bke::node_find_socket(
      *group_output, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
  ASSERT_NE(out_in, nullptr);
  ASSERT_NE(out_in->default_value, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(out_in->default_value)->value, 0.77f);
}

/** The bake hash follows the source's values, so leaving the row still re-bakes after a slider. */
TEST_F(PaintLayersGenerateTest, material_row_bake_hash_follows_source_value_edits)
{
  Material *source = add_principled_source("BakeValueSource", 0.3f);
  Material *layered = BKE_material_add(bmain, "BakeValueLayered");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_MATERIAL, "BakeRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*layered, row, source));

  uint32_t before[2];
  BKE_paint_layers_bake_hash(*row, before);

  bNode *principled = find_type(*source->nodetree, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.61f;
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);

  uint32_t after[2];
  BKE_paint_layers_bake_hash(*row, after);
  EXPECT_TRUE(before[0] != after[0] || before[1] != after[1]);
}

/**
 * Visibility is a value: disabling a row keeps the root and the row's own group nodes in place and
 * only drives the group-input factor to zero. Enabling restores the same tree with the value back.
 */
TEST_F(PaintLayersGenerateTest, visibility_toggle_keeps_the_graph)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  const Vector<bNode *> group_before = root_nodes(*group);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), group);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_TRUE(same_nodes(group_before, root_nodes(*group)));

  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "Bottom Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*root, *group, *iface);
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value,
                  layer->opacity);
}

/** Disabling a correction is a value edit too: no root or layer-group rebuild. */
TEST_F(PaintLayersGenerateTest, visibility_toggle_of_a_correction_keeps_the_graph)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Correction")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  const Vector<bNode *> group_before = root_nodes(*group);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, correction, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), group);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_TRUE(same_nodes(group_before, root_nodes(*group)));
}

/**
 * Turning a row off keeps it in the graph; an unrelated topology change is the chance to drop it,
 * and turning it back on is the one rebuild that brings it back.
 */
TEST_F(PaintLayersGenerateTest, disabled_row_leaves_on_incidental_rebuild_and_returns)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(layer_tree_find(*bmain, "Bottom"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, bottom, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Still in the graph: disabling is a value. */
  ASSERT_NE(layer_tree_find(*bmain, "Bottom"), nullptr);

  /* An unrelated topology change rebuilds the root and drops the disabled row. */
  add_paint_layer("Extra", add_image("Extra"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), nullptr);

  /* Enabling it back is a topology change and rebuilds it in. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, bottom, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Bottom"), nullptr);
}

/**
 * A parent folder's bake renders its children, so a disabled Material child changes the parent's
 * result and must move the parent's bake hash (the Material branch of `bake_hash_layer` used to
 * skip the visibility flag).
 */
TEST_F(PaintLayersGenerateTest, parent_bake_hash_sees_a_disabled_material_child)
{
  Material *source = add_principled_source("ChildSource", 0.3f);
  Material *layered = BKE_material_add(bmain, "ParentBake");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*layered, child, source));
  ASSERT_NE(BKE_paint_layers_bake_ensure(*folder), nullptr);

  uint32_t before[2];
  BKE_paint_layers_bake_hash(*folder, before);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*layered, child, false));
  uint32_t after[2];
  BKE_paint_layers_bake_hash(*folder, after);
  EXPECT_TRUE(before[0] != after[0] || before[1] != after[1]);
}

/**
 * A disabled active Material row stays live: its mode is still SourceGroup and a source value edit
 * still syncs into the wrapper (visibility drives only the factor).
 */
TEST_F(PaintLayersGenerateTest, disabled_active_material_row_still_syncs_source_values)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper = source_wrapper_find(*bmain, "HashSource");
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Visibility is not part of the mode decision. */
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  bNodeTree *path_copy = source_group_path_copy(*bmain, *wrapper);
  ASSERT_NE(path_copy, nullptr);

  bNode *src_principled = find_type(*on_path, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(src_principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*src_principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.281f;
  BKE_ntree_update_tag_all(on_path);
  BKE_ntree_update_after_single_tree_change(*bmain, *on_path);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNode *copy_principled = find_type(*path_copy, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(copy_principled, nullptr);
  bNodeSocket *copy_roughness = bke::node_find_socket(
      *copy_principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(copy_roughness, nullptr);
  EXPECT_FLOAT_EQ(
      static_cast<bNodeSocketValueFloat *>(copy_roughness->default_value)->value, 0.281f);
}

/**
 * Toggling a top-level row's visibility must not invalidate its own bake (the factor is live), while
 * a parent folder's bake, which renders its children, must see it.
 */
TEST_F(PaintLayersGenerateTest, visibility_does_not_invalidate_the_rows_own_bake)
{
  /* Paint row: its own bake stays valid, the folder above it does not. */
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *paint = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_move(*ma, paint, folder, PaintLayerPlace::Into);
  ASSERT_NE(BKE_paint_layers_bake_ensure(*paint), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *paint);
  ASSERT_NE(BKE_paint_layers_bake_ensure(*folder), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *folder);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *paint));

  uint32_t folder_before[2];
  BKE_paint_layers_bake_hash(*folder, folder_before);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, paint, false));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *paint));
  uint32_t folder_after[2];
  BKE_paint_layers_bake_hash(*folder, folder_after);
  EXPECT_TRUE(folder_before[0] != folder_after[0] || folder_before[1] != folder_after[1]);

  /* Material row: same rule. */
  Material *source = add_principled_source("SelfVisSource", 0.3f);
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  ASSERT_NE(BKE_paint_layers_bake_ensure(*mat), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *mat);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *mat));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, mat, false));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *mat));
}

/**
 * Adding a correction to one row rebuilds only that row's group; the root, a sibling and a source
 * wrapper stay untouched (their contracts do not change).
 */
TEST_F(PaintLayersGenerateTest, adding_a_correction_rebuilds_only_its_row)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  BKE_paint_layers_active_set(*ma, mat->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *bottom_group = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_group = layer_tree_find(*bmain, "Top");
  bNodeTree *wrapper = source_wrapper_find(*bmain, "HashSource");
  ASSERT_NE(bottom_group, nullptr);
  ASSERT_NE(top_group, nullptr);
  ASSERT_NE(wrapper, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  const Vector<bNode *> top_before = root_nodes(*top_group);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Correction")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_group);
  EXPECT_EQ(layer_tree_find(*bmain, "Top"), top_group);
  EXPECT_TRUE(same_nodes(top_before, root_nodes(*top_group)));
  EXPECT_EQ(source_wrapper_find(*bmain, "HashSource"), wrapper);
}

/**
 * A correction's opacity on a Material row is a value of the row group's interface; setting it and
 * syncing the values must reach the instance input in every mode. SourceGroup here.
 */
TEST_F(PaintLayersGenerateTest, material_row_correction_opacity_syncs)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  BKE_paint_layers_active_set(*ma, mat->marker);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, mat, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Correction"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "MatRow");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "MatRow C Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*ma->paint_layers_tree, *group, *iface);
  ASSERT_NE(socket, nullptr);
  const float before = static_cast<bNodeSocketValueFloat *>(socket->default_value)->value;

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.42f));
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.42f);
  EXPECT_NE(before, 0.42f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, correction, false));
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);
}

/**
 * A stack shaped like the user's: named and unnamed Paint rows, Material rows, a folder. Two
 * regenerations with no edits in between must keep every group and the root.
 */
TEST_F(PaintLayersGenerateTest, unnamed_row_and_material_stack_are_stable)
{
  add_paint_layer("Base Color", add_image("BaseColorMap"));
  MaterialPaintLayer *unnamed = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(unnamed, nullptr);
  add_channel(*unnamed, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("UnnamedMap"));

  Material *source = add_principled_source("StackSource", 0.3f);
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Wood", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
}

/**
 * Moving a row into a folder rebuilds the folder and the root, but keeps the moved row's own group
 * (its content did not change); moving it back out does the same in reverse.
 */
TEST_F(PaintLayersGenerateTest, folder_move_rebuilds_the_folder_and_keeps_the_row)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  /* Opacity below one keeps the folder isolating; a default folder would pass through instead. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *row_group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(row_group, nullptr);
  const Vector<bNode *> row_before = root_nodes(*row_group);

  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, folder, PaintLayerPlace::Into));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *folder_group = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_group, nullptr);
  EXPECT_EQ(layer_tree_find(*bmain, "Row"), row_group);
  EXPECT_TRUE(same_nodes(row_before, root_nodes(*row_group)));

  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, folder, PaintLayerPlace::Above));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Row"), row_group);
  EXPECT_TRUE(same_nodes(row_before, root_nodes(*row_group)));
}

/* -------------------------------------------------------------------- */
/** \name Pass Through structural signature
 *
 * A Pass Through folder must leave the generated shader code untouched, which without a GPU is
 * checked by a structural signature of the generated graph: EEVEE hashes the code it builds from
 * these node types, links and unlinked input values, so an equal signature means an equal shader.
 * Names and locations are deliberately absent -- the generator renames and lays out rows freely --
 * and group nodes are followed into their trees, because the shader inlines them.
 * \{ */

/** Append one unlinked socket's default value; the value a constant input feeds the shader. */
static void signature_socket_default(const bNodeSocket &socket, std::string &out)
{
  char buf[160];
  if (socket.default_value == nullptr) {
    out += "?";
    return;
  }
  switch (socket.type) {
    case SOCK_FLOAT: {
      const auto &value = *static_cast<const bNodeSocketValueFloat *>(socket.default_value);
      BLI_snprintf(buf, sizeof(buf), "f%.6g", double(value.value));
      out += buf;
      break;
    }
    case SOCK_INT: {
      const auto &value = *static_cast<const bNodeSocketValueInt *>(socket.default_value);
      BLI_snprintf(buf, sizeof(buf), "i%d", value.value);
      out += buf;
      break;
    }
    case SOCK_BOOLEAN: {
      const auto &value = *static_cast<const bNodeSocketValueBoolean *>(socket.default_value);
      out += value.value ? "t" : "n";
      break;
    }
    case SOCK_VECTOR: {
      const auto &value = *static_cast<const bNodeSocketValueVector *>(socket.default_value);
      BLI_snprintf(buf,
                   sizeof(buf),
                   "v%.6g,%.6g,%.6g",
                   double(value.value[0]),
                   double(value.value[1]),
                   double(value.value[2]));
      out += buf;
      break;
    }
    case SOCK_RGBA: {
      const auto &value = *static_cast<const bNodeSocketValueRGBA *>(socket.default_value);
      BLI_snprintf(buf,
                   sizeof(buf),
                   "c%.6g,%.6g,%.6g,%.6g",
                   double(value.value[0]),
                   double(value.value[1]),
                   double(value.value[2]),
                   double(value.value[3]));
      out += buf;
      break;
    }
    default:
      out += "?";
      break;
  }
}

/**
 * Append the structural signature of \a tree: node types and custom operation fields, each input
 * socket's link state and unlinked value, the link graph by node and socket index, then every group
 * instance's own tree. Deliberately excludes node and socket names and positions.
 */
static void signature_node_tree(const bNodeTree &tree, std::string &out, const int depth)
{
  if (depth > 8) {
    out += "[deep]";
    return;
  }
  tree.ensure_topology_cache();
  Vector<const bNode *> nodes;
  for (const bNode &node : tree.nodes) {
    nodes.append(&node);
  }
  char buf[192];
  BLI_snprintf(buf, sizeof(buf), "T%d[", int(nodes.size()));
  out += buf;
  for (const int i : nodes.index_range()) {
    const bNode &node = *nodes[i];
    BLI_snprintf(buf,
                 sizeof(buf),
                 "N%d/%d{%d,%d,%d,%d|",
                 i,
                 node.type_legacy,
                 node.custom1,
                 node.custom2,
                 node.custom3,
                 node.custom4);
    out += buf;
    int input_index = 0;
    for (const bNodeSocket &socket : node.inputs) {
      if (socket.directly_linked_links().is_empty()) {
        BLI_snprintf(buf, sizeof(buf), "i%d=", input_index);
        out += buf;
        signature_socket_default(socket, out);
      }
      else {
        out += "iL";
      }
      out += ",";
      input_index++;
    }
    int output_count = 0;
    for (const bNodeSocket &socket : node.outputs) {
      UNUSED_VARS(socket);
      output_count++;
    }
    BLI_snprintf(buf, sizeof(buf), "|o%d}", output_count);
    out += buf;
  }
  for (const bNodeLink &link : tree.links) {
    int from_node = -1;
    int to_node = -1;
    int from_socket = -1;
    int to_socket = -1;
    for (const int i : nodes.index_range()) {
      if (nodes[i] == link.fromnode) {
        from_node = i;
      }
      if (nodes[i] == link.tonode) {
        to_node = i;
      }
    }
    if (link.fromnode != nullptr) {
      int index = 0;
      for (const bNodeSocket &socket : link.fromnode->outputs) {
        if (&socket == link.fromsock) {
          from_socket = index;
        }
        index++;
      }
    }
    if (link.tonode != nullptr) {
      int index = 0;
      for (const bNodeSocket &socket : link.tonode->inputs) {
        if (&socket == link.tosock) {
          to_socket = index;
        }
        index++;
      }
    }
    BLI_snprintf(buf, sizeof(buf), "L%d.%d>%d.%d;", from_node, from_socket, to_node, to_socket);
    out += buf;
  }
  for (const bNode *node : nodes) {
    if (node->is_group() && node->id != nullptr && GS(node->id->name) == ID_NT &&
        !BKE_paint_material_is_normal_combine_group(*node))
    {
      out += "G";
      signature_node_tree(*reinterpret_cast<const bNodeTree *>(node->id), out, depth + 1);
    }
  }
  out += "]";
}

static std::string root_signature(bNodeTree &tree)
{
  std::string out;
  signature_node_tree(tree, out, 0);
  return out;
}

/** Group \a member into a fresh folder that takes its slot, as the Outliner's Group does. */
static MaterialPaintLayer *group_one(Material &ma, MaterialPaintLayer *member)
{
  MaterialPaintLayer *members[1] = {member};
  return BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 1));
}

/** A top-level row of \a ma by name, or null. */
static MaterialPaintLayer *find_named_layer(Material &ma, const char *name)
{
  for (MaterialPaintLayer &layer :
       *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&ma.paint_layers))
  {
    if (STREQ(layer.name, name)) {
      return &layer;
    }
  }
  return nullptr;
}

/** \} */

TEST_F(PaintLayersGenerateTest, pass_through_group_and_ungroup_keep_the_root_signature)
{
  add_paint_layer("A", add_image("A"));
  MaterialPaintLayer *b = add_paint_layer("B", add_image("B"));
  add_paint_layer("C", add_image("C"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const std::string before = root_signature(*root);

  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The folder owns no group and no node; the root is the same tree it was before the group. */
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(root_signature(*root), before);

  ASSERT_TRUE(BKE_paint_layers_ungroup(*ma, folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_NE(layer_tree_find(*bmain, "B"), nullptr);
  EXPECT_EQ(root_signature(*root), before);
}

TEST_F(PaintLayersGenerateTest, empty_pass_through_folder_keeps_the_root)
{
  add_paint_layer("A", add_image("A"));
  add_paint_layer("C", add_image("C"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const std::string before = root_signature(*root);

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Empty", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(folder_tree_find(*bmain, "Empty"), nullptr);
  EXPECT_EQ(root_signature(*root), before);
}

TEST_F(PaintLayersGenerateTest, nested_pass_through_folders_keep_the_root_signature)
{
  add_paint_layer("A", add_image("A"));
  add_paint_layer("B", add_image("B"));
  add_paint_layer("C", add_image("C"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const std::string before = root_signature(*root);

  MaterialPaintLayer *b = find_named_layer(*ma, "B");
  ASSERT_NE(b, nullptr);
  MaterialPaintLayer *inner = group_one(*ma, b);
  ASSERT_NE(inner, nullptr);
  MaterialPaintLayer *outer = group_one(*ma, inner);
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *inner));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *outer));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(root_signature(*root), before);
}

TEST_F(PaintLayersGenerateTest, pass_through_visibility_is_a_value_edit)
{
  MaterialPaintLayer *b = add_paint_layer("B", add_image("B"));
  add_paint_layer("A", add_image("A"));
  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> nodes_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "B");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "B Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *opacity = group_instance_input(*root, *group, *iface);
  ASSERT_NE(opacity, nullptr);
  ASSERT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 1.0f);

  /* Hiding the folder scales the child's factor through its value input: the root's nodes and links
   * and the child's group are the same objects, only the value socket moves. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(nodes_before, root_nodes(*root)));
  EXPECT_EQ(layer_tree_find(*bmain, "B"), group);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 0.0f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, true));
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 1.0f);
}

TEST_F(PaintLayersGenerateTest, pass_through_folder_mask_switches_to_isolating)
{
  MaterialPaintLayer *b = add_paint_layer("B", add_image("B"));
  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);

  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, folder, 0.5f);
  ASSERT_NE(item, nullptr);
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The mask forces the isolated form and the folder's own group appears. */
  EXPECT_NE(folder_tree_find(*bmain, "Folder"), nullptr);
}

TEST_F(PaintLayersGenerateTest, pass_through_folder_opacity_and_blend_switch_modes)
{
  MaterialPaintLayer *b = add_paint_layer("B", add_image("B"));
  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  const std::string before = root_signature(*root);
  ASSERT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(folder_tree_find(*bmain, "Folder"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 1.0f));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(root_signature(*root), before);

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(folder_tree_find(*bmain, "Folder"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MIX));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(root_signature(*root), before);
}

TEST_F(PaintLayersGenerateTest, pass_through_folder_with_a_bake_stays_isolating)
{
  MaterialPaintLayer *b = add_paint_layer("B", add_image("B"));
  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*folder);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("BakedColor");
  bake->coverage = add_image("BakedCoverage");
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*folder, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));

  /* A substituted folder is the baked result, not its live children, so the mode is off. */
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(folder_tree_find(*bmain, "Folder"), nullptr);
}

/**
 * The signature must also survive grouping a Material row whose source has its Principled inside a
 * group with an interface: the wrapper is a group instance in the root, so its stable identity is
 * exactly what the structural equality has to see unchanged.
 */
TEST_F(PaintLayersGenerateTest, pass_through_folder_around_a_material_row_keeps_the_signature)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  BKE_paint_layers_active_set(*ma, mat->marker);
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *mat), PaintLayerMaterialMode::SourceGroup);
  bNodeTree *wrapper = source_wrapper_find(*bmain, "HashSource");
  ASSERT_NE(wrapper, nullptr);

  bNodeTree *root = ma->paint_layers_tree;
  const std::string before = root_signature(*root);

  MaterialPaintLayer *folder = group_one(*ma, mat);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(source_wrapper_find(*bmain, "HashSource"), wrapper);
  EXPECT_EQ(root_signature(*root), before);
}

/* -------------------------------------------------------------------- */
/** \name Correction opacity through the RNA slider
 * \{ */

namespace {

/**
 * Set a per (row, channel) opacity exactly the way the Outliner's value slider does: through the
 * #MaterialPaintLayerChannelSettings RNA property, whose setter has to resolve the owning row first.
 * A content correction's slider must therefore reach the row, which lives in its parent's effects.
 */
void rna_set_channel_opacity(Material &ma,
                             MaterialPaintLayer &layer,
                             const int channel,
                             const float percent)
{
  PointerRNA ptr = RNA_pointer_create_discrete(
      &ma.id,
      RNA_struct_find("MaterialPaintLayerChannelSettings"),
      &layer.channel_settings[channel]);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "opacity");
  ASSERT_NE(prop, nullptr);
  RNA_property_float_set(&ptr, prop, percent);
}

/** Set a row's own opacity the way the Outliner does for a mask item; the pointer is the row. */
void rna_set_row_opacity(Material &ma, MaterialPaintLayer &layer, const float percent)
{
  PointerRNA ptr = RNA_pointer_create_discrete(
      &ma.id, RNA_struct_find("MaterialPaintLayer"), &layer);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "opacity");
  ASSERT_NE(prop, nullptr);
  RNA_property_float_set(&ptr, prop, percent);
}

}  // namespace

/**
 * A content correction's opacity slider is a per (row, channel) setting, and the RNA setter has to
 * find the correction -- an element of its parent's `effects` -- before it can write it. This is the
 * reported bug: the slider was a no-op for a Paint correction, while a mask item (whose slider uses
 * the row pointer) worked. The check is the generated group input the row's factor reads.
 */
TEST_F(PaintLayersGenerateTest, content_correction_opacity_rna_reaches_its_graph_input)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  BKE_paint_layers_active_set(*ma, mat->marker);

  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, mat, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(corr, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, corr, MA_PAINT_LAYER_SOURCE_IMAGE));

  MaterialPaintLayer *mask_item = BKE_paint_layers_mask_add(*ma, mat, 1.0f);
  ASSERT_NE(mask_item, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *mat), PaintLayerMaterialMode::SourceGroup);
  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *group = layer_tree_find(*bmain, "Mat");
  ASSERT_NE(group, nullptr);

  auto socket_value = [&](const char *name) -> float {
    bNodeTreeInterfaceSocket *iface = group_input_find(*group, name);
    EXPECT_NE(iface, nullptr) << name;
    bNodeSocket *socket = group_instance_input(*root, *group, *iface);
    EXPECT_NE(socket, nullptr) << name;
    return static_cast<bNodeSocketValueFloat *>(socket->default_value)->value;
  };
  const char *corr_socket = "Mat C Base Color Opacity";
  const char *mask_socket = "Mat Mask Base Color Opacity";
  EXPECT_FLOAT_EQ(socket_value(corr_socket), 1.0f);
  EXPECT_FLOAT_EQ(socket_value(mask_socket), 1.0f);

  /* The content correction's per-channel slider, exactly as the RNA setter runs it. */
  rna_set_channel_opacity(*ma, *corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 25.0f);
  EXPECT_FLOAT_EQ(socket_value(corr_socket), 0.25f);
  /* A value edit: neither the root nor the row's group is rebuilt. */
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Mat"), group);

  /* Visibility of the correction folds into the same factor input. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, corr, false));
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(socket_value(corr_socket), 0.0f);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, corr, true));
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(socket_value(corr_socket), 0.25f);

  /* The mask item's row slider keeps working through the row pointer. */
  rna_set_row_opacity(*ma, *mask_item, 40.0f);
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(socket_value(mask_socket), 0.4f);
}

/** The same slider on a plain Paint row's content correction, so the fix does not regress it. */
TEST_F(PaintLayersGenerateTest, paint_row_correction_opacity_rna_reaches_its_graph_input)
{
  MaterialPaintLayer *paint = add_paint_layer("Paint", add_image("Paint"));
  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, paint, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(corr, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, corr, MA_PAINT_LAYER_SOURCE_IMAGE));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "Paint C Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*root, *group, *iface);
  ASSERT_NE(socket, nullptr);
  ASSERT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 1.0f);

  rna_set_channel_opacity(*ma, *corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 30.0f);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.3f);
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Paint"), group);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ТЗ-29: no bake, no schedule -- the light-row gate
 *
 * A non-folder row's bake structure may only be allocated the instant it is about to render or be
 * queued, never eagerly. A row light enough to stay live (#PAINT_LAYERS_AUTO_BAKE_NODES) keeps
 * #MaterialPaintLayer::bake null forever: it is a final state, not a step toward a bake. A
 * structurally isolating folder follows the same rule (F2-D); a Pass Through folder is never a
 * candidate.
 * \{ */

/** Test #1: a light Paint row's bake stays null after the synchronous planner runs. */
TEST_F(PaintLayersGenerateTest, light_row_bake_ensure_leaves_no_bake_structure)
{
  /* One Base Color channel: weight 4 + 1 * 6 == 10, well under the AUTO threshold of 24. */
  MaterialPaintLayer *light = add_paint_layer("Light", add_image("LightImg"));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *light));

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(light->bake, nullptr);
  /* row_is_substituted's own rule for a non-Material row is exactly this. */
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *light));
}

/**
 * Test #2, defect A: a light row's opacity is a value edit, routed the way the Outliner's slider
 * runs it (#rna_set_row_opacity), not the raw C API. #paint_layers_tag_value_edited (untouched by
 * this task) only tags #MA_PAINT_LAYERS_REGEN when #paint_layer_or_ancestor_has_bake sees a non-null
 * #MaterialPaintLayer::bake on the edited row or an ancestor. A rejected "allocate up front for
 * every non-folder row" route would leave this light row's bake non-null (unrendered, invalid) and
 * make every future opacity edit force a needless topology rebuild; this test fails under that route
 * by both symptoms it names: the bake pointer itself, and the group/root staying unchanged.
 */
TEST_F(PaintLayersGenerateTest, light_row_opacity_edit_does_not_regen_or_rebuild)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.6f));

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_EQ(bottom->bake, nullptr)
      << "a naive up-front allocation for every non-folder row would leave a bake structure here";
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0);

  /* The RNA path: MaterialPaintLayer.opacity, exactly the Outliner's slider. */
  rna_set_row_opacity(*ma, *bottom, 42.0f);

  EXPECT_EQ(bottom->bake, nullptr);
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "an opacity-only edit on a permanently bakeless row must not tag a topology rebuild";

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.6f));
}

/**
 * Test #4, defect Б: two light rows that never get a bake structure must not stall the
 * #MA_PAINT_LAYERS_BAKE_STALE drain -- the pending scan (unchanged by this task) already skips a
 * row with a null bake, so it is blind to them, exactly as it must be. One real candidate (ALWAYS
 * mode, explicit size) actually bakes and is the only row the drain has to see settle. A rejected
 * "allocate up front" route would leave the two light rows with a non-null, permanently-invalid
 * bake, which the pending scan *does* see -- the drain would never clear.
 */
TEST_F(PaintLayersGenerateTest, bake_stale_drains_with_permanently_bakeless_light_rows)
{
  add_paint_layer("LightA", add_image("LightA"));
  add_paint_layer("LightB", add_image("LightB"));
  MaterialPaintLayer *real = add_paint_layer("RealCandidate", add_image("RealCandidateImg"));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *real, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *real, 4));

  ma->paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *real));
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*ma))
      << "two permanently bakeless light rows must not stall the stale drain";
}

/**
 * Test #5: RNA-parity guard. #BKE_paint_layers_bake_mode_get already reads a null bake as AUTO; a
 * light row's mode must still read AUTO after the planner has seen it once (and left the bake null).
 */
TEST_F(PaintLayersGenerateTest, light_row_mode_get_stays_auto_before_and_after_the_planner)
{
  MaterialPaintLayer *light = add_paint_layer("ModeLight", add_image("ModeLightImg"));
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*light), MA_PAINT_LAYER_BAKE_AUTO);

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_EQ(light->bake, nullptr);
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*light), MA_PAINT_LAYER_BAKE_AUTO);
}

/**
 * Test #7: regression guard for the existing rule that the active row and its ancestors stay live
 * regardless of weight -- #BKE_paint_layers_bake_row_is_deferred is checked before the weight gate in
 * both branches of #BKE_paint_layers_bake_ensure, so an active heavy row is never touched (no
 * allocation either), the same as before this task.
 */
TEST_F(PaintLayersGenerateTest, active_heavy_row_stays_live_and_gets_no_bake)
{
  MaterialPaintLayer *paint = add_paint_layer("ActiveHeavy", add_image("ActiveHeavyImg"));
  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, paint, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(corr, nullptr);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR})
  {
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, corr, channel), nullptr);
  }
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *paint));

  BKE_paint_layers_active_set(*ma, paint->marker);
  ASSERT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *paint));

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(paint->bake, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma))
      << "the deferred gate must keep the active row out of the heavy queue too";
}

/** The session uids of every map a row's bake currently owns. */
static Vector<uint32_t> bake_map_session_uids(const MaterialPaintLayer &layer)
{
  Vector<uint32_t> uids;
  for (const Image *image : layer.bake->images) {
    if (image != nullptr) {
      uids.append(image->id.session_uid);
    }
  }
  if (layer.bake->coverage != nullptr) {
    uids.append(layer.bake->coverage->id.session_uid);
  }
  return uids;
}

/**
 * Test #9 (rewritten from #baked_row_that_becomes_light_keeps_its_stale_bake): an AUTO row that
 * later drops under the weight threshold no longer passes the heavy gate, so the synchronous planner
 * releases its bake structure and the service maps only the bake owned. The stale map used to be
 * pinned forever: never revalidated, never freed, and -- through
 * #paint_layer_or_ancestor_has_bake -- it made every value edit on the row tag
 * #MA_PAINT_LAYERS_REGEN. The canary's old expectation (bake != null) is exactly the defect, so the
 * test now asserts the corrected behavior.
 */
TEST_F(PaintLayersGenerateTest, baked_row_that_becomes_light_drops_its_bake)
{
  MaterialPaintLayer *paint = add_paint_layer("BecomesLight", add_image("BecomesLightImg"));
  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, paint, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(corr, nullptr);
  MaterialPaintLayerChannel *corr_channel = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(corr_channel, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *paint, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *paint, 4));

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(paint->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *paint));
  const Vector<uint32_t> bake_uids = bake_map_session_uids(*paint);
  ASSERT_FALSE(bake_uids.is_empty()) << "the bake must have minted service maps";

  /* Switch to AUTO and drop the row under the weight threshold. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *paint, MA_PAINT_LAYER_BAKE_AUTO));
  ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *paint));
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *paint))
      << "the structural edit invalidates the stale hash";

  changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(paint->bake, nullptr);
  for (const uint32_t uid : bake_uids) {
    EXPECT_EQ(BKE_libblock_find_session_uid(bmain, ID_IM, uid), nullptr)
        << "a service map of the dropped bake was left behind";
  }

  /* The row no longer carries a bake, so #paint_layer_or_ancestor_has_bake sees none and a value
   * edit is no longer topology. */
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  rna_set_row_opacity(*ma, *paint, 42.0f);
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "a dropped bake must not keep making value edits topology";
}

/** Guard: a manual ALWAYS choice is the user's; the light gate never releases its bake. */
TEST_F(PaintLayersGenerateTest, always_row_that_becomes_light_keeps_its_bake)
{
  MaterialPaintLayer *paint = add_paint_layer("AlwaysLight", add_image("AlwaysLightImg"));
  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, paint, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(corr, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *paint, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *paint, 4));

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *paint));

  ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *paint));

  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_NE(paint->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *paint));
}

/** Guard: a heavy row keeps its bake; the light gate is not reached for it. */
TEST_F(PaintLayersGenerateTest, heavy_auto_row_keeps_its_bake)
{
  MaterialPaintLayer *heavy = add_paint_layer("HeavyRow", add_image("HeavyRowImg"));
  ASSERT_NE(add_channel(*heavy, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("HeavyRough")), nullptr);
  ASSERT_NE(add_channel(*heavy, PAINT_MATERIAL_CHANNEL_METALLIC, add_image("HeavyMetal")), nullptr);
  ASSERT_NE(add_channel(*heavy, PAINT_MATERIAL_CHANNEL_SPECULAR, add_image("HeavySpec")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *heavy));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *heavy));

  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_NE(heavy->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *heavy));
}

/** F2-D counterpart: an AUTO folder that becomes light releases its bake exactly like a row. */
TEST_F(PaintLayersGenerateTest, auto_folder_that_becomes_light_drops_its_bake)
{
  MaterialPaintLayer *child = add_paint_layer("FChild", add_image("FChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, 4));

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  ASSERT_NE(folder->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  const Vector<uint32_t> bake_uids = bake_map_session_uids(*folder);
  ASSERT_FALSE(bake_uids.is_empty());

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_AUTO));
  /* The mode is not part of the bake hash, so move a value too or the stored bake would still count
   * as valid and the valid-bake gate would return before the light gate. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.25f));
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  ASSERT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *folder));

  changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_EQ(folder->bake, nullptr);
  for (const uint32_t uid : bake_uids) {
    EXPECT_EQ(BKE_libblock_find_session_uid(bmain, ID_IM, uid), nullptr);
  }
}

/* -------------------------------------------------------------------- */
/** \name F2-D: auto-baking structurally isolating folders
 * \{ */

/** F2-D (a): a heavy isolating folder with no bake is queued, and the structure appears only then. */
TEST_F(PaintLayersGenerateTest, heavy_isolating_folder_becomes_a_bake_candidate)
{
  MaterialPaintLayer *child = add_paint_layer("IsChild", add_image("IsChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  /* No structure before the gates pass: the synchronous planner leaves a heavy folder to the job. */
  EXPECT_EQ(folder->bake, nullptr);
  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(folder->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* The queue allocates the structure and takes the row. */
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  EXPECT_NE(folder->bake, nullptr);
  BKE_paint_layers_bake_job_free(*job);
}

/** F2-D (c)/(h): a structurally Pass Through folder is never a candidate, however heavy. */
TEST_F(PaintLayersGenerateTest, pass_through_folder_is_never_a_bake_candidate)
{
  MaterialPaintLayer *child_a = add_paint_layer("PtChildA", add_image("PtChildAImg"));
  MaterialPaintLayer *child_b = add_paint_layer("PtChildB", add_image("PtChildBImg"));
  MaterialPaintLayer *members[2] = {child_a, child_b};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(folder->bake, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
  ASSERT_EQ(BKE_paint_layers_bake_job_create(*bmain, *ma), nullptr);
}

/** F2-D (d): a structurally Pass Through folder carrying a manual bake keeps its old path. */
TEST_F(PaintLayersGenerateTest, pass_through_folder_with_a_manual_bake_is_unchanged)
{
  MaterialPaintLayer *child = add_paint_layer("PtManChild", add_image("PtManChildImg"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, 4));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder))
      << "a manual mode and size do not bake pixels yet, so the folder is still structurally Pass "
         "Through";

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder))
      << "the baked result now stands in, exactly as before";
}

/** F2-D (e): the active row inside a folder keeps the folder live, so no bake is allocated. */
TEST_F(PaintLayersGenerateTest, active_child_defers_its_folder_bake)
{
  MaterialPaintLayer *child = add_paint_layer("ActChild", add_image("ActChildImg"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, child->marker);
  ASSERT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(folder->bake, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
}

/** F2-D (g): a folder whose mode is NEVER is never a candidate. */
TEST_F(PaintLayersGenerateTest, never_mode_folder_is_not_baked)
{
  MaterialPaintLayer *child = add_paint_layer("NeverChild", add_image("NeverChildImg"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_NEVER));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
  ASSERT_EQ(BKE_paint_layers_bake_job_create(*bmain, *ma), nullptr);
}

/** F2-D (f): a light isolating folder never gets a structure, so a child opacity edit is free. */
TEST_F(PaintLayersGenerateTest, light_isolating_folder_stays_bakeless_and_does_not_regen)
{
  MaterialPaintLayer *child = add_paint_layer("LightChild", add_image("LightChildImg"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(folder->bake, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));

  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  rna_set_row_opacity(*ma, *child, 42.0f);
  EXPECT_EQ(folder->bake, nullptr);
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "a value edit under a permanently-bakeless folder must not tag a topology rebuild";
}

/**
 * F2-D2 (step 1), guard. The reference for a row is #visibility_does_not_invalidate_the_rows_own_bake
 * (a baked row's own hash moves on a value edit) plus #light_row_opacity_edit_does_not_regen_or_rebuild
 * (`rna_set_row_opacity` is the Outliner path; #paint_layer_or_ancestor_has_bake decides
 * #MA_PAINT_LAYERS_REGEN). Here the same sequence runs for the child of a heavy, auto-baked,
 * isolating folder: the edit invalidates the folder's bake, tags stale/regen, and the drain restores
 * it.
 */
TEST_F(PaintLayersGenerateTest, auto_baked_folder_child_value_edit_invalidates_and_drains)
{
  MaterialPaintLayer *child = add_paint_layer("EditChild", add_image("EditChildImg"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  /* Bake the folder through the real heavy job (F2-D). */
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  uint32_t before[2];
  BKE_paint_layers_bake_hash(*folder, before);

  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BAKE_STALE);
  rna_set_row_opacity(*ma, *child, 42.0f);

  /* The child's opacity is part of the folder's hash, so the same flags a baked row gets are set. */
  uint32_t after[2];
  BKE_paint_layers_bake_hash(*folder, after);
  EXPECT_TRUE(before[0] != after[0] || before[1] != after[1])
      << "the child's value edit must move the folder's bake hash";
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0);
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "a baked ancestor makes the child's value edit topology";
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* Drain: re-bake through the job, then the planner clears the stale signal. */
  job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*ma));
}

/**
 * F2-D2 (step 2), guard. A value edit on a sibling row outside the folder does not touch the
 * folder's bake and never tags a rebuild: no bake covers the sibling.
 */
TEST_F(PaintLayersGenerateTest, editing_a_sibling_outside_the_folder_leaves_the_bake_alone)
{
  MaterialPaintLayer *child = add_paint_layer("SibChild", add_image("SibChildImg"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  MaterialPaintLayer *outside = add_paint_layer("Outside", add_image("OutsideImg"));
  ASSERT_NE(outside, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  uint32_t before[2];
  BKE_paint_layers_bake_hash(*folder, before);

  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BAKE_STALE);
  rna_set_row_opacity(*ma, *outside, 33.0f);

  uint32_t after[2];
  BKE_paint_layers_bake_hash(*folder, after);
  EXPECT_TRUE(before[0] == after[0] && before[1] == after[1])
      << "a sibling's value must not move the folder's bake hash";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "no bake covers the sibling, so its value edit must stay the free path";
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampler budget
 *
 * The counter reproduces EEVEE's `gpu_node_graph.cc` rules; these tests pin its arithmetic, then
 * exercise the fallback: hidden rows are dropped first, then live rows are pinned to their bakes.
 * \{ */

namespace {

/** An Image Texture node reading \a image with the given sampling, linked by the caller. */
bNode *add_tex_image_node(bNodeTree &tree,
                          Image &image,
                          const int interpolation,
                          const int projection)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  if (node == nullptr) {
    return nullptr;
  }
  node->id = &image.id;
  id_us_plus(&image.id);
  NodeTexImage *storage = static_cast<NodeTexImage *>(node->storage);
  storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
  storage->interpolation = interpolation;
  storage->projection = projection;
  return node;
}

bNodeSocket *out_socket(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_OUT, UString::from_ptr_noinline(name));
}

bNodeSocket *in_socket(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_IN, UString::from_ptr_noinline(name));
}

/** Three distinct image samplers feeding a Principled's Base Color: a live-worthy source. */
void source_set_three_image_base_color(Material &source,
                                       Image &a,
                                       Image &b,
                                       Image &c)
{
  bNodeTree &tree = *source.nodetree;
  bNode *principled = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  bNodeSocket *base = in_socket(*principled, "Base Color");
  bNode *na = add_tex_image_node(tree, a, SHD_INTERP_LINEAR, SHD_PROJ_BOX);
  bNode *nb = add_tex_image_node(tree, b, SHD_INTERP_LINEAR, SHD_PROJ_BOX);
  bNode *nc = add_tex_image_node(tree, c, SHD_INTERP_LINEAR, SHD_PROJ_BOX);
  bke::node_add_link(tree, *na, *out_socket(*na, "Color"), *principled, *base);
  bke::node_add_link(tree, *nb, *out_socket(*nb, "Color"), *na, *in_socket(*na, "Vector"));
  bke::node_add_link(tree, *nc, *out_socket(*nc, "Color"), *nb, *in_socket(*nb, "Vector"));
}

}  // namespace

TEST_F(PaintLayersGenerateTest, sampler_count_follows_the_eevee_rules)
{
  Material *source = add_principled_source("CountSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNodeSocket *base = in_socket(*principled, "Base Color");
  Image *shared = add_image("Shared");
  const int baseline = BKE_paint_layers_sampler_count(*source);

  /* One image in two nodes with the same sampler state -- the second reachable through the first's
   * Vector input -- is one sampler (`gpu_node_graph.cc:507-514` dedups by image and state). */
  bNode *a = add_tex_image_node(tree, *shared, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  bNode *b = add_tex_image_node(tree, *shared, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  bke::node_add_link(tree, *a, *out_socket(*a, "Color"), *principled, *base);
  bke::node_add_link(tree, *b, *out_socket(*b, "Color"), *a, *in_socket(*a, "Vector"));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);

  /* Closest changes the filtering, so the same image becomes a second sampler. */
  static_cast<NodeTexImage *>(b->storage)->interpolation = SHD_INTERP_CLOSEST;
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 2);
}

TEST_F(PaintLayersGenerateTest, sampler_count_udim_is_two)
{
  Material *source = add_principled_source("UdimSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  Image *udim = add_image("Udim");
  udim->source = IMA_SRC_TILED;
  const int baseline = BKE_paint_layers_sampler_count(*source);
  bNode *node = add_tex_image_node(tree, *udim, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  ASSERT_NE(node, nullptr);
  bke::node_add_link(
      tree, *node, *out_socket(*node, "Color"), *principled, *in_socket(*principled, "Base Color"));
  /* A tiled image needs its tile mapping array: one image sampler plus one mapping sampler. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 2);
}

TEST_F(PaintLayersGenerateTest, sampler_count_colorband_nodes_share_one)
{
  Material *source = add_principled_source("BandSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNodeSocket *base = in_socket(*principled, "Base Color");
  const int baseline = BKE_paint_layers_sampler_count(*source);

  bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNode *ramp = bke::node_add_static_node(nullptr, tree, SH_NODE_VALTORGB);
  bNode *curves = bke::node_add_static_node(nullptr, tree, SH_NODE_CURVE_RGB);
  ASSERT_NE(value, nullptr);
  ASSERT_NE(ramp, nullptr);
  ASSERT_NE(curves, nullptr);
  bke::node_add_link(tree, *value, *out_socket(*value, "Value"), *ramp, *in_socket(*ramp, "Fac"));
  bke::node_add_link(tree, *ramp, *out_socket(*ramp, "Color"), *curves, *in_socket(*curves, "Color"));
  bke::node_add_link(tree, *curves, *out_socket(*curves, "Color"), *principled, *base);
  /* Every colorband node shares the single per-material ramp texture: one sampler, not two. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);
}

TEST_F(PaintLayersGenerateTest, sampler_count_ignores_muted_and_unconnected)
{
  Material *source = add_principled_source("ReachSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNodeSocket *base = in_socket(*principled, "Base Color");
  const int baseline = BKE_paint_layers_sampler_count(*source);
  Image *image = add_image("Reach");
  bNode *node = add_tex_image_node(tree, *image, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  bke::node_add_link(
      tree, *node, *out_socket(*node, "Color"), *principled, *base);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);

  /* A muted node is not compiled. */
  node->flag |= NODE_MUTED;
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline);
  node->flag &= ~NODE_MUTED;

  /* A branch that reaches no output is not compiled either. */
  add_tex_image_node(tree, *add_image("Unlinked"), SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_under_limit_leaves_modes_untouched)
{
  Material *source = add_principled_source("NoOverSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("NoOverA"), *add_image("NoOverB"), *add_image("NoOverC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("NoOverBaked")));
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(1000, 1000);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.sampler_budget_exceeded);
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_EQ(report.material_rows.size(), 1u);
  EXPECT_EQ(report.material_rows[0].refusal, PaintLayersSourceGroupRefusal::None);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_cleanup_drops_hidden_pass_through)
{
  add_paint_layer("A", add_image("CleanupA"));
  MaterialPaintLayer *b = add_paint_layer("B", add_image("CleanupB"));
  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));

  /* Without a budget the hidden folder stays: it is a value edit. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "B"), nullptr);

  /* With a budget that only the visible row fits, the hidden folder and its child are dropped
   * instead of refusing the visible material. */
  BKE_paint_layers_sampler_budget_set(1, 1);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.sampler_budget_exceeded);
  EXPECT_EQ(layer_tree_find(*bmain, "B"), nullptr);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_NE(layer_tree_find(*bmain, "A"), nullptr);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_falls_back_a_live_row_to_its_bake)
{
  Material *source = add_principled_source("FallbackSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("FallbackA"), *add_image("FallbackB"), *add_image("FallbackC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_NE(BKE_paint_layers_bake_ensure(*row), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("FallbackBaked")));
  BKE_paint_layers_bake_finalize(*ma, *row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  const int live_count = BKE_paint_layers_sampler_count(*ma);
  ASSERT_GT(live_count, 2);

  BKE_paint_layers_sampler_budget_set(live_count - 1, live_count);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));

  EXPECT_FALSE(report.sampler_budget_exceeded);
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_EQ(report.material_rows.size(), 1u);
  EXPECT_EQ(report.material_rows[0].refusal, PaintLayersSourceGroupRefusal::TooManyTextures);
  EXPECT_LE(BKE_paint_layers_sampler_count(*ma), live_count - 1);

  /* Re-running without edits must not rebuild again: the forced set is re-derived identically. */
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> before = root_nodes(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(before, root_nodes(*root)));

  /* Raising the budget lifts the pin and the row goes live again. */
  BKE_paint_layers_sampler_budget_set(1000, 1000);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_without_a_bake_keeps_the_graph)
{
  Material *source = add_principled_source("NoBakeSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("NoBakeA"), *add_image("NoBakeB"), *add_image("NoBakeC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(1, 1);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));

  /* Nothing to fall back to: the graph is built as-is and the report carries the warning. */
  EXPECT_TRUE(report.sampler_budget_exceeded);
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_NE(instance_find(), nullptr);
  EXPECT_NE(layer_tree_find(*bmain, "Source"), nullptr);
}

/* -------------------------------------------------------------------- */
/** \name Sampler estimate completeness, gain fallback, loop safety
 * \{ */

namespace {

/** Give \a row a Material bake with \a maps as per-channel images and \a coverage, then finalize. */
void material_bake_set(PaintLayersGenerateTest &t,
                       Material &ma,
                       MaterialPaintLayer &row,
                       const Span<Image *> maps,
                       Image *coverage)
{
  EXPECT_NE(BKE_paint_layers_bake_ensure(row), nullptr);
  for (const int channel : maps.index_range()) {
    if (maps[channel] != nullptr) {
      EXPECT_TRUE(BKE_paint_layers_bake_set_map(ma, row, channel, maps[channel]));
    }
  }
  if (coverage != nullptr) {
    EXPECT_TRUE(BKE_paint_layers_bake_set_map(ma, row, -1, coverage));
  }
  BKE_paint_layers_bake_finalize(ma, row);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(ma, row));
}

}  // namespace

/**
 * The estimate must equal the finished count for a Material row, and the row's mask and correction
 * maps must be part of it in every mode. Arithmetic: the live SourceGroup row embeds a wrapper of
 * three image textures (its default alpha is a constant, so the wrapper coverage adds no sampler),
 * plus its Paint-correction map and its Paint-mask map = 5. Pinned to Baked it becomes the baked
 * Base Color map, the baked coverage and the same two correction maps = 4.
 */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_a_material_row_with_mask_and_correction)
{
  Material *source = add_principled_source("MCSource", 0.3f);
  source_set_three_image_base_color(
      *source, *add_image("MC_A"), *add_image("MC_B"), *add_image("MC_C"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  material_bake_set(*this,
                    *ma,
                    *row,
                    Span<Image *>(std::array<Image *, 1>{add_image("MC_Base")}.data(), 1),
                    add_image("MC_Coverage"));

  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(corr, nullptr);
  MaterialPaintLayerChannel *corr_rec = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(corr_rec, nullptr);
  corr_rec->image = add_image("MC_Corr");
  corr_rec->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, corr, MA_PAINT_LAYER_SOURCE_IMAGE));

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  MaterialPaintLayerChannel *mask_rec = BKE_paint_layers_channel_add(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(mask_rec, nullptr);
  mask_rec->image = add_image("MC_Mask");
  mask_rec->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  /* A map mask is Paint, not the constant Fill that #BKE_paint_layers_mask_add creates. */
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, mask, MA_PAINT_LAYER_SOURCE_IMAGE));

  BKE_paint_layers_active_set(*ma, row->marker);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  const int live_count = BKE_paint_layers_sampler_count(*ma);
  EXPECT_EQ(live_count, 5);

  /* A budget of live-1 forces the pin; the count becomes the four baked/correction maps and the
   * estimate follows it. */
  BKE_paint_layers_sampler_budget_set(live_count - 1, live_count);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 4);
}

/** An isolating folder with a valid bake is its maps: one baked color plus one coverage = 2. */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_an_isolating_folder_bake)
{
  MaterialPaintLayer *child = add_paint_layer("IsChild", add_image("IsChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*folder);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("IsBakedColor");
  bake->coverage = add_image("IsBakedCoverage");
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*folder, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 2);
}

/** A Pass Through folder is inlined: only its child's map counts, one sampler. */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_a_pass_through_folder)
{
  MaterialPaintLayer *child = add_paint_layer("PtChild", add_image("PtChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 1);
}

/**
 * Hybrid: the live part is counted instead of the baked map it shadows. The source's Roughness is a
 * live constant and its Alpha is a constant too, so the row's baked Roughness map and the coverage
 * fallback are both not built: the row contributes no sampler. (The pre-fix estimate counted the
 * baked maps regardless, so it disagreed with the graph.)
 */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_a_hybrid_row)
{
  Material *source = add_principled_source("HybridSource", 0.3f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Hybrid", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_ROUGHNESS] = add_image("HybridBaked");
  material_bake_set(*this, *ma, *row, maps, add_image("HybridCoverage"));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 0);
}

/**
 * One image read by the stack and by a node in the user tree is one sampler: the keys must agree.
 */
TEST_F(PaintLayersGenerateTest, sampler_estimate_dedups_a_map_shared_with_the_user_tree)
{
  Image *shared = add_image("SharedMap");
  add_paint_layer("SharedRow", shared);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_sampler_count(*ma), 1);

  bNode *principled = nullptr;
  for (bNode &node : ma->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);
  bNode *user_tex = add_tex_image_node(*ma->nodetree, *shared, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  ASSERT_NE(user_tex, nullptr);
  bke::node_add_link(*ma->nodetree,
                     *user_tex,
                     *out_socket(*user_tex, "Color"),
                     *principled,
                     *in_socket(*principled, "Metallic"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 1);
}

/**
 * A pin that does not lower the count is not taken: here the live wrapper is one sampler while the
 * baked maps are four, so forcing would raise the count. The row stays live and the report warns.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_keeps_a_row_when_a_pin_would_not_win)
{
  Material *source = add_principled_source("NoWinSource", 0.3f);
  source_set_three_image_base_color(
      *source, *add_image("NoWinA"), *add_image("NoWinB"), *add_image("NoWinC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "NoWin", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* Four baked maps plus coverage (5) cost more than the three-sampler live graph. */
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("NoWinB0");
  maps[PAINT_MATERIAL_CHANNEL_ROUGHNESS] = add_image("NoWinB1");
  maps[PAINT_MATERIAL_CHANNEL_METALLIC] = add_image("NoWinB2");
  maps[PAINT_MATERIAL_CHANNEL_ALPHA] = add_image("NoWinB3");
  material_bake_set(*this, *ma, *row, maps, add_image("NoWinCov"));
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(report.sampler_budget_exceeded);
}

/**
 * The fallback picks the row with the largest saving. Both live wrappers are three samplers; row A's
 * bake is one Base Color map (forcing leaves 1) and row B's is a Base Color map plus coverage (2).
 * So A gains 2 and B gains 1; with a budget of four, the single pin that fits is A.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_pins_the_largest_gain_first)
{
  /* Both sources are a four-sampler wrapper: a three-image Base Color chain plus a Metallic map.
   * The Metallic map keeps each row in SourceGroup even when it also has a baked Base Color map,
   * because Metallic has no baked map and so stays live-eligible. */
  auto make_source = [&](const char *name) -> Material * {
    Material *source = add_principled_source(name, 0.3f);
    char map_name[64];
    BLI_snprintf(map_name, sizeof(map_name), "%sBC0", name);
    Image *i0 = add_image(map_name);
    BLI_snprintf(map_name, sizeof(map_name), "%sBC1", name);
    Image *i1 = add_image(map_name);
    BLI_snprintf(map_name, sizeof(map_name), "%sBC2", name);
    Image *i2 = add_image(map_name);
    source_set_three_image_base_color(*source, *i0, *i1, *i2);
    bNodeTree &tree = *source->nodetree;
    bNode *principled = principled_of(*source);
    BLI_snprintf(map_name, sizeof(map_name), "%sMet", name);
    bNode *metallic = add_tex_image_node(tree, *add_image(map_name), SHD_INTERP_LINEAR, SHD_PROJ_BOX);
    bke::node_add_link(tree,
                       *metallic,
                       *out_socket(*metallic, "Color"),
                       *principled,
                       *in_socket(*principled, "Metallic"));
    return source;
  };
  auto add_row = [&](const char *name, Material *source) -> MaterialPaintLayer * {
    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(row, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
    return row;
  };

  MaterialPaintLayer *row_a = add_row("GainA", make_source("GainA"));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps_a{};
  maps_a[PAINT_MATERIAL_CHANNEL_METALLIC] = add_image("GainAMetBaked");
  material_bake_set(*this, *ma, *row_a, maps_a, nullptr);

  MaterialPaintLayer *row_b = add_row("GainB", make_source("GainB"));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps_b{};
  maps_b[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("GainBBCBaked");
  maps_b[PAINT_MATERIAL_CHANNEL_METALLIC] = add_image("GainBMetBaked");
  material_bake_set(*this, *ma, *row_b, maps_b, nullptr);

  /* Live is 8 (4 + 4). A pinned leaves 1 + 4 = 5, B pinned leaves 4 + 2 = 6. Budget 5 pins A. */
  BKE_paint_layers_sampler_budget_set(5, 8);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row_a));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row_b));
  EXPECT_FALSE(report.sampler_budget_exceeded);
}

/**
 * The whole point of doing the fallback before the wired set: with a budget exceeded, a hidden Pass
 * Through folder and a pinned row, a second regeneration without edits must keep the root.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_settles_without_a_rebuild_loop)
{
  Material *source = add_principled_source("LoopSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("LoopA"), *add_image("LoopB"), *add_image("LoopC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "LoopRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("LoopBaked");
  material_bake_set(*this, *ma, *row, maps, nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  MaterialPaintLayer *hidden_child = add_paint_layer("LoopHidden", add_image("LoopHiddenMap"));
  MaterialPaintLayer *hidden_folder = group_one(*ma, hidden_child);
  ASSERT_NE(hidden_folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *hidden_folder));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, hidden_folder, false));

  /* The live estimate is the wrapper (3) plus the inlined hidden map (1); after the cleanup and the
   * pin it is the single baked Base Color map. */
  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 1);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> before = root_nodes(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(before, root_nodes(*root)));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
}

/**
 * A pinned row is bakeable even while it is the active row; its ancestors stay live. When the source
 * is edited the row does not revive -- it stays on its stale maps until a fresh bake lands.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_pinned_active_row_is_bakeable_and_does_not_revive)
{
  Material *source = add_principled_source("PinnedSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("PinA"), *add_image("PinB"), *add_image("PinC"));
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "PinFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "PinChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, child, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("PinBaked");
  material_bake_set(*this, *ma, *child, maps, nullptr);
  BKE_paint_layers_active_set(*ma, child->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *child));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *child), PaintLayerMaterialMode::Baked);

  /* The pinned row is not shown live, so the planner may bake it even in the active chain. Its
   * ancestor (also a subtree of the active marker) stays deferred and live. */
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *child));
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  /* Edit the source: the bake hash no longer matches, yet the row stays Baked on the stale maps. */
  BKE_paint_layers_bake_ensure(*child)->hash[0] = 0;
  BKE_paint_layers_bake_ensure(*child)->hash[1] = 0;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *child));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *child), PaintLayerMaterialMode::Baked);
}

/* -------------------------------------------------------------------- */
/** \name Material row stays live until its bake is ready
 * \{ */

namespace {

/** Map every channel \a source can show onto \a row, plus coverage, and stamp the bake valid. */
void material_bake_all_channels(PaintLayersGenerateTest &t,
                                Material &ma,
                                MaterialPaintLayer &row,
                                Material &source,
                                const std::string &tag)
{
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&source);
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    /* Alpha has no slot of its own in #MaterialPaintLayerBake::images -- no bake pipeline ever
     * writes there. It is the row's #coverage, passed below, exactly as
     * #BKE_paint_layers_material_bake_apply diverts it. */
    if (channel != int(PAINT_MATERIAL_CHANNEL_ALPHA) &&
        resolve.channels[channel] != ChannelResolution::Unavailable)
    {
      maps[channel] = t.add_image((tag + std::to_string(channel)).c_str());
    }
  }
  material_bake_set(t, ma, row, maps, t.add_image((tag + "Cov").c_str()));
}

/**
 * Hand every channel \a source can show over to \a row through #BKE_paint_layers_material_bake_apply
 * -- the real entry point #material_bake_layered_rows_ensure's before_render callback and
 * #paint_material_layer.cc's hand-over call use, unlike #material_bake_all_channels above (which
 * goes through the low-level #BKE_paint_layers_bake_set_map + an explicit finalize and so cannot
 * catch a regression in #BKE_paint_layers_material_bake_apply's own finalize behaviour). Every
 * available channel is covered so the row can actually reach Baked once claims are released and it
 * is finalized -- a channel left without a map stays live forever
 * (#BKE_paint_layers_material_bake_ready), regardless of the bake hash.
 */
void material_bake_hand_over_all_channels(Main &bmain,
                                          Material &ma,
                                          MaterialPaintLayer &row,
                                          Material &source,
                                          PaintLayersGenerateTest &t,
                                          const std::string &tag)
{
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&source);
  Vector<int> channels;
  Vector<Image *> images;
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    if (channel != int(PAINT_MATERIAL_CHANNEL_ALPHA) &&
        resolve.channels[channel] != ChannelResolution::Unavailable)
    {
      channels.append(channel);
      images.append(t.add_image((tag + std::to_string(channel)).c_str()));
    }
  }
  channels.append(int(PAINT_MATERIAL_CHANNEL_ALPHA));
  images.append(t.add_image((tag + "Cov").c_str()));
  BKE_paint_layers_material_bake_apply(
      bmain, ma, row, 64, channels.as_span(), images.as_span());
  /* #BKE_paint_layers_material_bake_apply diverts every #PAINT_MATERIAL_CHANNEL_ALPHA entry into
   * the row's coverage (its transparency, #layer.bake->coverage) rather than
   * #MaterialPaintLayerBake::images[PAINT_MATERIAL_CHANNEL_ALPHA], which no bake pipeline ever
   * fills for a Material row. #paint_layer_material_source_map answers Alpha from `coverage` for
   * exactly that reason, so the row can settle on Baked once every other channel lands too. */
}

/** Claim or release every map of \a row the way a running bake job does. */
void material_bake_claim(const MaterialPaintLayer &row, const bool claim)
{
  Vector<uint32_t> uids;
  for (const Image *image : row.bake->images) {
    if (image != nullptr) {
      uids.append(image->id.session_uid);
    }
  }
  if (row.bake->coverage != nullptr) {
    uids.append(row.bake->coverage->id.session_uid);
  }
  for (const uint32_t uid : uids) {
    if (claim) {
      BKE_paint_layers_bake_image_pending_add(uid);
    }
    else {
      BKE_paint_layers_bake_image_pending_remove(uid);
    }
  }
}

}  // namespace

/**
 * A Material row that is not in the active chain stays live while its bake cannot be shown, and goes
 * to its maps once, when they have landed. Covers the whole edit cycle: a valid bake, an edit to the
 * source (invalid: the row comes alive, and it is still a candidate for the planner), the hand-over
 * (hash stamped, maps claimed: still live, and no longer a candidate) and the landing (one rebuild).
 */
TEST_F(PaintLayersGenerateTest, inactive_material_row_is_live_until_its_bake_lands)
{
  Material *source = add_principled_source("CycleSource", 0.5f);
  source_set_noise_base_color(*bmain, *source);
  Material *other = add_principled_source("CycleOther", 0.3f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "CycleX", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *next = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "CycleY", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(next, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, next, other));
  material_bake_all_channels(*this, *ma, *row, *source, "CycleXMap");
  material_bake_all_channels(*this, *ma, *next, *other, "CycleYMap");

  /* The planner's own filter for a Material row: neither the mode nor the live state is in it. */
  const auto is_planner_candidate = [&]() {
    return row->bake != nullptr && row->bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
           !BKE_paint_layers_bake_is_valid(*ma, *row) &&
           !BKE_paint_layers_bake_row_is_deferred(*ma, *row);
  };
  const auto stamp = [&]() -> bNodeTree * {
    bNodeTree *group = layer_tree_find(*bmain, "CycleX");
    EXPECT_NE(group, nullptr);
    EXPECT_TRUE(group != nullptr && group_mix_sentinel_set(*group, 0.125f));
    return group;
  };

  BKE_paint_layers_active_set(*ma, next->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_FALSE(is_planner_candidate());
  bNodeTree *group = stamp();
  ASSERT_NE(group, nullptr);

  /* The source is edited: the bake is invalid, the row comes alive in one rebuild, and the planner
   * still sees it. */
  bNodeSocket *roughness = bke::node_find_socket(
      *principled_of(*source), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.9f;
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_TRUE(is_planner_candidate());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_TRUE(is_planner_candidate());
  ASSERT_EQ(layer_tree_find(*bmain, "CycleX"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.125f)) << "the row did not come alive";
  stamp();

  /* Nothing changed: the planner has not run yet, so there is nothing to rebuild. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f));

  /* The hand-over: the hash is stamped and the maps claimed. Still live, and the planner has
   * nothing more to start, so the bake is started once. */
  BKE_paint_layers_bake_finalize(*ma, *row);
  material_bake_claim(*row, true);
  EXPECT_FALSE(is_planner_candidate());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f)) << "the hand-over rebuilt the row";
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f));

  /* The maps land: one rebuild, into Baked. */
  material_bake_claim(*row, false);
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_EQ(layer_tree_find(*bmain, "CycleX"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.125f)) << "the landing did not rebuild the row";
  stamp();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f)) << "a second regeneration rebuilt the row";
  EXPECT_FALSE(is_planner_candidate());
}

/**
 * ТЗ-25b, defect 1: a hand-over (#BKE_paint_layers_material_bake_apply's
 * #BKE_paint_layers_bake_set_map calls, as #material_bake_layered_rows_ensure's before_render does
 * on the main thread ahead of the job) attaches fresh target images to an inactive row before any
 * pixel exists behind them. It must not finalise the row on its own any more, so a job cancelled
 * before it renders anything -- claim released, hash never stamped -- leaves the row live instead of
 * showing it Baked on blank/stale maps. Regenerating again afterward, with nothing else changed,
 * must not keep rebuilding the row: the planner re-queues it only by the normal due/stale rules,
 * covered separately.
 */
TEST_F(PaintLayersGenerateTest, cancelled_bake_hand_over_leaves_row_live_not_baked)
{
  Material *source = add_principled_source("CancelSource", 0.5f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "CancelRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* Not the active row: the planner is free to bake it. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* A plain Principled source with no texture resolves every channel to a constant, so the row is
   * live in Hybrid (a live value, no sampler), not SourceGroup (a wrapper instance) -- either mode
   * is "live", which is all this test needs, but the assertion must match reality. */
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *group = layer_tree_find(*bmain, "CancelRow");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.25f));

  /* Hand-over through the real entry point a material bake hands its result over with
   * (#BKE_paint_layers_material_bake_apply -- exactly what #material_bake_layered_rows_ensure's
   * before_render callback and #paint_material_layer.cc's hand-over call), the way it runs before
   * any pixel exists behind the images: claimed right after, never finalized. Before the fix this
   * function finalized on its own, which is the bug this test must catch. Every available channel
   * is covered so the row can actually reach Baked later. */
  material_bake_hand_over_all_channels(*bmain, *ma, *row, *source, *this, "CancelMap");
  material_bake_claim(*row, true);
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "BKE_paint_layers_material_bake_apply must not stamp the bake valid before the render "
         "lands -- it used to finalize the row itself";
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid)
      << "still live: the in-flight claim keeps it off its incomplete maps too";
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.25f))
      << "the hand-over must not have rebuilt the row's group a second time";

  /* Cancellation: #material_bake_images_free always releases the claim, but (the fix) never
   * stamps the hash since it was never finalized -- the row cannot look valid over pixels that
   * were never written. */
  material_bake_claim(*row, false);
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid)
      << "a cancelled bake must not turn the row Baked over pixels that were never written";
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* No busy loop: an idle regeneration after the cancel, with nothing else changed, rebuilds
   * nothing further. */
  group_mix_sentinel_set(*group, 0.25f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.25f))
      << "an idle regeneration after the cancel must not keep rebuilding the row";
}

/**
 * ТЗ-25b, defect 2: the row's topology hash used to fold in the identity of its bake-map Image
 * data-blocks (#paint_layer_channel_image, which for a Material row is
 * #paint_layer_material_source_map -- #MaterialPaintLayer::bake::images) unconditionally, even for
 * a channel the generator shows live from the source and never reads that map for (see the build's
 * Hybrid branch, which `continue`s past #paint_layer_channel_image whenever
 * #BKE_paint_layers_material_live_constant or #BKE_paint_layers_material_live_image answers). A
 * first bake's hand-over mints brand-new Image data-blocks for a row that had none, which used to
 * change the hash and force a rebuild the graph did not need. This is the same hand-over as the
 * cancellation test above, carried through to a successful landing: at most one rebuild for the
 * hand-over (none, with the fix) plus exactly one for the landing into Baked.
 */
TEST_F(PaintLayersGenerateTest, first_bake_hand_over_does_not_force_an_extra_rebuild)
{
  Material *source = add_principled_source("FirstBakeSource", 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "FirstBakeRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* Active elsewhere from the start: the row has never been baked (no map yet), so it is live. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* A plain Principled source with no texture resolves every channel to a constant: Hybrid, not
   * SourceGroup -- both are "live", which is all this step needs. */
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *group = layer_tree_find(*bmain, "FirstBakeRow");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.375f));

  /* First bake's hand-over through the real entry point (#BKE_paint_layers_material_bake_apply):
   * brand-new Image data-blocks land in the row's bake slots for every available channel. Each
   * channel stays live (unfinalized, then claimed), so the generator still does not reference these
   * maps -- the fix must not rebuild the row's group over their mere identity. */
  material_bake_hand_over_all_channels(*bmain, *ma, *row, *source, *this, "FirstBakeMap");
  material_bake_claim(*row, true);
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.375f))
      << "the first bake's hand-over rebuilt the row's group without changing what it reads";

  /* Landing: the claim is released and the render's success finalizes the bake, turning the row
   * Baked -- the one rebuild the row is owed, now that its group must read the new map. */
  material_bake_claim(*row, false);
  BKE_paint_layers_bake_finalize(*ma, *row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_EQ(layer_tree_find(*bmain, "FirstBakeRow"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.375f)) << "landing did not rebuild the row";
  group_mix_sentinel_set(*group, 0.375f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.375f)) << "a second regeneration rebuilt the row";
}

/**
 * A row pinned onto its maps by the sampler budget outranks the new rule: with its bake invalid and
 * the active row elsewhere it is still Baked, on the maps it has. Lifting the budget lets it live.
 */
TEST_F(PaintLayersGenerateTest, sampler_pin_outranks_the_live_rule_for_an_invalid_bake)
{
  Material *source = add_principled_source("PinLiveSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("PinLiveA"), *add_image("PinLiveB"), *add_image("PinLiveC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "PinLiveRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("PinLiveBaked");
  material_bake_set(*this, *ma, *row, maps, nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));

  /* The user leaves the row and the source is edited: invalid, not deferred, still pinned. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  bNodeSocket *roughness = bke::node_find_socket(
      *principled_of(*source), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.8f;
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);

  BKE_paint_layers_sampler_budget_set(0, 0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
}

/**
 * A bake still being rendered is not a candidate for the sampler fallback: pinning the row would put
 * blank maps on screen. Once the maps land the same row is pinned.
 */
TEST_F(PaintLayersGenerateTest, sampler_fallback_waits_for_the_bake_to_land)
{
  Material *source = add_principled_source("WaitSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("WaitA"), *add_image("WaitB"), *add_image("WaitC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "WaitRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("WaitBaked");
  material_bake_set(*this, *ma, *row, maps, nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  material_bake_claim(*row, true);
  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_TRUE(report.sampler_budget_exceeded);

  material_bake_claim(*row, false);
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_FALSE(report.sampler_budget_exceeded);
}

/**
 * Part B (TZ-26): this test used to be named `hybrid_base_color_constant_edit_rebuilds_the_row_group`
 * and asserted a rebuild -- the old hash folded the live constant's own value into the row's
 * topology, on the documented grounds that #BKE_paint_layers_values_sync could not see a value
 * living on another material's node tree. TZ-26 is exactly the fix for that: the constant is now a
 * #ROLE_LIVE_CONSTANT group input, filled by #create_value_inputs and kept current by
 * #values_sync_socket via #BKE_paint_layers_material_live_constant, so moving the source's Base
 * Color must sync in place instead of rebuilding. The name and the body are rewritten together.
 */
TEST_F(PaintLayersGenerateTest, hybrid_base_color_constant_edit_syncs_the_row_group)
{
  Material *source = add_principled_source("PartBSource", 0.4f);
  add_paint_layer("PartBBottom", add_image("PartBBottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "PartB", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "PartB");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.125f));

  bNodeSocket *base = bke::node_find_socket(*principled_of(*source), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base, nullptr);
  static_cast<bNodeSocketValueRGBA *>(base->default_value)->value[0] = 0.9f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f)) << "Base Color is a value now, not topology";
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));

  bNodeTreeInterfaceSocket *iface = group_input_find(*group, "PartB Base Color Source");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *socket = group_instance_input(*ma->paint_layers_tree, *group, *iface);
  ASSERT_NE(socket, nullptr);
  const float *value = static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value;
  EXPECT_NEAR(value[0], 0.9f, 1e-4f);
}

/** \} */

/** Deleting a material drops its sampler runtime state, so a reused uid cannot inherit a pin. */
TEST_F(PaintLayersGenerateTest, sampler_runtime_state_is_dropped_with_its_material)
{
  Material *source = add_principled_source("CleanupSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("ClnA"), *add_image("ClnB"), *add_image("ClnC"));
  Material *other = BKE_material_add(bmain, "OtherLayered");
  other->paint_layers_flag |= MA_PAINT_LAYERED;
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *other, MA_PAINT_LAYER_SOURCE_MATERIAL, "ClnRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*other, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("ClnBaked");
  material_bake_set(*this, *other, *row, maps, nullptr);
  BKE_paint_layers_active_set(*other, row->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *other));
  ASSERT_TRUE(BKE_paint_layers_material_forced_bake(*other, *row));

  const uint32_t reused_uid = other->id.session_uid;
  const bUUID reused_marker = row->marker;
  BKE_id_delete(bmain, other);

  /* A fresh material with the same session uid and a row with the same marker must not report the
   * old pin: the runtime state was keyed by uid and has to have been dropped on free. */
  Material *revived = BKE_material_add(bmain, "RevivedLayered");
  revived->paint_layers_flag |= MA_PAINT_LAYERED;
  revived->id.session_uid = reused_uid;
  MaterialPaintLayer *revived_row = BKE_paint_layers_add(
      *revived, MA_PAINT_LAYER_SOURCE_MATERIAL, "ClnRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(revived_row, nullptr);
  revived_row->marker = reused_marker;
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*revived, *revived_row));
}

/**
 * ТЗ-23: #BKE_paint_layers_material_live_status answers what the UI shows for a Material row --
 * the same function the RNA getter calls. Live while the row shows its source (SourceGroup and
 * Hybrid), Baking while it stays live only because its bake cannot be shown yet, Baked on valid
 * maps, and Refused with the wrapper refusal when the source cannot be wrapped. Every status is
 * reached through the real mode and bake predicates on real rows, not fabricated inputs.
 */
TEST_F(PaintLayersGenerateTest, material_row_live_status)
{
  /* The refusal names the UI prints after "Refused: ". */
  EXPECT_STREQ(BKE_paint_layers_source_group_refusal_name(
                   PaintLayersSourceGroupRefusal::NoPrincipled),
               "no-principled");
  EXPECT_STREQ(BKE_paint_layers_source_group_refusal_name(
                   PaintLayersSourceGroupRefusal::TooManyTextures),
               "too-many-textures");

  /* Live in SourceGroup: a Noise graph the CPU cannot reproduce, shown from the wrapper. */
  Material *noise_source = add_principled_source("StatusNoiseSource", 0.5f);
  source_set_noise_base_color(*bmain, *noise_source);
  MaterialPaintLayer *live_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "StatusLive", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(live_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, live_row, noise_source));
  BKE_paint_layers_active_set(*ma, live_row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *live_row),
            PaintLayerMaterialMode::SourceGroup);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::BuildFailed;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *live_row, &refusal),
            PaintLayerMaterialLiveStatus::Live);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Live in Hybrid: a plain Principled resolves every channel to a constant. */
  Material *const_source = add_principled_source("StatusConstSource", 0.3f);
  MaterialPaintLayer *hybrid_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "StatusHybrid", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(hybrid_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, hybrid_row, const_source));
  BKE_paint_layers_active_set(*ma, hybrid_row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *hybrid_row), PaintLayerMaterialMode::Hybrid);
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *hybrid_row, &refusal),
            PaintLayerMaterialLiveStatus::Live);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Baking: out of the active chain, handed over but still claimed by the job. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  material_bake_hand_over_all_channels(
      *bmain, *ma, *live_row, *noise_source, *this, "StatusMap");
  material_bake_claim(*live_row, true);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *live_row),
            PaintLayerMaterialMode::SourceGroup);
  ASSERT_FALSE(BKE_paint_layers_material_bake_ready(*ma, *live_row));
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *live_row, &refusal),
            PaintLayerMaterialLiveStatus::Baking);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Baked: the claim is released and the render finalizes the bake. */
  material_bake_claim(*live_row, false);
  BKE_paint_layers_bake_finalize(*ma, *live_row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *live_row));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *live_row), PaintLayerMaterialMode::Baked);
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *live_row, &refusal),
            PaintLayerMaterialLiveStatus::Baked);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Refused: a source with no Principled cannot be wrapped, so the row keeps its maps. */
  Material *empty_source = BKE_material_add(bmain, "StatusEmptySource");
  MaterialPaintLayer *refused_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "StatusRefused", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(refused_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, refused_row, empty_source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *refused_row), PaintLayerMaterialMode::Baked);
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *refused_row, &refusal),
            PaintLayerMaterialLiveStatus::Refused);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::NoPrincipled);
}

/**
 * A Material row whose source wrapper failed to build must report Refused/BuildFailed through the
 * Main-free status the UI reads. The failure is a defensive branch of the wrapper factory that valid
 * data cannot reach, so the test seeds the runtime record at the row level -- exactly what
 * #populate_material_rows calls when the factory refuses -- and checks the status read. Clearing the
 * record (what a successful rebuild does) returns the row to Live.
 */
TEST_F(PaintLayersGenerateTest, build_failed_row_reports_refused_without_main)
{
  Material *source = add_principled_source("BuildFailSource", 0.5f);
  source_set_noise_base_color(*bmain, *source);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "BuildFailRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *row, &refusal),
            PaintLayerMaterialLiveStatus::Live);

  BKE_paint_layers_source_group_build_failed_set(*ma, *row, true);
  EXPECT_TRUE(BKE_paint_layers_source_group_build_failed_get(*ma, *row));
  refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *row, &refusal),
            PaintLayerMaterialLiveStatus::Refused);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::BuildFailed);
  EXPECT_STREQ(BKE_paint_layers_source_group_refusal_name(refusal), "build-failed");

  /* Fixing the source drops the record; the status returns to Live. */
  BKE_paint_layers_source_group_build_failed_set(*ma, *row, false);
  refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *row, &refusal),
            PaintLayerMaterialLiveStatus::Live);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
}

/**
 * ТЗ-27: freeing a material drops the generator's runtime state keyed by its `session_uid`, so a
 * reused uid cannot inherit the removed-rows set. The per-marker clear is the observable probe: it
 * reports whether the marker is still recorded.
 */
TEST_F(PaintLayersGenerateTest, removed_rows_state_is_dropped_with_its_material)
{
  /* Two disabled rows: the witness proves the reconcile recorded, the probe survives the delete.
   * Clearing consumes the marker it finds, so one marker cannot serve both roles -- and a second
   * regeneration is not guaranteed to reconcile again, so the probe must stay untouched. */
  MaterialPaintLayer *probe = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "GoneProbe", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *witness = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "GoneWitness", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(probe, nullptr);
  ASSERT_NE(witness, nullptr);
  const bUUID marker = probe->marker;
  const uint32_t uid = ma->id.session_uid;
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, probe, false));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, witness, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The disables were recorded. */
  EXPECT_TRUE(BKE_paint_layers_row_removed_clear(*ma, witness->marker));

  BKE_id_delete(bmain, ma);
  ma = nullptr;

  /* A fresh material reusing the uid must not inherit the probe's entry. */
  Material *revived = BKE_material_add(bmain, "RevivedLayered");
  revived->paint_layers_flag |= MA_PAINT_LAYERED;
  revived->id.session_uid = uid;
  MaterialPaintLayer *revived_row = BKE_paint_layers_add(
      *revived, MA_PAINT_LAYER_SOURCE_IMAGE, "GoneProbe", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(revived_row, nullptr);
  revived_row->marker = marker;
  EXPECT_FALSE(BKE_paint_layers_row_removed_clear(*revived, marker))
      << "a reused session_uid inherited the freed material's removed-rows entry";
}

/** \} */

TEST_F(PaintLayersGenerateTest, mesh_map_row_builds_no_nodes_or_samplers)
{
  /* The row's map type has no atlas assigned yet (no slot image): the row reads nothing, so it
   * contributes nothing -- no Image Texture node, no sampler, no wire from the row (spec M2
   * item 4). With an atlas the row builds a chain; that is covered by the atlas tests. */
  const int samplers_before = BKE_paint_layers_sampler_count(*ma);
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 0);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_before);
  EXPECT_EQ(interface_output_find("Result Base Color"), nullptr);
}

TEST_F(PaintLayersGenerateTest, mesh_map_type_moves_the_topology_hash)
{
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  const uint64_t hash_ao = paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>());

  ASSERT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, mesh_map, MA_MESH_MAP_EDGE));
  const uint64_t hash_edge = paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>());
  EXPECT_NE(hash_ao, hash_edge);
}

namespace {

/** The layer's float Non-Color atlas of \a size, blank content. */
Image *add_atlas_image(Main &bmain, const char *name, const int size)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  return BKE_image_add_generated(
      &bmain, size, size, name, 32, /*floatbuf=*/true, IMA_GENTYPE_BLANK, black, false, true, false);
}

/** Every TEX_IMAGE node of \a tree whose #Image is \a image, descending into layer groups. */
void collect_tex_images_of_image(const bNodeTree &tree, const Image *image, Vector<bNode *> &r_nodes)
{
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &image->id) {
      r_nodes.append(const_cast<bNode *>(&node));
    }
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        !BKE_paint_material_is_normal_combine_group(node))
    {
      collect_tex_images_of_image(*reinterpret_cast<const bNodeTree *>(node.id), image, r_nodes);
    }
  }
}

}  // namespace

/** Guard: the atlas chain is built by the MESH_MAP support in #paint_layers_tree_build (the
 * early-return removal) reading the shared resolver in paint_layers_intern.hh; revert either and
 * this finds no Extend Image Texture for the atlas. */
TEST_F(PaintLayersGenerateTest, mesh_map_row_atlas_builds_an_extend_tex_image)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);

  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  const int samplers_before = BKE_paint_layers_sampler_count(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The row paints the atlas: one Image Texture node carries it, sampled Linear + Extend, and the
   * row takes its sampler. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  Vector<bNode *> tex_images;
  collect_tex_images_of_image(*ma->paint_layers_tree, atlas, tex_images);
  ASSERT_EQ(tex_images.size(), 1);
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(tex_images.first()->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->extension, SHD_IMAGE_EXTENSION_EXTEND);
  EXPECT_EQ(storage->interpolation, SHD_INTERP_LINEAR);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_before + 1);
  EXPECT_NE(interface_output_find("Result Base Color"), nullptr);
}

/** Guard: the corrections' MESH_MAP support in #paint_layers_tree_build (content map node and the
 * mask item's Separate-X grey); revert it and neither element reads the atlas. */
TEST_F(PaintLayersGenerateTest, mesh_map_mask_and_effect_atlas_build_tex_images)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *owner = add_paint_layer("Paint", add_image("Paint"));

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  MaterialPaintLayer *mask_item = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO Mask");
  ASSERT_NE(mask_item, nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The owner's Paint map plus one atlas read for the content correction and one for the mask
   * item; the mask reads its R through a Separate XYZ, so an extra one sits in the tree. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 3);
  Vector<bNode *> tex_images;
  collect_tex_images_of_image(*ma->paint_layers_tree, atlas, tex_images);
  ASSERT_EQ(tex_images.size(), 2);
  for (const bNode *tex : tex_images) {
    const NodeTexImage *storage = static_cast<const NodeTexImage *>(tex->storage);
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->extension, SHD_IMAGE_EXTENSION_EXTEND);
  }
}

/** Guard: the atlas folds into the topology hash by Image identity (the material-aware
 * #topology_hash_layer / #topology_hash_correction) and into the sampler counter by Image; revert
 * either and a pixel edit or a slot re-point is seen wrong. */
TEST_F(PaintLayersGenerateTest, mesh_map_atlas_pixels_do_not_move_the_topology_hash)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_before = paint_layers_layer_topology_hash(
      *ma, *mesh_map, Span<int>(wired, 1));

  /* Repainting the atlas keeps the Image (its session UID) and so the tree: the hash stands. */
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  float *pixels = ibuf->float_data_for_write();
  pixels[0] = 0.75f;
  pixels[1] = 0.25f;
  BKE_image_release_ibuf(atlas, ibuf, lock);
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>(wired, 1)), hash_before);

  /* Pointing the slot at another Image is a structural edit: the hash moves. */
  Image *other = add_atlas_image(*bmain, "Other", 8);
  ASSERT_NE(other, nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, other));
  EXPECT_NE(paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>(wired, 1)), hash_before);
}

/** Guard: the sampler counter deduplicates by Image (the material-aware enumeration in
 * #BKE_paint_layers_regenerate); revert it and a second row on the same atlas takes a sampler. */
TEST_F(PaintLayersGenerateTest, two_mesh_map_rows_share_one_atlas_sampler)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  for (const int i : IndexRange(2)) {
    char name[32];
    BLI_snprintf(name, sizeof(name), "AO %d", i);
    MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, name, nullptr, PaintLayerPlace::Above);
    ASSERT_NE(mesh_map, nullptr);
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
              nullptr);
  }
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  const int samplers_before = BKE_paint_layers_sampler_count(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Both rows read the same atlas Image, so the dedup by Image leaves one sampler. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 2);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_before + 1);
}

/** Guard: the structural resolver (`paint_layer_mesh_map_image` without the loaded-buffer check);
 * restore the check and an assigned-but-unloaded atlas loses its node, its hash mix and its
 * sampler. */
TEST_F(PaintLayersGenerateTest, mesh_map_atlas_without_a_loaded_buffer_still_builds)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The assigned, loaded atlas: one Image Texture node, one sampler. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  const int samplers_loaded = BKE_paint_layers_sampler_count(*ma);
  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_loaded = paint_layers_layer_topology_hash(
      *ma, *mesh_map, Span<int>(wired, 1));

  /* Unload the buffer: the assignment (DNA) is untouched, so nothing structural may move. */
  BKE_image_free_buffers(atlas);
  ASSERT_FALSE(BKE_image_has_loaded_ibuf(atlas));

  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>(wired, 1)), hash_loaded);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_loaded);

  /* A regenerate after the unload still builds the row's node, not an empty tree. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  Vector<bNode *> tex_images;
  collect_tex_images_of_image(*ma->paint_layers_tree, atlas, tex_images);
  EXPECT_EQ(tex_images.size(), 1);
}

}  // namespace blender::bke::tests
