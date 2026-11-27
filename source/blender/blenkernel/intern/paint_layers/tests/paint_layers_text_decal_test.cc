/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** The ownership contract of a Text Decal effect's payload: the Add takes one user on the Text
 * object (never a copy of it), a duplication of the owning layer and a Material copy share the
 * object, a removal lets it go, and the projection frame comes up in PLANAR space with a live
 * Base Color map. */

#include "testing/testing.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_timer.h"
#include "BLI_path_utils.hh"
#include "BLI_string_utf8.h"

#include "BKE_gtest_base.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_vfont.hh"
#include "BKE_vfont_render.hh"

#include "../paint_layers_intern.hh"

#include "BLF_api.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_curve_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_vfont_types.h"

namespace blender::bke::tests {

namespace {

/* The number of pixels whose alpha is at least half-opaque, in a float raster. */
int imbuf_alpha_count(const ImBuf &ibuf)
{
  const float *pixels = ibuf.float_data();
  if (pixels == nullptr) {
    return 0;
  }
  int count = 0;
  for (const int i : IndexRange(size_t(ibuf.x) * ibuf.y)) {
    count += int(pixels[i * 4 + 3] >= 0.5f);
  }
  return count;
}

/* The number of pixels whose alpha is at least half-opaque, in the map's byte buffer. */
int image_alpha_count(Image &image)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return -1;
  }
  int count = 0;
  if (const uchar *pixels = ibuf->byte_data()) {
    for (const int i : IndexRange(size_t(ibuf->x) * ibuf->y)) {
      count += int(pixels[i * 4 + 3] >= 128);
    }
  }
  else if (const float *pixels = ibuf->float_data()) {
    for (const int i : IndexRange(size_t(ibuf->x) * ibuf->y)) {
      count += int(pixels[i * 4 + 3] >= 0.5f);
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  return count;
}

/* The lowest value of \a plane (0 red, 1 green, 2 blue) among the pixels whose alpha is at least
 * half-opaque (255 when none): a channel map holds its constant in the color planes where the text
 * is. */
int image_covered_plane_min(Image &image, const int plane = 0)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return -1;
  }
  int lowest = 255;
  if (const uchar *pixels = ibuf->byte_data()) {
    for (const int i : IndexRange(size_t(ibuf->x) * ibuf->y)) {
      if (pixels[i * 4 + 3] >= 128) {
        lowest = std::min(lowest, int(pixels[i * 4 + plane]));
      }
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  return lowest;
}

/* The highest value of \a plane among the covered pixels (0 when none). */
int image_covered_plane_max(Image &image, const int plane = 0)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return -1;
  }
  int highest = 0;
  if (const uchar *pixels = ibuf->byte_data()) {
    for (const int i : IndexRange(size_t(ibuf->x) * ibuf->y)) {
      if (pixels[i * 4 + 3] >= 128) {
        highest = std::max(highest, int(pixels[i * 4 + plane]));
      }
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  return highest;
}

/* The number of pixels whose red is at least half, in the map's byte buffer: a mask map's coverage. */
int image_red_count(Image &image)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return -1;
  }
  int count = 0;
  if (const uchar *pixels = ibuf->byte_data()) {
    for (const int i : IndexRange(size_t(ibuf->x) * ibuf->y)) {
      count += int(pixels[i * 4] >= 128);
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  return count;
}

/* Fill the map with fully transparent black, so a later redraw is observable. */
void zero_image(Image &image)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  if (uchar *pixels = ibuf->byte_data_for_write()) {
    memset(pixels, 0, size_t(ibuf->x) * ibuf->y * 4);
  }
  else if (float *pixels = ibuf->float_data_for_write()) {
    memset(pixels, 0, sizeof(float[4]) * size_t(ibuf->x) * ibuf->y);
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
}

}  // namespace

/**
 * The Ahem test font renders every character as the same solid block (an exact fraction of
 * the em box), which keeps the expected ink bounds simple and deterministic.
 */
class PaintLayersTextDecal : public bke::BlenderGTestBase {
 public:
  static void SetUpTestSuite()
  {
    BlenderGTestBase::SetUpTestSuite();
    BLF_init();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
    ma = BKE_material_add(bmain, "TD Mat");
    /* Small maps: the tests bake a real raster, but not a large one. */
    ma->paint_layers_image_size = 256;

    char filepath[FILE_MAX];
    BLI_path_join(filepath,
                  sizeof(filepath),
                  blender::tests::flags_test_asset_dir().c_str(),
                  "blenfont",
                  "Ahem.ttf");
    vfont = BKE_vfont_load(bmain, filepath);
    ASSERT_NE(vfont, nullptr);
  }
  void TearDown() override
  {
    /* The wake timer of a held-back edit-mode raster is only freed by the timer pass the test
     * process never runs. */
    BLI_timer_free();
    BKE_main_free(bmain);
    G_MAIN = nullptr;
    bmain = nullptr;
    ma = nullptr;
    vfont = nullptr;
  }

