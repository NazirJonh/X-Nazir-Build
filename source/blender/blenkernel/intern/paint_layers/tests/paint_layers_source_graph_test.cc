/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Source-graph tests: a material converted from its own node graph (the adoption of its links, the
 * release and the revert), the CPU cache that graph is read through, and the conversion fill.
 *
 * The fixture lives in `paint_layers_generate_test_fixture.hh`.
 */

#include "paint_layers_generate_test_fixture.hh"

#include "BKE_idprop.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_source_graph.hh"

#include "BLI_map.hh"
#include "BLI_math_vector.h"

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_generate_layout.hh"

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Source graph: a material converted from its own node graph
 *
 * A converted material's stack composites over the channels the author's graph feeds: the chain
 * bottoms are `Source <channel>` interface inputs, the links that fed the Principled are re-aimed
 * onto them, and not one node of the graph is edited on the way.
 * \{ */

/** An Image Texture feeding \a principled's Base Color, the shape a converted material starts
 * from. */
static bNode *base_color_image_add(bNodeTree &tree, bNode &principled)
{
  bNode *image = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  if (image == nullptr) {
    return nullptr;
  }
  bke::node_add_link(tree,
                     *image,
                     *bke::node_find_socket(*image, SOCK_OUT, "Color"_ustr),
                     principled,
                     *bke::node_find_socket(principled, SOCK_IN, "Base Color"_ustr));
  return image;
}

TEST_F(PaintLayersGenerateTest, source_graph_adopts_the_graphs_channel_links)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_TRUE(BKE_paint_layers_source_graph_get(*ma));
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.adopted_source_links);
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Source Base Color", false));
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Result Base Color", true));

  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  bNodeSocket *source_in = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(source_iface->identifier));
  ASSERT_NE(source_in, nullptr);

  /* The author's image ends at the stack's bottom now, and the stack's result holds the Principled
   * input the image used to feed. */
  const bNodeLink *adopted = fed_by_one(tree, *source_in);
  ASSERT_NE(adopted, nullptr);
  EXPECT_EQ(adopted->fromnode, image);
  const bNodeLink *wired = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(wired, nullptr);
  EXPECT_EQ(wired->fromnode, instance);

  /* Nothing of the graph was edited: the image is still there, with the one link it had. */
  const bNodeLink *still = fed_by_one(
      tree, *bke::node_find_socket(*image, SOCK_OUT, "Color"_ustr));
  EXPECT_EQ(still, adopted);

  /* A second pass finds the stack already wired: it adopts nothing and moves no link twice. */
  PaintLayersRegenerateReport second;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &second));
  EXPECT_FALSE(second.adopted_source_links);
  bNode *instance_again = instance_find();
  ASSERT_NE(instance_again, nullptr);
  const bNodeTreeInterfaceSocket *source_iface_again = interface_input_find(*ma->paint_layers_tree,
                                                                            "Source Base Color");
  ASSERT_NE(source_iface_again, nullptr);
  bNodeSocket *source_in_again = bke::node_find_socket(
      *instance_again, SOCK_IN, UString::from_ptr_noinline(source_iface_again->identifier));
  ASSERT_NE(source_in_again, nullptr);
  const bNodeLink *kept = fed_by_one(tree, *source_in_again);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->fromnode, image);

  /* A structural edit rebuilds the root, and with it the interface: the sockets come back with
   * fresh identifiers, so the author's link has to be re-aimed rather than dropped. */
  ASSERT_NE(add_paint_layer("Paint Top", add_image("Paint Top")), nullptr);
  PaintLayersRegenerateReport rebuilt;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &rebuilt));
  EXPECT_FALSE(rebuilt.adopted_source_links);
  bNode *instance_rebuilt = instance_find();
  ASSERT_NE(instance_rebuilt, nullptr);
  const bNodeTreeInterfaceSocket *source_iface_rebuilt = interface_input_find(
      *ma->paint_layers_tree, "Source Base Color");
  ASSERT_NE(source_iface_rebuilt, nullptr);
  bNodeSocket *source_in_rebuilt = bke::node_find_socket(
      *instance_rebuilt, SOCK_IN, UString::from_ptr_noinline(source_iface_rebuilt->identifier));
  ASSERT_NE(source_in_rebuilt, nullptr);
  const bNodeLink *survived = fed_by_one(tree, *source_in_rebuilt);
  ASSERT_NE(survived, nullptr);
  EXPECT_EQ(survived->fromnode, image);
  const bNodeLink *rewired = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(rewired, nullptr);
  EXPECT_EQ(rewired->fromnode, instance_rebuilt);
}

TEST_F(PaintLayersGenerateTest, source_graph_carries_the_unlinked_value_as_the_bottom)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  /* A value the graph never linked: it is the whole answer for the channel, so it becomes the
   * Source input's default instead of a moved link. */
  const float grey[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, grey);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.adopted_source_links);
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  ASSERT_NE(source_iface->socket_data, nullptr);
  const float *value = static_cast<const bNodeSocketValueRGBA *>(source_iface->socket_data)->value;
  EXPECT_FLOAT_EQ(value[0], grey[0]);
  EXPECT_FLOAT_EQ(value[1], grey[1]);
  EXPECT_FLOAT_EQ(value[2], grey[2]);
}

TEST_F(PaintLayersGenerateTest, source_graph_wires_every_channel_of_the_set)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  ASSERT_NE(principled, nullptr);

  /* A set wider than the rows: Metallic has no row, and the graph's own metallic has to reach the
   * Principled through the stack all the same. */
  const uint32_t set = uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR |
                       uint32_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC;
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(*ma, set));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  const Vector<int> wired = paint_layers::paint_layers_wired_channels(*ma);
  EXPECT_TRUE(std::find(wired.begin(), wired.end(), PAINT_MATERIAL_CHANNEL_METALLIC) !=
              wired.end());
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Source Metallic", false));
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Result Metallic", true));

  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeLink *metallic = fed_by_one(tree, *principled_input("Metallic"));
  ASSERT_NE(metallic, nullptr);
  EXPECT_EQ(metallic->fromnode, instance);
}

