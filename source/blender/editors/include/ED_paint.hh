/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_scene_types.h"
#include "DNA_view3d_enums.h"

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
struct UndoStep;
struct UndoType;
struct View3D;
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
 * Restore painting image to previous state. Used for anchored and drag-dot style brushes.
 */
void ED_image_undo_restore(UndoStep *us);

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
 * Clear the intermediate paint-curve geometry and detach from the source object.
 * Called when the user explicitly removes the source via the UI clear button so that
 * the Curve Edit tool starts fresh with an empty canvas.
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
 */
void ED_paintcurve_geometry_update_after_edit(struct PaintCurve *pc);

/**
 * Append one control point at the end of the active spline, starting a spline when the geometry is
 * empty, and return its point index.
 *
 * `radius` follows this codebase's paint-curve convention, where 1.0 means "full brush size" --
 * NOT #blender::bke::CurvesGeometry::radius()'s hair-oriented 0.01 default.
 *
 * The point gets AUTO bezier handles, unlike the ALIGN ones an interactive click leaves: a click is
 * the first half of a gesture that drags the handle out next, while a caller here supplies
 * positions and nothing else.
 */
int ED_paintcurve_geometry_add_point(struct PaintCurve *pc,
                                     const float position[3],
                                     float radius);

/** Drop every point and spline, leaving an empty but initialized geometry. */
void ED_paintcurve_geometry_clear(struct PaintCurve *pc);

/* `paint_curve_patch_apply.cc`, for callers outside this module -- the RNA layer. */

/**
 * The Curve Patch build parameters a brush implies for a patch driven from OUTSIDE a stroke: the
 * brush's own settings plus the four values a stroke would otherwise freeze -- world radius, radius
 * per unit of the Size slider, projection plane and stamp seed.
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

/* `paint_curve_patch_session.cc`: read-only view of a RUNNING Curve Patch edit, for the RNA layer.
 *
 * The session type itself lives in this module's private header, so the handle below is opaque and
 * everything about it is read through these accessors. Every one of them tolerates a null handle,
 * which is what "no patch is being edited" looks like. */

/** The live session published on `ob`, or null when no Curve Patch edit is running on it. */
const void *ED_curve_patch_session_get(const Object &ob);

/** Control point count of the session's live control curve. */
int ED_curve_patch_session_point_num(const void *session);
/** Index of the point the modal editor last acted on, or -1. Already validated against the curve. */
int ED_curve_patch_session_active_point(const void *session);
/** Whether the live control curve closes back on itself. */
bool ED_curve_patch_session_is_cyclic(const void *session);
/** World-space brush radius frozen when the patch started. */
float ED_curve_patch_session_radius(const void *session);
/** Stamp count of the last build; zero in Ribbon mode, which lays out none. */
int ED_curve_patch_session_stamp_num(const void *session);

/**
 * The live control point positions, in object space.
 *
 * The span points INTO the session and is invalidated by the next re-stamp, so a caller must copy
 * anything it intends to keep. Empty for a null handle.
 */
Span<float3> ED_curve_patch_session_positions(const void *session);

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

/**
 * Register the WM paint-cursor that tags the viewport for redraw when the mouse moves
 * (Unit D). Must be called once during editor init (after WM is ready).
 * Safe to call multiple times — guards against double-registration internally.
 */
void ED_paint_curve_overlay_redraw_register();

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

}  // namespace blender
