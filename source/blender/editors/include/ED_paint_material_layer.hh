/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * Editing a Material paint layer: one whose maps are not painted by hand but re-baked from
 * another material's Principled BSDF, kept in step with edits to that source
 * (#ed::material_bake::material_bake_images_rebake_stale).
 *
 * Every operation here shares the same shape: bake into fresh target #Image data-blocks, hand
 * them over to a #BKE_paint_material_layer_edit.hh call the moment the targets exist -- before
 * the render itself runs, since the bake job's worker touches node trees of its own and a target
 * freed out from under it would be a race -- and only then let the job fill in pixels. This is
 * the one place that sequence is written, shared by the Outliner's Add gesture and the Layer
 * Material tab's operators; neither owns it, so neither may drift from the other.
 */

#include "BKE_paint_material_layer_edit.hh"

struct bContext;
struct Material;
struct ReportList;

namespace blender::ed::sculpt_paint::material_layer {

/**
 * Add a Material layer to \a owner's stack, next to \a anchor_ordinal
 * (#PaintMaterialLayerAddParams::anchor_ordinal terms), baking every Principled channel
 * \a source feeds into fresh maps and taking them over as the new layer's own.
 *
 * \a place says which side of the anchor the layer lands on: #Above (and #Into, which the
 * add itself reads as "above, inside the anchor") inserts above the anchor the way the
 * #anchor_ordinal terms mean it; #Below inserts below it, at the anchor's own position in the
 * #PaintMaterialLayerAddParams::ordinal terms.
 *
 * A linked \a source (an asset, typically) is made local first: the layer is re-configured by
 * editing its source, which a linked material does not allow. Refusals -- nothing baked, the
 * source could not be made local, the bake failed -- are reported to \a C's window manager.
 *
 * \return the new layer's ordinal, or -1 when nothing was added.
 */
int add_from_material(bContext &C,
                      Material &owner,
                      int anchor_ordinal,
                      Material &source,
                      PaintMaterialLayerMovePlace place = PaintMaterialLayerMovePlace::Above);

/**
 * Switch \a channel of the active Material paint layer (#BKE_paint_material_active_layer_get) on
 * or off. Switching on a channel the layer does not have yet bakes a fresh map from its source;
 * switching on one it kept but disabled re-bakes it first only if it has fallen behind.
 *
 * \return whether anything changed. A refusal is reported to \a reports.
 */
bool channel_toggle(bContext &C, ReportList &reports, int channel);

/**
 * Re-fill \a channel's map of the active paint layer row (#BKE_paint_material_active_layer_get)
 * with the flat colour \a value stands for there, and record it as the channel's own value so a
 * later unlink restores it.
 *
 * An active correction row takes the value on its own channel set, not the ones of the layer it
 * hangs on.
 *
 * \return whether anything changed. A refusal is reported to \a reports.
 */
bool channel_value_set(bContext &C, ReportList &reports, int channel, const float value[4]);

/**
 * Detach whatever image \a channel of the active paint layer row shows, and give the channel back
 * a flat map of its own at the value it last recorded. The image it showed is never written to.
 *
 * An active correction row unlinks on its own channel set, not the ones of the layer it hangs on.
 *
 * \return whether anything changed. A refusal is reported to \a reports.
 */
bool channel_unlink(bContext &C, ReportList &reports, int channel);

/**
 * "Use layer result": bake the stack row at \a source_ordinal of the active row's owner for
 * \a channel -- the row's own content after its corrections, with its mask as the alpha -- and
 * wire the fresh map in as the active row's channel texture.
 *
 * \a source_ordinal names a row of the owner's stack by its position; a folder-nested or unwired
 * source has no endpoint a bake can reach and is refused before anything is baked. The bake runs
 * to completion on the calling thread; a map it minted that nothing took over is freed again.
 *
 * \return whether anything changed. A refusal is reported to \a reports.
 */
bool use_layer_result(bContext &C, ReportList &reports, int channel, int source_ordinal);

/**
 * Re-bake every map of the active Material paint layer from its source, whether or not it looks
 * current -- for when a map looks wrong although nothing the staleness check sees has changed.
 *
 * \return false when there is no active Material layer, or none of its maps carry a bake link.
 */
bool rebake(bContext &C, ReportList &reports);

/**
 * Resize every map of the active Material paint layer to \a size and re-bake them.
 *
 * \return false when there is no active Material layer, or none of its maps carry a bake link.
 */
bool resize(bContext &C, ReportList &reports, int size);

}  // namespace blender::ed::sculpt_paint::material_layer
