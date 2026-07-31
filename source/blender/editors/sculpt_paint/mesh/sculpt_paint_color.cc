/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "DNA_brush_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BLI_color.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_hash.h"
#include "BLI_index_mask.hh"
#include "BLI_math_base.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "ED_mesh.hh"

#include "IMB_colormanagement.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_intern.hh"
#include "sculpt_smooth.hh"

#include "IMB_imbuf.hh"

#include <cmath>

namespace blender::ed::sculpt_paint::color {

static void calc_local_positions(const float4x4 &mat,
                                 const Span<int> verts,
                                 const Span<float3> positions,
                                 const MutableSpan<float3> local_positions)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : verts.index_range()) {
    local_positions[i] = math::transform_point(mat, positions[verts[i]]);
  }
}

template<typename Func> inline void to_static_color_type(const CPPType &type, const Func &func)
{
  if (type.is<ColorGeometry4f>()) {
    func(MPropCol());
  }
  else if (type.is<ColorGeometry4b>()) {
    func(MLoopCol());
  }
}

template<typename T> float4 to_float(const T &src);

template<> float4 to_float(const MLoopCol &src)
{
  float4 dst;
  rgba_uchar_to_float(dst, reinterpret_cast<const uchar *>(&src));
  IMB_colormanagement_srgb_to_scene_linear_v3(dst, dst);
  return dst;
}
template<> float4 to_float(const MPropCol &src)
{
  return src.color;
}

template<typename T> void from_float(const float4 &src, T &dst);

template<> void from_float(const float4 &src, MLoopCol &dst)
{
  float4 temp;
  IMB_colormanagement_scene_linear_to_srgb_v3(temp, src);
  temp[3] = src[3];
  rgba_float_to_uchar(reinterpret_cast<uchar *>(&dst), temp);
}
template<> void from_float(const float4 &src, MPropCol &dst)
{
  copy_v4_v4(dst.color, src);
}

template<typename T>
static float4 color_vert_get(const OffsetIndices<int> faces,
                             const Span<int> corner_verts,
                             const GroupedSpan<int> vert_to_face_map,
                             const GSpan color_attribute,
                             const bke::AttrDomain color_domain,
                             const int vert)
{
  const T *colors_typed = static_cast<const T *>(color_attribute.data());
  if (color_domain == bke::AttrDomain::Corner) {
    float4 r_color(0.0f);
    for (const int face : vert_to_face_map[vert]) {
      const int corner = bke::mesh::face_find_corner_from_vert(faces[face], corner_verts, vert);
      r_color += to_float(colors_typed[corner]);
    }
    return r_color / float(vert_to_face_map[vert].size());
  }
  return to_float(colors_typed[vert]);
}

template<typename T>
static void color_vert_set(const OffsetIndices<int> faces,
                           const Span<int> corner_verts,
                           const GroupedSpan<int> vert_to_face_map,
                           const GMutableSpan color_attribute,
                           const bke::AttrDomain color_domain,
                           const int vert,
                           const float4 &color)
{
  if (color_domain == bke::AttrDomain::Corner) {
    for (const int i_face : vert_to_face_map[vert]) {
      const IndexRange face = faces[i_face];
      MutableSpan<T> colors{static_cast<T *>(color_attribute.data()) + face.start(), face.size()};
      Span<int> face_verts = corner_verts.slice(face);

      for (const int i : IndexRange(face.size())) {
        if (face_verts[i] == vert) {
          from_float(color, colors[i]);
        }
      }
    }
  }
  else {
    from_float(color, static_cast<T *>(color_attribute.data())[vert]);
  }
}

float4 color_vert_get(const OffsetIndices<int> faces,
                      const Span<int> corner_verts,
                      const GroupedSpan<int> vert_to_face_map,
                      const GSpan color_attribute,
                      const bke::AttrDomain color_domain,
                      const int vert)
{
  float4 color;
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    color = color_vert_get<T>(
        faces, corner_verts, vert_to_face_map, color_attribute, color_domain, vert);
  });
  return color;
}

void color_vert_set(const OffsetIndices<int> faces,
                    const Span<int> corner_verts,
                    const GroupedSpan<int> vert_to_face_map,
                    const bke::AttrDomain color_domain,
                    const int vert,
                    const float4 &color,
                    const GMutableSpan color_attribute)
{
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    color_vert_set<T>(
        faces, corner_verts, vert_to_face_map, color_attribute, color_domain, vert, color);
  });
}

