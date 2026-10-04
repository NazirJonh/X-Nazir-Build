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
#include "BKE_mesh_maps.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"

#include "paint_layers_intern.hh"

#include "BLI_listbase.h"
#include "BLI_index_range.hh"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"

#include <array>
#include <cmath>
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
  /* Add raises the factor by C (0.8 + 0.5 = 1.3), which the row blend clamps to 1: the white
   * layer shows fully. Subtract lowers it by C. */
  EXPECT_NEAR(evaluate(CompositeBlend::Add)[0], 1.0f, 1e-4f);
  EXPECT_NEAR(evaluate(CompositeBlend::Subtract)[0], 0.3f, 1e-4f);
  /* Darken takes the smaller of F and C, Lighten the larger. */
  EXPECT_NEAR(evaluate(CompositeBlend::Darken)[0], 0.5f, 1e-4f);
  EXPECT_NEAR(evaluate(CompositeBlend::Lighten)[0], 0.8f, 1e-4f);
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
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    /* A Layer-role row keeps Base Color in its record, the single storage location. */
    copy_v4_v4(record->value, color);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    record->image = nullptr;
    return layer;
  }

  /** A 2x2 Non-Color byte map with known corners, row-major bottom to top. */
  Image *add_quad_image(const char *name,
                        const uchar c00[4],
                        const uchar c10[4],
                        const uchar c01[4],
                        const uchar c11[4])
  {
    Image *image = add_solid_image(name, 2, 0, 0, 0, 255, true);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    memcpy(pixels + 0, c00, 4);
    memcpy(pixels + 4, c10, 4);
    memcpy(pixels + 8, c01, 4);
    memcpy(pixels + 12, c11, 4);
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** A 4x4 Non-Color byte map, white left half and black right half. */
  Image *add_halves_image(const char *name)
  {
    Image *image = add_solid_image(name, 4, 0, 0, 0, 255, true);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(4)) {
      for (const int x : IndexRange(4)) {
        uchar *p = pixels + (int64_t(y) * 4 + x) * 4;
        const uchar v = x < 2 ? 255 : 0;
        p[0] = p[1] = p[2] = v;
        p[3] = 255;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
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

TEST_F(PaintLayersCompositeTest, disabled_channel_contributes_nothing)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer(
      "Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  ASSERT_EQ(top->channels_num, 1);
  MaterialPaintLayerChannel *record = &top->channels[0];
  Image *image = record->image;
  ASSERT_NE(image, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
      *ma, top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));

  /* The disabled channel drops out of the CPU composite, so the red bottom shows through. */
  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);

  /* The record and its map are kept, so enabling it back restores the blue layer. */
  EXPECT_EQ(record->state, MA_PAINT_LAYER_CHANNEL_DISABLED);
  EXPECT_EQ(record->image, image);
  ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
      *ma, top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));
  ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[2], 255);
  IMB_freeImBuf(ibuf);
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

namespace {

/** A float Non-Color atlas of \a w x \a h filled per texel from \a fill, alpha one. */
template<typename Fill>
Image *add_atlas(Main &bmain, const char *name, const int w, const int h, Fill fill)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      &bmain, w, h, name, 32, /*floatbuf=*/true, IMA_GENTYPE_BLANK, black, false, true, false);
  EXPECT_NE(image, nullptr);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  EXPECT_NE(ibuf, nullptr);
  if (ibuf != nullptr) {
    EXPECT_NE(ibuf->float_data(), nullptr);
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
  return image;
}

}  // namespace

/** Guard: the CPU's MESH_MAP resample (#composite_resample_mesh_map) — bilinear onto the channel's
 * reference grid, the scalar atlas' R spread to grey, alpha one; revert it and the values drift. */
