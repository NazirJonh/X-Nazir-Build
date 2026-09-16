/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"

#include "RNA_access.hh"

#include "BLI_listbase.h"
#include "BLI_uuid.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_material_composite_internal.hh"
#include "paint_material_layer_edit_intern.hh"
#include "paint_material_layer_idprops.hh"

namespace blender::bke::tests {

class PaintMaterialLayerCorrectionTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *material = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    material = add_material_with_principled("Material");
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  Material *add_material_with_principled(const char *name)
  {
    Material *ma = BKE_material_add(bmain, name);
    bNodeTree &tree = *ma->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(tree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
    return ma;
  }

  /** Two layers wired on Base Color and Roughness; ordinal 1 is the top (Mix) layer. */
  void build_two_layer_stack(Material &ma, const int image_size = 8)
  {
    PaintMaterialLayerAddParams params;
    params.image_size = image_size;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, ma, params, nullptr, &error));
    ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, ma, params, nullptr, &error));
    const int channels[] = {PAINT_MATERIAL_CHANNEL_ROUGHNESS};
    ASSERT_TRUE(
        BKE_paint_material_layer_channels_ensure(*bmain, ma, Span<int>(channels, 1), &error));
  }

  void build_two_layer_stack()
  {
    build_two_layer_stack(*material);
  }

  Vector<PaintMaterialLayerStackEntry> entries()
  {
    Vector<PaintMaterialLayerStackEntry> r_entries;
    BKE_paint_material_layer_stack_from_material(*bmain, *material, r_entries);
    return r_entries;
  }
};

/** Insert one Content correction with \a marker into every wired channel of layer 1. */
static void insert_into_all_channels(Main &bmain,
                                     Material &ma,
                                     const bUUID &marker,
                                     PaintMaterialCorrectionSection section)
{
  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(chains_collect(ma, chains, error));
  for (ChannelChain &chain : chains) {
    ChainCorrection nodes;
    ASSERT_TRUE(correction_channel_insert(
        bmain, chain, chain.layers[1], section, PaintMaterialCorrectionEffect::Paint, marker, nodes));
    ma.nodetree->ensure_topology_cache();
  }
}

TEST_F(PaintMaterialLayerCorrectionTest, correction_idprops_roundtrip)
{
  bNode *mix = bke::node_add_static_node(nullptr, *material->nodetree, SH_NODE_MIX);
  EXPECT_FALSE(bke::paint_layer::node_is_correction(*mix));
  EXPECT_EQ(bke::paint_layer::correction_section_get(*mix),
            PaintMaterialCorrectionSection::Content);

  bke::paint_layer::kind_set(*mix, PaintMaterialLayerKind::Correction);
  bke::paint_layer::correction_section_set(*mix, PaintMaterialCorrectionSection::Mask);
  bke::paint_layer::correction_effect_set(*mix, PaintMaterialCorrectionEffect::Paint);

  EXPECT_TRUE(bke::paint_layer::node_is_correction(*mix));
  EXPECT_EQ(bke::paint_layer::correction_section_get(*mix), PaintMaterialCorrectionSection::Mask);
  EXPECT_EQ(bke::paint_layer::correction_effect_get(*mix), PaintMaterialCorrectionEffect::Paint);
}

TEST_F(PaintMaterialLayerCorrectionTest, channel_insert_builds_absent_content_shape)
{
  build_two_layer_stack();
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);
  BKE_paint_material_layer_markers_ensure(*material);

  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  ChannelChain &chain = chains.first();
  ChainLayer &layer = chain.layers[1];

  const bUUID marker = BLI_uuid_generate_random();
  ChainCorrection nodes;
  ASSERT_TRUE(correction_channel_insert(
      *bmain,
      chain,
      layer,
      PaintMaterialCorrectionSection::Content,
      PaintMaterialCorrectionEffect::Paint,
      marker,
      nodes));
  material->nodetree->ensure_topology_cache();

  EXPECT_TRUE(bke::paint_layer::node_is_correction(*nodes.mix));
  EXPECT_TRUE(BLI_uuid_equal(bke::paint_layer::marker_get(*nodes.mix), marker));
  EXPECT_EQ(nodes.map, nullptr);

  CompositeMixNode corr;
  ASSERT_TRUE(composite_mix_node_read(*nodes.mix, corr));
  /* The correction feeds the layer's map input, and the layer's base feeds the correction. */
  CompositeMixNode layer_mix;
  ASSERT_TRUE(composite_mix_node_read(*layer.node, layer_mix));
  EXPECT_EQ(composite_source_node_shallow(*layer_mix.top), nodes.mix);
  EXPECT_EQ(composite_source_node_shallow(*corr.bottom)->type_legacy, SH_NODE_TEX_IMAGE);
  /* Absent: coverage unlinked and zero (I1 form on the correction, spec I3). */
  PaintMaterialLayerChannelState state = PaintMaterialLayerChannelState::Enabled;
  ASSERT_TRUE(composite_mix_channel_state_get(corr, state));
  EXPECT_EQ(state, PaintMaterialLayerChannelState::Absent);
  EXPECT_NE(nodes.over_combine, nullptr);
}

TEST_F(PaintMaterialLayerCorrectionTest, channel_insert_mask_goes_on_coverage_input)
{
  build_two_layer_stack();
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);
  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  ChainLayer &layer = chains.first().layers[1];
  ChainCorrection nodes;
  ASSERT_TRUE(correction_channel_insert(*bmain,
                                        chains.first(),
                                        layer,
                                        PaintMaterialCorrectionSection::Mask,
                                        PaintMaterialCorrectionEffect::Paint,
                                        BLI_uuid_generate_random(),
                                        nodes));
  material->nodetree->ensure_topology_cache();
  CompositeMixNode layer_mix;
  ASSERT_TRUE(composite_mix_node_read(*layer.node, layer_mix));
  ASSERT_NE(layer_mix.factor_coverage, nullptr);
  EXPECT_EQ(composite_source_node_shallow(*layer_mix.factor_coverage), nodes.mix);
  EXPECT_EQ(nodes.over_combine, nullptr);
}

