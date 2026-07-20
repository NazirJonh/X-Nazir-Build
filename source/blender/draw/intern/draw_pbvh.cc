/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * bke::pbvh::Tree drawing.
 * Embeds GPU meshes inside of bke::pbvh::Tree nodes, used by mesh sculpt mode.
 */

#include <chrono>
#include <cstdio>

#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

/* Toggleable aggregate timing probe for the per-redraw node buffer extraction. */
#define PBVH_DRAW_DEBUG_PERF 0
#if PBVH_DRAW_DEBUG_PERF
#  define PDP_PERF(...) printf(__VA_ARGS__)
#else
#  define PDP_PERF(...) ((void)0)
#endif

#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_batch.hh"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_vertex_buffer.hh"

#include "DRW_engine.hh"
#include "DRW_pbvh.hh"
#include "DRW_render.hh"

#include "attribute_convert.hh"
#include "bmesh.hh"

namespace blender {

template<> struct DefaultHash<draw::pbvh::AttributeRequest> {
  uint64_t operator()(const draw::pbvh::AttributeRequest &value) const
  {
    using namespace draw::pbvh;
    if (const CustomRequest *request_type = std::get_if<CustomRequest>(&value)) {
      return get_default_hash(*request_type);
    }
    const GenericRequest &attr = std::get<GenericRequest>(value);
    return get_default_hash(attr);
  }
};

namespace draw::pbvh {

uint64_t ViewportRequest::hash() const
{
  return get_default_hash(attributes, use_coarse_grids);
}

/**
 * Because many sculpt mode operations skip tagging dependency graph for reevaluation for
 * performance reasons, the relevant data must be retrieved directly from the original mesh rather
 * than the evaluated copy.
 */
struct OrigMeshData {
  StringRef active_color;
  StringRef default_color;
  StringRef active_uv_map;
  StringRef default_uv_map;
  int face_set_default;
  int face_set_seed;
  bke::AttributeAccessor attributes;
  /* Kept alongside the #attributes accessor so the sculpt-layer mask fillers can read the tree
   * (DNA, lives on the mesh) without re-deriving the original mesh each call. Null only when the
   * PBVH is not mesh-backed, which is exactly the case the fillers refuse anyway. */
  const Mesh *mesh = nullptr;
  OrigMeshData(const Mesh &mesh)
      : active_color(mesh.active_color_attribute),
        default_color(mesh.default_color_attribute),
        active_uv_map(mesh.active_uv_map_name()),
        default_uv_map(mesh.default_uv_map_name()),
        face_set_default(mesh.face_sets_color_default),
        face_set_seed(mesh.face_sets_color_seed),
        attributes(mesh.attributes()),
        mesh(&mesh)
  {
  }
};

/**
 * Stores the data necessary to draw the PBVH geometry. A separate `*Impl` class is used to hide
 * implementation details from the public header.
 */
class DrawCacheImpl : public DrawCache {
  struct AttributeData {
    /** A vertex buffer for each BVH node. If null, the draw data for the node must be created. */
    Vector<gpu::VertBufPtr> vbos;
    /**
     * A separate "dirty" bit per node. We track the dirty value separately from deleting the VBO
     * for a node in order to avoid recreating batches with new VBOs. It's also a necessary
     * addition to the flags stored in the PBVH which are cleared after it's used for drawing
     * (those aren't sufficient when multiple viewports are drawing with the same PBVH but request
     * different sets of attributes).
     */
    BitVector<> dirty_nodes;
    /**
     * Mark attribute values dirty for specific nodes. The next time the attribute is requested,
     * the values will be extracted again.
     */
    void tag_dirty(const IndexMask &node_mask);
  };

  /** Used to determine whether to use indexed VBO layouts for multires grids. */
  BitVector<> use_flat_layout_;
  /** The material index for each node. */
  Array<int> material_indices_;

  /** Index buffers for wireframe geometry for each node. */
  Vector<gpu::IndexBufPtr> lines_ibos_;
  /** Index buffers for coarse "fast navigate" wireframe geometry for each node. */
  Vector<gpu::IndexBufPtr> lines_ibos_coarse_;
  /** Index buffers for triangles for each node, only used for grids. */
  Vector<gpu::IndexBufPtr> tris_ibos_;
  /** Index buffers for coarse "fast navigate" triangles for each node, only used for grids. */
  Vector<gpu::IndexBufPtr> tris_ibos_coarse_;
  /**
   * GPU data and per-node dirty status for all requested attributes.
   * \note Currently we do not remove "stale" attributes that haven't been requested in a while.
   */
  Map<AttributeRequest, AttributeData> attribute_vbos_;

  /** Batches for drawing wireframe geometry. */
  Vector<gpu::Batch *> lines_batches_;
  /** Batches for drawing coarse "fast navigate" wireframe geometry. */
  Vector<gpu::Batch *> lines_batches_coarse_;
  /**
   * Batches for drawing triangles, stored separately for each combination of attributes and
   * coarse-ness. Different viewports might request different sets of attributes, and we don't want
   * to recreate the batches on every redraw.
   */
  Map<ViewportRequest, Vector<gpu::Batch *>> tris_batches_;

  /**
   * Which nodes (might) have a different number of visible faces.
   *
   * \note Theoretically the dirty tag is redundant with checking for a different number of visible
   * triangles in the PBVH node on every redraw. We could do that too, but it's simpler overall to
   * just tag the node whenever there is such a topology change, and for now there is no real
   * downside.
   */
  BitVector<> dirty_topology_;

  /* Interactive sculpt-layer influence drag (see #begin_influence_drag). The operator only flips
   * these flags + accumulates the pending influence delta; the GPU work runs at draw time in
   * #ensure_influence_drag where a GPU context is bound. */
  bool influence_drag_active_ = false;
  bool influence_drag_rebuild_delta_ = false;
  int influence_drag_layer_uid_ = -1;
  float influence_drag_pending_scale_ = 0.0f;
  /** Corner-expanded active-layer delta, one VBO per node, parallel to the position VBOs. */
  Vector<gpu::VertBufPtr> influence_drag_delta_vbos_;
  /** Lazily created compute shader for #ensure_influence_drag. */
  gpu::Shader *influence_drag_shader_ = nullptr;

 public:
  ~DrawCacheImpl() override;

  void tag_positions_changed(const IndexMask &node_mask) override;
  void tag_positions_changed_no_normals(const IndexMask &node_mask) override;
  void tag_normals_changed(const IndexMask &node_mask) override;
  void tag_visibility_changed(const IndexMask &node_mask) override;
  void tag_topology_changed(const IndexMask &node_mask) override;
  void tag_face_sets_changed(const IndexMask &node_mask) override;
  void tag_masks_changed(const IndexMask &node_mask) override;
  void tag_layer_masks_changed(const IndexMask &node_mask) override;
  void tag_attribute_changed(const IndexMask &node_mask, StringRef attribute_name) override;

  bool begin_influence_drag(int layer_uid) override;
  void add_influence_drag_delta(float scale) override;
  void end_influence_drag() override;

  Span<gpu::Batch *> ensure_tris_batches(const Object &object,
                                         const ViewportRequest &request,
                                         const IndexMask &nodes_to_update) override;

  Span<gpu::Batch *> ensure_lines_batches(const Object &object,
                                          const ViewportRequest &request,
                                          const IndexMask &nodes_to_update) override;

  Span<int> ensure_material_indices(const Object &object) override;

 private:
  /**
   * Free all GPU data for nodes with a changed visible triangle count. The next time the data is
   * requested it will be rebuilt.
   */
  void free_nodes_with_changed_topology(const bke::pbvh::Tree &pbvh);

  BitSpan ensure_use_flat_layout(const Object &object, const OrigMeshData &orig_mesh_data);

  /**
   * GPU side of the interactive sculpt-layer influence drag: builds the active layer's corner
   * expanded delta buffers on the first call of a drag, then on each tick adds
   * `position += delta * pending_scale` into the node position buffers with a compute shader,
   * avoiding the CPU re-extract + re-upload. No-op outside a drag.
   */
  void ensure_influence_drag(const Object &object, const IndexMask &node_mask);

  Span<gpu::VertBufPtr> ensure_attribute_data(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const AttributeRequest &attr,
                                              const IndexMask &node_mask);

  Span<gpu::IndexBufPtr> ensure_tri_indices(const Object &object,
                                            const OrigMeshData &orig_mesh_data,
                                            const IndexMask &node_mask,
                                            bool coarse);

  Span<gpu::IndexBufPtr> ensure_lines_indices(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const IndexMask &node_mask,
                                              bool coarse);
};

void DrawCacheImpl::AttributeData::tag_dirty(const IndexMask &node_mask)
{
  this->dirty_nodes.resize(std::max(this->dirty_nodes.size(), node_mask.min_array_size()), false);
  node_mask.set_bits(this->dirty_nodes);
}

void DrawCacheImpl::tag_positions_changed(const IndexMask &node_mask)
{
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Position)) {
    data->tag_dirty(node_mask);
  }
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Normal)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_positions_changed_no_normals(const IndexMask &node_mask)
{
  /* Position buffers only; the Normal buffers are intentionally left clean so they are not
   * re-extracted on every interactive-drag tick (see #Tree::tag_positions_changed_no_normals). */
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Position)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_normals_changed(const IndexMask &node_mask)
{
  /* Normal buffers only; the influence drag keeps the position buffers on the GPU, so they are
   * intentionally left clean here (see #Tree::tag_normals_changed). */
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Normal)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_visibility_changed(const IndexMask &node_mask)
{
  dirty_topology_.resize(std::max(dirty_topology_.size(), node_mask.min_array_size()), false);
  node_mask.set_bits(dirty_topology_);
}

