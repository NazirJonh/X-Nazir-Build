/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt Lattice Tool: deform pipeline.
 *
 * Implements:
 *  - #sculpt_lattice_compute_deform_bounds  (variant A mask bbox)
 *  - #sculpt_lattice_build_affected_region  (session-entry snapshot)
 *  - #sculpt_lattice_sync_tracker_to_mesh   (re-seed the tracker before each drag)
 *  - #sculpt_lattice_compute_translations   (pure per-vertex math)
 *  - #sculpt_lattice_deform_apply           (live deform via #PositionDeformData)
 *
 * See .My_Docs_July_2026/Sculpt-Mode/Lattice-Tool/Plan_1/05_deform_pipeline.md
 */

#include "sculpt_lattice.hh"
#include "sculpt_lattice_intern.hh"

#include "sculpt_intern.hh"

#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_bounds_types.hh"
#include "BLI_index_mask.hh"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_lattice.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "DNA_lattice_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

namespace blender::ed::sculpt_paint::lattice {

LatticeDeformData *sculpt_lattice_deform_data_rebuild(Object *lat_ob, Object *mesh_ob)
{
  /* Drop evaluated displists so deform reads live #Lattice.def (see BKE_lattice_resize).
   * The temp cage is no-main and typically has no curve cache; skip the full derived-cache
   * walk in that case. */
  if (lat_ob->runtime->curve_cache) {
    BKE_object_free_derived_caches(lat_ob);
  }

  /* #Object::object_to_world() returns the runtime matrix, which is refreshed by the dependency
   * graph — not from loc/scale/rot directly. #sculpt_lattice_fit_temp_to_bounds writes only to
   * #Object.loc / #Object.scale; without a depsgraph re-eval the runtime matrix stays stale, so
   * #BKE_lattice_deform_data_create builds `latmat` from an outdated (often identity) matrix.
   * Mesh vertices then map outside the cage in deform space and the deformation evaluates to
   * ~zero — i.e. no live preview. Recompute the runtime matrix from the DNA transform. */
  BKE_object_to_mat4(lat_ob, lat_ob->runtime->object_to_world.ptr());

  return BKE_lattice_deform_data_create_from_lattice(
      id_cast<const Lattice *>(lat_ob->data),
      lat_ob->object_to_world().ptr(),
      mesh_ob->object_to_world().ptr());
}

void sculpt_lattice_evaluator_reset(LatticeToolData &state)
{
  if (state.deform_data) {
    BKE_lattice_deform_data_destroy(state.deform_data);
    state.deform_data = nullptr;
  }
}

void sculpt_lattice_evaluator_rebuild(LatticeToolData &state, Object &mesh_ob)
{
  sculpt_lattice_evaluator_reset(state);
  if (state.lattice_ob) {
    state.deform_data = sculpt_lattice_deform_data_rebuild(state.lattice_ob, &mesh_ob);
  }
}

void sculpt_lattice_evaluator_ensure(LatticeToolData &state, Object &mesh_ob)
{
  if (!state.deform_data) {
    sculpt_lattice_evaluator_rebuild(state, mesh_ob);
  }
}

void sculpt_lattice_evaluator_update_point(LatticeToolData &state, const int index)
{
  if (state.deform_data) {
    BKE_lattice_deform_data_update_point(state.deform_data, index);
  }
}

/* -------------------------------------------------------------------- */
/** \name Compute cage bounds (variant A mask bbox)
 * \{ */

/**
 * Axis-aligned bbox of vertices that will deform: those with `(1 - mask) > mask_eps`.
 * If no mask attribute exists, the whole mesh bounds are used (variant A, ADR-11).
 * Mesh PBVH only in MVP (grids / bmesh: phase 3).
 */
bool sculpt_lattice_compute_deform_bounds(const Depsgraph &depsgraph,
                                          const Object &ob_mesh,
                                          const float mask_eps,
                                          std::optional<Bounds<float3>> &r_bounds)
{
  /* Null or a non-Mesh tree (multires / dyntopo — phase 3). The caller is responsible for having
   * run #sculpt_lattice_pbvh_ensure first. */
  if (sculpt_lattice_pbvh_find(ob_mesh) == nullptr) {
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob_mesh.data);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh);
  if (positions.is_empty()) {
    return false;
  }

