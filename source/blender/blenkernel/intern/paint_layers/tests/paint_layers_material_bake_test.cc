/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * A Material row's source: what the row lays per channel (a plain value and a plain texture of the
 * source live, the rest from maps rendered from it), the maps handed over before the render and
 * landed after it, read by the generator and the CPU composite, and kept or dropped as the source
 * and the row change. The render itself is the editor's and is not run here; the tests play its
 * two calls (#BKE_paint_layers_material_bake_apply and _land) with generated images.
 *
 * The fixture lives in `paint_layers_generate_test_fixture.hh`.
 */

#include "paint_layers_generate_test_fixture.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"

#include "../paint_layers_generate_intern.hh"
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
    principled_source_set_up(*source, /*graphs=*/true);
  }

  /**
   * A Principled BSDF into the Material Output. With \a graphs, an RGB node feeds Base Color and a
   * Value node Roughness: graphs the editor has to render. Every other channel is a plain value.
   */
  void principled_source_set_up(Material &material, const bool graphs)
  {
    bNodeTree &ntree = *material.nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(ntree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
    if (!graphs) {
      return;
    }
    bNode *rgb = bke::node_add_static_node(nullptr, ntree, SH_NODE_RGB);
    bNode *value = bke::node_add_static_node(nullptr, ntree, SH_NODE_VALUE);
    bke::node_add_link(ntree,
                       *rgb,
                       *bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
    bke::node_add_link(ntree,
                       *value,
                       *bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr));
  }

  /** The Principled of \a material. */
  static bNode *principled_of(Material &material)
  {
    ChannelUnavailableReason reason = ChannelUnavailableReason::None;
    return const_cast<bNode *>(BKE_paint_material_principled_find(material, reason));
  }

  /** An Image Texture of \a material feeding the Principled input \a socket from \a output. */
  bNode *texture_link(Material &material,
                      Image &image,
                      const char *socket,
                      const char *output = "Color")
  {
    bNodeTree &ntree = *material.nodetree;
    bNode *tex = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
    tex->id = &image.id;
    bNode *principled = principled_of(material);
    bke::node_add_link(
        ntree,
        *tex,
        *bke::node_find_socket(*tex, SOCK_OUT, UString::from_ptr_noinline(output)),
        *principled,
        *bke::node_find_socket(*principled, SOCK_IN, UString::from_ptr_noinline(socket)));
    BKE_paint_layers_source_material_hash_forget(material);
    return tex;
  }

  /**
   * A Material row on \a from. The tests of the maps run it in the Always mode, where the row
   * shows what is rendered; the tests of the source's own graph pass the Auto mode.
   */
  MaterialPaintLayer *add_material_row(Material &from,
                                       const int bake_mode = MA_PAINT_LAYER_BAKE_ALWAYS)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Material;
    params.source = &from;
    MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
    EXPECT_NE(row, nullptr);
    if (row != nullptr && bake_mode != MA_PAINT_LAYER_BAKE_AUTO) {
      EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, bake_mode));
    }
    return row;
  }

  /** The group holding the copy of a source graph for the row \a row, or null. */
  bNodeTree *source_group_of(const MaterialPaintLayer &row)
  {
    for (bNodeTree &tree : bmain->nodetrees) {
      bUUID marker = BLI_uuid_nil();
      if (paint_layers::source_group_row_get(tree, marker) && BLI_uuid_equal(marker, row.marker)) {
        return &tree;
      }
    }
    return nullptr;
  }

  /** The first node of \a type in \a tree. */
  static const bNode *node_of_type(const bNodeTree &tree, const int type)
  {
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == type) {
        return &node;
      }
    }
    return nullptr;
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

  /** A Metallic value edit of the source, as the node tree update would report it. */
  void edit_source(const float metallic)
  {
    bNode *principled = principled_of(*source);
    ASSERT_NE(principled, nullptr);
    bNodeSocket *socket = bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr);
    ASSERT_NE(socket, nullptr);
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = metallic;
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

TEST_F(PaintLayersMaterialBakeTest, a_row_shows_the_plain_values_of_its_source_before_any_render)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  /* The graphs wait for their maps; a plain value needs none. */
  EXPECT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_AO))
      << "the source has no Ambient Occlusion input";

  EXPECT_TRUE(BKE_paint_layers_material_channel_needs_render(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(
      BKE_paint_layers_material_channel_needs_render(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC));
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
  Image *color = nullptr;
  ASSERT_TRUE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color));
  EXPECT_EQ(color, row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_FALSE(paint_layer_row_material_map(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC, nullptr))
      << "a plain value has no map";

  /* The source's Alpha is a plain value too, so there is no coverage map either. */
  EXPECT_EQ(row->bake->coverage, nullptr);
  EXPECT_EQ(paint_layer_material_coverage_source(*ma, *row).kind,
            PaintLayerMaterialCoverageKind::LiveConstant);

  /* It is not a cache of a subtree: the generator and the composite do not substitute the row. */
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));
}

