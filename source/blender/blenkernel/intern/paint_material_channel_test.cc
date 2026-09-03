/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BKE_appdir.hh"
#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_paint_types.hh"
#include "BKE_scene.hh"
#include "BKE_texture.h"

#include "BLI_fileops.h"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_constants.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

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

  void link_image_to_socket(Material &ma, Image &image, const char *socket)
  {
    bNodeTree &ntree = *ma.nodetree;
    bNode *principled = nullptr;
    for (bNode &node : ntree.nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        principled = &node;
        break;
      }
    }
    BLI_assert(principled != nullptr);
    bNode *tex = add_image_texture(ntree, image);
    bke::node_add_link(ntree,
                       *tex,
                       *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, UString(socket)));
  }
};

TEST(paint_material_channel, default_value_metallic_is_zero)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_METALLIC), 0.0f);
}

TEST(paint_material_channel, default_value_roughness_is_half)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_ROUGHNESS),
                  0.5f);
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
  float unpacked[3] = {result[0] * 2.0f - 1.0f, result[1] * 2.0f - 1.0f, result[2] * 2.0f - 1.0f};
  EXPECT_NEAR(len_v3(unpacked), 1.0f, 1e-5f);
}

TEST_F(PaintMaterialChannelTest, channel_helpers)
{
  PaintModeSettings mode_settings{};
  const int visible_channels = paint_material_channel_test_default_visibility();
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
  BLI_strncpy(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM].attribute_name,
              "my_attr",
              sizeof(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM]
                         .attribute_name));

  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_SPECULAR));
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_CUSTOM));

  EXPECT_FLOAT_EQ(BKE_paint_material_channel_value(
                      brush_paint, mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC),
                  0.25f);
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_value(
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
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_CUSTOM),
      "my_attr");

  mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM].attribute_name[0] = '\0';
  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_CUSTOM));
}

TEST_F(PaintMaterialChannelTest, channel_layer_binding_overrides_fixed_attribute_name)
{
  PaintModeSettings mode_settings{};

  /* No override set: falls back to the fixed name, exactly like today. */
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC),
      "material_metallic");

  BLI_strncpy(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_METALLIC].attribute_name,
              "PBR_Metallic_Layer2",
              sizeof(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_METALLIC]
                         .attribute_name));

  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC),
      "PBR_Metallic_Layer2");

  /* A different channel's override must not leak into this one. */
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
      "material_roughness");

  /* Clearing the override restores the fixed-name fallback. */
  mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_METALLIC].attribute_name[0] = '\0';
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC),
      "material_metallic");
}

TEST_F(PaintMaterialChannelTest, channel_layer_binding_is_the_custom_attribute_name)
{
  PaintModeSettings mode_settings{};

  /* Custom has no fixed name of its own, so its slot in the shared array *is* the attribute
   * name, not an override on top of a fallback: unconfigured must resolve to empty. */
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_CUSTOM),
      "");

  BLI_strncpy(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM].attribute_name,
              "my_attr",
              sizeof(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM]
                         .attribute_name));

  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_CUSTOM),
      "my_attr");

  /* Custom's slot must not leak into (or be affected by) a fixed channel's. */
  EXPECT_EQ(
      BKE_paint_material_channel_attribute_name(mode_settings, PAINT_MATERIAL_CHANNEL_METALLIC),
      "material_metallic");
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
  EXPECT_EQ(
      BKE_paint_material_channel_blend_mode(brush_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false),
      IMB_BLEND_MUL);
  EXPECT_EQ(
      BKE_paint_material_channel_blend_mode(brush_paint, PAINT_MATERIAL_CHANNEL_EMISSION, false),
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
  EXPECT_EQ(
      BKE_paint_material_channel_blend_mode(brush_paint, PAINT_MATERIAL_CHANNEL_NORMAL, true),
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
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_CUSTOM].use = 1;
  BLI_strncpy(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM].attribute_name,
              "my_attr",
              sizeof(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM]
                         .attribute_name));

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

TEST_F(PaintMaterialChannelTest, preferred_display_image_prefers_base_color)
{
  Object *ob = add_mesh_object("PrefBaseOb");
  Material *ma = add_material_with_principled(*ob, "PrefBaseMat");
  Image *base = add_image("PrefBaseColor");
  Image *metallic = add_image("PrefMetallic");
  Image *normal = add_image("PrefNormal");
  link_image_to_socket(*ma, *base, "Base Color");
  link_image_to_socket(*ma, *metallic, "Metallic");
  link_image_to_socket(*ma, *normal, "Normal");

  EXPECT_EQ(BKE_paint_material_preferred_display_image(*ob), base);
}

TEST_F(PaintMaterialChannelTest, preferred_display_image_skips_missing_base_color)
{
  Object *ob = add_mesh_object("PrefSkipBaseOb");
  Material *ma = add_material_with_principled(*ob, "PrefSkipBaseMat");
  Image *metallic = add_image("PrefSkipMetallic");
  Image *normal = add_image("PrefSkipNormal");
  link_image_to_socket(*ma, *metallic, "Metallic");
  link_image_to_socket(*ma, *normal, "Normal");

  EXPECT_EQ(BKE_paint_material_preferred_display_image(*ob), metallic);
}

TEST_F(PaintMaterialChannelTest, preferred_display_image_normal_before_alpha)
{
  Object *ob = add_mesh_object("PrefNormalAlphaOb");
  Material *ma = add_material_with_principled(*ob, "PrefNormalAlphaMat");
  Image *normal = add_image("PrefOnlyNormal");
  Image *alpha = add_image("PrefOnlyAlpha");
  link_image_to_socket(*ma, *normal, "Normal");
  link_image_to_socket(*ma, *alpha, "Alpha");

  EXPECT_EQ(BKE_paint_material_preferred_display_image(*ob), normal);
}

TEST_F(PaintMaterialChannelTest, preferred_display_image_alpha_when_only_map)
{
  Object *ob = add_mesh_object("PrefAlphaOb");
  Material *ma = add_material_with_principled(*ob, "PrefAlphaMat");
  Image *alpha = add_image("PrefSoloAlpha");
  link_image_to_socket(*ma, *alpha, "Alpha");

  EXPECT_EQ(BKE_paint_material_preferred_display_image(*ob), alpha);
}

TEST_F(PaintMaterialChannelTest, preferred_display_image_none_without_maps)
{
  Object *ob = add_mesh_object("PrefEmptyOb");
  add_material_with_principled(*ob, "PrefEmptyMat");

  EXPECT_EQ(BKE_paint_material_preferred_display_image(*ob), nullptr);
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

TEST_F(PaintMaterialChannelTest, channel_image_binding_overrides_principled_resolution)
{
  Object *ob = add_mesh_object("ImageBindingOb");
  Image *layer_image = add_image("LayerImage");

  PaintModeSettings mode_settings{};
  mode_settings.channel_image_bindings[PAINT_MATERIAL_CHANNEL_METALLIC].image = layer_image;

  Image *resolved_image = nullptr;
  ImageUser *resolved_iuser = nullptr;
  /* No material assigned at all - the override must still resolve, since it bypasses the
   * Principled BSDF socket lookup entirely instead of falling back to it. */
  EXPECT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &resolved_image, &resolved_iuser, &mode_settings));
  EXPECT_EQ(resolved_image, layer_image);
  EXPECT_EQ(resolved_iuser,
            &mode_settings.channel_image_bindings[PAINT_MATERIAL_CHANNEL_METALLIC].iuser);

  /* A different channel's override must not leak into this one. */
  Image *other_image = nullptr;
  ImageUser *other_iuser = nullptr;
  EXPECT_FALSE(BKE_paint_principled_channel_image_get(*ob,
                                                       PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                                       &other_image,
                                                       &other_iuser,
                                                       &mode_settings));

  /* Without a mode_settings argument, the override is never consulted - every pre-existing call
   * site keeps its old (socket-only) behavior. */
  Image *unaware_image = nullptr;
  ImageUser *unaware_iuser = nullptr;
  EXPECT_FALSE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &unaware_image, &unaware_iuser));
}

