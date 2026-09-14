/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_paint_material_layer_edit.hh"

#include "ED_material_bake.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <cmath>

namespace blender::ed::material_bake::tests {

/**
 * The "Use layer result" bake, end to end: a blocking bake of a material with a paint layer stack,
 * with and without a #BakeSourceOverride.
 *
 * Every test here runs a real EEVEE preview render, which needs a runnable EEVEE (a GPU context
 * and a display); the suite is meant for an operator's machine, not CI. Nothing is mocked.
 */
class MaterialBakeTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *material = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    /* The bake's endjob looks the source material and its targets up in #G_MAIN. */
    G_MAIN = bmain;
    material = add_material_with_principled("Material");
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
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

  /** Add a Fill layer filled with \a color on top of the stack, and return its row marker. */
  bUUID add_fill_layer(const float color[4], const char *name)
  {
    PaintMaterialLayerAddParams params;
    params.image_size = 8;
    params.kind = PaintMaterialLayerKind::Fill;
    copy_v4_v4(params.fill_color, color);
    params.name = name;
    int ordinal = -1;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    EXPECT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
        << int(error);
    Vector<PaintMaterialLayerStackEntry> entries;
    EXPECT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, *material, entries));
    return entries[ordinal].marker;
  }

  /** Blocking bake of \a targets at the test size, with no window manager. */
  MaterialBakeToImagesResult bake(const Span<BakeTargetSpec> targets)
  {
    MaterialBakeToImagesParams params;
    params.material = material;
    params.targets = targets;
    params.size = 64;
    params.blocking = true;
    return material_bake_to_images(*bmain, nullptr, nullptr, params);
  }

  /** Every texel of \a image equals \a color, alpha included, within the fill tolerance. */
  static void expect_image_is_solid_color(Image &image, const float color[4])
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    int64_t mismatches = 0;
    for (const int64_t texel : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      for (const int component : IndexRange(4)) {
        if (std::abs(ibuf->float_data()[texel * 4 + component] - color[component]) > 1e-3f) {
          mismatches++;
        }
      }
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
    EXPECT_EQ(mismatches, 0) << "expected solid (" << color[0] << ", " << color[1] << ", "
                             << color[2] << ", " << color[3] << ")";
  }
};

/* Fill colors are pure primaries, which read the same in scene linear as in the target image's
 * sRGB, so the assertions below are unaffected by the write-back's colorspace conversion. */

TEST_F(MaterialBakeTest, bake_without_override_still_bakes_whole_channel)
{
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_fill_layer(blue, "Base");
  const int images_before = BLI_listbase_count(&bmain->images);

  BakeTargetSpec spec;
  spec.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  MaterialBakeToImagesResult result = bake(Span<BakeTargetSpec>(&spec, 1));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.created.size(), 1);
  EXPECT_EQ(result.created_channels.size(), 1);
  EXPECT_EQ(result.created_channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_TRUE(result.skipped_unavailable.is_empty());
  EXPECT_TRUE(result.failed_overrides.is_empty());
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before + 1);
}

TEST_F(MaterialBakeTest, bake_with_source_override_bakes_layer_row_not_whole_material)
{
  /* Blue bottom, red "Row" on top of it, green topmost: the whole material's Base Color composites
   * to green, while the row named by the override renders red. A bake that ignored the override
   * would therefore come out green, which is what makes the assertion below able to fail. */
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  add_fill_layer(blue, "Base");
  const bUUID row_marker = add_fill_layer(red, "Row");
  ASSERT_FALSE(BLI_uuid_is_nil(row_marker));
  add_fill_layer(green, "Top");

  BakeSourceOverride override;
  override.layer_marker = row_marker;
  override.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  override.endpoint = BakeSourceOverride::Endpoint::LayerContentWithMask;

  BakeTargetSpec spec;
  spec.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  spec.source_override = override;
  MaterialBakeToImagesResult result = bake(Span<BakeTargetSpec>(&spec, 1));

  EXPECT_TRUE(result.ok);
  ASSERT_EQ(result.created.size(), 1);
  EXPECT_EQ(result.created_channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_TRUE(result.failed_overrides.is_empty());
  /* The row's own color, and its coverage as the alpha: the map holds what the row paints. */
  expect_image_is_solid_color(*result.created[0], red);
}

TEST_F(MaterialBakeTest, bake_with_source_override_missing_marker_fails_without_creating_orphan_image)
{
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  add_fill_layer(red, "Base");
  const int images_before = BLI_listbase_count(&bmain->images);

  BakeSourceOverride override;
  /* No row carries this marker, so neither the preflight nor the worker can resolve it. */
  override.layer_marker = BLI_uuid_generate_random();
  override.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  override.endpoint = BakeSourceOverride::Endpoint::LayerContentWithMask;

  BakeTargetSpec spec;
  spec.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  spec.source_override = override;
  MaterialBakeToImagesResult result = bake(Span<BakeTargetSpec>(&spec, 1));

  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.created.is_empty());
  EXPECT_TRUE(result.created_channels.is_empty());
  EXPECT_TRUE(result.skipped_unavailable.contains(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  /* The preflight resolved on the original material before any image was created, so none was. */
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
}

}  // namespace blender::ed::material_bake::tests
