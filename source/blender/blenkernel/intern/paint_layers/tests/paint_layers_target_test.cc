/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for #BKE_paint_layers_target.hh: resolving the active row's paint target and growing the
 * description on the first stroke into a channel the row has no map for yet.
 */

#include "testing/testing.h"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_target.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "../paint_layers_intern.hh"
#include "paint_layers_test_util.hh"

namespace blender::bke::tests {

class PaintLayersTargetTest : public PaintLayersTestBase {
 public:
  void SetUp() override
  {
    PaintLayersTestBase::SetUp();
    /* A new material carries 4K; the tests pass their own tiny sizes, which only win while the
     * material has none of its own. The size tests set it back explicitly. */
    ma->paint_layers_image_size = 0;
  }

  /** A Paint row on top of \a ma, made the active one. */
  MaterialPaintLayer *add_active_row(const char *name)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    BKE_paint_layers_active_set(*ma, layer->marker);
    return layer;
  }

  /** Composite \a channel of \a ma into a float image and read its first pixel. */
  void composite_pixel(const int channel, const int size, float r_rgba[4])
  {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *dst = BKE_image_add_generated(
        bmain, size, size, "Dst", 32, true, IMA_GENTYPE_BLANK, black, false, true, false);
    ASSERT_NE(dst, nullptr);
    EXPECT_TRUE(BKE_paint_layers_composite_image(*ma, channel, *dst, nullptr, nullptr));
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    copy_v4_v4(r_rgba, ibuf->float_data());
    BKE_image_release_ibuf(dst, ibuf, lock);
    BKE_id_free(bmain, dst);
  }

  /** The first pixel of \a image's byte buffer. */
  static void first_byte_pixel(Image &image, uchar r_rgba[4])
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    const uchar *pixels = ibuf->byte_data();
    for (const int i : IndexRange(4)) {
      r_rgba[i] = pixels[i];
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** The first pixel of \a image's byte buffer, read from UDIM \a tile_number. */
  static void tile_byte_pixel(Image &image, const int tile_number, uchar r_rgba[4])
  {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &iuser, &lock);
    ASSERT_NE(ibuf, nullptr);
    const uchar *pixels = ibuf->byte_data();
    for (const int i : IndexRange(4)) {
      r_rgba[i] = pixels[i];
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** A Non-Color byte mask map, every texel the opaque grey \a gray; \a size x \a size, optionally
   * tiled (UDIM, tile 1001 only). */
  Image *add_gray_mask(const char *name, const int size, const uchar gray, const bool tiled = false)
  {
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, clear, false, true, tiled);
    EXPECT_NE(image, nullptr);
    fill_tile_gray(*image, 1001, gray);
    return image;
  }

  /** Fill every texel of \a image's UDIM \a tile_number with the opaque grey \a gray. */
  static void fill_tile_gray(Image &image, const int tile_number, const uchar gray)
  {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &iuser, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      pixels[i * 4 + 0] = gray;
      pixels[i * 4 + 1] = gray;
      pixels[i * 4 + 2] = gray;
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** An object with a mesh and \a materials in its slots, the first one active. */
  Object *add_object_with(const Span<Material *> materials)
  {
    Mesh *mesh = BKE_mesh_add(bmain, "Mesh");
    Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "Object");
    ob->data = &mesh->id;
    id_us_plus(&mesh->id);
    for (const int i : materials.index_range()) {
      BKE_object_material_slot_add(bmain, ob);
      BKE_object_material_assign(bmain, ob, materials[i], short(i + 1), BKE_MAT_ASSIGN_OBJECT);
    }
    ob->actcol = 1;
    return ob;
  }
};

TEST_F(PaintLayersTargetTest, get_resolves_active_row_and_absent_channel)
{
  MaterialPaintLayer *layer = add_active_row("L");

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  EXPECT_EQ(target.material, ma);
  EXPECT_EQ(target.layer, layer);
  EXPECT_EQ(target.channel, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  /* The row has no record yet, so there is no map -- but the row is the target. */
  EXPECT_EQ(target.channel_record, nullptr);
  EXPECT_FALSE(BKE_paint_layers_target_is_live(target));
  EXPECT_EQ(BKE_paint_layers_target_image(target), nullptr);

  /* The channel has to be one of the material's. */
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, -1, target));
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_NUM, target));
}

TEST_F(PaintLayersTargetTest, get_refuses_flat_material_and_no_active_row)
{
  Material *flat = BKE_material_add(bmain, "FlatMat");
  PaintLayersTarget target;
  EXPECT_FALSE(BKE_paint_layers_target_get(*flat, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A layered material without an active marker has no target. */
  add_active_row("L");
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A marker that names no row is no target either. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_generate_random());
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
}

TEST_F(PaintLayersTargetTest, disabled_channel_is_not_a_paint_target)
{
  MaterialPaintLayer *layer = add_active_row("L");
  Image *base_image = add_solid_image("Base");
  Image *rough_image = add_solid_image("Rough");
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_image));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_image));

  /* Switching Base Color off keeps the record and its map, but the channel is no longer a paint
   * target and a stroke cannot switch it back on. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));

  PaintLayersTarget base_target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_target));
  EXPECT_FALSE(BKE_paint_layers_target_is_live(base_target));
  EXPECT_EQ(BKE_paint_layers_target_image(base_target), nullptr);
  EXPECT_EQ(BKE_paint_layers_target_ensure_writable(*bmain, base_target, 4), nullptr);
  const MaterialPaintLayerChannel *base = paint_layer_channel_find(
      *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->image, base_image);
  EXPECT_EQ(base->state, MA_PAINT_LAYER_CHANNEL_DISABLED);

  /* A neighbouring live channel is unaffected. */
  PaintLayersTarget rough_target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_target));
  EXPECT_TRUE(BKE_paint_layers_target_is_live(rough_target));
  EXPECT_EQ(BKE_paint_layers_target_image(rough_target), rough_image);

  /* Enabling it back restores the target with the same map. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_target));
  EXPECT_EQ(BKE_paint_layers_target_image(base_target), base_image);
}

TEST_F(PaintLayersTargetTest, ensure_creates_channel_and_neutral_map)
{
  MaterialPaintLayer *layer = add_active_row("L");

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->alpha_mode, IMA_ALPHA_STRAIGHT);
  EXPECT_NE(image->flag & IMA_PAINT_CANVAS, 0);
  ASSERT_NE(target.channel_record, nullptr);
  EXPECT_EQ(target.channel_record->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(BKE_paint_layers_target_image(target), image);
  /* The record is the map's only user, and the row did not have the record before. */
  EXPECT_EQ(image->id.us, 1);
  EXPECT_EQ(layer->channels_num, 1);
  /* A scalar channel uses a data colorspace. */
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(image->colorspace_settings.name));
  /* Named after the row and the channel. */
  EXPECT_STREQ(image->id.name + 2, "L Roughness");

  /* A record the row did not have starts from the channel's neutral, the composite's own bottom
   * colour: opaque mid grey for Roughness. */
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[0], 128);
  EXPECT_EQ(pixel[3], 255);

  /* A second call returns the same map, minting nothing new. */
  EXPECT_EQ(BKE_paint_layers_target_ensure_writable(*bmain, target, 4), image);
  EXPECT_EQ(layer->channels_num, 1);
}