TEST_F(PaintLayersCompositeTest, mesh_map_row_values_match_the_resample_formula)
{
  /* The Paint map below defines the 4x4 reference grid. */
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 200, 40, 10, 255));

  /* A 2x2 scalar atlas with known R values (row-major), small enough that the outer reference
   * texels read past the atlas edge and exercise the Extend clamp. */
  const float values[2][2] = {{0.1f, 0.3f}, {0.5f, 0.7f}};
  Image *atlas = add_atlas(*bmain, "Atlas", 2, 2, [&](float *p, const int x, const int y) {
    p[0] = values[y][x];
    p[1] = 0.0f;
    p[2] = 0.0f;
    p[3] = 1.0f;
  });
  ASSERT_NE(atlas, nullptr);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  /* The bilinear the resample is specified to do: the reference texel centre `(x + 0.5) / 4`
   * looked up in the atlas at `u * W_atlas - 0.5`, clamped to the edge. */
  auto manual_red = [&](const int x, const int y) {
    const float su = (float(x) + 0.5f) / 4.0f * 2.0f - 0.5f;
    const float sv = (float(y) + 0.5f) / 4.0f * 2.0f - 0.5f;
    const int x1 = int(floorf(su));
    const int y1 = int(floorf(sv));
    const float a = su - float(x1);
    const float b = sv - float(y1);
    auto clamp_texel = [](const int v) {
      return min_ii(max_ii(v, 0), 1);
    };
    const float p11 = values[clamp_texel(y1)][clamp_texel(x1)];
    const float p21 = values[clamp_texel(y1)][clamp_texel(x1 + 1)];
    const float p12 = values[clamp_texel(y1 + 1)][clamp_texel(x1)];
    const float p22 = values[clamp_texel(y1 + 1)][clamp_texel(x1 + 1)];
    return (1.0f - a) * (1.0f - b) * p11 + a * (1.0f - b) * p21 + (1.0f - a) * b * p12 +
           a * b * p22;
  };

  /* The atlas row covers fully, so its grey is the whole result. */
  for (const int px : {0, 1, 2, 3}) {
    const std::array<float, 4> got = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, px, px);
    const float red = manual_red(px, px);
    EXPECT_NEAR(got[0], red, 1e-4f) << px;
    EXPECT_NEAR(got[1], red, 1e-4f) << px;
    EXPECT_NEAR(got[2], red, 1e-4f) << px;
    EXPECT_NEAR(got[3], 1.0f, 1e-4f) << px;
  }
}

/** Guard: the CPU's MESH_MAP mask item reads the atlas R (#mesh_map_mask_reads_red), never the
 * mean of its RGB; revert it and a non-scalar atlas masks by its luminance. */
TEST_F(PaintLayersCompositeTest, mesh_map_mask_item_reads_the_atlas_red)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner",
                                              add_solid_image("Owner", 4, 30, 200, 90, 255));
  const std::array<float, 4> baseline = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);

  /* A non-scalar atlas: R is 1, G is 0, B is 0.5. The coverage is R; the mean would be 0.5. */
  Image *atlas = add_atlas(*bmain, "NormalAtlas", 2, 2, [](float *p, const int /*x*/, const int /*y*/) {
    p[0] = 1.0f;
    p[1] = 0.0f;
    p[2] = 0.5f;
    p[3] = 1.0f;
  });
  ASSERT_NE(atlas, nullptr);

  MaterialPaintLayer *item = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MESH_MAP, "Normal Mask");
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, item, MA_MESH_MAP_NORMAL_WORLD));
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_NORMAL_WORLD, atlas));

  const std::array<float, 4> got = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
  /* R is one: the factor stays 1 and the owner's colour stands untouched. */
  EXPECT_NEAR(got[0], baseline[0], 1e-4f);
  EXPECT_NEAR(got[1], baseline[1], 1e-4f);
  EXPECT_NEAR(got[2], baseline[2], 1e-4f);
  EXPECT_NEAR(got[3], baseline[3], 1e-4f);
}

/** Guard: the CPU acquires the atlas' buffer on demand (#composite_image_acquire, like every
 * painted map) instead of the resolver's old loaded-buffer check; restore that check and an
 * assigned-but-unloaded atlas drops out of the CPU composite. */
TEST_F(PaintLayersCompositeTest, mesh_map_atlas_buffer_is_acquired_on_demand)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 200, 40, 10, 255));

  const float values[2][2] = {{0.1f, 0.3f}, {0.5f, 0.7f}};
  Image *atlas = add_atlas(*bmain, "Atlas", 2, 2, [&](float *p, const int x, const int y) {
    p[0] = values[y][x];
    p[1] = 0.0f;
    p[2] = 0.0f;
    p[3] = 1.0f;
  });
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  /* The same bilinear the resample is specified to do, at the 2x2 atlas. */
  auto manual_red = [&](const int x, const int y) {
    const float su = (float(x) + 0.5f) / 4.0f * 2.0f - 0.5f;
    const float sv = (float(y) + 0.5f) / 4.0f * 2.0f - 0.5f;
    const int x1 = int(floorf(su));
    const int y1 = int(floorf(sv));
    const float a = su - float(x1);
    const float b = sv - float(y1);
    auto clamp_texel = [](const int v) {
      return min_ii(max_ii(v, 0), 1);
    };
    const float p11 = values[clamp_texel(y1)][clamp_texel(x1)];
    const float p21 = values[clamp_texel(y1)][clamp_texel(x1 + 1)];
    const float p12 = values[clamp_texel(y1 + 1)][clamp_texel(x1)];
    const float p22 = values[clamp_texel(y1 + 1)][clamp_texel(x1 + 1)];
    return (1.0f - a) * (1.0f - b) * p11 + a * (1.0f - b) * p21 + (1.0f - a) * b * p12 +
           a * b * p22;
  };

  for (const int px : {0, 1, 2, 3}) {
    const std::array<float, 4> got = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, px, px);
    const float red = manual_red(px, px);
    EXPECT_NEAR(got[0], red, 1e-4f) << px;
    EXPECT_NEAR(got[3], 1.0f, 1e-4f) << px;
  }

  /* Unload the buffer and evaluate again: the stack must not be rejected, the CPU reloads the
   * buffer, and the row reads whatever the re-acquired buffer holds. */
  BKE_image_free_buffers(atlas);
  ASSERT_FALSE(BKE_image_has_loaded_ibuf(atlas));
  const std::array<float, 4> reloaded = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
  EXPECT_TRUE(BKE_image_has_loaded_ibuf(atlas));
  /* The re-acquired atlas holds the generated fill (black): the grey collapses to zero. */
  EXPECT_NEAR(reloaded[0], 0.0f, 1e-4f);
  EXPECT_NEAR(reloaded[3], 1.0f, 1e-4f);
}