  /** A plain Paint layer for the effect to hang on. */
  MaterialPaintLayer *add_layer()
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Paint;
    return BKE_paint_layers_add_with_policy(*ma, params);
  }

  /**
   * A Text object carrying the Ahem font and the body "AB". The lifecycle tests never render;
   * the refresh tests do.
   */
  Object *add_text_object(const char *name)
  {
    Object *ob = BKE_object_add_only_object(bmain, OB_FONT, name);
    Curve *curve = static_cast<Curve *>(BKE_libblock_alloc(bmain, ID_CU_LEGACY, name, 0));
    curve->ob_type = OB_FONT;
    curve->totbox = 1;
    curve->tb = MEM_new_array<TextBox>(MAXTEXTBOX, "tb");
    curve->tb[0] = {};
    if (vfont != nullptr) {
      curve->vfont = vfont;
      id_us_plus(&vfont->id);
      set_body(*curve, "AB");
    }
    ob->data = &curve->id;
    id_us_plus(&curve->id);
    return ob;
  }

  /* Replace the body of \a curve (the bytes, the lengths and the per-character styles). */
  static void set_body(Curve &curve, const char *utf8)
  {
    const size_t len_char32 = BLI_strlen_utf8(utf8);
    const size_t len_bytes = strlen(utf8);
    MEM_SAFE_DELETE(curve.str);
    MEM_SAFE_DELETE(curve.strinfo);
    curve.str = MEM_new_array<char>(len_bytes + 1, "str");
    memcpy(curve.str, utf8, len_bytes + 1);
    curve.len = int(len_bytes);
    curve.len_char32 = int(len_char32);
    curve.strinfo = MEM_new_array<CharInfo>(len_char32 + 1, "strinfo");
    for (const int i : IndexRange(int(len_char32) + 1)) {
      curve.strinfo[i] = CharInfo{};
    }
  }

  void set_body(Object &text_object, const char *utf8)
  {
    set_body(*id_cast<Curve *>(text_object.data), utf8);
  }

  MaterialPaintLayer *add_effect(MaterialPaintLayer &layer, Object &text_object)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::TextDecalEffect;
    params.anchor = &layer;
    params.bmain = bmain;
    params.text_object = &text_object;
    /* The tests bake real rasters; ask for the smallest side before the Add bakes its first one,
     * instead of resizing after the default 2048 bake (#MA_PAINT_LAYER_TEXT_SIZE_DEFAULT). */
    params.text_decal_size = MA_PAINT_LAYER_TEXT_SIZE_MIN;
    return BKE_paint_layers_add_with_policy(*ma, params);
  }

  /** The effect's coverage map (the text as alpha), or null while it has none. */
  Image *base_color_image(MaterialPaintLayer &effect)
  {
    return effect.text_decal != nullptr ? effect.text_decal->coverage : nullptr;
  }

  Main *bmain = nullptr;
  Material *ma = nullptr;
  VFont *vfont = nullptr;
};

