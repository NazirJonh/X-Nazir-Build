/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_math_color.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.h"
#include "BLI_task.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "BLI_time.h"

#include <atomic>
#include <cstdio>

#include "mesh_brush_common.hh"
#include "paint_debug.hh"
#include "paint_material_source.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_intern.hh"
#include "sculpt_paint_material.hh"

namespace blender::ed::sculpt_paint::material {

/* Per-dab printf profiling. Master switch is #PBR_PAINT_DEBUG_LOG in paint_debug.hh. */
#if PBR_PAINT_MATERIAL_PROFILE
namespace {
struct MaterialPaintProfile {
  std::atomic<int64_t> vert_num{0};
  std::atomic<int64_t> active_vert_num{0};
  std::atomic<double> factors_seconds{0.0};
  std::atomic<double> automask_seconds{0.0};
  std::atomic<double> sample_and_blend_seconds{0.0};

  void reset()
  {
    vert_num.store(0);
    active_vert_num.store(0);
    factors_seconds.store(0.0);
    automask_seconds.store(0.0);
    sample_and_blend_seconds.store(0.0);
  }
};
}  // namespace

static MaterialPaintProfile g_scalar_profile;
static MaterialPaintProfile g_color_profile;
#endif

/* -------------------------------------------------------------------- */
/** \name Local Data Structures
 * \{ */

struct MaterialPaintLocalData {
  Vector<float> factors;
  Vector<float> distances;

