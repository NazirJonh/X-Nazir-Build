/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 * \brief Curves hide and show operations.
 */

#pragma once

#include <optional>

#include "BLI_index_range.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"

struct Object;

namespace blender {

struct Curves;

namespace bke {

class CurvesGeometry;

namespace curves {

/**
 * Action to perform on visibility state.
 */
enum class VisAction {
  /** Hide the selected elements. */
  Hide = 0,
  /** Show the selected elements. */
  Show = 1,
};

namespace hide {

/**
 * Ensure the ".hide_point" attribute exists for the curves and return a writable span.
 * If the attribute doesn't exist, it will be created with all values set to false.
 *
 * \param curves_id: The curves ID to ensure the attribute for.
 * \return A writable span to the hide point attribute.
 */
SpanAttributeWriter<bool> hide_point_ensure(Curves &curves_id);

/**
 * Ensure the ".hide_curve" attribute exists for the curves and return a writable span.
 * If the attribute doesn't exist, it will be created with all values set to false.
 *
 * \param curves_id: The curves ID to ensure the attribute for.
 * \return A writable span to the hide curve attribute.
 */
SpanAttributeWriter<bool> hide_curve_ensure(Curves &curves_id);

/**
 * Remove the hide attributes from the curves.
 * This removes both ".hide_point" and ".hide_curve" attributes if they exist.
 *
 * \param curves_id: The curves ID to remove the attributes from.
 */
void hide_attributes_remove(Curves &curves_id);

/**
 * Check if a specific point is hidden.
 *
 * \param curves: The curves geometry to check.
 * \param point_index: The index of the point to check.
 * \return True if the point is hidden, false otherwise.
 */
bool point_is_hidden(const CurvesGeometry &curves, int point_index);

/**
 * Check if a specific curve is hidden.
 *
 * \param curves: The curves geometry to check.
 * \param curve_index: The index of the curve to check.
 * \return True if the curve is hidden, false otherwise.
 */
bool curve_is_hidden(const CurvesGeometry &curves, int curve_index);

/**
 * Check if the curves have any hidden elements (points or curves).
 *
 * \param curves: The curves geometry to check.
 * \return True if there are any hidden elements, false otherwise.
 */
bool has_hidden_elements(const CurvesGeometry &curves);

/**
 * Hide or show points in the curves object.
 *
 * \param object: The curves object to modify.
 * \param point_mask: The mask of points to hide or show.
 * \param action: Whether to hide or show the points.
 */
void hide_points(Object &object, const IndexMask &point_mask, VisAction action);

/**
 * Hide or show curves in the curves object.
 *
 * \param object: The curves object to modify.
 * \param curve_mask: The mask of curves to hide or show.
 * \param action: Whether to hide or show the curves.
 */
void hide_curves(Object &object, const IndexMask &curve_mask, VisAction action);

/**
 * Show all hidden elements (points and curves) in the curves object.
 *
 * \param object: The curves object to show all elements for.
 * \param select: Whether to select the revealed elements.
 */
void show_all(Object &object, bool select = false);

/**
 * Invert the hidden state of elements in the curves object.
 * Hidden elements become visible and visible elements become hidden.
 *
 * \param object: The curves object to invert hide state for.
 * \param mask: The mask of elements to invert.
 * \param domain: The domain (point or curve) of the elements to invert.
 */
void invert_hide(Object &object, const IndexMask &mask, AttrDomain domain);

/**
 * Synchronize hide state from points to curves.
 * A curve is hidden if all its points are hidden.
 *
 * \param curves_id: The curves ID to synchronize.
 */
void sync_hide_from_points_to_curves(Curves &curves_id);

/**
 * Synchronize hide state from curves to points.
 * All points in a hidden curve are hidden.
 *
 * \param curves_id: The curves ID to synchronize.
 */
void sync_hide_from_curves_to_points(Curves &curves_id);

}  // namespace hide

}  // namespace curves

}  // namespace bke

}  // namespace blender
