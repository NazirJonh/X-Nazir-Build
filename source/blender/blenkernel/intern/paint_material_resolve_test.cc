/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_index_range.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

namespace blender::bke::tests {

class PaintMaterialResolveTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  /** A material whose node tree holds a Principled linked straight to a Material Output. */
  Material *add_material_with_principled(const char *name)
  {
    Material *ma = BKE_material_add(bmain, name);
    bNodeTree &ntree = *ma->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(ntree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
    return ma;
  }

  bNode *find_node(Material &ma, const int type_legacy)
  {
    for (bNode &node : ma.nodetree->nodes) {
      if (node.type_legacy == type_legacy) {
        return &node;
      }
    }
    return nullptr;
  }

  bNode *find_output(Material &ma)
  {
    return this->find_node(ma, SH_NODE_OUTPUT_MATERIAL);
  }
};

TEST_F(PaintMaterialResolveTest, principled_found_when_linked_directly)
{
  Material *ma = this->add_material_with_principled("Mat");
  ChannelUnavailableReason reason = ChannelUnavailableReason::NoMaterial;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  EXPECT_NE(principled, nullptr);
  EXPECT_EQ(reason, ChannelUnavailableReason::None);
}

TEST_F(PaintMaterialResolveTest, no_node_tree_reports_no_node_tree)
{
  Material *ma = BKE_material_add(bmain, "Flat");
  ma->use_nodes = false;
  if (ma->nodetree != nullptr) {
    bke::node_tree_free_embedded_tree(ma->nodetree);
    MEM_delete(ma->nodetree);
    ma->nodetree = nullptr;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  EXPECT_EQ(BKE_paint_material_principled_find(*ma, reason), nullptr);
  EXPECT_EQ(reason, ChannelUnavailableReason::NoNodeTree);
}

TEST_F(PaintMaterialResolveTest, emission_shader_on_surface_reports_no_principled)
{
  Material *ma = BKE_material_add(bmain, "Emit");
  bNodeTree &ntree = *ma->nodetree;
  /* BKE_material_add already seeds a Principled; remove it so only Emission drives Surface. */
  if (bNode *seeded = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED)) {
    bke::node_remove_node(bmain, ntree, *seeded, false);
  }
  bNode *emission = bke::node_add_static_node(nullptr, ntree, SH_NODE_EMISSION);
  bNode *output = this->find_output(*ma);
  bke::node_add_link(ntree,
                     *emission,
                     *bke::node_find_socket(*emission, SOCK_OUT, "Emission"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  EXPECT_EQ(BKE_paint_material_principled_find(*ma, reason), nullptr);
  EXPECT_EQ(reason, ChannelUnavailableReason::NoPrincipled);
}

TEST_F(PaintMaterialResolveTest, reroute_between_principled_and_output_is_followed)
{
  Material *ma = this->add_material_with_principled("Rerouted");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = this->find_output(*ma);
  bNodeSocket &surface = *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr);
  /* Drop the direct link and route it through a Reroute instead. */
  bke::node_remove_socket_links(ntree, surface);
  bNode *reroute = bke::node_add_static_node(nullptr, ntree, SH_NODE_REROUTE);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *reroute,
                     *static_cast<bNodeSocket *>(reroute->inputs.first));
  bke::node_add_link(ntree,
                     *reroute,
                     *static_cast<bNodeSocket *>(reroute->outputs.first),
                     *output,
                     surface);
  ChannelUnavailableReason reason = ChannelUnavailableReason::NoMaterial;
  EXPECT_EQ(BKE_paint_material_principled_find(*ma, reason), principled);
  EXPECT_EQ(reason, ChannelUnavailableReason::None);
}

TEST_F(PaintMaterialResolveTest, mix_shader_before_output_reports_no_principled)
{
  Material *ma = this->add_material_with_principled("Mixed");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = this->find_output(*ma);
  bNodeSocket &surface = *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr);
  bke::node_remove_socket_links(ntree, surface);
  bNode *mix = bke::node_add_static_node(nullptr, ntree, SH_NODE_MIX_SHADER);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *mix,
                     *static_cast<bNodeSocket *>(BLI_findlink(&mix->inputs, 1)));
  bke::node_add_link(ntree,
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_OUT, "Shader"_ustr),
                     *output,
                     surface);
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  EXPECT_EQ(BKE_paint_material_principled_find(*ma, reason), nullptr);
  EXPECT_EQ(reason, ChannelUnavailableReason::NoPrincipled);
}

