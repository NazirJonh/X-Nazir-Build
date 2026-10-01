/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Live Vector shape session of Sculpt Mode (the 3D counterpart of the Image Editor's
 * `paint_image_shape_vector.cc`). The session is owned by #SculptSession (a runtime pointer,
 * exactly like `CurvePatchSession`), drives the Image-canvas write backend with a live preview,
 * and is edited through the shared #ShapeVectorEditor / #VectorEditHost core.
 *
 * A Sculpt Vector session is always SurfaceAnchored: the shape lives in the tangent plane of the
 * surface under the first press, so the overlay and the gesture mapping reproject through the live
 * view and navigation passes through (the session follows the view; it never auto-Applies on it).
 */

#pragma once

#include <memory>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "../shapes/paint_shape.hh"
#include "../shapes/paint_shape_space.hh"

namespace blender {
/* Every type below lives in `blender`; the header may be included before the DNA / BKE headers, so
 * declaring them here (not at global scope) keeps the declarations below from binding to stray
 * global types. */
struct ARegion;
struct Brush;
struct Image;
struct Main;
struct Object;
struct Scene;
struct bContext;
struct wmEvent;
struct wmOperator;
}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

struct PaintShapeSession;
class ShapeEditSession;
class ShapeUVTraceCache;
struct ShapeUVTrace;
struct ShapeUVCageMap;
class VectorEditHost;

/** Open a live Vector session for \a ob from a just-created shape. No-op when a session is
 * already live on \a ob or the backend cannot begin; the session is published on completion. */
void paint_shape_session_begin(bContext *C,
                               wmOperator *op,
                               Object &ob,
                               ShapeSpaceDesc space_desc,
                               PaintShape shape,
                               const ShapeStyle &style);

/** The live session of \a ob, or null. */
PaintShapeSession *paint_shape_session_get(Object &ob);
const PaintShapeSession *paint_shape_session_get(const Object &ob);

/** True when \a ob owns a live Vector session. */
bool paint_shape_session_active(const Object &ob);

/** Commit the session of \a ob (bake one undo step, then free). False when there is none or the
 * bake reported nothing written. */
bool paint_shape_session_commit(bContext *C, Object &ob);
/** Cancel the session of \a ob (restore the preview, then free). */
void paint_shape_session_cancel(bContext *C, Object &ob);

/** Settle a session that must end now (its owning tool was switched away, with no shape modal
 * running to police it): commit it, and when the Apply is refused -- the anchored surface cannot
 * be restored -- cancel it and report a "shape discarded" warning, so no session is left alive
 * without the tool that owns it. True when the session was applied. */
bool paint_shape_session_settle_forced(bContext *C, Object &ob);

/** Context-less teardown: restore the preview and free. Registered as the #SculptSession free
 * callback; safe with a missing session, PBVH or context. */
void paint_shape_session_discard_on_session_end(Object &ob);

/** Cancel every live session in \a bmain (undo / redo pre-step). Returns true when at least one
 * session was cancelled. */
bool paint_shape_sessions_cancel_all(bContext *C);

/** Build the shared gesture host of \a session for the drawing operator's modal. */
std::unique_ptr<VectorEditHost> paint_shape_session_host_create(bContext &C,
                                                                PaintShapeSession &session,
                                                                float tolerance_region_px);

/** The modal operator currently drives \a session (blocks a second re-invoke). */
bool paint_shape_session_modal_active(const PaintShapeSession &session);
void paint_shape_session_modal_set_active(PaintShapeSession &session, bool active);

/** True when the active tool idname still matches the one the session began with; a mismatch ends
 * the session (Apply) so the new tool's events are not swallowed by the shape modal. */
bool paint_shape_session_tool_matches(const PaintShapeSession &session, const char *tool_id);

/** Region event -> session (tangent-plane) pixels via the view ray intersected with the anchor plane. False when
 * the ray is (near-)parallel to the plane: the caller keeps the last valid point, no Apply/Cancel. */
bool paint_shape_session_event_to_shape_px(const PaintShapeSession &session,
                                           const ARegion &region,
                                           const wmEvent &event,
                                           float2 &r_px);

/** Same as #paint_shape_session_event_to_shape_px for a bare region-pixel position. */
bool paint_shape_session_mval_to_shape_px(const PaintShapeSession &session,
                                          const ARegion &region,
                                          const int2 &region_mval,
                                          float2 &r_px);

/** Where a Ctrl+RMB cut at \a region_mval would land: the point on the active contour and its
 * direction, in region pixels. False when nothing would be inserted. \a tolerance_px is in session
 * pixels. */
