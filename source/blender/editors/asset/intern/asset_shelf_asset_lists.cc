/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "asset_shelf_asset_lists.hh"

#include <algorithm>
#include <cstddef>

#include "BLI_fileops.h"
#include "BLI_hash.hh"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_string.h"

#include "BKE_appdir.hh"
#include "BKE_main.hh"
#include "BKE_paint_types.hh"
#include "BKE_preferences.h"

#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "ED_view3d.hh"

namespace blender::ed::asset::shelf {

ShelfAssetRef ShelfAssetRef::from_weak_reference(const AssetWeakReference &weak_ref)
{
  ShelfAssetRef ref;
  ref.library_type = weak_ref.asset_library_type;
  ref.library_identifier = weak_ref.asset_library_identifier ? weak_ref.asset_library_identifier :
                                                               std::string();
  ref.relative_identifier = weak_ref.relative_asset_identifier ?
                                weak_ref.relative_asset_identifier :
                                std::string();
  if (ref.library_type == ASSET_LIBRARY_LOCAL) {
    /* A local asset is only addressed by its ID name, which is unique within one .blend file but
     * not across files. Scope it to the file it came from, so its lists entries don't leak into
     * other files (see #ShelfAssetRef::blend_filepath). The global #Main is what the asset system
     * itself resolves "Current File" assets against. */
    ref.blend_filepath = BKE_main_blendfile_path_from_global();
  }
  return ref;
}

uint64_t ShelfAssetRef::hash() const
{
  /* Deliberately coarse: #operator== compares the identifier strings with #BLI_path_cmp_normalized,
   * which can treat byte-different strings as equal (native slash direction, case on Windows).
   * Hashing the raw strings would then break the "equal keys hash equally" invariant that
   * #Set/#Map/#VectorSet rely on, so hash only on #library_type -- the one field #operator==
   * compares exactly. Two equal refs always share it. The recent/favorite lists are tiny (recent is
   * capped at #SHELF_ASSET_LISTS_RECENT_MAX), so the extra bucket collisions are irrelevant. */
  return get_default_hash(library_type);
}

bool ShelfAssetRef::operator==(const ShelfAssetRef &other) const
{
  /* Mirror #AssetWeakReference::operator== (see `asset_weak_reference.cc`) so this cache agrees with
   * the asset system on when two references identify the same asset: #library_identifier only
   * participates for custom libraries, and the (asset-library-relative) identifiers are compared
   * path-normalized rather than byte-exact. #blend_filepath is this module's own addition that
   * scopes local ("Current File") assets to the .blend file they came from. */
  if (library_type != other.library_type) {
    return false;
  }
  if (library_type == ASSET_LIBRARY_CUSTOM &&
      BLI_path_cmp_normalized(library_identifier.c_str(), other.library_identifier.c_str()) != 0)
  {
    return false;
  }
  if (BLI_path_cmp_normalized(relative_identifier.c_str(), other.relative_identifier.c_str()) != 0) {
    return false;
  }
  return BLI_path_cmp_normalized(blend_filepath.c_str(), other.blend_filepath.c_str()) == 0;
}

void record_recent_into(Vector<ShelfAssetRef> &recent, ShelfAssetRef ref, const int max_count)
{
  recent.remove_if([&](const ShelfAssetRef &existing) { return existing == ref; });
  recent.insert(0, std::move(ref));
  /* Guard the (public, test-visible) \a max_count: a negative value would make #Vector::resize hit
   * its `new_size >= 0` assert and crash a debug build. */
  if (max_count >= 0 && recent.size() > max_count) {
    recent.resize(max_count);
  }
}

void toggle_favorite_into(VectorSet<ShelfAssetRef> &favorites, const ShelfAssetRef &ref)
{
  if (favorites.contains(ref)) {
    favorites.remove_contained(ref);
  }
  else {
    favorites.add(ref);
  }
}

bool reorder_favorite_into(VectorSet<ShelfAssetRef> &favorites,
                           const ShelfAssetRef &ref,
                           const int new_index)
{
  const int64_t current = favorites.index_of_try(ref);
  if (current < 0) {
    return false;
  }
  const int count = int(favorites.size());
  const int target = std::clamp(new_index, 0, count - 1);
  if (target == current) {
    return false;
  }
  Vector<ShelfAssetRef> order(favorites.as_span());
  ShelfAssetRef moved = std::move(order[current]);
  order.remove(current);
  order.insert(target, std::move(moved));
  favorites.clear();
  favorites.add_multiple(order);
  return true;
}

int favorite_index_from_identifier(const Span<ShelfAssetRef> favorites,
                                   const StringRefNull target_identifier)
{
  for (const int i : favorites.index_range()) {
    if (favorites[i].relative_identifier == target_identifier) {
      return i;
    }
  }
  return -1;
}

static std::shared_ptr<io::serialize::DictionaryValue> shelf_asset_ref_to_json(
    const ShelfAssetRef &ref)
{
  auto dict = std::make_shared<io::serialize::DictionaryValue>();
  dict->append_int("asset_library_type", int64_t(ref.library_type));
  dict->append_str("asset_library_identifier", ref.library_identifier);
  dict->append_str("relative_identifier", ref.relative_identifier);
  dict->append_str("blend_filepath", ref.blend_filepath);
  return dict;
}

/** JSON key `relative_identifier` deliberately does not match the DNA field name
 * `relative_asset_identifier` -- kept short in the file format; #ShelfAssetRef::from_weak_reference()
 * and this pair of functions are the only place the two names need to line up. */
static std::optional<ShelfAssetRef> shelf_asset_ref_from_json(
    const io::serialize::DictionaryValue &dict)
{
  const std::optional<int64_t> library_type = dict.lookup_int("asset_library_type");
  const std::optional<StringRefNull> relative_identifier = dict.lookup_str(
      "relative_identifier");
  if (!library_type || !relative_identifier) {
    return std::nullopt;
  }
  ShelfAssetRef ref;
  ref.library_type = eAssetLibraryType(*library_type);
  if (const std::optional<StringRefNull> library_identifier = dict.lookup_str(
          "asset_library_identifier"))
  {
    ref.library_identifier = std::string(*library_identifier);
  }
  ref.relative_identifier = std::string(*relative_identifier);
  /* Optional: a file written before #ShelfAssetRef::blend_filepath existed has no such key, and
   * must still load (as an entry that belongs to no particular .blend file). */
  if (const std::optional<StringRefNull> blend_filepath = dict.lookup_str("blend_filepath")) {
    ref.blend_filepath = std::string(*blend_filepath);
  }
  return ref;
}

std::shared_ptr<io::serialize::DictionaryValue> lists_to_json(
    const Map<std::string, ShelfAssetLists> &shelves)
{
  auto root = std::make_shared<io::serialize::DictionaryValue>();
  root->append_int("version", 1);
  const std::shared_ptr<io::serialize::DictionaryValue> shelves_dict = root->append_dict(
      "shelves");
  for (const auto item : shelves.items()) {
    const std::shared_ptr<io::serialize::DictionaryValue> shelf_dict = shelves_dict->append_dict(
        item.key);
    const std::shared_ptr<io::serialize::ArrayValue> recent_array = shelf_dict->append_array(
        "recent");
    for (const ShelfAssetRef &ref : item.value.recent) {
      recent_array->append(shelf_asset_ref_to_json(ref));
    }
    const std::shared_ptr<io::serialize::ArrayValue> favorites_array = shelf_dict->append_array(
        "favorites");
    for (const ShelfAssetRef &ref : item.value.favorites) {
      favorites_array->append(shelf_asset_ref_to_json(ref));
    }
  }
  return root;
}

Map<std::string, ShelfAssetLists> lists_from_json(const io::serialize::Value &root)
{
  Map<std::string, ShelfAssetLists> shelves;

  const io::serialize::DictionaryValue *root_dict = root.as_dictionary_value();
  if (!root_dict) {
    return shelves;
  }
  const io::serialize::DictionaryValue *shelves_dict = root_dict->lookup_dict("shelves");
  if (!shelves_dict) {
    return shelves;
  }

  for (const auto &shelf_item : shelves_dict->elements()) {
    const io::serialize::DictionaryValue *shelf_dict = shelf_item.second->as_dictionary_value();
    if (!shelf_dict) {
      continue;
    }
    ShelfAssetLists lists;
    if (const io::serialize::ArrayValue *recent_array = shelf_dict->lookup_array("recent")) {
      for (const std::shared_ptr<io::serialize::Value> &entry : recent_array->elements()) {
        if (const io::serialize::DictionaryValue *entry_dict = entry->as_dictionary_value()) {
          if (std::optional<ShelfAssetRef> ref = shelf_asset_ref_from_json(*entry_dict)) {
            lists.recent.append(std::move(*ref));
          }
        }
      }
    }
    if (const io::serialize::ArrayValue *favorites_array = shelf_dict->lookup_array("favorites"))
    {
      for (const std::shared_ptr<io::serialize::Value> &entry : favorites_array->elements()) {
        if (const io::serialize::DictionaryValue *entry_dict = entry->as_dictionary_value()) {
          if (std::optional<ShelfAssetRef> ref = shelf_asset_ref_from_json(*entry_dict)) {
            lists.favorites.add(std::move(*ref));
          }
        }
      }
    }
    shelves.add(shelf_item.first, std::move(lists));
  }

  return shelves;
}

/**
 * Index matches the underlying value of #PaintMode. Kept in this module (rather than a C-side
 * table on #AssetShelfType, which has no such field) because it is the only place that needs a
 * mode <-> shelf-idname mapping; see the design doc's open-questions audit.
 */
static const char *brush_shelf_idname_table[] = {
    "VIEW3D_AST_brush_sculpt",         /* PaintMode::Sculpt */
    "VIEW3D_AST_brush_vertex_paint",   /* PaintMode::Vertex */
    "VIEW3D_AST_brush_weight_paint",   /* PaintMode::Weight */
    "VIEW3D_AST_brush_texture_paint",  /* PaintMode::Texture3D */
    "IMAGE_AST_brush_paint",           /* PaintMode::Texture2D */
    nullptr,                           /* Unused PaintMode value (5). */
    "VIEW3D_AST_brush_gpencil_paint",  /* PaintMode::GPencil */
    "VIEW3D_AST_brush_gpencil_vertex", /* PaintMode::VertexGPencil */
    "VIEW3D_AST_brush_gpencil_sculpt", /* PaintMode::SculptGPencil */
    "VIEW3D_AST_brush_gpencil_weight", /* PaintMode::WeightGPencil */
    "VIEW3D_AST_brush_sculpt_curves",  /* PaintMode::SculptCurves */
};

const char *brush_shelf_idname_from_paint_mode(const PaintMode mode)
{
  const int index = int(mode);
  if (index < 0 || index >= int(std::size(brush_shelf_idname_table))) {
    return nullptr;
  }
  return brush_shelf_idname_table[index];
}

bool shelf_idname_is_brush_shelf(const StringRef idname)
{
  for (const char *candidate : brush_shelf_idname_table) {
    if (candidate && idname == candidate) {
      return true;
    }
  }
  return false;
}

bool shelf_supports_asset_lists(const StringRef idname)
{
  return shelf_idname_is_brush_shelf(idname) || idname == ed::view3d::IMAGE_TEXTURE_SHELF_IDNAME;
}

namespace {

/** Process-local D7 stamp: temporary popup region → shelf type idname. */
Map<const ARegion *, std::string> &popup_region_shelf_idnames()
{
  static Map<const ARegion *, std::string> map;
  return map;
}

}  // namespace

void shelf_popup_region_bind(const ARegion &region, const StringRef shelf_idname)
{
  popup_region_shelf_idnames().add_overwrite(&region, std::string(shelf_idname));
}

void shelf_popup_region_unbind(const ARegion &region)
{
  popup_region_shelf_idnames().remove(&region);
}

std::optional<StringRefNull> shelf_popup_region_idname_get(const ARegion &region)
{
  if (const std::string *idname = popup_region_shelf_idnames().lookup_ptr(&region)) {
    return StringRefNull(*idname);
  }
  return std::nullopt;
}

#define SHELF_ASSET_LISTS_FILENAME "shelf_asset_lists.json"

namespace {
struct ShelfAssetListsCache {
  bool loaded = false;
  /** Set when the in-memory lists changed but haven't been written to disk yet. Flushed by
   * #shelf_asset_lists_flush() at exit; see #shelf_asset_lists_record_recent() for why recent isn't saved
   * eagerly. */
  bool dirty = false;
  Map<std::string, ShelfAssetLists> shelves;
};
}  // namespace

static void cache_load(Map<std::string, ShelfAssetLists> &shelves)
{
  const std::optional<std::string> cfgdir = BKE_appdir_folder_id(BLENDER_USER_CONFIG, nullptr);
  if (!cfgdir.has_value()) {
    return;
  }
  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), cfgdir->c_str(), SHELF_ASSET_LISTS_FILENAME);
  if (!BLI_exists(filepath)) {
    return;
  }
  const std::shared_ptr<io::serialize::Value> root = io::serialize::read_json_file(filepath);
  if (!root) {
    return;
  }
  shelves = lists_from_json(*root);
}

