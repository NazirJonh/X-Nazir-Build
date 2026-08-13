/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 * \brief Canvas rotation operators for the Image Editor.
 *
 * These only ever write #SpaceImage.rotation and #SpaceImage.rotation_pivot. Everything that has to
 * reproduce the rotation (drawing, coordinate conversion, tools) reads it back through
 * #View2D.rotation, which `image_main_region_set_view2d` re-syncs on every redraw.
 */

#include <cfloat>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_space_enums.h"
#include "DNA_space_types.h"

#include "BLI_dial_2d.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"

#include "ED_image.hh"
#include "ED_screen.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_vertex_format.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLF_api.hh"

#include "image_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Shared Utilities
 * \{ */

/** Canvas rotation is meaningless where the view is not a plain image canvas (the UV editor). */
static bool space_image_rotation_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  return ED_space_image_rotation_supported(sima);
}

/**
 * Single write path for the rotation, so wrapping and the redraw/notify pair cannot be forgotten by
 * one of the operators.
 */
static void image_view_rotation_set(bContext *C, SpaceImage *sima, const float rotation)
{
  sima->rotation = angle_wrap_rad(rotation);

  ED_region_tag_redraw(CTX_wm_region(C));
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Fixed Angle Operators
 * \{ */

static wmOperatorStatus image_view_rotate_cw_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  image_view_rotation_set(C, sima, sima->rotation - DEG2RADF(90.0f));
  return OPERATOR_FINISHED;
}

void IMAGE_OT_view_rotate_cw(wmOperatorType *ot)
{
  ot->name = "Rotate View Clockwise";
  ot->idname = "IMAGE_OT_view_rotate_cw";
  ot->description = "Rotate the canvas view 90 degrees clockwise";

  ot->exec = image_view_rotate_cw_exec;
  ot->poll = space_image_rotation_poll;

  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus image_view_rotate_ccw_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  image_view_rotation_set(C, sima, sima->rotation + DEG2RADF(90.0f));
  return OPERATOR_FINISHED;
}

void IMAGE_OT_view_rotate_ccw(wmOperatorType *ot)
{
  ot->name = "Rotate View Counter-Clockwise";
  ot->idname = "IMAGE_OT_view_rotate_ccw";
  ot->description = "Rotate the canvas view 90 degrees counter-clockwise";

  ot->exec = image_view_rotate_ccw_exec;
  ot->poll = space_image_rotation_poll;

  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus image_view_rotate_reset_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  image_view_rotation_set(C, sima, 0.0f);
  return OPERATOR_FINISHED;
}

void IMAGE_OT_view_rotate_reset(wmOperatorType *ot)
{
  ot->name = "Reset View Rotation";
  ot->idname = "IMAGE_OT_view_rotate_reset";
  ot->description = "Reset the canvas rotation to default";

  ot->exec = image_view_rotate_reset_exec;
  ot->poll = space_image_rotation_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Interactive Rotation Operator
 * \{ */

struct ImageRotateInteractiveData {
  /** View state captured on invoke, restored on cancel. */
  float initial_rotation;
  float initial_pivot[2];
  float initial_offset[2];

  /** Region the modal was started in. The draw callback is registered per space type, so it must
   * ignore every other region of the same type. */
  ARegion *region;
  /** Window-space position the indicator is drawn at. */
  float pivot_win[2];

  Dial *dial;
  short init_event_type;
  wmPaintCursor *cursor;
  bool use_center_pivot;
};

/**
 * Compensate the view offset (`sima->xof/yof`) so the displayed image does not jump when the
 * rotation pivot changes while rotation is non-zero.
 *
 * The canvas-rotation pipelines (DRW image engine, view2d overlays, Python POST_VIEW) all rotate
 * about the pivot's axis-aligned pixel position and leave that pixel fixed. So the canvas point that
 * equals the pivot is always displayed at its axis-aligned pixel position, regardless of rotation.
 * When the pivot changes from `pivot_old` to `pivot_new`, the canvas point `pivot_new` must stay
 * where it was on screen before the change. Before the change it was drawn (under rotation about
 * `pivot_old`) at `screen_old`; after the change it sits at `pivot_new`'s axis-aligned pixel
 * position. We pan the view by that pixel delta, converting to `xof/yof` units via `sima->zoom`
 * (see `image_main_region_set_view2d`: `dpixel = -zoom * dxof`).
 */
static void image_view_offset_compensate_pivot_change(SpaceImage *sima,
                                                      ARegion *region,
                                                      const float pivot_old[2],
                                                      const float pivot_new[2])
{
  if (sima->rotation == 0.0f) {
    return;
  }
  if (equals_v2v2(pivot_old, pivot_new)) {
    return;
  }

  /* Where `pivot_new` is displayed right now, under the rotation about `pivot_old`.
   * #ED_image_point_pos__reverse is rotation-aware (it applies the canvas rotation about the
   * current pivot when mapping view->screen). The View2D still holds `pivot_old`. */
  float screen_old[2];
  ED_image_point_pos__reverse(sima, region, pivot_new, screen_old);

  /* Axis-aligned pixel position of `pivot_new` - where it will sit once it becomes the pivot (the
   * pivot pixel is invariant under rotation). */
  float pivot_px_new[2];
  ui::view2d_view_to_region_navigation_fl(
      &region->v2d, pivot_new[0], pivot_new[1], &pivot_px_new[0], &pivot_px_new[1]);

  /* Pan so that `pivot_new` stays at `screen_old`: `pivot_px_new + (-zoom * dxof) = screen_old`,
   * i.e. `dxof = (pivot_px_new - screen_old) / zoom`. */
  float zoomx, zoomy;
  ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);

  sima->xof += (pivot_px_new[0] - screen_old[0]) / zoomx;
  sima->yof += (pivot_px_new[1] - screen_old[1]) / zoomy;
}

/**
 * Snap the pivot to the canvas center or a corner when the cursor is close enough to one in screen
 * space, so the common cases are exactly reachable without precise aiming.
 */
static void image_rotation_pivot_snap(SpaceImage *sima,
                                      ARegion *region,
                                      const int mval[2],
                                      float r_pivot[2])
{
  /* The center wins over a corner within a tighter radius, it is the more likely intent. */
  const float snap_distance_px = 12.0f * UI_SCALE_FAC;
  const float center_distance_px = snap_distance_px * 0.8f;

  const float cursor_px[2] = {float(mval[0]), float(mval[1])};
  float best_dist = snap_distance_px;

  auto try_snap = [&](const float u, const float v, const float max_dist) {
    const float candidate[2] = {u, v};
    float candidate_px[2];
    ED_image_point_pos__reverse(sima, region, candidate, candidate_px);
    const float dist = len_v2v2(candidate_px, cursor_px);
    if (dist < max_dist && dist < best_dist) {
      copy_v2_v2(r_pivot, candidate);
      best_dist = dist;
      return true;
    }
    return false;
  };

  if (try_snap(0.5f, 0.5f, center_distance_px)) {
    return;
  }
  const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
  for (const float(&corner)[2] : corners) {
    try_snap(corner[0], corner[1], snap_distance_px);
  }
}

static void image_view_rotate_interactive_update_header(bContext *C)
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  ScrArea *area = CTX_wm_area(C);
  if (sima == nullptr || area == nullptr) {
    return;
  }

  char msg[UI_MAX_DRAW_STR];
  SNPRINTF_UTF8(msg, IFACE_("Rotation: %.1f°"), RAD2DEGF(sima->rotation));
  ED_area_status_text(area, msg);
}

/**
 * Canvas X/Y axes as a gizmo-style cross with labeled end caps, drawn at the origin of the current
 * matrix. The canvas is displayed as `screen = Rot(-rotation) * view`, so the axes use the negated
 * angle to stay aligned with it.
 */
static void image_rotate_draw_axis_indicator(const float rotation)
{
  const float axis_size = 100.0f * UI_SCALE_FAC;
  const float axis_handle_size = 0.12f;

  const float cos_r = cosf(-rotation);
  const float sin_r = sinf(-rotation);
  const float x_dir[2] = {cos_r, sin_r};
  const float y_dir[2] = {-sin_r, cos_r};

  const float axis_length = axis_size * (1.0f - axis_handle_size);
  const float rad = axis_size * axis_handle_size;
  const float circle_distance = axis_length + rad;

  const float x_color[3] = {0.9f, 0.2f, 0.2f};
  const float y_color[3] = {0.2f, 0.9f, 0.2f};

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);

  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);

  /* Both axes in one batch: the end is opaque, the center fades out so the cross stays readable on
   * top of the image. */
  {
    GPUVertFormat *format = immVertexFormat();
    const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    const uint color_id = GPU_vertformat_attr_add(
        format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);

    immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_SMOOTH_COLOR);
    immUniform2fv("viewportSize", &viewport_size[2]);
    immUniform1f("lineWidth", 2.0f * UI_SCALE_FAC);
    immUniform1i("lineSmooth", 1);

    const float *axis_dir[2] = {x_dir, y_dir};
    const float *axis_color[2] = {x_color, y_color};

    immBegin(GPU_PRIM_LINES, 4);
    for (int axis = 0; axis < 2; axis++) {
      const float *color = axis_color[axis];
      const float *dir = axis_dir[axis];
      const float center_color[4] = {color[0], color[1], color[2], 0.6f};
      const float end_color[4] = {color[0], color[1], color[2], 1.0f};
      immAttr4fv(color_id, center_color);
      immVertex2f(pos, 0.0f, 0.0f);
      immAttr4fv(color_id, end_color);
      immVertex2f(pos, dir[0] * axis_length, dir[1] * axis_length);
    }
    immEnd();
    immUnbindProgram();
  }

  /* Crosshair marking the exact pivot pixel. */
  {
    const float cross_size = 6.0f * UI_SCALE_FAC;
    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor3f(1.0f, 1.0f, 1.0f);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex2f(pos, -cross_size * x_dir[0], -cross_size * x_dir[1]);
    immVertex2f(pos, cross_size * x_dir[0], cross_size * x_dir[1]);
    immVertex2f(pos, -cross_size * y_dir[0], -cross_size * y_dir[1]);
    immVertex2f(pos, cross_size * y_dir[0], cross_size * y_dir[1]);
    immEnd();
    immUnbindProgram();
  }

  GPU_line_smooth(false);

  const float x_end_circle[2] = {x_dir[0] * circle_distance, x_dir[1] * circle_distance};
  const float y_end_circle[2] = {y_dir[0] * circle_distance, y_dir[1] * circle_distance};

  auto draw_axis_circle = [&](const float center[2], const float color[3]) {
    ui::draw_roundbox_corner_set(ui::CNR_ALL);
    GPU_matrix_push();
    GPU_matrix_translate_2fv(center);

    rctf rect{};
    rect.xmin = -rad;
    rect.xmax = rad;
    rect.ymin = -rad;
    rect.ymax = rad;

    const float fill_color[4] = {color[0], color[1], color[2], 1.0f};
    ui::draw_roundbox_4fv(&rect, true, rad, fill_color);
    ui::draw_roundbox_4fv_ex(
        &rect, fill_color, nullptr, 0.0f, fill_color, 1.5f * UI_SCALE_FAC, rad);

    GPU_matrix_pop();
  };

  draw_axis_circle(x_end_circle, x_color);
  draw_axis_circle(y_end_circle, y_color);

  /* Axis labels, centered in the end caps. */
  {
    const int font_id = BLF_default();
    BLF_enable(font_id, BLF_BOLD);
    BLF_size(font_id, int(14.0f * UI_SCALE_FAC));
    const struct {
      const float *pos;
      const char *label;
    } labels[] = {{x_end_circle, "+X"}, {y_end_circle, "+Y"}};

    const float text_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    BLF_color4fv(font_id, text_color);
    for (const auto &label_data : labels) {
      const size_t label_len = strlen(label_data.label);
      float text_w = 0.0f, text_h = 0.0f;
      BLF_width_and_height(font_id, label_data.label, label_len, &text_w, &text_h);
      BLF_position(
          font_id, label_data.pos[0] - text_w * 0.5f, label_data.pos[1] - text_h * 0.5f, 0);
      BLF_draw(font_id, label_data.label, label_len);
    }
    BLF_disable(font_id, BLF_BOLD);
  }

  GPU_blend(GPU_BLEND_NONE);
}

