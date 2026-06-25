/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt Lattice Tool: deform pipeline.
 *
 * Implements:
 *  - sculpt_lattice_compute_deform_bounds  (variant A mask bbox)
 *  - sculpt_lattice_build_affected_region (snapshot per drag)
 *  - sculpt_lattice_deform_apply           (live deform via PositionDeformData)
 *
 * See .My_Docs_July_2026/Sculpt-Mode/Lattice-Tool/Plan_1/05_deform_pipeline.md
 */

#include "sculpt_lattice.hh"
#include "sculpt_lattice_intern.hh"

#include "sculpt_intern.hh"

#include <algorithm>
#include <cstdio> /* TEMP DEBUG: live-preview diagnostics, remove once resolved. */
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_bounds_types.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
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
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob_mesh);

  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    /* Phase 3: grids / bmesh. For MVP, refuse. */
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob_mesh.data);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh);
  if (positions.is_empty()) {
    return false;
  }

  const VArraySpan mask = *mesh.attributes().lookup<float>(".sculpt_mask",
                                                           bke::AttrDomain::Point);

  Bounds<float3> bounds = Bounds<float3>(positions[0]);
  bool any = false;

  if (mask.is_empty()) {
    /* No mask — entire mesh is deformable. */
    for (const float3 &p : positions) {
      math::min_max(p, bounds.min, bounds.max);
    }
    any = true;
  }
  else {
    for (const int i : positions.index_range()) {
      if ((1.0f - mask[i]) <= mask_eps) {
        continue;
      }
      any = true;
      math::min_max(positions[i], bounds.min, bounds.max);
    }
  }

  if (!any) {
    /* All vertices masked — nothing to deform. */
    return false;
  }

  r_bounds = bounds;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Build affected region (snapshot at slide start)
 * \{ */

/**
 * Rebuilds the affected vert list for the current cage:
 *  - all verts inside the (evaluated) cage bbox in object-space of the mesh
 *  - with `(1 - mask) > mask_eps`
 *
 * Captures rest_coords (the original, undeformed positions) and a mask snapshot per vert.
 * Built ONCE at session start: the cage is the single deformation accumulator and the mesh is
 * always recomputed as `lattice(rest)`, so rest must stay the original positions across drags.
 */
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

  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob_mesh);
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply live deformation (MOUSEMOVE)
 * \{ */

/**
 * Applies the lattice deformation to all affected verts, blending by mask:
 *   new_co = rest_co + (deformed_co - rest_co) * (strength * (1 - mask))
 * `rest_co` is the snapshot from the start of the current drag (accumulative).
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

  Vector<float3> translations(ar.verts.size());
  for (const int i : ar.verts.index_range()) {
    const float weight = state.strength * (1.0f - ar.mask[i]);
    if (weight <= 0.0f) {
      translations[i] = float3(0.0f);
      continue;
    }
    float deformed[3];
    copy_v3_v3(deformed, ar.rest_coords[i]);
    /* BKE_lattice_deform_data_eval_co mutates `co` in place. It interprets `co` as
     * object-space of the *target* mesh (the second arg passed to deform_data_create). */
    BKE_lattice_deform_data_eval_co(state.deform_data, deformed, 1.0f);
    const float3 target = math::interpolate(ar.rest_coords[i], float3(deformed), weight);
    /* The lattice deform is an absolute rest -> target mapping. #PositionDeformData::deform is
     * incremental, so move from the last position *this tool* wrote (tracked below) to `target`.
     * Using #PositionDeformData::eval here instead diverges, because the forced re-eval stopgap
     * desynchronises `eval` from the mesh (see #AffectedRegion::current_coords). */
    translations[i] = target - ar.current_coords[i];
    ar.current_coords[i] = target;
  }

  /* TEMP DEBUG: report whether the deform produced any motion at all, plus the interpolation
   * type actually in effect at deform time (KEY_LINEAR=0, KEY_CARDINAL=1, KEY_BSPLINE=2,
   * KEY_CATMULL_ROM=3). Settles whether a "B-Spline" UI selection truly reaches the deformer. */
  float max_disp = 0.0f;
  for (const float3 &t : translations) {
    max_disp = std::max(max_disp, math::length(t));
  }
  const Lattice *lt_dbg = state.lattice_ob ? id_cast<const Lattice *>(state.lattice_ob->data) :
                                             nullptr;
  printf("[lattice] deform_apply: verts=%d max_disp=%.6f typeu/v/w=%d/%d/%d pnts=%d/%d/%d\n",
         int(ar.verts.size()),
         max_disp,
         lt_dbg ? int(lt_dbg->typeu) : -1,
         lt_dbg ? int(lt_dbg->typev) : -1,
         lt_dbg ? int(lt_dbg->typew) : -1,
         lt_dbg ? int(lt_dbg->pntsu) : -1,
         lt_dbg ? int(lt_dbg->pntsv) : -1,
         lt_dbg ? int(lt_dbg->pntsw) : -1);
  fflush(stdout);

  position_data.deform(translations, ar.verts);

  /* Tag affected PBVH nodes so normals / bounds are recomputed. */
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob_mesh);
  if (pbvh.type() == bke::pbvh::Type::Mesh) {
    /* Build a vert set for O(1) lookup, then select nodes whose verts intersect it. */
    const int verts_num = bke::pbvh::vert_positions_eval(depsgraph, ob_mesh).size();
    Array<bool> affected(verts_num, false);
    for (const int v : ar.verts) {
      affected[v] = true;
    }
    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::search_nodes(
        pbvh, memory, [&](const bke::pbvh::Node &node) {
          const auto &mesh_node = static_cast<const bke::pbvh::MeshNode &>(node);
          for (const int nv : mesh_node.all_verts()) {
            if (affected[nv]) {
              return true;
            }
          }
          return false;
        });
    pbvh.tag_positions_changed(node_mask);
    pbvh.update_bounds(depsgraph, ob_mesh);
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::lattice
