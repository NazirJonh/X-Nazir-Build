/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

/* a full doc with API notes can be found in
 * bf-blender/trunk/blender/doc/guides/interface_API.txt */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"
#include "BLI_set.hh"

#include "BLT_translation.hh"

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLF_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "GPU_batch_presets.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "interface_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Defines & Structs
 * \{ */

#define ANIMATION_TIME 0.30
#define ANIMATION_INTERVAL 0.02

enum PanelRuntimeFlag {
  PANEL_LAST_ADDED = (1 << 0),
  PANEL_ACTIVE = (1 << 2),
  PANEL_WAS_ACTIVE = (1 << 3),
  PANEL_ANIM_ALIGN = (1 << 4),
  PANEL_NEW_ADDED = (1 << 5),
  PANEL_SEARCH_FILTER_MATCH = (1 << 7),
  /**
   * Use the status set by property search (#PANEL_SEARCH_FILTER_MATCH)
   * instead of #PNL_CLOSED. Set to true on every property search update.
   */
  PANEL_USE_CLOSED_FROM_SEARCH = (1 << 8),
  /** The Panel was before the start of the current / latest layout pass. */
  PANEL_WAS_CLOSED = (1 << 9),
  /**
   * Set when the panel is being dragged and while it animates back to its aligned
   * position. Unlike #PANEL_STATE_ANIMATION, this is applied to sub-panels as well.
   */
  PANEL_IS_DRAG_DROP = (1 << 10),
  /** Draw a border with the active color around the panel. */
  PANEL_ACTIVE_BORDER = (1 << 11),
};

/* The state of the mouse position relative to the panel. */
enum PanelMouseState {
  PANEL_MOUSE_OUTSIDE,        /** Mouse is not in the panel. */
  PANEL_MOUSE_INSIDE_CONTENT, /** Mouse is in the actual panel content. */
  PANEL_MOUSE_INSIDE_HEADER,  /** Mouse is in the panel header. */
  PANEL_MOUSE_INSIDE_LAYOUT_PANEL_HEADER /** Mouse is in the header of a layout panel. */,
};

enum HandlePanelState {
  PANEL_STATE_DRAG,
  PANEL_STATE_ANIMATION,
  PANEL_STATE_EXIT,
};

struct HandlePanelData {
  HandlePanelState state;

  /* Animation. */
  wmTimer *animtimer;
  double starttime;

  /* Dragging. */
  int startx, starty;
  int startofsx, startofsy;
  float start_cur_xmin, start_cur_ymin;
};

struct PanelSort {
  Panel *panel;
  int new_offset_x;
  int new_offset_y;
};

static void panel_set_expansion_from_list_data(const bContext *C, Panel *panel);
static int get_panel_real_size_y(const Panel *panel);
static void panel_activate_state(const bContext *C, Panel *panel, const HandlePanelState state);
static bool panel_type_context_poll(ARegion *region,
                                    const PanelType *panel_type,
                                    const char *context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Functions
 * \{ */

static bool panel_active_animation_changed(ListBaseT<Panel> *lb,
                                           Panel **r_panel_animation,
                                           bool *r_no_animation)
{
  for (Panel &panel : *lb) {
    /* Detect panel active flag changes. */
    if (!(panel.type && panel.type->parent)) {
      if ((panel.runtime_flag & PANEL_WAS_ACTIVE) && !(panel.runtime_flag & PANEL_ACTIVE)) {
        return true;
      }
      if (!(panel.runtime_flag & PANEL_WAS_ACTIVE) && (panel.runtime_flag & PANEL_ACTIVE)) {
        return true;
      }
    }

    /* Detect changes in panel expansions. */
    if (bool(panel.runtime_flag & PANEL_WAS_CLOSED) != panel_is_closed(&panel)) {
      *r_panel_animation = &panel;
      return false;
    }

    if ((panel.runtime_flag & PANEL_ACTIVE) && !panel_is_closed(&panel)) {
      if (panel_active_animation_changed(&panel.children, r_panel_animation, r_no_animation)) {
        return true;
      }
    }

    /* Detect animation. */
    if (panel.activedata) {
      HandlePanelData *data = static_cast<HandlePanelData *>(panel.activedata);
      if (data->state == PANEL_STATE_ANIMATION) {
        *r_panel_animation = &panel;
      }
      else {
        /* Don't animate while handling other interaction. */
        *r_no_animation = true;
      }
    }
    if ((panel.runtime_flag & PANEL_ANIM_ALIGN) && !(*r_panel_animation)) {
      *r_panel_animation = &panel;
    }
  }

  return false;
}

/**
 * \return True if the properties editor switch tabs since the last layout pass.
 */
static bool properties_space_needs_realign(const ScrArea *area, const ARegion *region)
{
  if (area->spacetype == SPACE_PROPERTIES && region->regiontype == RGN_TYPE_WINDOW) {
    const SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);

    if (sbuts->mainbo != sbuts->mainb) {
      return true;
    }
  }

  return false;
}

static bool panels_need_realign(const ScrArea *area, ARegion *region, Panel **r_panel_animation)
{
  *r_panel_animation = nullptr;

  if (properties_space_needs_realign(area, region)) {
    return true;
  }

  /* Detect if a panel was added or removed. */
  Panel *panel_animation = nullptr;
  bool no_animation = false;
  if (panel_active_animation_changed(&region->panels, &panel_animation, &no_animation)) {
    return true;
  }

  /* Detect panel marked for animation, if we're not already animating. */
  if (panel_animation) {
    if (!no_animation) {
      *r_panel_animation = panel_animation;
    }
    return true;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions for Instanced Panels
 * \{ */

static Panel *panel_add_instanced(ListBaseT<Panel> *panels,
                                  PanelType *panel_type,
                                  PointerRNA *custom_data)
{
  Panel *panel = BKE_panel_new(panel_type);

  panel->runtime->custom_data_ptr = custom_data;
  panel->runtime_flag |= PANEL_NEW_ADDED;

  /* Add the panel's children too. Although they aren't instanced panels, we can still use this
   * function to create them, as panel_begin does other things we don't need to do. */
  for (LinkData &child : panel_type->children) {
    PanelType *child_type = static_cast<PanelType *>(child.data);
    panel_add_instanced(&panel->children, child_type, custom_data);
  }

  /* Make sure the panel is added to the end of the display-order as well. This is needed for
   * loading existing files.
   *
   * NOTE: We could use special behavior to place it after the panel that starts the list of
   * instanced panels, but that would add complexity that isn't needed for now. */
  int max_sortorder = 0;
  for (Panel &existing_panel : *panels) {
    max_sortorder = std::max(existing_panel.sortorder, max_sortorder);
  }
  panel->sortorder = max_sortorder + 1;

  BLI_addtail(panels, panel);

  return panel;
}

Panel *panel_add_instanced(const bContext *C,
                           ARegion *region,
                           ListBaseT<Panel> *panels,
                           const char *panel_idname,
                           PointerRNA *custom_data)
{
  ARegionType *region_type = region->runtime->type;

  PanelType *panel_type = static_cast<PanelType *>(
      BLI_findstring(&region_type->paneltypes, panel_idname, offsetof(PanelType, idname)));

  if (panel_type == nullptr) {
    printf("Panel type '%s' not found.\n", panel_idname);
    return nullptr;
  }

  Panel *new_panel = panel_add_instanced(panels, panel_type, custom_data);

  /* Do this after #panel_add_instatnced so all sub-panels are added. */
  panel_set_expansion_from_list_data(C, new_panel);

  return new_panel;
}

void list_panel_unique_str(Panel *panel, char *r_name)
{
  /* The panel sort-order will be unique for a specific panel type because the instanced
   * panel list is regenerated for every change in the data order / length. */
  BLI_snprintf_utf8(r_name, INSTANCED_PANEL_UNIQUE_STR_SIZE, "%d", panel->sortorder);
}

/**
 * Free a panel and its children. Custom data is shared by the panel and its children
 * and is freed by #panels_free_instanced.
 *
 * \note The only panels that should need to be deleted at runtime are panels with the
 * #PANEL_TYPE_INSTANCED flag set.
 */
static void panel_delete(ARegion *region, ListBaseT<Panel> *panels, Panel *panel)
{
  /* Recursively delete children. */
  for (Panel &child : panel->children.items_mutable()) {
    panel_delete(region, &panel->children, &child);
  }
  BLI_freelistN(&panel->children);

  BLI_remlink(panels, panel);
  BKE_panel_free(panel);
}

static void panel_exit_state_recursive(const bContext *C, Panel &panel)
{
  if (panel.activedata != nullptr) {
    panel_activate_state(C, &panel, PANEL_STATE_EXIT);
  }
  for (Panel &child : panel.children) {
    panel_exit_state_recursive(C, child);
  }
}

void panels_free_instanced(const bContext *C, ARegion *region)
{
  /* Delete panels with the instanced flag. */
  for (Panel &panel : region->panels.items_mutable()) {
    if (!panel.type) {
      continue;
    }
    if ((panel.type->flag & PANEL_TYPE_INSTANCED) == 0) {
      continue;
    }
    /* Make sure any active handler is removed from this panel or its children before deleting
     * them. */
    if (C != nullptr) {
      panel_exit_state_recursive(C, panel);
    }

    /* Free panel's custom data. */
    if (panel.runtime->custom_data_ptr != nullptr) {
      MEM_delete(panel.runtime->custom_data_ptr);
    }

    /* Free the panel and its sub-panels. */
    panel_delete(region, &region->panels, &panel);
  }
}

bool panel_list_matches_data(ARegion *region,
                             ListBase *data,
                             ListPanelIDFromDataFunc panel_idname_func)
{
  /* Check for nullptr data. */
  int data_len = 0;
  Link *data_link = nullptr;
  if (data == nullptr) {
    data_len = 0;
    data_link = nullptr;
  }
  else {
    data_len = BLI_listbase_count(data);
    data_link = static_cast<Link *>(data->first);
  }

  int i = 0;
  for (Panel &panel : region->panels) {
    if (panel.type != nullptr && panel.type->flag & PANEL_TYPE_INSTANCED) {
      /* The panels were reordered by drag and drop. */
      if (panel.flag & PNL_INSTANCED_LIST_ORDER_CHANGED) {
        return false;
      }

      /* We reached the last data item before the last instanced panel. */
      if (data_link == nullptr) {
        return false;
      }

      /* Check if the panel type matches the panel type from the data item. */
      char panel_idname[MAX_NAME];
      panel_idname_func(data_link, panel_idname);
      if (!STREQ(panel_idname, panel.type->idname)) {
        return false;
      }

      data_link = data_link->next;
      i++;
    }
  }

  /* If we didn't make it to the last list item, the panel list isn't complete. */
  if (i != data_len) {
    return false;
  }

  return true;
}

static void reorder_instanced_panel_list(bContext *C, ARegion *region, Panel *drag_panel)
{
  /* Without a type we cannot access the reorder callback. */
  if (drag_panel->type == nullptr) {
    return;
  }
  /* Don't reorder if this instanced panel doesn't support drag and drop reordering. */
  if (drag_panel->type->reorder == nullptr) {
    return;
  }

  char *context = nullptr;
  if (!panel_category_is_visible(region)) {
    context = drag_panel->type->context;
  }

  /* Find how many instanced panels with this context string. */
  int start_index = -1;
  Vector<Panel *> panel_sort;
  for (Panel &panel : region->panels) {
    if (panel.type) {
      if (panel.type->flag & PANEL_TYPE_INSTANCED) {
        if (panel_type_context_poll(region, panel.type, context)) {
          if (&panel == drag_panel) {
            BLI_assert(start_index == -1); /* This panel should only appear once. */
            start_index = panel_sort.size();
          }
          panel_sort.append(&panel);
        }
      }
    }
  }
  BLI_assert(start_index != -1); /* The drag panel should definitely be in the list. */

  /* Sort the matching instanced panels by their display order. */
  std::stable_sort(panel_sort.begin(), panel_sort.end(), [](const Panel *a, const Panel *b) {
    return a->sortorder < b->sortorder;
  });

  /* Find how many of those panels are above this panel. */
  int move_to_index = 0;
  for (; move_to_index < panel_sort.size(); move_to_index++) {
    if (panel_sort[move_to_index] == drag_panel) {
      break;
    }
  }

  if (move_to_index == start_index) {
    /* In this case, the reorder was not changed, so don't do any updates or call the callback. */
    return;
  }

  /* Set the bit to tell the interface to instanced the list. */
  drag_panel->flag |= PNL_INSTANCED_LIST_ORDER_CHANGED;

  CTX_store_set(C, drag_panel->runtime->context);

  /* Finally, move this panel's list item to the new index in its list. */
  drag_panel->type->reorder(C, drag_panel, move_to_index);

  CTX_store_set(C, nullptr);
}

/**
 * Recursive implementation for #panel_set_expansion_from_list_data.
 *
 * \return Whether the closed flag for the panel or any sub-panels changed.
 */
static bool panel_set_expand_from_list_data_recursive(Panel *panel, short flag, short *flag_index)
{
  const bool open = (flag & (1 << *flag_index));
  bool changed = (open == panel_is_closed(panel));

  SET_FLAG_FROM_TEST(panel->flag, !open, PNL_CLOSED);

  for (Panel &child : panel->children) {
    *flag_index = *flag_index + 1;
    changed |= panel_set_expand_from_list_data_recursive(&child, flag, flag_index);
  }
  return changed;
}

/**
 * Set the expansion of the panel and its sub-panels from the flag stored in the
 * corresponding list data. The flag has expansion stored in each bit in depth first order.
 */
static void panel_set_expansion_from_list_data(const bContext *C, Panel *panel)
{
  BLI_assert(panel->type != nullptr);
  BLI_assert(panel->type->flag & PANEL_TYPE_INSTANCED);
  if (panel->type->get_list_data_expand_flag == nullptr) {
    /* Instanced panel doesn't support loading expansion. */
    return;
  }

  const short expand_flag = panel->type->get_list_data_expand_flag(C, panel);
  short flag_index = 0;

  /* Start panel animation if the open state was changed. */
  if (panel_set_expand_from_list_data_recursive(panel, expand_flag, &flag_index)) {
    panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
  }
}

/**
 * Set expansion based on the data for instanced panels.
 */
static void region_panels_set_expansion_from_list_data(const bContext *C, ARegion *region)
{
  for (Panel &panel : region->panels) {
    if (panel.runtime_flag & PANEL_ACTIVE) {
      PanelType *panel_type = panel.type;
      if (panel_type != nullptr && panel.type->flag & PANEL_TYPE_INSTANCED) {
        panel_set_expansion_from_list_data(C, &panel);
      }
    }
  }
}

/**
 * Recursive implementation for #set_panels_list_data_expand_flag.
 */
static void get_panel_expand_flag(const Panel *panel, short *flag, short *flag_index)
{
  const bool open = !(panel->flag & PNL_CLOSED);
  SET_FLAG_FROM_TEST(*flag, open, (1 << *flag_index));

  for (const Panel &child : panel->children) {
    *flag_index = *flag_index + 1;
    get_panel_expand_flag(&child, flag, flag_index);
  }
}

/**
 * Call the callback to store the panel and sub-panel expansion settings in the list item that
 * corresponds to each instanced panel.
 *
 * \note This needs to iterate through all of the region's panels because the panel with changed
 * expansion might have been the sub-panel of an instanced panel, meaning it might not know
 * which list item it corresponds to.
 */
static void set_panels_list_data_expand_flag(const bContext *C, const ARegion *region)
{
  for (Panel &panel : region->panels) {
    PanelType *panel_type = panel.type;
    if (panel_type == nullptr) {
      continue;
    }

    /* Check for #PANEL_ACTIVE so we only set the expand flag for active panels. */
    if (panel_type->flag & PANEL_TYPE_INSTANCED && panel.runtime_flag & PANEL_ACTIVE) {
      short expand_flag;
      short flag_index = 0;
      get_panel_expand_flag(&panel, &expand_flag, &flag_index);
      if (panel.type->set_list_data_expand_flag) {
        panel.type->set_list_data_expand_flag(C, &panel, expand_flag);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panels
 * \{ */

static bool panel_custom_pin_to_last_get(const Panel *panel)
{
  if (panel->type->pin_to_last_property[0] != '\0') {
    PointerRNA *ptr = panel_custom_data_get(panel);
    if (ptr != nullptr && !RNA_pointer_is_null(ptr)) {
      return RNA_boolean_get(ptr, panel->type->pin_to_last_property);
    }
  }

  return false;
}

static void panel_custom_pin_to_last_set(const bContext *C, const Panel *panel, const bool value)
{
  if (panel->type->pin_to_last_property[0] != '\0') {
    PointerRNA *ptr = panel_custom_data_get(panel);
    if (ptr != nullptr && !RNA_pointer_is_null(ptr)) {
      PropertyRNA *prop = RNA_struct_find_property(ptr, panel->type->pin_to_last_property);
      RNA_boolean_set(ptr, panel->type->pin_to_last_property, value);
      RNA_property_update(const_cast<bContext *>(C), ptr, prop);
    }
  }
}

static bool panel_custom_data_active_get(const Panel *panel)
{
  /* The caller should make sure the panel is active and has a type. */
  BLI_assert(panel_is_active(panel));
  BLI_assert(panel->type != nullptr);

  if (panel->type->active_property[0] != '\0') {
    PointerRNA *ptr = panel_custom_data_get(panel);
    if (ptr != nullptr && !RNA_pointer_is_null(ptr)) {
      return RNA_boolean_get(ptr, panel->type->active_property);
    }
  }

  return false;
}

static void panel_custom_data_active_set(Panel *panel)
{
  /* Since the panel is interacted with, it should be active and have a type. */
  BLI_assert(panel_is_active(panel));
  BLI_assert(panel->type != nullptr);

  if (panel->type->active_property[0] != '\0') {
    PointerRNA *ptr = panel_custom_data_get(panel);
    BLI_assert(RNA_struct_find_property(ptr, panel->type->active_property) != nullptr);
    if (ptr != nullptr && !RNA_pointer_is_null(ptr)) {
      RNA_boolean_set(ptr, panel->type->active_property, true);
    }
  }
}

/**
 * Set flag state for a panel and its sub-panels.
 */
static void panel_set_flag_recursive(Panel *panel, short flag, bool value)
{
  SET_FLAG_FROM_TEST(panel->flag, value, flag);

  for (Panel &child : panel->children) {
    panel_set_flag_recursive(&child, flag, value);
  }
}

/**
 * Set runtime flag state for a panel and its sub-panels.
 */
static void panel_set_runtime_flag_recursive(Panel *panel, short flag, bool value)
{
  SET_FLAG_FROM_TEST(panel->runtime_flag, value, flag);

  for (Panel &sub_panel : panel->children) {
    panel_set_runtime_flag_recursive(&sub_panel, flag, value);
  }
}

static void panels_collapse_all(ARegion *region, const Panel *from_panel)
{
  const bool has_category = panel_category_is_visible(region);
  const char *category = has_category ? panel_category_active_get(region, false) : nullptr;
  const PanelType *from_pt = from_panel->type;

  for (Panel &panel : region->panels) {
    PanelType *pt = panel.type;

    /* Close panels with headers in the same context. */
    if (pt && from_pt && !(pt->flag & PANEL_TYPE_NO_HEADER)) {
      if (!pt->context[0] || !from_pt->context[0] || STREQ(pt->context, from_pt->context)) {
        if ((panel.flag & PNL_PIN) || !category || !pt->category[0] ||
            STREQ(pt->category, category))
        {
          panel.flag |= PNL_CLOSED;
        }
      }
    }
  }
}

static bool panel_type_context_poll(ARegion *region,
                                    const PanelType *panel_type,
                                    const char *context)
{
  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    return STREQ(panel_type->category, panel_category_active_get(region, false));
  }

  if (panel_type->context[0] && STREQ(panel_type->context, context)) {
    return true;
  }

  return false;
}

Panel *panel_find_by_type(ListBaseT<Panel> *lb, const PanelType *pt)
{
  const char *idname = pt->idname;

  for (Panel &panel : *lb) {
    if (STREQLEN(panel.panelname, idname, sizeof(panel.panelname))) {
      return &panel;
    }
  }
  return nullptr;
}

Panel *panel_begin(
    ARegion *region, ListBaseT<Panel> *lb, Block *block, PanelType *pt, Panel *panel, bool *r_open)
{
  Panel *panel_last;
  const char *drawname = CTX_IFACE_(pt->translation_context, pt->label);
  const bool newpanel = (panel == nullptr);

  if (newpanel) {
    panel = BKE_panel_new(pt);

    if (pt->flag & PANEL_TYPE_DEFAULT_CLOSED) {
      panel->flag |= PNL_CLOSED;
      panel->runtime_flag |= PANEL_WAS_CLOSED;
    }

    panel->ofsx = 0;
    panel->ofsy = 0;
    panel->sizex = 0;
    panel->sizey = 0;
    panel->blocksizex = 0;
    panel->blocksizey = 0;
    panel->runtime_flag |= PANEL_NEW_ADDED;

    BLI_addtail(lb, panel);
  }
  else {
    /* Panel already exists. */
    panel->type = pt;
  }

  panel->runtime->block = block;

  panel_drawname_set(panel, drawname);

  /* If a new panel is added, we insert it right after the panel that was last added.
   * This way new panels are inserted in the right place between versions. */
  for (panel_last = static_cast<Panel *>(lb->first); panel_last; panel_last = panel_last->next) {
    if (panel_last->runtime_flag & PANEL_LAST_ADDED) {
      BLI_remlink(lb, panel);
      BLI_insertlinkafter(lb, panel_last, panel);
      break;
    }
  }

  if (newpanel) {
    panel->sortorder = (panel_last) ? panel_last->sortorder + 1 : 0;

    for (Panel &panel_next : *lb) {
      if (&panel_next != panel && panel_next.sortorder >= panel->sortorder) {
        panel_next.sortorder++;
      }
    }
  }

  if (panel_last) {
    panel_last->runtime_flag &= ~PANEL_LAST_ADDED;
  }

  /* Assign the new panel to the block. */
  block->panel = panel;
  panel->runtime_flag |= PANEL_ACTIVE | PANEL_LAST_ADDED;
  if (region->alignment == RGN_ALIGN_FLOAT) {
    block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);
  }

  *r_open = !panel_is_closed(panel);

  return panel;
}

void panel_header_buttons_begin(Panel *panel)
{
  Block *block = panel->runtime->block;

  block_new_button_group(block, UI_BUTTON_GROUP_LOCK | UI_BUTTON_GROUP_PANEL_HEADER);
}

void panel_header_buttons_end(Panel *panel)
{
  Block *block = panel->runtime->block;

  /* A button group should always be created in #panel_header_buttons_begin. */
  BLI_assert(!block->button_groups.is_empty());

  ButtonGroup &button_group = block->button_groups.last();
  button_group.flag &= ~UI_BUTTON_GROUP_LOCK;

  /* Repurpose the first header button group if it is empty, in case the first button added to
   * the panel doesn't add a new group (if the button is created directly rather than through an
   * interface layout call). */
  if (block->button_groups.size() == 1 && button_group.buttons.is_empty()) {
    button_group.flag &= ~UI_BUTTON_GROUP_PANEL_HEADER;
  }
  else {
    /* Always add a new button group. Although this may result in many empty groups, without it,
     * new buttons in the panel body not protected with a #block_new_button_group call would
     * end up in the panel header group. */
    block_new_button_group(block, ButtonGroupFlag(0));
  }
}

static float panel_region_offset_x_get(const ARegion *region)
{
  if (panel_category_tabs_is_visible(region)) {
    if (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT) {
      return UI_PANEL_CATEGORY_MARGIN_WIDTH;
    }
  }

  return 0.0f;
}

/**
 * Starting from the "block size" set in #panel_end, calculate the full size
 * of the panel including the sub-panel headers and buttons.
 */
static void panel_calculate_size_recursive(ARegion *region, Panel *panel)
{
  int width = panel->blocksizex;
  int height = panel->blocksizey;

  for (Panel &child_panel : panel->children) {
    if (child_panel.runtime_flag & PANEL_ACTIVE) {
      panel_calculate_size_recursive(region, &child_panel);
      width = max_ii(width, child_panel.sizex);
      height += get_panel_real_size_y(&child_panel);
    }
  }

  /* Update total panel size. */
  if (panel->runtime_flag & PANEL_NEW_ADDED) {
    panel->runtime_flag &= ~PANEL_NEW_ADDED;
    panel->sizex = width;
    panel->sizey = height;
  }
  else {
    const int old_sizex = panel->sizex, old_sizey = panel->sizey;
    const int old_region_ofsx = panel->runtime->region_ofsx;

    /* Update width/height if non-zero. */
    if (width != 0) {
      panel->sizex = width;
    }
    if (height != 0 || !panel_is_closed(panel)) {
      panel->sizey = height;
    }

    /* Check if we need to do an animation. */
    if (panel->sizex != old_sizex || panel->sizey != old_sizey) {
      panel->runtime_flag |= PANEL_ANIM_ALIGN;
      panel->ofsy += old_sizey - panel->sizey;
    }

    panel->runtime->region_ofsx = panel_region_offset_x_get(region);
    if (old_region_ofsx != panel->runtime->region_ofsx) {
      panel->runtime_flag |= PANEL_ANIM_ALIGN;
    }
  }
}

void panel_end(Panel *panel, int width, int height)
{
  /* Store the size of the buttons layout in the panel. The actual panel size
   * (including sub-panels) is calculated in #panels_end. */
  panel->blocksizex = width;
  panel->blocksizey = height;
}

void panel_drawname_set(Panel *panel, StringRef name)
{
  MEM_SAFE_DELETE(panel->drawname);
  panel->drawname = BLI_strdupn(name.data(), name.size());
}

static void offset_panel_block(Block *block)
{
  const uiStyle *style = style_get_dpi();

  /* Compute bounds and offset. */
  block_bounds_calc(block);

  const int panels_space = style->panelspace;
  const int ofsy = block->panel->sizey - panels_space;

  for (Button &but : block->buttons()) {
    but.rect.ymin += ofsy;
    but.rect.ymax += ofsy;
  }
  for (LayoutPanelBody &body : block->panel->runtime->layout_panels.bodies) {
    body.start_y -= panels_space;
    body.end_y -= panels_space;
  }
  for (LayoutPanelHeader &header : block->panel->runtime->layout_panels.headers) {
    header.start_y -= panels_space;
    header.end_y -= panels_space;
  }

  block->rect.xmax = block->panel->sizex;
  block->rect.ymax = block->panel->sizey;
  block->rect.xmin = block->rect.ymin = 0.0;
}

void panel_tag_search_filter_match(Panel *panel)
{
  panel->runtime_flag |= PANEL_SEARCH_FILTER_MATCH;
}

static void panel_matches_search_filter_recursive(const Panel *panel, bool *filter_matches)
{
  *filter_matches |= bool(panel->runtime_flag & PANEL_SEARCH_FILTER_MATCH);

  /* If the panel has no match we need to make sure that its children are too. */
  if (!*filter_matches) {
    for (const Panel &child_panel : panel->children) {
      panel_matches_search_filter_recursive(&child_panel, filter_matches);
    }
  }
}

bool panel_matches_search_filter(const Panel *panel)
{
  bool search_filter_matches = false;
  panel_matches_search_filter_recursive(panel, &search_filter_matches);
  return search_filter_matches;
}

/**
 * Set the flag telling the panel to use its search result status for its expansion.
 */
static void panel_set_expansion_from_search_filter_recursive(const bContext *C,
                                                             Panel *panel,
                                                             const bool use_search_closed)
{
  /* This has to run on inactive panels that may not have a type,
   * but we can prevent running on header-less panels in some cases. */
  if (panel->type == nullptr || !(panel->type->flag & PANEL_TYPE_NO_HEADER)) {
    SET_FLAG_FROM_TEST(panel->runtime_flag, use_search_closed, PANEL_USE_CLOSED_FROM_SEARCH);
  }

  for (Panel &child_panel : panel->children) {
    /* Don't check if the sub-panel is active, otherwise the
     * expansion won't be reset when the parent is closed. */
    panel_set_expansion_from_search_filter_recursive(C, &child_panel, use_search_closed);
  }
}

/**
 * Set the flag telling every panel to override its expansion with its search result status.
 */
static void region_panels_set_expansion_from_search_filter(const bContext *C,
                                                           ARegion *region,
                                                           const bool use_search_closed)
{
  for (Panel &panel : region->panels) {
    /* Don't check if the panel is active, otherwise the expansion won't
     * be correct when switching back to tab after exiting search. */
    panel_set_expansion_from_search_filter_recursive(C, &panel, use_search_closed);
  }
  set_panels_list_data_expand_flag(C, region);
}

/**
 * Hide buttons in invisible layouts, which are created because buttons must be
 * added for all panels in order to search, even panels that will end up closed.
 */
static void panel_remove_invisible_layouts_recursive(Panel *panel, const Panel *parent_panel)
{
  Block *block = panel->runtime->block;
  BLI_assert(block != nullptr);
  BLI_assert(block->active);
  if (parent_panel != nullptr && panel_is_closed(parent_panel)) {
    /* The parent panel is closed, so this panel can be completely removed. */
    block_set_search_only(block, true);
    for (Button &but : block->buttons()) {
      but.flag |= UI_HIDDEN;
    }
  }
  else if (panel_is_closed(panel)) {
    /* If sub-panels have no search results but the parent panel does, then the parent panel open
     * and the sub-panels will close. In that case there must be a way to hide the buttons in the
     * panel but keep the header buttons. */
    for (const ButtonGroup &button_group : block->button_groups) {
      if (button_group.flag & UI_BUTTON_GROUP_PANEL_HEADER) {
        continue;
      }
      for (Button *but : button_group.buttons) {
        but->flag |= UI_HIDDEN;
      }
    }
  }

  for (Panel &child_panel : panel->children) {
    if (child_panel.runtime_flag & PANEL_ACTIVE) {
      BLI_assert(child_panel.runtime->block != nullptr);
      panel_remove_invisible_layouts_recursive(&child_panel, panel);
    }
  }
}

static void region_panels_remove_invisible_layouts(ARegion *region)
{
  for (Panel &panel : region->panels) {
    if (panel.runtime_flag & PANEL_ACTIVE) {
      BLI_assert(panel.runtime->block != nullptr);
      panel_remove_invisible_layouts_recursive(&panel, nullptr);
    }
  }
}

bool panel_is_closed(const Panel *panel)
{
  /* Header-less panels can never be closed, otherwise they could disappear. */
  if (panel->type && panel->type->flag & PANEL_TYPE_NO_HEADER) {
    return false;
  }

  if (panel->runtime_flag & PANEL_USE_CLOSED_FROM_SEARCH) {
    return !panel_matches_search_filter(panel);
  }

  return panel->flag & PNL_CLOSED;
}

bool panel_is_active(const Panel *panel)
{
  return panel->runtime_flag & PANEL_ACTIVE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

void panels_draw(const bContext *C, ARegion *region)
{
  /* Draw in reverse order, because #Blocks are added in reverse order
   * and we need child panels to draw on top. */
  for (Block &block : region->runtime->uiblocks.items_reversed()) {
    if (block.active && block.panel && !panel_is_dragging(block.panel) &&
        !block_is_search_only(&block))
    {
      block_draw(C, &block);
    }
  }

  for (Block &block : region->runtime->uiblocks.items_reversed()) {
    if (block.active && block.panel && panel_is_dragging(block.panel) &&
        !block_is_search_only(&block))
    {
      block_draw(C, &block);
    }
  }
}

#define PNL_ICON UI_UNIT_X /* Could be UI_UNIT_Y too. */

void panel_label_offset(const Block *block, int *r_x, int *r_y)
{
  Panel *panel = block->panel;
  const bool is_subpanel = (panel->type && panel->type->parent);

  *r_x = UI_UNIT_X * 1.0f;
  *r_y = UI_UNIT_Y * 1.5f;

  if (is_subpanel) {
    *r_x += (0.7f * UI_UNIT_X);
  }
}

static void panel_title_color_get(const Panel *panel,
                                  const bool show_background,
                                  const bool region_search_filter_active,
                                  uchar r_color[4])
{
  if (!show_background) {
    /* Use menu colors for floating panels. */
    bTheme *btheme = theme::theme_get();
    const uiWidgetColors *wcol = &btheme->tui.wcol_menu_back;
    copy_v4_v4_uchar(r_color, static_cast<const uchar *>(wcol->text));
    return;
  }

  const bool search_match = panel_matches_search_filter(panel);

  theme::get_color_4ubv(TH_TITLE, r_color);
  if (region_search_filter_active && !search_match) {
    r_color[0] *= 0.5;
    r_color[1] *= 0.5;
    r_color[2] *= 0.5;
  }
}

static void panel_draw_border(const Panel *panel,
                              const rcti *rect,
                              const rcti *header_rect,
                              const bool is_active)
{
  const bool is_subpanel = panel->type->parent != nullptr;
  if (is_subpanel) {
    return;
  }

  float color[4];
  theme::get_color_4fv(is_active ? TH_PANEL_ACTIVE : TH_PANEL_OUTLINE, color);
  if (color[3] == 0.0f) {
    return; /* No border to draw. */
  }

  const bTheme *btheme = theme::theme_get();
  const float aspect = panel->runtime->block->aspect;
  const float radius = (btheme->tui.panel_roundness * U.widget_unit * 0.5f) / aspect;
  draw_roundbox_corner_set(CNR_ALL);

  rctf box_rect;
  box_rect.xmin = rect->xmin;
  box_rect.xmax = rect->xmax;
  box_rect.ymin = panel_is_closed(panel) ? header_rect->ymin : rect->ymin;
  box_rect.ymax = header_rect->ymax;
  draw_roundbox_4fv(&box_rect, false, radius, color);
}

static void panel_draw_aligned_widgets(const uiStyle *style,
                                       const Panel *panel,
                                       const rcti *header_rect,
                                       const float aspect,
                                       const bool show_pin,
                                       const bool show_background,
                                       const bool region_search_filter_active)
{
  const bool is_subpanel = panel->type->parent != nullptr;
  const uiFontStyle *fontstyle = (is_subpanel) ? &style->widget : &style->paneltitle;

  const int header_height = BLI_rcti_size_y(header_rect);
  const int header_width = BLI_rcti_size_x(header_rect);
  const int scaled_unit = round_fl_to_int(UI_UNIT_X / aspect);

  /* Offset triangle and text to the right for sub-panels. */
  rcti widget_rect;
  widget_rect.xmin = header_rect->xmin + (is_subpanel ? scaled_unit * 0.7f : 0);
  widget_rect.xmax = header_rect->xmax;
  widget_rect.ymin = header_rect->ymin;
  widget_rect.ymax = header_rect->ymax;

  uchar title_color[4];
  panel_title_color_get(panel, show_background, region_search_filter_active, title_color);
  title_color[3] = 255;

  /* Draw collapse icon. */
  {
    const float size_y = BLI_rcti_size_y(&widget_rect);
    GPU_blend(GPU_BLEND_ALPHA);
    float alpha = 0.8f;
    /* Dim as its space is reduced to zero. */
    if (header_width < (scaled_unit * 4)) {
      alpha *= std::max(float(header_width - scaled_unit) / float(scaled_unit * 3), 0.0f);
    }
    icon_draw_ex(widget_rect.xmin + size_y * 0.2f,
                 widget_rect.ymin + size_y * (panel_is_closed(panel) ? 0.17f : 0.14f),
                 panel_is_closed(panel) ? ICON_RIGHTARROW : ICON_DOWNARROW_HLT,
                 aspect * UI_INV_SCALE_FAC,
                 alpha,
                 0.0f,
                 title_color,
                 false,
                 UI_NO_ICON_OVERLAY_TEXT);
    GPU_blend(GPU_BLEND_NONE);
  }

  /* Draw text label. */
  if (panel->drawname && panel->drawname[0] != '\0') {
    rcti title_rect;
    title_rect.xmin = widget_rect.xmin + (panel->labelofs / aspect) + scaled_unit * 1.1f;
    title_rect.xmax = widget_rect.xmax;
    if (!is_subpanel && show_background) {
      /* Don't draw over the drag widget. */
      title_rect.xmax -= scaled_unit;
    }
    title_rect.ymin = widget_rect.ymin - 2.0f / aspect;
    title_rect.ymax = widget_rect.ymax;

    FontStyleDrawParams params{};
    params.align = UI_STYLE_TEXT_LEFT;
    fontstyle_draw(
        fontstyle, &title_rect, panel->drawname, strlen(panel->drawname), title_color, &params);
  }

  /* Draw the pin icon. */
  if (show_pin && (panel->flag & PNL_PIN)) {
    GPU_blend(GPU_BLEND_ALPHA);
    icon_draw_ex(widget_rect.xmax - scaled_unit * 2.2f,
                 widget_rect.ymin + 5.0f / aspect,
                 ICON_PINNED,
                 aspect * UI_INV_SCALE_FAC,
                 1.0f,
                 0.0f,
                 title_color,
                 false,
                 UI_NO_ICON_OVERLAY_TEXT);
    GPU_blend(GPU_BLEND_NONE);
  }

  /* Draw drag widget. */
  if (!is_subpanel && show_background) {
    const float x = widget_rect.xmax - scaled_unit * 1.15;
    const float y = widget_rect.ymin + (header_height - (header_height * 0.7f)) * 0.5f;
    const bool is_pin = panel_custom_pin_to_last_get(panel);
    const int icon = is_pin ? ICON_PINNED : ICON_GRIP;
    const float size = aspect * UI_INV_SCALE_FAC;
    float alpha = is_pin ? 1.0f : 0.5f;
    if (header_width < (scaled_unit * 5)) {
      alpha *= std::max((header_width - scaled_unit) / float(scaled_unit * 4), 0.0f);
    }
    icon_draw_ex(x, y, icon, size, alpha, 0.0f, title_color, false, UI_NO_ICON_OVERLAY_TEXT);
  }
}

void draw_layout_panels_backdrop(const ARegion *region,
                                 const Panel *panel,
                                 const float radius,
                                 float subpanel_backcolor[4])
{
  /* Draw backdrops for layout panels. */
  const float aspect = block_is_popup_any(panel->runtime->block) ? panel->runtime->block->aspect :
                                                                   1.0f;

  for (const LayoutPanelBody &body : panel->runtime->layout_panels.bodies) {

    rctf panel_blockspace = panel->runtime->block->rect;
    panel_blockspace.ymax = panel->runtime->block->rect.ymax + body.end_y;
    panel_blockspace.ymin = panel->runtime->block->rect.ymax + body.start_y;

    if (panel_blockspace.ymax <= panel->runtime->block->rect.ymin) {
      /* Layout panels no longer fits in block rectangle, stop drawing backdrops. */
      break;
    }
    if (panel_blockspace.ymin >= panel->runtime->block->rect.ymax) {
      /* Skip layout panels that scrolled to the top of the block rectangle. */
      continue;
    }
    /* If the layout panel is at the end of the root panel, it's bottom corners are rounded. */
    const bool is_main_panel_end = panel_blockspace.ymin - panel->runtime->block->rect.ymin <
                                   (10.0f * UI_SCALE_FAC / aspect);
    if (is_main_panel_end) {
      panel_blockspace.ymin = panel->runtime->block->rect.ymin;
      draw_roundbox_corner_set(CNR_BOTTOM_RIGHT | CNR_BOTTOM_LEFT);
    }
    else {
      draw_roundbox_corner_set(CNR_NONE);
    }
    panel_blockspace.ymax = std::min(panel_blockspace.ymax, panel->runtime->block->rect.ymax);

    rcti panel_pixelspace = rect_to_pixelrect(region, panel->runtime->block, &panel_blockspace);
    rctf panel_pixelspacef;
    BLI_rctf_rcti_copy(&panel_pixelspacef, &panel_pixelspace);
    draw_roundbox_4fv(&panel_pixelspacef, true, radius, subpanel_backcolor);
  }
}

static void panel_draw_softshadow(const rctf *box_rect,
                                  const int roundboxalign,
                                  const float radius,
                                  const float shadow_width)
{
  const float outline = U.pixelsize;

  rctf shadow_rect = *box_rect;
  BLI_rctf_pad(&shadow_rect, -outline, -outline);
  draw_roundbox_corner_set(roundboxalign);

  const float shadow_alpha = theme::theme_get()->tui.menu_shadow_fac;
  draw_dropshadow(&shadow_rect, radius, shadow_width, 1.0f, shadow_alpha);
}

static void panel_draw_aligned_backdrop(const ARegion *region,
                                        const Panel *panel,
                                        const rcti *rect,
                                        const rcti *header_rect)
{
  const bool is_open = !panel_is_closed(panel);
  const bool is_subpanel = panel->type->parent != nullptr;
  const bool has_header = (panel->type->flag & PANEL_TYPE_NO_HEADER) == 0;
  const bool is_dragging = panel_is_dragging(panel);

  if (is_subpanel && !is_open) {
    return;
  }

  const bTheme *btheme = theme::theme_get();
  const float aspect = panel->runtime->block->aspect;
  const float radius = btheme->tui.panel_roundness * U.widget_unit * 0.5f / aspect;

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_blend(GPU_BLEND_ALPHA);

  /* Draw shadow on top-level panels with headers during drag or region overlap. */
  if (!is_subpanel && has_header && (region->overlap || is_dragging)) {
    /* Make shadow wider (at least 16px) while the panel is being dragged. */
    const float shadow_width = is_dragging ? max_ii(int(16.0f * UI_SCALE_FAC),
                                                    theme::get_menu_shadow_width()) :
                                             theme::get_menu_shadow_width();
    const int roundboxalign = is_open ? CNR_BOTTOM_RIGHT | CNR_BOTTOM_LEFT : CNR_ALL;

    rctf box_rect;
    box_rect.xmin = rect->xmin;
    box_rect.xmax = rect->xmax;
    box_rect.ymin = is_open ? rect->ymin : header_rect->ymin;
    box_rect.ymax = header_rect->ymax;
    panel_draw_softshadow(&box_rect, roundboxalign, radius, shadow_width);
  }

  /* Panel backdrop. */
  if (is_open || !has_header) {
    float panel_backcolor[4];
    draw_roundbox_corner_set(is_open ? CNR_BOTTOM_RIGHT | CNR_BOTTOM_LEFT : CNR_ALL);
    if (!has_header) {
      theme::get_color_4fv(TH_BACK, panel_backcolor);
    }
    else {
      theme::get_color_4fv((is_subpanel ? TH_PANEL_SUB_BACK : TH_PANEL_BACK), panel_backcolor);
    }

    rctf box_rect;
    const float padding = is_subpanel ? U.widget_unit * 0.1f / aspect : 0.0f;
    box_rect.xmin = rect->xmin + padding;
    box_rect.xmax = rect->xmax - padding;
    box_rect.ymin = rect->ymin + padding;
    box_rect.ymax = rect->ymax;
    draw_roundbox_4fv(&box_rect, true, radius, panel_backcolor);

    float subpanel_backcolor[4];
    theme::get_color_4fv(TH_PANEL_SUB_BACK, subpanel_backcolor);
    draw_layout_panels_backdrop(region, panel, radius, subpanel_backcolor);
  }

  /* Panel header backdrops for non sub-panels. */
  if (!is_subpanel && has_header) {
    float panel_headercolor[4];
    theme::get_color_4fv(panel_matches_search_filter(panel) ? TH_MATCH : TH_PANEL_HEADER,
                         panel_headercolor);
    draw_roundbox_corner_set(is_open ? CNR_TOP_RIGHT | CNR_TOP_LEFT : CNR_ALL);

    /* Change the width a little bit to line up with the sides. */
    rctf box_rect;
    box_rect.xmin = rect->xmin;
    box_rect.xmax = rect->xmax;
    box_rect.ymin = header_rect->ymin;
    box_rect.ymax = header_rect->ymax;
    draw_roundbox_4fv(&box_rect, true, radius, panel_headercolor);
  }

  GPU_blend(GPU_BLEND_NONE);
  immUnbindProgram();
}

void draw_aligned_panel(const ARegion *region,
                        const uiStyle *style,
                        const Block *block,
                        const rcti *rect,
                        const bool show_pin,
                        const bool show_background,
                        const bool region_search_filter_active)
{
  const Panel *panel = block->panel;

  if (panel->sizex < 0 || panel->sizey < 0) {
    /* Nothing to draw. */
    return;
  }

  /* Add 0.001f to prevent flicker from float inaccuracy. */
  const rcti header_rect = {
      rect->xmin,
      rect->xmax,
      rect->ymax,
      rect->ymax + int(floor(PNL_HEADER / block->aspect + 0.001f)),
  };

  if (show_background || (panel->type->flag & PANEL_TYPE_NO_HEADER)) {
    panel_draw_aligned_backdrop(region, panel, rect, &header_rect);
  }

  /* Draw the widgets and text in the panel header. */
  if (!(panel->type->flag & PANEL_TYPE_NO_HEADER)) {
    panel_draw_aligned_widgets(style,
                               panel,
                               &header_rect,
                               block->aspect,
                               show_pin,
                               show_background,
                               region_search_filter_active);
  }

  /* Draw the panel outline on non-transparent panels. */
  if (panel_should_show_background(region, panel->type)) {
    panel_draw_border(panel, rect, &header_rect, panel_custom_data_active_get(panel));
  }
}

bool panel_should_show_background(const ARegion *region, const PanelType *panel_type)
{
  if (region->alignment == RGN_ALIGN_FLOAT) {
    return false;
  }

  if (panel_type && panel_type->flag & PANEL_TYPE_NO_HEADER) {
    if (region->regiontype == RGN_TYPE_TOOLS) {
      /* We never want a background around active tools. */
      return false;
    }
    /* Without a header there is no background except for region overlap. */
    return region->overlap != 0;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Drawing (Tabs)
 * \{ */

#define TABS_PADDING_BETWEEN_FACTOR 4.0f
#define TABS_PADDING_TEXT_FACTOR 6.0f
#define TABS_GLYPH_TEXT_GAP_FACTOR 6.0f
#define TABS_SETTINGS_ICON "\ue5d3"  /* Material Symbols: settings/gear \ue5c5   \ue5d3           */

/* Glyph darkening factor for inactive tabs (0.0 = no change, 1.0 = black). */
#define TABS_GLYPH_DARKEN_BASE 0.15f    /* Darkening for inactive tabs without hover. */

/* Tab background brightening factors for inactive tabs (0.0 = no change, 1.0 = white). */
#define TABS_BG_BRIGHTEN_BASE 0.0f        /* Base brightening for inactive tab background (disabled by default). */
#define TABS_BG_BRIGHTEN_HOVER 0.05f       /* Brightening when mouse hovers over inactive tab (disabled by default). */ 

static void panel_category_tabs_draw_settings_button(const bContext *C,
                                                      ARegion *region,
                                                      const float zoom,
                                                      const uchar theme_col_tab_text[3])
{
  const uiStyle *style = style_get();
  const int fontid = style->widget.uifont_id;

  /* Use pre-calculated rect (with scroll already applied). */
  const rcti *rct = &region->runtime->category_tabs_settings_rect;

  /* Validate hover state - check if mouse is actually over the button. */
  bool is_hover = region->runtime->category_tabs_settings_hover;
  wmWindow *win = CTX_wm_window(C);

  const double current_time = BLI_time_now_seconds();
  const double time_since_click = current_time - region->runtime->category_tabs_settings_click_time;
  const double time_since_hover = current_time - region->runtime->category_tabs_settings_hover_time;

  /* Check if drag is active - if so, don't set hover. */
  const bool drag_active = (region->runtime->category_tabs_drag_state != nullptr);
  const bool drag_pending = (region->runtime->category_tabs_drag_pending_id[0] != '\0');

  /* Auto-reset hover after 2 seconds if mouse is not over button.
   * This ensures the button doesn't stay highlighted forever when mouse leaves
   * and no redraw events are triggered.
   * Note: The area-level hover handler also resets hover when mouse moves
   * between regions, so this timeout is a fallback safety mechanism.
   */
  const bool hover_timeout_expired = time_since_hover > 2.0;
  UNUSED_VARS(time_since_click);

  if (win && win->runtime->eventstate) {
    /* Check if mouse is in this region and over the button. */
    const int mx = win->runtime->eventstate->xy[0] - region->winrct.xmin;
    const int my = win->runtime->eventstate->xy[1] - region->winrct.ymin;
    const bool mouse_in_region = BLI_rcti_isect_pt(&region->winrct,
                                                   win->runtime->eventstate->xy[0],
                                                   win->runtime->eventstate->xy[1]);
    const bool actually_over = mouse_in_region && BLI_rcti_isect_pt(rct, mx, my);

    /* Update hover state:
     * - If mouse is over button AND drag not active: set hover true, update hover_time
     * - If mouse is NOT over button AND hover_timeout expired: reset hover false
     * - If drag is active: reset hover false
     * - Otherwise: keep current hover state (popup might be open)
     */
    if (drag_active || drag_pending) {
      /* Drag is active - don't show hover. */
      is_hover = false;
    }
    else if (actually_over) {
      if (!is_hover) {
        /* Hover just became true - update timestamp. */
        region->runtime->category_tabs_settings_hover_time = current_time;
      }
      is_hover = true;
    }
    else if (hover_timeout_expired) {
      /* Timeout expired - reset hover regardless of popup state. */
      is_hover = false;
    }

    /* Update stored hover state. */
    region->runtime->category_tabs_settings_hover = is_hover;
  }

  /* Draw the button background (same style as inactive tab, or active when hovered). */
  bTheme *btheme = theme::theme_get();
  const float tab_curve_radius = btheme->tui.wcol_tab.roundness * U.widget_unit * zoom;
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  const int roundboxtype = region->overlap ? CNR_ALL :
                                             (is_left ? (CNR_TOP_LEFT | CNR_BOTTOM_LEFT) :
                                                        (CNR_TOP_RIGHT | CNR_BOTTOM_RIGHT));

  float theme_col_tab_bg[4];
  float theme_col_tab_outline[4];
  if (is_hover) {
    /* Use active tab colors when hovered. */
    theme::get_color_4fv(TH_TAB_ACTIVE, theme_col_tab_bg);
  }
  else {
    /* Use inactive tab colors normally. */
    theme::get_color_4fv(TH_TAB_INACTIVE, theme_col_tab_bg);
  }
  theme::get_color_4fv(TH_TAB_OUTLINE, theme_col_tab_outline);

  GPU_blend(GPU_BLEND_ALPHA);

  rctf box_rect;
  box_rect.xmin = float(rct->xmin);
  box_rect.xmax = float(rct->xmax);
  box_rect.ymin = float(rct->ymin);
  box_rect.ymax = float(rct->ymax);

  draw_roundbox_corner_set(roundboxtype);
  draw_roundbox_4fv(&box_rect, true, tab_curve_radius, theme_col_tab_bg);
  draw_roundbox_4fv(&box_rect, false, tab_curve_radius, theme_col_tab_outline);

  /* Draw the settings icon. */
  const float glyph_width = BLF_width(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX);
  const int ascender_i = BLF_ascender(fontid);
  const int descender_i = BLF_descender(fontid);
  const float ascender = float(ascender_i);
  const float descender = float(descender_i);
  const float glyph_height = ascender - descender;

  const float tab_center_x = float(rct->xmin + rct->xmax) * 0.5f;
  const float tab_center_y = float(rct->ymin + rct->ymax) * 0.5f;

  float pos_x = tab_center_x - glyph_width * 0.5f;
  float pos_y = tab_center_y - glyph_height * 0.5f - descender;

  /* Use highlighted text color when hovered. */
  uchar theme_col_text_hi[3];
  theme::get_color_3ubv(TH_TAB_TEXT_HI, theme_col_text_hi);

  BLF_disable(fontid, BLF_ROTATION);
  BLF_position(fontid, pos_x, pos_y, 0.0f);
  BLF_color3ubv(fontid, is_hover ? theme_col_text_hi : theme_col_tab_text);
  BLF_draw(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX);

  GPU_blend(GPU_BLEND_NONE);
}

/**
 * Lookup glyph for a category using priority chain:
 * 1. User override
 * 2. PanelType.icon_glyph
 * 3. Global mapping
 * 4. First character fallback
 */
/* Default glyph mappings - Material Symbols font */
static const struct {
  const char *category;
  const char *glyph;
} default_glyph_mappings[] = {
    {"Item", "\ue8f4"},       /* visibility */
    {"View", "\ue417"},       /* visibility */
    {"Edit", "\ue3c9"},       /* edit */
    {"Tool", "\ue166"},       /* construction */
    {"Asset", "\ue2c7"},      /* folder */
    {"Options", "\ue8b8"},    /* settings */
    {"Animation", "\ue71b"},  /* motion_photos_on */
    {"Physics", "\ue3d4"},    /* science */
    {"World", "\ue88e"},      /* public */
    {"Material", "\ue429"},   /* palette */
    {"Modifiers", "\ue429"},  /* palette */
    {"Texture", "\ue40a"},    /* texture */
    {"Particles", "\ue3d4"},  /* science */
    {"Curve", "\ue148"},      /* timeline */
    {"Mesh", "\ue204"},       /* category */
    {"Object", "\ue8d4"},     /* select_all */
    {"Scene", "\ue8f9"},      /* dashboard */
    {"Render", "\ue439"},     /* photo_camera */
    {"Script", "\ue86f"},     /* terminal */
    {"Sound", "\ue3a1"},      /* speaker */
    {"Surface", "\ue76c"},    /* waves */
    {"Volume", "\ue2c8"},     /* folder_open */
    {"Constraints", "\ue8d2"}, /* rule */
    {"Data", "\ue23e"},       /* database */
    {"Node", "\ue1b8"},       /* account_tree */
    {nullptr, nullptr},
};

/* Lookup glyph in static default mappings */
static const char *lookup_default_glyph(const char *category)
{
  for (int i = 0; default_glyph_mappings[i].category != nullptr; i++) {
    if (STREQ(default_glyph_mappings[i].category, category)) {
      return default_glyph_mappings[i].glyph;
    }
  }
  return nullptr;
}

/**
 * Check if a ListBase containing CategoryGlyphItem appears to be valid.
 * After file read, the list may contain garbage pointers from the old file's memory space.
 * We check if the first item's prev pointer is null (as expected for first item).
 */
static bool category_glyph_list_is_valid(const ListBase *list)
{
  if (list == nullptr || list->first == nullptr) {
    return true; /* Empty list is valid. */
  }

  const CategoryGlyphItem *first = static_cast<const CategoryGlyphItem *>(list->first);

  /* First item should have prev == nullptr. If not, list is corrupted. */
  if (first->prev != nullptr) {
    return false;
  }

  /* Check for obviously invalid next pointer (like -1 which is 0xFFFFFFFF...). */
  if (first->next == reinterpret_cast<const void *>(static_cast<intptr_t>(-1))) {
    return false;
  }

  return true;
}

/**
 * Check if a ListBase containing WorkspaceCategoryOrder appears to be valid.
 * After file read, the list may contain garbage pointers from the old file's memory space.
 */
static bool workspace_category_order_list_is_valid(const ListBase *list)
{
  if (list == nullptr || list->first == nullptr) {
    return true; /* Empty list is valid. */
  }

  const WorkspaceCategoryOrder *first = static_cast<const WorkspaceCategoryOrder *>(list->first);

  /* First item should have prev == nullptr. If not, list is corrupted. */
  if (first->prev != nullptr) {
    return false;
  }

  /* Check for obviously invalid next pointer (like -1 which is 0xFFFFFFFF...). */
  if (first->next == reinterpret_cast<const void *>(static_cast<intptr_t>(-1))) {
    return false;
  }

  return true;
}

static bool category_tag_list_is_valid(const ListBase *list)
{
  if (list == nullptr || list->first == nullptr) {
    return true;
  }

  const CategoryTagDef *first = static_cast<const CategoryTagDef *>(list->first);
  if (first->prev != nullptr) {
    return false;
  }
  if (first->next == reinterpret_cast<const void *>(static_cast<intptr_t>(-1))) {
    return false;
  }
  return true;
}

static const char *category_tags_string_lookup(const wmWindowManager *wm, const char *category)
{
  if (wm == nullptr || category == nullptr) {
    return "";
  }

  if (category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        return item->tags;
      }
    }
  }

  if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        return item->tags;
      }
    }
  }
  return "";
  return "";
}

static bool category_has_tag(const char *tags_string, const char *tag_name)
{
  if (tags_string == nullptr || tags_string[0] == '\0') {
    return false;
  }
  if (tag_name == nullptr || tag_name[0] == '\0') {
    return false;
  }

  const size_t tag_name_len = strlen(tag_name);
  const char *p = tags_string;
  while (*p != '\0') {
    const char *start = p;
    while (*p != '\0' && *p != ';') {
      p++;
    }
    const size_t len = size_t(p - start);
    if (len == tag_name_len && STREQLEN(start, tag_name, len)) {
      return true;
    }
    if (*p == ';') {
      p++;
    }
  }
  return false;
}

static bool tag_glyph_hex_to_utf8(const char *input, char r_utf8[8])
{
  r_utf8[0] = '\0';

  if (input == nullptr || input[0] == '\0') {
    return false;
  }

  const char *hex_str = input;
  if (hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
    hex_str += 2;
  }

  char *end = nullptr;
  const unsigned long code_point = strtoul(hex_str, &end, 16);
  if (end == hex_str || *end != '\0' || code_point > 0x10FFFFul) {
    return false;
  }

  const size_t len = BLI_str_utf8_from_unicode(uint(code_point), r_utf8, 8);
  return len != 0;
}

const char *panel_category_glyph_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        const PanelType *panel_type,
                                        bool *r_is_fallback_letter,
                                        float r_color[3])
{
  /* Initialize outputs. */
  if (r_is_fallback_letter) {
    *r_is_fallback_letter = false;
  }
  /* Initialize color to black (use theme). */
  if (r_color) {
    zero_v3(r_color);
  }

  /* 1. Check user overrides in wm->category_glyph_overrides.
   * If override has a glyph, use it immediately.
   * If override only has color (empty glyph), save color and continue looking for glyph. */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        /* If override has a glyph AND it's not a fallback letter, use it and return immediately. */
        if (item->glyph[0] != '\0') {
          /* Check if this is actually a fallback letter (first char of category). */
          const int category_first_char_size = BLI_str_utf8_size_safe(category);
          if (category_first_char_size > 0 &&
              STREQLEN(item->glyph, category, category_first_char_size) &&
              item->glyph[category_first_char_size] == '\0')
          {
            /* Glyph is the first character of category - treat as fallback letter.
             * Save color and continue to fallback. */
            if (r_color && !is_zero_v3(item->color)) {
              copy_v3_v3(r_color, item->color);
            }
            break;
          }
          if (r_color) {
            copy_v3_v3(r_color, item->color);
          }
          return item->glyph;
        }
        /* Override has no glyph but has color - save color and continue looking for glyph. */
        if (r_color && !is_zero_v3(item->color)) {
          copy_v3_v3(r_color, item->color);
        }
        break;  /* Found override entry, don't search further in overrides */
      }
    }
  }

  /* 2. Check global mappings in wm->category_glyph_mappings (registered by Python). */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        /* Save color if available, even if glyph is empty. */
        if (r_color && is_zero_v3(r_color) && !is_zero_v3(item->color)) {
          copy_v3_v3(r_color, item->color);
        }
        /* Only return if glyph is not empty AND not a fallback letter.
         * A glyph that matches the first character of the category is a fallback letter,
         * even if it was saved in the mapping. */
        if (item->glyph[0] != '\0') {
          /* Check if this is actually a fallback letter (first char of category). */
          const int category_first_char_size = BLI_str_utf8_size_safe(category);
          if (category_first_char_size > 0 &&
              STREQLEN(item->glyph, category, category_first_char_size) &&
              item->glyph[category_first_char_size] == '\0')
          {
            /* Glyph is the first character of category - treat as fallback letter.
             * Continue to fallback to set is_fallback_letter=true. */
            break;
          }
          return item->glyph;
        }
        break; /* Found entry but glyph is empty - continue to fallback. */
      }
    }
  }

  /* 3. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    /* Check for color override even if glyph comes from PanelType.
     * This allows setting color via category_glyph_overrides or category_glyph_mappings. */
    if (r_color && is_zero_v3(r_color)) {
      /* Check overrides for color (without requiring glyph in override). */
      if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
        for (const CategoryGlyphItem *item =
                 static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
             item;
             item = static_cast<const CategoryGlyphItem *>(item->next))
        {
          if (STREQ(item->category, category) && !is_zero_v3(item->color)) {
            copy_v3_v3(r_color, item->color);
            break;
          }
        }
      }
      /* Check mappings for color if not found in overrides. */
      if (is_zero_v3(r_color) && wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
        for (const CategoryGlyphItem *item =
                 static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
             item;
             item = static_cast<const CategoryGlyphItem *>(item->next))
        {
          if (STREQ(item->category, category) && !is_zero_v3(item->color)) {
            copy_v3_v3(r_color, item->color);
            break;
          }
        }
      }
    }
    return panel_type->icon_glyph;
  }

  /* 4. Check static default mappings. */
  const char *glyph = lookup_default_glyph(category);
  if (glyph) {
    return glyph;
  }

  /* 5. Fallback: return first character of category. */
  if (r_is_fallback_letter) {
    *r_is_fallback_letter = true;
  }
  /* Extract first UTF-8 character into static buffer. */
  static char first_char_buf[8];
  const int char_size = BLI_str_utf8_size_safe(category);
  if (char_size > 0 && char_size < int(sizeof(first_char_buf))) {
    memcpy(first_char_buf, category, char_size);
    first_char_buf[char_size] = '\0';
    return first_char_buf;
  }
  return category;
}

