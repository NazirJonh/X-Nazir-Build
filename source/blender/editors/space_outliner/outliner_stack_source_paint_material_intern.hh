/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup spoutliner
 *
 * Internal seams of the paint-material stack source, shared between the implementation and its
 * unit test.
 *
 * Nothing here is part of the #StackSource contract: these functions name the concrete material
 * types the source is built on, which is exactly what the generic Outliner code must not see. They
 * are declared once here so the test does not carry its own copy of a signature, which would
 * silently drift from the definition.
 */

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_paint_material_layer_model.hh"

#include "outliner_stack_source.hh"

namespace blender {

struct Image;
struct Main;
struct Paint;
struct Scene;

namespace ed::outliner {

/**
 * The mask-editing half of a preview-slot click, testable without a #bContext (see
 * #PaintMaterialStackSource::preview_activate, which resolves \a scene from context and calls
 * this). \a paint is the Paint whose brush is swapped. \a section_id names which preview was
 * clicked ("MASK" or "CHANNELS", see #outliner_stack_preview_section_from_cursor); any other
 * value is a no-op.
 */
bool paint_material_mask_preview_activate(Main &bmain,
                                           Scene &scene,
                                           Paint &paint,
                                           const StackRow &row,
                                           const StringRef section_id);

/**
 * Whether the MASK content section of \a row shows \a mask_image -- or, for a mask correction
 * row, whether the image is the correction's own map (named by its preview slot). Testable
 * without a #bContext like #paint_material_mask_preview_activate above: row activation uses it
 * to decide if the mask being painted survives the switch.
 */
bool paint_row_owns_mask_image(const StackRow &row, const Image *mask_image);

/**
 * One row of the stack, by the ordinal that addresses it: a layer row itself, or one of the
 * corrections hanging off it.
 *
 * The routes are what the tests have to check directly, so the type is shared with them rather
 * than duplicated there.
 */
struct PaintStackRowRoute {
  /** False for the row a layer or group itself gets, true for one of its corrections. */
  bool is_correction = false;
  /** The ordinal of the layer row the route stands for, or hangs off. */
  int layer_ordinal = -1;
  /** The correction's identity; nil for a layer route. */
  bUUID correction = {};
  /** The parent's section the correction hangs under; meaningless for a layer route. */
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
};

/**
 * The deterministic ordinal budget the rows are addressed by, shared by #rows_build and every
 * edit that routes an ordinal.
 */
Map<int, PaintStackRowRoute> paint_stack_routes_build(
    Span<PaintMaterialLayerStackEntry> entries, int &r_first_unaddressable_index);

/** The per-entry row builder. */
void paint_stack_rows_from_entries(Span<PaintMaterialLayerStackEntry> entries,
                                   int shown_channel,
                                   Vector<StackRow> &r_rows);

}  // namespace ed::outliner
}  // namespace blender
