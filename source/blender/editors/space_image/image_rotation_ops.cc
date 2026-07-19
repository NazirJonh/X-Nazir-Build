/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 * \brief Canvas rotation operators for image editor
 */

#include <algorithm>
#include <cmath>

#include "DNA_space_enums.h"
#include "DNA_space_types.h"

#include "BLI_dial_2d.h"
#include "BLI_math_base.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

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

#include "UI_interface_icons.hh"

#include "image_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Canvas Rotation Operators
 * \{ */

/** Poll function for canvas rotation operators.
 * Rotation is only supported in View, Paint, and Mask modes.
 */
static bool space_image_rotation_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  return ED_space_image_rotation_supported(sima);
}

/**
 * Normalize rotation angle to [-PI, PI] range.
 */
static void normalize_rotation_angle(float &rotation)
{
  while (rotation > float(M_PI)) {
    rotation -= 2.0f * float(M_PI);
  }
  while (rotation < -float(M_PI)) {
    rotation += 2.0f * float(M_PI);
  }
}

/* Rotate 90° Clockwise */
static wmOperatorStatus image_view_rotate_cw_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);

  sima->rotation -= DEG2RADF(90.0f);
  normalize_rotation_angle(sima->rotation);

  ED_region_tag_redraw(region);
  return OPERATOR_FINISHED;
}

void IMAGE_OT_view_rotate_cw(wmOperatorType *ot)
{
  ot->name = "Rotate View 90° Clockwise";
  ot->idname = "IMAGE_OT_view_rotate_cw";
  ot->description = "Rotate the canvas view 90 degrees clockwise";

  ot->exec = image_view_rotate_cw_exec;
  ot->poll = space_image_rotation_poll;

  ot->flag = OPTYPE_REGISTER;
}

/* Rotate 90° Counter-Clockwise */
static wmOperatorStatus image_view_rotate_ccw_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);

  sima->rotation += DEG2RADF(90.0f);
  normalize_rotation_angle(sima->rotation);

  ED_region_tag_redraw(region);
  return OPERATOR_FINISHED;
}

void IMAGE_OT_view_rotate_ccw(wmOperatorType *ot)
{
  ot->name = "Rotate View 90° Counter-Clockwise";
  ot->idname = "IMAGE_OT_view_rotate_ccw";
  ot->description = "Rotate the canvas view 90 degrees counter-clockwise";

  ot->exec = image_view_rotate_ccw_exec;
  ot->poll = space_image_rotation_poll;

  ot->flag = OPTYPE_REGISTER;
}

/* Reset Rotation */
static wmOperatorStatus image_view_rotate_reset_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);

  sima->rotation = 0.0f;

  ED_region_tag_redraw(region);
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

/* -------------------------------------------------------------------- */
/** \name Interactive Rotation Operator
 * \{ */

struct ImageRotateInteractiveData {
  float initial_rotation;
  float initial_pivot[2];
  float initial_offset[2];
  float current_pivot[2];
  float pivot_screen[2];
  Dial *dial;
  short init_event_type;
  wmPaintCursor *cursor;
  bool use_center_pivot;
};

/* Compensate the view offset (`sima->xof/yof`) so the displayed image does not jump when the
 * rotation pivot changes while rotation is non-zero.
 *
 * The canvas-rotation pipelines (DRW image engine, view2d overlays, Python POST_VIEW) all rotate
 * about the pivot's axis-aligned pixel position and leave that pixel fixed. So the canvas point that
 * equals the pivot is always displayed at its axis-aligned pixel position, regardless of rotation.
 * When the pivot changes from `initial_pivot` to `current_pivot`, the canvas point
 * `current_pivot` must stay where it was on screen before the change. Before the change it was drawn
 * (under rotation about `initial_pivot`) at `screen_old`; after the change it sits at
 * `current_pivot`'s axis-aligned pixel position. We pan the view by that pixel delta, converting to
 * `xof/yof` units via `sima->zoom` (see `image_main_region_set_view2d`: `dpixel = -zoom * dxof`). */
