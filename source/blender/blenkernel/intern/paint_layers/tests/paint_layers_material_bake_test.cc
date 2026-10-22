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

#include <array>

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"

#include "NOD_socket.hh"

#include "DNA_node_tree_interface_types.h"

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

TEST_F(PaintLayersMaterialBakeTest, a_material_row_carries_its_mapping_into_the_source_wrapper)
{
  /* A UV reader: an Image Texture with an open Vector, so the row's mapping has something to move
   * while the source stays wrappable through its Principled. */
  texture_link(*source, *add_image("MappingSource"), "Metallic");
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(BKE_paint_layers_mapping_supported(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(*ma, *row));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper = source_group_of(*row);
  ASSERT_NE(wrapper, nullptr);
  /* The wrapper carries the three values the row group relays into it, so a slider edit is a
   * value edit and never a rebuild. */
  EXPECT_NE(paint_layers::source_group_mapping_input_find(
                *wrapper, paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET),
            nullptr);
  EXPECT_NE(paint_layers::source_group_mapping_input_find(
                *wrapper, paint_layers::SOURCE_GROUP_ROLE_MAPPING_SCALE),
            nullptr);
  EXPECT_NE(paint_layers::source_group_mapping_input_find(
                *wrapper, paint_layers::SOURCE_GROUP_ROLE_MAPPING_ROTATION),
            nullptr);

  /* Disabling it drops the inputs again and the next regeneration refills the wrapper unmapped. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, false));
  EXPECT_FALSE(BKE_paint_layers_mapping_applies(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  wrapper = source_group_of(*row);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_EQ(paint_layers::source_group_mapping_input_find(
                *wrapper, paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET),
            nullptr);
  EXPECT_FALSE(paint_layers::source_group_mapping_present(*wrapper));
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

TEST_F(PaintLayersMaterialBakeTest, a_stroke_on_the_mask_of_the_row_does_not_make_the_row_stale)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  Image *mask_image = add_image("MaskBase");
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_image));

  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  ASSERT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));

  /* The mask stays live over the maps: what the render reads is the source, not the row's mask. */
  BKE_image_partial_update_mark_full_update(mask_image);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));
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

TEST_F(PaintLayersMaterialBakeTest, the_editors_mark_makes_the_result_stale_until_it_is_cleared)
{
  ASSERT_NE(add_material_row(*source, MA_PAINT_LAYER_BAKE_NEVER), nullptr);
  BKE_paint_layers_bake_stale_clear(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_scheduled_get(*ma));
  EXPECT_FALSE(BKE_paint_layers_is_stale(*ma)) << "nothing is waiting and nothing is due";

  BKE_paint_layers_bake_scheduled_set(*ma, true);
  EXPECT_TRUE(BKE_paint_layers_bake_scheduled_get(*ma));
  EXPECT_TRUE(BKE_paint_layers_is_stale(*ma));

  BKE_paint_layers_bake_scheduled_set(*ma, false);
  EXPECT_FALSE(BKE_paint_layers_is_stale(*ma));
  EXPECT_FALSE(BKE_paint_layers_is_stale(*source)) << "a plain material has no paint layer bake";
}

TEST_F(PaintLayersMaterialBakeTest, a_render_nobody_started_makes_the_result_stale_until_it_lands)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_bake_stale_clear(*ma);
  EXPECT_TRUE(BKE_paint_layers_bake_outstanding(*ma));
  EXPECT_TRUE(BKE_paint_layers_is_stale(*ma)) << "the maps of the graph are due";

  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness, hash, source_hash);
  EXPECT_TRUE(BKE_paint_layers_is_stale(*ma)) << "handed over, not landed";

  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  EXPECT_FALSE(BKE_paint_layers_bake_outstanding(*ma));
  EXPECT_FALSE(BKE_paint_layers_is_stale(*ma));

  edit_source(0.9f);
  EXPECT_TRUE(BKE_paint_layers_is_stale(*ma)) << "an edit of the source makes the maps due again";
}

TEST_F(PaintLayersMaterialBakeTest, the_gate_waits_for_a_person_and_refuses_a_script)
{
  using Action = PaintLayersBakeGateAction;
  /* stale, invoke, headless */
  EXPECT_EQ(BKE_paint_layers_bake_gate_decide(false, false, false), Action::Proceed);
  EXPECT_EQ(BKE_paint_layers_bake_gate_decide(false, true, false), Action::Proceed);
  EXPECT_EQ(BKE_paint_layers_bake_gate_decide(true, true, false), Action::Wait);
  EXPECT_EQ(BKE_paint_layers_bake_gate_decide(true, false, false), Action::Refuse);
  EXPECT_EQ(BKE_paint_layers_bake_gate_decide(true, true, true), Action::Proceed)
      << "nothing headless makes a stale result fresh";
  EXPECT_EQ(BKE_paint_layers_bake_gate_decide(true, false, true), Action::Proceed);
}

