/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for #BKE_paint_layers_target.hh: resolving the active row's paint target and growing the
 * description on the first stroke, including the Fill -> Paint conversion (C-3).
 */

#include "testing/testing.h"

#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
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
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

namespace blender::bke::tests {

class PaintLayersTargetTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
  }
  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  /** Composite \a channel of \a ma into a float image and read one pixel. */
  void composite_pixel(Material &ma, const int channel, const int size, float r_rgba[4])
  {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *dst = BKE_image_add_generated(
        bmain, size, size, "Dst", 32, true, IMA_GENTYPE_BLANK, black, false, true, false);
    EXPECT_NE(dst, nullptr);
    EXPECT_TRUE(BKE_paint_layers_composite_image(ma, channel, *dst, nullptr, nullptr));
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
    EXPECT_NE(ibuf, nullptr);
    const float *pixels = ibuf->float_data();
    copy_v4_v4(r_rgba, pixels);
    BKE_image_release_ibuf(dst, ibuf, lock);
    BKE_id_free(bmain, dst);
  }

  /** A byte map holding one solid colour, straight alpha. */
  Image *solid_image(const char *name, const int size, const uchar r, const uchar g, const uchar b)
  {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      pixels[i * 4 + 0] = r;
      pixels[i * 4 + 1] = g;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }
};

/** The map a mask item carries in its Base-Color channel record, or null. */
static Image *mask_item_map(const MaterialPaintLayer &item)
{
  for (int i = 0; i < item.channels_num; i++) {
    if (item.channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      return item.channels[i].image;
    }
  }
  return nullptr;
}

TEST_F(PaintLayersTargetTest, get_resolves_active_row_and_absent_channel)
{
  Material *ma = BKE_material_add(bmain, "TargetMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  BKE_paint_layers_active_set(*ma, layer->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
  EXPECT_EQ(target.material, ma);
  EXPECT_EQ(target.layer, layer);
  EXPECT_EQ(target.channel, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  /* The row has no record yet, so there is no map -- but the row is the target. */
  EXPECT_EQ(target.channel_record, nullptr);
  EXPECT_EQ(BKE_paint_layers_target_image(target), nullptr);
}

TEST_F(PaintLayersTargetTest, get_refuses_flat_folder_and_no_active)
{
  Material *flat = BKE_material_add(bmain, "FlatMat");
  PaintLayersTarget target;
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *flat, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));

  Material *ma = BKE_material_add(bmain, "FolderMat");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, folder->marker);
  /* A folder carries no maps, so it is never a target. */
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));

  /* A layered material without an active marker has no target. */
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
}