static void image_space_pivot_apply_offset_compensation(SpaceImage *sima,
                                                       ARegion *region,
                                                       const float initial_pivot[2],
                                                       const float current_pivot[2])
{
  if (sima->rotation == 0.0f) {
    return;
  }

  /* Where `current_pivot` is displayed right now, under the rotation about `initial_pivot`.
   * `ED_image_point_pos__reverse` is rotation-aware (it applies the canvas rotation about the
   * current pivot when mapping view->screen). The view2D still holds `initial_pivot`. */
  float screen_old[2];
  ED_image_point_pos__reverse(sima, region, current_pivot, screen_old);

  /* Axis-aligned pixel position of `current_pivot` — where it will sit once it becomes the pivot
   * (the pivot pixel is invariant under rotation). */
  float pivot_px_new[2];
  ui::view2d_view_to_region_navigation_fl(
      &region->v2d, current_pivot[0], current_pivot[1], &pivot_px_new[0], &pivot_px_new[1]);

  /* Pan so that `current_pivot` stays at `screen_old`: `pivot_px_new + (-zoom * dxof) = screen_old`,
   * i.e. `dxof = (pivot_px_new - screen_old) / zoom`. `zoom` is uniform on both axes here. */
  float zoomx, zoomy;
  ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);

  const float dxof = (pivot_px_new[0] - screen_old[0]) / zoomx;
  const float dyof = (pivot_px_new[1] - screen_old[1]) / zoomy;

  sima->xof += dxof;
  sima->yof += dyof;
}

static void image_view_rotate_interactive_update_header(wmOperator *op, bContext *C)
{
  ImageRotateInteractiveData *data = static_cast<ImageRotateInteractiveData *>(op->customdata);
  SpaceImage *sima = CTX_wm_space_image(C);
  ScrArea *area = CTX_wm_area(C);

  char msg[UI_MAX_DRAW_STR];
  SNPRINTF(msg, "Rotation: %.1f°", RAD2DEGF(sima->rotation));
  ED_area_status_text(area, msg);
}

