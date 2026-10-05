/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 * Sculpt Cursor Gizmo - Viewport button row of the sculpt cursor
 */

#include "MEM_guardedalloc.h"

#include <cstring>

#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_screen.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_theme_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "ED_gizmo_library.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"

#include "view3d_gizmo_sculpt_cursor_intern.hh"

namespace blender::ed::view3d {

/* -------------------------------------------------------------------- */
/** \name Sculpt Cursor Viewport Buttons
 *
 * The button row above the gizmo is defined by #UserDef.sculpt_cursor_buttons: the built-in
 * entries (mode, pin, shared cursor) and the user-defined ones are drawn the same way, in list
 * order. Every button is a plain `GIZMO_GT_button_2d` widget, which is designed for screen-space
 * (non-3D) gizmo groups: its hit-test compares the region mouse position directly against
 * `matrix_basis[3]` (see #gizmo_button2d_test_select) and its scale is a plain UI-pixel value (the
 * #WM_GIZMOGROUPTYPE_SCALE path of #wm_gizmo_calculate_scale). Putting them in the 3D cursor group
 * would both mis-size them and break clicking, so they live in their own group whose position is
 * the cursor's projected screen coordinate.
 * \{ */

/** What one state (inactive or active) of a button draws, resolved from its icon source. */
struct SculptCursorButtonIconState {
  /** #eSculptCursorButtonIconSource. A glyph or file source without data resolves to
   * #SCULPT_CURSOR_BUTTON_ICON_SOURCE_BLENDER_ICON. */
  int source = SCULPT_CURSOR_BUTTON_ICON_SOURCE_BLENDER_ICON;
  int icon = 0;
  char glyph[8] = "";
  char path[1024] = "";

  /** Only the data the source uses is compared. */
  friend bool operator==(const SculptCursorButtonIconState &a,
                         const SculptCursorButtonIconState &b)
  {
    if (a.source != b.source) {
      return false;
    }
    switch (a.source) {
      case SCULPT_CURSOR_BUTTON_ICON_SOURCE_GLYPH:
        return STREQ(a.glyph, b.glyph);
      case SCULPT_CURSOR_BUTTON_ICON_SOURCE_CUSTOM_FILE:
        return STREQ(a.path, b.path);
      default:
        return a.icon == b.icon;
    }
  }
};

static SculptCursorButtonIconState sculpt_cursor_button_icon_state_get(
    const int source, const int icon, const char *glyph, const char *path)
{
  SculptCursorButtonIconState state;
  state.icon = icon;
  /* An empty glyph also covers preferences saved before the icon source existed. */
  if (source == SCULPT_CURSOR_BUTTON_ICON_SOURCE_GLYPH && glyph[0] != '\0') {
    state.source = source;
    STRNCPY(state.glyph, glyph);
  }
  else if (source == SCULPT_CURSOR_BUTTON_ICON_SOURCE_CUSTOM_FILE && path[0] != '\0') {
    state.source = source;
    STRNCPY(state.path, path);
  }
  return state;
}

/** Resolve both states of \a btn. A button drawing the same thing in either state is
 * single-icon, and then \a r_on equals \a r_off. */
static bool sculpt_cursor_button_icon_states_get(const SculptCursorButton &btn,
                                                 SculptCursorButtonIconState &r_off,
                                                 SculptCursorButtonIconState &r_on)
{
  r_off = sculpt_cursor_button_icon_state_get(btn.icon_source, btn.icon, btn.glyph, btn.icon_path);
  if (btn.flag & SCULPT_CURSOR_BUTTON_USE_SINGLE_ICON) {
    r_on = r_off;
    return true;
  }
  r_on = sculpt_cursor_button_icon_state_get(
      btn.icon_active_source, btn.icon_active, btn.glyph_active, btn.icon_active_path);
  if (r_on == r_off) {
    r_on = r_off;
    return true;
  }
  return false;
}

/** Gizmos of one button entry (two variants for active/inactive icon states). */
struct SculptCursorButtonGizmos {
  /** Drawn when the button is not active (the only one in single-icon mode). */
  wmGizmo *off = nullptr;
  /** Drawn when the button is active; null in single-icon mode. */
  wmGizmo *on = nullptr;
  /** Decorative outline drawn over the button (not selectable), created with the entry. */
  wmGizmo *outline = nullptr;
  /** Snapshot to detect icon changes (the button gizmo caches its icon on the first draw). */
  SculptCursorButtonIconState state_off;
  SculptCursorButtonIconState state_on;
  bool single_icon = true;
};

struct SculptCursorButtonsGizmoGroup {
  /** Gizmos of the buttons of #UserDef.sculpt_cursor_buttons, built-in and user-defined alike,
   * keyed by #SculptCursorButton.unique_id. Owned by the group and rebuilt in refresh (the button
   * gizmo caches its icon on the first draw, so icon changes re-create it). */
  Map<int, SculptCursorButtonGizmos> buttons;
};

static bool sculpt_cursor_buttons_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  const Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  /* Like the gizmo handles, the buttons are usable from any tool while the cursor is on. */
  if (!ob || !ob->runtime->sculpt_session || !scene || !sculpt_paint::cursor::is_enabled(*scene))
  {
    return false;
  }

