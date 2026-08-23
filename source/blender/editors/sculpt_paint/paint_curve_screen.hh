/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Everything that puts a paint curve on a SCREEN and reads a cursor back off it: projection in
 * both directions, the picking cores every editor shares, the radius handle, surface placement and
 * the drag appliers that need a #ViewContext to do their work.
 *
 * The dividing line against `paint_curve_geometry.hh` is the #ViewContext: a function that takes
 * one belongs here, a function that works in the curve's own space belongs there. Implemented by
 * `paint_curve_sync.cc` and `paint_curve_slide.cc`.
 */

#pragma once

#include <cstdint>

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
/** \name Snap Marker
 * \{ */

/**
 * Overlay snap marker published by a 3D paint-curve slide or a Curve Patch point drag.
 * Overlay cannot read operator custom-data, so the hit is stored here in world space.
 * Returns false when no geometry snap is currently active.
 * \param r_world_pos: receives the snapped target in world space for per-viewport projection.
 * \param r_type: receives the active snap elements (#SCE_SNAP_TO_GEOM subset) for marker styling.
 */
bool paintcurve_snap_marker_get(float r_world_pos[3], int *r_type);
/** Store an object-space snap hit in world space for per-viewport overlay projection. */
void paintcurve_snap_marker_update(bContext *C,
                                   const float ob_to_world[4][4],
                                   const float hit_obj[3]);
