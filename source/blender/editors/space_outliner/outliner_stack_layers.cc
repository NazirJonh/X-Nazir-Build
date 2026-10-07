/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Navigation and activation for the Stack Layers display mode.
 *
 * Everything here is phrased in rows and ordinals. What a row *is* -- a paint layer, a shape key,
 * whatever comes next -- is the business of the #StackSource the space is set to, so the operators
 * below never mention a material, an image or a node.
 */

#include <algorithm>
#include <climits>
#include <cstdio>
#include <functional>

#include "DNA_defs.h"
#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
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
#include "ED_outliner_stack_automation.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_define.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "tree/tree_iterator.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/** Defined below: the shared resolver of the operators that address one row, marker first. */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op);
const ed::outliner::StackRow *outliner_stack_row_resolve(const SpaceOutliner &space_outliner,
                                                         const bUUID &row_id,
                                                         int ordinal_hint);

namespace {

/* The Stack Layers clipboard holds row identities rather than raw source data. A paste asks the
 * source to duplicate those rows, so every source keeps ownership of its data and copy
 * semantics. */
Vector<StackItemIdentity> stack_layer_clipboard;

/* The kind of rows the clipboard holds: false for plain rows, true for rows attached to a
 * parent's content section. The two never share a clipboard, because they paste through
 * different paths (a plain row is duplicated beside its anchor, an attached row is
 * re-attached into a target row), and a mixed copy would have to pick the half the user did not
 * copy. Set by every copy, read by a paste the identities of which no longer resolve -- one that
 * crossed between owners. */
static bool stack_layer_clipboard_attached = false;

bool stack_row_activate_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->is_editable(*owner);
}

/** The same question every mutating stack operator asks: is there a stack here to remove from,
 * and does its source take edits at all. */
bool stack_row_remove_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  const StackSource *source = stack_source_for_space(*space_outliner);
  return owner != nullptr && source->can_edit(*owner);
}

wmOperatorStatus stack_focus_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Main *bmain = CTX_data_main(C);
  char object_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "object", object_name);
  Object *object = id_cast<Object *>(BKE_libblock_find_name(bmain, ID_OB, object_name));
  if (object == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Object not found");
    return OPERATOR_CANCELLED;
  }
  const int sub_index = RNA_int_get(op->ptr, "sub_index");
  const bool enter_edit_mode = RNA_boolean_get(op->ptr, "enter_edit_mode");
  return outliner_stack_focus_set(C, *space_outliner, *object, sub_index, enter_edit_mode) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

wmOperatorStatus stack_back_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (RNA_boolean_get(op->ptr, "keep_view")) {
    /* Staying in the Stack Layers mode only steps back to the objects list. */
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
  }
  else {
    /* A plain invocation leaves the Stack Layers mode for the regular View Layer browsing. */
    space_outliner->outlinevis = SO_VIEW_LAYER;
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
    /* Leaving the mode through an operator skips #rna_SpaceOutliner_display_mode_update, so the
     * tool header that carries only the Stack Layers controls is synced here through the same
     * helper the RNA update calls. */
    ScrArea *area = CTX_wm_area(C);
    if (area != nullptr) {
      outliner_tool_header_visibility_sync(area, *space_outliner);
      ED_area_tag_redraw(area);
    }
  }
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus stack_back_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* The modifier is read once, at invoke: an exec reached from a script or from Repeat Last has
   * no event to read, and an operator whose behavior is not in its properties is not
   * reproducible. */
  RNA_boolean_set(op->ptr, "keep_view", (event->modifier & KM_ALT) != 0);
  return stack_back_exec(C, op);
}

wmOperatorStatus stack_pin_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  space_outliner->stack_layers_flag ^= SO_SL_PINNED;
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_activate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  return outliner_stack_row_activate(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
}

/**
 * Remember every row's UI state by identity, before the rows go away.
 *
 * Clicks and collapses live in the tree store, keyed by owner and ordinal: state a row keeps only
 * while no edit renumbers it. Read into #SpaceOutliner_Runtime::stack_row_ui_state here, while the
 * tree and the rows still agree on what each ordinal means, this is what re-attaches collapsed and
 * selected to the same rows after the edit -- whichever ordinals they land on.
 *
 * Called from #outliner_stack_rows_invalidate, which every path that re-reads the rows goes
 * through, so nothing that renumbers has to remember to ask.
 */
void stack_row_ui_state_capture(SpaceOutliner &space_outliner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* Once an edit has re-read the rows, the tree store still addresses the previous ordinals.
   * Capturing it again would overwrite identity state with the row now sitting at each old
   * ordinal.
   */
  if (runtime.stack_owner_uid == 0 || runtime.stack_rows_rebuilt_since_build) {
    return;
  }
  tree_iterator::all(space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type != TSE_STACK_LAYER) {
      return;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, int(tselem->nr));
    if (row == nullptr || BLI_uuid_is_nil(row->stable_id)) {
      return;
    }
    StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row->stable_id);
    state.closed = (tselem->flag & TSE_CLOSED) != 0;
    state.selected = (tselem->flag & TSE_SELECTED) != 0;
    state.active = (tselem->flag & TSE_ACTIVE) != 0;
  });
}

/**
 * The selection an edit just left behind: the row it names reads as selected and active once the
 * tree rebuilds, and every other row reads as not -- an edit renumbers the stack, and without
 * this the tree store would hand the old ordinal's selection to whichever row moved into it.
 *
 * The edit has already run, so the rows re-read here are the renumbered ones and \a select_ordinal
 * addresses them. -1 names nothing -- an edit that took a row away, such as a remove or an
 * ungroup -- and only clears. The map is what carries the selection across the rebuild; a row
 * without a #StackRow::stable_id cannot be addressed by it and keeps the tree store's state.
 */
void stack_row_ui_state_edit_select(const StackReadContext &ctx,
                                    SpaceOutliner &space_outliner,
                                    ID &owner,
                                    const int select_ordinal)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  outliner_stack_rows_ensure(ctx, space_outliner, owner);
  for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
    state.selected = false;
    state.active = false;
  }
  if (select_ordinal < 0) {
    return;
  }
  const StackItemIdentity identity = outliner_stack_identity_of(space_outliner, select_ordinal);
  if (BLI_uuid_is_nil(identity.row_id)) {
    return;
  }
  StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(identity.row_id);
  state.selected = true;
  state.active = true;
}

/**
 * The prologue and epilogue every mutating wrapper below shares: resolve the owner, get the
 * source's editor, run \a fn, invalidate the cached rows and notify on success.
 *
 * \param fn: `bool(const StackSource &source, const StackEditor &editor, const StackFocus &focus,
 * ID &owner, int &r_select_ordinal)` -- the one call that differs between callers, including
 * whichever guard it needs beyond an editor existing at all: #StackEditor::can_reorder for a
 * reorder, #StackSource::is_editable for the rest of what used to be separate `can_*` predicates,
 * or nothing at all for an edit that never had one. Returning false leaves the stack exactly as
 * this found it. \a r_select_ordinal starts at -1; a caller whose edit renumbers the stack sets it
 * to whichever row should read as selected once the tree rebuilds.
 * \param needs_renumber: whether the edit changes which row sits at which ordinal at all. A
 * rename or a visibility toggle does not, and leaves every row under the tree-store key it
 * already had -- no selection to re-target. \a r_ordinal: when given, receives \a
 * select_ordinal, for a caller that needs the row's new position for something of its own, such
 * as activating it.
 */
template<typename Fn>
bool stack_mutate(bContext &C,
                  SpaceOutliner &space_outliner,
                  const bool needs_renumber,
                  Fn &&fn,
                  int *r_ordinal = nullptr)
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  const StackEditor *editor = source.editor();
  /* The one editability check of the middle layer: the polls grey the UI out, the source's own
   * methods guard the seam, and a check in every lambda here would only be a third copy of the
   * same question. */
  if (editor == nullptr || !source.is_editable(*owner)) {
    return false;
  }
  int select_ordinal = -1;
  if (!fn(source, *editor, space_outliner.runtime->stack_focus, *owner, select_ordinal)) {
    return false;
  }
  if (r_ordinal != nullptr) {
    *r_ordinal = select_ordinal;
  }
  /* Invalidating captures the rows' UI state into the identity-keyed map before the rows go
   * away; a renumbering edit then re-targets the selection on the map, which is what carries it
   * across the rebuild. */
  outliner_stack_rows_invalidate(space_outliner);
  if (needs_renumber) {
    stack_row_ui_state_edit_select(ctx, space_outliner, *owner, select_ordinal);
  }
  WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return true;
}

/**
 * The ordinal of the row the operator's \a marker names, or -1.
 *
 * A marker is the row's identity -- #StackRow::stable_id, as an earlier call of the mode handed it
 * out. A script that re-addresses a row by its marker gets the same row after an edit moved it,
 * where a remembered ordinal is the position-trap the seam's own code avoids through that same
 * marker; the position properties stay for UI buttons, which speak for the row on screen at the
 * moment of the call.
 */