TEST_F(PaintMaterialLayerCorrectionTest, chain_collect_reads_corrections_and_base)
{
  build_two_layer_stack();
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);
  const bUUID a = BLI_uuid_generate_random();
  const bUUID b = BLI_uuid_generate_random();
  insert_into_all_channels(*bmain, *material, a, PaintMaterialCorrectionSection::Content);
  insert_into_all_channels(*bmain, *material, b, PaintMaterialCorrectionSection::Content);

  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  ASSERT_EQ(chains.size(), 2);
  for (ChannelChain &chain : chains) {
    ChainLayer &layer = chain.layers[1];
    ASSERT_EQ(layer.content_corrections.size(), 2);
    EXPECT_TRUE(BLI_uuid_equal(layer.content_corrections[0].marker, a));
    EXPECT_TRUE(BLI_uuid_equal(layer.content_corrections[1].marker, b));
  }
  /* Base Color layer 1 has a map; Roughness layer 1 was Absent after channels_ensure. */
  EXPECT_NE(chains[0].layers[1].base_map, nullptr);
  EXPECT_NE(chains[0].layers[1].image, nullptr);
}

TEST_F(PaintMaterialLayerCorrectionTest, disagreeing_channels_are_refused)
{
  build_two_layer_stack();
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);
  /* Only Base Color gets the correction: a damaged graph under spec 18 §4.1a. */
  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  ChainCorrection nodes;
  ASSERT_TRUE(correction_channel_insert(*bmain,
                                        chains.first(),
                                        chains.first().layers[1],
                                        PaintMaterialCorrectionSection::Content,
                                        PaintMaterialCorrectionEffect::Paint,
                                        BLI_uuid_generate_random(),
                                        nodes));
  LayerEditPlan plan;
  EXPECT_FALSE(layer_edit_plan_build(*bmain, *material, 1, LayerEditOp::Rename, plan, error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::ChannelsDisagree);
}

TEST_F(PaintMaterialLayerCorrectionTest, base_state_reads_below_corrections)
{
  build_two_layer_stack();
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);
  insert_into_all_channels(
      *bmain, *material, BLI_uuid_generate_random(), PaintMaterialCorrectionSection::Content);
  EXPECT_EQ(BKE_paint_material_layer_channel_state_get(
                *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            PaintMaterialLayerChannelState::Enabled);
  EXPECT_EQ(BKE_paint_material_layer_channel_state_get(
                *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            PaintMaterialLayerChannelState::Absent);
}

TEST_F(PaintMaterialLayerCorrectionTest, model_without_corrections_is_unchanged)
{
  build_two_layer_stack();
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list.size(), 2);
  for (const PaintMaterialLayerStackEntry &entry : list) {
    EXPECT_TRUE(entry.content_corrections.is_empty());
    EXPECT_TRUE(entry.mask_corrections.is_empty());
    EXPECT_TRUE(entry.supported);
  }
}

TEST_F(PaintMaterialLayerCorrectionTest, model_lists_channelless_correction)
{
  build_two_layer_stack();
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);
  const bUUID marker = BLI_uuid_generate_random();
  insert_into_all_channels(*bmain, *material, marker, PaintMaterialCorrectionSection::Content);
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  const PaintMaterialLayerStackEntry &top = list[1];
  ASSERT_EQ(top.content_corrections.size(), 1);
  EXPECT_TRUE(BLI_uuid_equal(top.content_corrections[0].marker, marker));
  EXPECT_TRUE(top.content_corrections[0].channel_images.is_empty());
  EXPECT_TRUE(top.content_corrections[0].channel_blend_props.contains(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(top.supported);
}

TEST_F(PaintMaterialLayerCorrectionTest, composite_collects_absent_correction)
{
  build_two_layer_stack();
  BKE_paint_material_layer_bottom_normalize(*bmain, *material);
  insert_into_all_channels(*bmain, *material, BLI_uuid_generate_random(),
                           PaintMaterialCorrectionSection::Content);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 2);
  /* The layer's own map is still wired and still collected; the correction hangs between it and
   * the layer's Mix, exactly as the graph does. */
  EXPECT_NE(layers[1].color_image, nullptr);
  ASSERT_EQ(layers[1].content_corrections.size(), 1);
  /* Inserted Absent in this channel: no map yet, so the correction is collected without one and
   * contributes nothing until a map is set for it. */
  EXPECT_EQ(layers[1].content_corrections[0].image, nullptr);
  EXPECT_TRUE(layers[1].mask_corrections.is_empty());
}

TEST_F(PaintMaterialLayerCorrectionTest, add_creates_absent_correction_in_every_channel)
{
  build_two_layer_stack();
  bUUID marker = {};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 1, PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "Fix", &marker, &error));
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[1].content_corrections.size(), 1);
  EXPECT_EQ(list[1].content_corrections[0].name, "Fix");
  EXPECT_TRUE(list[1].content_corrections[0].channel_images.is_empty());
  EXPECT_EQ(BKE_paint_material_layer_correction_owner_ordinal(*bmain, *material, marker), 1);
}

TEST_F(PaintMaterialLayerCorrectionTest, add_on_bare_bottom_normalizes_first)
{
  build_two_layer_stack();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 0, PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, nullptr, nullptr, &error));
  EXPECT_FALSE(entries()[0].is_bare_base);
}

TEST_F(PaintMaterialLayerCorrectionTest, content_correction_refused_on_group)
{
  build_two_layer_stack();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_group_make(*bmain, *material, 1, 1, &group_ordinal, &error));
  EXPECT_FALSE(BKE_paint_material_layer_correction_add(*bmain, *material, group_ordinal,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, nullptr, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::CorrectionNotAllowedOnGroup);
  EXPECT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, group_ordinal,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, nullptr, nullptr, &error));
}

