/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "DNA_meshdata_types.h"

#include "extract_mesh.hh"

#include "draw_cache_impl.hh"

#include "draw_subdivision.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_paint.hh"

namespace blender::draw {

static void mesh_render_data_edge_flag(const MeshRenderData &mr,
                                       const BMEdge *eed,
                                       EditLoopData &eattr)
{
  const ToolSettings *ts = mr.toolsettings;
  const bool is_vertex_select_mode = (ts != nullptr) && (ts->selectmode & SCE_SELECT_VERTEX) != 0;
  const bool is_face_only_select_mode = (ts != nullptr) && (ts->selectmode == SCE_SELECT_FACE);

  if (eed == mr.eed_act) {
    eattr.e_flag |= VFLAG_EDGE_ACTIVE;
  }
  if (!is_vertex_select_mode && BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
    eattr.e_flag |= VFLAG_EDGE_SELECTED;
  }
  if (is_vertex_select_mode && BM_elem_flag_test(eed->v1, BM_ELEM_SELECT) &&
      BM_elem_flag_test(eed->v2, BM_ELEM_SELECT))
  {
    eattr.e_flag |= VFLAG_EDGE_SELECTED;
    eattr.e_flag |= VFLAG_VERT_SELECTED;
  }
  if (BM_elem_flag_test(eed, BM_ELEM_SEAM)) {
    eattr.e_flag |= VFLAG_EDGE_SEAM;
  }
  if (!BM_elem_flag_test(eed, BM_ELEM_SMOOTH)) {
    eattr.e_flag |= VFLAG_EDGE_SHARP;
  }

  /* Use active edge color for active face edges because
   * specular highlights make it hard to see #55456#510873.
   *
   * This isn't ideal since it can't be used when mixing edge/face modes
   * but it's still better than not being able to see the active face. */
  if (is_face_only_select_mode) {
    if (mr.efa_act != nullptr) {
      if (BM_edge_in_face(eed, mr.efa_act)) {
        eattr.e_flag |= VFLAG_EDGE_ACTIVE;
      }
    }
  }

  /* Use half a byte for value range */
  if (mr.edge_crease_ofs != -1) {
    float crease = BM_ELEM_CD_GET_FLOAT(eed, mr.edge_crease_ofs);
    if (crease > 0) {
      eattr.crease = uchar(ceil(crease * 15.0f));
    }
  }
  /* Use a byte for value range */
  if (mr.bweight_ofs != -1) {
    float bweight = BM_ELEM_CD_GET_FLOAT(eed, mr.bweight_ofs);
    if (bweight > 0) {
      eattr.bweight = uchar(bweight * 255.0f);
    }
  }
#ifdef WITH_FREESTYLE
  if (mr.freestyle_edge_ofs != -1) {
    if (BM_ELEM_CD_GET_BOOL(eed, mr.freestyle_edge_ofs)) {
      eattr.e_flag |= VFLAG_EDGE_FREESTYLE;
    }
  }
#endif
}

static void mesh_render_data_vert_flag(const MeshRenderData &mr,
                                       const BMVert *eve,
                                       EditLoopData &eattr)
{
  if (eve == mr.eve_act) {
    eattr.e_flag |= VFLAG_VERT_ACTIVE;
  }
  if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
    eattr.e_flag |= VFLAG_VERT_SELECTED;
  }
  /* Use half a byte for value range */
  if (mr.vert_crease_ofs != -1) {
    float crease = BM_ELEM_CD_GET_FLOAT(eve, mr.vert_crease_ofs);
    if (crease > 0) {
      eattr.crease |= uchar(ceil(crease * 15.0f)) << 4;
    }
  }
}

static const GPUVertFormat &get_edit_data_format()
{
  static const GPUVertFormat format = []() {
    GPUVertFormat format{};
    /* WARNING: Adjust #EditLoopData struct accordingly. */
    GPU_vertformat_attr_add(&format, "data", gpu::VertAttrType::UINT_8_8_8_8);
    GPU_vertformat_alias_add(&format, "flag");
    return format;
  }();
  return format;
}

