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

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_bounds_types.hh"
#include "BLI_index_mask.hh"
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
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "DNA_lattice_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

namespace blender::ed::sculpt_paint::lattice {

LatticeDeformData *sculpt_lattice_deform_data_rebuild(Object *lat_ob, Object *mesh_ob)
{
  /* Drop evaluated displists so deform reads live #Lattice.def (see BKE_lattice_resize). */
  BKE_object_free_derived_caches(lat_ob);

  /* #Object::object_to_world() returns the runtime matrix, which is refreshed by the dependency
   * graph — not from loc/scale/rot directly. #sculpt_lattice_fit_temp_to_bounds writes only to
   * #Object.loc / #Object.scale; without a depsgraph re-eval the runtime matrix stays stale, so
   * #BKE_lattice_deform_data_create builds `latmat` from an outdated (often identity) matrix.
   * Mesh vertices then map outside the cage in deform space and the deformation evaluates to
   * ~zero — i.e. no live preview. Recompute the runtime matrix from the DNA transform. */
  BKE_object_to_mat4(lat_ob, lat_ob->runtime->object_to_world.ptr());

  return BKE_lattice_deform_data_create(lat_ob, mesh_ob);
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

void sculpt_lattice_build_affected_region(const Depsgraph &depsgraph,
                                          Object &ob_mesh,
                                          LatticeToolData &state)
{
  AffectedRegion &ar = state.current;
  ar.rest_coords = {};
  ar.current_coords = {};
  ar.verts.clear();
  ar.mask = {};

  if (!state.lattice_ob) {
    return;
  }

  if (sculpt_lattice_pbvh_find(ob_mesh) == nullptr) {
    return;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob_mesh.data);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh);
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

  const float3 &bmin = lt_bounds->min;
  const float3 &bmax = lt_bounds->max;

  for (const int i : positions.index_range()) {
    const float3 local = math::transform_point(mesh_to_lattice, positions[i]);
    if (local.x < bmin.x || local.x > bmax.x || local.y < bmin.y || local.y > bmax.y ||
        local.z < bmin.z || local.z > bmax.z)
    {
      continue;
    }
    const float v_mask = mask.is_empty() ? 0.0f : mask[i];
    if ((1.0f - v_mask) <= state.mask_eps) {
      continue;
    }
    ar.verts.append(i);
  }

  const int n = int(ar.verts.size());
  ar.rest_coords.reinitialize(n);
  ar.mask.reinitialize(n);
  for (const int j : ar.verts.index_range()) {
    const int v = ar.verts[j];
    ar.rest_coords[j] = positions[v];
    ar.mask[j] = mask.is_empty() ? 0.0f : mask[v];
  }

  /* Seed the applied-position tracker with the rest snapshot: nothing is deformed yet. */
  ar.current_coords = ar.rest_coords;
}

bool sculpt_lattice_sync_tracker_to_mesh(const Depsgraph &depsgraph,
                                         const Object &ob_mesh,
                                         LatticeToolData &state)
{
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh);
  if (positions.size() != state.entry_positions.size()) {
    return false;
  }

  AffectedRegion &ar = state.current;
  for (const int i : ar.verts.index_range()) {
    ar.current_coords[i] = positions[ar.verts[i]];
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply live deformation (MOUSEMOVE)
 * \{ */

void sculpt_lattice_compute_translations(LatticeDeformData &deform_data,
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
      if (weight <= 0.0f) {
        r_translations[i] = float3(0.0f);
        continue;
      }
      float deformed[3];
      copy_v3_v3(deformed, rest_coords[i]);
      /* #BKE_lattice_deform_data_eval_co mutates `co` in place. It interprets `co` as
       * object-space of the *target* mesh (the second arg passed to deform_data_create). */
      BKE_lattice_deform_data_eval_co(&deform_data, deformed, 1.0f);
      const float3 target = math::interpolate(rest_coords[i], float3(deformed), weight);
      /* Absolute rest -> target mapping turned into the increment #PositionDeformData::deform
       * expects, relative to the last position this tool wrote (see
       * #AffectedRegion::current_coords). */
      r_translations[i] = target - current_coords[i];
      current_coords[i] = target;
    }
  });
}

/**
 * Applies the lattice deformation to all affected verts, blending by mask:
 *   new_co = rest_co + (deformed_co - rest_co) * (strength * (1 - mask))
 * `rest_co` is the session-entry snapshot; the cage accumulates across drags.
 * Writes through PositionDeformData (same as Mesh Filter, ADR-6).
 */
void sculpt_lattice_deform_apply(const Depsgraph &depsgraph,
                                 Object &ob_mesh,
                                 LatticeToolData &state)
{
  AffectedRegion &ar = state.current;
  if (ar.verts.is_empty() || !state.deform_data) {
    return;
  }

  /* `PositionDeformData` is declared in sculpt_intern.hh (full type in editors). */
  const PositionDeformData position_data(depsgraph, ob_mesh);

  Array<float3> translations(ar.verts.size());
  sculpt_lattice_compute_translations(*state.deform_data,
                                      ar.rest_coords,
                                      ar.mask,
                                      state.strength,
                                      ar.current_coords,
                                      translations);

  position_data.deform(translations, ar.verts);

  sculpt_lattice_tag_affected_nodes(depsgraph, ob_mesh, ar);
}

void sculpt_lattice_tag_affected_nodes(const Depsgraph &depsgraph,
                                       Object &ob_mesh,
                                       const AffectedRegion &ar)
{
  if (ar.verts.is_empty()) {
    return;
  }

  /* Tag affected PBVH nodes so normals / bounds are recomputed. */
  bke::pbvh::Tree *pbvh = sculpt_lattice_pbvh_find(ob_mesh);
  if (pbvh == nullptr) {
    return;
  }

  /* Build a vert set for O(1) lookup, then select nodes whose verts intersect it. */
  const int verts_num = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh).size();
  Array<bool> affected(verts_num, false);
  for (const int v : ar.verts) {
    affected[v] = true;
  }
  /* Filter the flat node array rather than using #bke::pbvh::search_nodes: that traversal also
   * evaluates the predicate on inner BVH nodes and prunes their whole subtree when it fails.
   * Only leaf nodes carry vertex indices, so the predicate rejects the root and the search
   * returns an empty mask — nothing is tagged and the viewport keeps drawing the pre-deform
   * positions until something else forces a full re-evaluation. */
  const Span<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_predicate(
      nodes.index_range(), memory, [&](const int i) {
        for (const int nv : nodes[i].all_verts()) {
          if (affected[nv]) {
            return true;
          }
        }
        return false;
      });
  pbvh->tag_positions_changed(node_mask);
  pbvh->update_bounds(depsgraph, ob_mesh);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::lattice