bool paint_shape_session_insert_preview(const PaintShapeSession &session,
                                        const ARegion &region,
                                        const int2 &region_mval,
                                        float tolerance_px,
                                        float2 &r_point,
                                        float2 &r_tangent);

/** Session (tangent-plane) pixels -> region pixels through the live view (the session space's
 * #ShapeSpace::to_region). */
float2 paint_shape_session_shape_px_to_region(const PaintShapeSession &session,
                                              const ARegion &region,
                                              const float2 &px);

/** World-space matrix of the session's tangent plane: X/Y span one tangent-plane pixel, so a
 * shape-pixel point maps to world as `matrix * (px.x, px.y, 0, 1)`; Z is the plane normal. The 3D
 * transform cage uses it so perspective bends the cage the same way it bends the shape. False
 * without a session or with a degenerate (normal-parallel) tangent frame. */
bool paint_shape_session_plane_matrix(const PaintShapeSession &session, float4x4 &r_space);

/** True while the session's owner region still exists on a screen (guards a dangling pointer
 * after the area was closed or re-typed). */
bool paint_shape_session_region_alive(const PaintShapeSession &session);

/** True when the session's write targets include \a image: an Image Editor showing it is
 * "linked" and displays the session's contour / cage. */
bool paint_shape_session_targets_image(const PaintShapeSession &session, const Image &image);

/** The session's UV projection for the linked Image Editors, rebuilt lazily; null when
 * nothing valid can be shown. \a C supplies the depsgraph (may be null). */
const ShapeUVTrace *paint_shape_session_uv_trace(const PaintShapeSession &session,
                                                 const bContext *C);

/** The session's own #PaintShapeSettings copy, or null without a session. */
PaintShapeSettings *paint_shape_session_settings_get(Object &ob);

/** The tool idname the session began with (empty without a session). Used by the linked Image
 * Editor to switch its own tool to the matching shape tool on a cage click. */
const char *paint_shape_session_tool_id(const PaintShapeSession &session);

/** The affine plane-pixels <-> UV cage mapping a linked Image Editor gesture must use. While a
 * session gesture is in flight the mapping is frozen to the one captured when the gesture began
 * (the UV trace may rebuild for the contour mid-drag, but the cage mapping must not jump under the
 * cursor); after release it is recomputed. False when no mapping is available (nothing linked or
 * the Jacobian is degenerate). The mapping state lives with the session, not in the trace cache. */
bool paint_shape_session_linked_cage_map(PaintShapeSession &session,
                                         const bContext *C,
                                         ShapeUVCageMap &r_map);

/** Capture the cage mapping now, before a linked Image Editor starts its drag, so the mapping is
 * frozen from the press even if the trace rebuilds before the first move. Overwrites any stale
 * capture; a failed capture drops it (the drag then falls back to the live mapping). */
void paint_shape_session_freeze_cage_map(PaintShapeSession &session, const bContext *C);

/** Drop a press-time cage capture that never became a drag (a press over no handle). */
void paint_shape_session_clear_cage_map(PaintShapeSession &session);

/** Runtime "Transform" flag of the session (cage of a parametric shape), false without a session. */
bool paint_shape_session_transform_active(const Object &ob);

/** The active shape of the session (for the cage), or null. */
const PaintShape *paint_shape_session_active_shape(const Object &ob);
/** The session's edit session (for `gesture_active`), or null. */
const ShapeEditSession *paint_shape_session_edit(const Object &ob);

/** Toggle the session's runtime "Transform" flag (the UI button). */
void paint_shape_session_transform_toggle(Object &ob);

/** Refresh the live session whose own settings copy contains \a changed (context-free; used by the
 * RNA update). False when no 3D session matched. */
bool paint_shape_settings_update_3d(const Main *bmain, Scene *scene, PaintShapeSettings *changed);
/** Refresh the live sessions whose preview reads \a brush (or every session in \a scene when null;
 * a context-free brush / Strength / Blend change). False when none matched. */
bool paint_shape_brush_update_3d(const Main *bmain, const Scene *scene, const Brush *brush);

/** Refuse a workspace change while a live session owns the active object: the change is deferred
 * to the session's confirm dialog, which re-issues it once the shape is resolved. True when the
 * change was deferred (the dialog is up to carry it). */
bool paint_shape_session_defer_workspace_change(bContext *C, int workspace_session_uid);
/** Refuse an active-object change the same way; \a object_session_uid is the session-uid of the
 * object the user asked to activate. True when the change was deferred. */
bool paint_shape_session_defer_object_change(bContext *C, int object_session_uid);

}  // namespace blender::ed::sculpt_paint::shape
