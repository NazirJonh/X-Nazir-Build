/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"

#include "BLI_listbase.h"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

namespace blender::bke::tests {

class PaintMaterialMaskEditTest : public bke::BlenderGTestBase {
 protected:
  Main *bmain = nullptr;
  Scene *scene = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    scene = BKE_scene_add(bmain, "MaskEditScene");
    Paint *paint = &scene->toolsettings->imapaint.paint;
    BKE_paint_ensure(scene->toolsettings, &paint);
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
};

TEST_F(PaintMaterialMaskEditTest, deleting_the_mask_image_nulls_the_binding)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "Mask", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  scene->toolsettings->paint_mode.mask_image_binding.image = mask_image;

  BKE_id_delete(bmain, mask_image);
  EXPECT_EQ(scene->toolsettings->paint_mode.mask_image_binding.image, nullptr);
}

TEST_F(PaintMaterialMaskEditTest, deleting_the_mask_brush_nulls_the_pointer)
{
  Brush *brush = BKE_id_new<Brush>(bmain, "MaskBrush");
  scene->toolsettings->paint_mode.mask_active_brush = brush;

  BKE_id_delete(bmain, brush);
  EXPECT_EQ(scene->toolsettings->paint_mode.mask_active_brush, nullptr);
}

TEST_F(PaintMaterialMaskEditTest, targets_get_returns_one_mask_target_in_mask_mode)
{
  Object *ob = add_mesh_object("MaskTargetOb");
  Material *ma = add_material_with_principled(*ob, "MaskTargetMat");
  (void)ma;
  PaintModeSettings mode_settings{};
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "Mask", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  mode_settings.mask_image_binding.image = mask_image;

  const Vector<PaintMaterialImageTarget> targets = BKE_paint_material_image_targets_get(
      *ob, mode_settings, nullptr, PAINT_MATERIAL_CHANNELS_VISIBLE_ALL, 0.42f);
  ASSERT_EQ(targets.size(), 1);
  EXPECT_EQ(targets[0].image, mask_image);
  EXPECT_TRUE(targets[0].is_mask_target);
  EXPECT_FALSE(targets[0].is_color_channel);
  EXPECT_FALSE(targets[0].is_normal_channel);
  EXPECT_FLOAT_EQ(targets[0].value, 0.42f);
}

TEST_F(PaintMaterialMaskEditTest, targets_get_ignores_brush_material_paint_in_mask_mode)
{
  Object *ob = add_mesh_object("MaskIgnoreOb");
  Material *ma = add_material_with_principled(*ob, "MaskIgnoreMat");
  (void)ma;
  PaintModeSettings mode_settings{};
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "Mask2", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  mode_settings.mask_image_binding.image = mask_image;

  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].value[0] = 0.9f;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = 1;

  const Vector<PaintMaterialImageTarget> targets = BKE_paint_material_image_targets_get(
      *ob, mode_settings, &brush_paint, PAINT_MATERIAL_CHANNELS_VISIBLE_ALL, 0.5f);
  ASSERT_EQ(targets.size(), 1);
  EXPECT_FLOAT_EQ(targets[0].value, 0.5f);
}

TEST_F(PaintMaterialMaskEditTest, mask_edit_begin_snapshots_and_switches_brush)
{
  Brush *original = BKE_brush_add(bmain, "Original", OB_MODE_TEXTURE_PAINT);
  Brush *mask_brush = BKE_brush_add(bmain, "MaskBrush", OB_MODE_TEXTURE_PAINT);
  Paint &paint = scene->toolsettings->imapaint.paint;
  PaintModeSettings &mode = scene->toolsettings->paint_mode;
  paint.brush = original;
  mode.mask_active_brush = mask_brush;

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "MaskA", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  BKE_paint_material_mask_edit_begin_ex(*bmain, *scene, paint, mode, *mask_image);

  EXPECT_EQ(mode.mask_saved_brush, original);
  EXPECT_EQ(BKE_paint_brush(&paint), mask_brush);
  EXPECT_EQ(mode.mask_image_binding.image, mask_image);
}