  /** Undecoded Base Color source samples for the active (factor > 0) verts of one node, batch-
   * decoded to scene-linear in one call instead of per-vertex. See the raster engine's identical
   * pattern in sculpt_paint_image.cc / #ChannelSourceSampler::color for why. */
  Vector<float3> raw_source_colors;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blending Functions for Scalar Values
 * \{ */

/**
 * Composites \a target (expanded to gray, scaled by \a factor) onto the per-stroke coverage
 * accumulator \a mix, always with Mix regardless of the channel's configured blend mode - mirrors
 * #blend_color_mix_float's use for #StrokeCache::paint_brush.mix_colors. \a mix and the returned
 * value are pre-multiplied (RGB already scaled by alpha), matching the "pre-multiplied alpha float
 * blending modes" convention documented in BLI_math_color_blend.h/math_color_blend_inline.cc.
 */
static float4 accumulate_scalar_coverage(const float4 &mix, const float target, const float factor)
{
  const float4 paint(target * factor, target * factor, target * factor, factor);
  float4 result;
  IMB_blend_color_float(result, mix, paint, IMB_BLEND_MIX);
  return result;
}

/**
 * Applies \a blend_mode to a single scalar, blending \a current (expanded to an opaque gray)
 * against the accumulated coverage \a target_mix (already pre-multiplied by its own alpha, as
 * built up by #accumulate_scalar_coverage - see the same block comment there).
 *
 * The IMB blend modes are defined on RGBA colors. Rather than reimplementing each one, the scalar
 * is expanded to a gray color and run through #IMB_blend_color_float, so a channel always blends
 * exactly like the equivalent gray value would in Base Color. This keeps every brush blend mode
 * working instead of silently falling back to Mix for the unimplemented ones.
 */
static float apply_scalar_blend(const float current,
                                const float4 &target_mix,
                                const IMB_BlendMode blend_mode)
{
  const float4 current_color(current, current, current, 1.0f);
  float4 result;
  IMB_blend_color_float(result, current_color, target_mix, blend_mode);
  return result.x;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Tasks
 * \{ */

static void do_paint_scalar_task(const Depsgraph &depsgraph,
                                 Object &object,
                                 const Span<float3> vert_positions,
                                 const Span<float3> vert_normals,
                                 const MeshAttributeData &attribute_data,
                                 const Brush &brush,
                                 const ChannelSourceSampler *sampler,
                                 const eMaterialPaintChannel channel,
                                 const StringRef attr_name,
                                 const float target_value,
                                 const float2 value_range,
                                 const IMB_BlendMode blend_mode,
                                 const Span<float> alpha_cache,
                                 bke::pbvh::MeshNode &node,
                                 MaterialPaintLocalData &tls,
                                 const MutableSpan<float4> mix_scalars,
                                 MutableSpan<float> attribute)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  tls.distances.resize(verts.size());

  const MutableSpan<float> factors = tls.factors;
  const MutableSpan<float> distances = tls.distances;

#if PBR_PAINT_MATERIAL_PROFILE
  const double factors_start = BLI_time_now_seconds();
#endif
  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   vert_positions,
                                   vert_normals,
                                   node,
                                   factors,
                                   distances);
#if PBR_PAINT_MATERIAL_PROFILE
  g_scalar_profile.factors_seconds.fetch_add(BLI_time_now_seconds() - factors_start);
  const double automask_start = BLI_time_now_seconds();
#endif

  if (cache.automasking) {
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, factors);
  }
#if PBR_PAINT_MATERIAL_PROFILE
  g_scalar_profile.automask_seconds.fetch_add(BLI_time_now_seconds() - automask_start);
  const double sample_start = BLI_time_now_seconds();
  int64_t active_num = 0;
#endif

  /* The value from before this stroke touched this vertex, frozen for the whole stroke (see
   * #orig_material_scalar_data_lookup_mesh); every dab blends against this instead of the
   * live (already-dab-modified) #attribute, so overlapping dabs within one stroke converge
   * smoothly instead of visibly banding wherever their falloffs overlap. */
  const Span<float> orig = *orig_material_scalar_data_lookup_mesh(object, node, attr_name);

  for (const int i : verts.index_range()) {
    const float factor = factors[i];
    if (factor <= 0.0f) {
      continue;
    }
    const int vert = verts[i];
    /* A source texture replaces the slider value; the brush falloff still rides in the blend
     * alpha, so the stroke shape is unchanged. */
    const float target = sampler != nullptr ?
                             sampler->scalar(channel, vert_positions[vert], thread_id) :
                             target_value;
    /* Alpha does not mask its own write; only every other channel's write is scaled. */
    const float alpha_factor = (!alpha_cache.is_empty() && channel != PAINT_MATERIAL_CHANNEL_ALPHA) ?
                                   alpha_cache[vert] :
                                   1.0f;
    float4 &mix = mix_scalars[vert];
    mix = accumulate_scalar_coverage(mix, target, factor * alpha_factor);
    const float blended = apply_scalar_blend(orig[i], mix, blend_mode);
    attribute[vert] = math::clamp(blended, value_range.x, value_range.y);
#if PBR_PAINT_MATERIAL_PROFILE
    active_num++;
#endif
  }
#if PBR_PAINT_MATERIAL_PROFILE
  g_scalar_profile.sample_and_blend_seconds.fetch_add(BLI_time_now_seconds() - sample_start);
  g_scalar_profile.vert_num.fetch_add(verts.size());
  g_scalar_profile.active_vert_num.fetch_add(active_num);
#endif
}

static void do_paint_color_task(const Depsgraph &depsgraph,
                                Object &object,
                                const Span<float3> vert_positions,
                                const Span<float3> vert_normals,
                                const OffsetIndices<int> faces,
                                const Span<int> corner_verts,
                                const GroupedSpan<int> vert_to_face_map,
                                const MeshAttributeData &attribute_data,
                                const Brush &brush,
                                const ChannelSourceSampler *sampler,
                                const eMaterialPaintChannel channel,
                                const float3 &target_rgb,
                                const IMB_BlendMode blend_mode,
                                const Span<float> alpha_cache,
                                bke::pbvh::MeshNode &node,
                                MaterialPaintLocalData &tls,
                                const MutableSpan<float4> mix_colors,
                                bke::GSpanAttributeWriter &color_attribute)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  tls.distances.resize(verts.size());

  const MutableSpan<float> factors = tls.factors;
  const MutableSpan<float> distances = tls.distances;

#if PBR_PAINT_MATERIAL_PROFILE
  const double factors_start = BLI_time_now_seconds();
#endif
  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   vert_positions,
                                   vert_normals,
                                   node,
                                   factors,
                                   distances);
#if PBR_PAINT_MATERIAL_PROFILE
  g_color_profile.factors_seconds.fetch_add(BLI_time_now_seconds() - factors_start);
  const double automask_start = BLI_time_now_seconds();
#endif

