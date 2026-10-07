/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: interpreter, fixtures and multi-source basics (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


TEST_F(PaintLayersGraphEvalTest, compose_color_alpha_uses_linked_color_and_alpha)
{
  bNodeTree *tree = bke::node_tree_add_tree(bmain, "ComposeColorAlpha", "ShaderNodeTree");
  ASSERT_NE(tree, nullptr);

  bNode *rgb = bke::node_add_static_node(nullptr, *tree, SH_NODE_RGB);
  bNode *value = bke::node_add_static_node(nullptr, *tree, SH_NODE_VALUE);
  bNode *compose = bke::node_add_static_node(nullptr, *tree, SH_NODE_COMPOSE_COLOR_ALPHA);
  ASSERT_NE(rgb, nullptr);
  ASSERT_NE(value, nullptr);
  ASSERT_NE(compose, nullptr);

  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  bNodeSocket *value_out = bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr);
  bNodeSocket *color_in = bke::node_find_socket(*compose, SOCK_IN, "Color"_ustr);
  bNodeSocket *alpha_in = bke::node_find_socket(*compose, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(rgb_out, nullptr);
  ASSERT_NE(value_out, nullptr);
  ASSERT_NE(color_in, nullptr);
  ASSERT_NE(alpha_in, nullptr);

  const float color[4] = {0.2f, 0.5f, 0.9f, 0.1f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value, color);
  static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value = 0.3f;
  bke::node_add_link(*tree, *rgb, *rgb_out, *compose, *color_in);
  bke::node_add_link(*tree, *value, *value_out, *compose, *alpha_in);
  tree->ensure_topology_cache();

  GraphInterpreter interpreter;
  interpreter.tree = tree;
  const RGBA result = interpreter.eval_output(*compose, "Color");
  EXPECT_FLOAT_EQ(result.r, color[0]);
  EXPECT_FLOAT_EQ(result.g, color[1]);
  EXPECT_FLOAT_EQ(result.b, color[2]);
  EXPECT_FLOAT_EQ(result.a, 0.3f);
}

/** Stage 2: the interpreter reads a mapped chain (UV Map -> Mapping -> Image Texture) at the
 * vector the mapping produces, with the Repeat wrap, instead of the direct texel. */
TEST_F(PaintLayersGraphEvalTest, mapping_node_feeds_the_image_texture_vector)
{
  bNodeTree *tree = bke::node_tree_add_tree(bmain, "MappedChain", "ShaderNodeTree");
  ASSERT_NE(tree, nullptr);

  /* A 2x2 Non-Color map whose bottom-left texel is red, the rest green. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *map = BKE_image_add_generated(
      bmain, 2, 2, "MappedChainImg", 32, false, IMA_GENTYPE_BLANK, black, false, true, false);
  ASSERT_NE(map, nullptr);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(map, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    const uchar colors[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 255, 0, 255}, {0, 255, 0, 255}};
    for (const int i : IndexRange(4)) {
      memcpy(pixels + i * 4, colors[i], 4);
    }
    BKE_image_release_ibuf(map, ibuf, lock);
  }
  make_image_data(map);

  bNode *uv_map = bke::node_add_static_node(nullptr, *tree, SH_NODE_UVMAP);
  bNode *mapping = bke::node_add_static_node(nullptr, *tree, SH_NODE_MAPPING);
  bNode *tex = bke::node_add_static_node(nullptr, *tree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(uv_map, nullptr);
  ASSERT_NE(mapping, nullptr);
  ASSERT_NE(tex, nullptr);
  mapping->custom1 = NODE_MAPPING_TYPE_POINT;
  tex->id = &map->id;
  id_us_plus(&map->id);
  bke::node_add_link(*tree,
                     *uv_map,
                     *bke::node_find_socket(*uv_map, SOCK_OUT, "UV"_ustr),
                     *mapping,
                     *bke::node_find_socket(*mapping, SOCK_IN, "Vector"_ustr));
  bke::node_add_link(*tree,
                     *mapping,
                     *bke::node_find_socket(*mapping, SOCK_OUT, "Vector"_ustr),
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_IN, "Vector"_ustr));
  tree->ensure_topology_cache();

  GraphInterpreter interpreter;
  interpreter.tree = tree;
  /* With no ref grid the UV Map node reads (0, 0), so the Mapping output is its offset alone:
   * (0.25, 0.25) lands exactly on the bottom-left texel of the 2x2 map. */
  copy_v3_fl3(static_cast<bNodeSocketValueVector *>(
                  bke::node_find_socket(*mapping, SOCK_IN, "Location"_ustr)->default_value)
                  ->value,
              0.25f,
              0.25f,
              0.0f);
  const RGBA mapped = interpreter.eval_output(*tex, "Color");
  EXPECT_NEAR(mapped.r, 1.0f, 1e-5f);
  EXPECT_NEAR(mapped.g, 0.0f, 1e-5f);
  EXPECT_NEAR(mapped.b, 0.0f, 1e-5f);

  /* Repeat: offset (1.25, 0.25) wraps to the same texel. */
  copy_v3_fl3(static_cast<bNodeSocketValueVector *>(
                  bke::node_find_socket(*mapping, SOCK_IN, "Location"_ustr)->default_value)
                  ->value,
              1.25f,
              0.25f,
              0.0f);
  const RGBA wrapped = interpreter.eval_output(*tex, "Color");
  EXPECT_NEAR(wrapped.r, 1.0f, 1e-5f);
  EXPECT_NEAR(wrapped.g, 0.0f, 1e-5f);

  /* Offset (0.75, 0.25) reads the bottom-right texel: green. */
  copy_v3_fl3(static_cast<bNodeSocketValueVector *>(
                  bke::node_find_socket(*mapping, SOCK_IN, "Location"_ustr)->default_value)
                  ->value,
              0.75f,
              0.25f,
              0.0f);
  const RGBA shifted = interpreter.eval_output(*tex, "Color");
  EXPECT_NEAR(shifted.g, 1.0f, 1e-5f);
  EXPECT_NEAR(shifted.r, 0.0f, 1e-5f);
}

/**
 * A source whose Principled sits in a nested group fed through the group's interface: the root's
 * RGB and Value nodes reach the group inputs through Reroutes, Metallic is an instance value, and
 * the group output feeds the Material Output. The earlier helpers fed the Principled constants, so
 * a group interface lost in the wrapper copy stayed invisible.
 */
static Material *make_interface_wired_source(Main &bmain, const char *name)
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

/**
 * \a source with the Principled inside a nested group whose interface also declares an input named
 * "Specular" (the same name the wrapper gives its output). The Specular IOR Level is either linked
 * through that Group Input (its instance value 0.25) or left as an unlinked 0.25.
 */
static Material *make_specular_source(Main &bmain, const char *name, const bool linked)
{
  Material *source = make_interface_wired_source(bmain, name);
  bNodeTree *group = nullptr;
  bNode *group_instance = nullptr;
  for (bNode &node : source->nodetree->nodes) {
    if (node.is_group() && node.id != nullptr) {
      group = id_cast<bNodeTree *>(node.id);
      group_instance = &node;
    }
  }
  if (group == nullptr || group_instance == nullptr) {
    return source;
  }
  bNode *group_input = nullptr;
  bNode *principled = nullptr;
  for (bNode &node : group->nodes) {
    if (node.is_group_input()) {
      group_input = &node;
    }
    else if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
    }
  }
  if (group_input == nullptr || principled == nullptr) {
    return source;
  }
  bNodeTreeInterfaceSocket *spec_in = group->tree_interface.add_socket(
      "Specular", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  nodes::update_node_declaration_and_sockets(*group, *group_input);
  nodes::update_node_declaration_and_sockets(*group, *group_instance);
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);

  bNodeSocket *specular = bke::node_find_socket(*principled, SOCK_IN, "Specular IOR Level"_ustr);
  if (specular == nullptr) {
    return source;
  }
  if (linked && spec_in != nullptr && spec_in->identifier != nullptr) {
    bNodeSocket *instance_in = bke::node_find_socket(
        *group_instance, SOCK_IN, UString::from_ptr_noinline(spec_in->identifier));
    if (instance_in != nullptr && instance_in->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(instance_in->default_value)->value = 0.25f;
    }
    bNodeSocket *input_out = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(spec_in->identifier));
    if (input_out != nullptr) {
      bke::node_add_link(*group, *group_input, *input_out, *principled, *specular);
    }
  }
  else {
    static_cast<bNodeSocketValueFloat *>(specular->default_value)->value = 0.25f;
  }
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return source;
}

