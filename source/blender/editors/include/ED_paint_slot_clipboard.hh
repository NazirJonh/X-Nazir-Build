/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * The session clipboard of paint slot assignments ("Copy Image" / "Copy Material" in a slot's
 * context menu, and the Outliner's own copy verbs): it carries the assigned data-block from one
 * slot to another, across editors, for the rest of the session.
 */

#pragma once

namespace blender {

struct bContext;
struct ID;
struct ReportList;

/**
 * Remember \a id as the last copied paint-slot assignment: its kind and session UID, on the window
 * manager's runtime. The clipboard is deliberately session-only and shared across editors, so a
 * paste lands in any slot of the same kind, whatever panel or editor draws it.
 */
void ED_paint_slot_clipboard_store(const bContext *C, const ID &id);

/**
 * Store \a id as the copied assignment and report it as such: "Copied image/material <name>".
 * Shared with the Outliner's own copy verbs, so the panel and the rows read alike.
 */
void ED_paint_slot_clipboard_copy_report(const bContext *C, const ID &id, ReportList *reports);

/**
 * The copied data-block, resolved against the context's Main by its session UID, which survives
 * undo. Null when nothing was copied, \a expected_idcode is not the copied kind, or the copied
 * data-block was deleted since.
 */
ID *ED_paint_slot_clipboard_resolve(const bContext *C, short expected_idcode);

}  // namespace blender
