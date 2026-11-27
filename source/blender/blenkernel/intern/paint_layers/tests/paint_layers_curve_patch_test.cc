/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** The ownership contract of a Curve Patch effect's payload (T4/T5): the Add creates the container
 * with the effect as its single user (brush-less: the snapshot comes from the explicit assign), a
 * duplication of the owning layer copies the payload IDs, a Material copy copies them, and a
 * removal leaves an ID that someone else still uses alive. */

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
#include "BKE_scene.hh"
#include "BKE_texture.h"

#include "../paint_layers_intern.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

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

  /** The Add path the Outliner drives: the effect is created brush-less unless the caller passes
   * a brush candidate, which the policy validates itself. */
  MaterialPaintLayer *add_effect(Material &ma,
                                 MaterialPaintLayer &layer,
                                 const Brush *brush = nullptr)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::CurvePatchEffect;
    params.anchor = &layer;
    params.bmain = bmain;
    params.curve_patch_brush = brush;
    return BKE_paint_layers_add_with_policy(ma, params);
  }

  /** The mask Add: one Base Color map over the layer's base mask. */
  MaterialPaintLayer *add_mask_effect(Material &ma,
                                      MaterialPaintLayer &layer,
                                      const Brush *brush = nullptr)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::MaskCurvePatch;
    params.anchor = &layer;
    params.bmain = bmain;
    params.curve_patch_brush = brush;
    return BKE_paint_layers_add_with_policy(ma, params);
  }

  /** The assign path the Curve Patch panel drives: a snapshot duplicated from the source brush. */
  Brush *assign_brush(Material &ma, MaterialPaintLayer &effect, Brush &brush)
  {
    EXPECT_TRUE(BKE_paint_layers_curve_patch_brush_set(ma, *bmain, effect, brush));
    return effect.curve_patch->brush;
  }
};

TEST_F(PaintLayersCurvePatch, AddCreatesOwnedPayload)
{
  Material *ma = BKE_material_add(bmain, "CPMat");
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);

  MaterialPaintLayer *effect = add_effect(*ma, *layer);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(effect->curve_patch, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_CURVE_PATCH);
  EXPECT_GT(effect->channels_num, 0) << "the default channel set, like a Paint row";

  PaintVector *vectors = effect->curve_patch->vectors;
  ASSERT_NE(vectors, nullptr);
  /* The payload ID is the effect's alone. The brush is NOT created with the effect: the Add hands
   * none out, so no Sculpt brush can slip into a row that runs on a Paint Brush. */
  EXPECT_EQ(vectors->id.us, 1);
  EXPECT_EQ(effect->curve_patch->brush, nullptr);
  /* A container the Add created starts empty; the editors wrapper seeds the first curve. */
  EXPECT_EQ(vectors->items_num, 0);

  /* The explicit assign is what gives the row its snapshot: a full copy of the source, not the
   * source itself, named after it. The source is a Paint Brush, the one rule the assign enforces. */
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  brush->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  Brush *snapshot = assign_brush(*ma, *effect, *brush);
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->id.us, 1);
  EXPECT_NE(snapshot, brush);
  EXPECT_EQ(std::strncmp(snapshot->id.name + 2, ".CP ", 4), 0);

  /* The removal lets both IDs go: no user left. */
  PaintVector *payload_vectors = effect->curve_patch->vectors;
  Brush *payload_brush = effect->curve_patch->brush;
  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  EXPECT_EQ(payload_vectors->id.us, 0);
  EXPECT_EQ(payload_brush->id.us, 0);
}

