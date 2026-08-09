/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_texture.h"

#include "BLI_index_range.hh"
#include "BLI_math_constants.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

/* For #IMB_BlendMode, which #BKE_paint_material_channel_blend_mode returns as a `short`. */
#include "IMB_imbuf.hh"

namespace blender {

static int paint_material_channel_test_default_visibility()
{
  return (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR) | (1 << PAINT_MATERIAL_CHANNEL_METALLIC) |
         (1 << PAINT_MATERIAL_CHANNEL_ROUGHNESS) | (1 << PAINT_MATERIAL_CHANNEL_SPECULAR) |
         (1 << PAINT_MATERIAL_CHANNEL_NORMAL) | (1 << PAINT_MATERIAL_CHANNEL_HEIGHT) |
         (1 << PAINT_MATERIAL_CHANNEL_ALPHA) | (1 << PAINT_MATERIAL_CHANNEL_AO) |
         (1 << PAINT_MATERIAL_CHANNEL_EMISSION);
}

class PaintMaterialChannelTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  Object *add_mesh_object(const char *name)
  {
    Object *ob = BKE_object_add_only_object(bmain, OB_MESH, name);
    Mesh *mesh = BKE_mesh_add(bmain, name);
    ob->data = id_cast<ID *>(mesh);
    return ob;
  }

  Material *add_material_with_principled(Object &ob, const char *name)
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

    BKE_object_material_assign(bmain, &ob, ma, 1, BKE_MAT_ASSIGN_OBJECT);
    ob.actcol = 1;
    return ma;
  }

  Image *add_image(const char *name)
  {
    return static_cast<Image *>(BKE_id_new(bmain, ID_IM, name));
  }

  bNode *add_image_texture(bNodeTree &ntree, Image &image)
  {
    bNode *tex = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
    tex->id = &image.id;
    return tex;
  }
};

TEST(paint_material_channel, default_value_metallic_is_zero)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_METALLIC), 0.0f);
}

TEST(paint_material_channel, default_value_roughness_is_half)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_ROUGHNESS), 0.5f);
}

TEST(paint_material_channel, default_value_specular_is_half)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_SPECULAR), 0.5f);
}

TEST(paint_material_channel, default_value_normal_is_flat_z)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_NORMAL), 1.0f);
}

TEST(pbr_normal, pack_flat_tangent_byte)
{
  const float n[3] = {0.0f, 0.0f, 1.0f};
  float packed[3];
  BKE_pbr_normal_pack(n, false, packed);
  EXPECT_NEAR(packed[0], 0.5f, 1e-6f);
  EXPECT_NEAR(packed[1], 0.5f, 1e-6f);
  EXPECT_NEAR(packed[2], 1.0f, 1e-6f);
}

TEST(pbr_normal, blend_mix_renormalizes)
{
  const float current_packed[3] = {0.5f, 0.5f, 1.0f}; /* Flat tangent, byte-packed. */
  const float target[3] = {1.0f, 0.0f, 0.0f};
  float result[3];
  BKE_pbr_normal_blend_mix(current_packed, target, 0.5f, false, result);
  float unpacked[3] = {
      result[0] * 2.0f - 1.0f, result[1] * 2.0f - 1.0f, result[2] * 2.0f - 1.0f};
  EXPECT_NEAR(len_v3(unpacked), 1.0f, 1e-5f);
}

TEST_F(PaintMaterialChannelTest, channel_helpers)
{
  PaintModeSettings mode_settings{};
  mode_settings.visible_material_channels = paint_material_channel_test_default_visibility();
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 0;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_CUSTOM].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].value[0] = 0.25f;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].value[0] = 0.5f;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].value[0] = 0.75f;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_CUSTOM].value[0] = 0.1f;
  mode_settings.channel_custom_range[0] = 0.0f;
  mode_settings.channel_custom_range[1] = 1.0f;
  BLI_strncpy(mode_settings.material_paint_custom_attr,
              "my_attr",
              sizeof(mode_settings.material_paint_custom_attr));

  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_SPECULAR));
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_CUSTOM));

  EXPECT_FLOAT_EQ(
      BKE_paint_material_channel_value(
          brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC),
      0.25f);
  EXPECT_FLOAT_EQ(
      BKE_paint_material_channel_value(
          brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_SPECULAR),
      0.75f);
  EXPECT_FLOAT_EQ(
      BKE_paint_material_channel_value(brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_CUSTOM),
      0.1f);

  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC),
      "material_metallic");
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
      "material_roughness");
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_SPECULAR),
      "material_specular");
  EXPECT_EQ(BKE_paint_material_channel_attribute_name(mode_settings,
                                                      PAINT_MATERIAL_CHANNEL_CUSTOM),
            "my_attr");

  mode_settings.material_paint_custom_attr[0] = '\0';
  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_CUSTOM));
}