static int stack_operator_marker_ordinal_get(bContext &C,
                                             SpaceOutliner &space_outliner,
                                             wmOperator &op)
{
  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op.ptr, "marker", marker_str);
  if (marker_str[0] == '\0') {
    return -1;
  }
  bUUID marker;
  if (!BLI_uuid_parse_string(&marker, marker_str)) {
    return -1;
  }
  const StackReadContext ctx = outliner_stack_read_context(C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return -1;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_resolve(space_outliner, marker, -1);
  return (row != nullptr) ? row->ordinal : -1;
}

/**
 * The ordinal \a row shifts to for a one-step move up or down the listed order, or -1 when no
 * sibling sits in that direction.
 *
 * Rows attached to a parent's content section share the run of ordinals with the plain rows around
 * them but only reorder among themselves, so the step walks past everything the sibling predicate
 * refuses -- an attached row never lands among plain rows, and a plain row never lands between
 * another row's attached rows.
 */
static int stack_row_sibling_ordinal_find(const SpaceOutliner &space_outliner,
                                          const StackRow &row,
                                          const bool move_up)
{
  const int step = move_up ? 1 : -1;
  for (int candidate = row.ordinal + step; candidate >= 0 && candidate <= STACK_ROW_ORDINAL_MAX;
       candidate += step)
  {
    const StackRow *candidate_row = outliner_stack_row_find(space_outliner, candidate);
    if (candidate_row != nullptr && stack_rows_are_siblings(row, *candidate_row)) {
      return candidate;
    }
  }
  return -1;
}

