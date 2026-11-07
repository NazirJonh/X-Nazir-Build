/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <memory>

#include "MEM_guardedalloc.h"

#include "BKE_brush.hh"
#include "BKE_brush_material_correction.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_texture.h"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_enums.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"
#include "DNA_vec_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "../mesh/paint_material_correction_cache.hh"
#include "../mesh/paint_material_source.hh"

namespace blender::ed::sculpt_paint {

namespace {

class CorrectionTestSampler {
 public:
  Paint paint{};
  material::ChannelSourceSet sources;
  material::ChannelUvSampler sampler;

  CorrectionTestSampler(Brush &brush,
                        const PaintModeSettings &settings,
                        const bool resolve_mask_source)
      : sources(*brush.material_paint,
                settings,
                PAINT_MATERIAL_CHANNELS_VISIBLE_ALL,
                resolve_mask_source),
        sampler(sources, *brush.material_paint, settings, paint, brush)
  {
    /* The mask/shape paths place the texture themselves, exactly like the production samplers. */
    sampler.set_unit_uv_space(true);
  }
};

BrushMaterialPaintCorrection &ensure_correction(BrushMaterialPaint &brush_paint,
                                                const eMaterialPaintChannel channel)
{
  BrushMaterialPaintCorrection *correction =
      BKE_brush_material_paint_correction_ensure(brush_paint.channels[channel]);
  correction->flag |= BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED;
  return *correction;
}

/** The DNA bits are storage, not API: tests write and read them through these two. */
void set_correction_flag(BrushMaterialPaintCorrection &correction,
                         const eBrushMaterialPaintCorrectionFlag flag,
                         const bool value)
{
  SET_FLAG_FROM_TEST(correction.flag, value, flag);
}

bool correction_flag(const BrushMaterialPaintCorrection &correction,
                     const eBrushMaterialPaintCorrectionFlag flag)
{
  return (correction.flag & flag) != 0;
}

}  // namespace

class PaintMaterialSourceCorrectionTest : public bke::BlenderGTestBase {
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

  Image *add_image(const char *name,
                   const bool floatbuf,
                   const bool is_data,
                   const float color[4])
  {
    return BKE_image_add_generated(bmain,
                                   4,
                                   4,
                                   name,
                                   32,
                                   floatbuf,
                                   IMA_GENTYPE_BLANK,
                                   color,
                                   /*stereo3d=*/false,
                                   is_data,
                                   /*tiled=*/false);
  }

  Tex *add_source_tex(Image *image, const char *name)
  {
    Tex *tex = BKE_texture_add(bmain, name);
    tex->type = TEX_IMAGE;
    tex->ima = image;
    id_us_plus(&image->id);
    tex->extend = TEX_EXTEND;
    tex->imaflag |= TEX_INTERPOL;
    return tex;
  }

  void attach_source(Brush &brush, const eMaterialPaintChannel channel, Tex &tex)
  {
    BrushMaterialPaintChannel &channel_data = brush.material_paint->channels[channel];
    channel_data.use = 1;
    channel_data.source_mtex.tex = &tex;
    id_us_plus(&tex.id);
  }