/** Stage 1 mapping on the CPU: `uv' = R * (uv * scale) + offset` with Repeat edges, the same
 * transform the generated Mapping node (Point) applies. A 2x2 corner map over a 4x4 reference
 * grid, so a scale of two lands every reference texel exactly on a source texel. */
TEST_F(PaintLayersCompositeTest, mapping_formula_matches_the_shader_node)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const uchar red[4] = {255, 0, 0, 255};
  const uchar green[4] = {0, 255, 0, 255};
  const uchar blue[4] = {0, 0, 255, 255};
  const uchar white[4] = {255, 255, 255, 255};
  Image *map = add_quad_image("Map", red, green, blue, white);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));

  /* Scale two reads each quadrant exactly: bottom-left red, bottom-right green, top-left blue,
   * top-right white. The doubled pattern also tiles: x = 2 reads like x = 0 (Repeat). */
  const float scale2[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, scale2));
  auto at = [&](const int x, const int y) {
    return linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, x, y);
  };
  std::array<float, 4> p = at(0, 0);
  EXPECT_NEAR(p[0], 1.0f, 1e-4f);
  EXPECT_NEAR(p[1], 0.0f, 1e-4f);
  EXPECT_NEAR(p[2], 0.0f, 1e-4f);
  p = at(1, 0);
  EXPECT_NEAR(p[1], 1.0f, 1e-4f);
  p = at(0, 1);
  EXPECT_NEAR(p[2], 1.0f, 1e-4f);
  p = at(1, 1);
  EXPECT_NEAR(p[0], 1.0f, 1e-4f);
  EXPECT_NEAR(p[1], 1.0f, 1e-4f);
  EXPECT_NEAR(p[2], 1.0f, 1e-4f);
  p = at(2, 0);
  EXPECT_NEAR(p[0], 1.0f, 1e-4f);
  EXPECT_NEAR(p[1], 0.0f, 1e-4f);
  p = at(0, 2);
  EXPECT_NEAR(p[0], 1.0f, 1e-4f);
  EXPECT_NEAR(p[1], 0.0f, 1e-4f);

  /* Offset (0.5, 0.5) at unit scale bilinearly mixes the four corners 1:3:3:9. */
  const float one[2] = {1.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, one));
  const float half[2] = {0.5f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, correction, half));
  p = at(0, 0);
  EXPECT_NEAR(p[0], 0.625f, 1e-3f);
  EXPECT_NEAR(p[1], 0.75f, 1e-3f);
  EXPECT_NEAR(p[2], 0.75f, 1e-3f);

  /* Rotation 90 degrees, counter-clockwise like the shader: (0.125, 0.125) reads (0.875, 0.125)
   * wrapped, mixing white/blue/green/red 3:1:9:3 over 16. */
  const float zero[2] = {0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, correction, zero));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, correction, 1.57079632679f));
  p = at(0, 0);
  EXPECT_NEAR(p[0], 0.375f, 1e-3f);
  EXPECT_NEAR(p[1], 0.75f, 1e-3f);
  EXPECT_NEAR(p[2], 0.25f, 1e-3f);

  /* Scale 0.5 zooms in. Pixel (0,0): uv = (0.125, 0.125) -> 0.0625 -> su = 0.0625 * 2 - 0.5 =
   * -0.375, so x0 = -1 (wraps to texel 1) with a = 0.625, the same on y. The weights are
   * red 0.625^2 = 0.390625, green 0.234375, blue 0.234375, white 0.140625, so
   * R = red + white = 0.53125, G = green + white = 0.375, B = blue + white = 0.375. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, correction, 0.0f));
  const float half_scale[2] = {0.5f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, half_scale));
  p = at(0, 0);
  EXPECT_NEAR(p[0], 0.53125f, 1e-3f);
  EXPECT_NEAR(p[1], 0.375f, 1e-3f);
  EXPECT_NEAR(p[2], 0.375f, 1e-3f);

  /* Scale -1 mirrors. Pixel (0,0): uv = -0.125 wraps to 0.875 -> su = 1.75 - 0.5 = 1.25, so x0 = 1
   * with a = 0.25 and the second tap wraps to texel 0, the same on y. The weights are white
   * 0.75^2 = 0.5625, blue 0.1875 (x0 wrapped, y1), green 0.1875 (x1, y0), red 0.0625, so
   * R = 0.625, G = 0.75, B = 0.75. */
  const float mirror[2] = {-1.0f, -1.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, mirror));
  p = at(0, 0);
  EXPECT_NEAR(p[0], 0.625f, 1e-3f);
  EXPECT_NEAR(p[1], 0.75f, 1e-3f);
  EXPECT_NEAR(p[2], 0.75f, 1e-3f);

  /* Order: scale (2, 1), then rotate 90 degrees, then offset (0.25, 0). Pixel (0,0): uv =
   * (0.125, 0.125) -> scaled (0.25, 0.125) -> rotated (-0.125, 0.25) -> offset (0.125, 0.25).
   * su = 0.125 * 2 - 0.5 = -0.25 -> x0 = -1 (texel 1) with a = 0.75; sv = 0.25 * 2 - 0.5 = 0, so
   * only the row y = 0 is read: 0.25 * green + 0.75 * red = (0.75, 0.25, 0). Rotating before
   * scaling, or offsetting before rotating, lands on different texels. */
  const float aniso[2] = {2.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, aniso));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, correction, 1.57079632679f));
  const float side[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, correction, side));
  p = at(0, 0);
  EXPECT_NEAR(p[0], 0.75f, 1e-3f);
  EXPECT_NEAR(p[1], 0.25f, 1e-3f);
  EXPECT_NEAR(p[2], 0.0f, 1e-3f);
}