void swap_gathered_colors(const Span<int> indices,
                          GMutableSpan color_attribute,
                          MutableSpan<float4> r_colors)
{
  PRF_scope(ProfileCategory::Editor);
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    T *colors_typed = static_cast<T *>(color_attribute.data());
    for (const int i : indices.index_range()) {
      T temp = colors_typed[indices[i]];
      from_float(r_colors[i], colors_typed[indices[i]]);
      r_colors[i] = to_float(temp);
    }
  });
}

void gather_colors(const GSpan color_attribute,
                   const Span<int> indices,
                   MutableSpan<float4> r_colors)
{
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    const T *colors_typed = static_cast<const T *>(color_attribute.data());
    for (const int i : indices.index_range()) {
      r_colors[i] = to_float(colors_typed[indices[i]]);
    }
  });
}

void gather_colors_vert(const OffsetIndices<int> faces,
                        const Span<int> corner_verts,
                        const GroupedSpan<int> vert_to_face_map,
                        const GSpan color_attribute,
                        const bke::AttrDomain color_domain,
                        const Span<int> verts,
                        const MutableSpan<float4> r_colors)
{
  PRF_scope(ProfileCategory::Editor);
  if (color_domain == bke::AttrDomain::Point) {
    gather_colors(color_attribute, verts, r_colors);
  }
  else {
    to_static_color_type(color_attribute.type(), [&](auto dummy) {
      using T = decltype(dummy);
      for (const int i : verts.index_range()) {
        r_colors[i] = color_vert_get<T>(
            faces, corner_verts, vert_to_face_map, color_attribute, color_domain, verts[i]);
      }
    });
  }
}

bke::GAttributeReader active_color_attribute(const Mesh &mesh)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  const StringRef name = mesh.active_color_attribute;
  const bke::GAttributeReader colors = attributes.lookup(name);
  if (!colors) {
    return {};
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(colors.varray.type());
  if (!bke::mesh::is_color_attribute({colors.domain, data_type})) {
    return {};
  }
  return colors;
}

bke::GSpanAttributeWriter active_color_attribute_for_write(Mesh &mesh)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const StringRef name = mesh.active_color_attribute;
  bke::GSpanAttributeWriter colors = attributes.lookup_for_write_span(name);
  if (!colors) {
    return {};
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(colors.span.type());
  if (!bke::mesh::is_color_attribute({colors.domain, data_type})) {
    colors.finish();
    return {};
  }
  return colors;
}

void ensure_shared_color_attributes(Mesh &active_mesh, const Span<Mesh *> other_meshes)
{
  const bke::GAttributeReader reference = active_color_attribute(active_mesh);
  if (!reference) {
    return;
  }
  const StringRef ref_name = active_mesh.active_color_attribute;
  const bke::AttrDomain ref_domain = reference.domain;
  const bke::AttrType ref_type = bke::cpp_type_to_attribute_type(reference.varray.type());

  for (Mesh *mesh : other_meshes) {
    if (mesh == &active_mesh) {
      continue;
    }
    bool created = false;
    if (const std::optional<bke::AttributeMetaData> meta = mesh->attributes().lookup_meta_data(
            ref_name))
    {
      /* Name collision with a non-color attribute: leave this mesh untouched. Painting skips it
       * the same way it skips a mesh without a color attribute today. */
      if (!bke::mesh::is_color_attribute(*meta)) {
        continue;
      }
    }
    else {
      if (!mesh->attributes_for_write().add(
              ref_name, ref_domain, ref_type, bke::AttributeInitDefaultValue()))
      {
        continue;
      }
      created = true;
      BKE_mesh_tessface_clear(mesh);
    }

    const bool was_active = mesh->active_color_attribute &&
                            StringRef(mesh->active_color_attribute) == ref_name;
    if (!was_active) {
      BKE_id_attributes_active_color_set(&mesh->id, ref_name);
    }
    if (mesh->default_color_attribute == nullptr) {
      BKE_id_attributes_default_color_set(&mesh->id, ref_name);
    }
    if (created || !was_active) {
      DEG_id_tag_update(&mesh->id, ID_RECALC_GEOMETRY_ALL_MODES);
    }
  }
}

