/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tabs - Drawing and event handling for category tab panels.
 *
 * This module provides functionality for:
 * - Drawing category tabs with glyphs, text, and zoom support
 * - Handling mouse events (hover, click, drag)
 * - Drag & drop reordering of tabs
 * - Tooltips for category tabs
 * - Settings button for tab preferences
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

/* Global debug flag for tab drag operations - set to 0 to disable debug output */
#ifndef TAB_DRAG_DEBUG_ENABLED
#  define TAB_DRAG_DEBUG_ENABLED 0
#endif

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "BLF_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "interface_intern.hh"
#include "interface_tag_bar.hh"
#include "regions/interface_regions_intern.hh"

#include "interface_category_py_bridge.hh"
#include "interface_tab_categories_intern.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

struct Main;

namespace blender {

/* Extern declaration for space type enum items (defined in rna_space.cc) */
extern const EnumPropertyItem rna_enum_space_type_items[];

}  // namespace blender

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Debug Output Control
 * \{ */

/**
 * Debug output control flag - set to true to enable debug printf messages.
 *
 * The `CATEGORY_TAB_DEBUG_ENABLED` flag now lives in the shared header
 * `interface_tab_categories_intern.hh` so this file and
 * `interface_tab_categories_draw.cc` share a single definition. To enable debug
 * output, flip it to `true` there.
 */

/** \} */

/* Forward declarations */
static std::string normalize_category_key(const char *category);

static bool category_item_match_exact(const CategoryGlyphItem *item,
                                      const char *category,
                                      int space_type);
static bool category_item_match_normalized(const CategoryGlyphItem *item,
                                           const std::string &normalized_target,
                                           int space_type);
static const CategoryGlyphItem *category_item_find_exact_any_space(const ListBase *list,
                                                                   const char *category);
static const CategoryGlyphItem *category_item_find_overrides(const wmWindowManager *wm,
                                                             const char *category,
                                                             int space_type);
static const CategoryGlyphItem *category_item_find_mappings(const wmWindowManager *wm,
                                                            const char *category,
                                                            int space_type);
static bool category_order_is_crossing_reserved_boundary(const wmWindowManager *wm,
                                                         const Vector<std::string> &order);
static void workspace_category_order_clear(WorkSpace *workspace, int space_type, int region_type);
bool category_tab_should_expand_name(const ARegion *region,
                                            const char *category_id,
                                            const eUserPref_CategoryTabsDisplayMode display_mode,
                                            const bool is_active,
                                            const bool use_minimized_gate,
                                            const bool is_panel_minimized);

/* -------------------------------------------------------------------- */
/** \name Internal Category Lookup Helpers
 * \{ */

/**
 * Internal helper: Find category glyph item with exact match (category + space_type).
 * Returns first matching item or nullptr if not found.
 */
static bool category_item_match_exact(const CategoryGlyphItem *item,
                                      const char *category,
                                      int space_type)
{
  return item && category && STREQ(item->category, category) && item->space_type == space_type;
}

static bool category_item_match_normalized(const CategoryGlyphItem *item,
                                           const std::string &normalized_target,
                                           int space_type)
{
  if (!item || normalized_target.empty()) {
    return false;
  }
  const std::string normalized_item = normalize_category_key(item->category);
  return normalized_item == normalized_target && item->space_type == space_type;
}

static const CategoryGlyphItem *category_item_find_exact_any_space(const ListBase *list,
                                                                   const char *category)
{
  if (!list || !category || !category_glyph_list_is_valid(list)) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category)) {
      return item;
    }
  }

  return nullptr;
}

/* Iterate a CategoryGlyphItem ListBase and return the first item for which `match` returns true,
 * or nullptr. Centralizes the list-validity guard and the node iteration shared by the lookup
 * helpers below; each helper supplies only its match predicate (and any space-type guard). */
template<typename MatchFn>
static const CategoryGlyphItem *category_glyph_list_find(const ListBase *list, MatchFn &&match)
{
  if (!list || !category_glyph_list_is_valid(list)) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (match(item)) {
      return item;
    }
  }
  return nullptr;
}

static const CategoryGlyphItem *category_glyph_item_find_exact(const ListBase *list,
                                                               const char *category,
                                                               int space_type)
{
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return category_item_match_exact(item, category, space_type);
  });
}

/**
 * Internal helper: Find category glyph item with global fallback (space_type = -1).
 * Only searches global if space_type != -1.
 */
static const CategoryGlyphItem *category_glyph_item_find_global(const ListBase *list,
                                                                const char *category,
                                                                int space_type)
{
  if (space_type == -1) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return STREQ(item->category, category) && item->space_type == -1;
  });
}

/**
 * Internal helper: Find category glyph item with normalized exact match.
 * Uses normalize_category_key() for canonicalization fallback.
 */
static const CategoryGlyphItem *category_glyph_item_find_normalized_exact(const ListBase *list,
                                                                          const char *category,
                                                                          int space_type)
{
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return category_item_match_normalized(item, normalized_target, space_type);
  });
}

/**
 * Internal helper: Find category glyph item with normalized global fallback.
 * Uses normalize_category_key() for canonicalization with global space_type = -1.
 */
static const CategoryGlyphItem *category_glyph_item_find_normalized_global(const ListBase *list,
                                                                           const char *category,
                                                                           int space_type)
{
  if (space_type == -1) {
    return nullptr;
  }
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return category_item_match_normalized(item, normalized_target, -1);
  });
}

static const CategoryGlyphItem *category_glyph_item_find_global_only(const ListBase *list,
                                                                     const char *category)
{
  if (!category) {
    return nullptr;
  }

  /* First pass: exact global (space_type == -1) match. */
  if (const CategoryGlyphItem *item = category_glyph_list_find(
          list, [&](const CategoryGlyphItem *it) {
            return it->space_type == -1 && STREQ(it->category, category);
          }))
  {
    return item;
  }

  /* Second pass: normalized global match. */
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *it) {
    return it->space_type == -1 && category_item_match_normalized(it, normalized_target, -1);
  });
}

/**
 * Internal helper: Unified category lookup with priority order.
 * Searches in order: exact -> global -> normalized exact -> normalized global.
 * Keep this order stable: edit/live-preview/reset paths rely on the same resolution contract.
 */
static const CategoryGlyphItem *category_glyph_item_find_with_fallback(const ListBase *list,
                                                                        const char *category,
                                                                        int space_type)
{
  if (!list || !category) {
    return nullptr;
  }

  /* First pass: try exact match (category + space_type). */
  if (const CategoryGlyphItem *item = category_glyph_item_find_exact(list, category, space_type)) {
    return item;
  }

  /* Second pass: try global categories (space_type = -1) if not searching for global already. */
  if (const CategoryGlyphItem *item = category_glyph_item_find_global(list, category, space_type)) {
    return item;
  }

  /* Third pass: try canonicalization fallback with space_type match. */
  if (const CategoryGlyphItem *item = category_glyph_item_find_normalized_exact(list, category, space_type)) {
    return item;
  }

  /* Fourth pass: try canonicalization fallback with global categories. */
  if (const CategoryGlyphItem *item = category_glyph_item_find_normalized_global(list, category, space_type)) {
    return item;
  }

  return nullptr;
}

static const CategoryGlyphItem *category_item_find_overrides(const wmWindowManager *wm,
                                                             const char *category,
                                                             int space_type)
{
  if (!wm) {
    return nullptr;
  }
  return category_glyph_item_find_with_fallback(&wm->category_glyph_overrides, category, space_type);
}

static const CategoryGlyphItem *category_item_find_mappings(const wmWindowManager *wm,
                                                             const char *category,
                                                             int space_type)
{
  if (!wm) {
    return nullptr;
  }
  UNUSED_VARS(space_type);
  return category_glyph_item_find_global_only(&wm->category_glyph_mappings, category);
}

static bool category_item_override_icon_is_effective(const CategoryGlyphItem *item)
{
  if (!item) {
    return false;
  }

  const bool has_icon_payload = (item->icon_key[0] != '\0') || (item->icon_path[0] != '\0') ||
                                (item->icon_provider[0] != '\0');
  const bool has_explicit_icon_mode = ELEM(
      item->icon_source, CATEGORY_TAB_ICON_SOURCE_MANUAL, CATEGORY_TAB_ICON_SOURCE_OFF);

  /* Override entries are often created for glyph/color/tag edits.
   * If an AUTO override has no icon payload, don't mask JSON mapping icon data. */
  return has_icon_payload || has_explicit_icon_mode;
}

/* Two-pass any-space lookup shared by the color/effective-icon helpers below. Pass one matches
 * the exact category name, pass two falls back to the normalized name; both require `pred(item)`.
 * Each caller supplies only the payload predicate that distinguishes it. */
template<typename PredFn>
static const CategoryGlyphItem *category_item_find_any_space_matching(const ListBase *list,
                                                                      const char *category,
                                                                      PredFn &&pred)
{
  if (!category) {
    return nullptr;
  }

  /* First pass: exact category-name match satisfying the predicate, in any space. */
  if (const CategoryGlyphItem *item = category_glyph_list_find(
          list, [&](const CategoryGlyphItem *it) {
            return STREQ(it->category, category) && pred(it);
          }))
  {
    return item;
  }

  /* Second pass: normalized-name fallback satisfying the predicate, in any space. */
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *it) {
    return pred(it) && category_item_match_normalized(it, normalized_target, it->space_type);
  });
}

static const CategoryGlyphItem *category_item_find_with_color_any_space(const ListBase *list,
                                                                        const char *category)
{
  return category_item_find_any_space_matching(
      list, category, [](const CategoryGlyphItem *item) { return !is_zero_v3(item->color); });
}

static const CategoryGlyphItem *category_item_find_with_effective_icon_any_space(
    const ListBase *list, const char *category)
{
  return category_item_find_any_space_matching(list, category, category_item_override_icon_is_effective);
}

static const CategoryGlyphItem *category_item_find_override_with_display_name(
    const ListBase *list, const char *category, int space_type)
{
  if (!category) {
    return nullptr;
  }

  /* First pass: exact space-type match that carries a display name. */
  if (const CategoryGlyphItem *item = category_glyph_list_find(
          list, [&](const CategoryGlyphItem *it) {
            return category_item_match_exact(it, category, space_type) && it->display_name[0] != '\0';
          }))
  {
    return item;
  }

  if (space_type == -1) {
    return nullptr;
  }

  /* Second pass: global (space_type == -1) fallback that carries a display name. */
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *it) {
    return category_item_match_exact(it, category, -1) && it->display_name[0] != '\0';
  });
}

static const char *category_item_display_name_or_default_or_category(const CategoryGlyphItem *item,
                                                                     const char *category)
{
  if (!item) {
    return category;
  }
  if (item->display_name[0] != '\0') {
    return item->display_name;
  }
  if (item->default_display_name[0] != '\0') {
    return item->default_display_name;
  }
  return category;
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Constants & Macros
 * \{ */

/* Shared tab drawing/layout constants (TABS_*, EXTENSION_DROP_GHOST_HEIGHT,
 * CATEGORY_TAB_VISUAL_ACTIVE_OUTLINE_ENABLE) live in interface_tab_categories_intern.hh
 * so interface_tab_categories_draw.cc can use them too. */

/* Use UI_UI_TABS_VISUAL_EFFECT_SCALE from UI_interface_c.hh */

/** \} */

int category_tabs_vertical_padding_calc(float zoom)
{
  return int(std::floor(TABS_PADDING_BETWEEN_FACTOR * UI_SCALE_FAC * zoom));
}

/* -------------------------------------------------------------------- */
/** \name Pending Category Insert (Extension Drop)
 * \{ */


PendingCategoryInsert &pending_category_insert()
{
  static PendingCategoryInsert value;
  return value;
}

/* Deferred category activation - used to activate a category outside of layout phase.
 * This prevents crashes when extensions load previews in background threads during
 * panel_poll calls triggered by immediate activation. */

DeferredCategoryActivation g_deferred_category_activation;

/* Known categories before extension drop - used to detect new categories */
Set<std::string> &known_categories_before_extension_drop()
{
  static Set<std::string> value;
  return value;
}

struct DeferredActivationExtensionCallbackState {
  bCallbackFuncStore callback_store = {};
  bool registered = false;
};

static DeferredActivationExtensionCallbackState g_deferred_activation_extension_callback_state;

static void deferred_category_activation_extension_callback(Main * /*bmain*/,
                                                             PointerRNA ** /*pointers*/,
                                                             int /*pointers_num*/,
                                                             void * /*arg*/)
{
  /* Set extension signal received flag - this must happen regardless of debug mode. */
  if (g_deferred_category_activation.valid &&
      g_deferred_category_activation.wait_for_extension_signal) {
    g_deferred_category_activation.extension_signal_received = true;
  }

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] Extension callback triggered! valid=%d wait_for_signal=%d\n",
           g_deferred_category_activation.valid ? 1 : 0,
           g_deferred_category_activation.wait_for_extension_signal ? 1 : 0);
    printf("[CATEGORY ACTIVATE] Extension callback - source_extension_id='%s', discover_new_category=%d\n",
           g_deferred_category_activation.source_extension_id.empty() ? "" : g_deferred_category_activation.source_extension_id.c_str(),
           g_deferred_category_activation.discover_new_category ? 1 : 0);
    fflush(stdout);
    if (g_deferred_category_activation.valid &&
        g_deferred_category_activation.wait_for_extension_signal) {
      printf("[CATEGORY ACTIVATE] Extension signal received, will activate on next draw\n");
      fflush(stdout);
    }
  }
}

static void deferred_category_activation_register_extension_callback()
{
  if (g_deferred_activation_extension_callback_state.registered) {
    return;
  }
  g_deferred_activation_extension_callback_state.callback_store.alloc = false;
  g_deferred_activation_extension_callback_state.callback_store.arg = nullptr;
  g_deferred_activation_extension_callback_state.callback_store.func =
      deferred_category_activation_extension_callback;
  BKE_callback_add(&g_deferred_activation_extension_callback_state.callback_store,
                   BKE_CB_EVT_EXTENSION_REPOS_UPDATE_POST);
  g_deferred_activation_extension_callback_state.registered = true;
}

void category_tabs_tag_refresh_active_area_ui(const bContext *C)
{
  if (!C) {
    return;
  }

  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return;
  }

  /* Tag a full UI refresh for the whole area, not only the current region.
   * Add-on panels may register successfully but won't be reflected in category tabs
   * until regions are refreshed and panels are re-built. */
  ED_area_tag_refresh(area);
  for (ARegion &region_iter : area->regionbase) {
    ED_region_tag_refresh_ui(&region_iter);
    ED_region_tag_redraw(&region_iter);
  }
}

static void category_tabs_report_new_categories(const bContext *C,
                                                const Vector<std::string> &category_ids)
{
  if (!C || category_ids.is_empty()) {
    return;
  }

  ReportList *reports = CTX_wm_reports(C);
  if (!reports) {
    return;
  }

  std::string message = "New categories: ";
  for (int i = 0; i < category_ids.size(); i++) {
    if (i != 0) {
      message += ", ";
    }
    message += category_ids[i];
  }

  BKE_report(reports, RPT_INFO, message.c_str());
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_INFO_REPORT, nullptr);
}

