/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Modal operator for editing a live Curve Patch control curve (see design doc). Started
 * automatically right after a `BRUSH_STROKE_CURVE_PATCH` anchor stroke finishes (Stage 05).
 *
 * A single, persistent modal operator (`SCULPT_OT_curve_patch_edit`) owns every event for the
 * whole live-edit session -- point/handle/segment/radius drag, insert, delete, texture-axis
 * toggle, and Enter/Esc commit/cancel are all plain internal function calls from one
 * `switch (event->type)`, not separate `wmOperatorType`s dispatched through a `wmKeyMap`. An
 * earlier version of this file *did* split each action into its own short-lived operator bound
 * via a procedural C++ keymap (matching how `builtin.curves_edit`'s `PAINTCURVE_OT_slide` etc.
 * work) -- that approach was reverted after it reliably left the installed keymap with zero
 * items at dispatch time (`WM_keymap_poll()` warning "empty keymap 'Curve Patch Edit'" on every
 * event, confirmed even with `--factory-startup`). Blender's key-config merge machinery
 * (`WM_keyconfig_update_ex()`, `wm_keymap.cc`) is built around keymaps a Python preset
 * (`blender_default.py`) knows about; a keymap that exists *only* on the C side and auto-installs
 * mid-session (rather than being a manually-selected tool keymap) falls into a poorly-exercised
 * corner of that system. Rather than keep fighting it, this file avoids `wmKeyMap`/`WM_keyconfig_*`
 * entirely for Curve Patch's own interactions.
 *
 * The mouse-delta drag math below mirrors the "move entire point" path of `paintcurve_slide`'s
 * modal (`paint_curve.cc`, `paintcurve_apply_handle_move_3d`) and the radius-handle math of
 * `paintcurve_slide_radius` (`paint_curve.cc`), adapted to operate on `CurvePatchCache::control_curve`
 * -- a standalone `bke::CurvesGeometry` that is not wrapped in a `PaintCurve` ID. The segment
 * hit-test helpers (`paintcurve_build_screen_segment_polyline_from_geometry`,
 * `paintcurve_bezier_param_at_screen_pos_on_segment_from_geometry`, etc., `paint_curve_sync.cc`/
 * `paint_curve_geometry.cc`) are the `_from_geometry` cores of
 * `paintcurve_build_screen_segment_polyline`/`paintcurve_bezier_param_at_screen_pos_on_segment`,
 * shared with the Stage 06 overlay draw path (`ED_paint_curve_screen_handles_build_from_geometry`,
 * `paint_curve_draw.cc`).
 */

#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "BLI_index_mask.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_image.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "DNA_brush_types.h"
#include "DNA_color_types.h"
#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "ED_paint_curve_draw.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "mesh/sculpt_intern.hh"
#include "mesh/sculpt_undo.hh"
#include "paint_curve_intern.hh"
#include "paint_curve_patch_cache.hh"