/** Stage 1 mapping on the CPU: identity values read exactly the unmapped pixels. */
TEST_F(PaintLayersCompositeTest, identity_mapping_reads_unchanged_pixels)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  /* Same size as the reference grid: an unmapped map of another size is rejected by the
   * evaluator, so only an equal-size map can be compared with and without the mapping. */
  Image *map = add_solid_image("Map", 4, 0, 0, 0, 255, true);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(map, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int i : IndexRange(16)) {
      pixels[i * 4 + 0] = uchar(i * 16);
      pixels[i * 4 + 1] = uchar(255 - i * 16);
      pixels[i * 4 + 2] = uchar((i * 37) % 256);
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(map, ibuf, lock);
  }
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  std::array<std::array<float, 4>, 16> before;
  for (const int i : IndexRange(16)) {
    before[i] = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, i % 4, i / 4);
  }
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  for (const int i : IndexRange(16)) {
    const std::array<float, 4> after = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, i % 4, i / 4);
    for (const int k : IndexRange(4)) {
      EXPECT_NEAR(after[k], before[i][k], 1e-4f) << i << " " << k;
    }
  }
}

/** Stage 1 mapping on the CPU: the stack hash follows the normalized mapping values. */
TEST_F(PaintLayersCompositeTest, mapping_values_move_the_stack_hash)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const uchar red[4] = {255, 0, 0, 255};
  const uchar green[4] = {0, 255, 0, 255};
  const uchar blue[4] = {0, 0, 255, 255};
  const uchar white[4] = {255, 255, 255, 255};
  Image *map = add_quad_image("Map", red, green, blue, white);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  auto stack_hash = [&]() {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    return BKE_paint_material_composite_stack_hash(layers);
  };

  const uint64_t hash_plain = stack_hash();
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  const uint64_t hash_mapped = stack_hash();
  EXPECT_NE(hash_mapped, hash_plain);

  /* A slider move invalidates the cache: offset, rotation and scale each move the hash. */
  const float offset[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, correction, offset));
  const uint64_t hash_offset = stack_hash();
  EXPECT_NE(hash_offset, hash_mapped);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, correction, 0.5f));
  const uint64_t hash_rotated = stack_hash();
  EXPECT_NE(hash_rotated, hash_offset);
  const float two_scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, two_scale));
  EXPECT_NE(stack_hash(), hash_rotated);

  /* A zero scale (an old file's value) hashes like one: the setter normalizes, so build the
   * description directly to reach the hash's own normalization. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_FALSE(layers.is_empty());
  layers[0].mapping_scale[0] = 1.0f;
  layers[0].mapping_scale[1] = 1.0f;
  const uint64_t hash_one = BKE_paint_material_composite_stack_hash(layers);
  layers[0].mapping_scale[0] = 0.0f;
  layers[0].mapping_scale[1] = 0.0f;
  EXPECT_EQ(BKE_paint_material_composite_stack_hash(layers), hash_one);
  layers[0].mapping_scale[0] = 2.0f;
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), hash_one);
}

/** The stack hash reaches into a folder: a child's mapping offset moves it. */
TEST_F(PaintLayersCompositeTest, stack_hash_follows_a_folder_childs_mapping)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_solid_image("ChildMap", 4, 255, 0, 0, 255)));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, child, true));
  auto stack_hash = [&]() {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    EXPECT_FALSE(layers.is_empty());
    return BKE_paint_material_composite_stack_hash(layers);
  };
  const uint64_t hash_before = stack_hash();
  const float offset[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, child, offset));
  EXPECT_NE(stack_hash(), hash_before);
}