enum eCategoryGlyphBaseSource {
  CATEGORY_GLYPH_BASE_SOURCE_MAPPING,
  CATEGORY_GLYPH_BASE_SOURCE_PANEL_TYPE,
  CATEGORY_GLYPH_BASE_SOURCE_DEFAULT,
  CATEGORY_GLYPH_BASE_SOURCE_FALLBACK,
};

/**
 * Lookup the base glyph source for a category, without user overrides.
 * Used for reset functionality, comparison, and drag-and-drop tracking.
 */
static const char *panel_category_base_source_lookup(const wmWindowManager *wm,
                                                     const char *category,
                                                     const PanelType *panel_type,
                                                     bool *r_is_reserved = nullptr,
                                                     eCategoryGlyphBaseSource *r_source_type = nullptr)
{
  if (r_is_reserved) *r_is_reserved = false;
  if (r_source_type) *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_FALLBACK;

  /* 1. Check global mappings (registered by Python DEFAULT_CATEGORY_GLYPHS) */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (r_is_reserved) *r_is_reserved = true;
        if (r_source_type) *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_MAPPING;
        return item->glyph;
      }
    }
  }

  /* 2. Check static default mappings */
  const char *glyph = lookup_default_glyph(category);
  if (glyph) {
    if (r_is_reserved) *r_is_reserved = true;
    if (r_source_type) *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_DEFAULT;
    return glyph;
  }

  /* 3. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    if (r_source_type) *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_PANEL_TYPE;
    return panel_type->icon_glyph;
  }

  /* 4. Fallback: return first character of category. */
  if (r_source_type) *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_FALLBACK;
  static char first_char_buf[8];
  const int char_size = BLI_str_utf8_size_safe(category);
  if (char_size > 0 && char_size < int(sizeof(first_char_buf))) {
    memcpy(first_char_buf, category, char_size);
    first_char_buf[char_size] = '\0';
    return first_char_buf;
  }
  return category;
}