wmOperatorStatus stack_row_move_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);

  /* A marker or position the operator was given names one row; without one, the whole selection
   * moves together, and with nothing selected the active row -- the same vocabulary every other
   * operator reads. #stack_operator_ordinal_get cannot be shared here, because it answers a
   * selection with its first row, and a move of one row is not a move of the selection. */
  Vector<int> ordinals_to_move;
  int ordinal = stack_operator_marker_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    ordinal = RNA_int_get(op->ptr, "ordinal");
  }
  if (ordinal >= 0) {
    ordinals_to_move.append(ordinal);
  }
  else {
    stack_selected_ordinals_get(*space_outliner, ordinals_to_move);
  }
  if (ordinals_to_move.is_empty()) {
    /* Nothing picked: the row the user was on, which is what a bare Move has always meant. */
    const int active_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                                 *space_outliner);
    if (active_ordinal < 0) {
      return OPERATOR_CANCELLED;
    }
    ordinals_to_move.append(active_ordinal);
  }

  /* Up is toward the top of the screen, and the screen lists a stack top first: row 0 is the
   * bottom, so up means a larger ordinal. The block walks like a line of people, its leading row
   * first -- the row closest to where the block is going -- which is the highest ordinal moving
   * up and the lowest moving down: any other order makes the block step over itself. */
  const bool move_up = RNA_enum_get(op->ptr, "direction") == 0;
  if (move_up) {
    std::sort(ordinals_to_move.begin(), ordinals_to_move.end(), std::greater<int>());
  }
  else {
    std::sort(ordinals_to_move.begin(), ordinals_to_move.end());
  }

  /* Move each row one sibling step in the direction, resolving its ordinal fresh: every move
   * renumbers the stack, so an ordinal read before a move can name a different row after it. The
   * step lands on the nearest sibling rather than the next ordinal -- rows attached to a parent's
   * section and rows under another parent share the run of ordinals but reorder among themselves
   * only, so they are passed over rather than landed between. */
  bool any_moved = false;
  for (const int original_ordinal : ordinals_to_move) {
    const StackItemIdentity identity = outliner_stack_identity_of(*space_outliner,
                                                                  original_ordinal);
    if (!identity.is_valid()) {
      continue; /* Row disappeared mid-operation. */
    }
    const StackReadContext ctx = outliner_stack_read_context(*C);
    const int current_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, identity);
    if (current_ordinal < 0) {
      continue;
    }
    const StackRow *current_row = outliner_stack_row_find(*space_outliner, current_ordinal);
    if (current_row == nullptr) {
      continue;
    }
    const int target_ordinal = stack_row_sibling_ordinal_find(*space_outliner,
                                                              *current_row,
                                                              move_up);
    if (target_ordinal < 0) {
      continue;
    }
    if (outliner_stack_row_reorder(C, *space_outliner, current_ordinal, target_ordinal)) {
      any_moved = true;
    }
  }

  return any_moved ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_copy_exec(bContext *C, wmOperator *op)
{
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

  /* The kind the copy carries, named by the active row when it is one of the copied rows, and by
   * the first of them otherwise: plain rows copy as plain rows, rows attached to a parent's
   * content section copy as those. Rows of the other kind are left out rather than mixed in -- a
   * paste reads one kind out of the clipboard, and the report says what was not taken. */
  int kind_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                       *space_outliner);
  if (kind_ordinal < 0 || !selected.contains(kind_ordinal)) {
    kind_ordinal = selected.is_empty() ? -1 : selected[0];
  }
  const StackRow *kind_row = (kind_ordinal >= 0) ?
                                 outliner_stack_row_find(*space_outliner, kind_ordinal) :
                                 nullptr;
  const bool copy_attached = (kind_row != nullptr) && !kind_row->parent_section_id.empty();

  int skipped_kind = 0;
  Vector<int> copied_rows;
  for (const int ordinal : selected) {
    const StackRow *row = outliner_stack_row_find(*space_outliner, ordinal);
    const bool row_attached = (row != nullptr) && !row->parent_section_id.empty();
    if (row_attached != copy_attached) {
      skipped_kind++;
      continue;
    }
    copied_rows.append(ordinal);
  }
  if (skipped_kind > 0) {
    BKE_reportf(op->reports, RPT_INFO, "Skipped %d row(s) of a different kind", skipped_kind);
  }
  /* A folder kept by the kind filter still carries every row below it; running the walk after the
   * filter also keeps an attached row whose parent row was selected -- the folder would drop the
   * row as covered before the filter had said which kind the copy carries. */
  stack_ordinals_drop_covered_descendants(*space_outliner, copied_rows);

  stack_layer_clipboard.clear();
  for (const int ordinal : copied_rows) {
    const StackItemIdentity identity = outliner_stack_identity_of(*space_outliner, ordinal);
    if (identity.is_valid()) {
      stack_layer_clipboard.append(identity);
    }
  }
  if (stack_layer_clipboard.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stack layers to copy");
    return OPERATOR_CANCELLED;
  }
  stack_layer_clipboard_attached = copy_attached;
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

  /* The paste path the clipboard asks for. An identity that still resolves is the authority on
   * the kind it carries; when none does -- a clipboard pasted into another owner's stack -- the
   * flag the copy set is what still names it. */
  bool paste_attached = false;
  const int first_ordinal = outliner_stack_identity_resolve(ctx,
                                                            *space_outliner,
                                                            stack_layer_clipboard.first());
  const StackRow *first_row = (first_ordinal >= 0) ?
                                  outliner_stack_row_find(*space_outliner, first_ordinal) :
                                  nullptr;
  if (first_row != nullptr) {
    paste_attached = !first_row->parent_section_id.empty();
  }
  else {
    paste_attached = stack_layer_clipboard_attached;
  }
  if (paste_attached) {
    /* Rows attached to a parent's content section paste into rows rather than beside them, and
     * the source owns how. Everything below runs the source's paste vocabulary per target row. */
    ID *paste_owner = outliner_stack_owner_get(ctx, *space_outliner);
    if (paste_owner == nullptr) {
      BKE_report(op->reports, RPT_INFO, "Nothing to paste");
      return OPERATOR_CANCELLED;
    }
    outliner_stack_rows_ensure(ctx, *space_outliner, *paste_owner);

    /* The rows the copies hang on: every selected plain row, or the active row when nothing is
     * selected. An attached active row aims at its parent row -- pasting onto an attached row is
     * pasting beside it, onto the parent both hang under. */
    Vector<int> targets;
    Vector<int> selected;
    stack_selected_ordinals_get(*space_outliner, selected);
    int skipped_kind = 0;
    for (const int ordinal : selected) {
      const StackRow *row = outliner_stack_row_find(*space_outliner, ordinal);
      if (row != nullptr && row->parent_section_id.empty()) {
        targets.append(ordinal);
      }
      else {
        skipped_kind++;
      }
    }
    if (targets.is_empty()) {
      const int active_ordinal = outliner_stack_active_ordinal_get(ctx, *space_outliner);
      const StackRow *active_row = (active_ordinal >= 0) ?
                                       outliner_stack_row_find(*space_outliner, active_ordinal) :
                                       nullptr;
      if (active_row != nullptr) {
        const int aim_ordinal = active_row->parent_section_id.empty() ?
                                    active_ordinal :
                                    active_row->parent_ordinal;
        if (aim_ordinal >= 0 && outliner_stack_row_find(*space_outliner, aim_ordinal) != nullptr)
        {
          targets.append(aim_ordinal);
        }
      }
    }
    if (targets.is_empty()) {
      BKE_report(op->reports, RPT_INFO, "Nothing to paste");
      return OPERATOR_CANCELLED;
    }
    if (skipped_kind > 0) {
      BKE_reportf(op->reports, RPT_INFO, "Skipped %d row(s) of a different kind", skipped_kind);
    }

    /* Adding an attached row does not move the rows around it -- a new one takes its ordinal from
     * the top of the range -- so this is not a renumbering: the selection below is what carries
     * the new rows' state across the rebuild. */
    Vector<StackItemIdentity> pasted;
    for (const int target_ordinal : targets) {
      Vector<StackItemIdentity> created;
      const bool pasted_into = stack_mutate(
          *C,
          *space_outliner,
          false,
          [&](const StackSource & /*source*/,
              const StackEditor &editor,
              const StackFocus &focus,
              ID &owner,
              int & /*r_select_ordinal*/) {
            bool pasteable = false;
            for (const StackItemIdentity &source_identity : stack_layer_clipboard) {
              if (editor.can_paste_into(ctx, focus, owner, source_identity, target_ordinal)) {
                pasteable = true;
                break;
              }
            }
            if (!pasteable) {
              return false;
            }
            return editor.rows_paste_into(*C,
                                          focus,
                                          owner,
                                          stack_layer_clipboard,
                                          target_ordinal,
                                          created,
                                          op->reports);
          });
      if (pasted_into) {
        pasted.extend(created);
      }
    }

    if (pasted.is_empty()) {
      BKE_report(op->reports, RPT_INFO, "Nothing to paste");
      return OPERATOR_CANCELLED;
    }

    SpaceOutliner_Runtime &runtime = *space_outliner->runtime;
    for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
      state.selected = false;
      state.active = false;
    }
    for (const StackItemIdentity &identity : pasted) {
      /* A row the source gives no identity for cannot be addressed by this map; keying it at nil
       * would fold every such row into one shared state. */
      if (BLI_uuid_is_nil(identity.row_id)) {
        continue;
      }
      runtime.stack_row_ui_state.lookup_or_add_default(identity.row_id).selected = true;
    }
    const int last_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, pasted.last());
    if (last_ordinal >= 0) {
      outliner_stack_row_activate(C, *space_outliner, last_ordinal);
      if (!BLI_uuid_is_nil(pasted.last().row_id)) {
        StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(
            pasted.last().row_id);
        state.active = true;
        state.selected = true;
      }
    }
    BKE_reportf(op->reports, RPT_INFO, "Pasted %d item(s)", int(pasted.size()));
    return OPERATOR_FINISHED;
  }

  const int active_ordinal = outliner_stack_active_ordinal_get(ctx, *space_outliner);
  if (active_ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  StackItemIdentity anchor = outliner_stack_identity_of(*space_outliner, active_ordinal);
  if (!anchor.is_valid()) {
    return OPERATOR_CANCELLED;
  }

  Vector<StackItemIdentity> pasted;
  for (const StackItemIdentity &source_identity : stack_layer_clipboard) {
    const int source_ordinal = outliner_stack_identity_resolve(
        ctx, *space_outliner, source_identity);
    const int anchor_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, anchor);
    if (source_ordinal < 0 || anchor_ordinal < 0) {
      continue;
    }

    int copied_ordinal = -1;
    const bool copied = stack_mutate(
        *C,
        *space_outliner,
        true,
        [&](const StackSource & /*source*/,
            const StackEditor &editor,
            const StackFocus &focus,
            ID &owner,
            int &r_select_ordinal) {
          r_select_ordinal = editor.row_duplicate(*C, focus, owner, source_ordinal);
          return r_select_ordinal >= 0;
        },
        &copied_ordinal);
    if (!copied || copied_ordinal < 0) {
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
    pasted.append(copied_identity);
    anchor = copied_identity;
  }

  if (pasted.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "Could not paste the copied stack layers here");
    return OPERATOR_CANCELLED;
  }

  SpaceOutliner_Runtime &runtime = *space_outliner->runtime;
  for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
    state.selected = false;
    state.active = false;
  }
  for (const StackItemIdentity &identity : pasted) {
    /* A row the source gives no identity for cannot be addressed by this map; keying it at nil
     * would fold every such row into one shared state. */
    if (BLI_uuid_is_nil(identity.row_id)) {
      continue;
    }
    runtime.stack_row_ui_state.lookup_or_add_default(identity.row_id).selected = true;
  }
  const int last_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, pasted.last());
  if (last_ordinal >= 0) {
    outliner_stack_row_activate(C, *space_outliner, last_ordinal);
    if (!BLI_uuid_is_nil(pasted.last().row_id)) {
      StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(
          pasted.last().row_id);
      state.active = true;
      state.selected = true;
    }
  }
  BKE_reportf(op->reports, RPT_INFO, "Pasted %d stack layer(s)", int(pasted.size()));
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_remove_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* A marker names the row by identity and wins over the position; without one, the position the
   * operator was given addresses the single row, and -1 falls through to the selection. */
  int ordinal = stack_operator_marker_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    ordinal = RNA_int_get(op->ptr, "ordinal");
  }
  if (ordinal >= 0) {
    return outliner_stack_row_remove(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
  }
  /* The whole selection goes at once, the highest ordinal first: a removal renumbers the rows
   * above it, so walking down the stack keeps every remaining ordinal what it was when read. */
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  if (selected.is_empty()) {
    /* Nothing picked: the row the user was on, which is what a bare Remove has always meant. */
    const int active_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                                 *space_outliner);
    if (active_ordinal < 0) {
      return OPERATOR_CANCELLED;
    }
    selected.append(active_ordinal);
  }
  /* A layer or folder whose Mask section is the one showing is working on its mask: the key then
   * takes the mask with every correction over it, not the row. The Content section and a
   * correction row still remove the whole row. */
  if (selected.size() == 1) {
    const int target_ordinal = selected.first();
    const StackRow *target_row = outliner_stack_row_find(*space_outliner, target_ordinal);
    const bool is_active = target_ordinal == outliner_stack_active_ordinal_get(
                                                 outliner_stack_read_context(*C), *space_outliner);
    if (is_active && target_row != nullptr && target_row->parent_section_id.empty() &&
        !stack_mask_section_id(*space_outliner).is_empty() &&
        outliner_stack_row_active_section_get(*space_outliner, *target_row) ==
            stack_mask_section_id(*space_outliner))
    {
      const bool mask_removed = stack_mutate(
          *C,
          *space_outliner,
          false,
          [&](const StackSource & /*source*/,
              const StackEditor &editor,
              const StackFocus &focus,
              ID &owner,
              int & /*r_select_ordinal*/) {
            const StackGroupingEditor *grouping = editor.grouping();
            return grouping != nullptr &&
                   grouping->row_mask_set(*C, focus, owner, target_ordinal, false, nullptr);
          });
      if (mask_removed) {
        /* The row is still there, with nothing left to switch to: show its content again. */
        /* The mutation rebuilt the rows, so the row is looked up again. */
        if (const StackRow *rebuilt = outliner_stack_row_find(*space_outliner, target_ordinal)) {
          const StringRefNull default_section = stack_default_section_id(*space_outliner);
          if (!default_section.is_empty()) {
            outliner_stack_row_active_section_set(
                *space_outliner, *rebuilt, default_section);
          }
        }
        return OPERATOR_FINISHED;
      }
    }
  }
  bool removed_any = false;
  for (int64_t index = selected.size() - 1; index >= 0; index--) {
    removed_any |= outliner_stack_row_remove(C, *space_outliner, selected[index]);
  }
  return removed_any ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

/** The Add kind \a kind as the space's source declares it; false when it declares no such kind. */
static bool stack_add_kind_info_get(const bContext &C, const int kind, StackAddKindInfo &r_info)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackEditor *editor = stack_source_for_space(*space_outliner)->editor();
  if (editor == nullptr) {
    return false;
  }
  Vector<StackAddKindInfo> kinds;
  editor->add_kinds(kinds);
  if (!kinds.index_range().contains(kind)) {
    return false;
  }
  r_info = std::move(kinds[kind]);
  return true;
}