TEST_F(PaintMaterialLayerCorrectionTest, mask_correction_on_group_is_listed_and_removable)
{
  build_two_layer_stack();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_group_make(*bmain, *material, 1, 1, &group_ordinal, &error));

  /* The folder's children are listed before the folder's own row, so the row is found by its
   * ordinal rather than by its position in the array. */
  auto group_row = [&]() {
    Vector<PaintMaterialLayerStackEntry> list = entries();
    for (const PaintMaterialLayerStackEntry &entry : list) {
      if (entry.ordinal == group_ordinal) {
        return entry;
      }
    }
    return PaintMaterialLayerStackEntry{};
  };

  /* A folder without corrections reads as before: no correction rows on it. */
  ASSERT_TRUE(group_row().mask_corrections.is_empty());
  ASSERT_TRUE(group_row().content_corrections.is_empty());

  bUUID marker = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, group_ordinal,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", &marker, &error));

  /* The model lists the correction under the folder's row (spec 18 §4.5): the Outliner reads
   * this model, and remove and rename find the row through it. */
  const PaintMaterialLayerStackEntry row = group_row();
  ASSERT_EQ(row.mask_corrections.size(), 1);
  EXPECT_TRUE(BLI_uuid_equal(row.mask_corrections[0].marker, marker));
  EXPECT_EQ(row.mask_corrections[0].name, "M");
  EXPECT_EQ(BKE_paint_material_layer_correction_owner_ordinal(*bmain, *material, marker),
            group_ordinal);
  /* The correction's mask map answers through the tag, exactly as on a plain row. */
  EXPECT_NE(row.mask_corrections[0].channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr),
            nullptr);

  /* The flattened composite has no buffer for the folder's combined result, which is what the
   * correction shapes, so the stack is refused and the channel goes to the bake -- the rule
   * that a shape the byte composite cannot reproduce is evaluated by the graph instead. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));

  EXPECT_TRUE(BKE_paint_material_layer_correction_remove(*bmain, *material, marker));
  EXPECT_TRUE(group_row().mask_corrections.is_empty());
}

TEST_F(PaintMaterialLayerCorrectionTest, reorder_within_section_only)
{
  build_two_layer_stack();
  bUUID a = {}, b = {}, m = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "B", &b);
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", &m);
  ASSERT_TRUE(BKE_paint_material_layer_correction_reorder(*bmain, *material, b, 0));
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  EXPECT_TRUE(BLI_uuid_equal(list[1].content_corrections[0].marker, b));
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_correction_reorder(*bmain, *material, m, 5, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
}

TEST_F(PaintMaterialLayerCorrectionTest, remove_set_enabled_rename)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  EXPECT_TRUE(BKE_paint_material_layer_correction_set_enabled(*bmain, *material, a, false));
  EXPECT_FALSE(entries()[1].content_corrections[0].enabled);
  EXPECT_TRUE(BKE_paint_material_layer_correction_rename(*bmain, *material, a, "Renamed"));
  EXPECT_EQ(entries()[1].content_corrections[0].name, "Renamed");
  EXPECT_TRUE(BKE_paint_material_layer_correction_remove(*bmain, *material, a));
  EXPECT_TRUE(entries()[1].content_corrections.is_empty());
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_correction_remove(*bmain, *material, a, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::CorrectionNotFound);
}

TEST_F(PaintMaterialLayerCorrectionTest, set_enabled_gates_over_pair_instead_of_muting)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true);
  ASSERT_TRUE(BKE_paint_material_layer_correction_set_enabled(*bmain, *material, a, false));
  EXPECT_FALSE(entries()[1].content_corrections[0].enabled);

  /* Every channel still reads -- including Roughness, where the correction has nothing below it
   * and its pair is reachable through the gate alone. */
  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  for (const ChannelChain &chain : chains) {
    if (chain.channel == int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)) {
      ASSERT_EQ(chain.layers[1].content_corrections.size(), 1);
      EXPECT_NE(chain.layers[1].content_corrections[0].over_combine, nullptr);
    }
  }

  ChannelChain *base = nullptr;
  for (ChannelChain &chain : chains) {
    if (chain.channel == int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)) {
      base = &chain;
    }
  }
  ASSERT_NE(base, nullptr);
  ChainCorrection &nodes = base->layers[1].content_corrections[0];
  ASSERT_NE(nodes.mix, nullptr);
  ASSERT_NE(nodes.factor_multiply, nullptr);
  ASSERT_NE(nodes.over_invert, nullptr);
  ASSERT_NE(nodes.over_combine, nullptr);
  /* The mute is the Mix's alone; the pair's nodes stay live. */
  EXPECT_TRUE((nodes.mix->flag & NODE_MUTED) != 0);
  EXPECT_FALSE((nodes.over_invert->flag & NODE_MUTED) != 0);
  EXPECT_FALSE((nodes.over_combine->flag & NODE_MUTED) != 0);

  bNodeSocket *combine_a = static_cast<bNodeSocket *>(
      BLI_findlink(&nodes.over_combine->inputs, 0));
  bNodeSocket *combine_b = static_cast<bNodeSocket *>(
      BLI_findlink(&nodes.over_combine->inputs, 1));
  bNodeSocket *combine_c = static_cast<bNodeSocket *>(
      BLI_findlink(&nodes.over_combine->inputs, 2));
  bNodeSocket *invert_a = static_cast<bNodeSocket *>(
      BLI_findlink(&nodes.over_invert->inputs, 0));
  ASSERT_NE(combine_a, nullptr);
  ASSERT_NE(combine_b, nullptr);
  ASSERT_NE(combine_c, nullptr);
  ASSERT_NE(invert_a, nullptr);
  /* Off is the I1 form on the A input: unlinked, explicitly zero -- combine = 0*b + c. */
  EXPECT_TRUE(combine_a->directly_linked_links().is_empty());
  EXPECT_EQ(combine_a->default_value_typed<bNodeSocketValueFloat>()->value, 0.0f);
  /* The pair itself stays wired: B from the Subtract, C from the base alpha below. */
  ASSERT_EQ(combine_b->directly_linked_links().size(), 1);
  EXPECT_EQ(combine_b->directly_linked_links()[0]->fromnode, nodes.over_invert);
  bNodeSocket *base_alpha = base->layers[1].base_map != nullptr ?
                                bke::node_find_socket(*base->layers[1].base_map,
                                                      SOCK_OUT,
                                                      "Alpha"_ustr) :
                                nullptr;
  ASSERT_NE(base_alpha, nullptr);
  ASSERT_EQ(combine_c->directly_linked_links().size(), 1);
  EXPECT_EQ(combine_c->directly_linked_links()[0]->fromsock, base_alpha);
  /* The gate rides on the Subtract's first input, and the Mix's Factor keeps its Multiply. */
  ASSERT_EQ(invert_a->directly_linked_links().size(), 1);
  EXPECT_EQ(invert_a->directly_linked_links()[0]->fromnode, nodes.factor_multiply);
  CompositeMixNode corr;
  ASSERT_TRUE(composite_mix_node_read(*nodes.mix, corr));
  ASSERT_NE(corr.factor, nullptr);
  ASSERT_EQ(corr.factor->directly_linked_links().size(), 1);
  EXPECT_EQ(corr.factor->directly_linked_links()[0]->fromnode, nodes.factor_multiply);

  /* On again: the alpha link from the coverage Multiply is back, the gate is gone. */
  ASSERT_TRUE(BKE_paint_material_layer_correction_set_enabled(*bmain, *material, a, true));
  chains.clear();
  ASSERT_TRUE(chains_collect(*material, chains, error));
  base = nullptr;
  for (ChannelChain &chain : chains) {
    if (chain.channel == int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)) {
      base = &chain;
    }
  }
  ASSERT_NE(base, nullptr);
  ChainCorrection &back = base->layers[1].content_corrections[0];
  ASSERT_NE(back.mix, nullptr);
  ASSERT_NE(back.over_combine, nullptr);
  ASSERT_NE(back.over_invert, nullptr);
  EXPECT_FALSE((back.mix->flag & NODE_MUTED) != 0);
  combine_a = static_cast<bNodeSocket *>(BLI_findlink(&back.over_combine->inputs, 0));
  invert_a = static_cast<bNodeSocket *>(BLI_findlink(&back.over_invert->inputs, 0));
  ASSERT_NE(combine_a, nullptr);
  ASSERT_NE(invert_a, nullptr);
  ASSERT_EQ(combine_a->directly_linked_links().size(), 1);
  EXPECT_EQ(combine_a->directly_linked_links()[0]->fromnode, back.factor_multiply);
  EXPECT_TRUE(invert_a->directly_linked_links().is_empty());
  EXPECT_EQ(invert_a->default_value_typed<bNodeSocketValueFloat>()->value, 1.0f);
}