TEST_F(PaintMaterialChannelTest, channel_blend_mode_only_base_color_is_blendable)
{
  BrushMaterialPaint brush_paint{};
  /* Every channel stores a non-Mix mode, so a channel that reports Mix genuinely ignored it
   * rather than merely reading back a default. */
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    brush_paint.channels[info.channel].blend = IMB_BLEND_MUL;
  }

  /* Base Color and Emission are color channels, so blend modes apply to them. */
  EXPECT_EQ(BKE_paint_material_channel_blend_mode(
                brush_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false),
            IMB_BLEND_MUL);
  EXPECT_EQ(BKE_paint_material_channel_blend_mode(
                brush_paint, PAINT_MATERIAL_CHANNEL_EMISSION, false),
            IMB_BLEND_MUL);

  /* The scalar channels are data: blending them as if they were light is meaningless, so they
   * always interpolate with Mix no matter what is stored. */
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR,
                                              PAINT_MATERIAL_CHANNEL_CUSTOM,
                                              PAINT_MATERIAL_CHANNEL_HEIGHT})
  {
    EXPECT_EQ(BKE_paint_material_channel_blend_mode(brush_paint, channel, false), IMB_BLEND_MIX)
        << "scalar channel " << BKE_paint_material_channel_info(channel).ui_name
        << " must always blend with Mix";
  }
}

TEST_F(PaintMaterialChannelTest, channel_blend_mode_normal_and_invert_are_forced)
{
  BrushMaterialPaint brush_paint{};
  /* A stored mode that must be ignored, to prove the override is not just reading a default. */
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    brush_paint.channels[info.channel].blend = IMB_BLEND_MUL;
  }

  /* Normal must renormalize, so it always uses the dedicated mode -- including while erasing,
   * where blending toward the flat tangent still has to produce a unit vector. */
  EXPECT_EQ(
      BKE_paint_material_channel_blend_mode(brush_paint, PAINT_MATERIAL_CHANNEL_NORMAL, false),
      IMB_BLEND_NORMAL_MIX);
  EXPECT_EQ(BKE_paint_material_channel_blend_mode(brush_paint, PAINT_MATERIAL_CHANNEL_NORMAL, true),
            IMB_BLEND_NORMAL_MIX);

  /* Erasing interpolates toward the channel default, which only plain Mix expresses. This is the
   * one case where Base Color drops its stored mode too. */
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      continue;
    }
    EXPECT_EQ(BKE_paint_material_channel_blend_mode(brush_paint, info.channel, true),
              IMB_BLEND_MIX)
        << "channel " << info.ui_name << " should erase with Mix";
  }
}

TEST_F(PaintMaterialChannelTest, channel_table_is_indexable)
{
  const Span<MaterialPaintChannelInfo> channels = BKE_paint_material_channels();
  EXPECT_EQ(channels.size(), PAINT_MATERIAL_CHANNEL_NUM);
  for (const int i : channels.index_range()) {
    /* Call sites index the table by channel, so the rows must stay in enum order. */
    EXPECT_EQ(int(channels[i].channel), i);
    EXPECT_EQ(&BKE_paint_material_channel_info(channels[i].channel), &channels[i]);
  }

  /* Base Color and Emission are color channels; only the vertex-only channel lacks a socket. */
  EXPECT_TRUE(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_BASE_COLOR).is_color);
  EXPECT_TRUE(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_EMISSION).is_color);
  EXPECT_STREQ(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_NORMAL).ui_name, "Normal");
  EXPECT_STREQ(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_NORMAL).attribute_name,
               "material_paint_normal");
  EXPECT_STREQ(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_NORMAL).socket_name,
               "Normal");
  EXPECT_EQ(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_CUSTOM).socket_name, nullptr);
  EXPECT_EQ(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_CUSTOM).attribute_name,
            nullptr);
}