wmOperatorStatus stack_row_add_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int kind = RNA_enum_get(op->ptr, "type");
  StackAddKindInfo kind_info;
  if (!stack_add_kind_info_get(*C, kind, kind_info)) {
    return OPERATOR_CANCELLED;
  }

  StackAddArgs args;
  /* A kind made from an existing data-block is handed it by name rather than by a pointer the UI
   * held on to, so the call stays what the info log shows: a repeat or a script finds the same
   * data-block again. */
  char source_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "source", source_name);
  if (kind_info.source_id_type != 0) {
    args.source = (source_name[0] != '\0') ?
                      BKE_libblock_find_name(
                          CTX_data_main(C), kind_info.source_id_type, source_name) :
                      nullptr;
    if (args.source == nullptr) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  RPT_("No data-block named \"%s\" to make the layer from"),
                  source_name);
      return OPERATOR_CANCELLED;
    }
  }

  /* The row the Add is anchored to: a marker or the explicit "ordinal" property when a script gave
   * one, otherwise the selection and then the active row. -1 names no row and puts the new layer
   * on top of the stack. The source decides what "anchored to" means -- directly above the row,
   * or, when the row is a folder, inside it -- so a row inside a folder keeps the new layer in
   * that folder. */
  const int anchor_ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  /* The section the anchor row is showing, for a kind that hangs off one of its anchor's sections:
   * the Add lands where the user is looking, the way an anchor inside a folder keeps the folder.
   * Sources whose kinds mean nothing by a section ignore this. */
  if (anchor_ordinal >= 0) {
    const StackRow *anchor_row = outliner_stack_row_find(*space_outliner, anchor_ordinal);
    if (anchor_row != nullptr) {
      args.section_id = outliner_stack_row_active_section_get(*space_outliner, *anchor_row);
    }
  }
  float gamma_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float linear_color[4];
  RNA_float_get_array(op->ptr, "fill_color", gamma_color);
  /* The operator's colour is a picker colour (gamma); the description stores scene linear. */
  srgb_to_linearrgb_v4(linear_color, gamma_color);
  if (kind_info.takes_color) {
    args.color = linear_color;
  }
  return outliner_stack_row_add(C, *space_outliner, kind, anchor_ordinal, args) >= 0 ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

/** The RNA-Main collection the Add searches a source kind's data-blocks in. Null for a type with
 * no collection to search, where the dialog falls back to naming the data-block exactly. */
static const char *stack_add_source_collection_name(const short idcode)
{
  switch (idcode) {
    case ID_AC:
      return "actions";
    case ID_CA:
      return "cameras";
    case ID_CU_LEGACY:
      return "curves";
    case ID_CV:
      return "hair_curves";
    case ID_GP:
      return "grease_pencils";
    case ID_IM:
      return "images";
    case ID_LA:
      return "lights";
    case ID_MA:
      return "materials";
    case ID_ME:
      return "meshes";
    case ID_NT:
      return "node_groups";
    case ID_TE:
      return "textures";
    case ID_WO:
      return "worlds";
    default:
      return nullptr;
  }
}

/**
 * The Add's invoke: a kind whose creation takes a colour opens the color picker first, a kind made
 * from a data-block the call names none of opens the search for it, the rest run straight through.
 * The picker is what "add a fill layer" means -- the colour is the layer's content, not an option
 * to be changed afterwards. The search is what choosing a material means; a repeat or a script
 * names the data-block itself and never sees it.
 *
 * A confirm dialog rather than a redo popup: every change in a redo popup re-runs the whole add,
 * and dragging a colour would create and throw away a set of full-size maps per mouse move.
 */
static wmOperatorStatus stack_row_add_invoke(bContext *C,
                                             wmOperator *op,
                                             const wmEvent * /*event*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  StackAddKindInfo kind_info;
  const bool kind_known = space_outliner != nullptr && space_outliner->runtime != nullptr &&
                          stack_add_kind_info_get(*C, RNA_enum_get(op->ptr, "type"), kind_info);
  PropertyRNA *source_prop = RNA_struct_find_property(op->ptr, "source");
  const bool needs_source = kind_known && kind_info.source_id_type != 0 &&
                            source_prop != nullptr && !RNA_property_is_set(op->ptr, source_prop);
  if (!kind_known || (!kind_info.takes_color && !needs_source)) {
    return stack_row_add_exec(C, op);
  }
  return WM_operator_props_dialog_popup(C, op, 220, std::nullopt, IFACE_("Add"));
}

/**
 * Only the choices the kind asks for are the user's here: a colour for a kind that takes one, the
 * source data-block for a kind made from one. The anchor, the kind and the marker are what the
 * button that placed the call already decided.
 */
static void stack_row_add_ui(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  StackAddKindInfo kind_info;
  if (space_outliner == nullptr || space_outliner->runtime == nullptr ||
      !stack_add_kind_info_get(*C, RNA_enum_get(op->ptr, "type"), kind_info))
  {
    return;
  }
  if (kind_info.takes_color) {
    op->layout->prop(op->ptr, "fill_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (kind_info.source_id_type != 0) {
    const char *collection_name = stack_add_source_collection_name(kind_info.source_id_type);
    if (collection_name != nullptr) {
      PointerRNA main_ptr = RNA_main_pointer_create(CTX_data_main(C));
      op->layout->prop_search(op->ptr, "source", &main_ptr, collection_name, std::nullopt, ICON_NONE);
    }
    else {
      op->layout->prop(op->ptr, "source", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
}

bool stack_row_add_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
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

/**
 * The Add operator's own poll, on top of #stack_row_add_poll: a source that declares no kinds of
 * rows gets no Add at all, rather than an Add that would refuse whatever it was asked for.
 */
bool stack_add_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<StackAddKindInfo> kinds;
  if (const StackEditor *editor = stack_source_for_space(*space_outliner)->editor()) {
    editor->add_kinds(kinds);
  }
  return !kinds.is_empty();
}

wmOperatorStatus stack_row_visibility_toggle_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  if (ID *owner = outliner_stack_owner_get(ctx, *space_outliner)) {
    outliner_stack_rows_ensure(ctx, *space_outliner, *owner);
  }
  const StackRow *row = (ordinal < 0) ? nullptr :
                                         outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return OPERATOR_CANCELLED;
  }
  return outliner_stack_row_set_enabled(C, *space_outliner, ordinal, !row->enabled) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

bool stack_row_visibility_toggle_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
}

/**
 * The sub-selections of the focused object, as enum items.
 *
 * Built from the source's own sub-selections, so the Outliner offers "which material slot" without
 * knowing that a slot is what a paint stack's sub-index means; the item's preview icon comes from
 * the data-block the source named, when it named one.
 */
const EnumPropertyItem *stack_sub_index_itemf_impl(bContext *C,
                                                    SpaceOutliner &space_outliner,
                                                    bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  if (C != nullptr && space_outliner.runtime != nullptr) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    Vector<StackSubSelection> selections;
    stack_source_for_space(space_outliner)
        ->sub_selections_get(ctx, space_outliner.runtime->stack_focus, selections);
    for (const int index : selections.index_range()) {
      /* The identifier has to survive the item array, and a slot number is stable while the name
       * is not; the name is what the user reads. */
      char identifier[16];
      SNPRINTF_UTF8(identifier, "%d", index);
      EnumPropertyItem item = {};
      item.value = index;
      item.identifier = BLI_strdup(identifier);
      item.name = BLI_strdup(selections[index].name.empty() ? IFACE_("Empty Slot") :
                                                              selections[index].name.c_str());
      item.description = "";
      if (selections[index].preview_id != nullptr) {
        item.icon = ui::icon_id_preview_get(C, selections[index].preview_id);
      }
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

const EnumPropertyItem *stack_sub_index_itemf(bContext *C,
                                              PointerRNA * /*ptr*/,
                                              PropertyRNA * /*prop*/,
                                              bool *r_free)
{
  SpaceOutliner *space_outliner = (C != nullptr) ? CTX_wm_space_outliner(C) : nullptr;
  if (space_outliner == nullptr) {
    *r_free = true;
    return nullptr;
  }
  return stack_sub_index_itemf_impl(C, *space_outliner, r_free);
}

/**
 * The kinds of rows the source can add, as enum items.
 *
 * Built from the editor's own list, so the Add UI offers the domain's vocabulary without the
 * Outliner knowing it: the item's value is the kind's place in that list, which is what
 * #StackEditor::row_add reads back, and the identifier is the stable name scripts see.
 */
const EnumPropertyItem *stack_add_type_itemf(bContext *C,
                                             PointerRNA * /*ptr*/,
                                             PropertyRNA * /*prop*/,
                                             bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  SpaceOutliner *space_outliner = (C == nullptr) ? nullptr : CTX_wm_space_outliner(C);
  if (space_outliner != nullptr && space_outliner->runtime != nullptr) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
    const StackSource &source = *stack_source_for_space(*space_outliner);
    Vector<StackAddKindInfo> kinds;
    if (owner != nullptr && source.can_edit(*owner)) {
      if (const StackEditor *editor = source.editor()) {
        editor->add_kinds(kinds);
      }
    }
    for (const int64_t index : kinds.index_range()) {
      EnumPropertyItem item = {};
      item.value = int(index);
      item.identifier = BLI_strdup(kinds[index].identifier.c_str());
      /* The source's own strings are untranslated vocabulary; the display is where they meet the
       * user's language, names in the interface domain and descriptions in the tooltip one. */
      item.name = BLI_strdup(IFACE_(kinds[index].name.c_str()));
      item.description = BLI_strdup(TIP_(kinds[index].description.c_str()));
      item.icon = kinds[index].icon;
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

wmOperatorStatus stack_sub_index_set_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  outliner_stack_focus_sub_index_set(*space_outliner, RNA_enum_get(op->ptr, "sub_index"));
  return outliner_stack_focus_sub_index_apply(*C, *space_outliner) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

bool stack_sub_index_set_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->runtime != nullptr &&
         outliner_stack_focus_object_get(outliner_stack_read_context(*C),
                                         space_outliner->runtime->stack_focus) != nullptr;
}

wmOperatorStatus stack_rows_group_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  int from_ordinal = ordinal;
  int to_ordinal = RNA_int_get(op->ptr, "to_ordinal");
  if (to_ordinal < 0) {
    /* The selection is the run to wrap. One row on its own is a group of one, which is what "put
     * this in a folder" means when nothing else is picked. */
    Vector<int> selected;
    stack_selected_ordinals_get(*space_outliner, selected);
    if (selected.size() > 1) {
      from_ordinal = selected.first();
      to_ordinal = selected.last();
      if (to_ordinal - from_ordinal + 1 != selected.size()) {
        BKE_report(op->reports, RPT_ERROR, "Only layers next to each other can be grouped");
        return OPERATOR_CANCELLED;
      }
    }
    else {
      to_ordinal = from_ordinal;
    }
  }
  /* Wrapping a run of rows inserts the folder row among them, so everything above it is
   * renumbered; the folder itself is what the user is left working with. */
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
        if (grouping == nullptr) {
          return false;
        }
        r_select_ordinal = grouping->rows_group(*C, focus, owner, from_ordinal, to_ordinal);
        return r_select_ordinal >= 0;
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_group_add_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* -1 means the top of the stack, which is where a folder goes when nothing is pointed at. */
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  /* An empty folder is inserted into the stack, so every row above it has a new ordinal. */
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
        if (grouping == nullptr) {
          return false;
        }
        r_select_ordinal = grouping->group_add(*C, focus, owner, ordinal);
        return r_select_ordinal >= 0;
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_ungroup_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  /* The folder row is gone and its contents took its place, so there is no row left to hand the
   * selection to -- but the rows that moved up still have to be stripped of the state the tree
   * store holds under their new ordinals. */
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        const StackGroupingEditor *grouping = editor.grouping();
        return grouping != nullptr && grouping->row_ungroup(*C, focus, owner, ordinal) >= 0;
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_merge_down_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 1) {
    /* Nothing below the bottom of the stack to merge into. */
    return OPERATOR_CANCELLED;
  }
  /* The pair reads as one group row from then on, and that row is what the user works with. */
  int new_ordinal = -1;
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
        if (grouping == nullptr) {
          return false;
        }
        r_select_ordinal = grouping->row_merge_down(*C, focus, owner, ordinal);
        return r_select_ordinal >= 0;
      },
      &new_ordinal);
  if (!ok) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_row_activate(C, *space_outliner, new_ordinal);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_duplicate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        r_select_ordinal = editor.row_duplicate(*C, focus, owner, ordinal);
        return r_select_ordinal >= 0;
      },
      &new_ordinal);
  if (!ok) {
    return OPERATOR_CANCELLED;
  }
  /* The copy sits directly above the original; that is the seam's own promise for
   * #StackEditor::row_duplicate, not something the Outliner papers over here -- a source that
   * lands its copy elsewhere is a source bug, and a move on top of it would turn one wrong
   * ordinal into two edits and two undo steps. */
  outliner_stack_row_activate(C, *space_outliner, new_ordinal);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_rename_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  if (name[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "A layer needs a name");
    return OPERATOR_CANCELLED;
  }
  return outliner_stack_row_rename(C, *space_outliner, ordinal, name) ? OPERATOR_FINISHED :
                                                                       OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_rename_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const StackRow *row = (ordinal < 0) ? nullptr :
                                        outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* Start from the current name: a rename dialog that opens empty is a delete-and-retype. */
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "name");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_string_set(op->ptr, prop, row->name.c_str());
  }
  return WM_operator_props_popup_confirm(C, op, nullptr);
}