TEST_F(PaintMaterialLayerCorrectionTest, enabling_channel_creates_tagged_map_of_layer_size)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));
  const PaintMaterialLayerCorrectionEntry &corr = entries()[1].content_corrections[0];
  Image *map = corr.channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
  ASSERT_NE(map, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(map->paint_layer_id, a));
  EXPECT_EQ(map->paint_layer_channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  int w = 0, h = 0;
  ASSERT_TRUE(BKE_paint_material_layer_map_size_get(*bmain, *material, 1, w, h));
  ImBuf *ibuf = BKE_image_acquire_ibuf(map, nullptr, nullptr);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, w);
  EXPECT_EQ(ibuf->y, h);
  BKE_image_release_ibuf(map, ibuf, nullptr);
}

TEST_F(PaintMaterialLayerCorrectionTest, corrections_scale_resizes_correction_maps)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true);
  ASSERT_TRUE(BKE_paint_material_layer_corrections_scale(*bmain, *material, 1, 16, 16));
  Image *map = entries()[1].content_corrections[0].channel_images.lookup(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ImBuf *ibuf = BKE_image_acquire_ibuf(map, nullptr, nullptr);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, 16);
  EXPECT_EQ(ibuf->y, 16);
  BKE_image_release_ibuf(map, ibuf, nullptr);
}

TEST_F(PaintMaterialLayerCorrectionTest, absent_parent_channel_contributes_through_correction)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  ASSERT_EQ(BKE_paint_material_layer_channel_state_get(
                *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            PaintMaterialLayerChannelState::Absent);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_ROUGHNESS, true));
  const PaintMaterialLayerStackEntry top = entries()[1];
  EXPECT_TRUE(top.contributing_channels_mask & (1u << PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  /* Test 16: the correction enabled only in a non-reference channel is listed and composites. */
  EXPECT_EQ(top.content_corrections.size(), 1);
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_ROUGHNESS, layers));
  /* The absent rows below contribute nothing and are not collected; the row that is left is the
   * correction's own contribution, composited over transparency. */
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, nullptr);
  ASSERT_EQ(layers[0].content_corrections.size(), 1);
  EXPECT_NE(layers[0].content_corrections[0].image, nullptr);
}

TEST_F(PaintMaterialLayerCorrectionTest, disabled_parent_keeps_corrections_contributing)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true);
  ASSERT_TRUE(BKE_paint_material_layer_channel_enabled_set(
      *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false, nullptr));
  EXPECT_EQ(BKE_paint_material_layer_channel_state_get(
                *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            PaintMaterialLayerChannelState::Disabled);
  EXPECT_TRUE(entries()[1].contributing_channels_mask &
              (1u << PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintMaterialLayerCorrectionTest, ao_only_correction_keeps_params_on_mix_nodes)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_AO, true));
  PaintMaterialLayerCorrectionEntry corr = entries()[1].content_corrections[0];
  EXPECT_NE(corr.channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_AO, nullptr), nullptr);
  PointerRNA blend = corr.channel_blend_props.lookup(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  RNA_enum_set(&blend, "blend_type", MA_RAMP_MULT);
  BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true);
  EXPECT_EQ(entries()[1].content_corrections[0].blend, CompositeBlend::Multiply);
}

TEST_F(PaintMaterialLayerCorrectionTest, corrections_travel_with_move_duplicate_group)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true);

  int copy_ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_duplicate(*bmain, *material, 1, &copy_ordinal, &error));
  Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[copy_ordinal].content_corrections.size(), 1);
  const PaintMaterialLayerCorrectionEntry &copy = list[copy_ordinal].content_corrections[0];
  EXPECT_FALSE(BLI_uuid_equal(copy.marker, a));
  EXPECT_NE(copy.channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr),
            list[1].content_corrections[0].channel_images.lookup_default(
                PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr));

  ASSERT_TRUE(BKE_paint_material_layer_reorder(*bmain, *material, 1, 0, &error));
  EXPECT_EQ(BKE_paint_material_layer_correction_owner_ordinal(*bmain, *material, a), 0);

  int group_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_group_make(
      *bmain, *material, 0, 0, &group_ordinal, &error));
  EXPECT_GE(BKE_paint_material_layer_correction_owner_ordinal(*bmain, *material, a),
            PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE);
  ASSERT_TRUE(BKE_paint_material_layer_group_ungroup(*bmain, *material, group_ordinal, &error));
  EXPECT_GE(BKE_paint_material_layer_correction_owner_ordinal(*bmain, *material, a), 0);
}

TEST_F(PaintMaterialLayerCorrectionTest, mask_remove_keeps_mask_corrections_on_coverage)
{
  build_two_layer_stack();
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, *material, 1, white, 8, nullptr));
  bUUID m = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", &m);
  ASSERT_TRUE(BKE_paint_material_layer_mask_remove(*bmain, *material, 1, nullptr));
  const PaintMaterialLayerStackEntry top = entries()[1];
  EXPECT_FALSE(top.channel_images.contains(PAINT_LAYER_MAP_MASK));
  ASSERT_EQ(top.mask_corrections.size(), 1);
  EXPECT_TRUE(top.supported);
}