static void pending_category_insert_set(const std::string &tag_key,
                                        const char *target_category,
                                        const bool insert_above,
                                        const Vector<PanelCategoryDyn *> &ordered_categories,
                                        const Vector<std::string> &json_order)
{
  if (!target_category || target_category[0] == '\0') {
    return;
  }

  pending_category_insert().tag_key = tag_key;
  pending_category_insert().target_category = target_category;
  pending_category_insert().anchor_before.clear();
  pending_category_insert().anchor_after.clear();
  pending_category_insert().insert_above = insert_above;
  pending_category_insert().valid = true;
  pending_category_insert().timestamp = BLI_time_now_seconds();
  pending_category_insert().existing_categories.clear();
  pending_category_insert().pre_order = json_order;

  int target_index = -1;
  for (int i = 0; i < ordered_categories.size(); i++) {
    const PanelCategoryDyn *pc_dyn = ordered_categories[i];
    if (pc_dyn && pc_dyn->idname[0] != '\0' && STREQ(pc_dyn->idname, target_category)) {
      target_index = i;
      break;
    }
  }

  if (target_index != -1) {
    if (insert_above) {
      pending_category_insert().anchor_after = target_category;
      if (target_index > 0) {
        const PanelCategoryDyn *prev = ordered_categories[target_index - 1];
        if (prev && prev->idname[0] != '\0') {
          pending_category_insert().anchor_before = prev->idname;
        }
      }
    }
    else {
      pending_category_insert().anchor_before = target_category;
      if (target_index + 1 < ordered_categories.size()) {
        const PanelCategoryDyn *next = ordered_categories[target_index + 1];
        if (next && next->idname[0] != '\0') {
          pending_category_insert().anchor_after = next->idname;
        }
      }
    }
  }

  for (const PanelCategoryDyn *pc_dyn : ordered_categories) {
    if (pc_dyn && pc_dyn->idname[0] != '\0') {
      pending_category_insert().existing_categories.add(std::string(pc_dyn->idname));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extension Category Registration
 * \{ */

/**
 * Register a new category introduced by an extension as pending tag assignment.
 *
 * Calls the Python function `mark_category_from_extension` via the BPY string API,
 * then tags the tag bar for refresh so the "New Add-on!" virtual tag can appear.
 *
 * \param C            Blender context.
 * \param category_id  Category name (e.g. "Brushstroke Tools").
 * \param extension_id Extension package ID (e.g. "blender_org/brushstroke_tools").
 * \param space_type   Space type where the category was discovered (eSpace_Type).
 * \param mode_flag    Bitmask of mode flags where the category was discovered.
 * \param tag_already_assigned  True when a tag was already assigned (drag & drop onto tabs).
 */
void register_new_extension_category(const bContext *C,
                                             const char *category_id,
                                             const char *extension_id,
                                             int space_type,
                                             uint32_t mode_flag,
                                             bool tag_already_assigned)
{
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY REGISTER] register_new_extension_category called: category='%s', extension='%s', space_type=%d, mode_flag=0x%08X, tag_already_assigned=%d\n",
           category_id ? category_id : "(null)",
           extension_id ? extension_id : "(null)", 
           space_type, mode_flag, tag_already_assigned ? 1 : 0);
    fflush(stdout);
  }
  
  if (!C || !category_id || category_id[0] == '\0') {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY REGISTER] Early return: C=%p, category_id=%s\n", C, category_id ? category_id : "(null)");
      fflush(stdout);
    }
    return;
  }

#ifdef WITH_PYTHON
  /* Store extension ID for deferred activation (used for reserved-only extension detection) */
  g_deferred_category_activation.source_extension_id = extension_id;

  if (!tag_already_assigned) {
    /* Mark the category as pending tag assignment via Python (see interface_category_py_bridge). */
    category_py_mark_from_extension(const_cast<bContext *>(C),
                                    category_id,
                                    extension_id ? extension_id : "",
                                    space_type,
                                    mode_flag);
  }

  /* Tag the tag bar for refresh so the "New Add-on!" button can appear/disappear. */
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY REGISTER] Sending WM notification NC_WM | ND_CATEGORY_GLYPHS\n");
    fflush(stdout);
  }
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY REGISTER] Tagging area for redraw, spacetype=%d\n", area->spacetype);
      fflush(stdout);
    }
    ED_area_tag_redraw(area);
  }
  else {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY REGISTER] No area found for redraw\n");
      fflush(stdout);
    }
  }
#else
  UNUSED_VARS(category_id, extension_id, space_type, mode_flag, tag_already_assigned);
#endif
}

/**
 * Handle an extension being dropped onto the category tabs area.
 *
 * - Drop onto tabs (tab_category != nullptr): immediately assign the tag via Python
 *   (`assign_tag_to_category`) so `pending_tag_assignment` stays false.
 * - Drop into 3D Viewport (tab_category == nullptr): call `register_new_extension_category()`
 *   with `pending=true` so the "New Add-on!" virtual tag can surface the category.
 *
 * Does NOT add to `g_new_extension_categories_visible` — that set is being phased out.
 *
 * \param C              Blender context.
 * \param category_id    Category name introduced by the extension.
 * \param extension_id   Extension package ID.
 * \param tab_category   The tab the extension was dropped onto, or nullptr for viewport drop.
 * \param tag_name       Tag to assign when dropping onto a tab (may be nullptr).
 */
static void handle_extension_drop_on_tabs(const bContext *C,
                                           const char *category_id,
                                           const char *extension_id,
                                           const char *tab_category,
                                           const char *tag_name)
{
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[EXT DROP HANDLER] handle_extension_drop_on_tabs called: category='%s', extension='%s', tab_category='%s', tag_name='%s'\n",
           category_id ? category_id : "(null)",
           extension_id ? extension_id : "(null)", 
           tab_category ? tab_category : "(null)",
           tag_name ? tag_name : "(null)");
    fflush(stdout);
  }
  
  if (!C || !category_id || category_id[0] == '\0') {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[EXT DROP HANDLER] Early return: C=%p, category_id=%s\n", C, category_id ? category_id : "(null)");
      fflush(stdout);
    }
    return;
  }

#ifdef WITH_PYTHON
  const ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : -1;
  const uint32_t mode_flag = get_current_tag_mode_flag(C);

  std::string resolved_tag_name;
  if (tag_name != nullptr && tag_name[0] != '\0') {
    resolved_tag_name = tag_name;
  }
  else if (tab_category != nullptr) {
    TagFilterStateRef tag_state{};
    ScrArea *area_for_tag = CTX_wm_area(C);
    /* IMPORTANT: Only use active_tags if filter is actually ENABLED.
     * When filter is disabled (e.g., via "Tag Filter Tag" toggle), the category
     * should go to the general list without a tag. */
    if (tag_filter_state_from_area(area_for_tag, &tag_state) && tag_state.active_tags &&
        tag_state.active_tags[0] != '\0' &&
        tag_state.filter_enabled && *tag_state.filter_enabled)
    {
      resolved_tag_name = tag_state.active_tags;
    }
  }

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] Extension drop on tabs: resolved tag_name_to_assign='%s' (raw tag_name='%s')\n",
           resolved_tag_name.empty() ? "" : resolved_tag_name.c_str(),
           (tag_name != nullptr) ? tag_name : "(null)");
    fflush(stdout);
  }

  /* Store extension ID for deferred activation (used for reserved-only extension detection) */
  g_deferred_category_activation.source_extension_id = extension_id;

  if (tab_category != nullptr) {
    /* Drop onto tabs (with or without tag): set tag_already_assigned=true.
     * When dropping onto tabs, the category will be visible in the general list
     * without tag filtering, so we don't need to show "New Add-ons!" button.
     * If a tag is active, it will be assigned to the category when it appears. */
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Extension drop on tabs: deferring for category '%s', tag '%s'\n",
             category_id, resolved_tag_name.empty() ? "(none)" : resolved_tag_name.c_str());
      fflush(stdout);
    }

    /* Store tag name for deferred assignment (may be empty if no tag active) */
    if (!resolved_tag_name.empty()) {
      g_deferred_category_activation.tag_name_to_assign = resolved_tag_name;
    }
    else {
      g_deferred_category_activation.tag_name_to_assign.clear();
    }

    /* Register as pending extension category with tag_already_assigned=true.
     * This means: pending=false (user dropped onto tabs, category will be visible),
     * but we still need to activate the category when it appears. */
    register_new_extension_category(
        C, category_id, extension_id, space_type, mode_flag, /*tag_already_assigned=*/true);
  }
  else {
    /* Drop into viewport: set up deferred activation to discover and activate the new category.
     * This handles reserved-only extensions that need to switch to reserved category immediately.
     * For viewport drops, we show "New Add-ons!" button since the category won't be visible
     * without tag assignment. */
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Extension drop into viewport: setting up deferred activation for category '%s'\n",
             category_id);
      fflush(stdout);
    }

    /* Register extension callback if not already registered */
    deferred_category_activation_register_extension_callback();

    /* Save ALL known categories before extension installation to detect new ones later.
     * IMPORTANT: Use region->runtime->panels_category directly, NOT get_ordered_categories()!
     * get_ordered_categories() applies tag filtering, which would exclude categories without
     * the active tag. This would cause them to be incorrectly detected as "new" when the
     * extension installs. */
    ARegion *region = CTX_wm_region(C);
    if (region && region->runtime) {
      known_categories_before_extension_drop().clear();
      pending_category_insert().all_existing_categories.clear();
      for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
        if (pc_dyn.idname && pc_dyn.idname[0]) {
          known_categories_before_extension_drop().add(pc_dyn.idname);
          pending_category_insert().all_existing_categories.add(pc_dyn.idname);
        }
      }
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[KNOWN_CATS] Saving known categories before viewport extension drop:\n");
        for (const std::string &known_category : known_categories_before_extension_drop()) {
          printf("[KNOWN_CATS]   + '%s'\n", known_category.c_str());
        }
        printf("[KNOWN_CATS] Total: %zu categories saved\n",
               known_categories_before_extension_drop().size());
        fflush(stdout);
      }
    }

    /* Set up deferred activation to wait for extension installation signal,
     * then discover and activate the new category that appears. */
    g_deferred_category_activation.category_id.clear(); /* Will be set when new category appears */
    g_deferred_category_activation.valid = true;
    g_deferred_category_activation.timestamp = BLI_time_now_seconds();
    g_deferred_category_activation.wait_for_extension_signal = true;
    g_deferred_category_activation.extension_signal_received = false;
    g_deferred_category_activation.frame_delay = 0; /* No frame delay when waiting for signal */
    g_deferred_category_activation.discover_new_category = true; /* Find new category after install */
    g_deferred_category_activation.discover_retry_count = 0; /* Reset retry counter */
    g_deferred_category_activation.source_extension_id = extension_id;
    g_deferred_category_activation.activation_space_type = space_type;
    g_deferred_category_activation.activation_mode_flag = mode_flag;
    g_deferred_category_activation.tag_already_assigned = false;

    /* Save tag name for deferred assignment when the new category appears */
    if (!resolved_tag_name.empty()) {
      g_deferred_category_activation.tag_name_to_assign = resolved_tag_name;
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE] Will assign tag '%s' to new category when it appears\n", resolved_tag_name.c_str());
      }
    }
    else {
      g_deferred_category_activation.tag_name_to_assign.clear();
    }

    /* Build tag_key for deferred save */
    blender::ui::TagFilterStateRef tag_state{};
    ScrArea *area_for_tag = CTX_wm_area(C);
    if (blender::ui::tag_filter_state_from_area(area_for_tag, &tag_state) &&
        tag_state.active_tags) {
      char tag_key_buf[256];
      blender::ui::tag_build_combination_key(
          tag_state.active_tags, tag_key_buf, sizeof(tag_key_buf));
      g_deferred_category_activation.tag_key = tag_key_buf;
    }
    else {
      g_deferred_category_activation.tag_key.clear();
    }

    /* Register as pending extension category (this will call Python mark_category_from_extension) */
    register_new_extension_category(
        C, category_id, extension_id, space_type, mode_flag, /*tag_already_assigned=*/false);

    /* Auto-activate "New Add-ons!" filter to show the new category immediately.
     * This ensures users see the new category right after extension installation.
     * Only activate once - don't re-activate if filter was already auto-activated.
     * NOTE: Don't set active category here - it doesn't exist yet. Will be set in
     * deferred activation when category actually appears in the UI. */
    ScrArea *area = CTX_wm_area(C);
    if (area && !is_new_addon_filter_auto_activated(area)) {
      set_new_addon_filter_active(area, true, /*auto_activated=*/true);
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[NEW ADDON AUTO-ACTIVATE] Activated filter for viewport drop: category='%s', extension='%s'\n",
               category_id, extension_id);
        fflush(stdout);
      }
    }

    /* Force immediate UI refresh to show the new category and activate the filter */
    category_tabs_tag_refresh_active_area_ui(C);

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Deferred activation setup complete for viewport drop\n");
      fflush(stdout);
    }
  }
#else
  UNUSED_VARS(category_id, extension_id, tab_category, tag_name);
#endif
}

/**
 * Set up deferred category activation for viewport (WINDOW region) extension drops.
 * This is called from screen_ops.cc when an extension is dropped into a viewport
 * (not onto category tabs). It sets up the same deferred activation mechanism as
 * handle_extension_drop_on_tabs() but can be called from outside the tab drop flow.
 *
 * This is needed for reserved-only extensions like Bool Tool, where all panels
 * go into reserved categories (e.g., "Edit"). The deferred activation will:
 * 1. Wait for extension installation signal
 * 2. Discover if it's a reserved-only extension
 * 3. Switch to the appropriate reserved category
 */
