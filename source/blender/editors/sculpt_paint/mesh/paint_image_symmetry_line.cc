/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Interactive editing session of the canvas-space symmetry (#ImagePaintSettings::symmetry_type).
 *
 * The symmetry itself is always drawn by the Image Editor overlay while it is enabled; its
 * handles only exist during this session, so they never get in the way of painting:
 *
 * - The pivot (diamond) moves the whole symmetry.
 * - The line ends rotate the line and set its displayed length (Shift snaps the angle).
 * - The extent handle sets the circle radius or the spacing of the parallel copies.
 * - Dragging on empty canvas places the symmetry anew at the press.
 *
 * Enter or right-click confirms, Esc restores the symmetry as it was when the session started.
 * Running the operator again while a session is active ends it. Events outside the canvas and
 * those the session does not use pass through, so the view can still be navigated.
 */

#include <algorithm>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_constants.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_image_paint_symmetry.hh"
#include "ED_screen.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "paint_image_select_intern.hh"

namespace blender {

namespace symmetry = ed::image_paint_symmetry;

/* -------------------------------------------------------------------- */
/** \name Session state
 * \{ */

/** Cursor distance (UI-scaled pixels) that grabs a handle. */
static constexpr float SYMMETRY_EDIT_GRAB_RADIUS = 14.0f;
/** Cursor travel (pixels) before a press on empty canvas starts setting the direction. */
static constexpr float SYMMETRY_EDIT_PLACE_THRESHOLD = 4.0f;
/** Shift-drag snap for the line direction. */
static constexpr float SYMMETRY_EDIT_SNAP_ANGLE = float(M_PI) / 12.0f;

/** Everything the session may change, restored on Esc. */
struct SymmetrySettingsBackup {
  float pivot[2];
  float angle;
  float length;
  float radius;
  float width;
  short flag;

  void store(const ImagePaintSettings &s)
  {
    copy_v2_v2(this->pivot, s.symmetry_line_pivot);
    this->angle = s.symmetry_line_angle;
    this->length = s.symmetry_line_length;
    this->radius = s.symmetry_circle_radius;
    this->width = s.symmetry_parallel_width;
    this->flag = s.symmetry_line_flag;
  }

  void restore(ImagePaintSettings &s) const
  {
    copy_v2_v2(s.symmetry_line_pivot, this->pivot);
    s.symmetry_line_angle = this->angle;
    s.symmetry_line_length = this->length;
    s.symmetry_circle_radius = this->radius;
    s.symmetry_parallel_width = this->width;
    s.symmetry_line_flag = this->flag;
  }
};

enum class SymmetryDrag {
  None,
  /** A handle of #symmetry::EditHandle is dragged. */
  Handle,
  /** Press on empty canvas: the pivot is at the press, the drag sets the direction/radius. */
  Place,
};

struct SymmetryEditState {
  SpaceImage *sima = nullptr;
  ARegion *region = nullptr;
  SymmetrySettingsBackup backup;

  SymmetryDrag drag = SymmetryDrag::None;
  symmetry::EditHandle handle = symmetry::EditHandle::None;
  float2 press_uv = float2(0.0f);
  float2 press_mval = float2(0.0f);
  float2 press_pivot = float2(0.0f);
};

/**
 * Only one session runs at a time. The overlay needs to know about it from the draw code, and a
 * second invoke of the operator uses it to end the running session (toggle behavior).
 */
static const SpaceImage *g_session_sima = nullptr;
static symmetry::EditHandle g_hot_handle = symmetry::EditHandle::None;
static bool g_session_exit_requested = false;

namespace ed::image_paint_symmetry {

bool edit_session_active(const SpaceImage *sima)
{
  return sima != nullptr && sima == g_session_sima;
}

EditHandle edit_session_hot_handle()
{
  return g_hot_handle;
}

}  // namespace ed::image_paint_symmetry

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/** Cursor in canvas-region pixels; `event->mval` is relative to the handler's region. */
static float2 symmetry_edit_region_mval(const ARegion *region, const wmEvent *event)
{
  return float2(float(event->xy[0] - region->winrct.xmin),
                float(event->xy[1] - region->winrct.ymin));
}

static float2 symmetry_edit_region_uv(const ARegion *region, const float2 &mval)
{
  float2 uv;
  ui::view2d_region_to_view(&region->v2d, mval.x, mval.y, &uv.x, &uv.y);
  return uv;
}

static float symmetry_edit_distance_px(const ARegion *region,
                                       const float2 &mval,
                                       const float2 &uv)
{
  float2 co;
  ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &co.x, &co.y);
  return math::distance(mval, co);
}

static float symmetry_edit_snap_angle(const float angle, const wmEvent *event)
{
  if (event->modifier & KM_SHIFT) {
    return std::round(angle / SYMMETRY_EDIT_SNAP_ANGLE) * SYMMETRY_EDIT_SNAP_ANGLE;
  }
  return angle;
}

/** The handle under \a mval; the pivot wins over the others when they overlap. */
static symmetry::EditHandle symmetry_edit_hit_test(const ARegion *region,
                                                   const ImagePaintSettings &imapaint,
                                                   const Image *ima,
                                                   const float2 &mval)
{
  const symmetry::EditHandles handles = symmetry::edit_handles_calc(
      imapaint, symmetry::active_tile_origin(ima));
  const float grab = SYMMETRY_EDIT_GRAB_RADIUS * UI_SCALE_FAC;
  if (symmetry_edit_distance_px(region, mval, handles.pivot) <= grab) {
    return symmetry::EditHandle::Pivot;
  }
  if (handles.has_extent && symmetry_edit_distance_px(region, mval, handles.extent) <= grab) {
    return symmetry::EditHandle::Extent;
  }
  if (handles.has_ends) {
    if (symmetry_edit_distance_px(region, mval, handles.end_a) <= grab) {
      return symmetry::EditHandle::EndA;
    }
    if (symmetry_edit_distance_px(region, mval, handles.end_b) <= grab) {
      return symmetry::EditHandle::EndB;
    }
  }
  return symmetry::EditHandle::None;
}

/** Aim the line at \a uv from the pivot, and stretch its displayed length to reach it. */
static void symmetry_edit_drag_end(ImagePaintSettings &imapaint,
                                   const Image *ima,
                                   const float2 &uv,
                                   const bool is_end_a,
                                   const wmEvent *event)
{
  const float2 pivot(imapaint.symmetry_line_pivot[0], imapaint.symmetry_line_pivot[1]);
  /* End A lies on the negative side of the direction. */
  const float2 rel = is_end_a ? pivot - uv : uv - pivot;
  const float distance = math::length(rel);
  if (distance < 1e-6f) {
    return;
  }
  imapaint.symmetry_line_angle = symmetry_edit_snap_angle(math::atan2(rel.y, rel.x), event);

  const float2 dir(cosf(imapaint.symmetry_line_angle), sinf(imapaint.symmetry_line_angle));
  float t0, t1;
  if (symmetry::line_clip_to_tile(pivot, dir, symmetry::active_tile_origin(ima), t0, t1)) {
    const float extent = is_end_a ? -t0 : t1;
    if (extent > 1e-6f) {
      imapaint.symmetry_line_length = std::clamp(distance / extent, 0.05f, 1.0f);
    }
  }
}

static void symmetry_edit_apply_drag(SymmetryEditState &state,
                                     ImagePaintSettings &imapaint,
                                     const Image *ima,
                                     const wmEvent *event)
{
  const float2 mval = symmetry_edit_region_mval(state.region, event);
  const float2 uv = symmetry_edit_region_uv(state.region, mval);
  const float2 pivot(imapaint.symmetry_line_pivot[0], imapaint.symmetry_line_pivot[1]);
  const bool is_circle = imapaint.symmetry_type == IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE;

  if (state.drag == SymmetryDrag::Place) {
    if (math::distance(mval, state.press_mval) < SYMMETRY_EDIT_PLACE_THRESHOLD * UI_SCALE_FAC) {
      return;
    }
    const float2 rel = uv - pivot;
    if (is_circle) {
      imapaint.symmetry_circle_radius = std::max(math::length(rel), 1e-3f);
    }
    imapaint.symmetry_line_angle = symmetry_edit_snap_angle(math::atan2(rel.y, rel.x), event);
    return;
  }

  switch (state.handle) {
    case symmetry::EditHandle::Pivot: {
      const float2 new_pivot = state.press_pivot + (uv - state.press_uv);
      copy_v2_v2(imapaint.symmetry_line_pivot, new_pivot);
      break;
    }
    case symmetry::EditHandle::EndA:
    case symmetry::EditHandle::EndB:
      symmetry_edit_drag_end(imapaint, ima, uv, state.handle == symmetry::EditHandle::EndA, event);
      break;
    case symmetry::EditHandle::Extent: {
      const float2 rel = uv - pivot;
      if (is_circle) {
        imapaint.symmetry_circle_radius = std::max(math::length(rel), 1e-3f);
        if (math::length_squared(rel) > 1e-12f) {
          imapaint.symmetry_line_angle = math::atan2(rel.y, rel.x);
        }
      }
      else {
        const float2 normal(-sinf(imapaint.symmetry_line_angle),
                            cosf(imapaint.symmetry_line_angle));
        imapaint.symmetry_parallel_width = std::max(std::abs(math::dot(rel, normal)), 1e-3f);
      }
      break;
    }
    case symmetry::EditHandle::None:
      break;
  }
}

static void symmetry_edit_begin_drag(SymmetryEditState &state,
                                     ImagePaintSettings &imapaint,
                                     const Image *ima,
                                     const wmEvent *event)
{
  const float2 mval = symmetry_edit_region_mval(state.region, event);
  const float2 uv = symmetry_edit_region_uv(state.region, mval);
  state.press_uv = uv;
  state.press_mval = mval;
  state.press_pivot = float2(imapaint.symmetry_line_pivot[0], imapaint.symmetry_line_pivot[1]);
  state.handle = symmetry_edit_hit_test(state.region, imapaint, ima, mval);
  if (state.handle != symmetry::EditHandle::None) {
    state.drag = SymmetryDrag::Handle;
    return;
  }
  state.drag = SymmetryDrag::Place;
  copy_v2_v2(imapaint.symmetry_line_pivot, uv);
  if (imapaint.symmetry_type != IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE) {
    /* A new line spans the whole tile again. */
    imapaint.symmetry_line_length = 1.0f;
  }
}

/**
 * The brush circle is hidden during the session (see #paint_brush_cursor_poll), so the system
 * cursor tells what a press would do: an open hand over a handle, a closed one while dragging
 * it, a cross while placing the symmetry anew.
 */
static void symmetry_edit_cursor_update(bContext *C, const SymmetryEditState &state)
{
  int cursor = WM_CURSOR_DEFAULT;
  if (state.drag == SymmetryDrag::Handle) {
    cursor = WM_CURSOR_HAND_CLOSED;
  }
  else if (state.drag == SymmetryDrag::Place) {
    cursor = WM_CURSOR_CROSS;
  }
  else if (g_hot_handle != symmetry::EditHandle::None) {
    cursor = WM_CURSOR_HAND;
  }
  WM_cursor_modal_set(CTX_wm_window(C), cursor);
}

static void symmetry_edit_status_update(bContext *C, const SymmetryEditState &state)
{
  symmetry_edit_cursor_update(C, state);
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr) {
    return;
  }
  const char *text = nullptr;
  if (state.drag == SymmetryDrag::None) {
    text =
        "Canvas symmetry: drag the diamond to move, the ends to rotate and resize, the outer "
        "handle to set the radius/spacing, or drag on the canvas to place anew. "
        "Enter/Right-click to confirm, Esc to cancel";
  }
  else if (state.drag == SymmetryDrag::Place || state.handle != symmetry::EditHandle::Pivot) {
    text = "Shift to snap the angle, release to finish the drag";
  }
  else {
    text = "Release to finish the drag";
  }
  ED_area_status_text(area, text);
}

