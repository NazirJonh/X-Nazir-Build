/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers generator tests: Stage 2 UV mapping of Material rows (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_generate_test_fixture.hh` /
 * `paint_layers_generate_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_generate_test_fixture.hh"
#include "intern/paint_layers_generate_test_shared.hh"

namespace blender::bke::tests {

/** \name Stage 2: UV mapping of a Material row
 * \{ */

/** A source whose Base Color reads a Texture Coordinate-fed Image: a SourceGroup row's source. */
Material *add_texcoord_image_source(Main &bmain, const char *name, Image *image)
{
  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = nullptr;
  for (bNode &node : src_tree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  if (principled == nullptr) {
    /* A fresh material has no node graph of its own to start from. */
    principled = bke::node_add_static_node(nullptr, src_tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, src_tree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(src_tree,
                       *principled,
                       *sock_out(*principled, "BSDF"),
                       *output,
                       *sock_in(*output, "Surface"));
  }
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  tex->id = &image->id;
  id_us_plus(&image->id);
  bNode *tex_coord = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_COORD);
  bke::node_add_link(src_tree,
                     *tex_coord,
                     *sock_out(*tex_coord, "UV"),
                     *tex,
                     *sock_in(*tex, "Vector"));
  bke::node_add_link(src_tree,
                     *tex,
                     *sock_out(*tex, "Color"),
                     *principled,
                     *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(bmain, src_tree);
  return source;
}

/** Stage 2: a mapped row's wrapper puts one Mapping node before every fed UV output, and the
 * source itself is untouched. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_builds_one_node_per_uv_source)
{
  Material *source = add_principled_source("MapWrapSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  /* A second UV source: a UV Map naming a *source* layer, which is not the owner's mapping to
   * remap. Its texture stays wired to it directly. */
  bNode *tex_a = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_b = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_coord = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_COORD);
  bNode *src_uv = bke::node_add_static_node(nullptr, src_tree, SH_NODE_UVMAP);
  ASSERT_NE(tex_a, nullptr);
  ASSERT_NE(tex_b, nullptr);
  ASSERT_NE(tex_coord, nullptr);
  ASSERT_NE(src_uv, nullptr);
  BLI_strncpy(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map,
              "SourceUV",
              sizeof(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map));
  bke::node_add_link(src_tree, *tex_coord, *sock_out(*tex_coord, "UV"), *tex_a, *sock_in(*tex_a, "Vector"));
  bke::node_add_link(src_tree, *src_uv, *sock_out(*src_uv, "UV"), *tex_b, *sock_in(*tex_b, "Vector"));
  bke::node_add_link(src_tree, *tex_a, *sock_out(*tex_a, "Color"), *principled, *sock_in(*principled, "Base Color"));
  bke::node_add_link(src_tree, *tex_b, *sock_out(*tex_b, "Color"), *principled, *sock_in(*principled, "Roughness"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  group->ensure_topology_cache();

  /* One Mapping node: the Texture Coordinate's UV output feeds something, and the owner-named
   * UV Map node does not exist here (no open texture needed one). The source's own UV Map node
   * names another layer and is not the row's mapping to remap. */
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  bNode *mapping = find_type(*group, SH_NODE_MAPPING);
  ASSERT_NE(mapping, nullptr);
  EXPECT_TRUE(StringRef(mapping->name).startswith(".PL Mapping"));

  /* The coordinate's UV output has exactly one reader: the Mapping node, whose own Vector reads
   * the coordinate. */
  Vector<const bNodeLink *> uv_links = outgoing_links(*group, *node_by_name(*group, tex_coord->name), "UV");
  ASSERT_EQ(uv_links.size(), 1);
  EXPECT_EQ(uv_links[0]->tonode, mapping);
  bNodeSocket *map_vector = sock_in(*mapping, "Vector");
  ASSERT_NE(map_vector, nullptr);
  ASSERT_FALSE(map_vector->directly_linked_links().is_empty());
  EXPECT_EQ(map_vector->directly_linked_links()[0]->fromnode,
            node_by_name(*group, tex_coord->name));
  /* The texture reads the Mapping's Vector. */
  ASSERT_EQ(vector_source(*node_by_name(*group, tex_a->name))->type_legacy, SH_NODE_MAPPING);
  /* The other texture keeps its own source UV Map node. */
  bNode *copied_b = node_by_name(*group, tex_b->name);
  ASSERT_NE(copied_b, nullptr);
  bNode *b_source = vector_source(*copied_b);
  ASSERT_NE(b_source, nullptr);
  EXPECT_EQ(b_source->type_legacy, SH_NODE_UVMAP);
  EXPECT_STREQ(uv_node_layer(*b_source), "SourceUV");

  /* The wrapper's interface carries the three mapping inputs, marked by their roles. */
  EXPECT_NE(wrapper_mapping_input(*group, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET), nullptr);
  EXPECT_NE(wrapper_mapping_input(*group, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_SCALE), nullptr);
  EXPECT_NE(wrapper_mapping_input(*group, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_ROTATION), nullptr);

  /* The source never changed: no Mapping node, the coordinate still feeds its texture. */
  EXPECT_EQ(count_type(src_tree, SH_NODE_MAPPING), 0);
  ASSERT_EQ(outgoing_links(src_tree, *tex_coord, "UV").size(), 1);
  EXPECT_EQ(outgoing_links(src_tree, *tex_coord, "UV")[0]->tonode, tex_a);
}

/** Stage 2: a source with no UV readers builds no Mapping node even with the mapping on. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_noise_source_builds_no_node)
{
  Material *source = add_principled_source("MapNoiseSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
}

/** An Attribute (Geometry) node reading \a attribute_name. */
bNode *add_attribute_node(bNodeTree &tree, const char *attribute_name)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_ATTRIBUTE);
  NodeShaderAttribute *storage = static_cast<NodeShaderAttribute *>(node->storage);
  storage->type = SHD_ATTRIBUTE_GEOMETRY;
  BLI_strncpy(storage->name, attribute_name, sizeof(storage->name));
  return node;
}

/** The nodes fed by \a node's \a output in \a tree, read from the tree's links. */
Vector<const bNode *> output_readers(bNodeTree &tree, const bNode &node, const char *output)
{
  Vector<const bNode *> out;
  for (const bNodeLink *link : outgoing_links(tree, node, output)) {
    out.append(link->tonode);
  }
  return out;
}

/** Stage D: an Attribute naming the owner's UV layer is remapped on its `Vector` output; its
 * Color/Fac outputs stay as they are, and the source is untouched. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_remaps_attribute_vector_of_owner_uv)
{
  Material *source = add_principled_source("AttrMapSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *attr = add_attribute_node(src_tree, "UVMap");
  bke::node_add_link(src_tree, *attr, *sock_out(*attr, "Vector"), *tex, *sock_in(*tex, "Vector"));
  bke::node_add_link(
      src_tree, *tex, *sock_out(*tex, "Color"), *principled, *sock_in(*principled, "Base Color"));
  /* Color and Fac carry the attribute's own data, not the UV. */
  bke::node_add_link(
      src_tree, *attr, *sock_out(*attr, "Fac"), *principled, *sock_in(*principled, "Roughness"));
  bke::node_add_link(
      src_tree, *attr, *sock_out(*attr, "Color"), *principled, *sock_in(*principled, "Emission Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);
  const uint64_t source_hash = BKE_paint_layers_source_material_tree_hash(*source);
  const int source_nodes = src_tree.nodes.count();

  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  bNode *mapping = find_type(*group, SH_NODE_MAPPING);
  ASSERT_NE(mapping, nullptr);
  bNode *copied_attr = node_by_name(*group, attr->name);
  ASSERT_NE(copied_attr, nullptr);
  /* Vector: one reader, the Mapping, whose Vector input reads the Attribute's Vector. */
  const Vector<const bNode *> vector_readers = output_readers(*group, *copied_attr, "Vector");
  ASSERT_EQ(vector_readers.size(), 1);
  EXPECT_EQ(vector_readers[0], mapping);
  bool mapping_reads_attribute = false;
  for (const bNodeLink &link : group->links) {
    if (link.tonode == mapping && link.tosock == sock_in(*mapping, "Vector")) {
      mapping_reads_attribute = link.fromnode == copied_attr &&
                                link.fromsock == sock_out(*copied_attr, "Vector");
    }
  }
  EXPECT_TRUE(mapping_reads_attribute);
  /* The texture reads the Mapping's Vector. */
  bool tex_reads_mapping = false;
  for (const bNodeLink &link : group->links) {
    if (link.tonode == node_by_name(*group, tex->name) && STREQ(link.tosock->identifier, "Vector")) {
      tex_reads_mapping = link.fromnode == mapping;
    }
  }
  EXPECT_TRUE(tex_reads_mapping);
  /* Color and Fac are not rewired: their readers are not the Mapping. */
  for (const char *output : {"Color", "Fac"}) {
    for (const bNode *reader : output_readers(*group, *copied_attr, output)) {
      EXPECT_NE(reader, mapping);
    }
  }

  /* The source never changed. */
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash(*source), source_hash);
  EXPECT_EQ(src_tree.nodes.count(), source_nodes);
  EXPECT_EQ(count_type(src_tree, SH_NODE_MAPPING), 0);
  EXPECT_EQ(output_readers(src_tree, *attr, "Vector").size(), 1);
  EXPECT_EQ(output_readers(src_tree, *attr, "Vector")[0], tex);
}

/** Stage D: an Attribute naming another layer, or any Attribute while the owner names no layer,
 * is not remapped without the remap-all flag. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_skips_attribute_of_another_layer)
{
  Material *source = add_principled_source("AttrOtherSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *attr = add_attribute_node(src_tree, "Other");
  bke::node_add_link(src_tree, *attr, *sock_out(*attr, "Vector"), *tex, *sock_in(*tex, "Vector"));
  bke::node_add_link(
      src_tree, *tex, *sock_out(*tex, "Color"), *principled, *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);

  /* Same source, an owner with no UV layer named. */
  BLI_strncpy(static_cast<NodeShaderAttribute *>(attr->storage)->name,
              "UVMap",
              sizeof(static_cast<NodeShaderAttribute *>(attr->storage)->name));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);
  ma->paint_layers_uv_map[0] = '\0';
  group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
}