TEST_F(PaintLayersGenerateTest, source_graph_adopts_a_reroute_between_the_node_and_principled)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  bNode *reroute = bke::node_add_node(nullptr, tree, "NodeReroute"_ustr);
  ASSERT_NE(image, nullptr);
  ASSERT_NE(reroute, nullptr);
  bNodeSocket *reroute_in = static_cast<bNodeSocket *>(reroute->inputs.first);
  bNodeSocket *reroute_out = static_cast<bNodeSocket *>(reroute->outputs.first);
  ASSERT_NE(reroute_in, nullptr);
  ASSERT_NE(reroute_out, nullptr);
  bNodeSocket *principled_base = bke::node_find_socket(
      *principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(principled_base, nullptr);
  bke::node_add_link(tree,
                     *image,
                     *bke::node_find_socket(*image, SOCK_OUT, "Color"_ustr),
                     *reroute,
                     *reroute_in);
  bke::node_add_link(tree, *reroute, *reroute_out, *principled, *principled_base);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  bNodeSocket *source_in = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(source_iface->identifier));
  ASSERT_NE(source_in, nullptr);

  /* The reroute and the image are still there, and the reroute now ends at the stack's bottom. */
  const bNodeLink *adopted = fed_by_one(tree, *source_in);
  ASSERT_NE(adopted, nullptr);
  EXPECT_EQ(adopted->fromnode, reroute);
  const bNodeLink *into_reroute = fed_by_one(tree, *reroute_in);
  ASSERT_NE(into_reroute, nullptr);
  EXPECT_EQ(into_reroute->fromnode, image);
}

TEST_F(PaintLayersGenerateTest, source_graph_leaves_the_users_normal_chain_on_the_principled)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *normal_map = bke::node_add_static_node(nullptr, tree, SH_NODE_NORMAL_MAP);
  ASSERT_NE(normal_map, nullptr);
  bNodeSocket *normal_out = bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr);
  bNodeSocket *principled_normal = bke::node_find_socket(
      *principled, SOCK_IN, "Normal"_ustr);
  ASSERT_NE(normal_out, nullptr);
  ASSERT_NE(principled_normal, nullptr);
  bke::node_add_link(tree, *normal_map, *normal_out, *principled, *principled_normal);

  const uint32_t set = uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR |
                       uint32_t(1) << PAINT_MATERIAL_CHANNEL_NORMAL |
                       uint32_t(1) << PAINT_MATERIAL_CHANNEL_HEIGHT;
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(*ma, set));
  BKE_paint_layers_source_graph_set(*ma, true);
  MaterialPaintLayer *paint = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_NE(paint, nullptr);
  add_channel(*paint, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Paint Normal"));
  add_channel(*paint, PAINT_MATERIAL_CHANNEL_HEIGHT, add_image("Paint Height"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));

  /* The author's Normal Map still feeds the Principled: no Source input was made for either
   * channel, and the stack's own Normal/Height chain was not routed over it. */
  EXPECT_FALSE(interface_has_socket(*ma->paint_layers_tree, "Source Normal", false));
  EXPECT_FALSE(interface_has_socket(*ma->paint_layers_tree, "Source Height", false));
  const bNodeLink *normal = fed_by_one(tree, *principled_normal);
  ASSERT_NE(normal, nullptr);
  EXPECT_EQ(normal->fromnode, normal_map);
}

TEST_F(PaintLayersGenerateTest, source_graph_only_adopts_the_principled_it_renders_through)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  ASSERT_NE(principled, nullptr);
  bNode *other_principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(other_principled, nullptr);
  bNode *other_image = base_color_image_add(tree, *other_principled);
  ASSERT_NE(other_image, nullptr);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Only the Principled the Material Output renders through is wired; the second one keeps its
   * own graph, which is what the operator's refusal warns about before conversion. */
  bNodeSocket *other_base = bke::node_find_socket(
      *other_principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(other_base, nullptr);
  const bNodeLink *untouched = fed_by_one(tree, *other_base);
  ASSERT_NE(untouched, nullptr);
  EXPECT_EQ(untouched->fromnode, other_image);
  const bNodeLink *first = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->fromnode, instance_find());
}

TEST_F(PaintLayersGenerateTest, source_graph_rebuild_survives_the_user_deleting_a_node)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bke::node_remove_node(bmain, tree, *image, true);
  ASSERT_NE(add_paint_layer("Paint Top", add_image("Paint Top")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  bNodeSocket *source_in = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(source_iface->identifier));
  ASSERT_NE(source_in, nullptr);
  /* The deleted node's link is gone; the source falls back to its default, and the stack is whole. */
  EXPECT_EQ(fed_by_one(tree, *source_in), nullptr);
  const bNodeLink *stack_link = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(stack_link, nullptr);
  EXPECT_EQ(stack_link->fromnode, instance);
}

TEST_F(PaintLayersGenerateTest, source_graph_release_puts_the_links_and_values_back)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  bNodeSocket *roughness = bke::node_find_socket(
      *principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(image, nullptr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.37f;

  BKE_paint_layers_source_graph_capture(*ma);
  ASSERT_TRUE(BKE_paint_layers_source_graph_can_release(*ma));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma,
                                            (uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR) |
                                                (uint32_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS)));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const bNodeLink *adopted = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(adopted, nullptr);
  EXPECT_NE(adopted->fromnode, image);

  BKE_paint_layers_source_graph_release(*ma);

  EXPECT_FALSE(BKE_paint_layers_source_graph_get(*ma));
  EXPECT_FALSE(BKE_paint_layers_source_graph_can_release(*ma));
  const bNodeLink *restored = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->fromnode, image);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value, 0.37f);
}

TEST_F(PaintLayersGenerateTest, source_graph_copy_keeps_the_mode_and_the_release_record)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);
  BKE_paint_layers_source_graph_capture(*ma);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  EXPECT_TRUE(BKE_paint_layers_source_graph_get(*copy));
  EXPECT_TRUE(BKE_paint_layers_source_graph_can_release(*copy));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *copy));

  BKE_paint_layers_source_graph_release(*copy);
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *copy_principled = BKE_paint_material_principled_find(*copy, reason);
  ASSERT_NE(copy_principled, nullptr);
  bNodeSocket *copy_base = bke::node_find_socket(
      const_cast<bNode &>(*copy_principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(copy_base, nullptr);
  const bNodeLink *restored = fed_by_one(*copy->nodetree, *copy_base);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->fromnode->type_legacy, SH_NODE_TEX_IMAGE);
  EXPECT_FALSE(BKE_paint_layers_source_graph_get(*copy));
}

TEST_F(PaintLayersGenerateTest, source_graph_undo_prepare_and_validate_keep_the_record)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);
  BKE_paint_layers_source_graph_capture(*ma);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  BKE_paint_layers_undo_prepare(*bmain);
  BKE_paint_layers_undo_validate(*bmain);
  BKE_paint_layers_regenerate_tagged(*bmain);

  EXPECT_TRUE(BKE_paint_layers_source_graph_get(*ma));
  EXPECT_TRUE(BKE_paint_layers_source_graph_can_release(*ma));
  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  bNodeSocket *source_in = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(source_iface->identifier));
  ASSERT_NE(source_in, nullptr);
  const bNodeLink *kept = fed_by_one(tree, *source_in);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->fromnode, image);
}

