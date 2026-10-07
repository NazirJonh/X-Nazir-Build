/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers generator tests: pass-through signature, opacity edits, bake gates (Spec-29, F2-D) (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_generate_test_fixture.hh` /
 * `paint_layers_generate_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_generate_test_fixture.hh"
#include "intern/paint_layers_generate_test_shared.hh"

namespace blender::bke::tests {

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


/** A top-level row of \a ma by name, or null. */
static MaterialPaintLayer *find_named_layer(Material &ma, const char *name)
{
  for (MaterialPaintLayer &layer : ma.paint_layers) {
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
  /* The interface order of a rebuilt group renumbers its uniform slots without changing the code,
   * so the strict comparison is the code shape of the root with everything it instances. */
  const uint64_t before_shape = code_shape_deep(*root);
  const uint64_t code_shape_b = code_shape_signature(*layer_tree_find(*bmain, "B"));

  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The folder owns no group and no node; the root is the same tree it was before the group, and
   * B keeps its warm chains inside the Pass Through folder. */
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(code_shape_deep(*root), before_shape);
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "B")), code_shape_b);
  EXPECT_EQ(root_signature(*root).size(), before.size());

  ASSERT_TRUE(BKE_paint_layers_ungroup(*ma, folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_NE(layer_tree_find(*bmain, "B"), nullptr);
  EXPECT_EQ(code_shape_deep(*root), before_shape);
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "B")), code_shape_b);
  EXPECT_EQ(root_signature(*root).size(), before.size());
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
  const uint64_t before = code_shape_deep(*root);
  const uint64_t shape_b = code_shape_signature(*layer_tree_find(*bmain, "B"));

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
  /* Rows in Pass Through folders keep their warm chains, so the code is the same. */
  EXPECT_EQ(code_shape_deep(*root), before);
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "B")), shape_b);
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
  bNodeSocket *opacity = root_instance_input("B Base Color Opacity");
  ASSERT_NE(opacity, nullptr);
  ASSERT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 1.0f);

  /* Hiding the folder scales the child's factor through its value input: the root's nodes and links
   * and the child's group are the same objects, only the value written to the root mirror moves. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(nodes_before, root_nodes(*root)));
  EXPECT_EQ(layer_tree_find(*bmain, "B"), group);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  opacity = root_instance_input("B Base Color Opacity");
  ASSERT_NE(opacity, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 0.0f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, true));
  BKE_paint_layers_values_sync(*ma);
  opacity = root_instance_input("B Base Color Opacity");
  ASSERT_NE(opacity, nullptr);
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
  /* The code shape, not the raw signature: a rebuilt group renumbers its uniform slots. */
  const uint64_t before = code_shape_deep(*root);
  ASSERT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(folder_tree_find(*bmain, "Folder"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 1.0f));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(code_shape_deep(*root), before);

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(folder_tree_find(*bmain, "Folder"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MIX));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(code_shape_deep(*root), before);
}

TEST_F(PaintLayersGenerateTest, pass_through_folder_with_a_bake_stays_isolating)
{
  MaterialPaintLayer *b = add_paint_layer("B", add_image("B"));
  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*folder);
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
  const uint64_t before = code_shape_deep(*root);

  MaterialPaintLayer *folder = group_one(*ma, mat);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(source_wrapper_find(*bmain, "HashSource"), wrapper);
  /* The nested Material row keeps its warm chains, so the shader code is the same. */
  EXPECT_EQ(code_shape_deep(*root), before);
}

/* -------------------------------------------------------------------- */
/** \name Correction opacity through the BKE setter
 * \{ */

namespace {

/**
 * Set a per (row, channel) opacity through the BKE setter (9.2: BKE tests drive BKE, not RNA).
 * The write is followed by the same values sync the RNA slider's update runs, so the generated
 * group inputs below read the new factor. The slider reaching this field through a nested
 * correction is covered by the RNA suite (paint_layers_description_test.cc).
 */
void bke_set_channel_opacity(Material &ma,
                             MaterialPaintLayer &layer,
                             const int channel,
                             const float factor)
{
  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(ma, layer, channel, factor));
  BKE_paint_layers_values_sync(ma);
}

