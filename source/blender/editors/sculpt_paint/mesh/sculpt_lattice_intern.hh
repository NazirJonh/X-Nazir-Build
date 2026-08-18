/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt Lattice Tool internals: declarations used only by the tool's own translation units
 * (sculpt_lattice.cc, sculpt_lattice_deform.cc, sculpt_lattice_draw.cc,
 * sculpt_lattice_place.cc).
 *
 * Everything the rest of the editor module may call lives in sculpt_lattice.hh; the overlay
 * engine sees neither header, only ED_sculpt_lattice_draw.hh.
 *
 * See .My_Docs_July_2026/Sculpt-Mode/Lattice-Tool/Plan_1/03_data_model.md
 */

#pragma once

#include <optional>

#include "BLI_bounds_types.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

namespace blender {
struct bContext;
struct Depsgraph;
struct LatticeDeformData;
struct Object;
namespace bke::pbvh {
class Tree;
}
}  // namespace blender

namespace blender::ed::sculpt_paint::lattice {

/* The mask epsilon lives in the public header as #SCULPT_LATTICE_MASK_EPS_DEFAULT
 * (single source of truth); do not redefine it here. */

/* Screen-space Manhattan distance threshold for picking a BPoint, in unscaled pixels. Callers
 * multiply by #UI_SCALE_FAC so the pick radius stays constant in physical size across DPI. */
constexpr float PAINT_LATTICE_POINT_PICK_THRESHOLD = 40.0f;

/* Minimum lattice resolution per axis (ADR-8). */
constexpr int SCULPT_LATTICE_MIN_RESOLUTION = 2;

/**
 * Prepares \a ob_mesh for editing and returns its PBVH, or null when the tool cannot work with it.
 *
 * Every entry point that eventually touches #PositionDeformData, #bke::pbvh::vert_positions_eval
 * or #sculpt_lattice_tag_affected_nodes must come through here first. The PBVH is a `unique_ptr`
 * that #BKE_sculpt_update_object_before_eval resets on any geometry re-evaluation not covered by
 * an in-flight stroke, filter, expand or lattice slide, and the tool session outlives its
 * individual operators (ADR-12) — so a re-evaluation may well have happened since the last
 * interaction, leaving the readers below to dereference a freed tree.
 *
 * Also filters out the PBVH types the MVP does not support (multires / dynamic topology: phase 3),
 * so that check does not have to be repeated at every call site either.
 */
bke::pbvh::Tree *sculpt_lattice_pbvh_ensure(Depsgraph &depsgraph, Object &ob_mesh);

/**
 * The PBVH of \a ob_mesh when the tool can operate on it, or null.
 *
 * The read-only counterpart of #sculpt_lattice_pbvh_ensure, for the query helpers that take the
 * object as `const` and therefore cannot prepare it. It only guards against a missing tree and an
 * unsupported type; making sure the tree is current is the caller's job, and the split exists so
 * that responsibility is visible in the signature rather than left to convention.
 *
 * Overloaded on constness like the #bke::object::pbvh_get it wraps, so the tagging path can get a
 * mutable tree from the same guarded lookup instead of validating and fetching separately.
 */
bke::pbvh::Tree *sculpt_lattice_pbvh_find(Object &ob_mesh);
const bke::pbvh::Tree *sculpt_lattice_pbvh_find(const Object &ob_mesh);

/**
 * (Re)builds lattice deform data from the live #Lattice.def control points.
 * Drops derived caches only when a curve_cache exists (the no-main temp cage typically
 * has none). Refreshes #object_to_world from loc/rot/scale.
 */
LatticeDeformData *sculpt_lattice_deform_data_rebuild(Object *lat_ob, Object *mesh_ob);

struct AffectedRegion;

/**
 * Rebuilds the cached PBVH node list for \a ar when it is missing or the tree changed.
 * Cheap when the cache already matches the live PBVH.
 */
void sculpt_lattice_ensure_affected_nodes(const Depsgraph &depsgraph,
                                          Object &ob_mesh,
                                          AffectedRegion &ar);

/**
 * Screen-space rectangle of a mouse drag on the workplane, in the plane's local XY.
 *
 * \param local_delta: Current mouse minus the click origin, projected onto the workplane axes.
 * \param origin_center: When true the click is the rectangle center (Alt); otherwise a corner.
 * \param fixed_aspect: When true the height follows the width (Shift, 1:1).
 */
Bounds<float2> sculpt_lattice_box_local_rect(const float2 local_delta,
                                             bool origin_center,
                                             bool fixed_aspect);

/** World transform of the unit-cube cage that encloses a workplane rectangle plus thickness. */
struct LatticeBoxTransform {
  float3 location = float3(0.0f);
  float3 scale = float3(1.0f);
  float3x3 rotation = float3x3::identity();
  /** Center of the face that stays on the workplane. */
  float3 front_center = float3(0.0f);
  /** Unit direction from the front face into the cage volume. */
  float3 extrude_dir = float3(0.0f, 0.0f, -1.0f);
};

/**
 * Places the cage so the drawn rectangle lies on the workplane and the volume extends along
 * \a basis[2] (or the opposite when \a flip is set).
 */
LatticeBoxTransform sculpt_lattice_box_transform_from_rect(const float3 &plane_origin,
                                                           const float3x3 &basis,
                                                           const Bounds<float2> &rect,
                                                           const float3 &translation,
                                                           float thickness,
                                                           bool flip);

/**
 * Thickness along \a extrude_dir from the closest point between that axis and the mouse ray.
 * Returns nullopt when the lines are parallel.
 */
std::optional<float> sculpt_lattice_box_thickness_from_lines(const float3 &front_center,
                                                             const float3 &extrude_dir,
                                                             const float3 &ray_a,
                                                             const float3 &ray_b,
                                                             float min_thickness);

/** Status-bar hints for the idle (non-modal) Placement / Deform phases. */
void sculpt_lattice_status_idle(bContext *C);

}  // namespace blender::ed::sculpt_paint::lattice
