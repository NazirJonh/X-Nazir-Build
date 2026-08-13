/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <algorithm>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

#include "eyedropper_intern.hh" /* own include */

namespace blender::ui {

/* -------------------------------------------------------------------- */
/* Keymap
 */
/** \name Modal Keymap
 * \{ */

wmKeyMap *eyedropper_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {EYE_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {EYE_MODAL_SAMPLE_CONFIRM, "SAMPLE_CONFIRM", 0, "Confirm Sampling", ""},
      {EYE_MODAL_SAMPLE_BEGIN, "SAMPLE_BEGIN", 0, "Start Sampling", ""},
      {EYE_MODAL_SAMPLE_RESET, "SAMPLE_RESET", 0, "Reset Sampling", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Eyedropper Modal Map");

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return nullptr;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Eyedropper Modal Map", modal_items);

  /* assign to operators */
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_colorramp");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_color");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_id");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_bone");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_depth");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_driver");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_grease_pencil_color");

  return keymap;
}

wmKeyMap *eyedropper_colorband_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items_point[] = {
      {EYE_MODAL_POINT_CANCEL, "CANCEL", 0, "Cancel", ""},
      {EYE_MODAL_POINT_SAMPLE, "SAMPLE_SAMPLE", 0, "Sample a Point", ""},
      {EYE_MODAL_POINT_CONFIRM, "SAMPLE_CONFIRM", 0, "Confirm Sampling", ""},
      {EYE_MODAL_POINT_RESET, "SAMPLE_RESET", 0, "Reset Sampling", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Eyedropper ColorRamp PointSampling Map");
  if (keymap && keymap->modal_items) {
    return keymap;
  }

  keymap = WM_modalkeymap_ensure(
      keyconf, "Eyedropper ColorRamp PointSampling Map", modal_items_point);

  /* assign to operators */
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_colorramp_point");

  return keymap;
}

/** \} */

/* -------------------------------------------------------------------- */
/* Utility Functions
 */

/** \name Generic Shared Functions
 * \{ */

void eyedropper_draw_cursor_text_region(const int xy[2], const char *name)
{
  if (name[0] == '\0') {
    return;
  }

  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;

  /* Use the theme settings from tooltips. */
  const bTheme *btheme = theme::theme_get();
  const uiWidgetColors *wcol = &btheme->tui.wcol_tooltip;

  float col_fg[4], col_bg[4];
  rgba_uchar_to_float(col_fg, wcol->text);
  rgba_uchar_to_float(col_bg, wcol->inner);

  fontstyle_draw_simple_backdrop(fstyle, xy[0], xy[1] + U.widget_unit, name, col_fg, col_bg);
}

/**
 * Draw a color swatch next to the cursor, as a preview of the color that would be sampled.
 *
 * \param window: the window the preview is drawn in, its draw callback runs in full window space.
 * \param xy: cursor position in the window's pixel space. May lie outside of the window when the
 * cursor is over the window decorations or over another application, in which case the preview is
 * pinned to the edge of the window closest to the cursor.
 */
void eyedropper_draw_cursor_color_window(const wmWindow *window,
                                         const int xy[2],
                                         const float color[3])
{
  const int2 window_size = WM_window_native_pixel_size(window);

  const float radius = U.widget_unit * 1.1f;
  const float border_width = 2.5f * UI_SCALE_FAC;
  const float shadow_offset = 2.0f * UI_SCALE_FAC;
  /* Distance between the cursor and the center of the preview. */
  const float cursor_offset = 30.0f * UI_SCALE_FAC;

  const float half_size = radius + border_width + shadow_offset;
  const float min_x = half_size + border_width;
  const float max_x = float(window_size.x) - half_size - border_width;
  const float min_y = half_size + border_width;
  const float max_y = float(window_size.y) - half_size - border_width;
  if ((min_x > max_x) || (min_y > max_y)) {
    /* The window is too small to fit the preview. */
    return;
  }

  /* Offset to the bottom right of the cursor, flipping over when there is not enough room.
   * Clamping afterwards also pins the preview to the closest edge for a cursor outside of the
   * window, where the coordinates are negative or exceed the window size. */
  float center_x = float(xy[0]) + cursor_offset;
  float center_y = float(xy[1]) - cursor_offset;
  if (center_x + half_size > float(window_size.x)) {
    center_x = float(xy[0]) - cursor_offset;
  }
  if (center_y - half_size < 0.0f) {
    center_y = float(xy[1]) + cursor_offset;
  }
  center_x = std::clamp(center_x, min_x, max_x);
  center_y = std::clamp(center_y, min_y, max_y);

  /* The round-box corner state is left over from the last widget that was drawn, so it has to be
   * set explicitly, otherwise only some of the corners end up rounded. */
  draw_roundbox_corner_set(CNR_ALL);

  const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.3f};
  const float border_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  /* Sampled colors are not guaranteed to be display referred, clamp so the swatch stays valid. */
  const float inner_color[4] = {std::clamp(color[0], 0.0f, 1.0f),
                                std::clamp(color[1], 0.0f, 1.0f),
                                std::clamp(color[2], 0.0f, 1.0f),
                                1.0f};

  rctf rect;

  BLI_rctf_init(&rect,
                center_x - radius + shadow_offset,
                center_x + radius + shadow_offset,
                center_y - radius - shadow_offset,
                center_y + radius - shadow_offset);
  draw_roundbox_4fv(&rect, true, radius, shadow_color);

  BLI_rctf_init(&rect,
                center_x - radius - border_width,
                center_x + radius + border_width,
                center_y - radius - border_width,
                center_y + radius + border_width);
  draw_roundbox_4fv(&rect, true, radius + border_width, border_color);

  BLI_rctf_init(&rect, center_x - radius, center_x + radius, center_y - radius, center_y + radius);
  draw_roundbox_4fv(&rect, true, radius, inner_color);
}

Button *eyedropper_get_property_button_under_mouse(bContext *C, const wmEvent *event)
{
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event->xy);
  const ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_ANY, event->xy);

  Button *but = but_find_mouse_over(region, event);

  if (ELEM(nullptr, but, but->rnapoin.data, but->rnaprop)) {
    return nullptr;
  }
  return but;
}

void eyedropper_win_area_find(const bContext *C,
                              const int event_xy[2],
                              int r_event_xy[2],
                              wmWindow **r_win,
                              ScrArea **r_area)
{
  bScreen *screen = CTX_wm_screen(C);

  *r_win = CTX_wm_window(C);
  *r_area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event_xy);
  if (*r_area == nullptr) {
    *r_win = WM_window_find_under_cursor(*r_win, event_xy, r_event_xy);
    if (*r_win) {
      screen = WM_window_get_active_screen(*r_win);
      *r_area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, r_event_xy);
    }
  }
  else if (event_xy != r_event_xy) {
    copy_v2_v2_int(r_event_xy, event_xy);
  }
}

/** \} */

}  // namespace blender::ui
