/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Copy and paste of Stack Layers rows and of their masks. The clipboards hold row identities, so
 * the source keeps ownership of the data it duplicates.
 */

#include <algorithm>
#include <climits>
#include <functional>
#include <set>
#include <string>

#include "DNA_ID.h"
#include "DNA_defs.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_color.h"
#include "BLI_mempool.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "ED_object.hh"
#include "ED_outliner_stack.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_iterator.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_layers_intern.hh"

namespace blender::ed::outliner {

/* The Stack Layers clipboard holds row identities rather than raw source data. A paste asks the
 * source to duplicate those rows, so every source keeps ownership of its data and copy semantics. */
static Vector<StackItemIdentity> stack_layer_clipboard;

/* The row whose mask was copied last. It is a clipboard of its own so a copied mask is never
 * pasted as a layer, and the two are exclusive: copying either one empties the other, which is
 * what lets Ctrl+V mean the thing that was copied last. */
static StackItemIdentity stack_mask_clipboard;

wmOperatorStatus stack_row_copy_exec(bContext *C, wmOperator *op)
{
  stack_mask_clipboard = StackItemIdentity();
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  if (selected.is_empty()) {
    const int active_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                                 *space_outliner);
    if (active_ordinal >= 0) {
      selected.append(active_ordinal);
    }
  }
  /* A row kept by a folder is already carried by that folder; the copy takes only the roots. */
  stack_ordinals_drop_covered_descendants(*space_outliner, selected);

  stack_layer_clipboard.clear();
  for (const int ordinal : selected) {
    const StackItemIdentity identity = outliner_stack_identity_of(*space_outliner, ordinal);
    if (identity.is_valid()) {
      stack_layer_clipboard.append(identity);
    }
  }
  if (stack_layer_clipboard.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stack layers to copy");
    return OPERATOR_CANCELLED;
  }
  BKE_reportf(
      op->reports, RPT_INFO, "Copied %d stack layer(s)", int(stack_layer_clipboard.size()));
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_paste_exec(bContext *C, wmOperator *op)
{
  if (stack_layer_clipboard.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stack layers to paste");
    return OPERATOR_CANCELLED;
  }

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const int active_ordinal = outliner_stack_active_ordinal_get(ctx, *space_outliner);
  if (active_ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  /* The first copy lands above the active row; each further copy above the one before it, so the
   * pasted rows keep the order they were copied in. */
  StackItemIdentity anchor = outliner_stack_identity_of(*space_outliner, active_ordinal);
  if (!anchor.is_valid()) {
    return OPERATOR_CANCELLED;
  }
  bool pasted_any = false;
  int last_ordinal = -1;
  for (const StackItemIdentity &source_identity : stack_layer_clipboard) {
    const int source_ordinal = outliner_stack_identity_resolve(
        ctx, *space_outliner, source_identity);
    const int anchor_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, anchor);
    if (source_ordinal < 0 || anchor_ordinal < 0) {
      continue;
    }

    int copied_ordinal = -1;
    bool pasted_onto_layer = false;
    const bool copied = stack_mutate(
        *C,
        *space_outliner,
        true,
        [&](const StackSource & /*source*/,
            const StackEditor &editor,
            const StackFocus &focus,
            ID &owner,
            int &r_select_ordinal) {
          /* A correction or a mask item goes onto the layer the paste is aimed at, whichever layer
           * it was copied from; a duplicate would only make another one beside the original. */
          const int onto = editor.row_paste_correction(
              *C, focus, owner, source_ordinal, anchor_ordinal);
          if (onto != STACK_PASTE_NOT_APPLICABLE) {
            pasted_onto_layer = true;
            r_select_ordinal = onto;
            return onto >= 0;
          }
          r_select_ordinal = editor.row_duplicate(*C, focus, owner, source_ordinal);
          return r_select_ordinal >= 0;
        },
        &copied_ordinal);
    if (!copied || copied_ordinal < 0) {
      continue;
    }
    if (pasted_onto_layer) {
      /* It sits where its layer keeps it; there is no row to move above the anchor. */
      last_ordinal = copied_ordinal;
      pasted_any = true;
      continue;
    }

    const StackItemIdentity copied_identity = outliner_stack_identity_of(*space_outliner,
                                                                         copied_ordinal);
    if (!copied_identity.is_valid()) {
      continue;
    }
    const int copied_ordinal_now = outliner_stack_identity_resolve(
        ctx, *space_outliner, copied_identity);
    const int anchor_ordinal_now = outliner_stack_identity_resolve(ctx, *space_outliner, anchor);
    if (copied_ordinal_now < 0 || anchor_ordinal_now < 0 ||
        !outliner_stack_row_move(
            C, *space_outliner, copied_ordinal_now, anchor_ordinal_now, StackMovePlace::Above))
    {
      continue;
    }
    anchor = copied_identity;
    last_ordinal = copied_ordinal_now;
    pasted_any = true;
  }

  if (!pasted_any) {
    BKE_report(op->reports, RPT_INFO, "Could not paste the copied stack layers here");
    return OPERATOR_CANCELLED;
  }
  if (last_ordinal >= 0) {
    outliner_stack_row_activate(C, *space_outliner, last_ordinal);
  }
  BKE_reportf(
      op->reports, RPT_INFO, "Pasted %d stack layer(s)", int(stack_layer_clipboard.size()));
  return OPERATOR_FINISHED;
}

bool stack_row_clipboard_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  const SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->outlinevis == SO_STACK_LAYERS &&
         space_outliner->stack_layers_view == SO_SL_VIEW_STACK;
}

wmOperatorStatus stack_row_mask_copy_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const StackItemIdentity identity = (ordinal >= 0) ?
                                         outliner_stack_identity_of(*space_outliner, ordinal) :
                                         StackItemIdentity();
  if (!identity.is_valid()) {
    return OPERATOR_CANCELLED;
  }
  stack_layer_clipboard.clear();
  stack_mask_clipboard = identity;
  BKE_report(op->reports, RPT_INFO, "Copied the mask");
  return OPERATOR_FINISHED;
}

bool stack_row_mask_paste_poll(bContext *C)
{
  return stack_mask_clipboard.is_valid() && stack_row_clipboard_poll(C);
}

wmOperatorStatus stack_row_mask_paste_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const int target_ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const int source_ordinal = outliner_stack_identity_resolve(
      ctx, *space_outliner, stack_mask_clipboard);
  if (target_ordinal < 0 || source_ordinal < 0) {
    BKE_report(op->reports, RPT_INFO, "The copied mask is no longer there");
    return OPERATOR_CANCELLED;
  }
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr ||
            !grouping->row_aux_copy(*C, focus, owner, source_ordinal, target_ordinal))
        {
          return false;
        }
        r_select_ordinal = target_ordinal;
        return true;
      });
  if (!ok) {
    BKE_report(op->reports, RPT_INFO, "Could not paste the mask onto this layer");
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

}  // namespace blender::ed::outliner
