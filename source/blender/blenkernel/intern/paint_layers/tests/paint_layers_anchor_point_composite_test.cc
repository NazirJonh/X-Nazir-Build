/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Anchor Point slices and readers seen through the CPU composite: what a baked reader lays, what a
 * live slice renders, and what the two have in common. The structural rules live in
 * `paint_layers_anchor_point_test.cc`; this file compares pixels.
 */

#include "testing/testing.h"

#include "BLI_math_base.h"
#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"

#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <atomic>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "paint_layers_test_util.hh"

namespace blender::bke::tests {

class PaintLayersAnchorPointCompositeTest : public PaintLayersTestBase {
 protected:
  /** A solid \a size square byte map; \a data makes it a Non-Color map. */
  Image *add_image(const char *name,
                   const int size,
                   const uchar r,
                   const uchar g,
                   const uchar b,
                   const uchar a = 255,
                   const bool data = false)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, data, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      pixels[i * 4 + 0] = r;
      pixels[i * 4 + 1] = g;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = a;
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  MaterialPaintLayer *add_layer_with_image(const char *name,
                                           Image *image,
                                           const eMaterialPaintChannel channel =
                                               PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, layer, channel, image));
    return layer;
  }

  MaterialPaintLayer *add_point(MaterialPaintLayer &owner,
                                const PaintLayerAnchorPointList list =
                                    PaintLayerAnchorPointList::Effects,
                                MaterialPaintLayer *insert_after = nullptr)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &owner, list, insert_after, false, "Point", {});
  }

  MaterialPaintLayer *add_reader(MaterialPaintLayer &owner,
                                 const PaintLayerAnchorPointList list,
                                 const bUUID &source,
                                 MaterialPaintLayer *insert_after = nullptr)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &owner, list, insert_after, true, "Reader", source);
  }

  MaterialPaintLayer *add_correction(MaterialPaintLayer &owner, const char *name)
  {
    return BKE_paint_layers_correction_add(*ma, &owner, name);
  }

  void bake_plan()
  {
    BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
    BKE_paint_layers_bake_plan_run(*bmain, *ma);
  }

  bool render_channel(const int channel, Vector<float> &r_pixels, int &r_width, int &r_height)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    if (!BKE_paint_layers_composite_image_layers(*ma, channel, layers)) {
      return false;
    }
    if (!BKE_paint_material_composite_stack_dimensions(layers, r_width, r_height) ||
        r_width <= 0 || r_height <= 0)
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

  static const float *linear_pixel(const Vector<float> &pixels,
                                   const int width,
                                   const int x,
                                   const int y)
  {
    return pixels.data() + (int64_t(y) * width + x) * 4;
  }

  static void expect_pixels_equal(const Vector<float> &a,
                                  const Vector<float> &b,
                                  const float tolerance = 2.0f / 255.0f)
  {
    ASSERT_EQ(a.size(), b.size());
    for (int64_t i = 0; i < a.size(); i++) {
      EXPECT_NEAR(a[i], b[i], tolerance) << "texel component " << i;
    }
  }

  static const PaintMaterialCompositeCorrection *find_correction_reader(
      Span<PaintMaterialCompositeImageLayer> layers, const bUUID &reader)
  {
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
        if (BLI_uuid_equal(correction.anchor_point_reader, reader)) {
          return &correction;
        }
      }
      for (const PaintMaterialCompositeImageLayer &child : layer.children) {
        if (const PaintMaterialCompositeCorrection *found = find_correction_reader(
                Span<PaintMaterialCompositeImageLayer>(&child, 1), reader))
        {
          return found;
        }
      }
    }
    return nullptr;
  }

  static const PaintMaterialCompositeMaskItem *find_mask_reader(
      Span<PaintMaterialCompositeImageLayer> layers, const bUUID &reader)
  {
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
        if (BLI_uuid_equal(item.anchor_point_reader, reader)) {
          return &item;
        }
      }
      for (const PaintMaterialCompositeImageLayer &child : layer.children) {
        if (const PaintMaterialCompositeMaskItem *found = find_mask_reader(
                Span<PaintMaterialCompositeImageLayer>(&child, 1), reader))
        {
          return found;
        }
      }
    }
    return nullptr;
  }

  static const PaintMaterialCompositeImageLayer *find_layer_reader(
      Span<PaintMaterialCompositeImageLayer> layers, const bUUID &reader)
  {
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      if (BLI_uuid_equal(layer.anchor_point_reader, reader)) {
        return &layer;
      }
      for (const PaintMaterialCompositeImageLayer &child : layer.children) {
        if (const PaintMaterialCompositeImageLayer *found = find_layer_reader(
                Span<PaintMaterialCompositeImageLayer>(&child, 1), reader))
        {
          return found;
        }
      }
    }
    return nullptr;
  }
};

/* -------------------------------------------------------------------- */
/** \name Baked readers
 * \{ */

TEST_F(PaintLayersAnchorPointCompositeTest, corr_reader_always_reads_bake_maps)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 0, 255));
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);
  ASSERT_NE(reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  ASSERT_NE(reader->bake->coverage, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeCorrection *correction = find_correction_reader(layers, reader->marker);
  ASSERT_NE(correction, nullptr);
  EXPECT_EQ(correction->image, reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(correction->anchor_point_coverage_image, reader->bake->coverage);
  EXPECT_EQ(correction->anchor_point_slice, nullptr);
  EXPECT_FLOAT_EQ(correction->opacity, 1.0f)
      << "the coverage map already holds the reader's opacity; it is not applied twice";

  /* The factor comes from the coverage map: half red over the blue row, over a 2x2 block. */
  Vector<float> pixels;
  int width = 0, height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      const float *pixel = linear_pixel(pixels, width, x, y);
      EXPECT_NEAR(pixel[0], 0.5f, 2.0f / 255.0f) << "texel " << x << "," << y;
      EXPECT_NEAR(pixel[2], 0.5f, 2.0f / 255.0f);
    }
  }
}

TEST_F(PaintLayersAnchorPointCompositeTest, corr_reader_never_renders_live)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *owner = BKE_paint_layers_add(*ma, "Owner", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_NEVER));
  BKE_paint_layers_bake_struct_ensure(*reader);
  reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("Stale", 4, 0, 255, 0);
  reader->bake->coverage = add_image("Stale Coverage", 4, 0, 0, 255);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeCorrection *correction = find_correction_reader(layers, reader->marker);
  ASSERT_NE(correction, nullptr);
  EXPECT_EQ(correction->image, nullptr);
  EXPECT_EQ(correction->anchor_point_coverage_image, nullptr);
  ASSERT_NE(correction->anchor_point_slice, nullptr);
  ASSERT_NE(correction->anchor_point_coverage, nullptr);
}