TEST_F(PaintLayersMaterialBakeTest, a_rendered_alpha_map_becomes_the_coverage_of_the_row)
{
  /* The source's Alpha is a graph, so it is rendered. */
  bNodeTree &ntree = *source->nodetree;
  bNode *value = bke::node_add_static_node(nullptr, ntree, SH_NODE_VALUE);
  bNode *principled = principled_of(*source);
  bke::node_add_link(ntree,
                     *value,
                     *bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr));
  BKE_paint_layers_source_material_hash_forget(*source);

  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(paint_layer_material_coverage_source(*ma, *row).kind,
            PaintLayerMaterialCoverageKind::Pending);
  EXPECT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC))
      << "the row waits for its coverage";
  const int channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_ALPHA};
  uint32_t hash[2], source_hash[2];
  start_render(*row, channels, hash, source_hash);
  EXPECT_EQ(paint_layer_material_coverage_source(*ma, *row).kind,
            PaintLayerMaterialCoverageKind::Baked);
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC));

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
  EXPECT_EQ(PaintLayersGenerateTest::count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 2)
      << "the colour map and the roughness map; the other channels are values of the source";
  const bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(
      interface_has_socket(const_cast<bNodeTree &>(*group), "Source Metallic Source", false));

  /* Unlike a cache of a subtree, the row's opacity is a live value over the maps: editing it does
   * not turn the maps off. */
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(PaintLayersGenerateTest::count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 2);
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
  EXPECT_EQ(layers[0].mask_image, nullptr) << "the source's Alpha is a value: no coverage map";
  EXPECT_FALSE(layers[0].color_alpha_coverage);
  EXPECT_EQ(layers[0].content_corrections.size(), 1) << "the row's correction is still its own";

  /* A plain value is the layer's constant colour. */
  Vector<PaintMaterialCompositeImageLayer> metallic;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, metallic));
  ASSERT_EQ(metallic.size(), 1);
  EXPECT_TRUE(metallic[0].has_constant_color);
  EXPECT_EQ(metallic[0].color_image, nullptr);
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

TEST_F(PaintLayersMaterialBakeTest, a_source_of_plain_values_needs_no_render_at_all)
{
  Material *plain = BKE_material_add(bmain, "Plain");
  principled_source_set_up(*plain, /*graphs=*/false);
  MaterialPaintLayer *row = add_material_row(*plain);
  ASSERT_NE(row, nullptr);

  EXPECT_FALSE(BKE_paint_layers_material_row_needs_render(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row)) << "nothing to render is up to date";
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(paint_layer_material_channel_source(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR).kind,
            PaintLayerMaterialSourceKind::LiveConstant);
}