  Brush *make_brush(const char *name)
  {
    Brush *brush = BKE_brush_add(bmain, name, OB_MODE_SCULPT);
    BKE_brush_material_paint_ensure(brush);
    brush->material_paint->source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MAPS;
    return brush;
  }
};

TEST_F(PaintMaterialSourceCorrectionTest, AlphaMaskCoverageAppliesScalarCorrection)
{
  Brush *brush = make_brush("CorrectionAlphaMask");
  BrushMaterialPaint &brush_paint = *brush->material_paint;
  brush_paint.mask_source = BRUSH_MATERIAL_PAINT_MASK_SOURCE_ALPHA;
  const float color[4] = {0.0f, 0.0f, 0.0f, 0.25f};
  Image *image = add_image("CorrectionAlphaMaskImage", true, /*is_data=*/true, color);
  Tex *tex = add_source_tex(image, "CorrectionAlphaMaskTex");
  attach_source(*brush, PAINT_MATERIAL_CHANNEL_ALPHA, *tex);

  PaintModeSettings settings{};
  settings.layer_target_mode = PAINT_LAYER_TARGET_MASK;
  CorrectionTestSampler env(*brush, settings, /*resolve_mask_source=*/true);
  const float2 uv(0.5f, 0.5f);
  EXPECT_NEAR(material::mask_source_coverage_at_uv(env.sampler, brush_paint, uv, 0),
              0.25f,
              1e-4f);

  /* A stroke resolves its channel set - the correction among it - at its start, so every changed
   * correction below is read by a rebuilt set, exactly what the next stroke does. */
  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_ALPHA);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);
  CorrectionTestSampler inverted(*brush, settings, /*resolve_mask_source=*/true);
  EXPECT_NEAR(material::mask_source_coverage_at_uv(inverted.sampler, brush_paint, uv, 0),
              0.75f,
              1e-4f);

  /* The Mask Source's invert mirrors the corrected coverage, once, after the correction. Unlike
   * the resolved correction it is read live: it is a stroke flag, not part of the set. */
  brush_paint.mask_source_invert = 1;
  EXPECT_NEAR(material::mask_source_coverage_at_uv(inverted.sampler, brush_paint, uv, 0),
              0.25f,
              1e-4f);

  brush_paint.mask_source_invert = 0;
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, false);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_LEVELS, true);
  correction.levels_in_white = 0.5f;
  CorrectionTestSampler levels(*brush, settings, /*resolve_mask_source=*/true);
  EXPECT_NEAR(material::mask_source_coverage_at_uv(levels.sampler, brush_paint, uv, 0),
              0.5f,
              1e-4f);

  /* An allocated but disabled correction must not reroute the raw coverage. */
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED, false);
  CorrectionTestSampler disabled(*brush, settings, /*resolve_mask_source=*/true);
  EXPECT_NEAR(material::mask_source_coverage_at_uv(disabled.sampler, brush_paint, uv, 0),
              0.25f,
              1e-4f);
}

TEST_F(PaintMaterialSourceCorrectionTest, BaseColorMaskCoverageUsesCorrectedLuminance)
{
  Brush *brush = make_brush("CorrectionColorMask");
  BrushMaterialPaint &brush_paint = *brush->material_paint;
  brush_paint.mask_source = BRUSH_MATERIAL_PAINT_MASK_SOURCE_BASE_COLOR;
  const float color[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  Image *image = add_image("CorrectionColorMaskImage", true, /*is_data=*/true, color);
  Tex *tex = add_source_tex(image, "CorrectionColorMaskTex");
  attach_source(*brush, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *tex);

  PaintModeSettings settings{};
  settings.layer_target_mode = PAINT_LAYER_TARGET_MASK;
  CorrectionTestSampler env(*brush, settings, /*resolve_mask_source=*/true);
  const float2 uv(0.5f, 0.5f);
  EXPECT_NEAR(material::mask_source_coverage_at_uv(env.sampler, brush_paint, uv, 0),
              0.25f,
              1e-4f);

  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);
  /* The stroke's set is resolved at its start; the corrected read is the next stroke's. */
  CorrectionTestSampler corrected(*brush, settings, /*resolve_mask_source=*/true);
  EXPECT_NEAR(material::mask_source_coverage_at_uv(corrected.sampler, brush_paint, uv, 0),
              0.75f,
              1e-4f);
}

