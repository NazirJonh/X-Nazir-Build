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
#include "BKE_paint_layers_target.hh"

#include "BLI_index_range.hh"
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

/** \} */

}  // namespace blender::bke::tests
