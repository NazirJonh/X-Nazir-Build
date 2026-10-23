/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * A Material row that does not lay one of its channels, cross-checked between the generated node
 * graph and the CPU composite. This is the 3D-viewport regression: a saved row group must not be
 * dropped, or have its other channels lost, because one channel of the source is unavailable.
 */

#include "testing/testing.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"

#include <array>
#include <cmath>
#include <utility>

#include "BKE_node.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint_layers_edit.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

class PaintLayersGraphEvalMaterialRowTest : public PaintLayersGraphEvalTest {
 protected:
  /**
   * A source material whose Principled gives Base Color and Roughness as plain values and Normal
   * as a live map, while Metallic is fed by an Image Texture with no image: the channel resolves
   * to `Unavailable`, the "None" the row cannot lay.
   */
  Material *add_source_material(const char *name,
                                Image *metallic_image = nullptr,
                                const bool graph_base = false)
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

    if (graph_base) {
      bNode *rgb = bke::node_add_static_node(nullptr, ntree, SH_NODE_RGB);
      bke::node_add_link(ntree,
                         *rgb,
                         *bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr),
                         *principled,
                         *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
    }

    /* Metallic: an Image Texture with no image resolves to Unavailable; with an image it is a
     * live map. */
    bNode *metallic = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
    if (metallic_image != nullptr) {
      metallic->id = &metallic_image->id;
    }
    bke::node_add_link(ntree,
                       *metallic,
                       *bke::node_find_socket(*metallic, SOCK_OUT, "Color"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));

