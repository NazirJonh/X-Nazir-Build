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
#include "BLI_math_vector.hh"
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

  /** The Mix node of the layer named \a label, or null. */
  bNode *find_layer_node(const char *label)
  {
    for (bNode &node : material->nodetree->nodes) {
      if (node.type_legacy == SH_NODE_MIX && STREQ(node.label, label)) {
        return &node;
      }
    }
    return nullptr;
  }

  /** Add a Fill layer named \a name, filled with \a color, and return where it landed. */
  int add_fill_layer(const float color[4], const char *name)
  {
    PaintMaterialLayerAddParams params;
    params.image_size = 8;
    params.type = PaintMaterialLayerAddType::Fill;
    copy_v4_v4(params.fill_color, color);
    params.name = name;
    int ordinal = -1;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    EXPECT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
        << int(error);
    return ordinal;
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
/** \name Layer kinds
 *
 * What a layer *is* -- Paint, Fill, Material -- as a marker on the same nodes the identity marker
 * lives on. A node without one reads as Paint, which is what a hand-wired stack is.
 * \{ */

TEST_F(PaintMaterialLayerEditTest, layer_kind_defaults_to_paint)
{
  /* A stack built by hand, the way an old file or a Shader Editor wiring looks. */
  build_stack(3);
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));

  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
      EXPECT_EQ(BKE_paint_material_layer_kind_get(node), PaintMaterialLayerKind::Paint);
      float fill_color[4];
      EXPECT_FALSE(BKE_paint_material_layer_fill_color_get(node, fill_color));
    }
  }
}

TEST_F(PaintMaterialLayerEditTest, layer_kind_round_trips)
{
  /* Three layers: a bare base and the two Mix nodes the loop below marks. */
  build_stack(3);
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));

  int index = 0;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy != SH_NODE_MIX_RGB_LEGACY) {
      continue;
    }
    const PaintMaterialLayerKind kind = (index++ == 0) ? PaintMaterialLayerKind::Fill :
                                                         PaintMaterialLayerKind::Material;
    BKE_paint_material_layer_kind_set(node, kind);
    EXPECT_EQ(BKE_paint_material_layer_kind_get(node), kind);
  }
  EXPECT_EQ(index, 2);

  /* Reading back through a fresh reader of the same nodes, not the same pointer. */
  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 2);
}

TEST_F(PaintMaterialLayerEditTest, channel_has_unsupported_source_message)
{
  EXPECT_STRNE(BKE_paint_material_layer_edit_error_message(
                   PaintMaterialLayerEditError::ChannelHasUnsupportedSource),
               "");
}

TEST_F(PaintMaterialLayerEditTest, material_channel_set_and_multichannel_api_declared)
{
  EXPECT_EQ(PAINT_MATERIAL_LAYER_MATERIAL_CHANNELS[0], 0);
  EXPECT_EQ(PAINT_MATERIAL_LAYER_MATERIAL_CHANNELS[4], 4);
  EXPECT_EQ(PAINT_MATERIAL_LAYER_MATERIAL_CHANNELS[5], 7);
  EXPECT_EQ(PAINT_MATERIAL_LAYER_MATERIAL_CHANNELS[6], 9);
  auto *fn_ensure = &BKE_paint_material_layer_channels_ensure;
  auto *fn_base = &BKE_paint_material_layer_add_material_base;
  EXPECT_NE(fn_ensure, nullptr);
  EXPECT_NE(fn_base, nullptr);
}