TEST_F(PaintMaterialChannelTest, channel_image_binding_skips_auto_creation)
{
  Object *ob = add_mesh_object("ImageBindingEnsureOb");
  Material *ma = add_material_with_principled(*ob, "ImageBindingEnsureMat");
  Image *layer_image = add_image("LayerImageEnsure");

  PaintModeSettings mode_settings{};
  const int visible_channels = paint_material_channel_test_default_visibility();
  mode_settings.channel_image_bindings[PAINT_MATERIAL_CHANNEL_METALLIC].image = layer_image;

  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;

  const PaintMaterialImagesEnsureResult ensure_result = BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, visible_channels);
  EXPECT_EQ(ensure_result.created, 0);
  EXPECT_FALSE(ensure_result.conflicting_layer_ids);

  /* Overriding a channel must not wire anything into the shader graph - the Principled BSDF's
   * Metallic socket stays exactly as unconnected as it started; any display wiring is the
   * add-on's own responsibility. */
  bNode *principled = nullptr;
  for (bNode &node : ma->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);
  bNodeSocket *socket = bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr);
  ASSERT_NE(socket, nullptr);
  bool has_link = false;
  for (bNodeLink &link : ma->nodetree->links) {
    if (link.tosock == socket) {
      has_link = true;
      break;
    }
  }
  EXPECT_FALSE(has_link);

  /* The stroke target list must resolve to the override image, not create/find a Principled
   * one. */
  const Vector<PaintMaterialImageTarget> targets = BKE_paint_material_image_targets_get(
      *ob, mode_settings, &brush_paint, visible_channels);
  ASSERT_EQ(targets.size(), 1);
  EXPECT_EQ(targets[0].image, layer_image);
}

TEST_F(PaintMaterialChannelTest, image_paint_layer_id_defaults_to_nil)
{
  Image *image = add_image("LayerIdDefault");
  EXPECT_TRUE(BLI_uuid_is_nil(image->paint_layer_id));
}

TEST_F(PaintMaterialChannelTest, image_paint_layer_id_ensure_is_idempotent)
{
  Image *image = add_image("LayerIdEnsure");

  BKE_image_paint_layer_id_ensure(image);
  const bUUID first = image->paint_layer_id;
  EXPECT_FALSE(BLI_uuid_is_nil(first));

  BKE_image_paint_layer_id_ensure(image);
  EXPECT_TRUE(BLI_uuid_equal(first, image->paint_layer_id));
}

TEST_F(PaintMaterialChannelTest, images_ensure_stamps_one_layer_id_on_fresh_material)
{
  Object *ob = add_mesh_object("StampFreshOb");
  add_material_with_principled(*ob, "StampFreshMat");

  PaintModeSettings mode_settings{};
  mode_settings.new_channel_image_size = PAINT_NEW_CHANNEL_IMAGE_SIZE_256;
  const int visible_channels = paint_material_channel_test_default_visibility();

  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 1;

  const PaintMaterialImagesEnsureResult res = BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, visible_channels);

  EXPECT_GT(res.created, 0);
  EXPECT_FALSE(res.conflicting_layer_ids);

  Image *base = nullptr;
  Image *rough = nullptr;
  ImageUser *iuser = nullptr;
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &base, &iuser));
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_ROUGHNESS, &rough, &iuser));
  EXPECT_FALSE(BLI_uuid_is_nil(base->paint_layer_id));
  EXPECT_TRUE(BLI_uuid_equal(base->paint_layer_id, rough->paint_layer_id));
}

TEST_F(PaintMaterialChannelTest, images_ensure_joins_single_existing_layer_id)
{
  Object *ob = add_mesh_object("StampJoinOb");
  Material *ma = add_material_with_principled(*ob, "StampJoinMat");

  Image *base = add_image("StampJoinBase");
  base->paint_layer_id = BLI_uuid_generate_random();
  const bUUID existing = base->paint_layer_id;
  link_image_to_socket(*ma, *base, "Base Color");

  PaintModeSettings mode_settings{};
  mode_settings.new_channel_image_size = PAINT_NEW_CHANNEL_IMAGE_SIZE_256;
  const int visible_channels = paint_material_channel_test_default_visibility();

  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 1;

  const PaintMaterialImagesEnsureResult res = BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, visible_channels);

  EXPECT_GT(res.created, 0);
  EXPECT_FALSE(res.conflicting_layer_ids);
  EXPECT_TRUE(BLI_uuid_equal(base->paint_layer_id, existing)); /* untouched */

  Image *rough = nullptr;
  ImageUser *iuser = nullptr;
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_ROUGHNESS, &rough, &iuser));
  EXPECT_TRUE(BLI_uuid_equal(rough->paint_layer_id, existing));
}

TEST_F(PaintMaterialChannelTest, images_ensure_flags_conflicting_layer_ids)
{
  Object *ob = add_mesh_object("StampConflictOb");
  Material *ma = add_material_with_principled(*ob, "StampConflictMat");

  Image *base = add_image("StampConflictBase");
  base->paint_layer_id = BLI_uuid_generate_random();
  const bUUID id_a = base->paint_layer_id;
  link_image_to_socket(*ma, *base, "Base Color");

  Image *metallic = add_image("StampConflictMetallic");
  metallic->paint_layer_id = BLI_uuid_generate_random();
  const bUUID id_b = metallic->paint_layer_id;
  link_image_to_socket(*ma, *metallic, "Metallic");

  PaintModeSettings mode_settings{};
  mode_settings.new_channel_image_size = PAINT_NEW_CHANNEL_IMAGE_SIZE_256;
  const int visible_channels = paint_material_channel_test_default_visibility();

  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 1;

  const PaintMaterialImagesEnsureResult res = BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, visible_channels);

  EXPECT_GT(res.created, 0);
  EXPECT_TRUE(res.conflicting_layer_ids);
  EXPECT_TRUE(BLI_uuid_equal(base->paint_layer_id, id_a));     /* untouched */
  EXPECT_TRUE(BLI_uuid_equal(metallic->paint_layer_id, id_b)); /* untouched */

  Image *rough = nullptr;
  ImageUser *iuser = nullptr;
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_ROUGHNESS, &rough, &iuser));
  EXPECT_FALSE(BLI_uuid_is_nil(rough->paint_layer_id));
  /* Smoke check of the generator, not a uniqueness proof (spec 5.2). */
  EXPECT_FALSE(BLI_uuid_equal(rough->paint_layer_id, id_a));
  EXPECT_FALSE(BLI_uuid_equal(rough->paint_layer_id, id_b));
}