TEST_F(PaintLayersAnchorPointCompositeTest, mask_reader_always_reads_bake_maps)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  /* A fully hidden source mask: both readers' masks reduce the owner's coverage to zero, so the
   * result is the channel's bottom colour exactly. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.0f), nullptr);
  MaterialPaintLayer *mask_point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  MaterialPaintLayer *content_point = add_point(*source);
  ASSERT_NE(mask_point, nullptr);
  ASSERT_NE(content_point, nullptr);

  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 255, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, owner, 1.0f), nullptr);
  MaterialPaintLayer *mask_reader = add_reader(
      *owner, PaintLayerAnchorPointList::MaskStack, mask_point->marker);
  MaterialPaintLayer *content_reader = add_reader(
      *owner, PaintLayerAnchorPointList::MaskStack, content_point->marker);
  ASSERT_NE(mask_reader, nullptr);
  ASSERT_NE(content_reader, nullptr);
  /* Live first, so the baked pass has something to be compared with. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *mask_reader, MA_PAINT_LAYER_BAKE_NEVER));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *content_reader, MA_PAINT_LAYER_BAKE_NEVER));
  Vector<float> live;
  int live_width = 0, live_height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, live, live_width, live_height));

  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
  const float *live_pixel = linear_pixel(live, live_width, 1, 1);
  EXPECT_NEAR(live_pixel[0], bottom[0], 2.0f / 255.0f)
      << "the zero masks leave the owner's green hidden and the bottom showing";
  EXPECT_NEAR(live_pixel[1], bottom[1], 2.0f / 255.0f);
  EXPECT_NEAR(live_pixel[2], bottom[2], 2.0f / 255.0f);

  /* Positive control: without the readers the owner's green is shown. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, mask_reader, false));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, content_reader, false));
  Vector<float> without_readers;
  int control_width = 0, control_height = 0;
  ASSERT_TRUE(render_channel(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, without_readers, control_width, control_height));
  EXPECT_GT(std::abs(linear_pixel(without_readers, control_width, 1, 1)[1] - live_pixel[1]),
            0.1f)
      << "the readers are what hides the owner";
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, mask_reader, true));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, content_reader, true));

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *mask_reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *content_reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(mask_reader->bake, nullptr);
  ASSERT_NE(content_reader->bake, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeMaskItem *mask_item = find_mask_reader(layers, mask_reader->marker);
  const PaintMaterialCompositeMaskItem *content_item = find_mask_reader(
      layers, content_reader->marker);
  ASSERT_NE(mask_item, nullptr);
  ASSERT_NE(content_item, nullptr);
  /* A mask point's scalar is the grey of the baked colour map. */
  EXPECT_EQ(mask_item->image, mask_reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_FLOAT_EQ(mask_item->opacity, BKE_paint_layers_effective_opacity(*mask_reader));
  /* A content point read as a mask reads the reader's coverage map, whose opacity is already in
   * the pixels: only visibility stays live. */
  EXPECT_EQ(content_item->image, content_reader->bake->coverage);
  EXPECT_FLOAT_EQ(content_item->opacity,
                  (content_reader->flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f);

  Vector<float> baked;
  int baked_width = 0, baked_height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked, baked_width, baked_height));
  expect_pixels_equal(live, baked);
  EXPECT_NEAR(linear_pixel(baked, baked_width, 1, 1)[1], bottom[1], 2.0f / 255.0f)
      << "the baked coverage holds the same zero-mask result";
}

TEST_F(PaintLayersAnchorPointCompositeTest, baked_and_live_reader_layer_match)
{
  MaterialPaintLayer *below = add_layer_with_image("Below", add_image("Below", 4, 0, 0, 255));
  ASSERT_NE(below, nullptr);
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Row, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_NEVER));

  Vector<float> live;
  int width = 0, height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, live, width, height));
  const float *live_pixel = linear_pixel(live, width, 1, 1);
  EXPECT_NEAR(live_pixel[0], 0.5f, 1e-5f) << "the red slice over the blue row";
  EXPECT_NEAR(live_pixel[2], 0.5f, 1e-5f);

  /* Positive control: with the reader disabled the pixels are the blue row's. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, false));
  Vector<float> without_reader;
  ASSERT_TRUE(render_channel(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, without_reader, width, height));
  EXPECT_GT(std::abs(linear_pixel(without_reader, width, 1, 1)[0] - live_pixel[0]), 1e-3f)
      << "the reader is what puts red there";
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, true));

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *reader));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeImageLayer *described = find_layer_reader(layers, reader->marker);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described->anchor_point_slice, nullptr) << "the description reads the baked map";
  EXPECT_EQ(described->color_image, reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(described->mask_image, reader->bake->coverage);

  Vector<float> baked;
  int baked_width = 0, baked_height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked, baked_width, baked_height));
  expect_pixels_equal(live, baked);
  EXPECT_NEAR(linear_pixel(baked, baked_width, 1, 1)[0], 0.5f, 2.0f / 255.0f);
}

TEST_F(PaintLayersAnchorPointCompositeTest, baked_and_live_correction_reader_match)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 0, 255));
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_NEVER));

  Vector<float> live;
  int width = 0, height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, live, width, height));
  EXPECT_NEAR(linear_pixel(live, width, 1, 1)[0], 0.5f, 1e-5f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, false));
  Vector<float> without_reader;
  ASSERT_TRUE(render_channel(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, without_reader, width, height));
  EXPECT_GT(std::abs(linear_pixel(without_reader, width, 1, 1)[0] -
                     linear_pixel(live, width, 1, 1)[0]),
            1e-3f);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, true));

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *reader));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeCorrection *described = find_correction_reader(layers, reader->marker);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described->anchor_point_slice, nullptr) << "the description reads the baked maps";
  EXPECT_EQ(described->image, reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(described->anchor_point_coverage_image, reader->bake->coverage);

  Vector<float> baked;
  int baked_width = 0, baked_height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked, baked_width, baked_height));
  expect_pixels_equal(live, baked);
}

TEST_F(PaintLayersAnchorPointCompositeTest, baked_and_live_mask_reader_match)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.25f), nullptr);
  MaterialPaintLayer *mask_point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(mask_point, nullptr);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 255, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, owner, 1.0f), nullptr);
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::MaskStack, mask_point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_NEVER));

  Vector<float> live;
  int width = 0, height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, live, width, height));

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, false));
  Vector<float> without_reader;
  ASSERT_TRUE(render_channel(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, without_reader, width, height));
  EXPECT_GT(std::abs(linear_pixel(without_reader, width, 1, 1)[1] -
                     linear_pixel(live, width, 1, 1)[1]),
            1e-3f)
      << "the mask reader changes how much of the owner shows";
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, reader, true));

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *reader));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeMaskItem *described = find_mask_reader(layers, reader->marker);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described->anchor_point_slice, nullptr) << "the description reads the baked map";
  EXPECT_EQ(described->image, reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);

  Vector<float> baked;
  int baked_width = 0, baked_height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked, baked_width, baked_height));
  expect_pixels_equal(live, baked);
}

TEST_F(PaintLayersAnchorPointCompositeTest, bake_not_planned_for_live_reader)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *never_row = BKE_paint_layers_add(
      *ma, "Never", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *never = add_reader(
      *never_row, PaintLayerAnchorPointList::Effects, point->marker);
  MaterialPaintLayer *auto_row = BKE_paint_layers_add(
      *ma, "Auto", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *automatic = add_reader(
      *auto_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(never, nullptr);
  ASSERT_NE(automatic, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *never, MA_PAINT_LAYER_BAKE_NEVER));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *automatic, MA_PAINT_LAYER_BAKE_AUTO));
  bake_plan();
  EXPECT_EQ(never->bake, nullptr);
  EXPECT_EQ(automatic->bake, nullptr) << "a shallow Auto reader stays live";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CPU slices
 * \{ */