TEST_F(PaintMaterialLayerEditTest, channels_ensure_empty_is_not_a_stack)
{
  /* The fixture's material is a bare Principled with no chains. */
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int ch[1] = {0};
  EXPECT_FALSE(
      BKE_paint_material_layer_channels_ensure(*bmain, *material, Span<int>(ch, 1), &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::NotAStack);
}

TEST_F(PaintMaterialLayerEditTest, channels_ensure_procedural_denied)
{
  /* Base Color is a stack; Metallic is driven by a procedural node (Value -> Metallic). */
  build_stack(2);
  bNodeTree &tree = *material->nodetree;
  bNode *val = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNode &pr = principled_node();
  bke::node_add_link(tree,
                     *val,
                     *bke::node_find_socket(*val, SOCK_OUT, "Value"_ustr),
                     pr,
                     *bke::node_find_socket(pr, SOCK_IN, "Metallic"_ustr));
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int ch[2] = {0, 1};
  EXPECT_FALSE(
      BKE_paint_material_layer_channels_ensure(*bmain, *material, Span<int>(ch, 2), &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::ChannelHasUnsupportedSource);
}

TEST_F(PaintMaterialLayerEditTest, channels_ensure_complete_stack_is_noop)
{
  build_stack(2);
  ASSERT_TRUE(BKE_paint_material_layer_bottom_normalize(*bmain, *material));
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));
  const GraphShape before = graph_shape();
  int ch[1] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_TRUE(
      BKE_paint_material_layer_channels_ensure(*bmain, *material, Span<int>(ch, 1), &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, channels_ensure_mirrors_missing_channel_transparent)
{
  build_stack(3);
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));
  int ch[2] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_METALLIC};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(
      BKE_paint_material_layer_channels_ensure(*bmain, *material, Span<int>(ch, 2), &error))
      << int(error);
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  Vector<int> wired;
  BKE_paint_material_layer_channels_wired(*material, wired);
  EXPECT_EQ(wired.size(), 2);

  /* One row per channel per layer, bottom to top. The hand-built Base maps carry no
   * paint id, so the merge reads them positionally while the mirrored maps carry theirs. */
  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 3);
  Vector<bUUID> metal_ids;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    Image *base = entry.channel_images.lookup_default(
        PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
    Image *metal = entry.channel_images.lookup_default(
        PAINT_MATERIAL_CHANNEL_METALLIC, nullptr);
    ASSERT_NE(base, nullptr);
    ASSERT_NE(metal, nullptr);
    /* One identity per mirrored row. */
    EXPECT_FALSE(BLI_uuid_is_nil(metal->paint_layer_id));
    for (const bUUID &seen : metal_ids) {
      EXPECT_FALSE(BLI_uuid_equal(seen, metal->paint_layer_id));
    }
    metal_ids.append(metal->paint_layer_id);
  }
  /* Synthetic rows stay out of the way (transparent); the mirrored bottom keeps showing
   * what the free input used to supply (opaque). */
  const auto gen_alpha = [&](const Image *image) {
    const ImageTile *tile = BKE_image_get_tile(const_cast<Image *>(image), 0);
    EXPECT_NE(tile, nullptr);
    return (tile == nullptr) ? -1.0f : tile->gen_color[3];
  };
  EXPECT_FLOAT_EQ(
      gen_alpha(entries[2].channel_images.lookup_default(
          PAINT_MATERIAL_CHANNEL_METALLIC, nullptr)),
      0.0f);
  EXPECT_FLOAT_EQ(
      gen_alpha(entries[0].channel_images.lookup_default(
          PAINT_MATERIAL_CHANNEL_METALLIC, nullptr)),
      1.0f);
}

TEST_F(PaintMaterialLayerEditTest, material_base_builds_single_normalized_row)
{
  /* Baked maps as material_bake_to_images hands them over: fresh, one user each. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *base_img = BKE_image_add_generated(
      bmain, 8, 8, "Baked Base", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  Image *metal_img = BKE_image_add_generated(
      bmain, 8, 8, "Baked Metal", 32, false, IMA_GENTYPE_BLANK, black, false, true, false);
  ASSERT_NE(base_img, nullptr);
  ASSERT_NE(metal_img, nullptr);
  PaintMaterialLayerChannelImage maps[2] = {
      {PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_img},
      {PAINT_MATERIAL_CHANNEL_METALLIC, metal_img},
  };
  int ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add_material_base(
      *bmain, *material, Span<PaintMaterialLayerChannelImage>(maps, 2), &ordinal, &error))
      << int(error);
  EXPECT_EQ(ordinal, 0);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 1);
  Image *base = entries[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
  Image *metal = entries[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_METALLIC, nullptr);
  EXPECT_EQ(base, base_img);
  EXPECT_EQ(metal, metal_img);
  EXPECT_TRUE(BLI_uuid_equal(base->paint_layer_id, metal->paint_layer_id));
  EXPECT_FALSE(BLI_uuid_is_nil(entries[0].marker));
  /* Normalized Mix form on both channels, sharing the row marker. */
  int marked_mix = 0;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX &&
        BLI_uuid_equal(BKE_paint_material_layer_marker_get(node), entries[0].marker))
    {
      marked_mix++;
    }
  }
  EXPECT_EQ(marked_mix, 2);

  /* The row takes a kind like any other layer. */
  EXPECT_TRUE(BKE_paint_material_layer_kind_set(
      *bmain, *material, 0, PaintMaterialLayerKind::Material, &error))
      << int(error);
  entries.clear();
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].kind, int8_t(PaintMaterialLayerKind::Material));
}