TEST_F(PaintLayersTextDecal, AddCreatesPayloadAndTakesObjectUser)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");

  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(effect->text_decal, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_TEXT);
  EXPECT_EQ(effect->text_decal->text_object, text);
  EXPECT_EQ(text->id.us, 1) << "the effect is the object's single user";
  /* The fixture pins the smallest raster; the payload default is covered by
   * #SizeGetDefaultsAndClamps. */
  EXPECT_EQ(effect->text_decal->size, MA_PAINT_LAYER_TEXT_SIZE_MIN);
  EXPECT_EQ(effect->text_decal->padding, 0.05f);
  EXPECT_EQ(effect->text_decal->fit, MA_PAINT_LAYER_TEXT_FIT_CONTAIN);

  /* The frame comes up in PLANAR space: the raster is projected, never UV-mapped. */
  EXPECT_NE(effect->projection, nullptr);
  EXPECT_EQ(effect->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR);
  EXPECT_TRUE(BKE_paint_layers_projection_applies(*ma, *effect));

  /* The coverage map exists before the first raster; the channel keeps a flat value. */
  Image *image = effect->text_decal->coverage;
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->id.us, 1);
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->image, nullptr) << "the channel slot is the user's, not the text's";

  /* The removal lets the object and the map go. */
  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  EXPECT_EQ(text->id.us, 0);
  EXPECT_EQ(image->id.us, 0);
}

TEST_F(PaintLayersTextDecal, AddWithoutTextObjectIsRefused)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::TextDecalEffect;
  params.anchor = layer;
  params.bmain = bmain;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr);
  EXPECT_EQ(BLI_listbase_count(&layer->effects), 0) << "a refusal leaves nothing behind";
}

TEST_F(PaintLayersTextDecal, AddRefusedForNonTextObject)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *empty = BKE_object_add_only_object(bmain, OB_EMPTY, "Empty");

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::TextDecalEffect;
  params.anchor = layer;
  params.bmain = bmain;
  params.text_object = empty;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr);
  EXPECT_EQ(BLI_listbase_count(&layer->effects), 0);
  EXPECT_EQ(empty->id.us, 0);
}

TEST_F(PaintLayersTextDecal, DuplicateLayerSharesObject)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, layer);
  ASSERT_NE(copy, nullptr);
  ASSERT_EQ(BLI_listbase_count(&copy->effects), 1);
  const MaterialPaintLayer *copy_effect = static_cast<const MaterialPaintLayer *>(
      copy->effects.first);
  ASSERT_NE(copy_effect->text_decal, nullptr);
  /* The content belongs to the object: the copy names it, it does not get its own. */
  EXPECT_EQ(copy_effect->text_decal->text_object, text);
  EXPECT_EQ(text->id.us, 2);
  /* The raster is the copy's own: it starts empty and the next refresh builds a map of its own
   * rather than sharing one that two rows would resize and rewrite. */
  EXPECT_EQ(copy_effect->text_decal->coverage, nullptr);
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  ASSERT_NE(copy_effect->text_decal->coverage, nullptr);
  EXPECT_NE(copy_effect->text_decal->coverage, effect->text_decal->coverage);

  BKE_id_free(bmain, &ma->id);
  EXPECT_EQ(text->id.us, 0);
  ma = nullptr;
}

