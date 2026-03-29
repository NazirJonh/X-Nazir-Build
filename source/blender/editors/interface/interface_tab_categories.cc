/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved.
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
#  define TAB_DRAG_DEBUG_ENABLED 1
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
 * Debug messages include:
 * - Category activation and deferred activation state
 * - Extension drop handling
 * - Category registration
 * - Category order changes
 * - Tag assignment operations
 * 
 * To enable: Change to `static constexpr bool CATEGORY_TAB_DEBUG_ENABLED = true;`
 */
static constexpr bool CATEGORY_TAB_DEBUG_ENABLED = false;

/** \} */

/* Forward declarations */
static bool category_name_is_glyph(const char *category_id);
static std::string normalize_category_key(const char *category);
static void panel_category_color_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        float r_color[3]);

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
static bool category_tab_should_expand_name(const ARegion *region,
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

static const CategoryGlyphItem *category_glyph_item_find_exact(const ListBase *list,
                                                               const char *category,
                                                               int space_type)
{
  if (!list || !category_glyph_list_is_valid(list)) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (category_item_match_exact(item, category, space_type)) {
      return item;
    }
  }
  return nullptr;
}

/**
 * Internal helper: Find category glyph item with global fallback (space_type = -1).
 * Only searches global if space_type != -1.
 */
static const CategoryGlyphItem *category_glyph_item_find_global(const ListBase *list,
                                                                const char *category,
                                                                int space_type)
{
  if (!list || !category_glyph_list_is_valid(list) || space_type == -1) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category) && item->space_type == -1) {
      return item;
    }
  }
  return nullptr;
}

/**
 * Internal helper: Find category glyph item with normalized exact match.
 * Uses normalize_category_key() for canonicalization fallback.
 */
static const CategoryGlyphItem *category_glyph_item_find_normalized_exact(const ListBase *list,
                                                                          const char *category,
                                                                          int space_type)
{
  if (!list || !category_glyph_list_is_valid(list)) {
    return nullptr;
  }

  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (category_item_match_normalized(item, normalized_target, space_type)) {
      return item;
    }
  }
  return nullptr;
}

/**
 * Internal helper: Find category glyph item with normalized global fallback.
 * Uses normalize_category_key() for canonicalization with global space_type = -1.
 */
static const CategoryGlyphItem *category_glyph_item_find_normalized_global(const ListBase *list,
                                                                           const char *category,
                                                                           int space_type)
{
  if (!list || !category_glyph_list_is_valid(list) || space_type == -1) {
    return nullptr;
  }

  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (category_item_match_normalized(item, normalized_target, -1)) {
      return item;
    }
  }
  return nullptr;
}

static const CategoryGlyphItem *category_glyph_item_find_global_only(const ListBase *list,
                                                                     const char *category)
{
  if (!list || !category_glyph_list_is_valid(list) || !category) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (item->space_type == -1 && STREQ(item->category, category)) {
      return item;
    }
  }

  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (item->space_type == -1 && category_item_match_normalized(item, normalized_target, -1)) {
      return item;
    }
  }

  return nullptr;
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

  // First pass: try exact match (category + space_type)
  if (const CategoryGlyphItem *item = category_glyph_item_find_exact(list, category, space_type)) {
    return item;
  }

  // Second pass: try global categories (space_type = -1) if not searching for global already
  if (const CategoryGlyphItem *item = category_glyph_item_find_global(list, category, space_type)) {
    return item;
  }

  // Third pass: try canonicalization fallback with space_type match
  if (const CategoryGlyphItem *item = category_glyph_item_find_normalized_exact(list, category, space_type)) {
    return item;
  }

  // Fourth pass: try canonicalization fallback with global categories
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

static const CategoryGlyphItem *category_item_find_with_color_any_space(const ListBase *list,
                                                                        const char *category)
{
  if (!list || !category_glyph_list_is_valid(list) || !category) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first); item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category) && !is_zero_v3(item->color)) {
      return item;
    }
  }

  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first); item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (is_zero_v3(item->color)) {
      continue;
    }
    if (category_item_match_normalized(item, normalized_target, item->space_type)) {
      return item;
    }
  }

  return nullptr;
}

static const CategoryGlyphItem *category_item_find_with_effective_icon_any_space(
    const ListBase *list, const char *category)
{
  if (!list || !category_glyph_list_is_valid(list) || !category) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first); item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category) && category_item_override_icon_is_effective(item)) {
      return item;
    }
  }

  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first); item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (!category_item_override_icon_is_effective(item)) {
      continue;
    }
    if (category_item_match_normalized(item, normalized_target, item->space_type)) {
      return item;
    }
  }

  return nullptr;
}

static const CategoryGlyphItem *category_item_find_override_with_display_name(
    const ListBase *list, const char *category, int space_type)
{
  if (!list || !category_glyph_list_is_valid(list) || !category) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first); item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (category_item_match_exact(item, category, space_type) && item->display_name[0] != '\0') {
      return item;
    }
  }

  if (space_type == -1) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first); item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (category_item_match_exact(item, category, -1) && item->display_name[0] != '\0') {
      return item;
    }
  }

  return nullptr;
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

#define TABS_PADDING_BETWEEN_FACTOR 4.0f
#define TABS_PADDING_TEXT_FACTOR 6.0f
#define TABS_GLYPH_TEXT_GAP_FACTOR 6.0f
#define TABS_SETTINGS_ICON "\ue5d3" /* Material Symbols: settings/gear */
/* Glyph darkening factor for inactive tabs (0.0 = no change, 1.0 = black). */
#define TABS_GLYPH_DARKEN_BASE 0.15f
/* Built-in icon scale in tab content draw (70% of glyph/text-derived base size). */
#define TABS_BUILTIN_ICON_SCALE 1.0f

/* Enable experimental visual outline for active tabs when visual effect is active. */
#define CATEGORY_TAB_VISUAL_ACTIVE_OUTLINE_ENABLE 1

/* Tab background brightening factors for inactive tabs (0.0 = no change, 1.0 = white). */
#define TABS_BG_BRIGHTEN_BASE 0.0f
#define TABS_BG_BRIGHTEN_HOVER 0.05f

/* Extension drop preview ghost/offset height (in pixels). */
#define EXTENSION_DROP_GHOST_HEIGHT 20

/* Use UI_UI_TABS_VISUAL_EFFECT_SCALE from UI_interface_c.hh */

/** \} */

int category_tabs_vertical_padding_calc(float zoom)
{
  return int(std::floor(TABS_PADDING_BETWEEN_FACTOR * UI_SCALE_FAC * zoom));
}

/* -------------------------------------------------------------------- */
/** \name Pending Category Insert (Extension Drop)
 * \{ */

struct PendingCategoryInsert {
  std::string tag_key;
  std::string target_category;
  std::string anchor_before;
  std::string anchor_after;
  bool insert_above = true;
  bool valid = false;
  double timestamp = 0.0;
  Set<std::string> existing_categories;
  Set<std::string> all_existing_categories; /* ALL categories from region (not filtered by tag) */
  Vector<std::string> pre_order;
  std::string source_extension_id; /* ID of the extension being dropped (for drag & drop) */
};

static PendingCategoryInsert g_pending_category_insert;

/* Deferred category activation - used to activate a category outside of layout phase.
 * This prevents crashes when extensions load previews in background threads during
 * panel_poll calls triggered by immediate activation. */
struct DeferredCategoryActivation {
  std::string category_id;
  std::string tag_key;
  bool valid = false;
  double timestamp = 0.0;
  double activate_time = 0.0; /* Absolute time when activation becomes safe. */
  int frame_delay = 0;
  bool wait_for_extension_signal = false;
  bool extension_signal_received = false;
  bool discover_new_category = false; /* When true, find and activate new category after extension install */
  int discover_retry_count = 0; /* Retry counter for discover mode */
  /* Extension integration fields */
  std::string source_extension_id; /* ID of the extension that introduced this category */
  int activation_space_type = 0;   /* Space type where the extension was activated */
  uint32_t activation_mode_flag = 0; /* Mode flags where the extension was activated */
  bool tag_already_assigned = false; /* True if a tag was already assigned via drag & drop */
  std::string tag_name_to_assign;   /* Tag name to assign when category appears (for deferred tag assignment) */
  /* Pending insert position - copied from g_pending_category_insert before it's cleared */
  bool pending_insert_valid = false;
  std::string pending_insert_tag_key;      /* Full key like "VIEW3D:AAA" for JSON order */
  std::string pending_insert_anchor_before;
  std::string pending_insert_anchor_after;
  std::string pending_insert_target_category;
  bool pending_insert_insert_above = true;
};

static DeferredCategoryActivation g_deferred_category_activation;

/* Known categories before extension drop - used to detect new categories */
static Set<std::string> g_known_categories_before_extension_drop;

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

static void category_tabs_tag_refresh_active_area_ui(const bContext *C)
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

  g_pending_category_insert.tag_key = tag_key;
  g_pending_category_insert.target_category = target_category;
  g_pending_category_insert.anchor_before.clear();
  g_pending_category_insert.anchor_after.clear();
  g_pending_category_insert.insert_above = insert_above;
  g_pending_category_insert.valid = true;
  g_pending_category_insert.timestamp = BLI_time_now_seconds();
  g_pending_category_insert.existing_categories.clear();
  g_pending_category_insert.pre_order = json_order;

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
      g_pending_category_insert.anchor_after = target_category;
      if (target_index > 0) {
        const PanelCategoryDyn *prev = ordered_categories[target_index - 1];
        if (prev && prev->idname[0] != '\0') {
          g_pending_category_insert.anchor_before = prev->idname;
        }
      }
    }
    else {
      g_pending_category_insert.anchor_before = target_category;
      if (target_index + 1 < ordered_categories.size()) {
        const PanelCategoryDyn *next = ordered_categories[target_index + 1];
        if (next && next->idname[0] != '\0') {
          g_pending_category_insert.anchor_after = next->idname;
        }
      }
    }
  }

  for (const PanelCategoryDyn *pc_dyn : ordered_categories) {
    if (pc_dyn && pc_dyn->idname[0] != '\0') {
      g_pending_category_insert.existing_categories.add(std::string(pc_dyn->idname));
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
static void register_new_extension_category(const bContext *C,
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
  /* Escape category_id and extension_id for safe embedding in a Python string literal. */
  char esc_cat[512];
  {
    int j = 0;
    for (int i = 0; category_id[i] != '\0' && j < int(sizeof(esc_cat)) - 1; i++) {
      const char c = category_id[i];
      if (c == '\\' || c == '\'') {
        if (j + 1 < int(sizeof(esc_cat)) - 1) {
          esc_cat[j++] = '\\';
          esc_cat[j++] = c;
        }
      }
      else {
        esc_cat[j++] = c;
      }
    }
    esc_cat[j] = '\0';
  }

  char esc_ext[512];
  {
    int j = 0;
    const char *src = extension_id ? extension_id : "";
    for (int i = 0; src[i] != '\0' && j < int(sizeof(esc_ext)) - 1; i++) {
      const char c = src[i];
      if (c == '\\' || c == '\'') {
        if (j + 1 < int(sizeof(esc_ext)) - 1) {
          esc_ext[j++] = '\\';
          esc_ext[j++] = c;
        }
      }
      else {
        esc_ext[j++] = c;
      }
    }
    esc_ext[j] = '\0';
  }

  /* Store extension ID for deferred activation (used for reserved-only extension detection) */
  g_deferred_category_activation.source_extension_id = extension_id;

  if (!tag_already_assigned) {
    /* Mark the category as pending tag assignment via Python. */
    char python_expr[1280];
    SNPRINTF(python_expr,
             "__import__('bl_ui.space_userpref', fromlist=[''])."
             "mark_category_from_extension('%s', '%s', %d, %u)",
             esc_cat,
             esc_ext,
             space_type,
             mode_flag);

    const char *imports_none[] = {nullptr};
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY REGISTER] Executing Python: %s\n", python_expr);
      fflush(stdout);
    }
    BPY_run_string_exec(const_cast<bContext *>(C), imports_none, python_expr);
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY REGISTER] Python execution completed for mark_category_from_extension\n");
      fflush(stdout);
    }
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
      g_known_categories_before_extension_drop.clear();
      g_pending_category_insert.all_existing_categories.clear();
      for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
        if (pc_dyn.idname && pc_dyn.idname[0]) {
          g_known_categories_before_extension_drop.add(pc_dyn.idname);
          g_pending_category_insert.all_existing_categories.add(pc_dyn.idname);
        }
      }
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[KNOWN_CATS] Saving known categories before viewport extension drop:\n");
        for (const std::string &known_category : g_known_categories_before_extension_drop) {
          printf("[KNOWN_CATS]   + '%s'\n", known_category.c_str());
        }
        printf("[KNOWN_CATS] Total: %zu categories saved\n",
               g_known_categories_before_extension_drop.size());
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
  
  g_known_categories_before_extension_drop.clear();
  g_pending_category_insert.all_existing_categories.clear();
  if (region && region->runtime) {
    for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
      if (pc_dyn.idname && pc_dyn.idname[0]) {
        g_known_categories_before_extension_drop.add(pc_dyn.idname);
        g_pending_category_insert.all_existing_categories.add(pc_dyn.idname);
      }
    }
  }
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[VIEWPORT DROP DEFERRED] Saving known categories from region=%p:\n", (void*)region);
    if (g_known_categories_before_extension_drop.is_empty()) {
      printf("[VIEWPORT DROP DEFERRED]   (no categories found - region may not have panels_category yet)\n");
    }
    for (const std::string &known_category : g_known_categories_before_extension_drop) {
      printf("[VIEWPORT DROP DEFERRED]   + '%s'\n", known_category.c_str());
    }
    printf("[VIEWPORT DROP DEFERRED] Total: %zu categories saved\n",
           g_known_categories_before_extension_drop.size());
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
/** \name Enums
 * \{ */

enum eCategoryGlyphBaseSource {
  CATEGORY_GLYPH_BASE_SOURCE_MAPPING,
  CATEGORY_GLYPH_BASE_SOURCE_PANEL_TYPE,
  CATEGORY_GLYPH_BASE_SOURCE_FALLBACK,
};

struct CategoryTabIconResolved {
  int source = CATEGORY_TAB_ICON_SOURCE_AUTO;
  const char *key = nullptr;
  const char *path = nullptr;
  const char *provider = nullptr;
};

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
 * Equivalent to Python's _normalize_category_key: removes non-alphanumeric chars and converts to lowercase.
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
      result += (*c + ('a' - 'A')); // Convert to lowercase
    }
    // Skip all other characters (spaces, punctuation, etc.)
  }
  
  return result;
}

/**
 * Find category glyph mapping item with canonicalization fallback.
 * First tries exact match, then tries to match by normalized keys if no exact match found.
 */
static const CategoryGlyphItem *category_glyph_mapping_find(const wmWindowManager *wm,
                                                            const char *category,
                                                            int space_type = -1);
static const char *panel_category_glyph_lookup_apply_fallback(const wmWindowManager *wm,
                                                              const char *category,
                                                              bool *r_is_fallback_letter,
                                                              int space_type);

static const CategoryGlyphItem *category_glyph_mapping_find(const wmWindowManager *wm,
                                                             const char *category,
                                                             int space_type)
{
  return category_item_find_mappings(wm, category, space_type);
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

  // First check overrides with priority order: exact -> global -> normalized exact -> normalized global
  if (const CategoryGlyphItem *item = category_item_find_overrides(wm, category, space_type)) {
    return item->tags;
  }

  // Then check mappings with same priority order
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
      active_tags = v3d->active_tag_filter_tags;
    }
  }
  else if (area->spacetype == SPACE_PROPERTIES) {
    const SpaceProperties *sbuts = static_cast<const SpaceProperties *>(area->spacedata.first);
    if (sbuts) {
      active_tags = sbuts->active_tag_filter_tags;
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
/** \name Glyph Lookup Functions
 * \{ */

/* Forward declaration for glyph source lookup. */
static const char *panel_category_display_name_lookup(const wmWindowManager *wm,
                                                      const char *category,
                                                      int space_type);
static const char *panel_category_base_source_lookup(const wmWindowManager *wm,
                                                     const char *category,
                                                     const PanelType *panel_type,
                                                     bool *r_is_reserved,
                                                     eCategoryGlyphBaseSource *r_source_type);

/**
 * Internal helper: Process a single CategoryGlyphItem from overrides.
 * Handles glyph_mode, fallback letter detection, and color extraction.
 */
static const char *process_override_glyph_item(const CategoryGlyphItem *item,
                                               const char *category,
                                               bool *r_is_fallback_letter,
                                               float r_color[3],
                                               bool *r_handled,
                                               const wmWindowManager *wm,
                                               int space_type)
{
  if (item->glyph_mode == CATEGORY_TAB_GLYPH_MODE_FIRST_LETTER) {
    if (r_color && !is_zero_v3(item->color)) {
      copy_v3_v3(r_color, item->color);
    }
    if (r_is_fallback_letter) {
      *r_is_fallback_letter = true;
    }
    *r_handled = true;
    return nullptr;
  }

  if (item->glyph[0] != '\0') {
    /* For glyph_only categories (category name is a glyph), skip fallback letter check.
     * The glyph equals the category name, which would incorrectly be detected as fallback. */
    const bool is_glyph_only_category = category_name_is_glyph(category);
    const bool is_fallback_letter = is_glyph_only_category ?
                                        false :
                                        category_tab_glyph_is_fallback_letter(item->glyph, category);

    if (is_fallback_letter) {
      if (r_color && !is_zero_v3(item->color)) {
        copy_v3_v3(r_color, item->color);
      }
      /* Empty override used for tags only, keep searching mapping/defaults. */
      if (is_zero_v3(item->color) && item->display_name[0] == '\0') {
        return nullptr; /* Continue searching */
      }
      if (r_is_fallback_letter) {
        *r_is_fallback_letter = true;
      }
      *r_handled = true;
      return nullptr;
    }

    if (r_color && !is_zero_v3(item->color)) {
      copy_v3_v3(r_color, item->color);
    }
    *r_handled = true;
    return item->glyph;
  }

  /* Override has no glyph: may be explicit clear OR tags/color-only carrier.
   * For text_only/glyph_text categories (default_glyph is empty), this means reset to first letter.
   * For glyph_only categories (default_glyph is set), continue to mappings to get default.
   *
   * Important for glyph-id categories (normalized key can be empty): without this,
   * color-only overrides are lost in live preview until full restart/resync.
   */
  if (r_color && !is_zero_v3(item->color)) {
    copy_v3_v3(r_color, item->color);
  }

  /* Check if this is a text_only/glyph_text category by looking at mappings.
   * If default_glyph is empty, reset should return first letter (nullptr), not mapping glyph. */
  if (const CategoryGlyphItem *map_item = category_glyph_mapping_find(wm, category, space_type)) {
    /* Treat as true text-only only when both glyph and default_glyph are empty. */
    if (map_item->default_glyph[0] == '\0' && map_item->glyph[0] == '\0') {
      /* Text-based category with empty override glyph = reset to first letter.
       * Set handled=true to prevent fallback to mappings which would return old glyph.
       * Set is_fallback_letter=true so draw code knows to use first letter. */
      if (r_is_fallback_letter) {
        *r_is_fallback_letter = true;
      }
      *r_handled = true;
      return nullptr;
    }
  }

  return nullptr; /* Continue to mappings */
}

static const char *panel_category_glyph_lookup_override(const wmWindowManager *wm,
                                                         const char *category,
                                                         bool *r_is_fallback_letter,
                                                         float r_color[3],
                                                         bool *r_handled,
                                                         int space_type)
{
  *r_handled = false;

  if (!wm) {
    return nullptr;
  }

  // First try exact match, then normalized match using unified helper
  if (const CategoryGlyphItem *item = category_item_find_overrides(wm, category, space_type))
  {
    const char *result = process_override_glyph_item(
        item, category, r_is_fallback_letter, r_color, r_handled, wm, space_type);
    if (*r_handled || result) {
      return result;
    }
    /* Continue searching if process_override_glyph_item returned nullptr without setting handled */
  }

  return nullptr;
}

static const char *panel_category_glyph_lookup_mapping(const wmWindowManager *wm,
                                                       const char *category,
                                                       bool *r_is_fallback_letter,
                                                       float r_color[3],
                                                       bool *r_handled,
                                                       int space_type)
{
  *r_handled = false;

  const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category, space_type);
  if (!item) {
    return nullptr;
  }

  if (r_color && is_zero_v3(r_color) && !is_zero_v3(item->color)) {
    copy_v3_v3(r_color, item->color);
  }

  if (item->glyph_mode == CATEGORY_TAB_GLYPH_MODE_FIRST_LETTER) {
    if (r_is_fallback_letter) {
      *r_is_fallback_letter = true;
    }
    *r_handled = true;
    static char cached_letter[8];
    if (item->first_letter[0] != '\0') {
      STRNCPY(cached_letter, item->first_letter);
      return cached_letter;
    }
    return panel_category_glyph_lookup_apply_fallback(wm, category, r_is_fallback_letter, space_type);
  }

  /* For glyph_only categories (category name is a glyph), skip fallback letter check.
   * The glyph equals the category name, which would incorrectly be detected as fallback. */
  const bool is_glyph_only_category = category_name_is_glyph(category);

  if (item->glyph[0] != '\0') {
    if (is_glyph_only_category || !category_tab_glyph_is_fallback_letter(item->glyph, category)) {
      *r_handled = true;
      return item->glyph;
    }
  }

  if (item->default_glyph[0] != '\0') {
    if (is_glyph_only_category || !category_tab_glyph_is_fallback_letter(item->default_glyph, category)) {
      *r_handled = true;
      return item->default_glyph;
    }
  }

  return nullptr;
}

static const char *panel_category_glyph_lookup_apply_fallback(const wmWindowManager *wm,
                                                              const char *category,
                                                              bool *r_is_fallback_letter,
                                                              int space_type)
{
  if (r_is_fallback_letter) {
    /* If category itself is a glyph, don't treat it as fallback letter. */
    *r_is_fallback_letter = !category_name_is_glyph(category);
  }

  static char first_char_buf[8];
  const char *first_letter_source = panel_category_display_name_lookup(wm, category, space_type);
  if (!first_letter_source || first_letter_source[0] == '\0') {
    first_letter_source = category;
  }

  if (category_tab_first_utf8_char_copy(first_letter_source, first_char_buf, sizeof(first_char_buf))) {
    return first_char_buf;
  }
  return first_letter_source;
}

const char *panel_category_glyph_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        const PanelType *panel_type,
                                        bool *r_is_fallback_letter,
                                        float r_color[3],
                                        int space_type)
{
  /* Initialize outputs. */
  if (r_is_fallback_letter) {
    *r_is_fallback_letter = false;
  }
  /* Initialize color to black (use theme). */
  if (r_color) {
    zero_v3(r_color);
  }

  bool handled = false;
  if (const char *override_glyph = panel_category_glyph_lookup_override(
          wm, category, r_is_fallback_letter, r_color, &handled, space_type))
  {
    return override_glyph;
  }
  if (handled) {
    return nullptr;
  }

  if (const char *mapping_glyph = panel_category_glyph_lookup_mapping(
          wm, category, r_is_fallback_letter, r_color, &handled, space_type))
  {
    return mapping_glyph;
  }
  if (handled) {
    return nullptr;
  }

  /* 3. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    if (r_color && is_zero_v3(r_color)) {
      panel_category_color_lookup(wm, category, r_color);
    }
    return panel_type->icon_glyph;
  }

  return panel_category_glyph_lookup_apply_fallback(wm, category, r_is_fallback_letter, space_type);
}

bool panel_category_first_letter_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        int space_type,
                                        char r_letter[8])
{
  if (!wm || !r_letter) {
    return false;
  }

  r_letter[0] = '\0';
  const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category, space_type);
  if (!item || item->first_letter[0] == '\0') {
    return false;
  }

  BLI_strncpy(r_letter, item->first_letter, 8);
  return true;
}

static void panel_category_color_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        float r_color[3])
{
  if (!r_color) {
    return;
  }

  zero_v3(r_color);

  if (!wm || !category) {
    return;
  }

  if (const CategoryGlyphItem *item = category_item_find_with_color_any_space(
          &wm->category_glyph_overrides, category))
  {
    copy_v3_v3(r_color, item->color);
    return;
  }

  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category)) {
    if (!is_zero_v3(item->color)) {
      copy_v3_v3(r_color, item->color);
    }
  }
}

static bool panel_category_icon_data_lookup(const wmWindowManager *wm,
                                            const char *category,
                                            CategoryTabIconResolved *r_icon,
                                            int space_type = -1)
{
  if (!r_icon) {
    return false;
  }

  *r_icon = CategoryTabIconResolved{};

  const auto icon_data_copy_from_item = [&](const CategoryGlyphItem *item) {
    r_icon->source = item->icon_source;
    r_icon->key = item->icon_key;
    r_icon->path = item->icon_path;
    r_icon->provider = item->icon_provider;
  };

  /* 1) User overrides. */
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_with_effective_icon_any_space(
            &wm->category_glyph_overrides, category))
    {
      icon_data_copy_from_item(item);
      return true;
    }
  }

  /* 2) Global mappings from Python cache sync. Use space_type for proper space-specific lookup. */
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category, space_type)) {
    icon_data_copy_from_item(item);
    return true;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Base Source Lookup
 * \{ */

static const char *panel_category_base_source_lookup(const wmWindowManager *wm,
                                                     const char *category,
                                                     const PanelType *panel_type,
                                                     bool *r_is_reserved,
                                                     eCategoryGlyphBaseSource *r_source_type)
{
  if (r_is_reserved) {
    *r_is_reserved = false;
  }
  if (r_source_type) {
    *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_FALLBACK;
  }

  /* 1. Check global mappings (synced from Python DEFAULT_CATEGORY_GLYPHS). */
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category)) {
    if (r_is_reserved) {
      *r_is_reserved = (item->is_reserved != 0);
    }
    if (r_source_type) {
      *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_MAPPING;
    }
    if (item->glyph[0] != '\0') {
      return item->glyph;
    }
    if (item->default_glyph[0] != '\0') {
      return item->default_glyph;
    }
  }

  /* 2. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    if (r_source_type) {
      *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_PANEL_TYPE;
    }
    return panel_type->icon_glyph;
  }

  /* 3. Fallback: return first character of category. */
  if (r_source_type) {
    *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_FALLBACK;
  }
  static char first_char_buf[8];
  if (category_tab_first_utf8_char_copy(category, first_char_buf, sizeof(first_char_buf))) {
    return first_char_buf;
  }
  return category;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Display Name Lookup
 * \{ */

static const char *panel_category_display_name_lookup(const wmWindowManager *wm,
                                                      const char *category,
                                                      int space_type)
{
  if (!category) {
    return "";
  }

  /* 1. Check user overrides first (prefer requested space, then GLOBAL fallback). */
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_override_with_display_name(
            &wm->category_glyph_overrides, category, space_type))
    {
      return item->display_name;
    }
  }

  /* 2. Check mappings with per-space lookup semantics. */
  if (wm) {
    if (const CategoryGlyphItem *mapped = category_glyph_mapping_find(wm, category, space_type)) {
      if (mapped->display_name[0] != '\0') {
        return mapped->display_name;
      }
    }
  }

  return category;
}

