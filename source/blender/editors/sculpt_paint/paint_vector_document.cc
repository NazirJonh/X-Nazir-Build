/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the runtime Vector document and its session undo; see
 * #paint_vector_document.hh.
 */

#include "paint_vector_document.hh"

#include <algorithm>

#include "MEM_guardedalloc.h"

#include "BKE_paint_vector.hh"

namespace blender::ed::sculpt_paint::shape {

VectorItemStyle vector_item_style_make(const PaintShapeSettings &settings)
{
  PaintShapeSettings *copy = MEM_new<PaintShapeSettings>(__func__);
  BKE_paint_vector_style_copy(*copy, settings);
  return VectorItemStyle(copy, [](PaintShapeSettings *style) {
    BKE_paint_vector_style_free(*style);
    MEM_delete(style);
  });
}

/** Restore a step's items into the live document, keeping the live active index (clamped). */
static void restore_step_items(VectorDocument &doc, const VectorDocumentStep &step)
{
  doc.items = step.items;
  doc.active = std::clamp(doc.active, 0, std::max(int(doc.items.size()) - 1, 0));
}

void vector_document_set_items(VectorDocument &doc, Vector<VectorItem> items)
{
  doc.items = std::move(items);
  doc.active = 0;
  doc.undo_steps.clear();
  doc.undo_step_current = -1;
  doc.undo_anchor_trimmed = false;
  /* Entry 0 is the start state, so the invariant holds before any edit. */
  vector_document_undo_push(doc);
}

void vector_document_undo_push(VectorDocument &doc)
{
  /* Anything above the cursor is a redo branch the new edit invalidates. */
  doc.undo_steps.resize(doc.undo_step_current + 1);
  doc.undo_steps.append(VectorDocumentStep{doc.items});

  if (doc.undo_steps.size() > VECTOR_DOCUMENT_UNDO_STEPS_MAX) {
    doc.undo_steps.remove(0);
    /* Entry 0 is no longer the start state; "back at 0" must not be read as "nothing to undo". */
    doc.undo_anchor_trimmed = true;
  }
  doc.undo_step_current = int(doc.undo_steps.size()) - 1;
}

bool vector_document_undo_back(VectorDocument &doc)
{
  if (doc.undo_step_current <= 0) {
    /* Curve Patch semantics: once the oldest step was trimmed, index 0 is no longer the start
     * state, so "already at 0" must NOT be read as "nothing left to undo / cancel the patch". */
    return doc.undo_anchor_trimmed;
  }
  doc.undo_step_current--;
  restore_step_items(doc, doc.undo_steps[doc.undo_step_current]);
  return true;
}

bool vector_document_undo_forward(VectorDocument &doc)
{
  if (doc.undo_step_current + 1 >= int(doc.undo_steps.size())) {
    return false;
  }
  doc.undo_step_current++;
  restore_step_items(doc, doc.undo_steps[doc.undo_step_current]);
  return true;
}

void vector_document_undo_restore(VectorDocument &doc)
{
  if (doc.undo_step_current < 0 || doc.undo_step_current >= doc.undo_steps.size()) {
    return;
  }
  restore_step_items(doc, doc.undo_steps[doc.undo_step_current]);
}

}  // namespace blender::ed::sculpt_paint::shape
