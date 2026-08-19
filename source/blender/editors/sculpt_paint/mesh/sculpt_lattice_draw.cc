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
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_vector.hh"

#include "sculpt_lattice.hh"

namespace blender::ed::sculpt_paint::lattice {

void lattice_cage_overlay_topology_build(const int3 &res,
                                         const bool outer_shell_only,
                                         Vector<int> &r_point_indices,
                                         Vector<int2> &r_edges)
{
  r_point_indices.clear();
  r_edges.clear();
  const int u_num = res.x, v_num = res.y, w_num = res.z;
  if (u_num < 1 || v_num < 1 || w_num < 1) {
    return;
  }

  const int point_num = u_num * v_num * w_num;
  auto node_index = [&](const int u, const int v, const int w) -> int {
    return (w * (u_num * v_num) + (v * u_num) + u);
  };
  auto on_shell = [&](const int u, const int v, const int w) -> bool {
    if (!outer_shell_only) {
      return true;
    }
    return u == 0 || u == u_num - 1 || v == 0 || v == v_num - 1 || w == 0 || w == w_num - 1;
  };

  Vector<int> compact(point_num, -1);
  const int interior = math::max(0, u_num - 2) * math::max(0, v_num - 2) * math::max(0, w_num - 2);
  r_point_indices.reserve(outer_shell_only ? (point_num - interior) : point_num);
  for (int w = 0; w < w_num; w++) {
    for (int v = 0; v < v_num; v++) {
      for (int u = 0; u < u_num; u++) {
        if (!on_shell(u, v, w)) {
          continue;
        }
        const int i = node_index(u, v, w);
        compact[i] = r_point_indices.size();
        r_point_indices.append(i);
      }
    }
  }

  for (int w = 0; w < w_num; w++) {
    for (int v = 0; v < v_num; v++) {
      for (int u = 0; u < u_num; u++) {
        const int i = node_index(u, v, w);
        if (compact[i] < 0) {
          continue;
        }
        if (u + 1 < u_num) {
          const int n = node_index(u + 1, v, w);
          if (compact[n] >= 0) {
            r_edges.append(int2(compact[i], compact[n]));
          }
        }
        if (v + 1 < v_num) {
          const int n = node_index(u, v + 1, w);
          if (compact[n] >= 0) {
            r_edges.append(int2(compact[i], compact[n]));
          }
        }
        if (w + 1 < w_num) {
          const int n = node_index(u, v, w + 1);
          if (compact[n] >= 0) {
            r_edges.append(int2(compact[i], compact[n]));
          }
        }
      }
    }
  }
}

void lattice_cage_edges_build(const int3 &res, Vector<int2> &r_edges)
{
  Vector<int> unused;
  lattice_cage_overlay_topology_build(res, false, unused, r_edges);
}

static bool lattice_cage_overlay_use_shell(const int3 &res)
{
  return math::max(res.x, math::max(res.y, res.z)) > SCULPT_LATTICE_OVERLAY_FULL_RES_MAX;
}

static void lattice_cage_overlay_topology_ensure(LatticeToolData &state, const int3 &res)
{
  const bool shell = lattice_cage_overlay_use_shell(res);
  if (state.overlay_edges_res == res && state.overlay_edges_shell == shell &&
      !state.overlay_point_indices.is_empty())
  {
    return;
  }
  lattice_cage_overlay_topology_build(
      res, shell, state.overlay_point_indices, state.overlay_edges);
  state.overlay_edges_res = res;
  state.overlay_edges_shell = shell;
}

/* Resolves the tool state to draw for the active sculpt object, or nullptr when there is nothing
 * to draw. Single relevance check shared by both public entry points, so the cheap pre-check and
 * the builder can never disagree.
 *
 * The sculpt session lives on the ORIGINAL object; overlay sees the active object that may be an
 * evaluated copy, so map back with #DEG_get_original. */
static LatticeToolData *lattice_drawable_state_get(const Object *object_active)
{
  if (object_active == nullptr) {
    return nullptr;
  }
  const Object *ob_orig = DEG_get_original(object_active);
  if (ob_orig == nullptr || ob_orig->type != OB_MESH) {
    return nullptr;
  }
  SculptSession *ss = ob_orig->runtime->sculpt_session;
  if (ss == nullptr) {
    return nullptr;
  }
  LatticeToolData *state = ss->lattice_tool_state;
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
  LatticeToolData *state = lattice_drawable_state_get(object_active);
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

  lattice_cage_overlay_topology_ensure(*state, res);

  const float4x4 &obmat = lat_ob->object_to_world();
  r_out.points.reserve(state->overlay_point_indices.size() + 1);
  for (const int i : state->overlay_point_indices) {
    r_out.points.append(math::transform_point(obmat, float3(lt->def[i].vec)));
  }

  r_out.edges = state->overlay_edges;

  int active_compact = -1;
  if (state->pending_drag_index >= 0 && state->pending_drag_index < point_num) {
    for (const int i : state->overlay_point_indices.index_range()) {
      if (state->overlay_point_indices[i] == state->pending_drag_index) {
        active_compact = i;
        break;
      }
    }
    if (active_compact < 0) {
      r_out.points.append(
          math::transform_point(obmat, float3(lt->def[state->pending_drag_index].vec)));
      active_compact = r_out.points.size() - 1;
    }
  }

  r_out.resolution = res;
  r_out.active_point = active_compact;
  r_out.placement_phase = (state->phase == Phase::Placement);
  r_out.valid = true;
}

}  // namespace blender::ed::sculpt_paint::lattice