void category_tabs_setup_viewport_drop_deferred(const bContext *C,
                                                 const char *extension_id,
                                                 int space_type,
                                                 uint32_t mode_flag)
{
#ifdef WITH_PYTHON
  if (!C) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[VIEWPORT DROP DEFERRED] Invalid parameters: C=%p, extension_id=%s\n",
             C,
             extension_id ? extension_id : "(null)");
    }
    return;
  }

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[VIEWPORT DROP DEFERRED] Setting up deferred activation for extension '%s' (space_type=%d, mode_flag=%#010x)\n",
           extension_id ? extension_id : "",
           space_type,
           mode_flag);
    fflush(stdout);
  }

  /* Register extension callback if not already registered */
  deferred_category_activation_register_extension_callback();

  /* Save ALL known categories before extension installation to detect new ones later.
   * IMPORTANT: Get region from area, not from context, because context region may not have
   * panels_category runtime data. We need the region that actually contains category tabs. */
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = nullptr;
  if (area && area->regionbase.first) {
    /* Find the UI region that contains category tabs */
    for (ARegion *ar = static_cast<ARegion *>(area->regionbase.first); ar; ar = ar->next) {
      if (ar->runtime && ar->runtime->panels_category.first) {
        region = ar;
        break;
      }
    }
  }
  
  known_categories_before_extension_drop().clear();
  pending_category_insert().all_existing_categories.clear();
  if (region && region->runtime) {
    for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
      if (pc_dyn.idname && pc_dyn.idname[0]) {
        known_categories_before_extension_drop().add(pc_dyn.idname);
        pending_category_insert().all_existing_categories.add(pc_dyn.idname);
      }
    }
  }
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[VIEWPORT DROP DEFERRED] Saving known categories from region=%p:\n", (void*)region);
    if (known_categories_before_extension_drop().is_empty()) {
      printf("[VIEWPORT DROP DEFERRED]   (no categories found - region may not have panels_category yet)\n");
    }
    for (const std::string &known_category : known_categories_before_extension_drop()) {
      printf("[VIEWPORT DROP DEFERRED]   + '%s'\n", known_category.c_str());
    }
    printf("[VIEWPORT DROP DEFERRED] Total: %zu categories saved\n",
           known_categories_before_extension_drop().size());
    fflush(stdout);
  }

  /* Set up deferred activation to wait for extension installation signal,
   * then discover and activate the new category that appears. */
  g_deferred_category_activation.category_id.clear(); /* Will be set when new category appears */
  g_deferred_category_activation.valid = true;
  g_deferred_category_activation.timestamp = BLI_time_now_seconds();
  g_deferred_category_activation.wait_for_extension_signal = true;
  g_deferred_category_activation.extension_signal_received = false;
  g_deferred_category_activation.frame_delay = 0; /* No frame delay when waiting for signal */
  g_deferred_category_activation.discover_new_category = true; /* Find new category after install */
  g_deferred_category_activation.discover_retry_count = 0; /* Reset retry counter */
  g_deferred_category_activation.source_extension_id = extension_id;
  g_deferred_category_activation.activation_space_type = space_type;
  g_deferred_category_activation.activation_mode_flag = mode_flag;
  g_deferred_category_activation.tag_already_assigned = false;
  g_deferred_category_activation.tag_name_to_assign.clear();

  /* Build tag_key for deferred save */
  blender::ui::TagFilterStateRef tag_state{};
  ScrArea *area_for_tag = CTX_wm_area(C);
  if (blender::ui::tag_filter_state_from_area(area_for_tag, &tag_state) &&
      tag_state.active_tags) {
    char tag_key_buf[256];
    blender::ui::tag_build_combination_key(
        tag_state.active_tags, tag_key_buf, sizeof(tag_key_buf));
    g_deferred_category_activation.tag_key = tag_key_buf;
  }
  else {
    g_deferred_category_activation.tag_key.clear();
  }

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[VIEWPORT DROP DEFERRED] Deferred activation setup complete\n");
    fflush(stdout);
  }
#else
  UNUSED_VARS(C, extension_id, space_type, mode_flag);
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name List Validation Utilities
 * \{ */

/**
 * Check if a ListBase containing CategoryGlyphItem appears to be valid.
 * After file read, the list may contain garbage pointers from the old file's memory space.
 */
bool category_glyph_list_is_valid(const ListBase *list)
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

static bool workspace_category_order_list_is_valid(const ListBase *list)
{
  if (list == nullptr || list->first == nullptr) {
    return true; /* Empty list is valid. */
  }

  const WorkspaceCategoryOrder *first = static_cast<const WorkspaceCategoryOrder *>(list->first);

  if (first->prev != nullptr) {
    return false;
  }

  if (first->next == reinterpret_cast<const void *>(static_cast<intptr_t>(-1))) {
    return false;
  }

  return true;
}

bool category_tag_list_is_valid(const ListBase *list)
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

/**
 * Normalize category name for canonicalization-aware matching.
 * Mirrors Python's _normalize_category_key for ASCII input: removes non-alphanumeric ASCII
 * characters and converts to lowercase. Non-ASCII bytes are dropped, so results may differ from
 * the Python implementation for non-ASCII names.
 */
static std::string normalize_category_key(const char *category)
{
  if (!category) {
    return "";
  }
  
  std::string result;
  result.reserve(strlen(category));
  
  for (const char *c = category; *c; c++) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) {
      result += *c;
    }
    else if (*c >= 'A' && *c <= 'Z') {
      result += (*c + ('a' - 'A')); /* Convert to lowercase. */
    }
    /* Skip all other characters (spaces, punctuation, etc.). */
  }
  
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Utilities
 * \{ */

const char *category_tags_string_lookup(const wmWindowManager *wm,
                                         const char *category,
                                         int space_type)
{
  if (wm == nullptr || category == nullptr) {
    return "";
  }

  /* First check overrides with priority order: exact -> global -> normalized exact -> normalized
   * global. */
  if (const CategoryGlyphItem *item = category_item_find_overrides(wm, category, space_type)) {
    return item->tags;
  }

  /* Then check mappings with same priority order. */
  if (const CategoryGlyphItem *item = category_item_find_mappings(wm, category, space_type)) {
    return item->tags;
  }

  return "";
}

const char *category_active_tag_first_get(const bContext *C)
{
  if (!C) {
    return nullptr;
  }

  const ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return nullptr;
  }

  const char *active_tags = nullptr;
  if (area->spacetype == SPACE_VIEW3D) {
    const View3D *v3d = static_cast<const View3D *>(area->spacedata.first);
    if (v3d) {
      active_tags = v3d->tabs_state.active_tag_filter_tags;
    }
  }
  else if (area->spacetype == SPACE_PROPERTIES) {
    const SpaceProperties *sbuts = static_cast<const SpaceProperties *>(area->spacedata.first);
    if (sbuts) {
      active_tags = sbuts->tabs_state.active_tag_filter_tags;
    }
  }

  if (!active_tags || active_tags[0] == '\0') {
    return nullptr;
  }

  static char first_tag[64];
  const char *cursor = active_tags;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == ',' || *cursor == ';') {
      cursor++;
    }
    if (!*cursor) {
      break;
    }

    int i = 0;
    while (*cursor && *cursor != ',' && *cursor != ';' && i < int(sizeof(first_tag)) - 1) {
      first_tag[i++] = *cursor++;
    }
    while (i > 0 && first_tag[i - 1] == ' ') {
      i--;
    }
    first_tag[i] = '\0';

    if (first_tag[0] != '\0') {
      return first_tag;
    }
  }

  return nullptr;
}

bool category_has_tag(const char *tags_string, const char *tag_name)
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

