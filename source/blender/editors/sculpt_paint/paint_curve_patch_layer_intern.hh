/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Helpers shared between `paint_curve_patch_layer.cc` and the bend-curve half in
 * `paint_curve_patch_bend.cc`. They used to be file-static; a Text Decal bend is a Curve Patch row
 * that only carries a curve, so its entry and live-update code lives beside its own file instead of
 * growing the already large layer bridge.
 */

#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"

#include "paint_curve_patch_layer.hh"

namespace blender::ed::sculpt_paint {

/**
 * The write-back both spaces share: the document's patches go into the container's items, and the
 * active patch maps back to its item. Only the geometry differs (3D stores the control curve, 2D the
 * canonical UV curve), so \a geometry_of names it per patch; null keeps the item's stored geometry.
 */
void curve_patch_document_write_items(
    const CurvePatchDocument &doc,
    PaintVector &r_pv,
    FunctionRef<const bke::CurvesGeometry *(int)> geometry_of,
    bool ensure_radius);

/** Append one default curve item to \a pv: a two-point segment sized from \a brush. */
PaintVectorItem &curve_patch_layer_curve_item_default(PaintVector &pv, const Brush *brush);

/** Append the Text Decal's 2D bend curve: a two-point segment across the container's UV. */
PaintVectorItem &curve_patch_layer_bend_curve_item_add(PaintVector &pv, const Brush *brush);

/** The frozen stamp radius pair for a curve of the given Space: world units in 3D, image pixels
 * in 2D. */
void curve_patch_layer_brush_radius_for_space(const Brush *brush,
                                              bool is_2d,
                                              float &r_radius,
                                              float &r_radius_per_size);

/** Rebuild \a item's frozen stamp parameters from \a brush as it is now, keeping its seed. */
void curve_patch_layer_item_params_refresh(PaintVectorItem &item,
                                           const Brush &brush,
                                           float radius,
                                           float radius_per_size,
                                           const float3 &plane_normal);

/** The write half the two document stores share: resolve the row and write the document into its
 * container (and into the curves objects when the row reads them). */
MaterialPaintLayer *curve_patch_layer_document_to_row(CurvePatchSession &session, bool for_save);

/** The row a live 2D layer session edits, or null when it was deleted. */
MaterialPaintLayer *curve_patch_layer_session_row_2d(ImageCurvePatchSession &session);

/** Put a Text Decal's own Space and mapping in step with the bend curve's Space (2D -> Canvas,
 * 3D -> Projected); see #BKE_paint_layers_text_decal_bend_space_sync. */
void curve_patch_layer_bend_text_space_sync(Material &ma,
                                            MaterialPaintLayer &text_row,
                                            bool to_2d);

}  // namespace blender::ed::sculpt_paint
