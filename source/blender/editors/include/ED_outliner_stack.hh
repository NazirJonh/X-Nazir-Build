/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Access the Stack Layers display mode from outside its module, for RNA.
 *
 * Kept apart from #ED_outliner.hh so the module's main public header stays about the editor API:
 * this is the narrow door the Outliner's own RNA properties walk through, nothing more.
 */

#pragma once

struct bContext;
struct EnumPropertyItem;
struct SpaceOutliner;

namespace blender::ed::outliner {

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

}  // namespace blender::ed::outliner