void ensure_shared_color_attributes(Object &active_object, const Span<Object *> objects)
{
  BKE_sculpt_color_layer_create_if_needed(&active_object);

  Mesh &active_mesh = *BKE_object_get_original_mesh(&active_object);
  Vector<Mesh *> other_meshes;
  for (Object *object : objects) {
    if (object == &active_object || object->type != OB_MESH) {
      continue;
    }
    other_meshes.append(BKE_object_get_original_mesh(object));
  }
  ensure_shared_color_attributes(active_mesh, other_meshes);
}

bke::GSpanAttributeWriter ensure_active_color_attribute_for_write(Mesh &mesh)
{
  if (!active_color_attribute(mesh)) {
    ED_mesh_color_ensure(&mesh, DATA_("Color"));
  }
  return active_color_attribute_for_write(mesh);
}

struct ColorPaintLocalData {
  Vector<float> factors;
  Vector<float> auto_mask;
  Vector<float3> positions;
  Vector<float> distances;
  Vector<float4> colors;
  Vector<float4> new_colors;
  Vector<float4> mix_colors;
  Vector<int> neighbor_offsets;
  Vector<int> neighbor_data;
};

static void do_color_smooth_task(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const Span<float3> vert_positions,
                                 const Span<float3> vert_normals,
                                 const OffsetIndices<int> faces,
                                 const Span<int> corner_verts,
                                 const GroupedSpan<int> vert_to_face_map,
                                 const MeshAttributeData &attribute_data,
                                 const Brush &brush,
                                 const bke::pbvh::MeshNode &node,
                                 ColorPaintLocalData &tls,
                                 bke::GSpanAttributeWriter &color_attribute)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, vert_positions, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, vert_positions, verts, factors);
  scale_factors(factors, cache.bstrength);

  tls.colors.resize(verts.size());
  MutableSpan<float4> colors = tls.colors;
  for (const int i : verts.index_range()) {
    colors[i] = color_vert_get(faces,
                               corner_verts,
                               vert_to_face_map,
                               color_attribute.span,
                               color_attribute.domain,
                               verts[i]);
  }

  const GroupedSpan<int> neighbors = calc_vert_neighbors(faces,
                                                         corner_verts,
                                                         vert_to_face_map,
                                                         attribute_data.hide_poly,
                                                         verts,
                                                         tls.neighbor_offsets,
                                                         tls.neighbor_data);

  tls.new_colors.resize(verts.size());
  MutableSpan<float4> new_colors = tls.new_colors;
  smooth::neighbor_color_average(faces,
                                 corner_verts,
                                 vert_to_face_map,
                                 color_attribute.span,
                                 color_attribute.domain,
                                 neighbors,
                                 new_colors);

  for (const int i : colors.index_range()) {
    blend_color_interpolate_float(new_colors[i], colors[i], new_colors[i], factors[i]);
  }

  for (const int i : verts.index_range()) {
    color_vert_set(faces,
                   corner_verts,
                   vert_to_face_map,
                   color_attribute.domain,
                   verts[i],
                   new_colors[i],
                   color_attribute.span);
  }
}