static void extract_edit_data_mesh(const MeshRenderData &mr, MutableSpan<EditLoopData> vbo_data)
{
  MutableSpan corners_data = vbo_data.take_front(mr.corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(mr.corners_num, mr.loose_edges.size() * 2);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());

  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;
  const OffsetIndices faces = mr.faces;
  const Span<int> corner_verts = mr.corner_verts;
  const Span<int> corner_edges = mr.corner_edges;
  threading::parallel_for(faces.index_range(), 2048, [&](const IndexRange range) {
    for (const int face : range) {
      EditLoopData face_value = {};
      if (const BMFace *bm_face = bm_original_face_get(mr, face)) {
        mesh_render_data_face_flag(mr, bm_face, uv_offsets_none, face_value);
      }
      for (const int corner : faces[face]) {
        EditLoopData &value = corners_data[corner];
        value = face_value;
        if (const BMVert *bm_vert = bm_original_vert_get(mr, corner_verts[corner])) {
          mesh_render_data_vert_flag(mr, bm_vert, value);
        }
        if (const BMEdge *bm_edge = bm_original_edge_get(mr, corner_edges[corner])) {
          mesh_render_data_edge_flag(mr, bm_edge, value);
        }
      }
    }
  });

  const Span<int2> edges = mr.edges;
  mr.loose_edges.foreach_index(
      [&](const int edge_i, const int pos) {
        EditLoopData &value_1 = loose_edge_data[pos * 2 + 0];
        EditLoopData &value_2 = loose_edge_data[pos * 2 + 1];
        if (const BMEdge *bm_edge = bm_original_edge_get(mr, edge_i)) {
          value_1 = {};
          mesh_render_data_edge_flag(mr, bm_edge, value_1);
          value_2 = value_1;
        }
        else {
          value_2 = value_1 = {};
        }
        const int2 edge = edges[edge_i];
        if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[0])) {
          mesh_render_data_vert_flag(mr, bm_vert, value_1);
        }
        if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[1])) {
          mesh_render_data_vert_flag(mr, bm_vert, value_2);
        }
      },
      exec_mode::grain_size(2048));

  mr.loose_verts.foreach_index(
      [&](const int vert, const int pos) {
        loose_vert_data[pos] = {};
        if (const BMVert *eve = bm_original_vert_get(mr, vert)) {
          mesh_render_data_vert_flag(mr, eve, loose_vert_data[pos]);
        }
      },
      exec_mode::grain_size(2048));
}

static void extract_edit_data_bm(const MeshRenderData &mr, MutableSpan<EditLoopData> vbo_data)
{
  MutableSpan corners_data = vbo_data.take_front(mr.corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(mr.corners_num, mr.loose_edges.size() * 2);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());

  const BMesh &bm = *mr.bm;
  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;

  threading::parallel_for(IndexRange(bm.totface), 2048, [&](const IndexRange range) {
    for (const int face_index : range) {
      const BMFace &face = *BM_face_at_index(&const_cast<BMesh &>(bm), face_index);
      EditLoopData face_value = {};
      mesh_render_data_face_flag(mr, &face, uv_offsets_none, face_value);
      const BMLoop *loop = BM_FACE_FIRST_LOOP(&face);
      for ([[maybe_unused]] const int i : IndexRange(face.len)) {
        const int index = BM_elem_index_get(loop);
        EditLoopData &value = corners_data[index];
        value = face_value;
        mesh_render_data_edge_flag(mr, loop->e, value);
        mesh_render_data_vert_flag(mr, loop->v, value);
        loop = loop->next;
      }
    }
  });

  mr.loose_edges.foreach_index(
      [&](const int edge_i, const int pos) {
        EditLoopData &value_1 = loose_edge_data[pos * 2 + 0];
        EditLoopData &value_2 = loose_edge_data[pos * 2 + 1];
        const BMEdge &edge = *BM_edge_at_index(&const_cast<BMesh &>(bm), edge_i);
        value_1 = {};
        mesh_render_data_edge_flag(mr, &edge, value_1);
        value_2 = value_1;
        mesh_render_data_vert_flag(mr, edge.v1, value_1);
        mesh_render_data_vert_flag(mr, edge.v2, value_2);
      },
      exec_mode::grain_size(2048));

  mr.loose_verts.foreach_index(
      [&](const int vert_i, const int pos) {
        loose_vert_data[pos] = {};
        const BMVert &vert = *BM_vert_at_index(&const_cast<BMesh &>(bm), vert_i);
        mesh_render_data_vert_flag(mr, &vert, loose_vert_data[pos]);
      },
      exec_mode::grain_size(2048));
}

