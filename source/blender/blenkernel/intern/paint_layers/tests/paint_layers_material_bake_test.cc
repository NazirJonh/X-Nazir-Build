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

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <string>

#include "MEM_guardedalloc.h"

#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_tile.hh"
#include "BKE_wm_runtime.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "NOD_socket.hh"

#include "DNA_node_tree_interface_types.h"
#include "DNA_windowmanager_types.h"

#include "../paint_layers_bake_intern.hh"
#include "../paint_layers_effect_footprint.hh"
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

  /** What the editor does before a render starts: the side, the state it starts from, the maps.
   * \param map_size: the maps' own size when a test needs more than the fixture's 8x8: the render
   *        would hand maps of the bake side, and the bake subscription's regions are 256-pixel
   *        chunks, so a canvas larger than one chunk is what lets a partial write stay partial. */
  void start_render(MaterialPaintLayer &row,
                    const Span<const int> channels,
                    uint32_t r_hash[2],
                    uint32_t r_source_hash[2],
                    const int size = 64,
                    const int map_size = 0)
  {
    ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, row, size));
    BKE_paint_layers_material_bake_stamp_get(*ma, row, r_hash, r_source_hash);
    Vector<Image *> images;
    for (const int channel : channels) {
      if (map_size > 0) {
        const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        images.append(BKE_image_add_generated(bmain,
                                              map_size,
                                              map_size,
                                              channel == PAINT_MATERIAL_CHANNEL_ALPHA ? "Alpha" :
                                                                                        "Map",
                                              32,
                                              false,
                                              IMA_GENTYPE_BLANK,
                                              color,
                                              false,
                                              false,
                                              false));
      }
      else {
        images.append(add_image(channel == PAINT_MATERIAL_CHANNEL_ALPHA ? "Alpha" : "Map"));
      }
    }
    Vector<int> channel_values(channels.begin(), channels.end());
    BKE_paint_layers_material_bake_apply(*bmain, *ma, row, size, channel_values, images);
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

/** Mark \a region as changed in \a image's partial-update log, as a paint writer does. */
void material_test_mark_region(Image &image, const rcti &region)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  EXPECT_NE(ibuf, nullptr);
  if (ibuf == nullptr) {
    return;
  }
  /* The log keys its changesets by tile, so a null tile is not a valid stand-in for the first. */
  const ImageTile *tile = BKE_image_get_tile(&image, 1001);
  EXPECT_NE(tile, nullptr);
  BKE_image_partial_update_mark_region(&image, tile, ibuf, &region);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/** Make the right half of \a image transparent; returns the cleared rectangle. */
rcti material_test_clear_right_half_alpha(Image &image)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  EXPECT_NE(ibuf, nullptr);
  if (ibuf == nullptr) {
    return rcti{0, 0, 0, 0};
  }
  const int width = ibuf->x;
  const int height = ibuf->y;
  const int half = width / 2;
  uchar *pixels = ibuf->byte_data_for_write();
  EXPECT_NE(pixels, nullptr);
  if (pixels != nullptr) {
    for (const int y : IndexRange(height)) {
      for (int x = half; x < width; x++) {
        pixels[(int64_t(y) * width + x) * 4 + 3] = 0;
      }
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  return rcti{half, width, 0, height};
}

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
  const float offset[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, row, offset));
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);

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
  start_render(*row, Span<const int>(channels, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);

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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  Material *other = BKE_material_add(bmain, "Other");
  /* An empty source has nothing to render and is never due, so the new source needs a graph. */
  principled_source_set_up(*other, /*graphs=*/true);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayer *copied =
      static_cast<const MaterialPaintLayer *>(copy->paint_layers.first);
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

/** Defined with the Bump-as-Normal tests below: a Bump fed by a Value makes the Normal a render. */
static bNode *bump_normal_link(Material &material);

TEST_F(PaintLayersMaterialBakeTest, a_row_added_to_a_layered_source_marks_the_consumers_map_stale)
{
  Material *layered = BKE_material_add(bmain, "Layered Source");
  principled_source_set_up(*layered, /*graphs=*/false);
  /* The source must have a channel only a render can show, or the row keeps no maps: a Bump-made
   * Normal has no live form. Every other channel is a plain value the graph shows live, and a row
   * made only of those would never render, so the content move below would rightly go unnoticed. */
  ASSERT_NE(bump_normal_link(*layered), nullptr);
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *layered, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  MaterialPaintLayer *row = add_material_row(*layered);
  ASSERT_NE(row, nullptr);
  EXPECT_FALSE(BKE_paint_layers_material_sources_notice(*ma)) << "the first sight is not a change";
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  /* The new row changes what a FULL row renders, even though the shape of the consumer's group
   * follows the same channels: the content hash is what its maps must follow. */
  MaterialPaintLayer *added = BKE_paint_layers_add(
      *layered, "Added", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(added, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, added, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *layered));
  BKE_paint_layers_source_material_hash_forget(*layered);
  BKE_paint_layers_material_sources_notice(*ma);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

TEST_F(PaintLayersMaterialBakeTest, a_removed_row_forgets_its_source_content_and_signature)
{
  /* The remembered answers are keyed by row marker: one left behind by a removed row would be read
   * by whatever row takes that marker next, as if the old row's shape and source content were its
   * own. The notice drops the entries of rows the stack no longer holds, and does not read the
   * surviving rows as moved while doing so. */
  Material *plain = BKE_material_add(bmain, "Plain");
  principled_source_set_up(*plain, /*graphs=*/false);
  /* A channel only a render can show: without one the rows keep no maps, so no content is hashed
   * and there would be nothing to clean up. */
  ASSERT_NE(bump_normal_link(*plain), nullptr);
  MaterialPaintLayer *first = add_material_row(*plain);
  MaterialPaintLayer *second = add_material_row(*plain);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  EXPECT_FALSE(BKE_paint_layers_material_sources_notice(*ma)) << "the first sight is not a change";
  const UUID first_marker(first->marker);
  const UUID second_marker(second->marker);
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma->id.session_uid);
    ASSERT_NE(runtime, nullptr);
    EXPECT_TRUE(runtime->live_signature.contains(first_marker));
    EXPECT_TRUE(runtime->source_content.contains(first_marker));
    EXPECT_TRUE(runtime->source_content.contains(second_marker));
  }

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, first));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;
  EXPECT_FALSE(BKE_paint_layers_material_sources_notice(*ma)) << "the surviving row did not move";
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma->id.session_uid);
    ASSERT_NE(runtime, nullptr);
    EXPECT_FALSE(runtime->live_signature.contains(first_marker));
    EXPECT_FALSE(runtime->source_content.contains(first_marker));
    EXPECT_TRUE(runtime->live_signature.contains(second_marker));
    EXPECT_TRUE(runtime->source_content.contains(second_marker));
  }
}

TEST_F(PaintLayersMaterialBakeTest,
       a_mask_edit_of_a_content_baked_source_row_moves_the_narrowed_key)
{
  /* The narrowed reader renders the referenced row whole -- its mask included -- while the row's
   * own bake holds its content alone when it carries a CPU effect. The key the reader follows has
   * to be the whole-row one, or adding a mask to such a row would leave the reader's maps as they
   * were. */
  Material *layered = BKE_material_add(bmain, "Layered Source");
  principled_source_set_up(*layered, /*graphs=*/false);
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *layered, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  /* A CPU-computed effect: the row bakes its content alone, its mask living over that bake. */
  ASSERT_NE(BKE_paint_layers_filter_add(*layered, paint, MA_PAINT_LAYER_FILTER_BLUR), nullptr);

  BKE_paint_layers_source_row_hash_memo_drop();
  const uint64_t before = BKE_paint_layers_source_row_content_hash(*layered, paint->marker);
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*layered, paint, 1.0f);
  ASSERT_NE(mask, nullptr);
  BKE_paint_layers_source_row_hash_memo_drop();
  const uint64_t after = BKE_paint_layers_source_row_content_hash(*layered, paint->marker);
  EXPECT_NE(before, after) << "the reader shows the row whole, its mask inside";
}

TEST_F(PaintLayersMaterialBakeTest,
       a_narrowed_row_subscribes_to_the_referenced_rows_own_maps)
{
  /* The narrowed render describes the referenced row live, so its own channel and mask maps are
   * what a stroke lands in. A row that keeps no bake of its own has no bake maps to subscribe to:
   * the raw maps have to be in the list, or the reader would never follow a stroke. */
  Material *layered = BKE_material_add(bmain, "Layered Source");
  principled_source_set_up(*layered, /*graphs=*/false);
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  Image *color_map = add_image("Source Map");
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *layered, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *layered, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color_map));
  Image *mask_map = add_image("Source Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*layered, *paint, *mask_map), nullptr);
  ASSERT_EQ(paint->bake, nullptr) << "a light row keeps no bake, and so no bake maps";

  MaterialPaintLayer *reader = add_material_row(*layered);
  ASSERT_NE(reader, nullptr);
  reader->material_scope = MA_PAINT_LAYER_MATERIAL_SCOPE_ROW;
  reader->material_row_marker = paint->marker;

  Vector<Image *> images;
  BKE_paint_layers_bake_collect_source_images(*ma, *reader, images);
  EXPECT_TRUE(images.contains(color_map))
      << "a stroke into the source's own map reaches the reader";
  EXPECT_TRUE(images.contains(mask_map)) << "and so does a stroke into its mask";
}

TEST_F(PaintLayersMaterialBakeTest,
       a_mask_or_an_effect_on_a_material_row_source_moves_the_narrowed_key)
{
  /* The reader of a row that is itself a Material row renders it whole: the row's own corrections,
   * mask and live factors are laid over the maps it shows. Both the key and the pixel subscription
   * have to hold them, or adding a mask or an effect to the referenced row would leave the reader
   * showing the row as it was. */
  Material *layered = BKE_material_add(bmain, "Layered Source");
  principled_source_set_up(*layered, /*graphs=*/false);
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  Material *plain = BKE_material_add(bmain, "Plain");
  principled_source_set_up(*plain, /*graphs=*/false);
  PaintLayerAddParams nested_params;
  nested_params.kind = PaintLayerAddKind::Material;
  nested_params.source = plain;
  nested_params.name = "Nested";
  MaterialPaintLayer *nested = BKE_paint_layers_add_with_policy(*layered, nested_params);
  ASSERT_NE(nested, nullptr);

  BKE_paint_layers_source_row_hash_memo_drop();
  const uint64_t before = BKE_paint_layers_source_row_content_hash(*layered, nested->marker);
  Image *mask_map = add_image("Nested Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*layered, *nested, *mask_map), nullptr);
  BKE_paint_layers_source_row_hash_memo_drop();
  const uint64_t with_mask = BKE_paint_layers_source_row_content_hash(*layered, nested->marker);
  EXPECT_NE(before, with_mask) << "a mask on the referenced Material row is part of the picture";

  ASSERT_NE(BKE_paint_layers_filter_add(*layered, nested, MA_PAINT_LAYER_FILTER_BLUR), nullptr);
  BKE_paint_layers_source_row_hash_memo_drop();
  const uint64_t with_effect = BKE_paint_layers_source_row_content_hash(*layered, nested->marker);
  EXPECT_NE(with_mask, with_effect) << "and so is an effect added to the row";

  MaterialPaintLayer *reader = add_material_row(*layered);
  ASSERT_NE(reader, nullptr);
  reader->material_scope = MA_PAINT_LAYER_MATERIAL_SCOPE_ROW;
  reader->material_row_marker = nested->marker;
  Vector<Image *> images;
  BKE_paint_layers_bake_collect_source_images(*ma, *reader, images);
  EXPECT_TRUE(images.contains(mask_map)) << "a stroke into that mask reaches the reader";
}

TEST_F(PaintLayersMaterialBakeTest,
       a_value_edit_of_the_source_moves_the_live_input_without_a_rebuild)
{
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness_normal, 3), hash, source_hash);
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
  start_render(*row, Span<const int>(base_color_roughness_normal, 3), hash, source_hash);
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
  start_render(*row, Span<const int>(normal_only, 1), hash, source_hash);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
  EXPECT_FALSE(BKE_paint_layers_material_row_stale(*ma, *row));

  /* An Image Editor opens: its composite reads every map, which this render did not make. */
  BKE_paint_layers_preview_wanted_set(*ma, true);
  EXPECT_TRUE(BKE_paint_layers_material_channel_render_wanted(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(BKE_paint_layers_material_row_stale(*ma, *row));

  /* The full render that follows is current for the editor and for it closing again. */
  uint32_t full_hash[2], full_source_hash[2];
  start_render(*row, Span<const int>(base_color_roughness_normal, 3), full_hash, full_source_hash);
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
  /* Adding a row does not select it; the editing the test is about happens inside the active one. */
  BKE_paint_layers_active_set(*ma, row->marker);
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
  const float offset[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, row, offset));
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
  /* The effect's marker is part of its input names, so two corrections of one kind never share a
   * socket. */
  char marker[40];
  BLI_uuid_format(marker, effect->marker);
  const std::string source_name = std::string("Base Mat Metallic Source ") + marker;
  const std::string opacity_name = std::string("Base Mat Metallic Opacity ") + marker;
  EXPECT_TRUE(interface_has_socket(*group, source_name.c_str(), false))
      << "a plain value of the source is an input of the row's group";
  EXPECT_TRUE(interface_has_socket(*group, opacity_name.c_str(), false));

  /* Editing the value in the source reaches the input without a rebuild. */
  edit_source(0.8f);
  EXPECT_TRUE(BKE_paint_layers_values_sync(*ma));
  const bNodeTreeInterfaceSocket *live = interface_input_find(*group, source_name.c_str());
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

/* -------------------------------------------------------------------- */
/** \name A mask correction that reads a material
 * \{ */

class PaintLayersMaterialMaskTest : public PaintLayersMaterialBakeTest {
 public:
  /** A source whose Base Color is a plain value, which is all a mask correction lays live. */
  Material *plain = nullptr;

  void SetUp() override
  {
    PaintLayersMaterialBakeTest::SetUp();
    plain = BKE_material_add(bmain, "Plain");
    principled_source_set_up(*plain, /*graphs=*/false);
  }

  /** A Fill row named "Base" with a mask correction named "Grey" reading \a from. */
  MaterialPaintLayer *add_row_with_mask_item(Material *from, MaterialPaintLayer **r_item)
  {
    PaintLayerAddParams row_params;
    row_params.kind = PaintLayerAddKind::Fill;
    row_params.name = "Base";
    MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, row_params);
    EXPECT_NE(row, nullptr);
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::MaskCorrectionMaterial;
    params.anchor = row;
    params.name = "Grey";
    params.source = from;
    *r_item = BKE_paint_layers_add_with_policy(*ma, params);
    EXPECT_NE(*r_item, nullptr);
    return row;
  }
};

TEST_F(PaintLayersMaterialMaskTest, it_makes_the_base_mask_and_owns_no_records)
{
  MaterialPaintLayer *item = nullptr;
  MaterialPaintLayer *row = add_row_with_mask_item(plain, &item);
  ASSERT_NE(item, nullptr);
  EXPECT_NE(BKE_paint_layers_mask_base(*row), nullptr) << "made first, as for any mask correction";
  EXPECT_EQ(item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(item->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_EQ(item->material, plain);
  EXPECT_EQ(item->channels_num, 0);
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(*ma, *item), row);

  const int users = plain->id.us;
  EXPECT_TRUE(BKE_paint_layers_mask_item_remove(*ma, item));
  EXPECT_EQ(plain->id.us, users - 1) << "the mask correction was a user of its source";
}

TEST_F(PaintLayersMaterialMaskTest, it_refuses_itself_a_cycle_a_missing_source_and_no_base_mask)
{
  PaintLayerAddParams row_params;
  MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, row_params);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_material_add(*ma, row, "Self", ma), nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_material_add(*ma, row, "None", nullptr), nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_material_add(*ma, row, "NoBase", plain), nullptr)
      << "the Add policy makes the base mask; the kernel call does not";

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskCorrectionMaterial;
  params.anchor = row;
  params.source = nullptr;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr);

  PaintLayerAddParams source_params;
  MaterialPaintLayer *source_row = BKE_paint_layers_add_with_policy(*source, source_params);
  ASSERT_NE(source_row, nullptr);
  ASSERT_NE(BKE_paint_layers_correction_add_material(*source, source_row, "Back", ma), nullptr);
  params.source = source;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr)
      << "the source reads this material back";
}

