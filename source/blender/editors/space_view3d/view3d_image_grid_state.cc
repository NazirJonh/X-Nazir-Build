/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_image_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.h"
#include "BLI_set.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BKE_asset.hh"
#include "BKE_asset_catalog_memory.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_main.hh"
#include "BKE_name_matching.hh"
#include "BKE_preferences.h"

#include "MEM_guardedalloc.h"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_image_grid.hh"
#include "ED_view3d.hh"

#include "intern/asset_shelf_asset_lists.hh"

#include "UI_grid_view.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

static_assert(int(ImageGridShelfCatalogMode::All) == IMAGE_GRID_CATALOG_MODE_ALL);
static_assert(int(ImageGridShelfCatalogMode::CatalogPath) == IMAGE_GRID_CATALOG_MODE_CATALOG_PATH);
static_assert(int(ImageGridShelfCatalogMode::Recent) == IMAGE_GRID_CATALOG_MODE_RECENT);
static_assert(int(ImageGridShelfCatalogMode::Favorites) == IMAGE_GRID_CATALOG_MODE_FAVORITES);

/** Domain key for #bUserAssetCatalogMemory entries owned by the View3D Image Grid. */
constexpr const char *image_grid_catalog_memory_domain = "image_grid";

static Vector<bUUID> image_grid_paths_to_catalog_ids(const AssetLibraryReference &lib_ref,
                                                     const Set<std::string> &paths)
{
  Vector<bUUID> ids;
  if (paths.is_empty()) {
    return ids;
  }
  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  if (!library) {
    return ids;
  }
  for (const std::string &path : paths) {
    if (asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog_by_path(
            path.c_str()))
    {
      ids.append(catalog->catalog_id);
    }
  }
  return ids;
}

static Set<std::string> image_grid_catalog_ids_to_paths(const AssetLibraryReference &lib_ref,
                                                       const Span<bUUID> ids)
{
  Set<std::string> paths;
  if (ids.is_empty()) {
    return paths;
  }
  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  if (!library) {
    return paths;
  }
  for (const bUUID &catalog_id : ids) {
    if (const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog(
            asset_system::CatalogID(catalog_id)))
    {
      paths.add(catalog->path.str());
    }
  }
  return paths;
}

static Set<std::string> image_grid_remembered_catalog_paths(const AssetLibraryReference &lib_ref)
{
  if (BKE_asset_catalog_memory_get_mode(&U, lib_ref, image_grid_catalog_memory_domain) !=
      ASSET_CATALOG_MEMORY_SET)
  {
    return {};
  }
  return image_grid_catalog_ids_to_paths(
      lib_ref,
      BKE_asset_catalog_memory_get_set(&U, lib_ref, image_grid_catalog_memory_domain));
}

std::string image_grid_library_key(const AssetLibraryReference &lib_ref)
{
  return BKE_preferences_asset_library_identifier_from_ref(&U, &lib_ref);
}

blender::Vector<asset_system::AssetLibrary *> image_grid_all_mode_libraries()
{
  /* Same flags as #rna_image_grid_library_itemf / the library selector menu. */
  return ed::asset::all_mode_libraries(/*exclude_image_libraries=*/false,
                                       /*only_image_libraries=*/true);
}

void image_grid_fetch_all_mode_libraries(const bContext &C)
{
  /* Same flags as #image_grid_all_mode_libraries() / #rna_image_grid_library_itemf. */
  ed::asset::fetch_all_mode_libraries(
      C, /*exclude_image_libraries=*/false, /*only_image_libraries=*/true);
}

/* Reverse of #image_grid_library_key(): reconstruct the reference an identifier names.
 * Returns a default-constructed (#ASSET_LIBRARY_LOCAL) reference when a custom library's name
 * doesn't resolve -- the caller must check for that case using the key it looked up with, since
 * "local" legitimately produces the same default value. */
