/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <chrono>
#include <cstdio>
#include <thread>

#include "BLI_array_utils.hh"
#include "BLI_implicit_sharing.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"

#include "DNA_mesh_types.h"

namespace blender::bke {

static bool mesh_append_attribute_is_topology_builtin(const StringRef name)
{
  return name == "position" || name == ".edge_verts" || name == ".corner_vert" ||
         name == ".corner_edge";
}

static int64_t mesh_append_domain_size(const AttrDomain domain,
                                       const int point_num,
                                       const int edge_num,
                                       const int face_num,
                                       const int corner_num)
{
  switch (domain) {
    case AttrDomain::Point:
      return point_num;
    case AttrDomain::Edge:
      return edge_num;
    case AttrDomain::Face:
      return face_num;
    case AttrDomain::Corner:
      return corner_num;
    default:
      return 0;
  }
}

static void mesh_append_copy_gvarray(const GVArray &src, const GMutableSpan dst)
{
  BLI_assert(src.size() == dst.size());
  if (src.is_empty()) {
    return;
  }
  const CPPType &type = src.type();
  const CommonVArrayInfo info = src.common_info();
  if (info.type == CommonVArrayInfo::Type::Span) {
    type.copy_construct_n(info.data, dst.data(), src.size());
    return;
  }
  if (info.type == CommonVArrayInfo::Type::Single) {
    type.fill_construct_n(info.data, dst.data(), src.size());
    return;
  }
  threading::parallel_for(src.index_range(), 1024, [&](const IndexRange range) {
    src.materialize_compressed_to_uninitialized(range, dst.slice(range).data());
  });
}

struct MeshAppendAttributeTask {
  StringRef name;
  AttrDomain domain;
  AttrType data_type;
  int dst_offset;
  int append_size;
  GSpanAttributeWriter writer;
  GVArray append_data;
};

void mesh_append(Mesh &mesh, const Mesh &append)
{
  const int base_verts = mesh.verts_num;
  const int base_edges = mesh.edges_num;
  const int base_faces = mesh.faces_num;
  const int base_corners = mesh.corners_num;

  const int append_verts = append.verts_num;
  const int append_edges = append.edges_num;
  const int append_faces = append.faces_num;
  const int append_corners = append.corners_num;

  if (append_verts == 0 && append_edges == 0 && append_faces == 0 && append_corners == 0) {
    return;
  }

  const int total_verts = base_verts + append_verts;
  const int total_edges = base_edges + append_edges;
  const int total_faces = base_faces + append_faces;
  const int total_corners = base_corners + append_corners;

  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count() * 1e-6;
  };
  Clock::time_point t0{}, t1{};

  /* Each domain's CustomData is independent — grow all four in parallel using manual OS threads.
   * threading::parallel_for cannot be used here: TBB arena_max_concurrency == 1 in the sculpt
   * stroke cleanup context makes it single-threaded. */
  if (VDM_PERF_LOG_ENABLED) {
    t0 = Clock::now();
  }
  {
    Vector<std::thread> threads;
    threads.reserve(4);
    if (append_verts > 0) {
      threads.append(std::thread(
          [&] { CustomData_realloc(&mesh.vert_data, base_verts, total_verts, CD_SET_DEFAULT); }));
    }
    if (append_edges > 0) {
      threads.append(std::thread(
          [&] { CustomData_realloc(&mesh.edge_data, base_edges, total_edges, CD_SET_DEFAULT); }));
    }
    if (append_faces > 0) {
      threads.append(std::thread(
          [&] { CustomData_realloc(&mesh.face_data, base_faces, total_faces, CD_SET_DEFAULT); }));
    }
    if (append_corners > 0) {
      threads.append(std::thread([&] {
        CustomData_realloc(&mesh.corner_data, base_corners, total_corners, CD_SET_DEFAULT);
      }));
    }
    for (std::thread &t : threads) {
      t.join();
    }
  }
  if (VDM_PERF_LOG_ENABLED) {
    t1 = Clock::now();
    printf("[APPEND] CustomData_realloc (4 domains parallel) : %.2f ms\n", ms(t0, t1));
  }

  if (VDM_PERF_LOG_ENABLED) {
    t0 = Clock::now();
  }
  if (append_faces > 0) {
    implicit_sharing::resize_trivial_array(&mesh.face_offset_indices,
                                           &mesh.runtime->face_offsets_sharing_info,
                                           base_faces == 0 ? 0 : base_faces + 1,
                                           total_faces + 1);
  }
  if (VDM_PERF_LOG_ENABLED) {
    t1 = Clock::now();
    printf("[APPEND] face_offset_indices resize              : %.2f ms\n", ms(t0, t1));
  }

  if (VDM_PERF_LOG_ENABLED) {
    t0 = Clock::now();
  }
  mesh.attribute_storage.wrap().grow_domains(
      total_verts, total_edges, total_faces, total_corners);
  if (VDM_PERF_LOG_ENABLED) {
    t1 = Clock::now();
    printf("[APPEND] grow_domains (AttributeStorage)         : %.2f ms\n", ms(t0, t1));
  }

  mesh.verts_num = total_verts;
  mesh.edges_num = total_edges;
  mesh.faces_num = total_faces;
  mesh.corners_num = total_corners;

  if (VDM_PERF_LOG_ENABLED) {
    t0 = Clock::now();
  }
  if (append_verts > 0) {
    mesh.vert_positions_for_write()
        .slice(base_verts, append_verts)
        .copy_from(append.vert_positions());
  }

