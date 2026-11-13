/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Selection masks in the 3D Viewport: the gesture operators (box / lasso / polyline / circle) and
 * the temporary keymap polls of their key-invoked variants. Split from paint_image_select_view3d.cc
 * to keep that file under the line limit.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>

#include "BLF_api.hh"
#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_function_ref.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_library.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DRW_engine.hh"
#include "DRW_select_buffer.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_uvedit.hh"
#include "ED_view3d.hh"

#include "GPU_batch.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_vertex_buffer.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../shapes/paint_shape_space.hh"
#include "../paint_intern.hh"
#include "../mesh/mesh_brush_common.hh"
#include "paint_face_selection_mask.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_view3d.hh"
#include "../mesh/paint_image_uv_geom.hh"
#include "../mesh/sculpt_intern.hh"

#include "GEO_reverse_uv_sampler.hh"
namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Drag threshold (region px) under which a gesture counts as a simple click (deselect). */
constexpr int SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX = 3;
/** Lasso/polyline minimum point spacing (region px). */
constexpr float SELECT_VIEW3D_PATH_MIN_STEP_PX = 2.0f;

/** \} */

/** The temporary gesture keys of the Sculpt Mode keymap (B, Ctrl-RMB) must only take over the
 * sculpt mask keys while the toolbar shows the Paint group; failing this makes the keys fall
 * through the keymap to the sculpt bindings. The workspace's resolved display group is the same
 * state the toolbar and #paint_sculpt_tool_display_is_paint read. */
static bool image_paint_selection_view3d_paint_display_poll(bContext *C)
{
  if (!image_paint_selection_view3d_tool_poll(C)) {
    return false;
  }
  return paint_sculpt_tool_display_is_paint(C);
}

/** The lasso also yields its Ctrl-RMB back to #BRUSH_OT_stencil_control while the active brush
 * has a stencil to control (the two share the binding in the Sculpt keymap).
 *
 * \note this condition must stay the complement of `stencil_control_poll`: when the brush uses a
 * stencil this poll fails and the earlier keymap entry falls through to the stencil control, and
 * otherwise the stencil poll fails and this one owns the key. */
static bool image_paint_selection_view3d_lasso_key_poll(bContext *C)
{
  if (!image_paint_selection_view3d_paint_display_poll(C)) {
    return false;
  }
  Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush(paint);
  return brush != nullptr && !paint_brush_uses_stencil(*brush);
}

/* -------------------------------------------------------------------- */
/** \name Gesture operators (box / lasso / polyline / circle)
 *
 * Self-contained modals: the gesture geometry is accumulated in region coordinates and, on
 * confirm, mapped onto the anchor plane and rasterized through #image_select_view3d_gesture_apply.
 * Navigation input (orbit / zoom / pan) passes through to the default handlers, mirroring the 2D
 * polyline's discipline.
 * \{ */

enum class GestureKind {
  Box,
  Lasso,
  Polyline,
  Circle,
};

struct SelectGesture3DData {
  GestureKind kind = GestureKind::Box;
  ImageSelectView3DAnchor anchor;
  bool is_simple_click = true;
  int2 press_xy = int2(0);
  int2 last_xy = int2(0);
  /* Box. */
  bool box_valid = false;
  rctf box_region = {0.0f, 0.0f, 0.0f, 0.0f};
  /* Lasso / polyline. */
  Vector<float2> path_region;
  /* Circle. */
  float2 circle_center = float2(0.0f);
  float circle_radius = 0.0f;
  bool started = false;
  /** Space is held: the shape follows the cursor (see #gesture_move_handle). */
  bool move = false;
  /** Modifier keys (#KM_SHIFT / #KM_CTRL) seen since the gesture started. */
  int modifier_seen = 0;
  /**
   * Set when a keymap key (B, Ctrl-RMB) invoked the gesture instead of the active select tool's
   * drag. The mouse button (#0 when the key itself started it) confirms the gesture on release,
   * and the modifiers that are part of the binding are excluded from the #gesture_apply mode
   * re-interpretation, which is meant for modifiers the user adds on top of the binding.
   */
  struct KeyInvoke {
    short button = 0;
    /** #wmEvent::modifier bits (#KM_SHIFT / #KM_CTRL / ...) that belong to the binding. */
    uint8_t reserved_modifiers = 0;
  };
  std::optional<KeyInvoke> key_invoke;
  /* Preview draw callback (region px), registered in the invoke, removed in the finish. */
  ARegionType *owner_region_type = nullptr;
  void *draw_handle = nullptr;
};

