/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"

#include <array>
#include <cstring>
#include <utility>

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Evaluator
 * \{ */

class PaintMaterialCompositeEvalTest : public bke::BlenderGTestBase {
 public:
  static constexpr int size = 4;
  Vector<ImBuf *> owned_buffers;

  void TearDown() override
  {
    for (ImBuf *ibuf : owned_buffers) {
      IMB_freeImBuf(ibuf);
    }
    owned_buffers.clear();
  }

  ImBuf *add_buffer(const uchar r, const uchar g, const uchar b, const uchar a)
  {
    ImBuf *ibuf = IMB_allocImBuf(uint(size), uint(size), ImBufFlags::ByteData);
    ibuf->channels = 4;
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      pixels[i * 4 + 0] = r;
      pixels[i * 4 + 1] = g;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = a;
    }
    owned_buffers.append(ibuf);
    return ibuf;
  }

  static const uchar *pixel(const ImBuf &ibuf, const int x, const int y)
  {
    return ibuf.byte_data() + (int64_t(y) * ibuf.x + x) * 4;
  }

  /** Mark \a ibuf as data: a normal map is not a color, and must not be decoded as one. */
  static void make_data(ImBuf *ibuf)
  {
    IMB_colormanagement_assign_byte_colorspace(
        ibuf, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA));
  }

  /** The scene-linear composite at (x, y): the evaluator's own output, before encoding. */
  static std::array<float, 4> linear_pixel(const PaintMaterialCompositeStack &stack,
                                           const int x,
                                           const int y)
  {
    Vector<float> linear(int64_t(stack.width) * stack.height * 4);
    EXPECT_TRUE(BKE_paint_material_composite_eval_linear(stack, linear.data()));
    const float *p = linear.data() + (int64_t(y) * stack.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
  }
};

TEST_F(PaintMaterialCompositeEvalTest, single_layer_is_copied_not_blended)
{
  /* A lone layer has nothing under it, so its alpha must survive rather than being composited
   * over whatever the destination buffer happened to contain. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = add_buffer(200, 100, 50, 128);
  stack.layers.append(layer);

  ImBuf *composite = add_buffer(0, 0, 0, 255);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  const uchar *result = pixel(*composite, 1, 1);
  EXPECT_EQ(result[0], 200);
  EXPECT_EQ(result[3], 128);
}

TEST_F(PaintMaterialCompositeEvalTest, opaque_mix_layer_replaces_what_is_below)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(0, 0, 0, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  const uchar *result = pixel(*composite, 0, 0);
  EXPECT_EQ(result[0], 255);
  EXPECT_EQ(result[3], 255);
}

TEST_F(PaintMaterialCompositeEvalTest, opacity_scales_the_layer_coverage)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(0, 0, 0, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.opacity = 0.5f;
  stack.layers.append(top);

  /* In scene linear a half factor between black and white is exactly half, whatever the byte
   * encoding does to it on the way out. */
  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 0.5f, 1e-4f);
}

TEST_F(PaintMaterialCompositeEvalTest, zero_opacity_layer_leaves_the_stack_unchanged)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(10, 20, 30, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.opacity = 0.0f;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  const uchar *result = pixel(*composite, 0, 0);
  EXPECT_EQ(result[0], 10);
  EXPECT_EQ(result[1], 20);
}

TEST_F(PaintMaterialCompositeEvalTest, disabled_layer_is_skipped)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(10, 20, 30, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.enabled = false;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 10);
}

TEST_F(PaintMaterialCompositeEvalTest, multiply_blend_darkens)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(128, 128, 128, 255);
  top.blend = CompositeBlend::Multiply;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_LT(pixel(*composite, 0, 0)[0], 200);
}

TEST_F(PaintMaterialCompositeEvalTest, mask_modulates_opacity_per_pixel)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(0, 0, 0, 255);
  stack.layers.append(bottom);

  ImBuf *mask = add_buffer(255, 255, 255, 255);
  /* Black out one pixel of the mask; the layer must not reach the composite there. */
  uchar *masked = mask->byte_data_for_write();
  masked[0] = masked[1] = masked[2] = 0;

  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.mask_ibuf = mask;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 0);
  EXPECT_EQ(pixel(*composite, 1, 0)[0], 255);
}

