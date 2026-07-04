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
#include "BLI_task.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_intern.hh"
#include "sculpt_paint_material.hh"

namespace blender::ed::sculpt_paint::material {

/* -------------------------------------------------------------------- */
/** \name Local Data Structures
 * \{ */

struct MaterialPaintLocalData {
  Vector<float> factors;
  Vector<float> distances;
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
                                 const float target_value,
                                 const float2 value_range,
                                 const IMB_BlendMode blend_mode,
                                 bke::pbvh::MeshNode &node,
                                 MaterialPaintLocalData &tls,
                                 MutableSpan<float> attribute)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  tls.distances.resize(verts.size());

  const MutableSpan<float> factors = tls.factors;
  const MutableSpan<float> distances = tls.distances;

  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   vert_positions,
                                   vert_normals,
                                   node,
                                   factors,
                                   distances);

  if (cache.automasking) {
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, factors);
  }

  for (const int i : verts.index_range()) {
    const float factor = factors[i];
    if (factor <= 0.0f) {
      continue;
    }
    const int vert = verts[i];
    const float blended = apply_scalar_blend(attribute[vert], target_value, factor, blend_mode);
    attribute[vert] = math::clamp(blended, value_range.x, value_range.y);
  }
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
                                const float3 &target_rgb,
                                const IMB_BlendMode blend_mode,
                                bke::pbvh::MeshNode &node,
                                MaterialPaintLocalData &tls,
                                bke::GSpanAttributeWriter &color_attribute)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  tls.distances.resize(verts.size());

  const MutableSpan<float> factors = tls.factors;
  const MutableSpan<float> distances = tls.distances;

  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   vert_positions,
                                   vert_normals,
                                   node,
                                   factors,
                                   distances);

  if (cache.automasking) {
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, factors);
  }

  for (const int i : verts.index_range()) {
    const float factor = factors[i];
    if (factor <= 0.0f) {
      continue;
    }

    const float4 current = color::color_vert_get(faces,
                                                 corner_verts,
                                                 vert_to_face_map,
                                                 color_attribute.span,
                                                 color_attribute.domain,
                                                 verts[i]);
    /* Alpha carries brush falloff for IMB blend modes (same convention as color paint). */
    const float4 paint_color(target_rgb.x, target_rgb.y, target_rgb.z, factor);
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
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point: Material Painting
 * \{ */

Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> enabled_scalar_attribute_names(
    const PaintModeSettings &settings)
{
  Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> names;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.is_color || !BKE_paint_material_channel_is_enabled(settings, info.channel)) {
      continue;
    }
    /* A Custom channel pointed at one of the fixed attributes would otherwise be snapshotted
     * twice by undo. */
    names.append_non_duplicates(BKE_paint_material_channel_attribute_name(settings, info.channel));
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
    const PaintModeSettings &settings,
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
      settings, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

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
      settings, sd.paint, brush, ss.cache->toggle_settings.invert);

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

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  const bool invert = ss.cache->toggle_settings.invert;
  const IMB_BlendMode blend_mode = IMB_BlendMode(brush.blend);

  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const MeshAttributeData attribute_data(mesh);

  threading::EnumerableThreadSpecific<MaterialPaintLocalData> all_tls;
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!BKE_paint_material_channel_is_enabled(settings, info.channel)) {
      continue;
    }

    if (info.is_color) {
      paint_base_color_channel(depsgraph,
                               sd,
                               ob,
                               node_mask,
                               brush,
                               settings,
                               blend_mode,
                               vert_positions,
                               vert_normals,
                               attribute_data,
                               attributes,
                               pbvh,
                               nodes,
                               all_tls);
      continue;
    }

    const float2 range = BKE_paint_material_channel_range(settings, info.channel);
    const float value = BKE_paint_material_channel_value(settings, info.channel);
    /* Ctrl-invert mirrors the value across the range, so it also behaves sensibly for channels
     * whose range does not start at zero. This matches Base Color swapping to the secondary
     * color rather than doing nothing. */
    const float target_value = invert ? range.x + range.y - value : value;

    paint_scalar_channel(depsgraph,
                         ob,
                         node_mask,
                         brush,
                         BKE_paint_material_channel_attribute_name(settings, info.channel),
                         target_value,
                         range,
                         blend_mode,
                         vert_positions,
                         vert_normals,
                         attribute_data,
                         attributes,
                         pbvh,
                         nodes,
                         all_tls);
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::material
