/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

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

namespace blender {

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

TEST_F(PaintMaterialChannelTest, channel_helpers)
{
  PaintModeSettings settings{};
  settings.use_channel[PAINT_MATERIAL_CHANNEL_METALLIC] = 1;
  settings.use_channel[PAINT_MATERIAL_CHANNEL_ROUGHNESS] = 0;
  settings.use_channel[PAINT_MATERIAL_CHANNEL_SPECULAR] = 1;
  settings.use_channel[PAINT_MATERIAL_CHANNEL_CUSTOM] = 1;
  settings.channel_value[PAINT_MATERIAL_CHANNEL_METALLIC] = 0.25f;
  settings.channel_value[PAINT_MATERIAL_CHANNEL_ROUGHNESS] = 0.5f;
  settings.channel_value[PAINT_MATERIAL_CHANNEL_SPECULAR] = 0.75f;
  settings.channel_value[PAINT_MATERIAL_CHANNEL_CUSTOM] = 0.1f;
  settings.channel_custom_range[0] = 0.0f;
  settings.channel_custom_range[1] = 1.0f;
  BLI_strncpy(settings.material_paint_custom_attr,
              "my_attr",
              sizeof(settings.material_paint_custom_attr));

  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(settings, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(settings, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(settings, PAINT_MATERIAL_CHANNEL_SPECULAR));
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(settings, PAINT_MATERIAL_CHANNEL_CUSTOM));

  EXPECT_FLOAT_EQ(BKE_paint_material_channel_value(settings, PAINT_MATERIAL_CHANNEL_METALLIC),
                  0.25f);
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_value(settings, PAINT_MATERIAL_CHANNEL_SPECULAR),
                  0.75f);
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_value(settings, PAINT_MATERIAL_CHANNEL_CUSTOM), 0.1f);

  EXPECT_EQ(BKE_paint_material_channel_attribute_name(settings, PAINT_MATERIAL_CHANNEL_METALLIC),
            "material_metallic");
  EXPECT_EQ(BKE_paint_material_channel_attribute_name(settings, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            "material_roughness");
  EXPECT_EQ(BKE_paint_material_channel_attribute_name(settings, PAINT_MATERIAL_CHANNEL_SPECULAR),
            "material_specular");
  EXPECT_EQ(BKE_paint_material_channel_attribute_name(settings, PAINT_MATERIAL_CHANNEL_CUSTOM),
            "my_attr");

  settings.material_paint_custom_attr[0] = '\0';
  EXPECT_FALSE(BKE_paint_material_channel_is_enabled(settings, PAINT_MATERIAL_CHANNEL_CUSTOM));
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

  /* Exactly one color channel, and only the vertex-only channel lacks a socket. */
  EXPECT_TRUE(BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_BASE_COLOR).is_color);
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
  PaintModeSettings settings{};
  settings.use_channel[PAINT_MATERIAL_CHANNEL_CUSTOM] = 1;
  BLI_strncpy(settings.material_paint_custom_attr,
              "my_attr",
              sizeof(settings.material_paint_custom_attr));

  settings.channel_custom_range[0] = -2.0f;
  settings.channel_custom_range[1] = 3.0f;
  settings.channel_value[PAINT_MATERIAL_CHANNEL_CUSTOM] = 5.0f;
  EXPECT_FLOAT_EQ(BKE_paint_material_channel_value(settings, PAINT_MATERIAL_CHANNEL_CUSTOM), 3.0f);

  const float2 range = BKE_paint_material_channel_range(settings, PAINT_MATERIAL_CHANNEL_CUSTOM);
  EXPECT_FLOAT_EQ(range.x, -2.0f);
  EXPECT_FLOAT_EQ(range.y, 3.0f);

  /* An inverted range set through the API must not produce an empty clamp interval. */
  settings.channel_custom_range[0] = 4.0f;
  settings.channel_custom_range[1] = 1.0f;
  const float2 fixed = BKE_paint_material_channel_range(settings, PAINT_MATERIAL_CHANNEL_CUSTOM);
  EXPECT_FLOAT_EQ(fixed.x, 1.0f);
  EXPECT_FLOAT_EQ(fixed.y, 4.0f);

  /* The fixed channels ignore the custom range. */
  const float2 metallic = BKE_paint_material_channel_range(settings,
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

TEST_F(PaintMaterialChannelTest, BaseColorEnabledAndName)
{
  PaintModeSettings settings;
  settings.use_channel[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = 1;
  EXPECT_TRUE(BKE_paint_material_channel_is_enabled(settings, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(BKE_paint_material_channel_attribute_name(settings, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            "Color");
}

TEST_F(PaintMaterialChannelTest, BaseColorGetRespectsInvert)
{
  PaintModeSettings settings;
  copy_v3_fl3(settings.channel_base_color, 0.2f, 0.4f, 0.6f);

  Paint paint{};
  paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  paint.runtime->ob_mode = OB_MODE_SCULPT;

  Brush brush{};
  copy_v3_fl3(brush.secondary_color, 0.1f, 0.3f, 0.5f);

  const float3 non_invert = BKE_paint_material_base_color_get(settings, paint, brush, false);
  EXPECT_FLOAT_EQ(non_invert.x, 0.2f);
  EXPECT_FLOAT_EQ(non_invert.y, 0.4f);
  EXPECT_FLOAT_EQ(non_invert.z, 0.6f);

  const float3 inverted = BKE_paint_material_base_color_get(settings, paint, brush, true);
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
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure(mesh, &created),
            MaterialPaintAttributeStatus::Ok);
  EXPECT_TRUE(created);
  created = false;
  EXPECT_EQ(BKE_paint_mesh_material_color_attribute_ensure(mesh, &created),
            MaterialPaintAttributeStatus::Ok);
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

}  // namespace blender
