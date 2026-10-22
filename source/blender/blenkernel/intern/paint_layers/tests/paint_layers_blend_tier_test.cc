/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers runtime blend-tier tests: which channels a blend edit puts in the tier, what that
 * does to the topology hashes and the scheduling flags, the generated group instance and its
 * in-place `Mode` sync, and the timer / load-post demotion back to a static Mix node.
 *
 * The fixture lives in `paint_layers_generate_test_fixture.hh`.
 */

#include "paint_layers_generate_test_fixture.hh"

#include "BKE_idprop.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

#include "BLI_time.h"

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_intern.hh"

namespace blender::bke::tests {

/**
 * The generator fixture plus a clean runtime tier around every test: the tier state is a file-level
 * static, so one test's leftovers must not reach the next one.
 */
class PaintLayersBlendTierTest : public PaintLayersGenerateTest {
 public:
  void SetUp() override
  {
    PaintLayersGenerateTest::SetUp();
    BKE_paint_layers_blend_tier_clear();
  }

  void TearDown() override
  {
    BKE_paint_layers_blend_tier_clear();
    PaintLayersGenerateTest::TearDown();
  }
};

/** The root topology hash of the fixture's material, with the channels it currently wires. */
static uint64_t root_topology_hash(const Material &ma)
{
  return paint_layers::paint_layers_root_topology_hash(
      ma, paint_layers::paint_layers_wired_channels(ma));
}

/** The topology hash of \a layer, with the channels the fixture's material currently wires. */
static uint64_t layer_topology_hash(const Material &ma, const MaterialPaintLayer &layer)
{
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(ma);
  return paint_layers::paint_layers_layer_topology_hash(
      ma, layer, paint_layers::paint_layers_wired_channels(ma), forms);
}

/** The instance of the shared dynamic blend group directly in \a tree, or null. */
static bNode *blend_group_instance_find(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (BKE_paint_material_is_blend_group(node)) {
      return &node;
    }
  }
  return nullptr;
}

/** The keyed blend node of \a tree: either the static Mix or the dynamic group. */
static bNode *keyed_blend_node_find(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (paint_layers::prop_string_get(node.system_properties,
                                      paint_layers::BLEND_NODE_KEY_PROP) != nullptr)
    {
      return &node;
    }
  }
  return nullptr;
}

/**
 * The `Mode` input of \a instance, read as the integer socket the group was built with.
 *
 * #bke::node_find_socket matches identifiers, and the group's `Mode` identifier is not its display
 * name, so the input is looked up by name like the builder's own fallback does.
 */
static int blend_group_mode_get(const bNode &instance)
{
  for (const bNodeSocket &socket : instance.inputs) {
    if (STREQ(socket.name, "Mode")) {
      if (socket.default_value == nullptr) {
        return -1;
      }
      return static_cast<const bNodeSocketValueInt *>(socket.default_value)->value;
    }
  }
  return -1;
}

/** The persisted "saved in the dynamic tier" bit of \a ma's root tree, or -1 when absent. */
static int root_blend_dynamic_get(const Material &ma)
{
  if (ma.paint_layers_tree == nullptr) {
    return -1;
  }
  const IDProperty *props = IDP_GetProperties(&ma.paint_layers_tree->id);
  if (props == nullptr) {
    return -1;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      props, paint_layers::TREE_ROOT_BLEND_DYNAMIC_PROP, IDP_INT);
  return prop != nullptr ? IDP_int_get(prop) : -1;
}

/* -------------------------------------------------------------------- */
/** \name Setters and eligibility
 * \{ */

TEST_F(PaintLayersBlendTierTest, row_blend_set_notes_every_inheriting_channel)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  add_channel(*row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("Rough"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Both overrides inherit the row's blend, so a row-level edit scrubs both channels. */
  ASSERT_EQ(row->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].blend, -1);
  ASSERT_EQ(row->channel_settings[PAINT_MATERIAL_CHANNEL_ROUGHNESS].blend, -1);

  const uint64_t root_before = root_topology_hash(*ma);
  const uint64_t layer_before = layer_topology_hash(*ma, *row);
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_ADD));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_MULTIPLY));

  const uint64_t expected_mask = (uint64_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR) |
                                 (uint64_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *row), expected_mask);
  EXPECT_TRUE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  /* A tier transition changes which node a channel gets: both hashes move. */
  EXPECT_NE(root_topology_hash(*ma), root_before);
  EXPECT_NE(layer_topology_hash(*ma, *row), layer_before);

  /* The first step is a rebuild, not the in-place blend sync. */
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC, 0);
}

TEST_F(PaintLayersBlendTierTest, channel_blend_set_notes_only_its_channel)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  add_channel(*row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("Rough"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_ADD));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));

  /* Only the edited channel is scrubbed; the other one keeps its static Mix. */
  const uint64_t base_bit = uint64_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *row), base_bit);
  EXPECT_TRUE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* Back to inherit: the effective blend follows the row again, still inside the same tier. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, -1));
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            row->blend);
  EXPECT_TRUE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *row), base_bit);
  /* The mask did not move, so the change is a value sync, not a rebuild. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC, 0);
}

TEST_F(PaintLayersBlendTierTest, normal_channel_is_never_dynamic)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  add_channel(*row, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_ADD));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_MULTIPLY));
  /* The Normal channel combines normals in its own group: no tier record, no mask bit. */
  EXPECT_FALSE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *row) &
                (uint64_t(1) << PAINT_MATERIAL_CHANNEL_NORMAL),
            0);
  /* The Base Color channel did enter the tier, so the test is about Normal alone. */
  EXPECT_TRUE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  /* Replace is the Normal channel's structural choice, not a tier channel. */
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE));
  EXPECT_FALSE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_NORMAL));
}