/** True for a Box / Circle in Scene mode: a shape authored on the anchor plane (a rectangle /
 * circle of the surface, like the paint shapes' Surface projection), not a screen shape. */
static bool gesture_is_scene_shape(const SelectGesture3DData &data)
{
  return !data.anchor.screen && ELEM(data.kind, GestureKind::Box, GestureKind::Circle);
}

/** Region position of a plane-px point: the inverse of #image_select_view3d_mval_to_plane_px. */
static bool gesture_plane_px_to_region(const ImageSelectView3DAnchor &anchor,
                                       const ARegion &region,
                                       const float2 &plane_px,
                                       float2 &r_region)
{
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (anchor.object == nullptr || rv3d == nullptr) {
    return false;
  }
  const float3 world = math::transform_point(float4x4(anchor.object->object_to_world()),
                                             anchor.plane_px_to_object(plane_px));
  const float4 proj = float4x4(rv3d->persmat) * float4(world.x, world.y, world.z, 1.0f);
  if (proj.w <= 0.0f) {
    return false;
  }
  r_region = float2((proj.x / proj.w * 0.5f + 0.5f) * float(region.winx),
                    (proj.y / proj.w * 0.5f + 0.5f) * float(region.winy));
  return true;
}

/**
 * The plane-space shape of a Scene mode Box / Circle: the press point is its anchor, the cursor
 * the opposite corner (Box, axis-aligned in the tangent frame) or a point of the rim (Circle, the
 * radius is measured on the plane). \a r_a / \a r_b are the press and cursor in plane px.
 */
static bool gesture_scene_plane_points(const SelectGesture3DData &data,
                                       const ARegion &region,
                                       float2 &r_a,
                                       float2 &r_b)
{
  return image_select_view3d_mval_to_plane_px(data.anchor, region, data.press_xy, r_a) &&
         image_select_view3d_mval_to_plane_px(data.anchor, region, data.last_xy, r_b);
}

/** The plane-space outline of a Scene mode shape, closed, in plane px. */
static void gesture_scene_outline(const SelectGesture3DData &data,
                                  const float2 &a,
                                  const float2 &b,
                                  Vector<float2> &r_outline)
{
  if (data.kind == GestureKind::Box) {
    const float2 lo = math::min(a, b);
    const float2 hi = math::max(a, b);
    r_outline = {lo, float2(hi.x, lo.y), hi, float2(lo.x, hi.y)};
    return;
  }
  constexpr int SEGMENTS = 48;
  const float radius = math::distance(a, b);
  r_outline.clear();
  for (const int i : IndexRange(SEGMENTS)) {
    const float angle = (float(i) / float(SEGMENTS)) * 2.0f * float(M_PI);
    r_outline.append(a + float2(std::cos(angle), std::sin(angle)) * radius);
  }
}

/**
 * Region-post-pixel preview of the gesture the user is drawing. Without it the polyline is
 * unusable: its clicks are invisible until the mask lands, so there is no feedback where the
 * points land or that the tool even registered them.
 */