static void do_paint_brush_task(const Depsgraph &depsgraph,
                                Object &object,
                                const Span<float3> vert_positions,
                                const Span<float3> vert_normals,
                                const OffsetIndices<int> faces,
                                const Span<int> corner_verts,
                                const GroupedSpan<int> vert_to_face_map,
                                const MeshAttributeData &attribute_data,
                                const Paint &paint,
                                const Brush &brush,
                                const float4x4 &mat,
                                const float4 wet_mix_sampled_color,
                                bke::pbvh::MeshNode &node,
                                ColorPaintLocalData &tls,
                                const MutableSpan<float4> mix_colors,
                                bke::GSpanAttributeWriter &color_attribute)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const float bstrength = fabsf(ss.cache->bstrength);
  const float alpha = BKE_brush_alpha_get(&paint, &brush);

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, vert_positions, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  float radius;

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  if (brush.tip_roundness < 1.0f) {
    tls.positions.resize(verts.size());
    calc_local_positions(mat, verts, vert_positions, tls.positions);
    calc_brush_cube_distances<float3>(brush, tls.positions, distances);
    radius = 1.0f;
  }
  else {
    calc_brush_distances(
        ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
    radius = cache.radius;
  }
  filter_distances_with_radius(radius, distances, factors);
  apply_hardness_to_distances(radius, cache.hardness, distances);
  BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                               brush.curve_distance_falloff,
                               distances,
                               radius,
                               factors);

  MutableSpan<float> auto_mask;
  if (cache.automasking) {
    tls.auto_mask.resize(verts.size());
    auto_mask = tls.auto_mask;
    auto_mask.fill(1.0f);
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, auto_mask);
    scale_factors(factors, auto_mask);
  }

  calc_brush_texture_factors(ss, brush, vert_positions, verts, factors);
  scale_factors(factors, bstrength);

  const float density = ss.cache->paint_brush.density;
  if (density < 1.0f) {
    BLI_assert(ss.cache->paint_brush.density_seed);
    const float seed = ss.cache->paint_brush.density_seed.value_or(0.0f);
    for (const int i : verts.index_range()) {
      const float hash_noise = BLI_hash_int_01(seed * 1000 * verts[i]);
      if (hash_noise > density) {
        const float noise = density * hash_noise;
        factors[i] *= noise;
      }
    }
  }

  float3 brush_color_rgb = ss.cache->toggle_settings.invert ?
                               BKE_brush_secondary_color_get(&paint, &brush) :
                               BKE_brush_color_get(&paint, &brush);

  const std::optional<BrushColorJitterSettings> color_jitter_settings =
      BKE_brush_color_jitter_get_settings(&paint, &brush);
  if (color_jitter_settings) {
    brush_color_rgb = BKE_paint_randomize_color(*color_jitter_settings,
                                                *ss.cache->initial_hsv_jitter,
                                                ss.cache->stroke_distance,
                                                ss.cache->pressure,
                                                brush_color_rgb);
  }

  float4 brush_color(brush_color_rgb, 1.0f);

  const Span<float4> orig_colors = orig_color_data_get_mesh(object, node);

  MutableSpan<float4> color_buffer = gather_data_mesh(mix_colors.as_span(), verts, tls.mix_colors);

  if (brush.flag & BRUSH_USE_GRADIENT) {
    switch (brush.gradient_stroke_mode) {
      case BRUSH_GRADIENT_PRESSURE:
        BKE_colorband_evaluate(brush.gradient, ss.cache->pressure, brush_color);
        break;
      case BRUSH_GRADIENT_SPACING_REPEAT: {
        float coord = fmod(ss.cache->stroke_distance / brush.gradient_spacing, 1.0);
        BKE_colorband_evaluate(brush.gradient, coord, brush_color);
        break;
      }
      case BRUSH_GRADIENT_SPACING_CLAMP: {
        BKE_colorband_evaluate(
            brush.gradient, ss.cache->stroke_distance / brush.gradient_spacing, brush_color);
        break;
      }
    }
  }

  tls.new_colors.resize(verts.size());
  MutableSpan<float4> new_colors = tls.new_colors;
  for (const int i : verts.index_range()) {
    new_colors[i] = color_vert_get(faces,
                                   corner_verts,
                                   vert_to_face_map,
                                   color_attribute.span,
                                   color_attribute.domain,
                                   verts[i]);
  }

  for (const int i : verts.index_range()) {
    /* Brush paint color, brush test falloff and flow. */
    float4 paint_color = brush_color * factors[i] * ss.cache->paint_brush.flow;
    float4 wet_mix_color = wet_mix_sampled_color * factors[i] * ss.cache->paint_brush.flow;

    /* Interpolate with the wet_mix color for wet paint mixing. */
    blend_color_interpolate_float(
        paint_color, paint_color, wet_mix_color, ss.cache->paint_brush.wet_mix);
    blend_color_mix_float(color_buffer[i], color_buffer[i], paint_color);

    /* Final mix over the original color using brush alpha. We apply auto-making again
     * at this point to avoid washing out non-binary masking modes like cavity masking. */
    float automasking = auto_mask.is_empty() ? 1.0f : auto_mask[i];
    const float4 buffer_color = float4(color_buffer[i]) * alpha * automasking;

    IMB_blend_color_float(new_colors[i], orig_colors[i], buffer_color, IMB_BlendMode(brush.blend));
    new_colors[i] = math::clamp(new_colors[i], 0.0f, 1.0f);
  }

  scatter_data_mesh(color_buffer.as_span(), verts, mix_colors);

  for (const int i : verts.index_range()) {
    color_vert_set(faces,
                   corner_verts,
                   vert_to_face_map,
                   color_attribute.domain,
                   verts[i],
                   new_colors[i],
                   color_attribute.span);
  }
}

struct SampleWetPaintData {
  int tot_samples;
  float4 color;
};