gpu::VertBufPtr extract_edit_data(const MeshRenderData &mr)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(get_edit_data_format()));
  const int size = mr.corners_num + mr.loose_indices_num;
  GPU_vertbuf_data_alloc(*vbo, size);
  MutableSpan vbo_data = vbo->data<EditLoopData>();
  if (mr.extract_type == MeshExtractType::Mesh) {
    extract_edit_data_mesh(mr, vbo_data);
  }
  else {
    extract_edit_data_bm(mr, vbo_data);
  }
  return vbo;
}

static void extract_edit_subdiv_data_mesh(const MeshRenderData &mr,
                                          const DRWSubdivCache &subdiv_cache,
                                          MutableSpan<EditLoopData> vbo_data)
{
  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;
  const int corners_num = subdiv_cache.num_subdiv_loops;
  const int loose_edges_num = mr.loose_edges.size();
  const int verts_per_edge = subdiv_verts_per_coarse_edge(subdiv_cache);
  const Span<int> subdiv_loop_face_index(subdiv_cache.subdiv_loop_face_index, corners_num);
  const Span<int> subdiv_loop_vert_index = subdiv_cache.verts_orig_index->data<int>();
  /* NOTE: #subdiv_loop_edge_index already has the origindex layer baked in. */
  const Span<int> subdiv_loop_edge_index = subdiv_cache.edges_orig_index->data<int>();

  MutableSpan corners_data = vbo_data.take_front(corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(corners_num, loose_edges_num * verts_per_edge);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());

  threading::parallel_for(IndexRange(subdiv_cache.num_subdiv_quads), 2048, [&](IndexRange range) {
    for (const int subdiv_quad : range) {
      const int coarse_face = subdiv_loop_face_index[subdiv_quad * 4];

      EditLoopData face_value = {};
      if (const BMFace *bm_face = bm_original_face_get(mr, coarse_face)) {
        mesh_render_data_face_flag(mr, bm_face, uv_offsets_none, face_value);
      }
      for (const int subdiv_corner : IndexRange(subdiv_quad * 4, 4)) {
        EditLoopData &value = corners_data[subdiv_corner];
        value = face_value;

        const int vert_origindex = subdiv_loop_vert_index[subdiv_corner];
        if (vert_origindex != -1) {
          if (const BMVert *bm_vert = bm_original_vert_get(mr, vert_origindex)) {
            mesh_render_data_vert_flag(mr, bm_vert, value);
          }
        }

        const int edge_origindex = subdiv_loop_edge_index[subdiv_corner];
        if (edge_origindex != -1) {
          if (const BMEdge *bm_edge = BM_edge_at_index(mr.bm, edge_origindex)) {
            mesh_render_data_edge_flag(mr, bm_edge, value);
          }
        }
      }
    }
  });

  const Span<int2> edges = mr.edges;
  mr.loose_edges.foreach_index(
      [&](const int edge_i, const int pos) {
        MutableSpan<EditLoopData> data = loose_edge_data.slice(pos * verts_per_edge,
                                                               verts_per_edge);
        if (const BMEdge *edge = bm_original_edge_get(mr, edge_i)) {
          EditLoopData value{};
          mesh_render_data_edge_flag(mr, edge, value);
          data.fill(value);
        }
        else {
          data.fill({});
        }
        const int2 edge = edges[edge_i];
        if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[0])) {
          mesh_render_data_vert_flag(mr, bm_vert, data.first());
        }
        if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[1])) {
          mesh_render_data_vert_flag(mr, bm_vert, data.last());
        }
      },
      exec_mode::grain_size(2048));

  mr.loose_verts.foreach_index(
      [&](const int vert, const int pos) {
        loose_vert_data[pos] = {};
        if (const BMVert *eve = bm_original_vert_get(mr, vert)) {
          mesh_render_data_vert_flag(mr, eve, loose_vert_data[pos]);
        }
      },
      exec_mode::grain_size(2048));
}

