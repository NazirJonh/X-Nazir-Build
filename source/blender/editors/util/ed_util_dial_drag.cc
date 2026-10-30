/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cfloat>
#include <cmath>

#include "ED_util_dial_drag.hh"

#include "MEM_guardedalloc.h"

#include "BLI_dial_2d.h"
#include "BLI_math_base.h"
#include "BLI_math_rotation.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"

#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_screen.hh"

#include "UI_interface_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed {

struct DialDragData {
  const DialDragParams *params = nullptr;
  float initial_angle = 0.0f;
  /** The button that started the drag, so releasing that same button confirms. */
  int init_event_type = 0;
  Dial *dial = nullptr;
  float init_cursor_x = 0.0f;
  ARegion *region = nullptr;
};

static void dial_drag_update_header(bContext *C, const DialDragParams &params, const float angle)
{
  char msg[UI_MAX_DRAW_STR];
  SNPRINTF_UTF8(msg, "%s: %.1f\xc2\xb0", IFACE_(params.header_label), RAD2DEGF(angle));
  ED_area_status_text(CTX_wm_area(C), msg);
}

static void dial_drag_finish(bContext *C, wmOperator *op, const bool restore_state)
{
  DialDragData *data = static_cast<DialDragData *>(op->customdata);
  if (data == nullptr) {
    return;
  }
  if (restore_state) {
    data->params->set_angle(C, data->initial_angle);
  }

  WM_cursor_modal_restore(CTX_wm_window(C));
  ED_area_status_text(CTX_wm_area(C), nullptr);
  if (data->dial != nullptr) {
    BLI_dial_free(data->dial);
  }
  ED_region_tag_redraw(data->region);

  /* Sent on confirm too: the angle is also shown by UI (e.g. a popover slider) that only a
   * notifier refreshes. */
  WM_event_add_notifier(C, data->params->notifier, nullptr);

  MEM_delete(data);
  op->customdata = nullptr;
}

wmOperatorStatus dial_drag_invoke(bContext *C,
                                  wmOperator *op,
                                  const wmEvent *event,
                                  const DialDragParams &params)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return OPERATOR_CANCELLED;
  }

  DialDragData *data = MEM_new<DialDragData>(__func__);
  data->params = &params;
  data->initial_angle = params.get_angle(C);
  data->init_event_type = event->type;
  data->region = region;

  /* By default a dial about the press point: the cursor's bearing drives the angle, scaled by the
   * gain. */
  const float cursor_win[2] = {float(event->xy[0]), float(event->xy[1])};
  data->init_cursor_x = cursor_win[0];
  if (!params.horizontal) {
    data->dial = BLI_dial_init(cursor_win, FLT_EPSILON);
  }

  wmWindow *win = CTX_wm_window(C);
  if (WM_cursor_modal_is_set_ok(win)) {
    WM_cursor_modal_set(win, params.horizontal ? WM_CURSOR_X_MOVE : WM_CURSOR_NSEW_SCROLL);
  }

  op->customdata = data;
  dial_drag_update_header(C, params, data->initial_angle);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

wmOperatorStatus dial_drag_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  DialDragData *data = static_cast<DialDragData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  if (event->type == MOUSEMOVE) {
    const float cursor_win[2] = {float(event->xy[0]), float(event->xy[1])};
    float delta;
    if (data->params->horizontal) {
      /* Dragging to the right turns the angle counter-clockwise, as the light then appears to
       * follow the cursor. */
      delta = -data->params->cursor_gain * (cursor_win[0] - data->init_cursor_x) /
              float(max_ii(data->region->winx, 1));
    }
    else {
      delta = data->params->cursor_gain * BLI_dial_angle(data->dial, cursor_win);
    }
    float angle = data->initial_angle + delta;

    if (event->modifier & KM_CTRL) {
      const float snap_angle = DEG2RADF(DIAL_DRAG_SNAP_DEGREES);
      angle = roundf(angle / snap_angle) * snap_angle;
    }
    angle = angle_wrap_rad(angle);

    /* Only a redraw here. A notifier would run the whole UI update per mouse move, and
     * #dial_drag_finish sends one once the drag ends. */
    data->params->set_angle(C, angle);
    ED_region_tag_redraw(data->region);
    dial_drag_update_header(C, *data->params, angle);
  }

  if (event->type == data->init_event_type && event->val == KM_RELEASE) {
    dial_drag_finish(C, op, false);
    return OPERATOR_FINISHED;
  }
  if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
    dial_drag_finish(C, op, true);
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_RUNNING_MODAL;
}

void dial_drag_cancel(bContext *C, wmOperator *op)
{
  dial_drag_finish(C, op, true);
}

}  // namespace blender::ed
