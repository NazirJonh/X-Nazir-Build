/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Mesh Map rows, content corrections and mask items seen through the CPU composite (the Image
 * Editor's 2D preview): the atlas is the row's colour, resampled with Extend and no Mapping, its
 * R spread across the RGB when the map is scalar, and its alpha data rather than coverage. The
 * structural rules live in `paint_layers_description_test.cc`; this file compares pixels.
 */

#include "testing/testing.h"

#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_material_composite.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers_test_util.hh"

namespace blender::bke::tests {

namespace {

/** A material of its own, layered, for a test that needs more than one stack. */
Material *add_layered_material(Main &bmain, const char *name)
{
  Material *ma = BKE_material_add(&bmain, name);
  if (ma != nullptr) {
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
  }
  return ma;
}

/** A solid `size` square Non-Color byte map, so the composite reads raw bytes over 255. */
Image *add_byte_map(Main &bmain,
                    const char *name,
                    const int size,
                    const uchar r,
                    const uchar g,
                    const uchar b,
                    const uchar a = 255)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      &bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  if (image != nullptr) {
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    if (ibuf != nullptr) {
      uchar *pixels = ibuf->byte_data_for_write();
      for (const int64_t i : IndexRange(int64_t(size) * size)) {
        pixels[i * 4 + 0] = r;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = a;
      }
      BKE_image_release_ibuf(image, ibuf, lock);
    }
  }
  return image;
}

/** A `w` x `h` Non-Color float atlas filled by \a fill (row-major, bottom to top). */
Image *add_float_atlas(Main &bmain,
                       const char *name,
                       const int w,
                       const int h,
                       const FunctionRef<void(float *p, int x, int y)> fill)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      &bmain, w, h, name, 32, true, IMA_GENTYPE_BLANK, black, false, true, false);
  if (image != nullptr) {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    if (ibuf != nullptr) {
      if (ibuf->float_data() != nullptr) {
        float *pixels = ibuf->float_data_for_write();
        for (const int y : IndexRange(h)) {
          for (const int x : IndexRange(w)) {
            fill(pixels + (int64_t(y) * w + x) * 4, x, y);
          }
        }
      }
      BKE_image_release_ibuf(image, ibuf, lock);
    }
  }
  return image;
}

/** A Paint row backed by \a image on its enabled Base Color channel, added on top. */
MaterialPaintLayer *add_paint_row(Material &ma, const char *name, Image *image)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(ma, name, nullptr, PaintLayerPlace::Above);
  if (layer != nullptr) {
    if (BKE_paint_layers_channel_add(ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) != nullptr) {
      EXPECT_TRUE(
          BKE_paint_layers_channel_set_image(ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
    }
  }
  return layer;
}

/** A MESH_MAP row of the stack, reading \a atlas (which may be null: no slot image) for \a type. */
MaterialPaintLayer *add_mesh_map_row(Material &ma,
                                     const char *name,
                                     const eMaterialMeshMapType type,
                                     Image *atlas = nullptr)
{
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MeshMap;
  params.name = name;
  MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(ma, params);
  if (row != nullptr) {
    EXPECT_TRUE(BKE_paint_layers_mesh_map_type_set(ma, row, int8_t(type)));
    if (atlas != nullptr) {
      EXPECT_TRUE(BKE_mesh_maps_slot_image_set(ma, int8_t(type), atlas));
    }
  }
  return row;
}

/**
 * A Mesh Map content correction of \a owner, or a Mesh Map mask item over it: the Paint kind the
 * Add APIs make, flipped to the Mesh Map source the kind table allows for every role. The record
 * carries no image; the atlas is the material's slot for \a type.
 */
MaterialPaintLayer *add_mesh_map_correction(Material &ma,
                                            MaterialPaintLayer &owner,
                                            const char *name,
                                            const eMaterialMeshMapType type,
                                            Image *atlas)
{
  MaterialPaintLayer *item = BKE_paint_layers_correction_add(ma, &owner, name);
  if (item != nullptr) {
    item->source = MA_PAINT_LAYER_SOURCE_MESH_MAP;
    EXPECT_TRUE(BKE_paint_layers_mesh_map_type_set(ma, item, int8_t(type)));
    EXPECT_TRUE(BKE_mesh_maps_slot_image_set(ma, int8_t(type), atlas));
  }
  return item;
}

/** The mask item twin of #add_mesh_map_correction; the row has to hold a base mask already. */
MaterialPaintLayer *add_mesh_map_mask_item(Material &ma,
                                           MaterialPaintLayer &owner,
                                           const char *name,
                                           const eMaterialMeshMapType type,
                                           Image *atlas)
{
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      ma, &owner, MA_PAINT_LAYER_SOURCE_IMAGE, name);
  if (item != nullptr) {
    item->source = MA_PAINT_LAYER_SOURCE_MESH_MAP;
    EXPECT_TRUE(BKE_paint_layers_mesh_map_type_set(ma, item, int8_t(type)));
    EXPECT_TRUE(BKE_mesh_maps_slot_image_set(ma, int8_t(type), atlas));
  }
  return item;
}