AssetLibraryReference image_grid_library_ref_from_key(const std::string &key)
{
  if (key == "local") {
    return asset_system::current_file_library_reference();
  }
  if (key == "all") {
    return asset_system::all_library_reference();
  }
  if (key == "essentials") {
    AssetLibraryReference ref{};
    ref.type = ASSET_LIBRARY_ESSENTIALS;
    return ref;
  }
  if (key == "online_essentials") {
    AssetLibraryReference ref{};
    ref.type = ASSET_LIBRARY_ONLINE_ESSENTIALS;
    return ref;
  }

  AssetLibraryReference ref{};
  if (const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_by_name(
          &U, key.c_str()))
  {
    BKE_preferences_asset_library_reference_set(&U, &ref, user_library);
  }
  return ref;
}

static void image_grid_catalog_load_active(ImageGridUIState &state,
                                           const AssetLibraryReference &lib_ref)
{
  state.filter.enabled_catalog_paths.clear();
  state.filter.enabled_catalog_paths = image_grid_remembered_catalog_paths(lib_ref);
}

void image_grid_catalog_commit_active(ImageGridUIState &state)
{
  if (state.filter.enabled_catalog_paths.is_empty()) {
    BKE_asset_catalog_memory_set_all(&U, state.filter.lib_ref, image_grid_catalog_memory_domain);
    return;
  }
  const Vector<bUUID> ids = image_grid_paths_to_catalog_ids(state.filter.lib_ref,
                                                            state.filter.enabled_catalog_paths);
  if (ids.is_empty()) {
    /* Library still loading or paths invalid — do not clobber a previously saved set. */
    return;
  }
  BKE_asset_catalog_memory_set_set(
      &U, state.filter.lib_ref, image_grid_catalog_memory_domain, ids.as_span());
}

void image_grid_filter_set_show_all(ImageGridUIState &state)
{
  const bool leaving_membership =
      state.filter.catalog_mode == ImageGridShelfCatalogMode::Recent ||
      state.filter.catalog_mode == ImageGridShelfCatalogMode::Favorites;
  if (leaving_membership) {
    /* Restore the already-current #lib_ref's (typically #ASSET_LIBRARY_ALL) saved catalog filter
     * instead of force-clearing it, so the catalog tree's "All" item leaves membership the same
     * way #image_grid_catalog_swap_library does when the library menu re-selects that library.
     * Never commits here -- committing empty paths would remove the saved filter for #lib_ref. */
    image_grid_catalog_load_active(state, state.filter.lib_ref);
    state.filter.catalog_mode = state.filter.enabled_catalog_paths.is_empty() ?
                                    ImageGridShelfCatalogMode::All :
                                    ImageGridShelfCatalogMode::CatalogPath;
    return;
  }
  state.filter.enabled_catalog_paths.clear();
  state.filter.catalog_mode = ImageGridShelfCatalogMode::All;
  image_grid_catalog_commit_active(state);
}

void image_grid_filter_set_show_all_for_all_libraries(ImageGridUIState &state)
{
  const bool leaving_membership =
      state.filter.catalog_mode == ImageGridShelfCatalogMode::Recent ||
      state.filter.catalog_mode == ImageGridShelfCatalogMode::Favorites;
  for (asset_system::AssetLibrary *library : image_grid_all_mode_libraries()) {
    if (const std::optional<AssetLibraryReference> lib_ref = library->library_reference()) {
      BKE_asset_catalog_memory_set_all(&U, *lib_ref, image_grid_catalog_memory_domain);
    }
  }
  state.filter.enabled_catalog_paths.clear();
  if (!leaving_membership) {
    state.filter.catalog_mode = ImageGridShelfCatalogMode::All;
    /* Also clear membership mode stored under #ASSET_LIBRARY_ALL. */
    BKE_asset_catalog_memory_set_all(
        &U, asset_system::all_library_reference(), image_grid_catalog_memory_domain);
  }
  /* Membership (Recent/Favorites) already implies #ASSET_LIBRARY_ALL with no catalog
   * restriction; leaving it here would re-show every library's assets anyway, so #catalog_mode
   * intentionally keeps its Recent/Favorites value rather than being forced back to All -- this
   * mirrors #image_grid_filter_set_show_all()'s membership-exit branch, which restores rather
   * than overwrites. Do not call #BKE_asset_catalog_memory_set_all on #ASSET_LIBRARY_ALL while
   * membership is active (would clear RECENT/FAVORITES mode). */
}

