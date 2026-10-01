/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Runtime Vector document: the editable list of shape items a Vector session works on, plus the
 * session-local undo history. No target, no context: the session (`paint_shape_edit.hh`) and the
 * frontends drive it, and the persistence ID will serialize it.
 *
 * The undo history holds STATES with a cursor, mirroring #CurvePatchDocument::undo_steps: entry
 * 0 is the state the document started in and a new snapshot truncates any redo branch above the
 * cursor. Not a stack of deltas.
 *
 * Invariant: at rest (no gesture in flight) `undo_steps[undo_step_current]` equals the live
 * #VectorDocument::items. A gesture mutates the live items and, on a successful end, records the
 * post-state with #vector_document_undo_push; on cancel the caller restores the entry at the
 * cursor. #vector_document_undo_push is therefore called AFTER a change.
 *
 * The same header declares the converters between the runtime document and the persisted
 * `PaintVector` ID (#paint_vector_from_document / #paint_vector_to_document).
 */

#pragma once

#include <cstdint>
#include <memory>

#include "BLI_vector.hh"

#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

#include "paint_shape.hh"
#include "paint_shape_space.hh"

namespace blender {
struct PaintVector;
}

namespace blender::ed::sculpt_paint::shape {

enum class VectorItemType : int8_t {
  /** A shape drawn by the shape tools. */
  Shape,
  /** A Curve Patch control curve. */
  CurvePatch,
};

/**
 * Immutable, shared style snapshot of one #VectorItem. #PaintShapeSettings owns profiles and
 * color ramps, so it cannot be copied bitwise into undo steps; sharing one deep copy keeps item
 * copies cheap and frees the owned data with the last reference. Null means "default style".
 */
using VectorItemStyle = std::shared_ptr<const PaintShapeSettings>;

/** Deep-copy \a settings into a new shared snapshot (see #VectorItemStyle). */
VectorItemStyle vector_item_style_make(const PaintShapeSettings &settings);

/** One element of the document: the geometry plus the space it lives in.
 *
 * A Shape item uses #shape / #space. A CurvePatch item uses #control_curve / #params
 * instead: only the user-editable control curve and the frozen parameters live here, never the
 * derived #bke::CurvePatchGeometry (spline / ribbon LUT / frames / surface snapshot), which is a
 * session cache rebuilt after undo / redo / cancel. `bke::CurvesGeometry` is implicitly shared, so
 * a `VectorDocument` undo step copies a CurvePatch item cheaply. Shape items keep the CurvePatch
 * fields empty. */
struct VectorItem {
  VectorItemType type = VectorItemType::Shape;

  /* Shape item. */
  PaintShape shape;
  ShapeSpaceDesc space;

  /* CurvePatch item (ignored for Shape). */
  bke::CurvesGeometry control_curve;
  bke::CurvePatchParams params;

  /** The style this item is drawn and persisted with. It travels with the item through add /
   * delete / reorder and undo, so nothing is matched by index. */
  VectorItemStyle style;
};

/** One snapshot of the session-local undo stack: the items (geometry and style). */
struct VectorDocumentStep {
  Vector<VectorItem> items;
};

struct VectorDocument {
  /** Every element, in creation order; composites in order. */
  Vector<VectorItem> items;
  /**
   * Index into #items the session acts on; consumers must validate it.
   *
   * Not part of the undo history (variant (a), like selection / the active object in Blender's
   * other editors): changing it (a click on another shape, Tab) is not an undoable step. On
   * #vector_document_undo_push / _back / _forward the live value is kept and only clamped to the
   * restored item count.
   */
  int active = 0;

  /** Session-local undo states (see the file comment). */
  Vector<VectorDocumentStep> undo_steps;
  int undo_step_current = -1;
  /** True once the oldest step was trimmed; index 0 is then no longer the start state. */
  bool undo_anchor_trimmed = false;
};

/** Deep enough for any realistic session; bounds a pathological one. */
constexpr int VECTOR_DOCUMENT_UNDO_STEPS_MAX = 64;

/** Replace the items and reseed the undo history with the new state as entry 0. */
void vector_document_set_items(VectorDocument &doc, Vector<VectorItem> items);

/**
 * Record the current live state as a new undo entry. Call AFTER a change; drops any redo branch
 * above the cursor. Keeps the invariant `undo_steps[undo_step_current] == items` when it returns.
 */
void vector_document_undo_push(VectorDocument &doc);
/** Step the cursor back, restoring the previous state; false at the start. */
bool vector_document_undo_back(VectorDocument &doc);
/** Step the cursor forward, restoring the next state; false with no redo. */
bool vector_document_undo_forward(VectorDocument &doc);
/** Restore the state at the cursor without touching the history (a gesture cancel). */
void vector_document_undo_restore(VectorDocument &doc);

/* -------------------------------------------------------------------- */
/** \name Persistence Converters
 *
 * Converters between the runtime #VectorDocument and the persisted `PaintVector` ID.
 *
 * Every item carries its own style (#VectorItem::style); the persistence ID embeds a
 * `PaintShapeSettings` snapshot per item, so the two directions copy it item by item.
 * \{ */

/** Write \a doc into \a r_pv, snapshotting each item's own style (a default style for an item
 * without one). Clears \a r_pv first. */
void paint_vector_from_document(PaintVector &r_pv, const VectorDocument &doc);

/** Read \a pv (geometry and per-item style) into \a r_doc (its undo history is reset). */
void paint_vector_to_document(const PaintVector &pv, VectorDocument &r_doc);

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
