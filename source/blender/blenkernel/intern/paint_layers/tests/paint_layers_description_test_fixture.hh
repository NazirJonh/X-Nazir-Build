/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The fixture and the small row helpers shared by the paint layer description test files.
 */

#pragma once

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

class PaintLayersDescription : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  /** The cache budget the test found, restored on teardown. */
  size_t slice_cache_budget_before_ = 0;
  /** Scratch for the composite descriptions the height tests read. */
  Vector<PaintMaterialCompositeImageLayer> layers_;

  void SetUp() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    slice_cache_budget_before_ = paint_layer_anchor_point_slice_cache_budget();
    bmain = BKE_main_new();
    BKE_blender_globals_main_replace(bmain);
    BKE_tempdir_init(nullptr);
  }

  void TearDown() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    paint_layer_anchor_point_slice_cache_set_budget(slice_cache_budget_before_);
    G_MAIN = nullptr;
    BKE_main_free(bmain);
    BKE_tempdir_session_purge();
  }

  /** A row linked straight into the list, the way a file read leaves it: no BKE call involved. */
  MaterialPaintLayer *paint_layer_add(Material &ma, const char *name)
  {
    MaterialPaintLayer *layer = MEM_new<MaterialPaintLayer>(__func__);
    STRNCPY(layer->name, name);
    layer->marker = BLI_uuid_generate_random();
    layer->flag = MA_PAINT_LAYER_ENABLED;
    layer->opacity = 1.0f;
    BLI_addtail(&ma.paint_layers, layer);
    return layer;
  }

  MaterialPaintLayerChannel &paint_layer_channel_add(MaterialPaintLayer &layer, Image &image)
  {
    layer.channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
    layer.channels_num = 1;
    layer.channels[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    layer.channels[0].state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    layer.channels[0].image = &image;
    layer.channels[0].gradient_ramp = nullptr;
    return layer.channels[0];
  }
};

/** The first row of the stack, or null when it is empty. */
inline MaterialPaintLayer *paint_layers_first(Material &ma)
{
  return ma.paint_layers.is_empty() ? nullptr : &*ma.paint_layers.begin();
}

/** A Fill row whose Base Color record carries a map, the one shape that supports a mapping. */
inline MaterialPaintLayer *mapping_fill_with_map(Main &bmain, Material &ma, const char *name)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(ma, name, nullptr, PaintLayerPlace::Above);
  EXPECT_TRUE(BKE_paint_layers_set_source(ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT));
  Image *map = static_cast<Image *>(BKE_id_new(&bmain, ID_IM, name));
  /* A bare row owns no record yet: the map needs one to land in. */
  EXPECT_NE(BKE_paint_layers_channel_add(ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  EXPECT_TRUE(
      BKE_paint_layers_channel_set_image(ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  return fill;
}

}  // namespace blender::bke::tests
