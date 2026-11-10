/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_mask_coverage_cache.hh"

#include "paint_image_2d_mask_coverage.hh"

#include "ED_paint.hh"

#include <algorithm>
#include <cmath>

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_interp.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"

#include "BKE_paint.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::material {

/* -------------------------------------------------------------------- */
/** \name Stride-1 bilinear
 * \{ */

namespace {

/** A small random RGBA buffer, HDR on purpose (luminance can exceed 1). */
Array<float> random_rgba_buffer(const int width, const int height, RandomNumberGenerator &rng)
{
  Array<float> pixels(int64_t(width) * height * 4);
  for (float &v : pixels) {
    v = rng.get_float() * 2.0f;
  }
  return pixels;
}

/** The per-texel reduction the Alpha extraction uses, as #sample_layout_image applies it. */
float rgba_luminance(const float *texel)
{
  return IMB_colormanagement_get_luminance(texel);
}

/** Per-texel luminance of an RGBA buffer, stride-4 in, stride-1 out. */
Array<float> luminance_channel_buffer(const Span<float> rgba, const int width, const int height)
{
  Array<float> buffer(int64_t(width) * height);
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      buffer[int64_t(y) * width + x] = rgba_luminance(
          rgba.data() + (int64_t(y) * width + x) * 4);
    }
  }
  return buffer;
}

}  // namespace

TEST(MaskCoverageBilinear, MatchesLuminanceOfRgbBilinear)
{
  /* The cache's premise: the reduction is linear in the texel color, so a bilinear over the
   * per-texel greys equals the grey of the stride-4 bilinear - sampling the cached buffer cannot
   * drift from sampling the image itself. */
  const int width = 16;
  const int height = 12;
  RandomNumberGenerator rng(1234);
  const Array<float> rgba = random_rgba_buffer(width, height, rng);
  const Array<float> channel = luminance_channel_buffer(rgba, width, height);

  for (const int i : IndexRange(64)) {
    const float u = rng.get_float() * width;
    const float v = rng.get_float() * height;
    const float4 rgb_sample = math::interpolate_bilinear_fl(
        rgba.data(), width, height, u, v);
    const float expected = rgba_luminance(rgb_sample);
    const float cached = interpolate_bilinear_channel_fl(
        channel.data(), width, height, u, v, false);
    EXPECT_NEAR(cached, expected, 1e-5f);
  }
}

TEST(MaskCoverageBilinear, MatchesLuminanceOfRgbBilinearWrapped)
{
  const int width = 8;
  const int height = 6;
  RandomNumberGenerator rng(42);
  const Array<float> rgba = random_rgba_buffer(width, height, rng);
  const Array<float> channel = luminance_channel_buffer(rgba, width, height);

  for (const int i : IndexRange(64)) {
    /* Coordinates pushed past the edges exercise the wrap, like a Tiled / repeated source. */
    const float u = (rng.get_float() * 2.0f - 0.5f) * width;
    const float v = (rng.get_float() * 2.0f - 0.5f) * height;
    const float4 rgb_sample = math::interpolate_bilinear_wrap_fl(
        rgba.data(), width, height, u, v);
    const float expected = rgba_luminance(rgb_sample);
    const float cached = interpolate_bilinear_channel_fl(
        channel.data(), width, height, u, v, true);
    EXPECT_NEAR(cached, expected, 1e-5f);
  }
}

