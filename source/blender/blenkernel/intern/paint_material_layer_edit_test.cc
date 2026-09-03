/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"

#include "MEM_guardedalloc.h"

#include <string>

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_library_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "IMB_imbuf.hh"

namespace blender::bke::tests {

/**
 * A material whose Base Color is a chain of Mix nodes over Image Textures, which is the shape the
 * whole feature is about. Built by hand rather than by the (not yet written) layer-add function so
 * that the tests do not depend on it.
 */
class PaintMaterialLayerEditTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *material = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    material = BKE_material_add(bmain, "Material");
    bNodeTree &tree = *material->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(tree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  bNode &principled_node()
  {
    for (bNode &node : material->nodetree->nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        return node;
      }
    }
    BLI_assert_unreachable();
    return *static_cast<bNode *>(material->nodetree->nodes.first);
  }

  bNode &add_image_texture(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    bNode *node = bke::node_add_static_node(nullptr, *material->nodetree, SH_NODE_TEX_IMAGE);
    node->id = &image->id;
    return *node;
  }

  /** \a layer_num layers on Base Color, named "L0" (the bottom) upwards. */
  void build_stack(const int layer_num)
  {
    bNodeTree &tree = *material->nodetree;
    bNode *below = &add_image_texture("L0");
    bNodeSocket *below_out = bke::node_find_socket(*below, SOCK_OUT, "Color"_ustr);

    for (int i = 1; i < layer_num; i++) {
      char name[16];
      SNPRINTF_UTF8(name, "L%d", i);
      bNode &top = add_image_texture(name);
      bNode *mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX_RGB_LEGACY);
      STRNCPY_UTF8(mix->label, name);
      bke::node_add_link(tree,
                         *below,
                         *below_out,
                         *mix,
                         *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
      bke::node_add_link(tree,
                         top,
                         *bke::node_find_socket(top, SOCK_OUT, "Color"_ustr),
                         *mix,
                         *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
      below = mix;
      below_out = bke::node_find_socket(*mix, SOCK_OUT, "Color"_ustr);
    }
    bNode &principled = principled_node();
    bke::node_add_link(tree,
                       *below,
                       *below_out,
                       principled,
                       *bke::node_find_socket(principled, SOCK_IN, "Base Color"_ustr));
  }

  /** The stack as the reader sees it, bottom to top, by layer name. */
  Vector<std::string> layer_names()
  {
    Vector<PaintMaterialLayerStackEntry> entries;
    BKE_paint_material_layer_stack_from_material(*bmain, *material, entries);
    Vector<std::string> names;
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      names.append(entry.name);
    }
    return names;
  }

  /** The shape of the graph a refused edit has to leave exactly as it was. */
  struct GraphShape {
    int nodes = 0;
    int links = 0;
    int markers = 0;
    int images = 0;
  };

  GraphShape graph_shape()
  {
    GraphShape shape;
    shape.nodes = BLI_listbase_count(&material->nodetree->nodes);
    shape.links = BLI_listbase_count(&material->nodetree->links);
    shape.images = BLI_listbase_count(&bmain->images);
    for (bNode &node : material->nodetree->nodes) {
      if (!BLI_uuid_is_nil(BKE_paint_material_layer_marker_get(node))) {
        shape.markers++;
      }
    }
    return shape;
  }

  void expect_graph_unchanged(const GraphShape &before)
  {
    const GraphShape after = graph_shape();
    EXPECT_EQ(after.nodes, before.nodes);
    EXPECT_EQ(after.links, before.links);
    EXPECT_EQ(after.markers, before.markers);
    EXPECT_EQ(after.images, before.images);
  }
};

TEST_F(PaintMaterialLayerEditTest, markers_are_assigned_once_and_kept)
{
  build_stack(3);
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));

  Vector<bUUID> first_pass;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
      const bUUID marker = BKE_paint_material_layer_marker_get(node);
      EXPECT_FALSE(BLI_uuid_is_nil(marker));
      first_pass.append(marker);
    }
  }
  ASSERT_EQ(first_pass.size(), 2);
  EXPECT_FALSE(BLI_uuid_equal(first_pass[0], first_pass[1]));

  /* A second call must not re-issue identities, or every edit would break the previous one. */
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));
  int index = 0;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
      EXPECT_TRUE(BLI_uuid_equal(BKE_paint_material_layer_marker_get(node), first_pass[index++]));
    }
  }
}