/** Stage D: a UV Map node of another layer is remapped only with the remap-all flag; the flag
 * rebuilds the wrapper once each way and the topology comes back when it is cleared. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_remap_all_flag_rebuilds_wrapper)
{
  Material *source = add_principled_source("RemapAllSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *src_uv = bke::node_add_static_node(nullptr, src_tree, SH_NODE_UVMAP);
  BLI_strncpy(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map,
              "Lightmap",
              sizeof(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map));
  bke::node_add_link(src_tree, *src_uv, *sock_out(*src_uv, "UV"), *tex, *sock_in(*tex, "Vector"));
  bke::node_add_link(
      src_tree, *tex, *sock_out(*tex, "Color"), *principled, *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  /* Default off, as in a file written before the flag existed. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REMAP_ALL_UV, 0);
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bool changed = false;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed, nullptr, true);
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(changed);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
  EXPECT_EQ(BKE_paint_layers_source_group_ensure(
                *bmain, *ma, *source, refusal, &changed, nullptr, true),
            group);
  EXPECT_FALSE(changed);

  ma->paint_layers_flag |= MA_PAINT_LAYERS_REMAP_ALL_UV;
  group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed, nullptr, true);
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(changed);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  bNode *mapping = find_type(*group, SH_NODE_MAPPING);
  ASSERT_NE(mapping, nullptr);
  EXPECT_EQ(vector_source(*node_by_name(*group, tex->name)), mapping);
  BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal, &changed, nullptr, true);
  EXPECT_FALSE(changed);

  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REMAP_ALL_UV;
  group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed, nullptr, true);
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(changed);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
  EXPECT_EQ(vector_source(*node_by_name(*group, tex->name))->type_legacy, SH_NODE_UVMAP);
}

/** Stage D: with the remap-all flag an Attribute of any layer is remapped on `Vector`, three
 * rebuilds in a row add no Mapping or link, and the source stays untouched. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_remap_all_attribute_is_idempotent)
{
  Material *source = add_principled_source("RemapAttrSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *attr = add_attribute_node(src_tree, "Other");
  bke::node_add_link(src_tree, *attr, *sock_out(*attr, "Vector"), *tex, *sock_in(*tex, "Vector"));
  bke::node_add_link(
      src_tree, *tex, *sock_out(*tex, "Color"), *principled, *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);
  const uint64_t source_hash = BKE_paint_layers_source_material_tree_hash(*source);
  const int source_nodes = src_tree.nodes.count();

  ma->paint_layers_flag |= MA_PAINT_LAYERS_REMAP_ALL_UV;
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  const int mappings = count_type(*group, SH_NODE_MAPPING);
  const int links = group->links.count();
  const int nodes = group->nodes.count();
  EXPECT_EQ(mappings, 1);
  for (int i = 0; i < 3; i++) {
    bNodeTree *again = BKE_paint_layers_source_group_ensure(
        *bmain, *ma, *source, refusal, nullptr, nullptr, true);
    ASSERT_EQ(again, group);
    EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), mappings);
    EXPECT_EQ(group->links.count(), links);
    EXPECT_EQ(group->nodes.count(), nodes);
  }
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash(*source), source_hash);
  EXPECT_EQ(src_tree.nodes.count(), source_nodes);
  EXPECT_EQ(count_type(src_tree, SH_NODE_MAPPING), 0);
}

/** Stage 2: a source with its own Mapping node reads the row's mapping in front of it. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_applies_on_top_of_the_source_own_mapping)
{
  Material *source = add_principled_source("MapTopSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_coord = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_COORD);
  bNode *own_mapping = bke::node_add_static_node(nullptr, src_tree, SH_NODE_MAPPING);
  ASSERT_NE(own_mapping, nullptr);
  bke::node_add_link(src_tree, *tex_coord, *sock_out(*tex_coord, "UV"), *own_mapping, *sock_in(*own_mapping, "Vector"));
  bke::node_add_link(src_tree, *own_mapping, *sock_out(*own_mapping, "Vector"), *tex, *sock_in(*tex, "Vector"));
  bke::node_add_link(src_tree, *tex, *sock_out(*tex, "Color"), *principled, *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();

  /* Two Mapping nodes: the copied source one, named "Mapping", and the row's one in front. */
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 2);
  bNode *row_mapping = node_by_name(*group, ".PL Mapping");
  ASSERT_NE(row_mapping, nullptr);
  bNode *copied_own = node_by_name(*group, own_mapping->name);
  ASSERT_NE(copied_own, nullptr);
  /* The coordinate feeds the row's Mapping, the row's Mapping feeds the source's own. */
  bNodeSocket *own_vector = sock_in(*copied_own, "Vector");
  ASSERT_NE(own_vector, nullptr);
  ASSERT_FALSE(own_vector->directly_linked_links().is_empty());
  EXPECT_EQ(own_vector->directly_linked_links()[0]->fromnode, row_mapping);
  bNodeSocket *row_vector = sock_in(*row_mapping, "Vector");
  ASSERT_NE(row_vector, nullptr);
  ASSERT_FALSE(row_vector->directly_linked_links().is_empty());
  EXPECT_EQ(row_vector->directly_linked_links()[0]->fromnode,
            node_by_name(*group, tex_coord->name));
}