TEST_F(PaintLayersAnchorPointCompositeTest, slice_content_stops_before_marker)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, {});
  ASSERT_NE(reader, nullptr);
  /* The marker sits between the two corrections: one below it is in the slice, one above is not. */
  MaterialPaintLayer *below = add_correction(*source, "Below");
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::Effects, below);
  MaterialPaintLayer *above = add_correction(*source, "Above");
  ASSERT_NE(below, nullptr);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(above, nullptr);
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_add(
      *ma, above, PAINT_MATERIAL_CHANNEL_BASE_COLOR) != nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, above, PAINT_MATERIAL_CHANNEL_BASE_COLOR, blue));
  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, reader, point->marker));

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color, &coverage));
  ASSERT_NE(color, nullptr);
  const float *pixel = color->float_data();
  EXPECT_NEAR(pixel[0], 1.0f, 1e-5f);
  EXPECT_NEAR(pixel[1], 0.0f, 1e-5f);
  EXPECT_NEAR(pixel[2], 0.0f, 1e-5f)
      << "the blue correction above the marker is not part of the slice";
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_mask_includes_base_mask)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.25f), nullptr);
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color, &coverage));
  ASSERT_NE(color, nullptr);
  const float *pixel = color->float_data();
  EXPECT_NEAR(pixel[0], 0.25f, 1e-5f) << "the base mask is the first item of a mask slice";
  EXPECT_NEAR(pixel[1], 0.25f, 1e-5f);
  EXPECT_NEAR(pixel[2], 0.25f, 1e-5f);
  EXPECT_NEAR(pixel[3], 1.0f, 1e-5f);
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_ignores_source_opacity_and_enabled)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color, &coverage));
  ASSERT_NE(color, nullptr);
  const int64_t count = int64_t(color->x) * color->y * 4;
  std::vector<float> before(color->float_data(), color->float_data() + count);

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, source, 0.0f));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, source, false));
  /* The slice does not depend on the source's own opacity or visibility, but the cache would hand
   * the first render back: dropping it makes the second call render the hidden source again. */
  paint_layer_anchor_point_slice_cache_clear();
  std::shared_ptr<ImBuf> hidden_color;
  std::shared_ptr<ImBuf> hidden_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &hidden_color, &hidden_coverage));
  ASSERT_NE(hidden_color, nullptr);
  for (int64_t i = 0; i < count; i++) {
    EXPECT_FLOAT_EQ(hidden_color->float_data()[i], before[i]);
  }
  EXPECT_FLOAT_EQ(hidden_color->float_data()[0], 1.0f);
}

TEST_F(PaintLayersAnchorPointCompositeTest, reader_layer_keeps_own_effects)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Row, point->marker);
  ASSERT_NE(reader, nullptr);
  MaterialPaintLayer *effect = add_correction(*reader_row, "Own Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_add(
      *ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR) != nullptr);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, green));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeImageLayer *layer = find_layer_reader(layers, reader->marker);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(layer->anchor_point_color, nullptr);
  ASSERT_EQ(layer->content_corrections.size(), 1)
      << "the reader's own corrections are laid over the slice";

  /* The correction is what the composite shows: the green value over the red slice. */
  Vector<float> pixels;
  int width = 0, height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  const float *pixel = linear_pixel(pixels, width, 1, 1);
  EXPECT_NEAR(pixel[0], 0.0f, 1e-3f) << "the reader's own correction replaces the slice colour";
  EXPECT_NEAR(pixel[1], 1.0f, 1e-3f);
}

