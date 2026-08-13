/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
 * `paintcurve_slide_radius` (`paint_curve.cc`), adapted to operate on `CurvePatchEditState::control_curve`
 * -- a standalone `bke::CurvesGeometry` that is not wrapped in a `PaintCurve` ID. The segment
 * hit-test helpers (`paintcurve_build_screen_segment_polyline_from_geometry`,
 * `paintcurve_bezier_param_at_screen_pos_on_segment_from_geometry`, etc., `paint_curve_sync.cc`/
 * `paint_curve_geometry.cc`) are the `_from_geometry` cores of
 * `paintcurve_build_screen_segment_polyline`/`paintcurve_bezier_param_at_screen_pos_on_segment`,
 * shared with the Stage 06 overlay draw path (`ED_paint_curve_screen_handles_build_from_geometry`,
 * `paint_curve_draw.cc`).
 */

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <optional>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_bit_span_ops.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "DNA_brush_types.h"
#include "DNA_color_types.h"
#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_texture_types.h"
#include "DNA_workspace_types.h"

#include "BLT_translation.hh"

#include "ED_curves.hh"
#include "ED_paint.hh"
#include "ED_paint_curve_draw.hh"
#include "ED_screen.hh"
#include "ED_util_modal_multiwin.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "mesh/sculpt_intern.hh"
#include "paint_curve_intern.hh"
#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint {

/* Cheap order-sensitive digest of a brush's Curve Patch texture list, used only to detect that the
 * list changed and the patch must be re-stamped. Not a hash with any security or collision
 * guarantee -- a missed change would merely delay a re-stamp until the next edit. */
static uint64_t curve_patch_texture_list_digest(const Brush &brush)
{
  uint64_t digest = 1469598103934665603ull;
  /* Range-for, NOT `LISTBASE_FOREACH`: that macro is private to `listbase.cc` in this branch.
   * `ListBaseT<T>` iterates directly -- see `for (PaletteColor &color : palette->colors)` in
   * `blenkernel/intern/paint.cc`. */
  for (const BrushCurvePatchTextureSlot &slot : brush.curve_patch.texture_slots) {
    digest = (digest ^ uint64_t(uintptr_t(slot.tex))) * 1099511628211ull;
    uint32_t weight_bits;
    memcpy(&weight_bits, &slot.weight, sizeof(weight_bits));
    digest = (digest ^ uint64_t(weight_bits)) * 1099511628211ull;
  }
  return digest;
}

/* Pointer-only counterpart to the digest above, folding in each slot's `tex` pointer but not its
 * weight. A weight-only edit re-stamps (it changes `curve_patch_texture_list_digest()`) but does
 * not change WHICH images are sampled, so it must not by itself invalidate the session's texture
 * pool -- this narrower digest is what the pool-rebuild gate watches instead. Same combinator as
 * above, so a pure slot reorder still changes it. */
static uint64_t curve_patch_texture_pointer_digest(const Brush &brush)
{
  uint64_t digest = 1469598103934665603ull;
  for (const BrushCurvePatchTextureSlot &slot : brush.curve_patch.texture_slots) {
    digest = (digest ^ uint64_t(uintptr_t(slot.tex))) * 1099511628211ull;
  }
  return digest;
}

/* NOTE: the active point index lives on `CurvePatchSession`, not here -- the context menu's operators
 * cannot reach a running modal's `op->customdata`. See `CurvePatchEditState::active_point`. */
struct CurvePatchEditOpData {
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
   * live (not frozen -- see `CurvePatchSession::params`), so `curve_patch_edit_modal()` compares
   * the current `BKE_brush_alpha_get()` against this every event and re-stamps when it changes,
   * making a slider drag update the relief in real time. Seeded at invoke to the current value so
   * the first event does not re-stamp spuriously. */
  float last_synced_alpha = -1.0f;
  /* Last brush direction (Add vs Subtract, `BRUSH_DIR_IN`) this modal re-stamped with. Like the
   * strength above, the direction is read live -- `brush_strength()` folds it in via
   * `brush_flip()`, so `cache.bstrength` (and thus the relief's sign) flips with the brush's
   * Add/Subtract toggle. Watched here so toggling it re-stamps in real time. Seeded at invoke. */
  bool last_synced_dir_in = false;
  /* Last mesh mirror symmetry this modal re-stamped with. Read live so toggling Mirror X/Y/Z
   * re-projects immediately; it is applied by `do_symmetrical_brush_actions()` inside the re-stamp
   * itself, so this only has to TRIGGER one. Seeded at invoke.
   *
   * Everything the brush contributes to #bke::CurvePatchParams -- Length mode/Repeats, end falloff,
   * brush Size, the Stamps mode/projection/randomization, Spacing, Jitter and the stamp rotation --
   * needs no watchdog of its own: the poll rebuilds the whole parameter block through
   * #curve_patch_params_from_brush and compares it against the session's copy in one shot. Each of
   * those used to be a hand-written pair of fields here that every new parameter had to remember to
   * extend. */
  int last_synced_symm = -1;
  /* Multi-texture watch. The list's contents have no single scalar to compare, so two cheap
   * digests stand in for one: `last_synced_texture_list_digest` folds in both slot pointers and
   * weights and drives the re-stamp trigger (`multi_texture_changed`); the narrower
   * `last_synced_texture_pointer_digest` folds in only the pointers and drives the `ImagePool`
   * rebuild (`texture_identity_changed`) below, which only needs to notice a change to WHICH
   * images are sampled, not how they are weighted. */
  int last_synced_stamp_tex_source = -1;
  int last_synced_ribbon_tex_source = -1;
  float last_synced_cap_start_length = -1.0f;
  float last_synced_cap_end_length = -1.0f;
  uint64_t last_synced_texture_list_digest = 0;
  uint64_t last_synced_texture_pointer_digest = 0;
  const void *last_synced_cap_tex[3] = {nullptr, nullptr, nullptr};
  /* Last `BrushCurvePatchSettings::swap_axis` this modal saw ON THE BRUSH -- and the one watchdog
   * that
   * is not merely a re-stamp trigger. `CurvePatchParams::swap_axis` is owned by the session as much
   * as by the brush: the S hotkey and the session undo stack both write it without touching the
   * brush, so the poll may only push the brush's value across when the BRUSH itself changed. That
   * question cannot be answered from the two current values alone, which is why this one field
   * survives the collapse into #curve_patch_params_from_brush. Seeded at invoke. */
  bool last_synced_swap_axis = false;
  /* Last texture and texture-mapping state this modal re-stamped with. The relief already reads
   * `brush.mtex` live on every re-stamp (size/offset/`tex`), so these only need a re-stamp TRIGGER:
   * when the assigned texture or its mapping size/offset changes, re-project. Edits to the texture
   * DATABLOCK itself (procedural params, image swap, etc.) are caught via the
   * `BKE_paint_get_overlay_texture_edit_count()` monotonic counter below -- the brush texture is
   * not in the depsgraph, so `Tex_Runtime::last_update` on the original never bumps and cannot be
   * used here. */
  float last_synced_tex_size[2] = {0.0f, 0.0f};
  float last_synced_tex_ofs[2] = {0.0f, 0.0f};
  const void *last_synced_tex = nullptr;
  uint64_t last_synced_tex_edit_count = 0;
  /* Last brush primary color this modal re-stamped with. The color/image effects read
   * `BKE_brush_color_get()` live on every re-stamp, but unlike strength or texture mapping there is
   * no field in #bke::CurvePatchParams to compare -- a color-picker drag would otherwise leave the
   * patch frozen until some unrelated brush setting changed. */
  float3 last_synced_brush_color = float3(-1.0f);
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
  /* The brush datablock this patch was built from, captured at invoke. Compared against the live
   * `Sculpt::paint.brush` on every modal tick to notice that the user reached for another tool or
   * brush -- see the check at the top of `curve_patch_edit_modal()` for why that has to end the
   * session, and why the commit it performs has to put this pointer back first. Compared for
   * IDENTITY only; never dereferenced while stale. */
  Brush *brush_at_invoke = nullptr;
  /* `bToolRef::idname` of the tool active in this patch's viewport at invoke, watched alongside
   * `brush_at_invoke` for the same reason. Needed on its own because the tools that carry no brush
   * -- Move/Rotate/Scale/Transform, the filters, Trim, Line Project -- leave `Paint::brush`
   * untouched, so the brush comparison alone cannot see a switch to one of them.
   *
   * Compared as a STRING, not by `bToolRef` identity: #WM_toolsystem_ref_set_from_runtime reuses
   * the same `bToolRef` and overwrites `idname` in place (`wm_toolsystem.cc`), so the pointer is
   * unchanged across a switch. Empty when no tool was active at invoke, which disables the check. */
  char tool_idname_at_invoke[64] = "";
  /* Snap context reused for the duration of a point drag, mirroring `PointSlideData::snap_ctx`
   * (paint_curve.cc). Only the scene-snap-element level of #paintcurve_surface_place needs it, so
   * it is created lazily on the first MOUSEMOVE that reaches that level and freed when the drag
   * ends -- creating one per mouse move would rebuild the snap BVH on every event. */
  PaintCurveSnapContext *snap_ctx = nullptr;
};

/** Release the drag-scoped snap context, if one was created. Safe to call repeatedly. */
static void curve_patch_edit_snap_ctx_free(CurvePatchEditOpData &data)
{
  if (data.snap_ctx != nullptr) {
    ED_paintcurve_snap_context_destroy(data.snap_ctx);
    data.snap_ctx = nullptr;
  }
}

static CurvePatchSession &patch_cache_of(bContext *C)
{
  Object &ob = *CTX_data_active_object(C);
  return *ob.runtime->sculpt_session->curve_patch_session;
}

/* `sculpt_mode_poll()` alone only confirms Sculpt Mode is active -- it says nothing about whether
 * a live Curve Patch exists. Without this extra check, this operator is directly invocable (F3
 * search, a stray keymap entry, `bpy.ops.sculpt.curve_patch_edit()`) any time the user is in
 * Sculpt Mode, independent of whether Stage 05's anchor-stroke flow has ever started one --
 * `patch_cache_of()` would then dereference a null `curve_patch_session` on the very first event. */