TEST_F(PaintMaterialLayerEditTest, reorder_moves_a_layer_down)
{
  build_stack(4);
  EXPECT_EQ(layer_names()[3], "L3");

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_reorder(*bmain, *material, 3, 1, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 4);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "L3");
  EXPECT_EQ(names[2], "L1");
  EXPECT_EQ(names[3], "L2");
}

TEST_F(PaintMaterialLayerEditTest, reorder_refuses_the_bottom_layer)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_reorder(*bmain, *material, 0, 2, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  /* Refused means untouched, not partly applied. */
  const Vector<std::string> names = layer_names();
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[2], "L2");
}

TEST_F(PaintMaterialLayerEditTest, reorder_refuses_a_shared_chain)
{
  build_stack(3);
  /* A second consumer of an intermediate result: reordering would change what it receives. */
  bNode *extra = bke::node_add_static_node(
      nullptr, *material->nodetree, SH_NODE_MIX_RGB_LEGACY);
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY && STREQ(node.label, "L1")) {
      bke::node_add_link(*material->nodetree,
                         node,
                         *bke::node_find_socket(node, SOCK_OUT, "Color"_ustr),
                         *extra,
                         *bke::node_find_socket(*extra, SOCK_IN, "Color1"_ustr));
      break;
    }
  }

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_reorder(*bmain, *material, 2, 1, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::ChainIsShared);
}

TEST_F(PaintMaterialLayerEditTest, remove_closes_the_chain)
{
  build_stack(4);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_remove(*bmain, *material, 2, &error));

  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 3);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "L1");
  EXPECT_EQ(names[2], "L3");
}

TEST_F(PaintMaterialLayerEditTest, remove_bottom_promotes_the_layer_above)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_remove(*bmain, *material, 0, &error));

  /* L1's map becomes the new bottom, so its Mix node is gone and its name with it. */
  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 2);
  EXPECT_EQ(names[0], "L1");
  EXPECT_EQ(names[1], "L2");
}

TEST_F(PaintMaterialLayerEditTest, remove_refuses_the_only_layer)
{
  build_stack(1);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_remove(*bmain, *material, 0, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  EXPECT_EQ(layer_names().size(), 1);
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_on_empty_material)
{
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
      << int(error);

  /* The first layer of an empty material is the bottom one: a bare Image Texture on Base Color. */
  EXPECT_EQ(ordinal, 0);
  EXPECT_EQ(layer_names().size(), 1);
  EXPECT_TRUE(BKE_paint_material_has_layer_stack(*material));
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_on_top_of_stack)
{
  build_stack(2);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
      << int(error);

  EXPECT_EQ(ordinal, 2);
  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 3);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "L1");
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_at_ordinal)
{
  build_stack(3);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.ordinal = 1;
  params.name = "Inserted";
  int ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
      << int(error);

  EXPECT_EQ(ordinal, 1);
  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 4);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "Inserted");
  EXPECT_EQ(names[2], "L1");
  EXPECT_EQ(names[3], "L2");
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_refuses_the_bottom_ordinal)
{
  build_stack(2);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.ordinal = 0;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  EXPECT_EQ(layer_names().size(), 2);
}

TEST_F(PaintMaterialLayerEditTest, add_layer_rolls_back_on_channel_failure)
{
  build_stack(3);
  /* A second consumer of an intermediate result: the chain cannot be rewritten, and the refusal
   * has to happen before any node is created. */
  bNode *extra = bke::node_add_static_node(nullptr, *material->nodetree, SH_NODE_MIX_RGB_LEGACY);
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY && STREQ(node.label, "L1")) {
      bke::node_add_link(*material->nodetree,
                         node,
                         *bke::node_find_socket(node, SOCK_OUT, "Color"_ustr),
                         *extra,
                         *bke::node_find_socket(*extra, SOCK_IN, "Color1"_ustr));
      break;
    }
  }
  const int node_num_before = BLI_listbase_count(&material->nodetree->nodes);
  const int image_num_before = BLI_listbase_count(&bmain->images);

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::ChainIsShared);
  EXPECT_EQ(BLI_listbase_count(&material->nodetree->nodes), node_num_before);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), image_num_before);
}