  if (cache.automasking) {
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, factors);
  }
#if PBR_PAINT_MATERIAL_PROFILE
  g_color_profile.automask_seconds.fetch_add(BLI_time_now_seconds() - automask_start);
  const double sample_start = BLI_time_now_seconds();
  int64_t active_num = 0;
#endif

  /* Base Color's source is typically a byte (non-scene-linear) image; decoding it one
   * #IMB_colormanagement_colorspace_to_scene_linear_v3 call per vertex is the dominant cost of
   * painting that channel (confirmed by profiling the raster engine's identical decode). Gather
   * the active verts' raw samples first and decode the whole node in one batched call instead. */
  const bool batch_decode_color = sampler != nullptr &&
                                  sampler->needs_linear_conversion(channel);
  if (batch_decode_color) {
    tls.raw_source_colors.resize(0);
    tls.raw_source_colors.reserve(verts.size());
    for (const int i : verts.index_range()) {
      if (factors[i] <= 0.0f) {
        continue;
      }
      tls.raw_source_colors.append(sampler->color(channel,
                                                   vert_positions[verts[i]],
                                                   thread_id,
                                                   /*decode_linear=*/false));
    }
    ChannelSourceSampler::decode_linear_batch(tls.raw_source_colors,
                                              sampler->colorspace(channel));
  }

  /* The color from before this stroke touched this vertex, frozen for the whole stroke; every
   * dab blends against this instead of the live (already-dab-modified) color attribute, so
   * overlapping dabs within one stroke converge smoothly instead of visibly banding wherever
   * their falloffs overlap - mirrors #sculpt_paint_color.cc's do_paint_task's use of
   * #orig_color_data_get_mesh.
   *
   * A stroke that paints Base Color together with at least one scalar channel pushes a single
   * #Type::Material undo step covering both (see #push_undo_nodes); a Base-Color-only stroke
   * pushes a plain #Type::Color step instead, so #orig_material_color_data_lookup_mesh (which
   * only recognizes #Type::Material) misses it and #orig_color_data_get_mesh is needed instead. */
  const std::optional<Span<float4>> orig_material = orig_material_color_data_lookup_mesh(object,
                                                                                          node);
  const Span<float4> orig = orig_material ? *orig_material : orig_color_data_get_mesh(object, node);

  int decoded_i = 0;
  for (const int i : verts.index_range()) {
    const float factor = factors[i];
    if (factor <= 0.0f) {
      continue;
    }
#if PBR_PAINT_MATERIAL_PROFILE
    active_num++;
#endif

    const int vert = verts[i];
    const float3 target = batch_decode_color ? tls.raw_source_colors[decoded_i++] :
                          sampler != nullptr ?
                              sampler->color(channel, vert_positions[vert], thread_id) :
                              target_rgb;
    /* Alpha does not mask its own write; only every other channel's write is scaled. */
    const float alpha_factor = (!alpha_cache.is_empty() && channel != PAINT_MATERIAL_CHANNEL_ALPHA) ?
                                   alpha_cache[vert] :
                                   1.0f;
    /* Alpha carries brush falloff for IMB blend modes (same convention as color paint); the RGB
     * must be pre-multiplied by that same alpha, since every blend_color_*_float mode expects a
     * pre-multiplied second color (see the "pre-multiplied alpha float blending modes" block
     * comment in BLI_math_color_blend.h/math_color_blend_inline.cc). */
    const float total_factor = factor * alpha_factor;
    const float4 paint_color(target.x * total_factor,
                             target.y * total_factor,
                             target.z * total_factor,
                             total_factor);
    float4 &mix = mix_colors[vert];
    IMB_blend_color_float(mix, mix, paint_color, IMB_BLEND_MIX);

    float4 result;
    IMB_blend_color_float(result, orig[i], mix, blend_mode);
    color::color_vert_set(faces,
                          corner_verts,
                          vert_to_face_map,
                          color_attribute.domain,
                          verts[i],
                          result,
                          color_attribute.span);
  }
