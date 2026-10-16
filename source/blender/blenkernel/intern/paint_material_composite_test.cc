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
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"

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
    EXPECT_TRUE(BKE_paint_material_composite_eval_linear(
        stack, PaintMaterialCompositeLinearTarget::frame(linear, stack.width)));
    const float *p = linear.data() + (int64_t(y) * stack.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
  }
};

TEST_F(PaintMaterialCompositeEvalTest, linear_zero_opacity_layer_leaves_the_stack_unchanged)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(10, 20, 30, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.opacity = 0.0f;
  stack.layers.append(top);

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 10.0f / 255.0f, 1e-5f);
  EXPECT_NEAR(result[1], 20.0f / 255.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_multiply_darkens_and_keeps_the_bottom_alpha)
{
  /* `node_mix_mult` writes `outcol.a = col1.a`; only Mix carries the top's alpha. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(128, 128, 128, 0);
  make_data(top.color_ibuf);
  top.blend = CompositeBlend::Multiply;
  stack.layers.append(top);

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_LT(result[0], 200.0f / 255.0f);
  EXPECT_NEAR(result[3], 1.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_opaque_mix_layer_replaces_what_is_below)
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

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 1.0f, 1e-5f);
  EXPECT_NEAR(result[3], 1.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_opacity_scales_the_layer_coverage)
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

TEST_F(PaintMaterialCompositeEvalTest, linear_disabled_layer_is_skipped)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(10, 20, 30, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.enabled = false;
  stack.layers.append(top);

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 10.0f / 255.0f, 1e-5f);
  EXPECT_NEAR(result[1], 20.0f / 255.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_mask_modulates_opacity_per_pixel)
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

  EXPECT_NEAR(linear_pixel(stack, 0, 0)[0], 0.0f, 1e-5f);
  EXPECT_NEAR(linear_pixel(stack, 1, 0)[0], 1.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_top_alpha_is_not_coverage)
{
  /* The Mix node interpolates by the factor alone. A fully transparent top layer at factor 1
   * therefore replaces what is below, and a compositor that treated its alpha as coverage would
   * show the bottom layer where the render shows the top one. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(10, 10, 10, 0);
  make_data(top.color_ibuf);
  stack.layers.append(top);

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 10.0f / 255.0f, 1e-5f);
  EXPECT_NEAR(result[3], 0.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_mask_from_alpha_reads_alpha_not_luminance)
{
  /* The factor comes from the layer's own Alpha output. Its colour is black, so a mask read as
   * luminance would hide the layer entirely. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(200, 200, 200, 255);
  make_data(bottom.color_ibuf);
  stack.layers.append(bottom);

  ImBuf *black_but_opaque = add_buffer(0, 0, 0, 255);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = black_but_opaque;
  top.mask_ibuf = black_but_opaque;
  top.mask_from_alpha = true;
  stack.layers.append(top);

  EXPECT_NEAR(linear_pixel(stack, 0, 0)[0], 0.0f, 1e-5f);

  /* The same mask read as colour leaves the bottom layer showing through. */
  stack.layers[1].mask_from_alpha = false;
  EXPECT_NEAR(linear_pixel(stack, 0, 0)[0], 200.0f / 255.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_normal_combine_lays_relief_over_relief)
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

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_GT(result[0], 160.0f / 255.0f);
  /* Still a unit normal: the encoded Z has to drop as X grows. */
  EXPECT_LT(result[2], 1.0f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_normal_combine_over_a_flat_normal_keeps_the_detail)
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

  const std::array<float, 4> result = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(result[0], 200.0f / 255.0f, 0.01f);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_mask_influence_zero_ignores_the_mask)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer bottom;
  bottom.color_ibuf = add_buffer(0, 0, 0, 255);
  stack.layers.append(bottom);

  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(255, 255, 255, 255);
  top.mask_ibuf = add_buffer(0, 0, 0, 255);
  top.mask_influence = 0.0f;
  stack.layers.append(top);

  EXPECT_NEAR(linear_pixel(stack, 0, 0)[0], 1.0f, 1e-5f);
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

TEST_F(PaintMaterialCompositeEvalTest, linear_region_leaves_the_rest_of_the_buffer_alone)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = add_buffer(255, 255, 255, 255);
  stack.layers.append(layer);

  const float sentinel = -1.0f;
  Vector<float> linear(int64_t(size) * size * 4, sentinel);
  rcti region;
  BLI_rcti_init(&region, 0, 2, 0, 2);
  /* The target describes exactly the region: its corner, the frame's stride. */
  EXPECT_TRUE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::in_frame(linear, size, region), &region));
  EXPECT_NEAR(linear[0], 1.0f, 1e-5f);
  EXPECT_NEAR(linear[(int64_t(1) * size + 1) * 4], 1.0f, 1e-5f);
  EXPECT_FLOAT_EQ(linear[(int64_t(3) * size + 3) * 4], sentinel);
  EXPECT_FLOAT_EQ(linear[(int64_t(0) * size + 2) * 4], sentinel);

  /* A region outside the buffer is a no-op that still reports success. */
  Vector<float> untouched(int64_t(size) * size * 4, sentinel);
  BLI_rcti_init(&region, 100, 110, 100, 110);
  EXPECT_TRUE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::frame(untouched, size), &region));
  EXPECT_FLOAT_EQ(untouched[0], sentinel);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_layer_of_a_different_size_is_rejected)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = IMB_allocImBuf(uint(size * 2), uint(size), ImBufFlags::ByteData);
  owned_buffers.append(layer.color_ibuf);
  stack.layers.append(layer);

  Vector<float> linear(int64_t(size) * size * 4);
  EXPECT_FALSE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::frame(linear, size)));
}

TEST_F(PaintMaterialCompositeEvalTest, linear_empty_stack_is_rejected)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  Vector<float> linear(int64_t(size) * size * 4);
  EXPECT_FALSE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::frame(linear, size)));
}

TEST_F(PaintMaterialCompositeEvalTest, bottom_color_is_what_the_first_layer_blends_over)
{
  /* Every layer, the lowest included, blends over the stack's bottom colour; without one the
   * stack starts transparent. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.has_constant_color = true;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  copy_v4_v4(layer.constant_color, white);
  layer.opacity = 0.5f;
  stack.layers.append(layer);

  const std::array<float, 4> transparent = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(transparent[0], 0.5f, 1e-5f);
  EXPECT_NEAR(transparent[3], 0.5f, 1e-5f);

  const float bottom[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  copy_v4_v4(stack.bottom_color, bottom);
  stack.has_bottom_color = true;
  const std::array<float, 4> over_bottom = linear_pixel(stack, 0, 0);
  EXPECT_NEAR(over_bottom[0], 0.6f, 1e-5f);
  EXPECT_NEAR(over_bottom[3], 1.0f, 1e-5f);
}

TEST_F(PaintMaterialCompositeEvalTest, constant_layer_needs_no_buffer_and_a_bare_layer_is_rejected)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer constant;
  constant.has_constant_color = true;
  constant.constant_color[0] = 0.25f;
  constant.constant_color[3] = 1.0f;
  stack.layers.append(constant);
  EXPECT_NEAR(linear_pixel(stack, 3, 3)[0], 0.25f, 1e-5f);

  /* A layer with neither a map nor a constant has nothing to composite. */
  stack.layers.append(PaintMaterialCompositeLayer());
  Vector<float> linear(int64_t(size) * size * 4);
  EXPECT_FALSE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::frame(linear, size)));
}

TEST_F(PaintMaterialCompositeEvalTest, linear_target_too_small_is_refused)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.has_constant_color = true;
  layer.constant_color[0] = 0.5f;
  layer.constant_color[3] = 1.0f;
  stack.layers.append(layer);

  /* One float short of the frame: refused, and nothing is written. */
  const float sentinel = -1.0f;
  Vector<float> linear(int64_t(size) * size * 4 - 1, sentinel);
  EXPECT_FALSE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::frame(linear, size)));
  EXPECT_FLOAT_EQ(linear[0], sentinel);
}

