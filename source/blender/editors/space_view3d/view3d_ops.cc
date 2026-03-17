/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_appdir.hh"
#include "BKE_blender_copybuffer.hh"
#include "BKE_blendfile.hh"
#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BKE_report.hh"

#include "BLO_readfile.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_outliner.hh"
#include "ED_screen.hh"
#include "ED_transform.hh"

#include "UI_interface_c.hh"
#include "../interface/interface_intern.hh"
#include "../interface/interface_tag_bar.hh"

#include "view3d_intern.hh"
#include "view3d_navigate.hh"

#ifdef WIN32
#  include "BLI_math_base.h" /* M_PI */
#endif

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Local Utilities
 * \{ */

static void view3d_copybuffer_filepath_get(char filepath[FILE_MAX], size_t filepath_maxncpy)
{
  BLI_path_join(filepath, filepath_maxncpy, BKE_tempdir_base(), "copybuffer.blend");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Viewport Copy Operator
 * \{ */

static wmOperatorStatus view3d_copybuffer_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::blendfile;

  Main *bmain = CTX_data_main(C);
  PartialWriteContext copybuffer{*bmain};

  Object *obact = CTX_data_active_object(C);
  Object *obact_copy = nullptr;

  /* context, selection, could be generalized */
  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    ID *ob_id_copy = copybuffer.id_add(
        &ob->id,
        PartialWriteContext::IDAddOptions{
            (PartialWriteContext::IDAddOperations::SET_FAKE_USER |
             PartialWriteContext::IDAddOperations::SET_CLIPBOARD_MARK |
             PartialWriteContext::IDAddOperations::ADD_DEPENDENCIES)},
        nullptr);

    if (obact && (obact == ob)) {
      obact_copy = reinterpret_cast<Object *>(ob_id_copy);
    }
  }
  CTX_DATA_END;

  /* Explicitly adding an object to the copy/paste buffer _may_ add others as dependencies (e.g. a
   * parent object). So count to total amount of objects added, to get a matching number with the
   * one reported by the "paste" operation. */
  int num_copied = 0;

  /* Count & mark the active as done (when set). */
  for (Object &ob : copybuffer.bmain.objects) {
    ob.flag &= ~OB_FLAG_ACTIVE_CLIPBOARD;
    num_copied += 1;
  }

  if (num_copied == 0) {
    BKE_report(op->reports, RPT_INFO, "No objects selected to copy");
    return OPERATOR_CANCELLED;
  }

  if (obact_copy) {
    obact_copy->flag |= OB_FLAG_ACTIVE_CLIPBOARD;
  }

  char filepath[FILE_MAX];
  view3d_copybuffer_filepath_get(filepath, sizeof(filepath));
  copybuffer.write(filepath, *op->reports);

  BKE_reportf(op->reports, RPT_INFO, "Copied %d selected object(s)", num_copied);

  return OPERATOR_FINISHED;
}

static void VIEW3D_OT_copybuffer(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Copy Objects";
  ot->idname = "VIEW3D_OT_copybuffer";
  ot->description = "Copy the selected objects to the internal clipboard";

  /* API callbacks. */
  ot->exec = view3d_copybuffer_exec;
  ot->poll = ED_operator_scene;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Viewport Paste Operator
 * \{ */

static wmOperatorStatus view3d_pastebuffer_exec(bContext *C, wmOperator *op)
{
  char filepath[FILE_MAX];
  int flag = 0;

  if (RNA_boolean_get(op->ptr, "autoselect")) {
    flag |= FILE_AUTOSELECT | int(BLO_LIBLINK_APPEND_SET_OB_ACTIVE_CLIPBOARD);
  }
  if (RNA_boolean_get(op->ptr, "active_collection")) {
    flag |= FILE_ACTIVE_COLLECTION;
  }

  view3d_copybuffer_filepath_get(filepath, sizeof(filepath));

  const int num_pasted = BKE_copybuffer_paste(C, filepath, flag, op->reports, FILTER_ID_OB);
  if (num_pasted == 0) {
    BKE_report(op->reports, RPT_INFO, "No objects to paste");
    return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, nullptr);
  ED_outliner_select_sync_from_object_tag(C);

  BKE_reportf(op->reports, RPT_INFO, "%d object(s) pasted", num_pasted);

  return OPERATOR_FINISHED;
}

static void VIEW3D_OT_pastebuffer(wmOperatorType *ot)
{

  /* identifiers */
  ot->name = "Paste Objects";
  ot->idname = "VIEW3D_OT_pastebuffer";
  ot->description = "Paste objects from the internal clipboard";

  /* API callbacks. */
  ot->exec = view3d_pastebuffer_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "autoselect", true, "Select", "Select pasted objects");
  RNA_def_boolean(ot->srna,
                  "active_collection",
                  true,
                  "Active Collection",
                  "Put pasted objects in the active collection");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Bar Filter Toggle Operator
 * \{ */

static bool view3d_tag_bar_toggle_poll(bContext *C)
{
  const ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return false;
  }

  using namespace blender::ui;
  TagFilterStateRef state{};
  return tag_filter_state_from_area(area, &state) && state.active_tags && state.filter_enabled;
}

static wmOperatorStatus view3d_tag_bar_toggle_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return OPERATOR_CANCELLED;
  }

  using namespace blender::ui;
  TagFilterStateRef state{};
  if (!tag_filter_state_from_area(area, &state) || !state.active_tags || !state.filter_enabled) {
    return OPERATOR_CANCELLED;
  }

  /* Deactivate "New Add-on!" filter when clicking on a normal tag. */
  if (is_new_addon_filter_active(area)) {
    /* Clear saved tags since user is manually selecting tags */
    set_saved_tag_filter_tags(area, "");
    set_new_addon_filter_active(area, false);
  }

  /* Remember filter state BEFORE any changes - needed for N-Panel hide logic. */
  const bool was_filter_enabled_before = *state.filter_enabled != 0;

  /* Get current active category BEFORE changing tags. */
  ARegion *region_ui = BKE_area_find_region_type(area, RGN_TYPE_UI);
  const char *current_category = nullptr;
  if (region_ui) {
    current_category = ui::panel_category_active_get(region_ui, false);
  }

  /* Build tag combination key for current state (before toggle). */
  char current_tag_key[256];
  ui::tag_build_combination_key(state.active_tags, current_tag_key, sizeof(current_tag_key));

  /* Save current category for current tag combination. */
  if (current_category && current_category[0]) {
    ui::tag_save_last_active_category(C, current_tag_key, current_category);
  }

  char tag_name[64];
  RNA_string_get(op->ptr, "tag_name", tag_name);

  /* Check if Shift is pressed for multi-select */
  const bool shift_pressed = (event->modifier & KM_SHIFT) != 0;

  /* Copy current active tags to work with */
  char tags_copy[256];
  BLI_strncpy(tags_copy, state.active_tags, sizeof(tags_copy));



  /* Check if tag is already in the list */
  bool tag_found = false;
  char *tag = strtok(tags_copy, ",;");
  while (tag != nullptr) {
    while (*tag == ' ') {
      tag++;
    }
    if (STREQ(tag, tag_name)) {
      tag_found = true;
      break;
    }
    tag = strtok(nullptr, ",;");
  }

  /* Toggle: add if not found, remove if found */
  char new_tags[256] = "";
  const bool was_removing_tag = tag_found; /* Remember if we're removing a tag */

  if (!tag_found) {
    /* Add the tag to the list */
    if (shift_pressed) {
      /* Multi-select: add to existing tags */
      if (state.active_tags[0] != '\0') {
        SNPRINTF(new_tags, "%s,%s", state.active_tags, tag_name);
      }
      else {
        BLI_strncpy(new_tags, tag_name, sizeof(new_tags));
      }
    }
    else {
      /* Single-select: replace all tags with this one */
      BLI_strncpy(new_tags, tag_name, sizeof(new_tags));
    }
  }
  else {
    /* Remove the tag from the list */
    char temp[256];
    BLI_strncpy(temp, state.active_tags, sizeof(temp));
    char *token = strtok(temp, ",;");
    bool first = true;
    while (token != nullptr) {
      while (*token == ' ') {
        token++;
      }
      if (!STREQ(token, tag_name)) {
        if (!first) {
          BLI_strncat(new_tags, ",", sizeof(new_tags));
        }
        BLI_strncat(new_tags, token, sizeof(new_tags));
        first = false;
      }
      token = strtok(nullptr, ",;");
    }
  }

  /* Update the active tags string */
  BLI_strncpy(state.active_tags, new_tags, 256);

  /* Handle filter state based on whether tags were removed or added. */
  if (was_removing_tag) {
    /* Tag was removed - don't force enable filter, keep filter state unchanged */
    *state.filter_enabled = 0;  /* Keep filter OFF if all tags removed */
  }
  else {
    /* Tag was added - enable filter */
    *state.filter_enabled = 1;
  }

  /* After updating tags, try to restore saved category for the new combination. */
  if (region_ui) {
    char new_tag_key[256];
    ui::tag_build_combination_key(state.active_tags, new_tag_key, sizeof(new_tag_key));

    char saved_category[64];
    if (ui::tag_get_last_active_category(C, new_tag_key, saved_category, sizeof(saved_category))) {
      /* Check if saved category is visible with new tag filter. */
      const wmWindowManager *wm = CTX_wm_manager(C);
      if (ui::panel_category_is_visible_by_tags(C, wm, saved_category)) {
        ui::panel_category_active_set(region_ui, saved_category);
      }
      else {
        /* Fall back to first visible category. */
        ui::panel_category_tabs_ensure_active_visible(C, region_ui);
      }
    }
    else {
      /* No saved category - use default behavior (ensure active is visible). */
      ui::panel_category_tabs_ensure_active_visible(C, region_ui);
    }
  }

  /* Check N-Panel visibility before toggling */
  const bool was_npanel_hidden = region_ui && (region_ui->flag & RGN_FLAG_HIDDEN);

  /* Open N-Panel (Sidebar) if it's hidden, so users can see filtered categories */
  if (was_npanel_hidden) {
    ED_region_toggle_hidden(C, region_ui);
  }
  /* Hide N-Panel only if: filter was enabled before AND we removed the last tag. */
  else if (was_filter_enabled_before && was_removing_tag && new_tags[0] == '\0') {
    /* No more tags active and filter was on - hide N-Panel */
    ED_region_toggle_hidden(C, region_ui);
  }

  /* Trigger redraw to update category order for new tag combination */
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_CATEGORY_GLYPHS, nullptr);
  ED_area_tag_redraw(area);

  return OPERATOR_FINISHED;
}