TEST_F(PaintMaterialCompositeEvalTest, top_alpha_is_not_coverage)
{
  /* The Mix node interpolates by the factor alone. A fully transparent top layer at factor 1
   * therefore replaces what is below, and a compositor that treated its alpha as coverage would
   * show the bottom layer where the render shows the top one. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(10, 10, 10, 0);
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  const uchar *result = pixel(*composite, 0, 0);
  EXPECT_EQ(result[0], 10);
  EXPECT_EQ(result[3], 0);
}

TEST_F(PaintMaterialCompositeEvalTest, mask_from_alpha_reads_alpha_not_luminance)
{
  /* The layer stack case: the factor comes from the layer's own Alpha output. Its colour is
   * black, so a mask read as luminance would hide the layer entirely. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  stack.layers.append(bottom);

  ImBuf *black_but_opaque = add_buffer(0, 0, 0, 255);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = black_but_opaque;
  top.mask_ibuf = black_but_opaque;
  top.mask_from_alpha = true;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 0);

  /* The same mask read as colour leaves the bottom layer showing through. */
  stack.layers[1].mask_from_alpha = false;
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 200);
}

TEST_F(PaintMaterialCompositeEvalTest, multiply_keeps_the_bottom_alpha)
{
  /* `node_mix_mult` and friends write `outcol.a = col1.a`; only Mix carries the top's alpha. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(128, 128, 128, 0);
  top.blend = CompositeBlend::Multiply;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_EQ(pixel(*composite, 0, 0)[3], 255);
}

TEST_F(PaintMaterialCompositeEvalTest, normal_combine_lays_relief_over_relief)
{
  /* Two normal maps tilted the same way along X. Combining them must tilt further than either,
   * where a plain Mix would average them back towards the base. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(160, 128, 255, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(160, 128, 255, 255);
  make_data(top.color_ibuf);
  top.blend = CompositeBlend::NormalCombine;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  const uchar *result = pixel(*composite, 0, 0);
  EXPECT_GT(result[0], 160);
  /* Still a unit normal: the encoded Z has to drop as X grows. */
  EXPECT_LT(result[2], 255);
}

TEST_F(PaintMaterialCompositeEvalTest, normal_combine_over_a_flat_normal_keeps_the_detail)
{
  /* The flat map is the identity of this operation, which is what makes an unpainted layer
   * invisible in the composited normal pass. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(128, 128, 255, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  /* A unit-length detail normal: the whiteout renormalizes, so a non-unit encoded normal is not
   * the identity the test is about. Blue 232 decodes z to sqrt(1 - x^2 - y^2). */
  top.color_ibuf = add_buffer(200, 128, 232, 255);
  make_data(top.color_ibuf);
  top.blend = CompositeBlend::NormalCombine;
  stack.layers.append(top);

  /* Whiteout on the encoded data: the flat base is the identity, so the detail passes through. */
  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 200.0f / 255.0f, 0.01f);
}

TEST_F(PaintMaterialCompositeEvalTest, mask_influence_zero_ignores_the_mask)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(0, 0, 0, 255);
  stack.layers.append(bottom);

  ImBuf *mask = add_buffer(0, 0, 0, 255);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.mask_ibuf = mask;
  top.mask_influence = 0.0f;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 255);
}

