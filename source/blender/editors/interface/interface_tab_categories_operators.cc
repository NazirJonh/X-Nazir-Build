/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tabs - Quick-focus operator.
 *
 * Split out of interface_tab_categories.cc: the self-contained
 * `UI_OT_category_quick_focus` operator (enum-search popup that switches the
 * active category tab, with a small recent-history list). It depends on the rest
 * of the module only through the public `panel_category_active_set_safe()`
 * declared in interface_intern.hh.
 */

#include <algorithm>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_screen_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "interface_intern.hh"
#include "interface_tab_categories_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Operator: Category Quick Focus
 * \{ */

static constexpr bool CATEGORY_QUICK_FOCUS_DEBUG = false;

/* Recent categories history — stores up to 8 recently used category idnames. */
static constexpr int QUICK_FOCUS_RECENT_MAX = 8;
static char g_quick_focus_recent[QUICK_FOCUS_RECENT_MAX][64] = {};
static int g_quick_focus_recent_count = 0;

static void quick_focus_recent_add(const char *idname)
{
  if (!idname || idname[0] == '\0') {
    return;
  }
  /* Remove if already present (move to front). */
  for (int i = 0; i < g_quick_focus_recent_count; i++) {
    if (STREQ(g_quick_focus_recent[i], idname)) {
      for (int j = i; j > 0; j--) {
        STRNCPY(g_quick_focus_recent[j], g_quick_focus_recent[j - 1]);
      }
      STRNCPY(g_quick_focus_recent[0], idname);
      return;
    }
  }
  /* Shift down and insert at front. */
  const int new_count = std::min(g_quick_focus_recent_count + 1, QUICK_FOCUS_RECENT_MAX);
  for (int i = new_count - 1; i > 0; i--) {
    STRNCPY(g_quick_focus_recent[i], g_quick_focus_recent[i - 1]);
  }
  STRNCPY(g_quick_focus_recent[0], idname);
  g_quick_focus_recent_count = new_count;
}

static ARegion *category_quick_focus_region_get(const bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr) {
    return nullptr;
  }
  return BKE_area_find_region_type(area, RGN_TYPE_UI);
}

/** Ensure the UI sidebar is visible. Returns true if it was opened (was hidden before). */
static bool category_quick_focus_ensure_sidebar_visible(bContext *C,
                                                         ScrArea *area,
                                                         ARegion *region_ui)
{
  if (!(region_ui->flag & RGN_FLAG_HIDDEN)) {
    return false;
  }
  ED_region_visibility_change_update(C, area, region_ui);
  return true;
}

/**
 * Dynamic EnumProperty items callback — builds the category list for the search popup.
 * Recent categories appear first, then all others alphabetically.
 */
