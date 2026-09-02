/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_paint_material_combined.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"

#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <array>

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/** A byte RGBA buffer of \a size square filled with one colour. */
static ImBuf *test_byte_ibuf(const int size, const uchar4 value)
{
  ImBuf *ibuf = IMB_allocImBuf(uint(size), uint(size), ImBufFlags::ByteData);
  ibuf->channels = 4;
  uchar *data = ibuf->byte_data_for_write();
  for (const int64_t i : IndexRange(int64_t(size) * size)) {
    data[i * 4 + 0] = value.x;
    data[i * 4 + 1] = value.y;
    data[i * 4 + 2] = value.z;
    data[i * 4 + 3] = value.w;
  }
  return ibuf;
}

/** A float RGBA destination of \a size square. */
static ImBuf *test_float_ibuf(const int size)
{
  ImBuf *ibuf = IMB_allocImBuf(uint(size), uint(size), ImBufFlags::FloatData);
  ibuf->channels = 4;
  return ibuf;
}

/** Inputs whose every channel is its documented default, at \a size square. */
static CombinedInputs test_default_inputs(const int size)
{
  CombinedInputs inputs;
  inputs.width = size;
  inputs.height = size;
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    inputs.channels[i].constant = BKE_paint_material_combined_default_value(
        eMaterialPaintChannel(i));
  }
  return inputs;
}

/** One light straight down `+Z`, no wrap, no ambient, unit exposure. */
static CombinedPreviewLighting test_single_light()
{
  CombinedPreviewLighting lighting;
  lighting.lights_num = 1;
  lighting.lights[0] = CombinedPreviewLight{};
  lighting.ambient_color = float3(0.0f);
  lighting.exposure = 1.0f;
  lighting.ao_influence = 1.0f;
  return lighting;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluator
 * \{ */

TEST(paint_material_combined, defaults_are_principled_preview_values)
{
  /* Pinned because the obvious neighbour, #BKE_paint_material_channel_default_value, disagrees on
   * both of these and would give a black preview and an undefined normal. */
  EXPECT_EQ(BKE_paint_material_combined_default_value(PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            float4(0.8f, 0.8f, 0.8f, 1.0f));
  EXPECT_EQ(BKE_paint_material_combined_default_value(PAINT_MATERIAL_CHANNEL_NORMAL),
            float4(0.0f, 0.0f, 1.0f, 0.0f));
  EXPECT_EQ(BKE_paint_material_combined_default_value(PAINT_MATERIAL_CHANNEL_ROUGHNESS).x, 0.5f);
  EXPECT_EQ(BKE_paint_material_combined_default_value(PAINT_MATERIAL_CHANNEL_METALLIC).x, 0.0f);
  EXPECT_EQ(BKE_paint_material_combined_default_value(PAINT_MATERIAL_CHANNEL_AO).x, 1.0f);
  EXPECT_EQ(BKE_paint_material_combined_default_value(PAINT_MATERIAL_CHANNEL_ALPHA).x, 1.0f);
  EXPECT_EQ(BKE_paint_material_combined_default_value(PAINT_MATERIAL_CHANNEL_EMISSION).x, 0.0f);
}

TEST(paint_material_combined, flat_normal_head_on_light_is_base_times_ndotl)
{
  CombinedInputs inputs = test_default_inputs(4);
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].constant = float4(0.5f, 0.25f, 0.125f, 1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);

  ImBuf *dst = test_float_ibuf(4);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, test_single_light(), dst));

  /* N == L == V, so NdotL is 1 and diffuse is the base colour outright. Specular is zeroed by the
   * Specular channel, so nothing else contributes. */
  const float *px = dst->float_data();
  EXPECT_NEAR(px[0], 0.5f, 1e-4f);
  EXPECT_NEAR(px[1], 0.25f, 1e-4f);
  EXPECT_NEAR(px[2], 0.125f, 1e-4f);
  EXPECT_NEAR(px[3], 1.0f, 1e-4f);
  IMB_freeImBuf(dst);
}

TEST(paint_material_combined, metallic_removes_the_diffuse_term)
{
  CombinedInputs inputs = test_default_inputs(4);
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].constant = float4(0.9f, 0.1f, 0.1f, 1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_METALLIC].constant = float4(1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);

  CombinedPreviewLighting lighting = test_single_light();
  /* A metal still has a specular lobe, and the point here is the missing diffuse one; zeroing the
   * light's specular colour is the only way to isolate it. */
  lighting.lights[0].specular_color = float3(0.0f);

  ImBuf *dst = test_float_ibuf(4);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, lighting, dst));

  /* A fully metallic surface has no diffuse lobe at all, which is what distinguishes this from a
   * merely darker diffuse. */
  const float *px = dst->float_data();
  EXPECT_LT(px[0], 1e-3f);
  IMB_freeImBuf(dst);
}