wmOperatorStatus stack_row_mask_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const bool add = RNA_boolean_get(op->ptr, "add");
  /* The seam takes a fill color, so a third kind of mask is a new item on this menu, not a new
   * parameter carried through every layer between here and the generated image. */
  const bool black = RNA_enum_get(op->ptr, "initial_color") == 1;
  const float initial_color[4] = {black ? 0.0f : 1.0f,
                                  black ? 0.0f : 1.0f,
                                  black ? 0.0f : 1.0f,
                                  1.0f};
  /* A mask just added is the one the user means to work on, so its layer row reads as selected and
   * active like the brush target the editor switched to. Nothing renumbers, but the selection is
   * re-targeted all the same. */
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      add,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr ||
            !grouping->row_mask_set(*C, focus, owner, ordinal, add, initial_color))
        {
          return false;
        }
        if (add) {
          r_select_ordinal = ordinal;
        }
        return true;
      });
  if (ok && add) {
    /* The row now has a MASK section; show it, as a click on the mask thumbnail would. The mutation
     * rebuilt the rows, so the row is looked up again. */
    const StringRefNull mask_section = stack_mask_section_id(*space_outliner);
    if (const StackRow *rebuilt = outliner_stack_row_find(*space_outliner, ordinal)) {
      if (!mask_section.is_empty()) {
        outliner_stack_row_active_section_set(*space_outliner, *rebuilt, mask_section);
      }
    }
  }
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

bool stack_row_mask_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
}

wmOperatorStatus stack_row_mask_toggle_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
           int & /*r_select_ordinal*/) {
        const StackGroupingEditor *grouping = editor.grouping();
        return grouping != nullptr && grouping->row_mask_toggle(*C, focus, owner, ordinal);
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

bool stack_row_reorder_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->runtime != nullptr &&
         outliner_stack_can_reorder(*C, *space_outliner);
}

wmOperatorStatus stack_target_clear_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return stack_source_for_space(*space_outliner)->target_clear(*C) ? OPERATOR_FINISHED :
                                                                     OPERATOR_CANCELLED;
}

}  // namespace

const EnumPropertyItem *outliner_stack_focus_sub_index_itemf(bContext *C,
                                                              SpaceOutliner &space_outliner,
                                                              bool *r_free)
{
  return stack_sub_index_itemf_impl(C, space_outliner, r_free);
}

void outliner_stack_add_kinds_get(const SpaceOutliner &space_outliner,
                                  Vector<OutlinerStackAddKind> &r_kinds)
{
  const StackEditor *editor = stack_source_for_space(space_outliner)->editor();
  if (editor == nullptr) {
    return;
  }
  Vector<StackAddKindInfo> kinds;
  editor->add_kinds(kinds);
  for (const StackAddKindInfo &info : kinds) {
    OutlinerStackAddKind out;
    SNPRINTF(out.identifier, "%s", info.identifier.c_str());
    SNPRINTF(out.name, "%s", info.name.c_str());
    SNPRINTF(out.description, "%s", info.description.c_str());
    out.icon = info.icon;
    out.takes_color = info.takes_color;
    out.source_id_type = info.source_id_type;
    r_kinds.append(std::move(out));
  }
}

StackReadContext outliner_stack_read_context(const bContext &C)
{
  bContext &context = const_cast<bContext &>(C);
  StackReadContext ctx;
  ctx.bmain = CTX_data_main(&context);
  ctx.scene = CTX_data_scene(&context);
  ctx.view_layer = CTX_data_view_layer(&context);
  return ctx;
}

Object *outliner_stack_focus_object_get(const StackReadContext &ctx, const StackFocus &focus)
{
  if (focus.object_uid == 0) {
    /* Resolved at use rather than stored: the active object can be freed or replaced under a
     * stored pointer by an undo step, a file load or a delete. */
    if (ctx.bmain == nullptr || ctx.view_layer == nullptr) {
      return nullptr;
    }
    BKE_view_layer_synced_ensure(*ctx.bmain, ctx.scene, ctx.view_layer);
    return BKE_view_layer_active_object_get(ctx.view_layer);
  }
  if (ctx.bmain == nullptr) {
    return nullptr;
  }
  return id_cast<Object *>(BKE_libblock_find_session_uid(ctx.bmain, ID_OB, focus.object_uid));
}

Object *outliner_stack_focus_object_resolve(const StackReadContext &ctx,
                                            SpaceOutliner &space_outliner)
{
  Object *object = outliner_stack_focus_object_get(ctx, space_outliner.runtime->stack_focus);
  if (object == nullptr && space_outliner.runtime->stack_focus.object_uid != 0) {
    /* A focus that names a gone object pins nothing: drop it and the rows cached under it, and
     * fall back to the object the view layer has active now. */
    space_outliner.runtime->stack_focus = {};
    outliner_stack_rows_invalidate(space_outliner);
    object = outliner_stack_focus_object_get(ctx, space_outliner.runtime->stack_focus);
  }
  return object;
}