TEST_F(PaintLayersMaterialMaskTest, it_lays_the_grey_of_a_plain_base_color)
{
  MaterialPaintLayer *item = nullptr;
  MaterialPaintLayer *row = add_row_with_mask_item(plain, &item);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(paint_layer_mask_reads_source(*ma, *item));
  EXPECT_TRUE(paint_layer_mask_item_lays(*ma, *item));
  EXPECT_FALSE(BKE_paint_layers_material_row_needs_render(*ma, *item))
      << "a plain value is no render";

  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_GE(layers[0].mask_items.size(), 2) << "the base mask and the correction";
  const PaintMaterialCompositeMaskItem &described = layers[0].mask_items.last();
  EXPECT_EQ(described.image, nullptr);
  EXPECT_NEAR(described.constant_gray, 0.8f, 1e-5f) << "the mean of the default Base Color";
  UNUSED_VARS(row);
}

TEST_F(PaintLayersMaterialMaskTest, a_source_whose_transparency_is_a_map_lays_nothing)
{
  MaterialPaintLayer *item = nullptr;
  add_row_with_mask_item(plain, &item);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(paint_layer_mask_item_lays(*ma, *item));

  texture_link(*plain, *add_image("Cutout"), "Alpha", "Alpha");
  EXPECT_FALSE(paint_layer_mask_item_lays(*ma, *item))
      << "the mask correction has no coverage of its own to carry a map";
}

TEST_F(PaintLayersMaterialMaskTest, the_generator_gives_the_mask_correction_a_live_input)
{
  MaterialPaintLayer *item = nullptr;
  add_row_with_mask_item(plain, &item);
  ASSERT_NE(item, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Base");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(interface_has_socket(*group, "Base Grey Source", false))
      << "the plain Base Color of the source is an input of the row's group";
}

TEST_F(PaintLayersMaterialMaskTest, an_edit_of_the_source_concerns_the_material_it_shapes)
{
  MaterialPaintLayer *item = nullptr;
  add_row_with_mask_item(plain, &item);
  ASSERT_NE(item, nullptr);
  Vector<Material *> consumers;
  BKE_paint_layers_source_material_consumers(*bmain, *plain, consumers);
  ASSERT_EQ(consumers.size(), 1);
  EXPECT_EQ(consumers[0], ma);

  Vector<const MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach_source_row(static_cast<const Material &>(*ma),
                                      [&](const MaterialPaintLayer &row) {
                                        rows.append(&row);
                                        return true;
                                      });
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0], item);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name A CPU-computed effect on a Material row
 *
 * A Material row's maps are its rendered source, so no bake of the row can hold a Blur: the effect
 * keeps its result in a bake of its own (#paint_layer_effect_owns_result), which the live graph
 * lays over the row's content and the CPU composite reads instead of the effect cache.
 * \{ */

namespace owned_effect_gpu_test {

/** What the recording provider was last offered, for the region-offer tests below. */
bool g_provider_had_region = false;
rcti g_provider_region = {};

/** A registered provider that records the region it was offered and refuses. */
static std::shared_ptr<ImBuf> owned_effect_gpu_provider_records(const Material & /*ma*/,
                                                                const MaterialPaintLayer & /*row*/,
                                                                const MaterialPaintLayer & /*effect*/,
                                                                const int /*channel*/,
                                                                const int /*tile_number*/,
                                                                const rcti *changed_region)
{
  /* Sticky: the inline effect compute asks the provider after the job's precompute did, without a
   * region of its own; the test wants to know whether the job offered one at all. */
  g_provider_had_region = g_provider_had_region || changed_region != nullptr;
  if (changed_region != nullptr) {
    g_provider_region = *changed_region;
  }
  return nullptr;
}

}  // namespace owned_effect_gpu_test

class PaintLayersOwnedEffectTest : public PaintLayersMaterialBakeTest {
 public:
  void SetUp() override
  {
    PaintLayersMaterialBakeTest::SetUp();
    /* A dummy window manager, the way the bake tests make one: the bake path notifies image
     * editors by walking bmain->wm, which a bare test Main has none of. */
    wmWindowManager *wm = BKE_id_new<wmWindowManager>(bmain, "WMdummy");
    wm->runtime = MEM_new<bke::WindowManagerRuntime>(__func__);
    BKE_paint_layers_effect_cache_free_all();
    BKE_paint_layers_mask_effect_cache_free_all();
  }

  void TearDown() override
  {
    /* No test-provider may outlive its test: the hook is a plain pointer BKE reads during a bake. */
    BKE_paint_layers_owned_effect_gpu_provider_set(nullptr);
    owned_effect_gpu_test::g_provider_had_region = false;
    BKE_paint_layers_effect_cache_free_all();
    /* The mask effect cache is a module static the mask-grey tests fill: left uncleared it holds a
     * Vector past the leak check, the way the effect cache above would. */
    BKE_paint_layers_mask_effect_cache_free_all();
    BKE_paint_layers_bake_stroke_active_set(false);
    BKE_paint_layers_bake_drag_clear_all();
    PaintLayersMaterialBakeTest::TearDown();
  }

  /** Makes the planner compute on the main thread for the test's scope, then restores the flag. */
  struct ScopedBackground {
    bool saved;
    ScopedBackground() : saved(G.background)
    {
      G.background = true;
    }
    ~ScopedBackground()
    {
      G.background = saved;
    }
  };

  /** A Material row whose Base Color and Roughness maps have landed, with a box Blur on it. */
  MaterialPaintLayer *add_row_with_blur(MaterialPaintLayer **r_blur,
                                        const int size = 64,
                                        const int map_size = 0)
  {
    MaterialPaintLayer *row = add_material_row(*source);
    EXPECT_NE(row, nullptr);
    uint32_t hash[2], source_hash[2];
    start_render(
        *row, Span<const int>(base_color_roughness, 2), hash, source_hash, size, map_size);
    EXPECT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));
    MaterialPaintLayer *blur = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_BLUR);
    EXPECT_NE(blur, nullptr);
    EXPECT_TRUE(
        paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
    EXPECT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));
    BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
    *r_blur = blur;
    return row;
  }

  /** Write \a value into the RGB of every texel of \a rect; the alpha stays opaque. */
  static void write_map_rect(Image &image, const rcti &rect, const uchar value)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    if (ibuf == nullptr) {
      return;
    }
    uchar *pixels = ibuf->byte_data_for_write();
    ASSERT_NE(pixels, nullptr);
    if (pixels != nullptr) {
      for (const int y : IndexRange(rect.ymin, BLI_rcti_size_y(&rect))) {
        for (const int x : IndexRange(rect.xmin, BLI_rcti_size_x(&rect))) {
          uchar *texel = pixels + (int64_t(y) * ibuf->x + x) * 4;
          texel[0] = value;
          texel[1] = value;
          texel[2] = value;
          texel[3] = 255;
        }
      }
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** Report \a rect as changed in \a image's pixel journal, the way a paint writer does. */
  static void mark_map_rect(Image &image, const rcti &rect)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    if (ibuf == nullptr) {
      return;
    }
    BKE_image_partial_update_mark_region(
        &image, BKE_image_get_tile(&image, PAINT_TILE_NUMBER_BASE), ibuf, &rect);
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** Write \a value into \a rect, then report it as the dirty band of a dab. */
  static void edit_map_rect(Image &image, const rcti &rect, const uchar value)
  {
    write_map_rect(image, rect, value);
    mark_map_rect(image, rect);
  }

  /** The stored RGBA bytes of \a image, row by row. */
  static Vector<uchar> read_map(Image &image)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    EXPECT_NE(ibuf, nullptr);
    Vector<uchar> pixels;
    if (ibuf != nullptr && ibuf->byte_buffer.data != nullptr) {
      const int64_t count = int64_t(ibuf->x) * ibuf->y * 4;
      pixels.reinitialize(count);
      std::copy(ibuf->byte_buffer.data, ibuf->byte_buffer.data + count, pixels.begin());
    }
    if (ibuf != nullptr) {
      BKE_image_release_ibuf(&image, ibuf, lock);
    }
    return pixels;
  }

  static uchar map_byte_at(const Vector<uchar> &pixels, const int width, const int x, const int y)
  {
    return pixels[(int64_t(y) * width + x) * 4];
  }

  /** The Image Textures reading \a image in \a tree and the groups under it. */
  static int image_reader_count(const bNodeTree &tree, const Image *image)
  {
    int count = 0;
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &image->id) {
        count++;
      }
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
        count += image_reader_count(*reinterpret_cast<const bNodeTree *>(node.id), image);
      }
    }
    return count;
  }

  static void hash_get(const Material &material, const MaterialPaintLayer &layer, uint32_t r_hash[2])
  {
    BKE_paint_layers_bake_hash(material, layer, r_hash);
  }

  /**
   * Run the worker job the way the editor does: create on the main thread, compute, commit. False
   * when no job was queued or the commit wrote nothing. \a between runs after the compute, where an
   * edit that lands while the worker ran belongs.
   */
  bool run_bake_job(const FunctionRef<void()> between = {})
  {
    PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
    if (job == nullptr) {
      return false;
    }
    BKE_paint_layers_bake_job_compute(*job, {}, {});
    if (between) {
      between();
    }
    const bool landed = BKE_paint_layers_bake_job_commit(*job);
    BKE_paint_layers_bake_job_free(*job);
    return landed;
  }

  /**
   * The planner's own inline bake: a light row (its maps below the heavy size) is rendered by
   * #BKE_paint_layers_bake_plan_run, which is the path the footprint narrowing serves. False when
   * the plan wrote nothing.
   */
  bool run_plan()
  {
    ScopedBackground background;
    bool changed = false;
    BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed);
    return changed;
  }

  /**
   * Render \a row through the render the planner and the worker call, over \a dst (null for the
   * whole row), and return the base colour's output pixels.
   */
  Vector<float> render_row_node(const MaterialPaintLayer &row, const int size, const int *dst)
  {
    Vector<float> color(int64_t(size) * size * 4);
    Vector<float> coverage(int64_t(size) * size);
    EXPECT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                  row,
                                                  PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                  size,
                                                  color.data(),
                                                  coverage.data(),
                                                  dst,
                                                  nullptr));
    return color;
  }

  /**
   * A Fill row in the Always mode carrying a fresh \a map_size square map through a CLIP mapping
   * of \a scale, with a box Blur of radius 2 narrowed to Base Color, at an explicit square bake
   * \a size, whose maps the planner renders. This is the row kind the planner's band and footprint
   * narrowing serves: the mapping folds into the bake (a row with a CPU effect), its maps hold the
   * row's own content laid through the mapping, and the content ends inside the footprint grown by
   * the blur's halo. The blur's extra channel records are removed so the subtree stays under the
   * node budget the planner weighs a row by: a light due row is rendered by the plan itself, which
   * is the path the narrowing serves (a heavy one leaves for the worker job, which resolves the
   * same footprint at creation). \a effect_kind picks the CPU effect on top of the row: a box Blur
   * by default, another kind for the refusals the partial effect path makes. \a r_row and \a r_map
   * hand the row and its source map back for edits and read-back.
   */
  MaterialPaintLayer *add_bounded_fill(MaterialPaintLayer **r_row,
                                       Image **r_map,
                                       const float scale,
                                       const int size,
                                       const char *name,
                                       const int map_size = 8,
                                       const eMaterialPaintLayerFilter effect_kind =
                                           MA_PAINT_LAYER_FILTER_BLUR)
  {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *map = BKE_image_add_generated(
        bmain, map_size, map_size, name, 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Fill;
    params.name = name;
    MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
    EXPECT_NE(row, nullptr);
    add_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map);
    /* The authoring policy created records for every channel of the material's set, and each one
     * costs the node budget the planner weighs a row's subtree by. Base Color alone keeps the row
     * under it: a light due row is rendered by the plan itself, which is the path the narrowing
     * serves; a heavy one leaves for the worker job, which resolves the same footprint. */
    for (int i = row->channels_num - 1; i >= 0; i--) {
      const int channel = int(row->channels[i].channel);
      if (channel != int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)) {
        EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, row, eMaterialPaintChannel(channel)));
      }
    }
    const float scale2[2] = {scale, scale};
    EXPECT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale2));
    EXPECT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
    MaterialPaintLayer *blur = BKE_paint_layers_filter_add(*ma, row, effect_kind);
    EXPECT_NE(blur, nullptr);
    if (effect_kind == MA_PAINT_LAYER_FILTER_BLUR) {
      EXPECT_TRUE(
          paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
      EXPECT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));
    }
    for (int i = blur->channels_num - 1; i >= 0; i--) {
      const int channel = int(blur->channels[i].channel);
      if (channel != int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)) {
        EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, blur, eMaterialPaintChannel(channel)));
      }
    }
    EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
    EXPECT_TRUE(BKE_paint_layers_bake_size_set(*ma, *row, size));
    BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
    *r_row = row;
    *r_map = map;
    return row;
  }
};

TEST_F(PaintLayersOwnedEffectTest, a_bounded_footprint_narrows_the_planner_region)
{
  /* The footprint is the canvas rectangle a changed input can move the result in; a row whose
   * mapping is bounded (here scale-2 CLIP) is no longer owed a whole canvas. The subscription
   * band is the pixel-edit signal and lives in the input maps' pixels, which the row's mapping
   * moves: it is not intersected with the canvas-space footprint. */
  texture_link(*source, *add_image("FootprintSource"), "Metallic");
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mapping_supported(*ma, *row));
  const float scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  ASSERT_TRUE(run_bake_job());

  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  write_map_rect(*base_map, rcti{400, 401, 400, 401}, 200);
  mark_map_rect(*base_map, rcti{400, 401, 400, 401});
  BKE_paint_layers_bake_notice_changes(*ma);

  int band[4] = {};
  /* The band stands, so the narrowing below is the footprint's, not a missing subscription. */
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *blur, band));
  rcti resolved;
  const rcti *region = bake_effect_region_resolve(*ma, *blur, resolved);
  ASSERT_NE(region, nullptr);
  /* scale 2 over 512: the unit square's preimage is [0, 256) plus the pad and the blur halo, so
   * the write is a quarter of the canvas in each axis, not the whole 512. */
  EXPECT_GE(region->xmin, 0);
  EXPECT_GE(region->ymin, 0);
  EXPECT_LE(region->xmax, 258);
  EXPECT_LE(region->ymax, 258);
  EXPECT_LT(BLI_rcti_size_x(region), 512);
  EXPECT_LT(BLI_rcti_size_y(region), 512);

  /* Run the job the region belongs to: the compute/commit must use the narrowed band, which the
   * BLENDER_PBR_PERF log reports as `region=` smaller than `size=512x512`. */
  EXPECT_TRUE(run_bake_job());
}