TEST_F(PaintLayersTargetTest, ensure_creates_channel_and_neutral_map)
{
  Material *ma = BKE_material_add(bmain, "EnsureMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, layer->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, PaintLayersTargetMode::Content, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->alpha_mode, IMA_ALPHA_STRAIGHT);
  EXPECT_EQ(target.channel_record->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(BKE_paint_layers_target_image(target), image);
  /* A scalar channel uses a data colorspace. */
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(image->colorspace_settings.name));

  /* A second call returns the same map, minting nothing new. */
  EXPECT_EQ(BKE_paint_layers_target_ensure_writable(*bmain, target, 4), image);
}

TEST_F(PaintLayersTargetTest, ensure_normal_neutral_is_flat)
{
  Material *ma = BKE_material_add(bmain, "NormalMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, layer->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_NORMAL, PaintLayersTargetMode::Content, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 2);
  ASSERT_NE(image, nullptr);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *p = ibuf->byte_data();
  EXPECT_EQ(p[0], 128);
  EXPECT_EQ(p[1], 128);
  EXPECT_EQ(p[2], 255);
  BKE_image_release_ibuf(image, ibuf, lock);
}

TEST_F(PaintLayersTargetTest, ensure_mask_creates_white_straight_map)
{
  Material *ma = BKE_material_add(bmain, "MaskMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, layer->marker);

  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
  ASSERT_FALSE(items.is_empty());
  EXPECT_EQ(mask_item_map(*items.first()), image);
  /* One storage convention for every paint-layer map: straight bytes, GPU-linear flag. */
  EXPECT_EQ(image->alpha_mode, IMA_ALPHA_STRAIGHT);
  EXPECT_TRUE((image->flag & IMA_GPU_LINEAR_PREMUL) != 0);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(image->colorspace_settings.name));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *p = ibuf->byte_data();
  EXPECT_EQ(p[0], 255);
  BKE_image_release_ibuf(image, ibuf, lock);
}

TEST_F(PaintLayersTargetTest, ensure_painted_mask_item_creates_transparent_straight_map)
{
  Material *ma = BKE_material_add(bmain, "MaskCorrMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_PAINT, "M");
  ASSERT_NE(correction, nullptr);

  PaintLayersTarget target;
  target.material = ma;
  target.layer = correction;
  target.mask_item = correction;
  target.mode = PaintLayersTargetMode::Mask;
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(mask_item_map(*correction), image);
  EXPECT_EQ(image->alpha_mode, IMA_ALPHA_STRAIGHT);
  EXPECT_TRUE((image->flag & IMA_GPU_LINEAR_PREMUL) != 0);

  /* Transparent coverage: an unpainted item changes nothing. */
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *p = ibuf->byte_data();
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  EXPECT_EQ(p[3], 0);
  BKE_image_release_ibuf(image, ibuf, lock);
}

TEST_F(PaintLayersTargetTest, fill_content_refuses_strokes_and_its_mask_takes_them)
{
  const int size = 8;
  Material *ma = BKE_material_add(bmain, "FillMat");

  /* A bottom map gives the stack dimensions and something for the fill to blend over. */
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *bottom_base = BKE_paint_layers_channel_add(
      *ma, bottom, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  bottom_base->image = solid_image("BottomBase", size, 40, 80, 120);
  bottom_base->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  MaterialPaintLayerChannel *bottom_rough = BKE_paint_layers_channel_add(
      *ma, bottom, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  bottom_rough->image = solid_image("BottomRough", size, 200, 200, 200);
  bottom_rough->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FILL, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  const float fill[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, layer, fill));
  BKE_paint_layers_active_set(*ma, layer->marker);

  float before_base[4], before_rough[4];
  composite_pixel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, before_base);
  composite_pixel(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, size, before_rough);

  /* A Fill is a colour: its content refuses the stroke, and nothing about the row changes. It is
   * changed through a Correction on top instead. */
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
  EXPECT_NE(BKE_paint_layers_target_refusal(target), nullptr);
  EXPECT_EQ(BKE_paint_layers_target_ensure_writable(*bmain, target, size), nullptr);
  EXPECT_EQ(layer->kind, MA_PAINT_LAYER_KIND_FILL);
  for (int i = 0; i < layer->channels_num; i++) {
    EXPECT_EQ(layer->channels[i].image, nullptr);
  }
  float after_base[4], after_rough[4];
  composite_pixel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, after_base);
  composite_pixel(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, size, after_rough);
  EXPECT_FLOAT_EQ(after_base[0], before_base[0]);
  EXPECT_FLOAT_EQ(after_base[1], before_base[1]);
  EXPECT_FLOAT_EQ(after_base[2], before_base[2]);
  EXPECT_FLOAT_EQ(after_rough[0], before_rough[0]);

  /* Its mask is what a stroke on a Fill paints: the first stroke creates the mask's map. */
  PaintLayersTarget mask_target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, mask_target));
  EXPECT_EQ(BKE_paint_layers_target_refusal(mask_target), nullptr);
  Image *mask_image = BKE_paint_layers_target_ensure_writable(*bmain, mask_target, size);
  ASSERT_NE(mask_image, nullptr);
  const Vector<MaterialPaintLayer *> mask_items = BKE_paint_layers_mask_items(*layer);
  ASSERT_FALSE(mask_items.is_empty());
  EXPECT_EQ(mask_item_map(*mask_items.first()), mask_image);
  EXPECT_EQ(layer->kind, MA_PAINT_LAYER_KIND_FILL);
}