const char *outliner_stack_focus_name_get(const SpaceOutliner &space_outliner)
{
  return (space_outliner.runtime != nullptr) ?
             space_outliner.runtime->stack_focus_name.c_str() :
             "";
}

int outliner_stack_focus_sub_index_get(const SpaceOutliner &space_outliner)
{
  return (space_outliner.runtime != nullptr) ? space_outliner.runtime->stack_focus.sub_index : -1;
}

void outliner_stack_focus_sub_index_set(SpaceOutliner &space_outliner, const int sub_index)
{
  if (space_outliner.runtime != nullptr) {
    space_outliner.runtime->stack_focus.sub_index = sub_index;
  }
}

bool outliner_stack_focus_sub_index_apply(bContext &C, SpaceOutliner &space_outliner)
{
  if (space_outliner.runtime == nullptr) {
    return false;
  }
  Object *object = outliner_stack_focus_object_resolve(outliner_stack_read_context(C),
                                                        space_outliner);
  if (object == nullptr) {
    return false;
  }
  const int sub_index = outliner_stack_focus_sub_index_get(space_outliner);
  /* The sub-selection's own side effects in the data -- moving the material slot pointer along,
   * for one -- are the source's business; the generic path only passes through. */
  stack_source_for_space(space_outliner)->sub_selection_apply(
      C, space_outliner.runtime->stack_focus, *object);
  return outliner_stack_focus_set(&C, space_outliner, *object, sub_index, false);
}

void outliner_stack_rows_invalidate(SpaceOutliner &space_outliner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* The rows and the tree still agree on what each ordinal means; whatever the user did with
   * them is only about to be renumbered, so this is the last chance to keep it by identity. */
  stack_row_ui_state_capture(space_outliner);
  runtime.stack_rows.clear();
  runtime.stack_row_index.clear();
  runtime.stack_owner_uid = 0;
  runtime.stack_state_hash = 0;
  runtime.stack_rows_valid = false;
  /* The rows are about to go away, and the drop indicator names one of them.
   *
   * What a drop was aimed at is deliberately *not* cleared with them: it names its row by
   * identity, not by address, and is resolved against whatever rows exist when the drop lands.
   * Clearing it here loses the aim of a drop that is already under way -- importing a dragged
   * asset remaps data-blocks, which reaches this function through #outliner_id_remap, between the
   * poll that recorded the aim and the operator that reads it back. */
  runtime.stack_drop_indicator.ordinal = -1;
}

void outliner_stack_row_ui_state_capture_now(SpaceOutliner &space_outliner, const ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  if (space_outliner.treestore == nullptr) {
    return;
  }
  BLI_mempool_iter iter;
  BLI_mempool_iternew(space_outliner.treestore, &iter);
  while (const TreeStoreElem *tselem = static_cast<const TreeStoreElem *>(
             BLI_mempool_iterstep(&iter)))
  {
    if (tselem->type != TSE_STACK_LAYER || tselem->id != &owner) {
      continue;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, int(tselem->nr));
    if (row == nullptr || BLI_uuid_is_nil(row->stable_id)) {
      continue;
    }
    StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row->stable_id);
    state.closed = (tselem->flag & TSE_CLOSED) != 0;
    state.selected = (tselem->flag & TSE_SELECTED) != 0;
    state.active = (tselem->flag & TSE_ACTIVE) != 0;
  }
}

void outliner_stack_row_ui_state_sync(SpaceOutliner &space_outliner, const ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* The tree store speaks the ordinals of the tree it was filled by, so it can only be read
   * against the rows that tree was built from: the same owner, and not one an edit has re-read
   * since -- #outliner_stack_rows_ensure marks those, and the build that follows clears the
   * mark. */
  if (space_outliner.treestore == nullptr || runtime.stack_owner_uid != owner.session_uid ||
      runtime.stack_rows_rebuilt_since_build)
  {
    return;
  }
  outliner_stack_row_ui_state_capture_now(space_outliner, owner);
}

void outliner_stack_row_ui_state_prune(SpaceOutliner_Runtime &runtime, Set<UUID> &&seen)
{
  /* An entry unseen at this build and the previous one names a row the stack no longer has. */
  Vector<UUID> forgotten;
  for (const UUID &key : runtime.stack_row_ui_state.keys()) {
    if (!seen.contains(key) && !runtime.stack_row_ui_state_seen.contains(key)) {
      forgotten.append(key);
    }
  }
  for (const UUID &key : forgotten) {
    runtime.stack_row_ui_state.remove(key);
  }
  runtime.stack_row_ui_state_seen = std::move(seen);
}

StringRef outliner_stack_row_active_section_get(const SpaceOutliner &space_outliner,
                                                const StackRow &row)
{
  /* A row without sections has nothing to switch between; its content stays collapsed. */
  if (row.content_sections.is_empty()) {
    return {};
  }

  const SpaceOutliner_Runtime &runtime = *space_outliner.runtime;

  /* No stable_id means no persistent UI state: always use default section. */
  if (BLI_uuid_is_nil(row.stable_id)) {
    return row.content_sections[0].identifier;
  }

  const StackRowUiState *state = runtime.stack_row_ui_state.lookup_ptr(row.stable_id);
  if (state == nullptr || state->active_content_section.empty()) {
    /* No stored state: default to first section. */
    return row.content_sections[0].identifier;
  }

  /* Validate that the stored section still exists in the row's current data. */
  for (const StackContentSection &section : row.content_sections) {
    if (section.identifier == state->active_content_section) {
      return state->active_content_section;
    }
  }

  /* Stored section no longer exists (e.g., mask was deleted): fall back to first section. */
  return row.content_sections[0].identifier;
}

/** See #outliner_stack_row_resolve. */
const ed::outliner::StackRow *outliner_stack_row_resolve(const SpaceOutliner &space_outliner,
                                                         const bUUID &row_id,
                                                         const int ordinal_hint)
{
  if (!BLI_uuid_is_nil(row_id)) {
    for (const StackRow &row : space_outliner.runtime->stack_rows) {
      if (BLI_uuid_equal(row.stable_id, row_id)) {
        return &row;
      }
    }
  }
  return outliner_stack_row_find(space_outliner, ordinal_hint);
}

bool outliner_stack_row_active_section_set(SpaceOutliner &space_outliner,
                                           const StackRow &row,
                                           const StringRefNull section_id)
{
  /* Rows without content sections don't support section switching. */
  if (row.content_sections.is_empty()) {
    return false;
  }

  /* Validate that the section exists in the row. */
  bool section_exists = false;
  for (const StackContentSection &section : row.content_sections) {
    if (section.identifier == section_id) {
      section_exists = true;
      break;
    }
  }

  if (!section_exists) {
    return false;
  }

  /* No stable_id means no persistent UI state: cannot store selection. */
  if (BLI_uuid_is_nil(row.stable_id)) {
    return false;
  }

  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row.stable_id);
  state.active_content_section = section_id;

  return true;
}

bool stack_rows_are_siblings(const StackRow &a, const StackRow &b)
{
  return a.parent_ordinal == b.parent_ordinal && a.parent_section_id == b.parent_section_id;
}

Vector<StackAttachedPlacement> outliner_stack_attached_rows_plan(
    Span<StackRow> rows, FunctionRef<StringRef(const StackRow &)> active_section_get)
{
  Vector<StackAttachedPlacement> plan;
  /* Ordinal to index, plain rows only: an attached row hangs off the plain row its
   * #StackRow::parent_ordinal names, never off another attached row. */
  Map<int16_t, int64_t> plain_indices;
  for (const int64_t index : rows.index_range()) {
    if (rows[index].parent_section_id.empty()) {
      plain_indices.add(rows[index].ordinal, index);
    }
  }
  /* The rows run bottom to top, so walking them backward lists the plan top first -- the order a
   * stack is displayed in. */
  for (int64_t index = rows.size() - 1; index >= 0; index--) {
    const StackRow &row = rows[index];
    if (row.parent_section_id.empty() || !row.supported) {
      continue;
    }
    const int64_t *parent_index = plain_indices.lookup_ptr(row.parent_ordinal);
    if (parent_index == nullptr) {
      continue;
    }
    const StackRow &parent = rows[*parent_index];
    /* An unsupported parent has no sections of its own to list anything under. */
    if (!parent.supported) {
      continue;
    }
    if (active_section_get(parent) != StringRef(row.parent_section_id)) {
      continue;
    }
    plan.append({index, *parent_index});
  }
  return plan;
}

