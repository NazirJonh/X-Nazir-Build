/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Minimal public surface of the Sculpt Lattice tool for modules that must react to it but cannot
 * see its state type. #LatticeToolData stays private to the sculpt_paint module (ADR-13), and
 * mesh/sculpt_lattice.hh is not on the include path of editors/transform.
 */

#pragma once

namespace blender {
struct bContext;
struct Object;
struct Scene;
}  // namespace blender

namespace blender::ed::sculpt_paint::lattice {

/**
 * True when the cage — not the mesh — is what viewport transform should act on: the Lattice Tool
 * is the active tool AND \a ob has a live session sitting in its cage placement phase.
 *
 * Both halves are load-bearing. A session outlives the operators that created it, so "a session
 * exists" alone would keep retargeting G/R/S onto an invisible cage for any tool the user switched
 * to afterwards. #toolsystem_unlink_ref drops the session on tool switch, which should make the
 * tool check redundant; it is kept because the cost of a missed cleanup path is a transform that
 * silently moves the wrong thing.
 */
bool placement_active(bContext *C, const Object &ob);

/** The temp cage object of the Lattice Tool session on \a ob, or null when there is none. */
Object *cage_object(const Object &ob);

/**
 * Forwarders to the sculpt undo system (mesh/sculpt_undo.hh), which sits outside the include path
 * of editors/transform. Opening a sculpt-typed undo step is mandatory around the cage transform:
 * see section 8 of the design doc.
 */
void undo_push_begin(const Scene &scene, Object &ob, const char *name);
void undo_push_end(Object &ob);

}  // namespace blender::ed::sculpt_paint::lattice
