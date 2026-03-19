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

namespace blender::ui {

/* Forward declarations */
static bool category_name_is_glyph(const char *category_id);


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
  printf("[CATEGORY ACTIVATE] Extension callback triggered! valid=%d wait_for_signal=%d\n",
         g_deferred_category_activation.valid ? 1 : 0,
         g_deferred_category_activation.wait_for_extension_signal ? 1 : 0);
  fflush(stdout);
  if (g_deferred_category_activation.valid &&
      g_deferred_category_activation.wait_for_extension_signal) {
    g_deferred_category_activation.extension_signal_received = true;
    printf("[CATEGORY ACTIVATE] Extension signal received, will activate on next draw\n");
    fflush(stdout);
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
  if (!C || !category_id || category_id[0] == '\0') {
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
    BPY_run_string_exec(const_cast<bContext *>(C), imports_none, python_expr);
  }

  /* Tag the tag bar for refresh so the "New Add-on!" button can appear/disappear. */
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    ED_area_tag_redraw(area);
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
  if (!C || !category_id || category_id[0] == '\0') {
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
    if (tag_filter_state_from_area(area_for_tag, &tag_state) && tag_state.active_tags &&
        tag_state.active_tags[0] != '\0') {
      resolved_tag_name = tag_state.active_tags;
    }
  }

  printf("[CATEGORY ACTIVATE] Extension drop on tabs: resolved tag_name_to_assign='%s' (raw tag_name='%s')\n",
         resolved_tag_name.empty() ? "" : resolved_tag_name.c_str(),
         (tag_name != nullptr) ? tag_name : "(null)");
  fflush(stdout);

  if (tab_category != nullptr && !resolved_tag_name.empty()) {
    /* Drop onto tabs: defer tag assignment until category appears after extension install.
     * The category doesn't exist yet in _glyph_cache, so immediate assignment would fail.
     * We store the tag name and set tag_already_assigned=true so the deferred activation
     * will assign the tag when the category actually appears. */
    printf("[CATEGORY ACTIVATE] Extension drop on tabs: deferring tag assignment for category '%s', tag '%s'\n",
           category_id, resolved_tag_name.c_str());
    fflush(stdout);

    /* Store tag name for deferred assignment */
    g_deferred_category_activation.tag_name_to_assign = resolved_tag_name;

    /* Register as pending extension category with tag_already_assigned=true.
     * This means: pending=false (user already chose a tag via drag & drop),
     * but we still need to activate the category when it appears. */
    register_new_extension_category(
        C, category_id, extension_id, space_type, mode_flag, /*tag_already_assigned=*/true);
  }
  else {
    /* Drop into viewport: mark as pending so "New Add-on!" tag surfaces it. */
    register_new_extension_category(
        C, category_id, extension_id, space_type, mode_flag, /*tag_already_assigned=*/false);
  }
#else
  UNUSED_VARS(category_id, extension_id, tab_category, tag_name);
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
  if (!wm || !category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    return nullptr;
  }

  // First pass: try exact match (category + space_type)
  for (const CategoryGlyphItem *item =
           static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category) && item->space_type == space_type) {
      return item;
    }
  }

  // Second pass: try global categories (space_type = -1) if not searching for global already
  if (space_type != -1) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category) && item->space_type == -1) {
        return item;
      }
    }
  }

  // Third pass: try canonicalization fallback with space_type match
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item =
           static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    const std::string normalized_item = normalize_category_key(item->category);
    if (normalized_item == normalized_target && item->space_type == space_type) {
      return item;
    }
  }

  // Fourth pass: try canonicalization fallback with global categories
  if (space_type != -1) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      const std::string normalized_item = normalize_category_key(item->category);
      if (normalized_item == normalized_target && item->space_type == -1) {
        return item;
      }
    }
  }
  
  return nullptr;
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

  // First pass: try exact match in overrides (category + space_type)
  if (category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category) && item->space_type == space_type) {
        return item->tags;
      }
    }
  }

  // Second pass: try global overrides (space_type = -1) if not searching for global already
  if (space_type != -1) {
    if (category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
      for (const CategoryGlyphItem *item =
               static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
           item;
           item = static_cast<const CategoryGlyphItem *>(item->next))
      {
        if (STREQ(item->category, category) && item->space_type == -1) {
          return item->tags;
        }
      }
    }
  }

  // Third pass: try exact match in mappings (category + space_type)
  if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category) && item->space_type == space_type) {
        return item->tags;
      }
    }
  }

  // Fourth pass: try global mappings (space_type = -1) if not searching for global already
  if (space_type != -1) {
    if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
      for (const CategoryGlyphItem *item =
               static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
           item;
           item = static_cast<const CategoryGlyphItem *>(item->next))
      {
        if (STREQ(item->category, category) && item->space_type == -1) {
          return item->tags;
        }
      }
    }
  }

  // Fifth pass: try canonicalization fallback with space_type match
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return "";
  }

  // Check overrides with canonicalization and space_type
  if (category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      const std::string normalized_item = normalize_category_key(item->category);
      if (normalized_item == normalized_target && item->space_type == space_type) {
        return item->tags;
      }
    }
  }

  // Check global overrides with canonicalization
  if (space_type != -1) {
    if (category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
      for (const CategoryGlyphItem *item =
               static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
           item;
           item = static_cast<const CategoryGlyphItem *>(item->next))
      {
        const std::string normalized_item = normalize_category_key(item->category);
        if (normalized_item == normalized_target && item->space_type == -1) {
          return item->tags;
        }
      }
    }
  }

  // Check mappings with canonicalization and space_type
  if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      const std::string normalized_item = normalize_category_key(item->category);
      if (normalized_item == normalized_target && item->space_type == space_type) {
        return item->tags;
      }
    }
  }

  // Check global mappings with canonicalization
  if (space_type != -1) {
    if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
      for (const CategoryGlyphItem *item =
               static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
           item;
           item = static_cast<const CategoryGlyphItem *>(item->next))
      {
        const std::string normalized_item = normalize_category_key(item->category);
        if (normalized_item == normalized_target && item->space_type == -1) {
          return item->tags;
        }
      }
    }
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