  const VArraySpan mask = *mesh.attributes().lookup<float>(".sculpt_mask",
                                                           bke::AttrDomain::Point);

  if (mask.is_empty()) {
    /* No mask — entire mesh is deformable. */
    r_bounds = bounds::min_max(positions);
    return r_bounds.has_value();
  }

  /* Seed from the first *unmasked* vertex. Seeding from `positions[0]` unconditionally would keep
   * vertex 0 inside the bounds even when it is masked, which stretches the cage over the whole
   * object whenever vertex 0 sits on an extremum (true of most primitives). */
  std::optional<Bounds<float3>> unmasked_bounds;
  for (const int i : positions.index_range()) {
    if ((1.0f - mask[i]) <= mask_eps) {
      continue;
    }
    if (unmasked_bounds.has_value()) {
      math::min_max(positions[i], unmasked_bounds->min, unmasked_bounds->max);
    }
    else {
      unmasked_bounds = Bounds<float3>(positions[i]);
    }
  }

  if (!unmasked_bounds.has_value()) {
    /* All vertices masked — nothing to deform. */
    return false;
  }

  r_bounds = unmasked_bounds;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Build affected region (snapshot at session start / phase transition)
 * \{ */

static Bounds<float3> sculpt_lattice_transform_bounds(const float4x4 &mat,
                                                      const Bounds<float3> &src)
{
  Bounds<float3> dst(math::transform_point(mat, src.min));
  const float3 corners[7] = {
      {src.max.x, src.min.y, src.min.z},
      {src.min.x, src.max.y, src.min.z},
      {src.min.x, src.min.y, src.max.z},
      {src.max.x, src.max.y, src.min.z},
      {src.max.x, src.min.y, src.max.z},
      {src.min.x, src.max.y, src.max.z},
      src.max,
  };
  for (const float3 &corner : corners) {
    math::min_max(math::transform_point(mat, corner), dst.min, dst.max);
  }
  return dst;
}

static bool sculpt_lattice_vert_in_cage(const float3 &mesh_co,
                                        const float4x4 &mesh_to_lattice,
                                        const float3 &bmin,
                                        const float3 &bmax)
{
  const float3 local = math::transform_point(mesh_to_lattice, mesh_co);
  return !(local.x < bmin.x || local.x > bmax.x || local.y < bmin.y || local.y > bmax.y ||
           local.z < bmin.z || local.z > bmax.z);
}

static void sculpt_lattice_session_orig_capture(LatticeToolData &state)
{
  SessionOrigSnapshot &snap = state.session_orig;
  for (const AffectedNode &node : state.current.nodes) {
    if (snap.node_set.add(node.index)) {
      snap.node_indices.append(node.index);
    }
    for (const int i : node.verts.index_range()) {
      const int v = node.verts[i];
      if (snap.vert_set.add(v)) {
        snap.verts.append(v);
        snap.positions.append(node.rest_coords[i]);
      }
    }
  }
}

IndexMask sculpt_lattice_affected_node_mask(const AffectedRegion &ar, IndexMaskMemory &memory)
{
  if (ar.node_indices.is_empty()) {
    return {};
  }
  return IndexMask::from_indices(ar.node_indices.as_span(), memory);
}

static void sculpt_lattice_affected_region_clear(AffectedRegion &ar)
{
  ar.nodes.clear();
  ar.node_indices.clear();
  ar.pbvh = nullptr;
  ar.pbvh_nodes_num = -1;
}

void sculpt_lattice_build_affected_region(const Depsgraph &depsgraph,
                                          Object &ob_mesh,
                                          LatticeToolData &state)
{
  AffectedRegion &ar = state.current;
  sculpt_lattice_affected_region_clear(ar);

  if (!state.lattice_ob) {
    return;
  }

  bke::pbvh::Tree *pbvh = sculpt_lattice_pbvh_find(ob_mesh);
  if (pbvh == nullptr) {
    return;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob_mesh.data);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh);
  state.mesh_verts_num = int(positions.size());
  const VArraySpan mask = *mesh.attributes().lookup<float>(".sculpt_mask",
                                                           bke::AttrDomain::Point);

  const Lattice *lt = id_cast<const Lattice *>(state.lattice_ob->data);
  const std::optional<Bounds<float3>> lt_bounds = BKE_lattice_minmax(lt);
  if (!lt_bounds.has_value()) {
    return;
  }

  /* Mesh vertex (object-space) -> lattice data-space (same as #Lattice.def). */
  const float4x4 mesh_to_lattice = math::invert(state.lattice_ob->object_to_world()) *
                                   ob_mesh.object_to_world();
  const float4x4 lattice_to_mesh = math::invert(mesh_to_lattice);
  const Bounds<float3> cage_mesh = sculpt_lattice_transform_bounds(lattice_to_mesh, *lt_bounds);

  const float3 &bmin = lt_bounds->min;
  const float3 &bmax = lt_bounds->max;

  IndexMaskMemory memory;
  const IndexMask candidates = bke::pbvh::search_nodes(
      *pbvh, memory, [&](const bke::pbvh::Node &node) {
        return node.bounds().intersects(cage_mesh);
      });

  const Span<bke::pbvh::MeshNode> mesh_nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  candidates.foreach_index([&](const int node_i) {
    const bke::pbvh::MeshNode &mesh_node = mesh_nodes[node_i];
    Vector<int> verts;
    Vector<float3> rest;
    Vector<float> vert_mask;
    for (const int v : mesh_node.verts()) {
      if (!sculpt_lattice_vert_in_cage(positions[v], mesh_to_lattice, bmin, bmax)) {
        continue;
      }
      const float v_mask = mask.is_empty() ? 0.0f : mask[v];
      if ((1.0f - v_mask) <= state.mask_eps) {
        continue;
      }
      verts.append(v);
      rest.append(positions[v]);
      vert_mask.append(v_mask);
    }
    if (verts.is_empty()) {
      return;
    }
    ar.nodes.append({});
    AffectedNode &dst = ar.nodes.last();
    dst.index = node_i;
    dst.verts = std::move(verts);
    dst.rest_coords = Array<float3>(rest.as_span());
    dst.current_coords = dst.rest_coords;
    dst.mask = Array<float>(vert_mask.as_span());
    ar.node_indices.append(node_i);
  });

  ar.pbvh = pbvh;
  ar.pbvh_nodes_num = pbvh->nodes_num();

  sculpt_lattice_session_orig_capture(state);
}

bool sculpt_lattice_sync_tracker_to_mesh(const Depsgraph &depsgraph,
                                         const Object &ob_mesh,
                                         LatticeToolData &state)
{
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh);
  if (int(positions.size()) != state.mesh_verts_num) {
    return false;
  }