TEST_F(PaintLayersAnchorPointCompositeTest, reader_layer_equals_source_without_upper_corrections)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Row, {});
  ASSERT_NE(reader, nullptr);
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, reader, point->marker));
  MaterialPaintLayer *above = add_correction(*source, "Above");
  ASSERT_NE(above, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_add(
      *ma, above, PAINT_MATERIAL_CHANNEL_BASE_COLOR) != nullptr);
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, above, PAINT_MATERIAL_CHANNEL_BASE_COLOR, blue));

  /* The composite shows the correction above the marker, which the slice must not contain. */
  Vector<float> pixels;
  int width = 0, height = 0;
  ASSERT_TRUE(render_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, pixels, width, height));
  const float *pixel = linear_pixel(pixels, width, 1, 1);
  EXPECT_NEAR(pixel[2], 1.0f, 1e-3f) << "the composite shows the blue correction";

  paint_layer_anchor_point_slice_cache_clear();
  std::shared_ptr<ImBuf> slice_color;
  std::shared_ptr<ImBuf> slice_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &slice_color, &slice_coverage));
  ASSERT_NE(slice_color, nullptr);
  EXPECT_NEAR(slice_color->float_data()[0], 1.0f, 1e-5f);
  EXPECT_NEAR(slice_color->float_data()[2], 0.0f, 1e-5f)
      << "the correction above the marker is not in the slice";

  /* A second, independent render supplies the description (clear makes it render its own buffer
   * instead of handing the first one back). */
  paint_layer_anchor_point_slice_cache_clear();
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeImageLayer *layer = find_layer_reader(layers, reader->marker);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(layer->anchor_point_color, nullptr);
  ASSERT_NE(layer->anchor_point_color.get(), slice_color.get());
  for (int i = 0; i < slice_color->x * slice_color->y * 4; i++) {
    EXPECT_FLOAT_EQ(layer->anchor_point_color->float_data()[i], slice_color->float_data()[i]);
  }
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_cache_coverage_layout_is_rgba)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  const int rect[4] = {1, 1, 3, 3};
  std::shared_ptr<ImBuf> full_color;
  std::shared_ptr<ImBuf> full_coverage;
  std::shared_ptr<ImBuf> partial_color;
  std::shared_ptr<ImBuf> partial_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &full_color, &full_coverage));
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &partial_color, &partial_coverage, rect));
  ASSERT_NE(full_coverage, nullptr);
  ASSERT_NE(partial_coverage, nullptr);
  /* A full render fills every pixel: a coverage buffer left zero would fail here. */
  const float *full = full_coverage->float_data();
  for (int i = 0; i < full_coverage->x * full_coverage->y; i++) {
    EXPECT_FLOAT_EQ(full[i * 4 + 0], 1.0f) << "the source alpha covers fully at texel " << i;
    EXPECT_FLOAT_EQ(full[i * 4 + 1], 1.0f);
    EXPECT_FLOAT_EQ(full[i * 4 + 2], 1.0f);
    EXPECT_FLOAT_EQ(full[i * 4 + 3], 1.0f);
  }
  /* A partial render writes the rectangle and leaves the rest zeroed, alpha included. */
  const float *partial = partial_coverage->float_data();
  for (int y = 0; y < partial_coverage->y; y++) {
    for (int x = 0; x < partial_coverage->x; x++) {
      const int i = y * partial_coverage->x + x;
      const bool inside = x >= rect[0] && x < rect[2] && y >= rect[1] && y < rect[3];
      EXPECT_FLOAT_EQ(partial[i * 4 + 0], inside ? 1.0f : 0.0f) << "texel " << x << "," << y;
      EXPECT_FLOAT_EQ(partial[i * 4 + 1], partial[i * 4 + 0]);
      EXPECT_FLOAT_EQ(partial[i * 4 + 2], partial[i * 4 + 0]);
      EXPECT_FLOAT_EQ(partial[i * 4 + 3], inside ? 1.0f : 0.0f);
    }
  }
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_cache_same_key_two_inserts_counts_once)
{
  MaterialPaintLayer *source = add_layer_with_image(
      "Source", add_image("Source", 64, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  /* Both threads miss the empty cache, render the same key and race to insert it. One entry and
   * one pair's bytes must result, however they interleave. This test is probabilistic by nature: the
   * double-check under the mutex is what it observes, and a valid implementation that serializes
   * the two renders differently still has to pass. The rounds only widen the window. */
  for (int round = 0; round < 8; round++) {
    paint_layer_anchor_point_slice_cache_clear();
    std::atomic<int> ready{0};
    std::shared_ptr<ImBuf> first_color;
    std::shared_ptr<ImBuf> first_coverage;
    std::shared_ptr<ImBuf> second_color;
    std::shared_ptr<ImBuf> second_coverage;
    bool first_ok = false;
    bool second_ok = false;
    const auto render = [&](std::shared_ptr<ImBuf> &r_color,
                            std::shared_ptr<ImBuf> &r_coverage,
                            bool &r_ok) {
      ready.fetch_add(1);
      while (ready.load() < 2) {
      }
      r_ok = BKE_paint_layers_slice_render(*ma,
                                          *reader,
                                          PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                          &r_color,
                                          &r_coverage);
    };
    std::thread first(render, std::ref(first_color), std::ref(first_coverage), std::ref(first_ok));
    std::thread second(
        render, std::ref(second_color), std::ref(second_coverage), std::ref(second_ok));
    first.join();
    second.join();
    ASSERT_TRUE(first_ok);
    ASSERT_TRUE(second_ok);
    ASSERT_NE(first_color, nullptr);
    ASSERT_NE(second_color, nullptr);
    EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 1)
        << "round " << round << ": the second insert reuses the rendered entry";
    const size_t pair_bytes = size_t(first_color->x) * first_color->y * sizeof(float[4]) +
                              size_t(first_color->x) * first_color->y * sizeof(float);
    EXPECT_EQ(paint_layer_anchor_point_slice_cache_bytes(), pair_bytes)
        << "round " << round << ": the bytes are counted once";
  }
  paint_layer_anchor_point_slice_cache_clear();
}

TEST_F(PaintLayersAnchorPointCompositeTest, normal_slice_not_renormalized)
{
  Image *normal_image = add_image("Normal", 4, 128, 200, 64, 255, true);
  MaterialPaintLayer *source = add_layer_with_image(
      "Source", normal_image, PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_NORMAL, &color, &coverage));
  ASSERT_NE(color, nullptr);
  const float *pixel = color->float_data();
  EXPECT_NEAR(pixel[0], 128.0f / 255.0f, 1e-5f);
  EXPECT_NEAR(pixel[1], 200.0f / 255.0f, 1e-5f);
  EXPECT_NEAR(pixel[2], 64.0f / 255.0f, 1e-5f)
      << "the slice is the source's encoded value, not a normalized vector";
}

