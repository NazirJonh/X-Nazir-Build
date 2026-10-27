/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** The ownership contract of a Curve Patch effect's payload (T4/T5): the Add creates the two IDs
 * with the effect as their single user, a duplication of the owning layer copies them, a Material
 * copy copies them, and a removal leaves an ID that someone else still uses alive. */

#include "testing/testing.h"

#include <cstring>

#include "MEM_guardedalloc.h"

#include "BKE_brush.hh"
#include "BKE_curves.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_vector.hh"

#include "../paint_layers_intern.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

namespace blender::bke::tests {

class PaintLayersCurvePatch : public bke::BlenderGTestBase {
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

  /** A plain Paint layer for the effect to hang on. */
  MaterialPaintLayer *add_layer(Material &ma)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Paint;
    return BKE_paint_layers_add_with_policy(ma, params);
  }

  /** The Add path the Outliner drives: a source brush from the test's own database. */
  MaterialPaintLayer *add_effect(Material &ma, MaterialPaintLayer &layer, Brush &brush)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::CurvePatchEffect;
    params.anchor = &layer;
    params.bmain = bmain;
    params.curve_patch_brush = &brush;
    return BKE_paint_layers_add_with_policy(ma, params);
  }
};

TEST_F(PaintLayersCurvePatch, AddCreatesOwnedPayload)
{
  Material *ma = BKE_material_add(bmain, "CPMat");
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);

  MaterialPaintLayer *effect = add_effect(*ma, *layer, *brush);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(effect->curve_patch, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_CURVE_PATCH);
  EXPECT_GT(effect->channels_num, 0) << "the default channel set, like a Paint row";

  PaintVector *vectors = effect->curve_patch->vectors;
  Brush *snapshot = effect->curve_patch->brush;
  ASSERT_NE(vectors, nullptr);
  ASSERT_NE(snapshot, nullptr);
  /* The payload IDs are the effect's alone, and the snapshot is not the source brush. */
  EXPECT_EQ(vectors->id.us, 1);
  EXPECT_EQ(snapshot->id.us, 1);
  EXPECT_NE(snapshot, brush);
  EXPECT_EQ(std::strncmp(snapshot->id.name + 2, ".CP Brush ", 10), 0);
  /* A container the Add created starts empty; the editors wrapper seeds the first curve. */
  EXPECT_EQ(vectors->items_num, 0);

  /* The removal lets both IDs go: no user left. */
  PaintVector *payload_vectors = effect->curve_patch->vectors;
  Brush *payload_brush = effect->curve_patch->brush;
  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  EXPECT_EQ(payload_vectors->id.us, 0);
  EXPECT_EQ(payload_brush->id.us, 0);
}

TEST_F(PaintLayersCurvePatch, AddWithoutDatabaseIsRefused)
{
  Material *ma = BKE_material_add(bmain, "CPNoMain");
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::CurvePatchEffect;
  params.anchor = layer;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr);
  EXPECT_EQ(BLI_listbase_count(&layer->effects), 0) << "a refusal leaves nothing behind";
}

TEST_F(PaintLayersCurvePatch, DuplicateLayerCopiesPayload)
{
  Material *ma = BKE_material_add(bmain, "CPDup");
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer, *brush);
  ASSERT_NE(effect, nullptr);
  PaintVector *original_vectors = effect->curve_patch->vectors;
  Brush *original_brush = effect->curve_patch->brush;

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, layer);
  ASSERT_NE(copy, nullptr);
  ASSERT_EQ(BLI_listbase_count(&copy->effects), 1);
  const MaterialPaintLayer *copy_effect = static_cast<const MaterialPaintLayer *>(
      copy->effects.first);
  ASSERT_NE(copy_effect->curve_patch, nullptr);
  /* Deep copy: the two effects edit two containers and two snapshots. */
  EXPECT_NE(copy_effect->curve_patch->vectors, original_vectors);
  EXPECT_NE(copy_effect->curve_patch->brush, original_brush);
  EXPECT_EQ(copy_effect->curve_patch->vectors->id.us, 1);
  EXPECT_EQ(copy_effect->curve_patch->brush->id.us, 1);
  EXPECT_EQ(original_vectors->id.us, 1);
  EXPECT_EQ(original_brush->id.us, 1);

  /* The materials first: their free pass walks the payload pointers and drops the users, so
   * freeing the payload IDs afterwards is the plain end of their life. */
  PaintVector *copy_vectors = copy_effect->curve_patch->vectors;
  Brush *copy_brush = copy_effect->curve_patch->brush;
  BKE_id_free(bmain, &ma->id);
  BKE_id_free(bmain, &original_vectors->id);
  BKE_id_free(bmain, &original_brush->id);
  BKE_id_free(bmain, &copy_vectors->id);
  BKE_id_free(bmain, &copy_brush->id);
}

TEST_F(PaintLayersCurvePatch, MaterialCopyCopiesPayload)
{
  Material *ma = BKE_material_add(bmain, "CPCopyMat");
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer, *brush);
  ASSERT_NE(effect, nullptr);
  PaintVector *original_vectors = effect->curve_patch->vectors;
  Brush *original_brush = effect->curve_patch->brush;

  Material *copy_ma = id_cast<Material *>(
      BKE_id_copy_ex(bmain, &ma->id, nullptr, LIB_ID_COPY_DEFAULT));
  ASSERT_NE(copy_ma, nullptr);
  const MaterialPaintLayer *copy_layer = static_cast<const MaterialPaintLayer *>(
      copy_ma->paint_layers.first);
  ASSERT_NE(copy_layer, nullptr);
  const MaterialPaintLayer *copy_effect = static_cast<const MaterialPaintLayer *>(
      copy_layer->effects.first);
  ASSERT_NE(copy_effect, nullptr);
  ASSERT_NE(copy_effect->curve_patch, nullptr);
  /* The copy owns duplicates, each with exactly the copy effect's reference as its user: the
   * construction user was dropped, the generic copy pass added the effect's. */
  EXPECT_NE(copy_effect->curve_patch->vectors, original_vectors);
  EXPECT_NE(copy_effect->curve_patch->brush, original_brush);
  EXPECT_EQ(copy_effect->curve_patch->vectors->id.us, 1);
  EXPECT_EQ(copy_effect->curve_patch->brush->id.us, 1);

  /* The materials first: their free pass walks the payload pointers and drops the users. */
  PaintVector *copy_vectors = copy_effect->curve_patch->vectors;
  Brush *copy_brush = copy_effect->curve_patch->brush;
  BKE_id_free(bmain, &copy_ma->id);
  BKE_id_free(bmain, &ma->id);
  BKE_id_free(bmain, &original_vectors->id);
  BKE_id_free(bmain, &original_brush->id);
  BKE_id_free(bmain, &copy_vectors->id);
  BKE_id_free(bmain, &copy_brush->id);
}

TEST_F(PaintLayersCurvePatch, RemovalSurvivesExternalUser)
{
  Material *ma = BKE_material_add(bmain, "CPShared");
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer, *brush);
  ASSERT_NE(effect, nullptr);
  PaintVector *vectors = effect->curve_patch->vectors;

  /* A second user of the same container (the legacy manual save path links one this way). */
  id_us_plus(&vectors->id);
  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  /* The effect let go of its user; the external one keeps the container alive. */
  EXPECT_EQ(vectors->id.us, 1);
}

}  // namespace blender::bke::tests