  /* Hidden together with the "3D Cursor" viewport overlay toggle. */
  const View3D *v3d = CTX_wm_view3d(C);
  if (!v3d || (v3d->flag2 & V3D_HIDE_OVERLAYS) ||
      (v3d->overlay.flag & V3D_OVERLAY_HIDE_CURSOR))
  {
    return false;
  }

  return true;
}

/** Visual setup of a screen-space button: a round chip with \a icon on it, in the panel theme
 * colors so it reads like a regular UI panel chip. */
static wmGizmo *sculpt_cursor_screen_button_new(const wmGizmoType *gzt,
                                                wmGizmoGroup *gzgroup,
                                                const int icon)
{
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  /* Screen-space group: `scale_final` is `scale_basis * UI_SCALE_FAC`, i.e. UI pixels. */
  gz->scale_basis = 14.0f;
  gz->flag |= WM_GIZMO_DRAW_OFFSET_SCALE;
  /* -1 leaves the icon unset: the button then draws its `icon_value` or its `text` glyph. */
  if (icon != -1) {
    RNA_enum_set(gz->ptr, "icon", icon);
  }
  RNA_enum_set(gz->ptr, "draw_options",
               ED_GIZMO_BUTTON_SHOW_OUTLINE | ED_GIZMO_BUTTON_SHOW_BACKDROP);
  RNA_boolean_set(gz->ptr, "show_drag", false);
  /* Each button draws its own round backdrop; use the panel theme colors so it reads like a
   * regular UI panel chip. */
  ui::theme::get_color_4fv(TH_PANEL_BACK, gz->color);
  ui::theme::get_color_4fv(TH_PANEL_HEADER, gz->color_hi);
  return gz;
}

/**
 * A button gizmo running the #SCULPT_OT_cursor_button_exec proxy, which looks the button up in
 * #UserDef.sculpt_cursor_buttons by its stable \a unique_id and invokes the operator stored on it
 * (the built-in buttons drive `wm.context_toggle` / `wm.context_cycle_enum` this way, with the
 * data path in their properties). The button's IDProperty group must never be attached to the
 * gizmo directly: the gizmo takes ownership of the properties passed to #WM_gizmo_operator_set and
 * would free the DNA data.
 */
