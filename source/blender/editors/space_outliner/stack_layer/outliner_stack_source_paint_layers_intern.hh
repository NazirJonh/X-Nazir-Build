/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup spoutliner
 *
 * Internal seams of the paint-layers stack source, shared between its implementation and its unit
 * test. The paint source reads and writes `Material::paint_layers` directly through the
 * `BKE_paint_layers_*` API by marker, so these helpers name the concrete material type the source
 * is built on -- exactly what the generic Outliner code must not see. Declaring them once here lets
 * the test drive them without an Outliner (or a #bContext at all).
 */

#include "BLI_vector.hh"

#include "outliner_stack_source.hh"

namespace blender {

struct Material;
struct MaterialPaintLayer;

namespace ed::outliner {

/**
 * Rows for a layered material, built from its DNA description rather than a graph: one flat Paint
 * row per entry of `Material::paint_layers`, value and mode columns addressed through the row.
 *
 * \param channel: the #eMaterialPaintChannel the value columns address. Until the per (row,
 *                 channel) settings exist it only affects which edits rebuild the rows.
 */
void paint_stack_rows_from_description(const Material &material,
                                       int channel,
                                       Vector<StackRow> &r_rows);

/** The description row a flat-walk ordinal names, or null. */
MaterialPaintLayer *paint_description_row_for_ordinal(Material &material, int ordinal);

/** The ordinal #paint_stack_rows_from_description gives \a target, or -1. */
int paint_layers_ordinal_of(const Material &material, const MaterialPaintLayer *target);

}  // namespace ed::outliner
}  // namespace blender