/** A Fill effect's flat colour has no image behind it, so the hash has to carry it. */
TEST_F(PaintLayersCompositeTest, stack_hash_follows_a_fill_effect_constant)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(effect, nullptr);
  auto stack_hash = [&]() {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    return BKE_paint_material_composite_stack_hash(layers);
  };
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, green));
  const uint64_t hash_green = stack_hash();
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, blue));
  EXPECT_NE(stack_hash(), hash_green);
}

/** The CPU reads a mapped row's live map where the graph does: a valid bake of the row is not
 * substituted while a mapping applies. */
TEST_F(PaintLayersCompositeTest, mapped_row_with_valid_bake_reads_its_live_map)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillBaked", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  Image *map = add_solid_image("LiveMap", 4, 255, 0, 0, 255);
  Image *baked_color = add_solid_image("BakedColor", 4, 0, 255, 0, 255);
  Image *baked_coverage = add_solid_image("BakedCoverage", 4, 255, 255, 255, 255);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*fill);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = baked_color;
  bake->coverage = baked_coverage;
  const auto store_hash = [&]() {
    uint32_t hash[2];
    BKE_paint_layers_bake_hash(*ma, *fill, hash);
    bake->hash[0] = hash[0];
    bake->hash[1] = hash[1];
  };
  store_hash();
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *fill));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_EQ(layers[0].color_image, baked_color);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  store_hash();
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *fill));
  layers.clear();
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_EQ(layers[0].color_image, map);
  EXPECT_TRUE(layers[0].mapping_enabled);
}

/** Stage 2: a mapped Hybrid Material row's live source alpha -- the row's coverage factor --
 * reads through the mapping, the same transform the graph puts before the alpha texture. */
