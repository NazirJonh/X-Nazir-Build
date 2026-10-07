/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers generator tests: source-group wrappers, sync and visibility (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_generate_test_fixture.hh` /
 * `paint_layers_generate_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_generate_test_fixture.hh"
#include "intern/paint_layers_generate_test_shared.hh"

namespace blender::bke::tests {

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
  /* The source carries Specular, Alpha and Emission beyond the build default set. */
  extend_channel_set(channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION));
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
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 6); /* +warm: a Warm Mask map per channel. */
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
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 2); /* +warm: Warm Mask map. */
}

TEST_F(PaintLayersGenerateTest,
       source_group_mode_change_rebuilds_the_root_when_channels_are_untracked)
{
  /* F2-C4a gives every tracked map channel a content-alpha output, so a Material row whose channel
   * set includes one changes the row group's interface when it moves between Hybrid and SourceGroup
   * and the root is rebuilt (see the tracked-channel test below). This guards the other half: a row
   * whose only channel is untracked builds no content alpha in either mode. Its Hybrid live-constant
   * value input still disappears in SourceGroup, so the root mirror set changes and the root rebuilds
   * (A1). */
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
   * SourceGroup. The Hybrid live-constant value input disappears, so the root mirror set changes and
   * the root rebuilds (A1); the row group rebuilds too. */
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
  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
}

TEST_F(PaintLayersGenerateTest,
       source_group_mode_change_rebuilds_the_root_on_tracked_channels)
{
  /* Base Color is a tracked channel, but a Material row's transparency is its Alpha input, so the
   * row never exports a "Content Alpha Base Color" output in either mode. Constant -> Noise
   * therefore moves the row to SourceGroup without moving that part of the group's interface. The
   * Hybrid live-constant value input still disappears, so the root rebuilds (A1); the row group
   * rebuilds too. The complementary guard to
   * source_group_mode_change_rebuilds_the_root_when_channels_are_untracked. */
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

  /* Constant -> Noise moves the row to SourceGroup: its group rebuilds, and the root too (A1). */
  source_set_noise_base_color(*bmain, *source);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_EQ(layer_tree_find(*bmain, "Source"), group);
  EXPECT_FALSE(group_io_sentinel_get(*group, 0.125f));
  EXPECT_FALSE(interface_has_socket(*group, "Content Alpha Base Color", true));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));

  /* The rebuilt root is internally sound: every link still touches sockets its own endpoints own,
   * and every Compose Color Alpha that survived is fed (the Base Color Paint row below still tracks
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
  /* AUTO bakes only folders nested in another folder (level 2), so wrap it. */
  ASSERT_NE(group_one(*ma, folder), nullptr);

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
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* AUTO + heavy. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_AUTO));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, PAINT_LAYERS_HEAVY_BAKE_SIZE));
  changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
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

  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*row), nullptr);
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
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*row), nullptr);

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

  /* The value is written to the root instance mirror (A1). */
  bNodeSocket *socket = root_instance_input("Bottom Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  socket = root_instance_input("Bottom Base Color Opacity");
  ASSERT_NE(socket, nullptr);
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

/** Nothing changed between two regenerations: nothing is rebuilt (spec §8 counters). */
TEST_F(PaintLayersGenerateTest, regenerate_reports_no_rebuild_when_nothing_changed)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  PaintLayersRegenerateReport first;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &first));
  EXPECT_TRUE(first.root_rebuilt);
  EXPECT_GE(first.layer_groups_rebuilt, 1);

  PaintLayersRegenerateReport second;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &second));
  EXPECT_FALSE(second.root_rebuilt);
  EXPECT_EQ(second.layer_groups_rebuilt, 0);
}

/** A row's own visibility is not in its bake hash, so its own bake is no reason to rebuild. */
TEST_F(PaintLayersGenerateTest, own_visibility_of_a_baked_layer_does_not_tag_a_rebuild)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*layer), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