TEST_F(PaintLayersMaterialBakeTest, a_copy_does_not_inherit_the_editors_mark)
{
  BKE_paint_layers_bake_scheduled_set(*ma, true);
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_scheduled_get(*ma));
  EXPECT_FALSE(BKE_paint_layers_bake_scheduled_get(*copy))
      << "the copy has no timer and no job of its own";
}

TEST_F(PaintLayersMaterialBakeTest, an_edit_of_a_source_concerns_the_materials_whose_rows_read_it)
{
  ASSERT_NE(add_material_row(*source), nullptr);
  Material *other = BKE_material_add(bmain, "Other");

  Vector<Material *> consumers;
  BKE_paint_layers_source_material_consumers(*bmain, *source, consumers);
  ASSERT_EQ(consumers.size(), 1);
  EXPECT_EQ(consumers[0], ma);

  consumers.clear();
  BKE_paint_layers_source_material_consumers(*bmain, *other, consumers);
  EXPECT_TRUE(consumers.is_empty());
}

/* -------------------------------------------------------------------- */
/** \name A Bump as the source's Normal
 *
 * A normal made by a Bump has no live form (it comes from screen-space derivatives), so the row
 * shows the map rendered from it while every other channel stays the source's graph. Only what the
 * Normal reads can make that map stale.
 * \{ */

/**
 * A Value node feeding the Height of a Bump that feeds the Normal of the Principled of \a material.
 * \return the Value node, the one input of the Normal that is a plain number to drag.
 */
static bNode *bump_normal_link(Material &material)
{
  bNodeTree &ntree = *material.nodetree;
  bNode *principled = PaintLayersMaterialBakeTest::principled_of(material);
  bNode *value = bke::node_add_static_node(nullptr, ntree, SH_NODE_VALUE);
  bNode *bump = bke::node_add_static_node(nullptr, ntree, SH_NODE_BUMP);
  bke::node_add_link(ntree,
                     *value,
                     *bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr),
                     *bump,
                     *bke::node_find_socket(*bump, SOCK_IN, "Height"_ustr));
  bke::node_add_link(ntree,
                     *bump,
                     *bke::node_find_socket(*bump, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));
  BKE_paint_layers_source_material_hash_forget(material);
  return value;
}

/** Drag the number of \a value node, as the node tree update would report it. */
static void value_node_set(Material &material, bNode &value, const float number)
{
  bNodeSocket *out = bke::node_find_socket(value, SOCK_OUT, "Value"_ustr);
  static_cast<bNodeSocketValueFloat *>(out->default_value)->value = number;
  BKE_paint_layers_source_material_hash_forget(material);
}

static constexpr int base_color_roughness_normal[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                                      PAINT_MATERIAL_CHANNEL_NORMAL};

TEST_F(PaintLayersMaterialBakeTest, the_normal_hash_follows_what_the_normal_reads_and_nothing_else)
{
  bNode *height = bump_normal_link(*source);
  const uint64_t before = BKE_paint_layers_source_normal_hash_cached(*source);
  ASSERT_NE(before, 0u);

  edit_source(0.7f);
  EXPECT_EQ(BKE_paint_layers_source_normal_hash_cached(*source), before)
      << "a Metallic the Normal does not read";

  for (bNode &node : source->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_RGB) {
      bNodeSocket *out = bke::node_find_socket(node, SOCK_OUT, "Color"_ustr);
      static_cast<bNodeSocketValueRGBA *>(out->default_value)->value[0] = 0.9f;
    }
  }
  BKE_paint_layers_source_material_hash_forget(*source);
  EXPECT_EQ(BKE_paint_layers_source_normal_hash_cached(*source), before)
      << "the colour feeds the Base Color, not the Normal";

  value_node_set(*source, *height, 0.5f);
  EXPECT_NE(BKE_paint_layers_source_normal_hash_cached(*source), before)
      << "the Height of the Bump is what the Normal reads";
}