TEST_F(PaintMaterialChannelTest, channel_from_attribute_name)
{
  EXPECT_EQ(BKE_paint_material_channel_from_attribute_name("material_metallic"),
            PAINT_MATERIAL_CHANNEL_METALLIC);
  EXPECT_EQ(BKE_paint_material_channel_from_attribute_name("Color"),
            PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* An unrelated attribute that merely resembles a channel must not match, otherwise the draw
   * engines would switch shading on ordinary geometry-nodes data. */
  EXPECT_EQ(BKE_paint_material_channel_from_attribute_name("roughness"), std::nullopt);
  EXPECT_EQ(BKE_paint_material_channel_from_attribute_name(""), std::nullopt);
}

TEST_F(PaintMaterialChannelTest, custom_channel_range_clamps_value)
{
  PaintModeSettings mode_settings{};
  mode_settings.visible_material_channels = paint_material_channel_test_default_visibility();
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_CUSTOM].use = 1;
  BLI_strncpy(mode_settings.material_paint_custom_attr,
              "my_attr",
              sizeof(mode_settings.material_paint_custom_attr));

  mode_settings.channel_custom_range[0] = -2.0f;
  mode_settings.channel_custom_range[1] = 3.0f;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_CUSTOM].value[0] = 5.0f;
  EXPECT_FLOAT_EQ(
      BKE_paint_material_channel_value(brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_CUSTOM),
      3.0f);

  const float2 range = BKE_paint_material_channel_range(mode_settings,
                                                        PAINT_MATERIAL_CHANNEL_CUSTOM);
  EXPECT_FLOAT_EQ(range.x, -2.0f);
  EXPECT_FLOAT_EQ(range.y, 3.0f);

  /* An inverted range set through the API must not produce an empty clamp interval. */
  mode_settings.channel_custom_range[0] = 4.0f;
  mode_settings.channel_custom_range[1] = 1.0f;
  const float2 fixed = BKE_paint_material_channel_range(mode_settings,
                                                        PAINT_MATERIAL_CHANNEL_CUSTOM);
  EXPECT_FLOAT_EQ(fixed.x, 1.0f);
  EXPECT_FLOAT_EQ(fixed.y, 4.0f);

  /* The fixed channels ignore the custom range. */
  const float2 metallic = BKE_paint_material_channel_range(mode_settings,
                                                           PAINT_MATERIAL_CHANNEL_METALLIC);
  EXPECT_FLOAT_EQ(metallic.x, 0.0f);
  EXPECT_FLOAT_EQ(metallic.y, 1.0f);
}

TEST_F(PaintMaterialChannelTest, principled_direct_image_found)
{
  Object *ob = add_mesh_object("DirectImageOb");
  Material *ma = add_material_with_principled(*ob, "DirectImageMat");
  Image *image = add_image("MetallicMap");
  bNodeTree &ntree = *ma->nodetree;

  bNode *principled = nullptr;
  for (bNode &node : ntree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);

  bNode *tex = add_image_texture(ntree, *image);
  bke::node_add_link(ntree,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));

  Image *resolved_image = nullptr;
  ImageUser *resolved_iuser = nullptr;
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &resolved_image, &resolved_iuser));
  EXPECT_EQ(resolved_image, image);
  ASSERT_NE(resolved_iuser, nullptr);
  EXPECT_EQ(resolved_iuser, &static_cast<NodeTexImage *>(tex->storage)->iuser);
}

TEST_F(PaintMaterialChannelTest, cache_returns_same_image_as_uncached_lookup)
{
  Object *ob = add_mesh_object("CacheImageOb");
  Material *ma = add_material_with_principled(*ob, "CacheImageMat");
  Image *image = add_image("BaseColorMap");
  bNodeTree &ntree = *ma->nodetree;

  bNode *principled = nullptr;
  for (bNode &node : ntree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);

  bNode *tex = add_image_texture(ntree, *image);
  bke::node_add_link(ntree,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));

  Image *first = nullptr;
  ImageUser *first_iuser = nullptr;
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &first, &first_iuser));
  Image *second = nullptr;
  ImageUser *second_iuser = nullptr;
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &second, &second_iuser));
  EXPECT_EQ(first, second);
  EXPECT_EQ(first_iuser, second_iuser);
}

