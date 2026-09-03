/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Modal editor for the 2D Curve Patch (`BRUSH_STROKE_CURVE_PATCH`) Image Paint session.
 *
 * The operator is registered by `PAINT_OT_image_curve_patch_edit`. It runs only when an
 * #ImageCurvePatchSession is active; it does not own session lifecycle itself --
 * #image_curve_patch_session_commit / #image_curve_patch_session_cancel are the lifecycle entry
 * points. Every mutation bottoms out in #image_curve_patch_session_restore_and_restamp so the
 * canvas always shows exactly the current curve.
 *
 * \note Coordinate contract. The session's canonical curve lives in image UV so it survives pan
 * and zoom untouched. Hit testing, in contrast, is inherently screen-space, and every reusable
 * paint-curve picker (#paintcurve_find_in_screen_points,
 * #paintcurve_find_radius_handle_at_pos_from_geometry, #paintcurve_find_insert_segment_from_
 * geometry) expects a geometry whose positions ARE region pixels. So each event that needs to
 * pick something projects a throwaway copy of the canonical curve into region pixels
 * (#image_projected_geometry_get) and hit-tests that, then applies the resulting index / parameter
 * to the canonical UV geometry. Index- and parameter-based results transfer unchanged because
 * UV -> region is affine; positions never do, and are always re-derived from the event.
 *
 * Feature parity with the 3D Sculpt Mode Curve Patch editor (`paint_curve_patch_edit.cc`) covers
 * everything that has a meaning on a flat canvas: point, handle, segment and radius drags,
 * Ctrl+RMB insert-or-append, X / Del delete, C cyclic toggle, Y direction switch, V handle-type
 * cycle, the RMB context menu, Enter commit, Esc cancel. Surface snapping, per-point surface
 * normals and scene-curve silhouettes are 3D-only and deliberately absent.
 */

#include <algorithm>
#include <optional>

#include "BLI_index_mask.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"

#include "BLT_translation.hh"

#include "ED_curves.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_paint_curve_draw.hh"
#include "ED_screen.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "paint_curve_intern.hh"
#include "paint_curve_patch_actions.hh"
#include "paint_curve_patch_editor.hh"
#include "paint_curve_patch_host.hh"
#include "paint_image_curve_patch.hh"
#include "paint_image_curve_patch_edit.hh"
#include "paint_intern.hh"

namespace blender {

namespace ed::sculpt_paint::image::curve_patch::edit {

/* -------------------------------------------------------------------- */
/** \name Screen-space adapters
 * \{ */
/**
 * Project the session's canonical curve into region pixels. The shared pickers all want a geometry
 * whose positions ARE region pixels; the editing core builds the #PaintCurvePoint array from what
 * this returns. False when there is nothing pickable.
 */
static bool image_projected_geometry_get(bContext *C, bke::CurvesGeometry &r_geom)
{
  const ARegion *region = CTX_wm_region(C);
  if (!ED_image_curve_patch_overlay_geometry_get(region, r_geom)) {
    return false;
  }
  return paintcurve_geometry_is_valid(r_geom);
}

/** Region-local pixel position as canonical UV.
 *
 * Goes through #ED_image_mouse_pos rather than a plain #ui::view2d_region_to_view: the View2D's
 * own `cur`/`tot` stay axis-aligned even when the canvas is rotated (the rotation is an extra
 * screen-space transform layered on top for drawing), so a raw region-to-view conversion ignores
 * it and every point lands off the cursor once #SpaceImage::rotation is non-zero. */
static float2 mval_uv_get(bContext *C, const int mval[2])
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  const ARegion *region = CTX_wm_region(C);
  if (sima == nullptr || region == nullptr) {
    return float2(0.0f);
  }
  float2 uv(0.0f);
  ED_image_mouse_pos(const_cast<SpaceImage *>(sima), region, mval, uv);
  return uv;
}

/**
 * The canvas editor's status bar. Issued at invoke and re-issued whenever something transient --
 * a running G/R/S -- has replaced it, so the modal never leaves the bar showing a state that has
 * ended.
 *
 * Built with #WorkspaceStatus rather than #ED_workspace_status_text so the keys are drawn as key
 * icons, matching #curve_patch_edit_status_set in 3D Sculpt Mode. No `opmodal()` entries here: the
 * canvas has no modal keymap of its own (the keys are handled directly in the shared editing
 * core), so every icon is named explicitly.
 */
static void image_curve_patch_status_text_set(bContext *C, const ImageCurvePatchSession &session)
{
  const bke::CurvesGeometry &curve = session.curve;
  const bool is_cyclic = curve.curves_num() > 0 && curve.cyclic()[0];

  WorkspaceStatus status(C);
  status.item(IFACE_("Confirm"), ICON_EVENT_RETURN);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
  status.item(IFACE_("Move Point/Handle/Segment"), ICON_MOUSE_LMB_DRAG);
  status.item(IFACE_("Insert or Add Point"), ICON_EVENT_CTRL, ICON_MOUSE_RMB);
  status.item(IFACE_("Menu"), ICON_MOUSE_RMB);
  status.item(IFACE_("Delete Point"), ICON_EVENT_X);
  status.item(IFACE_("Move"), ICON_EVENT_G);
  status.item(IFACE_("Rotate"), ICON_EVENT_R);
  status.item(IFACE_("Resize"), ICON_EVENT_S);
  status.item(is_cyclic ? IFACE_("Open Curve") : IFACE_("Close Curve"), ICON_EVENT_C);
  status.item(IFACE_("Flip Direction"), ICON_EVENT_Y);
  status.item(IFACE_("Handle Type"), ICON_EVENT_V);
}

/** Mark the session's only point-level selection, which is what the overlay reads to color the
 * active handle. `sel_bits` uses the packed convention of #paintcurve_geom_set_selection. */
static void session_select_only(ImageCurvePatchSession &session,
                                const int point_index,
                                const uint8_t sel_bits)
{
  paintcurve_geom_set_all_selection(session.curve, 0);
  if (point_index >= 0) {
    paintcurve_geom_set_selection(session.curve, point_index, sel_bits);
  }
  session.doc.active_point = point_index;
}

/** Re-derive computed handles and push the result to the canvas. Every mutation ends here. */
static void session_apply_curve_change(bContext *C, ImageCurvePatchSession &session)
{
  session.curve.calculate_bezier_auto_handles();
  session.curve.calculate_bezier_aligned_handles();
  session.curve.tag_positions_changed();

  image_curve_patch_session_restore_and_restamp(C, &session);
  ED_region_tag_redraw(CTX_wm_region(C));
  ED_paint_curve_overlay_tag_redraw_all(C);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Target and screen adapters
 * \{ */

/* Defined with the context-menu operators below; the host opens the menu on plain RMB. */
static void image_curve_patch_context_menu_open(bContext *C,
                                                const ImageCurvePatchSession &session);

/**
 * The Image Editor's target, both for the shared action bodies (`paint_curve_patch_actions.hh`)
 * and for the shared modal editor (`paint_curve_patch_editor.hh`).
 *
 * Owns nothing. The session it points at is the module-level singleton, which outlives every
 * action; once #commit or #cancel has run, the reference is dead and the editor returns to its
 * operator immediately -- nothing here may be touched afterwards.
 */
class ImageCurvePatchHost : public CurvePatchEditorHost {
  ImageCurvePatchSession &session_;
  ReportList *reports_;

 public:
  ImageCurvePatchHost(ImageCurvePatchSession &session, ReportList *reports)
      : session_(session), reports_(reports)
  {
  }

  CurvePatchDocument &document() override
  {
    return session_.doc;
  }

  /** The canonical UV curve, NOT `doc.active_item().control_curve` -- that one is the pixel-space
   * copy the rasterizer builds from this one (see #ImageCurvePatchSession::doc). */
  bke::CurvesGeometry &curve() override
  {
    return session_.curve;
  }

  void restamp(bContext &C) override
  {
    session_apply_curve_change(&C, session_);
  }

  void redraw(bContext &C) override
  {
    ED_region_tag_redraw(CTX_wm_region(&C));
    ED_paint_curve_overlay_tag_redraw_all(&C);
  }

  /** The overlay colors the picked handle from the curve's own selection bits, so the active point
   * has to be mirrored into them. 3D reads the index alone and needs no such sync. */
  void active_point_set(const int point_index, const uint8_t sel_bits) override
  {
    session_select_only(session_, point_index, sel_bits);
  }

  void commit(bContext &C) override
  {
    /* One undo entry for the whole live session. */
    image_curve_patch_session_commit(&C, &session_);
  }

  void cancel(bContext &C) override
  {
    /* Tiles restored to the pre-anchor state, no undo entry. */
    image_curve_patch_session_cancel(&C, &session_);
  }

  bool sync_live_brush(bContext &C) override
  {
    if (!image_curve_patch_session_sync_live_brush(&C, &session_)) {
      return false;
    }
    this->redraw(C);
    return true;
  }

  /** Compares pointers rather than dereferencing: the session ends through #commit or #cancel,
   * which delete it, and the editing core re-checks this right afterwards. */
  bool is_alive(bContext & /*C*/) override
  {
    return image_curve_patch_session_active_get() == &session_;
  }

  void context_menu_open(bContext &C) override
  {
    image_curve_patch_context_menu_open(&C, session_);
  }

  /** Put the canvas hint line back after a transient one (a finished G/R/S) replaced it. */
  void status_refresh(bContext &C) override
  {
    image_curve_patch_status_text_set(&C, session_);
  }

  ReportList *reports() override
  {
    return reports_;
  }
};

/** The flat canvas the patch is edited on: everything projects through the region's #View2D. */
class ImageScreenAdapter : public CurvePatchScreenAdapter {
 public:
  /**
   * The pickers want region pixels, so the canvas hands them a projected copy.
   *
   * `source` is not read: the projection helper is the overlay's own, which projects the session's
   * canonical curve -- and that curve is the only pickable one here (the canvas has a single
   * patch), so the two are the same geometry by construction.
   */
  bool pick_space_get(bContext &C,
                      const bke::CurvesGeometry & /*source*/,
                      bke::CurvesGeometry &r_projected_storage,
                      CurvePatchPickSpace &r_space) const override
  {
    if (!image_projected_geometry_get(&C, r_projected_storage)) {
      return false;
    }
    r_space.geom = &r_projected_storage;
    r_space.use_3d_space = false;
    r_space.vc = nullptr;
    return true;
  }

  /** There is no surface to place against on a flat canvas, so the point simply lands on the
   * cursor and the normal stays +Z. */
  void place_new_point(bContext &C,
                       const int mval[2],
                       float3 &r_position,
                       float3 &r_normal) const override
  {
    const float2 uv = mval_uv_get(&C, mval);
    r_position = float3(uv.x, uv.y, 0.0f);
    r_normal = float3(0.0f, 0.0f, 1.0f);
  }

  /** UV is affine in region pixels, so a whole-point drag is the cursor delta, applied to the
   * positions captured at drag start. Handles are re-derived by the session's own restamp. */
  void drag_point_apply(bContext &C,
                        bke::CurvesGeometry &curve,
                        const int point_index,
                        const float3 initial[3],
                        const float2 &mval_start,
                        const float2 &mval_now) const override
  {
    const int start[2] = {int(mval_start.x), int(mval_start.y)};
    const int now[2] = {int(mval_now.x), int(mval_now.y)};
    const float2 delta = mval_uv_get(&C, now) - mval_uv_get(&C, start);
    for (int h = 0; h < 3; h++) {
      paintcurve_geom_co(curve, point_index, h) = initial[h] + float3(delta.x, delta.y, 0.0f);
    }
  }

  void drag_handle_apply(bContext &C,
                         bke::CurvesGeometry &curve,
                         const int point_index,
                         const bool handle_is_left,
                         const float2 &mval) const override
  {
    const int mval_i[2] = {int(mval.x), int(mval.y)};
    const float2 uv = mval_uv_get(&C, mval_i);
    paintcurve_geom_co(curve, point_index, handle_is_left ? 0 : 2) = float3(uv.x, uv.y, 0.0f);
  }

  void drag_segment_apply(bContext &C,
                          bke::CurvesGeometry &curve,
                          const int point_a,
                          const int point_b,
                          const float segment_t,
                          const float2 &mval) const override
  {
    const int mval_i[2] = {int(mval.x), int(mval.y)};
    const float2 uv = mval_uv_get(&C, mval_i);
    paintcurve_apply_segment_move_to_point(
        curve, point_a, point_b, segment_t, float3(uv.x, uv.y, 0.0f));
  }

  /**
   * `ED_area_find_region_xy_visual` rather than a plain `winrct` test: with region overlap the
   * canvas region extends UNDERNEATH the sidebar, so `winrct` reports "inside the canvas" for
   * every click on the panel -- which swallowed them and froze the whole UI for the session. Only
   * the topmost visible region at the cursor counts.
   */
  bool event_in_region(bContext &C, const wmEvent &event) const override
  {
    const ARegion *canvas_region = CTX_wm_region(&C);
    const ScrArea *area = CTX_wm_area(&C);
    return canvas_region != nullptr && area != nullptr &&
           ED_area_find_region_xy_visual(area, RGN_TYPE_ANY, event.xy) == canvas_region;
  }

  /** `reference` carries no depth on a flat canvas, so it is ignored: UV is affine in region
   * pixels and a cursor delta maps to a UV delta wherever the point happens to be. */
  float3 space_delta_from_screen(bContext &C,
                                 const float3 & /*reference*/,
                                 const float2 &mval_start,
                                 const float2 &mval_now) const override
  {
    const int start[2] = {int(mval_start.x), int(mval_start.y)};
    const int now[2] = {int(mval_now.x), int(mval_now.y)};
    const float2 delta = mval_uv_get(&C, now) - mval_uv_get(&C, start);
    return float3(delta.x, delta.y, 0.0f);
  }

  bool project_to_screen(bContext &C, const float3 &co, float2 &r_mval) const override
  {
    const SpaceImage *sima = CTX_wm_space_image(&C);
    const ARegion *region = CTX_wm_region(&C);
    if (sima == nullptr || region == nullptr) {
      return false;
    }
    /* #ED_image_point_pos__reverse rather than a plain #ui::view2d_view_to_region_fl: it applies
     * the canvas rotation (view->screen) that the axis-aligned View2D `cur`/`tot` does not know
     * about, matching the inverse used by #mval_uv_get above. */
    const float uv[2] = {co.x, co.y};
    float2 mval(0.0f);
    ED_image_point_pos__reverse(const_cast<SpaceImage *>(sima), region, uv, mval);
    r_mval = mval;
    return true;
  }

  /** The canvas IS the UV plane, so its own axes are the constraint axes and the "view" direction
   * is simply the normal every point of a flat curve shares. */
  void constraint_axes_get(bContext & /*C*/, float3 r_axes[3], float3 &r_view_axis) const override
  {
    r_axes[0] = float3(1.0f, 0.0f, 0.0f);
    r_axes[1] = float3(0.0f, 1.0f, 0.0f);
    r_axes[2] = float3(0.0f, 0.0f, 1.0f);
    r_view_axis = float3(0.0f, 0.0f, 1.0f);
  }
};

/* \} */

/* -------------------------------------------------------------------- */
/** \name Modal Operator
 * \{ */

static bool curve_patch_edit_poll(bContext *C)
{
  /* `image_paint_poll_ignore_tool` would let any image-paint context through; we narrow further
   * so this modal only runs when an #ImageCurvePatchSession is actually live -- otherwise the
   * user cannot be editing a curve, and a stray modal would freeze the area until Esc. */
  if (!image_curve_patch_session_active()) {
    return false;
  }
  if (CTX_wm_space_image(C) == nullptr) {
    return false;
  }
  return ED_image_tools_paint_poll(C);
}

/**
 * Everything this modal owns: the target, the space it is edited in, and the shared editing core
 * that drives the two. Lives in `op->customdata` for the modal's whole life, because the core
 * carries the drag state between events.
 */
struct ImageCurvePatchModal {
  ImageCurvePatchHost host;
  ImageScreenAdapter adapter;
  CurvePatchCurveEditor editor;

  /**
   * Drives the live-brush poll on its own cadence.
   *
   * #CurvePatchEditorHost::sync_live_brush only runs when an event reaches this modal, and a brush
   * edit made in a panel sends none: RNA broadcasts `NC_BRUSH | NA_EDITED` and nothing is pushed
   * at the session. Without a timer the canvas therefore kept the old colour until the user
   * happened to move the mouse. 3D Sculpt Mode has had exactly this timer since the beginning
   * (`CurvePatchEditOpData::sync_timer`), which is why it updated instantly and the canvas did
   * not.
   */
  wmTimer *sync_timer = nullptr;

  ImageCurvePatchModal(ImageCurvePatchSession &session, ReportList *reports)
      : host(session, reports)
  {
  }
};

static wmOperatorStatus curve_patch_edit_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent * /*event*/)
{
  ImageCurvePatchSession *session = image_curve_patch_session_active_get();
  if (session == nullptr) {
    return OPERATOR_CANCELLED;
  }

  op->customdata = MEM_new<ImageCurvePatchModal>(__func__, *session, op->reports);

  /* Tell the anchor stroke its session found an owner; without this it rolls the session back on
   * the assumption that `poll()` rejected the takeover. */
  session->modal_active = true;

  /* The anchor's own dabs were rolled back before the session opened its transaction, so the
   * canvas currently shows no patch at all. Stamp the curve once here so the user sees the result
   * of their gesture immediately instead of a blank canvas until the first edit. */
  image_curve_patch_session_restore_and_restamp(C, session);
  ED_region_tag_redraw(CTX_wm_region(C));
  ED_paint_curve_overlay_tag_redraw_all(C);

  image_curve_patch_status_text_set(C, *session);

  WM_event_add_modal_handler(C, op);
  /* Same cadence 3D Sculpt Mode uses (see #ImageCurvePatchModal::sync_timer): 20 Hz is immediate
   * to the eye, and an idle tick only compares scalars. */
  const double sync_timer_step = 0.05;
  static_cast<ImageCurvePatchModal *>(op->customdata)->sync_timer = WM_event_timer_add(
      CTX_wm_manager(C), CTX_wm_window(C), TIMER, sync_timer_step);
  return OPERATOR_RUNNING_MODAL;
}

static void curve_patch_edit_teardown(bContext *C, wmOperator *op)
{
  ED_workspace_status_text(C, nullptr);
  if (op->customdata != nullptr) {
    ImageCurvePatchModal *modal = static_cast<ImageCurvePatchModal *>(op->customdata);
    if (modal->sync_timer != nullptr) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), modal->sync_timer);
    }
    op->customdata = nullptr;
    MEM_delete(modal);
  }
  ED_paint_curve_overlay_tag_redraw_all(C);
}