TEST_F(PaintMaterialResolveTest, second_unlinked_output_does_not_make_it_ambiguous)
{
  Material *ma = this->add_material_with_principled("TwoOutputs");
  bNodeTree &ntree = *ma->nodetree;
  /* A stray, non-active output must not defeat resolution: only the active one counts. */
  bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  ChannelUnavailableReason reason = ChannelUnavailableReason::NoMaterial;
  EXPECT_NE(BKE_paint_material_principled_find(*ma, reason), nullptr);
  EXPECT_EQ(reason, ChannelUnavailableReason::None);
}

/* ------------------------------------------------------------------ Task 2 tests */

TEST_F(PaintMaterialResolveTest, unlinked_socket_resolves_to_its_constant)
{
  Material *ma = this->add_material_with_principled("Constants");
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNodeSocket *metallic = bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr);
  static_cast<bNodeSocketValueFloat *>(metallic->default_value)->value = 0.75f;

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_METALLIC], ChannelResolution::Constant);
  EXPECT_FLOAT_EQ(resolve.constants[PAINT_MATERIAL_CHANNEL_METALLIC].x, 0.75f);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_METALLIC], ChannelUnavailableReason::None);
}

TEST_F(PaintMaterialResolveTest, unlinked_color_socket_keeps_all_three_components)
{
  Material *ma = this->add_material_with_principled("ColorConstant");
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  bNodeSocketValueRGBA *rgba = static_cast<bNodeSocketValueRGBA *>(base_color->default_value);
  rgba->value[0] = 0.1f;
  rgba->value[1] = 0.2f;
  rgba->value[2] = 0.3f;

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR], ChannelResolution::Constant);
  EXPECT_FLOAT_EQ(resolve.constants[PAINT_MATERIAL_CHANNEL_BASE_COLOR].x, 0.1f);
  EXPECT_FLOAT_EQ(resolve.constants[PAINT_MATERIAL_CHANNEL_BASE_COLOR].y, 0.2f);
  EXPECT_FLOAT_EQ(resolve.constants[PAINT_MATERIAL_CHANNEL_BASE_COLOR].z, 0.3f);
}

TEST_F(PaintMaterialResolveTest, linked_socket_resolves_to_baked)
{
  Material *ma = this->add_material_with_principled("Linked");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *noise = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_NOISE);
  bke::node_add_link(ntree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr));

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS], ChannelResolution::Baked);
}

TEST_F(PaintMaterialResolveTest, channels_without_socket_are_unavailable)
{
  Material *ma = this->add_material_with_principled("NoSockets");
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_CUSTOM,
                                              PAINT_MATERIAL_CHANNEL_HEIGHT,
                                              PAINT_MATERIAL_CHANNEL_AO})
  {
    EXPECT_EQ(resolve.channels[channel], ChannelResolution::Unavailable);
    EXPECT_EQ(resolve.reasons[channel], ChannelUnavailableReason::NoSocketForChannel);
  }
}

TEST_F(PaintMaterialResolveTest, null_material_marks_every_channel_unavailable)
{
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(nullptr);
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    EXPECT_EQ(resolve.channels[i], ChannelResolution::Unavailable);
    EXPECT_EQ(resolve.reasons[i], ChannelUnavailableReason::NoMaterial);
  }
}

TEST_F(PaintMaterialResolveTest, reroute_on_channel_input_without_source_is_constant)
{
  Material *ma = this->add_material_with_principled("DanglingReroute");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *reroute = bke::node_add_static_node(nullptr, ntree, SH_NODE_REROUTE);
  bke::node_add_link(ntree,
                     *reroute,
                     *static_cast<bNodeSocket *>(reroute->outputs.first),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));

  /* A reroute with nothing behind it drives nothing, so the socket's own value still applies. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_METALLIC], ChannelResolution::Constant);
}

/* ------------------------------------------------------------------ Task 3 tests */

/** Links a Normal Map node into the Principled's Normal input and returns it. */
static bNode *link_normal_map(bNodeTree &ntree, bNode &principled)
{
  bNode *normal_map = bke::node_add_static_node(nullptr, ntree, SH_NODE_NORMAL_MAP);
  bke::node_add_link(ntree,
                     *normal_map,
                     *bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr),
                     principled,
                     *bke::node_find_socket(principled, SOCK_IN, "Normal"_ustr));
  return normal_map;
}