TEST_F(PaintLayersOwnedEffectTest, a_replaced_planner_map_is_written_whole)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  /* A light row (below the heavy side) is baked by the planner itself, which is the path the
   * replacement handling serves; scale 1 lays the content over the whole canvas. The source map is
   * wider than the bake map, so the edit's 256-pixel chunk is a band narrower than the canvas. */
  add_bounded_fill(&row, &map, 1.0f, 512, "ReplacedPlanner", 1024);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(map, nullptr);

  int map_width = 0;
  int map_height = 0;
  BKE_image_get_size(map, nullptr, &map_width, &map_height);
  ASSERT_GT(map_width, 0);
  ASSERT_GT(map_height, 0);
  const rcti map_rect{0, map_width, 0, map_height};
  write_map_rect(*map, map_rect, 120);
  ASSERT_TRUE(run_plan());

  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base, nullptr);

  /* A wrong size forces the next bake_service_image to replace the map with a blank one. */
  Image *replaced = bake_service_image(*bmain,
                                       *row,
                                       int(PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                                       4096,
                                       true,
                                       false,
                                       0);
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], replaced);

  /* A generated blank map's write lives in the buffer it was written through, which the first
   * plan's render released: lay the content again so this plan reads it. */
  write_map_rect(*map, map_rect, 120);
  const rcti dirty{4, 5, 4, 5};
  edit_map_rect(*map, dirty, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_plan());

  Image *written = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(written, nullptr);
  int width = 0;
  int height = 0;
  BKE_image_get_size(written, nullptr, &width, &height);
  ASSERT_EQ(width, 512);
  ASSERT_EQ(height, 512);
  const Vector<uchar> pixels = read_map(*written);
  ASSERT_EQ(pixels.size(), int64_t(width) * height * 4);

  /* Outside the 256-pixel band the edit reports: a partial write into the fresh map leaves this
   * zero, the whole render leaves the row's 120. */
  EXPECT_NEAR(map_byte_at(pixels, width, 400, 400), 120, 3)
      << "a replaced planner map must be rendered and written whole";
}

TEST_F(PaintLayersOwnedEffectTest, an_owned_effect_with_a_decal_input_refuses_the_region)
{
  MaterialPaintLayer *blur = nullptr;
  texture_link(*source, *add_image("DecalSource"), "Metallic");
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  /* A bounded row mapping gives the owned effect a footprint it would otherwise take. */
  ASSERT_TRUE(BKE_paint_layers_mapping_supported(*ma, *row));
  const float scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));

  MaterialPaintLayer *mirror = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_MIRROR, "DecalMirror");
  ASSERT_NE(mirror, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror(*ma, mirror, MA_PAINT_LAYER_MAPPING_MIRROR_U));
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, mirror, MA_PAINT_LAYER_MAPPING_SPACE_DECAL));
  /* Effects are bottom-to-top: the mirror must feed the blur (the Decal step is part of the blur's
   * input, the case the bounded footprint must not ignore). */
  BLI_remlink(&row->effects, mirror);
  BLI_addhead(&row->effects, mirror);

  ASSERT_TRUE(run_bake_job());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  write_map_rect(*base_map, rcti{400, 401, 400, 401}, 200);
  mark_map_rect(*base_map, rcti{400, 401, 400, 401});
  BKE_paint_layers_bake_notice_changes(*ma);

  int band[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *blur, band));
  rcti resolved;
  EXPECT_EQ(bake_effect_region_resolve(*ma, *blur, resolved), nullptr)
      << "a Decal step in the input makes the footprint unbounded for the regional write";
}

TEST_F(PaintLayersOwnedEffectTest, a_mixed_size_row_planner_refuses_the_band)
{
  /* The band is scaled by one channel's dimensions. Base is 1024 and Metallic 512: the edit below
   * lives in the Metallic map, and scaling its 256-pixel chunk by Base's width would name a
   * smaller rectangle than the edit really moves. With the mixed sizes refused, the plan renders
   * whole and rewrites the canary the wrongly scaled band would leave stale. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *base = BKE_image_add_generated(bmain,
                                        1024,
                                        1024,
                                        "MixedBase",
                                        32,
                                        false,
                                        IMA_GENTYPE_BLANK,
                                        black,
                                        false,
                                        false,
                                        false);
  Image *metallic = BKE_image_add_generated(bmain,
                                            512,
                                            512,
                                            "MixedMetallic",
                                            32,
                                            false,
                                            IMA_GENTYPE_BLANK,
                                            black,
                                            false,
                                            false,
                                            false);
  ASSERT_NE(base, nullptr);
  ASSERT_NE(metallic, nullptr);
  MaterialPaintLayer *row = add_paint_layer("MixedRow", base, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(row, nullptr);
  add_channel(*row, PAINT_MATERIAL_CHANNEL_METALLIC, metallic);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  const rcti base_rect{0, 1024, 0, 1024};
  write_map_rect(*base, base_rect, 120);
  write_map_rect(*metallic, rcti{0, 512, 0, 512}, 120);
  ASSERT_TRUE(run_plan());

  Image *bake_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(bake_map, nullptr);
  int width = 0;
  int height = 0;
  BKE_image_get_size(bake_map, nullptr, &width, &height);
  ASSERT_EQ(width, 1024);
  ASSERT_EQ(height, 1024);

  /* The partial-update log reports the edited 256-pixel chunk, so the Metallic edit at x=150 is the
   * chunk [0,256). The wrong Base-based scale makes that [0,512); the correct Metallic scale would
   * make it [0,768). Measured: with the helper disabled a canary at 300 stayed 120 (inside the
   * wrong [0,512)), so the canary is 600: outside the wrong band and inside the whole render the
   * helper falls back to. */
  write_map_rect(*bake_map, rcti{600, 601, 600, 601}, 255);
  /* The generated blank source buffer is released by the first plan; lay the content again so the
   * whole render this plan owes lays the 120 the test reads. */
  write_map_rect(*base, base_rect, 120);
  edit_map_rect(*metallic, rcti{150, 151, 150, 151}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  int band[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, band))
      << "the test's premise is a live subscription band";

  ASSERT_TRUE(run_plan());
  const Vector<uchar> pixels = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  ASSERT_EQ(pixels.size(), int64_t(width) * height * 4);
  EXPECT_NEAR(map_byte_at(pixels, width, 600, 600), 120, 3)
      << "a mixed-size row is rendered whole; the wrongly scaled band left the canary stale";
}

namespace {

/** Compare two byte maps of the same size, allowing a one-step quantization difference. */
void expect_maps_match(const Vector<uchar> &actual,
                       const Vector<uchar> &expected,
                       const char *what)
{
  ASSERT_EQ(actual.size(), expected.size()) << what;
  int mismatches = 0;
  int first = -1;
  for (const int64_t i : IndexRange(actual.size())) {
    if (std::abs(int(actual[i]) - int(expected[i])) > 1) {
      if (first < 0) {
        first = int(i);
      }
      mismatches++;
    }
  }
  EXPECT_EQ(mismatches, 0) << what << " differs at byte " << first;
}

}  // namespace

TEST_F(PaintLayersOwnedEffectTest, a_color_replacement_with_standing_coverage_keeps_the_band_consistent)
{
  MaterialPaintLayer *row = nullptr;
  Image *row_source = nullptr;
  add_bounded_fill(&row, &row_source, 1.0f, 512, "ColorReplaced", 1024);
  MaterialPaintLayer *reference = nullptr;
  Image *reference_source = nullptr;
  add_bounded_fill(&reference, &reference_source, 1.0f, 512, "ColorReference", 1024);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(reference, nullptr);

  const rcti source_rect{0, 1024, 0, 1024};
  write_map_rect(*row_source, source_rect, 120);
  write_map_rect(*reference_source, source_rect, 120);
  ASSERT_TRUE(run_plan());

  Image *row_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *reference_map = reference->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(row_map, nullptr);
  ASSERT_NE(reference_map, nullptr);
  ASSERT_NE(row->bake->coverage, nullptr);
  ASSERT_NE(reference->bake->coverage, nullptr);
  const uint32_t coverage_uid = row->bake->coverage->id.session_uid;

  /* Only the color map is replaced; the coverage map stands. */
  Image *replaced = bake_service_image(*bmain,
                                       *row,
                                       int(PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                                       4096,
                                       true,
                                       false,
                                       0);
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], replaced);

  /* The generated blank sources released their buffers in the first plan: lay them again, then the
   * same small edit in both rows. */
  write_map_rect(*row_source, source_rect, 120);
  write_map_rect(*reference_source, source_rect, 120);
  edit_map_rect(*row_source, rcti{4, 5, 4, 5}, 200);
  edit_map_rect(*reference_source, rcti{4, 5, 4, 5}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_plan());

  ASSERT_EQ(row->bake->coverage->id.session_uid, coverage_uid)
      << "the standing coverage map was not replaced";
  expect_maps_match(read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]),
                    read_map(*reference->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]),
                    "a replaced color map must match the reference content");
  expect_maps_match(read_map(*row->bake->coverage),
                    read_map(*reference->bake->coverage),
                    "the standing coverage must match the reference");
}

TEST_F(PaintLayersOwnedEffectTest, a_coverage_replacement_with_standing_channels_keeps_the_row_consistent)
{
  MaterialPaintLayer *row = nullptr;
  Image *row_source = nullptr;
  add_bounded_fill(&row, &row_source, 1.0f, 512, "CoverageReplaced", 1024);
  MaterialPaintLayer *reference = nullptr;
  Image *reference_source = nullptr;
  add_bounded_fill(&reference, &reference_source, 1.0f, 512, "CoverageReference", 1024);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(reference, nullptr);

  const rcti source_rect{0, 1024, 0, 1024};
  write_map_rect(*row_source, source_rect, 120);
  write_map_rect(*reference_source, source_rect, 120);
  ASSERT_TRUE(run_plan());

  ASSERT_NE(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  ASSERT_NE(reference->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  ASSERT_NE(row->bake->coverage, nullptr);
  ASSERT_NE(reference->bake->coverage, nullptr);
  const uint32_t row_map_uid =
      row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]->id.session_uid;

  /* Only the coverage map is replaced; the channel maps stand. */
  Image *replaced = bake_service_image(*bmain, *row, -1, 4096, false, false, 0);
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(row->bake->coverage, replaced);

  write_map_rect(*row_source, source_rect, 120);
  write_map_rect(*reference_source, source_rect, 120);
  edit_map_rect(*row_source, rcti{4, 5, 4, 5}, 200);
  edit_map_rect(*reference_source, rcti{4, 5, 4, 5}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_plan());

  ASSERT_EQ(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]->id.session_uid, row_map_uid)
      << "the standing channel map was not replaced";
  expect_maps_match(read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]),
                    read_map(*reference->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]),
                    "a standing channel map must match the reference content");
  expect_maps_match(read_map(*row->bake->coverage),
                    read_map(*reference->bake->coverage),
                    "a replaced coverage map must not zero what the standing channels need");
}

TEST_F(PaintLayersOwnedEffectTest, a_worker_coverage_replacement_keeps_the_row_consistent)
{
  MaterialPaintLayer *row = nullptr;
  Image *row_source = nullptr;
  add_bounded_fill(&row, &row_source, 4.0f, 2048, "JobCoverageReplaced", 512);
  MaterialPaintLayer *reference = nullptr;
  Image *reference_source = nullptr;
  add_bounded_fill(&reference, &reference_source, 4.0f, 2048, "JobCoverageReference", 512);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(reference, nullptr);

  const rcti source_rect{0, 512, 0, 512};
  write_map_rect(*row_source, source_rect, 120);
  write_map_rect(*reference_source, source_rect, 120);
  ASSERT_TRUE(run_bake_job());

  ASSERT_NE(row->bake->coverage, nullptr);
  ASSERT_NE(reference->bake->coverage, nullptr);

  /* Only the coverage map is replaced; the channel maps stand. */
  Image *replaced = bake_service_image(*bmain, *row, -1, 4096, false, false, 0);
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(row->bake->coverage, replaced);

  write_map_rect(*row_source, source_rect, 120);
  write_map_rect(*reference_source, source_rect, 120);
  edit_map_rect(*row_source, rcti{0, 8, 0, 8}, 200);
  edit_map_rect(*reference_source, rcti{0, 8, 0, 8}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_bake_job());

  expect_maps_match(read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]),
                    read_map(*reference->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]),
                    "a worker job with a replaced coverage must keep the channel content");
  expect_maps_match(read_map(*row->bake->coverage),
                    read_map(*reference->bake->coverage),
                    "a worker job must not zero coverage outside the band");
}

TEST_F(PaintLayersOwnedEffectTest, an_unbounded_row_keeps_its_subscription_band)
{
  /* A row the trust rules trust maps nothing, so its content footprint declines: the resolved
   * region is the subscription's band, unchanged -- the footprint never widens what the band
   * already names, and never disturbs a row it cannot bound. */
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(run_bake_job());

  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  write_map_rect(*base_map, rcti{400, 401, 400, 401}, 200);
  mark_map_rect(*base_map, rcti{400, 401, 400, 401});
  BKE_paint_layers_bake_notice_changes(*ma);

  int band[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *blur, band));
  rcti resolved;
  const rcti *region = bake_effect_region_resolve(*ma, *blur, resolved);
  ASSERT_NE(region, nullptr);
  EXPECT_EQ(region->xmin, band[0]);
  EXPECT_EQ(region->xmax, band[1]);
  EXPECT_EQ(region->ymin, band[2]);
  EXPECT_EQ(region->ymax, band[3]);
}

TEST_F(PaintLayersOwnedEffectTest, the_job_offers_the_footprint_when_no_band_exists)
{
  /* The first bake has no subscription band, so the content footprint is the fallback region the
   * provider is offered. The source gains an Image Texture, so the row's Mapping applies and the
   * footprint can bound it. The dispatch narrows; the read-back stays whole (v1). */
  texture_link(*source, *add_image("FootprintProviderSource"), "Metallic");
  owned_effect_gpu_test::g_provider_had_region = false;
  owned_effect_gpu_test::g_provider_region = rcti{};
  BKE_paint_layers_owned_effect_gpu_provider_set(
      &owned_effect_gpu_test::owned_effect_gpu_provider_records);
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mapping_supported(*ma, *row));
  const float scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  ASSERT_TRUE(run_bake_job());
  EXPECT_TRUE(owned_effect_gpu_test::g_provider_had_region)
      << "the first bake has no subscription band, so the footprint is the fallback";
  EXPECT_LE(BLI_rcti_size_x(&owned_effect_gpu_test::g_provider_region), 258);
  EXPECT_LE(BLI_rcti_size_y(&owned_effect_gpu_test::g_provider_region), 258);
}

TEST_F(PaintLayersOwnedEffectTest, an_unbounded_row_offers_the_provider_no_fallback)
{
  /* A row whose mapping repeats an axis bounds nothing: the provider is offered no region and
   * computes the whole canvas, the shape it had before the fallback existed. */
  texture_link(*source, *add_image("FootprintProviderSource"), "Metallic");
  owned_effect_gpu_test::g_provider_had_region = false;
  BKE_paint_layers_owned_effect_gpu_provider_set(
      &owned_effect_gpu_test::owned_effect_gpu_provider_records);
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  const float scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X));
  ASSERT_TRUE(run_bake_job());
  EXPECT_FALSE(owned_effect_gpu_test::g_provider_had_region);
}