void image_grid_filter_set_membership(ImageGridUIState &state,
                                      const ImageGridShelfCatalogMode membership_mode)
{
  BLI_assert(membership_mode == ImageGridShelfCatalogMode::Recent ||
             membership_mode == ImageGridShelfCatalogMode::Favorites);
  /* Mirror #image_grid_pending_schedule_from_asset: membership browses across all libraries and
   * must not commit empty paths into the per-library set. */
  if (state.filter.catalog_mode != ImageGridShelfCatalogMode::Recent &&
      state.filter.catalog_mode != ImageGridShelfCatalogMode::Favorites)
  {
    image_grid_catalog_commit_active(state);
  }
  state.filter.lib_ref = asset_system::all_library_reference();
  state.filter.enabled_catalog_paths.clear();
  state.filter.catalog_mode = membership_mode;
  /* Writes ONLY the mode field — never touches catalog_id_set (rev-4 membership guard). */
  BKE_asset_catalog_memory_set_mode(
      &U,
      state.filter.lib_ref,
      image_grid_catalog_memory_domain,
      (membership_mode == ImageGridShelfCatalogMode::Recent) ? ASSET_CATALOG_MEMORY_RECENT :
                                                               ASSET_CATALOG_MEMORY_FAVORITES);
}

void image_grid_catalog_swap_library(ImageGridUIState &state,
                                     const AssetLibraryReference & /*old_lib_ref*/,
                                     const AssetLibraryReference &new_lib_ref)
{
  /* Leaving membership: do not commit empty membership paths (would wipe ALL-library filters). */
  if (state.filter.catalog_mode != ImageGridShelfCatalogMode::Recent &&
      state.filter.catalog_mode != ImageGridShelfCatalogMode::Favorites)
  {
    image_grid_catalog_commit_active(state);
  }
  state.filter.lib_ref = new_lib_ref;
  image_grid_catalog_load_active(state, new_lib_ref);
  state.filter.catalog_mode = state.filter.enabled_catalog_paths.is_empty() ?
                                  ImageGridShelfCatalogMode::All :
                                  ImageGridShelfCatalogMode::CatalogPath;
}

void image_grid_catalog_sanitize_selection(ImageGridUIState &state)
{
  if (state.filter.enabled_catalog_paths.is_empty()) {
    return;
  }

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      state.filter.lib_ref);
  if (!library) {
    return;
  }

  Set<std::string> valid_paths;
  for (const std::string &path : state.filter.enabled_catalog_paths) {
    if (library->catalog_service().find_catalog_by_path(path.c_str())) {
      valid_paths.add(path);
    }
  }

  if (valid_paths == state.filter.enabled_catalog_paths) {
    return;
  }

  state.filter.enabled_catalog_paths = std::move(valid_paths);
  state.filter.catalog_mode = state.filter.enabled_catalog_paths.is_empty() ?
                                  ImageGridShelfCatalogMode::All :
                                  ImageGridShelfCatalogMode::CatalogPath;
  image_grid_catalog_commit_active(state);
}

bool image_grid_is_mask_slot_from_context(const bContext &C)
{
  return CTX_data_int_get(&C, "image_grid_is_mask_slot") != 0;
}

void image_grid_state_reset_catalog(ImageGridUIState &state)
{
  state.filter.enabled_catalog_paths.clear();
  BKE_asset_catalog_memory_set_all(&U, state.filter.lib_ref, image_grid_catalog_memory_domain);
  state.filter.catalog_mode = ImageGridShelfCatalogMode::All;
}

/* -------------------------------------------------------------------- */
/** \name Image Grid Filtered Sequence
 * \{ */

bool image_grid_is_assignable_texture(const Image &image)
{
  if (ELEM(image.source, IMA_SRC_VIEWER, IMA_SRC_GENERATED)) {
    return false;
  }
  if (ELEM(image.type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE)) {
    return false;
  }
  return true;
}

