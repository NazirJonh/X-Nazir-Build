/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Mask Source coverage reads for the Image Editor 2D engine. The per-texel path and the
 * precomputed #MaskSourceCoverage are decided here, so no sampling branch in
 * `paint_image_2d.cc` has to know whether a grey came from the cache.
 */

#include "BKE_brush.hh"

#include "paint_material_source.hh"

struct Brush;
struct ImagePool;
struct MTex;
struct Paint;

namespace blender::ed::sculpt_paint::material {

/** Inputs of one Mask Source coverage read at a dab-buffer texel. */
struct MaskCoverageSample2D {
  eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ChannelSourceSet *sources = nullptr;
  const ChannelSourceSet::ChannelSource *source = nullptr;
  /** Direct-sample plan of the source, when the placement has one. */
  const DirectSampleLayout *layout = nullptr;
  /** Shared precomputed coverage, or null when the source is not cacheable. */
  const MaskSourceCoverage *coverage = nullptr;
  /** The source's #MTex remapped to the 2D mapping mode, for the texture-engine fallback. */
  const MTex *mtex = nullptr;
  const Paint *paint = nullptr;
  const Brush *brush = nullptr;
  ImagePool *pool = nullptr;
};

/**
 * The Mask Source grey at a dab-buffer texel, clamped to 0..1 and still un-inverted: the caller
 * applies the Mask Source's invert once where the coverage is consumed (see
 * #mask_coverage_finish_2d). \a texco is the source coordinate the caller placed: the shared
 * source mapping for brush-mapped sources, the target-UV mapping for Baked Target UV.
 *
 * A cacheable source reads #MaskSourceCoverage; every other usable source is sampled and reduced
 * here, with the same rules the cache extraction uses (Alpha: the direct sampler's intensity,
 * Baked Target UV: the bake's first channel, Base Color: scene-linear luminance). A missing or
 * unusable source reads as full coverage, like the Sculpt sampler.
 */
float mask_coverage_sample_2d(const MaskCoverageSample2D &params,
                              const float3 &texco,
                              int thread);

/** Inputs of one Mask Source coverage read on the Area Plane raster. */
struct MaskCoverageSampleArea2D {
  eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ChannelSourceSet *sources = nullptr;
  const ChannelSourceSet::ChannelSource *source = nullptr;
  /** Shared precomputed coverage, or null when the source is not cacheable. */
  const MaskSourceCoverage *coverage = nullptr;
  /** The channel's #MTex remapped to the 2D mapping mode, for the texture-engine fallback. */
  const MTex *mtex = nullptr;
  const Paint *paint = nullptr;
  const Brush *brush = nullptr;
  ImagePool *pool = nullptr;
  /** #Brush::texture_sample_bias, applied the way the Sculpt Area sampler applies it. */
  float texture_sample_bias = 0.0f;
};

/**
 * The Mask Source grey at an Area Plane texel, clamped to 0..1 and still un-inverted: the caller
 * applies the Mask Source's invert once (see #mask_coverage_finish_2d).
 *
 * The branching and reductions are the ones #mask_coverage_sample_2d uses for the View path:
 * Constant, Baked Target UV (the bake's first channel), and the brush-mapped Image / Baked / Mtex
 * sources (Alpha: the direct sampler's intensity, Base Color: scene-linear luminance). Every
 * branch tries the precomputed #MaskCoverageSampleArea2D::coverage first and falls back to the
 * live sample, so this module is the only place that knows which one answered.
 *
 * Bias follows #ChannelSourceSampler::mask_coverage: an Area sample subtracts
 * #MaskCoverageSampleArea2D::texture_sample_bias after the reduction, while Target UV placement
 * carries no brush-relative bias.
 */
float mask_coverage_sample_area_2d(const MaskCoverageSampleArea2D &params,
                                   const float4x4 &local_mat,
                                   const float3 &position,
                                   const float2 &uv,
                                   int thread);

/** The one 0..1 clamp plus Mask Source invert, shared by every 2D coverage read. */
inline float mask_coverage_finish_2d(const bool invert, const float raw)
{
  return BKE_brush_mask_coverage_finish(invert, raw);
}

/** Whether \a coverage was built for \a source's current pixel grid. */
bool mask_coverage_matches_source(const MaskSourceCoverage *coverage,
                                  const ChannelSourceSet::ChannelSource &source);

}  // namespace blender::ed::sculpt_paint::material
