/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The bridge between a Stack Layers row (#MA_PAINT_LAYER_SOURCE_CURVE_PATCH) and the interactive
 * Curve Patch tool: entering the row's session (with the row's brush snapshot active), committing
 * the edited document back into the row's PaintVector, and the seeded Add wrapper the Outliner's
 * stack source uses.
 *
 * The state machine of a row is `Baked -> PassivePreview -> ActiveEditing`. The passive preview is
 * the overlay source #ED_curve_patch_overlay_data_get_stack (see `ED_curve_patch.hh`); the active
 * editing session is what this file enters and commits.
 */

#include <cstdint>
#include <memory>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

#include "BKE_curves.hh"
#include "BKE_paint_layers_edit.hh"

namespace blender {

struct bContext;
struct Brush;
struct Depsgraph;
enum ePaintLayerTargetMode : int8_t;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerCurvePatch;
struct Main;
struct Object;
struct Paint;
struct PaintVector;
struct PaintVectorItem;
struct ReportList;
struct Scene;
struct ViewContext;
struct wmOperatorType;
struct wmWindowManager;
class ToolBrushBindingsBackup;

namespace bke {
class CurvesGeometry;
}

}  // namespace blender

namespace blender::ed::sculpt_paint {

struct CurvePatchDocument;
struct CurvePatchSession;
struct ImageCurvePatchSession;

/** Fill the window, area, region and RegionView3D fields of \a r_vc from the first 3D Viewport of
 * \a wm, when there is one. Needs no `bContext`, so a caller with no view of its own (the headless
 * passive stamp) borrows the source view direction this way. False when \a wm has no usable one. */
bool curve_patch_view3d_find(wmWindowManager *wm, ViewContext &r_vc);

/**
 * The evaluated copy of \a curve_object the object source mode reads, or the original when the
 * graph has none (the object not yet evaluated, or no graph at all): modifiers and animation
 * reach the effect only through the evaluated copy, so every reader of the source -- the session's
 * publish and both overlays -- resolves it here instead of repeating the `DEG_get_evaluated` +
 * type check inline. BKE cannot call this (the layering forbids BKE -> editors); its refresh
 * resolves the same way inline (see `paint_layers_curve_patch_object.cc`).
 */
const Object *curve_patch_object_evaluated(const Depsgraph *depsgraph, const Object &curve_object);

/**
 * The ONE conversion the external-curve mode has: spline \a spline_i of \a curve_object's
 * CurvesGeometry (pass the copy #curve_patch_object_evaluated resolved), copied with its whole
 * attribute set, mapped into \a painted_object's local space, with the bezier handles recomputed
 * the way a publish requires and the radius convention filled. The session's document and both
 * overlays build the curve from this, so what an overlay draws is what the stamp stamps.
 *
 * \return false when the spline does not exist or is empty (\a r_spline is untouched).
 */
bool curve_patch_spline_from_object(const Object &curve_object,
                                    const Object &painted_object,
                                    int spline_i,
                                    bke::CurvesGeometry &r_spline);

/**
 * The control curve \a item reads from its external object (the evaluated copy through
 * \a depsgraph, mapped into \a painted_object's space), or false when the item names no object or
 * the spline cannot be read -- the item then reads its own geometry.
 */
bool curve_patch_item_object_curve(const PaintVectorItem &item,
                                   const Object &painted_object,
                                   const Depsgraph *depsgraph,
                                   bke::CurvesGeometry &r_curve);

/**
 * The transform that carries a point of \a curve_object's local space into \a painted_object's --
 * the space the object mode's published curves live in (#curve_patch_spline_from_object).
 */
float4x4 curve_patch_curve_to_painted_space(const Object &curve_object,
                                            const Object &painted_object);

/** The inverse of #curve_patch_curve_to_painted_space: the write-back's mapping back into the
 * curve object. Kept next to it so the two-way mapping lives in one module. */
float4x4 curve_patch_painted_to_curve_space(const Object &curve_object,
                                            const Object &painted_object);

/**
 * RAII: the single path that restores the previous brush on every exit (Apply / Discard / Failed /
 * object death). Holds the Scene and Paint it switched, the outgoing brush and the tool's brush
 * bindings; restores through the same user-facing switch (#BKE_paint_brush_set_synced, which
 * snapshots the PBR preset of whatever brush is outgoing at that moment) on destruction unless
 * #release was called.
 *
 * The bindings have to come back with the brush: the entry records the row's private snapshot in
 * them, and the next mode entry or tool refresh reads them instead of the brush pointer.
 *
 * With Material canvas brush sync on, the entry also pushes the row's snapshot into the paired
 * Paint (Sculpt <-> Image), so that one is captured and restored the same way: leaving it would
 * keep the session's brush in the other editor after the restore.
 *
 * A layer session owns one, so the restore survives every way the session can end -- including the
 * confirm dialog's teardown / relaunch round-trip, which destroys the modal's operator data while
 * the session lives on.
 */
class PrevBrushRestore {
 public:
  /** Capture \a paint's outgoing brush, to be restored on destruction. */
  PrevBrushRestore(Scene &scene, Paint &paint);
  PrevBrushRestore(const PrevBrushRestore &) = delete;
  PrevBrushRestore &operator=(const PrevBrushRestore &) = delete;
  ~PrevBrushRestore();

