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
#include "BLI_index_mask.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_boundary.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"
#include "editors/sculpt_paint/mesh/sculpt_smooth.hh"
#include "editors/sculpt_paint/mesh/sculpt_smooth_voxel.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {

inline namespace smooth_cc {

static Vector<float> iteration_strengths(const float strength)
{
  constexpr int max_iterations = 4;

  BLI_assert_msg(strength >= 0.0f,
                 "The smooth brush expects a non-negative strength to behave properly");
  const float clamped_strength = std::min(strength, 1.0f);

  const int count = int(clamped_strength * max_iterations);
  const float last = max_iterations * (clamped_strength - float(count) / max_iterations);
  Vector<float> result;
  result.append_n_times(1.0f, count);
  result.append(last);
  return result;
}

struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<int> neighbor_offsets;
  Vector<int> neighbor_data;
  Vector<float3> new_positions;
  Vector<float3> translations;
};

BLI_NOINLINE static void apply_positions_faces(const Sculpt &sd,
                                               const bke::pbvh::MeshNode &node,
                                               Object &object,
                                               LocalData &tls,
                                               const Span<float> factors,
                                               const Span<float3> new_positions,
                                               const PositionDeformData &position_data)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;

  const Span<int> verts = node.verts();

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_new_positions(new_positions, verts, position_data.eval, translations);
  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

