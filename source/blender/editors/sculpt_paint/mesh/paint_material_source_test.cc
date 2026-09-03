/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "paint_material_source.hh"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_scene_types.h"

namespace blender::ed::sculpt_paint::tests {

class PaintMaterialSourceMaskTest : public bke::BlenderGTestBase {
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
};

TEST_F(PaintMaterialSourceMaskTest, channel_source_set_is_inactive_in_mask_mode)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *mask_image = BKE_image_add_generated(
      bmain, 8, 8, "Mask3", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  PaintModeSettings mode_settings{};
  mode_settings.mask_image_binding.image = mask_image;

  BrushMaterialPaint brush_paint{};
  const material::ChannelSourceSet sources(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNELS_VISIBLE_ALL);
  EXPECT_FALSE(sources.is_active());
}

}  // namespace blender::ed::sculpt_paint::tests
