/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Access the Stack Layers display mode from outside its module, for RNA and for the automated test
 * suite.
 *
 * Kept apart from #ED_outliner.hh so the module's main public header stays about the editor API:
 * this is the narrow door the Outliner's own RNA properties and the tests walk through, nothing
 * more. Everything here is automation surface: it exists so a script can ask and act where a
 * mouse click would sit, and none of it is a stable scripting API.
 */

#pragma once

#include <cstdint>

struct bContext;
struct EnumPropertyItem;
struct ID;
struct ScrArea;
struct SpaceOutliner;

namespace blender::ed::outliner {

/**
 * Show the tool header only in Stack Layers.
 *
 * It carries nothing else, and an empty strip under the header in every other display mode is
 * wasted space the user did not ask for. Every path that changes the display mode outside the
 * RNA property's own update -- an operator that sets the mode directly, a mode restored from a
 * file, an undo step, a duplicated area -- goes through this, so they all agree with a mode set
 * from the UI.
 */
void outliner_tool_header_visibility_sync(ScrArea *area, const SpaceOutliner &space_outliner);

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
 * This is used by the Stack Layers header's RNA enum so its standard Ctrl-Wheel cycling follows
 * the same path as an explicit stack selection.
 */
void outliner_stack_focus_sub_index_set(SpaceOutliner &space_outliner, int sub_index);
bool outliner_stack_focus_sub_index_apply(bContext &C, SpaceOutliner &space_outliner);
/** Source-defined choices for #outliner_stack_focus_sub_index_get. */
const EnumPropertyItem *outliner_stack_focus_sub_index_itemf(bContext *C,
                                                             SpaceOutliner &space_outliner,
                                                             bool *r_free);

/**
 * Whether the stack row at \a ordinal is currently selected in the tree, or false when it is not
 * in the tree at all.
 *
 * For the automated test suite: nothing in the display mode itself needs a row's selection by
 * ordinal, since selection is tree-store state the tree already carries on its own.
 */
bool outliner_stack_row_is_selected(const SpaceOutliner &space_outliner, int ordinal);
/**
 * Whether the stack row at \a ordinal is open (not collapsed) in the tree, or false when it is not
 * in the tree at all.
 *
 * For the automated test suite; see #outliner_stack_row_is_selected.
 */
bool outliner_stack_row_is_open(const SpaceOutliner &space_outliner, int ordinal);
/**
 * Select or deselect the stack row at \a ordinal, the way clicking it would: the other rows'
 * selection is cleared, and nothing the row's data cares about happens (no activation). With \a
 * extend the click is the ctrl-clicked kind -- the row joins the selection instead of replacing
 * it, which is what a multi-row operation needs its rows picked by.
 *
 * For the automated test suite; see #outliner_stack_row_is_selected.
 */
void outliner_stack_row_select(bContext &C,
                               SpaceOutliner &space_outliner,
                               int ordinal,
                               bool select,
                               bool extend);
/**
 * Collapse or open the stack row at \a ordinal, the way its disclosure toggle would.
 *
 * For the automated test suite; see #outliner_stack_row_is_selected.
 */
void outliner_stack_row_closed_set(SpaceOutliner &space_outliner, int ordinal, bool closed);
/**
 * The session UID of the data-block whose preview the stack row at \a ordinal shows, or 0 when
 * the row has no preview.
 *
 * For the automated test suite; see #outliner_stack_row_is_selected.
 */
uint32_t outliner_stack_row_preview_uid(const SpaceOutliner &space_outliner, int ordinal);

/**
 * Move the row at \a source_ordinal in \a source_space's stack next to the row at
 * \a target_ordinal in \a space_outliner's, exactly as dropping the first onto the second would.
 *
 * For the automated test suite: a real drop needs a #wmDrag and a mouse event over a second,
 * genuinely drawn Outliner region, neither of which a script can produce on demand. This goes
 * through the same #outliner_stack_identity_resolve guard #OUTLINER_OT_stack_layer_drop does, so
 * it fails the same way a drop between two different stacks does.
 *
 * \return false when \a source_space and \a space_outliner do not show the same stack -- a
 * different owner or a different source -- or either row is gone.
 */
bool outliner_stack_layer_debug_drop(bContext &C,
                                     const SpaceOutliner &source_space,
                                     int source_ordinal,
                                     SpaceOutliner &space_outliner,
                                     int target_ordinal);

/**
 * Drop a data-block on \a space_outliner's stack -- on the row at \a target_ordinal, or on the
 * stack itself for -1 -- exactly as dragging it there would.
 *
 * For the automated test suite, mirroring #outliner_stack_layer_debug_drop: a real drop needs a
 * #wmDrag and a mouse event over a genuinely drawn Outliner region, neither of which a script can
 * produce on demand. The source's drop handler judges and executes the payload the same way the
 * stack drop operators hand it one.
 *
 * \return false when this Outliner shows no stack, its source has no drop handler, or the handler
 * refuses the payload.
 */
bool outliner_stack_layer_debug_drop_id(bContext &C,
                                        SpaceOutliner &space_outliner,
                                        const ID &dropped,
                                        int target_ordinal);

/**
 * The session UID of the data-block the drag started on the stack sub-row at \a ordinal and \a
 * role would carry, or 0 when there is no such row in the tree or it has nothing to drag.
 *
 * For the automated test suite, mirroring #outliner_stack_layer_debug_drop_id: a real drag needs
 * a mouse event over a genuinely drawn Outliner region, which a script cannot produce on demand.
 * This runs the same carrier logic the drag operator does -- the sub-row's data-block out of
 * #TreeElement.directdata -- and builds the same #wmDrag to report what it carries.
 */
uint32_t outliner_stack_item_debug_drag_id(SpaceOutliner &space_outliner, int ordinal, int role);

}  // namespace blender::ed::outliner