/** Set a row's own opacity the way the Outliner does for a mask item, through BKE (9.2). */
void bke_set_row_opacity(Material &ma, MaterialPaintLayer &layer, const float factor)
{
  ASSERT_TRUE(BKE_paint_layers_set_opacity(ma, &layer, factor));
}

}  // namespace

/**
 * A content correction's opacity slider is a per (row, channel) setting, and the RNA setter has to
 * find the correction -- an element of its parent's `effects` -- before it can write it. This is the
 * reported bug: the slider was a no-op for a Paint correction, while a mask item (whose slider uses
 * the row pointer) worked. The check is the generated group input the row's factor reads.
 */
TEST_F(PaintLayersGenerateTest, content_correction_opacity_bke_reaches_its_graph_input)
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
    bNodeSocket *socket = root_instance_input(name);
    EXPECT_NE(socket, nullptr) << name;
    return static_cast<bNodeSocketValueFloat *>(socket->default_value)->value;
  };
  const char *corr_socket = "Mat C Base Color Opacity";
  const char *mask_socket = "Mat Mask Base Color Opacity";
  EXPECT_FLOAT_EQ(socket_value(corr_socket), 1.0f);
  EXPECT_FLOAT_EQ(socket_value(mask_socket), 1.0f);

  /* The content correction's per-channel value, exactly as the BKE setter writes it. */
  bke_set_channel_opacity(*ma, *corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.25f);
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

  /* The mask item's row value keeps working through the BKE setter. */
  bke_set_row_opacity(*ma, *mask_item, 0.4f);
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(socket_value(mask_socket), 0.4f);
}

/** The same slider on a plain Paint row's content correction, so the fix does not regress it. */
TEST_F(PaintLayersGenerateTest, paint_row_correction_opacity_bke_reaches_its_graph_input)
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
  bNodeSocket *socket = root_instance_input("Paint C Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  ASSERT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 1.0f);

  bke_set_channel_opacity(*ma, *corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.3f);
  socket = root_instance_input("Paint C Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.3f);
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Paint"), group);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spec-29: no bake, no schedule -- the light-row gate
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
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(light->bake, nullptr);
  /* row_is_substituted's own rule for a non-Material row is exactly this. */
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *light));
}

/**
  * Test #2, defect A: a light row's opacity is a value edit, written through the BKE setter
  * (#bke_set_row_opacity, 9.2), not the raw C API. #paint_layers_tag_value_edited (untouched by
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
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_EQ(bottom->bake, nullptr)
      << "a naive up-front allocation for every non-folder row would leave a bake structure here";
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0);

  /* The BKE path: MaterialPaintLayer.opacity, what the Outliner's slider forwards to. */
  bke_set_row_opacity(*ma, *bottom, 0.42f);

  EXPECT_EQ(bottom->bake, nullptr);
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "an opacity-only edit on a permanently bakeless row must not tag a topology rebuild";

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.6f));
}