TEST_F(PaintMaterialMaskEditTest, mask_edit_begin_switching_masks_does_not_resnapshot)
{
  Brush *original = BKE_brush_add(bmain, "Original2", OB_MODE_TEXTURE_PAINT);
  Brush *mask_brush = BKE_brush_add(bmain, "MaskBrush2", OB_MODE_TEXTURE_PAINT);
  Paint &paint = scene->toolsettings->imapaint.paint;
  PaintModeSettings &mode = scene->toolsettings->paint_mode;
  paint.brush = original;
  mode.mask_active_brush = mask_brush;

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_a = BKE_image_add_generated(
      bmain, 8, 8, "MaskA2", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  Image *mask_b = BKE_image_add_generated(
      bmain, 8, 8, "MaskB2", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  BKE_paint_material_mask_edit_begin_ex(*bmain, *scene, paint, mode, *mask_a);

  /* User picks a different brush while masking -- begin() for mask B must not disturb it. */
  Brush *swapped = BKE_brush_add(bmain, "SwappedWhileMasking", OB_MODE_TEXTURE_PAINT);
  paint.brush = swapped;

  BKE_paint_material_mask_edit_begin_ex(*bmain, *scene, paint, mode, *mask_b);
  EXPECT_EQ(mode.mask_saved_brush, original);
  EXPECT_EQ(BKE_paint_brush(&paint), swapped);
  EXPECT_EQ(mode.mask_image_binding.image, mask_b);
}

TEST_F(PaintMaterialMaskEditTest, mask_edit_end_restores_the_original_brush)
{
  Brush *original = BKE_brush_add(bmain, "Original3", OB_MODE_TEXTURE_PAINT);
  Brush *mask_brush = BKE_brush_add(bmain, "MaskBrush3", OB_MODE_TEXTURE_PAINT);
  Paint &paint = scene->toolsettings->imapaint.paint;
  PaintModeSettings &mode = scene->toolsettings->paint_mode;
  paint.brush = original;
  mode.mask_active_brush = mask_brush;
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "MaskC", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  BKE_paint_material_mask_edit_begin_ex(*bmain, *scene, paint, mode, *mask_image);

  BKE_paint_material_mask_edit_end_ex(*bmain, *scene, paint, mode);
  EXPECT_EQ(BKE_paint_brush(&paint), original);
  EXPECT_EQ(mode.mask_image_binding.image, nullptr);
  EXPECT_EQ(mode.mask_saved_brush, nullptr);
  EXPECT_EQ(mode.mask_active_brush, mask_brush);
}

TEST_F(PaintMaterialMaskEditTest, mask_edit_end_without_begin_is_a_noop)
{
  Paint &paint = scene->toolsettings->imapaint.paint;
  PaintModeSettings &mode = scene->toolsettings->paint_mode;
  BKE_paint_material_mask_edit_end_ex(*bmain, *scene, paint, mode);
  EXPECT_EQ(mode.mask_image_binding.image, nullptr);
}

TEST_F(PaintMaterialMaskEditTest, mask_edit_end_falls_back_to_default_brush_when_saved_is_gone)
{
  Brush *original = BKE_brush_add(bmain, "Original4", OB_MODE_TEXTURE_PAINT);
  Brush *mask_brush = BKE_brush_add(bmain, "MaskBrush4", OB_MODE_TEXTURE_PAINT);
  Paint &paint = scene->toolsettings->imapaint.paint;
  PaintModeSettings &mode = scene->toolsettings->paint_mode;
  paint.brush = original;
  mode.mask_active_brush = mask_brush;
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "MaskD", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  BKE_paint_material_mask_edit_begin_ex(*bmain, *scene, paint, mode, *mask_image);

  /* Simulate the saved brush having been deleted meanwhile (remap already nulled it). */
  mode.mask_saved_brush = nullptr;

  BKE_paint_material_mask_edit_end_ex(*bmain, *scene, paint, mode);
  EXPECT_NE(BKE_paint_brush(&paint), nullptr);
}

TEST_F(PaintMaterialMaskEditTest, ensure_writable_does_nothing_in_mask_mode)
{
  Object *ob = add_mesh_object("EnsureMaskOb");
  Material *ma = add_material_with_principled(*ob, "EnsureMaskMat");
  PaintModeSettings mode_settings{};
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "MaskE", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  mode_settings.mask_image_binding.image = mask_image;
  BrushMaterialPaint brush_paint{};
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = 1;

  const int images_before = BLI_listbase_count(&bmain->images);
  const int nodes_before = BLI_listbase_count(&ma->nodetree->nodes);
  BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, PAINT_MATERIAL_CHANNELS_VISIBLE_ALL);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
  EXPECT_EQ(BLI_listbase_count(&ma->nodetree->nodes), nodes_before);
}

}  // namespace blender::bke::tests