TEST_F(PaintMaterialChannelTest, principled_image_through_math_not_found)
{
  Object *ob = add_mesh_object("MathImageOb");
  Material *ma = add_material_with_principled(*ob, "MathImageMat");
  Image *image = add_image("MetallicMapMath");
  bNodeTree &ntree = *ma->nodetree;

  bNode *principled = nullptr;
  for (bNode &node : ntree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);

  bNode *tex = add_image_texture(ntree, *image);
  bNode *math = bke::node_add_static_node(nullptr, ntree, SH_NODE_MATH);
  bke::node_add_link(ntree,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                     *math,
                     *bke::node_find_socket(*math, SOCK_IN, "Value"_ustr));
  bke::node_add_link(ntree,
                     *math,
                     *bke::node_find_socket(*math, SOCK_OUT, "Value"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));

  Image *resolved_image = nullptr;
  ImageUser *resolved_iuser = nullptr;
  EXPECT_FALSE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &resolved_image, &resolved_iuser));
  EXPECT_EQ(resolved_image, nullptr);
  EXPECT_EQ(resolved_iuser, nullptr);
}

TEST_F(PaintMaterialChannelTest, no_material_or_no_principled_not_found)
{
  Object *ob_no_mat = add_mesh_object("NoMatOb");
  Image *resolved_image = nullptr;
  ImageUser *resolved_iuser = nullptr;
  EXPECT_FALSE(BKE_paint_principled_channel_image_get(
      *ob_no_mat, PAINT_MATERIAL_CHANNEL_METALLIC, &resolved_image, &resolved_iuser));
  EXPECT_EQ(resolved_image, nullptr);
  EXPECT_EQ(resolved_iuser, nullptr);

  Object *ob = add_mesh_object("EmptyTreeOb");
  Material *ma = BKE_material_add(bmain, "EmptyTreeMat");
  BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBJECT);
  ob->actcol = 1;

  EXPECT_FALSE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &resolved_image, &resolved_iuser));
  EXPECT_EQ(resolved_image, nullptr);
  EXPECT_EQ(resolved_iuser, nullptr);

  EXPECT_FALSE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_CUSTOM, &resolved_image, &resolved_iuser));
}

TEST_F(PaintMaterialChannelTest, hidden_channel_is_not_enabled_for_painting)
{
  PaintModeSettings mode_settings{};
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  mode_settings.visible_material_channels = (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC));

  mode_settings.visible_material_channels |= (1 << PAINT_MATERIAL_CHANNEL_METALLIC);
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC));
}

TEST_F(PaintMaterialChannelTest, vertex_canvas_restricts_to_supported_channels)
{
  /* The Material Paint (vertex color) canvas has no per-vertex representation for Normal,
   * Height, Emission or Custom-without-a-name; #PAINT_CANVAS_SOURCE_MATERIAL must still enable
   * them since it paints into an image/socket, not a vertex attribute. */
  PaintModeSettings mode_settings{};
  mode_settings.visible_material_channels = paint_material_channel_test_default_visibility();
  BLI_strncpy(mode_settings.material_paint_custom_attr,
             "my_attr",
             sizeof(mode_settings.material_paint_custom_attr));
  BrushMaterialPaint brush_paint{};
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    brush_paint.channels[info.channel].use = 1;
  }

  mode_settings.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    EXPECT_TRUE(BKE_paint_material_channel_is_enabled(brush_paint, mode_settings, info.channel))
        << info.ui_name;
  }

  mode_settings.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL_PAINT;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    EXPECT_EQ(BKE_paint_material_channel_is_enabled(brush_paint, mode_settings, info.channel),
             info.supports_vertex_paint)
        << info.ui_name;
  }
}

TEST_F(PaintMaterialChannelTest, alpha_write_and_mask_modes)
{
  PaintModeSettings mode_settings{};
  mode_settings.visible_material_channels = paint_material_channel_test_default_visibility();
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = 1;

  brush_paint.use_alpha_map = 1;
  brush_paint.use_alpha_stroke_mask = 0;
  EXPECT_TRUE(BKE_paint_material_channel_writes_to_target(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_ALPHA));
  EXPECT_FALSE(BKE_paint_material_channel_masks_stroke(brush_paint, mode_settings));

  brush_paint.use_alpha_map = 0;
  brush_paint.use_alpha_stroke_mask = 1;
  EXPECT_FALSE(BKE_paint_material_channel_writes_to_target(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_ALPHA));
  EXPECT_TRUE(BKE_paint_material_channel_masks_stroke(brush_paint, mode_settings));

  brush_paint.use_alpha_map = 1;
  EXPECT_TRUE(BKE_paint_material_channel_writes_to_target(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_ALPHA));
  EXPECT_TRUE(BKE_paint_material_channel_masks_stroke(brush_paint, mode_settings));
}