void outliner_stack_rows_ensure(const StackReadContext &ctx,
                                SpaceOutliner &space_outliner,
                                ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  const StackSource &source = *stack_source_for_space(space_outliner);
  const uint64_t state_hash = source.state_hash(ctx, owner);
  if (runtime.stack_rows_valid && runtime.stack_owner_uid == owner.session_uid &&
      runtime.stack_state_hash == state_hash)
  {
    return;
  }
  runtime.stack_rows.clear();
  source.rows_build(ctx, runtime.stack_focus, owner, runtime.stack_rows);
  /* Rows are addressed by ordinal all over the mode; one map spares every read a walk. */
  runtime.stack_row_index.clear();
  for (const int64_t index : runtime.stack_rows.index_range()) {
    runtime.stack_row_index.add(int(runtime.stack_rows[index].ordinal), int(index));
  }
  runtime.stack_owner_uid = owner.session_uid;
  runtime.stack_state_hash = state_hash;
  runtime.stack_rows_valid = true;
  /* Whatever tree the tree store was last filled by speaks the previous rows' ordinals. */
  runtime.stack_rows_rebuilt_since_build = true;
  /* Headers and scripts ask for the owner's name through RNA, where there is no context of their
   * own to resolve the focus with. */
  runtime.stack_focus_name = owner.name + 2;
}

const StackRow *outliner_stack_row_find(const SpaceOutliner &space_outliner, const int ordinal)
{
  const int *index = space_outliner.runtime->stack_row_index.lookup_ptr(ordinal);
  return (index != nullptr) ? &space_outliner.runtime->stack_rows[*index] : nullptr;
}

ID *outliner_stack_owner_get(const StackReadContext &ctx, SpaceOutliner &space_outliner)
{
  /* Resolving here, so that a focus left pointing at a deleted object drops itself, and the rows
   * cached under it with it, before anything gets to read them. */
  if (outliner_stack_focus_object_resolve(ctx, space_outliner) == nullptr) {
    return nullptr;
  }
  return stack_source_for_space(space_outliner)->owner_get(
      ctx, space_outliner.runtime->stack_focus);
}

int outliner_stack_identity_resolve(const StackReadContext &ctx,
                                    SpaceOutliner &space_outliner,
                                    const StackItemIdentity &identity)
{
  if (!identity.is_valid()) {
    return -1;
  }
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr || owner->session_uid != identity.owner_uid ||
      space_outliner.stack_source != identity.source_type)
  {
    return -1;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  if (!BLI_uuid_is_nil(identity.row_id)) {
    for (const StackRow &row : space_outliner.runtime->stack_rows) {
      if (BLI_uuid_equal(row.stable_id, identity.row_id)) {
        return row.ordinal;
      }
    }
    return -1;
  }
  if (identity.ordinal_hint >= 0 &&
      outliner_stack_row_find(space_outliner, identity.ordinal_hint) != nullptr)
  {
    return identity.ordinal_hint;
  }
  return -1;
}

StackItemIdentity outliner_stack_identity_of(const SpaceOutliner &space_outliner,
                                              const int ordinal)
{
  StackItemIdentity identity;
  const SpaceOutliner_Runtime *runtime = space_outliner.runtime;
  if (runtime == nullptr || runtime->stack_owner_uid == 0) {
    return identity;
  }
  identity.owner_uid = runtime->stack_owner_uid;
  identity.source_type = space_outliner.stack_source;
  identity.ordinal_hint = int16_t(ordinal);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row != nullptr) {
    identity.row_id = row->stable_id;
  }
  return identity;
}

/**
 * The ordinal the operator was given, or the row the user is pointing at when it was left at -1.
 *
 * A marker naming the row by identity comes first, when the operator has one. Then selection:
 * clicking a row is how a layer manager says "this one", and it is what the context menu and the
 * header buttons act on. The activated row -- the one the paint tools write into -- is the
 * fallback, since a stack can be worked on without anything being selected.
 */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op)
{
  const int marker_ordinal = stack_operator_marker_ordinal_get(C, space_outliner, op);
  if (marker_ordinal >= 0) {
    return marker_ordinal;
  }
  const int ordinal = RNA_int_get(op.ptr, "ordinal");
  if (ordinal >= 0) {
    return ordinal;
  }
  Vector<int> selected;
  stack_selected_ordinals_get(space_outliner, selected);
  if (!selected.is_empty()) {
    return selected.first();
  }
  return outliner_stack_active_ordinal_get(outliner_stack_read_context(C), space_outliner);
}

/** Ordinals of the stack rows the user has selected, bottom-up. Empty when none are. */
void stack_selected_ordinals_get(SpaceOutliner &space_outliner, Vector<int> &r_ordinals)
{
  r_ordinals.clear();
  tree_iterator::all(space_outliner, [&](const TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && (tselem->flag & TSE_SELECTED)) {
      r_ordinals.append_non_duplicates(int(tselem->nr));
    }
  });
  std::sort(r_ordinals.begin(), r_ordinals.end());
}

void stack_ordinals_drop_covered_descendants(const SpaceOutliner &space_outliner,
                                             Vector<int> &r_ordinals)
{
  /* A folder in the set already carries every row it holds; a descendant kept alongside it is a
   * second copy of a row that is going to move anyway -- and its group-child ordinal, which is not
   * a position, breaks any code that treats the set as a contiguous run. A row attached to a
   * parent's content section needs no separate case: its #StackRow::parent_ordinal is the row it
   * hangs under, so selecting that row drops it from the set with everything else the walk lifts
   * out. */
  Set<int> ordinal_set;
  ordinal_set.add_multiple(r_ordinals);
  Vector<int> roots;
  for (const int ordinal : r_ordinals) {
    const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
    bool has_selected_ancestor = false;
    while (row != nullptr && row->parent_ordinal >= 0) {
      if (ordinal_set.contains(row->parent_ordinal)) {
        has_selected_ancestor = true;
        break;
      }
      row = outliner_stack_row_find(space_outliner, row->parent_ordinal);
    }
    if (!has_selected_ancestor) {
      roots.append(ordinal);
    }
  }
  r_ordinals = std::move(roots);
}

bool outliner_stack_focus_set(bContext *C,
                              SpaceOutliner &space_outliner,
                              Object &object,
                              const int sub_index,
                              const bool enter_edit_mode)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Base *base = BKE_view_layer_base_find(view_layer, &object);
  if (base == nullptr) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Object is not in the active view layer");
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (sub_index < -1) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Stack index is out of range");
    return false;
  }
  /* What a sub-index selects among is the source's own business; an empty list means the source
   * has no notion of one at all, and only the default (-1) is valid then. */
  StackFocus probe_focus;
  probe_focus.object_uid = object.id.session_uid;
  Vector<StackSubSelection> sub_selections;
  source.sub_selections_get(ctx, probe_focus, sub_selections);
  /* The object list opens a paint stack at its active material. Store that slot explicitly so the
   * header enum has a concrete current item and can cycle from it with Ctrl+Wheel. */
  const int resolved_sub_index = (sub_index < 0 && !sub_selections.is_empty()) ?
                                     int(object.actcol) - 1 :
                                     sub_index;
  if (resolved_sub_index >= 0) {
    if (resolved_sub_index >= sub_selections.size()) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, "Stack index is out of range");
      return false;
    }
  }

  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  runtime.stack_focus.object_uid = object.id.session_uid;
  runtime.stack_focus.sub_index = resolved_sub_index;
  outliner_stack_rows_invalidate(space_outliner);
  space_outliner.outlinevis = SO_STACK_LAYERS;
  space_outliner.stack_layers_view = SO_SL_VIEW_STACK;
  ScrArea *area = CTX_wm_area(C);
  if (area != nullptr) {
    for (ARegion &region : area->regionbase) {
      if (region.regiontype != RGN_TYPE_TOOL_HEADER) {
        continue;
      }
      const bool hidden = region.flag & RGN_FLAG_HIDDEN_BY_USER;
      if (bool(region.flag & RGN_FLAG_HIDDEN) != hidden) {
        SET_FLAG_FROM_TEST(region.flag, hidden, RGN_FLAG_HIDDEN);
        ED_area_tag_region_size_update(area, &region);
      }
      ED_region_tag_redraw(&region);
    }
    ED_area_tag_redraw(area);
  }

  /* A stack whose model partly lives in its own data needs a one-time write before its rows can
   * be read -- identities handed out, a legacy shape brought into line. That is an explicit,
   * undoable step tied to opening the stack here, not a side effect of reading it every redraw
   * -- see #StackSource::focus_will_open. */
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner != nullptr && source.focus_will_open(*C, *owner)) {
    ED_undo_push(C, "Open Layer Stack");
  }

  /* A source with no object mode of its own -- editing a shape key is not a mode -- leaves
   * nothing to enter, and asking for one anyway is not an error. */
  const std::optional<eObjectMode> mode = enter_edit_mode ? source.focus_object_mode() :
                                                            std::nullopt;
  if (mode.has_value()) {
    BKE_view_layer_base_select_and_set_active(view_layer, base);
    if (!object::mode_set(C, *mode)) {
      BKE_report(CTX_wm_reports(C), RPT_WARNING, "Could not enter the stack's edit mode");
    }
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  ED_area_tag_refresh(CTX_wm_area(C));
  return true;
}