TEST_F(PaintMaterialCompositeEvalTest, region_leaves_the_rest_of_the_buffer_alone)
{
  /* The whole point of the region: a stroke recomputes only what it touched, and everything else
   * keeps the pixels of the previous evaluation. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = add_buffer(255, 255, 255, 255);
  stack.layers.append(layer);

  ImBuf *composite = add_buffer(7, 7, 7, 255);
  rcti region;
  BLI_rcti_init(&region, 0, 2, 0, 2);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite, &region));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 255);
  EXPECT_EQ(pixel(*composite, 1, 1)[0], 255);
  EXPECT_EQ(pixel(*composite, 3, 3)[0], 7);
  EXPECT_EQ(pixel(*composite, 2, 0)[0], 7);
}

TEST_F(PaintMaterialCompositeEvalTest, region_outside_the_buffer_is_a_no_op)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = add_buffer(255, 255, 255, 255);
  stack.layers.append(layer);

  ImBuf *composite = add_buffer(7, 7, 7, 255);
  rcti region;
  BLI_rcti_init(&region, 100, 110, 100, 110);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite, &region));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 7);
}

TEST_F(PaintMaterialCompositeEvalTest, layer_of_a_different_size_is_rejected)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = IMB_allocImBuf(uint(size * 2), uint(size), ImBufFlags::ByteData);
  owned_buffers.append(layer.color_ibuf);
  stack.layers.append(layer);

  ImBuf *composite = add_buffer(0, 0, 0, 255);
  EXPECT_FALSE(BKE_paint_material_composite_eval(stack, composite));
}

TEST_F(PaintMaterialCompositeEvalTest, premultiplied_float_layer_is_straightened)
{
  /* A float buffer is premultiplied: half alpha on a 0.25 linear colour is stored as 0.125 in
   * RGB, and the evaluator has to straighten it before mixing. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = IMB_allocImBuf(uint(size), uint(size), ImBufFlags::FloatData);
  owned_buffers.append(layer.color_ibuf);
  layer.color_ibuf->channels = 4;
  float *pixels = layer.color_ibuf->float_data_for_write();
  for (int64_t i = 0; i < int64_t(size) * size; i++) {
    pixels[i * 4 + 0] = 0.125f;
    pixels[i * 4 + 1] = 0.125f;
    pixels[i * 4 + 2] = 0.125f;
    pixels[i * 4 + 3] = 0.5f;
  }
  stack.layers.append(layer);

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 0.25f, 1e-5f);
  EXPECT_NEAR(result[1], 0.25f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, add_is_not_clamped_in_linear)
{
  /* Add is meant to exceed one in the shader; the clamp belongs to the byte encode, not the mix. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(200, 200, 200, 255);
  make_data(top.color_ibuf);
  top.blend = CompositeBlend::Add;
  stack.layers.append(top);

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], (200.0f / 255.0f) * 2.0f, 1e-4f);
}

TEST_F(PaintMaterialCompositeEvalTest, srgb_mask_color_is_decoded)
{
  /* A mask read as a colour -- not as alpha -- is a data map; used here as sRGB it still has to be
   * decoded, or the CPU would apply a brighter factor than the shader. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(0, 0, 0, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.mask_ibuf = add_buffer(128, 128, 128, 255);
  stack.layers.append(top);

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  /* sRGB 128 decodes to a value below the raw 128/255; an undecoded mask would sit above it. */
  EXPECT_LT(result[0], 128.0f / 255.0f);
  EXPECT_GT(result[0], 0.1f);
}

TEST_F(PaintMaterialCompositeEvalTest, region_refresh_matches_the_full_evaluation)
{
  /* A region that straddles a tile edge has to recompute exactly what the full pass did; the tiled
   * core must not let a tile boundary show in the pixels. */
  const int big = 300;
  PaintMaterialCompositeStack stack;
  stack.width = big;
  stack.height = big;
  const float bottom_color[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  const float top_color[4] = {0.75f, 0.75f, 0.75f, 1.0f};
  PaintMaterialCompositeLayer bottom;
  bottom.has_constant_color = true;
  copy_v4_v4(bottom.constant_color, bottom_color);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.has_constant_color = true;
  top.opacity = 0.5f;
  copy_v4_v4(top.constant_color, top_color);
  stack.layers.append(top);

  Vector<float> full(int64_t(big) * big * 4);
  ASSERT_TRUE(BKE_paint_material_composite_eval_linear(stack, full.data()));

  const float sentinel = -1.0f;
  Vector<float> partial(int64_t(big) * big * 4, sentinel);
  rcti region;
  BLI_rcti_init(&region, 250, 300, 250, 300);
  ASSERT_TRUE(BKE_paint_material_composite_eval_linear(stack, partial.data(), &region));

  for (int y = region.ymin; y < region.ymax; y++) {
    for (int x = region.xmin; x < region.xmax; x++) {
      const int64_t offset = (int64_t(y) * big + x) * 4;
      EXPECT_FLOAT_EQ(partial[offset], full[offset]) << x << "," << y;
    }
  }
  EXPECT_FLOAT_EQ(partial[0], sentinel);
}

TEST_F(PaintMaterialCompositeEvalTest, empty_stack_is_rejected)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  ImBuf *composite = add_buffer(0, 0, 0, 255);
  EXPECT_FALSE(BKE_paint_material_composite_eval(stack, composite));
}