/** Writes through a temporary file + rename-overwrite so a crash mid-write cannot corrupt the
 * previous, valid contents. */
static void cache_save(const Map<std::string, ShelfAssetLists> &shelves)
{
  const std::optional<std::string> cfgdir = BKE_appdir_folder_id_create(BLENDER_USER_CONFIG,
                                                                         nullptr);
  if (!cfgdir.has_value()) {
    return;
  }
  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), cfgdir->c_str(), SHELF_ASSET_LISTS_FILENAME);
  char tmp_filepath[FILE_MAX];
  BLI_snprintf(tmp_filepath, sizeof(tmp_filepath), "%s.tmp", filepath);

  const std::shared_ptr<io::serialize::DictionaryValue> root = lists_to_json(shelves);
  io::serialize::write_json_file(tmp_filepath, *root);
  BLI_rename_overwrite(tmp_filepath, filepath);
}

static ShelfAssetListsCache &cache_instance()
{
  static ShelfAssetListsCache cache;
  return cache;
}

static ShelfAssetListsCache &cache_get()
{
  ShelfAssetListsCache &cache = cache_instance();
  if (!cache.loaded) {
    cache_load(cache.shelves);
    cache.loaded = true;
  }
  return cache;
}

int shelf_asset_lists_recent_max_count_get(const StringRef shelf_idname)
{
  const bUserAssetShelfSettings *settings = BKE_preferences_asset_shelf_settings_get(
      &U, std::string(shelf_idname).c_str());
  if (settings && (settings->popup_view_flag & USER_ASSET_SHELF_POPUP_VIEW_STORED) &&
      settings->recent_max_count > 0)
  {
    return settings->recent_max_count;
  }
  return SHELF_ASSET_LISTS_RECENT_MAX;
}