TEST_F(PaintMaterialChannelTest, BaseColorEnabledAndName)
{
  PaintModeSettings mode_settings;
  mode_settings.visible_material_channels = (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BrushMaterialPaint brush_paint;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
      "Color");
}

TEST_F(PaintMaterialChannelTest, BaseColorGetRespectsInvert)
{
  BrushMaterialPaint brush_paint;
  copy_v3_fl3(brush_paint.base_color, 0.2f, 0.4f, 0.6f);

  Paint paint{};
  paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  paint.runtime->ob_mode = OB_MODE_SCULPT;

  Brush brush{};
  copy_v3_fl3(brush.secondary_color, 0.1f, 0.3f, 0.5f);

  const float3 non_invert = BKE_paint_material_base_color_get(brush_paint, paint, brush, false);
  EXPECT_FLOAT_EQ(non_invert.x, 0.2f);
  EXPECT_FLOAT_EQ(non_invert.y, 0.4f);
  EXPECT_FLOAT_EQ(non_invert.z, 0.6f);

  const float3 inverted = BKE_paint_material_base_color_get(brush_paint, paint, brush, true);
  EXPECT_FLOAT_EQ(inverted.x, 0.1f);
  EXPECT_FLOAT_EQ(inverted.y, 0.3f);
  EXPECT_FLOAT_EQ(inverted.z, 0.5f);

  MEM_delete(paint.runtime);
}

TEST_F(PaintMaterialChannelTest, EnsureMaterialColorAttribute)
{
  Object *ob = add_mesh_object("Mesh");
  Mesh &mesh = *id_cast<Mesh *>(ob->data);
  bool created = false;
  EXPECT_EQ(
      BKE_paint_mesh_material_color_attribute_ensure(
          mesh, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &created),
      MaterialPaintAttributeStatus::Ok);
  EXPECT_TRUE(created);
  created = false;
  EXPECT_EQ(
      BKE_paint_mesh_material_color_attribute_ensure(
          mesh, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &created),
      MaterialPaintAttributeStatus::Ok);
  EXPECT_FALSE(created);
}

TEST_F(PaintMaterialChannelTest, EnsureMaterialColorAttributeEmissionOwnStorage)
{
  /* Regression test: Emission is a color channel (is_color = true) but must not alias Base
   * Color's "Color" attribute, and must not become the mesh's active color attribute. */
  Object *ob = add_mesh_object("Mesh");
  Mesh &mesh = *id_cast<Mesh *>(ob->data);
  bool created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure(
                mesh, PAINT_MATERIAL_CHANNEL_EMISSION, &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_TRUE(created);

  const StringRef base_color_name =
      BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_BASE_COLOR).attribute_name;
  const StringRef emission_name =
      BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_EMISSION).attribute_name;
  EXPECT_NE(base_color_name, emission_name);

  bke::AttributeAccessor attrs = mesh.attributes();
  EXPECT_TRUE(attrs.contains(emission_name));
  EXPECT_FALSE(attrs.contains(base_color_name));
  EXPECT_NE(BKE_id_attributes_active_color_name(&mesh.id), emission_name);
}