TEST_F(PaintMaterialCompositeEvalTest, content_correction_over_absent_base)
{
  /* A layer with no map of its own paints through its corrections: a transparent base that the
   * correction brings its own coverage into (spec 18 §5.3). */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = nullptr; /* Absent base */
  layer.mask_from_alpha = true;
  PaintMaterialCompositeCorrectionBuffer corr;
  corr.ibuf = add_buffer(255, 0, 0, 255);
  layer.content_corrections.append(corr);
  stack.layers.append(layer);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  ASSERT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  const uchar *result = pixel(*composite, 2, 2);
  EXPECT_EQ(result[0], 255); /* C = mix(0, red, 1) */
  EXPECT_EQ(result[3], 255); /* a = 0 + 1 * (1 - 0) */
}

TEST_F(PaintMaterialCompositeEvalTest, muted_correction_changes_nothing)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = add_buffer(0, 0, 255, 255);
  layer.mask_ibuf = layer.color_ibuf;
  layer.mask_from_alpha = true;
  PaintMaterialCompositeCorrectionBuffer corr;
  corr.ibuf = add_buffer(255, 0, 0, 255);
  corr.enabled = false;
  layer.content_corrections.append(corr);
  stack.layers.append(layer);
  ImBuf *composite = add_buffer(0, 0, 0, 0);
  ASSERT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_EQ(pixel(*composite, 0, 0)[2], 255);
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 0);
}

TEST_F(PaintMaterialCompositeEvalTest, mask_correction_multiplies_coverage)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer below;
  below.color_ibuf = add_buffer(0, 0, 0, 255);
  below.is_bare_base = true;
  stack.layers.append(below);
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = add_buffer(255, 255, 255, 255);
  layer.mask_ibuf = layer.color_ibuf;
  layer.mask_from_alpha = true;
  PaintMaterialCompositeCorrectionBuffer mask_corr;
  mask_corr.ibuf = add_buffer(0, 0, 0, 255); /* black, fully covering */
  mask_corr.blend = CompositeBlend::Multiply;
  layer.mask_corrections.append(mask_corr);
  stack.layers.append(layer);
  ImBuf *composite = add_buffer(0, 0, 0, 0);
  ASSERT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  /* MULTIPLY: F * (1 - 1) + F * 0 * 1 = 0: the layer is hidden where its item is black. */
  EXPECT_EQ(pixel(*composite, 1, 1)[0], 0);
}

TEST_F(PaintMaterialCompositeEvalTest, mask_item_mix_and_multiply_over_the_factor)
{
  /* F = 0.8 (the layer's own mask), C = 0.5, A = 1, op = 1. MIX replaces the factor with C: 0.5.
   * MULTIPLY darkens it by C: F * (1 - 1) + F * 0.5 * 1 = 0.4. */
  const auto evaluate = [&](const CompositeBlend blend) {
    PaintMaterialCompositeStack stack;
    stack.width = size;
    stack.height = size;
    PaintMaterialCompositeLayer bottom;
    bottom.color_ibuf = add_buffer(0, 0, 0, 255);
    stack.layers.append(bottom);
    PaintMaterialCompositeLayer layer;
    layer.color_ibuf = add_buffer(255, 255, 255, 255);
    ImBuf *mask = add_buffer(204, 204, 204, 255); /* 0.8 grey */
    make_data(mask);
    layer.mask_ibuf = mask;
    layer.mask_reads_grey = true;
    layer.mask_influence = 1.0f;
    PaintMaterialCompositeCorrectionBuffer correction;
    correction.has_constant_color = true;
    correction.constant_color[0] = correction.constant_color[1] = correction.constant_color[2] =
        0.5f;
    correction.constant_color[3] = 1.0f;
    correction.blend = blend;
    layer.mask_corrections.append(correction);
    stack.layers.append(layer);
    return linear_pixel(stack, 1, 1);
  };

  EXPECT_NEAR(evaluate(CompositeBlend::Mix)[0], 0.5f, 1e-4f);
  EXPECT_NEAR(evaluate(CompositeBlend::Multiply)[0], 0.4f, 1e-4f);
}

