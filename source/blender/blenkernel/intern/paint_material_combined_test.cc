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

#include <algorithm>
#include <array>
#include <cmath>

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

TEST(paint_material_combined, a_float_base_input_keeps_its_hdr_value)
{
  /* The Combined preview shades scene-referred, so a float base input -- which is what the
   * composite cache's float mode hands over -- lights past one the way the viewport's live graph
   * does. Clamping it here would clip the very HDR the float mode exists to keep. */
  CombinedInputs inputs = test_default_inputs(2);
  ImBuf *base = test_float_ibuf(2);
  float *data = base->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(2) * 2)) {
    data[i * 4 + 0] = 1.5f;
    data[i * 4 + 1] = 1.5f;
    data[i * 4 + 2] = 1.5f;
    data[i * 4 + 3] = 1.0f;
  }
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].ibuf = base;
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].is_srgb = false;
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(1.0f);

  ImBuf *dst = test_float_ibuf(2);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, test_single_light(), dst));

  /* N == L == V with the specular zeroed: the diffuse term is the base colour outright, HDR
   * included. */
  const float *px = dst->float_data();
  EXPECT_NEAR(px[0], 1.5f, 1e-4f);
  EXPECT_NEAR(px[1], 1.5f, 1e-4f);
  EXPECT_NEAR(px[2], 1.5f, 1e-4f);
  IMB_freeImBuf(base);
  IMB_freeImBuf(dst);
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

  CombinedPreviewLighting moved_off_plane = base;
  moved_off_plane.lights[0].distance = 0.25f;
  EXPECT_NE(moved_off_plane.hash(), base.hash());

  /* The shipped rig is finite-distance: this is what makes the finite-distance lights visible on
   * a flat canvas. A rig built by hand defaults to directional. */
  EXPECT_GT(BKE_paint_material_combined_lighting_default().lights[0].distance, 0.0f);
  EXPECT_EQ(CombinedPreviewLight{}.distance, 0.0f);
}

TEST(paint_material_combined, plane_extent_normalizes_the_larger_canvas_dimension)
{
  EXPECT_EQ(BKE_paint_material_combined_plane_extent(int2(16, 16)), float2(1.0f));
  /* The shorter axis carries the ratio; the larger one is always 1. */
  EXPECT_EQ(BKE_paint_material_combined_plane_extent(int2(8, 16)), float2(0.5f, 1.0f));
  EXPECT_EQ(BKE_paint_material_combined_plane_extent(int2(16, 8)), float2(1.0f, 0.5f));
}

static float test_pixel_luminance(const ImBuf *ibuf, const int x, const int y)
{
  const float *p = ibuf->float_data() + (int64_t(y) * ibuf->x + x) * 4;
  return (p[0] + p[1] + p[2]) / 3.0f;
}

TEST(paint_material_combined, finite_light_rotation_changes_flat_canvas)
{
  CombinedInputs inputs = test_default_inputs(16);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(0.25f);

  CombinedPreviewLighting lighting = BKE_paint_material_combined_lighting_default();
  ImBuf *a = test_float_ibuf(16);
  ImBuf *b = test_float_ibuf(16);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, lighting, a));

  BKE_paint_material_combined_lighting_rotate_z(lighting, float(M_PI) * 0.5f);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, lighting, b));

  float total_diff = 0.0f;
  for (const int i : IndexRange(16 * 16)) {
    const float *pa = a->float_data() + int64_t(i) * 4;
    const float *pb = b->float_data() + int64_t(i) * 4;
    total_diff += std::abs(pa[0] - pb[0]) + std::abs(pa[1] - pb[1]) + std::abs(pa[2] - pb[2]);
  }
  EXPECT_GT(total_diff, 1.0f);
  IMB_freeImBuf(a);
  IMB_freeImBuf(b);
}

TEST(paint_material_combined, finite_light_roughness_changes_flat_canvas)
{
  CombinedInputs sharp = test_default_inputs(24);
  sharp.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(0.15f);
  CombinedInputs rough = test_default_inputs(24);
  rough.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(0.9f);

  const CombinedPreviewLighting lighting = BKE_paint_material_combined_lighting_default();
  ImBuf *a = test_float_ibuf(24);
  ImBuf *b = test_float_ibuf(24);
  ASSERT_TRUE(BKE_paint_material_combined_eval(sharp, lighting, a));
  ASSERT_TRUE(BKE_paint_material_combined_eval(rough, lighting, b));

  float max_a = 0.0f;
  float max_b = 0.0f;
  for (const int i : IndexRange(24 * 24)) {
    max_a = std::max(max_a, test_pixel_luminance(a, i % 24, i / 24));
    max_b = std::max(max_b, test_pixel_luminance(b, i % 24, i / 24));
  }
  EXPECT_GT(max_a, max_b * 3.0f);
  IMB_freeImBuf(a);
  IMB_freeImBuf(b);
}