TEST_F(PaintLayersCurvePatch, AddMaskCorrectionCreatesOneBaseColorMap)
{
  Material *ma = BKE_material_add(bmain, "CPMaskMat");
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  ASSERT_EQ(BKE_paint_layers_mask_base(*layer), nullptr);
  ASSERT_EQ(BLI_listbase_count(&layer->mask_stack), 0);

  MaterialPaintLayer *item = add_mask_effect(*ma, *layer);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(item->source, MA_PAINT_LAYER_SOURCE_CURVE_PATCH);
  EXPECT_EQ(item->kind_subtype, MA_PAINT_LAYER_CURVE_PATCH_MASK);
  ASSERT_NE(BKE_paint_layers_mask_base(*layer), nullptr);
  ASSERT_EQ(BLI_listbase_count(&layer->mask_stack), 2);
  ASSERT_NE(item->curve_patch, nullptr);
  EXPECT_NE(item->curve_patch->vectors, nullptr);
  EXPECT_EQ(item->curve_patch->brush, nullptr);
  EXPECT_EQ(item->channels_num, 1);
  ASSERT_NE(item->channels, nullptr);
  EXPECT_EQ(item->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(item->channels[0].image, nullptr);

  /* The payload lookup and the brush assign both reach a mask item, not only a content effect. */
  EXPECT_EQ(BKE_paint_layers_curve_patch_row_find(*ma, *item->curve_patch), item);
  Brush *brush = BKE_brush_add(bmain, "CPMaskBrush", OB_MODE_SCULPT);
  brush->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  Brush *snapshot = assign_brush(*ma, *item, *brush);
  ASSERT_NE(snapshot, nullptr);
  ASSERT_NE(snapshot->material_paint, nullptr);
  EXPECT_EQ(snapshot->material_paint->mask_source, BRUSH_MATERIAL_PAINT_MASK_SOURCE_BASE_COLOR);

  PaintVector *payload_vectors = item->curve_patch->vectors;
  Brush *payload_brush = item->curve_patch->brush;
  ASSERT_TRUE(BKE_paint_layers_mask_item_remove(*ma, item));
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

TEST_F(PaintLayersCurvePatch, AddTakesUsableBrushCandidate)
{
  Material *ma = BKE_material_add(bmain, "CPAddBrush");
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);

  /* The typical Add: a usable brush in hand gives the effect its snapshot at once, so the tool can
   * open without a separate assign step. The snapshot is still a private copy. */
  Brush *brush = BKE_brush_add(bmain, "CPAddBrushSrc", OB_MODE_SCULPT);
  brush->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  ASSERT_TRUE(BKE_paint_layers_curve_patch_brush_is_assignable(*brush));
  MaterialPaintLayer *effect = add_effect(*ma, *layer, brush);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(effect->curve_patch, nullptr);
  ASSERT_NE(effect->curve_patch->brush, nullptr);
  EXPECT_NE(effect->curve_patch->brush, brush);
  EXPECT_EQ(effect->curve_patch->brush->id.us, 1);
  EXPECT_EQ(std::strncmp(effect->curve_patch->brush->id.name + 2, ".CP ", 4), 0);

  /* An unusable candidate leaves the row brush-less rather than duplicating it. */
  MaterialPaintLayer *second = add_effect(*ma, *layer);
  ASSERT_NE(second, nullptr);
  Brush *mask = BKE_brush_add(bmain, "CPAddMask", OB_MODE_SCULPT);
  mask->sculpt_brush_type = SCULPT_BRUSH_TYPE_MASK;
  mask->mask_tool = BRUSH_MASK_SMOOTH;
  ASSERT_FALSE(BKE_paint_layers_curve_patch_brush_can_stamp(*mask));
  MaterialPaintLayer *third = add_effect(*ma, *layer, mask);
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(third->curve_patch->brush, nullptr);

  /* A geometry brush can stamp a patch, but the effect is a Paint Brush effect: the Add must not
   * pick up the Sculpt brush in hand (the original defect of an Add that captured the active
   * brush). Assigning it explicitly stays possible. */
  Brush *draw = BKE_brush_add(bmain, "CPAddDraw", OB_MODE_SCULPT);
  draw->sculpt_brush_type = SCULPT_BRUSH_TYPE_DRAW;
  ASSERT_TRUE(BKE_paint_layers_curve_patch_brush_can_stamp(*draw));
  ASSERT_FALSE(BKE_paint_layers_curve_patch_brush_is_assignable(*draw));
  MaterialPaintLayer *fourth = add_effect(*ma, *layer, draw);
  ASSERT_NE(fourth, nullptr);
  EXPECT_EQ(fourth->curve_patch->brush, nullptr);
}

TEST_F(PaintLayersCurvePatch, DuplicateLayerCopiesPayload)
{
  Material *ma = BKE_material_add(bmain, "CPDup");
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  brush->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(assign_brush(*ma, *effect, *brush), nullptr);
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
  brush->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(assign_brush(*ma, *effect, *brush), nullptr);
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
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer);
  ASSERT_NE(effect, nullptr);
  PaintVector *vectors = effect->curve_patch->vectors;

  /* A second user of the same container (the legacy manual save path links one this way). */
  id_us_plus(&vectors->id);
  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  /* The effect let go of its user; the external one keeps the container alive. */
  EXPECT_EQ(vectors->id.us, 1);
}

TEST_F(PaintLayersCurvePatch, BrushSetRefusesUnusableSource)
{
  Material *ma = BKE_material_add(bmain, "CPRefuse");
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer);
  ASSERT_NE(effect, nullptr);

  /* Mask Smooth has no Curve Patch equivalent: its type cannot stamp a patch. */
  Brush *mask = BKE_brush_add(bmain, "CPMask", OB_MODE_SCULPT);
  mask->sculpt_brush_type = SCULPT_BRUSH_TYPE_MASK;
  mask->mask_tool = BRUSH_MASK_SMOOTH;
  ASSERT_FALSE(BKE_paint_layers_curve_patch_brush_can_stamp(*mask));
  EXPECT_FALSE(BKE_paint_layers_curve_patch_brush_set(*ma, *bmain, *effect, *mask));
  EXPECT_EQ(effect->curve_patch->brush, nullptr) << "a refused source leaves the row brush-less";

  /* A snapshot is private to its row and must not seed another one. */
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  brush->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  Brush *snapshot = assign_brush(*ma, *effect, *brush);
  ASSERT_NE(snapshot, nullptr);
  ASSERT_FALSE(BKE_paint_layers_curve_patch_brush_can_stamp(*snapshot));
  EXPECT_FALSE(BKE_paint_layers_curve_patch_brush_set(*ma, *bmain, *effect, *snapshot));
  EXPECT_EQ(effect->curve_patch->brush, snapshot) << "the row keeps the snapshot it had";
}