TEST_F(PaintMaterialCompositeEvalTest, mask_item_multiply_with_a_straight_map)
{
  /* F = 0.8, map stored straight as C = 0.5, A = 0.5, op = 1 (the one convention for every
   * paint-layer map). The texture upload pre-multiplies the straight bytes and the generator's
   * Divide recovers C, so both sides apply `F * (1 - A * op) + (F * C) * (A * op)`:
   * 0.8 * 0.5 + 0.8 * 0.5 * 0.5 = 0.6. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(0, 0, 0, 255);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = add_buffer(255, 255, 255, 255);
  ImBuf *mask = add_buffer(204, 204, 204, 255);
  make_data(mask);
  layer.mask_ibuf = mask;
  layer.mask_reads_grey = true;
  layer.mask_influence = 1.0f;
  PaintMaterialCompositeCorrectionBuffer correction;
  ImBuf *correction_map = add_buffer(128, 128, 128, 128); /* straight C = 0.5, A = 0.5 */
  make_data(correction_map);
  correction.ibuf = correction_map;
  correction.blend = CompositeBlend::Multiply;
  layer.mask_corrections.append(correction);
  stack.layers.append(layer);

  EXPECT_NEAR(linear_pixel(stack, 1, 1)[0], 0.6f, 5e-3f);
}