TEST(paint_material_combined, normal_relief_has_the_correct_sign)
{
  CombinedInputs inputs = test_default_inputs(2);
  /* Encoded tangent normal tilted towards +X: byte 230 in R decodes above zero. */
  ImBuf *normal = test_byte_ibuf(2, uchar4(230, 128, 200, 255));
  inputs.channels[PAINT_MATERIAL_CHANNEL_NORMAL].ibuf = normal;
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);

  CombinedPreviewLighting from_plus_x = test_single_light();
  from_plus_x.lights[0].direction = math::normalize(float3(1.0f, 0.0f, 1.0f));
  CombinedPreviewLighting from_minus_x = test_single_light();
  from_minus_x.lights[0].direction = math::normalize(float3(-1.0f, 0.0f, 1.0f));

  ImBuf *lit_plus = test_float_ibuf(2);
  ImBuf *lit_minus = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, from_plus_x, lit_plus));
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, from_minus_x, lit_minus));

  /* A surface tilted towards the light is brighter than the same surface tilted away. Sign rather
   * than magnitude, because getting the decode backwards produces a plausible-looking image whose
   * relief is inverted. */
  EXPECT_GT(lit_plus->float_data()[0], lit_minus->float_data()[0]);

  IMB_freeImBuf(lit_plus);
  IMB_freeImBuf(lit_minus);
  IMB_freeImBuf(normal);
}

TEST(paint_material_combined, ao_darkens_ambient_but_not_direct_light)
{
  CombinedInputs occluded = test_default_inputs(2);
  occluded.channels[PAINT_MATERIAL_CHANNEL_AO].constant = float4(0.0f);
  occluded.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);
  CombinedInputs lit = occluded;
  lit.channels[PAINT_MATERIAL_CHANNEL_AO].constant = float4(1.0f);

  CombinedPreviewLighting ambient_only = test_single_light();
  ambient_only.lights_num = 0;
  ambient_only.ambient_color = float3(1.0f);

  ImBuf *a = test_float_ibuf(2);
  ImBuf *b = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(occluded, ambient_only, a));
  ASSERT_TRUE(BKE_paint_material_combined_eval(lit, ambient_only, b));
  EXPECT_LT(a->float_data()[0], b->float_data()[0]);

  /* Direct light is untouched by occlusion: applying AO to it is what makes a preview look dirty
   * and is not how EEVEE applies it. */
  ImBuf *c = test_float_ibuf(2);
  ImBuf *d = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(occluded, test_single_light(), c));
  ASSERT_TRUE(BKE_paint_material_combined_eval(lit, test_single_light(), d));
  EXPECT_NEAR(c->float_data()[0], d->float_data()[0], 1e-5f);

  IMB_freeImBuf(a);
  IMB_freeImBuf(b);
  IMB_freeImBuf(c);
  IMB_freeImBuf(d);
}

TEST(paint_material_combined, emission_is_added_unlit)
{
  CombinedInputs inputs = test_default_inputs(2);
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].constant = float4(0.0f, 0.0f, 0.0f, 1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_EMISSION].constant = float4(1.0f, 0.0f, 0.0f, 1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);

  CombinedPreviewLighting dark = test_single_light();
  dark.lights_num = 0;

  ImBuf *dst = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, dark, dst));
  /* Black base under no light, so anything non-zero here can only be the emission. */
  EXPECT_NEAR(dst->float_data()[0], 1.0f, 1e-4f);
  IMB_freeImBuf(dst);
}

TEST(paint_material_combined, emission_strength_scales_the_emission)
{
  /* Principled's Emission Color defaults to white and its Emission Strength to zero, so a preview
   * that reads only the colour adds full white to every ordinary material and washes the image
   * out. The channel table has no row for the strength, which is why it is a field of its own. */
  CombinedInputs inputs = test_default_inputs(2);
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].constant = float4(0.0f, 0.0f, 0.0f, 1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_EMISSION].constant = float4(1.0f, 1.0f, 1.0f, 1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);
  inputs.emission_strength = 0.0f;

  CombinedPreviewLighting dark = test_single_light();
  dark.lights_num = 0;

  ImBuf *unlit = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, dark, unlit));
  EXPECT_NEAR(unlit->float_data()[0], 0.0f, 1e-5f);

  inputs.emission_strength = 2.0f;
  ImBuf *scaled = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, dark, scaled));
  EXPECT_NEAR(scaled->float_data()[0], 2.0f, 1e-4f);

  IMB_freeImBuf(unlit);
  IMB_freeImBuf(scaled);
}

TEST(paint_material_combined, alpha_passes_through_unmodified)
{
  CombinedInputs inputs = test_default_inputs(2);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ALPHA].constant = float4(0.25f);
  ImBuf *dst = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, test_single_light(), dst));
  EXPECT_NEAR(dst->float_data()[3], 0.25f, 1e-5f);
  IMB_freeImBuf(dst);
}