TEST_F(PaintLayersTargetTest, a_shared_map_is_copied_on_the_first_stroke)
{
  MaterialPaintLayer *layer = add_active_row("L");

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->id.us, 1);

  /* Another row sharing the map: the next stroke must not paint into its pixels. */
  id_us_plus(&image->id);
  ASSERT_EQ(image->id.us, 2);

  PaintLayersTarget shared_target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, shared_target));
  Image *owned = BKE_paint_layers_target_ensure_writable(*bmain, shared_target, 4);
  ASSERT_NE(owned, nullptr);
  EXPECT_NE(owned, image);
  ASSERT_NE(shared_target.channel_record, nullptr);
  EXPECT_EQ(shared_target.channel_record->image, owned);
  EXPECT_EQ(image->id.us, 1);
  EXPECT_EQ(owned->id.us, 1);
  EXPECT_EQ(layer->channels_num, 1);
}

TEST_F(PaintLayersTargetTest, a_correction_is_a_paint_target_with_a_transparent_first_map)
{
  MaterialPaintLayer *row = add_active_row("L");
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  BKE_paint_layers_active_set(*ma, effect->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  EXPECT_EQ(target.layer, effect);
  ASSERT_NE(target.channel_record, nullptr);

  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(target.channel_record->image, image);
  EXPECT_STREQ(image->id.name + 2, "Corr Base Color");
  /* The correction lays nothing until painted: its map starts transparent, not as the opaque
   * neutral a fresh layer's channel takes. */
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[3], 0);
  /* The row it hangs on got no map of its own. */
  EXPECT_EQ(paint_layer_channel_find(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  PaintLayersImageUse use;
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *image, use));
  EXPECT_EQ(use.layer, effect);
}

TEST_F(PaintLayersTargetTest, a_fill_correction_gets_a_map_that_keeps_its_look)
{
  MaterialPaintLayer *row = add_active_row("L");
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(effect, nullptr);
  /* A translucent colour: a Fill covers by its opacity alone, so its map starts opaque. */
  const float color[4] = {0.2f, 0.4f, 0.6f, 0.3f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, color));
  BKE_paint_layers_active_set(*ma, effect->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  EXPECT_EQ(target.layer, effect);
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);

  /* Still a Fill: the channels it has no map in keep their colour. The map is what it lays now. */
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(target.channel_record->image, image);
  EXPECT_TRUE(paint_layer_effect_lays(*ma, *effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[3], 255);
  /* The colour it laid, not black: the map keeps the look. */
  EXPECT_GT(pixel[2], pixel[0]);
  EXPECT_GT(pixel[0], 0);
  /* The row it hangs on got no map of its own. */
  EXPECT_EQ(paint_layer_channel_find(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
}

TEST_F(PaintLayersTargetTest, a_fill_correction_normal_map_starts_transparent_and_flat)
{
  MaterialPaintLayer *row = add_active_row("L");
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(effect, nullptr);
  const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, color));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  BKE_paint_layers_active_set(*ma, effect->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);

  /* A Fill laid nothing in Normal, so the map starts as a correction's does: no coverage, and the
   * flat tangent a bilinear edge interpolates to nothing. */
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[3], 0);
  EXPECT_NEAR(pixel[0], 128, 1);
  EXPECT_NEAR(pixel[1], 128, 1);
  EXPECT_EQ(pixel[2], 255);
}

TEST_F(PaintLayersTargetTest, removing_a_correction_lets_go_of_its_map_and_the_active_marker)
{
  MaterialPaintLayer *row = add_active_row("L");
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  BKE_paint_layers_active_set(*ma, effect->marker);
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  /* The record is the map's only user (see the first-stroke tests above). */
  EXPECT_EQ(image->id.us, 1);

  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  EXPECT_EQ(image->id.us, 0);
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(*ma)));
  EXPECT_TRUE(BLI_listbase_is_empty(&row->effects));
}

TEST_F(PaintLayersTargetTest, ensure_normal_neutral_is_flat)
{
  add_active_row("L");

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 2);
  ASSERT_NE(image, nullptr);

  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[0], 128);
  EXPECT_EQ(pixel[1], 128);
  EXPECT_EQ(pixel[2], 255);
}

TEST_F(PaintLayersTargetTest, ensure_keeps_the_look_of_the_records_flat_value)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(record, nullptr);
  const float value[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));

  /* The first map of a record that already carries a value paints that value, so the row looks the
   * same before and after the first stroke. */
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 2);
  ASSERT_NE(image, nullptr);
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[0], 128);
  EXPECT_EQ(pixel[1], 128);
  EXPECT_EQ(pixel[2], 128);
  EXPECT_EQ(pixel[3], 255);
}

TEST_F(PaintLayersTargetTest, ensure_encodes_a_color_value_into_the_maps_colorspace)
{
  MaterialPaintLayer *layer = add_active_row("L");
  const float linear[4] = {0.25f, 0.5f, 0.75f, 1.0f};

  /* A channel the row does not have yet takes the caller's colour, not the channel's neutral. */
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 2, linear);
  ASSERT_NE(image, nullptr);
  ASSERT_NE(target.channel_record, nullptr);
  EXPECT_FLOAT_EQ(target.channel_record->value[0], linear[0]);
  EXPECT_FALSE(IMB_colormanagement_space_name_is_data(image->colorspace_settings.name));

  /* The constant is scene linear; the byte buffer stores the colour in the map's colorspace. */
  float expected[4];
  copy_v4_v4(expected, linear);
  const ColorSpace *colorspace = IMB_colormanagement_space_get_named(
      image->colorspace_settings.name);
  ASSERT_NE(colorspace, nullptr);
  IMB_colormanagement_scene_linear_to_colorspace(expected, 1, 1, 4, colorspace);
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_NEAR(float(pixel[0]) / 255.0f, expected[0], 1.0f / 255.0f);
  EXPECT_NEAR(float(pixel[1]) / 255.0f, expected[1], 1.0f / 255.0f);
  EXPECT_NEAR(float(pixel[2]) / 255.0f, expected[2], 1.0f / 255.0f);
  EXPECT_EQ(layer->channels_num, 1);
}

