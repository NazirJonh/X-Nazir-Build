/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Centralized C++ -> Python bridge for the Category Tabs / Glyph / Tags system.
 *
 * The Category Tabs feature persists most of its state through Python helpers in
 * `scripts/startup/bl_ui/space_userpref.py` (plus `glyph_library.*`). Historically each call site
 * built its own `BPY_run_string_exec()` command string inline, scattering the Python entry-point
 * names and the (already shared) string escaping across six translation units.
 *
 * This module is the single place that knows the Python API surface for the write path
 * (state-mutating calls). Every wrapper escapes arbitrary user strings via
 * #category_tab_escape_for_python_literal before interpolation.
 *
 * Python entry points used by the write path (all in `bl_ui.space_userpref` unless noted):
 *   - reset_category_to_defaults              (reset glyph)
 *   - set_category_tags                       (reset tags)
 *   - _save_glyph_mappings_to_file            (save to JSON, background)
 *   - set_category_data / finalize_category_tag_changes (save edited category)
 *   - restore_category_tags_from_string / restore_category_glyph_from_snapshot (cancel restore)
 *   - update_tag                              (live icon update on dialog OK)
 *   - assign_tag_to_category                  (deferred tag assignment after extension install)
 *   - mark_category_from_extension            (mark new extension category pending)
 *   - mark_all_unassigned_categories_as_without_tag (Alt+click on "New Add-on!")
 *   - set_category_order                      (persist drag-reorder)
 *
 * Read-path queries are also routed here: the bridge runs the Python expression and returns the
 * raw value (an int, or a JSON string that the caller parses with its local helpers) -
 * get_reserved_category_priority, get_category_order, glyph search, auto-detect extension icon.
 *
 * NOT routed here (by design):
 *   - The temporary Popular-Addons-Database lookup in interface_tab_categories_edit.cc: it parses
 *     marker-formatted stdout (not the clean API surface) and is slated for removal once extensions
 *     bundle their own icons.
 *   - `sync_wm_to_glyph_cache` invoked from `makesrna/rna_wm.cc`, to avoid a makesrna -> editors
 *     dependency.
 */

#pragma once

#include <cstdint>
#include <string>

#include "BLI_vector.hh"

struct bContext;

namespace blender::ui {

/* Reset the category's glyph/icon to defaults. Reads `wm.category_tab_save_category` (the caller
 * sets it beforehand) and clears it. */
void category_py_reset_to_defaults(bContext *C, int space_type);

/* Reset the category's tags to empty. Reads `wm.category_tab_save_category` and clears it. */
void category_py_reset_tags(bContext *C, int space_type);

/* Persist glyph mappings to the JSON file (off the main thread on the Python side).
 * Reads `wm.category_tab_save_category` and clears it. */
void category_py_save_glyph_mappings_to_file(bContext *C);

/* Persist the edited category (display name, glyph, color, icon, glyph mode) and finalize tag
 * changes. `first_letter`, `glyph_hex`, `color_hex`, `icon_source`, `glyph_mode` are non-arbitrary
 * (hex/enum) and are passed through; the remaining string arguments are escaped internally. */
void category_py_save_category_data(bContext *C,
                                    const char *category,
                                    const char *display_name,
                                    const char *first_letter,
                                    const char *glyph_hex,
                                    const char *color_hex,
                                    const char *icon_source,
                                    const char *icon_key,
                                    const char *icon_path,
                                    const char *icon_provider,
                                    const char *glyph_mode,
                                    int space_type);

/* Restore tags and glyph snapshot when the edit dialog is cancelled. Reads
 * `wm.category_tab_save_category` and clears it. Arbitrary strings are escaped internally. */
void category_py_restore_on_cancel(bContext *C,
                                   const char *tags,
                                   const char *glyph_hex,
                                   int glyph_mode,
                                   const float color[3],
                                   int space_type,
                                   int icon_source,
                                   const char *icon_key,
                                   const char *icon_path,
                                   const char *icon_provider);

/* Live-update a tag's icon in the Python cache (dialog OK path). */
void category_py_update_tag_icon(bContext *C,
                                 const char *tag_name,
                                 const char *icon_key,
                                 int icon_source);

/* Assign a single tag to a category. */
void category_py_assign_tag(bContext *C, const char *category, const char *tag, int space_type);

/* Mark every unassigned category as "Without Tag" for the given space/mode. Returns whether the
 * Python call succeeded (the caller should refresh the UI only on success). */
bool category_py_mark_all_unassigned_without_tag(bContext *C, int space_type, uint32_t mode_flag);

/* Mark a freshly introduced extension category as pending tag assignment. */
void category_py_mark_from_extension(
    bContext *C, const char *category, const char *extension_id, int space_type, uint32_t mode_flag);

/* Persist the category order for a given tag-combination key. */
void category_py_set_category_order(bContext *C,
                                    const char *tag_key,
                                    const Vector<std::string> &order);

/* Enable/disable Python "preview mode", which suppresses mappings sync until the dialog is saved. */
void category_py_set_preview_mode(bContext *C, bool active);

/* -------------------------------------------------------------------- */
/* Read path: query helpers that return values. The bridge runs the Python expression and returns
 * the raw result; any JSON parsing stays at the call site (it owns the parse helpers). */

/* Reserved-category priority (lower = earlier). Returns -1 when unknown or on failure. */
int category_py_get_reserved_priority(bContext *C,
                                      const char *category_id,
                                      const char *space_type_name);

/* Category order for a tag-combination key as a JSON array string (e.g. "[\"A\", \"B\"]").
 * Returns an empty string on failure. */
std::string category_py_get_category_order_json(bContext *C, const char *tag_key);

/* Glyph search results as a JSON string. Returns an empty string on failure. */
std::string category_py_search_glyphs_json(bContext *C,
                                           const char *query,
                                           const char *category,
                                           int max_results);

/* Auto-detected extension icon as a JSON array string ["icon_path", "icon_provider"] (with
 * backslashes normalized to '/'). Returns an empty string on failure. */
std::string category_py_auto_detect_extension_icon_json(bContext *C, const char *category);

}  // namespace blender::ui