static const char *category_first_letter_source_name_get(const ARegion *region,
                                                         const wmWindowManager *wm,
                                                         const char *category_id,
                                                         const char *category_id_draw,
                                                         int space_type)
{
  if (category_id_draw && category_id_draw[0] != '\0' && !is_single_glyph_str(category_id_draw)) {
    return category_id_draw;
  }

  if (category_id && is_single_glyph_str(category_id) && region && region->runtime &&
      region->runtime->type)
  {
    /* CRITICAL FIX: When multiple extensions use the same tag-category (e.g., "Animation"),
     * prioritize panels from the specific source_extension over generic panels.
     * This prevents MPFB "Add walk cycle" from being used for all Animation extensions. */
    const char *target_source_extension = nullptr;
    
    /* Try to get source_extension from category mappings for context-aware panel selection */
    if (const CategoryGlyphItem *mapping_item = category_item_find_mappings(wm, category_id, space_type)) {
      if (mapping_item->source_extension[0] != '\0') {
        target_source_extension = mapping_item->source_extension;
      }
    }
    
    /* If no mapping found, try overrides */
    if (!target_source_extension) {
      if (const CategoryGlyphItem *override_item = category_item_find_overrides(wm, category_id, space_type)) {
        if (override_item && override_item->source_extension[0] != '\0') {
          target_source_extension = override_item->source_extension;
        }
      }
    }
    
    /* TODO: First pass - find panel from specific extension context
     * Currently disabled until proper source_extension detection is implemented */
    /* if (target_source_extension) {
      for (const PanelType &pt : region->runtime->type->paneltypes) {
        if (pt.category && STREQ(pt.category, category_id)) {
          // TODO: Add proper source_extension matching logic here
          const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
          if (panel_label && panel_label[0] != '\0') {
            return panel_label;
          }
        }
      }
    } */
    
    /* Second pass: fallback to any panel (original behavior) */
    for (const PanelType &pt : region->runtime->type->paneltypes) {
      if (pt.category && STREQ(pt.category, category_id)) {
        const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
        if (panel_label && panel_label[0] != '\0') {
          return panel_label;
        }
      }
    }
  }

  const char *display_name = panel_category_display_name_lookup(wm, category_id, space_type);
  if (display_name && display_name[0] != '\0' && !is_single_glyph_str(display_name)) {
    return display_name;
  }

  return category_id;
}

const char *panel_category_tooltip_name_get(const ARegion *region,
                                             const wmWindowManager *wm,
                                             const char *category_idname)
{
  /* 1. Check user overrides first. If category is in overrides and has display_name, use it. */
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_exact_any_space(
            &wm->category_glyph_overrides, category_idname))
    {
      return category_item_display_name_or_default_or_category(item, category_idname);
    }
  }

  /* 2. Check global mappings. If category is in mappings and has display_name, use it. */
  bool found_in_mappings = false;
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_exact_any_space(
            &wm->category_glyph_mappings, category_idname))
    {
      found_in_mappings = true;
      return category_item_display_name_or_default_or_category(item, category_idname);
    }
  }

  /* 3. For reserved categories (built-in Blender categories), use category name. */
  if (category_is_reserved(wm, category_idname)) {
    return category_idname;
  }

  /* 4. For categories NOT in mappings (not explicitly configured), look up panel label.
   * This handles special cases like "Script 3" where category name contains only a glyph. */
  if (!found_in_mappings) {
    /* CRITICAL FIX: When multiple extensions use the same tag-category (e.g., "Animation"),
     * prioritize panels from the specific source_extension over generic panels.
     * This prevents MPFB "Add walk cycle" from being used for all Animation extensions. */
    const char *target_source_extension = nullptr;
    
    /* Try to get source_extension from category mappings for context-aware panel selection */
    if (const CategoryGlyphItem *mapping_item = category_item_find_mappings(wm, category_idname, -1)) {
      if (mapping_item->source_extension[0] != '\0') {
        target_source_extension = mapping_item->source_extension;
      }
    }
    
    /* If no mapping found, try overrides */
    if (!target_source_extension) {
      if (const CategoryGlyphItem *override_item = category_item_find_overrides(wm, category_idname, -1)) {
        if (override_item && override_item->source_extension[0] != '\0') {
          target_source_extension = override_item->source_extension;
        }
      }
    }
    
    /* TODO: First pass - find panel from specific extension context
     * Currently disabled until proper source_extension detection is implemented */
    /* if (target_source_extension) {
      for (const PanelType &pt : region->runtime->type->paneltypes) {
        if (pt.category && STREQ(pt.category, category_idname)) {
          // TODO: Add proper source_extension matching logic here
          const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
          if (panel_label && panel_label[0]) {
            return panel_label;
          }
        }
      }
    } */
    
    /* Second pass: fallback to any panel (original behavior) */
    for (const PanelType &pt : region->runtime->type->paneltypes) {
      if (pt.category && STREQ(pt.category, category_idname)) {
        const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
        if (panel_label && panel_label[0]) {
          return panel_label;
        }
      }
    }
  }

  /* 5. Fallback to category name itself */
  return category_idname;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Utilities
 * \{ */

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

static void brighten_color_3ub(uchar color[3], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = uchar(color[i] + (255 - color[i]) * factor);
  }
}

static void darken_color_3ub(uchar color[3], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = uchar(color[i] * (1.0f - factor));
  }
}

static void brighten_color_4fv(float color[4], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = color[i] + (1.0f - color[i]) * factor;
  }
  /* Alpha remains unchanged. */
}

static void apply_glyph_darkening(const int fontid, uchar color[3], const float darken_factor)
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

static int category_tab_icon_id_resolve(const CategoryTabIconResolved &icon_resolved)
{
  if (icon_resolved.source == CATEGORY_TAB_ICON_SOURCE_OFF) {
    return ICON_NONE;
  }
  return category_tab_icon_id_resolve_from_key_path(icon_resolved.key, icon_resolved.path);
}

static void draw_category_tab_builtin_icon(const rcti *rct,
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

static bool category_name_is_glyph(const char *category_id)
{
  if (category_id == nullptr || category_id[0] == '\0') {
    return false;
  }

  /* Check if the category name is a single high Unicode character (glyph). */
  const size_t len = strlen(category_id);

  /* Single UTF-8 character that's a high Unicode glyph */
  if (len <= 4) {
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

    /* Check if codepoint is in Private Use Area */
    if (codepoint >= 0xE000 && codepoint <= 0xF8FF) {
      return true;
    }
  }

  return false;
}

bool category_is_reserved(const wmWindowManager *wm, const char *category_id)
{
  /* Categories with glyph names (high Unicode) are from addons and NOT reserved */
  if (category_name_is_glyph(category_id)) {
    return false;
  }

  /* Single source of truth: Python marks reserved categories in wm.category_glyph_mappings. */
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category_id)) {
    return (item->is_reserved != 0);
  }

  return false;
}

bool category_has_pending_tag_assignment(const wmWindowManager *wm,
                                          const char *category_id,
                                          int space_type)
{
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category_id, space_type)) {
    return (item->pending_tag_assignment != 0);
  }
  return false;
}

static bool category_is_reserved_for_reorder(const wmWindowManager *wm, const char *category_id)
{
  if (category_name_is_glyph(category_id)) {
    return false;
  }

  if (category_is_reserved(wm, category_id)) {
    return true;
  }

  auto reserved_name_fallback = [](const char *idname) {
    static const char *k_reserved_fallback[] = {
        "Item",        "View",      "Edit",        "Tool",     "Asset",   "Options",
        "Animation",   "Texture",   "Mesh",        "Object",   "Scene",   "Render",
        "Node",        "Cache",     "Proxy",       "Metadata",
        // Reserved categories (disabled, kept for future use):
        // "Physics", "World", "Material", "Modifiers", "Particles", "Curve",
        // "Script", "Sound", "Surface", "Volume", "Constraints", "Data",
    };
    for (const char *reserved_id : k_reserved_fallback) {
      if (STREQ(idname, reserved_id)) {
        return true;
      }
    }
    return false;
  };

  /* If reserved markers were not populated yet (or failed to sync), keep reorder protection
   * for known built-in categories. */
  bool has_any_reserved_marker = false;
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (item->is_reserved != 0) {
        has_any_reserved_marker = true;
        break;
      }
    }
  }

  if (!has_any_reserved_marker) {
    return reserved_name_fallback(category_id);
  }

  return false;
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

#ifdef WITH_PYTHON
  if (!space_type_name || space_type_name[0] == '\0') {
    space_type_name = "DEFAULT";
  }

  char escaped_id[256];
  int id_j = 0;
  for (int i = 0; category_id[i] != '\0' && id_j < int(sizeof(escaped_id)) - 1; i++) {
    const char c = category_id[i];
    if (c == '\\' || c == '\'') {
      if (id_j + 1 < int(sizeof(escaped_id)) - 1) {
        escaped_id[id_j++] = '\\';
        escaped_id[id_j++] = c;
      }
    }
    else {
      escaped_id[id_j++] = c;
    }
  }
  escaped_id[id_j] = '\0';

  char escaped_space[128];
  int space_j = 0;
  for (int i = 0; space_type_name[i] != '\0' && space_j < int(sizeof(escaped_space)) - 1; i++) {
    const char c = space_type_name[i];
    if (c == '\\' || c == '\'') {
      if (space_j + 1 < int(sizeof(escaped_space)) - 1) {
        escaped_space[space_j++] = '\\';
        escaped_space[space_j++] = c;
      }
    }
    else {
      escaped_space[space_j++] = c;
    }
  }
  escaped_space[space_j] = '\0';

  char python_expr[640];
  SNPRINTF(python_expr,
           "str(__import__('bl_ui.space_userpref', fromlist=[''])."
           "get_reserved_category_priority('%s', '%s'))",
           escaped_id,
           escaped_space);

  char *result_str = nullptr;
  const char *imports_none[] = {nullptr};
  const bool success = BPY_run_string_as_string(
      const_cast<bContext *>(C), imports_none, python_expr, nullptr, &result_str);
  if (!success || !result_str) {
    return -1;
  }

  const int prio = atoi(result_str);
  MEM_delete(result_str);
  return prio;
#else
  return -1;
