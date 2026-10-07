/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: normal channel and source groups (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


/** The `.PL Source <name>` wrapper group in \a bmain, or null. */
static bNodeTree *wrapper_tree_find(Main &bmain, const char *source_name)
{
  char full[MAX_ID_NAME - 2];
  SNPRINTF(full, ".PL Source %s", source_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, full)) {
      return &tree;
    }
  }
  return nullptr;
}

/**
 * B3 (a): a Material row in SourceGroup whose Normal comes from a computed source (not a Normal
 * Map) must show the same encoded [0,1] normal the bake would store: the wrapper encodes the
 * decoded vector through `0.5 * n + 0.5`. Before the fix the wrapper linked the decoded vector
 * itself, so the chain decoded it a second time and the result was wrong. The interpreter has no
 * Bump case, so the computed normal stands in for it.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_normal_computed_is_encoded)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  const float raw[3] = {2.0f, 1.0f, 0.5f};

  ma = BKE_material_add(bmain, "SGNormalComputed");
  Image *bottom_map = add_solid_image("SGNormalComputedBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  bNode *principled = nullptr;
  Material *source = make_wrapper_forced_source(*bmain, "SGNormalComputedSource", &principled);
  bNode *normal = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_VECTOR_MATH);
  ASSERT_NE(normal, nullptr);
  normal->custom1 = NODE_VECTOR_MATH_NORMALIZE;
  bNodeSocket *normal_in = bke::node_find_socket(*normal, SOCK_IN, "Vector"_ustr);
  ASSERT_NE(normal_in, nullptr);
  copy_v3_v3(static_cast<bNodeSocketValueVector *>(normal_in->default_value)->value, raw);
  bke::node_add_link(*source->nodetree,
                     *normal,
                     *bke::node_find_socket(*normal, SOCK_OUT, "Vector"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA bottom_enc = interpreter.sample_image(bottom_map, "Color");
  float detail[3] = {raw[0], raw[1], raw[2]};
  normalize_v3(detail);

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    float expected[3];
    normal_result_reference(bottom_enc, detail, 1.0f, expected);
    EXPECT_NEAR(graph.r, expected[0], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected[1], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected[2], tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * B3 (a follow-up): the same encoding for a computed normal that reaches the Principled through an
 * RGB node. The source itself carries the constant, so `detail` is that colour.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_normal_rgb_source_is_encoded)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;

  ma = BKE_material_add(bmain, "SGNormalRgb");
  Image *bottom_map = add_solid_image("SGNormalRgbBottom", size, 100, 160, 220, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  bNode *principled = nullptr;
  Material *source = make_wrapper_forced_source(*bmain, "SGNormalRgbSource", &principled);
  bNode *rgb = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_RGB);
  ASSERT_NE(rgb, nullptr);
  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  ASSERT_NE(rgb_out, nullptr);
  const float color[4] = {0.8f, 0.3f, 0.6f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value, color);
  bke::node_add_link(*source->nodetree,
                     *rgb,
                     *rgb_out,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA bottom_enc = interpreter.sample_image(bottom_map, "Color");
  const float detail[3] = {color[0], color[1], color[2]};

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    float expected[3];
    normal_result_reference(bottom_enc, detail, 1.0f, expected);
    EXPECT_NEAR(graph.r, expected[0], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected[1], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected[2], tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * B3 (c): a Normal Map over a live texture inside a SourceGroup row must expose the Normal Map's
 * Color input -- the encoded map -- without encoding it again. Decoding it gives the source normal
 * the resolver sees; a second encode would shift it.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_normal_map_color_is_not_reencoded)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;

  ma = BKE_material_add(bmain, "SGNormalMap");
  Image *bottom_map = add_solid_image("SGNormalMapBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  Image *source_normal = add_solid_image("SGNormalMapTexture", size, 255, 128, 128, 255);
  make_image_data(source_normal);

  bNode *principled = nullptr;
  Material *source = make_wrapper_forced_source(*bmain, "SGNormalMapSource", &principled);
  bNodeTree &ntree = *source->nodetree;
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(texture, nullptr);
  texture->id = &source_normal->id;
  id_us_plus(&source_normal->id);
  static_cast<NodeTexImage *>(texture->storage)->projection = SHD_PROJ_FLAT;
  bNode *normal_map = bke::node_add_static_node(nullptr, ntree, SH_NODE_NORMAL_MAP);
  ASSERT_NE(normal_map, nullptr);
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *normal_map,
                     *bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr));
  bke::node_add_link(ntree,
                     *normal_map,
                     *bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA bottom_enc = interpreter.sample_image(bottom_map, "Color");
  const RGBA row_enc = interpreter.sample_image(source_normal, "Color");
  /* The map is already encoded; the chain decodes it directly. */
  const float detail[3] = {row_enc.r * 2.0f - 1.0f,
                           row_enc.g * 2.0f - 1.0f,
                           row_enc.b * 2.0f - 1.0f};

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    float expected[3];
    normal_result_reference(bottom_enc, detail, 1.0f, expected);
    EXPECT_NEAR(graph.r, expected[0], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected[1], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected[2], tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * B3 (f) guard: a value edit to the source normal keeps the wrapper's ID and interface (values are
 * synced in place, no rebuild), and the row reflects the new value.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_normal_value_edit_keeps_the_interface)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  const float raw_a[3] = {2.0f, 1.0f, 0.5f};
  const float raw_b[3] = {0.2f, 3.0f, 1.5f};

  ma = BKE_material_add(bmain, "SGNormalValue");
  Image *bottom_map = add_solid_image("SGNormalValueBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  bNode *principled = nullptr;
  Material *source = make_wrapper_forced_source(*bmain, "SGNormalValueSource", &principled);
  bNode *normal = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_VECTOR_MATH);
  ASSERT_NE(normal, nullptr);
  normal->custom1 = NODE_VECTOR_MATH_NORMALIZE;
  bNodeSocket *normal_in = bke::node_find_socket(*normal, SOCK_IN, "Vector"_ustr);
  ASSERT_NE(normal_in, nullptr);
  copy_v3_v3(static_cast<bNodeSocketValueVector *>(normal_in->default_value)->value, raw_a);
  bke::node_add_link(*source->nodetree,
                     *normal,
                     *bke::node_find_socket(*normal, SOCK_OUT, "Vector"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  bNodeTree *wrapper_before = wrapper_tree_find(*bmain, "SGNormalValueSource");
  ASSERT_NE(wrapper_before, nullptr);
  wrapper_before->ensure_interface_cache();
  Vector<std::string> ids_before;
  for (bNodeTreeInterfaceSocket *iface : wrapper_before->interface_outputs()) {
    ids_before.append(iface->identifier != nullptr ? iface->identifier : "");
  }

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA before = interpreter.eval_result(result_name(channel));

  /* A value-only edit: the topology hash does not move, so the wrapper is synced, not rebuilt. */
  copy_v3_v3(static_cast<bNodeSocketValueVector *>(normal_in->default_value)->value, raw_b);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *wrapper_after = wrapper_tree_find(*bmain, "SGNormalValueSource");
  ASSERT_NE(wrapper_after, nullptr);
  EXPECT_EQ(wrapper_after, wrapper_before) << "a value edit must not rebuild the wrapper";
  wrapper_after->ensure_interface_cache();
  Vector<std::string> ids_after;
  for (bNodeTreeInterfaceSocket *iface : wrapper_after->interface_outputs()) {
    ids_after.append(iface->identifier != nullptr ? iface->identifier : "");
  }
  ASSERT_EQ(ids_after.size(), ids_before.size());
  for (const int64_t i : ids_after.index_range()) {
    EXPECT_EQ(ids_after[i], ids_before[i]);
  }

  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA after = interpreter.eval_result(result_name(channel));
  EXPECT_GT(fabsf(after.r - before.r) + fabsf(after.g - before.g) + fabsf(after.b - before.b),
            1e-3f)
      << "a value edit to the source normal must reach the row";

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The wrapper copy must keep the source group's interface: the root's RGB and Value reach the group
 * inputs through Reroutes, Metallic is an instance value. If the copy's Group Input lost its
 * sockets, the Principled would fall back to its defaults and this would read 0.8 / 0.5 / 0.0.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_live_wired_reads_the_source_values)
{
  ma = BKE_material_add(bmain, "WiredLive");
  Material *source = make_interface_wired_source(*bmain, "WiredLiveSource");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC})
  {
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, channel), nullptr);
  }
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  ASSERT_NE(interpreter.tree, nullptr);

  /* The wired source values, not the Principled defaults. */
  const RGBA base = interpreter.eval_result("Result Base Color");
  EXPECT_NEAR(base.r, 0.2f, 1e-4f);
  EXPECT_NEAR(base.g, 0.5f, 1e-4f);
  EXPECT_NEAR(base.b, 0.9f, 1e-4f);
  const RGBA rough = interpreter.eval_result("Result Roughness");
  EXPECT_NEAR(rough.r, 0.3f, 1e-4f);
  const RGBA metal = interpreter.eval_result("Result Metallic");
  EXPECT_NEAR(metal.r, 0.7f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/* -------------------------------------------------------------------- */
/** \name TZ-15: SourceGroup vs Hybrid/Baked parity for a Material row

/** Print the node chain feeding \a socket, so a divergence can be read off the graph. */
static void dump_socket_chain(const bNodeSocket &socket, const int depth)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (link->fromnode == nullptr) {
      continue;
    }
    const bNode &node = *link->fromnode;
    printf("paint layers dump: %*s<- %s '%s' custom1=%d custom2=%d\n",
           depth * 2,
           "",
           node.typeinfo != nullptr ? node.typeinfo->idname.c_str() : "?",
           node.name,
           node.custom1,
           node.custom2);
    for (const bNodeSocket &input : node.inputs) {
      if (input.default_value != nullptr && input.type == SOCK_FLOAT) {
        printf("paint layers dump: %*s   in '%s' = %.4f\n",
               depth * 2,
               "",
               input.name,
               static_cast<const bNodeSocketValueFloat *>(input.default_value)->value);
      }
      else if (input.default_value != nullptr && input.type == SOCK_RGBA) {
        const float *v = static_cast<const bNodeSocketValueRGBA *>(input.default_value)->value;
        printf("paint layers dump: %*s   in '%s' = (%.4f %.4f %.4f)\n",
               depth * 2,
               "",
               input.name,
               v[0],
               v[1],
               v[2]);
      }
      dump_socket_chain(input, depth + 1);
    }
  }
}