/** Stage 2: the value sync matches nodes by name; a source node called "Mapping" never collides
 * with the generated one, and a source value edit syncs without a rebuild. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_syncs_a_source_node_named_mapping)
{
  Material *source = add_principled_source("MapNameSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_coord = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_COORD);
  bNode *own_mapping = bke::node_add_static_node(nullptr, src_tree, SH_NODE_MAPPING);
  ASSERT_NE(own_mapping, nullptr);
  bke::node_add_link(src_tree, *tex_coord, *sock_out(*tex_coord, "UV"), *own_mapping, *sock_in(*own_mapping, "Vector"));
  bke::node_add_link(src_tree, *own_mapping, *sock_out(*own_mapping, "Vector"), *tex, *sock_in(*tex, "Vector"));
  bke::node_add_link(src_tree, *tex, *sock_out(*tex, "Color"), *principled, *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);
  /* The default name of a Mapping node is "Mapping". */
  ASSERT_STREQ(own_mapping->name, "Mapping");

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 2);
  bNode *copied_own = node_by_name(*group, "Mapping");
  ASSERT_NE(copied_own, nullptr);

  /* Move the source's own mapping: a value edit, so the wrapper syncs in place. */
  copy_v3_fl3(static_cast<bNodeSocketValueVector *>(sock_in(*own_mapping, "Location")->default_value)
                  ->value,
              0.25f,
              0.5f,
              0.0f);
  bool values_synced = false;
  bNodeTree *again = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, &values_synced, true);
  EXPECT_EQ(again, group);
  EXPECT_TRUE(values_synced);
  group->ensure_topology_cache();
  /* Both Mapping nodes survive under distinct names, and the source value reached its copy. */
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 2);
  EXPECT_NE(node_by_name(*group, ".PL Mapping"), nullptr);
  copied_own = node_by_name(*group, "Mapping");
  ASSERT_NE(copied_own, nullptr);
  const float *location = static_cast<const bNodeSocketValueVector *>(
                              sock_in(*copied_own, "Location")->default_value)
                              ->value;
  EXPECT_NEAR(location[0], 0.25f, 1e-5f);
  EXPECT_NEAR(location[1], 0.5f, 1e-5f);
}

/** Stage 2: a mapped SourceGroup row's values ride the row group's inputs into the wrapper's
 * Mapping; three regenerations duplicate nothing, and a toggle rebuilds cleanly both ways. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_wires_row_inputs_and_survives_repeats)
{
  Material *source = add_texcoord_image_source(*bmain, "MapRowSource", add_image("MapRowSourceImg"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  bNodeTree *wrapper = find_source_wrapper(*bmain, "MapRowSource");
  ASSERT_NE(wrapper, nullptr);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  wrapper->ensure_topology_cache();
  EXPECT_EQ(count_type(*wrapper, SH_NODE_MAPPING), 1);
  bNode *instance = group_instance_of(*group, *wrapper);
  ASSERT_NE(instance, nullptr);
  /* The row group carries the three value inputs. */
  EXPECT_TRUE(interface_has_value_for(*group, row->marker, bke::paint_layers::ROLE_MAPPING_OFFSET));
  EXPECT_TRUE(interface_has_value_for(*group, row->marker, bke::paint_layers::ROLE_MAPPING_SCALE));
  EXPECT_TRUE(interface_has_value_for(*group, row->marker, bke::paint_layers::ROLE_MAPPING_ROTATION));
  /* The instance's mapping inputs are fed by the row group's Group Input. */
  EXPECT_TRUE(instance_mapping_input_wired(*group, *instance, ".PL Mapping Offset"));
  EXPECT_TRUE(instance_mapping_input_wired(*group, *instance, ".PL Mapping Scale"));
  EXPECT_TRUE(instance_mapping_input_wired(*group, *instance, ".PL Mapping Rotation"));
  const int instance_links = group->links.count();
  const int wrapper_nodes = wrapper->nodes.count();
  const int64_t group_inputs = group->interface_inputs().size();

  /* Three regenerations in a row add no node, input or link. */
  for (int pass = 0; pass < 3; pass++) {
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  }
  EXPECT_EQ(find_source_wrapper(*bmain, "MapRowSource"), wrapper);
  wrapper->ensure_topology_cache();
  group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  EXPECT_EQ(count_type(*wrapper, SH_NODE_MAPPING), 1);
  EXPECT_EQ(wrapper->nodes.count(), wrapper_nodes);
  EXPECT_EQ(group->links.count(), instance_links);
  EXPECT_EQ(group->interface_inputs().size(), group_inputs);

  /* Turning the mapping off rebuilds the wrapper without any Mapping node and the row group
   * without its inputs; turning it back on restores exactly what was there. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  wrapper = find_source_wrapper(*bmain, "MapRowSource");
  ASSERT_NE(wrapper, nullptr);
  wrapper->ensure_topology_cache();
  group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  EXPECT_EQ(count_type(*wrapper, SH_NODE_MAPPING), 0);
  EXPECT_FALSE(interface_has_value_for(*group, row->marker, bke::paint_layers::ROLE_MAPPING_OFFSET));
  instance = group_instance_of(*group, *wrapper);
  ASSERT_NE(instance, nullptr);
  EXPECT_EQ(instance_mapping_socket(*instance, ".PL Mapping Offset"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  wrapper = find_source_wrapper(*bmain, "MapRowSource");
  ASSERT_NE(wrapper, nullptr);
  wrapper->ensure_topology_cache();
  group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  EXPECT_EQ(count_type(*wrapper, SH_NODE_MAPPING), 1);
  EXPECT_EQ(group->interface_inputs().size(), group_inputs);
  instance = group_instance_of(*group, *wrapper);
  ASSERT_NE(instance, nullptr);
  EXPECT_TRUE(instance_mapping_input_wired(*group, *instance, ".PL Mapping Offset"));
}

/** Stage 2: two rows over one source -- only the mapped one wires its values, and both share the
 * one wrapper. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_two_rows_unmapped_row_keeps_identity)
{
  Material *source = add_texcoord_image_source(*bmain, "MapPairSource", add_image("MapPairSourceImg"));
  MaterialPaintLayer *row_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "SourceA", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row_a, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row_a, source));
  MaterialPaintLayer *row_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "SourceB", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row_b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row_b, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row_a, true));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row_a), PaintLayerMaterialMode::SourceGroup);
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row_b), PaintLayerMaterialMode::SourceGroup);
  bNodeTree *wrapper = find_source_wrapper(*bmain, "MapPairSource");
  ASSERT_NE(wrapper, nullptr);
  wrapper->ensure_topology_cache();
  EXPECT_EQ(count_type(*wrapper, SH_NODE_MAPPING), 1);
  bNodeTree *group_a = layer_tree_find(*bmain, "SourceA");
  bNodeTree *group_b = layer_tree_find(*bmain, "SourceB");
  ASSERT_NE(group_a, nullptr);
  ASSERT_NE(group_b, nullptr);
  group_a->ensure_interface_cache();
  group_b->ensure_interface_cache();
  bNode *instance_a = group_instance_of(*group_a, *wrapper);
  bNode *instance_b = group_instance_of(*group_b, *wrapper);
  ASSERT_NE(instance_a, nullptr);
  ASSERT_NE(instance_b, nullptr);
  /* The mapped row feeds the wrapper's Mapping; the unmapped one leaves the inputs at their
   * identity defaults (the wrapper is shared, so its inputs exist -- just unwired here). */
  EXPECT_TRUE(instance_mapping_input_wired(*group_a, *instance_a, ".PL Mapping Offset"));
  EXPECT_FALSE(instance_mapping_input_wired(*group_b, *instance_b, ".PL Mapping Offset"));
}