#endif
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
        STRNCPY(active_tags, v3d->active_tag_filter_tags);
        filter_enabled = v3d->tag_filter_enabled;
      }
    }
    else if (area->spacetype == SPACE_PROPERTIES) {
      /* Tag bar might also be in Properties (for future use) */
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      if (sbuts) {
        STRNCPY(active_tags, sbuts->active_tag_filter_tags);
        filter_enabled = sbuts->tag_filter_enabled;
      }
    }
    else if (area->spacetype == SPACE_NODE) {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      if (snode) {
        STRNCPY(active_tags, snode->active_tag_filter_tags);
        filter_enabled = snode->tag_filter_enabled;
      }
    }
    else if (area->spacetype == SPACE_IMAGE) {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      if (sima) {
        STRNCPY(active_tags, sima->active_tag_filter_tags);
        filter_enabled = sima->tag_filter_enabled;
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

static int get_category_order_index(const bContext *C, ARegion *region, const char *category_id)
{
  ScrArea *area = CTX_wm_area(C);
  WorkSpace *workspace = CTX_wm_workspace(C);
  if (region == nullptr || workspace == nullptr) {
    return 0;
  }

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
      STRNCPY(active_tags_buffer, v3d->active_tag_filter_tags);
      filter_enabled = v3d->tag_filter_enabled;
      break;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      STRNCPY(active_tags_buffer, sbuts->active_tag_filter_tags);
      filter_enabled = sbuts->tag_filter_enabled;
      break;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      STRNCPY(active_tags_buffer, snode->active_tag_filter_tags);
      filter_enabled = snode->tag_filter_enabled;
      break;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      STRNCPY(active_tags_buffer, sima->active_tag_filter_tags);
      filter_enabled = sima->tag_filter_enabled;
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
static Vector<std::string> load_category_order_from_json(const bContext *C, const char *tag_key)
{
#ifdef WITH_PYTHON
  Vector<std::string> result;

  if (!C) {
    return result;
  }

  const std::string escaped_key = category_tab_escape_for_python_literal(tag_key);

  /* Use json.dumps to convert Python list to JSON string for C++ parsing */
  /* ensure_ascii=False to preserve Unicode characters (not escape them as \uXXXX) */
  const std::string python_expr =
      "json.dumps(__import__('bl_ui.space_userpref', fromlist=['']).get_category_order('" +
      escaped_key + "') or [], ensure_ascii=False)";

  /* Execute Python expression and capture output */
  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};

  /* BPY_run_string_as_string requires non-const context */
  const char *imports_json[] = {"json", nullptr};
  bool success = BPY_run_string_as_string(
      const_cast<bContext *>(C),
      imports_json,
      python_expr.c_str(),
      &err_info,
      &result_str);

  if (!success) {
    if (err_msg) {
      MEM_delete(err_msg);
    }
    return result;
  }

  if (!result_str) {
    return result;
  }

  category_tab_parse_json_string_array_minimal(result_str, result);

  MEM_delete(result_str);
  return result;
#else
  UNUSED_VARS(C, tag_key);
  return Vector<std::string>();
#endif
}

/**
 * Save category order to JSON for a specific tag combination.
 * Calls Python function set_category_order().
 */
static void save_category_order_to_json(const bContext *C,
                                        const char *tag_key,
                                        const Vector<std::string> &order)
{
#ifdef WITH_PYTHON
  if (!C) {
    return;
  }

  /* Build Python list of category IDs */
  std::string python_list = "[";
  for (int i = 0; i < order.size(); i++) {
    if (i > 0) {
      python_list += ", ";
    }
    python_list += "'" + category_tab_escape_for_python_literal(order[i].c_str()) + "'";
  }
  python_list += "]";

  const std::string escaped_key = category_tab_escape_for_python_literal(tag_key);

  const std::string python_cmd = "from bl_ui.space_userpref import set_category_order\n"
                                 "set_category_order('" +
                                 escaped_key + "', " + python_list + ")\n";

  /* BPY_run_string_exec requires non-const context */
  const char *imports_none[] = {nullptr};
  BPY_run_string_exec(const_cast<bContext *>(C), imports_none, python_cmd.c_str());
#else
  UNUSED_VARS(C, tag_key, order);
#endif
}


/* Forward declaration */
Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region);

static void category_order_pending_insert_expired_clear_if_needed(const std::string &tag_key)
{
  if (!(g_pending_category_insert.valid && g_pending_category_insert.tag_key == tag_key)) {
    return;
  }

  const double time_since_pending = BLI_time_now_seconds() - g_pending_category_insert.timestamp;
  if (time_since_pending > 120.0) {
    g_pending_category_insert.valid = false;
    g_pending_category_insert.all_existing_categories.clear();
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
  if (!(g_pending_category_insert.valid && g_pending_category_insert.tag_key == tag_key)) {
    return;
  }

  Vector<PanelCategoryDyn *> appeared_categories;
  if (g_tag_filter_debug_enabled) {
    printf("[GET_ORDERED] all_existing_categories.size()=%zu, existing_categories.size()=%zu, g_known_categories_before_extension_drop.size()=%zu\n",
           g_pending_category_insert.all_existing_categories.size(),
           g_pending_category_insert.existing_categories.size(),
           g_known_categories_before_extension_drop.size());
  }
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    /* IMPORTANT: Do NOT filter new extension categories through panel_category_is_visible_by_tags!
     * New extension categories may not have tags assigned yet, which would cause them to be
     * filtered out when tag filter is active. Extension categories must be discoverable
     * regardless of tag filter state so they can be positioned and activated.
     *
     * Use g_pending_category_insert.all_existing_categories when available (extension drop case),
     * as it contains ALL categories from the region, not just filtered ones.
     * This prevents existing categories without the active tag from being incorrectly
     * detected as "new" when an extension is dropped with an active tag filter.
     *
     * Fallback to g_known_categories_before_extension_drop for backwards compatibility,
     * and finally to existing_categories (filtered) if neither is available. */
    const bool use_full_category_list = !g_pending_category_insert.all_existing_categories.is_empty() ||
                                        !g_known_categories_before_extension_drop.is_empty();
    const bool is_new_category = !g_pending_category_insert.all_existing_categories.is_empty() ?
                                     !g_pending_category_insert.all_existing_categories.contains(
                                         std::string(pc_dyn.idname)) :
                                     (!g_known_categories_before_extension_drop.is_empty() ?
                                          !g_known_categories_before_extension_drop.contains(
                                              std::string(pc_dyn.idname)) :
                                          !g_pending_category_insert.existing_categories.contains(
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

  auto find_index_in_result = [&](const std::string &id) -> int {
    if (id.empty()) {
      return -1;
    }
    for (int i = 0; i < r_result.size(); i++) {
      if (STREQ(r_result[i]->idname, id.c_str())) {
        return i;
      }
    }
    return -1;
  };

  int insert_index = -1;
  if (!g_pending_category_insert.anchor_after.empty()) {
    insert_index = find_index_in_result(g_pending_category_insert.anchor_after);
  }
  if (insert_index == -1 && !g_pending_category_insert.anchor_before.empty()) {
    const int before_index = find_index_in_result(g_pending_category_insert.anchor_before);
    if (before_index != -1) {
      insert_index = before_index + 1;
    }
  }
  if (insert_index == -1) {
    const int target_index = find_index_in_result(g_pending_category_insert.target_category);
    if (target_index != -1) {
      insert_index = g_pending_category_insert.insert_above ? target_index : (target_index + 1);
    }
  }
  if (insert_index == -1) {
    insert_index = r_result.size();
  }
  insert_index = clamp_i(insert_index, 0, r_result.size());

  /* CRITICAL: Recalculate insert index after deletions to prevent vector bounds violation */
  insert_index = clamp_i(insert_index, 0, r_result.size());

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
    pending_order = g_pending_category_insert.pre_order;
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
  if (!g_pending_category_insert.anchor_after.empty()) {
    order_insert_index = find_index_in_order(g_pending_category_insert.anchor_after);
  }
  if (order_insert_index == -1 && !g_pending_category_insert.anchor_before.empty()) {
    const int before_index = find_index_in_order(g_pending_category_insert.anchor_before);
    if (before_index != -1) {
      order_insert_index = before_index + 1;
    }
  }
  if (order_insert_index == -1) {
    const int target_index = find_index_in_order(g_pending_category_insert.target_category);
    if (target_index != -1) {
      order_insert_index = g_pending_category_insert.insert_above ? target_index : (target_index + 1);
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
  if (g_pending_category_insert.valid) {
    g_deferred_category_activation.pending_insert_valid = true;
    g_deferred_category_activation.pending_insert_tag_key = g_pending_category_insert.tag_key;
    g_deferred_category_activation.pending_insert_anchor_before = g_pending_category_insert.anchor_before;
    g_deferred_category_activation.pending_insert_anchor_after = g_pending_category_insert.anchor_after;
    g_deferred_category_activation.pending_insert_target_category =
        g_pending_category_insert.target_category;
    g_deferred_category_activation.pending_insert_insert_above =
        g_pending_category_insert.insert_above;
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ORDER] Copied pending insert to deferred activation: tag_key='%s', anchor_before='%s', anchor_after='%s'\n",
             g_pending_category_insert.tag_key.c_str(),
             g_pending_category_insert.anchor_before.c_str(),
             g_pending_category_insert.anchor_after.c_str());
    }
  }
  else {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ORDER] g_pending_category_insert.valid is FALSE - no position to copy!\n");
    }
  }
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    fflush(stdout);
  }

  g_pending_category_insert.valid = false;
  g_pending_category_insert.all_existing_categories.clear();
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
                                    g_pending_category_insert.source_extension_id.c_str(),
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

static int calculate_insert_index(const bContext *C,
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
      const bool should_log = (now - _tab_drag_last_log_time > 1.0) ||
                              (_tab_drag_last_insert_idx != insert_index);
#if TAB_DRAG_DEBUG_ENABLED
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
  const bool tail_should_log = (tail_now - _tab_drag_tail_last_log_time > 1.0) ||
                               (_tab_drag_tail_last_insert_idx != insert_index);
#if TAB_DRAG_DEBUG_ENABLED
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

static void calculate_drag_insert_bounds(const wmWindowManager *wm,
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

static void update_insert_zone(const bContext *C,
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

static void apply_category_order(bContext *C, ARegion *region, CategoryDragState *state)
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
  g_known_categories_before_extension_drop.clear();
  g_pending_category_insert.all_existing_categories.clear();
  for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (pc_dyn.idname && pc_dyn.idname[0]) {
      g_known_categories_before_extension_drop.add(pc_dyn.idname);
      g_pending_category_insert.all_existing_categories.add(pc_dyn.idname);
    }
  }
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[KNOWN_CATS] Saving known categories before extension drop:\n");
    for (const std::string &known_category : g_known_categories_before_extension_drop) {
      printf("[KNOWN_CATS]   + '%s'\n", known_category.c_str());
    }
    printf("[KNOWN_CATS] Total: %zu categories (also saved to pending_insert)\n",
           g_known_categories_before_extension_drop.size());
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Drawing Functions
 * \{ */

struct CategoryTabsDrawContext {
  const ScrArea *area;
  int space_type;
  bool is_left;
  View2D *v2d;
  const uiStyle *style;
  const uiFontStyle *fstyle;
  int fontid;
  float fstyle_points;
  float aspect;
  eUserPref_CategoryTabsDisplayMode display_mode;
  float category_tabs_zoom;
  float zoom;
  const wmWindowManager *wm;
  int px;
  int category_tabs_width;
  float dpi_fac;
  int category_tabs_min_width;
  bool too_narrow;
  int tab_v_pad_text;
  int tab_v_pad;
};

struct CategoryTabMixedContentFlags {
  int resolved_icon_id;
  bool use_builtin_icon;
  bool mixed_mode_effective_has_glyph;
  bool mixed_mode_effective_fallback_letter;
  bool mixed_mode_effective_builtin_icon;
  bool mixed_mode_has_visible_glyph_content;
};

struct CategoryTabRenderData {
  const char *category_id;
  const char *category_id_draw;
  const char *category_id_draw_label;
};

struct CategoryTabVisualBoxCalcResult {
  rctf box_rect;
  bool is_visual_effect_active;
};

struct CategoryTabToneFactors {
  float darken_factor;
  float bg_brighten_factor;
};

struct CategoryTabNameLayout {
  bool should_expand_name;
  int tab_v_pad_text;
};

struct CategoryTabSingleDrawContext {
  const ARegion *region;
  const wmWindowManager *wm;
  const View2D *v2d;
  int rct_xmin;
  int rct_xmax;
  bool is_left;
  bool is_dragging;
  eUserPref_CategoryTabsDisplayMode display_mode;
  float tab_curve_radius;
  int roundboxtype;
  int px;
  float theme_col_tab_active[4];
  float theme_col_tab_inactive[4];
  float theme_col_tab_outline[4];
  float theme_col_tab_outline_sel[4];
  int fontid;
  const uiFontStyle *fstyle;
  float fstyle_points;
  float zoom;
  float category_tabs_zoom;
  uchar theme_col_tab_text[3];
  uchar theme_col_tab_text_sel[3];
  bool too_narrow;
  int space_type;
};

struct CategoryTabLoopDrawData {
  const char *category_id;
  const char *category_id_draw;
  bool is_active;
  bool is_hover;
  int current_tab_v_pad_text;
  float darken_factor;
  float bg_brighten_factor;
  float glyph_color[3];
};

struct DeferredHoverTabDrawData {
  bool valid = false;
  rcti rct = {};
  const char *category_id = nullptr;
  const char *category_id_draw = nullptr;
  bool is_active = false;
  int current_tab_v_pad_text = 0;
  float darken_factor = 0.0f;
  float bg_brighten_factor = 0.0f;
  float glyph_color[3] = {0.0f, 0.0f, 0.0f};
};

static CategoryTabsDrawContext category_tabs_draw_context_build(const bContext *C, ARegion *region)
{
  CategoryTabsDrawContext ctx{};
  ctx.area = CTX_wm_area(C);
  ctx.space_type = ctx.area ? ctx.area->spacetype : -1;
  ctx.is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  ctx.v2d = &region->v2d;
  ctx.style = style_get();
  ctx.fstyle = &ctx.style->widget;
  ctx.fontid = ctx.fstyle->uifont_id;
  ctx.fstyle_points = ctx.fstyle->points;

  const float raw_aspect = BLI_listbase_is_empty(&region->runtime->uiblocks) ?
                               1.0f :
                               (static_cast<Block *>(region->runtime->uiblocks.first))->aspect;
  /* Stabilize aspect: if very close to 1.0, use exactly 1.0 to avoid floating-point oscillation.
   * This prevents tab size jitter when aspect oscillates between 1.0 and 1.0000001. */
  ctx.aspect = (std::abs(raw_aspect - 1.0f) < 0.001f) ? 1.0f : raw_aspect;
  ctx.display_mode = ED_category_tabs_display_mode_get(ctx.area);
  ctx.category_tabs_zoom = category_tabs_zoom_value_get(ctx.area, ctx.display_mode);
  ctx.zoom = (1.0f / ctx.aspect) * ctx.category_tabs_zoom;
  ctx.wm = CTX_wm_manager(C);
  ctx.px = U.pixelsize;
  ctx.category_tabs_width = round_fl_to_int(UI_PANEL_CATEGORY_MARGIN_WIDTH * ctx.zoom);
  ctx.dpi_fac = UI_SCALE_FAC;
  ctx.category_tabs_min_width = category_tabs_min_width_get(ctx.area, ctx.aspect, ctx.display_mode);
  ctx.too_narrow = BLI_rcti_size_x(&region->winrct) <= ctx.category_tabs_min_width;
  ctx.tab_v_pad_text = int(std::floor(TABS_PADDING_TEXT_FACTOR * ctx.dpi_fac * ctx.zoom)) +
                       2 * ctx.px;
  ctx.tab_v_pad = category_tabs_vertical_padding_calc(ctx.zoom);
  return ctx;
}

void panel_category_tabs_draw_settings_button(const bContext *C,
                                              ARegion *region,
                                              float zoom,
                                              const unsigned char theme_col_tab_text[3])
{
  const uiStyle *style = style_get();
  const uiFontStyle *fstyle = &style->widget;
  const int fontid = fstyle->uifont_id;

  /* Reset font size to standard zoom (without visual effect scale).
   * This is necessary because the visual effect for tabs modifies BLF_size,
   * and we don't want the settings button glyph to be affected. */
  const float fstyle_points = fstyle->points;
  BLF_size(fontid, fstyle_points * UI_SCALE_FAC * zoom);

  const rcti *rct = &region->runtime->category_tabs_settings_rect;

  bool is_hover = region->runtime->category_tabs_settings_hover;
  wmWindow *win = CTX_wm_window(C);

  const double current_time = BLI_time_now_seconds();
  const double time_since_hover = current_time - region->runtime->category_tabs_settings_hover_time;

  const bool drag_active = (region->runtime->category_tabs_drag_state != nullptr);
  const bool drag_pending = (region->runtime->category_tabs_drag_pending_id[0] != '\0');

  const bool hover_timeout_expired = time_since_hover > 2.0;

  if (win && win->runtime->eventstate) {
    const int mx = win->runtime->eventstate->xy[0] - region->winrct.xmin;
    const int my = win->runtime->eventstate->xy[1] - region->winrct.ymin;
    const bool mouse_in_region = BLI_rcti_isect_pt(&region->winrct,
                                                   win->runtime->eventstate->xy[0],
                                                   win->runtime->eventstate->xy[1]);
    const bool actually_over = mouse_in_region && BLI_rcti_isect_pt(rct, mx, my);

    if (drag_active || drag_pending) {
      is_hover = false;
    }
    else if (actually_over) {
      if (!is_hover) {
        region->runtime->category_tabs_settings_hover_time = current_time;
      }
      is_hover = true;
    }
    else if (hover_timeout_expired) {
      is_hover = false;
    }

    region->runtime->category_tabs_settings_hover = is_hover;
  }

  bTheme *btheme = theme::theme_get();
  const float tab_curve_radius = btheme->tui.wcol_tab.roundness * U.widget_unit * zoom;
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  const int roundboxtype = region->overlap ? CNR_ALL :
                                             (is_left ? (CNR_TOP_LEFT | CNR_BOTTOM_LEFT) :
                                                        (CNR_TOP_RIGHT | CNR_BOTTOM_RIGHT));

  float theme_col_tab_bg[4];
  float theme_col_tab_outline[4];
  if (is_hover) {
    theme::get_color_4fv(TH_TAB_ACTIVE, theme_col_tab_bg);
  }
  else {
    theme::get_color_4fv(TH_TAB_INACTIVE, theme_col_tab_bg);
  }
  theme::get_color_4fv(TH_TAB_OUTLINE, theme_col_tab_outline);

  GPU_blend(GPU_BLEND_ALPHA);

  /* Apply visual effect scale when hovering over the settings button itself.
   * This ensures the glyph only scales when the cursor is actually over the button,
   * not when hovering over the last tab. */
  const float visual_effect_scale = is_hover ? UI_TABS_VISUAL_EFFECT_SCALE : 1.0f;
  BLF_size(fontid, fstyle_points * UI_SCALE_FAC * zoom * visual_effect_scale);

  rctf box_rect;
  box_rect.xmin = float(rct->xmin);
  box_rect.xmax = float(rct->xmax);
  box_rect.ymin = float(rct->ymin);
  box_rect.ymax = float(rct->ymax);

  draw_roundbox_corner_set(roundboxtype);
  draw_roundbox_4fv(&box_rect, true, tab_curve_radius, theme_col_tab_bg);
  draw_roundbox_4fv(&box_rect, false, tab_curve_radius, theme_col_tab_outline);

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

  uchar theme_col_text_hi[3];
  theme::get_color_3ubv(TH_TAB_TEXT_HI, theme_col_text_hi);

  BLF_disable(fontid, BLF_ROTATION);
  BLF_position(fontid, pos_x, pos_y, 0.0f);
  BLF_color3ubv(fontid, is_hover ? theme_col_text_hi : theme_col_tab_text);
  BLF_draw(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX);

  GPU_blend(GPU_BLEND_NONE);
}

static const char *category_tab_draw_label_resolve(const ARegion *region,
                                                   const char *category_id,
                                                   const char *category_id_draw)
{
  if (!category_id_draw || category_id_draw[0] == '\0' || !is_single_glyph_str(category_id_draw) ||
      !region || !region->runtime || !region->runtime->type)
  {
    return category_id_draw;
  }

  /* TODO: Extension-aware panel selection - currently simplified to original behavior
   * Extension context detection will be implemented when proper source_extension
   * tracking is available in PanelType structure */
  
  /* Original behavior: find first panel with matching category */
  for (const PanelType &pt : region->runtime->type->paneltypes) {
    if (pt.category && STREQ(pt.category, category_id)) {
      const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
      if (panel_label && panel_label[0] != '\0') {
        return panel_label;
      }
      break;
    }
  }

  return category_id_draw;
}

static CategoryTabRenderData category_tab_render_data_build(const wmWindowManager *wm,
                                                            const ARegion *region,
                                                            const char *category_id,
                                                            const int space_type)
{
  CategoryTabRenderData render_data{};
  render_data.category_id = category_id;
  render_data.category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id, space_type));
  render_data.category_id_draw_label = category_tab_draw_label_resolve(
      region, category_id, render_data.category_id_draw);
  return render_data;
}

static CategoryTabMixedContentFlags category_tab_mixed_content_flags_resolve(
    const wmWindowManager *wm,
    const char *category_id,
    const eUserPref_CategoryTabsDisplayMode display_mode,
    const int space_type,
    const bool has_glyph,
    const bool is_fallback_letter)
{
  CategoryTabMixedContentFlags flags{};

  const bool display_mode_allows_icon_content = ELEM(
      display_mode, USER_CATEGORY_TABS_GLYPHS_ONLY, USER_CATEGORY_TABS_GLYPHS_TEXT);

  CategoryTabIconResolved icon_resolved;
  panel_category_icon_data_lookup(wm, category_id, &icon_resolved, space_type);
  flags.resolved_icon_id = category_tab_icon_id_resolve(icon_resolved);

  bool icon_data_allows_icon_content = (icon_resolved.source != CATEGORY_TAB_ICON_SOURCE_OFF);
  if (icon_resolved.source == CATEGORY_TAB_ICON_SOURCE_MANUAL) {
    icon_data_allows_icon_content = (icon_resolved.key && icon_resolved.key[0] != '\0') ||
                                    (icon_resolved.path && icon_resolved.path[0] != '\0');
  }

  flags.use_builtin_icon =
      display_mode_allows_icon_content && icon_data_allows_icon_content &&
      (flags.resolved_icon_id != ICON_NONE);
  flags.mixed_mode_effective_has_glyph = has_glyph && U.category_tabs_mixed_show_glyphs;
  flags.mixed_mode_effective_fallback_letter =
      is_fallback_letter && U.category_tabs_mixed_show_first_letter;
  flags.mixed_mode_effective_builtin_icon = flags.use_builtin_icon && U.category_tabs_mixed_show_icons;
  flags.mixed_mode_has_visible_glyph_content = flags.mixed_mode_effective_has_glyph ||
                                               flags.mixed_mode_effective_fallback_letter ||
                                               flags.mixed_mode_effective_builtin_icon;

  return flags;
}

static int category_tab_v_pad_text_resolve(const eUserPref_CategoryTabsDisplayMode display_mode,
                                           const int tab_v_pad_text,
                                           const bool should_expand_name)
{
  if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
      U.category_tabs_shape == USER_CATEGORY_TABS_SHAPE_BOX && !should_expand_name)
  {
    return 0;
  }
  return tab_v_pad_text;
}

static CategoryTabToneFactors category_tab_tone_factors_calc(const bool is_active,
                                                             const bool is_hover)
{
  CategoryTabToneFactors tone_factors{};

  if (!is_active && !is_hover) {
    tone_factors.darken_factor = TABS_GLYPH_DARKEN_BASE;
  }

  if (!is_active) {
    tone_factors.bg_brighten_factor = is_hover ? TABS_BG_BRIGHTEN_HOVER : TABS_BG_BRIGHTEN_BASE;
  }

  return tone_factors;
}

static bool category_tab_visual_effect_allowed(const eUserPref_CategoryTabsDisplayMode display_mode,
                                               const bool is_dragging,
                                               const bool is_active)
{
  return (U.category_tabs_visual_effect && display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
          !is_dragging && (!U.category_tabs_show_active_name || !is_active));
}

static CategoryTabNameLayout category_tab_name_layout_resolve(const ARegion *region,
                                                              const char *category_id,
                                                              const eUserPref_CategoryTabsDisplayMode display_mode,
                                                              const bool is_active,
                                                              const bool use_minimized_gate,
                                                              const bool is_panel_minimized,
                                                              const int tab_v_pad_text)
{
  CategoryTabNameLayout name_layout{};
  name_layout.should_expand_name = category_tab_should_expand_name(region,
                                                                   category_id,
                                                                   display_mode,
                                                                   is_active,
                                                                   use_minimized_gate,
                                                                   is_panel_minimized);
  name_layout.tab_v_pad_text = category_tab_v_pad_text_resolve(
      display_mode, tab_v_pad_text, name_layout.should_expand_name);
  return name_layout;
}

static CategoryTabLoopDrawData category_tab_loop_draw_data_build(const wmWindowManager *wm,
                                                                 const ARegion *region,
                                                                 const rcti *rct,
                                                                 const char *category_id_raw,
                                                                 const char *category_id_active,
                                                                 const eUserPref_CategoryTabsDisplayMode display_mode,
                                                                 const int tab_v_pad_text,
                                                                 const bool too_narrow,
                                                                 const int mouse_x,
                                                                 const int mouse_y,
                                                                 const int space_type)
{
  const CategoryTabRenderData render_data = category_tab_render_data_build(
      wm, region, category_id_raw, space_type);

  CategoryTabLoopDrawData draw_data{};
  draw_data.category_id = render_data.category_id;
  draw_data.category_id_draw = render_data.category_id_draw;
  draw_data.is_active =
      !too_narrow && category_id_active && STREQ(draw_data.category_id, category_id_active);

  const CategoryTabNameLayout name_layout = category_tab_name_layout_resolve(
      region, draw_data.category_id, display_mode, draw_data.is_active, true, too_narrow, tab_v_pad_text);
  draw_data.current_tab_v_pad_text = name_layout.tab_v_pad_text;

  bool is_fallback_letter = false;
  panel_category_glyph_lookup(
      wm, draw_data.category_id, nullptr, &is_fallback_letter, draw_data.glyph_color, space_type);
  if (category_tab_edit_dialog_is_open_for_category(draw_data.category_id)) {
    copy_v3_v3(draw_data.glyph_color, category_tab_preview_color);
  }

  draw_data.is_hover = BLI_rcti_isect_pt(rct, mouse_x, mouse_y);

  const CategoryTabToneFactors tone_factors = category_tab_tone_factors_calc(draw_data.is_active,
                                                                              draw_data.is_hover);
  draw_data.darken_factor = tone_factors.darken_factor;
  draw_data.bg_brighten_factor = tone_factors.bg_brighten_factor;

  return draw_data;
}

static int category_tab_y_shift_resolve(const ARegion *region,
                                        const blender::Vector<PanelCategoryDyn *> &ordered_categories,
                                        const PanelCategoryDyn &pc_dyn,
                                        const bool is_dragging,
                                        const CategoryDragState *drag_state,
                                        const int current_display_index,
                                        const int tab_v_pad)
{
  int y_shift = 0;

  if (is_dragging && drag_state && !drag_state->is_reserved) {
    const int insert_idx = drag_state->current_insert_index;
    const int original_idx = drag_state->original_index;

    if (insert_idx > original_idx) {
      if (current_display_index > original_idx && current_display_index <= insert_idx) {
        y_shift = drag_state->drag_tab_height + tab_v_pad;
      }
    }
    else if (insert_idx < original_idx) {
      if (current_display_index >= insert_idx && current_display_index < original_idx) {
        y_shift = -drag_state->drag_tab_height - tab_v_pad;
      }
    }

    return y_shift;
  }

  if (!is_dragging && region->runtime && region->runtime->extension_drop_preview_state.active) {
    const ui::ExtensionDropPreviewState &preview = region->runtime->extension_drop_preview_state;
    bool preview_name_found = false;
    int preview_name_idx = -1;
    if (preview.target_category_id[0] != '\0') {
      int idx = 0;
      for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
        if (STREQ(pc_dyn_ptr->idname, preview.target_category_id)) {
          preview_name_found = true;
          preview_name_idx = idx;
          break;
        }
        idx++;
      }
    }

    const bool preview_fallback_used = false;
    /* Use unified insertion slot model consistently */
    int raw_insertion_index = preview_name_found ?
                                  (preview_name_idx + (preview.insert_above ? 0 : 1)) :
                                  -1;
    /* Prevent creating slots beyond existing tabs */
    const int insertion_index =
        (raw_insertion_index >= 0 && raw_insertion_index > int(ordered_categories.size())) ?
            int(ordered_categories.size()) :
            raw_insertion_index;

    static double _ext_shift_last_log_time = 0.0;
    static int _ext_shift_last_stored_idx = -999;
    static int _ext_shift_last_resolved_idx = -999;
    static bool _ext_shift_last_insert_above = false;
    static bool _ext_shift_last_name_found = false;
    static int _ext_shift_last_raw_insertion_idx = -999;
    static int _ext_shift_last_category_count = -999;
    const double shift_log_time = BLI_time_now_seconds();
    const bool shift_should_log = (shift_log_time - _ext_shift_last_log_time > 1.0) ||
                                  (_ext_shift_last_stored_idx != preview.target_index) ||
                                  (_ext_shift_last_resolved_idx != preview_name_idx) ||
                                  (_ext_shift_last_insert_above != preview.insert_above) ||
                                  (_ext_shift_last_name_found != preview_name_found) ||
                                  (_ext_shift_last_raw_insertion_idx != raw_insertion_index) ||
                                  (_ext_shift_last_category_count != int(ordered_categories.size()));
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (shift_should_log) {
        const int category_count = int(ordered_categories.size());
        const int max_valid_index = category_count - 1;
        printf("[EXT_SHIFT] target='%s' stored_idx=%d name_found=%d name_idx=%d "
               "insert_above=%d raw_insertion_idx=%d insertion_idx=%d category_count=%d max_valid_idx=%d shift=%d\n",
               preview.target_category_id,
               preview.target_index,
               preview_name_found ? 1 : 0,
               preview_name_idx,
               preview.insert_above ? 1 : 0,
               raw_insertion_index,
               insertion_index,
               category_count,
               max_valid_index,
               EXTENSION_DROP_GHOST_HEIGHT + preview.tab_v_pad);
        if (raw_insertion_index >= category_count && category_count > 0) {
          printf("[EXT_SHIFT] boundary: raw_insertion_idx >= category_count (end-insert path)\n");
        }
        if (!preview_name_found && preview.target_category_id[0] != '\0') {
          printf("[EXT_SHIFT] resolve_warning: target name not found, stored_idx=%d fallback_used=%d\n",
                 preview.target_index,
                 preview_fallback_used ? 1 : 0);
        }
        _ext_shift_last_log_time = shift_log_time;
        _ext_shift_last_stored_idx = preview.target_index;
        _ext_shift_last_resolved_idx = preview_name_idx;
        _ext_shift_last_insert_above = preview.insert_above;
        _ext_shift_last_name_found = preview_name_found;
        _ext_shift_last_raw_insertion_idx = raw_insertion_index;
        _ext_shift_last_category_count = int(ordered_categories.size());
      }
    }

    /* UNIFIED INSERTION SLOT MODEL:
     * insertion_index = target_index + (insert_above ? 0 : 1)
     * Shift tabs >= insertion_index down to create space for new tab.
     * This creates a consistent slot between tabs at insertion_index-1 and insertion_index. */
    if (insertion_index >= 0 && current_display_index >= insertion_index) {
      /* Always shift DOWN to create space above the insertion point.
       * Use negative y_shift because in Blender's coordinate system:
       * - Positive Y values are at the top of screen
       * - Adding positive value shifts rect UP, negative shifts DOWN */
      y_shift = -(EXTENSION_DROP_GHOST_HEIGHT + preview.tab_v_pad);
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        if (shift_should_log) {
          printf("[EXT_SHIFT] apply: tab='%s' display_idx=%d >= insertion_idx=%d y_shift=%d\n",
                 pc_dyn.idname,
                 current_display_index,
                 insertion_index,
                 y_shift);
        }
      }
    }
  }

  return y_shift;
}