TEST(MaskCoverageBilinear, ExtendClampsToEdges)
{
  /* Two-by-two with distinct texels: outside the image, the extended sampler reads the edge. */
  const int width = 2;
  const int height = 2;
  const float pixels[4] = {0.1f, 0.3f, 0.5f, 0.7f};
  EXPECT_NEAR(interpolate_bilinear_channel_fl(pixels, width, height, -0.5f, -0.5f, false),
              pixels[0],
              1e-6f);
  EXPECT_NEAR(interpolate_bilinear_channel_fl(pixels, width, height, 2.0f, 2.0f, false),
              pixels[3],
              1e-6f);
  /* A repeat sample at exactly one image away reads the same texels again. */
  EXPECT_NEAR(interpolate_bilinear_channel_fl(pixels, width, height, 0.5f, 0.5f, true),
              interpolate_bilinear_channel_fl(pixels, width, height, 2.5f, 0.5f, true),
              1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extraction rules
 * \{ */

namespace {

/** An #ImBuf holding \a pixels as a 4-channel float buffer, freed with the guard. */
struct FloatImBufGuard {
  ImBuf *ibuf = nullptr;

  FloatImBufGuard(const int width, const int height, Span<float> rgba)
  {
    ibuf = IMB_allocImBuf(uint(width), uint(height), ImBufFlags::Zero);
    if (ibuf != nullptr) {
      IMB_alloc_float_pixels(ibuf, 4, false);
      std::copy_n(rgba.data(), int64_t(width) * height * 4, ibuf->float_data_for_write());
    }
  }
  ~FloatImBufGuard()
  {
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
  }
};

/** An #ImBuf holding \a rgba as a 4-channel byte buffer, freed with the guard. */
struct ByteImBufGuard {
  ImBuf *ibuf = nullptr;

  ByteImBufGuard(const int width, const int height, const Span<uchar> rgba)
  {
    ibuf = IMB_allocImBuf(uint(width), uint(height), ImBufFlags::ByteData);
    if (ibuf != nullptr) {
      std::copy_n(rgba.data(), int64_t(width) * height * 4, ibuf->byte_data_for_write());
    }
  }
  ~ByteImBufGuard()
  {
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
  }
};

/** Fill \a source as a resolved Image-kind source pinned to \a ibuf. */
void image_source_with(ImBuf &ibuf, ChannelSourceSet::ChannelSource &source)
{
  source.kind = ChannelSourceKind::Image;
  source.usable = true;
  source.image_direct_sample = true;
  source.ibuf = &ibuf;
}

/** Fill \a source as a Baked Target UV source pinned to \a ibuf. */
void baked_target_uv_source_with(ImBuf &ibuf, ChannelSourceSet::ChannelSource &source)
{
  image_source_with(ibuf, source);
  source.kind = ChannelSourceKind::Baked;
  source.baked_target_uv = true;
}

/** The unit placement the Area tests feed to the shared sampler. */
void area_mtex_with(ChannelSourceSet::ChannelSource &source, MTex &mtex, Tex *tex = nullptr)
{
  mtex.size[0] = 1.0f;
  mtex.size[1] = 1.0f;
  mtex.ofs[0] = 0.0f;
  mtex.ofs[1] = 0.0f;
  mtex.tex = tex;
  source.mtex = &mtex;
}

/** A source set with no channels: the Area sampler only needs its direct-sampling methods, which
 * read nothing from the set itself. */
ChannelSourceSet &empty_source_set()
{
  static ChannelSourceSet sources(BrushMaterialPaint{}, PaintModeSettings{}, 0);
  return sources;
}

/** One Area coverage read through the module, with the cache optionally attached. */
float area_coverage_sample(const eMaterialPaintChannel channel,
                           const ChannelSourceSet::ChannelSource &source,
                           const MaskSourceCoverage *coverage,
                           const MTex &mtex,
                           const float3 &position,
                           const float2 &uv = float2(0.0f),
                           const float bias = 0.0f)
{
  MaskCoverageSampleArea2D params;
  params.channel = channel;
  params.sources = &empty_source_set();
  params.source = &source;
  params.coverage = coverage;
  params.mtex = &mtex;
  params.texture_sample_bias = bias;
  return mask_coverage_sample_area_2d(
      params, float4x4::identity(), position, uv, 0);
}

}  // namespace

TEST(MaskCoverageExtract, AlphaFollowsTheDirectSamplerIntensity)
{
  /* The Alpha rule the stroke path uses: the direct sampler's luminance of the stored color,
   * unclamped and un-inverted - the stroke's invert mirrors afterwards, not here. */
  const int width = 3;
  const int height = 2;
  const Array<float> rgba = {
      0.0f, 0.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.25f, 0.5f, 0.75f, 0.5f,
      0.5f, 0.5f, 0.5f, 0.25f, 2.0f, 0.0f, 0.0f, 1.0f,  0.0f,  3.0f, 0.0f,   1.0f,
  };
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  image_source_with(*ibuf_guard.ibuf, source);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_ALPHA, source);
  ASSERT_NE(coverage, nullptr);
  EXPECT_EQ(coverage->width(), width);
  EXPECT_EQ(coverage->height(), height);
  for (const int64_t i : IndexRange(int64_t(width) * height)) {
    EXPECT_FLOAT_EQ(coverage->data()[i], rgba_luminance(rgba.data() + i * 4));
  }
}

TEST(MaskCoverageExtract, AlphaBakedTargetUvReadsTheBakeChannel)
{
  /* Target UV placement samples through #sample_baked, whose scalar is the bake's first channel -
   * not the direct sampler's luminance the brush-mapped kinds use. */
  const int width = 2;
  const int height = 2;
  const Array<float> rgba = {
      0.25f, 0.75f, 0.5f, 1.0f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f,
  };
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  baked_target_uv_source_with(*ibuf_guard.ibuf, source);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_ALPHA, source);
  ASSERT_NE(coverage, nullptr);
  for (const int64_t i : IndexRange(int64_t(width) * height)) {
    EXPECT_FLOAT_EQ(coverage->data()[i], rgba[i * 4]);
  }
}

TEST(MaskCoverageExtract, BaseColorIsSceneLinearLuminance)
{
  const int width = 2;
  const int height = 2;
  const Array<float> rgba = {
      1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
  };
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  image_source_with(*ibuf_guard.ibuf, source);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, source);
  ASSERT_NE(coverage, nullptr);
  for (const int64_t i : IndexRange(int64_t(width) * height)) {
    EXPECT_FLOAT_EQ(coverage->data()[i],
                    BKE_paint_material_mask_luminance(float3(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2])));
  }
  /* Red alone reads as the Rec.709 red weight; white as full coverage. */
  EXPECT_NEAR(coverage->data()[0], 0.2126f, 1e-5f);
  EXPECT_NEAR(coverage->data()[3], 1.0f, 1e-5f);
}