TEST_F(PaintMaterialChannelTest, images_ensure_noop_run_changes_nothing)
{
  Object *ob = add_mesh_object("StampNoopOb");
  add_material_with_principled(*ob, "StampNoopMat");

  PaintModeSettings mode_settings{};
  mode_settings.new_channel_image_size = PAINT_NEW_CHANNEL_IMAGE_SIZE_256;
  const int visible_channels = paint_material_channel_test_default_visibility();

  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;

  /* First run creates the maps. */
  BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, visible_channels);

  Image *base = nullptr;
  Image *metallic = nullptr;
  ImageUser *iuser = nullptr;
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &base, &iuser));
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_METALLIC, &metallic, &iuser));
  const bUUID base_id = base->paint_layer_id;
  const bUUID metallic_id = metallic->paint_layer_id;
  const int images_before = BLI_listbase_count(&bmain->images);

  /* Second run creates nothing. */
  const PaintMaterialImagesEnsureResult res = BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, visible_channels);

  EXPECT_EQ(res.created, 0);
  EXPECT_FALSE(res.conflicting_layer_ids);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
  EXPECT_TRUE(BLI_uuid_equal(base->paint_layer_id, base_id));
  EXPECT_TRUE(BLI_uuid_equal(metallic->paint_layer_id, metallic_id));
}

TEST_F(PaintMaterialChannelTest, hidden_channel_is_not_enabled_for_painting)
{
  PaintModeSettings mode_settings{};
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  int visible_channels = (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_METALLIC));

  visible_channels |= (1 << PAINT_MATERIAL_CHANNEL_METALLIC);
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_METALLIC));
}

TEST_F(PaintMaterialChannelTest, vertex_canvas_restricts_to_supported_channels)
{
  /* The Material Paint (vertex color) canvas has no per-vertex representation for Normal,
   * Height, Emission or Custom-without-a-name; #PAINT_CANVAS_SOURCE_MATERIAL must still enable
   * them since it paints into an image/socket, not a vertex attribute. */
  PaintModeSettings mode_settings{};
  const int visible_channels = paint_material_channel_test_default_visibility();
  BLI_strncpy(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM].attribute_name,
              "my_attr",
              sizeof(mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_CUSTOM]
                         .attribute_name));
  BrushMaterialPaint brush_paint{};
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    brush_paint.channels[info.channel].use = 1;
  }

  mode_settings.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
                    brush_paint, mode_settings, visible_channels, info.channel))
        << info.ui_name;
  }

  mode_settings.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL_PAINT;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    EXPECT_EQ(BKE_paint_material_channel_is_enabled(
                  brush_paint, mode_settings, visible_channels, info.channel),
              info.supports_vertex_paint)
        << info.ui_name;
  }
}

TEST_F(PaintMaterialChannelTest, alpha_write_and_mask_modes)
{
  PaintModeSettings mode_settings{};
  const int visible_channels = paint_material_channel_test_default_visibility();
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = 1;

  brush_paint.use_alpha_map = 1;
  brush_paint.use_alpha_stroke_mask = 0;
  EXPECT_TRUE(BKE_paint_material_channel_writes_to_target(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_ALPHA));
  EXPECT_FALSE(BKE_paint_material_channel_masks_stroke(
      brush_paint, mode_settings, visible_channels));

  brush_paint.use_alpha_map = 0;
  brush_paint.use_alpha_stroke_mask = 1;
  EXPECT_FALSE(BKE_paint_material_channel_writes_to_target(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_ALPHA));
  EXPECT_TRUE(BKE_paint_material_channel_masks_stroke(
      brush_paint, mode_settings, visible_channels));

  brush_paint.use_alpha_map = 1;
  EXPECT_TRUE(BKE_paint_material_channel_writes_to_target(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_ALPHA));
  EXPECT_TRUE(BKE_paint_material_channel_masks_stroke(
      brush_paint, mode_settings, visible_channels));
}

TEST_F(PaintMaterialChannelTest, HeightDoesNotWriteToTarget)
{
  PaintModeSettings mode_settings{};
  const int visible_channels = paint_material_channel_test_default_visibility();
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_HEIGHT].use = 1;

  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_HEIGHT))
      << "visibility/use still describe the channel; writes_to_target is the backend gate";
  EXPECT_FALSE(BKE_paint_material_channel_writes_to_target(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_HEIGHT));
}