TEST_F(PaintLayersAnchorPointCompositeTest, udim_nested_mask_reader_in_content_slice)
{
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, inner, 1.0f), nullptr);
  Image *tiled = add_image("Tiled Mask", 4, 0, 255, 0);
  tiled->source = IMA_SRC_TILED;
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, inner, MA_PAINT_LAYER_SOURCE_IMAGE, "Tiled", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, tiled));
  MaterialPaintLayer *mask_point = add_point(*inner, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(mask_point, nullptr);

  MaterialPaintLayer *outer = add_layer_with_image("Outer", add_image("Outer", 4, 0, 0, 255));
  MaterialPaintLayer *nested = add_reader(
      *outer, PaintLayerAnchorPointList::Effects, mask_point->marker);
  MaterialPaintLayer *outer_point = add_point(*outer);
  ASSERT_NE(nested, nullptr);
  ASSERT_NE(outer_point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, outer_point->marker);
  ASSERT_NE(reader, nullptr);

  EXPECT_TRUE(paint_layer_anchor_point_slice_is_udim(*ma, *nested));
  EXPECT_TRUE(paint_layer_anchor_point_slice_is_udim(*ma, *reader))
      << "the content slice contains a mask reader whose slice is UDIM";
  EXPECT_FALSE(paint_layer_row_reads_anchor_point(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintLayersAnchorPointCompositeTest, udim_nested_mask_reader_in_child_mask_stack)
{
  /* The tiled mask map lives behind a reader in a child row's mask stack. The child is fully
   * inside the folder's content slice, so the folder reader depends on that map. */
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, inner, 1.0f), nullptr);
  Image *tiled = add_image("Tiled Mask", 4, 0, 255, 0);
  tiled->source = IMA_SRC_TILED;
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, inner, MA_PAINT_LAYER_SOURCE_IMAGE, "Tiled", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, tiled));
  MaterialPaintLayer *mask_point = add_point(*inner, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(mask_point, nullptr);

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma,
                                                 child,
                                                 PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                 add_image("Child", 4, 0, 0, 255)));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, child, 1.0f), nullptr);
  MaterialPaintLayer *nested = add_reader(
      *child, PaintLayerAnchorPointList::MaskStack, mask_point->marker);
  ASSERT_NE(nested, nullptr);

  MaterialPaintLayer *folder_point = add_point(*folder);
  ASSERT_NE(folder_point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, folder_point->marker);
  ASSERT_NE(reader, nullptr);

  EXPECT_TRUE(paint_layer_anchor_point_slice_is_udim(*ma, *nested));
  EXPECT_TRUE(paint_layer_anchor_point_slice_is_udim(*ma, *reader))
      << "the folder's content slice contains the child's mask reader";
  EXPECT_FALSE(paint_layer_row_reads_anchor_point(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintLayersAnchorPointCompositeTest, udim_issue_for_content_target)
{
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, inner, 1.0f), nullptr);
  Image *tiled = add_image("Tiled Mask", 4, 0, 255, 0);
  tiled->source = IMA_SRC_TILED;
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, inner, MA_PAINT_LAYER_SOURCE_IMAGE, "Tiled", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, tiled));
  MaterialPaintLayer *mask_point = add_point(*inner, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(mask_point, nullptr);

  MaterialPaintLayer *outer = add_layer_with_image("Outer", add_image("Outer", 4, 0, 0, 255));
  MaterialPaintLayer *nested = add_reader(
      *outer, PaintLayerAnchorPointList::Effects, mask_point->marker);
  MaterialPaintLayer *outer_point = add_point(*outer);
  ASSERT_NE(nested, nullptr);
  ASSERT_NE(outer_point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, outer_point->marker);
  ASSERT_NE(reader, nullptr);
  /* The reader resolves to a Content target; only a nested mask reader makes it UDIM. */
  EXPECT_EQ(paint_layer_anchor_point_resolve(*ma, *reader).target,
            PaintLayerAnchorPointTarget::Content);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  const PaintLayersIssue *found = nullptr;
  for (const PaintLayersIssue &issue : issues) {
    if (BLI_uuid_equal(issue.correction, reader->marker)) {
      found = &issue;
    }
  }
  ASSERT_NE(found, nullptr) << "a Content reader whose slice needs a tiled mask is diagnosed";
  EXPECT_EQ(found->code, PaintLayersIssueCode::AnchorPointInvalid);
  EXPECT_STREQ(found->text, "A mask Anchor Point is not supported in a UDIM material");
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_describe_equals_slice_render_input)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  /* A correction above the marker is not part of the slice description either. */
  MaterialPaintLayer *above = add_correction(*source, "Above");
  ASSERT_NE(above, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, above, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color, &coverage));
  ASSERT_NE(color, nullptr);

  PaintMaterialCompositeImageLayer entry;
  ASSERT_TRUE(BKE_paint_layers_slice_describe(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, entry));
  EXPECT_FLOAT_EQ(entry.opacity, 1.0f);
  EXPECT_TRUE(entry.enabled);
  EXPECT_TRUE(entry.mask_items.is_empty());
  EXPECT_EQ(entry.mask_image, nullptr);
  EXPECT_TRUE(entry.content_corrections.is_empty())
      << "the correction above the marker is not described";

  int width = 0, height = 0;
  ASSERT_TRUE(BKE_paint_material_composite_stack_dimensions(
      Span<PaintMaterialCompositeImageLayer>(&entry, 1), width, height));
  ASSERT_EQ(width, color->x);
  ASSERT_EQ(height, color->y);
  Vector<float> described_color(int64_t(width) * height * 4);
  Vector<float> described_coverage(int64_t(width) * height);
  rcti region;
  BLI_rcti_init(&region, 0, width, 0, height);
  ASSERT_TRUE(BKE_paint_material_composite_eval_row_content(entry,
                                                           described_color.data(),
                                                           described_coverage.data(),
                                                           &region,
                                                           PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  for (int i = 0; i < width * height * 4; i++) {
    EXPECT_FLOAT_EQ(described_color[i], color->float_data()[i]);
  }
  for (int i = 0; i < width * height; i++) {
    EXPECT_FLOAT_EQ(described_coverage[i], coverage->float_data()[i * 4]);
  }
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_describe_ignores_pass_through_scale_and_visibility)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = add_layer_with_image("Child", add_image("Child", 4, 255, 0, 0));
  ASSERT_NE(child, nullptr);
  /* Move the row into the folder (a pass-through folder holds its rows as if it were absent). */
  BLI_remlink(&ma->paint_layers, child);
  BLI_addtail(&folder->children, child);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));

  MaterialPaintLayer *point = add_point(*child);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, child, 0.0f));
  PaintMaterialCompositeImageLayer entry;
  ASSERT_TRUE(BKE_paint_layers_slice_describe(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, entry));
  EXPECT_FLOAT_EQ(entry.opacity, 1.0f) << "the slice is read straight, not by the stack's factor";
  EXPECT_TRUE(entry.enabled);
  const MaterialPaintLayerChannel *child_record = paint_layer_channel_find(
      *child, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(child_record, nullptr);
  EXPECT_EQ(entry.color_image, child_record->image)
      << "the hidden folder and the zero opacity do not change what the slice is made of";
}

TEST_F(PaintLayersAnchorPointCompositeTest, bake_partial_rect_does_not_render_full_slice_first)
{
  /* The reader's slice contains a nested reader. A partial bake must render that nested slice over
   * the requested rectangle only: a full one would be cached, which the cache count shows. */
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  MaterialPaintLayer *inner_point = add_point(*inner);
  ASSERT_NE(inner_point, nullptr);
  MaterialPaintLayer *outer = add_layer_with_image("Outer", add_image("Outer", 4, 0, 0, 255));
  MaterialPaintLayer *nested = add_reader(
      *outer, PaintLayerAnchorPointList::Effects, inner_point->marker);
  MaterialPaintLayer *outer_point = add_point(*outer);
  ASSERT_NE(nested, nullptr);
  ASSERT_NE(outer_point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, outer_point->marker);
  ASSERT_NE(reader, nullptr);

  float full_color[4 * 4 * 4];
  float full_coverage[4 * 4];
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *reader,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                4,
                                                full_color,
                                                full_coverage));

  float partial_color[4 * 4 * 4];
  float partial_coverage[4 * 4];
  const int rect[4] = {1, 1, 3, 3};
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *reader,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                4,
                                                partial_color,
                                                partial_coverage,
                                                rect));
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 0)
      << "the nested slice is rendered over the rectangle, not fully and cached";
  for (int y = 1; y < 3; y++) {
    for (int x = 1; x < 3; x++) {
      const int64_t index = int64_t(y) * 4 + x;
      for (int c = 0; c < 4; c++) {
        EXPECT_FLOAT_EQ(partial_color[index * 4 + c], full_color[index * 4 + c]);
      }
      EXPECT_FLOAT_EQ(partial_coverage[index], full_coverage[index]);
    }
  }
}