TEST_F(PaintMaterialLayerCorrectionTest, copy_enables_parent_channel_and_scales)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_METALLIC, true);

  /* A target of another size, with no Metallic chain. */
  Material *target = add_material_with_principled("Target");
  PaintMaterialLayerAddParams params;
  params.image_size = 16;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *target, params));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *target, params));

  const PaintMaterialCorrectionRef ref = {material->id.session_uid, a};
  Vector<bUUID> created;
  PaintMaterialCorrectionCopyReport report;
  ASSERT_TRUE(BKE_paint_material_layer_corrections_copy(
      *bmain, Span(&ref, 1), *target, 1, created, &report));
  ASSERT_EQ(created.size(), 1);
  EXPECT_EQ(report.enabled_parent_channels.size(), 1);
  EXPECT_EQ(report.scaled_maps, 1);

  Vector<PaintMaterialLayerStackEntry> list;
  BKE_paint_material_layer_stack_from_material(*bmain, *target, list);
  const PaintMaterialLayerCorrectionEntry &pasted = list[1].content_corrections[0];
  Image *map = pasted.channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_METALLIC, nullptr);
  ASSERT_NE(map, nullptr);
  int w = 0, h = 0;
  BKE_image_get_size(map, nullptr, &w, &h);
  EXPECT_EQ(w, 16);
}

TEST_F(PaintMaterialLayerCorrectionTest, copy_skips_missing_and_group_content)
{
  build_two_layer_stack();
  int group_ordinal = -1;
  BKE_paint_material_layer_group_make(*bmain, *material, 1, 1, &group_ordinal, nullptr);
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 0,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  const PaintMaterialCorrectionRef refs[] = {
      {material->id.session_uid, a}, {material->id.session_uid, BLI_uuid_generate_random()}};
  Vector<bUUID> created;
  PaintMaterialCorrectionCopyReport report;
  BKE_paint_material_layer_corrections_copy(
      *bmain, Span(refs, 2), *material, group_ordinal, created, &report);
  EXPECT_TRUE(created.is_empty());
  EXPECT_EQ(report.skipped_group, 1);
  EXPECT_EQ(report.skipped_missing, 1);
}

TEST_F(PaintMaterialLayerCorrectionTest, active_layer_resolves_correction_from_bindings)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true);
  Image *map = entries()[1].content_corrections[0].channel_images.lookup(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  PaintModeSettings settings = {};
  BKE_paint_material_channel_binding_set(
      settings.channel_image_bindings[PAINT_MATERIAL_CHANNEL_BASE_COLOR], map);
  const std::optional<PaintMaterialActiveLayer> active = BKE_paint_material_active_layer_get(
      *bmain, settings);
  ASSERT_TRUE(active.has_value());
  EXPECT_EQ(active->ordinal, 1);
  EXPECT_TRUE(BLI_uuid_equal(active->correction, a));
  EXPECT_EQ(active->maps.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr), map);
  BKE_paint_material_channel_binding_set(
      settings.channel_image_bindings[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
}

TEST_F(PaintMaterialLayerCorrectionTest, active_channelless_correction_survives_until_removed)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  PaintModeSettings settings = {};
  BKE_paint_material_active_correction_set(material, a);
  std::optional<PaintMaterialActiveLayer> active = BKE_paint_material_active_layer_get(*bmain,
                                                                                        settings);
  ASSERT_TRUE(active.has_value());
  EXPECT_TRUE(BLI_uuid_equal(active->correction, a));
  BKE_paint_material_layer_correction_remove(*bmain, *material, a);
  EXPECT_FALSE(BKE_paint_material_active_layer_get(*bmain, settings).has_value());
  BKE_paint_material_active_correction_set(nullptr, {});
}