TEST_F(PaintMaterialCompositeEvalTest, linear_packed_region_matches_full_frame)
{
  const int big = 300;
  PaintMaterialCompositeStack stack;
  stack.width = big;
  stack.height = big;
  PaintMaterialCompositeLayer layer;
  layer.has_constant_color = true;
  layer.constant_color[0] = 0.25f;
  layer.constant_color[3] = 1.0f;
  stack.layers.append(layer);

  Vector<float> full(int64_t(big) * big * 4);
  ASSERT_TRUE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::frame(full, big)));

  /* A buffer that holds just the region, straddling a tile edge: the cost of a refresh no longer
   * depends on the frame, and the texels are the ones of the full evaluation. */
  rcti region;
  BLI_rcti_init(&region, 250, 300, 250, 300);
  Vector<float> packed(int64_t(50) * 50 * 4);
  ASSERT_TRUE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::packed(packed, region), &region));
  for (int y = 0; y < 50; y++) {
    for (int x = 0; x < 50; x++) {
      const int64_t packed_offset = (int64_t(y) * 50 + x) * 4;
      const int64_t full_offset = (int64_t(y + 250) * big + (x + 250)) * 4;
      EXPECT_FLOAT_EQ(packed[packed_offset], full[full_offset]) << x << "," << y;
    }
  }
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
  ASSERT_TRUE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::frame(full, big)));

  const float sentinel = -1.0f;
  Vector<float> partial(int64_t(big) * big * 4, sentinel);
  rcti region;
  BLI_rcti_init(&region, 250, 300, 250, 300);
  ASSERT_TRUE(BKE_paint_material_composite_eval_linear(
      stack, PaintMaterialCompositeLinearTarget::in_frame(partial, big, region), &region));

  for (int y = region.ymin; y < region.ymax; y++) {
    for (int x = region.xmin; x < region.xmax; x++) {
      const int64_t offset = (int64_t(y) * big + x) * 4;
      EXPECT_FLOAT_EQ(partial[offset], full[offset]) << x << "," << y;
    }
  }
  EXPECT_FLOAT_EQ(partial[0], sentinel);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stack Derivation
 * \{ */

class PaintMaterialCompositeStackTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_paint_material_composite_cache_free_all();
    BKE_main_free(bmain);
  }

  Material *add_material_with_principled(const char *name)
  {
    Material *ma = BKE_material_add(bmain, name);
    bNodeTree &ntree = *ma->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(ntree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
    return ma;
  }

  bNode *find_node(Material &ma, const int type_legacy)
  {
    for (bNode &node : ma.nodetree->nodes) {
      if (node.type_legacy == type_legacy) {
        return &node;
      }
    }
    return nullptr;
  }

  /**
   * A generated byte image, not referenced by any node.
   *
   * \param image_size: 8 unless a test needs the image's partial-update chunks to be
   *                    distinguishable. That log works in 256-pixel chunks, so an 8x8 image has
   *                    exactly one and no edit inside it can be told from any other.
   */
  Image *add_image(const char *image_name, const int image_size = 8)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(bmain,
                                   image_size,
                                   image_size,
                                   image_name,
                                   32,
                                   false,
                                   IMA_GENTYPE_BLANK,
                                   color,
                                   false,
                                   false,
                                   false);
  }

  /** An Image Texture node with a generated image assigned. */
  bNode *add_image_texture(Material &ma, const char *image_name, const int image_size = 8)
  {
    Image *image = add_image(image_name, image_size);
    bNode *node = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_TEX_IMAGE);
    node->id = &image->id;
    return node;
  }

  /** A map that belongs to paint layer \a layer_id and says which channel it authors. */
  Image *add_layer_map(const char *image_name, const bUUID &layer_id, const int channel)
  {
    Image *image = add_image(image_name);
    image->paint_layer_id = layer_id;
    image->paint_layer_channel = channel;
    return image;
  }

  /**
   * Overwrite \a rect of \a image *and record the edit*, as the paint path does.
   *
   * The recording is the point: the composite cache learns what changed by polling the image's own
   * partial-update log, so a test that only writes pixels is testing the case the cache must
   * deliberately *not* notice. #imapaint_image_update makes the same call.
   */
  void paint_region(Image &image, const rcti &rect, const uchar value)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(rect.ymin, BLI_rcti_size_y(&rect))) {
      for (const int x : IndexRange(rect.xmin, BLI_rcti_size_x(&rect))) {
        uchar *p = pixels + (int64_t(y) * ibuf->x + x) * 4;
        p[0] = p[1] = p[2] = value;
        p[3] = 255;
      }
    }
    BKE_image_partial_update_mark_region(
        &image, static_cast<ImageTile *>(image.tiles.first), ibuf, &rect);
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** Overwrite every pixel of \a image, as an edit to a layer would. */
  void fill_image(Image &image, const uchar value)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      pixels[i * 4 + 0] = value;
      pixels[i * 4 + 1] = value;
      pixels[i * 4 + 2] = value;
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  static const uchar *pixel(const ImBuf &ibuf, const int x, const int y)
  {
    return ibuf.byte_data() + (int64_t(y) * ibuf.x + x) * 4;
  }

  bNodeSocket &base_color_socket(Material &ma)
  {
    bNode *principled = find_node(ma, SH_NODE_BSDF_PRINCIPLED);
    return *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  }

  /** Link the "Color" output of \a from_node to the Principled Base Color input. */
  void link_to_base_color(Material &ma, bNode &from_node)
  {
    bNode *principled = find_node(ma, SH_NODE_BSDF_PRINCIPLED);
    bke::node_add_link(*ma.nodetree,
                       from_node,
                       *bke::node_find_socket(from_node, SOCK_OUT, "Color"_ustr),
                       *principled,
                       base_color_socket(ma));
  }
};

TEST_F(PaintMaterialCompositeStackTest, unlinked_base_color_is_not_a_stack)
{
  Material *ma = add_material_with_principled("Mat");
  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
}

TEST_F(PaintMaterialCompositeStackTest, single_image_texture_is_a_stack_of_one)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, id_cast<Image *>(tex->id));
  EXPECT_EQ(layers[0].mask_image, nullptr);
}