/**
 * Test #4, defect B: two light rows that never get a bake structure must not stall the
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_EQ(light->bake, nullptr);
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*light), MA_PAINT_LAYER_BAKE_AUTO);
}

/**
 * Test #7: regression guard for the existing rule that the active row and its ancestors stay live
 * regardless of weight -- #BKE_paint_layers_bake_row_is_deferred is checked before the weight gate in
 * both branches of #BKE_paint_layers_bake_plan_run, so an active heavy row is never touched (no
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
                                              PAINT_MATERIAL_CHANNEL_SPECULAR,
                                              PAINT_MATERIAL_CHANNEL_NORMAL,
                                              PAINT_MATERIAL_CHANNEL_AO,
                                              PAINT_MATERIAL_CHANNEL_EMISSION})
  {
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, corr, channel), nullptr);
  }
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *paint));

  BKE_paint_layers_active_set(*ma, paint->marker);
  ASSERT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *paint));

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(paint->bake, nullptr);
  for (const uint32_t uid : bake_uids) {
    EXPECT_EQ(BKE_libblock_find_session_uid(bmain, ID_IM, uid), nullptr)
        << "a service map of the dropped bake was left behind";
  }

  /* The row no longer carries a bake, so #paint_layer_or_ancestor_has_bake sees none and a value
   * edit is no longer topology. */
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  bke_set_row_opacity(*ma, *paint, 0.42f);
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *paint));

  ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *paint));

  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_NE(add_channel(*heavy, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("HeavyNormal")), nullptr);
  ASSERT_NE(add_channel(*heavy, PAINT_MATERIAL_CHANNEL_AO, add_image("HeavyAO")), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *heavy));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *heavy));

  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  /* AUTO bakes only folders nested in another folder (level 2), so wrap it. */
  ASSERT_NE(group_one(*ma, folder), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  /* No structure before the gates pass: the synchronous planner leaves a heavy folder to the job. */
  EXPECT_EQ(folder->bake, nullptr);
  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, child->marker);
  ASSERT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_NEVER));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  bool changed = true;
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(folder->bake, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));

  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  bke_set_row_opacity(*ma, *child, 0.42f);
  EXPECT_EQ(folder->bake, nullptr);
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "a value edit under a permanently-bakeless folder must not tag a topology rebuild";
}

/**
 * F2-D2 (step 1), guard. The reference for a row is #visibility_does_not_invalidate_the_rows_own_bake
  * (a baked row's own hash moves on a value edit) plus #light_row_opacity_edit_does_not_regen_or_rebuild
  * (`bke_set_row_opacity` is the BKE side of the Outliner path; #paint_layer_or_ancestor_has_bake decides
 * #MA_PAINT_LAYERS_REGEN). Here the same sequence runs for the child of a heavy, auto-baked,
 * isolating folder: the edit invalidates the folder's bake, tags stale/regen, and the drain restores
 * it.
 */
TEST_F(PaintLayersGenerateTest, auto_baked_folder_child_value_edit_invalidates_and_drains)
{
  MaterialPaintLayer *child = add_paint_layer("EditChild", add_image("EditChildImg"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  /* AUTO bakes only folders nested in another folder (level 2), so wrap it. */
  ASSERT_NE(group_one(*ma, folder), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
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
  bke_set_row_opacity(*ma, *child, 0.42f);

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
  ASSERT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
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
  /* AUTO bakes only folders nested in another folder (level 2), so wrap it. */
  ASSERT_NE(group_one(*ma, folder), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
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
  bke_set_row_opacity(*ma, *outside, 0.33f);

  uint32_t after[2];
  BKE_paint_layers_bake_hash(*folder, after);
  EXPECT_TRUE(before[0] == after[0] && before[1] == after[1])
      << "a sibling's value must not move the folder's bake hash";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  EXPECT_FALSE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0)
      << "no bake covers the sibling, so its value edit must stay the free path";
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
}

TEST_F(PaintLayersGenerateTest, folder_level_counts_enclosing_folders)
{
  MaterialPaintLayer *root_row = add_paint_layer("Root", add_image("Root"));
  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
  ASSERT_NE(outer, nullptr);
  ASSERT_NE(inner, nullptr);
  MaterialPaintLayer *leaf = add_paint_layer_into(inner, "Leaf", add_image("Leaf"));
  EXPECT_EQ(BKE_paint_layers_folder_level(*ma, *root_row), 0);
  EXPECT_EQ(BKE_paint_layers_folder_level(*ma, *outer), 1);
  EXPECT_EQ(BKE_paint_layers_folder_level(*ma, *inner), 2);
  EXPECT_EQ(BKE_paint_layers_folder_level(*ma, *leaf), 2);
  MaterialPaintLayer stray{};
  EXPECT_EQ(BKE_paint_layers_folder_level(*ma, stray), -1);
}

/** Builds a heavy folder (weight above the AUTO threshold) holding one row with four channels. */
static MaterialPaintLayer *heavy_folder_make(PaintLayersGenerateTest &test)
{
  MaterialPaintLayer *child = test.add_paint_layer("HeavyChild", test.add_image("HeavyChildMap"));
  /* Three added channels plus the helper's Base Color keep the child itself light (28) while the
   * folder is heavy (40, over the 36-node AUTO threshold). */
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR})
  {
    EXPECT_NE(BKE_paint_layers_channel_add(*test.ma, child, channel), nullptr);
  }
  MaterialPaintLayer *folder = group_one(*test.ma, child);
  EXPECT_NE(folder, nullptr);
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*test.ma, folder, 0.5f));
  return folder;
}