TEST_F(PaintLayersMaterialBakeTest, the_structure_hash_holds_while_socket_values_move)
{
  const uint64_t structure = BKE_paint_layers_source_material_structure_hash(*source);
  const uint64_t content = BKE_paint_layers_source_material_tree_hash(*source);
  ASSERT_NE(structure, 0u);

  edit_source(0.4f);
  EXPECT_EQ(BKE_paint_layers_source_material_structure_hash(*source), structure);
  EXPECT_EQ(BKE_paint_layers_source_structure_hash_cached(*source), structure);
  EXPECT_NE(BKE_paint_layers_source_material_tree_hash(*source), content);

  texture_link(*source, *add_image("Tex"), "Metallic");
  EXPECT_NE(BKE_paint_layers_source_material_structure_hash(*source), structure)
      << "a value that became a texture is other nodes";
}

TEST_F(PaintLayersMaterialBakeTest, a_value_of_the_source_is_synced_into_its_group_in_place)
{
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = source_group_of(*row);
  ASSERT_NE(group, nullptr);

  uint64_t stored = 0;
  ASSERT_TRUE(paint_layers::source_group_structure_hash_get(*group, stored));
  EXPECT_EQ(stored, paint_layers::source_group_structure_hash(*source));

  bNode *rgb = nullptr;
  for (bNode &node : source->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_RGB) {
      rgb = &node;
    }
  }
  ASSERT_NE(rgb, nullptr);
  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value[0] = 0.125f;
  BKE_paint_layers_source_material_hash_forget(*source);

  EXPECT_EQ(paint_layers::source_group_structure_hash(*source), stored)
      << "a dragged value keeps the structure the group was filled from";
  ASSERT_TRUE(paint_layers::source_group_values_sync(*bmain, *group, *source));
  const bNode *copy = node_of_type(*group, SH_NODE_RGB);
  ASSERT_NE(copy, nullptr);
  const bNodeSocket *copy_out = bke::node_find_socket(
      const_cast<bNode &>(*copy), SOCK_OUT, "Color"_ustr);
  EXPECT_FLOAT_EQ(static_cast<const bNodeSocketValueRGBA *>(copy_out->default_value)->value[0],
                  0.125f);

  texture_link(*source, *add_image("Tex"), "Metallic");
  EXPECT_NE(paint_layers::source_group_structure_hash(*source), stored)
      << "other nodes are a refill, not a sync";
}

TEST_F(PaintLayersMaterialBakeTest, a_bump_normal_is_shown_from_its_map_and_the_rest_live)
{
  bump_normal_link(*source);
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);

  EXPECT_FALSE(paint_layers::source_group_normal_live(*source));
  EXPECT_TRUE(BKE_paint_layers_material_channel_needs_render(
      *ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_FALSE(paint_layer_material_source_group_channel(*ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_TRUE(
      paint_layer_material_source_group_channel(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(paint_layer_material_row_needs_map(*ma, *row));

  /* Without a Bump nothing is left to the render. */
  Material *plain = BKE_material_add(bmain, "Plain");
  principled_source_set_up(*plain, /*graphs=*/true);
  MaterialPaintLayer *plain_row = add_material_row(*plain, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(plain_row, nullptr);
  EXPECT_FALSE(paint_layer_material_row_needs_map(*ma, *plain_row));
}

TEST_F(PaintLayersMaterialBakeTest, an_edit_the_normal_does_not_read_renders_nothing)
{
  bNode *height = bump_normal_link(*source);
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(paint_layer_material_row_needs_map(*ma, *row));
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness_normal, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  ASSERT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));

  edit_source(0.9f);
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row)) << "the Metallic is not the Normal";
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));

  value_node_set(*source, *height, 0.75f);
  EXPECT_TRUE(BKE_paint_layers_material_row_stale(*ma, *row)) << "the Height is";
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row))
      << "the row being edited renders a map nothing live stands in for";
}

TEST_F(PaintLayersMaterialBakeTest, an_image_editor_closing_does_not_make_the_row_stale)
{
  bump_normal_link(*source);
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);

  /* A render made while an Image Editor reads the maps, which then closes. */
  BKE_paint_layers_preview_wanted_set(*ma, true);
  uint32_t hash[2], source_hash[2];
  start_render(*row, base_color_roughness_normal, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));

  BKE_paint_layers_preview_wanted_set(*ma, false);
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row))
      << "the state of the editor is not an edit of the source";
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));

  /* While one reads the maps, a colour edit has to reach them. */
  BKE_paint_layers_preview_wanted_set(*ma, true);
  edit_source(0.2f);
  EXPECT_TRUE(BKE_paint_layers_material_row_stale(*ma, *row));
}

