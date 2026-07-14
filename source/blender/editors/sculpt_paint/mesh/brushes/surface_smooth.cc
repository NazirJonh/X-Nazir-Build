/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"
#include "editors/sculpt_paint/mesh/sculpt_smooth.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {

inline namespace surface_smooth_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<int> neighbor_offsets;
  Vector<int> neighbor_data;
  Vector<float3> laplacian_disp;
  Vector<float3> average_positions;
  Vector<float3> translations;
  /* Positions with the sculpt-layer base view removed, and its neighbor average (see
   * #layers::stroke_base_view). */
  Vector<float3> base_view_storage;
  Vector<float3> base_view_avg;
};

BLI_NOINLINE static void clamp_factors(const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (float &factor : factors) {
    factor = std::clamp(factor, 0.0f, 1.0f);
  }
}

BLI_NOINLINE static void do_surface_smooth_brush_mesh(const Depsgraph &depsgraph,
                                                      const Sculpt &sd,
                                                      const Brush &brush,
                                                      const IndexMask &node_mask,
                                                      Object &object,
                                                      const MutableSpan<float3> all_laplacian_disp)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const StrokeCache &cache = *ss.cache;
  const float alpha = brush.surface_smooth_shape_preservation;
  const float beta = brush.surface_smooth_current_vertex;
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const MeshAttributeData attribute_data(mesh);

  const PositionDeformData position_data(depsgraph, object);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);

  Array<int> node_offset_data;
  const OffsetIndices node_offsets = create_node_vert_offsets(nodes, node_mask, node_offset_data);
  Array<float> all_factors(node_offsets.total_size());

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i, const int pos) {
        LocalData &tls = all_tls.local();
        const Span<int> verts = nodes[i].verts();

        /* Base view: region clipping and falloff distances are measured on the un-layered base so
         * the factors are not modulated by the layer pattern. The texture keeps sampling the
         * composed surface (see #sculpt_apply_texture). */
        const Span<float3> base_view_positions = layers::base_view_gather_mesh(
            object, verts, position_data.eval, tls.base_view_storage);

        const MutableSpan<float> factors = all_factors.as_mutable_span().slice(node_offsets[pos]);
        fill_factor_from_hide_and_mask(
            attribute_data.hide_vert, attribute_data.mask, verts, factors);
        if (base_view_positions.is_empty()) {
          filter_region_clip_factors(ss, position_data.eval, verts, factors);
        }
        else {
          filter_region_clip_factors(ss, base_view_positions, factors);
        }
        if (brush.flag & BRUSH_FRONTFACE) {
          calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
        }

        tls.distances.resize(verts.size());
        const MutableSpan<float> distances = tls.distances;
        if (base_view_positions.is_empty()) {
          calc_brush_distances(
              ss, position_data.eval, verts, eBrushFalloffShape(brush.falloff_shape), distances);
        }
        else {
          calc_brush_distances(
              ss, base_view_positions, eBrushFalloffShape(brush.falloff_shape), distances);
        }
        filter_distances_with_radius(cache.radius, distances, factors);
        apply_hardness_to_distances(cache, distances);
        calc_brush_strength_factors(cache, brush, distances, factors);

        auto_mask::calc_vert_factors(
            depsgraph, object, cache.automasking.get(), nodes[i], verts, factors);

        calc_brush_texture_factors(ss, brush, position_data.eval, verts, factors);

        scale_factors(factors, cache.bstrength);
        clamp_factors(factors);
      },
      exec_mode::grain_size(1));

  for ([[maybe_unused]] const int iteration : IndexRange(brush.surface_smooth_iterations)) {
    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Span<int> verts = nodes[i].verts();
          const MutableSpan positions = gather_data_mesh(position_data.eval, verts, tls.positions);
          const OrigPositionData orig_data = orig_position_data_get_mesh(object, nodes[i]);
          const Span<float> factors = all_factors.as_span().slice(node_offsets[pos]);

          const GroupedSpan<int> neighbors = calc_vert_neighbors(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 attribute_data.hide_poly,
                                                                 verts,
                                                                 tls.neighbor_offsets,
                                                                 tls.neighbor_data);

          tls.average_positions.resize(verts.size());
          const MutableSpan<float3> average_positions = tls.average_positions;
          smooth::neighbor_data_average_mesh(position_data.eval, neighbors, average_positions);

          tls.laplacian_disp.resize(verts.size());
          const MutableSpan<float3> laplacian_disp = tls.laplacian_disp;
          tls.translations.resize(verts.size());
          const MutableSpan<float3> translations = tls.translations;
          smooth::surface_smooth_laplacian_step(positions,
                                                orig_data.positions,
                                                average_positions,
                                                alpha,
                                                laplacian_disp,
                                                translations);

          const Span<float3> base_view = layers::stroke_base_view(object);
          if (!base_view.is_empty()) {
            /* Base-aware step: every term above is affine in the positions, so running it on the
             * base (`position - O`) instead of the composed surface amounts to adding
             * `O[v] - avg(O)` to both results. The layer detail then rides on the smoothed base
             * rather than being flattened into it. Averaging the offset with the same operator
             * (same neighbor lists) keeps boundary handling exact, and the raw offset is correct
             * here (no #layers::stroke_base_view_dc): only differences of positions are used, which
             * a constant shift leaves unchanged.
             *
             * The displacement step needs no such correction: it reads back the base-space
             * displacements stored here and is purely differential in them. */
            tls.base_view_avg.resize(verts.size());
            const MutableSpan<float3> base_view_avg = tls.base_view_avg;
            smooth::neighbor_data_average_mesh(base_view, neighbors, base_view_avg);
            for (const int vi : verts.index_range()) {
              const float3 correction = base_view[verts[vi]] - base_view_avg[vi];
              laplacian_disp[vi] += correction;
              translations[vi] += correction;
            }
          }
          scale_translations(translations, factors);

          scatter_data_mesh(laplacian_disp.as_span(), verts, all_laplacian_disp);

          clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
          position_data.deform(translations, verts);
        },
        exec_mode::grain_size(1));

    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Span<int> verts = nodes[i].verts();
          const Span<float> factors = all_factors.as_span().slice(node_offsets[pos]);

          const MutableSpan<float3> laplacian_disp = gather_data_mesh(
              all_laplacian_disp.as_span(), verts, tls.laplacian_disp);

          const GroupedSpan<int> neighbors = calc_vert_neighbors(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 attribute_data.hide_poly,
                                                                 verts,
                                                                 tls.neighbor_offsets,
                                                                 tls.neighbor_data);

          tls.average_positions.resize(verts.size());
          const MutableSpan<float3> average_laplacian_disps = tls.average_positions;
          smooth::neighbor_data_average_mesh(
              all_laplacian_disp.as_span(), neighbors, average_laplacian_disps);

          tls.translations.resize(verts.size());
          const MutableSpan<float3> translations = tls.translations;
          smooth::surface_smooth_displace_step(
              laplacian_disp, average_laplacian_disps, beta, translations);
          scale_translations(translations, factors);

          clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
          position_data.deform(translations, verts);
        },
        exec_mode::grain_size(1));
  }
}