TEST_F(PaintMaterialResolveTest, normal_through_tangent_space_normal_map_is_baked)
{
  Material *ma = this->add_material_with_principled("NormalOk");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *normal_map = link_normal_map(ntree, *principled);
  static_cast<NodeShaderNormalMap *>(normal_map->storage)->space = SHD_SPACE_TANGENT;

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Baked);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelUnavailableReason::None);
}

TEST_F(PaintMaterialResolveTest, normal_in_object_space_is_unavailable)
{
  Material *ma = this->add_material_with_principled("NormalObject");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *normal_map = link_normal_map(ntree, *principled);
  static_cast<NodeShaderNormalMap *>(normal_map->storage)->space = SHD_SPACE_OBJECT;

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Unavailable);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_NORMAL],
            ChannelUnavailableReason::NormalNotTangentSpace);
}

TEST_F(PaintMaterialResolveTest, normal_with_non_unit_strength_is_unavailable)
{
  Material *ma = this->add_material_with_principled("NormalStrength");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *normal_map = link_normal_map(ntree, *principled);
  static_cast<NodeShaderNormalMap *>(normal_map->storage)->space = SHD_SPACE_TANGENT;
  bNodeSocket *strength = bke::node_find_socket(*normal_map, SOCK_IN, "Strength"_ustr);
  static_cast<bNodeSocketValueFloat *>(strength->default_value)->value = 0.5f;

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Unavailable);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_NORMAL],
            ChannelUnavailableReason::NormalStrengthNotOne);
}

TEST_F(PaintMaterialResolveTest, normal_linked_without_normal_map_is_baked)
{
  Material *ma = this->add_material_with_principled("NormalDirect");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *noise = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_NOISE);
  bke::node_add_link(ntree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  /* Whatever the graph computes is the normal the material renders with, and on the bake quad
   * that vector already is the tangent-space map, so it can be baked. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Baked);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelUnavailableReason::None);
}

/** Links a Bump node into the Principled's Normal input and returns it. */
static bNode *link_bump(bNodeTree &ntree, bNode &principled)
{
  bNode *bump = bke::node_add_static_node(nullptr, ntree, SH_NODE_BUMP);
  bke::node_add_link(ntree,
                     *bump,
                     *bke::node_find_socket(*bump, SOCK_OUT, "Normal"_ustr),
                     principled,
                     *bke::node_find_socket(principled, SOCK_IN, "Normal"_ustr));
  return bump;
}

TEST_F(PaintMaterialResolveTest, normal_through_bump_with_height_is_baked)
{
  Material *ma = this->add_material_with_principled("NormalBump");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  bNode *bump = link_bump(ntree, *principled);
  bNode *noise = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_NOISE);
  bke::node_add_link(ntree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *bump,
                     *bke::node_find_socket(*bump, SOCK_IN, "Height"_ustr));

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Baked);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelUnavailableReason::None);
}

TEST_F(PaintMaterialResolveTest, normal_through_bump_without_height_is_flat_constant)
{
  Material *ma = this->add_material_with_principled("NormalBumpEmpty");
  bNodeTree &ntree = *ma->nodetree;
  bNode *principled = this->find_node(*ma, SH_NODE_BSDF_PRINCIPLED);
  link_bump(ntree, *principled);

  /* A constant Height has zero derivatives, so the node passes its Normal input through. Answering
   * with the flat map avoids spending a render on a uniform buffer. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Constant);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelUnavailableReason::None);
  EXPECT_FLOAT_EQ(resolve.constants[PAINT_MATERIAL_CHANNEL_NORMAL].x, 0.5f);
  EXPECT_FLOAT_EQ(resolve.constants[PAINT_MATERIAL_CHANNEL_NORMAL].y, 0.5f);
  EXPECT_FLOAT_EQ(resolve.constants[PAINT_MATERIAL_CHANNEL_NORMAL].z, 1.0f);
}

TEST_F(PaintMaterialResolveTest, normal_unlinked_stays_unavailable_not_constant)
{
  Material *ma = this->add_material_with_principled("NormalUnlinked");
  /* A flat tangent is not something the user asked to paint; treating it as a constant would
   * silently overwrite the target normal map with flat pixels. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
  EXPECT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Unavailable);
  EXPECT_EQ(resolve.reasons[PAINT_MATERIAL_CHANNEL_NORMAL],
            ChannelUnavailableReason::NormalNotThroughNormalMap);
}

}  // namespace blender::bke::tests