TEST_F(PaintLayersMaterialBakeTest, only_the_normal_is_rendered_until_something_reads_the_other_maps)
{
  bump_normal_link(*source);
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);

  EXPECT_TRUE(BKE_paint_layers_material_channel_render_wanted(
      *ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_FALSE(BKE_paint_layers_material_channel_render_wanted(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
      << "the shader shows the colour live and nothing reads its map";
  EXPECT_FALSE(BKE_paint_layers_material_channel_render_wanted(
      *ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC))
      << "a plain value needs no map at all";

  const int normal_only[] = {PAINT_MATERIAL_CHANNEL_NORMAL};
  uint32_t hash[2], source_hash[2];
  start_render(*row, normal_only, hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));

  /* An Image Editor opens: its composite reads every map, which this render did not make. */
  BKE_paint_layers_preview_wanted_set(*ma, true);
  EXPECT_TRUE(BKE_paint_layers_material_channel_render_wanted(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(BKE_paint_layers_material_row_stale(*ma, *row));

  /* The full render that follows is current for the editor and for it closing again. */
  uint32_t full_hash[2], full_source_hash[2];
  start_render(*row, base_color_roughness_normal, full_hash, full_source_hash);
  ASSERT_TRUE(
      BKE_paint_layers_material_bake_land(*ma, row->marker, full_hash, full_source_hash, true));
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));
  BKE_paint_layers_preview_wanted_set(*ma, false);
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));
}

TEST_F(PaintLayersMaterialBakeTest, the_edited_row_waits_for_a_map_only_the_render_can_show)
{
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_FLOAT_EQ(BKE_paint_layers_source_edit_delay(*ma, *source), 0.0f)
      << "the graph is live: an edit has nothing to wait for";

  bump_normal_link(*source);
  const float delay = BKE_paint_layers_source_edit_delay(*ma, *source);
  EXPECT_GT(delay, 0.0f) << "a Bump has no live form: its map is rendered after the hand stops";
  EXPECT_LT(delay, 5.0f) << "shorter than the delay of the Image Editor's preview";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Row UV mapping
 * \{ */

/** A Texture Coordinate whose UV feeds the Vector input of \a tex. */
static bNode *tex_coord_feed(bNodeTree &tree, bNode &tex)
{
  bNode *coord = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_COORD);
  bke::node_add_link(tree,
                     *coord,
                     *bke::node_find_socket(*coord, SOCK_OUT, "UV"_ustr),
                     tex,
                     *bke::node_find_socket(tex, SOCK_IN, "Vector"_ustr));
  return coord;
}

TEST_F(PaintLayersMaterialBakeTest, uv_readers_are_found_by_their_links_and_not_by_their_presence)
{
  /* A source of plain values reads no UV at all. */
  EXPECT_FALSE(paint_layers::uv_readers_find(*source->nodetree).any());
  EXPECT_FALSE(BKE_paint_layers_source_reads_uv(*source));

  /* A texture feeding the shader with an open Vector samples the active UV. */
  bNode *tex = texture_link(*source, *add_image("Reader"), "Metallic");
  paint_layers::UvReaders readers = paint_layers::uv_readers_find(*source->nodetree);
  EXPECT_TRUE(readers.sources.is_empty());
  ASSERT_EQ(readers.open_images.size(), 1);
  EXPECT_EQ(readers.open_images[0], tex);
  EXPECT_TRUE(BKE_paint_layers_source_reads_uv(*source));

  /* Fed from a Texture Coordinate it is no longer open: the coordinate is the reader. */
  bNode *coord = tex_coord_feed(*source->nodetree, *tex);
  BKE_paint_layers_source_material_hash_forget(*source);
  readers = paint_layers::uv_readers_find(*source->nodetree);
  EXPECT_TRUE(readers.open_images.is_empty());
  ASSERT_EQ(readers.sources.size(), 1);
  EXPECT_EQ(readers.sources[0], coord);
  EXPECT_TRUE(BKE_paint_layers_source_reads_uv(*source));
}

TEST_F(PaintLayersMaterialBakeTest, an_orphan_texture_and_a_named_uv_map_read_nothing_the_row_moves)
{
  Material *other = BKE_material_add(bmain, "Other");
  principled_source_set_up(*other, /*graphs=*/false);
  bNodeTree &ntree = *other->nodetree;

  /* Not connected to the shader: not part of it. */
  bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  EXPECT_FALSE(paint_layers::uv_readers_find(ntree).any());

  /* A named layer is the source's own; the texture's Vector is not open either. */
  bNode *tex = texture_link(*other, *add_image("Named"), "Metallic");
  bNode *uv_map = bke::node_add_static_node(nullptr, ntree, SH_NODE_UVMAP);
  STRNCPY(static_cast<NodeShaderUVMap *>(uv_map->storage)->uv_map, "Other UV");
  bke::node_add_link(ntree,
                     *uv_map,
                     *bke::node_find_socket(*uv_map, SOCK_OUT, "UV"_ustr),
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_IN, "Vector"_ustr));
  BKE_paint_layers_source_material_hash_forget(*other);
  EXPECT_FALSE(paint_layers::uv_readers_find(ntree).any());
  EXPECT_FALSE(BKE_paint_layers_source_reads_uv(*other));

  /* The same node with no layer named is the active UV. */
  static_cast<NodeShaderUVMap *>(uv_map->storage)->uv_map[0] = '\0';
  BKE_paint_layers_source_material_hash_forget(*other);
  EXPECT_EQ(paint_layers::uv_readers_find(ntree).sources.size(), 1);
  EXPECT_TRUE(BKE_paint_layers_source_reads_uv(*other));
}