TEST_F(PaintMaterialCompositeStackTest, mix_chain_is_collected_bottom_first)
{
  Material *ma = add_material_with_principled("Mat");
  bNodeTree &ntree = *ma->nodetree;
  bNode *bottom = add_image_texture(*ma, "Bottom");
  bNode *top = add_image_texture(*ma, "Top");
  bNode *mix = bke::node_add_static_node(nullptr, ntree, SH_NODE_MIX_RGB_LEGACY);
  mix->custom1 = MA_RAMP_MULT;

  bke::node_add_link(ntree,
                     *bottom,
                     *bke::node_find_socket(*bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(ntree,
                     *top,
                     *bke::node_find_socket(*top, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  link_to_base_color(*ma, *mix);

  bNodeSocket *fac = bke::node_find_socket(*mix, SOCK_IN, "Fac"_ustr);
  static_cast<bNodeSocketValueFloat *>(fac->default_value)->value = 0.25f;

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 2);
  EXPECT_EQ(layers[0].color_image, id_cast<Image *>(bottom->id));
  EXPECT_EQ(layers[1].color_image, id_cast<Image *>(top->id));
  EXPECT_EQ(layers[1].blend, CompositeBlend::Multiply);
  EXPECT_FLOAT_EQ(layers[1].opacity, 0.25f);
}

TEST_F(PaintMaterialCompositeStackTest, linked_factor_becomes_the_layer_mask)
{
  Material *ma = add_material_with_principled("Mat");
  bNodeTree &ntree = *ma->nodetree;
  bNode *bottom = add_image_texture(*ma, "Bottom");
  bNode *top = add_image_texture(*ma, "Top");
  bNode *mask = add_image_texture(*ma, "Mask");
  bNode *mix = bke::node_add_static_node(nullptr, ntree, SH_NODE_MIX_RGB_LEGACY);

  bke::node_add_link(ntree,
                     *bottom,
                     *bke::node_find_socket(*bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(ntree,
                     *top,
                     *bke::node_find_socket(*top, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  bke::node_add_link(ntree,
                     *mask,
                     *bke::node_find_socket(*mask, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Fac"_ustr));
  link_to_base_color(*ma, *mix);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 2);
  EXPECT_EQ(layers[1].mask_image, id_cast<Image *>(mask->id));
}

TEST_F(PaintMaterialCompositeStackTest, unsupported_blend_mode_is_not_a_stack)
{
  Material *ma = add_material_with_principled("Mat");
  bNodeTree &ntree = *ma->nodetree;
  bNode *bottom = add_image_texture(*ma, "Bottom");
  bNode *top = add_image_texture(*ma, "Top");
  bNode *mix = bke::node_add_static_node(nullptr, ntree, SH_NODE_MIX_RGB_LEGACY);
  mix->custom1 = MA_RAMP_SCREEN;

  bke::node_add_link(ntree,
                     *bottom,
                     *bke::node_find_socket(*bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(ntree,
                     *top,
                     *bke::node_find_socket(*top, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  link_to_base_color(*ma, *mix);

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
}

TEST_F(PaintMaterialCompositeStackTest, procedural_layer_is_not_a_stack)
{
  Material *ma = add_material_with_principled("Mat");
  bNodeTree &ntree = *ma->nodetree;
  bNode *bottom = add_image_texture(*ma, "Bottom");
  bNode *noise = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_NOISE);
  bNode *mix = bke::node_add_static_node(nullptr, ntree, SH_NODE_MIX_RGB_LEGACY);

  bke::node_add_link(ntree,
                     *bottom,
                     *bke::node_find_socket(*bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(ntree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  link_to_base_color(*ma, *mix);

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
}

TEST_F(PaintMaterialCompositeStackTest, normal_without_a_normal_map_node_is_not_a_stack)
{
  /* The Normal chain is read from the Normal Map node's Color input, since that is where the maps
   * still are in the space a stroke paints. Without that node there is nothing to read. */
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_NORMAL, layers));
}

TEST_F(PaintMaterialCompositeStackTest, hash_tracks_opacity_and_order)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *a = add_image_texture(*ma, "A");
  bNode *b = add_image_texture(*ma, "B");

  Vector<PaintMaterialCompositeImageLayer> layers;
  PaintMaterialCompositeImageLayer layer_a;
  layer_a.color_image = id_cast<Image *>(a->id);
  PaintMaterialCompositeImageLayer layer_b;
  layer_b.color_image = id_cast<Image *>(b->id);
  layers.append(layer_a);
  layers.append(layer_b);

  const uint64_t base = BKE_paint_material_composite_stack_hash(layers);

  layers[1].opacity = 0.5f;
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), base);

  layers[1].opacity = 1.0f;
  EXPECT_EQ(BKE_paint_material_composite_stack_hash(layers), base);

  std::swap(layers[0], layers[1]);
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), base);
}

TEST_F(PaintMaterialCompositeStackTest, cache_reuses_the_buffer_across_calls)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);

  ImBuf *first = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash);
  ASSERT_NE(first, nullptr);
  EXPECT_TRUE(
      BKE_paint_material_composite_cache_contains(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  ImBuf *second = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash);
  EXPECT_EQ(first, second);

  /* Invalidation must keep the buffer, only mark it: a caller in between has to get the previous
   * pixels rather than nothing. */
  BKE_paint_material_composite_cache_invalidate(ma);
  EXPECT_TRUE(
      BKE_paint_material_composite_cache_contains(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ImBuf *third = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash);
  EXPECT_EQ(first, third);
}

TEST_F(PaintMaterialCompositeStackTest, freeing_the_material_drops_its_composite)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_NE(BKE_paint_material_composite_cache_ensure(
                *ma,
                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                layers,
                BKE_paint_material_composite_stack_hash(layers)),
            nullptr);

  /* Unlike an invalidate, this drops the entry: after the material is gone its key can never be
   * looked up again, so leaving it would only hold onto the buffer. */
  BKE_paint_material_composite_cache_free_material(*ma);
  EXPECT_FALSE(
      BKE_paint_material_composite_cache_contains(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintMaterialCompositeStackTest, freeing_an_evaluated_copy_keeps_the_composite)
{
  /* A copy-on-evaluation datablock is not allocated through the usual path and therefore carries
   * the *original's* session UID -- which is exactly what both caches are keyed on. The depsgraph
   * frees such a copy on every relation rebuild, and dropping the entry there would silently turn
   * each rebuild into a full re-composite of a material that is still very much alive. */
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_NE(BKE_paint_material_composite_cache_ensure(
                *ma,
                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                layers,
                BKE_paint_material_composite_stack_hash(layers)),
            nullptr);

  /* What #deg_expand_eval_copy_datablock builds: a localized copy, tagged, wearing the original's
   * session UID. */
  ID *eval_id = nullptr;
  BKE_id_copy_ex(nullptr, &ma->id, &eval_id, LIB_ID_COPY_LOCALIZE);
  ASSERT_NE(eval_id, nullptr);
  eval_id->tag |= ID_TAG_COPIED_ON_EVAL;
  eval_id->session_uid = ma->id.session_uid;

  BKE_id_free(nullptr, eval_id);

  EXPECT_TRUE(BKE_paint_material_composite_cache_contains(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  BKE_paint_material_composite_cache_free_material(*ma);
}

TEST_F(PaintMaterialCompositeStackTest, mask_is_cached_apart_from_the_channels)
{
  /* #PAINT_LAYER_MAP_MASK is a role and not an #eMaterialPaintChannel, which is why the cache
   * takes an `int`. It still has to key like any other pass rather than collide with one. */
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);

  ImBuf *base = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash);
  ASSERT_NE(base, nullptr);
  ImBuf *mask = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_LAYER_MAP_MASK, layers, hash);
  ASSERT_NE(mask, nullptr);
  EXPECT_NE(base, mask);

  EXPECT_TRUE(BKE_paint_material_composite_cache_contains(*ma, PAINT_LAYER_MAP_MASK));
  BKE_paint_material_composite_cache_free_material(*ma);
  EXPECT_FALSE(BKE_paint_material_composite_cache_contains(*ma, PAINT_LAYER_MAP_MASK));
}

TEST_F(PaintMaterialCompositeStackTest, layer_map_fallback_can_be_refused)
{
  /* A caller looping over every material to find out *which* one owns an image cannot afford the
   * fallback: it derives the channel from the layer tags, which costs a graph walk per channel and
   * a pass over every image in the file, per material. The wired channel already names the
   * material, so that caller asks for the graph alone. */
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Image *base_image = id_cast<Image *>(tex->id);
  const bUUID layer_id = BLI_uuid_generate_random();
  base_image->paint_layer_id = layer_id;
  base_image->paint_layer_channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  /* An Ambient Occlusion map of the same layer: tagged, and wired nowhere. */
  Image *ao_image = add_layer_map("AO", layer_id, PAINT_MATERIAL_CHANNEL_AO);
  ASSERT_NE(ao_image, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_AO, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, ao_image);

  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_AO, layers, /*allow_layer_map_fallback=*/false));
  EXPECT_TRUE(layers.is_empty());

  /* Refusing the fallback must not affect a channel that really is wired. */
  EXPECT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, /*allow_layer_map_fallback=*/false));
  EXPECT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, base_image);
}

TEST_F(PaintMaterialCompositeStackTest, an_untagged_image_is_never_a_layer_map)
{
  /* The candidate filter that keeps the map lookup off the whole of #Main drops images with no
   * layer tag. An image whose channel says nothing must stay invisible to it even when some other
   * image of the same layer does carry a tag. */
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Image *base_image = id_cast<Image *>(tex->id);
  const bUUID layer_id = BLI_uuid_generate_random();
  base_image->paint_layer_id = layer_id;
  base_image->paint_layer_channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  /* Of the right layer, but saying nothing about which channel it authors. */
  Image *untagged = add_layer_map("Untagged", layer_id, PAINT_LAYER_MAP_NONE);
  ASSERT_NE(untagged, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_AO, layers));
}

