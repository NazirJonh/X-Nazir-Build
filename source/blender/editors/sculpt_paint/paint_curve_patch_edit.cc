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
 * toggle, and Enter/Esc commit/cancel. Mouse interaction stays as `switch (event->type)` because
 * hit-tests depend on cursor position. Keyboard actions go through a `WM_modalkeymap` attached to
 * this operator (`paint_curve_patch_edit_keymap.cc`), the same pattern as `PAINTCURVE_OT_slide`.
 * Session-local undo lives in `paint_curve_patch_edit_undo.cc`; multi-viewport context sync in
 * `paint_curve_patch_edit_sync.cc`.
 *
 * That is NOT a tool `wmKeyMap` of small operators. An earlier version split each action into its
 * own short-lived operator bound via a procedural C++ keymap; that left the installed keymap with
 * zero items at dispatch time (`WM_keymap_poll()` warning "empty keymap 'Curve Patch Edit'").
 * Blender's key-config merge is built around keymaps a Python preset knows about. A modal keymap
 * assigned to the operator type does not go through that path; bindings live in
 * `blender_default.py` so merge cannot empty the map, and so the user can rebind them.
 *
 * Entire-point drag uses the shared 3D cores (`paintcurve_apply_point_surface_snap`,
 * `paintcurve_object_delta_from_screen_drag`). Radius drag uses
 * `paintcurve_radius_from_handle_screen_pos`. Handle drag stays local: Paint Curve slide also
 * rotates the opposite handle and rewrites Free/Align; this editor only promotes computed
 * Auto/Vector to Align so a user-chosen Free stays free. Segment hit-tests and insert are the
 * `_from_geometry` cores in `paint_curve_sync.cc`. Overlay handles come from
 * `ED_paint_curve_screen_handles_build_from_geometry` (`paint_curve_draw.cc`).
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_bit_span_ops.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
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
#include "paint_curve_patch_actions.hh"
#include "paint_curve_patch_edit_intern.hh"
#include "paint_curve_patch_editor.hh"
#include "paint_curve_patch_host.hh"
#include "paint_curve_patch_live.hh"
#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint {

/* #curve_patch_live_inputs_capture plus the one live input only a mesh target has. The shared
 * capture deliberately leaves `symm` at its sentinel -- see its doc-string. */
static CurvePatchLiveInputs curve_patch_live_inputs_capture_sculpt(const Paint &paint,
                                                                   const Brush &brush,
                                                                   const Object &ob)
{
  CurvePatchLiveInputs in = curve_patch_live_inputs_capture(paint, brush);
  in.symm = mesh_symmetry_xyz_get(ob);
  return in;
}

/* NOTE: the active point index lives on `CurvePatchSession`, not here -- the context menu's
 * operators cannot reach a running modal's `op->customdata`. See
 * `CurvePatchDocument::active_point`. */