TEST_F(PaintMaterialLayerCorrectionTest, realign_restores_missing_correction_nodes)
{
  build_two_layer_stack();
  bUUID a = {};
  BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a);
  /* Damage: take the correction out of the Roughness channel only, links closed properly. */
  Vector<ChannelChain> chains;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  ASSERT_EQ(chains[1].channel, int(PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  correction_channel_remove(
      *bmain, chains[1], chains[1].layers[1], chains[1].layers[1].content_corrections[0]);
  BKE_ntree_update_after_single_tree_change(*bmain, *material->nodetree);
  ASSERT_FALSE(entries()[1].supported);
  ASSERT_TRUE(BKE_paint_material_layer_channels_realign(*bmain, *material, &error));
  EXPECT_TRUE(entries()[1].supported);
  EXPECT_EQ(entries()[1].content_corrections.size(), 1);
}

static ChannelChain *test_chain_for(Vector<ChannelChain> &chains, const int channel)
{
  for (ChannelChain &chain : chains) {
    if (chain.channel == channel) {
      return &chain;
    }
  }
  return nullptr;
}

static bool test_socket_linked(const bNodeSocket *socket)
{
  return socket != nullptr && !socket->directly_linked_links().is_empty();
}

TEST_F(PaintMaterialLayerCorrectionTest, switched_off_row_mutes_its_maps)
{
  build_two_layer_stack();
  bUUID a = {};
  bUUID b = {};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 1, PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "A", &a, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 1, PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Paint, "B", &b, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, a, PAINT_MATERIAL_CHANNEL_ROUGHNESS, true, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, b, PAINT_MATERIAL_CHANNEL_ROUGHNESS, true, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_set_enabled(*bmain, *material, a, false));

  Vector<ChannelChain> chains;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  ChannelChain *roughness = test_chain_for(chains, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(roughness, nullptr);
  const ChainLayer &layer = roughness->layers[1];
  ASSERT_EQ(layer.content_corrections.size(), 2);
  const ChainCorrection &lower = layer.content_corrections[0];
  const ChainCorrection &upper = layer.content_corrections[1];
  EXPECT_TRUE(BLI_uuid_equal(lower.marker, a));
  ASSERT_NE(lower.map, nullptr);
  ASSERT_NE(upper.map, nullptr);
  /* A muted Mix with nothing below would pass its own map on: the map mutes with the row. */
  EXPECT_TRUE(lower.mix->is_muted());
  EXPECT_TRUE(lower.map->is_muted());
  EXPECT_FALSE(upper.map->is_muted());
  /* Roughness is Absent under the corrections: the lower one's base is an explicit black. */
  CompositeMixNode lower_mix;
  ASSERT_TRUE(composite_mix_node_read(*lower.mix, lower_mix));
  EXPECT_FALSE(test_socket_linked(lower_mix.bottom));
  const float *base = static_cast<const bNodeSocketValueRGBA *>(lower_mix.bottom->default_value)
                          ->value;
  EXPECT_EQ(base[0], 0.0f);
  EXPECT_EQ(base[1], 0.0f);
  EXPECT_EQ(base[2], 0.0f);

  ASSERT_TRUE(BKE_paint_material_layer_correction_set_enabled(*bmain, *material, a, true));
  chains.clear();
  ASSERT_TRUE(chains_collect(*material, chains, error));
  roughness = test_chain_for(chains, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(roughness, nullptr);
  ASSERT_EQ(roughness->layers[1].content_corrections.size(), 2);
  EXPECT_FALSE(roughness->layers[1].content_corrections[0].map->is_muted());
}

TEST_F(PaintMaterialLayerCorrectionTest, mask_correction_follows_what_the_row_puts_in)
{
  build_two_layer_stack();
  bUUID m = {};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 1, PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", &m, &error));

  Vector<ChannelChain> chains;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  /* Base Color has the layer's map; Roughness is Absent there. A mask painted in a blend that
   * adds coverage must not bring the layer into a channel it puts nothing into. */
  for (const int channel : {int(PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                            int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)})
  {
    ChannelChain *chain = test_chain_for(chains, channel);
    ASSERT_NE(chain, nullptr);
    const ChainLayer &layer = chain->layers[1];
    ASSERT_EQ(layer.mask_corrections.size(), 1);
    CompositeMixNode corr;
    ASSERT_TRUE(composite_mix_node_read(*layer.mask_corrections[0].mix, corr));
    const bool row_puts_in = channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    EXPECT_EQ(test_socket_linked(corr.factor_coverage), row_puts_in);
    EXPECT_EQ(test_socket_linked(corr.bottom), row_puts_in);
  }
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  EXPECT_TRUE(list[1].supported);
  EXPECT_EQ(list[1].contributing_channels_mask & (1u << PAINT_MATERIAL_CHANNEL_ROUGHNESS), 0u);
}

TEST_F(PaintMaterialLayerCorrectionTest, disabling_base_zeroes_mask_chain_and_reads_disabled)
{
  build_two_layer_stack();
  bUUID m = {};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 1, PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", &m, &error));
  /* A second channel on, so switching Base Color off is not the row's last one. */
  ASSERT_TRUE(BKE_paint_material_layer_channel_enabled_set(
      *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_ROUGHNESS, true, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_channel_enabled_set(
      *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false, nullptr, &error));

  EXPECT_EQ(BKE_paint_material_layer_channel_state_get(
                *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            PaintMaterialLayerChannelState::Disabled);
  Vector<PaintMaterialLayerStackEntry> list = entries();
  EXPECT_TRUE(list[1].supported);
  ASSERT_EQ(list[1].mask_corrections.size(), 1);
  EXPECT_EQ(list[1].contributing_channels_mask & (1u << PAINT_MATERIAL_CHANNEL_BASE_COLOR), 0u);

  auto base_color_mask = [&](CompositeMixNode &r_corr, CompositeMixNode &r_row) {
    Vector<ChannelChain> chains;
    ASSERT_TRUE(chains_collect(*material, chains, error));
    ChannelChain *chain = test_chain_for(chains, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->layers[1].mask_corrections.size(), 1);
    ASSERT_TRUE(composite_mix_node_read(*chain->layers[1].mask_corrections[0].mix, r_corr));
    ASSERT_TRUE(composite_mix_node_read(*chain->layers[1].node, r_row));
  };
  CompositeMixNode corr;
  CompositeMixNode row;
  base_color_mask(corr, row);
  /* The chain stays on the row's coverage -- it is how the model finds the rows -- but it is
   * zeroed: no base under it, no coverage of its own. */
  EXPECT_TRUE(test_socket_linked(row.factor_coverage));
  EXPECT_FALSE(test_socket_linked(corr.bottom));
  EXPECT_FALSE(test_socket_linked(corr.factor_coverage));

  ASSERT_TRUE(BKE_paint_material_layer_channel_enabled_set(
      *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, nullptr, &error));
  EXPECT_EQ(BKE_paint_material_layer_channel_state_get(
                *bmain, *material, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            PaintMaterialLayerChannelState::Enabled);
  base_color_mask(corr, row);
  EXPECT_TRUE(test_socket_linked(corr.bottom));
  EXPECT_TRUE(test_socket_linked(corr.factor_coverage));
}

TEST_F(PaintMaterialLayerCorrectionTest, mask_toggle_off_zeroes_mask_chain)
{
  build_two_layer_stack();
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, *material, 1, white, 8, &error));
  bUUID m = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain,
      *material,
      1,
      PaintMaterialCorrectionSection::Mask,
      PaintMaterialCorrectionEffect::Paint,
      "M",
      &m,
      &error));

  auto base_color_mask = [&](CompositeMixNode &r_corr, CompositeMixNode &r_row) {
    Vector<ChannelChain> chains;
    ASSERT_TRUE(chains_collect(*material, chains, error));
    ChannelChain *chain = test_chain_for(chains, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->layers[1].mask_corrections.size(), 1);
    ASSERT_TRUE(composite_mix_node_read(*chain->layers[1].mask_corrections[0].mix, r_corr));
    ASSERT_TRUE(composite_mix_node_read(*chain->layers[1].node, r_row));
  };

  CompositeMixNode corr;
  CompositeMixNode row;
  base_color_mask(corr, row);
  EXPECT_TRUE(test_socket_linked(corr.bottom));
  EXPECT_TRUE(test_socket_linked(corr.factor_coverage));

  /* The correction shapes the mask, so a switched-off mask leaves it nothing to shape: its own
   * coverage is zeroed, so its Mix passes the row's fallback through unchanged. It comes back when
   * the mask does. */
  ASSERT_TRUE(BKE_paint_material_layer_mask_set_enabled(*bmain, *material, 1, false, &error))
      << int(error);
  base_color_mask(corr, row);
  EXPECT_FALSE(test_socket_linked(corr.factor_coverage));
  ASSERT_NE(corr.factor_coverage, nullptr);
  EXPECT_EQ(corr.factor_coverage->default_value_typed<bNodeSocketValueFloat>()->value, 0.0f);

  ASSERT_TRUE(BKE_paint_material_layer_mask_set_enabled(*bmain, *material, 1, true, &error))
      << int(error);
  base_color_mask(corr, row);
  EXPECT_TRUE(test_socket_linked(corr.bottom));
  EXPECT_TRUE(test_socket_linked(corr.factor_coverage));
}

TEST_F(PaintMaterialLayerCorrectionTest, mask_correction_has_no_channel_switch)
{
  build_two_layer_stack();
  bUUID m = {};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 1, PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", &m, &error));
  EXPECT_FALSE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, m, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::CorrectionSectionMismatch);
}

