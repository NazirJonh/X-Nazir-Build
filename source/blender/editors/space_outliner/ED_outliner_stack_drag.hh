/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edoutliner
 *
 * The drag payload of a Stack Layers row. The type belongs to the Outliner: WM only carries the
 * pointer and calls the free function the Outliner sets on the drag (#wmDrag::poin_free_fn), so
 * neither WM_types.hh nor WM code names the payload's type.
 */

#include "MEM_guardedalloc.h"

#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/**
 * Drag data for a Stack Layers row (texture layer, group, channel).
 *
 * Runtime-only and dynamically allocated with #MEM_new. The source row and owner are encoded as
 * #StackItemIdentity values (session UID + source type + row ID + ordinal hint) rather than
 * pointers, since they may be freed or renumbered before the drop happens. The drop target is
 * resolved each frame from the cursor position -- see `outliner_dragdrop.cc` for that half of the
 * dance.
 *
 * Unlike most drag payloads, this carries *multiple* source identities, for multi-selection drag:
 * all selected rows move together, preserving their relative order. An empty vector means the
 * drag is invalid (mid-construction or after a failed resolution).
 *
 * What the drop would look like on screen -- the indicator's row and zone -- is the Outliner's
 * own business, kept in the space's runtime rather than here.
 */
struct wmDragStackLayer {
  /** Identities of all dragged rows (from selection). Empty = invalid drag. */
  blender::Vector<StackItemIdentity> drag_rows;

  /** `target_owner_uid == 0` until a drop zone under the cursor has resolved to one. */
  uint32_t target_owner_uid;
  short target_source_type; /* #eSpaceOutliner_StackSource. */
  bUUID target_row_id;
  short target_ordinal_hint;
  short target_place; /* #StackMovePlace. */
};

/**
 * Free a #wmDragStackLayer payload, through #wmDrag::poin_free_fn.
 *
 * The payload owns a #blender::Vector, which allocates: a plain `MEM_delete_void` would free the
 * struct without running its destructor and leak the buffer.
 */
inline void stack_layer_drag_payload_free(void *payload)
{
  MEM_delete(static_cast<wmDragStackLayer *>(payload));
}

}  // namespace blender::ed::outliner