static void image_rotate_draw_pivot(bContext *C,
                                    const int2 & /*xy*/,
                                    const float2 & /*tilt*/,
                                    void *customdata)
{
  ImageRotateInteractiveData *data = static_cast<ImageRotateInteractiveData *>(customdata);
  if (data == nullptr) {
    return;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  GPU_matrix_push_projection();
  GPU_matrix_push();
  wmViewport(&region->winrct);

  GPU_matrix_translate_2f(data->pivot_screen[0] - region->winrct.xmin,
                          data->pivot_screen[1] - region->winrct.ymin);

  GPU_line_width(2.0f);
  const float size = 36.0f;

  auto draw_axis_indicator = [&](const bool show_labels, const float scale_factor) {
    const float axis_size = 100.0f * scale_factor;
    const float axis_handle_size = 0.12f;
    const float rotation = sima->rotation;
    const float cos_r = cosf(rotation);
    const float sin_r = sinf(rotation);

    const float x_dir[2] = {cos_r, sin_r};
    const float y_dir[2] = {-sin_r, cos_r};

    const float axis_length = axis_size * (1.0f - axis_handle_size);
    const float rad = axis_size * axis_handle_size;
    const float circle_distance = axis_length + rad;

    const float x_end[2] = {x_dir[0] * axis_length, x_dir[1] * axis_length};
    const float y_end[2] = {y_dir[0] * axis_length, y_dir[1] * axis_length};
    const float x_end_circle[2] = {x_dir[0] * circle_distance, x_dir[1] * circle_distance};
    const float y_end_circle[2] = {y_dir[0] * circle_distance, y_dir[1] * circle_distance};

    GPU_blend(GPU_BLEND_ALPHA);
    GPU_line_smooth(true);

    GPUVertFormat *format = immVertexFormat();
    const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    const uint color_id = GPU_vertformat_attr_add(
        format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);

    float viewport_size[4];
    GPU_viewport_size_get_f(viewport_size);

    /* X axis (RED). */
    immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_SMOOTH_COLOR);
    immUniform2fv("viewportSize", &viewport_size[2]);
    immUniform1f("lineWidth", 2.0f * scale_factor);
    immUniform1i("lineSmooth", 1);
    const float x_center_color[4] = {0.9f, 0.2f, 0.2f, 0.6f};
    const float x_end_color[4] = {0.9f, 0.2f, 0.2f, 1.0f};
    immBegin(GPU_PRIM_LINES, 2);
    immAttr4fv(color_id, x_center_color);
    immVertex2f(pos, 0.0f, 0.0f);
    immAttr4fv(color_id, x_end_color);
    immVertex2f(pos, x_end[0], x_end[1]);
    immEnd();
    immUnbindProgram();

    /* Y axis (GREEN). */
    immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_SMOOTH_COLOR);
    immUniform2fv("viewportSize", &viewport_size[2]);
    immUniform1f("lineWidth", 2.0f * scale_factor);
    immUniform1i("lineSmooth", 1);
    const float y_center_color[4] = {0.2f, 0.9f, 0.2f, 0.6f};
    const float y_end_color[4] = {0.2f, 0.9f, 0.2f, 1.0f};
    immBegin(GPU_PRIM_LINES, 2);
    immAttr4fv(color_id, y_center_color);
    immVertex2f(pos, 0.0f, 0.0f);
    immAttr4fv(color_id, y_end_color);
    immVertex2f(pos, y_end[0], y_end[1]);
    immEnd();
    immUnbindProgram();

    /* Crosshair. */
    const float cross_size = 6.0f * scale_factor;
    const uint pos_outer = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor3f(1.0f, 1.0f, 1.0f);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex2f(pos_outer, -cross_size * x_dir[0], -cross_size * x_dir[1]);
    immVertex2f(pos_outer, cross_size * x_dir[0], cross_size * x_dir[1]);
    immVertex2f(pos_outer, -cross_size * y_dir[0], -cross_size * y_dir[1]);
    immVertex2f(pos_outer, cross_size * y_dir[0], cross_size * y_dir[1]);
    immEnd();
    immUnbindProgram();

    GPU_line_smooth(false);

    /* Axis end circles (same look as patch). */
    const float axis_ring_width = 1.5f * scale_factor;
    const float x_color[3] = {0.9f, 0.2f, 0.2f};
    const float y_color[3] = {0.2f, 0.9f, 0.2f};

    auto draw_axis_circle = [&](const float center[2], const float color[3]) {
      ui::draw_roundbox_corner_set(ui::CNR_ALL);
      GPU_matrix_push();
      GPU_matrix_translate_2fv(center);

      rctf rect{};
      rect.xmin = -rad;
      rect.xmax = rad;
      rect.ymin = -rad;
      rect.ymax = rad;

      float center_color[4] = {color[0], color[1], color[2], 1.0f};
      float outline_color[4] = {color[0], color[1], color[2], 1.0f};

      ui::draw_roundbox_4fv(&rect, true, rad, center_color);
      ui::draw_roundbox_4fv_ex(
          &rect, center_color, nullptr, 0.0f, outline_color, axis_ring_width, rad);

      GPU_matrix_pop();
    };

    draw_axis_circle(x_end_circle, x_color);
    draw_axis_circle(y_end_circle, y_color);

    if (show_labels) {
      const int font_id = BLF_default();
      BLF_enable(font_id, BLF_BOLD);
      BLF_size(font_id, int(14.0f * scale_factor));
      const struct {
        const float *v;
        const char *label;
      } labels[] = {
          {x_end_circle, "+X"},
          {y_end_circle, "+Y"},
      };
      for (const auto &label_data : labels) {
        float text_w = 0.0f, text_h = 0.0f;
        BLF_width_and_height(
            font_id, label_data.label, strlen(label_data.label), &text_w, &text_h);
        BLF_position(
            font_id, label_data.v[0] - text_w * 0.5f, label_data.v[1] - text_h * 0.5f, 0);
        const float text_col[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        BLF_color4fv(font_id, text_col);
        BLF_draw(font_id, label_data.label, strlen(label_data.label));
      }
      BLF_disable(font_id, BLF_BOLD);
    }

    GPU_blend(GPU_BLEND_NONE);
  };

  if (data->use_center_pivot) {
    /* Patch parity: center-pivot mode draws axis indicator with labels. */
    draw_axis_indicator(true, 1.0f);
  }
  else {
    /* Patch parity: cursor-pivot mode draws arc+arrow in blue, plus axis indicator with labels. */
    GPU_blend(GPU_BLEND_ALPHA);
    GPU_line_smooth(true);

    const float blue_color[3] = {71.0f / 255.0f, 114.0f / 255.0f, 179.0f / 255.0f};

    const uint pos_blue = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor3fv(blue_color);

    const float cross_size = 15.0f;
    immBegin(GPU_PRIM_LINES, 4);
    immVertex2f(pos_blue, -cross_size, 0.0f);
    immVertex2f(pos_blue, cross_size, 0.0f);
    immVertex2f(pos_blue, 0.0f, -cross_size);
    immVertex2f(pos_blue, 0.0f, cross_size);
    immEnd();
    immUnbindProgram();

    immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
    immUniformColor3fv(blue_color);
    float viewport[4];
    GPU_viewport_size_get_f(viewport);
    immUniform2fv("viewportSize", &viewport[2]);
    immUniform1f("lineWidth", 2.0f * UI_SCALE_FAC);
    immUniform1i("lineSmooth", 1);

    const int arc_segments = 24;
    const float start_angle = -0.75f * float(M_PI);
    const float end_angle = 0.75f * float(M_PI);
    const float angle_step = (end_angle - start_angle) / arc_segments;
    immBegin(GPU_PRIM_LINE_STRIP, arc_segments + 1);
    for (int i = 0; i <= arc_segments; i++) {
      const float angle = start_angle + float(i) * angle_step;
      immVertex2f(pos_blue, cosf(angle) * size, sinf(angle) * size);
    }
    immEnd();
    immUnbindProgram();

    const uint pos_arrow = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor3fv(blue_color);
    GPU_line_width(2.0f);

    const float arrow_size = 12.0f;
    const float back_angle = end_angle - float(M_PI_2);
    const float arrow_x = cosf(end_angle) * size;
    const float arrow_y = sinf(end_angle) * size;
    const float half_arrow_angle = 0.4f;
    immBegin(GPU_PRIM_LINES, 4);
    immVertex2f(pos_arrow, arrow_x, arrow_y);
    immVertex2f(pos_arrow,
                arrow_x + cosf(back_angle - half_arrow_angle) * arrow_size,
                arrow_y + sinf(back_angle - half_arrow_angle) * arrow_size);
    immVertex2f(pos_arrow, arrow_x, arrow_y);
    immVertex2f(pos_arrow,
                arrow_x + cosf(back_angle + half_arrow_angle) * arrow_size,
                arrow_y + sinf(back_angle + half_arrow_angle) * arrow_size);
    immEnd();
    immUnbindProgram();

    GPU_line_smooth(false);
    GPU_blend(GPU_BLEND_NONE);

    draw_axis_indicator(true, 1.0f);
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor3f(0.2f, 0.6f, 0.9f);

  const int arc_segments = 24;
  const float start_angle = -0.75f * M_PI;
  const float end_angle = 0.75f * M_PI;
  const float angle_step = (end_angle - start_angle) / arc_segments;

  immBegin(GPU_PRIM_LINE_STRIP, arc_segments + 1);
  for (int i = 0; i <= arc_segments; i++) {
    float angle = start_angle + float(i) * angle_step;
    immVertex2f(pos, cosf(angle) * size, sinf(angle) * size);
  }
  immEnd();

  immUnbindProgram();
  GPU_line_smooth(false);
  GPU_blend(GPU_BLEND_NONE);

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

  data->use_center_pivot = RNA_boolean_get(op->ptr, "use_center_pivot");

  if (data->use_center_pivot) {
    data->initial_pivot[0] = sima->rotation_pivot[0];
    data->initial_pivot[1] = sima->rotation_pivot[1];
    data->current_pivot[0] = 0.5f;
    data->current_pivot[1] = 0.5f;

    /* Compensate view offset when switching pivot while rotation is active. */
    image_space_pivot_apply_offset_compensation(
        sima, region, data->initial_pivot, data->current_pivot);

    sima->rotation_pivot[0] = data->current_pivot[0];
    sima->rotation_pivot[1] = data->current_pivot[1];

    float pivot_uv[2] = {0.5f, 0.5f};
    float pivot_region[2];
    ED_image_point_pos__reverse(sima, region, pivot_uv, pivot_region);
    data->pivot_screen[0] = float(region->winrct.xmin + int(pivot_region[0]));
    data->pivot_screen[1] = float(region->winrct.ymin + int(pivot_region[1]));

    /* Ensure the indicator is visible immediately when invoked from the gizmo button.
     * The rotate gizmo starts a modal operator, but the draw callback won't run until the next
     * redraw unless we tag now. */
    ED_region_tag_redraw(region);
    ED_region_tag_redraw_editor_overlays(region);
  }
  else {
    ED_image_mouse_pos(sima, region, event->mval, data->current_pivot);
    data->initial_pivot[0] = sima->rotation_pivot[0];
    data->initial_pivot[1] = sima->rotation_pivot[1];

    /* Snap pivot to center/corners using screen-space distance (patch behavior). */
    {
      const float SNAP_DISTANCE_PX = 12.0f;
      const float CENTER_DISTANCE_PX = SNAP_DISTANCE_PX * 0.8f;

      const float cursor_pos[2] = {float(event->mval[0]), float(event->mval[1])};

      float snap_pivot[2] = {data->current_pivot[0], data->current_pivot[1]};
      bool snapped = false;
      float min_dist = SNAP_DISTANCE_PX;

      auto uv_to_screen = [&](const float u, const float v, float r_out[2]) {
        float uv_pos[2] = {u, v};
        ED_image_point_pos__reverse(sima, region, uv_pos, r_out);
      };

      {
        float center_screen[2];
        uv_to_screen(0.5f, 0.5f, center_screen);
        const float dist = len_v2v2(center_screen, cursor_pos);
        if (dist < CENTER_DISTANCE_PX) {
          snap_pivot[0] = 0.5f;
          snap_pivot[1] = 0.5f;
          snapped = true;
          min_dist = dist;
        }
      }

      if (!snapped) {
        const struct {
          float x, y;
        } corners[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
        for (const auto &corner : corners) {
          float corner_screen[2];
          uv_to_screen(corner.x, corner.y, corner_screen);
          const float dist = len_v2v2(corner_screen, cursor_pos);
          if (dist < SNAP_DISTANCE_PX && dist < min_dist) {
            snap_pivot[0] = corner.x;
            snap_pivot[1] = corner.y;
            snapped = true;
            min_dist = dist;
          }
        }
      }

      if (snapped) {
        data->current_pivot[0] = snap_pivot[0];
        data->current_pivot[1] = snap_pivot[1];
      }
    }

    /* Compensate view offset when changing pivot while rotation is active. */
    image_space_pivot_apply_offset_compensation(
        sima, region, data->initial_pivot, data->current_pivot);

    sima->rotation_pivot[0] = data->current_pivot[0];
    sima->rotation_pivot[1] = data->current_pivot[1];

    /* Patch behavior: draw pivot indicator exactly at click position in cursor mode. */
    data->pivot_screen[0] = float(region->winrct.xmin + event->mval[0]);
    data->pivot_screen[1] = float(region->winrct.ymin + event->mval[1]);
  }

  float cursor_screen[2] = {float(event->xy[0]), float(event->xy[1])};
  data->dial = BLI_dial_init(cursor_screen, FLT_EPSILON);

  data->cursor = WM_paint_cursor_activate(
      SPACE_IMAGE, RGN_TYPE_WINDOW, nullptr, image_rotate_draw_pivot, data);

  /* Ensure immediate draw on invoke for both pivot modes (avoids 1-frame delay). */
  ED_region_tag_redraw(region);
  ED_region_tag_redraw_editor_overlays(region);

  wmWindow *win = CTX_wm_window(C);
  if (WM_cursor_modal_is_set_ok(win)) {
    WM_cursor_modal_set(win, WM_CURSOR_NSEW_SCROLL);
  }

  op->customdata = data;

  image_view_rotate_interactive_update_header(op, C);

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
    SpaceImage *sima = CTX_wm_space_image(C);
    sima->rotation = data->initial_rotation;
    copy_v2_v2(sima->rotation_pivot, data->initial_pivot);
    sima->xof = data->initial_offset[0];
    sima->yof = data->initial_offset[1];
    ED_region_tag_redraw(CTX_wm_region(C));
  }

  WM_cursor_modal_restore(CTX_wm_window(C));
  ED_area_status_text(CTX_wm_area(C), nullptr);
  if (data->cursor) {
    WM_paint_cursor_end(data->cursor);
  }
  if (data->dial) {
    BLI_dial_free(data->dial);
  }
  MEM_delete(data);
  op->customdata = nullptr;
}

static wmOperatorStatus image_view_rotate_interactive_modal(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  ImageRotateInteractiveData *data = static_cast<ImageRotateInteractiveData *>(op->customdata);
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);

  switch (event->type) {
    case MOUSEMOVE: {
      float current_position[2] = {float(event->xy[0]), float(event->xy[1])};
      float angle = BLI_dial_angle(data->dial, current_position);

      float target_rotation = data->initial_rotation + angle;

      if (event->modifier & KM_CTRL) {
        float snap_angle = DEG2RADF(15.0f);
        target_rotation = roundf(target_rotation / snap_angle) * snap_angle;
      }

      sima->rotation = target_rotation;
      normalize_rotation_angle(sima->rotation);

      image_view_rotate_interactive_update_header(op, C);
      ED_region_tag_redraw(region);
      break;
    }
    default:
      break;
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