TEST_F(PaintMaterialLayerCorrectionTest, folder_with_mask_corrections_refuses_ungroup)
{
  build_two_layer_stack();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_group_make(*bmain, *material, 1, 1, &group_ordinal, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, group_ordinal, PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", nullptr, &error));
  EXPECT_FALSE(
      BKE_paint_material_layer_group_ungroup(*bmain, *material, group_ordinal, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::GroupHasMaskCorrections);
}

TEST_F(PaintMaterialLayerCorrectionTest, removing_folder_with_mask_correction_takes_its_instance)
{
  build_two_layer_stack();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int group_ordinal = -1;
  ASSERT_TRUE(
      BKE_paint_material_layer_group_make(*bmain, *material, 1, 1, &group_ordinal, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, group_ordinal, PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", nullptr, &error));
  auto instance_num = [&]() {
    int num = 0;
    for (const bNode &node : material->nodetree->nodes) {
      num += node.is_group() ? 1 : 0;
    }
    return num;
  };
  ASSERT_GT(instance_num(), 0);
  ASSERT_TRUE(BKE_paint_material_layer_remove(*bmain, *material, group_ordinal, &error));
  EXPECT_EQ(instance_num(), 0);
}

TEST_F(PaintMaterialLayerCorrectionTest, duplicate_counts_one_user_per_mask_map_node)
{
  build_two_layer_stack();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(
      *bmain, *material, 1, PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, "M", nullptr, &error));
  int copy_ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_duplicate(*bmain, *material, 1, &copy_ordinal, &error));

  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[1].mask_corrections.size(), 1);
  ASSERT_EQ(list[copy_ordinal].mask_corrections.size(), 1);
  const Image *source_map = list[1].mask_corrections[0].channel_images.lookup_default(
      PAINT_LAYER_MAP_MASK, nullptr);
  const Image *copy_map = list[copy_ordinal].mask_corrections[0].channel_images.lookup_default(
      PAINT_LAYER_MAP_MASK, nullptr);
  ASSERT_NE(source_map, nullptr);
  ASSERT_NE(copy_map, nullptr);
  EXPECT_NE(source_map, copy_map);
  auto node_users = [&](const Image *image) {
    int num = 0;
    for (const bNode &node : material->nodetree->nodes) {
      num += (node.id == &image->id) ? 1 : 0;
    }
    return num;
  };
  /* One shared mask map node per channel in the copy: each one a user of the copied image. */
  EXPECT_EQ(source_map->id.us, node_users(source_map));
  EXPECT_EQ(copy_map->id.us, node_users(copy_map));
}

TEST_F(PaintMaterialLayerCorrectionTest, CorrectionChannelValueApply_RoundTrips)
{
  /* A Fill layer with its Base Color map wired: the row the Fill correction hangs under. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.kind = PaintMaterialLayerKind::Fill;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));

  bUUID created = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Fill, nullptr,
      &created, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, &error));

  /* The apply re-fills the correction's own map and records the raw value on its Mix. */
  const float value[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_value_apply(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  float recorded[4];
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_value_get(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, recorded));
  EXPECT_NEAR(recorded[1], 0.4f, 1e-6f);

  /* The pixels moved too: the map takes what a Fill created there would, so the expectation is
   * whatever the generator's own fill writes for the same colour. */
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[1].content_corrections.size(), 1);
  Image *map = list[1].content_corrections[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
  ASSERT_NE(map, nullptr);
  uint8_t expected[4];
  BKE_image_buf_fill_color(expected, nullptr, 1, 1, value);
  void *lock = nullptr;
  ImBuf *buffer = BKE_image_acquire_ibuf(map, nullptr, &lock);
  ASSERT_NE(buffer, nullptr);
  ASSERT_NE(buffer->byte_buffer.data, nullptr);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(buffer->byte_buffer.data[i], expected[i]) << "component " << i;
  }
  BKE_image_release_ibuf(map, buffer, lock);
}

TEST_F(PaintMaterialLayerCorrectionTest, CorrectionChannelImageSet_ReplacesMap)
{
  /* A Fill layer with its Base Color map wired: the row the Fill correction hangs under. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.kind = PaintMaterialLayerKind::Fill;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));

  bUUID created = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Fill, nullptr,
      &created, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, &error));

  /* An image from outside the stack, carrying the one user a new data-block has. */
  const float blank[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *first = BKE_image_add_generated(
      bmain, 64, 64, "external", 32, false, IMA_GENTYPE_BLANK, blank, false, false, false);
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *first, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* The map the model lists is the image handed over, tagged the way a map of the correction
   * is; the map the channel was created with is orphaned by the replacement and freed. */
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[1].content_corrections.size(), 1);
  EXPECT_EQ(list[1].content_corrections[0].channel_images.lookup_default(
                PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr),
            first);
  EXPECT_TRUE(BLI_uuid_equal(first->paint_layer_id, created));
  EXPECT_EQ(first->paint_layer_channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* Assigning again replaces: the lookup answers with the second image, and the first keeps only
   * the user its holder outside the stack gave it -- the map node's user moved. */
  Image *second = BKE_image_add_generated(
      bmain, 64, 64, "external2", 32, false, IMA_GENTYPE_BLANK, blank, false, false, false);
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *second, &error));
  EXPECT_EQ(entries()[1].content_corrections[0].channel_images.lookup_default(
                PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr),
            second);
  EXPECT_EQ(first->id.us, 1);

  /* A channel the correction is not wired on is refused, leaving it as it was. */
  EXPECT_FALSE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_ROUGHNESS, *second, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IndexOutOfRange);
}

TEST_F(PaintMaterialLayerCorrectionTest, correction_channel_unlink_restores_last_applied_value)
{
  /* A Fill layer with its Base Color map wired: the row the Fill correction hangs under. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.kind = PaintMaterialLayerKind::Fill;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));

  bUUID created = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Content, PaintMaterialCorrectionEffect::Fill, nullptr,
      &created, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, &error));

  /* Give the correction's map a value, so there is one to restore. */
  const float value[4] = {0.3f, 0.3f, 0.3f, 1.0f};
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_value_apply(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value, &error));

  /* An image from outside the stack takes the map over, the way a drop does. */
  const float blank[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *external = BKE_image_add_generated(
      bmain, 16, 16, "external", 32, false, IMA_GENTYPE_BLANK, blank, false, false, false);
  ASSERT_NE(external, nullptr);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *external, &error));

  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_unlink(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* The channel shows a fresh map of the correction's own again; the dropped image is not it. */
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[1].content_corrections.size(), 1);
  Image *map = list[1].content_corrections[0].channel_images.lookup_default(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
  ASSERT_NE(map, nullptr);
  EXPECT_NE(map, external);

  /* The dropped image's pixels were not touched: the unlink writes only its own maps. */
  auto expect_pixel = [](Image *image, const float expected_color[4]) {
    uint8_t expected[4];
    BKE_image_buf_fill_color(expected, nullptr, 1, 1, expected_color);
    void *lock = nullptr;
    ImBuf *buffer = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(buffer->byte_buffer.data, nullptr);
    for (int i = 0; i < 4; i++) {
      EXPECT_EQ(buffer->byte_buffer.data[i], expected[i]) << "component " << i;
    }
    BKE_image_release_ibuf(image, buffer, lock);
  };
  expect_pixel(external, blank);
  expect_pixel(map, value);

  /* The record survived the round trip through the external image. */
  float recorded[4];
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_value_get(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, recorded));
  EXPECT_NEAR(recorded[0], 0.3f, 1e-6f);
}