TEST_F(PaintMaterialCompositeEvalTest, region_update_matches_full_with_corrections)
{
  ImBuf *corr_ibuf = add_buffer(0, 255, 0, 128);
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.mask_from_alpha = true;
  PaintMaterialCompositeCorrectionBuffer corr;
  corr.ibuf = corr_ibuf;
  corr.opacity = 0.5f;
  layer.content_corrections.append(corr);
  stack.layers.append(layer);

  ImBuf *partial = add_buffer(0, 0, 0, 0);
  ASSERT_TRUE(BKE_paint_material_composite_eval(stack, partial));

  /* A stroke touches one pixel of the correction; only its rectangle is refreshed. */
  uchar *edited = corr_ibuf->byte_data_for_write() + (int64_t(1) * size + 1) * 4;
  edited[0] = 255;
  edited[3] = 255;
  rcti region;
  BLI_rcti_init(&region, 1, 2, 1, 2);
  ASSERT_TRUE(BKE_paint_material_composite_eval(stack, partial, &region));

  ImBuf *reference = add_buffer(0, 0, 0, 0);
  ASSERT_TRUE(BKE_paint_material_composite_eval(stack, reference));
  EXPECT_EQ(memcmp(partial->byte_data(), reference->byte_data(), size_t(size) * size * 4), 0);
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Display Passes
 *
 * The role list and the display list are maintained by hand and are deliberately different; these
 * pin the difference rather than leaving it to be discovered when a selector shows the wrong thing.
 * \{ */

TEST(paint_material_composite, pass_list_covers_every_shading_channel)
{
  const Span<int> passes = BKE_paint_material_composite_passes();
  /* Specular and Emission are shading inputs of the Combined preview and must be inspectable on
   * their own, like every other channel a user can paint. */
  EXPECT_TRUE(passes.contains(PAINT_MATERIAL_CHANNEL_SPECULAR));
  EXPECT_TRUE(passes.contains(PAINT_MATERIAL_CHANNEL_EMISSION));
  /* Height has no part in phase 1 shading; listing it would offer a pass the preview ignores. */
  EXPECT_FALSE(passes.contains(PAINT_MATERIAL_CHANNEL_HEIGHT));
  /* Combined is a display mode, not a role: every consumer of this list indexes an array by the
   * value or looks it up in the channel descriptor table. */
  EXPECT_FALSE(passes.contains(PAINT_LAYER_PASS_COMBINED));
}

TEST(paint_material_composite, display_passes_lead_with_combined)
{
  const Span<int> display = BKE_paint_material_display_passes();
  const Span<int> roles = BKE_paint_material_composite_passes();
  ASSERT_FALSE(display.is_empty());
  EXPECT_EQ(display.first(), PAINT_LAYER_PASS_COMBINED);
  EXPECT_EQ(display.size(), roles.size() + 1);
  for (const int role : roles) {
    EXPECT_TRUE(display.contains(role));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Description -> CPU composite
 * \{ */

class PaintLayersCompositeTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
    ma = BKE_material_add(bmain, "Layered");
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Image *add_solid_image(const char *name,
                         const int size,
                         const uchar r,
                         const uchar g,
                         const uchar b,
                         const uchar a,
                         const bool is_data = false)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, is_data, false);
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

  MaterialPaintLayer *add_paint_layer(const char *name, Image *image)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  }

  MaterialPaintLayer *set_mask_value(MaterialPaintLayer &layer, const float value)
  {
    return BKE_paint_layers_mask_add(*ma, &layer, value);
  }

  MaterialPaintLayer *set_mask_image(MaterialPaintLayer &layer,
                                     Image *image,
                                     const bool enabled = true)
  {
    /* A mask is a stack item now: its map lives in the item's Base-Color channel record. */
    MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, &layer, 1.0f);
    EXPECT_NE(item, nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_add(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(
        *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
    BKE_paint_layers_correction_source_set(*ma, item, MA_PAINT_LAYER_SOURCE_IMAGE);
    EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, item, enabled));
    return item;
  }

  MaterialPaintLayer *add_content_correction(MaterialPaintLayer &owner,
                                             const char *name,
                                             Image *image,
                                             const float opacity)
  {
    MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
        *ma, &owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, name);
    EXPECT_NE(correction, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, opacity));
    return correction;
  }

  MaterialPaintLayer *add_mask_correction(MaterialPaintLayer &owner,
                                          const char *name,
                                          Image *image,
                                          const float opacity)
  {
    MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
        *ma, &owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, name);
    EXPECT_NE(correction, nullptr);
    /* A mask item is itself the row: its map lives in its Base-Color channel record. */
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, opacity));
    return correction;
  }

  MaterialPaintLayer *add_fill_layer(const char *name, const float color[4])
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    copy_v4_v4(layer->fill_color, color);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    record->image = nullptr;
    return layer;
  }

  ImBuf *composite(const int channel, int &r_width, int &r_height)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
    EXPECT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, r_width, r_height));
    ImBuf *ibuf = IMB_allocImBuf(uint(r_width), uint(r_height), ImBufFlags::ByteData);
    ibuf->channels = 4;
    EXPECT_TRUE(BKE_paint_layers_composite_channel(*ma, channel, *ibuf));
    return ibuf;
  }

  static const uchar *pixel(const ImBuf &ibuf, const int x, const int y)
  {
    return ibuf.byte_data() + (int64_t(y) * ibuf.x + x) * 4;
  }

  /** The scene-linear composite at (x, y): the evaluator's own output, before encoding. */
  std::array<float, 4> linear_pixel(const int channel, const int x, const int y)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
    int width = 0, height = 0;
    EXPECT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, width, height));
    Vector<float> linear(int64_t(width) * height * 4);
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom);
    EXPECT_TRUE(BKE_paint_material_composite_eval_images_linear(
        layers, linear.data(), nullptr, nullptr, bottom));
    const float *p = linear.data() + (int64_t(y) * width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
  }
};