TEST_F(PaintMaterialCompositeStackTest, edited_chunk_is_the_only_part_recomputed)
{
  /* 512 square so the image's partial-update log has four chunks rather than one: the whole point
   * of this test is that an edit in one of them leaves the others alone. */
  constexpr int image_size = 512;
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base", image_size);
  link_to_base_color(*ma, *tex);
  Image *base = id_cast<Image *>(tex->id);

  fill_image(*base, 10);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);

  uint64_t first_revision = 0;
  ImBuf *composite = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &first_revision);
  ASSERT_NE(composite, nullptr);
  ASSERT_EQ(pixel(*composite, 0, 0)[0], 10);
  ASSERT_EQ(pixel(*composite, 300, 300)[0], 10);

  /* The whole layer changes, but only a corner of it is *recorded*. Everything outside the chunk
   * that corner falls in has to keep the pixels it had: that is what makes a stroke cost its own
   * area rather than the whole canvas. */
  fill_image(*base, 200);
  rcti region;
  BLI_rcti_init(&region, 0, 2, 0, 2);
  paint_region(*base, region, 200);

  uint64_t second_revision = 0;
  ImBuf *refreshed = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &second_revision);
  ASSERT_EQ(refreshed, composite);
  EXPECT_NE(second_revision, first_revision);
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 200);
  EXPECT_EQ(pixel(*composite, 100, 100)[0], 200);
  /* Outside the first chunk, and so untouched. */
  EXPECT_EQ(pixel(*composite, 300, 300)[0], 10);
}

TEST_F(PaintMaterialCompositeStackTest, edits_are_detected_without_a_tag)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);
  Image *base = id_cast<Image *>(tex->id);

  fill_image(*base, 10);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);

  ImBuf *composite = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash);
  ASSERT_NE(composite, nullptr);
  ASSERT_EQ(pixel(*composite, 0, 0)[0], 10);

  /* Nobody tags the cache: the edit is recorded in the image's own partial-update log, and polling
   * that is what the cache now does. */
  rcti dab;
  BLI_rcti_init(&dab, 0, 2, 0, 2);
  paint_region(*base, dab, 200);

  rcti changed;
  BLI_rcti_init(&changed, 0, 0, 0, 0);
  ImBuf *refreshed = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, nullptr, nullptr, &changed);
  ASSERT_EQ(refreshed, composite);
  EXPECT_FALSE(BLI_rcti_is_empty(&changed));
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 200);
}

TEST_F(PaintMaterialCompositeStackTest, no_edit_means_no_reflatten)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);
  Image *base = id_cast<Image *>(tex->id);

  fill_image(*base, 10);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);

  uint64_t first_revision = 0;
  BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &first_revision);

  /* The steady state of a redraw that changed nothing: same revision, nothing reported, and above
   * all no re-flatten of the stack. */
  uint64_t second_revision = 0;
  rcti changed;
  BLI_rcti_init(&changed, 0, 0, 0, 0);
  BKE_paint_material_composite_cache_ensure(*ma,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            layers,
                                            hash,
                                            &second_revision,
                                            nullptr,
                                            &changed);
  EXPECT_EQ(first_revision, second_revision);
  EXPECT_TRUE(BLI_rcti_is_empty(&changed));
}

TEST_F(PaintMaterialCompositeStackTest, untagged_image_leaves_the_composite_alone)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);
  Image *base = id_cast<Image *>(tex->id);
  Image *stranger = add_image("Stranger");

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);

  uint64_t revision = 0;
  ASSERT_NE(BKE_paint_material_composite_cache_ensure(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &revision),
            nullptr);

  /* An image the stack does not read must not make it recompute; only the stack's own layers are
   * subscribed to, which is what stands between an unrelated paint stroke and a rebuild of every
   * composite in the file. */
  rcti dab;
  BLI_rcti_init(&dab, 0, 4, 0, 4);
  paint_region(*stranger, dab, 200);
  uint64_t revision_after = 0;
  ASSERT_NE(BKE_paint_material_composite_cache_ensure(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &revision_after),
            nullptr);
  EXPECT_EQ(revision_after, revision);

  paint_region(*base, dab, 200);
  uint64_t revision_final = 0;
  ASSERT_NE(BKE_paint_material_composite_cache_ensure(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &revision_final),
            nullptr);
  EXPECT_NE(revision_final, revision);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layer Maps
 *
 * The half of a paint layer that no node link describes: a baked Ambient Occlusion map and the
 * layer's own mask are found by #Image.paint_layer_id and #Image.paint_layer_channel instead.
 * \{ */

TEST_F(PaintMaterialCompositeStackTest, layer_maps_come_from_the_graph_and_from_the_tag)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base Color TexLayer");
  link_to_base_color(*ma, *tex);
  Image *base = id_cast<Image *>(tex->id);

  const bUUID layer_id = BLI_uuid_generate_random();
  base->paint_layer_id = layer_id;
  /* Deliberately left as #PAINT_LAYER_MAP_NONE: a wired channel is identified by the stack, so it
   * works for a layer an add-on never tagged. */

  Image *ao = add_layer_map("AO TexLayer", layer_id, PAINT_MATERIAL_CHANNEL_AO);
  Image *mask = add_layer_map("Mask TexLayer", layer_id, PAINT_LAYER_MAP_MASK);
  /* Same channels, different layer: must not be picked up. */
  add_layer_map("Other AO", BLI_uuid_generate_random(), PAINT_MATERIAL_CHANNEL_AO);

  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM + 1> maps;
  BKE_paint_material_layer_maps_get(*bmain, *ma, layer_id, maps);

  EXPECT_EQ(maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR], base);
  EXPECT_EQ(maps[PAINT_MATERIAL_CHANNEL_AO], ao);
  EXPECT_EQ(maps[PAINT_LAYER_MAP_MASK], mask);
  /* A channel the layer does not author stays null, which is what lets the canvas list show it
   * without pretending it is selectable. */
  EXPECT_EQ(maps[PAINT_MATERIAL_CHANNEL_ROUGHNESS], nullptr);
}

TEST_F(PaintMaterialCompositeStackTest, layer_maps_of_a_nil_uuid_are_empty)
{
  Material *ma = add_material_with_principled("Mat");
  add_layer_map("Untagged AO", BLI_uuid_nil(), PAINT_MATERIAL_CHANNEL_AO);

  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM + 1> maps;
  BKE_paint_material_layer_maps_get(*bmain, *ma, BLI_uuid_nil(), maps);
  for (Image *map : maps) {
    EXPECT_EQ(map, nullptr);
  }
}