TEST_F(PaintLayersOwnedEffectTest, only_an_effect_on_a_row_without_a_bake_of_its_own_owns_its_result)
{
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  EXPECT_TRUE(paint_layer_effect_owns_result(*ma, *row, *blur));
  EXPECT_TRUE(paint_layer_is_owned_effect(*ma, *blur));

  /* A painted row holds the effect inside its own content bake: the effect owns nothing there. */
  PaintLayerAddParams params;
  MaterialPaintLayer *painted = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(painted, nullptr);
  MaterialPaintLayer *painted_blur = BKE_paint_layers_filter_add(
      *ma, painted, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(painted_blur, nullptr);
  EXPECT_FALSE(paint_layer_effect_owns_result(*ma, *painted, *painted_blur));
  EXPECT_FALSE(paint_layer_is_owned_effect(*ma, *painted_blur));
}

TEST_F(PaintLayersOwnedEffectTest, an_owned_effect_reads_no_surface_while_its_row_cannot_project)
{
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(paint_layer_is_owned_effect(*ma, *blur));
  /* The effect owns its result only on a row that is not projected, so its input reads no surface;
   * the predicate asks the row's projection, which is what makes it true if that rule is lifted. */
  EXPECT_FALSE(paint_layers_bake_node_reads_surface(*ma, *blur));
  EXPECT_EQ(paint_layers_bake_node_reads_surface(*ma, *blur),
            BKE_paint_layers_projection_applies(*ma, *row));
}

TEST_F(PaintLayersOwnedEffectTest, the_hash_follows_the_parameters_and_not_the_visibility)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  uint32_t before[2], after[2];
  hash_get(*ma, *blur, before);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, blur, false));
  hash_get(*ma, *blur, after);
  EXPECT_EQ(before[0], after[0]);
  EXPECT_EQ(before[1], after[1]) << "switching the effect off moves no pixel of its result";
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, blur, true));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, blur, 0.4f));
  hash_get(*ma, *blur, after);
  EXPECT_TRUE(before[0] == after[0] && before[1] == after[1])
      << "the opacity is laid live over the map";
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, blur, 1.0f));

  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 3));
  hash_get(*ma, *blur, after);
  EXPECT_FALSE(before[0] == after[0] && before[1] == after[1])
      << "a wider radius is another result";
}

TEST_F(PaintLayersOwnedEffectTest, the_planner_bakes_the_result_and_a_toggle_asks_for_nothing)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* The synchronous plan computes nothing: the whole-canvas compute is the worker job's. */
  bool changed = false;
  BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed);
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *blur));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  ASSERT_TRUE(run_bake_job());
  ASSERT_NE(blur->bake, nullptr);
  EXPECT_NE(blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr)
      << "the result is a map of the effect's own";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0)
      << "the map holds the result, the cache keeps no copy";
  EXPECT_FALSE(run_bake_job()) << "nothing is left due";

  /* The visibility is the graph's opacity input: the map stays current and no job is queued. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, blur, false));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_FALSE(run_bake_job());
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, blur, true));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_FALSE(run_bake_job());
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0) << "the toggle computed nothing";

  /* A parameter is another result: the effect is due and the job bakes it again. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 3));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));
  EXPECT_TRUE(run_bake_job());
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

TEST_F(PaintLayersOwnedEffectTest, a_result_that_went_stale_while_the_worker_ran_is_not_written)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);

  /* The radius changes after the worker computed the old one: its pixels must not be stamped as the
   * current result. */
  EXPECT_FALSE(run_bake_job([&]() {
    ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 3));
  }));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma)) << "the next plan queues it again";

  EXPECT_TRUE(run_bake_job());
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

TEST_F(PaintLayersOwnedEffectTest, a_cancelled_job_writes_no_result)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, []() { return true; });
  EXPECT_FALSE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

TEST_F(PaintLayersOwnedEffectTest, a_new_render_of_the_source_stales_the_result)
{
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(run_bake_job());
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* A pixel edit of the map the effect reads reaches it through its subscription. */
  Image *source_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(source_map, nullptr);
  BKE_image_partial_update_mark_full_update(source_map);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_TRUE(run_bake_job());
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

/**
 * The worker job lays only the band the effect's subscription named at the creation, the way the
 * inline writer does: the texel outside it keeps what the previous bake put there. The job's
 * compute itself may answer the whole canvas (the cache entry behind a regional patch is dropped
 * when the map lands), which is why the write band starts from the creation's region rather than
 * from the compute's own rectangle.
 */
TEST_F(PaintLayersOwnedEffectTest, a_regional_job_write_leaves_the_rest_of_the_effect_map)
{
  MaterialPaintLayer *blur = nullptr;
  /* A 512-square canvas keeps the edit's 256-pixel partial-update chunk away from the canary. */
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(run_bake_job());
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  Image *effect_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(effect_map, nullptr);
  int width = 0;
  int height = 0;
  BKE_image_get_size(effect_map, nullptr, &width, &height);
  ASSERT_EQ(width, 512);
  ASSERT_EQ(height, 512);
  const uint32_t effect_map_uid = effect_map->id.session_uid;

  /* A flat grey the blur keeps flat, so the map outside the edit is known: 120 everywhere. */
  const rcti whole{0, width, 0, height};
  write_map_rect(*base_map, whole, 120);
  mark_map_rect(*base_map, whole);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_bake_job());
  {
    const Vector<uchar> flat = read_map(*effect_map);
    ASSERT_EQ(flat.size(), int64_t(width) * height * 4);
    EXPECT_NEAR(map_byte_at(flat, width, width / 2, height / 2), 120, 3);
  }

  /* A canary far from the edit: a regional write leaves it, a whole one overwrites it. */
  const int canary_x = width - 1;
  const int canary_y = height - 1;
  write_map_rect(*effect_map, rcti{canary_x, canary_x + 1, canary_y, canary_y + 1}, 17);

  const rcti dirty{5, 7, 3, 5};
  edit_map_rect(*base_map, dirty, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  int reported[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *blur, reported))
      << "the edit reached the effect's subscription as a rectangle";
  uint32_t current_hash[2];
  BKE_paint_layers_bake_hash(*ma, *blur, current_hash);
  ASSERT_EQ(current_hash[0], blur->bake->hash[0]);
  ASSERT_EQ(current_hash[1], blur->bake->hash[1]);

  ASSERT_TRUE(run_bake_job());
  Image *after_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(after_map, nullptr);
  EXPECT_EQ(after_map, effect_map) << "the regional write reuses the map";
  EXPECT_EQ(after_map->id.session_uid, effect_map_uid);
  const Vector<uchar> regional = read_map(*after_map);
  ASSERT_EQ(regional.size(), int64_t(width) * height * 4);
  EXPECT_EQ(map_byte_at(regional, width, canary_x, canary_y), 17)
      << "the job wrote the region only, as the canary expects";
  EXPECT_GT(
      map_byte_at(regional,
                  width,
                  (dirty.xmin + dirty.xmax) / 2,
                  (dirty.ymin + dirty.ymax) / 2),
      150)
      << "the edit's band was laid";
}

/** A registered provider that always refuses, the CPU fallback's own trigger. */
static std::shared_ptr<ImBuf> owned_effect_gpu_provider_refuses(
    const Material & /*ma*/,
    const MaterialPaintLayer & /*row*/,
    const MaterialPaintLayer & /*effect*/,
    const int /*channel*/,
    const int /*tile_number*/,
    const rcti * /*changed_region*/)
{
  return nullptr;
}

TEST_F(PaintLayersOwnedEffectTest, a_refusing_gpu_provider_leaves_the_cpu_result)
{
  BKE_paint_layers_owned_effect_gpu_provider_set(&owned_effect_gpu_provider_refuses);
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(run_bake_job());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *effect_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  ASSERT_NE(effect_map, nullptr);

  /* A flat grey the blur keeps flat: the same 120 the regional test expects. */
  const rcti whole{0, 512, 0, 512};
  write_map_rect(*base_map, whole, 120);
  mark_map_rect(*base_map, whole);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_bake_job());
  const Vector<uchar> flat = read_map(*effect_map);
  ASSERT_EQ(flat.size(), int64_t(512) * 512 * 4);
  EXPECT_NEAR(map_byte_at(flat, 512, 256, 256), 120, 3)
      << "a provider that refuses leaves the CPU compute exactly as it was";
}

namespace owned_effect_gpu_test {

/** How many channels the provider below was asked for. */
int g_provider_calls = 0;

/** A provider answering every channel with a flat 8-square read-back of white, opaque. White is
 * the one grey that lands at 255 in a byte map whatever the colour management does to it. */
static std::shared_ptr<ImBuf> owned_effect_gpu_provider_flat(const Material & /*ma*/,
                                                             const MaterialPaintLayer & /*row*/,
                                                             const MaterialPaintLayer & /*effect*/,
                                                             const int /*channel*/,
                                                             const int /*tile_number*/,
                                                             const rcti * /*changed_region*/)
{
  g_provider_calls++;
  ImBuf *buffer = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  if (buffer == nullptr) {
    return {};
  }
  float *pixels = buffer->float_data_for_write();
  for (const int i : IndexRange(8 * 8)) {
    pixels[i * 4 + 0] = 1.0f;
    pixels[i * 4 + 1] = 1.0f;
    pixels[i * 4 + 2] = 1.0f;
    pixels[i * 4 + 3] = 1.0f;
  }
  return std::shared_ptr<ImBuf>(buffer, [](ImBuf *ibuf) { IMB_freeImBuf(ibuf); });
}

}  // namespace owned_effect_gpu_test

TEST_F(PaintLayersOwnedEffectTest, the_creation_converts_the_gpu_result_before_the_worker)
{
  /* A float RGBA of the whole canvas is 256 MiB at 4K per channel; the job has to hold the map's own
   * format from the moment the provider answers, not that read-back through the worker's stage. */
  owned_effect_gpu_test::g_provider_calls = 0;
  BKE_paint_layers_owned_effect_gpu_provider_set(
      &owned_effect_gpu_test::owned_effect_gpu_provider_flat);
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  ASSERT_NE(row, nullptr);

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  EXPECT_GT(owned_effect_gpu_test::g_provider_calls, 0)
      << "the creation asks the provider for every GPU-computed channel";
  /* The store holds converted channel maps: at most one 512-square RGBA per channel the effect
   * filters, in the widest format a map can take (float). The bound is a ceiling, not an exact
   * number: the map is byte or float depending on #bake_channel_needs_float. */
  ASSERT_GT(blur->channels_num, 0);
  const int64_t ceiling = int64_t(blur->channels_num) * 512 * 512 * 4 * int64_t(sizeof(float));
  EXPECT_GT(BKE_paint_layers_bake_job_retained_bytes(*job), 0);
  EXPECT_LE(BKE_paint_layers_bake_job_retained_bytes(*job), ceiling);

  BKE_paint_layers_bake_job_compute(*job, {}, {});
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  Image *effect_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(effect_map, nullptr);
  const Vector<uchar> pixels = read_map(*effect_map);
  ASSERT_FALSE(pixels.is_empty());
  EXPECT_NEAR(map_byte_at(pixels, 512, 256, 256), 255, 1)
      << "the converted GPU answer is the map the commit lays";
}

TEST_F(PaintLayersOwnedEffectTest, a_precompute_over_the_budget_falls_back_to_the_cpu)
{
  /* A spent budget means the creation dispatches nothing: every channel falls back to the CPU
   * path, the result a refusing provider gives. The budget rules the creation alone; the compute
   * below runs on the main thread in this harness, where the CPU path's own GPU gate may still ask
   * the provider (a real job's worker cannot), so the call count is read before `compute`. */
  owned_effect_gpu_test::g_provider_calls = 0;
  BKE_paint_layers_owned_effect_gpu_provider_set(
      &owned_effect_gpu_test::owned_effect_gpu_provider_flat);
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  ASSERT_NE(row, nullptr);

  /* The default is read before the budget is spent, so the test never hard-codes it twice. */
  const size_t default_budget = paint_layers_bake_gpu_precompute_budget();
  paint_layers_bake_gpu_precompute_budget_set(0);
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  EXPECT_EQ(owned_effect_gpu_test::g_provider_calls, 0)
      << "a zero budget leaves every channel to the CPU path at creation";

  BKE_paint_layers_bake_job_compute(*job, {}, {});
  const bool landed = BKE_paint_layers_bake_job_commit(*job);
  BKE_paint_layers_bake_job_free(*job);
  paint_layers_bake_gpu_precompute_budget_set(default_budget);

  EXPECT_TRUE(landed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur))
      << "the CPU fallback still bakes the effect";
}

TEST_F(PaintLayersOwnedEffectTest, a_valid_region_is_the_offered_band_grown_and_clipped)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(*ma, *blur);
  ASSERT_NE(row, nullptr);

  const rcti offered{100, 140, 200, 240};
  rcti valid;
  BKE_paint_layers_effect_valid_region(*ma,
                                       *row,
                                       *blur,
                                       PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                       PAINT_TILE_NUMBER_BASE,
                                       &offered,
                                       512,
                                       512,
                                       valid);
  EXPECT_LE(valid.xmin, offered.xmin);
  EXPECT_LE(valid.ymin, offered.ymin);
  EXPECT_GE(valid.xmax, offered.xmax);
  EXPECT_GE(valid.ymax, offered.ymax);
  EXPECT_GE(valid.xmin, 0);
  EXPECT_GE(valid.ymin, 0);
  EXPECT_LE(valid.xmax, 512);
  EXPECT_LE(valid.ymax, 512);

  /* No band, or an empty one: the whole canvas, the CPU path's own rule. */
  rcti whole;
  BKE_paint_layers_effect_valid_region(*ma,
                                       *row,
                                       *blur,
                                       PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                       PAINT_TILE_NUMBER_BASE,
                                       nullptr,
                                       512,
                                       512,
                                       whole);
  EXPECT_EQ(whole.xmin, 0);
  EXPECT_EQ(whole.ymin, 0);
  EXPECT_EQ(whole.xmax, 512);
  EXPECT_EQ(whole.ymax, 512);

  const rcti empty{10, 10, 10, 10};
  BKE_paint_layers_effect_valid_region(*ma,
                                       *row,
                                       *blur,
                                       PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                       PAINT_TILE_NUMBER_BASE,
                                       &empty,
                                       512,
                                       512,
                                       whole);
  EXPECT_EQ(whole.xmax, 512);
  EXPECT_EQ(whole.ymax, 512);
}

/**
 * The inline writer with a changed rectangle writes only the part of the map the effect cache
 * answers for, and that part is the picture a full rebake of the same inputs gives. The texel left
 * outside the write keeps what the earlier bake put there -- a full write would have recomputed
 * it.
 */
TEST_F(PaintLayersOwnedEffectTest, a_regional_write_matches_the_full_one_and_leaves_the_rest)
{
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  int canvas_w = 0;
  int canvas_h = 0;
  BKE_image_get_size(base_map, nullptr, &canvas_w, &canvas_h);
  ASSERT_GE(canvas_w, 8);
  ASSERT_GE(canvas_h, 8);
  /* A flat grey the blur keeps flat, so a missing part of the write is visible. */
  const rcti whole{0, canvas_w, 0, canvas_h};
  write_map_rect(*base_map, whole, 120);
  mark_map_rect(*base_map, whole);

  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur)) << "the first bake writes whole";
  Image *effect_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(effect_map, nullptr);
  int width = 0;
  int height = 0;
  BKE_image_get_size(effect_map, nullptr, &width, &height);
  ASSERT_EQ(width, canvas_w);
  ASSERT_EQ(height, width);
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the write keeps its cache entry; the next regional patch builds on it";

  /* A texel far from the edit: a regional write leaves it alone, a whole one computes it. */
  write_map_rect(*effect_map, rcti{0, 1, 0, 1}, 255);
  const rcti dirty{5, 7, 3, 5};
  edit_map_rect(*base_map, dirty, 200);

  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur, &dirty));
  const Vector<uchar> regional = read_map(*effect_map);
  ASSERT_EQ(regional.size(), int64_t(width) * height * 4);
  EXPECT_EQ(map_byte_at(regional, width, 0, 0), 255)
      << "the regional write left the texel outside the edit as it was";

  /* A full rebake of the same inputs is the picture the regional write must have produced. */
  paint_layer_owned_effect_cache_drop(
      *ma, *blur, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR), PAINT_TILE_NUMBER_BASE);
  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  const Vector<uchar> full = read_map(*effect_map);
  ASSERT_EQ(full.size(), regional.size());
  EXPECT_NE(map_byte_at(full, width, 0, 0), 255) << "the full write recomputed the texel";
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      if (x == 0 && y == 0) {
        continue; /* The one texel the regional write left as it found it, by design. */
      }
      EXPECT_NEAR(map_byte_at(regional, width, x, y), map_byte_at(full, width, x, y), 2)
          << "the regional write's texel " << x << "," << y;
    }
  }
}