TEST_F(PaintLayersGenerateTest, source_graph_record_reverts_a_bake_style_rewire)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  bNode *replacement = bke::node_add_static_node(nullptr, tree, SH_NODE_BUMP);
  ASSERT_NE(image, nullptr);
  ASSERT_NE(replacement, nullptr);
  bNodeSocket *base = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base, nullptr);

  BKE_paint_layers_source_graph_capture(*ma);
  /* What a bake does: the stack's result, not the author's node, feeds the Principled. The
   * replacement carries the stack's ownership marker (#RESPONSE_NODE_KEY_PROP), because that is
   * what tells a release the link is the stack's own wiring; an unmarked feed is the user's and a
   * release leaves it. */
  bke::node_remove_socket_links(tree, *base);
  bke::node_add_link(tree,
                     *replacement,
                     *bke::node_find_socket(*replacement, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *base);
  paint_layers::prop_string_set(replacement->system_properties,
                                paint_layers::RESPONSE_NODE_KEY_PROP,
                                paint_layers::RESPONSE_NODE_BUMP);

  BKE_paint_layers_source_graph_release(*ma);

  const bNodeLink *restored = fed_by_one(tree, *base);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->fromnode, image);
  EXPECT_FALSE(BKE_paint_layers_source_graph_can_release(*ma));
}

/** How many nodes of \a tree the stack marks as its own (#RESPONSE_NODE_KEY_PROP). */
static int marked_node_count(const bNodeTree &tree)
{
  int count = 0;
  for (const bNode &node : tree.nodes) {
    if (paint_layers::node_is_generator_owned(node)) {
      count++;
    }
  }
  return count;
}

/** How many nodes of \a tree carry the integer \a key marker in their user properties. */
static int marked_node_count_for_prop(const bNodeTree &tree, const char *key)
{
  int count = 0;
  for (const bNode &node : tree.nodes) {
    if (paint_layers::prop_int_get(node.prop, key, 0) != 0) {
      count++;
    }
  }
  return count;
}

TEST_F(PaintLayersGenerateTest, revert_keeps_a_link_added_after_the_conversion)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);
  bNodeSocket *base = principled_input("Base Color");
  ASSERT_NE(base, nullptr);

  BKE_paint_layers_source_graph_capture(*ma);
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const bNodeLink *wired = fed_by_one(tree, *base);
  ASSERT_NE(wired, nullptr);
  EXPECT_NE(wired->fromnode, image);

  /* What the user adds afterwards: their own node takes the stack's input over. The link's source
   * is neither the instance nor a marked helper, so the revert treats it as theirs and the record
   * stays unused. */
  bNode *rgb = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  ASSERT_NE(rgb, nullptr);
  bke::node_remove_socket_links(tree, *base);
  bke::node_add_link(tree,
                     *rgb,
                     *bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *base);

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  EXPECT_TRUE(report.lost_channels.is_empty());

  const bNodeLink *kept = fed_by_one(tree, *base);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->fromnode, rgb);
  EXPECT_FALSE(BKE_paint_layers_is_layered(*ma));
  EXPECT_FALSE(BKE_paint_layers_source_graph_can_release(*ma));
  EXPECT_EQ(ma->paint_layers_tree, nullptr);
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
}

TEST_F(PaintLayersGenerateTest, revert_and_reconvert_keep_one_stack)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_capture(*ma);
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int rows_before = BLI_listbase_count(&ma->paint_layers);
  ASSERT_GT(rows_before, 0);

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  /* The records sleep: nothing the user painted goes away with the stack. */
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), rows_before);

  /* What the conversion does over sleeping rows: the set and the mode again, the layered flag back
   * up, and no second base row. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_capture(*ma);
  BKE_paint_layers_source_graph_set(*ma, true);
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), rows_before);
  EXPECT_NE(instance_find(), nullptr);
  const bNodeLink *base = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(base, nullptr);
  /* The stack composites over the graph again: the instance feeds the Principled, not the image. */
  EXPECT_NE(base->fromnode, image);
  EXPECT_TRUE(BKE_paint_layers_source_graph_get(*ma));
}

TEST_F(PaintLayersGenerateTest, revert_removes_the_generator_helpers_and_rewires_the_output)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);
  bNode *output = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL) {
      output = &node;
      break;
    }
  }
  ASSERT_NE(output, nullptr);

  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(*ma, normal_and_height_set()));
  BKE_paint_layers_source_graph_capture(*ma);
  BKE_paint_layers_source_graph_set(*ma, true);
  MaterialPaintLayer *row = add_paint_layer("Paint", add_image("Paint"));
  add_channel(*row, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The stack routes Normal through its own minted nodes in the material's tree, and the viewport
   * pass viewer adds its Mix and Emission between the Principled and the output. */
  ASSERT_GT(marked_node_count(tree), 0);
  ASSERT_TRUE(BKE_paint_layers_view_pass_set(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(BKE_paint_layers_view_pass_sync(bmain, *ma));
  bNodeSocket *surface = bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr);
  ASSERT_NE(surface, nullptr);
  const bNodeLink *viewed = fed_by_one(tree, *surface);
  ASSERT_NE(viewed, nullptr);
  EXPECT_NE(viewed->fromnode, principled);

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  EXPECT_TRUE(report.lost_channels.is_empty());

  EXPECT_EQ(marked_node_count(tree), 0);
  EXPECT_EQ(marked_node_count_for_prop(tree, paint_layers::VIEW_PASS_MIX_PROP), 0);
  EXPECT_EQ(marked_node_count_for_prop(tree, paint_layers::VIEW_PASS_EMISSION_PROP), 0);
  const bNodeLink *surfaced = fed_by_one(tree, *surface);
  ASSERT_NE(surfaced, nullptr);
  EXPECT_EQ(surfaced->fromnode, principled);
  EXPECT_EQ(surfaced->fromsock, bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr));
  /* The recorded link is back, and the Normal the graph never fed is unlinked again. */
  const bNodeLink *base = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->fromnode, image);
  EXPECT_EQ(fed_by_one(tree, *principled_input("Normal")), nullptr);
  EXPECT_EQ(ma->paint_layers_tree, nullptr);
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
  EXPECT_FALSE(BKE_paint_layers_is_layered(*ma));
}

TEST_F(PaintLayersGenerateTest, revert_restores_a_link_whose_source_node_was_renamed)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);
  bNodeSocket *base = principled_input("Base Color");
  ASSERT_NE(base, nullptr);

  BKE_paint_layers_source_graph_capture(*ma);
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The user renames the node while the stack owns the input: the record names the node by
   * identifier, so a rename does not touch it. */
  STRNCPY(image->name, "Renamed");

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  EXPECT_TRUE(report.lost_channels.is_empty());
  const bNodeLink *restored = fed_by_one(tree, *base);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->fromnode, image);
}

