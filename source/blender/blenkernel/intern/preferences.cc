/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * User defined asset library API.
 */

#include <algorithm>
#include <climits>
#include <cstring>

#include "AS_essentials_library.hh"
#include "AS_remote_library.hh"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_vector.hh"

#include "BKE_appdir.hh"
#include "BKE_asset.hh"
#include "BKE_blender_version.h"
#include "BKE_preferences.h"

#include "BLI_utildefines.h"

#include "CLG_log.h"

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

namespace blender {

#define U BLI_STATIC_ASSERT(false, "Global 'U' not allowed, only use arguments passed in!")

static CLG_LogRef LOG = {"bke.preferences"};

/* -------------------------------------------------------------------- */
/** \name Preferences File
 * \{ */

namespace bke::preferences {

bool exists()
{
  const std::optional<std::string> cfgdir = BKE_appdir_folder_id(BLENDER_USER_CONFIG, nullptr);
  if (!cfgdir.has_value()) {
    return false;
  }

  char userpref[FILE_MAX];
  BLI_path_join(userpref, sizeof(userpref), cfgdir->c_str(), BLENDER_USERPREF_FILE);
  return BLI_exists(userpref);
}

}  // namespace bke::preferences

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Library Hierarchy Helpers
 * \{ */

namespace blender::bke::preferences {

/**
 * Find an asset library by name using linear search in the linked list.
 * Used primarily during hierarchy restoration after file loading.
 *
 * \param userdef: The UserDef structure containing asset libraries.
 * \param name: The name to search for. Must not be null.
 * \return Pointer to the asset library with matching name, or nullptr if not found.
 * \note The search is case-sensitive and uses full name matching.
 */
static bUserAssetLibrary *find_asset_library_by_name(const UserDef *userdef, const char *name)
{
  if (!name || name[0] == '\0') {
    return nullptr;
  }

  for (bUserAssetLibrary *lib = static_cast<bUserAssetLibrary *>(userdef->asset_libraries.first);
       lib;
       lib = lib->next)
  {
    if (STREQ(lib->name, name)) {
      return lib;
    }
  }

  return nullptr;
}

/**
 * Update both the parent pointer and parent_name when changing a library's parent.
 * This ensures parent_name and parent pointer stay synchronized.
 *
 * \param library: The library whose parent is being changed.
 * \param new_parent: The new parent library, or nullptr for root level.
 * \note This is an internal helper function.
 */
static void update_asset_library_parent_name(bUserAssetLibrary *library,
                                             bUserAssetLibrary *new_parent)
{
  library->parent = new_parent;
  if (new_parent) {
    STRNCPY(library->parent_name, new_parent->name);
  }
  else {
    library->parent_name[0] = '\0';
  }
}

/**
 * Restore parent pointers from parent_name strings in the asset library hierarchy.
 *
 * Performs two passes:
 * 1. First pass: Restore parent pointers from parent_name
 * 2. Second pass: Detect and break cycles (shouldn't happen normally, but defensive)
 *
 * This function must be called after loading UserDef from a file.
 *
 * \param userdef: The UserDef structure with asset libraries.
 * \note Any broken parent_name references will result in the library being placed at root level.
 */
void restore_asset_library_hierarchy(UserDef *userdef)
{
  /* First pass: restore parent pointers from parent_name strings saved in DNA. */
  for (bUserAssetLibrary *lib = static_cast<bUserAssetLibrary *>(userdef->asset_libraries.first);
       lib;
       lib = lib->next)
  {
    lib->parent = find_asset_library_by_name(userdef, lib->parent_name);
    if (!lib->parent && lib->parent_name[0]) {
      CLOG_WARN(&LOG,
                "Asset library '%s': parent '%s' not found, placing at root level",
                lib->name,
                lib->parent_name);
      lib->parent_name[0] = '\0';
    }
  }

  /* Second pass: detect and break cycles using depth limit. */
  for (bUserAssetLibrary *lib = static_cast<bUserAssetLibrary *>(userdef->asset_libraries.first);
       lib;
       lib = lib->next)
  {
    if (!lib->parent) {
      continue;
    }
    const int MAX_DEPTH = 1000;
    int depth = 0;
    bUserAssetLibrary *current = lib->parent;
    while (current && depth < MAX_DEPTH) {
      if (current == lib) {
        CLOG_WARN(&LOG, "Asset library '%s': cycle detected, placing at root level", lib->name);
        lib->parent = nullptr;
        lib->parent_name[0] = '\0';
        break;
      }
      current = current->parent;
      depth++;
    }
    if (depth >= MAX_DEPTH && current) {
      CLOG_WARN(&LOG,
                "Asset library '%s': hierarchy depth exceeds limit, placing at root level",
                lib->name);
      lib->parent = nullptr;
      lib->parent_name[0] = '\0';
    }
  }
}

}  // namespace blender::bke::preferences

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Libraries
 * \{ */

bUserAssetLibrary *BKE_preferences_asset_library_add(UserDef *userdef,
                                                     const char *name,
                                                     const char *dirpath)
{
  bUserAssetLibrary *library = MEM_new<bUserAssetLibrary>(__func__);

  BLI_addtail(&userdef->asset_libraries, library);
  if (userdef->experimental.no_data_block_packing) {
    library->import_method = ASSET_IMPORT_APPEND_REUSE;
  }
  if (name) {
    BKE_preferences_asset_library_name_set(userdef, library, name);
  }
  if (dirpath) {
    STRNCPY(library->dirpath, dirpath);
  }

  return library;
}

/* Defined with the rest of the pinned-library API below. Declared here because every removal has to
 * re-compact the pin order. */
static void asset_library_pin_order_compact(UserDef *userdef);

void BKE_preferences_asset_library_remove(UserDef *userdef, bUserAssetLibrary *library)
{
  /* Caller must ensure the library has no children (use BKE_preferences_asset_library_can_delete).
   * Removing a library with children would leave dangling parent pointers. */
  BLI_assert(BKE_preferences_asset_library_can_delete(userdef, library));
  BLI_freelinkN(&userdef->asset_libraries, library);

  /* The entry just removed may have been pinned, which would leave a hole in the ordering. Every
   * removal in the codebase funnels through here, so this one call covers them all. */
  asset_library_pin_order_compact(userdef);
}

/**
 * Identifiers #BKE_preferences_asset_library_identifier_from_ref() hands out to the builtin
 * libraries. A custom library is identified by its name alone, so one taking a builtin's
 * identifier would be silently resolved to that builtin wherever the identifier is turned back
 * into a reference (asset browser collapse state, image grid catalog filters).
 */
static bool asset_library_name_is_reserved(const char *name)
{
  return STR_ELEM(name, "local", "all", "essentials", "online_essentials", "default");
}

void BKE_preferences_asset_library_name_set(UserDef *userdef,
                                            bUserAssetLibrary *library,
                                            const char *name)
{
  STRNCPY_UTF8(library->name, name);
  if (asset_library_name_is_reserved(library->name)) {
    /* Use the delimiter #BLI_uniquename() knows, so it resolves a further clash with an already
     * existing "local.001" below in the usual way. */
    BLI_strncat(library->name, ".001", sizeof(library->name));
  }
  BLI_uniquename(&userdef->asset_libraries,
                 library,
                 name,
                 '.',
                 offsetof(bUserAssetLibrary, name),
                 sizeof(library->name));

  /* When a folder is renamed, propagate the new name to all direct children's parent_name.
   * This keeps parent_name in sync so hierarchy is correctly restored after file reload. */
  if (library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    for (bUserAssetLibrary &item : userdef->asset_libraries) {
      if (item.parent == library) {
        STRNCPY(item.parent_name, library->name);
      }
    }
  }
}

void BKE_preferences_asset_library_path_set(bUserAssetLibrary *library, const char *path)
{
  STRNCPY(library->dirpath, path);
  if (BLI_is_file(library->dirpath)) {
    BLI_path_parent_dir(library->dirpath);
  }
}

bUserAssetLibrary *BKE_preferences_asset_library_find_index(const UserDef *userdef, int index)
{
  return static_cast<bUserAssetLibrary *>(BLI_findlink(&userdef->asset_libraries, index));
}

bUserAssetLibrary *BKE_preferences_asset_library_find_by_name(const UserDef *userdef,
                                                              const char *name)
{
  return static_cast<bUserAssetLibrary *>(
      BLI_findstring(&userdef->asset_libraries, name, offsetof(bUserAssetLibrary, name)));
}

bUserAssetLibrary *BKE_preferences_asset_library_find_from_ref(const UserDef *userdef,
                                                              const AssetLibraryReference *ref)
{
  if (ref->type != ASSET_LIBRARY_CUSTOM) {
    return nullptr;
  }
  if (ref->custom_library_name[0]) {
    return BKE_preferences_asset_library_find_by_name(userdef, ref->custom_library_name);
  }
  /* Written before #AssetLibraryReference.custom_library_name existed: the index is all there is.
   * It may already be stale, but there is nothing better to go on. */
  return BKE_preferences_asset_library_find_index(userdef, ref->custom_library_index);
}

void BKE_preferences_asset_library_reference_set(const UserDef *userdef,
                                                 AssetLibraryReference *r_ref,
                                                 const bUserAssetLibrary *user_library)
{
  r_ref->type = ASSET_LIBRARY_CUSTOM;
  STRNCPY(r_ref->custom_library_name, user_library->name);
  r_ref->custom_library_index = BLI_findindex(&userdef->asset_libraries, user_library);
}

bUserAssetLibrary *BKE_preferences_asset_library_containing_path(const UserDef *userdef,
                                                                 const char *path)
{
  for (bUserAssetLibrary &asset_lib_pref : userdef->asset_libraries) {
    if (asset_lib_pref.dirpath[0] && BLI_path_contains(asset_lib_pref.dirpath, path)) {
      return &asset_lib_pref;
    }
  }
  return nullptr;
}

int BKE_preferences_asset_library_get_index(const UserDef *userdef,
                                            const bUserAssetLibrary *library)
{
  return BLI_findindex(&userdef->asset_libraries, library);
}

bool BKE_preferences_asset_library_is_valid(const UserDef *userdef,
                                            const bUserAssetLibrary *library,
                                            const bool check_directory_exists)
{
  /* Folders are containers, never valid as a real asset library. */
  if (library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    return false;
  }

  /* Check disabled libraries, including inheritance from a disabled parent folder. */
  if (!BKE_preferences_asset_library_is_effectively_enabled(library)) {
    return false;
  }

  /* Check remote libraries. */
  const bool is_remote_library = library->flag & ASSET_LIBRARY_USE_REMOTE_URL;
  const bool skip_remote_libraries = !USER_EXPERIMENTAL_TEST(userdef, use_remote_asset_libraries);
  if (is_remote_library && skip_remote_libraries) {
    return false;
  }
  if (is_remote_library && !library->remote_url[0]) {
    return false;
  }

  /* Note that there's no check if the path exists on disk here. If an invalid library path is
   * used, the Asset Browser can give a nice hint on what's wrong, so include such items in enums
   * the user can choose from. */
  if (!library->dirpath[0]) {
    return false;
  }
  if (check_directory_exists && !BLI_is_dir(library->dirpath)) {
    return false;
  }

  return true;
}

void BKE_preferences_asset_library_default_add(UserDef *userdef)
{
  char documents_path[FILE_MAXDIR];

  /* No home or documents path found, not much we can do. */
  if (!BKE_appdir_folder_documents(documents_path) || !documents_path[0]) {
    return;
  }

  bUserAssetLibrary *library = BKE_preferences_asset_library_add(
      userdef, DATA_(BKE_PREFS_ASSET_LIBRARY_DEFAULT_NAME), nullptr);

  /* Add new "Default" library under '[doc_path]/Blender/Assets'. */
  BLI_path_join(
      library->dirpath, sizeof(library->dirpath), documents_path, N_("Blender"), N_("Assets"));
}

bUserAssetLibrary *BKE_preferences_remote_asset_library_add(UserDef *userdef,
                                                            const char *name,
                                                            const char *remote_url)
{
  bUserAssetLibrary *library = MEM_new<bUserAssetLibrary>(__func__);

  library->flag |= ASSET_LIBRARY_USE_REMOTE_URL;
  BLI_addtail(&userdef->asset_libraries, library);

  if (name) {
    BKE_preferences_asset_library_name_set(userdef, library, name);
  }

  BKE_preferences_remote_asset_library_url_set(library, remote_url);

  return library;
}

/**
 * Appends a slash to \a str if there isn't one there already. Will do nothing if \a str is empty.
 *
 * \param max_len: The maximum length \a str is allowed to have, including 0-terminator.
 */
static void url_ensure_trailing_slash(char *str, const size_t max_len)
{
  const size_t len = BLI_strnlen(str, max_len);
  BLI_assert_msg(str[len] == '\0', "String should be null-terminated");

  if (len > 0 && str[len - 1] != '/' && len + 1 < max_len) {
    str[len] = '/';
    str[len + 1] = '\0';
  }
}

void BKE_preferences_remote_asset_library_url_set(bUserAssetLibrary *library,
                                                  const StringRef remote_url)
{
  /* Always trim white-space off of URLs. */
  remote_url.trim().copy_bytes_truncated(library->remote_url);

  const bool ends_in_top_meta_file = asset_system::remote_library_url_ends_with_top_meta_file_name(
      library->remote_url);

  if (!ends_in_top_meta_file) {
    url_ensure_trailing_slash(library->remote_url, sizeof(library->remote_url));
  }

  /* Update location cache path. */
  const std::string library_dirpath =
      asset_system::is_online_essentials_url(library->remote_url) ?
          /* Special (unusual) case: When the URL path matches the online essentials URL, use the
           * online essentials cache directory path. Otherwise the downloader deduplicates the
           * requests, and only downloads file to one of the directories. */
          std::string{asset_system::online_essentials_cache_directory_path()} :
          asset_system::remote_library_cache_directory_path_from_url(remote_url);
  BLI_strncpy_utf8(library->dirpath, library_dirpath.c_str(), sizeof(library->dirpath));
}
bUserAssetLibrary *BKE_preferences_asset_library_folder_add(UserDef *userdef,
                                                            const char *name,
                                                            bUserAssetLibrary *parent)
{
  bUserAssetLibrary *folder = MEM_new<bUserAssetLibrary>(__func__);

  folder->type = USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER;
  /* Set parent using helper function to keep parent_name synchronized. */
  blender::bke::preferences::update_asset_library_parent_name(folder, parent);
  folder->dirpath[0] = '\0';
  folder->remote_url[0] = '\0';
  folder->import_method = ASSET_IMPORT_PACK;
  folder->flag = ASSET_LIBRARY_RELATIVE_PATH;

  if (name) {
    BKE_preferences_asset_library_name_set(userdef, folder, name);
  }

  /* Insert the folder in the correct position in the list:
   * after the parent and all its descendants. */
  if (parent) {
    bUserAssetLibrary *insert_after = parent;

    /* Find the last descendant of the parent. */
    for (bUserAssetLibrary &item : userdef->asset_libraries) {
      if (item.parent == parent) {
        insert_after = &item;
      }
    }

    BLI_insertlinkafter(&userdef->asset_libraries, insert_after, folder);
  }
  else {
    /* For root level folders, add to the end of the list. */
    BLI_addtail(&userdef->asset_libraries, folder);
  }

  return folder;
}

void BKE_preferences_asset_library_move_to_folder(UserDef *userdef,
                                                  bUserAssetLibrary *library,
                                                  bUserAssetLibrary *new_parent)
{
  /* This only relinks the item itself, not a subtree. Callers must not pass a populated folder;
   * use #BKE_preferences_asset_library_reorder for folders that may contain children.
   * #can_delete is true for any non-folder and for empty folders, i.e. exactly the childless case. */
  BLI_assert(BKE_preferences_asset_library_can_delete(userdef, library));

  if (library->parent == new_parent) {
    return; /* Already in the correct folder. */
  }

  /* Prevent moving a folder into itself or its descendants. */
  if (library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    bUserAssetLibrary *current = new_parent;
    while (current) {
      if (current == library) {
        return; /* Cannot move folder into itself. */
      }
      current = current->parent;
    }
  }

  /* Update parent using helper function to keep parent_name synchronized. */
  blender::bke::preferences::update_asset_library_parent_name(library, new_parent);

  /* Remove from current position. */
  BLI_remlink(&userdef->asset_libraries, library);

  /* Insert in the correct position. */
  if (new_parent) {
    bUserAssetLibrary *insert_after = new_parent;

    /* Find the last descendant of the new parent. */
    for (bUserAssetLibrary &item : userdef->asset_libraries) {
      if (item.parent == new_parent) {
        insert_after = &item;
      }
    }

    BLI_insertlinkafter(&userdef->asset_libraries, insert_after, library);
  }
  else {
    /* For root level, add to the end of the list. */
    BLI_addtail(&userdef->asset_libraries, library);
  }
}

bool BKE_preferences_asset_library_is_folder(const bUserAssetLibrary *library)
{
  return library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER;
}

bool BKE_preferences_asset_library_can_delete(const UserDef *userdef,
                                              const bUserAssetLibrary *library)
{
  if (library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    /* Folders can only be deleted if they are empty. */
    for (const bUserAssetLibrary &item : userdef->asset_libraries) {
      if (item.parent == library) {
        return false; /* Folder has children. */
      }
    }
  }
  return true;
}

/**
 * Reorder an asset library or folder within its parent.
 * \param userdef: The user preferences.
 * \param library: The library or folder to reorder.
 * \param target: The target library or folder to reorder relative to.
 * \param location: Where to place the library relative to the target (Before, After, Into).
 * \return True if the reorder was successful.
 */
bool BKE_preferences_asset_library_reorder(UserDef *userdef,
                                           bUserAssetLibrary *library,
                                           bUserAssetLibrary *target,
                                           eBKE_AssetLibraryMoveLocation location)
{
  if (!library || !target || library == target) {
    return false;
  }

  bUserAssetLibrary *new_parent = nullptr;

  if (location == ASSET_LIBRARY_MOVE_INTO) {
    /* Into: Move into the target folder. */
    if (target->type != USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      return false; /* Can only move into folders. */
    }
    new_parent = target;
  }
  else {
    /* Before/After: Move to the same parent as the target. */
    new_parent = target->parent;
  }

  /* Folders never nest inside other folders: a folder always lives at the root level. Reject any
   * move that would give a folder a folder parent (dropping it into a folder, or before/after an
   * item that itself lives inside one). Libraries can still be moved into folders. */
  if (library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER && new_parent != nullptr) {
    return false;
  }

  /* Prevent creating a cycle: the resulting parent must not be the moved folder itself nor any of
   * its descendants. This also covers Before/After drops onto an item that lives inside the folder
   * being moved (e.g. dropping a folder right after one of its own children), which would
   * otherwise set the folder as its own ancestor. */
  if (library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    for (bUserAssetLibrary *current = new_parent; current; current = current->parent) {
      if (current == library) {
        return false; /* Would create a cycle. */
      }
    }
  }

  /* If moving a folder, collect all items in its subtree to move together. */
  Vector<bUserAssetLibrary *> items_to_move;
  items_to_move.append(library);

  if (library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    for (bUserAssetLibrary &item : userdef->asset_libraries) {
      bUserAssetLibrary *current_parent = item.parent;
      bool is_descendant = false;
      while (current_parent) {
        if (current_parent == library) {
          is_descendant = true;
          break;
        }
        current_parent = current_parent->parent;
      }
      if (is_descendant) {
        items_to_move.append(&item);
      }
    }
  }

  /* Remove all items from current position (in reverse order to maintain relative positions). */
  for (int i = items_to_move.size() - 1; i >= 0; i--) {
    BLI_remlink(&userdef->asset_libraries, items_to_move[i]);
  }

  /* Update parent for the main item. Descendant items keep their internal parent relationships.
   * This also updates parent_name so the hierarchy survives a file save/load cycle. */
  blender::bke::preferences::update_asset_library_parent_name(library, new_parent);

  /* Insert the main item in the correct position. */
  if (location == ASSET_LIBRARY_MOVE_BEFORE) {
    BLI_insertlinkbefore(&userdef->asset_libraries, target, library);
  }
  else if (location == ASSET_LIBRARY_MOVE_AFTER) {
    BLI_insertlinkafter(&userdef->asset_libraries, target, library);
  }
  else {
    /* Into: Insert after the last descendant of the new parent. */
    if (new_parent) {
      bUserAssetLibrary *insert_after = new_parent;

      /* Find the last descendant in the entire subtree of the new parent.
       * We need to check all items and find the one with the deepest nesting
       * that has new_parent as an ancestor. */
      for (bUserAssetLibrary &item : userdef->asset_libraries) {
        /* Check if item is a descendant of new_parent. */
        bUserAssetLibrary *current_parent = item.parent;
        bool is_descendant = false;

        while (current_parent) {
          if (current_parent == new_parent) {
            is_descendant = true;
            break;
          }
          current_parent = current_parent->parent;
        }

        if (is_descendant) {
          insert_after = &item;
        }
      }

      BLI_insertlinkafter(&userdef->asset_libraries, insert_after, library);
    }
    else {
      /* For root level, add to the end of the list. */
      BLI_addtail(&userdef->asset_libraries, library);
    }
  }

  /* Insert all descendant items after the main item, maintaining their relative order. */
  for (int i = 1; i < items_to_move.size(); i++) {
    BLI_insertlinkafter(&userdef->asset_libraries, items_to_move[i - 1], items_to_move[i]);
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pinned Asset Libraries
 * \{ */

/* Re-establish the dense 0..N-1 ordering over the pinned libraries, preserving their relative
 * order. Every pin mutation ends here, which is what keeps #bUserAssetLibrary.pin_order free of
 * gaps and duplicates -- the invariant the popover's tab row relies on. */
static void asset_library_pin_order_compact(UserDef *userdef)
{
  Vector<bUserAssetLibrary *> pinned;
  for (bUserAssetLibrary &library : userdef->asset_libraries) {
    if (library.flag & ASSET_LIBRARY_IS_PINNED) {
      pinned.append(&library);
    }
  }
  /* Stable, so two entries that somehow share an order keep their listbase order rather than
   * swapping unpredictably. */
  std::stable_sort(pinned.begin(),
                   pinned.end(),
                   [](const bUserAssetLibrary *a, const bUserAssetLibrary *b) {
                     return a->pin_order < b->pin_order;
                   });
  for (const int i : pinned.index_range()) {
    pinned[i]->pin_order = short(i);
  }
}

void BKE_preferences_asset_library_pin_set(UserDef *userdef,
                                           bUserAssetLibrary *library,
                                           const bool pinned)
{
  if (pinned == ((library->flag & ASSET_LIBRARY_IS_PINNED) != 0)) {
    return;
  }

  if (pinned) {
    library->flag |= ASSET_LIBRARY_IS_PINNED;
    /* Sort last; the compaction below turns this into the real (dense) trailing index, so there is
     * no need to count the existing pins here. */
    library->pin_order = SHRT_MAX;
  }
  else {
    library->flag &= ~ASSET_LIBRARY_IS_PINNED;
    library->pin_order = 0;
  }

  asset_library_pin_order_compact(userdef);
}

int BKE_preferences_asset_library_pinned_count(const UserDef *userdef)
{
  int count = 0;
  for (const bUserAssetLibrary &library : userdef->asset_libraries) {
    if (library.flag & ASSET_LIBRARY_IS_PINNED) {
      count++;
    }
  }
  return count;
}

bool BKE_preferences_asset_library_pin_reorder(UserDef *userdef,
                                               bUserAssetLibrary *library,
                                               const int new_index)
{
  if ((library->flag & ASSET_LIBRARY_IS_PINNED) == 0) {
    return false;
  }

  const int count = BKE_preferences_asset_library_pinned_count(userdef);
  const int target = std::clamp(new_index, 0, count - 1);
  const int current = library->pin_order;
  if (target == current) {
    return false;
  }

  /* Shift everything between the old and the new slot one step towards the slot being vacated,
   * then drop the library into the new one. The order is dense on entry (invariant), so this
   * leaves it dense without a re-compaction. */
  for (bUserAssetLibrary &other : userdef->asset_libraries) {
    if (&other == library || (other.flag & ASSET_LIBRARY_IS_PINNED) == 0) {
      continue;
    }
    if (target < current && other.pin_order >= target && other.pin_order < current) {
      other.pin_order++;
    }
    else if (target > current && other.pin_order > current && other.pin_order <= target) {
      other.pin_order--;
    }
  }
  library->pin_order = short(target);

  return true;
}

/* The #UserDef.asset_flag bit carrying \a type's pin, or 0 for a library that has none.
 *
 * This is the single place that decides which built-ins are pinnable. #ASSET_LIBRARY_ALL is always
 * shown, and #ASSET_LIBRARY_CUSTOM keeps #ASSET_LIBRARY_IS_PINNED on its own #bUserAssetLibrary, so
 * both map to nothing.
 *
 * #ASSET_LIBRARY_ONLINE_ESSENTIALS is deliberately absent: the asset shelf builds its library enum
 * with `include_separate_online_essentials = false` hardcoded (#rna_asset_library_ui_reference_itemf),
 * and the tab row is derived from that enum, so a bit for it could never produce a tab. If the shelf
 * ever offers it separately, this is where it is added -- one case and one free bit. */
static eUserPref_AssetFlag asset_builtin_pin_flag_from_type(const eAssetLibraryType type)
{
  switch (type) {
    case ASSET_LIBRARY_LOCAL:
      return USER_ASSETS_PIN_CURRENT_FILE;
    case ASSET_LIBRARY_ESSENTIALS:
      return USER_ASSETS_PIN_ESSENTIALS;
    default:
      return eUserPref_AssetFlag(0);
  }
}

bool BKE_preferences_asset_builtin_pin_supported(const eAssetLibraryType type)
{
  return asset_builtin_pin_flag_from_type(type) != 0;
}

bool BKE_preferences_asset_builtin_pin_get(const UserDef *userdef, const eAssetLibraryType type)
{
  const eUserPref_AssetFlag flag = asset_builtin_pin_flag_from_type(type);
  return (flag != 0) && ((userdef->asset_flag & flag) != 0);
}

void BKE_preferences_asset_builtin_pin_set(UserDef *userdef,
                                           const eAssetLibraryType type,
                                           const bool pinned)
{
  const eUserPref_AssetFlag flag = asset_builtin_pin_flag_from_type(type);
  if (flag == 0) {
    return;
  }

  if (pinned) {
    userdef->asset_flag |= flag;
  }
  else {
    userdef->asset_flag &= ~flag;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extension Repositories
 * \{ */

/**
 * A string copy that ensures: `[A-Za-z]+[A-Za-z0-9_]*`.
 */
static size_t strncpy_py_module(char *dst, const char *src, const size_t dst_maxncpy)
{
  const size_t dst_len_max = dst_maxncpy - 1;
  dst[0] = '\0';
  size_t i_src = 0, i_dst = 0;
  while (src[i_src] && (i_dst < dst_len_max)) {
    const char c = src[i_src++];
    const bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    /* The first character must be `[a-zA-Z]`. */
    if (i_dst == 0 && !is_alpha) {
      continue;
    }
    const bool is_num = (is_alpha == false) && ((c >= '0' && c <= '9') || c == '_');
    if (!(is_alpha || is_num)) {
      continue;
    }
    dst[i_dst++] = c;
  }
  dst[i_dst] = '\0';
  return i_dst;
}

bUserExtensionRepo *BKE_preferences_extension_repo_add(UserDef *userdef,
                                                       const char *name,
                                                       const char *module,
                                                       const char *custom_dirpath)
{
  bUserExtensionRepo *repo = MEM_new<bUserExtensionRepo>(__func__);
  BLI_addtail(&userdef->extension_repos, repo);

  /* Set the unique ID-name. */
  BKE_preferences_extension_repo_name_set(userdef, repo, name);

  /* Set the unique module-name. */
  BKE_preferences_extension_repo_module_set(userdef, repo, module);

  /* Set the directory. */
  STRNCPY(repo->custom_dirpath, custom_dirpath);
  BLI_path_normalize(repo->custom_dirpath);
  BLI_path_slash_rstrip(repo->custom_dirpath);

  /* While not a strict rule, ignored paths that already exist, *
   * pointing to the same path is going to logical problems with package-management. */
  for (const bUserExtensionRepo &repo_iter : userdef->extension_repos) {
    if (repo == &repo_iter) {
      continue;
    }
    if (BLI_path_cmp(repo->custom_dirpath, repo_iter.custom_dirpath) == 0) {
      repo->custom_dirpath[0] = '\0';
      break;
    }
  }

  return repo;
}

void BKE_preferences_extension_repo_remove(UserDef *userdef, bUserExtensionRepo *repo)
{
  if (repo->access_token) {
    MEM_delete(repo->access_token);
  }
  BLI_freelinkN(&userdef->extension_repos, repo);
}

bUserExtensionRepo *BKE_preferences_extension_repo_add_default_remote(UserDef *userdef)
{
  bUserExtensionRepo *repo = BKE_preferences_extension_repo_add(
      userdef, "extensions.blender.org", "blender_org", "");
  /* The trailing slash on this URL is important, without it a redirect is used. */
  STRNCPY(repo->remote_url, "https://extensions.blender.org/api/v1/extensions/");
  /* Disable `blender.org` by default, the initial "Online Preferences" section gives
   * the option to enable this. */
  repo->flag |= USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL | USER_EXTENSION_REPO_FLAG_SYNC_ON_STARTUP;
  return repo;
}

bUserExtensionRepo *BKE_preferences_extension_repo_add_default_user(UserDef *userdef)
{
  bUserExtensionRepo *repo = BKE_preferences_extension_repo_add(
      userdef, "User Default", "user_default", "");
  return repo;
}

bUserExtensionRepo *BKE_preferences_extension_repo_add_default_system(UserDef *userdef)
{
  bUserExtensionRepo *repo = BKE_preferences_extension_repo_add(userdef, "System", "system", "");
  repo->source = USER_EXTENSION_REPO_SOURCE_SYSTEM;
  return repo;
}

void BKE_preferences_extension_repo_add_defaults_all(UserDef *userdef)
{
  BLI_assert(userdef->extension_repos.is_empty());
  BKE_preferences_extension_repo_add_default_remote(userdef);
  BKE_preferences_extension_repo_add_default_user(userdef);
  BKE_preferences_extension_repo_add_default_system(userdef);
}

void BKE_preferences_extension_repo_name_set(UserDef *userdef,
                                             bUserExtensionRepo *repo,
                                             const char *name)
{
  if (*name == '\0') {
    name = "User Repository";
  }
  STRNCPY_UTF8(repo->name, name);

  BLI_uniquename(&userdef->extension_repos,
                 repo,
                 name,
                 '.',
                 offsetof(bUserExtensionRepo, name),
                 sizeof(repo->name));
}

void BKE_preferences_extension_repo_module_set(UserDef *userdef,
                                               bUserExtensionRepo *repo,
                                               const char *module)
{
  if (strncpy_py_module(repo->module, module, sizeof(repo->module)) == 0) {
    STRNCPY(repo->module, "repository");
  }

  BLI_uniquename(&userdef->extension_repos,
                 repo,
                 module,
                 '_',
                 offsetof(bUserExtensionRepo, module),
                 sizeof(repo->module));
}

bool BKE_preferences_extension_repo_module_is_valid(const bUserExtensionRepo *repo)
{
  /* NOTE: this should only ever return false in the case of corrupt file/memory
   * and can be considered an exceptional situation. */
  char module_test[sizeof(bUserExtensionRepo::module)];
  const size_t module_len = strncpy_py_module(module_test, repo->module, sizeof(repo->module));
  if (module_len == 0) {
    return false;
  }
  if (module_len != STRNLEN(repo->module)) {
    return false;
  }
  return true;
}

void BKE_preferences_extension_repo_custom_dirpath_set(bUserExtensionRepo *repo, const char *path)
{
  STRNCPY(repo->custom_dirpath, path);
}

size_t BKE_preferences_extension_repo_dirpath_get(const bUserExtensionRepo *repo,
                                                  char *dirpath,
                                                  const int dirpath_maxncpy)
{
  if (repo->flag & USER_EXTENSION_REPO_FLAG_USE_CUSTOM_DIRECTORY) {
    return BLI_strncpy_rlen(dirpath, repo->custom_dirpath, dirpath_maxncpy);
  }

  std::optional<std::string> path = std::nullopt;

  uint8_t source = repo->source;
  if (repo->flag & USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL) {
    source = USER_EXTENSION_REPO_SOURCE_USER;
  }

  switch (source) {
    case USER_EXTENSION_REPO_SOURCE_SYSTEM: {
      path = BKE_appdir_folder_id(BLENDER_SYSTEM_EXTENSIONS, nullptr);
      break;
    }
    default: { /* #USER_EXTENSION_REPO_SOURCE_USER. */
      path = BKE_appdir_folder_id_user_notest(BLENDER_USER_EXTENSIONS, nullptr);
      break;
    }
  }

  /* Highly unlikely to fail as the directory doesn't have to exist. */
  if (!path) {
    dirpath[0] = '\0';
    return 0;
  }
  return BLI_path_join(dirpath, dirpath_maxncpy, path.value().c_str(), repo->module);
}

size_t BKE_preferences_extension_repo_user_dirpath_get(const bUserExtensionRepo *repo,
                                                       char *dirpath,
                                                       const int dirpath_maxncpy)
{
  if (std::optional<std::string> path = BKE_appdir_folder_id_user_notest(BLENDER_USER_EXTENSIONS,
                                                                         nullptr))
  {
    return BLI_path_join(dirpath, dirpath_maxncpy, path.value().c_str(), ".user", repo->module);
  }
  return 0;
}

bUserExtensionRepo *BKE_preferences_extension_repo_find_index(const UserDef *userdef, int index)
{
  return static_cast<bUserExtensionRepo *>(BLI_findlink(&userdef->extension_repos, index));
}

bUserExtensionRepo *BKE_preferences_extension_repo_find_by_module(const UserDef *userdef,
                                                                  const char *module)
{
  return static_cast<bUserExtensionRepo *>(
      BLI_findstring(&userdef->extension_repos, module, offsetof(bUserExtensionRepo, module)));
}

static bool url_char_is_delimiter(const char ch)
{
  /* Punctuation (space to comma). */
  if (ch >= 32 && ch <= 44) {
    return true;
  }
  /* Other characters (colon to at-sign). */
  if (ch >= 58 && ch <= 64) {
    return true;
  }
  if (ELEM(ch, '/', '\\')) {
    return true;
  }
  return false;
}

bUserExtensionRepo *BKE_preferences_extension_repo_find_by_remote_url_prefix(
    const UserDef *userdef, const char *remote_url_full, const bool only_enabled)
{
  const int path_full_len = strlen(remote_url_full);
  const int path_full_offset = BKE_preferences_remote_scheme_end(remote_url_full);

  for (bUserExtensionRepo &repo : userdef->extension_repos) {
    if (only_enabled && (repo.flag & USER_EXTENSION_REPO_FLAG_DISABLED)) {
      continue;
    }

    /* Has a valid remote path to check. */
    if ((repo.flag & USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL) == 0) {
      continue;
    }
    if (repo.remote_url[0] == '\0') {
      continue;
    }

    /* Set path variables which may be offset by the "scheme". */
    const char *path_repo = repo.remote_url;
    const char *path_test = remote_url_full;
    int path_test_len = path_full_len;

    /* Allow paths beginning with both `http` & `https` to be considered equivalent.
     * This is done by skipping the "scheme" prefix both have a scheme. */
    if (path_full_offset) {
      const int path_repo_offset = BKE_preferences_remote_scheme_end(path_repo);
      if (path_repo_offset) {
        path_repo += path_repo_offset;
        path_test += path_full_offset;
        path_test_len -= path_full_offset;
      }
    }

    /* The length of the path without trailing slashes. */
    int path_repo_len = strlen(path_repo);
    while (path_repo_len && ELEM(path_repo[path_repo_len - 1], '/', '\\')) {
      path_repo_len--;
    }

    if (path_test_len <= path_repo_len) {
      continue;
    }
    if (memcmp(path_repo, path_test, path_repo_len) != 0) {
      continue;
    }

    /* A delimiter must follow to ensure `path_test` doesn't reference a longer host-name.
     * Will typically be a `/` or a `:`. */
    if (!url_char_is_delimiter(path_test[path_repo_len])) {
      continue;
    }
    return &repo;
  }
  return nullptr;
}

int BKE_preferences_extension_repo_get_index(const UserDef *userdef,
                                             const bUserExtensionRepo *repo)
{
  return BLI_findindex(&userdef->extension_repos, repo);
}

void BKE_preferences_extension_repo_read_data(BlendDataReader *reader, bUserExtensionRepo *repo)
{
  if (repo->access_token) {
    BLO_read_string(reader, &repo->access_token);
  }
}

void BKE_preferences_extension_repo_write_data(BlendWriter *writer, const bUserExtensionRepo *repo)
{
  if (repo->access_token) {
    writer->write_string(repo->access_token);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Web/remote utilities
 * \{ */

int BKE_preferences_remote_scheme_end(const char *url)
{
  /* Technically the "://" are not part of the scheme, so subtract 3 from the return value. */
  const char *scheme_check[] = {
      "http://",
      "https://",
      "file://",
  };
  for (int i = 0; i < ARRAY_SIZE(scheme_check); i++) {
    const char *scheme = scheme_check[i];
    int scheme_len = strlen(scheme);
    if (strncmp(url, scheme, scheme_len) == 0) {
      return scheme_len - 3;
    }
  }
  return 0;
}

void BKE_preferences_remote_to_name(const char *remote_url, char name[MAX_NAME])
{
#ifdef _WIN32
  const bool is_win32 = true;
#else
  const bool is_win32 = false;
#endif
  const bool is_file = STRPREFIX(remote_url, "file://");
  name[0] = '\0';
  if (int offset = BKE_preferences_remote_scheme_end(remote_url)) {
    /* Skip the `://`. */
    remote_url += (offset + 3);

    if (is_file) {
      if (is_win32) {
        /* Skip the slash prefix for: `/C:/`,
         * not *required* but seems like a bug if it's not done. */
        if (remote_url[0] == '/' && isalpha(remote_url[1]) && (remote_url[2] == ':')) {
          remote_url += 1;
        }
      }
    }
    else {
      /* Skip the `www` as it's not useful information. */
      if (BLI_str_startswith(remote_url, "www.")) {
        remote_url += 4;
      }
    }
  }
  if (UNLIKELY(remote_url[0] == '\0')) {
    return;
  }

  const char *c = remote_url;
  if (is_file) {
    /* TODO: decode the URL, see: #GHOST_URL_decode which is not a public function. */

    /* Don't use domain name only logic for file paths as this causes
     * `file:///path/to/repo/index.json` -> `/path`
     * In this case `/path/to/repo` is preferred. */
    c = BLI_path_basename(remote_url);
    /* Remove trailing slash. */
    while ((remote_url < c) && url_char_is_delimiter(*(c - 1))) {
      c--;
    }
  }
  else {
    /* Skip any delimiters (likely forward slashes for `file:///` on UNIX).
     * Although the `file://` case is handled already. So this is quite unlikely.
     * Skip them anyway because failing to do so may cause the domain to be an empty string. */
    while (*c && url_char_is_delimiter(*c)) {
      c++;
    }
    /* Skip the domain name. */
    while (*c && !url_char_is_delimiter(*c)) {
      c++;
    }
  }

  BLI_strncpy_utf8(name, remote_url, std::min(size_t(c - remote_url) + 1, size_t(MAX_NAME)));

  if (is_win32) {
    if (is_file) {
      BLI_path_slash_native(name);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #bUserAssetShelfSettings
 * \{ */

static bUserAssetShelfSettings *asset_shelf_settings_new(UserDef *userdef,
                                                         const char *shelf_idname)
{
  bUserAssetShelfSettings *settings = MEM_new<bUserAssetShelfSettings>(__func__);
  BLI_addtail(&userdef->asset_shelves_settings, settings);
  STRNCPY(settings->shelf_idname, shelf_idname);
  BLI_assert(settings->enabled_catalog_paths.is_empty());
  return settings;
}

static bUserAssetShelfSettings *asset_shelf_settings_ensure(UserDef *userdef,
                                                            const char *shelf_idname)
{
  if (bUserAssetShelfSettings *settings = BKE_preferences_asset_shelf_settings_get(userdef,
                                                                                   shelf_idname))
  {
    return settings;
  }
  return asset_shelf_settings_new(userdef, shelf_idname);
}

bUserAssetShelfSettings *BKE_preferences_asset_shelf_settings_get(const UserDef *userdef,
                                                                  const char *shelf_idname)
{
  return static_cast<bUserAssetShelfSettings *>(
      BLI_findstring(&userdef->asset_shelves_settings,
                     shelf_idname,
                     offsetof(bUserAssetShelfSettings, shelf_idname)));
}

bool BKE_preferences_asset_shelf_settings_is_catalog_path_enabled(const UserDef *userdef,
                                                                  const char *shelf_idname,
                                                                  const char *catalog_path)
{
  const bUserAssetShelfSettings *settings = BKE_preferences_asset_shelf_settings_get(userdef,
                                                                                     shelf_idname);
  if (!settings) {
    return false;
  }
  return BKE_asset_catalog_path_list_has_path(settings->enabled_catalog_paths, catalog_path);
}

bool BKE_preferences_asset_shelf_settings_ensure_catalog_path_enabled(UserDef *userdef,
                                                                      const char *shelf_idname,
                                                                      const char *catalog_path)
{
  if (BKE_preferences_asset_shelf_settings_is_catalog_path_enabled(
          userdef, shelf_idname, catalog_path))
  {
    return false;
  }

  bUserAssetShelfSettings *settings = asset_shelf_settings_ensure(userdef, shelf_idname);
  BKE_asset_catalog_path_list_add_path(settings->enabled_catalog_paths, catalog_path);
  return true;
}

void BKE_preferences_asset_shelf_popup_view_load(const UserDef *userdef,
                                                 const char *shelf_idname,
                                                 short *r_preview_size,
                                                 short *r_display_flag,
                                                 short *r_width_units,
                                                 short *r_height_units)
{
  const bUserAssetShelfSettings *settings = BKE_preferences_asset_shelf_settings_get(userdef,
                                                                                     shelf_idname);
  if (!settings || (settings->popup_view_flag & USER_ASSET_SHELF_POPUP_VIEW_STORED) == 0) {
    return;
  }
  if (r_preview_size) {
    *r_preview_size = settings->popup_preview_size;
  }
  if (r_display_flag) {
    *r_display_flag = settings->popup_display_flag;
  }
  if (r_width_units) {
    *r_width_units = settings->popup_width_units;
  }
  if (r_height_units) {
    *r_height_units = settings->popup_height_units;
  }
}

void BKE_preferences_asset_shelf_popup_view_store(UserDef *userdef,
                                                  const char *shelf_idname,
                                                  short preview_size,
                                                  short display_flag,
                                                  short width_units,
                                                  short height_units)
{
  bUserAssetShelfSettings *settings = asset_shelf_settings_ensure(userdef, shelf_idname);
  settings->popup_preview_size = preview_size;
  settings->popup_display_flag = display_flag;
  settings->popup_width_units = width_units;
  settings->popup_height_units = height_units;
  settings->popup_view_flag |= USER_ASSET_SHELF_POPUP_VIEW_STORED;
}

/** \} */

const EnumPropertyItem *BKE_preferences_active_section_itemf(const UserDef *userdef, bool *r_free)
{

  const bool use_developer_ui = (userdef->flag & USER_DEVELOPER_UI) != 0;
  const bool is_alpha = BKE_blender_version_is_alpha();

  if (use_developer_ui && is_alpha) {
    *r_free = false;
    return rna_enum_preference_section_items;
  }

  EnumPropertyItem *items = nullptr;
  int totitem = 0;

  for (const EnumPropertyItem *it = rna_enum_preference_section_items; it->identifier != nullptr;
       it++)
  {
    if (it->value == USER_SECTION_EXPERIMENTAL) {
      if (is_alpha == false) {
        continue;
      }
    }
    else if (it->value == USER_SECTION_DEVELOPER_TOOLS) {
      if (use_developer_ui == false) {
        continue;
      }
    }

    RNA_enum_item_add(&items, &totitem, it);
  }

  RNA_enum_item_end(&items, &totitem);

  *r_free = true;
  return items;
}

/* -------------------------------------------------------------------- */
/** \name Asset Library Hierarchy Restoration (Public API)
 * \{ */

void BKE_preferences_asset_library_restore_hierarchy(UserDef *userdef)
{
  blender::bke::preferences::restore_asset_library_hierarchy(userdef);
}

bool BKE_preferences_asset_library_is_effectively_enabled(const bUserAssetLibrary *library)
{
  for (const bUserAssetLibrary *lib = library; lib; lib = lib->parent) {
    if (lib->flag & ASSET_LIBRARY_DISABLED) {
      return false;
    }
  }
  return true;
}

/** \} */

}  // namespace blender