/**
 * A map #bake_service_image replaced (another size or format) is blank, so a partial write into it
 * would leave every texel outside the rectangle black. The writer must fall back to the whole
 * buffer the cache answers with, without a second compute.
 */
TEST_F(PaintLayersOwnedEffectTest, a_replaced_map_is_written_whole)
{
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  int canvas_w = 0;
  int canvas_h = 0;
  BKE_image_get_size(base_map, nullptr, &canvas_w, &canvas_h);
  ASSERT_GE(canvas_w, 8);
  ASSERT_GE(canvas_h, 8);
  const rcti whole{0, canvas_w, 0, canvas_h};
  write_map_rect(*base_map, whole, 120);
  mark_map_rect(*base_map, whole);

  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  ASSERT_NE(blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);

  /* Another size is another map: the writer will make a blank one at the result's size. */
  Image *replaced = bake_service_image(
      *bmain, *blur, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR), canvas_w * 2, true, false, 0);
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], replaced);

  const rcti dirty{5, 7, 3, 5};
  edit_map_rect(*base_map, dirty, 200);
  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur, &dirty));

  Image *written = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(written, nullptr);
  int width = 0;
  int height = 0;
  BKE_image_get_size(written, nullptr, &width, &height);
  EXPECT_EQ(width, canvas_w) << "the map takes the result's size again";
  EXPECT_EQ(height, canvas_w);
  const Vector<uchar> pixels = read_map(*written);
  ASSERT_EQ(pixels.size(), int64_t(width) * height * 4);
  /* Away from the edit the flat input's blur is the same flat grey, not the blank black a partial
   * write into the replaced map would leave. */
  const int band_x0 = std::max(dirty.xmin - 1, 0);
  const int band_x1 = std::min(dirty.xmax + 1, width);
  const int band_y0 = std::max(dirty.ymin - 1, 0);
  const int band_y1 = std::min(dirty.ymax + 1, height);
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      if (x >= band_x0 && x < band_x1 && y >= band_y0 && y < band_y1) {
        continue; /* The band the edit moves, which either write fills. */
      }
      EXPECT_NEAR(map_byte_at(pixels, width, x, y), 120, 3)
          << "a replaced map is written whole, texel " << x << "," << y;
    }
  }
}

/**
 * A bake made at the coarse drag size owes a whole one, so the inline planner may not hand its
 * region to the writer: the texel outside the region keeps what the full write recomputes.
 */
TEST_F(PaintLayersOwnedEffectTest, a_drag_reduced_bake_is_written_whole)
{
  MaterialPaintLayer *blur = nullptr;
  /* The partial-update log reports an edit as the 256-pixel chunks it touches: on a smaller canvas
   * the edit's chunk is the whole canvas and the planner rightly falls back to a whole write. The
   * edit below sits in the first chunk, the canary outside it, so the maps themselves are 512. */
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  int canvas_w = 0;
  int canvas_h = 0;
  BKE_image_get_size(base_map, nullptr, &canvas_w, &canvas_h);
  ASSERT_GE(canvas_w, 8);
  ASSERT_GE(canvas_h, 8);
  const rcti whole{0, canvas_w, 0, canvas_h};
  write_map_rect(*base_map, whole, 120);
  mark_map_rect(*base_map, whole);

  /* A background session has no job to own the effect, so the planner computes it here. */
  ScopedBackground background;
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(blur->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur))
      << "the inline plan finalized the effect before the edit";
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the inline write kept the entry the next region builds on";
  Image *effect_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(effect_map, nullptr);
  const uint32_t effect_map_uid = effect_map->id.session_uid;
  int width = 0;
  int height = 0;
  BKE_image_get_size(effect_map, nullptr, &width, &height);
  ASSERT_EQ(width, canvas_w);
  ASSERT_EQ(height, width);
  const int canary_x = width - 1;
  const int canary_y = height - 1;

  /* A texel far from the edit: a regional write leaves it, a whole one computes it. */
  write_map_rect(*effect_map, rcti{canary_x, canary_x + 1, canary_y, canary_y + 1}, 255);
  const rcti dirty{5, 7, 3, 5};
  edit_map_rect(*base_map, dirty, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  int reported[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *blur, reported))
      << "the edit reached the effect's subscription as a rectangle";
  EXPECT_LE(reported[0], dirty.xmin);
  EXPECT_GE(reported[1], dirty.xmax);
  EXPECT_LE(reported[2], dirty.ymin);
  EXPECT_GE(reported[3], dirty.ymax);
  uint32_t current_hash[2];
  BKE_paint_layers_bake_hash(*ma, *blur, current_hash);
  ASSERT_EQ(current_hash[0], blur->bake->hash[0]);
  ASSERT_EQ(current_hash[1], blur->bake->hash[1]);

  /* Without a reduced bake the planner hands its region on: the texel outside it stands. */
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  Image *after_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(after_map, nullptr);
  EXPECT_EQ(after_map, effect_map) << "the regional write must reuse the map, not replace it";
  EXPECT_EQ(after_map->id.session_uid, effect_map_uid);
  {
    int after_w = 0;
    int after_h = 0;
    BKE_image_get_size(after_map, nullptr, &after_w, &after_h);
    EXPECT_EQ(after_w, width);
    EXPECT_EQ(after_h, height);
    EXPECT_EQ(blur->bake->size, width);
    const Vector<uchar> regional = read_map(*after_map);
    EXPECT_EQ(map_byte_at(regional, width, canary_x, canary_y), 255)
        << "the inline planner wrote the region only, as the test's canary expects";
  }

  /* The reduced-size bake owes a whole one (see #bake_drag_reduced_owes): the planner may not lay
   * a region into it, so the next write recomputes the texel the regional one left alone. */
  write_map_rect(*effect_map, rcti{canary_x, canary_x + 1, canary_y, canary_y + 1}, 255);
  edit_map_rect(*base_map, rcti{1, 3, 3, 5}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  bake_drag_reduced_remember(*ma, *blur, width);

  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  Image *whole_map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(whole_map, nullptr);
  const Vector<uchar> pixels = read_map(*whole_map);
  EXPECT_NE(map_byte_at(pixels, width, canary_x, canary_y), 255)
      << "the drag-reduced bake was recomputed whole, not patched by region";
  EXPECT_NEAR(map_byte_at(pixels, width, canary_x, canary_y), 120, 3);
  bake_drag_reduced_forget(*ma, *blur);
}

/**
 * A Material row reads its source: a stroke painting that source's map is an edit of the row's
 * input even though it does not run inside the row. The row's own source render waits for the
 * stroke to end, like a painted row's content effect waits for a stroke inside it.
 */
TEST_F(PaintLayersOwnedEffectTest, a_source_pixel_stroke_defers_the_material_rows_render)
{
  /* The stroke paints a live map of the source material: the row reads it through its source and
   * subscribes to it, exactly as it does for a painted Source. */
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *source_map = BKE_image_add_generated(
      bmain, 64, 64, "StrokeSource", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(source_map, nullptr);
  texture_link(*source, *source_map, "Base Color");

  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  ASSERT_NE(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  /* The editor subscribes after a landed render; the test plays that call so the mark below
   * reaches the row's subscription. */
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  ASSERT_NE(blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row));

  BKE_paint_layers_bake_stroke_active_set(true);
  BKE_image_partial_update_mark_full_update(source_map);
  BKE_paint_layers_bake_notice_changes(*ma);

  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *row))
      << "a source stroke defers the row even with no active marker inside it";
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *blur))
      << "the effect reads the row's source too: its own bake waits for the same stroke";
  EXPECT_FALSE(BKE_paint_layers_material_row_due(*ma, *row))
      << "the source render waits for the stroke to end";

  BKE_paint_layers_bake_stroke_active_set(false);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *blur));
  EXPECT_TRUE(BKE_paint_layers_material_row_due(*ma, *row))
      << "the stroke is over: the source render is due again";
}

TEST_F(PaintLayersOwnedEffectTest, a_whole_map_chain_keeps_the_whole_canvas)
{
  /* A Color Match reads the whole canvas; no content footprint can bound its result, so the
   * planner must keep the whole canvas even when the row is otherwise patched regionally. */
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur, 512, 512);
  ASSERT_NE(blur, nullptr);
  MaterialPaintLayer *match = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_COLOR_MATCH);
  ASSERT_NE(match, nullptr);
  ASSERT_TRUE(run_bake_job());

  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  write_map_rect(*base_map, rcti{400, 401, 400, 401}, 200);
  mark_map_rect(*base_map, rcti{400, 401, 400, 401});
  BKE_paint_layers_bake_notice_changes(*ma);

  int band[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *match, band))
      << "the whole-map refusal must be reached with a live band, not a missing subscription";
  rcti resolved;
  EXPECT_EQ(bake_effect_region_resolve(*ma, *match, resolved), nullptr);
}

TEST_F(PaintLayersOwnedEffectTest, a_freed_bake_map_is_replaced_not_dereferenced)
{
  MaterialPaintLayer *blur = nullptr;
  ASSERT_NE(add_row_with_blur(&blur), nullptr);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  Image *map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(map, nullptr);

  /* An undo or a file reset can free the map while the runtime bake slot still points at it: the
   * next write must make a new map, not read the freed image. */
  BKE_id_free(bmain, map);
  Image *replacement = bake_service_image(
      *bmain, *blur, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR), 64, true, false, 0);
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], replacement);
}

TEST_F(PaintLayersOwnedEffectTest, a_failed_compute_does_not_keep_the_bake_valid)
{
  MaterialPaintLayer *blur = nullptr;
  MaterialPaintLayer *row = add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* An undo can restore the effect's map (and with it the hash of the pass that filled it) while
   * the source it is computed from cannot be read yet. The pass then answers nothing, and the map
   * it left is the restored one, not the current input: the bake must stop calling itself valid, or
   * the planner would keep showing those restored pixels and never ask again. */
  Material *source_material = row->material;
  row->material = nullptr;
  EXPECT_FALSE(bake_effect_row_write(*bmain, *ma, *blur));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  row->material = source_material;

  /* With the source back the next pass fills the map and stamps the hash as usual. */
  EXPECT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

TEST_F(PaintLayersOwnedEffectTest, a_map_restored_by_an_undo_stales_the_bake)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* A memfile undo re-reads a changed map as a new Image, with a fresh runtime version, while the
   * hash stored beside the row rides the decoded material: the map is not the raster the pass wrote
   * any more, so the bake must stop calling itself current and the planner must fill it again. */
  Image *map = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(map, nullptr);
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *re_read = BKE_image_add_generated(bmain,
                                           64,
                                           64,
                                           "Blur 1 Base Color Bake",
                                           32,
                                           false,
                                           IMA_GENTYPE_BLANK,
                                           black,
                                           false,
                                           false,
                                           false);
  ASSERT_NE(re_read, nullptr);
  blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = re_read;
  BKE_id_free(bmain, map);

  /* Validity alone sees it: the hash still matches, but the map is not the one it was stamped on,
   * so nothing shows it as the current result even before the undo hook runs. */
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_TRUE(BKE_paint_layers_bake_restored_maps_invalidate(*ma));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  /* Already stale: a second check has nothing left to invalidate and must not keep asking for a
   * rebuild. */
  EXPECT_FALSE(BKE_paint_layers_bake_restored_maps_invalidate(*ma));
}

TEST_F(PaintLayersOwnedEffectTest, an_untouched_bake_survives_an_undo_check)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(bake_effect_row_write(*bmain, *ma, *blur));
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* An undo that touched no map of this bake leaves the record matching: no re-bake is owed, and
   * the invalidation must not report one. */
  EXPECT_TRUE(BKE_paint_layers_bake_maps_are_current(*ma, *blur));
  EXPECT_FALSE(BKE_paint_layers_bake_restored_maps_invalidate(*ma));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

TEST_F(PaintLayersOwnedEffectTest, the_composite_reads_the_map_the_planner_wrote)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);

  /* Before the bake the composite computes the result into the cache. */
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(
        BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    ASSERT_EQ(layers.size(), 1);
    ASSERT_EQ(layers[0].content_corrections.size(), 1);
    EXPECT_NE(layers[0].content_corrections[0].effect_color, nullptr);
    EXPECT_EQ(layers[0].content_corrections[0].effect_image, nullptr);
  }

  ASSERT_TRUE(run_bake_job());
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* After it the composite lays the map: no buffer is computed again. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_EQ(layers[0].content_corrections.size(), 1);
  const PaintMaterialCompositeCorrection &laid = layers[0].content_corrections[0];
  EXPECT_EQ(laid.effect_color, nullptr);
  EXPECT_EQ(laid.effect_image, blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0);
}

