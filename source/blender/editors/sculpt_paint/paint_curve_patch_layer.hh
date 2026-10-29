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

#include "BLI_math_vector_types.hh"

#include "BKE_paint_layers_edit.hh"

namespace blender {

struct bContext;
struct Brush;
enum ePaintLayerTargetMode : int8_t;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerCurvePatch;
struct Main;
struct Object;
struct Paint;
struct PaintVector;
struct Scene;
struct wmOperatorType;

}  // namespace blender

namespace blender::ed::sculpt_paint {

struct CurvePatchDocument;
struct CurvePatchSession;

/**
 * RAII: the single path that restores the previous brush on every exit (Apply / Discard / Failed /
 * object death). Holds the Scene and Paint it switched, and the outgoing brush; restores through
 * the same user-facing switch (#BKE_paint_brush_set_synced, which snapshots the PBR preset of
 * whatever brush is outgoing at that moment) on destruction unless #release was called.
 *
 * A layer session owns one, so the restore survives every way the session can end -- including the
 * confirm dialog's teardown / relaunch round-trip, which destroys the modal's operator data while
 * the session lives on.
 */
class PrevBrushRestore {
 public:
  PrevBrushRestore() = default;
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
 */
bool curve_patch_layer_activate(bContext &C, Material &ma, MaterialPaintLayer &row);

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
 * Write the live document into the row's container and mark the row's bake stale, with no undo step
 * and no notifier: for the teardown paths that have no context to commit through (the pixels of
 * such a session are put back, the curves must not be lost). No-op for a session that was not
 * entered from a row.
 */
void curve_patch_layer_store_document(CurvePatchSession &session);

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
/** The same question asked of \a ob's live session: false when it has none, or when the session was
 * not entered from a Stack Layers effect (an anchor session never blocks a row switch). */
bool curve_patch_layer_session_blocks_row(const Object &ob, const MaterialPaintLayer &row);

/**
 * Commit the live session of \a ob, if any, into its effect and end it: the silent counterpart of
 * the confirm dialog, for a switch the user can undo (another row of the stack becoming active).
 */
void curve_patch_layer_commit_live_session(bContext &C, Object &ob);

/**
 * The data-level activation of a stack row, shared by the Outliner's row click and the viewport
 * pick: a live Curve Patch session the switch would leave half-edited commits first, the row
 * becomes the active one, a Curve Patch effect enters its tool, the target mode follows the row's
 * masks, and the texture-paint slots resolve through the new active row. The material must be
 * editable. Returns false (with nothing changed) when it is not.
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
 * Ensure the row has a map for every channel its bake renders: the enabled records in the
 * material's channel set (Base Color and the active paint slot when the row carries no records
 * yet). The interactive Image effect resolves its targets from exactly these maps, so the live
 * preview shows the same channels the committed bake will. False when no map could be created.
 */
bool ED_paint_layers_curve_patch_row_maps_ensure(bContext &C,
                                                Material &ma,
                                                MaterialPaintLayer &row);

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
