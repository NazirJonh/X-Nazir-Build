/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The maps of a Material row: handed over before the render, landed after it, read by the generator
 * and the CPU composite as the source of a painted-like row, and kept or dropped as the source and
 * the row change. The render itself is the editor's and is not run here; the tests play its two
 * calls (#BKE_paint_layers_material_bake_apply and _land) with generated images.
 *
 * The fixture lives in `paint_layers_generate_test_fixture.hh`.
 */

#include "paint_layers_generate_test_fixture.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

class PaintLayersMaterialBakeTest : public PaintLayersGenerateTest {
 public:
  Material *source = nullptr;

  void SetUp() override
  {
    PaintLayersGenerateTest::SetUp();
    source = BKE_material_add(bmain, "Source");
    /* A source the editor can render: a Principled BSDF into the Material Output. */
    bNodeTree &ntree = *source->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(ntree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  }

  MaterialPaintLayer *add_material_row(Material &from)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Material;
    params.source = &from;
    MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
    EXPECT_NE(row, nullptr);
    return row;
  }

  /** What the editor does before a render starts: the side, the state it starts from, the maps. */
  void start_render(MaterialPaintLayer &row,
                    const Span<int> channels,
                    uint32_t r_hash[2],
                    uint32_t r_source_hash[2],
                    const int size = 64)
  {
    ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, row, size));
    BKE_paint_layers_material_bake_stamp_get(*ma, row, r_hash, r_source_hash);
    Vector<Image *> images;
    for (const int channel : channels) {
      images.append(add_image(channel == PAINT_MATERIAL_CHANNEL_ALPHA ? "Alpha" : "Map"));
    }
    BKE_paint_layers_material_bake_apply(*bmain, *ma, row, size, channels, images);
  }

  /** A Roughness value edit of the source, as the node tree update would report it. */
  void edit_source(const float roughness)
  {
    ChannelUnavailableReason reason = ChannelUnavailableReason::None;
    const bNode *principled = BKE_paint_material_principled_find(*source, reason);
    ASSERT_NE(principled, nullptr);
    bNodeSocket *socket = bke::node_find_socket(
        const_cast<bNode &>(*principled), SOCK_IN, "Roughness"_ustr);
    ASSERT_NE(socket, nullptr);
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = roughness;
    /* The scene update compares a cheap signal first; the tests have no node tree update to bump it. */
    BKE_paint_layers_source_material_hash_forget(*source);
  }
};

static constexpr int base_color_roughness[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                               PAINT_MATERIAL_CHANNEL_ROUGHNESS};

TEST_F(PaintLayersMaterialBakeTest, the_source_hash_follows_values_and_not_locations)
{
  const uint64_t before = BKE_paint_layers_source_material_tree_hash(*source);
  ASSERT_NE(before, 0u);
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash(*source), before);

  for (bNode &node : source->nodetree->nodes) {
    node.location[0] += 120.0f;
    node.location[1] -= 40.0f;
  }
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash(*source), before)
      << "moving nodes changes nothing the graph evaluates";

  edit_source(0.123f);
  EXPECT_NE(BKE_paint_layers_source_material_tree_hash(*source), before);
}

TEST_F(PaintLayersMaterialBakeTest, the_cached_source_hash_is_the_hash_until_the_signal_moves)
{
  const uint64_t hash = BKE_paint_layers_source_material_tree_hash(*source);
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash_cached(*source), hash);
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash_cached(*source), hash);

  BKE_paint_layers_source_material_hash_forget(*source);
  EXPECT_EQ(BKE_paint_layers_source_material_tree_hash_cached(*source), hash);
}

TEST_F(PaintLayersMaterialBakeTest, a_row_with_no_maps_takes_part_in_nothing_and_is_due)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    EXPECT_FALSE(paint_layer_row_participates(*ma, *row, channel));
  }
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));
}

TEST_F(PaintLayersMaterialBakeTest, the_maps_handed_over_are_what_the_row_takes_part_through)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);

  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC))
      << "a channel the render gave no map for";
  Image *color = nullptr;
  ASSERT_TRUE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color));
  EXPECT_EQ(color, row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);

  /* A source without transparency covers fully: one white texel is the coverage. */
  ASSERT_NE(row->bake->coverage, nullptr);
  int width = 0;
  int height = 0;
  BKE_image_get_size(row->bake->coverage, nullptr, &width, &height);
  EXPECT_EQ(width, 1);
  EXPECT_EQ(height, 1);

  /* It is not a cache of a subtree: the generator and the composite do not substitute the row. */
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));
}