TEST(paint_material_combined, srgb_flag_decides_linearization)
{
  ImBuf *mid_grey = test_byte_ibuf(2, uchar4(188, 188, 188, 255));

  CombinedInputs as_srgb = test_default_inputs(2);
  as_srgb.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].ibuf = mid_grey;
  as_srgb.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].is_srgb = true;
  as_srgb.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);
  CombinedInputs as_data = as_srgb;
  as_data.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].is_srgb = false;

  ImBuf *a = test_float_ibuf(2);
  ImBuf *b = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(as_srgb, test_single_light(), a));
  ASSERT_TRUE(BKE_paint_material_combined_eval(as_data, test_single_light(), b));
  /* sRGB 188/255 is about 0.50 linear, well below the 0.737 the raw bytes would give. */
  EXPECT_LT(a->float_data()[0], b->float_data()[0]);
  EXPECT_NEAR(a->float_data()[0], 0.5f, 0.02f);

  IMB_freeImBuf(a);
  IMB_freeImBuf(b);
  IMB_freeImBuf(mid_grey);
}

TEST(paint_material_combined, region_evaluation_matches_full_evaluation)
{
  /* The single most important test in this file: the whole interactive path rests on a region
   * refresh being indistinguishable from a full one inside the rectangle, and leaving everything
   * outside it exactly as it was. */
  CombinedInputs inputs = test_default_inputs(16);
  ImBuf *base = test_byte_ibuf(16, uchar4(200, 100, 50, 255));
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].ibuf = base;

  ImBuf *full = test_float_ibuf(16);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, test_single_light(), full));

  ImBuf *partial = test_float_ibuf(16);
  const float sentinel = -7.0f;
  float *partial_data = partial->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(16) * 16 * 4)) {
    partial_data[i] = sentinel;
  }
  rcti region;
  BLI_rcti_init(&region, 4, 8, 4, 8);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, test_single_light(), partial, &region));

  for (const int y : IndexRange(16)) {
    for (const int x : IndexRange(16)) {
      const int64_t offset = (int64_t(y) * 16 + x) * 4;
      const bool inside = x >= 4 && x < 8 && y >= 4 && y < 8;
      if (inside) {
        EXPECT_FLOAT_EQ(partial->float_data()[offset], full->float_data()[offset]);
      }
      else {
        EXPECT_FLOAT_EQ(partial->float_data()[offset], sentinel);
      }
    }
  }

  IMB_freeImBuf(full);
  IMB_freeImBuf(partial);
  IMB_freeImBuf(base);
}

TEST(paint_material_combined, mismatched_dimensions_are_rejected_not_resampled)
{
  CombinedInputs inputs = test_default_inputs(8);
  ImBuf *wrong_size = test_byte_ibuf(4, uchar4(255, 255, 255, 255));
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].ibuf = wrong_size;

  ImBuf *dst = test_float_ibuf(8);
  /* Resampling is the gatherer's decision, and only for the bake; the evaluator is strict so that
   * a size disagreement can never be silently absorbed here. */
  EXPECT_FALSE(BKE_paint_material_combined_eval(inputs, test_single_light(), dst));

  IMB_freeImBuf(dst);
  IMB_freeImBuf(wrong_size);
}

TEST(paint_material_combined, custom_and_height_take_no_part)
{
  CombinedInputs plain = test_default_inputs(4);
  plain.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);
  CombinedInputs poisoned = plain;
  poisoned.channels[PAINT_MATERIAL_CHANNEL_CUSTOM].constant = float4(1000.0f);
  poisoned.channels[PAINT_MATERIAL_CHANNEL_HEIGHT].constant = float4(1000.0f);

  ImBuf *a = test_float_ibuf(4);
  ImBuf *b = test_float_ibuf(4);
  ASSERT_TRUE(BKE_paint_material_combined_eval(plain, test_single_light(), a));
  ASSERT_TRUE(BKE_paint_material_combined_eval(poisoned, test_single_light(), b));
  for (const int64_t i : IndexRange(int64_t(4) * 4 * 4)) {
    EXPECT_FLOAT_EQ(a->float_data()[i], b->float_data()[i]);
  }
  IMB_freeImBuf(a);
  IMB_freeImBuf(b);
}

TEST(paint_material_combined, lighting_hash_tracks_every_field_that_shades)
{
  const CombinedPreviewLighting base = BKE_paint_material_combined_lighting_default();

  CombinedPreviewLighting moved = base;
  moved.lights[0].direction = math::normalize(float3(1.0f, 1.0f, 1.0f));
  EXPECT_NE(moved.hash(), base.hash());

  CombinedPreviewLighting brighter = base;
  brighter.exposure = 2.0f;
  EXPECT_NE(brighter.hash(), base.hash());

  CombinedPreviewLighting fewer = base;
  fewer.lights_num = 2;
  EXPECT_NE(fewer.hash(), base.hash());

  /* An unused slot is not part of the shading, so writing to it must not read as a change; a rig
   * arriving fresh from the UI each redraw would otherwise rebuild the canvas every frame. */
  CombinedPreviewLighting unused_slot_touched = fewer;
  unused_slot_touched.lights[3].direction = math::normalize(float3(0.0f, 1.0f, 1.0f));
  EXPECT_EQ(unused_slot_touched.hash(), fewer.hash());
}

/** \} */

}  // namespace blender::bke::tests