static wmOperatorStatus curve_patch_edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ImageCurvePatchSession *session = image_curve_patch_session_active_get();
  if (session == nullptr) {
    /* Session disappeared under us (cancelled from the other end, or the context went invalid).
     * Drop our modal silently. */
    curve_patch_edit_teardown(C, op);
    return OPERATOR_CANCELLED;
  }

  /* Validate the editing context on EVERY event, not just in `poll()`. A running modal handler
   * never re-polls, so without this the session survives the user leaving Texture Paint, toggling
   * Sculpt Mode, or closing the image -- and the next drag then restores tiles through an undo
   * step that those mode toggles already pushed and freed, which crashes.
   *
   * Stays in the operator rather than moving into the shared editor: WHAT to do about an invalid
   * context is target-specific. Here the patch is kept -- the user built it, and the editing
   * context going away is not a request to throw it out. */
  const SpaceImage *sima = CTX_wm_space_image(C);
  /* Which image the editor DISPLAYS stops identifying what the patch writes once Mode=`Material`
   * is in play: the patch then writes every enabled Principled map, and the displayed one is just
   * whichever channel the user is looking at. Switching that view -- or having it switched for
   * them -- must not end the session.
   *
   * `session->image` stays pinned to the image the session started on regardless, because it is
   * what `resolve_reference_tile()` builds the patch's pixel-space geometry against; re-pinning it
   * mid-session would silently rescale a patch the user has already shaped. */
  const Scene *scene = CTX_data_scene(C);
  const bool material_canvas = scene != nullptr &&
                               scene->toolsettings->paint_mode.canvas_source ==
                                   PAINT_CANVAS_SOURCE_MATERIAL;
  const bool canvas_valid = sima != nullptr && (material_canvas || sima->image == session->image);
  const bool context_valid = sima != nullptr && sima->mode == SI_MODE_PAINT && canvas_valid &&
                             CTX_wm_region(C) != nullptr;
  if (!context_valid) {
    image_curve_patch_session_commit(C, session);
    curve_patch_edit_teardown(C, op);
    return OPERATOR_FINISHED;
  }

  ImageCurvePatchModal *modal = static_cast<ImageCurvePatchModal *>(op->customdata);
  if (modal == nullptr) {
    /* Defensive: invoke should have set it. */
    modal = MEM_new<ImageCurvePatchModal>(__func__, *session, op->reports);
    op->customdata = modal;
  }

  /* The live-brush tick. Handled here rather than let through to the editing core: the core polls
   * the brush at the top of every `handle_event`, but it would then fall through to its `default:`
   * and report the timer as an event for someone else. Nothing else wants it. */
  if (event->type == TIMER && modal->sync_timer != nullptr &&
      event->customdata == modal->sync_timer)
  {
    const bool synced = modal->host.sync_live_brush(*C);
    if (synced && !modal->host.is_alive(*C)) {
      /* The re-stamp ended the session under us -- the same guard the core applies to its own
       * poll. */
      curve_patch_edit_teardown(C, op);
      return OPERATOR_CANCELLED;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  switch (modal->editor.handle_event(*C, *event, modal->host, modal->adapter)) {
    case CurvePatchCurveEditor::Status::Running:
      return OPERATOR_RUNNING_MODAL;
    case CurvePatchCurveEditor::Status::Finished:
      curve_patch_edit_teardown(C, op);
      return OPERATOR_FINISHED;
    case CurvePatchCurveEditor::Status::Cancelled:
      curve_patch_edit_teardown(C, op);
      return OPERATOR_CANCELLED;
    case CurvePatchCurveEditor::Status::PassThrough:
    case CurvePatchCurveEditor::Status::Unhandled:
      /* The flat canvas has no editing of its own beyond what the core does, so an event the core
       * has no opinion on -- pan, zoom, tool switches -- simply reaches the Image Editor. */
      return OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void curve_patch_edit_cancel(bContext *C, wmOperator *op)
{
  /* Modal cancel from the window manager: same path as Esc. */
  if (ImageCurvePatchSession *session = image_curve_patch_session_active_get()) {
    image_curve_patch_session_cancel(C, session);
  }
  curve_patch_edit_teardown(C, op);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Context Menu Operators
 *
 * A popup can only invoke operators -- it cannot call back into the running modal -- so each
 * menu entry needs a real operator. The 3D editor's `SCULPT_OT_curve_patch_*` set cannot be
 * reused: every one of them resolves its data through an #Object and #CurvePatchSession. These
 * resolve through the 2D session singleton instead, and are otherwise the same actions the modal
 * exposes on the keyboard.
 * \{ */

static bool image_curve_patch_op_poll(bContext *C)
{
  return curve_patch_edit_poll(C);
}

static bool image_curve_patch_active_point_poll(bContext *C)
{
  if (!curve_patch_edit_poll(C)) {
    return false;
  }
  const ImageCurvePatchSession *session = image_curve_patch_session_active_get();
  return session != nullptr && session->doc.active_point >= 0 &&
         session->doc.active_point < session->curve.points_num();
}

static wmOperatorStatus image_curve_patch_handle_type_set_exec(bContext *C, wmOperator *op)
{
  ImageCurvePatchSession &session = *image_curve_patch_session_active_get();
  const ed::curves::SetHandleType dst_type = ed::curves::SetHandleType(
      RNA_enum_get(op->ptr, "type"));

  ImageCurvePatchHost host(session, op->reports);
  curve_patch_action_set_handle_type(*C, host, session.doc.active_point, dst_type);
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_curve_patch_handle_type_set(wmOperatorType *ot)
{
  ot->name = "Set Curve Patch Handle Type";
  ot->description = "Set the handle type of the active 2D Curve Patch control point";
  ot->idname = "PAINT_OT_image_curve_patch_handle_type_set";

  ot->invoke = WM_menu_invoke;
  ot->exec = image_curve_patch_handle_type_set_exec;
  ot->poll = image_curve_patch_active_point_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          ed::curves::rna_enum_set_handle_type_items,
                          int(ed::curves::SetHandleType::Auto),
                          "Type",
                          nullptr);
}

static wmOperatorStatus image_curve_patch_delete_point_exec(bContext *C, wmOperator *op)
{
  ImageCurvePatchSession &session = *image_curve_patch_session_active_get();
  ImageCurvePatchHost host(session, op->reports);
  if (!curve_patch_action_delete_point(*C, host, session.doc.active_point)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_curve_patch_delete_point(wmOperatorType *ot)
{
  ot->name = "Delete Curve Patch Point";
  ot->description = "Delete the active 2D Curve Patch control point";
  ot->idname = "PAINT_OT_image_curve_patch_delete_point";

  ot->exec = image_curve_patch_delete_point_exec;
  ot->poll = image_curve_patch_active_point_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

static wmOperatorStatus image_curve_patch_toggle_cyclic_exec(bContext *C, wmOperator *op)
{
  ImageCurvePatchSession &session = *image_curve_patch_session_active_get();
  ImageCurvePatchHost host(session, op->reports);
  if (!curve_patch_action_toggle_cyclic(*C, host)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_curve_patch_toggle_cyclic(wmOperatorType *ot)
{
  ot->name = "Toggle Curve Patch Cyclic";
  ot->description = "Close or re-open the 2D Curve Patch control curve";
  ot->idname = "PAINT_OT_image_curve_patch_toggle_cyclic";

  ot->exec = image_curve_patch_toggle_cyclic_exec;
  /* Acts on the whole curve, so unlike the point operators it needs no active point. */
  ot->poll = image_curve_patch_op_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

static wmOperatorStatus image_curve_patch_switch_direction_exec(bContext *C, wmOperator *op)
{
  ImageCurvePatchSession &session = *image_curve_patch_session_active_get();
  ImageCurvePatchHost host(session, op->reports);
  curve_patch_action_switch_direction(*C, host);
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_curve_patch_switch_direction(wmOperatorType *ot)
{
  ot->name = "Switch Curve Patch Direction";
  ot->description = "Reverse the 2D Curve Patch control curve, swapping its start and end";
  ot->idname = "PAINT_OT_image_curve_patch_switch_direction";

  ot->exec = image_curve_patch_switch_direction_exec;
  ot->poll = image_curve_patch_op_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

/** Right-click menu over a control point. */
static void image_curve_patch_context_menu_open(bContext *C, const ImageCurvePatchSession &session)
{
  ui::PopupMenu *pup = ui::popup_menu_begin(C, IFACE_("Curve Patch"), ICON_NONE);
  ui::Layout &layout = *ui::popup_menu_layout(pup);
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);
  layout.op_menu_enum(
      C, "PAINT_OT_image_curve_patch_handle_type_set", "type", IFACE_("Handle Type"), ICON_NONE);

  const bool is_cyclic = paintcurve_geometry_is_valid(session.curve) &&
                         session.curve.curves_num() > 0 && session.curve.cyclic()[0];
  layout.separator();
  layout.op("PAINT_OT_image_curve_patch_toggle_cyclic",
            is_cyclic ? IFACE_("Open Curve") : IFACE_("Close Curve"),
            ICON_NONE);
  layout.op("PAINT_OT_image_curve_patch_switch_direction", IFACE_("Switch Direction"), ICON_NONE);

  layout.separator();
  layout.op("PAINT_OT_image_curve_patch_delete_point", std::nullopt, ICON_NONE);
  ui::popup_menu_end(C, pup);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Modal Operator Registration
 * \{ */

void PAINT_OT_image_curve_patch_edit(wmOperatorType *ot)
{
  ot->name = "Edit Curve Patch";
  ot->description = "Edit a live 2D Curve Patch in the Image Editor";
  ot->idname = "PAINT_OT_image_curve_patch_edit";

  /* The operator is registered without properties; modal-edit identity lives entirely in the
   * singleton session. */
  ot->flag = OPTYPE_INTERNAL;

  ot->invoke = curve_patch_edit_invoke;
  ot->modal = curve_patch_edit_modal;
  ot->cancel = curve_patch_edit_cancel;
  ot->poll = curve_patch_edit_poll;
}

/* \} */

}  // namespace ed::sculpt_paint::image::curve_patch::edit
}  // namespace blender
