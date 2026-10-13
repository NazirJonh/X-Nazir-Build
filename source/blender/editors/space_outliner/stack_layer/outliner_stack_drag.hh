/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The drag payload of a Stack Layers row. The type belongs to the Outliner: WM only carries the
 * pointer and calls the free function the Outliner sets on the drag (#wmDrag::poin_free_fn), so
 * neither WM_types.hh nor WM code names the payload's type.
 */

#pragma once

#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/**
 * Drag data for a Stack Layers row.
 *
 * Runtime-only and dynamically allocated with #MEM_new. The source rows and the target are
 * #StackItemIdentity values (session UID + source type + row ID + ordinal hint) rather than
 * pointers, since they may be freed or renumbered before the drop happens. The drop target is
 * resolved each frame from the cursor position -- see `outliner_stack_dragdrop.cc` for that half
 * of the dance.
 *
 * Unlike most drag payloads, this carries *multiple* source identities, for multi-selection drag:
 * all selected rows move together, preserving their relative order. An empty vector means the
 * drag is invalid (mid-construction or after a failed resolution).
 *
 * What the drop would look like on screen -- the indicator's row and zone -- is the Outliner's
 * own business, kept in the space's runtime rather than here.
 */
struct StackLayerDragData {
  /** Identities of all dragged rows, in stack order. Empty = invalid drag. */
  Vector<StackItemIdentity> drag_rows;

  /** Invalid (`owner_uid == 0`) until a drop zone under the cursor has resolved to one. */
  StackItemIdentity target;
  StackMovePlace target_place = StackMovePlace::Above;
};

/**
 * Free a #StackLayerDragData payload, through #wmDrag::poin_free_fn.
 *
 * The payload owns a #Vector, which allocates: a plain `MEM_delete_void` would free the struct
 * without running its destructor and leak the buffer.
 */
inline void stack_layer_drag_payload_free(void *payload)
{
  MEM_delete(static_cast<StackLayerDragData *>(payload));
}

}  // namespace blender::ed::outliner