TEST(MaskCoverageExtract, UnitWrapSamplesTheWholeImage)
{
  /* The Target UV sample convention: uv (0, 0) is the first texel, (1, 1) tiles back to it, and
   * the buffer's own repeat bilinear carries the in-between. */
  const int width = 4;
  const int height = 3;
  RandomNumberGenerator rng(7);
  const Array<float> rgba = random_rgba_buffer(width, height, rng);
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  baked_target_uv_source_with(*ibuf_guard.ibuf, source);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_ALPHA, source);
  ASSERT_NE(coverage, nullptr);
  for (const int i : IndexRange(32)) {
    const float u = rng.get_float();
    const float v = rng.get_float();
    const float expected = interpolate_bilinear_channel_fl(
        coverage->data().data(), width, height, u * width, v * height, true);
    EXPECT_NEAR(coverage->sample_unit_wrap(u, v), expected, 1e-6f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source identity (the shape tools' key)
 * \{ */

TEST(MaskCoverageSourceKey, InvertChangesTheKey)
{
  /* Value-initialized: every DNA field without a member initializer reads zero, so the two key
   * builds below differ only in the field the test changes. */
  Brush brush{};
  BrushMaterialPaint brush_paint{};
  brush_paint.mask_source = BRUSH_MATERIAL_PAINT_MASK_SOURCE_BASE_COLOR;
  Paint paint{};
  PaintModeSettings mode_settings{};

  const uint64_t base = shape::mask_source_key(brush_paint, brush, mode_settings, paint);
  EXPECT_NE(base, 0);

  brush_paint.mask_source_invert = 1;
  EXPECT_NE(shape::mask_source_key(brush_paint, brush, mode_settings, paint), base);
}

TEST(MaskCoverageSourceKey, ChannelInvertIsNotTheMaskKeyAnymore)
{
  /* The migration's premise: the Alpha entry's own flag no longer drives the Mask Source key -
   * the Mask Source's field does - so a leftover Alpha flag cannot resurrect an inverted mask. */
  Brush brush{};
  BrushMaterialPaint brush_paint{};
  brush_paint.mask_source = BRUSH_MATERIAL_PAINT_MASK_SOURCE_BASE_COLOR;
  Paint paint{};
  PaintModeSettings mode_settings{};

  const uint64_t base = shape::mask_source_key(brush_paint, brush, mode_settings, paint);
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].source_invert = 1;
  EXPECT_EQ(shape::mask_source_key(brush_paint, brush, mode_settings, paint), base);
}

TEST(MaskCoverageSourceKey, SourceImageChangesTheKey)
{
  Brush brush{};
  BrushMaterialPaint brush_paint{};
  brush_paint.mask_source = BRUSH_MATERIAL_PAINT_MASK_SOURCE_BASE_COLOR;
  Paint paint{};
  PaintModeSettings mode_settings{};
  Tex tex{};
  Image image_a{};
  Image image_b{};
  tex.ima = &image_a;
  brush_paint.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].source_mtex.tex = &tex;

  const uint64_t with_a = shape::mask_source_key(brush_paint, brush, mode_settings, paint);
  EXPECT_NE(with_a, 0);

  tex.ima = &image_b;
  EXPECT_NE(shape::mask_source_key(brush_paint, brush, mode_settings, paint), with_a);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coverage rule (the texel reduction the stroke mirrors)
 * \{ */

TEST(MaskCoverageRule, AlphaTexelUsesAlphaWhileTranslucent)
{
  /* An alpha mask stores its coverage in alpha while RGB holds black: the grey is the alpha, and
   * the invert mirrors it. */
  const float coverage = BKE_paint_material_mask_texel_coverage(
      float4(0.0f, 0.0f, 0.0f, 0.3f), true, false);
  EXPECT_NEAR(coverage, 0.3f, 1e-6f);
  EXPECT_NEAR(BKE_paint_material_mask_texel_coverage(float4(0.0f, 0.0f, 0.0f, 0.3f), true, true),
              0.7f,
              1e-6f);
}

TEST(MaskCoverageRule, OpaqueTexelFallsBackToLuminance)
{
  /* Opaque images keep the grayscale rule, so a plain black-and-white mask still works. */
  const float coverage = BKE_paint_material_mask_texel_coverage(
      float4(1.0f, 1.0f, 1.0f, 1.0f), true, false);
  EXPECT_NEAR(coverage, 1.0f, 1e-6f);
  EXPECT_NEAR(BKE_paint_material_mask_texel_coverage(float4(1.0f, 1.0f, 1.0f, 1.0f), true, true),
              0.0f,
              1e-6f);
}

TEST(MaskCoverageRule, BaseColorTexelIsLuminance)
{
  /* Base Color has no alpha to fall back on: the grey is the scene-linear luminance, invertible
   * the same way. */
  const float4 grey(0.25f, 0.25f, 0.25f, 1.0f);
  EXPECT_NEAR(BKE_paint_material_mask_texel_coverage(grey, false, false), 0.25f, 1e-6f);
  EXPECT_NEAR(BKE_paint_material_mask_texel_coverage(grey, false, true), 0.75f, 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coverage finish (the one clamp + invert a Mask read applies)
 * \{ */

TEST(MaskCoverageFinish, ClampsBeforeMirroring)
{
  /* The clamp happens first, so an HDR texel cannot mirror past the range: 1.5 clamps to 1 and
   * mirrors to 0, -0.5 clamps to 0 and mirrors to 1. Every stroke-side read of a Mask coverage
   * ends in this one function, in both engines. */
  EXPECT_FLOAT_EQ(mask_coverage_finish(false, 0.3f), 0.3f);
  EXPECT_FLOAT_EQ(mask_coverage_finish(true, 0.3f), 0.7f);
  EXPECT_FLOAT_EQ(mask_coverage_finish(false, 1.5f), 1.0f);
  EXPECT_FLOAT_EQ(mask_coverage_finish(true, 1.5f), 0.0f);
  EXPECT_FLOAT_EQ(mask_coverage_finish(false, -0.5f), 0.0f);
  EXPECT_FLOAT_EQ(mask_coverage_finish(true, -0.5f), 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registry (eviction, budget, pinning, teardown)
 *
 * The registry is process-global, so every test here drops it first with
 * #mask_source_coverage_cache_free_all. The limits it is pinned against are the module constants:
 * #mask_coverage_cache_max_entries and #mask_coverage_cache_budget_bytes.
 * \{ */

namespace {

/** A coverage of \a texel_num texels, the shape the tests insert with. */
std::shared_ptr<const MaskSourceCoverage> registry_coverage(const int texel_num)
{
  return std::make_shared<MaskSourceCoverage>(texel_num, 1, Array<float>(texel_num, 0.5f));
}

/** An identity distinct for every \a tag the test passes. */
MaskCoverageKey registry_key_with(const int tag)
{
  MaskCoverageKey key;
  key.image = reinterpret_cast<const Image *>(uintptr_t(0x50) + tag);
  key.width = tag;
  key.height = 1;
  return key;
}

}  // namespace

TEST(MaskCoverageRegistry, InsertThenLookupReturnsTheSameBuffer)
{
  mask_source_coverage_cache_free_all();

  const std::shared_ptr<const MaskSourceCoverage> coverage = registry_coverage(4);
  EXPECT_TRUE(mask_coverage_cache_insert(registry_key_with(1), coverage));
  EXPECT_EQ(mask_coverage_cache_lookup(registry_key_with(1)), coverage);
}

TEST(MaskCoverageRegistry, CountLimitEvictsLeastRecentlyUsed)
{
  mask_source_coverage_cache_free_all();

  /* Four entries fill the registry; the fifth evicts the first one, not the newest. */
  for (const int i : IndexRange(4)) {
    EXPECT_TRUE(mask_coverage_cache_insert(registry_key_with(i), registry_coverage(4)));
  }
  const std::shared_ptr<const MaskSourceCoverage> fifth = registry_coverage(4);
  EXPECT_TRUE(mask_coverage_cache_insert(registry_key_with(4), fifth));
  EXPECT_EQ(mask_coverage_cache_lookup(registry_key_with(0)), nullptr);
  EXPECT_EQ(mask_coverage_cache_lookup(registry_key_with(4)), fifth);
}

TEST(MaskCoverageRegistry, PixelVersionChangeMissesTheCache)
{
  mask_source_coverage_cache_free_all();

  /* The key carries the image's #ImageRuntime::pixel_version, so an in-place rewrite of the same
   * buffer - same pointers, same sharing info, same flags - drops the old entry. */
  MaskCoverageKey key = registry_key_with(1);
  key.pixel_version = 7;
  const std::shared_ptr<const MaskSourceCoverage> coverage = registry_coverage(4);
  EXPECT_TRUE(mask_coverage_cache_insert(MaskCoverageKey(key), coverage));
  EXPECT_EQ(mask_coverage_cache_lookup(key), coverage);

  key.pixel_version = 8;
  EXPECT_EQ(mask_coverage_cache_lookup(key), nullptr);
}

TEST(MaskCoverageRegistry, BakedRevisionChangeMissesTheCache)
{
  mask_source_coverage_cache_free_all();

  /* A baked source has no #Image pixel version; its own bake revision is the full identity, so
   * even a bake whose buffers reuse the old addresses gets a fresh entry. */
  MaskCoverageKey key = registry_key_with(1);
  key.baked_revision = 11;
  const std::shared_ptr<const MaskSourceCoverage> coverage = registry_coverage(4);
  EXPECT_TRUE(mask_coverage_cache_insert(MaskCoverageKey(key), coverage));
  EXPECT_EQ(mask_coverage_cache_lookup(key), coverage);

  key.baked_revision = 12;
  EXPECT_EQ(mask_coverage_cache_lookup(key), nullptr);
}

TEST(MaskCoverageRegistry, FreeAllDropsEveryEntry)
{
  mask_source_coverage_cache_free_all();

  const MaskCoverageKey key = registry_key_with(1);
  EXPECT_TRUE(mask_coverage_cache_insert(MaskCoverageKey(key), registry_coverage(4)));
  EXPECT_NE(mask_coverage_cache_lookup(key), nullptr);

  mask_source_coverage_cache_free_all();
  EXPECT_EQ(mask_coverage_cache_lookup(key), nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2D coverage read (cache versus per-texel)
 * \{ */

TEST(MaskCoverageSample2D, ConstantBaseColorReadsLuminanceUninverted)
{
  /* A flat source has no texels to cache, but the 2D read still has to reduce and clamp it. The
   * invert is deliberately NOT part of the sample: the caller applies it once where the coverage
   * is consumed. */
  ChannelSourceSet::ChannelSource source;
  source.kind = ChannelSourceKind::Constant;
  source.usable = true;
  source.constant_value = float4(0.25f, 0.25f, 0.25f, 1.0f);

  MaskCoverageSample2D params;
  params.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  params.source = &source;

  EXPECT_NEAR(mask_coverage_sample_2d(params, float3(0.0f), 0), 0.25f, 1e-6f);
  /* Out-of-range flat values clamp before the caller's mirror. */
  source.constant_value = float4(1.5f, 1.5f, 1.5f, 1.0f);
  EXPECT_NEAR(mask_coverage_sample_2d(params, float3(0.0f), 0), 1.0f, 1e-6f);
}

TEST(MaskCoverageSample2D, MatchesRequiresTheSameImageGrid)
{
  MaskSourceCoverage coverage(2, 2, Array<float>(4, 0.5f));
  const Array<float> rgba = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                             0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f};
  FloatImBufGuard ibuf_guard(2, 2, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  image_source_with(*ibuf_guard.ibuf, source);
  EXPECT_TRUE(mask_coverage_matches_source(&coverage, source));

  MaskSourceCoverage other(3, 1, Array<float>(3, 0.5f));
  EXPECT_FALSE(mask_coverage_matches_source(&other, source));
}

/* -------------------------------------------------------------------- */
/** \name Area coverage read (cache versus per-texel)
 * \{ */

namespace {

/** Positions that land inside the unit placement the Area tests build. */
const std::array<float3, 5> area_test_positions = {
    float3(0.0f, 0.0f, 0.0f),
    float3(-0.7f, -0.4f, 0.0f),
    float3(0.6f, 0.3f, 0.0f),
    float3(0.2f, -0.8f, 0.0f),
    float3(-0.3f, 0.9f, 0.0f),
};

}  // namespace

TEST(MaskCoverageSampleArea2D, AlphaByteCacheMatchesLiveSample)
{
  const int width = 4;
  const int height = 3;
  const Array<uchar> rgba = {
      0,   0,   0,   255, 64,  0,   0,   255, 0,   128, 0,   255, 255, 255, 255, 255,
      32,  32,  32,  255, 200, 100, 50,  255, 10,  20,  30,  255, 255, 0,   0,   255,
      0,   255, 0,   255, 0,   0,   255, 255, 128, 64,  32,  255, 255, 255, 0,   128,
  };
  ByteImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  image_source_with(*ibuf_guard.ibuf, source);
  MTex mtex;
  area_mtex_with(source, mtex);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_ALPHA, source);
  ASSERT_NE(coverage, nullptr);

  for (const float3 &position : area_test_positions) {
    const float cached = area_coverage_sample(
        PAINT_MATERIAL_CHANNEL_ALPHA, source, coverage.get(), mtex, position);
    const float live = area_coverage_sample(
        PAINT_MATERIAL_CHANNEL_ALPHA, source, nullptr, mtex, position);
    EXPECT_NEAR(cached, live, 1e-6f);
  }
}

TEST(MaskCoverageSampleArea2D, BaseColorFloatCacheMatchesLiveSample)
{
  const int width = 3;
  const int height = 2;
  const Array<float> rgba = {
      1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f,
      1.0f, 1.0f, 1.0f, 1.0f, 0.2f, 0.4f, 0.8f, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f,
  };
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  image_source_with(*ibuf_guard.ibuf, source);
  MTex mtex;
  area_mtex_with(source, mtex);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, source);
  ASSERT_NE(coverage, nullptr);

  for (const float3 &position : area_test_positions) {
    const float cached = area_coverage_sample(
        PAINT_MATERIAL_CHANNEL_BASE_COLOR, source, coverage.get(), mtex, position);
    const float live = area_coverage_sample(
        PAINT_MATERIAL_CHANNEL_BASE_COLOR, source, nullptr, mtex, position);
    EXPECT_NEAR(cached, live, 1e-6f);
  }
}

TEST(MaskCoverageSampleArea2D, BakedTargetUvCacheMatchesLiveSample)
{
  const int width = 3;
  const int height = 2;
  const Array<float> rgba = {
      0.1f, 0.2f, 0.3f, 1.0f, 0.4f, 0.5f, 0.6f, 1.0f, 0.7f, 0.8f, 0.9f, 1.0f,
      1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.25f, 0.75f, 1.0f,
  };
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  baked_target_uv_source_with(*ibuf_guard.ibuf, source);
  MTex mtex;
  area_mtex_with(source, mtex);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_ALPHA, source);
  ASSERT_NE(coverage, nullptr);

  const std::array<float2, 4> uvs = {
      float2(0.0f, 0.0f), float2(0.5f, 0.5f), float2(0.9f, 0.1f), float2(0.25f, 0.75f)};
  for (const float2 &uv : uvs) {
    const float cached = area_coverage_sample(
        PAINT_MATERIAL_CHANNEL_ALPHA, source, coverage.get(), mtex, float3(0.0f), uv);
    const float live = area_coverage_sample(
        PAINT_MATERIAL_CHANNEL_ALPHA, source, nullptr, mtex, float3(0.0f), uv);
    EXPECT_NEAR(cached, live, 1e-6f);
  }
}

TEST(MaskCoverageSampleArea2D, NonZeroBiasAppliesOnceInBothPaths)
{
  const int width = 2;
  const int height = 2;
  const Array<float> rgba = {0.8f, 0.8f, 0.8f, 1.0f, 0.8f, 0.8f, 0.8f, 1.0f,
                             0.8f, 0.8f, 0.8f, 1.0f, 0.8f, 0.8f, 0.8f, 1.0f};
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  ChannelSourceSet::ChannelSource source;
  image_source_with(*ibuf_guard.ibuf, source);
  MTex mtex;
  area_mtex_with(source, mtex);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_ALPHA, source);
  ASSERT_NE(coverage, nullptr);

  const float3 position(0.0f);
  const float without_bias = area_coverage_sample(
      PAINT_MATERIAL_CHANNEL_ALPHA, source, coverage.get(), mtex, position, float2(0.0f), 0.0f);
  const float cached = area_coverage_sample(
      PAINT_MATERIAL_CHANNEL_ALPHA, source, coverage.get(), mtex, position, float2(0.0f), 0.25f);
  const float live = area_coverage_sample(
      PAINT_MATERIAL_CHANNEL_ALPHA, source, nullptr, mtex, position, float2(0.0f), 0.25f);
  EXPECT_NEAR(cached, live, 1e-6f);
  EXPECT_NEAR(cached, without_bias - 0.25f, 1e-6f);
}

TEST(MaskCoverageSampleArea2D, ClipOutsideThePlacementReadsEmptyGrey)
{
  const int width = 2;
  const int height = 2;
  const Array<float> rgba = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                             1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  FloatImBufGuard ibuf_guard(width, height, rgba);
  ASSERT_NE(ibuf_guard.ibuf, nullptr);

  Tex tex{};
  tex.extend = TEX_CLIP;
  ChannelSourceSet::ChannelSource source;
  image_source_with(*ibuf_guard.ibuf, source);
  MTex mtex;
  area_mtex_with(source, mtex, &tex);
  const std::shared_ptr<const MaskSourceCoverage> coverage = mask_source_coverage_build(
      PAINT_MATERIAL_CHANNEL_ALPHA, source);
  ASSERT_NE(coverage, nullptr);

  /* u = (1.5 + 1) / 2 = 1.25: past the right edge, which TEX_CLIP reports as empty. */
  const float3 outside(1.5f, 0.0f, 0.0f);
  EXPECT_FLOAT_EQ(area_coverage_sample(
                      PAINT_MATERIAL_CHANNEL_ALPHA, source, coverage.get(), mtex, outside),
                  0.0f);
  EXPECT_FLOAT_EQ(area_coverage_sample(
                      PAINT_MATERIAL_CHANNEL_ALPHA, source, nullptr, mtex, outside),
                  0.0f);
}

/** \} */

/** \} */

}  // namespace blender::ed::sculpt_paint::material