TEST_F(PaintMaterialLayerCorrectionTest, FillMaskCorrection_RefusesSecondChannel)
{
  /* A Fill layer with its Base Color map wired: the row the Fill mask hangs under. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.kind = PaintMaterialLayerKind::Fill;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));

  bUUID created = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Fill, nullptr,
      &created, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* Picking the correction's one grayscale channel is allowed. */
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* A second channel is the multi-channel attempt the single-channel form refuses, before any
   * node of it is touched: no Roughness chain is wired by the refusal. */
  EXPECT_FALSE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_ROUGHNESS, true, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::CorrectionSectionMismatch);
  Vector<ChannelChain> chains;
  ASSERT_TRUE(chains_collect(*material, chains, error));
  EXPECT_EQ(chains.size(), 1);

  /* Disabling the picked channel and re-enabling it stay allowed. */
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
}

TEST_F(PaintMaterialLayerCorrectionTest, FillMaskCorrection_ImageSet_OnTheSingleChannel)
{
  /* A Fill layer with its Base Color map wired: the row the Fill mask hangs under. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.kind = PaintMaterialLayerKind::Fill;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));

  bUUID created = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Fill, nullptr,
      &created, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, &error));

  /* An image from outside the stack, carrying the one user a new data-block has. */
  const float blank[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *external = BKE_image_add_generated(
      bmain, 16, 16, "external mask", 32, false, IMA_GENTYPE_BLANK, blank, false, false, false);
  ASSERT_NE(external, nullptr);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *external, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* The mask map the model lists is the image handed over, tagged with the correction's marker
   * and the mask role every reader of a mask map keys on. */
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[1].mask_corrections.size(), 1);
  EXPECT_EQ(list[1].mask_corrections[0].channel_images.lookup_default(PAINT_LAYER_MAP_MASK,
                                                                      nullptr),
            external);
  EXPECT_TRUE(BLI_uuid_equal(external->paint_layer_id, created));
  EXPECT_EQ(external->paint_layer_channel, PAINT_LAYER_MAP_MASK);

  /* Another channel is still the multi-channel attempt: the assignment is refused. */
  EXPECT_FALSE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_ROUGHNESS, *external, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::CorrectionSectionMismatch);

  /* A painted mask correction keeps refusing the assignment: its map is synced, not assigned. */
  bUUID painted = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Paint, nullptr,
      &painted, &error));
  EXPECT_FALSE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, painted, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *external, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::CorrectionSectionMismatch);
}

TEST_F(PaintMaterialLayerCorrectionTest, FillMaskCorrection_Unlink_NeverWritesTheDroppedImage)
{
  /* A Fill layer with its Base Color map wired: the row the Fill mask hangs under. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.kind = PaintMaterialLayerKind::Fill;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));

  bUUID created = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain, *material, 1,
      PaintMaterialCorrectionSection::Mask, PaintMaterialCorrectionEffect::Fill, nullptr,
      &created, &error));
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_enabled_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true, &error));

  /* Give the correction's map a value, so there is one to restore. */
  const float value[4] = {0.3f, 0.3f, 0.3f, 1.0f};
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_value_apply(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value, &error));

  /* An image from outside the stack takes the map over, the way a drop does. Its bytes are
   * grabbed before the unlink runs. */
  const float blank[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *external = BKE_image_add_generated(
      bmain, 16, 16, "external mask", 32, false, IMA_GENTYPE_BLANK, blank, false, false, false);
  ASSERT_NE(external, nullptr);
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_image_set(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *external, &error));
  uint8_t before[16 * 16 * 4];
  {
    void *lock = nullptr;
    ImBuf *buffer = BKE_image_acquire_ibuf(external, nullptr, &lock);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(buffer->byte_buffer.data, nullptr);
    ASSERT_EQ(buffer->x * buffer->y * 4, int(sizeof(before)));
    for (int i = 0; i < int(sizeof(before)); i++) {
      before[i] = buffer->byte_buffer.data[i];
    }
    BKE_image_release_ibuf(external, buffer, lock);
  }

  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_unlink(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  /* The dropped image's pixels were not touched: the unlink rewired a fresh map of the
   * correction's own and never wrote to the one the drop handed over. */
  bool unchanged = true;
  {
    void *lock = nullptr;
    ImBuf *buffer = BKE_image_acquire_ibuf(external, nullptr, &lock);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(buffer->byte_buffer.data, nullptr);
    for (int i = 0; i < int(sizeof(before)); i++) {
      if (buffer->byte_buffer.data[i] != before[i]) {
        unchanged = false;
        break;
      }
    }
    BKE_image_release_ibuf(external, buffer, lock);
  }
  EXPECT_TRUE(unchanged);

  /* The mask the model lists is a fresh map of the correction's own again, not the dropped
   * image; the dropped image lost the tag with the node. */
  const Vector<PaintMaterialLayerStackEntry> list = entries();
  ASSERT_EQ(list[1].mask_corrections.size(), 1);
  const PaintMaterialLayerCorrectionEntry &mask_row = list[1].mask_corrections[0];
  Image *map = mask_row.channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr);
  ASSERT_NE(map, nullptr);
  EXPECT_NE(map, external);
  EXPECT_TRUE(BLI_uuid_is_nil(external->paint_layer_id));

  /* The record survived the round trip through the external image. */
  float recorded[4];
  ASSERT_TRUE(BKE_paint_material_layer_correction_channel_value_get(
      *bmain, *material, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR, recorded));
  EXPECT_NEAR(recorded[0], 0.3f, 1e-6f);
}

}  // namespace blender::bke::tests