static void gesture_draw_preview(const bContext * /*C*/, ARegion *region, void *arg)
{
  auto *data = static_cast<SelectGesture3DData *>(arg);
  if (data == nullptr) {
    return;
  }
  /* Scene mode: the shape lives on the surface plane, so it is drawn there (projected back to the
   * region) and matches what the commit selects. */
  Vector<float2> scene_loop;
  if (gesture_is_scene_shape(*data) && region != nullptr && !data->is_simple_click) {
    float2 a, b;
    if (gesture_scene_plane_points(*data, *region, a, b)) {
      Vector<float2> outline;
      gesture_scene_outline(*data, a, b, outline);
      for (const float2 &p : outline) {
        float2 screen;
        if (gesture_plane_px_to_region(data->anchor, *region, p, screen)) {
          scene_loop.append(screen);
        }
      }
    }
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(1.0f);
  const uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos",
                                           gpu::VertAttrType::SFLOAT_32_32);
  /* No 2D uniform-color builtin in this version: the region-pixel overlays (like the 2D move
   * preview) bind the 3D one with a 2-component pos attribute. */
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.85f);

  if (gesture_is_scene_shape(*data)) {
    if (scene_loop.size() >= 3) {
      immBegin(GPU_PRIM_LINE_LOOP, uint(scene_loop.size()));
      for (const float2 &p : scene_loop) {
        immVertex2f(pos, p.x, p.y);
      }
      immEnd();
    }
  }
  else if (data->kind == GestureKind::Box && data->box_valid) {
    immBegin(GPU_PRIM_LINE_LOOP, 4);
    immVertex2f(pos, data->box_region.xmin, data->box_region.ymin);
    immVertex2f(pos, data->box_region.xmax, data->box_region.ymin);
    immVertex2f(pos, data->box_region.xmax, data->box_region.ymax);
    immVertex2f(pos, data->box_region.xmin, data->box_region.ymax);
    immEnd();
  }
  else if (ELEM(data->kind, GestureKind::Lasso, GestureKind::Polyline) &&
           !data->path_region.is_empty())
  {
    /* The polyline is click-built: draw the committed points plus the segment to the live cursor
     * position (#last_xy, refreshed on every MOUSEMOVE), so the click target is visible. */
    /* A line strip needs two vertices: the first lasso point alone has nothing to connect to
     * yet, and `immBegin` asserts on it. */
    if (data->path_region.size() >= 2) {
      immBegin(GPU_PRIM_LINE_STRIP, uint(data->path_region.size()));
      for (const float2 &p : data->path_region) {
        immVertex2f(pos, p.x, p.y);
      }
      immEnd();
    }
    if (data->kind == GestureKind::Polyline) {
      /* The segment still being aimed is dashed, so it reads as not yet placed. Dashes are laid out
       * by hand: the immediate-mode line has no stipple in this version. */
      const float2 from = data->path_region.last();
      const float2 to = float2(data->last_xy);
      const float length = math::distance(from, to);
      const float dash = 6.0f * UI_SCALE_FAC;
      const float gap = 4.0f * UI_SCALE_FAC;
      if (length > 1.0f) {
        const float2 dir = (to - from) / length;
        const int dash_num = int(length / (dash + gap)) + 1;
        immBegin(GPU_PRIM_LINES, uint(dash_num) * 2);
        for (const int i : IndexRange(dash_num)) {
          const float start = float(i) * (dash + gap);
          const float end = std::min(start + dash, length);
          const float2 a = from + dir * start;
          const float2 b = from + dir * end;
          immVertex2f(pos, a.x, a.y);
          immVertex2f(pos, b.x, b.y);
        }
        immEnd();
      }
    }
  }
  else if (data->kind == GestureKind::Circle && data->circle_radius > 0.0f) {
    constexpr int SEGMENTS = 48;
    immBegin(GPU_PRIM_LINE_STRIP, SEGMENTS + 1);
    for (const int i : IndexRange(SEGMENTS + 1)) {
      const float angle = (float(i) / float(SEGMENTS)) * 2.0f * M_PI;
      immVertex2f(pos,
                  data->circle_center.x + std::cos(angle) * data->circle_radius,
                  data->circle_center.y + std::sin(angle) * data->circle_radius);
    }
    immEnd();
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  /* Angle of the segment being aimed, like the WM polyline gesture of the 2D editors. Hidden for
   * a trivial segment so a fresh click does not flicker a 0.0 degree readout. */
  if (data->kind == GestureKind::Polyline && !data->path_region.is_empty()) {
    const float2 anchor = data->path_region.last();
    const float2 live = float2(data->last_xy);
    if (math::distance_squared(live, anchor) > 1.0f) {
      const float2 delta = live - anchor;
      char text[64];
      SNPRINTF(text, "Angle = %.1f°", double(RAD2DEGF(std::atan2(delta.y, delta.x))));
      const float x = live.x + 15.0f * UI_SCALE_FAC;
      const float y = live.y + 20.0f * UI_SCALE_FAC;
      const int font_id = BLF_default();
      BLF_color4f(font_id, 0.0f, 0.0f, 0.0f, 0.5f);
      BLF_draw_default(x + 1.0f, y - 1.0f, 0.0f, text, strlen(text));
      BLF_color4f(font_id, 1.0f, 1.0f, 1.0f, 1.0f);
      BLF_draw_default(x, y, 0.0f, text, strlen(text));
    }
  }
}

/**
 * Holding Space while drawing moves the whole shape with the cursor instead of resizing it, like
 * the WM box / lasso gestures of the 2D editors (#GESTURE_MODAL_MOVE). Returns true when the event
 * was consumed.
 */