TEST_F(PaintLayersTextDecal, MaterialCopySharesObject)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  Material *copy_ma = id_cast<Material *>(
      BKE_id_copy_ex(bmain, &ma->id, nullptr, LIB_ID_COPY_DEFAULT));
  ASSERT_NE(copy_ma, nullptr);
  const MaterialPaintLayer *copy_layer = static_cast<const MaterialPaintLayer *>(
      copy_ma->paint_layers.first);
  ASSERT_NE(copy_layer, nullptr);
  const MaterialPaintLayer *copy_effect = static_cast<const MaterialPaintLayer *>(
      copy_layer->effects.first);
  ASSERT_NE(copy_effect, nullptr);
  ASSERT_NE(copy_effect->text_decal, nullptr);
  /* The generic copy pass added the copy's user through the row's dependencies. */
  EXPECT_EQ(copy_effect->text_decal->text_object, text);
  EXPECT_EQ(text->id.us, 2);
  /* A Material copy rebakes into a coverage of its own on its next refresh. */
  EXPECT_EQ(copy_effect->text_decal->coverage, nullptr);
  BKE_paint_layers_text_decal_refresh(*bmain, *copy_ma);
  ASSERT_NE(copy_effect->text_decal->coverage, nullptr);
  EXPECT_NE(copy_effect->text_decal->coverage, effect->text_decal->coverage);

  BKE_id_free(bmain, &copy_ma->id);
  EXPECT_EQ(text->id.us, 1);
  BKE_id_free(bmain, &ma->id);
  EXPECT_EQ(text->id.us, 0);
  ma = nullptr;
}

TEST_F(PaintLayersTextDecal, ObjectSetSwapsUsers)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text_a = add_text_object("TextA");
  Object *text_b = add_text_object("TextB");
  MaterialPaintLayer *effect = add_effect(*layer, *text_a);
  ASSERT_NE(effect, nullptr);

  /* A non-Text object is refused and changes nothing. */
  Object *empty = BKE_object_add_only_object(bmain, OB_EMPTY, "Empty");
  EXPECT_FALSE(BKE_paint_layers_text_decal_object_set(*ma, *effect, empty));
  EXPECT_EQ(effect->text_decal->text_object, text_a);
  EXPECT_EQ(text_a->id.us, 1);

  EXPECT_TRUE(BKE_paint_layers_text_decal_object_set(*ma, *effect, text_b));
  EXPECT_EQ(effect->text_decal->text_object, text_b);
  EXPECT_EQ(text_a->id.us, 0);
  EXPECT_EQ(text_b->id.us, 1);

  /* Null clears the source. */
  EXPECT_TRUE(BKE_paint_layers_text_decal_object_set(*ma, *effect, nullptr));
  EXPECT_EQ(effect->text_decal->text_object, nullptr);
  EXPECT_EQ(text_b->id.us, 0);
}

TEST_F(PaintLayersTextDecal, DependenciesReportObject)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  bool found = false;
  paint_layer_dependencies(*effect, [&](const PaintLayerDependency &dep) {
    if (dep.kind == PaintLayerDependencyKind::TextDecalObject) {
      EXPECT_EQ(dep.slot, reinterpret_cast<ID **>(&effect->text_decal->text_object));
      EXPECT_EQ(int(dep.influence), PAINT_LAYER_DEP_PIXELS | PAINT_LAYER_DEP_OWNERSHIP);
      found = true;
    }
  });
  EXPECT_TRUE(found);
}

TEST_F(PaintLayersTextDecal, RefreshBakesAndSkipsUnchanged)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  Image *image = base_color_image(*effect);
  ASSERT_NE(image, nullptr);
  /* The Add bakes right away: the glyphs are on the map. */
  EXPECT_GT(image_alpha_count(*image), 0);

  /* Wipe the map: a refresh without a change must not redraw, so the wipe stays. */
  zero_image(*image);
  EXPECT_EQ(image_alpha_count(*image), 0);
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_EQ(image_alpha_count(*image), 0) << "an unchanged hash means no redraw";

  /* An edit of the text changes the hash: the next refresh re-bakes. */
  set_body(*text, "ABAB");
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_GT(image_alpha_count(*image), 0);
}