  /** Switch the paint to \a brush (the row's snapshot), remembering the outgoing brush. */
  bool activate(Brush &brush);
  /** Drop the captured brush: the destructor will not switch anything back. */
  void release();

 private:
  Scene *scene_ = nullptr;
  Paint *paint_ = nullptr;
  Brush *prev_brush_ = nullptr;
  /**
   * The tool's brush bindings as they were before the session switched the brush. The entry goes
   * through the tool system, which records the row's private snapshot in the binding of the
   * snapshot's brush type (the Sculpt Paint tool has its own "PAINT" binding); restoring only the
   * brush pointer would leave that binding behind and the next mode entry or tool refresh would
   * put the snapshot back in the hand -- and a switch between two effects would carry the first
   * snapshot into the second one's restored brush.
   */
  std::unique_ptr<ToolBrushBindingsBackup> bindings_backup_;
  /**
   * The Paint Material canvas brush sync pairs with \a paint_ (Sculpt <-> Image), when sync is on.
   * The entry pushes the row's snapshot into it too, so its brush and bindings are captured here
   * and restored as well. Null when there is no pair.
   */
  Paint *paired_paint_ = nullptr;
  Brush *prev_paired_brush_ = nullptr;
  std::unique_ptr<ToolBrushBindingsBackup> paired_bindings_backup_;
  bool released_ = false;
};

/**
 * Enter the row's curve-patch tool: activate the row's brush snapshot (user-facing semantics:
 * #BKE_paint_brush_set_synced snapshots the outgoing brush's PBR preset), build the session
 * document from the row's PaintVector, publish, and invoke `SCULPT_OT_curve_patch_edit`.
 *
 * The row must be a #MA_PAINT_LAYER_SOURCE_CURVE_PATCH row with a payload, and the active object a
 * mesh. Sculpt Mode is entered by the call itself when the object is not in it yet. False (with
 * nothing left behind) when any step refused: the brush restore is rolled back, no session is
 * published.
 *
 * A row whose Space is 2D routes to #curve_patch_layer_activate_2d instead.
 */
bool curve_patch_layer_activate(bContext &C, Material &ma, MaterialPaintLayer &row);

/**
 * The 2D Space entry: open the row's Curve Patch tool in the Image Editor instead of the 3D
 * Viewport. The container's curves are canvas UV coordinates; the session is the Image Editor's
 * #ImageCurvePatchSession, the stamp rasterizes directly into the UV space of the row's channel
 * maps, and the commit writes the curves back into the row's container
 * (#curve_patch_layer_commit_session_2d).
 *
 * Texture Paint mode is entered (both the editor's and the object's) by the call itself; an
 * Image Editor on the screen is REQUIRED -- without one the entry refuses with a report, exactly
 * like the 3D entry refuses without a 3D Viewport. False (with nothing left behind) when any
 * step refused.
 */
bool curve_patch_layer_activate_2d(bContext &C, Material &ma, MaterialPaintLayer &row);

/**
 * A press of the Curve Patch brush in the Image Editor while the active object's active Curve Patch
 * effect is in 2D Space: enter that effect's 2D session instead of letting the press start a new
 * anchor curve. The effect's curves are on screen (the passive UV preview), so the press is meant
 * for them. Returns true when the press was consumed (the entry was attempted); false when there is
 * no such effect, or a session of it is already live.
 */
bool curve_patch_layer_enter_active_2d_row(bContext &C);

/**
 * Enter the tool for the active Curve Patch effect of \a C's active object, if there is one and the
 * object is in Sculpt Mode: the automatic entry when Sculpt Mode is (re)entered, so a stroke of
 * work left by a mode exit resumes without a dialog. Returns false when nothing was entered.
 */
bool curve_patch_layer_enter_active_row(bContext &C);

/**
 * The automatic entry: selecting (or adding) a Curve Patch effect while the object is in Sculpt Mode
 * swaps the user's brush for the effect's snapshot and opens its session, so the next stroke
 * edits the effect's curves instead of painting with the previous brush. Outside Sculpt Mode this
 * does nothing (False); a refused entry reports the reason and also returns False.
 */
bool curve_patch_layer_enter_if_sculpting(bContext &C, Material &ma, MaterialPaintLayer &row);

/**
 * Apply-side wrapper: write the session document back into the row's PaintVector, tag it changed,
 * request the row bake, push a "Curve Patch Layer Edit" memfile step and notify the stack. No-op
 * for a session that was not entered from a row. Called on every successful commit of a layer
 * session, before the session is freed.
 */
void curve_patch_layer_commit_document(bContext &C, CurvePatchSession &session);

/**
 * Live half of a bend-curve session: write the document into the row's PaintVector and tag the
 * Text Decal that bends along it. No pixels are touched and the row is not baked; the material is
 * tagged and the next scene update runs the refresh, so the text follows the drag without the
 * modal re-entering the BLF rasterizer. No-op for a session that is not a bend curve.
 */
void curve_patch_layer_live_update(bContext &C, CurvePatchSession &session);

/**
 * The 2D counterpart of #curve_patch_layer_live_update: write the canonical UV curves into the
 * row's container and tag the bending Text Decal while the Image Editor session runs. Like the 3D
 * path, the raster is rebuilt by the following scene update, never inside the modal event.
 */
void curve_patch_layer_live_update_2d(bContext &C, ImageCurvePatchSession &session);

/**
 * Restore the curves a bend session opened with after a cancel: a live bend session writes the
 * container on every edit, so the pristine curves have to be put back explicitly.
 */
void curve_patch_layer_restore_session_2d(bContext &C, ImageCurvePatchSession &session);

/**
 * The 2D Space counterpart of #curve_patch_layer_commit_document: write the canonical UV curves
 * of the Image Editor session back into the row's PaintVector, tag it changed, request the bake,
 * push the "Curve Patch Layer Edit" memfile step and notify the stack. No-op when the row the
 * session was entered from no longer exists. Called by #image_curve_patch_session_commit for a
 * session with `is_layer` set, before the session is freed (the session's own brush restore runs
 * through its destructor right after).
 */
void curve_patch_layer_commit_session_2d(bContext &C, ImageCurvePatchSession &session);

/**
 * The cancel counterpart: no pixels, no container write-back and no undo step -- only the
 * stack-visible parts of the session's exit (the notifier the UI needs to unlock the row's
 * editing). No-op when the row is gone. Called by #image_curve_patch_session_cancel for a
 * session with `is_layer` set.
 */
void curve_patch_layer_cancel_session_2d(ImageCurvePatchSession &session);

/**
 * The 2D counterpart of #curve_patch_layer_active_item_follow: fold the container's per-curve
 * edits (name, Width, Opacity, Hide, Mirror) and its active item into the live 2D document, so the
 * Curve Patch panel works while the Image Editor session runs. Returns whether anything changed
 * (the caller re-stamps).
 */
bool curve_patch_layer_follow_2d(ImageCurvePatchSession &session);

/**
 * Whether \a ma is still a material of the file. A session addresses its material by raw pointer,
 * and an undo step or a library reload can free the material under a running modal: every access
 * through #CurvePatchSession::layer_material or #ImageCurvePatchSession::layer_material checks this
 * first, so a stale pointer is never dereferenced.
 */
bool curve_patch_layer_material_alive(const Material *ma);

/**
 * The Space a Curve Patch effect row works in. A row without a payload reads as 3D, the value every
 * file written before the Space existed reads back as. The one place that decides it, so no caller
 * compares the raw DNA field.
 */
inline eMaterialPaintLayerCurvePatchSpace curve_patch_layer_row_space(const MaterialPaintLayer &row)
{
  return row.curve_patch != nullptr ? eMaterialPaintLayerCurvePatchSpace(row.curve_patch->space) :
                                      MA_CURVE_PATCH_SPACE_3D;
}

/**
 * Write the live document into the row's container, tag the PaintVector changed and mark the row's
 * bake stale, with no undo step and no notifier: for the teardown paths that have no context to
 * commit through (the pixels of such a session are put back, the curves must not be lost). No-op
 * for a session that was not entered from a row.
 */
void curve_patch_layer_store_document(CurvePatchSession &session);
/**
 * Write the live document's curves that name an external Curves object into those objects, so the
 * object follows every edit of the session as it happens (not only the commit). A session that is
 * not a Stack Layers effect, or one re-publishing from the objects, writes nothing.
 */
void curve_patch_layer_sync_objects(CurvePatchSession &session);

/**
 * The save half of the same write: carry the live document into the row's container for a file
 * write, without tagging the PaintVector changed and without a bake request -- the live maps are
 * not stale, and a loaded file re-bakes anyway (the blend read clears the row's bake hash). No-op
 * for a session that was not entered from a row.
 */
void curve_patch_layer_store_document_for_save(CurvePatchSession &session);

/**
 * Sync the row's `PaintVector::active_item` with the session's active patch (through each patch's
 * remembered item index), so the passive overlay and the Active Tool list follow the Tab presses
 * of a live session. No-op for a session that was not entered from a row.
 */
void curve_patch_layer_active_item_sync(const CurvePatchSession &session);

/**
 * The opposite direction: make the session's active patch follow the row's
 * `PaintVector::active_item`, which the Active Tool list writes. Returns true when the active patch
 * changed, or when the per-curve look (opacity, width, hide) was edited in the list: the caller
 * re-stamps and redraws. No-op for a session that was not entered from a row.
 */
bool curve_patch_layer_active_item_follow(CurvePatchSession &session,
                                          bool *r_look_changed = nullptr);

/**
 * Rebuild the row's container curve items from the live document: the document is the truth while
 * a session runs (it is what undo restores), so an item whose patch is gone is dropped and a patch
 * without an item gets one. Shape items are left alone. No-op for a session that was not entered
 * from a row.
 */
void curve_patch_layer_mirror_vector(CurvePatchSession &session);

/**
 * Whether the overlay and the pointer picking are limited to the active curve of \a session (Solo,
 * the default), as opposed to showing every curve of the effect. False for a session that was not
 * entered from a row.
 */
bool curve_patch_layer_solo(const CurvePatchSession &session);

/**
 * Whether a live session edits a row OTHER than \a row: switching the stack's active row away
 * from the session's own would leave its curves half-edited. The Outliner's row activation
 * consults this before switching.
 */
bool curve_patch_layer_session_blocks_row(const CurvePatchSession &session,
                                          const MaterialPaintLayer &row);

/**
 * Whether a free-standing Curve Patch session (3D or 2D: a patch started on an ordinary layer, not
 * owned by a Curve Patch effect row) is alive in \a bmain, so the active row must not change: the
 * session has no row to keep its curves in and writes into the layer that was active when it
 * started. A session of an effect row never blocks: its state lives in the row's container, and a
 * switch commits it. The one pure predicate every switch path and the Outliner's pre-selection
 * check share; which row is about to become active makes no difference, so it takes none. The
 * checking-and-acting counterpart is #curve_patch_layer_row_switch_prepare.
 */
bool curve_patch_layer_row_switch_blocked(Main &bmain);

/**
 * Get the context ready for the active row to change, and answer whether it may. When the pure
 * #curve_patch_layer_row_switch_blocked finds a free-standing Curve Patch session, the option
 * `paint_layers_commit_patch_on_switch` commits it and the switch is allowed; otherwise the
 * refusal is reported (an INFO telling the user to commit first) and the switch is not allowed.
 * The name says so on purpose: this is not a check, it may commit a session.
 */
bool curve_patch_layer_row_switch_prepare(bContext &C);

/** What a brush stroke on the active Curve Patch effect row must do. The pure half of
 * #curve_patch_layer_stroke_target_resolve, so a test can drive the table without a context. */
enum class CurvePatchStrokeTarget : int8_t {
  /** The row's own snapshot brush is the stroke's brush: the row's tool handles it. */
  Allow,
  /** The row has no usable snapshot yet and the stroke's brush can seed one. */
  Adopt,
  /** The row's tool has to take the brush: the stroke is cancelled and the tool entered. */
  Redirect,
  /** The stroke's brush cannot paint the row at all. */
  Refuse,
};

/**
 * Decide what a stroke with \a brush on the Curve Patch effect \a row does. #Adopt is answered
 * only when \a brush is a Paint Brush (#BKE_paint_layers_curve_patch_brush_is_assignable) and the
 * row has no snapshot; #Refuse distinguishes "the effect has no brush", "the brush cannot stamp a
 * patch", and a row snapshot that is not the brush in hand (#Redirect, the tool has to take over).
 *
 * The decision is pure: it reads the row's payload, the brush, and whether the row's session is
 * live. Carrying it out is #curve_patch_layer_stroke_target_resolve.
 */
CurvePatchStrokeTarget curve_patch_layer_stroke_target(const MaterialPaintLayer &row,
                                                       const Brush *brush,
                                                       bool row_session_live);

/**
 * #curve_patch_layer_stroke_target, carried out: #Adopt seeds the row's snapshot from \a brush and
 * #Adopt/#Redirect enter the row's tool, reporting what they did; #Refuse reports its reason. This
 * is the side-effectful half -- the decision itself is the pure #curve_patch_layer_stroke_target.
 *
 * Returns the final target: #Allow when the stroke should paint normally, every other answer
 * cancels it. #Adopt can turn into #Refuse when the row could not take the brush in hand.
 */
CurvePatchStrokeTarget curve_patch_layer_stroke_target_resolve(bContext &C,
                                                               Material &material,
                                                               MaterialPaintLayer &row,
                                                               const Brush *brush,
                                                               ReportList *reports);

/**
 * Commit the live session of \a ob, if any, into its effect and end it: the silent counterpart of
 * the confirm dialog, for a switch the user can undo (another row of the stack becoming active).
 */
void curve_patch_layer_commit_live_session(bContext &C, Object &ob);

/**
 * Commit EVERY live layer session of the file, whatever object runs it, except (when \a
 * target_marker is given) the session of the row that is being (re)activated. A session that
 * stayed behind would keep its effect's brush in the hand after the user's brush was restored,
 * and its stroke polls would keep blocking the paint -- so the paths where the active row changes
 * without a user switch that could be refused (a row removal, a mode exit, a target-mode change)
 * commit the strays first. A user switch to another row is refused instead, see
 * #curve_patch_layer_row_switch_blocked.
 */
void curve_patch_layer_commit_live_sessions(bContext &C, const bUUID *target_marker = nullptr);

/**
 * The data-level activation of a stack row, shared by the Outliner's row click and the viewport
 * pick: a live Curve Patch session of ANOTHER row refuses the switch (nothing changes, the reason
 * is reported), the row becomes the active one, a Curve Patch effect enters its tool, the target
 * mode follows the row's masks, and the texture-paint slots resolve through the new active row.
 * The material must be editable. Returns false (with nothing changed) when it is not, or when a
 * live session refused the switch.
 *
 * \param curve_item_index: when non-negative and still valid, the Curve Patch effect's container
 * makes that item active before the session opens, so the clicked curve is the one it starts on.
 * The check happens before any mutation, so a refused activation leaves `active_item` untouched.
 *
 * UI that follows the activation (Outliner row selection, reveal) observes the
 * `NC_MATERIAL | ND_SHADING` notifier sent here.
 */
bool paint_stack_row_activate(bContext &C,
                              Material &material,
                              const bUUID &row_marker,
                              int curve_item_index = -1);

/**
 * Make a Gradient row active in Sculpt Mode, remembering the user's tool, and restore that tool
 * when a row that is not a Gradient becomes active.
 */
void paint_stack_gradient_tool_activate(bContext &C, const MaterialPaintLayer *layer);

/** The dedicated `PaintVector <-> CurvePatchDocument` converter (the shapes converter produces a
 * `VectorDocument`, not this document). Only `PAINT_VECTOR_ITEM_CURVE_PATCH` items are read;
 * Shape items keep their indices untouched on the way back. Each patch remembers its item index in
 * #CurvePatchItem::source_curve_index, and `active_patch` matches `PaintVector::active_item`
 * clamped to the curve items.
 */
void curve_patch_document_from_vector(const PaintVector &pv, CurvePatchDocument &r_doc);
/**
 * Replace the control curve of every patch of \a r_doc whose container item names an external
 * Curves object by that object's spline (see #curve_patch_item_object_curve): the document the
 * session shows and stamps then carries the object's shape, while the frozen parameters stay the
 * item's. Patches are matched to items through #CurvePatchItem::source_curve_index.
 */
void curve_patch_document_apply_objects(const PaintVector &pv,
                                        const Object &painted_object,
                                        const Depsgraph *depsgraph,
                                        CurvePatchDocument &r_doc);
/** Write the document's patches back into the Curve Patch items of \a r_pv. Patches whose
 * #CurvePatchItem::source_curve_index is -1 (created inside the session) are appended; existing
 * Shape items and their order are never disturbed. */
void curve_patch_document_to_vector(const CurvePatchDocument &doc, PaintVector &r_pv);

/**
 * Give the payload's empty container its first control curve: one two-point bezier item, sized
 * from the snapshot brush (or a unit fallback when the brush has no radius yet), with the frozen
 * parameters the brush would stamp with. Called by the Add path; a container that already holds
 * items is left alone.
 */
void ED_paint_layers_curve_patch_row_seed_curve(MaterialPaintLayerCurvePatch &payload);

/**
 * Add a Curve Patch effect beside the Text Decal \a text_row, born in the 3D Space with one
 * segment across the decal frame's middle and every channel on but the row's opacity at zero, and
 * make its container the text's bend curve. The effect is how the curve is edited: the Sculpt Mode
 * curve tool, with the decal's frame telling where the segment starts. \a painted is the object
 * whose space the curve is written in.
 *
 * \return the new effect, or null when the row has no owner to hang it on or the effect could not
 * be added or its curve taken as the bend; nothing is left behind in either case.
 */
MaterialPaintLayer *ED_paint_layers_curve_patch_bend_row_add(bContext &C,
                                                            Object &painted,
                                                            Material &ma,
                                                            MaterialPaintLayer &text_row);

/**
 * Ensure the row has a map for every channel its bake renders: the enabled records in the
 * material's channel set (Base Color and the active paint slot when the row carries no records
 * yet). The interactive Image effect resolves its targets from exactly these maps, so the live
 * preview shows the same channels the committed bake will. False when no map could be created.
 */
bool ED_paint_layers_curve_patch_row_maps_ensure(bContext &C,
                                                Material &ma,
                                                MaterialPaintLayer &row);

/**
 * Reset one map the session is about to re-render: a content target addresses the row's channel
 * record, a mask target its single Base Color record through the item. Shared by the interactive
 * entry and the passive object-source rebuild so the two cannot disagree on the mask/content
 * branch.
 */
bool curve_patch_layer_row_map_reset(Material &ma, MaterialPaintLayer &row, int channel);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve list operators
 *
 * The Active Tool panel's Add / Delete / Rename for the row's curve list. The persisted container
 * is a DNA array, so these are C operators.
 * \{ */

void PAINT_OT_curve_patch_layer_curve_add(wmOperatorType *ot);
void PAINT_OT_curve_patch_layer_curve_remove(wmOperatorType *ot);
void PAINT_OT_curve_patch_layer_curve_duplicate(wmOperatorType *ot);
void PAINT_OT_curve_patch_layer_curve_move(wmOperatorType *ot);
/** Internal: enters the active effect's tool one event-loop tick after a mode change. */
void PAINT_OT_curve_patch_layer_resume(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tool entry and brush assignment
 * \{ */

/** Enter the active row's tool (its brush snapshot becomes the active brush). */
void PAINT_OT_curve_patch_layer_edit(wmOperatorType *ot);
/** Re-snapshot the active row's brush from `brush`, or the active brush when unset. */
void PAINT_OT_curve_patch_layer_brush_assign(wmOperatorType *ot);

/** \} */

}  // namespace blender::ed::sculpt_paint
