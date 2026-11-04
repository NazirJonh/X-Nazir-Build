/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Core of #SCULPT_OT_layer_mirror_copy: side tests, the per-domain locators, and the mapping build
 * and write. See `sculpt_layer_mirror_map.hh` for the interface and `sculpt_layer_mirror.cc` for
 * the operator around it.
 */

#include <algorithm>
#include <climits>
#include <optional>

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_kdtree.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_ccg.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_paint.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "sculpt_intern.hh"
#include "sculpt_layer_mirror_map.hh"

#include "../paint_intern.hh"

namespace blender::ed::sculpt_paint::layers::mirror_copy {

/* -------------------------------------------------------------------- */
/** \name Core: parameters and side tests
 * \{ */

enum class SideSign {
  Negative = -1,
  Plane = 0,
  Positive = 1,
};

static SideSign side_sign_of(const float v, const float plane_eps)
{
  if (math::abs(v) < plane_eps) {
    return SideSign::Plane;
  }
  return v > 0.0f ? SideSign::Positive : SideSign::Negative;
}

/** The sign an element on the far side of the same plane carries. */
static SideSign opposite_sign(const SideSign sign)
{
  return (sign == SideSign::Positive) ? SideSign::Negative : SideSign::Positive;
}

/**
 * True when \a co lies on the same side as the source point for every enabled axis. A source point
 * sitting on an axis' plane leaves that axis unfiltered (any non-plane sign is a source); an element
 * on a plane is never a source, because its displacement does not mirror onto itself when the
 * along-axis component is non-zero.
 */
static bool is_source_side(const float3 &co, const MirrorParams &params)
{
  for (const int axis : {0, 1, 2}) {
    if (!(params.symm & ePaintSymmetryFlags(1 << axis))) {
      continue;
    }
    const SideSign point_sign = side_sign_of(params.source_point[axis], params.plane_eps);
    const SideSign elem_sign = side_sign_of(co[axis], params.plane_eps);
    if (point_sign != SideSign::Plane) {
      if (elem_sign != point_sign) {
        return false;
      }
    }
    else if (elem_sign == SideSign::Plane) {
      return false;
    }
  }
  return true;
}

/**
 * True when \a co is a valid mirror target: opposite sign to the source point on every enabled axis,
 * or on the plane — a target on the plane takes the flipped delta's in-plane components only, which
 * is the correct mirror of a plane-adjacent source.
 */
static bool is_target_side(const float3 &co, const MirrorParams &params)
{
  for (const int axis : {0, 1, 2}) {
    if (!(params.symm & ePaintSymmetryFlags(1 << axis))) {
      continue;
    }
    const SideSign point_sign = side_sign_of(params.source_point[axis], params.plane_eps);
    if (point_sign == SideSign::Plane) {
      continue;
    }
    const SideSign elem_sign = side_sign_of(co[axis], params.plane_eps);
    if (elem_sign != SideSign::Plane && elem_sign != opposite_sign(point_sign)) {
      return false;
    }
  }
  return true;
}

float mirror_element_spacing(const Mesh &mesh, int top_grid_size)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  if (edges.is_empty()) {
    return 0.0f;
  }
  double sum = 0.0;
  for (const int2 &edge : edges) {
    sum += double(math::distance(positions[edge[0]], positions[edge[1]]));
  }
  const float coarse = float(sum / double(edges.size()));
  return (top_grid_size > 1) ? coarse / float(top_grid_size - 1) : coarse;
}

/**
 * The composite weight mask of one layer, folded once for a whole mapping pass.
 *
 * #bke::sculpt_layers::node_mask_for_composite and #bke::sculpt_layers::grid_masks_for_composite walk
 * the folder chain above the layer, so asking them per element would re-resolve the same chain for
 * every source. Blocks are folded lazily and the last one is kept, which is the whole grid for the
 * grid domain (the sources are scanned grid-major) and a `SCULPT_LAYER_MASK_VERT_BLOCK` run for the
 * vertex domain.
 */
