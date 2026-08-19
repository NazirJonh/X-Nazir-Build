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

#include "BLI_math_vector_types.hh"

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
 * to afterwards. The tool-system unlinks this specific tool on tool switch; the tool check remains
 * as a safeguard against a missed cleanup path silently moving the wrong thing.
 */
bool placement_active(bContext *C, const Object &ob);

/** The temp cage object of the Lattice Tool session on \a ob, or null when there is none. */
Object *cage_object(const Object &ob);

/**
 * Snapshots the live cage loc/quat/scale so a later confirmed G/R/S can restore them.
 * Does not open an undo step: Esc must not leave a dummy item on the stack.
 */
void placement_transform_undo_store(Object &ob);

/**
 * Commits a sculpt-typed undo step that stores the snapshot from
 * #placement_transform_undo_store, not mesh positions. Call only when the transform is
 * confirmed. Required so #OPTYPE_UNDO does not fall through to memfile.
 */
void placement_transform_undo_commit(const Scene &scene, Object &ob, const char *name);

/** Drops a snapshot taken by #placement_transform_undo_store. No-op if none is stored. */
void placement_transform_undo_abort(Object &ob);

/**
 * Swaps \a loc / \a quat / \a scale with the live cage transform on \a ob, then refreshes
 * the cage runtime matrix. If necessary, recreates the editor-private cage from the stored session
 * settings while the Lattice Tool is active. No-op when recreation is unavailable.
 */
void undo_restore_cage(bContext *C,
                       Object &ob,
                       float loc[3],
                       float quat[4],
                       float scale[3],
                       const int3 &resolution,
                       int interpolation,
                       float margin,
                       float mask_eps);

/** Remove placement-only lattice undo steps that cannot be restored after a tool switch. */
void undo_purge_cage_steps(const Object &ob);

}  // namespace blender::ed::sculpt_paint::lattice
