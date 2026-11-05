/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_image_2d_mask_coverage.hh"

#include "paint_mask_coverage_cache.hh"

#include "../paint_intern.hh"

#include "BKE_brush.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

namespace blender::ed::sculpt_paint::material {

bool mask_coverage_matches_source(const MaskSourceCoverage *coverage,
                                  const ChannelSourceSet::ChannelSource &source)
{
  return coverage != nullptr && source.ibuf != nullptr && coverage->matches(*source.ibuf);
}

float mask_coverage_sample_2d(const MaskCoverageSample2D &params,
                              const float3 &texco,
                              const int thread)
{
  const ChannelSourceSet::ChannelSource &source = *params.source;

  if (source.kind == ChannelSourceKind::Constant) {
    const float raw = params.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR ?
                          BKE_paint_material_mask_luminance(float3(source.constant_value)) :
                          source.constant_value.x;
    return BKE_brush_mask_coverage_finish(false, raw);
  }
  if (!source.usable) {
    return 1.0f;
  }

  /* Target UV placement samples the bake itself (or its coverage) at the caller's UV. */
  if (source.kind == ChannelSourceKind::Baked && source.baked_target_uv) {
    float value;
    float4 rgba;
    if (mask_coverage_matches_source(params.coverage, source)) {
      value = params.coverage->sample_unit_wrap(texco.x, texco.y);
    }
    else if (!params.sources->sample_baked(source, texco.x, texco.y, &value, rgba)) {
      return BKE_brush_mask_coverage_finish(false, 0.0f);
    }
    return BKE_brush_mask_coverage_finish(false, value);
  }

  float intensity = 0.0f;
  float4 rgba;
  bool sampled_from_coverage = false;
  bool sampled_direct = false;
  bool decoded_by_engine = false;
  if (params.layout != nullptr) {
    sampled_from_coverage = params.layout->coverage_pixels != nullptr;
    sampled_direct = sample_direct_layout(
        *params.layout, float3(0.0f), float2(texco.x, texco.y), &intensity, rgba);
  }
  if (!sampled_direct) {
    if (ELEM(source.kind, ChannelSourceKind::Image, ChannelSourceKind::Baked)) {
      /* No #Tex for the texture engine to evaluate: the source cannot be sampled this way. */
      return BKE_brush_mask_coverage_finish(false, 0.0f);
    }
    const bke::PaintRuntime *paint_runtime =
        params.paint != nullptr ? params.paint->runtime : nullptr;
    intensity = BKE_brush_sample_tex_3d(params.paint,
                                        params.brush,
                                        params.mtex,
                                        texco,
                                        rgba,
                                        thread,
                                        params.pool);
    decoded_by_engine = paint_runtime != nullptr && paint_runtime->do_linear_conversion;
  }

  if (params.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR && !sampled_from_coverage) {
    if (source.do_linear_conversion && source.colorspace != nullptr && !decoded_by_engine) {
      IMB_colormanagement_colorspace_to_scene_linear_v3(rgba, source.colorspace);
    }
    intensity = BKE_paint_material_mask_luminance(float3(rgba.x, rgba.y, rgba.z));
  }
  return BKE_brush_mask_coverage_finish(false, intensity);
}

float mask_coverage_sample_area_2d(const MaskCoverageSampleArea2D &params,
                                   const float4x4 &local_mat,
                                   const float3 &position,
                                   const float2 &uv,
                                   const int thread)
{
  const ChannelSourceSet::ChannelSource &source = *params.source;

  if (source.kind == ChannelSourceKind::Constant) {
    const float raw = params.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR ?
                          BKE_paint_material_mask_luminance(float3(source.constant_value)) :
                          source.constant_value.x;
    return BKE_brush_mask_coverage_finish(false, raw);
  }
  if (!source.usable) {
    return 1.0f;
  }

  /* Area placement follows the Sculpt sampler (#ChannelSourceSampler::mask_coverage): the brush
   * texture bias is subtracted from the scalar grey before the clamp. Target UV placement is
   * pinned to the bake's own coordinates and carries no brush-relative bias. */
  const auto finish_area = [&](const float raw) {
    return BKE_brush_mask_coverage_finish(false, raw - params.texture_sample_bias);
  };

  if (source.kind == ChannelSourceKind::Baked && source.baked_target_uv) {
    float value;
    float4 rgba;
    if (mask_coverage_matches_source(params.coverage, source)) {
      value = params.coverage->sample_unit_wrap(uv.x, uv.y);
    }
    else if (!params.sources->sample_baked(source, uv.x, uv.y, &value, rgba)) {
      return BKE_brush_mask_coverage_finish(false, 0.0f);
    }
    return BKE_brush_mask_coverage_finish(false, value);
  }

  const MTex *mtex = params.mtex != nullptr ? params.mtex : source.mtex;
  if (mtex == nullptr) {
    return BKE_brush_mask_coverage_finish(false, 0.0f);
  }
  float3 point = position;
  mul_m4_v3(local_mat.ptr(), point);
  const float tex_x = point.x * mtex->size[0] + mtex->ofs[0];
  const float tex_y = point.y * mtex->size[1] + mtex->ofs[1];

  float value = 0.0f;
  float4 rgba;
  if (mask_coverage_matches_source(params.coverage, source) &&
      params.sources->sample_image_direct_coverage(
          source, *params.coverage, tex_x, tex_y, &value, rgba))
  {
    return finish_area(value);
  }

  if (source.kind == ChannelSourceKind::Baked || source.kind == ChannelSourceKind::Image ||
      source.image_direct_sample)
  {
    if (!params.sources->sample_image_direct(source, tex_x, tex_y, &value, rgba)) {
      if (source.kind == ChannelSourceKind::Baked ||
          source.kind == ChannelSourceKind::Image)
      {
        /* No #Tex for the texture engine: the source cannot be sampled this way. */
        return finish_area(0.0f);
      }
    }
    else {
      if (params.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
        if (source.do_linear_conversion && source.colorspace != nullptr) {
          IMB_colormanagement_colorspace_to_scene_linear_v3(rgba, source.colorspace);
        }
        value = BKE_paint_material_mask_luminance(float3(rgba.x, rgba.y, rgba.z));
      }
      return finish_area(value);
    }
  }

  if (source.kind != ChannelSourceKind::Mtex) {
    return finish_area(0.0f);
  }
  /* Procedural or otherwise engine-sampled texture: the one branch the cache cannot cover. */
  const bke::PaintRuntime *paint_runtime = params.paint != nullptr ? params.paint->runtime : nullptr;
  const bool decoded_by_engine = paint_runtime != nullptr && paint_runtime->do_linear_conversion;
  paint_get_tex_pixel(mtex, tex_x, tex_y, params.pool, thread, &value, rgba);
  if (params.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    if (source.do_linear_conversion && source.colorspace != nullptr && !decoded_by_engine) {
      IMB_colormanagement_colorspace_to_scene_linear_v3(rgba, source.colorspace);
    }
    value = BKE_paint_material_mask_luminance(float3(rgba.x, rgba.y, rgba.z));
  }
  return finish_area(value);
}

}  // namespace blender::ed::sculpt_paint::material