int image_grid_foreach_filtered_item(
    Main &bmain,
    const AssetLibraryReference &lib_ref,
    const blender::Set<std::string> &enabled_catalog_paths,
    blender::FunctionRef<bool(const ImageGridFilteredItem &, int)> fn)
{
  const bool catalog_filtering_enabled = !enabled_catalog_paths.is_empty();

  /* Build catalog filters once from the enabled paths; reused for every asset. */
  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  blender::Vector<asset_system::AssetCatalogFilter> catalog_filters;
  if (catalog_filtering_enabled && library) {
    catalog_filters.reserve(enabled_catalog_paths.size());
    for (const std::string &path : enabled_catalog_paths) {
      asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog_by_path(
          path.c_str());
      if (catalog) {
        catalog_filters.append(
            library->catalog_service().create_catalog_filter(catalog->catalog_id));
      }
    }
  }

  blender::Set<ID *> seen_ids;
  int filtered_index = 0;
  bool continue_iter = true;

  /* Phase 1: image assets from the library list. */
  if (library) {
    ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
      if (!continue_iter) {
        return false;
      }
      if (asset.get_id_type() != ID_IM) {
        return true;
      }
      /* Catalog filter: when enabled but filters could not be built (library still loading),
       * skip all assets so the display remains consistent with filter intent. */
      if (catalog_filtering_enabled) {
        if (catalog_filters.is_empty()) {
          return true;
        }
        bool in_any = false;
        for (const asset_system::AssetCatalogFilter &f : catalog_filters) {
          if (f.contains(asset.get_metadata().catalog_id)) {
            in_any = true;
            break;
          }
        }
        if (!in_any) {
          return true;
        }
      }
      /* Assignability check and dedup for locally-loaded assets. */
      if (ID *id = asset.local_id()) {
        if (GS(id->name) == ID_IM) {
          const Image *image = id_cast<const Image *>(id);
          if (!image_grid_is_assignable_texture(*image)) {
            return true;
          }
        }
        if (seen_ids.contains(id)) {
          return true;
        }
        seen_ids.add_new(id);
      }

      ImageGridFilteredItem item;
      item.asset = &asset;
      if (!fn(item, filtered_index)) {
        continue_iter = false;
      }
      filtered_index++;
      return continue_iter;
    });
  }

  /* Phase 2: non-asset images from the current file (LOCAL library only). */
  if (lib_ref.type == ASSET_LIBRARY_LOCAL) {
    ID *id;
    FOREACH_MAIN_ID_BEGIN (&bmain, id) {
      if (GS(id->name) != ID_IM) {
        continue;
      }
      if (id->asset_data) {
        /* Already iterated as an asset in phase 1. */
        continue;
      }
      if (seen_ids.contains(id)) {
        continue;
      }
      Image *image = id_cast<Image *>(id);
      if (!image_grid_is_assignable_texture(*image)) {
        continue;
      }
      seen_ids.add_new(id);

      if (continue_iter) {
        ImageGridFilteredItem item;
        item.image = image;
        if (!fn(item, filtered_index)) {
          continue_iter = false;
        }
      }
      filtered_index++;
    }
    FOREACH_MAIN_ID_END;
  }

  return filtered_index;
}