/** The linear composite of \a material's \a channel, or false when it cannot be described. */
bool render_channel(Material &material,
                    const int channel,
                    Vector<float> &r_pixels,
                    int &r_width,
                    int &r_height)
{
  Vector<PaintMaterialCompositeImageLayer> layers;
  if (!BKE_paint_layers_composite_image_layers(material, channel, layers)) {
    return false;
  }
  if (!BKE_paint_material_composite_stack_dimensions(layers, r_width, r_height) || r_width <= 0 ||
      r_height <= 0)
  {
    return false;
  }
  r_pixels.resize(int64_t(r_width) * r_height * 4);
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom);
  return BKE_paint_material_composite_eval_images_linear(
      layers,
      PaintMaterialCompositeLinearTarget::frame(r_pixels, r_width),
      nullptr,
      nullptr,
      bottom);
}

const float *linear_pixel(const Vector<float> &pixels, const int width, const int x, const int y)
{
  return pixels.data() + (int64_t(y) * width + x) * 4;
}

/** The Extend bilinear the MESH_MAP resample is specified to do, of a 2x2 atlas onto 4x4. */
float reference_resample(const float values[2][2], const int x, const int y)
{
  const float su = (float(x) + 0.5f) / 4.0f * 2.0f - 0.5f;
  const float sv = (float(y) + 0.5f) / 4.0f * 2.0f - 0.5f;
  const int x1 = int(floorf(su));
  const int y1 = int(floorf(sv));
  const float a = su - float(x1);
  const float b = sv - float(y1);
  auto clamp_texel = [](const int v) { return min_ii(max_ii(v, 0), 1); };
  const float p11 = values[clamp_texel(y1)][clamp_texel(x1)];
  const float p21 = values[clamp_texel(y1)][clamp_texel(x1 + 1)];
  const float p12 = values[clamp_texel(y1 + 1)][clamp_texel(x1)];
  const float p22 = values[clamp_texel(y1 + 1)][clamp_texel(x1 + 1)];
  return (1.0f - a) * (1.0f - b) * p11 + a * (1.0f - b) * p21 + (1.0f - a) * b * p12 +
         a * b * p22;
}

}  // namespace

/** A Mesh Map row lays its atlas: a scalar atlas' R spread across the RGB, an RGB atlas passed
 * through; the atlas alpha is data, so the row covers fully and reads back opaque. */