/**
 * A non-neutral Normal value written into the channel map's own ImBuf is read back from the map and
 * reaches the CPU composite: the map is the committed store of the row, not a GPU-only preview.
 */
TEST_F(PaintLayersTargetTest, normal_value_written_to_map_reaches_the_cpu_composite)
{
  const int size = 4;
  add_active_row("L");

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, size);
  ASSERT_NE(image, nullptr);

  /* A tilted tangent (0.5, 0.5, 0.5) packed for byte storage is (0.75, 0.75, 0.75): non-neutral,
   * so it differs from the flat (128, 128, 255) the map starts from. */
  const uchar packed = uchar(0.75f * 255.0f + 0.5f);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      pixels[i * 4 + 0] = packed;
      pixels[i * 4 + 1] = packed;
      pixels[i * 4 + 2] = packed;
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(image, ibuf, lock);
  }

  /* The CPU composite reads the same map and yields a non-flat normal. */
  float rgba[4];
  composite_pixel(PAINT_MATERIAL_CHANNEL_NORMAL, size, rgba);
  EXPECT_NEAR(rgba[2], 0.75f, 0.05f);
}

/** The active layered material is the active object's active slot, and only when it is layered. */
TEST_F(PaintLayersTargetTest, active_material_follows_the_active_slot)
{
  MaterialPaintLayer *row = add_active_row("Layer");
  Material *plain = BKE_material_add(bmain, "Plain");
  Object *ob = add_object_with({ma, plain});

  /* The active slot is the plain material: no layered material, even though one exists elsewhere. */
  ob->actcol = 2;
  EXPECT_EQ(BKE_paint_layers_active_material_get(ob), nullptr);
  PaintLayersTarget target;
  EXPECT_FALSE(BKE_paint_layers_target_get(*ob, -1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  ob->actcol = 1;
  EXPECT_EQ(BKE_paint_layers_active_material_get(ob), ma);
  EXPECT_EQ(BKE_paint_layers_active_layer_get(*ma), row);
  ASSERT_TRUE(BKE_paint_layers_target_get(*ob, -1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  EXPECT_EQ(target.layer, row);

  /* An explicit slot resolves its own material, whatever slot is active: the slot is 0-based. */
  ob->actcol = 2;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ob, 0, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  EXPECT_EQ(target.material, ma);
  EXPECT_FALSE(BKE_paint_layers_target_get(*ob, 1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  ob->actcol = 1;

  /* No active row marker: the layer answer is null, the material answer is not. */
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_EQ(BKE_paint_layers_active_layer_get(*ma), nullptr);
  EXPECT_EQ(BKE_paint_layers_active_material_get(ob), ma);

  EXPECT_EQ(BKE_paint_layers_active_material_get(nullptr), nullptr);
}

TEST_F(PaintLayersTargetTest, find_image_use_reads_the_description)
{
  Image *content = add_solid_image("Content");
  MaterialPaintLayer *layer = add_paint_layer("L", content);
  Image *foreign = add_solid_image("Foreign");

  PaintLayersImageUse use;
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *content, use));
  EXPECT_EQ(use.layer, layer);
  EXPECT_EQ(use.channel, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  /* An image no row carries. */
  EXPECT_FALSE(BKE_paint_layers_find_image_use(*ma, *foreign, use));

  /* A map shared by two materials resolves to the row of the material that was searched. */
  Material *other = BKE_material_add(bmain, "Other");
  MaterialPaintLayer *other_layer = BKE_paint_layers_add(
      *other, "O", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*other, other_layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *other, other_layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, content));

  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *content, use));
  EXPECT_EQ(use.layer, layer);
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*other, *content, use));
  EXPECT_EQ(use.layer, other_layer);
  EXPECT_EQ(use.channel, int(PAINT_MATERIAL_CHANNEL_ROUGHNESS));
}

/* -------------------------------------------------------------------- */
/** \name Mask target
 * \{ */

TEST_F(PaintLayersTargetTest, mask_target_resolves_the_base_mask_by_base_color_alone)
{
  MaterialPaintLayer *layer = add_active_row("L");

  /* A row without a mask has nothing to paint in Mask mode; Content is unaffected. */
  PaintLayersTarget target;
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));

  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.6f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.mode, PaintLayersTargetMode::Mask);
  EXPECT_EQ(target.layer, layer);
  EXPECT_EQ(target.mask_item, item);
  EXPECT_EQ(target.channel_record,
            paint_layer_channel_find(*item, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(BKE_paint_layers_target_is_live(target));
  /* No map yet: the first stroke makes it. */
  EXPECT_EQ(BKE_paint_layers_target_image(target), nullptr);

  /* A mask is one grey map: no other channel names it. */
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, PaintLayersTargetMode::Mask, target));
}

TEST_F(PaintLayersTargetTest, mask_target_follows_the_active_correction)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(base, nullptr);
  MaterialPaintLayer *paint = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE, "Corr");
  MaterialPaintLayer *fill = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT, "Flat", 0.6f);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(fill, nullptr);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.mask_item, base);

  /* The row is still the layer; the correction is the mask item the stroke paints. */
  BKE_paint_layers_active_set(*ma, paint->marker);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.layer, layer);
  EXPECT_EQ(target.mask_item, paint);
  EXPECT_TRUE(BKE_paint_layers_target_is_live(target));
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
  EXPECT_EQ(target.layer, layer);
  EXPECT_EQ(target.mask_item, nullptr);

  /* A Paint correction's first map is transparent, so it changes no pixel until stroked. */
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  EXPECT_STREQ(image->id.name + 2, "L Corr");
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[3], 0);
  EXPECT_EQ(paint_layer_channel_find(*paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR)->image, image);
  EXPECT_EQ(paint_layer_channel_find(*base, PAINT_MATERIAL_CHANNEL_BASE_COLOR)->image, nullptr);

  /* A Fill's first map keeps its grey, opaque. */
  BKE_paint_layers_active_set(*ma, fill->marker);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  Image *flat = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(flat, nullptr);
  first_byte_pixel(*flat, pixel);
  EXPECT_EQ(pixel[3], 255);
  EXPECT_NEAR(pixel[0], 153, 1);

  /* The row itself addresses the base again. */
  BKE_paint_layers_active_set(*ma, layer->marker);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.mask_item, base);

  PaintLayersImageUse use;
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *image, use));
  EXPECT_EQ(use.layer, paint);
  EXPECT_TRUE(use.is_mask);
}

