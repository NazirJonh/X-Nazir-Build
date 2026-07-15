/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <climits>
#include <sstream>
#include <optional>
#include <set>
#include <string>

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_string.h"
#include "BLI_string_utils.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"
#ifdef WIN32
#  include "BLI_winstuff.h"
#endif

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"
#include "DNA_uuid_types.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_preferences.h"
#include "BKE_preview_image.hh"

#include "IMB_thumbs.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "ED_asset_image_library.hh"
#include "ED_asset_library.hh"

#include "WM_api.hh"

namespace blender::ed::asset {

using namespace blender::io::serialize;
using namespace blender::asset_system;

static const char *IMAGE_EXTENSIONS[] = {
    ".png",
    ".jpg",
    ".jpeg",
    ".tga",
    ".tiff",
    ".tif",
    ".exr",
    ".hdr",
    ".bmp",
    nullptr,
};

constexpr StringRef ATTR_VERSION("version");
constexpr StringRef ATTR_LIBRARY_ROOT("library_root");
constexpr StringRef ATTR_ENTRIES("entries");
constexpr StringRef ATTR_RELATIVE_PATH("relative_path");
constexpr StringRef ATTR_MTIME("mtime");
constexpr StringRef ATTR_SIZE("size");
constexpr StringRef ATTR_CATALOG_ID("catalog_id");

constexpr int IMAGE_LIBRARY_INDEX_VERSION = 1;

struct ImageLibraryIndexEntry {
  std::string relative_path;
  int64_t mtime = 0;
  int64_t size = 0;
  bUUID catalog_id = BLI_uuid_nil();
};

struct ImageLibraryIndex {
  int version = IMAGE_LIBRARY_INDEX_VERSION;
  std::string library_root;
  Vector<ImageLibraryIndexEntry> entries;
};

bool image_library_is_image_filepath(const char *filepath)
{
  const char *ext = BLI_path_extension(filepath);
  if (!ext || !ext[0]) {
    return false;
  }
  for (int i = 0; IMAGE_EXTENSIONS[i]; i++) {
    if (BLI_strcasecmp(ext, IMAGE_EXTENSIONS[i]) == 0) {
      return true;
    }
  }
  return false;
}

static void image_library_index_filepath(const char *library_root_path, char r_path[FILE_MAX])
{
  BLI_path_join(r_path, FILE_MAX, library_root_path, IMAGE_LIBRARY_INDEX_FILENAME);
}

static void image_library_index_delete_if_exists(const char *library_root_path)
{
  char index_path[FILE_MAX];
  image_library_index_filepath(library_root_path, index_path);
  if (BLI_exists(index_path)) {
    BLI_delete(index_path, false, false);
  }
}

static std::string relative_dir_from_catalog_path(const StringRef catalog_path)
{
  std::string rel_dir(catalog_path);
  if (rel_dir == IMAGE_LIBRARY_ROOT_CATALOG_PATH) {
    return {};
  }
  /* Catalogs created under the virtual root item use paths like "Root/Textures". */
  const char *root_prefix = IMAGE_LIBRARY_ROOT_CATALOG_PATH;
  const size_t root_prefix_len = strlen(root_prefix);
  if (rel_dir.size() > root_prefix_len && rel_dir[root_prefix_len] == '/' &&
      BLI_str_startswith(rel_dir.c_str(), root_prefix))
  {
    rel_dir = rel_dir.substr(root_prefix_len + 1);
  }
  for (char &c : rel_dir) {
    if (c == '/') {
      c = SEP;
    }
  }
  return rel_dir;
}

static void join_library_root_and_relative(const char *library_root,
                                           const StringRef relative,
                                           char r_path[FILE_MAX])
{
  if (relative.is_empty()) {
    BLI_strncpy(r_path, library_root, FILE_MAX);
  }
  else {
    BLI_path_join(r_path, FILE_MAX, library_root, relative.data());
  }
  BLI_path_normalize(r_path);
}

static std::string catalog_path_from_image_relative_path(const char *relative_image_path)
{
  char dir[FILE_MAX];
  BLI_path_split_dir_part(relative_image_path, dir, sizeof(dir));
  if (!dir[0]) {
    return IMAGE_LIBRARY_ROOT_CATALOG_PATH;
  }
  /* BLI_path_split_dir_part includes the trailing separator (e.g. "textures/metal/").
   * Strip it so the path matches the catalog registered by scan_directory_recursive,
   * which uses rel_dir without a trailing separator. */
  BLI_path_slash_rstrip(dir);
  if (!dir[0]) {
    return IMAGE_LIBRARY_ROOT_CATALOG_PATH;
  }
  std::string catalog_path(dir);
  for (char &c : catalog_path) {
    if (c == SEP) {
      c = '/';
    }
  }
  return catalog_path;
}

static bUUID catalog_id_for_image_relative_path(AssetLibrary &library,
                                                const char *relative_image_path)
{
  const std::string catalog_path_str = catalog_path_from_image_relative_path(relative_image_path);
  const AssetCatalogPath catalog_path(catalog_path_str);
  if (const AssetCatalog *catalog = library.catalog_service().find_catalog_by_path(catalog_path)) {
    return catalog->catalog_id;
  }
  return BLI_uuid_nil();
}

static std::unique_ptr<ImageLibraryIndex> image_library_index_read(const char *library_root_path)
{
  char index_path[FILE_MAX];
  image_library_index_filepath(library_root_path, index_path);
  if (!BLI_exists(index_path)) {
    return nullptr;
  }

  FILE *file = BLI_fopen(index_path, "rb");
  if (!file) {
    return nullptr;
  }
  fseek(file, 0, SEEK_END);
  const long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  std::string file_content(size_t(file_size), '\0');
  const size_t read_size = fread(file_content.data(), 1, size_t(file_size), file);
  fclose(file);
  if (int64_t(read_size) != int64_t(file_size)) {
    return nullptr;
  }

  JsonFormatter formatter;
  std::istringstream stream(file_content);
  std::unique_ptr<Value> root_value = formatter.deserialize(stream);
  if (!root_value || root_value->type() != eValueType::Dictionary) {
    return nullptr;
  }

  const DictionaryValue &root = *root_value->as_dictionary_value();
  auto index = std::make_unique<ImageLibraryIndex>();

  if (const std::optional<int64_t> version = root.lookup_int(ATTR_VERSION)) {
    index->version = int(*version);
  }
  if (const std::optional<StringRefNull> library_root = root.lookup_str(ATTR_LIBRARY_ROOT)) {
    index->library_root = library_root->c_str();
  }

  if (index->version != IMAGE_LIBRARY_INDEX_VERSION) {
    return nullptr;
  }

  char norm_root[FILE_MAX];
  STRNCPY(norm_root, library_root_path);
  BLI_path_normalize(norm_root);
  if (!index->library_root.empty() && index->library_root != norm_root) {
    return nullptr;
  }

  const ArrayValue *entries_value = root.lookup_array(ATTR_ENTRIES);
  if (!entries_value) {
    return index;
  }

  for (const std::shared_ptr<Value> &element : entries_value->elements()) {
    if (element->type() != eValueType::Dictionary) {
      continue;
    }
    const DictionaryValue &entry_dict = *element->as_dictionary_value();
    ImageLibraryIndexEntry entry;

    if (const std::optional<StringRefNull> rel_path = entry_dict.lookup_str(ATTR_RELATIVE_PATH)) {
      entry.relative_path = rel_path->c_str();
    }
    if (const std::optional<int64_t> mtime = entry_dict.lookup_int(ATTR_MTIME)) {
      entry.mtime = *mtime;
    }
    if (const std::optional<int64_t> size = entry_dict.lookup_int(ATTR_SIZE)) {
      entry.size = *size;
    }
    if (const std::optional<StringRefNull> catalog_id_str = entry_dict.lookup_str(ATTR_CATALOG_ID))
    {
      bUUID parsed;
      if (BLI_uuid_parse_string(&parsed, catalog_id_str->c_str())) {
        entry.catalog_id = parsed;
      }
    }

    if (!entry.relative_path.empty()) {
      index->entries.append(entry);
    }
  }

  return index;
}

static bool image_library_index_write_atomic(const char *library_root_path,
                                             const ImageLibraryIndex &index)
{
  char index_path[FILE_MAX];
  char writing_path[FILE_MAX];
  char backup_path[FILE_MAX];

  image_library_index_filepath(library_root_path, index_path);
  BLI_path_join(
      writing_path, sizeof(writing_path), library_root_path, "blender_image_index.json.writing");
  BLI_path_join(backup_path, sizeof(backup_path), library_root_path, "blender_image_index.json~");

  DictionaryValue root;
  root.append_int(ATTR_VERSION, index.version);
  root.append_str(ATTR_LIBRARY_ROOT, index.library_root);

  auto entries_array = std::make_shared<ArrayValue>();
  for (const ImageLibraryIndexEntry &entry : index.entries) {
    DictionaryValue &entry_dict = *entries_array->append_dict();
    entry_dict.append_str(ATTR_RELATIVE_PATH, entry.relative_path);
    entry_dict.append_int(ATTR_MTIME, entry.mtime);
    entry_dict.append_int(ATTR_SIZE, entry.size);
    entry_dict.append_str(ATTR_CATALOG_ID, CatalogID(entry.catalog_id).str());
  }
  root.append(ATTR_ENTRIES, entries_array);

  if (!BLI_file_ensure_parent_dir_exists(writing_path)) {
    return false;
  }

  {
    JsonFormatter formatter;
    std::ostringstream out_stream;
    formatter.serialize(out_stream, root);
    const std::string json_content = out_stream.str();

    FILE *file = BLI_fopen(writing_path, "wb");
    if (!file) {
      return false;
    }
    const size_t written = fwrite(json_content.data(), 1, json_content.size(), file);
    fclose(file);
    if (written != json_content.size()) {
      return false;
    }
  }

  if (BLI_exists(index_path)) {
    if (BLI_rename_overwrite(index_path, backup_path) != 0) {
      return false;
    }
  }
  if (BLI_rename_overwrite(writing_path, index_path) != 0) {
    return false;
  }

  return true;
}

/* Bounds both pathologically deep trees and directory symlink/junction loops (stat follows
 * symlinks, so a loop would otherwise recurse until the stack overflows). */
static constexpr int IMAGE_LIBRARY_MAX_SCAN_DEPTH = 64;

static void scan_directory_recursive(const char *dir_abs,
                                     const char *rel_dir,
                                     std::set<std::string> &catalog_paths,
                                     Vector<ImageLibraryIndexEntry> &image_entries,
                                     const int depth)
{
  if (depth > IMAGE_LIBRARY_MAX_SCAN_DEPTH) {
    return;
  }

  direntry *entries = nullptr;
  const int entries_num = BLI_filelist_dir_contents(dir_abs, &entries);

  bool has_image = false;

  for (int i = 0; i < entries_num; i++) {
    const direntry &entry = entries[i];
    if (FILENAME_IS_CURRPAR(entry.relname)) {
      continue;
    }

    char sub_abs[FILE_MAX];
    BLI_path_join(sub_abs, sizeof(sub_abs), dir_abs, entry.relname);

    if (S_ISDIR(entry.type)) {
      char sub_rel[FILE_MAX];
      if (rel_dir[0]) {
        BLI_path_join(sub_rel, sizeof(sub_rel), rel_dir, entry.relname);
      }
      else {
        STRNCPY(sub_rel, entry.relname);
      }
      scan_directory_recursive(sub_abs, sub_rel, catalog_paths, image_entries, depth + 1);
      continue;
    }

    if (!image_library_is_image_filepath(sub_abs)) {
      continue;
    }

    has_image = true;

    char rel_path[FILE_MAX];
    if (rel_dir[0]) {
      BLI_path_join(rel_path, sizeof(rel_path), rel_dir, entry.relname);
    }
    else {
      STRNCPY(rel_path, entry.relname);
    }
    for (char &c : rel_path) {
      if (c == SEP) {
        c = '/';
      }
    }

    ImageLibraryIndexEntry index_entry;
    index_entry.relative_path = rel_path;
    index_entry.mtime = int64_t(entry.s.st_mtime);
    index_entry.size = int64_t(entry.s.st_size);
    image_entries.append(index_entry);
  }

  BLI_filelist_free(entries, entries_num);

  if (has_image) {
    if (rel_dir[0]) {
      std::string catalog_path_str(rel_dir);
      for (char &c : catalog_path_str) {
        if (c == SEP) {
          c = '/';
        }
      }
      catalog_paths.insert(catalog_path_str);
    }
    else {
      catalog_paths.insert(IMAGE_LIBRARY_ROOT_CATALOG_PATH);
    }
  }
}

/**
 * Resolve the user asset library that owns `library_root_path`.
 *
 * Unlike #BKE_preferences_asset_library_containing_path (which returns the *first* library whose
 * directory contains the path), this returns the most specific match -- the one with the longest
 * matching directory. That matters for nested libraries: a library added inside another library's
 * directory must resolve to itself, not to its enclosing parent, otherwise its type flags (image /
 * brush / remote) would be read from the wrong library and, for image libraries, its index would be
 * treated as stale and deleted. For the common non-nested case exactly one library matches, so the
 * result is identical to a first-match lookup.
 */
static const bUserAssetLibrary *image_library_owner_of_root(const char *library_root_path)
{
  const bUserAssetLibrary *best = nullptr;
  size_t best_len = 0;
  for (const bUserAssetLibrary &user_lib : U.asset_libraries) {
    if (!user_lib.dirpath[0] || !BLI_path_contains(user_lib.dirpath, library_root_path)) {
      continue;
    }
    const size_t len = BLI_strnlen(user_lib.dirpath, sizeof(user_lib.dirpath));
    if (len > best_len) {
      best = &user_lib;
      best_len = len;
    }
  }
  return best;
}

static asset_system::AssetLibrary *image_library_load_from_root(const char *library_root_path)
{
  const bUserAssetLibrary *user_lib = image_library_owner_of_root(library_root_path);
  if (!user_lib) {
    return nullptr;
  }
  if (user_lib->flag & ASSET_LIBRARY_USE_REMOTE_URL) {
    return nullptr;
  }
  const AssetLibraryReference lib_ref = user_library_to_library_ref(*user_lib);
  return AS_asset_library_load(G_MAIN, lib_ref);
}

static bool image_library_is_remote_root(const char *library_root_path)
{
  const bUserAssetLibrary *user_lib = image_library_owner_of_root(library_root_path);
  return user_lib && (user_lib->flag & ASSET_LIBRARY_USE_REMOTE_URL);
}

/**
 * True only for libraries created via "Add Image Library". Indexing is opt-in, not merely
 * opt-out for brush libraries: a "Local" library that happens to contain loose image files (e.g.
 * reference photos sitting next to unrelated assets) must not have them surface as texture assets
 * either. #filelist_readjob_ensure_image_library_indexed() would otherwise opportunistically index
 * *any* local library it reads a file list for, regardless of which UI triggered that read, so
 * this has to be enforced here rather than only at the UI-selector level.
 */
static bool image_library_root_is_image_library(const char *library_root_path)
{
  const bUserAssetLibrary *user_lib = image_library_owner_of_root(library_root_path);
  return user_lib && (user_lib->flag & ASSET_LIBRARY_IS_IMAGE_LIBRARY);
}

static bool image_library_is_editable_root(const char *library_root_path)
{
  if (!library_root_path || !library_root_path[0]) {
    return false;
  }
  if (!BLI_is_dir(library_root_path) || image_library_is_remote_root(library_root_path)) {
    return false;
  }
  return true;
}

static bool image_library_index_update_entry(const char *library_root_path,
                                             const char *old_relative_path,
                                             const char *new_relative_path,
                                             const bUUID &catalog_id)
{
  std::unique_ptr<ImageLibraryIndex> index = image_library_index_read(library_root_path);
  if (!index) {
    return false;
  }

  bool found = false;
  for (ImageLibraryIndexEntry &entry : index->entries) {
    if (entry.relative_path != old_relative_path) {
      continue;
    }
    entry.relative_path = new_relative_path;
    entry.catalog_id = catalog_id;

    char abs_path[FILE_MAX];
    join_library_root_and_relative(library_root_path, new_relative_path, abs_path);
    BLI_stat_t st;
    if (BLI_stat(abs_path, &st) == 0) {
      entry.mtime = int64_t(st.st_mtime);
      entry.size = int64_t(st.st_size);
    }
    found = true;
    break;
  }

  if (!found) {
    return false;
  }
  return image_library_index_write_atomic(library_root_path, *index);
}

static void image_library_invalidate_preview_at_path(const char *abs_path)
{
  BKE_previewimg_cached_release(abs_path);
  IMB_thumb_delete(abs_path, THB_LARGE);
  IMB_thumb_delete(abs_path, THB_NORMAL);
}

int image_library_scan_and_index(const char *library_root_path,
                                 asset_system::AssetLibrary *library)
{
  if (!library_root_path || !library_root_path[0]) {
    return -1;
  }
  if (!BLI_is_dir(library_root_path) || image_library_is_remote_root(library_root_path)) {
    return -1;
  }
  if (!image_library_root_is_image_library(library_root_path)) {
    /* Remove any stray index (e.g. from before the flag was cleared, or from before this
     * opt-in policy existed) so its images stop showing up as texture assets. */
    image_library_index_delete_if_exists(library_root_path);
    return 0;
  }

  std::set<std::string> catalog_paths;
  Vector<ImageLibraryIndexEntry> image_entries;
  scan_directory_recursive(library_root_path, "", catalog_paths, image_entries, 0);

  if (image_entries.is_empty()) {
    image_library_index_delete_if_exists(library_root_path);
    return 0;
  }

  if (!library) {
    library = image_library_load_from_root(library_root_path);
  }
  if (!library) {
    return -1;
  }

  asset_system::AssetCatalogService &catalog_service = library->catalog_service();
  for (const std::string &path_str : catalog_paths) {
    const AssetCatalogPath catalog_path(path_str);
    if (!catalog_service.find_catalog_by_path(catalog_path)) {
      library_ensure_catalogs_in_path(*library, catalog_path);
      if (AssetCatalog *cat = catalog_service.find_catalog_by_path(catalog_path)) {
        catalog_service.tag_has_unsaved_changes(cat);
      }
    }
  }

  for (ImageLibraryIndexEntry &entry : image_entries) {
    entry.catalog_id = catalog_id_for_image_relative_path(*library, entry.relative_path.c_str());
  }

  char norm_root[FILE_MAX];
  STRNCPY(norm_root, library_root_path);
  BLI_path_normalize(norm_root);

  ImageLibraryIndex index;
  index.library_root = norm_root;
  index.entries = std::move(image_entries);
  if (!image_library_index_write_atomic(library_root_path, index)) {
    return -1;
  }

  catalog_service.write_to_disk(library_root_path);

  BLI_assert(index.entries.size() <= INT_MAX);
  return int(index.entries.size());
}

bool image_library_needs_reindex(const char *library_root_path)
{
  if (!library_root_path || !library_root_path[0]) {
    return false;
  }
  if (!BLI_is_dir(library_root_path) || image_library_is_remote_root(library_root_path)) {
    return false;
  }
  if (!image_library_root_is_image_library(library_root_path)) {
    /* Only libraries explicitly added via "Add Image Library" are (re)indexed -- see
     * #image_library_scan_and_index(). Clean up any stray index here (not only in that function)
     * since callers skip calling it once this returns false, e.g. #image_library_on_startup() and
     * the Asset Browser's file-list read job, which previously indexed any local library
     * opportunistically before this opt-in policy existed. */
    image_library_index_delete_if_exists(library_root_path);
    return false;
  }

  std::unique_ptr<ImageLibraryIndex> index = image_library_index_read(library_root_path);
  if (!index) {
    return true;
  }

  Map<std::string, ImageLibraryIndexEntry> indexed_entries;
  for (const ImageLibraryIndexEntry &entry : index->entries) {
    indexed_entries.add(entry.relative_path, entry);
  }

  std::set<std::string> catalog_paths;
  Vector<ImageLibraryIndexEntry> disk_entries;
  scan_directory_recursive(library_root_path, "", catalog_paths, disk_entries, 0);

  if (disk_entries.size() != indexed_entries.size()) {
    return true;
  }

  for (const ImageLibraryIndexEntry &disk_entry : disk_entries) {
    const ImageLibraryIndexEntry *indexed = indexed_entries.lookup_ptr(disk_entry.relative_path);
    if (!indexed) {
      return true;
    }
    if (indexed->mtime != disk_entry.mtime || indexed->size != disk_entry.size) {
      return true;
    }
  }

  return false;
}

void image_library_notify_catalogs_changed(const bContext *C, const char *library_root_path)
{
  if (!C || !library_root_path || !library_root_path[0]) {
    return;
  }
  asset_system::all_library_tag_catalogs_dirty();
  if (bUserAssetLibrary *user_lib = BKE_preferences_asset_library_containing_path(
          &U, library_root_path))
  {
    refresh_asset_library(C, *user_lib);
  }
  WM_main_add_notifier(NC_ASSET | NA_EDITED, nullptr);
}

void image_library_on_library_added(const bContext *C, const char *library_root_path)
{
  if (!library_root_path || !library_root_path[0]) {
    return;
  }
  if (!BLI_is_dir(library_root_path) || image_library_is_remote_root(library_root_path)) {
    return;
  }
  if (!image_library_needs_reindex(library_root_path)) {
    return;
  }

  const int result = image_library_scan_and_index(library_root_path);
  if (result > 0) {
    image_library_notify_catalogs_changed(C, library_root_path);
  }
}

static bool image_library_invalidate_preview_callback(void * /*userdata*/,
                                                      const char *library_root,
                                                      const char *relative_image_path,
                                                      const char * /*image_name*/,
                                                      const bUUID & /*catalog_id*/)
{
  char full_path[FILE_MAX];
  BLI_path_join(full_path, sizeof(full_path), library_root, relative_image_path);
  image_library_invalidate_preview_at_path(full_path);
  return true;
}

void image_library_invalidate_cached_previews(const char *library_root_path)
{
  if (!library_root_path || !library_root_path[0]) {
    return;
  }
  image_library_foreach_image(
      library_root_path, image_library_invalidate_preview_callback, nullptr);
}

bool image_library_foreach_image(const char *library_root,
                                 ImageLibraryForeachCallback callback,
                                 void *userdata)
{
  if (!library_root || !library_root[0] || !callback) {
    return true;
  }
  if (image_library_is_remote_root(library_root)) {
    return true;
  }

  std::unique_ptr<ImageLibraryIndex> index = image_library_index_read(library_root);
  if (!index) {
    return true;
  }

  for (const ImageLibraryIndexEntry &entry : index->entries) {
    char image_name[FILE_MAX];
    BLI_path_split_file_part(entry.relative_path.c_str(), image_name, sizeof(image_name));

    if (!callback(
            userdata, library_root, entry.relative_path.c_str(), image_name, entry.catalog_id))
    {
      return false;
    }
  }

  return true;
}

void image_library_on_startup()
{
  /* Startup indexing is a synchronous recursive filesystem scan; skip it entirely in background
   * (`-b`) mode where there is no UI to serve and the cost is pure overhead. Interactive async
   * indexing is handled separately. */
  if (G.background) {
    return;
  }

  for (const bUserAssetLibrary &user_lib : U.asset_libraries) {
    if (user_lib.flag & (ASSET_LIBRARY_DISABLED | ASSET_LIBRARY_USE_REMOTE_URL)) {
      continue;
    }
    if (!user_lib.dirpath[0]) {
      continue;
    }
    if (!image_library_needs_reindex(user_lib.dirpath)) {
      continue;
    }
    image_library_scan_and_index(user_lib.dirpath);
  }
}

const char *image_library_editable_root_from_asset_library(const AssetLibrary &library)
{
  const std::optional<AssetLibraryReference> ref = library.library_reference();
  if (!ref || ref->type != ASSET_LIBRARY_CUSTOM) {
    return nullptr;
  }
  const bUserAssetLibrary *user_lib = BKE_preferences_asset_library_find_index(
      &U, ref->custom_library_index);
  if (!user_lib || (user_lib->flag & ASSET_LIBRARY_USE_REMOTE_URL) || !user_lib->dirpath[0]) {
    return nullptr;
  }
  if (!image_library_is_editable_root(user_lib->dirpath)) {
    return nullptr;
  }
  return user_lib->dirpath;
}

bool image_library_asset_is_movable_on_disk(const AssetRepresentation &asset)
{
  if (asset.local_id() || asset.get_id_type() != ID_IM) {
    return false;
  }
  const char *library_root = image_library_editable_root_from_asset_library(
      asset.owner_asset_library());
  if (!library_root) {
    return false;
  }
  const std::string full_path = asset.full_path();
  if (!BLI_path_contains(library_root, full_path.c_str())) {
    return false;
  }
  return image_library_is_image_filepath(full_path.c_str());
}

bool image_library_catalog_directory_ensure(const char *library_root_path,
                                            const StringRef catalog_path)
{
  if (!image_library_is_editable_root(library_root_path)) {
    return false;
  }

  const std::string rel_dir = relative_dir_from_catalog_path(catalog_path);
  if (rel_dir.empty()) {
    return true;
  }

  char abs_dir[FILE_MAX];
  join_library_root_and_relative(library_root_path, rel_dir, abs_dir);
  return BLI_dir_create_recursive(abs_dir);
}

static bool directory_is_empty(const char *dir_abs)
{
  direntry *entries = nullptr;
  const int entries_num = BLI_filelist_dir_contents(dir_abs, &entries);
  bool is_empty = true;
  for (int i = 0; i < entries_num; i++) {
    if (!FILENAME_IS_CURRPAR(entries[i].relname)) {
      is_empty = false;
      break;
    }
  }
  BLI_filelist_free(entries, entries_num);
  return is_empty;
}

bool image_library_catalog_directory_remove_if_empty(const char *library_root_path,
                                                      const StringRef catalog_path)
{
  if (!image_library_is_editable_root(library_root_path)) {
    return false;
  }

  const std::string rel_dir = relative_dir_from_catalog_path(catalog_path);
  if (rel_dir.empty()) {
    /* The "Root" pseudo-catalog maps to the library root itself; never remove that. */
    return false;
  }

  char abs_dir[FILE_MAX];
  join_library_root_and_relative(library_root_path, rel_dir, abs_dir);

  /* Containment guard: never touch anything outside the library root. */
  if (!BLI_path_contains(library_root_path, abs_dir)) {
    return false;
  }

  if (!BLI_exists(abs_dir)) {
    return true; /* Nothing to remove. */
  }
  if (!BLI_is_dir(abs_dir) || !directory_is_empty(abs_dir)) {
    /* Non-empty: the folder still holds image files moved into this catalog, so removing it
     * would destroy user data. Leave it in place. */
    return false;
  }

  return BLI_delete(abs_dir, true, false) == 0;
}

bool image_library_catalog_directory_relocate(const char *library_root_path,
                                              const StringRef old_catalog_path,
                                              const StringRef new_catalog_path)
{
  if (!image_library_is_editable_root(library_root_path)) {
    return false;
  }

  const std::string old_rel = relative_dir_from_catalog_path(old_catalog_path);
  const std::string new_rel = relative_dir_from_catalog_path(new_catalog_path);
  if (old_rel == new_rel) {
    return true;
  }
  if (old_rel.empty()) {
    return true;
  }

  char old_abs[FILE_MAX];
  char new_abs[FILE_MAX];
  join_library_root_and_relative(library_root_path, old_rel, old_abs);
  join_library_root_and_relative(library_root_path, new_rel, new_abs);

  if (BLI_exists(new_abs) && BLI_path_cmp(old_abs, new_abs) != 0) {
    return false;
  }

  if (!BLI_exists(old_abs)) {
    return BLI_dir_create_recursive(new_abs);
  }

  if (!BLI_file_ensure_parent_dir_exists(new_abs)) {
    return false;
  }

  return BLI_rename(old_abs, new_abs) == 0;
}

bool image_library_assign_image_to_catalog(const char *library_root_path,
                                           AssetLibrary &library,
                                           const StringRef relative_image_path,
                                           const bUUID &catalog_id)
{
  if (!image_library_is_editable_root(library_root_path)) {
    return false;
  }

  const AssetCatalog *catalog = library.catalog_service().find_catalog(catalog_id);
  if (!catalog) {
    return false;
  }

  const std::string target_rel_dir = relative_dir_from_catalog_path(catalog->path.c_str());

  char filename[FILE_MAX];
  BLI_path_split_file_part(relative_image_path.data(), filename, sizeof(filename));

  char new_relative[FILE_MAX];
  if (target_rel_dir.empty()) {
    STRNCPY(new_relative, filename);
  }
  else {
    BLI_path_join(new_relative, sizeof(new_relative), target_rel_dir.c_str(), filename);
  }
  for (char &c : new_relative) {
    if (c == SEP) {
      c = '/';
    }
  }

  if (relative_image_path == new_relative) {
    return image_library_index_update_entry(
        library_root_path, relative_image_path.data(), new_relative, catalog_id);
  }

  char old_abs[FILE_MAX];
  char new_abs[FILE_MAX];
  join_library_root_and_relative(library_root_path, relative_image_path, old_abs);
  join_library_root_and_relative(library_root_path, new_relative, new_abs);

  if (BLI_path_cmp(old_abs, new_abs) == 0) {
    return image_library_index_update_entry(
        library_root_path, relative_image_path.data(), new_relative, catalog_id);
  }

  if (BLI_exists(new_abs) && BLI_path_cmp(old_abs, new_abs) != 0) {
    char unique_name[FILE_MAX];
    BLI_uniquename_cb(
        [&](const StringRef check_name) {
          char test_relative[FILE_MAX];
          if (target_rel_dir.empty()) {
            BLI_strncpy(test_relative, check_name.data(), sizeof(test_relative));
          }
          else {
            BLI_path_join(
                test_relative, sizeof(test_relative), target_rel_dir.c_str(), check_name.data());
          }
          char test_abs[FILE_MAX];
          join_library_root_and_relative(library_root_path, test_relative, test_abs);
          return BLI_exists(test_abs);
        },
        filename,
        '.',
        unique_name,
        sizeof(unique_name));

    if (target_rel_dir.empty()) {
      STRNCPY(new_relative, unique_name);
    }
    else {
      BLI_path_join(new_relative, sizeof(new_relative), target_rel_dir.c_str(), unique_name);
    }
    for (char &c : new_relative) {
      if (c == SEP) {
        c = '/';
      }
    }
    join_library_root_and_relative(library_root_path, new_relative, new_abs);
  }

  if (!BLI_exists(old_abs)) {
    if (BLI_exists(new_abs)) {
      /* File was already moved (e.g. by a previous interrupted drop); only update the index. */
      return image_library_index_update_entry(
          library_root_path, relative_image_path.data(), new_relative, catalog_id);
    }
    return false;
  }

  if (!BLI_file_ensure_parent_dir_exists(new_abs)) {
    return false;
  }
  if (BLI_rename(old_abs, new_abs) != 0) {
    return false;
  }

  image_library_invalidate_preview_at_path(old_abs);
  image_library_invalidate_preview_at_path(new_abs);

  return image_library_index_update_entry(
      library_root_path, relative_image_path.data(), new_relative, catalog_id);
}

}  // namespace blender::ed::asset