static bool gesture_move_handle(bContext *C, SelectGesture3DData &data, const wmEvent *event)
{
  const int2 mval(event->mval[0], event->mval[1]);
  if (event->type == EVT_SPACEKEY && ELEM(event->val, KM_PRESS, KM_RELEASE)) {
    data.move = (event->val == KM_PRESS);
    data.last_xy = mval;
    return true;
  }
  if (event->type == MOUSEMOVE && data.move) {
    const int2 delta_px = mval - data.last_xy;
    const float2 delta = float2(delta_px);
    data.last_xy = mval;
    data.press_xy += delta_px;
    data.box_region.xmin += delta.x;
    data.box_region.xmax += delta.x;
    data.box_region.ymin += delta.y;
    data.box_region.ymax += delta.y;
    data.circle_center += delta;
    for (float2 &p : data.path_region) {
      p += delta;
    }
    ED_region_tag_redraw(CTX_wm_region(C));
    return true;
  }
  return false;
}

static void gesture_data_free(SelectGesture3DData *data)
{
  if (data != nullptr) {
    MEM_delete(data);
  }
}

/** True for the view-navigation input a gesture must pass through untouched. */
static bool gesture_is_navigation(const wmEvent *event)
{
  if (ELEM(event->type, MIDDLEMOUSE, WHEELUPMOUSE, WHEELDOWNMOUSE, WHEELINMOUSE, WHEELOUTMOUSE)) {
    return true;
  }
  return event->type == MOUSEPAN || event->type == MOUSEZOOM || event->type == MOUSEROTATE ||
         event->type == NDOF_MOTION;
}

static float2 gesture_mval_f(const wmEvent *event)
{
  return float2(float(event->mval[0]), float(event->mval[1]));
}

static void gesture_status_box(bContext *C, const SelectGesture3DData &data)
{
  const float width = data.box_region.xmax - data.box_region.xmin;
  const float height = data.box_region.ymax - data.box_region.ymin;
  char text[128];
  SNPRINTF(text, "Size: %.0f × %.0f px", width, height);
  ED_workspace_status_text(C, IFACE_(text));
}

static void gesture_status_circle(bContext *C, const SelectGesture3DData &data)
{
  char text[128];
  SNPRINTF(text, "Radius: %.0f px", double(data.circle_radius));
  ED_workspace_status_text(C, IFACE_(text));
}

/** Map a region-space rectangle to a plane-px quad (4 corners through the anchor). */
static bool gesture_box_to_plane_quad(const SelectGesture3DData &data,
                                      const ARegion &region,
                                      float2 r_quad[4])
{
  const int2 corners[4] = {int2(int(data.box_region.xmin), int(data.box_region.ymin)),
                           int2(int(data.box_region.xmax), int(data.box_region.ymin)),
                           int2(int(data.box_region.xmax), int(data.box_region.ymax)),
                           int2(int(data.box_region.xmin), int(data.box_region.ymax))};
  for (const int i : IndexRange(4)) {
    if (!image_select_view3d_mval_to_plane_px(data.anchor, region, corners[i], r_quad[i])) {
      return false;
    }
  }
  return true;
}

/**
 * Edge policy of the free-form gestures, from the same feather setting the 2D Wand / Quick
 * selection use. The default feathered policy leaves a visible outline around the selected
 * fragment.
 */
static PaintSelectionEdgePolicy gesture_edge_policy_from_settings(const bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  const float feather_px = scene != nullptr ? scene->toolsettings->imapaint.select_feather_px :
                                              0.0f;
  return image_paint_selection_edge_policy_from_feather(feather_px);
}