TEST_F(PaintLayersAnchorPointCompositeTest, bake_partial_rect_renders_only_region_of_slice)
{
  /* A source that differs along both axes, so a misplaced rectangle cannot compare equal. */
  Image *source_image = add_image("Source", 4, 255, 0, 0);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(source_image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  uchar *pixels = ibuf->byte_data_for_write();
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      uchar *p = pixels + (int64_t(y) * 4 + x) * 4;
      p[0] = uchar(x * 85);
      p[1] = uchar(y * 85);
      p[2] = 0;
      p[3] = 255;
    }
  }
  BKE_image_release_ibuf(source_image, ibuf, lock);
  MaterialPaintLayer *source = add_layer_with_image("Source", source_image);
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  float full_color[8 * 8 * 4];
  float full_coverage[8 * 8];
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *reader,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                8,
                                                full_color,
                                                full_coverage));

  const float sentinel = -1.0f;
  float partial_color[8 * 8 * 4];
  float partial_coverage[8 * 8];
  for (float &value : partial_color) {
    value = sentinel;
  }
  for (float &value : partial_coverage) {
    value = sentinel;
  }
  const int rect[4] = {2, 2, 6, 6};
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *reader,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                8,
                                                partial_color,
                                                partial_coverage,
                                                rect));
  for (int y = 2; y < 6; y++) {
    for (int x = 2; x < 6; x++) {
      const int64_t index = int64_t(y) * 8 + x;
      for (int c = 0; c < 4; c++) {
        EXPECT_FLOAT_EQ(partial_color[index * 4 + c], full_color[index * 4 + c]);
      }
      EXPECT_FLOAT_EQ(partial_coverage[index], full_coverage[index]);
    }
  }
  /* A bake that rendered the whole slice first would still write only the rectangle, so the
   * observable outside it is the untouched caller buffer. The nested-slice variant, where a full
   * pre-render would also populate the slice cache, is
   * `bake_partial_rect_does_not_render_full_slice_first`. */
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      if (x >= rect[0] && x < rect[2] && y >= rect[1] && y < rect[3]) {
        continue;
      }
      const int64_t index = int64_t(y) * 8 + x;
      for (int c = 0; c < 4; c++) {
        EXPECT_FLOAT_EQ(partial_color[index * 4 + c], sentinel) << "outside texel " << x << "," << y;
      }
      EXPECT_FLOAT_EQ(partial_coverage[index], sentinel) << "outside texel " << x << "," << y;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hashes
 * \{ */

TEST_F(PaintLayersAnchorPointCompositeTest, slice_hash_ignores_source_opacity)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  const uint64_t before = paint_layer_anchor_point_slice_hash(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, source, 0.1f));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, source, false));
  EXPECT_EQ(paint_layer_anchor_point_slice_hash(
                *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            before);
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_hash_changes_on_nested_reader_target)
{
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  MaterialPaintLayer *inner_point = add_point(*inner);
  MaterialPaintLayer *outer = add_layer_with_image("Outer", add_image("Outer", 4, 0, 255, 0));
  MaterialPaintLayer *nested = add_reader(
      *outer, PaintLayerAnchorPointList::Effects, inner_point->marker);
  MaterialPaintLayer *outer_point = add_point(*outer);
  ASSERT_NE(nested, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, outer_point->marker);
  ASSERT_NE(reader, nullptr);
  const uint64_t before = paint_layer_anchor_point_slice_hash(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, nested, {}));
  EXPECT_NE(paint_layer_anchor_point_slice_hash(
                *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            before);
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_hash_content_ignores_mask_stack)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *content_point = add_point(*source);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.5f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, source, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask Item", 0.25f);
  ASSERT_NE(item, nullptr);
  /* The point comes after the item, so the mask slice includes it and the content slice does not. */
  MaterialPaintLayer *mask_point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *content_reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, content_point->marker);
  MaterialPaintLayer *mask_reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, mask_point->marker);
  ASSERT_NE(content_reader, nullptr);
  ASSERT_NE(mask_reader, nullptr);
  const uint64_t content_before = paint_layer_anchor_point_slice_hash(
      *ma, *content_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const uint64_t mask_before = paint_layer_anchor_point_slice_hash(
      *ma, *mask_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, item, 0.5f));
  EXPECT_EQ(paint_layer_anchor_point_slice_hash(
                *ma, *content_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            content_before)
      << "a content slice never reads the mask";
  EXPECT_NE(paint_layer_anchor_point_slice_hash(
                *ma, *mask_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            mask_before)
      << "the positive control: the mask slice does read it";
}

TEST_F(PaintLayersAnchorPointCompositeTest, slice_hash_mask_ignores_effects)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.5f), nullptr);
  MaterialPaintLayer *mask_point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  MaterialPaintLayer *effect = add_correction(*source, "Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_add(
      *ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR) != nullptr);
  /* The point is after the correction, so the content slice contains it and the mask slice does
   * not. */
  MaterialPaintLayer *content_point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *mask_reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, mask_point->marker);
  MaterialPaintLayer *content_reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, content_point->marker);
  ASSERT_NE(mask_reader, nullptr);
  ASSERT_NE(content_reader, nullptr);
  const uint64_t mask_before = paint_layer_anchor_point_slice_hash(
      *ma, *mask_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const uint64_t content_before = paint_layer_anchor_point_slice_hash(
      *ma, *content_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, green));
  EXPECT_EQ(paint_layer_anchor_point_slice_hash(
                *ma, *mask_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            mask_before)
      << "a mask slice never reads the corrections";
  EXPECT_NE(paint_layer_anchor_point_slice_hash(
                *ma, *content_reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            content_before)
      << "the positive control: the content slice does read them";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invalidation
 * \{ */

TEST_F(PaintLayersAnchorPointCompositeTest, source_edit_marks_reader_cache_stale)
{
  Image *source_image = add_image("Source", 4, 255, 0, 0);
  MaterialPaintLayer *source = add_layer_with_image("Source", source_image);
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *reader));

  BKE_image_partial_update_mark_full_update(source_image);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *reader));
}