#if PBR_PAINT_MATERIAL_PROFILE
  g_color_profile.sample_and_blend_seconds.fetch_add(BLI_time_now_seconds() - sample_start);
  g_color_profile.vert_num.fetch_add(verts.size());
  g_color_profile.active_vert_num.fetch_add(active_num);
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point: Material Painting
 * \{ */

bool paint_supported_on_object(const Scene &scene, Object &ob)
{
  if (ob.type != OB_MESH) {
    return false;
  }
  if (BKE_object_sculpt_use_dyntopo(&ob)) {
    return false;
  }
  return BKE_sculpt_multires_active(&scene, &ob) == nullptr;
}

Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> enabled_scalar_attribute_names(
    const BrushMaterialPaint &brush_paint, const PaintModeSettings &mode_settings)
{
  Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> names;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.is_color || !info.supports_vertex_paint ||
        !BKE_paint_material_channel_writes_to_target(brush_paint, mode_settings, info.channel))
    {
      continue;
    }
    /* A Custom channel pointed at one of the fixed attributes would otherwise be snapshotted
     * twice by undo. */
    names.append_non_duplicates(
        BKE_paint_material_channel_attribute_name(mode_settings, info.channel));
  }
  return names;
}

/**
 * Paints the scalar channel stored in \a attr_name.
 *
 * The attribute is expected to exist already: it is created once per stroke by #brush_stroke_init,
 * so that its creation is covered by the stroke's undo step and is not repeated for every dab.
 */