void DrawCacheImpl::tag_topology_changed(const IndexMask &node_mask)
{
  /** Currently the only times where topology changes are for BMesh dynamic topology, where tagging
   * a visibility update deletes all the GPU data anyway. */
  this->tag_visibility_changed(node_mask);
}

void DrawCacheImpl::tag_face_sets_changed(const IndexMask &node_mask)
{
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::FaceSet)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_masks_changed(const IndexMask &node_mask)
{
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Mask)) {
    data->tag_dirty(node_mask);
  }
  /* The sculpt mask buffer feeds either attribute depending on whether a layer mask editing
   * session is open, and this cache has no way to ask. Tagging both is always correct and merely
   * redundant: when the layer overlay is off the lookup misses and the second tag costs nothing. */
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::LayerMask)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_layer_masks_changed(const IndexMask &node_mask)
{
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::LayerMask)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_attribute_changed(const IndexMask &node_mask, StringRef attribute_name)
{
  for (const auto &[data_request, data] : attribute_vbos_.items()) {
    if (const GenericRequest *request = std::get_if<GenericRequest>(&data_request)) {
      if (*request == attribute_name) {
        data.tag_dirty(node_mask);
      }
    }
  }
}

DrawCache &ensure_draw_data(std::unique_ptr<bke::pbvh::DrawCache> &ptr)
{
  if (!ptr) {
    ptr = std::make_unique<DrawCacheImpl>();
  }
  return dynamic_cast<DrawCache &>(*ptr);
}

BLI_NOINLINE static void free_ibos(const MutableSpan<gpu::IndexBufPtr> ibos,
                                   const IndexMask &node_mask)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_intersection(node_mask, ibos.index_range(), memory);
  mask.foreach_index([&](const int i) { ibos[i].reset(); });
}

BLI_NOINLINE static void free_vbos(const MutableSpan<gpu::VertBufPtr> vbos,
                                   const IndexMask &node_mask)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_intersection(node_mask, vbos.index_range(), memory);
  mask.foreach_index([&](const int i) { vbos[i].reset(); });
}

BLI_NOINLINE static void free_batches(const MutableSpan<gpu::Batch *> batches,
                                      const IndexMask &node_mask)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_intersection(node_mask, batches.index_range(), memory);
  mask.foreach_index([&](const int i) { GPU_BATCH_DISCARD_SAFE(batches[i]); });
}

static const GPUVertFormat &position_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  return format;
}

static const GPUVertFormat &normal_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "nor", gpu::VertAttrType::SNORM_16_16_16_16);
  return format;
}

static const GPUVertFormat &mask_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute("msk",
                                                                    gpu::VertAttrType::SFLOAT_32);
  return format;
}

static const GPUVertFormat &layer_mask_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute("layer_weight",
                                                                    gpu::VertAttrType::SFLOAT_32);
  return format;
}

static const GPUVertFormat &face_set_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "fset", gpu::VertAttrType::UNORM_8_8_8_8);
  return format;
}

static GPUVertFormat attribute_format(const OrigMeshData &orig_mesh_data,
                                      const StringRef name,
                                      const bke::AttrType data_type)
{
  GPUVertFormat format = init_format_for_attribute(data_type, "data");

  bool is_render, is_active;
  const char *prefix = "a";

  if (CD_TYPE_AS_MASK(*bke::attr_type_to_custom_data_type(data_type))) {
    prefix = "c";
    is_active = orig_mesh_data.active_color == name;
    is_render = orig_mesh_data.default_color == name;
  }
  if (data_type == bke::AttrType::Float2) {
    prefix = "u";
    is_active = orig_mesh_data.active_uv_map == name;
    is_render = orig_mesh_data.default_uv_map == name;
  }

  DRW_cdlayer_attr_aliases_add(&format, prefix, data_type, name, is_render, is_active);
  return format;
}

inline short4 normal_float_to_short(const float3 &value)
{
  short3 result;
  normal_float_to_short_v3(result, value);
  return short4(result.x, result.y, result.z, 0);
}

template<typename T>
void extract_data_vert_mesh(const OffsetIndices<int> faces,
                            const Span<int> corner_verts,
                            const Span<T> attribute,
                            const Span<int> face_indices,
                            gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();
  for (const int face : face_indices) {
    for (const int vert : corner_verts.slice(faces[face])) {
      *data = Converter::convert(attribute[vert]);
      data++;
    }
  }
}

template<typename T>
void extract_data_face_mesh(const OffsetIndices<int> faces,
                            const Span<T> attribute,
                            const Span<int> face_indices,
                            gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  VBOType *data = vbo.data<VBOType>().data();
  for (const int face : face_indices) {
    const int face_size = faces[face].size();
    std::fill_n(data, face_size, Converter::convert(attribute[face]));
    data += face_size;
  }
}

template<typename T>
void extract_data_corner_mesh(const OffsetIndices<int> faces,
                              const Span<T> attribute,
                              const Span<int> face_indices,
                              gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  VBOType *data = vbo.data<VBOType>().data();
  for (const int face : face_indices) {
    for (const int corner : faces[face]) {
      *data = Converter::convert(attribute[corner]);
      data++;
    }
  }
}

template<typename T> const T &bmesh_cd_vert_get(const BMVert &vert, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(vert.head.data, offset));
}

template<typename T> const T &bmesh_cd_loop_get(const BMLoop &loop, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(loop.head.data, offset));
}

template<typename T> const T &bmesh_cd_face_get(const BMFace &face, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(face.head.data, offset));
}

template<typename T>
void extract_data_vert_bmesh(const Set<BMFace *, 0> &faces, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    const BMLoop *l = face->l_first;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->prev->v, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->v, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->next->v, cd_offset));
    data++;
  }
}

template<typename T>
void extract_data_face_bmesh(const Set<BMFace *, 0> &faces, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    std::fill_n(data, 3, Converter::convert(bmesh_cd_face_get<T>(*face, cd_offset)));
    data += 3;
  }
}

template<typename T>
void extract_data_corner_bmesh(const Set<BMFace *, 0> &faces,
                               const int cd_offset,
                               gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    const BMLoop *l = face->l_first;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l->prev, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l->next, cd_offset));
    data++;
  }
}

static int count_visible_tris_bmesh(const Set<BMFace *, 0> &faces)
{
  return std::count_if(faces.begin(), faces.end(), [&](const BMFace *face) {
    return !BM_elem_flag_test_bool(face, BM_ELEM_HIDDEN);
  });
}

DrawCacheImpl::~DrawCacheImpl()
{
  free_batches(lines_batches_, lines_batches_.index_range());
  free_batches(lines_batches_coarse_, lines_batches_coarse_.index_range());
  for (MutableSpan<gpu::Batch *> batches : tris_batches_.values()) {
    free_batches(batches, batches.index_range());
  }
  if (influence_drag_shader_) {
    GPU_shader_free(influence_drag_shader_);
  }
}

void DrawCacheImpl::free_nodes_with_changed_topology(const bke::pbvh::Tree &pbvh)
{
  /* NOTE: Theoretically we shouldn't need to free batches with a changed triangle count, but
   * currently it's the simplest way to reallocate all the GPU data while keeping everything in a
   * consistent state. */
  IndexMaskMemory memory;
  const IndexMask nodes_to_free = IndexMask::from_bits(dirty_topology_, memory);
  if (nodes_to_free.is_empty()) {
    return;
  }

  dirty_topology_.clear_and_shrink();

  free_ibos(lines_ibos_, nodes_to_free);
  free_ibos(lines_ibos_coarse_, nodes_to_free);
  free_ibos(tris_ibos_, nodes_to_free);
  free_ibos(tris_ibos_coarse_, nodes_to_free);
  if (pbvh.type() == bke::pbvh::Type::BMesh) {
    /* For BMesh, VBOs are only filled with data for visible triangles, and topology can also
     * completely change due to dynamic topology, so VBOs must be rebuilt from scratch. For other
     * types, actual topology doesn't change, and visibility changes are accounted for by the index
     * buffers. */
    for (AttributeData &data : attribute_vbos_.values()) {
      free_vbos(data.vbos, nodes_to_free);
    }
  }

  free_batches(lines_batches_, nodes_to_free);
  free_batches(lines_batches_coarse_, nodes_to_free);
  for (MutableSpan<gpu::Batch *> batches : tris_batches_.values()) {
    free_batches(batches, nodes_to_free);
  }
}

BLI_NOINLINE static void ensure_vbos_allocated_mesh(const Object &object,
                                                    const GPUVertFormat &format,
                                                    const IndexMask &node_mask,
                                                    const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        if (!vbos[i]) {
          vbos[i] = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
        }
        GPU_vertbuf_data_alloc(*vbos[i], nodes[i].corners_num());
      },
      exec_mode::grain_size(64));
}

BLI_NOINLINE static void ensure_vbos_allocated_grids(const Object &object,
                                                     const GPUVertFormat &format,
                                                     const BitSpan use_flat_layout,
                                                     const IndexMask &node_mask,
                                                     const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  node_mask.foreach_index(
      [&](const int i) {
        if (!vbos[i]) {
          vbos[i] = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
        }
        const int verts_per_grid = use_flat_layout[i] ? square_i(subdiv_ccg.grid_size - 1) * 4 :
                                                        square_i(subdiv_ccg.grid_size);
        const int verts_num = nodes[i].grids().size() * verts_per_grid;
        GPU_vertbuf_data_alloc(*vbos[i], verts_num);
      },
      exec_mode::grain_size(64));
}