bool tag_glyph_hex_to_utf8(const char *input, char r_utf8[8])
{
  r_utf8[0] = '\0';
  return hex_codepoint_to_utf8(input, r_utf8, 8);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Utilities
 * \{ */

bool set_glyph_color(const int fontid,
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

void darken_color_3ub(uchar color[3], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = uchar(color[i] * (1.0f - factor));
  }
}

void brighten_color_4fv(float color[4], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = color[i] + (1.0f - color[i]) * factor;
  }
  /* Alpha remains unchanged. */
}

void apply_glyph_darkening(const int fontid, uchar color[3], const float darken_factor)
{
  if (darken_factor <= 0.0f) {
    return;
  }

  darken_color_3ub(color, darken_factor);
  BLF_color3ubv(fontid, color);
}

static void category_tab_icon_tint_get(const int icon_id,
                                       const float custom_color[3],
                                       const float darken_factor,
                                       uchar r_tint[4])
{
  const bool has_custom_color = !is_zero_v3(custom_color);
  r_tint[0] = 255;
  r_tint[1] = 255;
  r_tint[2] = 255;
  r_tint[3] = 255;

  const bool is_monochrome = icon_is_monochrome(icon_id);

  if (is_monochrome && has_custom_color) {
    r_tint[0] = uchar(custom_color[0] * 255.0f);
    r_tint[1] = uchar(custom_color[1] * 255.0f);
    r_tint[2] = uchar(custom_color[2] * 255.0f);
  }
  else if (is_monochrome) {
    /* Standard theme color for monochrome icons. */
    uchar theme_icon_color[4];
    if (icon_get_theme_color(icon_id, theme_icon_color)) {
      copy_v4_v4_uchar(r_tint, theme_icon_color);
    }
  }
  else {
    /* Multi-color SVG/icon - keep white tint (draws original colors). */
  }

  if (darken_factor > 0.0f) {
    darken_color_3ub(r_tint, darken_factor);
  }
}

int category_tab_icon_id_resolve(const CategoryTabIconResolved &icon_resolved)
{
  if (icon_resolved.source == CATEGORY_TAB_ICON_SOURCE_OFF) {
    return ICON_NONE;
  }
  return category_tab_icon_id_resolve_from_key_path(icon_resolved.key, icon_resolved.path);
}

void draw_category_tab_builtin_icon(const rcti *rct,
                                           const int icon_id,
                                           const float icon_center_y,
                                           const float icon_size_px,
                                           const float custom_color[3],
                                           const bool /*is_active*/,
                                           const float darken_factor,
                                           const uchar /*theme_col_text*/[3],
                                           const uchar /*theme_col_text_sel*/[3])
{
  if (icon_id == ICON_NONE) {
    return;
  }

  uchar icon_tint[4];
  category_tab_icon_tint_get(icon_id, custom_color, darken_factor, icon_tint);

  const float icon_draw_size = std::max(icon_size_px, 10.0f * UI_SCALE_FAC);
  const float center_x = float(rct->xmin + rct->xmax) * 0.5f;
  const float icon_pos_x = center_x - icon_draw_size * 0.5f;
  const float icon_pos_y = icon_center_y - icon_draw_size * 0.5f;
  const float icon_aspect = float(ICON_DEFAULT_WIDTH) / icon_draw_size;

  GPU_blend(GPU_BLEND_ALPHA);
  icon_draw_ex(icon_pos_x,
               icon_pos_y,
               icon_id,
               icon_aspect,
               1.0f,
               0.0f,
               icon_tint,
               false,
               UI_NO_ICON_OVERLAY_TEXT);
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reserved Categories
 * \{ */

bool category_name_is_glyph(const char *category_id)
{
  if (category_id == nullptr || category_id[0] == '\0') {
    return false;
  }

  /* Only a single UTF-8 character can be a glyph name. */
  if (!is_single_glyph_str(category_id)) {
    return false;
  }

  /* ASCII is never a glyph. */
  if ((category_id[0] & 0x80) == 0) {
    return false;
  }

  const unsigned int codepoint = BLI_str_utf8_as_unicode_safe(category_id);
  if (codepoint == BLI_UTF8_ERR) {
    return false;
  }

  /* Glyph category names use the Basic Multilingual Plane Private Use Area. */
  return (codepoint >= 0xE000 && codepoint <= 0xF8FF);
}

/**
 * Get reserved category priority from Python.
 * Returns -1 if category is not in priority list, 0+ for known categories.
 * Lower value = higher priority (appears earlier).
 */
static int get_reserved_category_priority_py(const bContext *C,
                                             const char *category_id,
                                             const char *space_type_name)
{
  if (!category_id || category_id[0] == '\0') {
    return -1;
  }

  /* Query lives in the centralized Python bridge. */
  return category_py_get_reserved_priority(
      const_cast<bContext *>(C), category_id, space_type_name);
}

/**
 * Comparator for sorting reserved categories by priority.
 */
static bool compare_reserved_categories_by_priority(
    const bContext *C,
    const char *a,
    const char *b,
    const char *space_type_name)
{
  int prio_a = get_reserved_category_priority_py(C, a, space_type_name);
  int prio_b = get_reserved_category_priority_py(C, b, space_type_name);

  /* Both known - sort by priority */
  if (prio_a >= 0 && prio_b >= 0) {
    return prio_a < prio_b;
  }
  /* One known, one unknown - known comes first */
  if (prio_a >= 0) return true;
  if (prio_b >= 0) return false;
  /* Both unknown - sort alphabetically */
  return BLI_strcasecmp_natural(a, b) < 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tag Mode Functions
 * \{ */

/**
 * Get the current object mode as a CategoryTagMode bitmask.
 * For edit mode, returns a detailed flag based on object type (mesh_edit, curve_edit, etc.)
 */
uint32_t get_current_tag_mode_flag(const bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    if (area->spacetype == SPACE_NODE) {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      if (snode) {
        if (STREQ(snode->tree_idname, "GeometryNodeTree")) {
          return static_cast<uint32_t>(CategoryTagMode::GEOMETRY_NODES);
        }
        if (STREQ(snode->tree_idname, "ShaderNodeTree")) {
          return static_cast<uint32_t>(CategoryTagMode::SHADER_EDITOR);
        }
      }
    }
    else if (area->spacetype == SPACE_IMAGE) {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      if (sima) {
        if (sima->mode == SI_MODE_PAINT) {
          return static_cast<uint32_t>(CategoryTagMode::IMAGE_PAINT);
        }
        if (sima->mode == SI_MODE_UV) {
          return static_cast<uint32_t>(CategoryTagMode::UV_EDIT);
        }
      }
    }
  }

  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
  }

  switch (ob->mode) {
    case OB_MODE_OBJECT:
      return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
    case OB_MODE_EDIT: {
      /* Return detailed edit mode flag based on object type.
       * This matches bl_context values like "mesh_edit", "curve_edit", etc. */
      switch (ob->type) {
        case OB_MESH:
          return static_cast<uint32_t>(CategoryTagMode::MESH_EDIT);
        case OB_CURVES_LEGACY:
        case OB_CURVES:
          return static_cast<uint32_t>(CategoryTagMode::CURVE_EDIT);
        case OB_SURF:
          return static_cast<uint32_t>(CategoryTagMode::SURFACE_EDIT);
        case OB_ARMATURE:
          return static_cast<uint32_t>(CategoryTagMode::ARMATURE_EDIT);
        case OB_LATTICE:
          return static_cast<uint32_t>(CategoryTagMode::LATTICE_EDIT);
        case OB_MBALL:
          return static_cast<uint32_t>(CategoryTagMode::META_EDIT);
        case OB_FONT:
          return static_cast<uint32_t>(CategoryTagMode::FONT_EDIT);
        case OB_GREASE_PENCIL:
          return static_cast<uint32_t>(CategoryTagMode::GREASE_PENCIL_EDIT);
        case OB_POINTCLOUD:
          return static_cast<uint32_t>(CategoryTagMode::POINTCLOUD_EDIT);
        case OB_VOLUME:
          return static_cast<uint32_t>(CategoryTagMode::VOLUME_EDIT);
        default:
          /* Fallback to generic EDIT_MODE for unknown object types */
          return static_cast<uint32_t>(CategoryTagMode::EDIT_MODE);
      }
    }
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Visibility by Tags
 * \{ */

/**
 * Check if the category passes the tag filter.
 * When multiple tags are active, category must have AT LEAST ONE of them (OR logic).
 */
static bool category_passes_tag_filter(const bContext *C, const char *category_idname)
{
  if (!C) {
    return true;
  }

  const wmWindowManager *wm = CTX_wm_manager(C);
  ScrArea *area = CTX_wm_area(C);

  /* Get active filter tags string and filter enabled flag from current space type */
  char active_tags[256] = "";
  bool filter_enabled = false;

  if (area) {
    if (area->spacetype == SPACE_VIEW3D) {
      /* Tag bar is in View3D */
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      if (v3d) {
        STRNCPY(active_tags, v3d->tabs_state.active_tag_filter_tags);
        filter_enabled = v3d->tabs_state.tag_filter_enabled;
      }
    }
    else if (area->spacetype == SPACE_PROPERTIES) {
      /* Tag bar might also be in Properties (for future use) */
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      if (sbuts) {
        STRNCPY(active_tags, sbuts->tabs_state.active_tag_filter_tags);
        filter_enabled = sbuts->tabs_state.tag_filter_enabled;
      }
    }
    else if (area->spacetype == SPACE_NODE) {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      if (snode) {
        STRNCPY(active_tags, snode->tabs_state.active_tag_filter_tags);
        filter_enabled = snode->tabs_state.tag_filter_enabled;
      }
    }
    else if (area->spacetype == SPACE_IMAGE) {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      if (sima) {
        STRNCPY(active_tags, sima->tabs_state.active_tag_filter_tags);
        filter_enabled = sima->tabs_state.tag_filter_enabled;
      }
    }
  }

  /* If filter is not enabled - show all categories */
  if (!filter_enabled || filter_enabled == 0) {
    return true;
  }

  /* If filter is enabled but no tags selected - show all (no filtering applied) */
  if (active_tags[0] == '\0') {
    return true;
  }

  /* Check if any active tag is valid for the current mode.
   * If none of the active tags apply to the current mode (e.g., user selected a tag
   * in Object Mode but switched to Edit Mode where that tag doesn't apply),
   * treat the filter as inactive so extension categories are not hidden.
   * This prevents the situation where switching modes leaves only reserved categories visible. */
  const uint32_t current_mode_flag = get_current_tag_mode_flag(C);
  bool any_tag_valid_for_mode = false;

  if (wm && category_tag_list_is_valid(&wm->category_tags)) {
    Vector<std::string> check_tag_list;
    category_tab_split_tags(active_tags, check_tag_list, ",;");

    for (const std::string &tag_name : check_tag_list) {
      for (const CategoryTagDef *tag_def =
               static_cast<const CategoryTagDef *>(wm->category_tags.first);
           tag_def;
           tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
      {
        if (STREQ(tag_name.c_str(), tag_def->name)) {
          /* Same mode matching logic as tag_bar_buttons_update:
           * - mode_flags == 0 means tag applies to all modes
           * - Direct bit match with current_mode_flag
           * - EDIT_MODE generalization for detailed edit modes */
          if (tag_def->mode_flags == 0 ||
              (tag_def->mode_flags & current_mode_flag) ||
              ((current_mode_flag & static_cast<uint32_t>(CategoryTagMode::EDIT_MODE_MASK)) &&
               (tag_def->mode_flags & static_cast<uint32_t>(CategoryTagMode::EDIT_MODE))))
          {
            any_tag_valid_for_mode = true;
          }
          break;
        }
      }
      if (any_tag_valid_for_mode) {
        break;
      }
    }
  }
  else {
    /* No tag definitions available - can't determine validity, allow filter */
    any_tag_valid_for_mode = true;
  }

  if (!any_tag_valid_for_mode) {
    return true; /* No active tag applies to current mode - show all categories */
  }

  /* Get category tags with space_type awareness */
  const int space_type = area ? area->spacetype : -1;
  const char *category_tags = category_tags_string_lookup(wm, category_idname, space_type);

  /* If category has no tags, it must not pass a normal tag filter.
   * Unassigned extension categories are shown via dedicated "New Add-ons!" filter,
   * not by leaking into an arbitrary active tag (e.g. AAA/BBB). */
  if (!category_tags || category_tags[0] == '\0') {
    if (g_tag_filter_debug_enabled) {
      printf("[TAG FILTER] Reject untagged category in normal tag filter: category='%s', active_tags='%s'\n",
             category_idname,
             active_tags);
      fflush(stdout);
    }
    return false;
  }

  /* Check if ANY active tag is present in the category (OR logic). */
  Vector<std::string> active_tag_list;
  category_tab_split_tags(active_tags, active_tag_list, ",;");

  for (const std::string &active_tag : active_tag_list) {
    if (has_tag_in_string(category_tags, active_tag.c_str())) {
      return true; /* Found at least one matching tag. */
    }
  }

  return false; /* No matching tags found in category. */
}

bool panel_category_is_visible_by_tags(const bContext *C,
                                       const wmWindowManager *wm,
                                       const char *category)
{
  const ScrArea *area = C ? CTX_wm_area(C) : nullptr;

  /* "New Add-on!" filter: when active, show ONLY pending (unassigned) categories.
   * This check must come BEFORE reserved categories check so reserved tabs are hidden
   * when the filter is active. */
  if (is_new_addon_filter_active(area)) {
    const int space_type = area ? area->spacetype : -1;
    const uint32_t mode_flag = get_current_tag_mode_flag(C);
    const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category, space_type);
    return category_is_unassigned_for_context(wm, item, space_type, mode_flag);
  }

  /* Reserved categories are always visible (when New Add-on filter is NOT active) */
  if (category_is_reserved_for_reorder(wm, category)) {
    return true;
  }

  /* Tag filtering - check horizontal tag bar filter */
  if (!category_passes_tag_filter(C, category)) {
    return false;
  }

  /* Get tags assigned to this category with space_type awareness */
  const int space_type = area ? area->spacetype : -1;
  const char *tags_string = category_tags_string_lookup(wm, category, space_type);
  if (tags_string == nullptr || tags_string[0] == '\0') {
    return true; /* No tags = always visible */
  }

  /* Categories with tags are visible by default.
   * Mode-based filtering is only applied for tag bar filtering (above),
   * not for general category visibility across different editors.
   * This ensures categories remain accessible even when they have
   * mode-specific tags assigned (e.g., UV Editor tags in 3D Viewport). */
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Order Functions
 * \{ */

/**
 * Get space type prefix for category order keys.
 * This ensures each editor type has its own independent category order storage.
 */
static std::string get_space_type_prefix(short space_type)
{
  switch (space_type) {
    case SPACE_VIEW3D:
      return "VIEW3D:";
    case SPACE_IMAGE:
      return "IMAGE:";
    case SPACE_NODE:
      return "NODE:";
    case SPACE_PROPERTIES:
      return "PROPS:";
    case SPACE_OUTLINER:
      return "OUTLINER:";
    case SPACE_FILE:
      return "FILE:";
    case SPACE_SEQ:
      return "SEQUENCE:";
    case SPACE_TEXT:
      return "TEXT:";
    case SPACE_CLIP:
      return "CLIP:";
    case SPACE_SPREADSHEET:
      return "SPREADSHEET:";
    default:
      return "OTHER:";
  }
}

/**
 * Generate a unique key for the current active tag combination.
 * Tags are sorted alphabetically to ensure consistent keys.
 * Key format: "SPACE_TYPE:tag1;tag2" (e.g., "VIEW3D:modeling", "IMAGE:animation;modeling")
 * Returns "SPACE_TYPE:" for no filter, ensuring each editor has independent storage.
 */
static std::string get_tag_combination_key(const wmWindowManager *wm, const bContext *C)
{
  UNUSED_VARS(wm);  /* Reserved for future use */

  char active_tags_buffer[256] = "";
  bool filter_enabled = false;

  ScrArea *area = CTX_wm_area(C);
  if (!area || !area->spacedata.first) {
    return "";  /* No area - no key */
  }

  /* Get space type prefix for this editor */
  const std::string space_prefix = get_space_type_prefix(area->spacetype);

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      STRNCPY(active_tags_buffer, v3d->tabs_state.active_tag_filter_tags);
      filter_enabled = v3d->tabs_state.tag_filter_enabled;
      break;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      STRNCPY(active_tags_buffer, sbuts->tabs_state.active_tag_filter_tags);
      filter_enabled = sbuts->tabs_state.tag_filter_enabled;
      break;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      STRNCPY(active_tags_buffer, snode->tabs_state.active_tag_filter_tags);
      filter_enabled = snode->tabs_state.tag_filter_enabled;
      break;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      STRNCPY(active_tags_buffer, sima->tabs_state.active_tag_filter_tags);
      filter_enabled = sima->tabs_state.tag_filter_enabled;
      break;
    }
    default:
      /* For unsupported space types, still return space prefix for independence */
      return space_prefix;
  }

  if (!filter_enabled) {
    return space_prefix;  /* Filter disabled - return space prefix only */
  }

  if (active_tags_buffer[0] == '\0') {
    return space_prefix;  /* Filter enabled but no tags */
  }

  /* Parse and collect tag names (without mutating source buffer). */
  Vector<std::string> active_tags;
  category_tab_split_tags(active_tags_buffer, active_tags, ",;");

  if (active_tags.is_empty()) {
    return space_prefix;
  }

  /* Sort alphabetically for consistent keys */
  std::sort(active_tags.begin(), active_tags.end());

  /* Join with semicolons and add space prefix */
  std::string key = space_prefix;
  for (int i = 0; i < active_tags.size(); i++) {
    if (i > 0) {
      key += ";";
    }
    key += active_tags[i];
  }

  return key;
}

/**
 * Load category order from JSON for a specific tag combination.
 * Calls Python function get_category_order().
 */
Vector<std::string> load_category_order_from_json(const bContext *C, const char *tag_key)
{
  Vector<std::string> result;
  if (!C) {
    return result;
  }

  /* Fetch the order as a JSON array string from the centralized Python bridge, then parse it here
   * (the minimal JSON parser stays at this call site). */
  const std::string json = category_py_get_category_order_json(const_cast<bContext *>(C), tag_key);
  if (!json.empty()) {
    category_tab_parse_json_string_array_minimal(json.c_str(), result);
  }
  return result;
}

/**
 * Save category order to JSON for a specific tag combination.
 * Calls Python function set_category_order().
 */
void save_category_order_to_json(const bContext *C,
                                        const char *tag_key,
                                        const Vector<std::string> &order)
{
  if (!C) {
    return;
  }
  /* Command construction lives in the centralized Python bridge. */
  category_py_set_category_order(const_cast<bContext *>(C), tag_key, order);
}


/* Forward declaration */
Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region);

static void category_order_pending_insert_expired_clear_if_needed(const std::string &tag_key)
{
  if (!(pending_category_insert().valid && pending_category_insert().tag_key == tag_key)) {
    return;
  }

  const double time_since_pending = BLI_time_now_seconds() - pending_category_insert().timestamp;
  if (time_since_pending > 120.0) {
    pending_category_insert().valid = false;
    pending_category_insert().all_existing_categories.clear();
  }
}

static Map<std::string, PanelCategoryDyn *> collect_visible_existing_categories(const bContext *C,
                                                                                const wmWindowManager *wm,
                                                                                ARegion *region)
{
  Map<std::string, PanelCategoryDyn *> existing;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      existing.add(std::string(pc_dyn.idname), &pc_dyn);
    }
  }
  return existing;
}

static Vector<PanelCategoryDyn *> collect_visible_remaining_categories(const bContext *C,
                                                                       const wmWindowManager *wm,
                                                                       ARegion *region,
                                                                       const Set<std::string> &added)
{
  Vector<PanelCategoryDyn *> remaining;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    std::string id(pc_dyn.idname);
    if (!added.contains(id) && panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      remaining.append(&pc_dyn);
    }
  }
  return remaining;
}

static void apply_json_order(const Vector<std::string> &json_order,
                             const Map<std::string, PanelCategoryDyn *> &existing,
                             Vector<PanelCategoryDyn *> &r_result,
                             Set<std::string> &r_added)
{
  for (const std::string &cat_id : json_order) {
    PanelCategoryDyn *const *pc = existing.lookup_ptr(cat_id);
    if (pc && !r_added.contains(cat_id)) {
      r_result.append(*pc);
      r_added.add(cat_id);
    }
  }
}

static const char *space_type_name_resolve(const ScrArea *area)
{
  if (!area) {
    return "DEFAULT";
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D:
      return "VIEW_3D";
    case SPACE_PROPERTIES:
      return "PROPERTIES";
    case SPACE_NODE:
      return "NODE_EDITOR";
    case SPACE_IMAGE:
      return "IMAGE_EDITOR";
    case SPACE_SEQ:
      return "SEQUENCE_EDITOR";
    case SPACE_CLIP:
      return "CLIP_EDITOR";
    case SPACE_TEXT:
      return "TEXT_EDITOR";
    case SPACE_ACTION:
      return "DOPESHEET_EDITOR";
    case SPACE_GRAPH:
      return "GRAPH_EDITOR";
    case SPACE_NLA:
      return "NLA_EDITOR";
    default:
      return "DEFAULT";
  }
}

static void append_remaining_categories_with_reserved_priority(
    const bContext *C,
    const wmWindowManager *wm,
    const char *space_type_name,
    const Vector<PanelCategoryDyn *> &remaining,
    Vector<PanelCategoryDyn *> &r_result,
    Set<std::string> &r_added)
{
  Vector<PanelCategoryDyn *> remaining_reserved;
  Vector<PanelCategoryDyn *> remaining_non_reserved;

  for (PanelCategoryDyn *pc_dyn : remaining) {
    const std::string id(pc_dyn->idname);
    if (!r_added.contains(id)) {
      if (category_is_reserved_for_reorder(wm, pc_dyn->idname)) {
        remaining_reserved.append(pc_dyn);
      }
      else {
        remaining_non_reserved.append(pc_dyn);
      }
    }
  }

  std::sort(remaining_reserved.begin(),
            remaining_reserved.end(),
            [&](PanelCategoryDyn *a, PanelCategoryDyn *b) {
              return compare_reserved_categories_by_priority(C, a->idname, b->idname, space_type_name);
            });

  for (PanelCategoryDyn *pc_dyn : remaining_reserved) {
    r_result.append(pc_dyn);
    r_added.add(pc_dyn->idname);
  }
  for (PanelCategoryDyn *pc_dyn : remaining_non_reserved) {
    r_result.append(pc_dyn);
    r_added.add(pc_dyn->idname);
  }
}

static void normalize_reserved_boundary_order(const bContext *C,
                                             const wmWindowManager *wm,
                                             const char *space_type_name,
                                             Vector<std::string> &order)
{
  Vector<std::string> reserved;
  Vector<std::string> non_reserved;
  reserved.reserve(order.size());
  non_reserved.reserve(order.size());

  for (const std::string &category_id : order) {
    if (category_is_reserved_for_reorder(wm, category_id.c_str())) {
      reserved.append(category_id);
    }
    else {
      non_reserved.append(category_id);
    }
  }

  std::sort(reserved.begin(), reserved.end(), [&](const std::string &a, const std::string &b) {
    return compare_reserved_categories_by_priority(C, a.c_str(), b.c_str(), space_type_name);
  });

  order.clear();
  order.reserve(reserved.size() + non_reserved.size());
  for (const std::string &category_id : reserved) {
    order.append(category_id);
  }
  for (const std::string &category_id : non_reserved) {
    order.append(category_id);
  }
}

/** Linear search for a category id within an ordered category vector. Returns -1 if absent. */
static int find_index_in_categories(const Vector<PanelCategoryDyn *> &cats, const std::string &id)
{
  if (id.empty()) {
    return -1;
  }
  for (int i = 0; i < cats.size(); i++) {
    if (STREQ(cats[i]->idname, id.c_str())) {
      return i;
    }
  }
  return -1;
}

/**
 * Detect categories that newly appeared in the region (e.g. an extension just installed) relative
 * to the snapshot captured before the drop. Extracted from #apply_pending_insert.
 *
 * New categories are intentionally NOT filtered through #panel_category_is_visible_by_tags: a fresh
 * extension category may not have tags yet and must remain discoverable regardless of tag filter.
 */
static Vector<PanelCategoryDyn *> collect_appeared_categories(const bContext *C,
                                                              const wmWindowManager *wm,
                                                              ARegion *region)
{
  Vector<PanelCategoryDyn *> appeared_categories;
  if (g_tag_filter_debug_enabled) {
    printf("[GET_ORDERED] all_existing_categories.size()=%zu, existing_categories.size()=%zu, known_categories_before_extension_drop().size()=%zu\n",
           pending_category_insert().all_existing_categories.size(),
           pending_category_insert().existing_categories.size(),
           known_categories_before_extension_drop().size());
  }
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    /* Use pending_category_insert().all_existing_categories when available (extension drop case),
     * as it contains ALL categories from the region, not just filtered ones. This prevents existing
     * categories without the active tag from being incorrectly detected as "new" when an extension
     * is dropped with an active tag filter. Fallback to known_categories_before_extension_drop() for
     * backwards compatibility, and finally to existing_categories (filtered) if neither is set. */
    const bool use_full_category_list = !pending_category_insert().all_existing_categories.is_empty() ||
                                        !known_categories_before_extension_drop().is_empty();
    const bool is_new_category = !pending_category_insert().all_existing_categories.is_empty() ?
                                     !pending_category_insert().all_existing_categories.contains(
                                         std::string(pc_dyn.idname)) :
                                     (!known_categories_before_extension_drop().is_empty() ?
                                          !known_categories_before_extension_drop().contains(
                                              std::string(pc_dyn.idname)) :
                                          !pending_category_insert().existing_categories.contains(
                                              std::string(pc_dyn.idname)));
    if (g_tag_filter_debug_enabled) {
      printf("[GET_ORDERED]   cat='%s' use_full=%d is_new=%d\n",
             pc_dyn.idname,
             use_full_category_list,
             is_new_category);
    }
    if (!is_new_category && !panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      continue;
    }
    if (is_new_category) {
      appeared_categories.append(&pc_dyn);
    }
  }
  return appeared_categories;
}

/**
 * Resolve the insertion index for pending-inserted categories using the anchor cascade
 * (anchor_after → anchor_before+1 → target ± insert_above → end). Clamped to `r_result`.
 * Extracted from #apply_pending_insert.
 */
static int compute_pending_insert_index(const Vector<PanelCategoryDyn *> &r_result)
{
  int insert_index = -1;
  if (!pending_category_insert().anchor_after.empty()) {
    insert_index = find_index_in_categories(r_result, pending_category_insert().anchor_after);
  }
  if (insert_index == -1 && !pending_category_insert().anchor_before.empty()) {
    const int before_index = find_index_in_categories(r_result,
                                                      pending_category_insert().anchor_before);
    if (before_index != -1) {
      insert_index = before_index + 1;
    }
  }
  if (insert_index == -1) {
    const int target_index = find_index_in_categories(r_result,
                                                      pending_category_insert().target_category);
    if (target_index != -1) {
      insert_index = pending_category_insert().insert_above ? target_index : (target_index + 1);
    }
  }
  if (insert_index == -1) {
    insert_index = r_result.size();
  }
  return clamp_i(insert_index, 0, r_result.size());
}

static void apply_pending_insert(const bContext *C,
                                 const wmWindowManager *wm,
                                 ARegion *region,
                                 const std::string &tag_key,
                                 const Vector<std::string> &json_order,
                                 const Map<std::string, PanelCategoryDyn *> &existing,
                                 Vector<PanelCategoryDyn *> &r_result,
                                 Set<std::string> &r_added,
                                 Vector<PanelCategoryDyn *> &r_remaining,
                                 bool &r_pending_applied,
                                 Vector<std::string> &r_pending_inserted_ids)
{
  if (!(pending_category_insert().valid && pending_category_insert().tag_key == tag_key)) {
    return;
  }

  Vector<PanelCategoryDyn *> appeared_categories = collect_appeared_categories(C, wm, region);
  if (appeared_categories.is_empty()) {
    return;
  }

  Set<std::string> appeared_ids;
  for (PanelCategoryDyn *pc_dyn : appeared_categories) {
    appeared_ids.add(std::string(pc_dyn->idname));
  }

  for (int i = r_result.size() - 1; i >= 0; i--) {
    const std::string id(r_result[i]->idname);
    if (appeared_ids.contains(id)) {
      r_result.remove_and_reorder(i);
      r_added.remove(id);
    }
  }

  for (int i = r_remaining.size() - 1; i >= 0; i--) {
    const std::string id(r_remaining[i]->idname);
    if (appeared_ids.contains(id)) {
      r_remaining.remove_and_reorder(i);
    }
  }

  /* Compute insertion point after the deletions above (indices are relative to r_result). */
  const int insert_index = compute_pending_insert_index(r_result);

  Vector<PanelCategoryDyn *> appeared_ordered;
  appeared_ordered.reserve(appeared_categories.size());
  Set<std::string> ordered_ids;
  for (const std::string &cat_id : json_order) {
    if (!appeared_ids.contains(cat_id) || ordered_ids.contains(cat_id)) {
      continue;
    }
    PanelCategoryDyn *const *pc = existing.lookup_ptr(cat_id);
    if (pc) {
      appeared_ordered.append(*pc);
      ordered_ids.add(cat_id);
    }
  }
  for (PanelCategoryDyn *pc_dyn : appeared_categories) {
    const std::string id(pc_dyn->idname);
    if (!ordered_ids.contains(id)) {
      appeared_ordered.append(pc_dyn);
      ordered_ids.add(id);
    }
  }

  int insert_offset = 0;
  for (PanelCategoryDyn *pc_dyn : appeared_ordered) {
    const std::string id(pc_dyn->idname);
    if (!r_added.contains(id)) {
      r_result.insert(insert_index + insert_offset, pc_dyn);
      insert_offset++;
      r_added.add(id);
      r_pending_inserted_ids.append(id);
    }
  }

  if (!r_pending_inserted_ids.is_empty()) {
    r_pending_applied = true;
    category_tabs_report_new_categories(C, r_pending_inserted_ids);
  }
}

static void normalize_runtime_result_reserved_first(const bContext *C,
                                                    const wmWindowManager *wm,
                                                    const char *space_type_name,
                                                    Vector<PanelCategoryDyn *> &r_result)
{
  Vector<PanelCategoryDyn *> reserved_sorted;
  Vector<PanelCategoryDyn *> non_reserved_ordered;
  reserved_sorted.reserve(r_result.size());
  non_reserved_ordered.reserve(r_result.size());

  for (PanelCategoryDyn *pc_dyn : r_result) {
    if (category_is_reserved_for_reorder(wm, pc_dyn->idname)) {
      reserved_sorted.append(pc_dyn);
    }
    else {
      non_reserved_ordered.append(pc_dyn);
    }
  }

  std::sort(reserved_sorted.begin(), reserved_sorted.end(), [&](PanelCategoryDyn *a, PanelCategoryDyn *b) {
    return compare_reserved_categories_by_priority(C, a->idname, b->idname, space_type_name);
  });

  Vector<PanelCategoryDyn *> normalized;
  normalized.reserve(r_result.size());
  for (PanelCategoryDyn *pc_dyn : reserved_sorted) {
    normalized.append(pc_dyn);
  }
  for (PanelCategoryDyn *pc_dyn : non_reserved_ordered) {
    normalized.append(pc_dyn);
  }

  r_result = std::move(normalized);
}

static Vector<std::string> build_pending_order_after_insert(const bContext *C,
                                                            const wmWindowManager *wm,
                                                            const char *space_type_name,
                                                            const std::string &tag_key,
                                                            const Vector<std::string> &json_order,
                                                            const Vector<PanelCategoryDyn *> &result,
                                                            const Vector<std::string> &pending_inserted_ids)
{
  Vector<std::string> pending_order = json_order;
  if (pending_order.is_empty()) {
    pending_order = pending_category_insert().pre_order;
  }

  if (pending_order.is_empty()) {
    Vector<std::string> from_result;
    from_result.reserve(result.size());
    for (PanelCategoryDyn *pc_dyn : result) {
      from_result.append(pc_dyn->idname);
    }
    normalize_reserved_boundary_order(C, wm, space_type_name, from_result);
    pending_order = std::move(from_result);
  }
  else if (category_order_is_crossing_reserved_boundary(wm, pending_order)) {
    normalize_reserved_boundary_order(C, wm, space_type_name, pending_order);
  }

  auto find_index_in_order = [&](const std::string &id) -> int {
    if (id.empty()) {
      return -1;
    }
    for (int i = 0; i < pending_order.size(); i++) {
      if (STREQ(pending_order[i].c_str(), id.c_str())) {
        return i;
      }
    }
    return -1;
  };

  auto erase_first_in_order = [&](const std::string &id) {
    const int idx = find_index_in_order(id);
    if (idx != -1) {
      pending_order.remove_and_reorder(idx);
    }
  };

  int order_insert_index = -1;
  if (!pending_category_insert().anchor_after.empty()) {
    order_insert_index = find_index_in_order(pending_category_insert().anchor_after);
  }
  if (order_insert_index == -1 && !pending_category_insert().anchor_before.empty()) {
    const int before_index = find_index_in_order(pending_category_insert().anchor_before);
    if (before_index != -1) {
      order_insert_index = before_index + 1;
    }
  }
  if (order_insert_index == -1) {
    const int target_index = find_index_in_order(pending_category_insert().target_category);
    if (target_index != -1) {
      order_insert_index = pending_category_insert().insert_above ? target_index : (target_index + 1);
    }
  }
  if (order_insert_index == -1) {
    order_insert_index = pending_order.size();
  }

  order_insert_index = clamp_i(order_insert_index, 0, pending_order.size());

  for (const std::string &id : pending_inserted_ids) {
    erase_first_in_order(id);
  }

  /* CRITICAL: Recalculate insert index after deletions to prevent vector bounds violation */
  order_insert_index = clamp_i(order_insert_index, 0, pending_order.size());

  int order_offset = 0;
  for (const std::string &id : pending_inserted_ids) {
    pending_order.insert(order_insert_index + order_offset, id);
    order_offset++;
  }

  if (!tag_key.empty() && tag_key.back() != ':') {
    Vector<std::string> filtered_order;
    Set<std::string> inserted_set;
    for (const std::string &id : pending_inserted_ids) {
      inserted_set.add(id);
    }
    for (const std::string &cat_id : pending_order) {
      const bool is_reserved = category_is_reserved_for_reorder(wm, cat_id.c_str());
      const bool is_newly_inserted = inserted_set.contains(cat_id);
      const bool passes_tag_filter = panel_category_is_visible_by_tags(C, wm, cat_id.c_str());
      if (is_reserved || is_newly_inserted || passes_tag_filter) {
        filtered_order.append(cat_id);
      }
    }
    pending_order = std::move(filtered_order);
  }

  normalize_reserved_boundary_order(C, wm, space_type_name, pending_order);
  return pending_order;
}

static void persist_pending_order(const bContext *C,
                                  ARegion *region,
                                  const std::string &tag_key,
                                  const Vector<std::string> &pending_order)
{
  save_category_order_to_json(C, tag_key.c_str(), pending_order);

  ScrArea *save_area = CTX_wm_area(C);
  const int save_space_type = save_area ? save_area->spacetype : 0;
  const int save_region_type = region->regiontype;
  WorkSpace *save_workspace = CTX_wm_workspace(C);
  if (!save_workspace) {
    return;
  }

  workspace_category_order_clear(save_workspace, save_space_type, save_region_type);
  for (int i = 0; i < pending_order.size(); i++) {
    WorkspaceCategoryOrder *item = MEM_new<WorkspaceCategoryOrder>(__func__);
    item->space_type = save_space_type;
    item->region_type = save_region_type;
    STRNCPY(item->category_id, pending_order[i].c_str());
    item->order_index = i;
    BLI_addtail(&save_workspace->category_order, item);
  }
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);
}