/** Hide the overlay snap marker. Safe to call when none is active. */
void paintcurve_snap_marker_clear();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projection & Picking
 * \{ */

/**
 * ID-editor closest-segment pick. Intentionally not a wrapper around
 * #paintcurve_find_closest_segment_from_geometry: that core uses squared polyline distance,
 * this path uses the Paint Curve edge-hit metric.
 */
bool paintcurve_find_closest_segment(PaintCurve *pc,
                                     const ViewContext *vc,
                                     Span<PaintCurvePoint> screen_points,
                                     const float pos[2],
                                     float threshold,
                                     int *r_segment_index,
                                     int *r_segment_index_next,
                                     float *r_edge_t);
/**
 * Closest Bezier segment to \a pos within \a threshold screen pixels, on a standalone
 * `CurvesGeometry`. Uses squared polyline distance (#ED_paint_curve_polyline_distance_sq), not
 * the ID-editor's edge-hit metric in #paintcurve_find_closest_segment. \a use_3d_space /
 * \a screen_points_fallback match the other `_from_geometry` pick helpers. Also used by
 * #paintcurve_find_insert_segment_from_geometry.
 */
bool paintcurve_find_closest_segment_from_geometry(const bke::CurvesGeometry &geom,
                                                   bool use_3d_space,
                                                   const ViewContext *vc,
                                                   Span<PaintCurvePoint> screen_points_fallback,
                                                   const float pos[2],
                                                   float threshold,
                                                   int *r_segment_index,
                                                   int *r_segment_index_next,
                                                   float *r_edge_t,
                                                   float *r_dist_sq = nullptr);
/**
 * Insert-on-segment hit-test over a standalone `CurvesGeometry`. Shared by the Paint Curve ID
 * editor (#paintcurve_try_insert_point_at_mouse) and Curve Patch Ctrl+RMB. Uses the polyline
 * metric of #paintcurve_find_closest_segment_from_geometry, not the ID-editor's edge-hit
 * #paintcurve_find_closest_segment. Returns false when a control point is closer than
 * #PAINT_CURVE_HOVER_THRESHOLD, no segment is within #PAINT_CURVE_INSERT_SEGMENT_THRESHOLD,
 * the hit is too close to a segment endpoint, or handle attributes are missing.
 * \a r_segment_index_next and \a r_dist_sq may be null.
 */
bool paintcurve_find_insert_segment_from_geometry(const bke::CurvesGeometry &geom,
                                                  bool use_3d_space,
                                                  const ViewContext *vc,
                                                  const float pos[2],
                                                  int *r_segment_index,
                                                  int *r_segment_index_next,
                                                  float *r_edge_t,
                                                  float *r_dist_sq = nullptr);
void paintcurve_object_to_screen(const ViewContext *vc,
                                 const float ob_to_world[4][4],
                                 const float ob_co[3],
                                 float r_screen[2]);
void paintcurve_screen_to_object(const ViewContext *vc,
                                 const float pivot_world[3],
                                 const float world_to_ob[4][4],
                                 const float screen_co[2],
                                 float r_ob_co[3]);
/**
 * Object-space delta of a screen drag through the plane at \a pivot_world.
 * Shared by Paint Curve slide (entire-point) and Curve Patch point drag when surface snap misses.
 */
void paintcurve_object_delta_from_screen_drag(const ViewContext *vc,
                                              const float world_to_ob[4][4],
                                              const float pivot_world[3],
                                              const float mval_init[2],
                                              const float mval_curr[2],
                                              float r_obj_delta[3]);
/**
 * Move a control point and both handles by \a obj_delta from drag-start positions.
 * Does not recalculate Bezier handles -- callers that need that (Curve Patch; surface snap)
 * do it themselves. Paint Curve entire-point slide historically does not.
 */
void paintcurve_apply_point_translate_3d(bke::CurvesGeometry &geom,
                                         int point_index,
                                         const float3 initial_loc_3d[3],
                                         const float3 &obj_delta);
/**
 * Snap a control point onto a surface hit: translate from drag-start positions and store
 * \a hit_no_obj as the point's surface normal. Recalculates Bezier auto/aligned handles.
 */
void paintcurve_apply_point_surface_snap(bke::CurvesGeometry &geom,
                                         int point_index,
                                         const float3 initial_loc_3d[3],
                                         const float3 &hit_obj,
                                         const float3 &hit_no_obj);

/**
 * Place a curve point under `mval` on the active object's surface.
 *
 * Levels, first success wins: scene snap elements (only while the header Snap toggle is on),
 * the active object's BVH, the depth buffer, and finally the object-origin plane. Shared by the
 * `PAINTCURVE_OT_*` operators and the curve-patch edit modal so both place points identically.
 *
 * \param snap_ctx: reusable snap context, may be null (created and destroyed per call).
 * \param prev_co_world: previous world position used by the snap heuristics, may be null.
 * \param use_depth_fallback: allow the depth-buffer level. Off for modal drags, where the
 * required depth-buffer refresh would run on every mouse move.
 * \param r_co_obj: hit position in the active object's space.
 * \param r_no_obj: unit-length surface normal in the active object's space.
 * \return true when a real surface was hit; false when only the origin plane was used.
 */
bool paintcurve_surface_place(bContext *C,
                              PaintCurveSnapContext *snap_ctx,
                              const ViewContext &vc,
                              const float mval[2],
                              const float prev_co_world[3],
                              bool use_depth_fallback,
                              float r_co_obj[3],
                              float r_no_obj[3]);
/**
 * Core of #paintcurve_point_add: append a control point at `co`, either extending the spline at
 * `active_curve`/`add_index` or starting a new spline when `create_new_spline` is true.
 * `active_curve` and `add_index` are updated in place to reflect the point that was added,
 * mirroring #PaintCurve.active_curve/add_index.
 * `surface_normal` is stored per point in the active object's space; see
 * #paintcurve_geom_set_surface_normal.
 */
/**
 * Find the point index in `screen_points` whose control position is closest to `pos`.
 * Returns -1 if `screen_points` is empty.
 */
int paintcurve_find_point_index(Span<PaintCurvePoint> screen_points,
                                const float pos[2],
                                float threshold);
/** Bit values written to `paintcurve_find_in_screen_points()`'s `r_selflag`: left handle,
 * pivot/control point, right handle respectively. */
#define SEL_F1 (1 << 0)
#define SEL_F2 (1 << 1)
#define SEL_F3 (1 << 2)

/**
 * Closest control point / handle under \a pos within manhattan \a threshold pixels.
 * When \a ignore_pivot is true, a click on the pivot redirects to the nearer handle.
 * Returns the point index, or -1. Optionally sets \a r_selflag (SEL_F1/F2/F3).
 */
int paintcurve_find_in_screen_points(Span<PaintCurvePoint> screen_points,
                                     const float pos[2],
                                     bool ignore_pivot,
                                     float threshold,
                                     char *r_selflag);

/**
 * Reshape the Bezier segment `point_i1` -> `point_i2` so it passes through `mval` at parameter
 * `segment_t` (clamped to [0.1, 0.9]), solving for the two adjacent handles. `depth_world` is the
 * world-space point used as the win-to-3d unprojection plane (same plane as
 * #paintcurve_object_delta_from_screen_drag). Operates directly on `geom` -- takes no #PaintCurve.
 */
void paintcurve_apply_segment_move_3d(bke::CurvesGeometry &geom,
                                      int point_i1,
                                      int point_i2,
                                      float segment_t,
                                      const ViewContext *vc,
                                      const float world_to_ob[4][4],
                                      const float mval[2],
                                      const float depth_world[3]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Radius Handle Screen-Space Helpers
 * \{ */

struct PaintCurveRadiusHandleScreen {
  float2 point;
  float2 end;
  float2 perp;
};

/**
 * Core of #paintcurve_build_screen_points: operates directly on `geom`/`use_3d_space` so it can
 * be reused without a #PaintCurve (e.g. for a standalone control curve).
 */
void paintcurve_build_screen_points_from_geometry(const bke::CurvesGeometry &geom,
                                                  const bool use_3d_space,
                                                  const ViewContext *vc,
                                                  Vector<PaintCurvePoint> &r_screen_points);
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
 * Core of #paintcurve_build_screen_segment_polyline for a standalone control curve (e.g. a Curve
 * Patch) or a viewport-bound paint curve. When `use_3d_space` is false, tessellates from
 * `screen_points_fallback`; otherwise projects object-space geometry through `vc`.
 */
void paintcurve_build_screen_segment_polyline_from_geometry(
    const bke::CurvesGeometry &geom,
    bool use_3d_space,
    const ViewContext *vc,
    int point_index_a,
    int point_index_b,
    Span<PaintCurvePoint> screen_points_fallback,
    Vector<float2> &r_polyline);

/**
 * Screen hit on a tessellated segment, returning the Bezier parameter in [0, 1] for
 * #bke::curves::bezier::insert. Tessellation steps are uniform in Bezier parameter space,
 * not arc-length (unlike #ED_paint_curve_polyline_param_at_closest_point).
 */
bool paintcurve_bezier_param_at_screen_pos_on_segment(const ViewContext *vc,
                                                      PaintCurve *pc,
                                                      const float pos[2],
                                                      int point_index_a,
                                                      int point_index_b,
                                                      Span<PaintCurvePoint> screen_points,
                                                      float &r_bezier_t,
                                                      float *r_min_dist = nullptr);

/** Core of #paintcurve_bezier_param_at_screen_pos_on_segment for a standalone control curve. */
bool paintcurve_bezier_param_at_screen_pos_on_segment_from_geometry(
    const ViewContext *vc,
    const bke::CurvesGeometry &geom,
    bool use_3d_space,
    const float pos[2],
    int point_index_a,
    int point_index_b,
    Span<PaintCurvePoint> screen_points_fallback,
    float &r_bezier_t);

/**
 * Project every bezier spline of `geom` (in object local space) into screen-space polylines,
 * one #blender::Vector<float2> per spline, smoothed with forward-difference subdivision.
 * `ob_to_world` is the curve object's transform; `vc` supplies the region used for projection.
 * Non-bezier splines are skipped. Coordinates are guarded against NaN/Inf.
 */
void paintcurve_build_object_screen_polylines(
    const blender::bke::CurvesGeometry &geom,
    const blender::float4x4 &ob_to_world,
    const ViewContext *vc,
    blender::Vector<blender::Vector<blender::float2>> &r_polylines);

/**
 * Build screen-space polylines for a single scene curve object (#OB_CURVES or #OB_CURVES_LEGACY),
 * converting a legacy curve to a temporary #Curves as needed. Other object types yield no output.
 */
void paintcurve_object_screen_polylines(const ViewContext *vc,
                                        const Object *ob,
                                        Vector<Vector<float2>> &r_polylines);

/**
 * Return the view-layer curve object (#OB_CURVES or #OB_CURVES_LEGACY) whose projected
 * silhouette passes closest to screen-space `mval`, within `threshold` pixels, or nullptr.
 * `exclude` is skipped. When non-null, `r_polylines` receives the winning object's
 * screen polylines so the caller can draw them without recomputing.
 */
Object *paintcurve_nearest_scene_curve(
    const ViewContext *vc,
    blender::float2 mval,
    float threshold,
    const Object *exclude,
    blender::Vector<blender::Vector<blender::float2>> *r_polylines);

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
/** Core of #paintcurve_radius_handle_screen_get for a standalone control curve. */
void paintcurve_radius_handle_screen_get_from_geometry(const bke::CurvesGeometry &geom,
                                                       const PaintCurvePoint *screen_points,
                                                       int point_index,
                                                       PaintCurveRadiusHandleScreen *r_handle);
int paintcurve_find_radius_handle_at_pos(const PaintCurve *pc,
                                         const PaintCurvePoint *screen_points,
                                         const float pos[2],
                                         float threshold);
/** Core of #paintcurve_find_radius_handle_at_pos for a standalone control curve. */
int paintcurve_find_radius_handle_at_pos_from_geometry(const bke::CurvesGeometry &geom,
                                                       Span<PaintCurvePoint> screen_points,
                                                       const float pos[2],
                                                       float threshold);
float paintcurve_radius_from_handle_screen_pos(const PaintCurveRadiusHandleScreen *handle,
                                               const float pos[2]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Screen-space handle length at paint-curve radius factor 1.0. */
constexpr float PAINT_CURVE_RADIUS_HANDLE_BASE_LEN = 40.0f;
/** Minimum screen-space offset of the radius-handle endpoint from the pivot. Keeps the handle a
 * distinct, grabbable target that never shadows clicks on the pivot, even at radius 0. */
constexpr float PAINT_CURVE_RADIUS_HANDLE_MIN_OFFSET = 10.0f;
/** Screen-space pixel radius within which the cursor "hovers" a scene curve silhouette,
 * or a handle point on the active curve. */
constexpr float PAINT_CURVE_HOVER_THRESHOLD = 12.0f;
/** Screen-space pixel radius (Euclidean) within which the cursor hovers a Bezier segment
 * on the active curve, used for the segment-slide affordance. */
constexpr float PAINT_CURVE_SEGMENT_HOVER_THRESHOLD = 30.0f;
/** Screen-space manhattan radius for picking control points and handles. */
constexpr float PAINT_CURVE_POINT_SELECT_THRESHOLD = 40.0f;
/** Screen-space pixel radius for Ctrl+RMB segment insertion. Wider than
 * #PAINT_CURVE_SEGMENT_HOVER_THRESHOLD so clicks near a tessellated curve wire reliably subdivide
 * instead of extending the spline. */
constexpr float PAINT_CURVE_INSERT_SEGMENT_THRESHOLD = 30.0f;
/** Bezier \a t range for subdividing a segment. Hits outside append/extend instead of insert. */
constexpr float PAINT_CURVE_INSERT_T_MIN = 0.1f;
constexpr float PAINT_CURVE_INSERT_T_MAX = 0.9f;
/* The radius-handle endpoint circle radius is shared with the overlay engine, so its single
 * definition lives in `ED_paint_curve_draw.hh`. Re-export it here so the internal translation
 * units can keep using the unqualified name. */
using ed::sculpt_paint::PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS;

/** \} */

}  // namespace blender
