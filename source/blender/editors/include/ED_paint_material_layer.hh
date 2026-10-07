/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * Editing a layered Material's rows that the description alone cannot express: adding a Material
 * row (whose maps are baked from another material's Principled BSDF) and keeping mask editing in
 * step with the description when the row being painted is removed or switched off.
 *
 * The old graph-truth edit verbs (channel toggle/value/unlink, rebake, resize) were removed in
 * phase 6; the description's own verbs live in `BKE_paint_layers.hh` alone, and the Outliner
 * source only translates its row vocabulary onto them.
 */

#include "BKE_paint_layers.hh"

#include "BLI_uuid.h"

struct bContext;
struct Material;

namespace blender::ed::sculpt_paint::material_layer {

/**
 * Add a Material row to \a owner's description stack that reads \a source's Principled channels.
 * With a window manager the row starts without maps and is shown live from its source; the bake
 * planner renders its first maps once the row is no longer active. Without one (file read,
 * background) the maps are rendered right away, blocking.
 *
 * \a anchor and \a place are the same placement rules as #BKE_paint_layers_add. A linked \a source
 * is made local first. \return the new row, or null when it could not be added.
 */
MaterialPaintLayer *add_material_layer_from_material(bContext &C,
                                                     Material &owner,
                                                     Material &source,
                                                     MaterialPaintLayer *anchor,
                                                     PaintLayerPlace place);

/**
 * Whether layered material \a ma's mask target would be lost by removing \a removed_marker: the
 * target mode is #PAINT_LAYER_TARGET_MASK and the active marker is \a removed_marker or nested
 * under it.
 *
 * Context-free, so a unit test can drive it; the mode switch itself is
 * #mask_edit_end_if_target_removed on top.
 */
bool mask_target_is_removed(const Material &ma, const bUUID &removed_marker, int8_t target_mode);

/**
 * Leave mask editing when the layered material's active row loses the target the mask stroke
 * writes into: \a removed_marker names a row about to be removed (a layer, a folder, a correction),
 * or the row whose mask is about to be removed or switched off.
 *
 * The check is on the description alone: a layered material's target is
 * `Material.active_layer_marker` + `PaintModeSettings.layer_target_mode`. When the mode is MASK and
 * the active marker is \a removed_marker or anywhere under it, the mode returns to CONTENT -- the
 * same brush restore a mode switch always does. A no-op otherwise.
 */
void mask_edit_end_if_target_removed(bContext &C, Material &ma, const bUUID &removed_marker);

}  // namespace blender::ed::sculpt_paint::material_layer
