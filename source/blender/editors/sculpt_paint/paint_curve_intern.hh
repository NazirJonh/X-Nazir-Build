/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal API shared between the paint-curve translation units
 * (`paint_curve.cc`, `paint_curve_geometry.cc`, `paint_curve_sync.cc`,
 * `paint_curve_convert.cc`, `paint_curve_undo.cc`) and their consumers in the
 * sculpt/paint module. Cross-module entry points live in `ED_paint.hh` instead.
 */

#pragma once

#include "BLI_function_ref.hh"
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

struct Brush;
struct Curve;
struct Main;
struct Paint;
struct ViewContext;
struct bContext;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperatorType;

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

void PAINTCURVE_OT_new(wmOperatorType *ot);
void PAINTCURVE_OT_add_point(wmOperatorType *ot);
void PAINTCURVE_OT_insert_or_add_point(wmOperatorType *ot);
void PAINTCURVE_OT_new_spline(wmOperatorType *ot);
void PAINTCURVE_OT_delete_point(wmOperatorType *ot);
void PAINTCURVE_OT_duplicate(wmOperatorType *ot);
void PAINTCURVE_OT_select(wmOperatorType *ot);
void PAINTCURVE_OT_slide(wmOperatorType *ot);
void PAINTCURVE_OT_slide_radius(wmOperatorType *ot);
void PAINTCURVE_OT_draw(wmOperatorType *ot);
void PAINTCURVE_OT_cursor(wmOperatorType *ot);
void PAINTCURVE_OT_from_curve_object(wmOperatorType *ot);
void PAINTCURVE_OT_to_curve_object(wmOperatorType *ot);
void PAINTCURVE_OT_sculpt_pick(wmOperatorType *ot);
wmKeyMap *paintcurve_slide_modal_keymap(wmKeyConfig *keyconf);
bool paintcurve_slide_is_active();

/** \} */

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
/** Check if the paint curve is cyclic (single-curve only, use paintcurve_is_curve_cyclic for multi-curve). */
bool paintcurve_is_cyclic(const PaintCurve *pc);
bool paintcurve_is_curve_cyclic(const PaintCurve *pc, int curve_index);
bool paintcurve_has_multi_curves(const PaintCurve *pc);
bool paintcurve_uses_3d_geometry(const PaintCurve *pc);
void paintcurve_foreach_bezier_segment(
    const PaintCurve *pc, FunctionRef<void(int point_index_a, int point_index_b)> fn);
void paintcurve_geometry_init_bezier(bke::CurvesGeometry &geom, int point_num);
/** Cycle the handle type of a control point: Auto → Vector → Free → Auto. */
void paintcurve_cycle_point_handle_type(PaintCurve *pc, int point_index);
/** Index of the spline that owns point `point_index`, or -1 if out of range. */
int paintcurve_curve_of_point(const PaintCurve *pc, int point_index);
/** Clamp/validate the active spline index against current topology. */
int paintcurve_active_curve_get(const PaintCurve *pc);
/** Append `point_num` bezier points as a NEW spline; returns new spline index (or -1). */
int paintcurve_geometry_add_spline(bke::CurvesGeometry &geom, int point_num);
/** Remove selected points (mask over all points), dropping any spline left empty. */
void paintcurve_geometry_remove_points(bke::CurvesGeometry &geom,
                                       const IndexMask &points_to_remove);
/**
 * Find the point index in `screen_points` whose control position is closest to `pos`.
 * Returns -1 if `screen_points` is empty.
 */