/** Stage 2: moving the mapping values is value-only -- neither the row's group, nor the root, nor
 * the wrapper rebuild. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_value_edit_never_rebuilds)
{
  Material *source = add_texcoord_image_source(*bmain, "MapDragSource", add_image("MapDragSourceImg"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *wrapper = find_source_wrapper(*bmain, "MapDragSource");
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(wrapper, nullptr);
  ASSERT_NE(group, nullptr);
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> wrapper_before = root_nodes(*wrapper);
  const Vector<bNode *> root_before = root_nodes(*root);
  ASSERT_TRUE(group_io_sentinel_set(*group, 0.75f));

  const float offset[2] = {0.25f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, row, offset));
  const float scale[2] = {-2.0f, 3.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, row, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Nothing was rebuilt: the stamps and the node lists survive, and the wrapper keeps its nodes. */
  EXPECT_EQ(find_source_wrapper(*bmain, "MapDragSource"), wrapper);
  group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group_io_sentinel_get(*group, 0.75f));
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_TRUE(same_nodes(wrapper_before, root_nodes(*wrapper)));
}

/** Stage 2: a shared source group whose tree reads the owner's UVs is copied privately when the
 * mapping applies, and left shared when it does not; the original never changes. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_shared_group_copies_privately)
{
  Material *source = add_principled_source("MapCowSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  /* The shared group: Texture Coordinate -> Image -> Group Output "Color". */
  bNodeTree *shared = bke::node_tree_add_tree(bmain, "PBR texture", "ShaderNodeTree");
  ASSERT_NE(shared, nullptr);
  ASSERT_NE(shared->tree_interface.add_socket(
                "Color", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr),
            nullptr);
  bNode *shared_output = bke::node_add_node(nullptr, *shared, "NodeGroupOutput"_ustr);
  bNode *shared_tex = bke::node_add_static_node(nullptr, *shared, SH_NODE_TEX_IMAGE);
  bNode *shared_coord = bke::node_add_static_node(nullptr, *shared, SH_NODE_TEX_COORD);
  ASSERT_NE(shared_output, nullptr);
  ASSERT_NE(shared_tex, nullptr);
  ASSERT_NE(shared_coord, nullptr);
  nodes::update_node_declaration_and_sockets(*shared, *shared_output);
  bke::node_add_link(
      *shared, *shared_coord, *sock_out(*shared_coord, "UV"), *shared_tex, *sock_in(*shared_tex, "Vector"));
  BKE_ntree_update_tag_all(shared);
  BKE_ntree_update_after_single_tree_change(*bmain, *shared);
  bke::node_add_link(*shared,
                     *shared_tex,
                     *sock_out(*shared_tex, "Color"),
                     *shared_output,
                     *sock_in_named(*shared_output, "Color"));
  BKE_ntree_update_tag_all(shared);
  BKE_ntree_update_after_single_tree_change(*bmain, *shared);
  /* The source instantiates it into Base Color. */
  bNode *shared_instance = bke::node_add_node(nullptr, src_tree, "ShaderNodeGroup"_ustr);
  ASSERT_NE(shared_instance, nullptr);
  shared_instance->id = &shared->id;
  id_us_plus(&shared->id);
  nodes::update_node_declaration_and_sockets(src_tree, *shared_instance);
  bke::node_add_link(src_tree,
                     *shared_instance,
                     *sock_out_named(*shared_instance, "Color"),
                     *principled,
                     *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);
  const int shared_nodes = shared->nodes.count();
  const uint64_t shared_topology = BKE_paint_layers_source_material_topology_hash(*source);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  /* Without the mapping: the shared group stays shared. */
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();
  bNode *copied_instance = node_by_name(*group, shared_instance->name);
  ASSERT_NE(copied_instance, nullptr);
  EXPECT_EQ(copied_instance->id, &shared->id);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
  const int users_shared = shared->id.us;

  /* With the mapping: a private copy carries the Mapping, the shared group is untouched. */
  bNodeTree *mapped = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(mapped, nullptr);
  mapped->ensure_topology_cache();
  copied_instance = node_by_name(*mapped, shared_instance->name);
  ASSERT_NE(copied_instance, nullptr);
  ASSERT_NE(copied_instance->id, &shared->id);
  bNodeTree *private_copy = reinterpret_cast<bNodeTree *>(copied_instance->id);
  ASSERT_NE(private_copy, nullptr);
  EXPECT_TRUE(StringRef(private_copy->id.name + 2).startswith(".PL Source MapCowSource PBR texture"));
  EXPECT_EQ(bke::paint_layers::prop_int_get(
                private_copy->id.properties, bke::paint_layers::TREE_SOURCE_PROP, 0),
            int(source->id.session_uid));
  private_copy->ensure_topology_cache();
  EXPECT_EQ(count_type(*private_copy, SH_NODE_MAPPING), 1);
  /* The shared group lost only the wrapper's reference: the source still holds its own. */
  EXPECT_EQ(shared->id.us, users_shared - 1);
  EXPECT_EQ(shared->nodes.count(), shared_nodes);
  EXPECT_EQ(BKE_paint_layers_source_material_topology_hash(*source), shared_topology);
  /* The private copy's Mapping sits between the coordinate and the texture, fed by the copy's
   * interface inputs. */
  EXPECT_NE(wrapper_mapping_input(*private_copy, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET), nullptr);
  /* The parent relays its own input into the copy's, each socket found in its own tree's
   * interface (the copy's identifier is not the parent's). */
  bNodeTreeInterfaceSocket *root_offset = wrapper_mapping_input(
      *mapped, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET);
  bNodeTreeInterfaceSocket *copy_offset = wrapper_mapping_input(
      *private_copy, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET);
  ASSERT_NE(root_offset, nullptr);
  ASSERT_NE(copy_offset, nullptr);
  bNodeSocket *relay_dst = bke::node_find_socket(
      *copied_instance, SOCK_IN, UString::from_ptr_noinline(copy_offset->identifier));
  ASSERT_NE(relay_dst, nullptr);
  int relay_feeds = 0;
  for (const bNodeLink &link : mapped->links) {
    if (link.tosock == relay_dst) {
      relay_feeds++;
      EXPECT_TRUE(link.fromnode->is_group_input());
      EXPECT_STREQ(link.fromsock->identifier, root_offset->identifier);
    }
  }
  EXPECT_EQ(relay_feeds, 1);
  bNode *copy_coord = node_by_name(*private_copy, shared_coord->name);
  ASSERT_NE(copy_coord, nullptr);
  ASSERT_EQ(outgoing_links(*private_copy, *copy_coord, "UV").size(), 1);
  EXPECT_EQ(outgoing_links(*private_copy, *copy_coord, "UV")[0]->tonode->type_legacy,
            SH_NODE_MAPPING);

  /* Back without the mapping: the wrapper rebuilds, the private copy has no user and is gone. */
  bNodeTree *plain = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(plain, nullptr);
  plain->ensure_topology_cache();
  copied_instance = node_by_name(*plain, shared_instance->name);
  ASSERT_NE(copied_instance, nullptr);
  EXPECT_EQ(copied_instance->id, &shared->id);
  EXPECT_EQ(source_wrapper_trees(*bmain).size(), 1);
  EXPECT_EQ(shared->id.us, users_shared);
}