static void pending_insert_state_transfer_to_deferred_activation()
{
  /* Copy pending insert position to deferred activation BEFORE clearing it.
   * This ensures the insert position is preserved for deferred_category_activation_execute. */
  if (pending_category_insert().valid) {
    g_deferred_category_activation.pending_insert_valid = true;
    g_deferred_category_activation.pending_insert_tag_key = pending_category_insert().tag_key;
    g_deferred_category_activation.pending_insert_anchor_before = pending_category_insert().anchor_before;
    g_deferred_category_activation.pending_insert_anchor_after = pending_category_insert().anchor_after;
    g_deferred_category_activation.pending_insert_target_category =
        pending_category_insert().target_category;
    g_deferred_category_activation.pending_insert_insert_above =
        pending_category_insert().insert_above;
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ORDER] Copied pending insert to deferred activation: tag_key='%s', anchor_before='%s', anchor_after='%s'\n",
             pending_category_insert().tag_key.c_str(),
             pending_category_insert().anchor_before.c_str(),
             pending_category_insert().anchor_after.c_str());
    }
  }
  else {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ORDER] pending_category_insert().valid is FALSE - no position to copy!\n");
    }
  }
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    fflush(stdout);
  }

  pending_category_insert().valid = false;
  pending_category_insert().all_existing_categories.clear();
}

static void schedule_activation_for_pending_inserted_categories(
    const bContext *C,
    ARegion *region,
    const wmWindowManager *wm,
    const ScrArea *area,
    const Vector<std::string> &pending_inserted_ids)
{
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] pending_inserted_ids count: %zu\n", pending_inserted_ids.size());
  }
  if (pending_inserted_ids.is_empty()) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Activation block completed\n");
    }
    return;
  }

  for (const std::string &category_id : pending_inserted_ids) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Checking category: '%s'\n", category_id.c_str());
    }

    /* Use tag_already_assigned from deferred activation state.
     * This preserves the value set by handle_extension_drop_on_tabs or
     * category_tabs_apply_drop_insert (true for tab drops, false for viewport drops). */
    const bool tag_already_assigned = g_deferred_category_activation.tag_already_assigned;
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   tag_already_assigned: %s\n", tag_already_assigned ? "true" : "false");
    }

    register_new_extension_category(C,
                                    category_id.c_str(),
                                    pending_category_insert().source_extension_id.c_str(),
                                    area ? area->spacetype : -1,
                                    get_current_tag_mode_flag(C),
                                    tag_already_assigned);
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   Registered as pending extension category\n");
    }

    const bool is_visible = panel_category_is_visible_by_tags(C, wm, category_id.c_str());
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   is_visible: %s\n", is_visible ? "true" : "false");
    }

    if (!is_visible) {
      continue;
    }

    const char *current_active = panel_category_active_get(region, false);
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   current_active: '%s'\n", current_active ? current_active : "(null)");
    }

    const bool should_activate = (current_active == nullptr || !STREQ(category_id.c_str(), current_active));
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   should_activate: %s\n", should_activate ? "true" : "false");
    }

    if (should_activate) {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE]   Deferring activation for: '%s' (3 frame delay)\n",
               category_id.c_str());
      }

      bool already_has_category = !g_deferred_category_activation.category_id.empty();
      bool signal_already_received = g_deferred_category_activation.extension_signal_received;
      bool already_waiting_for_signal = g_deferred_category_activation.wait_for_extension_signal;

      if (already_has_category) {
        if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
          printf("[CATEGORY ACTIVATE]   Skipping - category already set: '%s'\n",
                 g_deferred_category_activation.category_id.c_str());
        }
        break;
      }

      g_deferred_category_activation.category_id = category_id;
      g_deferred_category_activation.valid = true;
      g_deferred_category_activation.timestamp = BLI_time_now_seconds();
      g_deferred_category_activation.frame_delay = 3;

      if (!signal_already_received && !already_waiting_for_signal) {
        g_deferred_category_activation.wait_for_extension_signal = true;
        g_deferred_category_activation.extension_signal_received = false;
      }
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE]   signal_already_received: %s, already_waiting: %s\n",
               signal_already_received ? "true" : "false",
               already_waiting_for_signal ? "true" : "false");
      }

      blender::ui::TagFilterStateRef tag_state{};
      ScrArea *area_for_tag = CTX_wm_area(C);
      /* Only use tag_key if filter is enabled. If filter is disabled,
       * category goes to general list (empty tag_key). */
      if (blender::ui::tag_filter_state_from_area(area_for_tag, &tag_state) && tag_state.active_tags &&
          tag_state.filter_enabled && *tag_state.filter_enabled)
      {
        char tag_key_buf[256];
        blender::ui::tag_build_combination_key(tag_state.active_tags, tag_key_buf, sizeof(tag_key_buf));
        g_deferred_category_activation.tag_key = tag_key_buf;
      }
      else {
        g_deferred_category_activation.tag_key.clear();
      }
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE]   Deferred activation scheduled\n");
      }
    }

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   Breaking loop after first visible category\n");
    }
    break;
  }

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] Activation block completed\n");
  }
}