static bool curve_patch_edit_poll(bContext *C)
{
  if (!sculpt_mode_poll(C)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->runtime->sculpt_session && ob->runtime->sculpt_session->curve_patch_session;
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
                                       float *r_edge_t,
                                       float *r_dist_sq = nullptr)
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
    paintcurve_build_screen_segment_polyline_from_geometry(
        geom, true, vc, point_a, point_b, {}, polyline);
    const float dist_sq = ED_paint_curve_polyline_distance_sq(polyline, mval);
    if (dist_sq < best_dist_sq) {
      float bezier_t = 0.0f;
      if (paintcurve_bezier_param_at_screen_pos_on_segment_from_geometry(
              vc, geom, true, pos, point_a, point_b, {}, bezier_t))
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
  if (r_dist_sq) {
    *r_dist_sq = best_dist_sq;
  }
  return true;
}

/** Control point under `pos` for X-to-delete, mirroring the combined hit tests in the modal's
 * `LEFTMOUSE` press handler (radius widget, pivot/handles, then segment wire). Returns -1 when
 * nothing is hovered -- in that case X must reach `PAINT_OT_brush_colors_flip` (and, during an
 * active stencil RMB-drag, `BRUSH_OT_stencil_control`'s axis constraints). */
static int patch_find_point_index_at_pos_for_delete(const bke::CurvesGeometry &geom,
                                                    const ViewContext *vc,
                                                    const float pos[2])
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return -1;
  }

  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points_from_geometry(geom, true, vc, screen_points);

  const int radius_hit = patch_find_radius_handle_at_pos(
      geom, screen_points.as_span(), pos, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
  if (radius_hit >= 0) {
    return radius_hit;
  }

  char selflag = 0;
  const int hit = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                   pos,
                                                   /*ignore_pivot=*/false,
                                                   PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                   &selflag);
  if (hit >= 0) {
    return hit;
  }

  int segment_index = -1;
  int segment_index_next = -1;
  float edge_t = 0.0f;
  if (patch_find_closest_segment(geom,
                                 vc,
                                 pos,
                                 PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                 &segment_index,
                                 &segment_index_next,
                                 &edge_t))
  {
    return edge_t < 0.5f ? segment_index : segment_index_next;
  }

  return -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multi-Viewport Context
 * \{ */

/** Point the session's owned `ViewContext` (and `StrokeCache::vc`) at the viewport under edit. */
static void curve_patch_sync_view_context(bContext *C,
                                          ScrArea *area,
                                          ARegion *region,
                                          CurvePatchSession &patch)
{
  if (!area || !region) {
    return;
  }
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, region);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  patch.view_context = ED_view3d_viewcontext_init(C, depsgraph);
  Object *ob = CTX_data_active_object(C);
  if (ob && ob->runtime->sculpt_session && ob->runtime->sculpt_session->cache) {
    ob->runtime->sculpt_session->cache->vc = &patch.view_context;
  }
}

/**
 * Called from outside the modal (the overlay-redraw poll) to catch windows opened after the
 * session started -- `curve_patch_edit_modal()` re-runs
 * #WM_event_add_modal_handler_all_windows() itself on every tick too, but this covers the gap
 * before the next event reaches it. Discovers the running #SCULPT_OT_curve_patch_edit instance
 * by type: there is always at most one active session, so a type-based search (unlike an
 * instance-match idempotency check) is the right tool here -- there is no already-known
 * `wmOperator *` to compare against yet.
 */
void ED_paint_curve_patch_modal_handlers_ensure(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (!ob || !ob->runtime->sculpt_session || !ob->runtime->sculpt_session->curve_patch_session) {
    return;
  }
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm) {
    return;
  }
  const wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_curve_patch_edit", false);
  if (!ot) {
    return;
  }
  wmOperator *op = nullptr;
  for (wmWindow &win : wm->windows) {
    op = WM_operator_find_modal_by_type(&win, ot);
    if (op) {
      break;
    }
  }
  if (!op) {
    return;
  }
  WM_event_add_modal_handler_all_windows(C, op, SPACE_VIEW3D, RGN_TYPE_WINDOW);
}

/** \} */

/* Status-bar hint, refreshed on invoke and every axis toggle. Doubles as a visible signal that
 * an event actually reached this modal handler -- if the text never appears/updates, the
 * event isn't getting here at all (as opposed to it arriving but the texture effect being hard
 * to see for a given brush/texture setup). */
/* -------------------------------------------------------------------- */
/** \name Shared Active-Point Actions
 *
 * Bodies shared by the modal's own hotkeys and by the context-menu operators further down, so the
 * two entry points cannot drift apart.
 * \{ */

/* Defined below; the cyclic toggle refreshes the status bar, which spells out what C will do next. */
static void curve_patch_edit_status_set(bContext *C, const CurvePatchSession &patch);

/** Refresh the paint-curve overlay in every 3D View / Image editor (cheap; per MOUSEMOVE frame). */
static void curve_patch_tag_overlay_redraw_all(bContext *C)
{
  ED_paint_curve_overlay_tag_redraw_all(C);
}

/** Overlay plus the finished-stroke redraw handshake after a discrete edit (drag release, undo,
 * delete, etc.). Always issues the shared #UpdateType::Position flush; Image/Color effects also
 * need their own type so #flush_update_done reaches the #SPACE_IMAGE loop and the paint-done
 * handshake -- the same reason #curve_patch_finish_commit issues a second effect-typed flush.
 * Without that second call, Texture Paint (Draw brush / image canvas) keeps a stale GPU preview
 * after moving points or segments even though the ImBuf was already re-stamped. */
static void curve_patch_tag_viewports_redraw_after_edit(bContext &C,
                                                       Object &ob,
                                                       const CurvePatchSession &patch)
{
  ED_paint_curve_overlay_tag_redraw_all(&C);
  flush_update_done(&C, ob, UpdateType::Position);
  if (!patch.effect) {
    return;
  }
  const UpdateType update_type = patch.effect->update_type();
  if (update_type != UpdateType::Position && bits::any_bit_set(patch.apply.all_touched_nodes)) {
    flush_update_done(&C, ob, update_type);
  }
}

/* -------------------------------------------------------------------- */
/** \name Session-Local Undo
 *
 * See `CurvePatchEditState::undo_steps` for why this cannot go through Blender's own undo systems.
 * \{ */

/* Deep enough for any realistic editing session; the snapshots are a handful of control points
 * each, so the cap exists to bound a pathological session rather than to save meaningful memory. */
static constexpr int CURVE_PATCH_UNDO_STEPS_MAX = 64;

/* Record the CURRENT state as a new step. Called after an action completes -- once per action, not
 * once per event, so a drag is a single step. */
static void curve_patch_undo_push(CurvePatchSession &patch)
{
  /* Anything above the cursor is a redo branch the new edit invalidates. */
  patch.edit.undo_steps.resize(patch.edit.undo_step_current + 1);

  CurvePatchEditStep step;
  step.items.reserve(patch.patches.size());
  for (const CurvePatchItem &item : patch.patches) {
    step.items.append({item.control_curve, item.params.swap_axis, item.params.stamp_seed});
  }
  step.active_patch = patch.active_patch;
  patch.edit.undo_steps.append(std::move(step));

  if (patch.edit.undo_steps.size() > CURVE_PATCH_UNDO_STEPS_MAX) {
    patch.edit.undo_steps.remove(0);
  }
  patch.edit.undo_step_current = int(patch.edit.undo_steps.size()) - 1;
}