TEST_F(PaintLayersTargetTest, mask_ensure_writable_fills_the_map_with_the_constant)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.6f);
  ASSERT_NE(item, nullptr);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);

  /* The item holds the map, and the map is its only user: removing the mask lets it go. */
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->image, image);
  EXPECT_EQ(image->id.us, 1);
  EXPECT_EQ(BKE_paint_layers_target_image(target), image);
  EXPECT_STREQ(image->id.name + 2, "L Mask");

  /* One convention for every paint-layer map: straight bytes. A mask is data, not a colour. */
  EXPECT_EQ(image->alpha_mode, IMA_ALPHA_STRAIGHT);
  EXPECT_NE(image->flag & IMA_PAINT_CANVAS, 0);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(image->colorspace_settings.name));

  /* The map keeps the look of the constant it replaces: opaque grey 0.6. */
  uchar pixel[4];
  first_byte_pixel(*image, pixel);
  EXPECT_EQ(pixel[0], 153);
  EXPECT_EQ(pixel[3], 255);

  /* A second call hands back the same map. */
  EXPECT_EQ(BKE_paint_layers_target_ensure_writable(*bmain, target, 4), image);

  /* The map is the mask's: the row's content channels are untouched. */
  EXPECT_EQ(layer->channels_num, 0);
}

TEST_F(PaintLayersTargetTest, mask_off_is_refused_not_switched_back_on)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.6f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, item, false));

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_FALSE(BKE_paint_layers_target_is_live(target));
  EXPECT_EQ(BKE_paint_layers_target_ensure_writable(*bmain, target, 4), nullptr);
  EXPECT_EQ(item->flag & MA_PAINT_LAYER_ENABLED, 0);

  /* A switched-off record is refused too, like any other channel's. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, item, true));
  ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(BKE_paint_layers_target_ensure_writable(*bmain, target, 4), nullptr);
}

TEST_F(PaintLayersTargetTest, find_image_use_reports_a_mask_map_as_the_mask_item)
{
  Image *content = add_solid_image("Content");
  MaterialPaintLayer *layer = add_paint_layer("L", content);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);
  Image *map = add_solid_image("MaskMap");
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  PaintLayersImageUse use;
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *map, use));
  EXPECT_EQ(use.layer, item);
  EXPECT_TRUE(use.is_mask);
  EXPECT_EQ(use.channel, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *content, use));
  EXPECT_EQ(use.layer, layer);
  EXPECT_FALSE(use.is_mask);
}

TEST_F(PaintLayersTargetTest, stroke_funnel_answers_the_mask_map_in_mask_mode)
{
  Image *content = add_solid_image("Content");
  MaterialPaintLayer *layer = add_paint_layer("L", content);
  Image *rough = add_solid_image("Rough");
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough));
  /* Normal is the channel whose Content path differs most from a scalar's, so it is checked too. */
  Image *normal = add_solid_image("Normal");
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL, normal));
  BKE_paint_layers_active_set(*ma, layer->marker);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);
  Image *map = add_solid_image("MaskMap");
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  Object *ob = add_object_with({ma});

  Image *image = nullptr;
  ImageUser *iuser = nullptr;
  PaintModeSettings settings;

  /* Content: the row's own maps. */
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &image, &iuser, &settings));
  EXPECT_EQ(image, content);
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_ROUGHNESS, &image, &iuser, &settings));
  EXPECT_EQ(image, rough);
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_NORMAL, &image, &iuser, &settings));
  EXPECT_EQ(image, normal);

  /* Mask: Base Color is the mask's map, and the other channels have nothing to write into. */
  settings.layer_target_mode = PAINT_LAYER_TARGET_MASK;
  ASSERT_TRUE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &image, &iuser, &settings));
  EXPECT_EQ(image, map);
  EXPECT_FALSE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_ROUGHNESS, &image, &iuser, &settings));
  EXPECT_EQ(image, nullptr);
  EXPECT_FALSE(BKE_paint_principled_channel_image_get(
      *ob, PAINT_MATERIAL_CHANNEL_NORMAL, &image, &iuser, &settings));
  EXPECT_EQ(image, nullptr);
}

TEST_F(PaintLayersTargetTest, mask_mode_gives_the_active_slot_back_on_leaving)
{
  MaterialPaintLayer *layer = add_active_row("L");
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 1.0f), nullptr);
  Object *ob = add_object_with({ma});
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "SlotScene"));
  ASSERT_NE(scene, nullptr);
  PaintModeSettings &settings = scene->toolsettings->paint_mode;

  /* The user's choice: the Roughness slot (by its channel, whatever its index is). */
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  int roughness_slot = -1;
  for (const int slot : IndexRange(ma->tot_slots)) {
    if (BKE_paint_layers_texpaint_slot_channel(slot) == PAINT_MATERIAL_CHANNEL_ROUGHNESS) {
      roughness_slot = slot;
    }
  }
  ASSERT_NE(roughness_slot, -1);
  ma->paint_active_slot = short(roughness_slot);

  /* Mask: the slot moves to Base Color, which holds the mask. */
  settings.layer_target_mode = PAINT_LAYER_TARGET_MASK;
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_EQ(BKE_paint_layers_texpaint_slot_channel(ma->paint_active_slot),
            int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  /* A second refresh in Mask mode must not take the override for the user's choice. */
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_EQ(BKE_paint_layers_texpaint_slot_channel(ma->paint_active_slot),
            int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  /* Content: the choice comes back, and nothing is held any more. */
  settings.layer_target_mode = PAINT_LAYER_TARGET_CONTENT;
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_EQ(ma->paint_active_slot, roughness_slot);
  EXPECT_EQ(ma->paint_content_slot, 0);
}

TEST_F(PaintLayersTargetTest, hidden_row_keeps_its_texpaint_slots)
{
  MaterialPaintLayer *layer = add_active_row("L");
  Object *ob = add_object_with({ma});
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "HiddenSlotScene"));
  ASSERT_NE(scene, nullptr);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  Image *map = BKE_paint_layers_target_ensure_writable(*bmain, target, 8);
  ASSERT_NE(map, nullptr);

  const auto base_color_slot = [&]() {
    for (const int slot : IndexRange(ma->tot_slots)) {
      if (BKE_paint_layers_texpaint_slot_channel(slot) == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
        return slot;
      }
    }
    return -1;
  };

  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  const int slot = base_color_slot();
  ASSERT_NE(slot, -1);
  ASSERT_TRUE(ma->texpaintslot[slot].valid);
  ASSERT_EQ(ma->texpaintslot[slot].ima, map);

  /* Hiding a row stops strokes but does not empty its maps: the slot cache outlives a visibility
   * toggle, so showing the row again needs no refresh. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_EQ(base_color_slot(), slot);
  EXPECT_TRUE(ma->texpaintslot[slot].valid);
  EXPECT_EQ(ma->texpaintslot[slot].ima, map);
}

TEST_F(PaintLayersTargetTest, mask_mode_does_not_touch_the_content_maps_of_the_row)
{
  MaterialPaintLayer *layer = add_active_row("L");
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 0.5f), nullptr);
  Object *ob = add_object_with({ma});

  /* Entering Mask mode and stroking creates the mask's map and nothing else. */
  PaintModeSettings settings;
  settings.layer_target_mode = PAINT_LAYER_TARGET_MASK;
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ob, -1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, BKE_paint_layers_target_mode(&settings), target));
  ASSERT_NE(BKE_paint_layers_target_ensure_writable(*bmain, target, 4), nullptr);
  EXPECT_EQ(layer->channels_num, 0);

  /* Back in Content the row still has no map of its own. */
  ASSERT_TRUE(BKE_paint_layers_target_get(*ob, -1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  EXPECT_EQ(BKE_paint_layers_target_image(target), nullptr);
}

TEST_F(PaintLayersTargetTest, the_materials_image_size_wins_over_the_callers)
{
  add_active_row("L");

  /* A material with a size of its own overrides the fallback the caller passes. */
  ma->paint_layers_image_size = 8;
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, target));
  Image *sized = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(sized, nullptr);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(sized, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, 8);
  BKE_image_release_ibuf(sized, ibuf, lock);

  /* Zero is what older files carry: the caller's size is used. */
  ma->paint_layers_image_size = 0;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, target));
  Image *fallback = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(fallback, nullptr);
  ibuf = BKE_image_acquire_ibuf(fallback, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, 4);
  BKE_image_release_ibuf(fallback, ibuf, lock);
}