  if (append_edges > 0) {
    const Span<int2> src_edges = append.edges();
    MutableSpan<int2> dst_edges = mesh.edges_for_write().slice(base_edges, append_edges);
    threading::parallel_for(src_edges.index_range(), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        const int2 edge = src_edges[i];
        dst_edges[i] = {edge[0] + base_verts, edge[1] + base_verts};
      }
    });
  }

  if (append_faces > 0) {
    const Span<int> src_face_offsets = append.face_offsets();
    MutableSpan<int> dst_face_offsets = mesh.face_offsets_for_write();
    threading::parallel_for(IndexRange(1, append_faces), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        dst_face_offsets[base_faces + i] = src_face_offsets[i] + base_corners;
      }
    });
  }

  if (append_corners > 0) {
    const Span<int> src_corner_verts = append.corner_verts();
    MutableSpan<int> dst_corner_verts = mesh.corner_verts_for_write().slice(base_corners,
                                                                            append_corners);
    threading::parallel_for(src_corner_verts.index_range(), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        dst_corner_verts[i] = src_corner_verts[i] + base_verts;
      }
    });

    const Span<int> src_corner_edges = append.corner_edges();
    MutableSpan<int> dst_corner_edges = mesh.corner_edges_for_write().slice(base_corners,
                                                                            append_corners);
    threading::parallel_for(src_corner_edges.index_range(), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        dst_corner_edges[i] = src_corner_edges[i] + base_edges;
      }
    });

    mesh.face_offsets_for_write().last() = total_corners;
  }
  if (VDM_PERF_LOG_ENABLED) {
    t1 = Clock::now();
    printf("[APPEND] topology data copy (stamp->active)      : %.2f ms\n", ms(t0, t1));
  }

  if (VDM_PERF_LOG_ENABLED) {
    t0 = Clock::now();
  }
  const AttributeAccessor base_attrs = mesh.attributes();
  const AttributeAccessor append_attrs = append.attributes();
  MutableAttributeAccessor dst_attrs = mesh.attributes_for_write();

  Vector<MeshAppendAttributeTask> attribute_tasks;
  base_attrs.foreach_attribute([&](const AttributeIter &iter) {
    if (iter.is_stopped()) {
      return;
    }
    if (iter.data_type == AttrType::String) {
      return;
    }
    if (mesh_append_attribute_is_topology_builtin(iter.name)) {
      return;
    }

    const int append_size = int(mesh_append_domain_size(iter.domain,
                                                        append_verts,
                                                        append_edges,
                                                        append_faces,
                                                        append_corners));
    if (append_size == 0) {
      return;
    }

    attribute_tasks.append({});
    MeshAppendAttributeTask &task = attribute_tasks.last();
    task.name = iter.name;
    task.domain = iter.domain;
    task.data_type = iter.data_type;
    task.dst_offset = int(mesh_append_domain_size(
        iter.domain, base_verts, base_edges, base_faces, base_corners));
    task.append_size = append_size;
    task.writer = dst_attrs.lookup_for_write_span(iter.name);
    if (!task.writer) {
      attribute_tasks.pop_last();
      return;
    }

    if (append_attrs.domain_size(iter.domain) > 0) {
      task.append_data = *append_attrs.lookup_or_default(
          iter.name, iter.domain, iter.data_type, nullptr);
    }
  });

  append_attrs.foreach_attribute([&](const AttributeIter &iter) {
    if (iter.is_stopped()) {
      return;
    }
    if (iter.data_type == AttrType::String) {
      return;
    }
    if (mesh_append_attribute_is_topology_builtin(iter.name)) {
      return;
    }
    if (base_attrs.contains(iter.name)) {
      return;
    }

    const int append_size = int(mesh_append_domain_size(iter.domain,
                                                        append_verts,
                                                        append_edges,
                                                        append_faces,
                                                        append_corners));
    if (append_size == 0) {
      return;
    }

    attribute_tasks.append({});
    MeshAppendAttributeTask &task = attribute_tasks.last();
    task.name = iter.name;
    task.domain = iter.domain;
    task.data_type = iter.data_type;
    task.dst_offset = int(mesh_append_domain_size(
        iter.domain, base_verts, base_edges, base_faces, base_corners));
    task.append_size = append_size;
    task.append_data = *append_attrs.lookup_or_default(
        iter.name, iter.domain, iter.data_type, nullptr);
    task.writer = dst_attrs.lookup_or_add_for_write_only_span(
        iter.name, iter.domain, iter.data_type);
    if (!task.writer) {
      attribute_tasks.pop_last();
    }
  });
  if (VDM_PERF_LOG_ENABLED) {
    t1 = Clock::now();
    printf("[APPEND] attribute_tasks build (%d tasks)          : %.2f ms\n",
           int(attribute_tasks.size()),
           ms(t0, t1));
  }

  if (VDM_PERF_LOG_ENABLED) {
    t0 = Clock::now();
  }
  threading::parallel_for(attribute_tasks.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      MeshAppendAttributeTask &task = attribute_tasks[i];
      if (task.append_data && task.append_size > 0) {
        mesh_append_copy_gvarray(task.append_data,
                                 task.writer.span.slice(task.dst_offset, task.append_size));
      }
    }
  });

  for (MeshAppendAttributeTask &task : attribute_tasks) {
    task.writer.finish();
  }
  if (VDM_PERF_LOG_ENABLED) {
    t1 = Clock::now();
    printf("[APPEND] attribute_tasks copy + finish             : %.2f ms\n", ms(t0, t1));
    fflush(stdout);
  }

  BKE_mesh_runtime_clear_cache(&mesh);
  mesh.tag_positions_changed();
  mesh.tag_topology_changed();
}

}  // namespace blender::bke