TEST_F(PaintMaterialLayerEditTest, add_layer_on_linked_material_cancels)
{
  build_stack(2);
  Library *library = static_cast<Library *>(BKE_id_new(bmain, ID_LI, "Library"));
  material->id.lib = library;

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::NotEditable);

  /* Leave the material local again so that freeing `bmain` does not trip the linked-ID checks. */
  material->id.lib = nullptr;
}

TEST_F(PaintMaterialLayerEditTest, added_layers_share_one_marker_and_layer_id)
{
  build_stack(2);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, nullptr));

  /* The new layer's Mix node carries a marker, which is what lets a later reorder move it in
   * every channel at once. */
  bool marker_found = false;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy != SH_NODE_MIX) {
      continue;
    }
    EXPECT_FALSE(BLI_uuid_is_nil(BKE_paint_material_layer_marker_get(node)));
    marker_found = true;
  }
  EXPECT_TRUE(marker_found);
}

/* -------------------------------------------------------------------- */
/** \name Transactional refusals
 *
 * A refused edit leaves the graph byte-for-byte unchanged: no node created, no link rewritten, no
 * marker handed out -- and, since the shape conversion moved behind the preflight, no bare bottom
 * wrapped in a Mix node either. The stacks here all keep a bare bottom on purpose: that is the
 * shape the old normalize-first prologue used to convert before refusing.
 * \{ */