BLI_NOINLINE static void do_surface_smooth_brush_grids(
    const Depsgraph &depsgraph,
    const Sculpt &sd,
    const Brush &brush,
    const IndexMask &node_mask,
    Object &object,
    const MutableSpan<float3> all_laplacian_disp)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const StrokeCache &cache = *ss.cache;
  const float alpha = brush.surface_smooth_shape_preservation;
  const float beta = brush.surface_smooth_current_vertex;
  MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  Array<int> node_offset_data;
  const OffsetIndices node_offsets = create_node_vert_offsets(
      BKE_subdiv_ccg_key_top_level(subdiv_ccg), nodes, node_mask, node_offset_data);
  Array<float> all_factors(node_offsets.total_size());

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i, const int pos) {
        LocalData &tls = all_tls.local();
        const Span<int> grids = nodes[i].grids();
        const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

        /* Base view: see the mesh variant in #do_surface_smooth_brush_mesh. */
        const Span<float3> calc_positions = layers::base_view_adjust_compact_grids(
            object, subdiv_ccg, grids, positions, tls.base_view_storage);

        const MutableSpan<float> factors = all_factors.as_mutable_span().slice(node_offsets[pos]);
        fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
        filter_region_clip_factors(ss, calc_positions, factors);
        if (brush.flag & BRUSH_FRONTFACE) {
          calc_front_face(cache.view_normal_symm, subdiv_ccg, grids, factors);
        }

        tls.distances.resize(positions.size());
        const MutableSpan<float> distances = tls.distances;
        calc_brush_distances(
            ss, calc_positions, eBrushFalloffShape(brush.falloff_shape), distances);
        filter_distances_with_radius(cache.radius, distances, factors);
        apply_hardness_to_distances(cache, distances);
        calc_brush_strength_factors(cache, brush, distances, factors);

        auto_mask::calc_grids_factors(
            depsgraph, object, cache.automasking.get(), nodes[i], grids, factors);

        calc_brush_texture_factors(ss, brush, positions, factors);

        scale_factors(factors, cache.bstrength);
        clamp_factors(factors);
      },
      exec_mode::grain_size(1));

  for ([[maybe_unused]] const int iteration : IndexRange(brush.surface_smooth_iterations)) {
    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Span<int> grids = nodes[i].grids();
          const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);
          const OrigPositionData orig_data = orig_position_data_get_grids(object, nodes[i]);
          const Span<float> factors = all_factors.as_span().slice(node_offsets[pos]);

          tls.average_positions.resize(positions.size());
          const MutableSpan<float3> average_positions = tls.average_positions;
          smooth::average_data_grids(
              subdiv_ccg, subdiv_ccg.positions.as_span(), grids, average_positions);

          tls.laplacian_disp.resize(positions.size());
          const MutableSpan<float3> laplacian_disp = tls.laplacian_disp;
          tls.translations.resize(positions.size());
          const MutableSpan<float3> translations = tls.translations;
          smooth::surface_smooth_laplacian_step(positions,
                                                orig_data.positions,
                                                average_positions,
                                                alpha,
                                                laplacian_disp,
                                                translations);

          const Span<float3> base_view = layers::stroke_base_view(object);
          if (!base_view.is_empty()) {
            /* Base-aware step; see the mesh variant in #do_surface_smooth_brush_mesh. */
            tls.base_view_avg.resize(positions.size());
            const MutableSpan<float3> base_view_avg = tls.base_view_avg;
            smooth::average_data_grids(subdiv_ccg, base_view, grids, base_view_avg);
            const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
            for (const int grid : grids.index_range()) {
              const IndexRange node_range = bke::ccg::grid_range(key, grid);
              const IndexRange grid_range = bke::ccg::grid_range(key, grids[grid]);
              for (const int offset : IndexRange(key.grid_area)) {
                const float3 correction = base_view[grid_range[offset]] -
                                          base_view_avg[node_range[offset]];
                laplacian_disp[node_range[offset]] += correction;
                translations[node_range[offset]] += correction;
              }
            }
          }
          scale_translations(translations, factors);

          scatter_data_grids(subdiv_ccg, laplacian_disp.as_span(), grids, all_laplacian_disp);

          clip_and_lock_translations(sd, ss, positions, translations);
          apply_translations(translations, grids, subdiv_ccg);
        },
        exec_mode::grain_size(1));

    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Span<int> grids = nodes[i].grids();
          const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);
          const Span<float> factors = all_factors.as_span().slice(node_offsets[pos]);

          const MutableSpan<float3> laplacian_disp = gather_data_grids(
              subdiv_ccg, all_laplacian_disp.as_span(), grids, tls.laplacian_disp);

          tls.average_positions.resize(positions.size());
          const MutableSpan<float3> average_laplacian_disps = tls.average_positions;
          smooth::average_data_grids(
              subdiv_ccg, all_laplacian_disp.as_span(), grids, average_laplacian_disps);

          tls.translations.resize(positions.size());
          const MutableSpan<float3> translations = tls.translations;
          smooth::surface_smooth_displace_step(
              laplacian_disp, average_laplacian_disps, beta, translations);
          scale_translations(translations, factors);

          clip_and_lock_translations(sd, ss, positions, translations);
          apply_translations(translations, grids, subdiv_ccg);
        },
        exec_mode::grain_size(1));
  }
}