static void VIEW3D_OT_tag_bar_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Tag Filter";
  ot->idname = "VIEW3D_OT_tag_bar_toggle";
  ot->description = "Toggle a tag filter in the 3D View tag bar";

  ot->invoke = view3d_tag_bar_toggle_invoke;
  ot->poll = view3d_tag_bar_toggle_poll;

  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "tag_name", nullptr, 64, "Tag Name", "Name of the tag to toggle");
}

/** \name Tag Bar Filter Toggle Operator
 * \{ */

static bool view3d_tag_bar_filter_toggle_poll(bContext *C)
{
  const ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return false;
  }

  using namespace blender::ui;
  TagFilterStateRef state{};
  return tag_filter_state_from_area(area, &state) && state.active_tags && state.filter_enabled;
}

static wmOperatorStatus view3d_tag_bar_filter_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return OPERATOR_CANCELLED;
  }

  using namespace blender::ui;
  TagFilterStateRef state{};
  if (!tag_filter_state_from_area(area, &state) || !state.active_tags || !state.filter_enabled) {
    return OPERATOR_CANCELLED;
  }

  /* Get runtime data for saving/restoring tags */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data) {
    return OPERATOR_CANCELLED;
  }

  /* Deactivate "New Add-on!" filter when toggling normal filter. */
  if (is_new_addon_filter_active(area)) {
    /* Clear saved tags since user is manually toggling filter */
    set_saved_tag_filter_tags(area, "");
    set_new_addon_filter_active(area, false);
  }

  ARegion *region_ui = BKE_area_find_region_type(area, RGN_TYPE_UI);

  /* Toggle the filter enabled flag */
  *state.filter_enabled = *state.filter_enabled ? 0 : 1;

  if (*state.filter_enabled) {
    /* Filter INACTIVE -> ACTIVE: Restore saved tags and show N-Panel */
    if (data->saved_tags[0] != '\0') {
      BLI_strncpy(state.active_tags, data->saved_tags, 256);
    }
    else {
      /* No saved tags - clear active tags so show all categories when filter is enabled. */
      state.active_tags[0] = '\0';
    }

    /* Restore last active category for this tag combination. */
    if (region_ui) {
      char tag_key[256];
      tag_build_combination_key(state.active_tags, tag_key, sizeof(tag_key));
      char saved_category[64];
      if (tag_get_last_active_category(C, tag_key, saved_category, sizeof(saved_category))) {
        const wmWindowManager *wm = CTX_wm_manager(C);
        if (ui::panel_category_is_visible_by_tags(C, wm, saved_category)) {
          ui::panel_category_active_set(region_ui, saved_category);
        }
        else {
          ui::panel_category_tabs_ensure_active_visible(C, region_ui);
        }
      }
      else {
        ui::panel_category_tabs_ensure_active_visible(C, region_ui);
      }
    }

    /* Open N-Panel if it's hidden */
    if (region_ui && (region_ui->flag & RGN_FLAG_HIDDEN)) {
      ED_region_toggle_hidden(C, region_ui);
    }
  }
  else {
    /* Filter ACTIVE -> INACTIVE: Save tags and current category. */
    BLI_strncpy(data->saved_tags, state.active_tags, sizeof(data->saved_tags));

    if (region_ui) {
      const char *current_category = ui::panel_category_active_get(region_ui, false);
      if (current_category && current_category[0]) {
        char tag_key[256];
        ui::tag_build_combination_key(state.active_tags, tag_key, sizeof(tag_key));
        ui::tag_save_last_active_category(C, tag_key, current_category);
      }
    }
    /* Note: Don't clear active_tag_filter_tags - keep them for next activation */
  }

  /* Trigger redraw to update UI */
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_CATEGORY_GLYPHS, nullptr);
  ED_region_tag_redraw(CTX_wm_region(C));
  ED_area_tag_redraw(area);

  return OPERATOR_FINISHED;
}

