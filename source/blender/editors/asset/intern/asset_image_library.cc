/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <climits>
#include <fstream>
#include <optional>
#include <set>
#include <string>

#include "BLI_fileops.h"
#include "BLI_map.hh"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"
#ifdef WIN32
#  include "BLI_winstuff.h"
#endif

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_preferences.h"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"

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
  const std::string catalog_path_str = catalog_path_from_image_relative_path(
      relative_image_path);
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

  JsonFormatter formatter;
  std::ifstream stream(index_path);
  if (!stream.is_open()) {
    return nullptr;
  }

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
    if (const std::optional<StringRefNull> catalog_id_str = entry_dict.lookup_str(
            ATTR_CATALOG_ID))
    {
      entry.catalog_id = CatalogID(*catalog_id_str);
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
  BLI_path_join(writing_path, sizeof(writing_path), library_root_path, "blender_image_index.json.writing");
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
    std::ofstream stream(writing_path, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
      return false;
    }
    formatter.serialize(stream, root);
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

static void scan_directory_recursive(const char *dir_abs,
                                     const char *rel_dir,
                                     std::set<std::string> &catalog_paths,
                                     Vector<ImageLibraryIndexEntry> &image_entries)
{
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
      scan_directory_recursive(sub_abs, sub_rel, catalog_paths, image_entries);
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

static asset_system::AssetLibrary *image_library_load_from_root(const char *library_root_path)
{
  const bUserAssetLibrary *user_lib = BKE_preferences_asset_library_containing_path(
      &U, library_root_path);
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
  const bUserAssetLibrary *user_lib = BKE_preferences_asset_library_containing_path(
      &U, library_root_path);
  return user_lib && (user_lib->flag & ASSET_LIBRARY_USE_REMOTE_URL);
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

  std::set<std::string> catalog_paths;
  Vector<ImageLibraryIndexEntry> image_entries;
  scan_directory_recursive(library_root_path, "", catalog_paths, image_entries);

  if (image_entries.is_empty()) {
    char index_path[FILE_MAX];
    image_library_index_filepath(library_root_path, index_path);
    if (BLI_exists(index_path)) {
      BLI_delete(index_path, false, false);
    }
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
  scan_directory_recursive(library_root_path, "", catalog_paths, disk_entries);

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

    if (!callback(userdata,
                  library_root,
                  entry.relative_path.c_str(),
                  image_name,
                  entry.catalog_id))
    {
      return false;
    }
  }

  return true;
}

}  // namespace blender::ed::asset
