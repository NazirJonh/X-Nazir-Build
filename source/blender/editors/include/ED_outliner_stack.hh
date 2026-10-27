/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Access the Stack Layers display mode from outside its module: for RNA, and for the editors that
 * have to follow the stack the user is working on.
 *
 * Kept apart from #ED_outliner.hh so the module's main public header stays about the editor API:
 * this is the narrow door the Outliner's own RNA properties walk through, nothing more.
 */

#pragma once

#include "BLI_uuid.h"

struct bContext;
struct EnumPropertyItem;
struct ID;
struct ScrArea;
struct SpaceOutliner;

namespace blender::ed::outliner {

/**
 * Show the tool header only in Stack Layers.
 *
 * The tool header carries the Stack Layers controls and nothing else, so an empty strip under the
 * header in every other display mode is wasted space the user did not ask for. Every path that
 * changes the display mode outside the RNA property's own update -- an operator that sets the mode
 * directly, a mode restored from a file, a duplicated area -- goes through this, so they all agree
 * with a mode set from the UI. The re-layout it triggers is deferred to the next window draw (see
 * the definition), which is why it needs no `bContext`.
 */
void outliner_tool_header_visibility_sync(bContext * /*C*/,
                                          ScrArea *area,
                                          const SpaceOutliner &space_outliner);

/**
 * The name of the data-block the displayed Stack Layers stack belongs to, as the source last
 * resolved it.
 *
 * Kept on the space's runtime rather than resolved on the fly, because an RNA getter has no
 * context to resolve a focus with. Empty until the stack has been drawn once.
 */
const char *outliner_stack_focus_name_get(const SpaceOutliner &space_outliner);

/** Index of the focused stack's source-defined sub-selection, such as a material slot. */
int outliner_stack_focus_sub_index_get(const SpaceOutliner &space_outliner);
/**
 * Select one of the focused object's source-defined sub-selections and make it the active stack.
 *
 * Used by the Stack Layers header's RNA enum so its standard Ctrl-Wheel cycling follows the same
 * path as an explicit stack selection.
 */
void outliner_stack_focus_sub_index_set(SpaceOutliner &space_outliner, int sub_index);
bool outliner_stack_focus_sub_index_apply(bContext &C, SpaceOutliner &space_outliner);
/** Source-defined choices for #outliner_stack_focus_sub_index_get. */
const EnumPropertyItem *outliner_stack_focus_sub_index_itemf(bContext *C,
                                                             SpaceOutliner &space_outliner,
                                                             bool *r_free);

/**
 * Follow the active object with the display again, and put it on the object's stack now.
 *
 * Answers a display the user left by hand and the space's own switch, so the option that turns the
 * follow on reads as what the Outliner does rather than as a promise for the next object change.
 * Returns true when the display mode changed.
 */
bool outliner_stack_view_follow_resume(bContext &C, SpaceOutliner &space_outliner);
/**
 * The display mode was chosen by hand, so the follow no longer owns it: a View Layer it opened is
 * the user's from now on, and it will not bring the Stack Layers mode back on the next change.
 */
void outliner_stack_follow_release_mode(SpaceOutliner &space_outliner);
/** As the display's own normalization of the focus, for the RNA update of Pin Stack. */
void outliner_stack_focus_normalize(bContext &C, SpaceOutliner &space_outliner);

/**
 * Tell the Stack Layers displays that \a owner gained the row identified by \a row_id outside their
 * own operators -- the Add popover's Material layer is created by an RNA property update, not by
 * the stack's Add operator.
 *
 * Every Outliner currently showing that owner re-reads its rows and reads the named one as the
 * active, selected row, which is what #outliner_stack_row_add leaves behind for a row it made.
 */
void outliner_stack_row_added_sync(bContext &C, ID &owner, const bUUID &row_id);

}  // namespace blender::ed::outliner