TEST_F(PaintLayersMaterialBakeTest, the_mapped_flag_moves_both_source_group_hashes)
{
  EXPECT_NE(paint_layers::source_group_content_hash(*source, false),
            paint_layers::source_group_content_hash(*source, true));
  EXPECT_NE(paint_layers::source_group_structure_hash(*source, false),
            paint_layers::source_group_structure_hash(*source, true));
  EXPECT_EQ(paint_layers::source_group_content_hash(*source, true),
            paint_layers::source_group_content_hash(*source, true))
      << "the same flag over the same source is the same hash: a slider never refills";
}

TEST_F(PaintLayersMaterialBakeTest, the_mapping_chain_is_marked_and_a_plain_node_is_not)
{
  bNodeTree *tree = bke::node_tree_add_tree(bmain, "MappingChain", "ShaderNodeTree");
  ASSERT_NE(tree, nullptr);
  bNode *coord = bke::node_add_static_node(nullptr, *tree, SH_NODE_TEX_COORD);
  bNodeSocket *uv = bke::node_find_socket(*coord, SOCK_OUT, "UV"_ustr);
  ASSERT_NE(uv, nullptr);

  const paint_layers::MappingChain chain = paint_layers::mapping_chain_add(
      *tree, *coord, *uv, nullptr, nullptr, nullptr, nullptr);
  ASSERT_NE(chain.rotate, nullptr);
  ASSERT_NE(chain.mapping, nullptr);
  EXPECT_TRUE(paint_layers::node_is_mapping_chain(*chain.rotate));
  EXPECT_TRUE(paint_layers::node_is_mapping_chain(*chain.mapping));
  EXPECT_FALSE(paint_layers::node_is_mapping_chain(*coord))
      << "the marker, not the name, tells a generated node from the source's own";

  /* UV -> Rotate -> Mapping. */
  int links_into_chain = 0;
  for (const bNodeLink &link : tree->links) {
    if (link.tonode == chain.rotate && link.fromnode == coord) {
      links_into_chain++;
    }
    if (link.tonode == chain.mapping && link.fromnode == chain.rotate) {
      links_into_chain++;
    }
  }
  EXPECT_EQ(links_into_chain, 2);
}

TEST_F(PaintLayersMaterialBakeTest, the_normal_remap_builds_in_a_tree_with_no_topology_cache)
{
  /* The regression of a crash: nothing here may ask a socket for its owner node, the tree being
   * built has no cache for it. */
  bNodeTree *tree = bke::node_tree_add_tree(bmain, "NormalRemap", "ShaderNodeTree");
  ASSERT_NE(tree, nullptr);
  bNode *tex = bke::node_add_static_node(nullptr, *tree, SH_NODE_TEX_IMAGE);
  bNodeSocket *color = bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr);
  ASSERT_NE(color, nullptr);
  bNode *coord = bke::node_add_static_node(nullptr, *tree, SH_NODE_TEX_COORD);
  bNodeSocket *uv = bke::node_find_socket(*coord, SOCK_OUT, "UV"_ustr);

  Vector<bNode *> made;
  bNode *encode = nullptr;
  bNodeSocket *encoded = paint_layers::normal_remap_nodes_add(
      *tree, *tex, *color, coord, uv, coord, uv, made, encode);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(made.size(), 5);
  ASSERT_NE(encode, nullptr);
  EXPECT_EQ(made.last(), encode);

  /* With no rotation and no scale it is still complete: the identity transform. */
  Vector<bNode *> made_plain;
  bNode *encode_plain = nullptr;
  EXPECT_NE(paint_layers::normal_remap_nodes_add(
                *tree, *tex, *color, nullptr, nullptr, nullptr, nullptr, made_plain, encode_plain),
            nullptr);
  EXPECT_EQ(made_plain.size(), 5);
}

