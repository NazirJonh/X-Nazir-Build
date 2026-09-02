/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "BKE_brush.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_remap.hh"
#include "BKE_main.hh"
#include "BKE_main_namemap.hh"

#include "BLI_ghash.h"
#include "BLI_listbase.h"

#include "DNA_ID.h"
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

/* -------------------------------------------------------------------- */
/** \name Stale PBR Paint ID Backstop
 *
 * #BKE_brush_material_paint_stale_ids_clear compares addresses only, so these tests point the
 * brush at IDs living in a second #Main rather than at freed memory: absent from the #Main being
 * cleaned is exactly the state an undo step leaves behind, and nothing is ever dereferenced.
 * \{ */

class BrushStaleIDsTest : public BrushTest {
 public:
  Main *other_bmain = nullptr;

  void SetUp() override
  {
    BrushTest::SetUp();
    other_bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(other_bmain);
    BrushTest::TearDown();
  }

  /** A brush with an allocated #BrushMaterialPaint and no extra user from #BKE_brush_add. */
  Brush *brush_with_material_paint_add()
  {
    Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
    id_us_min(&brush->id);
    BKE_brush_material_paint_ensure(brush);
    return brush;
  }
};

TEST_F(BrushStaleIDsTest, source_material_missing_from_main_is_cleared)
{
  Brush *brush = brush_with_material_paint_add();
  /* Deliberately no #id_us_plus: this stands in for a pointer an undo step left behind, whose
   * user count no longer exists. */
  brush->material_paint->source_material = static_cast<Material *>(
      BKE_id_new(other_bmain, ID_MA, "UnitTestMaterial"));

  EXPECT_EQ(BKE_brush_material_paint_stale_ids_clear(*bmain), 1);
  EXPECT_EQ(brush->material_paint->source_material, nullptr);
}

TEST_F(BrushStaleIDsTest, source_material_present_in_main_is_kept)
{
  Brush *brush = brush_with_material_paint_add();
  Material *ma = static_cast<Material *>(BKE_id_new(bmain, ID_MA, "UnitTestMaterial"));
  brush->material_paint->source_material = ma;
  id_us_plus(&ma->id);

  EXPECT_EQ(BKE_brush_material_paint_stale_ids_clear(*bmain), 0);
  EXPECT_EQ(brush->material_paint->source_material, ma);
}

TEST_F(BrushStaleIDsTest, channel_texture_missing_from_main_is_cleared)
{
  Brush *brush = brush_with_material_paint_add();
  brush->material_paint->channels[0].source_mtex.tex = static_cast<Tex *>(
      BKE_id_new(other_bmain, ID_TE, "UnitTestTexture"));

  EXPECT_EQ(BKE_brush_material_paint_stale_ids_clear(*bmain), 1);
  EXPECT_EQ(brush->material_paint->channels[0].source_mtex.tex, nullptr);
}

TEST_F(BrushStaleIDsTest, shared_mapping_texture_missing_from_main_is_cleared)
{
  Brush *brush = brush_with_material_paint_add();
  brush->material_paint->shared_source_mapping.tex = static_cast<Tex *>(
      BKE_id_new(other_bmain, ID_TE, "UnitTestTexture"));

  EXPECT_EQ(BKE_brush_material_paint_stale_ids_clear(*bmain), 1);
  EXPECT_EQ(brush->material_paint->shared_source_mapping.tex, nullptr);
}