BLI_NOINLINE static void do_smooth_brush_mesh(const Depsgraph &depsgraph,
                                              const Sculpt &sd,
                                              const Brush &brush,
                                              Object &object,
                                              const IndexMask &node_mask,
                                              const float brush_strength)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const MeshAttributeData attribute_data(mesh);

  const PositionDeformData position_data(depsgraph, object);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);

  Array<int> node_offset_data;
  const OffsetIndices<int> node_vert_offsets = create_node_vert_offsets(
      nodes, node_mask, node_offset_data);
  Array<float3> new_positions(node_vert_offsets.total_size());
  Array<float> all_factors(node_vert_offsets.total_size());
  Array<float> all_distances(node_vert_offsets.total_size());

  threading::EnumerableThreadSpecific<LocalData> all_tls;

  /* The Shape path averages over a fixed world-space scale rather than the topological one-ring,
   * so a stroke smooths broad forms just as strongly on a dense mesh as on a coarse one. Its
   * region gather and voxel field run once per iteration, before the per-node loop, which then
   * only reads the result back for the vertices it owns. */
  const bool use_shape = brush.smooth_algorithm == BRUSH_SMOOTH_SHAPE;

  smooth::VoxelSmoothParams voxel_params;
  voxel_params.kernel_radius = ss.cache->radius * brush.smooth_scale;
  voxel_params.preserve_volume = (brush.smooth_flag & BRUSH_SMOOTH_PRESERVE_VOLUME) != 0;

  smooth::SpatialRegion region;
  Array<float3> region_targets;

  /* Calculate the new positions into a separate array in a separate loop because multiple loops
   * are updated in parallel. Without this there would be non-threadsafe access to changing
   * positions in other bke::pbvh::Tree nodes. */
  /* The topological path needs several sub-passes because each Laplacian step moves a vertex only a
   * little. The voxel field already produces a fully smoothed target in a single solve, so the
   * Shape path runs exactly one pass at the full strength. Iterating it would re-rasterize the
   * already-moved vertices into the voxel grid every pass, snapping each cell's vertices toward its
   * centroid and baking the grid into the surface as voxel-sized stair-stepped facets. */
  const Vector<float> pass_strengths = use_shape ?
                                           Vector<float>{std::min(brush_strength, 1.0f)} :
                                           iteration_strengths(brush_strength);
  for (const float strength : pass_strengths) {
    if (use_shape) {
      /* A zero-strength iteration would still gather the full region and solve the voxel field,
       * only to multiply every translation by zero afterward, so skip it outright. */
      if (strength == 0.0f) {
        continue;
      }

      /* Two kernel radii of margin beyond the brush footprint keep the field fully populated for
       * the vertices at the edge of the stroke, which is what prevents a seam there. */
      const float gather_radius = ss.cache->radius + 2.0f * voxel_params.kernel_radius;
      smooth::gather_spatial_region(
          depsgraph, object, ss.cache->location_symm, gather_radius, region);
      region_targets.reinitialize(region.elems.size());
      smooth::voxel_smooth_positions(region.positions,
                                     region.normals,
                                     region.field_mask,
                                     region.anchor_mask,
                                     voxel_params,
                                     region_targets);
    }

    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Span<int> verts = nodes[i].verts();
          const MutableSpan<float> node_factors = all_factors.as_mutable_span().slice(
              node_vert_offsets[pos]);
          calc_factors_common_mesh_indexed(
              depsgraph,
              brush,
              object,
              attribute_data,
              position_data.eval,
              vert_normals,
              nodes[i],
              node_factors,
              all_distances.as_mutable_span().slice(node_vert_offsets[pos]));
          scale_factors(node_factors, strength);

          const MutableSpan<float3> node_new_positions =
              new_positions.as_mutable_span().slice(node_vert_offsets[pos]);

          if (use_shape) {
            /* The region is gathered around the same symmetry-mirrored center as the node mask,
             * with sufficient margin beyond the radius, ensuring every node is inside and its
             * vertices form one contiguous block. */
            const int offset = region.node_offsets[i];
            if (offset < 0) {
              /* TEMP DEBUG (remove once the Shape-mode PBVH-tile artifact is diagnosed): this
               * branch should be unreachable for a node in node_mask, since gather_radius is
               * always >= the brush radius used to build node_mask and both use bounds_orig()
               * around the same center. If it fires, the region and node_mask disagree right at
               * this node's boundary, which is a candidate root cause for tile-shaped seams. */
              if (!verts.is_empty()) {
                const Bounds<float3> &b = nodes[i].bounds_orig();
                const float gather_radius = ss.cache->radius + 2.0f * voxel_params.kernel_radius;
                fprintf(stderr,
                        "[smooth-shape-debug] node %d MISSING from region: %d verts frozen | "
                        "bounds_orig min=(%.4f,%.4f,%.4f) max=(%.4f,%.4f,%.4f) | "
                        "cache center=(%.4f,%.4f,%.4f) brush_radius=%.4f gather_radius=%.4f\n",
                        i,
                        int(verts.size()),
                        b.min.x, b.min.y, b.min.z,
                        b.max.x, b.max.y, b.max.z,
                        ss.cache->location_symm.x, ss.cache->location_symm.y,
                        ss.cache->location_symm.z,
                        ss.cache->radius,
                        gather_radius);
              }
              for (const int v : verts.index_range()) {
                node_new_positions[v] = position_data.eval[verts[v]];
              }
            }
            else {
              node_new_positions.copy_from(region_targets.as_span().slice(offset, verts.size()));
            }
          }
          else {
            const GroupedSpan<int> neighbors = calc_vert_neighbors_interior(
                faces,
                corner_verts,
                vert_to_face_map,
                ss.boundary_info_cache->verts,
                ss.boundary_info_cache->edges,
                attribute_data.hide_poly,
                verts,
                node_factors,
                tls.neighbor_offsets,
                tls.neighbor_data);
            smooth::neighbor_data_average_mesh_check_loose(
                position_data.eval, verts, neighbors, node_new_positions);
          }
        },
        exec_mode::grain_size(1));

    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          apply_positions_faces(sd,
                                nodes[i],
                                object,
                                tls,
                                all_factors.as_mutable_span().slice(node_vert_offsets[pos]),
                                new_positions.as_span().slice(node_vert_offsets[pos]),
                                position_data);
        },
        exec_mode::grain_size(1));
  }
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const OffsetIndices<int> faces,
                       const Span<int> corner_verts,
                       const BitSpan boundary_verts,
                       const Set<OrderedEdge> &boundary_edges,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  calc_factors_common_grids(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  scale_factors(tls.factors, strength);

  tls.new_positions.resize(positions.size());
  const MutableSpan<float3> new_positions = tls.new_positions;
  smooth::neighbor_position_average_interior_grids(faces,
                                                   corner_verts,
                                                   boundary_verts,
                                                   boundary_edges,
                                                   subdiv_ccg,
                                                   grids,
                                                   tls.factors,
                                                   new_positions);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_new_positions(new_positions, positions, translations);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

BLI_NOINLINE static void do_smooth_brush_grids(const Depsgraph &depsgraph,
                                               const Sculpt &sd,
                                               const Brush &brush,
                                               Object &object,
                                               const IndexMask &node_mask,
                                               const float brush_strength)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const Mesh &base_mesh = *id_cast<const Mesh *>(object.data);
  const OffsetIndices faces = base_mesh.faces();
  const Span<int> corner_verts = base_mesh.corner_verts();

  /* As in #do_smooth_brush_mesh, the Shape path averages over a fixed world-space scale rather
   * than the topological one-ring, so multires benefits from it the same way a dense mesh does. */
  const bool use_shape = brush.smooth_algorithm == BRUSH_SMOOTH_SHAPE;

  smooth::VoxelSmoothParams voxel_params;
  voxel_params.kernel_radius = ss.cache->radius * brush.smooth_scale;
  voxel_params.preserve_volume = (brush.smooth_flag & BRUSH_SMOOTH_PRESERVE_VOLUME) != 0;

  smooth::SpatialRegion region;
  Array<float3> region_targets;

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

  /* One pass at full strength for Shape: the voxel field is already a fully smoothed target, and
   * iterating it would re-rasterize moved grid elements into the voxel grid every pass, snapping
   * each cell's elements toward its centroid and baking the grid in as stair-stepped facets. */
  const Vector<float> pass_strengths = use_shape ?
                                           Vector<float>{std::min(brush_strength, 1.0f)} :
                                           iteration_strengths(brush_strength);
  for (const float strength : pass_strengths) {
    if (!use_shape) {
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_grids(depsgraph,
                       sd,
                       faces,
                       corner_verts,
                       ss.boundary_info_cache->verts,
                       ss.boundary_info_cache->edges,
                       object,
                       brush,
                       strength,
                       nodes[i],
                       tls);
          },
          exec_mode::grain_size(1));
      continue;
    }

    /* A zero-strength iteration would still gather the full region and solve the voxel field,
     * only to multiply every translation by zero afterward, so skip it outright. */
    if (strength == 0.0f) {
      continue;
    }

    /* Two kernel radii of margin beyond the brush footprint keep the field fully populated for
     * the grid elements at the edge of the stroke, which is what prevents a seam there. */
    const float gather_radius = ss.cache->radius + 2.0f * voxel_params.kernel_radius;
    /* The gather must be centered on the same symmetry-mirrored point used to build node_mask,
     * not the unmirrored input location: otherwise, under mirror symmetry, the gathered region
     * would land on the opposite half from the nodes being smoothed, every node would miss the
     * region, and this pass would silently do nothing. */
    smooth::gather_spatial_region(
        depsgraph, object, ss.cache->location_symm, gather_radius, region);
    region_targets.reinitialize(region.elems.size());
    smooth::voxel_smooth_positions(region.positions,
                                   region.normals,
                                   region.field_mask,
                                   region.anchor_mask,
                                   voxel_params,
                                   region_targets);

    const int grid_area = subdiv_ccg.grid_area;
    node_mask.foreach_index(
        [&](const int i) {
          LocalData &tls = all_tls.local();
          const Span<int> grids = nodes[i].grids();
          const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

          calc_factors_common_grids(
              depsgraph, brush, object, positions, nodes[i], tls.factors, tls.distances);
          scale_factors(tls.factors, strength);

          /* The region is gathered around the same symmetry-mirrored center as the node mask,
           * with sufficient margin beyond the radius, ensuring every node's grids are inside and
           * form one contiguous block. */
          const int elem_num = grids.size() * grid_area;
          tls.new_positions.resize(elem_num);
          const MutableSpan<float3> new_positions = tls.new_positions;
          const int offset = region.node_offsets[i];
          if (offset < 0) {
            new_positions.copy_from(positions);
          }
          else {
            new_positions.copy_from(region_targets.as_span().slice(offset, elem_num));
          }

          tls.translations.resize(elem_num);
          const MutableSpan<float3> translations = tls.translations;
          translations_from_new_positions(new_positions, positions, translations);
          scale_translations(translations, tls.factors);

          clip_and_lock_translations(sd, ss, positions, translations);
          apply_translations(translations, grids, subdiv_ccg);
        },
        exec_mode::grain_size(1));
  }
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  calc_factors_common_bmesh(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  scale_factors(tls.factors, strength);

  tls.new_positions.resize(verts.size());
  const MutableSpan<float3> new_positions = tls.new_positions;
  smooth::neighbor_position_average_interior_bmesh(verts, tls.factors, new_positions);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_new_positions(new_positions, positions, translations);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace smooth_cc

void do_smooth_brush(const Depsgraph &depsgraph,
                     const Sculpt &sd,
                     Object &object,
                     const IndexMask &node_mask,
                     const float brush_strength)
{
  PRF_scope(ProfileCategory::Editor);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  boundary::ensure_boundary_info(object);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      do_smooth_brush_mesh(depsgraph, sd, brush, object, node_mask, brush_strength);
      break;
    case bke::pbvh::Type::Grids:
      do_smooth_brush_grids(depsgraph, sd, brush, object, node_mask, brush_strength);
      break;
    case bke::pbvh::Type::BMesh: {
      vert_random_access_ensure(object);
      threading::EnumerableThreadSpecific<LocalData> all_tls;
      for (const float strength : iteration_strengths(brush_strength)) {
        MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
        node_mask.foreach_index(
            [&](const int i) {
              LocalData &tls = all_tls.local();
              calc_bmesh(depsgraph, sd, object, brush, strength, nodes[i], tls);
            },
            exec_mode::grain_size(1));
      }
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.update_bounds(depsgraph, object);
}

}  // namespace blender::ed::sculpt_paint::brushes
