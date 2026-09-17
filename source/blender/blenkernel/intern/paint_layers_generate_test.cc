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
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_scene.hh"

#include "paint_material_composite_internal.hh"

#include "NOD_socket.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include <algorithm>
#include <string>
#include <thread>

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_color_types.h"
#include "DNA_colorband_types.h"

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
        *ma, MA_PAINT_LAYER_KIND_PAINT, name, nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_FILL, "Fill", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Paint", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
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
      *ma, MA_PAINT_LAYER_KIND_FILL, "Fill", nullptr, PaintLayerPlace::Above);
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
      BKE_paint_layers_correction_set_effect(*ma, item_const, MA_PAINT_LAYER_EFFECT_PAINT));
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
      *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
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

TEST_F(PaintLayersGenerateTest, fill_effect_correction_builds_a_constant_mix)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_FILL, "C");
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
      *ma, layer, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_FILL, "C");
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
      *ma, top, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_PAINT, "M");
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
      *ma, layer, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
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
      *ma, top, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_PAINT, "M");
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "ChildA", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *child_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "ChildB", folder, PaintLayerPlace::Into);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Inner", outer, PaintLayerPlace::Into);
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Leaf", inner, PaintLayerPlace::Into);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Base Color only: the root keeps the group I/O, the channel's bottom constant and the group
   * instances. A Normal channel would add its decode-normalize-encode Vector Math at the end. */
  for (bNode &node : ma->paint_layers_tree->nodes) {
    const bool allowed = node.is_group_input() || node.is_group_output() || node.is_group() ||
                         node.type_legacy == SH_NODE_RGB;
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
      *ma, MA_PAINT_LAYER_KIND_FILL, "Fill", nullptr, PaintLayerPlace::Above);
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
      *ma, bottom, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
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
      *ma, bottom, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
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

TEST_F(PaintLayersGenerateTest, live_material_constant_builds_a_constant_source)
{
  Material *source = add_principled_source("LiveSource", 0.42f);
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* A baked map for the row, so the non-live path has something to show. */
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("SourceBake")));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  /* Live: the source's constant is built in, and the row's baked map is not read. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 0);
  EXPECT_GE(count_type(*group, SH_NODE_RGB), 1);

  /* Leaving the row rebuilds its group on the baked map. */
  BKE_paint_layers_active_set(*ma, bottom->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 1);
}

TEST_F(PaintLayersGenerateTest, live_material_constant_is_topology_but_keeps_the_root)
{
  Material *source = add_principled_source("LiveSource", 0.1f);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.125f));

  /* Move the source's constant: the value is part of the layer group's topology. */
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.9f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The group was rebuilt (the stamp is gone), but the root's signature did not change. */
  ASSERT_EQ(layer_tree_find(*bmain, "Source"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.125f));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
}

TEST_F(PaintLayersGenerateTest, material_row_participation_is_stable_across_focus)
{
  Material *source = add_principled_source("StableSource", 0.3f);
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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

}  // namespace

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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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

TEST_F(PaintLayersGenerateTest, source_group_mode_change_keeps_the_root)
{
  Material *source = add_principled_source("ModeHashSource", 0.3f);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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

  /* Constant -> Noise moves the row to SourceGroup: its group rebuilds, the root does not. */
  source_set_noise_base_color(*bmain, *source);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_EQ(layer_tree_find(*bmain, "Source"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.125f));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
}

TEST_F(PaintLayersGenerateTest, source_group_row_stays_on_baked_maps_on_cpu)
{
  Material *source = add_principled_source("CpuSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "ReportRow", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "HybridRow", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "BakedRow", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "SourceA", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "SourceA2", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(material, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, material, source));
  ASSERT_NE(BKE_paint_layers_add(
                *ma, MA_PAINT_LAYER_KIND_PAINT, "PaintChild", folder, PaintLayerPlace::Into),
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
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Outside", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "HashRow", nullptr, PaintLayerPlace::Above);
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
      *layered, MA_PAINT_LAYER_KIND_MATERIAL, "MaskRow", nullptr, PaintLayerPlace::Above);
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
      *layered, MA_PAINT_LAYER_KIND_PAINT, "PaintRow", nullptr, PaintLayerPlace::Above);
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
      *layered, MA_PAINT_LAYER_KIND_MATERIAL, "BakeRow", nullptr, PaintLayerPlace::Above);
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
      *ma, layer, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
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
      *layered, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_KIND_MATERIAL, "MatChild", folder, PaintLayerPlace::Into);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatRow", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatRow", nullptr, PaintLayerPlace::Above);
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
      *ma, bottom, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  BKE_paint_layers_active_set(*ma, mat->marker);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, mat, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
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
      *ma, MA_PAINT_LAYER_KIND_PAINT, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(unnamed, nullptr);
  add_channel(*unnamed, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("UnnamedMap"));

  Material *source = add_principled_source("StackSource", 0.3f);
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Wood", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Empty", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
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
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  BKE_paint_layers_active_set(*ma, mat->marker);

  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, mat, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
  ASSERT_NE(corr, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_correction_set_effect(*ma, corr, MA_PAINT_LAYER_EFFECT_PAINT));

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
      *ma, paint, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
  ASSERT_NE(corr, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_correction_set_effect(*ma, corr, MA_PAINT_LAYER_EFFECT_PAINT));

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

}  // namespace blender::bke::tests