/**
 * A child's visibility is part of its baked parent's hash, so it must rebuild. Only a valid bake
 * substitutes the folder's nodes; REGEN is tagged when the substitution state changes
 * (#paint_layer_ancestor_bake_state), so the fixture needs a finalized bake.
 */
TEST_F(PaintLayersGenerateTest, child_visibility_under_a_baked_folder_tags_a_rebuild)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = add_paint_layer_into(folder, "Child", add_image("Child"));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*folder), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *folder);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, child, false));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

/**
 * A stale (never finalized) folder bake substitutes nothing: the child's nodes are live, so
 * hiding it is a multiplier edit and the substitution state does not change, hence no REGEN.
 */
TEST_F(PaintLayersGenerateTest, child_visibility_under_a_stale_baked_folder_keeps_the_graph)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = add_paint_layer_into(folder, "Child", add_image("Child"));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*folder), nullptr);
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, child, false));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

/**
 * The active row stays in the graph while hidden, so an unrelated rebuild does not drop it and
 * showing it again is a value edit (the user is only checking how the result looks).
 */
TEST_F(PaintLayersGenerateTest, active_hidden_row_survives_an_incidental_rebuild)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  BKE_paint_layers_active_set(*ma, bottom->marker);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, bottom, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  add_paint_layer("Extra", add_image("Extra"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Bottom"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, bottom, true));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.root_rebuilt);
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

  /* Once the row has been hidden past the cold tier, an unrelated topology change is the chance to
   * drop it. Under the tier it would stay (the cold-tier tests cover the age). */
  age_cold_mark(bottom);
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
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*folder), nullptr);

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
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*paint), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *paint);
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*folder), nullptr);
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
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*mat), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *mat);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *mat));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, mat, false));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *mat));
}

/**
 * Adding a correction to one row rebuilds only that row's group; the root, a sibling and a source
 * wrapper stay untouched (their contracts do not change).
 */
TEST_F(PaintLayersGenerateTest, adding_a_correction_rebuilds_its_row_and_the_root)
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

  /* The effect adds a value input, so the root mirror set changes and the root rebuilds (A1); the
   * other rows and the source wrapper are untouched. */
  EXPECT_EQ(ma->paint_layers_tree, root);
  /* +warm: the real effect takes the spare's slot, so the root is unchanged. */
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

  /* The value is mirrored onto the root instance; the row group's socket is fed from it (A1). */
  bNodeSocket *socket = root_instance_input("MatRow C Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  const float before = static_cast<bNodeSocketValueFloat *>(socket->default_value)->value;

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.42f));
  BKE_paint_layers_values_sync(*ma);
  socket = root_instance_input("MatRow C Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.42f);
  EXPECT_NE(before, 0.42f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, correction, false));
  BKE_paint_layers_values_sync(*ma);
  socket = root_instance_input("MatRow C Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);
}

/** The first Image Texture node of \a tree (descending nested layer groups) whose id is \a image. */
static bNode *find_tex_image_of(bNodeTree &tree, const Image &image)
{
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &image.id) {
      return &node;
    }
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        !BKE_paint_material_is_normal_combine_group(node))
    {
      if (bNode *found = find_tex_image_of(*reinterpret_cast<bNodeTree *>(node.id), image)) {
        return found;
      }
    }
  }
  return nullptr;
}

/**
 * An Effect correction with source Material behaves exactly like a Layer row of that source: in
 * Baked mode (no Principled on the source, so #BKE_paint_layers_material_mode answers Baked) it
 * reads its own external bake through the same #paint_layer_channel_image resolver a Layer row's
 * Baked mode uses.
 */
TEST_F(PaintLayersGenerateTest, effect_material_baked_reads_the_bake_map)
{
  Material *source = BKE_material_add(bmain, "EffectBakedSource");
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "MC");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  Image *bake_map = add_image("EffectBakedMap");
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, bake_map));
  BKE_paint_layers_bake_finalize(*ma, *correction);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::Baked);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  EXPECT_NE(find_tex_image_of(*group, *bake_map), nullptr);
}