TEST_F(PaintLayersGenerateTest, auto_bake_skips_a_folder_in_the_stack_root)
{
  MaterialPaintLayer *folder = heavy_folder_make(*this);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
  EXPECT_EQ(BKE_paint_layers_bake_job_create(*bmain, *ma), nullptr);
}

TEST_F(PaintLayersGenerateTest, auto_bake_takes_a_folder_nested_in_another_folder)
{
  MaterialPaintLayer *inner = heavy_folder_make(*this);
  MaterialPaintLayer *outer = group_one(*ma, inner);
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(BKE_paint_layers_folder_level(*ma, *inner), 2);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  EXPECT_NE(inner->bake, nullptr);
  EXPECT_EQ(outer->bake, nullptr) << "the outer folder is at level 1 and stays live";
  BKE_paint_layers_bake_job_free(*job);
}

/** A manual ALWAYS is the user's choice: the root folder still bakes. */
TEST_F(PaintLayersGenerateTest, always_mode_still_bakes_a_folder_in_the_stack_root)
{
  MaterialPaintLayer *child = add_paint_layer("AlwaysChild", add_image("AlwaysChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, 4));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
}

/** A bake an AUTO root folder carried from before the rule is released once. */
TEST_F(PaintLayersGenerateTest, auto_root_folder_bake_from_before_the_rule_is_released)
{
  MaterialPaintLayer *child = add_paint_layer("OldChild", add_image("OldChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, 4));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(folder->bake, nullptr);

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_AUTO));
  /* The mode is not in the bake hash; the level rule releases the root folder's bake before any
   * gate reads it, and the value move keeps this test independent of the gate order. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.25f));
  changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_EQ(folder->bake, nullptr);
}

/** The size of a root folder's released bake is the user's choice: the folder's next bake uses it. */
TEST_F(PaintLayersGenerateTest, root_folder_bake_size_survives_the_auto_release)
{
  MaterialPaintLayer *child = add_paint_layer("KeptChild", add_image("KeptChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *folder, 4));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(folder->bake, nullptr);

  /* Back to AUTO: the rule releases the root folder's bake, structure and all. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_AUTO));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.25f));
  changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_EQ(folder->bake, nullptr);

  /* Baking the folder again (a manual ALWAYS) uses the kept size, not the map's own 8x8. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(folder->bake, nullptr);
  EXPECT_EQ(folder->bake->size, 4);
}

/** A child folder whose bake stands in costs the parent one map, not its whole subtree. */
TEST_F(PaintLayersGenerateTest, baked_child_folder_counts_as_one_map_in_the_parent_weight)
{
  MaterialPaintLayer *inner = heavy_folder_make(*this);
  MaterialPaintLayer *outer = group_one(*ma, inner);
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *outer));

  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*inner), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *inner);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *inner));
  EXPECT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *outer))
      << "a valid child bake must not be counted as its full subtree";
}

/** The maps that stand in for a Material child are part of what its parent's bake is valid for. */
TEST_F(PaintLayersGenerateTest, parent_bake_hash_sees_the_maps_of_a_material_child)
{
  Material *source = add_principled_source("MapSource", 0.3f);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, child, source));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*folder), nullptr);
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*child), nullptr);

  uint32_t folder_before[2];
  uint32_t child_before[2];
  BKE_paint_layers_bake_hash(*folder, folder_before);
  BKE_paint_layers_bake_hash(*child, child_before);

  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ChildMap")));

  uint32_t folder_after[2];
  uint32_t child_after[2];
  BKE_paint_layers_bake_hash(*folder, folder_after);
  BKE_paint_layers_bake_hash(*child, child_after);
  EXPECT_TRUE(folder_before[0] != folder_after[0] || folder_before[1] != folder_after[1]);
  /* The row's own bake *is* those maps, so its own hash must not move with them. */
  EXPECT_EQ(child_before[0], child_after[0]);
  EXPECT_EQ(child_before[1], child_after[1]);
}