TEST_F(PaintLayersTextDecal, HashFollowsPaddingAndFont)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  Image *image = base_color_image(*effect);
  ASSERT_NE(image, nullptr);

  /* A settled state hashes the same: the refresh leaves the wiped map alone. */
  zero_image(*image);
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_EQ(image_alpha_count(*image), 0) << "an unchanged font and padding mean no redraw";

  /* The padding is a raster input. */
  effect->text_decal->padding = 0.2f;
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_GT(image_alpha_count(*image), 0) << "a padding edit re-bakes";

  zero_image(*image);
  Curve *curve = id_cast<Curve *>(text->data);
  /* A fresh Curve may carry a zero font size, which the layout reads as the default; give it a
   * concrete value so doubling it is a real edit of the hash. */
  if (curve->fsize <= 0.0f) {
    curve->fsize = 1.0f;
    BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  }
  zero_image(*image);
  curve->fsize *= 2.0f;
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_GT(image_alpha_count(*image), 0) << "a font size edit re-bakes";

  zero_image(*image);
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_EQ(image_alpha_count(*image), 0) << "the new state settles again";
}

TEST_F(PaintLayersTextDecal, SizeChangeRecreatesMap)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  Image *image = base_color_image(*effect);
  ASSERT_NE(image, nullptr);
  int width = 0, height = 0;
  BKE_image_get_size(image, nullptr, &width, &height);
  EXPECT_EQ(width, MA_PAINT_LAYER_TEXT_SIZE_MIN);

  /* The map follows the payload's size, not the material's, and is resized in place so the
   * generated tree keeps naming it. */
  effect->text_decal->size = 512;
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  Image *resized = base_color_image(*effect);
  ASSERT_NE(resized, nullptr);
  EXPECT_EQ(resized, image) << "the map is resized, not replaced";
  BKE_image_get_size(resized, nullptr, &width, &height);
  EXPECT_EQ(width, 512);
  EXPECT_EQ(height, 512);
  EXPECT_GT(image_alpha_count(*resized), 0);
  /* A second pass at the same size keeps the map: no churn. */
  Image *same = base_color_image(*effect);
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_EQ(base_color_image(*effect), same);
}

TEST_F(PaintLayersTextDecal, SizeGetDefaultsAndClamps)
{
  MaterialPaintLayerText payload;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_get(payload), MA_PAINT_LAYER_TEXT_SIZE_DEFAULT);
  payload.size = 8;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_get(payload), 256);
  payload.size = 999999;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_get(payload), 4096);
  payload.size = 512;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_get(payload), 512);
}

TEST_F(PaintLayersTextDecal, SizeStepMovesByPowersOfTwo)
{
  MaterialPaintLayerText payload;
  payload.size = 1024;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_step(payload, 1), 2048);
  EXPECT_EQ(BKE_paint_layers_text_decal_size_step(payload, -1), 512);
  payload.size = 4096;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_step(payload, 1), 4096) << "held at the cap";
  payload.size = 256;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_step(payload, -1), 256) << "held at the floor";
  payload.size = 0;
  EXPECT_EQ(BKE_paint_layers_text_decal_size_step(payload, 1), 4096) << "0 reads as the default";
}

TEST_F(PaintLayersTextDecal, ProjectionSupportFollowsTheMap)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  /* The raster is read through the frame, exactly like a decal. */
  EXPECT_TRUE(BKE_paint_layers_projection_supported(*ma, *effect));
  EXPECT_TRUE(BKE_paint_layers_projection_applies(*ma, *effect));
  /* A Decal frame is the same flat frame as Planar, and the text map reads through it too. */
  EXPECT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, effect, MA_PAINT_LAYER_MAPPING_SPACE_DECAL));
  EXPECT_EQ(effect->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_DECAL);
  EXPECT_TRUE(BKE_paint_layers_projection_applies(*ma, *effect));

  /* Flat channel values are projected with the coverage; a dropped texture follows the same frame. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *texture = paint_layers_map_create(*bmain, "Texture", 4, true, black);
  ASSERT_NE(texture, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, texture));
  EXPECT_TRUE(BKE_paint_layers_projection_applies(*ma, *effect));

  /* Without the coverage there is nothing the frame places. */
  id_us_min(&effect->text_decal->coverage->id);
  effect->text_decal->coverage = nullptr;
  EXPECT_FALSE(BKE_paint_layers_projection_supported(*ma, *effect));
  EXPECT_FALSE(BKE_paint_layers_projection_applies(*ma, *effect));
}