TEST_F(PaintMaterialCompositeStackTest, ao_stack_is_assembled_from_the_layer_maps)
{
  Material *ma = add_material_with_principled("Mat");
  bNodeTree &ntree = *ma->nodetree;
  bNode *bottom = add_image_texture(*ma, "Bottom");
  bNode *top = add_image_texture(*ma, "Top");
  bNode *mix = bke::node_add_static_node(nullptr, ntree, SH_NODE_MIX_RGB_LEGACY);
  mix->custom1 = MA_RAMP_MULT;

  bke::node_add_link(ntree,
                     *bottom,
                     *bke::node_find_socket(*bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(ntree,
                     *top,
                     *bke::node_find_socket(*top, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  link_to_base_color(*ma, *mix);
  bNodeSocket *fac = bke::node_find_socket(*mix, SOCK_IN, "Fac"_ustr);
  static_cast<bNodeSocketValueFloat *>(fac->default_value)->value = 0.25f;

  const bUUID bottom_id = BLI_uuid_generate_random();
  const bUUID top_id = BLI_uuid_generate_random();
  id_cast<Image *>(bottom->id)->paint_layer_id = bottom_id;
  id_cast<Image *>(top->id)->paint_layer_id = top_id;
  Image *bottom_ao = add_layer_map("Bottom AO", bottom_id, PAINT_MATERIAL_CHANNEL_AO);
  Image *top_ao = add_layer_map("Top AO", top_id, PAINT_MATERIAL_CHANNEL_AO);

  /* Ambient Occlusion has no Principled input to walk, so the stack's shape -- the order, the
   * blend and the opacity -- is borrowed from the channel that does have a chain, and only the
   * images are swapped for this channel's maps. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_AO, layers));
  ASSERT_EQ(layers.size(), 2);
  EXPECT_EQ(layers[0].color_image, bottom_ao);
  EXPECT_EQ(layers[1].color_image, top_ao);
  EXPECT_EQ(layers[1].blend, CompositeBlend::Multiply);
  EXPECT_FLOAT_EQ(layers[1].opacity, 0.25f);
}

TEST_F(PaintMaterialCompositeStackTest, a_layer_without_a_map_for_the_channel_is_skipped)
{
  Material *ma = add_material_with_principled("Mat");
  bNodeTree &ntree = *ma->nodetree;
  bNode *bottom = add_image_texture(*ma, "Bottom");
  bNode *top = add_image_texture(*ma, "Top");
  bNode *mix = bke::node_add_static_node(nullptr, ntree, SH_NODE_MIX_RGB_LEGACY);

  bke::node_add_link(ntree,
                     *bottom,
                     *bke::node_find_socket(*bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(ntree,
                     *top,
                     *bke::node_find_socket(*top, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  link_to_base_color(*ma, *mix);

  const bUUID bottom_id = BLI_uuid_generate_random();
  id_cast<Image *>(bottom->id)->paint_layer_id = bottom_id;
  id_cast<Image *>(top->id)->paint_layer_id = BLI_uuid_generate_random();
  Image *bottom_ao = add_layer_map("Bottom AO", bottom_id, PAINT_MATERIAL_CHANNEL_AO);

  /* Only one of the two layers has an AO map. That layer still composites: a user who baked AO
   * for one layer should see it, rather than have the whole channel refuse to resolve. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_AO, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, bottom_ao);
}

TEST_F(PaintMaterialCompositeStackTest, ao_without_any_map_is_not_a_stack)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);
  id_cast<Image *>(tex->id)->paint_layer_id = BLI_uuid_generate_random();

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_AO, layers));
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

TEST_F(PaintMaterialCompositeStackTest, combined_is_not_a_layer_stack)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_LAYER_PASS_COMBINED, layers));
  EXPECT_TRUE(layers.is_empty());
}

TEST_F(PaintMaterialCompositeStackTest, changed_region_reports_what_was_recomputed)
{
  Material *ma = add_material_with_principled("Mat");
  bNode *tex = add_image_texture(*ma, "Base");
  link_to_base_color(*ma, *tex);
  Image *base = id_cast<Image *>(tex->id);
  fill_image(*base, 10);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_material_composite_stack_from_material(
      *bmain, *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  const uint64_t hash = BKE_paint_material_composite_stack_hash(layers);

  rcti changed;

  /* First call builds everything, so the whole buffer changed. */
  ImBuf *ibuf = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, nullptr, nullptr, &changed);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(BLI_rcti_size_x(&changed), ibuf->x);
  EXPECT_EQ(BLI_rcti_size_y(&changed), ibuf->y);

  /* Nothing changed since: nothing is recomputed and the rectangle is empty. */
  BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, nullptr, nullptr, &changed);
  EXPECT_TRUE(BLI_rcti_is_empty(&changed));

  /* An edited rectangle is reported as the rectangle recomputed -- rounded out to the image's
   * partial-update chunks, which are 256 square, so on an image this small that is the whole
   * buffer. What has to hold is that it covers the edit and no more than the buffer. */
  rcti dab;
  BLI_rcti_init(&dab, 1, 3, 2, 4);
  paint_region(*base, dab, 200);
  BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, nullptr, nullptr, &changed);
  EXPECT_LE(changed.xmin, dab.xmin);
  EXPECT_GE(changed.xmax, dab.xmax);
  EXPECT_LE(changed.ymin, dab.ymin);
  EXPECT_GE(changed.ymax, dab.ymax);
  EXPECT_LE(BLI_rcti_size_x(&changed), ibuf->x);
  EXPECT_LE(BLI_rcti_size_y(&changed), ibuf->y);
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

  /** A 2x2 Non-Color byte map: \a c00 bottom left, \a c10 bottom right, \a c01 top left, \a c11 top
   * right. */
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

  /** A Fill row, over the rows already there, whose \a channel record is backed by \a image. */
  MaterialPaintLayer *add_fill_layer(
      const char *name,
      Image *image,
      const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, layer, channel, image));
    return layer;
  }

  /** A Fill correction of \a owner whose Base Color record is backed by \a image. */
  MaterialPaintLayer *add_fill_correction(MaterialPaintLayer *owner, Image *image)
  {
    MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
        *ma, owner, "C", MA_PAINT_LAYER_SOURCE_CONSTANT);
    EXPECT_NE(correction, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
              nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(
        *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
    return correction;
  }

  /** A row whose \a channel record is backed by \a image. */
  MaterialPaintLayer *add_paint_layer(
      const char *name,
      Image *image,
      const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, layer, channel, image));
    return layer;
  }

  /** A row whose \a channel record carries a flat value and no map. */
  MaterialPaintLayer *add_constant_layer(
      const char *name,
      const float value[4],
      const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_value(*ma, layer, channel, value));
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
        layers, PaintMaterialCompositeLinearTarget::frame(linear, width), nullptr, nullptr, bottom));
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

TEST_F(PaintLayersCompositeTest, transparent_texels_of_a_painted_map_show_what_is_below)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  /* A fresh map is transparent where nothing was painted; it must not cover the row below. */
  add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 0, 0));

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, disabled_channel_contributes_nothing)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  ASSERT_EQ(top->channels_num, 1);
  MaterialPaintLayerChannel *record = &top->channels[0];
  Image *image = record->image;
  ASSERT_NE(image, nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));

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
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));
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

TEST_F(PaintLayersCompositeTest, a_channel_of_another_row_is_not_part_of_the_stack)
{
  /* A row takes part only in the channels it has a live record for. */
  add_paint_layer("Color", add_solid_image("Color", 4, 255, 0, 0, 255));
  add_paint_layer("Rough",
                  add_solid_image("Rough", 4, 255, 255, 255, 255, true),
                  PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  Vector<PaintMaterialCompositeImageLayer> color_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color_layers));
  EXPECT_EQ(color_layers.size(), 1);

  Vector<PaintMaterialCompositeImageLayer> rough_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_layers));
  EXPECT_EQ(rough_layers.size(), 1);

  Vector<PaintMaterialCompositeImageLayer> metallic_layers;
  EXPECT_FALSE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_METALLIC, metallic_layers));
  EXPECT_TRUE(metallic_layers.is_empty());
}

TEST_F(PaintLayersCompositeTest, constant_value_below_paint_does_not_stop_it)
{
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_constant_layer("Flat", blue);
  add_paint_layer("Top", add_solid_image("Top", 4, 255, 0, 0, 255));

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, constant_value_above_paint_covers_it)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_constant_layer("Flat", blue);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_BASE_COLOR, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 255);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, constant_only_channel_has_no_dimensions)
{
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_constant_layer("Flat", blue);

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

TEST_F(PaintLayersCompositeTest, unpainted_row_lays_nothing)
{
  /* A fresh row carries a transparent record and no map: it takes part in nothing yet. */
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "Empty", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_FALSE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  EXPECT_TRUE(layers.is_empty());

  /* A channel number outside the table has no stack either. */
  EXPECT_FALSE(BKE_paint_layers_composite_image_layers(*ma, -1, layers));
  EXPECT_FALSE(BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_NUM, layers));
}

TEST_F(PaintLayersCompositeTest, bottom_color_is_the_channels_neutral_value)
{
  /* A half-opaque Roughness row fades towards the channel's neutral 0.5, not towards zero. */
  MaterialPaintLayer *row = add_paint_layer("Rough",
                                            add_solid_image("Rough", 4, 255, 255, 255, 255, true),
                                            PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  BKE_paint_layers_set_opacity(*ma, row, 0.5f);

  const std::array<float, 4> result = linear_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS, 0, 0);
  EXPECT_NEAR(result[0], 0.75f, 1e-4f);
}

