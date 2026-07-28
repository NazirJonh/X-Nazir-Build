/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/paint_mask.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {

inline namespace layer_cc {

struct LocalData {
  Vector<float3> persistent_positions;
  Vector<float3> persistent_normals;
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float> masks;
  Vector<float> displacement_factors;
  Vector<float3> translations;
  /* Positions with the sculpt-layer base view removed (see #layers::stroke_base_view). */
  Vector<float3> base_view_storage;
};

BLI_NOINLINE static void offset_displacement_factors(const MutableSpan<float> displacement_factors,
                                                     const Span<float> factors,
                                                     const float strength)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : displacement_factors.index_range()) {
    displacement_factors[i] += factors[i] * strength * (1.05f - std::abs(displacement_factors[i]));
  }
}

/**
 * When using persistent base, the layer brush (holding Control) invert mode resets the
 * height of the layer to 0. This makes possible to clean edges of previously added layers
 * on top of the base.
 *
 * The main direction of the layers is inverted using the regular brush strength with the
 * brush direction property.
 */
BLI_NOINLINE static void reset_displacement_factors(const MutableSpan<float> displacement_factors,
                                                    const Span<float> factors,
                                                    const float strength)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : displacement_factors.index_range()) {
    displacement_factors[i] += std::abs(factors[i] * strength * displacement_factors[i]) *
                               (displacement_factors[i] > 0.0f ? -1.0f : 1.0f);
  }
}

BLI_NOINLINE static void clamp_displacement_factors(const MutableSpan<float> displacement_factors,
                                                    const Span<float> masks)
{
  PRF_scope(ProfileCategory::Editor);
  if (masks.is_empty()) {
    for (const int i : displacement_factors.index_range()) {
      displacement_factors[i] = std::clamp(displacement_factors[i], -1.0f, 1.0f);
    }
  }
  else {
    for (const int i : displacement_factors.index_range()) {
      const float clamp_mask = 1.0f - masks[i];
      displacement_factors[i] = std::clamp(displacement_factors[i], -clamp_mask, clamp_mask);
    }
  }
}

BLI_NOINLINE static void calc_translations(const Span<float3> orig_positions,
                                           const Span<float3> orig_normals,
                                           const Span<float3> positions,
                                           const Span<float> displacement_factors,
                                           const Span<float> factors,
                                           const float height,
                                           const MutableSpan<float3> r_translations)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : positions.index_range()) {
    const float3 offset = orig_normals[i] * height * displacement_factors[i];
    const float3 translation = orig_positions[i] + offset - positions[i];
    r_translations[i] = translation * factors[i];
  }
}

