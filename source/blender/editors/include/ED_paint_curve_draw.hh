/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DNA_space_enums.h"

namespace blender {
struct ARegion;
struct Brush;
struct Depsgraph;
struct Object;
struct Paint;
struct RegionView3D;
struct Scene;
struct Sculpt;
struct SpaceLink;
struct View3D;
struct ViewContext;
struct ViewLayer;
}  // namespace blender

struct bContext;

namespace blender::ed::sculpt_paint {

struct PaintCurveRadiusHandleDrawData {
  float2 point;
  float2 end;
  float2 perp;
  float4 color;
};

struct PaintCurveHandleDrawData {
  float2 position;
  float2 handle_left;
  float2 handle_right;
  float4 color_left;
  float4 color_right;
  int8_t h1;
  int8_t h2;
  bool selected_left;
  bool selected_right;
  bool selected_center;
};

struct PaintCurveSegmentDrawData {
  blender::Vector<float2> polyline;
  float4 wire_color;
  float4 outline_color;
};

struct PaintCurveScreenHandles {
  blender::Vector<PaintCurveHandleDrawData> points;
  blender::Vector<PaintCurveRadiusHandleDrawData> radius_handles;
  blender::Vector<PaintCurveSegmentDrawData> segments;
};

struct PaintCurveCachedObjectSilhouette {
  const Object *object;
  blender::Vector<blender::Vector<float2>> polylines;
};

struct PaintCurveScreenSilhouettes {
  blender::Vector<PaintCurveCachedObjectSilhouette> faint_objects;
  blender::Vector<blender::Vector<float2>> hover;
  const Object *hover_object = nullptr;
};

/** Screen-space radius of the radius-handle endpoint circle (matches paint_curve_intern). */
constexpr float PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS = 10.0f;

bool ED_paint_curve_is_curves_edit_tool(const char *active_tool_idname);

bool ED_paint_curve_overlay_is_relevant(const Brush *brush,
                                        const char *active_tool_idname,
                                        bool is_space_v3d,
                                        bool is_space_image);

bool ED_paint_curve_overlay_wants_redraw(const bContext *C);

bool ED_paint_curve_slide_is_active();

ViewContext ED_paint_curve_viewcontext_from_state(Depsgraph *depsgraph,
                                                  Scene *scene,
                                                  ViewLayer *view_layer,
                                                  ARegion *region,
                                                  View3D *v3d,
                                                  RegionView3D *rv3d);

Paint *ED_paint_curve_resolve_active_paint(Depsgraph *depsgraph,
                                           Scene *scene,
                                           ViewLayer *view_layer,
                                           const SpaceLink *space_data,
                                           eSpace_Type space_type);

void ED_paint_curve_screen_handles_build(const ViewContext &vc,
                                         const Brush &brush,
                                         const Sculpt *sculpt,
                                         PaintCurveScreenHandles &r_out);

void ED_paint_curve_screen_silhouettes_build(const ViewContext &vc,
                                             float2 mval_region,
                                             const Object *source_object,
                                             bool compute_hover,
                                             PaintCurveScreenSilhouettes &r_out);

uint64_t ED_paint_curve_silhouette_cache_key_hash(const ViewContext &vc);

void ED_paint_curve_screen_silhouettes_build_cached(
    const ViewContext &vc,
    float2 mval_region,
    const Object *source_object,
    bool compute_hover,
    uint64_t cache_key,
    blender::Vector<PaintCurveCachedObjectSilhouette> &r_cache_store,
    uint64_t &r_cache_store_key,
    PaintCurveScreenSilhouettes &r_out);

}  // namespace blender::ed::sculpt_paint