TEST_F(PaintLayersTestBase, mesh_map_row_lays_the_atlas_as_its_colour)
{
  const int ref = 4;
  /* The Paint map below defines the 4x4 reference grid. */
  add_paint_row(*ma, "Bottom", add_byte_map(*bmain, "BottomMap", ref, 200, 40, 10));

  /* A 4x4 atlas at the reference grid: every texel centre lands exactly on its atlas texel, so
   * the expected colour is the atlas itself. */
  Image *ao_atlas = add_float_atlas(
      *bmain, "AOAtlas", ref, ref, [](float *p, const int x, const int y) {
        p[0] = 0.05f + 0.2f * x + 0.05f * y;
        /* G, B and a zero alpha must be ignored for a scalar map. */
        p[1] = 0.9f;
        p[2] = 0.2f;
        p[3] = 0.0f;
      });
  ASSERT_NE(ao_atlas, nullptr);
  ASSERT_NE(add_mesh_map_row(*ma, "AO", MA_MESH_MAP_AO, ao_atlas), nullptr);

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  ASSERT_EQ(width, ref);
  for (const int y : IndexRange(ref)) {
    for (const int x : IndexRange(ref)) {
      const float *got = linear_pixel(pixels, width, x, y);
      const float red = 0.05f + 0.2f * x + 0.05f * y;
      EXPECT_NEAR(got[0], red, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[1], red, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[2], red, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[3], 1.0f, 1e-4f) << "texel " << x << "," << y;
    }
  }

  /* An RGB map passes its stored RGB through, spread to nothing. */
  Material *rgb_material = add_layered_material(*bmain, "MeshMapRGB");
  ASSERT_NE(rgb_material, nullptr);
  add_paint_row(*rgb_material, "Bottom", add_byte_map(*bmain, "RGBBottomMap", ref, 10, 200, 40));
  Image *normal_atlas = add_float_atlas(
      *bmain, "NormalAtlas", ref, ref, [](float *p, const int x, const int y) {
        p[0] = 0.05f + 0.2f * x;
        p[1] = 0.05f + 0.2f * y;
        p[2] = 0.5f;
        p[3] = 0.0f;
      });
  ASSERT_NE(normal_atlas, nullptr);
  ASSERT_NE(add_mesh_map_row(*rgb_material, "Normal", MA_MESH_MAP_NORMAL_WORLD, normal_atlas),
            nullptr);

  ASSERT_TRUE(
      render_channel(*rgb_material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  ASSERT_EQ(width, ref);
  for (const int y : IndexRange(ref)) {
    for (const int x : IndexRange(ref)) {
      const float *got = linear_pixel(pixels, width, x, y);
      EXPECT_NEAR(got[0], 0.05f + 0.2f * x, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[1], 0.05f + 0.2f * y, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[2], 0.5f, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[3], 1.0f, 1e-4f) << "texel " << x << "," << y;
    }
  }
}

/** A differently sized atlas is resampled onto the reference grid the way the generator's Image
 * Texture reads it, Extend-clamped, so the edges and corners agree instead of wrapping. */
TEST_F(PaintLayersTestBase, mesh_map_row_resamples_a_smaller_atlas_with_extend)
{
  const int ref = 4;
  add_paint_row(*ma, "Bottom", add_byte_map(*bmain, "BottomMap", ref, 200, 40, 10));

  /* A 2x2 scalar atlas: the outer reference texels read past the atlas edge and exercise the
   * Extend clamp. */
  const float values[2][2] = {{0.1f, 0.3f}, {0.5f, 0.7f}};
  Image *atlas = add_float_atlas(*bmain, "Atlas", 2, 2, [&](float *p, const int x, const int y) {
    p[0] = values[y][x];
    p[1] = 0.0f;
    p[2] = 0.0f;
    p[3] = 0.0f;
  });
  ASSERT_NE(atlas, nullptr);
  ASSERT_NE(add_mesh_map_row(*ma, "AO", MA_MESH_MAP_AO, atlas), nullptr);

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  ASSERT_EQ(width, ref);
  for (const int y : IndexRange(ref)) {
    for (const int x : IndexRange(ref)) {
      const float *got = linear_pixel(pixels, width, x, y);
      const float grey = reference_resample(values, x, y);
      EXPECT_NEAR(got[0], grey, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[1], grey, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[2], grey, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[3], 1.0f, 1e-4f) << "texel " << x << "," << y;
    }
  }
}

/** A Mesh Map row with no atlas assigned contributes nothing: the stack reads as if the row were
 * not there, and describing it does not fail. */
TEST_F(PaintLayersTestBase, mesh_map_row_without_an_atlas_is_neutral)
{
  const int ref = 4;
  Image *bottom = add_byte_map(*bmain, "BottomMap", ref, 200, 40, 10);
  add_paint_row(*ma, "Bottom", bottom);

  Vector<float> baseline;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, baseline, width, height));

  /* No slot image for MA_MESH_MAP_AO: the row drops out of the description. */
  ASSERT_NE(add_mesh_map_row(*ma, "AO", MA_MESH_MAP_AO), nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1) << "only the Paint row is carried";

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  ASSERT_EQ(pixels.size(), baseline.size());
  for (const int64_t i : baseline.index_range()) {
    EXPECT_NEAR(pixels[i], baseline[i], 1e-5f) << "component " << i;
  }
}

/** A Mesh Map mask item and content correction cover by their opacity alone: the atlas' R (or its
 * mean) is the grey, and the atlas alpha is data, never the weight. */