static int image_grid_foreach_membership_item(
    Main & /*bmain*/,
    const Span<ed::asset::shelf::ShelfAssetRef> membership,
    blender::FunctionRef<bool(const ImageGridFilteredItem &, int)> fn)
{
  /* Mirror AssetView::build_items(): park matches at list indices, then emit in list order. */
  blender::VectorSet<ed::asset::shelf::ShelfAssetRef> ordered;
  ordered.add_multiple(membership);

  blender::Vector<asset_system::AssetRepresentation *> ordered_assets(ordered.size(), nullptr);

  const AssetLibraryReference all_lib_ref = asset_system::all_library_reference();
  ed::asset::list::iterate(all_lib_ref, [&](asset_system::AssetRepresentation &asset) {
    if (asset.get_id_type() != ID_IM) {
      return true;
    }
    if (ID *id = asset.local_id()) {
      if (GS(id->name) == ID_IM) {
        const Image *image = id_cast<const Image *>(id);
        if (!image_grid_is_assignable_texture(*image)) {
          return true;
        }
      }
    }
    const ed::asset::shelf::ShelfAssetRef ref =
        ed::asset::shelf::ShelfAssetRef::from_weak_reference(asset.make_weak_reference());
    const int64_t index = ordered.index_of_try(ref);
    if (index < 0) {
      return true;
    }
    ordered_assets[index] = &asset;
    return true;
  });

  int filtered_index = 0;
  bool continue_iter = true;
  for (asset_system::AssetRepresentation *asset : ordered_assets) {
    if (!asset) {
      /* List entry absent from the loaded ALL-library view (removed / not yet fetched). */
      continue;
    }
    if (continue_iter) {
      ImageGridFilteredItem item;
      item.asset = asset;
      if (!fn(item, filtered_index)) {
        continue_iter = false;
      }
    }
    filtered_index++;
  }
  return filtered_index;
}

int image_grid_foreach_filtered_item(
    Main &bmain,
    const ImageGridUIState &state,
    blender::FunctionRef<bool(const ImageGridFilteredItem &, int)> fn)
{
  int out_index = 0;
  const NameMatchResolvedFilter name_match_resolved = BKE_name_match_filter_resolve(
      state.filter.name_match, U);
  auto gate = [&](const ImageGridFilteredItem &item, int /*inner*/) -> bool {
    StringRef name;
    Vector<StringRef> tags;
    if (item.asset != nullptr) {
      for (const AssetTag &tag : item.asset->get_metadata().tags) {
        tags.append(tag.name);
      }
      name = item.asset->get_name();
    }
    else if (item.image != nullptr) {
      name = item.image->id.name + 2;
    }
    else {
      return fn(item, out_index++);
    }
    if (!BKE_name_match_resolved_asset_passes(name_match_resolved, name, tags)) {
      return true;
    }
    return fn(item, out_index++);
  };

  if (state.filter.catalog_mode == ImageGridShelfCatalogMode::Recent) {
    image_grid_foreach_membership_item(
        bmain,
        ed::asset::shelf::shelf_asset_lists_recent(IMAGE_TEXTURE_SHELF_IDNAME),
        gate);
    return out_index;
  }
  if (state.filter.catalog_mode == ImageGridShelfCatalogMode::Favorites) {
    image_grid_foreach_membership_item(
        bmain,
        ed::asset::shelf::shelf_asset_lists_favorites(IMAGE_TEXTURE_SHELF_IDNAME),
        gate);
    return out_index;
  }
  if (state.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
    bool continue_iter = true;
    auto stop_tracking_gate = [&](const ImageGridFilteredItem &item, int inner) -> bool {
      if (!continue_iter) {
        return false;
      }
      continue_iter = gate(item, inner);
      return continue_iter;
    };
    const Set<std::string> empty_set;
    for (asset_system::AssetLibrary *library : image_grid_all_mode_libraries()) {
      const AssetLibraryReference lib_ref = *library->library_reference();
      const Set<std::string> per_lib_paths = image_grid_remembered_catalog_paths(lib_ref);
      image_grid_foreach_filtered_item(bmain,
                                       lib_ref,
                                       per_lib_paths.is_empty() ? empty_set : per_lib_paths,
                                       stop_tracking_gate);
      if (!continue_iter) {
        break;
      }
    }
    return out_index;
  }
  image_grid_foreach_filtered_item(
      bmain, state.filter.lib_ref, state.filter.enabled_catalog_paths, gate);
  return out_index;
}

/** \} */

}  // namespace blender::ed::view3d