  for (AffectedNode &node : state.current.nodes) {
    for (const int i : node.verts.index_range()) {
      node.current_coords[i] = positions[node.verts[i]];
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply live deformation (MOUSEMOVE)
 * \{ */

bool sculpt_lattice_compute_translations(LatticeDeformData &deform_data,
                                         const Span<float3> rest_coords,
                                         const Span<float> mask,
                                         const float strength,
                                         MutableSpan<float3> current_coords,
                                         MutableSpan<float3> r_translations)
{
  BLI_assert(rest_coords.size() == mask.size());
  BLI_assert(rest_coords.size() == current_coords.size());
  BLI_assert(rest_coords.size() == r_translations.size());

  /* #BKE_lattice_deform_data_eval_co only reads from the context, so the same grain size the
   * kernel's own lattice deform uses applies here (see #BKE_lattice_deform_coords_impl). */
  threading::parallel_for(rest_coords.index_range(), 512, [&](const IndexRange range) {
    for (const int i : range) {
      const float weight = strength * (1.0f - mask[i]);
      float3 target = rest_coords[i];
      if (weight > 0.0f) {
        float deformed[3];
        copy_v3_v3(deformed, rest_coords[i]);
        /* #BKE_lattice_deform_data_eval_co mutates `co` in place. It interprets `co` as
         * object-space of the *target* mesh (the second arg passed to deform_data_create). */
        BKE_lattice_deform_data_eval_co(&deform_data, deformed, 1.0f);
        target = math::interpolate(rest_coords[i], float3(deformed), weight);
      }
      /* Absolute rest -> target mapping turned into the increment #PositionDeformData::deform
       * expects, relative to the last position this tool wrote. Weight 0 still targets rest so a
       * live Strength change can roll the mesh back. */
      r_translations[i] = target - current_coords[i];
      current_coords[i] = target;
    }
  });

  for (const float3 &delta : r_translations) {
    if (!math::is_zero(delta)) {
      return true;
    }
  }
  return false;
}

bool sculpt_lattice_deform_would_change(LatticeToolData &state)
{
  AffectedRegion &ar = state.current;
  if (ar.is_empty() || !state.deform_data) {
    return false;
  }

  int max_verts = 0;
  for (const AffectedNode &node : ar.nodes) {
    max_verts = math::max(max_verts, int(node.verts.size()));
  }
  if (state.translations.size() < max_verts) {
    state.translations.reinitialize(max_verts);
  }

  for (const AffectedNode &node : ar.nodes) {
    const MutableSpan<float3> trans = state.translations.as_mutable_span().take_front(
        node.verts.size());
    for (const int i : node.verts.index_range()) {
      const float weight = state.strength * (1.0f - node.mask[i]);
      float3 target = node.rest_coords[i];
      if (weight > 0.0f) {
        float deformed[3];
        copy_v3_v3(deformed, node.rest_coords[i]);
        BKE_lattice_deform_data_eval_co(state.deform_data, deformed, 1.0f);
        target = math::interpolate(node.rest_coords[i], float3(deformed), weight);
      }
      trans[i] = target - node.current_coords[i];
      if (!math::is_zero(trans[i])) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Applies the lattice deformation to all affected verts, blending by mask:
 *   new_co = rest_co + (deformed_co - rest_co) * (strength * (1 - mask))
 * `rest_co` is the deform-phase snapshot; the cage accumulates across drags.
 * Writes through PositionDeformData (same as Mesh Filter, ADR-6).
 * Returns false when every translation is zero so callers skip mesh writes and PBVH work.
 */
bool sculpt_lattice_deform_apply(const Depsgraph &depsgraph,
                                 Object &ob_mesh,
                                 LatticeToolData &state)
{
  AffectedRegion &ar = state.current;
  if (ar.is_empty() || !state.deform_data) {
    return false;
  }

  sculpt_lattice_ensure_affected_nodes(depsgraph, ob_mesh, ar);

  int max_verts = 0;
  for (const AffectedNode &node : ar.nodes) {
    max_verts = math::max(max_verts, int(node.verts.size()));
  }
  if (state.translations.size() < max_verts) {
    state.translations.reinitialize(max_verts);
  }

  std::optional<PositionDeformData> position_data;
  bool changed = false;
  for (AffectedNode &node : ar.nodes) {
    MutableSpan<float3> trans = state.translations.as_mutable_span().take_front(node.verts.size());
    if (!sculpt_lattice_compute_translations(*state.deform_data,
                                             node.rest_coords,
                                             node.mask,
                                             state.strength,
                                             node.current_coords,
                                             trans))
    {
      continue;
    }
    if (!position_data.has_value()) {
      position_data.emplace(depsgraph, ob_mesh);
    }
    position_data->deform(trans, node.verts);
    changed = true;
  }

  if (changed) {
    sculpt_lattice_tag_affected_nodes(depsgraph, ob_mesh, ar);
    state.session_has_mesh_changes = true;
  }
  return changed;
}

static void sculpt_lattice_rebind_nodes(const Depsgraph &depsgraph,
                                        Object &ob_mesh,
                                        AffectedRegion &ar)
{
  Vector<int> verts;
  Vector<float3> rest;
  Vector<float3> current;
  Vector<float> mask;
  for (const AffectedNode &node : ar.nodes) {
    verts.extend(node.verts);
    rest.extend(node.rest_coords.as_span());
    current.extend(node.current_coords.as_span());
    mask.extend(node.mask.as_span());
  }

  Map<int, int> vert_to_flat;
  vert_to_flat.reserve(verts.size());
  for (const int i : verts.index_range()) {
    vert_to_flat.add(verts[i], i);
  }

  sculpt_lattice_affected_region_clear(ar);

  bke::pbvh::Tree *pbvh = sculpt_lattice_pbvh_find(ob_mesh);
  if (pbvh == nullptr || verts.is_empty()) {
    ar.pbvh = pbvh;
    ar.pbvh_nodes_num = pbvh ? pbvh->nodes_num() : -1;
    return;
  }

  const Span<bke::pbvh::MeshNode> mesh_nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  IndexMaskMemory memory;
  const IndexMask leaves = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  leaves.foreach_index([&](const int node_i) {
    const bke::pbvh::MeshNode &mesh_node = mesh_nodes[node_i];
    Vector<int> node_verts;
    Vector<float3> node_rest;
    Vector<float3> node_current;
    Vector<float> node_mask;
    for (const int v : mesh_node.verts()) {
      const int *flat = vert_to_flat.lookup_ptr(v);
      if (flat == nullptr) {
        continue;
      }
      node_verts.append(v);
      node_rest.append(rest[*flat]);
      node_current.append(current[*flat]);
      node_mask.append(mask[*flat]);
    }
    if (node_verts.is_empty()) {
      return;
    }
    ar.nodes.append({});
    AffectedNode &dst = ar.nodes.last();
    dst.index = node_i;
    dst.verts = std::move(node_verts);
    dst.rest_coords = Array<float3>(node_rest.as_span());
    dst.current_coords = Array<float3>(node_current.as_span());
    dst.mask = Array<float>(node_mask.as_span());
    ar.node_indices.append(node_i);
  });

  ar.pbvh = pbvh;
  ar.pbvh_nodes_num = pbvh->nodes_num();
  (void)depsgraph;
}

void sculpt_lattice_ensure_affected_nodes(const Depsgraph &depsgraph,
                                          Object &ob_mesh,
                                          AffectedRegion &ar)
{
  bke::pbvh::Tree *pbvh = sculpt_lattice_pbvh_find(ob_mesh);
  if (pbvh == nullptr) {
    if (!ar.is_empty()) {
      sculpt_lattice_affected_region_clear(ar);
    }
    return;
  }
  if (ar.pbvh == pbvh && ar.pbvh_nodes_num == pbvh->nodes_num()) {
    return;
  }
  sculpt_lattice_rebind_nodes(depsgraph, ob_mesh, ar);
}

void sculpt_lattice_tag_affected_nodes(const Depsgraph &depsgraph,
                                       Object &ob_mesh,
                                       AffectedRegion &ar)
{
  if (ar.is_empty()) {
    return;
  }

  bke::pbvh::Tree *pbvh = sculpt_lattice_pbvh_find(ob_mesh);
  if (pbvh == nullptr) {
    return;
  }

  sculpt_lattice_ensure_affected_nodes(depsgraph, ob_mesh, ar);

  IndexMaskMemory memory;
  const IndexMask node_mask = sculpt_lattice_affected_node_mask(ar, memory);
  pbvh->tag_positions_changed(node_mask);
  pbvh->update_bounds(depsgraph, ob_mesh);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::lattice