TEST_F(PaintLayersTargetTest, active_maps_ensure_covers_the_channel_set_and_spares_switched_off)
{
  MaterialPaintLayer *layer = add_active_row("L");
  const uint16_t set = (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)) |
                       (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)) |
                       (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_METALLIC));
  BKE_paint_layers_channel_set_mask_set(*ma, set);

  /* Metallic has a record the user switched off: it must stay off and mapless. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, layer, PAINT_MATERIAL_CHANNEL_METALLIC, false));

  /* Base Color and Roughness have no record at all; the call adds each with its map. */
  EXPECT_EQ(BKE_paint_layers_active_maps_ensure(*bmain, *ma, 4), 2);
  for (const int channel : {int(PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                            int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)})
  {
    PaintLayersTarget target;
    ASSERT_TRUE(BKE_paint_layers_target_get(*ma, channel, target));
    EXPECT_NE(BKE_paint_layers_target_image(target), nullptr);
  }
  PaintLayersTarget metallic;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, metallic));
  EXPECT_EQ(BKE_paint_layers_target_image(metallic), nullptr);
  EXPECT_FALSE(BKE_paint_layers_target_is_live(metallic));

  /* A channel outside the set is not a valid paint target. */
  PaintLayersTarget normal;
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, normal));

  /* Idempotent: nothing is left to create. */
  EXPECT_EQ(BKE_paint_layers_active_maps_ensure(*bmain, *ma, 4), 0);
}

TEST_F(PaintLayersTargetTest, resolve_reports_why_a_target_is_refused)
{
  using Status = PaintLayersTargetStatus;
  PaintLayersTarget target;
  constexpr PaintLayersTargetMode content = PaintLayersTargetMode::Content;
  constexpr PaintLayersTargetMode mask = PaintLayersTargetMode::Mask;

  /* No active row yet. */
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, content, target),
            Status::NoActiveRow);

  MaterialPaintLayer *layer = add_active_row("L");
  ASSERT_NE(layer, nullptr);
  BKE_paint_layers_channel_set_mask_set(
      *ma,
      (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)) |
          (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_METALLIC)));

  /* Channel indices outside the table. */
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, -1, content, target), Status::InvalidChannel);
  EXPECT_EQ(
      BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_NUM, content, target),
      Status::InvalidChannel);

  /* Ok means "resolvable", not "has a record": the record appears on the first stroke. */
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, content, target),
            Status::Ok);
  EXPECT_EQ(target.channel, int(PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  /* A channel the set leaves out concerns that channel alone. */
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, content, target),
            Status::OutsideChannelSet);
  EXPECT_EQ(
      BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, content, target),
      Status::OutsideChannelSet);

  /* Mask: a row without a mask, then a mask that only Base Color names, whatever the set says. */
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask, target),
            Status::NoMask);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 1.0f), nullptr);
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask, target),
            Status::Ok);
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, mask, target),
            Status::ChannelNotInMode);

  /* The bool overload is the same answer. */
  EXPECT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, content, target));
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, content, target));

  /* An adjustment owns no map a stroke could write into, whatever the set says. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Adjustment;
  params.filter = MA_PAINT_LAYER_FILTER_INVERT;
  MaterialPaintLayer *adjustment = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(adjustment, nullptr);
  BKE_paint_layers_active_set(*ma, adjustment->marker);
  EXPECT_EQ(BKE_paint_layers_target_resolve(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, content, target),
            Status::RowTakesNoStrokes);
}

TEST_F(PaintLayersTargetTest, resolve_through_an_object_applies_the_object_mask_only_on_ok)
{
  add_active_row("L");
  BKE_paint_layers_channel_set_mask_set(*ma,
                                        uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  Object *ob = add_object_with({ma});

  PaintLayersTarget target;
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ob, -1, PAINT_MATERIAL_CHANNEL_ROUGHNESS, PaintLayersTargetMode::Content, target),
            PaintLayersTargetStatus::Ok);
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ob, -1, PAINT_MATERIAL_CHANNEL_NORMAL, PaintLayersTargetMode::Content, target),
            PaintLayersTargetStatus::OutsideChannelSet);

  /* An object without the layered material has nothing to resolve. */
  Object *plain = add_object_with({});
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *plain, -1, PAINT_MATERIAL_CHANNEL_ROUGHNESS, PaintLayersTargetMode::Content, target),
            PaintLayersTargetStatus::NotLayered);
}