TEST_F(PaintLayersTextDecal, DeletedObjectDoesNotCrash)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  /* The deletion remaps the material's dependencies: the payload's pointer is the slot. */
  BKE_id_delete(bmain, &text->id);
  EXPECT_EQ(effect->text_decal->text_object, nullptr);

  /* The refresh skips the row and prunes its state; the map keeps the last raster it had. */
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  Image *image = base_color_image(*effect);
  ASSERT_NE(image, nullptr);
  EXPECT_GT(image_alpha_count(*image), 0);
}

TEST_F(PaintLayersTextDecal, ReBakeInvalidationRedraws)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  Image *image = base_color_image(*effect);
  ASSERT_NE(image, nullptr);

  zero_image(*image);
  BKE_paint_layers_text_decal_invalidate(*ma);
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_GT(image_alpha_count(*image), 0) << "the forced refresh ignores the cached hash";
}

TEST_F(PaintLayersTextDecal, EmptyMaterialCachesAbsenceThenAddStartsPolling)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  /* The first pass remembers that the material has no Text Decal. */
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);

  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr) << "the add clears the cached absence";
  Image *image = base_color_image(*effect);
  ASSERT_NE(image, nullptr);
  EXPECT_GT(image_alpha_count(*image), 0);
}

TEST_F(PaintLayersTextDecal, SmartAssetRefusesTextDecal)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  ASSERT_NE(add_effect(*layer, *text), nullptr);

  const MaterialPaintLayer *rows_array[1] = {layer};
  const Span<const MaterialPaintLayer *> rows(rows_array, 1);
  const PaintLayerAssetPortability portability = BKE_paint_layers_asset_portability_check(
      *ma, rows, PaintLayerAssetType::Layer);
  EXPECT_EQ(portability.text_decal_num, 1);
  EXPECT_TRUE(portability.blocking());
}

TEST_F(PaintLayersTextDecal, EffectStartsLikeAFillWithAWhiteBaseColor)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  /* Base Color alone, as a Fill starts: the other channels are opted into. */
  ASSERT_EQ(effect->channels_num, 1);
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->value[0], 1.0f);
  EXPECT_EQ(record->value[1], 1.0f);
  EXPECT_EQ(record->value[2], 1.0f);
  EXPECT_EQ(record->value[3], 1.0f);
  EXPECT_EQ(paint_layer_channel_find(*effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
}

TEST_F(PaintLayersTextDecal, CoverageIsAlphaOverWhiteAndChannelValuesAreNotInIt)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  Image *coverage = base_color_image(*effect);
  ASSERT_NE(coverage, nullptr);
  const int covered = image_alpha_count(*coverage);
  ASSERT_GT(covered, 0);
  EXPECT_EQ(image_covered_plane_min(*coverage, 0), 255);

  /* The colour is the record's, laid by the generator and the composite: a new constant touches
   * neither the coverage nor the raster. */
  MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  zero_image(*coverage);
  record->value[0] = 1.0f;
  record->value[1] = 0.0f;
  record->value[2] = 0.0f;
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_EQ(image_alpha_count(*coverage), 0) << "a value edit never re-rasterizes";
  EXPECT_EQ(record->image, nullptr);
}