TEST_F(PaintLayersCompositeTest, two_paint_layers_mix_top_over_bottom)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  ASSERT_EQ(w, 4);
  ASSERT_EQ(h, 4);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 255);
  EXPECT_EQ(p[3], 255);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, opacity_blends_top_over_bottom)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  BKE_paint_layers_set_opacity(*ma, top, 0.5f);

  /* Red and blue are one in either encoding, so the half mix is exactly half in scene linear. */
  const std::array<float, 4> result = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
  EXPECT_NEAR(result[0], 0.5f, 1e-4f);
  EXPECT_NEAR(result[1], 0.0f, 1e-4f);
  EXPECT_NEAR(result[2], 0.5f, 1e-4f);
}

TEST_F(PaintLayersCompositeTest, disabled_layer_contributes_nothing)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  BKE_paint_layers_set_enabled(*ma, top, false);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, multiply_mode_matches_the_mix_node)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 255, 0, 255));
  BKE_paint_layers_set_blend(*ma, top, MA_PAINT_LAYER_BLEND_MULTIPLY);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  /* Bottom red times top green is black. */
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, fill_only_channel_has_no_dimensions)
{
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_fill_layer("Fill", blue);

  /* The builder expresses the row as a constant, but nothing in the stack can size a buffer. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_TRUE(layers[0].has_constant_color);

  ImBuf *ibuf = IMB_allocImBuf(4, 4, ImBufFlags::ByteData);
  ibuf->channels = 4;
  EXPECT_FALSE(BKE_paint_layers_composite_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *ibuf));
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, fill_constant_below_paint_does_not_stop_it)
{
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_fill_layer("Fill", blue);
  add_paint_layer("Top", add_solid_image("Top", 4, 255, 0, 0, 255));

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, fill_constant_above_paint_covers_it)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_fill_layer("Fill", blue);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 255);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, disabled_fill_contributes_nothing)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  MaterialPaintLayer *fill = add_fill_layer("Fill", blue);
  BKE_paint_layers_set_enabled(*ma, fill, false);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, fill_effect_correction_replaces_the_row_colour)
{
  MaterialPaintLayer *bottom = add_paint_layer(
      "Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, correction, green));

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 255);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, content_correction_replaces_the_row_colour)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  add_content_correction(*top, "C", add_solid_image("Correction", 4, 0, 255, 0, 255), 1.0f);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 255);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, content_correction_opacity_blends)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  add_content_correction(*top, "C", add_solid_image("Correction", 4, 0, 255, 0, 255), 0.5f);

  /* Green at half over blue is half green and half blue in scene linear, and the layer then
   * replaces the red below it at full coverage. */
  const std::array<float, 4> result = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
  EXPECT_NEAR(result[0], 0.0f, 1e-4f);
  EXPECT_NEAR(result[1], 0.5f, 1e-4f);
  EXPECT_NEAR(result[2], 0.5f, 1e-4f);
}

TEST_F(PaintLayersCompositeTest, mask_correction_hides_the_row)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  /* A black mask correction, opaque: the row's factor becomes zero. */
  add_mask_correction(*top, "M", add_solid_image("MaskCorr", 4, 0, 0, 0, 255), 1.0f);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, mask_correction_gray_scales_the_row)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  /* A mid-grey mask correction halves the row's coverage. Data, like every mask. */
  add_mask_correction(*top, "M", add_solid_image("MaskCorr", 4, 128, 128, 128, 255, true), 1.0f);

  /* 128/255 of the layer stays: a linear mix of the red bottom and the blue top. */
  const float factor = 128.0f / 255.0f;
  const std::array<float, 4> result = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
  EXPECT_NEAR(result[0], 1.0f - factor, 1e-4f);
  EXPECT_NEAR(result[1], 0.0f, 1e-4f);
  EXPECT_NEAR(result[2], factor, 1e-4f);
}

TEST_F(PaintLayersCompositeTest, mask_correction_transparent_changes_nothing)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  /* A fully transparent map carries no coverage: F = 1 * (1 - 0) + 0 = 1, the row stays. */
  add_mask_correction(*top, "M", add_solid_image("MaskCorr", 4, 0, 0, 0, 0), 1.0f);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 255);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, constant_mask_limits_coverage)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  set_mask_value(*top, 0.5f);

  const std::array<float, 4> result = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
  EXPECT_NEAR(result[0], 0.5f, 1e-4f);
  EXPECT_NEAR(result[1], 0.0f, 1e-4f);
  EXPECT_NEAR(result[2], 0.5f, 1e-4f);
}