TEST_F(BrushStaleIDsTest, every_stale_reference_is_counted)
{
  Brush *brush = brush_with_material_paint_add();
  brush->material_paint->source_material = static_cast<Material *>(
      BKE_id_new(other_bmain, ID_MA, "UnitTestMaterial"));
  brush->material_paint->channels[0].source_mtex.tex = static_cast<Tex *>(
      BKE_id_new(other_bmain, ID_TE, "UnitTestTextureA"));
  brush->material_paint->shared_source_mapping.tex = static_cast<Tex *>(
      BKE_id_new(other_bmain, ID_TE, "UnitTestTextureB"));

  EXPECT_EQ(BKE_brush_material_paint_stale_ids_clear(*bmain), 3);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source Material Localization
 *
 * #BrushMaterialPaint is allocated separately from #Brush (#MEM_new), so nothing reaches
 * #BrushMaterialPaint.source_material except the generic ID traversal in `brush_foreach_id()`.
 * These tests pin that down: if the traversal ever stops covering the field, a brush made local
 * keeps pointing at the linked material, which is both an illegal linked -> local reference and a
 * pointer that undo can leave dangling.
 * \{ */

class BrushSourceMaterialLocalTest : public BrushTest {
 public:
  /** Move \a id into \a lib, keeping the name map consistent. */
  void id_move_to_library(ID &id, Library *lib)
  {
    BKE_main_namemap_remove_id(*bmain, id);
    id.lib = lib;
    id.tag |= ID_TAG_EXTERN;
    BKE_main_namemap_get_unique_name(*bmain, id, id.name + 2);
  }
};

TEST_F(BrushSourceMaterialLocalTest, source_material_is_remapped)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  id_us_min(&brush->id);
  BKE_brush_material_paint_ensure(brush);

  Material *ma_old = BKE_id_new<Material>(bmain, "UnitTestMaterialOld");
  Material *ma_new = BKE_id_new<Material>(bmain, "UnitTestMaterialNew");
  brush->material_paint->source_material = ma_old;
  id_us_plus(&ma_old->id);

  BKE_libblock_remap(bmain, ma_old, ma_new, ID_REMAP_SKIP_USER_CLEAR);

  /* The whole point: the remap has to reach a field inside the separately allocated
   * #BrushMaterialPaint, which it can only do through `brush_foreach_id()`. */
  EXPECT_EQ(brush->material_paint->source_material, ma_new);
}

TEST_F(BrushSourceMaterialLocalTest, source_material_is_cleared_on_remap_to_null)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  id_us_min(&brush->id);
  BKE_brush_material_paint_ensure(brush);

  Material *ma = BKE_id_new<Material>(bmain, "UnitTestMaterial");
  brush->material_paint->source_material = ma;
  id_us_plus(&ma->id);

  BKE_libblock_remap(bmain, ma, nullptr, ID_REMAP_SKIP_USER_CLEAR);

  EXPECT_EQ(brush->material_paint->source_material, nullptr);
}

TEST_F(BrushSourceMaterialLocalTest, make_local_localizes_brush_and_its_source_material)
{
  /* S3 of the spec: a linked brush whose source material is linked as well. Making the library
   * local has to localize both, and leave the brush pointing at the local copy of the material. */
  Library *lib = BKE_id_new<Library>(bmain, "UnitTestLibrary");

  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  id_us_min(&brush->id);
  BKE_brush_material_paint_ensure(brush);

  Material *ma = BKE_id_new<Material>(bmain, "UnitTestMaterial");
  brush->material_paint->source_material = ma;
  id_us_plus(&ma->id);

  id_move_to_library(brush->id, lib);
  id_move_to_library(ma->id, lib);
  ASSERT_TRUE(ID_IS_LINKED(&brush->id));
  ASSERT_TRUE(ID_IS_LINKED(&ma->id));

  GHash *old_to_new_ids = BLI_ghash_ptr_new(__func__);
  BKE_library_make_local(bmain, lib, old_to_new_ids, false, false, false);

  /* #BKE_library_make_local may localize in place (the ID keeps its address and simply loses its
   * library) or through a copy recorded in the hash; accept either. */
  Brush *local_brush = static_cast<Brush *>(BLI_ghash_lookup(old_to_new_ids, brush));
  if (local_brush == nullptr) {
    local_brush = brush;
  }
  Material *local_ma = static_cast<Material *>(BLI_ghash_lookup(old_to_new_ids, ma));
  if (local_ma == nullptr) {
    local_ma = ma;
  }
  BLI_ghash_free(old_to_new_ids, nullptr, nullptr);

  ASSERT_NE(local_brush, nullptr);
  ASSERT_NE(local_brush->material_paint, nullptr);
  EXPECT_FALSE(ID_IS_LINKED(&local_brush->id));
  EXPECT_FALSE(ID_IS_LINKED(&local_ma->id));
  /* A local brush must never be left referencing the linked material. */
  EXPECT_EQ(local_brush->material_paint->source_material, local_ma);
  EXPECT_FALSE(ID_IS_LINKED(&local_brush->material_paint->source_material->id));
}

/** \} */

}  // namespace blender
