/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Public draw-data API for the Sculpt Lattice cage overlay (Plan 2).
 * The overlay engine consumes only this header; the LatticeToolData type stays
 * private to editors (see mesh/sculpt_lattice.hh).
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {
struct Depsgraph;
struct Object;
}  // namespace blender

namespace blender::ed::sculpt_paint::lattice {

/** Cage geometry projected for the overlay. Rebuilt every redraw (pull model, ADR-16). */
struct LatticeCageDrawData {
  /** Control points in world space. */
  blender::Vector<blender::float3> points;
  /** Cage edges as index pairs into #points (U/V/W neighbours). */
  blender::Vector<blender::int2> edges;
  /** Index of the active / dragged point, or -1. */
  int active_point = -1;
  /** True while the cage is being placed rather than deformed; the overlay colors it
   * differently. */
  bool placement_phase = false;
  /** Lattice resolution (for optional per-axis coloring). */
  blender::int3 resolution = blender::int3(0);
  bool valid = false;
};

/**
 * Builds cage edge index pairs for a `res` (pntsu, pntsv, pntsw) grid.
 * Pure function (no state), unit-testable. Index = (w * res.y + v) * res.x + u.
 */
void lattice_cage_edges_build(const blender::int3 &res, blender::Vector<blender::int2> &r_edges);

/**
 * Overlay topology: lattice-def point indices and edges into that compact list.
 * When \a outer_shell_only is true, interior points and interior edges are omitted.
 */
void lattice_cage_overlay_topology_build(const blender::int3 &res,
                                         bool outer_shell_only,
                                         blender::Vector<int> &r_point_indices,
                                         blender::Vector<blender::int2> &r_edges);

/**
 * Cheap check, called every begin_sync: resolves the active sculpt object and reports whether
 * the lattice tool is active with a live tool state to draw.
 */
bool ED_sculpt_lattice_cage_is_relevant(const blender::Depsgraph *depsgraph,
                                        const blender::Object *object_active);

/**
 * Builds #LatticeCageDrawData from the live tool state. Sets r_out.valid = false when there is
 * nothing to draw.
 */
void ED_sculpt_lattice_cage_build(const blender::Depsgraph *depsgraph,
                                  const blender::Object *object_active,
                                  LatticeCageDrawData &r_out);

}  // namespace blender::ed::sculpt_paint::lattice