/**
 * Dial hinting at the drag gesture, drawn at the origin of the current matrix. Only shown when the
 * pivot follows the cursor, where the user is expected to drag around it.
 */
static void image_rotate_draw_dial()
{
  const float dial_radius = 36.0f * UI_SCALE_FAC;
  const float color[3] = {71.0f / 255.0f, 114.0f / 255.0f, 179.0f / 255.0f};
  const float start_angle = -0.75f * float(M_PI);
  const float end_angle = 0.75f * float(M_PI);

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);

  /* Cross marking the drag origin. */
  {
    const float cross_size = 15.0f * UI_SCALE_FAC;
    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor3fv(color);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex2f(pos, -cross_size, 0.0f);
    immVertex2f(pos, cross_size, 0.0f);
    immVertex2f(pos, 0.0f, -cross_size);
    immVertex2f(pos, 0.0f, cross_size);
    immEnd();
    immUnbindProgram();
  }

  /* Arc plus an arrow head at its end, indicating the drag direction. */
  {
    const int arc_segments = 24;
    const float angle_step = (end_angle - start_angle) / arc_segments;

    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
    immUniformColor3fv(color);
    float viewport_size[4];
    GPU_viewport_size_get_f(viewport_size);
    immUniform2fv("viewportSize", &viewport_size[2]);
    immUniform1f("lineWidth", 2.0f * UI_SCALE_FAC);
    immUniform1i("lineSmooth", 1);

    immBegin(GPU_PRIM_LINE_STRIP, arc_segments + 1);
    for (int i = 0; i <= arc_segments; i++) {
      const float angle = start_angle + float(i) * angle_step;
      immVertex2f(pos, cosf(angle) * dial_radius, sinf(angle) * dial_radius);
    }
    immEnd();

    const float arrow_size = 12.0f * UI_SCALE_FAC;
    const float half_arrow_angle = 0.4f;
    /* Tangent at the arc end, so the head points along the sweep. */
    const float back_angle = end_angle - float(M_PI_2);
    const float arrow_x = cosf(end_angle) * dial_radius;
    const float arrow_y = sinf(end_angle) * dial_radius;
    immBegin(GPU_PRIM_LINES, 4);
    for (const float side : {-half_arrow_angle, half_arrow_angle}) {
      immVertex2f(pos, arrow_x, arrow_y);
      immVertex2f(pos,
                  arrow_x + cosf(back_angle + side) * arrow_size,
                  arrow_y + sinf(back_angle + side) * arrow_size);
    }
    immEnd();
    immUnbindProgram();
  }

  GPU_line_smooth(false);
  GPU_blend(GPU_BLEND_NONE);
}

