/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Draw-data builders for the Sculpt Lattice cage overlay (Plan 2). */

#include "ED_sculpt_lattice_draw.hh"

#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "DNA_curve_types.h" /* For #BPoint (full type behind #Lattice.def). */
#include "DNA_lattice_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph_query.hh"

#include "BLI_index_range.hh"
#include "BLI_math_matrix.hh"

#include "sculpt_lattice.hh"

namespace blender::ed::sculpt_paint::lattice {

void lattice_cage_edges_build(const int3 &res, Vector<int2> &r_edges)
{
  r_edges.clear();
  const int u_num = res.x, v_num = res.y, w_num = res.z;
  if (u_num < 1 || v_num < 1 || w_num < 1) {
    return;
  }
  /* Must stay identical to #BKE_lattice_index_from_uvw, which defines the #Lattice.def layout the
   * indices below refer to. Kept as a pure function of the resolution so the overlay layer does
   * not need the #Lattice type, and so it can be exercised on its own. */
  auto node_index = [&](const int u, const int v, const int w) -> int {
    return (w * (u_num * v_num) + (v * u_num) + u);
  };
  for (int w = 0; w < w_num; w++) {
    for (int v = 0; v < v_num; v++) {
      for (int u = 0; u < u_num; u++) {
        const int i = node_index(u, v, w);
        if (u + 1 < u_num) {
          r_edges.append(int2(i, node_index(u + 1, v, w)));
        }
        if (v + 1 < v_num) {
          r_edges.append(int2(i, node_index(u, v + 1, w)));
        }
        if (w + 1 < w_num) {
          r_edges.append(int2(i, node_index(u, v, w + 1)));
        }
      }
    }
  }
}

/* Resolves the tool state to draw for the active sculpt object, or nullptr when there is nothing
 * to draw. Single relevance check shared by both public entry points, so the cheap pre-check and
 * the builder can never disagree.
 *
 * The sculpt session lives on the ORIGINAL object; overlay sees the active object that may be an
 * evaluated copy, so map back with #DEG_get_original. */
static const LatticeToolData *lattice_drawable_state_get(const Object *object_active)
{
  if (object_active == nullptr) {
    return nullptr;
  }
  const Object *ob_orig = DEG_get_original(object_active);
  if (ob_orig == nullptr || ob_orig->type != OB_MESH) {
    return nullptr;
  }
  const SculptSession *ss = ob_orig->runtime->sculpt_session;
  if (ss == nullptr) {
    return nullptr;
  }
  const LatticeToolData *state = ss->lattice_tool_state;
  /* Draw in every live phase: the cage must stay visible while it is being placed, not only
   * while it deforms. */
  if (state == nullptr || state->phase == Phase::Inactive || state->lattice_ob == nullptr) {
    return nullptr;
  }
  return state;
}

bool ED_sculpt_lattice_cage_is_relevant(const Depsgraph * /*depsgraph*/,
                                        const Object *object_active)
{
  return lattice_drawable_state_get(object_active) != nullptr;
}

void ED_sculpt_lattice_cage_build(const Depsgraph * /*depsgraph*/,
                                  const Object *object_active,
                                  LatticeCageDrawData &r_out)
{
  r_out = {};
  const LatticeToolData *state = lattice_drawable_state_get(object_active);
  if (state == nullptr) {
    return;
  }
  const Object *lat_ob = state->lattice_ob;
  const Lattice *lt = id_cast<const Lattice *>(lat_ob->data);
  if (lt == nullptr || lt->def == nullptr) {
    return;
  }

  const int3 res(lt->pntsu, lt->pntsv, lt->pntsw);
  const int point_num = res.x * res.y * res.z;
  if (point_num <= 0) {
    return;
  }

  /* World-space control points. */
  const float4x4 &obmat = lat_ob->object_to_world();
  r_out.points.reserve(point_num);
  for (const int i : IndexRange(point_num)) {
    r_out.points.append(math::transform_point(obmat, float3(lt->def[i].vec)));
  }

  lattice_cage_edges_build(res, r_out.edges);
  r_out.resolution = res;
  r_out.active_point = state->pending_drag_index;
  r_out.placement_phase = (state->phase == Phase::Placement);
  r_out.valid = true;
}

}  // namespace blender::ed::sculpt_paint::lattice