BLI_NOINLINE static void calc_translations(const Span<float3> base_positions,
                                           const Span<float3> base_normals,
                                           const Span<int> verts,
                                           const Span<float3> positions,
                                           const Span<float> displacement_factors,
                                           const Span<float> factors,
                                           const float height,
                                           const MutableSpan<float3> r_translations)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : positions.index_range()) {
    const float3 offset = base_normals[verts[i]] * height * displacement_factors[i];
    const float3 translation = base_positions[verts[i]] + offset - positions[i];
    r_translations[i] = translation * factors[i];
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const MeshAttributeData &attribute_data,
                       const Span<float3> vert_normals,
                       const bool use_base,
                       const Span<float3> base_positions,
                       const Span<float3> base_normals,
                       Object &object,
                       bke::pbvh::MeshNode &node,
                       LocalData &tls,
                       MutableSpan<float> layer_displacement_factor,
                       const PositionDeformData &position_data)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();
  const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);
  const MutableSpan positions = gather_data_mesh(position_data.eval, verts, tls.positions);

  /* Base view: region clipping and falloff distances are measured on the un-layered base so they
   * are not modulated by the layer pattern. The height offset itself needs no adjustment: it is
   * built from the original positions, which carry the same offset as the live ones, so it cancels
   * out of the translation. The texture keeps sampling the composed surface (see
   * #sculpt_apply_texture). */
  const Span<float3> calc_positions = layers::base_view_adjust_compact_mesh(
      object, verts, orig_data.positions, tls.base_view_storage);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, calc_positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(ss, calc_positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  if (attribute_data.mask.is_empty()) {
    tls.masks.clear();
  }
  else {
    tls.masks.resize(verts.size());
    gather_data_mesh(attribute_data.mask, verts, tls.masks.as_mutable_span());
  }
  const MutableSpan<float> masks = tls.masks;

  tls.displacement_factors.resize(verts.size());
  const MutableSpan<float> displacement_factors = tls.displacement_factors;
  gather_data_mesh(layer_displacement_factor.as_span(), verts, displacement_factors);

  if (use_base) {
    /* Only the manually set persistent base uses the invert toggle to erase layers back to the
     * base. The uniform depth base keeps the regular layer behavior where inverting carves in the
     * opposite direction. */
    if ((brush.flag & BRUSH_PERSISTENT) && cache.toggle_settings.invert) {
      reset_displacement_factors(displacement_factors, tls.factors, cache.bstrength);
    }
    else {
      offset_displacement_factors(displacement_factors, tls.factors, cache.bstrength);
    }
    clamp_displacement_factors(displacement_factors, masks);

    scatter_data_mesh(displacement_factors.as_span(), verts, layer_displacement_factor);

    tls.translations.resize(verts.size());
    const MutableSpan<float3> translations = tls.translations;
    calc_translations(base_positions,
                      base_normals,
                      verts,
                      positions,
                      displacement_factors,
                      tls.factors,
                      brush.height,
                      translations);

    clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
    position_data.deform(translations, verts);
  }
  else {
    offset_displacement_factors(displacement_factors, tls.factors, cache.bstrength);
    clamp_displacement_factors(displacement_factors, masks);

    scatter_data_mesh(displacement_factors.as_span(), verts, layer_displacement_factor);

    tls.translations.resize(verts.size());
    const MutableSpan<float3> translations = tls.translations;
    calc_translations(orig_data.positions,
                      orig_data.normals,
                      positions,
                      displacement_factors,
                      tls.factors,
                      brush.height,
                      translations);

    clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
    position_data.deform(translations, verts);
  }
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       Object &object,
                       const bool use_base,
                       const Span<float3> base_positions,
                       const Span<float3> base_normals,
                       bke::pbvh::GridsNode &node,
                       LocalData &tls,
                       MutableSpan<float> layer_displacement_factor)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const OrigPositionData orig_data = orig_position_data_get_grids(object, node);
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  /* Base view: see the mesh variant in #calc_faces. */
  const Span<float3> calc_positions = layers::base_view_adjust_compact_grids(
      object, subdiv_ccg, grids, orig_data.positions, tls.base_view_storage);

  tls.factors.resize(positions.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, calc_positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, subdiv_ccg, grids, factors);
  }

  tls.distances.resize(positions.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(ss, calc_positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  if (subdiv_ccg.masks.is_empty()) {
    tls.masks.clear();
  }
  else {
    tls.masks.resize(positions.size());
    gather_data_grids(subdiv_ccg, subdiv_ccg.masks.as_span(), grids, tls.masks.as_mutable_span());
  }
  const MutableSpan<float> masks = tls.masks;

  const MutableSpan<float> displacement_factors = gather_data_grids(
      subdiv_ccg, layer_displacement_factor.as_span(), grids, tls.displacement_factors);

  if (use_base) {
    /* See #calc_faces for why only the persistent base reacts to the invert toggle. */
    if ((brush.flag & BRUSH_PERSISTENT) && cache.toggle_settings.invert) {
      reset_displacement_factors(displacement_factors, tls.factors, cache.bstrength);
    }
    else {
      offset_displacement_factors(displacement_factors, tls.factors, cache.bstrength);
    }
    clamp_displacement_factors(displacement_factors, masks);

    scatter_data_grids(
        subdiv_ccg, displacement_factors.as_span(), grids, layer_displacement_factor);

    tls.translations.resize(positions.size());
    const MutableSpan<float3> translations = tls.translations;
    calc_translations(
        gather_data_grids(subdiv_ccg, base_positions, grids, tls.persistent_positions),
        gather_data_grids(subdiv_ccg, base_normals, grids, tls.persistent_normals),
        positions,
        displacement_factors,
        tls.factors,
        brush.height,
        translations);

    clip_and_lock_translations(sd, ss, positions, translations);
    apply_translations(translations, grids, subdiv_ccg);
  }
  else {
    offset_displacement_factors(displacement_factors, tls.factors, cache.bstrength);
    clamp_displacement_factors(displacement_factors, masks);

    scatter_data_grids(
        subdiv_ccg, displacement_factors.as_span(), grids, layer_displacement_factor);

    tls.translations.resize(positions.size());
    const MutableSpan<float3> translations = tls.translations;
    calc_translations(orig_data.positions,
                      orig_data.normals,
                      positions,
                      displacement_factors,
                      tls.factors,
                      brush.height,
                      translations);

    clip_and_lock_translations(sd, ss, positions, translations);
    apply_translations(translations, grids, subdiv_ccg);
  }
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       Object &object,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls,
                       MutableSpan<float> layer_displacement_factor)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  Array<float3> orig_positions(verts.size());
  Array<float3> orig_normals(verts.size());
  orig_position_data_gather_bmesh(*ss.bm_log, verts, orig_positions, orig_normals);

  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(ss, orig_positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  const MutableSpan<float> displacement_factors = gather_data_bmesh(
      layer_displacement_factor.as_span(), verts, tls.displacement_factors);

  offset_displacement_factors(displacement_factors, tls.factors, cache.bstrength);

  tls.masks.resize(verts.size());
  const MutableSpan<float> masks = tls.masks;
  mask::gather_mask_bmesh(*ss.bm, verts, masks);
  clamp_displacement_factors(displacement_factors, masks);

  scatter_data_bmesh(displacement_factors.as_span(), verts, layer_displacement_factor);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_translations(orig_positions,
                    orig_normals,
                    positions,
                    displacement_factors,
                    tls.factors,
                    brush.height,
                    translations);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace layer_cc

void layer_uniform_base_update(const Depsgraph &depsgraph, const Brush &brush, Object &object)
{
  if (brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_LAYER) {
    return;
  }

  SculptSession &ss = *object.runtime->sculpt_session;

  /* The persistent base is a reference of its own and takes precedence, so the option is inactive
   * while it is enabled. Releasing the data in every case where it is unused also gives the user a
   * way to capture a new reference: sculpt with the option disabled, then enable it again. */
  if ((brush.flag2 & BRUSH_LAYER_UNIFORM_DEPTH) == 0 || (brush.flag & BRUSH_PERSISTENT) != 0) {
    ss.layer_uniform_base = {};
    return;
  }

  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
      const int positions_num = int(positions.size());
      if (ss.layer_uniform_base.elements_num == positions_num) {
        break;
      }
      ss.layer_uniform_base.positions = positions;
      ss.layer_uniform_base.normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      ss.layer_uniform_base.displacement = Array<float>(positions_num, 0.0f);
      ss.layer_uniform_base.elements_num = positions_num;
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const int positions_num = int(subdiv_ccg.positions.size());
      if (ss.layer_uniform_base.elements_num == positions_num) {
        break;
      }
      ss.layer_uniform_base.positions = subdiv_ccg.positions;
      ss.layer_uniform_base.normals = subdiv_ccg.normals;
      ss.layer_uniform_base.displacement = Array<float>(positions_num, 0.0f);
      ss.layer_uniform_base.elements_num = positions_num;
      break;
    }
    case bke::pbvh::Type::BMesh: {
      /* Dynamic topology changes the vertex set between strokes, so a stored reference surface
       * cannot be matched back to the geometry. */
      ss.layer_uniform_base = {};
      break;
    }
  }
}

