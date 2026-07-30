/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Public provider API for paint-curve overlay drawing.
 * Implements the functions declared in ED_paint_curve_draw.hh.
 */

#include "ED_paint_curve_draw.hh"

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_workspace_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curves.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_utildefines.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Anonymous helpers (theme colors, radius gate)
 * \{ */

namespace {

static void paintcurve_theme_handle_color_draw(const int8_t handle_type,
                                               const bool selected,
                                               float r_col[4])
{
  int color;
  if (selected) {
    switch (handle_type) {
      case BEZIER_HANDLE_AUTO:
        color = TH_HANDLE_SEL_AUTO;
        break;
      case BEZIER_HANDLE_VECTOR:
        color = TH_HANDLE_SEL_VECT;
        break;
      case BEZIER_HANDLE_ALIGN:
        color = TH_HANDLE_SEL_ALIGN;
        break;
      default:
        color = TH_HANDLE_SEL_FREE;
        break;
    }
  }
  else {
    switch (handle_type) {
      case BEZIER_HANDLE_AUTO:
        color = TH_HANDLE_AUTO;
        break;
      case BEZIER_HANDLE_VECTOR:
        color = TH_HANDLE_VECT;
        break;
      case BEZIER_HANDLE_ALIGN:
        color = TH_HANDLE_ALIGN;
        break;
      default:
        color = TH_HANDLE_FREE;
        break;
    }
  }
  ui::theme::get_color_type_4fv(color, SPACE_VIEW3D, r_col);
}

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

static float paintcurve_polyline_distance_sq(const Span<float2> polyline, const float2 mval)
{
  if (polyline.size() < 2) {
    return FLT_MAX;
  }

  float min_dist_sq = FLT_MAX;
  for (const int i : IndexRange(polyline.size() - 1)) {
    const float seg_dist_sq = dist_squared_to_line_segment_v2(mval, polyline[i], polyline[i + 1]);
    min_dist_sq = min_ff(min_dist_sq, seg_dist_sq);
  }
  return min_dist_sq;
}

static bool paintcurve_polyline_point_and_tangent_at_bezier_param(const Span<float2> polyline,
                                                                const float bezier_t,
                                                                float2 &r_point,
                                                                float2 &r_tangent)
{
  if (polyline.size() < 2) {
    return false;
  }

  const float f_index = math::clamp(bezier_t, 0.0f, 1.0f) * float(polyline.size() - 1);
  const int i0 = min_ii(int(f_index), int(polyline.size()) - 2);
  const float frac = f_index - float(i0);
  const float2 a = polyline[i0];
  const float2 b = polyline[i0 + 1];
  r_point = math::interpolate(a, b, frac);
  const float2 ab = b - a;
  r_tangent = math::length_squared(ab) >= 1e-8f ? math::normalize(ab) : float2(1.0f, 0.0f);
  return true;
}

static bool paintcurve_polyline_closest_point_and_tangent(const Span<float2> polyline,
                                                          const float2 mval,
                                                          float2 &r_point,
                                                          float2 &r_tangent,
                                                          float &r_edge_t)
{
  if (polyline.size() < 2) {
    return false;
  }

  float best_dist_sq = FLT_MAX;
  float2 best_point = polyline[0];
  float2 best_tangent = float2(1.0f, 0.0f);
  float best_arc = 0.0f;

  float arc_len = 0.0f;
  for (const int i : IndexRange(polyline.size() - 1)) {
    const float2 a = polyline[i];
    const float2 b = polyline[i + 1];
    const float2 ab = b - a;
    const float seg_len_sq = math::length_squared(ab);
    float seg_t = 0.0f;
    float2 closest = a;
    if (seg_len_sq >= 1e-8f) {
      seg_t = math::clamp(math::dot(mval - a, ab) / seg_len_sq, 0.0f, 1.0f);
      closest = a + ab * seg_t;
    }
    const float dist_sq = math::distance_squared(mval, closest);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best_point = closest;
      best_tangent = seg_len_sq >= 1e-8f ? math::normalize(ab) : float2(1.0f, 0.0f);
      best_arc = arc_len + seg_t * sqrtf(seg_len_sq);
    }
    arc_len += sqrtf(max_ff(seg_len_sq, 0.0f));
  }

  r_point = best_point;
  r_tangent = best_tangent;
  r_edge_t = arc_len > 1e-8f ? best_arc / arc_len : 0.0f;
  return true;
}

}  // anonymous namespace

