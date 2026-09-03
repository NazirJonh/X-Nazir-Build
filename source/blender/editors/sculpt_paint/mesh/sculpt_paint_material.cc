/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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

#include "BLI_array.hh"
#include "BLI_assert.h"
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
#include "paint_material_blend.hh"
#include "paint_material_source.hh"
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
  Vector<float> distances;

  /** Source samples for one node's vertices, gathered in one #ChannelSourceSampler call rather
   * than one per vertex, so the sampler resolves its texture mapping once per node. For Base
   * Color these are also left undecoded and converted to scene-linear by a single batched call;
   * see the raster engine's identical pattern in sculpt_paint_image.cc. */
  Vector<float3> source_colors;
  Vector<float> source_values;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Tasks
 * \{ */

/**
 * Paints one PBVH node's slice of a scalar channel.
 *
 * \a factors, \a alpha and \a source_values are all node-local: element `i` describes
 * `node.verts()[i]`. \a factors is the dab's shared brush falloff (see
 * #StrokeCache::MaterialDabScratch), \a alpha is empty unless the stroke masks with the Alpha
 * channel, and \a source_values is empty unless this channel has a usable source texture, in
 * which case it replaces \a target_value per vertex.
 */
static void do_paint_scalar_task(Object &object,
                                 const eMaterialPaintChannel channel,
                                 const int orig_attribute_index,
                                 const float target_value,
                                 const float2 value_range,
                                 const IMB_BlendMode blend_mode,
                                 const float brush_alpha,
                                 const Span<float> alpha,
                                 const Span<float> factors,
                                 const Span<float> source_values,
                                 bke::pbvh::MeshNode &node,
                                 const MutableSpan<float2> mix_scalars,
                                 MutableSpan<float> attribute)
{
  const Span<int> verts = node.verts();
  BLI_assert(factors.size() == verts.size());
#if PBR_PAINT_MATERIAL_PROFILE
  const double sample_start = BLI_time_now_seconds();
  int64_t active_num = 0;
#endif

  /* The value from before this stroke touched this vertex, frozen for the whole stroke (see
   * #orig_material_scalar_data_lookup_mesh); every dab blends against this instead of the
   * live (already-dab-modified) #attribute, so overlapping dabs within one stroke converge
   * smoothly instead of visibly banding wherever their falloffs overlap. */
  const std::optional<Span<float>> orig_lookup = orig_material_scalar_data_lookup_mesh(
      object, node, orig_attribute_index);
  if (!orig_lookup) {
    /* This node has no undo data in the current step, so there is nothing to blend against.
     * #push_undo_nodes runs over the same node mask this dab paints, so the two can only disagree
     * if that invariant is broken - which would silently corrupt undo, hence the assert rather
     * than a quiet fallback. */
    BLI_assert_unreachable();
    return;
  }
  const Span<float> orig = *orig_lookup;

  const bool use_alpha = channel_uses_alpha_mask(!alpha.is_empty(), channel);

  for (const int i : verts.index_range()) {
    const float factor = factors[i];
    if (factor <= 0.0f) {
      continue;
    }
    /* A source texture replaces the slider value; the brush falloff still rides in the blend
     * alpha, so the stroke shape is unchanged. */
    const float target = source_values.is_empty() ? target_value : source_values[i];
    /* Alpha does not mask its own write; only every other channel's write is scaled. */
    const float alpha_factor = use_alpha ? alpha[i] : 1.0f;
    float2 &mix = mix_scalars[verts[i]];
    mix = accumulate_scalar_coverage(mix, target, factor * alpha_factor);
    /* #mix is the stroke's running coverage accumulator and must stay unscaled so later dabs
     * keep converging against the same value; the brush Strength slider only caps how much of
     * that coverage shows through this composite, mirroring the separate `alpha` scale
     * #sculpt_paint_color.cc's do_paint_brush_task applies to its own accumulator at composite
     * time rather than folding it into the accumulator itself. */
    const float2 composite_mix = mix * brush_alpha;
    const float blended = apply_scalar_blend(orig[i], composite_mix, blend_mode);
    attribute[verts[i]] = math::clamp(blended, value_range.x, value_range.y);
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

/** Base Color counterpart to #do_paint_scalar_task; the same node-local span convention applies,
 * with \a source_colors standing in for \a source_values. */
static void do_paint_color_task(Object &object,
                                const OffsetIndices<int> faces,
                                const Span<int> corner_verts,
                                const GroupedSpan<int> vert_to_face_map,
                                const eMaterialPaintChannel channel,
                                const float3 &target_rgb,
                                const IMB_BlendMode blend_mode,
                                const float brush_alpha,
                                const int orig_attribute_index,
                                const Span<float> alpha,
                                const Span<float> factors,
                                const Span<float3> source_colors,
                                bke::pbvh::MeshNode &node,
                                const MutableSpan<float4> mix_colors,
                                bke::GSpanAttributeWriter &color_attribute)
{
  const Span<int> verts = node.verts();
  BLI_assert(factors.size() == verts.size());
#if PBR_PAINT_MATERIAL_PROFILE
  const double sample_start = BLI_time_now_seconds();
  int64_t active_num = 0;
#endif

  /* The color from before this stroke touched this vertex, frozen for the whole stroke; every
   * dab blends against this instead of the live (already-dab-modified) color attribute, so
   * overlapping dabs within one stroke converge smoothly instead of visibly banding wherever
   * their falloffs overlap - mirrors #sculpt_paint_color.cc's do_paint_task's use of
   * #orig_color_data_get_mesh.
   *
   * Every material-paint color stroke now pushes a #Type::Material undo step with this
   * attribute's name in #undo::StepData::material_attributes (see #push_undo_nodes /
   * #enabled_color_attribute_names) - the old Type::Color-only fallback for a Base-Color-only
   * stroke is gone, so #orig_material_color_data_lookup_mesh always applies here. */
  BLI_assert(orig_attribute_index >= 0);
  const std::optional<Span<float4>> orig_material = orig_material_color_data_lookup_mesh(
      object, node, orig_attribute_index);
  if (!orig_material) {
    /* This node has no undo data in the current step - mirrors #do_paint_scalar_task's identical
     * guard; #push_undo_nodes runs over the same node mask this dab paints. */
    BLI_assert_unreachable();
    return;
  }
  const Span<float4> orig = *orig_material;

  const bool use_alpha = channel_uses_alpha_mask(!alpha.is_empty(), channel);

  for (const int i : verts.index_range()) {
    const float factor = factors[i];
    if (factor <= 0.0f) {
      continue;
    }
#if PBR_PAINT_MATERIAL_PROFILE
    active_num++;
#endif

    const float3 target = source_colors.is_empty() ? target_rgb : source_colors[i];
    /* Alpha does not mask its own write; only every other channel's write is scaled. */
    const float alpha_factor = use_alpha ? alpha[i] : 1.0f;
    /* Alpha carries brush falloff for IMB blend modes (same convention as color paint); the RGB
     * must be pre-multiplied by that same alpha, since every blend_color_*_float mode expects a
     * pre-multiplied second color (see the "pre-multiplied alpha float blending modes" block
     * comment in BLI_math_color_blend.h/math_color_blend_inline.cc). */
    const float total_factor = factor * alpha_factor;
    const float4 paint_color(target.x * total_factor,
                             target.y * total_factor,
                             target.z * total_factor,
                             total_factor);
    float4 &mix = mix_colors[verts[i]];
    IMB_blend_color_float(mix, mix, paint_color, IMB_BLEND_MIX);

    /* #mix is the stroke's running coverage accumulator and must stay unscaled so later dabs
     * keep converging against the same value; the brush Strength slider only caps how much of
     * that coverage shows through this composite, mirroring the separate `alpha` scale
     * #sculpt_paint_color.cc's do_paint_brush_task applies to its own accumulator at composite
     * time rather than folding it into the accumulator itself. */
    const float4 composite_mix = mix * brush_alpha;
    const float4 result = composite_coverage(orig[i], composite_mix, blend_mode);
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
    const BrushMaterialPaint &brush_paint,
    const PaintModeSettings &mode_settings,
    const int visible_material_channels)
{
  Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> names;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.is_color || !info.supports_vertex_paint ||
        !BKE_paint_material_channel_writes_to_target(
            brush_paint, mode_settings, visible_material_channels, info.channel))
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

Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> enabled_color_attribute_names(
    const BrushMaterialPaint &brush_paint,
    const PaintModeSettings &mode_settings,
    const int visible_material_channels)
{
  Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> names;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!info.is_color || !info.supports_vertex_paint ||
        !BKE_paint_material_channel_writes_to_target(
            brush_paint, mode_settings, visible_material_channels, info.channel))
    {
      continue;
    }
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
static void paint_scalar_channel(Object &ob,
                                 const IndexMask &node_mask,
                                 const StringRef attr_name,
                                 const ChannelSourceSampler *sampler,
                                 const eMaterialPaintChannel channel,
                                 const float target_value,
                                 const float2 value_range,
                                 const IMB_BlendMode blend_mode,
                                 const float brush_alpha,
                                 const StrokeCache::MaterialDabScratch &dab,
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

  /* Resolved once for the whole channel; #do_paint_scalar_task would otherwise redo the name
   * comparison for every node of every dab. */
  const int orig_attribute_index = orig_material_scalar_attribute_index(ob, attr_name);
  if (orig_attribute_index < 0) {
    /* The stroke's undo step does not cover this attribute. #push_undo_nodes derives its list
     * from #enabled_scalar_attribute_names, i.e. the same predicate that selected this channel
     * for painting, so this cannot happen unless that invariant is broken. */
    BLI_assert_unreachable();
    attribute.finish();
    return;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  SculptSession &ss = *ob.runtime->sculpt_session;
  Array<float2> &mix_scalars = ss.cache->material_mix_scalars[channel];
  if (mix_scalars.is_empty()) {
    mix_scalars = Array<float2>(mesh.verts_num, float2(0));
  }

  const bool use_source = sampler != nullptr;
  const MutableSpan<float> values = attribute.span;
  node_mask.foreach_index(
      [&](const int i, const int pos) {
        const IndexRange slice = dab.node_range(pos);
        const Span<float> factors = dab.factors.as_span().slice(slice);

        Span<float> source_values;
        if (use_source) {
          /* Gathered for the whole node in one call, so the sampler resolves its texture
           * mapping (Area / View / ...) once per node instead of once per vertex. */
          MaterialPaintLocalData &local = tls.local();
          local.source_values.resize(slice.size());
          sampler->gather_scalars(channel,
                                  dab.contexts.as_span().slice(slice),
                                  factors,
                                  BLI_task_parallel_thread_id(nullptr),
                                  local.source_values);
          source_values = local.source_values;
        }

        do_paint_scalar_task(ob,
                             channel,
                             orig_attribute_index,
                             target_value,
                             value_range,
                             blend_mode,
                             brush_alpha,
                             dab.alpha.is_empty() ? Span<float>() :
                                                    dab.alpha.as_span().slice(slice),
                             factors,
                             source_values,
                             nodes[i],
                             mix_scalars,
                             values);
      },
      exec_mode::grain_size(1));

  pbvh.tag_attribute_changed(node_mask, attr_name);
  attribute.finish();
}

static void paint_color_channel(
    const Sculpt &sd,
    Object &ob,
    const IndexMask &node_mask,
    const Brush &brush,
    const BrushMaterialPaint &brush_paint,
    const eMaterialPaintChannel channel,
    const PaintModeSettings &mode_settings,
    const ChannelSourceSampler *sampler,
    const IMB_BlendMode blend_mode,
    const float brush_alpha,
    const StrokeCache::MaterialDabScratch &dab,
    bke::MutableAttributeAccessor &attributes,
    bke::pbvh::Tree &pbvh,
    MutableSpan<bke::pbvh::MeshNode> nodes,
    threading::EnumerableThreadSpecific<MaterialPaintLocalData> &tls)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRef attr_name = BKE_paint_material_channel_attribute_name(mode_settings, channel);
  const int orig_attribute_index = orig_material_color_attribute_index(ob, attr_name);
  if (orig_attribute_index < 0) {
    /* The stroke's undo step does not cover this attribute. #push_undo_nodes derives its list
     * from #enabled_color_attribute_names, so this cannot happen unless that invariant is broken. */
    BLI_assert_unreachable();
    return;
  }

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

  const bool use_source = sampler != nullptr;
  /* Base Color's source is typically a byte (non-scene-linear) image, and decoding it with one
   * #IMB_colormanagement_colorspace_to_scene_linear_v3 call per vertex is the dominant cost of
   * painting the channel. Gather the node's samples still encoded and decode them all in one
   * batched call instead. */
  const bool batch_decode = use_source && sampler->needs_linear_conversion(channel);

  node_mask.foreach_index(
      [&](const int i, const int pos) {
        const IndexRange slice = dab.node_range(pos);
        const Span<float> factors = dab.factors.as_span().slice(slice);

        Span<float3> source_colors;
        if (use_source) {
          MaterialPaintLocalData &local = tls.local();
          local.source_colors.resize(slice.size());
          sampler->gather_colors(channel,
                                 dab.contexts.as_span().slice(slice),
                                 factors,
                                 BLI_task_parallel_thread_id(nullptr),
                                 !batch_decode,
                                 local.source_colors);
          if (batch_decode) {
            ChannelSourceSampler::decode_linear_batch(local.source_colors,
                                                      sampler->colorspace(channel));
          }
          source_colors = local.source_colors;
        }

        do_paint_color_task(ob,
                            faces,
                            corner_verts,
                            vert_to_face_map,
                            channel,
                            target_rgb,
                            blend_mode,
                            brush_alpha,
                            orig_attribute_index,
                            dab.alpha.is_empty() ? Span<float>() :
                                                   dab.alpha.as_span().slice(slice),
                            factors,
                            source_colors,
                            nodes[i],
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

  /* #StrokeCache::bstrength (see the shared factors pass below) only carries pressure, overlap
   * and feather - #brush_strength's #SCULPT_BRUSH_TYPE_PAINT case deliberately leaves the brush
   * Strength slider out of it. #sculpt_paint_color.cc's do_paint_brush_task applies that slider
   * separately, scaling its per-stroke coverage accumulator only at composite time so repeated
   * dabs still converge against the same accumulator; the two channel passes below mirror that. */
  const float brush_alpha = BKE_brush_alpha_get(&sd.paint, &brush);

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
                                    BKE_paint_material_channel_masks_stroke(
                                        brush_paint, settings, sd.paint.visible_material_channels);

  /* Lay out this dab's per-vertex scratch: one contiguous slice per node of \a node_mask, in mask
   * order. The buffers live in the stroke cache and are only ever grown, so a dab that is no
   * bigger than the previous one allocates nothing at all. */
  StrokeCache::MaterialDabScratch &dab = ss.cache->material_dab;
  dab.offsets.resize(node_mask.size() + 1);
  {
    int offset = 0;
    node_mask.foreach_index([&](const int i, const int pos) {
      dab.offsets[pos] = offset;
      offset += nodes[i].verts().size();
    });
    dab.offsets.last() = offset;
    dab.factors.resize(offset);
    dab.contexts.resize(active_sampler != nullptr ? offset : 0);
    dab.alpha.resize(alpha_masking_active ? offset : 0);
  }

#if PBR_PAINT_MATERIAL_PROFILE
  const double shared_factors_start = BLI_time_now_seconds();
#endif
  /* Brush falloff, the texture-sample coordinates and the Alpha mask are all channel-independent,
   * so they are built once here and reused by every channel pass below instead of being redone
   * per channel. */
  node_mask.foreach_index(
      [&](const int i, const int pos) {
        bke::pbvh::MeshNode &node = nodes[i];
        const Span<int> verts = node.verts();
        const IndexRange slice = dab.node_range(pos);
        const MutableSpan<float> factors = dab.factors.as_mutable_span().slice(slice);

        MaterialPaintLocalData &tls = all_tls.local();
        tls.distances.resize(verts.size());
        calc_factors_common_mesh_indexed(depsgraph,
                                         brush,
                                         ob,
                                         attribute_data,
                                         vert_positions,
                                         vert_normals,
                                         node,
                                         factors,
                                         tls.distances);
        /* NOTE: no automasking call here. #calc_factors_common_mesh_indexed already applies it
         * (as it does for every other brush); calling it again would multiply the automask factor
         * in a second time, squaring it. */
        /* #calc_factors_common_mesh_indexed does not fold in the brush Strength slider; every
         * other paint task (see #sculpt_paint_color.cc's do_paint_task) applies it explicitly
         * after computing the base falloff, which this channel-shared factor pass mirrors. */
        scale_factors(factors, ss.cache->bstrength);
        if (dab.contexts.is_empty()) {
          return;
        }
        const MutableSpan<TexelSampleContext> contexts = dab.contexts.as_mutable_span().slice(
            slice);
        for (const int vert_i : verts.index_range()) {
          /* A PBVH node reaches well beyond the brush radius, so most of its vertices have a zero
           * factor and are never sampled. Skipping the view projection for those is most of the
           * cost of this loop. They are still zeroed rather than left as whatever the previous
           * dab wrote, so that a sampler can never read a stale coordinate. */
          if (factors[vert_i] > 0.0f) {
            contexts[vert_i] = sculpt_texel_sample_context(ss, vert_positions[verts[vert_i]]);
          }
          else {
            contexts[vert_i] = {};
          }
        }
        if (dab.alpha.is_empty()) {
          return;
        }
        /* Alpha masks every other channel's write, so it is sampled here rather than once per
         * channel. It is gathered through the same batched call the channel passes use, which
         * both resolves the texture mapping once per node and skips the zero-factor vertices. */
        const MutableSpan<float> alpha = dab.alpha.as_mutable_span().slice(slice);
        active_sampler->gather_scalars(PAINT_MATERIAL_CHANNEL_ALPHA,
                                       contexts,
                                       factors,
                                       BLI_task_parallel_thread_id(nullptr),
                                       alpha);
        for (float &value : alpha) {
          value = math::clamp(value, 0.0f, 1.0f);
        }
      },
      exec_mode::grain_size(1));
#if PBR_PAINT_MATERIAL_PROFILE
  const double shared_factors_ms = (BLI_time_now_seconds() - shared_factors_start) * 1000.0;
  g_scalar_profile.factors_seconds.fetch_add(shared_factors_ms / 1000.0);
#endif

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!BKE_paint_material_channel_writes_to_target(
            brush_paint, settings, sd.paint.visible_material_channels, info.channel))
    {
      continue;
    }

    const IMB_BlendMode channel_blend_mode = IMB_BlendMode(
        BKE_paint_material_channel_blend_mode(brush_paint, info.channel, invert));

    if (info.is_color) {
      paint_color_channel(sd,
                          ob,
                          node_mask,
                          brush,
                          brush_paint,
                          info.channel,
                          settings,
                          active_sampler,
                          channel_blend_mode,
                          brush_alpha,
                          dab,
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

    paint_scalar_channel(ob,
                         node_mask,
                         BKE_paint_material_channel_attribute_name(settings, info.channel),
                         active_sampler,
                         info.channel,
                         target_value,
                         range,
                         channel_blend_mode,
                         brush_alpha,
                         dab,
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