/* -------------------------------------------------------------------- */
/** \name Reusable Material sources for the multi-row stack tests
 *
 * A and C put the Principled inside a nested group and drive it through the group interface, a
 * Reroute, an Image Texture and a Color Ramp, so every wired channel resolves to a graph and the
 * row must use the SourceGroup wrapper. B keeps the Principled at the top level with constant
 * sockets, a live Alpha and a Normal Map over a flat data texture: every channel is a constant or
 * a trivial map, so the row uses Hybrid.
 * \{ */

/* NOTE: the specs, their constants and the two forward declarations above live in
 * `paint_layers_graph_eval_test_fixture.hh` (plan 9.3); only TU-local helpers stay here. */

/** Configure a Color Ramp node into a two-stop linear ramp whose alpha runs \a a0 -> \a a1. */
static void configure_color_ramp(bNode &node, const float a0, const float a1)
{
  ColorBand *band = static_cast<ColorBand *>(node.storage);
  if (band == nullptr) {
    return;
  }
  band->tot = 2;
  band->ipotype = COLBAND_INTERP_LINEAR;
  band->color_mode = COLBAND_BLEND_RGB;
  band->data[0].pos = 0.0f;
  band->data[0].r = 0.0f;
  band->data[0].g = 0.0f;
  band->data[0].b = 0.0f;
  band->data[0].a = a0;
  band->data[1].pos = 1.0f;
  band->data[1].r = 1.0f;
  band->data[1].g = 1.0f;
  band->data[1].b = 1.0f;
  band->data[1].a = a1;
}

}  // namespace blender::bke::tests
