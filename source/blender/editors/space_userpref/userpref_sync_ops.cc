/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 */

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DNA_theme_types.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_addon.h"
#include "BKE_blender.hh"
#include "BKE_blendfile.hh"
#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_preferences.h"
#include "BKE_report.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "MEM_guardedalloc.h"

#include "userpref_intern.hh"

namespace blender {
/* -------------------------------------------------------------------- */
/** \name Synchronize Preferences Analysis Operator
 * \{ */

static bool asset_library_dirpaths_match(const char *path_a, const char *path_b)
{
  if (path_a[0] == '\0' && path_b[0] == '\0') {
    return true;
  }
  char norm_a[FILE_MAX];
  char norm_b[FILE_MAX];
  STRNCPY(norm_a, path_a);
  STRNCPY(norm_b, path_b);
  BLI_path_normalize(norm_a);
  BLI_path_normalize(norm_b);
  BLI_path_slash_rstrip(norm_a);
  BLI_path_slash_rstrip(norm_b);
  return BLI_path_cmp_normalized(norm_a, norm_b) == 0;
}

static wmOperatorStatus preferences_userdef_sync_analyze_exec(bContext * /*C*/, wmOperator *op)
{
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  UserDef *userdef_src = BKE_blendfile_userdef_read(filepath, op->reports);
  if (userdef_src == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* A preferences file created by an older Blender may not contain the default user asset library.
   * The normal startup path supplies this entry while initializing preferences; do the same for
   * the isolated source UserDef used by this analysis operator. */
  if (userdef_src->asset_libraries.is_empty()) {
    BKE_preferences_asset_library_default_add(userdef_src);
  }

  std::unordered_map<std::string, std::string> source_libraries;
  for (const bUserAssetLibrary &library : userdef_src->asset_libraries) {
    source_libraries.emplace(library.name, library.dirpath);
  }

  std::unordered_map<std::string, std::string> destination_libraries;
  for (const bUserAssetLibrary &library : U.asset_libraries) {
    destination_libraries.emplace(library.name, library.dirpath);
  }

  BKE_reportf(op->reports,
              RPT_INFO,
              "Asset libraries read: source %zu, current %zu",
              source_libraries.size(),
              destination_libraries.size());

  std::string added;
  std::string updated;
  std::string disabled;
  std::string source_names;
  std::string destination_names;
  for (const auto &[name, path] : source_libraries) {
    (void)path;
    if (!source_names.empty()) {
      source_names += ';';
    }
    source_names += name;
  }
  for (const auto &[name, path] : destination_libraries) {
    (void)path;
    if (!destination_names.empty()) {
      destination_names += ';';
    }
    destination_names += name;
  }
  for (const auto &[name, path] : source_libraries) {
    const auto found = destination_libraries.find(name);
    if (found == destination_libraries.end()) {
      if (!added.empty()) {
        added += ';';
      }
      added += name;
    }
    else if (!asset_library_dirpaths_match(found->second.c_str(), path.c_str())) {
      if (!updated.empty()) {
        updated += ';';
      }
      updated += name;
    }
  }
  for (const auto &[name, path] : destination_libraries) {
    (void)path;
    if (!source_libraries.contains(name)) {
      if (!disabled.empty()) {
        disabled += ';';
      }
      disabled += name;
    }
  }

  std::unordered_set<std::string> source_themes;
  std::unordered_set<std::string> destination_themes;
  for (const bTheme &theme : userdef_src->themes) {
    if (theme.name[0] != '\0') {
      source_themes.insert(theme.name);
    }
  }
  for (const bTheme &theme : U.themes) {
    if (theme.name[0] != '\0') {
      destination_themes.insert(theme.name);
    }
  }

  std::string themes_added;
  std::string themes_updated;
  for (const std::string &name : source_themes) {
    if (!destination_themes.contains(name)) {
      if (!themes_added.empty()) {
        themes_added += ';';
      }
      themes_added += name;
    }
    else {
      if (!themes_updated.empty()) {
        themes_updated += ';';
      }
      themes_updated += name;
    }
  }

  RNA_string_set(op->ptr, "added", added.c_str());
  RNA_string_set(op->ptr, "updated", updated.c_str());
  RNA_string_set(op->ptr, "disabled", disabled.c_str());
  RNA_string_set(op->ptr, "source_names", source_names.c_str());
  RNA_string_set(op->ptr, "destination_names", destination_names.c_str());
  RNA_string_set(op->ptr, "themes_added", themes_added.c_str());
  RNA_string_set(op->ptr, "themes_updated", themes_updated.c_str());

  /* Python calls operators with a non-zero #wmWindowManager.op_undo_depth, so the window manager
   * does not store last-used properties for bpy.ops. Persist results so
   * #WM_operator_last_properties_ensure can expose them to scripts and the sync UI. */
  WM_operator_last_properties_store(op);

  BKE_blender_userdef_data_free(userdef_src, false);
  MEM_delete(userdef_src);
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_userdef_sync_analyze(wmOperatorType *ot)
{
  ot->name = "Analyze Preferences Synchronization";
  ot->idname = "PREFERENCES_OT_userdef_sync_analyze";
  ot->description = "Compare asset libraries and themes in another preferences file with the current ones";
  ot->exec = preferences_userdef_sync_analyze_exec;
  ot->flag = OPTYPE_INTERNAL | OPTYPE_REGISTER;

  RNA_def_string_file_path(ot->srna, "filepath", nullptr, FILE_MAX, "File Path", "Preferences file to analyze");
  RNA_def_string(ot->srna, "added", nullptr, 0, "Added", "Semicolon-separated added asset libraries");
  RNA_def_string(ot->srna, "updated", nullptr, 0, "Updated", "Semicolon-separated updated asset libraries");
  RNA_def_string(ot->srna, "disabled", nullptr, 0, "Disabled", "Semicolon-separated disabled asset libraries");
  RNA_def_string(ot->srna,
                 "source_names",
                 nullptr,
                 0,
                 "Source Libraries",
                 "Semicolon-separated asset libraries read from the source preferences");
  RNA_def_string(ot->srna,
                 "destination_names",
                 nullptr,
                 0,
                 "Destination Libraries",
                 "Semicolon-separated asset libraries in the current preferences");
  RNA_def_string(ot->srna,
                 "themes_added",
                 nullptr,
                 0,
                 "Added Themes",
                 "Semicolon-separated themes present in the source but not in the current preferences");
  RNA_def_string(ot->srna,
                 "themes_updated",
                 nullptr,
                 0,
                 "Updated Themes",
                 "Semicolon-separated themes present in both preferences");
}

static bool addon_module_is_extension(const char *module)
{
  return BLI_str_startswith(module, "bl_ext.");
}

static std::string extension_selection_key_from_module(const char *module)
{
  if (!addon_module_is_extension(module)) {
    return {};
  }
  const char *rest = module + strlen("bl_ext.");
  const char *dot = strchr(rest, '.');
  if (dot == nullptr || dot[1] == '\0') {
    return {};
  }
  return std::string(rest, dot - rest) + "/" + (dot + 1);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Merge Preferences Operator
 * \{ */

static wmOperatorStatus preferences_userdef_merge_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  /* #BKE_blendfile_userdef_read already reports the failure into op->reports. */
  UserDef *userdef_src = BKE_blendfile_userdef_read(filepath, op->reports);
  if (userdef_src == nullptr) {
    return OPERATOR_CANCELLED;
  }

  int flags = 0;
  const bool use_theme = RNA_boolean_get(op->ptr, "use_theme");
  const bool sync_themes_selective = use_theme && RNA_boolean_get(op->ptr, "selected_themes_active");
  if (RNA_boolean_get(op->ptr, "use_addons")) {
    flags |= USER_SYNC_ADDONS;
  }
  if (RNA_boolean_get(op->ptr, "use_repos")) {
    flags |= USER_SYNC_REPOS;
  }
  if (use_theme && !sync_themes_selective) {
    flags |= USER_SYNC_THEME;
  }
  if (RNA_boolean_get(op->ptr, "use_keymap")) {
    flags |= USER_SYNC_KEYMAP;
  }
  if (RNA_boolean_get(op->ptr, "use_favorites")) {
    flags |= USER_SYNC_FAVORITES;
  }
  if (RNA_boolean_get(op->ptr, "use_paths")) {
    flags |= USER_SYNC_PATHS;
  }

  auto selected_names = [&](const char *property) {
    char value[FILE_MAX];
    RNA_string_get(op->ptr, property, value);
    std::unordered_set<std::string> result;
    std::string current;
    for (const char character : std::string(value)) {
      if (character == ';') {
        if (!current.empty()) {
          result.insert(current);
          current.clear();
        }
      }
      else {
        current += character;
      }
    }
    if (!current.empty()) {
      result.insert(current);
    }
    return result;
  };

  const std::unordered_set<std::string> selected_addons = selected_names("selected_addons");
  const std::unordered_set<std::string> selected_extensions = selected_names("selected_extensions");
  const std::unordered_set<std::string> selected_asset_libraries = selected_names(
      "selected_asset_libraries");
  const std::unordered_set<std::string> selected_themes = selected_names("selected_themes");
  if (RNA_boolean_get(op->ptr, "selected_addons_active")) {
    std::vector<std::string> addons_to_remove;
    for (bAddon &addon : userdef_src->addons.items_mutable()) {
      if (addon_module_is_extension(addon.module)) {
        continue;
      }
      if (!selected_addons.contains(addon.module)) {
        addons_to_remove.emplace_back(addon.module);
      }
    }
    for (const std::string &module : addons_to_remove) {
      BKE_addon_remove_safe(&userdef_src->addons, module.c_str());
    }
  }
  if (RNA_boolean_get(op->ptr, "selected_extensions_active")) {
    std::vector<std::string> addons_to_remove;
    for (bAddon &addon : userdef_src->addons.items_mutable()) {
      if (!addon_module_is_extension(addon.module)) {
        continue;
      }
      const std::string key = extension_selection_key_from_module(addon.module);
      if (!selected_extensions.contains(key)) {
        addons_to_remove.emplace_back(addon.module);
      }
    }
    for (const std::string &module : addons_to_remove) {
      BKE_addon_remove_safe(&userdef_src->addons, module.c_str());
    }
  }
  if (RNA_boolean_get(op->ptr, "selected_asset_libraries_active")) {
    std::vector<std::string> libraries_to_remove;
    for (bUserAssetLibrary &library : userdef_src->asset_libraries.items_mutable()) {
      if (!selected_asset_libraries.contains(library.name)) {
        libraries_to_remove.emplace_back(library.name);
      }
    }
    for (const std::string &name : libraries_to_remove) {
      if (bUserAssetLibrary *library = BKE_preferences_asset_library_find_by_name(userdef_src,
                                                                                   name.c_str()))
      {
        BKE_preferences_asset_library_remove(userdef_src, library);
      }
    }
  }
  if (RNA_boolean_get(op->ptr, "selected_themes_active")) {
    std::vector<std::string> themes_to_remove;
    for (const bTheme &theme : userdef_src->themes) {
      if (!selected_themes.contains(theme.name)) {
        themes_to_remove.emplace_back(theme.name);
      }
    }
    for (const std::string &name : themes_to_remove) {
      bTheme *theme = static_cast<bTheme *>(
          BLI_findstring(&userdef_src->themes, name.c_str(), offsetof(bTheme, name)));
      if (theme != nullptr) {
        BLI_remlink(&userdef_src->themes, theme);
        MEM_delete(theme);
      }
    }
  }

  /* A repository pointing inside the source installation would resolve to a directory the file
   * sync never populates, since only the modules it does not have yet are copied there. Rather
   * than merging it with a rewritten directory that was never filled, drop it entirely so the
   * merge does not leave behind a repository whose extensions silently fail to load. */
  char source_root[FILE_MAX];
  RNA_string_get(op->ptr, "source_root", source_root);
  if (source_root[0] != '\0') {
    for (bUserExtensionRepo &repo : userdef_src->extension_repos.items_mutable()) {
      if (repo.custom_dirpath[0] != '\0' && BLI_path_contains(source_root, repo.custom_dirpath)) {
        BLI_remlink(&userdef_src->extension_repos, &repo);
        MEM_SAFE_DELETE(repo.access_token);
        MEM_delete(&repo);
      }
    }
  }

  BKE_callback_exec_null(bmain, BKE_CB_EVT_EXTENSION_REPOS_UPDATE_PRE);

  BKE_blender_userdef_sync_from(&U, userdef_src, flags);

  if (sync_themes_selective) {
    BKE_blender_userdef_sync_themes_selective(&U, userdef_src);
  }

  if ((flags & USER_SYNC_THEME) || sync_themes_selective) {
    /* #blender::ui::theme caches a raw #bTheme pointer that is only refreshed at region-draw
     * time. Refresh it now so a stale pointer is never live between the merge above and the next
     * redraw. Deliberately not `init_default()`: that would overwrite the freshly merged colors
     * with the built-in defaults. */
    ui::theme::theme_set(0, 0);
  }

  /* The source fonts were never loaded into BLF, so they must not be unloaded from it. */
  BKE_blender_userdef_data_free(userdef_src, false);
  MEM_delete(userdef_src);

  BKE_callback_exec_null(bmain, BKE_CB_EVT_EXTENSION_REPOS_UPDATE_POST);

  WM_reinit_gizmomap_all(bmain);
  WM_keyconfig_reload(C);

  U.runtime.is_dirty = true;
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  WM_event_add_notifier(C, NC_UI | ND_UI_FONT, nullptr);

  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_userdef_merge(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Merge Preferences";
  ot->idname = "PREFERENCES_OT_userdef_merge";
  ot->description = "Merge selected categories of another preferences file into the current ones";

  /* callbacks */
  ot->exec = preferences_userdef_merge_exec;

  /* flags */
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_string_file_path(
      ot->srna, "filepath", nullptr, FILE_MAX, "File Path", "Preferences file to merge from");
  RNA_def_string_dir_path(ot->srna,
                          "source_root",
                          nullptr,
                          FILE_MAX,
                          "Source Root",
                          "Configuration directory the file belongs to, used to reject extension "
                          "repositories that point back into it");

  RNA_def_boolean(ot->srna, "use_addons", false, "Add-ons", "Merge add-ons and their preferences");
  RNA_def_boolean(ot->srna, "use_repos", false, "Repositories", "Merge extension repositories");
  RNA_def_boolean(ot->srna, "use_theme", false, "Theme", "Replace the theme");
  RNA_def_boolean(ot->srna, "use_keymap", false, "Keymap", "Replace the key map");
  RNA_def_boolean(ot->srna, "use_favorites", false, "Quick Favorites", "Replace quick favorites");
  RNA_def_boolean(
      ot->srna, "use_paths", false, "Paths", "Merge asset libraries, script and auto-run paths");
  RNA_def_string(ot->srna, "selected_addons", nullptr, FILE_MAX, "Selected Add-ons", "Semicolon-separated add-on modules");
  RNA_def_string(ot->srna, "selected_extensions", nullptr, FILE_MAX, "Selected Extensions", "Semicolon-separated extension modules");
  RNA_def_string(ot->srna, "selected_asset_libraries", nullptr, FILE_MAX, "Selected Asset Libraries", "Semicolon-separated asset library names");
  RNA_def_string(ot->srna, "selected_themes", nullptr, FILE_MAX, "Selected Themes", "Semicolon-separated theme names");
  RNA_def_boolean(ot->srna, "selected_addons_active", false, "Use Add-on Selection", "Use only selected add-ons");
  RNA_def_boolean(ot->srna, "selected_extensions_active", false, "Use Extension Selection", "Use only selected extensions");
  RNA_def_boolean(ot->srna, "selected_asset_libraries_active", false, "Use Asset Library Selection", "Use only selected asset libraries");
  RNA_def_boolean(ot->srna, "selected_themes_active", false, "Use Theme Selection", "Use only selected themes");
}

/** \} */
void ED_operatortypes_userpref_sync()
{
  WM_operatortype_append(PREFERENCES_OT_userdef_sync_analyze);
  WM_operatortype_append(PREFERENCES_OT_userdef_merge);
}

}  // namespace blender
