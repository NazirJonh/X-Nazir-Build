/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_layers_test_util.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"

namespace blender::bke::tests {

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
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
  EXPECT_NE(layer, nullptr);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  EXPECT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
  return layer;
}

}  // namespace blender::bke::tests