static wmGizmo *sculpt_cursor_button_gizmo_new(const wmGizmoType *gzt,
                                               wmGizmoGroup *gzgroup,
                                               wmOperatorType *ot_exec,
                                               const int unique_id,
                                               const int icon,
                                               const int icon_value,
                                               const char *text)
{
  wmGizmo *gz = sculpt_cursor_screen_button_new(gzt, gzgroup, icon);
  if (icon_value != 0) {
    /* A dynamic icon id (a resolved custom image); the button gizmo defers its load. */
    RNA_int_set(gz->ptr, "icon_value", icon_value);
  }
  if (text != nullptr && text[0] != '\0') {
    RNA_string_set(gz->ptr, "text", text);
  }
  PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_exec, nullptr);
  RNA_int_set(ptr, "unique_id", unique_id);
  return gz;
}

static wmGizmo *sculpt_cursor_button_gizmo_new_for_state(const wmGizmoType *gzt,
                                                         wmGizmoGroup *gzgroup,
                                                         wmOperatorType *ot_exec,
                                                         const int unique_id,
                                                         const SculptCursorButtonIconState &state)
{
  switch (state.source) {
    case SCULPT_CURSOR_BUTTON_ICON_SOURCE_GLYPH:
      return sculpt_cursor_button_gizmo_new(gzt, gzgroup, ot_exec, unique_id, -1, 0, state.glyph);
    case SCULPT_CURSOR_BUTTON_ICON_SOURCE_CUSTOM_FILE: {
      /* Resolving reads the image file (cached by the preview system), so it belongs on the
       * refresh path, not on redraw. A missing file draws the placeholder icon instead of
       * nothing. */
      const int icon_id = ui::category_tab_icon_id_resolve_from_path(state.path);
      return sculpt_cursor_button_gizmo_new(gzt,
                                            gzgroup,
                                            ot_exec,
                                            unique_id,
                                            -1,
                                            (icon_id != ICON_NONE) ? icon_id : ICON_IMAGE_DATA,
                                            nullptr);
    }
    default:
      return sculpt_cursor_button_gizmo_new(
          gzt, gzgroup, ot_exec, unique_id, state.icon, 0, nullptr);
  }
}

/**
 * Decorative circle outline drawn over a button. `GIZMO_GT_button_2d` draws its own outline in the
 * same color as the fill (so it is invisible); this separate, non-selectable layer adds a readable
 * border in a contrasting theme color.
 */
static wmGizmo *sculpt_cursor_screen_outline_new(const wmGizmoType *gzt, wmGizmoGroup *gzgroup)
{
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  gz->scale_basis = 14.0f;
  gz->flag |= WM_GIZMO_DRAW_OFFSET_SCALE | WM_GIZMO_HIDDEN_SELECT;
  gz->line_width = 1.5f;
  RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_BACKDROP);
  RNA_float_set(gz->ptr, "backdrop_fill_alpha", 0.0f);
  ui::theme::get_color_4fv(TH_PANEL_OUTLINE, gz->color);
  copy_v4_v4(gz->color_hi, gz->color);
  return gz;
}

static void sculpt_cursor_buttons_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  sculpt_paint::cursor::ED_sculpt_cursor_buttons_ensure_builtins();

  SculptCursorButtonsGizmoGroup *ggd = MEM_new<SculptCursorButtonsGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  /* Free through the C++ type so the destructor of the #Map member runs. */
  gzgroup->customdata_free = [](void *data) {
    MEM_delete(static_cast<SculptCursorButtonsGizmoGroup *>(data));
  };
}

/** Style a button as active (the theme's toggle-button color) or inactive (panel color). */
static void sculpt_cursor_button_set_active(wmGizmo *gz, const bool active)
{
  if (active) {
    const bTheme *btheme = ui::theme::theme_get();
    if (btheme) {
      const uiWidgetColors &wcol = btheme->tui.wcol_toggle;
      rgba_uchar_to_float(gz->color, wcol.inner_sel);
      rgba_uchar_to_float(gz->color_hi, wcol.inner_sel);
      return;
    }
  }
  float color[4];
  ui::theme::get_color_4fv(TH_PANEL_BACK, color);
  copy_v4_v4(gz->color, color);
  ui::theme::get_color_4fv(TH_PANEL_HEADER, color);
  copy_v4_v4(gz->color_hi, color);
}