TEST_F(PaintMaterialChannelTest, BaseColorEnabledAndName)
{
  PaintModeSettings mode_settings;
  int visible_channels = (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BrushMaterialPaint brush_paint;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(
      brush_paint, mode_settings, visible_channels, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
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

TEST_F(PaintMaterialChannelTest, ChannelColorGetEmissionUsesChannelValueNotBaseColor)
{
  BrushMaterialPaint brush_paint;
  copy_v3_fl3(brush_paint.base_color, 1.0f, 0.0f, 0.0f);
  copy_v3_fl3(brush_paint.channels[PAINT_MATERIAL_CHANNEL_EMISSION].value, 0.2f, 0.4f, 0.8f);

  Paint paint{};
  paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  paint.runtime->ob_mode = OB_MODE_SCULPT;
  Brush brush{};
  copy_v3_fl3(brush.secondary_color, 0.0f, 1.0f, 0.0f);

  const float3 emission = BKE_paint_material_channel_color_get(
      brush_paint, paint, brush, PAINT_MATERIAL_CHANNEL_EMISSION, false);
  EXPECT_FLOAT_EQ(emission.x, 0.2f);
  EXPECT_FLOAT_EQ(emission.y, 0.4f);
  EXPECT_FLOAT_EQ(emission.z, 0.8f);

  const float3 emission_erase = BKE_paint_material_channel_color_get(
      brush_paint, paint, brush, PAINT_MATERIAL_CHANNEL_EMISSION, true);
  EXPECT_FLOAT_EQ(emission_erase.x, 0.0f);
  EXPECT_FLOAT_EQ(emission_erase.y, 0.0f);
  EXPECT_FLOAT_EQ(emission_erase.z, 0.0f);

  const float3 base = BKE_paint_material_channel_color_get(
      brush_paint, paint, brush, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false);
  EXPECT_FLOAT_EQ(base.x, 1.0f);
  EXPECT_FLOAT_EQ(base.y, 0.0f);
  EXPECT_FLOAT_EQ(base.z, 0.0f);

  MEM_delete(paint.runtime);
}

TEST_F(PaintMaterialChannelTest, EnsureMaterialColorAttribute)
{
  Object *ob = add_mesh_object("Mesh");
  Mesh &mesh = *id_cast<Mesh *>(ob->data);
  bool created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure(
                mesh, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_TRUE(created);
  created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure(
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

TEST_F(PaintMaterialChannelTest, EnsureMaterialColorAttributeNamedCreatesArbitraryAttribute)
{
  Object *ob = add_mesh_object("Mesh");
  Mesh &mesh = *id_cast<Mesh *>(ob->data);
  bool created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure_named(
                mesh, "PBR_Metallic_Layer2", &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_TRUE(created);

  bke::AttributeAccessor attrs = mesh.attributes();
  EXPECT_TRUE(attrs.contains("PBR_Metallic_Layer2"));

  created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure_named(
                mesh, "PBR_Metallic_Layer2", &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_FALSE(created);
}

TEST_F(PaintMaterialChannelTest, EnsureMaterialColorAttributeNamedDoesNotTouchActiveColor)
{
  /* Regression test for the §1b fix: creating a layer attribute must never make it the mesh's
   * active/default color attribute - that would fight over a single mesh-wide flag every time a
   * new layer is created. */
  Object *ob = add_mesh_object("Mesh");
  Mesh &mesh = *id_cast<Mesh *>(ob->data);
  bool created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure_named(
                mesh, "PBR_Metallic_Layer2", &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_TRUE(created);
  EXPECT_NE(BKE_id_attributes_active_color_name(&mesh.id), "PBR_Metallic_Layer2");
}

TEST_F(PaintMaterialChannelTest, EnsureMaterialColorAttributeNamedRejectsIncompatibleType)
{
  Object *ob = add_mesh_object("Mesh");
  Mesh &mesh = *id_cast<Mesh *>(ob->data);
  bke::MutableAttributeAccessor attrs = mesh.attributes_for_write();
  attrs.add<float>("PBR_Metallic_Layer2", bke::AttrDomain::Point, bke::AttributeInitDefaultValue());

  bool created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure_named(
                mesh, "PBR_Metallic_Layer2", &created),
            MaterialPaintAttributeStatus::TypeMismatch);
  EXPECT_FALSE(created);
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
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      PAINT_MATERIAL_CHANNEL_HEIGHT);
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
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      PAINT_MATERIAL_CHANNEL_ALPHA);
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
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      PAINT_MATERIAL_CHANNEL_AO);
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
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      PAINT_MATERIAL_CHANNEL_EMISSION);
  EXPECT_STREQ(info.ui_name, "Emission");
  EXPECT_STREQ(info.attribute_name, "material_emission");
  EXPECT_STREQ(info.socket_name, "Emission Color");
  EXPECT_FLOAT_EQ(info.value_min, 0.0f);
  EXPECT_FLOAT_EQ(info.value_max, 1.0f);
  EXPECT_TRUE(info.is_color);
}

TEST(material_paint_channel, emission_default_value)
{
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_default_value(PAINT_MATERIAL_CHANNEL_EMISSION), 0.0f);
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

TEST_F(PaintMaterialChannelTest, preview_mtex_prefers_base_color_then_alpha)
{
  PaintModeSettings mode_settings{};
  const int visible_channels = paint_material_channel_test_default_visibility();
  BrushMaterialPaint brush_paint{};
  Tex *base_tex = BKE_texture_add(bmain, "PreviewBase");
  Tex *alpha_tex = BKE_texture_add(bmain, "PreviewAlpha");
  Tex *metallic_tex = BKE_texture_add(bmain, "PreviewMetallic");
  Tex *normal_tex = BKE_texture_add(bmain, "PreviewNormal");

  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex = base_tex;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].source_mtex.tex = alpha_tex;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].source_mtex.tex = metallic_tex;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_NORMAL].source_mtex.tex = normal_tex;

  MTex preview{};
  EXPECT_TRUE(BKE_paint_material_preview_mtex_get(brush_paint, mode_settings, visible_channels, preview));
  EXPECT_EQ(preview.tex, base_tex);

  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 0;
  EXPECT_TRUE(BKE_paint_material_preview_mtex_get(brush_paint, mode_settings, visible_channels, preview));
  EXPECT_EQ(preview.tex, alpha_tex);

  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = 0;
  EXPECT_TRUE(BKE_paint_material_preview_mtex_get(brush_paint, mode_settings, visible_channels, preview));
  EXPECT_EQ(preview.tex, metallic_tex);

  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 0;
  EXPECT_TRUE(BKE_paint_material_preview_mtex_get(brush_paint, mode_settings, visible_channels, preview));
  EXPECT_EQ(preview.tex, normal_tex);
}

TEST_F(PaintMaterialChannelTest, preview_mtex_normal_is_last)
{
  PaintModeSettings mode_settings{};
  const int visible_channels = paint_material_channel_test_default_visibility();
  BrushMaterialPaint brush_paint{};
  Tex *roughness_tex = BKE_texture_add(bmain, "PreviewRoughnessLast");
  Tex *normal_tex = BKE_texture_add(bmain, "PreviewNormalLast");

  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].source_mtex.tex = roughness_tex;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_NORMAL].source_mtex.tex = normal_tex;

  MTex preview{};
  EXPECT_TRUE(BKE_paint_material_preview_mtex_get(brush_paint, mode_settings, visible_channels, preview));
  EXPECT_EQ(preview.tex, roughness_tex);
}

TEST_F(PaintMaterialChannelTest, MaterialPaintCopyIsIndependent)
{
  Brush *brush = BKE_brush_add(bmain, "CopySource", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  Image *image = add_image("SourceTex");
  Tex *tex = BKE_texture_add(bmain, "SourceTexWrapper");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex = tex;
  id_us_plus(&tex->id);
  const int tex_users_before = tex->id.us;

  BrushMaterialPaint *copy = BKE_brush_material_paint_copy(*brush->material_paint, 0);

  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex, tex);
  EXPECT_EQ(tex->id.us, tex_users_before + 1) << "copy must take its own counted reference";

  /* Mutating the copy's channel pointer must not affect the source's. The reference the copy
   * held is released by hand here, standing in for the owning ID's unlink pass - freeing the
   * copy deliberately does not touch user counts. */
  copy->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex = nullptr;
  id_us_min(&tex->id);
  EXPECT_EQ(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex,
            tex);

  BKE_brush_material_paint_free(copy);
  EXPECT_EQ(tex->id.us, tex_users_before);
}

TEST_F(PaintMaterialChannelTest, MaterialPaintCopyWithoutRefcountSkipsUserCounts)
{
  Brush *brush = BKE_brush_add(bmain, "NoRefcountSource", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  Image *image = add_image("NoRefcountTex");
  Tex *tex = BKE_texture_add(bmain, "NoRefcountTexWrapper");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex = tex;
  id_us_plus(&tex->id);
  const int tex_users_before = tex->id.us;

  /* Copies made for evaluated/no-main data must not touch user counts, or every depsgraph
   * evaluation of the scene would inflate them. */
  BrushMaterialPaint *copy = BKE_brush_material_paint_copy(*brush->material_paint,
                                                           LIB_ID_CREATE_NO_USER_REFCOUNT);
  EXPECT_EQ(tex->id.us, tex_users_before);

  BKE_brush_material_paint_free(copy);
  EXPECT_EQ(tex->id.us, tex_users_before);
}

TEST_F(PaintMaterialChannelTest, MaterialPaintCopyIntoSwapsTexReferenceCounts)
{
  Brush *brush_a = BKE_brush_add(bmain, "SwapA", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush_a);
  Brush *brush_b = BKE_brush_add(bmain, "SwapB", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush_b);

  Image *image_a = add_image("TexA");
  Tex *tex_a = BKE_texture_add(bmain, "TexWrapperA");
  tex_a->type = TEX_IMAGE;
  tex_a->ima = image_a;
  id_us_plus(&image_a->id);
  brush_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].source_mtex.tex = tex_a;
  id_us_plus(&tex_a->id);

  Image *image_b = add_image("TexB");
  Tex *tex_b = BKE_texture_add(bmain, "TexWrapperB");
  tex_b->type = TEX_IMAGE;
  tex_b->ima = image_b;
  id_us_plus(&image_b->id);
  brush_b->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].source_mtex.tex = tex_b;
  id_us_plus(&tex_b->id);

  const int tex_a_users_before = tex_a->id.us;
  const int tex_b_users_before = tex_b->id.us;

  /* dst (brush_a's material_paint, currently referencing tex_a) adopts src's (tex_b). */
  BKE_brush_material_paint_copy_into(*brush_a->material_paint, *brush_b->material_paint);

  EXPECT_EQ(brush_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].source_mtex.tex,
            tex_b);
  EXPECT_EQ(tex_b->id.us, tex_b_users_before + 1) << "dst now also references tex_b";
  EXPECT_EQ(tex_a->id.us, tex_a_users_before - 1) << "dst dropped its reference to tex_a";
}