/**
 * An Effect correction with source Material in SourceGroup mode (its source reads a Noise graph
 * the CPU cannot reproduce) goes through the same `.PL Source` wrapper a Layer row of that source
 * uses, not a baked Image Texture.
 */
TEST_F(PaintLayersGenerateTest, effect_material_source_group_uses_the_wrapper)
{
  Material *source = add_principled_source("EffectSgSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "MC");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction),
            PaintLayerMaterialMode::SourceGroup);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  /* Only the owner row's own map is an Image Texture; the correction's whole source graph goes
   * through the wrapper instance instead. */
  /* +warm: Warm Mask and Warm Effect maps of the owner. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 3);
  bool wrapper_found = false;
  for (bNode &node : group->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        !BKE_paint_material_is_normal_combine_group(node) &&
        StringRef(node.id->name + 2).startswith(".PL Source"))
    {
      wrapper_found = true;
      break;
    }
  }
  EXPECT_TRUE(wrapper_found);
}

/**
 * A Material correction in SourceGroup mode is transparent through its source's Alpha input, like
 * a Layer row of that source (`layer_factor`, ~2276-2356): its own coverage/mix factor is wired to
 * the wrapper's dedicated `Coverage` output, never a flat opacity and never the content channel's
 * own map alpha (the correction has no content map in SourceGroup mode to begin with).
 */
TEST_F(PaintLayersGenerateTest, effect_material_source_group_factor_uses_the_wrapper_coverage)
{
  Material *source = add_principled_source("EffectSgCoverageSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.5f;

  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "MC");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction),
            PaintLayerMaterialMode::SourceGroup);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);

  bNode *wrapper_instance = nullptr;
  for (bNode &node : group->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        !BKE_paint_material_is_normal_combine_group(node) &&
        StringRef(node.id->name + 2).startswith(".PL Source"))
    {
      wrapper_instance = &node;
      break;
    }
  }
  ASSERT_NE(wrapper_instance, nullptr);
  bNodeTree *wrapper_tree = reinterpret_cast<bNodeTree *>(wrapper_instance->id);
  ASSERT_NE(wrapper_tree, nullptr);
  bNodeSocket *coverage_out = nullptr;
  for (bNodeSocket *s : wrapper_instance->output_sockets()) {
    if (s->name != nullptr && STREQ(s->name, "Coverage")) {
      coverage_out = s;
      break;
    }
  }
  ASSERT_NE(coverage_out, nullptr);
  /* The wrapper's Coverage output must feed *something* in the correction's factor chain -- the
   * generator wires it into the Multiply that scales the correction's opacity, exactly like
   * `layer_factor` does for the row's own COVERAGE query at the same output. */
  EXPECT_FALSE(coverage_out->directly_linked_links().is_empty());
}

/**
 * An Effect correction with source Node Group is Baked-only, like a Layer row of that source: with
 * no valid bake yet it takes no part at all (mirrors the Layer row's own
 * #custom_bake_missing_warn_once skip).
 */
TEST_F(PaintLayersGenerateTest, effect_node_group_without_bake_does_not_participate)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  const int nodes_before = node_count(*group);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "NGC");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  /* No bake yet: the correction contributes no node at all. */
  EXPECT_EQ(node_count(*group), nodes_before);
  /* +warm: Warm Mask and Warm Effect maps of the owner. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 3);
}

/**
 * The same Node Group correction, once it has a valid external bake, reads that bake through an
 * Image Texture node -- the same shape a baked Custom Layer row's own map takes.
 */
TEST_F(PaintLayersGenerateTest, effect_node_group_with_bake_reads_the_bake_image)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "NGC");
  ASSERT_NE(correction, nullptr);
  Image *bake_map = add_image("NodeGroupBakeMap");
  Image *coverage = add_image("NodeGroupBakeCoverage");
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, bake_map));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *correction, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *correction);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  EXPECT_NE(find_tex_image_of(*group, *bake_map), nullptr);
}

/**
 * TZ-26 for a correction: editing the live source's own value is not topology, so it must sync
 * through the group input in place, exactly like a Layer row's live constant
 * (#live_material_constant_edit_syncs_without_rebuild).
 */