TEST_F(PaintLayersCompositeTest, material_row_mapping_remaps_the_coverage)
{
  /* The source: Base Color from one flat image, Alpha from another, both unlinked and Repeat --
   * a Hybrid row's source. */
  Material *source = BKE_material_add(bmain, "CovSource");
  ASSERT_NE(source, nullptr);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = nullptr;
  for (bNode &node : src_tree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  if (principled == nullptr) {
    /* A fresh material has no node graph of its own to start from. */
    principled = bke::node_add_static_node(nullptr, src_tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, src_tree, SH_NODE_OUTPUT_MATERIAL);
    ASSERT_NE(principled, nullptr);
    ASSERT_NE(output, nullptr);
    bke::node_add_link(src_tree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  }
  Image *color_map = add_solid_image("CovColor", 2, 255, 0, 0, 255);
  Image *alpha_map = add_solid_image("CovAlpha", 2, 0, 0, 0, 255);
  {
    /* Alpha: white bottom row, transparent top row, in 2x2 texels. */
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(alpha_map, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(2)) {
      for (const int x : IndexRange(2)) {
        pixels[(int64_t(y) * 2 + x) * 4 + 3] = y == 0 ? 255 : 0;
      }
    }
    BKE_image_release_ibuf(alpha_map, ibuf, lock);
  }
  bNode *tex_color = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_alpha = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex_color, nullptr);
  ASSERT_NE(tex_alpha, nullptr);
  tex_color->id = &color_map->id;
  id_us_plus(&color_map->id);
  tex_alpha->id = &alpha_map->id;
  id_us_plus(&alpha_map->id);
  bke::node_add_link(src_tree,
                     *tex_color,
                     *bke::node_find_socket(*tex_color, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  bke::node_add_link(src_tree,
                     *tex_alpha,
                     *bke::node_find_socket(*tex_alpha, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  /* The row's coverage factor: the source alpha, sampled in UV space. */
  auto row_coverage = [&]() {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    int width = 0, height = 0;
    EXPECT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, width, height));
    Vector<float> color(int64_t(width) * height * 4);
    Vector<float> coverage(int64_t(width) * height);
    EXPECT_TRUE(BKE_paint_material_composite_eval_row_content(
        layers, row->marker, color.data(), coverage.data()));
    return coverage;
  };

  /* Unmapped: the coverage reads the alpha map straight, opaque bottom, transparent top. */
  Vector<float> coverage = row_coverage();
  ASSERT_EQ(coverage.size(), 4);
  EXPECT_NEAR(coverage[0], 1.0f, 1e-4f);
  EXPECT_NEAR(coverage[1], 1.0f, 1e-4f);
  EXPECT_NEAR(coverage[2], 0.0f, 1e-4f);
  EXPECT_NEAR(coverage[3], 0.0f, 1e-4f);

  /* The remap samples bilinearly with Repeat, so the cases below land on texel centers. Scale two
   * along x only: each row is uniform in x, so the factor reads exactly as unmapped. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  const float scale2[2] = {2.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale2));
  coverage = row_coverage();
  EXPECT_NEAR(coverage[0], 1.0f, 1e-4f);
  EXPECT_NEAR(coverage[1], 1.0f, 1e-4f);
  EXPECT_NEAR(coverage[2], 0.0f, 1e-4f);
  EXPECT_NEAR(coverage[3], 0.0f, 1e-4f);

  /* Offset half a tile along v flips the read: the transparent row covers instead, and the
   * opaque one wraps around to the bottom row. */
  const float half[2] = {0.0f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, row, half));
  coverage = row_coverage();
  EXPECT_NEAR(coverage[0], 0.0f, 1e-4f);
  EXPECT_NEAR(coverage[1], 0.0f, 1e-4f);
  EXPECT_NEAR(coverage[2], 1.0f, 1e-4f);
  EXPECT_NEAR(coverage[3], 1.0f, 1e-4f);
}

/** A source whose Base Color and Alpha read one image each. With \a texcoord_fed the Base Color
 * texture reads a Texture Coordinate, which is not a plain live map: the source then needs its
 * wrapper (SourceGroup); otherwise both maps are unlinked and the row is Hybrid. */
static Material *composite_test_map_source(Main &bmain,
                                           const char *name,
                                           Image &color_map,
                                           Image &alpha_map,
                                           const bool texcoord_fed)
{
  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &src_tree = *source->nodetree;
  bNode *principled = nullptr;
  for (bNode &node : src_tree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  if (principled == nullptr) {
    /* A fresh material has no node graph of its own to start from. */
    principled = bke::node_add_static_node(nullptr, src_tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, src_tree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(src_tree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  }
  bNode *tex_color = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_alpha = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  tex_color->id = &color_map.id;
  id_us_plus(&color_map.id);
  tex_alpha->id = &alpha_map.id;
  id_us_plus(&alpha_map.id);
  if (texcoord_fed) {
    bNode *tex_coord = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_COORD);
    bke::node_add_link(src_tree,
                       *tex_coord,
                       *bke::node_find_socket(*tex_coord, SOCK_OUT, "UV"_ustr),
                       *tex_color,
                       *bke::node_find_socket(*tex_color, SOCK_IN, "Vector"_ustr));
  }
  bke::node_add_link(src_tree,
                     *tex_color,
                     *bke::node_find_socket(*tex_color, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  bke::node_add_link(src_tree,
                     *tex_alpha,
                     *bke::node_find_socket(*tex_alpha, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(bmain, src_tree);
  return source;
}

/** Stage B: a Material correction or mask is remapped by the CPU only while its source is Hybrid
 * (the graph builds its Mapping in the row's own tree); a SourceGroup source keeps the mapping
 * inside its wrapper, which the CPU does not evaluate. */
TEST_F(PaintLayersCompositeTest, material_correction_mapping_follows_the_hybrid_mode)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_solid_image("Owner", 2, 0, 0, 255, 255));
  Material *hybrid = composite_test_map_source(*bmain,
                                               "CorrHybrid",
                                               *add_solid_image("CorrHybridC", 2, 255, 0, 0, 255),
                                               *add_solid_image("CorrHybridA", 2, 0, 0, 0, 255),
                                               false);
  Material *wrapped = composite_test_map_source(*bmain,
                                                "CorrWrapped",
                                                *add_solid_image("CorrWrappedC", 2, 255, 0, 0, 255),
                                                *add_solid_image("CorrWrappedA", 2, 0, 0, 0, 255),
                                                true);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "Effect");
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mask");
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(mask, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, effect, hybrid));
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, hybrid));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, effect, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, mask, true));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *effect), PaintLayerMaterialMode::Hybrid);

  auto collect = [&](const PaintMaterialCompositeCorrection *&r_effect,
                     const PaintMaterialCompositeCorrection *&r_mask,
                     Vector<PaintMaterialCompositeImageLayer> &layers) {
    layers.clear();
    ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    r_effect = nullptr;
    r_mask = nullptr;
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      if (BLI_uuid_equal(layer.marker, owner->marker)) {
        ASSERT_EQ(layer.content_corrections.size(), 1);
        ASSERT_EQ(layer.mask_corrections.size(), 1);
        r_effect = &layer.content_corrections[0];
        r_mask = &layer.mask_corrections[0];
      }
    }
    ASSERT_NE(r_effect, nullptr);
    ASSERT_NE(r_mask, nullptr);
  };

  Vector<PaintMaterialCompositeImageLayer> layers;
  const PaintMaterialCompositeCorrection *eff = nullptr;
  const PaintMaterialCompositeCorrection *msk = nullptr;
  collect(eff, msk, layers);
  EXPECT_TRUE(eff->mapping_enabled);
  EXPECT_TRUE(eff->coverage_mapping_enabled);
  EXPECT_TRUE(msk->mapping_enabled);

  /* The same two rows over a source that needs its wrapper: the CPU stays unmapped. */
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, effect, wrapped));
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, wrapped));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *effect), PaintLayerMaterialMode::SourceGroup);
  /* Without a bake a SourceGroup row has nothing on the CPU; give both their baked maps. */
  for (MaterialPaintLayer *item : {effect, mask}) {
    ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*item), nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma,
                                              *item,
                                              PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              add_solid_image("ItemBaked", 2, 0, 255, 0, 255)));
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(
        *ma, *item, -1, add_solid_image("ItemCoverage", 2, 255, 255, 255, 255)));
    BKE_paint_layers_bake_finalize(*ma, *item);
  }
  collect(eff, msk, layers);
  EXPECT_FALSE(eff->mapping_enabled);
  EXPECT_FALSE(eff->coverage_mapping_enabled);
  EXPECT_FALSE(msk->mapping_enabled);
}

