/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_brush.hh"
#include "BKE_paint_types.hh"

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "testing/testing.h"

#include <array>

namespace blender {

/* The flip/invert contract lives in one place (#BKE_brush_mtex_flip_x/_y and
 * #BKE_brush_texture_invert_apply) and keys off the slot, not off the caller's intent. These tests
 * pin the three rules every stroke path relies on: the static bits mirror independently of stroke
 * state, the random draw XORs with them (so a random bit always lands 50/50), and only the brush
 * texture slot takes #BRUSH_TEXTURE_INVERT. */

namespace {

class BrushTextureSlotTest : public testing::Test {
 public:
  Paint paint{};
  Brush brush{};

  void SetUp() override
  {
    paint.runtime = MEM_new<bke::PaintRuntime>(__func__);
  }
  void TearDown() override
  {
    if (paint.runtime != nullptr) {
      MEM_delete(paint.runtime);
    }
  }
};

void set_mapping_flags(MTex &mtex, const int mapping_flags)
{
  mtex.mapping_flags = eMTex_BrushMappingFlags(mapping_flags);
}

}  // namespace

TEST_F(BrushTextureSlotTest, static_flip_does_not_need_stroke_state)
{
  MTex mtex_x;
  set_mapping_flags(mtex_x, MTEX_MAPPING_FLIP_X);
  EXPECT_TRUE(BKE_brush_mtex_flip_x(nullptr, mtex_x, eBrushTextureSlot::Brush));
  EXPECT_FALSE(BKE_brush_mtex_flip_y(nullptr, mtex_x, eBrushTextureSlot::Brush));

  MTex mtex_y;
  set_mapping_flags(mtex_y, MTEX_MAPPING_FLIP_Y);
  EXPECT_FALSE(BKE_brush_mtex_flip_x(nullptr, mtex_y, eBrushTextureSlot::Brush));
  EXPECT_TRUE(BKE_brush_mtex_flip_y(nullptr, mtex_y, eBrushTextureSlot::Brush));
}

TEST_F(BrushTextureSlotTest, static_flip_is_per_slot_bit_not_per_slot_kind)
{
  /* The bits belong to the slot's own MTex: the helper must not read another slot's bits. */
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X);
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Mask));
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::FaceSetColor));
}

TEST_F(BrushTextureSlotTest, random_flip_xors_with_the_static_bit)
{
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM);

  paint.runtime->tex_flip.brush_x = false;
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));

  paint.runtime->tex_flip.brush_x = true;
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));

  /* Static flip plus random: the random draw decides each step, so it is 50/50 either way. */
  MTex static_and_random;
  set_mapping_flags(static_and_random, MTEX_MAPPING_FLIP_X | MTEX_MAPPING_FLIP_X_RANDOM);
  paint.runtime->tex_flip.brush_x = true;
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, static_and_random, eBrushTextureSlot::Brush));
  paint.runtime->tex_flip.brush_x = false;
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, static_and_random, eBrushTextureSlot::Brush));
}

TEST_F(BrushTextureSlotTest, axis_random_draws_are_independent)
{
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM | MTEX_MAPPING_FLIP_Y_RANDOM);

  paint.runtime->tex_flip.brush_x = true;
  paint.runtime->tex_flip.brush_y = false;
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));
  EXPECT_FALSE(BKE_brush_mtex_flip_y(&paint, mtex, eBrushTextureSlot::Brush));
}

TEST_F(BrushTextureSlotTest, mask_random_reads_the_mask_state)
{
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM);

  /* The brush slot's draw must not leak into the mask slot and vice versa. */
  paint.runtime->tex_flip.brush_x = true;
  paint.runtime->tex_flip.mask_x = false;
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Mask));

  paint.runtime->tex_flip.brush_x = false;
  paint.runtime->tex_flip.mask_x = true;
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Mask));
}

TEST_F(BrushTextureSlotTest, faceset_takes_no_random_flip)
{
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM);
  paint.runtime->tex_flip.brush_x = true;

  /* The Face Set color slot has no stroke-lifetime random state: static bits only. */
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::FaceSetColor));
}