BLI_NOINLINE static void do_surface_smooth_brush_bmesh(
    const Depsgraph &depsgraph,
    const Sculpt &sd,
    const Brush &brush,
    const IndexMask &node_mask,
    Object &object,
    const MutableSpan<float3> all_laplacian_disp)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const StrokeCache &cache = *ss.cache;
  const float alpha = brush.surface_smooth_shape_preservation;
  const float beta = brush.surface_smooth_current_vertex;
  MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();

  Array<int> node_offset_data;
  const OffsetIndices node_offsets = create_node_vert_offsets_bmesh(
      nodes, node_mask, node_offset_data);
  Array<float> all_factors(node_offsets.total_size());

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i, const int pos) {
        LocalData &tls = all_tls.local();
        const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&nodes[i]);
        const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

        const MutableSpan<float> factors = all_factors.as_mutable_span().slice(node_offsets[pos]);
        fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
        filter_region_clip_factors(ss, positions, factors);
        if (brush.flag & BRUSH_FRONTFACE) {
          calc_front_face(cache.view_normal_symm, verts, factors);
        }

        tls.distances.resize(positions.size());
        const MutableSpan<float> distances = tls.distances;
        calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
        filter_distances_with_radius(cache.radius, distances, factors);
        apply_hardness_to_distances(cache, distances);
        calc_brush_strength_factors(cache, brush, distances, factors);

        auto_mask::calc_vert_factors(
            depsgraph, object, cache.automasking.get(), nodes[i], verts, factors);

        calc_brush_texture_factors(ss, brush, positions, factors);

        scale_factors(factors, cache.bstrength);
        clamp_factors(factors);
      },
      exec_mode::grain_size(1));

  for ([[maybe_unused]] const int iteration : IndexRange(brush.surface_smooth_iterations)) {
    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&nodes[i]);
          const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);
          Array<float3> orig_positions(verts.size());
          Array<float3> orig_normals(verts.size());
          orig_position_data_gather_bmesh(*ss.bm_log, verts, orig_positions, orig_normals);
          const Span<float> factors = all_factors.as_span().slice(node_offsets[pos]);

          tls.average_positions.resize(positions.size());
          const MutableSpan<float3> average_positions = tls.average_positions;
          smooth::neighbor_position_average_bmesh(verts, average_positions);

          tls.laplacian_disp.resize(positions.size());
          const MutableSpan<float3> laplacian_disp = tls.laplacian_disp;
          tls.translations.resize(positions.size());
          const MutableSpan<float3> translations = tls.translations;
          smooth::surface_smooth_laplacian_step(
              positions, orig_positions, average_positions, alpha, laplacian_disp, translations);
          scale_translations(translations, factors);

          scatter_data_bmesh(laplacian_disp.as_span(), verts, all_laplacian_disp);

          clip_and_lock_translations(sd, ss, positions, translations);
          apply_translations(translations, verts);
        },
        exec_mode::grain_size(1));

    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&nodes[i]);
          const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);
          const Span<float> factors = all_factors.as_span().slice(node_offsets[pos]);

          const MutableSpan<float3> laplacian_disp = gather_data_bmesh(
              all_laplacian_disp.as_span(), verts, tls.laplacian_disp);

          tls.average_positions.resize(positions.size());
          const MutableSpan<float3> average_laplacian_disps = tls.average_positions;
          smooth::average_data_bmesh(all_laplacian_disp.as_span(), verts, average_laplacian_disps);

          tls.translations.resize(positions.size());
          const MutableSpan<float3> translations = tls.translations;
          smooth::surface_smooth_displace_step(
              laplacian_disp, average_laplacian_disps, beta, translations);
          scale_translations(translations, factors);

          clip_and_lock_translations(sd, ss, positions, translations);
          apply_translations(translations, verts);
        },
        exec_mode::grain_size(1));
  }
}

}  // namespace surface_smooth_cc

void do_surface_smooth_brush(const Depsgraph &depsgraph,
                             const Sculpt &sd,
                             Object &object,
                             const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  if (ss.cache->surface_smooth_laplacian_disp.is_empty()) {
    BLI_assert_msg(stroke_is_first_brush_step(*ss.cache),
                   "Should only be allocated on the first step");
    ss.cache->surface_smooth_laplacian_disp = Array<float3>(vertex_count_get(object), float3(0));
  }

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      do_surface_smooth_brush_mesh(
          depsgraph, sd, brush, node_mask, object, ss.cache->surface_smooth_laplacian_disp);
      break;
    case bke::pbvh::Type::Grids: {
      do_surface_smooth_brush_grids(
          depsgraph, sd, brush, node_mask, object, ss.cache->surface_smooth_laplacian_disp);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      vert_random_access_ensure(object);
      do_surface_smooth_brush_bmesh(
          depsgraph, sd, brush, node_mask, object, ss.cache->surface_smooth_laplacian_disp);
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.update_bounds(depsgraph, object);
}

}  // namespace blender::ed::sculpt_paint::brushes