static bool category_tab_should_expand_name(const ARegion *region,
                                            const char *category_id,
                                            const eUserPref_CategoryTabsDisplayMode display_mode,
                                            const bool is_active,
                                            const bool use_minimized_gate,
                                            const bool is_panel_minimized)
{
  const bool is_sticky_inactive = (U.category_tabs_inactive_behavior ==
                                   USER_CATEGORY_TABS_INACTIVE_STICKY);
  const bool is_sticky_mode = (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
                               U.category_tabs_show_active_name);
  const bool can_show_previous = is_sticky_inactive && is_sticky_mode;
  const bool is_previous_active_raw = (region->runtime->category_tabs_previous_active_id[0] != '\0' &&
                                       STREQ(category_id,
                                             region->runtime->category_tabs_previous_active_id));
  const bool is_previous_active = use_minimized_gate ?
                                      (can_show_previous && is_panel_minimized && is_previous_active_raw) :
                                      (can_show_previous && is_previous_active_raw);

  return ((is_sticky_mode || is_active) && U.category_tabs_show_active_name &&
          !region->runtime->category_tabs_active_name_hidden && (is_active || is_previous_active));
}

static CategoryTabVisualBoxCalcResult category_tab_visual_box_calc(const rcti *rct,
                                                                   const View2D *v2d,
                                                                   const bool is_left,
                                                                   const bool is_active,
                                                                   const bool is_hover,
                                                                   const bool visual_effect_allowed_for_tab)
{
  CategoryTabVisualBoxCalcResult result{};
  result.is_visual_effect_active = false;
  result.box_rect.xmin = float(rct->xmin);
  result.box_rect.xmax = float(rct->xmax);
  result.box_rect.ymin = float(rct->ymin);
  result.box_rect.ymax = float(rct->ymax);

  if (visual_effect_allowed_for_tab && (is_active || is_hover)) {
    result.is_visual_effect_active = true;

    const int tab_height = rct->ymax - rct->ymin;
    const int expanded_height = round_fl_to_int(tab_height * UI_TABS_VISUAL_EFFECT_SCALE);
    const int extra_height = expanded_height - tab_height;

    int extra_top = extra_height / 2;
    int extra_bottom = extra_height - extra_top;

    const int available_top = std::max(v2d->mask.ymax - rct->ymax, 0);
    const int available_bottom = std::max(rct->ymin - v2d->mask.ymin, 0);
    extra_top = std::min(extra_top, available_top);
    extra_bottom = std::min(extra_bottom, available_bottom);

    result.box_rect.ymin -= extra_bottom;
    result.box_rect.ymax += extra_top;

    const int tab_width = rct->xmax - rct->xmin;
    const int expanded_width = round_fl_to_int(tab_width * UI_TABS_VISUAL_EFFECT_SCALE);
    const int extra_width = expanded_width - tab_width;
    const int available_extra_width = is_left ? std::max(v2d->mask.xmax - rct->xmax, 0) :
                                               std::max(rct->xmin - v2d->mask.xmin, 0);
    const int applied_extra_width = std::min(extra_width, available_extra_width);

    if (is_left) {
      result.box_rect.xmax += applied_extra_width;
    }
    else {
      result.box_rect.xmin -= applied_extra_width;
    }
  }

  return result;
}

static void draw_category_tab_color_indicator(const rcti *rct,
                                              const float glyph_color[3],
                                              bool is_left,
                                              eUserPref_CategoryTabsDisplayMode display_mode,
                                              bool show_color_indicator,
                                              bool is_active_tab);
static void ui_panel_category_draw_content(const ARegion *region,
                                           const wmWindowManager *wm,
                                           const char *category_id,
                                           const char *category_id_draw,
                                           const rcti *rct,
                                           int rct_xmin,
                                           int rct_xmax,
                                           bool is_active,
                                           bool is_left,
                                           eUserPref_CategoryTabsDisplayMode display_mode,
                                           int fontid,
                                           const uiFontStyle *fstyle,
                                           float fstyle_points,
                                           float zoom,
                                           float category_tabs_zoom,
                                           int tab_v_pad_text,
                                           float darken_factor,
                                           const uchar theme_col_tab_text[3],
                                           const uchar theme_col_tab_text_sel[3],
                                           bool is_panel_minimized,
                                           int space_type);

static void category_tab_draw_background_and_border(const ARegion *region,
                                                    const rcti *rct,
                                                    const rctf &box_rect,
                                                    const bool is_visual_effect_active,
                                                    const bool is_active,
                                                    const bool is_left,
                                                    const eUserPref_CategoryTabsDisplayMode display_mode,
                                                    const float bg_brighten_factor,
                                                    const float glyph_color[3],
                                                    const float tab_curve_radius,
                                                    const int roundboxtype,
                                                    const int px,
                                                    const float theme_col_tab_active[4],
                                                    const float theme_col_tab_inactive[4],
                                                    const float theme_col_tab_outline[4],
                                                    const float theme_col_tab_outline_sel[4])
{
  draw_roundbox_corner_set(roundboxtype);

  float tab_bg_color[4];
  if (is_active) {
    copy_v4_v4(tab_bg_color, theme_col_tab_active);
  }
  else {
    copy_v4_v4(tab_bg_color, theme_col_tab_inactive);
    brighten_color_4fv(tab_bg_color, bg_brighten_factor);
  }

  draw_roundbox_4fv(&box_rect, true, tab_curve_radius, tab_bg_color);
  draw_roundbox_4fv(
      &box_rect, false, tab_curve_radius, is_active ? theme_col_tab_outline_sel : theme_col_tab_outline);

#if CATEGORY_TAB_VISUAL_ACTIVE_OUTLINE_ENABLE
  const bool is_visual_active_outline = is_visual_effect_active && is_active &&
                                        U.category_tabs_visual_outline;
  if (is_visual_active_outline) {
    float visual_outline_color[4];
    rgba_uchar_to_float(visual_outline_color, U.category_tabs_visual_outline_color);
    const float visual_outline_width = float(px) + 0.5f;
    draw_roundbox_4fv_ex(&box_rect,
                         nullptr,
                         nullptr,
                         1.0f,
                         visual_outline_color,
                         visual_outline_width,
                         tab_curve_radius);
  }
#endif

  /* Draw color indicator bar for TEXT_ONLY mode to show assigned glyph color. */
  draw_category_tab_color_indicator(
      rct, glyph_color, is_left, display_mode, U.category_tabs_text_mode_show_color_indicator, is_active);

  if (!region->overlap) {
    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    immUniformColor4fv(tab_bg_color);
    /* Use expanded box_rect when visual effect is active, otherwise use standard rct */
    if (is_visual_effect_active) {
      immRectf(pos,
               is_left ? box_rect.xmax - px : box_rect.xmin,
               box_rect.ymin + px,
               is_left ? box_rect.xmax : box_rect.xmin + px,
               box_rect.ymax - px);
    }
    else {
      immRectf(pos,
               is_left ? rct->xmax - px : rct->xmin,
               rct->ymin + px,
               is_left ? rct->xmax : rct->xmin + px,
               rct->ymax - px);
    }
    immUnbindProgram();
  }
}

static void category_tab_draw_content_layer(const ARegion *region,
                                            const wmWindowManager *wm,
                                            const char *category_id,
                                            const char *category_id_draw,
                                            const rcti *rct,
                                            const rctf &box_rect,
                                            const bool is_visual_effect_active,
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
                                            const int current_tab_v_pad_text,
                                            const float darken_factor,
                                            const uchar theme_col_tab_text[3],
                                            const uchar theme_col_tab_text_sel[3],
                                            const bool too_narrow,
                                            const int space_type)
{
  rcti expanded_rct;
  const rcti *content_rct;
  if (is_visual_effect_active) {
    expanded_rct.xmin = int(box_rect.xmin);
    expanded_rct.xmax = int(box_rect.xmax);
    expanded_rct.ymin = int(box_rect.ymin);
    expanded_rct.ymax = int(box_rect.ymax);
    content_rct = &expanded_rct;
  }
  else {
    content_rct = rct;
  }

  float current_category_tabs_zoom = category_tabs_zoom;
  if (is_visual_effect_active) {
    current_category_tabs_zoom *= UI_TABS_VISUAL_EFFECT_SCALE;
  }

  ui_panel_category_draw_content(region,
                                 wm,
                                 category_id,
                                 category_id_draw,
                                 content_rct,
                                 rct_xmin,
                                 rct_xmax,
                                 is_active,
                                 is_left,
                                 display_mode,
                                 fontid,
                                 fstyle,
                                 fstyle_points,
                                 zoom,
                                 current_category_tabs_zoom,
                                 current_tab_v_pad_text,
                                 darken_factor,
                                 theme_col_tab_text,
                                 theme_col_tab_text_sel,
                                  too_narrow,
                                  space_type);
}

static void category_tab_draw_single(const CategoryTabSingleDrawContext &ctx,
                                     const rcti *rct,
                                     const char *category_id,
                                     const char *category_id_draw,
                                     const bool is_active,
                                     const bool is_hover,
                                     const int current_tab_v_pad_text,
                                     const float darken_factor,
                                     const float bg_brighten_factor,
                                     const float glyph_color[3])
{
  GPU_blend(GPU_BLEND_ALPHA);

  const bool visual_effect_allowed_for_tab = category_tab_visual_effect_allowed(
      ctx.display_mode, ctx.is_dragging, is_active);
  const CategoryTabVisualBoxCalcResult visual_box = category_tab_visual_box_calc(
      rct, ctx.v2d, ctx.is_left, is_active, is_hover, visual_effect_allowed_for_tab);
  const bool is_visual_effect_active = visual_box.is_visual_effect_active;
  const rctf &box_rect = visual_box.box_rect;

  category_tab_draw_background_and_border(ctx.region,
                                          rct,
                                          box_rect,
                                          is_visual_effect_active,
                                          is_active,
                                          ctx.is_left,
                                          ctx.display_mode,
                                          bg_brighten_factor,
                                          glyph_color,
                                          ctx.tab_curve_radius,
                                          ctx.roundboxtype,
                                          ctx.px,
                                          ctx.theme_col_tab_active,
                                          ctx.theme_col_tab_inactive,
                                          ctx.theme_col_tab_outline,
                                          ctx.theme_col_tab_outline_sel);

  category_tab_draw_content_layer(ctx.region,
                                  ctx.wm,
                                  category_id,
                                  category_id_draw,
                                  rct,
                                  box_rect,
                                  is_visual_effect_active,
                                  ctx.rct_xmin,
                                  ctx.rct_xmax,
                                  is_active,
                                  ctx.is_left,
                                  ctx.display_mode,
                                  ctx.fontid,
                                  ctx.fstyle,
                                  ctx.fstyle_points,
                                  ctx.zoom,
                                  ctx.category_tabs_zoom,
                                  current_tab_v_pad_text,
                                  darken_factor,
                                  ctx.theme_col_tab_text,
                                  ctx.theme_col_tab_text_sel,
                                  ctx.too_narrow,
                                  ctx.space_type);
}

static void category_tab_draw_dispatch(const CategoryTabSingleDrawContext &ctx,
                                       const rcti *rct,
                                       const CategoryTabLoopDrawData &draw_data,
                                       DeferredHoverTabDrawData &r_deferred_hover_tab_draw)
{
  const bool visual_effect_allowed_for_tab = category_tab_visual_effect_allowed(
      ctx.display_mode, ctx.is_dragging, draw_data.is_active);

  if (draw_data.is_hover && visual_effect_allowed_for_tab) {
    r_deferred_hover_tab_draw.valid = true;
    r_deferred_hover_tab_draw.rct = *rct;
    r_deferred_hover_tab_draw.category_id = draw_data.category_id;
    r_deferred_hover_tab_draw.category_id_draw = draw_data.category_id_draw;
    r_deferred_hover_tab_draw.is_active = draw_data.is_active;
    r_deferred_hover_tab_draw.current_tab_v_pad_text = draw_data.current_tab_v_pad_text;
    r_deferred_hover_tab_draw.darken_factor = draw_data.darken_factor;
    r_deferred_hover_tab_draw.bg_brighten_factor = draw_data.bg_brighten_factor;
    copy_v3_v3(r_deferred_hover_tab_draw.glyph_color, draw_data.glyph_color);
    return;
  }

  category_tab_draw_single(ctx,
                           rct,
                           draw_data.category_id,
                           draw_data.category_id_draw,
                           draw_data.is_active,
                           draw_data.is_hover,
                           draw_data.current_tab_v_pad_text,
                           draw_data.darken_factor,
                           draw_data.bg_brighten_factor,
                           draw_data.glyph_color);
}

static void category_tab_draw_deferred_hover_flush(
    const CategoryTabSingleDrawContext &ctx, const DeferredHoverTabDrawData &deferred_hover_tab_draw)
{
  if (!deferred_hover_tab_draw.valid) {
    return;
  }

  category_tab_draw_single(ctx,
                           &deferred_hover_tab_draw.rct,
                           deferred_hover_tab_draw.category_id,
                           deferred_hover_tab_draw.category_id_draw,
                           deferred_hover_tab_draw.is_active,
                           true,
                           deferred_hover_tab_draw.current_tab_v_pad_text,
                           deferred_hover_tab_draw.darken_factor,
                           deferred_hover_tab_draw.bg_brighten_factor,
                           deferred_hover_tab_draw.glyph_color);
}

static void category_tab_settings_draw_with_preview_shift(
    const bContext *C,
    ARegion *region,
    const View2D *v2d,
    const blender::Vector<PanelCategoryDyn *> &ordered_categories,
    const bool is_dragging,
    const float zoom,
    const uchar theme_col_tab_text[3])
{
  if (BLI_listbase_is_empty(&region->runtime->panels_category)) {
    return;
  }

  rcti *settings_rct_ptr = &region->runtime->category_tabs_settings_rect;
  const rcti settings_rct_backup = *settings_rct_ptr;
  bool settings_rect_overridden = false;

  if (!is_dragging && region->runtime && region->runtime->extension_drop_preview_state.active) {
    const ui::ExtensionDropPreviewState &preview = region->runtime->extension_drop_preview_state;
    if (preview.target_category_id[0] != '\0') {
      bool preview_name_found = false;
      for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
        if (STREQ(pc_dyn_ptr->idname, preview.target_category_id)) {
          preview_name_found = true;
          break;
        }
      }

      if (preview_name_found) {
        /* A successful drop inserts a new tab in the category stack regardless of insertion point.
         * Keep spacing consistent by shifting Display Mode Settings down during preview. */
        const int shift = EXTENSION_DROP_GHOST_HEIGHT + preview.tab_v_pad;
        settings_rct_ptr->ymin -= shift;
        settings_rct_ptr->ymax -= shift;
        settings_rect_overridden = true;
      }
    }
  }

  if (settings_rct_ptr->ymin <= v2d->mask.ymax && settings_rct_ptr->ymax >= v2d->mask.ymin) {
    panel_category_tabs_draw_settings_button(C, region, zoom, theme_col_tab_text);
  }

  if (settings_rect_overridden) {
    *settings_rct_ptr = settings_rct_backup;
  }
}

static CategoryTabSingleDrawContext category_tab_single_draw_context_build(
    const ARegion *region,
    const wmWindowManager *wm,
    const View2D *v2d,
    const int rct_xmin,
    const int rct_xmax,
    const bool is_left,
    const bool is_dragging,
    const eUserPref_CategoryTabsDisplayMode display_mode,
    const float tab_curve_radius,
    const int roundboxtype,
    const int px,
    const float theme_col_tab_active[4],
    const float theme_col_tab_inactive[4],
    const float theme_col_tab_outline[4],
    const float theme_col_tab_outline_sel[4],
    const int fontid,
    const uiFontStyle *fstyle,
    const float fstyle_points,
    const float zoom,
    const float category_tabs_zoom,
    const uchar theme_col_tab_text[3],
    const uchar theme_col_tab_text_sel[3],
    const bool too_narrow,
    const int space_type)
{
  CategoryTabSingleDrawContext draw_ctx{};
  draw_ctx.region = region;
  draw_ctx.wm = wm;
  draw_ctx.v2d = v2d;
  draw_ctx.rct_xmin = rct_xmin;
  draw_ctx.rct_xmax = rct_xmax;
  draw_ctx.is_left = is_left;
  draw_ctx.is_dragging = is_dragging;
  draw_ctx.display_mode = display_mode;
  draw_ctx.tab_curve_radius = tab_curve_radius;
  draw_ctx.roundboxtype = roundboxtype;
  draw_ctx.px = px;
  copy_v4_v4(draw_ctx.theme_col_tab_active, theme_col_tab_active);
  copy_v4_v4(draw_ctx.theme_col_tab_inactive, theme_col_tab_inactive);
  copy_v4_v4(draw_ctx.theme_col_tab_outline, theme_col_tab_outline);
  copy_v4_v4(draw_ctx.theme_col_tab_outline_sel, theme_col_tab_outline_sel);
  draw_ctx.fontid = fontid;
  draw_ctx.fstyle = fstyle;
  draw_ctx.fstyle_points = fstyle_points;
  draw_ctx.zoom = zoom;
  draw_ctx.category_tabs_zoom = category_tabs_zoom;
  copy_v3_v3_uchar(draw_ctx.theme_col_tab_text, theme_col_tab_text);
  copy_v3_v3_uchar(draw_ctx.theme_col_tab_text_sel, theme_col_tab_text_sel);
  draw_ctx.too_narrow = too_narrow;
  draw_ctx.space_type = space_type;
  return draw_ctx;
}