TEST_F(PaintLayersGenerateTest, revert_reports_a_recorded_source_that_is_gone)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);
  bNodeSocket *base = principled_input("Base Color");
  ASSERT_NE(base, nullptr);
  /* The value the graph leaves on the input: a conversion moves links, never socket values, so
   * this is what the input falls back on when its recorded source is gone. */
  const float stored[4] = {0.1f, 0.2f, 0.3f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base->default_value)->value, stored);

  BKE_paint_layers_source_graph_capture(*ma);
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The recorded source is deleted while the stack owns the input: the record can no longer name
   * it. */
  bke::node_remove_node(bmain, tree, *image, true);

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  ASSERT_EQ(report.lost_channels.size(), 1);
  EXPECT_EQ(report.lost_channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(fed_by_one(tree, *base), nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueRGBA *>(base->default_value)->value[0], 0.1f);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueRGBA *>(base->default_value)->value[1], 0.2f);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueRGBA *>(base->default_value)->value[2], 0.3f);
}

/**
 * The helpers a revert removes are the ones the generator minted, named by the created record:
 * the ownership marker alone cannot be the criterion, because system properties are copied with
 * the node -- a duplicate the user made of a helper carries the marker too, and it is theirs.
 */
TEST_F(PaintLayersGenerateTest, revert_keeps_a_duplicate_of_a_marked_helper)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(*ma, normal_and_height_set()));
  BKE_paint_layers_source_graph_capture(*ma);
  BKE_paint_layers_source_graph_set(*ma, true);
  MaterialPaintLayer *row = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_NE(row, nullptr);
  /* A set with Height only wires the Bump once a row carries a Height record. */
  add_channel(*row, PAINT_MATERIAL_CHANNEL_HEIGHT, add_image("Height"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNode *bump = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      bump = &node;
    }
  }
  ASSERT_NE(bump, nullptr);
  ASSERT_TRUE(paint_layers::node_is_generator_owned(*bump));

  /* The user duplicates the stack's Bump to build on it: the copy takes the marker along (system
   * properties are copied) and an identifier of its own. */
  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  bNode *copy = bke::node_copy_with_mapping(
      &tree, *bump, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy->identifier, bump->identifier);
  EXPECT_TRUE(paint_layers::node_is_generator_owned(*copy));

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  EXPECT_TRUE(report.lost_channels.is_empty());

  /* The minted Bump went the way of the stack; the user's duplicate stayed. */
  bool minted_found = false;
  bool copy_found = false;
  for (const bNode &node : tree.nodes) {
    minted_found = minted_found || node.identifier == bump->identifier;
    copy_found = copy_found || node.identifier == copy->identifier;
  }
  EXPECT_FALSE(minted_found);
  EXPECT_TRUE(copy_found);
  /* The graph the conversion kept is what the revert hands back. */
  const bNodeLink *base = fed_by_one(tree, *principled_input("Base Color"));
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->fromnode, image);
}

/**
 * Adoption makes a node of the user's the stack's own because the stack itself had wired it; the
 * marker it takes on does not make it the generator's to delete. The revert leaves the adopted
 * node in the tree.
 */
TEST_F(PaintLayersGenerateTest, revert_keeps_a_response_node_the_stack_adopted)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  ASSERT_NE(principled, nullptr);
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT, add_image("Height"));
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(*ma, normal_and_height_set()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The user replaces the stack's own Bump with one of theirs, wired the same way the stack wires
   * it; a structural edit makes the next pass a rebuild, which is where adoption runs. */
  bNode *bump = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      bump = &node;
    }
  }
  ASSERT_NE(bump, nullptr);
  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  bNodeSocket *result_height = paint_layers::instance_output_find(*ma->paint_layers_tree,
                                                                  *instance,
                                                                  "Result Height");
  ASSERT_NE(result_height, nullptr);
  bke::node_remove_node(bmain, tree, *bump, true);
  bNode *user_bump = bke::node_add_static_node(nullptr, tree, SH_NODE_BUMP);
  ASSERT_NE(user_bump, nullptr);
  bke::node_add_link(tree,
                     *instance,
                     *result_height,
                     *user_bump,
                     *bke::node_find_socket(*user_bump, SOCK_IN, "Height"_ustr));
  bke::node_add_link(tree,
                     *user_bump,
                     *bke::node_find_socket(*user_bump, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));
  ASSERT_NE(add_paint_layer("Paint Top", add_image("Paint Top")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Adopted: the stack wired the user's node, so the marker went onto it. */
  EXPECT_TRUE(paint_layers::node_is_generator_owned(*user_bump));

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  EXPECT_TRUE(report.lost_channels.is_empty());
  bool user_bump_found = false;
  for (const bNode &node : tree.nodes) {
    user_bump_found = user_bump_found || &node == user_bump;
  }
  EXPECT_TRUE(user_bump_found);
}

/** } */

TEST_F(PaintLayersGenerateTest, source_graph_channel_hash_reads_the_linked_channel)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The graph feeds Base Color by link: the hash names the content and exactly one feed answers,
   * on the instance's Source input. */
  EXPECT_NE(BKE_paint_layers_source_channel_graph_hash(*ma), 0);
  PaintLayersSourceChannelFeed feeds[PAINT_MATERIAL_CHANNEL_NUM];
  const int feeds_num = BKE_paint_layers_source_graph_feeds_find(
      *ma, MutableSpan<PaintLayersSourceChannelFeed>(feeds, PAINT_MATERIAL_CHANNEL_NUM));
  ASSERT_EQ(feeds_num, 1);
  EXPECT_EQ(feeds[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  EXPECT_EQ(feeds[0].socket,
            bke::node_find_socket(
                *instance, SOCK_IN, UString::from_ptr_noinline(source_iface->identifier)));
}

TEST_F(PaintLayersGenerateTest, source_graph_feeds_come_in_the_canonical_order)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);
  /* Metallic is a scalar: a Value carries it the way an author's graph would. */
  bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  ASSERT_NE(value, nullptr);
  bNodeSocket *metallic = bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr);
  ASSERT_NE(metallic, nullptr);
  bke::node_add_link(tree,
                     *value,
                     *bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr),
                     *principled,
                     *metallic);

  const uint32_t set = uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR |
                       uint32_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC;
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(*ma, set));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Both feeds answer, in the canonical channel order: Base Color before the scalars. */
  EXPECT_NE(BKE_paint_layers_source_channel_graph_hash(*ma), 0);
  PaintLayersSourceChannelFeed feeds[PAINT_MATERIAL_CHANNEL_NUM];
  const int feeds_num = BKE_paint_layers_source_graph_feeds_find(
      *ma, MutableSpan<PaintLayersSourceChannelFeed>(feeds, PAINT_MATERIAL_CHANNEL_NUM));
  ASSERT_EQ(feeds_num, 2);
  EXPECT_EQ(feeds[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(feeds[1].channel, PAINT_MATERIAL_CHANNEL_METALLIC);
}

TEST_F(PaintLayersGenerateTest, source_graph_channel_hash_skips_an_unlinked_source)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  /* A value the graph never linked: the Source input carries it as the bottom, unlinked. */
  const float grey[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, grey);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Source Base Color", false));

  /* The graph feeds nothing: the hash reads zero and no feed answers. */
  EXPECT_EQ(BKE_paint_layers_source_channel_graph_hash(*ma), 0);
  PaintLayersSourceChannelFeed feeds[PAINT_MATERIAL_CHANNEL_NUM];
  const int feeds_num = BKE_paint_layers_source_graph_feeds_find(
      *ma, MutableSpan<PaintLayersSourceChannelFeed>(feeds, PAINT_MATERIAL_CHANNEL_NUM));
  EXPECT_EQ(feeds_num, 0);
}