TEST(paint_material_combined, finite_light_roughness_extremes_are_finite)
{
  for (const float roughness : {0.0f, 1.0f}) {
    CombinedInputs inputs = test_default_inputs(8);
    inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(roughness);
    ImBuf *dst = test_float_ibuf(8);
    ASSERT_TRUE(BKE_paint_material_combined_eval(
        inputs, BKE_paint_material_combined_lighting_default(), dst));
    for (const int i : IndexRange(8 * 8 * 4)) {
      EXPECT_TRUE(std::isfinite(dst->float_data()[i]));
    }
    IMB_freeImBuf(dst);
  }
}

TEST(paint_material_combined, mixed_rig_keeps_directional_light_unchanged)
{
  /* One directional light, one finite light with a negligible colour: the result must be the
   * directional light's own, so `distance = 0` really ignores the plane. */
  CombinedInputs inputs = test_default_inputs(8);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);

  CombinedPreviewLighting directional = test_single_light();
  ImBuf *expected = test_float_ibuf(8);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, directional, expected));

  CombinedPreviewLighting mixed = directional;
  mixed.lights_num = 2;
  mixed.lights[1].direction = math::normalize(float3(0.5f, 0.5f, 1.0f));
  mixed.lights[1].distance = 0.6f;
  mixed.lights[1].diffuse_color = float3(1e-6f);
  mixed.lights[1].specular_color = float3(1e-6f);
  ImBuf *actual = test_float_ibuf(8);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, mixed, actual));

  for (const int i : IndexRange(8 * 8 * 4)) {
    EXPECT_NEAR(actual->float_data()[i], expected->float_data()[i], 1e-4f);
  }
  IMB_freeImBuf(expected);
  IMB_freeImBuf(actual);
}

TEST(paint_material_combined, finite_light_keeps_canvas_symmetry)
{
  /* A single light straight down `+Z` above the canvas centre: on a non-square canvas the plane
   * must mirror around x. */
  CombinedInputs inputs = test_default_inputs(8);
  inputs.width = 8;
  inputs.height = 4;
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(0.5f);

  CombinedPreviewLighting lighting = test_single_light();
  lighting.lights[0].distance = 0.6f;
  ImBuf *dst = IMB_allocImBuf(8, 4, ImBufFlags::FloatData);
  dst->channels = 4;
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, lighting, dst));
  for (const int y : IndexRange(4)) {
    for (const int x : IndexRange(4)) {
      EXPECT_NEAR(test_pixel_luminance(dst, x, y), test_pixel_luminance(dst, 7 - x, y), 1e-4f);
    }
  }
  IMB_freeImBuf(dst);
}

TEST(paint_material_combined, finite_light_matches_hand_computed_texel)
{
  /* 4x4 canvas, one finite light. Texel (1, 0) is the bottom row, so its plane point is
   * (-0.125, -0.375, 0): with a normalized direction (0.5, 0.5, 1) * 0.6 the light direction is
   * normalize(0.36994897, 0.61994897, 0.48989795), and NdotL is 0.561500. Base 0.5 with no
   * ambient, specular or wrap gives 0.5 * 0.561500 = 0.280750. A y-sign flip would give
   * 0.390347. */
  CombinedInputs inputs = test_default_inputs(4);
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].constant = float4(0.5f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);

  CombinedPreviewLighting lighting = test_single_light();
  lighting.lights[0].direction = math::normalize(float3(0.5f, 0.5f, 1.0f));
  lighting.lights[0].distance = 0.6f;

  ImBuf *dst = test_float_ibuf(4);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, lighting, dst));
  const float *p = dst->float_data() + (int64_t(0) * 4 + 1) * 4;
  EXPECT_NEAR(p[0], 0.280750f, 1e-4f);
  IMB_freeImBuf(dst);
}

TEST(paint_material_combined, negative_distance_is_directional)
{
  /* A negative distance is not a light below the plane: it is the same light as zero. */
  CombinedInputs inputs = test_default_inputs(8);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(0.4f);

  CombinedPreviewLighting zeroed = test_single_light();
  zeroed.lights[0].distance = 0.0f;
  CombinedPreviewLighting negative = zeroed;
  negative.lights[0].distance = -0.5f;

  /* The hash folds the normalized distance, so the two rigs are indistinguishable to the cache. */
  EXPECT_EQ(negative.hash(), zeroed.hash());

  ImBuf *a = test_float_ibuf(8);
  ImBuf *b = test_float_ibuf(8);
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, zeroed, a));
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, negative, b));
  for (const int64_t i : IndexRange(int64_t(8) * 8 * 4)) {
    EXPECT_FLOAT_EQ(a->float_data()[i], b->float_data()[i]);
  }
  IMB_freeImBuf(a);
  IMB_freeImBuf(b);
}