/**
 * Lookup display name for a category.
 * Returns user override if exists, otherwise checks global mappings, otherwise returns the category name itself.
 */
static const char *panel_category_display_name_lookup(const wmWindowManager *wm,
                                                        const char *category)
{
  /* 1. Check user overrides first */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        break;
      }
    }
  }

  /* 2. Check global mappings */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        break;
      }
    }
  }

  return category;
}

/**
 * Get category display name for tooltip.
 * Returns user override if exists, otherwise looks up panel label from panel types,
 * otherwise returns the category name itself.
 * This is different from panel_category_display_name_lookup which doesn't check panel types.
 */
static const char *panel_category_tooltip_name_get(const ARegion *region,
                                                   const wmWindowManager *wm,
                                                   const char *category_idname)
{
  /* 1. Check user overrides first */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category_idname)) {
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        break;
      }
    }
  }

  /* 2. Check global mappings */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category_idname)) {
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        break;
      }
    }
  }

  /* 3. Look up panel label from panel types */
  for (const PanelType &pt : region->runtime->type->paneltypes) {
    if (pt.category && STREQ(pt.category, category_idname)) {
      const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
      if (panel_label && panel_label[0]) {
        return panel_label;
      }
    }
  }

  /* 4. Fallback to category name itself */
  return category_idname;
}

/**
 * Check if a string is a single glyph (not regular text).
 * Used to determine if the category is using an icon glyph.
 */
static bool is_single_glyph_str(const char *str)
{
  if (!str || !str[0]) {
    return false;
  }

  const int utf8_char_size = BLI_str_utf8_size_safe(str);
  const size_t len = BLI_strnlen(str, 64);

  /* Single ASCII character or single UTF-8 character. */
  return (len == 1) || (utf8_char_size > 0 && size_t(utf8_char_size) == len);
}

/**
 * Set glyph color: custom color if set, otherwise theme color.
 * Returns true if custom color was applied.
 * \param r_color: Output RGB color that was set (in ubyte format).
 */
static bool set_glyph_color(const int fontid,
                            const float custom_color[3],
                            const bool is_active,
                            const unsigned char theme_col_text[3],
                            const unsigned char theme_col_text_sel[3],
                            unsigned char r_color[3])
{
  if (!is_zero_v3(custom_color)) {
    BLF_color3fv_alpha(fontid, custom_color, 1.0f);
    r_color[0] = uchar(custom_color[0] * 255);
    r_color[1] = uchar(custom_color[1] * 255);
    r_color[2] = uchar(custom_color[2] * 255);
    return true;
  }
  const unsigned char *col = is_active ? theme_col_text_sel : theme_col_text;
  BLF_color3ubv(fontid, col);
  r_color[0] = col[0];
  r_color[1] = col[1];
  r_color[2] = col[2];
  return false;
}

/**
 * Brighten a color by interpolating towards white.
 * \param color: RGB color to brighten (modified in-place)
 * \param factor: Brightening factor (0.0 = no change, 1.0 = white)
 */
static void brighten_color_3ub(uchar color[3], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = uchar(color[i] + (255 - color[i]) * factor);
  }
}

/**
 * Darken a color by interpolating towards black.
 * \param color: RGB color to darken (modified in-place)
 * \param factor: Darkening factor (0.0 = no change, 1.0 = black)
 */
static void darken_color_3ub(uchar color[3], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = uchar(color[i] * (1.0f - factor));
  }
}

/**
 * Brighten an RGBA color by interpolating towards white.
 * \param color: RGBA color to brighten (modified in-place)
 * \param factor: Brightening factor (0.0 = no change, 1.0 = white)
 */
static void brighten_color_4fv(float color[4], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = color[i] + (1.0f - color[i]) * factor;
  }
  /* Alpha remains unchanged. */
}

/**
 * Apply darkening to color and set it as BLF color if needed.
 * \param fontid: Font ID for BLF functions
 * \param color: RGB color to darken (modified in-place) and set
 * \param darken_factor: Factor to darken (0.0 = no change, 1.0 = black)
 */
static void apply_glyph_darkening(const int fontid, uchar color[3], const float darken_factor)
{
  if (darken_factor <= 0.0f) {
    return;
  }

  darken_color_3ub(color, darken_factor);
  BLF_color3ubv(fontid, color);
}

/* -------------------------------------------------------------------- */
/** \name Category Tab Drag & Drop Helpers
 * \{ */

/**
 * Check if a category name is a glyph (high Unicode character).
 * Glyph names are typically used by addons and should not be considered reserved.
 */
static bool category_name_is_glyph(const char *category_id)
{
  if (category_id == nullptr || category_id[0] == '\0') {
    return false;
  }

  /* Check if the category name is a single high Unicode character (glyph).
   * Glyphs from Material Symbols are in the Private Use Area (0xE000-0xF8FF)
   * or other high Unicode ranges. */
  const size_t len = strlen(category_id);

  /* Single UTF-8 character that's a high Unicode glyph */
  if (len <= 4) {
    /* Decode first UTF-8 character */
    unsigned int codepoint = 0;
    if ((category_id[0] & 0x80) == 0) {
      /* ASCII - not a glyph */
      return false;
    }
    else if ((category_id[0] & 0xE0) == 0xC0) {
      /* 2-byte UTF-8 */
      codepoint = (category_id[0] & 0x1F) << 6;
      if (category_id[1]) {
        codepoint |= (category_id[1] & 0x3F);
      }
    }
    else if ((category_id[0] & 0xF0) == 0xE0) {
      /* 3-byte UTF-8 */
      codepoint = (category_id[0] & 0x0F) << 12;
      if (category_id[1]) {
        codepoint |= (category_id[1] & 0x3F) << 6;
      }
      if (category_id[2]) {
        codepoint |= (category_id[2] & 0x3F);
      }
    }
    else if ((category_id[0] & 0xF8) == 0xF0) {
      /* 4-byte UTF-8 */
      codepoint = (category_id[0] & 0x07) << 18;
      if (category_id[1]) {
        codepoint |= (category_id[1] & 0x3F) << 12;
      }
      if (category_id[2]) {
        codepoint |= (category_id[2] & 0x3F) << 6;
      }
      if (category_id[3]) {
        codepoint |= (category_id[3] & 0x3F);
      }
    }

    /* Check if codepoint is in Private Use Area or other glyph ranges */
    if (codepoint >= 0xE000 && codepoint <= 0xF8FF) {
      return true;
    }
  }

  return false;
}