TEST_F(PaintLayersMaterialBakeTest, a_plain_texture_of_the_source_is_shown_as_it_is)
{
  Material *textured = BKE_material_add(bmain, "Textured");
  principled_source_set_up(*textured, /*graphs=*/false);
  Image *texture = add_image("Rough");
  texture_link(*textured, *texture, "Roughness");
  MaterialPaintLayer *row = add_material_row(*textured);
  ASSERT_NE(row, nullptr);

  const PaintLayerMaterialSource source = paint_layer_material_channel_source(
      *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_EQ(source.kind, PaintLayerMaterialSourceKind::LiveImage);
  EXPECT_EQ(source.image, texture);
  EXPECT_FALSE(BKE_paint_layers_material_row_needs_render(*ma, *row));

  /* The generator reads the source's own image, and so does the CPU composite. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(PaintLayersGenerateTest::count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, texture);
}

TEST_F(PaintLayersMaterialBakeTest, a_texture_the_row_cannot_show_alone_is_rendered)
{
  Material *textured = BKE_material_add(bmain, "Textured");
  principled_source_set_up(*textured, /*graphs=*/false);
  Image *texture = add_image("Rough");
  bNode *tex = texture_link(*textured, *texture, "Roughness");
  /* A projection other than flat is a graph of its own. */
  static_cast<NodeTexImage *>(tex->storage)->projection = SHD_PROJ_BOX;
  BKE_paint_layers_source_material_hash_forget(*textured);
  MaterialPaintLayer *row = add_material_row(*textured);
  ASSERT_NE(row, nullptr);
  EXPECT_NE(paint_layer_material_channel_source(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS).kind,
            PaintLayerMaterialSourceKind::LiveImage);
  EXPECT_TRUE(
      BKE_paint_layers_material_channel_needs_render(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  /* The Alpha output into a colour channel is not the Color output the row reads. */
  Material *alpha_fed = BKE_material_add(bmain, "AlphaFed");
  principled_source_set_up(*alpha_fed, /*graphs=*/false);
  texture_link(*alpha_fed, *texture, "Base Color", "Alpha");
  MaterialPaintLayer *alpha_row = add_material_row(*alpha_fed);
  ASSERT_NE(alpha_row, nullptr);
  EXPECT_TRUE(BKE_paint_layers_material_channel_needs_render(
      *ma, *alpha_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintLayersMaterialBakeTest, the_coverage_follows_the_alpha_of_the_source)
{
  Material *half = BKE_material_add(bmain, "Half");
  principled_source_set_up(*half, /*graphs=*/false);
  bNodeSocket *alpha = bke::node_find_socket(*principled_of(*half), SOCK_IN, "Alpha"_ustr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.5f;
  BKE_paint_layers_source_material_hash_forget(*half);
  MaterialPaintLayer *row = add_material_row(*half);
  ASSERT_NE(row, nullptr);

  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(*ma, *row);
  EXPECT_EQ(coverage.kind, PaintLayerMaterialCoverageKind::LiveConstant);
  EXPECT_FLOAT_EQ(coverage.constant, 0.5f);

  /* The CPU folds a plain-value coverage into the row's opacity, as the graph multiplies it. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_FLOAT_EQ(layers[0].opacity, 0.5f);

  /* The Alpha output of the source's texture is the coverage as a map. */
  Material *textured = BKE_material_add(bmain, "AlphaTexture");
  principled_source_set_up(*textured, /*graphs=*/false);
  Image *texture = add_image("Cutout");
  texture_link(*textured, *texture, "Alpha", "Alpha");
  MaterialPaintLayer *cutout = add_material_row(*textured);
  ASSERT_NE(cutout, nullptr);
  const PaintLayerMaterialCoverage cutout_coverage = paint_layer_material_coverage_source(*ma,
                                                                                          *cutout);
  EXPECT_EQ(cutout_coverage.kind, PaintLayerMaterialCoverageKind::LiveAlpha);
  EXPECT_EQ(cutout_coverage.image, texture);
}

TEST_F(PaintLayersMaterialBakeTest, a_source_edit_that_changes_the_shape_marks_the_owner)
{
  Material *plain = BKE_material_add(bmain, "Plain");
  principled_source_set_up(*plain, /*graphs=*/false);
  MaterialPaintLayer *row = add_material_row(*plain);
  ASSERT_NE(row, nullptr);

  EXPECT_FALSE(BKE_paint_layers_material_sources_notice(*ma)) << "the first sight is not a change";
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  EXPECT_FALSE(BKE_paint_layers_material_sources_notice(*ma));

  /* A value that became a texture is other nodes. */
  texture_link(*plain, *add_image("Tex"), "Metallic");
  EXPECT_TRUE(BKE_paint_layers_material_sources_notice(*ma));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  EXPECT_FALSE(BKE_paint_layers_material_sources_notice(*ma));
}

TEST_F(PaintLayersMaterialBakeTest,
       a_value_edit_of_the_source_moves_the_live_input_without_a_rebuild)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_values_sync(*ma));

  edit_source(0.8f);
  EXPECT_TRUE(BKE_paint_layers_values_sync(*ma)) << "a value of the source is an input";
  PaintLayerMaterialSource metallic = paint_layer_material_channel_source(
      *ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC);
  ASSERT_EQ(metallic.kind, PaintLayerMaterialSourceKind::LiveConstant);
  EXPECT_FLOAT_EQ(metallic.constant[0], 0.8f);

  /* Once the value is a texture the group has the wrong shape and only a rebuild is right. */
  texture_link(*source, *add_image("Tex"), "Metallic");
  EXPECT_FALSE(BKE_paint_layers_values_sync(*ma));
}

TEST_F(PaintLayersMaterialBakeTest, a_graph_the_row_cannot_show_alone_is_the_graph_of_its_source)
{
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);

  /* In the generated graph the colour and the roughness are the source's graph, before any
   * render; the CPU composite has no shader evaluator and waits for the maps. */
  EXPECT_EQ(paint_layer_material_channel_source(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR).kind,
            PaintLayerMaterialSourceKind::SourceGroup);
  EXPECT_EQ(paint_layer_material_channel_source(
                *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, PaintLayerMaterialUse::Graph)
                .kind,
            PaintLayerMaterialSourceKind::SourceGroup);
  EXPECT_EQ(paint_layer_material_channel_source(
                *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayerMaterialUse::Composite)
                .kind,
            PaintLayerMaterialSourceKind::None);
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  /* The maps are still rendered for what reads maps. */
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row));
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  EXPECT_EQ(paint_layer_material_channel_source(
                *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayerMaterialUse::Composite)
                .kind,
            PaintLayerMaterialSourceKind::Baked);
  EXPECT_EQ(paint_layer_material_channel_source(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR).kind,
            PaintLayerMaterialSourceKind::SourceGroup)
      << "a landed render does not take the graph out of the shader";
}

