/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "BLI_enum_flags.hh"

#include "DNA_defs.h"
#include "DNA_listBase.h"
#include "DNA_uuid_types.h"

#include <memory>

namespace blender {

class StringRef;
namespace asset_system {
class AssetLibrary;
}  // namespace asset_system

enum eAssetLibraryType : short {
  /** Display assets from the current session (current "Main"). */
  ASSET_LIBRARY_LOCAL = 1,
  ASSET_LIBRARY_ALL = 2,
  /** Display assets bundled with Blender by default. */
  ASSET_LIBRARY_ESSENTIALS = 3,
  /** Additions to the essentials library that are stored online - displayed in the UI as part of
   * the normal essentials library. */
  ASSET_LIBRARY_ONLINE_ESSENTIALS = 4,

  /** Display assets from custom asset libraries, as defined in the preferences
   * (#bUserAssetLibrary). The name will be taken from #FileSelectParams.asset_library_ref.idname
   * then.
   * In RNA, we add the index of the custom library to this to identify it by index. So keep
   * this last! */
  ASSET_LIBRARY_CUSTOM = 100,
};

enum eAssetImportMethod : int {
  /** Regular data-block linking. */
  ASSET_IMPORT_LINK = 0,
  /** Regular data-block appending (basically linking + "Make Local"). */
  ASSET_IMPORT_APPEND = 1,
  /** Append data-block with the #BLO_LIBLINK_APPEND_LOCAL_ID_REUSE flag enabled. Some typically
   * heavy data dependencies (e.g. the image data-blocks of a material, the mesh of an object) may
   * be reused from an earlier append. */
  ASSET_IMPORT_APPEND_REUSE = 2,
  /** Link data-block, but also pack it as read-only data. */
  ASSET_IMPORT_PACK = 3,
};

enum eAssetLibrary_Flag : int {
  ASSET_LIBRARY_RELATIVE_PATH = (1 << 0),
  ASSET_LIBRARY_DISABLED = (1 << 1),
  ASSET_LIBRARY_USE_REMOTE_URL = (1 << 2),
  /** Set on libraries created via "Add Image Library": a folder of image files indexed as image
   * assets. Used to leave these out of UI surfaces that only ever show a different asset type
   * (e.g. the brush shelf), independent of what happens to be scanned into the library's on-disk
   * image index (see #image_library_scan_and_index, which -- for image-asset support -- indexes
   * any local library that has image files in it, not just ones added this way). */
  ASSET_LIBRARY_IS_IMAGE_LIBRARY = (1 << 3),
  /** Set on libraries created via "Add Brush Library": a library dedicated to brush assets. Used
   * to leave these out of Texture asset browsing (the image grid), so incidental image files
   * living alongside the brushes (e.g. stencil/alpha textures) never show up as texture assets. */
  ASSET_LIBRARY_IS_BRUSH_LIBRARY = (1 << 4),
  /** Set when the user pins this library, which shows it as a tab at the top of the asset shelf
   * popover. Distinct from the brush "favorites" feature in the same popover, which is about
   * individual brush assets, not libraries. */
  ASSET_LIBRARY_IS_PINNED = (1 << 5),
};

enum class AssetAccess : int8_t {
  OnlineAndOffline = 0,
  OnlyOnline = 1,
  OnlyOffline = 2,
};

/**
 * \brief User defined tag.
 * Currently only used by assets, could be used more often at some point.
 * Maybe add a custom icon and color to these in future?
 */
struct AssetTag {
  struct AssetTag *next = nullptr, *prev = nullptr;
  char name[/*MAX_NAME*/ 64] = "";
};

enum AssetMetaDataFlag : int {
  /**
   * When the import method is set to "Follow Asset or Preferences", use the asset's own import
   * method instead of the one from the library. Not used often, but for some assets there's a
   * specific preferred import method. For example, base mesh objects may always want to use
   * appending, so they can be edited directly and independently from previous usages.
   */
  ASSETDATA_USE_OWN_IMPORT_METHOD = (1 << 0),
};
ENUM_OPERATORS(AssetMetaDataFlag);

/**
 * \brief The meta-data of an asset.
 * By creating and giving this for a data-block (#ID.asset_data), the data-block becomes an asset.
 *
 * \note This struct must be readable without having to read anything but blocks from the ID it is
 *       attached to! That way, asset information of a file can be read, without reading anything
 *       more than that from the file. So pointers to other IDs or ID data are strictly forbidden.
 */
struct AssetMetaData {
  /** Runtime type, to reference event callbacks. Only valid for local assets. */
  struct AssetTypeInfo *local_type_info = nullptr;