TEST_F(PaintLayersTargetTest, resolver_reads_the_mode_from_settings)
{
  Material *ma = BKE_material_add(bmain, "ModeMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, layer->marker);

  PaintModeSettings settings = {};
  settings.layer_target_mode = PAINT_LAYER_TARGET_MASK;

  /* MASK on a row without a mask: the target resolves, carries no item, and the first stroke
   * creates one through ensure_writable. No silent fall back to content. */
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.mask_item, nullptr);
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, target, 4);
  EXPECT_NE(image, nullptr);
  const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
  ASSERT_FALSE(items.is_empty());
  EXPECT_EQ(mask_item_map(*items.first()), image);
}

TEST_F(PaintLayersTargetTest, layer_target_mode_rna_is_read_only)
{
  PaintModeSettings settings = {};
  PointerRNA ptr = RNA_pointer_create_discrete(
      nullptr, RNA_struct_find("PaintModeSettings"), &settings);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "layer_target_mode");
  ASSERT_NE(prop, nullptr);
  /* Read-only, like Object.mode: switching goes through the operator that also moves the brush. */
  EXPECT_FALSE(RNA_property_editable(&ptr, prop));
}

TEST_F(PaintLayersTargetTest, is_frozen_follows_always_bake_on_row_or_ancestor)
{
  Material *ma = BKE_material_add(bmain, "FrozenMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, layer->marker);
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));

  /* No bake, AUTO and NEVER do not freeze. */
  EXPECT_FALSE(BKE_paint_layers_target_is_frozen(target));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(
      *ma, *layer, MA_PAINT_LAYER_BAKE_AUTO));
  EXPECT_FALSE(BKE_paint_layers_target_is_frozen(target));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(
      *ma, *layer, MA_PAINT_LAYER_BAKE_NEVER));
  EXPECT_FALSE(BKE_paint_layers_target_is_frozen(target));

  /* ALWAYS on the row itself freezes. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(
      *ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  EXPECT_TRUE(BKE_paint_layers_target_is_frozen(target));

  /* ALWAYS on an ancestor folder freezes a child too. */
  Material *ma2 = BKE_material_add(bmain, "FrozenParent");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma2, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma2, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  BKE_paint_layers_active_set(*ma2, child->marker);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(
      *ma2, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  PaintLayersTarget child_target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma2, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, child_target));
  EXPECT_TRUE(BKE_paint_layers_target_is_frozen(child_target));
}

/** The active layered material is the active object's active slot, and only when it is layered. */
TEST_F(PaintLayersTargetTest, active_material_follows_the_active_slot)
{
  Mesh *mesh = BKE_mesh_add(bmain, "Mesh");
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "Object");
  ob->data = &mesh->id;
  id_us_plus(&mesh->id);

  Material *layered = BKE_material_add(bmain, "Layered");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_KIND_PAINT, "Layer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*layered, row->marker);
  Material *plain = BKE_material_add(bmain, "Plain");

  BKE_object_material_slot_add(bmain, ob);
  BKE_object_material_slot_add(bmain, ob);
  BKE_object_material_assign(bmain, ob, layered, 1, BKE_MAT_ASSIGN_OBJECT);
  BKE_object_material_assign(bmain, ob, plain, 2, BKE_MAT_ASSIGN_OBJECT);

  /* The active slot is the plain material: no layered material, even though one exists elsewhere. */
  ob->actcol = 2;
  EXPECT_EQ(BKE_paint_layers_active_material_get(ob), nullptr);

  ob->actcol = 1;
  EXPECT_EQ(BKE_paint_layers_active_material_get(ob), layered);
  EXPECT_EQ(BKE_paint_layers_active_layer_get(*layered), row);

  /* No active row marker: the layer answer is null, the material answer is not. */
  BKE_paint_layers_active_set(*layered, {});
  EXPECT_EQ(BKE_paint_layers_active_layer_get(*layered), nullptr);
  EXPECT_EQ(BKE_paint_layers_active_material_get(ob), layered);

  EXPECT_EQ(BKE_paint_layers_active_material_get(nullptr), nullptr);
}