TEST_F(PaintLayersOwnedEffectTest, the_graph_lays_the_map_and_a_toggle_is_a_value)
{
  MaterialPaintLayer *blur = nullptr;
  add_row_with_blur(&blur);
  ASSERT_NE(blur, nullptr);

  /* Before the bake the graph has nothing to lay for the effect. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  ASSERT_TRUE(run_bake_job());
  ASSERT_NE(blur->bake, nullptr);
  Image *result = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, result), 1)
      << "the effect is an Image Texture over the row's content";

  /* Visibility is the opacity input's value: the shape the group has is the right one. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, blur, false));
  EXPECT_TRUE(BKE_paint_layers_values_sync(*ma));
  EXPECT_TRUE(paint_layers::paint_layers_maps_sync(*ma, *bmain));
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, result), 1);
}

/**
 * A Material row's coverage can be a live image (its source's Alpha output). A region outside the
 * covered part is still kept: the row re-renders its source whole, so the culling must not narrow
 * it to the plane's rectangle.
 */
TEST_F(PaintLayersMaterialBakeTest, an_edit_outside_a_material_rows_coverage_is_kept)
{
  Material *textured = BKE_material_add(bmain, "AlphaTexture");
  principled_source_set_up(*textured, /*graphs=*/false);
  Image *texture = add_image("Cutout");
  texture_link(*textured, *texture, "Alpha", "Alpha");
  MaterialPaintLayer *row = add_material_row(*textured);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  const rcti uncovered = material_test_clear_right_half_alpha(*texture);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);
  material_test_mark_region(*texture, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  material_test_mark_region(*texture, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a Material row re-renders its source whole, so the plane cannot narrow it";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

/**
 * A CPU effect on a Material row owns its result: it bakes apart, and its own subscription reports
 * its edits. The row's bake still re-renders the source whole, so a region on it is never culled
 * by the effect's reach.
 */
TEST_F(PaintLayersMaterialBakeTest, a_material_row_whose_effect_owns_its_result_keeps_every_region)
{
  Material *textured = BKE_material_add(bmain, "AlphaTexture");
  principled_source_set_up(*textured, /*graphs=*/false);
  Image *texture = add_image("Cutout");
  texture_link(*textured, *texture, "Alpha", "Alpha");
  MaterialPaintLayer *row = add_material_row(*textured);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *effect = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(effect, nullptr);
  EXPECT_TRUE(paint_layer_effect_owns_result(*ma, *row, *effect));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  const rcti uncovered = material_test_clear_right_half_alpha(*texture);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);
  material_test_mark_region(*texture, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  material_test_mark_region(*texture, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "an owning effect has its own subscription; the row's regions stay whole";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
}

/**
 * A source whose Alpha is a plain zero covers nothing: every reported region lies outside it, so
 * no region may mark the row stale and start a whole-canvas re-bake.
 */
TEST_F(PaintLayersMaterialBakeTest, an_edit_of_a_material_row_that_covers_nothing_is_dropped)
{
  Material *plain = BKE_material_add(bmain, "PlainZero");
  principled_source_set_up(*plain, /*graphs=*/false);
  /* A map in the source tree, so the row has a subscription with regions to report. */
  Image *map = add_image("SourceMap");
  texture_link(*plain, *map, "Base Color");
  bNode *principled = principled_of(*plain);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.0f;
  BKE_paint_layers_source_material_hash_forget(*plain);

  MaterialPaintLayer *row = add_material_row(*plain);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(*ma, *row);
  ASSERT_EQ(coverage.kind, PaintLayerMaterialCoverageKind::LiveConstant);
  ASSERT_FLOAT_EQ(coverage.constant, 0.0f);

  material_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  material_test_mark_region(*map, rcti{1, 4, 1, 4});
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  EXPECT_FALSE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a row that covers nothing cannot be moved by any region";
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Row source render narrowed by the row's own footprint
 *
 * The planner renders a Fill row's bake whole the first time and, while the maps stand and the
 * description still agrees with them, takes only the rectangle the row's own read chain bounds:
 * the preimage of the unit square under the row's mapping, grown by its blur's halo, aligned
 * outward to the 256 tiles. Scale 5 over a 512 canvas lays the content in `[0, 103)`; the blur runs
 * on the row's own 8-square stack, so its two-texel radius reaches 128 canvas texels, and the pad
 * and halo still align the write rect to `[0, 256) x [0, 256)` -- a quarter of the canvas -- so a
 * sentinel written outside it survives every narrowed write. The row is a Fill,
 * not a Material row: a Material row's maps are rendered from its source by a different path,
 * which this narrowing does not serve.
 * \{ */

TEST_F(PaintLayersOwnedEffectTest, a_bounded_fill_row_full_edit_is_written_in_its_footprint)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 5.0f, 512, "Bounded");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_plan());
  ASSERT_NE(row->bake, nullptr);
  ASSERT_NE(row->bake->coverage, nullptr);
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  const Vector<uchar> before = read_map(*base_map);

  /* A texel outside the `[0, 256)` write rect: a narrowed write leaves it, a whole one recomputes
   * it. */
  write_map_rect(*base_map, rcti{460, 470, 460, 470}, 255);
  edit_map_rect(*map, rcti{0, 8, 0, 8}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);

  /* The reference: an identical row whose first bake happens after the edit, so its one render is
   * a full one of the same edited content. */
  MaterialPaintLayer *reference = nullptr;
  Image *reference_map = nullptr;
  add_bounded_fill(&reference, &reference_map, 5.0f, 512, "BoundedRef");
  ASSERT_NE(reference, nullptr);
  write_map_rect(*reference_map, rcti{0, 8, 0, 8}, 200);

  ASSERT_TRUE(run_plan());
  Image *after_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_EQ(after_map, base_map) << "the narrowed write reuses the map, it does not replace it";
  const Vector<uchar> after = read_map(*after_map);
  const Vector<uchar> full = read_map(*reference->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  ASSERT_EQ(after.size(), full.size());

  EXPECT_EQ(map_byte_at(after, 512, 460, 460), 255)
      << "the write took the footprint's rectangle alone; the sentinel outside it stands";
  /* Inside the footprint the edit landed; just past the content's edge and outside both, the
   * bounded row lays what the full render lays. */
  EXPECT_NEAR(map_byte_at(after, 512, 60, 60), map_byte_at(full, 512, 60, 60), 3);
  EXPECT_NEAR(map_byte_at(after, 512, 130, 60), map_byte_at(full, 512, 130, 60), 3)
      << "the halo just past the content's edge matches the full render";
  EXPECT_NEAR(map_byte_at(after, 512, 135, 60), map_byte_at(full, 512, 135, 60), 3);
  EXPECT_NEAR(map_byte_at(after, 512, 300, 300), map_byte_at(full, 512, 300, 300), 3);
  EXPECT_NE(map_byte_at(after, 512, 60, 60), map_byte_at(before, 512, 60, 60))
      << "the edited content was re-rendered inside the footprint";
}

TEST_F(PaintLayersOwnedEffectTest, an_unbounded_fill_row_keeps_re_rendering_whole)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 512, "Unbounded");
  ASSERT_NE(row, nullptr);
  /* The mapping off reads the map at the raw UV: the content spans wherever the map's alpha does,
   * and nothing bounds it. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, false));
  ASSERT_TRUE(run_plan());
  ASSERT_NE(row->bake, nullptr);
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  const Vector<uchar> before = read_map(*base_map);
  write_map_rect(*base_map, rcti{460, 470, 460, 470}, 255);

  edit_map_rect(*map, rcti{0, 8, 0, 8}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  const size_t partial_before = paint_layer_effect_partial_compute_count();
  ASSERT_TRUE(run_plan());
  const Vector<uchar> after = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);

  EXPECT_EQ(paint_layer_effect_partial_compute_count(), partial_before)
      << "an unbounded row offers no rectangle, so the effect keeps the whole-canvas compute";
  EXPECT_NE(map_byte_at(after, 512, 460, 460), 255)
      << "an unbounded row is rendered whole; the sentinel outside any footprint is rewritten";
  EXPECT_NE(map_byte_at(after, 512, 460, 460), map_byte_at(before, 512, 460, 460));
  EXPECT_NE(map_byte_at(after, 512, 300, 300), map_byte_at(before, 512, 300, 300))
      << "the edited content covers the canvas, as the unmapped fill reads it";
}

TEST_F(PaintLayersOwnedEffectTest, a_structural_change_re_renders_a_bounded_fill_row_whole)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 512, "Structural");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_plan());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  const Vector<uchar> before = read_map(*base_map);
  write_map_rect(*base_map, rcti{460, 470, 460, 470}, 255);

  edit_map_rect(*map, rcti{0, 8, 0, 8}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  /* Scale 5 lays the content in `[0, 103)` -- its aligned rect is still `[0, 256)`, so only a
   * whole render can reach the sentinel. The row's bake hash folds its mapping values (a row with
   * a CPU effect bakes them in), so the change is a structural one. */
  const float scale[2] = {5.0f, 5.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*ma, *row, hash);
  ASSERT_TRUE(hash[0] != row->bake->hash[0] || hash[1] != row->bake->hash[1])
      << "the test's premise is a structural hash that moved";

  ASSERT_TRUE(run_plan());
  const Vector<uchar> after = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_NE(map_byte_at(after, 512, 460, 460), 255)
      << "a hash mismatch renders whole; the old content outside the new footprint is not kept";
  EXPECT_NE(map_byte_at(after, 512, 80, 80), map_byte_at(before, 512, 80, 80))
      << "the whole render laid the edited fill at the new structure's scale";
}

TEST_F(PaintLayersOwnedEffectTest, a_first_bake_lays_transparent_outside_a_bounded_footprint)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 512, "First");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_plan());
  ASSERT_NE(row->bake, nullptr);
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);

  /* The first bake is a full render: the bounded fill lays opaque content inside its footprint and
   * transparent outside it -- alpha 0, not black with an alpha of 1 -- and the coverage follows. */
  const Vector<uchar> pixels = read_map(*base_map);
  EXPECT_EQ(pixels[(int64_t(60) * 512 + 60) * 4 + 3], 255)
      << "the fill's own alpha inside the footprint";
  EXPECT_EQ(pixels[(int64_t(400) * 512 + 400) * 4 + 3], 0)
      << "nothing of a bounded row lies outside its footprint";
  Image *coverage = row->bake->coverage;
  ASSERT_NE(coverage, nullptr);
  const Vector<uchar> coverage_pixels = read_map(*coverage);
  EXPECT_EQ(map_byte_at(coverage_pixels, 512, 60, 60), 255);
  EXPECT_EQ(map_byte_at(coverage_pixels, 512, 400, 400), 0);
}

TEST_F(PaintLayersOwnedEffectTest, a_whole_map_effect_keeps_a_bounded_fill_row_whole)
{
  Image *map = add_image("Matched");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  params.name = "Matched";
  MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(row, nullptr);
  add_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map);
  for (int i = row->channels_num - 1; i >= 0; i--) {
    const int channel = int(row->channels[i].channel);
    if (channel != int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)) {
      ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, row, eMaterialPaintChannel(channel)));
    }
  }
  const float scale[2] = {4.0f, 4.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  MaterialPaintLayer *match = BKE_paint_layers_filter_add(*ma, row,
                                                         MA_PAINT_LAYER_FILTER_COLOR_MATCH);
  ASSERT_NE(match, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *row, 512));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(run_plan());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  write_map_rect(*base_map, rcti{460, 470, 460, 470}, 255);

  edit_map_rect(*map, rcti{0, 8, 0, 8}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_plan());
  const Vector<uchar> after = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_NE(map_byte_at(after, 512, 460, 460), 255)
      << "a Color Match reads the whole canvas; no footprint narrows its row's render";
}

TEST_F(PaintLayersOwnedEffectTest, a_laid_correction_keeps_the_row_source_footprint_off)
{
  Image *map = add_image("FillLaid");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  params.name = "FillLaid";
  MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(row, nullptr);
  add_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map);
  const float scale[2] = {4.0f, 4.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, row, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  /* A CPU effect makes the row bakeable, so the refusal below is the laid correction's. */
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));
  /* A Material correction lays its own maps through its own frame, which the row's chain does not
   * bound: v1 refuses the rectangle instead of unioning the frames. The fixture's source supplies
   * Metallic as a plain value, so the correction lays. */
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add_material(
      *ma, row, "Laid", source);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(paint_layer_effect_lays(*ma, *correction, PAINT_MATERIAL_CHANNEL_METALLIC))
      << "the test's refusal is only meaningful over a correction that lays";
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  rcti region;
  EXPECT_FALSE(bke::paint_layers::footprint::row_source_region(
      *ma, *row, PaintLayerTileContext{.tile_number = PAINT_TILE_NUMBER_BASE}, 512, region));
}

TEST_F(PaintLayersOwnedEffectTest, a_full_update_re_renders_a_bounded_fill_row_in_its_footprint)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 5.0f, 512, "FullUpdate");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_plan());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *coverage = row->bake->coverage;
  ASSERT_NE(base_map, nullptr);
  ASSERT_NE(coverage, nullptr);
  const Vector<uchar> before = read_map(*base_map);
  /* Sentinels outside the `[0, 256)` write rect, in the colour map and in the coverage map. */
  write_map_rect(*base_map, rcti{460, 470, 460, 470}, 255);
  write_map_rect(*coverage, rcti{460, 470, 460, 470}, 255);

  /* A full update carries no rectangle: the row is due again although its hash still matches, which
   * is the path a full render takes on standing maps -- and it renders the footprint alone. */
  BKE_image_partial_update_mark_full_update(map);
  BKE_paint_layers_bake_notice_changes(*ma);
  int band[4] = {};
  ASSERT_FALSE(BKE_paint_layers_bake_changed_region(*ma, *row, band))
      << "the test's premise is a due row with no subscription band";

  ASSERT_TRUE(run_plan());
  const Vector<uchar> after = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(map_byte_at(after, 512, 460, 460), 255)
      << "the full render took the footprint's rectangle; the sentinel outside it stands";
  EXPECT_EQ(map_byte_at(read_map(*row->bake->coverage), 512, 460, 460), 255)
      << "the coverage write takes the same rectangle";
  /* Inside the footprint the re-render lays the content the first bake laid. */
  EXPECT_EQ(map_byte_at(after, 512, 60, 60), map_byte_at(before, 512, 60, 60));
  EXPECT_NEAR(map_byte_at(after, 512, 130, 60), map_byte_at(before, 512, 130, 60), 3);
}

TEST_F(PaintLayersOwnedEffectTest, an_edit_whose_band_misses_the_footprint_keeps_the_band_whole)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  /* The source map shares the canvas' side, so a far-corner edit names a far-corner band. */
  add_bounded_fill(&row, &map, 4.0f, 512, "MissBand", 512);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_plan());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  const Vector<uchar> before = read_map(*base_map);
  write_map_rect(*base_map, rcti{460, 470, 460, 470}, 255);

  /* A source edit in the far corner maps to the band `[256, 512)`, which the tile-aligned footprint
   * `[0, 256)` misses: v1 keeps the band whole rather than write nothing, and the content it lays
   * there is the transparent outside the row's footprint. */
  edit_map_rect(*map, rcti{500, 512, 500, 512}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  int band[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, band));
  ASSERT_TRUE(run_plan());
  const Vector<uchar> after = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_NE(map_byte_at(after, 512, 460, 460), 255)
      << "the band was rendered whole; the sentinel inside it is rewritten";
  EXPECT_EQ(map_byte_at(after, 512, 60, 60), map_byte_at(before, 512, 60, 60))
      << "the footprint the band misses keeps the previous render";
}

/* -------------------------------------------------------------------- */
/** \name Row source render narrowed in the worker job
 *
 * A heavy row (its maps at or above the heavy side) leaves the planner for the worker job, which
 * composites from the localized copy and commits on the main thread. With the maps standing and
 * the hash unchanged, the creation resolves the same footprint and the job renders and writes its
 * tile-aligned rectangle; a first bake, a structural change and a live row keep the whole canvas.
 * Scale 4 over a 2048 canvas lays the content in the first 512 texels; the Blur runs on the row's
 * 8-square stack, so its two-texel radius reaches 512 canvas texels, and the footprint grown by
 * that halo aligns outward to `[0, 1024)` -- half the canvas -- so a sentinel written outside it
 * survives every narrowed write.
 * \{ */

TEST_F(PaintLayersOwnedEffectTest, a_bounded_fill_row_heavy_edit_is_written_in_its_footprint)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 2048, "HeavyBounded");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_bake_job());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  ASSERT_NE(row->bake->coverage, nullptr);
  /* The first bake is a whole render. The bounded fill lays its content in `[0, 512)`; the Blur
   * runs on the row's own 8-square stack, so its two-texel radius reaches 512 canvas texels and
   * the content alpha spreads to `[0, 1024)`. Colour stays nowhere outside the content. */
  const Vector<uchar> first = read_map(*base_map);
  EXPECT_EQ(first[(int64_t(1200) * 2048 + 1200) * 4 + 3], 0)
      << "the spread stops at the Blur's halo in canvas texels";
  EXPECT_GT(first[(int64_t(1000) * 2048 + 1000) * 4 + 3], 0)
      << "the Blur spreads the content alpha over the empty map";
  EXPECT_EQ(first[(int64_t(1000) * 2048 + 1000) * 4 + 0], 0)
      << "the content's colour lies inside its footprint only";
  EXPECT_EQ(first[(int64_t(60) * 2048 + 60) * 4 + 3], 255)
      << "the content itself stays opaque";
  /* Sentinels outside the `[0, 1024)` write rect -- the footprint grown by the stack-scaled halo --
   * in the colour map and in the coverage map. */
  write_map_rect(*base_map, rcti{1500, 1510, 1500, 1510}, 255);
  write_map_rect(*row->bake->coverage, rcti{1500, 1510, 1500, 1510}, 255);

  /* The reference: an identical heavy row whose first (whole) bake happens after the same edit. */
  MaterialPaintLayer *reference = nullptr;
  Image *reference_map = nullptr;
  add_bounded_fill(&reference, &reference_map, 4.0f, 2048, "HeavyBoundedRef");
  ASSERT_NE(reference, nullptr);
  edit_map_rect(*map, rcti{0, 8, 0, 8}, 200);
  edit_map_rect(*reference_map, rcti{0, 8, 0, 8}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);

  /* A whole-canvas effect cannot be cropped; a neighbourhood one can, and this narrowed bake is
   * the one that must offer its rectangle to the folded Blur. */
  const size_t partial_before = paint_layer_effect_partial_compute_count();

  ASSERT_TRUE(run_bake_job());
  Image *after_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_EQ(after_map, base_map) << "the narrowed write reuses the map, it does not replace it";
  const Vector<uchar> after = read_map(*after_map);
  const Vector<uchar> full = read_map(*reference->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);

  EXPECT_GT(paint_layer_effect_partial_compute_count(), partial_before)
      << "the narrowed bake computed the folded Blur over its rectangle alone";

  EXPECT_EQ(map_byte_at(after, 2048, 1500, 1500), 255)
      << "the job wrote the footprint's rectangle alone; the sentinel outside it stands";
  EXPECT_EQ(map_byte_at(read_map(*row->bake->coverage), 2048, 1500, 1500), 255)
      << "the coverage write takes the same rectangle";
  EXPECT_NEAR(map_byte_at(after, 2048, 60, 60), map_byte_at(full, 2048, 60, 60), 3)
      << "the content inside the footprint matches a whole render";
  EXPECT_NEAR(map_byte_at(after, 2048, 514, 514), map_byte_at(full, 2048, 514, 514), 3)
      << "the halo just past the content's edge matches a whole render";
  EXPECT_NEAR(map_byte_at(after, 2048, 900, 900), map_byte_at(full, 2048, 900, 900), 3)
      << "the stack-scaled halo's outskirt matches a whole render too";
  EXPECT_NE(map_byte_at(after, 2048, 60, 60), 0)
      << "the edited content was re-rendered inside the footprint";
}