static void autosave_initial_order_if_needed(const bContext *C,
                                             const wmWindowManager *wm,
                                             const char *space_type_name,
                                             const std::string &tag_key,
                                             const Vector<std::string> &json_order,
                                             const Vector<PanelCategoryDyn *> &result)
{
  if (!json_order.is_empty() || result.is_empty()) {
    return;
  }

  Vector<std::string> initial_order;
  initial_order.reserve(result.size());
  for (PanelCategoryDyn *pc_dyn : result) {
    initial_order.append(pc_dyn->idname);
  }

  normalize_reserved_boundary_order(C, wm, space_type_name, initial_order);

  if (!category_order_is_crossing_reserved_boundary(wm, initial_order)) {
    save_category_order_to_json(C, tag_key.c_str(), initial_order);
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ORDER] Auto-saved initial order for tag_key='%s' with %zu categories\n",
             tag_key.c_str(),
             initial_order.size());
    }
  }
}

int calculate_insert_index(const bContext *C,
                                  ARegion *region,
                                  CategoryDragState *state)
{
  const int min_insert_index = state->min_insert_index;
  const int max_insert_index = state->max_insert_index;
  int index = 0;

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    if (STREQ(pc_dyn.idname, state->drag_category_id)) {
      continue;
    }

    const int tab_height = BLI_rcti_size_y(&pc_dyn.rect);

    int y_shift = 0;
    if (!state->is_reserved) {
      int curr_disp = index;
      if (index >= state->original_index) {
        curr_disp++;
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

    const int tab_center_y = (pc_dyn.rect.ymax + y_shift) - tab_height / 2;

    int edge_offset;
    if (state->drag_offset_y > state->prev_drag_offset_y) {
      edge_offset = state->drag_top_edge_offset;
    }
    else {
      edge_offset = state->drag_bottom_edge_offset;
    }
    const int effective_y = state->drag_start_y + edge_offset + int(state->drag_offset_y);

    if (effective_y > tab_center_y) {
      const int insert_index = clamp_i(index, min_insert_index, max_insert_index);
      static double _tab_drag_last_log_time = 0.0;
      static int _tab_drag_last_insert_idx = -999;
      const double now = BLI_time_now_seconds();
#if TAB_DRAG_DEBUG_ENABLED
      const bool should_log = (now - _tab_drag_last_log_time > 1.0) ||
                              (_tab_drag_last_insert_idx != insert_index);
      if (should_log) {
        printf("[TAB_DRAG_HIT] drag='%s' effective_y=%d tab='%s' tab_center=%d raw_idx=%d insert_idx=%d bounds=[%d,%d]\n",
               state->drag_category_id,
               effective_y,
               pc_dyn.idname,
               tab_center_y,
               index,
               insert_index,
               min_insert_index,
               max_insert_index);
        _tab_drag_last_log_time = now;
        _tab_drag_last_insert_idx = insert_index;
      }
#endif
      return insert_index;
    }

    index++;
  }

  const int insert_index = clamp_i(index, min_insert_index, max_insert_index);
  static double _tab_drag_tail_last_log_time = 0.0;
  static int _tab_drag_tail_last_insert_idx = -999;
  const double tail_now = BLI_time_now_seconds();
#if TAB_DRAG_DEBUG_ENABLED
  const bool tail_should_log = (tail_now - _tab_drag_tail_last_log_time > 1.0) ||
                               (_tab_drag_tail_last_insert_idx != insert_index);
  if (tail_should_log) {
    printf("[TAB_DRAG_HIT] drag='%s' effective_y=TAIL raw_idx=%d insert_idx=%d bounds=[%d,%d]\n",
           state->drag_category_id,
           index,
           insert_index,
           min_insert_index,
           max_insert_index);
    _tab_drag_tail_last_log_time = tail_now;
    _tab_drag_tail_last_insert_idx = insert_index;
  }
#endif
  return insert_index;
}

void calculate_drag_insert_bounds(const wmWindowManager *wm,
                                         const Vector<PanelCategoryDyn *> &ordered_categories,
                                         const char *drag_category_id,
                                         const bool drag_is_reserved,
                                         int *r_min_insert_index,
                                         int *r_max_insert_index)
{
  int drag_full_index = -1;
  for (int i = 0; i < ordered_categories.size(); i++) {
    if (STREQ(ordered_categories[i]->idname, drag_category_id)) {
      drag_full_index = i;
      break;
    }
  }

  if (drag_full_index == -1) {
    *r_min_insert_index = 0;
    *r_max_insert_index = 0;
    return;
  }

  int opposite_before = -1;
  for (int i = drag_full_index - 1; i >= 0; i--) {
    const bool is_reserved = category_is_reserved_for_reorder(wm, ordered_categories[i]->idname);
    if (is_reserved != drag_is_reserved) {
      opposite_before = i;
      break;
    }
  }

  int opposite_after = -1;
  for (int i = drag_full_index + 1; i < ordered_categories.size(); i++) {
    const bool is_reserved = category_is_reserved_for_reorder(wm, ordered_categories[i]->idname);
    if (is_reserved != drag_is_reserved) {
      opposite_after = i;
      break;
    }
  }

  const int total_non_drag = max_ii(int(ordered_categories.size()) - 1, 0);

  int min_insert_index = 0;
  if (opposite_before != -1) {
    for (int i = 0; i <= opposite_before; i++) {
      if (i == drag_full_index) {
        continue;
      }
      min_insert_index++;
    }
  }

  int max_insert_index = total_non_drag;
  if (opposite_after != -1) {
    max_insert_index = 0;
    for (int i = 0; i < opposite_after; i++) {
      if (i == drag_full_index) {
        continue;
      }
      max_insert_index++;
    }
  }

  if (min_insert_index > max_insert_index) {
    min_insert_index = max_insert_index;
  }

  *r_min_insert_index = min_insert_index;
  *r_max_insert_index = max_insert_index;
}

static bool category_order_is_crossing_reserved_boundary(const wmWindowManager *wm,
                                                         const Vector<std::string> &order)
{
  bool seen_non_reserved = false;
  for (const std::string &category_id : order) {
    if (category_is_reserved_for_reorder(wm, category_id.c_str())) {
      if (seen_non_reserved) {
        return true;
      }
    }
    else {
      seen_non_reserved = true;
    }
  }
  return false;
}

void update_insert_zone(const bContext *C,
                               const wmWindowManager * /*wm*/,
                               ARegion *region,
                               CategoryDragState *state)
{
  int current_index = 0;
  int y_accumulated = 0;

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    const int tab_height = BLI_rcti_size_y(&pc_dyn.rect);

    if (current_index == state->current_insert_index) {
      state->insert_y_start = y_accumulated;
      state->insert_y_end = y_accumulated + tab_height + state->tab_v_pad;
      return;
    }

    if (!STREQ(pc_dyn.idname, state->drag_category_id)) {
      y_accumulated += tab_height + state->tab_v_pad;
    }
    current_index++;
  }

  state->insert_y_start = y_accumulated;
  state->insert_y_end = y_accumulated + state->drag_tab_height + state->tab_v_pad;
}

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

void apply_category_order(bContext *C, ARegion *region, CategoryDragState *state)
{
  ScrArea *area = CTX_wm_area(C);
  const wmWindowManager *wm = CTX_wm_manager(C);

  /* Get tag combination key for current filter state */
  std::string tag_key = get_tag_combination_key(wm, C);

  Vector<std::string> final_order;
  int insert_idx = 0;
  const int safe_insert_index = clamp_i(
      state->current_insert_index, state->min_insert_index, state->max_insert_index);

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;

    if (insert_idx == safe_insert_index) {
      final_order.append(state->drag_category_id);
    }

    if (!STREQ(pc_dyn.idname, state->drag_category_id)) {
      final_order.append(pc_dyn.idname);
      insert_idx++;
    }
  }

  if (insert_idx <= safe_insert_index &&
      !final_order.contains(state->drag_category_id))
  {
    final_order.append(state->drag_category_id);
  }

  /* Safety net: never allow mixed ordering between reserved and non-reserved.
   * If boundary crossing is detected, keep original order unchanged. */
  if (category_order_is_crossing_reserved_boundary(wm, final_order)) {
    return;
  }

  /* Save to JSON for this tag combination. */
  save_category_order_to_json(C, tag_key.c_str(), final_order);

  /* Also update WorkspaceCategoryOrder for backward compatibility */
  WorkSpace *workspace = CTX_wm_workspace(C);
  const int space_type = area ? area->spacetype : 0;
  const int region_type = region->regiontype;
  if (!workspace) {
    return;
  }

  workspace_category_order_clear(workspace, space_type, region_type);
  for (int i = 0; i < final_order.size(); i++) {
    WorkspaceCategoryOrder *item = MEM_new<WorkspaceCategoryOrder>(__func__);
    item->space_type = space_type;
    item->region_type = region_type;
    STRNCPY(item->category_id, final_order[i].c_str());
    item->order_index = i;
    BLI_addtail(&workspace->category_order, item);
  }

  /* Trigger full redraw to ensure new order is used */
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);
}

/**
 * Safe category activation that checks if we need to wait for extension installation.
 * This should be used instead of direct panel_category_active_set calls when the
 * activation might be triggered by extension installation.
 */
void panel_category_active_set_safe(const bContext *C,
                                    ARegion *region,
                                    const char *category_id,
                                    bool check_extension)
{
  if (!C || !region || !category_id || !category_id[0]) {
    return;
  }

  /* Check if this might be an extension category that needs deferred activation */
  if (check_extension) {
    /* Check if this category might be from an extension */
    const bool might_be_extension = (
        /* Check for common extension naming patterns */
        strstr(category_id, "ucupaint") != nullptr ||
        strstr(category_id, "extension") != nullptr ||
        strstr(category_id, "addon") != nullptr ||
        /* Check if category has extension-style naming (contains underscores/hyphens) */
        (strchr(category_id, '_') != nullptr || strchr(category_id, '-') != nullptr) ||
        /* Check if we're currently in the middle of an extension operation */
        g_deferred_category_activation.valid
    );
    
    /* If this might be an extension and we have pending operations, use deferred activation */
    if (might_be_extension) {
      /* Register extension callback if not already registered */
      deferred_category_activation_register_extension_callback();
      
      /* Set up deferred activation to wait for extension installation signal */
      g_deferred_category_activation.category_id = category_id;
      g_deferred_category_activation.valid = true;
      g_deferred_category_activation.timestamp = BLI_time_now_seconds();
      g_deferred_category_activation.wait_for_extension_signal = true;
      g_deferred_category_activation.extension_signal_received = false;
      g_deferred_category_activation.frame_delay = 0;
      
      /* Build tag_key for deferred save */
      blender::ui::TagFilterStateRef tag_state{};
      ScrArea *area_for_tag = CTX_wm_area(C);
      if (blender::ui::tag_filter_state_from_area(area_for_tag, &tag_state) &&
          tag_state.active_tags) {
        char tag_key_buf[256];
        blender::ui::tag_build_combination_key(
            tag_state.active_tags, tag_key_buf, sizeof(tag_key_buf));
        g_deferred_category_activation.tag_key = tag_key_buf;
      }
      else {
        g_deferred_category_activation.tag_key.clear();
      }
      
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE] Safe activation: deferred activation set for '%s', waiting for signal\n",
               category_id);
      }
      return;
    }
  }
  
  /* Direct activation for non-extension categories */
  panel_category_active_set(region, category_id);
}

/* -------------------------------------------------------------------- */
/** \name Extension Drop Preview State API
 * \{ */

/* Static state for rate-limited debug logging */
static double _ext_preview_last_log_time = 0.0;
static int _ext_preview_last_target_index = -999;
static bool _ext_preview_last_insert_above = false;
static char _ext_preview_last_target_id[UI_MAX_NAME_STR] = "";

void category_tabs_extension_preview_set(ARegion *region,
                                          const char *target_category_id,
                                          int target_index,
                                          bool insert_above,
                                          int tab_height,
                                          int tab_v_pad,
                                          int cursor_y)
{
  if (!region || !region->runtime) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[EXT_PREVIEW] preview_set: region or runtime is NULL\n");
    }
    return;
  }

  /* Don't activate preview if category drag is in progress. */
  if (region->runtime->category_tabs_drag_state) {
    ui::CategoryDragState *drag_state = static_cast<ui::CategoryDragState *>(
        region->runtime->category_tabs_drag_state);
    if (drag_state->is_dragging) {
      return;
    }
  }

  ui::ExtensionDropPreviewState &state = region->runtime->extension_drop_preview_state;

  /* Log only when state changes OR once per second (time-limited) */
  const double current_time = BLI_time_now_seconds();
  const bool time_elapsed = (current_time - _ext_preview_last_log_time) > 1.0;
  const bool state_changed = (_ext_preview_last_target_index != target_index ||
                               _ext_preview_last_insert_above != insert_above);

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    if (time_elapsed || state_changed) {
      const char *prev_target_id = state.target_category_id[0] ? state.target_category_id : "(none)";
      printf("[EXT_PREVIEW] preview_set: region=%p target='%s' idx=%d insert_above=%d h=%d pad=%d cursor_y=%d active_before=%d prev_target='%s' prev_idx=%d prev_insert_above=%d\n",
             static_cast<void *>(region),
             target_category_id ? target_category_id : "(null)",
             target_index,
             insert_above ? 1 : 0,
             tab_height,
             tab_v_pad,
             cursor_y,
             state.active ? 1 : 0,
             prev_target_id,
             state.target_index,
             state.insert_above ? 1 : 0);
      if (state.active) {
        const bool target_changed = !STREQ(prev_target_id, target_category_id ? target_category_id : "");
        const bool index_changed = (state.target_index != target_index);
        const bool side_changed = (state.insert_above != insert_above);
        if (target_changed || index_changed || side_changed) {
          printf("[EXT_PREVIEW] transition: target_changed=%d index_changed=%d side_changed=%d\n",
                 target_changed ? 1 : 0,
                 index_changed ? 1 : 0,
                 side_changed ? 1 : 0);
        }
      }
      _ext_preview_last_log_time = current_time;
      _ext_preview_last_target_index = target_index;
      _ext_preview_last_insert_above = insert_above;
      STRNCPY(_ext_preview_last_target_id, target_category_id ? target_category_id : "");
    }
  }

  state.active = true;
  if (target_category_id && target_category_id[0]) {
    STRNCPY(state.target_category_id, target_category_id);
  }
  else {
    state.target_category_id[0] = '\0';
  }
  state.target_index = target_index;
  state.insert_above = insert_above;
  state.tab_height = tab_height;
  state.tab_v_pad = tab_v_pad;
  state.cursor_y = cursor_y;
}