TEST_F(PaintLayersTargetTest, active_slot_leaves_a_channel_outside_the_set)
{
  add_active_row("L");
  /* No Base Color: the first slot, where the active slot starts, is not a target. */
  BKE_paint_layers_channel_set_mask_set(
      *ma,
      (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)) |
          (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_METALLIC)));
  Object *ob = add_object_with({ma});
  Scene *scene = static_cast<Scene *>(BKE_id_new(bmain, ID_SCE, "NoBaseSlotScene"));
  ASSERT_NE(scene, nullptr);

  const auto active_channel = [&]() {
    return BKE_paint_layers_texpaint_slot_channel(ma->paint_active_slot);
  };
  const auto slot_of = [&](const eMaterialPaintChannel channel) {
    for (const int slot : IndexRange(ma->tot_slots)) {
      if (BKE_paint_layers_texpaint_slot_channel(slot) == int(channel)) {
        return slot;
      }
    }
    return -1;
  };

  ma->paint_active_slot = 0;
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, eMaterialPaintChannel(active_channel())));

  /* A slot the user picked inside the set is kept. */
  const int metallic_slot = slot_of(PAINT_MATERIAL_CHANNEL_METALLIC);
  ASSERT_NE(metallic_slot, -1);
  ma->paint_active_slot = short(metallic_slot);
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_EQ(ma->paint_active_slot, metallic_slot);

  /* One outside the set is not a target any more and moves to a channel of the set. */
  const int normal_slot = slot_of(PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_NE(normal_slot, -1);
  ma->paint_active_slot = short(normal_slot);
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, eMaterialPaintChannel(active_channel())));

  /* Mask mode still uses Base Color's slot although the set leaves it out, and leaving it gives a
   * channel of the set back, not the held Base Color slot. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, BKE_paint_layers_active_layer_get(*ma), 1.0f), nullptr);
  scene->toolsettings->paint_mode.layer_target_mode = PAINT_LAYER_TARGET_MASK;
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_EQ(active_channel(), int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  scene->toolsettings->paint_mode.layer_target_mode = PAINT_LAYER_TARGET_CONTENT;
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, eMaterialPaintChannel(active_channel())));
  EXPECT_EQ(ma->paint_content_slot, 0);
}

/* -------------------------------------------------------------------- */
/** \name Painting lock
 * \{ */

TEST_F(PaintLayersTargetTest, lock_refuses_the_target_of_the_row)
{
  MaterialPaintLayer *layer = add_active_row("L");

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A locked row resolves no target, and unlocking gives it back. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, layer, true));
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *layer), layer);
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target),
            PaintLayersTargetStatus::Locked);
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, layer, false));
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *layer), nullptr);
  EXPECT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
}

TEST_F(PaintLayersTargetTest, lock_of_a_folder_or_a_mask_item_protects_what_it_holds)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "F", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "C", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  BKE_paint_layers_active_set(*ma, child->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A locked folder protects its whole subtree, and says so as the blocker. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, folder, true));
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *child), folder);
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, folder, false));
  EXPECT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A locked mask correction keeps its pixels; the row's own content stays paint-able. */
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, child, 1.0f);
  MaterialPaintLayer *paint = BKE_paint_layers_mask_item_add(
      *ma, child, MA_PAINT_LAYER_SOURCE_IMAGE, "Corr");
  ASSERT_NE(base, nullptr);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_active_set(*ma, paint->marker);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.mask_item, paint);

  /* The row a mask item hangs on locks its mask too. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, child, true));
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *paint), child);
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, child, false));

  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, paint, true));
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *paint), paint);
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target),
            PaintLayersTargetStatus::Locked);
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  BKE_paint_layers_active_set(*ma, child->marker);
  EXPECT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, paint, false));
  BKE_paint_layers_active_set(*ma, paint->marker);
  EXPECT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Painting visibility
 * \{ */

TEST_F(PaintLayersTargetTest, hidden_refuses_the_target_of_the_row)
{
  MaterialPaintLayer *layer = add_active_row("L");

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A switched-off row resolves no target, and showing it gives the target back. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *layer), layer);
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target),
            PaintLayersTargetStatus::Hidden);
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, true));
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *layer), nullptr);
  EXPECT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
}

TEST_F(PaintLayersTargetTest, hidden_folder_or_owner_mutes_what_it_holds)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "F", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "C", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  BKE_paint_layers_active_set(*ma, child->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A hidden folder mutes its whole subtree, and says so as the blocker. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *child), folder);
  EXPECT_FALSE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, true));
  EXPECT_TRUE(BKE_paint_layers_target_get(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, target));

  /* A hidden row mutes the mask corrections it holds as well. */
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, child, 1.0f);
  MaterialPaintLayer *paint = BKE_paint_layers_mask_item_add(
      *ma, child, MA_PAINT_LAYER_SOURCE_IMAGE, "Corr");
  ASSERT_NE(base, nullptr);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_active_set(*ma, paint->marker);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.mask_item, paint);

  /* The row a mask item hangs on hides its mask too. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, child, false));
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *paint), child);
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, child, true));

  /* The mask item's own switch is not a hidden refusal, unlike the row's: the item stays the
   * target and only its map stops being writable, the soft refusal
   * #BKE_paint_layers_target_is_live answers (see mask_off_is_refused_not_switched_back_on). */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, paint, false));
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *paint), paint);
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target),
            PaintLayersTargetStatus::Ok);
  EXPECT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_FALSE(BKE_paint_layers_target_is_live(target));
}

TEST_F(PaintLayersTargetTest, lock_blocker_reason_names_where_the_lock_sits)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "F", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "C", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  BKE_paint_layers_active_set(*ma, child->marker);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(*ma, child, "Corr");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, child, 1.0f);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, child, MA_PAINT_LAYER_SOURCE_IMAGE, "Item");
  ASSERT_NE(base, nullptr);
  ASSERT_NE(item, nullptr);

  PaintLayersBlockerPlace reason = PaintLayersBlockerPlace::Folder;

  /* The row itself. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, child, true));
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *child, &reason), child);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Self);

  /* A correction and a mask item hang on the row, so its lock sits at Owner. */
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *correction, &reason), child);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Owner);
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *item, &reason), child);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Owner);
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, child, false));

  /* A lock over the chain sits at Folder. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, folder, true));
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *child, &reason), folder);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Folder);
  EXPECT_EQ(BKE_paint_layers_lock_blocker(*ma, *correction, &reason), folder);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Folder);
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, folder, false));
}

TEST_F(PaintLayersTargetTest, hidden_blocker_reason_names_where_the_switch_sits)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "F", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "C", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  BKE_paint_layers_active_set(*ma, child->marker);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(*ma, child, "Corr");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, child, 1.0f);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, child, MA_PAINT_LAYER_SOURCE_IMAGE, "Item");
  ASSERT_NE(base, nullptr);
  ASSERT_NE(item, nullptr);

  PaintLayersBlockerPlace reason = PaintLayersBlockerPlace::Folder;

  /* The row itself, and the correction that hangs on it. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, child, false));
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *child, &reason), child);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Self);
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *correction, &reason), child);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Owner);
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *item, &reason), child);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Owner);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, child, true));

  /* A switched-off folder mutes the chain below it. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *child, &reason), folder);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Folder);
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *correction, &reason), folder);
  EXPECT_EQ(reason, PaintLayersBlockerPlace::Folder);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, true));
}

TEST_F(PaintLayersTargetTest, locked_wins_over_hidden)
{
  MaterialPaintLayer *layer = add_active_row("L");
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, layer, true));

  /* Both refuse the target, but the lock is reported: it comes first in the resolve. */
  EXPECT_EQ(BKE_paint_layers_hidden_blocker(*ma, *layer), layer);
  PaintLayersTarget target;
  EXPECT_EQ(BKE_paint_layers_target_resolve(
                *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target),
            PaintLayersTargetStatus::Locked);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask stack merge
 * \{ */

TEST_F(PaintLayersTargetTest, merge_folds_the_stack_into_one_base_map)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, layer, 0.5f);
  MaterialPaintLayer *flat = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT, "Flat", 0.25f);
  ASSERT_NE(base, nullptr);
  ASSERT_NE(flat, nullptr);

  Image *merged = BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr);
  ASSERT_NE(merged, nullptr);

  /* The base alone is left, reading the merged map with the settings a fresh base mask has. */
  EXPECT_EQ(layer->mask_stack.first, layer->mask_stack.last);
  EXPECT_EQ(BKE_paint_layers_mask_base(*layer), base);
  EXPECT_EQ(paint_layer_channel_find(*base, PAINT_MATERIAL_CHANNEL_BASE_COLOR)->image, merged);
  EXPECT_EQ(base->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_FLOAT_EQ(base->opacity, 1.0f);
  EXPECT_FALSE(BKE_paint_layers_mapping_applies(*ma, *base));

  /* The base's 0.5 laid over by the Mix constant 0.25 is 0.25, the grey every texel holds. */
  uchar pixel[4];
  first_byte_pixel(*merged, pixel);
  EXPECT_EQ(pixel[0], 64);
  EXPECT_EQ(pixel[3], 255);
}

