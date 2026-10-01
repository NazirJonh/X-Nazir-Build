/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The UV projection of a live 3D (SurfaceAnchored) shape, for display in the Image Editor:
 * the contour is sampled exactly (plane pixel -> object point on the anchor plane ->
 * nearest point on the evaluated mesh -> barycentric -> UV of the anchor's UV map) and split into
 * UV-continuous parts at UV seams; the transform cage maps linearly through the exact
 * "plane pixels -> UV" Jacobian of one triangle.
 */

#pragma once

#include <string>

#include "BLI_index_range.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_bvhutils.hh"

#include "paint_shape.hh"
#include "paint_shape_space.hh"
#include "paint_vector_document.hh"

namespace blender {

struct Depsgraph;
struct Object;

namespace ed::sculpt_paint::shape {

/** A UV seam may never be detected below this jump (UV fraction). */
constexpr float SHAPE_UV_SEAM_JUMP_MIN = 0.02f;
/** ...and a jump is relative to the polyline's lower-quartile sample step, so dense outlines on
 * small islands still split. */
constexpr float SHAPE_UV_SEAM_JUMP_FACTOR = 4.0f;

/**
 * One UV-continuous run of contour samples. A shape crossing a UV seam yields several parts; the
 * Image Editor draws all of them (the non-cage ones muted) and the cage only on #cage_part.
 */
struct ShapeUVTracePart {
  /** UV per sample, in trace order (open run; the seam ends are not repeated). */
  Vector<float2> uvs;
  /** Hit triangle per sample, parallel to #uvs (island identity for the cage mapping). */
  Vector<int> tris;
  bool cyclic = false;
  /** Index into #uvs of the sample closest to the shape's origin: the reference of the part's own
   * plane-px -> UV mapping (used when the anchor is not on this part). */
  int ref_sample = -1;
  float2 ref_px = float2(0.0f);
  /** Exact within the reference sample's triangle. */
  float2x2 jacobian = float2x2::identity();
  bool jacobian_valid = false;
};

/** The contour + linear cage mapping of a session's shapes, in the UV domain. */
struct ShapeUVTrace {
  bool valid = false;
  /** The UV map the trace sampled through (the anchor's). */
  std::string uv_map_name;
  /** The restored anchor the trace was built through (the staleness check compares a fresh
   * restore against these). */
  float3 anchor_co = float3(0.0f);
  float3 anchor_normal = float3(0.0f, 0.0f, 1.0f);

  /** Plane-px -> UV at the anchor (exact within the anchor triangle). */
  float2 anchor_uv = float2(0.0f);
  float2x2 anchor_jacobian = float2x2::identity();
  bool anchor_jacobian_valid = false;
  /** The anchor's restored hit triangle (island identity for the cage mapping). */
  int anchor_tri = -1;

  Vector<ShapeUVTracePart> parts;
  /** The part the cage draws on: the largest one (area for a loop, length otherwise). */
  int cage_part = -1;
};

/** The affine plane-px -> UV mapping the cage draws through: the anchor's when its triangle is
 * among the cage part's samples, the cage part's reference mapping otherwise. */
struct ShapeUVCageMap {
  float2 ref_px = float2(0.0f);
  float2 ref_uv = float2(0.0f);
  float2x2 jacobian = float2x2::identity();
  /** True when #ref_px / #ref_uv are the anchor's (the exact per-triangle map at the plane
   * origin); false for a cage part reference. */
  bool at_anchor = false;
};

/** One run of the seam split: sample indices into the source arrays, in trace order. */
struct ShapeUVTraceRun {
  Vector<int> indices;
  bool cyclic = false;
};

/**
 * Split \a uvs into runs of UV-continuous samples. The jump threshold adapts to the polyline's
 * median sample step, so it works at any UV density; a cyclic polyline whose wrap is continuous
 * stays one cyclic run, otherwise the run is rotated open at the largest jump so the seam never
 * cuts through a run's middle. Pure (unit-tested).
 */
Vector<ShapeUVTraceRun> shape_uv_trace_split_runs(Span<float2> uvs, bool cyclic);

/**
 * The exact "plane pixels -> UV" Jacobian inside one triangle given in plane-px coordinates
 * (#p0..#p2) with corner UVs (#uv0..#uv2): `uv = uv0 + J * (p - p0)`. False when the triangle is
 * degenerate in the plane. Pure (unit-tested).
 */
float2x2 shape_uv_trace_jacobian(const float2 &p0,
                                 const float2 &p1,
                                 const float2 &p2,
                                 const float2 &uv0,
                                 const float2 &uv1,
                                 const float2 &uv2,
                                 bool &r_ok);

/**
 * Resolve the cage mapping of \a trace (see #ShapeUVCageMap). False without a valid trace, cage
 * part or Jacobian.
 */
bool shape_uv_trace_cage_map(const ShapeUVTrace &trace, ShapeUVCageMap &r_map);

/**
 * Per-session owner of the trace: rebuilds it when the shapes changed (immediate, from a gesture
 * / settings edit) or when the surface moved under the anchor (checked on demand, cheap), and
 * reuses the nearest-surface BVH while the evaluated positions keep their identity. The owned
 * #BVHTreeFromMesh borrows the evaluated mesh arrays, so it is rebuilt whenever those change.
 */
class ShapeUVTraceCache {
 public:
  ~ShapeUVTraceCache();

  /** Mark the shapes dirty (a gesture / settings edit changed the plane-pixel geometry). */
  void mark_shape_dirty()
  {
    shape_dirty_ = true;
  }

  /**
   * The current trace, rebuilt when stale; null when nothing valid can be shown (no UV map, the
   * anchor cannot be restored, nothing drawn). \a depsgraph may be null (nothing to check then).
   */
  const ShapeUVTrace *update(const Depsgraph *depsgraph,
                             const Object &ob,
                             const ShapeSpaceDesc &space_desc,
                             Span<VectorItem> items,
                             double now);

 private:
  bool ensure_bvh(const Depsgraph &depsgraph, const Object &ob);
  bool rebuild(const Depsgraph &depsgraph,
               const Object &ob,
               const ShapeSpaceDesc &space_desc,
               Span<VectorItem> items);

  bke::BVHTreeFromMesh bvh_{};
  bool bvh_valid_ = false;
  /** The positions span the BVH was built over (identity check for reuse). */
  Span<float3> bvh_positions_;

  ShapeUVTrace trace_;
  bool shape_dirty_ = true;
  double last_check_ = 0.0;
  /** Summed positions of the trace triangles' vertices plus the anchor: the cheap deformation
   * fingerprint. Sculpt strokes / filters move the evaluated positions in place, where a Span
   * identity check sees nothing; a changed fingerprint (checked at most at the deform-check pace)
   * stales the BVH and the trace. */
  float3 positions_fingerprint_ = float3(0.0f);
};

}  // namespace ed::sculpt_paint::shape
}  // namespace blender
