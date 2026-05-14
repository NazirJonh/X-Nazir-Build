/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spbuttons
 * \brief Property drag & drop to animatable fields in Properties Editor.
 */

#include <cstring>
#include <string>

#include <fmt/format.h>

#include "DNA_space_types.h"

#include "BLI_string.h"

#include "BKE_context.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"

#include "WM_api.hh"

#include "ED_object.hh"

#include "buttons_intern.hh"

/* For accessing Button::rnaprop and Button::rnapoin. */
#include "../interface/interface_intern.hh"

namespace blender::ed::buttons {

/* ------------------------------------------------------------------- */
/** \name Property Drop to Animatable Field - Drop Box
 * \{ */

/**
 * Poll function for property drop to animatable field.
 * Checks if the UI button under mouse is animatable.
 */
static bool prop_drop_to_field_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  /* Step 1: Validate drag type. */
  if (drag->type != WM_DRAG_STRING) {
    return false;
  }

  /* Step 2: Quick payload format check (must have at least 2 colons). */
  const std::string &payload = WM_drag_get_string(drag);
  if (std::count(payload.begin(), payload.end(), ':') < 2) {
    return false;
  }

  /* Step 3: Find UI button under mouse cursor. */
  ARegion *region = CTX_wm_region(C);
  if (!region || !event) {
    printf("[PROP DROP POLL] No region or event\n");
    return false;
  }

  ui::Button *but = ui::but_find_mouse_over(region, event);
  if (!but) {
    return false;
  }
  if (!but->rnaprop) {
    printf("[PROP DROP POLL] Button found but no rnaprop\n");
    return false;
  }

  /* Step 4: Check if property is animatable (KEY CHECK!). */
  if (!RNA_property_animateable(&but->rnapoin, but->rnaprop)) {
    printf("[PROP DROP POLL] Property '%s' not animatable\n",
           RNA_property_identifier(but->rnaprop));
    return false;
  }

  /* Step 5: Check property type (must be numeric or boolean). */
  PropertyType prop_type = RNA_property_type(but->rnaprop);
  if (prop_type != PROP_FLOAT && prop_type != PROP_INT && prop_type != PROP_BOOLEAN) {
    printf("[PROP DROP POLL] Property '%s' wrong type: %d\n",
           RNA_property_identifier(but->rnaprop),
           int(prop_type));
    return false;
  }

  printf("[PROP DROP POLL] PASS: property '%s'\n", RNA_property_identifier(but->rnaprop));
  return true;
}

/**
 * Copy function for property drop to animatable field.
 * Extracts target property information from UI button context.
 */
static void prop_drop_to_field_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  /* Step 1: Copy drag string. */
  const std::string &payload = WM_drag_get_string(drag);
  RNA_string_set(drop->ptr, "drag_string", payload.c_str());

  /* Step 2: Get UI button under mouse cursor. */
  wmWindow *win = CTX_wm_window(C);
  if (!win || !win->runtime->eventstate) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  ui::Button *but = ui::but_find_mouse_over(region, win->runtime->eventstate);
  if (!but || !but->rnaprop) {
    return;
  }

  /* Step 3: Extract target property info. */
  const char *prop_id = RNA_property_identifier(but->rnaprop);
  RNA_string_set(drop->ptr, "target_property", prop_id);

  /* Step 4: Determine array index. */
  int array_idx = -1;
  if (RNA_property_array_check(but->rnaprop)) {
    array_idx = but->rnaindex;
  }
  RNA_int_set(drop->ptr, "target_array_index", array_idx);

  /* Step 5: Extract target ID info. */
  ID *target_id = but->rnapoin.owner_id;
  if (target_id) {
    /* ID name without 2-char prefix. */
    RNA_string_set(drop->ptr, "target_id_name", target_id->name + 2);

    /* 2-character ID type code. */
    char id_type[3] = {target_id->name[0], target_id->name[1], '\0'};
    RNA_string_set(drop->ptr, "target_id_type", id_type);
  }
}

/**
 * Tooltip function for property drop to animatable field.
 * Shows "Link \"source_prop\" → \"target_prop\"".
 */
static std::string prop_drop_to_field_tooltip(bContext *C,
                                              wmDrag *drag,
                                              const int /*xy*/[2],
                                              wmDropBox * /*drop*/)
{
  wmWindow *win = CTX_wm_window(C);
  if (!win || !win->runtime->eventstate) {
    return "";
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return "";
  }

  ui::Button *but = ui::but_find_mouse_over(region, win->runtime->eventstate);
  if (!but || !but->rnaprop) {
    return "";
  }

  /* Extract property name from payload (last component after last colon). */
  const std::string &payload = WM_drag_get_string(drag);
  size_t last_colon = payload.rfind(':');
  std::string prop_name = (last_colon != std::string::npos) ? payload.substr(last_colon + 1) :
                                                              payload;

  /* Get target property name. */
  const char *target_prop = RNA_property_identifier(but->rnaprop);

  /* Build tooltip. */
  return fmt::format("Link \"{}\" \u2192 \"{}\"", prop_name, target_prop);
}

/** \} */

}  // namespace blender::ed::buttons

/* ------------------------------------------------------------------- */
/** \name Drop Box Registration
 * \{ */

namespace blender {

void buttons_dropboxes_property()
{
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find("Properties", SPACE_PROPERTIES, RGN_TYPE_WINDOW);

  WM_dropbox_add(lb,
                 "OBJECT_OT_prop_drop_to_field",
                 ed::buttons::prop_drop_to_field_poll,
                 ed::buttons::prop_drop_to_field_copy,
                 nullptr,
                 ed::buttons::prop_drop_to_field_tooltip);
}

}  // namespace blender

/** \} */
