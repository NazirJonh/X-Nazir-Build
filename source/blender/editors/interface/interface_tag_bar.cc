/* SPDX-FileCopyrightText: 2026 Nazir Galimov */
/*
 * Horizontal Tag Bar for Category Filtering
 */

#include <map>
#include <optional>
#include <algorithm>
#include <iostream>
#include <cstdio>

#include "interface_tag_bar.hh"
#include "interface_intern.hh"
#include "interface_category_py_bridge.hh"

#include "DNA_userdef_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"



#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "RNA_enum_types.hh"

#include "MEM_guardedalloc.h"

#include "BLF_api.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

namespace blender::ui {

using blender::wmWindowManager;
using blender::bContext;
using blender::View3D;
using blender::SpaceProperties;
using blender::SpaceNode;
using blender::SpaceImage;
using blender::ScrArea;
using blender::CategoryTagDef;
using blender::ARegion;
using blender::wmNotifier;
using blender::wmEvent;
using blender::wmRegionListenerParams;
using blender::wmRegionMessageSubscribeParams;
using blender::Vector;
using blender::Span;

/* Global cache for tag bar data, keyed by wmWindowManager pointer */
static std::map<const wmWindowManager *, TagBarRuntimeData *> g_tag_bar_cache;

/* Master debug flag for tag bar operations. Declared in interface_intern.hh and
 * defined in interface_debug_flags.cc (the single source of truth for interface
 * debug flags). Set it to true there to enable output. */
using blender::g_tag_bar_debug_enabled;

/* Forward declarations */
static bool is_tag_valid_for_mode(const wmWindowManager *wm,
                                   const char *tag_name,
                                   uint32_t mode_flag);
static void tag_filter_append_valid_tags_for_mode(const wmWindowManager *wm,
                                                  const char *tags_string,
                                                  uint32_t mode_flag,
                                                  Vector<std::string> &r_tags);
static void tag_filter_join_tags(const Vector<std::string> &tags,
                                 char *r_tags,
                                 int max_len);
static const char *tag_filter_last_tag_in_string(const char *tags_string,
                                                 char *r_tag,
                                                 int max_len);

/**
 * Convert CategoryTagMode bit flag to category_tag_filter_mode enum value.
 * CategoryTagMode uses bit flags (1 << n), while category_tag_filter_mode uses
 * sequential enum values (n + 1).
 *
 * Mapping:
 * - CategoryTagMode::OBJECT_MODE (1 << 0) = 1 → enum value 1
 * - CategoryTagMode::EDIT_MODE (1 << 1) = 2 → enum value 2
 * - etc.
 *
 * Note: Detailed edit modes (MESH_EDIT, CURVE_EDIT, etc.) are mapped to EDIT_MODE (2)
 * since the Preferences filter enum doesn't have separate values for them.
 */
static int category_tag_mode_flag_to_filter_enum(uint32_t mode_flag)
{
  if (mode_flag == 0) {
    return 0; /* ALL */
  }

  /* Special handling for detailed edit modes: map them all to EDIT_MODE (2).
   * This matches the RNA enum which only has EDIT_MODE, not MESH_EDIT, etc. */
  if (mode_flag & static_cast<uint32_t>(CategoryTagMode::EDIT_MODE_MASK)) {
    return 2; /* EDIT_MODE */
  }

  /* Find the bit position and add 1 to get the enum value */
  for (int bit = 0; bit < 32; bit++) {
    if (mode_flag == (1u << bit)) {
      return bit + 1;
    }
  }

  /* If multiple bits or unknown, default to 0 (ALL) */
  return 0;
}

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

/**
 * Mark all cached tag bar data as needing update.
 * Called from listeners when glyphs or tags change.
 */
void tag_bar_mark_all_dirty()
{
  for (auto &entry : g_tag_bar_cache) {
    if (entry.second) {
      entry.second->needs_update = true;
    }
  }
}

void tag_bar_runtime_free_all()
{
  for (auto &entry : g_tag_bar_cache) {
    MEM_delete(entry.second);
  }
  g_tag_bar_cache.clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Functions
 * \{ */

/* Forward declarations for per-mode tag filter state functions
 * (defined later in this file, but needed by get_tag_bar_data_global). */
static char *tag_filter_per_mode_storage_get(const ScrArea *area);
static uint32_t tag_filter_get_last_mode(const ScrArea *area);
static void tag_filter_set_last_mode(ScrArea *area, uint32_t mode);
static void tag_filter_per_mode_save(ScrArea *area,
                                      uint32_t mode_flag,
                                      const char *tags,
                                      int enabled);

/**
 * Per-mode tag filter save/restore.
 *
 * Detect mode changes and save/restore the per-mode tag filter selection. When the user switches
 * from Object Mode to Edit Mode (or any other mode), the current selection is saved for the old
 * mode and a selection is rebuilt for the new mode (carrying over shared tags where possible).
 *
 * Extracted from #get_tag_bar_data_global so the cache/update bookkeeping stays readable. Sets
 * `data->needs_update` when the active selection changed and a rebuild is required.
 *
 * Caller guarantees `state.active_tags` and `state.filter_enabled` are non-null.
 */
static void tag_bar_apply_per_mode_state(const bContext *C,
                                         wmWindowManager *wm,
                                         TagFilterStateRef &state,
                                         TagBarRuntimeData *data)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return;
  }

  const uint32_t current_mode = get_current_tag_mode_flag(C);
  const uint32_t last_mode = tag_filter_get_last_mode(area);

  if (last_mode == 0) {
    /* First call: initialize last_mode and save current state as baseline
     * so that when the user switches away and comes back, the tag is restored. */
    tag_filter_per_mode_save(area, current_mode, state.active_tags, *state.filter_enabled);
    tag_filter_set_last_mode(area, current_mode);
    return;
  }

  if (current_mode == last_mode) {
    return;
  }

  /* Mode changed! Remember current tags before save/restore for carry-over. */
  char prev_tags[256];
  BLI_strncpy(prev_tags, state.active_tags, sizeof(prev_tags));
  const bool prev_enabled = (*state.filter_enabled != 0);

  /* Save current state for the previous mode. */
  tag_filter_per_mode_save(area, last_mode, state.active_tags, *state.filter_enabled);

  /* Rebuild the active tag list for the new mode from all tags that are valid there.
   * This preserves shared selections across modes and supports multi-select. */
  Vector<std::string> merged_tags;
  if (prev_enabled) {
    tag_filter_append_valid_tags_for_mode(wm, prev_tags, current_mode, merged_tags);
  }

  if (!merged_tags.is_empty()) {
    tag_filter_join_tags(merged_tags, state.active_tags, 256);
    *state.filter_enabled = 1;

    /* Update saved state to reflect the merged selection in the new mode. */
    tag_filter_per_mode_save(area, current_mode, state.active_tags, *state.filter_enabled);
  }
  else if (prev_enabled) {
    /* No shared tags remain. Fall back to the last selected tag from the previous mode.
     * This keeps the filter useful when the intersection is empty. */
    char last_tag[64] = "";
    if (tag_filter_last_tag_in_string(prev_tags, last_tag, sizeof(last_tag)) &&
        is_tag_valid_for_mode(wm, last_tag, current_mode))
    {
      BLI_strncpy(state.active_tags, last_tag, 256);
      *state.filter_enabled = 1;
      tag_filter_per_mode_save(area, current_mode, state.active_tags, *state.filter_enabled);
    }
    else {
      state.active_tags[0] = '\0';
      *state.filter_enabled = 0;
      tag_filter_per_mode_save(area, current_mode, state.active_tags, *state.filter_enabled);
    }
  }
  else {
    /* No saved state for this mode and nothing valid to carry over. */
    state.active_tags[0] = '\0';
    *state.filter_enabled = 0;

    /* Save initial state for new mode (even if empty) so future returns
     * to this mode restore the correct state. */
    tag_filter_per_mode_save(area, current_mode, state.active_tags, *state.filter_enabled);
  }

  /* Update last mode. */
  tag_filter_set_last_mode(area, current_mode);

  /* Force rebuild. */
  data->needs_update = true;
}

/**
 * Get or create tag bar runtime data from global cache.
 * Creates the data if it doesn't exist.
 * Updates data if needs_update flag is set.
 */
TagBarRuntimeData *get_tag_bar_data_global(const bContext *C)
{
  if (!C) {
    if (g_tag_bar_debug_enabled) {
      printf("[TAG BAR DATA] get_tag_bar_data_global: C=NULL\n");
      fflush(stdout);
    }
    return nullptr;
  }

  wmWindowManager *wm = CTX_wm_manager(C);

  if (!wm) {
    if (g_tag_bar_debug_enabled) {
      printf("[TAG BAR DATA] get_tag_bar_data_global: wm=NULL\n");
      fflush(stdout);
    }
    return nullptr;
  }

  /* Free cache entries that belong to a different (now freed) window manager.
   * Blender keeps a single active wmWindowManager; when a new .blend is loaded
   * the previous manager is destroyed, leaving stale entries with dangling keys.
   * Reclaim them here so the cache never leaks or returns a stale object. */
  for (auto it = g_tag_bar_cache.begin(); it != g_tag_bar_cache.end();) {
    if (it->first != wm) {
      MEM_delete(it->second);
      it = g_tag_bar_cache.erase(it);
    }
    else {
      ++it;
    }
  }

  TagBarRuntimeData *data = g_tag_bar_cache[wm];

  if (!data) {
    data = MEM_new<TagBarRuntimeData>(__func__);
    g_tag_bar_cache[wm] = data;
    if (g_tag_bar_debug_enabled) {
      printf("[TAG BAR DATA] created runtime data: wm=%p data=%p\n", (void*)wm, (void*)data);
      fflush(stdout);
    }
  }

  TagFilterStateRef state{};
  const bool has_state = tag_filter_state_from_context(C, &state);

  if (has_state && state.active_tags && state.filter_enabled) {
    tag_bar_apply_per_mode_state(C, wm, state, data);
  }

  if (g_tag_bar_debug_enabled) {
    printf("[TAG BAR DATA] fetch: wm=%p data=%p needs_update=%d has_state=%d\n",
           (void*)wm, (void*)data, int(data->needs_update), int(has_state));
    fflush(stdout);
  }

  /* Update data if needs_update flag is set */
  if (data->needs_update) {
    if (g_tag_bar_debug_enabled) {
      printf("[TAG BAR DATA] calling tag_bar_buttons_update\n");
      fflush(stdout);
    }
    tag_bar_buttons_update(C, wm, has_state ? &state : nullptr, data);
    data->needs_update = false;
    if (g_tag_bar_debug_enabled) {
      printf("[TAG BAR DATA] update complete: button_count=%zu show_new_addon_button=%d unassigned_count=%d\n",
           data->buttons.size(), int(data->show_new_addon_button), data->unassigned_count);
      fflush(stdout);
    }
  }

  return data;
}

bool tag_filter_state_from_area(const ScrArea *area, TagFilterStateRef *r_state)
{
  if (!r_state) {
    return false;
  }

  *r_state = TagFilterStateRef{};
  if (!area || !area->spacedata.first) {
    return false;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      r_state->active_tags = v3d->tabs_state.active_tag_filter_tags;
      r_state->filter_enabled = &v3d->tabs_state.tag_filter_enabled;
      r_state->scroll_offset = &v3d->tabs_state.tag_bar_scroll_offset;
      return true;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      r_state->active_tags = sbuts->tabs_state.active_tag_filter_tags;
      r_state->filter_enabled = &sbuts->tabs_state.tag_filter_enabled;
      r_state->scroll_offset = &sbuts->tabs_state.tag_bar_scroll_offset;
      return true;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      r_state->active_tags = snode->tabs_state.active_tag_filter_tags;
      r_state->filter_enabled = &snode->tabs_state.tag_filter_enabled;
      r_state->scroll_offset = &snode->tabs_state.tag_bar_scroll_offset;
      return true;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      r_state->active_tags = sima->tabs_state.active_tag_filter_tags;
      r_state->filter_enabled = &sima->tabs_state.tag_filter_enabled;
      r_state->scroll_offset = &sima->tabs_state.tag_bar_scroll_offset;
      return true;
    }
  }

