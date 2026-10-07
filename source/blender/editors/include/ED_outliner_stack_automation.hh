/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Access the Stack Layers display mode from outside its module, for RNA.
 *
 * Kept apart from #ED_outliner.hh so the module's main public header stays about the editor API:
 * this is the narrow door the Outliner's own RNA properties walk through, nothing more. Everything
 * here is the surface the space's RNA properties need: the focus name, the sub-selection, the
 * source's add kinds, and the tool header sync.
 */

#pragma once

#include <cstdint>

#include "BLI_vector.hh"

struct bContext;
struct EnumPropertyItem;
struct ScrArea;
struct SpaceOutliner;

/**
 * One kind of row the displayed stack's source can add, as its #StackEditor declares it.
 *
 * A plain, copyable description so RNA can hand it to a script: fixed-size strings, no ownership.
 * The global name matches what RNA's sdna string expects of a struct it iterates an array of.
 */
struct OutlinerStackAddKind {
  char identifier[64];
  char name[128];
  char description[256];
  int icon;
  bool takes_color;
  int source_id_type;
};

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

/**
 * The kinds of rows the displayed stack's source can add, in the order its Add reads them back.
 *
 * Empty when the space shows no stack or its source has no editor. The Add UI draws from this
 * rather than naming kinds of its own, so a source that declares none grows no Add at all and one
 * with paint kinds grows no second UI to keep in step.
 */
void outliner_stack_add_kinds_get(const SpaceOutliner &space_outliner,
                                  blender::Vector<OutlinerStackAddKind> &r_kinds);

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

}  // namespace blender::ed::outliner