int paintcurve_find_point_index(Span<PaintCurvePoint> screen_points,
                                const float pos[2],
                                float threshold);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-Point Selection Attributes
 *
 * Packed `int8_t` attribute per point: bit 0 = left handle, bit 1 = control point,
 * bit 2 = right handle.  Stored as geometry attribute so it persists across save/load/undo.
 * \{ */

uint8_t paintcurve_geom_get_selection(const bke::CurvesGeometry &geom, int point_index);
void paintcurve_geom_set_selection(bke::CurvesGeometry &geom, int point_index, uint8_t flags);
void paintcurve_geom_set_all_selection(bke::CurvesGeometry &geom, uint8_t flags);
bool paintcurve_geom_any_selected(const bke::CurvesGeometry &geom);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Conversion
 * \{ */

/** Create a new paint-curve data-block owned alongside `brush` and return it. */
PaintCurve *paintcurve_for_brush_add(Main *bmain, const char *name, const Brush *brush);
void paintcurve_geometry_from_curves(PaintCurve *pc,
                                     const bke::CurvesGeometry &src,
                                     const float4x4 &transform);
void paintcurve_geometry_from_legacy_curve(PaintCurve *pc,
                                           const Curve *curve,
                                           const float4x4 &transform);
bool paintcurve_sync_to_source_object(bContext *C, PaintCurve *pc);

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
float paintcurve_radius_to_pixel_radius(const Paint *paint, const Brush *brush, float point_radius);
/** Size factor relative to the brush radius, for stroke spacing along a paint curve. */
float paintcurve_radius_to_size_factor(const Paint *paint, const Brush *brush, float point_radius);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Radius Handle Screen-Space Helpers
 * \{ */

struct PaintCurveRadiusHandleScreen {
  float2 point;
  float2 end;
  float2 perp;
};

void paintcurve_build_screen_points(const PaintCurve *pc,
                                    const ViewContext *vc,
                                    Vector<PaintCurvePoint> &r_screen_points);

/**
 * Tessellate one Bezier segment into screen-space polyline vertices.
 * For 3D paint curves, evaluates the segment in object space (matching viewport curve wires)
 * then projects. For legacy 2D curves, tessellates projected control points in screen space.
 */
void paintcurve_build_screen_segment_polyline(const PaintCurve *pc,
                                              const ViewContext *vc,
                                              int point_index_a,
                                              int point_index_b,
                                              Span<PaintCurvePoint> screen_points_fallback,
                                              Vector<float2> &r_polyline);

/**
 * Project every bezier spline of `geom` (in object local space) into screen-space polylines,
 * one #blender::Vector<float2> per spline, smoothed with forward-difference subdivision.
 * `ob_to_world` is the curve object's transform; `vc` supplies the region used for projection.
 * Non-bezier splines are skipped. Coordinates are guarded against NaN/Inf.
 */
void paintcurve_build_object_screen_polylines(const blender::bke::CurvesGeometry &geom,
                                              const blender::float4x4 &ob_to_world,
                                              const ViewContext *vc,
                                              blender::Vector<blender::Vector<blender::float2>>
                                                  &r_polylines);

/**
 * Return the view-layer curve object (#OB_CURVES or #OB_CURVES_LEGACY) whose projected
 * silhouette passes closest to screen-space `mval`, within `threshold` pixels, or nullptr.
 * `exclude` is skipped. When non-null, `r_polylines` receives the winning object's
 * screen polylines so the caller can draw them without recomputing.
 */
Object *paintcurve_nearest_scene_curve(const ViewContext *vc,
                                       blender::float2 mval,
                                       float threshold,
                                       const Object *exclude,
                                       blender::Vector<blender::Vector<blender::float2>>
                                           *r_polylines);

/**
 * Distance-test `mval` against pre-projected cached polylines; returns the nearest object within
 * `threshold` pixels (or nullptr). `r_hover_polylines` receives that object's polylines.
 * Used by ED_paint_curve_screen_silhouettes_build_cached so the draw module avoids re-projecting.
 */
const Object *paintcurve_nearest_from_cached_polylines(
    float2 mval,
    float threshold,
    const Object *exclude,
    Span<ed::sculpt_paint::PaintCurveCachedObjectSilhouette> cached,
    Vector<Vector<float2>> *r_hover_polylines);

void paintcurve_radius_handle_screen_get(const PaintCurve *pc,
                                         const PaintCurvePoint *screen_points,
                                         int point_index,
                                         PaintCurveRadiusHandleScreen *r_handle);
int paintcurve_find_radius_handle_at_pos(const PaintCurve *pc,
                                         const PaintCurvePoint *screen_points,
                                         const float pos[2],
                                         float threshold);
float paintcurve_radius_from_handle_screen_pos(const PaintCurveRadiusHandleScreen *handle,
                                               const float pos[2]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Defines
 * \{ */

/* Number of segments used to tessellate a paint-curve bezier segment for drawing/stroking. */
#define PAINT_CURVE_NUM_SEGMENTS 40
/** Screen-space handle length at paint-curve radius factor 1.0. */
#define PAINT_CURVE_RADIUS_HANDLE_BASE_LEN 40.0f
/** Screen-space radius of the radius-handle endpoint circle. */
#define PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS 10.0f
/** Minimum screen-space offset of the radius-handle endpoint from the pivot. Keeps the handle a
 * distinct, grabbable target that never shadows clicks on the pivot, even at radius 0. */
#define PAINT_CURVE_RADIUS_HANDLE_MIN_OFFSET 10.0f
/** Screen-space pixel radius within which the cursor "hovers" a scene curve silhouette,
 * or a handle point on the active curve. */
#define PAINT_CURVE_HOVER_THRESHOLD 12.0f
/** Screen-space pixel radius (Euclidean) within which the cursor hovers a Bezier segment
 * on the active curve, used for the segment-slide affordance. */
#define PAINT_CURVE_SEGMENT_HOVER_THRESHOLD 30.0f

/** \} */

}  // namespace blender