static void do_sample_wet_paint_task(const Depsgraph &depsgraph,
                                     const Object &object,
                                     const Span<float3> vert_positions,
                                     const Span<float3> vert_normals,
                                     const OffsetIndices<int> faces,
                                     const Span<int> corner_verts,
                                     const GroupedSpan<int> vert_to_face_map,
                                     const MeshAttributeData &attribute_data,
                                     const GSpan color_attribute,
                                     const bke::AttrDomain color_domain,
                                     const Brush &brush,
                                     const bke::pbvh::MeshNode &node,
                                     ColorPaintLocalData &tls,
                                     SampleWetPaintData &swptd)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  const float radius = cache.radius * brush.wet_paint_radius_factor;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }
  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(radius, distances, factors);

  for (const int i : verts.index_range()) {
    if (factors[i] > 0.0f) {
      swptd.color +=
          color_vert_get(
              faces, corner_verts, vert_to_face_map, color_attribute, color_domain, verts[i]) *
          factors[i];
      swptd.tot_samples++;
    }
  }
}

/* Multi-object wet-paint sampling: accumulates weighted color samples from every object in
 * `objects` around a shared brush center expressed in `reference_ob`'s local space, mirroring
 * #calc_area_sample_multi_object_mesh. All inputs are reference-space, so every requesting object
 * computes the identical result and the per-object wet_mix_prev_color states stay in sync without
 * any shared mutable state. Mesh PBVH only; non-mesh objects and meshes without a valid color
 * attribute are skipped. */
static SampleWetPaintData sample_wet_paint_multi_object_mesh(const Depsgraph &depsgraph,
                                                             const Brush &brush,
                                                             const Object &reference_ob,
                                                             const Span<Object *> objects,
                                                             const float3 &ref_location,
                                                             const float3 &ref_view_normal,
                                                             const float ref_radius)
{
  const float radius_sq = ref_radius * ref_radius;
  const bool use_tube = eBrushFalloffShape(brush.falloff_shape) == PAINT_FALLOFF_SHAPE_TUBE;
  float4 tube_plane;
  if (use_tube) {
    plane_from_point_normal_v3(tube_plane, ref_location, ref_view_normal);
  }

  SampleWetPaintData swptd{};

  for (Object *object_ptr : objects) {
    Object &ob = *object_ptr;
    bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
    if (!pbvh || pbvh->type() != bke::pbvh::Type::Mesh) {
      continue;
    }
    SculptSession &ss = *ob.runtime->sculpt_session;
    if (!ss.cache) {
      continue;
    }
    Mesh &mesh = *id_cast<Mesh *>(ob.data);
    const bke::GAttributeReader color_attribute = active_color_attribute(mesh);
    if (!color_attribute) {
      continue;
    }
    const GVArraySpan colors = *color_attribute;

    const float4x4 ref_from_obj = reference_ob.world_to_object() * ob.object_to_world();
    const float4x4 obj_from_ref = ob.world_to_object() * reference_ob.object_to_world();
    const float3 obj_center = math::transform_point(obj_from_ref, ref_location);
    const float3 obj_view_normal = math::normalize(
        math::transform_direction(obj_from_ref, ref_view_normal));
    /* Conservative gather radius in this object's local units; the precise filtering happens
     * per-vertex in reference space below. */
    const float obj_scale = max_ff(
        max_ff(math::length(obj_from_ref.x_axis()), math::length(obj_from_ref.y_axis())),
        math::length(obj_from_ref.z_axis()));
    const float gather_radius = ref_radius * obj_scale * 1.25f;
    const float gather_radius_sq = gather_radius * gather_radius;

    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::search_nodes(
        *pbvh, memory, [&](const bke::pbvh::Node &node) {
          return node_in_sphere(node, obj_center, gather_radius_sq, false);
        });
    if (node_mask.is_empty()) {
      continue;
    }

    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();
    const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
    const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
    const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
    const MeshAttributeData attribute_data(mesh);
    const Span<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();

    threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
    const SampleWetPaintData object_sample = threading::parallel_reduce(
        node_mask.index_range(),
        1,
        SampleWetPaintData{},
        [&](const IndexRange range, SampleWetPaintData swptd_local) {
          ColorPaintLocalData &tls = all_tls.local();
          node_mask.slice(range).foreach_index([&](const int i) {
            const bke::pbvh::MeshNode &node = nodes[i];
            const Span<int> verts = node.verts();

            tls.factors.resize(verts.size());
            const MutableSpan<float> factors = tls.factors;
            fill_factor_from_hide_and_mask(
                attribute_data.hide_vert, attribute_data.mask, verts, factors);
            if (brush.flag & BRUSH_FRONTFACE) {
              calc_front_face(obj_view_normal, vert_normals, verts, factors);
            }
            auto_mask::calc_vert_factors(
                depsgraph, ob, ss.cache->automasking.get(), node, verts, factors);

            for (const int k : verts.index_range()) {
              if (factors[k] <= 0.0f) {
                continue;
              }
              const float3 pos_ref = math::transform_point(ref_from_obj, vert_positions[verts[k]]);
              float distance_sq;
              if (use_tube) {
                float3 projected;
                closest_to_plane_normalized_v3(projected, tube_plane, pos_ref);
                distance_sq = math::distance_squared(projected, ref_location);
              }
              else {
                distance_sq = math::distance_squared(ref_location, pos_ref);
              }
              if (distance_sq > radius_sq) {
                continue;
              }
              swptd_local.color += color_vert_get(faces,
                                                  corner_verts,
                                                  vert_to_face_map,
                                                  colors,
                                                  color_attribute.domain,
                                                  verts[k]) *
                                   factors[k];
              swptd_local.tot_samples++;
            }
          });
          return swptd_local;
        },
        [](const SampleWetPaintData &a, const SampleWetPaintData &b) {
          SampleWetPaintData joined{};
          joined.color = a.color + b.color;
          joined.tot_samples = a.tot_samples + b.tot_samples;
          return joined;
        });

    swptd.color += object_sample.color;
    swptd.tot_samples += object_sample.tot_samples;
  }

  return swptd;
}