/**
 * Whether the state \a btn reflects is currently set. The state is read through the `data_path`
 * property stored on the button (a path relative to the #Scene, the same paths the built-ins pass
 * to `wm.context_toggle` / `wm.context_cycle_enum` through the proxy operator). Booleans are
 * active when true, enums when their current value matches the button's `value_active` identifier.
 * Buttons without a usable `data_path` (plain action buttons) never read as active.
 */
static bool sculpt_cursor_button_state_active(const bContext *C, const SculptCursorButton &btn)
{
  if (btn.properties == nullptr) {
    return false;
  }
  const IDProperty *path_prop = IDP_GetPropertyTypeFromGroup(
      btn.properties, "data_path", IDP_STRING);
  if (path_prop == nullptr) {
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr) {
    return false;
  }
  PointerRNA scene_ptr = RNA_id_pointer_create(&scene->id);
  PointerRNA target;
  PropertyRNA *prop = nullptr;
  if (!RNA_path_resolve(&scene_ptr, IDP_string_get(path_prop), &target, &prop)) {
    return false;
  }
  if (prop == nullptr) {
    return false;
  }

  switch (RNA_property_type(prop)) {
    case PROP_BOOLEAN:
      return RNA_property_boolean_get(&target, prop);
    case PROP_ENUM: {
      const IDProperty *active_prop = IDP_GetPropertyTypeFromGroup(
          btn.properties, "value_active", IDP_STRING);
      if (active_prop == nullptr) {
        return false;
      }
      const char *identifier = nullptr;
      if (!RNA_property_enum_identifier(const_cast<bContext *>(C),
                                        &target,
                                        prop,
                                        RNA_property_enum_get(&target, prop),
                                        &identifier))
      {
        return false;
      }
      /* Copy out of the enum item so the comparison cannot read dangling data. */
      char identifier_buf[BKE_ST_MAXNAME];
      STRNCPY(identifier_buf, (identifier != nullptr) ? identifier : "");
      return STREQ(identifier_buf, IDP_string_get(active_prop));
    }
    default:
      return false;
  }
}

