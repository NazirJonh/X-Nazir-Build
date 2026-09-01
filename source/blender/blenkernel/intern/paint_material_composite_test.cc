/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_rect.h"
#include "BLI_uuid.h"

#include <array>
#include <utility>

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

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

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  const uchar *result = pixel(*composite, 0, 0);
  EXPECT_NEAR(result[0], 128, 2);
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
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(160, 128, 255, 255);
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
  stack.layers.append(bottom);
  PaintMaterialCompositeLayer top;
  top.color_ibuf = add_buffer(200, 128, 255, 255);
  top.blend = CompositeBlend::NormalCombine;
  stack.layers.append(top);

  ImBuf *composite = add_buffer(0, 0, 0, 0);
  EXPECT_TRUE(BKE_paint_material_composite_eval(stack, composite));
  EXPECT_NEAR(pixel(*composite, 0, 0)[0], 200, 2);
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

TEST_F(PaintMaterialCompositeEvalTest, float_only_layer_is_rejected)
{
  /* Converting scene-referred float to display-referred byte is a colour management step, not
   * something the evaluator may improvise; such a material belongs to the bake. */
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  PaintMaterialCompositeLayer layer;
  layer.color_ibuf = IMB_allocImBuf(uint(size), uint(size), ImBufFlags::FloatData);
  owned_buffers.append(layer.color_ibuf);
  stack.layers.append(layer);

  ImBuf *composite = add_buffer(0, 0, 0, 255);
  EXPECT_FALSE(BKE_paint_material_composite_eval(stack, composite));
}

TEST_F(PaintMaterialCompositeEvalTest, empty_stack_is_rejected)
{
  PaintMaterialCompositeStack stack;
  stack.width = size;
  stack.height = size;
  ImBuf *composite = add_buffer(0, 0, 0, 255);
  EXPECT_FALSE(BKE_paint_material_composite_eval(stack, composite));
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

  /** A generated byte image, not referenced by any node. */
  Image *add_image(const char *image_name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(
        bmain, 8, 8, image_name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }

  /** An Image Texture node with a generated image assigned. */
  bNode *add_image_texture(Material &ma, const char *image_name)
  {
    Image *image = add_image(image_name);
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

  /** Link the "Color" output of  from_node to the Principled Base Color input. */
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

TEST_F(PaintMaterialCompositeStackTest, tagged_region_is_the_only_part_recomputed)
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
  ImBuf *composite = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &first_revision);
  ASSERT_NE(composite, nullptr);
  ASSERT_EQ(pixel(*composite, 0, 0)[0], 10);
  ASSERT_EQ(pixel(*composite, 7, 7)[0], 10);

  /* The whole layer changes, but only a corner of it is reported. Everything outside that corner
   * has to keep the pixels it had: that is what makes a stroke cost its own area rather than the
   * whole canvas. */
  fill_image(*base, 200);
  rcti region;
  BLI_rcti_init(&region, 0, 2, 0, 2);
  BKE_paint_material_composite_cache_tag_image_region(*base, region);

  uint64_t second_revision = 0;
  ImBuf *refreshed = BKE_paint_material_composite_cache_ensure(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &second_revision);
  ASSERT_EQ(refreshed, composite);
  EXPECT_NE(second_revision, first_revision);
  EXPECT_EQ(pixel(*composite, 0, 0)[0], 200);
  EXPECT_EQ(pixel(*composite, 1, 1)[0], 200);
  EXPECT_EQ(pixel(*composite, 2, 2)[0], 10);
  EXPECT_EQ(pixel(*composite, 7, 7)[0], 10);
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

  /* An image the stack does not read must not make it recompute; the dependency list is what
   * stands between an unrelated paint stroke and a rebuild of every composite in the file. */
  BKE_paint_material_composite_cache_tag_image_changed(*stranger);
  uint64_t revision_after = 0;
  ASSERT_NE(BKE_paint_material_composite_cache_ensure(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, hash, &revision_after),
            nullptr);
  EXPECT_EQ(revision_after, revision);

  BKE_paint_material_composite_cache_tag_image_changed(*base);
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

}  // namespace blender::bke::tests