/** Stage B: a mapped Hybrid correction's live source alpha -- its coverage -- reads through the
 * mapping, the same transform the graph puts before the alpha texture. */
TEST_F(PaintLayersCompositeTest, material_correction_mapping_remaps_the_coverage)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_solid_image("Owner", 2, 0, 0, 255, 255));
  Image *alpha_map = add_solid_image("CorrCovAlpha", 2, 0, 0, 0, 255);
  {
    /* Alpha: opaque first row, transparent second row, in 2x2 texels. */
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(alpha_map, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(2)) {
      for (const int x : IndexRange(2)) {
        pixels[(int64_t(y) * 2 + x) * 4 + 3] = y == 0 ? 255 : 0;
      }
    }
    BKE_image_release_ibuf(alpha_map, ibuf, lock);
  }
  Material *source = composite_test_map_source(
      *bmain, "CorrCovSource", *add_solid_image("CorrCovColor", 2, 255, 0, 0, 255), *alpha_map, false);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, effect, source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *effect), PaintLayerMaterialMode::Hybrid);

  /* Red where the correction's source alpha covers, the blue owner elsewhere. */
  auto is_red = [&](const int y) {
    const std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, y);
    return p[0] > 0.99f && p[2] < 0.01f;
  };
  auto is_blue = [&](const int y) {
    const std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, y);
    return p[0] < 0.01f && p[2] > 0.99f;
  };
  EXPECT_TRUE(is_red(0));
  EXPECT_TRUE(is_blue(1));

  /* Offset half a tile along v: the transparent row covers instead, the opaque one wraps. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, effect, true));
  const float half[2] = {0.0f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, effect, half));
  EXPECT_TRUE(is_blue(0));
  EXPECT_TRUE(is_red(1));
}

/** Stage 1 mapping on the CPU: a Fill mask with a map reads the map like the graph does, and a
 * mapping shifts the read. */
TEST_F(PaintLayersCompositeTest, fill_mask_with_map_reads_like_the_graph)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  /* A Fill mask: constant source, its map in the Base Color record. */
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, top, 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_halves_image("Mask")));
  auto at = [&](const int x, const int y) {
    return linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, x, y);
  };

  /* Left half white keeps the blue top, right half black shows the red bottom. */
  std::array<float, 4> p = at(0, 1);
  EXPECT_NEAR(p[2], 1.0f, 1e-3f);
  p = at(3, 1);
  EXPECT_NEAR(p[0], 1.0f, 1e-3f);
  EXPECT_NEAR(p[2], 0.0f, 1e-3f);

  /* An offset of half a tile moves the read into the black half. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, item, true));
  const float offset[2] = {0.5f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, item, offset));
  p = at(0, 1);
  EXPECT_NEAR(p[0], 1.0f, 1e-3f);
  EXPECT_NEAR(p[2], 0.0f, 1e-3f);
}

/** A negative scale mirrors the map on the CPU like the graph's Mapping node does. */
TEST_F(PaintLayersCompositeTest, fill_mask_negative_scale_mirrors_the_map)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, top, 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_halves_image("MirrorMask")));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, item, true));
  const float mirror[2] = {-1.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, item, mirror));

  /* The white left half now reads on the right: the blue top shows there, red on the left. */
  std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, 1);
  EXPECT_NEAR(p[0], 1.0f, 1e-3f);
  EXPECT_NEAR(p[2], 0.0f, 1e-3f);
  p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 3, 1);
  EXPECT_NEAR(p[2], 1.0f, 1e-3f);
}