class MaskWeights {
  bke::sculpt_layers::CompositeMask masks_;
  int block_size_;
  int cached_block_ = -1;
  bke::sculpt_layers::MaskBlockWeight cached_weight_;

 public:
  MaskWeights(const SculptLayer &layer,
              const bool grids,
              const int64_t elem_num,
              const int block_size)
      : masks_(grids ? bke::sculpt_layers::grid_masks_for_composite(layer, elem_num, block_size) :
                       bke::sculpt_layers::node_mask_for_composite(layer, elem_num)),
        block_size_(block_size)
  {
  }

  /** The element's weight, folding the layer's own mask with its folder chain. */
  float at(const int64_t elem)
  {
    if (!masks_.primary && !masks_.secondary) {
      return 1.0f;
    }
    const int block = int(elem / block_size_);
    if (block != cached_block_) {
      cached_block_ = block;
      cached_weight_ = bke::sculpt_layers::mask_block_weight(masks_, block, 1.0f);
    }
    if (cached_weight_.skip) {
      return 0.0f;
    }
    return bke::sculpt_layers::mask_elem_weight(cached_weight_, int(elem % block_size_));
  }
};

/** A pair before the domain-specific delta transform (see #MirrorPair). */
struct MirrorDraft {
  int64_t src;
  int64_t dst;
  /** The symmetry pass, as the argument of #symmetry_flip. */
  int symm_it;
};

/**
 * Sort by (dst, src) so every destination forms one contiguous run and the run resolves
 * deterministically: #mirror_mapping_apply takes the largest source of the run and Add sums it in index
 * order, whatever order the spatial search happened to return.
 */