TEST_F(PaintLayersTestBase, mesh_map_mask_item_and_correction_cover_by_opacity_alone)
{
  const int ref = 4;
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
  auto mix = [&](const float *base, const float *top, const float t, const int k) {
    return base[k] + (top[k] - base[k]) * t;
  };

  /* A scalar mask item: the grey is R (0.5), not the mean of its RGB, and the zero alpha does not
   * weight it -- the item covers by its opacity, which is one. */
  Image *bottom_map = add_byte_map(*bmain, "BottomMap", ref, 200, 40, 10);
  MaterialPaintLayer *owner = add_paint_row(*ma, "Owner", bottom_map);
  ASSERT_NE(owner, nullptr);
  /* A mask item lays over the base mask; the white base is neutral, so the baseline stands. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, owner, 1.0f), nullptr);
  Vector<float> baseline;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, baseline, width, height));

  Image *ao_atlas = add_float_atlas(
      *bmain, "AOAtlas", ref, ref, [](float *p, const int /*x*/, const int /*y*/) {
        p[0] = 0.5f;
        p[1] = 0.9f;
        p[2] = 0.1f;
        p[3] = 0.0f;
      });
  ASSERT_NE(ao_atlas, nullptr);
  ASSERT_NE(add_mesh_map_mask_item(*ma, *owner, "AO Mask", MA_MESH_MAP_AO, ao_atlas), nullptr);

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  ASSERT_EQ(width, ref);
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      const float *got = linear_pixel(pixels, width, x, y);
      const float *base = linear_pixel(baseline, width, x, y);
      for (const int k : IndexRange(4)) {
        EXPECT_NEAR(got[k], mix(bottom, base, 0.5f, k), 1e-4f)
            << "texel " << x << "," << y << " component " << k;
      }
    }
  }

  /* A non-scalar mask item reads the mean of its RGB ((1 + 0 + 0.5) / 3 = 0.5), not its R. */
  Material *rgb_material = add_layered_material(*bmain, "MeshMapMaskRGB");
  ASSERT_NE(rgb_material, nullptr);
  MaterialPaintLayer *rgb_owner = add_paint_row(
      *rgb_material, "Owner", add_byte_map(*bmain, "RGBBottomMap", ref, 200, 40, 10));
  ASSERT_NE(rgb_owner, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*rgb_material, rgb_owner, 1.0f), nullptr);
  Vector<float> rgb_baseline;
  ASSERT_TRUE(
      render_channel(*rgb_material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rgb_baseline, width, height));

  Image *normal_atlas = add_float_atlas(
      *bmain, "NormalAtlas", ref, ref, [](float *p, const int /*x*/, const int /*y*/) {
        p[0] = 1.0f;
        p[1] = 0.0f;
        p[2] = 0.5f;
        p[3] = 0.0f;
      });
  ASSERT_NE(normal_atlas, nullptr);
  ASSERT_NE(add_mesh_map_mask_item(
                *rgb_material, *rgb_owner, "Normal Mask", MA_MESH_MAP_NORMAL_WORLD, normal_atlas),
            nullptr);

  Vector<float> rgb_pixels;
  ASSERT_TRUE(render_channel(
      *rgb_material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rgb_pixels, width, height));
  ASSERT_EQ(rgb_pixels.size(), rgb_baseline.size());
  for (const int64_t i : rgb_baseline.index_range()) {
    const float *base = rgb_baseline.data() + i - (i % 4);
    EXPECT_NEAR(rgb_pixels[i], mix(bottom, base, 0.5f, int(i % 4)), 1e-4f) << "component " << i;
  }

  /* A scalar content correction lays its atlas grey over the owner's colour by its opacity
   * alone: the atlas alpha is zero, and the correction still covers fully. */
  Material *corr_material = add_layered_material(*bmain, "MeshMapCorrection");
  ASSERT_NE(corr_material, nullptr);
  MaterialPaintLayer *corr_owner = add_paint_row(
      *corr_material, "Owner", add_byte_map(*bmain, "CorrBottomMap", ref, 200, 40, 10));
  ASSERT_NE(corr_owner, nullptr);
  Image *corr_atlas = add_float_atlas(
      *bmain, "CorrAtlas", ref, ref, [](float *p, const int /*x*/, const int /*y*/) {
        p[0] = 0.6f;
        p[1] = 0.1f;
        p[2] = 0.9f;
        p[3] = 0.0f;
      });
  ASSERT_NE(corr_atlas, nullptr);
  ASSERT_NE(add_mesh_map_correction(
                *corr_material, *corr_owner, "AO Correction", MA_MESH_MAP_AO, corr_atlas),
            nullptr);

  Vector<float> corr_pixels;
  ASSERT_TRUE(render_channel(
      *corr_material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, corr_pixels, width, height));
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      const float *got = linear_pixel(corr_pixels, width, x, y);
      EXPECT_NEAR(got[0], 0.6f, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[1], 0.6f, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[2], 0.6f, 1e-4f) << "texel " << x << "," << y;
      EXPECT_NEAR(got[3], 1.0f, 1e-4f) << "texel " << x << "," << y;
    }
  }
}

}  // namespace blender::bke::tests