BLI_NOINLINE static void ensure_vbos_allocated_bmesh(const Object &object,
                                                     const GPUVertFormat &format,
                                                     const IndexMask &node_mask,
                                                     const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        if (!vbos[i]) {
          vbos[i] = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
        }
        const Set<BMFace *, 0> &faces = BKE_pbvh_bmesh_node_faces(
            &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
        const int verts_num = count_visible_tris_bmesh(faces) * 3;
        GPU_vertbuf_data_alloc(*vbos[i], verts_num);
      },
      exec_mode::grain_size(64));
}

static void update_positions_mesh(const Object &object,
                                  const IndexMask &node_mask,
                                  MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval_from_eval(object);
  ensure_vbos_allocated_mesh(object, position_format(), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        extract_data_vert_mesh<float3>(
            faces, corner_verts, vert_positions, nodes[i].faces(), *vbos[i]);
      },
      exec_mode::grain_size(1));
}

static void update_normals_mesh(const Object &object,
                                const IndexMask &node_mask,
                                MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval_from_eval(object);
  const Span<float3> face_normals = bke::pbvh::face_normals_eval_from_eval(object);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
  ensure_vbos_allocated_mesh(object, normal_format(), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        short4 *data = vbos[i]->data<short4>().data();

        for (const int face : nodes[i].faces()) {
          if (!sharp_faces.is_empty() && sharp_faces[face]) {
            const int face_size = faces[face].size();
            std::fill_n(data, face_size, normal_float_to_short(face_normals[face]));
            data += face_size;
          }
          else {
            for (const int vert : corner_verts.slice(faces[face])) {
              *data = normal_float_to_short(vert_normals[vert]);
              data++;
            }
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_masks_mesh(const Object &object,
                                           const OrigMeshData &orig_mesh_data,
                                           const IndexMask &node_mask,
                                           MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  /* While a layer mask editing session is open the live `.sculpt_mask` buffer holds the layer's
   * weights rather than the user's own mask. The user's mask is parked in the session and must
   * still be drawn: the two are independent and can cover different parts of the mesh. */
  const SculptSession *ss = object.runtime->sculpt_session;
  const bool session_live = ss != nullptr && ss->layers.mask_edit.node_uid != 0 &&
                            ss->layers.mask_edit.suspend_depth == 0;

  /* Held at function scope rather than inside the branch below: #VArraySpan owns a materialized
   * buffer whenever the attribute is not a plain span, so a #Span pointing into it must not outlive
   * it. Not looked up at all during a session, where the live buffer holds the wrong data. */
  const VArraySpan<float> live = session_live ?
                                     VArraySpan<float>() :
                                     VArraySpan<float>(*orig_mesh_data.attributes.lookup<float>(
                                         ".sculpt_mask", bke::AttrDomain::Point));

  /* A parked copy whose length no longer matches the mesh cannot be indexed by vertex; the same
   * guard gates every other reader of it (see #sculpt_layers_mask.cc). Missing either way means the
   * user has no mask, which draws as zero rather than as nothing. */
  const Span<float> parked = session_live ? ss->layers.mask_edit.saved_vert_mask.as_span() :
                                            Span<float>(live);
  const bool parked_usable = !parked.is_empty() && parked.size() == mesh.verts_num;

  ensure_vbos_allocated_mesh(object, mask_format(), node_mask, vbos);
  if (!parked_usable) {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(0.0f); },
                            exec_mode::grain_size(64));
    return;
  }
  node_mask.foreach_index(
      [&](const int i) {
        float *data = vbos[i]->data<float>().data();
        for (const int face : nodes[i].faces()) {
          for (const int vert : corner_verts.slice(faces[face])) {
            *data = parked[vert];
            data++;
          }
        }
      },
      exec_mode::grain_size(1));
}

/**
 * Resolve which layer node the overlay draws, and where its weights currently live.
 *
 * While a mask editing session is open the node's own #SculptLayerMask is stale: the weights are
 * expanded into the live sculpt mask buffer and only compressed back on session end (see task 6).
 * The session's node also need not be the active one — a folder mask can be edited while a layer
 * inside it stays active — so the session wins over the active node rather than being checked
 * against it.
 *
 * #orig_mesh_data carries the original mesh (sculpt mode skips depsgraph updates), and the session
 * is read off the evaluated object's runtime, which shares the same #SculptSession as the original
 * (the depsgraph copies the pointer verbatim during eval).
 */
static const SculptLayerTreeNode *layer_mask_node_for_draw(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           bool &r_session_live)
{
  r_session_live = false;
  if (orig_mesh_data.mesh == nullptr) {
    return nullptr;
  }
  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss != nullptr && ss->layers.mask_edit.node_uid != 0 &&
      ss->layers.mask_edit.suspend_depth == 0)
  {
    r_session_live = true;
    return bke::sculpt_layers::node_find_by_uid(*orig_mesh_data.mesh,
                                                ss->layers.mask_edit.node_uid);
  }
  const SculptLayer *active = bke::sculpt_layers::active_get(*orig_mesh_data.mesh);
  return active != nullptr ? &active->base : nullptr;
}

/**
 * The overlay deliberately re-checks what #node_mask_for_composite checks rather than calling it:
 * the composer folds parent folder masks in, while the overlay shows the weights of one node. Both
 * fail open the same way — an unusable mask means "nothing hidden", not "all hidden".
 *
 * With one deliberate disagreement: #SCULPT_LAYER_REC_EXEMPT is *not* consulted. The composer drops
 * an exempt node's mask because an armed REC must record unattenuated, but the mask itself is intact
 * and applies again the moment REC is disarmed. Hiding it would black out the overlay for exactly
 * the layer the user is working on, at exactly the moment they armed recording on it — the mask is
 * information about the layer, not about what the surface is doing this instant.
 */
static bool layer_mask_is_drawable(const SculptLayerTreeNode &node,
                                   const int64_t elem_num,
                                   const int grid_area)
{
  if (node.mask == nullptr) {
    return false;
  }
  if (!bke::sculpt_layers::mask_enabled(node)) {
    return false;
  }
  if (bke::sculpt_layers::is_stale_mask(*node.mask, elem_num)) {
    return false;
  }
  /* A grid mask is stored one block per grid; a mismatched block_size is silently dropped by the
   * composer, so the overlay agrees and renders the layer at full weight instead of reading
   * mismatched blocks. `grid_area == 0` means the vertex domain, where block_size is free. */
  if (grid_area != 0 && node.mask->block_size != grid_area) {
    return false;
  }
  return true;
}

BLI_NOINLINE static void update_layer_masks_mesh(const Object &object,
                                                 const OrigMeshData &orig_mesh_data,
                                                 const IndexMask &node_mask,
                                                 MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  /* The same mesh the neighboring fillers walk: node face indices address the drawn mesh, while
   * #OrigMeshData only supplies the per-vertex values. */
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  ensure_vbos_allocated_mesh(object, layer_mask_format(), node_mask, vbos);

  bool session_live = false;
  const SculptLayerTreeNode *node = layer_mask_node_for_draw(object, orig_mesh_data, session_live);

  /* While a session is open the live sculpt mask buffer holds the layer's expanded weights, not the
   * DNA mask on the node (which is parked and stale until #mask_edit_end compresses it back). The
   * overlay therefore reads the same source #update_masks_mesh reads outside a session.
   *
   * Held at function scope for the reason spelled out in #update_masks_mesh: #VArraySpan can own
   * the buffer it exposes. */
  const VArraySpan<float> live = session_live ?
                                     VArraySpan<float>(*orig_mesh_data.attributes.lookup<float>(
                                         ".sculpt_mask", bke::AttrDomain::Point)) :
                                     VArraySpan<float>();
  if (session_live) {
    if (live.size() != mesh.verts_num) {
      node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(1.0f); },
                              exec_mode::grain_size(64));
      return;
    }
    node_mask.foreach_index(
        [&](const int i) {
          float *data = vbos[i]->data<float>().data();
          for (const int face : nodes[i].faces()) {
            for (const int vert : corner_verts.slice(faces[face])) {
              /* Complemented back to a weight: the session buffer holds how much the layer is
               * hidden (see #session_buffer_flip), while this attribute carries the weight the
               * shader then inverts for the tint. */
              *data = 1.0f - live[vert];
              data++;
            }
          }
        },
        exec_mode::grain_size(1));
    return;
  }

  if (node == nullptr || !layer_mask_is_drawable(*node, mesh.verts_num, 0)) {
    /* A missing or undrawable node means "nothing hidden": the layer contributes fully, so the
     * overlay has no blue to paint and a neutral weight is correct. */
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(1.0f); },
                            exec_mode::grain_size(64));
    return;
  }

  const SculptLayerMask &layer_mask = *node->mask;
  node_mask.foreach_index(
      [&](const int i) {
        float *data = vbos[i]->data<float>().data();
        for (const int face : nodes[i].faces()) {
          for (const int vert : corner_verts.slice(faces[face])) {
            *data = float(bke::sculpt_layers::mask_value_at(layer_mask, vert)) / 255.0f;
            data++;
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_face_sets_mesh(const Object &object,
                                               const OrigMeshData &orig_mesh_data,
                                               const IndexMask &node_mask,
                                               MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const VArraySpan face_sets = *orig_mesh_data.attributes.lookup<int>(".sculpt_face_set",
                                                                      bke::AttrDomain::Face);
  ensure_vbos_allocated_mesh(object, face_set_format(), node_mask, vbos);
  if (!face_sets.is_empty()) {
    node_mask.foreach_index(
        [&](const int i) {
          uchar4 *data = vbos[i]->data<uchar4>().data();
          for (const int face : nodes[i].faces()) {
            const int id = face_sets[face];

            uchar4 fset_color(UCHAR_MAX);
            if (id != color_default) {
              BKE_paint_face_set_overlay_color_get(id, color_seed, fset_color);
            }
            else {
              /* Skip for the default color face set to render it white. */
              fset_color[0] = fset_color[1] = fset_color[2] = UCHAR_MAX;
            }

            const int face_size = faces[face].size();
            std::fill_n(data, face_size, fset_color);
            data += face_size;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<uchar4>().fill(uchar4(255)); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_generic_attribute_mesh(const Object &object,
                                                       const OrigMeshData &orig_mesh_data,
                                                       const IndexMask &node_mask,
                                                       const StringRef name,
                                                       MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const bke::GAttributeReader attr = attributes.lookup(name);
  if (!attr || attr.domain == bke::AttrDomain::Edge) {
    return;
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(attr.varray.type());
  ensure_vbos_allocated_mesh(
      object, attribute_format(orig_mesh_data, name, data_type), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        bke::attribute_math::to_static_type(attr.varray.type(), [&]<typename T>() {
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            const VArraySpan<T> src = attr.varray.typed<T>();
            switch (attr.domain) {
              case bke::AttrDomain::Point:
                extract_data_vert_mesh<T>(faces, corner_verts, src, nodes[i].faces(), *vbos[i]);
                break;
              case bke::AttrDomain::Face:
                extract_data_face_mesh<T>(faces, src, nodes[i].faces(), *vbos[i]);
                break;
              case bke::AttrDomain::Corner:
                extract_data_corner_mesh<T>(faces, src, nodes[i].faces(), *vbos[i]);
                break;
              default:
                BLI_assert_unreachable();
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void fill_positions_grids(const Object &object,
                                              const BitSpan use_flat_layout,
                                              const IndexMask &node_mask,
                                              const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const Span<float3> positions = subdiv_ccg.positions;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  ensure_vbos_allocated_grids(object, position_format(), use_flat_layout, node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        float3 *data = vbos[i]->data<float3>().data();
        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            for (int y = 0; y < grid_size_1; y++) {
              for (int x = 0; x < grid_size_1; x++) {
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y + 1)];
                data++;
              }
            }
          }
        }
        else {
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            std::copy_n(grid_positions.data(), grid_positions.size(), data);
            data += grid_positions.size();
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void fill_normals_grids(const Object &object,
                                            const OrigMeshData &orig_mesh_data,
                                            const BitSpan use_flat_layout,
                                            const IndexMask &node_mask,
                                            const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const Span<float3> positions = subdiv_ccg.positions;
  const Span<float3> normals = subdiv_ccg.normals;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
  ensure_vbos_allocated_grids(object, normal_format(), use_flat_layout, node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        short4 *data = vbos[i]->data<short4>().data();

        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            const Span<float3> grid_normals = normals.slice(bke::ccg::grid_range(key, grid));
            if (!sharp_faces.is_empty() && sharp_faces[grid_to_face_map[grid]]) {
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  float3 no;
                  normal_quad_v3(no,
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y + 1)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y)]);
                  std::fill_n(data, 4, normal_float_to_short(no));
                  data += 4;
                }
              }
            }
            else {
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  std::fill_n(data,
                              4,
                              normal_float_to_short(
                                  grid_normals[CCG_grid_xy_to_index(key.grid_size, x, y)]));
                  data += 4;
                }
              }
            }
          }
        }
        else {
          /* The non-flat VBO layout does not support sharp faces. */
          for (const int grid : nodes[i].grids()) {
            for (const float3 &normal : normals.slice(bke::ccg::grid_range(key, grid))) {
              *data = normal_float_to_short(normal);
              data++;
            }
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void fill_masks_grids(const Object &object,
                                          const BitSpan use_flat_layout,
                                          const IndexMask &node_mask,
                                          const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  /* While a layer mask editing session is open the live #subdiv_ccg.masks buffer holds the layer's
   * weights rather than the user's own mask. The user's mask is parked in the session and must
   * still be drawn: the two are independent and can cover different parts of the mesh. */
  const SculptSession *ss = object.runtime->sculpt_session;
  const bool session_live = ss != nullptr && ss->layers.mask_edit.node_uid != 0 &&
                            ss->layers.mask_edit.suspend_depth == 0;
  /* A parked copy whose length no longer matches the grids cannot be sliced per grid; the same
   * guard gates every other reader of it (see #sculpt_layers_mask.cc). Missing either way means the
   * user has no mask, which draws as zero rather than as nothing. */
  const Span<float> masks = session_live ? ss->layers.mask_edit.saved_grid_mask.as_span() :
                                           subdiv_ccg.masks.as_span();
  const int64_t elem_num = int64_t(subdiv_ccg.grids_num) * subdiv_ccg.grid_area;

  ensure_vbos_allocated_grids(object, mask_format(), use_flat_layout, node_mask, vbos);
  if (masks.size() != elem_num) {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(0.0f); },
                            exec_mode::grain_size(64));
    return;
  }
  node_mask.foreach_index(
      [&](const int i) {
        float *data = vbos[i]->data<float>().data();
        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const Span<float> grid_masks = masks.slice(bke::ccg::grid_range(key, grid));
            for (int y = 0; y < grid_size_1; y++) {
              for (int x = 0; x < grid_size_1; x++) {
                *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y)];
                data++;
                *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y)];
                data++;
                *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)];
                data++;
                *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y + 1)];
                data++;
              }
            }
          }
        }
        else {
          for (const int grid : nodes[i].grids()) {
            const Span<float> grid_masks = masks.slice(bke::ccg::grid_range(key, grid));
            std::copy_n(grid_masks.data(), grid_masks.size(), data);
            data += grid_masks.size();
          }
        }
      },
      exec_mode::grain_size(1));
}