namespace blender::ed::sculpt_paint {

struct CurvePatchEditOpData {
  int active_point = -1;
  bool dragging_point = false;
  bool dragging_radius = false;
  /* True while dragging one of the point's two Bezier tangent handles (left/right), which shapes
   * how sharply the curve bends through that point -- as opposed to `dragging_point`, which
   * translates the whole point (co + both handles together) without changing its local shape. */
  bool dragging_handle = false;
  bool handle_is_left = false;
  /* True while dragging a point on the curve WIRE (between two control points), reshaping that
   * segment by solving for its two adjacent handles -- matches Curve Edit's segment-slide
   * affordance (#PAINTCURVE_OT_slide's `move_segment`). */
  bool dragging_segment = false;
  int segment_index_a = -1;
  int segment_index_b = -1;
  float segment_t = 0.0f;
  float2 drag_start_mval = float2(0.0f);
  /* Object-space positions of the active point's left handle / co / right handle, captured when
   * the drag starts. Mirrors `PointSlideData::point_initial_loc_3d` (paint_curve.cc): every
   * MOUSEMOVE re-derives the delta from these ORIGINAL positions instead of the current ones, so
   * floating-point drift never accumulates across a long drag. */
  float3 point_initial_loc_3d[3] = {float3(0.0f), float3(0.0f), float3(0.0f)};
  /* Screen-space radius-handle axis captured once when a radius drag starts (see
   * `paintcurve_slide_radius_invoke`, `paint_curve.cc:2653-2656`); reused unchanged for every
   * MOUSEMOVE of that drag via `paintcurve_radius_from_handle_screen_pos()`. */
  PaintCurveRadiusHandleScreen radius_handle = {};
  /* Last brush Strength slider value this modal re-stamped with. The brush strength is applied
   * live (not frozen -- see `CurvePatchFrozenBrushParams`), so `curve_patch_edit_modal()` compares
   * the current `BKE_brush_alpha_get()` against this every event and re-stamps when it changes,
   * making a slider drag update the relief in real time. Seeded at invoke to the current value so
   * the first event does not re-stamp spuriously. */
  float last_synced_alpha = -1.0f;
  /* Last brush direction (Add vs Subtract, `BRUSH_DIR_IN`) this modal re-stamped with. Like the
   * strength above, the direction is read live -- `brush_strength()` folds it in via
   * `brush_flip()`, so `cache.bstrength` (and thus the relief's sign) flips with the brush's
   * Add/Subtract toggle. Watched here so toggling it re-stamps in real time. Seeded at invoke. */
  bool last_synced_dir_in = false;
  /* Last texture-along-length mapping (`MTex::curve_patch_length_mode` /
   * `curve_patch_length_repeat`) and mesh mirror symmetry this modal re-stamped with. These are
   * read live from the brush/mesh so changing the Length mode, the Repeats count, or toggling
   * Mirror X/Y/Z re-projects the texture immediately. When any changes, the poll copies the new
   * length values into `frozen_params` (which the relief formula reads) and re-stamps; symmetry is
   * picked up live by `do_symmetrical_brush_actions()` on that same re-stamp. Seeded at invoke. */
  int last_synced_length_mode = -1;
  int last_synced_length_repeat = -1;
  int last_synced_symm = -1;
  /* Last texture and texture-mapping state this modal re-stamped with. The relief already reads
   * `brush.mtex` live on every re-stamp (size/offset/`tex`), so these only need a re-stamp TRIGGER:
   * when the assigned texture, its mapping size/offset, or the swap-axis flag changes, re-project.
   * Edits to the texture DATABLOCK itself (procedural params, image swap, etc.) are caught via the
   * `BKE_paint_get_overlay_texture_edit_count()` monotonic counter below -- the brush texture is
   * not in the depsgraph, so `Tex_Runtime::last_update` on the original never bumps and cannot be
   * used here. `swap_axis` also feeds `frozen_params`, so the poll copies it across before
   * re-stamping. */
  bool last_synced_swap_axis = false;
  float last_synced_tex_size[2] = {0.0f, 0.0f};
  float last_synced_tex_ofs[2] = {0.0f, 0.0f};
  const void *last_synced_tex = nullptr;
  uint64_t last_synced_tex_edit_count = 0;
  /* Last brush Falloff (distance falloff) this modal re-stamped with. The relief reads the falloff
   * live on every re-stamp via `BKE_brush_curve_strength()`, which folds in
   * `Brush::curve_distance_falloff_preset` and the custom `curve_distance_falloff` CurveMapping.
   * So, like the texture above, these only need to trigger a re-stamp on change: the preset for a
   * shape switch, and the CurveMapping's `changed_timestamp` (bumped by any edit to the custom
   * curve, see #BKE_curvemapping_changed). Seeded at invoke. */
  int last_synced_falloff_preset = -1;
  int last_synced_falloff_curve_ts = -1;
  /* Steady-cadence timer (see `curve_patch_edit_invoke`). The live-sync poll at the top of
   * `curve_patch_edit_modal` only runs on window events, so a discrete brush change made in a panel
   * with no follow-up event -- e.g. picking a texture or image from a browse menu -- would not
   * re-project until the next stray event reached this modal. This timer makes the poll tick
   * regardless, so such changes update the relief promptly. */
  wmTimer *sync_timer = nullptr;
};

static CurvePatchCache &patch_cache_of(bContext *C)
{
  Object &ob = *CTX_data_active_object(C);
  return *ob.runtime->sculpt_session->curve_patch_cache;
}

/* `sculpt_mode_poll()` alone only confirms Sculpt Mode is active -- it says nothing about whether
 * a live Curve Patch exists. Without this extra check, this operator is directly invocable (F3
 * search, a stray keymap entry, `bpy.ops.sculpt.curve_patch_edit()`) any time the user is in
 * Sculpt Mode, independent of whether Stage 05's anchor-stroke flow has ever started one --
 * `patch_cache_of()` would then dereference a null `curve_patch_cache` on the very first event. */
static bool curve_patch_edit_poll(bContext *C)
{
  if (!sculpt_mode_poll(C)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->runtime->sculpt_session && ob->runtime->sculpt_session->curve_patch_cache;
}

/* -------------------------------------------------------------------- */
/** \name Geometry-Only Radius-Handle Helpers
 *
 * Thin local wrappers around the shared `_from_geometry` cores (`paint_curve_sync.cc`), which
 * take `bke::CurvesGeometry` directly instead of a `PaintCurve *` and are also used by the
 * Stage 06 overlay draw path (`ED_paint_curve_screen_handles_build_from_geometry`).
 * \{ */

static int patch_find_radius_handle_at_pos(const bke::CurvesGeometry &geom,
                                           const Span<PaintCurvePoint> screen_points,
                                           const float pos[2],
                                           const float threshold)
{
  if (!paintcurve_geometry_is_valid(geom) || screen_points.is_empty()) {
    return -1;
  }

  int best_index = -1;
  float best_dist = threshold;

  for (const int i : IndexRange(geom.points_num())) {
    PaintCurveRadiusHandleScreen handle;
    paintcurve_radius_handle_screen_get_from_geometry(geom, screen_points.data(), i, &handle);
    const float end[2] = {handle.end.x, handle.end.y};
    const float dist_end = len_v2v2(pos, end);
    if (dist_end < best_dist) {
      best_dist = dist_end;
      best_index = i;
    }
  }

  return best_index;
}

/** Closest Bezier segment to `pos` within `threshold` screen pixels, mirroring
 * #paintcurve_find_closest_segment (paint_curve.cc) but operating on a standalone `geom` --
 * shared by the Ctrl+RMB insert hit-test and the plain-click segment-drag start below, both of
 * which need the same (segment_index, segment_index_next, edge_t) search. */
static bool patch_find_closest_segment(const bke::CurvesGeometry &geom,
                                       const ViewContext *vc,
                                       const float pos[2],
                                       const float threshold,
                                       int *r_segment_index,
                                       int *r_segment_index_next,
                                       float *r_edge_t)
{
  if (!paintcurve_geometry_is_valid(geom) || geom.points_num() < 2) {
    return false;
  }

  int segment_index = -1;
  int segment_index_next = -1;
  float edge_t = 0.0f;
  float best_dist_sq = square_f(threshold);
  const float2 mval(pos[0], pos[1]);

  paintcurve_foreach_bezier_segment_from_geometry(geom, [&](const int point_a, const int point_b) {
    Vector<float2> polyline;
    paintcurve_build_screen_segment_polyline_from_geometry(geom, vc, point_a, point_b, polyline);
    const float dist_sq = ED_paint_curve_polyline_distance_sq(polyline, mval);
    if (dist_sq < best_dist_sq) {
      float bezier_t = 0.0f;
      if (paintcurve_bezier_param_at_screen_pos_on_segment_from_geometry(
              vc, geom, pos, point_a, point_b, bezier_t))
      {
        best_dist_sq = dist_sq;
        segment_index = point_a;
        segment_index_next = point_b;
        edge_t = bezier_t;
      }
    }
  });

  if (segment_index < 0) {
    return false;
  }
  *r_segment_index = segment_index;
  *r_segment_index_next = segment_index_next;
  *r_edge_t = edge_t;
  return true;
}

/** \} */

/* Status-bar hint, refreshed on invoke and every axis toggle. Doubles as a visible signal that
 * an event actually reached this modal handler -- if the text never appears/updates, the
 * event isn't getting here at all (as opposed to it arriving but the texture effect being hard
 * to see for a given brush/texture setup). */
static void curve_patch_edit_status_set(bContext *C, const CurvePatchCache &patch)
{
  const std::string msg = fmt::format(
      "Enter: Commit | Esc: Cancel | Tab/S: Swap Texture Axis (currently {})",
      patch.frozen_params.swap_axis ? "U" : "V");
  ED_workspace_status_text(C, msg.c_str());
}

static wmOperatorStatus curve_patch_edit_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  CurvePatchEditOpData *data = MEM_new<CurvePatchEditOpData>(__func__);
  op->customdata = data;
  /* Seed the live-strength watchdog with the current slider value so the first modal event does
   * not read a "change" against the default and re-stamp needlessly (see
   * `CurvePatchEditOpData::last_synced_alpha`). */
  const ToolSettings *tool_settings = CTX_data_tool_settings(C);
  if (tool_settings && tool_settings->sculpt) {
    const Sculpt &sd = *tool_settings->sculpt;
    if (const Brush *brush = BKE_paint_brush_for_read(&sd.paint)) {
      data->last_synced_alpha = BKE_brush_alpha_get(&sd.paint, brush);
      data->last_synced_dir_in = (brush->flag & BRUSH_DIR_IN) != 0;
      data->last_synced_length_mode = brush->mtex.curve_patch_length_mode;
      data->last_synced_length_repeat = brush->mtex.curve_patch_length_repeat;
      data->last_synced_swap_axis = brush->mtex.use_curve_patch_swap_axis != 0;
      data->last_synced_tex_size[0] = brush->mtex.size[0];
      data->last_synced_tex_size[1] = brush->mtex.size[1];
      data->last_synced_tex_ofs[0] = brush->mtex.ofs[0];
      data->last_synced_tex_ofs[1] = brush->mtex.ofs[1];
      data->last_synced_tex = brush->mtex.tex;
      data->last_synced_tex_edit_count = BKE_paint_get_overlay_texture_edit_count();
      data->last_synced_falloff_preset = brush->curve_distance_falloff_preset;
      data->last_synced_falloff_curve_ts = brush->curve_distance_falloff ?
                                               brush->curve_distance_falloff->changed_timestamp :
                                               0;
    }
  }
  if (const Object *ob = CTX_data_active_object(C)) {
    data->last_synced_symm = mesh_symmetry_xyz_get(*ob);
  }
  WM_event_add_modal_handler(C, op);
  /* Drive the live-sync poll at a steady cadence (see `CurvePatchEditOpData::sync_timer`). 20 Hz:
   * fast enough that a panel change feels immediate, light enough that idle ticks -- which only
   * compare scalars and re-stamp on an actual change -- cost nothing. */
  const double sync_timer_step = 0.05;
  data->sync_timer = WM_event_timer_add(
      CTX_wm_manager(C), CTX_wm_window(C), TIMER, sync_timer_step);
  curve_patch_edit_status_set(C, patch_cache_of(C));
  return OPERATOR_RUNNING_MODAL;
}

static void curve_patch_edit_finish(bContext *C, wmOperator *op, const bool is_cancel);

static wmOperatorStatus curve_patch_edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  CurvePatchEditOpData &data = *static_cast<CurvePatchEditOpData *>(op->customdata);