/**
 * TZ-15/16: a Material row read live through the SourceGroup wrapper (run A) and the same row read
 * from its baked maps in Hybrid/Baked (run B) must both equal the source's own values. The maps are
 * synthesized the way #material_bake_to_images/#BKE_paint_layers_material_bake_apply leave them:
 * float data maps, coverage = the source alpha.
 */
TEST_F(PaintLayersGraphEvalTest, material_row_source_group_matches_its_baked_maps)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "ParityLayered");
  Material *source = make_specular_source(*bmain, "ParitySource", false);

  BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  MaterialPaintLayer *top = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Top", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(top, nullptr);
  /* Specular and Emission are read from the source beyond the build default set. */
  channel_set_extend(*ma,
                     channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                         channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION));

  const eMaterialPaintChannel channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            PAINT_MATERIAL_CHANNEL_METALLIC,
                                            PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                            PAINT_MATERIAL_CHANNEL_SPECULAR,
                                            PAINT_MATERIAL_CHANNEL_EMISSION};
  /* The source's own values, read off #make_specular_source: RGB Base Color, instance Metallic,
   * Value Roughness, unlinked Specular 0.25, Principled Emission default. */
  const RGBA expected[] = {{0.2f, 0.5f, 0.9f, 1.0f},
                           {0.7f, 0.7f, 0.7f, 1.0f},
                           {0.3f, 0.3f, 0.3f, 1.0f},
                           {0.25f, 0.25f, 0.25f, 1.0f},
                           {1.0f, 1.0f, 1.0f, 1.0f}};
  const float tolerance = 1e-3f;

  /* Run A: the Material row is active, so it reads its source through the wrapper. */
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interp_a;
  interp_a.instance = find_instance();
  interp_a.tree = ma->paint_layers_tree;
  interp_a.x = 1;
  interp_a.y = 1;
  ASSERT_NE(interp_a.instance, nullptr);
  interp_a.tree->ensure_topology_cache();

  Vector<RGBA> a_values;
  for (const int i : IndexRange(ARRAY_SIZE(channels))) {
    a_values.append(eval_channel_result(interp_a, channels[i]));
    const char *ui_name = BKE_paint_material_channel_info(channels[i]).ui_name;
    EXPECT_NEAR(a_values[i].r, expected[i].r, tolerance) << ui_name;
    EXPECT_NEAR(a_values[i].g, expected[i].g, tolerance) << ui_name;
    EXPECT_NEAR(a_values[i].b, expected[i].b, tolerance) << ui_name;
  }
  {
    const bNode *output = GraphInterpreter::active_group_output(*interp_a.tree);
    ASSERT_NE(output, nullptr);
    char result_name[64];
    BLI_snprintf(result_name,
                 sizeof(result_name),
                 "Result %s",
                 BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_SPECULAR).ui_name);
    for (bNodeTreeInterfaceSocket *iface : interp_a.tree->interface_outputs()) {
      if (iface->name == nullptr || iface->identifier == nullptr ||
          !STREQ(iface->name, result_name))
      {
        continue;
      }
      if (const bNodeSocket *input = bke::node_find_socket(
              const_cast<bNode &>(*output),
              SOCK_IN,
              UString::from_ptr_noinline(iface->identifier)))
      {
        printf("paint layers dump: run A specular chain\n");
        dump_socket_chain(*input, 1);
      }
    }
  }

  /* Run B: synthesize the maps the material bake would write from the source's values, hand them
   * to the row, and move the active marker to the top row so the Material row reads them. */
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage = make_bake_data_map(bmain, "ParityCoverage", size, white);
  ASSERT_NE(coverage, nullptr);
  for (const int i : IndexRange(ARRAY_SIZE(channels))) {
    char name[64];
    BLI_snprintf(
        name, sizeof(name), "Parity %s", BKE_paint_material_channel_info(channels[i]).ui_name);
    const float rgba[4] = {expected[i].r, expected[i].g, expected[i].b, 1.0f};
    Image *map = make_bake_data_map(bmain, name, size, rgba);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, channels[i], map));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *row);

  BKE_paint_layers_active_set(*ma, top->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interp_b;
  interp_b.instance = find_instance();
  interp_b.tree = ma->paint_layers_tree;
  interp_b.x = 1;
  interp_b.y = 1;
  ASSERT_NE(interp_b.instance, nullptr);
  interp_b.tree->ensure_topology_cache();

  for (const int i : IndexRange(ARRAY_SIZE(channels))) {
    const RGBA b = eval_channel_result(interp_b, channels[i]);
    const char *ui_name = BKE_paint_material_channel_info(channels[i]).ui_name;
    printf("paint layers parity: %-12s source=(%.4f %.4f %.4f) A=(%.4f %.4f %.4f) "
           "B=(%.4f %.4f %.4f)\n",
           ui_name,
           expected[i].r,
           expected[i].g,
           expected[i].b,
           a_values[i].r,
           a_values[i].g,
           a_values[i].b,
           b.r,
           b.g,
           b.b);
    EXPECT_NEAR(b.r, expected[i].r, tolerance) << ui_name;
    EXPECT_NEAR(b.g, expected[i].g, tolerance) << ui_name;
    EXPECT_NEAR(b.b, expected[i].b, tolerance) << ui_name;
  }
  {
    const bNode *output = GraphInterpreter::active_group_output(*interp_b.tree);
    ASSERT_NE(output, nullptr);
    char result_name[64];
    BLI_snprintf(result_name,
                 sizeof(result_name),
                 "Result %s",
                 BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_SPECULAR).ui_name);
    for (bNodeTreeInterfaceSocket *iface : interp_b.tree->interface_outputs()) {
      if (iface->name == nullptr || iface->identifier == nullptr ||
          !STREQ(iface->name, result_name))
      {
        continue;
      }
      if (const bNodeSocket *input = bke::node_find_socket(
              const_cast<bNode &>(*output),
              SOCK_IN,
              UString::from_ptr_noinline(iface->identifier)))
      {
        printf("paint layers dump: run B specular chain\n");
        dump_socket_chain(*input, 1);
      }
    }
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-16 regression: the wrapper must carry the SPECULAR channel. Its interface output used to
 * collide by identifier with the group copy's own `BSDF` output, so Specular read 0. Checked after
 * the first build and after a Base Color edit takes the rebuild branch, for a Specular linked
 * through a Group Input named "Specular" and an unlinked one.
 */
TEST_F(PaintLayersGraphEvalTest, material_row_source_group_keeps_specular)
{
  for (const bool linked : {false, true}) {
    ma = BKE_material_add(bmain, linked ? "SpecLinked" : "SpecUnlinked");
    Material *source = make_specular_source(
        *bmain, linked ? "SpecLinkedSource" : "SpecUnlinkedSource", linked);

    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
    /* Specular is outside the build default set; the source wrapper must still carry it. */
    channel_set_extend(*ma, channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR));
    BKE_paint_layers_active_set(*ma, row->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

    auto eval_channel = [&](const eMaterialPaintChannel channel) -> RGBA {
      GraphInterpreter interp;
      interp.instance = find_instance();
      interp.tree = ma->paint_layers_tree;
      interp.x = 1;
      interp.y = 1;
      interp.tree->ensure_topology_cache();
      return eval_channel_result(interp, channel);
    };

    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_SPECULAR).r, 0.25f, 1e-3f)
        << (linked ? "linked, first build" : "unlinked, first build");

    /* Move Base Color in the source: the source hash moves, so the wrapper takes the rebuild
     * branch of #BKE_paint_layers_source_group_ensure. */
    bNode *rgb = nullptr;
    for (bNode &node : source->nodetree->nodes) {
      if (node.type_legacy == SH_NODE_RGB) {
        rgb = &node;
      }
    }
    ASSERT_NE(rgb, nullptr);
    bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
    ASSERT_NE(rgb_out, nullptr);
    static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value[0] = 0.77f;
    BKE_ntree_update_tag_all(source->nodetree);
    BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_SPECULAR).r, 0.25f, 1e-3f)
        << (linked ? "linked, rebuilt" : "unlinked, rebuilt");
    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR).r, 0.77f, 1e-3f);
    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_ROUGHNESS).r, 0.3f, 1e-3f);
    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_METALLIC).r, 0.7f, 1e-3f);

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/** \} */

}  // namespace blender::bke::tests
