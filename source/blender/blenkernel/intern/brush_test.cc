/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "BKE_brush.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

#include "BLI_listbase.h"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

namespace blender {

class BrushTest : public bke::BlenderGTestBase {
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
};

static void check_id_and_name(const ID *a, const ID *b)
{
  EXPECT_NE(a, b) << "ID " << a->name << " and " << b->name << "should be different pointers";
  EXPECT_EQ(a->us, 1) << "ID " << a->name << " should have 1 user";
  EXPECT_EQ(b->us, 1) << "ID " << b->name << " should have 1 user";
  EXPECT_STRNE(a->name, b->name);
}

static void check_embedded_copy(const ID *a, const ID *b)
{
  EXPECT_NE(a, b) << "ID " << a->name << " and " << b->name << "should be different pointers";
  EXPECT_EQ(a->us, 0) << "ID " << a->name << " should have 0 users";
  EXPECT_EQ(b->us, 0) << "ID " << b->name << " should have 0 users";
  EXPECT_TRUE(a->flag & ID_FLAG_EMBEDDED_DATA);
  EXPECT_TRUE(b->flag & ID_FLAG_EMBEDDED_DATA);
}

TEST_F(BrushTest, deep_copy)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  /* TODO: Ideally this shouldn't be needed, but BKE_brush_add generates an extra user. Remove this
   * once that has been fixed. */
  id_us_min(&brush->id);

  /* Normal Linked Data */
  brush->paint_curve = static_cast<PaintCurve *>(BKE_id_new(bmain, ID_PC, "UnitTestPaintCurve"));
  brush->mtex.tex = static_cast<Tex *>(BKE_id_new(bmain, ID_TE, "UnitTestTexture"));
  brush->mtex.tex->ima = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "UnitTestImage"));

  /* Embedded Data */
  brush->mtex.tex->nodetree = BKE_id_new_nomain<bNodeTree>("UnitTestNodeTree");
  brush->mtex.tex->nodetree->id.flag |= ID_FLAG_EMBEDDED_DATA;

  Brush *duplicated_brush = BKE_brush_duplicate(
      bmain, brush, USER_DUP_OBDATA | USER_DUP_LINKED_ID, LIB_ID_DUPLICATE_IS_ROOT_ID);

  check_id_and_name(&brush->id, &duplicated_brush->id);
  check_id_and_name(&brush->paint_curve->id, &duplicated_brush->paint_curve->id);
  check_id_and_name(&brush->mtex.tex->id, &duplicated_brush->mtex.tex->id);
  check_id_and_name(&brush->mtex.tex->ima->id, &duplicated_brush->mtex.tex->ima->id);

  check_embedded_copy(&brush->mtex.tex->nodetree->id, &duplicated_brush->mtex.tex->nodetree->id);

  EXPECT_TRUE(bmain->nodetrees.is_empty());
}

TEST_F(BrushTest, deep_copy_grease_pencil_brush)
{
  /* Grease pencil brushes potentially have more ID linked to them, hence a separate test */

  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_PAINT_GREASE_PENCIL);
  /* TODO: Ideally this shouldn't be needed, but #BKE_brush_add generates an extra user.
   * Remove this once that has been fixed. */
  id_us_min(&brush->id);

  /* Normal Linked Data */
  brush->paint_curve = static_cast<PaintCurve *>(BKE_id_new(bmain, ID_PC, "UnitTestPaintCurve"));
  brush->gpencil_settings->material = static_cast<Material *>(
      BKE_id_new(bmain, ID_MA, "UnitTestMaterial"));
  brush->gpencil_settings->material_alt = static_cast<Material *>(
      BKE_id_new(bmain, ID_MA, "UnitTestMaterialAlt"));

  Brush *duplicated_brush = BKE_brush_duplicate(
      bmain, brush, USER_DUP_OBDATA | USER_DUP_LINKED_ID, LIB_ID_DUPLICATE_IS_ROOT_ID);

  check_id_and_name(&brush->id, &duplicated_brush->id);
  check_id_and_name(&brush->paint_curve->id, &duplicated_brush->paint_curve->id);
  check_id_and_name(&brush->gpencil_settings->material->id,
                    &duplicated_brush->gpencil_settings->material->id);
  check_id_and_name(&brush->gpencil_settings->material_alt->id,
                    &duplicated_brush->gpencil_settings->material_alt->id);

  check_embedded_copy(&brush->gpencil_settings->material->nodetree->id,
                      &duplicated_brush->gpencil_settings->material->nodetree->id);
  check_embedded_copy(&brush->gpencil_settings->material_alt->nodetree->id,
                      &duplicated_brush->gpencil_settings->material_alt->nodetree->id);

  EXPECT_TRUE(bmain->nodetrees.is_empty());
}