/**
 * Read one grid element from a layer mask as a normalized float. The mask is stored one block per
 * grid (block_size == grid_area), so its element indexing matches #subdiv_ccg.masks one-to-one and
 * only the grid-local offset needs translating to a global element index.
 */
static float layer_weight_at(const SculptLayerMask &mask,
                             const IndexRange &grid_range,
                             const int offset_in_grid)
{
  /* The element index is narrowed explicitly: #mask_value_at takes an `int`, and the caller has
   * already established that the mask covers every grid element, so the sum fits by construction. */
  const int elem = int(grid_range.start()) + offset_in_grid;
  return float(bke::sculpt_layers::mask_value_at(mask, elem)) / 255.0f;
}

BLI_NOINLINE static void fill_layer_masks_grids(const Object &object,
                                                const OrigMeshData &orig_mesh_data,
                                                const BitSpan use_flat_layout,
                                                const IndexMask &node_mask,
                                                MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  ensure_vbos_allocated_grids(object, layer_mask_format(), use_flat_layout, node_mask, vbos);

  bool session_live = false;
  const SculptLayerTreeNode *node = layer_mask_node_for_draw(object, orig_mesh_data, session_live);
  const int64_t elem_num = int64_t(subdiv_ccg.grids_num) * subdiv_ccg.grid_area;

  /* While a session is open the live sculpt mask buffer holds the layer's expanded weights, so the
   * overlay reads #subdiv_ccg.masks — the same source #fill_masks_grids reads outside a session. */
  if (session_live) {
    const Span<float> live = subdiv_ccg.masks;
    if (live.size() != elem_num) {
      node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(1.0f); },
                              exec_mode::grain_size(64));
      return;
    }
    /* Complemented back to a weight: the session buffer holds how much the layer is hidden (see
     * #session_buffer_flip), while this attribute carries the weight the shader then inverts for
     * the tint. That complement is also why the compact layout copies element by element here,
     * where #fill_masks_grids can memcpy a whole grid. */
    node_mask.foreach_index(
        [&](const int i) {
          float *data = vbos[i]->data<float>().data();
          if (use_flat_layout[i]) {
            const int grid_size_1 = key.grid_size - 1;
            for (const int grid : nodes[i].grids()) {
              const Span<float> grid_masks = live.slice(bke::ccg::grid_range(key, grid));
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  *data = 1.0f - grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y)];
                  data++;
                  *data = 1.0f - grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y)];
                  data++;
                  *data = 1.0f - grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)];
                  data++;
                  *data = 1.0f - grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y + 1)];
                  data++;
                }
              }
            }
          }
          else {
            for (const int grid : nodes[i].grids()) {
              const Span<float> grid_masks = live.slice(bke::ccg::grid_range(key, grid));
              for (const float mask_value : grid_masks) {
                *data = 1.0f - mask_value;
                data++;
              }
            }
          }
        },
        exec_mode::grain_size(1));
    return;
  }

  if (node == nullptr || !layer_mask_is_drawable(*node, elem_num, subdiv_ccg.grid_area)) {
    /* A missing or undrawable node means "nothing hidden": the layer contributes fully, so the
     * overlay has no blue to paint and a neutral weight is correct. */
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(1.0f); },
                            exec_mode::grain_size(64));
    return;
  }

  const SculptLayerMask &layer_mask = *node->mask;
  node_mask.foreach_index(
      [&](const int i) {
        float *data = vbos[i]->data<float>().data();
        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const IndexRange range = bke::ccg::grid_range(key, grid);
            for (int y = 0; y < grid_size_1; y++) {
              for (int x = 0; x < grid_size_1; x++) {
                *data = layer_weight_at(layer_mask, range, CCG_grid_xy_to_index(key.grid_size, x, y));
                data++;
                *data = layer_weight_at(
                    layer_mask, range, CCG_grid_xy_to_index(key.grid_size, x + 1, y));
                data++;
                *data = layer_weight_at(
                    layer_mask, range, CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1));
                data++;
                *data = layer_weight_at(
                    layer_mask, range, CCG_grid_xy_to_index(key.grid_size, x, y + 1));
                data++;
              }
            }
          }
        }
        else {
          for (const int grid : nodes[i].grids()) {
            const IndexRange range = bke::ccg::grid_range(key, grid);
            for (const int offset : IndexRange(range.size())) {
              *data = layer_weight_at(layer_mask, range, offset);
              data++;
            }
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void fill_face_sets_grids(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const BitSpan use_flat_layout,
                                              const IndexMask &node_mask,
                                              const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  ensure_vbos_allocated_grids(object, face_set_format(), use_flat_layout, node_mask, vbos);
  if (const VArray<int> face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                            bke::AttrDomain::Face))
  {
    const VArraySpan<int> face_sets_span(face_sets);
    node_mask.foreach_index(
        [&](const int i) {
          const Span<int> grids = nodes[i].grids();
          const int verts_per_grid = use_flat_layout[i] ? square_i(key.grid_size - 1) * 4 :
                                                          square_i(key.grid_size);
          uchar4 *data = vbos[i]->data<uchar4>().data();
          for (const int i : grids.index_range()) {
            uchar4 color{UCHAR_MAX};
            const int fset = face_sets[grid_to_face_map[grids[i]]];
            if (fset != color_default) {
              BKE_paint_face_set_overlay_color_get(fset, color_seed, color);
            }

            std::fill_n(data, verts_per_grid, color);
            data += verts_per_grid;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<uchar4>().fill(uchar4{UCHAR_MAX}); },
                            exec_mode::grain_size(1));
  }
}

BLI_NOINLINE static void update_positions_bmesh(const Object &object,
                                                const IndexMask &node_mask,
                                                const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  ensure_vbos_allocated_bmesh(object, position_format(), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        float3 *data = vbos[i]->data<float3>().data();
        for (const BMFace *face :
             BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
        {
          if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
            continue;
          }
          const BMLoop *l = face->l_first;
          *data = l->prev->v->co;
          data++;
          *data = l->v->co;
          data++;
          *data = l->next->v->co;
          data++;
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_normals_bmesh(const Object &object,
                                              const IndexMask &node_mask,
                                              const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  ensure_vbos_allocated_bmesh(object, normal_format(), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        short4 *data = vbos[i]->data<short4>().data();
        for (const BMFace *face :
             BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
        {
          if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
            continue;
          }
          if (BM_elem_flag_test(face, BM_ELEM_SMOOTH)) {
            const BMLoop *l = face->l_first;
            *data = normal_float_to_short(l->prev->v->no);
            data++;
            *data = normal_float_to_short(l->v->no);
            data++;
            *data = normal_float_to_short(l->next->v->no);
            data++;
          }
          else {
            std::fill_n(data, 3, normal_float_to_short(face->no));
            data += 3;
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_masks_bmesh(const Object &object,
                                            const IndexMask &node_mask,
                                            const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const int cd_offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
  ensure_vbos_allocated_bmesh(object, mask_format(), node_mask, vbos);
  if (cd_offset != -1) {
    node_mask.foreach_index(
        [&](const int i) {
          float *data = vbos[i]->data<float>().data();
          for (const BMFace *face :
               BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
          {
            if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
              continue;
            }
            const BMLoop *l = face->l_first;
            *data = bmesh_cd_vert_get<float>(*l->prev->v, cd_offset);
            data++;
            *data = bmesh_cd_vert_get<float>(*l->v, cd_offset);
            data++;
            *data = bmesh_cd_vert_get<float>(*l->next->v, cd_offset);
            data++;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(0.0f); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_layer_masks_bmesh(const Object &object,
                                                  const IndexMask &node_mask,
                                                  const MutableSpan<gpu::VertBufPtr> vbos)
{
  /* Sculpt layers do not exist on the BMesh (dyntopo) domain, so the overlay has nothing to show
   * here. The attribute still has to be filled: it is part of the batch either way. */
  ensure_vbos_allocated_bmesh(object, layer_mask_format(), node_mask, vbos);
  node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(1.0f); },
                          exec_mode::grain_size(64));
}

BLI_NOINLINE static void update_face_sets_bmesh(const Object &object,
                                                const OrigMeshData &orig_mesh_data,
                                                const IndexMask &node_mask,
                                                const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const int offset = CustomData_get_offset_named(&bm.pdata, CD_PROP_INT32, ".sculpt_face_set");
  ensure_vbos_allocated_bmesh(object, face_set_format(), node_mask, vbos);
  if (offset != -1) {
    node_mask.foreach_index(
        [&](const int i) {
          uchar4 *data = vbos[i]->data<uchar4>().data();
          for (const BMFace *face :
               BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
          {
            if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
              continue;
            }
            uchar4 color{UCHAR_MAX};
            const int fset = bmesh_cd_face_get<int>(*face, offset);
            if (fset != color_default) {
              BKE_paint_face_set_overlay_color_get(fset, color_seed, color);
            }
            std::fill_n(data, 3, color);
            data += 3;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<uchar4>().fill(uchar4(255)); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_generic_attribute_bmesh(const Object &object,
                                                        const OrigMeshData &orig_mesh_data,
                                                        const IndexMask &node_mask,
                                                        const StringRef name,
                                                        const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const BMDataLayerLookup attr = BM_data_layer_lookup(bm, name);
  if (attr.domain == bke::AttrDomain::Edge) {
    return;
  }

  if (!attr) {
    ensure_vbos_allocated_bmesh(
        object, attribute_format(orig_mesh_data, name, bke::AttrType::Float3), node_mask, vbos);
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float3>().fill(float3(0.0f)); },
                            exec_mode::grain_size(1));
    return;
  }

  ensure_vbos_allocated_bmesh(
      object, attribute_format(orig_mesh_data, name, attr.type), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        bke::attribute_math::to_static_type(attr.type, [&]<typename T>() {
          const auto &faces = BKE_pbvh_bmesh_node_faces(
              &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            switch (attr.domain) {
              case bke::AttrDomain::Point:
                extract_data_vert_bmesh<T>(faces, attr.offset, *vbos[i]);
                break;
              case bke::AttrDomain::Face:
                extract_data_face_bmesh<T>(faces, attr.offset, *vbos[i]);
                break;
              case bke::AttrDomain::Corner:
                extract_data_corner_bmesh<T>(faces, attr.offset, *vbos[i]);
                break;
              default:
                BLI_assert_unreachable();
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

static gpu::IndexBufPtr create_lines_index_faces(const OffsetIndices<int> faces,
                                                 const Span<bool> hide_poly,
                                                 const Span<int> face_indices)
{
  int corners_count = 0;
  for (const int face : face_indices) {
    if (!hide_poly.is_empty() && hide_poly[face]) {
      continue;
    }
    corners_count += faces[face].size();
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, corners_count, INT_MAX);
  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();

  int node_corner_offset = 0;
  int line_index = 0;
  for (const int face_index : face_indices) {
    const int face_size = faces[face_index].size();
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      node_corner_offset += face_size;
      continue;
    }
    for (const int i : IndexRange(face_size)) {
      const int next = (i == face_size - 1) ? 0 : i + 1;
      data[line_index] = uint2(i, next) + node_corner_offset;
      line_index++;
    }

    node_corner_offset += face_size;
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, node_corner_offset, false));
}

static gpu::IndexBufPtr create_lines_index_bmesh(const Set<BMFace *, 0> &faces,
                                                 const int visible_faces_num)
{
  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, visible_faces_num * 3, INT_MAX);

  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();

  int line_index = 0;
  int vert_index = 0;

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }

    data[line_index] = uint2(vert_index, vert_index + 1);
    line_index++;
    data[line_index] = uint2(vert_index + 1, vert_index + 2);
    line_index++;
    data[line_index] = uint2(vert_index + 2, vert_index);
    line_index++;

    vert_index += 3;
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, visible_faces_num * 3, false));
}

static int create_tri_index_grids(const Span<int> grid_indices,
                                  const BitGroupVector<> &grid_hidden,
                                  const int gridsize,
                                  const int skip,
                                  const int totgrid,
                                  MutableSpan<uint3> data)
{
  int tri_index = 0;
  int offset = 0;
  const int grid_vert_len = gridsize * gridsize;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    uint v0, v1, v2, v3;

    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }
        /* Indices in a Clockwise QUAD disposition. */
        v0 = offset + CCG_grid_xy_to_index(gridsize, x, y);
        v1 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y);
        v2 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y + skip);
        v3 = offset + CCG_grid_xy_to_index(gridsize, x, y + skip);

        data[tri_index] = uint3(v0, v2, v1);
        tri_index++;
        data[tri_index] = uint3(v0, v3, v2);
        tri_index++;
      }
    }
  }

  return tri_index;
}

static int create_tri_index_grids_flat_layout(const Span<int> grid_indices,
                                              const BitGroupVector<> &grid_hidden,
                                              const int gridsize,
                                              const int skip,
                                              const int totgrid,
                                              MutableSpan<uint3> data)
{
  int tri_index = 0;
  int offset = 0;
  const int grid_vert_len = square_uint(gridsize - 1) * 4;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    uint v0, v1, v2, v3;
    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }

        v0 = (y * (gridsize - 1) + x) * 4;

        if (skip > 1) {
          v1 = (y * (gridsize - 1) + x + skip - 1) * 4;
          v2 = ((y + skip - 1) * (gridsize - 1) + x + skip - 1) * 4;
          v3 = ((y + skip - 1) * (gridsize - 1) + x) * 4;
        }
        else {
          v1 = v2 = v3 = v0;
        }

        /* VBO data are in a Clockwise QUAD disposition.  Note
         * that vertices might be in different quads if we're
         * building a coarse index buffer.
         */
        v0 += offset;
        v1 += offset + 1;
        v2 += offset + 2;
        v3 += offset + 3;

        data[tri_index] = uint3(v0, v2, v1);
        tri_index++;
        data[tri_index] = uint3(v0, v3, v2);
        tri_index++;
      }
    }
  }
  return tri_index;
}

static void create_lines_index_grids(const Span<int> grid_indices,
                                     int display_gridsize,
                                     const BitGroupVector<> &grid_hidden,
                                     const int gridsize,
                                     const int skip,
                                     const int totgrid,
                                     MutableSpan<uint2> data)
{
  int line_index = 0;
  int offset = 0;
  const int grid_vert_len = gridsize * gridsize;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    uint v0, v1, v2, v3;
    bool grid_visible = false;

    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }
        /* Indices in a Clockwise QUAD disposition. */
        v0 = offset + CCG_grid_xy_to_index(gridsize, x, y);
        v1 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y);
        v2 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y + skip);
        v3 = offset + CCG_grid_xy_to_index(gridsize, x, y + skip);

        data[line_index] = uint2(v0, v1);
        line_index++;
        data[line_index] = uint2(v0, v3);
        line_index++;

        if (y / skip + 2 == display_gridsize) {
          data[line_index] = uint2(v2, v3);
          line_index++;
        }
        grid_visible = true;
      }

      if (grid_visible) {
        data[line_index] = uint2(v1, v2);
        line_index++;
      }
    }
  }
}