static void paint_scalar_channel(const Depsgraph &depsgraph,
                                 Object &ob,
                                 const IndexMask &node_mask,
                                 const Brush &brush,
                                 const StringRef attr_name,
                                 const ChannelSourceSampler *sampler,
                                 const eMaterialPaintChannel channel,
                                 const float target_value,
                                 const float2 value_range,
                                 const IMB_BlendMode blend_mode,
                                 const Span<float> alpha_cache,
                                 const Span<float3> vert_positions,
                                 const Span<float3> vert_normals,
                                 const MeshAttributeData &attribute_data,
                                 bke::MutableAttributeAccessor &attributes,
                                 bke::pbvh::Tree &pbvh,
                                 MutableSpan<bke::pbvh::MeshNode> nodes,
                                 threading::EnumerableThreadSpecific<MaterialPaintLocalData> &tls)
{
  bke::SpanAttributeWriter<float> attribute = attributes.lookup_for_write_span<float>(attr_name);
  if (!attribute) {
    /* Missing, or of an incompatible type. #brush_stroke_init already reported this. */
    return;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  SculptSession &ss = *ob.runtime->sculpt_session;
  Array<float4> &mix_scalars = ss.cache->material_mix_scalars[channel];
  if (mix_scalars.is_empty()) {
    mix_scalars = Array<float4>(mesh.verts_num, float4(0));
  }

  const MutableSpan<float> values = attribute.span;
  node_mask.foreach_index(
      [&](const int i) {
        do_paint_scalar_task(depsgraph,
                             ob,
                             vert_positions,
                             vert_normals,
                             attribute_data,
                             brush,
                             sampler,
                             channel,
                             attr_name,
                             target_value,
                             value_range,
                             blend_mode,
                             alpha_cache,
                             nodes[i],
                             tls.local(),
                             mix_scalars,
                             values);
      },
      exec_mode::grain_size(1));

  pbvh.tag_attribute_changed(node_mask, attr_name);
  attribute.finish();
}

/**
 * Fills #StrokeCache.material_alpha_cache for every vertex touched by \a node_mask this dab, so
 * the channel-painting passes below can read a precomputed Alpha factor instead of resampling
 * Alpha's source once per channel per vertex. Each PBVH node owns a disjoint set of vertices via
 * #bke::pbvh::MeshNode::verts() (the same iteration #do_paint_scalar_task already uses for its
 * writes), so concurrent nodes never write the same cache slot — no synchronization needed.
 *
 * No-op (and the cache is left empty) when \a sampler is null, matching how erase/invert and a
 * disabled Alpha channel are already handled by the caller.
 */
static void precompute_alpha_cache(Object & /*ob*/,
                                   const IndexMask &node_mask,
                                   const ChannelSourceSampler *sampler,
                                   const Span<float3> vert_positions,
                                   MutableSpan<bke::pbvh::MeshNode> nodes,
                                   Array<float> &alpha_cache)
{
  if (sampler == nullptr) {
    return;
  }

  node_mask.foreach_index(
      [&](const int i) {
        const int thread_id = BLI_task_parallel_thread_id(nullptr);
        bke::pbvh::MeshNode &node = nodes[i];
        for (const int vert : node.verts()) {
          const float value = sampler->scalar(
              PAINT_MATERIAL_CHANNEL_ALPHA, vert_positions[vert], thread_id);
          alpha_cache[vert] = math::clamp(value, 0.0f, 1.0f);
        }
      },
      exec_mode::grain_size(1));
}

static void paint_color_channel(
    const Depsgraph &depsgraph,
    const Sculpt &sd,
    Object &ob,
    const IndexMask &node_mask,
    const Brush &brush,
    const BrushMaterialPaint &brush_paint,
    const eMaterialPaintChannel channel,
    const PaintModeSettings &mode_settings,
    const ChannelSourceSampler *sampler,
    const IMB_BlendMode blend_mode,
    const Span<float> alpha_cache,
    const Span<float3> vert_positions,
    const Span<float3> vert_normals,
    const MeshAttributeData &attribute_data,
    bke::MutableAttributeAccessor &attributes,
    bke::pbvh::Tree &pbvh,
    MutableSpan<bke::pbvh::MeshNode> nodes,
    threading::EnumerableThreadSpecific<MaterialPaintLocalData> &tls)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRef attr_name = BKE_paint_material_channel_attribute_name(mode_settings, channel);

  bke::GSpanAttributeWriter color_attribute = attributes.lookup_for_write_span(attr_name);
  if (!color_attribute) {
    return;
  }
  if (!bke::mesh::is_color_attribute(
          {color_attribute.domain, bke::cpp_type_to_attribute_type(color_attribute.span.type())}))
  {
    color_attribute.finish();
    return;
  }

  SculptSession &ss = *ob.runtime->sculpt_session;
  const float3 target_rgb = BKE_paint_material_channel_color_get(
      brush_paint, sd.paint, brush, channel, ss.cache->toggle_settings.invert);

  Array<float4> &mix_colors = ss.cache->material_mix_base_color;
  if (mix_colors.is_empty()) {
    mix_colors = Array<float4>(mesh.verts_num, float4(0));
  }

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();

  node_mask.foreach_index(
      [&](const int i) {
        do_paint_color_task(depsgraph,
                            ob,
                            vert_positions,
                            vert_normals,
                            faces,
                            corner_verts,
                            vert_to_face_map,
                            attribute_data,
                            brush,
                            sampler,
                            channel,
                            target_rgb,
                            blend_mode,
                            alpha_cache,
                            nodes[i],
                            tls.local(),
                            mix_colors,
                            color_attribute);
      },
      exec_mode::grain_size(1));

  pbvh.tag_attribute_changed(node_mask, attr_name);
  color_attribute.finish();
}

void do_paint_material_brush(const Depsgraph &depsgraph,
                             const Sculpt &sd,
                             Object &ob,
                             const IndexMask &node_mask,
                             const PaintModeSettings &settings)
{
  PRF_scope(ProfileCategory::Editor);

  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  if (brush.material_paint == nullptr) {
    return;
  }
  const BrushMaterialPaint &brush_paint = *brush.material_paint;
  const SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    /* Material painting currently only supports mesh mode. */
    return;
  }

  /* The first step of a symmetry pass only seeds the stroke, matching color painting. */
  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    return;
  }

#if PBR_PAINT_MATERIAL_PROFILE
  const double step_start = BLI_time_now_seconds();
  g_scalar_profile.reset();
  g_color_profile.reset();
