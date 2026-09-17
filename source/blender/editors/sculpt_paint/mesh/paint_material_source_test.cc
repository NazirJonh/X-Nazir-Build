/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "paint_material_source.hh"

#include "BKE_gtest_base.hh"
#include "BKE_main.hh"

#include "DNA_brush_types.h"
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
  PaintModeSettings mode_settings{};
  mode_settings.layer_target_mode = PAINT_LAYER_TARGET_MASK;

  BrushMaterialPaint brush_paint{};
  const material::ChannelSourceSet sources(
      brush_paint, mode_settings, PAINT_MATERIAL_CHANNELS_VISIBLE_ALL);
  EXPECT_FALSE(sources.is_active());
}

}  // namespace blender::ed::sculpt_paint::tests