static void extract_edit_subdiv_data_bm(const MeshRenderData &mr,
                                        const DRWSubdivCache &subdiv_cache,
                                        MutableSpan<EditLoopData> vbo_data)
{
  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;
  const int corners_num = subdiv_cache.num_subdiv_loops;
  const int loose_edges_num = mr.loose_edges.size();
  const int verts_per_edge = subdiv_verts_per_coarse_edge(subdiv_cache);
  const Span<int> subdiv_loop_face_index(subdiv_cache.subdiv_loop_face_index, corners_num);
  const Span<int> subdiv_loop_vert_index = subdiv_cache.verts_orig_index->data<int>();
  const Span<int> subdiv_loop_edge_index = subdiv_cache.edges_orig_index->data<int>();

  MutableSpan corners_data = vbo_data.take_front(corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(corners_num, loose_edges_num * verts_per_edge);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());

  BMesh &bm = *mr.bm;
  threading::parallel_for(IndexRange(subdiv_cache.num_subdiv_quads), 2048, [&](IndexRange range) {
    for (const int subdiv_quad : range) {
      const int coarse_face = subdiv_loop_face_index[subdiv_quad * 4];
      const BMFace *bm_face = BM_face_at_index(&bm, coarse_face);

      EditLoopData face_value = {};
      mesh_render_data_face_flag(mr, bm_face, uv_offsets_none, face_value);

      for (const int subdiv_corner : IndexRange(subdiv_quad * 4, 4)) {
        EditLoopData &value = corners_data[subdiv_corner];
        value = face_value;

        const int vert_origindex = subdiv_loop_vert_index[subdiv_corner];
        if (vert_origindex != -1) {
          const BMVert *bm_vert = BM_vert_at_index(mr.bm, vert_origindex);
          mesh_render_data_vert_flag(mr, bm_vert, value);
        }

        const int edge_origindex = subdiv_loop_edge_index[subdiv_corner];
        if (edge_origindex != -1) {
          const BMEdge *bm_edge = BM_edge_at_index(mr.bm, edge_origindex);
          mesh_render_data_edge_flag(mr, bm_edge, value);
        }
      }
    }
  });

  mr.loose_edges.foreach_index(
      [&](const int edge_i, const int pos) {
        MutableSpan<EditLoopData> data = loose_edge_data.slice(pos * verts_per_edge,
                                                               verts_per_edge);
        const BMEdge *edge = BM_edge_at_index(&bm, edge_i);
        EditLoopData value{};
        mesh_render_data_edge_flag(mr, edge, value);
        data.fill(value);
        mesh_render_data_vert_flag(mr, edge->v1, data.first());
        mesh_render_data_vert_flag(mr, edge->v2, data.last());
      },
      exec_mode::grain_size(2048));

  mr.loose_verts.foreach_index(
      [&](const int vert_i, const int pos) {
        loose_vert_data[pos] = {};
        const BMVert *vert = BM_vert_at_index(&bm, vert_i);
        mesh_render_data_vert_flag(mr, vert, loose_vert_data[pos]);
      },
      exec_mode::grain_size(2048));
}

gpu::VertBufPtr extract_edit_data_subdiv(const MeshRenderData &mr,
                                         const DRWSubdivCache &subdiv_cache)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(get_edit_data_format()));
  const int size = subdiv_full_vbo_size(mr, subdiv_cache);
  GPU_vertbuf_data_alloc(*vbo, size);
  MutableSpan vbo_data = vbo->data<EditLoopData>();
  if (mr.extract_type == MeshExtractType::Mesh) {
    extract_edit_subdiv_data_mesh(mr, subdiv_cache, vbo_data);
  }
  else {
    extract_edit_subdiv_data_bm(mr, subdiv_cache, vbo_data);
  }
  return vbo;
}

/* ---------------------------------------------------------------------- */
/** \name Face Set color extraction (separate VBO to avoid bloating EditLoopData)
 * \{ */

static const GPUVertFormat &get_edit_face_set_format()
{
  static const GPUVertFormat format = []() {
    GPUVertFormat format{};
    GPU_vertformat_attr_add(&format, "face_set_color_in", gpu::VertAttrType::UNORM_8_8_8_8);
    return format;
  }();
  return format;
}

/* The overlay color is computed here on the CPU (single source of truth with sculpt mode, see
 * #BKE_paint_face_set_overlay_color_get) instead of being duplicated in the shader. The default
 * face set is left transparent; the alpha channel doubles as a flag (0 = default, 255 = explicit
 * face set) so the shader can special-case the default without needing the seed or default id. */