void category_tabs_extension_preview_clear(ARegion *region)
{
  if (!region || !region->runtime) {
    return;
  }

  ui::ExtensionDropPreviewState &state = region->runtime->extension_drop_preview_state;

  if (state.active) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[EXT_PREVIEW] preview_clear: region=%p was active target='%s' idx=%d insert_above=%d tab_h=%d pad=%d cursor_y=%d\n",
             static_cast<void *>(region),
             state.target_category_id,
             state.target_index,
             state.insert_above ? 1 : 0,
             state.tab_height,
             state.tab_v_pad,
             state.cursor_y);
    }
    /* Reset static variables for next drag operation */
    _ext_preview_last_target_index = -999;
    _ext_preview_last_insert_above = false;
    _ext_preview_last_log_time = 0.0;
  }

  state.active = false;
  state.target_category_id[0] = '\0';
  state.target_index = -1;
  state.tab_height = 0;
  state.tab_v_pad = 0;
}

bool category_tabs_extension_preview_is_active(const ARegion *region)
{
  if (!region || !region->runtime) {
    return false;
  }
  return region->runtime->extension_drop_preview_state.active;
}

bool category_tabs_extension_drop_target_from_mouse(const bContext *C,
                                                     ARegion *region,
                                                     int mouse_x_local,
                                                     int mouse_y_local,
                                                     int hit_margin,
                                                     const char **r_target_category_id,
                                                     int *r_target_index,
                                                     bool *r_insert_above,
                                                     int *r_tab_height)
{
  if (!C || !region || !region->runtime) {
    return false;
  }

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);
  if (ordered_categories.is_empty()) {
    return false;
  }
  
  /* CRITICAL FIX: Create a mapping from category name to full region index.
   * This ensures ghost positioning works correctly regardless of tag filter state.
   * get_ordered_categories() returns filtered results, but we need absolute indices
   * for consistent ghost positioning across different filter states. */
  Map<std::string, int> category_to_full_index;
  {
    int full_idx = 0;
    for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
      if (pc_dyn.idname && pc_dyn.idname[0] != '\0') {
        category_to_full_index.add(std::string(pc_dyn.idname), full_idx);
      }
      full_idx++;
    }
  }

  const wmWindowManager *wm = CTX_wm_manager(C);

  auto set_target_after_reserved_prefix = [&](const bool log_redirect) -> bool {
    int last_reserved_prefix_idx = -1;
    PanelCategoryDyn *last_reserved_prefix_tab = nullptr;

    int idx = 0;
    for (PanelCategoryDyn *pc_dyn : ordered_categories) {
      if (!category_is_reserved(wm, pc_dyn->idname)) {
        break;
      }
      last_reserved_prefix_idx = idx;
      last_reserved_prefix_tab = pc_dyn;
      idx++;
    }

    if (!last_reserved_prefix_tab) {
      return false;
    }

    if (r_target_category_id) {
      *r_target_category_id = last_reserved_prefix_tab->idname;
    }
    if (r_target_index) {
      /* Use full region index for consistent ghost positioning */
      int full_index = category_to_full_index.lookup_default(std::string(last_reserved_prefix_tab->idname), last_reserved_prefix_idx);
      *r_target_index = full_index;
    }
    if (r_insert_above) {
      *r_insert_above = false;
    }
    if (r_tab_height) {
      *r_tab_height = BLI_rcti_size_y(&last_reserved_prefix_tab->rect);
    }

    if (log_redirect) {
      static double _ext_hit_reserved_redirect_log_time = 0.0;
      const double current_time = BLI_time_now_seconds();
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        if (current_time - _ext_hit_reserved_redirect_log_time > 1.0) {
          printf("[EXT_HIT] local=(%d,%d) reserved_redirect -> target='%s' idx=%d insert_above=0\n",
                 mouse_x_local,
                 mouse_y_local,
                 last_reserved_prefix_tab->idname,
                 last_reserved_prefix_idx);
          _ext_hit_reserved_redirect_log_time = current_time;
        }
      }
    }

    return true;
  };

  /* Hard clamp in reserved-prefix zone.
   * While cursor is over reserved tabs (with hit margin), always preview insertion
   * after the reserved block. This prevents target oscillation between reserved,
   * settings/below-all, and neighboring tabs. */
  {
    bool has_reserved_prefix = false;
    rcti reserved_block_rect = {};
    bool reserved_block_rect_initialized = false;

    for (PanelCategoryDyn *pc_dyn : ordered_categories) {
      if (!category_is_reserved(wm, pc_dyn->idname)) {
        break;
      }

      has_reserved_prefix = true;
      const rcti tab_rect = pc_dyn->rect;
      if (!reserved_block_rect_initialized) {
        reserved_block_rect = tab_rect;
        reserved_block_rect_initialized = true;
      }
      else {
        reserved_block_rect.xmin = std::min(reserved_block_rect.xmin, tab_rect.xmin);
        reserved_block_rect.xmax = std::max(reserved_block_rect.xmax, tab_rect.xmax);
        reserved_block_rect.ymin = std::min(reserved_block_rect.ymin, tab_rect.ymin);
        reserved_block_rect.ymax = std::max(reserved_block_rect.ymax, tab_rect.ymax);
      }
    }

    if (has_reserved_prefix && reserved_block_rect_initialized) {
      if (hit_margin > 0) {
        reserved_block_rect.xmin -= hit_margin;
        reserved_block_rect.xmax += hit_margin;
        reserved_block_rect.ymin -= hit_margin;
        reserved_block_rect.ymax += hit_margin;
      }

      if (BLI_rcti_isect_pt(&reserved_block_rect, mouse_x_local, mouse_y_local)) {
        if (set_target_after_reserved_prefix(true)) {
          return true;
        }
      }
    }
  }

  /* Check below_all_tabs FIRST to prevent ghost appearing at top when cursor below tabs */
  const PanelCategoryDyn *bottom_tab = nullptr;
  int bottom_tab_index = -1;
  int tabs_bottom_y = (1 << 30);

  int scan_index = 0;
  for (PanelCategoryDyn *pc_dyn : ordered_categories) {
    if (pc_dyn->rect.ymin < tabs_bottom_y) {
      tabs_bottom_y = pc_dyn->rect.ymin;
      bottom_tab = pc_dyn;
      bottom_tab_index = scan_index;
    }
    scan_index++;
  }

  if (bottom_tab && mouse_y_local < tabs_bottom_y) {
    /* If bottom tab is reserved, redirect to after reserved block instead of below_all */
    if (category_is_reserved(wm, bottom_tab->idname)) {
      if (set_target_after_reserved_prefix(true)) {
        return true;
      }
    }

    if (r_target_category_id) {
      *r_target_category_id = bottom_tab->idname;
    }
    if (r_target_index) {
      /* Use full region index for consistent ghost positioning */
      int full_index = category_to_full_index.lookup_default(std::string(bottom_tab->idname), bottom_tab_index);
      *r_target_index = full_index;
    }
    if (r_insert_above) {
      *r_insert_above = false;
    }
    if (r_tab_height) {
      *r_tab_height = BLI_rcti_size_y(&bottom_tab->rect);
    }

    static double _ext_hit_below_last_log_time = 0.0;
    const double current_time = BLI_time_now_seconds();
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (current_time - _ext_hit_below_last_log_time > 1.0) {
        printf("[EXT_HIT] local=(%d,%d) mode=below_all -> target='%s' idx=%d insert_above=0 tabs_bottom_y=%d\n",
               mouse_x_local,
               mouse_y_local,
               bottom_tab->idname,
               bottom_tab_index,
               tabs_bottom_y);
        _ext_hit_below_last_log_time = current_time;
      }
    }
    return true;
  }

  const rcti settings_rect = region->runtime->category_tabs_settings_rect;
  bool hit_settings_button = BLI_rcti_isect_pt(&settings_rect, mouse_x_local, mouse_y_local);
  if (!hit_settings_button && hit_margin > 0) {
    rcti settings_hit_rect = settings_rect;
    settings_hit_rect.xmin -= hit_margin;
    settings_hit_rect.xmax += hit_margin;
    settings_hit_rect.ymin -= hit_margin;
    settings_hit_rect.ymax += hit_margin;
    hit_settings_button = BLI_rcti_isect_pt(&settings_hit_rect, mouse_x_local, mouse_y_local);
  }

  if (hit_settings_button) {
    PanelCategoryDyn *first_tab = ordered_categories.first();
    if (!first_tab) {
      return false;
    }

    /* If first tab is reserved, redirect preview after reserved block. */
    if (category_is_reserved(wm, first_tab->idname)) {
      if (set_target_after_reserved_prefix(true)) {
        return true;
      }
      return false;
    }

    if (r_target_category_id) {
      *r_target_category_id = first_tab->idname;
    }
    if (r_target_index) {
      /* Use full region index for consistent ghost positioning */
      int full_index = category_to_full_index.lookup_default(std::string(first_tab->idname), 0);
      *r_target_index = full_index;
    }
    if (r_insert_above) {
      *r_insert_above = true;
    }
    if (r_tab_height) {
      *r_tab_height = BLI_rcti_size_y(&first_tab->rect);
    }

    static double _ext_hit_settings_last_log_time = 0.0;
    const double current_time = BLI_time_now_seconds();
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (current_time - _ext_hit_settings_last_log_time > 1.0) {
        printf("[EXT_HIT] local=(%d,%d) tab='Display Mode Settings' mode=settings -> target='%s' idx=0 insert_above=1\n",
               mouse_x_local,
               mouse_y_local,
               first_tab->idname);
        _ext_hit_settings_last_log_time = current_time;
      }
    }
    return true;
  }

  const PanelCategoryDyn *hit_tab = nullptr;
  int hit_visual_index = -1;
  bool hit_by_margin = false;

  const PanelCategoryDyn *margin_best_tab = nullptr;
  int margin_best_index = -1;
  int margin_best_score = (1 << 30);

  int visual_index = 0;
  for (PanelCategoryDyn *pc_dyn : ordered_categories) {
    const rcti tab_rect = pc_dyn->rect;

    if (BLI_rcti_isect_pt(&tab_rect, mouse_x_local, mouse_y_local)) {
      hit_tab = pc_dyn;
      hit_visual_index = visual_index;
      hit_by_margin = false;
      break;
    }

    if (hit_margin > 0) {
      rcti hit_rect = tab_rect;
      hit_rect.xmin -= hit_margin;
      hit_rect.xmax += hit_margin;
      hit_rect.ymin -= hit_margin;
      hit_rect.ymax += hit_margin;

      if (BLI_rcti_isect_pt(&hit_rect, mouse_x_local, mouse_y_local)) {
        const int dx = (mouse_x_local < tab_rect.xmin) ? (tab_rect.xmin - mouse_x_local) :
                       (mouse_x_local > tab_rect.xmax) ? (mouse_x_local - tab_rect.xmax) :
                                                      0;
        const int dy = (mouse_y_local < tab_rect.ymin) ? (tab_rect.ymin - mouse_y_local) :
                       (mouse_y_local > tab_rect.ymax) ? (mouse_y_local - tab_rect.ymax) :
                                                      0;
        const int score = dx + dy;

        if (score < margin_best_score) {
          margin_best_score = score;
          margin_best_tab = pc_dyn;
          margin_best_index = visual_index;
        }
      }
    }

    visual_index++;
  }

  if (!hit_tab && margin_best_tab) {
    hit_tab = margin_best_tab;
    hit_visual_index = margin_best_index;
    hit_by_margin = true;
  }

  if (!hit_tab) {
    return false;
  }

  const rcti tab_rect = hit_tab->rect;
  const int tab_center_y = (tab_rect.ymin + tab_rect.ymax) / 2;
  const bool insert_above = (mouse_y_local > tab_center_y);

  /* If trying to insert above a reserved category, redirect after reserved block. */
  if (insert_above) {
    if (category_is_reserved(wm, hit_tab->idname)) {
      if (set_target_after_reserved_prefix(true)) {
        return true;
      }
      return false;
    }
  }

  if (r_target_category_id) {
    *r_target_category_id = hit_tab->idname;
  }
  if (r_target_index) {
    /* CRITICAL FIX: Use full region index instead of filtered visual index.
     * This ensures ghost positioning works correctly when tag filters change
     * the visible set of tabs between mouse events. */
    int full_index = category_to_full_index.lookup_default(std::string(hit_tab->idname), hit_visual_index);
    *r_target_index = full_index;
  }
  if (r_insert_above) {
    *r_insert_above = insert_above;
  }
  if (r_tab_height) {
    *r_tab_height = BLI_rcti_size_y(&tab_rect);
  }

  static double _ext_hit_last_log_time = 0.0;
  static int _ext_hit_last_idx = -999;
  static bool _ext_hit_last_insert_above = false;
  static bool _ext_hit_last_by_margin = false;
  const double current_time = BLI_time_now_seconds();
  const bool should_log = (current_time - _ext_hit_last_log_time > 1.0) ||
                          (_ext_hit_last_idx != hit_visual_index) ||
                          (_ext_hit_last_insert_above != insert_above) ||
                          (_ext_hit_last_by_margin != hit_by_margin);
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    if (should_log) {
      printf("[EXT_HIT] local=(%d,%d) tab='%s' idx=%d mode=%s tab_y=[%d,%d] center=%d insert_above=%d\n",
             mouse_x_local,
             mouse_y_local,
             hit_tab->idname,
             hit_visual_index,
             hit_by_margin ? "margin" : "strict",
             tab_rect.ymin,
             tab_rect.ymax,
             tab_center_y,
             insert_above ? 1 : 0);
      _ext_hit_last_log_time = current_time;
      _ext_hit_last_idx = hit_visual_index;
      _ext_hit_last_insert_above = insert_above;
      _ext_hit_last_by_margin = hit_by_margin;
    }
  }

  return true;
}

/** \} */