/** Every Image Editor showing the scene draws the symmetry, not only the one being edited. */
static void symmetry_edit_tag_redraw(bContext *C, ARegion *region)
{
  ED_region_tag_redraw(region);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

static void symmetry_edit_exit(bContext *C, wmOperator *op)
{
  SymmetryEditState *state = static_cast<SymmetryEditState *>(op->customdata);
  if (state && state->region) {
    symmetry_edit_tag_redraw(C, state->region);
  }
  MEM_delete(state);
  op->customdata = nullptr;
  g_session_sima = nullptr;
  g_hot_handle = symmetry::EditHandle::None;
  g_session_exit_requested = false;
  WM_cursor_modal_restore(CTX_wm_window(C));
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator callbacks
 * \{ */

static wmOperatorStatus symmetry_edit_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (g_session_sima != nullptr) {
    /* Toggle: the running session ends on its next event. */
    g_session_exit_requested = true;
    WM_event_add_mousemove(CTX_wm_window(C));
    return OPERATOR_CANCELLED;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  Image *ima = sima ? sima->image : nullptr;
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = area ? BKE_area_find_region_type(area, RGN_TYPE_WINDOW) : nullptr;
  Scene *scene = CTX_data_scene(C);
  if (!sima || !ima || !region || !scene) {
    return OPERATOR_CANCELLED;
  }
  ImagePaintSettings &imapaint = scene->toolsettings->imapaint;

  SymmetryEditState *state = MEM_new<SymmetryEditState>(__func__);
  state->sima = sima;
  state->region = region;
  state->backup.store(imapaint);

  if (!(imapaint.symmetry_line_flag & IMAGE_PAINT_SYMMETRY_LINE_ENABLED)) {
    /* First use: a vertical line through the active tile's center, like Photoshop's default. */
    const float2 center = symmetry::active_tile_origin(ima) + float2(0.5f);
    copy_v2_v2(imapaint.symmetry_line_pivot, center);
    imapaint.symmetry_line_angle = float(M_PI) / 2.0f;
    imapaint.symmetry_line_length = 1.0f;
    imapaint.symmetry_line_flag |= IMAGE_PAINT_SYMMETRY_LINE_ENABLED;
  }

  /* From a keymap press on the canvas the drag starts right away; from a button or a menu the
   * invoking event is the click on it. */
  if (CTX_wm_region(C) == region && event->type == LEFTMOUSE && event->val == KM_PRESS) {
    symmetry_edit_begin_drag(*state, imapaint, ima, event);
  }

  op->customdata = state;
  g_session_sima = sima;
  g_session_exit_requested = false;
  WM_event_add_modal_handler(C, op);
  symmetry_edit_status_update(C, *state);
  symmetry_edit_tag_redraw(C, region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus symmetry_edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  SymmetryEditState *state = static_cast<SymmetryEditState *>(op->customdata);
  SpaceImage *sima = CTX_wm_space_image(C);
  Scene *scene = CTX_data_scene(C);
  if (state == nullptr || !scene || sima != state->sima || !sima->image ||
      sima->mode != SI_MODE_PAINT || !symmetry::canvas_mode_active(*scene->toolsettings))
  {
    symmetry_edit_exit(C, op);
    return OPERATOR_FINISHED;
  }
  if (g_session_exit_requested && state->drag == SymmetryDrag::None) {
    symmetry_edit_exit(C, op);
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
    return OPERATOR_FINISHED;
  }

  ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  ARegion *region = state->region;
  const bool in_canvas = BLI_rcti_isect_pt_v(&region->winrct, event->xy);

  switch (event->type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      if (state->drag != SymmetryDrag::None) {
        symmetry_edit_apply_drag(*state, imapaint, sima->image, event);
        symmetry_edit_tag_redraw(C, region);
        return OPERATOR_RUNNING_MODAL;
      }
      const symmetry::EditHandle hot = in_canvas ?
                                           symmetry_edit_hit_test(
                                               region,
                                               imapaint,
                                               sima->image,
                                               symmetry_edit_region_mval(region, event)) :
                                           symmetry::EditHandle::None;
      if (hot != g_hot_handle) {
        g_hot_handle = hot;
        symmetry_edit_cursor_update(C, *state);
        ED_region_tag_redraw(region);
      }
      return OPERATOR_PASS_THROUGH;
    }

    case LEFTMOUSE: {
      if (event->val == KM_PRESS && in_canvas && state->drag == SymmetryDrag::None) {
        symmetry_edit_begin_drag(*state, imapaint, sima->image, event);
        g_hot_handle = state->handle;
        symmetry_edit_status_update(C, *state);
        symmetry_edit_tag_redraw(C, region);
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->val == KM_RELEASE && state->drag != SymmetryDrag::None) {
        state->drag = SymmetryDrag::None;
        state->handle = symmetry::EditHandle::None;
        symmetry_edit_status_update(C, *state);
        symmetry_edit_tag_redraw(C, region);
        WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
        return OPERATOR_RUNNING_MODAL;
      }
      return in_canvas ? OPERATOR_RUNNING_MODAL : OPERATOR_PASS_THROUGH;
    }

    case EVT_RETKEY:
    case EVT_PADENTER:
    case RIGHTMOUSE: {
      if (event->val == KM_PRESS && (in_canvas || event->type != RIGHTMOUSE)) {
        symmetry_edit_exit(C, op);
        WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_PASS_THROUGH;
    }

    case EVT_ESCKEY: {
      if (event->val == KM_PRESS) {
        state->backup.restore(imapaint);
        symmetry_edit_exit(C, op);
        WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    default:
      return OPERATOR_PASS_THROUGH;
  }
}

static void symmetry_edit_cancel(bContext *C, wmOperator *op)
{
  SymmetryEditState *state = static_cast<SymmetryEditState *>(op->customdata);
  if (state == nullptr) {
    return;
  }
  if (Scene *scene = CTX_data_scene(C)) {
    state->backup.restore(scene->toolsettings->imapaint);
  }
  symmetry_edit_exit(C, op);
}

static bool symmetry_edit_poll(bContext *C)
{
  if (!image_paint_selection_poll(C)) {
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || !symmetry::canvas_mode_active(*scene->toolsettings)) {
    CTX_wm_operator_poll_msg_set(C, "Canvas symmetry needs the 2D Canvas symmetry mode");
    return false;
  }
  return true;
}

/** \} */

void PAINT_OT_image_symmetry_edit(wmOperatorType *ot)
{
  ot->name = "Edit Canvas Symmetry";
  ot->idname = "PAINT_OT_image_symmetry_edit";
  ot->description =
      "Show the canvas symmetry handles to place, move, rotate and resize it; run again, press "
      "Enter or right-click to finish";

  ot->invoke = symmetry_edit_invoke;
  ot->modal = symmetry_edit_modal;
  ot->cancel = symmetry_edit_cancel;
  ot->poll = symmetry_edit_poll;
  /* No #OPTYPE_UNDO: tool settings are not part of the image paint undo stack, so an undo step
   * would restore nothing. Esc restores the symmetry instead. */
  ot->flag = OPTYPE_REGISTER;
}

}  // namespace blender