#endif

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  const bool invert = ss.cache->toggle_settings.invert;

  /* Erasing pulls the channel back to its neutral value and must not read a source texture. */
  const ChannelSourceSampler *sampler = invert ? nullptr :
                                                 ss.cache->material_source_sampler.get();
  const ChannelSourceSampler *active_sampler = (sampler != nullptr && sampler->is_active()) ?
                                                   sampler :
                                                   nullptr;

  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const MeshAttributeData attribute_data(mesh);

  threading::EnumerableThreadSpecific<MaterialPaintLocalData> all_tls;
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  const bool alpha_masking_active = !invert && active_sampler != nullptr &&
                                    BKE_paint_material_channel_masks_stroke(brush_paint, settings);
  if (alpha_masking_active) {
    precompute_alpha_cache(
        ob, node_mask, active_sampler, vert_positions, nodes, ss.cache->material_alpha_cache);
  }
  const Span<float> alpha_cache = alpha_masking_active ?
                                      ss.cache->material_alpha_cache.as_span() :
                                      Span<float>();

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!BKE_paint_material_channel_writes_to_target(brush_paint, settings, info.channel)) {
      continue;
    }

    const IMB_BlendMode channel_blend_mode = IMB_BlendMode(
        BKE_paint_material_channel_blend_mode(brush_paint, info.channel, invert));

    if (info.is_color) {
      paint_color_channel(depsgraph,
                          sd,
                          ob,
                          node_mask,
                          brush,
                          brush_paint,
                          info.channel,
                          settings,
                          active_sampler,
                          channel_blend_mode,
                          alpha_cache,
                          vert_positions,
                          vert_normals,
                          attribute_data,
                          attributes,
                          pbvh,
                          nodes,
                          all_tls);
      continue;
    }

    /* Normal is map-only (Image Texture → Normal Map); skip vertex float attributes. */
    if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      continue;
    }

    const float2 range = BKE_paint_material_channel_range(settings, info.channel);
    /* Erasing pulls the channel back to its neutral value; the blend mode that goes with that is
     * already decided by #BKE_paint_material_channel_blend_mode. */
    const float target_value = invert ?
                                   BKE_paint_material_channel_default_value(info.channel) :
                                   BKE_paint_material_channel_value(
                                       brush_paint, settings, info.channel);

    paint_scalar_channel(depsgraph,
                         ob,
                         node_mask,
                         brush,
                         BKE_paint_material_channel_attribute_name(settings, info.channel),
                         active_sampler,
                         info.channel,
                         target_value,
                         range,
                         channel_blend_mode,
                         alpha_cache,
                         vert_positions,
                         vert_normals,
                         attribute_data,
                         attributes,
                         pbvh,
                         nodes,
                         all_tls);
  }

#if PBR_PAINT_MATERIAL_PROFILE
  const double step_seconds = BLI_time_now_seconds() - step_start;
  /* One line per brush dab step per symmetry pass: cheap enough to leave on for a whole stroke
   * and shows the factors/automasking/sample+blend split, which the per-stroke sampler summary
   * cannot. */
  printf(
      "[pbr_paint] do_paint_material_brush step: total=%.3fms | scalar verts=%lld/%lld "
      "factors=%.3fms automask=%.3fms sample+blend=%.3fms | color verts=%lld/%lld "
      "factors=%.3fms automask=%.3fms sample+blend=%.3fms\n",
      step_seconds * 1000.0,
      static_cast<long long>(g_scalar_profile.active_vert_num.load()),
      static_cast<long long>(g_scalar_profile.vert_num.load()),
      g_scalar_profile.factors_seconds.load() * 1000.0,
      g_scalar_profile.automask_seconds.load() * 1000.0,
      g_scalar_profile.sample_and_blend_seconds.load() * 1000.0,
      static_cast<long long>(g_color_profile.active_vert_num.load()),
      static_cast<long long>(g_color_profile.vert_num.load()),
      g_color_profile.factors_seconds.load() * 1000.0,
      g_color_profile.automask_seconds.load() * 1000.0,
      g_color_profile.sample_and_blend_seconds.load() * 1000.0);
#endif
}

/** \} */

}  // namespace blender::ed::sculpt_paint::material