TEST_F(PaintLayersCompositeTest, black_mask_image_hides_the_row)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  /* A black mask hides the row entirely: a mask is read by its grey, the way it is painted. */
  set_mask_image(*top, add_solid_image("Mask", 4, 0, 0, 0, 255));

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, opaque_mask_image_keeps_the_row)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  set_mask_image(*top, add_solid_image("Mask", 4, 255, 255, 255, 255));

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 255);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, disabled_mask_is_ignored)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  set_mask_image(*top, add_solid_image("Mask", 4, 0, 0, 0, 255), false);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 255);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, blend_mode_is_the_generated_mix_mode)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));

  /* Every mode the description offers maps to the Mix node's own ramp code, and the node clamps
   * only its factor, never its result -- the shader's `ramp_blend`. */
  const struct {
    eMaterialPaintLayerBlend blend;
    int ramp;
  } cases[] = {
      {MA_PAINT_LAYER_BLEND_MIX, MA_RAMP_BLEND},
      {MA_PAINT_LAYER_BLEND_MULTIPLY, MA_RAMP_MULT},
      {MA_PAINT_LAYER_BLEND_OVERLAY, MA_RAMP_OVERLAY},
      {MA_PAINT_LAYER_BLEND_ADD, MA_RAMP_ADD},
      {MA_PAINT_LAYER_BLEND_DARKEN, MA_RAMP_DARK},
      {MA_PAINT_LAYER_BLEND_BURN, MA_RAMP_BURN},
      {MA_PAINT_LAYER_BLEND_LIGHTEN, MA_RAMP_LIGHT},
      {MA_PAINT_LAYER_BLEND_SCREEN, MA_RAMP_SCREEN},
      {MA_PAINT_LAYER_BLEND_DODGE, MA_RAMP_DODGE},
      {MA_PAINT_LAYER_BLEND_SUBTRACT, MA_RAMP_SUB},
      {MA_PAINT_LAYER_BLEND_DIVIDE, MA_RAMP_DIV},
      {MA_PAINT_LAYER_BLEND_DIFFERENCE, MA_RAMP_DIFF},
      {MA_PAINT_LAYER_BLEND_EXCLUSION, MA_RAMP_EXCLUSION},
      {MA_PAINT_LAYER_BLEND_SOFT_LIGHT, MA_RAMP_SOFT},
      {MA_PAINT_LAYER_BLEND_LINEAR_LIGHT, MA_RAMP_LINEAR},
      {MA_PAINT_LAYER_BLEND_HUE, MA_RAMP_HUE},
      {MA_PAINT_LAYER_BLEND_SATURATION, MA_RAMP_SAT},
      {MA_PAINT_LAYER_BLEND_COLOR, MA_RAMP_COLOR},
      {MA_PAINT_LAYER_BLEND_VALUE, MA_RAMP_VAL},
  };
  for (const auto &expect : cases) {
    BKE_paint_layers_set_blend(*ma, top, expect.blend);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    bool found = false;
    /* Every row now builds in its own group, so look for the Mix recursively. */
    auto scan = [&](auto &self, bNodeTree &t) -> void {
      for (bNode &node : t.nodes) {
        if (node.type_legacy == SH_NODE_MIX) {
          const NodeShaderMix &storage = *static_cast<const NodeShaderMix *>(node.storage);
          if (storage.blend_type == expect.ramp && storage.clamp_factor &&
              !storage.clamp_result)
          {
            found = true;
            return;
          }
        }
        if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
          self(self, *reinterpret_cast<bNodeTree *>(node.id));
          if (found) {
            return;
          }
        }
      }
    };
    scan(scan, *ma->paint_layers_tree);
    EXPECT_TRUE(found) << "blend " << int(expect.blend) << " -> ramp " << expect.ramp;
  }
}

/** \} */

}  // namespace blender::bke::tests