/**
 * Check if a category is reserved (cannot be reordered).
 * This checks ONLY the base source (DEFAULT/mappings), NOT overrides.
 * User overrides (color, glyph) should NOT affect reserved status.
 *
 * Reserved categories are:
 * - Categories in wm.category_glyph_mappings (from DEFAULT_CATEGORY_GLYPHS) with TEXT names
 * - Categories in static default_glyph_mappings
 *
 * NOT reserved:
 * - Categories with glyph names (high Unicode) - these are from addons
 * - Categories with PanelType.icon_glyph (addons)
 * - Categories with fallback letter
 * - Categories with user overrides
 */
bool category_is_reserved(const wmWindowManager * /*wm*/, const char *category_id)
{
  /* Categories with glyph names (high Unicode) are from addons and NOT reserved */
  if (category_name_is_glyph(category_id)) {
    return false;
  }

  /* Only check static default_glyph_mappings (built-in Blender categories).
   * Do NOT check category_glyph_mappings as it contains all categories including addons. */
  if (lookup_default_glyph(category_id)) {
    return true;
  }

  /* PanelType.icon_glyph and fallback are NOT reserved */
  return false;
}

/**
 * Check if a category is reserved (cannot be reordered).
 * This is the public API wrapper for backwards compatibility.
 */
static bool category_is_reserved_for_reorder(const wmWindowManager *wm, const char *category_id)
{
  return category_is_reserved(wm, category_id);
}

/**
 * Count the number of reserved tabs at the beginning of the category list.
 * These tabs cannot be reordered and act as a "header" - non-reserved tabs
 * can only be placed after them.
 */
static int count_reserved_tabs_at_start(const wmWindowManager *wm, ARegion *region)
{
  int reserved_count = 0;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (category_is_reserved_for_reorder(wm, pc_dyn.idname)) {
      reserved_count++;
    }
    else {
      /* Stop counting at first non-reserved tab - reserved tabs are contiguous at start */
      break;
    }
  }
  return reserved_count;
}

/**
 * Get the current object mode as a CategoryTagMode bitmask.
 */
uint32_t get_current_tag_mode_flag(const bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
  }

  switch (ob->mode) {
    case OB_MODE_OBJECT:
      return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
    case OB_MODE_EDIT:
      return static_cast<uint32_t>(CategoryTagMode::EDIT_MODE);
    case OB_MODE_SCULPT:
      return static_cast<uint32_t>(CategoryTagMode::SCULPT_MODE);
    case OB_MODE_VERTEX_PAINT:
      return static_cast<uint32_t>(CategoryTagMode::VERTEX_PAINT);
    case OB_MODE_WEIGHT_PAINT:
      return static_cast<uint32_t>(CategoryTagMode::WEIGHT_PAINT);
    case OB_MODE_TEXTURE_PAINT:
      return static_cast<uint32_t>(CategoryTagMode::TEXTURE_PAINT);
    case OB_MODE_POSE:
      return static_cast<uint32_t>(CategoryTagMode::POSE_MODE);
    default:
      return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
  }
}

/**
 * Check if a category should be visible based on tag filtering and current mode.
 *
 * Rules:
 * 1. Reserved categories are ALWAYS visible
 * 2. Categories without tags are ALWAYS visible
 * 3. Category is visible if it has at least one tag active in current mode
 */
static bool panel_category_is_visible_by_tags(const bContext *C,
                                                const wmWindowManager *wm,
                                                const char *category)
{
  /* Reserved categories are always visible */
  if (category_is_reserved_for_reorder(wm, category)) {
    return true;
  }

  /* Get tags assigned to this category */
  const char *tags_string = category_tags_string_lookup(wm, category);
  if (tags_string == nullptr || tags_string[0] == '\0') {
    return true; /* No tags = always visible */
  }

  /* Get current mode */
  uint32_t current_mode_flag = get_current_tag_mode_flag(C);

  /* Parse semicolon-separated tags and check if any is active in current mode */
  char tag_name[64];
  const char *cursor = tags_string;

  while (*cursor) {
    /* Extract tag name */
    int i = 0;
    while (*cursor && *cursor != ';' && i < 63) {
      tag_name[i++] = *cursor++;
    }
    tag_name[i] = '\0';
    if (*cursor == ';') {
      cursor++;
    }

    /* Skip empty tags */
    if (tag_name[0] == '\0') {
      continue;
    }

    /* Find tag definition and check mode */
    for (const CategoryTagDef *tag = static_cast<const CategoryTagDef *>(
             wm->category_tags.first);
         tag;
         tag = static_cast<const CategoryTagDef *>(tag->next))
    {
      if (STREQ(tag->name, tag_name)) {
        /* mode_flags == 0 means all modes active */
        if (tag->mode_flags == 0 || (tag->mode_flags & current_mode_flag)) {
          return true;
        }
      }
    }
  }

  return false; /* No active tags found for current mode */
}

/**
 * Get the current index of a category in the workspace order,
 * or its default position if not in workspace order.
 */
static int get_category_order_index(const bContext *C, ARegion *region, const char *category_id)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : 0;
  const int region_type = region->regiontype;

  /* Look up in workspace order */
  int user_index = 0;
  if (workspace_category_order_list_is_valid(&workspace->category_order)) {
    for (WorkspaceCategoryOrder *order =
             static_cast<WorkspaceCategoryOrder *>(workspace->category_order.first);
         order;
         order = order->next)
    {
      if (order->space_type == space_type && order->region_type == region_type) {
        if (STREQ(order->category_id, category_id)) {
          return user_index;
        }
        user_index++;
      }
    }
  }

  /* Not found in order - return default position */
  int default_index = 0;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (STREQ(pc_dyn.idname, category_id)) {
      return default_index;
    }
    default_index++;
  }
  return 0;
}

/**
 * Get ordered categories based on workspace customization.
 * Forward declaration - defined later in this file.
 */
static Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region);

/**
 * Calculate the insert index based on cursor position during drag.
 * The insert index is clamped to never be less than the number of reserved
 * tabs at the start - non-reserved tabs can only be placed after reserved ones.
 */
static int calculate_insert_index(const bContext *C,
                                  const wmWindowManager *wm,
                                  ARegion *region,
                                  CategoryDragState *state)
{
  /* Get the minimum insert index (after all reserved tabs) */
  const int min_insert_index = count_reserved_tabs_at_start(wm, region);

  int index = 0;

  /* Get ordered categories from workspace */
  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  /* Iterate through all tabs (allow all categories for now) */
  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    /* Skip the dragged tab */
    if (STREQ(pc_dyn.idname, state->drag_category_id)) {
      continue;
    }

    const int tab_height = BLI_rcti_size_y(&pc_dyn.rect);

    /* Calculate the current visual shift of this tab to prevent jumping (hysteresis). */
    int y_shift = 0;
    if (!state->is_reserved) {
      /* Map the loop index (which skips dragged tab) to current_display_index */
      int curr_disp = index;
      if (index >= state->original_index) {
        curr_disp++; /* Shift upward if we are past the originally pulled tab slot */
      }

      if (state->current_insert_index > state->original_index) {
        if (curr_disp > state->original_index && curr_disp <= state->current_insert_index) {
          y_shift = state->drag_tab_height + state->tab_v_pad;
        }
      }
      else if (state->current_insert_index < state->original_index) {
        if (curr_disp >= state->current_insert_index && curr_disp < state->original_index) {
          y_shift = -state->drag_tab_height - state->tab_v_pad;
        }
      }
    }

    /* Shift the visual center exactly by the same amount the view does */
    const int tab_center_y = (pc_dyn.rect.ymax + y_shift) - tab_height / 2;

    /* Use appropriate edge of dragged tab based on actual direction of movement.
     * Compare current offset with previous frame's offset to detect direction.
     * When moving up, use top edge; when moving down, use bottom edge. */
    int edge_offset;
    if (state->drag_offset_y > state->prev_drag_offset_y) {
      /* Moving up - use top edge */
      edge_offset = state->drag_top_edge_offset;
    }
    else {
      /* Moving down or stationary - use bottom edge */
      edge_offset = state->drag_bottom_edge_offset;
    }
    const int effective_y = state->drag_start_y + edge_offset + int(state->drag_offset_y);

    /* If effective position is above this tab's center, insert before it */
    if (effective_y > tab_center_y) {
      /* Clamp to minimum index - can't insert before reserved tabs */
      return max_ii(index, min_insert_index);
    }

    index++;
  }

  return index; /* Insert at end */
}

/**
 * Update the insert zone boundaries for visual shift calculation.
 */
static void update_insert_zone(const bContext *C,
                               const wmWindowManager * /*wm*/,
                               ARegion *region,
                               CategoryDragState *state)
{
  int current_index = 0;
  int y_accumulated = 0;

  /* Get ordered categories from workspace */
  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    const int tab_height = BLI_rcti_size_y(&pc_dyn.rect);

    if (current_index == state->current_insert_index) {
      /* Found insert position */
      state->insert_y_start = y_accumulated;
      state->insert_y_end = y_accumulated + tab_height + state->tab_v_pad;
      return;
    }

    if (!STREQ(pc_dyn.idname, state->drag_category_id)) {
      y_accumulated += tab_height + state->tab_v_pad;
    }
    current_index++;
  }

  /* Insert at end */
  state->insert_y_start = y_accumulated;
  state->insert_y_end = y_accumulated + state->drag_tab_height + state->tab_v_pad;
}

/**
 * Clear category order for a specific region in the workspace.
 */
static void workspace_category_order_clear(WorkSpace *workspace, int space_type, int region_type)
{
  if (!workspace_category_order_list_is_valid(&workspace->category_order)) {
    return;
  }

  WorkspaceCategoryOrder *order = static_cast<WorkspaceCategoryOrder *>(
      workspace->category_order.first);
  WorkspaceCategoryOrder *order_next;

  while (order) {
    order_next = order->next;
    if (order->space_type == space_type && order->region_type == region_type) {
      BLI_remlink(&workspace->category_order, order);
      MEM_delete(order);
    }
    order = order_next;
  }
}

/**
 * Apply the new category order and save to Workspace.
 */
static void apply_category_order(bContext *C, ARegion *region, CategoryDragState *state)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : 0;
  const int region_type = region->regiontype;

  /* Build final order - allow ALL categories for now (ignore reserved check) */
  Vector<std::string> final_order;
  int insert_idx = 0;

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;

    /* Insert dragged category at correct position */
    if (insert_idx == state->current_insert_index) {
      final_order.append(state->drag_category_id);
    }

    /* Add other categories */
    if (!STREQ(pc_dyn.idname, state->drag_category_id)) {
      final_order.append(pc_dyn.idname);
      insert_idx++;
    }
  }

  /* If not inserted yet, add to end */
  if (insert_idx <= state->current_insert_index &&
      !final_order.contains(state->drag_category_id))
  {
    final_order.append(state->drag_category_id);
  }

  /* Clear old order for this region */
  workspace_category_order_clear(workspace, space_type, region_type);

  /* Save new order */
  for (int i = 0; i < final_order.size(); i++) {
    WorkspaceCategoryOrder *item = MEM_new<WorkspaceCategoryOrder>(__func__);
    item->space_type = space_type;
    item->region_type = region_type;
    STRNCPY(item->category_id, final_order[i].c_str());
    item->order_index = i;
    BLI_addtail(&workspace->category_order, item);
  }

  /* Notify of change */
  WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);
}

/**
 * Ensure the active category is visible. If not, switch to the first visible category.
 * This is called when tag filtering changes to prevent having an invisible active category.
 */
void panel_category_tabs_ensure_active_visible(const bContext *C, ARegion *region)
{
  if (!panel_category_tabs_is_visible(region)) {
    return;
  }

  const wmWindowManager *wm = CTX_wm_manager(C);
  const char *current_active = panel_category_active_get(region, false);
  
  /* Check if current active category is still visible */
  if (current_active && panel_category_is_visible_by_tags(C, wm, current_active)) {
    return; /* Current active is still visible, nothing to do */
  }

  /* Current active is not visible, find first visible category */
  Vector<PanelCategoryDyn *> visible_categories = get_ordered_categories(C, region);
  if (!visible_categories.is_empty()) {
    panel_category_active_set(region, visible_categories[0]->idname);
  }
}

/**
 * Get categories in workspace order, with unlisted categories appended at end.
 * Only returns categories that are visible based on tag filtering.
 */
static Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : 0;
  const int region_type = region->regiontype;
  const wmWindowManager *wm = CTX_wm_manager(C);

  /* Map of existing categories for quick lookup */
  Map<std::string, PanelCategoryDyn *> existing;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    /* Only include categories that are visible by tag filtering */
    if (panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      existing.add(std::string(pc_dyn.idname), &pc_dyn);
    }
  }

  /* Collect workspace order entries for this region */
  Vector<std::pair<int, std::string>> workspace_order;
  if (workspace_category_order_list_is_valid(&workspace->category_order)) {
    for (WorkspaceCategoryOrder *order =
             static_cast<WorkspaceCategoryOrder *>(workspace->category_order.first);
         order;
         order = order->next)
    {
      if (order->space_type == space_type && order->region_type == region_type) {
        workspace_order.append(std::make_pair(order->order_index, std::string(order->category_id)));
      }
    }
  }

  /* Sort by order_index */
  std::sort(workspace_order.begin(), workspace_order.end());

  /* Build result list */
  Vector<PanelCategoryDyn *> result;
  Set<std::string> added;

  /* First add in workspace order */
  for (const auto &item : workspace_order) {
    PanelCategoryDyn **pc = existing.lookup_ptr(item.second);
    if (pc && !added.contains(item.second)) {
      result.append(*pc);
      added.add(item.second);
    }
  }

  /* Then add remaining categories (new ones not in workspace order) */
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    std::string id(pc_dyn.idname);
    if (!added.contains(id) && panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      result.append(&pc_dyn);
      added.add(id);
    }
  }

  return result;
}

/** \} */

/**
 * Draw the content (glyph and/or text) of a single category tab.
 *
 * Unified helper that handles all display modes:
 * - GLYPHS_ONLY: single glyph centered, or glyph+text when active with show_active_name
 * - GLYPHS_TEXT: glyph at top + rotated text below
 * - TEXT_ONLY: rotated text
 *
 * \param rct: Target rectangle for drawing.
 * \param rct_xmin, rct_xmax: Column bounds for text X positioning (may differ from rct for
 *                             dragged tabs).
 * \param darken_factor: Darkening for inactive tabs (0.0 = none).
 */
static void ui_panel_category_draw_content(
    const ARegion *region,
    const wmWindowManager *wm,
    const char *category_id,
    const char *category_id_draw,
    const rcti *rct,
    const int rct_xmin,
    const int rct_xmax,
    const bool is_active,
    const bool is_left,
    const eUserPref_CategoryTabsDisplayMode display_mode,
    const int fontid,
    const uiFontStyle *fstyle,
    const float fstyle_points,
    const float zoom,
    const float category_tabs_zoom,
    const int tab_v_pad_text,
    const float darken_factor,
    const uchar theme_col_tab_text[3],
    const uchar theme_col_tab_text_sel[3])
{
  /* Look up glyph for this category. */
  bool is_fallback_letter = false;
  float glyph_color[3] = {0.0f, 0.0f, 0.0f};
  const char *glyph = panel_category_glyph_lookup(
      wm, category_id, nullptr, &is_fallback_letter, glyph_color);
  const bool has_glyph = is_single_glyph_str(glyph) && !is_fallback_letter;

  /* Decide whether to draw dual mode (glyph/letter at top + rotated text below)
   * or single item (just a glyph centered, or just text rotated). */
  bool draw_dual = false;
  const char *text_for_name = category_id_draw;

  if (display_mode == USER_CATEGORY_TABS_GLYPHS_TEXT && has_glyph) {
    draw_dual = true;
  }
  else if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
           U.category_tabs_show_active_name && is_active)
  {
    if (has_glyph) {
      /* Glyph categories: always show glyph + text when active.
       * If category name is a single glyph, resolve text from panel label. */
      draw_dual = true;
      if (is_single_glyph_str(category_id_draw)) {
        for (const PanelType &pt : region->runtime->type->paneltypes) {
          if (pt.category && STREQ(pt.category, category_id)) {
            const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
            if (panel_label && panel_label[0]) {
              text_for_name = panel_label;
              break;
            }
          }
        }
      }
    }
    else if (is_fallback_letter) {
      draw_dual = true;
      if (is_single_glyph_str(category_id_draw)) {
        for (const PanelType &pt : region->runtime->type->paneltypes) {
          if (pt.category && STREQ(pt.category, category_id)) {
            const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
            if (panel_label && panel_label[0]) {
              text_for_name = panel_label;
              break;
            }
          }
        }
      }
    }
  }

  /* Helper lambda for shadow setup/teardown. */
  auto shadow_enable = [&]() {
    if (fstyle->shadow) {
      BLF_enable(fontid, BLF_SHADOW);
      const float shadow_color[4] = {
          fstyle->shadowcolor, fstyle->shadowcolor, fstyle->shadowcolor, fstyle->shadowalpha};
      BLF_shadow(fontid, FontShadowType(fstyle->shadow), shadow_color);
      BLF_shadow_offset(fontid, fstyle->shadx, fstyle->shady);
    }
  };
  auto shadow_disable = [&]() {
    if (fstyle->shadow) {
      BLF_disable(fontid, BLF_SHADOW);
    }
  };

  if (draw_dual) {
    /* === Dual mode: glyph/letter at top, rotated text below === */
    BLF_disable(fontid, BLF_ROTATION);

    const float glyph_width = BLF_width(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX);
    const float ascender = float(BLF_ascender(fontid));
    const float descender = float(BLF_descender(fontid));
    const float glyph_height = ascender - descender;

    /* Position glyph from top of tab. Fallback letters get a small upward shift. */
    const float tab_center_x = float(rct->xmin + rct->xmax) * 0.5f;
    const float extra_shift = is_fallback_letter ? (4.0f * UI_SCALE_FAC) : 0.0f;
    const float glyph_pos_y = float(rct->ymax) - glyph_height - (tab_v_pad_text - extra_shift);

    BLF_position(fontid, tab_center_x - glyph_width * 0.5f, glyph_pos_y - descender, 0.0f);
    uchar glyph_color_out[3];
    set_glyph_color(
        fontid, glyph_color, is_active, theme_col_tab_text, theme_col_tab_text_sel, glyph_color_out);
    if (!is_active && darken_factor > 0.0f) {
      apply_glyph_darkening(fontid, glyph_color_out, darken_factor);
    }

    shadow_enable();
    BLF_draw(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX);
    shadow_disable();

    /* Draw rotated text below glyph. */
    BLF_enable(fontid, BLF_ROTATION);
    BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);

    const int text_v_ofs = round_fl_to_int(float(rct_xmax - rct_xmin) * 0.5f);
    const int text_size_offset = round_fl_to_int(fstyle_points * UI_SCALE_FAC *
                                                  category_tabs_zoom * 0.35f);
    const float text_pos_x = is_left ? rct->xmax - text_v_ofs + text_size_offset :
                                        rct->xmin + text_v_ofs - text_size_offset;

    const int glyph_h = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
    const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC * zoom);
    const float text_pos_y = is_left ? rct->ymin + tab_v_pad_text + glyph_text_gap :
                                        rct->ymax - tab_v_pad_text - glyph_h - glyph_text_gap;

    BLF_position(fontid, text_pos_x, text_pos_y, 0.0f);

    /* Text color: use active color for active tabs, apply darkening for inactive. */
    if (!is_active && darken_factor > 0.0f) {
      uchar text_color[3] = {theme_col_tab_text[0], theme_col_tab_text[1], theme_col_tab_text[2]};
      darken_color_3ub(text_color, darken_factor);
      BLF_color3ubv(fontid, text_color);
    }
    else {
      BLF_color3ubv(fontid, is_active ? theme_col_tab_text_sel : theme_col_tab_text);
    }

    shadow_enable();
    BLF_draw(fontid, text_for_name, BLF_DRAW_STR_DUMMY_MAX);
    shadow_disable();

    BLF_disable(fontid, BLF_ROTATION);
    return;
  }

  /* === Single item mode === */
  const char *draw_str;
  bool draw_as_glyph;
  bool should_rotate = false;

  switch (display_mode) {
    case USER_CATEGORY_TABS_GLYPHS_ONLY:
      draw_str = glyph;
      draw_as_glyph = !is_fallback_letter;
      should_rotate = false;
      break;
    case USER_CATEGORY_TABS_GLYPHS_TEXT:
      draw_str = category_id_draw;
      draw_as_glyph = is_single_glyph_str(category_id_draw);
      should_rotate = !draw_as_glyph;
      break;
    case USER_CATEGORY_TABS_TEXT_ONLY:
    default:
      if (is_single_glyph_str(category_id_draw)) {
        const char *panel_label = nullptr;
        for (const PanelType &pt : region->runtime->type->paneltypes) {
          if (pt.category && STREQ(pt.category, category_id)) {
            panel_label = CTX_IFACE_(pt.translation_context, pt.label);
            if (panel_label && panel_label[0]) {
              break;
            }
          }
        }
        draw_str = panel_label ? panel_label : category_id_draw;
      }
      else {
        draw_str = category_id_draw;
      }
      draw_as_glyph = false;
      should_rotate = true;
      break;
  }

  /* Position. */
  if (!should_rotate) {
    BLF_disable(fontid, BLF_ROTATION);
    const float gw = BLF_width(fontid, draw_str, BLF_DRAW_STR_DUMMY_MAX);
    const float asc = float(BLF_ascender(fontid));
    const float desc = float(BLF_descender(fontid));
    const float gh = asc - desc;
    const float cx = float(rct->xmin + rct->xmax) * 0.5f;
    const float cy = float(rct->ymin + rct->ymax) * 0.5f;
    BLF_position(fontid, cx - gw * 0.5f, cy - gh * 0.5f - desc, 0.0f);
  }
  else {
    BLF_enable(fontid, BLF_ROTATION);
    BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
    const int text_v_ofs = round_fl_to_int(float(rct_xmax - rct_xmin) * 0.5f);
    const int text_size_offset = round_fl_to_int(fstyle_points * UI_SCALE_FAC *
                                                  category_tabs_zoom * 0.35f);
    const float px = is_left ? rct->xmax - text_v_ofs + text_size_offset :
                               rct->xmin + text_v_ofs - text_size_offset;
    const float py = is_left ? rct->ymin + tab_v_pad_text : rct->ymax - tab_v_pad_text;
    BLF_position(fontid, px, py, 0.0f);
  }

  /* Color. */
  if (draw_as_glyph) {
    uchar glyph_color_out[3];
    set_glyph_color(
        fontid, glyph_color, is_active, theme_col_tab_text, theme_col_tab_text_sel, glyph_color_out);
    apply_glyph_darkening(fontid, glyph_color_out, darken_factor);
  }
  else {
    uchar text_color[3];
    if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY && !is_zero_v3(glyph_color)) {
      text_color[0] = uchar(glyph_color[0] * 255);
      text_color[1] = uchar(glyph_color[1] * 255);
      text_color[2] = uchar(glyph_color[2] * 255);
    }
    else {
      text_color[0] = is_active ? theme_col_tab_text_sel[0] : theme_col_tab_text[0];
      text_color[1] = is_active ? theme_col_tab_text_sel[1] : theme_col_tab_text[1];
      text_color[2] = is_active ? theme_col_tab_text_sel[2] : theme_col_tab_text[2];
    }
    if (darken_factor > 0.0f) {
      darken_color_3ub(text_color, darken_factor);
    }
    BLF_color3ubv(fontid, text_color);
  }

  shadow_enable();
  BLF_draw(fontid, draw_str, BLF_DRAW_STR_DUMMY_MAX);
  shadow_disable();

  BLF_disable(fontid, BLF_ROTATION);
}