void category_tabs_apply_drop_insert(bContext *C,
                                     ARegion *region,
                                     const char *category_id,
                                     const char *target_category_id,
                                     bool insert_above,
                                     const char *tag_name)
{
  /* Clear any active preview state before applying actual insert. */
  if (region && region->runtime) {
    category_tabs_extension_preview_clear(region);
  }

  if (!C || !region || !category_id || !category_id[0]) {
    return;
  }

  /* Register extension callback if not already registered */
  deferred_category_activation_register_extension_callback();

  /* For extension drop, we DON'T know the new category name yet.
   * Set up deferred activation to wait for extension installation signal,
   * then discover and activate the new category that appears.
   * Use empty category_id to indicate "discover new category" mode. */
  g_deferred_category_activation.category_id.clear(); /* Will be set when new category appears */
  g_deferred_category_activation.valid = true;
  g_deferred_category_activation.timestamp = BLI_time_now_seconds();
  g_deferred_category_activation.wait_for_extension_signal = true;
  g_deferred_category_activation.extension_signal_received = false;
  g_deferred_category_activation.frame_delay = 0; /* No frame delay when waiting for signal */
  g_deferred_category_activation.discover_new_category = true; /* Find new category after install */
  g_deferred_category_activation.discover_retry_count = 0; /* Reset retry counter */

  /* Save tag name for deferred assignment when the new category appears.
   * This allows assigning the active tag filter to the new category after extension install.
   * IMPORTANT: tag_already_assigned is always true for tab drops because the category will be
   * visible in the general list without tag filtering. We don't need "New Add-ons!" button.
   * If a tag is active, it will be assigned to the category when it appears.
   *
   * NOTE: tag_name is already filtered by filter_enabled in category_tab_extension_drop_copy.
   * If filter was disabled at drop time, tag_name will be empty. */
  if (tag_name && tag_name[0] != '\0') {
    g_deferred_category_activation.tag_name_to_assign = tag_name;
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Will assign tag '%s' to new category when it appears\n", tag_name);
    }
  }
  else {
    g_deferred_category_activation.tag_name_to_assign.clear();
  }
  g_deferred_category_activation.tag_already_assigned = true;

  /* Build tag_key for deferred save */
  blender::ui::TagFilterStateRef tag_state{};
  ScrArea *area_for_tag = CTX_wm_area(C);
  if (blender::ui::tag_filter_state_from_area(area_for_tag, &tag_state) &&
      tag_state.active_tags) {
    char tag_key_buf[256];
    blender::ui::tag_build_combination_key(
        tag_state.active_tags, tag_key_buf, sizeof(tag_key_buf));
    g_deferred_category_activation.tag_key = tag_key_buf;
  }
  else {
    g_deferred_category_activation.tag_key.clear();
  }

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] Extension drop: waiting for new category to appear (target was '%s')\n",
           category_id);
  }

  ScrArea *area = CTX_wm_area(C);
  const wmWindowManager *wm = CTX_wm_manager(C);

  /* Get tag combination key for current filter state. */
  std::string tag_key = get_tag_combination_key(wm, C);

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  /* Save ALL known categories before extension installation to detect new ones later.
   * IMPORTANT: Use region->runtime->panels_category directly, NOT get_ordered_categories()!
   * get_ordered_categories() applies tag filtering, which would exclude categories without
   * the active tag. This would cause them to be incorrectly detected as "new" when the
   * extension installs. */
  known_categories_before_extension_drop().clear();
  pending_category_insert().all_existing_categories.clear();
  for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (pc_dyn.idname && pc_dyn.idname[0]) {
      known_categories_before_extension_drop().add(pc_dyn.idname);
      pending_category_insert().all_existing_categories.add(pc_dyn.idname);
    }
  }
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[KNOWN_CATS] Saving known categories before extension drop:\n");
    for (const std::string &known_category : known_categories_before_extension_drop()) {
      printf("[KNOWN_CATS]   + '%s'\n", known_category.c_str());
    }
    printf("[KNOWN_CATS] Total: %zu categories (also saved to pending_insert)\n",
           known_categories_before_extension_drop().size());
  }

  Vector<std::string> json_order = load_category_order_from_json(C, tag_key.c_str());

  pending_category_insert_set(
      tag_key, target_category_id, insert_above, ordered_categories, json_order);

  /* For extension drop, defer JSON updates until the new category actually appears.
   * The category_id is the hovered tab (target), not the new extension category. */
  if (!category_id || !category_id[0] ||
      (target_category_id && target_category_id[0] && STREQ(category_id, target_category_id)))
  {
    return;
  }

  int target_index = -1;
  int drag_index = -1;
  for (int i = 0; i < ordered_categories.size(); i++) {
    const char *idname = ordered_categories[i]->idname;
    if (idname && idname[0]) {
      if (target_category_id && target_category_id[0] && STREQ(idname, target_category_id)) {
        target_index = i;
      }
      if (STREQ(idname, category_id)) {
        drag_index = i;
      }
    }
  }

  Vector<std::string> order;
  order.reserve(ordered_categories.size() + 1);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    const char *idname = pc_dyn_ptr->idname;
    if (idname && idname[0] && !STREQ(idname, category_id)) {
      order.append(idname);
    }
  }

  int insert_index = order.size();
  if (target_index != -1) {
    insert_index = insert_above ? target_index : (target_index + 1);
    if (drag_index != -1 && drag_index < insert_index) {
      insert_index--;
    }
  }

  insert_index = clamp_i(insert_index, 0, order.size());
  order.insert(insert_index, std::string(category_id));

  if (category_order_is_crossing_reserved_boundary(wm, order)) {
    return;
  }

  save_category_order_to_json(C, tag_key.c_str(), order);

  WorkSpace *workspace = CTX_wm_workspace(C);
  const int space_type = area ? area->spacetype : 0;
  const int region_type = region->regiontype;
  if (workspace) {
    workspace_category_order_clear(workspace, space_type, region_type);
    for (int i = 0; i < order.size(); i++) {
      WorkspaceCategoryOrder *item = MEM_new<WorkspaceCategoryOrder>(__func__);
      item->space_type = space_type;
      item->region_type = region_type;
      STRNCPY(item->category_id, order[i].c_str());
      item->order_index = i;
      BLI_addtail(&workspace->category_order, item);
    }

    WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
    WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);
  }
}

#include "interface_tag_bar.hh"

void panel_category_tabs_ensure_active_visible(const bContext *C, ARegion *region)
{
  if (!panel_category_tabs_is_visible(region)) {
    return;
  }

  ScrArea *area = CTX_wm_area(C);
  const wmWindowManager *wm = CTX_wm_manager(C);
  const int space_type = area ? area->spacetype : -1;

  const char *current_active = panel_category_active_get(region, false);

  /* Check if "New Add-ons!" filter should be auto-activated.
   * This handles cases where extension was installed but filter wasn't activated yet.
   * IMPORTANT: Only activate ONCE when category first appears. Don't re-activate if user
   * manually switched to other tags (indicated by active_tags being set) or if user
   * manually deactivated the filter (indicated by auto_activated flag being false). */
  if (area) {
    const bool filter_active = is_new_addon_filter_active(area);
    const bool was_auto_activated = is_new_addon_filter_auto_activated(area);
    const uint32_t current_mode_flag = get_current_tag_mode_flag(C);
    const bool has_unassigned = should_show_new_addon_tag(wm, space_type, current_mode_flag);

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[NEW ADDON CHECK] filter_active=%d was_auto_activated=%d has_unassigned=%d\n",
             filter_active ? 1 : 0, was_auto_activated ? 1 : 0, has_unassigned ? 1 : 0);
      fflush(stdout);
    }

    /* Only auto-activate if:
     * 1. Filter is NOT active
     * 2. Has unassigned categories
     * 3. Filter was NOT manually deactivated (auto_activated flag should be true for auto-reactivation)
     *
     * NOTE: After manual deactivation, auto_activated is set to false, preventing re-activation here. */
    if (!filter_active && has_unassigned) {
      /* If filter was auto-activated before, we can re-activate it.
       * If it was manually deactivated (auto_activated=false), don't re-activate. */
      if (was_auto_activated) {
        /* Check if user has manually selected other tags */
        TagFilterStateRef state{};
        bool user_selected_other_tags = false;
        if (tag_filter_state_from_area(area, &state) && state.active_tags && state.active_tags[0] != '\0') {
          user_selected_other_tags = true;
          if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
            printf("[NEW ADDON CHECK] User has selected other tags: '%s', skipping auto-activate\n",
                   state.active_tags);
            fflush(stdout);
          }
        }

        /* Only auto-activate if user hasn't selected other tags */
        if (!user_selected_other_tags) {
          if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
            printf("[NEW ADDON AUTO-ACTIVATE] Auto-activating filter (was_auto_activated=true)\n");
            fflush(stdout);
          }
          set_new_addon_filter_active(area, true, /*auto_activated=*/true);
        }
      }
      else {
        if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
          printf("[NEW ADDON CHECK] Filter was manually deactivated (was_auto_activated=false), skipping auto-activate\n");
          fflush(stdout);
        }
      }
    }
  }

  if (area && is_new_addon_filter_active(area)) {
    const uint32_t current_mode_flag = get_current_tag_mode_flag(C);
    const bool has_unassigned = should_show_new_addon_tag(wm, space_type, current_mode_flag);

    if (!has_unassigned) {
      set_new_addon_filter_active(area, false);
      set_saved_tag_filter_tags(area, "");

      TagFilterStateRef state{};
      if (tag_filter_state_from_area(area, &state) && state.active_tags && state.filter_enabled) {
        const char *category_for_restore =
            (current_active && current_active[0] != '\0') ? current_active : nullptr;

        const char *category_tags = nullptr;
        if (category_for_restore) {
          category_tags = category_tags_string_lookup(wm, category_for_restore, space_type);
        }

        Vector<std::string> tag_list;
        if (category_tags && category_tags[0] != '\0') {
          category_tab_split_tags(category_tags, tag_list, ",;");
        }

        if (!tag_list.is_empty() && !tag_list[0].empty()) {
          BLI_strncpy(state.active_tags, tag_list[0].c_str(), 256);
          *state.filter_enabled = 1;
        }
        else {
          state.active_tags[0] = '\0';
          *state.filter_enabled = 0;
        }
      }
    }
  }

  current_active = panel_category_active_get(region, false);

  if (current_active && panel_category_is_visible_by_tags(C, wm, current_active)) {
    return;
  }

  /* Current category is hidden or null. Try to restore from memory first. */
  char tag_key[256];
  TagFilterStateRef state{};
  /* Only use tag_key if filter is enabled. If filter is disabled,
   * use empty tag_key for general list. */
  if (tag_filter_state_from_area(CTX_wm_area(C), &state) && state.active_tags &&
      state.filter_enabled && *state.filter_enabled)
  {
    tag_build_combination_key(state.active_tags, tag_key, sizeof(tag_key));

    char saved_category[64];
    if (tag_get_last_active_category(
            const_cast<bContext *>(C), tag_key, saved_category, sizeof(saved_category)))
    {
      if (panel_category_is_visible_by_tags(C, wm, saved_category)) {
        panel_category_active_set_safe(C, region, saved_category);
        return;
      }
    }
  }

  Vector<PanelCategoryDyn *> visible_categories = get_ordered_categories(C, region);
  if (!visible_categories.is_empty()) {
    panel_category_active_set_safe(C, region, visible_categories[0]->idname);
  }
}

struct CategoryOrderPipelineState {
  Map<std::string, PanelCategoryDyn *> existing;
  Vector<PanelCategoryDyn *> result;
  Set<std::string> added;
  Vector<PanelCategoryDyn *> remaining;
  bool pending_applied = false;
  Vector<std::string> pending_inserted_ids;
};

struct CategoryOrderPipelineContext {
  const bContext *C;
  const wmWindowManager *wm;
  ARegion *region;
  ScrArea *area;
  const std::string *tag_key;
  const Vector<std::string> *json_order;
  const char *space_type_name;
};

static CategoryOrderPipelineState category_order_stage_collect(
    const CategoryOrderPipelineContext &ctx)
{
  CategoryOrderPipelineState state{};

  state.existing = collect_visible_existing_categories(ctx.C, ctx.wm, ctx.region);
  apply_json_order(*ctx.json_order, state.existing, state.result, state.added);
  state.remaining = collect_visible_remaining_categories(ctx.C, ctx.wm, ctx.region, state.added);

  return state;
}

static void category_order_stage_apply(const CategoryOrderPipelineContext &ctx,
                                       CategoryOrderPipelineState &r_state)
{
  apply_pending_insert(ctx.C,
                       ctx.wm,
                       ctx.region,
                       *ctx.tag_key,
                       *ctx.json_order,
                       r_state.existing,
                       r_state.result,
                       r_state.added,
                       r_state.remaining,
                       r_state.pending_applied,
                       r_state.pending_inserted_ids);

  append_remaining_categories_with_reserved_priority(
      ctx.C, ctx.wm, ctx.space_type_name, r_state.remaining, r_state.result, r_state.added);

  normalize_runtime_result_reserved_first(ctx.C, ctx.wm, ctx.space_type_name, r_state.result);
}

static void category_order_stage_persist(const CategoryOrderPipelineContext &ctx,
                                         CategoryOrderPipelineState &r_state)
{
  if (r_state.pending_applied) {
    Vector<std::string> pending_order = build_pending_order_after_insert(
        ctx.C,
        ctx.wm,
        ctx.space_type_name,
        *ctx.tag_key,
        *ctx.json_order,
        r_state.result,
        r_state.pending_inserted_ids);

    if (!category_order_is_crossing_reserved_boundary(ctx.wm, pending_order)) {
      persist_pending_order(ctx.C, ctx.region, *ctx.tag_key, pending_order);
    }

    pending_insert_state_transfer_to_deferred_activation();
    schedule_activation_for_pending_inserted_categories(
        ctx.C, ctx.region, ctx.wm, ctx.area, r_state.pending_inserted_ids);
  }

  autosave_initial_order_if_needed(
      ctx.C, ctx.wm, ctx.space_type_name, *ctx.tag_key, *ctx.json_order, r_state.result);
}

Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region)
{
  ScrArea *area = CTX_wm_area(C);
  const wmWindowManager *wm = CTX_wm_manager(C);

  /* Get tag combination key for current filter state */
  std::string tag_key = get_tag_combination_key(wm, C);

  /* Load order from JSON for this tag combination */
  Vector<std::string> json_order = load_category_order_from_json(C, tag_key.c_str());

  category_order_pending_insert_expired_clear_if_needed(tag_key);

  /* Get space type name for priority lookup (C++ is the source of truth). */
  const char *space_type_name = space_type_name_resolve(area);

  CategoryOrderPipelineContext ctx{};
  ctx.C = C;
  ctx.wm = wm;
  ctx.region = region;
  ctx.area = area;
  ctx.tag_key = &tag_key;
  ctx.json_order = &json_order;
  ctx.space_type_name = space_type_name;

  CategoryOrderPipelineState state = category_order_stage_collect(ctx);
  category_order_stage_apply(ctx, state);
  category_order_stage_persist(ctx, state);

  return state.result;
}

/* The `UI_OT_category_quick_focus` operator was split out into
 * interface_tab_categories_operators.cc (self-contained enum-search popup). */

/** \} */

}  // namespace blender::ui
#include "BKE_callbacks.hh"