namespace {
/* Defined further down with the other source builders. */
void source_set_three_image_base_color(Material &source, Image &a, Image &b, Image &c);
}  // namespace

/** Stage 2: a forced bake keeps the row on its raw maps -- no Mapping anywhere, and the report
 * says the mapping was ignored. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_forced_bake_ignores_it_never_bake_keeps_it)
{
  Material *source = add_principled_source("MapForceSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("MapForceA"), *add_image("MapForceB"), *add_image("MapForceC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*row), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("MapForceBaked")));
  BKE_paint_layers_bake_finalize(*ma, *row);
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  const int live_count = BKE_paint_layers_sampler_count(*ma);
  ASSERT_GT(live_count, 2);

  BKE_paint_layers_sampler_budget_set(live_count - 1, live_count);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_EQ(report.material_rows.size(), 1u);
  EXPECT_TRUE(report.material_rows[0].mapping_ignored);
  /* The row's group builds no Mapping node and carries no mapping inputs: a Baked row ignores
   * the mapping entirely. */
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
  EXPECT_FALSE(interface_has_value_for(*group, row->marker, bke::paint_layers::ROLE_MAPPING_OFFSET));

  /* A BAKE_NEVER row is not a forced bake: a mapped row stays live and its mapping applies, so
   * nothing is reported as ignored. */
  BKE_paint_layers_sampler_budget_set(1000, 1000);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_NEVER));
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  ASSERT_EQ(report.material_rows.size(), 1u);
  EXPECT_FALSE(report.material_rows[0].mapping_ignored);
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(*ma, *row));
  bNodeTree *wrapper = find_source_wrapper(*bmain, "MapForceSource");
  ASSERT_NE(wrapper, nullptr);
  EXPECT_GT(count_type(*wrapper, SH_NODE_MAPPING), 0);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_AUTO));
}

/** Every node of \a group that instantiates \a wrapper. */
static Vector<bNode *> group_instances_of_all(bNodeTree &group, const bNodeTree &wrapper)
{
  Vector<bNode *> instances;
  for (bNode &node : group.nodes) {
    if (node.is_group() && node.id == &const_cast<bNodeTree &>(wrapper).id) {
      instances.append(&node);
    }
  }
  return instances;
}

/** The Group Input socket feeding the instance's mapping input \a name, or null when something
 * else (or nothing) does. */
static const bNodeSocket *instance_mapping_feed(bNodeTree &group, bNode &instance, const char *name)
{
  bNodeSocket *dst = instance_mapping_socket(instance, name);
  if (dst == nullptr) {
    return nullptr;
  }
  for (const bNodeLink &link : group.links) {
    if (link.tosock == dst) {
      return link.fromnode->is_group_input() ? link.fromsock : nullptr;
    }
  }
  return nullptr;
}

/** Restores the sampler budget a test lowered, even when an assertion returns early. */
struct SamplerBudgetGuard {
  ~SamplerBudgetGuard()
  {
    BKE_paint_layers_sampler_budget_set(1000, 1000);
  }
};

/** A source whose Base Color and Alpha read one unlinked image each: a Hybrid row's source. */
static Material *add_hybrid_map_source(PaintLayersGenerateTest &test,
                                       const char *name,
                                       Image *color_map,
                                       Image *alpha_map)
{
  Material *source = test.add_principled_source(name, 0.5f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = PaintLayersGenerateTest::principled_of(*source);
  const auto link_image = [&](Image *image, const char *input) {
    bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
    tex->id = &image->id;
    id_us_plus(&image->id);
    bke::node_add_link(src_tree,
                       *tex,
                       *sock_out(*tex, "Color"),
                       *principled,
                       *sock_in(*principled, input));
  };
  link_image(color_map, "Base Color");
  link_image(alpha_map, "Alpha");
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*test.bmain, src_tree);
  return source;
}

/** A Layer row and a Material correction (or mask item) over one SourceGroup source, each with
 * its own mapping offset: one wrapper and one Mapping inside it, two instances in the row's
 * group, and each instance's inputs fed by its own row's group inputs. */