static void image_rotate_draw_pivot(bContext *C,
                                    const int2 & /*xy*/,
                                    const float2 & /*tilt*/,
                                    void *customdata)
{
  const ImageRotateInteractiveData *data = static_cast<ImageRotateInteractiveData *>(customdata);
  const SpaceImage *sima = CTX_wm_space_image(C);
  const ARegion *region = CTX_wm_region(C);
  if (data == nullptr || sima == nullptr || region == nullptr) {
    return;
  }
  /* Paint cursors are registered per space type, so every Image Editor gets this callback. Only the
   * region the modal runs in should show the indicator. */
  if (region != data->region) {
    return;
  }

  GPU_matrix_push_projection();
  GPU_matrix_push();
  wmViewport(&region->winrct);

  GPU_matrix_translate_2f(data->pivot_win[0] - region->winrct.xmin,
                          data->pivot_win[1] - region->winrct.ymin);

  if (!data->use_center_pivot) {
    image_rotate_draw_dial();
  }
  image_rotate_draw_axis_indicator(sima->rotation);

  GPU_matrix_pop();
  GPU_matrix_pop_projection();
}

static wmOperatorStatus image_view_rotate_interactive_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);

  if (sima == nullptr || region == nullptr) {
    return OPERATOR_CANCELLED;
  }

  ImageRotateInteractiveData *data = MEM_new<ImageRotateInteractiveData>(__func__);
  data->initial_rotation = sima->rotation;
  data->init_event_type = event->type;
  data->initial_offset[0] = sima->xof;
  data->initial_offset[1] = sima->yof;
  data->region = region;
  copy_v2_v2(data->initial_pivot, sima->rotation_pivot);
  data->use_center_pivot = RNA_boolean_get(op->ptr, "use_center_pivot");

  /* Invoked from the navigation gizmo button there is no meaningful drag origin, so use the canvas
   * center; otherwise the cursor position becomes the pivot. */
  float pivot[2];
  if (data->use_center_pivot) {
    copy_v2_fl(pivot, 0.5f);
  }
  else {
    ED_image_mouse_pos(sima, region, event->mval, pivot);
    image_rotation_pivot_snap(sima, region, event->mval, pivot);
  }

  image_view_offset_compensate_pivot_change(sima, region, data->initial_pivot, pivot);
  copy_v2_v2(sima->rotation_pivot, pivot);

  /* Anchor the indicator on the pivot itself in center mode; in cursor mode draw it exactly under
   * the click, which is where the user expects the handle even after snapping. */
  if (data->use_center_pivot) {
    float pivot_region[2];
    ED_image_point_pos__reverse(sima, region, pivot, pivot_region);
    data->pivot_win[0] = float(region->winrct.xmin) + pivot_region[0];
    data->pivot_win[1] = float(region->winrct.ymin) + pivot_region[1];
  }
  else {
    data->pivot_win[0] = float(region->winrct.xmin + event->mval[0]);
    data->pivot_win[1] = float(region->winrct.ymin + event->mval[1]);
  }

  const float cursor_win[2] = {float(event->xy[0]), float(event->xy[1])};
  data->dial = BLI_dial_init(cursor_win, FLT_EPSILON);

  data->cursor = WM_paint_cursor_activate(
      SPACE_IMAGE, RGN_TYPE_WINDOW, nullptr, image_rotate_draw_pivot, data);

  /* The paint cursor is only picked up on the next redraw, tag now so the indicator appears
   * immediately rather than on the first mouse move. */
  ED_region_tag_redraw(region);
  ED_region_tag_redraw_editor_overlays(region);

  wmWindow *win = CTX_wm_window(C);
  if (WM_cursor_modal_is_set_ok(win)) {
    WM_cursor_modal_set(win, WM_CURSOR_NSEW_SCROLL);
  }

  op->customdata = data;

  image_view_rotate_interactive_update_header(C);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