TEST_F(PaintLayersTargetTest, mask_base_map_ensure_makes_the_base_map_once)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, layer, 0.25f);
  ASSERT_NE(base, nullptr);
  ASSERT_EQ(paint_layer_channel_find(*base, PAINT_MATERIAL_CHANNEL_BASE_COLOR)->image, nullptr);

  Image *map = BKE_paint_layers_mask_base_map_ensure(*bmain, *ma, *layer, 8);
  ASSERT_NE(map, nullptr);
  EXPECT_EQ(paint_layer_channel_find(*base, PAINT_MATERIAL_CHANNEL_BASE_COLOR)->image, map);
  EXPECT_TRUE(BKE_paint_layers_image_is_internal(*map))
      << "the map the stack made is reached through the layer UI, like the mask it belongs to";

  /* The map starts as the grey the constant laid, so the row looks the same before and after. */
  uchar pixel[4];
  first_byte_pixel(*map, pixel);
  EXPECT_EQ(pixel[0], 64);
  EXPECT_EQ(pixel[3], 255);

  /* Made once: a second call answers the map that is already there. */
  EXPECT_EQ(BKE_paint_layers_mask_base_map_ensure(*bmain, *ma, *layer, 8), map);

  /* A row without a mask has no base mask, so no map either. */
  MaterialPaintLayer *bare = add_active_row("Bare");
  EXPECT_EQ(BKE_paint_layers_mask_base_map_ensure(*bmain, *ma, *bare, 8), nullptr);
}

TEST_F(PaintLayersTargetTest, merge_reads_back_what_the_sampler_read)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  MaterialPaintLayer *corr = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE, "Corr");
  ASSERT_NE(base, nullptr);
  ASSERT_NE(corr, nullptr);
  Image *base_map = add_gray_mask("BaseMap", 8, 128);
  Image *corr_map = add_gray_mask("CorrMap", 8, 64);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_map));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, corr_map));

  /* What a stroke saw before the merge: the base's grey laid over by the correction's Mix grey. */
  PaintLayersMaskSampler before(*ma, *layer);
  before.tile_acquire(1001, 8, 8);
  const float factor_before = before.sample(1001, 4, 4);

  Image *merged = BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr);
  ASSERT_NE(merged, nullptr);

  /* The base alone answers the same, to the byte the merged map was quantized to. */
  PaintLayersMaskSampler after(*ma, *layer);
  after.tile_acquire(1001, 8, 8);
  const float factor_after = after.sample(1001, 4, 4);
  EXPECT_NEAR(factor_after, factor_before, 1.0f / 255.0f + 1e-6);
}

TEST_F(PaintLayersTargetTest, merge_refuses_locks_sizes_and_a_stack_without_corrections)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(base, nullptr);
  /* The base alone has nothing to fold. */
  EXPECT_EQ(BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr), nullptr);

  MaterialPaintLayer *corr = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE, "Corr");
  ASSERT_NE(corr, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_gray_mask("BaseMap", 8, 128)));

  /* A locked row or a locked correction refuses the merge and leaves the stack whole. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, layer, true));
  EXPECT_EQ(BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, layer, false));
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, corr, true));
  EXPECT_EQ(BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, corr, false));

  /* A map read at its own size has to agree with the others, or the merge would drop it. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_gray_mask("CorrMap", 16, 64)));
  EXPECT_EQ(BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr), nullptr);
  EXPECT_NE(layer->mask_stack.first, layer->mask_stack.last);

  /* The refusal left the maps in place, so the agreeing merge still runs. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_gray_mask("CorrMap8", 8, 64)));
  EXPECT_NE(BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr), nullptr);
  EXPECT_EQ(layer->mask_stack.first, layer->mask_stack.last);
}

TEST_F(PaintLayersTargetTest, merge_refuses_a_folder_lock_above_the_row)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "F", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "C", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  BKE_paint_layers_active_set(*ma, child->marker);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, child, 1.0f), nullptr);
  ASSERT_NE(BKE_paint_layers_mask_item_add(
                *ma, child, MA_PAINT_LAYER_SOURCE_CONSTANT, "Flat", 0.5f),
            nullptr);

  /* A locked folder protects the whole subtree, the merge the same as a stroke. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, folder, true));
  EXPECT_EQ(BKE_paint_layers_mask_stack_merge(*bmain, *ma, *child, nullptr), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, folder, false));
  EXPECT_NE(BKE_paint_layers_mask_stack_merge(*bmain, *ma, *child, nullptr), nullptr);
}

TEST_F(PaintLayersTargetTest, merge_covers_every_udim_tile_of_the_stack)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  MaterialPaintLayer *corr = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE, "Corr");
  ASSERT_NE(base, nullptr);
  ASSERT_NE(corr, nullptr);

  /* A tiled correction map: tile 1001 grey 32, tile 1010 grey 224; the base stays a constant. */
  Image *corr_map = add_gray_mask("CorrMap", 8, 32, /*tiled=*/true);
  ASSERT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*corr_map, 1010));
  fill_tile_gray(*corr_map, 1010, 224);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, corr_map));

  Image *merged = BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr);
  ASSERT_NE(merged, nullptr);
  EXPECT_EQ(merged->source, IMA_SRC_TILED);

  /* Each tile holds the grey the correction's own tile laid over the base's constant one. */
  uchar pixel[4];
  tile_byte_pixel(*merged, 1001, pixel);
  EXPECT_EQ(pixel[0], 32);
  tile_byte_pixel(*merged, 1010, pixel);
  EXPECT_EQ(pixel[0], 224);
}

