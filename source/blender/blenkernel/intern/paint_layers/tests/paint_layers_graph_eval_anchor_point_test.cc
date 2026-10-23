/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Anchor Point readers cross-checked between the generated node graph and the CPU composite.
 */

#include "testing/testing.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_uuid.h"

#include <cmath>

#include "BKE_node.hh"
#include "BKE_node_tree_interface.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

class PaintLayersGraphEvalAnchorPointTest : public PaintLayersGraphEvalTest {
 protected:
  MaterialPaintLayer *add_point(MaterialPaintLayer &owner,
                                const PaintLayerAnchorPointList list =
                                    PaintLayerAnchorPointList::Effects,
                                MaterialPaintLayer *insert_after = nullptr)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &owner, list, insert_after, false, "Point", {});
  }

  MaterialPaintLayer *add_reader(MaterialPaintLayer &owner,
                                 const PaintLayerAnchorPointList list,
                                 const bUUID &source,
                                 MaterialPaintLayer *insert_after = nullptr)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &owner, list, insert_after, true, "Reader", source);
  }

  /** A source row with an opaque red Base Color map and a point at its end. */
  MaterialPaintLayer *add_source_with_point(MaterialPaintLayer **r_point)
  {
    MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 255));
    *r_point = add_point(*source);
    EXPECT_NE(*r_point, nullptr);
    return source;
  }

  /** The reader input that crosses into a row group, matched by its interface name. */
  static bool is_anchor_point_input(const bNodeSocket &socket)
  {
    return socket.name != nullptr && StringRef(socket.name).startswith("AnchorPoint ");
  }

  /** Every reader instance in \a root has its AnchorPoint input linked; returns how many are. */
  static int check_reader_inputs_linked(const bNodeTree &root)
  {
    int anchors = 0;
    for (const bNode &node : root.nodes) {
      if (node.id == nullptr || GS(node.id->name) != ID_NT) {
        continue;
      }
      for (const bNodeSocket &socket : node.inputs) {
        if (!is_anchor_point_input(socket)) {
          continue;
        }
        anchors++;
        /* Scan the links rather than `socket.directly_linked_links()`: the latter asserts the
         * topology cache is available, which this const tree has not ensured. */
        bool linked = false;
        for (const bNodeLink &link : root.links) {
          if (link.tosock == &socket) {
            linked = true;
            break;
          }
        }
        EXPECT_TRUE(linked) << "instance " << node.name << " input " << socket.name
                            << " must be wired";
      }
    }
    return anchors;
  }
};

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_anchor_point_content_matches_cpu)
{
  MaterialPaintLayer *point = nullptr;
  add_source_with_point(&point);
  MaterialPaintLayer *reader_row = add_layer("Reader", add_solid_image("Reader", 0, 0, 255, 255));
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* The red slice over the row's own blue content: a dropped reader would leave it blue. */
  const RGBA pixel = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(pixel.r, 0.5f, 2e-3f);
  EXPECT_NEAR(pixel.b, 0.5f, 2e-3f);
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_anchor_point_mask_matches_cpu)
{
  MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 255));
  /* A fully hidden source mask: its slice is grey 0, so the reader's mask item reduces the reader
   * row's coverage to zero. A dropped reader would leave the row fully visible. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.0f), nullptr);
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = add_layer("Reader", add_solid_image("Reader", 0, 0, 255, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, reader_row, 1.0f), nullptr);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::MaskStack, point->marker);
  ASSERT_NE(reader, nullptr);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA with_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, false));
  const RGBA without_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, true));
  const RGBA with_reader_again = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* The reader's zero mask hides the blue row: the graph shows what is below it, and the positive
   * control without the reader is the full blue row. */
  EXPECT_NEAR(with_reader_again.b, with_reader.b, 1e-5f);
  EXPECT_LT(with_reader.b, 0.5f);
  EXPECT_GT(without_reader.b, 0.9f);
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_reader_in_same_row_matches_cpu)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 255, 0, 0, 255));
  MaterialPaintLayer *point = add_point(*row);
  ASSERT_NE(point, nullptr);
  /* A green correction above the point makes the row's own content differ from the slice. */
  MaterialPaintLayer *green = BKE_paint_layers_correction_add(*ma, row, "Green");
  ASSERT_NE(green, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, green, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  const float green_value[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, green, PAINT_MATERIAL_CHANNEL_BASE_COLOR, green_value));
  MaterialPaintLayer *reader = add_reader(
      *row, PaintLayerAnchorPointList::Effects, point->marker, green);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* Red laid at half over the green row: neither the full slice nor the row's own colour. */
  const RGBA pixel = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(pixel.r, 0.5f, 2e-3f);
  EXPECT_NEAR(pixel.g, 0.5f, 2e-3f);
  EXPECT_NEAR(pixel.b, 0.0f, 2e-3f);
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_tap_present_with_zero_opacity_source)
{
  MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 255));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, source, 0.0f));
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, point->marker), nullptr);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* The slice is published before the row's opacity, so the reader still sees the red source. */
  const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_GT(graph.r, 0.9f);
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_reuse_keeps_tap_sockets)
{
  MaterialPaintLayer *neighbour = add_layer("Neighbour", add_solid_image("Neighbour", 0, 255, 0, 255));
  ASSERT_NE(neighbour, nullptr);
  MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 255));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = add_layer(
      "Reader", add_solid_image("Reader", 0, 0, 255, 255));
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = find_layer_group(*reader_row);
  ASSERT_NE(group, nullptr);
  bNode *input_before = nullptr;
  for (bNode &node : group->nodes) {
    if (node.is_group_input()) {
      input_before = &node;
      break;
    }
  }
  ASSERT_NE(input_before, nullptr);

  /* A value edit in another row rebuilds nothing: the reader's group and its nodes stay. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, neighbour, 0.25f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(find_layer_group(*reader_row), group) << "the group is the same tree";
  bNode *input_after = nullptr;
  for (bNode &node : group->nodes) {
    if (node.is_group_input()) {
      input_after = &node;
      break;
    }
  }
  EXPECT_EQ(input_after, input_before) << "the group's nodes were not rebuilt";

  /* The reader is observable: red over the row's blue content. */
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA pixel = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(pixel.r, 0.5f, 2e-3f);
  EXPECT_NEAR(pixel.b, 0.5f, 2e-3f);
  EXPECT_GT(check_reader_inputs_linked(*ma->paint_layers_tree), 0)
      << "the positive control: the reader input exists and is linked";
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_reuse_keeps_mask_tap_sockets)
{
  MaterialPaintLayer *neighbour = add_layer("Neighbour", add_solid_image("Neighbour", 0, 255, 0, 255));
  ASSERT_NE(neighbour, nullptr);
  MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.25f), nullptr);
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = add_layer("Reader", add_solid_image("Reader", 0, 0, 255, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, reader_row, 1.0f), nullptr);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::MaskStack, point->marker);
  ASSERT_NE(reader, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = find_layer_group(*reader_row);
  ASSERT_NE(group, nullptr);
  bNode *input_before = nullptr;
  for (bNode &node : group->nodes) {
    if (node.is_group_input()) {
      input_before = &node;
      break;
    }
  }
  ASSERT_NE(input_before, nullptr);

  /* A value edit elsewhere keeps the group, including the links that feed its mask reader. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, neighbour, 0.25f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(find_layer_group(*reader_row), group) << "the group is the same tree";
  bNode *input_after = nullptr;
  for (bNode &node : group->nodes) {
    if (node.is_group_input()) {
      input_after = &node;
      break;
    }
  }
  EXPECT_EQ(input_after, input_before) << "the group's nodes were not rebuilt";

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_GT(check_reader_inputs_linked(*ma->paint_layers_tree), 0)
      << "the positive control: the mask reader's input is linked in the root";
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_reader_link_has_no_duplicate_links)
{
  MaterialPaintLayer *neighbour = add_layer("Neighbour", add_solid_image("Neighbour", 0, 255, 0, 255));
  ASSERT_NE(neighbour, nullptr);
  MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 255));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = add_layer(
      "Reader", add_solid_image("Reader", 0, 0, 255, 255));
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, point->marker), nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* A value edit between the two regenerations exercises the relink path rather than a rebuild. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, neighbour, 0.25f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Both the root and each row group, with the positive control that the reader link is there. */
  EXPECT_EQ(duplicate_link_count(*ma->paint_layers_tree), 0) << "the root tree";
  for (const bNodeTree &tree : bmain->nodetrees) {
    EXPECT_EQ(duplicate_link_count(tree), 0) << "tree " << tree.id.name + 2;
  }
  EXPECT_GT(check_reader_inputs_linked(*ma->paint_layers_tree), 0);
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_normal_slice_matches_cpu)
{
  Image *normal_image = add_solid_image("Normal", 128, 200, 64, 255);
  make_image_data(normal_image);
  MaterialPaintLayer *source = add_layer(
      "Source", normal_image, PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  /* The reader row carries a normal of its own, so laying the slice over it is visible. */
  Image *row_normal = add_solid_image("Row Normal", 64, 128, 255, 255);
  make_image_data(row_normal);
  MaterialPaintLayer *reader_row = add_layer(
      "Reader", row_normal, PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);

  const RGBA with_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, false));
  const RGBA without_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, true));
  EXPECT_GT(std::abs(with_reader.r - without_reader.r) + std::abs(with_reader.g - without_reader.g),
            1e-3f)
      << "the reader's own normal changes the combine result";
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_mask_reader_uses_first_participating_channel)
{
  /* Base Color is transparent (a content slice whose coverage is 0) and Roughness opaque (coverage
   * 1): the reader's mask item hides the row only when the first participating channel is read as
   * the mask channel. */
  MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 0));
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, source, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma,
      source,
      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
      add_solid_image("Rough Source", 255, 255, 255, 255)));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = add_layer("Reader", add_solid_image("Reader", 0, 0, 255, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, reader_row, 1.0f), nullptr);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::MaskStack, point->marker);
  ASSERT_NE(reader, nullptr);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA with_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, false));
  const RGBA without_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, true));

  /* Reading Roughness first would leave the row blue; reading Base Color first hides it. */
  EXPECT_LT(with_reader.b, 0.5f);
  EXPECT_GT(without_reader.b, 0.9f);
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_reader_in_folder_child_matches_cpu)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "Folder", nullptr, PaintLayerPlace::Above);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 1.0f));
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_solid_image("Child", 255, 0, 0, 255)));
  MaterialPaintLayer *point = add_point(*child);
  ASSERT_NE(point, nullptr);
  /* A blue reader row with the child's red slice over it at half: the result differs from both. */
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, reader_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, reader_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_solid_image("Reader", 0, 0, 255, 255)));
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA with_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, false));
  const RGBA without_reader = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, true));

  EXPECT_NEAR(with_reader.r, 0.5f, 2e-3f);
  EXPECT_NEAR(with_reader.b, 0.5f, 2e-3f);
  EXPECT_LT(without_reader.r, 0.1f) << "without the reader the row is its own blue";
  EXPECT_GT(without_reader.b, 0.9f);
}

}  // namespace blender::bke::tests