  /** Custom asset meta-data. Cannot store pointers to IDs (#STRUCT_NO_DATABLOCK_IDPROPERTIES)! */
  struct IDProperty *properties = nullptr;

  /**
   * Asset Catalog identifier. Should not contain spaces.
   * Mapped to a path in the asset catalog hierarchy by an #AssetCatalogService.
   * Use #BKE_asset_metadata_catalog_id_set() to ensure a valid ID is set.
   */
  struct bUUID catalog_id;
  /**
   * Short name of the asset's catalog. This is for debugging purposes only, to allow (partial)
   * reconstruction of asset catalogs in the unfortunate case that the mapping from catalog UUID to
   * catalog path is lost. The catalog's simple name is copied to #catalog_simple_name whenever
   * #catalog_id is updated. */
  char catalog_simple_name[/*MAX_NAME*/ 64] = "";

  /** Optional name of the author for display in the UI. Dynamic length. */
  char *author = nullptr;

  /** Optional description of this asset for display in the UI. Dynamic length. */
  char *description = nullptr;

  /** Optional copyright of this asset for display in the UI. Dynamic length. */
  char *copyright = nullptr;

  /** Optional license of this asset for display in the UI. Dynamic length. */
  char *license = nullptr;

  /** User defined tags for this asset. The asset manager uses these for filtering, but how they
   * function exactly (e.g. how they are registered to provide a list of searchable available tags)
   * is up to the asset-engine. */
  ListBaseT<AssetTag> tags = {nullptr, nullptr};
  short active_tag = 0;
  /** Store the number of tags to avoid continuous counting. Could be turned into runtime data, we
   * can always reliably reconstruct it from the list. */
  short tot_tags = 0;

  AssetMetaDataFlag flag = {};

  /** The import method to use when "Follow Asset or Preferences" is used and
   * #AssetMetaDataFlag::ASSETDATA_USE_OWN_IMPORT_METHOD is set in the flags above. */
  eAssetImportMethod preferred_import_method = ASSET_IMPORT_APPEND;