static const char *panel_category_glyph_lookup_override(const wmWindowManager *wm,
                                                        const char *category,
                                                        bool *r_is_fallback_letter,
                                                        float r_color[3],
                                                        bool *r_handled)
{
  *r_handled = false;

  if (!(wm && category_glyph_list_is_valid(&wm->category_glyph_overrides))) {
    return nullptr;
  }

  // First pass: try exact match
  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(
           wm->category_glyph_overrides.first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (!STREQ(item->category, category)) {
      continue;
    }

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
          continue;
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
    bool is_text_based_category = false;
    for (const CategoryGlyphItem *map_item = static_cast<const CategoryGlyphItem *>(
             wm->category_glyph_mappings.first);
         map_item;
         map_item = static_cast<const CategoryGlyphItem *>(map_item->next))
    {
      if (STREQ(map_item->category, category)) {
        /* If default_glyph is empty, this is a text_only or glyph_text category.
         * Reset should return first letter, not the glyph from mappings. */
        if (map_item->default_glyph[0] == '\0') {
          is_text_based_category = true;
        }
        break;
      }
    }

    if (is_text_based_category) {
      /* Text-based category with empty override glyph = reset to first letter.
       * Set handled=true to prevent fallback to mappings which would return old glyph.
       * Set is_fallback_letter=true so draw code knows to use first letter. */
      if (r_is_fallback_letter) {
        *r_is_fallback_letter = true;
      }
      *r_handled = true;
      return nullptr;
    }

    break;
  }

  // Second pass: try canonicalization fallback
  const std::string normalized_target = normalize_category_key(category);
  if (!normalized_target.empty()) {
    for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(
             wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      const std::string normalized_item = normalize_category_key(item->category);
      if (normalized_item != normalized_target) {
        continue;
      }

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
          continue;
        }
        if (r_is_fallback_letter) {
          *r_is_fallback_letter = true;
        }
        *r_handled = true;
        return nullptr;
      }

      if (r_color) {
        copy_v3_v3(r_color, item->color);
      }
      *r_handled = true;
      return item->glyph;
    }

    /* Override has no glyph: may be explicit clear OR tags-only carrier.
     * For text_only/glyph_text categories, this means reset to first letter. */
    if (r_color && !is_zero_v3(item->color)) {
      copy_v3_v3(r_color, item->color);
    }

    /* Check if this is a text_only/glyph_text category by looking at mappings.
     * If default_glyph is empty, reset should return first letter, not mapping glyph. */
    bool is_text_based_category = false;
    for (const CategoryGlyphItem *map_item = static_cast<const CategoryGlyphItem *>(
             wm->category_glyph_mappings.first);
         map_item;
         map_item = static_cast<const CategoryGlyphItem *>(map_item->next))
    {
      if (STREQ(map_item->category, category)) {
        if (map_item->default_glyph[0] == '\0') {
          is_text_based_category = true;
        }
        break;
      }
    }

    if (is_text_based_category) {
      /* Text-based category with empty override glyph = reset to first letter.
       * Set is_fallback_letter=true so draw code knows to use first letter. */
      if (r_is_fallback_letter) {
        *r_is_fallback_letter = true;
      }
      *r_handled = true;
      return nullptr;
    }

    break;
    }
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
          wm, category, r_is_fallback_letter, r_color, &handled))
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
      if (is_zero_v3(r_color) && wm &&
          category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
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

  if (category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(
             wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category) && !is_zero_v3(item->color)) {
        copy_v3_v3(r_color, item->color);
        return;
      }
    }

    const std::string normalized_target = normalize_category_key(category);
    if (!normalized_target.empty()) {
      for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(
               wm->category_glyph_overrides.first);
           item;
           item = static_cast<const CategoryGlyphItem *>(item->next))
      {
        if (is_zero_v3(item->color)) {
          continue;
        }
        const std::string normalized_item = normalize_category_key(item->category);
        if (normalized_item == normalized_target) {
          copy_v3_v3(r_color, item->color);
          return;
        }
      }
    }
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

  const auto override_icon_is_effective = [](const CategoryGlyphItem *item) {
    const bool has_icon_payload = (item->icon_key[0] != '\0') || (item->icon_path[0] != '\0') ||
                                  (item->icon_provider[0] != '\0');
    const bool has_explicit_icon_mode = ELEM(
        item->icon_source, CATEGORY_TAB_ICON_SOURCE_MANUAL, CATEGORY_TAB_ICON_SOURCE_OFF);

    /* Important: override entries are often created for glyph/color/tag edits.
     * If an AUTO override has no icon payload, don't mask JSON mapping icon data. */
    return has_icon_payload || has_explicit_icon_mode;
  };

  /* 1) User overrides. */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (!STREQ(item->category, category) || !override_icon_is_effective(item)) {
        continue;
      }
      icon_data_copy_from_item(item);
      return true;
    }

    const std::string normalized_target = normalize_category_key(category);
    if (!normalized_target.empty()) {
      for (const CategoryGlyphItem *item =
               static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
           item;
           item = static_cast<const CategoryGlyphItem *>(item->next))
      {
        if (!override_icon_is_effective(item)) {
          continue;
        }

        const std::string normalized_item = normalize_category_key(item->category);
        if (normalized_item != normalized_target) {
          continue;
        }

        icon_data_copy_from_item(item);
        return true;
      }
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
  const CategoryGlyphItem *global_override = nullptr;
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (!STREQ(item->category, category)) {
        continue;
      }
      if (item->space_type == space_type && item->display_name[0] != '\0') {
        return item->display_name;
      }
      if (space_type != -1 && item->space_type == -1 && item->display_name[0] != '\0' &&
          global_override == nullptr)
      {
        global_override = item;
      }
    }
  }

  if (global_override) {
    return global_override->display_name;
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
        /* Category is in overrides but display_name is empty - try default_display_name. */
        if (item->default_display_name[0] != '\0') {
          return item->default_display_name;
        }
        /* Both display_name and default_display_name are empty - use category name. */
        return category_idname;
      }
    }
  }

  /* 2. Check global mappings. If category is in mappings and has display_name, use it. */
  bool found_in_mappings = false;
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category_idname)) {
        found_in_mappings = true;
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        /* Category is in mappings but display_name is empty - try default_display_name. */
        if (item->default_display_name[0] != '\0') {
          return item->default_display_name;
        }
        /* Both display_name and default_display_name are empty - use category name. */
        return category_idname;
      }
    }
  }

  /* 3. For reserved categories (built-in Blender categories), use category name. */
  if (category_is_reserved(wm, category_idname)) {
    return category_idname;
  }

  /* 4. For categories NOT in mappings (not explicitly configured), look up panel label.
   * This handles special cases like "Script 3" where category name contains only a glyph. */
  if (!found_in_mappings) {
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

  if (has_custom_color) {
    r_tint[0] = uchar(custom_color[0] * 255.0f);
    r_tint[1] = uchar(custom_color[1] * 255.0f);
    r_tint[2] = uchar(custom_color[2] * 255.0f);
  }
  else {
    uchar theme_icon_color[4];
    if (icon_get_theme_color(icon_id, theme_icon_color)) {
      copy_v4_v4_uchar(r_tint, theme_icon_color);
    }
    else {
      /* Default monochrome icon color. */
    }
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

  /* Get category tags with space_type awareness */
  const int space_type = area ? area->spacetype : -1;
  const char *category_tags = category_tags_string_lookup(wm, category_idname, space_type);

  /* If category has no tags - check if it's a newly installed extension category.
   * New extension categories should be visible regardless of tag filter state,
   * allowing users to see and configure them immediately after installation. */
  if (!category_tags || category_tags[0] == '\0') {
    /* Exception: if this category has a pending tag assignment (introduced by an extension),
     * show it so users can see and configure it immediately after installation. */
    const CategoryGlyphItem *glyph_item = category_glyph_mapping_find(wm, category_idname, space_type);
    if (glyph_item && glyph_item->pending_tag_assignment) {
      return true;
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
      
      printf("[CATEGORY ACTIVATE] Safe activation: deferred activation set for '%s', waiting for signal\n",
             category_id);
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
    printf("[EXT_PREVIEW] preview_set: region or runtime is NULL\n");
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
    printf("[EXT_PREVIEW] preview_clear: region=%p was active target='%s' idx=%d insert_above=%d tab_h=%d pad=%d cursor_y=%d\n",
           static_cast<void *>(region),
           state.target_category_id,
           state.target_index,
           state.insert_above ? 1 : 0,
           state.tab_height,
           state.tab_v_pad,
           state.cursor_y);
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
      *r_target_index = last_reserved_prefix_idx;
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
      if (current_time - _ext_hit_reserved_redirect_log_time > 1.0) {
        printf("[EXT_HIT] local=(%d,%d) reserved_redirect -> target='%s' idx=%d insert_above=0\n",
               mouse_x_local,
               mouse_y_local,
               last_reserved_prefix_tab->idname,
               last_reserved_prefix_idx);
        _ext_hit_reserved_redirect_log_time = current_time;
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
      *r_target_index = bottom_tab_index;
    }
    if (r_insert_above) {
      *r_insert_above = false;
    }
    if (r_tab_height) {
      *r_tab_height = BLI_rcti_size_y(&bottom_tab->rect);
    }

    static double _ext_hit_below_last_log_time = 0.0;
    const double current_time = BLI_time_now_seconds();
    if (current_time - _ext_hit_below_last_log_time > 1.0) {
      printf("[EXT_HIT] local=(%d,%d) mode=below_all -> target='%s' idx=%d insert_above=0 tabs_bottom_y=%d\n",
             mouse_x_local,
             mouse_y_local,
             bottom_tab->idname,
             bottom_tab_index,
             tabs_bottom_y);
      _ext_hit_below_last_log_time = current_time;
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
      *r_target_index = 0;
    }
    if (r_insert_above) {
      *r_insert_above = true;
    }
    if (r_tab_height) {
      *r_tab_height = BLI_rcti_size_y(&first_tab->rect);
    }

    static double _ext_hit_settings_last_log_time = 0.0;
    const double current_time = BLI_time_now_seconds();
    if (current_time - _ext_hit_settings_last_log_time > 1.0) {
      printf("[EXT_HIT] local=(%d,%d) tab='Display Mode Settings' mode=settings -> target='%s' idx=0 insert_above=1\n",
             mouse_x_local,
             mouse_y_local,
             first_tab->idname);
      _ext_hit_settings_last_log_time = current_time;
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
    *r_target_index = hit_visual_index;
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
   * This allows assigning the active tag filter to the new category after extension install. */
  if (tag_name && tag_name[0] != '\0') {
    g_deferred_category_activation.tag_name_to_assign = tag_name;
    g_deferred_category_activation.tag_already_assigned = true;
    printf("[CATEGORY ACTIVATE] Will assign tag '%s' to new category when it appears\n", tag_name);
  }
  else {
    g_deferred_category_activation.tag_name_to_assign.clear();
    g_deferred_category_activation.tag_already_assigned = false;
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

  printf("[CATEGORY ACTIVATE] Extension drop: waiting for new category to appear (target was '%s')\n",
         category_id);

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
  printf("[KNOWN_CATS] Saving known categories before extension drop:\n");
  for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (pc_dyn.idname && pc_dyn.idname[0]) {
      g_known_categories_before_extension_drop.add(pc_dyn.idname);
      g_pending_category_insert.all_existing_categories.add(pc_dyn.idname);
      printf("[KNOWN_CATS]   + '%s'\n", pc_dyn.idname);
    }
  }
  printf("[KNOWN_CATS] Total: %zu categories (also saved to pending_insert)\n",
         g_known_categories_before_extension_drop.size());

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
  if (tag_filter_state_from_area(CTX_wm_area(C), &state) && state.active_tags) {
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

Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region)
{
  ScrArea *area = CTX_wm_area(C);
  const wmWindowManager *wm = CTX_wm_manager(C);

  /* Get tag combination key for current filter state */
  std::string tag_key = get_tag_combination_key(wm, C);

  /* Load order from JSON for this tag combination */
  Vector<std::string> json_order = load_category_order_from_json(C, tag_key.c_str());

  if (g_pending_category_insert.valid && g_pending_category_insert.tag_key == tag_key) {
    const double time_since_pending = BLI_time_now_seconds() - g_pending_category_insert.timestamp;
    if (time_since_pending > 120.0) {
      g_pending_category_insert.valid = false;
      g_pending_category_insert.all_existing_categories.clear();
    }
  }

  /* Map of existing categories for quick lookup */
  Map<std::string, PanelCategoryDyn *> existing;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    /* Only include categories that are visible by tag filtering */
    if (panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      existing.add(std::string(pc_dyn.idname), &pc_dyn);
    }
  }

  /* Build result list following JSON order */
  Vector<PanelCategoryDyn *> result;
  Set<std::string> added;

  /* First: categories in JSON order (skip missing - disabled addons) */
  for (const std::string &cat_id : json_order) {
    PanelCategoryDyn **pc = existing.lookup_ptr(cat_id);
    if (pc && !added.contains(cat_id)) {
      result.append(*pc);
      added.add(cat_id);
    }
    /* Else: category in JSON but not registered - skip silently (disabled addon) */
  }

  /* Then: remaining categories (new ones or not in JSON order) */
  Vector<PanelCategoryDyn *> remaining;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    std::string id(pc_dyn.idname);
    if (!added.contains(id) && panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      remaining.append(&pc_dyn);
    }
  }

  bool pending_applied = false;
  Vector<std::string> pending_inserted_ids;
  if (g_pending_category_insert.valid && g_pending_category_insert.tag_key == tag_key)
  {
    Vector<PanelCategoryDyn *> appeared_categories;
    printf("[GET_ORDERED] all_existing_categories.size()=%zu, existing_categories.size()=%zu, g_known_categories_before_extension_drop.size()=%zu\n",
           g_pending_category_insert.all_existing_categories.size(),
           g_pending_category_insert.existing_categories.size(),
           g_known_categories_before_extension_drop.size());
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
          !g_pending_category_insert.all_existing_categories.contains(std::string(pc_dyn.idname)) :
          (!g_known_categories_before_extension_drop.is_empty() ?
              !g_known_categories_before_extension_drop.contains(std::string(pc_dyn.idname)) :
              !g_pending_category_insert.existing_categories.contains(std::string(pc_dyn.idname)));
      printf("[GET_ORDERED]   cat='%s' use_full=%d is_new=%d\n",
             pc_dyn.idname, use_full_category_list, is_new_category);
      if (!is_new_category && !panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
        continue;
      }
      if (is_new_category) {
        appeared_categories.append(&pc_dyn);
      }
    }

    if (!appeared_categories.is_empty()) {
      Set<std::string> appeared_ids;
      for (PanelCategoryDyn *pc_dyn : appeared_categories) {
        appeared_ids.add(std::string(pc_dyn->idname));
      }

      for (int i = result.size() - 1; i >= 0; i--) {
        const std::string id(result[i]->idname);
        if (appeared_ids.contains(id)) {
          result.remove_and_reorder(i);
          added.remove(id);
        }
      }

      for (int i = remaining.size() - 1; i >= 0; i--) {
        const std::string id(remaining[i]->idname);
        if (appeared_ids.contains(id)) {
          remaining.remove_and_reorder(i);
        }
      }

      auto find_index_in_result = [&](const std::string &id) -> int {
        if (id.empty()) {
          return -1;
        }
        for (int i = 0; i < result.size(); i++) {
          if (STREQ(result[i]->idname, id.c_str())) {
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
          insert_index = g_pending_category_insert.insert_above ? target_index :
                                                                 (target_index + 1);
        }
      }
      if (insert_index == -1) {
        insert_index = result.size();
      }
      insert_index = clamp_i(insert_index, 0, result.size());

      Vector<PanelCategoryDyn *> appeared_ordered;
      appeared_ordered.reserve(appeared_categories.size());
      Set<std::string> ordered_ids;
      for (const std::string &cat_id : json_order) {
        if (!appeared_ids.contains(cat_id) || ordered_ids.contains(cat_id)) {
          continue;
        }
        PanelCategoryDyn **pc = existing.lookup_ptr(cat_id);
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
        if (!added.contains(id)) {
          result.insert(insert_index + insert_offset, pc_dyn);
          insert_offset++;
          added.add(id);
          pending_inserted_ids.append(id);
        }
      }

      if (!pending_inserted_ids.is_empty()) {
        pending_applied = true;
        category_tabs_report_new_categories(C, pending_inserted_ids);
      }
    }
  }

  /* Get space type name for priority lookup (C++ is the source of truth). */
  const char *space_type_name = "DEFAULT";
  if (area) {
    switch (area->spacetype) {
      case SPACE_VIEW3D: space_type_name = "VIEW_3D"; break;
      case SPACE_PROPERTIES: space_type_name = "PROPERTIES"; break;
      case SPACE_NODE: space_type_name = "NODE_EDITOR"; break;
      case SPACE_IMAGE: space_type_name = "IMAGE_EDITOR"; break;
      case SPACE_SEQ: space_type_name = "SEQUENCE_EDITOR"; break;
      case SPACE_CLIP: space_type_name = "CLIP_EDITOR"; break;
      case SPACE_TEXT: space_type_name = "TEXT_EDITOR"; break;
      case SPACE_ACTION: space_type_name = "DOPESHEET_EDITOR"; break;
      case SPACE_GRAPH: space_type_name = "GRAPH_EDITOR"; break;
      case SPACE_NLA: space_type_name = "NLA_EDITOR"; break;
      default: space_type_name = "DEFAULT"; break;
    }
  }

  /* After processing JSON order and pending inserts,
   * separate remaining categories into reserved and non-reserved */
  Vector<PanelCategoryDyn *> remaining_reserved;
  Vector<PanelCategoryDyn *> remaining_non_reserved;

  for (PanelCategoryDyn *pc_dyn : remaining) {
    std::string id(pc_dyn->idname);
    if (!added.contains(id)) {
      if (category_is_reserved_for_reorder(wm, pc_dyn->idname)) {
        remaining_reserved.append(pc_dyn);
      } else {
        remaining_non_reserved.append(pc_dyn);
      }
    }
  }

  /* Sort reserved categories by priority */
  std::sort(remaining_reserved.begin(), remaining_reserved.end(),
    [&](PanelCategoryDyn *a, PanelCategoryDyn *b) {
      return compare_reserved_categories_by_priority(C, a->idname, b->idname, space_type_name);
    });

  /* Append: reserved first, then non-reserved (boundary rule preserved) */
  for (PanelCategoryDyn *pc_dyn : remaining_reserved) {
    result.append(pc_dyn);
    added.add(pc_dyn->idname);
  }
  for (PanelCategoryDyn *pc_dyn : remaining_non_reserved) {
    result.append(pc_dyn);
    added.add(pc_dyn->idname);
  }

  /* Enforce global invariant for runtime display order:
   * reserved categories must be grouped first and sorted by reserved priority.
   * Non-reserved categories keep their relative order. */
  {
    Vector<PanelCategoryDyn *> reserved_sorted;
    Vector<PanelCategoryDyn *> non_reserved_ordered;
    reserved_sorted.reserve(result.size());
    non_reserved_ordered.reserve(result.size());

    for (PanelCategoryDyn *pc_dyn : result) {
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
    normalized.reserve(result.size());
    for (PanelCategoryDyn *pc_dyn : reserved_sorted) {
      normalized.append(pc_dyn);
    }
    for (PanelCategoryDyn *pc_dyn : non_reserved_ordered) {
      normalized.append(pc_dyn);
    }
    result = std::move(normalized);
  }

  if (pending_applied) {
    Vector<std::string> pending_order = json_order;
    if (pending_order.is_empty()) {
      pending_order = g_pending_category_insert.pre_order;
    }

    auto normalize_reserved_boundary = [&](Vector<std::string> &order) {
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
    };

    if (pending_order.is_empty()) {
      Vector<std::string> from_result;
      from_result.reserve(result.size());
      for (PanelCategoryDyn *pc_dyn : result) {
        from_result.append(pc_dyn->idname);
      }
      normalize_reserved_boundary(from_result);
      pending_order = std::move(from_result);
    }
    else if (category_order_is_crossing_reserved_boundary(wm, pending_order)) {
      normalize_reserved_boundary(pending_order);
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
        order_insert_index = g_pending_category_insert.insert_above ? target_index :
                                                                      (target_index + 1);
      }
    }
    if (order_insert_index == -1) {
      order_insert_index = pending_order.size();
    }

    order_insert_index = clamp_i(order_insert_index, 0, pending_order.size());

    for (const std::string &id : pending_inserted_ids) {
      erase_first_in_order(id);
    }

    int order_offset = 0;
    for (const std::string &id : pending_inserted_ids) {
      pending_order.insert(order_insert_index + order_offset, id);
      order_offset++;
    }

    /* Filter pending_order to only include categories that should be visible with current tag filter.
     * This prevents saving categories without the active tag to the tag-specific order in JSON,
     * which would cause them to briefly appear before being filtered out.
     * Keep: reserved categories, newly inserted categories, and categories passing tag filter. */
    if (!tag_key.empty() && tag_key.back() != ':') {
      /* tag_key has an active tag filter (not just space prefix like "VIEW3D:") */
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

    /* Persisted order must always satisfy reserved-first + reserved-priority invariant. */
    normalize_reserved_boundary(pending_order);

    if (!category_order_is_crossing_reserved_boundary(wm, pending_order)) {
      save_category_order_to_json(C, tag_key.c_str(), pending_order);
      ScrArea *save_area = CTX_wm_area(C);
      const int save_space_type = save_area ? save_area->spacetype : 0;
      const int save_region_type = region->regiontype;
      WorkSpace *save_workspace = CTX_wm_workspace(C);
      if (save_workspace) {
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
    }

    /* Copy pending insert position to deferred activation BEFORE clearing it.
     * This ensures the insert position is preserved for deferred_category_activation_execute. */
    if (g_pending_category_insert.valid) {
      g_deferred_category_activation.pending_insert_valid = true;
      g_deferred_category_activation.pending_insert_tag_key = g_pending_category_insert.tag_key;
      g_deferred_category_activation.pending_insert_anchor_before = g_pending_category_insert.anchor_before;
      g_deferred_category_activation.pending_insert_anchor_after = g_pending_category_insert.anchor_after;
      g_deferred_category_activation.pending_insert_target_category = g_pending_category_insert.target_category;
      g_deferred_category_activation.pending_insert_insert_above = g_pending_category_insert.insert_above;
      printf("[CATEGORY ORDER] Copied pending insert to deferred activation: tag_key='%s', anchor_before='%s', anchor_after='%s'\n",
             g_pending_category_insert.tag_key.c_str(),
             g_pending_category_insert.anchor_before.c_str(),
             g_pending_category_insert.anchor_after.c_str());
    }
    else {
      printf("[CATEGORY ORDER] g_pending_category_insert.valid is FALSE - no position to copy!\n");
    }
    fflush(stdout);

    g_pending_category_insert.valid = false;
    g_pending_category_insert.all_existing_categories.clear();

    /* Auto-activate newly appeared category after extension installation.
     * Uses deferred activation with extension signal to ensure:
     * 1. Extension installation is complete
     * 2. UI has time to stabilize (frame delay)
     * 3. Category activation happens safely on main thread
     */
#if 1
    printf("[CATEGORY ACTIVATE] pending_inserted_ids count: %zu\n", pending_inserted_ids.size());
    if (!pending_inserted_ids.is_empty()) {
      for (const std::string &category_id : pending_inserted_ids) {
        printf("[CATEGORY ACTIVATE] Checking category: '%s'\n", category_id.c_str());

        /* Mark this category as a new extension category via the pending_tag_assignment
         * mechanism. This replaces the old g_new_extension_categories_visible set and
         * ensures the category is visible through the DNA-backed pending flag. */
        register_new_extension_category(C,
                                        category_id.c_str(),
                                        g_pending_category_insert.source_extension_id.c_str(),
                                        area ? area->spacetype : -1,
                                        get_current_tag_mode_flag(C),
                                        /*tag_already_assigned=*/false);
        printf("[CATEGORY ACTIVATE]   Registered as pending extension category\n");

        const bool is_visible = panel_category_is_visible_by_tags(C, wm, category_id.c_str());
        printf("[CATEGORY ACTIVATE]   is_visible: %s\n", is_visible ? "true" : "false");

        if (is_visible) {
          const char *current_active = panel_category_active_get(region, false);
          printf("[CATEGORY ACTIVATE]   current_active: '%s'\n",
                 current_active ? current_active : "(null)");

          const bool should_activate = (current_active == nullptr ||
                                        !STREQ(category_id.c_str(), current_active));
          printf("[CATEGORY ACTIVATE]   should_activate: %s\n", should_activate ? "true" : "false");

          if (should_activate) {
            /* Defer activation to avoid crashes during panel layout.
             * Some extensions (like Ucupaint) load previews in background threads,
             * which crashes when triggered from panel_poll during layout.
             * We wait 3 frames before activating to allow UI to stabilize. */
            printf("[CATEGORY ACTIVATE]   Deferring activation for: '%s' (3 frame delay)\n",
                   category_id.c_str());

            /* Check if deferred activation is already set up (from discover mode or previous call) */
            bool already_has_category = !g_deferred_category_activation.category_id.empty();
            bool signal_already_received = g_deferred_category_activation.extension_signal_received;
            bool already_waiting_for_signal = g_deferred_category_activation.wait_for_extension_signal;

            /* Skip if already set up - don't overwrite existing activation state */
            if (already_has_category) {
              printf("[CATEGORY ACTIVATE]   Skipping - category already set: '%s'\n",
                     g_deferred_category_activation.category_id.c_str());
              break;
            }

            g_deferred_category_activation.category_id = category_id;
            g_deferred_category_activation.valid = true;
            g_deferred_category_activation.timestamp = BLI_time_now_seconds();
            g_deferred_category_activation.frame_delay = 3; /* Wait 3 frames before activating */

            /* Only set wait_for_extension_signal if not already in progress */
            if (!signal_already_received && !already_waiting_for_signal) {
              g_deferred_category_activation.wait_for_extension_signal = true;
              g_deferred_category_activation.extension_signal_received = false;
            }
            printf("[CATEGORY ACTIVATE]   signal_already_received: %s, already_waiting: %s\n",
                   signal_already_received ? "true" : "false",
                   already_waiting_for_signal ? "true" : "false");

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
            printf("[CATEGORY ACTIVATE]   Deferred activation scheduled\n");
          }
          printf("[CATEGORY ACTIVATE]   Breaking loop after first visible category\n");
          break;
        }
      }
    }
    printf("[CATEGORY ACTIVATE] Activation block completed\n");
#endif
  }

  /* Auto-save initial category order when JSON order was empty.
   * This ensures category_orders is populated on first run with discovered categories. */
  if (json_order.is_empty() && !result.is_empty()) {
    Vector<std::string> initial_order;
    initial_order.reserve(result.size());
    for (PanelCategoryDyn *pc_dyn : result) {
      initial_order.append(pc_dyn->idname);
    }

    /* Normalize reserved boundary before saving */
    Vector<std::string> reserved;
    Vector<std::string> non_reserved;
    reserved.reserve(initial_order.size());
    non_reserved.reserve(initial_order.size());
    for (const std::string &category_id : initial_order) {
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

    initial_order.clear();
    initial_order.reserve(reserved.size() + non_reserved.size());
    for (const std::string &category_id : reserved) {
      initial_order.append(category_id);
    }
    for (const std::string &category_id : non_reserved) {
      initial_order.append(category_id);
    }

    if (!category_order_is_crossing_reserved_boundary(wm, initial_order)) {
      save_category_order_to_json(C, tag_key.c_str(), initial_order);
      printf("[CATEGORY ORDER] Auto-saved initial order for tag_key='%s' with %zu categories\n",
             tag_key.c_str(), initial_order.size());
    }
  }

  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Drawing Functions
 * \{ */

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
  const bool display_mode_allows_icon_content = ELEM(
      display_mode, USER_CATEGORY_TABS_GLYPHS_ONLY, USER_CATEGORY_TABS_GLYPHS_TEXT);

  CategoryTabIconResolved icon_resolved;
  panel_category_icon_data_lookup(wm, category_id, &icon_resolved, space_type);
  const int resolved_icon_id = category_tab_icon_id_resolve(icon_resolved);

  bool icon_data_allows_icon_content = (icon_resolved.source != CATEGORY_TAB_ICON_SOURCE_OFF);
  if (icon_resolved.source == CATEGORY_TAB_ICON_SOURCE_MANUAL) {
    /* In manual mode require explicit key/path input, otherwise use glyph fallback. */
    icon_data_allows_icon_content = (icon_resolved.key && icon_resolved.key[0] != '\0') ||
                                    (icon_resolved.path && icon_resolved.path[0] != '\0');
  }

  const bool use_builtin_icon =
      display_mode_allows_icon_content && icon_data_allows_icon_content && (resolved_icon_id != ICON_NONE);
  const float tab_font_size = fstyle_points * UI_SCALE_FAC * category_tabs_zoom;
  const bool is_being_edited_in_dialog = !is_active &&
                                         category_tab_edit_dialog_is_open_for_category(category_id);
  const float draw_darken_factor = is_being_edited_in_dialog ? 0.0f : darken_factor;

  bool is_fallback_letter = false;
  float glyph_color[3] = {0.0f, 0.0f, 0.0f};
  const char *glyph = panel_category_glyph_lookup(
      wm, category_id, nullptr, &is_fallback_letter, glyph_color, space_type);

  /* Use live preview color when dialog is open for this category */
  if (is_being_edited_in_dialog && !is_zero_v3(category_tab_preview_color)) {
    copy_v3_v3(glyph_color, category_tab_preview_color);
  }

  /* Safety net for built-in icons:
   * icon tint must use category custom color even when glyph lookup resolves through
   * fallback branches that may leave color unset in transient live-preview states. */
  if (use_builtin_icon && is_zero_v3(glyph_color)) {
    panel_category_color_lookup(wm, category_id, glyph_color);
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

  /* --- BEGIN: MIXED_MODE_CONTENT_FLAGS --- */
  /* In Mixed mode, apply per-content-type visibility flags.
   * To remove this feature: replace these effective variables with their source values
   * (e.g., mixed_mode_effective_has_glyph -> has_glyph) and delete this block. */
  const bool mixed_mode_effective_has_glyph =
      has_glyph && U.category_tabs_mixed_show_glyphs;
  const bool mixed_mode_effective_fallback_letter =
      is_fallback_letter && U.category_tabs_mixed_show_first_letter;
  const bool mixed_mode_effective_builtin_icon =
      use_builtin_icon && U.category_tabs_mixed_show_icons;

  /* For draw_dual in Mixed mode: show glyph/letter/icon + text if any glyph content is visible.
   * To remove: replace with `(has_glyph || is_fallback_letter || use_builtin_icon)` */
  const bool mixed_mode_has_visible_glyph_content =
      mixed_mode_effective_has_glyph || mixed_mode_effective_fallback_letter ||
      mixed_mode_effective_builtin_icon;
  /* --- END: MIXED_MODE_CONTENT_FLAGS --- */

  const bool use_reserved_inactive_icon_only =
      U.category_tabs_hide_reserved_inactive_text && !is_active &&
      ELEM(display_mode, USER_CATEGORY_TABS_GLYPHS_TEXT, USER_CATEGORY_TABS_TEXT_ONLY) &&
      category_is_reserved_for_reorder(wm, category_id) && has_glyph;

  bool draw_dual = false;
  const char *text_for_name = category_id_draw;

  if (display_mode == USER_CATEGORY_TABS_GLYPHS_TEXT && !use_reserved_inactive_icon_only &&
      mixed_mode_has_visible_glyph_content)
  {
    draw_dual = true;
    if (is_single_glyph_str(text_for_name)) {
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

/* Execute deferred category activation if pending.
 * This is called from panel_category_tabs_draw_all which runs in a safe context
 * after panel layout is complete, avoiding crashes when extensions load previews
 * in background threads. */
static void deferred_category_activation_execute(const bContext *C, ARegion *region)
{
  if (!g_deferred_category_activation.valid) {
    return;
  }

  std::string category_id = g_deferred_category_activation.category_id;

  /* Check if we need to wait for extension installation signal */
 if (g_deferred_category_activation.wait_for_extension_signal) {
 if (!g_deferred_category_activation.extension_signal_received) {
 printf("[CATEGORY ACTIVATE] Waiting for extension installation signal for: '%s'\n",
 category_id.c_str());
 return;
 }
 printf("[CATEGORY ACTIVATE] Extension installation signal received for: '%s', proceeding with activation\n",
 category_id.c_str());

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
    printf("[CATEGORY ACTIVATE] Discover mode: looking for new categories... (retry %d/30)\n",
           g_deferred_category_activation.discover_retry_count);

    /* IMPORTANT: Search directly in region->runtime->panels_category, NOT through get_ordered_categories()!
     * get_ordered_categories() applies tag filtering via panel_category_is_visible_by_tags(),
     * which would hide new categories that don't have tags assigned yet.
     * New extension categories must be discoverable regardless of tag filter state. */
    std::string new_category_id;
    for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
      if (pc_dyn.idname && pc_dyn.idname[0]) {
        if (!g_known_categories_before_extension_drop.contains(pc_dyn.idname)) {
          new_category_id = pc_dyn.idname;
          printf("[CATEGORY ACTIVATE]   Found new category: '%s'\n", new_category_id.c_str());
          break; /* Take the first new category */
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
        printf("[CATEGORY ACTIVATE]   No new category found yet, will retry\n");
        return;
      }
      printf("[CATEGORY ACTIVATE]   No new category found after timeout, clearing deferred activation\n");
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
    printf("[CATEGORY ACTIVATE]   Registered '%s' as pending extension category\n", new_category_id.c_str());
    g_deferred_category_activation.discover_retry_count = 0; /* Reset retry counter */
    printf("[CATEGORY ACTIVATE]   Will activate new category: '%s'\n", category_id.c_str());
  }

  /* Check frame delay - wait N frames before activating */
  if (g_deferred_category_activation.frame_delay > 0) {
    g_deferred_category_activation.frame_delay--;
    printf("[CATEGORY ACTIVATE] Frame delay: %d remaining for: '%s'\n",
           g_deferred_category_activation.frame_delay,
           category_id.c_str());
    return;
  }

  printf("[CATEGORY ACTIVATE] Executing deferred activation for: '%s'\n", category_id.c_str());

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
    printf("[CATEGORY ACTIVATE]   Category no longer exists, skipping\n");
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
    printf("[CATEGORY ACTIVATE]   Category already active, skipping\n");
    g_deferred_category_activation.valid = false;
    g_deferred_category_activation.discover_new_category = false;
    g_deferred_category_activation.discover_retry_count = 0;
    g_deferred_category_activation.tag_name_to_assign.clear();
    g_known_categories_before_extension_drop.clear();
    return;
  }

  /* Perform activation */
  printf("[CATEGORY ACTIVATE]   Setting active category to: '%s'\n", category_id.c_str());
  panel_category_active_set(region, category_id.c_str());
  printf("[CATEGORY ACTIVATE]   panel_category_active_set completed\n");

  /* Save to tag category memory */
  if (!g_deferred_category_activation.tag_key.empty()) {
    printf("[CATEGORY ACTIVATE]   Saving to tag memory, tag_key: '%s'\n",
           g_deferred_category_activation.tag_key.c_str());
    blender::ui::tag_save_last_active_category(
        const_cast<bContext *>(C),
        g_deferred_category_activation.tag_key.c_str(),
        category_id.c_str());
    printf("[CATEGORY ACTIVATE]   tag_save_last_active_category completed\n");
  }

  /* Assign deferred tag if one was saved from drag & drop on tabs.
   * This happens when an extension was dropped onto a tab with an active tag filter.
   * The category didn't exist yet, so we deferred tag assignment until now. */
  if (!g_deferred_category_activation.tag_name_to_assign.empty()) {
    printf("[CATEGORY ACTIVATE]   Assigning deferred tag '%s' to category '%s'\n",
           g_deferred_category_activation.tag_name_to_assign.c_str(),
           category_id.c_str());
    fflush(stdout);

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
      printf("[CATEGORY ACTIVATE]   Assigned tag '%s' to category '%s'\n",
             single_tag.c_str(),
             category_id.c_str());
      fflush(stdout);
    }

    printf("[CATEGORY ACTIVATE]   Deferred tag assignment completed (%zu tags)\n", tag_list.size());
    fflush(stdout);

    WM_event_add_notifier(const_cast<bContext *>(C), NC_WM | ND_CATEGORY_GLYPHS, nullptr);
    if (area) {
      ED_area_tag_redraw(const_cast<ScrArea *>(area));
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
  printf("[CATEGORY ACTIVATE]   Checking pending insert: pending_insert_valid=%d, pending_tag_key='%s', deferred_tag_key='%s'\n",
         g_deferred_category_activation.pending_insert_valid ? 1 : 0,
         g_deferred_category_activation.pending_insert_tag_key.c_str(),
         g_deferred_category_activation.tag_key.c_str());
  fflush(stdout);

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
  printf("[CATEGORY ACTIVATE]   tag_keys_match=%d\n", tag_keys_match ? 1 : 0);
  fflush(stdout);

  if (tag_keys_match)
  {
    printf("[CATEGORY ACTIVATE]   Applying pending insert position for '%s' (pending_key='%s', deferred_key='%s')\n",
           category_id.c_str(),
           g_deferred_category_activation.pending_insert_tag_key.c_str(),
           g_deferred_category_activation.tag_key.c_str());

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
        printf("[CATEGORY ACTIVATE]     Inserted '%s' at index %d (between '%s' and '%s')\n",
               category_id.c_str(),
               insert_index,
               g_deferred_category_activation.pending_insert_anchor_before.c_str(),
               g_deferred_category_activation.pending_insert_anchor_after.c_str());
      }
      else {
        /* Fallback: append to end */
        current_order.append(category_id);
        printf("[CATEGORY ACTIVATE]     Appended '%s' to end (no anchor found)\n",
               category_id.c_str());
      }

      /* Save updated order to JSON */
      save_category_order_to_json(C, g_deferred_category_activation.pending_insert_tag_key.c_str(), current_order);
      printf("[CATEGORY ACTIVATE]     Order saved to JSON\n");
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
  printf("[CATEGORY ACTIVATE] Deferred activation completed\n");
}

void panel_category_tabs_draw_all(const bContext *C, ARegion *region, const char *category_id_active)
{
  /* Execute deferred category activation if pending.
   * This is a safe point to activate categories after panel layout is complete. */
  deferred_category_activation_execute(C, region);


  const ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : -1;
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  View2D *v2d = &region->v2d;
  const uiStyle *style = style_get();
  const uiFontStyle *fstyle = &style->widget;
  fontstyle_set(fstyle);
  const int fontid = fstyle->uifont_id;
  float fstyle_points = fstyle->points;
  const float raw_aspect = BLI_listbase_is_empty(&region->runtime->uiblocks) ?
                           1.0f :
                           (static_cast<Block *>(region->runtime->uiblocks.first))->aspect;
  /* Stabilize aspect: if very close to 1.0, use exactly 1.0 to avoid floating-point oscillation.
   * This prevents tab size jitter when aspect oscillates between 1.0 and 1.0000001. */
  const float aspect = (std::abs(raw_aspect - 1.0f) < 0.001f) ? 1.0f : raw_aspect;

  CategoryDragState *drag_state = static_cast<CategoryDragState *>(
      region->runtime->category_tabs_drag_state);
  const bool is_dragging = (drag_state != nullptr && drag_state->is_dragging);
  const char *drag_category_id = is_dragging ? drag_state->drag_category_id : "";

  const eUserPref_CategoryTabsDisplayMode display_mode = ED_category_tabs_display_mode_get(area);

  const float category_tabs_zoom = category_tabs_zoom_value_get(area, display_mode);
  const float zoom = (1.0f / aspect) * category_tabs_zoom;

  const wmWindowManager *wm = CTX_wm_manager(C);

  const int px = U.pixelsize;
  const int category_tabs_width = round_fl_to_int(UI_PANEL_CATEGORY_MARGIN_WIDTH * zoom);
  const float dpi_fac = UI_SCALE_FAC;
  /* Calculate too_narrow early - needed for width calculation in first loop */
  const int category_tabs_min_width = category_tabs_min_width_get(area, aspect, display_mode);
  const bool too_narrow = BLI_rcti_size_x(&region->winrct) <= category_tabs_min_width;
  const int tab_v_pad_text = int(std::floor(TABS_PADDING_TEXT_FACTOR * dpi_fac * zoom)) + 2 * px;
  const int tab_v_pad = category_tabs_vertical_padding_calc(zoom);

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
    const char *category_id = pc_dyn.idname;
    const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id, space_type));
    /* When panel is minimized (too_narrow), the active tab should not expand */
    const bool is_active = !too_narrow && category_id_active && STREQ(category_id, category_id_active);

    bool is_fallback_letter = false;
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    const char *glyph = panel_category_glyph_lookup(
        wm, category_id, nullptr, &is_fallback_letter, glyph_color, space_type);

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

    /* --- BEGIN: MIXED_MODE_CONTENT_FLAGS (width calculation) --- */
    /* Resolve icon for this category (needed for Mixed mode width calculation with flags).
     * To remove this feature: delete this icon resolution block and the effective flags below,
     * then replace mixed_mode_has_visible_glyph_content with (has_glyph || is_fallback_letter). */
    const bool display_mode_allows_icon_content = ELEM(
        display_mode, USER_CATEGORY_TABS_GLYPHS_ONLY, USER_CATEGORY_TABS_GLYPHS_TEXT);
    CategoryTabIconResolved icon_resolved;
    panel_category_icon_data_lookup(wm, category_id, &icon_resolved, space_type);
    const int resolved_icon_id = category_tab_icon_id_resolve(icon_resolved);
    bool icon_data_allows_icon_content = (icon_resolved.source != CATEGORY_TAB_ICON_SOURCE_OFF);
    if (icon_resolved.source == CATEGORY_TAB_ICON_SOURCE_MANUAL) {
      icon_data_allows_icon_content = (icon_resolved.key && icon_resolved.key[0] != '\0') ||
                                      (icon_resolved.path && icon_resolved.path[0] != '\0');
    }
    const bool use_builtin_icon =
        display_mode_allows_icon_content && icon_data_allows_icon_content && (resolved_icon_id != ICON_NONE);

    /* In Mixed mode, apply per-content-type visibility flags. */
    const bool mixed_mode_effective_has_glyph =
        has_glyph && U.category_tabs_mixed_show_glyphs;
    const bool mixed_mode_effective_fallback_letter =
        is_fallback_letter && U.category_tabs_mixed_show_first_letter;
    const bool mixed_mode_effective_builtin_icon =
        use_builtin_icon && U.category_tabs_mixed_show_icons;
    /* For width calculation in Mixed mode: consider glyph visible if any content type is enabled. */
    const bool mixed_mode_has_visible_glyph_content =
        mixed_mode_effective_has_glyph || mixed_mode_effective_fallback_letter ||
        mixed_mode_effective_builtin_icon;
    /* --- END: MIXED_MODE_CONTENT_FLAGS (width calculation) --- */

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

        /* Determine if this tab should expand to show the name.
         * Show name for active tab OR for the previous active tab (only in minimized state for STICKY mode).
         *
         * In DEFAULT inactive behavior mode: previous_active tab should NOT expand (stays glyph-only).
         * In STICKY inactive behavior mode: previous_active tab expands to show name ONLY when panel is minimized.
         */
        const bool is_sticky_inactive = (U.category_tabs_inactive_behavior == USER_CATEGORY_TABS_INACTIVE_STICKY);
        const bool is_sticky_mode = (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
                                     U.category_tabs_show_active_name);
        /* Only use previous_active when BOTH sticky inactive AND sticky mode are enabled */
        const bool can_show_previous = is_sticky_inactive && is_sticky_mode;
        const bool is_previous_active_raw = (region->runtime->category_tabs_previous_active_id[0] != '\0' &&
                                             STREQ(category_id, region->runtime->category_tabs_previous_active_id));
        const bool is_previous_active = can_show_previous && too_narrow && is_previous_active_raw;
        /* Active tab can expand in sticky mode. Previous active tab can expand only in sticky inactive mode. */
        const bool should_expand_name = ((is_sticky_mode || is_active) &&
                                         U.category_tabs_show_active_name &&
                                         !region->runtime->category_tabs_active_name_hidden &&
                                         (is_active || is_previous_active));

        if (U.category_tabs_shape == USER_CATEGORY_TABS_SHAPE_BOX) {
          if (!should_expand_name) {
            /* Box shape without name: use square size with no padding */
            current_tab_v_pad_text = 0;
            category_width = square_size + 3.0; // Add padding for Box shape(No change value 3.0!)
          }
        }

        if (should_expand_name) {
          /* Get text width for name expansion */
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

        const char *text_for_width = category_id_draw;
        if (mixed_mode_has_visible_glyph_content) {
          if (is_single_glyph_str(text_for_width)) {
            for (const PanelType &pt : region->runtime->type->paneltypes) {
              if (pt.category && STREQ(pt.category, category_id)) {
                const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
                if (panel_label && panel_label[0]) {
                  text_for_width = panel_label;
                  break;
                }
              }
            }
          }
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

  int current_display_index = 0;

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;

    if (is_dragging && !drag_state->is_reserved && STREQ(pc_dyn.idname, drag_category_id)) {
      current_display_index++;
      continue;
    }

    int y_shift = 0;
    if (is_dragging && !drag_state->is_reserved) {
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
    }
    else if (!is_dragging && region->runtime &&
             region->runtime->extension_drop_preview_state.active)
    {
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
      const int insertion_index = (raw_insertion_index >= 0 && raw_insertion_index > int(ordered_categories.size())) ?
                                   int(ordered_categories.size()) : raw_insertion_index;

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
        if (shift_should_log) {
          printf("[EXT_SHIFT] apply: tab='%s' display_idx=%d >= insertion_idx=%d y_shift=%d\n",
                 pc_dyn.idname,
                 current_display_index,
                 insertion_index,
                 y_shift);
        }
      }
    }

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
    const char *category_id = pc_dyn.idname;
    const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id, space_type));
    const bool is_active = !too_narrow && category_id_active && STREQ(category_id, category_id_active);

    /* Determine if this tab should expand to show the name.
     * Show name for active tab OR for the previous active tab (only in minimized state for STICKY mode).
     *
     * In DEFAULT mode: previous_active tab should NOT expand (stays glyph-only).
     * In STICKY inactive behavior mode: previous_active tab expands to show name ONLY when panel is minimized (too_narrow).
     */
    const bool is_sticky_inactive = (U.category_tabs_inactive_behavior == USER_CATEGORY_TABS_INACTIVE_STICKY);
    const bool is_sticky_mode = (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
                                 U.category_tabs_show_active_name);
    const bool can_show_previous = is_sticky_inactive && is_sticky_mode;
    const bool is_previous_active_raw = (region->runtime->category_tabs_previous_active_id[0] != '\0' &&
                                         STREQ(category_id, region->runtime->category_tabs_previous_active_id));
    const bool is_previous_active = can_show_previous && too_narrow && is_previous_active_raw;
    const bool should_expand_name = ((is_sticky_mode || is_active) &&
                                     U.category_tabs_show_active_name &&
                                     !region->runtime->category_tabs_active_name_hidden &&
                                     (is_active || is_previous_active));

    int current_tab_v_pad_text = tab_v_pad_text;
    if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
        U.category_tabs_shape == USER_CATEGORY_TABS_SHAPE_BOX)
    {
      if (!should_expand_name) {
        current_tab_v_pad_text = 0;
      }
    }

    /* Get glyph color for color indicator in TEXT_ONLY mode. */
    bool is_fallback_letter = false;
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    panel_category_glyph_lookup(
        wm, category_id, nullptr, &is_fallback_letter, glyph_color, space_type);

    current_display_index++;

    const bool is_hover = BLI_rcti_isect_pt(rct, mouse_x, mouse_y);

    float darken_factor = 0.0f;
    if (!is_active && !is_hover) {
      darken_factor = TABS_GLYPH_DARKEN_BASE;
    }

    float bg_brighten_factor = 0.0f;
    if (!is_active) {
      bg_brighten_factor = is_hover ? TABS_BG_BRIGHTEN_HOVER : TABS_BG_BRIGHTEN_BASE;
    }

    GPU_blend(GPU_BLEND_ALPHA);

    // --- BEGIN: TABS_VISUAL_EFFECT_OVERLAP ---
    /* Visual effect: expand tab on hover/active.
     * The tab expands by consuming padding from neighbors first, then overlapping.
     * This keeps the tab grid stable (no jitter) while providing visual feedback.
     */
    bool is_visual_effect_active = false;
    rctf box_rect;
    box_rect.xmin = float(rct->xmin);
    box_rect.xmax = float(rct->xmax);
    box_rect.ymin = float(rct->ymin);
    box_rect.ymax = float(rct->ymax);

    /* Allow visual effect even when "Show Active Tab Name" is enabled, but skip the
     * active tab to avoid stretching the expanded label layout. */
    const bool visual_effect_allowed_for_tab = (U.category_tabs_visual_effect &&
                                                display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
                                                !is_dragging &&
                                                (!U.category_tabs_show_active_name || !is_active));

    if (visual_effect_allowed_for_tab)
    {
      if (is_active || is_hover) {
        is_visual_effect_active = true;

        /* Vertical expansion: scale height by UI_TABS_VISUAL_EFFECT_SCALE (1.2) */
        const int tab_height = rct->ymax - rct->ymin;
        const int expanded_height = round_fl_to_int(tab_height * UI_TABS_VISUAL_EFFECT_SCALE);
        const int extra_height = expanded_height - tab_height;

        /* Distribute extra height equally: half up, half down */
        const int extra_top = extra_height / 2;
        const int extra_bottom = extra_height - extra_top;

        /* Expand vertically (consumes padding first, then overlaps neighbors) */
        box_rect.ymin -= extra_bottom;
        box_rect.ymax += extra_top;

        /* Horizontal expansion: expand away from the panel edge */
        const int tab_width = rct->xmax - rct->xmin;
        const int expanded_width = round_fl_to_int(tab_width * UI_TABS_VISUAL_EFFECT_SCALE);
        const int extra_width = expanded_width - tab_width;

        if (is_left) {
          box_rect.xmax += extra_width;
        }
        else {
          box_rect.xmin -= extra_width;
        }
      }
    }
    // --- END: TABS_VISUAL_EFFECT_OVERLAP ---

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
      draw_roundbox_4fv(&box_rect, false, tab_curve_radius,
                        is_active ? theme_col_tab_outline_sel : theme_col_tab_outline);

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
        pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
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

    /* Prepare expanded rect for content drawing when visual effect is active */
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

    if (is_left) {
      pc_dyn.rect.xmin = v2d->mask.xmin;
    }
    else {
      pc_dyn.rect.xmax = v2d->mask.xmax;
    }
  }

  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
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
      panel_category_glyph_lookup(
          wm, category_id, nullptr, &is_fallback_letter, glyph_color, space_type);

      /* Draw color indicator bar for TEXT_ONLY mode to show assigned glyph color. */
      draw_category_tab_color_indicator(
          &drag_rect, glyph_color, is_left, display_mode, U.category_tabs_text_mode_show_color_indicator, true);

      const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id, space_type));
      const rcti *rct = &drag_rect;

      const bool is_active_tab = category_id_active && STREQ(category_id, category_id_active);

      /* Determine if this tab should expand to show the name.
       * Show name for active tab OR for the previous active tab (only in minimized state for STICKY mode).
       *
       * During drag: too_narrow check is not available, so use a basic flag.
       * In STICKY inactive behavior mode (GLYPHS_ONLY + show_active_name), expand for previous_active.
       * In DEFAULT inactive behavior mode: previous_active tab should NOT expand.
       */
      const bool is_sticky_inactive = (U.category_tabs_inactive_behavior == USER_CATEGORY_TABS_INACTIVE_STICKY);
      const bool is_sticky_mode = (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
                                   U.category_tabs_show_active_name);
      const bool can_show_previous = is_sticky_inactive && is_sticky_mode;
      const bool is_previous_active_raw = (region->runtime->category_tabs_previous_active_id[0] != '\0' &&
                                           STREQ(category_id, region->runtime->category_tabs_previous_active_id));
      const bool is_previous_active = can_show_previous && is_previous_active_raw;
      const bool should_expand_name = ((is_sticky_mode || is_active_tab) &&
                                       U.category_tabs_show_active_name &&
                                       !region->runtime->category_tabs_active_name_hidden &&
                                       (is_active_tab || is_previous_active));

      int current_drag_tab_v_pad_text = tab_v_pad_text;
      if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
          U.category_tabs_shape == USER_CATEGORY_TABS_SHAPE_BOX)
      {
        if (!should_expand_name) {
          current_drag_tab_v_pad_text = 0;
        }
      }

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
  }

  const char *ghost_skip_reason = nullptr;

  /* Draw ghost tab for extension drop preview (only if not dragging a category) */
  if (!is_dragging && region->runtime && region->runtime->extension_drop_preview_state.active) {
    const ui::ExtensionDropPreviewState &preview = region->runtime->extension_drop_preview_state;

    rcti ghost_rect;
    /* Use same X positioning as regular tabs (respects is_left alignment) */
    ghost_rect.xmin = rct_xmin;
    ghost_rect.xmax = rct_xmax;

    /* Ghost height: compact fixed indicator (~10 px). */
    const int ghost_height = EXTENSION_DROP_GHOST_HEIGHT;

    /* Debug: log ghost position info (time-limited: max once per second) */
    static double _ghost_last_log_time = 0.0;
    static int _ghost_last_target = -999;
    const double current_time = BLI_time_now_seconds();
    const bool should_log = (current_time - _ghost_last_log_time > 1.0) ||
                            (_ghost_last_target != preview.target_index);

    if (should_log) {
      /* Log all existing tabs positions for debugging */
      printf("[EXT_GHOST] === TABS POSITIONS (top to bottom) ===\n");
      int log_idx = 0;
      for (PanelCategoryDyn *pc : ordered_categories) {
        printf("[EXT_GHOST]   tab[%d] '%s' y=[%d,%d] h=%d\n",
               log_idx, pc->idname,
               pc->rect.ymin, pc->rect.ymax,
               BLI_rcti_size_y(&pc->rect));
        log_idx++;
        if (log_idx > 10) {
          printf("[EXT_GHOST]   ... (more tabs)\n");
          break;
        }
      }
      printf("[EXT_GHOST] v2d_mask: y=[%d,%d] x=[%d,%d]\n",
             v2d->mask.ymin, v2d->mask.ymax,
             v2d->mask.xmin, v2d->mask.xmax);
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

    if (preview.target_index >= 0 && preview.target_index < int(ordered_categories.size())) {
      PanelCategoryDyn *target_tab = nullptr;

      /* Find target tab by NAME, not by index!
       * The index from screen_ops.cc is based on panels_category order,
       * but ordered_categories may have different sorting. */
      bool found_by_name = false;
      int resolved_target_index = -1;
      if (preview.target_category_id[0] != '\0') {
        int idx = 0;
        for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
          if (STREQ(pc_dyn_ptr->idname, preview.target_category_id)) {
            target_tab = pc_dyn_ptr;
            found_by_name = true;
            resolved_target_index = idx;
            break;
          }
          idx++;
        }
      }

      /* Fallback to index if name not found */
      if (!target_tab) {
        int loop_idx = 0;
        for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
          if (loop_idx == preview.target_index) {
            target_tab = pc_dyn_ptr;
            resolved_target_index = loop_idx;
            break;
          }
          loop_idx++;
        }
        if (should_log && target_tab) {
          printf("[EXT_GHOST] Found by INDEX fallback: '%s' at idx=%d\n",
                 target_tab->idname, preview.target_index);
        }
      }

      if (target_tab) {
        /* UNIFIED INSERTION SLOT MODEL:
         * insertion_index = target_index + (insert_above ? 0 : 1)
         * Ghost should be centered in the slot between tabs at insertion_index-1 and insertion_index
         * after tab shifting creates space. */
        const int category_count = int(ordered_categories.size());
        const int raw_insertion_index = (resolved_target_index >= 0) ?
                                            (resolved_target_index + (preview.insert_above ? 0 : 1)) :
                                            -1;
        const int insertion_index = raw_insertion_index;
        const int shift_space = EXTENSION_DROP_GHOST_HEIGHT + preview.tab_v_pad;
        
        /* Find slot boundaries after tab shifting */
        int slot_top_y, slot_bottom_y;
        
        if (insertion_index == 0) {
          /* Insert at top: all tabs shift DOWN, ghost appears above first tab's original position.
           * Ghost slot: from ymax to (ymax + shift_space) */
          slot_top_y = target_tab->rect.ymax + shift_space;
          slot_bottom_y = target_tab->rect.ymax;
        }
        else if (insertion_index >= int(ordered_categories.size())) {
          /* Insert at end: treat as inserting below the actual last tab. */
          /* This prevents creating empty space beyond existing tabs. */
          PanelCategoryDyn *last_tab = ordered_categories[int(ordered_categories.size()) - 1];
          slot_top_y = last_tab->rect.ymin;
          slot_bottom_y = slot_top_y - shift_space;
        }
        else {
          /* Insert between tabs: find boundaries of the created slot.
           * After fix: tabs shift DOWN (y_shift negative), so ghost appears
           * in the space ABOVE where the shifted tabs end up. */
          if (preview.insert_above) {
            /* Inserting above target: target shifts DOWN, ghost fills the gap above.
             * Ghost slot: from (ymax - shift_space) to ymax */
            slot_top_y = target_tab->rect.ymax;
            slot_bottom_y = slot_top_y - shift_space;
          }
          else {
            /* Inserting below target: tabs below shift DOWN, ghost fills the gap.
             * Ghost slot: from (ymin - shift_space) to ymin */
            slot_top_y = target_tab->rect.ymin;
            slot_bottom_y = slot_top_y - shift_space;
          }
        }
        
        /* Center ghost in the insertion slot.
         * Ghost should be perfectly centered in the free space between tabs. */
        const int slot_center_y = (slot_top_y + slot_bottom_y) / 2;
        const int half_ghost_height = ghost_height / 2;

        /* Ghost rect centered in the slot */
        ghost_rect.ymin = slot_center_y - half_ghost_height;
        ghost_rect.ymax = slot_center_y + half_ghost_height;

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

        if (should_log) {
          const int ghost_center_y = (ghost_rect.ymin + ghost_rect.ymax) / 2;
          const int center_diff = abs(ghost_center_y - slot_center_y);
          printf("[EXT_GHOST] FINAL: cursor_y=%d ghost=[%d,%d] center=%d slot_center=%d diff=%d target='%s'\n",
                 preview.cursor_y, ghost_rect.ymin, ghost_rect.ymax,
                 ghost_center_y, slot_center_y, center_diff,
                 target_tab->idname);
          if (center_diff > 1) {
            printf("[EXT_GHOST] WARNING: Ghost not centered in slot! (diff=%d px)\n", center_diff);
          }
        }
      }
      else {
        printf("[EXT_GHOST] SKIP: target_tab NULL for name='%s' idx=%d\n",
               preview.target_category_id, preview.target_index);
        ghost_skip_reason = "target_tab_null";
        goto skip_extension_ghost;
      }
    }
    else {
      /* No target tab - position at top of region */
      ghost_rect.ymin = v2d->mask.ymax - ghost_height - 10;
      ghost_rect.ymax = v2d->mask.ymax - 10;
      if (should_log) {
        printf("[EXT_GHOST] FINAL (no target): ghost y=[%d,%d]\n",
               ghost_rect.ymin, ghost_rect.ymax);
      }
    }

    /* Ensure ghost is within viewport bounds */
    const bool ghost_in_viewport = (ghost_rect.ymax >= v2d->mask.ymin && ghost_rect.ymin <= v2d->mask.ymax);
    
    if (should_log) {
      printf("[EXT_GHOST] VIEWPORT CHECK: ghost=[%d,%d] viewport=[%d,%d] in_viewport=%d\n",
             ghost_rect.ymin, ghost_rect.ymax,
             v2d->mask.ymin, v2d->mask.ymax,
             ghost_in_viewport ? 1 : 0);
    }
    
    if (!ghost_in_viewport) {
      /* Clamp ghost to viewport if outside */
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
      
      if (should_log) {
        printf("[EXT_GHOST] CLAMPED: ghost=[%d,%d]\n", ghost_rect.ymin, ghost_rect.ymax);
      }
    }

    rctf ghost_box_rect;
    ghost_box_rect.xmin = float(ghost_rect.xmin);
    ghost_box_rect.xmax = float(ghost_rect.xmax);
    ghost_box_rect.ymin = float(ghost_rect.ymin);
    ghost_box_rect.ymax = float(ghost_rect.ymax);

    float ghost_bg_color[4];
    copy_v4_v4(ghost_bg_color, theme_col_tab_active);
    ghost_bg_color[3] = 0.5f;  /* Make more visible for debugging */

    GPU_blend(GPU_BLEND_ALPHA);
    draw_roundbox_corner_set(roundboxtype);
    draw_roundbox_4fv(&ghost_box_rect, true, tab_curve_radius, ghost_bg_color);

    float ghost_outline[4];
    copy_v3_v3(ghost_outline, theme_col_tab_outline_sel);
    ghost_outline[3] = 0.8f;  /* Make outline more visible */
    draw_roundbox_4fv(&ghost_box_rect, false, tab_curve_radius, ghost_outline);

    GPU_blend(GPU_BLEND_NONE);
    if (should_log) {
      printf("[EXT_GHOST] DRAWN: ghost_rect=[%d,%d]-[%d,%d]\n",
             ghost_rect.xmin,
             ghost_rect.ymin,
             ghost_rect.xmax,
             ghost_rect.ymax);
    }
  }
skip_extension_ghost:
  if (!is_dragging && region->runtime && region->runtime->extension_drop_preview_state.active) {
    const ui::ExtensionDropPreviewState &preview = region->runtime->extension_drop_preview_state;
    if (ghost_skip_reason) {
      printf("[EXT_GHOST] SKIPPED: reason=%s active=%d target='%s' idx=%d cursor_y=%d\n",
             ghost_skip_reason,
             preview.active ? 1 : 0,
             preview.target_category_id,
             preview.target_index,
             preview.cursor_y);
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
