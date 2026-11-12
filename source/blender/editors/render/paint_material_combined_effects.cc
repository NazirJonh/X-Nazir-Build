/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * The GPU Combined preview's pass machinery and its effect stage; the split-off half of
 * #paint_material_combined_gather.cc. #CombinedPasses dispatches one pass of each kind over whatever
 * target the caller names, and #combined_preview_gpu_effects_run runs the effects: the walk that
 * collects them in their rows' order, the GPU result cache's loop with each input's size, plan and
 * fingerprints, the input rendered whole at its own resolution, and the pass plan dispatched and
 * published. The dispatch order, the barriers and the texture releases are the ones the gather
 * established; nothing here changes them.
 */

#include "paint_material_combined_effects.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "BKE_image.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_utils.hh"
#include "BLI_rect.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::ed::material_combined {

/**
 * The GPU texture of \a image read through \a image_user (null selects the default), and whether it
 * stores premultiplied colour, by the rule the texture upload used. The upload premultiplies a byte
 * buffer (assumed straight) and straightens a float one (assumed premultiplied) exactly when it
 * stores premultiplied, so dividing by alpha gives the straight colour `composite_decode_tile`
 * decodes. Null when the image has no GPU texture.
 */
static gpu::Texture *gpu_map_texture(Image *image,
                                     const ImageUser *image_user,
                                     bool &r_premultiplied)
{
  ImageUser iuser;
  if (image_user != nullptr) {
    iuser = *image_user;
  }
  else {
    BKE_imageuser_default(&iuser);
  }
  gpu::Texture *texture = BKE_image_get_gpu_texture(image, &iuser);
  if (texture == nullptr) {
    return nullptr;
  }
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
  r_premultiplied = BKE_image_has_gpu_texture_premultiplied_alpha(image, ibuf);
  BKE_image_release_ibuf(image, ibuf, nullptr);
  return texture;
}

/**
 * Turn a row's UV \a mapping into the flags and values the shaders read, when it applies. The same
 * values the CPU resample reads, the scale through the shared normalization.
 */
static void gpu_mapping_setup(const PaintMaterialCompositeMapping &mapping,
                              int &r_flags,
                              GpuMappingUniforms &r_uniforms)
{
  if (!mapping.enabled) {
    return;
  }
  bool repeat_u = true;
  bool repeat_v = true;
  BKE_paint_layers_mapping_wrap_axes(mapping.wrap, repeat_u, repeat_v);
  r_flags |= gpu_layer_mapped;
  if (repeat_u) {
    r_flags |= gpu_layer_repeat_u;
  }
  if (repeat_v) {
    r_flags |= gpu_layer_repeat_v;
  }
  if (mapping.tangent_normal) {
    r_flags |= gpu_layer_tangent_normal;
  }
  if (mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U)) {
    r_flags |= gpu_layer_mirror_u;
  }
  if (mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V)) {
    r_flags |= gpu_layer_mirror_v;
  }
  r_uniforms.offset = float2(mapping.offset[0], mapping.offset[1]);
  r_uniforms.scale = float2(BKE_paint_layers_mapping_scale_normalize(mapping.scale[0]),
                            BKE_paint_layers_mapping_scale_normalize(mapping.scale[1]));
  r_uniforms.rotation = float2(std::cos(mapping.rotation), std::sin(mapping.rotation));
  r_uniforms.mirror_center = float2(mapping.mirror_center[0], mapping.mirror_center[1]);
}

/** Upload the mapping values of \a uniforms; the three passes name them alike. */
static void gpu_mapping_uniforms_set(gpu::Shader *shader, const GpuMappingUniforms &uniforms)
{
  GPU_shader_uniform_2fv(shader, "mapping_offset", &uniforms.offset.x);
  GPU_shader_uniform_2fv(shader, "mapping_scale", &uniforms.scale.x);
  GPU_shader_uniform_2fv(shader, "mapping_rotation", &uniforms.rotation.x);
  GPU_shader_uniform_2fv(shader, "mapping_mirror_center", &uniforms.mirror_center.x);
}

/* -------------------------------------------------------------------- */
/** \name CombinedPasses
 * \{ */

CombinedPasses::CombinedPasses(gpu::TexturePool &pool,
                               gpu::Texture *dummy,
                               gpu::Texture *slot_dummy,
                               gpu::Shader *layer_shader,
                               gpu::Shader *mask_shader,
                               gpu::Shader *correction_shader,
                               gpu::Shader *folder_shader,
                               gpu::Shader *effect_shader,
                               Vector<gpu::Texture *> &filter_luts,
                               Vector<GpuSliceSlot> &slice_slots,
                               Vector<GpuMaskSlot> &mask_slots,
                               Vector<GpuEffectResult> &effect_results,
                               eGPUTextureUsage accum_usage)
    : pool(pool),
      dummy(dummy),
      slot_dummy(slot_dummy),
      layer_shader(layer_shader),
      mask_shader(mask_shader),
      correction_shader(correction_shader),
      folder_shader(folder_shader),
      effect_shader(effect_shader),
      filter_luts(filter_luts),
      slice_slots(slice_slots),
      mask_slots(mask_slots),
      effect_results(effect_results),
      accum_usage(accum_usage)
{
}

bool CombinedPasses::color_ramp_is_constant(const ColorBand &band)
{
  return band.color_mode == COLBAND_BLEND_RGB && band.ipotype == COLBAND_INTERP_CONSTANT;
}

gpu::Texture *CombinedPasses::filter_lut_acquire(const int filter_kind,
                                                 const ColorBand *ramp,
                                                 const CurveMapping *curves)
{
  Vector<float4> texels;
  int width = 0;
  int height = 0;
  if (filter_kind == MA_PAINT_LAYER_FILTER_COLOR_RAMP) {
    if (ramp == nullptr || ramp->tot == 0) {
      return nullptr;
    }
    paint_layer_color_ramp_lut_build(*ramp, texels, width);
    height = 1;
  }
  else if (filter_kind == MA_PAINT_LAYER_FILTER_CURVES) {
    if (curves == nullptr) {
      return nullptr;
    }
    paint_layer_curves_lut_build(*curves, texels, width, height);
  }
  else {
    return dummy;
  }
  gpu::Texture *lut = pool.acquire_texture_2d(
      int2(width, height),
      1,
      gpu::TextureFormat::SFLOAT_32_32_32_32,
      eGPUTextureUsage(GPU_TEXTURE_USAGE_SHADER_READ),
      "combined_filter_lut");
  if (lut == nullptr) {
    return nullptr;
  }
  GPU_texture_update(lut, GPU_DATA_FLOAT, texels.data());
  /* The upload is read by the dispatch that follows; the tests upload-and-read the same way. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  filter_luts.append(lut);
  return lut;
}

float CombinedPasses::map_lod(CombinedTarget &t,
                              gpu::Texture *map,
                              const int flags,
                              const float2 &mapping_scale)
{
  if (map == nullptr || (flags & gpu_layer_alpha_coverage) != 0 || (flags & gpu_layer_slot) != 0) {
    /* A painted map stores straight colour with black where it is transparent, and its mip
     * levels average that black in: a stroke edge seen from afar would come out several times
     * darker than the CPU composite reads it. The base level has no such bleed. A slice slot is
     * screen-space and has no mip levels at all. */
    return 0.0f;
  }
  const float2 map_size(GPU_texture_width(map), GPU_texture_height(map));
  const float3x3 &to_uv = t.texel_to_uv;
  float texels_per_pixel = std::max(
      math::length(float2(to_uv[0].x, to_uv[0].y) * map_size),
      math::length(float2(to_uv[1].x, to_uv[1].y) * map_size));
  if ((flags & gpu_layer_mapped) != 0) {
    /* A scaled mapping reads the map denser or sparser than the canvas does. */
    texels_per_pixel *= std::max(std::abs(mapping_scale.x), std::abs(mapping_scale.y));
  }
  return std::max(0.0f, std::log2(std::max(texels_per_pixel, 1e-6f)));
}