void do_layer_brush(const Depsgraph &depsgraph,
                    const Sculpt &sd,
                    Object &object,
                    const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  /* The reference surface for this option is captured once per stroke in
   * #layer_uniform_base_update. The persistent base takes precedence: both are a fixed reference,
   * but only the persistent one is stored with the mesh and reacts to the invert toggle. */
  const bool use_uniform_depth = (brush.flag2 & BRUSH_LAYER_UNIFORM_DEPTH) != 0 &&
                                 (brush.flag & BRUSH_PERSISTENT) == 0;

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      const PositionDeformData position_data(depsgraph, object);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);

      const MeshAttributeData attribute_data(mesh);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      const VArraySpan persistent_position = *attributes.lookup<float3>(".sculpt_persistent_co",
                                                                        bke::AttrDomain::Point);
      const VArraySpan persistent_normal = *attributes.lookup<float3>(".sculpt_persistent_no",
                                                                      bke::AttrDomain::Point);

      bke::SpanAttributeWriter<float> persistent_disp_attr;
      bool use_base = false;
      Span<float3> base_positions;
      Span<float3> base_normals;
      MutableSpan<float> displacement;
      if (brush.flag & BRUSH_PERSISTENT) {
        if (!persistent_position.is_empty() && !persistent_normal.is_empty()) {
          persistent_disp_attr = attributes.lookup_or_add_for_write_span<float>(
              ".sculpt_persistent_disp", bke::AttrDomain::Point);
          if (persistent_disp_attr) {
            use_base = true;
            base_positions = persistent_position;
            base_normals = persistent_normal;
            displacement = persistent_disp_attr.span;
          }
        }
      }
      else if (use_uniform_depth) {
        if (const std::optional<LayerUniformBaseData> base = ss.layer_uniform_base_data(
                int(position_data.eval.size())))
        {
          use_base = true;
          base_positions = base->positions;
          base_normals = base->normals;
          displacement = base->displacements;
        }
      }

      if (displacement.is_empty()) {
        if (ss.cache->layer_displacement_factor.is_empty()) {
          ss.cache->layer_displacement_factor = Array<float>(vertex_count_get(object), 0.0f);
        }
        displacement = ss.cache->layer_displacement_factor;
      }

      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_faces(depsgraph,
                       sd,
                       brush,
                       attribute_data,
                       vert_normals,
                       use_base,
                       base_positions,
                       base_normals,
                       object,
                       nodes[i],
                       tls,
                       displacement,
                       position_data);
            bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
          },
          exec_mode::grain_size(1));
      persistent_disp_attr.finish();
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;

      const std::optional<PersistentMultiresData> persistent_multires_data =
          ss.persistent_multires_data();

      bool use_base = false;
      Span<float3> base_positions;
      Span<float3> base_normals;
      MutableSpan<float> displacement;
      if (brush.flag & BRUSH_PERSISTENT) {
        if (persistent_multires_data) {
          use_base = true;
          base_positions = persistent_multires_data->positions;
          base_normals = persistent_multires_data->normals;
          displacement = persistent_multires_data->displacements;
        }
      }
      else if (use_uniform_depth) {
        if (const std::optional<LayerUniformBaseData> base = ss.layer_uniform_base_data(
                int(positions.size())))
        {
          use_base = true;
          base_positions = base->positions;
          base_normals = base->normals;
          displacement = base->displacements;
        }
      }

      if (displacement.is_empty()) {
        if (ss.cache->layer_displacement_factor.is_empty()) {
          ss.cache->layer_displacement_factor = Array<float>(positions.size(), 0.0f);
        }
        displacement = ss.cache->layer_displacement_factor;
      }

      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_grids(depsgraph,
                       sd,
                       brush,
                       object,
                       use_base,
                       base_positions,
                       base_normals,
                       nodes[i],
                       tls,
                       displacement);
            bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      if (ss.cache->layer_displacement_factor.is_empty()) {
        ss.cache->layer_displacement_factor = Array<float>(vertex_count_get(object), 0.0f);
      }
      const MutableSpan<float> displacement = ss.cache->layer_displacement_factor;
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_bmesh(depsgraph, sd, brush, object, nodes[i], tls, displacement);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

}  // namespace blender::ed::sculpt_paint::brushes