TEST_F(PaintLayersTextDecal, ChannelEditsNeverRewriteTheCoverage)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  Image *coverage = base_color_image(*effect);
  ASSERT_NE(coverage, nullptr);

  /* A channel the user switches on keeps a flat value and no map of its own. */
  MaterialPaintLayerChannel *roughness = BKE_paint_layers_channel_add(
      *ma, effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(roughness, nullptr);
  zero_image(*coverage);
  const float value[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  roughness = paint_layer_channel_find(*effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_EQ(roughness->image, nullptr);
  EXPECT_EQ(roughness->value[0], 0.25f);
  EXPECT_EQ(image_alpha_count(*coverage), 0) << "the value edit did no raster work";
}

TEST_F(PaintLayersTextDecal, DroppedTextureOnAChannelSurvivesTheRaster)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *texture = paint_layers_map_create(*bmain, "Texture", 4, true, black);
  ASSERT_NE(texture, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, texture));
  set_body(*text, "ABC");
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_EQ(paint_layer_channel_find(*effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR)->image, texture)
      << "the raster writes the coverage only";
  EXPECT_GT(image_alpha_count(*base_color_image(*effect)), 0);
  /* A Fill-style row: the texture is a source for strokes to leave alone. */
  EXPECT_FALSE(BKE_paint_layers_row_is_paintable(*effect));
}

TEST_F(PaintLayersTextDecal, MaskItemHasOneChannelHoldingCoverage)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskTextDecal;
  params.anchor = layer;
  params.bmain = bmain;
  params.text_object = text;
  MaterialPaintLayer *item = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(item, nullptr);
  /* The base mask the item lays over was made with it. */
  EXPECT_NE(BKE_paint_layers_mask_base(*layer), nullptr);
  EXPECT_EQ(item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(item->source, MA_PAINT_LAYER_SOURCE_TEXT);
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(*ma, *item), layer);
  ASSERT_NE(item->text_decal, nullptr);
  EXPECT_EQ(text->id.us, 1);

  /* Exactly one channel: the Base Color map, whose grey is the coverage. */
  ASSERT_EQ(item->channels_num, 1);
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  ASSERT_NE(record->image, nullptr);
  EXPECT_GT(image_red_count(*record->image), 0);
  EXPECT_TRUE(paint_layer_mask_item_lays(*item));

  /* It is projected through its own frame, like the effect form. */
  EXPECT_TRUE(BKE_paint_layers_projection_applies(*ma, *item));

  /* A later edit of the text re-bakes the mask map too. */
  zero_image(*record->image);
  set_body(*text, "ABAB");
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_GT(image_red_count(*record->image), 0);

  ASSERT_TRUE(BKE_paint_layers_mask_item_remove(*ma, item));
  EXPECT_EQ(text->id.us, 0);
}

TEST_F(PaintLayersTextDecal, StrokesAreRefusedOnTheEffectAndTheMaskItem)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);

  /* The text is the effect's content: no stroke resolves a target on it. */
  BKE_paint_layers_active_set(*ma, effect->marker);
  PaintLayersTarget target;
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target),
            PaintLayersTargetStatus::RowTakesNoStrokes);

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskTextDecal;
  params.anchor = layer;
  params.bmain = bmain;
  params.text_object = text;
  MaterialPaintLayer *item = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(item, nullptr);
  BKE_paint_layers_active_set(*ma, item->marker);
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target),
            PaintLayersTargetStatus::RowTakesNoStrokes);
}

TEST_F(PaintLayersTextDecal, MaskItemDuplicatesWithItsOwner)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskTextDecal;
  params.anchor = layer;
  params.bmain = bmain;
  params.text_object = text;
  ASSERT_NE(BKE_paint_layers_add_with_policy(*ma, params), nullptr);

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, layer);
  ASSERT_NE(copy, nullptr);
  int text_items = 0;
  for (const MaterialPaintLayer &item : copy->mask_stack) {
    if (item.source == MA_PAINT_LAYER_SOURCE_TEXT) {
      text_items++;
      ASSERT_NE(item.text_decal, nullptr);
      EXPECT_EQ(item.text_decal->text_object, text);
    }
  }
  EXPECT_EQ(text_items, 1);
  EXPECT_EQ(text->id.us, 2);
}