TEST_F(PaintLayersMaterialBakeTest, a_mapped_wrapper_feeds_its_open_texture_through_the_chain)
{
  texture_link(*source, *add_image("MappedSource"), "Metallic");
  MaterialPaintLayer *row = add_material_row(*source, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *wrapper = source_group_of(*row);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_TRUE(paint_layers::source_group_mapping_present(*wrapper));
  const bNode *tex = node_of_type(*wrapper, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex, nullptr);
  const bNodeSocket *vector = bke::node_find_socket(*tex, SOCK_IN, "Vector"_ustr);
  ASSERT_NE(vector, nullptr);
  bool fed_by_chain = false;
  for (const bNodeLink &link : wrapper->links) {
    if (link.tosock == vector && paint_layers::node_is_mapping_chain(*link.fromnode)) {
      fed_by_chain = true;
    }
  }
  EXPECT_TRUE(fed_by_chain) << "the texture's open Vector reads the UV the row mapping moved";
}

/**
 * A material whose Principled BSDF sits inside a group: Group -> Material Output. The group's
 * tree is \a r_group, which the caller may add nodes to.
 */
static Material *grouped_source_add(Main &bmain, bNodeTree *&r_group, bNode *&r_principled)
{
  Material *material = BKE_material_add(&bmain, "Grouped");
  bNodeTree &root = *material->nodetree;
  bNodeTree *group = bke::node_tree_add_tree(&bmain, "PathGroup", "ShaderNodeTree");
  bNodeTreeInterfaceSocket *iface = group->tree_interface.add_socket(
      "BSDF", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNode *principled = bke::node_add_static_node(nullptr, *group, SH_NODE_BSDF_PRINCIPLED);
  bNode *group_out = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  nodes::update_node_declaration_and_sockets(*group, *group_out);
  bke::node_add_link(*group,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *group_out,
                     *bke::node_find_socket(
                         *group_out, SOCK_IN, UString::from_ptr_noinline(iface->identifier)));

  bNode *instance = bke::node_add_node(nullptr, root, root.typeinfo->group_idname);
  instance->id = &group->id;
  id_us_plus(&group->id);
  nodes::update_node_declaration_and_sockets(root, *instance);
  bNode *output = bke::node_add_static_node(nullptr, root, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(root,
                     *instance,
                     *bke::node_find_socket(
                         *instance, SOCK_OUT, UString::from_ptr_noinline(iface->identifier)),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  r_group = group;
  r_principled = principled;
  return material;
}

TEST_F(PaintLayersMaterialBakeTest, a_group_on_the_path_to_the_principled_is_a_level_the_mapping_moves)
{
  bNodeTree *group = nullptr;
  bNode *principled = nullptr;
  Material *grouped = grouped_source_add(*bmain, group, principled);
  ASSERT_NE(group, nullptr);
  EXPECT_FALSE(BKE_paint_layers_source_reads_uv(*grouped));

  /* An image with an open Vector inside the group feeds the Principled: the fill copies that
   * level privately and puts the chain in front of it. */
  bNode *tex = bke::node_add_static_node(nullptr, *group, SH_NODE_TEX_IMAGE);
  tex->id = &add_image("InGroup")->id;
  bke::node_add_link(*group,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));
  BKE_paint_layers_source_material_hash_forget(*grouped);
  EXPECT_TRUE(paint_layers::uv_readers_find(*group).any());
  EXPECT_TRUE(BKE_paint_layers_source_reads_uv(*grouped));
}

TEST_F(PaintLayersMaterialBakeTest, a_group_beside_the_path_is_shared_and_its_uv_read_is_not_offered)
{
  Material *other = BKE_material_add(bmain, "Beside");
  principled_source_set_up(*other, /*graphs=*/false);
  bNodeTree &root = *other->nodetree;

  /* A group that reads the UV, used by a node of the root that is not on the Principled's path. */
  bNodeTree *side = bke::node_tree_add_tree(bmain, "SideGroup", "ShaderNodeTree");
  bNode *tex = bke::node_add_static_node(nullptr, *side, SH_NODE_TEX_IMAGE);
  tex->id = &add_image("InSide")->id;
  bNode *math = bke::node_add_static_node(nullptr, *side, SH_NODE_MATH);
  bke::node_add_link(*side,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Alpha"_ustr),
                     *math,
                     *bke::node_find_socket(*math, SOCK_IN, "Value"_ustr));
  bNode *instance = bke::node_add_node(nullptr, root, root.typeinfo->group_idname);
  instance->id = &side->id;
  id_us_plus(&side->id);
  BKE_paint_layers_source_material_hash_forget(*other);

  EXPECT_TRUE(paint_layers::uv_readers_find(*side).any())
      << "the group does read the UV: it is the walk, not the finder, that leaves it out";
  EXPECT_FALSE(BKE_paint_layers_source_reads_uv(*other))
      << "a group beside the path is shared as is, so its read is never moved";
}

TEST_F(PaintLayersMaterialBakeTest, a_clipping_wrap_reaches_the_cpu_coverage_and_the_graph_alike)
{
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 8, 8, "Red", 32, false, IMA_GENTYPE_BLANK, red, false, false, false);
  Material *textured = BKE_material_add(bmain, "Textured");
  principled_source_set_up(*textured, /*graphs=*/false);
  texture_link(*textured, *image, "Base Color");

  MaterialPaintLayer *row = add_material_row(*textured, MA_PAINT_LAYER_BAKE_AUTO);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  /* Scale two: the source covers the lower-left quarter of the canvas once. */
  const float scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));

  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
  const auto composite_at = [&](const int x, const int y) {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(
        BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    EXPECT_FALSE(layers.is_empty());
    int width = 0;
    int height = 0;
    EXPECT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, width, height));
    Vector<float> linear(int64_t(width) * height * 4);
    EXPECT_TRUE(BKE_paint_material_composite_eval_images_linear(
        layers, PaintMaterialCompositeLinearTarget::frame(linear, width), nullptr, nullptr, bottom));
    const float *p = linear.data() + (int64_t(y) * width + x) * 4;
    return std::array<float, 4>{p[0], p[1], p[2], p[3]};
  };

  /* Repeat: the pattern tiles, so the far corner is the source too. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT));
  EXPECT_NEAR(composite_at(6, 6)[0], 1.0f, 1e-3f);
  EXPECT_NEAR(composite_at(6, 6)[1], 0.0f, 1e-3f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  const int repeat_clip_nodes = count_type(*ma->paint_layers_tree, SH_NODE_SEPXYZ);

  /* Clip: the CPU leaves the far corner to what is below, and the graph gains its clip factor. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_TRUE(layers.last().mapping_clips_coverage);
  EXPECT_NEAR(composite_at(1, 1)[0], 1.0f, 1e-3f) << "inside the square the source is shown";
  EXPECT_NEAR(composite_at(1, 1)[1], 0.0f, 1e-3f);
  EXPECT_NEAR(composite_at(6, 6)[0], bottom[0], 1e-3f) << "outside it the rows below show";
  EXPECT_NEAR(composite_at(6, 6)[1], bottom[1], 1e-3f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_GT(count_type(*ma->paint_layers_tree, SH_NODE_SEPXYZ), repeat_clip_nodes)
      << "the same setting adds the generated graph's clip factor";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name A content correction that reads a material
 * \{ */

