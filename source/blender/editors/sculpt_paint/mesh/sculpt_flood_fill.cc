/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "sculpt_flood_fill.hh"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_object_types.hh"

#include "DNA_mesh_types.h"

#include "../paint_intern.hh"
#include "sculpt_hide.hh"
#include "sculpt_intern.hh"

#include "bmesh.hh"

#include "BLI_time.h"
#include <cinttypes>

static bool g_perf_log_flood_fill = false;

/* -------------------------------------------------------------------- */
/** \name Sculpt Flood Fill API
 *
 * Iterate over connected vertices, starting from one or more initial vertices.
 * \{ */

namespace blender::ed::sculpt_paint::flood_fill {

void FillDataMesh::add_initial(const int vertex)
{
  this->queue_current.append(vertex);
}

void FillDataMesh::add_initial(const Span<int> verts)
{
  for (const int vert : verts) {
    this->add_initial(vert);
  }
}

void FillDataGrids::add_initial(const SubdivCCGCoord vertex)
{
  this->queue.push(vertex);
}

void FillDataGrids::add_initial(const CCGKey &key, const Span<int> verts)
{
  for (const int vert : verts) {
    this->add_initial(SubdivCCGCoord::from_index(key, vert));
  }
}

void FillDataBMesh::add_initial(BMVert *vertex)
{
  this->queue.push(vertex);
}

void FillDataBMesh::add_initial(BMesh &bm, const Span<int> verts)
{
  for (const int vert : verts) {
    this->add_initial(BM_vert_at_index(&bm, vert));
  }
}

void FillDataMesh::add_and_skip_initial(const int vertex)
{
  this->queue_current.append(vertex);
  this->visited_verts[vertex].set();
}

void FillDataGrids::add_and_skip_initial(const SubdivCCGCoord vertex, const int index)
{
  this->queue.push(vertex);
  this->visited_verts[index].set();
}

void FillDataBMesh::add_and_skip_initial(BMVert *vertex, const int index)
{
  this->queue.push(vertex);
  this->visited_verts[index].set();
}

void FillDataMesh::execute(Object &object,
                           const GroupedSpan<int> /*vert_to_face_map*/,
                           FunctionRef<bool(int from_v, int to_v)> func)
{
  printf("DEBUG: FillDataMesh::execute CALLED\n");
  const double t_start = g_perf_log_flood_fill ? BLI_time_now_seconds() : 0.0;
  uint64_t queue_pops = 0;
  uint64_t neighbors_total = 0;
  uint64_t skipped_visited = 0;
  uint64_t skipped_hidden = 0;
  uint64_t enqueued = 0;

  double t_neighbor_get = 0.0, t_func_call = 0.0;

  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const Span<int2> edges = mesh.edges();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_edge = *attributes.lookup<bool>(".hide_edge",
                                                              bke::AttrDomain::Edge);
  const VArraySpan<bool> hide_vert = *attributes.lookup<bool>(".hide_vert",
                                                              bke::AttrDomain::Point);

  SculptSession &ss = *object.runtime->sculpt_session;
  if (ss.vert_to_edge_map.is_empty()) {
    ss.vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
        edges, mesh.verts_num, ss.vert_to_edge_offsets, ss.vert_to_edge_indices);
  }

  Vector<int> neighbors;
  while (!this->queue_current.is_empty()) {
    for (const int from_v : this->queue_current) {
      queue_pops++;

      double t1 = g_perf_log_flood_fill ? BLI_time_now_seconds() : 0.0;
      vert_neighbors_get_mesh(edges, ss.vert_to_edge_map, hide_edge, from_v, neighbors);
      if (!this->fake_neighbors.is_empty() && this->fake_neighbors[from_v] != FAKE_NEIGHBOR_NONE) {
        neighbors.append(this->fake_neighbors[from_v]);
      }
      t_neighbor_get += g_perf_log_flood_fill ? (BLI_time_now_seconds() - t1) * 1000.0 : 0.0;

      neighbors_total += uint64_t(neighbors.size());

      for (const int neighbor : neighbors) {
        if (this->visited_verts[neighbor]) {
          skipped_visited++;
          continue;
        }

        if (!hide_vert.is_empty() && hide_vert[neighbor]) {
          skipped_hidden++;
          continue;
        }

        this->visited_verts[neighbor].set();
        double t4 = g_perf_log_flood_fill ? BLI_time_now_seconds() : 0.0;
        if (func(from_v, neighbor)) {
          this->queue_next.append(neighbor);
          enqueued++;
        }
        t_func_call += g_perf_log_flood_fill ? (BLI_time_now_seconds() - t4) * 1000.0 : 0.0;
      }
    }
    this->queue_current.clear();
    std::swap(this->queue_current, this->queue_next);
  }

  if (g_perf_log_flood_fill) {
    double total = (BLI_time_now_seconds() - t_start) * 1000.0;
    printf("FillDataMesh::execute: pops=%" PRIu64 " neighbors=%" PRIu64
           " skip_visited=%" PRIu64 " skip_hidden=%" PRIu64
           " enqueued=%" PRIu64 " total=%.2fms"
           " (neighbor_get=%.2fms func_call=%.2fms)\n",
           queue_pops,
           neighbors_total,
           skipped_visited,
           skipped_hidden,
           enqueued,
           total,
           t_neighbor_get,
           t_func_call);
  }
}