void do_paint_brush(const Depsgraph &depsgraph,
                    PaintModeSettings &paint_mode_settings,
                    const Sculpt &sd,
                    Object &ob,
                    const IndexMask &node_mask,
                    const IndexMask &texnode_mask)
{
  if (SCULPT_use_image_paint_brush(paint_mode_settings, ob)) {
    SCULPT_do_paint_brush_image(depsgraph, sd, ob, texnode_mask);
    return;
  }
  PRF_scope(ProfileCategory::Editor);

  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  if (!ss.cache->paint_brush.density_seed) {
    ss.cache->paint_brush.density_seed = BLI_hash_int_01(ss.cache->location_symm[0] * 1000);
  }

  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    return;
  }

  float4x4 mat;

  /* If the brush is round the tip does not need to be aligned to the surface, so this saves a
   * whole iteration over the affected nodes. */
  if (brush.tip_roundness < 1.0f) {
    cube_tip_init(sd, ob, brush, mat.ptr());

    if (is_zero_m4(mat.ptr())) {
      return;
    }
  }

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const MeshAttributeData attribute_data(mesh);
  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return;
  }

  /* Regular Paint mode. */

  /* Wet paint color sampling. */
  float4 wet_color(0);
  if (ss.cache->paint_brush.wet_mix > 0.0f) {
    SampleWetPaintData swptd{};
    const Span<Object *> sample_objects = ss.cache->multi_object_sample_objects;
    const Object *reference_ob = ss.cache->multi_object_sample_reference;
    const SculptSession *ref_ss = reference_ob ? reference_ob->runtime->sculpt_session : nullptr;
    if (!sample_objects.is_empty() && reference_ob && ref_ss && ref_ss->cache) {
      /* Multi-object stroke: pool the wet sample across every object so the "wet paint" color
       * matches a joined mesh at object boundaries. Reference-space inputs keep the result
       * identical for every requesting object. */
      const float4x4 ref_from_cur = reference_ob->world_to_object() * ob.object_to_world();
      const float3 ref_location = math::transform_point(ref_from_cur, ss.cache->location_symm);
      const float3 ref_view_normal = math::normalize(
          math::transform_direction(ref_from_cur, ss.cache->view_normal_symm));
      const float ref_radius = ref_ss->cache->radius * brush.wet_paint_radius_factor;
      swptd = sample_wet_paint_multi_object_mesh(depsgraph,
                                                 brush,
                                                 *reference_ob,
                                                 sample_objects,
                                                 ref_location,
                                                 ref_view_normal,
                                                 ref_radius);
    }
    else {
      threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
      swptd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          SampleWetPaintData{},
          [&](const IndexRange range, SampleWetPaintData swptd) {
            ColorPaintLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              do_sample_wet_paint_task(depsgraph,
                                       ob,
                                       vert_positions,
                                       vert_normals,
                                       faces,
                                       corner_verts,
                                       vert_to_face_map,
                                       attribute_data,
                                       color_attribute.span,
                                       color_attribute.domain,
                                       brush,
                                       nodes[i],
                                       tls,
                                       swptd);
            });
            return swptd;
          },
          [](const SampleWetPaintData &a, const SampleWetPaintData &b) {
            SampleWetPaintData joined{};
            joined.color = a.color + b.color;
            joined.tot_samples = a.tot_samples + b.tot_samples;
            return joined;
          });
    }

    if (swptd.tot_samples > 0 && is_finite_v4(swptd.color)) {
      wet_color = math::clamp(swptd.color / float(swptd.tot_samples), 0.0f, 1.0f);

      if (ss.cache->first_time) {
        ss.cache->paint_brush.wet_mix_prev_color = wet_color;
      }
      blend_color_interpolate_float(wet_color,
                                    wet_color,
                                    ss.cache->paint_brush.wet_mix_prev_color,
                                    ss.cache->paint_brush.wet_persistence);
      ss.cache->paint_brush.wet_mix_prev_color = math::clamp(wet_color, 0.0f, 1.0f);
    }
  }

  if (ss.cache->paint_brush.mix_colors.is_empty()) {
    ss.cache->paint_brush.mix_colors = Array<float4>(mesh.verts_num, float4(0));
  }

  threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        ColorPaintLocalData &tls = all_tls.local();
        do_paint_brush_task(depsgraph,
                            ob,
                            vert_positions,
                            vert_normals,
                            faces,
                            corner_verts,
                            vert_to_face_map,
                            attribute_data,
                            sd.paint,
                            brush,
                            mat,
                            wet_color,
                            nodes[i],
                            tls,
                            ss.cache->paint_brush.mix_colors,
                            color_attribute);
      },
      exec_mode::grain_size(1));
  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