static void expect_row_and_item_share_one_wrapper(PaintLayersGenerateTest &test, const bool mask)
{
  Material &ma = *test.ma;
  Material *source = add_texcoord_image_source(
      *test.bmain, "MapItemSource", test.add_image("MapItemSourceImg"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(ma, row, source));
  MaterialPaintLayer *item = BKE_paint_layers_correction_add(
      ma,
      row,
      mask ? MA_PAINT_LAYER_ROLE_MASK_ITEM : MA_PAINT_LAYER_ROLE_EFFECT,
      MA_PAINT_LAYER_SOURCE_MATERIAL,
      "Item");
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(ma, item, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, row, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, item, true));
  const float row_offset[2] = {0.1f, 0.0f};
  const float item_offset[2] = {0.3f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, row, row_offset));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, item, item_offset));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*test.bmain, ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_EQ(BKE_paint_layers_material_mode(ma, *item), PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(ma, *item));

  bNodeTree *wrapper = find_source_wrapper(*test.bmain, "MapItemSource");
  bNodeTree *group = PaintLayersGenerateTest::layer_tree_find(*test.bmain, "Source");
  ASSERT_NE(wrapper, nullptr);
  ASSERT_NE(group, nullptr);
  wrapper->ensure_topology_cache();
  group->ensure_interface_cache();
  /* One Mapping, inside the wrapper; the row's own tree never builds one in this mode. */
  EXPECT_EQ(PaintLayersGenerateTest::count_type(*wrapper, SH_NODE_MAPPING), 1);
  int own_mappings = 0;
  for (const bNode &node : group->nodes) {
    own_mappings += (node.type_legacy == SH_NODE_MAPPING) ? 1 : 0;
  }
  EXPECT_EQ(own_mappings, 0);
  EXPECT_TRUE(PaintLayersGenerateTest::interface_has_value_for(
      *group, row->marker, bke::paint_layers::ROLE_MAPPING_OFFSET));
  EXPECT_TRUE(PaintLayersGenerateTest::interface_has_value_for(
      *group, item->marker, bke::paint_layers::ROLE_MAPPING_OFFSET));

  const Vector<bNode *> instances = group_instances_of_all(*group, *wrapper);
  ASSERT_EQ(instances.size(), 2);
  for (const char *name : {".PL Mapping Offset", ".PL Mapping Scale", ".PL Mapping Rotation"}) {
    const bNodeSocket *feed_a = instance_mapping_feed(*group, *instances[0], name);
    const bNodeSocket *feed_b = instance_mapping_feed(*group, *instances[1], name);
    ASSERT_NE(feed_a, nullptr) << name;
    ASSERT_NE(feed_b, nullptr) << name;
    /* Each row's own value input: the two instances never share one. */
    EXPECT_STRNE(feed_a->identifier, feed_b->identifier) << name;
  }
}

/** Stage B: a Layer row and a Material correction over one source share the wrapper but keep
 * their own mapping values. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_layer_and_correction_share_one_wrapper)
{
  expect_row_and_item_share_one_wrapper(*this, false);
}

/** Stage B: the same for a Material mask item. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_layer_and_mask_share_one_wrapper)
{
  expect_row_and_item_share_one_wrapper(*this, true);
}

/** Every Image Texture of \a group reading one of \a image_prefix's images, with the Mapping
 * nodes it reads through: expects each to read a Mapping Vector and to repeat. */
static int expect_hybrid_maps_read_through_mapping(bNodeTree &group, const char *image_prefix)
{
  int through = 0;
  for (bNode &node : group.nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE || node.id == nullptr ||
        !STRPREFIX(node.id->name + 2, image_prefix))
    {
      continue;
    }
    const bNodeSocket *vector = bke::node_find_socket(node, SOCK_IN, "Vector"_ustr);
    int links = 0;
    for (const bNodeLink &link : group.links) {
      if (link.tosock == vector) {
        links++;
        EXPECT_EQ(link.fromnode->type_legacy, SH_NODE_MAPPING);
      }
    }
    EXPECT_EQ(links, 1);
    EXPECT_EQ(static_cast<NodeTexImage *>(node.storage)->extension, SHD_IMAGE_EXTENSION_REPEAT);
    through += links;
  }
  return through;
}

/** Stage B: a Hybrid Material correction builds exactly one Mapping in its owner's tree, and its
 * colour and alpha maps both read that Vector. */
TEST_F(PaintLayersGenerateTest, hybrid_correction_mapping_builds_one_node_for_color_and_alpha)
{
  Material *source = add_hybrid_map_source(
      *this, "HybCorrSource", add_image("HybCorrColor"), add_image("HybCorrAlpha"));
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "Corr");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::Hybrid);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  EXPECT_EQ(expect_hybrid_maps_read_through_mapping(*group, "HybCorrColor"), 1);
  EXPECT_EQ(expect_hybrid_maps_read_through_mapping(*group, "HybCorrAlpha"), 1);
  /* The correction's own values ride the owner group's inputs. */
  group->ensure_interface_cache();
  EXPECT_TRUE(interface_has_value_for(
      *group, correction->marker, bke::paint_layers::ROLE_MAPPING_OFFSET));
}

/** Stage B: the same for a Hybrid Material mask item (its colour map and its coverage map). */
TEST_F(PaintLayersGenerateTest, hybrid_mask_mapping_builds_one_node_for_color_and_alpha)
{
  Material *source = add_hybrid_map_source(
      *this, "HybMaskSource", add_image("HybMaskColor"), add_image("HybMaskAlpha"));
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mask");
  ASSERT_NE(mask, nullptr);
  /* The colour map is read only when the mask reads a colour channel; Alpha reads the coverage. */
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *mask), PaintLayerMaterialMode::Hybrid);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, mask, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Owner");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  EXPECT_EQ(expect_hybrid_maps_read_through_mapping(*group, "HybMaskColor"), 1);
  EXPECT_EQ(expect_hybrid_maps_read_through_mapping(*group, "HybMaskAlpha"), 1);
}

/** Stage B: a Material correction's slider edits are value-only, and its enabled toggle is the one
 * topology change, both ways. */
TEST_F(PaintLayersGenerateTest, hybrid_correction_mapping_values_are_value_only_enabled_is_topology)
{
  Material *source = add_hybrid_map_source(
      *this, "HybHashSource", add_image("HybHashColor"), add_image("HybHashAlpha"));
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "Corr");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const auto hash = [&]() {
    return paint_layers_layer_topology_hash(*ma, *owner, Span<int>(wired, 1));
  };
  /* Regenerate before each reading: the hash reads state the pass settles (modes, caches). */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t hash_plain = hash();

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BAKE_STALE);
  const uint64_t hash_mapped = hash();
  EXPECT_NE(hash_mapped, hash_plain);

  const float offset[2] = {0.25f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, correction, offset));
  const float scale[2] = {2.0f, 3.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, correction, 0.5f));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(hash(), hash_mapped);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(hash(), hash_plain);
}