TEST_F(PaintLayersGenerateTest, source_graph_channel_hash_moves_when_a_link_changes)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t hash_before = BKE_paint_layers_source_channel_graph_hash(*ma);
  EXPECT_NE(hash_before, 0);

  /* The user rewires what feeds the stack's bottom: the graph the cache belongs to is a different
   * one, and the hash says so. */
  bNode *rgb = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  ASSERT_NE(rgb, nullptr);
  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  bNodeSocket *source_in = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(source_iface->identifier));
  ASSERT_NE(source_in, nullptr);
  bke::node_remove_socket_links(tree, *source_in);
  bke::node_add_link(tree,
                     *rgb,
                     *bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr),
                     *instance,
                     *source_in);

  const uint64_t hash_after = BKE_paint_layers_source_channel_graph_hash(*ma);
  EXPECT_NE(hash_after, 0);
  EXPECT_NE(hash_after, hash_before);
}

/**
 * A duplicate the user made of a marked helper carries the ownership marker along (system
 * properties are copied), but the tree's minted record does not name it. The channel-graph walk
 * hashes it like any node of theirs, so building with it on the graph side moves the hash and a
 * later edit of it moves the hash again -- the cache cannot read as fresh over a changed graph.
 */
TEST_F(PaintLayersGenerateTest, source_graph_channel_hash_counts_a_duplicate_of_a_marked_helper)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(*ma, normal_and_height_set()));
  BKE_paint_layers_source_graph_set(*ma, true);
  MaterialPaintLayer *row = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_NE(row, nullptr);
  /* A set with Height only wires the Bump once a row carries a Height record. */
  add_channel(*row, PAINT_MATERIAL_CHANNEL_HEIGHT, add_image("Height"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNode *bump = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      bump = &node;
    }
  }
  ASSERT_NE(bump, nullptr);
  ASSERT_TRUE(paint_layers::node_is_generator_owned(*bump));
  /* The walk still sees through the minted helper: it takes no part in the content. */
  const uint64_t hash_before = BKE_paint_layers_source_channel_graph_hash(*ma);
  EXPECT_NE(hash_before, 0);

  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  bNode *copy = bke::node_copy_with_mapping(
      &tree, *bump, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
  ASSERT_NE(copy, nullptr);
  EXPECT_TRUE(paint_layers::node_is_generator_owned(*copy));
  EXPECT_FALSE(paint_layers::response_node_is_created(tree, *copy));

  /* The duplicate joins the graph side: the image feeds it, it feeds the stack's bottom. */
  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *source_iface = interface_input_find(*ma->paint_layers_tree,
                                                                      "Source Base Color");
  ASSERT_NE(source_iface, nullptr);
  bNodeSocket *source_in = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(source_iface->identifier));
  ASSERT_NE(source_in, nullptr);
  bNodeSocket *copy_height = bke::node_find_socket(*copy, SOCK_IN, "Height"_ustr);
  bNodeSocket *copy_normal = bke::node_find_socket(*copy, SOCK_OUT, "Normal"_ustr);
  ASSERT_NE(copy_height, nullptr);
  ASSERT_NE(copy_normal, nullptr);
  bke::node_remove_socket_links(tree, *source_in);
  bke::node_add_link(tree,
                     *image,
                     *bke::node_find_socket(*image, SOCK_OUT, "Color"_ustr),
                     *copy,
                     *copy_height);
  bke::node_add_link(tree, *copy, *copy_normal, *instance, *source_in);

  const uint64_t hash_after = BKE_paint_layers_source_channel_graph_hash(*ma);
  EXPECT_NE(hash_after, 0);
  EXPECT_NE(hash_after, hash_before);

  /* An edit of the duplicate moves the hash again, like an edit of any node of theirs would. */
  bNodeSocket *copy_strength = bke::node_find_socket(*copy, SOCK_IN, "Strength"_ustr);
  ASSERT_NE(copy_strength, nullptr);
  static_cast<bNodeSocketValueFloat *>(copy_strength->default_value)->value = 0.5f;
  EXPECT_NE(BKE_paint_layers_source_channel_graph_hash(*ma), hash_after);
}

/** Convert a material whose graph feeds Base Color through a fresh \a type node's \a output_name:
 * the shape that puts that node on the channel-graph walk's path. The fatal failures live here as
 * expectations, so the helper can answer the question the test asks. */
static bool object_dependent_after_conversion(PaintLayersGenerateTest &fixture,
                                              const int node_type,
                                              const char *output_name)
{
  bNodeTree &tree = *fixture.ma->nodetree;
  bNode *principled = PaintLayersGenerateTest::principled_with_output_add(tree);
  bNode *reader = bke::node_add_static_node(nullptr, tree, node_type);
  EXPECT_NE(principled, nullptr);
  EXPECT_NE(reader, nullptr);
  if (principled == nullptr || reader == nullptr) {
    return false;
  }
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  bNodeSocket *from = bke::node_find_socket(
      *reader, SOCK_OUT, UString::from_ptr_noinline(output_name));
  EXPECT_NE(base_color, nullptr);
  EXPECT_NE(from, nullptr);
  if (base_color == nullptr || from == nullptr) {
    return false;
  }
  bke::node_add_link(tree, *reader, *from, *principled, *base_color);

  EXPECT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *fixture.ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*fixture.ma, true);
  EXPECT_NE(fixture.add_paint_layer("Paint", fixture.add_image("Paint")), nullptr);
  if (!BKE_paint_layers_regenerate(*fixture.bmain, *fixture.ma)) {
    return false;
  }
  return BKE_paint_layers_source_graph_object_dependent(*fixture.ma);
}

