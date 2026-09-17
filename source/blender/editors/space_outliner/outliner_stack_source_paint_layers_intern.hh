/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup spoutliner
 *
 * Internal seams of the paint-layers stack source, shared between the implementation and its unit
 * test. The old graph-truth source these were once shared with is gone; everything here is built on
 * the DNA description (`Material.paint_layers`).
 *
 * Nothing here is part of the #StackSource contract: these functions name the concrete material
 * types the source is built on, which is exactly what the generic Outliner code must not see. They
 * are declared once here so the test does not carry its own copy of a signature, which would
 * silently drift from the definition.
 */

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "outliner_stack_source.hh"

namespace blender {

struct Main;
struct Material;
struct MaterialPaintLayer;

namespace ed::outliner {

/**
 * The paint source's Add vocabulary: indices into #StackEditor::add_kinds, read back by
 * #StackEditor::row_add. Paint and Fill corrections are separate kinds rather than one kind plus an
 * effect argument, so the seam carries no paint enum (S-1).
 */
enum PaintStackAddKind : int {
  PAINT_STACK_ADD_PAINT = 0,
  PAINT_STACK_ADD_FILL,
  PAINT_STACK_ADD_MATERIAL,
  PAINT_STACK_ADD_FOLDER,
  PAINT_STACK_ADD_CORRECTION_PAINT,
  PAINT_STACK_ADD_CORRECTION_FILL,
  PAINT_STACK_ADD_MASK_CORRECTION_PAINT,
  PAINT_STACK_ADD_MASK_CORRECTION_FILL,
};

/**
 * Rows for a layered material, built from its DNA description rather than a graph. Folders nest
 * through their children, corrections hang off their owner's section, and a folder is the row that
 * can hold children.
 *
 * \param channel: the #eMaterialPaintChannel the value/mode columns address; a row whose pair has a
 *                 record for it points its columns at that record, the rest inherit the row.
 */
void paint_stack_rows_from_description(const Material &material,
                                       int channel,
                                       Vector<StackRow> &r_rows);

/** The description row (a layer or correction) a flat-walk ordinal names, or null. */
MaterialPaintLayer *paint_description_row_for_ordinal(Material &material, int ordinal);

/**
 * The context-free description edits behind #PaintLayersStackSource's verbs: they touch only
 * `Material::paint_layers` through the `BKE_paint_layers_*` API, so a unit test can drive them
 * without an Outliner (or any #bContext at all). Each returns the affected row's ordinal (>=0), or
 * -1 on refusal; the booleans mean what they say. The source's verbs wrap these with reports and
 * notifiers.
 */
int paint_layers_edit_add(Material &material, int kind, int ordinal, const StackAddArgs &args);
bool paint_layers_edit_remove(Material &material, int ordinal);
bool paint_layers_edit_move(Material &material,
                            int from_ordinal,
                            int anchor_ordinal,
                            StackMovePlace place,
                            int *r_ordinal);
bool paint_layers_edit_set_enabled(Material &material, int ordinal, bool enable);
int paint_layers_edit_duplicate(Main &bmain, Material &material, int ordinal);
bool paint_layers_edit_rename(Material &material, int ordinal, const char *name);
bool paint_layers_edit_mask_set(Material &material, int ordinal, bool add);
bool paint_layers_edit_mask_toggle(Material &material, int ordinal);
int paint_layers_edit_merge_down(Material &material, int ordinal);
int paint_layers_edit_group_range(Material &material, int from_ordinal, int to_ordinal);
int paint_layers_edit_ungroup(Material &material, int ordinal);
int paint_layers_edit_group_add(Material &material, int ordinal);
bool paint_layers_edit_color_tag(Material &material, int ordinal, int color_tag);
bool paint_layers_edit_fill_color(Material &material, int ordinal, const float color[4]);
bool paint_layers_edit_reorder(Material &material, int from_ordinal, int to_ordinal);

}  // namespace ed::outliner
}  // namespace blender