TEST_F(PaintLayersTextDecal, MaskAddRefusedForNonTextObject)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *empty = BKE_object_add_only_object(bmain, OB_EMPTY, "Empty");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskTextDecal;
  params.anchor = layer;
  params.bmain = bmain;
  params.text_object = empty;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_base(*layer), nullptr) << "a refusal leaves no base mask behind";
}

TEST_F(PaintLayersTextDecal, EditModeRefreshWaitsForTypingToStop)
{
  MaterialPaintLayer *layer = add_layer();
  ASSERT_NE(layer, nullptr);
  Object *text = add_text_object("Text");
  MaterialPaintLayer *effect = add_effect(*layer, *text);
  ASSERT_NE(effect, nullptr);
  Image *image = base_color_image(*effect);
  ASSERT_NE(image, nullptr);

  /* A live edit buffer holding a different text: the raster must not follow the first keystroke. */
  Curve *curve = id_cast<Curve *>(text->data);
  EditFont *editfont = MEM_new<EditFont>(__func__);
  const char32_t body[] = U"ABABAB";
  editfont->len = 6;
  editfont->textbuf = MEM_new_array<char32_t>(7, __func__);
  editfont->textbufinfo = MEM_new_array<CharInfo>(7, __func__);
  for (const int i : IndexRange(7)) {
    editfont->textbuf[i] = body[i];
  }
  curve->editfont = editfont;

  zero_image(*image);
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_EQ(image_alpha_count(*image), 0) << "typing is not rasterized per keystroke";

  /* Leaving Edit Mode renders at once, whatever the pause. The body differs from the last raster,
   * as an edit that ended with a change does; an unchanged hash would rightly skip the raster. */
  curve->editfont = nullptr;
  set_body(*text, "ABAB");
  BKE_paint_layers_text_decal_refresh(*bmain, *ma);
  EXPECT_GT(image_alpha_count(*image), 0);

  MEM_delete(editfont->textbufinfo);
  MEM_delete(editfont->textbuf);
  MEM_delete(editfont);
}

TEST_F(PaintLayersTextDecal, BendRendersAndFallsBackOnDegenerateCurve)
{
  Object *text = add_text_object("Text");
  Curve *curve = id_cast<Curve *>(text->data);
  if (curve->fsize <= 0.0f) {
    curve->fsize = 1.0f;
  }

  /* All three modes draw ink for a two-point polyline; the mode only changes where the glyphs go,
   * not whether the bent raster replaces the straight one. */
  const std::array<float2, 2> points = {float2{0.1f, 0.5f}, float2{0.9f, 0.5f}};
  for (const VFontRenderBendMode mode : {VFontRenderBendMode::Fit,
                                         VFontRenderBendMode::Clip,
                                         VFontRenderBendMode::Clamp})
  {
    VFontRenderParams params;
    params.side = 64;
    params.bend.points = points;
    params.bend.mode = mode;
    ImBuf *raster = BKE_vfont_object_render(*text, params);
    ASSERT_NE(raster, nullptr);
    EXPECT_GT(imbuf_alpha_count(*raster), 0) << "bend mode " << int(mode);
    IMB_freeImBuf(raster);
  }

  /* A single point cannot form a polyline: the bend is refused and the straight raster returned,
   * not a null one. */
  const std::array<float2, 1> one_point = {float2{0.5f, 0.5f}};
  VFontRenderParams params;
  params.side = 64;
  params.bend.points = one_point;
  ImBuf *raster = BKE_vfont_object_render(*text, params);
  ASSERT_NE(raster, nullptr);
  EXPECT_GT(imbuf_alpha_count(*raster), 0);
  IMB_freeImBuf(raster);
}

}  // namespace blender::bke::tests