static void sort_pairs(Vector<MirrorPair> &r_pairs)
{
  std::sort(r_pairs.begin(), r_pairs.end(), [](const MirrorPair &a, const MirrorPair &b) {
    if (a.dst != b.dst) {
      return a.dst < b.dst;
    }
    return a.src < b.src;
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Core: the vertex-domain locator
 * \{ */

/**
 * The vertex positions without any sculpt layer on them. The composed surface cannot pair mirrored
 * elements: a vertex a layer moved far from rest has a flipped position that lands near no vertex of
 * the untouched other side, so only the faintly displaced rim of a sculpted area would find a target.
 * The runtime base is the layer-free shape; under a deform modifier or a shape key no base is kept and
 * the mesh positions are the untouched basis already.
 */
static Span<float3> vert_base_positions(Object &object)
{
  const SculptSession *ss = session_of(object);
  if (ss != nullptr && !ss->layers.mesh_base.is_empty()) {
    return ss->layers.mesh_base.as_span();
  }
  return mesh_of(object).vert_positions();
}

/**
 * Vertex domain: pairs are found on the layer-free base positions (see #vert_base_positions), and
 * targets from a KD-tree of the visible vertices.
 */
class VertLocator {
  const MirrorParams &params_;
  Mesh &mesh_;
  Span<float3> positions_;
  Span<float3> layer_data_;
  /** Visible vertices, inserted into the tree. */
  KDTree<float3> *tree_ = nullptr;
  Vector<int64_t> sources_;
  /** Vertices of any face carrying #MirrorParams::face_set_id, empty when unfiltered. */
  BitVector<> face_set_verts_;
  /** Non-zero when the mesh carries face sets, so #face_set_at can answer at all. */
  VArraySpan<int> face_set_;
  VArraySpan<bool> hide_vert_;
  MaskWeights weights_;
  bool valid_ = false;

 public:
  VertLocator(const MirrorParams &params, Object &object, const SculptLayer &layer)
      : params_(params),
        mesh_(mesh_of(object)),
        positions_(vert_base_positions(object)),
        layer_data_(static_cast<const float3 *>(layer.data), layer.totelem),
        weights_(layer, false, layer.totelem, SCULPT_LAYER_MASK_VERT_BLOCK)
  {
    if (int64_t(positions_.size()) != layer.totelem) {
      /* The domain the layer describes is not the one being sculpted. The staleness gate should have
       * caught it; refuse rather than index a mismatched layout. */
      return;
    }
    const bke::AttributeAccessor attributes = mesh_.attributes();
    hide_vert_ = VArraySpan<bool>(*attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point));
    /* A mesh without the attribute is possible (face sets are not a sculpt-mode guarantee), and the
     * empty span it leaves behind is what tells every filter below to restrict nothing. */
    if (attributes.contains(".sculpt_face_set")) {
      face_set_ = VArraySpan<int>(*attributes.lookup<int>(".sculpt_face_set",
                                                           bke::AttrDomain::Face));
    }

    tree_ = blender::kdtree_new<float3>(uint(positions_.size()));
    for (const int64_t i : positions_.index_range()) {
      if (!hide_vert_.is_empty() && hide_vert_[int(i)]) {
        continue;
      }
      blender::kdtree_insert(tree_, int(i), positions_[i]);
    }
    blender::kdtree_balance(tree_);
    valid_ = true;
  }

  VertLocator(const VertLocator &) = delete;
  VertLocator &operator=(const VertLocator &) = delete;
  ~VertLocator()
  {
    blender::kdtree_free(tree_);
  }

  bool valid() const
  {
    return valid_;
  }

  /** The base position of an element picked by the click; the source side is decided by it. */
  float3 point_at(const int64_t elem) const
  {
    return positions_[elem];
  }

  /**
   * Gather the source side, once #MirrorParams::source_point is known. Every element of it is a
   * source, an empty one included: Replace has to be able to zero the target of a source that carries
   * nothing.
   */
  bool collect_sources()
  {
    for (const int64_t i : positions_.index_range()) {
      if (!hide_vert_.is_empty() && hide_vert_[int(i)]) {
        continue;
      }
      if (is_source_side(positions_[i], params_)) {
        sources_.append(i);
      }
    }
    return !sources_.is_empty();
  }

  const Vector<int64_t> &sources() const
  {
    return sources_;
  }

  float3 position(const int64_t elem) const
  {
    return positions_[elem];
  }

  /** A vertex has no seam copies: the target is the only element to write. */
  template<typename FnT> void for_each_target(const int64_t dst, FnT fn) const
  {
    fn(dst);
  }

  /** The visible vertex nearest to \a co, within tolerance and not \a exclude itself. */
  std::optional<int64_t> nearest(const float3 &co, const int64_t exclude) const
  {
    KDTreeNearest<float3> found;
    if (blender::kdtree_find_nearest(tree_, co, &found) < 0) {
      return std::nullopt;
    }
    if (found.index < 0 || int64_t(found.index) == exclude || found.dist > params_.tolerance) {
      return std::nullopt;
    }
    return int64_t(found.index);
  }

  /**
   * Mark the sources of #MirrorParams::face_set_id. A vertex belongs to the set when any of its faces
   * does, which is what keeps a set border on the source side whole instead of leaving it to whichever
   * face happens to come first.
   */
  void restrict_to_face_set(const int face_set_id)
  {
    if (face_set_id < 0 || face_set_.is_empty()) {
      return;
    }
    face_set_verts_ = BitVector<>(positions_.size(), false);
    const OffsetIndices<int> faces = mesh_.faces();
    for (const int face : faces.index_range()) {
      if (face_set_[face] != face_set_id) {
        continue;
      }
      for (const int vert : mesh_.corner_verts().slice(faces[face])) {
        face_set_verts_[vert].set(true);
      }
    }
  }

  /** The face set of the vertex nearest to \a co, or -1. Only the exec path asks: the modal path
   * reads the face straight off the ray hit. */
  int face_set_at(const float3 &co) const
  {
    if (face_set_.is_empty()) {
      return -1;
    }
    const std::optional<int64_t> vert = nearest(co, -1);
    if (!vert) {
      return -1;
    }
    const GroupedSpan<int> vert_to_corner = mesh_.vert_to_corner_map();
    const Span<int> corners = vert_to_corner[int(*vert)];
    if (corners.is_empty()) {
      return -1;
    }
    return face_set_[mesh_.corner_to_face_map()[corners[0]]];
  }

  bool in_face_set(const int64_t elem) const
  {
    return face_set_verts_.is_empty() ? true : bool(face_set_verts_[elem]);
  }

  /** The mirrored delta needs no basis change in the vertex domain: the data is already object
   * space. */
  bool resolve(const Vector<MirrorDraft> &drafts, Vector<MirrorPair> &r_pairs)
  {
    r_pairs.reserve(drafts.size());
    for (const MirrorDraft &draft : drafts) {
      r_pairs.append(
          {draft.src,
           draft.dst,
           symmetry_flip(layer_data_[draft.src], ePaintSymmetryFlags(draft.symm_it)),
           weights_.at(draft.src),
           in_face_set(draft.src)});
    }
    sort_pairs(r_pairs);
    return true;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Core: the grid (multires) domain locator
 * \{ */

/**
 * Grid domain.
 *
 * The mapping addresses the layer's own top-level layout, which the gate guarantees is the live CCG's
 * (one CCG point per layer element, so an element index is also the flat CCG index).
 *
 * Pairs are found on the CCG positions with every enabled grid layer's contribution taken back off:
 * the composed positions cannot pair mirrored points, because a point a layer moved far from rest
 * has a flipped position that lands near no point of the undisplaced other side (see
 * #vert_base_positions). The contribution comes from the same evaluator the base flush subtracts
 * with, so the result is the shape the layers were composed onto.
 */
class GridLocator {
  const MirrorParams &params_;
  Mesh &base_mesh_;
  SubdivCCG &subdiv_ccg_;
  const CCGKey key_;
  const int top_level_;
  const int top_grid_size_;
  const int64_t top_area_;
  /** Nesting stride between the layer's top level and the live CCG level. */
  const int step_;
  /** The CCG positions with the layers taken off, one per element. */
  Array<float3> base_positions_;
  /**
   * Seam duplicates: a coarse vertex or an edge point sits in every grid that touches it, as separate
   * elements. #alias_group_ gives each element's group (-1 for an interior point), and every group
   * lists its elements with the smallest first, which is the one that stands for the point as a source.
   */
  Array<int> alias_group_;
  Vector<Vector<int64_t>> alias_groups_;
  KDTree<float3> *tree_ = nullptr;
  Span<float3> layer_data_;
  Vector<int64_t> sources_;
  /** One entry per grid: whether it belongs to #MirrorParams::face_set_id. */
  Vector<bool> grid_in_face_set_;
  /** Faces carrying `.sculpt_face_set` at all; empty means the limit restricts nothing. */
  VArraySpan<int> face_set_;
  MaskWeights weights_;
  bool valid_ = false;

 public:
  GridLocator(const MirrorParams &params, Object &object, const SculptLayer &layer)
      : params_(params),
        base_mesh_(*id_cast<Mesh *>(object.data)),
        subdiv_ccg_(*session_of(object)->subdiv_ccg),
        key_(BKE_subdiv_ccg_key_top_level(subdiv_ccg_)),
        top_level_(layer.level),
        top_grid_size_(CCG_grid_size(layer.level)),
        top_area_(int64_t(top_grid_size_) * top_grid_size_),
        step_((top_grid_size_ - 1) / (key_.grid_size - 1)),
        layer_data_(static_cast<const float3 *>(layer.data), layer.totelem),
        weights_(layer, true, layer.totelem, int(top_area_))
  {
    /* The CCG indexing below assumes one CCG point per layer element (`step_ == 1`); the gate refuses
     * every other sculpt level, this keeps a missed path from indexing out of the CCG. */
    if (layer.totelem != int64_t(subdiv_ccg_.grids_num) * top_area_ || step_ != 1 ||
        layer.totelem > INT_MAX || int64_t(subdiv_ccg_.positions.size()) != layer.totelem)
    {
      return;
    }
    if (base_mesh_.attributes().contains(".sculpt_face_set")) {
      face_set_ = VArraySpan<int>(
          *base_mesh_.attributes().lookup<int>(".sculpt_face_set", bke::AttrDomain::Face));
    }

    base_positions_ = Array<float3>(subdiv_ccg_.positions.as_span());
    Array<float3> contribution(base_positions_.size());
    for (const SculptLayer *grid_layer : bke::sculpt_layers::layers(base_mesh_)) {
      const float weight = bke::sculpt_layers::effective(*grid_layer);
      if (weight == 0.0f || grid_layer->domain != SCULPT_LAYER_DOMAIN_GRID ||
          grid_layer->data == nullptr || grid_layer->level != top_level_ ||
          bke::sculpt_layers::is_stale(base_mesh_, *grid_layer))
      {
        continue;
      }
      if (!BKE_multires_sculpt_layer_object_contribution(
              base_mesh_, subdiv_ccg_, *grid_layer, contribution))
      {
        return;
      }
      for (const int64_t i : base_positions_.index_range()) {
        base_positions_[i] -= contribution[i] * weight;
      }
    }

    alias_groups_build();

    tree_ = blender::kdtree_new<float3>(uint(base_positions_.size()));
    for (const int grid : IndexRange(subdiv_ccg_.grids_num)) {
      for (const int y_top : IndexRange(top_grid_size_)) {
        for (const int x_top : IndexRange(top_grid_size_)) {
          if (top_visible(grid, x_top, y_top)) {
            const int64_t elem = int64_t(grid) * top_area_ + int64_t(y_top) * top_grid_size_ + x_top;
            blender::kdtree_insert(tree_, int(elem), base_positions_[elem]);
          }
        }
      }
    }
    blender::kdtree_balance(tree_);
    valid_ = true;
  }

  GridLocator(const GridLocator &) = delete;
  GridLocator &operator=(const GridLocator &) = delete;
  ~GridLocator()
  {
    blender::kdtree_free(tree_);
  }

  bool valid() const
  {
    return valid_;
  }

  /** The base position of an element picked by the click; the source side is decided by it. */
  float3 point_at(const int64_t elem) const
  {
    return base_positions_[elem];
  }

  /** Gather the source side, once #MirrorParams::source_point is known. */
  bool collect_sources()
  {
    for (const int grid : IndexRange(subdiv_ccg_.grids_num)) {
      for (const int y_top : IndexRange(top_grid_size_)) {
        for (const int x_top : IndexRange(top_grid_size_)) {
          const int64_t elem = int64_t(grid) * top_area_ + int64_t(y_top) * top_grid_size_ + x_top;
          /* A seam point is one source, however many grids hold a copy of it: its copies would map
           * onto the same targets and Add would count the displacement once per copy. */
          if (top_visible(grid, x_top, y_top) && is_alias_representative(elem) &&
              is_source_side(base_positions_[elem], params_))
          {
            sources_.append(elem);
          }
        }
      }
    }
    return !sources_.is_empty();
  }

  const Vector<int64_t> &sources() const
  {
    return sources_;
  }

  float3 position(const int64_t elem) const
  {
    return base_positions_[elem];
  }

  /**
   * Call \a fn for the target element and for every copy of it on a seam. The nearest search answers
   * with one of the copies; writing only that one leaves the others at their old value, and the surface
   * tears along the grid edge. Each copy gets its own pair, because its tangent frame is its grid's own.
   */
  template<typename FnT> void for_each_target(const int64_t dst, FnT fn) const
  {
    const int group = alias_group_[dst];
    if (group < 0) {
      fn(dst);
      return;
    }
    for (const int64_t elem : alias_groups_[group]) {
      fn(elem);
    }
  }

  /**
   * The visible element nearest to \a co, within tolerance and not \a exclude itself. The mirror of a
   * symmetric mesh sits on its target almost exactly, so a wider gap means the two sides do not
   * correspond here.
   */
  std::optional<int64_t> nearest(const float3 &co, const int64_t exclude) const
  {
    KDTreeNearest<float3> found;
    if (blender::kdtree_find_nearest(tree_, co, &found) < 0) {
      return std::nullopt;
    }
    if (found.index < 0 || int64_t(found.index) == exclude || found.dist > params_.tolerance) {
      return std::nullopt;
    }
    return int64_t(found.index);
  }

  /** One grid is one face-set block, so the filter is a per-grid answer. */
  void restrict_to_face_set(const int face_set_id)
  {
    if (face_set_id < 0 || face_set_.is_empty()) {
      return;
    }
    grid_in_face_set_ = Vector<bool>(subdiv_ccg_.grids_num, false);
    for (const int grid : IndexRange(subdiv_ccg_.grids_num)) {
      const int face = subdiv_ccg_.grid_to_face_map[grid];
      grid_in_face_set_[grid] = (face >= 0 && face < face_set_.size()) ?
                                    (face_set_[face] == face_set_id) :
                                    false;
    }
  }

  int face_set_at(const float3 &co) const
  {
    if (face_set_.is_empty()) {
      return -1;
    }
    const std::optional<int64_t> found = nearest(co, -1);
    if (!found) {
      return -1;
    }
    const int face = subdiv_ccg_.grid_to_face_map[int(*found / top_area_)];
    return (face >= 0 && face < face_set_.size()) ? face_set_[face] : -1;
  }

  bool in_face_set(const int64_t elem) const
  {
    return grid_in_face_set_.is_empty() ? true : grid_in_face_set_[int(elem / top_area_)];
  }

  /**
   * Re-express every draft's source delta in the target point's tangent basis.
   *
   * Frames are evaluated for the unique elements of the drafts in one
   * #BKE_multires_sculpt_layer_grid_frames_get call, and a zero source displacement is left out
   * altogether: mirrored into any basis it is still zero, which is the case that dominates a Replace
   * mapping of a side that was only sculpted in patches.
   */
  bool resolve(const Vector<MirrorDraft> &drafts, Vector<MirrorPair> &r_pairs)
  {
    Map<int64_t, int> frame_of;
    Vector<int64_t> frame_elems;
    for (const MirrorDraft &draft : drafts) {
      if (layer_data_[draft.src] == float3(0.0f)) {
        continue;
      }
      for (const int64_t elem : {draft.src, draft.dst}) {
        if (!frame_of.contains(elem)) {
          frame_of.add(elem, int(frame_elems.size()));
          frame_elems.append(elem);
        }
      }
    }

    Array<float3x3> tangent(frame_elems.size());
    Array<float3x3> inv_tangent(frame_elems.size());
    if (!frame_elems.is_empty()) {
      Vector<MultiresSculptLayerGridElem> requests;
      requests.reserve(frame_elems.size());
      for (const int64_t elem : frame_elems) {
        requests.append({int(elem / top_area_), int(elem % top_area_)});
      }
      if (!BKE_multires_sculpt_layer_grid_frames_get(
              base_mesh_, subdiv_ccg_, top_level_, requests, tangent))
      {
        return false;
      }
      for (const int64_t i : frame_elems.index_range()) {
        inv_tangent[i] = math::invert(tangent[i]);
      }
    }

    r_pairs.reserve(drafts.size());
    for (const MirrorDraft &draft : drafts) {
      const int64_t src_elem = draft.src;
      float3 dst_delta(0.0f);
      if (layer_data_[src_elem] != float3(0.0f)) {
        const int src_frame = frame_of.lookup(src_elem);
        const int dst_frame = frame_of.lookup(draft.dst);
        const float3 object_delta = math::transform_direction(tangent[src_frame],
                                                             layer_data_[src_elem]);
        const float3 mirrored = symmetry_flip(object_delta,
                                              ePaintSymmetryFlags(draft.symm_it));
        dst_delta = math::transform_direction(inv_tangent[dst_frame], mirrored);
      }
      r_pairs.append({draft.src,
                      draft.dst,
                      dst_delta,
                      weights_.at(draft.src),
                      in_face_set(draft.src)});
    }
    sort_pairs(r_pairs);
    return true;
  }

 private:
  int64_t elem_of(const SubdivCCGCoord &coord) const
  {
    /* One CCG point per layer element (`step_ == 1`), so a CCG coordinate is the element's own. */
    return int64_t(coord.grid_index) * top_area_ + int64_t(coord.y) * top_grid_size_ + coord.x;
  }

  void alias_join(const int64_t a, const int64_t b)
  {
    if (a == b) {
      return;
    }
    const int group_a = alias_group_[a];
    const int group_b = alias_group_[b];
    if (group_a < 0 && group_b < 0) {
      Vector<int64_t> group;
      group.append(a);
      group.append(b);
      alias_group_[a] = alias_group_[b] = int(alias_groups_.size());
      alias_groups_.append(std::move(group));
    }
    else if (group_a >= 0 && group_b < 0) {
      alias_group_[b] = group_a;
      alias_groups_[group_a].append(b);
    }
    else if (group_a < 0) {
      alias_group_[a] = group_b;
      alias_groups_[group_b].append(a);
    }
    else if (group_a != group_b) {
      for (const int64_t elem : alias_groups_[group_b]) {
        alias_group_[elem] = group_a;
        alias_groups_[group_a].append(elem);
      }
      alias_groups_[group_b].clear();
    }
  }

  /** Collect the seam duplicates the CCG's adjacency lists name, as the base flush does. */
  void alias_groups_build()
  {
    alias_group_ = Array<int>(base_positions_.size(), -1);
    for (const SubdivCCGAdjacentVertex &adjacent : subdiv_ccg_.adjacent_verts) {
      const Span<SubdivCCGCoord> coords = adjacent.corner_coords;
      for (const int64_t i : coords.index_range().drop_front(1)) {
        alias_join(elem_of(coords[0]), elem_of(coords[i]));
      }
    }
    /* Each adjacent-face record holds `grid_size * 2` coordinates, where `record[i]` and
     * `record[grid_size + i]` are the two grid views of one physical edge point; the ends are coarse
     * vertices, handled above. */
    for (const SubdivCCGAdjacentEdge &adjacent : subdiv_ccg_.adjacent_edges) {
      for (const Array<SubdivCCGCoord> &coords : adjacent.boundary_coords) {
        if (coords.size() < key_.grid_size * 2) {
          continue;
        }
        for (int i = 1; i < key_.grid_size - 1; i++) {
          alias_join(elem_of(coords[i]), elem_of(coords[key_.grid_size + i]));
        }
      }
    }
    for (Vector<int64_t> &group : alias_groups_) {
      std::sort(group.begin(), group.end());
    }
  }

  bool is_alias_representative(const int64_t elem) const
  {
    const int group = alias_group_[elem];
    return group < 0 || alias_groups_[group][0] == elem;
  }

  bool top_visible(const int grid, const int x_top, const int y_top) const
  {
    const BitGroupVector<> &grid_hidden = subdiv_ccg_.grid_hidden;
    if (grid_hidden.is_empty()) {
      return true;
    }
    return !grid_hidden[grid][(y_top * step_) * key_.grid_size + x_top * step_].test();
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Core: mapping and write
 * \{ */

/**
 * The mapping of one domain: for every source element, one pair per valid symmetry pass, whose target
 * is whatever the domain's spatial search answers for the flipped position.
 *
 * The symmetry passes, the side filters and the deterministic order are spelled here once; the
 * locators only answer where things are and how a delta changes basis.
 */
template<typename LocatorT>
static bool build_mapping(LocatorT &locator,
                          const MirrorParams &params,
                          Vector<MirrorPair> &r_pairs)
{
  if (!locator.valid() || !locator.collect_sources()) {
    return false;
  }
  locator.restrict_to_face_set(params.face_set_id);

  /* One draft vector per thread, merged afterwards: the nearest searches only read the KD-tree, and
   * #sort_pairs puts every destination's run in index order, so the merge order does not show up in
   * the result (see #sort_pairs). */
  threading::EnumerableThreadSpecific<Vector<MirrorDraft>> drafts_per_thread;
  threading::parallel_for(locator.sources().index_range(), 1024, [&](const IndexRange range) {
    Vector<MirrorDraft> &thread_drafts = drafts_per_thread.local();
    for (const int64_t src_index : range) {
      const int64_t src = locator.sources()[src_index];
      const float3 co = locator.position(src);
      for (int symm_it = 1; symm_it <= int(params.symm); symm_it++) {
        if (!is_symmetry_iteration_valid(symm_it, char(params.symm))) {
          continue;
        }
        const std::optional<int64_t> dst = locator.nearest(
            symmetry_flip(co, ePaintSymmetryFlags(symm_it)), src);
        if (!dst) {
          continue;
        }
        if (!is_target_side(locator.position(*dst), params)) {
          continue;
        }
        locator.for_each_target(*dst, [&](const int64_t target) {
          thread_drafts.append({src, target, symm_it});
        });
      }
    }
  });
  Vector<MirrorDraft> drafts;
  for (const Vector<MirrorDraft> &thread_drafts : drafts_per_thread) {
    drafts.extend(thread_drafts.as_span());
  }
  if (drafts.is_empty()) {
    return false;
  }
  return locator.resolve(drafts, r_pairs);
}

int mirror_mapping_apply(const MirrorParams &params,
                         const Span<MirrorPair> pairs,
                         MutableSpan<float3> data)
{
  const bool filter_face_set = (params.limit == Limit::FaceSet) && (params.face_set_id >= 0);

  int written = 0;
  int64_t i = 0;
  while (i < pairs.size()) {
    const int64_t dst = pairs[i].dst;
    int64_t j = i;
    if (params.mode == Mode::Replace) {
      /* One run per destination: the largest source of the run decides, so the result does not depend
       * on the order the spatial search returned. A run the face-set filter empties writes nothing,
       * which is what leaves a destination outside the copied set alone. */
      int64_t best = -1;
      for (; j < pairs.size() && pairs[j].dst == dst; ++j) {
        if (filter_face_set && !pairs[j].src_in_face_set) {
          continue;
        }
        if (best < 0 || pairs[j].src >= pairs[best].src) {
          best = j;
        }
      }
      if (best >= 0) {
        const float weight = params.use_mask ? pairs[best].src_weight : 1.0f;
        data[dst] = pairs[best].dst_delta * weight;
        written++;
      }
    }
    else {
      float3 sum(0.0f);
      bool any = false;
      for (; j < pairs.size() && pairs[j].dst == dst; ++j) {
        if (filter_face_set && !pairs[j].src_in_face_set) {
          continue;
        }
        /* A zero contribution is skipped rather than added, so a destination keeps its exact bits:
         * `x + 0.0f` is not `x` when `x` is `-0.0f`. */
        if (pairs[j].dst_delta == float3(0.0f)) {
          continue;
        }
        const float weight = params.use_mask ? pairs[j].src_weight : 1.0f;
        if (weight == 0.0f) {
          continue;
        }
        sum += pairs[j].dst_delta * weight;
        any = true;
      }
      if (any) {
        data[dst] += sum;
        written++;
      }
    }
    i = j;
  }

  return written;
}

bool mirror_mapping_build(Object &object,
                          const SculptLayer &layer,
                          MirrorParams &params,
                          const int64_t click_elem,
                          Vector<MirrorPair> &r_pairs)
{
  /* The vertex domain pairs on the runtime base, which only exists once the session state does. */
  session_state_ensure(object);

  Vector<MirrorPair> pairs;
  bool found = false;
  /* A click names an element, and the side is decided by that element's *base* position, in the same
   * space the pairs are found in; a script names the point directly. */
  auto settle_source_point = [&](const auto &locator) {
    if (click_elem >= 0) {
      params.source_point = locator.point_at(click_elem);
    }
    if (params.limit == Limit::FaceSet && params.face_set_id < 0) {
      params.face_set_id = locator.face_set_at(params.source_point);
    }
  };
  if (params.grids) {
    GridLocator locator(params, object, layer);
    if (locator.valid()) {
      settle_source_point(locator);
    }
    found = build_mapping(locator, params, pairs);
  }
  else {
    VertLocator locator(params, object, layer);
    if (locator.valid()) {
      settle_source_point(locator);
    }
    found = build_mapping(locator, params, pairs);
  }
  if (!found) {
    r_pairs.clear();
    return false;
  }
  r_pairs = std::move(pairs);
  return !r_pairs.is_empty();
}

/** \} */

}  // namespace blender::ed::sculpt_paint::layers::mirror_copy