void panel_category_tabs_draw_all(const bContext *C, ARegion *region, const char *category_id_active)
{
  // #define USE_FLAT_INACTIVE
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  View2D *v2d = &region->v2d;
  const uiStyle *style = style_get();
  const uiFontStyle *fstyle = &style->widget;
  fontstyle_set(fstyle);
  const int fontid = fstyle->uifont_id;
  float fstyle_points = fstyle->points;
  const float aspect = BLI_listbase_is_empty(&region->runtime->uiblocks) ?
                           1.0f :
                           (static_cast<Block *>(region->runtime->uiblocks.first))->aspect;

  /* Check for drag state */
  CategoryDragState *drag_state = static_cast<CategoryDragState *>(
      region->runtime->category_tabs_drag_state);
  const bool is_dragging = (drag_state != nullptr && drag_state->is_dragging);
  const char *drag_category_id = is_dragging ? drag_state->drag_category_id : "";

  /* Get display mode from preferences. */
  const eUserPref_CategoryTabsDisplayMode display_mode =
      static_cast<eUserPref_CategoryTabsDisplayMode>(U.category_tabs_display_mode);

  /* Get zoom based on display mode. */
  float category_tabs_zoom;
  switch (display_mode) {
    case USER_CATEGORY_TABS_GLYPHS_ONLY:
      category_tabs_zoom = U.category_tabs_zoom_icon;
      break;
    case USER_CATEGORY_TABS_GLYPHS_TEXT:
      category_tabs_zoom = U.category_tabs_zoom_mixed;
      break;
    case USER_CATEGORY_TABS_TEXT_ONLY:
    default:
      category_tabs_zoom = U.category_tabs_zoom_text;
      break;
  }
  const float zoom = (1.0f / aspect) * category_tabs_zoom;

  /* Get window manager for glyph lookup. */
  const wmWindowManager *wm = CTX_wm_manager(C);

  const int px = U.pixelsize;
  const int category_tabs_width = round_fl_to_int(UI_PANEL_CATEGORY_MARGIN_WIDTH * zoom);
  const float dpi_fac = UI_SCALE_FAC;
  /* Padding of tabs around text. */
  const int tab_v_pad_text = round_fl_to_int(TABS_PADDING_TEXT_FACTOR * dpi_fac * zoom) + 2 * px;
  /* Padding between tabs. */
  const int tab_v_pad = round_fl_to_int(TABS_PADDING_BETWEEN_FACTOR * dpi_fac * zoom);
  bTheme *btheme = theme::theme_get();
  const float tab_curve_radius = btheme->tui.wcol_tab.roundness * U.widget_unit * zoom;
  /* Round all corners when region overlap is on. */
  const int roundboxtype = region->overlap ? CNR_ALL :
                                             (is_left ? (CNR_TOP_LEFT | CNR_BOTTOM_LEFT) :
                                                        (CNR_TOP_RIGHT | CNR_BOTTOM_RIGHT));
  bool is_alpha;
#ifdef USE_FLAT_INACTIVE
  bool is_active_prev = false;
#endif
  /* Same for all tabs. */
  /* Intentionally don't scale by 'px'. */
  const int rct_xmin = is_left ? v2d->mask.xmin + 3 : (v2d->mask.xmax - category_tabs_width);
  const int rct_xmax = is_left ? v2d->mask.xmin + category_tabs_width : (v2d->mask.xmax - 3);

  int y_ofs = tab_v_pad;

  /* Primary theme colors. */
  uchar theme_col_back[4];

  /* Tab colors. */
  uchar theme_col_tab_bg[4];
  uchar theme_col_tab_text[3];
  uchar theme_col_tab_text_sel[3];
  float theme_col_tab_active[4];
  float theme_col_tab_inactive[4];
  float theme_col_tab_outline[4];
  float theme_col_tab_outline_sel[4];

  theme::get_color_4ubv(TH_BACK, theme_col_back);
  theme::get_color_3ubv(TH_TAB_TEXT, theme_col_tab_text);
  theme::get_color_3ubv(TH_TAB_TEXT_HI, theme_col_tab_text_sel);
  theme::get_color_4ubv(TH_TAB_BACK, theme_col_tab_bg);
  theme::get_color_4fv(TH_TAB_ACTIVE, theme_col_tab_active);
  theme::get_color_4fv(TH_TAB_INACTIVE, theme_col_tab_inactive);
  theme::get_color_4fv(TH_TAB_OUTLINE, theme_col_tab_outline);
  theme::get_color_4fv(TH_TAB_OUTLINE_ACTIVE, theme_col_tab_outline_sel);

  is_alpha = (region->overlap && (theme_col_back[3] != 255));

  fontscale(&fstyle_points, aspect);
  BLF_size(fontid, fstyle_points * UI_SCALE_FAC * category_tabs_zoom);

  /* Check the region type supports categories to avoid an assert
   * for showing 3D view panels in the properties space. */
  if (BKE_regiontype_uses_category_tabs(region->runtime->type)) {
    BLI_assert(panel_category_is_visible(region));
  }

  /* Get mouse position for hover detection. */
  wmWindow *win = CTX_wm_window(C);
  int mouse_x = 0, mouse_y = 0;
  bool mouse_in_region = false;
  if (win && win->runtime->eventstate) {
    mouse_x = win->runtime->eventstate->xy[0] - region->winrct.xmin;
    mouse_y = win->runtime->eventstate->xy[1] - region->winrct.ymin;
    mouse_in_region = BLI_rcti_isect_pt(&region->winrct,
                                        win->runtime->eventstate->xy[0],
                                        win->runtime->eventstate->xy[1]);
  }

  /* Get ordered categories from workspace */
  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  /* Calculate tab rectangle for each panel using ordered list. */
  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    rcti *rct = &pc_dyn.rect;
    const char *category_id = pc_dyn.idname;
    const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id));

    /* Get glyph for this category using priority chain. */
    bool is_fallback_letter = false;
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    const char *glyph = panel_category_glyph_lookup(wm, category_id, nullptr, &is_fallback_letter, glyph_color);
    const bool has_glyph = is_single_glyph_str(glyph) && !is_fallback_letter;

    /* Calculate width based on display mode. */
    int category_width;
    switch (display_mode) {
      case USER_CATEGORY_TABS_GLYPHS_ONLY:
        /* Icon mode: ALL glyphs/letters are rotated (-90 for right, +90 for left).
           So always calculate rotated width.
         */
        BLF_enable(fontid, BLF_ROTATION);
        BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
        category_width = round_fl_to_int(BLF_width(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
        BLF_disable(fontid, BLF_ROTATION);

        /* Expand for active tab name if option enabled and this is active tab */
        if (U.category_tabs_show_active_name && STREQ(category_id, category_id_active)) {
          /* Get text to display:
           * - For single glyph category_id: find panel label
           * - For fallback letter: use category_id_draw (the category name)
           */
          const char *text_for_name = category_id_draw;
          if (is_single_glyph_str(category_id_draw)) {
            for (const PanelType &pt : region->runtime->type->paneltypes) {
              if (pt.category && STREQ(pt.category, category_id)) {
                const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
                if (panel_label && panel_label[0]) {
                  text_for_name = panel_label;
                  break;
                }
              }
            }
          }
          /* For fallback letters, use category_id_draw directly (matches DRAW code) */
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          const int text_w = round_fl_to_int(BLF_width(fontid, text_for_name, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
          const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC * zoom);
          category_width += text_w + glyph_text_gap;
        }
        break;

      case USER_CATEGORY_TABS_GLYPHS_TEXT:
        /* Glyph + text combined width. */
        if (has_glyph) {
          const int glyph_h = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          const int text_w = round_fl_to_int(BLF_width(fontid, category_id_draw, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
          const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC * zoom);
          category_width = glyph_h + text_w + glyph_text_gap;
        }
        else {
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          category_width = round_fl_to_int(BLF_width(fontid, category_id_draw, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
        }
        break;

      case USER_CATEGORY_TABS_TEXT_ONLY:
      default: {
        /* Text-only mode: display text VERTICALLY with rotation. */
        /* If category_id is a single glyph, find panel label to use instead. */
        const char *text_for_size = category_id_draw;
        if (is_single_glyph_str(category_id_draw)) {
          for (const PanelType &pt : region->runtime->type->paneltypes) {
            if (pt.category && STREQ(pt.category, category_id)) {
              const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
              if (panel_label && panel_label[0]) {
                text_for_size = panel_label;
                break;
              }
            }
          }
        }
        BLF_enable(fontid, BLF_ROTATION);
        BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
        category_width = round_fl_to_int(BLF_width(fontid, text_for_size, BLF_DRAW_STR_DUMMY_MAX));
        BLF_disable(fontid, BLF_ROTATION);
        break;
      }
    }

    rct->xmin = rct_xmin;
    rct->xmax = rct_xmax;

    rct->ymin = v2d->mask.ymax - (y_ofs + category_width + (tab_v_pad_text * 2));
    rct->ymax = v2d->mask.ymax - (y_ofs);

    y_ofs += category_width + tab_v_pad + (tab_v_pad_text * 2);
  }

  /* Calculate settings button rect (last element, scrolls with tabs). */
  const int settings_icon_height = round_fl_to_int(BLF_height(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX));
  const int settings_button_height = settings_icon_height + (tab_v_pad_text * 2);
  rcti *settings_rct = &region->runtime->category_tabs_settings_rect;
  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    settings_rct->xmin = rct_xmin;
    settings_rct->xmax = rct_xmax;
    settings_rct->ymin = v2d->mask.ymax - (y_ofs + settings_button_height);
    settings_rct->ymax = v2d->mask.ymax - y_ofs;
  }

  /* Include settings button height in max_scroll calculation so it stays visible when scrolling. */
  const int total_content_height = y_ofs + settings_button_height + tab_v_pad;
  const int max_scroll = max_ii(total_content_height - BLI_rcti_size_y(&v2d->mask), 0);
  const int scroll = clamp_i(region->category_scroll, 0, max_scroll);
  region->category_scroll = scroll;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    rcti *rct = &pc_dyn.rect;
    rct->ymin += scroll;
    rct->ymax += scroll;
  }

  /* Apply scroll to settings button rect. */
  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    settings_rct->ymin += scroll;
    settings_rct->ymax += scroll;
  }

  /* Begin drawing. */
  GPU_line_smooth(true);

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Draw the background. */
  if (is_alpha) {
    GPU_blend(GPU_BLEND_ALPHA);
    immUniformColor4ubv(theme_col_tab_bg);
  }
  else {
    immUniformColor3ubv(theme_col_tab_bg);
  }

  if (is_left) {
    immRectf(
        pos, v2d->mask.xmin, v2d->mask.ymin, v2d->mask.xmin + category_tabs_width, v2d->mask.ymax);
  }
  else {
    immRectf(pos,
             v2d->mask.xmax - category_tabs_width,
             v2d->mask.ymin,
             v2d->mask.xmax + 1,
             v2d->mask.ymax);
  }

  if (is_alpha) {
    GPU_blend(GPU_BLEND_NONE);
  }

  immUnbindProgram();

  /* If the area is too small to show panels, then don't show any tabs as active. */
  const bool too_narrow = BLI_rcti_size_x(&region->winrct) <=
                          int(UI_PANEL_CATEGORY_MIN_WIDTH * UI_SCALE_FAC / aspect);

  /* Track current index for drag shift calculation */
  int current_display_index = 0;

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;

    /* Skip drawing the dragged tab in its normal position */
    if (is_dragging && !drag_state->is_reserved && STREQ(pc_dyn.idname, drag_category_id)) {
      current_display_index++;
      continue;
    }

    /* Calculate Y shift for visual feedback during drag.
     * Only shift tabs that are between original position and insert position.
     */
    int y_shift = 0;
    if (is_dragging && !drag_state->is_reserved) {
      const int insert_idx = drag_state->current_insert_index;
      const int original_idx = drag_state->original_index;

      if (insert_idx > original_idx) {
        /* Moving DOWN: shift tabs between original+1 and insert UP to fill gap */
        if (current_display_index > original_idx && current_display_index <= insert_idx) {
          y_shift = drag_state->drag_tab_height + tab_v_pad;
        }
      }
      else if (insert_idx < original_idx) {
        /* Moving UP: shift tabs between insert and original-1 DOWN to make room */
        if (current_display_index >= insert_idx && current_display_index < original_idx) {
          y_shift = -drag_state->drag_tab_height - tab_v_pad;
        }
      }
      /* If insert_idx == original_idx, no shift needed */
    }

    /* Apply shift to drawing rect */
    rcti shifted_rect = pc_dyn.rect;
    shifted_rect.ymin += y_shift;
    shifted_rect.ymax += y_shift;

    /* Use shifted rect as the main drawing rect */
    const rcti *rct = &shifted_rect;

    if (rct->ymin > v2d->mask.ymax) {
      /* Scrolled outside the top of the view, check the next tab. */
      current_display_index++;
      continue;
    }
    if (rct->ymax < v2d->mask.ymin) {
      /* Scrolled past visible bounds, no need to draw other tabs. */
      break;
    }
    const char *category_id = pc_dyn.idname;
    const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id));
    const bool is_active = !too_narrow && STREQ(category_id, category_id_active);

    /* Increment display index after processing */
    current_display_index++;

    /* Check if mouse is hovering over this tab. */
    const bool is_hover = BLI_rcti_isect_pt(rct, mouse_x, mouse_y);

    /* Calculate darkening factor for non-active tabs in all modes.
     * Darken only when inactive AND not hovering (hover shows original color). */
    float darken_factor = 0.0f;
    if (!is_active && !is_hover) {
      darken_factor = TABS_GLYPH_DARKEN_BASE;
    }

    /* Calculate background brightening for inactive tabs (all modes). */
    float bg_brighten_factor = 0.0f;
    if (!is_active) {
      bg_brighten_factor = is_hover ? TABS_BG_BRIGHTEN_HOVER : TABS_BG_BRIGHTEN_BASE;
    }

    GPU_blend(GPU_BLEND_ALPHA);

#ifdef USE_FLAT_INACTIVE
    /* Draw line between inactive tabs. */
    if (is_active == false && is_active_prev == false && pc_dyn.prev) {
      pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      immUniformColor3fvAlpha(theme_col_tab_outline, 0.3f);
      immRectf(pos,
               is_left ? v2d->mask.xmin + (category_tabs_width / 5) :
                         v2d->mask.xmax - (category_tabs_width / 5),
               rct->ymax + px,
               is_left ? (v2d->mask.xmin + category_tabs_width) - (category_tabs_width / 5) :
                         (v2d->mask.xmax - category_tabs_width) + (category_tabs_width / 5),
               rct->ymax + (px * 3));
      immUnbindProgram();
    }

    is_active_prev = is_active;

    if (is_active)
#endif
    {
      /* Draw filled rectangle and outline for tab. */
      draw_roundbox_corner_set(roundboxtype);
      rctf box_rect;
      box_rect.xmin = rct->xmin;
      box_rect.xmax = rct->xmax;
      box_rect.ymin = rct->ymin;
      box_rect.ymax = rct->ymax;

      /* Prepare tab background color with brightening for inactive tabs. */
      float tab_bg_color[4];
      if (is_active) {
        copy_v4_v4(tab_bg_color, theme_col_tab_active);
      }
      else {
        copy_v4_v4(tab_bg_color, theme_col_tab_inactive);
        brighten_color_4fv(tab_bg_color, bg_brighten_factor);
      }

      draw_roundbox_4fv(&box_rect,
                        true,
                        tab_curve_radius,
                        tab_bg_color);
      draw_roundbox_4fv(&box_rect,
                        false,
                        tab_curve_radius,
                        is_active ? theme_col_tab_outline_sel : theme_col_tab_outline);

      /* Disguise the outline on one side to join the tab to the panel. */
      if (!region->overlap) {
        pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
        immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

        immUniformColor4fv(tab_bg_color);
        immRectf(pos,
                 is_left ? rct->xmax - px : rct->xmin,
                 rct->ymin + px,
                 is_left ? rct->xmax : rct->xmin + px,
                 rct->ymax - px);
        immUnbindProgram();
      }
    }

    /* Tab titles. */
    ui_panel_category_draw_content(region,
                                   wm,
                                   category_id,
                                   category_id_draw,
                                   rct,
                                   rct_xmin,
                                   rct_xmax,
                                   is_active,
                                   is_left,
                                   display_mode,
                                   fontid,
                                   fstyle,
                                   fstyle_points,
                                   zoom,
                                   category_tabs_zoom,
                                   tab_v_pad_text,
                                   darken_factor,
                                   theme_col_tab_text,
                                   theme_col_tab_text_sel);


    /* Not essential, but allows events to be handled right up to the region edge (#38171). */
    if (is_left) {
      pc_dyn.rect.xmin = v2d->mask.xmin;
    }
    else {
      pc_dyn.rect.xmax = v2d->mask.xmax;
    }
  }

  /* Draw settings button (last element, scrolls with tabs). */
  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    /* Only draw if visible (after scroll). */
    const rcti *settings_rct = &region->runtime->category_tabs_settings_rect;
    if (settings_rct->ymin <= v2d->mask.ymax && settings_rct->ymax >= v2d->mask.ymin) {
      panel_category_tabs_draw_settings_button(C, region, zoom, theme_col_tab_text);
    }
  }

  /* Draw the dragged tab at cursor position and ghost tab at insert position */
  if (is_dragging && !drag_state->is_reserved) {
    PanelCategoryDyn *drag_tab = nullptr;
    for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
      if (STREQ(pc_dyn.idname, drag_category_id)) {
        drag_tab = &pc_dyn;
        break;
      }
    }

    if (drag_tab) {
      const int insert_idx = drag_state->current_insert_index;
      const int original_idx = drag_state->original_index;
      const int tab_h = drag_state->drag_tab_height;

      /* Find the tab at insert position to determine ghost location */
      PanelCategoryDyn *insert_position_tab = nullptr;
      int current_idx = 0;
      for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
        if (STREQ(pc_dyn_ptr->idname, drag_category_id)) {
          continue;
        }
        if (current_idx == insert_idx) {
          insert_position_tab = pc_dyn_ptr;
          break;
        }
        current_idx++;
      }

      /* Draw ghost tab at insert position */
      if (insert_position_tab || insert_idx >= int(ordered_categories.size()) - 1) {
        rcti ghost_rect = drag_tab->rect;
        
        /* Find the tab to position relative to.
         * If inserting before a tab, position above it.
         * If appending (insert_position_tab is NULL), position below the last visible tab. */
        PanelCategoryDyn *target_tab = insert_position_tab;
        bool position_above = true;
        int target_orig_idx = -1;

        if (target_tab) {
          /* Need to find the original index of the target tab for shift calculation */
          int loop_idx = 0;
          for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
            if (pc_dyn_ptr == target_tab) {
              target_orig_idx = loop_idx;
              break;
            }
            loop_idx++;
          }
        }
        else {
          /* Append case: use last visible tab */
          int loop_idx = 0;
          for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
            if (STREQ(pc_dyn_ptr->idname, drag_category_id)) {
              loop_idx++;
              continue;
            }
            target_tab = pc_dyn_ptr;
            target_orig_idx = loop_idx;
            loop_idx++;
          }
          position_above = false;
        }

        if (target_tab && target_orig_idx != -1) {
          /* Calculate the visual shift that was applied to the target tab. */
          int target_shift_y = 0;
          if (insert_idx > original_idx) {
            if (target_orig_idx > original_idx && target_orig_idx <= insert_idx) {
              target_shift_y = tab_h + tab_v_pad;
            }
          }
          else if (insert_idx < original_idx) {
            if (target_orig_idx >= insert_idx && target_orig_idx < original_idx) {
              target_shift_y = -tab_h - tab_v_pad;
            }
          }

          /* Apply shift to target rect before calculating ghost position */
          rcti target_rect = target_tab->rect;
          target_rect.ymin += target_shift_y;
          target_rect.ymax += target_shift_y;

          if (position_above) {
            /* Ghost appears above the target tab */
            ghost_rect.ymin = target_rect.ymax + tab_v_pad;
            ghost_rect.ymax = target_rect.ymax + tab_h + tab_v_pad;
          }
          else {
            /* Ghost appears below the target tab */
            ghost_rect.ymin = target_rect.ymin - tab_h - tab_v_pad;
            ghost_rect.ymax = target_rect.ymin - tab_v_pad;
          }
        }

        /* Draw ghost tab (semi-transparent placeholder at insert position) */
        {
          rctf ghost_box_rect;
          ghost_box_rect.xmin = float(ghost_rect.xmin);
          ghost_box_rect.xmax = float(ghost_rect.xmax);
          ghost_box_rect.ymin = float(ghost_rect.ymin);
          ghost_box_rect.ymax = float(ghost_rect.ymax);

          /* Very transparent background for ghost */
          float ghost_bg_color[4];
          copy_v4_v4(ghost_bg_color, theme_col_tab_active);
          ghost_bg_color[3] = 0.3f;  /* More transparent */

          GPU_blend(GPU_BLEND_ALPHA);
          draw_roundbox_corner_set(roundboxtype);
          draw_roundbox_4fv(&ghost_box_rect, true, tab_curve_radius, ghost_bg_color);

          /* Dashed outline for ghost */
          float ghost_outline[4];
          copy_v3_v3(ghost_outline, theme_col_tab_outline_sel);
          ghost_outline[3] = 0.5f;
          draw_roundbox_4fv(&ghost_box_rect, false, tab_curve_radius, ghost_outline);

          GPU_blend(GPU_BLEND_NONE);
        }
      }

      /* Calculate dragged tab position (follows cursor) */
      rcti drag_rect = drag_tab->rect;

      /* Adjust for scroll change since drag start so the tab follows the mouse */
      const int scroll_diff = region->category_scroll - drag_state->initial_scroll;
      drag_rect.ymin -= scroll_diff;
      drag_rect.ymax -= scroll_diff;

      const int offset_y = int(drag_state->drag_offset_y);
      drag_rect.ymin += offset_y;
      drag_rect.ymax += offset_y;

      /* Draw the dragged tab with alpha */
      {
        rctf box_rect;
        box_rect.xmin = float(drag_rect.xmin);
        box_rect.xmax = float(drag_rect.xmax);
        box_rect.ymin = float(drag_rect.ymin);
        box_rect.ymax = float(drag_rect.ymax);

        /* Semi-transparent background */
        float drag_bg_color[4];
        copy_v4_v4(drag_bg_color, theme_col_tab_active);
        drag_bg_color[3] = 0.7f;  /* Semi-transparent */

        GPU_blend(GPU_BLEND_ALPHA);
        draw_roundbox_corner_set(roundboxtype);
        draw_roundbox_4fv(&box_rect, true, tab_curve_radius, drag_bg_color);
        draw_roundbox_4fv(&box_rect, false, tab_curve_radius, theme_col_tab_outline_sel);

        /* Draw the tab content (glyph/text) */
        const char *category_id = drag_tab->idname;
        const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id));
        const rcti *rct = &drag_rect;

        ui_panel_category_draw_content(region,
                                       wm,
                                       category_id,
                                       category_id_draw,
                                       rct,
                                       rct->xmin,
                                       rct->xmax,
                                       STREQ(category_id, category_id_active), /* show text only if dragged tab is actually active */
                                       is_left,
                                       display_mode,
                                       fontid,
                                       fstyle,
                                       fstyle_points,
                                       zoom,
                                       category_tabs_zoom,
                                       tab_v_pad_text,
                                       0.0f, /* no darkening for dragged tab */
                                       theme_col_tab_text,
                                       theme_col_tab_text_sel);

      }
    }
  }

  /* Draw tooltip for reserved tabs during drag attempt */
  /* Tooltip is handled by CategoryDragState->tooltip_region created in invoke/modal */

  GPU_line_smooth(false);

  BLF_disable(fontid, BLF_ROTATION);
}

