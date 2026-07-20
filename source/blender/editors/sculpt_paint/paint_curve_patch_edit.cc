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

#include <cstring>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "MEM_guardedalloc.h"

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
#include "BKE_image.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

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
#include "paint_curve_patch_cache.hh"

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
  for (const BrushCurvePatchTextureSlot &slot : brush.curve_patch_texture_slots) {
    digest = (digest ^ uint64_t(uintptr_t(slot.tex))) * 1099511628211ull;
    uint32_t weight_bits;
    memcpy(&weight_bits, &slot.weight, sizeof(weight_bits));
    digest = (digest ^ uint64_t(weight_bits)) * 1099511628211ull;
  }
  return digest;
}

/* Pointer-only counterpart to the digest above, folding in each slot's `tex` pointer but not its
 * weight. A weight-only edit re-stamps (it changes `curve_patch_texture_list_digest()`) but does
 * not change WHICH images are sampled, so it must not by itself invalidate `ss.tex_pool` -- this
 * narrower digest is what the pool-rebuild gate watches instead. Same Horner-style combinator as
 * above, so a pure slot reorder still changes it. */
static uint64_t curve_patch_texture_pointer_digest(const Brush &brush)
{
  uint64_t digest = 1469598103934665603ull;
  for (const BrushCurvePatchTextureSlot &slot : brush.curve_patch_texture_slots) {
    digest = (digest ^ uint64_t(uintptr_t(slot.tex))) * 1099511628211ull;
  }
  return digest;
}

/* NOTE: the active point index lives on `CurvePatchCache`, not here -- the context menu's operators
 * cannot reach a running modal's `op->customdata`. See `CurvePatchCache::active_point`. */
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
  /* Last end-falloff state (`MTex::curve_patch_end_falloff` / `curve_patch_end_falloff_percent`)
   * this modal re-stamped with. Handled exactly like the length values above: read live, copied
   * into `frozen_params` and re-stamped on change, so switching Hard/Smooth or dragging the
   * Falloff Length slider fades the strip's ends in real time. Seeded at invoke. */
  int last_synced_end_falloff_mode = -1;
  int last_synced_end_falloff_percent = -1;
  /* Last brush Size this modal re-stamped with. Unlike every other frozen param, the radius used to
   * be captured once and never revisited, so dragging the Size slider mid-patch did nothing. It is
   * watched here and converted back to a world radius through `frozen_params.radius_per_size`. The
   * radius is part of `ribbon_source_hash()`, so the ribbon and its LUT rebuild on change; the
   * untouched `orig_positions` still restores the surface correctly first. Seeded at invoke. */
  int last_synced_brush_size = -1;
  /* Last Stamps-mode state this modal re-stamped with: the mode itself, the two randomization
   * amounts, and the brush Spacing/Jitter the layout reads. Handled exactly like the length values
   * above -- read live, copied into `frozen_params` and re-stamped on change. Seeded at invoke. */
  int last_synced_stamp_mode = -1;
  int last_synced_stamp_projection = -1;
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
  int last_synced_stamp_size_random = -1;
  int last_synced_stamp_strength_random = -1;
  int last_synced_spacing = -1;
  float last_synced_jitter = -1.0f;
  /* `last_synced_jitter` above stores whichever of `Brush::jitter` / `jitter_absolute` is currently
   * active, so the `BRUSH_ABSOLUTE_JITTER` flag has to be watched on its own: with the two fields
   * numerically equal, toggling the flag changes the world-space jitter (a fraction of the radius
   * versus a pixel count scaled by `radius_per_size`) without moving the stored value. Seeded at
   * invoke. */
  bool last_synced_jitter_absolute = false;
  /* Last stamp ROTATION state this modal re-stamped with: `MTex::rot` (the fixed texture angle) and
   * `MTex::random_angle` (the Random Rotation slider the Stamps panel exposes). Both are read live
   * off the brush by `curve_patch_stamps_build()` on every re-stamp, so like the texture watch above
   * they only need a re-stamp TRIGGER here -- without one, dragging Random Rotation would appear
   * dead until some unrelated edit happened to fire a re-stamp. Seeded at invoke. */
  float last_synced_stamp_rot = -1.0f;
  float last_synced_stamp_random_angle = -1.0f;
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
/* -------------------------------------------------------------------- */
/** \name Shared Active-Point Actions
 *
 * Bodies shared by the modal's own hotkeys and by the context-menu operators further down, so the
 * two entry points cannot drift apart.
 * \{ */