/** Stage B: a forced bake keeps a mapped Material correction on its raw maps -- no Mapping, no
 * applied mapping -- and BAKE_NEVER keeps it live and mapped. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_forced_bake_on_a_correction)
{
  const SamplerBudgetGuard budget_guard;
  Material *source = add_principled_source("MapCorrForceSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("MapCorrForceA"), *add_image("MapCorrForceB"), *add_image("MapCorrForceC"));
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "Corr");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*correction), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("MapCorrForceBaked")));
  BKE_paint_layers_bake_finalize(*ma, *correction);
  BKE_paint_layers_active_set(*ma, correction->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(*ma, *correction));
  const int live_count = BKE_paint_layers_sampler_count(*ma);
  ASSERT_GT(live_count, 2);

  /* The budget planner pins Layer rows only, so a correction's forced bake is set by hand: the
   * predicate every reader shares must then say the mapping does not apply. */
  bke::paint_layers::forced_bake_add(*ma, correction->marker);
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *correction));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::Baked);
  EXPECT_FALSE(BKE_paint_layers_mapping_applies(*ma, *correction));
  bke::paint_layers::forced_bake_clear(*ma);
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(*ma, *correction));

  /* BAKE_NEVER is not a forced bake: the row stays live and keeps its mapping. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *correction, MA_PAINT_LAYER_BAKE_NEVER));
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *correction));
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(*ma, *correction));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *correction, MA_PAINT_LAYER_BAKE_AUTO));
}

/** The wrapper interface inputs carrying \a role, so a toggle can be checked for duplicates. */
static int wrapper_role_input_count(bNodeTree &tree, const char *role)
{
  tree.ensure_interface_cache();
  int count = 0;
  for (bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    const IDProperty *marker = socket->properties != nullptr ?
                                   IDP_GetPropertyTypeFromGroup(
                                       socket->properties, bke::paint_layers::INPUT_ROLE_PROP, IDP_STRING) :
                                   nullptr;
    if (marker != nullptr && STREQ(IDP_string_get(marker), role)) {
      count++;
    }
  }
  return count;
}

/** Stage 2: the row group's mapping inputs reach the wrapper instance's inputs of the same role,
 * each side found by its own interface -- the identifiers are different trees' own. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_row_values_reach_the_wrapper_by_role)
{
  Material *source = add_texcoord_image_source(*bmain, "MapRoleSource", add_image("MapRoleSourceImg"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  const float offset[2] = {0.25f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, row, offset));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *wrapper = find_source_wrapper(*bmain, "MapRoleSource");
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(wrapper, nullptr);
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  bNode *instance = group_instance_of(*group, *wrapper);
  ASSERT_NE(instance, nullptr);
  char marker_text[UUID_STRING_SIZE];
  BLI_uuid_format(marker_text, row->marker);
  const char *const pairs[3][2] = {
      {bke::paint_layers::ROLE_MAPPING_OFFSET, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET},
      {bke::paint_layers::ROLE_MAPPING_SCALE, bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_SCALE},
      {bke::paint_layers::ROLE_MAPPING_ROTATION,
       bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_ROTATION},
  };
  for (const auto &pair : pairs) {
    const bNodeTreeInterfaceSocket *row_iface = nullptr;
    for (const bNodeTreeInterfaceSocket *socket : group->interface_inputs()) {
      const IDProperty *role_prop = socket->properties != nullptr ?
                                        IDP_GetPropertyTypeFromGroup(
                                            socket->properties, "pbr_paint_layers_role", IDP_STRING) :
                                        nullptr;
      const IDProperty *marker_prop = socket->properties != nullptr ?
                                          IDP_GetPropertyTypeFromGroup(
                                              socket->properties, "pbr_paint_layers_layer", IDP_STRING) :
                                          nullptr;
      if (role_prop != nullptr && STREQ(IDP_string_get(role_prop), pair[0]) &&
          marker_prop != nullptr && STREQ(IDP_string_get(marker_prop), marker_text))
      {
        row_iface = socket;
      }
    }
    const bNodeTreeInterfaceSocket *wrapper_iface = wrapper_mapping_input(*wrapper, pair[1]);
    ASSERT_NE(row_iface, nullptr) << pair[0];
    ASSERT_NE(wrapper_iface, nullptr) << pair[1];
    bNodeSocket *dst = bke::node_find_socket(
        *instance, SOCK_IN, UString::from_ptr_noinline(wrapper_iface->identifier));
    ASSERT_NE(dst, nullptr) << pair[1];
    int feeds = 0;
    for (const bNodeLink &link : group->links) {
      if (link.tosock != dst) {
        continue;
      }
      feeds++;
      EXPECT_TRUE(link.fromnode->is_group_input());
      EXPECT_STREQ(link.fromsock->identifier, row_iface->identifier) << pair[0];
    }
    EXPECT_EQ(feeds, 1) << pair[1];
  }
}

/** Stage 2: two rows over one source, the mapping on the second only -- the one wrapper still
 * carries the Mapping and the mapped row feeds it, whichever row the build meets first. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_only_on_the_second_row_still_builds_it)
{
  Material *source = add_texcoord_image_source(*bmain, "MapSecondSource", add_image("MapSecondSourceImg"));
  MaterialPaintLayer *row_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "SourceA", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row_a, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row_a, source));
  MaterialPaintLayer *row_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "SourceB", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row_b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row_b, source));
  /* Both orders: the mapped row is the one the build meets last, then first. */
  for (MaterialPaintLayer *mapped_row : {row_b, row_a}) {
    MaterialPaintLayer *plain_row = (mapped_row == row_a) ? row_b : row_a;
    ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, plain_row, false));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, mapped_row, true));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    bNodeTree *wrapper = find_source_wrapper(*bmain, "MapSecondSource");
    ASSERT_NE(wrapper, nullptr);
    wrapper->ensure_topology_cache();
    EXPECT_EQ(count_type(*wrapper, SH_NODE_MAPPING), 1) << mapped_row->name;
    bNodeTree *mapped_group = layer_tree_find(*bmain, mapped_row->name);
    ASSERT_NE(mapped_group, nullptr);
    bNode *instance = group_instance_of(*mapped_group, *wrapper);
    ASSERT_NE(instance, nullptr);
    EXPECT_TRUE(instance_mapping_input_wired(*mapped_group, *instance, ".PL Mapping Offset"))
        << mapped_row->name;
  }
}

