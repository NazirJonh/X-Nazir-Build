/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 * \brief Blender util stuff
 */

namespace blender {

struct Main;
struct UserDef;

/**
 * Only to be called on exit Blender.
 */
void BKE_blender_free();

void BKE_blender_globals_init();
void BKE_blender_globals_clear();

/** Replace current global Main by the given one, freeing existing one. */
void BKE_blender_globals_main_replace(Main *bmain);
/**
 * Replace current global Main by the given one, returning the old one.
 *
 * \warning Advanced, risky workaround addressing the issue that current RNA is not able to process
 * correctly non-G_MAIN data, use with (a lot of) care.
 */
Main *BKE_blender_globals_main_swap(Main *new_gmain);

void BKE_blender_globals_crash_path_get(char *filepath);

void BKE_blender_userdef_data_swap(UserDef *userdef_a, UserDef *userdef_b);
void BKE_blender_userdef_data_set(UserDef *userdef);
void BKE_blender_userdef_data_set_and_free(UserDef *userdef);

/**
 * This function defines which settings a template will override for the user preferences.
 *
 * \note the order of `userdef_a` & `userdef_b` isn't important as values are simply swapped.
 */
void BKE_blender_userdef_app_template_data_swap(UserDef *userdef_a, UserDef *userdef_b);
void BKE_blender_userdef_app_template_data_set(UserDef *userdef);
void BKE_blender_userdef_app_template_data_set_and_free(UserDef *userdef);

/**
 * Categories of #UserDef that #BKE_blender_userdef_sync_from is able to transfer.
 */
enum eUserdefSyncFlag {
  /** #UserDef::addons, including the #bAddon::prop add-on preferences. */
  USER_SYNC_ADDONS = 1 << 0,
  /** #UserDef::extension_repos. */
  USER_SYNC_REPOS = 1 << 1,
  /** #UserDef::themes. */
  USER_SYNC_THEME = 1 << 2,
  /** #UserDef::user_keymaps, #UserDef::user_keyconfig_prefs and #UserDef::keyconfigstr. */
  USER_SYNC_KEYMAP = 1 << 3,
  /** #UserDef::user_menus, the quick favorites. */
  USER_SYNC_FAVORITES = 1 << 4,
  /** #UserDef::asset_libraries, #UserDef::script_directories and #UserDef::autoexec_paths. */
  USER_SYNC_PATHS = 1 << 5,
};

/**
 * Transfer the selected categories from the preferences of another Blender configuration.
 *
 * Only the categories listed in #eUserdefSyncFlag are read or written, so preferences this build
 * adds on top of the ones the source knows about are left untouched. This is what separates it
 * from replacing the whole configuration directory.
 *
 * List nodes are moved, not copied: whatever is taken over gets unlinked from \a userdef_src, so
 * the caller can still free \a userdef_src with #BKE_blender_userdef_data_free afterwards.
 *
 * \param flags: Bit-field of #eUserdefSyncFlag.
 */
void BKE_blender_userdef_sync_from(UserDef *userdef_dst, UserDef *userdef_src, int flags);

/**
 * Merge named themes from \a userdef_src into \a userdef_dst, replacing same-named entries.
 * Unmatched themes in \a userdef_dst are kept. Nodes are moved out of \a userdef_src.
 */
void BKE_blender_userdef_sync_themes_selective(UserDef *userdef_dst, UserDef *userdef_src);

/**
 * When loading a new userdef from file,
 * or when exiting Blender.
 */
void BKE_blender_userdef_data_free(UserDef *userdef, bool clear_fonts);

/* Blenders' own atexit (avoids leaking) */
void BKE_blender_atexit_register(void (*func)(void *user_data), void *user_data);
void BKE_blender_atexit_unregister(void (*func)(void *user_data), const void *user_data);
void BKE_blender_atexit();

}  // namespace blender