bool CombinedPasses::lay(CombinedTarget &t,
                         gpu::Texture *dest,
                         gpu::Texture *map,
                         const int flags,
                         const float4 &layer_color,
                         const float4 &init_color,
                         const float opacity,
                         const GpuMappingUniforms &mapping,
                         const int blend,
                         gpu::Texture *mask,
                         const int filter_kind,
                         const float *filter_params,
                         const ColorBand *filter_ramp,
                         const CurveMapping *filter_curves,
                         gpu::Texture *slot_coverage)
{
  gpu::Texture *sampled = map != nullptr ? map : dummy;
  gpu::Texture *mask_sampled = mask != nullptr ? mask : dummy;
  gpu::Texture *slot_cov_sampled = slot_coverage != nullptr ? slot_coverage : dummy;
  const float lod = map_lod(t, map, flags, mapping.scale);
  GPU_shader_bind(layer_shader);
  GPU_shader_uniform_mat4(layer_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(layer_shader, "texture_size", &t.size.x);
  GPU_shader_uniform_4fv(layer_shader, "layer_color", &layer_color.x);
  GPU_shader_uniform_4fv(layer_shader, "init_color", &init_color.x);
  gpu_mapping_uniforms_set(layer_shader, mapping);
  GPU_shader_uniform_1f(layer_shader, "opacity", opacity);
  GPU_shader_uniform_1f(layer_shader, "lod", lod);
  /* The scene luminance the F0 map encoding reads a specular colour by: the coefficients the RGB
   * to BW nodes and the CPU helper use, with `w` carrying the F0 a level of one is pre-divided, so
   * the layer pass multiplies by one value. */
  float luma_coefficients[3];
  IMB_colormanagement_get_luminance_coefficients(luma_coefficients);
  const float4 map_luma(luma_coefficients[0],
                        luma_coefficients[1],
                        luma_coefficients[2],
                        1.0f / SPECULAR_LEVEL_F0_FULL);
  GPU_shader_uniform_4fv(layer_shader, "map_luma", &map_luma.x);
  GPU_shader_uniform_1i(layer_shader, "flags", flags);
  GPU_shader_uniform_1i(layer_shader, "blend", blend);
  GPU_shader_uniform_1i(layer_shader, "filter_kind", filter_kind);
  const float4 adjustment_0 = filter_params != nullptr ?
                                  float4(filter_params[0], filter_params[1], filter_params[2], filter_params[3]) :
                                  float4(0.0f);
  float4 adjustment_1 = filter_params != nullptr ?
                            float4(filter_params[4], filter_params[5], filter_params[6], filter_params[7]) :
                            float4(0.0f);
  /* A Color Balance's exponent triple; zero reads as a disabled filter's unused parameter. */
  float4 adjustment_2 = filter_params != nullptr ?
                            float4(filter_params[8],
                                   filter_params[9],
                                   filter_params[10],
                                   filter_params[11]) :
                            float4(0.0f);
  if (filter_kind == MA_PAINT_LAYER_FILTER_COLOR_RAMP && filter_ramp != nullptr) {
    /* The band's CONSTANT interpolation, which the shader reads from `filter_params_1.x`. */
    adjustment_1.x = color_ramp_is_constant(*filter_ramp) ? 1.0f : 0.0f;
  }
  /* The shader applies a layer filter only on an adjustment that is not reading a row already
   * filtered: the row-use pass would otherwise build a table it never samples. */
  const bool applies_filter = (flags & gpu_layer_adjustment) != 0 &&
                              (flags & gpu_layer_row_use) == 0;
  gpu::Texture *filter_lut = applies_filter ?
                                 filter_lut_acquire(filter_kind, filter_ramp, filter_curves) :
                                 dummy;
  if (filter_lut == nullptr) {
    return gpu_refuse("a filter LUT could not be built");
  }
  GPU_shader_uniform_4fv(layer_shader, "filter_params_0", &adjustment_0.x);
  GPU_shader_uniform_4fv(layer_shader, "filter_params_1", &adjustment_1.x);
  GPU_shader_uniform_4fv(layer_shader, "filter_params_2", &adjustment_2.x);

  /* The previous pass wrote this accumulator through an image. */
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(layer_shader, "layer_tx"));
  GPU_texture_bind(mask_sampled, GPU_shader_get_sampler_binding(layer_shader, "mask_tx"));
  GPU_texture_bind(slot_cov_sampled,
                   GPU_shader_get_sampler_binding(layer_shader, "slot_cov_tx"));
  GPU_texture_bind(filter_lut, GPU_shader_get_sampler_binding(layer_shader, "filter_lut_tx"));
  GPU_texture_image_bind(dest, GPU_shader_get_sampler_binding(layer_shader, "accum_img"));
  GPU_texture_image_bind(t.row_color,
                         GPU_shader_get_sampler_binding(layer_shader, "row_color_img"));
  GPU_texture_image_bind(t.row_factor,
                         GPU_shader_get_sampler_binding(layer_shader, "row_factor_img"));
  GPU_compute_dispatch(layer_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(t.row_factor);
  GPU_texture_image_unbind(t.row_color);
  GPU_texture_image_unbind(dest);
  GPU_texture_unbind(filter_lut);
  GPU_texture_unbind(slot_cov_sampled);
  GPU_texture_unbind(mask_sampled);
  GPU_texture_unbind(sampled);
  dispatches++;
  return true;
}

bool CombinedPasses::mask_step(CombinedTarget &t,
                               const int mode,
                               gpu::Texture *map,
                               const int flags,
                               const float constant_gray,
                               const float opacity,
                               const float influence,
                               const int blend,
                               const int filter,
                               const float *filter_params,
                               const ColorBand *filter_ramp,
                               const CurveMapping *filter_curves,
                               const PaintMaterialCompositeMapping *mapping,
                               gpu::Texture *slot_dst,
                               gpu::Texture *mask_source,
                               const float contrast)
{
  gpu::Texture *sampled = map != nullptr ? map : dummy;
  int step_flags = flags;
  GpuMappingUniforms mapping_uniforms;
  if (mapping != nullptr && map != nullptr) {
    gpu_mapping_setup(*mapping, step_flags, mapping_uniforms);
  }
  float4 params_0(0.0f);
  float4 params_1(0.0f);
  /* A Color Balance's exponent triple; zero reads as a disabled filter's unused parameter. */
  float4 params_2(0.0f);
  if (filter_params != nullptr) {
    params_0 = float4(filter_params[0], filter_params[1], filter_params[2], filter_params[3]);
    params_1 = float4(filter_params[4], filter_params[5], filter_params[6], filter_params[7]);
    params_2 = float4(filter_params[8], filter_params[9], filter_params[10], filter_params[11]);
  }
  if (filter == MA_PAINT_LAYER_FILTER_COLOR_RAMP && filter_ramp != nullptr) {
    params_1.x = color_ramp_is_constant(*filter_ramp) ? 1.0f : 0.0f;
  }
  gpu::Texture *filter_lut = filter_lut_acquire(filter, filter_ramp, filter_curves);
  if (filter_lut == nullptr) {
    return gpu_refuse("a filter LUT could not be built");
  }
  /* A mask map is painted like a layer's: its base level, for the same reason. */
  const float lod = 0.0f;
  GPU_shader_bind(mask_shader);
  GPU_shader_uniform_mat4(mask_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(mask_shader, "texture_size", &t.size.x);
  gpu_mapping_uniforms_set(mask_shader, mapping_uniforms);
  GPU_shader_uniform_4fv(mask_shader, "filter_params_0", &params_0.x);
  GPU_shader_uniform_4fv(mask_shader, "filter_params_1", &params_1.x);
  GPU_shader_uniform_4fv(mask_shader, "filter_params_2", &params_2.x);
  GPU_shader_uniform_1f(mask_shader, "constant_gray", constant_gray);
  GPU_shader_uniform_1f(mask_shader, "opacity", opacity);
  GPU_shader_uniform_1f(mask_shader, "contrast", contrast);
  GPU_shader_uniform_1f(mask_shader, "influence", influence);
  GPU_shader_uniform_1f(mask_shader, "lod", lod);
  GPU_shader_uniform_1i(mask_shader, "flags", step_flags);
  GPU_shader_uniform_1i(mask_shader, "mode", mode);
  GPU_shader_uniform_1i(mask_shader, "blend", blend);
  GPU_shader_uniform_1i(mask_shader, "filter_kind", filter);

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  gpu::Texture *mask_target = mask_source != nullptr ? mask_source : t.mask_accum;
  GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(mask_shader, "mask_tx"));
  GPU_texture_bind(filter_lut, GPU_shader_get_sampler_binding(mask_shader, "filter_lut_tx"));
  GPU_texture_image_bind(mask_target, GPU_shader_get_sampler_binding(mask_shader, "mask_img"));
  GPU_texture_image_bind(slot_dst != nullptr ? slot_dst : slot_dummy,
                         GPU_shader_get_sampler_binding(mask_shader, "slot_img"));
  GPU_compute_dispatch(mask_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(slot_dst != nullptr ? slot_dst : slot_dummy);
  GPU_texture_image_unbind(mask_target);
  GPU_texture_unbind(filter_lut);
  GPU_texture_unbind(sampled);
  dispatches++;
  return true;
}

bool CombinedPasses::correction_step(CombinedTarget &t,
                                     const PaintMaterialCompositeCorrection &correction,
                                     const bool tracks_alpha,
                                     gpu::Texture *slot_color,
                                     gpu::Texture *slot_coverage,
                                     gpu::Texture *effect_result)
{
  gpu::Texture *map = nullptr;
  int step_flags = tracks_alpha ? gpu_layer_tracks_alpha : 0;
  if (effect_result != nullptr) {
    /* A CPU-computed effect the GPU computed: the result texture is the same buffer the CPU cache
     * hands the evaluator, at the same size and in the same texel space, so the shader's effect
     * branch stands in for the correction's map and for its filter at once. */
    map = effect_result;
    step_flags |= gpu_effect_result;
  }
  else if (slot_color != nullptr) {
    /* A live Anchor Point slice: screen-space, no mapping, coverage in the paired slot. */
    map = slot_color;
    step_flags |= gpu_layer_slot;
  }
  else if (correction.filter == 0 && correction.image != nullptr) {
    bool premultiplied = false;
    map = gpu_map_texture(correction.image, correction.iuser, premultiplied);
    if (map == nullptr) {
      return false;
    }
    step_flags |= gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
  }
  GpuMappingUniforms mapping_uniforms;
  if (map != nullptr && slot_color == nullptr && effect_result == nullptr) {
    gpu_mapping_setup(correction.mapping, step_flags, mapping_uniforms);
  }
  gpu::Texture *sampled = (map != nullptr && effect_result == nullptr) ? map : dummy;
  gpu::Texture *effect_sampled = effect_result != nullptr ? effect_result : dummy;
  gpu::Texture *slot_sampled = slot_coverage != nullptr ? slot_coverage : dummy;
  const float4 constant_color(correction.constant_color);
  const float4 params_0(correction.filter_params[0],
                        correction.filter_params[1],
                        correction.filter_params[2],
                        correction.filter_params[3]);
  float4 params_1(correction.filter_params[4],
                  correction.filter_params[5],
                  correction.filter_params[6],
                  correction.filter_params[7]);
  /* A Color Balance's exponent triple; zero reads as a disabled filter's unused parameter. */
  const float4 params_2(correction.filter_params[8],
                        correction.filter_params[9],
                        correction.filter_params[10],
                        correction.filter_params[11]);
  if (correction.filter == MA_PAINT_LAYER_FILTER_COLOR_RAMP && correction.filter_ramp != nullptr)
  {
    params_1.x = color_ramp_is_constant(*correction.filter_ramp) ? 1.0f : 0.0f;
  }
  gpu::Texture *filter_lut = filter_lut_acquire(
      int(correction.filter), correction.filter_ramp, correction.filter_curves);
  if (filter_lut == nullptr) {
    return gpu_refuse("a filter LUT could not be built");
  }
  /* A correction map is painted like a layer's: its base level, for the same reason. */
  const float lod = 0.0f;
  GPU_shader_bind(correction_shader);
  GPU_shader_uniform_mat4(correction_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(correction_shader, "texture_size", &t.size.x);
  GPU_shader_uniform_4fv(correction_shader, "constant_color", &constant_color.x);
  gpu_mapping_uniforms_set(correction_shader, mapping_uniforms);
  GPU_shader_uniform_4fv(correction_shader, "filter_params_0", &params_0.x);
  GPU_shader_uniform_4fv(correction_shader, "filter_params_1", &params_1.x);
  GPU_shader_uniform_4fv(correction_shader, "filter_params_2", &params_2.x);
  GPU_shader_uniform_1f(correction_shader, "opacity", correction.opacity);
  GPU_shader_uniform_1f(correction_shader, "lod", lod);
  GPU_shader_uniform_1i(correction_shader, "flags", step_flags);
  GPU_shader_uniform_1i(correction_shader, "blend", int(correction.blend));
  GPU_shader_uniform_1i(correction_shader, "filter_kind", int(correction.filter));

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(correction_shader, "correction_tx"));
  GPU_texture_bind(effect_sampled,
                   GPU_shader_get_sampler_binding(correction_shader, "effect_tx"));
  GPU_texture_bind(slot_sampled,
                   GPU_shader_get_sampler_binding(correction_shader, "slot_cov_tx"));
  GPU_texture_bind(filter_lut,
                   GPU_shader_get_sampler_binding(correction_shader, "filter_lut_tx"));
  GPU_texture_image_bind(t.row_color,
                         GPU_shader_get_sampler_binding(correction_shader, "row_color_img"));
  GPU_texture_image_bind(t.row_factor,
                         GPU_shader_get_sampler_binding(correction_shader, "row_factor_img"));
  GPU_compute_dispatch(correction_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(t.row_factor);
  GPU_texture_image_unbind(t.row_color);
  GPU_texture_unbind(filter_lut);
  GPU_texture_unbind(slot_sampled);
  GPU_texture_unbind(effect_sampled);
  GPU_texture_unbind(sampled);
  dispatches++;
  return true;
}

void CombinedPasses::folder_step(CombinedTarget &t,
                                 const int mode,
                                 const int level,
                                 const int flags,
                                 const float opacity,
                                 const int blend,
                                 gpu::Texture *mask)
{
  gpu::Texture *mask_sampled = mask != nullptr ? mask : dummy;
  GPU_shader_bind(folder_shader);
  GPU_shader_uniform_mat4(folder_shader, "texel_to_uv", t.texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(folder_shader, "texture_size", &t.size.x);
  GPU_shader_uniform_1f(folder_shader, "opacity", opacity);
  GPU_shader_uniform_1i(folder_shader, "flags", flags);
  GPU_shader_uniform_1i(folder_shader, "blend", blend);
  GPU_shader_uniform_1i(folder_shader, "mode", mode);

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(mask_sampled, GPU_shader_get_sampler_binding(folder_shader, "mask_tx"));
  GPU_texture_image_bind(t.row_color, GPU_shader_get_sampler_binding(folder_shader, "row_color_img"));
  GPU_texture_image_bind(t.row_factor,
                         GPU_shader_get_sampler_binding(folder_shader, "row_factor_img"));
  GPU_texture_image_bind(t.folder_premul[level],
                         GPU_shader_get_sampler_binding(folder_shader, "premul_img"));
  GPU_texture_image_bind(t.folder_cover[level],
                         GPU_shader_get_sampler_binding(folder_shader, "cover_img"));
  GPU_compute_dispatch(folder_shader, uint(t.groups.x), uint(t.groups.y), 1);
  GPU_texture_image_unbind(t.folder_cover[level]);
  GPU_texture_image_unbind(t.folder_premul[level]);
  GPU_texture_image_unbind(t.row_factor);
  GPU_texture_image_unbind(t.row_color);
  GPU_texture_unbind(mask_sampled);
  dispatches++;
}

bool CombinedPasses::folder_levels_ensure(CombinedTarget &t, const int level)
{
  if (level < 0 || level >= gpu_folder_levels_max) {
    return false;
  }
  if (t.folder_premul[level] == nullptr) {
    t.folder_premul[level] = pool.acquire_texture_2d(
        t.size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_folder_premul");
  }
  if (t.folder_cover[level] == nullptr) {
    t.folder_cover[level] = pool.acquire_texture_2d(
        t.size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_folder_cover");
  }
  return t.folder_premul[level] != nullptr && t.folder_cover[level] != nullptr;
}

GpuSliceSlot *CombinedPasses::slice_slot_find(const bUUID &point, const int channel)
{
  for (GpuSliceSlot &slot : slice_slots) {
    if (BKE_paint_layers_gpu_slot_key_equal({slot.point, slot.channel}, {point, channel})) {
      return &slot;
    }
  }
  return nullptr;
}

GpuMaskSlot *CombinedPasses::mask_slot_find(const bUUID &point)
{
  for (GpuMaskSlot &slot : mask_slots) {
    if (BLI_uuid_equal(slot.point, point)) {
      return &slot;
    }
  }
  return nullptr;
}

gpu::Texture *CombinedPasses::effect_result_find(const bUUID &marker, const int channel)
{
  for (const GpuEffectResult &result : effect_results) {
    if (BLI_uuid_equal(result.marker, marker) && result.channel == channel) {
      return result.texture;
    }
  }
  return nullptr;
}

bool CombinedPasses::mask_build(CombinedTarget &t,
                                const PaintMaterialCompositeImageLayer &layer,
                                bool &r_has_mask)
{
  r_has_mask = layer.mask_image != nullptr;
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    r_has_mask |= item.opacity > 0.0f;
  }
  if (!r_has_mask) {
    return true;
  }
  if (!mask_step(t,
                 gpu_mask_init,
                 nullptr,
                 0,
                 1.0f,
                 1.0f,
                 1.0f,
                 int(CompositeBlend::Mix),
                 0,
                 nullptr,
                 nullptr,
                 nullptr,
                 nullptr))
  {
    return false;
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    if (item.opacity <= 0.0f) {
      continue;
    }
    int item_flags = item.invert ? gpu_mask_invert : 0;
    if (item.map_coverage != 0) {
      /* The match accumulates over the passes; a missing or unreadable map leaves it at zero, the
       * full restriction the CPU composite gives. */
      if (!mask_step(t,
                     gpu_mask_cov_begin,
                     nullptr,
                     0,
                     1.0f,
                     1.0f,
                     1.0f,
                     int(CompositeBlend::Mix),
                     0,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr))
      {
        return false;
      }
      bool premultiplied = false;
      gpu::Texture *coverage_map = item.coverage_image != nullptr ?
                                       gpu_map_texture(
                                           item.coverage_image, item.coverage_iuser, premultiplied) :
                                       nullptr;
      if (coverage_map != nullptr) {
        const int map_flags = gpu_layer_has_map | gpu_map_nearest |
                              (premultiplied ? gpu_layer_premultiplied : 0);
        if (item.map_coverage == MA_PAINT_LAYER_FILTER_UV_ISLAND) {
          if (!mask_step(t,
                         gpu_mask_cov_scalar,
                         coverage_map,
                         map_flags,
                         1.0f,
                         1.0f,
                         1.0f,
                         int(CompositeBlend::Mix),
                         0,
                         nullptr,
                         nullptr,
                         nullptr,
                         &item.coverage_mapping))
          {
            return false;
          }
        }
        else if (item.map_coverage == MA_PAINT_LAYER_FILTER_COLOR_ID) {
          const float shared_tol = std::max(0.0f, item.coverage_tolerance);
          const float shared_soft = std::max(0.0f, item.coverage_softness);
          for (const PaintMaterialCompositeIdColor &entry : item.coverage_colors) {
            if (!entry.enabled) {
              continue;
            }
            /* Advanced gives each colour its own tolerance and a hard edge. */
            const float params[8] = {entry.color[0],
                                     entry.color[1],
                                     entry.color[2],
                                     entry.own_tolerance ? std::max(0.0f, entry.tolerance) : shared_tol,
                                     entry.own_tolerance ? 0.0f : shared_soft,
                                     0.0f,
                                     0.0f,
                                     0.0f};
            if (!mask_step(t,
                           gpu_mask_cov_color,
                           coverage_map,
                           map_flags,
                           1.0f,
                           1.0f,
                           1.0f,
                           int(CompositeBlend::Mix),
                           0,
                           params,
                           nullptr,
                           nullptr,
                           &item.coverage_mapping))
            {
              return false;
            }
          }
        }
      }
      if (!mask_step(t,
                     gpu_mask_cov_end,
                     nullptr,
                     item_flags,
                     1.0f,
                     item.opacity,
                     item.coverage_output,
                     int(CompositeBlend::Mix),
                     0,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     item.contrast))
      {
        return false;
      }
      continue;
    }
    gpu::Texture *item_map = nullptr;
    const bool live_reader = !BLI_uuid_is_nil(item.anchor_point_reader) &&
                             item.image == nullptr && item.filter == 0;
    if (live_reader) {
      if (item.anchor_point_target == int8_t(PaintLayerAnchorPointTarget::Mask)) {
        GpuMaskSlot *slot = mask_slot_find(item.anchor_point_point);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point mask slice has no captured slot");
        }
        item_map = slot->texture;
      }
      else {
        GpuSliceSlot *slot = slice_slot_find(item.anchor_point_point,
                                             item.anchor_point_read_channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point Content slice has no captured slot");
        }
        /* A content slice read as a mask is its coverage, which the coverage slot holds. */
        item_map = slot->coverage;
      }
      item_flags |= gpu_mask_slot;
    }
    else if (item.filter == 0 && item.image != nullptr) {
      bool premultiplied = false;
      item_map = gpu_map_texture(item.image, item.iuser, premultiplied);
      if (item_map == nullptr) {
        return gpu_refuse("a mask map has no GPU texture");
      }
      item_flags |= gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
    }
    if (!mask_step(t,
                   gpu_mask_item,
                   item_map,
                   item_flags,
                   item.constant_gray,
                   item.opacity,
                   1.0f,
                   int(item.blend),
                   int(item.filter),
                   item.filter_params,
                   item.filter_ramp,
                   item.filter_curves,
                   live_reader ? nullptr : &item.mapping,
                   nullptr,
                   nullptr,
                   item.contrast))
    {
      return false;
    }
  }
  if (layer.mask_image != nullptr) {
    bool premultiplied = false;
    gpu::Texture *mask_map = gpu_map_texture(layer.mask_image, layer.mask_iuser, premultiplied);
    if (mask_map == nullptr) {
      return gpu_refuse("a row's mask map has no GPU texture");
    }
    int map_flags = gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
    if (layer.mask_from_alpha) {
      map_flags |= gpu_mask_from_alpha;
    }
    if (!mask_step(t,
                   gpu_mask_map,
                   mask_map,
                   map_flags,
                   1.0f,
                   1.0f,
                   layer.mask_influence,
                   int(CompositeBlend::Mix),
                   0,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr))
    {
      return false;
    }
  }
  return true;
}

bool CombinedPasses::row_emit(CombinedTarget &t,
                              const PaintMaterialCompositeImageLayer &layer,
                              const int channel,
                              const int level,
                              gpu::Texture *capture_color,
                              gpu::Texture *capture_coverage)
{
  if (!layer.enabled || layer.opacity <= 0.0f) {
    /* The composite skips it as well. */
    return true;
  }
  const bool in_folder = level >= 0;

  /* The corrections that change anything. */
  bool has_corrections = false;
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.opacity <= 0.0f) {
      continue;
    }
    has_corrections |= correction.filter != 0 || correction.image != nullptr ||
                       correction.has_constant_color ||
                       !BLI_uuid_is_nil(correction.anchor_point_reader);
  }

  /* Where the row's colour and coverage are decided: the row textures for a folder, for a row in
   * a folder (its parent reads them there) and for a row with corrections; the map itself
   * otherwise. A capture reads the row textures even when nothing else would. */
  bool row_in_textures = layer.is_folder || in_folder || has_corrections ||
                         capture_color != nullptr;
  int flags = 0;
  gpu::Texture *map = nullptr;
  GpuSliceSlot *reader_slot = nullptr;
  if (!BLI_uuid_is_nil(layer.anchor_point_reader) && layer.color_image == nullptr) {
    /* A live reader row: its slice is in a captured slot. A baked reader has a colour map and
     * no slot. */
    /* Slots are keyed by the channel enum, which only equals the index in the shaded set for the
     * first channels. */
    reader_slot = slice_slot_find(layer.anchor_point_point, channel);
    if (reader_slot == nullptr) {
      return gpu_refuse("an Anchor Point row slice has no captured slot");
    }
  }
  float4 layer_color(layer.constant_color);
  GpuMappingUniforms mapping_uniforms;
  bool has_mask = false;

  if (layer.is_folder) {
    const int own_level = level + 1;
    if (!folder_levels_ensure(t, own_level)) {
      return gpu_refuse("folders are nested too deep for the GPU path");
    }
    folder_step(t, gpu_folder_init, own_level, 0, 1.0f, int(CompositeBlend::Mix), nullptr);
    for (const PaintMaterialCompositeImageLayer &child : layer.children) {
      if (!row_emit(t, child, channel, own_level, nullptr, nullptr)) {
        return false;
      }
    }
    /* After the children: the mask accumulator is theirs until they are laid. */
    if (!mask_build(t, layer, has_mask)) {
      return false;
    }
    folder_step(t, gpu_folder_resolve, own_level, 0, 1.0f, int(CompositeBlend::Mix), nullptr);
  }
  else {
    if (!mask_build(t, layer, has_mask)) {
      return false;
    }
    /* A row covers by its own alpha when it says so and no mask map takes the alpha's place. */
    const bool own_alpha = layer.mask_image == nullptr && layer.mask_from_alpha;
    if (reader_slot != nullptr) {
      map = reader_slot->color;
      flags |= gpu_layer_slot;
    }
    else if (layer.color_image != nullptr) {
      bool premultiplied = false;
      map = gpu_map_texture(layer.color_image, layer.color_iuser, premultiplied);
      if (map == nullptr) {
        return gpu_refuse("a layer map has no GPU texture");
      }
      flags |= gpu_layer_has_map;
      if (layer.normal_directx) {
        flags |= gpu_layer_normal_directx;
      }
      switch (eMaterialPaintLayerMapEncoding(layer.map_encoding)) {
        case MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS:
          flags |= gpu_layer_map_glossiness;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA:
          flags |= gpu_layer_map_glossiness_alpha;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_F0:
          flags |= gpu_layer_map_f0;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_F0_GLOSSINESS_ALPHA:
          flags |= gpu_layer_map_f0_glossiness_alpha;
          break;
        case MA_PAINT_LAYER_MAP_ENCODING_CHANNEL:
          break;
      }
      if (premultiplied) {
        flags |= gpu_layer_premultiplied;
      }
      if (layer.color_alpha_coverage || own_alpha) {
        flags |= gpu_layer_alpha_coverage;
      }
    }
    else if (own_alpha) {
      /* A flat colour covers by its own alpha when the row says so. */
      flags |= gpu_layer_alpha_coverage;
    }
    if (layer.empty_base) {
      flags |= gpu_layer_empty_base;
    }
    if (layer.adjustment_filter != 0) {
      flags |= gpu_layer_adjustment;
    }
    if (map != nullptr && reader_slot == nullptr) {
      gpu_mapping_setup(layer.mapping, flags, mapping_uniforms);
    }
    if (row_in_textures) {
      if (!lay(t,
               t.dest,
               map,
               flags | gpu_layer_row_store,
               layer_color,
               float4(0.0f),
               1.0f,
               mapping_uniforms,
               int(CompositeBlend::Mix),
               nullptr,
               int(layer.adjustment_filter),
               layer.adjustment_params,
               layer.adjustment_ramp,
               layer.adjustment_curves,
               reader_slot != nullptr ? reader_slot->coverage : nullptr))
      {
        return false;
      }
    }
  }

  if (row_in_textures) {
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      if (correction.opacity <= 0.0f) {
        continue;
      }
      const bool live_reader = !BLI_uuid_is_nil(correction.anchor_point_reader) &&
                               correction.image == nullptr &&
                               !correction.has_constant_color && correction.filter == 0;
      if (correction.filter == 0 && correction.image == nullptr &&
          !correction.has_constant_color && !live_reader)
      {
        continue;
      }
      gpu::Texture *slot_color = nullptr;
      gpu::Texture *slot_coverage = nullptr;
      if (live_reader) {
        GpuSliceSlot *slot = slice_slot_find(correction.anchor_point_point, channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point correction slice has no captured slot");
        }
        slot_color = slot->color;
        slot_coverage = slot->coverage;
      }
      gpu::Texture *effect_result = nullptr;
      if (paint_correction_is_effect(correction)) {
        /* A CPU-computed effect: the pass that computes it ran before the stacks, and its texture is
         * the correction's content now. Nothing found means this frame has no result for the effect
         * -- the row it sits on was not walked, or a captured slice holds a second effect -- so the
         * CPU path has to shade the whole stack rather than drop the effect. */
        effect_result = effect_result_find(correction.effect_marker, channel);
        if (effect_result == nullptr) {
          return gpu_refuse("a GPU effect has no computed result for this channel");
        }
      }
      if (!correction_step(
              t, correction, layer.tracks_content_alpha, slot_color, slot_coverage, effect_result))
      {
        return gpu_refuse("a correction map has no GPU texture");
      }
    }
  }

  if (capture_color != nullptr) {
    /* The row's content before its own opacity, mask and blend: what a reader of this point lays.
     * The row textures hold the corrections already, so only a straight copy is needed. */
    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_UPDATE);
    GPU_texture_copy(capture_color, t.row_color);
    /* The coverage slot is grey in rgb and opaque in alpha; the mask store pass does that from
     * the row factor's `x`. */
    if (!mask_step(t,
                   gpu_mask_slot_store,
                   nullptr,
                   0,
                   1.0f,
                   1.0f,
                   1.0f,
                   int(CompositeBlend::Mix),
                   0,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr,
                   capture_coverage,
                   t.row_factor))
    {
      return false;
    }
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
    return true;
  }

  const int mask_flags = has_mask ? gpu_layer_has_mask : 0;
  if (in_folder) {
    folder_step(t,
                gpu_folder_blend,
                level,
                mask_flags,
                layer.opacity,
                int(layer.blend),
                has_mask ? t.mask_accum : nullptr);
    return true;
  }
  if (!lay(t,
           t.dest,
           row_in_textures ? nullptr : map,
           (row_in_textures ? gpu_layer_row_use : flags) | mask_flags,
           layer_color,
           float4(0.0f),
           layer.opacity,
           mapping_uniforms,
           int(layer.blend),
           has_mask ? t.mask_accum : nullptr,
           int(layer.adjustment_filter),
           layer.adjustment_params,
           layer.adjustment_ramp,
           layer.adjustment_curves,
           reader_slot != nullptr ? reader_slot->coverage : nullptr))
  {
    return false;
  }
  return true;
}