TEST_F(PaintMaterialChannelTest, MaterialPaintCopyIntoSelfAssignIsNoOp)
{
  Brush *brush = BKE_brush_add(bmain, "SelfAssign", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  Image *image = add_image("SelfTex");
  Tex *tex = BKE_texture_add(bmain, "SelfTexWrapper");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].source_mtex.tex = tex;
  id_us_plus(&tex->id);
  const int tex_users_before = tex->id.us;

  BKE_brush_material_paint_copy_into(*brush->material_paint, *brush->material_paint);

  EXPECT_EQ(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].source_mtex.tex,
            tex);
  EXPECT_EQ(tex->id.us, tex_users_before) << "self-assign must not double-count or drop";
}

TEST_F(PaintMaterialChannelTest, ToolSettingsCopyDeepCopiesPresetList)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "PresetScene"));
  PaintMaterialBrushPreset *preset = MEM_new<PaintMaterialBrushPreset>(__func__);
  preset->local_name = BLI_strdup("TestBrush");
  preset->material_paint = MEM_new<BrushMaterialPaint>(__func__);
  BLI_addtail(&scene->toolsettings->paint_mode.material_paint_brush_presets, preset);

  ToolSettings *copy = BKE_toolsettings_copy(scene->toolsettings, 0);

  ASSERT_FALSE(copy->paint_mode.material_paint_brush_presets.is_empty());
  PaintMaterialBrushPreset &copied_preset = *copy->paint_mode.material_paint_brush_presets.begin();
  EXPECT_NE(&copied_preset, preset) << "must be a distinct node, not the same pointer";
  EXPECT_NE(copied_preset.local_name, preset->local_name)
      << "local_name must be a distinct allocation";
  EXPECT_STREQ(copied_preset.local_name, "TestBrush");
  EXPECT_NE(copied_preset.material_paint, preset->material_paint)
      << "material_paint must be a distinct allocation";

  BKE_toolsettings_free(copy);
  EXPECT_EQ(scene->toolsettings->paint_mode.material_paint_brush_presets.count(), 1);
  PaintMaterialBrushPreset &still_there =
      *scene->toolsettings->paint_mode.material_paint_brush_presets.begin();
  EXPECT_STREQ(still_there.local_name, "TestBrush");

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, PresetFindReturnsNullBeforeEnsure)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "FindScene"));
  Brush *brush = BKE_brush_add(bmain, "Unfound", OB_MODE_SCULPT);

  EXPECT_EQ(BKE_paint_material_brush_preset_find(*scene, *brush), nullptr);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, PresetEnsureCreatesAndFindLocatesByLocalName)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "EnsureScene"));
  Brush *brush = BKE_brush_add(bmain, "LocalBrush", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;

  PaintMaterialBrushPreset *created = BKE_paint_material_brush_preset_ensure(*scene, *brush);

  ASSERT_NE(created, nullptr);
  ASSERT_NE(created->material_paint, nullptr);
  EXPECT_TRUE(created->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use)
      << "ensure must seed a freshly created preset from the brush's current state";
  EXPECT_EQ(BKE_paint_material_brush_preset_find(*scene, *brush), created);

  EXPECT_EQ(BKE_paint_material_brush_preset_ensure(*scene, *brush), created);
  EXPECT_EQ(scene->toolsettings->paint_mode.material_paint_brush_presets.count(), 1);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, PresetEnsureSeedsDefaultsWhenBrushHasNoMaterialPaintYet)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "DefaultsScene"));
  Brush *brush = BKE_brush_add(bmain, "NeverTouched", OB_MODE_SCULPT);
  ASSERT_EQ(brush->material_paint, nullptr);

  PaintMaterialBrushPreset *created = BKE_paint_material_brush_preset_ensure(*scene, *brush);

  ASSERT_NE(created, nullptr);
  ASSERT_NE(created->material_paint, nullptr)
      << "must seed from BKE_brush_material_paint_ensure's own defaults, not crash on null";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, PresetKeyDoesNotCollideBetweenTwoLocalBrushes)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "CollisionScene"));
  Brush *brush_a = BKE_brush_add(bmain, "BrushA", OB_MODE_SCULPT);
  Brush *brush_b = BKE_brush_add(bmain, "BrushB", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush_a);
  BKE_brush_material_paint_ensure(brush_b);
  brush_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  brush_b->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 0;

  PaintMaterialBrushPreset *preset_a = BKE_paint_material_brush_preset_ensure(*scene, *brush_a);
  PaintMaterialBrushPreset *preset_b = BKE_paint_material_brush_preset_ensure(*scene, *brush_b);

  EXPECT_NE(preset_a, preset_b);
  EXPECT_TRUE(preset_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use);
  EXPECT_FALSE(preset_b->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, ApplyDoesNotCreateMaterialPaintWithoutPreset)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "NoPresetApplyScene"));
  Brush *brush = BKE_brush_add(bmain, "NeverOptedIn", OB_MODE_SCULPT);
  ASSERT_EQ(brush->material_paint, nullptr);

  BKE_paint_material_brush_preset_apply(*scene, *brush);

  EXPECT_EQ(brush->material_paint, nullptr)
      << "switching onto a brush must not opt it into PBR Paint";
  EXPECT_EQ(BKE_paint_material_brush_preset_find(*scene, *brush), nullptr);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, ApplyCopiesPresetOntoBrush)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "ApplyScene"));
  Brush *brush = BKE_brush_add(bmain, "ApplyTarget", OB_MODE_SCULPT);
  PaintMaterialBrushPreset *preset = BKE_paint_material_brush_preset_ensure(*scene, *brush);
  preset->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = 1;
  preset->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].value[2] = 0.5f;

  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = 0;

  BKE_paint_material_brush_preset_apply(*scene, *brush);

  ASSERT_NE(brush->material_paint, nullptr);
  EXPECT_TRUE(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].use);
  EXPECT_FLOAT_EQ(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].value[2], 0.5f);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, EnableAddedVisibleChannelsSetsUse)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "VisibleUseScene"));
  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  sculpt_data->paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  Brush *brush = BKE_brush_add(bmain, "VisibleUseBrush", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = 0;
  sculpt_data->paint.brush = brush;

  BKE_paint_material_enable_added_visible_channels(sculpt_data->paint,
                                                   1 << PAINT_MATERIAL_CHANNEL_ALPHA);

  EXPECT_TRUE(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ALPHA].use);

  BKE_id_free(bmain, scene);
}

/**
 * Scene with a Sculpt/Image Paint pair on the Material canvas, each with its own brush, ready for
 * the brush-sync entry points.
 */
static Scene *paint_material_sync_test_scene_add(Main *bmain,
                                                 const char *name,
                                                 Brush **r_sculpt_brush,
                                                 Brush **r_image_brush)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, name));
  ToolSettings *ts = scene->toolsettings;
  ts->paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;

  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  ts->sculpt = sculpt_data;
  sculpt_data->paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  sculpt_data->paint.runtime->ob_mode = OB_MODE_SCULPT;
  if (ts->imapaint.paint.runtime == nullptr) {
    ts->imapaint.paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  }
  ts->imapaint.paint.runtime->ob_mode = OB_MODE_TEXTURE_PAINT;

  Brush *sculpt_brush = BKE_brush_add(bmain, "SyncSculptBrush", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(sculpt_brush);
  sculpt_data->paint.brush = sculpt_brush;

  Brush *image_brush = BKE_brush_add(bmain, "SyncImageBrush", OB_MODE_TEXTURE_PAINT);
  BKE_brush_material_paint_ensure(image_brush);
  ts->imapaint.paint.brush = image_brush;

  *r_sculpt_brush = sculpt_brush;
  *r_image_brush = image_brush;
  return scene;
}