static void curve_patch_undo_restore(bContext &C, Object &ob, CurvePatchSession &patch)
{
  const CurvePatchEditStep &step = patch.edit.undo_steps[patch.edit.undo_step_current];
  /* Resized rather than rebuilt: only the three snapshotted fields are restored, so each patch
   * keeps its surface snapshot and its derived geometry, which the next re-stamp overwrites
   * anyway. A step can hold fewer patches than the session currently has. */
  patch.patches.resize(step.items.size());
  for (const int i : step.items.index_range()) {
    patch.patches[i].control_curve = step.items[i].curve;
    patch.patches[i].params.swap_axis = step.items[i].swap_axis;
    patch.patches[i].params.stamp_seed = step.items[i].stamp_seed;
  }
  patch.active_patch = std::min(step.active_patch, int(patch.patches.size()) - 1);
  /* The restored curve may hold fewer points than the one just replaced. */
  patch.edit.active_point = -1;

  curve_patch_edit_status_set(&C, patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  curve_patch_tag_viewports_redraw_after_edit(C, ob, patch);
}

/* Returns false when there is nothing left to undo, i.e. the session is back at the state the
 * anchor stroke produced. The caller then cancels the patch outright. */
static bool curve_patch_undo_step_back(bContext &C, Object &ob, CurvePatchSession &patch)
{
  if (patch.edit.undo_step_current <= 0) {
    return false;
  }
  patch.edit.undo_step_current--;
  curve_patch_undo_restore(C, ob, patch);
  return true;
}

static void curve_patch_undo_step_forward(bContext &C, Object &ob, CurvePatchSession &patch)
{
  if (patch.edit.undo_step_current + 1 >= int(patch.edit.undo_steps.size())) {
    return;
  }
  patch.edit.undo_step_current++;
  curve_patch_undo_restore(C, ob, patch);
}

/** \} */

static bool curve_patch_active_point_is_valid(const CurvePatchSession &patch)
{
  const bke::CurvesGeometry &geom = patch.active_item().control_curve;
  return patch.edit.active_point >= 0 && paintcurve_geometry_is_valid(geom) &&
         patch.edit.active_point < geom.points_num();
}

/* Returns false (having reported why) when the point cannot be removed. */
static bool curve_patch_delete_active_point(bContext &C,
                                            Object &ob,
                                            CurvePatchSession &patch,
                                            ReportList *reports)
{
  if (!curve_patch_active_point_is_valid(patch)) {
    return false;
  }
  bke::CurvesGeometry &geom = patch.active_item().control_curve;
  /* Two points is the floor whether the curve is open or closed -- a 2-point cyclic Bezier is a
   * perfectly good loop (see #curve_patch_toggle_cyclic). */
  if (geom.points_num() - 1 < 2) {
    /* Refuse rather than auto-cancelling the whole session -- the user decides explicitly via Esc
     * if they want to cancel the patch instead of losing it to an accidental X/Delete press on the
     * last removable point. */
    BKE_report(
        reports, RPT_WARNING, "Curve Patch needs at least 2 points -- press Esc to cancel instead");
    return false;
  }

  const int point = patch.edit.active_point;
  IndexMaskMemory memory;
  const IndexMask delete_mask = IndexMask::from_indices<int>(Span<int>(&point, 1), memory);
  paintcurve_geometry_remove_points(geom, delete_mask);
  patch.edit.active_point = -1;

  curve_patch_undo_push(patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  curve_patch_tag_viewports_redraw_after_edit(C, ob, patch);
  return true;
}

static void curve_patch_set_active_handle_type(bContext &C,
                                               Object &ob,
                                               CurvePatchSession &patch,
                                               const ed::curves::SetHandleType dst_type)
{
  bke::CurvesGeometry &geom = patch.active_item().control_curve;
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  types_left[patch.edit.active_point] = paintcurve_resolve_handle_type(types_left[patch.edit.active_point],
                                                                  dst_type);
  types_right[patch.edit.active_point] = paintcurve_resolve_handle_type(types_right[patch.edit.active_point],
                                                                   dst_type);
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();

  curve_patch_undo_push(patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  curve_patch_tag_viewports_redraw_after_edit(C, ob, patch);
}

/* Close or re-open the control curve. Returns false only when there is no usable curve at all. */
static bool curve_patch_toggle_cyclic(bContext &C, Object &ob, CurvePatchSession &patch)
{
  bke::CurvesGeometry &geom = patch.active_item().control_curve;
  if (!paintcurve_geometry_is_valid(geom) || geom.curves_num() == 0) {
    return false;
  }
  /* Two points are enough, as everywhere else in Blender: a cyclic Bezier of N points has N
   * segments (#bke::curves::segments_num), and the two a 2-point loop produces are distinct curves
   * -- one runs through point 0's right handle into point 1's left, the other through point 1's
   * right back into point 0's left -- so they bow to opposite sides and enclose a real shape. Only
   * collinear handles degenerate that, which is equally true of three collinear points and is the
   * user's business, not a reason to refuse. */
  const bool was_cyclic = geom.cyclic()[0];
  geom.cyclic_for_write().first() = !was_cyclic;
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_topology_changed();

  curve_patch_undo_push(patch);
  curve_patch_edit_status_set(&C, patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  curve_patch_tag_viewports_redraw_after_edit(C, ob, patch);
  return true;
}

/* Roll a new random layout for the Stamps-mode relief. Returns false in Ribbon mode, which has no
 * randomization for a seed to drive. */
static bool curve_patch_reseed_stamps(bContext &C, Object &ob, CurvePatchSession &patch)
{
  if (patch.active_item().params.stamp_mode != CurvePatchStampMode::Stamps) {
    return false;
  }
  /* Alongside `curve_patch_begin_editing()` the only place a stateful RNG is touched: every
   * per-stamp offset downstream is a pure hash of this seed, so re-rolling it here is what makes
   * the whole layout change while every other input stays put. */
  patch.active_item().params.stamp_seed = RandomNumberGenerator::from_random_seed().get_uint32();

  curve_patch_undo_push(patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  curve_patch_tag_viewports_redraw_after_edit(C, ob, patch);
  return true;
}

static bool curve_patch_is_cyclic(const CurvePatchSession &patch)
{
  const bke::CurvesGeometry &geom = patch.active_item().control_curve;
  return paintcurve_geometry_is_valid(geom) && geom.curves_num() > 0 && geom.cyclic()[0];
}

/* Right-click menu over a control point. The two point actions go through real operators (declared
 * below) because a popup can only invoke operators, never call back into the running modal; the
 * remaining entries are plain brush properties, which need no operator at all -- the modal's
 * live-sync poll notices the changed `mtex` values and re-stamps on its next tick. */
static void curve_patch_edit_context_menu_open(bContext *C)
{
  ui::PopupMenu *pup = ui::popup_menu_begin(C, IFACE_("Curve Patch"), ICON_NONE);
  ui::Layout &layout = *ui::popup_menu_layout(pup);
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);
  layout.op_menu_enum(
      C, "SCULPT_OT_curve_patch_handle_type_set", "type", IFACE_("Handle Type"), ICON_NONE);

  const CurvePatchSession &patch = patch_cache_of(C);
  const bool is_cyclic = curve_patch_is_cyclic(patch);
  layout.separator();
  layout.op("SCULPT_OT_curve_patch_toggle_cyclic",
            is_cyclic ? IFACE_("Open Curve") : IFACE_("Close Curve"),
            ICON_NONE);

  /* Ribbon mode has no randomization, so the entry would poll false and only ever show greyed
   * out -- leave it out entirely there. */
  if (patch.active_item().params.stamp_mode == CurvePatchStampMode::Stamps) {
    layout.op("SCULPT_OT_curve_patch_stamp_reseed", std::nullopt, ICON_NONE);
  }

  const ToolSettings *tool_settings = CTX_data_tool_settings(C);
  Brush *brush = (tool_settings && tool_settings->sculpt) ?
                     BKE_paint_brush(&tool_settings->sculpt->paint) :
                     nullptr;
  if (brush != nullptr) {
    /* These identifiers are strings the compiler cannot check: they must track
     * #rna_def_brush_curve_patch_settings, and the pointer must be the settings block itself. */
    PointerRNA settings_ptr = RNA_pointer_create_discrete(
        &brush->id, RNA_BrushCurvePatchSettings, &brush->curve_patch);
    layout.separator();
    /* A closed curve has no ends to fade, and the relief ignores the setting there -- so do not
     * offer it. The brush panel still shows it: it has no access to the live patch, and the setting
     * remains meaningful for every open one. */
    if (!is_cyclic) {
      /* Enum props in popup menus must use #Layout::prop_menu_enum, not #Layout::prop: the
       * string overload passes #RNA_NO_INDEX, which routes enums through #item_with_label and
       * leaves later items in the value column (see the Curve Patch context menu layout). */
      PropertyRNA *end_falloff_prop = RNA_struct_find_property(&settings_ptr, "end_falloff");
      layout.prop_menu_enum(&settings_ptr, end_falloff_prop, IFACE_("End Falloff"), ICON_NONE);
      if (brush->curve_patch.end_falloff == BRUSH_CURVE_PATCH_END_SMOOTH) {
        layout.prop(&settings_ptr,
                    "end_falloff_percent",
                    UI_ITEM_NONE,
                    IFACE_("Falloff Length"),
                    ICON_NONE);
      }
    }
    layout.prop(
        &settings_ptr, "use_swap_axis", UI_ITEM_NONE, IFACE_("Swap Texture Axis"), ICON_NONE);
  }

  layout.separator();
  layout.op("SCULPT_OT_curve_patch_delete_point", std::nullopt, ICON_NONE);
  ui::popup_menu_end(C, pup);
}

/** \} */

/* The keys are still matched by name in the modal below rather than through a modal keymap, so
 * #WorkspaceStatus::opmodal (which reads one) cannot be used -- each entry names its key icon
 * directly. The gain over the raw status string this replaced is the key ICONS and the translation
 * of every label. */
static void curve_patch_edit_status_set(bContext *C, const CurvePatchSession &patch)
{
  WorkspaceStatus status(C);
  status.item(IFACE_("Commit"), ICON_EVENT_RETURN);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
  status.item(IFACE_("Undo"), ICON_EVENT_CTRL, ICON_EVENT_Z);
  /* Two whole strings rather than one with a substituted axis letter: a translator needs the
   * sentence, and neither half reads as a sentence on its own. */
  status.item(patch.active_item().params.swap_axis ? IFACE_("Swap Texture Axis (now U)") :
                                       IFACE_("Swap Texture Axis (now V)"),
              ICON_EVENT_Y);
  status.item(curve_patch_is_cyclic(patch) ? IFACE_("Open Curve") : IFACE_("Close Curve"),
              ICON_EVENT_C);
  /* Reseed has no shortcut -- this modal has no free key left -- so advertise the route that does
   * reach it. Meaningless in Ribbon mode, which has nothing random to re-roll. */
  if (patch.active_item().params.stamp_mode == CurvePatchStampMode::Stamps) {
    status.item(IFACE_("Reseed Stamps"), ICON_MOUSE_RMB);
  }
}

static wmOperatorStatus curve_patch_edit_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  CurvePatchEditOpData *data = MEM_new<CurvePatchEditOpData>(__func__);
  op->customdata = data;
  /* Seed the live-strength watchdog with the current slider value so the first modal event does
   * not read a "change" against the default and re-stamp needlessly (see
   * `CurvePatchEditOpData::last_synced_alpha`). */
  ToolSettings *tool_settings = CTX_data_tool_settings(C);
  if (tool_settings && tool_settings->sculpt) {
    Sculpt &sd = *tool_settings->sculpt;
    /* Non-const so `brush_at_invoke` can be handed straight back to `Paint::brush` when the modal
     * commits after a tool switch (see `curve_patch_edit_modal()`). */
    if (Brush *brush = BKE_paint_brush(&sd.paint)) {
      data->brush_at_invoke = brush;
      data->last_synced_alpha = BKE_brush_alpha_get(&sd.paint, brush);
      data->last_synced_dir_in = (brush->flag & BRUSH_DIR_IN) != 0;
      data->last_synced_stamp_tex_source = brush->curve_patch.stamp_texture_source;
      data->last_synced_ribbon_tex_source = brush->curve_patch.ribbon_texture_source;
      data->last_synced_cap_start_length = brush->curve_patch.cap_start_length;
      data->last_synced_cap_end_length = brush->curve_patch.cap_end_length;
      data->last_synced_texture_list_digest = curve_patch_texture_list_digest(*brush);
      data->last_synced_texture_pointer_digest = curve_patch_texture_pointer_digest(*brush);
      data->last_synced_cap_tex[0] = brush->curve_patch.tex_start;
      data->last_synced_cap_tex[1] = brush->curve_patch.tex_middle;
      data->last_synced_cap_tex[2] = brush->curve_patch.tex_end;
      data->last_synced_swap_axis = brush->curve_patch.swap_axis != 0;
      data->last_synced_tex_size[0] = brush->mtex.size[0];
      data->last_synced_tex_size[1] = brush->mtex.size[1];
      data->last_synced_tex_ofs[0] = brush->mtex.ofs[0];
      data->last_synced_tex_ofs[1] = brush->mtex.ofs[1];
      data->last_synced_tex = brush->mtex.tex;
      data->last_synced_tex_edit_count = BKE_paint_get_overlay_texture_edit_count();
      data->last_synced_brush_color = BKE_brush_color_get(&sd.paint, brush);
      data->last_synced_falloff_preset = brush->curve_distance_falloff_preset;
      data->last_synced_falloff_curve_ts = brush->curve_distance_falloff ?
                                               brush->curve_distance_falloff->changed_timestamp :
                                               0;
    }
  }
  if (const Object *ob = CTX_data_active_object(C)) {
    data->last_synced_symm = mesh_symmetry_xyz_get(*ob);
  }
  /* Snapshot the active tool of the viewport this patch belongs to (see
   * `CurvePatchEditOpData::tool_idname_at_invoke`). `CTX_wm_area()` is that viewport both here and
   * in the modal, which reads the frozen area captured at this moment. */
  if (const ScrArea *area = CTX_wm_area(C)) {
    if (const bToolRef *tref = area->runtime.tool) {
      STRNCPY(data->tool_idname_at_invoke, tref->idname);
    }
  }
  WM_event_add_modal_handler_all_windows(C, op, SPACE_VIEW3D, RGN_TYPE_WINDOW);
  /* Drive the live-sync poll at a steady cadence (see `CurvePatchEditOpData::sync_timer`). 20 Hz:
   * fast enough that a panel change feels immediate, light enough that idle ticks -- which only
   * compare scalars and re-stamp on an actual change -- cost nothing. */
  const double sync_timer_step = 0.05;
  data->sync_timer = WM_event_timer_add(
      CTX_wm_manager(C), CTX_wm_window(C), TIMER, sync_timer_step);
  CurvePatchSession &patch = patch_cache_of(C);
  /* Seed the session undo stack with the state the anchor stroke produced. Ctrl+Z walks back to
   * this entry and, once there, cancels the patch instead of stepping further. */
  patch.edit.undo_steps.clear();
  patch.edit.undo_step_current = -1;
  curve_patch_undo_push(patch);
  curve_patch_edit_status_set(C, patch);
  return OPERATOR_RUNNING_MODAL;
}

/* Returns true when the patch was actually committed. A commit request can still end up writing
 * nothing -- see the `CurvePatchApplyState::invalidated` branch inside -- and the modal reports that as
 * `OPERATOR_CANCELLED`. */
static bool curve_patch_edit_finish(bContext *C, wmOperator *op, const bool is_cancel);

/**
 * True once the tool or the brush this patch was started with is no longer the active one, which
 * means the session has been superseded and must end (see the caller for why it commits).
 *
 * Two independent axes, because neither subsumes the other:
 *
 * - The BRUSH. Switching to another brush-based tool, the asset shelf, `brush.asset_activate` (V)
 *   and Python all reassign `Sculpt::paint.brush`
 *   (`toolsystem_brush_activate_from_toolref_for_object_paint()` -> #BKE_paint_brush_set,
 *   `wm_toolsystem.cc`). Catching this matters beyond ending the session: the incoming brush has
 *   never been through a paint stroke, so its pressure CurveMappings are still uninitialized
 *   (#bke::brush::common_pressure_curves_init runs from `PaintStroke`'s constructor only), and the
 *   next re-stamp dereferenced a null `CurveMap::table` inside #BKE_curvemapping_evaluateF
 *   (`mesh/sculpt.cc`'s `brush_strength()`).
 *
 * - The TOOL. Sculpt Mode's non-brush tools -- Move/Rotate/Scale/Transform, the Mesh/Cloth/Color
 *   filters, Trim, Line Project -- carry no brush at all, so switching to one leaves
 *   `Paint::brush` exactly as it was and the brush axis above stays blind to it.
 *
 * Both are polled rather than observed: the tool system publishes no notifier, only an RNA
 * message-bus message, and modal handlers receive neither.
 */
static bool curve_patch_edit_session_superseded(const bContext *C, const CurvePatchEditOpData &data)
{
  const ToolSettings *tool_settings = CTX_data_tool_settings(C);
  if (tool_settings && tool_settings->sculpt && data.brush_at_invoke &&
      BKE_paint_brush_for_read(&tool_settings->sculpt->paint) != data.brush_at_invoke)
  {
    return true;
  }
  if (data.tool_idname_at_invoke[0] != '\0') {
    const ScrArea *area = CTX_wm_area(C);
    const bToolRef *tref = area ? area->runtime.tool : nullptr;
    /* A null `tref` is not treated as a change: it only appears while
     * `toolsystem_refresh_screen_from_active_tool()` is mid-rebuild, which dispatches no events. */
    if (tref && !STREQ(tref->idname, data.tool_idname_at_invoke)) {
      return true;
    }
  }
  return false;
}

static wmOperatorStatus curve_patch_edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  CurvePatchEditOpData &data = *static_cast<CurvePatchEditOpData *>(op->customdata);

  /* The live patch's backing data can vanish out from under this modal operator without an
   * explicit Enter/Esc: unhandled keys (see the `default:` case below) deliberately pass through
   * to the ordinary View3D keymap, and a global Undo (Ctrl+Z) reaching it that way tears down and
   * reallocates the active object's `SculptSession` via the ordinary undo-restore path -- freeing
   * `curve_patch_session` out from under this operator without ever calling
   * #curve_patch_edit_finish. `curve_patch_edit_poll()` only guards the initial invoke, so without
   * this re-check here, the very next event (even a plain MOUSEMOVE) dereferences a dangling
   * `SculptSession`/`curve_patch_session` in #patch_cache_of and crashes. */
  const Object *ob_check = CTX_data_active_object(C);
  if (!ob_check || !ob_check->runtime->sculpt_session ||
      !ob_check->runtime->sculpt_session->curve_patch_session)
  {
    ED_workspace_status_text(C, nullptr);
    if (data.sync_timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data.sync_timer);
    }
    curve_patch_edit_snap_ctx_free(data);
    MEM_delete(&data);
    op->customdata = nullptr;
    WM_event_remove_modal_handler_other_windows(C, op);
    return OPERATOR_CANCELLED;
  }

  /* Windows opened after invoke only get a handler once this runs (also polled from the overlay
   * redraw cursor on MOUSEMOVE in any viewport). Without it, #sculpt_mode_and_brush_poll blocks
   * strokes globally but nothing in that window consumes curve-edit events. */
  WM_event_add_modal_handler_all_windows(C, op, SPACE_VIEW3D, RGN_TYPE_WINDOW);

  /* Commit and end the session once another tool or brush has taken over (see
   * #curve_patch_edit_session_superseded for how that is detected and why it has to be polled).
   * Committing rather than cancelling matches the intent behind reaching for another tool: the
   * patch the user built is kept, not silently discarded.
   *
   * Deliberately ABOVE the live-sync block below, which would otherwise read an incoming brush's
   * parameters and re-stamp with them before the switch is ever noticed.
   *
   * The commit re-stamps one final time inside #curve_patch_edit_finish, and that pass reads the
   * live brush -- so when the brush is what changed, the ORIGINAL one has to be restored for the
   * duration, or the commit would crash on its uninitialized pressure curves exactly the way an
   * unguarded switch does. A direct assignment is used rather than #BKE_paint_brush_set because
   * this is a temporary, exactly-symmetric restore that must not touch
   * `Paint::brush_asset_reference` (which still describes the brush the user just picked); the
   * assignment is all #BKE_paint_brush_set does to `Paint::brush` anyway, and it carries no user
   * counting. The patch must in any case be finalized with the brush it was built from. */
  if (curve_patch_edit_session_superseded(C, data)) {
    ToolSettings *ts = CTX_data_tool_settings(C);
    Paint *paint = (ts && ts->sculpt) ? &ts->sculpt->paint : nullptr;
    const bool swap_brush = paint && data.brush_at_invoke;
    Brush *brush_incoming = swap_brush ? BKE_paint_brush(paint) : nullptr;
    if (swap_brush) {
      paint->brush = data.brush_at_invoke;
    }
    const bool committed = curve_patch_edit_finish(C, op, false);
    if (swap_brush) {
      paint->brush = brush_incoming;
    }
    return committed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
  }

  CurvePatchSession &patch = patch_cache_of(C);
  Object &ob = *CTX_data_active_object(C);

  /* Live brush sync. Neither the Strength slider nor the Add/Subtract direction is frozen into the
   * patch (unlike radius/axis -- see `CurvePatchSession::params`): every re-stamp reads them live
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
      /* Multi-texture settings. The relief reads all of them live off the brush and the session, so
       * these exist purely to trigger a re-stamp -- nothing is copied into `params`. */
      const int stamp_tex_source = brush->curve_patch.stamp_texture_source;
      const int ribbon_tex_source = brush->curve_patch.ribbon_texture_source;
      const float cap_start_length = brush->curve_patch.cap_start_length;
      const float cap_end_length = brush->curve_patch.cap_end_length;
      const uint64_t texture_list_digest = curve_patch_texture_list_digest(*brush);
      const uint64_t texture_pointer_digest = curve_patch_texture_pointer_digest(*brush);
      const void *cap_tex[3] = {brush->curve_patch.tex_start,
                                brush->curve_patch.tex_middle,
                                brush->curve_patch.tex_end};
      const bool multi_texture_changed =
          stamp_tex_source != data.last_synced_stamp_tex_source ||
          ribbon_tex_source != data.last_synced_ribbon_tex_source ||
          cap_start_length != data.last_synced_cap_start_length ||
          cap_end_length != data.last_synced_cap_end_length ||
          texture_list_digest != data.last_synced_texture_list_digest ||
          cap_tex[0] != data.last_synced_cap_tex[0] ||
          cap_tex[1] != data.last_synced_cap_tex[1] ||
          cap_tex[2] != data.last_synced_cap_tex[2];
      /* Narrower than `multi_texture_changed` above: true only when the set of sampled IMAGES
       * changed -- a cap texture reassignment or a list slot added/removed/retargeted/reordered --
       * as opposed to a cap-length drag or a slot weight edit, which re-stamp but keep sampling the
       * same images. Drives the texture-pool rebuild gate below; a cap-length or weight-only change
       * must not free/reallocate the pool on every tick of a slider drag. */
      const bool texture_identity_changed =
          texture_pointer_digest != data.last_synced_texture_pointer_digest ||
          cap_tex[0] != data.last_synced_cap_tex[0] ||
          cap_tex[1] != data.last_synced_cap_tex[1] ||
          cap_tex[2] != data.last_synced_cap_tex[2];
      /* Mesh mirror symmetry is read live too, so toggling Mirror X/Y/Z re-projects immediately; it
       * is applied by `do_symmetrical_brush_actions()` inside the re-stamp itself. */
      const int symm = mesh_symmetry_xyz_get(ob);
      /* The one watchdog that arbitrates rather than merely triggers -- see
       * `CurvePatchEditOpData::last_synced_swap_axis`. Recorded unconditionally: it tracks the
       * BRUSH's value, and the S hotkey may well have already made the session agree with the new
       * one, in which case the parameter compare below finds nothing to do and must not leave this
       * reporting a change forever. */
      const bool swap_axis = brush->curve_patch.swap_axis != 0;
      const bool brush_swap_axis_changed = swap_axis != data.last_synced_swap_axis;
      data.last_synced_swap_axis = swap_axis;
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
      const float3 brush_color = BKE_brush_color_get(&sd.paint, brush);
      const bool color_changed = brush_color != data.last_synced_brush_color;
      /* Every brush-driven build parameter in one shot, instead of the two dozen hand-written field
       * compares this used to be. The values the brush cannot supply come from the session: the
       * frozen ones, and `swap_axis`, which the brush may only overwrite when the BRUSH is what
       * changed (the S hotkey and the session undo stack write it too).
       *
       * A zero `radius_per_size` means the patch started with a zero brush size, which cannot happen
       * through the UI; guard anyway rather than collapse the patch to nothing. */
      const int brush_size = BKE_brush_size_get(&sd.paint, brush);

      /* Check if any parameter changed by comparing against the ACTIVE patch. All patches share
       * the same brush-driven parameters, so we only need to check once. */
      CurvePatchItem &active_item = patch.active_item();
      const float radius = active_item.params.radius_per_size > 0.0f ?
                               active_item.params.radius_per_size * float(brush_size) :
                               active_item.params.radius;
      bke::CurvePatchParams live = curve_patch_params_from_brush(
          *brush,
          radius,
          active_item.params.radius_per_size,
          active_item.params.plane_normal,
          active_item.params.stamp_seed,
          brush_swap_axis_changed ? swap_axis : active_item.params.swap_axis);
      /* Carried across rather than defaulted: this poll never runs inside the commit's
       * final-quality build, but making the compare depend on that would be a trap. */
      live.final_quality = active_item.params.final_quality;

      if (live != active_item.params || alpha != data.last_synced_alpha ||
          dir_in != data.last_synced_dir_in || symm != data.last_synced_symm || tex_changed ||
          color_changed || falloff_preset != data.last_synced_falloff_preset ||
          falloff_curve_ts != data.last_synced_falloff_curve_ts || multi_texture_changed)
      {
        /* Update params for ALL patches, not just the active one. Each patch keeps its own
         * plane_normal, radius_per_size, stamp_seed, and final_quality, but all other parameters
         * come from the brush and must be synchronized across all patches. */
        for (CurvePatchItem &item : patch.patches) {
          const float item_radius = item.params.radius_per_size > 0.0f ?
                                        item.params.radius_per_size * float(brush_size) :
                                        item.params.radius;
          bke::CurvePatchParams item_live = curve_patch_params_from_brush(
              *brush,
              item_radius,
              item.params.radius_per_size,
              item.params.plane_normal,
              item.params.stamp_seed,
              brush_swap_axis_changed ? swap_axis : item.params.swap_axis);
          item_live.final_quality = item.params.final_quality;
          item.params = item_live;
        }
        data.last_synced_alpha = alpha;
        data.last_synced_dir_in = dir_in;
        data.last_synced_symm = symm;
        data.last_synced_tex = tex;
        data.last_synced_tex_edit_count = tex_edit_count;
        data.last_synced_brush_color = brush_color;
        data.last_synced_tex_size[0] = brush->mtex.size[0];
        data.last_synced_tex_size[1] = brush->mtex.size[1];
        data.last_synced_tex_ofs[0] = brush->mtex.ofs[0];
        data.last_synced_tex_ofs[1] = brush->mtex.ofs[1];
        data.last_synced_falloff_preset = falloff_preset;
        data.last_synced_falloff_curve_ts = falloff_curve_ts;
        data.last_synced_stamp_tex_source = stamp_tex_source;
        data.last_synced_ribbon_tex_source = ribbon_tex_source;
        data.last_synced_cap_start_length = cap_start_length;
        data.last_synced_cap_end_length = cap_end_length;
        data.last_synced_texture_list_digest = texture_list_digest;
        data.last_synced_texture_pointer_digest = texture_pointer_digest;
        data.last_synced_cap_tex[0] = cap_tex[0];
        data.last_synced_cap_tex[1] = cap_tex[1];
        data.last_synced_cap_tex[2] = cap_tex[2];
        /* The relief samples the texture through the session's `ImagePool`, which caches ImBuf
         * handles for the whole sculpt session (#SculptSession::tex_pool_ensure). A changed image --
         * a different Image datablock on the texture, or edited pixels -- would otherwise keep
         * sampling the old buffer. Rebuild the pool whenever the set of sampled textures' IDENTITY
         * changes: the brush's own texture (`tex_changed`), or a cap/list texture reassignment
         * (`texture_identity_changed`, a list/cap counterpart of the same idea). Deliberately NOT
         * gated on the broader `multi_texture_changed`: a cap-length drag or a slot WEIGHT edit
         * re-stamps (see `multi_texture_changed` above) but samples the same images as before, so
         * rebuilding the pool for those would only free and reallocate it needlessly on every tick
         * of a slider drag.
         *
         * Only dropped here, never rebuilt: the re-stamp two lines below reaches the effect, which
         * creates the pool it needs through `SculptSession::tex_pool_ensure()`. */
        if (tex_changed || texture_identity_changed) {
          ob.runtime->sculpt_session->tex_pool_invalidate();
        }
        /* `swap_axis` and `stamp_mode` both feed the status text, so it has to be rebuilt here and
         * not only where the modal's own hotkeys change them. */
        curve_patch_edit_status_set(C, patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_viewports_redraw_after_edit(*C, ob, patch);
      }
    }
  }

  /* A foreign operator changed the mesh's element count (see `CurvePatchApplyState::element_num`). The
   * patch cannot be committed or even restored, so end the modal as soon as the flag is seen.
   *
   * Deliberately ABOVE the pass-through gate below rather than after the event switch. The
   * live-sync block just above is one of the places that raises the flag, and it runs on events
   * whose cursor sits outside this patch's viewport -- exactly where the cursor is while the user
   * drags a brush slider in the N-panel, the interaction that block exists for. The gate returns
   * early for those events, so a check placed after the switch would never see them.
   *
   * This is not the only place the flag can be raised: the commit key bypasses the gate, and its
   * re-stamp runs inside `curve_patch_edit_finish()`, which reports back whether a commit actually
   * happened so that path can answer `OPERATOR_CANCELLED` on its own.
   *
   * `curve_patch_restore_only()` is a no-op in this state, so the cancel path leaves the mesh
   * exactly as the foreign operator left it. */
  if (patch.apply.invalidated) {
    curve_patch_edit_finish(C, op, true);
    return OPERATOR_CANCELLED;
  }

  /* #WM_event_add_modal_handler_all_windows() (called from `curve_patch_edit_invoke()`)
   * registers this operator as a WINDOW-level modal handler (`win->runtime->modalhandlers`), not
   * an area/region one -- so it receives every mouse/keyboard event in the entire window, for
   * whichever area happens to be under the cursor, before that area's own handlers ever run.
   * Without this guard, a click on a Properties panel button, an Outliner entry, the N-panel, or
   * a header -- anywhere outside the 3D view this patch was started in -- was consumed by the
   * `LEFTMOUSE`/`RIGHTMOUSE` cases below (after missing the curve hit-test), so the click never
   * reached the editor it was actually meant for and all other UI interaction appeared frozen for
   * the whole session.
   *
   * The event belongs to this modal when the cursor sits over ANY 3D viewport WINDOW region in the
   * window (split views included), not only the one frozen at invoke time --
   * #ModalViewportTracker resolves that the same way the window manager itself routes events to
   * regions (#ED_area_find_region_xy_visual).
   *
   * Enter/Esc are exempt so commit/cancel works even with the cursor off the viewport. An active
   * drag is exempt too, so dragging a point over the N-panel or past the viewport edge does not
   * orphan the drag or eat the `LEFTMOUSE` release that ends it. */
  const bool is_dragging = data.dragging_point || data.dragging_radius || data.dragging_handle ||
                           data.dragging_segment;
  /* The undo chord is ours no matter where the cursor is, and `EVT_ZKEY`'s case below swallows it.
   * Global undo while a patch is live would pop a previously committed step and move mesh vertices
   * out from under this patch's `orig_positions` snapshot, leaving every later restore and the
   * commit itself writing against a surface that no longer matches what was recorded. That case
   * sits BEHIND this gate, so without lifting the chord above it the chord escaped to the global
   * keymap whenever the cursor sat over a panel or the header -- doing exactly what it exists to
   * prevent. Plain Z is deliberately not exempt: it belongs to Sculpt Mode's shading pie, and the
   * case below passes it through on its own. */
  const bool is_undo_chord = (event->type == EVT_ZKEY) &&
                             (event->modifier & (KM_CTRL | KM_OSKEY)) != 0;
  const bool is_commit_key = ELEM(event->type, EVT_RETKEY, EVT_PADENTER, EVT_ESCKEY) ||
                             is_undo_chord;

  /* Constructed before the pass-through decision (not after, as the two locals it replaces used
   * to be computed): a "not found" construction never touches `CTX_wm_area()`/`CTX_wm_region()`,
   * so returning #OPERATOR_PASS_THROUGH immediately below is exactly as safe as it was when the
   * lookup was a plain read-only query -- the tracker's destructor restores the frozen context on
   * that path too, as a no-op. */
  ed::ModalViewportTracker tracker(*C, *event, SPACE_VIEW3D, RGN_TYPE_WINDOW);
  const bool over_viewport = tracker.found();
  const bool should_pass_through = !is_dragging && !over_viewport && !is_commit_key;
  if (should_pass_through) {
    return OPERATOR_PASS_THROUGH;
  }

  /* Window-level modals freeze `CTX_wm_area()`/`CTX_wm_region()` at invoke time; the tracker above
   * already repointed them at the viewport actually under the cursor. Refresh the session's owned
   * `ViewContext` so hit-tests, screen projection and sculpt flushes all target the right region.
   * During an in-flight drag that wanders over an N-panel, fall back to the last synced viewport. */
  if (!tracker.found() && is_dragging && patch.view_context.region) {
    tracker.use_fallback_region(patch.view_context.region);
  }
  if (tracker.found()) {
    curve_patch_sync_view_context(C, tracker.area(), tracker.region(), patch);
  }
  const int event_mval[2] = {tracker.mval().x, tracker.mval().y};

  /* Below this point the modal only ever ends via an explicit Enter (commit) or Esc (cancel) -- no
   * click and no mouse-leaving-the-viewport implicitly ends it. Events that don't match anything
   * below are simply consumed like any other event while the patch is live; the user must press
   * Enter/Esc first. A tool or brush switch is the one exception, and it is handled by the poll at
   * the top of this function rather than here: it arrives as a click on the toolbar, which the
   * pass-through gate above hands to the toolbar before any case here could see it. */

  switch (event->type) {
    case LEFTMOUSE:
      if (event->val == KM_PRESS && data.dragging_radius && !data.dragging_point &&
          !data.dragging_handle && !data.dragging_segment)
      {
        /* A radius drag started by Alt+S (see `EVT_SKEY` below) has no mouse button held down
         * the way a click-started drag does, so there is no RELEASE to end it on -- the next
         * click confirms it instead, mirroring how G/R/S transform modals confirm on click. */
        data.dragging_radius = false;
        curve_patch_edit_snap_ctx_free(data);
        curve_patch_undo_push(patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_viewports_redraw_after_edit(*C, ob, patch);
        break;
      }
      if (event->val == KM_PRESS) {
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        const float loc_fl[2] = {float(event_mval[0]), float(event_mval[1])};

        /* Every hit-test below scans all patches and keeps the CLOSEST hit, not the first one
         * found. Overlapping patches would otherwise give the click an arbitrary priority based
         * on patch list order rather than what the user actually pointed at. */

        int best_patch = -1;
        int hit_radius = -1;
        float best_radius_dist = FLT_MAX;
        Vector<PaintCurvePoint> best_screen_points;

        /* Radius handles take priority. */
        for (const int i : patch.patches.index_range()) {
          bke::CurvesGeometry &geom = patch.patches[i].control_curve;
          Vector<PaintCurvePoint> screen_points;
          paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);
          const int hit = patch_find_radius_handle_at_pos(
              geom, screen_points.as_span(), loc_fl, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
          if (hit < 0) {
            continue;
          }
          PaintCurveRadiusHandleScreen handle;
          paintcurve_radius_handle_screen_get_from_geometry(
              geom, screen_points.data(), hit, &handle);
          const float end[2] = {handle.end.x, handle.end.y};
          const float dist = len_v2v2(loc_fl, end);
          if (dist < best_radius_dist) {
            best_radius_dist = dist;
            best_patch = i;
            hit_radius = hit;
            best_screen_points = screen_points;
          }
        }

        if (best_patch >= 0) {
          patch.active_patch = best_patch;
          patch.edit.active_point = hit_radius;
          data.dragging_radius = true;
          paintcurve_radius_handle_screen_get_from_geometry(
              patch.active_item().control_curve,
              best_screen_points.data(),
              hit_radius,
              &data.radius_handle);
          /* Drive the overlay's selected-point highlight (`hd.selected_center`, read from the
           * geometry's own selection attribute -- see `paintcurve_geom_get_selection` -- not from
           * `active_point`), matching the classic paint-curve click (`paintcurve.select`,
           * paint_curve.cc). Without this, clicking a point moves `active_point` (so drags and
           * hotkeys act on the right point) but the diamond never visibly changes color. */
          bke::CurvesGeometry &geom_sel = patch.active_item().control_curve;
          paintcurve_geom_set_all_selection(geom_sel, 0);
          paintcurve_geom_set_selection(geom_sel, hit_radius, 0x07);
          break;
        }

        /* Tests the pivot AND both Bezier tangent handles. */
        int hit_point = -1;
        char best_selflag = 0;
        float best_point_dist = FLT_MAX;

        for (const int i : patch.patches.index_range()) {
          bke::CurvesGeometry &geom = patch.patches[i].control_curve;
          Vector<PaintCurvePoint> screen_points;
          paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);
          char selflag = 0;
          const int hit = paintcurve_find_in_screen_points(
              screen_points.as_span(), loc_fl, /*ignore_pivot=*/false,
              PAINT_CURVE_POINT_SELECT_THRESHOLD, &selflag);
          if (hit < 0) {
            continue;
          }
          const float *hit_co = (selflag == SEL_F1)  ? screen_points[hit].bez.vec[0] :
                                 (selflag == SEL_F3)  ? screen_points[hit].bez.vec[2] :
                                                        screen_points[hit].bez.vec[1];
          const float dist = len_v2v2(loc_fl, hit_co);
          if (dist < best_point_dist) {
            best_point_dist = dist;
            best_patch = i;
            hit_point = hit;
            best_selflag = selflag;
          }
        }

        if (best_patch >= 0) {
          patch.active_patch = best_patch;
          patch.edit.active_point = hit_point;
          /* See the matching comment on the radius-handle hit above: this is what makes the
           * overlay's diamond actually change color on click. */
          {
            bke::CurvesGeometry &geom_sel = patch.active_item().control_curve;
            const uint8_t sel_bit = (best_selflag == SEL_F1) ? 0x01 :
                                    (best_selflag == SEL_F3) ? 0x04 :
                                                                0x02;
            paintcurve_geom_set_all_selection(geom_sel, 0);
            paintcurve_geom_set_selection(geom_sel, hit_point, sel_bit);
          }
          if (best_selflag == SEL_F1 || best_selflag == SEL_F3) {
            data.dragging_handle = true;
            data.handle_is_left = (best_selflag == SEL_F1);
          }
          else {
            data.dragging_point = true;
            data.drag_start_mval = float2(loc_fl[0], loc_fl[1]);
            bke::CurvesGeometry &geom = patch.active_item().control_curve;
            for (int h = 0; h < 3; h++) {
              data.point_initial_loc_3d[h] = paintcurve_geom_co(geom, hit_point, h);
            }
          }
          break;
        }

        /* No point/handle hit -- try the curve wire itself. */
        int hit_segment = -1;
        int hit_segment_next = -1;
        float hit_segment_t = 0.0f;
        float best_segment_dist_sq = FLT_MAX;

        for (const int i : patch.patches.index_range()) {
          bke::CurvesGeometry &geom = patch.patches[i].control_curve;
          int segment_index = -1;
          int segment_index_next = -1;
          float edge_t = 0.0f;
          float dist_sq = FLT_MAX;
          if (patch_find_closest_segment(geom,
                                         &vc,
                                         loc_fl,
                                         PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                         &segment_index,
                                         &segment_index_next,
                                         &edge_t,
                                         &dist_sq) &&
              dist_sq < best_segment_dist_sq)
          {
            best_segment_dist_sq = dist_sq;
            best_patch = i;
            hit_segment = segment_index;
            hit_segment_next = segment_index_next;
            hit_segment_t = edge_t;
          }
        }

        if (best_patch >= 0) {
          patch.active_patch = best_patch;
          data.dragging_segment = true;
          data.segment_index_a = hit_segment;
          data.segment_index_b = hit_segment_next;
          data.segment_t = hit_segment_t;
          break;
        }

        /* A click that doesn't land on a point or the wire is simply not a drag start. */
      }
      else if (event->val == KM_RELEASE &&
               (data.dragging_point || data.dragging_radius || data.dragging_handle ||
                data.dragging_segment))
      {
        data.dragging_point = false;
        data.dragging_radius = false;
        data.dragging_handle = false;
        data.dragging_segment = false;
        curve_patch_edit_snap_ctx_free(data);
        /* One undo step per drag, recorded on release -- not one per MOUSEMOVE, which would fill
         * the stack with intermediate positions nobody wants to step through. */
        curve_patch_undo_push(patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_viewports_redraw_after_edit(*C, ob, patch);
      }
      break;
    case MOUSEMOVE:
      if (data.dragging_point) {
        bke::CurvesGeometry &geom = patch.active_item().control_curve;
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
        const float3 &pivot = geom.positions()[patch.edit.active_point];
        float pivot_world[3];
        mul_v3_m4v3(pivot_world, ob_to_world, pivot);

        const float mval_init[2] = {data.drag_start_mval.x, data.drag_start_mval.y};
        const float mval_curr[2] = {float(event_mval[0]), float(event_mval[1])};

        /* Keep the point on the surface while it is dragged, same as #paintcurve_point_slide_modal.
         * `use_depth_fallback` is off: the depth-buffer level needs a viewport redraw, which would
         * run on every mouse move. On a miss this falls through to the screen-delta path below. */
        float obj_delta[3];
        float hit_obj[3];
        float hit_no_obj[3];
        if (data.snap_ctx == nullptr) {
          data.snap_ctx = ED_paintcurve_snap_context_create();
        }
        if (paintcurve_surface_place(C,
                                     data.snap_ctx,
                                     vc,
                                     mval_curr,
                                     pivot_world,
                                     /*use_depth_fallback=*/false,
                                     hit_obj,
                                     hit_no_obj))
        {
          sub_v3_v3v3(obj_delta, hit_obj, data.point_initial_loc_3d[1]);
          paintcurve_geom_set_surface_normal(geom, patch.edit.active_point, float3(hit_no_obj));
        }
        else {
          float world_init[3], world_curr[3];
          ED_view3d_win_to_3d(vc.v3d, vc.region, pivot_world, mval_init, world_init);
          ED_view3d_win_to_3d(vc.v3d, vc.region, pivot_world, mval_curr, world_curr);

          float obj_init[3], obj_curr[3];
          mul_v3_m4v3(obj_init, world_to_ob, world_init);
          mul_v3_m4v3(obj_curr, world_to_ob, world_curr);
          sub_v3_v3v3(obj_delta, obj_curr, obj_init);
        }

        for (int h = 0; h < 3; h++) {
          add_v3_v3v3(
              paintcurve_geom_co(geom, patch.edit.active_point, h), obj_delta, data.point_initial_loc_3d[h]);
        }
        geom.calculate_bezier_auto_handles();
        geom.calculate_bezier_aligned_handles();
        geom.tag_positions_changed();

        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_overlay_redraw_all(C);
      }
      else if (data.dragging_radius) {
        /* Mirrors #paintcurve_slide_radius_modal (paint_curve.cc:2689-2699): the drag axis was
         * fixed at drag-start, so every move just re-projects the current mouse position onto it. */
        const float mval_fl[2] = {float(event_mval[0]), float(event_mval[1])};
        const float new_radius = paintcurve_radius_from_handle_screen_pos(&data.radius_handle,
                                                                          mval_fl);
        patch.active_item().control_curve.radius_for_write()[patch.edit.active_point] = new_radius;
        patch.active_item().control_curve.tag_positions_changed();

        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_overlay_redraw_all(C);
      }
      else if (data.dragging_handle) {
        bke::CurvesGeometry &geom = patch.active_item().control_curve;
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
        const float3 &pivot = geom.positions()[patch.edit.active_point];
        float pivot_world[3];
        mul_v3_m4v3(pivot_world, ob_to_world, pivot);

        const float mval_curr[2] = {float(event_mval[0]), float(event_mval[1])};
        float world_curr[3];
        ED_view3d_win_to_3d(vc.v3d, vc.region, pivot_world, mval_curr, world_curr);
        mul_v3_m4v3(paintcurve_geom_co(geom, patch.edit.active_point, data.handle_is_left ? 0 : 2),
                    world_to_ob,
                    world_curr);

        /* A direct handle drag makes that point's shape freely adjustable, so a COMPUTED handle
         * type (Auto/Vector, whose positions #calculate_bezier_auto_handles() would immediately
         * overwrite) is promoted to Align -- the curve then stays smooth (tangent-continuous)
         * through the point because #calculate_bezier_aligned_handles() below re-aligns the OTHER
         * handle's direction while preserving its own length. A type the user chose explicitly via
         * the context menu (#SCULPT_OT_curve_patch_handle_type_set) is left alone: overwriting it
         * here would make Free unusable, since dragging either handle of a Free point would
         * silently re-couple it to its opposite. */
        for (MutableSpan<int8_t> types :
             {geom.handle_types_left_for_write(), geom.handle_types_right_for_write()})
        {
          if (ELEM(types[patch.edit.active_point], BEZIER_HANDLE_AUTO, BEZIER_HANDLE_VECTOR)) {
            types[patch.edit.active_point] = BEZIER_HANDLE_ALIGN;
          }
        }
        geom.calculate_bezier_auto_handles();
        geom.calculate_bezier_aligned_handles();
        geom.tag_positions_changed();

        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_overlay_redraw_all(C);
      }
      else if (data.dragging_segment) {
        bke::CurvesGeometry &geom = patch.active_item().control_curve;
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

        const float mval_fl[2] = {float(event_mval[0]), float(event_mval[1])};
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
        curve_patch_tag_overlay_redraw_all(C);
      }
      else {
        /* Hover / Ctrl+RMB insert preview is drawn by the overlay engine, which only rebuilds on a
         * tagged viewport redraw. Curve Edit gets that from the WM paint-cursor poll; this modal
         * consumes MOUSEMOVE without dragging, so refresh explicitly. */
        curve_patch_tag_overlay_redraw_all(C);
      }
      break;
    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY:
      curve_patch_tag_overlay_redraw_all(C);
      break;
    case RIGHTMOUSE:
      /* Plain right-click over a control point opens the context menu -- the equivalent of Paint
       * Curve's #PAINTCURVE_OT_context_menu, which cannot be reused here because every
       * `PAINTCURVE_OT_*` operator resolves its data through a `PaintCurve` ID and this editor's
       * control curve is a standalone `CurvesGeometry`. Ctrl+RMB keeps its insert/append meaning
       * below; plain RMB away from a point stays the no-op it has always been. */
      if (event->val == KM_PRESS && (event->modifier & KM_CTRL) == 0) {
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        const float loc_fl[2] = {float(event_mval[0]), float(event_mval[1])};

        /* Scans all patches and keeps the CLOSEST hit, not the first found -- see the LEFTMOUSE
         * handler above for why. */
        int best_patch = -1;
        int hit_point = -1;
        float best_point_dist = FLT_MAX;

        for (const int i : patch.patches.index_range()) {
          const bke::CurvesGeometry &geom = patch.patches[i].control_curve;
          if (!paintcurve_geometry_is_valid(geom)) {
            continue;
          }
          Vector<PaintCurvePoint> screen_points;
          paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);
          char selflag = 0;
          const int hit = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                           loc_fl,
                                                           /*ignore_pivot=*/false,
                                                           PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                           &selflag);
          /* A hit on either tangent handle counts as a hit on its point -- same as the pivot,
           * since every menu entry acts on the whole point. */
          if (hit < 0) {
            continue;
          }
          const float *hit_co = (selflag == SEL_F1)  ? screen_points[hit].bez.vec[0] :
                                 (selflag == SEL_F3)  ? screen_points[hit].bez.vec[2] :
                                                        screen_points[hit].bez.vec[1];
          const float dist = len_v2v2(loc_fl, hit_co);
          if (dist < best_point_dist) {
            best_point_dist = dist;
            best_patch = i;
            hit_point = hit;
          }
        }

        if (best_patch >= 0) {
          patch.active_patch = best_patch;
          patch.edit.active_point = hit_point;
          curve_patch_edit_context_menu_open(C);
          break;
        }
        /* Plain RMB away from a control point must reach `BRUSH_OT_stencil_control` and the rest of
         * the paint keymap -- consuming the event on a miss left stencil adjustment (and its X/Y
         * axis constraints) dead for the whole session. */
        return OPERATOR_PASS_THROUGH;
      }
      if (event->val == KM_PRESS && (event->modifier & KM_CTRL)) {
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        const float loc_fl[2] = {float(event_mval[0]), float(event_mval[1])};

        bool did_insert = false;
        /* Scans all patches and keeps the CLOSEST hit, not the first found -- see the LEFTMOUSE
         * handler above for why. */
        int best_patch = -1;
        int hit_segment_index = -1;
        int hit_segment_index_next = -1;
        float hit_edge_t = 0.0f;
        float best_segment_dist_sq = FLT_MAX;

        for (const int i : patch.patches.index_range()) {
          bke::CurvesGeometry &geom = patch.patches[i].control_curve;
          if (!paintcurve_geometry_is_valid(geom) || geom.points_num() < 2) {
            continue;
          }
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
          if (near_point) {
            continue;
          }

          int segment_index = -1;
          int segment_index_next = -1;
          float edge_t = 0.0f;
          float dist_sq = FLT_MAX;
          const bool found_segment = patch_find_closest_segment(
              geom,
              &vc,
              loc_fl,
              PAINT_CURVE_INSERT_SEGMENT_THRESHOLD,
              &segment_index,
              &segment_index_next,
              &edge_t,
              &dist_sq);

          /* Clicks near segment endpoints extend the spline instead of subdividing it (same
           * 0.1/0.9 bounds as #paintcurve_try_insert_point_at_mouse). */
          if (found_segment && edge_t >= 0.1f && edge_t <= 0.9f &&
              geom.handle_positions_right().has_value() && geom.handle_positions_left().has_value() &&
              dist_sq < best_segment_dist_sq)
          {
            best_segment_dist_sq = dist_sq;
            best_patch = i;
            hit_segment_index = segment_index;
            hit_segment_index_next = segment_index_next;
            hit_edge_t = edge_t;
          }
        }

        if (best_patch >= 0) {
          patch.active_patch = best_patch;
          bke::CurvesGeometry &geom = patch.active_item().control_curve;
          /* CurvePatchEditState::control_curve is always a single spline (built via
           * paintcurve_geometry_init_bezier), so the owning curve is always index 0. */
          const int insert_index = paintcurve_geometry_insert_point_at_segment(
              geom, hit_segment_index, hit_segment_index_next, /*active_curve=*/0, hit_edge_t);
          if (insert_index >= 0) {
            patch.edit.active_point = insert_index;
            did_insert = true;
          }
        }

        if (!did_insert) {
          bke::CurvesGeometry &geom = patch.active_item().control_curve;
          /* Append: extend the single spline at its end (or start it, if empty). Uses the same
           * placement chain as #paintcurve_point_add so points land on the surface here too;
           * `patch.active_item().params.plane_normal` only stands in when no real surface was hit. */
          float obj_co[3];
          float obj_no[3];
          const bool placed = paintcurve_surface_place(
              C, nullptr, vc, loc_fl, nullptr, /*use_depth_fallback=*/true, obj_co, obj_no);

          const bool create_new_spline = (geom.points_num() == 0);
          int active_curve = 0;
          int add_index = paintcurve_geometry_is_valid(geom) ?
                              int(geom.points_by_curve()[0].size()) :
                              0;
          paintcurve_geometry_add_point(geom,
                                        float3(obj_co),
                                        placed ? float3(obj_no) : patch.active_item().params.plane_normal,
                                        create_new_spline,
                                        active_curve,
                                        add_index);
          patch.edit.active_point = geom.points_num() - 1;
        }

        curve_patch_undo_push(patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_viewports_redraw_after_edit(*C, ob, patch);
      }
      break;
    case EVT_XKEY:
      if (event->val == KM_PRESS) {
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        const float loc_fl[2] = {float(event_mval[0]), float(event_mval[1])};

        /* Unlike the hit tests above, this stops at the first patch with any hit (radius, point,
         * or wire) rather than the globally closest one -- `patch_find_point_index_at_pos_for_delete`
         * mixes those three hit kinds internally and does not report a comparable distance. Given
         * the small selection threshold this rarely matters in practice for a delete action. */
        int best_patch = -1;
        int hit_point = -1;
        for (const int i : patch.patches.index_range()) {
          bke::CurvesGeometry &geom = patch.patches[i].control_curve;
          hit_point = patch_find_point_index_at_pos_for_delete(geom, &vc, loc_fl);
          if (hit_point >= 0) {
            best_patch = i;
            break;
          }
        }

        if (best_patch < 0) {
          /* Away from the control curve, X belongs to Sculpt/Texture Paint's primary/secondary color
           * swap (`PAINT_OT_brush_colors_flip`). During an active stencil RMB-drag it also reaches
           * `BRUSH_OT_stencil_control` for axis constraints. */
          return OPERATOR_PASS_THROUGH;
        }

        patch.active_patch = best_patch;
        patch.edit.active_point = hit_point;
        if (curve_patch_delete_active_point(*C, ob, patch, op->reports)) {
          /* The removed point may have been the one under an in-flight drag; drop the drag state so
           * the next MOUSEMOVE does not index into geometry that no longer has it. */
          data.dragging_point = false;
          data.dragging_radius = false;
          data.dragging_handle = false;
          data.dragging_segment = false;
          curve_patch_edit_snap_ctx_free(data);
        }
      }
      break;
    case EVT_DELKEY:
      if (event->val == KM_PRESS) {
        if (curve_patch_delete_active_point(*C, ob, patch, op->reports)) {
          data.dragging_point = false;
          data.dragging_radius = false;
          data.dragging_handle = false;
          data.dragging_segment = false;
          curve_patch_edit_snap_ctx_free(data);
        }
      }
      break;
    case EVT_ZKEY:
      /* Ctrl+Z / Ctrl+Shift+Z (Cmd on macOS) drive the SESSION undo stack, and are swallowed
       * whatever the outcome. Letting them reach the global keymap lets `ED_OT_undo` pop the
       * PREVIOUS committed step -- moving mesh vertices out from under this patch's
       * `orig_positions` snapshot and leaving the session inconsistent. */
      if ((event->modifier & (KM_CTRL | KM_OSKEY)) == 0) {
        /* Plain Z belongs to Sculpt Mode's shading pie menu; only the undo chord is ours. */
        return OPERATOR_PASS_THROUGH;
      }
      if (event->val == KM_PRESS) {
        if (event->modifier & KM_SHIFT) {
          curve_patch_undo_step_forward(*C, ob, patch);
        }
        else if (!curve_patch_undo_step_back(*C, ob, patch)) {
          /* Back at the state the anchor stroke produced: there is no earlier in-session state, so
           * undoing once more discards the patch itself. */
          curve_patch_edit_finish(C, op, true);
          return OPERATOR_CANCELLED;
        }
      }
      break;
    case EVT_CKEY:
      /* Matches Blender's curve-editing convention for toggling a curve closed. */
      if (event->val == KM_PRESS) {
        curve_patch_toggle_cyclic(*C, ob, patch);
      }
      break;
    case EVT_YKEY:
      /* Deliberately NOT Tab, which this used to answer to as well: Tab belongs to the mode toggle,
       * and swallowing it left no way to leave Sculpt Mode for the whole session. X is not an
       * option either -- away from the curve it swaps brush colors (see `EVT_XKEY` above). S is not
       * an option either -- this modal runs over the viewport and would swallow the S of Ctrl+S /
       * Cmd+S save. */
      if (event->val == KM_PRESS) {
        bke::CurvePatchParams &params = patch.active_item().params;
        params.swap_axis = !params.swap_axis;
        curve_patch_undo_push(patch);
        curve_patch_edit_status_set(C, patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_viewports_redraw_after_edit(*C, ob, patch);
      }
      break;
    case EVT_SKEY:
      /* Alt+S only -- matches Curve Edit's own radius shortcut (`transform.transform` /
       * CURVE_SHRINKFATTEN, bound Alt+S in `km_paint_curve`/`km_3d_view_tool_sculpt_curves_edit`).
       * Plain S is deliberately left alone (see the `EVT_YKEY` comment above: this modal runs over
       * the whole viewport and would otherwise swallow Ctrl+S/Cmd+S save, and plain S is Sculpt
       * Mode's brush-size shortcut). */
      if (event->val == KM_PRESS && (event->modifier & KM_ALT) &&
          !(event->modifier & (KM_CTRL | KM_SHIFT | KM_OSKEY)) && !is_dragging &&
          curve_patch_active_point_is_valid(patch))
      {
        bke::CurvesGeometry &geom = patch.active_item().control_curve;
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        Vector<PaintCurvePoint> screen_points;
        paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);
        if (!screen_points.is_empty()) {
          /* No mouse button is held for this drag (unlike grabbing the radius handle directly),
           * so `MOUSEMOVE`'s existing `data.dragging_radius` branch is reused unchanged and the
           * `LEFTMOUSE` press guard above confirms it on the next click instead of a release. */
          data.dragging_radius = true;
          paintcurve_radius_handle_screen_get_from_geometry(
              geom, screen_points.data(), patch.edit.active_point, &data.radius_handle);
        }
        break;
      }
      return OPERATOR_PASS_THROUGH;
    case EVT_RETKEY:
    case EVT_PADENTER:
      if (event->val == KM_PRESS) {
        return curve_patch_edit_finish(C, op, false) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
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

static bool curve_patch_edit_finish(bContext *C, wmOperator *op, const bool is_cancel)
{
  ED_workspace_status_text(C, nullptr);

  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  CurvePatchSession *patch = ss.curve_patch_session;

  bool committed = false;
  if (patch) {
    if (is_cancel) {
      /* No undo bookkeeping HERE: the modal never opened a transaction (the one the anchor stroke
       * opened is discarded the moment the editor starts, see `SculptPaintStroke::done()` in
       * `mesh/sculpt.cc`) and never pushed a node. The restore is what puts the mesh back.
       *
       * A cancelled patch still leaves the undo history exactly as it found it, but for the relief
       * and vertex-color targets only that is because nothing was ever opened. A target whose data
       * has its own undo system does open one -- the image canvas holds an `ImageUndoStep` for the
       * whole session, because that is the only way its per-tile "before" data can be captured as
       * it is painted -- and it is that effect's own destructor, reached through `MEM_delete(patch)`
       * below, that discards the step instead of committing it once this restore has put the pixels
       * back. See #ImageColorEffect's destructor. */
      curve_patch_restore_only(ob, *patch);
      /* Same full redraw handshake as commit: restore only updates mesh data in memory; without
       * tagging every viewport the relief in other areas/windows stays stale until the cursor
       * happens to move there. */
      curve_patch_tag_viewports_redraw_after_edit(*C, ob, *patch);
    }
    else {
      /* Re-stamp once at final quality BEFORE the undo step is built, so the mesh (and with it the
       * undo history) keeps the smoothed profile rather than the harder interactive preview. The
       * order also decides which nodes the step covers, since this pass is the last one to widen
       * `CurvePatchApplyState::all_touched_nodes`. See
       * `docs/superpowers/specs/2026-07-18-curve-patch-final-quality-design.md`. */
      curve_patch_set_final_quality(*patch, true);
      curve_patch_restore_and_restamp(*C, ob, *patch);
      curve_patch_set_final_quality(*patch, false);

      /* That re-stamp is the last chance to notice that a foreign operator changed the mesh's
       * element count (see `CurvePatchApplyState::element_num`), and the commit key reaches this
       * function without passing the modal's own check for it. A patch in that state writes
       * nothing: the commit below and the restore above are both no-ops, so report it as
       * canceled rather than finished. */
      if (!patch->apply.invalidated) {
        /* Builds and closes the patch's single position undo step, then optionally writes the face
         * set as a second one -- see `curve_patch_finish_commit()`. */
        curve_patch_finish_commit(*C, ob, *patch);

        /* The re-stamp ends in `flush_update_step()`, which only arms the fast paint-redraw path;
         * that is torn down the instant this operator finishes. Issue the full finished-stroke
         * redraw so the committed positions actually reach the screen -- same reasoning as the
         * initial preview stamp in `curve_patch_publish_and_launch_modal()`. */
        flush_update_done(C, ob, UpdateType::Position);
        committed = true;
      }
    }
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    MEM_delete(patch);
    ss.curve_patch_session = nullptr;
  }

  CurvePatchEditOpData *op_data = static_cast<CurvePatchEditOpData *>(op->customdata);
  if (op_data) {
    if (op_data->sync_timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), op_data->sync_timer);
    }
    curve_patch_edit_snap_ctx_free(*op_data);
  }
  MEM_delete(op_data);
  op->customdata = nullptr;
  WM_event_remove_modal_handler_other_windows(C, op);
  return committed;
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

/* -------------------------------------------------------------------- */
/** \name Context-Menu Operators
 *
 * The Curve Patch actions that exist as real `wmOperatorType`s (see this file's header comment for
 * why everything else is a plain call inside the modal): a popup menu can invoke operators and
 * nothing else. The two point actions act on `CurvePatchEditState::active_point`, which the modal sets
 * from the right-click hit test just before opening the menu; the rest act on the whole patch.
 *
 * None carries `OPTYPE_UNDO`: like the modal's own hotkeys they only mutate the live patch and its
 * session-local history (`CurvePatchEditState::undo_steps`), which is not Blender's undo stack. The
 * modal touches that stack at no point in its life -- the patch's single step is built when it
 * commits, in `curve_patch_finish_commit()`. An `OPTYPE_UNDO` here would push a step for an edit
 * that is not part of the mesh yet.
 * \{ */

static bool curve_patch_active_point_poll(bContext *C)
{
  if (!curve_patch_edit_poll(C)) {
    return false;
  }
  return curve_patch_active_point_is_valid(patch_cache_of(C));
}

static wmOperatorStatus curve_patch_handle_type_set_exec(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  CurvePatchSession &patch = patch_cache_of(C);
  curve_patch_set_active_handle_type(
      *C, ob, patch, ed::curves::SetHandleType(RNA_enum_get(op->ptr, "type")));
  return OPERATOR_FINISHED;
}

void SCULPT_OT_curve_patch_handle_type_set(wmOperatorType *ot)
{
  ot->name = "Set Curve Patch Handle Type";
  ot->description = "Set the handle type of the active Curve Patch control point";
  ot->idname = "SCULPT_OT_curve_patch_handle_type_set";

  ot->invoke = WM_menu_invoke;
  ot->exec = curve_patch_handle_type_set_exec;
  ot->poll = curve_patch_active_point_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          ed::curves::rna_enum_set_handle_type_items,
                          int(ed::curves::SetHandleType::Auto),
                          "Type",
                          nullptr);
}

static wmOperatorStatus curve_patch_delete_point_exec(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  CurvePatchSession &patch = patch_cache_of(C);
  if (!curve_patch_delete_active_point(*C, ob, patch, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_curve_patch_delete_point(wmOperatorType *ot)
{
  ot->name = "Delete Curve Patch Point";
  ot->description = "Delete the active Curve Patch control point";
  ot->idname = "SCULPT_OT_curve_patch_delete_point";

  ot->exec = curve_patch_delete_point_exec;
  ot->poll = curve_patch_active_point_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

static wmOperatorStatus curve_patch_toggle_cyclic_exec(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);
  CurvePatchSession &patch = patch_cache_of(C);
  if (!curve_patch_toggle_cyclic(*C, ob, patch)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_curve_patch_toggle_cyclic(wmOperatorType *ot)
{
  ot->name = "Toggle Curve Patch Cyclic";
  ot->description = "Close or re-open the Curve Patch control curve";
  ot->idname = "SCULPT_OT_curve_patch_toggle_cyclic";

  ot->exec = curve_patch_toggle_cyclic_exec;
  /* Unlike the two point operators this one acts on the whole curve, so it needs no active point. */
  ot->poll = curve_patch_edit_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

static wmOperatorStatus curve_patch_stamp_reseed_exec(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);
  CurvePatchSession &patch = patch_cache_of(C);
  if (!curve_patch_reseed_stamps(*C, ob, patch)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_curve_patch_stamp_reseed(wmOperatorType *ot)
{
  ot->name = "Reseed Curve Patch Stamps";
  ot->description = "Roll a new random layout for the Curve Patch stamps";
  ot->idname = "SCULPT_OT_curve_patch_stamp_reseed";

  ot->exec = curve_patch_stamp_reseed_exec;
  /* Acts on the whole patch rather than a point, like #SCULPT_OT_curve_patch_toggle_cyclic. The
   * Ribbon-mode refusal lives in the exec, not here: a poll that fails would grey the menu entry
   * out, and the entry is simply omitted in that mode instead. */
  ot->poll = curve_patch_edit_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