TEST_F(PaintMaterialSourceCorrectionTest, DeferredDecodeDefersTheCorrection)
{
  Brush *brush = make_brush("CorrectionDeferredDecode");
  BrushMaterialPaint &brush_paint = *brush->material_paint;
  const float color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  Image *image = add_image("CorrectionDeferredImage", /*floatbuf=*/false, /*is_data=*/false, color);
  Tex *tex = add_source_tex(image, "CorrectionDeferredTex");
  attach_source(*brush, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *tex);
  /* A crop disqualifies the direct path (see #channel_source_image_direct_ok), so the source falls
   * back to the texture engine: exactly the on-the-fly correction the deferred decode is about. A
   * directly sampled source with a correction bakes it into its buffer instead, and has nothing
   * left to defer. The image is a flat color, so the crop does not change any sampled value. */
  tex->cropxmax = 0.9f;

  PaintModeSettings settings{};
  const float2 uv(0.5f, 0.5f);
  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);
  CorrectionTestSampler env(*brush, settings, /*resolve_mask_source=*/false);

  /* A deferred sample must stay raw: correcting it before the caller's colorspace decode would
   * run Levels/Invert in the stored encoding. */
  const float3 deferred = env.sampler.color_at_uv(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, uv, 0, /*decode_linear=*/false);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED, false);
  CorrectionTestSampler off(*brush, settings, /*resolve_mask_source=*/false);
  const float3 raw = off.sampler.color_at_uv(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, uv, 0, /*decode_linear=*/false);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED, true);
  EXPECT_V3_NEAR(deferred, raw, 1e-6f);

  /* Decoding first and then correcting must match the sampler's own decoded overload. */
  float4 decoded(raw.x, raw.y, raw.z, 1.0f);
  IMB_colormanagement_colorspace_to_scene_linear_v3(
      decoded, env.sampler.colorspace(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  float expected[3] = {decoded.x, decoded.y, decoded.z};
  BKE_brush_material_paint_correction_apply_rgb(correction, expected);
  const float3 sampled_decoded = env.sampler.color_at_uv(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, uv, 0, /*decode_linear=*/true);
  EXPECT_V3_NEAR(sampled_decoded, float3(expected[0], expected[1], expected[2]), 1e-5f);

  /* The batched helper, used by the deferred stroke paths, lands on the same color: it applies the
   * prepared constants the stroke resolved at start (#ChannelSourceSet::ChannelSource). */
  Array<float3> batch(1);
  batch[0] = float3(decoded.x, decoded.y, decoded.z);
  material::ChannelSourceSampler::apply_correction_batch(
      batch, env.sources.source(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_V3_NEAR(batch[0], sampled_decoded, 1e-6f);
}

TEST(PaintMaterialSourceCorrection, scalar_finish_gates_by_source_mode_and_normal)
{
  BrushMaterialPaint brush_paint;
  brush_paint.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MAPS;
  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_METALLIC);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);
  EXPECT_NEAR(material::scalar_source_finish(brush_paint,
                                             /*source=*/nullptr,
                                             PAINT_MATERIAL_CHANNEL_METALLIC,
                                             float2(0.0f, 1.0f),
                                             0.25f),
              0.75f,
              1e-6f);

  /* Material/Layer bakes are not "Correction Image" consumers. */
  brush_paint.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL;
  EXPECT_NEAR(material::scalar_source_finish(brush_paint,
                                             /*source=*/nullptr,
                                             PAINT_MATERIAL_CHANNEL_METALLIC,
                                             float2(0.0f, 1.0f),
                                             0.25f),
              0.25f,
              1e-6f);

  /* And the tangent-space Normal map never takes a correction. */
  brush_paint.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MAPS;
  BrushMaterialPaintCorrection &normal_correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_NORMAL);
  set_correction_flag(normal_correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);
  EXPECT_NEAR(material::scalar_source_finish(brush_paint,
                                             /*source=*/nullptr,
                                             PAINT_MATERIAL_CHANNEL_NORMAL,
                                             float2(-1.0f, 1.0f),
                                             0.5f),
              0.5f,
              1e-6f);
}