TEST_F(PaintMaterialChannelTest, EnsureMaterialAttributeReportsConflicts)
{
  Object *ob = add_mesh_object("ScalarMesh");
  Mesh &mesh = *id_cast<Mesh *>(ob->data);

  bool created = false;
  EXPECT_EQ(BKE_paint_mesh_material_attribute_ensure(mesh, "material_metallic", &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_TRUE(created);

  created = false;
  EXPECT_EQ(BKE_paint_mesh_material_attribute_ensure(mesh, "material_metallic", &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_FALSE(created);

  /* An unconfigured Custom channel must be reported rather than creating a nameless attribute. */
  EXPECT_EQ(BKE_paint_mesh_material_attribute_ensure(mesh, "", &created),
            MaterialPaintAttributeStatus::InvalidName);

  /* A name taken by an incompatible attribute is reported, not silently painted into. */
  mesh.attributes_for_write().add<int>(
      "occupied", bke::AttrDomain::Face, bke::AttributeInitDefaultValue());
  EXPECT_EQ(BKE_paint_mesh_material_attribute_ensure(mesh, "occupied", &created),
            MaterialPaintAttributeStatus::TypeMismatch);
}

TEST_F(PaintMaterialChannelTest, face_active_slot_routing)
{
  Object *ob = add_mesh_object("MultiMatOb");
  add_material_with_principled(*ob, "MatA");
  Material *ma_b = BKE_material_add(bmain, "MatB");
  BKE_object_material_assign(bmain, ob, ma_b, 2, BKE_MAT_ASSIGN_OBJECT);

  ob->actcol = 1;
  EXPECT_TRUE(BKE_paint_material_face_matches_active_slot(*ob, 0));
  EXPECT_FALSE(BKE_paint_material_face_matches_active_slot(*ob, 1));

  ob->actcol = 2;
  EXPECT_FALSE(BKE_paint_material_face_matches_active_slot(*ob, 0));
  EXPECT_TRUE(BKE_paint_material_face_matches_active_slot(*ob, 1));

  /* Single-slot objects skip per-face filtering. */
  Object *single = add_mesh_object("SingleMatOb");
  add_material_with_principled(*single, "OnlyMat");
  EXPECT_TRUE(BKE_paint_material_face_matches_active_slot(*single, 0));
  EXPECT_TRUE(BKE_paint_material_face_matches_active_slot(*single, 1));
}

TEST_F(PaintMaterialChannelTest, principled_normal_map_image_found)
{
  Object *ob = add_mesh_object("NormalMapOb");
  Material *ma = add_material_with_principled(*ob, "NormalMapMat");
  Image *image = add_image("NormalMap");
  bNodeTree &ntree = *ma->nodetree;

  bNode *principled = nullptr;
  for (bNode &node : ntree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);

  bNode *tex = add_image_texture(ntree, *image);
  bNode *nor_map = bke::node_add_static_node(nullptr, ntree, SH_NODE_NORMAL_MAP);
  bke::node_add_link(ntree,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                     *nor_map,
                     *bke::node_find_socket(*nor_map, SOCK_IN, "Color"_ustr));
  bke::node_add_link(ntree,
                     *nor_map,
                     *bke::node_find_socket(*nor_map, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  Image *resolved_image = nullptr;
  ImageUser *resolved_iuser = nullptr;
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_NORMAL, &resolved_image, &resolved_iuser));
  EXPECT_EQ(resolved_image, image);
  ASSERT_NE(resolved_iuser, nullptr);
  EXPECT_EQ(resolved_iuser, &static_cast<NodeTexImage *>(tex->storage)->iuser);
}

TEST_F(PaintMaterialChannelTest, cache_refresh_after_invalidate)
{
  Object *ob = add_mesh_object("InvalidateOb");
  Material *ma = add_material_with_principled(*ob, "InvalidateMat");
  Image *image_a = add_image("MapA");
  Image *image_b = add_image("MapB");
  bNodeTree &ntree = *ma->nodetree;

  bNode *principled = nullptr;
  for (bNode &node : ntree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);

  bNode *tex = add_image_texture(ntree, *image_a);
  bke::node_add_link(ntree,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));

  Image *resolved = nullptr;
  ImageUser *resolved_iuser = nullptr;
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &resolved, &resolved_iuser));
  EXPECT_EQ(resolved, image_a);

  BKE_paint_material_channel_cache_invalidate(ma);
  tex->id = &image_b->id;

  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &resolved, &resolved_iuser));
  EXPECT_EQ(resolved, image_b);
}

TEST_F(PaintMaterialChannelTest, principled_image_target_follows_actcol)
{
  Object *ob = add_mesh_object("ActcolImageOb");
  Material *ma_a = add_material_with_principled(*ob, "ActcolMatA");
  Image *image_a = add_image("BaseColorA");
  Material *ma_b = BKE_material_add(bmain, "ActcolMatB");
  BKE_object_material_assign(bmain, ob, ma_b, 2, BKE_MAT_ASSIGN_OBJECT);
  Image *image_b = add_image("BaseColorB");

  auto link_base_color = [&](Material *ma, Image *image) {
    bNodeTree &ntree = *ma->nodetree;
    bNode *principled = nullptr;
    for (bNode &node : ntree.nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        principled = &node;
        break;
      }
    }
    ASSERT_NE(principled, nullptr);
    bNode *tex = add_image_texture(ntree, *image);
    bke::node_add_link(ntree,
                       *tex,
                       *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  };
  link_base_color(ma_a, image_a);
  link_base_color(ma_b, image_b);

  Image *resolved = nullptr;
  ImageUser *resolved_iuser = nullptr;

  ob->actcol = 1;
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &resolved, &resolved_iuser));
  EXPECT_EQ(resolved, image_a);

  ob->actcol = 2;
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &resolved, &resolved_iuser));
  EXPECT_EQ(resolved, image_b);
}

