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
#include "paint_material_source.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_intern.hh"
#include "sculpt_paint_material.hh"

namespace blender::ed::sculpt_paint::material {

/* WORKAROUND: temporary printf profiling, paired with the one in paint_material_source.cc, to
 * see where time goes *within* a single #do_paint_material_brush call (one call per brush dab
 * per channel per symmetry pass, so many more data points per stroke than the sampler's one
 * per-stroke summary). Remove once the perf work is done. */
#define PBR_PAINT_MATERIAL_PROFILE 1

#ifdef PBR_PAINT_MATERIAL_PROFILE
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
 * Applies \a blend_mode to a single scalar.
 *
 * The IMB blend modes are defined on RGBA colors. Rather than reimplementing each one, the scalar
 * is expanded to a gray color and run through #IMB_blend_color_float, so a channel always blends
 * exactly like the equivalent gray value would in Base Color. This keeps every brush blend mode
 * working instead of silently falling back to Mix for the unimplemented ones.
 *
 * \param factor: brush falloff, passed as alpha the same way color painting does.
 */
static float apply_scalar_blend(const float current,
                                const float target,
                                const float factor,
                                const IMB_BlendMode blend_mode)
{
  const float4 current_color(current, current, current, 1.0f);
  const float4 target_color(target, target, target, factor);
  float4 result;
  IMB_blend_color_float(result, current_color, target_color, blend_mode);
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
                                 const float target_value,
                                 const float2 value_range,
                                 const IMB_BlendMode blend_mode,
                                 bke::pbvh::MeshNode &node,
                                 MaterialPaintLocalData &tls,
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

#ifdef PBR_PAINT_MATERIAL_PROFILE
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
#ifdef PBR_PAINT_MATERIAL_PROFILE
  g_scalar_profile.factors_seconds.fetch_add(BLI_time_now_seconds() - factors_start);
  const double automask_start = BLI_time_now_seconds();
#endif

  if (cache.automasking) {
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, factors);
  }
#ifdef PBR_PAINT_MATERIAL_PROFILE
  g_scalar_profile.automask_seconds.fetch_add(BLI_time_now_seconds() - automask_start);
  const double sample_start = BLI_time_now_seconds();
  int64_t active_num = 0;
#endif

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
    const float blended = apply_scalar_blend(attribute[vert], target, factor, blend_mode);
    attribute[vert] = math::clamp(blended, value_range.x, value_range.y);
#ifdef PBR_PAINT_MATERIAL_PROFILE
    active_num++;
#endif
  }
#ifdef PBR_PAINT_MATERIAL_PROFILE
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
                                const float3 &target_rgb,
                                const IMB_BlendMode blend_mode,
                                bke::pbvh::MeshNode &node,
                                MaterialPaintLocalData &tls,
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

#ifdef PBR_PAINT_MATERIAL_PROFILE
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
#ifdef PBR_PAINT_MATERIAL_PROFILE
  g_color_profile.factors_seconds.fetch_add(BLI_time_now_seconds() - factors_start);
  const double automask_start = BLI_time_now_seconds();
#endif

  if (cache.automasking) {
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, factors);
  }
#ifdef PBR_PAINT_MATERIAL_PROFILE
  g_color_profile.automask_seconds.fetch_add(BLI_time_now_seconds() - automask_start);
  const double sample_start = BLI_time_now_seconds();
  int64_t active_num = 0;