static void create_lines_index_grids_flat_layout(const Span<int> grid_indices,
                                                 int display_gridsize,
                                                 const BitGroupVector<> &grid_hidden,
                                                 const int gridsize,
                                                 const int skip,
                                                 const int totgrid,
                                                 MutableSpan<uint2> data)
{
  int line_index = 0;
  int offset = 0;
  const int grid_vert_len = square_uint(gridsize - 1) * 4;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    bool grid_visible = false;
    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    uint v0, v1, v2, v3;
    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }

        v0 = (y * (gridsize - 1) + x) * 4;

        if (skip > 1) {
          v1 = (y * (gridsize - 1) + x + skip - 1) * 4;
          v2 = ((y + skip - 1) * (gridsize - 1) + x + skip - 1) * 4;
          v3 = ((y + skip - 1) * (gridsize - 1) + x) * 4;
        }
        else {
          v1 = v2 = v3 = v0;
        }

        /* VBO data are in a Clockwise QUAD disposition.  Note
         * that vertices might be in different quads if we're
         * building a coarse index buffer.
         */
        v0 += offset;
        v1 += offset + 1;
        v2 += offset + 2;
        v3 += offset + 3;

        data[line_index] = uint2(v0, v1);
        line_index++;
        data[line_index] = uint2(v0, v3);
        line_index++;

        if (y / skip + 2 == display_gridsize) {
          data[line_index] = uint2(v2, v3);
          line_index++;
        }
        grid_visible = true;
      }

      if (grid_visible) {
        data[line_index] = uint2(v1, v2);
        line_index++;
      }
    }
  }
}

