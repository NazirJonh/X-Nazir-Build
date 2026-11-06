/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {
struct Brush;
struct bContext;
struct ID;
struct Object;
struct PaintModeSettings;
struct ReportList;

namespace bke {
class CurvesGeometry;
}
}  // namespace blender

struct PaintCurveSnapContext;
struct ViewContext;

namespace blender::ed::sculpt_paint {

struct CurvePatchDocument;
struct CurvePatchObjectTarget;
struct CurvePatchSession;

/** World-space bounding sphere used to decide which secondary targets a stamp can reach. */
struct CurvePatchWorldStamp {
  bool any_buildable = false;
  float3 bbox_min = float3(0.0f);
  float3 bbox_max = float3(0.0f);
  float3 center = float3(0.0f);
  float reach_radius = 0.0f;
};

/** Transform one control curve in place, including both handle-position arrays. */
void curve_patch_control_curve_transform(const float4x4 &matrix, bke::CurvesGeometry &curve);

/** Largest absolute axis scale of the object's WORLD matrix. */
float curve_patch_object_max_scale(const Object &ob);

/** World-space stamp derived from the document's control curves and the owner's matrix. */
CurvePatchWorldStamp curve_patch_world_stamp_from_primary(const Object &primary,
                                                          const CurvePatchSession &session);

bool curve_patch_target_in_reach(const Object &target_ob,
                                 const float3 &world_center,
                                 float world_reach_radius);

/** Rebuild one secondary target's mirror when the document or either transform changed. */
void curve_patch_target_mirror_sync(CurvePatchSession &session,
                                    CurvePatchObjectTarget &target,
                                    ReportList *reports);

/**
 * Add one object to a LIVE session as a secondary target, the way the Shift+drag gesture does.
 *
 * Runs the session's compatibility filter (a Mesh-typed PBVH, the same effect kind, no shared
 * mesh/canvas, no stroke in progress on the object), creates the target's stand-in #StrokeCache
 * and effect, republishes the session on the object, then performs a full re-stamp and
 * finished-stroke flush for it. Returns false, with the reason in \a reports, when the object
 * cannot join; the session is left unchanged in that case.
 *
 * The new target is appended at the end of #CurvePatchSession::targets; `[0]` stays the owner.
 */
bool curve_patch_session_add_target(bContext &C,
                                    CurvePatchSession &session,
                                    Object &ob,
                                    ReportList *reports);

/** Remove one object from a session's target list. Returns whether a target was removed. */
bool curve_patch_session_remove_target(CurvePatchSession &session, const Object &ob);

/** Claim an object's mesh for the shared-surface dedup. Returns false when already claimed. */
bool curve_patch_target_mesh_claim(Set<const ID *> &taken_meshes, const Object &ob);

/** Restore and release ONE secondary target when its SculptSession is about to be destroyed. */
void curve_patch_target_detach_on_session_end(Object &ob);

/**
 * The Shift+drag gesture of the whole-point drag: find the object under the cursor (other than the
 * owner), make it a target of \a session when it is not one yet, and place the dragged point on
 * ITS surface. On success the hit is reported in the OWNER's object space (\a r_owner_co /
 * \a r_owner_no) -- the document never leaves the owner's space -- so the caller applies it to the
 * control curve like any other surface snap.
 *
 * Returns false when the gesture stays on the owner's own surface (no other object under the
 * cursor, the candidate refused, or no placement); the caller then falls back to its own
 * placement path. \a refused collects the objects already reported during THIS drag, so a
 * MOUSEMOVE does not repeat a refusal; the caller clears it when a new drag starts. \a snap_ctx is
 * the drag-scoped snap context borrowed from the operator data, created on first use.
 *
 * Two raycasts per move are deliberate: #stroke_get_location_bvh names the object under the cursor
 * across every Sculpt Mode object but supplies no surface normal, while #paintcurve_surface_place
 * resolves the placement (snap levels included) against ONE object -- the hit -- so its normal can
 * be carried back into the owner's space.
 */
bool curve_patch_drag_point_onto_other_object(bContext &C,
                                              CurvePatchSession &session,
                                              Object &owner,
                                              const ViewContext &vc,
                                              const float2 &mval,
                                              const float3 &pivot_world,
                                              PaintCurveSnapContext *&snap_ctx,
                                              Vector<const Object *> &refused,
                                              float3 &r_owner_co,
                                              float3 &r_owner_no);

}  // namespace blender::ed::sculpt_paint