static void category_tab_draw_dragged_with_reorder_ghost(
    const ARegion *region,
    const wmWindowManager *wm,
    const blender::Vector<PanelCategoryDyn *> &ordered_categories,
    const CategoryDragState *drag_state,
    const char *drag_category_id,
    const char *category_id_active,
    const eUserPref_CategoryTabsDisplayMode display_mode,
    const bool is_left,
    const int tab_v_pad,
    const int tab_v_pad_text,
    const int roundboxtype,
    const float tab_curve_radius,
    const int fontid,
    const uiFontStyle *fstyle,
    const float fstyle_points,
    const float zoom,
    const float category_tabs_zoom,
    const uchar theme_col_tab_text[3],
    const uchar theme_col_tab_text_sel[3],
    const float theme_col_tab_active[4],
    const float theme_col_tab_outline_sel[4],
    const bool too_narrow,
    const int space_type)
{
  const PanelCategoryDyn *drag_tab = nullptr;
  for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (STREQ(pc_dyn.idname, drag_category_id)) {
      drag_tab = &pc_dyn;
      break;
    }
  }

  if (!drag_tab) {
    return;
  }

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
      ghost_bg_color[3] = 0.3f;

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
  const int scroll_diff = region->category_scroll - drag_state->initial_scroll;
  drag_rect.ymin -= scroll_diff;
  drag_rect.ymax -= scroll_diff;

  const int offset_y = int(drag_state->drag_offset_y);
  drag_rect.ymin += offset_y;
  drag_rect.ymax += offset_y;

  rctf box_rect;
  box_rect.xmin = float(drag_rect.xmin);
  box_rect.xmax = float(drag_rect.xmax);
  box_rect.ymin = float(drag_rect.ymin);
  box_rect.ymax = float(drag_rect.ymax);

  float drag_bg_color[4];
  copy_v4_v4(drag_bg_color, theme_col_tab_active);
  drag_bg_color[3] = 0.7f;

  GPU_blend(GPU_BLEND_ALPHA);
  draw_roundbox_corner_set(roundboxtype);
  draw_roundbox_4fv(&box_rect, true, tab_curve_radius, drag_bg_color);
  draw_roundbox_4fv(&box_rect, false, tab_curve_radius, theme_col_tab_outline_sel);

  /* Get glyph color for color indicator in TEXT_ONLY mode. */
  bool is_fallback_letter = false;
  float glyph_color[3] = {0.0f, 0.0f, 0.0f};
  const char *category_id = drag_tab->idname;
  panel_category_glyph_lookup(wm, category_id, nullptr, &is_fallback_letter, glyph_color, space_type);

  /* Draw color indicator bar for TEXT_ONLY mode to show assigned glyph color. */
  draw_category_tab_color_indicator(
      &drag_rect, glyph_color, is_left, display_mode, U.category_tabs_text_mode_show_color_indicator, true);

  const CategoryTabRenderData render_data = category_tab_render_data_build(
      wm, region, category_id, space_type);
  const char *category_id_draw = render_data.category_id_draw;
  const rcti *rct = &drag_rect;

  const bool is_active_tab = category_id_active && STREQ(category_id, category_id_active);

  const CategoryTabNameLayout name_layout = category_tab_name_layout_resolve(
      region, category_id, display_mode, is_active_tab, false, false, tab_v_pad_text);
  int current_drag_tab_v_pad_text = name_layout.tab_v_pad_text;

  ui_panel_category_draw_content(region,
                                 wm,
                                 category_id,
                                 category_id_draw,
                                 rct,
                                 rct->xmin,
                                 rct->xmax,
                                 is_active_tab,
                                 is_left,
                                 display_mode,
                                 fontid,
                                 fstyle,
                                 fstyle_points,
                                 zoom,
                                 category_tabs_zoom,
                                 current_drag_tab_v_pad_text,
                                 0.0f,
                                 theme_col_tab_text,
                                 theme_col_tab_text_sel,
                                 too_narrow,
                                 space_type);
}