  return false;
}

bool tag_filter_state_from_context(const bContext *C, TagFilterStateRef *r_state)
{
  return tag_filter_state_from_area(CTX_wm_area(C), r_state);
}

/**
 * Check if a tag exists in a comma-separated tag string.
 */
bool has_tag_in_string(const char *tags_string, const char *tag_name)
{
  if (!tags_string || !tags_string[0] || !tag_name || !tag_name[0]) {
    return false;
  }

  const char *cursor = tags_string;
  char tag[64];

  while (*cursor) {
    /* Skip whitespace */
    while (*cursor == ' ') {
      cursor++;
    }

    if (!*cursor) {
      break;
    }

    int i = 0;
    while (*cursor && *cursor != ',' && *cursor != ';' && i < 63) {
      tag[i++] = *cursor++;
    }

    while (i > 0 && tag[i - 1] == ' ') {
      i--;
    }
    tag[i] = '\0';

    if (*cursor == ',' || *cursor == ';') {
      cursor++;
    }

    if (tag[0] == '\0') {
      continue;
    }

    if (STREQ(tag, tag_name)) {
      return true;
    }
  }

  return false;
}

/**
 * Check if any active tag matches the given tag string (OR logic).
 */
bool has_any_tag_active(const wmWindowManager *wm,
                        const char *tags_string,
                        int64_t active_mask)
{
  if (active_mask == 0) {
    return true;  /* Filter not active - show all */
  }

  if (!tags_string || !tags_string[0]) {
    return false;  /* No tags - hide */
  }

  Vector<std::string> tags;
  category_tab_split_tags(tags_string, tags, ",;");

  for (const std::string &tag : tags) {
    /* Find tag_def for this tag */
    if (wm && category_tag_list_is_valid(&wm->category_tags)) {
      for (const CategoryTagDef *tag_def =
               static_cast<const CategoryTagDef *>(wm->category_tags.first);
           tag_def;
           tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
      {
        if (STREQ(tag.c_str(), tag_def->name)) {
          if (active_mask & tag_def->mode_flags) {
            return true;  /* This tag is active */
          }
          break;
        }
      }
    }
  }

  return false;
}

/**
 * Check if ALL active tags match the given tag string (AND logic).
 * When multiple tags are active, the category must have ALL of them to be visible.
 */
bool has_all_tags_active(const wmWindowManager *wm,
                         const char *tags_string,
                         int64_t active_mask)
{
  if (active_mask == 0) {
    return true;  /* Filter not active - show all */
  }

  if (!tags_string || !tags_string[0]) {
    return false;  /* No tags - hide */
  }

  /* First, collect all tag bits that the category has */
  int64_t category_tag_bits = 0;

  Vector<std::string> tags;
  category_tab_split_tags(tags_string, tags, ",;");

  for (const std::string &tag : tags) {
    /* Find tag_def for this tag and add its bit to category_tag_bits */
    if (wm && category_tag_list_is_valid(&wm->category_tags)) {
      for (const CategoryTagDef *tag_def =
               static_cast<const CategoryTagDef *>(wm->category_tags.first);
           tag_def;
           tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
      {
        if (STREQ(tag.c_str(), tag_def->name)) {
          category_tag_bits |= tag_def->mode_flags;
          break;
        }
      }
    }
  }

  /* Check if ALL active tag bits are present in category's tags */
  /* (active_mask & category_tag_bits) must equal active_mask for AND logic */
  return (active_mask & category_tag_bits) == active_mask;
}

/**
 * Check if a tag name is valid (visible) for a given mode flag.
 * Uses the same mode-matching logic as tag_bar_buttons_update:
 * - mode_flags == 0 means tag is valid for all modes
 * - Direct bit match with mode_flag
 * - EDIT_MODE generalization for detailed edit modes
 */
static bool is_tag_valid_for_mode(const wmWindowManager *wm,
                                   const char *tag_name,
                                   uint32_t mode_flag)
{
  if (!tag_name || !tag_name[0]) {
    return false;
  }
  if (!wm || !category_tag_list_is_valid(&wm->category_tags)) {
    return true; /* No tag definitions → assume valid */
  }
  for (const CategoryTagDef *tag_def =
           static_cast<const CategoryTagDef *>(wm->category_tags.first);
       tag_def;
       tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
  {
    if (STREQ(tag_def->name, tag_name)) {
      if (tag_def->mode_flags == 0) {
        return true; /* Tag valid for all modes */
      }
      if (tag_def->mode_flags & mode_flag) {
        return true; /* Direct mode match */
      }
      if ((mode_flag & static_cast<uint32_t>(CategoryTagMode::EDIT_MODE_MASK)) &&
          (tag_def->mode_flags & static_cast<uint32_t>(CategoryTagMode::EDIT_MODE)))
      {
        return true; /* EDIT_MODE generalization */
      }
      return false;
    }
  }
  return false; /* Tag not found in definitions */
}

/**
 * Append tags that are valid for a given mode to a destination list.
 * Preserves order, skips invalid tags, and avoids duplicates.
 */
static void tag_filter_append_valid_tags_for_mode(const wmWindowManager *wm,
                                                  const char *tags_string,
                                                  uint32_t mode_flag,
                                                  Vector<std::string> &r_tags)
{
  if (!tags_string || !tags_string[0]) {
    return;
  }

  Vector<std::string> tags;
  category_tab_split_tags(tags_string, tags, ",;");

  for (const std::string &tag : tags) {
    if (!is_tag_valid_for_mode(wm, tag.c_str(), mode_flag)) {
      continue;
    }
    if (std::find(r_tags.begin(), r_tags.end(), tag) == r_tags.end()) {
      r_tags.append(tag);
    }
  }
}

/**
 * Join a list of tags into a comma-separated string.
 */
static void tag_filter_join_tags(const Vector<std::string> &tags,
                                 char *r_tags,
                                 int max_len)
{
  if (!r_tags || max_len <= 0) {
    return;
  }

  r_tags[0] = '\0';
  for (int i = 0; i < tags.size(); i++) {
    if (i > 0) {
      BLI_strncat(r_tags, ",", max_len);
    }
    BLI_strncat(r_tags, tags[i].c_str(), max_len);
  }
}

/**
 * Extract the last tag from a comma/semicolon separated tag string.
 * Returns r_tag on success, nullptr otherwise.
 */
static const char *tag_filter_last_tag_in_string(const char *tags_string,
                                                 char *r_tag,
                                                 int max_len)
{
  if (!tags_string || !tags_string[0] || !r_tag || max_len <= 0) {
    return nullptr;
  }

  Vector<std::string> tags;
  category_tab_split_tags(tags_string, tags, ",;");
  if (tags.is_empty()) {
    return nullptr;
  }

  BLI_strncpy(r_tag, tags.last().c_str(), max_len);
  return r_tag;
}

/**
 * Build a single tag-bar button from a tag definition.
 *
 * Resolves the icon, computes mode-aware visibility, active state and the count of categories
 * carrying the tag. Extracted from #tag_bar_buttons_update to keep its iteration loop readable.
 */
static TagButton tag_button_from_def(const wmWindowManager *wm,
                                     const CategoryTagDef *tag_def,
                                     const char *active_tags,
                                     const uint32_t current_mode_flag)
{
  TagButton btn;
  STRNCPY(btn.tag_name, tag_def->name);
  STRNCPY(btn.glyph, tag_def->glyph);
  copy_v3_v3(btn.color, tag_def->color);

  /* Store icon_key for fallback rendering. */
  STRNCPY(btn.icon_key, tag_def->icon_key);

  /* Resolve icon from icon_key. */
  btn.icon_id = ICON_NONE;
  btn.use_builtin_icon = false;

  if (g_tag_bar_debug_enabled) {
    printf("[TAG_BAR] tag='%s' icon_source=%d icon_key='%s' glyph='%s'\n",
           tag_def->name, tag_def->icon_source, tag_def->icon_key, tag_def->glyph);
  }

  if (tag_def->icon_source == 1 && tag_def->icon_key[0] != '\0') {
    /* icon_source == 1 means use Blender icon. */
    btn.icon_id = category_tab_icon_id_resolve_from_key_path(tag_def->icon_key, nullptr);
    btn.use_builtin_icon = (btn.icon_id != ICON_NONE);
    if (g_tag_bar_debug_enabled) {
      printf("[TAG_BAR] tag='%s' resolved icon_id=%d use_builtin_icon=%d\n",
             tag_def->name, btn.icon_id, btn.use_builtin_icon);
    }
  }

  /* Visibility:
   * 1. Must have a glyph (not empty) OR icon (if using icon mode).
   * 2. Must be active for current mode (mode_flags check). */
  const bool has_glyph = (tag_def->glyph[0] != '\0');
  /* has_icon is based on icon_key presence, not icon_id resolution success: icon_id may be
   * ICON_NONE if resolution fails, but the tag should still show. */
  const bool has_icon = (tag_def->icon_source == 1 && tag_def->icon_key[0] != '\0');

  if (g_tag_bar_debug_enabled) {
    printf("[TAG_BAR] tag='%s' has_glyph=%d has_icon=%d mode_flags=%u current_mode_flag=%u\n",
           tag_def->name, has_glyph, has_icon, tag_def->mode_flags, current_mode_flag);
  }

  if (!has_glyph && !has_icon) {
    btn.is_visible = false;
  }
  else {
    /* Mode visibility is centralized in is_tag_valid_for_mode (handles mode_flags == 0 as
     * all-modes, direct bit match and EDIT_MODE generalization for detailed edit modes). */
    btn.is_visible = is_tag_valid_for_mode(wm, tag_def->name, current_mode_flag);
  }

  if (g_tag_bar_debug_enabled) {
    printf("[TAG_BAR] tag='%s' final is_visible=%d\n", tag_def->name, btn.is_visible);
  }

  btn.is_hovered = false;
  /* Check if this tag is in the active tags list. */
  btn.is_active = has_tag_in_string(active_tags, tag_def->name);

  /* Count categories with this tag. */
  btn.category_count = 0;
  if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (has_tag_in_string(item->tags, tag_def->name)) {
        btn.category_count++;
      }
    }
  }

  return btn;
}

/**
 * Compute whether the virtual "New Add-on!" button should be shown and populate it.
 * Extracted from #tag_bar_buttons_update.
 */
static void tag_bar_update_new_addon_button(const bContext *C,
                                            const wmWindowManager *wm,
                                            TagBarRuntimeData *data,
                                            const uint32_t current_mode_flag)
{
  const ScrArea *area = C ? CTX_wm_area(C) : nullptr;
  const ARegion *region = C ? CTX_wm_region(C) : nullptr;
  const int space_type = area ? area->spacetype : -1;
  if (g_tag_bar_debug_enabled) {
    printf("[TAG BAR DEBUG] area=%p space_type=%d wm=%p mode_flag=0x%x\n",
           (const void*)area, space_type, (void*)wm, current_mode_flag);
    fflush(stdout);
  }

  /* Use region-aware check to filter out categories whose panels are not visible
   * (e.g., due to poll() returning false in the current context). */
  const bool show_new_addon = wm && should_show_new_addon_tag_for_region(wm, region, space_type, current_mode_flag);

  if (g_tag_bar_debug_enabled) {
    printf("[TAG BAR DEBUG] show_new_addon=%d\n", (show_new_addon ? 1 : 0));
    fflush(stdout);
  }

  data->show_new_addon_button = show_new_addon;
  data->unassigned_count = show_new_addon ?
                               get_unassigned_categories_count_for_region(wm, region, space_type, current_mode_flag) :
                               0;

  if (g_tag_bar_debug_enabled) {
    printf("[TAG BAR UPDATE] New Add-on button result: show_new_addon_button=%d unassigned_count=%d\n",
           (data->show_new_addon_button ? 1 : 0), data->unassigned_count);
    fflush(stdout);
  }

  if (show_new_addon) {
    if (g_tag_bar_debug_enabled) {
      printf("[NEW ADDON BUTTON] CREATING BUTTON: count=%d\n", data->unassigned_count);
      fflush(stdout);
    }

    TagButton &btn = data->new_addon_button;
    STRNCPY(btn.tag_name, "New Add-on!");
    STRNCPY(btn.glyph, get_new_addon_tag_glyph());
    get_new_addon_tag_color(btn.color);
    btn.is_visible = true;
    btn.is_hovered = false;
    btn.is_active = area ? is_new_addon_filter_active(area) : false;
    btn.category_count = data->unassigned_count;
  }
}

/**
 * Update tag bar buttons based on tags from window manager.
 * Tags are stored in the order they appear in wm->category_tags ListBase,
 * which matches the order from JSON's "tag_order" array.
 */
void tag_bar_buttons_update(const bContext *C,
                            const wmWindowManager *wm,
                            const TagFilterStateRef *state,
                            TagBarRuntimeData *data)
{
  if (!data) {
    return;
  }

  data->buttons.clear();
  data->total_width = 0;

  /* Get active filter tags from View3D */
  char active_tags[256] = "";
  if (state && state->active_tags) {
    STRNCPY(active_tags, state->active_tags);
  }

  /* Get current mode bit for filtering. */
  const uint32_t current_mode_flag = get_current_tag_mode_flag(C);

  /* DEBUG: Log current mode flag */
  if (g_tag_bar_debug_enabled) {
    printf("[TAG_BAR] current_mode_flag=%u\n", current_mode_flag);
  }

  /* Iterate through all tags from wm in their original order (from JSON tag_order) */
  if (wm && category_tag_list_is_valid(&wm->category_tags)) {
    if (g_tag_bar_debug_enabled) {
      printf("[TAG_BAR] Iterating through %d tags\n", BLI_listbase_count(&wm->category_tags));
    }
    for (const CategoryTagDef *tag_def =
             static_cast<const CategoryTagDef *>(wm->category_tags.first);
         tag_def;
         tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
    {
      /* Append button - order is preserved from wm->category_tags (JSON tag_order). */
      const TagButton btn = tag_button_from_def(wm, tag_def, active_tags, current_mode_flag);
      data->buttons.append(btn);
      if (g_tag_bar_debug_enabled) {
        printf("[TAG_BAR] Added button for tag='%s' is_visible=%d has_glyph=%d has_icon=%d\n",
               btn.tag_name, btn.is_visible, (btn.glyph[0] != '\0'), btn.use_builtin_icon);
      }
    }
  }

  /* NOTE: No sorting here! The order from wm->category_tags matches JSON's "tag_order".
   * Python code uses _tag_order_cache which is loaded from JSON, and the ListBase
   * is populated in that same order during JSON loading. */

  if (g_tag_bar_debug_enabled) {
    printf("[TAG_BAR] Total buttons added: %llu (visible: %zu)\n",
           (unsigned long long)data->buttons.size(),
           std::count_if(data->buttons.begin(), data->buttons.end(),
                        [](const TagButton& b) { return b.is_visible; }));

    /* DEBUG: List all buttons */
    printf("[TAG_BAR] All buttons after creation:\n");
    for (const TagButton &btn : data->buttons) {
      printf("[TAG_BAR]   - '%s': is_visible=%d icon_key='%s' icon_id=%d\n",
             btn.tag_name, int(btn.is_visible), btn.icon_key, btn.icon_id);
    }
  }

  /* Check if "New Add-on!" virtual button should be shown and populate it. */
  tag_bar_update_new_addon_button(C, wm, data, current_mode_flag);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Navigation Helper Functions
 * \{ */

/**
 * Count the number of active tags in the tag filter string.
 */
static int count_active_tags(const char *active_tags)
{
  if (!active_tags || active_tags[0] == '\0') {
    return 0;
  }

  Vector<std::string> tags;
  category_tab_split_tags(active_tags, tags, ",;");

  return int(tags.size());
}

/**
 * Find the index of the currently active single tag among visible buttons.
 * Returns -1 if no tag is active or multiple tags are active.
 */
static int find_active_single_tag_index(const TagBarRuntimeData *data)
{
  if (!data || data->buttons.is_empty()) {
    return -1;
  }

  /* Find the single active tag button among visible ones */
  int active_index = -1;
  int visible_index = 0;
  for (int i = 0; i < data->buttons.size(); i++) {
    if (!data->buttons[i].is_visible) {
      continue;  /* Skip invisible buttons */
    }
    if (data->buttons[i].is_active) {
      if (active_index != -1) {
        /* Multiple tags active */
        return -1;
      }
      active_index = visible_index;
    }
    visible_index++;
  }

  return active_index;
}

/**
 * Activate a specific tag by visible index, deactivating all others.
 * tag_index is the index among visible buttons only.
 * Returns true if successful.
 */
static bool activate_tag_by_index(bContext *C, int tag_index)
{
  if (!C || tag_index < 0) {
    return false;
  }

  ScrArea *area = CTX_wm_area(C);
  TagFilterStateRef state{};
  if (!tag_filter_state_from_area(area, &state) || !state.active_tags || !state.filter_enabled) {
    return false;
  }

  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data) {
    return false;
  }

  /* Find the tag at the given visible index */
  int visible_index = 0;
  const TagButton *target_btn = nullptr;
  for (int i = 0; i < data->buttons.size(); i++) {
    if (!data->buttons[i].is_visible) {
      continue;  /* Skip invisible buttons */
    }
    if (visible_index == tag_index) {
      target_btn = &data->buttons[i];
      break;
    }
    visible_index++;
  }

  if (!target_btn) {
    return false;  /* Invalid visible index */
  }

  /* Set the new active tag */
  BLI_strncpy(state.active_tags, target_btn->tag_name, 256);

  /* Enable tag filter */
  *state.filter_enabled = 1;

  /* Update button states immediately for next event handling */
  wmWindowManager *wm = CTX_wm_manager(C);
  tag_bar_buttons_update(C, wm, &state, data);

  /* Save per-mode tag filter state for current mode. */
  {
    const uint32_t current_mode = get_current_tag_mode_flag(C);
    tag_filter_per_mode_save(area, current_mode, state.active_tags, *state.filter_enabled);
    tag_filter_set_last_mode(area, current_mode);
  }

  /* Trigger redraw */
  WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_CATEGORY_GLYPHS, nullptr);
  ED_area_tag_redraw(area);

  return true;
}

/**
 * Cycle through visible tags: move to next or previous tag.
 * direction: 1 for next, -1 for previous
 */
static bool cycle_active_tag(bContext *C, int direction)
{
  if (!C) {
    return false;
  }

  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data || data->buttons.is_empty()) {
    return false;
  }

  ScrArea *area = CTX_wm_area(C);
  TagFilterStateRef state{};
  if (!tag_filter_state_from_area(area, &state) || !state.active_tags) {
    return false;
  }

  /* Count visible buttons */
  int visible_count = 0;
  for (const TagButton &btn : data->buttons) {
    if (btn.is_visible) {
      visible_count++;
    }
  }

  if (visible_count == 0) {
    return false;  /* No visible tags */
  }

  /* Only cycle if exactly one tag is active */
  const int active_count = count_active_tags(state.active_tags);
  if (active_count != 1) {
    return false;
  }

  /* Find current active tag visible index */
  int current_index = find_active_single_tag_index(data);
  if (current_index == -1) {
    return false;
  }

  /* Calculate next visible index without wrap-around.
   * Stop at boundaries (first/last visible tag), matching non-cyclic category-tab behavior. */
  const int new_index = current_index + direction;
  if (new_index < 0 || new_index >= visible_count) {
    return true; /* Edge reached: consume the cycling event, keep current tag unchanged. */
  }

  return activate_tag_by_index(C, new_index);
}

/**
 * Check if mouse position is within the tag bar region and tags are available.
 * Returns true if the mouse is in a region where tag cycling should work.
 */
static bool is_mouse_over_tag_bar(const bContext *C, const wmEvent *event)
{
  if (!event || !C) {
    return false;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return false;
  }

  /* Check if we're in a TAG_BAR region or HEADER region (where tags are drawn) */
  if (region->regiontype != RGN_TYPE_TAG_BAR && region->regiontype != RGN_TYPE_HEADER) {
    return false;
  }

  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data || data->buttons.is_empty()) {
    return false;
  }

  /* Check if mouse is within the region bounds */
  const int mx = event->xy[0];
  const int my = event->xy[1];

  /* Convert mouse coordinates to region-relative coordinates */
  const int region_x = mx - region->winrct.xmin;
  const int region_y = my - region->winrct.ymin;

  /* For top-aligned regions (TAG_BAR, HEADER), check if y is within button height */
  if (region_y < 0 || region_y > UI_UNIT_Y) {
    return false;
  }

  /* Check if x is within the region */
  if (region_x < 0 || region_x > region->winx) {
    return false;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Bar Event Handler
 * \{ */

/**
 * Event handler for tag bar mouse wheel navigation.
 * Handles Ctrl + Mouse Wheel to cycle through tags.
 */
int tag_bar_region_handler(bContext *C, const wmEvent *event, void * /*userdata*/)
{
  if (!event || !C) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Only handle mouse wheel events */
  if (event->type != blender::WHEELUPMOUSE && event->type != blender::WHEELDOWNMOUSE) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Check if Ctrl modifier is pressed */
  if ((event->modifier & blender::KM_CTRL) == 0) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Check if mouse is over tag bar area */
  if (!is_mouse_over_tag_bar(C, event)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Determine scroll direction */
  const int direction = (event->type == blender::WHEELUPMOUSE) ? -1 : 1;

  /* Cycle through tags */
  if (cycle_active_tag(C, direction)) {
    return WM_UI_HANDLER_BREAK;
  }

  return WM_UI_HANDLER_CONTINUE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Callbacks
 * \{ */

/**
 * Toggle a tag in the active tag filter.
 * This function directly modifies the tag filter state.
 *
 * \param C: The context
 * \param tag_name: The name of the tag to toggle
 */
static void tag_toggle_impl(bContext &C, const char *tag_name)
{
  ScrArea *area = CTX_wm_area(&C);
  if (!area) {
    return;
  }

  TagFilterStateRef state{};
  if (!tag_filter_state_from_area(area, &state) || !state.active_tags || !state.filter_enabled) {
    return;
  }

  /* Deactivate "New Add-on!" filter when clicking on a normal tag.
   * This allows user to switch away from "New Add-ons!" filter to other tags. */
  if (is_new_addon_filter_active(area)) {
    set_saved_tag_filter_tags(area, "");
    set_new_addon_filter_active(area, false);
  }

  /* Split current active tags to work with. */
  Vector<std::string> tags;
  category_tab_split_tags(state.active_tags, tags, ",;");

  /* Check if tag is already in the list */
  bool tag_found = false;
  for (const std::string &tag : tags) {
    if (STREQ(tag.c_str(), tag_name)) {
      tag_found = true;
      break;
    }
  }

  /* Toggle: add if not found, remove if found */
  char new_tags[256] = "";
  const bool was_removing_tag = tag_found;

  if (!tag_found) {
    /* Add the tag - multi-select with max 3 tags.
     * If limit reached, do nothing (ignore the click). */
    if (int(tags.size()) >= 3) {
      return;
    }
    /* Append to existing tags */
    BLI_strncpy(new_tags, state.active_tags, sizeof(new_tags));
    if (new_tags[0] != '\0') {
      BLI_strncat(new_tags, ",", sizeof(new_tags));
    }
    BLI_strncat(new_tags, tag_name, sizeof(new_tags));
  }
  else {
    /* Remove the tag from the list, rebuilding the remaining tags. */
    bool first = true;
    for (const std::string &tag : tags) {
      if (STREQ(tag.c_str(), tag_name)) {
        continue;
      }
      if (!first) {
        BLI_strncat(new_tags, ",", sizeof(new_tags));
      }
      BLI_strncat(new_tags, tag.c_str(), sizeof(new_tags));
      first = false;
    }
  }

  /* Update the active tags string */
  BLI_strncpy(state.active_tags, new_tags, 256);

  /* Handle filter state based on whether tags were removed or added. */
  if (was_removing_tag) {
    /* Tag was removed - disable filter if all tags removed */
    if (new_tags[0] == '\0') {
      *state.filter_enabled = 0;
    }
  }
  else {
    /* Tag was added - enable filter */
    *state.filter_enabled = 1;
  }

  /* Ensure first visible category is active */
  ARegion *region_ui = BKE_area_find_region_type(area, RGN_TYPE_UI);
  if (region_ui) {
    panel_category_tabs_ensure_active_visible(&C, region_ui);
  }

  /* Save per-mode tag filter state for current mode. */
  {
    const uint32_t current_mode = get_current_tag_mode_flag(&C);
    tag_filter_per_mode_save(area, current_mode, state.active_tags, *state.filter_enabled);
    tag_filter_set_last_mode(area, current_mode);
  }

  /* Trigger redraw */
  WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_main_add_notifier(NC_SPACE | ND_CATEGORY_GLYPHS, nullptr);
  ED_area_tag_redraw(area);
}

/* Callback for tag button click - deprecated, kept for compatibility */
void tag_button_click_by_mode(bContext *C, void *arg1, void *arg2)
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(arg1);
  const int mode_flags = POINTER_AS_INT(arg2);
  (void)mode_flags; /* Unused - deprecated function */
  (void)wm;

  TagFilterStateRef state{};
  const bool has_state = tag_filter_state_from_context(C, &state);

  /* Update cache and redraw */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (data) {
    tag_bar_buttons_update(C, wm, has_state ? &state : nullptr, data);
    data->needs_update = false;  /* Just updated, no need to update again */
  }

  WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  ED_region_tag_redraw(CTX_wm_region(C));
}

void buttons_tag_bar_region_init(wmWindowManager *wm, ARegion *region)
{
  /* Initialize region for UI interaction */
  ED_region_header_init(region);

  /* Add UI handlers for button interaction (mouse click, hover, etc) */
  region_handlers_add(&region->runtime->handlers);

  UNUSED_VARS(wm);
}

void buttons_tag_bar_region_exit(wmWindowManager * /*wm*/, ARegion * /*region*/)
{
  /* Cache entries are keyed by window manager and reclaimed lazily in
   * get_tag_bar_data_global() when the active window manager changes, so no
   * per-region cleanup is needed here. */
}

/* -------------------------------------------------------------------- */
/** \name New Add-on! Button Drawing Helper
 * \{ */

/**
 * Draw the "New Add-on!" virtual button at the given x position.
 * Returns the width of the drawn button (0 if not drawn).
 */
static int draw_new_addon_button(const bContext * /*C*/,
                                 Block *block,
                                 TagBarRuntimeData *data,
                                 const ScrArea *area,
                                 int xco,
                                 int yco,
                                 int btn_height,
                                 float font_size_factor)
{
  if (!data || !data->show_new_addon_button) {
    if (g_tag_bar_debug_enabled) {
      printf("[NEW ADDON DRAW] SKIP: data=%p show_new_addon_button=%d\n",
             (void*)data, (data ? int(data->show_new_addon_button) : -1));
      fflush(stdout);
    }
    return 0;
  }

  const uiStyle *style = style_get_dpi();
  const float dpi_fac = UI_SCALE_FAC;
  const int fontid = style->widget.uifont_id;
  BLF_size(fontid, UI_UNIT_Y * font_size_factor * dpi_fac);

  TagButton &btn = data->new_addon_button;

  /* Convert glyph from hex to UTF-8 for display */
  char glyph_utf8[8] = "";
  const char *display_glyph = "";
  if (btn.glyph[0] != '\0') {
    if (tag_glyph_hex_to_utf8(btn.glyph, glyph_utf8)) {
      display_glyph = glyph_utf8;
    }
    else {
      display_glyph = btn.glyph;
    }
  }

  /* Build label: glyph + "New Add-on!" + count */
  char count_str[16] = "";
  if (data->unassigned_count > 0) {
    SNPRINTF(count_str, " (%d)", data->unassigned_count);
  }

  char button_label[96];
  if (display_glyph[0]) {
    SNPRINTF(button_label, "%s New Add-on!%s", display_glyph, count_str);
  }
  else {
    SNPRINTF(button_label, "New Add-on!%s", count_str);
  }

  const int text_width = BLF_width(fontid, button_label, strlen(button_label));
  const int btn_width = text_width + UI_UNIT_X;

  if (g_tag_bar_debug_enabled) {
    printf("[NEW ADDON DRAW] DRAW: area=%p label='%s' count=%d x=%d y=%d width=%d height=%d\n",
           (const void*)area, button_label, data->unassigned_count, xco, yco, btn_width, btn_height);
    fflush(stdout);
  }

  /* Update is_active from per-space state BEFORE creating button */
  btn.is_active = area ? is_new_addon_filter_active(area) : false;

  Button *but = uiDefBut(block,
                         ButtonType::ButToggle,
                         button_label,
                         xco,
                         yco,
                         btn_width,
                         btn_height,
                         nullptr,
                         0.0f,
                         0.0f,
                         TIP_("Show only categories from newly installed add-ons"));

  if (but) {
    /* Green color for the button */
    float color[3];
    get_new_addon_tag_color(color);

    if (btn.is_active) {
      /* Active: fill background with green and show as selected */
      rgb_float_to_uchar(but->col, color);
      but->col[3] = 255;
      but->flag |= UI_SELECT;  /* Show button as pressed/selected */
    }
    else {
      /* Inactive: color the text green */
      rgb_float_to_uchar(but->col, color);
      but->col[3] = 255;
      but->drawflag |= BUT_TEXT_USE_COL;
    }

    /* Store button rect for hit testing */
    btn.rect.xmin = but->rect.xmin;
    btn.rect.xmax = but->rect.xmax;
    btn.rect.ymin = but->rect.ymin;
    btn.rect.ymax = but->rect.ymax;

    /* "New Add-on!" tag click handler.
     * When activating: save current tags to saved_tag_filter_tags, clear active tags.
     * When deactivating: restore tags from saved_tag_filter_tags.
     * Special case: if no tags are selected when activating, show all categories
     * and disable the tag filter (filter_enabled = 0).
     * 
     * Alt+Click: Mark all unassigned categories as "Without Tag" (clear pending). */
    but->func = [](bContext *C_cb, void * /*arg1*/, void * /*arg2*/) {
      ScrArea *cb_area = CTX_wm_area(C_cb);
      if (!cb_area) {
        return;
      }

      /* Check if Alt modifier is pressed - if so, mark all unassigned as "Without Tag" */
      const wmWindow *win = CTX_wm_window(C_cb);
      const wmEvent *event = win ? win->runtime->eventstate : nullptr;
      const bool alt_pressed = event && (event->modifier & blender::KM_ALT);

      if (alt_pressed) {
        /* Alt+Click: Mark all unassigned categories as "Without Tag" (via Python bridge). */
        const bool ok = category_py_mark_all_unassigned_without_tag(
            const_cast<bContext *>(C_cb), cb_area->spacetype, get_current_tag_mode_flag(C_cb));
        /* Only refresh the UI when the Python action actually succeeded;
         * the bridge already prints the traceback on failure. */
        if (ok) {
          WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
          ED_area_tag_redraw(cb_area);
        }
        return;
      }

      const bool currently_active = is_new_addon_filter_active(cb_area);

      /* Get current tag filter state */
      TagFilterStateRef state{};
      if (!tag_filter_state_from_area(cb_area, &state) || !state.active_tags) {
        /* Fallback: just toggle the flag if we can't get tag state */
        set_new_addon_filter_active(cb_area, !currently_active);
        WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
        ED_area_tag_redraw(cb_area);
        return;
      }

      if (!currently_active) {
        /* Activating: save current tags, clear active tags to show only pending categories */
        const bool has_active_tags = (state.active_tags[0] != '\0');
        set_saved_tag_filter_tags(cb_area, state.active_tags);
        state.active_tags[0] = '\0';
        set_new_addon_filter_active(cb_area, true, /*auto_activated=*/false);

        /* If no tags were selected, disable the tag filter to show all categories.
         * This allows user to see all categories when clicking "New Add-on!" without
         * any tag filter active. */
        if (!has_active_tags && state.filter_enabled) {
          *state.filter_enabled = 0;
        }
      }
      else {
        /* Deactivating: restore saved tags.
         * IMPORTANT: Clear auto_activated flag so filter won't be re-activated automatically.
         * User manually deactivated the filter, so they want to see other categories. */
        char *saved_tags = get_saved_tag_filter_tags(cb_area);
        if (saved_tags && saved_tags[0] != '\0') {
          BLI_strncpy(state.active_tags, saved_tags, 256);
        }
        else {
          state.active_tags[0] = '\0';
        }
        set_new_addon_filter_active(cb_area, false, /*auto_activated=*/false);
      }

      WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
      ED_area_tag_redraw(cb_area);
    };
    but->func_arg1 = nullptr;
    but->func_arg2 = nullptr;
  }

  return btn_width;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public New Add-on Button Draw Function
 * \{ */

int tag_bar_draw_new_addon_button(const bContext *C,
                                  Block *block,
                                  const ScrArea *area,
                                  int xco,
                                  int yco,
                                  int btn_height,
                                  float font_size_factor)
{
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data) {
    return 0;
  }
  return draw_new_addon_button(C, block, data, area, xco, yco, btn_height, font_size_factor);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Icon Drawing Helper
 * \{ */

/* Darken RGB color by factor (0.0 = no change, 1.0 = black).
 * Shared with the category tabs: defined in interface_tab_categories.cc. Forward-declared here
 * (rather than including the category-core intern header) to avoid coupling the tag bar to the
 * whole category lookup surface for a single color helper. */
void darken_color_3ub(uchar color[3], const float factor);

/**
 * Draw a Blender icon for a tag button with proper tinting.
 * Monochrome icons get tag color tint, colored icons keep original.
 */
static void draw_tag_icon_with_tint(const TagButton &btn,
                                     float x, float y,
                                     float icon_size,
                                     float darken_factor)
{
  int icon_id_to_draw = btn.icon_id;

  /* If icon_id is NONE but we have icon_key, try to resolve it now.
   * Special case for FUND is handled manually if resolution fails. */
  if (icon_id_to_draw == ICON_NONE && btn.icon_key[0] != '\0') {
    icon_id_to_draw = category_tab_icon_id_resolve_from_key_path(btn.icon_key, nullptr);
  }

  if (icon_id_to_draw == ICON_NONE) {
    if (strcmp(btn.icon_key, "FUND") == 0) {
      /* Hardcoded fallback for Development Fund icon if not in RNA.
       * We try to use the Blender logo icon if FUND is not resolvable. */
      RNA_enum_value_from_identifier(rna_enum_icon_items, "BLENDER", &icon_id_to_draw);
    }
  }

  if (icon_id_to_draw == ICON_NONE) {
    return;
  }

  uchar icon_tint[4] = {255, 255, 255, 255};

  /* Check if icon is monochrome (can be tinted). */
  const bool is_monochrome = icon_is_monochrome(icon_id_to_draw);

  if (is_monochrome && !is_zero_v3(btn.color)) {
    /* Monochrome icon - apply tag color tint. */
    icon_tint[0] = uchar(btn.color[0] * 255.0f);
    icon_tint[1] = uchar(btn.color[1] * 255.0f);
    icon_tint[2] = uchar(btn.color[2] * 255.0f);
  }
  else if (is_monochrome) {
    /* Use theme default for monochrome if no tag color. */
    uchar theme_icon_color[4];
    if (icon_get_theme_color(icon_id_to_draw, theme_icon_color)) {
      copy_v4_v4_uchar(icon_tint, theme_icon_color);
    }
  }
  /* else: Colored icon - keep white tint (original colors). */

  if (darken_factor > 0.0f) {
    darken_color_3ub(icon_tint, darken_factor);
  }

  GPU_blend(GPU_BLEND_ALPHA);
  icon_draw_ex(x, y, icon_id_to_draw, icon_size / ICON_DEFAULT_WIDTH, 1.0f, 0.0f,
               icon_tint, false, UI_NO_ICON_OVERLAY_TEXT);
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Draw Functions
 * \{ */

/**
 * Per-context predicate selecting whether a tag button renders as an icon (vs. a glyph/text).
 * The three tag-bar draw entry points historically used three different rules; they are kept
 * exact here so the merge into #tag_bar_draw_single_button does not change which buttons show
 * an icon.
 */
enum class TagBarIconMode {
  /** Region overlay: icon if a resolved icon_id exists, or for the special "FUND" key. */
  RegionOverlay,
  /** View3D header inline: icon if marked builtin, or any icon_key is present. */
  Header,
  /** TAG_BAR layout region: icon only if marked builtin AND a resolved icon_id exists. */
  Layout,
};

/**
 * Shared styling for a single tag button, capturing the (many) layout differences between the
 * three tag-bar draw contexts so the per-button body can live in one place.
 */
struct TagBarDrawStyle {
  TagBarIconMode icon_mode;
  float font_scale;        /* BLF size = UI_UNIT_Y * font_scale * dpi_fac. */
  float icon_width_factor; /* Reserved width for an icon = UI_UNIT_Y * icon_width_factor. */
  float icon_size_factor;  /* Drawn icon size = UI_UNIT_Y * icon_size_factor * dpi_fac. */
  int left_padding;
  int icon_text_spacing;
  int right_padding;
  short btn_height;
  int xco_gap;                    /* Extra horizontal gap added after each button. */
  bool icon_y_centered;           /* true: center icon in UI_UNIT_Y; false: btn_height * 0.15. */
  bool icon_draw_requires_icon_id; /* true: only draw the icon when icon_id != ICON_NONE. */
};

static bool tag_button_use_icon(const TagButton &btn, const TagBarIconMode mode)
{
  switch (mode) {
    case TagBarIconMode::RegionOverlay:
      return (btn.icon_id != ICON_NONE) || (strcmp(btn.icon_key, "FUND") == 0);
    case TagBarIconMode::Header:
      return btn.use_builtin_icon || (btn.icon_key[0] != '\0');
    case TagBarIconMode::Layout:
      return btn.use_builtin_icon && (btn.icon_id != ICON_NONE);
  }
  return false;
}

/**
 * Draw a single tag toggle button at (xco, yco) and return its computed width.
 *
 * This is the single source of truth for tag-button geometry, label construction, icon drawing,
 * color application and the click callback; the three draw entry points only differ in the
 * #TagBarDrawStyle they pass and in their surrounding block/scroll setup.
 *
 * NOTE: color is applied uniformly via #rgb_float_to_uchar with full alpha. The region overlay
 * previously assigned the float color directly into the uchar `but->col` (leaving alpha 0), which
 * left active overlay buttons effectively un-colored; the unified path fixes that.
 */
static int tag_bar_draw_single_button(Block *block,
                                      TagButton &btn,
                                      const TagBarDrawStyle &cfg,
                                      const uiStyle *style,
                                      const float dpi_fac,
                                      const int xco,
                                      const int yco)
{
  const int fontid = style->widget.uifont_id;
  BLF_size(fontid, UI_UNIT_Y * cfg.font_scale * dpi_fac);

  const bool use_icon = tag_button_use_icon(btn, cfg.icon_mode);

  /* Convert glyph from hex to UTF-8 for display (only if not using icon). */
  char glyph_utf8[8] = "";
  const char *display_glyph = "";
  if (!use_icon && btn.glyph[0] != '\0') {
    if (tag_glyph_hex_to_utf8(btn.glyph, glyph_utf8)) {
      display_glyph = glyph_utf8;
    }
    else {
      display_glyph = btn.glyph;
    }
  }

  const int text_width = BLF_width(fontid, btn.tag_name, strlen(btn.tag_name));
  const int icon_glyph_width = use_icon ?
                                   int(UI_UNIT_Y * cfg.icon_width_factor) :
                               (display_glyph[0]) ?
                                   BLF_width(fontid, display_glyph, strlen(display_glyph)) :
                                   0;

  const int btn_width = cfg.left_padding + icon_glyph_width + cfg.icon_text_spacing + text_width +
                        cfg.right_padding;

  /* Create button label - use proper spacing for icon, glyph text otherwise. */
  char button_label[72];
  if (use_icon) {
    /* For icon mode, add enough spaces for proper icon placement and spacing. */
    SNPRINTF(button_label, "        %s", btn.tag_name);
  }
  else if (display_glyph[0]) {
    SNPRINTF(button_label, "%s %s", display_glyph, btn.tag_name);
  }
  else {
    STRNCPY(button_label, btn.tag_name);
  }

  Button *but = uiDefBut(block,
                         ButtonType::ButToggle,
                         button_label,
                         xco,
                         yco,
                         btn_width,
                         cfg.btn_height,
                         nullptr,
                         0.0f,
                         0.0f,
                         TIP_("Toggle category filter"));

  /* Draw icon manually if using icon mode. */
  const bool draw_icon = use_icon &&
                         (!cfg.icon_draw_requires_icon_id || btn.icon_id != ICON_NONE);
  if (draw_icon) {
    const float icon_size = UI_UNIT_Y * cfg.icon_size_factor * dpi_fac;
    const float icon_x = float(xco + cfg.left_padding);
    const float icon_y = cfg.icon_y_centered ? (UI_UNIT_Y * dpi_fac - icon_size) / 2.0f :
                                               float(cfg.btn_height) * 0.15f;
    draw_tag_icon_with_tint(btn, icon_x, icon_y, icon_size, 0.0f);
  }

  if (but) {
    /* Unified color application (see function NOTE). */
    if (btn.is_active) {
      /* Active button: background uses tag color. */
      rgb_float_to_uchar(but->col, btn.color);
      but->col[3] = 255;
    }
    else if (!use_icon && btn.glyph[0]) {
      /* Inactive button with glyph: color the text (glyph + label).
       * BUT_TEXT_USE_COL is required for but->col to affect text drawing. */
      rgb_float_to_uchar(but->col, btn.color);
      but->col[3] = 255;
      but->drawflag |= BUT_TEXT_USE_COL;
    }

    /* Capture tag_name by value (copy) for the lambda. */
    std::string captured_tag_name(btn.tag_name);
    but->apply_func = [captured_tag_name](bContext &C) {
      tag_toggle_impl(C, captured_tag_name.c_str());
    };

    /* Store button rectangle for mouse hit testing. */
    btn.rect.xmin = but->rect.xmin;
    btn.rect.xmax = but->rect.xmax;
    btn.rect.ymin = but->rect.ymin;
    btn.rect.ymax = but->rect.ymax;
  }

  return btn_width;
}

void buttons_tag_bar_region_draw(const bContext *C, ARegion *region)
{
  ScrArea *area = CTX_wm_area(C);
  if (g_tag_bar_debug_enabled) {
    printf("[TAG BAR REGION DRAW] enter: area=%p region=%p space_type=%d\n",
           (void*)area, (void*)region, (area ? area->spacetype : -1));
    fflush(stdout);
  }

  if (!area) {
    return;
  }

  if (ELEM(area->spacetype, SPACE_NODE, SPACE_IMAGE)) {
    /* Node/Image TAG_BAR is drawn from Python Header classes for full UI parity with View3D. */
    ED_region_header(C, region);
    return;
  }

  TagFilterStateRef state{};
  if (!tag_filter_state_from_area(area, &state)) {
    return;
  }

  /* Get or create data */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data) {
    return;
  }

  const bool has_visible_buttons = std::any_of(
      data->buttons.begin(), data->buttons.end(), [](const TagButton &btn) { return btn.is_visible; });

  if (g_tag_bar_debug_enabled) {
    printf("[TAG BAR DEBUG] show_new_addon_button=%d unassigned_count=%d\n",
           int(data->show_new_addon_button), data->unassigned_count);
    fflush(stdout);
  }

  if (!has_visible_buttons) {
    ED_region_header(C, region);
    return;
  }

  const uiStyle *style = style_get_dpi();
  const float dpi_fac = UI_SCALE_FAC;

  /* No background drawing - overlay region is transparent */

  /* Create UI block for buttons */
  Block *block = block_begin(C, region, __func__, EmbossType::Emboss);
  Layout &layout = block_layout(block,
                                LayoutDirection::Horizontal,
                                LayoutType::Header,
                                0, 0,
                                region->winx, region->winy,
                                0,
                                style);

  /* Calculate scroll offset. A negative stored value is used by the scroll
   * widget to request a recompute of max_scroll; treat the sign as an explicit
   * flag here and work with the absolute offset afterwards. */
  const int stored_scroll = state.scroll_offset ? *state.scroll_offset : 0;
  const bool recompute_max_scroll = (stored_scroll < 0);
  const int scroll_offset = recompute_max_scroll ? -stored_scroll : stored_scroll;
  if (recompute_max_scroll) {
    data->max_scroll = std::max(0, data->total_width - region->winx + 20);
  }
  UNUSED_VARS(scroll_offset);

  /* Draw buttons via UI API */
  const TagBarDrawStyle cfg = {
      TagBarIconMode::RegionOverlay,
      0.7f,             /* font_scale */
      1.4f,             /* icon_width_factor */
      1.4f,             /* icon_size_factor */
      UI_UNIT_X / 2,    /* left_padding */
      UI_UNIT_X / 3,    /* icon_text_spacing */
      UI_UNIT_X,        /* right_padding */
      short(UI_UNIT_Y), /* btn_height */
      UI_UNIT_X / 4,    /* xco_gap */
      true,             /* icon_y_centered */
      false,            /* icon_draw_requires_icon_id */
  };

  int xco = UI_UNIT_X / 2;
  for (TagButton &btn : data->buttons) {
    if (!btn.is_visible) {
      continue;
    }
    const int btn_width = tag_bar_draw_single_button(block, btn, cfg, style, dpi_fac, xco, 0);
    xco += btn_width + cfg.xco_gap;
  }

  /* Draw "New Add-on!" virtual button at the end */
  const int new_addon_width = draw_new_addon_button(
      C, block, data, area, xco, 0, UI_UNIT_Y, 0.7f);
  xco += new_addon_width > 0 ? new_addon_width + UI_UNIT_X / 4 : 0;

  data->total_width = xco;

  /* Scroll button (if needed) */
  if (data->total_width > region->winx) {
    [[maybe_unused]] Layout &row = layout.row(false);
    uiDefBut(block, ButtonType::Scroll, "",
             0, 0, UI_UNIT_X, UI_UNIT_Y,
             state.scroll_offset, 0.0f, float(data->max_scroll),
             "");
  }

  block_end(C, block);
  block_draw(C, block);
}

/**
 * Draw tag bar inside an existing region (e.g., header region).
 * This is called from view3d_header_region_draw to display tags inline.
 */
void buttons_tag_bar_draw_in_header(const bContext *C, ARegion *region)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return;
  }

  /* Only draw in View3D */
  if (area->spacetype != SPACE_VIEW3D) {
    return;
  }

  /* Get or create data from global cache */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data || data->buttons.is_empty()) {
    return;
  }

  /* Draw a simple horizontal bar with tag buttons */
  const uiStyle *style = style_get_dpi();
  const float dpi_fac = UI_SCALE_FAC;

  /* Create UI block for tag buttons - use default positioning */
  Block *block = block_begin(C, region, __func__, EmbossType::Emboss);
  Layout &layout = block_layout(block,
                                LayoutDirection::Horizontal,
                                LayoutType::Header,
                                0, 0,
                                region->winx, region->winy,
                                0,
                                style);

  /* Create row for buttons with alignment */
  [[maybe_unused]] Layout &row = layout.row(true);  /* true = align items */

  /* Draw tag buttons */
  const TagBarDrawStyle cfg = {
      TagBarIconMode::Header,
      0.5f,                 /* font_scale */
      1.0f,                 /* icon_width_factor */
      1.0f,                 /* icon_size_factor */
      10,                   /* left_padding */
      15,                   /* icon_text_spacing */
      5,                    /* right_padding */
      short(UI_UNIT_Y - 4), /* btn_height */
      4,                    /* xco_gap */
      false,                /* icon_y_centered */
      true,                 /* icon_draw_requires_icon_id */
  };

  int xco = 0;
  for (TagButton &btn : data->buttons) {
    if (!btn.is_visible) {
      continue;
    }
    const int btn_width = tag_bar_draw_single_button(block, btn, cfg, style, dpi_fac, xco, 0);
    xco += btn_width + cfg.xco_gap;
  }

  /* Draw "New Add-on!" virtual button at the end */
  draw_new_addon_button(C, block, data, area, xco, 0, UI_UNIT_Y - 4, 0.5f);

  block_end(C, block);
  block_draw(C, block);
}

/**
 * Draw tag buttons into a given UI block.
 * This is designed to be called from region draw callbacks.
 * \param C: Context
 * \param block: UI block to add buttons to
 * \param region: Region for size calculations
 * \param start_x: X position to start drawing from (after external buttons)
 */
void tag_bar_draw_in_layout(const bContext *C, Block *block, ARegion *region, int start_x)
{
  (void)region; /* Reserved for future use */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);

  if (!data) {
    return;
  }

  const uiStyle *style = style_get_dpi();
  const float dpi_fac = UI_SCALE_FAC;

  /* Start from the given position (after external buttons) with some padding.
   * For TAG_BAR region, use y=0 (top-aligned region) with proper height. */
  const TagBarDrawStyle cfg = {
      TagBarIconMode::Layout,
      0.7f,                 /* font_scale */
      1.4f,                 /* icon_width_factor */
      1.4f,                 /* icon_size_factor */
      UI_UNIT_X / 2,        /* left_padding */
      UI_UNIT_X / 3,        /* icon_text_spacing */
      UI_UNIT_X / 8,        /* right_padding */
      short(UI_UNIT_Y - 4), /* btn_height (match header button height) */
      UI_UNIT_X / 4,        /* xco_gap */
      false,                /* icon_y_centered */
      true,                 /* icon_draw_requires_icon_id */
  };

  int xco = start_x + UI_UNIT_X / 2;
  const int yco = 0;  /* TAG_BAR is top-aligned, y=0 is the top edge */

  /* Draw tag buttons (if any exist) */
  for (TagButton &btn : data->buttons) {
    if (!btn.is_visible) {
      continue;
    }
    const int btn_width = tag_bar_draw_single_button(block, btn, cfg, style, dpi_fac, xco, yco);
    xco += btn_width + cfg.xco_gap;
  }

  /* Draw "New Add-on!" virtual button at the end */
  {
    const ScrArea *area = C ? CTX_wm_area(C) : nullptr;
    const int new_addon_width = draw_new_addon_button(
        C, block, data, area, xco, yco, UI_UNIT_Y - 4, 0.7f);
    xco += new_addon_width > 0 ? new_addon_width + UI_UNIT_X / 4 : 0;
  }

  /* Store total width for View2D scrolling */
  data->total_width = xco;
}

void buttons_tag_bar_region_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  ARegion *region = params->region;

  if (g_tag_bar_debug_enabled) {
    printf("[TAG BAR LISTENER] category=%d data=%d action=%d region=%p\n",
           wmn->category, wmn->data, wmn->action, (void*)region);
    fflush(stdout);
  }

  switch (wmn->category) {
    case NC_WM:
      if (wmn->data == ND_CATEGORY_GLYPHS) {
        /* Mark all tag bar data as dirty - will be updated on next draw */
        tag_bar_mark_all_dirty();
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_VIEW3D) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

void buttons_tag_bar_region_message_subscribe(const wmRegionMessageSubscribeParams *params)
{
  wmMsgBus *mbus = params->message_bus;
  ARegion *region = params->region;

  wmMsgSubscribeValue msg_sub_value_region_tag_redraw{};
  msg_sub_value_region_tag_redraw.owner = region;
  msg_sub_value_region_tag_redraw.user_data = region;
  msg_sub_value_region_tag_redraw.notify = ED_region_do_msg_notify_tag_redraw;

  WM_msg_subscribe_rna_anon_prop(mbus, Window, view_layer, &msg_sub_value_region_tag_redraw);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Order Tree View
 * \{ */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Bar Filter Popover Panel
 * \{ */

/**
 * Draw callback for the tag bar filter popover panel.
 * This panel provides tag order management with UIList.
 */
static void tag_bar_filter_popover_panel_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm || !category_tag_list_is_valid(&wm->category_tags)) {
    layout.label(IFACE_("No tags available"), ICON_NONE);
    return;
  }

  /* Header with info */
  layout.label(IFACE_("Tag Order Management"), ICON_NONE);
  layout.separator();

  /* Create RNA pointer for window manager */
  PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);

  /* Calculate UIList size - count visible tags for current mode + 1 extra slot. */
  const uint32_t current_mode_flag = get_current_tag_mode_flag(C);

  /* Count visible tags (with icon/glyph + matching mode) */
  int visible_tag_count = 0;
  for (const CategoryTagDef *tag_def = static_cast<const CategoryTagDef *>(wm->category_tags.first);
       tag_def;
       tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
  {
    /* Check if tag has glyph OR icon (icon_source == 1 and icon_key not empty) */
    const bool has_glyph = (tag_def->glyph[0] != '\0');
    const bool has_icon = (tag_def->icon_source == 1 && tag_def->icon_key[0] != '\0');
    if (!has_glyph && !has_icon) {
      continue;
    }

    /* Mode visibility is centralized in is_tag_valid_for_mode (handles
     * mode_flags == 0 as all-modes, direct bit match and EDIT_MODE
     * generalization for detailed edit modes). */
    if (is_tag_valid_for_mode(wm, tag_def->name, current_mode_flag)) {
      visible_tag_count++;
    }
  }

  /* visible_rows = visible count + 1 extra slot, cap at 20 rows for readability */
  int visible_rows = visible_tag_count + 1;
  if (visible_rows > 20) {
    visible_rows = 20;
  }
  if (visible_rows < 2) {
    visible_rows = 2; /* Minimum 2 rows for usability */
  }

  /* UIList with tags */
  ui::Layout &row = layout.row(false);
  ui::Layout &list_col = row.column(true);

  template_uilist(&list_col,
                C,
                "VIEW3D_UL_tag_order_list",  /* UIList type name */
                "tag_order_list",             /* Unique list ID */
                &wm_ptr,
                "category_tags",             /* Collection property */
                 &wm_ptr,
                 "category_tags_active_index", /* Active index property */
                 nullptr,                     /* item_dyntip_propname */
                 visible_rows,                /* rows - show all tags (up to 20) */
                 20,                          /* maxrows - always 20 */
                 0,                           /* layout_type (UILST_LAYOUT_DEFAULT = 0) */
                 TEMPLATE_LIST_FLAG_NONE);    /* flags */

  /* Checkbox for showing tag names in Tag Bar (Glyph+Name vs Glyph-only mode) */
  layout.separator();
  ui::Layout &prop_row = layout.row(false);
  prop_row.prop(&wm_ptr, "show_tag_names", UI_ITEM_NONE, IFACE_("Show Tag Names"), ICON_NONE);

  /* Checkbox for showing tag names only for active tags */
  ui::Layout &prop_row2 = layout.row(false);
  prop_row2.active_set(RNA_boolean_get(&wm_ptr, "show_tag_names"));
  prop_row2.prop(&wm_ptr, "show_tag_names_active_only", UI_ITEM_NONE, IFACE_("Only Active Tags"), ICON_NONE);

  /* Move buttons column */
  ui::Layout &button_col = row.column(true);

  /* Move Up button - simple op call */
  button_col.op("view3d.tag_move_up", "", ICON_TRIA_UP, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);

  /* Move Down button - simple op call */
  button_col.op("view3d.tag_move_down", "", ICON_TRIA_DOWN, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);

  button_col.separator();

  /* New Tag button - uses centered popup wrapper */
  PointerRNA new_tag_ptr = button_col.op("wm.centered_popup_operator_wrapper",
                                          "",
                                          ICON_ADD,
                                          wm::OpCallContext::InvokeDefault,
                                          UI_ITEM_NONE);
  RNA_string_set(&new_tag_ptr, "operator_idname", "wm.category_tag_create");
  RNA_int_set(&new_tag_ptr, "width", UI_CATEGORY_TAG_CREATE_POPUP_WIDTH);

  /* Delete Tag button - deletes currently selected tag */
  button_col.op("wm.category_tag_delete",
                "",
                ICON_REMOVE,
                wm::OpCallContext::InvokeDefault,
                UI_ITEM_NONE);

  button_col.separator();

  /* Open preferences for full tag management - jump to TAGS section.
   * First, sync the tag filter mode to match the current context mode,
   * so the user sees the same tags in Preferences that they saw in the popover. */
  const int filter_mode_enum = category_tag_mode_flag_to_filter_enum(current_mode_flag);
  wm->category_tag_filter_mode = static_cast<char>(filter_mode_enum);

  PointerRNA prefs_ptr = button_col.op("screen.userpref_show",
                                        "",
                                        ICON_PREFERENCES,
                                        wm::OpCallContext::ExecDefault,
                                        UI_ITEM_NONE);
  RNA_enum_set(&prefs_ptr, "section", USER_SECTION_TAGS);

  /* "Mark All as Distributed" button - shown only when there are unassigned categories
   * (i.e. when the "New Add-on!" button is visible in the tag bar). */
  if (should_show_new_addon_tag(wm, -1, current_mode_flag)) {
    button_col.separator();
    button_col.op("wm.mark_all_unassigned_as_distributed",
                  "",
                  ICON_CHECKMARK,
                  wm::OpCallContext::ExecDefault,
                  UI_ITEM_NONE);
  }
}

#include "interface_intern.hh"

/* Maximum storage size before LRU cleanup (leave some margin from 1024). */
#define TAG_LAST_ACTIVE_CATEGORIES_MAX_SIZE 900

/**
 * Get the tag_last_active_categories storage buffer for a given area.
 * Returns nullptr if the area type doesn't support tag category memory.
 *
 * \param area: The screen area to get storage for
 * \return Pointer to the storage buffer, or nullptr if unsupported
 */
static char *tag_last_active_categories_storage_get(const ScrArea *area)
{
  if (!area || !area->spacedata.first) {
    return nullptr;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      return v3d->tabs_state.tag_last_active_categories;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      return sbuts->tabs_state.tag_last_active_categories;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      return snode->tabs_state.tag_last_active_categories;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      return sima->tabs_state.tag_last_active_categories;
    }
    default:
      return nullptr;
  }
}

/* -------------------------------------------------------------------- */
/** \name Per-Mode Tag Filter State
 * \{ */

/**
 * Get the per-mode tag filter state storage buffer for a given area.
 * Format: "flag|enabled|tags;flag|enabled|tags;..."
 * Returns nullptr if the area type doesn't support per-mode tag state.
 */
static char *tag_filter_per_mode_storage_get(const ScrArea *area)
{
  if (!area || !area->spacedata.first) {
    return nullptr;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      return v3d->tabs_state.tag_filter_state_per_mode;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      return sbuts->tabs_state.tag_filter_state_per_mode;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      return snode->tabs_state.tag_filter_state_per_mode;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      return sima->tabs_state.tag_filter_state_per_mode;
    }
    default:
      return nullptr;
  }
}

/**
 * Get the last known mode flag for per-mode tag filter save/restore.
 */
static uint32_t tag_filter_get_last_mode(const ScrArea *area)
{
  if (!area || !area->spacedata.first) {
    return 0;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      return v3d->tabs_state.tag_filter_last_mode;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      return sbuts->tabs_state.tag_filter_last_mode;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      return snode->tabs_state.tag_filter_last_mode;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      return sima->tabs_state.tag_filter_last_mode;
    }
    default:
      return 0;
  }
}

/**
 * Set the last known mode flag for per-mode tag filter save/restore.
 */
static void tag_filter_set_last_mode(ScrArea *area, uint32_t mode)
{
  if (!area || !area->spacedata.first) {
    return;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      v3d->tabs_state.tag_filter_last_mode = mode;
      break;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      sbuts->tabs_state.tag_filter_last_mode = mode;
      break;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      snode->tabs_state.tag_filter_last_mode = mode;
      break;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      sima->tabs_state.tag_filter_last_mode = mode;
      break;
    }
  }
}

/**
 * Save tag filter state for a specific mode into per-mode storage.
 * Format: "flag|enabled|tags;flag|enabled|tags;..."
 * If an entry for the given mode_flag already exists, it is updated.
 */
static void tag_filter_per_mode_save(ScrArea *area,
                                      uint32_t mode_flag,
                                      const char *tags,
                                      int enabled)
{
  if (!area || mode_flag == 0) {
    return;
  }

  char *storage = tag_filter_per_mode_storage_get(area);
  if (!storage) {
    return;
  }

  /* Build new entry for this mode. */
  char new_entry[320];
  SNPRINTF(new_entry, "%u|%d|%s", (unsigned)mode_flag, enabled, tags ? tags : "");

  /* Parse existing entries, replace the one for this mode. */
  blender::Vector<std::string> entries;
  if (storage[0]) {
    char *data_copy = BLI_strdup(storage);
    char *cursor = data_copy;
    while (*cursor) {
      char *entry_start = cursor;
      /* Find next ';' separator. */
      while (*cursor && *cursor != ';') {
        cursor++;
      }
      if (*cursor == ';') {
        *cursor = '\0';
        cursor++;
      }

      /* Check if this entry is for the same mode. */
      /* Entry format: "flag|enabled|tags" */
      unsigned int entry_flag = 0;
      if (sscanf(entry_start, "%u|", &entry_flag) == 1 && entry_flag == mode_flag) {
        /* Skip old entry - will be replaced. */
        continue;
      }
      entries.append(std::string(entry_start));
    }
    MEM_delete(data_copy);
  }

  /* Add new/updated entry. */
  entries.append(std::string(new_entry));

  /* Rebuild storage string. */
  storage[0] = '\0';
  for (int i = 0; i < entries.size(); i++) {
    size_t current_len = strlen(storage);
    size_t entry_len = entries[i].size();
    /* +1 for separator or null terminator. */
    if (current_len + entry_len + 2 >= 1024) {
      break;  /* Storage full. */
    }
    if (i > 0) {
      BLI_strncat(storage, ";", 1024);
    }
    BLI_strncat(storage, entries[i].c_str(), 1024);
  }
}

/** \} */

/**
 * Build a sorted tag combination key from active tags string.
 * Tags are sorted alphabetically to ensure consistent keys.
 */
void tag_build_combination_key(const char *active_tags, char *r_key, int max_len)
{
  if (!active_tags || !active_tags[0]) {
    r_key[0] = '\0';
    return;
  }

  /* Use shared utility to split tags (avoids strtok static state). */
  blender::Vector<std::string> tags;
  category_tab_split_tags(active_tags, tags, ",;");

  if (tags.is_empty()) {
    r_key[0] = '\0';
    return;
  }

  /* Sort alphabetically for consistent keys */
  std::sort(tags.begin(), tags.end());

  /* Build key string */
  r_key[0] = '\0';
  for (int i = 0; i < tags.size(); i++) {
    if (i > 0) {
      BLI_strncat(r_key, ";", max_len);
    }
    BLI_strncat(r_key, tags[i].c_str(), max_len);
  }
}

/**
 * Save the last active category for a specific tag combination.
 * Uses LRU cleanup when approaching storage limit to prevent overflow.
 */
void tag_save_last_active_category(bContext *C, const char *tags_combination, const char *category)
{
  if (!C || !tags_combination || !category) {
    return;
  }

  ScrArea *area = CTX_wm_area(C);
  char *storage = tag_last_active_categories_storage_get(area);
  if (!storage) {
    return;
  }

  /* Parse existing data into entries, skipping entry with same tags.
   * Records are separated by '\n' (newline never appears in a tag key, which
   * uses ';' internally), so multi-tag combination keys parse correctly. */
  blender::Vector<std::string> entries;
  if (storage[0]) {
    blender::Vector<std::string> tokens;
    category_tab_split_tags(storage, tokens, "\n");
    for (const std::string &entry : tokens) {
      /* Check if this entry is for the same tags. The combination key never
       * contains ':', so the first ':' always separates key from category. */
      size_t colon_pos = entry.find(':');
      if (colon_pos != std::string::npos) {
        std::string existing_tags = entry.substr(0, colon_pos);
        if (existing_tags == tags_combination) {
          /* Skip old entry - will be replaced at the end (most recent). */
          continue;
        }
      }
      entries.append(entry);
    }
  }

  /* Add new entry (most recent goes at the end). */
  std::string new_entry = std::string(tags_combination) + ":" + category;
  entries.append(new_entry);

  /* LRU cleanup: remove oldest entries if approaching size limit.
   * Track the total size once and decrement as entries are dropped to keep
   * the trim linear in the number of entries. */
  size_t total_size = 0;
  for (const std::string &entry : entries) {
    total_size += entry.size() + 1; /* +1 for separator or null. */
  }
  int first_kept = 0;
  while (first_kept < entries.size() - 1 &&
         total_size > TAG_LAST_ACTIVE_CATEGORIES_MAX_SIZE)
  {
    total_size -= entries[first_kept].size() + 1;
    first_kept++;
  }

  /* Rebuild string. Records are separated by '\n' so multi-tag keys (which use
   * ';' internally) round-trip without ambiguity. */
  storage[0] = '\0';
  for (int i = first_kept; i < entries.size(); i++) {
    if (i > first_kept) {
      BLI_strncat(storage, "\n", 1024);
    }
    BLI_strncat(storage, entries[i].c_str(), 1024);
  }
}

/**
 * Get the last active category for a specific tag combination.
 */
bool tag_get_last_active_category(
    bContext *C, const char *tags_combination, char *r_category, int max_len)
{
  if (!C || !tags_combination || !r_category || max_len <= 0) {
    return false;
  }

  ScrArea *area = CTX_wm_area(C);
  const char *storage = tag_last_active_categories_storage_get(area);
  if (!storage || !storage[0]) {
    return false;
  }

  /* Parse and find entry. Records are separated by '\n'; the combination key
   * never contains ':', so the first ':' separates key from category. */
  blender::Vector<std::string> entries;
  category_tab_split_tags(storage, entries, "\n");

  for (const std::string &entry : entries) {
    size_t colon_pos = entry.find(':');
    if (colon_pos != std::string::npos) {
      std::string existing_tags = entry.substr(0, colon_pos);
      if (existing_tags == tags_combination) {
        std::string category_str = entry.substr(colon_pos + 1);
        BLI_strncpy(r_category, category_str.c_str(), max_len);
        return true;
      }
    }
  }

  r_category[0] = '\0';
  return false;
}

/**
 * Register the tag bar filter popover panel for a given region type.
 * This function should be called from each spacetype that uses the TAG_BAR region.
 *
 * \param art: The region type to register the panel with (typically TAG_BAR region)
 */
void tag_bar_filter_popover_panel_register(ARegionType *art)
{
  PanelType *pt = MEM_new_zeroed<PanelType>("tag bar filter popover panel");
  STRNCPY_UTF8(pt->idname, "VIEW3D_PT_tag_bar_filter_popover");
  STRNCPY_UTF8(pt->label, N_("Tag Filter"));
  STRNCPY_UTF8(pt->category, "");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = tag_bar_filter_popover_panel_draw;
  pt->ui_units_x = 10;  /* Width of popover in UI units */
  BLI_addtail(&art->paneltypes, pt);

  /* Add to global panel type list so popovers can find it from anywhere */
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Add-on Filter State
 * \{ */

bool is_new_addon_filter_active(const ScrArea *area)
{
  if (!area || !area->spacedata.first) {
    return false;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      const View3D *v3d = static_cast<const View3D *>(area->spacedata.first);
      return v3d->tabs_state.new_addon_filter_active != 0;
    }
    case SPACE_PROPERTIES: {
      const SpaceProperties *sbuts = static_cast<const SpaceProperties *>(area->spacedata.first);
      return sbuts->tabs_state.new_addon_filter_active != 0;
    }
    case SPACE_NODE: {
      const SpaceNode *snode = static_cast<const SpaceNode *>(area->spacedata.first);
      return snode->tabs_state.new_addon_filter_active != 0;
    }
    case SPACE_IMAGE: {
      const SpaceImage *sima = static_cast<const SpaceImage *>(area->spacedata.first);
      return sima->tabs_state.new_addon_filter_active != 0;
    }
  }

  return false;
}

void set_new_addon_filter_active(ScrArea *area, bool active)
{
  set_new_addon_filter_active(area, active, false);
}

void set_new_addon_filter_active(ScrArea *area, bool active, bool auto_activated)
{
  if (!area || !area->spacedata.first) {
    return;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      v3d->tabs_state.new_addon_filter_active = active ? 1 : 0;
      v3d->tabs_state.new_addon_filter_auto_activated = auto_activated ? 1 : 0;
      break;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      sbuts->tabs_state.new_addon_filter_active = active ? 1 : 0;
      sbuts->tabs_state.new_addon_filter_auto_activated = auto_activated ? 1 : 0;
      break;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      snode->tabs_state.new_addon_filter_active = active ? 1 : 0;
      snode->tabs_state.new_addon_filter_auto_activated = auto_activated ? 1 : 0;
      break;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      sima->tabs_state.new_addon_filter_active = active ? 1 : 0;
      sima->tabs_state.new_addon_filter_auto_activated = auto_activated ? 1 : 0;
      break;
    }
  }
}

bool is_new_addon_filter_auto_activated(const ScrArea *area)
{
  if (!area || !area->spacedata.first) {
    return false;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      const View3D *v3d = static_cast<const View3D *>(area->spacedata.first);
      return v3d->tabs_state.new_addon_filter_auto_activated != 0;
    }
    case SPACE_PROPERTIES: {
      const SpaceProperties *sbuts = static_cast<const SpaceProperties *>(area->spacedata.first);
      return sbuts->tabs_state.new_addon_filter_auto_activated != 0;
    }
    case SPACE_NODE: {
      const SpaceNode *snode = static_cast<const SpaceNode *>(area->spacedata.first);
      return snode->tabs_state.new_addon_filter_auto_activated != 0;
    }
    case SPACE_IMAGE: {
      const SpaceImage *sima = static_cast<const SpaceImage *>(area->spacedata.first);
      return sima->tabs_state.new_addon_filter_auto_activated != 0;
    }
  }

  return false;
}

char *get_saved_tag_filter_tags(const ScrArea *area)
{
  if (!area || !area->spacedata.first) {
    return nullptr;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      const View3D *v3d = static_cast<const View3D *>(area->spacedata.first);
      return const_cast<char *>(v3d->tabs_state.saved_tag_filter_tags);
    }
    case SPACE_PROPERTIES: {
      const SpaceProperties *sbuts = static_cast<const SpaceProperties *>(area->spacedata.first);
      return const_cast<char *>(sbuts->tabs_state.saved_tag_filter_tags);
    }
    case SPACE_NODE: {
      const SpaceNode *snode = static_cast<const SpaceNode *>(area->spacedata.first);
      return const_cast<char *>(snode->tabs_state.saved_tag_filter_tags);
    }
    case SPACE_IMAGE: {
      const SpaceImage *sima = static_cast<const SpaceImage *>(area->spacedata.first);
      return const_cast<char *>(sima->tabs_state.saved_tag_filter_tags);
    }
  }

  return nullptr;
}

void set_saved_tag_filter_tags(ScrArea *area, const char *tags)
{
  if (!area || !area->spacedata.first) {
    return;
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      STRNCPY(v3d->tabs_state.saved_tag_filter_tags, tags ? tags : "");
      break;
    }
    case SPACE_PROPERTIES: {
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      STRNCPY(sbuts->tabs_state.saved_tag_filter_tags, tags ? tags : "");
      break;
    }
    case SPACE_NODE: {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      STRNCPY(snode->tabs_state.saved_tag_filter_tags, tags ? tags : "");
      break;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      STRNCPY(sima->tabs_state.saved_tag_filter_tags, tags ? tags : "");
      break;
    }
  }
}

const char *get_new_addon_tag_glyph()
{
  /* Hex codepoint for U+F23A (Material Symbols "new_releases" icon). */
  return "f23a";
}

void get_new_addon_tag_color(float r_color[3])
{
  r_color[0] = 0.0f;
  r_color[1] = 0.6f;
  r_color[2] = 0.02f;
}

/** \} */

}  // namespace blender::ui