TEST(material_paint_value_ramp_math, gradient_mode)
{
  EXPECT_EQ(BKE_paint_material_value_gradient_mode(0.0f, 1.0f),
            MaterialPaintValueGradientMode::Unipolar);
  EXPECT_EQ(BKE_paint_material_value_gradient_mode(-1.0f, 1.0f),
            MaterialPaintValueGradientMode::Bipolar);
}

TEST(material_paint_value_ramp_math, unipolar_endpoints)
{
  float rgb[3];
  BKE_paint_material_value_gradient_color(0.0f, 1.0f, 0.0f, rgb);
  EXPECT_FLOAT_EQ(rgb[0], 0.0f);
  EXPECT_FLOAT_EQ(rgb[1], 0.0f);
  EXPECT_FLOAT_EQ(rgb[2], 0.0f);
  BKE_paint_material_value_gradient_color(0.0f, 1.0f, 1.0f, rgb);
  EXPECT_FLOAT_EQ(rgb[0], 1.0f);
  EXPECT_FLOAT_EQ(rgb[1], 1.0f);
  EXPECT_FLOAT_EQ(rgb[2], 1.0f);
}

TEST(material_paint_value_ramp_math, bipolar_center_black)
{
  float rgb[3];
  BKE_paint_material_value_gradient_color(-1.0f, 1.0f, 0.5f, rgb);
  EXPECT_FLOAT_EQ(rgb[0], 0.0f);
  EXPECT_FLOAT_EQ(rgb[1], 0.0f);
  EXPECT_FLOAT_EQ(rgb[2], 0.0f);
  BKE_paint_material_value_gradient_color(-1.0f, 1.0f, 0.0f, rgb);
  EXPECT_FLOAT_EQ(rgb[0], 1.0f);
  BKE_paint_material_value_gradient_color(-1.0f, 1.0f, 1.0f, rgb);
  EXPECT_FLOAT_EQ(rgb[0], 1.0f);
}

TEST(material_paint_value_ramp_math, t_value_roundtrip)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_value_from_t(0.0f, 1.0f, 0.25f), 0.25f);
  EXPECT_FLOAT_EQ(BKE_paint_material_t_from_value(0.0f, 1.0f, 0.25f), 0.25f);
  EXPECT_FLOAT_EQ(BKE_paint_material_value_from_t(-1.0f, 1.0f, 0.5f), 0.0f);
  EXPECT_FLOAT_EQ(BKE_paint_material_t_from_value(-1.0f, 1.0f, 0.0f), 0.5f);
}

TEST(material_paint_value_ramp_math, invert_mirror)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_value_invert(0.0f, 1.0f, 0.25f), 0.75f);
  EXPECT_FLOAT_EQ(BKE_paint_material_value_invert(-1.0f, 1.0f, -0.3f), 0.3f);
  EXPECT_FLOAT_EQ(BKE_paint_material_value_invert(-1.0f, 1.0f, 0.0f), 0.0f);
}

TEST(material_paint_channel, height_descriptor)
{
  const MaterialPaintChannelInfo &info =
      BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_STREQ(info.ui_name, "Height");
  EXPECT_STREQ(info.attribute_name, "material_height");
  EXPECT_EQ(info.socket_name, nullptr);
  EXPECT_FLOAT_EQ(info.value_min, -1.0f);
  EXPECT_FLOAT_EQ(info.value_max, 1.0f);
  EXPECT_FALSE(info.is_color);
  EXPECT_EQ(BKE_paint_material_channels().size(), PAINT_MATERIAL_CHANNEL_NUM);
}

TEST(material_paint_channel, height_from_attribute_name)
{
  EXPECT_EQ(BKE_paint_material_channel_from_attribute_name("material_height"),
            PAINT_MATERIAL_CHANNEL_HEIGHT);
}

TEST(material_paint_channel, height_default_value)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_HEIGHT), 0.0f);
}

TEST(material_paint_channel, alpha_descriptor)
{
  const MaterialPaintChannelInfo &info =
      BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_ALPHA);
  EXPECT_STREQ(info.ui_name, "Alpha");
  EXPECT_STREQ(info.attribute_name, "material_alpha");
  EXPECT_STREQ(info.socket_name, "Alpha");
  EXPECT_FLOAT_EQ(info.value_min, 0.0f);
  EXPECT_FLOAT_EQ(info.value_max, 1.0f);
  EXPECT_FALSE(info.is_color);
}