TEST_F(PaintMaterialLayerEditTest, fill_layer_add_records_its_color)
{
  build_stack(2);
  const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  const int ordinal = add_fill_layer(color, "Filler");
  ASSERT_EQ(ordinal, 2);

  bNode *filler = find_layer_node("Filler");
  ASSERT_NE(filler, nullptr);
  EXPECT_EQ(BKE_paint_material_layer_kind_get(*filler), PaintMaterialLayerKind::Fill);
  float recorded[4];
  ASSERT_TRUE(BKE_paint_material_layer_fill_color_get(*filler, recorded));
  EXPECT_V4_NEAR(float4(recorded), float4(color), 1e-6f);
}

TEST_F(PaintMaterialLayerEditTest, fill_color_apply_refills_every_wired_channel)
{
  /* The Fill as the very first layer of an empty material: a bare Image Texture on Base Color. */
  const float color[4] = {0.1f, 0.9f, 0.5f, 1.0f};
  const int ordinal = add_fill_layer(color, "Filler");
  ASSERT_EQ(ordinal, 0);

  /* Wire Roughness as a second channel, with its own bare base carrying the same kind marker --
   * the shape a Fill added to a wired channel has. */
  bNodeTree &tree = *material->nodetree;
  bNode &principled = principled_node();
  Image *roughness_image = BKE_image_add_generated(
      bmain, 8, 8, "Roughness TexLayer", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  bNode *roughness_tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  roughness_tex->id = &roughness_image->id;
  BKE_paint_material_layer_kind_set(*roughness_tex, PaintMaterialLayerKind::Fill);
  BKE_paint_material_layer_fill_color_set(*roughness_tex, color);
  bke::node_add_link(tree,
                     *roughness_tex,
                     *bke::node_find_socket(*roughness_tex, SOCK_OUT, "Color"_ustr),
                     principled,
                     *bke::node_find_socket(principled, SOCK_IN, "Roughness"_ustr));

  /* A different colour for the re-fill: the scalar Roughness map has to take the red component,
   * while the Base Color map takes all three. */
  const float new_color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(
      BKE_paint_material_layer_fill_color_apply(*bmain, *material, ordinal, new_color, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 1);
  Image *color_image = entries[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
  Image *roughness_map = entries[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_ROUGHNESS, nullptr);
  ASSERT_NE(color_image, nullptr);
  ASSERT_NE(roughness_map, nullptr);

  /* Layer maps are byte buffers, so the expectation is whatever the generator's own fill writes
   * for the same colour -- comparing against it keeps the test free of the byte conversion. */
  auto expect_pixel = [](Image *image, const float expected_color[4]) {
    uint8_t expected[4];
    BKE_image_buf_fill_color(expected, nullptr, 1, 1, expected_color);
    void *lock = nullptr;
    ImBuf *buffer = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(buffer->byte_buffer.data, nullptr);
    for (int i = 0; i < 4; i++) {
      EXPECT_EQ(buffer->byte_buffer.data[i], expected[i]) << "component " << i;
    }
    BKE_image_release_ibuf(image, buffer, lock);
  };
  expect_pixel(color_image, new_color);
  const float roughness_expected[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  expect_pixel(roughness_map, roughness_expected);

  /* A generated map nobody painted must rebuild to the new colour, not the creation one. */
  EXPECT_V4_NEAR(float4(BKE_image_get_tile(color_image, 0)->gen_color), float4(new_color), 1e-6f);

  /* The marker records the colour the layer now stands for. The layer is the stack's bare base,
   * so the marker lives on the Image Texture that shows its map. */
  bNode *filler = nullptr;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &color_image->id) {
      filler = &node;
      break;
    }
  }
  ASSERT_NE(filler, nullptr);
  float recorded[4];
  ASSERT_TRUE(BKE_paint_material_layer_fill_color_get(*filler, recorded));
  EXPECT_V4_NEAR(float4(recorded), float4(new_color), 1e-6f);
}

TEST_F(PaintMaterialLayerEditTest, fill_color_apply_refuses_a_paint_layer)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_fill_color_apply(*bmain, *material, 1, color, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, fill_color_preview_refills_pixels_but_keeps_marker)
{
  const float color[4] = {0.1f, 0.9f, 0.5f, 1.0f};
  const int ordinal = add_fill_layer(color, "Filler");
  ASSERT_EQ(ordinal, 0);

  /* Wire Roughness as a second channel, the same shape the apply test uses. */
  bNodeTree &tree = *material->nodetree;
  bNode &principled = principled_node();
  Image *roughness_image = BKE_image_add_generated(
      bmain, 8, 8, "Roughness TexLayer", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  bNode *roughness_tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  roughness_tex->id = &roughness_image->id;
  BKE_paint_material_layer_kind_set(*roughness_tex, PaintMaterialLayerKind::Fill);
  BKE_paint_material_layer_fill_color_set(*roughness_tex, color);
  bke::node_add_link(tree,
                     *roughness_tex,
                     *bke::node_find_socket(*roughness_tex, SOCK_OUT, "Color"_ustr),
                     principled,
                     *bke::node_find_socket(principled, SOCK_IN, "Roughness"_ustr));

  const float preview_color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(
      BKE_paint_material_layer_fill_color_preview(*bmain, *material, ordinal, preview_color, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* Pixels moved in every wired channel, with the scalar rule applied. */
  auto expect_pixel = [](Image *image, const float expected_color[4]) {
    uint8_t expected[4];
    BKE_image_buf_fill_color(expected, nullptr, 1, 1, expected_color);
    void *lock = nullptr;
    ImBuf *buffer = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(buffer->byte_buffer.data, nullptr);
    for (int i = 0; i < 4; i++) {
      EXPECT_EQ(buffer->byte_buffer.data[i], expected[i]) << "component " << i;
    }
    BKE_image_release_ibuf(image, buffer, lock);
  };
  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 1);
  Image *color_image = entries[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
  Image *roughness_map = entries[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_ROUGHNESS, nullptr);
  ASSERT_NE(color_image, nullptr);
  ASSERT_NE(roughness_map, nullptr);
  expect_pixel(color_image, preview_color);
  const float roughness_expected[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  expect_pixel(roughness_map, roughness_expected);

  /* The marker still names the old colour in every channel: only the bake moves it. */
  for (bNode &node : material->nodetree->nodes) {
    float recorded[4];
    if (BKE_paint_material_layer_fill_color_get(node, recorded)) {
      EXPECT_V4_NEAR(float4(recorded), float4(color), 1e-6f);
    }
  }
}

TEST_F(PaintMaterialLayerEditTest, fill_color_preview_refuses_a_paint_layer)
{
  build_stack(3);
  const GraphShape before = graph_shape();

  const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_fill_color_preview(*bmain, *material, 1, color, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
  expect_graph_unchanged(before);
}

TEST_F(PaintMaterialLayerEditTest, fill_color_preview_bumps_revision_and_invalidates)
{
  const float color[4] = {0.1f, 0.9f, 0.5f, 1.0f};
  const int ordinal = add_fill_layer(color, "Filler");
  ASSERT_EQ(ordinal, 0);

  /* The revision readers poll to know the stack moved on. Warming the composite cache itself
   * needs an assembled composite span, so the revision bump -- which shares the
   * #paint_layer_edit_committed path with the apply, including the cache invalidation -- is
   * the observable contract asserted here. */
  const uint64_t revision_before = BKE_material_paint_layer_revision_get(*material);
  const float preview_color[4] = {0.7f, 0.2f, 0.3f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(
      BKE_paint_material_layer_fill_color_preview(*bmain, *material, ordinal, preview_color, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  EXPECT_GT(BKE_material_paint_layer_revision_get(*material), revision_before);
}

TEST_F(PaintMaterialLayerEditTest, layer_kind_survives_reorder_and_duplicate)
{
  build_stack(2);
  const float color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  const int ordinal = add_fill_layer(color, "Filler");
  ASSERT_EQ(ordinal, 2);

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_reorder(*bmain, *material, 2, 1, &error));
  bNode *moved = find_layer_node("Filler");
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(BKE_paint_material_layer_kind_get(*moved), PaintMaterialLayerKind::Fill);

  int copy_ordinal = -1;
  /* After the reorder the Fill sits at 1, directly above the bare base. */
  ASSERT_TRUE(BKE_paint_material_layer_duplicate(*bmain, *material, 1, &copy_ordinal, &error));
  int copies = 0;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX && STREQ(node.label, "Filler")) {
      EXPECT_EQ(BKE_paint_material_layer_kind_get(node), PaintMaterialLayerKind::Fill);
      float recorded[4];
      ASSERT_TRUE(BKE_paint_material_layer_fill_color_get(node, recorded));
      EXPECT_V4_NEAR(float4(recorded), float4(color), 1e-6f);
      copies++;
    }
  }
  EXPECT_EQ(copies, 2);
}

/** \} */

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

TEST_F(PaintMaterialLayerEditTest, add_anchored_to_a_grouped_layer_lands_in_the_group)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, &group_ordinal, &error));

  /* Anchor to "L1", the lower of the two rows the folder holds. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.name = "Inserted";
  params.anchor_ordinal = PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE + 0;
  int new_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &new_ordinal, &error))
      << int(error);
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  /* It landed inside the folder, so its ordinal is a group-child one. */
  EXPECT_GE(new_ordinal, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 5);
  EXPECT_EQ(entries[0].name, "L0");
  EXPECT_TRUE(entries[1].is_group);
  EXPECT_EQ(entries[2].name, "L1");
  EXPECT_EQ(entries[3].name, "Inserted");
  EXPECT_EQ(entries[4].name, "L2");
  /* The new row nests exactly as deep as the siblings it was dropped between. */
  EXPECT_EQ(entries[3].depth, entries[2].depth);
}

TEST_F(PaintMaterialLayerEditTest, add_anchored_to_a_folder_lands_inside_it_on_top)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, &group_ordinal, &error));

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.name = "OnTop";
  params.anchor_ordinal = group_ordinal;
  int new_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &new_ordinal, &error))
      << int(error);
  EXPECT_GE(new_ordinal, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  ASSERT_EQ(entries.size(), 5);
  EXPECT_EQ(entries[2].name, "L1");
  EXPECT_EQ(entries[3].name, "L2");
  EXPECT_EQ(entries[4].name, "OnTop");
  EXPECT_EQ(entries[4].depth, entries[3].depth);
}

TEST_F(PaintMaterialLayerEditTest, add_anchored_to_an_empty_folder_creates_its_first_layer)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int folder_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_group_add(
      *bmain, *material, -1, PaintMaterialLayerMovePlace::Above, &folder_ordinal, &error))
      << int(error);

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.name = "First";
  params.anchor_ordinal = folder_ordinal;
  int new_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &new_ordinal, &error))
      << int(error);
  EXPECT_GE(new_ordinal, PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
  bool found_nested_first = false;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.name == "First" && entry.depth > 0) {
      found_nested_first = true;
    }
  }
  EXPECT_TRUE(found_nested_first);
}

TEST_F(PaintMaterialLayerEditTest, add_anchored_inside_a_shared_folder_is_refused)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(*bmain, *material, 1, 2, nullptr, &error));

  /* A second material whose node tree reaches the same folder tree: adding a layer here would
   * grow a stack shown in that material too. */
  Material *twin = id_cast<Material *>(BKE_id_copy(bmain, &material->id));
  ASSERT_NE(twin, nullptr);

  const GraphShape before = graph_shape();
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.anchor_ordinal = PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE + 0;
  error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::TreeShared);
  expect_graph_unchanged(before);

  BKE_id_free(bmain, &twin->id);
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
