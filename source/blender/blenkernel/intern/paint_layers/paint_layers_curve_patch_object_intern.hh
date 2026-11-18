/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Lifetime hooks of the Curve Patch object-source runtime
 * (`paint_layers_curve_patch_object.cc`). The state itself is private to that file; only the two
 * calls the bake runtime's own lifetime paths need are visible here.
 *
 * Integration points of a generated source (the next one must cover all of them):
 * - free (`curve_patch_object_runtime_free`) and owner transfer
 *   (`curve_patch_object_runtime_owner_transfer`) from the bake runtime's lifetime paths;
 * - `BKE_paint_layers_curve_patch_object_invalidate` on copy, import and undo;
 * - depsgraph nodes and relation edges (`deg_builder_nodes.cc`, `deg_builder_relations.cc`);
 * - a refresh call after every scene update (`scene.cc`).
 */

#pragma once

namespace blender {

struct Material;

/** Drop the entry of \a ma when it owns it (the material is being freed). */
void curve_patch_object_runtime_free(Material &ma);
/** Move the owned entry to \a dst (the material was replaced by a new ID instance). */
void curve_patch_object_runtime_owner_transfer(Material &dst, Material &src);

}  // namespace blender
