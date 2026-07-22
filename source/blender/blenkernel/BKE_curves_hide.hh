/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 * \brief Element visibility for #CurvesGeometry.
 *
 * Visibility is stored in the optional `.hide_point` and `.hide_curve` boolean attributes,
 * mirroring the mesh `.hide_vert` / `.hide_edge` / `.hide_poly` convention. A missing attribute
 * means that every element of that domain is visible, so the attributes are removed rather than
 * filled with false when nothing is hidden.
 *
 * The two domains are kept consistent by the flush functions below, in the same way meshes flush
 * vertex visibility to edges and faces.
 */

#pragma once

#include "BLI_index_mask_fwd.hh"

#include "BKE_attribute_enums.hh"

namespace blender::bke {
class CurvesGeometry;
}

namespace blender::bke::curves {

/** \return True when at least one point or curve is hidden. */
bool has_hidden_elements(const CurvesGeometry &curves);

/**
 * Compute the elements of \a domain that are not hidden.
 *
 * \param domain: Either #AttrDomain::Point or #AttrDomain::Curve. Every element is considered
 * visible for any other domain.
 */
IndexMask visible_mask(const CurvesGeometry &curves,
                       AttrDomain domain,
                       IndexMaskMemory &memory);

/**
 * Make curve visibility consistent with point visibility, hiding a curve when all of its points
 * are hidden. Both attributes are removed when nothing is hidden anymore.
 */
void hide_point_flush(CurvesGeometry &curves);

/**
 * Make point visibility consistent with curve visibility, hiding exactly the points of hidden
 * curves. Both attributes are removed when nothing is hidden anymore.
 *
 * \note Like #mesh_hide_face_flush this overwrites the point domain entirely, so per-point
 * visibility set beforehand is not preserved.
 */
void hide_curve_flush(CurvesGeometry &curves);

/** Hide or show the points in \a mask and flush the result to the curve domain. */
void hide_points(CurvesGeometry &curves, const IndexMask &mask, bool hide);

/** Hide or show the curves in \a mask and flush the result to the point domain. */
void hide_curves(CurvesGeometry &curves, const IndexMask &mask, bool hide);

/**
 * Make every element visible again by removing both visibility attributes.
 *
 * \return True when something was hidden before.
 */
bool show_all(CurvesGeometry &curves);

}  // namespace blender::bke::curves