TEST(PaintMaterialSourceCorrection, for_channel_gates_by_mode_normal_and_enabled)
{
  BrushMaterialPaint brush_paint;
  brush_paint.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MAPS;
  EXPECT_EQ(BKE_brush_material_paint_correction_for_channel(brush_paint,
                                                            PAINT_MATERIAL_CHANNEL_METALLIC),
            nullptr)
      << "a channel without a correction must answer null";

  BrushMaterialPaintCorrection &correction = ensure_correction(brush_paint,
                                                              PAINT_MATERIAL_CHANNEL_METALLIC);
  EXPECT_EQ(BKE_brush_material_paint_correction_for_channel(brush_paint,
                                                            PAINT_MATERIAL_CHANNEL_METALLIC),
            &correction);

  /* The tangent-space Normal map never takes a correction. */
  EXPECT_EQ(BKE_brush_material_paint_correction_for_channel(brush_paint,
                                                            PAINT_MATERIAL_CHANNEL_NORMAL),
            nullptr);

  /* Switched off: the parameters stay, the answer does not. */
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED, false);
  EXPECT_EQ(BKE_brush_material_paint_correction_for_channel(brush_paint,
                                                            PAINT_MATERIAL_CHANNEL_METALLIC),
            nullptr);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED, true);

  /* Material/Layer sources are not "Correction Image" consumers. */
  brush_paint.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL;
  EXPECT_EQ(BKE_brush_material_paint_correction_for_channel(brush_paint,
                                                            PAINT_MATERIAL_CHANNEL_METALLIC),
            nullptr);

  BKE_brush_material_paint_correction_delete(brush_paint.channels[PAINT_MATERIAL_CHANNEL_METALLIC]);
}

TEST(PaintMaterialSourceCorrection, hash_is_zero_while_disabled)
{
  BrushMaterialPaintCorrection correction;
  EXPECT_EQ(BKE_brush_material_paint_correction_hash(correction), 0u)
      << "an off correction changes nothing, so it must not invalidate anything";

  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED, true);
  const uint64_t enabled = BKE_brush_material_paint_correction_hash(correction);
  EXPECT_NE(enabled, 0u);

  correction.levels_in_black = 0.25f;
  EXPECT_NE(BKE_brush_material_paint_correction_hash(correction), enabled)
      << "every parameter must move the hash";

  correction.levels_in_black = 0.0f;
  EXPECT_EQ(BKE_brush_material_paint_correction_hash(correction), enabled)
      << "the same parameters must hash the same";
}

TEST(PaintMaterialSourceCorrection, corrections_hash_mixes_the_channel)
{
  BrushMaterialPaint metallic;
  metallic.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MAPS;
  set_correction_flag(ensure_correction(metallic, PAINT_MATERIAL_CHANNEL_METALLIC),
                      BRUSH_MATERIAL_PAINT_CORRECTION_INVERT,
                      true);

  BrushMaterialPaint roughness;
  roughness.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MAPS;
  set_correction_flag(ensure_correction(roughness, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
                      BRUSH_MATERIAL_PAINT_CORRECTION_INVERT,
                      true);

  EXPECT_NE(BKE_brush_material_paint_corrections_hash(metallic),
            BKE_brush_material_paint_corrections_hash(roughness))
      << "moving a correction to another channel changes what a stroke paints";

  /* Only active, applicable corrections take part: Material mode ignores them altogether. */
  roughness.source_mode = BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL;
  EXPECT_EQ(BKE_brush_material_paint_corrections_hash(roughness),
            BKE_brush_material_paint_corrections_hash(BrushMaterialPaint{}));

  BKE_brush_material_paint_correction_delete(metallic.channels[PAINT_MATERIAL_CHANNEL_METALLIC]);
  BKE_brush_material_paint_correction_delete(
      roughness.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS]);
}

