/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_scene_types.h"
#include "DNA_view3d_enums.h"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include <cstdint>

namespace blender {

namespace bke {
class CurvesGeometry;
struct CurvePatchParams;
}  // namespace bke

enum class PaintMode : int8_t;
struct ARegion;
struct bContext;
struct ARegionType;
struct Brush;
struct bToolRef;
struct Depsgraph;
struct Image;
struct ImageUndoStep;
struct ImageUser;
struct ImBuf;
struct Main;
struct Object;
struct Paint;
struct PaintModeSettings;
struct PaintTileMap;
struct ReportList;
struct Scene;
struct SpaceImage;
struct UndoStep;
struct UndoType;
struct View3D;
struct wmGizmoGroupType;
struct wmKeyConfig;
struct wmOperator;

/* `paint_ops.cc` */

void ED_operatortypes_paint();
void ED_operatormacros_paint();
void ED_keymap_paint(wmKeyConfig *keyconf);

/* `paint_image.cc` */

void ED_imapaint_clear_partial_redraw();
void ED_imapaint_dirty_region(
    Image *ima, ImBuf *ibuf, ImageUser *iuser, int x, int y, int w, int h, bool find_old);
void ED_imapaint_bucket_fill(bContext *C,
                             const float color[3],
                             wmOperator *op,
                             const int mouse[2]);

/* `paint_image_proj.cc` */

void ED_paint_data_warning(
    ReportList *reports, bool has_uvs, bool has_mat, bool has_tex, bool has_stencil);
/**
 * Make sure that active object has a material,
 * and assign UVs and image layers if they do not exist.
 */
bool ED_paint_proj_mesh_data_check(Scene &scene,
                                   Object &ob,
                                   bool *r_has_uvs,
                                   bool *r_has_mat,
                                   bool *r_has_tex,
                                   bool *r_has_stencil);

/* `image_undo.cc` */

/**
 * The caller is responsible for running #ED_image_undo_push_end,
 * failure to do so causes an invalid state for the undo system.
 */
void ED_image_undo_push_begin(const char *name, PaintMode paint_mode);
void ED_image_undo_push_begin_with_image(const char *name,
                                         Image *image,
                                         ImBuf *ibuf,
                                         ImageUser *iuser);
void ED_image_undo_push_begin_with_image_all_udims(const char *name,
                                                   Image *image,
                                                   ImageUser *iuser);
void ED_image_undo_push(Image *image, ImBuf *ibuf, ImageUser *iuser, ImageUndoStep *us);
void ED_image_undo_push_end();
/**
 * Whether an image undo step is the one currently being built.
 *
 * Writers that capture pristine tiles have to know this: a tile pushed with no open step is a
 * pixel edit that can never be undone. Answers the question through the undo API rather than
 * having every caller reach into #UndoStack::step_init and compare types itself.
 */
bool ED_image_undo_is_step_active();
/**
 * Capture a snapshot of the selection mask for `tile_number` into the currently open image undo
 * step. Must be called after #ED_image_undo_push_begin* and before any mask modifications.
 * On undo, the mask is restored alongside the pixel data.
 */
void ED_image_undo_capture_selection_mask(Image *image, int tile_number);
/**
 * Begin an image undo step that records only selection mask changes (no pixel data).
 * Captures the pre-operation mask state for all tiles of `image`.
 * The caller must call #ED_image_undo_push_end when the mask modification is complete.
 * Do NOT combine with OPTYPE_UNDO on the operator - the step is managed manually.
 */
void ED_image_undo_push_begin_selection(const char *name, Image *image);
/**
 * Restore painting image to previous state. Used for anchored and drag-dot style brushes.
 */
void ED_image_undo_restore(UndoStep *us);

/**
 * A paint-tile map owned by its caller rather than by an in-flight undo step.
 *
 * A long-lived paint session (the 2D Curve Patch, which the user edits for as long as they like
 * before confirming) needs the "before" pixels of everything it has touched, so that every edit
 * can restore and re-stamp from the pristine canvas. Blender's own transaction gives exactly that
 * -- but only by holding `UndoStack::step_init` for the session's whole life, where any other
 * operator's undo push would adopt or free it underneath.
 *
 * These four functions decouple the two: the session captures into its own map, restores from it
 * as often as it likes, and only at commit does the map become one history entry. Nothing is in
 * flight in between, so there is no slot to lose.
 */
PaintTileMap *ED_image_paint_tile_map_new();
void ED_image_paint_tile_map_free(PaintTileMap *paint_tile_map);

/**
 * Write every captured tile back into its image, notify the partial-update system, and mark the
 * tiles invalid.
 *
 * The invalidation is what keeps a later #ED_image_undo_push_from_tile_map honest: only tiles a
 * writer touches again (which re-validates them) end up in the history entry, so a tile some
 * earlier re-stamp touched and the final one did not is restored to pristine here and correctly
 * left out. Re-touching preserves the originally captured pixels; #ED_image_paint_tile_push finds
 * the existing tile instead of capturing the current, already-painted ones.
 */
void ED_image_paint_tile_map_restore(PaintTileMap *paint_tile_map);

/**
 * Turn `paint_tile_map`'s captured tiles into exactly one image undo history entry.
 *
 * The map is emptied into the step; the caller still owns and must free the handle. Call it once,
 * at commit, after the final re-stamp.
 */
void ED_image_undo_push_from_tile_map(const char *name,
                                      PaintMode paint_mode,
                                      PaintTileMap *paint_tile_map);

/** Export for ED_undo_sys. */
void ED_image_undosys_type(UndoType *ut);

const ImBuf *ED_image_paint_tile_find(PaintTileMap *paint_tile_map,
                                      Image *image,
                                      ImBuf *ibuf,
                                      ImageUser *iuser,
                                      int x_tile,
                                      int y_tile,
                                      unsigned short **r_mask,
                                      bool validate);
const ImBuf *ED_image_paint_tile_push(PaintTileMap *paint_tile_map,
                                      Image *image,
                                      ImBuf *ibuf,
                                      ImageUser *iuser,
                                      int x_tile,
                                      int y_tile,
                                      unsigned short **r_mask,
                                      bool **r_valid,
                                      bool use_thread_lock,
                                      bool find_prev);
void ED_image_paint_tile_lock_init();
void ED_image_paint_tile_lock_end();

PaintTileMap *ED_image_paint_tile_map_get();

#define ED_IMAGE_UNDO_TILE_BITS 6
#define ED_IMAGE_UNDO_TILE_SIZE (1 << ED_IMAGE_UNDO_TILE_BITS)
#define ED_IMAGE_UNDO_TILE_NUMBER(size) \
  (((size) + ED_IMAGE_UNDO_TILE_SIZE - 1) >> ED_IMAGE_UNDO_TILE_BITS)

/* `paint_curve_undo.cc` */

void ED_paintcurve_undo_push_begin(bContext *C, const char *name);
void ED_paintcurve_undo_push_end(bContext *C);

/** Export for ED_undo_sys. */
void ED_paintcurve_undosys_type(UndoType *ut);

/**
 * Copy geometry from #Sculpt.paint_curve_source_object into the active brush paint curve.
 * \return true when import was performed.
 */
bool ED_paintcurve_import_from_source_object(bContext *C, ReportList *reports, bool use_undo);

/** Re-import the paint curve after entering sculpt mode when a source curve is assigned. */
void ED_paintcurve_refresh_on_sculpt_mode_enter(bContext *C);

/**
 * Stop syncing the intermediate paint curve back to the source object.
 *
 * Does NOT touch the curve itself: this runs from an RNA property assignment, where destroying
 * user data as a side effect is invisible from Python. Clearing the canvas is
 * #PAINTCURVE_OT_clear.
 */
void ED_paintcurve_detach_source(bContext *C);

/** Commit radius edits from transform (Curve Shrink/Fatten) to paint-curve data. */
void ED_paintcurve_flush_radius_transform(bContext *C, struct PaintCurve *pc);

/* `paint_curve_geometry.cc`, for callers outside this module -- the RNA layer. The geometry
 * helpers themselves live in the module-private `paint_curve_intern.hh`. */

/**
 * Recompute what a control-point edit invalidates: the auto/aligned bezier handles.
 *
 * A no-op on an uninitialized or empty geometry, and on one whose handle position attributes were
 * never created. Call after changing point positions from outside the paint-curve operators; those
 * recompute for themselves.
 *
 * Pushes no undo step. A caller reached through RNA is covered by the global memfile undo, which
 * is how comparable RNA edits behave; #ED_paintcurve_undo_push_begin serves the modal operators.
 */
void ED_paintcurve_geometry_update_after_edit(struct PaintCurve *pc);

/**
 * Append one control point at the end of the active spline, starting a spline when the geometry is
 * empty, and return its point index.
 *
 * `radius` follows this codebase's paint-curve convention, where 1.0 means "full brush size" --
 * NOT #blender::bke::CurvesGeometry::radius()'s hair-oriented 0.01 default.
 *
 * The point gets AUTO bezier handles, unlike the ALIGN ones an interactive click leaves: a click
 * is the first half of a gesture that drags the handle out next, while a caller here supplies
 * positions and nothing else.
 */
int ED_paintcurve_geometry_add_point(struct PaintCurve *pc, const float position[3], float radius);

/** Drop every point and spline, leaving an empty but initialized geometry. */
void ED_paintcurve_geometry_clear(struct PaintCurve *pc);

/**
 * Replace the whole geometry with a single bezier spline built from `positions`.
 *
 * The batch counterpart of #ED_paintcurve_geometry_add_point, which recomputes the auto handles of
 * the whole curve on every call -- building an N-point curve with it costs N full recomputes. This
 * one recomputes once.
 *
 * `radii` is either empty, in which case every point gets 1.0, or the same length as `positions`.
 * The value follows this codebase's paint-curve convention, where 1.0 means "full brush size" and
 * NOT #blender::bke::CurvesGeometry::radius()'s hair-oriented 0.01 default.
 *
 * Points get AUTO handles, for the reason #ED_paintcurve_geometry_add_point gives: a caller that
 * supplies positions and nothing else has no handles to place, and ALIGNED ones sitting on top of
 * their points would make the curve a polyline.
 *
 * Replaces EVERYTHING, splines and point attributes alike -- a paint curve holding several splines
 * cannot be rebuilt spline by spline through this call.
 */
void ED_paintcurve_geometry_points_set(struct PaintCurve *pc,
                                       Span<float3> positions,
                                       Span<float> radii,
                                       bool cyclic);

/**
 * One spline of a paint curve, as the Curve Patch build wants it: a standalone single-spline
 * geometry carrying every attribute of the original, with the bezier handle POSITION attributes
 * materialized, the auto/aligned handles recomputed, and a `radius` attribute guaranteed present.
 *
 * `spline_index` selects the spline; a negative one means the curve's own
 * #PaintCurve.active_curve. The index is clamped, so an out-of-range one yields the nearest
 * existing spline rather than nothing.
 *
 * Neither materialization step is optional, and both fail silently when skipped. A curve whose
 * handle position attributes were never created evaluates to a bezier collapsed at the origin. A
 * curve with no `radius` attribute answers #blender::bke::CurvesGeometry::radius()'s hair-oriented
 * 0.01 default, and the strip comes out a hundredth of its intended width.
 *
 * Returns an empty geometry when the paint curve holds no splines.
 */
bke::CurvesGeometry ED_paintcurve_control_curve_for_patch(const struct PaintCurve &pc,
                                                          int spline_index);

/**
 * True when the active tool edits the brush's paint curve independently of the brush's stroke
 * method, so a transform invoked now must target the curve rather than the mesh.
 *
 * Exists for the transform system, which otherwise had to spell the tool's `idname` out itself:
 * a tool rename on the Python side would then have silently disabled paint-curve transforms
 * instead of failing to build. Which tools qualify is this module's business.
 *
 * Defined in `paint_curve.cc`.
 */
bool ED_paint_curve_transform_target_poll(const bContext *C);

/* `paint_curve_patch_apply.cc`, for callers outside this module -- the RNA layer. */

/**
 * The Curve Patch build parameters a brush implies for a patch driven from OUTSIDE a stroke: the
 * brush's own settings plus the four values a stroke would otherwise freeze -- world radius,
 * radius per unit of the Size slider, projection plane and stamp seed.
 *
 * The plane comes from `control_curve` itself (Newell's normal over its points as a closed loop,
 * the object's +Z when degenerate), which is what makes the result independent of any viewport and
 * identical in background mode. The stamp seed is rolled fresh on every call.
 *
 * This is the ONE place those four values are decided. A caller that assembles
 * #blender::bke::CurvePatchParams itself would silently read a different patch than the one
 * `SCULPT_OT_curve_patch_apply` stamps.
 */
bke::CurvePatchParams ED_curve_patch_params_from_brush(const Paint &paint,
                                                       const Brush &brush,
                                                       const bke::CurvesGeometry &control_curve);

/**
 * The cumulative weight table a Stamps-mode build draws its texture slot from, or an empty array
 * when the brush is in single-texture mode. `radius` is the patch's base world radius.
 *
 * Exists so the RNA read-back can produce the same stamp-to-slot assignment a live session does.
 * The binding itself stays private to the editor module: only the weights cross the boundary,
 * because only they reach the core build.
 *
 * Defined in `paint_curve_patch_params.cc`.
 */
Array<float> ED_curve_patch_stamp_texture_weights_from_brush(const Brush &brush, float radius);

/* Running Curve Patch session: typed public API in `ED_curve_patch.hh`. */

/**
 * Return true when `mval` is over a paint-curve handle that is currently selected.
 * Used to block #transform.translate CLICK_DRAG from moving selected points without a
 * direct click on a control point.
 */
bool ED_paintcurve_cursor_on_selected_handle(bContext *C, const float mval[2]);

/**
 * Write the paint-curve geometry positions back to the linked source Curves or Curve object.
 * Does nothing when no source object is set or sync is disabled.
 */
bool ED_paintcurve_sync_to_source(bContext *C, struct PaintCurve *pc);

/**
 * Convert paint-curve control points after toggling #PaintCurve.use_3d_space.
 * Requires an active 3D viewport and sculpt paint object.
 */
bool ED_paintcurve_convert_space_on_toggle(bContext *C, struct PaintCurve *pc);

enum ePaintCurveExportCurveType {
  PAINT_CURVE_EXPORT_BEZIER = 0,
  PAINT_CURVE_EXPORT_CURVES = 1,
};

/**
 * Create a new Curve or Curves object on the scene from the active brush paint-curve geometry.
 * The destination object receives the same world transform as the active sculpt object.
 * When \a use_selection is true, only fully selected control points are exported (one spline
 * per contiguous run); otherwise the entire paint curve is exported.
 * When \a assign_as_source is true, the new object is linked as #Sculpt.paint_curve_source_object
 * with sync enabled so subsequent edits are written back to the scene object.
 * \return true when export was performed. Optionally returns the destination object.
 */
bool ED_paintcurve_export_to_scene_object(bContext *C,
                                          ReportList *reports,
                                          Object **r_dst_ob,
                                          ePaintCurveExportCurveType curve_type,
                                          bool use_selection,
                                          bool assign_as_source);

/* `paint_cursor.cc` */

/** Free GPU overlay textures used by the paint cursor. Must be called while a GPU context is
 * active. Called on application exit to ensure Vulkan memory allocations are released. */
void ED_paint_cursor_delete_textures();

/**
 * Register the WM paint-cursor that tags the viewport for redraw when the mouse moves
 * (Unit D). Must be called once during editor init (after WM is ready).
 * Safe to call multiple times — guards against double-registration internally.
 */
void ED_paint_curve_overlay_redraw_register();

/**
 * Free the brush overlay textures cached in the `paint_cursor.cc` file-static snapshots.
 *
 * They are normally only released when leaving a paint or sculpt mode. Blender's exit path never
 * leaves those modes, so on quit the textures outlive the GPU backend and the Vulkan allocator
 * asserts on the unfreed image. Must be called while the GPU context is still alive.
 */
void ED_paint_cursor_free_textures();

/* `paint_curve_snap.cc` (implemented in #bf_editor_transform) */

struct PaintCurveSnapContext;

PaintCurveSnapContext *ED_paintcurve_snap_context_create();
void ED_paintcurve_snap_context_destroy(PaintCurveSnapContext *snap_ctx);

/**
 * Effective snap-element mask for paint-curve editing.
 * Matches the header snap popover (#ToolSettings.snap_mode via `snap_elements`).
 */
eSnapMode ED_paintcurve_snap_elements(const ToolSettings *ts);

/**
 * When geometry snap targets are enabled, exclude increment/grid so a missed hit does not
 * fall back to a different snap type (paint-curve editing uses exclusive snap modes).
 */
eSnapMode ED_paintcurve_snap_mode_sanitize(eSnapMode snap_mode);

/**
 * Snap under \a mval to scene geometry (vertex / edge / face per #ToolSettings.snap_mode).
 *
 * \param prev_co_world: Optional world-space reference (edge-perpendicular, etc.).
 * \param r_hit_ob: Hit location in \a obact object space.
 */
bool ED_paintcurve_snap_point(bContext *C,
                              PaintCurveSnapContext *snap_ctx,
                              Depsgraph *depsgraph,
                              const View3D *v3d,
                              ARegion *region,
                              Object *obact,
                              const float mval[2],
                              const float prev_co_world[3],
                              float r_hit_ob[3]);

/* `paint_canvas.cc` */

/** Color type of an object can be overridden in sculpt/paint mode. */
eV3DShadingColorType ED_paint_shading_color_override(bContext *C,
                                                     const PaintModeSettings *settings,
                                                     Object &ob,
                                                     eV3DShadingColorType orig_color_type);

/**
 * Does the given tool use a paint canvas.
 *
 * When #tref isn't given the active tool from the context is used.
 */
bool ED_paint_brush_type_use_canvas(bContext *C, bToolRef *tref);

/** Store the last used tool in the sculpt session. */
void ED_paint_brush_type_update_sticky_shading_color(bContext *C, Object *ob);

void ED_object_vpaintmode_enter_ex(Main &bmain, Depsgraph &depsgraph, Scene &scene, Object &ob);
void ED_object_vpaintmode_enter(bContext *C, Depsgraph &depsgraph);
void ED_object_wpaintmode_enter_ex(Main &bmain, Depsgraph &depsgraph, Scene &scene, Object &ob);
void ED_object_wpaintmode_enter(bContext *C, Depsgraph &depsgraph);

void ED_object_vpaintmode_exit_ex(Object &ob);
void ED_object_vpaintmode_exit(bContext *C);
void ED_object_wpaintmode_exit_ex(Object &ob);
void ED_object_wpaintmode_exit(bContext *C);

void ED_object_texture_paint_mode_enter_ex(Main &bmain,
                                           Scene &scene,
                                           Depsgraph &depsgraph,
                                           Object &ob);
void ED_object_texture_paint_mode_enter(bContext *C);

void ED_object_texture_paint_mode_exit_ex(Main &bmain, Scene &scene, Object &ob);
void ED_object_texture_paint_mode_exit(bContext *C);

/* `paint_image_select_transform.cc` */

bool ED_image_paint_select_is_transforming(SpaceImage *sima);
void ED_image_paint_select_translation_get(SpaceImage *sima, float r_translation[2]);
void ED_image_paint_select_translation_set(SpaceImage *sima, const float translation[2]);
float ED_image_paint_select_rotation_get(SpaceImage *sima);
void ED_image_paint_select_rotation_set(SpaceImage *sima, float rotation);
void ED_image_paint_select_scale_get(SpaceImage *sima, float r_scale[2]);
void ED_image_paint_select_scale_set(SpaceImage *sima, const float scale[2]);
/** Register the gizmo group that draws the floating selection transform cage. */
void ED_image_paint_select_transform_gizmo_setup(wmGizmoGroupType *gzgt);

/* `paint_image_shape_gizmo.cc` */

/** Register the gizmo group that draws the live Vector shape session's transform cage. */
void ED_image_shape_transform_gizmo_setup(wmGizmoGroupType *gzgt);

/** True when \a mval is over the live shape session's cage handles or interior. The draw
 * operator's modal yields LEFTMOUSE to the gizmo when this returns true. */
bool ED_image_shape_transform_gizmo_hit(const bContext *C, const int mval[2]);

/** Runtime "Transform" mode of the live Vector session in \a sima (cage of a parametric shape without an analytic SDF). */
bool ED_image_shape_transform_is_active(SpaceImage *sima);

/* `paint_shape_gizmo_view3d.cc` */

/** Register the 3D Viewport gizmo group that draws the live Sculpt Vector session's cage. */
void ED_view3d_shape_transform_gizmo_setup(wmGizmoGroupType *gzgt);

/** True when \a mval is over the live Sculpt shape session's cage handles or interior. */
bool ED_view3d_shape_transform_gizmo_hit(const bContext *C, const int mval[2]);

/* `paint_shape_image_3d.cc` */

/** Register the Image Editor gizmo group that displays a live 3D Sculpt shape session's cage in
 * the editor's UV space. */
void ED_image_paint_shape3d_gizmo_setup(wmGizmoGroupType *gzgt);

/** Add the draw callback that shows a live 3D Sculpt shape session's contour in every Image Editor
 * displaying one of its target images. Called once from the Image Editor's main region type
 * setup. */
void ED_image_paint_shape3d_draw_register(ARegionType *art);

/** True while any live 3D Sculpt shape session exists (cheap guard for listeners and draws). */
bool ED_paint_shape_sessions_alive();

/* `paint_shape_image_3d.cc`: the linked Image Editor edits the same 3D session. */

/** True when the Image Editor of the current context shows a live 3D Sculpt shape session (one of
 * its target images, without an Image Vector session of its own). */
bool ED_image_paint_shape3d_session_linked(const bContext *C);

/** Apply (bake) / cancel the 3D session shown by the Image Editor of the current context. False
 * when this editor is not linked to one. */
bool ED_image_paint_shape3d_session_apply(bContext *C);
bool ED_image_paint_shape3d_session_cancel(bContext *C);

/** Ask the user what to do with a live 3D Sculpt shape session (Apply / Discard / Continue)
 * before the workspace changes; see #ED_paint_shape_session_defer_object_change. \return true
 * when the caller must abandon this workspace change. */
bool ED_paint_shape_session_defer_workspace_change(bContext *C, int workspace_session_uid);

/** Ask the user what to do with a live 3D Sculpt shape session on the active object before it
 * stops being active (a viewport pick, an Outliner / animation-editor row, a mode transfer; see
 * #object::base_activate_user). \a object_new_session_uid is the object the user asked to
 * activate; the change is re-issued once the session is resolved. \return true when the caller
 * must abandon this activation. */
bool ED_paint_shape_session_defer_object_change(bContext *C, int object_new_session_uid);

/* `paint_image_select_move.cc` */

bool ED_image_paint_select_is_moving(SpaceImage *sima);
void ED_image_paint_select_move_offset_get(SpaceImage *sima, float r_offset[2]);
void ED_image_paint_select_move_offset_set(SpaceImage *sima, const float offset[2]);

/* `paint_image_select_mask.cc` */

/**
 * Cancel the floating session: restore lifted pixels or gradient backups, close an open
 * image-undo step, then free. \a C may be null (space free / editor close).
 */
void ED_image_paint_select_session_cancel(bContext *C, SpaceImage *sima);

/**
 * Settle \a sima's floating session the takeover way: the user's in-progress edit is kept (a
 * shape session bakes with its own undo step, a lifted-pixels session commits, a gradient
 * preview is restored and discarded). Used by the image-save paths, which must not write
 * unconfirmed preview pixels to disk without an undo step.
 */
void ED_image_paint_select_session_settle(bContext *C, SpaceImage *sima);

/**
 * #ED_image_paint_select_session_settle for every Image Editor in \a bmain (any window).
 * Used by Save All, which writes images of editors the context is not showing.
 */
void ED_image_paint_select_sessions_settle_all(bContext *C);

/**
 * Cancel (restore + free) the floating session of every Image Editor in \a bmain. Called from
 * the undo/redo pre-step: undo only rewrites the tiles and regions a step recorded, so a session
 * preview outside them would otherwise survive in the live buffers with no undo step behind it,
 * and restoring after the load would paint the pre-preview backups back over the undone canvas.
 * Cancelling matches every tool's own Esc semantics. This changes the previously discarded-on-
 * undo behavior of the gradient/warp/move/transform sessions as well.
 */
void ED_image_paint_select_sessions_cancel_all(bContext *C);

/**
 * Cancel only the live Image Editor Vector shape session(s). Returns true when at least one was
 * cancelled, so the undo pre-step can consume the step (the uncommitted shape takes the first
 * undo) while the other floating tools keep their settle-then-undo behavior.
 */
bool ED_image_paint_shape_sessions_cancel_all(bContext *C);

/**
 * Discard only the floating transform state, leaving the rest of the session untouched. Used
 * when an undo step invalidates the source pixels the floating transform was lifted from.
 */
void ED_image_paint_select_transform_state_free(SpaceImage *sima);

/* `paint_image_select_gradient.cc` */

/**
 * Invalidate the live gradient preview so the next paint-cursor tick of any floating gradient
 * session re-evaluates the tool settings.
 */
void ED_image_paint_select_gradient_settings_revision_bump();

/* `paint_image_select_warp.cc` */

/**
 * Invalidate the live warp preview so the next non-blocking paint-cursor tick of any floating
 * warp session picks up a changed #ImagePaintSettings::warp_grid_size.
 */
void ED_image_paint_select_warp_settings_revision_bump();

/* `paint_image_shape_vector.cc` */

struct Main;
/**
 * A #PaintShapeSettings block changed. \a changed is the block the RNA setter wrote: when it is a
 * live Vector session's own copy, only that session is refreshed; a change to the global
 * `tool_settings.image_paint.shape` is ignored by live sessions (they own their copy).
 */
void ED_paint_shape_settings_update(Main *bmain, Scene *scene, PaintShapeSettings *changed);

/**
 * The active brush changed (refresh the live Vector sessions whose preview reads it). Strength and
 * blend are folded into the preview from the brush (#style_brush_values_from_brush), so a change
 * from any editor must re-composite. \a brush is the notifier reference: when non-null only
 * sessions whose image-paint brush is \a brush are refreshed; null refreshes every session in
 * \a scene. Shape settings stay isolated; only the brush is re-read.
 */
void ED_paint_shape_brush_update(const Main *bmain, const Scene *scene, const Brush *brush);

/**
 * The live Vector session's own #PaintShapeSettings copy when \a sima owns a session, else null.
 * The Image Editor's shape UI edits this copy so settings changes elsewhere never reach the
 * session and the shared settings stay untouched.
 */
PaintShapeSettings *ED_image_shape_session_settings_get(SpaceImage *sima);

/* `paint_shape_vector_3d.cc` — live Vector shape session of Sculpt Mode (Image canvas) */

namespace ed::sculpt_paint::shape {
struct PaintShapeSession;
}

/** The live Vector shape session of \a ob, or null. */
ed::sculpt_paint::shape::PaintShapeSession *ED_paint_shape_session_get(Object &ob);
bool ED_paint_shape_session_active(const Object &ob);
/** Cancel every live Vector session in \a bmain; called from the undo/redo pre-step. Returns true
 * when at least one session was cancelled, so the caller can consume the undo step (the session's
 * uncommitted preview takes the first undo, like its Esc). */
bool ED_paint_shape_sessions_cancel_all(bContext *C);
/** Context-less teardown: restore the preview and free (object deletion, mode exit). */
void ED_paint_shape_session_discard_on_session_end(Object &ob);
/** The live 3D session's own #PaintShapeSettings copy, or null. Edited by the Sculpt shape UI. */
PaintShapeSettings *ED_paint_shape_session_settings_get(Object &ob);
/** Runtime "Transform" flag of the live 3D session (cage of a parametric shape without an analytic SDF). */
bool ED_paint_shape_transform_is_active(const Object &ob);
/** True when \a ob owns a live 3D shape session whose write targets include \a image: an Image
 * Editor showing that image is "linked" to the session. */
bool ED_paint_shape_session_shows_image(const Object &ob, const Image &image);
/** True when the live session's active shape supports the Transform cage (a generated
 * parametric shape without an analytic SDF). */
bool ED_paint_shape_transform_is_available(const Object &ob);

}  // namespace blender