TEST_F(PaintLayersGenerateTest, effect_material_live_constant_edit_syncs_without_rebuild)
{
  Material *source = add_principled_source("EffectLiveSource", 0.1f);
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "MC");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);

  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value[0] = 0.9f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Neither the row's group nor the root was rebuilt. */
  EXPECT_EQ(layer_tree_find(*bmain, "Owner"), group);
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));

  /* The live constant is mirrored onto the root instance (A1). */
  bNodeSocket *socket = root_instance_input("Owner MC Base Color Source");
  ASSERT_NE(socket, nullptr);
  const float *value = static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value;
  EXPECT_NEAR(value[0], 0.9f, 1e-4f);
}

/**
 * Switching a correction's own source (Image to Material) is topology and must rebuild its
 * owner's group, matching #BKE_paint_layers_correction_source_set now allowing the change.
 */
TEST_F(PaintLayersGenerateTest, effect_correction_source_change_to_material_rebuilds_the_group)
{
  Material *source = BKE_material_add(bmain, "EffectSourceChangeMat");
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "MC");
  ASSERT_NE(correction, nullptr);
  BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("CorrectionImage"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> nodes_before = root_nodes(*group);

  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, correction, MA_PAINT_LAYER_SOURCE_MATERIAL));
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  /* The group's own node list changed shape: the Image Texture correction node is gone. */
  EXPECT_FALSE(same_nodes(nodes_before, root_nodes(*group)));
}

/**
 * Changing a Mask Item's `mask_channel` is topology (a different channel reads a different
 * socket and reduces it a different way -- RGBTOBW, Separate X, or the coverage itself), so it
 * must rebuild the owner's group; a value edit on the source, meanwhile, must not.
 */
TEST_F(PaintLayersGenerateTest, mask_channel_change_rebuilds_the_group)
{
  Material *source = BKE_material_add(bmain, "MaskChannelChangeSource");
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> nodes_before = root_nodes(*group);

  ASSERT_TRUE(mask->mask_channel != int8_t(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  /* Base Color is a colour channel (RGBTOBW) and Roughness a scalar one (Separate X): the node
   * shape must differ. */
  EXPECT_FALSE(same_nodes(nodes_before, root_nodes(*group)));
}

/**
 * A Material mask's own coverage-source value edit, by contrast, is not topology (Spec-26): editing
 * the source's Roughness with `mask_channel` unchanged must sync in place, exactly like a Layer
 * row's or an Effect correction's own live constant.
 */
TEST_F(PaintLayersGenerateTest, mask_material_live_constant_edit_syncs_without_rebuild)
{
  Material *source = add_principled_source("MaskLiveEditSource", 0.1f);
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);

  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.9f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(layer_tree_find(*bmain, "Owner"), group);
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
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
  const uint64_t root_signature = code_shape_signature(*row_group);
  /* The row's map plus the Warm Mask and Warm Effect maps. */
  EXPECT_EQ(count_type(*row_group, SH_NODE_TEX_IMAGE), 3);

  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, folder, PaintLayerPlace::Into));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *folder_group = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_group, nullptr);
  EXPECT_EQ(layer_tree_find(*bmain, "Row"), row_group);
  /* Nested, the row loses its warm chains: its group rebuilds without them. */
  EXPECT_EQ(count_type(*row_group, SH_NODE_TEX_IMAGE), 1);
  EXPECT_NE(code_shape_signature(*row_group), root_signature);

  ASSERT_TRUE(BKE_paint_layers_move(*ma, row, folder, PaintLayerPlace::Above));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Row"), row_group);
  /* Back at the root the row gets its warm chains again: the shape is what it was. */
  EXPECT_EQ(count_type(*row_group, SH_NODE_TEX_IMAGE), 3);
  EXPECT_EQ(code_shape_signature(*row_group), root_signature);
}

/* -------------------------------------------------------------------- */

}  // namespace blender::bke::tests
