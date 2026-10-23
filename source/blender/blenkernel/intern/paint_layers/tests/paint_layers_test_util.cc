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

#include "../paint_layers_intern.hh"

namespace blender::bke::tests {

/**
 * The slice cache is process-wide: a slice a previous test rendered must not be found by this one,
 * and a budget a previous test shrank must not hide a slice here. The budget found is remembered
 * rather than reset to a constant, so the default lives in one place (the cache itself).
 */
void PaintLayersTestBase::SetUp()
{
  paint_layer_anchor_point_slice_cache_clear();
  slice_cache_budget_before_ = paint_layer_anchor_point_slice_cache_budget();
  bmain = BKE_main_new();
  G_MAIN = bmain;
  ma = BKE_material_add(bmain, "Layered");
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
}

void PaintLayersTestBase::TearDown()
{
  paint_layer_anchor_point_slice_cache_clear();
  paint_layer_anchor_point_slice_cache_set_budget(slice_cache_budget_before_);
  BKE_main_free(bmain);
  G_MAIN = nullptr;
}

AnchorSliceCacheBudgetGuard::AnchorSliceCacheBudgetGuard(const size_t bytes)
    : previous_(paint_layer_anchor_point_slice_cache_budget())
{
  paint_layer_anchor_point_slice_cache_set_budget(bytes);
}

AnchorSliceCacheBudgetGuard::~AnchorSliceCacheBudgetGuard()
{
  paint_layer_anchor_point_slice_cache_set_budget(previous_);
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