TEST_F(PaintMaterialChannelTest, VisibleChannelsAreIndependentPerPaint)
{
  Brush *sculpt_brush = nullptr;
  Brush *image_brush = nullptr;
  Scene *scene = paint_material_sync_test_scene_add(
      bmain, "IndependentScene", &sculpt_brush, &image_brush);
  ToolSettings *ts = scene->toolsettings;
  ASSERT_NE(sculpt_brush, image_brush);

  ts->sculpt->paint.visible_material_channels = (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ts->imapaint.paint.visible_material_channels = (1 << PAINT_MATERIAL_CHANNEL_METALLIC);

  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  const PaintModeSettings &mode_settings = ts->paint_mode;

  EXPECT_TRUE(
      BKE_paint_material_channel_is_enabled(brush_paint,
                                            mode_settings,
                                            ts->sculpt->paint.visible_material_channels,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(
      BKE_paint_material_channel_is_enabled(brush_paint,
                                            mode_settings,
                                            ts->sculpt->paint.visible_material_channels,
                                            PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(
      BKE_paint_material_channel_is_enabled(brush_paint,
                                            mode_settings,
                                            ts->imapaint.paint.visible_material_channels,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(
      BKE_paint_material_channel_is_enabled(brush_paint,
                                            mode_settings,
                                            ts->imapaint.paint.visible_material_channels,
                                            PAINT_MATERIAL_CHANNEL_METALLIC));

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, DirectionalBrushSyncCopiesVisibleChannels)
{
  Brush *sculpt_brush = nullptr;
  Brush *image_brush = nullptr;
  Scene *scene = paint_material_sync_test_scene_add(
      bmain, "DirectionalScene", &sculpt_brush, &image_brush);
  ToolSettings *ts = scene->toolsettings;

  ts->sculpt->paint.visible_material_channels = (1 << PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ts->imapaint.paint.visible_material_channels = (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* The one-shot copy must work while automatic sync is off, and must leave it off. */
  ASSERT_EQ(ts->paint_mode.material_paint_flag & PAINT_MATERIAL_BRUSH_SYNC, 0);
  EXPECT_TRUE(BKE_paint_material_brush_sync_directional(
      scene, &ts->sculpt->paint, &ts->imapaint.paint));

  EXPECT_EQ(ts->imapaint.paint.visible_material_channels,
            (1 << PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_EQ(BKE_paint_brush(&ts->imapaint.paint), sculpt_brush);
  EXPECT_EQ(ts->paint_mode.material_paint_flag & PAINT_MATERIAL_BRUSH_SYNC, 0);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, DirectionalBrushSyncKeepsSourceBrushLiveChannels)
{
  Brush *sculpt_brush = nullptr;
  Brush *image_brush = nullptr;
  Scene *scene = paint_material_sync_test_scene_add(
      bmain, "LiveChannelScene", &sculpt_brush, &image_brush);
  ToolSettings *ts = scene->toolsettings;
  ASSERT_NE(sculpt_brush, image_brush);

  /* A preset from an earlier brush switch, with the channel still unconfigured. */
  PaintMaterialBrushPreset *stale = BKE_paint_material_brush_preset_ensure(*scene, *sculpt_brush);
  stale->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 0;
  stale->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].source_mtex.tex = nullptr;

  /* What the user set up in Sculpt Mode since then. */
  Tex *roughness_tex = BKE_texture_add(bmain, "SyncRoughnessTex");
  sculpt_brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 1;
  sculpt_brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].source_mtex.tex =
      roughness_tex;

  ASSERT_TRUE(BKE_paint_material_brush_sync_directional(
      scene, &ts->sculpt->paint, &ts->imapaint.paint));

  /* Syncing must not roll the source brush back to its stored preset. */
  EXPECT_TRUE(sculpt_brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use);
  EXPECT_EQ(sculpt_brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].source_mtex.tex,
            roughness_tex);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, DirectionalBrushSyncRejectsUnpairedPaint)
{
  Brush *sculpt_brush = nullptr;
  Brush *image_brush = nullptr;
  Scene *scene = paint_material_sync_test_scene_add(
      bmain, "UnpairedScene", &sculpt_brush, &image_brush);
  ToolSettings *ts = scene->toolsettings;
  ASSERT_NE(sculpt_brush, image_brush);

  Paint foreign_paint{};
  EXPECT_FALSE(
      BKE_paint_material_brush_sync_directional(scene, &ts->sculpt->paint, &foreign_paint));

  /* The vertex color canvas is painted by Sculpt Mode alone, so there is nothing to mirror. */
  ts->paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL_PAINT;
  EXPECT_FALSE(BKE_paint_material_brush_sync_directional(
      scene, &ts->sculpt->paint, &ts->imapaint.paint));

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BrushSyncDisableGivesImagePaintItsOwnBrush)
{
  Brush *sculpt_brush = nullptr;
  Brush *image_brush = nullptr;
  Scene *scene = paint_material_sync_test_scene_add(
      bmain, "DisableScene", &sculpt_brush, &image_brush);
  ToolSettings *ts = scene->toolsettings;

  ASSERT_TRUE(BKE_paint_material_brush_sync_directional(
      scene, &ts->sculpt->paint, &ts->imapaint.paint));
  ASSERT_EQ(BKE_paint_brush(&ts->imapaint.paint), sculpt_brush);

  EXPECT_TRUE(BKE_paint_material_brush_sync_disable(bmain, scene));
  Brush *own_brush = BKE_paint_brush(&ts->imapaint.paint);
  EXPECT_NE(own_brush, sculpt_brush);
  EXPECT_NE(own_brush, nullptr);
  /* Splitting an already independent pair must not keep creating brushes. */
  EXPECT_FALSE(BKE_paint_material_brush_sync_disable(bmain, scene));
  EXPECT_EQ(BKE_paint_brush(&ts->imapaint.paint), own_brush);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, SnapshotCapturesBrushOntoPreset)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "SnapshotScene"));
  Brush *brush = BKE_brush_add(bmain, "SnapshotSource", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 1;
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].value[0] = 0.75f;

  BKE_paint_material_brush_preset_snapshot(*scene, *brush);

  PaintMaterialBrushPreset *preset = BKE_paint_material_brush_preset_find(*scene, *brush);
  ASSERT_NE(preset, nullptr);
  ASSERT_NE(preset->material_paint, nullptr);
  EXPECT_TRUE(preset->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use);
  EXPECT_FLOAT_EQ(preset->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].value[0],
                  0.75f);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, SnapshotIsNoOpWhenBrushHasNoMaterialPaint)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "NoOpSnapshotScene"));
  Brush *brush = BKE_brush_add(bmain, "NeverUsed", OB_MODE_SCULPT);
  ASSERT_EQ(brush->material_paint, nullptr);

  BKE_paint_material_brush_preset_snapshot(*scene, *brush);

  EXPECT_EQ(BKE_paint_material_brush_preset_find(*scene, *brush), nullptr)
      << "snapshot must not create a preset from nothing";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, ApplyThenSnapshotRoundTripsAndDoesNotLeakTexUsers)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "RoundTripScene"));
  Brush *brush = BKE_brush_add(bmain, "RoundTrip", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  Image *image = add_image("RoundTripTex");
  Tex *tex = BKE_texture_add(bmain, "RoundTripTexWrapper");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex = tex;
  id_us_plus(&tex->id);
  const int tex_users_baseline = tex->id.us;

  BKE_paint_material_brush_preset_snapshot(*scene, *brush);
  BKE_paint_material_brush_preset_apply(*scene, *brush);
  BKE_paint_material_brush_preset_snapshot(*scene, *brush);
  BKE_paint_material_brush_preset_apply(*scene, *brush);

  EXPECT_EQ(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex,
            tex);
  EXPECT_EQ(tex->id.us, tex_users_baseline)
      << "repeated apply/snapshot must not drift the Tex user count";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BrushesEnsureDoesNotOverwriteLiveChannelEdits)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "EnsurePreservesScene"));
  Brush *brush = BKE_brush_add(bmain, "PreExisting", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_AO].use = 1;
  BKE_paint_material_brush_preset_snapshot(*scene, *brush);

  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_AO].use = 0;

  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  Paint *paint = &sculpt_data->paint;
  paint->runtime = MEM_new<bke::PaintRuntime>(__func__);
  paint->runtime->ob_mode = OB_MODE_SCULPT;
  paint->brush = brush;

  BKE_paint_brushes_ensure(bmain, scene, paint);

  EXPECT_FALSE(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_AO].use)
      << "ensure() must not re-apply a stale preset over live channel edits";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BrushesEnsureAppliesPresetWhenBrushHasNoMaterialPaint)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "EnsureAppliesNullScene"));
  Brush *brush = BKE_brush_add(bmain, "NeedsPreset", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_AO].use = 1;
  BKE_paint_material_brush_preset_snapshot(*scene, *brush);

  BKE_brush_material_paint_free(brush->material_paint);
  brush->material_paint = nullptr;

  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  Paint *paint = &sculpt_data->paint;
  paint->runtime = MEM_new<bke::PaintRuntime>(__func__);
  paint->runtime->ob_mode = OB_MODE_SCULPT;
  paint->brush = brush;

  BKE_paint_brushes_ensure(bmain, scene, paint);

  ASSERT_NE(brush->material_paint, nullptr);
  EXPECT_TRUE(brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_AO].use)
      << "ensure() must still apply the scene preset when the brush has no live PBR state";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BrushSetSyncedSnapshotsOutgoingAndAppliesIncoming)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "SyncedSetScene"));
  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  Paint &paint = sculpt_data->paint;
  paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  paint.runtime->ob_mode = OB_MODE_SCULPT;

  Brush *brush_a = BKE_brush_add(bmain, "SyncedA", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush_a);
  brush_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use = 1;
  paint.brush = brush_a;

  Brush *brush_b = BKE_brush_add(bmain, "SyncedB", OB_MODE_SCULPT);
  PaintMaterialBrushPreset *preset_b = BKE_paint_material_brush_preset_ensure(*scene, *brush_b);
  preset_b->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = 1;

  ASSERT_TRUE(BKE_paint_brush_set_synced(*scene, paint, brush_b));

  EXPECT_EQ(paint.brush, brush_b);
  ASSERT_NE(brush_b->material_paint, nullptr);
  EXPECT_TRUE(brush_b->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use)
      << "incoming brush must have the matching preset applied";

  PaintMaterialBrushPreset *preset_a = BKE_paint_material_brush_preset_find(*scene, *brush_a);
  ASSERT_NE(preset_a, nullptr);
  EXPECT_TRUE(preset_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_METALLIC].use)
      << "outgoing brush's state must have been snapshotted before the switch";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BrushSetSyncedSamePointerIsNoOpForSync)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "SamePointerScene"));
  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  Paint &paint = sculpt_data->paint;
  paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  paint.runtime->ob_mode = OB_MODE_SCULPT;

  Brush *brush = BKE_brush_add(bmain, "SameBrush", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  paint.brush = brush;

  ASSERT_TRUE(BKE_paint_brush_set_synced(*scene, paint, brush));

  EXPECT_LE(scene->toolsettings->paint_mode.material_paint_brush_presets.count(), 1);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, SnapshotAllCapturesEveryScenesActiveBrush)
{
  Scene *scene_a = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "SnapAllSceneA"));
  Sculpt *sculpt_a = MEM_new<Sculpt>(__func__);
  scene_a->toolsettings->sculpt = sculpt_a;
  sculpt_a->paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  Brush *brush_a = BKE_brush_add(bmain, "SnapAllBrushA", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush_a);
  brush_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = 1;
  sculpt_a->paint.brush = brush_a;

  Scene *scene_b = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "SnapAllSceneB"));
  scene_b->toolsettings->imapaint.paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  Brush *brush_b = BKE_brush_add(bmain, "SnapAllBrushB", OB_MODE_TEXTURE_PAINT);
  BKE_brush_material_paint_ensure(brush_b);
  brush_b->material_paint->channels[PAINT_MATERIAL_CHANNEL_EMISSION].use = 1;
  scene_b->toolsettings->imapaint.paint.brush = brush_b;

  BKE_paint_material_brush_presets_prepare_for_save(bmain);

  PaintMaterialBrushPreset *preset_a = BKE_paint_material_brush_preset_find(*scene_a, *brush_a);
  ASSERT_NE(preset_a, nullptr);
  EXPECT_TRUE(preset_a->material_paint->channels[PAINT_MATERIAL_CHANNEL_ALPHA].use);

  PaintMaterialBrushPreset *preset_b = BKE_paint_material_brush_preset_find(*scene_b, *brush_b);
  ASSERT_NE(preset_b, nullptr);
  EXPECT_TRUE(preset_b->material_paint->channels[PAINT_MATERIAL_CHANNEL_EMISSION].use);

  EXPECT_EQ(scene_a->toolsettings->paint_mode.material_paint_brush_presets.count(), 1);
  EXPECT_EQ(scene_b->toolsettings->paint_mode.material_paint_brush_presets.count(), 1);

  BKE_id_free(bmain, scene_a);
  BKE_id_free(bmain, scene_b);
}