TEST(PaintMaterialSourceCorrection, apply_rgb_batch_matches_single_apply)
{
  BrushMaterialPaintCorrection correction;
  correction.flag = BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED |
                    BRUSH_MATERIAL_PAINT_CORRECTION_LEVELS |
                    BRUSH_MATERIAL_PAINT_CORRECTION_HSV | BRUSH_MATERIAL_PAINT_CORRECTION_INVERT;
  correction.levels_in_black = 0.1f;
  correction.levels_in_white = 0.9f;
  correction.levels_gamma = 0.8f;
  correction.levels_out_white = 0.95f;
  correction.hsv_hue = 0.6f;
  correction.hsv_saturation = 1.3f;
  correction.hsv_value = 0.9f;

  Array<float3> colors = {float3(0.0f, 0.5f, 1.0f),
                          float3(1.0f, 1.0f, 1.0f),
                          float3(0.2f, 0.05f, 0.7f),
                          float3(-0.1f, 1.4f, 0.3f)};
  Array<float3> expected(colors.size());
  for (const int i : colors.index_range()) {
    float rgb[3] = {colors[i].x, colors[i].y, colors[i].z};
    BKE_brush_material_paint_correction_apply_rgb(correction, rgb);
    expected[i] = float3(rgb[0], rgb[1], rgb[2]);
  }

  BKE_brush_material_paint_correction_apply_rgb_batch(correction, colors);
  for (const int i : colors.index_range()) {
    EXPECT_V3_NEAR(colors[i], expected[i], 1e-7f);
  }
}

TEST(PaintMaterialSourceCorrection, copy_into_splits_a_shared_correction)
{
  BrushMaterialPaint dst;
  BrushMaterialPaint src;
  BrushMaterialPaintCorrection *shared = MEM_new<BrushMaterialPaintCorrection>(__func__);
  shared->flag = BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED |
                 BRUSH_MATERIAL_PAINT_CORRECTION_LEVELS;
  shared->levels_in_black = 0.25f;
  src.channels[PAINT_MATERIAL_CHANNEL_METALLIC].correction = shared;
  /* Copies made before corrections became deep-copied share one block. */
  dst.channels[PAINT_MATERIAL_CHANNEL_METALLIC].correction = shared;

  BKE_brush_material_paint_copy_into(dst, src);
  /* copy_into took ownership of the shared block while copying, so src must not be read through
   * it any more. */
  src.channels[PAINT_MATERIAL_CHANNEL_METALLIC].correction = nullptr;

  BrushMaterialPaintCorrection *copied = dst.channels[PAINT_MATERIAL_CHANNEL_METALLIC].correction;
  ASSERT_NE(copied, nullptr);
  EXPECT_NE(copied, shared);
  EXPECT_FLOAT_EQ(copied->levels_in_black, 0.25f);
  EXPECT_TRUE(correction_flag(*copied, BRUSH_MATERIAL_PAINT_CORRECTION_LEVELS));
  MEM_delete(copied);
}

TEST_F(PaintMaterialSourceCorrectionTest, CorrectionCacheHitsUntilParametersChange)
{
  Brush *brush = make_brush("CorrectionCacheHits");
  BrushMaterialPaint &brush_paint = *brush->material_paint;
  const float color[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  Image *image = add_image("CorrectionCacheHitsImage", /*floatbuf=*/true, /*is_data=*/true, color);

  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);

  ImBuf *source = BKE_image_acquire_ibuf(image, nullptr, nullptr);
  ASSERT_NE(source, nullptr);
  ASSERT_NE(source->float_data(), nullptr);

  /* The counters are session-wide, so every expectation is a delta. */
  const material::CorrectionCacheStats start = material::correction_cache_stats();
  material::correction_cache_clear();
  const material::CorrectionCacheRequest request{brush_paint,
                                                 correction,
                                                 PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                 *image,
                                                 *source,
                                                 /*decode_colorspace=*/nullptr,
                                                 material::CorrectionSourceKey{},
                                                 float2(0.0f, 1.0f)};

  const std::shared_ptr<const ImBuf> first = material::correction_cache_get(request);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(material::correction_cache_stats().builds, start.builds + 1);
  /* Inverted, in scene-linear floats. */
  EXPECT_NEAR(first->float_data()[0], 0.75f, 1e-5f);

  const std::shared_ptr<const ImBuf> second = material::correction_cache_get(request);
  EXPECT_EQ(second.get(), first.get()) << "an unchanged request must reuse the buffer";
  EXPECT_EQ(material::correction_cache_stats().builds, start.builds + 1);
  EXPECT_EQ(material::correction_cache_stats().hits, start.hits + 1);

  /* Any parameter change is a different key. */
  correction.levels_in_black = 0.2f;
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_LEVELS, true);
  EXPECT_NE(material::correction_cache_get(request), nullptr);
  EXPECT_EQ(material::correction_cache_stats().builds, start.builds + 2);

  /* Dropping every entry only rebuilds. */
  material::correction_cache_clear();
  EXPECT_EQ(material::correction_cache_stats().entries, 0u);
  EXPECT_NE(material::correction_cache_get(request), nullptr);
  EXPECT_EQ(material::correction_cache_stats().builds, start.builds + 3);

  material::correction_cache_clear();
  BKE_image_release_ibuf(image, source, nullptr);
}