TEST_F(PaintLayersTargetTest, find_image_use_reads_the_description)
{
  Material *ma = BKE_material_add(bmain, "UseMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);

  Image *content = solid_image("Content", 2, 10, 20, 30);
  Image *mask = solid_image("Mask", 2, 255, 255, 255);
  Image *correction_map = solid_image("Corr", 2, 1, 2, 3);
  Image *foreign = solid_image("Foreign", 2, 9, 9, 9);

  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = content;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *mask_item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(mask_item, nullptr);
  MaterialPaintLayerChannel *mask_record = BKE_paint_layers_channel_add(
      *ma, mask_item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(mask_record, nullptr);
  mask_record->image = mask;
  mask_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, nullptr);
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *correction_record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(correction_record, nullptr);
  correction_record->image = correction_map;
  correction_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  PaintLayersImageUse use;

  /* The row's channel map. */
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *content, use));
  EXPECT_EQ(use.layer, layer);
  EXPECT_EQ(use.role, PaintLayersImageUseRole::Content);
  EXPECT_EQ(use.channel, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(BLI_uuid_is_nil(use.correction));

  /* The row's mask item map. */
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *mask, use));
  EXPECT_EQ(use.layer, mask_item);
  EXPECT_EQ(use.role, PaintLayersImageUseRole::Mask);
  EXPECT_EQ(use.channel, PAINT_LAYER_MAP_MASK);
  EXPECT_FALSE(BLI_uuid_is_nil(use.correction));

  /* A correction's map: the correction row is what comes back, carrying its marker. */
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *correction_map, use));
  EXPECT_EQ(use.layer, correction);
  EXPECT_EQ(use.role, PaintLayersImageUseRole::Correction);
  EXPECT_TRUE(BLI_uuid_equal(use.correction, correction->marker));

  /* An image no row carries. */
  EXPECT_FALSE(BKE_paint_layers_find_image_use(*ma, *foreign, use));

  /* A map shared by two materials resolves to the row of the material that was searched. */
  Material *other = BKE_material_add(bmain, "Other");
  MaterialPaintLayer *other_layer = BKE_paint_layers_add(
      *other, MA_PAINT_LAYER_KIND_PAINT, "O", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *other_record = BKE_paint_layers_channel_add(
      *other, other_layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(other_record, nullptr);
  other_record->image = content;
  other_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_find_image_use(*ma, *content, use));
  EXPECT_EQ(use.layer, layer);
  ASSERT_TRUE(BKE_paint_layers_find_image_use(*other, *content, use));
  EXPECT_EQ(use.layer, other_layer);
  EXPECT_EQ(use.channel, int(PAINT_MATERIAL_CHANNEL_ROUGHNESS));
}

TEST_F(PaintLayersTargetTest, channel_blend_opacity_effective_and_setters)
{
  Material *ma = BKE_material_add(bmain, "ChannelBlendMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_EQ(layer->channels_num, 0);

  /* No record: the row's own values are what is effective. */
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            layer->blend);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_opacity_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
      layer->opacity);

  layer->blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
  layer->opacity = 0.5f;

  /* A `-1` override still reads back as the row's blend; the settings array is written, no channel
   * record is created. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BAKE_STALE);
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, -1));
  EXPECT_EQ(layer->channels_num, 0);
  EXPECT_EQ(layer->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].blend, -1);
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0);

  /* The opacity edit is value-only and creates no record either. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BAKE_STALE);
  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.25f));
  EXPECT_EQ(layer->channels_num, 0);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_opacity_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
      0.125f);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0);

  /* A blend override is structural. */
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_ADD));
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            MA_PAINT_LAYER_BLEND_ADD);
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0);

  /* The Normal Combine is the Normal channel's own and is refused as an override. */
  EXPECT_FALSE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_NORMAL_COMBINE));

  /* The Normal channel has no blend override at all: its combine is forced, so a stored blend
   * could never take effect. */
  EXPECT_FALSE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_NORMAL, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_EQ(layer->channel_settings[PAINT_MATERIAL_CHANNEL_NORMAL].blend, -1);
}

