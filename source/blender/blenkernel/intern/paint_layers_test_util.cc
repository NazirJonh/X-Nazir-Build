/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "intern/paint_layers_test_util.hh"

#include "DNA_material_types.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_uuid.h"

#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"

#include "MEM_guardedalloc.h"

namespace blender::bke::test {

void PaintLayersTestBase::SetUp()
{
  bmain = BKE_main_new();
  G_MAIN = bmain;
  ma = BKE_material_add(bmain, "Layered");
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
}

void PaintLayersTestBase::TearDown()
{
  BKE_main_free(bmain);
  G_MAIN = nullptr;
}

Image *PaintLayersTestBase::add_solid_image(const char *name)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  return BKE_image_add_generated(
      bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
}

MaterialPaintLayer *PaintLayersTestBase::add_paint_layer(const char *name, Image *image)
{
  return add_paint_layer_into(nullptr, name, image);
}

MaterialPaintLayer *PaintLayersTestBase::add_paint_layer_into(MaterialPaintLayer *anchor,
                                                              const char *name,
                                                              Image *image)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, anchor, PaintLayerPlace::Above);
  EXPECT_NE(layer, nullptr);
  layer->channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
  layer->channels[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  layer->channels[0].state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  layer->channels[0].image = image;
  layer->channels_num = 1;
  return layer;
}

MaterialPaintLayer *PaintLayersTestBase::add_folder(const char *name)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma,
                                                    MA_PAINT_LAYER_SOURCE_STACK,
                                                    name,
                                                    nullptr,
                                                    PaintLayerPlace::Above);
  EXPECT_NE(folder, nullptr);
  return folder;
}

MaterialPaintLayer *PaintLayersTestBase::add_mask_item(MaterialPaintLayer &parent,
                                                       const char *name,
                                                       Image *image)
{
  /* Hand-built, not through #BKE_paint_layers_add: the role decides which list of the parent a row
   * joins, so a Mask Item is born on the parent's mask stack rather than moved there afterwards.
   * The same shape every description test's own mask helper uses. */
  MaterialPaintLayer *item = MEM_new<MaterialPaintLayer>(__func__);
  STRNCPY(item->name, name);
  item->marker = BLI_uuid_generate_random();
  item->source = MA_PAINT_LAYER_SOURCE_IMAGE;
  item->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  item->channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
  item->channels[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  item->channels[0].state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  item->channels[0].image = image;
  item->channels_num = 1;
  BLI_addtail(&parent.mask_stack, item);
  return item;
}

void PaintLayersTestBase::age_cold_mark(MaterialPaintLayer *layer)
{
  MaterialPaintLayersRuntime *runtime = paint_layers_runtime_mutable(*ma);
  ASSERT_NE(runtime, nullptr);
  double *since = runtime->hidden_since.lookup_ptr(layer->marker);
  ASSERT_NE(since, nullptr);
  *since = BLI_time_now_seconds() - PAINT_LAYERS_COLD_TIER_SECONDS - 1.0;
}

}  // namespace blender::bke::test