TEST_F(PaintLayersCompositeTest, normal_channel_combines_instead_of_mixing)
{
  add_paint_layer("Bottom",
                  add_solid_image("Bottom", 4, 160, 128, 255, 255, true),
                  PAINT_MATERIAL_CHANNEL_NORMAL);
  add_paint_layer(
      "Top", add_solid_image("Top", 4, 160, 128, 255, 255, true), PAINT_MATERIAL_CHANNEL_NORMAL);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, layers));
  ASSERT_EQ(layers.size(), 2);
  EXPECT_EQ(layers[0].blend, CompositeBlend::NormalCombine);
  EXPECT_EQ(layers[1].blend, CompositeBlend::NormalCombine);

  /* Two equal tilts add up: the encoded X goes beyond either map's. */
  const std::array<float, 4> result = linear_pixel(PAINT_MATERIAL_CHANNEL_NORMAL, 1, 1);
  EXPECT_GT(result[0], 160.0f / 255.0f);
}

TEST_F(PaintLayersCompositeTest, normal_channel_export_is_unit_length)
{
  add_paint_layer("Tilt",
                  add_solid_image("Tilt", 4, 160, 128, 255, 255, true),
                  PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *top = add_paint_layer("Tilt2",
                                            add_solid_image("Tilt2", 4, 160, 128, 255, 255, true),
                                            PAINT_MATERIAL_CHANNEL_NORMAL);
  BKE_paint_layers_set_opacity(*ma, top, 0.5f);

  int w = 0, h = 0;
  ImBuf *ibuf = composite(PAINT_MATERIAL_CHANNEL_NORMAL, w, h);
  const uchar *p = pixel(*ibuf, 1, 1);
  const float x = float(p[0]) / 127.5f - 1.0f;
  const float y = float(p[1]) / 127.5f - 1.0f;
  const float z = float(p[2]) / 127.5f - 1.0f;
  EXPECT_NEAR(x * x + y * y + z * z, 1.0f, 0.03f);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, region_refresh_touches_only_the_region)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));

  ImBuf *ibuf = IMB_allocImBuf(4, 4, ImBufFlags::ByteData);
  ibuf->channels = 4;
  uchar *pixels = ibuf->byte_data_for_write();
  memset(pixels, 7, size_t(4) * 4 * 4);
  rcti region;
  BLI_rcti_init(&region, 0, 2, 0, 2);
  ASSERT_TRUE(
      BKE_paint_layers_composite_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *ibuf, &region));
  EXPECT_EQ(pixel(*ibuf, 0, 0)[2], 255);
  EXPECT_EQ(pixel(*ibuf, 1, 1)[2], 255);
  EXPECT_EQ(pixel(*ibuf, 3, 3)[2], 7);
  EXPECT_EQ(pixel(*ibuf, 2, 0)[2], 7);
  IMB_freeImBuf(ibuf);
}

TEST_F(PaintLayersCompositeTest, composite_image_float_region_leaves_the_rest)
{
  add_paint_layer("L", add_solid_image("Map", 4, 0, 0, 0, 255, true));

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *dst = BKE_image_add_generated(
      bmain, 4, 4, "Dst", 32, true, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(dst, nullptr);

  /* A sentinel everywhere, then a region: only the region may change, and in place. */
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  float *pixels = ibuf->float_data_for_write();
  for (int i = 0; i < 4 * 4 * 4; i++) {
    pixels[i] = 7.0f;
  }
  BKE_image_release_ibuf(dst, ibuf, lock);

  rcti region;
  BLI_rcti_init(&region, 0, 2, 0, 2);
  ASSERT_TRUE(BKE_paint_layers_composite_image(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *dst, &region, nullptr));

  ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const float *p = ibuf->float_data();
  EXPECT_FLOAT_EQ(p[3], 1.0f);                                   /* (0, 0): written. */
  EXPECT_FLOAT_EQ(p[(int64_t(1) * 4 + 1) * 4 + 3], 1.0f);       /* (1, 1): written. */
  EXPECT_FLOAT_EQ(p[(int64_t(3) * 4 + 3) * 4], 7.0f);           /* (3, 3): untouched. */
  EXPECT_FLOAT_EQ(p[(int64_t(0) * 4 + 2) * 4], 7.0f);           /* (2, 0): untouched. */
  BKE_image_release_ibuf(dst, ibuf, lock);
}

TEST_F(PaintLayersCompositeTest, constant_alpha_is_content_alpha_not_coverage)
{
  /* A map gives the stack its size; the constant above it covers it fully whatever its alpha. */
  add_paint_layer("Map", add_solid_image("Map", 4, 0, 0, 255, 255));
  const float half_red[4] = {1.0f, 0.0f, 0.0f, 0.5f};
  add_constant_layer("Flat", half_red);

  const std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, 0);
  EXPECT_NEAR(p[0], 1.0f, 1e-5f);
  EXPECT_NEAR(p[2], 0.0f, 1e-5f);
  /* The value's alpha is mixed into the result's alpha, it does not scale the factor. */
  EXPECT_NEAR(p[3], 0.5f, 1e-5f);
}

TEST_F(PaintLayersCompositeTest, composite_image_writes_a_float_channel)
{
  add_paint_layer("L", add_solid_image("Map", 4, 0, 0, 0, 255, true));

  /* A float destination: the composite is written scene linear, premultiplied. */
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *dst = BKE_image_add_generated(
      bmain, 4, 4, "Dst", 32, true, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(dst, nullptr);
  ASSERT_TRUE(BKE_paint_layers_composite_image(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *dst, nullptr, nullptr));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const float *pixels = ibuf->float_data();
  ASSERT_NE(pixels, nullptr);
  EXPECT_FLOAT_EQ(pixels[3], 1.0f);
  BKE_image_release_ibuf(dst, ibuf, lock);
}

TEST_F(PaintLayersCompositeTest, composite_image_marks_the_destination_dirty)
{
  add_paint_layer("L", add_solid_image("Map", 4, 0, 0, 0, 255, true));
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *dst = BKE_image_add_generated(
      bmain, 4, 4, "Dst", 32, true, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(dst, nullptr);
  ASSERT_TRUE(BKE_paint_layers_composite_image(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *dst, nullptr, nullptr));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_NE(ibuf->userflags & IB_DISPLAY_BUFFER_INVALID, 0);
  BKE_image_release_ibuf(dst, ibuf, lock);
}

TEST_F(PaintLayersCompositeTest, composite_image_refuses_a_non_rgba_float_destination)
{
  add_paint_layer("L", add_solid_image("Map", 4, 0, 0, 0, 255, true));
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *dst = BKE_image_add_generated(
      bmain, 4, 4, "Dst", 32, true, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(dst, nullptr);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ibuf->channels = 3;
  BKE_image_release_ibuf(dst, ibuf, lock);

  EXPECT_FALSE(BKE_paint_layers_composite_image(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *dst, nullptr, nullptr));

  /* The buffer is the fixture's own: put it back the way it was. */
  ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ibuf->channels = 4;
  BKE_image_release_ibuf(dst, ibuf, lock);
}

TEST_F(PaintLayersCompositeTest, composite_image_refuses_a_channel_without_a_stack)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *dst = BKE_image_add_generated(
      bmain, 4, 4, "Dst", 32, true, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(dst, nullptr);
  EXPECT_FALSE(BKE_paint_layers_composite_image(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *dst, nullptr, nullptr));
}

/**
 * The Image Editor reads a layered material through the composite cache, not through
 * #BKE_paint_layers_composite_channel: its rows have to start from the channel's own neutral value
 * and a flat value has to take part, or what a stroke is judged against differs from the render.
 */
TEST_F(PaintLayersCompositeTest, cache_lays_the_rows_over_the_channels_neutral_value)
{
  const int channel = PAINT_MATERIAL_CHANNEL_ROUGHNESS;
  /* A map that is transparent everywhere shows what is below it: the neutral value, 0.5. */
  Image *map = add_solid_image("Map", 4, 0, 0, 0, 0, true);
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, eMaterialPaintChannel(channel)), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, row, eMaterialPaintChannel(channel), map));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
  const uint64_t map_hash = BKE_paint_material_composite_stack_hash(layers);
  ImBuf *composite = BKE_paint_material_composite_cache_ensure(*ma, channel, layers, map_hash);
  ASSERT_NE(composite, nullptr);
  EXPECT_NEAR(float(composite->byte_data()[0]), 128.0f, 1.0f);

  /* A flat value half-covering it: 0.5 * 0.5 + 0.5 * 1. */
  MaterialPaintLayer *top = BKE_paint_layers_add(*ma, "Top", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, top, eMaterialPaintChannel(channel)), nullptr);
  const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, top, eMaterialPaintChannel(channel), one));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
  ASSERT_EQ(layers.size(), 2);
  const uint64_t value_hash = BKE_paint_material_composite_stack_hash(layers);
  EXPECT_NE(value_hash, map_hash);
  composite = BKE_paint_material_composite_cache_ensure(*ma, channel, layers, value_hash);
  ASSERT_NE(composite, nullptr);
  EXPECT_NEAR(float(composite->byte_data()[0]), 191.0f, 1.0f);

  /* An edit of the flat value alone moves the hash, so the cached pixels are not served stale. */
  const float quarter[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, top, eMaterialPaintChannel(channel), quarter));
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), value_hash);
}