TEST_F(PaintMaterialLayerEditTest, remove_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_remove(*bmain, *material, 999, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, remove_only_layer_leaves_the_graph_alone)
{
  build_stack(1);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_remove(*bmain, *material, 0, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
  EXPECT_EQ(layer_names().size(), 1);
}

TEST_F(PaintMaterialLayerEditTest, add_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.ordinal = 999;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, add_bottom_ordinal_leaves_the_graph_alone)
{
  build_stack(2);
  const GraphShape before = graph_shape();

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.ordinal = 0;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
  EXPECT_EQ(layer_names().size(), 2);
}

TEST_F(PaintMaterialLayerEditTest, reorder_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_reorder(*bmain, *material, 999, 1, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, reorder_bottom_layer_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_reorder(*bmain, *material, 0, 2, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
  /* Refused means untouched, not partly applied. */
  const Vector<std::string> names = layer_names();
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[2], "L2");
}

TEST_F(PaintMaterialLayerEditTest, move_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_move(
      *bmain, *material, 999, 1, PaintMaterialLayerMovePlace::Above, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, move_into_a_plain_row_is_refused_without_touching_the_graph)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_move(
      *bmain, *material, 1, 0, PaintMaterialLayerMovePlace::Into, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::ChainNotPlain);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, rename_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(
      BKE_paint_material_layer_rename(*bmain, *material, 999, "Renamed", &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, rename_bottom_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_rename(*bmain, *material, 0, "Renamed", &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
  EXPECT_EQ(layer_names()[0], "L0");
}

TEST_F(PaintMaterialLayerEditTest, set_enabled_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_set_enabled(*bmain, *material, 999, false, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, set_enabled_bottom_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_set_enabled(*bmain, *material, 0, false, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, duplicate_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_duplicate(*bmain, *material, 999, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, duplicate_bottom_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_duplicate(*bmain, *material, 0, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, duplicate_group_copies_its_children)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, nullptr, &error));

  int duplicate_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_duplicate(*bmain, *material, 1, &duplicate_ordinal, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 7);
  ASSERT_NE(entries[1].group_tree, nullptr);
  ASSERT_NE(entries[4].group_tree, nullptr);
  EXPECT_NE(entries[1].group_tree, entries[4].group_tree);
  EXPECT_EQ(entries[2].name, "L1");
  EXPECT_EQ(entries[3].name, "L2");
  EXPECT_EQ(entries[5].name, "L1");
  EXPECT_EQ(entries[6].name, "L2");
}

TEST_F(PaintMaterialLayerEditTest, group_duplicate_gives_the_copied_maps_their_own_users)
{
  /* The maps a group duplicate copies are the ones inside its own node tree -- the maps of the
   * layers it holds. A mask on the group itself lives in the *enclosing* tree, next to the group's
   * Mix node, so it is not part of what the copied tree carries. */
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, &group_ordinal, &error));

  const auto base_color_map_of = [](const PaintMaterialLayerStackEntry &entry) {
    return entry.channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
  };

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  Image *original_map = nullptr;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.name == "L1") {
      original_map = base_color_map_of(entry);
      break;
    }
  }
  ASSERT_NE(original_map, nullptr);
  const int users_before = original_map->us;

  int copy_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_duplicate(*bmain, *material, group_ordinal, &copy_ordinal, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* Two rows read as "L1" now; the copy is the one whose map is not the original's. */
  entries.clear();
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  Image *copied_map = nullptr;
  int named_l1 = 0;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.name != "L1") {
      continue;
    }
    named_l1++;
    Image *map = base_color_map_of(entry);
    if (map != nullptr && map != original_map) {
      copied_map = map;
    }
  }
  EXPECT_EQ(named_l1, 2);
  ASSERT_NE(copied_map, nullptr);

  /* The duplicate's maps are its own, and each is counted exactly the way the original is: one
   * texture node, one user -- the one a freshly created data-block already carries. A leftover
   * user on the original would keep it alive for the rest of the session. */
  EXPECT_EQ(original_map->us, users_before);
  EXPECT_EQ(copied_map->us, users_before);
}

TEST_F(PaintMaterialLayerEditTest, mask_add_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_mask_add(*bmain, *material, 999, white, 8, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, mask_add_bottom_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_mask_add(*bmain, *material, 0, white, 8, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, mask_add_uses_requested_initial_color)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;

  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, *material, 1, white, 8, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, *material, 2, black, 8, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 3);
  Image *white_mask = entries[1].channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr);
  Image *black_mask = entries[2].channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr);
  ASSERT_NE(white_mask, nullptr);
  ASSERT_NE(black_mask, nullptr);

  void *white_lock = nullptr;
  ImBuf *white_buffer = BKE_image_acquire_ibuf(white_mask, nullptr, &white_lock);
  ASSERT_NE(white_buffer, nullptr);
  ASSERT_NE(white_buffer->byte_data(), nullptr);
  EXPECT_EQ(white_buffer->byte_data()[0], 255);
  BKE_image_release_ibuf(white_mask, white_buffer, white_lock);

  void *black_lock = nullptr;
  ImBuf *black_buffer = BKE_image_acquire_ibuf(black_mask, nullptr, &black_lock);
  ASSERT_NE(black_buffer, nullptr);
  ASSERT_NE(black_buffer->byte_data(), nullptr);
  EXPECT_EQ(black_buffer->byte_data()[0], 0);
  BKE_image_release_ibuf(black_mask, black_buffer, black_lock);
}

TEST_F(PaintMaterialLayerEditTest, mask_add_to_group_is_listed_on_the_group)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, &group_ordinal, &error));

  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_material_layer_mask_add(*bmain, *material, group_ordinal, white, 8, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  const PaintMaterialLayerStackEntry *group = nullptr;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.ordinal == group_ordinal) {
      group = &entry;
      break;
    }
  }
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group->is_group);
  EXPECT_NE(group->channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr), nullptr);
}