static void do_smear_brush_task(const Depsgraph &depsgraph,
                                Object &object,
                                const Span<float3> vert_positions,
                                const Span<float3> vert_normals,
                                const OffsetIndices<int> faces,
                                const Span<int> corner_verts,
                                const GroupedSpan<int> vert_to_face_map,
                                const MeshAttributeData &attribute_data,
                                const Brush &brush,
                                bke::pbvh::MeshNode &node,
                                ColorPaintLocalData &tls,
                                bke::GSpanAttributeWriter &color_attribute)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  const float strength = ss.cache->bstrength;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, vert_positions, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, vert_positions, verts, factors);
  scale_factors(factors, strength);

  float3 brush_delta;
  if (brush.stroke_method == BRUSH_STROKE_ANCHORED) {
    brush_delta = ss.cache->grab_delta_symm;
  }
  else {
    brush_delta = ss.cache->location_symm - ss.cache->last_location_symm;
  }

  Vector<int> neighbors;
  Vector<int> neighbor_neighbors;

  for (const int i : verts.index_range()) {
    if (factors[i] == 0.0f) {
      continue;
    }
    const int vert = verts[i];
    const float3 &no = vert_normals[vert];

    float3 current_disp;
    switch (brush.smear_deform_type) {
      case BRUSH_SMEAR_DEFORM_DRAG:
        current_disp = brush_delta;
        break;
      case BRUSH_SMEAR_DEFORM_PINCH:
        current_disp = ss.cache->location_symm - vert_positions[vert];
        break;
      case BRUSH_SMEAR_DEFORM_EXPAND:
        current_disp = vert_positions[vert] - ss.cache->location_symm;
        break;
    }

    /* Project into vertex plane. */
    current_disp += no * -math::dot(current_disp, no);

    const float3 current_disp_norm = math::normalize(current_disp);

    current_disp = current_disp_norm * strength;

    float4 accum(0);
    float totw = 0.0f;

    /*
     * NOTE: we have to do a nested iteration here to avoid
     * blocky artifacts on quad topologies.  The runtime cost
     * is not as bad as it seems due to neighbor iteration
     * in the sculpt code being cache bound; once the data is in
     * the cache iterating over it a few more times is not terribly
     * costly.
     */

    for (const int neighbor : vert_neighbors_get_mesh(
             faces, corner_verts, vert_to_face_map, attribute_data.hide_poly, vert, neighbors))
    {
      const float3 &nco = vert_positions[neighbor];
      for (const int neighbor_neighbor : vert_neighbors_get_mesh(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 attribute_data.hide_poly,
                                                                 neighbor,
                                                                 neighbor_neighbors))
      {
        if (neighbor_neighbor == vert) {
          continue;
        }

        float3 vert_disp = vert_positions[neighbor_neighbor] - vert_positions[vert];

        /* Weight by how close we are to our target distance from vd.co. */
        float w = (1.0f + fabsf(math::length(vert_disp) / strength - 1.0f));

        /* TODO: use cotangents (or at least face areas) here. */
        float len = math::distance(vert_positions[neighbor_neighbor], nco);
        if (len > 0.0f) {
          len = strength / len;
        }
        else { /* Coincident point. */
          len = 1.0f;
        }

        /* Multiply weight with edge lengths (in the future this will be
         * cotangent weights or face areas). */
        w *= len;

        /* Build directional weight. */

        /* Project into vertex plane. */
        vert_disp += no * -math::dot(no, vert_disp);
        const float3 vert_disp_norm = math::normalize(vert_disp);

        if (math::dot(current_disp_norm, vert_disp_norm) >= 0.0f) {
          continue;
        }

        const float4 &neighbor_color = ss.cache->paint_brush.prev_colors[neighbor_neighbor];
        float color_interp = -math::dot(current_disp_norm, vert_disp_norm);

        /* Square directional weight to get a somewhat sharper result. */
        w *= color_interp * color_interp;

        accum += neighbor_color * w;
        totw += w;
      }
    }

    if (totw != 0.0f) {
      accum /= totw;
    }

    float4 col = color_vert_get(
        faces, corner_verts, vert_to_face_map, color_attribute.span, color_attribute.domain, vert);
    blend_color_interpolate_float(col, ss.cache->paint_brush.prev_colors[vert], accum, factors[i]);
    color_vert_set(faces,
                   corner_verts,
                   vert_to_face_map,
                   color_attribute.domain,
                   vert,
                   col,
                   color_attribute.span);
  }
}