TEST_F(PaintLayersCompositeTest, stack_hash_follows_the_order_of_the_rows)
{
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  MaterialPaintLayer *a = add_paint_layer("A", add_solid_image("A", 4, 10, 10, 10, 255));
  MaterialPaintLayer *b = add_paint_layer("B", add_solid_image("B", 4, 20, 20, 20, 255));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
  const uint64_t before = BKE_paint_material_composite_stack_hash(layers);

  /* The same rows with the same values, reordered: a different composite, so a different hash. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, b, 0));
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), before);
}

/* UV mapping: a Fill's own map is resampled through the row's transform before it is composited. */

TEST_F(PaintLayersCompositeTest, mapping_formula_matches_the_shader_node)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom",
                                               add_solid_image("Bottom", 4, 255, 0, 0, 255));
  const uchar red[4] = {255, 0, 0, 255};
  const uchar green[4] = {0, 255, 0, 255};
  const uchar blue[4] = {0, 0, 255, 255};
  const uchar white[4] = {255, 255, 255, 255};
  MaterialPaintLayer *correction = add_fill_correction(
      bottom, add_quad_image("Map", red, green, blue, white));
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
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(*ma, correction, false));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, aniso));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, correction, 1.57079632679f));
  const float side[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, correction, side));
  p = at(0, 0);
  EXPECT_NEAR(p[0], 0.75f, 1e-3f);
  EXPECT_NEAR(p[1], 0.25f, 1e-3f);
  EXPECT_NEAR(p[2], 0.0f, 1e-3f);
}

/** The mapping reaches a Fill layer's own map, not only a correction's. */
TEST_F(PaintLayersCompositeTest, mapping_remaps_the_map_of_a_fill_layer)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 0, 0, 0, 255));
  const uchar red[4] = {255, 0, 0, 255};
  const uchar green[4] = {0, 255, 0, 255};
  const uchar blue[4] = {0, 0, 255, 255};
  const uchar white[4] = {255, 255, 255, 255};
  MaterialPaintLayer *fill = add_fill_layer("Fill", add_quad_image("Map", red, green, blue, white));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  const float scale2[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, fill, scale2));

  std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 0);
  EXPECT_NEAR(p[0], 0.0f, 1e-4f);
  EXPECT_NEAR(p[1], 1.0f, 1e-4f);
  EXPECT_NEAR(p[2], 0.0f, 1e-4f);
  p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, 1);
  EXPECT_NEAR(p[2], 1.0f, 1e-4f);

  /* Switched off, the row reads its map as it is: the 2x2 map is of another size than the stack,
   * which the evaluator refuses, so only the mapped one composites at all. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, false));
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_FALSE(layers.last().mapping.enabled);
}

/** A mapped Fill mask is resampled before the grey is reduced, so the coverage follows the map. */
TEST_F(PaintLayersCompositeTest, mapping_remaps_the_map_of_a_fill_mask_item)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_CONSTANT, "F", 1.0f);
  ASSERT_NE(item, nullptr);
  const uchar white[4] = {255, 255, 255, 255};
  const uchar black[4] = {0, 0, 0, 255};
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma,
      item,
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      add_quad_image("MaskMap", white, black, black, black)));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, item, true));
  const float scale2[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, item, scale2));

  /* Where the mapped mask is white the top row shows, where it is black the bottom does. */
  std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, 0);
  EXPECT_NEAR(p[0], 0.0f, 1e-3f);
  EXPECT_NEAR(p[2], 1.0f, 1e-3f);
  p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 0);
  EXPECT_NEAR(p[0], 1.0f, 1e-3f);
  EXPECT_NEAR(p[2], 0.0f, 1e-3f);
  p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 2, 0);
  EXPECT_NEAR(p[2], 1.0f, 1e-3f) << "the doubled mask also tiles";
}

/** Identity values read exactly the unmapped pixels. */
TEST_F(PaintLayersCompositeTest, identity_mapping_reads_unchanged_pixels)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom",
                                               add_solid_image("Bottom", 4, 255, 0, 0, 255));
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
  MaterialPaintLayer *correction = add_fill_correction(bottom, map);

  std::array<std::array<float, 4>, 16> before;
  for (const int i : IndexRange(16)) {
    before[i] = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, i % 4, i / 4);
  }
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  for (const int i : IndexRange(16)) {
    const std::array<float, 4> after = linear_pixel(
        PAINT_MATERIAL_CHANNEL_BASE_COLOR, i % 4, i / 4);
    for (const int k : IndexRange(4)) {
      EXPECT_NEAR(after[k], before[i][k], 1e-4f) << i << " " << k;
    }
  }
}

/** The stack hash follows the normalized mapping values of a row, a correction and a mask item. */
TEST_F(PaintLayersCompositeTest, mapping_values_move_the_stack_hash)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom",
                                               add_solid_image("Bottom", 4, 255, 0, 0, 255));
  const uchar red[4] = {255, 0, 0, 255};
  const uchar green[4] = {0, 255, 0, 255};
  const uchar blue[4] = {0, 0, 255, 255};
  const uchar white[4] = {255, 255, 255, 255};
  MaterialPaintLayer *correction = add_fill_correction(
      bottom, add_quad_image("Map", red, green, blue, white));
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
  ASSERT_EQ(layers[0].content_corrections.size(), 1);
  PaintMaterialCompositeMapping &mapping = layers[0].content_corrections[0].mapping;
  ASSERT_TRUE(mapping.enabled);
  mapping.scale[0] = 1.0f;
  mapping.scale[1] = 1.0f;
  const uint64_t hash_one = BKE_paint_material_composite_stack_hash(layers);
  mapping.scale[0] = 0.0f;
  mapping.scale[1] = 0.0f;
  EXPECT_EQ(BKE_paint_material_composite_stack_hash(layers), hash_one);
  mapping.scale[0] = 2.0f;
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), hash_one);
}

/** The stack hash reaches into a folder: a child's mapping offset moves it. */
TEST_F(PaintLayersCompositeTest, stack_hash_follows_a_folder_childs_mapping)
{
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Folder;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, child, MA_PAINT_LAYER_SOURCE_CONSTANT));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma,
      child,
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      add_solid_image("ChildMap", 4, 255, 0, 0, 255)));
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