TEST_F(PaintLayersAnchorPointCompositeTest, composite_cache_sees_source_stroke)
{
  Image *source_image = add_image("Source", 4, 255, 0, 0);
  MaterialPaintLayer *source = add_layer_with_image("Source", source_image);
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);
  uint64_t first_revision = 0;
  ImBuf *composite = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &first_revision);
  ASSERT_NE(composite, nullptr);
  ASSERT_NE(composite->byte_data(), nullptr);
  /* The control: without an edit the same description keeps its revision. */
  uint64_t same_revision = 0;
  BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &same_revision);
  EXPECT_EQ(same_revision, first_revision) << "no edit, no new revision";

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(source_image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  rcti dab;
  BLI_rcti_init(&dab, 0, 2, 0, 2);
  BKE_image_partial_update_mark_region(source_image, static_cast<ImageTile *>(source_image->tiles.first), ibuf, &dab);
  BKE_image_release_ibuf(source_image, ibuf, lock);

  uint64_t second_revision = 0;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &second_revision);
  EXPECT_NE(second_revision, first_revision)
      << "the reader's slice subscribes to the source map through anchor_point_images";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resolver and bake blocking
 * \{ */

static uint64_t layer_topology_hash(const Material &ma, const MaterialPaintLayer &layer)
{
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(ma);
  return paint_layers::paint_layers_layer_topology_hash(
      ma, layer, paint_layers::paint_layers_wired_channels(ma), forms);
}

TEST_F(PaintLayersAnchorPointCompositeTest, resolve_cycle_detected)
{
  MaterialPaintLayer *bottom = add_layer_with_image("Bottom", add_image("Bottom", 4, 255, 0, 0));
  MaterialPaintLayer *cycle_reader = add_reader(
      *bottom, PaintLayerAnchorPointList::Effects, {});
  MaterialPaintLayer *bottom_point = add_point(*bottom);
  ASSERT_NE(cycle_reader, nullptr);
  ASSERT_NE(bottom_point, nullptr);

  MaterialPaintLayer *middle = add_layer_with_image("Middle", add_image("Middle", 4, 0, 255, 0));
  MaterialPaintLayer *middle_reader = add_reader(
      *middle, PaintLayerAnchorPointList::Effects, bottom_point->marker);
  MaterialPaintLayer *middle_point = add_point(*middle);
  ASSERT_NE(middle_reader, nullptr);
  ASSERT_NE(middle_point, nullptr);

  MaterialPaintLayer *top = add_layer_with_image("Top", add_image("Top", 4, 0, 0, 255));
  MaterialPaintLayer *top_reader = add_reader(
      *top, PaintLayerAnchorPointList::Effects, middle_point->marker);
  MaterialPaintLayer *top_point = add_point(*top);
  ASSERT_NE(top_reader, nullptr);
  ASSERT_NE(top_point, nullptr);
  /* The chain is acyclic before the closing edit: the positive control for the cycle report. */
  EXPECT_EQ(paint_layer_anchor_point_resolve(*ma, *middle_reader).issue,
            PaintLayerAnchorPointIssue::None);
  /* The bottom reader names a point in the top row; the top row's slice contains the middle reader,
   * whose slice contains the bottom one again. The starting reference is valid, so the chain walk
   * is what has to report the loop. */
  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, cycle_reader, top_point->marker));

  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *middle_reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::Cycle);
}

TEST_F(PaintLayersAnchorPointCompositeTest, resolve_rejects_non_marker_reference)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(*row, PaintLayerAnchorPointList::Effects, {});
  MaterialPaintLayer *correction = add_correction(*source, "Not A Point");
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(correction, nullptr);
  /* Written directly on purpose: the setter refuses a correction's marker, which is the state this
   * test resolves. */
  reader->anchor_point_source = correction->marker;
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::NotFound);
  EXPECT_EQ(ref.target, PaintLayerAnchorPointTarget::None);
}

TEST_F(PaintLayersAnchorPointCompositeTest, nested_reader_object_mask_detected)
{
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  inner->object_mask_flag |= MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  MaterialPaintLayer *inner_point = add_point(*inner);
  MaterialPaintLayer *middle = add_layer_with_image("Middle", add_image("Middle", 4, 0, 255, 0));
  MaterialPaintLayer *middle_reader = add_reader(
      *middle, PaintLayerAnchorPointList::Effects, inner_point->marker);
  MaterialPaintLayer *middle_point = add_point(*middle);
  MaterialPaintLayer *outer = BKE_paint_layers_add(*ma, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *outer_reader = add_reader(
      *outer, PaintLayerAnchorPointList::Effects, middle_point->marker);
  ASSERT_NE(middle_reader, nullptr);
  ASSERT_NE(outer_reader, nullptr);
  EXPECT_TRUE(paint_layer_anchor_point_slice_has_object_mask(*ma, *outer_reader))
      << "the object mask of the nested reader's own slice is inside the outer slice";
}

TEST_F(PaintLayersAnchorPointCompositeTest, limit_twelve_points_counts_only_participating)
{
  for (int i = 0; i < PAINT_LAYER_ANCHOR_POINT_LIMIT; i++) {
    MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
    MaterialPaintLayer *reader = add_reader(*row, PaintLayerAnchorPointList::Effects, {});
    MaterialPaintLayer *point = add_point(*row);
    ASSERT_NE(reader, nullptr);
    ASSERT_NE(point, nullptr);
    ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, reader, point->marker));
  }
  /* An unused point and a disabled reader take no part. */
  MaterialPaintLayer *spare_row = BKE_paint_layers_add(
      *ma, "Spare", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(spare_row, nullptr);
  MaterialPaintLayer *spare_point = add_point(*spare_row);
  ASSERT_NE(spare_point, nullptr);
  MaterialPaintLayer *disabled = add_reader(
      *spare_row, PaintLayerAnchorPointList::Effects, spare_point->marker);
  ASSERT_NE(disabled, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, disabled, false));

  int points = 0;
  int readers = 0;
  paint_layer_anchor_point_counts(*ma, points, readers);
  EXPECT_EQ(points, PAINT_LAYER_ANCHOR_POINT_LIMIT);
  EXPECT_EQ(readers, PAINT_LAYER_ANCHOR_POINT_LIMIT);
}

TEST_F(PaintLayersAnchorPointCompositeTest, source_row_with_read_point_not_substituted)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, point->marker), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *source, MA_PAINT_LAYER_BAKE_ALWAYS));
  EXPECT_TRUE(BKE_paint_layers_bake_blocked(*ma, *source));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *source))
      << "a cache of the row would swallow its published point";

  /* The positive control: the same kind of row with no read point is substituted by its bake. */
  MaterialPaintLayer *control = add_layer_with_image(
      "Control", add_image("Control", 4, 0, 0, 255));
  ASSERT_NE(control, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *control, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *control));
}

TEST_F(PaintLayersAnchorPointCompositeTest, parent_folder_with_read_point_not_substituted)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "Folder", nullptr, PaintLayerPlace::Above);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child", 4, 255, 0, 0)));
  MaterialPaintLayer *point = add_point(*child);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, point->marker), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  EXPECT_TRUE(BKE_paint_layers_bake_blocked(*ma, *folder));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *folder));

  /* The positive control: a folder with no read point inside is substituted by its bake. */
  MaterialPaintLayer *control = BKE_paint_layers_add(
      *ma, "Control Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(control, nullptr);
  control->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *control_child = BKE_paint_layers_add(
      *ma, "Control Child", control, PaintLayerPlace::Into);
  ASSERT_NE(control_child, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, control_child, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma,
                                                 control_child,
                                                 PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                 add_image("Control Child", 4, 0, 255, 0)));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *control, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *control));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Topology hashes
 * \{ */

TEST_F(PaintLayersAnchorPointCompositeTest, topology_hash_unchanged_by_pixel_edit)
{
  Image *image = add_image("Source", 4, 255, 0, 0);
  MaterialPaintLayer *source = add_layer_with_image("Source", image);
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, point->marker), nullptr);
  const uint64_t before = layer_topology_hash(*ma, *reader_row);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ibuf->byte_data_for_write()[0] = 0;
  BKE_image_release_ibuf(image, ibuf, lock);
  EXPECT_EQ(layer_topology_hash(*ma, *reader_row), before);
}

