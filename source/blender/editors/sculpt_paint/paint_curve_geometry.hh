/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The paint curve as DATA: what a #blender::bke::CurvesGeometry holding a paint curve contains,
 * how it is queried and how it is mutated. Nothing here knows about a region, a projection or an
 * operator -- every position is in the curve's own space, whichever that is.
 *
 * Implemented by `paint_curve_geometry.cc`. Screen-space picking and projection live in
 * `paint_curve_screen.hh`; operators, keymaps and object conversion in `paint_curve_ops.hh`.
 */

#pragma once

#include <cstdint>

#include "BLI_function_ref.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "ED_paint_curve_draw.hh"

#include "DNA_brush_types.h"

namespace blender {

namespace bke {
class CurvesGeometry;
}

namespace ed::curves {
/* Forward-declared rather than pulling in `ED_curves.hh`; the underlying type must match the
 * definition there. */
enum class SetHandleType : uint8_t;
}  // namespace ed::curves

struct Brush;
struct Curve;
struct Main;
struct Paint;
struct ViewContext;
struct bContext;
struct PaintCurveSnapContext;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperator;
struct wmOperatorType;

/* -------------------------------------------------------------------- */
/** \name Geometry Access & Queries
 * \{ */

/**
 * True when the embedded #CurvesGeometry was placement-new'd. Zeroed DNA memory leaves
 * `runtime` null — accessing attributes in that state is undefined behaviour.
 */
bool paintcurve_geometry_runtime_is_initialized(const bke::CurvesGeometry &geom);
/**
 * True when the embedded geometry is allocated and holds at least one point, i.e. its attribute
 * accessors are safe to call. Centralizes the `runtime != nullptr && points_num() > 0` guard.
 */
bool paintcurve_geometry_is_valid(const bke::CurvesGeometry &geom);
/** Mutable reference to a control point position (`handle_idx` 0 = left, 1 = co, 2 = right). */
float3 &paintcurve_geom_co(bke::CurvesGeometry &geom, int point_idx, int handle_idx);
/**
 * Resolve a `SetHandleType` menu choice against a handle's current type into the concrete
 * `BEZIER_HANDLE_*` to store. Only `Toggle` actually consults `handle_type`.
 *
 * Shared with the Curve Patch editor, which drives the same enum over its own standalone
 * `CurvesGeometry` (`paint_curve_patch_edit.cc`).
 */
int8_t paintcurve_resolve_handle_type(int8_t handle_type, ed::curves::SetHandleType dst_type);
/** Check if the paint curve is cyclic (single-curve only, use paintcurve_is_curve_cyclic for
 * multi-curve). */
bool paintcurve_is_cyclic(const PaintCurve *pc);
bool paintcurve_is_curve_cyclic(const PaintCurve *pc, int curve_index);
bool paintcurve_has_multi_curves(const PaintCurve *pc);
bool paintcurve_uses_3d_geometry(const PaintCurve *pc);
void paintcurve_foreach_bezier_segment(const PaintCurve *pc,
                                       FunctionRef<void(int point_index_a, int point_index_b)> fn);
/** Core of #paintcurve_foreach_bezier_segment: operates directly on `geom` so it can be reused
 * without a #PaintCurve (e.g. for a standalone control curve). */
void paintcurve_foreach_bezier_segment_from_geometry(
    const bke::CurvesGeometry &geom, FunctionRef<void(int point_index_a, int point_index_b)> fn);
void paintcurve_geometry_init_bezier(bke::CurvesGeometry &geom, int point_num);
/** Cycle the handle type of a control point: Free → Auto → Vector → Align (matches Curves Pen). */
void paintcurve_cycle_point_handle_type(PaintCurve *pc, int point_index);
/** Index of the spline that owns point `point_index`, or -1 if out of range. */
int paintcurve_curve_of_point(const PaintCurve *pc, int point_index);
/** Core of #paintcurve_curve_of_point: operates directly on `geom` so it can be reused without a
 * #PaintCurve (e.g. for a standalone control curve). */
int paintcurve_curve_of_point_from_geometry(const bke::CurvesGeometry &geom, int point_index);
/** Clamp/validate the active spline index against current topology. */
int paintcurve_active_curve_get(const PaintCurve *pc);
/** Append `point_num` bezier points as a NEW spline; returns new spline index (or -1). */
int paintcurve_geometry_add_spline(bke::CurvesGeometry &geom, int point_num);
/**
 * Surface normal captured when the point was placed, in the active object's space.
 * Returns `(0, 0, 1)` for points that predate the attribute.
 */
float3 paintcurve_geom_get_surface_normal(const bke::CurvesGeometry &geom, int point_index);
/** Store the surface normal for `point_index`; `normal` is normalized on write. */
void paintcurve_geom_set_surface_normal(bke::CurvesGeometry &geom,
                                        int point_index,
                                        const float3 &normal);
void paintcurve_geometry_add_point(bke::CurvesGeometry &geom,
                                   const float3 &co,
                                   const float3 &surface_normal,
                                   bool create_new_spline,
                                   int &active_curve,
                                   int &add_index);
/**
 * Core of #paintcurve_insert_point_at_segment: subdivide the bezier segment
 * `segment_index` -> `segment_index_next` of `geom` at `edge_t` (matches
 * #subdivide_curves.cc semantics), selecting the newly inserted point. `active_curve` is used to
 * shift subsequent spline offsets after the resize. Returns the index of the inserted point, or
 * -1 when the segment's handle attributes are missing.
 */
int paintcurve_geometry_insert_point_at_segment(bke::CurvesGeometry &geom,
                                                int segment_index,
                                                int segment_index_next,
                                                int active_curve,
                                                float edge_t);
/** Remove selected points (mask over all points), dropping any spline left empty. */
void paintcurve_geometry_remove_points(bke::CurvesGeometry &geom,
                                       const IndexMask &points_to_remove);
/**
 * Duplicate contiguous ranges of selected control points as new splines (matches
 * #ed::curves::duplicate_points segment semantics). Returns the number of splines added.
 */
int paintcurve_geometry_duplicate_selected_points(bke::CurvesGeometry &geom);
/** Remove all selected control points from the geometry. Returns false when nothing was selected.
 */
bool paintcurve_geometry_remove_selected_points(bke::CurvesGeometry &geom);
/** Split splines at each selected point, creating new splines for each segment. */
bool paintcurve_geometry_split_at_selected_points(bke::CurvesGeometry &geom,
                                                  int *r_selected_curve = nullptr);
/** Join two splines at their selected endpoints, consuming the second spline. */
bool paintcurve_geometry_merge_curve_endpoints(
    bke::CurvesGeometry &geom, int curve_a, bool a_is_start, int curve_b, bool b_is_start);
/** Select every point on splines that have at least one selected point. */
void paintcurve_geometry_select_linked(bke::CurvesGeometry &geom);
/** Close or re-open one spline. Returns false when `curve_index` is out of range. */
bool paintcurve_geometry_toggle_cyclic(bke::CurvesGeometry &geom, int curve_index);
bool paintcurve_geometry_any_point_selected(const bke::CurvesGeometry &geom);
/**
 * Check if there is at least one spline with the minimum number of selected points.
 * Returns true if any spline has at least `min_points` selected points.
 */
bool paintcurve_geometry_has_enough_selected_points_on_spline(const bke::CurvesGeometry &geom,
                                                              int min_points);
/**
 * Build a standalone geometry with only the selected points (one spline per contiguous run on each
 * source spline). Returns empty geometry when nothing is selected.
 */
bke::CurvesGeometry paintcurve_geometry_build_from_selected_points(
    const bke::CurvesGeometry &geom);
/**
 * Space-agnostic core of #paintcurve_apply_segment_move_3d: reshape the Bezier segment
 * `point_i1` -> `point_i2` so it passes through `target` at parameter `segment_t` (clamped to
 * [0.1, 0.9]), solving for the two adjacent handles. `target` must be in the same space as the
 * geometry's positions -- object space for a 3D curve, region pixels or image UV for a flat one.
 */
void paintcurve_apply_segment_move_to_point(
    bke::CurvesGeometry &geom, int point_i1, int point_i2, float segment_t, const float3 &target);

/* -------------------------------------------------------------------- */
/** \name Per-Point Selection Attributes
 *
 * Stored in the three attributes every curves editor uses -- `.selection` for the control point,
 * `.selection_handle_left` / `.selection_handle_right` for the Bezier handles -- so `ed::curves`,
 * Transform and the overlay all read the same data. The packed `uint8_t` these take and return
 * (bit 0 = left handle, bit 1 = control point, bit 2 = right handle) is a call-site convenience
 * over those three, not a storage format.
 *
 * \note An ABSENT selection attribute means everything is selected, which is the curves
 * convention and the opposite of what a paint curve wants. "Nothing selected" is therefore always
 * written out; see the matching note in `paint_curve_geometry.cc`.
 * \{ */

uint8_t paintcurve_geom_get_selection(const bke::CurvesGeometry &geom, int point_index);
void paintcurve_geom_set_selection(bke::CurvesGeometry &geom, int point_index, uint8_t flags);
void paintcurve_geom_set_all_selection(bke::CurvesGeometry &geom, uint8_t flags);
bool paintcurve_geom_any_selected(const bke::CurvesGeometry &geom);

/** \} */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Radius Semantics
 * \{ */

/**
 * Map radius from a curve object (bevel radius semantics) to paint-curve brush factor.
 * Uninitialized/zero source values become 1.0 (full brush). Values above 1.0 are kept as-is.
 */
float paintcurve_radius_from_source_geometry(float source_radius);
/** Radius of a paint-curve control point (>= 0, unbounded above). Read from geometry. */
float paintcurve_get_point_radius(const PaintCurve *pc, int point_index);
/**
 * Map paint-curve radius to brush pixel radius.
 * 0 -> 1 px, 1 -> full brush size, >1 -> brush size multiplied by radius.
 */
float paintcurve_radius_to_pixel_radius(const Paint *paint,
                                        const Brush *brush,
                                        float point_radius);
/** Size factor relative to the brush radius, for stroke spacing along a paint curve. */
float paintcurve_radius_to_size_factor(const Paint *paint, const Brush *brush, float point_radius);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/* #PAINT_CURVE_NUM_SEGMENTS lives in `BKE_paint.hh`: the geometry stores it as its `resolution`,
 * so builders outside this module need it as well. */

/** Fraction of the shorter adjacent segment length used to offset endpoints after split. */
constexpr float PAINT_CURVE_SPLIT_ENDPOINT_SEPARATION = 0.05f;

/** \} */

}  // namespace blender