/** A mapped Normal map rotates and mirrors its tangent-space vectors with the pattern. */
TEST_F(PaintLayersCompositeTest, mapped_normal_map_rotates_and_mirrors_its_vectors)
{
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  add_paint_layer("Bottom", add_solid_image("Flat", 4, 128, 128, 255, 255, true), channel);
  MaterialPaintLayer *fill = add_fill_layer(
      "MappedNormal", add_solid_image("Detail", 4, 204, 128, 230, 255, true), channel);
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
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(*ma, fill, false));
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
  MaterialPaintLayer *fill = add_fill_layer(
      "Mapped",
      add_solid_image("FlagMap", 4, 128, 128, 255, 255, true),
      PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma,
      fill,
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      add_solid_image("FlagColor", 4, 128, 128, 255, 255, true)));
  auto flag_for = [&](const eMaterialPaintChannel channel) {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
    EXPECT_FALSE(layers.is_empty());
    return layers.is_empty() ? false : layers[0].mapping.tangent_normal;
  };
  EXPECT_FALSE(flag_for(PAINT_MATERIAL_CHANNEL_NORMAL));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  EXPECT_TRUE(flag_for(PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_FALSE(flag_for(PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_NORMAL, layers));
  ASSERT_FALSE(layers.is_empty());
  const uint64_t with_flag = BKE_paint_material_composite_stack_hash(layers);
  layers[0].mapping.tangent_normal = false;
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), with_flag);
}

/* UV wrap: an axis that clips reads transparent outside the unit square, so the map's alpha takes
 * the part outside out of the coverage, while an axis that repeats tiles. */

/** The quad map of the wrap cases: red bottom left, green bottom right, blue top left, white top
 * right. */
static const uchar kWrapRed[4] = {255, 0, 0, 255};
static const uchar kWrapGreen[4] = {0, 255, 0, 255};
static const uchar kWrapBlue[4] = {0, 0, 255, 255};
static const uchar kWrapWhite[4] = {255, 255, 255, 255};

TEST_F(PaintLayersCompositeTest, wrap_modes_tile_or_clip_each_axis)
{
  /* A black base shows wherever the map is transparent. */
  MaterialPaintLayer *bottom = add_paint_layer("Bottom",
                                               add_solid_image("Bottom", 4, 0, 0, 0, 255));
  MaterialPaintLayer *correction = add_fill_correction(
      bottom, add_quad_image("Map", kWrapRed, kWrapGreen, kWrapBlue, kWrapWhite));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  /* Scale two puts the texel centres of the 4x4 grid at u, v = 0.25, 0.75, 1.25, 1.75. */
  const float scale2[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, scale2));
  auto expect_color = [&](const int x, const int y, const float r, const float g, const float b) {
    const std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, x, y);
    EXPECT_NEAR(p[0], r, 1e-4f) << x << "," << y;
    EXPECT_NEAR(p[1], g, 1e-4f) << x << "," << y;
    EXPECT_NEAR(p[2], b, 1e-4f) << x << "," << y;
  };

  /* Repeat: both axes tile. */
  ASSERT_EQ(BKE_paint_layers_mapping_wrap_get(*correction), MA_PAINT_LAYER_MAPPING_WRAP_REPEAT);
  expect_color(2, 2, 1.0f, 0.0f, 0.0f);
  expect_color(3, 3, 1.0f, 1.0f, 1.0f);

  /* Clip: one copy, black (the base) outside on both axes. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, correction, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  expect_color(0, 0, 1.0f, 0.0f, 0.0f);
  expect_color(1, 0, 0.0f, 1.0f, 0.0f);
  expect_color(0, 1, 0.0f, 0.0f, 1.0f);
  expect_color(1, 1, 1.0f, 1.0f, 1.0f);
  expect_color(2, 0, 0.0f, 0.0f, 0.0f);
  expect_color(0, 2, 0.0f, 0.0f, 0.0f);
  expect_color(3, 3, 0.0f, 0.0f, 0.0f);

  /* Repeat X: tiles along U, clips along V. */
  ASSERT_TRUE(
      BKE_paint_layers_mapping_set_wrap(*ma, correction, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X));
  expect_color(2, 0, 1.0f, 0.0f, 0.0f);
  expect_color(3, 0, 0.0f, 1.0f, 0.0f);
  expect_color(0, 2, 0.0f, 0.0f, 0.0f);
  expect_color(2, 2, 0.0f, 0.0f, 0.0f);

  /* Repeat Y: tiles along V, clips along U. */
  ASSERT_TRUE(
      BKE_paint_layers_mapping_set_wrap(*ma, correction, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y));
  expect_color(0, 2, 1.0f, 0.0f, 0.0f);
  expect_color(1, 3, 1.0f, 1.0f, 1.0f);
  expect_color(2, 0, 0.0f, 0.0f, 0.0f);
  expect_color(3, 3, 0.0f, 0.0f, 0.0f);
}

TEST_F(PaintLayersCompositeTest, a_clipped_fill_layer_lets_the_rows_below_show)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 0, 40, 200, 255));
  /* What the row below shows on its own, in scene linear. */
  const std::array<float, 4> below = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 3, 3);
  MaterialPaintLayer *fill = add_fill_layer(
      "Fill", add_quad_image("Map", kWrapRed, kWrapGreen, kWrapBlue, kWrapWhite));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  const float scale2[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, fill, scale2));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));

  std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, 0);
  EXPECT_NEAR(p[0], 1.0f, 1e-4f) << "inside the square the map covers";
  EXPECT_NEAR(p[2], 0.0f, 1e-4f);
  /* Outside, the transparent texel shows the row below as it is. */
  p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 3, 3);
  EXPECT_NEAR(p[0], below[0], 1e-4f);
  EXPECT_NEAR(p[1], below[1], 1e-4f);
  EXPECT_NEAR(p[2], below[2], 1e-4f);
}

TEST_F(PaintLayersCompositeTest, a_clipped_mask_item_leaves_the_factor_alone_outside)
{
  add_paint_layer("Bottom", add_solid_image("Bottom", 4, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("Top", 4, 0, 0, 255, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_CONSTANT, "F", 1.0f);
  ASSERT_NE(item, nullptr);
  const uchar black[4] = {0, 0, 0, 255};
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma,
      item,
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      add_quad_image("MaskMap", black, black, black, black)));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, item, true));
  const float scale2[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, item, scale2));

  /* Repeat: the black map hides the top row everywhere. */
  std::array<float, 4> p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 2, 0);
  EXPECT_NEAR(p[0], 1.0f, 1e-4f);
  EXPECT_NEAR(p[2], 0.0f, 1e-4f);

  /* Clip: inside the square the map still hides it, outside it adds nothing to the mask, so the
   * base mask's full factor shows the top row. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, item, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0, 0);
  EXPECT_NEAR(p[0], 1.0f, 1e-4f);
  EXPECT_NEAR(p[2], 0.0f, 1e-4f);
  p = linear_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 2, 0);
  EXPECT_NEAR(p[0], 0.0f, 1e-4f);
  EXPECT_NEAR(p[2], 1.0f, 1e-4f);
}

TEST_F(PaintLayersCompositeTest, the_wrap_moves_the_stack_hash_only_while_the_mapping_applies)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom",
                                               add_solid_image("Bottom", 4, 0, 0, 0, 255));
  MaterialPaintLayer *correction = add_fill_correction(
      bottom, add_quad_image("Map", kWrapRed, kWrapGreen, kWrapBlue, kWrapWhite));
  auto stack_hash = [&]() {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    return BKE_paint_material_composite_stack_hash(layers);
  };
  /* A wrap without an enabled mapping reads nothing: the map is as it is. */
  const uint64_t plain = stack_hash();
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, correction, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  EXPECT_EQ(stack_hash(), plain);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  const uint64_t clipped = stack_hash();
  ASSERT_TRUE(
      BKE_paint_layers_mapping_set_wrap(*ma, correction, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT));
  const uint64_t repeated = stack_hash();
  EXPECT_NE(clipped, repeated);
  ASSERT_TRUE(
      BKE_paint_layers_mapping_set_wrap(*ma, correction, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X));
  EXPECT_NE(stack_hash(), clipped);
  EXPECT_NE(stack_hash(), repeated);
}

/** \} */

}  // namespace blender::bke::tests