void shelf_asset_lists_recent_max_count_set(const StringRef shelf_idname, const int recent_max_count)
{
  const int max_count = std::max(1, recent_max_count);

  ShelfAssetListsCache &cache = cache_get();
  ShelfAssetLists &lists = cache.shelves.lookup_or_add_default(std::string(shelf_idname));
  if (lists.recent.size() > max_count) {
    lists.recent.resize(max_count);
  }

  /* Persist immediately so the list on disk never exceeds the new limit. */
  cache_save(cache.shelves);
  cache.dirty = false;
}

void shelf_asset_lists_record_recent(const StringRef shelf_idname, const AssetWeakReference &weak_ref)
{
  ShelfAssetListsCache &cache = cache_get();
  ShelfAssetLists &lists = cache.shelves.lookup_or_add_default(std::string(shelf_idname));
  record_recent_into(lists.recent,
                     ShelfAssetRef::from_weak_reference(weak_ref),
                     shelf_asset_lists_recent_max_count_get(shelf_idname));
  /* Persist lazily rather than here: brush activation is a hot path (cycling brushes with a hotkey),
   * and a synchronous serialize + temp-file rename on every activation is a noticeable stall on slow
   * disks. The change is flushed by #shelf_asset_lists_flush() at exit, and opportunistically whenever a
   * favorite is toggled. Losing the tail of "recent" on a hard crash is acceptable for that list. */
  cache.dirty = true;
}