class PaintLayersMaterialCorrectionTest : public PaintLayersMaterialBakeTest {
 public:
  /** A Paint row named "Base" with a Material correction named "Mat" reading the fixture's source.
   */
  MaterialPaintLayer *add_row_with_correction(MaterialPaintLayer **r_effect)
  {
    PaintLayerAddParams params;
    params.name = "Base";
    MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
    EXPECT_NE(row, nullptr);
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add_material(*ma, row, "Mat", source);
    EXPECT_NE(effect, nullptr);
    *r_effect = effect;
    return row;
  }
};

TEST_F(PaintLayersMaterialCorrectionTest, it_hangs_on_a_row_and_owns_no_records)
{
  MaterialPaintLayer *effect = nullptr;
  MaterialPaintLayer *row = add_row_with_correction(&effect);
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_EQ(effect->material, source);
  EXPECT_EQ(effect->channels_num, 0) << "what it lays is what its source supplies";
  EXPECT_EQ(BKE_paint_layers_correction_owner(*ma, *effect), row);
  EXPECT_EQ(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);

  const int users = source->id.us;
  EXPECT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  EXPECT_EQ(source->id.us, users - 1) << "the correction was a user of its source";
}

TEST_F(PaintLayersMaterialCorrectionTest, it_refuses_itself_a_cycle_and_a_missing_source)
{
  PaintLayerAddParams params;
  MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add_material(*ma, row, "Self", ma), nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add_material(*ma, row, "None", nullptr), nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add_material(*ma, nullptr, "Orphan", source), nullptr);

  /* The source reads this material through a correction of its own: reading it back is a cycle. */
  PaintLayerAddParams source_params;
  MaterialPaintLayer *source_row = BKE_paint_layers_add_with_policy(*source, source_params);
  ASSERT_NE(source_row, nullptr);
  ASSERT_NE(BKE_paint_layers_correction_add_material(*source, source_row, "Back", ma), nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add_material(*ma, row, "Cycle", source), nullptr);
  EXPECT_TRUE(BKE_paint_layers_material_depends_on(*source, *ma));
}

