/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Snapshot of the surface the Curve Patch ribbon is wrapped onto.
 *
 * A snapshot rather than the live mesh: the patch displaces vertices itself, so
 * `Mesh::bvh_corner_tris()` would be invalidated on every re-stamp and the tree rebuilt from
 * scratch. Semantically it is also the only correct choice -- the ribbon has to wrap the ORIGINAL
 * geometry, otherwise it wraps the relief it just applied and a feedback loop appears.
 */

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "BKE_bvhutils.hh"

struct Mesh;

namespace blender::ed::sculpt_paint {

struct CurvePatchSurfaceSnapshot {
  Array<float3> positions;
  /** Vertex normals as of the snapshot. The orientation culling has to compare against THESE: the
   * live normals already carry the relief the patch applied, so the culling would end up depending
   * on its own result. */
  Array<float3> vert_normals;
  bke::BVHTreeFromMesh bvh;
  bool ready = false;

  void clear();
};

/**
 * Builds a snapshot from the mesh's current (pristine) positions. Returns false for an empty mesh
 * or a failed BVH build -- the caller then stays on the single-window path.
 */
bool curve_patch_surface_snapshot_build(const Mesh &mesh, CurvePatchSurfaceSnapshot &r_snapshot);

/**
 * Pulls each position onto the nearest point of the snapshot and reports the normal of the triangle
 * it hit. A sample farther away than `max_dist` is left where it is and its normal stays zero -- the
 * "no normal here" marker #curve_patch_surface_fill_invalid_normals looks for.
 */
void curve_patch_surface_shrinkwrap(const CurvePatchSurfaceSnapshot &snapshot,
                                    float max_dist,
                                    MutableSpan<float3> positions,
                                    MutableSpan<float3> r_normals);

/**
 * Fills the zero (invalid) normals by interpolating between the nearest valid neighbours; writes
 * `fallback` everywhere when there is no valid normal at all.
 */
void curve_patch_surface_fill_invalid_normals(MutableSpan<float3> normals, const float3 &fallback);

}  // namespace blender::ed::sculpt_paint