/** Stage 2: with no UV layer named, an implicitly-UV Image Texture of a mapped wrapper is fed by
 * the Mapping too (through a Texture Coordinate), not left outside the row mapping. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_empty_uv_name_maps_implicit_textures)
{
  Material *source = add_principled_source("MapEmptyUvSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNode *tex = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex, nullptr);
  bke::node_add_link(src_tree,
                     *tex,
                     *sock_out(*tex, "Color"),
                     *principled,
                     *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  BLI_strncpy(ma->paint_layers_uv_map, "", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  bNode *copied_tex = node_by_name(*group, tex->name);
  ASSERT_NE(copied_tex, nullptr);
  bNodeSocket *vector = sock_in(*copied_tex, "Vector");
  ASSERT_NE(vector, nullptr);
  const bNodeLink *feed = nullptr;
  int feeds = 0;
  for (const bNodeLink &link : group->links) {
    if (link.tosock == vector) {
      feed = &link;
      feeds++;
    }
  }
  ASSERT_EQ(feeds, 1);
  ASSERT_EQ(feed->fromnode->type_legacy, SH_NODE_MAPPING);
  bNodeSocket *mapping_vector = sock_in(*feed->fromnode, "Vector");
  ASSERT_NE(mapping_vector, nullptr);
  int mapping_feeds = 0;
  for (const bNodeLink &link : group->links) {
    if (link.tosock == mapping_vector) {
      mapping_feeds++;
      EXPECT_EQ(link.fromnode->type_legacy, SH_NODE_TEX_COORD);
    }
  }
  EXPECT_EQ(mapping_feeds, 1);

  /* Unmapped, the same wrapper keeps the textures on the render UV: no node is added. */
  bNodeTree *plain = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(count_type(*plain, SH_NODE_MAPPING), 0);
  EXPECT_EQ(count_type(*plain, SH_NODE_TEX_COORD), 0);
}

/** Stage 2: toggling the mapping on, off and on leaves exactly one wrapper input per role, and
 * none while it is off. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_toggle_keeps_one_wrapper_input_per_role)
{
  Material *source = add_texcoord_image_source(*bmain, "MapToggleSource", add_image("MapToggleSourceImg"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  const char *const roles[3] = {bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET,
                                bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_SCALE,
                                bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_ROTATION};
  for (const bool enabled : {true, false, true}) {
    ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, enabled));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    bNodeTree *wrapper = find_source_wrapper(*bmain, "MapToggleSource");
    ASSERT_NE(wrapper, nullptr);
    for (const char *role : roles) {
      EXPECT_EQ(wrapper_role_input_count(*wrapper, role), enabled ? 1 : 0) << role;
    }
    /* No instance keeps a socket the wrapper no longer declares, and no link points at one. */
    bNodeTree *group = layer_tree_find(*bmain, "Source");
    ASSERT_NE(group, nullptr);
    bNode *instance = group_instance_of(*group, *wrapper);
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance_mapping_socket(*instance, ".PL Mapping Offset") != nullptr, enabled);
    for (const bNodeLink &link : group->links) {
      EXPECT_NE(link.tosock, nullptr);
      EXPECT_NE(link.fromsock, nullptr);
    }
  }
}

/** Stage 2: a value edited inside a shared off-path group of the source reaches its private copy
 * through the in-place value sync -- no rebuild -- and the original group is not written. */
TEST_F(PaintLayersGenerateTest, source_group_mapping_value_sync_reaches_private_off_path_copy)
{
  Material *source = add_principled_source("MapSyncSource", 0.3f);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNodeTree *shared = bke::node_tree_add_tree(bmain, "PBR sync", "ShaderNodeTree");
  ASSERT_NE(shared, nullptr);
  ASSERT_NE(shared->tree_interface.add_socket(
                "Color", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr),
            nullptr);
  bNode *shared_output = bke::node_add_node(nullptr, *shared, "NodeGroupOutput"_ustr);
  bNode *shared_tex = bke::node_add_static_node(nullptr, *shared, SH_NODE_TEX_IMAGE);
  bNode *shared_coord = bke::node_add_static_node(nullptr, *shared, SH_NODE_TEX_COORD);
  bNode *shared_rgb = bke::node_add_static_node(nullptr, *shared, SH_NODE_RGB);
  ASSERT_NE(shared_output, nullptr);
  ASSERT_NE(shared_tex, nullptr);
  ASSERT_NE(shared_coord, nullptr);
  ASSERT_NE(shared_rgb, nullptr);
  nodes::update_node_declaration_and_sockets(*shared, *shared_output);
  bke::node_add_link(
      *shared, *shared_coord, *sock_out(*shared_coord, "UV"), *shared_tex, *sock_in(*shared_tex, "Vector"));
  bke::node_add_link(*shared,
                     *shared_tex,
                     *sock_out(*shared_tex, "Color"),
                     *shared_output,
                     *sock_in_named(*shared_output, "Color"));
  BKE_ntree_update_tag_all(shared);
  BKE_ntree_update_after_single_tree_change(*bmain, *shared);
  bNode *shared_instance = bke::node_add_node(nullptr, src_tree, "ShaderNodeGroup"_ustr);
  ASSERT_NE(shared_instance, nullptr);
  shared_instance->id = &shared->id;
  id_us_plus(&shared->id);
  nodes::update_node_declaration_and_sockets(src_tree, *shared_instance);
  bke::node_add_link(src_tree,
                     *shared_instance,
                     *sock_out_named(*shared_instance, "Color"),
                     *principled,
                     *sock_in(*principled, "Base Color"));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, nullptr, nullptr, true);
  ASSERT_NE(group, nullptr);
  bNode *copied_instance = node_by_name(*group, shared_instance->name);
  ASSERT_NE(copied_instance, nullptr);
  ASSERT_NE(copied_instance->id, &shared->id);
  bNodeTree *private_copy = id_cast<bNodeTree *>(copied_instance->id);
  bNode *copied_rgb = node_by_name(*private_copy, shared_rgb->name);
  ASSERT_NE(copied_rgb, nullptr);

  /* The edit lands in the shared group the user owns; only the wrapper's copy has to follow. */
  bNodeSocket *shared_color = sock_out(*shared_rgb, "Color");
  ASSERT_NE(shared_color, nullptr);
  float *shared_value = static_cast<bNodeSocketValueRGBA *>(shared_color->default_value)->value;
  shared_value[0] = 0.125f;
  shared_value[1] = 0.25f;
  shared_value[2] = 0.5f;
  const int shared_nodes = shared->nodes.count();

  bool changed = true;
  bool values_synced = false;
  bNodeTree *again = BKE_paint_layers_source_group_ensure(
      *bmain, *ma, *source, refusal, &changed, &values_synced, true);
  EXPECT_EQ(again, group);
  EXPECT_FALSE(changed);
  EXPECT_TRUE(values_synced);
  const float *copy_value = static_cast<const bNodeSocketValueRGBA *>(
                                sock_out(*copied_rgb, "Color")->default_value)->value;
  EXPECT_FLOAT_EQ(copy_value[0], 0.125f);
  EXPECT_FLOAT_EQ(copy_value[1], 0.25f);
  EXPECT_FLOAT_EQ(copy_value[2], 0.5f);
  /* Still the same private copy, and the original group is exactly as the user left it. */
  EXPECT_EQ(node_by_name(*group, shared_instance->name)->id, &private_copy->id);
  EXPECT_EQ(shared->nodes.count(), shared_nodes);
  EXPECT_FLOAT_EQ(shared_value[0], 0.125f);
}

/** \} */

}  // namespace blender::bke::tests