#undef TABS_PADDING_BETWEEN_FACTOR
#undef TABS_PADDING_TEXT_FACTOR
#undef TABS_GLYPH_TEXT_GAP_FACTOR

/** \} */

static int panel_category_show_active_tab(ARegion *region, const int mval[2])
{
  if (!ED_region_panel_category_gutter_isect_xy(region, mval)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  BLI_assert(BKE_regiontype_uses_category_tabs(region->runtime->type));

  const View2D *v2d = &region->v2d;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    const bool is_active = STREQ(pc_dyn.idname, region->runtime->category);
    if (!is_active) {
      continue;
    }
    const rcti *rct = &pc_dyn.rect;
    region->category_scroll = v2d->mask.ymax - (rct->ymax - region->category_scroll);

    if (pc_dyn.next) {
      const PanelCategoryDyn *pc_dyn_next = static_cast<PanelCategoryDyn *>(pc_dyn.next);
      const int tab_v_pad = rct->ymin - pc_dyn_next->rect.ymax;
      region->category_scroll -= tab_v_pad;
    }
    break;
  }
  ED_region_tag_redraw(region);
  return WM_UI_HANDLER_BREAK;
}
/* -------------------------------------------------------------------- */
/** \name Panel Alignment
 * \{ */

static int get_panel_size_y(const Panel *panel)
{
  if (panel->type && (panel->type->flag & PANEL_TYPE_NO_HEADER)) {
    return panel->sizey;
  }

  return PNL_HEADER + panel->sizey;
}

static int get_panel_real_size_y(const Panel *panel)
{
  const int sizey = panel_is_closed(panel) ? 0 : panel->sizey;

  if (panel->type && (panel->type->flag & PANEL_TYPE_NO_HEADER)) {
    return sizey;
  }

  return PNL_HEADER + sizey;
}

int panel_size_y(const Panel *panel)
{
  return get_panel_real_size_y(panel);
}

/**
 * This function is needed because #Block and Panel itself don't
 * change #Panel.sizey or location when closed.
 */
static int get_panel_real_ofsy(Panel *panel)
{
  if (panel_is_closed(panel)) {
    return panel->ofsy + panel->sizey;
  }
  return panel->ofsy;
}

bool panel_is_dragging(const Panel *panel)
{
  return panel->runtime_flag & PANEL_IS_DRAG_DROP;
}

/**
 * \note about sorting:
 * The #Panel.sortorder has a lower value for new panels being added.
 * however, that only works to insert a single panel, when more new panels get
 * added the coordinates of existing panels and the previously stored to-be-inserted
 * panels do not match for sorting.
 */

static bool find_highest_panel(const PanelSort &a, const PanelSort &b)
{
  /* Stick uppermost header-less panels to the top of the region -
   * prevent them from being sorted (multiple header-less panels have to be sorted though). */
  if (a.panel->type->flag & PANEL_TYPE_NO_HEADER && b.panel->type->flag & PANEL_TYPE_NO_HEADER) {
    /* Pass the no-header checks and check for `ofsy` and #Panel.sortorder below. */
  }
  else if (a.panel->type->flag & PANEL_TYPE_NO_HEADER) {
    return true;
  }
  else if (b.panel->type->flag & PANEL_TYPE_NO_HEADER) {
    return false;
  }

  const bool pin_last_a = panel_custom_pin_to_last_get(a.panel);
  const bool pin_last_b = panel_custom_pin_to_last_get(b.panel);
  if (pin_last_a && !pin_last_b) {
    return false;
  }
  if (!pin_last_a && pin_last_b) {
    return true;
  }

  if (a.panel->ofsy + a.panel->sizey < b.panel->ofsy + b.panel->sizey) {
    return false;
  }
  if (a.panel->ofsy + a.panel->sizey > b.panel->ofsy + b.panel->sizey) {
    return true;
  }
  return a.panel->sortorder < b.panel->sortorder;
}

static bool compare_panel(const PanelSort &a, const PanelSort &b)
{
  return a.panel->sortorder < b.panel->sortorder;
}

static void align_sub_panels(Panel *panel)
{
  /* Position sub panels. */
  int ofsy = panel->ofsy + panel->sizey - panel->blocksizey;

  for (Panel &pachild : panel->children) {
    if (pachild.runtime_flag & PANEL_ACTIVE) {
      pachild.ofsx = panel->ofsx;
      pachild.ofsy = ofsy - get_panel_size_y(&pachild);
      ofsy -= get_panel_real_size_y(&pachild);

      if (pachild.children.first) {
        align_sub_panels(&pachild);
      }
    }
  }
}

/**
 * Calculate the position and order of panels as they are opened, closed, and dragged.
 */
static bool uiAlignPanelStep(ARegion *region, const float factor, const bool drag)
{
  Vector<PanelSort> panel_sort;
  for (Panel &panel : region->panels) {
    if (panel.runtime_flag & PANEL_ACTIVE) {
      /* These panels should have types since they are currently displayed to the user. */
      BLI_assert(panel.type != nullptr);
      panel_sort.append({&panel, 0, 0});
    }
  }
  if (panel_sort.is_empty()) {
    return false;
  }

  if (drag) {
    /* While dragging, sort based on location and update #Panel.sortorder. */
    std::stable_sort(panel_sort.begin(), panel_sort.end(), find_highest_panel);
    for (int i : panel_sort.index_range()) {
      panel_sort[i].panel->sortorder = i;
    }
  }
  else {
    /* Otherwise use #Panel.sortorder. */
    std::stable_sort(panel_sort.begin(), panel_sort.end(), compare_panel);
  }
  /* X offset. */
  const int region_offset_x = panel_region_offset_x_get(region);
  for (PanelSort &ps : panel_sort) {
    const bool show_background = panel_should_show_background(region, ps.panel->type);
    ps.panel->runtime->region_ofsx = region_offset_x;
    ps.new_offset_x = region_offset_x + (show_background ? UI_PANEL_MARGIN_X : 0);
  }

  /* Y offset. */
  int y = 0;
  for (PanelSort &ps : panel_sort) {
    const bool show_background = panel_should_show_background(region, ps.panel->type);

    y -= get_panel_real_size_y(ps.panel);

    /* Separate panel boxes a bit further (if they are drawn). */
    if (show_background) {
      y -= UI_PANEL_MARGIN_Y;
    }
    ps.new_offset_y = y;
    /* The header still draws offset by the size of closed panels, so apply the offset here. */
    if (panel_is_closed(ps.panel)) {
      ps.new_offset_y -= ps.panel->sizey;
    }
  }

  /* Interpolate based on the input factor. */
  bool changed = false;
  for (PanelSort &ps : panel_sort) {
    if (ps.panel->flag & PNL_SELECT) {
      continue;
    }

    if (ps.new_offset_x != ps.panel->ofsx) {
      const float x = interpf(float(ps.new_offset_x), float(ps.panel->ofsx), factor);
      ps.panel->ofsx = round_fl_to_int(x);
      changed = true;
    }
    if (ps.new_offset_y != ps.panel->ofsy) {
      const float y = interpf(float(ps.new_offset_y), float(ps.panel->ofsy), factor);
      ps.panel->ofsy = round_fl_to_int(y);
      changed = true;
    }
  }

  /* Set locations for tabbed and sub panels. */
  for (Panel &panel : region->panels) {
    if (panel.runtime_flag & PANEL_ACTIVE) {
      if (panel.children.first) {
        align_sub_panels(&panel);
      }
    }
  }

  return changed;
}

static void panels_size(ARegion *region, int *r_x, int *r_y)
{
  int sizex = 0;
  int sizey = 0;
  bool has_panel_with_background = false;

  /* Compute size taken up by panels, for setting in view2d. */
  for (Panel &panel : region->panels) {
    if (panel.runtime_flag & PANEL_ACTIVE) {
      const int pa_sizex = panel.ofsx + panel.sizex;
      const int pa_sizey = get_panel_real_ofsy(&panel);

      sizex = max_ii(sizex, pa_sizex);
      sizey = min_ii(sizey, pa_sizey);
      if (panel_should_show_background(region, panel.type)) {
        has_panel_with_background = true;
      }
    }
  }

  if (sizex == 0) {
    sizex = UI_PANEL_WIDTH;
  }
  if (sizey == 0) {
    sizey = -UI_PANEL_WIDTH;
  }
  /* Extra margin after the list so the view scrolls a few pixels further than the panel border.
   * Also makes the bottom match the top margin. */
  if (has_panel_with_background) {
    sizey -= UI_PANEL_MARGIN_Y;
  }

  *r_x = sizex;
  *r_y = sizey;
}

static void do_animate(bContext *C, Panel *panel)
{
  HandlePanelData *data = static_cast<HandlePanelData *>(panel->activedata);
  ARegion *region = CTX_wm_region(C);

  float fac = 1.0f;
  if (!(U.uiflag & USER_REDUCE_MOTION)) {
    fac = (BLI_time_now_seconds() - data->starttime) / ANIMATION_TIME;
    fac = min_ff(sqrtf(fac), 1.0f);
  }

  if (uiAlignPanelStep(region, fac, false)) {
    ED_region_tag_redraw(region);
  }
  else {
    if (panel_is_dragging(panel)) {
      /* NOTE: doing this in #panel_activate_state would require
       * removing `const` for context in many other places. */
      reorder_instanced_panel_list(C, region, panel);
    }

    panel_activate_state(C, panel, PANEL_STATE_EXIT);
  }
}

static void panels_layout_begin_clear_flags(ListBaseT<Panel> *lb)
{
  for (Panel &panel : *lb) {
    /* Flags to copy over to the next layout pass. */
    const short flag_copy = PANEL_USE_CLOSED_FROM_SEARCH | PANEL_IS_DRAG_DROP;

    const bool was_active = panel.runtime_flag & PANEL_ACTIVE;
    const bool was_closed = panel_is_closed(&panel);
    panel.runtime_flag &= flag_copy;
    SET_FLAG_FROM_TEST(panel.runtime_flag, was_active, PANEL_WAS_ACTIVE);
    SET_FLAG_FROM_TEST(panel.runtime_flag, was_closed, PANEL_WAS_CLOSED);

    panels_layout_begin_clear_flags(&panel.children);
  }
}

void panels_begin(const bContext * /*C*/, ARegion *region)
{
  /* Set all panels as inactive, so that at the end we know which ones were used. Also
   * clear other flags so we know later that their values were set for the current redraw. */
  panels_layout_begin_clear_flags(&region->panels);
}