TEST_F(PaintMaterialChannelTest, PresetRemoveDropsMatchingPresetOnly)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "RemoveScene"));
  Brush *brush_a = BKE_brush_add(bmain, "RemoveA", OB_MODE_SCULPT);
  Brush *brush_b = BKE_brush_add(bmain, "RemoveB", OB_MODE_SCULPT);
  BKE_paint_material_brush_preset_ensure(*scene, *brush_a);
  BKE_paint_material_brush_preset_ensure(*scene, *brush_b);

  BKE_paint_material_brush_preset_remove(*scene, *brush_a);

  EXPECT_EQ(BKE_paint_material_brush_preset_find(*scene, *brush_a), nullptr);
  EXPECT_NE(BKE_paint_material_brush_preset_find(*scene, *brush_b), nullptr);
  EXPECT_EQ(scene->toolsettings->paint_mode.material_paint_brush_presets.count(), 1);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, PresetRemoveReleasesTexUserCount)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "RemoveTexScene"));
  Brush *brush = BKE_brush_add(bmain, "RemoveTexBrush", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);

  Image *image = add_image("RemoveTexImage");
  Tex *tex = BKE_texture_add(bmain, "RemoveTexWrapper");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].source_mtex.tex = tex;
  id_us_plus(&tex->id);
  const int us_before_snapshot = tex->id.us;

  BKE_paint_material_brush_preset_snapshot(*scene, *brush);
  EXPECT_EQ(tex->id.us, us_before_snapshot + 1)
      << "snapshot copy_into must take a counted reference on the preset";

  BKE_paint_material_brush_preset_remove(*scene, *brush);
  EXPECT_EQ(tex->id.us, us_before_snapshot)
      << "remove must drop the preset's Tex user count; foreach_id will not run";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, PrepareForSavePurgesPresetsOfDeletedLocalBrushes)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "PurgeScene"));
  Brush *brush = BKE_brush_add(bmain, "PurgeSurvivor", OB_MODE_SCULPT);
  BKE_paint_material_brush_preset_ensure(*scene, *brush);

  /* A preset whose local brush no longer exists, as left behind by deleting a brush, plus a
   * malformed node with no key at all - both must be dropped. */
  PaintMaterialBrushPreset *orphan = MEM_new<PaintMaterialBrushPreset>(__func__);
  orphan->local_name = BLI_strdup("GoneBrush");
  orphan->material_paint = BKE_brush_material_paint_create_default();
  BLI_addtail(&scene->toolsettings->paint_mode.material_paint_brush_presets, orphan);
  PaintMaterialBrushPreset *malformed = MEM_new<PaintMaterialBrushPreset>(__func__);
  malformed->material_paint = BKE_brush_material_paint_create_default();
  BLI_addtail(&scene->toolsettings->paint_mode.material_paint_brush_presets, malformed);

  BKE_paint_material_brush_presets_prepare_for_save(bmain);

  EXPECT_EQ(scene->toolsettings->paint_mode.material_paint_brush_presets.count(), 1);
  EXPECT_NE(BKE_paint_material_brush_preset_find(*scene, *brush), nullptr);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BaseColorSyncWritesImagePaintUnifiedColor)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "BaseColorSyncScene"));
  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  sculpt_data->paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  sculpt_data->paint.runtime->ob_mode = OB_MODE_SCULPT;
  scene->toolsettings->imapaint.paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  scene->toolsettings->imapaint.paint.runtime->ob_mode = OB_MODE_TEXTURE_PAINT;

  scene->toolsettings->paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  scene->toolsettings->paint_mode.material_paint_flag = PAINT_MATERIAL_BRUSH_SYNC;
  sculpt_data->paint.unified_paint_settings.flag |= UNIFIED_PAINT_COLOR;
  scene->toolsettings->imapaint.paint.unified_paint_settings.flag |= UNIFIED_PAINT_COLOR;

  Brush *brush = BKE_brush_add(bmain, "BaseColorSyncBrush", OB_MODE_SCULPT | OB_MODE_TEXTURE_PAINT);
  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->use_sync_base_color_with_brush = 1;
  sculpt_data->paint.brush = brush;
  scene->toolsettings->imapaint.paint.brush = brush;

  copy_v3_v3(sculpt_data->paint.unified_paint_settings.color, float3(1.0f, 0.0f, 0.0f));
  copy_v3_v3(scene->toolsettings->imapaint.paint.unified_paint_settings.color,
             float3(0.0f, 1.0f, 0.0f));
  copy_v3_v3(brush->material_paint->base_color, float3(0.2f, 0.4f, 0.8f));

  BKE_brush_material_paint_base_color_sync_to_brush(&sculpt_data->paint, brush, scene);

  EXPECT_TRUE(equals_v3v3(sculpt_data->paint.unified_paint_settings.color,
                          brush->material_paint->base_color));
  EXPECT_TRUE(equals_v3v3(scene->toolsettings->imapaint.paint.unified_paint_settings.color,
                          brush->material_paint->base_color))
      << "Image Editor Color uses Image Paint unified settings, not only the PBR Base Color field";

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BrushSyncAfterLoadSharesSculptBrushWithImagePaint)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "LoadSyncScene"));
  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  sculpt_data->paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  sculpt_data->paint.runtime->ob_mode = OB_MODE_SCULPT;

  scene->toolsettings->imapaint.paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  scene->toolsettings->imapaint.paint.runtime->ob_mode = OB_MODE_TEXTURE_PAINT;

  scene->toolsettings->paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  scene->toolsettings->paint_mode.material_paint_flag = PAINT_MATERIAL_BRUSH_SYNC;

  Brush *sculpt_brush = BKE_brush_add(bmain, "LoadSyncSculpt", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(sculpt_brush);
  sculpt_brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = 1;
  sculpt_data->paint.brush = sculpt_brush;

  Brush *image_brush = BKE_brush_add(bmain, "LoadSyncImage", OB_MODE_TEXTURE_PAINT);
  BKE_brush_material_paint_ensure(image_brush);
  image_brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = 0;
  scene->toolsettings->imapaint.paint.brush = image_brush;

  BKE_paint_material_brush_sync_after_load(bmain);

  EXPECT_EQ(scene->toolsettings->imapaint.paint.brush, sculpt_brush)
      << "after load, Image Paint must share the Sculpt brush so channel Use flags are the same "
         "datablock";
  EXPECT_TRUE(sculpt_brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_NORMAL].use);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, BrushSyncAfterLoadIsNoOpWhenSyncDisabled)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "LoadSyncOffScene"));
  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  sculpt_data->paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  sculpt_data->paint.runtime->ob_mode = OB_MODE_SCULPT;
  scene->toolsettings->imapaint.paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  scene->toolsettings->imapaint.paint.runtime->ob_mode = OB_MODE_TEXTURE_PAINT;

  scene->toolsettings->paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  scene->toolsettings->paint_mode.material_paint_flag = ePaintMaterialFlag(0);

  Brush *sculpt_brush = BKE_brush_add(bmain, "LoadSyncOffSculpt", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(sculpt_brush);
  sculpt_data->paint.brush = sculpt_brush;

  Brush *image_brush = BKE_brush_add(bmain, "LoadSyncOffImage", OB_MODE_TEXTURE_PAINT);
  BKE_brush_material_paint_ensure(image_brush);
  scene->toolsettings->imapaint.paint.brush = image_brush;

  BKE_paint_material_brush_sync_after_load(bmain);

  EXPECT_EQ(scene->toolsettings->imapaint.paint.brush, image_brush);

  BKE_id_free(bmain, scene);
}