static const char *category_tab_extension_drop_ghost_draw(
    const ARegion *region,
    const View2D *v2d,
    const blender::Vector<PanelCategoryDyn *> &ordered_categories,
    const bool is_dragging,
    const int rct_xmin,
    const int rct_xmax,
    const int roundboxtype,
    const float tab_curve_radius,
    const float theme_col_tab_active[4],
    const float theme_col_tab_outline_sel[4])
{
  if (is_dragging || !region->runtime || !region->runtime->extension_drop_preview_state.active) {
    return nullptr;
  }

  const ui::ExtensionDropPreviewState &preview = region->runtime->extension_drop_preview_state;

  rcti ghost_rect;
  ghost_rect.xmin = rct_xmin;
  ghost_rect.xmax = rct_xmax;

  const int ghost_height = EXTENSION_DROP_GHOST_HEIGHT;

  static double _ghost_last_log_time = 0.0;
  static int _ghost_last_target = -999;
  const double current_time = BLI_time_now_seconds();
  const bool should_log = (current_time - _ghost_last_log_time > 1.0) ||
                          (_ghost_last_target != preview.target_index);

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    if (should_log) {
      printf("[EXT_GHOST] === TABS POSITIONS (top to bottom) ===\n");
      int log_idx = 0;
      for (PanelCategoryDyn *pc : ordered_categories) {
        printf("[EXT_GHOST]   tab[%d] '%s' y=[%d,%d] h=%d\n",
               log_idx,
               pc->idname,
               pc->rect.ymin,
               pc->rect.ymax,
               BLI_rcti_size_y(&pc->rect));
        log_idx++;
        if (log_idx > 10) {
          printf("[EXT_GHOST]   ... (more tabs)\n");
          break;
        }
      }
      printf("[EXT_GHOST] v2d_mask: y=[%d,%d] x=[%d,%d]\n",
             v2d->mask.ymin,
             v2d->mask.ymax,
             v2d->mask.xmin,
             v2d->mask.xmax);
      printf("[EXT_GHOST] ghost_h=%d target_idx=%d insert_above=%d\n",
             ghost_height,
             preview.target_index,
             preview.insert_above ? 1 : 0);
      printf("[EXT_GHOST] preview_state: active=%d target='%s' idx=%d insert_above=%d tab_h=%d pad=%d cursor_y=%d\n",
             preview.active ? 1 : 0,
             preview.target_category_id,
             preview.target_index,
             preview.insert_above ? 1 : 0,
             preview.tab_height,
             preview.tab_v_pad,
             preview.cursor_y);
      _ghost_last_log_time = current_time;
      _ghost_last_target = preview.target_index;
    }
  }

  /* CRITICAL FIX: Find target tab using category name first, then fall back to full region index.
   * preview.target_index now contains full region indices (not filtered visual indices),
   * so we need to map it back to the filtered ordered_categories list correctly. */
  
  PanelCategoryDyn *target_tab = nullptr;
  int resolved_target_index = -1;
  
  /* First, try to find by category name (most reliable) */
  if (preview.target_category_id[0] != '\0') {
    int visual_idx = 0;
    for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
      if (STREQ(pc_dyn_ptr->idname, preview.target_category_id)) {
        target_tab = pc_dyn_ptr;
        resolved_target_index = visual_idx;
        break;
      }
      visual_idx++;
    }
  }
  
  /* If not found by name, build a mapping from full region index to visual index */
  if (!target_tab && preview.target_index >= 0) {
    /* Create mapping from full region index to current visual index */
    Map<int, int> full_to_visual_index;
    int visual_idx = 0;
    int full_idx = 0;
    
    for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
      if (pc_dyn.idname && pc_dyn.idname[0] != '\0') {
        /* Check if this category is in the current filtered list */
        for (PanelCategoryDyn *visible_cat : ordered_categories) {
          if (STREQ(visible_cat->idname, pc_dyn.idname)) {
            full_to_visual_index.add(full_idx, visual_idx);
            visual_idx++;
            break;
          }
        }
      }
      full_idx++;
    }
    
    /* Now find the target using the mapped index */
    if (int *visual_index_ptr = full_to_visual_index.lookup_ptr(preview.target_index)) {
      int target_visual_index = *visual_index_ptr;
      if (target_visual_index < ordered_categories.size()) {
        target_tab = ordered_categories[target_visual_index];
        resolved_target_index = target_visual_index;
        
        if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
          if (should_log) {
            printf("[EXT_GHOST] Found by MAPPED INDEX: full_idx=%d -> visual_idx=%d -> '%s'\n",
                   preview.target_index, target_visual_index, target_tab->idname);
          }
        }
      }
    } else {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        if (should_log) {
          printf("[EXT_GHOST] Could not map full_idx=%d to visual index (category may be filtered out)\n",
                 preview.target_index);
        }
      }
    }
  }

  if (!target_tab) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (should_log) {
        printf("[EXT_GHOST] SKIP: target_tab NULL for name='%s' full_idx=%d (may be filtered out)\n",
               preview.target_category_id,
               preview.target_index);
      }
    }
    return "target_tab_null";
  }

  const int category_count = int(ordered_categories.size());
  const int raw_insertion_index = (resolved_target_index >= 0) ?
                                      (resolved_target_index + (preview.insert_above ? 0 : 1)) :
                                      -1;
  const int insertion_index = raw_insertion_index;
  const int shift_space = EXTENSION_DROP_GHOST_HEIGHT + preview.tab_v_pad;

  int slot_top_y, slot_bottom_y;

  if (insertion_index == 0) {
    slot_top_y = target_tab->rect.ymax + shift_space;
    slot_bottom_y = target_tab->rect.ymax;
  }
  else if (insertion_index >= category_count) {
    PanelCategoryDyn *last_tab = ordered_categories[category_count - 1];
    slot_top_y = last_tab->rect.ymin;
    slot_bottom_y = slot_top_y - shift_space;
  }
  else {
    if (preview.insert_above) {
      slot_top_y = target_tab->rect.ymax;
      slot_bottom_y = slot_top_y - shift_space;
    }
    else {
      slot_top_y = target_tab->rect.ymin;
      slot_bottom_y = slot_top_y - shift_space;
    }
  }

    const int slot_center_y = (slot_top_y + slot_bottom_y) / 2;
    const int half_ghost_height = ghost_height / 2;

    ghost_rect.ymin = slot_center_y - half_ghost_height;
    ghost_rect.ymax = slot_center_y + half_ghost_height;

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (should_log) {
        printf("[EXT_GHOST] resolve: target='%s' stored_idx=%d resolved_idx=%d insert_above=%d raw_insertion_idx=%d insertion_idx=%d category_count=%d\n",
               preview.target_category_id,
               preview.target_index,
               resolved_target_index,
               preview.insert_above ? 1 : 0,
               raw_insertion_index,
               insertion_index,
               category_count);
        if (raw_insertion_index >= category_count && category_count > 0) {
          printf("[EXT_GHOST] boundary: raw_insertion_idx >= category_count (using end slot below last tab)\n");
        }
        printf("[EXT_GHOST] insertion_idx=%d slot=[%d,%d] center=%d ghost=[%d,%d] target_rect=[%d,%d]\n",
               insertion_index,
               slot_bottom_y,
             slot_top_y,
             slot_center_y,
             ghost_rect.ymin,
             ghost_rect.ymax,
             target_tab->rect.ymin,
             target_tab->rect.ymax);
      }
    }

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (should_log) {
        const int ghost_center_y = (ghost_rect.ymin + ghost_rect.ymax) / 2;
        const int center_diff = abs(ghost_center_y - slot_center_y);
        printf("[EXT_GHOST] FINAL: cursor_y=%d ghost=[%d,%d] center=%d slot_center=%d diff=%d target='%s'\n",
               preview.cursor_y,
               ghost_rect.ymin,
               ghost_rect.ymax,
               ghost_center_y,
               slot_center_y,
               center_diff,
               target_tab->idname);
        if (center_diff > 1) {
          printf("[EXT_GHOST] WARNING: Ghost not centered in slot! (diff=%d px)\n", center_diff);
        }
      }
    }

  const bool ghost_in_viewport = (ghost_rect.ymax >= v2d->mask.ymin &&
                                  ghost_rect.ymin <= v2d->mask.ymax);

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    if (should_log) {
      printf("[EXT_GHOST] VIEWPORT CHECK: ghost=[%d,%d] viewport=[%d,%d] in_viewport=%d\n",
             ghost_rect.ymin,
             ghost_rect.ymax,
             v2d->mask.ymin,
             v2d->mask.ymax,
             ghost_in_viewport ? 1 : 0);
    }
  }

  if (!ghost_in_viewport) {
    if (ghost_rect.ymax < v2d->mask.ymin) {
      const int ghost_height_local = ghost_rect.ymax - ghost_rect.ymin;
      ghost_rect.ymin = v2d->mask.ymin;
      ghost_rect.ymax = v2d->mask.ymin + ghost_height_local;
    }
    else if (ghost_rect.ymin > v2d->mask.ymax) {
      const int ghost_height_local = ghost_rect.ymax - ghost_rect.ymin;
      ghost_rect.ymax = v2d->mask.ymax;
      ghost_rect.ymin = v2d->mask.ymax - ghost_height_local;
    }

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (should_log) {
        printf("[EXT_GHOST] CLAMPED: ghost=[%d,%d]\n", ghost_rect.ymin, ghost_rect.ymax);
      }
    }
  }

  rctf ghost_box_rect;
  ghost_box_rect.xmin = float(ghost_rect.xmin);
  ghost_box_rect.xmax = float(ghost_rect.xmax);
  ghost_box_rect.ymin = float(ghost_rect.ymin);
  ghost_box_rect.ymax = float(ghost_rect.ymax);

  float ghost_bg_color[4];
  copy_v4_v4(ghost_bg_color, theme_col_tab_active);
  ghost_bg_color[3] = 0.5f;

  GPU_blend(GPU_BLEND_ALPHA);
  draw_roundbox_corner_set(roundboxtype);
  draw_roundbox_4fv(&ghost_box_rect, true, tab_curve_radius, ghost_bg_color);

  float ghost_outline[4];
  copy_v3_v3(ghost_outline, theme_col_tab_outline_sel);
  ghost_outline[3] = 0.8f;
  draw_roundbox_4fv(&ghost_box_rect, false, tab_curve_radius, ghost_outline);

  GPU_blend(GPU_BLEND_NONE);
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    if (should_log) {
      printf("[EXT_GHOST] DRAWN: ghost_rect=[%d,%d]-[%d,%d]\n",
             ghost_rect.xmin,
             ghost_rect.ymin,
             ghost_rect.xmax,
             ghost_rect.ymax);
    }
  }

  return nullptr;
}

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
    const uchar theme_col_tab_text_sel[3],
    const bool is_panel_minimized,
    const int space_type)
{
  const float tab_font_size = fstyle_points * UI_SCALE_FAC * category_tabs_zoom;
  const bool is_being_edited_in_dialog =
      category_tab_edit_dialog_is_open_for_category(category_id);
  const float draw_darken_factor = is_being_edited_in_dialog ? 0.0f : darken_factor;

  bool is_fallback_letter = false;
  float glyph_color[3] = {0.0f, 0.0f, 0.0f};
  const char *glyph = panel_category_glyph_lookup(
      wm, category_id, nullptr, &is_fallback_letter, glyph_color, space_type);

  /* Use live preview glyph and color when dialog is open for this category */
  if (is_being_edited_in_dialog) {
    if (category_tab_preview_glyph[0] != '\0') {
      glyph = category_tab_preview_glyph;
      is_fallback_letter = (category_tab_preview_first_letter[0] != '\0') &&
                           STREQ(category_tab_preview_glyph, category_tab_preview_first_letter);
    }
    copy_v3_v3(glyph_color, category_tab_preview_color);
  }

  /* Handle nullptr glyph (explicitly cleared) - use fallback letter from category */
  char fallback_glyph_buf[8];
  if (glyph == nullptr && is_fallback_letter) {
    /* For glyph-id categories use human-readable name (panel label/display name),
     * otherwise use category id. */
    const char *first_letter_source = category_first_letter_source_name_get(
        region, wm, category_id, category_id_draw, space_type);
    const int first_char_size = BLI_str_utf8_size_safe(first_letter_source);
    if (first_char_size > 0) {
      memcpy(fallback_glyph_buf, first_letter_source, first_char_size);
      fallback_glyph_buf[first_char_size] = '\0';
      glyph = fallback_glyph_buf;
    }
    else {
      /* Fallback to category_id if we can't extract first char */
      glyph = category_id;
    }
  }

  const bool has_glyph = is_single_glyph_str(glyph) && !is_fallback_letter;

  const CategoryTabMixedContentFlags mixed_content_flags = category_tab_mixed_content_flags_resolve(
      wm, category_id, display_mode, space_type, has_glyph, is_fallback_letter);
  const int resolved_icon_id = mixed_content_flags.resolved_icon_id;
  const bool use_builtin_icon = mixed_content_flags.use_builtin_icon;
  const bool mixed_mode_effective_has_glyph = mixed_content_flags.mixed_mode_effective_has_glyph;
  const bool mixed_mode_effective_fallback_letter =
      mixed_content_flags.mixed_mode_effective_fallback_letter;
  const bool mixed_mode_effective_builtin_icon =
      mixed_content_flags.mixed_mode_effective_builtin_icon;
  const bool mixed_mode_has_visible_glyph_content =
      mixed_content_flags.mixed_mode_has_visible_glyph_content;

  /* Safety net for built-in icons:
   * icon tint must use category custom color even when glyph lookup resolves through
   * fallback branches that may leave color unset in transient live-preview states. */
  if (use_builtin_icon && is_zero_v3(glyph_color)) {
    const bool preview_explicit_no_color = is_being_edited_in_dialog &&
                                           is_zero_v3(category_tab_preview_color);
    if (!preview_explicit_no_color) {
      panel_category_color_lookup(wm, category_id, glyph_color);
    }
  }

  const bool use_reserved_inactive_icon_only =
      U.category_tabs_hide_reserved_inactive_text && !is_active &&
      ELEM(display_mode, USER_CATEGORY_TABS_GLYPHS_TEXT, USER_CATEGORY_TABS_TEXT_ONLY) &&
      category_is_reserved_for_reorder(wm, category_id) && has_glyph;

  bool draw_dual = false;
  const char *text_for_name = category_tab_draw_label_resolve(region, category_id, category_id_draw);

  if (display_mode == USER_CATEGORY_TABS_GLYPHS_TEXT && !use_reserved_inactive_icon_only &&
      mixed_mode_has_visible_glyph_content)
  {
    draw_dual = true;
  }
  else if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
           U.category_tabs_show_active_name &&
           !region->runtime->category_tabs_active_name_hidden)
  {
    /* Show name for active tab OR for the previous active tab (in minimized state).
     * The previous active tab is set when clicking on active tab to minimize the panel.
     * This allows users to see which tab they just collapsed.
     * IMPORTANT: Previous active tab only shows name when panel is minimized (collapsed)
     * AND inactive behavior is set to STICKY. */
    const bool is_sticky_inactive = (U.category_tabs_inactive_behavior == USER_CATEGORY_TABS_INACTIVE_STICKY);
    const bool is_previous_active = (is_sticky_inactive &&
                                     is_panel_minimized &&
                                     region->runtime->category_tabs_previous_active_id[0] != '\0' &&
                                     STREQ(category_id, region->runtime->category_tabs_previous_active_id));
    if ((is_active || is_previous_active) && (has_glyph || is_fallback_letter)) {
      draw_dual = true;
    }
  }

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
    BLF_disable(fontid, BLF_ROTATION);
    BLF_size(fontid, tab_font_size);

    const float glyph_width_val = BLF_width(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX);
    const float ascender = float(BLF_ascender(fontid));
    const float descender = float(BLF_descender(fontid));
    const float glyph_height = ascender - descender;

    const float tab_center_x = float(rct->xmin + rct->xmax) * 0.5f;
    /* extra_shift is only for visual positioning of fallback letters.
     * Apply shift if the content IS a fallback letter (regardless of visibility settings). */
    const float extra_shift = is_fallback_letter ? (4.0f * UI_SCALE_FAC) : 0.0f;
    const float glyph_pos_y = float(rct->ymax) - glyph_height - (tab_v_pad_text - extra_shift);

    /* --- BEGIN: MIXED_MODE_CONTENT_FLAGS (icon/glyph drawing) --- */
    /* In Mixed mode, use effective icon visibility; in other modes use original flag.
     * To remove: replace with: const bool should_draw_builtin_icon = use_builtin_icon; */
    const bool should_draw_builtin_icon = (display_mode == USER_CATEGORY_TABS_GLYPHS_TEXT) ?
        mixed_mode_effective_builtin_icon : use_builtin_icon;

    if (should_draw_builtin_icon) {
      const float builtin_icon_size = glyph_height * TABS_BUILTIN_ICON_SCALE;
      const float icon_center_y = float(rct->ymax) - tab_v_pad_text - glyph_height * 0.5f +
                                  extra_shift;
      draw_category_tab_builtin_icon(rct,
                                     resolved_icon_id,
                                     icon_center_y,
                                     builtin_icon_size,
                                     glyph_color,
                                     is_active,
                                     !is_active ? draw_darken_factor : 0.0f,
                                     theme_col_tab_text,
                                     theme_col_tab_text_sel);
    }
    /* To remove: replace condition with: else if (has_glyph || is_fallback_letter) */
    else if (display_mode == USER_CATEGORY_TABS_GLYPHS_TEXT ?
             (mixed_mode_effective_has_glyph || mixed_mode_effective_fallback_letter) :
             (has_glyph || is_fallback_letter)) {
      /* Draw glyph/fallback letter only if the appropriate content type is enabled.
       * In Mixed mode, respect per-content-type visibility flags.
       * In other modes, draw if any glyph content exists. */
      BLF_position(fontid, tab_center_x - glyph_width_val * 0.5f, glyph_pos_y - descender, 0.0f);
      uchar glyph_color_out[3];
      set_glyph_color(
          fontid, glyph_color, is_active, theme_col_tab_text, theme_col_tab_text_sel, glyph_color_out);
      if (!is_active && draw_darken_factor > 0.0f) {
        apply_glyph_darkening(fontid, glyph_color_out, draw_darken_factor);
      }

      shadow_enable();
      BLF_draw(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX);
      shadow_disable();
    }
    /* --- END: MIXED_MODE_CONTENT_FLAGS (icon/glyph drawing) --- */

    /* Built-in icon draw can touch BLF internal state (SVG path). Restore tab text size explicitly
     * so category names remain identical with/without assigned icon. */
    BLF_size(fontid, tab_font_size);

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

    if (!is_active && draw_darken_factor > 0.0f) {
      uchar text_color[3] = {theme_col_tab_text[0], theme_col_tab_text[1], theme_col_tab_text[2]};
      darken_color_3ub(text_color, draw_darken_factor);
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

  const char *draw_str;
  bool draw_as_glyph;
  bool should_rotate = false;

  if (use_reserved_inactive_icon_only) {
    draw_str = glyph;
    draw_as_glyph = true;
    should_rotate = false;
  }
  else {
    switch (display_mode) {
      case USER_CATEGORY_TABS_GLYPHS_ONLY:
        draw_str = glyph;
        /* In icon-only mode, a resolved built-in icon must still draw even when the glyph source
         * is marked as fallback letter (common when no custom glyph is stored for the category). */
        draw_as_glyph = use_builtin_icon ? true : !is_fallback_letter;
        should_rotate = false;
        break;
      case USER_CATEGORY_TABS_GLYPHS_TEXT:
        /* --- BEGIN: MIXED_MODE_CONTENT_FLAGS (panel label for single-glyph) --- */
        /* In Mixed mode, if glyph content is not visible (no dual mode),
         * use panel label for single-glyph categories (same as TEXT_ONLY mode).
         * To remove: replace this block with simple: draw_str = category_id_draw; */
          if (!draw_dual && is_single_glyph_str(category_id_draw)) {
            const char *panel_label = nullptr;
            
            /* TODO: Extension-aware panel selection - currently using original behavior
             * Will be implemented when proper source_extension tracking is available */
            
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
        /* --- END: MIXED_MODE_CONTENT_FLAGS (panel label for single-glyph) --- */
        draw_as_glyph = is_single_glyph_str(draw_str);
        should_rotate = !draw_as_glyph;
        break;
      case USER_CATEGORY_TABS_TEXT_ONLY:
      default:
         if (is_single_glyph_str(category_id_draw)) {
           const char *panel_label = nullptr;
           
           /* TODO: Extension-aware panel selection - currently using original behavior
            * Will be implemented when proper source_extension tracking is available */
           
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
  }

  BLF_size(fontid, tab_font_size);

  if (!should_rotate) {
    BLF_disable(fontid, BLF_ROTATION);
    const float gw = BLF_width(fontid, draw_str, BLF_DRAW_STR_DUMMY_MAX);
    const float asc = float(BLF_ascender(fontid));
    const float desc = float(BLF_descender(fontid));
    const float gh = asc - desc;
    const float cx = float(rct->xmin + rct->xmax) * 0.5f;
    const float cy = float(rct->ymin + rct->ymax) * 0.5f;
    const float draw_pos_y = cy - gh * 0.5f - desc;
    BLF_position(fontid, cx - gw * 0.5f, draw_pos_y, 0.0f);

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

  if (draw_as_glyph) {
    if (use_builtin_icon) {
      /* Keep icon size independent from string-specific glyph bounds and active state. */
      const float icon_font_height = float(BLF_ascender(fontid)) - float(BLF_descender(fontid));
      const float builtin_icon_size = icon_font_height * TABS_BUILTIN_ICON_SCALE;
      const float icon_center_y = float(rct->ymin + rct->ymax) * 0.5f;
      draw_category_tab_builtin_icon(rct,
                                     resolved_icon_id,
                                     icon_center_y,
                                     builtin_icon_size,
                                     glyph_color,
                                     is_active,
                                     draw_darken_factor,
                                     theme_col_tab_text,
                                     theme_col_tab_text_sel);
      BLF_disable(fontid, BLF_ROTATION);
      return;
    }

    uchar glyph_color_out[3];
    set_glyph_color(
        fontid, glyph_color, is_active, theme_col_tab_text, theme_col_tab_text_sel, glyph_color_out);
    apply_glyph_darkening(fontid, glyph_color_out, draw_darken_factor);
  }
  else {
    uchar text_color[3];
    /* Use custom glyph color even for fallback letters or when in icon-only mode.
     * Respect the 'Show Colored Text' preference in Text mode.
     * For active tab in TEXT_ONLY mode with colored text enabled: use standard color
     * and show color indicator instead.
     * In Mixed mode: only use custom color if glyphs are actually visible (dual mode). */
    bool use_custom_color = !is_zero_v3(glyph_color);
    if (display_mode == USER_CATEGORY_TABS_TEXT_ONLY) {
      if (!U.category_tabs_text_mode_show_colored_text) {
        use_custom_color = false;
      }
      else if (is_active) {
        /* Active tab: use standard color, color indicator will be shown instead. */
        use_custom_color = false;
      }
    }
    else if (display_mode == USER_CATEGORY_TABS_GLYPHS_TEXT) {
      /* --- BEGIN: MIXED_MODE_CONTENT_FLAGS (text color when glyph disabled) --- */
      /* In Mixed mode, only use custom color if we're showing glyph content (dual mode)
       * AND the content is either a custom glyph or fallback letter (not just an icon).
       * If only icons are visible, or no glyph content is visible, use standard theme color.
       * To remove: delete this entire else-if block. */
      const bool showing_glyph_or_fallback = draw_dual &&
          (mixed_mode_effective_has_glyph || mixed_mode_effective_fallback_letter);
      if (!showing_glyph_or_fallback) {
        use_custom_color = false;
      }
      /* --- END: MIXED_MODE_CONTENT_FLAGS (text color when glyph disabled) --- */
    }

    if (use_custom_color) {
      text_color[0] = uchar(glyph_color[0] * 255);
      text_color[1] = uchar(glyph_color[1] * 255);
      text_color[2] = uchar(glyph_color[2] * 255);
    }
    else {
      text_color[0] = is_active ? theme_col_tab_text_sel[0] : theme_col_tab_text[0];
      text_color[1] = is_active ? theme_col_tab_text_sel[1] : theme_col_tab_text[1];
      text_color[2] = is_active ? theme_col_tab_text_sel[2] : theme_col_tab_text[2];
    }
    if (draw_darken_factor > 0.0f) {
      darken_color_3ub(text_color, draw_darken_factor);
    }
    BLF_color3ubv(fontid, text_color);
  }

  shadow_enable();
  BLF_draw(fontid, draw_str, BLF_DRAW_STR_DUMMY_MAX);
  shadow_disable();

  BLF_disable(fontid, BLF_ROTATION);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Indicator Drawing
 * \{ */

/**
 * Draw color indicator bar for TEXT_ONLY mode to show assigned glyph color.
 * This is used both for regular tabs and during drag & drop.
 * When 'Show Colored Text' is enabled, only draw indicator for the active tab.
 */
static void draw_category_tab_color_indicator(const rcti *rct,
                                                const float glyph_color[3],
                                                const bool is_left,
                                                const eUserPref_CategoryTabsDisplayMode display_mode,
                                                const bool show_color_indicator,
                                                const bool is_active)
{
  /* Only draw indicator in TEXT_ONLY mode. */
  if (display_mode != USER_CATEGORY_TABS_TEXT_ONLY) {
    return;
  }

  /* Check if custom glyph color is assigned (non-zero means custom color set). */
  if (glyph_color[0] == 0.0f && glyph_color[1] == 0.0f && glyph_color[2] == 0.0f) {
    return;
  }

  /* Two modes for showing indicator:
   * 1. When 'Show Colored Text' is enabled: show indicator ONLY for active tab.
   * 2. When 'Show Colored Text' is disabled: respect 'Show Color Indicator' for all tabs. */
  if (U.category_tabs_text_mode_show_colored_text) {
    /* Colored text mode: indicator only for active tab. */
    if (!is_active) {
      return;
    }
  }
  else {
    /* Standard mode: use Show Color Indicator setting. */
    if (!show_color_indicator) {
      return;
    }
  }

  const int indicator_thickness = round_fl_to_int(1.0f * UI_SCALE_FAC);

  /* For left-aligned tabs: indicator on left edge (outer edge). */
  /* For right-aligned tabs: indicator on right edge (outer edge). */
  const int indicator_xmin = is_left ? rct->xmin : rct->xmax - indicator_thickness;
  const int indicator_xmax = indicator_xmin + indicator_thickness;

  /* Setup GPU for immediate mode rectangle drawing. */
  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  float indicator_color[4] = {glyph_color[0], glyph_color[1], glyph_color[2], 0.8f};
  immUniformColor4fv(indicator_color);
  immRectf(pos, float(indicator_xmin), float(rct->ymin), float(indicator_xmax), float(rct->ymax));

  immUnbindProgram();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Drawing Function
 * \{ */

/**
 * Find the reserved category for an extension and switch to it.
 * Used for reserved-only extensions like Bool Tool that add panels only to reserved categories.
 * 
 * \param C              Blender context.
 * \param region         Region where tabs are drawn.
 * \param source_extension Extension package ID.
 * \param space_type     Space type to search in.
 * \return True if a reserved category was found and activated.
 */
bool switch_to_reserved_category_for_extension(const bContext *C,
                                               ARegion *region,
                                               const char *source_extension,
                                               int space_type)
{
  /* Currently space_type is not used, but kept for future extension. */
  (void)space_type;

  printf("[RESERVED SWITCH] switch_to_reserved_category_for_extension called: extension='%s', space_type=%d\n",
         source_extension ? source_extension : "(null)", space_type);
  fflush(stdout);

  if (!C || !region || !source_extension || source_extension[0] == '\0') {
    printf("[RESERVED SWITCH] Invalid parameters: C=%p, region=%p, source_extension=%s\n",
           C, (void *)region, source_extension ? source_extension : "(null)");
    return false;
  }

  const wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm) {
    printf("[RESERVED SWITCH] No window manager\n");
    return false;
  }

  /* Debug: List all categories in wm->category_glyph_mappings */
  printf("[RESERVED SWITCH] Scanning wm->category_glyph_mappings for extension '%s':\n", source_extension);
  int total_items = 0;
  int matching_items = 0;
  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(
           wm->category_glyph_mappings.first);
       item;
       item = item->next)
  {
    total_items++;
    if (STREQ(item->source_extension, source_extension)) {
      matching_items++;
      printf("[RESERVED SWITCH]   MATCH: category='%s', is_reserved=%d, discovered_in_spaces=%d\n",
             item->category, item->is_reserved, item->discovered_in_spaces);
    }
  }
  printf("[RESERVED SWITCH] Total items: %d, matching extension: %d\n", total_items, matching_items);
  fflush(stdout);

  /* Find all categories for this extension and look for reserved ones.
   * For reserved categories, we don't require discovered_in_spaces != 0,
   * since the category may not have panels registered yet (extension still loading).
   * The reserved category is defined in Python DEFAULT_CATEGORY_GLYPHS and exists
   * in wm->category_glyph_mappings immediately after extension install. */
  std::string reserved_category;
  bool found_reserved = false;

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(
           wm->category_glyph_mappings.first);
       item;
       item = item->next)
  {
    if (STREQ(item->source_extension, source_extension)) {
      if (item->is_reserved != 0) {
        /* Found a reserved category (with or without panels yet). */
        reserved_category = item->category;
        found_reserved = true;
        printf("[RESERVED SWITCH] Found reserved category '%s' for extension '%s' (discovered_in_spaces=%d)\n",
               reserved_category.c_str(), source_extension, item->discovered_in_spaces);
        break; /* Take the first reserved category */
      }
    }
  }

  if (!found_reserved || reserved_category.empty()) {
    printf("[RESERVED SWITCH] No reserved category found for extension '%s'\n", source_extension);
    return false;
  }

  /* Check if already active */
  const char *current_active = panel_category_active_get(region, false);
  if (current_active && STREQ(reserved_category.c_str(), current_active)) {
    printf("[RESERVED SWITCH] Reserved category '%s' already active\n", reserved_category.c_str());
    return true;
  }

  /* Activate the reserved category */
  printf("[RESERVED SWITCH] Activating reserved category '%s' for extension '%s'\n",
         reserved_category.c_str(), source_extension);
  panel_category_active_set(region, reserved_category.c_str());

  /* Save to tag category memory */
  blender::ui::TagFilterStateRef tag_state{};
  ScrArea *area = CTX_wm_area(C);
  if (blender::ui::tag_filter_state_from_area(area, &tag_state) && tag_state.active_tags &&
      tag_state.filter_enabled && *tag_state.filter_enabled)
  {
    char tag_key_buf[256];
    blender::ui::tag_build_combination_key(tag_state.active_tags, tag_key_buf, sizeof(tag_key_buf));
    blender::ui::tag_save_last_active_category(
        const_cast<bContext *>(C), tag_key_buf, reserved_category.c_str());
  }

  /* Trigger UI refresh */
  WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);
  ED_region_tag_redraw(region);

  printf("[RESERVED SWITCH] Successfully switched to reserved category '%s'\n", reserved_category.c_str());
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Operator: Switch to Reserved Category
 * \{ */

static wmOperatorStatus switch_to_reserved_category_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent * /*event*/)
{
  std::string source_extension = RNA_string_get(op->ptr, "source_extension");
  const int space_type = RNA_enum_get(op->ptr, "space_type");

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    BKE_report(op->reports, RPT_ERROR, "No active region found");
    return OPERATOR_CANCELLED;
  }

  if (switch_to_reserved_category_for_extension(C, region, source_extension.c_str(), space_type)) {
    return OPERATOR_FINISHED;
  }

  BKE_report(op->reports, RPT_WARNING, "No reserved category found for this extension");
  return OPERATOR_CANCELLED;
}

static bool switch_to_reserved_category_poll(bContext * /*C*/)
{
  return true;
}

void UI_OT_switch_to_reserved_category(wmOperatorType *ot)
{
  ot->name = "Switch to Reserved Category";
  ot->idname = "UI_OT_switch_to_reserved_category";
  ot->description = "Switch to the reserved category for a reserved-only extension";

  ot->invoke = switch_to_reserved_category_invoke;
  ot->poll = switch_to_reserved_category_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna, "source_extension", nullptr, 0, "Source Extension",
                 "Extension package ID to find the reserved category for");

  RNA_def_enum(ot->srna, "space_type", rna_enum_space_type_items, SPACE_EMPTY, "Space Type",
               "Space type to search in");
}

/** \} */

/* Execute deferred category activation if pending.
 * This is called from panel_category_tabs_draw_all which runs in a safe context
 * after panel layout is complete, avoiding crashes when extensions load previews
 * in background threads. */
static void deferred_category_activation_execute(const bContext *C, ARegion *region)
{
  if (!g_deferred_category_activation.valid) {
    return;
  }

  /* Debug: Log deferred activation state */
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] deferred_category_activation_execute called:\n");
    printf("[CATEGORY ACTIVATE]   valid=%d, category_id='%s', source_extension_id='%s'\n",
           g_deferred_category_activation.valid,
           g_deferred_category_activation.category_id.c_str(),
           g_deferred_category_activation.source_extension_id.c_str());
    printf("[CATEGORY ACTIVATE]   wait_for_signal=%d, signal_received=%d, discover_mode=%d, retry=%d\n",
           g_deferred_category_activation.wait_for_extension_signal,
           g_deferred_category_activation.extension_signal_received,
           g_deferred_category_activation.discover_new_category,
           g_deferred_category_activation.discover_retry_count);
    fflush(stdout);
  }

  /* Check for reserved-only extension - switch immediately without waiting for discover mode.
   * This handles cases like Bool Tool where all panels are in reserved categories.
   * Check this FIRST before any other deferred activation logic. */
  if (!g_deferred_category_activation.source_extension_id.empty()) {
    const wmWindowManager *wm = CTX_wm_manager(C);
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Checking reserved-only for extension '%s', wm=%p\n",
             g_deferred_category_activation.source_extension_id.c_str(), (void *)wm);
      printf("[CATEGORY ACTIVATE]   discover_new_category=%d, category_id='%s'\n",
             g_deferred_category_activation.discover_new_category ? 1 : 0,
             g_deferred_category_activation.category_id.c_str());
      fflush(stdout);
    }

    if (wm && blender::ui::extension_has_only_reserved_categories(
              wm, g_deferred_category_activation.source_extension_id.c_str()))
    {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE] *** RESERVED-ONLY EXTENSION DETECTED *** (source='%s')\n",
               g_deferred_category_activation.source_extension_id.c_str());
        printf("[CATEGORY ACTIVATE] This will bypass new category activation and switch to reserved category\n");
        fflush(stdout);
      }
      const ScrArea *current_area = CTX_wm_area(C);
      const int space_type = current_area ? current_area->spacetype : -1;
      if (switch_to_reserved_category_for_extension(
              C, region, g_deferred_category_activation.source_extension_id.c_str(), space_type))
      {
        if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
          printf("[CATEGORY ACTIVATE] Reserved-only switch completed, clearing deferred activation\n");
        }
        g_deferred_category_activation.valid = false;
        g_deferred_category_activation.wait_for_extension_signal = false;
        g_deferred_category_activation.extension_signal_received = false;
        g_deferred_category_activation.discover_new_category = false;
        g_deferred_category_activation.discover_retry_count = 0;
        g_deferred_category_activation.tag_name_to_assign.clear();
        g_known_categories_before_extension_drop.clear();
        return;
      }
      else {
        if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
          printf("[CATEGORY ACTIVATE] Reserved-only switch FAILED (category may not have panels yet), will continue with normal activation...\n");
        }
      }
    }
    else {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE] Extension is NOT reserved-only, proceeding with normal activation\n");
        fflush(stdout);
      }
    }
  }

  std::string category_id = g_deferred_category_activation.category_id;

  /* Check if we need to wait for extension installation signal */
  if (g_deferred_category_activation.wait_for_extension_signal) {
    if (!g_deferred_category_activation.extension_signal_received) {
      bool discovered_without_signal = false;
      if (region && region->runtime && !g_known_categories_before_extension_drop.is_empty()) {
        for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
          if (pc_dyn.idname && pc_dyn.idname[0] &&
              !g_known_categories_before_extension_drop.contains(pc_dyn.idname))
          {
            discovered_without_signal = true;
            break;
          }
        }
      }

      if (discovered_without_signal) {
        g_deferred_category_activation.extension_signal_received = true;
      }
      else {
        const double wait_seconds = BLI_time_now_seconds() - g_deferred_category_activation.timestamp;
        if (wait_seconds > 2.0) {
          g_deferred_category_activation.extension_signal_received = true;
        }
      }
    }

    if (!g_deferred_category_activation.extension_signal_received) {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE] Waiting for extension installation signal for: '%s'\n",
               category_id.c_str());
      }
      return;
    }
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Extension installation signal received for: '%s', proceeding with activation\n",
             category_id.c_str());
    }

    /* Force immediate UI refresh right after extension install signal.
     * New panels/categories may already be registered but not drawn yet. */
    WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);
    WM_event_add_notifier(C, NC_WINDOW | ND_DRAW, nullptr);
    ED_region_tag_refresh_ui(region);
    ED_region_tag_redraw(region);
    category_tabs_tag_refresh_active_area_ui(C);
  }


  /* If in discover mode, find the new category that appeared after extension installation */
  if (g_deferred_category_activation.discover_new_category && category_id.empty()) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Discover mode: looking for new categories... (retry %d/30)\n",
             g_deferred_category_activation.discover_retry_count);
      printf("[CATEGORY ACTIVATE]   region=%p, region->runtime=%p\n",
             (void*)region, region ? (void*)region->runtime : nullptr);
      if (region && region->runtime) {
        printf("[CATEGORY ACTIVATE]   panels_category count: %d\n",
               BLI_listbase_count(&region->runtime->panels_category));
      }
      printf("[CATEGORY ACTIVATE]   known categories before drop: %zu\n",
             g_known_categories_before_extension_drop.size());
    }

    /* IMPORTANT: Search directly in region->runtime->panels_category, NOT through get_ordered_categories()!
     * get_ordered_categories() applies tag filtering via panel_category_is_visible_by_tags(),
     * which would hide new categories that don't have tags assigned yet.
     * New extension categories must be discoverable regardless of tag filter state. */
    std::string new_category_id;
    if (region && region->runtime) {
      for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
        if (pc_dyn.idname && pc_dyn.idname[0]) {
          if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
            printf("[CATEGORY ACTIVATE]   Checking category: '%s', known=%d\n",
                   pc_dyn.idname,
                   g_known_categories_before_extension_drop.contains(pc_dyn.idname) ? 1 : 0);
          }
          if (!g_known_categories_before_extension_drop.contains(pc_dyn.idname)) {
            new_category_id = pc_dyn.idname;
            if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
              printf("[CATEGORY ACTIVATE]   Found new category: '%s'\n", new_category_id.c_str());
            }
            break; /* Take the first new category */
          }
        }
      }
    }

    if (new_category_id.empty()) {
      /* Category not visible yet - may need more time for addon to register.
       * Retry with frame delay instead of giving up immediately. */
      ED_region_tag_refresh_ui(region);
      category_tabs_tag_refresh_active_area_ui(C);
      g_deferred_category_activation.discover_retry_count++;
      if (g_deferred_category_activation.discover_retry_count < 30) {
      g_deferred_category_activation.frame_delay = 3; /* Retry in 3 frames */
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE]   No new category found yet, will retry\n");
      }
      return;
    }
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   No new category found after timeout, clearing deferred activation\n");
    }
    g_deferred_category_activation.valid = false;
    g_deferred_category_activation.wait_for_extension_signal = false;
    g_deferred_category_activation.extension_signal_received = false;
    g_deferred_category_activation.discover_new_category = false;
    g_deferred_category_activation.discover_retry_count = 0;
    g_deferred_category_activation.tag_name_to_assign.clear();
    g_known_categories_before_extension_drop.clear();
    return;
  }

  category_id = new_category_id;
  g_deferred_category_activation.category_id = new_category_id;
  g_deferred_category_activation.wait_for_extension_signal = false; /* Signal already received */

  /* Mark this category as pending tag assignment via the DNA-backed mechanism.
   * This replaces the old g_new_extension_categories_visible set.
   * IMPORTANT: Use CURRENT space type where the category was discovered, not the space type
   * where the extension was dropped. This ensures categories are properly tagged
   * for the correct editor (e.g., Hot Node extension dropped in 3D Viewport but
   * category appears in Node Editor). */
  const ScrArea *current_area = CTX_wm_area(C);
  const int current_space_type = current_area ? current_area->spacetype : -1;
  register_new_extension_category(C,
                                  new_category_id.c_str(),
                                  g_deferred_category_activation.source_extension_id.c_str(),
                                  current_space_type,  // Use current space, not drop space
                                  g_deferred_category_activation.activation_mode_flag,
                                  g_deferred_category_activation.tag_already_assigned);
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE]   Registered '%s' as pending extension category\n", new_category_id.c_str());
    printf("[CATEGORY ACTIVATE]   Will activate new category: '%s'\n", category_id.c_str());
  }
  g_deferred_category_activation.discover_retry_count = 0; /* Reset retry counter */

  /* Auto-activate "New Add-ons!" filter to show the new category immediately.
   * Only activate if tag was not already assigned (i.e., category is truly new).
   * Only activate once - don't re-activate if filter was already auto-activated. */
  if (!g_deferred_category_activation.tag_already_assigned && current_area &&
      !is_new_addon_filter_auto_activated(current_area)) {
    set_new_addon_filter_active(const_cast<ScrArea *>(current_area), true, /*auto_activated=*/true);
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[NEW ADDON AUTO-ACTIVATE] Activated filter for discovered category: '%s', extension: '%s'\n",
             new_category_id.c_str(), g_deferred_category_activation.source_extension_id.c_str());
      fflush(stdout);
    }
  }
  
  /* IMPORTANT: Set the NEW category as active, not the first one in the list.
   * This ensures user sees the category they just added via extension install.
   * Do this AFTER filter activation check so category is set even if filter was already active. */
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[NEW ADDON AUTO-ACTIVATE] DEBUG: tag_already_assigned=%d, region=%p, new_category_id='%s', category_id='%s'\n",
           g_deferred_category_activation.tag_already_assigned ? 1 : 0,
           (void*)region,
           new_category_id.c_str(),
           category_id.c_str());
    fflush(stdout);
  }
  if (!g_deferred_category_activation.tag_already_assigned && region && 
      !category_id.empty()) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[NEW ADDON AUTO-ACTIVATE] Setting active category to: '%s'\n", category_id.c_str());
      fflush(stdout);
    }
    panel_category_active_set(region, category_id.c_str());
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[NEW ADDON AUTO-ACTIVATE] Successfully set active category: '%s'\n", category_id.c_str());
      fflush(stdout);
    }
  }
  else {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[NEW ADDON AUTO-ACTIVATE] SKIPPED: tag_already_assigned=%d, region=%p, category_id.empty()=%d\n",
             g_deferred_category_activation.tag_already_assigned ? 1 : 0,
             (void*)region,
             category_id.empty() ? 1 : 0);
      fflush(stdout);
    }
  }
  
  /* NEW: Check if this is a reserved-only extension and switch to reserved category instead.
   * This handles cases like Bool Tool, which creates panels only in reserved categories.
   * For such extensions, we automatically switch to the reserved category instead of
   * showing the "New Add-ons!" button. */
  const wmWindowManager *wm = CTX_wm_manager(C);
  if (wm && blender::ui::extension_has_only_reserved_categories(
                wm, g_deferred_category_activation.source_extension_id.c_str()))
  {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE] Reserved-only extension detected, switching to reserved category...\n");
    }
    const int space_type = current_area ? current_area->spacetype : -1;
    if (switch_to_reserved_category_for_extension(
            C, region, g_deferred_category_activation.source_extension_id.c_str(), space_type))
    {
      /* Successfully switched to reserved category - clear deferred activation.
       * Don't show "New Add-ons!" for reserved-only extensions. */
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE] Reserved-only switch completed, clearing deferred activation\n");
      }
      g_deferred_category_activation.valid = false;
      g_deferred_category_activation.discover_new_category = false;
      g_deferred_category_activation.discover_retry_count = 0;
      g_deferred_category_activation.tag_name_to_assign.clear();
      g_known_categories_before_extension_drop.clear();
      return;
    }
  }
}