/** Rasterize the quad / polygon / circle the gesture described. */
static bool gesture_apply(bContext *C, wmOperator *op, SelectGesture3DData &data)
{
  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  if (region == nullptr || ob == nullptr) {
    return false;
  }
  eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  /* The keymap only maps Shift / Ctrl held at the press; holding them later (or while moving the
   * shape with Space) must still extend / subtract. An explicit mode of the tool is kept. The
   * modifiers that are part of the keymap binding itself (the temporary lasso always holds Ctrl)
   * don't count: they would re-interpret its replace mode into a subtract. */
  if (sel_op == SEL_OP_SET) {
    const int reserved_modifiers = data.key_invoke ? int(data.key_invoke->reserved_modifiers) : 0;
    const int dynamic_modifiers = data.modifier_seen & ~reserved_modifiers;
    if (dynamic_modifiers & KM_SHIFT) {
      sel_op = SEL_OP_ADD;
    }
    else if (dynamic_modifiers & KM_CTRL) {
      sel_op = SEL_OP_SUB;
    }
  }

  /* The gesture geometry is mapped onto the anchor plane once: the inside test below runs per
   * projected texel and must not redo the screen->plane mapping per pixel. */
  Vector<float2> polygon;
  float2 circle_center = float2(0.0f);
  float circle_radius = 0.0f;
  const char *undo_name = "Select";
  PaintSelectionEdgePolicy edge_policy = BKE_image_paint_selection_edge_policy_hard();

  float2 scene_a, scene_b;
  if (gesture_is_scene_shape(data) && !gesture_scene_plane_points(data, *region, scene_a, scene_b))
  {
    return false;
  }
  if (data.kind == GestureKind::Box) {
    if (gesture_is_scene_shape(data)) {
      Vector<float2> outline;
      gesture_scene_outline(data, scene_a, scene_b, outline);
      polygon.extend(outline.as_span());
    }
    else {
      float2 quad[4];
      if (!gesture_box_to_plane_quad(data, *region, quad)) {
        return false;
      }
      polygon.extend(quad, 4);
    }
    undo_name = "Box Select";
  }
  else if (data.kind == GestureKind::Lasso || data.kind == GestureKind::Polyline) {
    if (data.path_region.size() < 3) {
      return false;
    }
    polygon.resize(data.path_region.size());
    for (const int i : data.path_region.index_range()) {
      float2 p;
      if (!image_select_view3d_mval_to_plane_px(
              data.anchor, *region, int2(int(data.path_region[i].x), int(data.path_region[i].y)), p))
      {
        return false;
      }
      polygon[i] = p;
    }
    undo_name = "Lasso Select";
    edge_policy = gesture_edge_policy_from_settings(C);
  }
  else { /* Circle. */
    if (gesture_is_scene_shape(data)) {
      /* The radius is measured on the plane, so the circle stays a circle of the surface. */
      circle_center = scene_a;
      circle_radius = std::max(math::distance(scene_a, scene_b), 1.0f);
    }
    else if (!image_select_view3d_mval_to_plane_px(data.anchor,
                                                   *region,
                                                   int2(int(data.circle_center.x),
                                                        int(data.circle_center.y)),
                                                   circle_center))
    {
      return false;
    }
    else {
      circle_radius = std::max(data.circle_radius, 1.0f);
    }
    undo_name = "Circle Select";
    edge_policy = gesture_edge_policy_from_settings(C);
  }

  rctf domain;
  BLI_rctf_init(&domain, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
  if (!polygon.is_empty()) {
    for (const float2 &p : polygon) {
      domain.xmin = std::min(domain.xmin, p.x);
      domain.xmax = std::max(domain.xmax, p.x);
      domain.ymin = std::min(domain.ymin, p.y);
      domain.ymax = std::max(domain.ymax, p.y);
    }
  }
  else {
    domain.xmin = circle_center.x - circle_radius;
    domain.xmax = circle_center.x + circle_radius;
    domain.ymin = circle_center.y - circle_radius;
    domain.ymax = circle_center.y + circle_radius;
  }

  return image_select_view3d_gesture_apply(
      C,
      *ob,
      data.anchor,
      domain,
      [&](const float2 &plane_px) {
        if (!polygon.is_empty()) {
          const float pt[2] = {plane_px.x, plane_px.y};
          return isect_point_poly_v2(pt,
                                     reinterpret_cast<const float (*)[2]>(polygon.data()),
                                     uint(polygon.size()));
        }
        return math::distance(plane_px, circle_center) <= circle_radius;
      },
      edge_policy,
      undo_name,
      sel_op);
}

static void gesture_finish_common(bContext *C, wmOperator *op)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data != nullptr && data->draw_handle != nullptr && data->owner_region_type != nullptr) {
    ED_region_draw_cb_exit(data->owner_region_type, data->draw_handle);
  }
  gesture_data_free(data);
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  WM_cursor_modal_restore(CTX_wm_window(C));
}

/** The mouse button whose release confirms the gesture: the one it was invoked with, or the left
 * button for the keymap keys that have no mouse button of their own. */