TEST_F(PaintLayersMaterialBakeTest, the_always_mode_shows_the_render_and_the_never_mode_the_graph)
{
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_ALWAYS);
  ASSERT_NE(row, nullptr);
  EXPECT_NE(paint_layer_material_channel_source(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR).kind,
            PaintLayerMaterialSourceKind::SourceGroup);
  EXPECT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_NEVER));
  EXPECT_EQ(paint_layer_material_channel_source(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR).kind,
            PaintLayerMaterialSourceKind::SourceGroup);
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row)) << "nothing is rendered in Never";
}

TEST_F(PaintLayersMaterialBakeTest, the_source_graph_is_copied_without_its_principled_and_output)
{
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  const bNodeTree *group = source_group_of(*row);
  ASSERT_NE(group, nullptr);
  EXPECT_NE(node_of_type(*group, SH_NODE_RGB), nullptr);
  EXPECT_NE(node_of_type(*group, SH_NODE_VALUE), nullptr);
  EXPECT_EQ(node_of_type(*group, SH_NODE_BSDF_PRINCIPLED), nullptr);
  EXPECT_EQ(node_of_type(*group, SH_NODE_OUTPUT_MATERIAL), nullptr);
  EXPECT_TRUE(interface_has_socket(const_cast<bNodeTree &>(*group), "Base Color", true));
  EXPECT_TRUE(interface_has_socket(const_cast<bNodeTree &>(*group), "Roughness", true));
  EXPECT_FALSE(interface_has_socket(const_cast<bNodeTree &>(*group), "Metallic", true))
      << "a plain value feeds no node and is the row's live input";

  /* No image map is generated for the channels the graph shows. */
  EXPECT_EQ(PaintLayersGenerateTest::count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 0);
  const bNodeTree *layer_group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(layer_group, nullptr);
  EXPECT_NE(
      group_instance_find(const_cast<bNodeTree &>(*layer_group), const_cast<bNodeTree &>(*group)),
      nullptr);
  EXPECT_EQ(group->id.us, 1) << "the instance in the layer group is its only user";
}

TEST_F(PaintLayersMaterialBakeTest, an_edit_of_the_source_graph_reaches_the_copy_in_place)
{
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = source_group_of(*row);
  ASSERT_NE(group, nullptr);
  const int groups_before = layer_tree_count(*bmain);

  bNode *rgb = nullptr;
  for (bNode &node : source->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_RGB) {
      rgb = &node;
    }
  }
  ASSERT_NE(rgb, nullptr);
  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value[0] = 0.25f;
  BKE_paint_layers_source_material_hash_forget(*source);

  BKE_paint_layers_regenerate_tagged(*bmain);
  ASSERT_EQ(source_group_of(*row), group) << "the same data-block, refilled";
  EXPECT_EQ(layer_tree_count(*bmain), groups_before);
  const bNode *copy = node_of_type(*group, SH_NODE_RGB);
  ASSERT_NE(copy, nullptr);
  const bNodeSocket *copy_out = bke::node_find_socket(
      const_cast<bNode &>(*copy), SOCK_OUT, "Color"_ustr);
  EXPECT_FLOAT_EQ(static_cast<const bNodeSocketValueRGBA *>(copy_out->default_value)->value[0],
                  0.25f);
  EXPECT_EQ(group->id.us, 1);
}

TEST_F(PaintLayersMaterialBakeTest, a_row_that_stops_showing_the_graph_leaves_no_group_behind)
{
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(source_group_of(*row), nullptr);

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(source_group_of(*row), nullptr) << "pruned with the layer group that held it";
}

}  // namespace blender::bke::tests