/* Check frame delay - wait N frames before activating */
if (g_deferred_category_activation.frame_delay > 0) {
  g_deferred_category_activation.frame_delay--;
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] Frame delay: %d remaining for: '%s'\n",
           g_deferred_category_activation.frame_delay,
           category_id.c_str());
  }
  return;
}

if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
  printf("[CATEGORY ACTIVATE] Executing deferred activation for: '%s'\n", category_id.c_str());
}

/* Verify category still exists in panels_category.
 * NOTE: We check existence directly in region->runtime->panels_category instead of using
 * panel_category_is_visible_by_tags() because new extension categories may not have
 * tags assigned yet, which would cause them to be filtered out when tag filter is active.
 * Extension categories should be activatable regardless of current tag filter state. */
bool category_exists = false;
for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
  if (STREQ(pc_dyn.idname, category_id.c_str())) {
    category_exists = true;
    break;
  }
}
if (!category_exists) {
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE]   Category no longer exists, skipping\n");
  }
  g_deferred_category_activation.valid = false;
  g_deferred_category_activation.discover_new_category = false;
  g_deferred_category_activation.discover_retry_count = 0;
  g_deferred_category_activation.tag_name_to_assign.clear();
  g_known_categories_before_extension_drop.clear();
  return;
}

/* Check if already active */
const char *current_active = panel_category_active_get(region, false);
if (current_active && STREQ(category_id.c_str(), current_active)) {
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE]   Category already active, skipping\n");
  }
  g_deferred_category_activation.valid = false;
  g_deferred_category_activation.discover_new_category = false;
  g_deferred_category_activation.discover_retry_count = 0;
  g_deferred_category_activation.tag_name_to_assign.clear();
  g_known_categories_before_extension_drop.clear();
  return;
}

/* Perform activation */
if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
  printf("[CATEGORY ACTIVATE]   Setting active category to: '%s'\n", category_id.c_str());
}
panel_category_active_set(region, category_id.c_str());
if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
  printf("[CATEGORY ACTIVATE]   panel_category_active_set completed\n");
}

/* Save to tag category memory */
if (!g_deferred_category_activation.tag_key.empty()) {
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE]   Saving to tag memory, tag_key: '%s'\n",
           g_deferred_category_activation.tag_key.c_str());
  }
  blender::ui::tag_save_last_active_category(
      const_cast<bContext *>(C),
      g_deferred_category_activation.tag_key.c_str(),
      category_id.c_str());
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE]   tag_save_last_active_category completed\n");
  }
}

  /* Assign deferred tag if one was saved from drag & drop on tabs.
   * This happens when an extension was dropped onto a tab with an active tag filter.
   * The category didn't exist yet, so we deferred tag assignment until now. */
  if (!g_deferred_category_activation.tag_name_to_assign.empty()) {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   Assigning deferred tag '%s' to category '%s'\n",
             g_deferred_category_activation.tag_name_to_assign.c_str(),
             category_id.c_str());
      fflush(stdout);
    }

#ifdef WITH_PYTHON
    /* Escape category_id and tag_name for safe embedding in a Python string literal. */
    char esc_cat[512];
    {
      int j = 0;
      for (int i = 0; category_id[i] != '\0' && j < int(sizeof(esc_cat)) - 1; i++) {
        const char c = category_id[i];
        if (c == '\\' || c == '\'') {
          if (j + 1 < int(sizeof(esc_cat)) - 1) {
            esc_cat[j++] = '\\';
            esc_cat[j++] = c;
          }
        }
        else {
          esc_cat[j++] = c;
        }
      }
      esc_cat[j] = '\0';
    }

    const std::string &tag_names = g_deferred_category_activation.tag_name_to_assign;

    /* Split multiple tags (may be separated by comma or semicolon) and assign each one. */
    Vector<std::string> tag_list;
    category_tab_split_tags(tag_names.c_str(), tag_list, ",;");

    const ScrArea *area = CTX_wm_area(C);
    const int space_type = area ? area->spacetype : -1;

    for (const std::string &single_tag : tag_list) {
      /* Escape the tag name for safe embedding in a Python string literal. */
      char esc_tag[256];
      {
        int j = 0;
        for (size_t i = 0; i < single_tag.size() && j < int(sizeof(esc_tag)) - 1; i++) {
          const char c = single_tag[i];
          if (c == '\\' || c == '\'') {
            if (j + 1 < int(sizeof(esc_tag)) - 1) {
              esc_tag[j++] = '\\';
              esc_tag[j++] = c;
            }
          }
          else {
            esc_tag[j++] = c;
          }
        }
        esc_tag[j] = '\0';
      }

      char python_expr[1280];
      SNPRINTF(python_expr,
               "__import__('bl_ui.space_userpref', fromlist=[''])."
               "assign_tag_to_category('%s', '%s', %d)",
               esc_cat,
               esc_tag,
               space_type);

      const char *imports_none[] = {nullptr};
      BPY_run_string_exec(const_cast<bContext *>(C), imports_none, python_expr);
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE]   Assigned tag '%s' to category '%s'\n",
               single_tag.c_str(),
               category_id.c_str());
        fflush(stdout);
      }
    }

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   Deferred tag assignment completed (%zu tags)\n", tag_list.size());
      fflush(stdout);
    }

    WM_event_add_notifier(const_cast<bContext *>(C), NC_WM | ND_CATEGORY_GLYPHS, nullptr);
    if (area) {
      ED_area_tag_redraw(const_cast<ScrArea *>(area));
    }

    /* Auto-deactivate "New Add-ons!" filter if it was auto-activated.
     * This ensures the filter is turned off after the category gets its tag. */
    if (area && is_new_addon_filter_auto_activated(area)) {
      set_new_addon_filter_active(const_cast<ScrArea *>(area), false);
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[NEW ADDON AUTO-DEACTIVATE] Filter deactivated after tag assignment for category '%s'\n",
               category_id.c_str());
        fflush(stdout);
      }
    }
#else
    UNUSED_VARS(C);
#endif
    g_deferred_category_activation.tag_name_to_assign.clear();
  }

  /* Apply pending category insert position to JSON order if available.
   * This ensures the new category is inserted at the correct position (between anchors)
   * rather than being appended to the end.
   *
   * Note: g_pending_category_insert.tag_key has format "VIEW3D:AAA" while
   * g_deferred_category_activation.tag_key has format "AAA", so we check if
   * the pending key ends with the deferred key (after a colon separator). */
  bool tag_keys_match = false;

  /* Debug: print state for diagnosis */
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE]   Checking pending insert: pending_insert_valid=%d, pending_tag_key='%s', deferred_tag_key='%s'\n",
           g_deferred_category_activation.pending_insert_valid ? 1 : 0,
           g_deferred_category_activation.pending_insert_tag_key.c_str(),
           g_deferred_category_activation.tag_key.c_str());
    fflush(stdout);
  }

  if (g_deferred_category_activation.pending_insert_valid && !g_deferred_category_activation.tag_key.empty()) {
    const std::string &pending_key = g_deferred_category_activation.pending_insert_tag_key;
    const std::string &deferred_key = g_deferred_category_activation.tag_key;
    /* Check if pending_key ends with ":" + deferred_key or equals deferred_key */
    if (pending_key == deferred_key) {
      tag_keys_match = true;
    }
    else {
      size_t colon_pos = pending_key.rfind(':');
      if (colon_pos != std::string::npos) {
        std::string pending_tag = pending_key.substr(colon_pos + 1);
        if (pending_tag == deferred_key) {
          tag_keys_match = true;
        }
      }
    }
  }
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE]   tag_keys_match=%d\n", tag_keys_match ? 1 : 0);
  }
  fflush(stdout);

  if (tag_keys_match)
  {
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY ACTIVATE]   Applying pending insert position for '%s' (pending_key='%s', deferred_key='%s')\n",
             category_id.c_str(),
             g_deferred_category_activation.pending_insert_tag_key.c_str(),
             g_deferred_category_activation.tag_key.c_str());
    }

    /* Load current order from JSON */
    Vector<std::string> current_order = load_category_order_from_json(
        C, g_deferred_category_activation.pending_insert_tag_key.c_str());

    /* Check if category is already in order */
    bool category_in_order = false;
    for (const auto &cat : current_order) {
      if (cat == category_id) {
        category_in_order = true;
        break;
      }
    }

    if (!category_in_order) {
      /* Find insert position based on anchors */
      int insert_index = -1;

      /* Try to find position relative to anchor_after (insert before this) */
      if (!g_deferred_category_activation.pending_insert_anchor_after.empty()) {
        for (int i = 0; i < current_order.size(); i++) {
          if (current_order[i] == g_deferred_category_activation.pending_insert_anchor_after) {
            insert_index = i;
            break;
          }
        }
      }

      /* If not found, try anchor_before (insert after this) */
      if (insert_index == -1 && !g_deferred_category_activation.pending_insert_anchor_before.empty()) {
        for (int i = 0; i < current_order.size(); i++) {
          if (current_order[i] == g_deferred_category_activation.pending_insert_anchor_before) {
            insert_index = i + 1;
            break;
          }
        }
      }

      /* If still not found, try target_category */
      if (insert_index == -1 && !g_deferred_category_activation.pending_insert_target_category.empty()) {
        for (int i = 0; i < current_order.size(); i++) {
          if (current_order[i] == g_deferred_category_activation.pending_insert_target_category) {
            insert_index = g_deferred_category_activation.pending_insert_insert_above ? i : i + 1;
            break;
          }
        }
      }

      /* Insert category at the determined position */
      if (insert_index >= 0 && insert_index <= current_order.size()) {
        current_order.insert(insert_index, category_id);
        if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
          printf("[CATEGORY ACTIVATE]     Inserted '%s' at index %d (between '%s' and '%s')\n",
                 category_id.c_str(),
                 insert_index,
                 g_deferred_category_activation.pending_insert_anchor_before.c_str(),
                 g_deferred_category_activation.pending_insert_anchor_after.c_str());
        }
      }
      else {
        /* Fallback: append to end */
        current_order.append(category_id);
        if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
          printf("[CATEGORY ACTIVATE]     Appended '%s' to end (no anchor found)\n",
                 category_id.c_str());
        }
      }

      /* Save updated order to JSON */
      save_category_order_to_json(C, g_deferred_category_activation.pending_insert_tag_key.c_str(), current_order);
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY ACTIVATE]     Order saved to JSON\n");
      }
    }

    /* Clear pending insert (in deferred activation) */
    g_deferred_category_activation.pending_insert_valid = false;
  }

  /* Clear deferred activation */
  g_deferred_category_activation.valid = false;
  g_deferred_category_activation.wait_for_extension_signal = false;
  g_deferred_category_activation.extension_signal_received = false;
  g_deferred_category_activation.discover_new_category = false;
  g_deferred_category_activation.discover_retry_count = 0;
  g_deferred_category_activation.tag_name_to_assign.clear();
  g_deferred_category_activation.pending_insert_valid = false;
  g_known_categories_before_extension_drop.clear();
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CATEGORY ACTIVATE] Deferred activation completed\n");
  }
}