static Array<int> calc_material_indices(const Object &object, const OrigMeshData &orig_mesh_data)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray material_indices = *attributes.lookup<int>("material_index",
                                                              bke::AttrDomain::Face);
      if (!material_indices) {
        return {};
      }
      Array<int> node_materials(nodes.size());
      threading::parallel_for(nodes.index_range(), 64, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> face_indices = nodes[i].faces();
          if (face_indices.is_empty()) {
            continue;
          }
          node_materials[i] = material_indices[face_indices.first()];
        }
      });
      return node_materials;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      /* Use original mesh data because evaluated mesh is empty. */
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArray material_indices = *attributes.lookup<int>("material_index",
                                                              bke::AttrDomain::Face);
      if (!material_indices) {
        return {};
      }
      Array<int> node_materials(nodes.size());
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const Span<int> grid_faces = subdiv_ccg.grid_to_face_map;
      threading::parallel_for(nodes.index_range(), 64, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> grids = nodes[i].grids();
          if (grids.is_empty()) {
            continue;
          }
          node_materials[i] = material_indices[grid_faces[grids.first()]];
        }
      });
      return node_materials;
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

static BitVector<> calc_use_flat_layout(const Object &object, const OrigMeshData &orig_mesh_data)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      /* NOTE: Theoretically it would be possible to used vertex indexed buffers if there are no
       * face corner attributes, sharp faces, or face sets. */
      return {};
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
      if (sharp_faces.is_empty()) {
        return BitVector<>(nodes.size(), false);
      }

      const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;

      /* Use boolean array instead of #BitVector for parallelized writing. */
      Array<bool> use_flat_layout(nodes.size());
      threading::parallel_for(nodes.index_range(), 4, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> grids = nodes[i].grids();
          if (grids.is_empty()) {
            continue;
          }
          use_flat_layout[i] = std::any_of(grids.begin(), grids.end(), [&](const int grid) {
            return sharp_faces[grid_to_face_map[grid]];
          });
        }
      });
      return BitVector<>(use_flat_layout);
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

static gpu::IndexBufPtr create_tri_index_mesh(const OffsetIndices<int> faces,
                                              const Span<int3> corner_tris,
                                              const Span<bool> hide_poly,
                                              const bke::pbvh::MeshNode &node)
{
  const Span<int> face_indices = node.faces();
  int tris_num = 0;
  if (hide_poly.is_empty()) {
    tris_num = poly_to_tri_count(face_indices.size(), node.corners_num());
  }
  else {
    for (const int face : face_indices) {
      if (hide_poly[face]) {
        continue;
      }
      tris_num += bke::mesh::face_triangles_num(faces[face].size());
    }
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, tris_num, INT_MAX);
  MutableSpan<uint3> data = GPU_indexbuf_get_data(&builder).cast<uint3>();

  int tri_index = 0;
  int node_corner_offset = 0;
  for (const int face_index : face_indices) {
    const IndexRange face = faces[face_index];
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      node_corner_offset += face.size();
      continue;
    }
    for (const int3 &tri : corner_tris.slice(bke::mesh::face_triangles_range(faces, face_index))) {
      for (int i : IndexRange(3)) {
        const int corner = tri[i];
        const int index_in_face = corner - face.first();
        data[tri_index][i] = node_corner_offset + index_in_face;
      }
      tri_index++;
    }
    node_corner_offset += face.size();
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, node_corner_offset, false));
}

static gpu::IndexBufPtr create_tri_index_grids(const CCGKey &key,
                                               const BitGroupVector<> &grid_hidden,
                                               const bool do_coarse,
                                               const Span<int> grid_indices,
                                               const bool use_flat_layout)
{
  int gridsize = key.grid_size;
  int display_gridsize = gridsize;
  int totgrid = grid_indices.size();
  int skip = 1;

  const int display_level = do_coarse ? 0 : key.level;

  if (display_level < key.level) {
    display_gridsize = (1 << display_level) + 1;
    skip = 1 << (key.level - display_level - 1);
  }

  uint visible_quad_len = bke::pbvh::count_grid_quads(
      grid_hidden, grid_indices, key.grid_size, display_gridsize);

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, 2 * visible_quad_len, INT_MAX);

  MutableSpan<uint3> data = GPU_indexbuf_get_data(&builder).cast<uint3>();

  int tri_count;
  if (use_flat_layout) {
    tri_count = create_tri_index_grids_flat_layout(
        grid_indices, grid_hidden, gridsize, skip, totgrid, data);
  }
  else {
    tri_count = create_tri_index_grids(grid_indices, grid_hidden, gridsize, skip, totgrid, data);
  }

  builder.index_len = tri_count * 3;
  builder.index_min = 0;
  builder.index_max = 6 * visible_quad_len;
  builder.uses_restart_indices = false;
  gpu::IndexBufPtr result = gpu::IndexBufPtr(GPU_indexbuf_calloc());
  GPU_indexbuf_build_in_place(&builder, result.get());
  return result;
}