static void VIEW3D_OT_tag_bar_filter_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Tag Filtering";
  ot->idname = "VIEW3D_OT_tag_bar_filter_toggle";
  ot->description = "Toggle tag filtering on/off (when off, all categories are shown)";

  ot->exec = view3d_tag_bar_filter_toggle_exec;
  ot->poll = view3d_tag_bar_filter_toggle_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void view3d_operatortypes()
{
  WM_operatortype_append(VIEW3D_OT_rotate);
  WM_operatortype_append(VIEW3D_OT_move);
  WM_operatortype_append(VIEW3D_OT_zoom);
  WM_operatortype_append(VIEW3D_OT_zoom_camera_1_to_1);
  WM_operatortype_append(VIEW3D_OT_dolly);
#ifdef WITH_INPUT_NDOF
  WM_operatortype_append(VIEW3D_OT_ndof_orbit_zoom);
  WM_operatortype_append(VIEW3D_OT_ndof_orbit);
  WM_operatortype_append(VIEW3D_OT_ndof_pan);
  WM_operatortype_append(VIEW3D_OT_ndof_all);
#endif /* WITH_INPUT_NDOF */
  WM_operatortype_append(VIEW3D_OT_view_all);
  WM_operatortype_append(VIEW3D_OT_view_axis);
  WM_operatortype_append(VIEW3D_OT_view_camera);
  WM_operatortype_append(VIEW3D_OT_view_orbit);
  WM_operatortype_append(VIEW3D_OT_view_roll);
  WM_operatortype_append(VIEW3D_OT_view_pan);
  WM_operatortype_append(VIEW3D_OT_view_persportho);
  WM_operatortype_append(VIEW3D_OT_camera_background_image_add);
  WM_operatortype_append(VIEW3D_OT_camera_background_image_remove);
  WM_operatortype_append(VIEW3D_OT_drop_world);
  WM_operatortype_append(VIEW3D_OT_view_selected);
  WM_operatortype_append(VIEW3D_OT_view_lock_clear);
  WM_operatortype_append(VIEW3D_OT_view_lock_to_active);
  WM_operatortype_append(VIEW3D_OT_view_center_cursor);
  WM_operatortype_append(VIEW3D_OT_view_center_pick);
  WM_operatortype_append(VIEW3D_OT_view_center_camera);
  WM_operatortype_append(VIEW3D_OT_view_center_lock);
  WM_operatortype_append(VIEW3D_OT_select);
  WM_operatortype_append(VIEW3D_OT_select_box);
  WM_operatortype_append(VIEW3D_OT_clip_border);
  WM_operatortype_append(VIEW3D_OT_select_circle);
  WM_operatortype_append(VIEW3D_OT_smoothview);
  WM_operatortype_append(VIEW3D_OT_render_border);
  WM_operatortype_append(VIEW3D_OT_clear_render_border);
  WM_operatortype_append(VIEW3D_OT_zoom_border);
  WM_operatortype_append(VIEW3D_OT_cursor3d);
  WM_operatortype_append(VIEW3D_OT_select_lasso);
  WM_operatortype_append(VIEW3D_OT_select_menu);
  WM_operatortype_append(VIEW3D_OT_bone_select_menu);
  WM_operatortype_append(VIEW3D_OT_camera_to_view);
  WM_operatortype_append(VIEW3D_OT_camera_to_view_selected);
  WM_operatortype_append(VIEW3D_OT_object_as_camera);
  WM_operatortype_append(VIEW3D_OT_localview);
  WM_operatortype_append(VIEW3D_OT_localview_remove_from);
  WM_operatortype_append(VIEW3D_OT_fly);
  WM_operatortype_append(VIEW3D_OT_walk);
  WM_operatortype_append(VIEW3D_OT_navigate);
  WM_operatortype_append(VIEW3D_OT_copybuffer);
  WM_operatortype_append(VIEW3D_OT_pastebuffer);
  WM_operatortype_append(VIEW3D_OT_tag_bar_toggle);
  WM_operatortype_append(VIEW3D_OT_tag_bar_filter_toggle);

  WM_operatortype_append(VIEW3D_OT_object_mode_pie_or_toggle);

  WM_operatortype_append(VIEW3D_OT_snap_selected_to_grid);
  WM_operatortype_append(VIEW3D_OT_snap_selected_to_cursor);
  WM_operatortype_append(VIEW3D_OT_snap_selected_to_active);
  WM_operatortype_append(VIEW3D_OT_snap_cursor_to_grid);
  WM_operatortype_append(VIEW3D_OT_snap_cursor_to_center);
  WM_operatortype_append(VIEW3D_OT_snap_cursor_to_selected);
  WM_operatortype_append(VIEW3D_OT_snap_cursor_to_active);

  WM_operatortype_append(VIEW3D_OT_interactive_add);

  WM_operatortype_append(VIEW3D_OT_toggle_shading);
  WM_operatortype_append(VIEW3D_OT_toggle_xray);
  WM_operatortype_append(VIEW3D_OT_toggle_matcap_flip);

  WM_operatortype_append(VIEW3D_OT_ruler_add);
  WM_operatortype_append(VIEW3D_OT_ruler_remove);

  ed::transform::transform_operatortypes();
}

void view3d_keymap(wmKeyConfig *keyconf)
{
  WM_keymap_ensure(keyconf, "3D View Generic", SPACE_VIEW3D, RGN_TYPE_WINDOW);

  /* only for region 3D window */
  WM_keymap_ensure(keyconf, "3D View", SPACE_VIEW3D, RGN_TYPE_WINDOW);

  fly_modal_keymap(keyconf);
  walk_modal_keymap(keyconf);
  viewrotate_modal_keymap(keyconf);
  viewmove_modal_keymap(keyconf);
  viewzoom_modal_keymap(keyconf);
  viewdolly_modal_keymap(keyconf);
  viewplace_modal_keymap(keyconf);
}

/** \} */

}  // namespace blender