/**
 * Production change that fails this test: decrementing `texture_active_index` for every removal,
 * including a slot after the active one (the bug in `BKE_brush_curve_patch_texture_slot_remove`).
 */
TEST_F(BrushTest, curve_patch_texture_slot_remove_keeps_active_when_later_slot_removed)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  id_us_min(&brush->id);

  BKE_brush_curve_patch_texture_slot_add(*brush);
  BKE_brush_curve_patch_texture_slot_add(*brush);
  BKE_brush_curve_patch_texture_slot_add(*brush);
  BKE_brush_curve_patch_texture_slot_add(*brush);
  BrushCurvePatchTextureSlot *later = BKE_brush_curve_patch_texture_slot_add(*brush);

  brush->curve_patch.texture_active_index = 2;
  ASSERT_TRUE(BKE_brush_curve_patch_texture_slot_remove(*brush, *later));
  EXPECT_EQ(brush->curve_patch.texture_active_index, 2);
  EXPECT_EQ(brush->curve_patch.texture_slots.count(), 4);
}

TEST_F(BrushTest, curve_patch_texture_slot_remove_decrements_active_when_earlier_slot_removed)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  id_us_min(&brush->id);

  BrushCurvePatchTextureSlot *earlier = BKE_brush_curve_patch_texture_slot_add(*brush);
  BKE_brush_curve_patch_texture_slot_add(*brush);
  BKE_brush_curve_patch_texture_slot_add(*brush);

  brush->curve_patch.texture_active_index = 2;
  ASSERT_TRUE(BKE_brush_curve_patch_texture_slot_remove(*brush, *earlier));
  EXPECT_EQ(brush->curve_patch.texture_active_index, 1);
  EXPECT_EQ(brush->curve_patch.texture_slots.count(), 2);
}

TEST_F(BrushTest, curve_patch_texture_slot_remove_clamps_active_when_last_slot_was_active)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  id_us_min(&brush->id);

  BKE_brush_curve_patch_texture_slot_add(*brush);
  BKE_brush_curve_patch_texture_slot_add(*brush);
  BrushCurvePatchTextureSlot *last = BKE_brush_curve_patch_texture_slot_add(*brush);

  ASSERT_EQ(brush->curve_patch.texture_active_index, 2);
  ASSERT_TRUE(BKE_brush_curve_patch_texture_slot_remove(*brush, *last));
  EXPECT_EQ(brush->curve_patch.texture_active_index, 1);
  EXPECT_EQ(brush->curve_patch.texture_slots.count(), 2);
}

TEST_F(BrushTest, curve_patch_texture_slot_remove_resets_active_when_list_empties)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  id_us_min(&brush->id);

  BrushCurvePatchTextureSlot *only = BKE_brush_curve_patch_texture_slot_add(*brush);
  ASSERT_TRUE(BKE_brush_curve_patch_texture_slot_remove(*brush, *only));
  EXPECT_EQ(brush->curve_patch.texture_active_index, 0);
  EXPECT_EQ(brush->curve_patch.texture_slots.count(), 0);
}

}  // namespace blender