static gpu::IndexBufPtr create_lines_index_grids(const CCGKey &key,
                                                 const BitGroupVector<> &grid_hidden,
                                                 const bool do_coarse,
                                                 const Span<int> grid_indices,
                                                 const bool use_flat_layout)
{
  int gridsize = key.grid_size;
  int display_gridsize = gridsize;
  int totgrid = grid_indices.size();
  int skip = 1;

  const int display_level = do_coarse ? 0 : key.level;

  if (display_level < key.level) {
    display_gridsize = (1 << display_level) + 1;
    skip = 1 << (key.level - display_level - 1);
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(
      &builder, GPU_PRIM_LINES, 2 * totgrid * display_gridsize * (display_gridsize - 1), INT_MAX);

  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();
  /* The buffer might contain hidden elements which are not initialized but still accounted. We
   * don't count them to skip from allocation, so must fill that gaps by 0 to hide redundant edges.
   */
  data.fill(uint2(0));

  if (use_flat_layout) {
    create_lines_index_grids_flat_layout(
        grid_indices, display_gridsize, grid_hidden, gridsize, skip, totgrid, data);
  }
  else {
    create_lines_index_grids(
        grid_indices, display_gridsize, grid_hidden, gridsize, skip, totgrid, data);
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(
      &builder, 0, 2 * totgrid * display_gridsize * (display_gridsize - 1), false));
}

Span<gpu::IndexBufPtr> DrawCacheImpl::ensure_lines_indices(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           const IndexMask &node_mask,
                                                           const bool coarse)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  Vector<gpu::IndexBufPtr> &ibos = coarse ? lines_ibos_coarse_ : lines_ibos_;
  ibos.resize(pbvh.nodes_num());

  IndexMaskMemory memory;
  const IndexMask nodes_to_calculate = IndexMask::from_predicate(
      node_mask, memory, [&](const int i) { return !ibos[i]; });

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const OffsetIndices<int> faces = mesh.faces();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            ibos[i] = create_lines_index_faces(faces, hide_poly, nodes[i].faces());
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
            const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
            ibos[i] = create_lines_index_grids(
                key, subdiv_ccg.grid_hidden, coarse, nodes[i].grids(), use_flat_layout_[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            const Set<BMFace *, 0> &faces = BKE_pbvh_bmesh_node_faces(
                &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
            const int visible_faces_num = count_visible_tris_bmesh(faces);
            ibos[i] = create_lines_index_bmesh(faces, visible_faces_num);
          },
          exec_mode::grain_size(1));
      break;
    }
  }

  return ibos;
}

BitSpan DrawCacheImpl::ensure_use_flat_layout(const Object &object,
                                              const OrigMeshData &orig_mesh_data)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (use_flat_layout_.size() != pbvh.nodes_num()) {
    use_flat_layout_ = calc_use_flat_layout(object, orig_mesh_data);
  }
  return use_flat_layout_;
}

BLI_NOINLINE static void flush_vbo_data(const Span<gpu::VertBufPtr> vbos,
                                        const IndexMask &node_mask)
{
  node_mask.foreach_index([&](const int i) { GPU_vertbuf_use(vbos[i].get()); });
}

Span<gpu::VertBufPtr> DrawCacheImpl::ensure_attribute_data(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           const AttributeRequest &attr,
                                                           const IndexMask &node_mask)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  AttributeData &data = attribute_vbos_.lookup_or_add_default(attr);
  Vector<gpu::VertBufPtr> &vbos = data.vbos;
  vbos.resize(pbvh.nodes_num());

  /* The nodes we recompute here are a combination of:
   *   1. null VBOs, which correspond to nodes that either haven't been drawn before, or have been
   *      cleared completely by #free_nodes_with_changed_topology.
   *   2. Nodes that have been tagged dirty as their values are changed.
   * We also only process a subset of the nodes referenced by the caller, for example to only
   * recompute visible nodes. */
  IndexMaskMemory memory;
  const IndexMask empty_mask = IndexMask::from_predicate(
      node_mask, memory, [&](const int i) { return !vbos[i]; });
  const IndexMask dirty_mask = IndexMask::from_bits(
      node_mask.slice_content(data.dirty_nodes.index_range()), data.dirty_nodes, memory);
  const IndexMask mask = IndexMask::from_union(empty_mask, dirty_mask, memory);

  /* Aggregate timing of the node-buffer extraction loop (one line per redraw call, not per node). */
#if PBVH_DRAW_DEBUG_PERF
  const auto pdp_start = std::chrono::high_resolution_clock::now();
#endif

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            update_positions_mesh(object, mask, vbos);
            break;
          case CustomRequest::Normal:
            update_normals_mesh(object, mask, vbos);
            break;
          case CustomRequest::Mask:
            update_masks_mesh(object, orig_mesh_data, mask, vbos);
            break;
          case CustomRequest::FaceSet:
            update_face_sets_mesh(object, orig_mesh_data, mask, vbos);
            break;
          case CustomRequest::LayerMask:
            update_layer_masks_mesh(object, orig_mesh_data, mask, vbos);
            break;
        }
      }
      else {
        update_generic_attribute_mesh(
            object, orig_mesh_data, mask, std::get<GenericRequest>(attr), vbos);
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            fill_positions_grids(object, use_flat_layout_, mask, vbos);
            break;
          case CustomRequest::Normal:
            fill_normals_grids(object, orig_mesh_data, use_flat_layout_, mask, vbos);
            break;
          case CustomRequest::Mask:
            fill_masks_grids(object, use_flat_layout_, mask, vbos);
            break;
          case CustomRequest::FaceSet:
            fill_face_sets_grids(object, orig_mesh_data, use_flat_layout_, mask, vbos);
            break;
          case CustomRequest::LayerMask:
            fill_layer_masks_grids(object, orig_mesh_data, use_flat_layout_, mask, vbos);
            break;
        }
      }
      else {
        ensure_vbos_allocated_grids(
            object,
            attribute_format(orig_mesh_data, "Dummy", bke::AttrType::Float3),
            use_flat_layout_,
            mask,
            vbos);
        mask.foreach_index([&](const int i) { vbos[i]->data<float3>().fill(float3(0.0f)); },
                           exec_mode::grain_size(1));
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            update_positions_bmesh(object, mask, vbos);
            break;
          case CustomRequest::Normal:
            update_normals_bmesh(object, mask, vbos);
            break;
          case CustomRequest::Mask:
            update_masks_bmesh(object, mask, vbos);
            break;
          case CustomRequest::FaceSet:
            update_face_sets_bmesh(object, orig_mesh_data, mask, vbos);
            break;
          case CustomRequest::LayerMask:
            update_layer_masks_bmesh(object, mask, vbos);
            break;
        }
      }
      else {
        update_generic_attribute_bmesh(
            object, orig_mesh_data, mask, std::get<GenericRequest>(attr), vbos);
      }
      break;
    }
  }

#if PBVH_DRAW_DEBUG_PERF
  const auto pdp_end = std::chrono::high_resolution_clock::now();
  const long long pdp_us =
      std::chrono::duration_cast<std::chrono::microseconds>(pdp_end - pdp_start).count();
  const char *pdp_attr_name = "Generic";
  if (const CustomRequest *pdp_rq = std::get_if<CustomRequest>(&attr)) {
    switch (*pdp_rq) {
      case CustomRequest::Position:
        pdp_attr_name = "Position";
        break;
      case CustomRequest::Normal:
        pdp_attr_name = "Normal";
        break;
      case CustomRequest::Mask:
        pdp_attr_name = "Mask";
        break;
      case CustomRequest::FaceSet:
        pdp_attr_name = "FaceSet";
        break;
      case CustomRequest::LayerMask:
        pdp_attr_name = "LayerMask";
        break;
    }
  }
#endif
  PDP_PERF("[DEBUG-perf] pbvh_draw_extract: attr=%s rebuilt %d nodes in %lld us\n",
           pdp_attr_name,
           int(mask.size()),
           pdp_us);

  /* TODO: It would be good to deallocate the bit vector if all of the bits have been reset to
   * avoid unnecessary processing in subsequent redraws. */
  dirty_mask.foreach_index_optimized<int>([&](const int i) { data.dirty_nodes[i].reset(); });

  flush_vbo_data(vbos, mask);

  return vbos;
}