struct CurvePatchEditOpData {
  /* Drag state and the mouse editing state machine, shared with the flat-canvas editor. The host
   * and the screen adapter are NOT stored alongside it: both are built on the stack per event
   * because the object and the session are re-resolved from the context every time. */
  CurvePatchCurveEditor editor;
  /* True while a radius drag started from the modal keymap (Alt+S) rather than from a click. Such
   * a drag has no button held down, so no RELEASE will arrive to end it -- the next click does,
   * the way G/R/S transform modals confirm. */
  bool radius_drag_from_key = false;
  /* Last `BrushCurvePatchSettings::swap_axis` this modal saw ON THE BRUSH -- the one watchdog
   * that is not merely a re-stamp trigger. `CurvePatchParams::swap_axis` is owned by the session
   * as much as by the brush: the Y hotkey and the session undo stack both write it without
   * touching the brush, so the poll may only push the brush's value across when the BRUSH itself
   * changed. That question cannot be answered from the two current values alone. Seeded at invoke.
   */
  bool last_synced_swap_axis = false;
  /* Steady-cadence timer (see `curve_patch_edit_invoke`). The live-sync poll at the top of
   * `curve_patch_edit_modal` only runs on window events, so a discrete brush change made in a
   * panel with no follow-up event -- e.g. picking a texture or image from a browse menu -- would
   * not re-project until the next stray event reached this modal. This timer makes the poll tick
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
   * unchanged across a switch. Empty when no tool was active at invoke, which disables the check.
   */
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
  paintcurve_snap_marker_clear();
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
 * `patch_cache_of()` would then dereference a null `curve_patch_session` on the very first event.
 */
static bool curve_patch_edit_poll(bContext *C)
{
  if (!sculpt_mode_poll(C)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->runtime->sculpt_session && ob->runtime->sculpt_session->curve_patch_session;
}

/* -------------------------------------------------------------------- */
/** \name Geometry-Only Hit Tests
 *
 * Wrappers around the shared `_from_geometry` pick cores (`paint_curve_sync.cc`).
 * \{ */

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

  const int radius_hit = paintcurve_find_radius_handle_at_pos_from_geometry(
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
  if (paintcurve_find_closest_segment_from_geometry(geom,
                                                    true,
                                                    vc,
                                                    {},
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
/** \name Shared Active-Point Actions
 *
 * Bodies shared by the modal's own hotkeys and by the context-menu operators further down, so the
 * two entry points cannot drift apart.
 * \{ */

/** Refresh the paint-curve overlay in every 3D View / Image editor (cheap; per MOUSEMOVE frame).
 */
void curve_patch_tag_overlay_redraw_all(bContext *C)
{
  ED_paint_curve_overlay_tag_redraw_all(C);
}

/** Overlay plus the finished-stroke redraw handshake after a discrete edit (drag release, undo,
 * delete, etc.). Always issues the shared #UpdateType::Position flush; Image/Color effects also
 * need their own type so #flush_update_done reaches the #SPACE_IMAGE loop and the paint-done
 * handshake -- the same reason #curve_patch_finish_commit issues a second effect-typed flush.
 * Without that second call, Texture Paint (Draw brush / image canvas) keeps a stale GPU preview
 * after moving points or segments even though the ImBuf was already re-stamped. */
void curve_patch_tag_viewports_redraw_after_edit(bContext &C,
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

/* Defined below; the host reaches them from inside the shared editing core. */
static void curve_patch_edit_context_menu_open(bContext *C);
static bool curve_patch_edit_session_finish(bContext *C, bool is_cancel);

/**
 * 3D Sculpt Mode's target, both for the shared action bodies (`paint_curve_patch_actions.hh`) and
 * for the shared modal editor (`paint_curve_patch_editor.hh`).
 *
 * Owns nothing and is built on the stack per event: the object and the session are re-resolved
 * from the context every time, so a host stored across events could outlive either.
 */
class SculptCurvePatchHost : public CurvePatchEditorHost {
  Object &ob_;
  CurvePatchSession &patch_;
  ReportList *reports_;

 public:
  SculptCurvePatchHost(Object &ob, CurvePatchSession &patch, ReportList *reports)
      : ob_(ob), patch_(patch), reports_(reports)
  {
  }

  CurvePatchDocument &document() override
  {
    return patch_.doc;
  }

  bke::CurvesGeometry &curve() override
  {
    return patch_.active_item().control_curve;
  }

  /** Every patch of the session, so a click picks the closest point across all of them and makes
   * the patch it belongs to active. */
  void pickable_curves(Vector<bke::CurvesGeometry *> &r_curves) override
  {
    r_curves.clear();
    r_curves.reserve(patch_.doc.patches.size());
    for (CurvePatchItem &item : patch_.doc.patches) {
      r_curves.append(&item.control_curve);
    }
  }

  void pickable_curve_activate(const int index) override
  {
    patch_.doc.active_patch = index;
  }

  /**
   * Drives the overlay's selected-point highlight as well as the index.
   *
   * `hd.selected_center` is read from the geometry's own selection attribute (see
   * #paintcurve_geom_get_selection), not from `active_point`, so without this a click would move
   * the point every drag and hotkey acts on while the diamond never visibly changed color. Matches
   * the classic paint-curve click (`paintcurve.select`, `paint_curve.cc`).
   */
  void active_point_set(const int point_index, const uint8_t sel_bits) override
  {
    patch_.doc.active_point = point_index;
    bke::CurvesGeometry &geom = this->curve();
    paintcurve_geom_set_all_selection(geom, 0);
    if (point_index >= 0) {
      paintcurve_geom_set_selection(geom, point_index, sel_bits);
    }
  }

  /* Overlay tag only, NOT #curve_patch_tag_viewports_redraw_after_edit: this runs once per
   * MOUSEMOVE of a drag, and that helper's `flush_update_done()` would clear the `RV3D_PAINTING`
   * that the `flush_update_step()` at the end of the re-stamp just set -- killing the live
   * preview. The full flush belongs to #interaction_end. */
  void restamp(bContext &C) override
  {
    curve_patch_restore_and_restamp(C, ob_, patch_);
    curve_patch_tag_overlay_redraw_all(&C);
  }

  void interaction_end(bContext &C) override
  {
    curve_patch_tag_viewports_redraw_after_edit(C, ob_, patch_);
  }

  void redraw(bContext &C) override
  {
    /* Hover and the Ctrl+RMB insert preview are drawn by the overlay engine, which only rebuilds
     * on a tagged viewport redraw. This modal consumes events the paint-cursor poll would
     * otherwise deliver, so the tag has to be issued explicitly. */
    curve_patch_tag_overlay_redraw_all(&C);
  }

  void undo_push() override
  {
    curve_patch_undo_push(patch_);
  }

  /* No `undo_step_back` / `undo_step_forward` here: stepping the session history is bound to this
   * mode's own modal keymap (#CURVE_PATCH_MODAL_UNDO / #CURVE_PATCH_MODAL_REDO), which the
   * operator handles before anything reaches the shared editor. */

  void status_refresh(bContext &C) override
  {
    curve_patch_edit_status_set(&C, patch_);
  }

  void commit(bContext &C) override
  {
    curve_patch_edit_session_finish(&C, /*is_cancel=*/false);
  }

  void cancel(bContext &C) override
  {
    curve_patch_edit_session_finish(&C, /*is_cancel=*/true);
  }

  /** The live-brush watchdog runs in the modal itself, above its own pass-through gate, so that a
   * slider dragged in the N-panel is caught even when the event never reaches the editing core.
   * Reporting `false` here keeps the core from polling it a second time. */
  bool sync_live_brush(bContext & /*C*/) override
  {
    return false;
  }

  bool is_alive(bContext & /*C*/) override
  {
    const SculptSession *ss = ob_.runtime->sculpt_session;
    return ss != nullptr && ss->curve_patch_session == &patch_;
  }

  void context_menu_open(bContext &C) override
  {
    curve_patch_edit_context_menu_open(&C);
  }

  ReportList *reports() override
  {
    return reports_;
  }
};

/**
 * The 3D viewport the patch is edited in: everything projects through a #ViewContext, and a whole
 * point stays on the surface while it is dragged.
 *
 * Built on the stack per event -- the object and the session are re-resolved from the context
 * every time -- so the one piece of state that must outlive an event, the snap context, is
 * BORROWED from the operator data rather than owned here. Rebuilding it per mouse move would
 * re-walk the object's snap BVH.
 */
class SculptScreenAdapter : public CurvePatchScreenAdapter {
  Object &ob_;
  CurvePatchSession &patch_;
  /* Refreshed from the context on every use: a window-level modal's region moves between split
   * viewports, and `pick_space_get` hands out a pointer to this. */
  mutable ViewContext vc_ = {};
  PaintCurveSnapContext *&snap_ctx_;

 public:
  SculptScreenAdapter(Object &ob, CurvePatchSession &patch, PaintCurveSnapContext *&snap_ctx)
      : ob_(ob), patch_(patch), snap_ctx_(snap_ctx)
  {
  }

  /** The pickers project object-space positions themselves, so the curve is handed over as-is with
   * a live #ViewContext and no projected copy. */
  bool pick_space_get(bContext &C,
                      const bke::CurvesGeometry &source,
                      bke::CurvesGeometry & /*r_projected_storage*/,
                      CurvePatchPickSpace &r_space) const override
  {
    vc_ = this->view_context_get(C);
    if (vc_.region == nullptr) {
      return false;
    }
    r_space.geom = &source;
    r_space.use_3d_space = true;
    r_space.vc = &vc_;
    return true;
  }

  /**
   * Same placement chain #paintcurve_point_add uses, so a new point lands on the surface here too.
   * The patch's own projection plane normal only stands in when no real surface was hit.
   */
  void place_new_point(bContext &C,
                       const int mval[2],
                       float3 &r_position,
                       float3 &r_normal) const override
  {
    const ViewContext vc = this->view_context_get(C);
    const float loc_fl[2] = {float(mval[0]), float(mval[1])};
    float obj_co[3];
    float obj_no[3];
    const bool placed = paintcurve_surface_place(
        &C, nullptr, vc, loc_fl, nullptr, /*use_depth_fallback=*/true, obj_co, obj_no);
    r_position = float3(obj_co);
    r_normal = placed ? float3(obj_no) : patch_.active_item().params.plane_normal;
  }

  /**
   * Always true: the modal's own pass-through gate (#ed::ModalViewportTracker) has already decided
   * whether the event belongs here, and it does so BEFORE the editing core is reached because it
   * also repoints `CTX_wm_area`/`CTX_wm_region` at the viewport under the cursor. A second, weaker
   * test here could only disagree with it.
   */
  bool event_in_region(bContext & /*C*/, const wmEvent & /*event*/) const override
  {
    return true;
  }

  void drag_point_apply(bContext &C,
                        bke::CurvesGeometry &curve,
                        const int point_index,
                        const float3 initial[3],
                        const float2 &mval_start,
                        const float2 &mval_now) const override
  {
    const ViewContext vc = this->view_context_get(C);
    float ob_to_world[4][4];
    float world_to_ob[4][4];
    copy_m4_m4(ob_to_world, ob_.object_to_world().ptr());
    copy_m4_m4(world_to_ob, ob_.world_to_object().ptr());

    /* Re-derive the pivot's current world position every move so the unprojection plane tracks
     * depth, but apply the delta to the ORIGINAL handle positions so drift does not accumulate. */
    const float3 &pivot = curve.positions()[point_index];
    float pivot_world[3];
    mul_v3_m4v3(pivot_world, ob_to_world, pivot);

    const float mval_init[2] = {mval_start.x, mval_start.y};
    const float mval_curr[2] = {mval_now.x, mval_now.y};

    /* Keep the point on the surface while it is dragged, same as #paintcurve_point_slide_modal.
     * `use_depth_fallback` is off: the depth-buffer level needs a viewport redraw, which would run
     * on every mouse move. On a miss this falls through to the screen-delta path below. */
    float hit_obj[3];
    float hit_no_obj[3];
    if (snap_ctx_ == nullptr) {
      snap_ctx_ = ED_paintcurve_snap_context_create();
    }
    if (paintcurve_surface_place(&C,
                                 snap_ctx_,
                                 vc,
                                 mval_curr,
                                 pivot_world,
                                 /*use_depth_fallback=*/false,
                                 hit_obj,
                                 hit_no_obj))
    {
      paintcurve_apply_point_surface_snap(
          curve, point_index, initial, float3(hit_obj), float3(hit_no_obj));
      paintcurve_snap_marker_update(&C, ob_to_world, hit_obj);
    }
    else {
      paintcurve_snap_marker_clear();
      float obj_delta[3];
      paintcurve_object_delta_from_screen_drag(
          &vc, world_to_ob, pivot_world, mval_init, mval_curr, obj_delta);
      paintcurve_apply_point_translate_3d(curve, point_index, initial, float3(obj_delta));
      curve.calculate_bezier_auto_handles();
      curve.calculate_bezier_aligned_handles();
    }
    curve.tag_positions_changed();
  }

  void drag_handle_apply(bContext &C,
                         bke::CurvesGeometry &curve,
                         const int point_index,
                         const bool handle_is_left,
                         const float2 &mval) const override
  {
    const ViewContext vc = this->view_context_get(C);
    float ob_to_world[4][4];
    float world_to_ob[4][4];
    copy_m4_m4(ob_to_world, ob_.object_to_world().ptr());
    copy_m4_m4(world_to_ob, ob_.world_to_object().ptr());

    /* Project the cursor onto the pivot plane (#paintcurve_screen_to_object). Not
     * #paintcurve_apply_handle_move_3d -- that path also rotates the opposite handle and rewrites
     * Free/Align types, while the editing core has already settled the types. */
    const float3 &pivot = curve.positions()[point_index];
    float pivot_world[3];
    mul_v3_m4v3(pivot_world, ob_to_world, pivot);

    const float mval_curr[2] = {mval.x, mval.y};
    paintcurve_screen_to_object(&vc,
                                pivot_world,
                                world_to_ob,
                                mval_curr,
                                paintcurve_geom_co(curve, point_index, handle_is_left ? 0 : 2));

    /* Re-aligns the OTHER handle's direction while preserving its length, so the curve stays
     * tangent-continuous through an Align point. */
    curve.calculate_bezier_auto_handles();
    curve.calculate_bezier_aligned_handles();
    curve.tag_positions_changed();
  }

  void drag_segment_apply(bContext &C,
                          bke::CurvesGeometry &curve,
                          const int point_a,
                          const int point_b,
                          const float segment_t,
                          const float2 &mval) const override
  {
    const ViewContext vc = this->view_context_get(C);
    float ob_to_world[4][4];
    float world_to_ob[4][4];
    copy_m4_m4(ob_to_world, ob_.object_to_world().ptr());
    copy_m4_m4(world_to_ob, ob_.world_to_object().ptr());

    /* Matches #paintcurve_apply_segment_move_3d's own modal caller (paint_curve.cc): the
     * unprojection plane is anchored at the segment's first endpoint, not re-derived per move from
     * anything else -- the segment's own two endpoints do not move, only their shared handles
     * do. */
    float depth_world[3];
    mul_v3_m4v3(depth_world, ob_to_world, curve.positions()[point_a]);

    const float mval_curr[2] = {mval.x, mval.y};
    paintcurve_apply_segment_move_3d(
        curve, point_a, point_b, segment_t, &vc, world_to_ob, mval_curr, depth_world);
    curve.tag_positions_changed();
  }

  float3 space_delta_from_screen(bContext &C,
                                 const float3 &reference,
                                 const float2 &mval_start,
                                 const float2 &mval_now) const override
  {
    const ViewContext vc = this->view_context_get(C);
    float world_to_ob[4][4];
    copy_m4_m4(world_to_ob, ob_.world_to_object().ptr());

    /* The same unprojection plane the point drag falls back on when surface snap misses: anchored
     * at the reference point's depth, so a screen delta means a comparable object-space one. */
    float reference_world[3];
    mul_v3_m4v3(reference_world, ob_.object_to_world().ptr(), reference);

    const float start[2] = {mval_start.x, mval_start.y};
    const float now[2] = {mval_now.x, mval_now.y};
    float obj_delta[3];
    paintcurve_object_delta_from_screen_drag(
        &vc, world_to_ob, reference_world, start, now, obj_delta);
    return float3(obj_delta);
  }

  bool project_to_screen(bContext &C, const float3 &co, float2 &r_mval) const override
  {
    const ViewContext vc = this->view_context_get(C);
    if (vc.region == nullptr || vc.rv3d == nullptr) {
      return false;
    }
    float screen[2];
    paintcurve_object_to_screen(&vc, ob_.object_to_world().ptr(), co, screen);
    if (!isfinite(screen[0]) || !isfinite(screen[1])) {
      return false;
    }
    r_mval = float2(screen[0], screen[1]);
    return true;
  }

  /** World X/Y/Z carried into the object's space, matching what the Transform hand-off constrained
   * to: it built its axes from `t->spacemtx`, which for this converter is the world basis. */
  void constraint_axes_get(bContext &C, float3 r_axes[3], float3 &r_view_axis) const override
  {
    const float4x4 world_to_ob = ob_.world_to_object();
    for (const int axis : IndexRange(3)) {
      float3 world_dir(0.0f);
      world_dir[axis] = 1.0f;
      r_axes[axis] = math::normalize(math::transform_direction(world_to_ob, world_dir));
    }
    /* The view direction, so an unconstrained rotation turns in the plane of the screen. */
    const ViewContext vc = this->view_context_get(C);
    float3 view_world(0.0f, 0.0f, 1.0f);
    if (vc.rv3d != nullptr) {
      view_world = float3(vc.rv3d->viewinv[2]);
    }
    r_view_axis = math::normalize(math::transform_direction(world_to_ob, view_world));
  }

 private:
  ViewContext view_context_get(bContext &C) const
  {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
    return ED_view3d_viewcontext_init(&C, depsgraph);
  }
};

static bool curve_patch_active_point_is_valid(const CurvePatchSession &patch)
{
  const bke::CurvesGeometry &geom = patch.active_item().control_curve;
  return patch.doc.active_point >= 0 && paintcurve_geometry_is_valid(geom) &&
         patch.doc.active_point < geom.points_num();
}

/* The five actions below are thin per-mode wrappers: they resolve this mode's host and call the
 * shared body in `paint_curve_patch_actions.hh`, which both the 3D and the flat-canvas editor run.
 * The wrappers exist so the modal's own call sites keep their current signatures. */

/* Returns false (having reported why) when the point cannot be removed. */
static bool curve_patch_delete_active_point(bContext &C,
                                            Object &ob,
                                            CurvePatchSession &patch,
                                            ReportList *reports)
{
  SculptCurvePatchHost host(ob, patch, reports);
  return curve_patch_action_delete_point(C, host, patch.doc.active_point);
}

static void curve_patch_set_active_handle_type(bContext &C,
                                               Object &ob,
                                               CurvePatchSession &patch,
                                               const ed::curves::SetHandleType dst_type)
{
  SculptCurvePatchHost host(ob, patch, nullptr);
  curve_patch_action_set_handle_type(C, host, patch.doc.active_point, dst_type);
}

static bool curve_patch_toggle_cyclic(bContext &C, Object &ob, CurvePatchSession &patch)
{
  SculptCurvePatchHost host(ob, patch, nullptr);
  return curve_patch_action_toggle_cyclic(C, host);
}

static bool curve_patch_reseed_stamps(bContext &C, Object &ob, CurvePatchSession &patch)
{
  SculptCurvePatchHost host(ob, patch, nullptr);
  return curve_patch_action_reseed_stamps(C, host);
}

static bool curve_patch_switch_direction(bContext &C, Object &ob, CurvePatchSession &patch)
{
  SculptCurvePatchHost host(ob, patch, nullptr);
  return curve_patch_action_switch_direction(C, host);
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
  layout.op("SCULPT_OT_curve_patch_switch_direction", IFACE_("Switch Direction"), ICON_NONE);

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
     * offer it. The brush panel still shows it: it has no access to the live patch, and the
     * setting remains meaningful for every open one. */
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
      PropertyRNA *start_point_shape_prop = RNA_struct_find_property(&settings_ptr,
                                                                     "start_point_shape");
      layout.prop_menu_enum(
          &settings_ptr, start_point_shape_prop, IFACE_("Falloff Start Point"), ICON_NONE);
      PropertyRNA *end_point_shape_prop = RNA_struct_find_property(&settings_ptr,
                                                                   "end_point_shape");
      layout.prop_menu_enum(
          &settings_ptr, end_point_shape_prop, IFACE_("Falloff End Point"), ICON_NONE);
    }
    layout.prop(
        &settings_ptr, "use_swap_axis", UI_ITEM_NONE, IFACE_("Swap Texture Axis"), ICON_NONE);
  }

  layout.separator();
  layout.op("SCULPT_OT_curve_patch_delete_point", std::nullopt, ICON_NONE);
  ui::popup_menu_end(C, pup);
}

/** \} */

/* Status-bar hint, refreshed on invoke and every axis toggle. Doubles as a visible signal that
 * an event actually reached this modal handler -- if the text never appears/updates, the
 * event isn't getting here at all (as opposed to it arriving but the texture effect being hard
 * to see for a given brush/texture setup). The keys are read from this operator's modal keymap,
 * so #WorkspaceStatus::opmodal can show whatever the user rebound them to. */
void curve_patch_edit_status_set(bContext *C, const CurvePatchSession &patch)
{
  WorkspaceStatus status(C);
  const wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_curve_patch_edit", true);
  if (ot && ot->modalkeymap) {
    status.opmodal(IFACE_("Commit"), ot, CURVE_PATCH_MODAL_CONFIRM);
    status.opmodal(IFACE_("Cancel"), ot, CURVE_PATCH_MODAL_CANCEL);
    status.opmodal(IFACE_("Undo"), ot, CURVE_PATCH_MODAL_UNDO);
    status.opmodal(patch.active_item().params.swap_axis ? IFACE_("Swap Texture Axis (now U)") :
                                                          IFACE_("Swap Texture Axis (now V)"),
                   ot,
                   CURVE_PATCH_MODAL_SWAP_AXIS);
    status.opmodal(curve_patch_is_cyclic(patch) ? IFACE_("Open Curve") : IFACE_("Close Curve"),
                   ot,
                   CURVE_PATCH_MODAL_TOGGLE_CYCLIC);
    /* The three keyboard transforms are handled by the shared editing core now, but they still
     * arrive through this modal keymap, so `opmodal()` still resolves their real bindings. */
    status.opmodal(IFACE_("Move"), ot, CURVE_PATCH_MODAL_TRANSLATE);
    status.opmodal(IFACE_("Rotate"), ot, CURVE_PATCH_MODAL_ROTATE);
    status.opmodal(IFACE_("Resize"), ot, CURVE_PATCH_MODAL_SCALE);
    status.opmodal(IFACE_("Radius"), ot, CURVE_PATCH_MODAL_RADIUS);
    status.opmodal(IFACE_("Delete Point"), ot, CURVE_PATCH_MODAL_DELETE);
  }
  else {
    status.item(IFACE_("Commit"), ICON_EVENT_RETURN);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
    status.item(IFACE_("Undo"), ICON_EVENT_CTRL, ICON_EVENT_Z);
    status.item(patch.active_item().params.swap_axis ? IFACE_("Swap Texture Axis (now U)") :
                                                       IFACE_("Swap Texture Axis (now V)"),
                ICON_EVENT_Y);
    status.item(curve_patch_is_cyclic(patch) ? IFACE_("Open Curve") : IFACE_("Close Curve"),
                ICON_EVENT_C);
  }
  /* Reseed has no shortcut -- this modal has no free key left -- so advertise the route that does
   * reach it. Meaningless in Ribbon mode, which has nothing random to re-roll. */
  if (patch.active_item().params.stamp_mode == CurvePatchStampMode::Stamps) {
    status.item(IFACE_("Reseed Stamps"), ICON_MOUSE_RMB);
  }
}

static wmOperatorStatus curve_patch_edit_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent * /*event*/)
{
  CurvePatchEditOpData *data = MEM_new<CurvePatchEditOpData>(__func__);
  op->customdata = data;
  /* Seed the swap-axis watchdog; the live-input watchdog is seeded on the document below, once
   * the session is resolved (see #CurvePatchLiveInputs). */
  ToolSettings *tool_settings = CTX_data_tool_settings(C);
  if (tool_settings && tool_settings->sculpt) {
    Sculpt &sd = *tool_settings->sculpt;
    /* Non-const so `brush_at_invoke` can be handed straight back to `Paint::brush` when the modal
     * commits after a tool switch (see `curve_patch_edit_modal()`). */
    if (Brush *brush = BKE_paint_brush(&sd.paint)) {
      data->brush_at_invoke = brush;
      data->last_synced_swap_axis = brush->curve_patch.swap_axis != 0;
    }
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
  /* Seed the live-input watchdog on the document so the first poll does not re-stamp against
   * defaults (see #CurvePatchLiveInputs). Poll already requires an active object; the checks keep
   * a missing context from dereferencing null before the liveness guard runs. */
  if (tool_settings && tool_settings->sculpt) {
    const Sculpt &sd = *tool_settings->sculpt;
    if (const Brush *brush = BKE_paint_brush_for_read(&sd.paint)) {
      if (const Object *ob = CTX_data_active_object(C)) {
        patch.doc.last_synced = curve_patch_live_inputs_capture_sculpt(sd.paint, *brush, *ob);
      }
    }
  }
  /* Seed the session undo stack with the state the anchor stroke produced. Ctrl+Z walks back to
   * this entry and, once there, cancels the patch instead of stepping further. */
  patch.doc.undo_steps.clear();
  patch.doc.undo_step_current = -1;
  curve_patch_undo_push(patch);
  curve_patch_edit_status_set(C, patch);
  return OPERATOR_RUNNING_MODAL;
}

/* Returns true when the patch was actually committed. A commit request can still end up writing
 * nothing -- see the `CurvePatchApplyState::invalidated` branch inside -- and the modal reports
 * that as `OPERATOR_CANCELLED`. */
static bool curve_patch_edit_finish(bContext *C, wmOperator *op, const bool is_cancel);

/* The operator-state half of #curve_patch_edit_finish, for the paths where the session has already
 * been ended through #CurvePatchEditorHost by the shared editing core. */
static void curve_patch_edit_teardown(bContext *C, wmOperator *op);

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
static bool curve_patch_edit_session_superseded(const bContext *C,
                                                const CurvePatchEditOpData &data)
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
     * `toolsystem_refresh_screen_from_active_tool()` is mid-rebuild, which dispatches no events.
     */
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
   * to the ordinary View3D keymap. A session teardown that never calls #curve_patch_edit_finish
   * (object free, a committed undo from another operator) would otherwise leave the next event
   * dereferencing a dangling `SculptSession`/`curve_patch_session` in #patch_cache_of.
   * `curve_patch_edit_poll()` only guards the initial invoke. */
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

  /* Live brush sync. Frozen per-patch fields stay on `item.params`; everything else is overlaid
   * from the live brush by #curve_patch_params_live_overlay, plus #CurvePatchLiveInputs for
   * strength/color/textures/falloff/symmetry which are not in params. Only re-stamp on an actual
   * change. Deliberately ABOVE the pass-through gate so a panel edit outside the viewport is
   * still caught. */
  const ToolSettings *tool_settings = CTX_data_tool_settings(C);
  if (tool_settings && tool_settings->sculpt) {
    const Sculpt &sd = *tool_settings->sculpt;
    if (const Brush *brush = BKE_paint_brush_for_read(&sd.paint)) {
      const CurvePatchLiveInputs live_inputs = curve_patch_live_inputs_capture_sculpt(
          sd.paint, *brush, ob);
      /* The one watchdog that arbitrates rather than merely triggers -- see
       * `CurvePatchEditOpData::last_synced_swap_axis`. Recorded unconditionally: it tracks the
       * BRUSH's value, and the Y hotkey may well have already made the session agree with the new
       * one, in which case the parameter compare below finds nothing to do and must not leave this
       * reporting a change forever. */
      const bool swap_axis = brush->curve_patch.swap_axis != 0;
      const bool brush_swap_axis_changed = swap_axis != data.last_synced_swap_axis;
      data.last_synced_swap_axis = swap_axis;

      const int brush_size = BKE_brush_size_get(&sd.paint, brush);
      CurvePatchItem &active_item = patch.active_item();
      const bke::CurvePatchParams live = curve_patch_params_live_overlay(
          *brush, active_item.params, brush_size, brush_swap_axis_changed);

      if (live != active_item.params || live_inputs != patch.doc.last_synced) {
        /* Brush-driven fields onto every patch; frozen fields stay per-item. */
        for (CurvePatchItem &item : patch.doc.patches) {
          item.params = curve_patch_params_live_overlay(
              *brush, item.params, brush_size, brush_swap_axis_changed);
        }
        const bool rebuild_tex_pool = live_inputs.needs_texture_pool_rebuild(
            patch.doc.last_synced);
        patch.doc.last_synced = live_inputs;
        /* The relief samples through the session's `ImagePool`. Rebuild only when the set of
         * sampled images or their mapping changed -- a cap-length or slot-weight edit re-stamps
         * but keeps the same images, so it must not free/reallocate the pool on every slider tick.
         * Only dropped here; the re-stamp below creates the pool through
         * `SculptSession::tex_pool_ensure()`. */
        if (rebuild_tex_pool) {
          ob.runtime->sculpt_session->tex_pool_invalidate();
        }
        curve_patch_edit_status_set(C, patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_viewports_redraw_after_edit(*C, ob, patch);
      }
    }
  }

  /* A foreign operator changed the mesh's element count (see `CurvePatchApplyState::element_num`).
   * The patch cannot be committed or even restored, so end the modal as soon as the flag is seen.
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
   * Modal-keymap actions (Enter/Esc, undo, G/R/S, ...) are exempt so they work with the cursor
   * off the viewport. An active drag is exempt too, so dragging a point over the N-panel or past
   * the viewport edge does not orphan the drag or eat the `LEFTMOUSE` release that ends it. */
  const bool is_dragging = data.editor.is_dragging();
  /* Keyboard actions arrive as #EVT_MODAL_MAP once the modal keymap is assigned. Undo/confirm/
   * cancel must reach this operator even with the cursor over a panel: global undo would pop a
   * previously committed step out from under `orig_positions`, and Enter/Esc are the only way to
   * leave the session from outside the viewport. Other modal-map items are exempt too -- once WM
   * has converted the original key, passing the map event through would deliver nothing useful to
   * the panel underneath. Plain Z is not in the map, so it still reaches Sculpt Mode's shading
   * pie. */
  const bool is_modal_map = event->type == EVT_MODAL_MAP;
  /* The raw chord must clear the gate too: with an empty modal map it is not #EVT_MODAL_MAP,
   * and off-viewport it would pass through to global undo before the `EVT_ZKEY` case can
   * swallow it. */
  const bool is_raw_undo_chord = event->type == EVT_ZKEY && event->val == KM_PRESS &&
                                 (event->modifier & (KM_CTRL | KM_OSKEY)) != 0;

  /* Constructed before the pass-through decision (not after, as the two locals it replaces used
   * to be computed): a "not found" construction never touches `CTX_wm_area()`/`CTX_wm_region()`,
   * so returning #OPERATOR_PASS_THROUGH immediately below is exactly as safe as it was when the
   * lookup was a plain read-only query -- the tracker's destructor restores the frozen context on
   * that path too, as a no-op. */
  ed::ModalViewportTracker tracker(*C, *event, SPACE_VIEW3D, RGN_TYPE_WINDOW);
  const bool over_viewport = tracker.found();
  const bool should_pass_through = !is_dragging && !over_viewport && !is_modal_map &&
                                   !is_raw_undo_chord;
  if (should_pass_through) {
    return OPERATOR_PASS_THROUGH;
  }

  /* Window-level modals freeze `CTX_wm_area()`/`CTX_wm_region()` at invoke time; the tracker above
   * already repointed them at the viewport actually under the cursor. Refresh the session's owned
   * `ViewContext` so hit-tests, screen projection and sculpt flushes all target the right region.
   * Drag over an N-panel, and modal-map keys with the cursor off the viewport (G/R/S, Alt+S),
   * fall back to the last synced region -- otherwise transform starts against the invoke-frozen
   * viewport after the user has been working in a split view. */
  if (!tracker.found() && (is_dragging || is_modal_map) && patch.view_context.region) {
    tracker.use_fallback_region(patch.view_context.region);
  }
  if (tracker.found()) {
    curve_patch_sync_view_context(C, tracker.area(), tracker.region(), patch);
  }
  const int event_mval[2] = {tracker.mval().x, tracker.mval().y};

  /* Built here, per event, rather than stored on the operator: the object and the session are
   * re-resolved from the context above every time, and the snap context -- the one piece of drag
   * state the adapter needs to keep -- lives on the operator data and is borrowed. */
  SculptCurvePatchHost host(ob, patch, op->reports);
  SculptScreenAdapter adapter(ob, patch, data.snap_ctx);

  /* A running G/R/S owns the modal until it confirms or aborts, so every event goes straight to
   * the core -- ahead of the dispatch below, where Esc would end the whole patch and X would
   * delete a point.
   *
   * The modal keymap is undone on the way in rather than translated item by item: this map binds
   * Esc/Enter (confirm and cancel, which the transform wants for itself) and Y (Swap Texture Axis,
   * which during a transform is the Y axis), so the core would otherwise never see them as keys at
   * all. #wm_event_modalkeymap_begin leaves the physical key that matched in
   * `prev_type`/`prev_val` precisely so a handler can recover it. */
  if (data.editor.is_xform_active()) {
    wmEvent event_local = *event;
    event_local.mval[0] = event_mval[0];
    event_local.mval[1] = event_mval[1];
    if (event_local.type == EVT_MODAL_MAP) {
      event_local.type = event_local.prev_type;
      event_local.val = event_local.prev_val;
    }
    data.editor.handle_event(*C, event_local, host, adapter);
    /* Always consumed: a transform is modal, exactly as the `OPTYPE_BLOCKING` hand-off was. The
     * core reports nothing but `Running` while one is in flight. */
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case CURVE_PATCH_MODAL_CONFIRM:
        return curve_patch_edit_finish(C, op, false) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      case CURVE_PATCH_MODAL_CANCEL:
        curve_patch_edit_finish(C, op, true);
        return OPERATOR_CANCELLED;
      case CURVE_PATCH_MODAL_UNDO:
        if (!curve_patch_undo_step_back(*C, ob, patch)) {
          curve_patch_edit_finish(C, op, true);
          return OPERATOR_CANCELLED;
        }
        break;
      case CURVE_PATCH_MODAL_REDO:
        curve_patch_undo_step_forward(*C, ob, patch);
        break;
      case CURVE_PATCH_MODAL_TOGGLE_CYCLIC:
        curve_patch_toggle_cyclic(*C, ob, patch);
        break;
      case CURVE_PATCH_MODAL_SWAP_AXIS: {
        bke::CurvePatchParams &params = patch.active_item().params;
        params.swap_axis = !params.swap_axis;
        curve_patch_undo_push(patch);
        curve_patch_edit_status_set(C, patch);
        curve_patch_restore_and_restamp(*C, ob, patch);
        curve_patch_tag_viewports_redraw_after_edit(*C, ob, patch);
        break;
      }
      /* Handled by the shared editing core rather than by `transform.translate` and friends. Those
       * carry `OPTYPE_UNDO` and finish on a later event, so every G/R/S press used to add a global
       * undo step and the finished patch could no longer be undone in one go -- see
       * #CurvePatchXform. Guard miss consumes the event: this is already #EVT_MODAL_MAP, so
       * PASS_THROUGH would not re-deliver G/R/S to the View3D keymap the way a raw key used to. */
      case CURVE_PATCH_MODAL_TRANSLATE:
      case CURVE_PATCH_MODAL_ROTATE:
      case CURVE_PATCH_MODAL_SCALE: {
        const CurvePatchXform mode = (event->val == CURVE_PATCH_MODAL_TRANSLATE) ?
                                         CurvePatchXform::Translate :
                                     (event->val == CURVE_PATCH_MODAL_ROTATE) ?
                                         CurvePatchXform::Rotate :
                                         CurvePatchXform::Scale;
        if (!is_dragging && curve_patch_active_point_is_valid(patch)) {
          /* The core reads `wmEvent::mval`, which this window-level modal leaves relative to the
           * region frozen at invoke -- the same correction the mouse branch below makes. */
          wmEvent event_local = *event;
          event_local.mval[0] = event_mval[0];
          event_local.mval[1] = event_mval[1];
          data.editor.xform_begin(*C, mode, event_local, host, adapter);
        }
        break;
      }
      case CURVE_PATCH_MODAL_RADIUS:
        if (!is_dragging && curve_patch_active_point_is_valid(patch)) {
          data.radius_drag_from_key = data.editor.radius_drag_begin(*C, host, adapter);
        }
        break;
      case CURVE_PATCH_MODAL_DELETE:
        if (curve_patch_delete_active_point(*C, ob, patch, op->reports)) {
          data.editor.drag_end();
          curve_patch_edit_snap_ctx_free(data);
        }
        break;
      default:
        break;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  /* Below this point the modal only ever ends via an explicit Enter (commit) or Esc (cancel) -- no
   * click and no mouse-leaving-the-viewport implicitly ends it. Events that don't match anything
   * below are simply consumed like any other event while the patch is live; the user must press
   * Enter/Esc first. A tool or brush switch is the one exception, and it is handled by the poll at
   * the top of this function rather than here: it arrives as a click on the toolbar, which the
   * pass-through gate above hands to the toolbar before any case here could see it. */

  switch (event->type) {
    /* Mouse editing is the shared editing core's job: the press hit-test across every patch, the
     * four drags, Ctrl+RMB insert-or-append and the right-click menu are the same wherever a patch
     * is stamped. What stays here is everything the core has no opinion on -- the modal keymap,
     * the Transform hand-off, surface snapping (which reaches the core through
     * #SculptScreenAdapter) and X delete, whose 3D hit-test mixes radius, point and wire hits. */
    case LEFTMOUSE:
    case MOUSEMOVE:
    case RIGHTMOUSE: {
      if (event->type == LEFTMOUSE && event->val == KM_PRESS && data.radius_drag_from_key) {
        /* A radius drag started by Alt+S (`CURVE_PATCH_MODAL_RADIUS`) has no mouse button held
         * down the way a click-started drag does, so there is no RELEASE to end it on -- the next
         * click confirms it instead, mirroring how G/R/S transform modals confirm on click. */
        data.radius_drag_from_key = false;
        data.editor.drag_end();
        curve_patch_edit_snap_ctx_free(data);
        host.after_curve_change(*C);
        break;
      }

      /* The core reads `wmEvent::mval`, which a window-level modal leaves relative to the region
       * frozen at invoke. `tracker.mval()` is the same position relative to the viewport actually
       * under the cursor, which is what every hit test here has always used. */
      wmEvent event_local = *event;
      event_local.mval[0] = event_mval[0];
      event_local.mval[1] = event_mval[1];

      const bool was_dragging = data.editor.is_dragging();
      const CurvePatchCurveEditor::Status status = data.editor.handle_event(
          *C, event_local, host, adapter);
      if (was_dragging && !data.editor.is_dragging()) {
        /* The drag ended inside the core, which knows nothing about snapping: drop the
         * drag-scoped snap context and its viewport marker. */
        curve_patch_edit_snap_ctx_free(data);
      }

      switch (status) {
        case CurvePatchCurveEditor::Status::Finished:
          curve_patch_edit_teardown(C, op);
          return OPERATOR_FINISHED;
        case CurvePatchCurveEditor::Status::Cancelled:
          curve_patch_edit_teardown(C, op);
          return OPERATOR_CANCELLED;
        case CurvePatchCurveEditor::Status::PassThrough:
          return OPERATOR_PASS_THROUGH;
        case CurvePatchCurveEditor::Status::Running:
        case CurvePatchCurveEditor::Status::Unhandled:
          break;
      }
      break;
    }
    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY:
      curve_patch_tag_overlay_redraw_all(C);
      break;
    case EVT_XKEY:
      if (event->val == KM_PRESS) {
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        const float loc_fl[2] = {float(event_mval[0]), float(event_mval[1])};

        /* Unlike the hit tests above, this stops at the first patch with any hit (radius, point,
         * or wire) rather than the globally closest one --
         * `patch_find_point_index_at_pos_for_delete` mixes those three hit kinds internally and
         * does not report a comparable distance. Given the small selection threshold this rarely
         * matters in practice for a delete action. */
        int best_patch = -1;
        int hit_point = -1;
        for (const int i : patch.doc.patches.index_range()) {
          bke::CurvesGeometry &geom = patch.doc.patches[i].control_curve;
          hit_point = patch_find_point_index_at_pos_for_delete(geom, &vc, loc_fl);
          if (hit_point >= 0) {
            best_patch = i;
            break;
          }
        }

        if (best_patch < 0) {
          /* Away from the control curve, X belongs to Sculpt/Texture Paint's primary/secondary
           * color swap (`PAINT_OT_brush_colors_flip`). During an active stencil RMB-drag it also
           * reaches `BRUSH_OT_stencil_control` for axis constraints. */
          return OPERATOR_PASS_THROUGH;
        }

        patch.doc.active_patch = best_patch;
        patch.doc.active_point = hit_point;
        if (curve_patch_delete_active_point(*C, ob, patch, op->reports)) {
          /* The removed point may have been the one under an in-flight drag; drop the drag state
           * so the next MOUSEMOVE does not index into geometry that no longer has it. */
          data.editor.drag_end();
          curve_patch_edit_snap_ctx_free(data);
        }
      }
      break;
    case EVT_ZKEY:
      /* Undo/redo live on the modal map. A raw Z with Ctrl/Cmd means the map did not convert
       * the event: unbound, or a key-config (Industry Compatible) that never listed this map
       * and so #WM_keymap_active falls back to the empty defaultconf map. Passing through
       * would reach global undo and pop a committed step out from under `orig_positions` --
       * already burned once when this modal let Ctrl+Z leak. Swallow the chord so the
       * invariant holds in C regardless of whether the map has items. Plain Z still reaches
       * Sculpt Mode's shading pie. Shift is included so Ctrl+Shift+Z cannot global-redo either. */
      if (event->val == KM_PRESS && (event->modifier & (KM_CTRL | KM_OSKEY))) {
        return OPERATOR_RUNNING_MODAL;
      }
      return OPERATOR_PASS_THROUGH;
    case EVT_RETKEY:
    case EVT_PADENTER:
    case EVT_ESCKEY:
    case EVT_CKEY:
    case EVT_YKEY:
    case EVT_GKEY:
    case EVT_RKEY:
    case EVT_SKEY:
    case EVT_DELKEY:
      /* Bound through #curve_patch_edit_modal_keymap; WM converts a match to #EVT_MODAL_MAP
       * before this function runs. Reaching here means the map did not claim the event (unbound,
       * or a modifier combination the map does not own -- plain Z is handled above, Ctrl+S,
       * Ctrl+C, Ctrl+Y). C/Y/G/R/S on the map are clean keys only; modified chords pass through
       * so they can reach the rest of the keymap. */
      return OPERATOR_PASS_THROUGH;
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
       * mid-cut). LEFTMOUSE, RIGHTMOUSE and MOUSEMOVE are still fully consumed even on a miss
       * (see the case bodies, all ending in a plain `break;`), so a stray click still cannot leak
       * through to start a brush stroke on the mesh. Keyboard actions go through the modal keymap;
       * an unbound leftover (plain Z, Ctrl+S) falls through here. */
      return OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_RUNNING_MODAL;
}

/**
 * End the session itself -- the half of #curve_patch_edit_finish that owns no operator state.
 *
 * Split out because the shared editor reaches commit and cancel through #CurvePatchEditorHost
 * while its own object lives in `op->customdata`: a session end that also freed the operator state
 * would destroy the editor from inside one of its own calls. The operator tears its state down
 * afterwards, once #CurvePatchCurveEditor::handle_event has returned.
 */
static bool curve_patch_edit_session_finish(bContext *C, const bool is_cancel)
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
       * it is painted -- and it is that effect's own destructor, reached through
       * `MEM_delete(patch)` below, that discards the step instead of committing it once this
       * restore has put the pixels back. See #ImageColorEffect's destructor. */
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
      curve_patch_set_final_quality(patch->doc, true);
      curve_patch_restore_and_restamp(*C, ob, *patch);
      curve_patch_set_final_quality(patch->doc, false);

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
         * initial preview stamp in `curve_patch_begin_editing()`. The helper above also issues the
         * effect-specific flush for non-position targets. */
        flush_update_done(C, ob, UpdateType::Position);
        committed = true;
      }
    }
    curve_patch_session_free(ob);
  }
  return committed;
}

/** Drop everything the modal operator owns. Safe to call twice. */
static void curve_patch_edit_teardown(bContext *C, wmOperator *op)
{
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
}

static bool curve_patch_edit_finish(bContext *C, wmOperator *op, const bool is_cancel)
{
  const bool committed = curve_patch_edit_session_finish(C, is_cancel);
  curve_patch_edit_teardown(C, op);
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
 * nothing else. The two point actions act on `CurvePatchDocument::active_point`, which the modal
 * sets from the right-click hit test just before opening the menu; the rest act on the whole
 * patch.
 *
 * None carries `OPTYPE_UNDO`: like the modal's own hotkeys they only mutate the live patch and its
 * session-local history (`CurvePatchDocument::undo_steps`), which is not Blender's undo stack.
 * The modal touches that stack at no point in its life -- the patch's single step is built when it
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
  /* Unlike the two point operators this one acts on the whole curve, so it needs no active point.
   */
  ot->poll = curve_patch_edit_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

static wmOperatorStatus curve_patch_switch_direction_exec(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);
  CurvePatchSession &patch = patch_cache_of(C);
  if (!curve_patch_switch_direction(*C, ob, patch)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_curve_patch_switch_direction(wmOperatorType *ot)
{
  ot->name = "Switch Curve Patch Direction";
  ot->description = "Reverse the active Curve Patch control curve's direction";
  ot->idname = "SCULPT_OT_curve_patch_switch_direction";

  ot->exec = curve_patch_switch_direction_exec;
  /* Acts on the whole curve rather than a point, like #SCULPT_OT_curve_patch_toggle_cyclic. */
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