TEST_F(PaintLayersAnchorPointCompositeTest, topology_hash_changes_on_point_move)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *first = add_correction(*source, "First");
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::Effects, first);
  MaterialPaintLayer *second = add_correction(*source, "Second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(second, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, point->marker), nullptr);
  const uint64_t before = layer_topology_hash(*ma, *source);

  BLI_remlink(&source->effects, point);
  BLI_insertlinkafter(&source->effects, second, point);
  EXPECT_NE(layer_topology_hash(*ma, *source), before)
      << "the point now also contains the second correction";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invalidation and duplicates
 * \{ */

TEST_F(PaintLayersAnchorPointCompositeTest, delete_point_releases_reader_cache)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);

  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, point));
  EXPECT_EQ(reader->bake, nullptr);
}

TEST_F(PaintLayersAnchorPointCompositeTest, mask_remove_releases_reader_cache)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.5f), nullptr);
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);

  ASSERT_TRUE(BKE_paint_layers_mask_remove(*ma, source));
  EXPECT_EQ(reader->bake, nullptr);
}

TEST_F(PaintLayersAnchorPointCompositeTest, source_set_releases_reader_cache)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);

  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, reader, {}));
  EXPECT_EQ(reader->bake, nullptr);
}

TEST_F(PaintLayersAnchorPointCompositeTest, mask_copy_remaps_reference_inside_mask_stack)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.5f), nullptr);
  MaterialPaintLayer *reader = add_reader(
      *source, PaintLayerAnchorPointList::MaskStack, {});
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(point, nullptr);
  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, reader, point->marker));

  MaterialPaintLayer *target = add_layer_with_image("Target", add_image("Target", 4, 0, 255, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, target, 1.0f), nullptr);
  ASSERT_TRUE(BKE_paint_layers_mask_copy(*bmain, *ma, *source, *target));

  const MaterialPaintLayer *copied_point = nullptr;
  const MaterialPaintLayer *copied_reader = nullptr;
  for (const MaterialPaintLayer &item : target->mask_stack) {
    if (paint_layer_kind_is_anchor_point_marker(item)) {
      copied_point = &item;
    }
    if (paint_layer_kind_reads_anchor_point(item)) {
      copied_reader = &item;
    }
  }
  ASSERT_NE(copied_point, nullptr);
  ASSERT_NE(copied_reader, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(copied_reader->anchor_point_source, copied_point->marker));
  EXPECT_FALSE(BLI_uuid_equal(copied_reader->anchor_point_source, point->marker));
}

TEST_F(PaintLayersAnchorPointCompositeTest, correction_paste_keeps_external_reference)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 255, 0));
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  MaterialPaintLayer *target = add_layer_with_image("Target", add_image("Target", 4, 0, 0, 255));
  MaterialPaintLayer *pasted = BKE_paint_layers_correction_paste(
      *bmain, *ma, *reader, target);
  ASSERT_NE(pasted, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(pasted->anchor_point_source, point->marker))
      << "an external point still exists in this material and keeps its reference";
}

TEST_F(PaintLayersAnchorPointCompositeTest, paste_into_other_material_clears_reference)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 255, 0));
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  Material *other = BKE_material_add(bmain, "Other");
  other->paint_layers_flag |= MA_PAINT_LAYERED;
  MaterialPaintLayer *target = BKE_paint_layers_add(
      *other, "Target", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(target, nullptr);
  MaterialPaintLayer *pasted = BKE_paint_layers_correction_paste(
      *bmain, *other, *reader, target);
  ASSERT_NE(pasted, nullptr);
  EXPECT_TRUE(BLI_uuid_is_nil(pasted->anchor_point_source))
      << "the other material has no such point";
}

TEST_F(PaintLayersAnchorPointCompositeTest, composite_cache_sees_baked_coverage_edit)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 0, 255));
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);
  ASSERT_NE(reader->bake->coverage, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);
  uint64_t first_revision = 0;
  ASSERT_NE(BKE_paint_material_composite_cache_ensure(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &first_revision),
            nullptr);
  /* The control: without an edit the same description keeps its revision. */
  uint64_t same_revision = 0;
  BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &same_revision);
  EXPECT_EQ(same_revision, first_revision) << "no edit, no new revision";

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(reader->bake->coverage, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  rcti dab;
  BLI_rcti_init(&dab, 0, 2, 0, 2);
  BKE_image_partial_update_mark_region(
      reader->bake->coverage, static_cast<ImageTile *>(reader->bake->coverage->tiles.first), ibuf, &dab);
  BKE_image_release_ibuf(reader->bake->coverage, ibuf, lock);

  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  uint64_t second_revision = 0;
  BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &second_revision);
  EXPECT_NE(second_revision, first_revision)
      << "the baked reader's coverage map is a source of its composite";
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_descriptor_carries_reader_reference)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Row, point->marker);
  ASSERT_NE(reader, nullptr);
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeImageLayer *layer = find_layer_reader(layers, reader->marker);
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(layer->anchor_point_reader, reader->marker));
  EXPECT_EQ(layer->anchor_point_target, int8_t(PaintLayerAnchorPointTarget::Content));
  EXPECT_TRUE(BLI_uuid_equal(layer->anchor_point_point, point->marker))
      << "the descriptor names the point the slice comes from, not the reader";
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_descriptor_marks_producer_after_correction)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, source, 0.5f), nullptr);
  MaterialPaintLayer *first = add_correction(*source, "First");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, first, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  MaterialPaintLayer *content_point = add_point(*source);
  MaterialPaintLayer *mask_point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(content_point, nullptr);
  ASSERT_NE(mask_point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, content_point->marker),
            nullptr);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, mask_point->marker),
            nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  /* Find the source row by what it lays, not by a marker it is then checked to contain. */
  const MaterialPaintLayerChannel *source_record = paint_layer_channel_find(
      *source, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(source_record, nullptr);
  const PaintMaterialCompositeImageLayer *source_layer = nullptr;
  for (const PaintMaterialCompositeImageLayer &layer : layers) {
    if (layer.color_image == source_record->image) {
      source_layer = &layer;
      break;
    }
  }
  ASSERT_NE(source_layer, nullptr);
  ASSERT_EQ(source_layer->anchor_point_content_producers.size(), 1u);
  EXPECT_TRUE(BLI_uuid_equal(source_layer->anchor_point_content_producers[0],
                             content_point->marker))
      << "only the content marker is a producer; the corrections around it are not";
  ASSERT_EQ(source_layer->anchor_point_mask_producers.size(), 1u);
  EXPECT_TRUE(BLI_uuid_equal(source_layer->anchor_point_mask_producers[0], mask_point->marker));
}

/** \} */

}  // namespace blender::bke::tests