/**
 * A mapped Fill row on the Normal channel rotates and mirrors the map's tangent-space vectors, not
 * just its read point: `x' = sx * (cos*nx + sin*ny)`, `y' = sy * (-sin*nx + cos*ny)` with only the
 * sign of the scale. Over a flat bottom the whiteout combine passes the (unit) detail through, so
 * the composited pixel is the re-oriented vector itself.
 */
TEST_F(PaintLayersCompositeTest, mapped_normal_map_rotates_and_mirrors_its_vectors)
{
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("Flat", 4, 128, 128, 255, 255, true);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "MappedNormal", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, channel), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, fill, channel, add_solid_image("Detail", 4, 204, 128, 230, 255, true)));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));

  const float detail[3] = {204.0f / 255.0f * 2.0f - 1.0f,
                           128.0f / 255.0f * 2.0f - 1.0f,
                           230.0f / 255.0f * 2.0f - 1.0f};
  struct Case {
    float rotation;
    float scale[2];
  };
  const Case cases[] = {
      {0.0f, {1.0f, 1.0f}},
      {1.57079632679f, {1.0f, 1.0f}},
      {3.14159265359f, {1.0f, 1.0f}},
      {0.0f, {-1.0f, 1.0f}},
      {0.0f, {1.0f, -1.0f}},
      {1.57079632679f, {-2.0f, 1.0f}},
      {0.7f, {1.0f, -0.5f}},
  };
  for (const Case &test : cases) {
    SCOPED_TRACE(::testing::Message() << "rotation=" << test.rotation << " scale=" << test.scale[0]
                                      << "," << test.scale[1]);
    ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, fill, test.rotation));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, fill, test.scale));
    const float cos_rot = cosf(test.rotation);
    const float sin_rot = sinf(test.rotation);
    float expected[3] = {(test.scale[0] < 0.0f ? -1.0f : 1.0f) *
                             (cos_rot * detail[0] + sin_rot * detail[1]),
                         (test.scale[1] < 0.0f ? -1.0f : 1.0f) *
                             (-sin_rot * detail[0] + cos_rot * detail[1]),
                         detail[2]};
    normalize_v3(expected);
    const std::array<float, 4> got = linear_pixel(channel, 1, 2);
    EXPECT_NEAR(got[0], expected[0] * 0.5f + 0.5f, 0.01f);
    EXPECT_NEAR(got[1], expected[1] * 0.5f + 0.5f, 0.01f);
    EXPECT_NEAR(got[2], expected[2] * 0.5f + 0.5f, 0.01f);
  }
}

/** The tangent-normal flag follows the Normal channel of a mapped row only, and moves the hash. */
TEST_F(PaintLayersCompositeTest, tangent_normal_flag_is_only_set_for_a_mapped_normal_channel)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mapped", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  for (const eMaterialPaintChannel channel :
       {PAINT_MATERIAL_CHANNEL_NORMAL, PAINT_MATERIAL_CHANNEL_BASE_COLOR})
  {
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, channel), nullptr);
    ASSERT_TRUE(BKE_paint_layers_channel_set_image(
        *ma, fill, channel, add_solid_image("FlagMap", 4, 128, 128, 255, 255, true)));
  }
  auto flag_for = [&](const eMaterialPaintChannel channel) {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
    EXPECT_FALSE(layers.is_empty());
    return std::make_pair(layers.is_empty() ? false : layers[0].tangent_normal,
                          layers.is_empty() ? 0 : BKE_paint_material_composite_stack_hash(layers));
  };
  EXPECT_FALSE(flag_for(PAINT_MATERIAL_CHANNEL_NORMAL).first);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  EXPECT_TRUE(flag_for(PAINT_MATERIAL_CHANNEL_NORMAL).first);
  EXPECT_FALSE(flag_for(PAINT_MATERIAL_CHANNEL_BASE_COLOR).first);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_NORMAL, layers));
  ASSERT_FALSE(layers.is_empty());
  const uint64_t with_flag = BKE_paint_material_composite_stack_hash(layers);
  layers[0].tangent_normal = false;
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), with_flag);
}

/** \} */

}  // namespace blender::bke::tests