TEST_F(PaintMaterialLayerEditTest, mask_remove_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_mask_remove(*bmain, *material, 999, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, group_add_invalid_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_group_add(
      *bmain, *material, 999, PaintMaterialLayerMovePlace::Above, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, group_add_below_the_bottom_row_lands_under_the_stack)
{
  build_stack(3);
  /* Under a bare base there is nothing to blend with, so the shape conversion comes first -- the
   * same one the Outliner runs when a stack is opened. */
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);

  int group_ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_TRUE(BKE_paint_material_layer_group_add(
      *bmain, *material, 0, PaintMaterialLayerMovePlace::Below, &group_ordinal, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  EXPECT_EQ(group_ordinal, 0);
}

TEST_F(PaintMaterialLayerEditTest, group_make_invalid_range_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(
      BKE_paint_material_layer_group_make(*bmain, *material, 999, 999, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, group_make_reversed_range_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_group_make(*bmain, *material, 2, 1, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, group_make_bottom_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_group_make(*bmain, *material, 0, 1, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, ungroup_bottom_ordinal_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_group_ungroup(*bmain, *material, 0, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, ungroup_plain_row_leaves_the_graph_alone)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_group_ungroup(*bmain, *material, 1, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::NotAStack);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, ungroup_leaves_the_group_tree_a_recognized_orphan)
{
  /* The instances hold the group tree, one per channel; ungroup removes them all. What is left
   * must reach #us == 0, so Clean Up sees it and the next file write drops it. A surplus user
   * from creation would strand it at one -- kept in #Main, invisible to the orphan tools. */
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, nullptr, &error));

  bNodeTree *group_tree = nullptr;
  for (bNode &node : material->nodetree->nodes) {
    if (BKE_paint_material_is_layer_group(node)) {
      group_tree = id_cast<bNodeTree *>(node.id);
      break;
    }
  }
  ASSERT_NE(group_tree, nullptr);
  /* One user per channel instance, and no more: the material stack has a single channel here. */
  EXPECT_EQ(group_tree->id.us, 1);

  ASSERT_TRUE(BKE_paint_material_layer_group_ungroup(*bmain, *material, 1, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  EXPECT_EQ(group_tree->id.us, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Write scope: linked, overridden and shared groups
 *
 * A layer inside a group is edited in the group's own node tree, so the write right follows the
 * tree and not the material. The folder built here holds two rows, at the nested ordinals 1024
 * and 1025, and every refusal below leaves the graph byte-for-byte unchanged.
 * \{ */

TEST_F(PaintMaterialLayerEditTest, edits_inside_a_linked_group_are_refused)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, nullptr, &error));

  bNodeTree *group_tree = nullptr;
  for (bNode &node : material->nodetree->nodes) {
    if (BKE_paint_material_is_layer_group(node)) {
      group_tree = id_cast<bNodeTree *>(node.id);
      break;
    }
  }
  ASSERT_NE(group_tree, nullptr);
  Library *library = static_cast<Library *>(BKE_id_new(bmain, ID_LI, "Library"));
  group_tree->id.lib = library;

  const GraphShape before = graph_shape();
  error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_rename(*bmain, *material, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE + 1, "Renamed", &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::TreeNotEditable);

  error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_remove(*bmain, *material, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::TreeNotEditable);
  expect_graph_unchanged(before);

  /* Leave the tree local again, so that freeing `bmain` does not trip the linked-ID checks. */
  group_tree->id.lib = nullptr;
}

TEST_F(PaintMaterialLayerEditTest, edits_inside_an_overridden_group_are_refused)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, nullptr, &error));

  bNodeTree *group_tree = nullptr;
  for (bNode &node : material->nodetree->nodes) {
    if (BKE_paint_material_is_layer_group(node)) {
      group_tree = id_cast<bNodeTree *>(node.id);
      break;
    }
  }
  ASSERT_NE(group_tree, nullptr);
  /* A minimal stand-in for a real override: the gate only asks whether one is attached, and a
   * real one always carries the linked ID it was made from. */
  IDOverrideLibrary *override = MEM_new<IDOverrideLibrary>(__func__);
  override->reference = &group_tree->id;
  group_tree->id.override_library = override;

  const GraphShape before = graph_shape();
  error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_set_enabled(*bmain, *material, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE, false, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::TreeIsOverride);
  expect_graph_unchanged(before);

  group_tree->id.override_library = nullptr;
  MEM_delete(override);
}

TEST_F(PaintMaterialLayerEditTest, edits_inside_a_shared_group_are_refused)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, nullptr, &error));

  /* A second material whose node tree reaches the same group: editing the folder here would edit
   * a foreign stack there. The copies of the instance nodes share the group tree. */
  Material *twin = id_cast<Material *>(BKE_id_copy(bmain, &material->id));
  ASSERT_NE(twin, nullptr);

  const GraphShape before = graph_shape();
  error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_set_enabled(*bmain, *material, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE, false, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::TreeShared);

  error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_rename(*bmain, *material, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE + 1, "Renamed", &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::TreeShared);
  expect_graph_unchanged(before);

  BKE_id_free(bmain, &twin->id);
}

/** \} */

}  // namespace blender::bke::tests