static uchar4 face_set_color_get(const int face_set_id,
                                 const int face_set_default,
                                 const int face_set_seed)
{
  if (face_set_id == face_set_default) {
    return uchar4(0);
  }
  uchar4 color(0);
  BKE_paint_face_set_overlay_color_get(face_set_id, face_set_seed, color);
  color[3] = 255;
  return color;
}

gpu::VertBufPtr extract_edit_face_set(const MeshRenderData &mr)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(
      GPU_vertbuf_create_with_format(get_edit_face_set_format()));
  const int size = mr.corners_num + mr.loose_indices_num;
  GPU_vertbuf_data_alloc(*vbo, size);
  MutableSpan<uchar4> data = vbo->data<uchar4>();

  const uchar4 default_color(0);
  const int face_set_default = mr.mesh->face_sets_color_default;
  const int face_set_seed = mr.mesh->face_sets_color_seed;

  if (mr.extract_type == MeshExtractType::Mesh) {
    const bke::AttributeAccessor attributes = mr.mesh->attributes();
    const VArraySpan<int> face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                              bke::AttrDomain::Face);
    if (face_sets.is_empty()) {
      data.fill(default_color);
    }
    else {
      const OffsetIndices faces = mr.faces;
      threading::parallel_for(faces.index_range(), 2048, [&](const IndexRange range) {
        for (const int face : range) {
          const uchar4 color = face_set_color_get(face_sets[face], face_set_default, face_set_seed);
          for (const int corner : faces[face]) {
            data[corner] = color;
          }
        }
      });
      /* Loose elements default. */
      data.slice(mr.corners_num, mr.loose_indices_num).fill(default_color);
    }
  }
  else {
    const BMesh &bm = *mr.bm;
    const int face_set_offset = CustomData_get_offset_named(
        &bm.pdata, CD_PROP_INT32, ".sculpt_face_set");
    if (face_set_offset == -1) {
      data.fill(default_color);
    }
    else {
      threading::parallel_for(IndexRange(bm.totface), 2048, [&](const IndexRange range) {
        for (const int face_index : range) {
          const BMFace &face = *BM_face_at_index(&const_cast<BMesh &>(bm), face_index);
          const int face_set_id = BM_ELEM_CD_GET_INT(&face, face_set_offset);
          const uchar4 color = face_set_color_get(face_set_id, face_set_default, face_set_seed);
          const BMLoop *loop = BM_FACE_FIRST_LOOP(&face);
          for ([[maybe_unused]] const int i : IndexRange(face.len)) {
            const int index = BM_elem_index_get(loop);
            data[index] = color;
            loop = loop->next;
          }
        }
      });
      data.slice(mr.corners_num, mr.loose_indices_num).fill(default_color);
    }
  }

  return vbo;
}

gpu::VertBufPtr extract_edit_face_set_subdiv(const MeshRenderData &mr,
                                             const DRWSubdivCache &subdiv_cache)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(
      GPU_vertbuf_create_with_format(get_edit_face_set_format()));
  const int size = subdiv_full_vbo_size(mr, subdiv_cache);
  GPU_vertbuf_data_alloc(*vbo, size);
  MutableSpan<uchar4> data = vbo->data<uchar4>();

  const uchar4 default_color(0);
  const int face_set_default = mr.mesh->face_sets_color_default;
  const int face_set_seed = mr.mesh->face_sets_color_seed;
  const Span<int> subdiv_loop_face_index(subdiv_cache.subdiv_loop_face_index,
                                         subdiv_cache.num_subdiv_loops);

  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const VArraySpan<int> face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                            bke::AttrDomain::Face);
  if (face_sets.is_empty()) {
    data.fill(default_color);
  }
  else {
    threading::parallel_for(
        IndexRange(subdiv_cache.num_subdiv_loops), 4096, [&](const IndexRange range) {
          for (const int i : range) {
            const int face_index = subdiv_loop_face_index[i];
            data[i] = face_set_color_get(face_sets[face_index], face_set_default, face_set_seed);
          }
        });
    /* Loose portion defaults. */
    data.slice(subdiv_cache.num_subdiv_loops, size - subdiv_cache.num_subdiv_loops)
        .fill(default_color);
  }

  return vbo;
}

}  // namespace blender::draw