static short gesture_confirm_button(const SelectGesture3DData &data)
{
  if (data.key_invoke && data.key_invoke->button != 0) {
    return data.key_invoke->button;
  }
  return short(LEFTMOUSE);
}

static bool gesture_is_confirm_release(const SelectGesture3DData &data, const wmEvent &event)
{
  return event.val == KM_RELEASE && event.type == gesture_confirm_button(data);
}

/** A gesture started by a key (B, or a key with no mouse button of its own) has no held button to
 * finish it, so a right-button press cancels. Tool-invoked gestures and bindings that use the
 * right button themselves keep the button for their own confirmation. */
static bool gesture_is_key_invoke_cancel(const SelectGesture3DData &data, const wmEvent &event)
{
  return event.val == KM_PRESS && event.type == RIGHTMOUSE && data.key_invoke &&
         data.key_invoke->button == 0;
}

/** Confirm the gesture on the release of its button: apply the shape unless it was a click
 * without a drag. A key-invoked click only cancels, so a stray key press cannot wipe the whole
 * selection; the tool's plain click deselects. */
static wmOperatorStatus gesture_confirm(bContext *C, wmOperator *op, SelectGesture3DData &data)
{
  Object *ob = CTX_data_active_object(C);
  const bool simple = data.is_simple_click && !data.key_invoke.has_value();
  /* A box only has a shape once the drag threshold was crossed; the lasso has no separate flag. */
  const bool has_shape = (data.kind == GestureKind::Box) ? data.box_valid : !data.is_simple_click;
  const bool applied = has_shape && ob != nullptr && gesture_apply(C, op, data);
  gesture_finish_common(C, op);
  if (simple && ob != nullptr) {
    image_select_view3d_deselect(C, *ob);
  }
  return applied || simple ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static wmOperatorStatus gesture_invoke_common(bContext *C,
                                              wmOperator *op,
                                              const wmEvent *event,
                                              const GestureKind kind)
{
  image_paint_selection_mask_map_ensure(C);
  /* A floating fragment under the cursor is moved, not re-selected (2D parity). Checked BEFORE
   * the general poll, which refuses while a session is live. */
  if (PaintSelectView3DFloatingSession *floating = image_select_view3d_session_active()) {
    if (floating->owner_v3d == CTX_wm_view3d(C) &&
        floating->cursor_over_fragment(C, int2(event->mval[0], event->mval[1])))
    {
      WM_operator_name_call(
          C, "PAINT_OT_image_select_view3d_move", wm::OpCallContext::InvokeDefault, nullptr, event);
      return OPERATOR_FINISHED;
    }
    /* A live session of another tool is committed first, like the 2D gestures do. */
    image_select_view3d_session_end(C, true);
  }
  if (!image_paint_selection_view3d_poll(C)) {
    return OPERATOR_PASS_THROUGH;
  }

  auto *data = MEM_new<SelectGesture3DData>(__func__);
  data->kind = kind;
  /* The shared tool setting picks View (screen-space shape) or Scene (shape laid on the surface)
   * for the Box and Circle gestures; Lasso and Polyline always lay their path on the surface. */
  const Scene *scene = CTX_data_scene(C);
  const bool use_screen_space = ELEM(kind, GestureKind::Box, GestureKind::Circle) &&
                                scene != nullptr &&
                                scene->toolsettings->imapaint.selection_space ==
                                    IMAGE_PAINT_SELECT_SPACE_VIEW;
  if (!image_select_view3d_anchor_from_mval(
          C, int2(event->mval[0], event->mval[1]), data->anchor, use_screen_space))
  {
    gesture_data_free(data);
    BKE_report(op->reports, RPT_WARNING, "Selection: no surface under the cursor");
    return OPERATOR_CANCELLED;
  }
  data->press_xy = int2(event->mval[0], event->mval[1]);
  data->last_xy = data->press_xy;
  data->modifier_seen = event->modifier;
  if (kind == GestureKind::Circle) {
    data->circle_center = gesture_mval_f(event);
  }
  if (kind == GestureKind::Polyline) {
    data->path_region.append(gesture_mval_f(event));
  }
  op->customdata = data;
  if (ARegion *region = CTX_wm_region(C)) {
    data->owner_region_type = region->runtime->type;
    data->draw_handle = ED_region_draw_cb_activate(
        data->owner_region_type, gesture_draw_preview, data, REGION_DRAW_POST_PIXEL);
  }
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  ED_workspace_status_text(C, nullptr);
  return OPERATOR_RUNNING_MODAL;
}

/** Shared init of the keymap-invoked gestures: the key press has no held selection mouse button,
 * and the modifiers that matched the binding are not dynamic ones (see #gesture_apply). */
static void gesture_init_key_invoked(wmOperator *op, const wmEvent &event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  data->key_invoke = SelectGesture3DData::KeyInvoke{
      short(ELEM(event.type, LEFTMOUSE, RIGHTMOUSE) ? event.type : 0),
      uint8_t(event.modifier)};
}

static wmOperatorStatus gesture_invoke_key_common(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event,
                                                  const GestureKind kind)
{
  const wmOperatorStatus status = gesture_invoke_common(C, op, event, kind);
  if (status == OPERATOR_RUNNING_MODAL) {
    gesture_init_key_invoked(op, *event);
  }
  return status;
}

/** Shared modal tail: confirm on release / cancel, navigation pass-through. */
static wmOperatorStatus gesture_modal_common(bContext * /*C*/,
                                             wmOperator * /*op*/,
                                             const wmEvent *event,
                                             SelectGesture3DData &data)
{
  /* Shift / Ctrl held at the start or at any later event (e.g. while moving the shape with Space,
   * which the keymap cannot know) extend / subtract, see #gesture_apply. */
  data.modifier_seen |= event->modifier;
  if (gesture_is_navigation(event)) {
    return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus view3d_select_box_invoke(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Box);
}

static wmOperatorStatus view3d_select_box_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      const float2 mval = gesture_mval_f(event);
      data->last_xy = int2(event->mval[0], event->mval[1]);
      if (math::distance(mval, float2(data->press_xy)) >
          float(SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX))
      {
        data->is_simple_click = false;
        data->box_valid = true;
      }
      data->box_region.xmin = std::min(float(data->press_xy.x), mval.x);
      data->box_region.xmax = std::max(float(data->press_xy.x), mval.x);
      data->box_region.ymin = std::min(float(data->press_xy.y), mval.y);
      data->box_region.ymax = std::max(float(data->press_xy.y), mval.y);
      gesture_status_box(C, *data);
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case LEFTMOUSE:
    case RIGHTMOUSE: {
      /* A tool-invoked box has no #key_invoke, so #gesture_confirm_button is LMB and an RMB
       * release falls through here untouched, as before the right button was handled. */
      if (gesture_is_confirm_release(*data, *event)) {
        return gesture_confirm(C, op, *data);
      }
      /* A key-invoked box has no mouse button held: RMB cancels it, like the WM box gestures. */
      if (gesture_is_key_invoke_cancel(*data, *event)) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    }
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_box(wmOperatorType *ot)
{
  ot->name = "Box Select";
  ot->idname = "PAINT_OT_image_select_view3d_box";
  ot->description = "Select a box region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_box_invoke;
  ot->modal = view3d_select_box_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

/* Temporary keymap binding (B): the same gesture as the Box tool, but the poll keeps it to the
 * Paint toolbar display so the key falls through to the sculpt mask gesture otherwise. */
static wmOperatorStatus view3d_select_box_key_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  /* The box anchors at the cursor position of the key press and grows from the following mouse
   * motion; the first LMB press is not consumed, so a click without motion confirms to nothing
   * (#gesture_confirm) instead of selecting. */
  return gesture_invoke_key_common(C, op, event, GestureKind::Box);
}

void PAINT_OT_image_select_view3d_box_key(wmOperatorType *ot)
{
  ot->name = "Box Select (Paint Display)";
  ot->idname = "PAINT_OT_image_select_view3d_box_key";
  ot->description =
      "Select a box region of the paint canvas drawn on the surface, without switching the "
      "active tool. Only while the Sculpt Mode toolbar shows the Paint group, so the sculpt "
      "mask keys keep the shortcut otherwise";
  ot->invoke = view3d_select_box_key_invoke;
  ot->modal = view3d_select_box_modal;
  ot->poll = image_paint_selection_view3d_paint_display_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

static wmOperatorStatus view3d_select_lasso_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Lasso);
}

static wmOperatorStatus view3d_select_lasso_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      const float2 mval = gesture_mval_f(event);
      if (math::distance(mval, float2(data->press_xy)) >
          float(SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX))
      {
        data->is_simple_click = false;
      }
      if (data->path_region.is_empty() ||
          math::distance(mval, data->path_region.last()) >= SELECT_VIEW3D_PATH_MIN_STEP_PX)
      {
        data->path_region.append(mval);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
    case LEFTMOUSE:
    case RIGHTMOUSE:
      if (gesture_is_confirm_release(*data, *event)) {
        return gesture_confirm(C, op, *data);
      }
      if (gesture_is_key_invoke_cancel(*data, *event)) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_lasso(wmOperatorType *ot)
{
  ot->name = "Lasso Select";
  ot->idname = "PAINT_OT_image_select_view3d_lasso";
  ot->description = "Select a lasso region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_lasso_invoke;
  ot->modal = view3d_select_lasso_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

/* Temporary keymap binding (Ctrl-RMB press): the same gesture as the Lasso tool, but the poll
 * keeps it to the Paint toolbar display (and off the brushes with a stencil), so the key falls
 * through to the stencil control otherwise. */
static wmOperatorStatus view3d_select_lasso_key_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  return gesture_invoke_key_common(C, op, event, GestureKind::Lasso);
}

void PAINT_OT_image_select_view3d_lasso_key(wmOperatorType *ot)
{
  ot->name = "Lasso Select (Paint Display)";
  ot->idname = "PAINT_OT_image_select_view3d_lasso_key";
  ot->description =
      "Select a lasso region of the paint canvas drawn on the surface, without switching the "
      "active tool. Only while the Sculpt Mode toolbar shows the Paint group and the brush has "
      "no stencil, so the stencil control keeps the shortcut otherwise";
  ot->invoke = view3d_select_lasso_key_invoke;
  ot->modal = view3d_select_lasso_modal;
  ot->poll = image_paint_selection_view3d_lasso_key_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

static wmOperatorStatus view3d_select_polyline_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Polyline);
}

static wmOperatorStatus view3d_select_polyline_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      /* Refreshed for the preview draw: the segment from the last committed point to the live
       * cursor is what the user aims with between clicks. */
      data->last_xy = int2(int(event->mval[0]), int(event->mval[1]));
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        const float2 mval = gesture_mval_f(event);
        /* A click within the close radius of the first point closes the polygon, like the WM
         * polyline gesture (#wm::gesture::POLYLINE_CLICK_RADIUS). With fewer than 3 points the
         * click cannot close anything and just places another point: canceling on it would make
         * the radius swallow the second click of a short polyline. */
        if (data->path_region.size() >= 3 &&
            math::distance(mval, data->path_region.first()) <
                wm::gesture::POLYLINE_CLICK_RADIUS * UI_SCALE_FAC)
        {
          if (gesture_apply(C, op, *data)) {
            gesture_finish_common(C, op);
            return OPERATOR_FINISHED;
          }
          gesture_finish_common(C, op);
          return OPERATOR_CANCELLED;
        }
        data->path_region.append(mval);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case EVT_RETKEY:
    case EVT_PADENTER:
      if (event->val == KM_PRESS) {
        if (data->path_region.size() >= 3 && gesture_apply(C, op, *data)) {
          gesture_finish_common(C, op);
          return OPERATOR_FINISHED;
        }
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_polyline(wmOperatorType *ot)
{
  ot->name = "Polyline Select";
  ot->idname = "PAINT_OT_image_select_view3d_polyline";
  ot->description = "Select a polyline region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_polyline_invoke;
  ot->modal = view3d_select_polyline_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

static wmOperatorStatus view3d_select_circle_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Circle);
}

static wmOperatorStatus view3d_select_circle_modal(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      data->last_xy = int2(event->mval[0], event->mval[1]);
      data->circle_radius = math::distance(gesture_mval_f(event), data->circle_center);
      data->is_simple_click = data->circle_radius <=
                              float(SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX);
      gesture_status_circle(C, *data);
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        Object *ob = CTX_data_active_object(C);
        const bool simple = data->is_simple_click;
        const bool applied = !simple && ob != nullptr && gesture_apply(C, op, *data);
        gesture_finish_common(C, op);
        if (simple && ob != nullptr) {
          image_select_view3d_deselect(C, *ob);
        }
        return applied || simple ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_circle(wmOperatorType *ot)
{
  ot->name = "Circle Select";
  ot->idname = "PAINT_OT_image_select_view3d_circle";
  ot->description = "Select a circular region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_circle_invoke;
  ot->modal = view3d_select_circle_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