static const EnumPropertyItem *category_quick_focus_enum_items(bContext *C,
                                                                PointerRNA * /*ptr*/,
                                                                PropertyRNA * /*prop*/,
                                                                bool *r_free)
{
  *r_free = false;

  if (C == nullptr) {
    return rna_enum_dummy_NULL_items;
  }

  ARegion *region_ui = category_quick_focus_region_get(C);
  if (region_ui == nullptr || region_ui->runtime == nullptr) {
    return rna_enum_dummy_NULL_items;
  }

  /* Collect all category idnames. */
  Vector<std::string> all_cats;
  for (PanelCategoryDyn *pc_dyn = static_cast<PanelCategoryDyn *>(
           region_ui->runtime->panels_category.first);
       pc_dyn != nullptr;
       pc_dyn = static_cast<PanelCategoryDyn *>(pc_dyn->next))
  {
    if (pc_dyn->idname[0] != '\0') {
      all_cats.append(std::string(pc_dyn->idname));
    }
  }

  if (all_cats.is_empty()) {
    return rna_enum_dummy_NULL_items;
  }

  /* Build ordered list: recent first, then remaining sorted. */
  Vector<std::string> ordered;
  /* Add recent entries that still exist. */
  for (int i = 0; i < g_quick_focus_recent_count; i++) {
    for (const std::string &cat : all_cats) {
      if (cat == g_quick_focus_recent[i]) {
        ordered.append(cat);
        break;
      }
    }
  }
  /* Add remaining categories (not in recent), sorted. */
  Vector<std::string> rest;
  for (const std::string &cat : all_cats) {
    bool in_recent = false;
    for (const std::string &r : ordered) {
      if (r == cat) {
        in_recent = true;
        break;
      }
    }
    if (!in_recent) {
      rest.append(cat);
    }
  }
  std::sort(rest.begin(), rest.end(), [](const std::string &a, const std::string &b) {
    return BLI_strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  for (const std::string &cat : rest) {
    ordered.append(cat);
  }

  /* Stable storage for identifier/name strings. RNA frees the item array on the free path but
   * does not free the per-item strings, so they must outlive the returned array. Rebuild this
   * storage on every call; the previous contents are no longer referenced by RNA at that point. */
  static Vector<std::string> g_quick_focus_enum_strings;
  g_quick_focus_enum_strings = ordered;

  const int count = g_quick_focus_enum_strings.size();
  EnumPropertyItem *items = nullptr;
  int totitem = 0;
  for (int i = 0; i < count; i++) {
    EnumPropertyItem item_tmp = {0, nullptr, 0, nullptr, nullptr};
    item_tmp.value = i;
    item_tmp.identifier = g_quick_focus_enum_strings[i].c_str();
    item_tmp.icon = ICON_NONE;
    item_tmp.name = g_quick_focus_enum_strings[i].c_str();
    item_tmp.description = "";
    RNA_enum_item_add(&items, &totitem, &item_tmp);
  }
  RNA_enum_item_end(&items, &totitem);

  *r_free = true;
  return items;
}

static bool category_quick_focus_poll(bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region_ui = category_quick_focus_region_get(C);
  if (area == nullptr || region_ui == nullptr) {
    return false;
  }
  if (region_ui->runtime == nullptr || region_ui->runtime->type == nullptr) {
    return false;
  }
  if (!BKE_regiontype_uses_category_tabs(region_ui->runtime->type)) {
    return false;
  }
  return true;
}

static wmOperatorStatus category_quick_focus_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region_ui = category_quick_focus_region_get(C);
  if (region_ui == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Ensure sidebar is visible. */
  if (area) {
    category_quick_focus_ensure_sidebar_visible(C, area, region_ui);
  }

  /* Get selected category identifier from the EnumProperty. */
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "category");
  if (prop == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int enum_val = RNA_property_enum_get(op->ptr, prop);
  bool r_free = false;
  const EnumPropertyItem *items = category_quick_focus_enum_items(C, op->ptr, prop, &r_free);
  if (items == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const char *idname = nullptr;
  for (int i = 0; items[i].identifier != nullptr; i++) {
    if (items[i].value == enum_val) {
      idname = items[i].identifier;
      break;
    }
  }

  wmOperatorStatus result = OPERATOR_CANCELLED;
  if (idname && idname[0] != '\0') {
    panel_category_active_set_safe(C, region_ui, idname);
    quick_focus_recent_add(idname);
    result = OPERATOR_FINISHED;
    if (CATEGORY_QUICK_FOCUS_DEBUG) {
      printf("[QuickFocus] exec: activated '%s'\n", idname);
      fflush(stdout);
    }
  }

  if (r_free) {
    /* Only the item array is owned here; identifier/name strings live in stable storage. */
    MEM_delete(const_cast<EnumPropertyItem *>(items));
  }

  return result;
}

static wmOperatorStatus category_quick_focus_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region_ui = category_quick_focus_region_get(C);
  if (area == nullptr || region_ui == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Open sidebar if hidden so categories are populated. */
  category_quick_focus_ensure_sidebar_visible(C, area, region_ui);

  if (CATEGORY_QUICK_FOCUS_DEBUG) {
    printf("[QuickFocus] invoke: area=%p region_ui=%p\n", (void *)area, (void *)region_ui);
    fflush(stdout);
  }

  return WM_enum_search_invoke(C, op, event);
}

void UI_OT_category_quick_focus(wmOperatorType *ot)
{
  ot->name = "Category Quick Focus";
  ot->idname = "UI_OT_category_quick_focus";
  ot->description = "Search and switch to a category tab";

  ot->poll = category_quick_focus_poll;
  ot->invoke = category_quick_focus_invoke;
  ot->exec = category_quick_focus_exec;

  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "category", rna_enum_dummy_NULL_items, 0, "Category", "Category to focus");
  RNA_def_enum_funcs(prop, category_quick_focus_enum_items);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

/** \} */

}  // namespace blender::ui
