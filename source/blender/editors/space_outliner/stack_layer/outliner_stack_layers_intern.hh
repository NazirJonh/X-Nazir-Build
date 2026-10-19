/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Helpers the Stack Layers operator files of this directory share: the mode poll, the marker
 * resolver, and the wrapper every edit goes through.
 */

#pragma once

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/** The marker resolved to no row; see #stack_operator_marker_ordinal_get. */
constexpr int STACK_MARKER_UNRESOLVED = -2;

bool stack_mode_poll(bContext *C);
int stack_operator_marker_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op);
void stack_row_ui_state_edit_select(const StackReadContext &ctx,
                                    SpaceOutliner &space_outliner,
                                    ID &owner,
                                    int select_ordinal);
const char *stack_enum_string_intern(StringRefNull text);

/**
 * What an edit that changed which row sits at which ordinal owes the mode: the cached rows go so
 * the next read sees the new ones, the UI state is re-targeted at \a select_ordinal (or cleared
 * for -1), and the Outliner is told to rebuild.
 *
 * \param needs_renumber: false for an edit that changed no position, such as a rename or a
 * visibility toggle; the selection then stays where it was.
 *
 * Shared by #stack_mutate and by an edit that adds or removes rows through the source itself, such
 * as a dropped image becoming a layer, so both leave the mode in the same state.
 */
void stack_row_edit_finished(bContext &C,
                             SpaceOutliner &space_outliner,
                             ID &owner,
                             int select_ordinal,
                             bool needs_renumber);

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
  stack_row_edit_finished(C, space_outliner, *owner, select_ordinal, needs_renumber);
  return true;
}

}  // namespace blender::ed::outliner