void panels_end(const bContext *C, ARegion *region, int *r_x, int *r_y)
{
  ScrArea *area = CTX_wm_area(C);

  region_panels_set_expansion_from_list_data(C, region);

  const bool region_search_filter_active = region->flag & RGN_FLAG_SEARCH_FILTER_ACTIVE;

  if (properties_space_needs_realign(area, region)) {
    region_panels_set_expansion_from_search_filter(C, region, region_search_filter_active);
  }
  else if (region->flag & RGN_FLAG_SEARCH_FILTER_UPDATE) {
    region_panels_set_expansion_from_search_filter(C, region, region_search_filter_active);
  }

  if (region->flag & RGN_FLAG_SEARCH_FILTER_ACTIVE) {
    /* Clean up the extra panels and buttons created for searching. */
    region_panels_remove_invisible_layouts(region);
  }

  for (Panel &panel : region->panels) {
    if (panel.runtime_flag & PANEL_ACTIVE) {
      BLI_assert(panel.runtime->block != nullptr);
      panel_calculate_size_recursive(region, &panel);
    }
  }

  /* Offset contents. */
  for (Block &block : region->runtime->uiblocks) {
    if (block.active && block.panel) {
      offset_panel_block(&block);

      /* Update bounds for all "views" in this block. Usually this is done in #block_end(), but
       * that wouldn't work because of the offset applied above. */
      block_views_end(region, &block);
    }
  }

  /* Re-align, possibly with animation. */
  Panel *panel;
  if (panels_need_realign(area, region, &panel)) {
    if (panel) {
      panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
    }
    else {
      uiAlignPanelStep(region, 1.0, false);
    }
  }

  /* Compute size taken up by panels. */
  panels_size(region, r_x, r_y);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panel Dragging
 * \{ */

#define DRAG_REGION_PAD (PNL_HEADER * 0.5)
static void do_drag(const bContext *C, const wmEvent *event, Panel *panel)
{
  HandlePanelData *data = static_cast<HandlePanelData *>(panel->activedata);
  ARegion *region = CTX_wm_region(C);

  /* Keep the drag position in the region with a small pad to keep the panel visible. */
  const int y = clamp_i(event->xy[1], region->winrct.ymin, region->winrct.ymax + DRAG_REGION_PAD);

  float dy = float(y - data->starty);

  /* Adjust for region zoom. */
  dy *= BLI_rctf_size_y(&region->v2d.cur) / float(BLI_rcti_size_y(&region->winrct));

  /* Add the movement of the view due to edge scrolling while dragging. */
  dy += (region->v2d.cur.ymin - data->start_cur_ymin);

  panel->ofsy = data->startofsy + round_fl_to_int(dy);

  uiAlignPanelStep(region, 0.2f, true);

  ED_region_tag_redraw(region);
}
#undef DRAG_REGION_PAD

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Level Panel Interaction
 * \{ */

LayoutPanelHeader *layout_panel_header_under_mouse(const Panel &panel, const int my)
{
  for (LayoutPanelHeader &header : panel.runtime->layout_panels.headers) {
    if (IN_RANGE(float(my - panel.runtime->block->rect.ymax), header.start_y, header.end_y)) {
      return &header;
    }
  }
  return nullptr;
}

std::string get_tags_for_category_ui(const wmWindowManager *wm,
                                     const char *category,
                                     bool filter_show_all_modes,
                                     bool filter_current_mode,
                                     uint32_t current_mode_flag)
{
  if (wm == nullptr || category == nullptr) {
    return {};
  }

  if (!category_tag_list_is_valid(&wm->category_tags)) {
    return {};
  }

  const char *tags_string = category_tags_string_lookup(wm, category);

  std::string result;
  for (const CategoryTagDef *tag = static_cast<const CategoryTagDef *>(wm->category_tags.first); tag;
       tag = static_cast<const CategoryTagDef *>(tag->next))
  {
    if (tag->name[0] == '\0') {
      continue;
    }

    /* Apply filter logic:
     * - Both off: show all tags (default behavior)
     * - show_all_modes on: show all tags
     * - current_mode on: tags for current mode (mode_flags == 0 || mode_flags & current_mode_flag)
     * - Both on: combined (union of both conditions)
     */
    bool include_tag = false;

    if (filter_show_all_modes || !filter_current_mode) {
      /* Show all tags (default when both filters are off, or when show_all_modes is on) */
      include_tag = true;
    }
    else {
      /* Show tags for current mode or all modes */
      include_tag = (tag->mode_flags == 0) || (tag->mode_flags & current_mode_flag);
    }

    if (!include_tag) {
      continue;
    }

    const bool is_active = category_has_tag(tags_string, tag->name);

    char glyph_utf8[8] = "";
    const char *glyph_out = "";
    if (tag->glyph[0] != '\0') {
      if (tag_glyph_hex_to_utf8(tag->glyph, glyph_utf8)) {
        glyph_out = glyph_utf8;
      }
      else {
        glyph_out = tag->glyph;
      }
    }

    if (!result.empty()) {
      result.push_back(';');
    }
    result.append(tag->name);
    result.push_back('|');
    result.append(glyph_out);
    result.push_back('|');
    result.append(is_active ? "1" : "0");
    /* Add color in format r,g,b (0.0-1.0) */
    result.push_back('|');
    char color_str[32];
    SNPRINTF(color_str, "%.3f,%.3f,%.3f", tag->color[0], tag->color[1], tag->color[2]);
    result.append(color_str);
  }

  return result;
}

static PanelMouseState ui_panel_mouse_state_get(const Block *block,
                                                const Panel *panel,
                                                const int mx,
                                                const int my)
{
  if (!IN_RANGE(float(mx), block->rect.xmin, block->rect.xmax)) {
    return PANEL_MOUSE_OUTSIDE;
  }

  if (IN_RANGE(float(my), block->rect.ymax, block->rect.ymax + PNL_HEADER)) {
    return PANEL_MOUSE_INSIDE_HEADER;
  }
  if (layout_panel_header_under_mouse(*panel, my) != nullptr) {
    return PANEL_MOUSE_INSIDE_LAYOUT_PANEL_HEADER;
  }

  if (!panel_is_closed(panel)) {
    if (IN_RANGE(float(my), block->rect.ymin, block->rect.ymax + PNL_HEADER)) {
      return PANEL_MOUSE_INSIDE_CONTENT;
    }
  }

  return PANEL_MOUSE_OUTSIDE;
}

struct PanelDragCollapseHandle {
  bool was_first_open;
  int xy_init[2];
};

static void panel_drag_collapse_handler_remove(bContext * /*C*/, void *userdata)
{
  PanelDragCollapseHandle *dragcol_data = static_cast<PanelDragCollapseHandle *>(userdata);
  MEM_delete(dragcol_data);
}

static void panel_drag_collapse(const bContext *C,
                                const PanelDragCollapseHandle *dragcol_data,
                                const int xy_dst[2])
{
  ARegion *region = CTX_wm_region_popup(C);
  if (!region) {
    region = CTX_wm_region(C);
  }
  for (Block &block : region->runtime->uiblocks) {
    float xy_a_block[2] = {float(dragcol_data->xy_init[0]), float(dragcol_data->xy_init[1])};
    float xy_b_block[2] = {float(xy_dst[0]), float(xy_dst[1])};
    Panel *panel = block.panel;

    if (panel == nullptr) {
      continue;
    }

    /* Lock axis. */
    xy_b_block[0] = dragcol_data->xy_init[0];

    /* Use cursor coords in block space. */
    window_to_block_fl(region, &block, &xy_a_block[0], &xy_a_block[1]);
    window_to_block_fl(region, &block, &xy_b_block[0], &xy_b_block[1]);

    for (LayoutPanelHeader &header : panel->runtime->layout_panels.headers) {
      rctf rect = block.rect;
      rect.ymin = block.rect.ymax + header.start_y;
      rect.ymax = block.rect.ymax + header.end_y;

      if (BLI_rctf_isect_segment(&rect, xy_a_block, xy_b_block)) {
        RNA_boolean_set(
            &header.open_owner_ptr, header.open_prop_name.c_str(), !dragcol_data->was_first_open);
        RNA_property_update(
            const_cast<bContext *>(C),
            &header.open_owner_ptr,
            RNA_struct_find_property(&header.open_owner_ptr, header.open_prop_name.c_str()));
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    }

    if (panel->type && (panel->type->flag & PANEL_TYPE_NO_HEADER)) {
      continue;
    }
    const int oldflag = panel->flag;

    /* Set up `rect` to match header size. */
    rctf rect = block.rect;
    rect.ymin = rect.ymax;
    rect.ymax = rect.ymin + PNL_HEADER;

    /* Touch all panels between last mouse coordinate and the current one. */
    if (BLI_rctf_isect_segment(&rect, xy_a_block, xy_b_block)) {
      /* Force panel to open or close. */
      panel->runtime_flag &= ~PANEL_USE_CLOSED_FROM_SEARCH;
      SET_FLAG_FROM_TEST(panel->flag, dragcol_data->was_first_open, PNL_CLOSED);

      /* If panel->flag has changed this means a panel was opened/closed here. */
      if (panel->flag != oldflag) {
        panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
      }
    }
  }
  /* Update the instanced panel data expand flags with the changes made here. */
  set_panels_list_data_expand_flag(C, region);
}

/**
 * Panel drag-collapse (modal handler).
 * Clicking and dragging over panels toggles their collapse state based on the panel
 * that was first dragged over. If it was open all affected panels including the initial
 * one are closed and vice versa.
 */
static int panel_drag_collapse_handler(bContext *C, const wmEvent *event, void *userdata)
{
  wmWindow *win = CTX_wm_window(C);
  PanelDragCollapseHandle *dragcol_data = static_cast<PanelDragCollapseHandle *>(userdata);
  short retval = WM_UI_HANDLER_CONTINUE;

  switch (event->type) {
    case MOUSEMOVE:
      panel_drag_collapse(C, dragcol_data, event->xy);

      retval = WM_UI_HANDLER_BREAK;
      break;
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        /* Done! */
        WM_event_remove_ui_handler(&win->runtime->modalhandlers,
                                   panel_drag_collapse_handler,
                                   panel_drag_collapse_handler_remove,
                                   dragcol_data,
                                   true);
        panel_drag_collapse_handler_remove(C, dragcol_data);
      }
      /* Don't let any left-mouse event fall through! */
      retval = WM_UI_HANDLER_BREAK;
      break;
    default: {
      break;
    }
  }

  return retval;
}

void panel_drag_collapse_handler_add(const bContext *C, const bool was_open)
{
  wmWindow *win = CTX_wm_window(C);
  const wmEvent *event = win->runtime->eventstate;
  PanelDragCollapseHandle *dragcol_data = MEM_new_zeroed<PanelDragCollapseHandle>(__func__);

  dragcol_data->was_first_open = was_open;
  copy_v2_v2_int(dragcol_data->xy_init, event->xy);

  WM_event_add_ui_handler(C,
                          &win->runtime->modalhandlers,
                          panel_drag_collapse_handler,
                          panel_drag_collapse_handler_remove,
                          dragcol_data,
                          eWM_EventHandlerFlag(0));
}

bool layout_panel_toggle_open(const bContext *C, LayoutPanelHeader *header)
{
  const bool is_open = RNA_boolean_get(&header->open_owner_ptr, header->open_prop_name.c_str());
  RNA_boolean_set(&header->open_owner_ptr, header->open_prop_name.c_str(), !is_open);
  RNA_property_update(
      const_cast<bContext *>(C),
      &header->open_owner_ptr,
      RNA_struct_find_property(&header->open_owner_ptr, header->open_prop_name.c_str()));
  return !is_open;
}

static void handle_layout_panel_header(
    bContext *C, const Block *block, const int /*mx*/, const int my, const int event_type)
{
  Panel *panel = block->panel;
  BLI_assert(panel->type != nullptr);

  LayoutPanelHeader *header = layout_panel_header_under_mouse(*panel, my);
  if (header == nullptr) {
    return;
  }
  const bool new_state = layout_panel_toggle_open(C, header);
  ED_region_tag_redraw(CTX_wm_region(C));
  WM_tooltip_clear(C, CTX_wm_window(C));

  if (event_type == LEFTMOUSE) {
    panel_drag_collapse_handler_add(C, !new_state);
  }
}

/**
 * Supposing the block has a panel and isn't a menu, handle opening, closing, pinning, etc.
 * Code currently assumes layout style for location of widgets
 *
 * \param mx: The mouse x coordinate, in panel space.
 */
static void handle_panel_header(const bContext *C,
                                const Block *block,
                                const int mx,
                                const int event_type,
                                const bool ctrl,
                                const bool shift)
{
  Panel *panel = block->panel;
  ARegion *region = CTX_wm_region(C);

  BLI_assert(panel->type != nullptr);
  BLI_assert(!(panel->type->flag & PANEL_TYPE_NO_HEADER));

  const bool is_subpanel = (panel->type->parent != nullptr);
  const bool use_pin = panel_category_tabs_is_visible(region) && panel_can_be_pinned(panel);
  const bool show_pin = use_pin && (panel->flag & PNL_PIN);
  const bool show_drag = !is_subpanel;

  /* Handle panel pinning. */
  if (use_pin && ELEM(event_type, EVT_RETKEY, EVT_PADENTER, LEFTMOUSE) && shift) {
    panel->flag ^= PNL_PIN;
    ED_region_tag_redraw(region);
    return;
  }

  float expansion_area_xmax = block->rect.xmax;
  if (show_drag) {
    expansion_area_xmax -= (PNL_ICON * 1.5f);
  }
  if (show_pin) {
    expansion_area_xmax -= PNL_ICON;
  }

  /* Collapse and expand panels. */
  if (ELEM(event_type, EVT_RETKEY, EVT_PADENTER, EVT_AKEY) || mx < expansion_area_xmax) {
    if (ctrl && !is_subpanel) {
      /* For parent panels, collapse all other panels or toggle children. */
      if (panel_is_closed(panel) || BLI_listbase_is_empty(&panel->children)) {
        panels_collapse_all(region, panel);

        /* Reset the view - we don't want to display a view without content. */
        view2d_offset(&region->v2d, 0.0f, 1.0f);
      }
      else {
        /* If a panel has sub-panels and it's open, toggle the expansion
         * of the sub-panels (based on the expansion of the first sub-panel). */
        Panel *first_child = static_cast<Panel *>(panel->children.first);
        BLI_assert(first_child != nullptr);
        panel_set_flag_recursive(panel, PNL_CLOSED, !panel_is_closed(first_child));
        panel->flag |= PNL_CLOSED;
      }
    }

    SET_FLAG_FROM_TEST(panel->flag, !panel_is_closed(panel), PNL_CLOSED);

    if (event_type == LEFTMOUSE) {
      panel_drag_collapse_handler_add(C, panel_is_closed(panel));
    }

    /* Set panel custom data (modifier) active when expanding sub-panels, but not top-level
     * panels to allow collapsing and expanding without setting the active element. */
    if (is_subpanel) {
      panel_custom_data_active_set(panel);
    }

    set_panels_list_data_expand_flag(C, region);
    panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
    return;
  }

  /* Handle panel dragging. For now don't allow dragging in floating regions. */
  if (show_drag && !(region->alignment == RGN_ALIGN_FLOAT)) {
    const float drag_area_xmin = block->rect.xmax - (PNL_ICON * 1.5f);
    const float drag_area_xmax = block->rect.xmax;
    if (IN_RANGE(mx, drag_area_xmin, drag_area_xmax)) {
      if (panel_custom_pin_to_last_get(panel)) {
        panel_custom_pin_to_last_set(C, panel, false);
        return;
      }
      panel_activate_state(C, panel, PANEL_STATE_DRAG);
      return;
    }
  }

  /* Handle panel unpinning. */
  if (show_pin) {
    const float pin_area_xmin = expansion_area_xmax;
    const float pin_area_xmax = pin_area_xmin + PNL_ICON;
    if (IN_RANGE(mx, pin_area_xmin, pin_area_xmax)) {
      panel->flag ^= PNL_PIN;
      ED_region_tag_redraw(region);
      return;
    }
  }
}

bool panel_category_is_visible(const ARegion *region)
{
  /* Check for more than one category. */
  return region->runtime->panels_category.first &&
         (!bool(region->runtime->type->flag & ARegionTypeFlag::HideSinglePanelCategories) ||
          region->runtime->panels_category.first != region->runtime->panels_category.last);
}

bool panel_category_tabs_is_visible(const ARegion *region)
{
  return panel_category_is_visible(region) &&
         BKE_regiontype_uses_category_tabs(region->runtime->type);
}

PanelCategoryDyn *panel_category_find(const ARegion *region, const char *idname)
{
  return static_cast<PanelCategoryDyn *>(BLI_findstring(
      &region->runtime->panels_category, idname, offsetof(PanelCategoryDyn, idname)));
}

int panel_category_index_find(ARegion *region, const char *idname)
{
  return BLI_findstringindex(
      &region->runtime->panels_category, idname, offsetof(PanelCategoryDyn, idname));
}

PanelCategoryStack *panel_category_active_find(ARegion *region, const char *idname)
{
  return static_cast<PanelCategoryStack *>(BLI_findstring(
      &region->panels_category_active, idname, offsetof(PanelCategoryStack, idname)));
}

static void panel_category_active_set(ARegion *region, const char *idname, bool fallback)
{
  ListBaseT<PanelCategoryStack> *lb = &region->panels_category_active;
  PanelCategoryStack *pc_act = panel_category_active_find(region, idname);

  if (pc_act) {
    BLI_remlink(lb, pc_act);
  }
  else {
    pc_act = MEM_new<PanelCategoryStack>(__func__);
    STRNCPY_UTF8(pc_act->idname, idname);
  }

  if (fallback) {
    /* For fall-backs, add at the end so explicitly chosen categories have priority. */
    BLI_addtail(lb, pc_act);
  }
  else {
    BLI_addhead(lb, pc_act);
  }

  /* Validate all active panels. We could do this on load, they are harmless -
   * but we should remove them somewhere.
   * (Add-ons could define panels and gather cruft over time). */
  {
    PanelCategoryStack *pc_act_next;
    /* intentionally skip first */
    pc_act_next = pc_act->next;
    while ((pc_act = pc_act_next)) {
      pc_act_next = pc_act->next;
      if (!BLI_findstring(
              &region->runtime->type->paneltypes, pc_act->idname, offsetof(PanelType, category)))
      {
        BLI_remlink(lb, pc_act);
        MEM_delete(pc_act);
      }
    }
  }
  ED_region_tag_redraw(region);
}

void panel_category_active_set(ARegion *region, const char *idname)
{
  panel_category_active_set(region, idname, false);
}

void panel_category_index_active_set(ARegion *region, const int index)
{
  PanelCategoryDyn *pc_dyn = static_cast<PanelCategoryDyn *>(
      BLI_findlink(&region->runtime->panels_category, index));
  if (!pc_dyn) {
    return;
  }

  panel_category_active_set(region, pc_dyn->idname, false);
}

void panel_category_active_set_default(ARegion *region, const char *idname)
{
  if (!panel_category_active_find(region, idname)) {
    panel_category_active_set(region, idname, true);
  }
}

const char *panel_category_active_get(ARegion *region, bool set_fallback)
{
  for (PanelCategoryStack &pc_act : region->panels_category_active) {
    if (panel_category_find(region, pc_act.idname)) {
      return pc_act.idname;
    }
  }

  if (set_fallback) {
    PanelCategoryDyn *pc_dyn = static_cast<PanelCategoryDyn *>(
        region->runtime->panels_category.first);
    if (pc_dyn) {
      panel_category_active_set(region, pc_dyn->idname, true);
      return pc_dyn->idname;
    }
  }

  return nullptr;
}

static PanelCategoryDyn *panel_categories_find_mouse_over(ARegion *region, const wmEvent *event)
{
  BLI_assert(BKE_regiontype_uses_category_tabs(region->runtime->type));

  for (PanelCategoryDyn &ptd : region->runtime->panels_category) {
    if (BLI_rcti_isect_pt(&ptd.rect, event->mval[0], event->mval[1])) {
      return &ptd;
    }
  }

  return nullptr;
}

bool panel_category_is_mouse_over(ARegion *region, const wmEvent *event)
{
  if (!panel_category_tabs_is_visible(region)) {
    return false;
  }
  return panel_categories_find_mouse_over(region, event) != nullptr;
}

bool panel_category_tabs_settings_contains(ARegion *region, const int mval[2])
{
  if (!panel_category_tabs_is_visible(region)) {
    return false;
  }

  const rcti *rct = &region->runtime->category_tabs_settings_rect;

  /* Check if mval is inside the settings button rect. */
  return BLI_rcti_isect_pt(rct, mval[0], mval[1]);
}

void panel_category_tabs_settings_popover_open(bContext *C, ARegion *region)
{
  /* Store click time for hover reset timeout. */
  region->runtime->category_tabs_settings_click_time = BLI_time_now_seconds();

  /* Invoke the Python operator which shows the settings popup. */
  WM_operator_name_call(C, "VIEW3D_OT_category_tabs_settings", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
}

static ARegion *ui_panel_category_tooltip_init(
    bContext *C, ARegion *region, int * /*pass*/, double * /*r_pass_delay*/, bool *r_exit_on_event)
{
  *r_exit_on_event = true;

  wmWindow *win = CTX_wm_window(C);
  const wmEvent *event = win->runtime->eventstate;

  if (!region) {
    return nullptr;
  }

  /* Get display mode from preferences.
   * In TEXT_ONLY mode the category name is already visible, so don't show tooltips.
   * In GLYPHS_ONLY and GLYPHS_TEXT (Mixed) modes, tooltips are useful. */
  const eUserPref_CategoryTabsDisplayMode display_mode =
      static_cast<eUserPref_CategoryTabsDisplayMode>(U.category_tabs_display_mode);

  if (display_mode == USER_CATEGORY_TABS_TEXT_ONLY) {
    return nullptr;
  }

  /* Calculate mval from screen coordinates. */
  int mval[2];
  mval[0] = event->xy[0] - region->winrct.xmin;
  mval[1] = event->xy[1] - region->winrct.ymin;

  /* Determine if tabs are on the left or right side. */
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;

  /* Get window manager for category display name lookup. */
  const wmWindowManager *wm = CTX_wm_manager(C);

  /* Find the category tab under the mouse. */
  for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (BLI_rcti_isect_pt(&pc_dyn.rect, mval[0], mval[1])) {
      const char *category_idname = pc_dyn.idname;

      std::string tooltip_text;

      if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY) {
        /* In GLYPHS_ONLY mode, show the category display name (from user override or panel label). */
        const char *category_display_name = panel_category_tooltip_name_get(region, wm, category_idname);
        tooltip_text = IFACE_(category_display_name);
      }
      else {
        /* In GLYPHS_TEXT mode, collect panel names in this category. */
        Vector<std::string> panel_names;
        for (const Panel &panel : region->panels) {
          if (panel.type && STREQ(panel.type->category, category_idname)) {
            const char *label = CTX_IFACE_(panel.type->translation_context, panel.type->label);
            if (label && label[0]) {
              panel_names.append(label);
            }
          }
        }

        if (!panel_names.is_empty()) {
          /* Join panel names with commas. */
          tooltip_text = panel_names[0];
          for (int i = 1; i < panel_names.size(); i++) {
            tooltip_text += ", " + panel_names[i];
          }
        }
        else {
          /* Fallback to category name if no panels found. */
          const char *category_display_name = panel_category_tooltip_name_get(region, wm, category_idname);
          tooltip_text = IFACE_(category_display_name);
        }
      }

      /* Position tooltip to avoid overlapping the tab.
       * Convert tab rect from region-local to screen coordinates.
       * Use mouse Y position for the rect to keep tooltip aligned with cursor vertically. */
      rcti tab_rect_screen;
      tab_rect_screen.xmin = region->winrct.xmin + pc_dyn.rect.xmin;
      tab_rect_screen.xmax = region->winrct.xmin + pc_dyn.rect.xmax;
      /* Use mouse Y position to keep tooltip vertically aligned with cursor. */
      tab_rect_screen.ymin = event->xy[1] - UI_UNIT_Y / 2;
      tab_rect_screen.ymax = event->xy[1] + UI_UNIT_Y / 2;

      int position[2];
      if (is_left) {
        /* Tabs on left side: position tooltip to the right of tabs. */
        position[0] = tab_rect_screen.xmax + UI_POPUP_MARGIN;
      }
      else {
        /* Tabs on right side: position tooltip to the left of tabs. */
        position[0] = tab_rect_screen.xmin - UI_POPUP_MARGIN;
      }
      position[1] = event->xy[1];

      /* Use init_rect_overlap to ensure tooltip doesn't overlap the tab.
       * For tabs on right side, prefer left side positioning first. */
      const bool prefer_left = !is_left;
      return tooltip_create_from_text(
          C, tooltip_text.c_str(), position, &tab_rect_screen, prefer_left);
    }
  }

  return nullptr;
}

static ARegion *ui_panel_category_active_tooltip_init(
    bContext *C, ARegion *region, int *pass, double *r_pass_delay, bool *r_exit_on_event)
{
  *r_exit_on_event = true;

  if (*pass == 1) {
    return nullptr; /* Hide after delay. */
  }

  /* pass == 0 */
  *pass = 1;
  *r_pass_delay = 2.0; /* Hide after 2 seconds. */

  if (region == nullptr) {
    return nullptr;
  }

  const char *category_idname = panel_category_active_get(region, false);
  if (category_idname == nullptr) {
    return nullptr;
  }

  const std::string tooltip_text = std::string("Active tab: ") + IFACE_(category_idname);

  wmWindow *win = CTX_wm_window(C);
  const wmEvent *event = win->runtime->eventstate;

  /* Find the category tab for the active category. */
  const PanelCategoryDyn *pc_dyn = panel_category_find(region, category_idname);
  
  rcti tab_rect_screen;
  bool use_tab_rect = false;

  int position[2];

  if (pc_dyn) {
      tab_rect_screen.xmin = region->winrct.xmin + pc_dyn->rect.xmin;
      tab_rect_screen.xmax = region->winrct.xmin + pc_dyn->rect.xmax;
      tab_rect_screen.ymin = event->xy[1] - UI_UNIT_Y / 2;
      tab_rect_screen.ymax = event->xy[1] + UI_UNIT_Y / 2;

      const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
      if (is_left) {
        position[0] = tab_rect_screen.xmax + UI_POPUP_MARGIN / 4;
      }
      else {
        position[0] = tab_rect_screen.xmin - UI_POPUP_MARGIN / 4;
      }
      position[1] = event->xy[1];
      use_tab_rect = true;
  } else {
      position[0] = event->xy[0];
      position[1] = event->xy[1] - UI_POPUP_MARGIN / 4;
  }

  /* ─────────────────────────────────────────────────────────────────────────────
   * TAB SCROLL TOOLTIP POSITIONING
   * ─────────────────────────────────────────────────────────────────────────────
   * When scrolling through tabs with Ctrl+MouseWheel, position the tooltip:
   * - At window center Y if editor area > 50% of window height (large editor)
   * - At mouse cursor Y if editor area <= 50% of window height (small editor)
   *
   * Configurable parameters:
   * - Threshold: win_size[1] / 2 (50% of window height)
   * - Both position[1] and tab_rect_screen Y must be updated for correct positioning
   * ───────────────────────────────────────────────────────────────────────────── */
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    const int2 win_size = WM_window_native_pixel_size(win);
    const int area_height = BLI_rcti_size_y(&area->totrct);
    const int threshold = win_size[1] / 2;  /* 50% of window height */

    if (area_height > threshold) {
      /* Large editor: position tooltip at window center. */
      rcti win_rect;
      WM_window_rect_calc(win, &win_rect);
      position[1] = BLI_rcti_cent_y(&win_rect);
      /* Update tab_rect_screen Y so tooltip_create_from_text_fixed_width
       * positions correctly relative to the centered rect. */
      if (use_tab_rect) {
        tab_rect_screen.ymin = position[1] - UI_UNIT_Y / 2;
        tab_rect_screen.ymax = position[1] + UI_UNIT_Y / 2;
      }
    }
  }

  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  const bool prefer_left = !is_left;

  const uiStyle *style = style_get();
  uiFontStyle fstyle = style->tooltip;
  fontstyle_set(&fstyle);
  BLF_size(fstyle.uifont_id, fstyle.points * UI_SCALE_FAC);
  const int font_id = fstyle.uifont_id;

  int max_text_width = 0;
  for (const PanelCategoryDyn &pc_dyn_it : region->runtime->panels_category) {
    const std::string text_it = std::string("Active tab: ") + IFACE_(pc_dyn_it.idname);
    ResultBLF info = {0};
    const int text_width = BLF_width(font_id, text_it.c_str(), text_it.size(), &info);
    max_text_width = max_ii(max_text_width, text_width);
  }

  const int lineh = BLF_height_max(font_id);
  const int min_width = max_text_width + int(round(lineh * 1.95f));

  return tooltip_create_from_text_fixed_width(C,
                                             tooltip_text.c_str(),
                                             position,
                                             use_tab_rect ? &tab_rect_screen : nullptr,
                                             prefer_left,
                                             min_width);
}

void panel_category_tooltip_timer_init(bContext *C, ARegion *region)
{
  wmWindow *win = CTX_wm_window(C);
  ScrArea *area = CTX_wm_area(C);

  if ((U.flag & USER_TOOLTIPS) == 0) {
    return;
  }

  WM_tooltip_timer_init(C, win, area, region, ui_panel_category_tooltip_init);
}

void panel_category_add(ARegion *region, const char *name)
{
  PanelCategoryDyn *pc_dyn = MEM_new<PanelCategoryDyn>(__func__);
  BLI_addtail(&region->runtime->panels_category, pc_dyn);

  STRNCPY_UTF8(pc_dyn->idname, name);

  /* 'pc_dyn->rect' must be set on draw. */
}

void panel_category_clear_all(ARegion *region)
{
  BLI_freelistN(&region->runtime->panels_category);
}

/**
 * Handle tab cycling with Ctrl+MouseWheel or Ctrl+Tab.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * TAB CYCLING BEHAVIOR
 * ─────────────────────────────────────────────────────────────────────────────
 * - Cycles through VISIBLE categories only (respects tag filtering)
 * - Stops at first/last visible category (no wrapping)
 * - Skips hidden categories (filtered by panel_category_is_visible_by_tags)
 *
 * Controls:
 * - Ctrl+WheelDown / Ctrl+Tab: next category
 * - Ctrl+WheelUp / Ctrl+Shift+Tab: previous category
 * ─────────────────────────────────────────────────────────────────────────────
 */
static int ui_handle_panel_category_cycling(bContext *C,
                                            const wmEvent *event,
                                            ARegion *region,
                                            const Button *active_but)
{
  BLI_assert(BKE_regiontype_uses_category_tabs(region->runtime->type));

  const wmWindowManager *wm = CTX_wm_manager(C);
  const bool is_mousewheel = ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE);
  const bool inside_tabregion =
      ((RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT) ?
           (event->mval[0] <
            (static_cast<PanelCategoryDyn *>(region->runtime->panels_category.first))->rect.xmax) :
           (event->mval[0] >
            (static_cast<PanelCategoryDyn *>(region->runtime->panels_category.first))->rect.xmin));

  /* If mouse is inside non-tab region, ctrl key is required. */
  if (is_mousewheel && (event->modifier & KM_CTRL) == 0 && !inside_tabregion) {
    return WM_UI_HANDLER_CONTINUE;
  }

  if (active_but && button_supports_cycling(active_but)) {
    /* Skip - exception to make cycling buttons using ctrl+mousewheel work in tabbed regions. */
  }
  else {
    const char *category = panel_category_active_get(region, false);
    if (LIKELY(category)) {
      PanelCategoryDyn *pc_dyn = panel_category_find(region, category);
      if (LIKELY(pc_dyn) && (event->modifier & KM_CTRL)) {
        const bool backwards = is_mousewheel ? (event->type == WHEELUPMOUSE) : (event->modifier & KM_SHIFT);

        /* Find next visible category, skipping hidden ones.
         * Stops at first/last visible category (no wrapping). */
        PanelCategoryDyn *next = backwards ? pc_dyn->prev : pc_dyn->next;

        while (next && !panel_category_is_visible_by_tags(C, wm, next->idname)) {
          next = backwards ? next->prev : next->next;
        }

        if (next) {
          /* Intentionally don't reset scroll in this case,
           * allowing for quick browsing between tabs. */
          panel_category_active_set(region, next->idname);
        }
        return WM_UI_HANDLER_BREAK;
      }
    }
  }

  return WM_UI_HANDLER_CONTINUE;
}

static void panel_region_width_set(ARegion *region, const float aspect, int unscaled_size)
{
  const float size_new = unscaled_size / aspect;
  if (region->alignment & RGN_ALIGN_RIGHT) {
    region->winrct.xmin = region->winrct.xmax - (size_new * UI_SCALE_FAC);
  }
  else {
    region->winrct.xmax = region->winrct.xmin + (size_new * UI_SCALE_FAC);
  }
  region->winx = size_new * UI_SCALE_FAC;
  region->sizex = size_new;
  region->v2d.winx = region->winx;
  region->v2d.cur.xmin = 0;
  region->v2d.cur.xmax = size_new * UI_SCALE_FAC;
  region->v2d.mask.xmin = 0;
  region->v2d.mask.xmax = size_new * UI_SCALE_FAC;
  view2d_curRect_validate(&region->v2d);
}

