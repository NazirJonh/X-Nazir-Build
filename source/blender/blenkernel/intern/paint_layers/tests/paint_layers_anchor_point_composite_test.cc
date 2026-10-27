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
#include "BLI_string.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"
#include "BLI_utildefines.h"

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
#include <array>
#include <cmath>
#include <limits>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
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

  /**
   * The GPU capture plan of one channel's stack. The stack is described without rendering any
   * slice, as the GPU path does.
   */
  bool capture_plan_for_channel(const int channel, Vector<PaintLayersGpuCaptureJob> &r_jobs)
  {
    Vector<PaintMaterialCompositeImageLayer> stack;
    BKE_paint_layers_composite_image_layers(*ma, channel, stack, /*describe_only=*/true);
    std::array<Span<const PaintMaterialCompositeImageLayer>, 1> stacks = {
        Span<const PaintMaterialCompositeImageLayer>(stack.data(), int64_t(stack.size()))};
    std::array<int, 1> channels = {channel};
    const char *reason = nullptr;
    const Span<Span<const PaintMaterialCompositeImageLayer>> stack_view(
        stacks.data(), int64_t(stacks.size()));
    const Span<int> channel_view(channels.data(), int64_t(channels.size()));
    const bool ok = BKE_paint_layers_gpu_capture_plan(
        *ma, stack_view, channel_view, r_jobs, &reason);
    EXPECT_TRUE(ok) << (reason != nullptr ? reason : "the plan failed without a reason");
    return ok;
  }

  static const PaintLayersGpuCaptureJob *find_job(const Vector<PaintLayersGpuCaptureJob> &jobs,
                                                  const bUUID &point)
  {
    for (const PaintLayersGpuCaptureJob &job : jobs) {
      if (BLI_uuid_equal(job.point, point)) {
        return &job;
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
  EXPECT_EQ(described->anchor_point_color, nullptr) << "the description reads the baked map";
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
                              size_t(first_color->x) * first_color->y * sizeof(float[4]);
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

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_unsupported_reason_allows_live_slices_and_refuses_baked_coverage)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 0, 255));
  MaterialPaintLayer *reader = add_reader(
      *owner, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  bake_plan();
  ASSERT_NE(reader->bake, nullptr);
  ASSERT_NE(reader->bake->coverage, nullptr);

  /* A baked correction covers by a coverage map the correction pass cannot read: refused. */
  Vector<PaintMaterialCompositeImageLayer> baked_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked_layers));
  bool refused = false;
  for (const PaintMaterialCompositeImageLayer &layer : baked_layers) {
    refused |= BKE_paint_layers_gpu_unsupported_reason(layer) != nullptr;
  }
  EXPECT_TRUE(refused) << "a baked Anchor Point correction is not laid by the GPU path";

  /* The same reader live is a slice slot: every row stays shadable. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_NEVER));
  Vector<PaintMaterialCompositeImageLayer> live_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, live_layers));
  for (const PaintMaterialCompositeImageLayer &layer : live_layers) {
    EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(layer), nullptr);
  }
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_descriptor_carries_reader_reference){
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

/* -------------------------------------------------------------------- */
/** \name Describe-only stacks and the GPU capture plan
 * \{ */

TEST_F(PaintLayersAnchorPointCompositeTest, describe_only_reader_has_descriptor_without_buffers)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  paint_layer_anchor_point_slice_cache_clear();
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, /*describe_only=*/true));
  const PaintMaterialCompositeImageLayer *layer = find_layer_reader(layers, reader->marker);
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(layer->anchor_point_reader, reader->marker));
  EXPECT_TRUE(BLI_uuid_equal(layer->anchor_point_point, point->marker));
  EXPECT_EQ(layer->anchor_point_target, int8_t(PaintLayerAnchorPointTarget::Content));
  EXPECT_EQ(layer->anchor_point_hash,
            paint_layer_anchor_point_slice_hash(*ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(layer->anchor_point_images.is_empty())
      << "a describe-only reader still subscribes to the maps its slice is rendered from";
  EXPECT_EQ(layer->anchor_point_color, nullptr) << "no CPU slice is rendered for the GPU path";
  EXPECT_EQ(layer->anchor_point_coverage, nullptr);
}

TEST_F(PaintLayersAnchorPointCompositeTest, describe_only_does_not_touch_slice_cache)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::Effects, point->marker), nullptr);

  paint_layer_anchor_point_slice_cache_clear();
  Vector<PaintMaterialCompositeImageLayer> described;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, described, /*describe_only=*/true));
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 0)
      << "describe-only must not render and cache a slice";
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_bytes(), 0);

  /* Positive control: a full description still renders and caches the same slice. */
  Vector<PaintMaterialCompositeImageLayer> full;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, full));
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 1);
  EXPECT_GT(paint_layer_anchor_point_slice_cache_bytes(), 0u);
}