void panel_category_tabs_draw_all(const bContext *C, ARegion *region, const char *category_id_active)
{
  /* Execute deferred category activation if pending.
   * This is a safe point to activate categories after panel layout is complete. */
  deferred_category_activation_execute(C, region);

  const CategoryTabsDrawContext draw_ctx = category_tabs_draw_context_build(C, region);
  const int space_type = draw_ctx.space_type;
  const bool is_left = draw_ctx.is_left;
  View2D *v2d = draw_ctx.v2d;
  const uiFontStyle *fstyle = draw_ctx.fstyle;
  fontstyle_set(fstyle);
  const int fontid = draw_ctx.fontid;
  float fstyle_points = draw_ctx.fstyle_points;
  const float aspect = draw_ctx.aspect;

  CategoryDragState *drag_state = static_cast<CategoryDragState *>(
      region->runtime->category_tabs_drag_state);
  const bool is_dragging = (drag_state != nullptr && drag_state->is_dragging);
  const char *drag_category_id = is_dragging ? drag_state->drag_category_id : "";

  const eUserPref_CategoryTabsDisplayMode display_mode = draw_ctx.display_mode;
  const float category_tabs_zoom = draw_ctx.category_tabs_zoom;
  const float zoom = draw_ctx.zoom;
  const wmWindowManager *wm = draw_ctx.wm;
  const int px = draw_ctx.px;
  const int category_tabs_width = draw_ctx.category_tabs_width;
  const bool too_narrow = draw_ctx.too_narrow;
  const int tab_v_pad_text = draw_ctx.tab_v_pad_text;
  const int tab_v_pad = draw_ctx.tab_v_pad;

  /* Update drag_state->tab_v_pad during drag to ensure correct shift calculations.
   * This must be done before the draw loop because calculate_insert_index and
   * update_insert_zone depend on this value. */
  if (is_dragging && drag_state != nullptr) {
    const int prev_tab_v_pad = drag_state->tab_v_pad;
    drag_state->tab_v_pad = tab_v_pad;

    /* Update insert zone when tab_v_pad changes (first frame of drag) */
    if (prev_tab_v_pad != tab_v_pad) {
      update_insert_zone(C, wm, region, drag_state);
    }
  }

  bTheme *btheme = theme::theme_get();
  const float tab_curve_radius = btheme->tui.wcol_tab.roundness * U.widget_unit * zoom;
  const int roundboxtype = region->overlap ? CNR_ALL :
                                             (is_left ? (CNR_TOP_LEFT | CNR_BOTTOM_LEFT) :
                                                        (CNR_TOP_RIGHT | CNR_BOTTOM_RIGHT));
  bool is_alpha;

  const int rct_xmin = is_left ? v2d->mask.xmin + 3 : (v2d->mask.xmax - category_tabs_width);
  const int rct_xmax = is_left ? v2d->mask.xmin + category_tabs_width : (v2d->mask.xmax - 3);
  const int square_size = rct_xmax - rct_xmin;

  int y_ofs = tab_v_pad;

  uchar theme_col_back[4];
  uchar theme_col_tab_bg[4];
  uchar theme_col_tab_text[3];
  uchar theme_col_tab_text_sel[3];
  float theme_col_tab_active[4];
  float theme_col_tab_inactive[4];
  float theme_col_tab_outline[4];
  float theme_col_tab_outline_sel[4];
  float theme_col_selection[4];

  theme::get_color_4ubv(TH_BACK, theme_col_back);
  theme::get_color_3ubv(TH_TAB_TEXT, theme_col_tab_text);
  theme::get_color_3ubv(TH_TAB_TEXT_HI, theme_col_tab_text_sel);
  theme::get_color_4ubv(TH_TAB_BACK, theme_col_tab_bg);
  theme::get_color_4fv(TH_TAB_ACTIVE, theme_col_tab_active);
  theme::get_color_4fv(TH_TAB_INACTIVE, theme_col_tab_inactive);
  theme::get_color_4fv(TH_TAB_OUTLINE, theme_col_tab_outline);
  theme::get_color_4fv(TH_TAB_OUTLINE_ACTIVE, theme_col_tab_outline_sel);
  theme::get_color_4fv(TH_TAB_ICON_SELECTION, theme_col_selection);

  is_alpha = (region->overlap && (theme_col_back[3] != 255));

  fontscale(&fstyle_points, aspect);
  BLF_size(fontid, fstyle_points * UI_SCALE_FAC * category_tabs_zoom);

  if (BKE_regiontype_uses_category_tabs(region->runtime->type)) {
    BLI_assert(panel_category_is_visible(region));
  }

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

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    rcti *rct = &pc_dyn.rect;
    const CategoryTabRenderData render_data = category_tab_render_data_build(
        wm, region, pc_dyn.idname, space_type);
    const char *category_id = render_data.category_id;
    const char *category_id_draw = render_data.category_id_draw;
    const char *category_id_draw_label = render_data.category_id_draw_label;
    /* When panel is minimized (too_narrow), the active tab should not expand */
    const bool is_active = !too_narrow && category_id_active && STREQ(category_id, category_id_active);

    bool is_fallback_letter = false;
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    const char *glyph = panel_category_glyph_lookup(
        wm, category_id, nullptr, &is_fallback_letter, glyph_color, space_type);
    if (category_tab_edit_dialog_is_open_for_category(category_id)) {
      if (category_tab_preview_glyph[0] != '\0') {
        glyph = category_tab_preview_glyph;
        is_fallback_letter = (category_tab_preview_first_letter[0] != '\0') &&
                             STREQ(category_tab_preview_glyph, category_tab_preview_first_letter);
      }
      copy_v3_v3(glyph_color, category_tab_preview_color);
    }

    /* Handle nullptr glyph (explicitly cleared) - use fallback letter from category */
    char fallback_glyph_buf[8];
    if (glyph == nullptr && is_fallback_letter) {
      /* For glyph-id categories use human-readable name (panel label/display name),
       * otherwise use category id. */
      const char *first_letter_source = category_first_letter_source_name_get(
          region, wm, category_id, category_id_draw, space_type);
      const int first_char_size = BLI_str_utf8_size_safe(first_letter_source);
      if (first_char_size > 0) {
        memcpy(fallback_glyph_buf, first_letter_source, first_char_size);
        fallback_glyph_buf[first_char_size] = '\0';
        glyph = fallback_glyph_buf;
      }
      else {
        /* Fallback to category_id if we can't extract first char */
        glyph = category_id;
      }
    }

    const bool has_glyph = is_single_glyph_str(glyph) && !is_fallback_letter;

    const CategoryTabMixedContentFlags mixed_content_flags = category_tab_mixed_content_flags_resolve(
        wm, category_id, display_mode, space_type, has_glyph, is_fallback_letter);
    const bool mixed_mode_has_visible_glyph_content =
        mixed_content_flags.mixed_mode_has_visible_glyph_content;

    const bool use_reserved_inactive_icon_only =
        U.category_tabs_hide_reserved_inactive_text && !is_active &&
        ELEM(display_mode, USER_CATEGORY_TABS_GLYPHS_TEXT, USER_CATEGORY_TABS_TEXT_ONLY) &&
        category_is_reserved_for_reorder(wm, category_id) && has_glyph;

    int category_width;
    int current_tab_v_pad_text = tab_v_pad_text;

    const int glyph_h = int(std::floor(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX)));

    switch (display_mode) {
      case USER_CATEGORY_TABS_GLYPHS_ONLY: {
        /* Use glyph height (without rotation) for consistent sizing with GLYPHS_TEXT mode. */
        category_width = glyph_h;

        const CategoryTabNameLayout name_layout = category_tab_name_layout_resolve(
            region, category_id, display_mode, is_active, true, too_narrow, tab_v_pad_text);
        const bool should_expand_name = name_layout.should_expand_name;
        current_tab_v_pad_text = name_layout.tab_v_pad_text;

        if (current_tab_v_pad_text == 0) {
          /* Box shape without name: use square size with no padding */
          category_width = square_size + 3.0; // Add padding for Box shape(No change value 3.0!)
        }

        if (should_expand_name) {
          /* Get text width for name expansion */
          const char *text_for_name = category_id_draw_label;
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          const int text_w = round_fl_to_int(
              BLF_width(fontid, text_for_name, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
          const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC *
                                                     zoom);

          /* Expand tab width to show name */
          category_width = glyph_h + text_w + glyph_text_gap;
        }
        break;
      }

      case USER_CATEGORY_TABS_GLYPHS_TEXT: {
        if (use_reserved_inactive_icon_only) {
          category_width = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
          break;
        }

        const char *text_for_width = category_id_draw_label;
        if (mixed_mode_has_visible_glyph_content) {
          const int glyph_h = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          const int text_w = round_fl_to_int(BLF_width(fontid, text_for_width, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
          const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC * zoom);
          category_width = glyph_h + text_w + glyph_text_gap;
        }
        else {
          /* --- BEGIN: MIXED_MODE_CONTENT_FLAGS (text-only sizing when glyph disabled) --- */
          /* No glyph content visible - use text-only sizing.
           * For single-glyph categories, look up panel label (same as TEXT_ONLY mode).
           * To remove: replace this block with original code:
           *   if (is_single_glyph_str(category_id_draw)) {
           *     category_width = round_fl_to_int(BLF_height(fontid, category_id_draw, ...));
           *   } else {
           *     BLF_enable(fontid, BLF_ROTATION); ... category_width = BLF_width(...);
           *   }
           */
          const char *text_for_size = category_id_draw_label;
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          category_width = round_fl_to_int(
              BLF_width(fontid, text_for_size, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
          /* --- END: MIXED_MODE_CONTENT_FLAGS (text-only sizing when glyph disabled) --- */
        }
        break;
      }

      case USER_CATEGORY_TABS_TEXT_ONLY:
      default: {
        if (use_reserved_inactive_icon_only) {
          category_width = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
          break;
        }

        const char *text_for_size = category_id_draw_label;
        BLF_enable(fontid, BLF_ROTATION);
        BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
        category_width = round_fl_to_int(BLF_width(fontid, text_for_size, BLF_DRAW_STR_DUMMY_MAX));
        BLF_disable(fontid, BLF_ROTATION);
        break;
      }
    }

    rct->xmin = rct_xmin;
    rct->xmax = rct_xmax;
    rct->ymin = v2d->mask.ymax - (y_ofs + category_width + (current_tab_v_pad_text * 2));
    rct->ymax = v2d->mask.ymax - (y_ofs);
    y_ofs += category_width + tab_v_pad + (current_tab_v_pad_text * 2);
  }

  const int settings_icon_height = round_fl_to_int(BLF_height(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX));
  const int settings_button_height = settings_icon_height + (tab_v_pad_text * 2);
  rcti *settings_rct = &region->runtime->category_tabs_settings_rect;
  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    settings_rct->xmin = rct_xmin;
    settings_rct->xmax = rct_xmax;
    settings_rct->ymin = v2d->mask.ymax - (y_ofs + settings_button_height);
    settings_rct->ymax = v2d->mask.ymax - y_ofs;
  }

  const int total_content_height = y_ofs + settings_button_height + tab_v_pad;
  const int max_scroll = max_ii(total_content_height - BLI_rcti_size_y(&v2d->mask), 0);
  const int scroll = clamp_i(region->category_scroll, 0, max_scroll);

  region->category_scroll = scroll;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    rcti *rct = &pc_dyn.rect;
    rct->ymin += scroll;
    rct->ymax += scroll;
  }

  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    settings_rct->ymin += scroll;
    settings_rct->ymax += scroll;
  }

  GPU_line_smooth(true);

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  if (is_alpha) {
    GPU_blend(GPU_BLEND_ALPHA);
    immUniformColor4ubv(theme_col_tab_bg);
  }
  else {
    immUniformColor3ubv(theme_col_tab_bg);
  }

  if (is_left) {
    immRectf(pos, v2d->mask.xmin, v2d->mask.ymin, v2d->mask.xmin + category_tabs_width, v2d->mask.ymax);
  }
  else {
    immRectf(pos, v2d->mask.xmax - category_tabs_width, v2d->mask.ymin, v2d->mask.xmax + 1, v2d->mask.ymax);
  }

  if (is_alpha) {
    GPU_blend(GPU_BLEND_NONE);
  }

  immUnbindProgram();

  /* too_narrow was calculated earlier for use in width calculation */

  DeferredHoverTabDrawData deferred_hover_tab_draw;
  const CategoryTabSingleDrawContext single_draw_ctx = category_tab_single_draw_context_build(
      region,
      wm,
      v2d,
      rct_xmin,
      rct_xmax,
      is_left,
      is_dragging,
      display_mode,
      tab_curve_radius,
      roundboxtype,
      px,
      theme_col_tab_active,
      theme_col_tab_inactive,
      theme_col_tab_outline,
      theme_col_tab_outline_sel,
      fontid,
      fstyle,
      fstyle_points,
      zoom,
      category_tabs_zoom,
      theme_col_tab_text,
      theme_col_tab_text_sel,
      too_narrow,
      space_type);

  int current_display_index = 0;

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;

    if (is_dragging && !drag_state->is_reserved && STREQ(pc_dyn.idname, drag_category_id)) {
      current_display_index++;
      continue;
    }

    const int y_shift = category_tab_y_shift_resolve(
        region, ordered_categories, pc_dyn, is_dragging, drag_state, current_display_index, tab_v_pad);

    rcti shifted_rect = pc_dyn.rect;
    shifted_rect.ymin += y_shift;
    shifted_rect.ymax += y_shift;

    const rcti *rct = &shifted_rect;

    if (rct->ymin > v2d->mask.ymax) {
      current_display_index++;
      continue;
    }
    if (rct->ymax < v2d->mask.ymin) {
      break;
    }
    const CategoryTabLoopDrawData draw_data = category_tab_loop_draw_data_build(wm,
                                                                                 region,
                                                                                 rct,
                                                                                 pc_dyn.idname,
                                                                                 category_id_active,
                                                                                 display_mode,
                                                                                 tab_v_pad_text,
                                                                                 too_narrow,
                                                                                 mouse_x,
                                                                                 mouse_y,
                                                                                 space_type);
    current_display_index++;
    category_tab_draw_dispatch(single_draw_ctx, rct, draw_data, deferred_hover_tab_draw);

    if (is_left) {
      pc_dyn.rect.xmin = v2d->mask.xmin;
    }
    else {
      pc_dyn.rect.xmax = v2d->mask.xmax;
    }
  }

  category_tab_draw_deferred_hover_flush(single_draw_ctx, deferred_hover_tab_draw);

  category_tab_settings_draw_with_preview_shift(
      C, region, v2d, ordered_categories, is_dragging, zoom, theme_col_tab_text);

  /* Draw the dragged tab at cursor position and ghost tab at insert position */
  if (is_dragging && !drag_state->is_reserved) {
    category_tab_draw_dragged_with_reorder_ghost(region,
                                                 wm,
                                                 ordered_categories,
                                                 drag_state,
                                                 drag_category_id,
                                                 category_id_active,
                                                 display_mode,
                                                 is_left,
                                                 tab_v_pad,
                                                 tab_v_pad_text,
                                                 roundboxtype,
                                                 tab_curve_radius,
                                                 fontid,
                                                 fstyle,
                                                 fstyle_points,
                                                 zoom,
                                                 category_tabs_zoom,
                                                 theme_col_tab_text,
                                                 theme_col_tab_text_sel,
                                                 theme_col_tab_active,
                                                 theme_col_tab_outline_sel,
                                                 too_narrow,
                                                 space_type);
  }

  const char *ghost_skip_reason = category_tab_extension_drop_ghost_draw(region,
                                                                          v2d,
                                                                          ordered_categories,
                                                                          is_dragging,
                                                                          rct_xmin,
                                                                          rct_xmax,
                                                                          roundboxtype,
                                                                          tab_curve_radius,
                                                                          theme_col_tab_active,
                                                                          theme_col_tab_outline_sel);

  if (!is_dragging && region->runtime && region->runtime->extension_drop_preview_state.active) {
    const ui::ExtensionDropPreviewState &preview = region->runtime->extension_drop_preview_state;
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      if (ghost_skip_reason) {
        printf("[EXT_GHOST] SKIPPED: reason=%s active=%d target='%s' idx=%d cursor_y=%d\n",
               ghost_skip_reason,
               preview.active ? 1 : 0,
               preview.target_category_id,
               preview.target_index,
               preview.cursor_y);
      }
    }
  }

  GPU_line_smooth(false);
  BLF_disable(fontid, BLF_ROTATION);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hover Handler
 * \{ */

static int screen_category_tabs_hover_handler(bContext *C, const wmEvent *event, void * /*userdata*/)
{
  if (!ISMOUSE_MOTION(event->type)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  bScreen *screen = CTX_wm_screen(C);
  if (!screen) {
    return WM_UI_HANDLER_CONTINUE;
  }

  wmWindow *win = CTX_wm_window(C);
  if (!win || !win->runtime->eventstate) {
    return WM_UI_HANDLER_CONTINUE;
  }

  const int mouse_x = win->runtime->eventstate->xy[0];
  const int mouse_y = win->runtime->eventstate->xy[1];
  const double current_time = BLI_time_now_seconds();

  for (ScrArea &area_check : screen->areabase) {
    for (ARegion &region_check : area_check.regionbase) {
      if (!panel_category_tabs_is_visible(&region_check)) {
        continue;
      }

      const bool mouse_in_this_region = BLI_rcti_isect_pt(&region_check.winrct, mouse_x, mouse_y);

      if (region_check.runtime->category_tabs_settings_hover) {
        if (!mouse_in_this_region) {
          const double time_since_click = current_time -
                                          region_check.runtime->category_tabs_settings_click_time;
          if (time_since_click > 0.5) {
            region_check.runtime->category_tabs_settings_hover = false;
            ED_region_tag_redraw(&region_check);
          }
        }
        else {
          const int mx_local = mouse_x - region_check.winrct.xmin;
          const int my_local = mouse_y - region_check.winrct.ymin;
          const bool mouse_over_button = BLI_rcti_isect_pt(
              &region_check.runtime->category_tabs_settings_rect, mx_local, my_local);

          if (!mouse_over_button) {
            const double time_since_click = current_time -
                                            region_check.runtime->category_tabs_settings_click_time;
            if (time_since_click > 0.5) {
              region_check.runtime->category_tabs_settings_hover = false;
              ED_region_tag_redraw(&region_check);
            }
          }
        }
      }

      if (mouse_in_this_region) {
        const int mx_local = mouse_x - region_check.winrct.xmin;
        const int my_local = mouse_y - region_check.winrct.ymin;

        bool is_over_any_tab = false;
        for (const PanelCategoryDyn &pc_dyn : region_check.runtime->panels_category) {
          if (BLI_rcti_isect_pt(&pc_dyn.rect, mx_local, my_local)) {
            is_over_any_tab = true;
            break;
          }
        }

        if (!is_over_any_tab) {
          ED_region_tag_redraw(&region_check);
        }
      }
      else {
        ED_region_tag_redraw(&region_check);
      }
    }
  }

  return WM_UI_HANDLER_CONTINUE;
}

static void screen_category_tabs_hover_handler_remove(bContext * /*C*/, void * /*userdata*/)
{
  /* Nothing to clean up */
}

void screen_category_tabs_hover_handler_add(ListBaseT<wmEventHandler> *handlers)
{
  WM_event_remove_ui_handler(handlers,
                             screen_category_tabs_hover_handler,
                             screen_category_tabs_hover_handler_remove,
                             nullptr,
                             false);
  WM_event_add_ui_handler(
      nullptr,
      handlers,
      screen_category_tabs_hover_handler,
      screen_category_tabs_hover_handler_remove,
      nullptr,
      eWM_EventHandlerFlag(0));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Show Active Tab
 * \{ */

int ui_panel_category_show_active_tab(const ScrArea *area, ARegion *region, const int mval[2])
{
  if (!ED_region_panel_category_gutter_isect_xy(area, region, mval)) {
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag Operator
 * \{ */

/* Forward declaration */
static void category_tab_drag_cancel(bContext *C, wmOperator *op);

static bool category_tab_drag_poll(bContext *C)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return false;
  }
  if (!panel_category_tabs_is_visible(region)) {
    return false;
  }
  /* Drag & drop is always allowed, independent of Allow Edit Category Data setting. */
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

  /* Check if reserved (cannot be reordered). */
  const wmWindowManager *wm = CTX_wm_manager(C);
  bool is_reserved_glyph = category_is_reserved_for_reorder(wm, clicked_pc->idname);

  /* Initialize drag state */
  CategoryDragState *state = MEM_new<CategoryDragState>(__func__);
  state->is_dragging = true;
  state->is_reserved = is_reserved_glyph;

  if (is_reserved_glyph) {
    /* Create persistent tooltip with proper alignment to avoid overlapping tabs. */
    char msg[128];
    SNPRINTF(msg, "%s (Cannot Reorder)", IFACE_(clicked_pc->idname));

    /* Use the same positioning logic as hover tooltips. */
    const bool is_left = (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT);

    /* Position tooltip to avoid overlapping the tab.
     * Convert tab rect from region-local to screen coordinates. */
    rcti tab_rect_screen;
    tab_rect_screen.xmin = region->winrct.xmin + clicked_pc->rect.xmin;
    tab_rect_screen.xmax = region->winrct.xmin + clicked_pc->rect.xmax;
    tab_rect_screen.ymin = event->xy[1] - UI_UNIT_Y / 2;
    tab_rect_screen.ymax = event->xy[1] + UI_UNIT_Y / 2;

    int position[2];
    if (is_left) {
      position[0] = tab_rect_screen.xmax + UI_POPUP_MARGIN;
    }
    else {
      position[0] = tab_rect_screen.xmin - UI_POPUP_MARGIN;
    }
    position[1] = event->xy[1];

    const bool prefer_left = !is_left;
    state->tooltip_region = tooltip_create_from_text(
        C, msg, position, &tab_rect_screen, prefer_left);
    /* Store initial X position and dimensions for tooltip management during drag. */
    state->tooltip_initial_x = state->tooltip_region->winrct.xmin;
    state->tooltip_width = BLI_rcti_size_x(&state->tooltip_region->winrct);
    state->tooltip_height = BLI_rcti_size_y(&state->tooltip_region->winrct);
  }
  else {
    /* Check if we need to show category name tooltip for Icon mode.
     * Show tooltip in Icon mode when:
     * - Show Drag Tooltips is enabled
     * - AND tab name is not visible (not active AND not previous active)
     * This helps users identify tabs when only icons are visible. */
    const eUserPref_CategoryTabsDisplayMode display_mode = ED_category_tabs_display_mode_get(
        CTX_wm_area(C));
    const bool is_active = STREQ(clicked_pc->idname, region->runtime->category);
    const bool is_previous_active = (region->runtime->category_tabs_previous_active_id[0] != '\0' &&
                                     STREQ(clicked_pc->idname, region->runtime->category_tabs_previous_active_id));

    /* Determine if the tab name is visible (no tooltip needed when name is shown).
     * Show name for active tab OR for the previous active tab (in collapsed state). */
    const bool show_tab_name = (U.category_tabs_show_active_name &&
                                !region->runtime->category_tabs_active_name_hidden &&
                                (is_active || is_previous_active));

    if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
        U.category_tabs_show_drag_tooltips &&
        !show_tab_name)
    {
      /* Get category display name using the same method as hover tooltips. */
      const char *category_display_name = panel_category_tooltip_name_get(region, wm, clicked_pc->idname);
      if (category_display_name && category_display_name[0]) {
        /* Create tooltip with proper alignment to avoid overlapping tabs.
         * Uses the same positioning logic as hover tooltips. */
        const bool is_left = (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT);

        /* Position tooltip to avoid overlapping the tab.
         * Convert tab rect from region-local to screen coordinates.
         * Use mouse Y position to keep tooltip aligned with cursor vertically. */
        rcti tab_rect_screen;
        tab_rect_screen.xmin = region->winrct.xmin + clicked_pc->rect.xmin;
        tab_rect_screen.xmax = region->winrct.xmin + clicked_pc->rect.xmax;
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

        /* For tabs on right side, prefer left side positioning first. */
        const bool prefer_left = !is_left;
        state->tooltip_region = tooltip_create_from_text(
            C, IFACE_(category_display_name), position, &tab_rect_screen, prefer_left);
        /* Store initial X position and dimensions for tooltip management during drag. */
        state->tooltip_initial_x = state->tooltip_region->winrct.xmin;
        state->tooltip_width = BLI_rcti_size_x(&state->tooltip_region->winrct);
        state->tooltip_height = BLI_rcti_size_y(&state->tooltip_region->winrct);
      }
    }
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
  state->min_insert_index = state->original_index;
  state->max_insert_index = state->original_index;

  calculate_drag_insert_bounds(wm,
                               ordered_categories,
                               state->drag_category_id,
                               state->is_reserved,
                               &state->min_insert_index,
                               &state->max_insert_index);

  state->current_insert_index = clamp_i(
      state->current_insert_index, state->min_insert_index, state->max_insert_index);

  state->drag_offset_y = 0.0f;
  state->prev_drag_offset_y = 0.0f;
  state->initial_scroll = region->category_scroll;
  state->tab_v_pad = 0;  /* Will be calculated during draw */
  state->insert_y_start = 0;
  state->insert_y_end = 0;

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

  /* Update tooltip position on mouse move (for all categories with tooltip).
   * Keep tooltip fixed horizontally (X), only move vertically (Y) with cursor.
   * Hide tooltip when cursor leaves the source region to avoid showing it in other areas. */
  if (event->type == MOUSEMOVE && state->tooltip_region) {
    /* Check if cursor is within the source region bounds */
    const bool cursor_in_region = (event->mval[0] >= 0 && event->mval[0] <= region->winx &&
                                    event->mval[1] >= 0 && event->mval[1] <= region->winy);

    if (cursor_in_region) {
      /* Cursor is inside region - show and update tooltip position */
      if (state->tooltip_hidden) {
        /* Restore tooltip visibility - center on cursor with saved dimensions */
        state->tooltip_hidden = false;
        /* Use event->xy[1] for screen Y coordinate (available in modal handler) */
        const int mouse_y_screen = event->xy[1];
        /* Center tooltip vertically on cursor */
        const int half_height = state->tooltip_height / 2;
        state->tooltip_region->winrct.ymin = mouse_y_screen - half_height;
        state->tooltip_region->winrct.ymax = mouse_y_screen + half_height;
        /* Restore X position */
        state->tooltip_region->winrct.xmin = state->tooltip_initial_x;
        state->tooltip_region->winrct.xmax = state->tooltip_initial_x + state->tooltip_width;
      }
      else {
        /* Normal update - move tooltip vertically with cursor */
        int dy = event->mval[1] - state->current_mouse_y;
        state->tooltip_region->winrct.ymin += dy;
        state->tooltip_region->winrct.ymax += dy;
        /* Ensure X position stays at initial value. */
        state->tooltip_region->winrct.xmin = state->tooltip_initial_x;
        state->tooltip_region->winrct.xmax = state->tooltip_initial_x + state->tooltip_width;
      }
    }
    else {
      /* Cursor is outside region - hide tooltip by moving it off-screen */
      if (!state->tooltip_hidden) {
        state->tooltip_hidden = true;
      }
      /* Move tooltip far off-screen to prevent it from being visible in other areas */
      state->tooltip_region->winrct.xmin = -10000;
      state->tooltip_region->winrct.xmax = -9999;
      state->tooltip_region->winrct.ymin = -10000;
      state->tooltip_region->winrct.ymax = -9999;
    }

    state->current_mouse_x = event->mval[0];
    state->current_mouse_y = event->mval[1];
    ED_region_tag_redraw(region);
  }

  /* Special handling for reserved tabs (tooltip only) */
  if (state->is_reserved) {
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

  if (is_timer || event->type == MOUSEMOVE || event->type == WHEELUPMOUSE ||
      event->type == WHEELDOWNMOUSE)
  {
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
      int current_mouse_y = state->drag_start_y + int(state->drag_offset_y);

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
      region->category_scroll += int(scroll_amount);

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
      state->current_insert_index = calculate_insert_index(C, region, state);
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
#include "BKE_callbacks.hh"