/* Defined below; the cyclic toggle refreshes the status bar, which spells out what C will do next. */
static void curve_patch_edit_status_set(bContext *C, const CurvePatchCache &patch);

/* -------------------------------------------------------------------- */
/** \name Session-Local Undo
 *
 * See `CurvePatchCache::undo_steps` for why this cannot go through Blender's own undo systems.
 * \{ */

/* Deep enough for any realistic editing session; the snapshots are a handful of control points
 * each, so the cap exists to bound a pathological session rather than to save meaningful memory. */
static constexpr int CURVE_PATCH_UNDO_STEPS_MAX = 64;

/* Record the CURRENT state as a new step. Called after an action completes -- once per action, not
 * once per event, so a drag is a single step. */
static void curve_patch_undo_push(CurvePatchCache &patch)
{
  /* Anything above the cursor is a redo branch the new edit invalidates. */
  patch.undo_steps.resize(patch.undo_step_current + 1);

  CurvePatchEditStep step;
  step.curve = patch.control_curve;
  step.swap_axis = patch.frozen_params.swap_axis;
  step.stamp_seed = patch.frozen_params.stamp_seed;
  patch.undo_steps.append(std::move(step));

  if (patch.undo_steps.size() > CURVE_PATCH_UNDO_STEPS_MAX) {
    patch.undo_steps.remove(0);
  }
  patch.undo_step_current = int(patch.undo_steps.size()) - 1;
}