bool shelf_asset_lists_is_favorite(const StringRef shelf_idname, const AssetWeakReference &weak_ref)
{
  ShelfAssetListsCache &cache = cache_get();
  const ShelfAssetLists *lists = cache.shelves.lookup_ptr(std::string(shelf_idname));
  if (!lists) {
    return false;
  }
  return lists->favorites.contains(ShelfAssetRef::from_weak_reference(weak_ref));
}

void shelf_asset_lists_toggle_favorite(const StringRef shelf_idname, const AssetWeakReference &weak_ref)
{
  ShelfAssetListsCache &cache = cache_get();
  ShelfAssetLists &lists = cache.shelves.lookup_or_add_default(std::string(shelf_idname));
  toggle_favorite_into(lists.favorites, ShelfAssetRef::from_weak_reference(weak_ref));
  /* Favorites are deliberately curated (not spammed like recent), so persist them immediately to
   * keep them crash-safe. This write also flushes any recent changes that were only marked dirty. */
  cache_save(cache.shelves);
  cache.dirty = false;
}

void shelf_asset_lists_reorder_favorite(const StringRef shelf_idname,
                                  const AssetWeakReference &weak_ref,
                                  const int new_index)
{
  ShelfAssetListsCache &cache = cache_get();
  ShelfAssetLists &lists = cache.shelves.lookup_or_add_default(std::string(shelf_idname));
  if (!reorder_favorite_into(lists.favorites,
                             ShelfAssetRef::from_weak_reference(weak_ref),
                             new_index))
  {
    return;
  }
  cache_save(cache.shelves);
  cache.dirty = false;
}