TEST_F(PaintLayersTargetTest, kind_change_keeps_channel_participation)
{
  Material *ma = BKE_material_add(bmain, "KindParticipation");
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *b = BKE_paint_layers_channel_add(
      *ma, bottom, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(b, nullptr);
  b->image = solid_image("BottomBC", 4, 255, 255, 255);
  b->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *top = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Top", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *t = BKE_paint_layers_channel_add(
      *ma, top, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(t, nullptr);
  t->image = solid_image("TopBC", 4, 255, 0, 0);
  t->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(
      *ma, *top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.5f));
  /* The Fill the conversion produces shows this colour; matching the map's linear value makes the
   * two states comparable. */
  const float fill[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  copy_v4_v4(top->fill_color, fill);

  float painted[4];
  composite_pixel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 4, painted);

  /* Paint -> Fill clears the map but keeps the record, so the pair still takes part with the same
   * blend and opacity and the composite is unchanged. */
  ASSERT_TRUE(BKE_paint_layers_kind_change(*ma, top, MA_PAINT_LAYER_KIND_FILL));
  ASSERT_EQ(top->channels_num, 1);
  EXPECT_EQ(top->channels[0].image, nullptr);
  EXPECT_EQ(top->channels[0].state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(top->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].blend,
            MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_FLOAT_EQ(top->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].opacity, 0.5f);

  float filled[4];
  composite_pixel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 4, filled);
  EXPECT_NEAR(painted[0], filled[0], 1e-4f);
  EXPECT_NEAR(painted[1], filled[1], 1e-4f);
  EXPECT_NEAR(painted[2], filled[2], 1e-4f);

  /* Fill -> Paint -> Fill keeps the record too: the rule is about the pair, not the kind. */
  ASSERT_TRUE(BKE_paint_layers_kind_change(*ma, top, MA_PAINT_LAYER_KIND_PAINT));
  ASSERT_TRUE(BKE_paint_layers_kind_change(*ma, top, MA_PAINT_LAYER_KIND_FILL));
  ASSERT_EQ(top->channels_num, 1);
  EXPECT_EQ(top->channels[0].image, nullptr);
  EXPECT_EQ(top->channels[0].state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_FLOAT_EQ(top->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].opacity, 0.5f);
}

TEST_F(PaintLayersTargetTest, folder_mask_is_a_target_but_content_is_not)
{
  Material *ma = BKE_material_add(bmain, "FolderMaskMat");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  BKE_paint_layers_active_set(*ma, folder->marker);

  /* A folder has no channel maps of its own: content stays refused. */
  PaintLayersTarget content;
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, content));

  /* Its mask is a coverage over the whole subtree and resolves like any row's. */
  PaintLayersTarget mask;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, mask));
  EXPECT_EQ(mask.layer, folder);
  EXPECT_EQ(mask.mode, PaintLayersTargetMode::Mask);

  ASSERT_NE(BKE_paint_layers_mask_add(*ma, folder, 1.0f), nullptr);
  PaintLayersTarget mask_with_map;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, mask_with_map));
  const Vector<MaterialPaintLayer *> folder_masks = BKE_paint_layers_mask_items(*folder);
  ASSERT_FALSE(folder_masks.is_empty());
  EXPECT_EQ(mask_with_map.mask_item, folder_masks.first());

  /* The first stroke's ensure_writable grows the folder's mask map. */
  Image *image = BKE_paint_layers_target_ensure_writable(*bmain, mask_with_map, 4);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(mask_item_map(*folder_masks.first()), image);
}

}  // namespace blender::bke::tests
