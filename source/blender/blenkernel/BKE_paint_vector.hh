/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Paint Vector ID: the persisted vector document of the Paint Shapes + Curve Patch tools.
 * Owns its item geometry and the embedded style snapshots; the runtime counterpart and the
 * `VectorDocument` converters live in the editor layer (`editors/sculpt_paint`).
 */

#pragma once

#include "BLI_span.hh"

#include "DNA_paint_vector_types.h"

namespace blender {

struct Main;
struct Material;
struct PaintVector;
struct PaintVectorItem;
struct PaintVectorPoint;
struct PaintVectorSpline;

namespace bke {
struct CurvePatchParams;
}  // namespace bke

/**
 * ID-property group on a Material that holds its PaintVector references. The link is
 * an ordinary `IDP_ID` custom property, so `foreach_id` counts the PaintVector as used by the
 * material and append / duplicate carry it; it is never stored on material nodes.
 */
#define PAINT_VECTOR_MATERIAL_GROUP "pbr_paint_vectors"

/** Add (or refresh) a reference to \a pv in \a ma's `pbr_paint_vectors` group, keyed by name. */
void BKE_paint_vector_material_link(Material &ma, PaintVector &pv);
/** Remove \a pv's reference from \a ma, if present. */
void BKE_paint_vector_material_unlink(Material &ma, const PaintVector &pv);
/** The PaintVector referenced by \a ma under \a name, or null. */
PaintVector *BKE_paint_vector_material_get(const Material &ma, const char *name);
/** Number of PaintVector references on \a ma. */
int BKE_paint_vector_material_count(const Material &ma);
/** The \a index-th (in group order) PaintVector referenced by \a ma, or null. */
PaintVector *BKE_paint_vector_material_get_index(const Material &ma, int index);

/** Mark the document changed: bumps `bake_revision` (call after every write). */
void BKE_paint_vector_tag_changed(PaintVector &pv);
/** Record that the target maps were baked from the current `bake_revision`. */
void BKE_paint_vector_tag_baked(PaintVector &pv);
/** True when the document changed after the last bake. */
bool BKE_paint_vector_needs_rebake(const PaintVector &pv);
/** True once the target maps were baked at least once (the explicit DNA `BAKED` flag). */
bool BKE_paint_vector_was_baked(const PaintVector &pv);

/** Add an empty PaintVector to \a bmain. */
PaintVector *BKE_paint_vector_add(Main *bmain, const char *name);

/** Remove every item (and its owned geometry / style profiles) from \a pv. */
void BKE_paint_vector_items_clear(PaintVector &pv);

/** Append an empty item, make it active and return it. The arrays start empty. */
PaintVectorItem &BKE_paint_vector_item_add(PaintVector &pv);

/**
 * Remove the \a index-th item, freeing its owned data. The items after it shift one slot down and
 * #PaintVector::active_item is clamped. False when the index is out of range.
 */
bool BKE_paint_vector_item_remove(PaintVector &pv, int index);

/**
 * Exchange the \a a-th and \a b-th items in place, keeping every owned array with its item.
 * #PaintVector::active_item follows the item it pointed at. Used by the Curve Patch curve list's
 * up/down controls. False when either index is out of range.
 */
bool BKE_paint_vector_item_swap(PaintVector &pv, int a, int b);

/**
 * Allocate (or reallocate) the item's flat point array and return it. Any previous array is freed
 * first, so calling this again on the same item does not leak.
 */
MutableSpan<PaintVectorPoint> BKE_paint_vector_item_points_alloc(PaintVectorItem &item, int num);

/** Same as points_alloc, for the spline table. */
MutableSpan<PaintVectorSpline> BKE_paint_vector_item_splines_alloc(PaintVectorItem &item, int num);

/**
 * Give \a item's embedded style snapshot its non-null owned profiles / color ramps (the same
 * defaults a fresh `PaintShapeSettings` gets, see #BKE_paint_shape_settings_init). A new item has
 * a zeroed style; call this before filling it.
 */
void BKE_paint_vector_item_style_init(PaintVectorItem &item);

/**
 * Deep-copy \a src into \a item's embedded style snapshot: scalars copy directly, the owned
 * profiles / color ramps are duplicated so the item can be freed independently. Any owned data
 * already on \a item is freed first.
 */
void BKE_paint_vector_item_style_copy(PaintVectorItem &item, const PaintShapeSettings &src);

/** Free the owned profiles / color ramps of a standalone style snapshot (leaves scalars). */
void BKE_paint_vector_style_free(PaintShapeSettings &style);
/** Deep-copy a standalone style snapshot (the owned pointers are duplicated). */
void BKE_paint_vector_style_copy(PaintShapeSettings &dst, const PaintShapeSettings &src);

/**
 * Whether \a item's embedded Curve Patch geometry is constructed and safe to read or copy. False
 * for every other item type, and for a geometry whose runtime was never established (raw file
 * bytes, or data left behind by an old write-time view).
 */
bool BKE_paint_vector_item_curve_geometry_is_valid(const PaintVectorItem &item);

/**
 * The field-by-field conversion between the DNA #PaintVectorCurveParams mirror and
 * `bke::CurvePatchParams` (see `BKE_curve_patch.hh`). Pure POD, no editor types, so the bake
 * renderer can decode the frozen per-curve parameters. Callers include `BKE_curve_patch.hh`
 * themselves; the return type is only forward-declared here.
 */
bke::CurvePatchParams BKE_paint_vector_curve_params_decode(const PaintVectorCurveParams &params);
/** The inverse of #BKE_paint_vector_curve_params_decode. */
PaintVectorCurveParams BKE_paint_vector_curve_params_encode(const bke::CurvePatchParams &params);

}  // namespace blender