TEST_F(PaintLayersGenerateTest, source_graph_object_dependent_names_the_geometry_node)
{
  EXPECT_TRUE(object_dependent_after_conversion(*this, SH_NODE_NEW_GEOMETRY, "Position"));
}

TEST_F(PaintLayersGenerateTest, source_graph_object_dependent_names_the_texture_coordinate_node)
{
  EXPECT_TRUE(object_dependent_after_conversion(*this, SH_NODE_TEX_COORD, "UV"));
}

TEST_F(PaintLayersGenerateTest, source_graph_object_dependent_names_the_object_info_node)
{
  EXPECT_TRUE(object_dependent_after_conversion(*this, SH_NODE_OBJECT_INFO, "Color"));
}

TEST_F(PaintLayersGenerateTest, source_graph_object_dependent_names_the_camera_data_node)
{
  EXPECT_TRUE(object_dependent_after_conversion(*this, SH_NODE_CAMERA, "View Vector"));
}

TEST_F(PaintLayersGenerateTest, source_graph_object_dependent_names_the_light_path_node)
{
  EXPECT_TRUE(object_dependent_after_conversion(*this, SH_NODE_LIGHT_PATH, "Is Camera Ray"));
}

TEST_F(PaintLayersGenerateTest, source_graph_of_a_plain_image_is_not_object_dependent)
{
  bNodeTree &tree = *ma->nodetree;
  bNode *principled = principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  ASSERT_NE(image, nullptr);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_mask_set(*ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*ma, true);
  ASSERT_NE(add_paint_layer("Paint", add_image("Paint")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_FALSE(BKE_paint_layers_source_graph_object_dependent(*ma));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source-graph cache: the CPU bottom the conversion's graph is read through
 *
 * The cache of a converted material is a claim on the material (a map per fed channel and the
 * hash of the graph as baked), not a row of the stack: nothing of it belongs to the stack's
 * description, its hashes or the map writer, and every reader gates on the source-graph mode.
 * \{ */

/** Convert the fixture's material from its own graph, the shape every cache test starts from: the
 * graph feeds Base Color by link, one row owns the stack. */
static void source_graph_material_convert(PaintLayersGenerateTest &fixture)
{
  bNodeTree &tree = *fixture.ma->nodetree;
  bNode *principled = PaintLayersGenerateTest::principled_with_output_add(tree);
  bNode *image = base_color_image_add(tree, *principled);
  EXPECT_NE(image, nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *fixture.ma, uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_source_graph_set(*fixture.ma, true);
  EXPECT_NE(fixture.add_paint_layer("Paint", fixture.add_image("Paint")), nullptr);
  EXPECT_TRUE(BKE_paint_layers_regenerate(*fixture.bmain, *fixture.ma));
}

/** Stamp the cache claim a landed bake leaves: \a cache on Base Color, the live graph hash beside
 * it. The validation answers fresh afterwards. */
static void source_cache_claim_set(PaintLayersGenerateTest &fixture, Image *cache)
{
  fixture.ma->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = cache;
  fixture.ma->paint_layers_source_hash = BKE_paint_layers_source_channel_graph_hash(*fixture.ma);
}

TEST_F(PaintLayersGenerateTest, source_cache_copy_claims_nothing_of_the_original)
{
  source_graph_material_convert(*this);
  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  source_cache_claim_set(*this, cache);
  ASSERT_FALSE(BKE_paint_layers_source_cache_validate(*ma));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  /* The cache describes the original's graph: the copy starts from no claim, and its first
   * validation reads stale -- the state that plans the copy's own bake. */
  EXPECT_EQ(copy->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_EQ(copy->paint_layers_source_hash, 0);
  EXPECT_TRUE(BKE_paint_layers_source_cache_validate(*copy));
  EXPECT_TRUE(BKE_paint_layers_source_cache_stale_get(*copy));
  /* The original keeps what it had. */
  EXPECT_EQ(ma->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], cache);
  EXPECT_NE(ma->paint_layers_source_hash, 0);

  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, source_cache_revert_leaves_no_dangling_cache)
{
  source_graph_material_convert(*this);
  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  source_cache_claim_set(*this, cache);

  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));

  /* The claim went with the stack: no pointer left behind, no mark on a plain material. */
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    EXPECT_EQ(ma->paint_layers_source_images[i], nullptr);
  }
  EXPECT_EQ(ma->paint_layers_source_hash, 0);
  EXPECT_FALSE(BKE_paint_layers_source_graph_get(*ma));
  EXPECT_FALSE(BKE_paint_layers_source_cache_stale_get(*ma));

  /* The map itself was the material's own: it is gone from the file, not orphaned in it. */
  bool found = false;
  for (Image &iterated : bmain->images) {
    found |= &iterated == cache;
  }
  EXPECT_FALSE(found);
}

TEST_F(PaintLayersGenerateTest, source_cache_never_carries_normal_or_height)
{
  source_graph_material_convert(*this);
  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  source_cache_claim_set(*this, cache);

  /* The reader serves what is claimed, but Normal and Height are never the cache's to serve: the
   * author's normal chain stays on the Principled, the cache bottom is a value map only. */
  EXPECT_EQ(BKE_paint_layers_source_cache_image_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR), cache);
  EXPECT_EQ(BKE_paint_layers_source_cache_image_get(*ma, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  EXPECT_EQ(BKE_paint_layers_source_cache_image_get(*ma, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
}

TEST_F(PaintLayersGenerateTest, source_cache_sits_outside_the_rows_and_their_hashes)
{
  source_graph_material_convert(*this);
  const int rows_before = BLI_listbase_count(&ma->paint_layers);
  const Vector<int> wired = paint_layers::paint_layers_wired_channels(*ma);
  const uint64_t hash_before = paint_layers::paint_layers_root_topology_hash(*ma, wired);

  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  source_cache_claim_set(*this, cache);

  /* The cache is a claim on the material, not a row: the stack neither grows nor re-hashes for
   * it, and the map writer that packs the rows' maps leaves it out. */
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), rows_before);
  EXPECT_EQ(paint_layers::paint_layers_root_topology_hash(*ma, wired), hash_before);

  Vector<Image *> packed;
  BKE_paint_layers_maps_collect(*bmain, packed);
  EXPECT_FALSE(packed.is_empty());
  EXPECT_TRUE(std::find(packed.begin(), packed.end(), cache) == packed.end());
}

TEST_F(PaintLayersGenerateTest, source_cache_fresh_hash_is_not_rebaked)
{
  source_graph_material_convert(*this);
  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  source_cache_claim_set(*this, cache);
  const uint64_t live = ma->paint_layers_source_hash;
  ASSERT_NE(live, 0);

  /* The claim naming the live graph: nothing is behind, nothing is due. */
  EXPECT_FALSE(BKE_paint_layers_source_cache_validate(*ma));
  EXPECT_FALSE(BKE_paint_layers_source_cache_stale_get(*ma));

  /* A claim that no longer names the graph (an edit since the bake) marks the cache stale, and so
   * does a zero claim over a fed channel -- the maps were taken away under the material. */
  ma->paint_layers_source_hash = live ^ 1;
  EXPECT_TRUE(BKE_paint_layers_source_cache_validate(*ma));
  EXPECT_TRUE(BKE_paint_layers_source_cache_stale_get(*ma));
  ma->paint_layers_source_hash = 0;
  EXPECT_TRUE(BKE_paint_layers_source_cache_validate(*ma));
}

TEST_F(PaintLayersGenerateTest, a_material_without_source_graph_reads_no_cache)
{
  add_paint_layer("Paint", add_image("Paint"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_is_layered(*ma));
  ASSERT_FALSE(BKE_paint_layers_source_graph_get(*ma));

  /* Even a stray claim on the array stays invisible: the mode's flag is the gate every reader
   * asks first. */
  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  ma->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = cache;
  ma->paint_layers_source_hash = 12345;

  EXPECT_EQ(BKE_paint_layers_source_cache_image_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  EXPECT_FALSE(BKE_paint_layers_source_cache_validate(*ma));
  EXPECT_FALSE(BKE_paint_layers_source_cache_stale_get(*ma));

  /* The composite gathers the rows alone: no cache layer is described for it. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  for (const PaintMaterialCompositeImageLayer &layer : layers) {
    EXPECT_FALSE(layer.is_source_cache);
  }
}

TEST_F(PaintLayersGenerateTest, source_cache_pixels_lost_on_a_load_read_constant_and_stale)
{
  source_graph_material_convert(*this);
  /* A generated, unpacked map: exactly what a load hands back, the cache being deliberately not
   * packed. */
  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  source_cache_claim_set(*this, cache);
  ASSERT_FALSE(BKE_paint_layers_source_cache_validate(*ma));

  /* The load-time validation: the pixels cannot come back but a re-bake, so the claim goes with
   * the empty image and the mark turns on. */
  EXPECT_TRUE(BKE_paint_layers_source_cache_validate(*ma, /*check_unpacked=*/true));
  EXPECT_TRUE(BKE_paint_layers_source_cache_stale_get(*ma));
  EXPECT_EQ(ma->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_EQ(ma->paint_layers_source_hash, 0);

  /* No cache image answers the reader: the CPU serves the channel's constant. */
  EXPECT_EQ(BKE_paint_layers_source_cache_image_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
}

TEST_F(PaintLayersGenerateTest, source_cache_land_apply_stamps_only_a_full_landing)
{
  source_graph_material_convert(*this);
  Image *cache = add_image("Cache");
  Image *metallic_cache = add_image("Cache Metallic");
  ASSERT_NE(cache, nullptr);
  ASSERT_NE(metallic_cache, nullptr);
  const uint64_t live = BKE_paint_layers_source_channel_graph_hash(*ma);
  ASSERT_NE(live, 0);

  /* A bake asked for two channels and one rendered: the claim does not stamp, so the partial
   * answer cannot read as fresh over the channel whose pixels never arrived. */
  const eMaterialPaintChannel requested[2] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_METALLIC};
  const PaintLayersSourceCacheLandedChannel landed_one[] = {
      {PAINT_MATERIAL_CHANNEL_BASE_COLOR, cache}};
  EXPECT_EQ(BKE_paint_layers_source_cache_land_apply(*ma,
                                                     live,
                                                     Span<eMaterialPaintChannel>(requested, 2),
                                                     Span<PaintLayersSourceCacheLandedChannel>(
                                                         landed_one, 1)),
            PaintLayersSourceCacheLandState::Partial);
  EXPECT_EQ(ma->paint_layers_source_hash, 0);
  EXPECT_EQ(ma->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], cache);
  EXPECT_EQ(ma->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_METALLIC], nullptr);
  EXPECT_TRUE(BKE_paint_layers_source_cache_stale_get(*ma));

  /* Every requested channel landed, at the graph as it is now: the claim stamps and reads fresh.
   * The map that was already claimed keeps its one user. */
  const PaintLayersSourceCacheLandedChannel landed_both[] = {
      {PAINT_MATERIAL_CHANNEL_BASE_COLOR, cache},
      {PAINT_MATERIAL_CHANNEL_METALLIC, metallic_cache}};
  EXPECT_EQ(BKE_paint_layers_source_cache_land_apply(*ma,
                                                     live,
                                                     Span<eMaterialPaintChannel>(requested, 2),
                                                     Span<PaintLayersSourceCacheLandedChannel>(
                                                         landed_both, 2)),
            PaintLayersSourceCacheLandState::Fresh);
  EXPECT_EQ(ma->paint_layers_source_hash, live);
  EXPECT_EQ(ma->paint_layers_source_images[PAINT_MATERIAL_CHANNEL_METALLIC], metallic_cache);
  EXPECT_FALSE(BKE_paint_layers_source_cache_stale_get(*ma));

  /* A full landing of a graph that moved while the bake ran: the claim stamps the bake's own
   * hash, and the validation reads stale for the newer graph. */
  EXPECT_EQ(BKE_paint_layers_source_cache_land_apply(*ma,
                                                     live ^ 1,
                                                     Span<eMaterialPaintChannel>(requested, 2),
                                                     Span<PaintLayersSourceCacheLandedChannel>(
                                                         landed_both, 2)),
            PaintLayersSourceCacheLandState::Stale);
  EXPECT_EQ(ma->paint_layers_source_hash, live ^ 1);
  EXPECT_TRUE(BKE_paint_layers_source_cache_stale_get(*ma));
}

TEST_F(PaintLayersGenerateTest, source_cache_bake_due_gates_on_state_and_memory)
{
  source_graph_material_convert(*this);
  /* The stale mark is the editor material update's to set; in a BKE-only test the re-derivation
   * marks it from the missing claim. */
  ASSERT_TRUE(BKE_paint_layers_source_cache_validate(*ma));
  /* A stale cache over a graph that feeds a channel: due. */
  EXPECT_TRUE(BKE_paint_layers_source_cache_bake_due(*ma, /*manual=*/false));

  /* A graph the bake already failed to land: the auto path leaves it alone, the manual run is the
   * way past it. */
  const uint64_t live = BKE_paint_layers_source_channel_graph_hash(*ma);
  BKE_paint_layers_source_cache_failure_remember(*ma, live);
  EXPECT_TRUE(BKE_paint_layers_source_cache_failure_blocks(*ma));
  EXPECT_FALSE(BKE_paint_layers_source_cache_bake_due(*ma, /*manual=*/false));
  EXPECT_TRUE(BKE_paint_layers_source_cache_bake_due(*ma, /*manual=*/true));

  /* The graph moving on makes the memory moot: the failure named a hash that no longer is. */
  BKE_paint_layers_source_cache_failure_remember(*ma, live ^ 1);
  EXPECT_FALSE(BKE_paint_layers_source_cache_failure_blocks(*ma));
  EXPECT_TRUE(BKE_paint_layers_source_cache_bake_due(*ma, /*manual=*/false));

  /* A fresh cache is not due for anybody. */
  Image *cache = add_image("Cache");
  ASSERT_NE(cache, nullptr);
  source_cache_claim_set(*this, cache);
  ASSERT_FALSE(BKE_paint_layers_source_cache_validate(*ma));
  EXPECT_FALSE(BKE_paint_layers_source_cache_bake_due(*ma, /*manual=*/false));
  EXPECT_FALSE(BKE_paint_layers_source_cache_bake_due(*ma, /*manual=*/true));
}

/** \} */

/** \name Conversion fill: the row a conversion's bake fills again
 * \{ */

/** The channel record of \a layer, or null: the tests read the record state and map directly. */
static const MaterialPaintLayerChannel *conversion_record_find(
    const MaterialPaintLayer &layer, const eMaterialPaintChannel channel)
{
  for (int i = 0; i < layer.channels_num; i++) {
    if (layer.channels[i].channel == int8_t(channel)) {
      return &layer.channels[i];
    }
  }
  return nullptr;
}

/** The Fill a conversion's bake adds, marked the way the operator marks it. */
static MaterialPaintLayer *conversion_fill_add(PaintLayersGenerateTest &test,
                                               const uint32_t channels)
{
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  params.empty = true;
  params.channels = channels;
  params.name = test.ma->id.name + 2;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*test.ma, params);
  if (fill != nullptr) {
    fill->flag |= MA_PAINT_LAYER_CONVERSION_FILL;
  }
  return fill;
}

TEST_F(PaintLayersGenerateTest, conversion_fill_find_ignores_a_user_made_fill)
{
  /* The user's own bottom Fill: the same kind the conversion makes, but never marked. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  params.empty = true;
  params.name = "User Fill";
  MaterialPaintLayer *user_fill = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(user_fill, nullptr);
  EXPECT_EQ(user_fill->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  Image *user_map = add_image("User Map");
  add_channel(*user_fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, user_map);

  /* The conversion marks its own on top of it. */
  MaterialPaintLayer *converted = conversion_fill_add(*this, 0);
  ASSERT_NE(converted, nullptr);
  add_channel(*converted, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("Baked Roughness"));

  /* The bottom row is the user's: nothing to reuse, so a re-bake stacks its own Fill instead of
   * touching theirs, and their map stays where it was. */
  EXPECT_EQ(BKE_paint_layers_conversion_fill_find(*ma), nullptr);
  const MaterialPaintLayerChannel *record = conversion_record_find(
      *user_fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(record->image, user_map);

  /* The user's row gone, the marked one answers: this is what a re-bake over a stack with no user
   * fill at the bottom sees. */
  EXPECT_TRUE(BKE_paint_layers_remove(*ma, user_fill));
  EXPECT_EQ(BKE_paint_layers_conversion_fill_find(*ma), converted);
}

TEST_F(PaintLayersGenerateTest, conversion_fill_reuse_survives_revert_and_copy)
{
  /* The conversion's fill on an empty stack: bottom, marked, carrying a map the bake made. */
  MaterialPaintLayer *fill = conversion_fill_add(*this,
                                                 uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(fill, nullptr);
  add_channel(*fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Baked"));
  ASSERT_EQ(BKE_paint_layers_conversion_fill_find(*ma), fill);

  /* A Revert keeps the rows: the next bake puts its maps back into the same one. */
  PaintLayersRevertReport report;
  ASSERT_TRUE(BKE_paint_layers_revert_to_plain(*bmain, *ma, &report));
  EXPECT_EQ(BKE_paint_layers_conversion_fill_find(*ma), fill);

  /* A copy of the material carries the marker with the row: a re-bake over the copy fills the
   * copy's own row and never the original's. (The file load reads the same field back, so this
   * stands in for it here.) */
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  MaterialPaintLayer *copied_fill = BKE_paint_layers_conversion_fill_find(*copy);
  ASSERT_NE(copied_fill, nullptr);
  EXPECT_NE(copied_fill, fill);
  EXPECT_EQ(BKE_paint_layers_conversion_fill_find(*ma), fill);
}

TEST_F(PaintLayersGenerateTest, conversion_fill_align_switches_channels_without_dropping_maps)
{
  /* The conversion's fill carrying two baked maps. */
  MaterialPaintLayer *fill = conversion_fill_add(*this, 0);
  ASSERT_NE(fill, nullptr);
  Image *base_map = add_image("Baked Base");
  Image *rough_map = add_image("Baked Roughness");
  add_channel(*fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_map);
  add_channel(*fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_map);

  /* A re-bake over {Base Color, Metallic}: Roughness switches off with its map kept, Metallic
   * arrives without one, Base Color stays live. */
  const uint32_t rebaked = uint32_t(uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR |
                                    uint32_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC);
  BKE_paint_layers_conversion_fill_channels_align(*ma, *fill, rebaked);

  const MaterialPaintLayerChannel *rough = conversion_record_find(
      *fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  EXPECT_EQ(rough->state, MA_PAINT_LAYER_CHANNEL_DISABLED);
  EXPECT_EQ(rough->image, rough_map);
  const MaterialPaintLayerChannel *base = conversion_record_find(
      *fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(base->image, base_map);
  const MaterialPaintLayerChannel *metal = conversion_record_find(
      *fill, PAINT_MATERIAL_CHANNEL_METALLIC);
  ASSERT_NE(metal, nullptr);
  EXPECT_EQ(metal->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(metal->image, nullptr);

  /* The next bake asks for Roughness again: the record comes back live on its own old map, and
   * the channel the last bake left out sits disabled without dropping whatever it holds. */
  const uint32_t again = uint32_t(uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR |
                                  uint32_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  BKE_paint_layers_conversion_fill_channels_align(*ma, *fill, again);
  rough = conversion_record_find(*fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  EXPECT_EQ(rough->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(rough->image, rough_map);
  const MaterialPaintLayerChannel *off_metal = conversion_record_find(
      *fill, PAINT_MATERIAL_CHANNEL_METALLIC);
  ASSERT_NE(off_metal, nullptr);
  EXPECT_EQ(off_metal->state, MA_PAINT_LAYER_CHANNEL_DISABLED);
  EXPECT_EQ(off_metal->image, nullptr);
}

/** \} */

}  // namespace blender::bke::tests