void FillDataGrids::execute(
    Object & /*object*/,
    const SubdivCCG &subdiv_ccg,
    FunctionRef<bool(SubdivCCGCoord from_v, SubdivCCGCoord to_v, bool is_duplicate)> func)
{
  printf("DEBUG: FillDataGrids::execute CALLED\n");
  const double t_start = g_perf_log_flood_fill ? BLI_time_now_seconds() : 0.0;
  uint64_t queue_pops = 0;
  uint64_t neighbors_total = 0;
  uint64_t duplicates = 0;
  uint64_t skipped_hidden = 0;
  uint64_t enqueued = 0;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  while (!this->queue.empty()) {
    SubdivCCGCoord from_v = this->queue.front();
    this->queue.pop();
    queue_pops++;

    SubdivCCGNeighbors neighbors;
    BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, from_v, true, neighbors);
    if (!this->fake_neighbors.is_empty() &&
        this->fake_neighbors[from_v.to_index(key)] != FAKE_NEIGHBOR_NONE)
    {
      neighbors.coords.insert(
          0, SubdivCCGCoord::from_index(key, this->fake_neighbors[from_v.to_index(key)]));
    }

    const int num_unique = neighbors.coords.size() - neighbors.num_duplicates;
    neighbors_total += uint64_t(neighbors.coords.size());

    /* Flood fill expects the duplicate entries to be passed to the per-neighbor lambda first, so
     * iterate from the end of the vector to the beginning. */
    for (int i = neighbors.coords.size() - 1; i >= 0; i--) {
      SubdivCCGCoord neighbor = neighbors.coords[i];
      const int index_in_grid = neighbor.y * key.grid_size + neighbor.x;
      const int index = neighbor.grid_index * key.grid_area + index_in_grid;
      if (this->visited_verts[index]) {
        continue;
      }

      if (!subdiv_ccg.grid_hidden.is_empty() &&
          subdiv_ccg.grid_hidden[neighbor.grid_index][index_in_grid])
      {
        skipped_hidden++;
        continue;
      }

      this->visited_verts[index].set();
      const bool is_duplicate = i >= num_unique;
      if (is_duplicate) {
        duplicates++;
      }
      if (func(from_v, neighbor, is_duplicate)) {
        this->queue.push(neighbor);
        enqueued++;
      }
    }
  }

  if (g_perf_log_flood_fill) {
    printf("FillDataGrids::execute: pops=%" PRIu64 " neighbors=%" PRIu64
           " duplicates=%" PRIu64 " skip_hidden=%" PRIu64
           " enqueued=%" PRIu64 " time=%.2fms\n",
           queue_pops, neighbors_total, duplicates,
           skipped_hidden, enqueued,
           (BLI_time_now_seconds() - t_start) * 1000.0);
  }
}

void FillDataBMesh::execute(Object &object, FunctionRef<bool(BMVert *from_v, BMVert *to_v)> func)
{
  const double t_start = g_perf_log_flood_fill ? BLI_time_now_seconds() : 0.0;
  uint64_t queue_pops = 0;
  uint64_t neighbors_total = 0;
  uint64_t skipped_visited = 0;
  uint64_t skipped_hidden = 0;
  uint64_t enqueued = 0;

  BMesh *bm = object.runtime->sculpt_session->bm;
  BMeshNeighborVerts neighbors;
  while (!this->queue.empty()) {
    BMVert *from_v = this->queue.front();
    this->queue.pop();
    queue_pops++;

    if (!this->fake_neighbors.is_empty() &&
        this->fake_neighbors[BM_elem_index_get(from_v)] != FAKE_NEIGHBOR_NONE)
    {
      neighbors.append(BM_vert_at_index(bm, this->fake_neighbors[BM_elem_index_get(from_v)]));
    }

    for (BMVert *neighbor : vert_neighbors_get_bmesh(*from_v, neighbors)) {
      const int neighbor_idx = BM_elem_index_get(neighbor);
      if (this->visited_verts[neighbor_idx]) {
        skipped_visited++;
        continue;
      }

      if (BM_elem_flag_test(neighbor, BM_ELEM_HIDDEN)) {
        skipped_hidden++;
        continue;
      }

      this->visited_verts[neighbor_idx].set();
      if (func(from_v, neighbor)) {
        this->queue.push(neighbor);
        enqueued++;
      }
      neighbors_total++;
    }
  }

  if (g_perf_log_flood_fill) {
    printf("FillDataBMesh::execute: pops=%" PRIu64 " neighbors=%" PRIu64
           " skip_visited=%" PRIu64 " skip_hidden=%" PRIu64
           " enqueued=%" PRIu64 " time=%.2fms\n",
           queue_pops, neighbors_total, skipped_visited,
           skipped_hidden, enqueued,
           (BLI_time_now_seconds() - t_start) * 1000.0);
  }
}

}  // namespace blender::ed::sculpt_paint::flood_fill

/** \} */
