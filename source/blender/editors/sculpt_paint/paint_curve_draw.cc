/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Public provider API for paint-curve overlay drawing.
 * Implements the functions declared in ED_paint_curve_draw.hh.
 */

#include <array>
#include <cstring>
#include <optional>

#include "ED_paint_curve_draw.hh"

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curves.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_vector.hh"
#include "BKE_workspace.hh"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "paint_curve_intern.hh"
#include "paint_image_curve_patch.hh"
#include "paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Anonymous helpers (radius gate)
 * \{ */

namespace {

static bool should_show_radius_handle_draw_from_geometry(const Sculpt *sculpt,
                                                         const bke::CurvesGeometry &geom,
                                                         const Span<PaintCurvePoint> points,
                                                         const int point_index)
{
  if (!sculpt || !sculpt->paint_curve_show_radius_handles) {
    return false;
  }

  const int8_t display_mode = sculpt->paint_curve_radius_display_mode;

  if (display_mode == SCULPT_PAINT_CURVE_RADIUS_ALL) {
    return true;
  }

  if (display_mode == SCULPT_PAINT_CURVE_RADIUS_SELECT) {
    const int curve_index = paintcurve_curve_of_point_from_geometry(geom, point_index);
    if (curve_index < 0) {
      return false;
    }
    const OffsetIndices<int> points_by_curve = geom.points_by_curve();
    const IndexRange curve_pts = points_by_curve[curve_index];
    for (const int i : curve_pts) {
      const PaintCurvePoint &pcp = points[i];
      if (pcp.bez.f1 || pcp.bez.f2 || pcp.bez.f3) {
        return true;
      }
    }
    return false;
  }

  if (display_mode == SCULPT_PAINT_CURVE_RADIUS_TIPS) {
    const int curve_index = paintcurve_curve_of_point_from_geometry(geom, point_index);
    if (curve_index < 0) {
      return false;
    }
    const OffsetIndices<int> points_by_curve = geom.points_by_curve();
    const IndexRange curve_pts = points_by_curve[curve_index];
    return (point_index == curve_pts.first() || point_index == curve_pts.last());
  }

  return false;
}

static bool should_show_radius_handle_draw(const Sculpt *sculpt,
                                           const PaintCurve *pc,
                                           const Span<PaintCurvePoint> points,
                                           const int point_index)
{
  return should_show_radius_handle_draw_from_geometry(
      sculpt, pc->geometry.wrap(), points, point_index);
}


/** The screen-space points of a Curve Patch / paint curve as a shared control-point source, so
 * their draw data comes from the same builder as every other Bezier editor's. Segments are not
 * exposed: they carry point indices the slide / insert logic below depends on. */
class ScreenPointsSource final : public control_points::Source {
 public:
  explicit ScreenPointsSource(const Span<PaintCurvePoint> points) : points_(points) {}

  int points_num() const override
  {
    return int(points_.size());
  }
  float2 point_position(const int point) const override
  {
    return float2(points_[point].bez.vec[1]);
  }
  void point_handles(const int point, float2 &r_left, float2 &r_right) const override
  {
    r_left = float2(points_[point].bez.vec[0]);
    r_right = float2(points_[point].bez.vec[2]);
  }
  int8_t handle_type(const int point, const bool left) const override
  {
    const BezTriple &bez = points_[point].bez;
    return int8_t(left ? bez.h1 : bez.h2);
  }
  uint8_t selection(const int point) const override
  {
    const BezTriple &bez = points_[point].bez;
    return uint8_t((bez.f1 != 0 ? SEL_F1 : 0) | (bez.f2 != 0 ? SEL_F2 : 0) |
                   (bez.f3 != 0 ? SEL_F3 : 0));
  }
  int segments_num() const override
  {
    return 0;
  }
  void segment_polyline(const int /*segment*/, Vector<float2> & /*r_out*/) const override {}

 private:
  Span<PaintCurvePoint> points_;
};

}  // anonymous namespace

bool ED_paint_curve_polyline_closest(const Span<float2> polyline,
                                     const float2 mval,
                                     PaintCurvePolylineHit &r_hit)
{
  if (polyline.size() < 2) {
    return false;
  }

  r_hit.dist_sq = FLT_MAX;
  float best_arc = 0.0f;
  float best_param = 0.0f;
  float arc_len = 0.0f;
  for (const int i : IndexRange(polyline.size() - 1)) {
    const float2 a = polyline[i];
    const float2 ab = polyline[i + 1] - a;
    const float seg_len_sq = math::length_squared(ab);
    const bool is_degenerate = seg_len_sq < 1e-8f;
    const float seg_len = is_degenerate ? 0.0f : sqrtf(seg_len_sq);
    float seg_t = 0.0f;
    float2 closest = a;
    if (!is_degenerate) {
      seg_t = math::clamp(math::dot(mval - a, ab) / seg_len_sq, 0.0f, 1.0f);
      closest = a + ab * seg_t;
    }
    const float dist_sq = math::distance_squared(mval, closest);
    if (dist_sq < r_hit.dist_sq) {
      r_hit.dist_sq = dist_sq;
      r_hit.point = closest;
      r_hit.tangent = is_degenerate ? float2(1.0f, 0.0f) : ab / seg_len;
      best_arc = arc_len + seg_t * seg_len;
      best_param = (float(i) + seg_t) / float(polyline.size() - 1);
    }
    arc_len += seg_len;
  }
  r_hit.arc_t = arc_len > 1e-8f ? best_arc / arc_len : 0.0f;
  r_hit.param_t = best_param;
  return true;
}

bool ED_paint_curve_polyline_at_param(const Span<float2> polyline,
                                      const float param_t,
                                      float2 &r_point,
                                      float2 &r_tangent)
{
  if (polyline.size() < 2) {
    return false;
  }

  const float f_index = math::clamp(param_t, 0.0f, 1.0f) * float(polyline.size() - 1);
  const int i0 = min_ii(int(f_index), int(polyline.size()) - 2);
  const float frac = f_index - float(i0);
  const float2 a = polyline[i0];
  const float2 b = polyline[i0 + 1];
  r_point = math::interpolate(a, b, frac);
  const float2 ab = b - a;
  r_tangent = math::length_squared(ab) >= 1e-8f ? math::normalize(ab) : float2(1.0f, 0.0f);
  return true;
}

float ED_paint_curve_polyline_distance_sq(const Span<float2> polyline, const float2 mval)
{
  PaintCurvePolylineHit hit;
  return ED_paint_curve_polyline_closest(polyline, mval, hit) ? hit.dist_sq : FLT_MAX;
}

bool ED_paint_curve_polyline_param_at_closest_point(const Span<float2> polyline,
                                                    const float2 mval,
                                                    float &r_edge_t)
{
  PaintCurvePolylineHit hit;
  if (!ED_paint_curve_polyline_closest(polyline, mval, hit)) {
    return false;
  }
  r_edge_t = hit.arc_t;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gate helpers
 * \{ */

bool ED_paint_curve_is_curves_edit_tool(const char *active_tool_idname)
{
  return active_tool_idname && STREQ(active_tool_idname, PAINT_CURVE_EDIT_TOOL_IDNAME);
}

bool ED_paint_curve_sculpt_tool_display_is_paint(const bContext *C)
{
  const WorkSpace *workspace = CTX_wm_workspace(C);
  return workspace != nullptr && workspace->runtime != nullptr &&
         workspace->runtime->sculpt_paint_tool_display == bke::eSculptPaintToolDisplay::Paint;
}

void ED_paint_curve_sculpt_tool_display_set(bContext *C, const bool is_paint)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  if (workspace == nullptr || workspace->runtime == nullptr) {
    return;
  }
  workspace->runtime->sculpt_paint_tool_display =
      is_paint ? bke::eSculptPaintToolDisplay::Paint : bke::eSculptPaintToolDisplay::Sculpt;
  /* The workspace property's RNA setter sends the same notifier; the toolbar and the asset shelf
   * redraw the switched group from it. */
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
}

bool ED_paint_curve_overlay_is_relevant(const Brush *brush,
                                        const char *active_tool_idname,
                                        bool is_space_v3d,
                                        bool is_space_image)
{
  if (!is_space_v3d && !is_space_image) {
    return false;
  }
  if (brush == nullptr) {
    return false;
  }
  /* A live 2D Curve Patch session keeps its overlay for as long as it exists, independent of the
   * brush state: the user may change brushes mid-session, and the session's frozen parameters
   * mean the curve on screen is still the one being edited. #bke::brush::supports_curve_patch
   * cannot answer this -- it inspects `sculpt_brush_type`, which carries no meaning for an image
   * paint brush. */
  if (is_space_image && image_curve_patch_session_active()) {
    return true;
  }
  if (brush->stroke_method == BRUSH_STROKE_CURVE) {
    return true;
  }
  if (ELEM(brush->stroke_method, BRUSH_STROKE_CURVE_PATCH, BRUSH_STROKE_ROLL) &&
      bke::brush::supports_curve_patch(*brush))
  {
    return true;
  }
  return ED_paint_curve_is_curves_edit_tool(active_tool_idname);
}

PaintCurveOverlayModes ED_paint_curve_overlay_modes_get(const Brush *brush,
                                                        const char *active_tool_idname,
                                                        const bool is_curve_patch_active,
                                                        const bool cursor_valid,
                                                        const bool ctrl_pressed)
{
  PaintCurveOverlayModes modes;
  if (!cursor_valid || paintcurve_slide_is_active()) {
    /* Mid-slide the affordances would fight the drag: the hover would chase the point being
     * moved, and the insert marker would offer to subdivide a segment that is reshaping. A plain
     * Stroke Method: Curve is screen-space and edited in the Image Editor just as legitimately as
     * a 3D one is edited in the viewport, so space is not a gate here -- #ED_paint_curve_overlay_
     * draws_brush_curve already refused to draw an object-space curve outside a View3D region. */
    return modes;
  }

  /* An already-active session keeps its affordances regardless of the brush: the stroke-method
   * gate applies at session entry, not inside a session that was legitimately started. */
  const bool is_curve_patch_stroke = brush && brush->stroke_method == BRUSH_STROKE_CURVE_PATCH &&
                                     bke::brush::supports_curve_patch(*brush);
  const bool is_curve_stroke = (brush && brush->stroke_method == BRUSH_STROKE_CURVE) ||
                               is_curve_patch_stroke || is_curve_patch_active;
  if (!is_curve_stroke && !ED_paint_curve_is_curves_edit_tool(active_tool_idname)) {
    return modes;
  }

  modes.compute_hover = true;
  modes.show_insert_preview = ctrl_pressed;
  return modes;
}

bool ED_paint_curve_overlay_draws_brush_curve(const Brush *brush,
                                              const char *active_tool_idname,
                                              const bool is_space_v3d)
{
  if (brush == nullptr) {
    return false;
  }
  const bool is_curves_edit = ED_paint_curve_is_curves_edit_tool(active_tool_idname);
  const bool is_curve_patch_stroke = brush->stroke_method == BRUSH_STROKE_CURVE_PATCH &&
                                     bke::brush::supports_curve_patch(*brush);
  if (brush->stroke_method != BRUSH_STROKE_CURVE && !is_curve_patch_stroke && !is_curves_edit) {
    return false;
  }
  /* The overlay may draw the brush's paint curve only where the curve's space matches the region's
   * one. Curve Patch and the Curve Edit tool only work with an object-space curve; a plain Stroke
   * Method: Curve carries whichever space it was built in.
   */
  const PaintCurve *paint_curve = brush->paint_curve;
  const bool curve_is_object_space = is_curve_patch_stroke || is_curves_edit ||
                                     (paint_curve != nullptr && paint_curve->use_3d_space);
  if (is_space_v3d) {
    /* Curve Patch and the Curve Edit tool only ever edit an object-space curve, so a screen-space
     * one (left behind by a plain Curve stroke) is not theirs to show: its region pixels would be
     * reinterpreted as object-space coordinates, and Curve Patch never adopts it anyway (see
     * `curve_patch_start_from_anchor()`). The paint curve's own flag decides. A plain Stroke Method:
     * Curve carries whichever space it was built in and is drawn as-is: an object-space curve
     * projects through the region, a screen-space one is painted as a region-pixel overlay exactly
     * as the Image Editor draws it (the builder reads `use_3d_space` and skips projection for it).
     * Refusing the screen-space case here is what made turning off "3D Curve" appear to delete the
     * curve instead of pinning it to the view. */
    if (is_curve_patch_stroke || is_curves_edit) {
      return paint_curve == nullptr || paint_curve->use_3d_space;
    }
    return true;
  }
  /* Outside a viewport there is no #RegionView3D to project an object-space curve through -- and
   * the pixels it would produce would name nothing on this canvas anyway. */
  return !curve_is_object_space;
}

bool ED_paint_curve_slide_is_active()
{
  return paintcurve_slide_is_active();
}

bool ED_paint_curve_snap_marker_get(float r_world_pos[3], int *r_type)
{
  return paintcurve_snap_marker_get(r_world_pos, r_type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ViewContext + Paint resolver
 * \{ */

ViewContext ED_paint_curve_viewcontext_from_state(Depsgraph *depsgraph,
                                                  Scene *scene,
                                                  ViewLayer *view_layer,
                                                  ARegion *region,
                                                  View3D *v3d,
                                                  RegionView3D *rv3d)
{
  ViewContext vc = {};
  vc.C = nullptr;
  vc.bmain = DEG_get_bmain(depsgraph);
  vc.depsgraph = depsgraph;
  vc.scene = scene;
  vc.view_layer = view_layer;
  vc.region = region;
  vc.v3d = v3d;
  vc.rv3d = rv3d;
  vc.obact = BKE_view_layer_active_object_get(view_layer);
  vc.obedit = nullptr;
  vc.em = nullptr;
  vc.win = nullptr;
  vc.mval[0] = vc.mval[1] = 0;
  return vc;
}

Paint *ED_paint_curve_resolve_active_paint(Depsgraph *depsgraph,
                                           Scene *scene,
                                           ViewLayer *view_layer,
                                           const SpaceLink *space_data,
                                           eSpace_Type space_type)
{
  if (scene == nullptr || view_layer == nullptr || scene->toolsettings == nullptr) {
    return nullptr;
  }
  if (space_type == SPACE_IMAGE) {
    const SpaceImage *sima = reinterpret_cast<const SpaceImage *>(space_data);
    if (sima && sima->mode == SI_MODE_PAINT) {
      return &scene->toolsettings->imapaint.paint;
    }
  }
  Main *bmain = DEG_get_bmain(depsgraph);
  if (bmain == nullptr) {
    return nullptr;
  }
  return BKE_paint_get_active(*bmain, scene, view_layer);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen handles provider
 * \{ */

CurvePatchHandleVisibility ED_curve_patch_handle_visibility_get(const Brush *brush)
{
  CurvePatchHandleVisibility visibility;
  if (brush == nullptr) {
    /* No brush, no per-point anything: the plain paint-curve overlay still shows its radius
     * handles through `show_radius_handles`, exactly as before. */
    visibility.radius_regular = true;
    return visibility;
  }
  if (brush->stroke_method != BRUSH_STROKE_CURVE_PATCH ||
      brush->curve_patch.stamp_mode != BRUSH_CURVE_PATCH_STAMP_STAMPS)
  {
    /* Ribbon: radius handles follow the Sculpt display-mode settings, as they always have. */
    visibility.radius_regular = true;
    return visibility;
  }
  if (brush->curve_patch.stamp_layout != BRUSH_CURVE_PATCH_STAMP_LAYOUT_POINTS) {
    /* Stamps + Fill: every per-point handle is hidden -- commit `9030395d`'s behavior. */
    return visibility;
  }
  /* Stamps + POINTS: each set answers to its own brush toggle, and the rotation set exists only
   * while Random Rotation is off -- with it on, the random draw overrides the per-point angle, so
   * a handle for it would edit a value nothing reads. */
  visibility.radius_all_points = brush->curve_patch.show_point_radius_handles != 0;
  visibility.strength = brush->curve_patch.show_point_strength_handles != 0;
  visibility.angle = (brush->mtex.brush_angle_mode & MTEX_ANGLE_RANDOM) == 0;
  return visibility;
}

void ED_paint_curve_screen_handles_build_from_geometry(
    const ViewContext &vc,
    const bke::CurvesGeometry &geometry,
    const bool use_3d_space,
    const Sculpt *sculpt,
    const bool show_radius_handles,
    const float2 mval_region,
    const bool compute_segment_hover,
    const bool show_insert_preview,
    PaintCurveScreenHandles &r_out,
    const CurvePatchHandleVisibility &curve_patch_visibility)
{
  r_out.clear();

  if (!paintcurve_geometry_is_valid(geometry)) {
    return;
  }

  /* An object-space curve can only be projected in a View3D region: the projection bottoms out in
   * `ED_view3d_project_v2`, which reads the region's #RegionView3D, and another editor's region
   * has none. Reachable because this overlay also runs in the Image Editor, where the active
   * object may well own a live 3D patch. Refusing here rather than only at the call sites keeps
   * the next target from rediscovering it as a crash. */
  if (use_3d_space && vc.rv3d == nullptr) {
    return;
  }

  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points_from_geometry(geometry, use_3d_space, &vc, screen_points);
  if (screen_points.is_empty()) {
    return;
  }

  /* Control-point colors (TH_VERTEX / TH_VERTEX_SELECT) are resolved at draw time in the overlay
   * module, so only the wire and radius-handle colors are produced here. */
  float wire_col[4], radius_col[4];
  ui::theme::get_color_type_4fv(TH_WIRE, SPACE_VIEW3D, wire_col);
  ui::theme::get_color_type_4fv(TH_EDGE_SELECT, SPACE_VIEW3D, radius_col);

  int hover_point_index = -1;
  bool radius_handle_hovered = false;
  int64_t hover_segment_index = -1;
  int hover_point_a = -1;
  int hover_point_b = -1;
  int slide_segment_point_a = -1;
  int slide_segment_point_b = -1;
  const bool slide_segment_active = paintcurve_slide_segment_active(&slide_segment_point_a,
                                                                    &slide_segment_point_b);
  int64_t slide_segment_draw_index = -1;
  const float segment_hover_threshold = show_insert_preview ?
                                            PAINT_CURVE_INSERT_SEGMENT_THRESHOLD :
                                            PAINT_CURVE_SEGMENT_HOVER_THRESHOLD;
  float hover_segment_dist_sq = square_f(segment_hover_threshold);
  const bool detect_segment_hover = compute_segment_hover || show_insert_preview;

  /* Point draw data (resolved handles, selection promotion, theme colors) comes from the shared
   * builder. Hover stays below: it has to respect the radius / strength / angle handle priority. */
  control_points::BuildOptions points_options;
  control_points::handles_build(
      ScreenPointsSource(screen_points.as_span()), points_options, r_out);

  if (show_radius_handles || curve_patch_visibility.radius_all_points) {
    float hover_radius_dist = PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS;
    int hover_radius_handle_index = -1;
    for (const int i : screen_points.index_range()) {
      /* The Sculpt display-mode filter only decides the plain paint-curve display (and Ribbon
       * mode's): a POINTS radius handle is shown for every point the brush's own toggle asks
       * for, whatever those settings say. */
      if (!curve_patch_visibility.radius_all_points &&
          !should_show_radius_handle_draw_from_geometry(sculpt, geometry, screen_points, i))
      {
        continue;
      }
      PaintCurveRadiusHandleScreen handle_screen;
      paintcurve_radius_handle_screen_get_from_geometry(
          geometry, screen_points.data(), i, &handle_screen);
      PaintCurveRadiusHandleDrawData rd;
      rd.point = handle_screen.point;
      rd.end = handle_screen.end;
      rd.perp = handle_screen.perp;
      copy_v4_v4(rd.color, radius_col);
      if (compute_segment_hover) {
        const float mval[2] = {mval_region.x, mval_region.y};
        const float end[2] = {rd.end.x, rd.end.y};
        const float dist = len_v2v2(mval, end);
        if (dist < hover_radius_dist) {
          hover_radius_dist = dist;
          hover_radius_handle_index = r_out.radius_handles.size();
        }
      }
      r_out.radius_handles.append(rd);
    }
    if (hover_radius_handle_index >= 0) {
      r_out.radius_handles[hover_radius_handle_index].hovered = true;
      radius_handle_hovered = true;
    }
  }

  /* Per-point strength handles: a fixed-length track opposite the radius handle with a knob at
   * the point's current value. Curve Patch POINTS only -- the plain paint-curve callers pass an
   * all-false visibility and never reach this. */
  bool strength_handle_hovered = false;
  if (curve_patch_visibility.strength) {
    float hover_strength_dist = PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS;
    int hover_strength_index = -1;
    for (const int i : screen_points.index_range()) {
      PaintCurveStrengthHandleScreen handle_screen;
      paintcurve_strength_handle_screen_get_from_geometry(
          geometry, screen_points.data(), i, &handle_screen);
      PaintCurveStrengthHandleDrawData sd;
      sd.point = handle_screen.point;
      sd.base = handle_screen.base;
      sd.end = handle_screen.end;
      sd.knob = handle_screen.knob;
      sd.value = handle_screen.value;
      ui::theme::get_color_type_4fv(TH_GIZMO_B, SPACE_VIEW3D, sd.color);
      /* Only the nearest knob highlights, and only when no radius handle already owns the
       * highlight -- the same radius -> strength -> angle priority the press hit test uses. */
      if (compute_segment_hover && !radius_handle_hovered) {
        const float mval[2] = {mval_region.x, mval_region.y};
        const float knob[2] = {sd.knob.x, sd.knob.y};
        const float dist = len_v2v2(mval, knob);
        if (dist < hover_strength_dist) {
          hover_strength_dist = dist;
          hover_strength_index = int(r_out.strength_handles.size());
        }
      }
      r_out.strength_handles.append(sd);
    }
    if (hover_strength_index >= 0) {
      r_out.strength_handles[hover_strength_index].hovered = true;
      strength_handle_hovered = true;
    }
  }

  /* Per-point rotation handles: an arc around the point, centered on the stamp's current screen
   * angle, with the knob on the arc. Same POINTS-only gate as the strength handles above. */
  if (curve_patch_visibility.angle) {
    float hover_angle_dist = PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS;
    int hover_angle_index = -1;
    for (const int i : screen_points.index_range()) {
      PaintCurveAngleHandleScreen handle_screen;
      paintcurve_angle_handle_screen_get_from_geometry(
          geometry, screen_points.data(), i, &handle_screen);
      PaintCurveAngleHandleDrawData ad;
      ad.center = handle_screen.center;
      ad.radius = handle_screen.radius;
      ad.angle = handle_screen.angle;
      ad.knob = handle_screen.knob;
      ui::theme::get_color_type_4fv(TH_GIZMO_PRIMARY, SPACE_VIEW3D, ad.color);
      /* Same nearest-knob rule as strength, below radius in the priority order. */
      if (compute_segment_hover && !radius_handle_hovered && !strength_handle_hovered) {
        const float mval[2] = {mval_region.x, mval_region.y};
        const float knob[2] = {ad.knob.x, ad.knob.y};
        const float dist = len_v2v2(mval, knob);
        if (dist < hover_angle_dist) {
          hover_angle_dist = dist;
          hover_angle_index = int(r_out.angle_handles.size());
        }
      }
      r_out.angle_handles.append(ad);
    }
    if (hover_angle_index >= 0) {
      r_out.angle_handles[hover_angle_index].hovered = true;
    }
  }

  paintcurve_foreach_bezier_segment_from_geometry(
      geometry, [&](const int point_a, const int point_b) {
        PaintCurveSegmentDrawData seg;
        paintcurve_build_screen_segment_polyline_from_geometry(
            geometry, use_3d_space, &vc, point_a, point_b, screen_points, seg.polyline);
        if (seg.polyline.size() < 2) {
          return;
        }
        copy_v4_v4(seg.wire_color, wire_col);
        seg.outline_color = float4(0.0f, 0.0f, 0.0f, 0.5f);
        if (detect_segment_hover) {
          const float dist_sq = ED_paint_curve_polyline_distance_sq(seg.polyline, mval_region);
          if (dist_sq < hover_segment_dist_sq) {
            hover_segment_dist_sq = dist_sq;
            hover_segment_index = r_out.segments.size();
            hover_point_a = point_a;
            hover_point_b = point_b;
          }
        }
        if (slide_segment_active && point_a == slide_segment_point_a &&
            point_b == slide_segment_point_b)
        {
          slide_segment_draw_index = r_out.segments.size();
        }
        r_out.segments.append(std::move(seg));
      });

  if (compute_segment_hover) {
    const float mval[2] = {mval_region.x, mval_region.y};
    char point_selflag = 0;
    hover_point_index = paintcurve_find_in_screen_points(
        screen_points.as_span(), mval, false, PAINT_CURVE_HOVER_THRESHOLD, &point_selflag);
    if (hover_point_index >= 0 && point_selflag == SEL_F2 && !radius_handle_hovered) {
      r_out.points[hover_point_index].hovered_center = true;
    }
  }

  if (hover_segment_index >= 0 && compute_segment_hover && hover_point_index < 0 &&
      !radius_handle_hovered)
  {
    r_out.segments[hover_segment_index].hovered = true;
  }
  else if (slide_segment_draw_index >= 0) {
    r_out.segments[slide_segment_draw_index].hovered = true;
  }

  if (hover_segment_index >= 0 && show_insert_preview) {
    const PaintCurveSegmentDrawData &hover_seg = r_out.segments[hover_segment_index];
    const float mval[2] = {mval_region.x, mval_region.y};
    float bezier_t = 0.0f;
    PaintCurveInsertPreviewDrawData preview;
    if (hover_point_a >= 0 && hover_point_b >= 0 &&
        paintcurve_bezier_param_at_screen_pos_on_segment_from_geometry(&vc,
                                                                       geometry,
                                                                       use_3d_space,
                                                                       mval,
                                                                       hover_point_a,
                                                                       hover_point_b,
                                                                       screen_points,
                                                                       bezier_t) &&
        ED_paint_curve_polyline_at_param(
            hover_seg.polyline, bezier_t, preview.point, preview.tangent))
    {
      /* Match #paintcurve_find_insert_segment_from_geometry: do not preview subdivision near
       * endpoints. */
      if (bezier_t >= PAINT_CURVE_INSERT_T_MIN && bezier_t <= PAINT_CURVE_INSERT_T_MAX) {
        if (math::length_squared(preview.tangent) < 1e-6f) {
          preview.tangent = float2(1.0f, 0.0f);
        }
        else {
          preview.tangent = math::normalize(preview.tangent);
        }
        preview.perp = float2(-preview.tangent.y, preview.tangent.x);
        preview.valid = true;
        r_out.insert_preview = preview;
      }
    }
  }
}

void ED_paint_curve_screen_handles_build(const ViewContext &vc,
                                         const Brush &brush,
                                         const Sculpt *sculpt,
                                         const float2 mval_region,
                                         const bool compute_segment_hover,
                                         const bool show_insert_preview,
                                         PaintCurveScreenHandles &r_out)
{
  PaintCurve *pc = brush.paint_curve;
  if (pc == nullptr) {
    r_out.clear();
    return;
  }
  ED_paint_curve_screen_handles_build_from_geometry(vc,
                                                    pc->geometry.wrap(),
                                                    pc->use_3d_space != 0,
                                                    sculpt,
                                                    pc->show_radius_handles != 0,
                                                    mval_region,
                                                    compute_segment_hover,
                                                    show_insert_preview,
                                                    r_out);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Silhouettes provider (non-cached)
 * \{ */

void ED_paint_curve_screen_silhouettes_build(const ViewContext &vc,
                                             const float2 mval_region,
                                             const Object *source_object,
                                             const bool compute_hover,
                                             PaintCurveScreenSilhouettes &r_out)
{
  r_out.faint_objects.clear();
  r_out.hover.clear();
  r_out.hover_object = nullptr;

  if (vc.scene == nullptr || vc.view_layer == nullptr || vc.bmain == nullptr) {
    return;
  }

  /* Hover detection. */
  const Object *hover_ob = nullptr;
  Vector<Vector<float2>> hover_polylines;
  if (compute_hover && !paintcurve_slide_is_active()) {
    hover_ob = paintcurve_nearest_scene_curve(
        &vc, mval_region, PAINT_CURVE_HOVER_THRESHOLD, source_object, &hover_polylines);
  }
  r_out.hover_object = hover_ob;
  if (hover_ob) {
    r_out.hover = std::move(hover_polylines);
  }

  /* Faint pass: every scene curve except source and hovered. */
  BKE_view_layer_synced_ensure(*vc.bmain, vc.scene, vc.view_layer);
  for (Base &base : *BKE_view_layer_object_bases_get(vc.view_layer)) {
    Object *ob = base.object;
    if (ob == source_object || ob == hover_ob || !ELEM(ob->type, OB_CURVES, OB_CURVES_LEGACY)) {
      continue;
    }
    if ((base.flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) == 0) {
      continue;
    }
    PaintCurveCachedObjectSilhouette entry;
    entry.object = ob;
    paintcurve_object_screen_polylines(&vc, ob, entry.polylines);
    if (!entry.polylines.is_empty()) {
      r_out.faint_objects.append(std::move(entry));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Silhouettes provider (cached)
 * \{ */

/** Small FNV-1a 64-bit mixer shared by the silhouette cache keys. */
struct PaintCurveCacheHasher {
  uint64_t value = 14695981039346656037ULL;

  void mix_u32(const uint32_t v)
  {
    value ^= uint64_t(v);
    value *= 1099511628211ULL;
  }
  void mix_float(const float v)
  {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    mix_u32(bits);
  }
  void mix_floats(const Span<float> values)
  {
    for (const float v : values) {
      mix_float(v);
    }
  }
};

uint64_t ED_paint_curve_silhouette_cache_key_hash(const ViewContext &vc)
{
  PaintCurveCacheHasher hash;

  if (vc.rv3d) {
    hash.mix_floats(Span<float>(&vc.rv3d->persmat[0][0], 16));
  }
  if (vc.region) {
    hash.mix_u32(uint32_t(vc.region->winx));
    hash.mix_u32(uint32_t(vc.region->winy));
  }
  if (vc.view_layer && vc.bmain && vc.scene) {
    BKE_view_layer_synced_ensure(*vc.bmain, vc.scene, vc.view_layer);
    for (Base &base : *BKE_view_layer_object_bases_get(vc.view_layer)) {
      Object *ob = base.object;
      if (!ELEM(ob->type, OB_CURVES, OB_CURVES_LEGACY)) {
        continue;
      }
      hash.mix_u32(ob->id.session_uid);
      /* Hash geometry so edits (position changes, point add/remove) are detected.
       * Without this the silhouette cache returns stale polylines after a sync. */
      if (ob->type == OB_CURVES) {
        const Curves *curves_id = id_cast<const Curves *>(ob->data);
        const bke::CurvesGeometry &geom = curves_id->geometry.wrap();
        const int point_num = geom.points_num();
        hash.mix_u32(uint32_t(point_num));
        /* Sample positions to detect moves. Cap at 128 to stay cheap for hair. */
        if (point_num > 0) {
          const Span<float3> positions = geom.positions();
          const int sample = min_ii(point_num, 128);
          for (const int i : IndexRange(sample)) {
            hash.mix_floats(Span<float>(&positions[i].x, 3));
          }
        }
      }
      else { /* OB_CURVES_LEGACY */
        const Curve *cu = id_cast<const Curve *>(ob->data);
        const ListBaseT<Nurb> *nurb_lb = BKE_curve_nurbs_get_for_read(cu);
        int nurb_count = 0;
        for (const Nurb &nu : *nurb_lb) {
          hash.mix_u32(uint32_t(nu.pntsu));
          hash.mix_u32(uint32_t(nu.pntsv));
          nurb_count++;
        }
        hash.mix_u32(uint32_t(nurb_count));
      }
    }
  }

  return hash.value;
}

void ED_paint_curve_screen_silhouettes_build_cached(
    const ViewContext &vc,
    const float2 mval_region,
    const Object *source_object,
    const bool compute_hover,
    const uint64_t cache_key,
    Vector<PaintCurveCachedObjectSilhouette> &r_cache_store,
    uint64_t &r_cache_store_key,
    PaintCurveScreenSilhouettes &r_out)
{
  r_out.faint_objects.clear();
  r_out.hover.clear();
  r_out.hover_object = nullptr;

  if (vc.scene == nullptr || vc.view_layer == nullptr || vc.bmain == nullptr) {
    return;
  }

  /* Rebuild projection cache when scene or view has changed. */
  if (cache_key != r_cache_store_key) {
    r_cache_store.clear();
    BKE_view_layer_synced_ensure(*vc.bmain, vc.scene, vc.view_layer);
    for (Base &base : *BKE_view_layer_object_bases_get(vc.view_layer)) {
      Object *ob = base.object;
      if (!ELEM(ob->type, OB_CURVES, OB_CURVES_LEGACY)) {
        continue;
      }
      if ((base.flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) == 0) {
        continue;
      }
      PaintCurveCachedObjectSilhouette entry;
      entry.object = ob;
      paintcurve_object_screen_polylines(&vc, ob, entry.polylines);
      if (!entry.polylines.is_empty()) {
        r_cache_store.append(std::move(entry));
      }
    }
    r_cache_store_key = cache_key;
  }

  /* Hover detection: distance check over cached polylines. */
  const Object *hover_ob = nullptr;
  if (compute_hover && !paintcurve_slide_is_active()) {
    Vector<Vector<float2>> hover_polylines;
    hover_ob = paintcurve_nearest_from_cached_polylines(
        mval_region, PAINT_CURVE_HOVER_THRESHOLD, source_object, r_cache_store, &hover_polylines);
    r_out.hover_object = hover_ob;
    if (hover_ob) {
      r_out.hover = std::move(hover_polylines);
    }
  }

  /* Populate faint objects (all cached except source and hovered). */
  for (const PaintCurveCachedObjectSilhouette &entry : r_cache_store) {
    if (entry.object == source_object || entry.object == hover_ob) {
      continue;
    }
    r_out.faint_objects.append(entry);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve Patch stack silhouettes (Curve Edit tool, Paint display group)
 * \{ */

/** The screen polylines of one control curve, one merged strip per curve: every Bezier segment
 * tessellated exactly the way the handle overlay and the pick draw it, consecutive segments
 * joined so a line strip per curve suffices. */
static void paintcurve_stack_curve_screen_polylines(const ViewContext &vc,
                                                    const bke::CurvesGeometry &geom,
                                                    Vector<Vector<float2>> &r_polylines)
{
  r_polylines.clear();
  if (!paintcurve_geometry_is_valid(geom)) {
    return;
  }
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);
  if (screen_points.size() < 2) {
    return;
  }
  Vector<float2> strip;
  for (const int point_a : IndexRange(screen_points.size() - 1)) {
    Vector<float2> segment;
    paintcurve_build_screen_segment_polyline_from_geometry(
        geom, true, &vc, point_a, point_a + 1, screen_points.as_span(), segment);
    if (segment.size() < 2) {
      continue;
    }
    if (!strip.is_empty() && strip.last() == segment[0]) {
      strip.extend(segment.as_span().drop_front(1));
    }
    else {
      if (!strip.is_empty()) {
        r_polylines.append(std::move(strip));
        strip = {};
      }
      strip = segment;
    }
  }
  if (!strip.is_empty()) {
    r_polylines.append(std::move(strip));
  }
}

/** The Curve Patch effects of every SELECTED sculpt-mode object's active material, in view-layer
 * order, each as its screen polylines projected through its own object's transform. Solo
 * (`PAINT_VECTOR_SHOW_ALL_CURVES`) is honored per effect, and a switched-off effect is skipped --
 * the same set the pick offers. */
static void paintcurve_stack_silhouettes_collect(const ViewContext &vc,
                                                 Vector<PaintCurveStackSilhouette> &r_out)
{
  r_out.clear();

  const MaterialPaintLayer *current_effect = nullptr;
  PaintCurveStackSilhouette entry;
  const auto flush_effect = [&]() {
    if (!entry.polylines.is_empty()) {
      r_out.append(std::move(entry));
    }
    entry = {};
  };

  paintcurve_stack_foreach_curve_patch_item(
      vc,
      [&](Object & /*ob*/,
          Material & /*material*/,
          MaterialPaintLayer &effect,
          const int /*item_index*/,
          const bke::CurvesGeometry &geom,
          const ViewContext &ob_vc) {
        if (&effect != current_effect) {
          flush_effect();
          current_effect = &effect;
        }
        Vector<Vector<float2>> polylines;
        paintcurve_stack_curve_screen_polylines(ob_vc, geom, polylines);
        for (Vector<float2> &polyline : polylines) {
          entry.polylines.append(std::move(polyline));
        }
      });
  flush_effect();
}

uint64_t ED_paint_curve_stack_silhouette_cache_key_hash(const ViewContext &vc)
{
  PaintCurveCacheHasher hash;

  if (vc.rv3d) {
    hash.mix_floats(Span<float>(&vc.rv3d->persmat[0][0], 16));
  }
  if (vc.region) {
    hash.mix_u32(uint32_t(vc.region->winx));
    hash.mix_u32(uint32_t(vc.region->winy));
  }

  paintcurve_stack_foreach_curve_patch_item(
      vc,
      [&](Object &ob,
          Material &material,
          MaterialPaintLayer &effect,
          const int item_index,
          const bke::CurvesGeometry &geom,
          const ViewContext &ob_vc) {
        /* The selection itself reaches the key: selecting another object changes what is drawn. */
        hash.mix_u32(uint32_t(ob.id.session_uid));
        const float4x4 ob_to_world = ob_vc.obact->object_to_world();
        hash.mix_floats(Span<float>(ob_to_world.ptr()[0], 16));
        hash.mix_u32(uint32_t(material.id.session_uid));
        hash.mix_u32(uint32_t(ob.actcol));

        /* Solo and the active item decide WHICH curves are drawn, the row flag WHETHER they are:
         * a visibility toggle has to reach the key or the cache would keep drawing a switched-off
         * effect. */
        hash.mix_u32(UUID(effect.marker).hash());
        hash.mix_u32(uint32_t(effect.flag));
        const PaintVector &pv = *effect.curve_patch->vectors;
        hash.mix_u32(uint32_t(pv.flag));
        hash.mix_u32(uint32_t(pv.active_item));
        hash.mix_u32(uint32_t(item_index));

        if (!paintcurve_geometry_is_valid(geom)) {
          hash.mix_u32(0);
          return;
        }
        hash.mix_u32(uint32_t(geom.curves_num()));
        const Span<float3> positions = geom.positions();
        hash.mix_u32(uint32_t(positions.size()));
        /* Every position and handle: editing any control point has to invalidate the cache, and
         * a fixed-size sample would miss edits past it. */
        for (const float3 &position : positions) {
          hash.mix_floats(Span<float>(&position.x, 3));
        }
        const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
        const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
        if (handles_left.has_value()) {
          for (const float3 &handle : *handles_left) {
            hash.mix_floats(Span<float>(&handle.x, 3));
          }
        }
        if (handles_right.has_value()) {
          for (const float3 &handle : *handles_right) {
            hash.mix_floats(Span<float>(&handle.x, 3));
          }
        }
        /* Handle types pick the tessellation (vector segments draw as one edge), and the
         * cyclic/resolution arrays decide how many evaluated points a segment has. */
        const VArray<int8_t> handle_types_left = geom.handle_types_left();
        const VArray<int8_t> handle_types_right = geom.handle_types_right();
        const VArray<bool> cyclic = geom.cyclic();
        const VArray<int> resolution = geom.resolution();
        for (const int i : IndexRange(geom.points_num())) {
          hash.mix_u32(uint32_t(handle_types_left[i]));
          hash.mix_u32(uint32_t(handle_types_right[i]));
        }
        for (const int i : geom.curves_range()) {
          hash.mix_u32(uint32_t(cyclic[i]));
          hash.mix_u32(uint32_t(resolution[i]));
        }
      });

  return hash.value;
}

void ED_paint_curve_stack_silhouettes_build_cached(
    const ViewContext &vc,
    const std::optional<float2> hover_mval,
    const uint64_t cache_key,
    Vector<PaintCurveStackSilhouette> &r_cache_store,
    uint64_t &r_cache_store_key,
    Vector<int> &r_faint_indices,
    int &r_hover_index)
{
  r_faint_indices.clear();
  r_hover_index = -1;

  /* Rebuild the projection cache when the selection's effects or the view have changed. */
  if (cache_key != r_cache_store_key) {
    paintcurve_stack_silhouettes_collect(vc, r_cache_store);
    r_cache_store_key = cache_key;
  }

  /* Hover detection: the effect holding the nearest polyline within the threshold. */
  if (hover_mval.has_value() && !paintcurve_slide_is_active() && !r_cache_store.is_empty()) {
    int best_entry = -1;
    float best_dist_sq = square_f(PAINT_CURVE_HOVER_THRESHOLD);
    for (const int entry_i : r_cache_store.index_range()) {
      for (const Vector<float2> &polyline : r_cache_store[entry_i].polylines) {
        PaintCurvePolylineHit hit;
        if (ED_paint_curve_polyline_closest(polyline, *hover_mval, hit) &&
            hit.dist_sq < best_dist_sq)
        {
          best_dist_sq = hit.dist_sq;
          best_entry = entry_i;
        }
      }
    }
    r_hover_index = best_entry;
  }

  for (const int entry_i : r_cache_store.index_range()) {
    if (entry_i != r_hover_index) {
      r_faint_indices.append(entry_i);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Redraw poll
 * \{ */

bool ED_paint_curve_overlay_wants_redraw(const bContext *C)
{
  if (C == nullptr) {
    return false;
  }
  ScrArea *area = CTX_wm_area(C);
  const ARegion *region = CTX_wm_region(C);
  if (area == nullptr || region == nullptr || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  if (!ELEM(area->spacetype, SPACE_VIEW3D, SPACE_IMAGE)) {
    return false;
  }
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  /* Use WM_toolsystem_key_from_context + WM_toolsystem_ref_find instead of
   * WM_toolsystem_ref_from_context to avoid BLI_assert(tref == area->runtime.tool) which fires
   * when the active object changes (e.g. after creating a Curves object from selection) but the
   * tool system has not yet been refreshed to reflect the new mode. */
  const bToolRef *tref = nullptr;
  {
    Main *bmain = CTX_data_main(C);
    const Scene *scene = CTX_data_scene(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    WorkSpace *workspace = CTX_wm_workspace(C);
    if (workspace && bmain && scene) {
      bToolKey tkey{};
      if (WM_toolsystem_key_from_context(*bmain, scene, view_layer, area, &tkey)) {
        tref = WM_toolsystem_ref_find(workspace, &tkey);
      }
    }
  }
  const char *tool_id = tref ? tref->idname : nullptr;
  return ED_paint_curve_overlay_is_relevant(
      brush, tool_id, area->spacetype == SPACE_VIEW3D, area->spacetype == SPACE_IMAGE);
}

void ED_paint_curve_overlay_tag_redraw_all(bContext *C)
{
  if (!ED_paint_curve_overlay_wants_redraw(C)) {
    return;
  }

  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return;
  }

  /* #SPACE_IMAGE alongside #SPACE_VIEW3D is this overlay's own legacy scope (Paint Curve also
   * draws over the Image Editor for 2D texture-paint curves, see
   * #ED_paint_curve_overlay_is_relevant's `is_space_image` parameter) -- not a general property
   * of the redraw-broadcast helper, which any caller can point at whichever spacetypes it needs.
   * A `std::array`, not a bare C array: #blender::Span has an implicit constructor from
   * `std::array` but not from a raw `T[N]` (only from a `(pointer, size)` pair). */
  const std::array<int, 2> space_types = {SPACE_VIEW3D, SPACE_IMAGE};
  ED_screen_tag_redraw_spacetype_all_windows(wm, space_types, RGN_TYPE_WINDOW);
  for (wmWindow &win : wm->windows) {
    WM_paint_cursor_tag_redraw(&win, nullptr);
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