#endif

  /* Base Color's source is typically a byte (non-scene-linear) image; decoding it one
   * #IMB_colormanagement_colorspace_to_scene_linear_v3 call per vertex is the dominant cost of
   * painting that channel (confirmed by profiling the raster engine's identical decode). Gather
   * the active verts' raw samples first and decode the whole node in one batched call instead. */
  const bool batch_decode_color = sampler != nullptr &&
                                  sampler->needs_linear_conversion(
                                      PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (batch_decode_color) {
    tls.raw_source_colors.resize(0);
    tls.raw_source_colors.reserve(verts.size());
    for (const int i : verts.index_range()) {
      if (factors[i] <= 0.0f) {
        continue;
      }
      tls.raw_source_colors.append(sampler->color(PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                   vert_positions[verts[i]],
                                                   thread_id,
                                                   /*decode_linear=*/false));
    }
    ChannelSourceSampler::decode_linear_batch(
        tls.raw_source_colors, sampler->colorspace(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  }

  int decoded_i = 0;
  for (const int i : verts.index_range()) {
    const float factor = factors[i];
    if (factor <= 0.0f) {
      continue;
    }
#ifdef PBR_PAINT_MATERIAL_PROFILE
    active_num++;
#endif

    const float4 current = color::color_vert_get(faces,
                                                 corner_verts,
                                                 vert_to_face_map,
                                                 color_attribute.span,
                                                 color_attribute.domain,
                                                 verts[i]);
    const float3 target = batch_decode_color ? tls.raw_source_colors[decoded_i++] :
                          sampler != nullptr ?
                              sampler->color(PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                             vert_positions[verts[i]],
                                             thread_id) :
                              target_rgb;
    /* Alpha carries brush falloff for IMB blend modes (same convention as color paint). */
    const float4 paint_color(target.x, target.y, target.z, factor);
    float4 result;
    IMB_blend_color_float(result, current, paint_color, blend_mode);
    color::color_vert_set(faces,
                          corner_verts,
                          vert_to_face_map,
                          color_attribute.domain,
                          verts[i],
                          result,
                          color_attribute.span);
  }
#ifdef PBR_PAINT_MATERIAL_PROFILE
  g_color_profile.sample_and_blend_seconds.fetch_add(BLI_time_now_seconds() - sample_start);
  g_color_profile.vert_num.fetch_add(verts.size());
  g_color_profile.active_vert_num.fetch_add(active_num);
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point: Material Painting
 * \{ */

Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> enabled_scalar_attribute_names(
    const BrushMaterialPaint &brush_paint, const PaintModeSettings &mode_settings)
{
  Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> names;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.is_color || info.channel == PAINT_MATERIAL_CHANNEL_NORMAL ||
        !BKE_paint_material_channel_is_enabled(brush_paint, mode_settings, info.channel))
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
                             target_value,
                             value_range,
                             blend_mode,
                             nodes[i],
                             tls.local(),
                             values);
      },
      exec_mode::grain_size(1));

  pbvh.tag_attribute_changed(node_mask, attr_name);
  attribute.finish();
}

static void paint_base_color_channel(
    const Depsgraph &depsgraph,
    const Sculpt &sd,
    Object &ob,
    const IndexMask &node_mask,
    const Brush &brush,
    const BrushMaterialPaint &brush_paint,
    const PaintModeSettings &mode_settings,
    const ChannelSourceSampler *sampler,
    const IMB_BlendMode blend_mode,
    const Span<float3> vert_positions,
    const Span<float3> vert_normals,
    const MeshAttributeData &attribute_data,
    bke::MutableAttributeAccessor &attributes,
    bke::pbvh::Tree &pbvh,
    MutableSpan<bke::pbvh::MeshNode> nodes,
    threading::EnumerableThreadSpecific<MaterialPaintLocalData> &tls)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRef attr_name = BKE_paint_material_channel_attribute_name(
      mode_settings, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

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

  const SculptSession &ss = *ob.runtime->sculpt_session;
  const float3 target_rgb = BKE_paint_material_base_color_get(
      brush_paint, sd.paint, brush, ss.cache->toggle_settings.invert);

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
                            target_rgb,
                            blend_mode,
                            nodes[i],
                            tls.local(),
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

#ifdef PBR_PAINT_MATERIAL_PROFILE
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

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!BKE_paint_material_channel_is_enabled(brush_paint, settings, info.channel)) {
      continue;
    }

    const IMB_BlendMode channel_blend_mode = IMB_BlendMode(
        BKE_paint_material_channel_blend_mode(brush_paint, info.channel, invert));

    if (info.is_color) {
      paint_base_color_channel(depsgraph,
                               sd,
                               ob,
                               node_mask,
                               brush,
                               brush_paint,
                               settings,
                               active_sampler,
                               channel_blend_mode,
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
                         vert_positions,
                         vert_normals,
                         attribute_data,
                         attributes,
                         pbvh,
                         nodes,
                         all_tls);
  }

#ifdef PBR_PAINT_MATERIAL_PROFILE
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