/**
 * A paint stroke on the source image itself must re-correct the edited texels from the new pixels,
 * not correct the already corrected buffer a second time.
 */
TEST_F(PaintMaterialSourceCorrectionTest, CorrectionCacheRegionRefreshStartsFromTheSourcePixels)
{
  Brush *brush = make_brush("CorrectionRegionRefresh");
  BrushMaterialPaint &brush_paint = *brush->material_paint;
  const float color[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  Image *image = add_image(
      "CorrectionRegionRefreshImage", /*floatbuf=*/true, /*is_data=*/true, color);

  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);

  ImBuf *source = BKE_image_acquire_ibuf(image, nullptr, nullptr);
  ASSERT_NE(source, nullptr);
  ASSERT_NE(source->float_data(), nullptr);

  /* Prime the journal's idea of the buffer size: the first mark on a fresh image counts as a
   * resize, and a resize always means "everything changed". */
  rcti empty_rect{0, 0, 0, 0};
  BKE_image_partial_update_mark_region(image, nullptr, source, &empty_rect);

  material::correction_cache_clear();
  const material::CorrectionCacheStats start = material::correction_cache_stats();
  const material::CorrectionCacheRequest request{brush_paint,
                                                 correction,
                                                 PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                 *image,
                                                 *source,
                                                 /*decode_colorspace=*/nullptr,
                                                 material::CorrectionSourceKey{},
                                                 float2(0.0f, 1.0f)};

  std::shared_ptr<const ImBuf> first = material::correction_cache_get(request);
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(material::correction_cache_stats().builds, start.builds + 1);
  EXPECT_NEAR(first->float_data()[0], 0.75f, 1e-5f);
  const ImBuf *first_buffer = first.get();
  /* Drop the test's own reference: while anyone else holds the buffer the cache rebuilds rather
   * than changing the texels under them, which is what a running stroke relies on. */
  first.reset();

  /* Paint the top-left 2x2 texels to 0.5 and mark exactly that rectangle. */
  float *pixels = source->float_data_for_write();
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      float *texel = pixels + (int64_t(y) * source->x + x) * 4;
      texel[0] = 0.5f;
      texel[1] = 0.5f;
      texel[2] = 0.5f;
    }
  }
  rcti painted_rect{0, 2, 0, 2};
  BKE_image_partial_update_mark_region(image, nullptr, source, &painted_rect);

  const std::shared_ptr<const ImBuf> refreshed = material::correction_cache_get(request);
  ASSERT_NE(refreshed, nullptr);
  EXPECT_EQ(refreshed.get(), first_buffer)
      << "a region edit refreshes the buffer, it does not replace it";
  EXPECT_EQ(material::correction_cache_stats().builds, start.builds + 1)
      << "no full rebuild for a region edit";
  EXPECT_EQ(material::correction_cache_stats().region_updates, start.region_updates + 1);

  const float *after = refreshed->float_data();
  /* Inside: invert(0.5) - correcting the old 0.75 again would give 0.25. */
  EXPECT_NEAR(after[0], 0.5f, 1e-5f);
  /* Outside: still the untouched invert(0.25). */
  const int64_t outside = int64_t(3) * refreshed->x + 3;
  EXPECT_NEAR(after[outside * 4], 0.75f, 1e-5f);

  material::correction_cache_clear();
  BKE_image_release_ibuf(image, source, nullptr);
}

