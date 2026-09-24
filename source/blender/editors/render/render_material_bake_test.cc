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
#include "BKE_node_legacy_types.hh"
#include "BKE_paint_layers.hh"

#include "ED_material_bake.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_colormanagement.hh"

#include <cmath>

namespace blender::ed::material_bake::tests {

/**
 * The material-to-images bake of a plain Principled material: a constant channel is filled
 * synchronously, with no render.
 */
class MaterialBakeTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *material = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
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

  /** A square scene-linear buffer touching every RGB texel with the same value. */
  static ImBuf *make_rendered_buffer(const int size, const float r, const float g, const float b)
  {
    ImBuf *rendered = IMB_allocImBuf(size, size, ImBufFlags::Zero);
    EXPECT_NE(rendered, nullptr);
    if (rendered == nullptr) {
      return nullptr;
    }
    rendered->channels = 4;
    EXPECT_TRUE(IMB_alloc_float_pixels(rendered, 4, false));
    float *dst = rendered->float_data_for_write();
    for (const int64_t texel : IndexRange(int64_t(size) * size)) {
      dst[texel * 4 + 0] = r;
      dst[texel * 4 + 1] = g;
      dst[texel * 4 + 2] = b;
      dst[texel * 4 + 3] = 1.0f;
    }
    return rendered;
  }
};

TEST_F(MaterialBakeTest, constant_channel_is_filled_without_a_render)
{
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  bNode *principled = nullptr;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
    }
  }
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, blue);

  const int images_before = BLI_listbase_count(&bmain->images);

  BakeTargetSpec spec;
  spec.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = Span<BakeTargetSpec>(&spec, 1);
  params.size = 64;
  params.blocking = true;
  MaterialBakeToImagesResult result = material_bake_to_images(*bmain, nullptr, nullptr, params);

  EXPECT_TRUE(result.ok);
  ASSERT_EQ(result.created.size(), 1);
  ASSERT_EQ(result.created_channels.size(), 1);
  EXPECT_EQ(result.created_channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_TRUE(result.skipped_unavailable.is_empty());
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before + 1);
  expect_image_is_solid_color(*result.created[0], blue);
}

/**
 * A source read live by an active Material row is not re-baked by the automatic stale pass. The
 * fixture has no window manager, so the live-source guard (which sits before the wm check) is the
 * one that returns; the predicate's truth table is covered in PaintLayersDescription.
 */
TEST_F(MaterialBakeTest, rebake_stale_skips_a_live_source)
{
  Material *layered = BKE_material_add(bmain, "LayeredLive");
  Material *source = add_material_with_principled("LiveSourceBake");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*layered, row, source));
  BKE_paint_layers_active_set(*layered, row->marker);
  ASSERT_TRUE(BKE_paint_layers_source_material_is_live(*bmain, *source));

  material_bake_images_rebake_stale(*bmain, *source);
  SUCCEED();
}

/**
 * A freshly created color target, reloaded from its own colorspace (what the texture cache does
 * between the bake job starting and its completion callback) and then written with a scene-linear
 * render, must hold those scene-linear values.
 *
 * The shader path uploads a float buffer raw, so a float map is expected to be scene linear or
 * data whatever its channel (see FloatBufferCache's assertion). Declaring a color map sRGB and
 * encoding on write makes the shader read the encoded, too-light values as if they were linear.
 */
TEST_F(MaterialBakeTest, baked_color_map_stores_scene_linear_values)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *image = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, "test-layer", hash);
  ASSERT_NE(image, nullptr);

  /* Reload the pixels from the image's declared colorspace, as a cache eviction does. */
  BKE_image_free_buffers(image);

  ImBuf *rendered = make_rendered_buffer(8, 0.2f, 0.5f, 0.8f);
  ASSERT_NE(rendered, nullptr);
  bake_target_image_write_back(*image, *rendered);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  const char *float_cs = IMB_colormanagement_get_float_colorspace(ibuf);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_scene_linear(float_cs) ||
              IMB_colormanagement_space_name_is_data(float_cs))
      << "float buffer carries colorspace '" << float_cs << "'";
  EXPECT_NEAR(ibuf->float_data()[0], 0.2f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[1], 0.5f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[2], 0.8f, 1e-3f);
  BKE_image_release_ibuf(image, ibuf, lock);
  IMB_freeImBuf(rendered);
}