float ED_paint_curve_polyline_distance_sq(const Span<float2> polyline, const float2 mval)
{
  return paintcurve_polyline_distance_sq(polyline, mval);
}

bool ED_paint_curve_polyline_param_at_closest_point(const Span<float2> polyline,
                                                    const float2 mval,
                                                    float &r_edge_t)
{
  float2 point;
  float2 tangent;
  return paintcurve_polyline_closest_point_and_tangent(polyline, mval, point, tangent, r_edge_t);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gate helpers
 * \{ */

bool ED_paint_curve_is_curves_edit_tool(const char *active_tool_idname)
{
  return active_tool_idname && STREQ(active_tool_idname, "builtin.curves_edit");
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

bool ED_paint_curve_slide_is_active()
{
  return paintcurve_slide_is_active();
}

bool ED_paint_curve_snap_marker_get(float r_screen[2], int *r_type)
{
  return paintcurve_snap_marker_get(r_screen, r_type);
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

void ED_paint_curve_screen_handles_build_from_geometry(const ViewContext &vc,
                                                        const bke::CurvesGeometry &geometry,
                                                        const bool use_3d_space,
                                                        const Sculpt *sculpt,
                                                        const bool show_radius_handles,
                                                        const float2 mval_region,
                                                        const bool compute_segment_hover,
                                                        const bool show_insert_preview,
                                                        PaintCurveScreenHandles &r_out)
{
  r_out.points.clear();
  r_out.radius_handles.clear();
  r_out.segments.clear();
  r_out.insert_preview = {};

  if (!paintcurve_geometry_is_valid(geometry)) {
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

  int64_t hover_segment_index = -1;
  int hover_point_a = -1;
  int hover_point_b = -1;
  int slide_segment_point_a = -1;
  int slide_segment_point_b = -1;
  const bool slide_segment_active = paintcurve_slide_segment_active(&slide_segment_point_a,
                                                                   &slide_segment_point_b);
  int64_t slide_segment_draw_index = -1;
  const float segment_hover_threshold = show_insert_preview ? PAINT_CURVE_INSERT_SEGMENT_THRESHOLD :
                                                              PAINT_CURVE_SEGMENT_HOVER_THRESHOLD;
  float hover_segment_dist_sq = square_f(segment_hover_threshold);
  const bool detect_segment_hover = compute_segment_hover || show_insert_preview;

  for (const int i : screen_points.index_range()) {
    const PaintCurvePoint &pcp = screen_points[i];
    PaintCurveHandleDrawData hd;
    hd.position = float2(pcp.bez.vec[1][0], pcp.bez.vec[1][1]);
    hd.handle_left = float2(pcp.bez.vec[0][0], pcp.bez.vec[0][1]);
    hd.handle_right = float2(pcp.bez.vec[2][0], pcp.bez.vec[2][1]);
    hd.h1 = int8_t(pcp.bez.h1);
    hd.h2 = int8_t(pcp.bez.h2);
    hd.selected_center = (pcp.bez.f2 != 0);
    hd.selected_left = (pcp.bez.f1 != 0) || hd.selected_center;
    hd.selected_right = (pcp.bez.f3 != 0) || hd.selected_center;
    float lc[4], rc[4];
    paintcurve_theme_handle_color_draw(hd.h1, hd.selected_left, lc);
    paintcurve_theme_handle_color_draw(hd.h2, hd.selected_right, rc);
    copy_v4_v4(hd.color_left, lc);
    copy_v4_v4(hd.color_right, rc);
    r_out.points.append(hd);
  }

  if (show_radius_handles) {
    for (const int i : screen_points.index_range()) {
      if (!should_show_radius_handle_draw_from_geometry(sculpt, geometry, screen_points, i)) {
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
      r_out.radius_handles.append(rd);
    }
  }

  paintcurve_foreach_bezier_segment_from_geometry(geometry, [&](const int point_a, const int point_b) {
    PaintCurveSegmentDrawData seg;
    paintcurve_build_screen_segment_polyline_from_geometry(geometry,
                                                           use_3d_space,
                                                           &vc,
                                                           point_a,
                                                           point_b,
                                                           screen_points,
                                                           seg.polyline);
    if (seg.polyline.size() < 2) {
      return;
    }
    copy_v4_v4(seg.wire_color, wire_col);
    seg.outline_color = float4(0.0f, 0.0f, 0.0f, 0.5f);
    if (detect_segment_hover) {
      const float dist_sq = paintcurve_polyline_distance_sq(seg.polyline, mval_region);
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

  if (hover_segment_index >= 0 && compute_segment_hover) {
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
        paintcurve_bezier_param_at_screen_pos_on_segment_from_geometry(
            &vc, geometry, use_3d_space, mval, hover_point_a, hover_point_b, screen_points, bezier_t) &&
        paintcurve_polyline_point_and_tangent_at_bezier_param(
            hover_seg.polyline, bezier_t, preview.point, preview.tangent))
    {
      /* Match insert_or_add_point: do not preview subdivision near endpoints. */
      if (bezier_t >= 0.1f && bezier_t <= 0.9f) {
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
    r_out.points.clear();
    r_out.radius_handles.clear();
    r_out.segments.clear();
    r_out.insert_preview = {};
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

uint64_t ED_paint_curve_silhouette_cache_key_hash(const ViewContext &vc)
{
  /* FNV-1a 64-bit. */
  uint64_t h = 14695981039346656037ULL;
  const uint64_t prime = 1099511628211ULL;

  auto mix_u32 = [&](uint32_t v) {
    h ^= uint64_t(v);
    h *= prime;
  };

  if (vc.rv3d) {
    const float *m = &vc.rv3d->persmat[0][0];
    for (int i = 0; i < 16; i++) {
      uint32_t bits;
      memcpy(&bits, &m[i], sizeof(bits));
      mix_u32(bits);
    }
  }
  if (vc.region) {
    mix_u32(uint32_t(vc.region->winx));
    mix_u32(uint32_t(vc.region->winy));
  }
  if (vc.view_layer && vc.bmain && vc.scene) {
    BKE_view_layer_synced_ensure(*vc.bmain, vc.scene, vc.view_layer);
    for (Base &base : *BKE_view_layer_object_bases_get(vc.view_layer)) {
      Object *ob = base.object;
      if (!ELEM(ob->type, OB_CURVES, OB_CURVES_LEGACY)) {
        continue;
      }
      mix_u32(ob->id.session_uid);
      /* Hash geometry so edits (position changes, point add/remove) are detected.
       * Without this the silhouette cache returns stale polylines after a sync. */
      if (ob->type == OB_CURVES) {
        const Curves *curves_id = id_cast<const Curves *>(ob->data);
        const bke::CurvesGeometry &geom = curves_id->geometry.wrap();
        const int point_num = geom.points_num();
        mix_u32(uint32_t(point_num));
        /* Sample positions to detect moves. Cap at 128 to stay cheap for hair. */
        if (point_num > 0) {
          const Span<float3> positions = geom.positions();
          const int sample = min_ii(point_num, 128);
          for (const int i : IndexRange(sample)) {
            uint32_t bits;
            memcpy(&bits, &positions[i].x, sizeof(bits));
            mix_u32(bits);
            memcpy(&bits, &positions[i].y, sizeof(bits));
            mix_u32(bits);
            memcpy(&bits, &positions[i].z, sizeof(bits));
            mix_u32(bits);
          }
        }
      }
      else { /* OB_CURVES_LEGACY */
        const Curve *cu = id_cast<const Curve *>(ob->data);
        const ListBaseT<Nurb> *nurb_lb = BKE_curve_nurbs_get_for_read(cu);
        int nurb_count = 0;
        for (const Nurb &nu : *nurb_lb) {
          mix_u32(uint32_t(nu.pntsu));
          mix_u32(uint32_t(nu.pntsv));
          nurb_count++;
        }
        mix_u32(uint32_t(nurb_count));
      }
    }
  }

  return h;
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
/** \name Redraw poll (Unit D)
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

/** \} */

}  // namespace blender::ed::sculpt_paint