TEST_F(PaintLayersMaterialBakeTest, an_alpha_map_becomes_the_coverage_of_the_row)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  const int channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_ALPHA};
  uint32_t hash[2], source_hash[2];
  start_render(*row, channels, hash, source_hash);

  ASSERT_NE(row->bake->coverage, nullptr);
  int width = 0;
  int height = 0;
  BKE_image_get_size(row->bake->coverage, nullptr, &width, &height);
  EXPECT_EQ(width, 8) << "the rendered alpha map itself, not the one-texel white";
  EXPECT_EQ(row->bake->images[PAINT_MATERIAL_CHANNEL_ALPHA], nullptr)
      << "alpha is the row's coverage, not a channel of its own";
}

TEST_F(PaintLayersMaterialBakeTest, the_generator_reads_the_maps_and_keeps_the_rows_own_edits)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_correction_add(*ma, row, "Fix", MA_PAINT_LAYER_SOURCE_CONSTANT),
            nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  Image *color = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(PaintLayersGenerateTest::count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 4)
      << "the colour map and the roughness map, and the coverage each of them is laid by";

  /* Unlike a cache of a subtree, the row's opacity is a live value over the maps: editing it does
   * not turn the maps off. */
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(PaintLayersGenerateTest::count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 4);
  EXPECT_TRUE(BKE_paint_layers_values_sync(*ma)) << "a slider drag is a value, not a rebuild";
}

TEST_F(PaintLayersMaterialBakeTest, the_cpu_composite_lays_the_maps_with_the_coverage_as_the_mask)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_correction_add(*ma, row, "Fix", MA_PAINT_LAYER_SOURCE_CONSTANT),
            nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(layers[0].mask_image, row->bake->coverage);
  EXPECT_FALSE(layers[0].color_alpha_coverage);
  EXPECT_EQ(layers[0].content_corrections.size(), 1) << "the row's correction is still its own";
}

TEST_F(PaintLayersMaterialBakeTest, the_row_is_current_only_for_the_state_its_render_started_from)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);

  /* Handed over but not landed: the maps show, and the row still owes its render. */
  EXPECT_TRUE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr));
  EXPECT_TRUE(BKE_paint_layers_material_row_stale(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));

  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* The source moves on: the old maps keep showing until the new ones land. */
  edit_source(0.9f);
  EXPECT_TRUE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr));
  EXPECT_TRUE(BKE_paint_layers_material_row_stale(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* A render that started before that edit lands what it started from: stale against the source. */
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));
}

TEST_F(PaintLayersMaterialBakeTest, a_resized_or_repointed_row_does_not_keep_its_maps)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  Material *other = BKE_material_add(bmain, "Other");
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, other));
  EXPECT_FALSE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr))
      << "the maps are the previous source's";
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));

  /* A render that started from the old state lands nothing into the changed row. */
  EXPECT_FALSE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
}

TEST_F(PaintLayersMaterialBakeTest, a_render_that_did_not_land_is_not_started_again_as_is)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));

  EXPECT_FALSE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, false));
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row))
      << "the same state would fail the same way, and every scene update would start it";

  edit_source(0.31f);
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row)) << "a change lifts the hold";

  uint32_t again[2], again_source[2];
  BKE_paint_layers_material_bake_stamp_get(*ma, *row, again, again_source);
  ASSERT_FALSE(BKE_paint_layers_material_bake_land(*ma, row->marker, again, again_source, false));
  ASSERT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));
  BKE_paint_layers_material_bake_retry(*ma, *row);
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row)) << "an explicit retry lifts it too";
}

TEST_F(PaintLayersMaterialBakeTest, a_hidden_a_never_and_a_sourceless_row_are_not_due)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row)) << "nothing shows a hidden row";
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, true));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_NEVER));
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));

  /* The planner of the paint rows is not this row's: it renders nothing of it. */
  bool changed = false;
  BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed);
  EXPECT_FALSE(changed);

  PaintLayerAddParams paint_params;
  MaterialPaintLayer *paint = BKE_paint_layers_add_with_policy(*ma, paint_params);
  ASSERT_NE(paint, nullptr);
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *paint)) << "only a Material row reads one";
}

TEST_F(PaintLayersMaterialBakeTest, a_texture_of_the_source_painted_on_makes_the_row_stale)
{
  Image *texture = add_image("Texture");
  bNode *tex = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex, nullptr);
  tex->id = &texture->id;
  BKE_paint_layers_source_material_hash_forget(*source);

  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  ASSERT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));

  BKE_image_partial_update_mark_full_update(texture);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_TRUE(BKE_paint_layers_material_row_stale(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));
  EXPECT_TRUE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr))
      << "the old maps show until the new ones land";
}

TEST_F(PaintLayersMaterialBakeTest, a_copy_of_the_owner_starts_without_the_maps_of_the_row)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayer *copied = copy->paint_layers.first;
  ASSERT_NE(copied, nullptr);
  EXPECT_EQ(copied->material, source);
  EXPECT_FALSE(paint_layer_row_material_map(*copy, *copied, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr))
      << "the copy renders its own: its maps are not shared with the original's";
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*copy, *copied));
  EXPECT_TRUE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr))
      << "the original keeps its maps";
}

}  // namespace blender::bke::tests