TEST_F(PaintLayersOwnedEffectTest, a_full_update_re_renders_a_heavy_fill_row_in_its_footprint)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 2048, "HeavyFullUpdate");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_bake_job());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  ASSERT_NE(row->bake->coverage, nullptr);
  const Vector<uchar> before = read_map(*base_map);
  /* Sentinels outside the `[0, 1024)` write rect -- the footprint grown by the stack-scaled halo,
   * the same rectangle the rewritten bounded-fill test above lands on. */
  write_map_rect(*base_map, rcti{1500, 1510, 1500, 1510}, 255);
  write_map_rect(*row->bake->coverage, rcti{1500, 1510, 1500, 1510}, 255);

  /* A full update carries no rectangle: the row is due again although its hash still matches, and
   * the job's full render takes the footprint alone. */
  BKE_image_partial_update_mark_full_update(map);
  BKE_paint_layers_bake_notice_changes(*ma);
  int band[4] = {};
  ASSERT_FALSE(BKE_paint_layers_bake_changed_region(*ma, *row, band))
      << "the test's premise is a due row with no subscription band";

  const size_t partial_before = paint_layer_effect_partial_compute_count();
  ASSERT_TRUE(run_bake_job());
  const Vector<uchar> after = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_GT(paint_layer_effect_partial_compute_count(), partial_before)
      << "a full update still offers the footprint to the folded effect";
  EXPECT_EQ(map_byte_at(after, 2048, 1500, 1500), 255)
      << "the job's full render took the footprint's rectangle; the sentinel outside it stands";
  EXPECT_EQ(map_byte_at(read_map(*row->bake->coverage), 2048, 1500, 1500), 255)
      << "the coverage write takes the same rectangle";
  EXPECT_EQ(map_byte_at(after, 2048, 60, 60), map_byte_at(before, 2048, 60, 60))
      << "inside the footprint the re-render lays what the first bake laid";
}

TEST_F(PaintLayersOwnedEffectTest, a_structural_change_re_renders_a_heavy_fill_row_whole)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 2048, "HeavyStructural");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_bake_job());
  Image *base_map = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base_map, nullptr);
  write_map_rect(*base_map, rcti{1000, 1010, 1000, 1010}, 255);

  /* Scale 8 keeps the content in `[0, 256)`, whose aligned rect is `[0, 512)`; the sentinel is
   * outside both. Only a whole render reaches it. The row's bake hash folds its mapping values, so
   * the change is a structural one. */
  const float scale[2] = {8.0f, 8.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  uint32_t hash[2];
  hash_get(*ma, *row, hash);
  ASSERT_TRUE(hash[0] != row->bake->hash[0] || hash[1] != row->bake->hash[1])
      << "the test's premise is a structural hash that moved";

  ASSERT_TRUE(run_bake_job());
  const Vector<uchar> after = read_map(*row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_NE(map_byte_at(after, 2048, 1000, 1000), 255)
      << "a hash mismatch renders whole; the old content outside the footprint is not kept";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cropped folded-effect compute
 *
 * A bake render that knows its rectangle offers it to the folded CPU effect: a neighbourhood kind
 * (the Blur) computes only that rectangle grown by its halo and hands the cropped buffer over with
 * its origin, the CPU composite samples it, and nothing of it enters the effect cache. A whole-map
 * kind, a whole-map correction below the effect, a non-region kind and a full render keep the
 * whole-canvas path. The tests render the node directly, which is what the planner and the worker
 * call, and compare the rectangle with the full render of the same row.
 * \{ */

TEST_F(PaintLayersOwnedEffectTest, a_partial_folded_effect_matches_the_full_render_in_its_rect)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 512, "PartialRect");
  ASSERT_NE(row, nullptr);
  /* The reference: the whole row as every full render lays it. */
  const Vector<float> full = render_row_node(*row, 512, nullptr);
  const size_t partial_before = paint_layer_effect_partial_compute_count();

  const int dst[4] = {0, 0, 256, 256};
  const Vector<float> partial = render_row_node(*row, 512, dst);

  EXPECT_EQ(paint_layer_effect_partial_compute_count(), partial_before + 1)
      << "the folded Blur computed the offered rectangle, not the whole canvas";
  ASSERT_EQ(partial.size(), full.size());
  for (const int y : IndexRange(0, 256)) {
    for (const int x : IndexRange(0, 256)) {
      for (const int k : IndexRange(4)) {
        const int64_t i = (int64_t(y) * 512 + x) * 4 + k;
        EXPECT_NEAR(partial[i], full[i], 1e-4f)
            << "the cropped effect's pixel " << x << "," << y << " component " << k
            << " is the full render's";
      }
    }
  }
}

TEST_F(PaintLayersOwnedEffectTest, a_partial_folded_effect_matches_the_full_render_at_the_halo_edge)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  /* Scale 4 over 512: the content ends at 128, and the box Blur radius 2 reaches two texels past
   * it -- the band the crop has to carry but the composite never samples outside the footprint. */
  add_bounded_fill(&row, &map, 4.0f, 512, "PartialHalo");
  ASSERT_NE(row, nullptr);
  const Vector<float> full = render_row_node(*row, 512, nullptr);

  const int dst[4] = {0, 0, 256, 256};
  const Vector<float> partial = render_row_node(*row, 512, dst);
  ASSERT_EQ(partial.size(), full.size());

  /* Just past the content's edge, inside the halo: the cropped kernel's band has to agree with the
   * full one, or a stroke at the edge would leave a seam. */
  for (const int x : {128, 129, 130, 131, 135}) {
    for (const int k : IndexRange(4)) {
      const int64_t i = (int64_t(60) * 512 + x) * 4 + k;
      EXPECT_NEAR(partial[i], full[i], 1e-4f)
          << "the halo texel " << x << ",60 component " << k << " matches the full render";
    }
  }
  for (const int y : {128, 129, 130}) {
    for (const int k : IndexRange(4)) {
      const int64_t i = (int64_t(y) * 512 + 60) * 4 + k;
      EXPECT_NEAR(partial[i], full[i], 1e-4f)
          << "the halo texel 60," << y << " component " << k << " matches the full render";
    }
  }
}

TEST_F(PaintLayersOwnedEffectTest, a_whole_map_effect_below_the_blur_refuses_the_partial_compute)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 512, "WholeMapBelow");
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *match = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_COLOR_MATCH);
  ASSERT_NE(match, nullptr);
  /* Below the Blur: the Blur reads what the match lays, which is a whole-canvas statistic. The
   * effects list is bottom to top; `BKE_paint_layers_reorder` moves stack rows, not effects. */
  BLI_remlink(&row->effects, match);
  BLI_addhead(&row->effects, match);

  const Vector<float> full = render_row_node(*row, 512, nullptr);
  const size_t partial_before = paint_layer_effect_partial_compute_count();
  const int dst[4] = {0, 0, 256, 256};
  const Vector<float> partial = render_row_node(*row, 512, dst);

  EXPECT_EQ(paint_layer_effect_partial_compute_count(), partial_before)
      << "a whole-map correction below the effect makes the input whole-canvas; no crop is made";
  ASSERT_EQ(partial.size(), full.size());
  for (const int y : IndexRange(0, 256)) {
    for (const int x : IndexRange(0, 256)) {
      for (const int k : IndexRange(4)) {
        const int64_t i = (int64_t(y) * 512 + x) * 4 + k;
        EXPECT_NEAR(partial[i], full[i], 1e-4f)
            << "the whole-canvas fallback's pixel " << x << "," << y << " matches the full render";
      }
    }
  }
}

TEST_F(PaintLayersOwnedEffectTest, a_non_region_effect_kind_refuses_the_partial_compute)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  /* A Noise grains by the texel's absolute coordinates, so the region gate refuses it: the full
   * compute alone puts the grain where the full render puts it. */
  add_bounded_fill(&row, &map, 4.0f, 512, "NoisePartial", 8, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_NE(row, nullptr);
  const Vector<float> full = render_row_node(*row, 512, nullptr);
  const size_t partial_before = paint_layer_effect_partial_compute_count();
  const int dst[4] = {0, 0, 256, 256};
  const Vector<float> partial = render_row_node(*row, 512, dst);

  EXPECT_EQ(paint_layer_effect_partial_compute_count(), partial_before)
      << "a kind whose kernel is not a local function of the neighbourhood keeps the full compute";
  ASSERT_EQ(partial.size(), full.size());
  for (const int y : IndexRange(0, 256)) {
    for (const int x : IndexRange(0, 256)) {
      for (const int k : IndexRange(4)) {
        const int64_t i = (int64_t(y) * 512 + x) * 4 + k;
        EXPECT_NEAR(partial[i], full[i], 1e-4f)
            << "the full-compute fallback's pixel " << x << "," << y << " matches the full render";
      }
    }
  }
}

TEST_F(PaintLayersOwnedEffectTest, a_full_render_does_not_compute_a_partial_effect)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 4.0f, 512, "FullNoPartial");
  ASSERT_NE(row, nullptr);
  /* An identical row, laid after the same generated map: its full render is the reference. */
  MaterialPaintLayer *reference = nullptr;
  Image *reference_map = nullptr;
  add_bounded_fill(&reference, &reference_map, 4.0f, 512, "FullNoPartialRef");
  ASSERT_NE(reference, nullptr);

  const size_t partial_before = paint_layer_effect_partial_compute_count();
  const Vector<float> full = render_row_node(*row, 512, nullptr);
  EXPECT_EQ(paint_layer_effect_partial_compute_count(), partial_before)
      << "a render with no rectangle keeps the whole-canvas compute";
  const Vector<float> reference_pixels = render_row_node(*reference, 512, nullptr);

  ASSERT_EQ(full.size(), reference_pixels.size());
  for (const int64_t i : IndexRange(int64_t(full.size()))) {
    EXPECT_NEAR(full[i], reference_pixels[i], 1e-4f) << "whole-render texel " << i;
  }
}

TEST_F(PaintLayersOwnedEffectTest, a_partial_bake_keeps_the_effect_cache_unchanged)
{
  MaterialPaintLayer *row = nullptr;
  Image *map = nullptr;
  add_bounded_fill(&row, &map, 5.0f, 512, "PartialCache");
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(run_plan());
  ASSERT_NE(row->bake, nullptr);
  ASSERT_NE(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);

  edit_map_rect(*map, rcti{0, 8, 0, 8}, 200);
  BKE_paint_layers_bake_notice_changes(*ma);
  const int entries_before = paint_layer_effect_cache_entry_count();
  const size_t partial_before = paint_layer_effect_partial_compute_count();

  ASSERT_TRUE(run_plan());
  EXPECT_GT(paint_layer_effect_partial_compute_count(), partial_before)
      << "the narrowed bake computed the folded Blur over its rectangle";
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), entries_before)
      << "a cropped effect result is never inserted into the effect cache";
}

/**
 * A row stored through the batch path lands as the same bytes the per-pixel writer produces: the
 * colour encoded into the map's colorspace in one OCIO call left the texels identical, the data
 * channel is not encoded at all, and the alpha is clamped and quantized the same way. The in-place
 * form, which the row writer uses, is checked against the copied one too.
 */
TEST_F(PaintLayersMaterialBakeTest, a_batched_byte_row_is_the_per_pixel_encoding)
{
  const ColorSpace *colorspace = IMB_colormanagement_space_get_named(
      IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DEFAULT_BYTE));
  ASSERT_NE(colorspace, nullptr);
  const float row[16] = {0.05f,
                         0.25f,
                         0.5f,
                         1.0f,
                         0.9f,
                         0.5f,
                         0.1f,
                         0.25f,
                         0.0f,
                         1.0f,
                         0.33f,
                         1.6f,
                         0.75f,
                         0.6f,
                         0.2f,
                         0.0f};
  constexpr int count = 4;
  for (const bool is_color : {true, false}) {
    SCOPED_TRACE(is_color ? "a colour channel is encoded" :
                            "a data channel is quantized as it came");
    uchar per_pixel[count * 4];
    uchar batched[count * 4];
    for (int i = 0; i < count; i++) {
      bake_pixel_store_byte(&row[i * 4], is_color, colorspace, &per_pixel[i * 4]);
    }
    float scratch[count * 4];
    bake_pixel_store_byte_row(row, count, is_color, colorspace, scratch, batched);
    for (int i = 0; i < count * 4; i++) {
      EXPECT_EQ(batched[i], per_pixel[i]) << "byte " << i << " of the row";
    }
    float in_place[count * 4];
    std::copy(row, row + count * 4, in_place);
    uchar in_place_bytes[count * 4];
    bake_pixel_store_byte_row(in_place, count, is_color, colorspace, in_place, in_place_bytes);
    for (int i = 0; i < count * 4; i++) {
      EXPECT_EQ(in_place_bytes[i], per_pixel[i]) << "in-place byte " << i << " of the row";
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask effect: a regional grey patch and a regional map write
 *
 * The mask cache patches a region of its grey exactly the way the row's effect cache does, and the
 * bake writes only the band the patch names. These tests drive the two directly, over a mask built
 * from one painted map so that a pixel edit is a rectangle the subscription reports.
 * \{ */

/** A blank \a size-square generated image, the mask base the tests paint into. */
static Image *add_square_image(Main *bmain, const int size, const char *name)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  return BKE_image_add_generated(
      bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
}

TEST_F(PaintLayersOwnedEffectTest, a_mask_grey_patch_matches_the_full_compute)
{
  /* The regional patch and the full compute must land the same texels: the patch re-renders the
   * band grown by the halo, runs the whole kernel over that small buffer and copies the interior
   * into a clone of the cached grey. The mask's map and the row's canvas share the size here, so
   * the subscription's band is the canvas band the patch names. */
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash, 512, 512);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  Image *mask_map = add_square_image(bmain, 512, "MaskGrey");
  ASSERT_NE(mask_map, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));
  MaterialPaintLayer *blur = BKE_paint_layers_mask_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_BLUR, nullptr);
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));

  /* A flat grey the blur keeps flat, so the patch's interior is known: 120 everywhere. */
  const rcti whole{0, 512, 0, 512};
  write_map_rect(*mask_map, whole, 120);
  mark_map_rect(*mask_map, whole);
  BKE_paint_layers_bake_notice_changes(*ma);

  std::shared_ptr<ImBuf> full = paint_layer_mask_effect_buffer_get(
      *ma, *row, *blur, /*describe_only=*/false, nullptr, PAINT_TILE_NUMBER_BASE);
  ASSERT_NE(full, nullptr);

  /* A band the mask's own map changed in: the cache sees it as a region and patches it. */
  const rcti dirty{100, 140, 100, 140};
  edit_map_rect(*mask_map, dirty, 220);
  BKE_paint_layers_bake_notice_changes(*ma);

  rcti valid;
  BLI_rcti_init(&valid, 0, 0, 0, 0);
  std::shared_ptr<ImBuf> patched = paint_layer_mask_effect_buffer_get(*ma,
                                                                      *row,
                                                                      *blur,
                                                                      /*describe_only=*/false,
                                                                      nullptr,
                                                                      PAINT_TILE_NUMBER_BASE,
                                                                      nullptr,
                                                                      &dirty,
                                                                      &valid);
  ASSERT_NE(patched, nullptr);
  EXPECT_FALSE(BLI_rcti_is_empty(&valid)) << "the patch names the band it made correct";

  /* The next full compute from the edited map is the reference the patch has to match. */
  BKE_paint_layers_mask_effect_cache_free_all();
  std::shared_ptr<ImBuf> recomputed = paint_layer_mask_effect_buffer_get(
      *ma, *row, *blur, /*describe_only=*/false, nullptr, PAINT_TILE_NUMBER_BASE);
  ASSERT_NE(recomputed, nullptr);

  ASSERT_EQ(patched->x, recomputed->x);
  ASSERT_EQ(patched->y, recomputed->y);
  for (const int64_t i : IndexRange(int64_t(patched->x) * patched->y)) {
    EXPECT_NEAR(patched->float_data()[i * 4],
                recomputed->float_data()[i * 4],
                1e-5f)
        << "the patch and the full compute disagree at texel " << i;
  }
}