/* Tear down the modal state. When `restore_state` is true the view is reset to the values captured
 * on invoke (cancel), otherwise the interactively set rotation is kept (confirm). */
static void image_view_rotate_interactive_finish(bContext *C,
                                                 wmOperator *op,
                                                 const bool restore_state)
{
  ImageRotateInteractiveData *data = static_cast<ImageRotateInteractiveData *>(op->customdata);
  if (data == nullptr) {
    return;
  }

  if (restore_state) {
    if (SpaceImage *sima = CTX_wm_space_image(C)) {
      sima->rotation = data->initial_rotation;
      copy_v2_v2(sima->rotation_pivot, data->initial_pivot);
      sima->xof = data->initial_offset[0];
      sima->yof = data->initial_offset[1];
    }
  }

  WM_cursor_modal_restore(CTX_wm_window(C));
  ED_area_status_text(CTX_wm_area(C), nullptr);
  if (data->cursor) {
    WM_paint_cursor_end(data->cursor);
  }
  if (data->dial) {
    BLI_dial_free(data->dial);
  }
  ED_region_tag_redraw(data->region);

  MEM_delete(data);
  op->customdata = nullptr;

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

static wmOperatorStatus image_view_rotate_interactive_modal(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  ImageRotateInteractiveData *data = static_cast<ImageRotateInteractiveData *>(op->customdata);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (data == nullptr || sima == nullptr) {
    image_view_rotate_interactive_finish(C, op, true);
    return OPERATOR_CANCELLED;
  }

  if (event->type == MOUSEMOVE) {
    const float cursor_win[2] = {float(event->xy[0]), float(event->xy[1])};
    float rotation = data->initial_rotation + BLI_dial_angle(data->dial, cursor_win);

    if (event->modifier & KM_CTRL) {
      const float snap_angle = DEG2RADF(15.0f);
      rotation = roundf(rotation / snap_angle) * snap_angle;
    }

    /* Only tag a redraw here: the notifier would run the full UI update on every mouse move, and
     * #image_view_rotate_interactive_finish sends one once the drag ends. */
    sima->rotation = angle_wrap_rad(rotation);
    ED_region_tag_redraw(data->region);
    image_view_rotate_interactive_update_header(C);
  }

  if (event->type == data->init_event_type && event->val == KM_RELEASE) {
    image_view_rotate_interactive_finish(C, op, false);
    return OPERATOR_FINISHED;
  }
  if (ELEM(event->type, RIGHTMOUSE, EVT_ESCKEY) && event->val == KM_PRESS) {
    image_view_rotate_interactive_finish(C, op, true);
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void image_view_rotate_interactive_cancel(bContext *C, wmOperator *op)
{
  image_view_rotate_interactive_finish(C, op, true);
}

void IMAGE_OT_view_rotate_interactive(wmOperatorType *ot)
{
  ot->name = "Rotate View Interactive";
  ot->idname = "IMAGE_OT_view_rotate_interactive";
  ot->description = "Rotate the canvas";

  ot->invoke = image_view_rotate_interactive_invoke;
  ot->modal = image_view_rotate_interactive_modal;
  ot->cancel = image_view_rotate_interactive_cancel;
  ot->poll = space_image_rotation_poll;

  ot->flag = OPTYPE_BLOCKING | OPTYPE_GRAB_CURSOR_XY;

  PropertyRNA *prop;
  prop = RNA_def_boolean(ot->srna,
                         "use_center_pivot",
                         false,
                         "Use Center Pivot",
                         "Rotate around the center of the image instead of the cursor position");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

}  // namespace blender