TEST_F(BrushTextureSlotTest, channel_source_reads_its_own_random_state)
{
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM | MTEX_MAPPING_FLIP_Y_RANDOM);

  /* The PBR shared mapping has its own draws; the brush slot's must not leak into it. */
  paint.runtime->tex_flip.brush_x = true;
  paint.runtime->tex_flip.channel_x = false;
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::ChannelSource));

  paint.runtime->tex_flip.brush_y = false;
  paint.runtime->tex_flip.channel_y = true;
  EXPECT_TRUE(BKE_brush_mtex_flip_y(&paint, mtex, eBrushTextureSlot::ChannelSource));

  /* The static bits still apply, exactly like the brush slot's. */
  MTex static_mtex;
  set_mapping_flags(static_mtex, MTEX_MAPPING_FLIP_X | MTEX_MAPPING_FLIP_Y);
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, static_mtex, eBrushTextureSlot::ChannelSource));
  EXPECT_TRUE(BKE_brush_mtex_flip_y(&paint, static_mtex, eBrushTextureSlot::ChannelSource));
}

TEST_F(BrushTextureSlotTest, channel_source_random_is_independent_of_mask)
{
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM);

  paint.runtime->tex_flip.mask_x = true;
  paint.runtime->tex_flip.channel_x = false;
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Mask));
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::ChannelSource));
}

TEST_F(BrushTextureSlotTest, null_paint_reads_static_bits_only)
{
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM);
  /* No Paint means no stroke state: a preview must show the configured flip, not a random one. */
  EXPECT_FALSE(BKE_brush_mtex_flip_x(nullptr, mtex, eBrushTextureSlot::Brush));

  MTex static_mtex;
  set_mapping_flags(static_mtex, MTEX_MAPPING_FLIP_X);
  EXPECT_TRUE(BKE_brush_mtex_flip_x(nullptr, static_mtex, eBrushTextureSlot::Brush));

  /* A Paint without a runtime (the editor can hand one before #paint_runtime_init) behaves the
   * same: static bits only, no crash. */
  MEM_delete(paint.runtime);
  paint.runtime = nullptr;
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));
  EXPECT_TRUE(BKE_brush_mtex_flip_x(&paint, static_mtex, eBrushTextureSlot::Brush));
}

TEST(BrushTexFlipState, default_state_is_off)
{
  /* The reset in #PaintStroke::done relies on value-initializing the struct back to "no flips". */
  const bke::BrushTexFlipState state;
  EXPECT_FALSE(state.brush_x);
  EXPECT_FALSE(state.brush_y);
  EXPECT_FALSE(state.mask_x);
  EXPECT_FALSE(state.mask_y);
  EXPECT_FALSE(state.channel_x);
  EXPECT_FALSE(state.channel_y);
}

TEST_F(BrushTextureSlotTest, object_space_slot_takes_no_flip)
{
  /* Object-space sampling has no 2D coordinate to mirror, so every bit reads as off in
   * #MTEX_MAP_MODE_3D. The stroke samplers and the previews both rely on this. */
  MTex mtex;
  set_mapping_flags(mtex,
                    MTEX_MAPPING_FLIP_X | MTEX_MAPPING_FLIP_Y | MTEX_MAPPING_FLIP_X_RANDOM |
                        MTEX_MAPPING_FLIP_Y_RANDOM);
  mtex.brush_map_mode = MTEX_MAP_MODE_3D;
  paint.runtime->tex_flip.brush_x = true;
  paint.runtime->tex_flip.brush_y = true;
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Brush));
  EXPECT_FALSE(BKE_brush_mtex_flip_y(&paint, mtex, eBrushTextureSlot::Brush));
  EXPECT_FALSE(BKE_brush_mtex_flip_x(&paint, mtex, eBrushTextureSlot::Mask));
}

TEST_F(BrushTextureSlotTest, flip_random_eligibility_requires_a_texture_and_a_random_bit)
{
  /* This predicate gates the stroke's RNG spending, not the sampling result: a slot that cannot
   * flip must not make the stroke draw a random value for it. */
  Tex tex{};
  MTex mtex;
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM);
  EXPECT_FALSE(BKE_brush_mtex_flip_random_eligible(mtex));

  mtex.tex = &tex;
  EXPECT_TRUE(BKE_brush_mtex_flip_random_eligible(mtex));

  /* Static flips alone need no draw. */
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X | MTEX_MAPPING_FLIP_Y);
  EXPECT_FALSE(BKE_brush_mtex_flip_random_eligible(mtex));

  /* Object-space mapping takes no flip at all (see #BKE_brush_mtex_flip_x). */
  set_mapping_flags(mtex, MTEX_MAPPING_FLIP_X_RANDOM | MTEX_MAPPING_FLIP_Y_RANDOM);
  mtex.brush_map_mode = MTEX_MAP_MODE_3D;
  EXPECT_FALSE(BKE_brush_mtex_flip_random_eligible(mtex));
}