TEST_F(PaintLayersAnchorPointCompositeTest, full_description_unchanged_by_describe_only_flag)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);

  Vector<PaintMaterialCompositeImageLayer> full;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, full));
  const PaintMaterialCompositeImageLayer *full_layer = find_layer_reader(full, reader->marker);
  ASSERT_NE(full_layer, nullptr);
  ASSERT_NE(full_layer->anchor_point_color, nullptr);
  ASSERT_NE(full_layer->anchor_point_coverage, nullptr);

  Vector<PaintMaterialCompositeImageLayer> described;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, described, /*describe_only=*/true));
  const PaintMaterialCompositeImageLayer *described_layer = find_layer_reader(
      described, reader->marker);
  ASSERT_NE(described_layer, nullptr);
  EXPECT_EQ(described_layer->anchor_point_hash, full_layer->anchor_point_hash);
  EXPECT_EQ(described_layer->anchor_point_images, full_layer->anchor_point_images);
  EXPECT_EQ(described_layer->opacity, full_layer->opacity);
  EXPECT_EQ(described_layer->enabled, full_layer->enabled);
  EXPECT_EQ(described_layer->blend, full_layer->blend);
  EXPECT_EQ(described_layer->color_image, full_layer->color_image);

  Vector<PaintMaterialCompositeImageLayer> full_after;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, full_after));
  const PaintMaterialCompositeImageLayer *after_layer = find_layer_reader(
      full_after, reader->marker);
  ASSERT_NE(after_layer, nullptr);
  EXPECT_NE(after_layer->anchor_point_color, nullptr)
      << "a CPU description after a describe-only pass still renders its slice";
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_capture_plan_collects_nested_reader_of_disabled_source)
{
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  MaterialPaintLayer *inner_point = add_point(*inner);
  MaterialPaintLayer *outer = add_layer_with_image("Outer", add_image("Outer", 4, 0, 0, 255));
  ASSERT_NE(inner_point, nullptr);
  ASSERT_NE(add_reader(*outer, PaintLayerAnchorPointList::Effects, inner_point->marker), nullptr);
  MaterialPaintLayer *outer_point = add_point(*outer);
  ASSERT_NE(outer_point, nullptr);
  MaterialPaintLayer *top = add_layer_with_image("Top", add_image("Top", 4, 120, 80, 40, 255));
  ASSERT_NE(add_reader(*top, PaintLayerAnchorPointList::Effects, outer_point->marker), nullptr);
  /* Nothing lays the source row itself: its point is still read by the visible reader. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, outer, false));

  Vector<PaintLayersGpuCaptureJob> jobs;
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  ASSERT_EQ(jobs.size(), 2u);
  EXPECT_NE(find_job(jobs, outer_point->marker), nullptr);
  const PaintLayersGpuCaptureJob *nested = find_job(jobs, inner_point->marker);
  ASSERT_NE(nested, nullptr) << "the reader nested in the disabled source's slice still gets a slot";
  EXPECT_EQ(nested->kind, PaintLayersGpuCaptureKind::Content);
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_capture_plan_orders_by_point_rank_not_reader_rank)
{
  /* Stack, bottom to top: row 1 carries J2's point; row 5 carries J1's point; J1's reader is on
   * row 6, J2's on row 9. The walk meets J1's reader first, so an unsorted plan (or one sorted by
   * reader rank) is [J1, J2]; only the point rank puts J2 first. */
  MaterialPaintLayer *row1 = add_layer_with_image("Row1", add_image("Row1", 4, 255, 0, 0));
  MaterialPaintLayer *point2 = add_point(*row1);
  ASSERT_NE(point2, nullptr);
  for (int i = 2; i <= 4; i++) {
    char name[32];
    SNPRINTF(name, "Filler %d", i);
    ASSERT_NE(BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above), nullptr);
  }
  MaterialPaintLayer *row5 = add_layer_with_image("Row5", add_image("Row5", 4, 0, 255, 0));
  MaterialPaintLayer *point1 = add_point(*row5);
  ASSERT_NE(point1, nullptr);
  MaterialPaintLayer *row6 = add_layer_with_image("Row6", add_image("Row6", 4, 0, 0, 255));
  MaterialPaintLayer *reader1 = add_reader(
      *row6, PaintLayerAnchorPointList::Effects, point1->marker);
  ASSERT_NE(reader1, nullptr);
  for (int i = 7; i <= 8; i++) {
    char name[32];
    SNPRINTF(name, "Filler %d", i);
    ASSERT_NE(BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above), nullptr);
  }
  MaterialPaintLayer *row9 = add_layer_with_image("Row9", add_image("Row9", 4, 120, 80, 40, 255));
  MaterialPaintLayer *reader2 = add_reader(
      *row9, PaintLayerAnchorPointList::Effects, point2->marker);
  ASSERT_NE(reader2, nullptr);

  const int point_rank1 = paint_layer_anchor_point_rank(*ma, point1->marker);
  const int point_rank2 = paint_layer_anchor_point_rank(*ma, point2->marker);
  const int reader_rank1 = paint_layer_anchor_point_rank(*ma, reader1->marker);
  const int reader_rank2 = paint_layer_anchor_point_rank(*ma, reader2->marker);
  EXPECT_LT(point_rank2, point_rank1) << "J2's point is below J1's";
  EXPECT_LT(reader_rank1, reader_rank2) << "J1's reader is met first";

  Vector<PaintLayersGpuCaptureJob> jobs;
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  ASSERT_EQ(jobs.size(), 2u);
  EXPECT_TRUE(BLI_uuid_equal(jobs[0].point, point2->marker))
      << "the plan is sorted by point rank, not by reader rank";
  EXPECT_TRUE(BLI_uuid_equal(jobs[1].point, point1->marker));
  EXPECT_EQ(jobs[0].rank, point_rank2);
  EXPECT_EQ(jobs[1].rank, point_rank1);
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_capture_plan_orders_by_point_rank)
{
  MaterialPaintLayer *bottom = add_layer_with_image("Bottom", add_image("Bottom", 4, 255, 0, 0));
  MaterialPaintLayer *bottom_point = add_point(*bottom);
  MaterialPaintLayer *middle = add_layer_with_image("Middle", add_image("Middle", 4, 0, 255, 0));
  MaterialPaintLayer *middle_point = add_point(*middle);
  MaterialPaintLayer *top = add_layer_with_image("Top", add_image("Top", 4, 120, 80, 40, 255));
  ASSERT_NE(bottom_point, nullptr);
  ASSERT_NE(middle_point, nullptr);
  ASSERT_NE(add_reader(*top, PaintLayerAnchorPointList::Effects, middle_point->marker), nullptr);
  MaterialPaintLayer *top_reader = add_reader(
      *top, PaintLayerAnchorPointList::Effects, bottom_point->marker, nullptr);
  ASSERT_NE(top_reader, nullptr);

  Vector<PaintLayersGpuCaptureJob> jobs;
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  ASSERT_EQ(jobs.size(), 2u);
  EXPECT_LT(jobs[0].rank, jobs[1].rank);
  EXPECT_TRUE(BLI_uuid_equal(jobs[0].point, bottom_point->marker));
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_capture_plan_dedups_shared_point)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *first = add_layer_with_image("First", add_image("First", 4, 20, 40, 60, 255));
  MaterialPaintLayer *second = add_layer_with_image("Second", add_image("Second", 4, 60, 40, 20, 255));
  ASSERT_NE(add_reader(*first, PaintLayerAnchorPointList::Effects, point->marker), nullptr);
  ASSERT_NE(add_reader(*second, PaintLayerAnchorPointList::Effects, point->marker), nullptr);

  Vector<PaintLayersGpuCaptureJob> jobs;
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  ASSERT_EQ(jobs.size(), 1u) << "two readers of one point share a single slot";
  EXPECT_EQ(jobs[0].reader_count, 2);
  EXPECT_EQ(jobs[0].readers.size(), 2u);
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_capture_plan_skips_reader_on_disabled_row_when_unread)
{
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *owner = add_layer_with_image("Owner", add_image("Owner", 4, 0, 0, 255));
  ASSERT_NE(point, nullptr);
  ASSERT_NE(add_reader(*owner, PaintLayerAnchorPointList::Effects, point->marker), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, owner, false));

  Vector<PaintLayersGpuCaptureJob> jobs;
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  EXPECT_TRUE(jobs.is_empty())
      << "a reader on a hidden row that nothing reads must not capture a slot";

  /* Positive control: once the owner row is visible the same reader is captured. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, owner, true));
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  ASSERT_EQ(jobs.size(), 1u);
  EXPECT_TRUE(BLI_uuid_equal(jobs[0].point, point->marker));
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_capture_plan_keeps_disabled_reader_read_by_visible_reader)
{
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  MaterialPaintLayer *inner_point = add_point(*inner);
  MaterialPaintLayer *disabled = add_layer_with_image("Disabled", add_image("Disabled", 4, 0, 0, 255));
  ASSERT_NE(inner_point, nullptr);
  ASSERT_NE(add_reader(*disabled, PaintLayerAnchorPointList::Effects, inner_point->marker), nullptr);
  MaterialPaintLayer *disabled_point = add_point(*disabled);
  ASSERT_NE(disabled_point, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, disabled, false));
  /* A visible reader reads a point on the hidden row: its slice still lays the nested reader. */
  MaterialPaintLayer *top = add_layer_with_image("Top", add_image("Top", 4, 120, 80, 40, 255));
  ASSERT_NE(add_reader(*top, PaintLayerAnchorPointList::Effects, disabled_point->marker), nullptr);

  Vector<PaintLayersGpuCaptureJob> jobs;
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  ASSERT_EQ(jobs.size(), 2u);
  EXPECT_NE(find_job(jobs, disabled_point->marker), nullptr)
      << "the job of the point on the disabled row is collected";
  EXPECT_NE(find_job(jobs, inner_point->marker), nullptr)
      << "the nested reader on the disabled row is laid by the visible reader's slice";
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_capture_plan_empty_mask_slice_still_has_slot)
{
  /* A mask point on a row with no mask base publishes an empty mask stack. It is a factor of one,
   * not a refused slice. */
  MaterialPaintLayer *source = add_layer_with_image("Source", add_image("Source", 4, 255, 0, 0));
  MaterialPaintLayer *point = add_point(*source, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_base(*source), nullptr);
  MaterialPaintLayer *reader_row = add_layer_with_image(
      "Reader", add_image("Reader", 4, 0, 255, 0));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, reader_row, 1.0f), nullptr);
  ASSERT_NE(add_reader(*reader_row, PaintLayerAnchorPointList::MaskStack, point->marker), nullptr);

  Vector<PaintLayersGpuCaptureJob> jobs;
  ASSERT_TRUE(capture_plan_for_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, jobs));
  const PaintLayersGpuCaptureJob *job = find_job(jobs, point->marker);
  ASSERT_NE(job, nullptr) << "an empty mask slice still gets a slot";
  EXPECT_EQ(job->kind, PaintLayersGpuCaptureKind::MaskItem);
  EXPECT_TRUE(job->clear_to_one);
  EXPECT_TRUE(job->entry.mask_items.is_empty());
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_slot_key_uses_channel_enum_not_index)
{
  const bUUID point = BLI_uuid_generate_random();
  /* The combined stack order: Base, Metallic, Roughness, Specular, Normal, AO, Alpha, Emission.
   * Their channel enums are 0, 1, 2, 3, 4, 8, 7, 9: Alpha and Emission are the pair whose stack
   * index (6, 7) differs from their enum. */
  const std::array<PaintLayersGpuSlotKey, 8> slots = {
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_METALLIC},
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_ROUGHNESS},
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_SPECULAR},
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_NORMAL},
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_AO},
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_ALPHA},
      PaintLayersGpuSlotKey{point, PAINT_MATERIAL_CHANNEL_EMISSION},
  };
  /* A reader on Emission finds the Emission slot (index 7), not the Alpha slot (index 6). */
  const Span<PaintLayersGpuSlotKey> slot_view(slots.data(), int64_t(slots.size()));
  EXPECT_EQ(BKE_paint_layers_gpu_slot_find(slot_view, point, PAINT_MATERIAL_CHANNEL_EMISSION), 7);
  EXPECT_EQ(slots[7].channel, int(PAINT_MATERIAL_CHANNEL_EMISSION));
  EXPECT_EQ(slots[6].channel, int(PAINT_MATERIAL_CHANNEL_ALPHA));
  EXPECT_NE(slots[6].channel, slots[7].channel);
  /* A reader on AO finds the AO slot at stack index 5, whose enum is 8. */
  EXPECT_EQ(BKE_paint_layers_gpu_slot_find(slot_view, point, PAINT_MATERIAL_CHANNEL_AO), 5);
  EXPECT_EQ(slots[5].channel, int(PAINT_MATERIAL_CHANNEL_AO));
  EXPECT_EQ(BKE_paint_layers_gpu_slot_find(slot_view, point, PAINT_MATERIAL_CHANNEL_ALPHA), 6);
  EXPECT_EQ(BKE_paint_layers_gpu_slot_find(slot_view, point, PAINT_MATERIAL_CHANNEL_BASE_COLOR), 0);
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_slot_allocation_failure_releases_allocated_textures)
{
  struct Recorder {
    int released = 0;
    void *last = nullptr;
  };
  Recorder recorder;
  const auto release = [](void *data, void *resource) {
    Recorder *count = static_cast<Recorder *>(data);
    count->released++;
    count->last = resource;
  };
  {
    PaintLayersGpuSlotGuard guard;
    guard.set_release(release, &recorder);
    guard.own(reinterpret_cast<void *>(uintptr_t(0x1)));
    EXPECT_EQ(guard.size(), 1);
    /* The second allocation failed: it owns nothing, and the first is still released. */
    guard.own(nullptr);
    EXPECT_EQ(guard.size(), 1);
  }
  EXPECT_EQ(recorder.released, 1);
  EXPECT_EQ(recorder.last, reinterpret_cast<void *>(uintptr_t(0x1)));
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_slot_guard_move_releases_each_resource_once)
{
  struct Recorder {
    int released = 0;
  };
  Recorder recorder;
  const auto release = [](void *data, void * /*resource*/) {
    static_cast<Recorder *>(data)->released++;
  };
  Vector<PaintLayersGpuSlotGuard> guards;
  {
    PaintLayersGpuSlotGuard first;
    first.set_release(release, &recorder);
    first.own(reinterpret_cast<void *>(uintptr_t(0x1)));
    first.own(reinterpret_cast<void *>(uintptr_t(0x2)));
    guards.append(std::move(first));
    EXPECT_EQ(first.size(), 0) << "the moved-from guard owns nothing";
  }
  EXPECT_EQ(guards.size(), 1);
  EXPECT_EQ(recorder.released, 0);
  guards.clear();
  EXPECT_EQ(recorder.released, 2) << "exactly one release per resource, not two";
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_unsupported_reason_refuses_baked_coverage_correction_inside_slice)
{
  MaterialPaintLayer *inner = add_layer_with_image("Inner", add_image("Inner", 4, 255, 0, 0));
  MaterialPaintLayer *inner_point = add_point(*inner);
  MaterialPaintLayer *outer = add_layer_with_image("Outer", add_image("Outer", 4, 0, 0, 255));
  ASSERT_NE(inner_point, nullptr);
  MaterialPaintLayer *baked_reader = add_reader(
      *outer, PaintLayerAnchorPointList::Effects, inner_point->marker);
  ASSERT_NE(baked_reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *baked_reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  MaterialPaintLayer *outer_point = add_point(*outer);
  ASSERT_NE(outer_point, nullptr);
  MaterialPaintLayer *reader_row = add_layer_with_image(
      "Reader", add_image("Reader", 4, 0, 255, 128, 255));
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, outer_point->marker);
  ASSERT_NE(reader, nullptr);
  bake_plan();
  ASSERT_NE(baked_reader->bake, nullptr);
  ASSERT_NE(baked_reader->bake->coverage, nullptr);

  PaintMaterialCompositeImageLayer entry;
  ASSERT_TRUE(BKE_paint_layers_slice_describe(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, entry));
  ASSERT_FALSE(entry.content_corrections.is_empty());
  EXPECT_NE(BKE_paint_layers_gpu_unsupported_reason(entry), nullptr)
      << "a baked correction inside the slice covers by a map the GPU pass cannot read";

  /* Positive control: with the correction live the same slice is supported. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *baked_reader, MA_PAINT_LAYER_BAKE_NEVER));
  ASSERT_TRUE(BKE_paint_layers_slice_describe(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, entry));
  EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(entry), nullptr);
}

TEST_F(PaintLayersAnchorPointCompositeTest, mask_item_descriptor_carries_read_channel)
{
  /* The source takes part in Roughness only, so a content point read as a mask must take its
   * coverage from Roughness rather than falling back to Base Color. */
  MaterialPaintLayer *source = BKE_paint_layers_add(*ma, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(source, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, source, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, source, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("Rough", 4, 0, 255, 0, 255)));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = add_layer_with_image(
      "Reader", add_image("Reader", 4, 0, 0, 255));
  ASSERT_NE(point, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, reader_row, 1.0f), nullptr);
  MaterialPaintLayer *mask_reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::MaskStack, point->marker);
  ASSERT_NE(mask_reader, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const PaintMaterialCompositeMaskItem *item = find_mask_reader(layers, mask_reader->marker);
  ASSERT_NE(item, nullptr);
  const int expected = paint_layer_anchor_point_mask_read_channel(*ma, *mask_reader);
  EXPECT_EQ(expected, int(PAINT_MATERIAL_CHANNEL_ROUGHNESS))
      << "the source does not take part in Base Color";
  EXPECT_EQ(item->anchor_point_read_channel, expected);
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_verify_compare_accepts_half_float_rounding)
{
  const float gpu[6] = {0.5f + 0.0002f, 0.25f, 1.0f, 0.1f, 0.5f + 0.0004f, 0.9f};
  const float cpu[6] = {0.5f, 0.25f + 0.0001f, 1.0f, 0.1f, 0.5f, 0.9f};
  const PaintLayersGpuVerifyDiff diff = BKE_paint_layers_gpu_verify_compare_slot(
      gpu, cpu, 2, 3, 1e-2f);
  EXPECT_TRUE(diff.ok);
  EXPECT_EQ(diff.samples, 2);
  EXPECT_NEAR(diff.max_diff, 0.0004f, 1e-6f);
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_verify_compare_reports_mismatch_with_max_diff)
{
  const float gpu[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
  const float cpu[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.8f, 0.5f};
  const PaintLayersGpuVerifyDiff diff = BKE_paint_layers_gpu_verify_compare_slot(
      gpu, cpu, 2, 3, 1e-2f);
  EXPECT_FALSE(diff.ok);
  EXPECT_NEAR(diff.max_diff, 0.3f, 1e-6f);
  EXPECT_NEAR(diff.mean_diff, 0.15f, 1e-6f) << "judged by the mean of each sample's worst component";
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_verify_compare_tolerates_one_edge_sample)
{
  /* A lone sample off by a lot (the edge of a stroke, sampled bilinearly on the GPU) must not fail a
   * slot that is right everywhere else. */
  float gpu[300];
  float cpu[300];
  for (int i = 0; i < 300; i++) {
    gpu[i] = cpu[i] = 0.5f;
  }
  gpu[0] = 1.0f;
  const PaintLayersGpuVerifyDiff diff = BKE_paint_layers_gpu_verify_compare_slot(
      gpu, cpu, 100, 3, 1e-2f);
  EXPECT_TRUE(diff.ok);
  EXPECT_NEAR(diff.max_diff, 0.5f, 1e-6f);
}

TEST_F(PaintLayersAnchorPointCompositeTest, gpu_verify_compare_fails_on_nan)
{
  float gpu[3] = {0.5f, std::numeric_limits<float>::quiet_NaN(), 0.5f};
  const float cpu[3] = {0.5f, 0.5f, 0.5f};
  const PaintLayersGpuVerifyDiff diff = BKE_paint_layers_gpu_verify_compare_slot(
      gpu, cpu, 1, 3, 1e-2f);
  EXPECT_FALSE(diff.ok);
  EXPECT_TRUE(diff.has_nan);
}

/** \} */

/** \} */

}  // namespace blender::bke::tests