Span<gpu::IndexBufPtr> DrawCacheImpl::ensure_tri_indices(const Object &object,
                                                         const OrigMeshData &orig_mesh_data,
                                                         const IndexMask &node_mask,
                                                         const bool coarse)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      Vector<gpu::IndexBufPtr> &ibos = tris_ibos_;
      ibos.resize(nodes.size());

      /* Whenever a node's visible triangle count has changed the index buffers are freed, so we
       * only recalculate null IBOs here. A new mask is recalculated for more even task
       * distribution between threads. */
      IndexMaskMemory memory;
      const IndexMask nodes_to_calculate = IndexMask::from_predicate(
          node_mask, memory, [&](const int i) { return !ibos[i]; });

      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int3> corner_tris = mesh.corner_tris();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            ibos[i] = create_tri_index_mesh(faces, corner_tris, hide_poly, nodes[i]);
          },
          exec_mode::grain_size(1));
      return ibos;
    }
    case bke::pbvh::Type::Grids: {
      /* Unlike the other geometry types, multires grids use indexed vertex buffers because when
       * there are no flat faces, vertices can be shared between neighboring quads. This results in
       * a 4x decrease in the amount of data uploaded. Theoretically it also means freeing VBOs
       * because of visibility changes is unnecessary.
       *
       * TODO: With the "flat layout" and no hidden faces, the index buffers are unnecessary, we
       * should avoid creating them in that case. */
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

      Vector<gpu::IndexBufPtr> &ibos = coarse ? tris_ibos_coarse_ : tris_ibos_;
      ibos.resize(nodes.size());

      /* Whenever a node's visible triangle count has changed the index buffers are freed, so we
       * only recalculate null IBOs here. A new mask is recalculated for more even task
       * distribution between threads. */
      IndexMaskMemory memory;
      const IndexMask nodes_to_calculate = IndexMask::from_predicate(
          node_mask, memory, [&](const int i) { return !ibos[i]; });

      const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

      nodes_to_calculate.foreach_index(
          [&](const int i) {
            ibos[i] = create_tri_index_grids(
                key, subdiv_ccg.grid_hidden, coarse, nodes[i].grids(), use_flat_layout_[i]);
          },
          exec_mode::grain_size(1));
      return ibos;
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

bool DrawCacheImpl::begin_influence_drag(const int layer_uid)
{
  influence_drag_active_ = true;
  influence_drag_rebuild_delta_ = true;
  influence_drag_layer_uid_ = layer_uid;
  influence_drag_pending_scale_ = 0.0f;
  return true;
}

void DrawCacheImpl::add_influence_drag_delta(const float scale)
{
  influence_drag_pending_scale_ += scale;
}

void DrawCacheImpl::end_influence_drag()
{
  /* The delta buffers stay allocated until the next #ensure_influence_drag frees them in a bound
   * GPU context. */
  influence_drag_active_ = false;
  influence_drag_rebuild_delta_ = false;
  influence_drag_pending_scale_ = 0.0f;
  influence_drag_layer_uid_ = -1;
}

void DrawCacheImpl::ensure_influence_drag(const Object &object, const IndexMask &node_mask)
{
  if (!influence_drag_active_) {
    if (!influence_drag_delta_vbos_.is_empty()) {
      influence_drag_delta_vbos_.clear_and_shrink();
    }
    return;
  }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return;
  }
  AttributeData *pos_data = attribute_vbos_.lookup_ptr(CustomRequest::Position);
  if (!pos_data) {
    return;
  }
  Vector<gpu::VertBufPtr> &pos_vbos = pos_data->vbos;
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  /* Build the corner-expanded delta buffers for the dragged layer once at the start of the drag. */
  if (influence_drag_rebuild_delta_) {
    influence_drag_rebuild_delta_ = false;
    Mesh &mesh_orig = *id_cast<Mesh *>(DEG_get_original(&object)->data);
    const SculptLayer *layer = bke::sculpt_layers::node_as_layer(
        bke::sculpt_layers::node_find_by_uid(mesh_orig, influence_drag_layer_uid_));
    const Mesh &mesh_eval = DRW_object_get_data_for_drawing<Mesh>(object);
    if (!layer || !layer->data || layer->totelem < mesh_eval.verts_num) {
      /* Topology mismatch or missing data: abandon the GPU path; the operator's release still does
       * a full CPU reconcile. */
      PDP_PERF("[DEBUG-perf] influence_drag: rebuild bail (uid=%d layer=%p data=%p totelem=%d verts=%d)\n",
               influence_drag_layer_uid_,
               (void *)layer,
               layer ? layer->data : nullptr,
               layer ? layer->totelem : -1,
               mesh_eval.verts_num);
      influence_drag_active_ = false;
      return;
    }
    const OffsetIndices<int> faces = mesh_eval.faces();
    const Span<int> corner_verts = mesh_eval.corner_verts();
    const Span<float3> deltas(static_cast<const float3 *>(layer->data), layer->totelem);
    influence_drag_delta_vbos_.clear();
    influence_drag_delta_vbos_.resize(pbvh.nodes_num());
    ensure_vbos_allocated_mesh(object, position_format(), node_mask, influence_drag_delta_vbos_);
    node_mask.foreach_index(
        [&](const int i) {
          extract_data_vert_mesh<float3>(
              faces, corner_verts, deltas, nodes[i].faces(), *influence_drag_delta_vbos_[i]);
        },
        exec_mode::grain_size(1));
    flush_vbo_data(influence_drag_delta_vbos_, node_mask);
  }

  if (influence_drag_pending_scale_ == 0.0f || influence_drag_delta_vbos_.is_empty()) {
    return;
  }
  const float scale = influence_drag_pending_scale_;
  influence_drag_pending_scale_ = 0.0f;

  if (!influence_drag_shader_) {
    influence_drag_shader_ = GPU_shader_create_from_info_name("pbvh_layer_drag");
  }
  if (!influence_drag_shader_) {
    return;
  }
  GPU_shader_bind(influence_drag_shader_);
  GPU_shader_uniform_1f(influence_drag_shader_, "scale", scale);

#if PBVH_DRAW_DEBUG_PERF
  const auto t0 = std::chrono::high_resolution_clock::now();
#endif
  int dispatch_count = 0;
  /* GPU binding/dispatch is not thread-safe, so this loop must stay serial. */
  node_mask.foreach_index([&](const int i) {
    if (!pos_vbos[i] || !influence_drag_delta_vbos_[i]) {
      return;
    }
    const int verts_num = nodes[i].corners_num();
    GPU_shader_uniform_1i(influence_drag_shader_, "verts_num", verts_num);
    GPU_vertbuf_bind_as_ssbo(influence_drag_delta_vbos_[i].get(), 0);
    GPU_vertbuf_bind_as_ssbo(pos_vbos[i].get(), 1);
    GPU_compute_dispatch(influence_drag_shader_, (verts_num + 63) / 64, 1, 1);
    dispatch_count++;
  });
  GPU_memory_barrier(GPU_BARRIER_VERTEX_ATTRIB_ARRAY | GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

#if PBVH_DRAW_DEBUG_PERF
  const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::high_resolution_clock::now() - t0)
                           .count();
  PDP_PERF("[DEBUG-perf] influence_drag_compute: %d dispatches in %lld us\n", dispatch_count, us);
#else
  UNUSED_VARS(dispatch_count);
#endif
}

Span<gpu::Batch *> DrawCacheImpl::ensure_tris_batches(const Object &object,
                                                      const ViewportRequest &request,
                                                      const IndexMask &nodes_to_update)
{
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data{*id_cast<const Mesh *>(object_orig.data)};
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  this->ensure_use_flat_layout(object, orig_mesh_data);
  this->free_nodes_with_changed_topology(pbvh);

  const Span<gpu::IndexBufPtr> ibos = this->ensure_tri_indices(
      object, orig_mesh_data, nodes_to_update, request.use_coarse_grids);

  for (const AttributeRequest &attr : request.attributes) {
    this->ensure_attribute_data(object, orig_mesh_data, attr, nodes_to_update);
  }

  /* Interactive influence drag: recompute the position buffers on the GPU instead of re-extracting
   * them from the CPU. Runs after the position VBOs exist and before they are bound into batches. */
  this->ensure_influence_drag(object, nodes_to_update);

  /* Collect VBO spans in a different loop because #ensure_attribute_data invalidates the allocated
   * arrays when its map is changed. */
  Vector<Span<gpu::VertBufPtr>> attr_vbos;
  for (const AttributeRequest &attr : request.attributes) {
    if (const AttributeData *attr_data = attribute_vbos_.lookup_ptr(attr)) {
      attr_vbos.append(attr_data->vbos);
    }
  }

  /* Except for the first iteration of the draw loop, we only need to rebuild batches for nodes
   * with changed topology (visible triangle count). */
  Vector<gpu::Batch *> &batches = tris_batches_.lookup_or_add_default(request);
  batches.resize(pbvh.nodes_num(), nullptr);
  nodes_to_update.foreach_index(
      [&](const int i) {
        if (!batches[i]) {
          batches[i] = GPU_batch_create(
              GPU_PRIM_TRIS, nullptr, ibos.is_empty() ? nullptr : ibos[i].get());
          for (const Span<gpu::VertBufPtr> vbos : attr_vbos) {
            GPU_batch_vertbuf_add(batches[i], vbos[i].get(), false);
          }
        }
      },
      exec_mode::grain_size(64));

  return batches;
}

Span<gpu::Batch *> DrawCacheImpl::ensure_lines_batches(const Object &object,
                                                       const ViewportRequest &request,
                                                       const IndexMask &nodes_to_update)
{
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data(*id_cast<const Mesh *>(object_orig.data));
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  this->ensure_use_flat_layout(object, orig_mesh_data);
  this->free_nodes_with_changed_topology(pbvh);

  const Span<gpu::VertBufPtr> position = this->ensure_attribute_data(
      object, orig_mesh_data, CustomRequest::Position, nodes_to_update);
  const Span<gpu::IndexBufPtr> lines = this->ensure_lines_indices(
      object, orig_mesh_data, nodes_to_update, request.use_coarse_grids);

  /* Except for the first iteration of the draw loop, we only need to rebuild batches for nodes
   * with changed topology (visible triangle count). */
  Vector<gpu::Batch *> &batches = request.use_coarse_grids ? lines_batches_coarse_ :
                                                             lines_batches_;
  batches.resize(pbvh.nodes_num(), nullptr);
  nodes_to_update.foreach_index(
      [&](const int i) {
        if (!batches[i]) {
          batches[i] = GPU_batch_create(GPU_PRIM_LINES, nullptr, lines[i].get());
          GPU_batch_vertbuf_add(batches[i], position[i].get(), false);
        }
      },
      exec_mode::grain_size(64));

  return batches;
}

Span<int> DrawCacheImpl::ensure_material_indices(const Object &object)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (material_indices_.size() != pbvh.nodes_num()) {
    const Object &object_orig = *DEG_get_original(&object);
    const OrigMeshData orig_mesh_data(*id_cast<const Mesh *>(object_orig.data));
    material_indices_ = calc_material_indices(object, orig_mesh_data);
  }
  return material_indices_;
}

}  // namespace draw::pbvh
}  // namespace blender
