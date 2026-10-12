/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Access the Stack Layers display mode from outside its module, for RNA.
 *
 * Kept apart from #ED_outliner.hh so the module's main public header stays about the editor API:
 * this is the narrow door the Outliner's own RNA properties walk through, nothing more. Everything
 * here is the surface the space's RNA properties need: the focus name and the tool header sync.
 */

#pragma once

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

}  // namespace blender::ed::outliner
