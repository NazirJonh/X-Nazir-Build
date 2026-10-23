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

  static int duplicate_link_count(const bNodeTree &tree)
  {
    int duplicates = 0;
    for (const bNodeLink &a : tree.links) {
      int matches = 0;
      for (const bNodeLink &b : tree.links) {
        if (a.fromnode == b.fromnode && a.fromsock == b.fromsock && a.tonode == b.tonode &&
            a.tosock == b.tosock)
        {
          matches++;
        }
      }
      if (matches > 1) {
        duplicates++;
      }
    }
    return duplicates;
  }

  /** The one row group of \a layer, found by its marker rather than by its name. */
  bNodeTree *find_layer_group(const MaterialPaintLayer &layer)
  {
    bNodeTree *found = nullptr;
    for (bNodeTree &tree : bmain->nodetrees) {
      bUUID marker = BLI_uuid_nil();
      if (!layer_tree_marker_get(tree, marker) || !BLI_uuid_equal(marker, layer.marker)) {
        continue;
      }
      EXPECT_EQ(found, nullptr) << "a row has exactly one kept group";
      found = &tree;
    }
    return found;
  }

  /** Remove the interface socket named \a name from \a tree; true when it was there. */
  static bool remove_interface_socket(bNodeTree &tree, const char *name, const bool input)
  {
    for (bNodeTreeInterfaceItem *item : tree.tree_interface.items()) {
      if (item->item_type != NodeTreeInterfaceItemType::Socket) {
        continue;
      }
      auto &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(*item);
      if (((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0) != input ||
          socket.name == nullptr || !STREQ(socket.name, name))
      {
        continue;
      }
      tree.tree_interface.remove_item(*item);
      return true;
    }
    return false;
  }

  static bool interface_has_socket(const bNodeTree &tree, const char *prefix)
  {
    for (const bNodeTreeInterfaceItem *item : tree.tree_interface.items()) {
      if (item->item_type != NodeTreeInterfaceItemType::Socket) {
        continue;
      }
      const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(*item);
      if (socket.name != nullptr && StringRef(socket.name).startswith(prefix)) {
        return true;
      }
    }
    return false;
  }

  /** Remove one interface input whose name starts with \a prefix; true when it was there. */
  static bool remove_interface_input_prefix(bNodeTree &tree, const char *prefix)
  {
    for (bNodeTreeInterfaceItem *item : tree.tree_interface.items()) {
      if (item->item_type != NodeTreeInterfaceItemType::Socket) {
        continue;
      }
      auto &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(*item);
      if ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) == 0 || socket.name == nullptr ||
          !StringRef(socket.name).startswith(prefix))
      {
        continue;
      }
      tree.tree_interface.remove_item(*item);
      return true;
    }
    return false;
  }

  /**
   * The nodes a failed rebuild may duplicate or orphan: the group's own Group Input/Output, the
   * group's instances in the root, and the divide/zero nodes a folder keeps in the root.
   */
  struct NodeCounts {
    int group_inputs = 0;
    int group_outputs = 0;
    int instances = 0;
    int divide = 0;
    int zero = 0;

    bool operator==(const NodeCounts &other) const
    {
      return group_inputs == other.group_inputs && group_outputs == other.group_outputs &&
             instances == other.instances && divide == other.divide && zero == other.zero;
    }
  };

  static NodeCounts count_nodes(const bNodeTree &root, const bNodeTree &group)
  {
    NodeCounts counts;
    for (const bNode &node : group.nodes) {
      counts.group_inputs += node.is_group_input() ? 1 : 0;
      counts.group_outputs += node.is_group_output() ? 1 : 0;
    }
    for (const bNode &node : root.nodes) {
      counts.instances += (node.id == &group.id) ? 1 : 0;
      if (node.type_legacy == SH_NODE_MATH && node.custom1 == NODE_MATH_DIVIDE) {
        counts.divide++;
      }
      if (node.type_legacy == SH_NODE_VALUE) {
        for (const bNodeSocket &output : node.outputs) {
          if (output.default_value != nullptr && output.type == SOCK_FLOAT &&
              static_cast<const bNodeSocketValueFloat *>(output.default_value)->value == 0.0f)
          {
            counts.zero++;
            break;
          }
        }
      }
    }
    return counts;
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

  /**
   * A source and a reader row that both take part in Base Color and Roughness, the reader laying
   * its slice over the row's own content at half opacity, so its effect on the pixels is visible.
   */
  MaterialPaintLayer *set_up_two_channel_reader()
  {
    MaterialPaintLayer *source = add_layer("Source", add_solid_image("Source", 255, 0, 0, 255));
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, source, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
              nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(
        *ma,
        source,
        PAINT_MATERIAL_CHANNEL_ROUGHNESS,
        add_solid_image("Source Rough", 0, 255, 0, 255)));
    MaterialPaintLayer *point = add_point(*source);
    EXPECT_NE(point, nullptr);
    if (point == nullptr) {
      return nullptr;
    }

    MaterialPaintLayer *reader_row = add_layer(
        "Reader", add_solid_image("Reader", 0, 0, 255, 255));
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, reader_row, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
              nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma,
                                                  reader_row,
                                                  PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                                  add_solid_image("Reader Rough", 255, 255, 255, 255)));
    MaterialPaintLayer *reader = add_reader(
        *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
    EXPECT_NE(reader, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));
    return reader_row;
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

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_failed_relink_rebuilds_group)
{
  MaterialPaintLayer *reader_row = set_up_two_channel_reader();
  ASSERT_NE(reader_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = find_layer_group(*reader_row);
  ASSERT_NE(group, nullptr) << "the reader row's group was found by its marker";
  const NodeCounts clean = count_nodes(*ma->paint_layers_tree, *group);
  ASSERT_EQ(clean.group_inputs, 1);
  ASSERT_EQ(clean.group_outputs, 1);
  ASSERT_EQ(clean.instances, 1);

  /* The kept group looks like one built before the first channel existed: its interface has no
   * contract sockets for Base Color. The topology hash still matches, so the factory keeps the
   * group and the relink on the first channel is what fails. */
  ASSERT_TRUE(remove_interface_socket(*group, "Below Base Color", true));
  ASSERT_TRUE(remove_interface_socket(*group, "Result Base Color", false));
  /* A structural edit elsewhere forces the full build that reaches the kept group. The Trigger row
   * adds no node of this group and no divide/zero node, so the counts stay comparable. */
  ASSERT_NE(add_layer("Trigger", add_solid_image("Trigger", 0, 0, 0, 0)), nullptr);

  /* Rebuilt from an empty tree rather than dropped, and the channel rebuilt first is not lost. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  const RGBA base = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(base.r, 0.5f, 2e-3f) << "the reader's red slice is laid over the blue row";
  EXPECT_NEAR(base.b, 0.5f, 2e-3f);
  EXPECT_TRUE(interface_has_socket(*group, "Below Base Color"))
      << "the removed Base Color input is back after the rebuild";
  EXPECT_TRUE(interface_has_socket(*group, "Result Base Color"));
  EXPECT_EQ(duplicate_link_count(*group), 0);
  EXPECT_EQ(duplicate_link_count(*ma->paint_layers_tree), 0);
  const NodeCounts recovered = count_nodes(*ma->paint_layers_tree, *group);
  EXPECT_EQ(recovered.group_inputs, clean.group_inputs) << "no duplicate Group Input";
  EXPECT_EQ(recovered.group_outputs, clean.group_outputs) << "no duplicate Group Output";
  EXPECT_EQ(recovered.instances, clean.instances) << "no duplicate instance in the root";
  EXPECT_EQ(recovered.divide, clean.divide) << "no orphaned divide node";
  EXPECT_EQ(recovered.zero, clean.zero) << "no orphaned zero node";
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_failed_relink_rebuilds_group_two_channels)
{
  MaterialPaintLayer *reader_row = set_up_two_channel_reader();
  ASSERT_NE(reader_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = find_layer_group(*reader_row);
  ASSERT_NE(group, nullptr) << "the reader row's group was found by its marker";
  const NodeCounts clean = count_nodes(*ma->paint_layers_tree, *group);
  ASSERT_EQ(clean.group_inputs, 1);
  ASSERT_EQ(clean.group_outputs, 1);
  ASSERT_EQ(clean.instances, 1);

  /* The failure happens on the second channel: the first one has already been chained through the
   * instance, and the rebuild must not cut it off. */
  ASSERT_TRUE(remove_interface_socket(*group, "Below Roughness", true));
  ASSERT_TRUE(remove_interface_socket(*group, "Result Roughness", false));
  /* A structural edit elsewhere forces the full build that reaches the kept group. */
  ASSERT_NE(add_layer("Trigger", add_solid_image("Trigger", 0, 0, 0, 0)), nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  const RGBA base = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(base.r, 0.5f, 2e-3f) << "Base Color is still the reader over the row's content";
  EXPECT_NEAR(base.b, 0.5f, 2e-3f);
  EXPECT_TRUE(interface_has_socket(*group, "Below Roughness"))
      << "the removed Roughness input is back after the rebuild";
  EXPECT_TRUE(interface_has_socket(*group, "Result Roughness"));
  EXPECT_EQ(duplicate_link_count(*group), 0);
  EXPECT_EQ(duplicate_link_count(*ma->paint_layers_tree), 0);
  const NodeCounts recovered = count_nodes(*ma->paint_layers_tree, *group);
  EXPECT_EQ(recovered.group_inputs, clean.group_inputs);
  EXPECT_EQ(recovered.group_outputs, clean.group_outputs);
  EXPECT_EQ(recovered.instances, clean.instances);
  EXPECT_EQ(recovered.divide, clean.divide);
  EXPECT_EQ(recovered.zero, clean.zero);
}

TEST_F(PaintLayersGraphEvalAnchorPointTest, graph_failed_relink_group_is_recreated_next_regen)
{
  MaterialPaintLayer *reader_row = set_up_two_channel_reader();
  ASSERT_NE(reader_row, nullptr);

  /* A pure build cannot rebuild a kept group: it is the no-Main branch of the generator, exercised
   * here so the failure path and the pass after it are both real. */
  int serial = 0;
  const auto add_tree = [&](const char *prefix) {
    char name[64];
    SNPRINTF(name, "%s %d", prefix, serial++);
    return bke::node_tree_add_tree(bmain, name, "ShaderNodeTree");
  };
  Map<const MaterialPaintLayer *, bNodeTree *> groups;
  const auto layer_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    bNodeTree *&group = groups.lookup_or_add_default(&layer);
    if (group == nullptr) {
      group = add_tree("Layer");
    }
    return group;
  };
  bNodeTree *root = add_tree("Root");
  ASSERT_NE(root, nullptr);
  PaintLayersBuildContext initial;
  initial.layer_tree_get = layer_tree_get;
  BKE_paint_layers_tree_build(*ma, *root, initial);
  bNodeTree *group = groups.lookup_default(reader_row, nullptr);
  ASSERT_NE(group, nullptr);

  /* The kept group lost a required reader input: the relink fails and, without a Main, the row
   * cannot be rebuilt in this pass. */
  ASSERT_TRUE(remove_interface_input_prefix(*group, "AnchorPoint "));

  bNodeTree *second_root = add_tree("Root");
  ASSERT_NE(second_root, nullptr);
  const MaterialPaintLayer *invalidated = nullptr;
  /* The context holds function references, not copies: the callables must outlive the build. */
  const auto kept_unchanged = [](const MaterialPaintLayer & /*layer*/) { return true; };
  const auto kept_invalidate = [&](const MaterialPaintLayer &layer) { invalidated = &layer; };
  PaintLayersBuildContext kept;
  kept.layer_tree_get = layer_tree_get;
  kept.layer_unchanged = kept_unchanged;
  kept.layer_invalidate = kept_invalidate;
  kept.bmain = nullptr;
  BKE_paint_layers_tree_build(*ma, *second_root, kept);
  EXPECT_EQ(invalidated, reader_row) << "the failed row asks for a rebuild on the next pass";
  EXPECT_EQ(count_nodes(*second_root, *group).instances, 0)
      << "the orphan instance was dropped from the root";

  /* The next pass finds the group invalidated: a fresh one is built, complete and instantiated
   * exactly once. */
  Map<const MaterialPaintLayer *, bNodeTree *> renewed;
  const auto rebuild_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    if (&layer == invalidated) {
      bNodeTree *&fresh = renewed.lookup_or_add_default(&layer);
      if (fresh == nullptr) {
        fresh = add_tree("Rebuilt Layer");
      }
      return fresh;
    }
    return groups.lookup_default(&layer, nullptr);
  };
  const auto rebuild_unchanged = [&](const MaterialPaintLayer &layer) {
    return &layer != invalidated;
  };
  PaintLayersBuildContext rebuild;
  rebuild.layer_tree_get = rebuild_tree_get;
  rebuild.layer_unchanged = rebuild_unchanged;
  rebuild.bmain = nullptr;
  bNodeTree *third_root = add_tree("Root");
  ASSERT_NE(third_root, nullptr);
  BKE_paint_layers_tree_build(*ma, *third_root, rebuild);

  bNodeTree *rebuilt = renewed.lookup_default(reader_row, nullptr);
  ASSERT_NE(rebuilt, nullptr);
  const NodeCounts counts = count_nodes(*third_root, *rebuilt);
  EXPECT_EQ(counts.group_inputs, 1) << "no duplicate Group Input in the rebuilt group";
  EXPECT_EQ(counts.group_outputs, 1) << "no duplicate Group Output in the rebuilt group";
  EXPECT_EQ(counts.instances, 1) << "one instance of the rebuilt group in the root";
  EXPECT_EQ(count_nodes(*third_root, *group).instances, 0)
      << "the stale group is not instantiated beside the rebuilt one";
}

}  // namespace blender::bke::tests