void CombinedPasses::effect_pass_dispatch(const EffectDispatchArgs &args,
                                          const float4 &luma_coefficients)
{
  const GpuEffectJob &job = args.job;
  const rcti &rect = args.rect;
  const int2 origin(rect.xmin, rect.ymin);
  const int2 rect_size(rect.xmax - rect.xmin, rect.ymax - rect.ymin);
  const int2 groups = math::divide_ceil(rect_size, int2(16));
  if (groups.x <= 0 || groups.y <= 0) {
    return;
  }
  GPU_shader_bind(effect_shader);
  GPU_shader_uniform_2iv(effect_shader, "texture_size", &job.input_size.x);
  GPU_shader_uniform_2iv(effect_shader, "dispatch_origin", &origin.x);
  const int2 end(rect.xmax, rect.ymax);
  GPU_shader_uniform_2iv(effect_shader, "dispatch_end", &end.x);
  GPU_shader_uniform_1i(effect_shader, "pass_kind", int(args.pass.kind));
  GPU_shader_uniform_1i(effect_shader, "radius", args.pass.radius);
  GPU_shader_uniform_1i(effect_shader, "maximum", args.pass.maximum ? 1 : 0);
  GPU_shader_uniform_1f(effect_shader, "aux0", args.pass.aux0);
  GPU_shader_uniform_4fv(effect_shader, "filter_params_0", job.correction->filter_params);
  GPU_shader_uniform_4fv(effect_shader, "luma_coefficients", &luma_coefficients.x);
  GPU_shader_uniform_1i(effect_shader, "tracks_alpha", job.tracks_alpha ? 1 : 0);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  GPU_texture_bind(args.src, GPU_shader_get_sampler_binding(effect_shader, "input_tx"));
  GPU_texture_bind(args.aux != nullptr ? args.aux : dummy,
                   GPU_shader_get_sampler_binding(effect_shader, "aux_tx"));
  GPU_texture_image_bind(args.dst,
                         GPU_shader_get_sampler_binding(effect_shader, "result_img"));
  GPU_compute_dispatch(effect_shader, uint(groups.x), uint(groups.y), 1);
  GPU_texture_image_unbind(args.dst);
  GPU_texture_unbind(args.src);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  dispatches++;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Effect Stage
 * \{ */

/** The GPU effect cache's acquire hook: a canvas of the shape the frame's own accumulators have --
 * half float, read by the correction pass and written by the effect pass. The cache keeps one
 * reference to what this returns; a frame holding it across a dispatch takes one of its own. */
static void *effect_gpu_acquire(void * /*data*/, const int width, const int height)
{
  gpu::Texture *texture = GPU_texture_create_2d(
      "combined_effect_result",
      width,
      height,
      1,
      gpu::TextureFormat::SFLOAT_16_16_16_16,
      eGPUTextureUsage(GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE),
      nullptr);
  if (texture == nullptr) {
    return nullptr;
  }
  /* The result is read at the display's resolution, so a canvas texel is usually covered by several
   * screen pixels and filtered between them, exactly as a painted map is. */
  GPU_texture_filter_mode(texture, true);
  return texture;
}

/** The cache's release hook: drop its reference, which destroys the texture unless a frame still
 * holds one. */
static void effect_gpu_release(void * /*data*/, void *resource)
{
  if (resource != nullptr) {
    GPU_texture_free(static_cast<gpu::Texture *>(resource));
  }
}

/** The hooks the GPU effect cache works through for this module's textures. */
static const PaintLayerEffectGpuHooks effect_gpu_hooks = {
    nullptr,
    effect_gpu_acquire,
    effect_gpu_release,
};

bool combined_preview_gpu_effects_run(
    const Material &ma,
    Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
    Span<int> channel_enums,
    Span<bool> has_stack,
    int wanted_channel,
    CombinedPasses &passes,
    Vector<GpuEffectJob> &r_jobs)
{
  /* GPU effects: the content each effect reads, then the effect's own passes into the cache. This
   * runs after the Anchor Point captures -- a content render may read their slots -- and before the
   * channels' stacks, which mix the results in as corrections. A row may carry several effects:
   * they are collected in the row's own order, and each one's input (described up to its marker)
   * holds the results of the ones before it, so they are rendered and published in that order
   * below. */
  const auto effect_jobs_walk = [&](auto &&self,
                                   const PaintMaterialCompositeImageLayer &layer,
                                   const int channel) -> void {
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      /* An effect correction is the one kind that carries a parameter hash of its own, and the
       * refusal above already dropped any effect the preview cannot compute itself. */
      if (correction.opacity <= 0.0f || !paint_correction_is_effect(correction)) {
        continue;
      }
      GpuEffectJob job;
      job.correction = &correction;
      job.channel = channel;
      /* The effects before this one in the same row feed its input: their markers and parameter
       * hashes order-dependently re-key this entry. */
      job.chain_hash = BKE_paint_layers_effect_chain_hash(
          Span<const PaintMaterialCompositeCorrection>(
              layer.content_corrections.data(), int64_t(layer.content_corrections.size())),
          correction.effect_marker);
      r_jobs.append(std::move(job));
    }
    for (const PaintMaterialCompositeImageLayer &child : layer.children) {
      self(self, child, channel);
    }
  };
  for (const int i : IndexRange(stacks.size())) {
    if (!has_stack[i] || (wanted_channel >= 0 && i != wanted_channel)) {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : stacks[i]) {
      effect_jobs_walk(effect_jobs_walk, layer, channel_enums[i]);
    }
  }
  for (GpuEffectJob &job : r_jobs) {
    /* The description names the effect by its marker but not the row it sits on, and the input has
     * to be described from both: find them in the material the same way the CPU cache's caller
     * does. */
    job.effect = BKE_paint_layers_find(ma, job.correction->effect_marker);
    if (job.effect == nullptr || !BKE_paint_layers_effect_is_gpu_computed(*job.effect)) {
      return gpu_refuse("a GPU effect row cannot be resolved");
    }
    job.row = BKE_paint_layers_correction_owner(ma, *job.effect);
    if (job.row == nullptr) {
      return gpu_refuse("a GPU effect has no owner row");
    }
  }

  /* The cache first, and each input's size, plan and fingerprints with it: a hit costs no dispatch,
   * and the versions of the input's maps are read as the frame sees them. The input is described once
   * here (describe-only, no slice rendered) and the same description is re-used by the render below. */
  bke::paint_perf::ScopedTimer perf_cache("gpu_effect.cache");
  int effect_hits = 0;
  /* The effects of one row chain through each other's results, and the walk collected them in
   * the row's own order, so the halo of the ones before this job is a running sum that resets
   * when the row changes. */
  const MaterialPaintLayer *chain_row = nullptr;
  int chain_channel = -1;
  int chain_halo = 0;
  for (GpuEffectJob &job : r_jobs) {
    if (!BKE_paint_layers_effect_input_describe(
            ma, *job.row, *job.effect, job.channel, /*dimensions_only=*/false,
            /*describe_only=*/true, job.input_entry))
    {
      return gpu_refuse("an effect input cannot be described");
    }
    int width = 0;
    int height = 0;
    if (!BKE_paint_material_composite_stack_dimensions(
            Span<PaintMaterialCompositeImageLayer>(&job.input_entry, 1), width, height) ||
        width <= 0 || height <= 0)
    {
      return gpu_refuse("an effect input has no dimensions");
    }
    job.input_size = int2(width, height);
    /* The jobs run channel by channel, so the same row comes round again for the next channel: its
     * chain starts over there as well, or the halo of the previous channel's effects would widen
     * every region of the next. */
    if (job.row != chain_row || job.channel != chain_channel) {
      chain_row = job.row;
      chain_channel = job.channel;
      chain_halo = 0;
    }
    job.predecessor_halo = chain_halo;
    BKE_paint_layers_effect_input_images(job.input_entry, job.images);
    /* The pass plan, the neighbourhood halo, the scalar fingerprint of the input, and whether the
     * region the cache reports can narrow the dispatch -- all decided from the description. */
    const float *params = job.correction->filter_params;
    job.input_scalar_hash = BKE_paint_layers_effect_input_scalar_hash(job.input_entry);
    job.halo = BKE_paint_layers_effect_halo(
        eMaterialPaintLayerFilter(job.correction->filter), params, width, height);
    chain_halo += job.halo;
    BKE_paint_layers_effect_pass_plan(
        eMaterialPaintLayerFilter(job.correction->filter), params, width, height, job.passes);
    if (job.passes.is_empty()) {
      return gpu_refuse("a GPU effect has no pass plan");
    }
    void *resource = nullptr;
    const PaintLayerEffectGpuState state = BKE_paint_layers_effect_gpu_cache_get(
        ma,
        *job.effect,
        job.channel,
        width,
        height,
        job.images,
        PaintLayerEffectGpuKeyExtras{job.chain_hash, job.input_scalar_hash},
        effect_gpu_hooks,
        &job.region,
        &resource);
    if (state == PaintLayerEffectGpuState::Failed || resource == nullptr) {
      return gpu_refuse("a GPU effect texture could not be allocated");
    }
    job.result = static_cast<gpu::Texture *>(resource);
    /* A reference of the frame's own: the cache may evict the entry -- or replace it, the effect
     * having been edited -- while these passes still bind its texture. */
    GPU_texture_ref(job.result);
    job.needs_render = state == PaintLayerEffectGpuState::Render;
    job.region_exact = BKE_paint_layers_effect_region_is_exact(
        job.input_entry, job.images, width, height);
    passes.effect_results.append(GpuEffectResult{
        .marker = job.correction->effect_marker, .channel = job.channel, .texture = job.result});
    if (!job.needs_render) {
      effect_hits++;
    }
  }
  perf_cache.note("effects=%d hit=%d render=%d budget=%d MiB",
                  int(r_jobs.size()),
                  effect_hits,
                  int(r_jobs.size()) - effect_hits,
                  int(BKE_paint_layers_effect_gpu_cache_budget() / (1024 * 1024)));

  {
    bke::paint_perf::ScopedTimer perf_pass("gpu_effect.pass");
    /* The working space's luminance weights, which `IMB_colormanagement_get_luminance` reads: the
     * shader's own Rec.709 triple would grain a different grey in another space. */
    float luma[3];
    IMB_colormanagement_get_luminance_coefficients(luma);
    const float4 luma_coefficients(luma[0], luma[1], luma[2], 0.0f);

    /* The textures one job's plan runs over: the rendered input, the luminance a grayscale High
     * Pass keeps, and the two ring slots the window passes ping-pong through. The plan names its
     * buffers by role, so the dispatch loop below is a straight walk of the plan and adding a
     * pass kind touches the plan builder and the shader only. */
    class EffectPassBuffers {
     public:
      EffectPassBuffers(gpu::Texture *input,
                        gpu::Texture *luminance,
                        gpu::Texture *ring_a,
                        gpu::Texture *ring_b,
                        gpu::Texture *result)
          : input_(input), luminance_(luminance), ring_{ring_a, ring_b}, result_(result)
      {
      }

      gpu::Texture *source(const ePaintLayerEffectPassSource role) const
      {
        switch (role) {
          case ePaintLayerEffectPassSource::Input:
            return input_;
          case ePaintLayerEffectPassSource::Previous:
            return previous_;
          case ePaintLayerEffectPassSource::Luminance:
            return luminance_;
        }
        return input_;
      }

      gpu::Texture *aux(const ePaintLayerEffectPassAux role) const
      {
        switch (role) {
          case ePaintLayerEffectPassAux::None:
            return nullptr;
          case ePaintLayerEffectPassAux::Previous:
            return previous_;
          case ePaintLayerEffectPassAux::Original:
            return input_;
        }
        return nullptr;
      }

      /* The buffer \a pass writes: the cache texture on the last pass, the luminance buffer for
       * the pre-pass, and a free ring slot for every pass in between. */
      gpu::Texture *destination(const PaintLayerEffectPass &pass) const
      {
        if (pass.pass_index == pass.pass_count - 1) {
          return result_;
        }
        if (pass.kind == ePaintLayerEffectPass::Luminance) {
          return luminance_;
        }
        return ring_[ring_i_ ^ 1];
      }

      void commit(const PaintLayerEffectPass &pass, gpu::Texture *dst)
      {
        if (dst != result_ && pass.kind != ePaintLayerEffectPass::Luminance) {
          ring_i_ ^= 1;
        }
        previous_ = dst;
      }

     private:
      gpu::Texture *input_;
      gpu::Texture *luminance_;
      gpu::Texture *ring_[2];
      gpu::Texture *result_;
      gpu::Texture *previous_ = nullptr;
      int ring_i_ = 0;
    };

    int rendered = 0;
    int last_kind = 0;
    int last_radius = 0;
    int last_passes = 0;
    rcti last_region;
    BLI_rcti_init(&last_region, 0, 0, 0, 0);
    bool last_full = true;

    /* Effects render in the order they were collected: an effect's input is laid from the results of
     * the ones before it, which are published by the time its own input is rendered. */
    for (GpuEffectJob &job : r_jobs) {
      if (!job.needs_render) {
        continue;
      }
      /* The scratch of this job alone, returned to the pool when the job ends -- the pass results
       * the later effects read live in the GPU result cache, so nothing here has to outlive the
       * publish below, and the next job of the same size reuses the textures instead of the frame
       * growing by a set of canvases per effect. */
      Vector<gpu::Texture *> job_scratch;
      BLI_SCOPED_DEFER([&]() {
        for (gpu::Texture *texture : job_scratch) {
          passes.pool.release_texture(texture);
        }
      });
      const auto job_acquire = [&](const int2 canvas_size) -> gpu::Texture * {
        gpu::Texture *texture = passes.pool.acquire_texture_2d(canvas_size,
                                                               1,
                                                               gpu::TextureFormat::SFLOAT_16_16_16_16,
                                                               passes.accum_usage,
                                                               "combined_effect_scratch");
        if (texture != nullptr) {
          job_scratch.append(texture);
        }
        return texture;
      };
      const int2 canvas = job.input_size;
      const int n = int(job.passes.size());

      /* The content the effect reads, laid into a canvas of the input's own resolution and texel
       * space. The input is rendered whole (no region): a capture has no region of its own, and its
       * cost is measured against the effect passes below -- if it dominates, a regional input is the
       * next step. */
      {
        bke::paint_perf::ScopedTimer perf_input("gpu_effect.input");
        CombinedTarget effect_target;
        /* The folder accumulators this target grows are its own: released when the job's scope ends, on
         * every path out of it, because the pool keeps a texture that is never released until the
         * context's pool is destroyed. */
        BLI_SCOPED_DEFER([&]() {
          for (gpu::Texture *texture : effect_target.folder_premul) {
            if (texture != nullptr) {
              passes.pool.release_texture(texture);
            }
          }
          for (gpu::Texture *texture : effect_target.folder_cover) {
            if (texture != nullptr) {
              passes.pool.release_texture(texture);
            }
          }
        });
        effect_target.size = canvas;
        effect_target.groups = math::divide_ceil(canvas, int2(16));
        effect_target.texel_to_uv = math::from_scale<float3x3, 2>(1.0f / float2(canvas));
        effect_target.texel_to_uv_4x4 = float4x4::identity();
        effect_target.texel_to_uv_4x4[0] = float4(
            effect_target.texel_to_uv[0].x, effect_target.texel_to_uv[0].y, 0.0f, 0.0f);
        effect_target.texel_to_uv_4x4[1] = float4(
            effect_target.texel_to_uv[1].x, effect_target.texel_to_uv[1].y, 0.0f, 0.0f);
        effect_target.texel_to_uv_4x4[3] = float4(
            effect_target.texel_to_uv[2].x, effect_target.texel_to_uv[2].y, 0.0f, 1.0f);
        /* The four textures the input's row_emit reads, held only while it lays: the defer
         * returns them the moment this block ends, so the passes below run against the input and
         * the ping-pong alone. */
        gpu::Texture *target_scratch[4] = {nullptr, nullptr, nullptr, nullptr};
        BLI_SCOPED_DEFER([&]() {
          for (gpu::Texture *texture : target_scratch) {
            if (texture != nullptr) {
              passes.pool.release_texture(texture);
            }
          }
        });
        const auto target_acquire = [&](const int2 canvas_size) -> gpu::Texture * {
          gpu::Texture *texture = passes.pool.acquire_texture_2d(canvas_size,
                                                                 1,
                                                                 gpu::TextureFormat::SFLOAT_16_16_16_16,
                                                                 passes.accum_usage,
                                                                 "combined_effect_scratch");
          for (gpu::Texture *&slot : target_scratch) {
            if (slot == nullptr) {
              slot = texture;
              break;
            }
          }
          return texture;
        };
        effect_target.row_color = target_acquire(canvas);
        effect_target.row_factor = target_acquire(canvas);
        effect_target.mask_accum = target_acquire(canvas);
        /* The capture returns before anything blends onto the destination, but the layer pass binds it
         * as an image all the same: a placeholder of its own keeps it from being the texture the same
         * dispatch samples. */
        effect_target.dest = target_acquire(int2(1));
        job.input = job_acquire(canvas);
        if (effect_target.row_color == nullptr || effect_target.row_factor == nullptr ||
            effect_target.mask_accum == nullptr || effect_target.dest == nullptr ||
            job.input == nullptr)
        {
          return gpu_refuse("an effect input texture could not be allocated");
        }
        /* An effect's input has no mask to build (the describe clears the row's): refuse rather than
         * rely on that, since the capture's store step would write a canvas into the one-texel
         * placeholder. */
        if (!job.input_entry.mask_items.is_empty() || job.input_entry.mask_image != nullptr) {
          return gpu_refuse("an effect input carries a mask");
        }
        if (!passes.row_emit(effect_target, job.input_entry, job.channel, -1, job.input, nullptr)) {
          return false;
        }
        /* The alpha the CPU buffer holds when the kernel is done: the content alpha where the channel
         * tracks one, one elsewhere (`eval_row_content`'s rule), because the correction pass divides the
         * result by it as the CPU evaluator straightens a float buffer. */
        job.tracks_alpha = job.input_entry.tracks_content_alpha ||
                           job.channel == PAINT_MATERIAL_CHANNEL_NORMAL;
        GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
        perf_input.note("kind=%d size=%dx%d full=1", int(job.correction->filter), canvas.x, canvas.y);
      }

      /* The luminance buffer a grayscale High Pass keeps, and the two ring slots the window
       * passes ping-pong through. Each is acquired only when the plan uses it: a box blur needs no
       * luminance buffer, a pointwise Noise needs neither, and the job's scratch defer above hands
       * every one of them back to the pool when the job's scope ends. */
      gpu::Texture *scratch[3] = {nullptr, nullptr, nullptr};
      if (!job.passes.is_empty() && job.passes[0].kind == ePaintLayerEffectPass::Luminance) {
        scratch[0] = job_acquire(canvas);
        if (scratch[0] == nullptr) {
          return gpu_refuse("an effect scratch texture could not be allocated");
        }
      }
      if (n > 1) {
        scratch[1] = job_acquire(canvas);
        scratch[2] = job_acquire(canvas);
        if (scratch[1] == nullptr || scratch[2] == nullptr) {
          return gpu_refuse("an effect scratch texture could not be allocated");
        }
      }

      /* The dispatch rectangle of every pass: the dirty region grown by the reach the passes
       * after it still read, per axis, and by the halo of the effects before this one in the
       * row. The last pass writes the cache over the whole dirty + halo band. A render that is
       * not region exact -- or a first render, whose dirty region is the whole canvas -- runs
       * over the canvas instead. */
      Vector<rcti> pass_rects(n);
      {
        bke::paint_perf::ScopedTimer perf_region("gpu_effect.region");
        BKE_paint_layers_effect_pass_regions(
            Span<const PaintLayerEffectPass>(job.passes.data(), job.passes.size()),
            job.region,
            job.predecessor_halo,
            canvas.x,
            canvas.y,
            MutableSpan<rcti>(pass_rects.data(), pass_rects.size()));
        perf_region.note("kind=%d region=%dx%d full=%d halo=%d",
                         int(job.correction->filter),
                         BLI_rcti_size_x(&pass_rects[n - 1]),
                         BLI_rcti_size_y(&pass_rects[n - 1]),
                         job.region_exact ? 0 : 1,
                         job.halo);
      }
      rcti full_rect;
      BLI_rcti_init(&full_rect, 0, canvas.x, 0, canvas.y);
      const auto rect_for = [&](const int p) -> const rcti & {
        /* An empty dirty region on a render means the cache could not name what moved. */
        return (job.region_exact && !BLI_rcti_is_empty(&job.region)) ? pass_rects[p] : full_rect;
      };

      EffectPassBuffers buffers(job.input, scratch[0], scratch[1], scratch[2], job.result);
      for (const PaintLayerEffectPass &pass : job.passes) {
        gpu::Texture *dst = buffers.destination(pass);
        passes.effect_pass_dispatch({job,
                                     pass,
                                     buffers.source(pass.source),
                                     buffers.aux(pass.aux),
                                     dst,
                                     rect_for(pass.pass_index)},
                                    luma_coefficients);
        buffers.commit(pass, dst);
      }

      /* The versions are stamped after the render, so an edit that landed while it ran is newer than
       * this entry and costs one more update rather than a result that never clears. The hashes are
       * the ones the entry was keyed on above. */
      BKE_paint_layers_effect_gpu_cache_publish(ma,
                                                *job.effect,
                                                job.channel,
                                                job.input_size.x,
                                                job.input_size.y,
                                                job.images,
                                                PaintLayerEffectGpuKeyExtras{job.chain_hash,
                                                                             job.input_scalar_hash});
      rendered++;
      last_kind = int(job.correction->filter);
      last_radius = job.halo;
      last_passes = n;
      last_region = rect_for(n - 1);
      last_full = !job.region_exact || (BLI_rcti_size_x(&job.region) >= canvas.x &&
                                        BLI_rcti_size_y(&job.region) >= canvas.y);
    }
    if (rendered > 0) {
      /* The correction pass samples the results next. */
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    }
    perf_pass.note("effects=%d kind=%d halo=%d passes=%d region=%dx%d full=%d",
                   rendered,
                   last_kind,
                   last_radius,
                   last_passes,
                   BLI_rcti_size_x(&last_region),
                   BLI_rcti_size_y(&last_region),
                   last_full ? 1 : 0);
  }

  return true;
}

/** \} */

}  // namespace blender::ed::material_combined
