/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include <cmath>

#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_window.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"

#include "GHOST_IWindow.hh"
#include "GHOST_Types.hh"

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

void eyedropper_draw_cursor_color_region(const wmWindow *window, const int xy[2], const float color[3])
{
  if (!window || !window->runtime || !window->runtime->ghostwin) {
    return;
  }

  /* Проверяем, что окно не минимизировано (избегаем проблем с GPU драйверами) */
#ifdef WIN32
  GHOST_IWindow *ghost_window = static_cast<GHOST_IWindow *>(window->runtime->ghostwin);
  if (ghost_window && ghost_window->getState() == GHOST_kWindowStateMinimized) {
    return;
  }
#endif

  /* Проверяем, что GPU контекст доступен */
  if (!window->runtime->gpuctx) {
    return;
  }

  /* Размеры элементов превью */
  const float radius = U.widget_unit * 1.1f;
  const float border_width = 2.5f;
  const float shadow_offset = 2.0f;
  const float offset = 30.0f; /* Отступ от курсора */
  
  /* Получаем размеры окна для проверки границ */
  rcti window_rect;
  WM_window_rect_calc(window, &window_rect);
  const int window_width = BLI_rcti_size_x(&window_rect);
  const int window_height = BLI_rcti_size_y(&window_rect);
  
  /* Проверяем валидность размеров окна */
  if (window_width <= 0 || window_height <= 0) {
    return;
  }
  
  /* Координаты xy находятся в pixel space окна (относительные координаты внутри окна).
   * WM_window_rect_calc возвращает прямоугольник от (0, 0) до (width, height),
   * где (0, 0) - левый нижний угол клиентской области окна (без заголовка).
   * Когда курсор вне окна, координаты могут быть отрицательными или выходить за пределы. */
  const int original_cursor_x = xy[0];
  const int original_cursor_y = xy[1];

  /* Проверяем валидность координат (избегаем слишком больших значений) */
  if (original_cursor_x < -10000 || original_cursor_x > 100000 ||
      original_cursor_y < -10000 || original_cursor_y > 100000) {
    return;
  }

  /* Размер превью с учетом отступов */
  const float preview_size = (radius + border_width + shadow_offset) * 2.0f;
  const float half_size = preview_size / 2.0f;

  /* Границы для позиционирования превью внутри окна */
  const float min_x = half_size + border_width;
  const float max_x = float(window_width) - half_size - border_width;
  const float min_y = half_size + border_width;
  const float max_y = float(window_height) - half_size - border_width;

  /* Определяем, находится ли курсор вне окна */
  const bool cursor_outside_left = (original_cursor_x < 0);
  const bool cursor_outside_right = (original_cursor_x > window_width);
  const bool cursor_outside_top = (original_cursor_y < 0);      /* Y < 0 = выше окна */
  const bool cursor_outside_bottom = (original_cursor_y > window_height); /* Y > height = ниже окна */
  const bool cursor_outside = cursor_outside_left || cursor_outside_right ||
                               cursor_outside_top || cursor_outside_bottom;

  float center_x, center_y;

  if (cursor_outside) {
    /* Курсор вне окна - размещаем превью на краю окна в направлении курсора */

    if (cursor_outside_right) {
      /* Курсор справа от окна - превью на правом краю */
      float clamped_y = std::max(min_y, std::min(max_y, float(original_cursor_y)));
      center_x = max_x;
      center_y = clamped_y;
    }
    else if (cursor_outside_left) {
      /* Курсор слева от окна - превью на левом краю */
      float clamped_y = std::max(min_y, std::min(max_y, float(original_cursor_y)));
      center_x = min_x;
      center_y = clamped_y;
    }
    else if (cursor_outside_top) {
      /* Курсор выше окна (Y < 0) - превью на верхнем краю (Y ≈ 0) */
      float clamped_x = std::max(min_x, std::min(max_x, float(original_cursor_x)));
      center_x = clamped_x;
      center_y = min_y;
    }
    else if (cursor_outside_bottom) {
      /* Курсор ниже окна (Y > height) - превью на нижнем краю */
      float clamped_x = std::max(min_x, std::min(max_x, float(original_cursor_x)));
      center_x = clamped_x;
      center_y = max_y;
    }
    else {
      /* Fallback - не должно происходить */
      center_x = float(window_width) / 2.0f;
      center_y = float(window_height) / 2.0f;
    }
  }
  else {
    /* Курсор внутри окна - обычное позиционирование */
    int cursor_x = original_cursor_x;
    int cursor_y = original_cursor_y;

    /* Позиционируем превью справа и ниже курсора по умолчанию */
    center_x = float(cursor_x) + offset;
    center_y = float(cursor_y) - offset;

    /* Проверяем, помещается ли превью справа от курсора */
    if (center_x + preview_size / 2.0f > float(window_width)) {
      /* Не помещается справа - размещаем слева */
      center_x = float(cursor_x) - offset;
    }

    /* Проверяем, помещается ли превью ниже курсора */
    if (center_y - preview_size / 2.0f < 0.0f) {
      /* Не помещается ниже - размещаем выше */
      center_y = float(cursor_y) + offset;
    }

    /* Ограничиваем координаты границами окна */
    center_x = std::max(min_x, std::min(max_x, center_x));
    center_y = std::max(min_y, std::min(max_y, center_y));
  }

  /* Проверяем валидность финальных координат (избегаем NaN/Inf) */
  if (!std::isfinite(center_x) || !std::isfinite(center_y) || 
      !std::isfinite(radius) || radius <= 0.0f) {
    return;
  }
  
  /* Проверяем, что превью не выходит за границы окна после всех корректировок */
  if (center_x - half_size < 0.0f || center_x + half_size > float(window_width) ||
      center_y - half_size < 0.0f || center_y + half_size > float(window_height)) {
    return;
  }
  
  /* Дополнительные проверки для предотвращения сбоя GPU драйвера */
  if (radius <= 0.0f || radius > 1000.0f) {
    return;
  }
  if (half_size <= 0.0f || half_size > float(window_width) || half_size > float(window_height)) {
    return;
  }
  
  /* Проверяем валидность цветов (должны быть в диапазоне [0, 1]) */
  if (!std::isfinite(color[0]) || !std::isfinite(color[1]) || !std::isfinite(color[2])) {
    return;
  }
  
  /* Цвета для отрисовки */
  float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.3f};
  float border_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float color_rgba[4] = {
    std::max(0.0f, std::min(1.0f, color[0])),
    std::max(0.0f, std::min(1.0f, color[1])),
    std::max(0.0f, std::min(1.0f, color[2])),
    1.0f
  };
  
  /* Вычисляем прямоугольники и проверяем их валидность */
  const float shadow_xmin = center_x - radius + shadow_offset;
  const float shadow_xmax = center_x + radius + shadow_offset;
  const float shadow_ymin = center_y - radius - shadow_offset;
  const float shadow_ymax = center_y + radius - shadow_offset;
  
  if (shadow_xmin >= shadow_xmax || shadow_ymin >= shadow_ymax ||
      !std::isfinite(shadow_xmin) || !std::isfinite(shadow_xmax) ||
      !std::isfinite(shadow_ymin) || !std::isfinite(shadow_ymax)) {
    return;
  }
  
  const rctf shadow_rect = {shadow_xmin, shadow_xmax, shadow_ymin, shadow_ymax};
  draw_roundbox_4fv(&shadow_rect, true, radius, shadow_color);
  
  /* Рисуем белый контур */
  const float border_xmin = center_x - radius - border_width;
  const float border_xmax = center_x + radius + border_width;
  const float border_ymin = center_y - radius - border_width;
  const float border_ymax = center_y + radius + border_width;
  
  if (border_xmin >= border_xmax || border_ymin >= border_ymax ||
      !std::isfinite(border_xmin) || !std::isfinite(border_xmax) ||
      !std::isfinite(border_ymin) || !std::isfinite(border_ymax)) {
    return;
  }
  
  const rctf border_rect = {border_xmin, border_xmax, border_ymin, border_ymax};
  draw_roundbox_4fv(&border_rect, true, radius + border_width, border_color);
  
  /* Рисуем цветной круг внутри */
  const float color_xmin = center_x - radius;
  const float color_xmax = center_x + radius;
  const float color_ymin = center_y - radius;
  const float color_ymax = center_y + radius;
  
  if (color_xmin >= color_xmax || color_ymin >= color_ymax ||
      !std::isfinite(color_xmin) || !std::isfinite(color_xmax) ||
      !std::isfinite(color_ymin) || !std::isfinite(color_ymax)) {
    return;
  }
  
  const rctf color_rect = {color_xmin, color_xmax, color_ymin, color_ymax};
  draw_roundbox_4fv(&color_rect, true, radius, color_rgba);
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