TEST_F(PaintLayersCurvePatch, SnapshotOwnsItsTextures)
{
  Material *ma = BKE_material_add(bmain, "CPTexMat");
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer);
  ASSERT_NE(effect, nullptr);

  Brush *source = BKE_brush_add(bmain, "CPTexSrc", OB_MODE_SCULPT);
  source->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  BKE_brush_material_paint_ensure(source);
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CPTexImage"));
  Tex *brush_tex = BKE_texture_add(bmain, "CPTexBrush");
  brush_tex->type = TEX_IMAGE;
  brush_tex->ima = image;
  id_us_plus(&image->id);
  source->mtex.tex = brush_tex;
  Tex *channel_tex = BKE_texture_add(bmain, "CPTexChannel");
  channel_tex->type = TEX_IMAGE;
  channel_tex->ima = image;
  id_us_plus(&image->id);
  BrushMaterialPaintChannel &source_channel =
      source->material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  source_channel.source_mtex.tex = channel_tex;

  Brush *snapshot = assign_brush(*ma, *effect, *source);
  ASSERT_NE(snapshot, nullptr);

  /* The snapshot is a private duplicate: no Tex is shared with the source, so editing one of them
   * (another image, a cleared slot) can never change the source or a second effect. */
  EXPECT_NE(snapshot->mtex.tex, nullptr);
  EXPECT_NE(snapshot->mtex.tex, brush_tex);
  EXPECT_EQ(source->mtex.tex, brush_tex);
  EXPECT_EQ(brush_tex->ima, image);
  EXPECT_EQ(snapshot->mtex.tex->ima, image) << "the Image itself stays shared";
  BrushMaterialPaintChannel &snapshot_channel =
      snapshot->material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  EXPECT_NE(snapshot_channel.source_mtex.tex, nullptr);
  EXPECT_NE(snapshot_channel.source_mtex.tex, channel_tex);
  EXPECT_EQ(source_channel.source_mtex.tex, channel_tex);
  EXPECT_EQ(ID_REAL_USERS(&brush_tex->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&channel_tex->id), 1);
}

TEST_F(PaintLayersCurvePatch, PresetsSkipBrushSnapshots)
{
  Scene *scene = BKE_scene_add(bmain, "CPPresetScene");
  ASSERT_NE(scene, nullptr);
  Material *ma = BKE_material_add(bmain, "CPPreset");
  MaterialPaintLayer *layer = add_layer(*ma);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *effect = add_effect(*ma, *layer);
  ASSERT_NE(effect, nullptr);
  Brush *brush = BKE_brush_add(bmain, "CPBrush", OB_MODE_SCULPT);
  brush->sculpt_brush_type = SCULPT_BRUSH_TYPE_PAINT;
  Brush *snapshot = assign_brush(*ma, *effect, *brush);
  ASSERT_NE(snapshot, nullptr);

  auto &presets = scene->toolsettings->paint_mode.material_paint_brush_presets;
  /* A snapshot's channels are its private state, kept in the snapshot: a preset keyed by its name
   * would outlive the row and come back on a reused name with another effect's textures. */
  BKE_paint_material_brush_preset_snapshot(*scene, *snapshot);
  EXPECT_EQ(BLI_listbase_count(&presets), 0) << "a snapshot writes no preset";

  /* A normal brush still does, so the check is not skipping everything. */
  BKE_brush_material_paint_ensure(brush);
  BKE_paint_material_brush_preset_snapshot(*scene, *brush);
  EXPECT_EQ(BLI_listbase_count(&presets), 1);

  /* A preset an older build keyed by a snapshot name is stale by construction and is purged. The
   * name is the live snapshot's, so only the prefix rule can drop it (not the orphan check). */
  PaintMaterialBrushPreset *stale = MEM_new<PaintMaterialBrushPreset>(__func__);
  stale->asset_ref = nullptr;
  stale->local_name = BLI_strdup(snapshot->id.name + 2);
  stale->material_paint = BKE_brush_material_paint_create_default();
  BLI_addtail(&presets, stale);
  ASSERT_EQ(BLI_listbase_count(&presets), 2);

  BKE_paint_material_brush_presets_prepare_for_save(bmain);
  EXPECT_EQ(BLI_listbase_count(&presets), 1) << "only the real brush's preset survives";
}

}  // namespace blender::bke::tests