bool outliner_stack_row_activate(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  const bool result = stack_source_for_space(space_outliner)
                          ->row_activate(*C, space_outliner.runtime->stack_focus, *owner, ordinal, *row);
  return result;
}

bool outliner_stack_row_preview_activate(bContext *C,
                                         SpaceOutliner &space_outliner,
                                         const int ordinal,
                                         const StringRef section_id)
{
  if (ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  const bool result = stack_source_for_space(space_outliner)
                          ->preview_activate(*C, *owner, *row, section_id);
  return result;
}

bool outliner_stack_sub_row_activate(bContext *C, SpaceOutliner &space_outliner, const int nr)
{
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, nr / STACK_ROW_SUB_ROW_STRIDE);
  if (row == nullptr) {
    return false;
  }
  const int role = nr % STACK_ROW_SUB_ROW_STRIDE;
  Span<StackSubRow> sub_rows;
  const StringRef active_section = outliner_stack_row_active_section_get(space_outliner, *row);
  for (const StackContentSection &section : row->content_sections) {
    if (section.identifier == active_section) {
      sub_rows = section.sub_rows;
      break;
    }
  }
  for (const StackSubRow &sub_row : sub_rows) {
    if (sub_row.role == role) {
      return stack_source_for_space(space_outliner)
          ->sub_row_activate(*C, space_outliner.runtime->stack_focus, *owner, *row, sub_row);
    }
  }
  return false;
}

int outliner_stack_active_ordinal_get(const StackReadContext &ctx,
                                      SpaceOutliner &space_outliner)
{
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return -1;
  }
  /* The rows may never have been built in this space: a header button is reachable before the
   * tree has been drawn once. */
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackSource &source = *stack_source_for_space(space_outliner);
  for (const StackRow &row : space_outliner.runtime->stack_rows) {
    if (source.row_is_active(ctx, space_outliner.runtime->stack_focus, *owner, row)) {
      return row.ordinal;
    }
  }
  return -1;
}

bool outliner_stack_row_is_active(const StackReadContext &ctx,
                                  SpaceOutliner &space_outliner,
                                  const int ordinal)
{
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (row == nullptr || owner == nullptr) {
    return false;
  }
  return stack_source_for_space(space_outliner)
      ->row_is_active(ctx, space_outliner.runtime->stack_focus, *owner, *row);
}

bool outliner_stack_can_reorder(const bContext &C, SpaceOutliner &space_outliner)
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  const ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  const StackEditor *editor = stack_source_for_space(space_outliner)->editor();
  return owner != nullptr && editor != nullptr && editor->can_reorder(*owner);
}

bool outliner_stack_row_reorder(bContext *C,
                                SpaceOutliner &space_outliner,
                                const int from_ordinal,
                                const int to_ordinal)
{
  if (from_ordinal == to_ordinal || from_ordinal < 0 || to_ordinal < 0) {
    return false;
  }
  /* Ordinals are positions, so every row past the move has a new one. The moved row keeps the
   * selection -- it is the one the user just acted on -- and the groups they had open have to be
   * asked for by identity rather than by a number that just changed under them. */
  return stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        if (!editor.can_reorder(owner)) {
          return false;
        }
        r_select_ordinal = to_ordinal;
        return editor.row_reorder(*C, focus, owner, from_ordinal, to_ordinal);
      });
}

bool outliner_stack_row_move(bContext *C,
                             SpaceOutliner &space_outliner,
                             const int from_ordinal,
                             const int anchor_ordinal,
                             const StackMovePlace place,
                             int *r_ordinal)
{
  if (from_ordinal < 0 || anchor_ordinal < 0) {
    return false;
  }
  /* Ordinals are positions, so every row past the move has a new one. The moved row itself is the
   * one the user was just working with, so it stays the one selected and active rather than the
   * drop reading as if it landed on nothing. */
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        if (!editor.can_reorder(owner)) {
          return false;
        }
        return editor.row_move(
            *C, focus, owner, from_ordinal, anchor_ordinal, place, &r_select_ordinal);
      },
      &new_ordinal);
  if (!ok) {
    return false;
  }
  if (r_ordinal != nullptr) {
    *r_ordinal = new_ordinal;
  }
  if (new_ordinal >= 0) {
    outliner_stack_row_activate(C, space_outliner, new_ordinal);
  }
  return true;
}

int outliner_stack_row_add(bContext *C,
                           SpaceOutliner &space_outliner,
                           const int kind,
                           const int ordinal,
                           const StackAddArgs &args)
{
  /* Inserting a row renumbers everything above it. */
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        r_select_ordinal = editor.row_add(*C, focus, owner, kind, ordinal, args);
        return r_select_ordinal >= 0;
      },
      &new_ordinal);
  if (!ok) {
    return -1;
  }
  /* A row attached to a section of its parent (a mask correction under MASK) is listed only while
   * that section is active, so switch the parent to it or the new row would stay hidden. */
  if (const StackRow *added = outliner_stack_row_find(space_outliner, new_ordinal)) {
    if (!added->parent_section_id.empty() && added->parent_ordinal >= 0) {
      if (const StackRow *parent = outliner_stack_row_find(space_outliner, added->parent_ordinal)) {
        outliner_stack_row_active_section_set(space_outliner, *parent, added->parent_section_id);
      }
    }
  }
  /* A layer the user just asked for is the one they mean to work on next. */
  outliner_stack_row_activate(C, space_outliner, new_ordinal);
  return new_ordinal;
}

bool outliner_stack_row_fill_color_set(bContext *C,
                                       SpaceOutliner &space_outliner,
                                       const int ordinal,
                                       const float color[4],
                                       StackColorSession *session)
{
  /* A re-fill changes pixels and a marker, not the row order, so needs_renumber = false. */
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        const StackColorEditor *color_editor = editor.color();
        if (color_editor == nullptr) {
          return false;
        }
        return color_editor->row_fill_color_set(*C, focus, owner, ordinal, color, session);
      });
}

bool outliner_stack_row_fill_color_preview(bContext *C,
                                           SpaceOutliner &space_outliner,
                                           const int ordinal,
                                           const float color[4],
                                           StackColorSession *session)
{
  /* A preview tick touches pixels only: no row is renumbered, so -- unlike the bake above --
   * the cached rows stay standing and no selection is re-targeted. No undo step either: the
   * caller is the picker's RNA update, and the commit (a memfile step plus the source's own undo
   * entry) belongs to the dialog's exec. */
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  const StackEditor *editor = source.editor();
  if (editor == nullptr || !source.is_editable(*owner)) {
    return false;
  }
  const StackColorEditor *color_editor = editor->color();
  if (color_editor == nullptr) {
    return false;
  }
  return color_editor->row_fill_color_preview(
      *C, space_outliner.runtime->stack_focus, *owner, ordinal, color, session);
}

bool outliner_stack_row_color_tag_set(bContext *C,
                                      SpaceOutliner &space_outliner,
                                      const int ordinal,
                                      const int color_tag)
{
  /* Color tag is not a stack mutation (it does not renumber), so needs_renumber = false. */
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr) {
          return false;
        }
        return grouping->row_color_tag_set(*C, focus, owner, ordinal, color_tag);
      });
}

bool outliner_stack_row_set_enabled(bContext *C,
                                    SpaceOutliner &space_outliner,
                                    const int ordinal,
                                    const bool enable)
{
  if (ordinal < 0) {
    return false;
  }
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        return editor.row_set_enabled(*C, focus, owner, ordinal, enable);
      });
}

bool outliner_stack_row_remove(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  /* Taking a row out renumbers everything that was above it, and the row that inherits the removed
   * one's ordinal must not inherit its selection with it. */
  return stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        return editor.row_remove(*C, focus, owner, ordinal);
      });
}

char *outliner_stack_row_name_buffer(const SpaceOutliner &space_outliner, const int ordinal)
{
  const StackRow *row = (ordinal < 0) ? nullptr : outliner_stack_row_find(space_outliner, ordinal);
  return (row != nullptr) ? row->name_buffer : nullptr;
}

bool outliner_stack_row_rename(bContext *C,
                               SpaceOutliner &space_outliner,
                               const int ordinal,
                               const StringRefNull name)
{
  if (ordinal < 0 || name.is_empty()) {
    return false;
  }
  /* A rename leaves every ordinal where it was, so the rows come back under the same tree store
   * keys and there is no pending state to carry across. */
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        return editor.row_rename(*C, focus, owner, ordinal, name);
      });
}

void outliner_stack_sources_undo_reset()
{
  for (const StackSource *source : stack_sources_get()) {
    source->undo_reset();
  }
}

}  // namespace blender::ed::outliner