static void sculpt_cursor_buttons_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  SculptCursorButtonsGizmoGroup *ggd = static_cast<SculptCursorButtonsGizmoGroup *>(
      gzgroup->customdata);

  const auto hide_all = [&]() {
    for (const SculptCursorButtonGizmos &gizmos : ggd->buttons.values()) {
      if (gizmos.off) {
        WM_gizmo_set_flag(gizmos.off, WM_GIZMO_HIDDEN, true);
      }
      if (gizmos.on) {
        WM_gizmo_set_flag(gizmos.on, WM_GIZMO_HIDDEN, true);
      }
      if (gizmos.outline) {
        WM_gizmo_set_flag(gizmos.outline, WM_GIZMO_HIDDEN, true);
      }
    }
  };

  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  if (!region || !ob || !ob->runtime->sculpt_session || !scene) {
    hide_all();
    return;
  }

  /* Checked here rather than in the poll: a poll is not re-run when the setting changes, so the
   * buttons would never come back. */
  if (!sculpt_paint::cursor::is_enabled(*scene) ||
      !sculpt_paint::cursor::buttons_visible_get(*scene))
  {
    hide_all();
    return;
  }

  float world_mat[4][4];
  sculpt_cursor_world_matrix_get(*scene, *ob, world_mat);

  /* While a cursor handle is dragged the buttons fade out so the result stays visible. In Deform
   * mode the cursor itself is only written when the Transform session ends, so follow the live
   * Transform pivot the handles are drawn at (see #sculpt_cursor_gizmo_modal) instead. */
  bool is_dragging = false;
  if (const wmGizmo *modal_gz = region->runtime->gizmo_map ?
                                    WM_gizmomap_get_modal(region->runtime->gizmo_map) :
                                    nullptr)
  {
    if (modal_gz->parent_gzgroup &&
        STREQ(modal_gz->parent_gzgroup->type->idname, "VIEW3D_GGT_sculpt_cursor"))
    {
      is_dragging = true;
      const wmGizmoOpElem *gzop = WM_gizmo_operator_get(const_cast<wmGizmo *>(modal_gz), 0);
      if (gzop && gzop->type && STRPREFIX(gzop->type->idname, "TRANSFORM_OT_")) {
        copy_v3_v3(world_mat[3], ob->runtime->sculpt_session->transform_pivot_pos_world);
      }
    }
  }
  const float drag_alpha = 0.25f;

  float co[2];
  if (ED_view3d_project_float_global(region, world_mat[3], co, V3D_PROJ_TEST_CLIP_NEAR) !=
      V3D_PROJ_RET_OK)
  {
    hide_all();
    return;
  }

  /* Rows of buttons above the gizmo, one slot per enabled entry of
   * #UserDef.sculpt_cursor_buttons in list order (the built-ins come first). The cursor gizmo
   * handles extend roughly `U.gizmo_size` UI pixels from the center, so keep the row above them.
   * Each row is centered on the cursor based on the number of visible buttons in it. When
   * #UserDef.sculpt_cursor_buttons_per_row is set, further buttons wrap to rows stacked upward,
   * away from the gizmo. */
  /* User-defined buttons are only shown while the custom buttons extension is active. */
  const bool custom_allowed = sculpt_paint::cursor::addon_active();
  const auto button_hidden = [&](const SculptCursorButton &btn) {
    return !(btn.flag & SCULPT_CURSOR_BUTTON_ENABLED) ||
           (btn.builtin_id == SCULPT_CURSOR_BUTTON_BUILTIN_CUSTOM && !custom_allowed);
  };

  int total = 0;
  for (const SculptCursorButton &btn : U.sculpt_cursor_buttons) {
    if (!button_hidden(btn) && ggd->buttons.contains(btn.unique_id)) {
      total++;
    }
  }
  const float spacing = 34.0f * UI_SCALE_FAC;
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  const float gizmo_size = rv3d ? gizmo_size_factor_get(*scene, *ob, *rv3d, world_mat[3]) : 1.0f;
  const float y_off = (max_ff(float(U.gizmo_size), 1.0f) * gizmo_size * 1.6f + 18.0f) *
                      UI_SCALE_FAC;
  const int per_row = (U.sculpt_cursor_buttons_per_row > 0) ? U.sculpt_cursor_buttons_per_row :
                                                              max_ii(total, 1);

  /* The pool may still hold gizmos of buttons freed without a refresh (e.g. on a preferences
   * reload); the loop below only reveals the gizmos of buttons still in the list. */
  hide_all();

  int row_index = 0;
  for (const SculptCursorButton &btn : U.sculpt_cursor_buttons) {
    const SculptCursorButtonGizmos *gizmos = ggd->buttons.lookup_ptr(btn.unique_id);
    if (gizmos == nullptr) {
      /* Refresh builds a pool entry for every button; nothing to draw before it ran. */
      continue;
    }

    /* Decorative outline drawn over the button, one per entry (see
     * #sculpt_cursor_screen_outline_new). */
    wmGizmo *outline = gizmos->outline;

    if (button_hidden(btn)) {
      /* Disabled buttons keep their gizmos in the pool (refresh builds one for every button),
       * they are just not shown and take no slot in the row. */
      if (gizmos->off) {
        WM_gizmo_set_flag(gizmos->off, WM_GIZMO_HIDDEN, true);
      }
      if (gizmos->on) {
        WM_gizmo_set_flag(gizmos->on, WM_GIZMO_HIDDEN, true);
      }
      if (outline) {
        WM_gizmo_set_flag(outline, WM_GIZMO_HIDDEN, true);
      }
      continue;
    }

    const bool active = sculpt_cursor_button_state_active(C, btn);
    /* Single-icon buttons show the same gizmo in either state; the others switch between the
     * off/on icon variants by visibility (the button gizmo caches its icon on the first draw). */
    const bool show_off = gizmos->single_icon || !active;
    const bool show_on = !gizmos->single_icon && active;

    const int row = row_index / per_row;
    const int col = row_index % per_row;
    const int row_count = min_ii(per_row, total - row * per_row);
    const float x = co[0] + (float(col) - (float(row_count) - 1.0f) / 2.0f) * spacing;
    const float y = co[1] + y_off + float(row) * spacing;
    row_index++;

    for (const bool on_variant : {false, true}) {
      wmGizmo *gz = on_variant ? gizmos->on : gizmos->off;
      if (gz == nullptr) {
        continue;
      }
      gz->matrix_basis[3][0] = x;
      gz->matrix_basis[3][1] = y;
      gz->matrix_basis[3][2] = 0.0f;
      const bool visible = on_variant ? show_on : show_off;
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, !visible);
      if (visible) {
        /* Built-in entries are state toggles and read as active when their state is set (the
         * theme's toggle-button color); user-defined ones are action buttons and keep the plain
         * panel colors. The colors are re-read from the theme every draw so the drag fade-out
         * below does not accumulate. */
        sculpt_cursor_button_set_active(
            gz, active && (btn.builtin_id != SCULPT_CURSOR_BUTTON_BUILTIN_CUSTOM));
        if (is_dragging) {
          gz->color[3] *= drag_alpha;
          gz->color_hi[3] *= drag_alpha;
        }
      }
    }

    if (outline) {
      outline->matrix_basis[3][0] = x;
      outline->matrix_basis[3][1] = y;
      outline->matrix_basis[3][2] = 0.0f;
      ui::theme::get_color_4fv(TH_PANEL_OUTLINE, outline->color);
      if (is_dragging) {
        outline->color[3] *= drag_alpha;
      }
      copy_v4_v4(outline->color_hi, outline->color);
      WM_gizmo_set_flag(outline, WM_GIZMO_HIDDEN, false);
    }
  }
}