void shelf_asset_lists_clear_recent(const StringRef shelf_idname)
{
  ShelfAssetListsCache &cache = cache_get();
  ShelfAssetLists *lists = cache.shelves.lookup_ptr(std::string(shelf_idname));
  if (!lists || lists->recent.is_empty()) {
    return;
  }
  lists->recent.clear();
  /* An explicit, infrequent user action (unlike #shelf_asset_lists_record_recent's hot path), so persist
   * immediately instead of only marking dirty. */
  cache_save(cache.shelves);
  cache.dirty = false;
}

void shelf_asset_lists_clear_favorites(const StringRef shelf_idname)
{
  ShelfAssetListsCache &cache = cache_get();
  ShelfAssetLists *lists = cache.shelves.lookup_ptr(std::string(shelf_idname));
  if (!lists || lists->favorites.is_empty()) {
    return;
  }
  lists->favorites.clear();
  cache_save(cache.shelves);
  cache.dirty = false;
}

void shelf_asset_lists_flush()
{
  /* Use #cache_instance() (not #cache_get()) so a session that never touched the lists doesn't read
   * the file from disk just to write it back unchanged at exit. */
  ShelfAssetListsCache &cache = cache_instance();
  if (!cache.loaded || !cache.dirty) {
    return;
  }
  cache_save(cache.shelves);
  cache.dirty = false;
}

Span<ShelfAssetRef> shelf_asset_lists_recent(const StringRef shelf_idname)
{
  ShelfAssetListsCache &cache = cache_get();
  const ShelfAssetLists *lists = cache.shelves.lookup_ptr(std::string(shelf_idname));
  return lists ? Span<ShelfAssetRef>(lists->recent) : Span<ShelfAssetRef>();
}

Span<ShelfAssetRef> shelf_asset_lists_favorites(const StringRef shelf_idname)
{
  ShelfAssetListsCache &cache = cache_get();
  const ShelfAssetLists *lists = cache.shelves.lookup_ptr(std::string(shelf_idname));
  return lists ? lists->favorites.as_span() : Span<ShelfAssetRef>();
}

}  // namespace blender::ed::asset::shelf