/** A data target stores its scalar unchanged, and stays in a data colorspace. */
TEST_F(MaterialBakeTest, baked_data_map_stores_the_value_unchanged)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *image = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 8, "test-layer", hash);
  ASSERT_NE(image, nullptr);

  BKE_image_free_buffers(image);

  ImBuf *rendered = make_rendered_buffer(8, 0.3f, 0.3f, 0.3f);
  ASSERT_NE(rendered, nullptr);
  bake_target_image_write_back(*image, *rendered);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(
      IMB_colormanagement_get_float_colorspace(ibuf)));
  EXPECT_NEAR(ibuf->float_data()[0], 0.3f, 1e-3f);
  BKE_image_release_ibuf(image, ibuf, lock);
  IMB_freeImBuf(rendered);
}

/**
 * Re-baking into the maps a row already owns must reuse the same #Image IDs and create none: an
 * earlier version always minted a fresh `.00N` set, which leaked tens of megabytes per re-bake.
 */
TEST_F(MaterialBakeTest, rebake_reuses_the_rows_existing_maps)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *color = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, "test-layer", hash);
  Image *rough = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 8, "test-layer", hash);
  ASSERT_NE(color, nullptr);
  ASSERT_NE(rough, nullptr);

  const int images_before = BLI_listbase_count(&bmain->images);
  BakeTargetSpec specs[2] = {
      {PAINT_MATERIAL_CHANNEL_BASE_COLOR, color},
      {PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough},
  };
  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = Span<BakeTargetSpec>(specs, 2);
  params.size = 8;
  params.blocking = true;
  MaterialBakeToImagesResult result = material_bake_to_images(*bmain, nullptr, nullptr, params);

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
  ASSERT_EQ(result.created.size(), 2);
  EXPECT_EQ(result.created[0], color);
  EXPECT_EQ(result.created[1], rough);
}

/**
 * A map baked before the scene-linear fix carries an sRGB tag and sRGB-encoded floats. Reusing it
 * must convert the pixels back to scene linear, not merely relabel the buffer.
 */
TEST_F(MaterialBakeTest, legacy_srgb_color_map_is_normalized)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *image = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, "test-layer", hash);
  ASSERT_NE(image, nullptr);

  const ColorSpace *srgb = IMB_colormanagement_space_get_named("sRGB");
  ASSERT_NE(srgb, nullptr);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    float *dst = ibuf->float_data_for_write();
    for (const int64_t texel : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      dst[texel * 4 + 0] = 0.2f;
      dst[texel * 4 + 1] = 0.5f;
      dst[texel * 4 + 2] = 0.8f;
      dst[texel * 4 + 3] = 1.0f;
    }
    /* Encode as the old write-back did, and declare the buffer sRGB. */
    IMB_colormanagement_scene_linear_to_colorspace(dst, ibuf->x, ibuf->y, 4, srgb);
    IMB_colormanagement_assign_float_colorspace(ibuf, "sRGB");
    BKE_image_release_ibuf(image, ibuf, lock);
  }
  STRNCPY(image->colorspace_settings.name, "sRGB");

  bake_target_image_normalize_colorspace(*image, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_scene_linear(
      IMB_colormanagement_get_float_colorspace(ibuf)));
  EXPECT_NEAR(ibuf->float_data()[0], 0.2f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[1], 0.5f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[2], 0.8f, 1e-3f);
  BKE_image_release_ibuf(image, ibuf, lock);
}

}  // namespace blender::ed::material_bake::tests