static void curve_patch_undo_restore(bContext &C, Object &ob, CurvePatchCache &patch)
{
  const CurvePatchEditStep &step = patch.undo_steps[patch.undo_step_current];
  patch.control_curve = step.curve;
  patch.frozen_params.swap_axis = step.swap_axis;
  patch.frozen_params.stamp_seed = step.stamp_seed;
  /* The restored curve may hold fewer points than the one just replaced. */
  patch.active_point = -1;

  curve_patch_edit_status_set(&C, patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  ED_region_tag_redraw(CTX_wm_region(&C));
}

/* Returns false when there is nothing left to undo, i.e. the session is back at the state the
 * anchor stroke produced. The caller then cancels the patch outright. */
static bool curve_patch_undo_step_back(bContext &C, Object &ob, CurvePatchCache &patch)
{
  if (patch.undo_step_current <= 0) {
    return false;
  }
  patch.undo_step_current--;
  curve_patch_undo_restore(C, ob, patch);
  return true;
}

static void curve_patch_undo_step_forward(bContext &C, Object &ob, CurvePatchCache &patch)
{
  if (patch.undo_step_current + 1 >= int(patch.undo_steps.size())) {
    return;
  }
  patch.undo_step_current++;
  curve_patch_undo_restore(C, ob, patch);
}

/** \} */

static bool curve_patch_active_point_is_valid(const CurvePatchCache &patch)
{
  return patch.active_point >= 0 && paintcurve_geometry_is_valid(patch.control_curve) &&
         patch.active_point < patch.control_curve.points_num();
}

/* Returns false (having reported why) when the point cannot be removed. */
static bool curve_patch_delete_active_point(bContext &C,
                                            Object &ob,
                                            CurvePatchCache &patch,
                                            ReportList *reports)
{
  if (!curve_patch_active_point_is_valid(patch)) {
    return false;
  }
  bke::CurvesGeometry &geom = patch.control_curve;
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

  const int point = patch.active_point;
  IndexMaskMemory memory;
  const IndexMask delete_mask = IndexMask::from_indices<int>(Span<int>(&point, 1), memory);
  paintcurve_geometry_remove_points(geom, delete_mask);
  patch.active_point = -1;

  curve_patch_undo_push(patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  ED_region_tag_redraw(CTX_wm_region(&C));
  return true;
}

static void curve_patch_set_active_handle_type(bContext &C,
                                               Object &ob,
                                               CurvePatchCache &patch,
                                               const ed::curves::SetHandleType dst_type)
{
  bke::CurvesGeometry &geom = patch.control_curve;
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  types_left[patch.active_point] = paintcurve_resolve_handle_type(types_left[patch.active_point],
                                                                  dst_type);
  types_right[patch.active_point] = paintcurve_resolve_handle_type(types_right[patch.active_point],
                                                                   dst_type);
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();

  curve_patch_undo_push(patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  ED_region_tag_redraw(CTX_wm_region(&C));
}

/* Close or re-open the control curve. Returns false only when there is no usable curve at all. */
static bool curve_patch_toggle_cyclic(bContext &C, Object &ob, CurvePatchCache &patch)
{
  bke::CurvesGeometry &geom = patch.control_curve;
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
  ED_region_tag_redraw(CTX_wm_region(&C));
  return true;
}

/* Roll a new random layout for the Stamps-mode relief. Returns false in Ribbon mode, which has no
 * randomization for a seed to drive. */
static bool curve_patch_reseed_stamps(bContext &C, Object &ob, CurvePatchCache &patch)
{
  if (patch.frozen_params.stamp_mode != MTEX_CURVE_PATCH_STAMP_STAMPS) {
    return false;
  }
  /* Alongside `curve_patch_begin_editing()` the only place a stateful RNG is touched: every
   * per-stamp offset downstream is a pure hash of this seed, so re-rolling it here is what makes
   * the whole layout change while every other input stays put. */
  patch.frozen_params.stamp_seed = RandomNumberGenerator::from_random_seed().get_uint32();

  curve_patch_undo_push(patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  ED_region_tag_redraw(CTX_wm_region(&C));
  return true;
}

static bool curve_patch_is_cyclic(const CurvePatchCache &patch)
{
  const bke::CurvesGeometry &geom = patch.control_curve;
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

  const CurvePatchCache &patch = patch_cache_of(C);
  const bool is_cyclic = curve_patch_is_cyclic(patch);
  layout.separator();
  layout.op("SCULPT_OT_curve_patch_toggle_cyclic",
            is_cyclic ? IFACE_("Open Curve") : IFACE_("Close Curve"),
            ICON_NONE);

  /* Ribbon mode has no randomization, so the entry would poll false and only ever show greyed
   * out -- leave it out entirely there. */
  if (patch.frozen_params.stamp_mode == MTEX_CURVE_PATCH_STAMP_STAMPS) {
    layout.op("SCULPT_OT_curve_patch_stamp_reseed", std::nullopt, ICON_NONE);
  }

  const ToolSettings *tool_settings = CTX_data_tool_settings(C);
  Brush *brush = (tool_settings && tool_settings->sculpt) ?
                     BKE_paint_brush(&tool_settings->sculpt->paint) :
                     nullptr;
  if (brush != nullptr) {
    PointerRNA tex_slot_ptr = RNA_pointer_create_discrete(
        &brush->id, RNA_BrushTextureSlot, &brush->mtex);
    layout.separator();
    /* A closed curve has no ends to fade, and the relief ignores the setting there -- so do not
     * offer it. The brush panel still shows it: it has no access to the live patch, and the setting
     * remains meaningful for every open one. */
    if (!is_cyclic) {
      layout.prop(&tex_slot_ptr,
                  "curve_patch_end_falloff",
                  UI_ITEM_NONE,
                  IFACE_("End Falloff"),
                  ICON_NONE);
      if (brush->mtex.curve_patch_end_falloff == MTEX_CURVE_PATCH_END_SMOOTH) {
        layout.prop(&tex_slot_ptr,
                    "curve_patch_end_falloff_length",
                    UI_ITEM_NONE,
                    IFACE_("Falloff Length"),
                    ICON_NONE);
      }
    }
    layout.prop(&tex_slot_ptr,
                "use_curve_patch_swap_axis",
                UI_ITEM_NONE,
                IFACE_("Swap Texture Axis"),
                ICON_NONE);
  }

  layout.separator();
  layout.op("SCULPT_OT_curve_patch_delete_point", std::nullopt, ICON_NONE);
  ui::popup_menu_end(C, pup);
}

/** \} */

static void curve_patch_edit_status_set(bContext *C, const CurvePatchCache &patch)
{
  std::string msg = fmt::format(
      "Enter: Commit | Esc: Cancel | Ctrl+Z: Undo | S: Swap Texture Axis (currently {}) | C: {} "
      "Curve",
      patch.frozen_params.swap_axis ? "U" : "V",
      curve_patch_is_cyclic(patch) ? "Open" : "Close");
  /* Reseed has no shortcut -- this modal has no free key left -- so advertise the route that does
   * reach it. Meaningless in Ribbon mode, which has nothing random to re-roll. */
  if (patch.frozen_params.stamp_mode == MTEX_CURVE_PATCH_STAMP_STAMPS) {
    msg += " | RMB Menu: Reseed Stamps";
  }
  ED_workspace_status_text(C, msg.c_str());
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
      data->last_synced_length_mode = brush->mtex.curve_patch_length_mode;
      data->last_synced_length_repeat = brush->mtex.curve_patch_length_repeat;
      data->last_synced_end_falloff_mode = brush->mtex.curve_patch_end_falloff;
      data->last_synced_end_falloff_percent = brush->mtex.curve_patch_end_falloff_percent;
      data->last_synced_brush_size = BKE_brush_size_get(&sd.paint, brush);
      data->last_synced_stamp_mode = brush->mtex.curve_patch_stamp_mode;
      data->last_synced_stamp_projection = brush->mtex.curve_patch_stamp_projection;
      data->last_synced_stamp_tex_source = brush->mtex.curve_patch_stamp_texture_source;
      data->last_synced_ribbon_tex_source = brush->mtex.curve_patch_ribbon_texture_source;
      data->last_synced_cap_start_length = brush->curve_patch_cap_start_length;
      data->last_synced_cap_end_length = brush->curve_patch_cap_end_length;
      data->last_synced_texture_list_digest = curve_patch_texture_list_digest(*brush);
      data->last_synced_texture_pointer_digest = curve_patch_texture_pointer_digest(*brush);
      data->last_synced_cap_tex[0] = brush->curve_patch_tex_start;
      data->last_synced_cap_tex[1] = brush->curve_patch_tex_middle;
      data->last_synced_cap_tex[2] = brush->curve_patch_tex_end;
      data->last_synced_stamp_size_random = brush->mtex.curve_patch_stamp_size_random;
      data->last_synced_stamp_strength_random = brush->mtex.curve_patch_stamp_strength_random;
      data->last_synced_spacing = brush->spacing;
      data->last_synced_jitter = (brush->flag & BRUSH_ABSOLUTE_JITTER) ? brush->jitter_absolute :
                                                                          brush->jitter;
      data->last_synced_jitter_absolute = (brush->flag & BRUSH_ABSOLUTE_JITTER) != 0;
      data->last_synced_stamp_rot = brush->mtex.rot;
      data->last_synced_stamp_random_angle = brush->mtex.random_angle;
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
  /* Snapshot the active tool of the viewport this patch belongs to (see
   * `CurvePatchEditOpData::tool_idname_at_invoke`). `CTX_wm_area()` is that viewport both here and
   * in the modal, which reads the frozen area captured at this moment. */
  if (const ScrArea *area = CTX_wm_area(C)) {
    if (const bToolRef *tref = area->runtime.tool) {
      STRNCPY(data->tool_idname_at_invoke, tref->idname);
    }
  }
  WM_event_add_modal_handler(C, op);
  /* Drive the live-sync poll at a steady cadence (see `CurvePatchEditOpData::sync_timer`). 20 Hz:
   * fast enough that a panel change feels immediate, light enough that idle ticks -- which only
   * compare scalars and re-stamp on an actual change -- cost nothing. */
  const double sync_timer_step = 0.05;
  data->sync_timer = WM_event_timer_add(
      CTX_wm_manager(C), CTX_wm_window(C), TIMER, sync_timer_step);
  CurvePatchCache &patch = patch_cache_of(C);
  /* Seed the session undo stack with the state the anchor stroke produced. Ctrl+Z walks back to
   * this entry and, once there, cancels the patch instead of stepping further. */
  patch.undo_steps.clear();
  patch.undo_step_current = -1;
  curve_patch_undo_push(patch);
  curve_patch_edit_status_set(C, patch);
  return OPERATOR_RUNNING_MODAL;
}

/* Returns true when the patch was actually committed. A commit request can still end up writing
 * nothing -- see the `CurvePatchCache::invalidated` branch inside -- and the modal reports that as
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
    curve_patch_edit_snap_ctx_free(data);
    MEM_delete(&data);
    op->customdata = nullptr;
    return OPERATOR_CANCELLED;
  }

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
      const int end_falloff_mode = brush->mtex.curve_patch_end_falloff;
      const int end_falloff_percent = brush->mtex.curve_patch_end_falloff_percent;
      /* Stamps-mode state and the brush Spacing/Jitter the stamp layout reads (see
       * `curve_patch_stamp_layout_build()`, `paint_curve_patch_cache.cc`) are watched the same way:
       * read live, copied into `frozen_params` (bar Spacing/Jitter, which the layout already reads
       * straight off the live brush) and used only as a re-stamp trigger here. */
      const int brush_size = BKE_brush_size_get(&sd.paint, brush);
      const int stamp_mode = brush->mtex.curve_patch_stamp_mode;
      const int stamp_projection = brush->mtex.curve_patch_stamp_projection;
      const int stamp_size_random = brush->mtex.curve_patch_stamp_size_random;
      const int stamp_strength_random = brush->mtex.curve_patch_stamp_strength_random;
      const int spacing = brush->spacing;
      const float jitter = (brush->flag & BRUSH_ABSOLUTE_JITTER) ? brush->jitter_absolute :
                                                                     brush->jitter;
      /* Watched separately from `jitter` above, which only ever holds the ACTIVE one of the two
       * fields: with equal values, the toggle alone still changes the world-space jitter. */
      const bool jitter_absolute = (brush->flag & BRUSH_ABSOLUTE_JITTER) != 0;
      /* Fixed texture angle and Random Rotation amount. `curve_patch_stamps_build()` reads both
       * live off the brush, so these are pure re-stamp triggers -- nothing to copy into
       * `frozen_params`. */
      const float stamp_rot = brush->mtex.rot;
      const float stamp_random_angle = brush->mtex.random_angle;
      /* Multi-texture settings. The relief reads all of them live off the brush and the cache, so
       * these exist purely to trigger a re-stamp -- nothing is copied into `frozen_params`. */
      const int stamp_tex_source = brush->mtex.curve_patch_stamp_texture_source;
      const int ribbon_tex_source = brush->mtex.curve_patch_ribbon_texture_source;
      const float cap_start_length = brush->curve_patch_cap_start_length;
      const float cap_end_length = brush->curve_patch_cap_end_length;
      const uint64_t texture_list_digest = curve_patch_texture_list_digest(*brush);
      const uint64_t texture_pointer_digest = curve_patch_texture_pointer_digest(*brush);
      const void *cap_tex[3] = {brush->curve_patch_tex_start,
                                brush->curve_patch_tex_middle,
                                brush->curve_patch_tex_end};
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
       * same images. Drives the `ss.tex_pool` rebuild gate below; a cap-length or weight-only change
       * must not free/reallocate the pool on every tick of a slider drag. */
      const bool texture_identity_changed =
          texture_pointer_digest != data.last_synced_texture_pointer_digest ||
          cap_tex[0] != data.last_synced_cap_tex[0] ||
          cap_tex[1] != data.last_synced_cap_tex[1] ||
          cap_tex[2] != data.last_synced_cap_tex[2];
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
          length_repeat != data.last_synced_length_repeat ||
          end_falloff_mode != data.last_synced_end_falloff_mode ||
          end_falloff_percent != data.last_synced_end_falloff_percent ||
          symm != data.last_synced_symm ||
          swap_axis != data.last_synced_swap_axis || tex_changed ||
          falloff_preset != data.last_synced_falloff_preset ||
          falloff_curve_ts != data.last_synced_falloff_curve_ts ||
          brush_size != data.last_synced_brush_size ||
          stamp_mode != data.last_synced_stamp_mode ||
          stamp_projection != data.last_synced_stamp_projection ||
          stamp_size_random != data.last_synced_stamp_size_random ||
          stamp_strength_random != data.last_synced_stamp_strength_random ||
          spacing != data.last_synced_spacing || jitter != data.last_synced_jitter ||
          jitter_absolute != data.last_synced_jitter_absolute ||
          stamp_rot != data.last_synced_stamp_rot ||
          stamp_random_angle != data.last_synced_stamp_random_angle || multi_texture_changed)
      {
        data.last_synced_alpha = alpha;
        data.last_synced_dir_in = dir_in;
        data.last_synced_length_mode = length_mode;
        data.last_synced_length_repeat = length_repeat;
        data.last_synced_end_falloff_mode = end_falloff_mode;
        data.last_synced_end_falloff_percent = end_falloff_percent;
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
        patch.frozen_params.end_falloff_mode = end_falloff_mode;
        patch.frozen_params.end_falloff_percent = end_falloff_percent;
        patch.frozen_params.swap_axis = swap_axis;
        data.last_synced_brush_size = brush_size;
        data.last_synced_stamp_mode = stamp_mode;
        data.last_synced_stamp_projection = stamp_projection;
        data.last_synced_stamp_size_random = stamp_size_random;
        data.last_synced_stamp_strength_random = stamp_strength_random;
        data.last_synced_spacing = spacing;
        data.last_synced_jitter = jitter;
        data.last_synced_jitter_absolute = jitter_absolute;
        data.last_synced_stamp_rot = stamp_rot;
        data.last_synced_stamp_random_angle = stamp_random_angle;
        data.last_synced_stamp_tex_source = stamp_tex_source;
        data.last_synced_ribbon_tex_source = ribbon_tex_source;
        data.last_synced_cap_start_length = cap_start_length;
        data.last_synced_cap_end_length = cap_end_length;
        data.last_synced_texture_list_digest = texture_list_digest;
        data.last_synced_texture_pointer_digest = texture_pointer_digest;
        data.last_synced_cap_tex[0] = cap_tex[0];
        data.last_synced_cap_tex[1] = cap_tex[1];
        data.last_synced_cap_tex[2] = cap_tex[2];
        patch.frozen_params.stamp_mode = stamp_mode;
        patch.frozen_params.stamp_projection = stamp_projection;
        patch.frozen_params.stamp_size_random = float(stamp_size_random) / 100.0f;
        patch.frozen_params.stamp_strength_random = float(stamp_strength_random) / 100.0f;
        /* A zero ratio means the patch started with a zero brush size, which cannot happen through
         * the UI; guard anyway rather than collapse the patch to nothing. */
        if (patch.frozen_params.radius_per_size > 0.0f) {
          patch.frozen_params.radius = patch.frozen_params.radius_per_size * float(brush_size);
        }
        /* The relief samples the texture through `ss.tex_pool`, an `ImagePool` that caches ImBuf
         * handles for the whole sculpt session (`brush_init_tex`, `sculpt.cc`). A changed image --
         * a different Image datablock on the texture, or edited pixels -- would otherwise keep
         * sampling the old buffer. Rebuild the pool whenever the set of sampled textures' IDENTITY
         * changes: the brush's own texture (`tex_changed`), or a cap/list texture reassignment
         * (`texture_identity_changed`, a list/cap counterpart of the same idea). Deliberately NOT
         * gated on the broader `multi_texture_changed`: a cap-length drag or a slot WEIGHT edit
         * re-stamps (see `multi_texture_changed` above) but samples the same images as before, so
         * rebuilding the pool for those would only free and reallocate it needlessly on every tick
         * of a slider drag. */
        if (tex_changed || texture_identity_changed) {
          ImagePool *&tex_pool = ob.runtime->sculpt_session->tex_pool;
          if (tex_pool != nullptr) {
            BKE_image_pool_free(tex_pool);
          }
          tex_pool = BKE_image_pool_new();
        }
        /* `swap_axis` and `stamp_mode` both feed the status text, so it has to be rebuilt here and
         * not only where the modal's own hotkeys change them. */
        curve_patch_edit_status_set(C, patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
    }
  }

  /* A foreign operator changed the mesh's element count (see `CurvePatchCache::element_num`). The
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
  if (patch.invalidated) {
    curve_patch_edit_finish(C, op, true);
    return OPERATOR_CANCELLED;
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
  const ARegion *region_hovered = area ? ED_area_find_region_xy_visual(
                                             area, RGN_TYPE_ANY, event->xy) :
                                         nullptr;
  const bool over_our_region = region && (region_hovered == region);
  const bool should_pass_through = !is_dragging && !over_our_region && !is_commit_key;
  if (should_pass_through) {
    return OPERATOR_PASS_THROUGH;
  }

  /* Below this point the modal only ever ends via an explicit Enter (commit) or Esc (cancel) -- no
   * click and no mouse-leaving-the-viewport implicitly ends it. Events that don't match anything
   * below are simply consumed like any other event while the patch is live; the user must press
   * Enter/Esc first. A tool or brush switch is the one exception, and it is handled by the poll at
   * the top of this function rather than here: it arrives as a click on the toolbar, which the
   * pass-through gate above hands to the toolbar before any case here could see it. */

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
          patch.active_point = radius_hit;
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

        patch.active_point = hit;
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
        curve_patch_edit_snap_ctx_free(data);
        /* One undo step per drag, recorded on release -- not one per MOUSEMOVE, which would fill
         * the stack with intermediate positions nobody wants to step through. */
        curve_patch_undo_push(patch);
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
        const float3 &pivot = geom.positions()[patch.active_point];
        float pivot_world[3];
        mul_v3_m4v3(pivot_world, ob_to_world, pivot);

        const float mval_init[2] = {data.drag_start_mval.x, data.drag_start_mval.y};
        const float mval_curr[2] = {float(event->mval[0]), float(event->mval[1])};

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
          paintcurve_geom_set_surface_normal(geom, patch.active_point, float3(hit_no_obj));
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
              paintcurve_geom_co(geom, patch.active_point, h), obj_delta, data.point_initial_loc_3d[h]);
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
        patch.control_curve.radius_for_write()[patch.active_point] = new_radius;
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
        const float3 &pivot = geom.positions()[patch.active_point];
        float pivot_world[3];
        mul_v3_m4v3(pivot_world, ob_to_world, pivot);

        const float mval_curr[2] = {float(event->mval[0]), float(event->mval[1])};
        float world_curr[3];
        ED_view3d_win_to_3d(vc.v3d, vc.region, pivot_world, mval_curr, world_curr);
        mul_v3_m4v3(paintcurve_geom_co(geom, patch.active_point, data.handle_is_left ? 0 : 2),
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
          if (ELEM(types[patch.active_point], BEZIER_HANDLE_AUTO, BEZIER_HANDLE_VECTOR)) {
            types[patch.active_point] = BEZIER_HANDLE_ALIGN;
          }
        }
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
      /* Plain right-click over a control point opens the context menu -- the equivalent of Paint
       * Curve's #PAINTCURVE_OT_context_menu, which cannot be reused here because every
       * `PAINTCURVE_OT_*` operator resolves its data through a `PaintCurve` ID and this editor's
       * control curve is a standalone `CurvesGeometry`. Ctrl+RMB keeps its insert/append meaning
       * below; plain RMB away from a point stays the no-op it has always been. */
      if (event->val == KM_PRESS && (event->modifier & KM_CTRL) == 0) {
        const bke::CurvesGeometry &geom = patch.control_curve;
        if (paintcurve_geometry_is_valid(geom)) {
          Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
          ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
          Vector<PaintCurvePoint> screen_points;
          paintcurve_build_screen_points_from_geometry(geom, true, &vc, screen_points);

          const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};
          char selflag = 0;
          const int hit = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                           loc_fl,
                                                           /*ignore_pivot=*/false,
                                                           PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                           &selflag);
          /* A hit on either tangent handle counts as a hit on its point -- same as the pivot,
           * since every menu entry acts on the whole point. */
          if (hit >= 0) {
            patch.active_point = hit;
            curve_patch_edit_context_menu_open(C);
          }
        }
        break;
      }
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
                patch.active_point = insert_index;
                did_insert = true;
              }
            }
          }
        }

        if (!did_insert) {
          /* Append: extend the single spline at its end (or start it, if empty). Uses the same
           * placement chain as #paintcurve_point_add so points land on the surface here too;
           * `patch.plane_normal` only stands in when no real surface was hit. */
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
                                        placed ? float3(obj_no) : patch.plane_normal,
                                        create_new_spline,
                                        active_curve,
                                        add_index);
          patch.active_point = geom.points_num() - 1;
        }

        curve_patch_undo_push(patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case EVT_XKEY:
    case EVT_DELKEY:
      if (event->val == KM_PRESS &&
          curve_patch_delete_active_point(*C, ob, patch, op->reports))
      {
        /* The removed point may have been the one under an in-flight drag; drop the drag state so
         * the next MOUSEMOVE does not index into geometry that no longer has it. */
        data.dragging_point = false;
        data.dragging_radius = false;
        data.dragging_handle = false;
        data.dragging_segment = false;
        curve_patch_edit_snap_ctx_free(data);
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
    case EVT_SKEY:
      /* Deliberately NOT Tab, which this used to answer to as well: Tab belongs to the mode toggle,
       * and swallowing it left no way to leave Sculpt Mode for the whole session. X is not an
       * option either -- it deletes the active point (see `EVT_XKEY` above), per Blender
       * convention. */
      if (event->val == KM_PRESS) {
        patch.frozen_params.swap_axis = !patch.frozen_params.swap_axis;
        curve_patch_undo_push(patch);
        curve_patch_edit_status_set(C, patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
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
  CurvePatchCache *patch = ss.curve_patch_cache;

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
    }
    else {
      /* Re-stamp once at final quality BEFORE the undo step is built, so the mesh (and with it the
       * undo history) keeps the smoothed profile rather than the harder interactive preview. The
       * order also decides which nodes the step covers, since this pass is the last one to widen
       * `CurvePatchCache::all_touched_nodes`. See
       * `docs/superpowers/specs/2026-07-18-curve-patch-final-quality-design.md`. */
      patch->final_quality = true;
      curve_patch_restore_and_restamp(*C, ob, *patch);
      patch->final_quality = false;

      /* That re-stamp is the last chance to notice that a foreign operator changed the mesh's
       * element count (see `CurvePatchCache::element_num`), and the commit key reaches this
       * function without passing the modal's own check for it. A patch in that state writes
       * nothing: the commit below and the restore above are both no-ops, so report it as
       * canceled rather than finished. */
      if (!patch->invalidated) {
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
    ss.curve_patch_cache = nullptr;
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
 * nothing else. The two point actions act on `CurvePatchCache::active_point`, which the modal sets
 * from the right-click hit test just before opening the menu; the rest act on the whole patch.
 *
 * None carries `OPTYPE_UNDO`: like the modal's own hotkeys they only mutate the live patch and its
 * session-local history (`CurvePatchCache::undo_steps`), which is not Blender's undo stack. The
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
  CurvePatchCache &patch = patch_cache_of(C);
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
  CurvePatchCache &patch = patch_cache_of(C);
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
  CurvePatchCache &patch = patch_cache_of(C);
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
  CurvePatchCache &patch = patch_cache_of(C);
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