namespace blender::ed::image_grid {

struct ImageGridStatesPerView3D {
  ImageGridUIState texture;
  ImageGridUIState mask;
  bool texture_initialized = false;
  bool mask_initialized = false;
};

/**
 * Return the per-owner grid state stored in the owner's runtime cache slot, creating it on first
 * access. Keeping it on the owning space (instead of a global map keyed by raw pointer) ties its
 * lifetime to the owner and avoids aliasing when a freed space's address is reused.
 */
static ImageGridStatesPerView3D &image_grid_states_ensure(const ImageGridOwner owner)
{
  void *&slot = owner.runtime_state_slot();
  if (!slot) {
    slot = MEM_new<ImageGridStatesPerView3D>(__func__);
  }
  return *static_cast<ImageGridStatesPerView3D *>(slot);
}

struct ImageGridOwnerDNAFields {
  AssetLibraryReference &library_ref;
  short &catalog_mode;
  char &filter_name_match_enabled;
  ListBaseT<AssetNameMatchIdLink> &filter_name_match_map_types;
};

static ImageGridOwnerDNAFields image_grid_owner_dna_fields(const ImageGridOwner owner,
                                                           const bool is_mask_slot)
{
  ImageGridSlotDNA &slot = owner.slot_dna(is_mask_slot);
  return {slot.library_ref,
          slot.catalog_mode,
          slot.filter_name_match_enabled,
          slot.filter_name_match_map_types};
}

static void image_grid_init_state_from_owner_dna(ImageGridUIState &state,
                                                 const ImageGridOwner owner,
                                                 const bool is_mask_slot)
{
  const ImageGridOwnerDNAFields dna = image_grid_owner_dna_fields(owner, is_mask_slot);

  if (dna.library_ref.type != 0) {
    state.filter.lib_ref = dna.library_ref;
  }
  else {
    state.filter.lib_ref = asset_system::current_file_library_reference();
  }

  /* Catalog filters come only from UserDef (domain #"image_grid"). Old blend-file DNA path lists
   * are intentionally dropped — no DNA→UserDef migration. Persist clears those DNA lists. */
  view3d::image_grid_catalog_load_active(state, state.filter.lib_ref);

  state.filter.name_match.enabled = (dna.filter_name_match_enabled != 0);
  state.filter.name_match.active_map_type_ids.clear();
  if (BLI_listbase_head_is_plausible(&dna.filter_name_match_map_types)) {
    for (const AssetNameMatchIdLink &link : dna.filter_name_match_map_types) {
      if (link.id[0] != '\0') {
        state.filter.name_match.active_map_type_ids.add(link.id);
      }
    }
  }

  const view3d::ImageGridShelfCatalogMode dna_mode =
      view3d::ImageGridShelfCatalogMode(dna.catalog_mode);
  if (dna_mode == view3d::ImageGridShelfCatalogMode::Recent ||
      dna_mode == view3d::ImageGridShelfCatalogMode::Favorites)
  {
    /* Membership: browse across all libraries; do not load catalog paths into the active filter. */
    state.filter.lib_ref = asset_system::all_library_reference();
    state.filter.enabled_catalog_paths.clear();
    state.filter.catalog_mode = dna_mode;
    BKE_asset_catalog_memory_set_mode(
        &U,
        state.filter.lib_ref,
        view3d::image_grid_catalog_memory_domain,
        (dna_mode == view3d::ImageGridShelfCatalogMode::Recent) ? ASSET_CATALOG_MEMORY_RECENT :
                                                                  ASSET_CATALOG_MEMORY_FAVORITES);
  }
  else {
    view3d::image_grid_catalog_sanitize_selection(state);
    state.filter.catalog_mode = state.filter.enabled_catalog_paths.is_empty() ?
                                    view3d::ImageGridShelfCatalogMode::All :
                                    view3d::ImageGridShelfCatalogMode::CatalogPath;
  }
}

ImageGridUIState &image_grid_state_get(const ImageGridOwner owner, const bool is_mask_slot)
{
  ImageGridStatesPerView3D &per_owner = image_grid_states_ensure(owner);
  ImageGridUIState &state = is_mask_slot ? per_owner.mask : per_owner.texture;
  bool &initialized = is_mask_slot ? per_owner.mask_initialized : per_owner.texture_initialized;
  if (!initialized) {
    initialized = true;
    image_grid_init_state_from_owner_dna(state, owner, is_mask_slot);
  }

  /* Re-checked on every access, not just on init: the Preferences can change at any time while the
   * state is alive. A Missing library is deliberately not repaired here -- the reference is kept so
   * the draw code can name it (§6). */
  ed::asset::library_reference_ensure_resolved(state.filter.lib_ref);
  return state;
}

bool image_grid_library_is_missing(const ImageGridOwner owner, const bool is_mask_slot)
{
  /* #image_grid_state_get already resolved the reference; just ask whether it landed. Using the
   * const #find_from_ref rather than the mutating gate keeps this safe to call from draw. */
  const ImageGridUIState &state = image_grid_state_get(owner, is_mask_slot);
  if (state.filter.lib_ref.type != ASSET_LIBRARY_CUSTOM) {
    return false;
  }
  return BKE_preferences_asset_library_find_from_ref(&U, &state.filter.lib_ref) == nullptr;
}

ImageGridUIState &image_grid_state_get_from_context(const bContext &C)
{
  /* Callers of this function only invoke it from a context where a grid is already known to be
   * drawing (see #image_grid_owner_from_context callers upstream), so an owner always resolves
   * here in practice; the assert documents that invariant instead of silently returning a
   * dangling reference if it's ever violated. */
  const std::optional<ImageGridOwner> owner = image_grid_owner_from_context(C);
  BLI_assert(owner.has_value());
  return image_grid_state_get(*owner, view3d::image_grid_is_mask_slot_from_context(C));
}

std::string image_grid_session_id(const ImageGridOwner owner,
                                  const bool is_mask_slot,
                                  const bool is_popover)
{
  /* One shared scroll/grip session per grid variant (texture/mask × sidebar/popover), keyed by the
   * owning space so split viewports (or Image Editors) never share it. The four variants of an
   * owner each get their own #GridSessionState in the shared registry (see
   * #AbstractGridView::use_session_scroll). */
  return fmt::format("img_grid:{}:{}{}",
                     fmt::ptr(owner.identity()),
                     is_mask_slot ? "mask" : "tex",
                     is_popover ? ":pop" : "");
}

void image_grid_reset_scroll(const ImageGridOwner owner, const bool is_mask_slot)
{
  /* A content-set change (filter / library / catalog) makes the old position meaningless: both the
   * sidebar and the popover host of this slot jump back to the top. */
  ui::grid_view_session_reset_scroll(image_grid_session_id(owner, is_mask_slot, false));
  ui::grid_view_session_reset_scroll(image_grid_session_id(owner, is_mask_slot, true));
}

void image_grid_state_remove(const ImageGridOwner owner)
{
  /* Drop this owner's four grid sessions from the shared registry before the state is freed; safe
   * because the space is being torn down, so no live view still references them. */
  for (const bool is_mask : {false, true}) {
    for (const bool is_popover : {false, true}) {
      ui::grid_view_session_remove(image_grid_session_id(owner, is_mask, is_popover));
    }
  }
  void *&slot = owner.runtime_state_slot();
  if (slot) {
    MEM_delete(static_cast<ImageGridStatesPerView3D *>(slot));
    slot = nullptr;
  }
}

void image_grid_foreach_live_library_ref(const ImageGridOwner owner,
                                         blender::FunctionRef<void(AssetLibraryReference &)> fn)
{
  /* Only touch state that already exists -- a rename must not eagerly create runtime state for an
   * owner that has never displayed an image grid. */
  if (!owner.runtime_state_slot()) {
    return;
  }
  fn(image_grid_state_get(owner, false).filter.lib_ref);
  fn(image_grid_state_get(owner, true).filter.lib_ref);
}

void image_grid_foreach_live_name_match_ids(
    const ImageGridOwner owner, blender::FunctionRef<void(blender::Set<std::string> &)> fn)
{
  if (!owner.runtime_state_slot()) {
    return;
  }
  fn(image_grid_state_get(owner, false).filter.name_match.active_map_type_ids);
  fn(image_grid_state_get(owner, true).filter.name_match.active_map_type_ids);
}

}  // namespace blender::ed::image_grid