TEST_F(PaintLayersBlendTierTest, folder_returning_to_pass_through_drops_its_tier)
{
  MaterialPaintLayer *first = add_paint_layer("First", add_image("First"));
  MaterialPaintLayer *second = add_paint_layer("Second", add_image("Second"));
  MaterialPaintLayer *members[2] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  /* A non-Mix blend takes the folder out of Pass Through, so its channels build a row of their
   * own and can be scrubbed. */
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  /* Leaving Pass Through is a single edit: the channels are only pending, still static Mix. */
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *folder), 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* Back to Mix inlines the folder again: every record must go with the row it belonged to. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MIX));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *folder), 0);
  EXPECT_FALSE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, folder->marker, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersBlendTierTest, non_participating_channel_does_not_enter_the_tier)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  /* A record that lays nothing: no map and no coverage, so the row does not take part in it. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_FALSE(paint_layers::paint_layer_blend_dynamic_eligible(
      *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  const uint64_t root_before = root_topology_hash(*ma);
  const uint64_t layer_before = layer_topology_hash(*ma, *row);
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_BLEND_MULTIPLY));

  /* No blend node exists for the channel, so there is nothing to scrub and nothing to rebuild. */
  EXPECT_FALSE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *row), 0);
  EXPECT_EQ(root_topology_hash(*ma), root_before);
  EXPECT_EQ(layer_topology_hash(*ma, *row), layer_before);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Timer
 * \{ */

TEST_F(PaintLayersBlendTierTest, tier_timer_demotes_an_expired_channel)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  const bUUID marker = row->marker;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  paint_layers::paint_layers_blend_tier_note(*ma, marker, channel);
  paint_layers::paint_layers_blend_tier_note(*ma, marker, channel);
  EXPECT_TRUE(paint_layers::paint_layers_blend_tier_is_dynamic(*ma, marker, channel));
  EXPECT_GT(BKE_paint_layers_blend_tier_next_deadline(), 0.0);

  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  EXPECT_TRUE(BKE_paint_layers_blend_tier_demote_due(*bmain, BLI_time_now_seconds() + 3.0));

  EXPECT_FALSE(paint_layers::paint_layers_blend_tier_is_dynamic(*ma, marker, channel));
  EXPECT_EQ(paint_layers::paint_layer_blend_dynamic_mask(*ma, *row), 0);
  /* Demotion is a rebuild back to the static Mix node. */
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(BKE_paint_layers_blend_tier_next_deadline(), -1.0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generated graph
 * \{ */

TEST_F(PaintLayersBlendTierTest, generated_row_uses_the_blend_group_and_syncs_mode_in_place)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  /* The first edit moves the channel into the tier, so the row group is rebuilt with the shared
   * group instead of a static Mix. */
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_ADD));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_MULTIPLY));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);

  bNodeTree *group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  bNode *instance = blend_group_instance_find(*group);
  ASSERT_NE(instance, nullptr);
  EXPECT_TRUE(BKE_paint_material_is_blend_group(*instance));
  EXPECT_EQ(blend_group_mode_get(*instance), MA_RAMP_MULT);
  const uint64_t root_after_first = root_topology_hash(*ma);

  /* A second mode in the same tier is a value: the same group node takes the new Mode and no
   * topology hash moves. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_ADD));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC, 0);
  EXPECT_EQ(root_topology_hash(*ma), root_after_first);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(blend_group_instance_find(*group), instance);
  EXPECT_EQ(blend_group_mode_get(*instance), MA_RAMP_ADD);
  EXPECT_EQ(root_topology_hash(*ma), root_after_first);
}

TEST_F(PaintLayersBlendTierTest, demoted_row_rebuilds_as_a_static_mix)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_ADD));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_MULTIPLY));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(root_blend_dynamic_get(*ma), 1);

  ASSERT_TRUE(BKE_paint_layers_blend_tier_demote_due(*bmain, BLI_time_now_seconds() + 3.0));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(blend_group_instance_find(*group), nullptr);
  bNode *mix = keyed_blend_node_find(*group);
  ASSERT_NE(mix, nullptr);
  EXPECT_EQ(mix->type_legacy, SH_NODE_MIX);
  ASSERT_NE(mix->storage, nullptr);
  EXPECT_EQ(static_cast<NodeShaderMix *>(mix->storage)->blend_type, MA_RAMP_MULT);
  EXPECT_EQ(root_blend_dynamic_get(*ma), 0);
}

TEST_F(PaintLayersBlendTierTest, load_post_demotes_a_tree_saved_in_the_tier)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Base"));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_ADD));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, row, MA_PAINT_LAYER_BLEND_MULTIPLY));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(root_blend_dynamic_get(*ma), 1);

  /* The loader has no tier state: the marker asks for one static rebuild. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  BKE_paint_layers_blend_load_post(*bmain);
  EXPECT_FALSE(paint_layers::paint_layers_blend_tier_is_dynamic(
      *ma, row->marker, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(blend_group_instance_find(*group), nullptr);
  bNode *mix = keyed_blend_node_find(*group);
  ASSERT_NE(mix, nullptr);
  EXPECT_EQ(mix->type_legacy, SH_NODE_MIX);
  ASSERT_NE(mix->storage, nullptr);
  EXPECT_EQ(static_cast<NodeShaderMix *>(mix->storage)->blend_type, MA_RAMP_MULT);
  EXPECT_EQ(root_blend_dynamic_get(*ma), 0);
}

/** \} */

}  // namespace blender::bke::tests