  /* The live patch's backing data can vanish out from under this modal operator without an
   * explicit Enter/Esc: unhandled keys (see the `default:` case below) deliberately pass through
   * to the ordinary View3D keymap, and a global Undo (Ctrl+Z) reaching it that way tears down and
   * reallocates the active object's `SculptSession` via the ordinary undo-restore path -- freeing
   * `curve_patch_cache` out from under this operator without ever calling
   * #curve_patch_edit_finish. `curve_patch_edit_poll()` only guards the initial invoke, so without
   * this re-check here, the very next event (even a plain MOUSEMOVE) dereferences a dangling
   * `SculptSession`/`curve_patch_cache` in #patch_cache_of and crashes. */
  const Object *ob_check = CTX_data_active_object(C);
  if (!ob_check || !ob_check->runtime->sculpt_session ||
      !ob_check->runtime->sculpt_session->curve_patch_cache)
  {
    ED_workspace_status_text(C, nullptr);
    if (data.sync_timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data.sync_timer);
    }
    MEM_delete(&data);
    op->customdata = nullptr;
    return OPERATOR_CANCELLED;
  }

  CurvePatchCache &patch = patch_cache_of(C);
  Object &ob = *CTX_data_active_object(C);

  /* Live brush sync. Neither the Strength slider nor the Add/Subtract direction is frozen into the
   * patch (unlike radius/axis -- see `CurvePatchFrozenBrushParams`): every re-stamp reads them live
   * via `brush_strength()` (which folds the direction in through `brush_flip()`, so `cache.bstrength`
   * -- and thus the relief's sign -- flips with the Add/Subtract toggle). So whenever either changes
   * -- typically a slider drag or a direction toggle in the Properties/N-panel/header, whose events
   * this window-level modal sees before they pass through to the UI -- re-stamp so the relief tracks
   * the brush in real time. Only re-stamp on an ACTUAL change (exact compares), so ordinary events
   * stay free. `BKE_brush_alpha_get()` is the same UI value `brush_strength()` reads (unified-strength
   * aware). Deliberately kept ABOVE the pass-through gate below, so a change made OUTSIDE the viewport
   * (which the gate would otherwise pass straight through) is still caught here first. */
  const ToolSettings *tool_settings = CTX_data_tool_settings(C);
  if (tool_settings && tool_settings->sculpt) {
    const Sculpt &sd = *tool_settings->sculpt;
    if (const Brush *brush = BKE_paint_brush_for_read(&sd.paint)) {
      const float alpha = BKE_brush_alpha_get(&sd.paint, brush);
      const bool dir_in = (brush->flag & BRUSH_DIR_IN) != 0;
      /* Texture-along-length mapping and mesh mirror symmetry are read live too, so changing the
       * Length mode / Repeats count in the Stroke panel or toggling Mirror X/Y/Z re-projects the
       * texture immediately. The length values feed the relief formula through `frozen_params`;
       * symmetry is applied by `do_symmetrical_brush_actions()` inside the re-stamp itself. */
      const int length_mode = brush->mtex.curve_patch_length_mode;
      const int length_repeat = brush->mtex.curve_patch_length_repeat;
      const int symm = mesh_symmetry_xyz_get(ob);
      /* Texture + mapping watch (see `CurvePatchEditOpData`): the relief re-reads `brush.mtex` live,
       * so these just need to trigger a re-stamp when they change. */
      const bool swap_axis = brush->mtex.use_curve_patch_swap_axis != 0;
      const void *tex = brush->mtex.tex;
      /* Any edit to the brush's primary texture -- assign/clear, a mapping tweak, or an edit to
       * the texture datablock itself -- bumps this monotonic counter (see
       * `BKE_paint_invalidate_overlay_tex`). It replaces the original texture's `last_update`,
       * which never bumps because brush textures are not evaluated by the depsgraph. */
      const uint64_t tex_edit_count = BKE_paint_get_overlay_texture_edit_count();
      /* Brush Falloff (distance falloff) is read live by the relief through
       * `BKE_brush_curve_strength()`, so watch it as a re-stamp trigger only: the preset for a
       * shape switch, and the custom curve's `changed_timestamp` for point edits. */
      const int falloff_preset = brush->curve_distance_falloff_preset;
      const int falloff_curve_ts = brush->curve_distance_falloff ?
                                       brush->curve_distance_falloff->changed_timestamp :
                                       0;
      const bool tex_changed = tex != data.last_synced_tex ||
                               tex_edit_count != data.last_synced_tex_edit_count ||
                               brush->mtex.size[0] != data.last_synced_tex_size[0] ||
                               brush->mtex.size[1] != data.last_synced_tex_size[1] ||
                               brush->mtex.ofs[0] != data.last_synced_tex_ofs[0] ||
                               brush->mtex.ofs[1] != data.last_synced_tex_ofs[1];
      if (alpha != data.last_synced_alpha || dir_in != data.last_synced_dir_in ||
          length_mode != data.last_synced_length_mode ||
          length_repeat != data.last_synced_length_repeat || symm != data.last_synced_symm ||
          swap_axis != data.last_synced_swap_axis || tex_changed ||
          falloff_preset != data.last_synced_falloff_preset ||
          falloff_curve_ts != data.last_synced_falloff_curve_ts)
      {
        data.last_synced_alpha = alpha;
        data.last_synced_dir_in = dir_in;
        data.last_synced_length_mode = length_mode;
        data.last_synced_length_repeat = length_repeat;
        data.last_synced_symm = symm;
        data.last_synced_swap_axis = swap_axis;
        data.last_synced_tex = tex;
        data.last_synced_tex_edit_count = tex_edit_count;
        data.last_synced_tex_size[0] = brush->mtex.size[0];
        data.last_synced_tex_size[1] = brush->mtex.size[1];
        data.last_synced_tex_ofs[0] = brush->mtex.ofs[0];
        data.last_synced_tex_ofs[1] = brush->mtex.ofs[1];
        data.last_synced_falloff_preset = falloff_preset;
        data.last_synced_falloff_curve_ts = falloff_curve_ts;
        patch.frozen_params.length_mode = length_mode;
        patch.frozen_params.length_repeat = length_repeat;
        patch.frozen_params.swap_axis = swap_axis;
        /* The relief samples the texture through `ss.tex_pool`, an `ImagePool` that caches ImBuf
         * handles for the whole sculpt session (`brush_init_tex`, `sculpt.cc`). A changed image --
         * a different Image datablock on the texture, or edited pixels -- would otherwise keep
         * sampling the old buffer. Rebuild the pool on any texture change so the re-stamp below
         * picks up the new image. Gated on `tex_changed` so ordinary re-stamps (strength/falloff
         * drags) keep the pool's caching. */
        if (tex_changed) {
          ImagePool *&tex_pool = ob.runtime->sculpt_session->tex_pool;
          if (tex_pool != nullptr) {
            BKE_image_pool_free(tex_pool);
          }
          tex_pool = BKE_image_pool_new();
        }
        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
    }
  }

  /* #WM_event_add_modal_handler() (called from `curve_patch_edit_invoke()`) registers this
   * operator as a WINDOW-level modal handler (`win->runtime->modalhandlers`), not an area/region
   * one -- so it receives every mouse/keyboard event in the entire window, for whichever area
   * happens to be under the cursor, before that area's own handlers ever run. Without this guard,
   * a click on a Properties panel button, an Outliner entry, the N-panel, or a header -- anywhere
   * outside the 3D view this patch was started in -- was consumed by the `LEFTMOUSE`/`RIGHTMOUSE`
   * cases below (after missing the curve hit-test), so the click never reached the editor it was
   * actually meant for and all other UI interaction appeared frozen for the whole session.
   *
   * The event belongs to this modal ONLY when the region VISUALLY under the cursor is our frozen
   * viewport WINDOW region. This is decided exactly the way the window manager itself routes
   * events to regions -- #wm_event_do_handlers_area_regions() -> #ED_area_find_region_xy_visual()
   * (wm_event_system.cc). A plain `winrct` test is NOT enough: the 3D viewport's overlapping
   * regions (the N-panel #RGN_TYPE_UI, the redo HUD, the tool/area headers) are drawn ON TOP of
   * the WINDOW region, whose `winrct` extends underneath them. A click on the N-panel therefore
   * still lies inside the WINDOW `winrct`, so an earlier `winrct`-only test reported it as "inside
   * the viewport" and consumed it -- which is why those on-viewport panels stayed frozen even
   * after the first fix. #ED_area_find_region_xy_visual walks overlapping regions first, so it
   * returns the N-panel/HUD/header for such a click, and our WINDOW region only for a click on the
   * actual 3D view.
   *
   * `CTX_wm_area(C)`/`CTX_wm_region(C)` here are the FROZEN area/region captured at invoke time
   * (`wm_handler_op_context()` sets them before calling `ot->modal`), always this patch's own
   * viewport regardless of where the cursor has since moved -- so a query against a different area
   * (Properties/Outliner) finds no matching region and returns null, and we pass through. Enter/Esc
   * are exempt so commit/cancel works even with the cursor off the viewport. An active drag is
   * exempt too, so dragging a point over the N-panel or past the viewport edge does not orphan the
   * drag or eat the `LEFTMOUSE` release that ends it. */
  const bool is_dragging = data.dragging_point || data.dragging_radius || data.dragging_handle ||
                           data.dragging_segment;
  const ScrArea *area = CTX_wm_area(C);
  const ARegion *region = CTX_wm_region(C);
  const bool is_commit_key = ELEM(event->type, EVT_RETKEY, EVT_PADENTER, EVT_ESCKEY);
  const ARegion *region_hovered = area ? ED_area_find_region_xy_visual(
                                             area, RGN_TYPE_ANY, event->xy) :
                                         nullptr;
  const bool over_our_region = region && (region_hovered == region);
  const bool should_pass_through = !is_dragging && !over_our_region && !is_commit_key;
  if (should_pass_through) {
    return OPERATOR_PASS_THROUGH;
  }

  /* By design, this modal operator only ever ends via an explicit Enter (commit) or Esc
   * (cancel) -- no click, mouse-leaving-the-viewport, or tool switch implicitly ends it. Events
   * that don't match anything below are simply consumed like any other event while the patch is
   * live; the user must press Enter/Esc first. */

  switch (event->type) {
    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        bke::CurvesGeometry &geom = patch.control_curve;
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        Vector<PaintCurvePoint> screen_points;
        paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);

        const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};

        /* Radius handles take priority over the pivot hit test, matching #PAINTCURVE_OT_slide's
         * yield-to-slide_radius precedence (paint_curve.cc:2198-2207). */
        const int radius_hit = patch_find_radius_handle_at_pos(
            geom, screen_points.as_span(), loc_fl, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
        if (radius_hit >= 0) {
          data.active_point = radius_hit;
          data.dragging_radius = true;
          paintcurve_radius_handle_screen_get_from_geometry(
              geom, screen_points.data(), radius_hit, &data.radius_handle);
          break;
        }

        /* Tests the pivot AND both Bezier tangent handles (SEL_F1 = left, SEL_F2 = pivot,
         * SEL_F3 = right), matching #PAINTCURVE_OT_slide's own combined hit test. */
        char selflag = 0;
        const int hit = paintcurve_find_in_screen_points(
            screen_points.as_span(), loc_fl, /*ignore_pivot=*/false,
            PAINT_CURVE_POINT_SELECT_THRESHOLD, &selflag);
        if (hit < 0) {
          /* No point/handle hit -- try the curve wire itself, matching Curve Edit's
           * segment-slide affordance (#PAINTCURVE_OT_slide's `move_segment`): dragging a point on
           * the wire between two control points reshapes that segment's curvature by solving for
           * its two adjacent handles, without adding or moving any control point. */
          int segment_index = -1;
          int segment_index_next = -1;
          float edge_t = 0.0f;
          if (patch_find_closest_segment(geom,
                                         &vc,
                                         loc_fl,
                                         PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                         &segment_index,
                                         &segment_index_next,
                                         &edge_t))
          {
            data.dragging_segment = true;
            data.segment_index_a = segment_index;
            data.segment_index_b = segment_index_next;
            data.segment_t = edge_t;
            break;
          }

          /* A click that doesn't land on a point or the wire is simply not a drag start -- it
           * does not commit or otherwise end the patch. Only Enter (commit) or Esc (cancel)
           * does that. */
          break;
        }

        data.active_point = hit;
        if (selflag == SEL_F1 || selflag == SEL_F3) {
          data.dragging_handle = true;
          data.handle_is_left = (selflag == SEL_F1);
          break;
        }

        data.dragging_point = true;
        data.drag_start_mval = float2(loc_fl[0], loc_fl[1]);
        for (int h = 0; h < 3; h++) {
          data.point_initial_loc_3d[h] = paintcurve_geom_co(geom, hit, h);
        }
      }
      else if (event->val == KM_RELEASE &&
               (data.dragging_point || data.dragging_radius || data.dragging_handle ||
                data.dragging_segment))
      {
        data.dragging_point = false;
        data.dragging_radius = false;
        data.dragging_handle = false;
        data.dragging_segment = false;
        curve_patch_restore_and_restamp(*C, ob, patch);
      }
      break;
    case MOUSEMOVE:
      if (data.dragging_point) {
        bke::CurvesGeometry &geom = patch.control_curve;
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        float ob_to_world[4][4];
        float world_to_ob[4][4];
        copy_m4_m4(ob_to_world, ob.object_to_world().ptr());
        copy_m4_m4(world_to_ob, ob.world_to_object().ptr());

        /* Mirrors the "move entire point" path of #paintcurve_apply_handle_move_3d
         * (paint_curve.cc:2013-2026): re-derive the pivot's CURRENT world position every move (so
         * the win_to_3d projection plane tracks the point's actual depth) but always apply the
         * resulting delta on top of the point's ORIGINAL (drag-start) handle positions, so
         * floating-point drift never accumulates across the drag. */
        const float3 &pivot = geom.positions()[data.active_point];
        float pivot_world[3];
        mul_v3_m4v3(pivot_world, ob_to_world, pivot);

        const float mval_init[2] = {data.drag_start_mval.x, data.drag_start_mval.y};
        const float mval_curr[2] = {float(event->mval[0]), float(event->mval[1])};
        float world_init[3], world_curr[3];
        ED_view3d_win_to_3d(vc.v3d, vc.region, pivot_world, mval_init, world_init);
        ED_view3d_win_to_3d(vc.v3d, vc.region, pivot_world, mval_curr, world_curr);

        float obj_init[3], obj_curr[3], obj_delta[3];
        mul_v3_m4v3(obj_init, world_to_ob, world_init);
        mul_v3_m4v3(obj_curr, world_to_ob, world_curr);
        sub_v3_v3v3(obj_delta, obj_curr, obj_init);

        for (int h = 0; h < 3; h++) {
          add_v3_v3v3(
              paintcurve_geom_co(geom, data.active_point, h), obj_delta, data.point_initial_loc_3d[h]);
        }
        geom.calculate_bezier_auto_handles();
        geom.calculate_bezier_aligned_handles();
        geom.tag_positions_changed();

        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      else if (data.dragging_radius) {
        /* Mirrors #paintcurve_slide_radius_modal (paint_curve.cc:2689-2699): the drag axis was
         * fixed at drag-start, so every move just re-projects the current mouse position onto it. */
        const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
        const float new_radius = paintcurve_radius_from_handle_screen_pos(&data.radius_handle,
                                                                          mval_fl);
        patch.control_curve.radius_for_write()[data.active_point] = new_radius;
        patch.control_curve.tag_positions_changed();

        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      else if (data.dragging_handle) {
        bke::CurvesGeometry &geom = patch.control_curve;
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        float ob_to_world[4][4];
        float world_to_ob[4][4];
        copy_m4_m4(ob_to_world, ob.object_to_world().ptr());
        copy_m4_m4(world_to_ob, ob.world_to_object().ptr());

        /* Unlike the whole-point drag above, a directly-clicked handle simply follows the
         * cursor -- no drag-start delta needed. This matches #paintcurve_apply_handle_move_3d's
         * direct-click path (paint_curve.cc): its `pivot_screen + (mval - pivot_screen)` target
         * algebraically reduces to just `mval`, projected onto the plane through the pivot's
         * current world position. */
        const float3 &pivot = geom.positions()[data.active_point];
        float pivot_world[3];
        mul_v3_m4v3(pivot_world, ob_to_world, pivot);

        const float mval_curr[2] = {float(event->mval[0]), float(event->mval[1])};
        float world_curr[3];
        ED_view3d_win_to_3d(vc.v3d, vc.region, pivot_world, mval_curr, world_curr);
        mul_v3_m4v3(paintcurve_geom_co(geom, data.active_point, data.handle_is_left ? 0 : 2),
                    world_to_ob,
                    world_curr);

        /* A direct handle drag makes that point's shape freely adjustable. Both handle types are
         * set to Align rather than Free so the curve stays smooth (tangent-continuous) through
         * the point -- #calculate_bezier_aligned_handles() below re-aligns the OTHER handle's
         * direction while preserving its own length, the same call every other mutation in this
         * file already makes. Simpler than #PAINTCURVE_OT_slide's full Free/Vector/Auto handle-
         * type cycling, matching this editor's reduced command set. */
        geom.handle_types_left_for_write()[data.active_point] = BEZIER_HANDLE_ALIGN;
        geom.handle_types_right_for_write()[data.active_point] = BEZIER_HANDLE_ALIGN;
        geom.calculate_bezier_auto_handles();
        geom.calculate_bezier_aligned_handles();
        geom.tag_positions_changed();

        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      else if (data.dragging_segment) {
        bke::CurvesGeometry &geom = patch.control_curve;
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        float ob_to_world[4][4];
        float world_to_ob[4][4];
        copy_m4_m4(ob_to_world, ob.object_to_world().ptr());
        copy_m4_m4(world_to_ob, ob.world_to_object().ptr());

        /* Matches #paintcurve_apply_segment_move_3d's own modal caller (paint_curve.cc): the
         * unprojection plane is anchored at the segment's first endpoint, not re-derived per
         * move from anything else -- the segment's own two endpoints don't move, only their
         * shared handles do. */
        float depth_world[3];
        mul_v3_m4v3(depth_world, ob_to_world, geom.positions()[data.segment_index_a]);

        const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
        paintcurve_apply_segment_move_3d(geom,
                                         data.segment_index_a,
                                         data.segment_index_b,
                                         data.segment_t,
                                         &vc,
                                         world_to_ob,
                                         mval_fl,
                                         depth_world);
        geom.tag_positions_changed();

        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case RIGHTMOUSE:
      if (event->val == KM_PRESS && (event->modifier & KM_CTRL)) {
        bke::CurvesGeometry &geom = patch.control_curve;
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};

        bool did_insert = false;

        if (paintcurve_geometry_is_valid(geom) && geom.points_num() >= 2) {
          Vector<PaintCurvePoint> screen_points;
          paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);

          /* Do not insert when a control point is closer than the segment (matches
           * #paintcurve_try_insert_point_at_mouse, paint_curve.cc:830-839). */
          char point_selflag;
          const bool near_point = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                                    loc_fl,
                                                                    false,
                                                                    PAINT_CURVE_HOVER_THRESHOLD,
                                                                    &point_selflag) >= 0;

          if (!near_point) {
            int segment_index = -1;
            int segment_index_next = -1;
            float edge_t = 0.0f;
            const bool found_segment = patch_find_closest_segment(
                geom,
                &vc,
                loc_fl,
                PAINT_CURVE_INSERT_SEGMENT_THRESHOLD,
                &segment_index,
                &segment_index_next,
                &edge_t);

            /* Clicks near segment endpoints extend the spline instead of subdividing it (same
             * 0.1/0.9 bounds as #paintcurve_try_insert_point_at_mouse). */
            if (found_segment && edge_t >= 0.1f && edge_t <= 0.9f &&
                geom.handle_positions_right().has_value() && geom.handle_positions_left().has_value())
            {
              /* CurvePatchCache::control_curve is always a single spline (built via
               * paintcurve_geometry_init_bezier), so the owning curve is always index 0. */
              const int insert_index = paintcurve_geometry_insert_point_at_segment(
                  geom, segment_index, segment_index_next, /*active_curve=*/0, edge_t);
              if (insert_index >= 0) {
                data.active_point = insert_index;
                did_insert = true;
              }
            }
          }
        }

        if (!did_insert) {
          /* Append: extend the single spline at its end (or start it, if empty), matching
           * #paintcurve_point_add's object-space fallback path (no surface snap, no active brush
           * context to snap against here) when it is not itself inserting mid-segment. */
          float ob_origin_world[3];
          copy_v3_v3(ob_origin_world, ob.object_to_world().location());
          float world_co[3];
          ED_view3d_win_to_3d(vc.v3d, vc.region, ob_origin_world, loc_fl, world_co);
          float world_to_ob[4][4];
          copy_m4_m4(world_to_ob, ob.world_to_object().ptr());
          float obj_co[3];
          mul_v3_m4v3(obj_co, world_to_ob, world_co);

          const bool create_new_spline = (geom.points_num() == 0);
          int active_curve = 0;
          int add_index = paintcurve_geometry_is_valid(geom) ?
                              int(geom.points_by_curve()[0].size()) :
                              0;
          paintcurve_geometry_add_point(geom, float3(obj_co), create_new_spline, active_curve, add_index);
          data.active_point = geom.points_num() - 1;
        }

        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case EVT_XKEY:
    case EVT_DELKEY:
      if (event->val == KM_PRESS && data.active_point != -1) {
        bke::CurvesGeometry &geom = patch.control_curve;
        if (paintcurve_geometry_is_valid(geom) && geom.points_num() - 1 >= 2) {
          Vector<int> points_to_delete;
          points_to_delete.append(data.active_point);
          IndexMaskMemory memory;
          const IndexMask delete_mask = IndexMask::from_indices<int>(points_to_delete.as_span(),
                                                                     memory);
          paintcurve_geometry_remove_points(geom, delete_mask);
          data.active_point = -1;
          data.dragging_point = false;
          data.dragging_radius = false;
          data.dragging_handle = false;
          data.dragging_segment = false;

          curve_patch_restore_and_restamp(*C, ob, patch);
          ED_region_tag_redraw(CTX_wm_region(C));
        }
        else {
          /* Refuse rather than auto-cancelling the whole session -- the user decides explicitly
           * via Esc if they want to cancel the patch instead of losing it to an accidental
           * X/Delete press on the last removable point. */
          BKE_report(op->reports,
                    RPT_WARNING,
                    "Curve Patch needs at least 2 points -- press Esc to cancel instead");
        }
      }
      break;
    case EVT_TABKEY:
    case EVT_SKEY:
      if (event->val == KM_PRESS) {
        patch.frozen_params.swap_axis = !patch.frozen_params.swap_axis;
        curve_patch_edit_status_set(C, patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case EVT_VKEY:
      /* Sculpt Mode's default keymap binds plain V to `brush.asset_activate` (switches the
       * active `Brush` datablock, see `blender_default.py`). Unlike the navigation-only events
       * the `default:` case below lets through, that operator mutates `Sculpt::paint.brush` --
       * exactly the live brush this modal re-reads on every restamp (`curve_patch_edit_modal`'s
       * live-strength-sync block above, and `do_symmetrical_brush_actions()` ->
       * `brush_strength()`, `mesh/sculpt.cc`). Letting a brush swap slip through here crashed the
       * very next restamp on a freshly-activated brush whose `curve_strength` CurveMapping had
       * never been initialized (`BKE_curvemapping_evaluateF()` dereferencing a null `curve_strength`,
       * `mesh/sculpt.cc:2340`). Swallow it unconditionally rather than acting on it -- Curve Patch
       * has no V-bound action of its own. */
      break;
    case EVT_RETKEY:
    case EVT_PADENTER:
      if (event->val == KM_PRESS) {
        curve_patch_edit_finish(C, op, false);
        return OPERATOR_FINISHED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        curve_patch_edit_finish(C, op, true);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      /* Anything we don't explicitly recognise here -- middle-mouse orbit, wheel/trackpad zoom,
       * NDOF motion, numpad view keys, etc. -- is not something Curve Patch editing cares about,
       * so let it fall through to the ordinary View3D navigation keymap underneath instead of
       * blocking it.
       *
       * Deliberately a BARE `OPERATOR_PASS_THROUGH`, NOT combined with `OPERATOR_RUNNING_MODAL`:
       * for an operator whose modal() is already running (as opposed to a fresh keymap-triggered
       * invoke), `wm_handler_operator_call()` (wm_event_system.cc) translates the combination
       * `OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH` to `WM_HANDLER_BREAK | WM_HANDLER_MODAL`
       * -- the BREAK bit still stops `wm_event_do_handlers()` from ever reaching area/region
       * handling for this event (verified: navigation stayed blocked with that combination). A
       * BARE `OPERATOR_PASS_THROUGH` translates to plain `WM_HANDLER_CONTINUE` (no BREAK) instead,
       * which does let area/region handling proceed, while the handler stays registered/alive
       * regardless (removal is gated on `OPERATOR_CANCELLED | OPERATOR_FINISHED`, neither of which
       * this sets). This exactly matches the reference idiom for "long-lived modal that must not
       * block view navigation" -- see `editmesh_knife.cc`'s `WHEELUPMOUSE`/`WHEELDOWNMOUSE`/
       * `MOUSEPAN`/`MOUSEZOOM`/`MOUSEROTATE`/`NDOF_MOTION` cases, which return bare
       * `OPERATOR_PASS_THROUGH` for exactly this reason (knife tool famously still lets you orbit
       * mid-cut). LEFTMOUSE, RIGHTMOUSE, MOUSEMOVE and the explicit hotkeys above are still fully
       * consumed even on a miss (see the case bodies, all ending in a plain `break;`), so a stray
       * click still cannot leak through to start a brush stroke on the mesh. */
      return OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void curve_patch_edit_finish(bContext *C, wmOperator *op, const bool is_cancel)
{
  ED_workspace_status_text(C, nullptr);

  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  CurvePatchCache *patch = ss.curve_patch_cache;

  if (patch) {
    if (is_cancel) {
      curve_patch_restore_only(ob, *patch);
      /* Undo transaction opened by the anchor stroke's `stroke_undo_begin()` is deliberately
       * never closed here -- matches the existing cancelled-stroke idiom at
       * `mesh/sculpt.cc:SculptPaintStroke::done()` (`if (!is_cancel && stroke_started) {
       * stroke_undo_end(...); }`): an unclosed `push_begin_ex` transaction is simply discarded,
       * nothing is written to the undo stack. */
    }
    else {
      /* Re-stamp once at final quality before closing the undo step, so the mesh (and the undo
       * history) keeps the smoothed profile rather than the harder interactive preview. See
       * `docs/superpowers/specs/2026-07-18-curve-patch-final-quality-design.md`. */
      patch->final_quality = true;
      curve_patch_restore_and_restamp(*C, ob, *patch);
      patch->final_quality = false;
      undo::push_end(ob);

      /* The re-stamp ends in `flush_update_step()`, which only arms the fast paint-redraw path; that
       * is torn down the instant this operator finishes. Issue the full finished-stroke redraw so
       * the committed positions actually reach the screen -- same reasoning as the initial preview
       * stamp in `curve_patch_publish_and_launch_modal()`. */
      flush_update_done(C, ob, UpdateType::Position);
    }
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    MEM_delete(patch);
    ss.curve_patch_cache = nullptr;
  }

  CurvePatchEditOpData *op_data = static_cast<CurvePatchEditOpData *>(op->customdata);
  if (op_data && op_data->sync_timer) {
    WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), op_data->sync_timer);
  }
  MEM_delete(op_data);
  op->customdata = nullptr;
}

static void curve_patch_edit_cancel(bContext *C, wmOperator *op)
{
  curve_patch_edit_finish(C, op, true);
}

void SCULPT_OT_curve_patch_edit(wmOperatorType *ot)
{
  ot->name = "Curve Patch Edit";
  ot->idname = "SCULPT_OT_curve_patch_edit";
  ot->description = "Edit the live control curve of a Curve Patch stroke";

  ot->invoke = curve_patch_edit_invoke;
  ot->modal = curve_patch_edit_modal;
  ot->cancel = curve_patch_edit_cancel;
  ot->poll = curve_patch_edit_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

}  // namespace blender::ed::sculpt_paint