TEST(material_paint_channel, alpha_default_value)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_ALPHA), 1.0f);
}

TEST(material_paint_channel, ao_descriptor)
{
  const MaterialPaintChannelInfo &info =
      BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_AO);
  EXPECT_STREQ(info.ui_name, "AO");
  EXPECT_STREQ(info.attribute_name, "material_ao");
  EXPECT_EQ(info.socket_name, nullptr);
  EXPECT_FLOAT_EQ(info.value_min, 0.0f);
  EXPECT_FLOAT_EQ(info.value_max, 1.0f);
  EXPECT_FALSE(info.is_color);
}

TEST(material_paint_channel, ao_default_value)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_AO), 1.0f);
}

TEST(material_paint_channel, emission_descriptor)
{
  const MaterialPaintChannelInfo &info =
      BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_EMISSION);
  EXPECT_STREQ(info.ui_name, "Emission");
  EXPECT_STREQ(info.attribute_name, "material_emission");
  EXPECT_STREQ(info.socket_name, "Emission Color");
  EXPECT_FLOAT_EQ(info.value_min, 0.0f);
  EXPECT_FLOAT_EQ(info.value_max, 1.0f);
  EXPECT_TRUE(info.is_color);
}

TEST(material_paint_channel, emission_default_value)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_EMISSION),
                  0.0f);
}

TEST_F(PaintMaterialChannelTest, source_mtex_defaults)
{
  Brush *brush = BKE_brush_add(bmain, "SourceDefaults", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);

  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const MTex &mtex = brush->material_paint->channels[i].source_mtex;
    /* A zeroed MTex would sample nothing: size 0 collapses the texture and map mode 0 is not a
     * valid brush mapping. The in-class DNA initializers must survive brush creation. */
    EXPECT_EQ(mtex.tex, nullptr);
    EXPECT_EQ(mtex.brush_map_mode, MTEX_MAP_MODE_VIEW);
    EXPECT_FLOAT_EQ(mtex.size[0], 1.0f);
    EXPECT_FLOAT_EQ(mtex.size[1], 1.0f);
    EXPECT_FLOAT_EQ(mtex.size[2], 1.0f);
  }
}

TEST_F(PaintMaterialChannelTest, normal_from_sample_flat)
{
  /* #8080FF, the neutral tangent normal stored by every normal map. */
  const float rgb[3] = {0.5f, 0.5f, 1.0f};
  float normal[3];
  EXPECT_TRUE(BKE_paint_material_normal_from_sample(rgb, normal));
  EXPECT_NEAR(normal[0], 0.0f, 1e-5f);
  EXPECT_NEAR(normal[1], 0.0f, 1e-5f);
  EXPECT_NEAR(normal[2], 1.0f, 1e-5f);
}

TEST_F(PaintMaterialChannelTest, normal_from_sample_is_normalized)
{
  /* An off-axis sample whose unpacked vector is not unit length. */
  const float rgb[3] = {1.0f, 1.0f, 1.0f};
  float normal[3];
  EXPECT_TRUE(BKE_paint_material_normal_from_sample(rgb, normal));
  EXPECT_NEAR(len_v3(normal), 1.0f, 1e-5f);
}

TEST_F(PaintMaterialChannelTest, normal_from_sample_degenerate_rejected)
{
  /* Mid gray unpacks to the zero vector; normalizing it would produce NaN, so callers must be
   * told to fall back to the channel's slider value instead. */
  const float rgb[3] = {0.5f, 0.5f, 0.5f};
  float normal[3];
  EXPECT_FALSE(BKE_paint_material_normal_from_sample(rgb, normal));
}

TEST_F(PaintMaterialChannelTest, channel_has_source_follows_tex_pointer)
{
  Brush *brush = BKE_brush_add(bmain, "HasSource", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  BrushMaterialPaintChannel &channel =
      brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS];

  EXPECT_FALSE(BKE_paint_material_channel_has_source(channel));

  Tex *tex = BKE_texture_add(bmain, "SourceTex");
  tex->type = TEX_IMAGE;
  channel.source_mtex.tex = tex;
  EXPECT_TRUE(BKE_paint_material_channel_has_source(channel));

  channel.source_mtex.tex = nullptr;
  EXPECT_FALSE(BKE_paint_material_channel_has_source(channel));
}

}  // namespace blender