TEST_F(PaintLayersMaterialCorrectionTest, it_lays_the_channels_its_source_supplies_as_values)
{
  MaterialPaintLayer *effect = nullptr;
  MaterialPaintLayer *row = add_row_with_correction(&effect);
  ASSERT_NE(effect, nullptr);
  edit_source(0.3f);

  EXPECT_TRUE(paint_layer_effect_lays(*ma, *effect, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_TRUE(paint_layer_effects_lay(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(paint_layer_effect_lays(*ma, *effect, PAINT_MATERIAL_CHANNEL_NORMAL))
      << "a plain value is no tangent normal";
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC))
      << "an unpainted row is kept for its corrections";
  EXPECT_TRUE(BKE_paint_layers_material_row_needs_render(*ma, *effect))
      << "the graphs of the fixture's source are rendered, its values are not";
  EXPECT_FALSE(BKE_paint_layers_material_channel_needs_render(
      *ma, *effect, PAINT_MATERIAL_CHANNEL_METALLIC));
}

TEST_F(PaintLayersMaterialCorrectionTest, a_source_whose_transparency_is_a_map_lays_nothing)
{
  MaterialPaintLayer *effect = nullptr;
  add_row_with_correction(&effect);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(paint_layer_effect_lays(*ma, *effect, PAINT_MATERIAL_CHANNEL_METALLIC));

  texture_link(*source, *add_image("Cutout"), "Alpha", "Alpha");
  EXPECT_FALSE(paint_layer_effect_lays(*ma, *effect, PAINT_MATERIAL_CHANNEL_METALLIC))
      << "the correction has no coverage of its own to carry a map";
  EXPECT_FALSE(BKE_paint_layers_material_row_needs_render(*ma, *effect))
      << "and so there is nothing of it to render";
}

TEST_F(PaintLayersMaterialCorrectionTest, the_generator_gives_the_correction_a_live_input)
{
  MaterialPaintLayer *effect = nullptr;
  add_row_with_correction(&effect);
  ASSERT_NE(effect, nullptr);
  edit_source(0.3f);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Base");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(interface_has_socket(*group, "Base Mat Metallic Source", false))
      << "a plain value of the source is an input of the row's group";
  EXPECT_TRUE(interface_has_socket(*group, "Base Mat Metallic Opacity", false));

  /* Editing the value in the source reaches the input without a rebuild. */
  edit_source(0.8f);
  EXPECT_TRUE(BKE_paint_layers_values_sync(*ma));
  const bNodeTreeInterfaceSocket *live = interface_input_find(*group, "Base Mat Metallic Source");
  ASSERT_NE(live, nullptr);
  ASSERT_NE(live->socket_data, nullptr);
  EXPECT_FLOAT_EQ(static_cast<const bNodeSocketValueRGBA *>(live->socket_data)->value[0], 0.8f);
}

TEST_F(PaintLayersMaterialCorrectionTest, the_cpu_composite_carries_the_value_as_a_correction)
{
  MaterialPaintLayer *effect = nullptr;
  add_row_with_correction(&effect);
  ASSERT_NE(effect, nullptr);
  edit_source(0.3f);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_EQ(layers[0].content_corrections.size(), 1);
  const PaintMaterialCompositeCorrection &correction = layers[0].content_corrections[0];
  EXPECT_TRUE(correction.has_constant_color);
  EXPECT_FLOAT_EQ(correction.constant_color[0], 0.3f);
  EXPECT_EQ(correction.image, nullptr);
}

TEST_F(PaintLayersMaterialCorrectionTest, an_edit_of_the_source_concerns_the_material_it_corrects)
{
  MaterialPaintLayer *effect = nullptr;
  add_row_with_correction(&effect);
  ASSERT_NE(effect, nullptr);
  Vector<Material *> consumers;
  BKE_paint_layers_source_material_consumers(*bmain, *source, consumers);
  ASSERT_EQ(consumers.size(), 1);
  EXPECT_EQ(consumers[0], ma);

  Vector<const MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach_source_row(static_cast<const Material &>(*ma),
                                      [&](const MaterialPaintLayer &row) {
                                        rows.append(&row);
                                        return true;
                                      });
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0], effect);
}

/** \} */

}  // namespace blender::bke::tests