/* Rebuild the pool of button gizmos so it matches #UserDef.sculpt_cursor_buttons. Only the pool
 * membership is handled here; position and visibility are applied in
 * #sculpt_cursor_buttons_draw_prepare. */
static void sculpt_cursor_buttons_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  SculptCursorButtonsGizmoGroup *ggd = static_cast<SculptCursorButtonsGizmoGroup *>(
      gzgroup->customdata);

  sculpt_paint::cursor::ED_sculpt_cursor_buttons_ensure_builtins();

  /* The buttons, keyed by #SculptCursorButton::unique_id. */
  Map<int, const SculptCursorButton *> buttons_by_uid;
  for (const SculptCursorButton &btn : U.sculpt_cursor_buttons) {
    buttons_by_uid.add(btn.unique_id, &btn);
  }

  /* Remove the gizmos of buttons that are gone, or whose icon state changed: `GIZMO_GT_button_2d`
   * caches its icon on the first draw, so a changed icon needs a re-created gizmo. */
  ggd->buttons.remove_if([&](const auto &item) {
    const SculptCursorButton *const *btn = buttons_by_uid.lookup_ptr(item.key);
    const SculptCursorButtonGizmos &gizmos = item.value;
    bool changed = true;
    if (btn != nullptr) {
      SculptCursorButtonIconState state_off, state_on;
      const bool single = sculpt_cursor_button_icon_states_get(**btn, state_off, state_on);
      changed = gizmos.single_icon != single || !(gizmos.state_off == state_off) ||
                !(gizmos.state_on == state_on);
    }
    if (changed) {
      /* #WM_gizmo_unlink resets any highlight/modal/select state, removes the gizmo from the group
       * and frees it; freeing the gizmo directly would leave the group's list dangling. */
      for (wmGizmo *gz : {gizmos.off, gizmos.on, gizmos.outline}) {
        if (gz != nullptr) {
          WM_gizmo_unlink(
              &gzgroup->gizmos, gzgroup->parent_gzmap, gz, const_cast<bContext *>(C));
        }
      }
      return true;
    }
    return false;
  });

  /* Create the gizmos of buttons that don't have them yet, in list order. Disabled buttons get
   * gizmos too; #sculpt_cursor_buttons_draw_prepare decides what is drawn. */
  const wmGizmoType *gzt_button = WM_gizmotype_find("GIZMO_GT_button_2d", true);
  wmOperatorType *ot_exec = WM_operatortype_find("SCULPT_OT_cursor_button_exec", true);
  if (gzt_button != nullptr && ot_exec != nullptr) {
    for (const SculptCursorButton &btn : U.sculpt_cursor_buttons) {
      if (ggd->buttons.contains(btn.unique_id)) {
        continue;
      }
      SculptCursorButtonGizmos gizmos;
      gizmos.single_icon = sculpt_cursor_button_icon_states_get(
          btn, gizmos.state_off, gizmos.state_on);
      gizmos.off = sculpt_cursor_button_gizmo_new_for_state(
          gzt_button, gzgroup, ot_exec, btn.unique_id, gizmos.state_off);
      if (!gizmos.single_icon) {
        gizmos.on = sculpt_cursor_button_gizmo_new_for_state(
            gzt_button, gzgroup, ot_exec, btn.unique_id, gizmos.state_on);
      }
      /* Created after the button gizmos of the entry: gizmos draw in creation order, so a later
       * one lies on top. Created earlier, the button's own backdrop would hide the outline. */
      gizmos.outline = sculpt_cursor_screen_outline_new(gzt_button, gzgroup);
      ggd->buttons.add(btn.unique_id, gizmos);
    }
  }
}