TEST(paint_material_combined, finite_light_y_scale_on_a_tall_canvas)
{
  /* 4x8 canvas: the plane extent is (0.5, 1), so the shorter X axis spans 0.5 and Y spans 1. For
   * texel (0, 0) the plane point is (-0.1875, -0.4375, 0): with the direction (0, 1, 1) * 0.6 the
   * light sits at (0, 0.42426407, 0.42426407), and NdotL is 0.433511. Base 0.5 with no ambient
   * gives 0.5 * 0.433511 = 0.216756. A sign-flipped Y would read 0.457143. */
  CombinedInputs inputs = test_default_inputs(8);
  inputs.width = 4;
  inputs.height = 8;
  inputs.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].constant = float4(0.5f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].constant = float4(1.0f);
  inputs.channels[PAINT_MATERIAL_CHANNEL_SPECULAR].constant = float4(0.0f);

  CombinedPreviewLighting lighting = test_single_light();
  lighting.lights[0].direction = math::normalize(float3(0.0f, 1.0f, 1.0f));
  lighting.lights[0].distance = 0.6f;

  ImBuf *dst = IMB_allocImBuf(4, 8, ImBufFlags::FloatData);
  dst->channels = 4;
  ASSERT_TRUE(BKE_paint_material_combined_eval(inputs, lighting, dst));
  const float *p = dst->float_data() + (int64_t(0) * 4 + 0) * 4;
  EXPECT_NEAR(p[0], 0.216756f, 1e-4f);
  IMB_freeImBuf(dst);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Output Resolution
 * \{ */

/* A reduced output shades its own texel count, not the input's. */
TEST(paint_material_combined, output_resolution_shades_the_output)
{
  CombinedInputs inputs = test_default_inputs(64);
  inputs.output_width = 16;
  inputs.output_height = 16;
  ImBuf *dst = test_float_ibuf(16);

  CombinedEvalStats stats;
  EXPECT_TRUE(BKE_paint_material_combined_eval(inputs, test_single_light(), dst, nullptr, &stats));
  EXPECT_EQ(stats.pixels_processed, 16 * 16);
  IMB_freeImBuf(dst);
}

/* A destination that disagrees with the declared output size is still a caller bug. */
TEST(paint_material_combined, output_resolution_mismatch_is_rejected)
{
  CombinedInputs inputs = test_default_inputs(64);
  inputs.output_width = 16;
  inputs.output_height = 16;
  ImBuf *dst = test_float_ibuf(64);
  EXPECT_FALSE(
      BKE_paint_material_combined_eval(inputs, test_single_light(), dst, nullptr, nullptr));
  IMB_freeImBuf(dst);
}

/* Reducing resolution must not move the image: a constant input shades to the same colour at any
 * output size, which is the cheapest check that the sampling is centred rather than skewed. */
TEST(paint_material_combined, output_resolution_preserves_a_constant)
{
  const CombinedPreviewLighting lighting = test_single_light();

  CombinedInputs full = test_default_inputs(64);
  ImBuf *dst_full = test_float_ibuf(64);
  ASSERT_TRUE(BKE_paint_material_combined_eval(full, lighting, dst_full, nullptr, nullptr));

  CombinedInputs reduced = test_default_inputs(64);
  reduced.output_width = 16;
  reduced.output_height = 16;
  ImBuf *dst_reduced = test_float_ibuf(16);
  ASSERT_TRUE(BKE_paint_material_combined_eval(reduced, lighting, dst_reduced, nullptr, nullptr));

  const float *a = dst_full->float_data();
  const float *b = dst_reduced->float_data();
  for (const int i : IndexRange(4)) {
    EXPECT_NEAR(a[i], b[i], 1e-5f);
  }
  IMB_freeImBuf(dst_full);
  IMB_freeImBuf(dst_reduced);
}

/* A region is in output coordinates, so it bounds output pixels rather than canvas texels. */
TEST(paint_material_combined, output_resolution_region_is_in_output_coordinates)
{
  CombinedInputs inputs = test_default_inputs(64);
  inputs.output_width = 16;
  inputs.output_height = 16;
  ImBuf *dst = test_float_ibuf(16);

  rcti region;
  BLI_rcti_init(&region, 0, 4, 0, 4);
  CombinedEvalStats stats;
  EXPECT_TRUE(
      BKE_paint_material_combined_eval(inputs, test_single_light(), dst, &region, &stats));
  EXPECT_EQ(stats.pixels_processed, 4 * 4);
  IMB_freeImBuf(dst);
}

/** \} */

}  // namespace blender::bke::tests