/**
 * Baked or not, a channel has to describe the pooled source image's encoding the same way: the
 * shape texture reads the pooled buffer and decodes it by these two fields.
 */
TEST_F(PaintMaterialSourceCorrectionTest, CorrectionBakeKeepsThePooledSourceEncoding)
{
  Brush *brush = make_brush("CorrectionPooledEncoding");
  BrushMaterialPaint &brush_paint = *brush->material_paint;
  const float color[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  Image *image = add_image(
      "CorrectionPooledEncodingImage", /*floatbuf=*/false, /*is_data=*/false, color);
  Tex *tex = add_source_tex(image, "CorrectionPooledEncodingTex");
  attach_source(*brush, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *tex);

  PaintModeSettings settings{};
  CorrectionTestSampler plain(*brush, settings, /*resolve_mask_source=*/false);
  const auto &before = plain.sources.source(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(before.image_direct_sample);
  ASSERT_TRUE(before.pooled_do_linear_conversion)
      << "an sRGB byte source needs the stroke's own decode";
  ASSERT_NE(before.pooled_colorspace, nullptr);

  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_correction_flag(correction, BRUSH_MATERIAL_PAINT_CORRECTION_INVERT, true);

  CorrectionTestSampler baked(*brush, settings, /*resolve_mask_source=*/false);
  const auto &after = baked.sources.source(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(after.correction_baked) << "an enabled correction on a direct source is baked";
  /* The corrected copy is scene-linear, so the sampler has nothing left to decode... */
  EXPECT_FALSE(after.do_linear_conversion);
  EXPECT_EQ(after.colorspace, nullptr);
  /* ...while the pooled source keeps its own encoding for anyone reading it directly. */
  EXPECT_TRUE(after.pooled_do_linear_conversion);
  EXPECT_EQ(after.pooled_colorspace, before.pooled_colorspace);
  EXPECT_EQ(after.pooled_ibuf, before.pooled_ibuf);
}

/**
 * A scalar channel reads an intensity, which no path colorspace-decodes. The cache must therefore
 * store the same numbers the on-the-fly path produces, even for an sRGB-tagged source.
 */
TEST_F(PaintMaterialSourceCorrectionTest, CorrectionCacheLeavesAScalarChannelUndecoded)
{
  Brush *brush = make_brush("CorrectionScalarDecode");
  BrushMaterialPaint &brush_paint = *brush->material_paint;
  const float color[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  Image *image = add_image(
      "CorrectionScalarDecodeImage", /*floatbuf=*/false, /*is_data=*/false, color);
  Tex *tex = add_source_tex(image, "CorrectionScalarDecodeTex");
  attach_source(*brush, PAINT_MATERIAL_CHANNEL_METALLIC, *tex);

  PaintModeSettings settings{};
  const float2 uv(0.5f, 0.5f);

  /* Reference: the on-the-fly path, before any correction exists. */
  CorrectionTestSampler plain(*brush, settings, /*resolve_mask_source=*/false);
  ASSERT_FALSE(plain.sources.source(PAINT_MATERIAL_CHANNEL_METALLIC).correction_baked);
  const float reference = plain.sampler.scalar_at_uv(PAINT_MATERIAL_CHANNEL_METALLIC, uv, 0);

  /* An enabled correction with no stage switched on changes nothing, so the cached answer has to
   * match the reference exactly; a colorspace decode baked into the cache would not. */
  BrushMaterialPaintCorrection &correction = ensure_correction(
      brush_paint, PAINT_MATERIAL_CHANNEL_METALLIC);
  EXPECT_EQ(correction.flag, short(BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED));

  CorrectionTestSampler baked(*brush, settings, /*resolve_mask_source=*/false);
  ASSERT_TRUE(baked.sources.source(PAINT_MATERIAL_CHANNEL_METALLIC).correction_baked);
  EXPECT_NEAR(baked.sampler.scalar_at_uv(PAINT_MATERIAL_CHANNEL_METALLIC, uv, 0), reference, 1e-5f);
}

}  // namespace blender::ed::sculpt_paint