void do_smear_brush(const Depsgraph &depsgraph,
                    const Sculpt &sd,
                    Object &ob,
                    const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (ss.cache->bstrength == 0.0f) {
    return;
  }

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const MeshAttributeData attribute_data(mesh);

  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return;
  }

  if (ss.cache->paint_brush.prev_colors.is_empty()) {
    ss.cache->paint_brush.prev_colors = Array<float4>(mesh.verts_num);
    threading::parallel_for(IndexRange(mesh.verts_num), 1024, [&](const IndexRange range) {
      for (const int vert : range) {
        ss.cache->paint_brush.prev_colors[vert] = color_vert_get(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 color_attribute.span,
                                                                 color_attribute.domain,
                                                                 vert);
      }
    });
  }

  /* Smear mode. */
  node_mask.foreach_index(
      [&](const int i) {
        for (const int vert : nodes[i].verts()) {
          ss.cache->paint_brush.prev_colors[vert] = color_vert_get(faces,
                                                                   corner_verts,
                                                                   vert_to_face_map,
                                                                   color_attribute.span,
                                                                   color_attribute.domain,
                                                                   vert);
        }
      },
      exec_mode::grain_size(1));
  threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        ColorPaintLocalData &tls = all_tls.local();
        do_smear_brush_task(depsgraph,
                            ob,
                            vert_positions,
                            vert_normals,
                            faces,
                            corner_verts,
                            vert_to_face_map,
                            attribute_data,
                            brush,
                            nodes[i],
                            tls,
                            color_attribute);
      },
      exec_mode::grain_size(1));

  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

void do_blur_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &ob,
                   const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  if (!ss.cache->paint_brush.density_seed) {
    ss.cache->paint_brush.density_seed = BLI_hash_int_01(ss.cache->location_symm[0] * 1000);
  }

  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    return;
  }

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const MeshAttributeData attribute_data(mesh);
  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return;
  }

  threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        ColorPaintLocalData &tls = all_tls.local();
        do_color_smooth_task(depsgraph,
                             ob,
                             vert_positions,
                             vert_normals,
                             faces,
                             corner_verts,
                             vert_to_face_map,
                             attribute_data,
                             brush,
                             nodes[i],
                             tls,
                             color_attribute);
      },
      exec_mode::grain_size(1));
  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

}  // namespace blender::ed::sculpt_paint::color