    /* Normal: a Normal Map whose Color is a live image, so the row lays the map itself. */
    Image *normal_image = add_solid_image("Normal", 128, 128, 255, 255);
    make_image_data(normal_image);
    bNode *normal_tex = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
    normal_tex->id = &normal_image->id;
    bNode *normal_map = bke::node_add_static_node(nullptr, ntree, SH_NODE_NORMAL_MAP);
    bke::node_add_link(ntree,
                       *normal_tex,
                       *bke::node_find_socket(*normal_tex, SOCK_OUT, "Color"_ustr),
                       *normal_map,
                       *bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr));
    bke::node_add_link(ntree,
                       *normal_map,
                       *bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));
    BKE_paint_layers_source_material_hash_forget(*source);
    return source;
  }

  /**
   * A source whose Alpha is a graph, so its coverage is `Pending`: until the alpha is rendered the
   * row lays nothing at all.
   */
  Material *add_pending_coverage_source(const char *name)
  {
    Material *source = add_source_material(name);
    bNodeTree &ntree = *source->nodetree;
    bNode *principled = nullptr;
    for (bNode &node : ntree.nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        principled = &node;
        break;
      }
    }
    if (principled == nullptr) {
      return source;
    }
    bNode *value = bke::node_add_static_node(nullptr, ntree, SH_NODE_VALUE);
    bke::node_add_link(ntree,
                       *value,
                       *bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr));
    BKE_paint_layers_source_material_hash_forget(*source);
    return source;
  }

  /** Set the plain Principled Base Color and Roughness values of a source. */
  static void set_source_constants(Material &source,
                                   const float base[3],
                                   const float roughness)
  {
    bNodeTree &ntree = *source.nodetree;
    for (bNode &node : ntree.nodes) {
      if (node.type_legacy != SH_NODE_BSDF_PRINCIPLED) {
        continue;
      }
      bNodeSocket *base_color = bke::node_find_socket(node, SOCK_IN, "Base Color"_ustr);
      if (base_color != nullptr && base_color->default_value != nullptr &&
          base_color->type == SOCK_RGBA)
      {
        float *value = static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value;
        value[0] = base[0];
        value[1] = base[1];
        value[2] = base[2];
      }
      bNodeSocket *rough = bke::node_find_socket(node, SOCK_IN, "Roughness"_ustr);
      if (rough != nullptr && rough->default_value != nullptr && rough->type == SOCK_FLOAT) {
        static_cast<bNodeSocketValueFloat *>(rough->default_value)->value = roughness;
      }
      break;
    }
    BKE_paint_layers_source_material_hash_forget(source);
  }

  /**
   * A Paint row below the Material row with maps on Base Color and Roughness. A Material row whose
   * source supplies those channels as constants has no map of its own, so without this there is no
   * stack size and no CPU pixel to compare against.
   */
  MaterialPaintLayer *add_underlay()
  {
    MaterialPaintLayer *layer = add_layer(
        "Underlay", add_solid_image("Underlay Base", 60, 80, 100, 255));
    EXPECT_NE(layer, nullptr);
    Image *roughness = add_solid_image("Underlay Roughness", 96, 96, 96, 255);
    make_image_data(roughness);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(
        *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, roughness));
    return layer;
  }

  MaterialPaintLayer *add_material_row(Material &source)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Material;
    params.source = &source;
    MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
    EXPECT_NE(row, nullptr);
    return row;
  }

  /** A Paint row with one Base Color map directly below \a anchor. */
  MaterialPaintLayer *add_layer_below(const char *name, MaterialPaintLayer &anchor, Image *image)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, name, &anchor, PaintLayerPlace::Below);
    EXPECT_NE(layer, nullptr);
    if (layer == nullptr) {
      return nullptr;
    }
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(
        *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
    return layer;
  }

  MaterialPaintLayer *add_point(MaterialPaintLayer &owner)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &owner, PaintLayerAnchorPointList::Effects, nullptr, false, "Point", {});
  }
  MaterialPaintLayer *add_reader(MaterialPaintLayer &owner, const bUUID &source)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &owner, PaintLayerAnchorPointList::Effects, nullptr, true, "Reader", source);
  }

  /** The one row group of \a layer, found by its marker rather than by its name. */
  bNodeTree *row_group(const MaterialPaintLayer &layer)
  {
    bNodeTree *found = nullptr;
    for (bNodeTree &tree : bmain->nodetrees) {
      bUUID marker = BLI_uuid_nil();
      if (!paint_layers::layer_tree_marker_get(tree, marker) || !BLI_uuid_equal(marker, layer.marker)) {
        continue;
      }
      EXPECT_EQ(found, nullptr) << "a row has exactly one kept group";
      found = &tree;
    }
    return found;
  }

  static const bNode *group_input_node(const bNodeTree &tree)
  {
    for (const bNode &node : tree.nodes) {
      if (node.is_group_input()) {
        return &node;
      }
    }
    return nullptr;
  }

  static const bNode *group_output_node(const bNodeTree &tree)
  {
    for (const bNode &node : tree.nodes) {
      if (node.is_group_output()) {
        return &node;
      }
    }
    return nullptr;
  }

  static int instance_count(const bNodeTree &root, const bNodeTree &group)
  {
    int count = 0;
    for (const bNode &node : root.nodes) {
      count += (node.id == &group.id) ? 1 : 0;
    }
    return count;
  }

  /** The Group Output input carrying the result \a result_name, or null. */
  static const bNodeSocket *result_socket(bNodeTree &root, const char *result_name)
  {
    root.ensure_interface_cache();
    const bNode *output = group_output_node(root);
    if (output == nullptr) {
      return nullptr;
    }
    for (bNodeTreeInterfaceSocket *iface : root.interface_outputs()) {
      if (iface->name == nullptr || !STREQ(iface->name, result_name) ||
          iface->identifier == nullptr)
      {
        continue;
      }
      return bke::node_find_socket(
          const_cast<bNode &>(*output), SOCK_IN, UString::from_ptr_noinline(iface->identifier));
    }
    return nullptr;
  }

  /**
   * Whether \a result_name's output socket is fed by an instance of \a group, directly or through
   * the row chain. That is what "the chain is not broken" means: the result is not a constant left
   * behind by a skipped row.
   */
  static bool instance_feeds_result(bNodeTree &root,
                                    const bNodeTree &group,
                                    const char *result_name)
  {
    const bNodeSocket *socket = result_socket(root, result_name);
    if (socket == nullptr) {
      return false;
    }
    const bNodeLink *start = nullptr;
    for (const bNodeLink &link : root.links) {
      if (link.tosock == socket) {
        start = &link;
        break;
      }
    }
    if (start == nullptr) {
      return false;
    }
    Vector<const bNode *> stack;
    stack.append(start->fromnode);
    while (!stack.is_empty()) {
      const bNode *node = stack.pop_last();
      if (node->id == &group.id) {
        return true;
      }
      for (const bNodeLink &link : root.links) {
        if (link.tonode == node) {
          stack.append(link.fromnode);
        }
      }
    }
    return false;
  }

  RGBA eval_root(bNodeTree &root, const eMaterialPaintChannel channel)
  {
    GraphInterpreter interpreter;
    interpreter.tree = &root;
    interpreter.x = 1;
    interpreter.y = 1;
    interpreter.object_mask_value = object_mask_value;
    return interpreter.eval_result(result_name(channel));
  }

  void expect_root_matches_cpu(bNodeTree &root,
                               const eMaterialPaintChannel channel,
                               const float tolerance = 2e-3f)
  {
    const RGBA graph = eval_root(root, channel);
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "channel " << int(channel);
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "channel " << int(channel);
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "channel " << int(channel);
  }

  /** Remove one interface input whose name starts with \a prefix; true when it was there. */
  static bool remove_interface_input_prefix(bNodeTree &tree, const char *prefix)
  {
    for (bNodeTreeInterfaceItem *item : tree.tree_interface.root_panel.items()) {
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

  /** Remove one interface socket with the exact \a name; true when it was there. */
  static bool remove_interface_socket(bNodeTree &tree, const char *name, const bool input)
  {
    for (bNodeTreeInterfaceItem *item : tree.tree_interface.root_panel.items()) {
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

  static bool interface_has_socket_prefix(const bNodeTree &tree, const char *prefix)
  {
    for (const bNodeTreeInterfaceItem *item : tree.tree_interface.root_panel.items()) {
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

  /** A user node in \a group that a rebuild from an empty tree would drop. */
  static bNode *add_user_marker(bNodeTree &group)
  {
    bNode *node = bke::node_add_static_node(nullptr, group, SH_NODE_VALUE);
    if (node != nullptr) {
      BLI_strncpy(node->name, "User Marker", sizeof(node->name));
    }
    return node;
  }

  /**
   * Whether \a group still holds the node #add_user_marker made. Found by name, not by address: a
   * rebuild frees the marker and creates many nodes of the same size, so the allocator can hand the
   * old address to a new node and an address comparison would say the marker survived.
   */
  static bool group_has_node(const bNodeTree &group, const bNode * /*marker*/)
  {
    for (const bNode &candidate : group.nodes) {
      if (STREQ(candidate.name, "User Marker")) {
        return true;
      }
    }
    return false;
  }

  /** The kept group of every row of \a ma, by the row's marker. */
  Map<const MaterialPaintLayer *, bNodeTree *> collect_row_groups()
  {
    Map<const MaterialPaintLayer *, bNodeTree *> groups;
    BKE_paint_layers_foreach(
        *ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) {
          for (bNodeTree &tree : bmain->nodetrees) {
            bUUID marker = BLI_uuid_nil();
            if (paint_layers::layer_tree_marker_get(tree, marker) && BLI_uuid_equal(marker, layer.marker)) {
              groups.add(&layer, &tree);
              break;
            }
          }
          return true;
        });
    return groups;
  }

  bNodeTree *add_root_tree(const char *prefix)
  {
    char name[64];
    SNPRINTF(name, "%s %d", prefix, tree_serial_++);
    return bke::node_tree_add_tree(bmain, name, "ShaderNodeTree");
  }

  int tree_serial_ = 0;

  /** How many instances of \a group any generated tree holds: a nested row lives in a folder. */
  int instance_count_everywhere(const bNodeTree &group)
  {
    int count = 0;
    for (bNodeTree &tree : bmain->nodetrees) {
      count += instance_count(tree, group);
    }
    return count;
  }
};

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_missing_channel_skips_and_restores)
{
  Material *source = add_source_material("Source");
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(paint_layer_row_participates(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  ASSERT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  bNode *user_marker = add_user_marker(*group);
  ASSERT_NE(user_marker, nullptr);
  const int nodes_before = int(group->nodes.count());

  /* Make the kept group look like one built before Roughness existed. The row still takes part in
   * the channel, so the relink is what fails; without an Anchor Point the channel must be skipped
   * rather than the group rebuilt. */
  ASSERT_TRUE(remove_interface_socket(*group, "Below Roughness", true));
  ASSERT_TRUE(remove_interface_socket(*group, "Result Roughness", false));

  Map<const MaterialPaintLayer *, bNodeTree *> groups = collect_row_groups();
  ASSERT_TRUE(groups.contains(row));
  const auto layer_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    return groups.lookup_default(&layer, nullptr);
  };
  const auto layer_unchanged = [&](const MaterialPaintLayer &layer) {
    return groups.contains(&layer);
  };
  bool invalidated = false;
  const auto layer_invalidate = [&](const MaterialPaintLayer &layer) {
    if (&layer == row) {
      invalidated = true;
      paint_layers::tree_hash_clear(*group);
    }
  };

  bNodeTree *root = add_root_tree("Root");
  ASSERT_NE(root, nullptr);
  PaintLayersBuildContext ctx;
  ctx.layer_tree_get = layer_tree_get;
  ctx.layer_unchanged = layer_unchanged;
  ctx.layer_invalidate = layer_invalidate;
  ctx.bmain = bmain;
  BKE_paint_layers_tree_build(*ma, *root, ctx);

  EXPECT_FALSE(invalidated)
      << "a row without an Anchor Point keeps the HEAD behaviour: the channel is skipped, the "
         "kept group is not invalidated";
  EXPECT_TRUE(group_has_node(*group, user_marker)) << "the kept group was not rebuilt";
  EXPECT_EQ(int(group->nodes.count()), nodes_before);
  EXPECT_EQ(instance_count(*root, *group), 1);
  EXPECT_TRUE(instance_feeds_result(*root, *group, "Result Base Color"));
  EXPECT_TRUE(instance_feeds_result(*root, *group, "Result Normal"));
  EXPECT_FALSE(instance_feeds_result(*root, *group, "Result Roughness"))
      << "the channel the kept group cannot lay is skipped";

  /* A pass that rebuilds from the description (the row is not unchanged there) restores Roughness. */
  bNodeTree *rebuilt_group = nullptr;
  const auto rebuilt_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    if (&layer != row) {
      return groups.lookup_default(&layer, nullptr);
    }
    if (rebuilt_group == nullptr) {
      rebuilt_group = add_root_tree("Rebuilt Row");
    }
    return rebuilt_group;
  };
  const auto rebuilt_unchanged = [&](const MaterialPaintLayer &layer) {
    return &layer != row && groups.contains(&layer);
  };
  bNodeTree *root_rebuilt = add_root_tree("Root Rebuilt");
  ASSERT_NE(root_rebuilt, nullptr);
  PaintLayersBuildContext rebuild_ctx;
  rebuild_ctx.layer_tree_get = rebuilt_tree_get;
  rebuild_ctx.layer_unchanged = rebuilt_unchanged;
  rebuild_ctx.bmain = bmain;
  BKE_paint_layers_tree_build(*ma, *root_rebuilt, rebuild_ctx);

  ASSERT_NE(rebuilt_group, nullptr);
  EXPECT_FALSE(group_has_node(*rebuilt_group, user_marker));
  EXPECT_TRUE(interface_has_socket_prefix(*rebuilt_group, "Below Roughness"));
  EXPECT_TRUE(interface_has_socket_prefix(*rebuilt_group, "Result Roughness"));
  EXPECT_EQ(instance_count(*root_rebuilt, *rebuilt_group), 1);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root_rebuilt, *rebuilt_group, result_name(channel)));
    expect_root_matches_cpu(*root_rebuilt, channel);
  }
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_anchor_point_missing_channel_rebuilds_immediately)
{
  Material *source = add_source_material("Source");
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *paint_source = add_layer(
      "PaintSource", add_solid_image("PaintSource", 230, 50, 20, 255));
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *point = add_point(*paint_source);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(add_reader(*row, point->marker), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  bNode *user_marker = add_user_marker(*group);
  ASSERT_NE(user_marker, nullptr);
  ASSERT_TRUE(remove_interface_input_prefix(*group, "AnchorPoint "));
  ASSERT_TRUE(remove_interface_socket(*group, "Below Roughness", true));
  ASSERT_TRUE(remove_interface_socket(*group, "Result Roughness", false));

  Map<const MaterialPaintLayer *, bNodeTree *> groups = collect_row_groups();
  const auto layer_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    return groups.lookup_default(&layer, nullptr);
  };
  const auto layer_unchanged = [&](const MaterialPaintLayer &layer) {
    return groups.contains(&layer);
  };
  bNodeTree *root = add_root_tree("Root");
  ASSERT_NE(root, nullptr);
  PaintLayersBuildContext ctx;
  ctx.layer_tree_get = layer_tree_get;
  ctx.layer_unchanged = layer_unchanged;
  ctx.bmain = bmain;
  BKE_paint_layers_tree_build(*ma, *root, ctx);

  /* A row with an Anchor Point is rebuilt at once; it is never left with a channel missing. */
  EXPECT_FALSE(group_has_node(*group, user_marker)) << "the reader row rebuilt its group";
  EXPECT_TRUE(interface_has_socket_prefix(*group, "AnchorPoint "))
      << "the removed reader input came back";
  EXPECT_EQ(instance_count(*root, *group), 1);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root, *group, result_name(channel)));
    expect_root_matches_cpu(*root, channel);
  }
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_first_build_and_reused_build_match)
{
  Material *source = add_source_material("Source");
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  const int group_links_first = int(group->links.count());
  const RGBA base_first = eval_root(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA rough_first = eval_root(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  const RGBA normal_first = eval_root(*root, PAINT_MATERIAL_CHANNEL_NORMAL);

  /* A structural edit forces the rebuilt pass: the row's group is kept and re-linked. The extra
   * row lays only Metallic, which this row does not lay, so the compared channels stay the same. */
  MaterialPaintLayer *extra = BKE_paint_layers_add(
      *ma, "Extra", row, PaintLayerPlace::Below);
  ASSERT_NE(extra, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, extra, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma,
      extra,
      PAINT_MATERIAL_CHANNEL_METALLIC,
      add_solid_image("Extra Metallic", 10, 10, 10, 255)));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  group = row_group(*row);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(int(group->links.count()), group_links_first)
      << "the kept row group's own chain is unchanged";

  const RGBA base_second = eval_root(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA rough_second = eval_root(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  const RGBA normal_second = eval_root(*root, PAINT_MATERIAL_CHANNEL_NORMAL);
  for (const auto &pair : {std::pair<RGBA, RGBA>{base_first, base_second},
                           std::pair<RGBA, RGBA>{rough_first, rough_second},
                           std::pair<RGBA, RGBA>{normal_first, normal_second}})
  {
    EXPECT_NEAR(pair.first.r, pair.second.r, 1e-5f);
    EXPECT_NEAR(pair.first.g, pair.second.g, 1e-5f);
    EXPECT_NEAR(pair.first.b, pair.second.b, 1e-5f);
  }

  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_NORMAL);
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_anchor_point_reader_rebuild_matches_cpu)
{
  Material *source = add_source_material("Source");
  MaterialPaintLayer *paint_source = add_layer(
      "PaintSource", add_solid_image("PaintSource", 230, 50, 20, 255));
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *point = add_point(*paint_source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader = add_reader(*row, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  bNode *user_marker = add_user_marker(*group);
  ASSERT_NE(user_marker, nullptr);
  ASSERT_TRUE(group_has_node(*group, user_marker));
  /* The saved group lost a required reader input: the relink fails and, because the row has an
   * Anchor Point, the rebuild path must take over instead of dropping the row. */
  ASSERT_TRUE(remove_interface_input_prefix(*group, "AnchorPoint "));
  MaterialPaintLayer *extra = add_layer_below(
      "Extra", *row, add_solid_image("Extra", 10, 200, 30, 255));
  ASSERT_NE(extra, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  group = row_group(*row);
  ASSERT_NE(group, nullptr);
  EXPECT_FALSE(group_has_node(*group, user_marker))
      << "the saved group was rebuilt from its description";
  EXPECT_TRUE(interface_has_socket_prefix(*group, "AnchorPoint "))
      << "the missing reader input came back";
  EXPECT_EQ(instance_count(*root, *group), 1)
      << "the row is rebuilt and instantiated exactly once";
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root, *group, result_name(channel)));
    expect_root_matches_cpu(*root, channel);
  }
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_source_group_all_channels_wired)
{
  Image *metallic_image = add_solid_image("Source Metallic", 40, 40, 40, 255);
  make_image_data(metallic_image);
  Material *source = add_source_material(
      "Graph Source", metallic_image, /*graph_base=*/true);
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);

  /* The Base Color graph has to take part as a SourceGroup; a Material row shows a graph channel
   * only while it also has a map the CPU side can read, so give it one. */
  Image *baked_base = add_solid_image("Baked Base", 70, 90, 110, 255);
  const std::array<int, 1> bake_channels = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const std::array<Image *, 1> bake_images = {baked_base};
  BKE_paint_layers_material_bake_apply(
      *bmain, *ma, *row, 4, bake_channels, bake_images);
  ASSERT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC));
  ASSERT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  ASSERT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_EQ(paint_layer_material_channel_source(
                *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayerMaterialUse::Graph)
                .kind,
            PaintLayerMaterialSourceKind::SourceGroup);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root, *group, result_name(channel)))
        << "channel " << int(channel) << " is lost from the root chain";
  }
  /* The CPU cannot evaluate a SourceGroup, so Base Color is wiring-only; the other three are
   * live maps/constants and must agree with the CPU. */
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_METALLIC);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_NORMAL);
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_first_build_after_add)
{
  Material *source = add_source_material("Source");
  MaterialPaintLayer *underlay = add_underlay();
  ASSERT_NE(underlay, nullptr);
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);

  const auto check = [&](const bool expect_value_edit_kept) {
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    bNodeTree *group = row_group(*row);
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(instance_count_everywhere(*group), 1);
    for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                                PAINT_MATERIAL_CHANNEL_NORMAL})
    {
      EXPECT_TRUE(paint_layer_row_participates(*ma, *row, channel));
    }
    if (expect_value_edit_kept) {
      bNodeTree *root = ma->paint_layers_tree;
      ASSERT_NE(root, nullptr);
      expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
      expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_NORMAL);
    }
  };

  /* First build, then a value edit of a neighbour. */
  check(true);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, underlay, 0.3f));
  check(true);

  /* A Pass Through folder takes the row without losing any channel. */
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, folder, PaintLayerPlace::Into));
  check(false);

  /* And out again. */
  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, nullptr, PaintLayerPlace::Above));
  check(true);
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_in_folder_and_passthrough)
{
  Material *source = add_source_material("Source");
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *paint_row = add_layer(
      "Paint", add_solid_image("Paint", 200, 40, 60, 255));
  ASSERT_NE(paint_row, nullptr);
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);

  /* A Pass Through folder above the paint row. */
  MaterialPaintLayer *passthrough = BKE_paint_layers_add(
      *ma, "Pass Through", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(passthrough, nullptr);
  passthrough->source = MA_PAINT_LAYER_SOURCE_STACK;
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*passthrough));
  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, passthrough, PaintLayerPlace::Into));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(instance_count_everywhere(*group), 1);
  expect_root_matches_cpu(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_root_matches_cpu(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_NORMAL);

  /* A regular folder: raising its opacity takes it out of Pass Through. */
  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, nullptr, PaintLayerPlace::Above));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, passthrough, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*passthrough));
  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, passthrough, PaintLayerPlace::Into));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = row_group(*row);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(instance_count_everywhere(*group), 1);
  expect_root_matches_cpu(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_root_matches_cpu(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_NORMAL);
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_source_change_rebuilds_all_channels)
{
  Material *source_a = add_source_material("Source A");
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *row = add_material_row(*source_a);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC));

  Image *metallic = add_solid_image("Source B Metallic", 30, 30, 30, 255);
  make_image_data(metallic);
  Material *source_b = add_source_material(
      "Source B", metallic, /*graph_base=*/false);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source_b));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *group = row_group(*row);
  ASSERT_NE(root, nullptr);
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC));
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root, *group, result_name(channel)));
  }
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_METALLIC);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_NORMAL);
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_no_main_branch_keeps_other_channels)
{
  Material *source = add_source_material("Source");
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *paint_source = add_layer(
      "PaintSource", add_solid_image("PaintSource", 230, 50, 20, 255));
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  /* The no-Main rebuild branch is only reached for a row an Anchor Point touches; a plain row
   * keeps the HEAD skip. */
  MaterialPaintLayer *point = add_point(*paint_source);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(add_reader(*row, point->marker), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  bNode *user_marker = add_user_marker(*group);
  ASSERT_NE(user_marker, nullptr);
  ASSERT_TRUE(remove_interface_socket(*group, "Below Roughness", true));
  ASSERT_TRUE(remove_interface_socket(*group, "Result Roughness", false));

  Map<const MaterialPaintLayer *, bNodeTree *> groups = collect_row_groups();
  const auto layer_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    return groups.lookup_default(&layer, nullptr);
  };
  const auto layer_unchanged = [&](const MaterialPaintLayer &layer) {
    return groups.contains(&layer);
  };
  bool invalidated = false;
  const auto layer_invalidate = [&](const MaterialPaintLayer &layer) {
    if (&layer == row) {
      invalidated = true;
      paint_layers::tree_hash_clear(*group);
    }
  };
  bNodeTree *root = add_root_tree("Pure Root");
  ASSERT_NE(root, nullptr);
  PaintLayersBuildContext ctx;
  ctx.layer_tree_get = layer_tree_get;
  ctx.layer_unchanged = layer_unchanged;
  ctx.layer_invalidate = layer_invalidate;
  ctx.bmain = nullptr;
  BKE_paint_layers_tree_build(*ma, *root, ctx);

  EXPECT_TRUE(invalidated);
  EXPECT_TRUE(group_has_node(*group, user_marker)) << "a pure build cannot rebuild the group";
  /* Without a Main the row's instance is dropped for this pass, but the next real pass restores
   * every channel from the invalidated description. A structural edit forces that full pass. */
  ASSERT_NE(add_layer("Trigger", add_solid_image("Trigger", 5, 10, 15, 255)), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = row_group(*row);
  root = ma->paint_layers_tree;
  ASSERT_NE(group, nullptr);
  ASSERT_NE(root, nullptr);
  EXPECT_FALSE(group_has_node(*group, user_marker));
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root, *group, result_name(channel)));
    expect_root_matches_cpu(*root, channel);
  }
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_row_all_source_kinds_keep_every_channel)
{
  Image *live_metallic = add_solid_image("Live Metallic", 35, 35, 35, 255);
  make_image_data(live_metallic);
  /* Base is a graph (SourceGroup), Metallic is None. */
  Material *all_source = add_source_material("All Kinds", nullptr, /*graph_base=*/true);
  /* Base is a plain value, Metallic a live image, Normal an image. */
  Material *image_source = add_source_material("Live Image", live_metallic, false);
  /* Alpha is a graph, so coverage is Pending and the row lays nothing until it is rendered. */
  Material *pending_source = add_pending_coverage_source("Pending Coverage");

  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *pending_row = add_material_row(*pending_source);
  MaterialPaintLayer *image_row = add_material_row(*image_source);
  MaterialPaintLayer *all_row = add_material_row(*all_source);
  ASSERT_NE(pending_row, nullptr);
  ASSERT_NE(image_row, nullptr);
  ASSERT_NE(all_row, nullptr);

  /* Control: in Auto a graph alpha is wrapped live (SourceGroup), so the row does lay. Pending is
   * what Always gives a source graph with no coverage map rendered yet. */
  EXPECT_TRUE(paint_layer_row_participates(*ma, *pending_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
      << "in Auto the graph alpha is wrapped, so the row participates";
  BKE_paint_layers_bake_mode_set(*ma, *pending_row, MA_PAINT_LAYER_BAKE_ALWAYS);

  Image *baked_base = add_solid_image("All Kinds Baked", 70, 90, 110, 255);
  const std::array<int, 1> bake_channels = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const std::array<Image *, 1> bake_images = {baked_base};
  BKE_paint_layers_material_bake_apply(
      *bmain, *ma, *all_row, 4, bake_channels, bake_images);

  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_FALSE(paint_layer_row_participates(*ma, *pending_row, channel))
        << "Pending coverage keeps the row out of every channel";
  }
  EXPECT_TRUE(paint_layer_row_participates(*ma, *image_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *image_row, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *image_row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *image_row, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_NORMAL));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  bNodeTree *all_group = row_group(*all_row);
  bNodeTree *image_group = row_group(*image_row);
  ASSERT_NE(all_group, nullptr);
  ASSERT_NE(image_group, nullptr);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root, *all_group, result_name(channel)));
  }
  /* The top row does not lay Metallic, so the row below it still feeds that result. */
  EXPECT_FALSE(instance_feeds_result(*root, *all_group, "Result Metallic"));
  EXPECT_TRUE(instance_feeds_result(*root, *image_group, "Result Metallic"));
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_NORMAL);

  /* The same rows in the other order lay the same set of channels: moving rows is not a channel
   * order and cannot lose them. */
  ASSERT_TRUE(BKE_paint_layers_move(*ma, all_row, image_row, PaintLayerPlace::Below));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  root = ma->paint_layers_tree;
  all_group = row_group(*all_row);
  image_group = row_group(*image_row);
  ASSERT_NE(root, nullptr);
  ASSERT_NE(all_group, nullptr);
  ASSERT_NE(image_group, nullptr);
  EXPECT_TRUE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *all_row, PAINT_MATERIAL_CHANNEL_NORMAL));
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    EXPECT_TRUE(instance_feeds_result(*root, *image_group, result_name(channel)));
  }
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_METALLIC);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_NORMAL);
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_material_rows_do_not_share_source_state)
{
  const float base_a[3] = {0.15f, 0.40f, 0.75f};
  const float base_b[3] = {0.75f, 0.15f, 0.05f};
  Material *source_a = add_source_material("Source A");
  set_source_constants(*source_a, base_a, 0.8f);
  Material *source_b = add_source_material("Source B");
  set_source_constants(*source_b, base_b, 0.2f);

  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *row_a = add_material_row(*source_a);
  MaterialPaintLayer *row_b = add_material_row(*source_b);
  ASSERT_NE(row_a, nullptr);
  ASSERT_NE(row_b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row_a, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row_b, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  bNodeTree *group_a = row_group(*row_a);
  bNodeTree *group_b = row_group(*row_b);
  ASSERT_NE(group_a, nullptr);
  ASSERT_NE(group_b, nullptr);
  EXPECT_EQ(instance_count(*root, *group_a), 1);
  EXPECT_EQ(instance_count(*root, *group_b), 1);
  const RGBA base_a_over_b = eval_root(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA rough_a_over_b = eval_root(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  /* Swapping the rows must change the result: each row keeps its own source constants rather
   * than sharing one input socket. */
  ASSERT_TRUE(BKE_paint_layers_move(*ma, row_a, row_b, PaintLayerPlace::Above));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const RGBA base_b_over_a = eval_root(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA rough_b_over_a = eval_root(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_GT(std::abs(base_a_over_b.r - base_b_over_a.r), 1e-3f);
  EXPECT_GT(std::abs(rough_a_over_b.r - rough_b_over_a.r), 1e-3f);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

/* -------------------------------------------------------------------- */
/** \name The topology hash catches structural edits of a Material source
 *
 * The same contract as in the Anchor Point tests: a kept group is only re-linked, so the hash has
 * to move for each edit below and the factory clears the group before the build. The safety net
 * (#paint_layers_kept_group_rebuild_count) must stay untouched.
 * \{ */

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_hash_catches_mask_source_channel_change)
{
  Material *mask_source = add_source_material("MaskSource");
  const float base[3] = {0.2f, 0.2f, 0.2f};
  set_source_constants(*mask_source, base, 0.9f);
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 255, 0, 0, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_material_add(*ma, row, "Mask", mask_source);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The grey now comes from Roughness: other group inputs, other values. */
  paint_layers::paint_layers_kept_group_rebuild_count_reset();
  ASSERT_TRUE(
      BKE_paint_layers_mask_source_channel_set(*ma, item, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(paint_layers::paint_layers_kept_group_rebuild_count(), 0)
      << "the mask item reads another channel: the hash of the row follows it";
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalMaterialRowTest, graph_hash_catches_source_gaining_a_channel)
{
  Material *source = add_source_material("Source");
  ASSERT_NE(add_underlay(), nullptr);
  MaterialPaintLayer *paint_source = add_layer(
      "PaintSource", add_solid_image("PaintSource", 230, 50, 20, 255));
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *point = add_point(*paint_source);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(add_reader(*row, point->marker), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Metallic was an Image Texture with no image, a channel the row could not lay; with a map it is
   * one more channel to build, with the sockets that come with it. */
  Image *metallic_image = add_solid_image("Metallic", 200, 200, 200, 255);
  make_image_data(metallic_image);
  bNodeTree &source_tree = *source->nodetree;
  for (bNode &node : source_tree.nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == nullptr) {
      node.id = &metallic_image->id;
      break;
    }
  }
  BKE_paint_layers_source_material_hash_forget(*source);

  paint_layers::paint_layers_kept_group_rebuild_count_reset();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(paint_layers::paint_layers_kept_group_rebuild_count(), 0)
      << "the source supplies a channel it did not: the live signature in the hash moves";
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  bNodeTree *group = row_group(*row);
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(instance_feeds_result(*root, *group, result_name(PAINT_MATERIAL_CHANNEL_METALLIC)));
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_root_matches_cpu(*root, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

/** \} */

}  // namespace blender::bke::tests