TEST_F(PaintMaterialChannelTest, LocalBrushChannelDataSurvivesSaveReload)
{
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "PersistScene"));
  Sculpt *sculpt_data = MEM_new<Sculpt>(__func__);
  scene->toolsettings->sculpt = sculpt_data;
  sculpt_data->paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  sculpt_data->paint.runtime->ob_mode = OB_MODE_SCULPT;

  Brush *brush = BKE_brush_add(bmain, "PersistBrush", OB_MODE_SCULPT);
  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_HEIGHT].use = 1;
  brush->material_paint->channels[PAINT_MATERIAL_CHANNEL_HEIGHT].value[0] = 0.42f;
  sculpt_data->paint.brush = brush;

  BKE_paint_material_brush_presets_prepare_for_save(bmain);

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "persist_test.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Scene *reloaded_scene = nullptr;
  for (Scene &sce : bfd->main->scenes) {
    if (STREQ(sce.id.name + 2, "PersistScene")) {
      reloaded_scene = &sce;
      break;
    }
  }
  ASSERT_NE(reloaded_scene, nullptr);

  ASSERT_FALSE(reloaded_scene->toolsettings->paint_mode.material_paint_brush_presets.is_empty());
  PaintMaterialBrushPreset &reloaded_preset =
      *reloaded_scene->toolsettings->paint_mode.material_paint_brush_presets.begin();
  ASSERT_NE(reloaded_preset.material_paint, nullptr);
  EXPECT_TRUE(reloaded_preset.material_paint->channels[PAINT_MATERIAL_CHANNEL_HEIGHT].use);
  EXPECT_FLOAT_EQ(reloaded_preset.material_paint->channels[PAINT_MATERIAL_CHANNEL_HEIGHT].value[0],
                  0.42f);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);

  BKE_id_free(bmain, scene);
}

}  // namespace blender