TEST_F(BrushTextureSlotTest, pbr_flip_random_eligibility_skips_object_space_mapping)
{
  BrushMaterialPaint material_paint{};
  EXPECT_FALSE(BKE_brush_material_source_flip_random_eligible(nullptr));

  material_paint.shared_source_mapping.brush_map_mode = MTEX_MAP_MODE_VIEW;
  set_mapping_flags(material_paint.shared_source_mapping,
                    MTEX_MAPPING_FLIP_X_RANDOM | MTEX_MAPPING_FLIP_Y_RANDOM);
  EXPECT_TRUE(BKE_brush_material_source_flip_random_eligible(&material_paint));

  /* Mixed-slot case: a brush or Mask texture enables random flips, so the stroke reaches the flip
   * block, while the PBR shared mapping is object-space. The mapping takes no flip, so it must be
   * ineligible here too — otherwise the stroke would spend an RNG draw and change
   * #bke::BrushTexFlipState::channel_x/_y for a flip that sampling never applies. */
  material_paint.shared_source_mapping.brush_map_mode = MTEX_MAP_MODE_3D;
  EXPECT_FALSE(BKE_brush_material_source_flip_random_eligible(&material_paint));

  /* Static bits alone need no draw either. */
  material_paint.shared_source_mapping.brush_map_mode = MTEX_MAP_MODE_AREA;
  set_mapping_flags(material_paint.shared_source_mapping,
                    MTEX_MAPPING_FLIP_X | MTEX_MAPPING_FLIP_Y);
  EXPECT_FALSE(BKE_brush_material_source_flip_random_eligible(&material_paint));
}

TEST_F(BrushTextureSlotTest, invert_only_applies_to_the_brush_slot)
{
  brush.flag2 |= BRUSH_TEXTURE_INVERT;

  const auto invert = [&](const eBrushTextureSlot slot) {
    float intensity = 0.25f;
    float rgba[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    BKE_brush_texture_invert_apply(slot, brush, true, &intensity, rgba);
    return std::array<float, 5>{intensity, rgba[0], rgba[1], rgba[2], rgba[3]};
  };

  const std::array<float, 5> brush_sample = invert(eBrushTextureSlot::Brush);
  EXPECT_FLOAT_EQ(brush_sample[0], 0.75f);
  EXPECT_FLOAT_EQ(brush_sample[1], 0.1f);
  EXPECT_FLOAT_EQ(brush_sample[2], 0.2f);
  EXPECT_FLOAT_EQ(brush_sample[3], 0.3f);
  EXPECT_FLOAT_EQ(brush_sample[4], 0.6f);

  for (const eBrushTextureSlot slot :
       {eBrushTextureSlot::Mask, eBrushTextureSlot::FaceSetColor, eBrushTextureSlot::ChannelSource})
  {
    const std::array<float, 5> sample = invert(slot);
    EXPECT_FLOAT_EQ(sample[0], 0.25f) << "slot " << int(slot);
    EXPECT_FLOAT_EQ(sample[4], 0.4f) << "slot " << int(slot);
  }
}

TEST_F(BrushTextureSlotTest, invert_is_off_without_the_flag)
{
  const eBrushTextureSlot slot = eBrushTextureSlot::Brush;
  float intensity = 0.25f;
  float rgba[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  BKE_brush_texture_invert_apply(slot, brush, true, &intensity, rgba);
  EXPECT_FLOAT_EQ(intensity, 0.25f);
  EXPECT_FLOAT_EQ(rgba[3], 0.4f);
}

TEST_F(BrushTextureSlotTest, invert_of_a_grayscale_sample_fills_rgb_from_the_value)
{
  /* A grayscale sample carries no RGB: the invert must leave the three channels equal to the
   * flipped scalar, so consumers that read either see the same value. */
  float intensity = 0.2f;
  float rgba[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  BKE_brush_texture_invert_value(false, &intensity, rgba);
  EXPECT_FLOAT_EQ(intensity, 0.8f);
  EXPECT_FLOAT_EQ(rgba[0], 0.8f);
  EXPECT_FLOAT_EQ(rgba[1], 0.8f);
  EXPECT_FLOAT_EQ(rgba[2], 0.8f);
  EXPECT_FLOAT_EQ(rgba[3], 1.0f);
}

}  // namespace blender