TEST_F(PaintLayersOwnedEffectTest, a_mask_grey_with_an_anchor_slice_refuses_the_patch)
{
  /* A grey that reads an Anchor Point's slice is another row's canvas: a rectangle of this item's
   * maps says nothing about it, and the cache has to answer with the whole canvas. */
  /* A row that reads a material takes no channel record of its own: #BKE_paint_layers_channel_add
   * refuses one a kind never reads, so the Normal it lays has to come from the source's graph. A
   * Bump on the Principled makes the row take part in the channel, and the render's own Normal map
   * below is the one the point reads. */
  bump_normal_link(*source);
  MaterialPaintLayer *source_row = add_material_row(*source);
  ASSERT_NE(source_row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*source_row,
               Span<const int>(base_color_roughness_normal, 3),
               hash,
               source_hash,
               512,
               512);
  ASSERT_TRUE(
      BKE_paint_layers_material_bake_land(*ma, source_row->marker, hash, source_hash, true));
  ASSERT_NE(source_row->bake, nullptr);
  Image *normal_map = source_row->bake->images[PAINT_MATERIAL_CHANNEL_NORMAL];
  ASSERT_NE(normal_map, nullptr);

  MaterialPaintLayer *owner = add_material_row(*source);
  ASSERT_NE(owner, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, owner, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_normal_to_mask_add(*ma, owner, source_row);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(paint_layer_kind_reads_anchor_point(*item));

  std::shared_ptr<ImBuf> grey = paint_layer_mask_effect_buffer_get(
      *ma, *owner, *item, /*describe_only=*/false, nullptr, PAINT_TILE_NUMBER_BASE);
  ASSERT_NE(grey, nullptr);

  /* An edit of the referenced normal is a rectangle in its own grid; the grey is this canvas's, and
   * no band of it can be named from that rectangle. */
  const rcti dirty{100, 140, 100, 140};
  edit_map_rect(*normal_map, dirty, 220);
  BKE_paint_layers_bake_notice_changes(*ma);

  rcti valid;
  BLI_rcti_init(&valid, 0, 0, 0, 0);
  std::shared_ptr<ImBuf> answer = paint_layer_mask_effect_buffer_get(*ma,
                                                                     *owner,
                                                                     *item,
                                                                     /*describe_only=*/false,
                                                                     nullptr,
                                                                     PAINT_TILE_NUMBER_BASE,
                                                                     nullptr,
                                                                     &dirty,
                                                                     &valid);
  ASSERT_NE(answer, nullptr);
  EXPECT_TRUE(valid.xmin == 0 && valid.ymin == 0 && valid.xmax >= answer->x &&
              valid.ymax >= answer->y)
      << "an anchor-slice grey cannot narrow the written band";
}

TEST_F(PaintLayersOwnedEffectTest, a_regional_mask_write_leaves_the_rest_of_the_grey_map)
{
  /* The mirror of the owned effect's regional write, for a mask item: a band of the mask's map
   * changes, the plan resolves that band and the grey map outside it keeps its pixels. */
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash, 512, 512);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  Image *mask_map = add_square_image(bmain, 512, "MaskWrite");
  ASSERT_NE(mask_map, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));
  MaterialPaintLayer *item = BKE_paint_layers_mask_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_BLUR, nullptr);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*item, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*item, PAINT_LAYER_BLUR_RADIUS, 1));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *item, MA_PAINT_LAYER_BAKE_ALWAYS));

  const rcti whole{0, 512, 0, 512};
  write_map_rect(*mask_map, whole, 120);
  mark_map_rect(*mask_map, whole);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_plan());
  Image *grey_map = item->bake != nullptr ? item->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] :
                                            nullptr;
  ASSERT_NE(grey_map, nullptr);
  int flat_value = 0;
  {
    const Vector<uchar> flat = read_map(*grey_map);
    ASSERT_FALSE(flat.is_empty());
    flat_value = int(map_byte_at(flat, 512, 256, 256));
    EXPECT_GT(flat_value, 0) << "the flat mask laid a grey";
  }
  /* A canary far from the edit: a regional write leaves it, a whole one overwrites it. */
  const int canary_x = 511;
  const int canary_y = 511;
  write_map_rect(*grey_map, rcti{canary_x, canary_x + 1, canary_y, canary_y + 1}, 17);

  const rcti dirty{5, 7, 3, 5};
  edit_map_rect(*mask_map, dirty, 220);
  BKE_paint_layers_bake_notice_changes(*ma);
  rcti resolved;
  ASSERT_NE(bake_mask_effect_region_resolve(*ma, *item, resolved), nullptr)
      << "the edit reached the item's subscription as a rectangle";

  ASSERT_TRUE(run_plan());
  const Vector<uchar> regional = read_map(*grey_map);
  ASSERT_FALSE(regional.is_empty());
  EXPECT_EQ(map_byte_at(regional, 512, canary_x, canary_y), 17)
      << "the plan wrote the band only, as the canary expects";
  const int band_value = int(map_byte_at(regional,
                                         512,
                                         (dirty.xmin + dirty.xmax) / 2,
                                         (dirty.ymin + dirty.ymax) / 2));
  EXPECT_GT(std::abs(band_value - flat_value), 20)
      << "the edit's band was laid, the flat grey of it replaced";
}

TEST_F(PaintLayersOwnedEffectTest, a_mask_item_with_a_transform_is_written_whole)
{
  /* A transform item moves the grey instead of correcting it: no rectangle of the input names a
   * band of the result, so the plan refuses the region and writes whole. */
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash, 512, 512);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  Image *mask_map = add_square_image(bmain, 512, "MaskTransform");
  ASSERT_NE(mask_map, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));
  MaterialPaintLayer *item = BKE_paint_layers_mask_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_TRANSFORM, nullptr);
  ASSERT_NE(item, nullptr);
  const float scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, item, scale));
  ASSERT_TRUE(paint_layer_mask_item_is_transform(*item));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *item, MA_PAINT_LAYER_BAKE_ALWAYS));

  const rcti whole{0, 512, 0, 512};
  write_map_rect(*mask_map, whole, 120);
  mark_map_rect(*mask_map, whole);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_plan());
  Image *grey_map = item->bake != nullptr ? item->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] :
                                            nullptr;
  ASSERT_NE(grey_map, nullptr);

  const int canary_x = 511;
  const int canary_y = 511;
  write_map_rect(*grey_map, rcti{canary_x, canary_x + 1, canary_y, canary_y + 1}, 17);

  const rcti dirty{5, 7, 3, 5};
  edit_map_rect(*mask_map, dirty, 220);
  BKE_paint_layers_bake_notice_changes(*ma);
  rcti resolved;
  EXPECT_EQ(bake_mask_effect_region_resolve(*ma, *item, resolved), nullptr)
      << "a transform item has no band in the canvas to write";

  ASSERT_TRUE(run_plan());
  const Vector<uchar> whole_map = read_map(*grey_map);
  ASSERT_FALSE(whole_map.is_empty());
  EXPECT_NE(map_byte_at(whole_map, 512, canary_x, canary_y), 17)
      << "the whole write recomputed the canary";
}

TEST_F(PaintLayersOwnedEffectTest, a_mask_region_without_a_changed_band_keeps_the_canary)
{
  /* Without a band the resolve answers null and nothing is due: the plan leaves the map alone, the
   * canary included. A whole write would have to be asked for by a reason of its own. */
  MaterialPaintLayer *row = add_material_row(*source);
  ASSERT_NE(row, nullptr);
  uint32_t hash[2], source_hash[2];
  start_render(*row, Span<const int>(base_color_roughness, 2), hash, source_hash, 512, 512);
  ASSERT_TRUE(BKE_paint_layers_material_bake_land(*ma, row->marker, hash, source_hash, true));

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  Image *mask_map = add_square_image(bmain, 512, "MaskNoBand");
  ASSERT_NE(mask_map, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));
  MaterialPaintLayer *item = BKE_paint_layers_mask_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_BLUR, nullptr);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *item, MA_PAINT_LAYER_BAKE_ALWAYS));

  const rcti whole{0, 512, 0, 512};
  write_map_rect(*mask_map, whole, 120);
  mark_map_rect(*mask_map, whole);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(run_plan());
  Image *grey_map = item->bake != nullptr ? item->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] :
                                            nullptr;
  ASSERT_NE(grey_map, nullptr);

  const int canary_x = 511;
  const int canary_y = 511;
  write_map_rect(*grey_map, rcti{canary_x, canary_x + 1, canary_y, canary_y + 1}, 17);

  rcti resolved;
  EXPECT_EQ(bake_mask_effect_region_resolve(*ma, *item, resolved), nullptr)
      << "nothing changed, so there is no band to name";
  EXPECT_FALSE(run_plan()) << "the item is not due";
  const Vector<uchar> kept = read_map(*grey_map);
  ASSERT_FALSE(kept.is_empty());
  EXPECT_EQ(map_byte_at(kept, 512, canary_x, canary_y), 17);
}

/** \} */

TEST_F(PaintLayersMaterialBakeTest, a_nested_projection_on_a_cpu_source_row_moves_the_narrowed_key)
{
  /* The narrowed reader's key is the referenced row's whole-row hash. That row is not a child of an
   * outer bake, so it has to ask #paint_layer_subtree_has_cpu_effect itself: only then is the
   * projection of a nested node folded into the key. Without that a change to a projection inside
   * the row would leave the reader's maps as they were. The projection sits on a mask item and the
   * CPU effect on the row that owns it, so neither #paint_layer_row_folds_into_bake (the row) nor
   * the item alone can fold it -- only the row's own CPU subtree can. */
  Material *layered = BKE_material_add(bmain, "Layered Source");
  principled_source_set_up(*layered, /*graphs=*/false);
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *layered, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_NE(BKE_paint_layers_filter_add(*layered, paint, MA_PAINT_LAYER_FILTER_BLUR), nullptr);
  Image *mask_map = add_image("Mask Map");
  MaterialPaintLayer *item = BKE_paint_layers_mask_add_image(*layered, *paint, *mask_map);
  ASSERT_NE(item, nullptr);
  ASSERT_EQ(item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *layered, item, MA_PAINT_LAYER_MAPPING_SPACE_BOX));
  ASSERT_NE(item->projection, nullptr);
  ASSERT_TRUE(paint_layer_subtree_has_cpu_effect(*paint));
  ASSERT_FALSE(paint_layer_row_folds_into_bake(*layered, *item))
      << "the item does not fold through its own effect; only the row's CPU subtree folds it";

  const uint64_t before = BKE_paint_layers_bake_hash_row_whole(*layered, *paint);
  /* A projection parameter moves: the reader renders the row whole, so its key must follow. */
  item->projection->size[0] += 0.25f;
  const uint64_t after = BKE_paint_layers_bake_hash_row_whole(*layered, *paint);
  EXPECT_NE(before, after) << "a nested projection inside a CPU row is part of what the reader shows";
}

TEST_F(PaintLayersMaterialBakeTest, a_narrowed_live_row_over_the_ceiling_is_refused)
{
  /* A narrowed row has no canvas in Live, but its synchronous render uses the same buffers, so the
   * ceiling covers it too: the honest state is "Source render too large", not a quietly clamped
   * render. Before the narrowed Live mode existed this path could not be reached. */
  Material *layered = BKE_material_add(bmain, "Layered Source");
  principled_source_set_up(*layered, /*graphs=*/false);
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *layered, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);

  MaterialPaintLayer *reader = add_material_row(*layered);
  ASSERT_NE(reader, nullptr);
  reader->material_scope = MA_PAINT_LAYER_MATERIAL_SCOPE_ROW;
  reader->material_row_marker = paint->marker;
  reader->material_canvas_mode = MA_PAINT_LAYER_CANVAS_MODE_LIVE;
  ASSERT_FALSE(BKE_paint_layers_material_row_is_canvas(*reader));

  /* A Live narrowed row's side is the bake's, or the owning material's layer size before it has
   * one. The reader belongs to the fixture's material (#add_material_row). */
  ma->paint_layers_image_size = BKE_paint_layers_material_canvas_max_side() + 1;
  EXPECT_TRUE(BKE_paint_layers_material_canvas_refused(*ma, *reader));
  EXPECT_EQ(BKE_paint_layers_material_source_status(*ma, *reader),
            MA_PAINT_LAYER_SOURCE_STATUS_TOO_LARGE);
}

/** \} */

TEST_F(PaintLayersMaterialBakeTest, a_directx_normal_row_is_rendered_not_read_as_a_plain_image)
{
  /* The DirectX convention lives on the row's channel record, and a Material row that reads the
   * source has no record of its own. The flag must therefore be applied inside the source's own
   * render: the generated chain puts the green flip between the row's map and the Normal Map's
   * Color, so the channel resolves as Baked and never as Image. A reader shows the render (which
   * carries the flip), not the encoded map sampled raw. The test locks the invariant down: a
   * generator that handed an Image straight to the Normal Map would silently lose the flag for
   * every Material row reading this source, and nothing on the reader side could recover it. */
  Material *layered = BKE_material_add(bmain, "Layered Source");
  principled_source_set_up(*layered, /*graphs=*/false);
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *layered, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  Image *normal_map = add_image("Normal Map");
  ASSERT_NE(BKE_paint_layers_channel_add(*layered, paint, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *layered, paint, PAINT_MATERIAL_CHANNEL_NORMAL, normal_map));
  ASSERT_TRUE(BKE_paint_layers_normal_set_directx(*layered, paint, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *layered));

  const std::shared_ptr<const MaterialSourceResolve> resolve =
      BKE_paint_layers_source_resolve(*layered);
  ASSERT_TRUE(resolve != nullptr);
  EXPECT_EQ(resolve->channels[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelResolution::Baked)
      << "the flip is a node in the chain, so the Normal channel is a render, not the map";
  EXPECT_EQ(resolve->reasons[PAINT_MATERIAL_CHANNEL_NORMAL], ChannelUnavailableReason::None);

  /* The reader cannot sample that map as a plain image: it would read DirectX data as OpenGL. */
  MaterialPaintLayer *reader = add_material_row(*layered);
  ASSERT_NE(reader, nullptr);
  const PaintLayerMaterialSource source = paint_layer_material_channel_source(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_NE(source.kind, PaintLayerMaterialSourceKind::LiveImage)
      << "an encoded map with a convention the reader cannot know must never be read raw";
}

}  // namespace blender::bke::tests