TEST_F(PaintLayersTargetTest, merge_lets_go_of_the_old_maps)
{
  MaterialPaintLayer *layer = add_active_row("L");
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  MaterialPaintLayer *corr = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE, "Corr");
  ASSERT_NE(base, nullptr);
  ASSERT_NE(corr, nullptr);
  Image *base_map = add_gray_mask("BaseMap", 8, 128);
  Image *corr_map = add_gray_mask("CorrMap", 8, 64);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_map));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR, corr_map));
  /* Each map holds its newborn user and its record's before the merge. */
  const int base_users = base_map->id.us;
  const int corr_users = corr_map->id.us;

  Image *merged = BKE_paint_layers_mask_stack_merge(*bmain, *ma, *layer, nullptr);
  ASSERT_NE(merged, nullptr);

  /* The merged map is the base's record's own; the old ones are orphans the purge takes. */
  EXPECT_EQ(merged->id.us, 1);
  EXPECT_EQ(base_map->id.us, base_users - 1);
  EXPECT_EQ(corr_map->id.us, corr_users - 1);
}

/** \} */

TEST_F(PaintLayersTargetTest, composite_display_image_ignores_the_active_row)
{
  Image *map = add_solid_image("CompositeMap");
  MaterialPaintLayer *paint = add_paint_layer("Paint", map);
  ASSERT_NE(paint, nullptr);

  /* A row that owns no map: the active row is what the old, row-dependent lookup followed. */
  MaterialPaintLayer *bare = add_active_row("Bare");
  ASSERT_NE(bare, nullptr);
  EXPECT_EQ(BKE_paint_layers_active_layer_get(*ma), bare);

  EXPECT_EQ(BKE_paint_material_composite_display_image(*ma), map);
  /* The row-dependent preferred image still reports nothing, which is why the composite needs the
   * stack-wide lookup. */
  BKE_paint_layers_active_set(*ma, paint->marker);
  EXPECT_EQ(BKE_paint_material_composite_display_image(*ma), map);
  /* A material that holds a map never needs the stand-in. */
  EXPECT_EQ(BKE_paint_material_composite_canvas_image_ensure(*bmain, *ma), map);
  EXPECT_EQ(ma->paint_layers_canvas_image, nullptr);
}

TEST_F(PaintLayersTargetTest, composite_canvas_image_is_created_once_for_a_stack_without_maps)
{
  MaterialPaintLayer *bare = add_active_row("Bare");
  ASSERT_NE(bare, nullptr);
  EXPECT_EQ(BKE_paint_material_composite_display_image(*ma), nullptr);

  Image *canvas = BKE_paint_material_composite_canvas_image_ensure(*bmain, *ma);
  ASSERT_NE(canvas, nullptr);
  EXPECT_EQ(ma->paint_layers_canvas_image, canvas);
  EXPECT_EQ(canvas->id.us, 1);
  EXPECT_TRUE(BKE_paint_material_composite_canvas_image_is(*bmain, *canvas));
  EXPECT_FALSE(canvas->id.flag & ID_FLAG_FAKEUSER);

  /* The lookup key is reused: a second click must not mint another data-block. */
  EXPECT_EQ(BKE_paint_material_composite_canvas_image_ensure(*bmain, *ma), canvas);
  EXPECT_EQ(canvas->id.us, 1);
}

TEST_F(PaintLayersTargetTest, composite_canvas_image_is_recreated_for_a_material_copy)
{
  MaterialPaintLayer *bare = add_active_row("Bare");
  ASSERT_NE(bare, nullptr);
  Image *canvas = BKE_paint_material_composite_canvas_image_ensure(*bmain, *ma);
  ASSERT_NE(canvas, nullptr);
  ASSERT_EQ(canvas->id.us, 1);

  /* The stand-in is a lookup key: a copy must not share it, or both materials would answer to the
   * same image and the editor would show whichever one it found first. */
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->paint_layers_canvas_image, nullptr);
  EXPECT_EQ(canvas->id.us, 1);

  Image *copy_canvas = BKE_paint_material_composite_canvas_image_ensure(*bmain, *copy);
  ASSERT_NE(copy_canvas, nullptr);
  EXPECT_NE(copy_canvas, canvas);
  EXPECT_EQ(copy_canvas->id.us, 1);
  BKE_id_free(bmain, copy);
  EXPECT_EQ(canvas->id.us, 1);
}

TEST_F(PaintLayersTargetTest, internal_maps_mark_marks_only_maps_the_stack_owns)
{
  Image *painted_map = add_solid_image("Painted");
  MaterialPaintLayer *painted = add_paint_layer("Painted", painted_map);
  ASSERT_NE(painted, nullptr);

  /* A Fill owns no map: an image in its channel was assigned by the user. */
  MaterialPaintLayer *fill = BKE_paint_layers_add(*ma, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  Image *user_map = add_solid_image("UserMap");
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, user_map));

  /* Nor does the base mask own the map the user assigns to it. */
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, painted, 1.0f);
  ASSERT_NE(mask, nullptr);
  Image *mask_user_map = add_solid_image("MaskUserMap");
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_user_map));

  Image *canvas = BKE_paint_material_composite_canvas_image_ensure(*bmain, *ma);
  ASSERT_NE(canvas, nullptr);

  /* A file saved before the flag existed: no map carries it yet. */
  painted_map->flag &= ~IMA_PAINT_LAYER_INTERNAL_MAP;
  user_map->flag &= ~IMA_PAINT_LAYER_INTERNAL_MAP;
  mask_user_map->flag &= ~IMA_PAINT_LAYER_INTERNAL_MAP;
  canvas->flag &= ~IMA_PAINT_LAYER_INTERNAL_MAP;

  BKE_paint_layers_internal_maps_mark(*bmain);

  EXPECT_TRUE(BKE_paint_layers_image_is_internal(*painted_map));
  EXPECT_FALSE(BKE_paint_layers_image_is_internal(*user_map));
  EXPECT_FALSE(BKE_paint_layers_image_is_internal(*mask_user_map));
  EXPECT_TRUE(BKE_paint_layers_image_is_internal(*canvas));
}

}  // namespace blender::bke::tests