int handler_panel_region(bContext *C,
                         const wmEvent *event,
                         ARegion *region,
                         const Button *active_but)
{
  /* Handle mouse motion for settings button hover state. */
  if (ISMOUSE_MOTION(event->type)) {
    if (panel_category_tabs_is_visible(region)) {
      /* Check if mouse is over the settings button and trigger redraw for hover effect. */
      const rcti *rct = &region->runtime->category_tabs_settings_rect;

      /* Check if mouse is inside the region bounds first. */
      const bool mouse_in_region = BLI_rcti_isect_pt(&region->winrct, event->xy[0], event->xy[1]);
      const bool is_over_settings = mouse_in_region && BLI_rcti_isect_pt(rct, event->mval[0], event->mval[1]);

      if (is_over_settings != region->runtime->category_tabs_settings_hover) {
        if (is_over_settings) {
          /* Hover just became true - record the time. */
          region->runtime->category_tabs_settings_hover_time = BLI_time_now_seconds();
          region->runtime->category_tabs_settings_hover = true;
          ED_region_tag_redraw(region);
        }
        else {
          /* Hover just became false. */
          region->runtime->category_tabs_settings_hover = false;
          ED_region_tag_redraw(region);
        }
      }

      /* Check if mouse is over any category tab for hover effect. */
      bool is_over_any_tab = false;
      for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
        if (BLI_rcti_isect_pt(&pc_dyn.rect, event->mval[0], event->mval[1])) {
          is_over_any_tab = true;
          break;
        }
      }
      /* Redraw on mouse motion in region to update hover effect (including when leaving a tab). */
      if (mouse_in_region) {
        ED_region_tag_redraw(region);
      }

      /* Check for drag threshold exceeded to start category tab drag */
      if (region->runtime->category_tabs_drag_pending_id[0] != '\0') {
        const int drag_delta_y = abs(event->mval[1] - region->runtime->category_tabs_drag_start_y);
        const double time_elapsed = BLI_time_now_seconds() -
                                     region->runtime->category_tabs_drag_start_time;

        if (drag_delta_y > CATEGORY_DRAG_THRESHOLD_PX ||
            time_elapsed > CATEGORY_DRAG_DELAY_SEC)
        {
          if (U.category_tabs_allow_edit) {
             return WM_UI_HANDLER_CONTINUE;
           }

          /* Start the drag operator */
          wmOperatorType *ot = WM_operatortype_find("UI_OT_category_tab_drag", true);
          if (ot) {
            /* Clear pending state before invoking operator */
            region->runtime->category_tabs_drag_pending_id[0] = '\0';

            /* Create a modified event with the original start position */
            wmEvent drag_event = *event;
            drag_event.mval[1] = region->runtime->category_tabs_drag_start_y;

            WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, nullptr, &drag_event);
            /* Return BREAK - modal operator now handles events */
            return WM_UI_HANDLER_BREAK;
          }
        }
      }
    }

    /* Note: Hover state reset for ALL regions is now handled by the area-level
     * hover handler (area_category_tabs_hover_handler) which runs for all regions
     * in an area, ensuring hover resets when mouse moves between regions. */

    return WM_UI_HANDLER_CONTINUE;
  }

  /* We only use KM_PRESS events in this function, so it's simpler to return early. */
  if (event->val != KM_PRESS) {
    /* Handle LEFTMOUSE RELEASE for pending drag state */
    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
      if (region->runtime->category_tabs_drag_pending_id[0] != '\0') {
        /* This was a click, not a drag - handle normally */
        PanelCategoryDyn *pc_dyn = panel_category_find(region,
                                                        region->runtime->category_tabs_drag_pending_id);
        if (pc_dyn) {
          const bool already_active = STREQ(pc_dyn->idname,
                                            panel_category_active_get(region, false));
          panel_category_active_set(region, pc_dyn->idname);

          const float aspect = BLI_rctf_size_y(&region->v2d.cur) /
                               (BLI_rcti_size_y(&region->v2d.mask) + 1);
          const bool too_narrow = BLI_rcti_size_x(&region->winrct) <=
                                  int(std::ceil(UI_PANEL_CATEGORY_MIN_WIDTH * UI_SCALE_FAC /
                                                aspect));
          if (too_narrow) {
            /* Enlarge region. */
            const int new_width = region->runtime->type->prefsizex ?
                                      region->runtime->type->prefsizex :
                                      250;
            ui_panel_region_width_set(region, aspect, new_width);
            WM_event_add_notifier(C, NC_SCREEN | NA_EDITED, nullptr);
          }
          else if (already_active) {
            /* Minimize region. */
            region->runtime->type->prefsizex = int(float(BLI_rcti_size_x(&region->winrct) + 1) /
                                                   UI_SCALE_FAC * aspect);
            ui_panel_region_width_set(region, aspect, UI_PANEL_CATEGORY_MIN_WIDTH);
            WM_event_add_notifier(C, NC_SCREEN | NA_EDITED, nullptr);
          }

          ED_region_tag_redraw(region);

          /* Reset scroll to the top (#38348). */
          view2d_offset(&region->v2d, -1.0f, 1.0f);
        }

        /* Clear pending state */
        region->runtime->category_tabs_drag_pending_id[0] = '\0';
        return WM_UI_HANDLER_BREAK;
      }
    }
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Scroll-bars can overlap panels now, they have handling priority. */
  if (view2d_mouse_in_scrollers(region, &region->v2d, event->xy)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  int retval = WM_UI_HANDLER_CONTINUE;

  /* Handle category tabs. */
  if (panel_category_tabs_is_visible(region)) {
    if (event->type == LEFTMOUSE) {
      /* Check settings button first (it's at the bottom). */
      if (panel_category_tabs_settings_contains(region, event->mval)) {
        panel_category_tabs_settings_popover_open(C, region);
        ED_region_tag_redraw(region);
        return WM_UI_HANDLER_BREAK;
      }

      PanelCategoryDyn *pc_dyn = panel_categories_find_mouse_over(region, event);
      if (pc_dyn) {
        /* Store pending drag state - allow drag for all categories for now */
        STRNCPY(region->runtime->category_tabs_drag_pending_id, pc_dyn->idname);
        region->runtime->category_tabs_drag_start_y = event->mval[1];
        region->runtime->category_tabs_drag_start_time = BLI_time_now_seconds();
        /* Return CONTINUE to keep receiving MOUSEMOVE events for drag detection */
        retval = WM_UI_HANDLER_CONTINUE;
      }
    }
    else if (((event->type == EVT_TABKEY) && (event->modifier & KM_CTRL)) ||
             ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE))
    {
      /* Cycle tabs. */
      retval = ui_handle_panel_category_cycling(C, event, region, active_but);
      if (retval == WM_UI_HANDLER_BREAK) {
        /* Show or update tooltip with active tab name. */
        wmWindow *win = CTX_wm_window(C);
        const char *category_idname = panel_category_active_get(region, false);

        if (category_idname) {
          const std::string tooltip_text = std::string("Active tab: ") +
                                           IFACE_(category_idname);

          /* Try to update existing tooltip first to avoid flickering. */
          if (!WM_tooltip_update_text(C, win, tooltip_text.c_str())) {
            /* No existing tooltip - create new one. */
            WM_tooltip_immediate_init(C,
                                      win,
                                      CTX_wm_area(C),
                                      region,
                                      ui_panel_category_active_tooltip_init);
          }
        }
      }
    }
    if (event->type == EVT_PADPERIOD) {
      retval = panel_category_show_active_tab(region, event->xy);
    }
  }

  if (retval == WM_UI_HANDLER_BREAK) {
    return retval;
  }

  const Button *region_active_but = region_find_active_but(region);
  const bool region_has_active_button = region_active_but &&
                                        region_active_but->type != ButtonType::Label;

  for (Block &block : region->runtime->uiblocks) {
    Panel *panel = block.panel;
    if (panel == nullptr || panel->type == nullptr) {
      continue;
    }
    /* We can't expand or collapse panels without headers, they would disappear. Layout panels can
     * be expanded and collapsed though. */
    const bool has_panel_header = !(panel->type->flag & PANEL_TYPE_NO_HEADER);

    int mx = event->xy[0];
    int my = event->xy[1];
    window_to_block(region, &block, &mx, &my);

    const PanelMouseState mouse_state = panel_mouse_state_get(&block, panel, mx, my);

    if (has_panel_header && mouse_state != PANEL_MOUSE_OUTSIDE) {
      /* Mark panels that have been interacted with so their expansion
       * doesn't reset when property search finishes. */
      SET_FLAG_FROM_TEST(panel->flag, panel_is_closed(panel), PNL_CLOSED);
      panel->runtime_flag &= ~PANEL_USE_CLOSED_FROM_SEARCH;

      /* The panel collapse / expand key "A" is special as it takes priority over
       * active button handling. */
      if ((event->type == EVT_AKEY) && (event->modifier == 0)) {
        retval = WM_UI_HANDLER_BREAK;
        handle_panel_header(
            C, &block, mx, event->type, event->modifier & KM_CTRL, event->modifier & KM_SHIFT);
        break;
      }
    }

    /* Don't do any other panel handling with an active button. */
    if (region_has_active_button) {
      continue;
    }

    if (has_panel_header && mouse_state == PANEL_MOUSE_INSIDE_HEADER) {
      /* All mouse clicks inside panel headers should return in break. */
      if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER, LEFTMOUSE)) {
        retval = WM_UI_HANDLER_BREAK;
        handle_panel_header(
            C, &block, mx, event->type, event->modifier & KM_CTRL, event->modifier & KM_SHIFT);
      }
      else if (event->type == RIGHTMOUSE) {
        retval = WM_UI_HANDLER_BREAK;
        popup_context_menu_for_panel(C, region, block.panel);
      }
      break;
    }
    if (mouse_state == PANEL_MOUSE_INSIDE_LAYOUT_PANEL_HEADER) {
      if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER, LEFTMOUSE)) {
        retval = WM_UI_HANDLER_BREAK;
        handle_layout_panel_header(C, &block, mx, my, event->type);
      }
    }
  }

  return retval;
}

static void panel_custom_data_set_recursive(Panel *panel, PointerRNA *custom_data)
{
  panel->runtime->custom_data_ptr = custom_data;

  for (Panel &child_panel : panel->children) {
    panel_custom_data_set_recursive(&child_panel, custom_data);
  }
}

void panel_context_pointer_set(Panel *panel, const char *name, PointerRNA *ptr)
{
  panel->layout->context_ptr_set(name, ptr);
  panel->runtime->context = panel->layout->context_store();
}

void panel_custom_data_set(Panel *panel, PointerRNA *custom_data)
{
  BLI_assert(panel->type != nullptr);

  /* Free the old custom data, which should be shared among all of the panel's sub-panels. */
  if (panel->runtime->custom_data_ptr != nullptr) {
    MEM_delete(panel->runtime->custom_data_ptr);
  }

  panel_custom_data_set_recursive(panel, custom_data);
}

PointerRNA *panel_custom_data_get(const Panel *panel)
{
  return panel->runtime->custom_data_ptr;
}

PointerRNA *region_panel_custom_data_under_cursor(const bContext *C, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  if (region) {
    for (Block &block : region->runtime->uiblocks) {
      Panel *panel = block.panel;
      if (panel == nullptr) {
        continue;
      }

      int mx = event->xy[0];
      int my = event->xy[1];
      window_to_block(region, &block, &mx, &my);
      const int mouse_state = panel_mouse_state_get(&block, panel, mx, my);
      if (ELEM(mouse_state, PANEL_MOUSE_INSIDE_CONTENT, PANEL_MOUSE_INSIDE_HEADER)) {
        return panel_custom_data_get(panel);
      }
    }
  }

  return nullptr;
}

bool panel_can_be_pinned(const Panel *panel)
{
  return (panel->type->parent == nullptr) && !(panel->type->flag & PANEL_TYPE_INSTANCED);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Level Modal Panel Interaction
 * \{ */

/* NOTE: this is modal handler and should not swallow events for animation. */
static int handler_panel(bContext *C, const wmEvent *event, void *userdata)
{
  Panel *panel = static_cast<Panel *>(userdata);
  HandlePanelData *data = static_cast<HandlePanelData *>(panel->activedata);

  /* Verify if we can stop. */
  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    panel_activate_state(C, panel, PANEL_STATE_ANIMATION);
  }
  else if (event->type == MOUSEMOVE) {
    if (data->state == PANEL_STATE_DRAG) {
      do_drag(C, event, panel);
    }
  }
  else if (event->type == TIMER && event->customdata == data->animtimer) {
    if (data->state == PANEL_STATE_ANIMATION) {
      do_animate(C, panel);
    }
    else if (data->state == PANEL_STATE_DRAG) {
      do_drag(C, event, panel);
    }
  }

  data = static_cast<HandlePanelData *>(panel->activedata);

  if (data && data->state == PANEL_STATE_ANIMATION) {
    return WM_UI_HANDLER_CONTINUE;
  }
  return WM_UI_HANDLER_BREAK;
}

static void handler_remove_panel(bContext *C, void *userdata)
{
  Panel *panel = static_cast<Panel *>(userdata);

  panel_activate_state(C, panel, PANEL_STATE_EXIT);
}

static void panel_handle_data_ensure(const bContext *C,
                                     wmWindow *win,
                                     const ARegion *region,
                                     Panel *panel,
                                     const HandlePanelState state)
{
  BLI_assert(ELEM(state, PANEL_STATE_DRAG, PANEL_STATE_ANIMATION));

  if (panel->activedata == nullptr) {
    panel->activedata = MEM_new_zeroed<HandlePanelData>(__func__);
    WM_event_add_ui_handler(C,
                            &win->runtime->modalhandlers,
                            handler_panel,
                            handler_remove_panel,
                            panel,
                            eWM_EventHandlerFlag(0));
  }

  HandlePanelData *data = static_cast<HandlePanelData *>(panel->activedata);

  /* Only create a new timer if necessary. Reuse can occur when PANEL_STATE_ANIMATION follows
   * PANEL_STATE_DRAG for example (i.e. panel->activedata was present already). */
  if (!data->animtimer) {
    data->animtimer = WM_event_timer_add(CTX_wm_manager(C), win, TIMER, ANIMATION_INTERVAL);
  }

  data->state = state;
  data->startx = win->runtime->eventstate->xy[0];
  data->starty = win->runtime->eventstate->xy[1];
  data->startofsx = panel->ofsx;
  data->startofsy = panel->ofsy;
  data->start_cur_xmin = region->v2d.cur.xmin;
  data->start_cur_ymin = region->v2d.cur.ymin;
  data->starttime = BLI_time_now_seconds();
}

/**
 * \note "select" and "drag drop" flags: First, the panel is "picked up" and both flags are set.
 * Then when the mouse releases and the panel starts animating to its aligned position, PNL_SELECT
 * is unset. When the animation finishes, PANEL_IS_DRAG_DROP is cleared.
 */
static void panel_activate_state(const bContext *C, Panel *panel, const HandlePanelState state)
{
  HandlePanelData *data = static_cast<HandlePanelData *>(panel->activedata);
  wmWindow *win = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);

  if (data != nullptr && data->state == state) {
    return;
  }

  if (state == PANEL_STATE_DRAG) {
    panel_custom_data_active_set(panel);

    panel_set_flag_recursive(panel, PNL_SELECT, true);
    panel_set_runtime_flag_recursive(panel, PANEL_IS_DRAG_DROP, true);

    panel_handle_data_ensure(C, win, region, panel, state);

    /* Initiate edge panning during drags for scrolling beyond the initial region view. */
    wmOperatorType *ot = WM_operatortype_find("VIEW2D_OT_edge_pan", true);
    handle_afterfunc_add_operator(ot, wm::OpCallContext::InvokeDefault);
  }
  else if (state == PANEL_STATE_ANIMATION) {
    panel_set_flag_recursive(panel, PNL_SELECT, false);

    panel_handle_data_ensure(C, win, region, panel, state);
  }
  else if (state == PANEL_STATE_EXIT) {
    panel_set_runtime_flag_recursive(panel, PANEL_IS_DRAG_DROP, false);

    BLI_assert(data != nullptr);

    if (data->animtimer) {
      WM_event_timer_remove(CTX_wm_manager(C), win, data->animtimer);
      data->animtimer = nullptr;
    }

    MEM_delete(data);
    panel->activedata = nullptr;

    WM_event_remove_ui_handler(
        &win->runtime->modalhandlers, handler_panel, handler_remove_panel, panel, false);
  }

  ED_region_tag_redraw(region);
}

void panel_stop_animation(const bContext *C, Panel *panel)
{
  if (panel->activedata) {
    panel_activate_state(C, panel, PANEL_STATE_EXIT);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Drag Operator
 * \{ */

static bool category_tab_drag_poll(bContext *C)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return false;
  }
  if (!panel_category_tabs_is_visible(region)) {
    return false;
  }
  if (U.category_tabs_allow_edit) {
    return false;
  }
  return true;
}

static wmOperatorStatus category_tab_drag_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Find clicked tab */
  PanelCategoryDyn *clicked_pc = nullptr;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (BLI_rcti_isect_pt(&pc_dyn.rect, event->mval[0], event->mval[1])) {
      clicked_pc = &pc_dyn;
      break;
    }
  }

  if (clicked_pc == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Check if reserved (cannot be reordered) */
  bool is_reserved_glyph = category_is_reserved(CTX_wm_manager(C), clicked_pc->idname);

  /* Initialize drag state (allow all categories for now) */
  CategoryDragState *state = MEM_new<CategoryDragState>(__func__);
  state->is_dragging = true;
  state->is_reserved = is_reserved_glyph;

  if (is_reserved_glyph) {
    /* Create persistent tooltip */
    char msg[128];
    SNPRINTF(msg, "%s (Cannot Reorder)", IFACE_(clicked_pc->idname));
    state->tooltip_region = tooltip_create_from_text(C, msg, event->xy);
  }
  state->current_mouse_x = event->mval[0];
  state->current_mouse_y = event->mval[1];
  STRNCPY(state->drag_category_id, clicked_pc->idname);
  state->drag_start_y = event->mval[1];
  state->drag_tab_height = BLI_rcti_size_y(&clicked_pc->rect);

  /* Calculate offsets from click point to tab edges.
   * When moving up, we use top edge; when moving down, we use bottom edge.
   * This provides intuitive insert position feedback regardless of click position. */
  state->drag_top_edge_offset = clicked_pc->rect.ymax - event->mval[1];
  state->drag_bottom_edge_offset = clicked_pc->rect.ymin - event->mval[1];

  /* Calculate original index based on visual order, not workspace order value */
  int visual_index = 0;
  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);
  for (PanelCategoryDyn *pc_dyn : ordered_categories) {
    if (STREQ(pc_dyn->idname, clicked_pc->idname)) {
      break;
    }
    visual_index++;
  }
  state->original_index = visual_index;
  state->current_insert_index = state->original_index;
  state->drag_offset_y = 0.0f;
  state->initial_scroll = region->category_scroll;
  state->tab_v_pad = 0;  /* Will be calculated during draw */

  op->customdata = state;

  /* Store initial state in region runtime */
  region->runtime->category_tabs_drag_state = state;
  region->runtime->category_tabs_drag_pending_id[0] = '\0';  /* Clear pending */

  /* Start auto-scroll timer */
  state->scroll_timer = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.02f);

  WM_event_add_modal_handler(C, op);

  /* Set grab cursor during drag */
  WM_cursor_modal_set(CTX_wm_window(C), state->is_reserved ? WM_CURSOR_HAND : WM_CURSOR_HAND_CLOSED);

  ED_region_tag_redraw(region);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus category_tab_drag_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  CategoryDragState *state = static_cast<CategoryDragState *>(op->customdata);

  if (region == nullptr || state == nullptr) {
    if (state && state->scroll_timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
      state->scroll_timer = nullptr;
    }
    WM_cursor_modal_restore(CTX_wm_window(C));
    return OPERATOR_CANCELLED;
  }

  const bool is_timer = (event->type == TIMER && event->customdata == state->scroll_timer);

  /* Special handling for reserved tabs (tooltip only) */
  if (state->is_reserved) {
    if (event->type == MOUSEMOVE) {
      if (state->tooltip_region) {
        int dx = event->mval[0] - state->current_mouse_x;
        int dy = event->mval[1] - state->current_mouse_y;
        BLI_rcti_translate(&state->tooltip_region->winrct, dx, dy);
      }
      state->current_mouse_x = event->mval[0];
      state->current_mouse_y = event->mval[1];
      ED_region_tag_redraw(region);
    }
    
    /* Finish on mouse release or leaving region */
    bool finish = (event->type == LEFTMOUSE && event->val == KM_RELEASE);
    if (!finish && (event->mval[0] < 0 || event->mval[0] > region->winx ||
                    event->mval[1] < 0 || event->mval[1] > region->winy))
    {
      finish = true;
    }

    if (finish) {
      if (state->tooltip_region) {
        tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
        state->tooltip_region = nullptr;
      }
      if (state->scroll_timer) {
        WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
        state->scroll_timer = nullptr;
      }
      region->runtime->category_tabs_drag_state = nullptr;
      MEM_delete(state);
      op->customdata = nullptr;
      WM_cursor_modal_restore(CTX_wm_window(C));
      ED_region_tag_redraw(region);
      return OPERATOR_FINISHED;
    }
    
    return OPERATOR_RUNNING_MODAL;
  }

  if (is_timer || event->type == MOUSEMOVE || event->type == WHEELUPMOUSE || event->type == WHEELDOWNMOUSE) {
    if (event->type == MOUSEMOVE) {
      /* Save previous offset for direction detection, then update */
      state->prev_drag_offset_y = state->drag_offset_y;
      state->drag_offset_y = float(event->mval[1] - state->drag_start_y);
    }

    bool scrolled = false;
    float scroll_amount = 0.0f;

    if (event->type == WHEELUPMOUSE) {
      scroll_amount = -20.0f * U.pixelsize;
    }
    else if (event->type == WHEELDOWNMOUSE) {
      scroll_amount = 20.0f * U.pixelsize;
    }
    else {
      /* Auto-scroll */
      const float edge_margin = 30.0f * U.pixelsize;
      const float auto_scroll_speed = 10.0f * U.pixelsize;
      /* Use calculated mouse Y because TIMER event might not have valid mval */
      int current_mouse_y = state->drag_start_y + (int)state->drag_offset_y;

      if (current_mouse_y > region->winrct.ymax - region->winrct.ymin - edge_margin) {
        scroll_amount = -auto_scroll_speed;
      }
      else if (current_mouse_y < edge_margin) {
        scroll_amount = auto_scroll_speed;
      }
    }

    if (scroll_amount != 0.0f) {
      const int old_scroll = region->category_scroll;

      /* Apply scroll */
      region->category_scroll += (int)scroll_amount;

      /* Note: Clamping happens in panel_category_tabs_draw_all during redraw.
       * We rely on that to keep category_scroll within valid bounds. */

      if (old_scroll != region->category_scroll) {
        scrolled = true;
        ED_region_tag_redraw(region);
      }
    }

    if (scrolled || event->type == MOUSEMOVE) {
      /* Calculate new insert index */
      const wmWindowManager *wm = CTX_wm_manager(C);
      state->current_insert_index = calculate_insert_index(C, wm, region, state);
      update_insert_zone(C, wm, region, state);

      /* Check if cursor is over a reserved tab and update cursor accordingly */
      bool over_reserved = false;
      for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
        if (BLI_rcti_isect_pt(&pc_dyn.rect, event->mval[0], event->mval[1])) {
          if (category_is_reserved_for_reorder(wm, pc_dyn.idname)) {
            over_reserved = true;
          }
          break;
        }
      }

      /* Set cursor: STOP if over reserved tab, otherwise closed hand for dragging */
      WM_cursor_modal_set(CTX_wm_window(C),
                          over_reserved ? WM_CURSOR_STOP : WM_CURSOR_HAND_CLOSED);

      ED_region_tag_redraw(region);
    }

    if (is_timer) {
      return OPERATOR_RUNNING_MODAL;
    }
    /* Consume mouse move and wheel events */
    return OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        /* Apply the new order (only for non-reserved tabs) */
        if (!state->is_reserved) {
          apply_category_order(C, region, state);
        }

        /* Cleanup */
        if (state->tooltip_region) {
          tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
          state->tooltip_region = nullptr;
        }
        if (state->scroll_timer) {
          WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
          state->scroll_timer = nullptr;
        }
        region->runtime->category_tabs_drag_state = nullptr;
        MEM_delete(state);
        op->customdata = nullptr;

        WM_cursor_modal_restore(CTX_wm_window(C));

        ED_region_tag_redraw(region);
        return OPERATOR_FINISHED;
      }
      break;

    case EVT_ESCKEY:
    case RIGHTMOUSE:
      /* Cancel drag */
      if (state->tooltip_region) {
        tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
        state->tooltip_region = nullptr;
      }
      if (state->scroll_timer) {
        WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
        state->scroll_timer = nullptr;
      }
      region->runtime->category_tabs_drag_state = nullptr;
      MEM_delete(state);
      op->customdata = nullptr;

      WM_cursor_modal_restore(CTX_wm_window(C));

      ED_region_tag_redraw(region);
      return OPERATOR_CANCELLED;
      
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void category_tab_drag_cancel(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  if (region && region->runtime->category_tabs_drag_state) {
    CategoryDragState *state = static_cast<CategoryDragState *>(op->customdata);

    if (state && state->tooltip_region) {
      tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
      state->tooltip_region = nullptr;
    }

    if (state && state->scroll_timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
      state->scroll_timer = nullptr;
    }

    MEM_delete(state);
    region->runtime->category_tabs_drag_state = nullptr;
    op->customdata = nullptr;

    WM_cursor_modal_restore(CTX_wm_window(C));

    ED_region_tag_redraw(region);
  }
}

void UI_OT_category_tab_drag(wmOperatorType *ot)
{
  ot->name = "Category Tab Drag";
  ot->idname = "UI_OT_category_tab_drag";
  ot->description = "Drag to reorder category tabs";

  ot->invoke = category_tab_drag_invoke;
  ot->modal = category_tab_drag_modal;
  ot->cancel = category_tab_drag_cancel;
  ot->poll = category_tab_drag_poll;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

}  // namespace blender::ui
