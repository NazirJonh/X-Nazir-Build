/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Asset-library source for the ID browser popover (see #interface_template_id_browser.cc).
 *
 * Kept separate from the ID browser itself because none of this is needed for the default
 * blend-data source. Library/catalog/name-match state lives in
 * #wmWindowManager::id_browser_grid_view_settings (not on a space), so the same selection applies
 * wherever the popover is opened from.
 */

#include <optional>

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_asset_catalog_memory.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_preferences.h"

#include "BLI_map.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BLT_translation.hh"

#include "DNA_asset_types.h"
#include "DNA_ID.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.hh"
#include "ED_screen.hh"

/* Recent/Favorites asset-list registry (#ShelfAssetRef, #shelf_asset_lists_recent/favorites).
 * Internal to `editors/asset/`, but already reused outside it the same way (see
 * `view3d_image_grid_state.cc`, `brush_asset_ops.cc`) for the same Recent/Favorites feature. */
#include "../../asset/intern/asset_shelf_asset_lists.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"

#include "interface_grid_view_settings_utils.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Grid settings access
 * \{ */

PointerRNA id_browser_grid_settings_ptr(wmWindowManager &wm)
{
  PointerRNA wm_ptr = RNA_id_pointer_create(&wm.id);
  return RNA_pointer_get(&wm_ptr, "id_browser_grid_view_settings");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Library display
 * \{ */

AssetLibraryReference id_browser_library_ref_get(wmWindowManager &wm)
{
  PointerRNA settings = id_browser_grid_settings_ptr(wm);
  AssetLibraryReference ref{};
  if (settings.data) {
    ref = grid_settings::library_ref_get(settings);
  }
  else {
    ref.type = ASSET_LIBRARY_LOCAL;
    ref.custom_library_index = -1;
  }
  ed::asset::library_reference_ensure_resolved(ref);
  return ref;
}

void id_browser_library_ref_set(wmWindowManager &wm, const AssetLibraryReference &library_ref)
{
  PointerRNA settings = id_browser_grid_settings_ptr(wm);
  if (settings.data != nullptr) {
    grid_settings::library_ref_set(settings, library_ref);
  }
}

bool id_browser_library_is_missing(wmWindowManager &wm)
{
  AssetLibraryReference ref = id_browser_library_ref_get(wm);
  return ed::asset::library_reference_ensure_resolved(ref) ==
         ed::asset::LibraryRefStatus::Missing;
}

const char *id_browser_library_ui_name(const AssetLibraryReference &lib_ref)
{
  switch (lib_ref.type) {
    case ASSET_LIBRARY_ALL:
      return IFACE_("All Libraries");
    case ASSET_LIBRARY_LOCAL:
      return IFACE_("Current File");
    case ASSET_LIBRARY_ESSENTIALS:
      return IFACE_("Essentials");
    case ASSET_LIBRARY_ONLINE_ESSENTIALS:
      return IFACE_("Online Essentials");
    case ASSET_LIBRARY_CUSTOM: {
      const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(&U,
                                                                                          &lib_ref);
      if (user_library && user_library->name[0]) {
        return user_library->name;
      }
      return IFACE_("Asset Library");
    }
    default:
      return IFACE_("Asset Library");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recent / Favorites membership
 * \{ */

std::string id_browser_shelf_idname(const short idcode)
{
  return std::string(ed::asset::shelf::ID_BROWSER_SHELF_IDNAME_PREFIX) +
        BKE_idtype_idcode_to_name(idcode);
}

void id_browser_set_membership(wmWindowManager &wm, const grid_settings::CatalogMode mode)
{
  PointerRNA settings = id_browser_grid_settings_ptr(wm);
  if (settings.data == nullptr) {
    return;
  }
  /* Recent/Favorites are membership lists spanning every loaded library (mirrors
   * #image_grid_filter_set_membership), not a per-library catalog filter, so the library selector
   * is pointed at #ASSET_LIBRARY_ALL to keep #id_browser_library_is_missing() and the header label
   * consistent with what #id_browser_foreach_membership_asset actually iterates. */
  grid_settings::library_ref_set(settings, asset_system::all_library_reference());
  grid_settings::catalog_mode_set_membership(settings, mode);
  grid_view_session_reset_scroll(id_browser_grid_session_key);
  WM_file_tag_modified();

}

void id_browser_foreach_membership_asset(const bContext &C,
                                         const grid_settings::CatalogMode mode,
                                         const short idcode,
                                         FunctionRef<bool(asset_system::AssetRepresentation &)> fn)
{
  BLI_assert(ELEM(mode, grid_settings::CatalogMode::Recent, grid_settings::CatalogMode::Favorites));

  const std::string shelf_idname = id_browser_shelf_idname(idcode);
  const Span<ed::asset::shelf::ShelfAssetRef> membership =
      (mode == grid_settings::CatalogMode::Recent) ?
          ed::asset::shelf::shelf_asset_lists_recent(shelf_idname) :
          ed::asset::shelf::shelf_asset_lists_favorites(shelf_idname);
  if (membership.is_empty()) {
    return;
  }

  /* Park matches at their list index, then emit in list order (most-recent/favorite-order first),
   * not asset-library iteration order. Mirrors #image_grid_foreach_membership_item
   * (view3d_image_grid_state.cc), the same pattern for the Image Grid's Recent/Favorites. */
  VectorSet<ed::asset::shelf::ShelfAssetRef> ordered;
  ordered.add_multiple(membership);

  Vector<asset_system::AssetRepresentation *> ordered_assets(ordered.size(), nullptr);

  const AssetLibraryReference all_lib_ref = asset_system::all_library_reference();
  ed::asset::list::storage_fetch(&all_lib_ref, &C);
  const ID_Type id_type = ID_Type(idcode);
  ed::asset::list::iterate(all_lib_ref, [&](asset_system::AssetRepresentation &asset) {
    if (asset.get_id_type() != id_type) {
      return true;
    }
    const ed::asset::shelf::ShelfAssetRef ref = ed::asset::shelf::ShelfAssetRef::from_weak_reference(
        asset.make_weak_reference());
    const int64_t index = ordered.index_of_try(ref);
    if (index < 0) {
      return true;
    }
    ordered_assets[index] = &asset;
    return true;
  });

  for (asset_system::AssetRepresentation *asset : ordered_assets) {
    if (asset != nullptr && !fn(*asset)) {
      break;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset iteration
 * \{ */

/**
 * Drop catalog UUIDs that no longer exist in \a library (catalog renamed or removed elsewhere);
 * otherwise a stale id would silently filter everything out with no way to see why.
 */
static void id_browser_catalog_selection_sanitize(const AssetLibraryReference &lib_ref,
                                                   const asset_system::AssetLibrary &library)
{
  if (BKE_asset_catalog_memory_get_mode(&U, lib_ref, grid_settings::id_browser_catalog_memory_domain) !=
      ASSET_CATALOG_MEMORY_SET)
  {
    return;
  }

  const Vector<bUUID> enabled_ids = BKE_asset_catalog_memory_get_set(
      &U, lib_ref, grid_settings::id_browser_catalog_memory_domain);
  if (enabled_ids.is_empty()) {
    return;
  }

  Vector<bUUID> valid_ids;
  for (const bUUID &catalog_id : enabled_ids) {
    if (library.catalog_service().find_catalog(asset_system::CatalogID(catalog_id))) {
      valid_ids.append(catalog_id);
    }
  }

  if (valid_ids.size() == enabled_ids.size()) {
    return;
  }

  if (valid_ids.is_empty()) {
    BKE_asset_catalog_memory_set_all(&U, lib_ref, grid_settings::id_browser_catalog_memory_domain);
  }
  else {
    BKE_asset_catalog_memory_set_set(
        &U, lib_ref, grid_settings::id_browser_catalog_memory_domain, valid_ids.as_span());
  }
  /* Preferences-only write (#BKE_asset_catalog_memory_set_all/_set_set already flag
   * #UserDef.runtime.is_dirty) -- this does not touch blend-file data, so the file itself must
   * not be marked modified. */
}

static bool catalog_id_in_span(const Span<bUUID> ids, const bUUID &catalog_id)
{
  for (const bUUID &id : ids) {
    if (BLI_uuid_equal(id, catalog_id)) {
      return true;
    }
  }
  return false;
}

void id_browser_foreach_asset(const bContext &C,
                              const AssetLibraryReference &lib_ref,
                              const short idcode,
                              FunctionRef<bool(asset_system::AssetRepresentation &)> fn)
{
  /* Asynchronous: the first draw may see an empty list. The block's asset listener (see
   * #id_browser_popover_draw) redraws the popover once the library is read. */
  ed::asset::list::storage_fetch(&lib_ref, &C);

  const bool all_libraries = lib_ref.type == ASSET_LIBRARY_ALL;

  Vector<bUUID> catalog_ids;
  const eAssetCatalogMemoryMode mode = BKE_asset_catalog_memory_get_mode(
      &U, lib_ref, grid_settings::id_browser_catalog_memory_domain);
  if (!all_libraries && mode == ASSET_CATALOG_MEMORY_SET) {
    catalog_ids = BKE_asset_catalog_memory_get_set(
        &U, lib_ref, grid_settings::id_browser_catalog_memory_domain);
  }

  if (!all_libraries && !catalog_ids.is_empty() && ed::asset::list::is_loaded(&lib_ref)) {
    if (const asset_system::AssetLibrary *library =
            ed::asset::list::library_get_once_available(lib_ref))
    {
      id_browser_catalog_selection_sanitize(lib_ref, *library);
      if (BKE_asset_catalog_memory_get_mode(
              &U, lib_ref, grid_settings::id_browser_catalog_memory_domain) ==
          ASSET_CATALOG_MEMORY_SET)
      {
        catalog_ids = BKE_asset_catalog_memory_get_set(
            &U, lib_ref, grid_settings::id_browser_catalog_memory_domain);
      }
      else {
        catalog_ids.clear();
      }
    }
  }

  const bool catalog_filtering_enabled = !all_libraries && !catalog_ids.is_empty();

  /* Build catalog filters once from the enabled UUIDs; reused for every asset. */
  Vector<asset_system::AssetCatalogFilter> catalog_filters;
  if (catalog_filtering_enabled) {
    if (const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
            lib_ref))
    {
      for (const bUUID &catalog_id : catalog_ids) {
        if (library->catalog_service().find_catalog(asset_system::CatalogID(catalog_id))) {
          catalog_filters.append(
              library->catalog_service().create_catalog_filter(asset_system::CatalogID(catalog_id)));
        }
      }
    }
    /* The library is still loading: showing an unfiltered list would flash items the user has
     * filtered out. Show nothing until the catalogs are known. */
    if (catalog_filters.is_empty()) {
      return;
    }
  }

  /* Precompute once per call rather than per asset: #BKE_asset_catalog_memory_get_mode /
   * #_get_set walk #UserDef.catalog_memory and heap-allocate a #Vector, which used to happen for
   * every asset that reached this branch when filtering across #ASSET_LIBRARY_ALL (thousands of
   * redundant list walks + allocations per redraw on a large multi-library setup). */
  Map<std::string, Vector<bUUID>> all_libraries_catalog_ids_by_library;
  if (all_libraries) {
    for (asset_system::AssetLibrary *library :
        ed::asset::all_mode_libraries(/*exclude_image_libraries=*/false,
                                      /*only_image_libraries=*/false))
    {
      const std::optional<AssetLibraryReference> per_lib_ref = library->library_reference();
      if (!per_lib_ref) {
        continue;
      }
      if (BKE_asset_catalog_memory_get_mode(
              &U, *per_lib_ref, grid_settings::id_browser_catalog_memory_domain) !=
          ASSET_CATALOG_MEMORY_SET)
      {
        continue;
      }
      Vector<bUUID> ids = BKE_asset_catalog_memory_get_set(
          &U, *per_lib_ref, grid_settings::id_browser_catalog_memory_domain);
      if (ids.is_empty()) {
        continue;
      }
      all_libraries_catalog_ids_by_library.add(
          BKE_preferences_asset_library_identifier_from_ref(&U, &*per_lib_ref), std::move(ids));
    }
  }

  const ID_Type id_type = ID_Type(idcode);
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) -> bool {
    if (asset.get_id_type() != id_type) {
      return true;
    }
    if (catalog_filtering_enabled) {
      const AssetMetaData &metadata = asset.get_metadata();
      bool in_catalog = false;
      for (const asset_system::AssetCatalogFilter &filter : catalog_filters) {
        if (filter.contains(metadata.catalog_id)) {
          in_catalog = true;
          break;
        }
      }
      if (!in_catalog) {
        return true;
      }
    }
    if (all_libraries) {
      /* Catalog ids are only unique within a library, so filtering across #ASSET_LIBRARY_ALL
       * must resolve each asset's *actual* source library rather than trusting the merged "All"
       * pseudo-library's own catalog id space. Mirrors
       * #image_grid_asset_is_visible_in_state (view3d_image_shelf_sync.cc). */
      const std::optional<AssetLibraryReference> asset_lib_ref =
          asset.owner_asset_library().library_reference();
      if (!asset_lib_ref.has_value()) {
        return true;
      }
      if (const Vector<bUUID> *per_lib_ids = all_libraries_catalog_ids_by_library.lookup_ptr(
              BKE_preferences_asset_library_identifier_from_ref(&U, &*asset_lib_ref)))
      {
        const AssetMetaData &metadata = asset.get_metadata();
        if (!catalog_id_in_span(*per_lib_ids, metadata.catalog_id)) {
          return true;
        }
      }
    }
    return fn(asset);
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Library Operator
 * \{ */

const EnumPropertyItem *id_browser_library_rna_itemf(const bContext *C, bool *r_free)
{
  /* Restrict the library list to libraries explicitly set up via "Add Image Library" when the
   * popover is actually browsing images (its only current use -- see
   * #interface_template_id_browser.cc's docstring). Image indexing itself is opt-in (see
   * #image_library_needs_reindex()), so an untagged library can never surface an image asset here
   * either. Resolved the same way #build_id_grid() resolves its target idcode. Falls back to the
   * permissive default for any other browsed ID type. */
  bool only_image_libraries = false;
  PointerRNA target_ptr = CTX_data_pointer_get(C, "id_browser_ptr");
  const std::optional<StringRefNull> prop_name = CTX_data_string_get(C, "id_browser_prop");
  if (target_ptr.data && prop_name) {
    if (PropertyRNA *target_prop = RNA_struct_find_property(&target_ptr, prop_name->c_str())) {
      if (RNA_property_type(target_prop) == PROP_POINTER) {
        const StructRNA *ptr_type = RNA_property_pointer_type(&target_ptr, target_prop);
        only_image_libraries = ptr_type && RNA_type_to_ID_code(ptr_type) == ID_IM;
      }
    }
  }

  const EnumPropertyItem *items = ed::asset::library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false,
      /*exclude_image_libraries=*/false,
      only_image_libraries);
  *r_free = (items != nullptr);
  return items;
}

bool id_browser_set_asset_library(wmWindowManager &wm, const int library_enum_value)
{
  PointerRNA settings = id_browser_grid_settings_ptr(wm);
  if (settings.data == nullptr) {
    return false;
  }
  const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(
      library_enum_value);
  const AssetLibraryReference old_ref = grid_settings::library_ref_get(settings);
  if (new_ref.type == old_ref.type && new_ref.custom_library_index == old_ref.custom_library_index)
  {
    return false;
  }

  grid_settings::library_ref_set(settings, new_ref);
  /* Per-library catalog filters live in #UserDef.catalog_memory (domain #"id_browser"); switching
   * libraries restores that library's remembered mode/set rather than clearing a shared path list. */
  grid_view_session_reset_scroll(id_browser_grid_session_key);
  WM_file_tag_modified();
  return true;
}

static const EnumPropertyItem *rna_id_browser_library_itemf(bContext *C,
                                                            PointerRNA * /*ptr*/,
                                                            PropertyRNA * /*prop*/,
                                                            bool *r_free)
{
  return id_browser_library_rna_itemf(C, r_free);
}

static wmOperatorStatus id_browser_set_library_exec(bContext *C, wmOperator *op)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return OPERATOR_CANCELLED;
  }

  if (!id_browser_set_asset_library(*wm, RNA_enum_get(op->ptr, "asset_library_reference"))) {
    return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_ASSET | ND_ASSET_LIST, nullptr);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
  return OPERATOR_FINISHED;
}

void UI_OT_id_browser_set_library(wmOperatorType *ot)
{
  ot->name = "Set Asset Library";
  ot->description = "Set the asset library browsed by the ID browser";
  ot->idname = "UI_OT_id_browser_set_library";

  ot->exec = id_browser_set_library_exec;

  /* UI state only, no undo push. */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "asset_library_reference", rna_enum_dummy_NULL_items, 0, "Asset Library", "");
  RNA_def_enum_funcs(prop, rna_id_browser_library_itemf);
}

/** \} */

}  // namespace blender::ui