  char _pad[4] = {};

#if defined(__cplusplus) && !defined(DNA_NO_EXTERNAL_CONSTRUCTORS)
  AssetMetaData() = default;
  AssetMetaData(const AssetMetaData &other);
  AssetMetaData(AssetMetaData &&other);
  /** Enables use with `std::unique_ptr<AssetMetaData>`. */
  ~AssetMetaData();
#endif
};

#
#
struct AssetImportSettings {
  eAssetImportMethod method = {};
  bool use_instance_collections = false;
};

/**
 * Information to identify an asset library. May be either one of the predefined types (current
 * 'Main', builtin library, project library), or a custom type as defined in the Preferences.
 *
 * If the type is set to #ASSET_LIBRARY_CUSTOM, `custom_library_name` identifies the custom
 * library. Otherwise neither of the custom members is used.
 */
struct AssetLibraryReference {
  eAssetLibraryType type = ASSET_LIBRARY_LOCAL;
  char _pad1[2] = {};
  /**
   * Runtime handle: the index of the #bUserAssetLibrary within #UserDef.asset_libraries. Derived
   * from #custom_library_name and kept in sync with it, so that Blender versions without the name
   * field still resolve this reference. Should be ignored for other types (but better set to -1
   * then, for sanity and debugging).
   *
   * \warning Never resolve this directly. An index is a *position*: it shifts whenever the
   * Preferences list is reordered or an entry removed, silently pointing at a different library.
   * Use #BKE_preferences_asset_library_find_from_ref().
   */
  int custom_library_index = -1;
  /**
   * Persistent identity of the custom library: the #bUserAssetLibrary.name, which is unique
   * (#BKE_preferences_asset_library_name_set uniquifies it). This is the truth on disk;
   * #custom_library_index is merely a cache of it.
   *
   * Empty means the reference was written before this field existed, and only
   * #custom_library_index can resolve it.
   */
  char custom_library_name[/*MAX_NAME*/ 64] = "";
};

/**
 * Information to refer to an asset (may be stored in files) on a "best effort" basis. It should
 * work well enough for many common cases, but can break. For example when the location of the
 * asset changes, the available asset libraries in the Preferences change, an asset library is
 * renamed, or when a file storing this is opened on a different system (with different
 * Preferences).
 *
 * It has two main components:
 * - A reference to the asset library: The #eAssetLibraryType and if that is not enough to identify
 *   the library, a library name (typically given by the user, but may change).
 * - An identifier for the asset within the library: A relative path currently, which can break if
 *   the asset is moved. Could also be a unique key for a database for example.
 *
 * \note Needs freeing through the destructor, so either use a smart pointer or #MEM_delete() for
 *       explicit freeing.
 */
struct AssetWeakReference {
  char _pad[6] = {};

  eAssetLibraryType asset_library_type = {};
  /** If #asset_library_type is not enough to identify the asset library, this string can provide
   * further location info (allocated string). Null otherwise. */
  const char *asset_library_identifier = nullptr;

  const char *relative_asset_identifier = nullptr;

#if defined(__cplusplus) && !defined(DNA_NO_EXTERNAL_CONSTRUCTORS)
  AssetWeakReference();
  AssetWeakReference(const AssetWeakReference &);
  AssetWeakReference(AssetWeakReference &&);
  AssetWeakReference &operator=(const AssetWeakReference &);
  AssetWeakReference &operator=(AssetWeakReference &&);
  ~AssetWeakReference();

  friend bool operator==(const AssetWeakReference &a, const AssetWeakReference &b);
  friend bool operator!=(const AssetWeakReference &a, const AssetWeakReference &b)
  {
    return !(a == b);
  }

  /**
   * See AssetRepresentation::make_weak_reference().
   */
  static AssetWeakReference make_reference(const asset_system::AssetLibrary &library,
                                           StringRef library_relative_identifier);
#endif
};

struct AssetCatalogPathLink {
  struct AssetCatalogPathLink *next = nullptr, *prev = nullptr;
  char *path = nullptr;
};

/**
 * Active Name Matching map-type identifier selected on an #AssetShelfSettings
 * (Preferences map type #bUserNameMatchMapType.identifier).
 */
struct AssetNameMatchIdLink {
  struct AssetNameMatchIdLink *next = nullptr, *prev = nullptr;
  char id[/*MAX_NAME*/ 64] = "";
};

/**
 * Active Name Matching filter tag selected on an #AssetShelfSettings.
 */
struct AssetNameMatchTagLink {
  struct AssetNameMatchTagLink *next = nullptr, *prev = nullptr;
  char name[/*MAX_NAME*/ 64] = "";
};

/**
 * Persistent collapsed/expanded state of a single asset catalog path in a tree view.
 * Kept separate from #AssetCatalogPathLink so existing enabled-catalog lists and their on-disk
 * format are untouched.
 */
struct AssetCatalogState {
  struct AssetCatalogState *next = nullptr, *prev = nullptr;

  /** Full catalog path. */
  char *path = nullptr;

  /** Time when this entry was last used (seconds since epoch). Used to drop stale entries. */
  uint32_t last_used = 0;

  char is_collapsed = false;
  char _pad[3] = {};
};

}  // namespace blender