/* Rebuild the pool when the button row changes: the #UserDef collection itself (add/remove/move/
 * clear) or any of its items (name, icon, operator, enabled, ...). The active state of a built-in
 * button (pin and such) is a ToolSettings change: it sends an NC_SCENE notifier and a regular
 * redraw, and #sculpt_cursor_buttons_draw_prepare re-evaluates it on every redraw, so it needs no
 * subscription here. */
static void sculpt_cursor_buttons_message_subscribe(const bContext *C,
                                                    wmGizmoGroup *gzgroup,
                                                    wmMsgBus *mbus)
{
  ARegion *region = CTX_wm_region(C);

  wmMsgSubscribeValue msg_sub_value_gz_tag_refresh{};
  msg_sub_value_gz_tag_refresh.owner = region;
  msg_sub_value_gz_tag_refresh.user_data = gzgroup->parent_gzmap;
  msg_sub_value_gz_tag_refresh.notify = WM_gizmo_do_msg_notify_tag_refresh;

  WM_msg_subscribe_rna_anon_prop(
      mbus, Preferences, sculpt_cursor_buttons, &msg_sub_value_gz_tag_refresh);
  /* Changes on an item publish on the item pointer, which resolves to this type. */
  WM_msg_subscribe_rna_anon_type(mbus, SculptCursorButton, &msg_sub_value_gz_tag_refresh);
}

/** \} */

void VIEW3D_GGT_sculpt_cursor_buttons(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Sculpt Cursor Buttons";
  gzgt->idname = "VIEW3D_GGT_sculpt_cursor_buttons";

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_SCALE |
               WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = sculpt_cursor_buttons_poll;
  gzgt->setup = sculpt_cursor_buttons_setup;
  gzgt->refresh = sculpt_cursor_buttons_refresh;
  gzgt->draw_prepare = sculpt_cursor_buttons_draw_prepare;
  gzgt->message_subscribe = sculpt_cursor_buttons_message_subscribe;
}

}  // namespace blender::ed::view3d