/** A pixel change in a Material child's map makes its parent's bake stale. */
TEST_F(PaintLayersGenerateTest, parent_bake_is_stale_when_a_material_childs_map_pixels_change)
{
  Material *source = add_principled_source("PixelSource", 0.3f);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatChild", folder, PaintLayerPlace::Into);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, child, source));
  Image *map = add_image("ChildPixels");
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*folder), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *folder);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));

  BKE_image_partial_update_mark_full_update(map);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *folder));
}

TEST_F(PaintLayersGenerateTest, deleting_a_row_source_keeps_the_row_on_its_maps)
{
  Material *source = add_principled_source("DoomedSource", 0.3f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  Image *map = add_image("RowMap");
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  BKE_id_delete(bmain, source);
  EXPECT_EQ(row->material, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_NE(row->bake, nullptr);
  EXPECT_EQ(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], map);
}

TEST_F(PaintLayersGenerateTest, deleting_a_child_source_invalidates_the_parent_bake_once)
{
  Material *source = add_principled_source("DoomedChildSource", 0.3f);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Child", folder, PaintLayerPlace::Into);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, child, source));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ChildMap")));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*folder), nullptr);
  BKE_paint_layers_bake_finalize(*ma, *folder);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));

  BKE_id_delete(bmain, source);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The child's source is gone from the hash, so the folder re-renders once from the child's maps. */
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *folder));
}

/** The oracle notices a structural change (guards the helper itself). */
TEST_F(PaintLayersGenerateTest, code_shape_signature_moves_when_the_structure_changes)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Bottom"));

  /* A Fill (constant) effect does not fit the Paint layer's warm effect slot, so the code changes;
   * a Paint effect would take the slot and keep it. */
  MaterialPaintLayer *fx = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fx");
  ASSERT_NE(fx, nullptr);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fx, green));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before);
}

/** Target behavior: adding a Paint mask to a root Paint layer does not change the shader code. */
TEST_F(PaintLayersGenerateTest, adding_a_paint_mask_to_a_root_paint_layer_keeps_the_code_shape)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Bottom"));

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(mask, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  /* +warm: the spare reads a Non-Color straight map, so the real one must decode the same way. */
  Image *mask_map = add_image("MaskMap");
  make_generate_image_mask_like(*mask_map);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before);
}

/**
 * The UI adds the correction first and its source is picked afterward, so the spare must keep
 * standing in until the real chain is actually built: consuming on an imageless mask would drop
 * the spare's chain while nothing replaces it, and one user action would change the shader code
 * twice.
 */
TEST_F(PaintLayersGenerateTest, adding_an_imageless_paint_mask_keeps_the_code_shape)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Bottom"));

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(mask, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before);
  EXPECT_NE(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);

  /* The map's arrival is the moment the real chain replaces the spare: same shape, same code. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  Image *mask_map = add_image("MaskMap");
  make_generate_image_mask_like(*mask_map);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before);
  EXPECT_EQ(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);
}

/**
 * Negative control: a row inside an isolating folder has no warm slot, so the same edit changes the
 * code. (Rows inside a Pass Through folder do carry warm slots now, hence the opacity.)
 */
TEST_F(PaintLayersGenerateTest, adding_a_paint_mask_to_a_nested_row_changes_the_code_shape)
{
  MaterialPaintLayer *child = add_paint_layer("Nested", add_image("Nested"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Nested"));

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, child, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("NestedMask")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(code_shape_signature(*layer_tree_find(*bmain, "Nested")), before);
}

}  // namespace blender::bke::tests
