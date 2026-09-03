/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "interface_grid_view_sources.hh"

#include <memory>
#include <string>

#include "BLI_utildefines.h"

#include "interface_grid_view_settings_utils.hh"

#include "BKE_name_matching.hh"

#include "DNA_userdef_types.h"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_string.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "DNA_asset_types.h"
#include "DNA_ID.h"

#include "ED_asset.hh"
#include "ED_asset_list.hh"
#include "ED_asset_menu_utils.hh"
#include "intern/asset_shelf_asset_lists.hh"

#include "RNA_access.hh"

#include "UI_grid_view.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Optional-string helpers
 *
 * #AssetGridSourceParams carries its strings as plain `const char *` because its callers hand it
 * string literals and DNA fields; null and "" both mean "not set" there.
 * \{ */

/** Whether \a str carries anything. */
static bool string_is_set(const char *str)
{
  return str != nullptr && str[0] != '\0';
}

/** \a str as an owned string; null becomes empty. */
static std::string string_or_empty(const char *str)
{
  return (str != nullptr) ? std::string(str) : std::string();
}

/** \a str when it is set, \a fallback otherwise. */
static const char *string_or(const char *str, const char *fallback)
{
  return string_is_set(str) ? str : fallback;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name AssetGridItem — grid tile for a single asset
 * \{ */

class AssetGridItem : public PreviewGridItem {
  asset_system::AssetRepresentation *asset_; /* Not owned; valid for asset-list lifetime. */
  AssetLibraryReference library_ref_;        /* Copied for is_loaded check during draw. */
  AssetGridItemParams params_;

 public:
  AssetGridItem(asset_system::AssetRepresentation &asset,
                const AssetLibraryReference &library_ref,
                StringRef identifier,
                StringRef label,
                const bool is_active,
                const AssetGridItemParams &params)
      : PreviewGridItem(identifier, label, ICON_NONE),
        asset_(&asset),
        library_ref_(library_ref),
        params_(params)
  {
    /* Activate on click rather than on press, so a touch/pen drag over the grid scrolls it
     * instead of assigning whatever tile the gesture happened to start on. */
    this->select_on_click_set();

    if (!params_.show_names) {
      this->hide_label();
    }

    /* Answer for every item, not just the active one: an item that stays silent lets the view keep
     * whatever tile was activated by an earlier click, so an assignment made elsewhere (a drop on
     * the host's own field) would leave the old tile highlighted. Answering unconditionally also
     * covers the host having nothing assigned at all, which is how clearing the assignment
     * (#PAINT_OT_material_channel_source_clear) drops the highlight. */
    this->set_is_active_fn([is_active]() { return is_active; });
  }

  void build_grid_tile(const bContext &C, Layout &layout) const override;
  void on_activate(bContext &C) override;
  std::unique_ptr<AbstractViewItemDragController> create_drag_controller() const override;
};

/**
 * Default asset/ID drag, matching #AssetDragController on the asset shelf. When
 * \a drag_operator is set, the operator is invoked instead and the default drag is disabled.
 */
class AssetGridItemDragController : public AbstractViewItemDragController {
  asset_system::AssetRepresentation &asset_;
  std::string drag_operator_;

 public:
  AssetGridItemDragController(AbstractGridView &view,
                              asset_system::AssetRepresentation &asset,
                              StringRef drag_operator)
      : AbstractViewItemDragController(view), asset_(asset), drag_operator_(drag_operator)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    if (!drag_operator_.empty()) {
      /* Disable the default asset/ID drag; #on_drag_start() invokes #drag_operator_. */
      return std::nullopt;
    }
    return asset_.is_local_id() ? WM_DRAG_ID : WM_DRAG_ASSET;
  }

  void *create_drag_data() const override
  {
    if (!drag_operator_.empty()) {
      return nullptr;
    }
    ID *local_id = asset_.local_id();
    if (local_id) {
      return static_cast<void *>(local_id);
    }

    eAssetImportMethod import_method = asset_.get_import_method().value_or(ASSET_IMPORT_PACK);
    if (U.experimental.no_data_block_packing && import_method == ASSET_IMPORT_PACK) {
      import_method = ASSET_IMPORT_APPEND_REUSE;
    }

    AssetImportSettings import_settings{};
    import_settings.method = import_method;
    import_settings.use_instance_collections = false;
    return WM_drag_create_asset_data(&asset_, import_settings);
  }

  void on_drag_start(bContext &C, AbstractViewItem & /*item*/) override
  {
    if (drag_operator_.empty()) {
      return;
    }
    wmOperatorType *ot = WM_operatortype_find(drag_operator_.c_str(), true);
    if (!ot) {
      return;
    }
    PointerRNA *op_props = MEM_new<PointerRNA>(__func__, WM_operator_properties_create_ptr(ot));
    ed::asset::operator_asset_reference_props_set(asset_, *op_props);
    WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::InvokeRegionWin, op_props, nullptr);
    WM_operator_properties_free(op_props);
    MEM_delete(op_props);
  }
};

std::unique_ptr<AbstractViewItemDragController> AssetGridItem::create_drag_controller() const
{
  if (!params_.use_drag) {
    /* No drag controller at all: the press is left to the grid's own drag-scroll arbitration
     * instead of being claimed by asset drag-and-drop. */
    return nullptr;
  }
  return std::make_unique<AssetGridItemDragController>(
      this->get_view(), *asset_, params_.drag_operator);
}

void AssetGridItem::build_grid_tile(const bContext &C, Layout &layout) const
{
  /* Trigger async thumbnail loading for visible items only. */
  asset_->ensure_previewable(C);

  const int preview_id = ed::asset::list::is_loaded(&library_ref_) ?
                             ed::asset::asset_preview_or_icon(*asset_) :
                             ICON_PREVIEW_LOADING;

  const GridViewStyle &style = this->get_view().get_style();
  Button *item_but = reinterpret_cast<Button *>(this->view_item_button());
  button_view_item_draw_size_set(
      item_but, style.tile_width + 2 * U.pixelsize, style.tile_height + 2 * U.pixelsize);

  Layout &overlap = layout.overlap();
  overlap.ui_units_x_set(style.tile_width / float(UI_UNIT_X));
  overlap.ui_units_y_set(style.tile_height / float(UI_UNIT_Y));

  this->build_grid_tile_button(overlap.column(true), preview_id);

  button_func_tooltip_custom_set(
      item_but,
      [](bContext &C, TooltipData &tip, Button * /*but*/, void *argN) {
        const auto *asset = static_cast<const asset_system::AssetRepresentation *>(argN);
        ed::asset::asset_tooltip(&C, *asset, tip);
      },
      asset_,
      nullptr);
}

void AssetGridItem::on_activate(bContext &C)
{
  if (params_.activate_operator.empty()) {
    return;
  }
  wmOperatorType *ot = WM_operatortype_find(params_.activate_operator.c_str(), true);
  if (!ot) {
    return;
  }
  PointerRNA *op_props = MEM_new<PointerRNA>(__func__, WM_operator_properties_create_ptr(ot));
  ed::asset::operator_asset_reference_props_set(*asset_, *op_props);
  /* Host-defined target, handed over explicitly rather than through the layout context: an
   * operator invoked from a tile cannot rely on the context store the panel published. */
  if (!params_.activate_context_id.empty() && RNA_struct_find_property(op_props, "context_id")) {
    RNA_string_set(op_props, "context_id", params_.activate_context_id.c_str());
  }
  WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::InvokeRegionWin, op_props, nullptr);
  WM_operator_properties_free(op_props);
  MEM_delete(op_props);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Name-match filter helpers
 * \{ */

static bool asset_passes_name_match(const NameMatchResolvedFilter &resolved,
                                    const asset_system::AssetRepresentation &asset)
{
  if (!resolved.active) {
    /* Disabled, or enabled with nothing resolvable: everything passes, so the asset's tags need
     * not be collected at all. Checked here rather than left to
     * #BKE_name_match_resolved_asset_passes because gathering them is the expensive half, and
     * this runs once per asset on every filtering pass. */
    return true;
  }
  Vector<StringRef> metadata_tags;
  for (const AssetTag &tag : asset.get_metadata().tags) {
    metadata_tags.append(tag.name);
  }
  return BKE_name_match_resolved_asset_passes(resolved, asset.get_name(), metadata_tags);
}

/**
 * Case-insensitive substring match of the grid's search field against the asset name. Matches how
 * #uiList's own name filter reads to a user: a plain "contains", not a glob or a fuzzy score.
 *
 * #BLI_strcasestr works straight on both null-terminated strings, so nothing is allocated here --
 * this runs once per asset on every filtering pass over the whole library.
 */
static bool asset_passes_search(const StringRefNull search,
                                const asset_system::AssetRepresentation &asset)
{
  if (search.is_empty()) {
    return true;
  }
  return BLI_strcasestr(asset.get_name().c_str(), search.c_str()) != nullptr;
}

static bool asset_grid_extra_poll(const asset_system::AssetRepresentation &asset,
                                  const Set<short> &filter_id_types,
                                  const NameMatchResolvedFilter &name_match_resolved,
                                  const StringRefNull search)
{
  /* Cheapest test first: the type check is an integer set lookup, the search a string scan, and
   * the name match the only one that walks the asset's tags. */
  if (!filter_id_types.is_empty() && !filter_id_types.contains(asset.get_id_type())) {
    return false;
  }
  if (!asset_passes_search(search, asset)) {
    return false;
  }
  return asset_passes_name_match(name_match_resolved, asset);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name AssetGridDataSource
 * \{ */

AssetGridDataSource::AssetGridDataSource(AssetGridDataSourceConfig config)
    : config_(std::move(config))
{
}

/**
 * Assets of the shelf's Recent / Favorites list, in list order. Mirrors
 * #image_grid_foreach_membership_item: the lists hold weak references, so the assets themselves
 * are found by walking the combined "All Libraries" list once and parking matches at their list
 * index.
 *
 * Resolved on first use and cached for this source's lifetime (#membership_assets_): the walk is
 * over every asset of every library, and #foreach_filtered runs several times per redraw.
 */
Span<asset_system::AssetRepresentation *> AssetGridDataSource::membership_assets() const
{
  if (membership_assets_) {
    return *membership_assets_;
  }

  const Span<ed::asset::shelf::ShelfAssetRef> membership =
      config_.membership_is_favorites ?
          ed::asset::shelf::shelf_asset_lists_favorites(config_.membership_shelf_idname) :
          ed::asset::shelf::shelf_asset_lists_recent(config_.membership_shelf_idname);

  VectorSet<ed::asset::shelf::ShelfAssetRef> ordered;
  ordered.add_multiple(membership);
  Vector<asset_system::AssetRepresentation *> slots(ordered.size(), nullptr);

  const AssetLibraryReference all_lib_ref = asset_system::all_library_reference();
  ed::asset::list::iterate(all_lib_ref, [&](asset_system::AssetRepresentation &asset) {
    const int64_t index = ordered.index_of_try(
        ed::asset::shelf::ShelfAssetRef::from_weak_reference(asset.make_weak_reference()));
    if (index >= 0) {
      slots[index] = &asset;
    }
    return true;
  });

  /* A list entry whose asset is gone (file moved, library disabled) leaves an empty slot; drop
   * those so the cached listing is exactly what gets enumerated. */
  Vector<asset_system::AssetRepresentation *> assets;
  assets.reserve(slots.size());
  for (asset_system::AssetRepresentation *asset : slots) {
    if (asset != nullptr) {
      assets.append(asset);
    }
  }

  membership_assets_ = std::move(assets);
  return *membership_assets_;
}

/** The library whose list must be read before this source can enumerate anything: membership
 * browses across every library, so it reads the combined one. */
AssetLibraryReference AssetGridDataSource::listed_library_ref() const
{
  return config_.membership_shelf_idname.empty() ? config_.library_ref :
                                                   asset_system::all_library_reference();
}

void AssetGridDataSource::foreach_filtered(
    const FunctionRef<bool(asset_system::AssetRepresentation &, int)> fn) const
{
  const NameMatchResolvedFilter name_match_resolved = BKE_name_match_filter_resolve(
      config_.name_match, U);
  const auto extra_poll = [&](const asset_system::AssetRepresentation &asset) {
    return asset_grid_extra_poll(
        asset, config_.filter_id_types, name_match_resolved, config_.search);
  };

  if (!config_.membership_shelf_idname.empty()) {
    /* Membership is its own ordering (list order), so catalogs do not apply; the type and
     * name-match filters still do. */
    int index = 0;
    for (asset_system::AssetRepresentation *asset : this->membership_assets()) {
      if (!extra_poll(*asset)) {
        continue;
      }
      if (!fn(*asset, index++)) {
        return;
      }
    }
    return;
  }

  ed::asset::foreach_filtered_asset(
      config_.library_ref,
      config_.enabled_catalogs.is_empty() ? nullptr : &config_.enabled_catalogs,
      ed::asset::CatalogContainment::IncludeChildren,
      extra_poll,
      fn);
}

int AssetGridDataSource::item_count(const bContext & /*C*/) const
{
  const AssetLibraryReference listed_ref = this->listed_library_ref();
  if (!ed::asset::list::is_loaded(&listed_ref)) {
    /* Iterating an unread list sorts entries whose catalog data is not filled in yet
     * (#filelist_files_ensure -> #filelist_sort). Report empty until the read finishes; the
     * #ND_ASSET_LIST notifier rebuilds the grid then. */
    return 0;
  }

  int count = 0;
  this->foreach_filtered([&](asset_system::AssetRepresentation & /*asset*/, int /*index*/) {
    count++;
    return true;
  });
  return count;
}

bool AssetGridDataSource::item_count_ready(const bContext & /*C*/) const
{
  const AssetLibraryReference listed_ref = this->listed_library_ref();
  return ed::asset::list::is_loaded(&listed_ref);
}

int AssetGridDataSource::filtered_index_of(const bContext & /*C*/,
                                           const StringRef identifier) const
{
  const AssetLibraryReference listed_ref = this->listed_library_ref();
  if (identifier.is_empty() || !ed::asset::list::is_loaded(&listed_ref)) {
    return -1;
  }

  int found = -1;
  this->foreach_filtered([&](asset_system::AssetRepresentation &asset, const int index) {
    if (asset.library_relative_identifier() == identifier) {
      found = index;
      return false;
    }
    return true;
  });
  return found;
}

int AssetGridDataSource::filtered_index_of_with_count(const bContext & /*C*/,
                                                      const StringRef identifier,
                                                      int *r_count) const
{
  *r_count = 0;
  const AssetLibraryReference listed_ref = this->listed_library_ref();
  if (!ed::asset::list::is_loaded(&listed_ref)) {
    return -1;
  }

  /* One pass rather than #item_count followed by #filtered_index_of: the walk has to run to the
   * end for the count anyway, so the index falls out of it for free. */
  int found = -1;
  int count = 0;
  this->foreach_filtered([&](asset_system::AssetRepresentation &asset, const int index) {
    if (found < 0 && !identifier.is_empty() &&
        asset.library_relative_identifier() == identifier)
    {
      found = index;
    }
    count++;
    return true;
  });
  *r_count = count;
  return found;
}

std::string AssetGridDataSource::filtered_identifier_at(const bContext & /*C*/,
                                                       const int index) const
{
  const AssetLibraryReference listed_ref = this->listed_library_ref();
  if (index < 0 || !ed::asset::list::is_loaded(&listed_ref)) {
    return "";
  }

  std::string found;
  this->foreach_filtered([&](asset_system::AssetRepresentation &asset, const int i) {
    if (i == index) {
      found = asset.library_relative_identifier();
      return false;
    }
    return true;
  });
  return found;
}

/**
 * The enabled-catalog paths the catalog selector popover last wrote for \a lib_ref under
 * \a domain. The popover stores catalog UUIDs in the preferences catalog memory
 * (#id_browser_catalog_id_set_enabled), while asset filtering matches on catalog paths, so the
 * ids are resolved through the library's catalog service here -- the same conversion the image
 * grid does in #image_grid_remembered_catalog_paths. An empty result means "show all", either
 * because the domain is not in #ASSET_CATALOG_MEMORY_SET mode or because the library is not
 * loaded yet (in which case the next redraw, after the load, resolves them).
 */
static Set<std::string> catalog_memory_enabled_paths(const AssetLibraryReference &lib_ref,
                                                     const char *domain)
{
  Set<std::string> paths;
  if (BKE_asset_catalog_memory_get_mode(&U, lib_ref, domain) != ASSET_CATALOG_MEMORY_SET) {
    return paths;
  }
  const Vector<bUUID> ids = BKE_asset_catalog_memory_get_set(&U, lib_ref, domain);
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

/**
 * The host's Recent/Favorites membership mode, or #CatalogMode::All when this grid has no
 * membership shelf. Recent / Favorites are not libraries: they list the named shelf's membership
 * across every library, so the browsed library becomes the combined one and catalogs no longer
 * apply.
 */
static grid_settings::CatalogMode asset_grid_membership_mode(
    PointerRNA &settings, const AssetGridSourceParams &params)
{
  if (!string_is_set(params.membership_shelf_idname)) {
    return grid_settings::CatalogMode::All;
  }
  const char *domain = string_or(params.catalog_memory_domain,
                                 grid_settings::id_browser_catalog_memory_domain);
  const grid_settings::CatalogMode mode = grid_settings::catalog_mode_get(settings, domain);
  return ELEM(mode, grid_settings::CatalogMode::Recent, grid_settings::CatalogMode::Favorites) ?
             mode :
             grid_settings::CatalogMode::All;
}

AssetLibraryReference asset_grid_library_from_settings(PointerRNA &settings,
                                                       const AssetGridSourceParams &params)
{
  if (asset_grid_membership_mode(settings, params) != grid_settings::CatalogMode::All) {
    return asset_system::all_library_reference();
  }
  return grid_settings::library_ref_get(settings);
}

std::unique_ptr<AssetGridDataSource> asset_grid_source_from_settings(
    PointerRNA &settings,
    const AssetGridSourceParams &params,
    AssetLibraryReference *r_library_ref)
{
  const grid_settings::CatalogMode mode = asset_grid_membership_mode(settings, params);
  const bool is_membership = mode != grid_settings::CatalogMode::All;
  /* A host that keeps its catalog filter apart from its Recent/Favorites history names a separate
   * domain; otherwise the two share one entry. */
  const char *filter_domain = string_or(
      params.catalog_filter_domain,
      string_or(params.catalog_memory_domain, grid_settings::id_browser_catalog_memory_domain));
  const std::string membership_shelf = is_membership ?
                                           string_or_empty(params.membership_shelf_idname) :
                                           std::string();
  const bool membership_is_favorites = mode == grid_settings::CatalogMode::Favorites;
  const AssetLibraryReference lib_ref = asset_grid_library_from_settings(settings, params);

  if (r_library_ref != nullptr) {
    *r_library_ref = lib_ref;
  }

  /* The catalog selector popover writes UUIDs into the preferences catalog memory, so that is the
   * authority here. #GridViewSettings.enabled_catalogs stays as the fallback for callers that set
   * it from Python, and while a membership mode is active catalogs do not apply at all. */
  Set<std::string> catalog_paths;
  if (!is_membership) {
    catalog_paths = catalog_memory_enabled_paths(lib_ref, filter_domain);
    if (catalog_paths.is_empty()) {
      catalog_paths = grid_settings::enabled_catalogs_get(settings);
    }
  }

  AssetGridDataSourceConfig config;
  config.library_ref = lib_ref;
  config.enabled_catalogs = std::move(catalog_paths);
  config.filter_id_types = grid_settings::filter_id_types_get(settings);
  config.name_match = grid_settings::name_match_filter_get(settings);
  config.search = grid_settings::filter_search_get(settings);
  config.active_identifier = string_or_empty(params.active_identifier);
  config.membership_shelf_idname = membership_shelf;
  config.membership_is_favorites = membership_is_favorites;
  config.item.activate_operator = string_or_empty(params.activate_operator);
  config.item.drag_operator = string_or_empty(params.drag_operator);
  config.item.activate_context_id = string_or_empty(params.activate_context_id);
  config.item.use_drag = params.use_drag;
  config.item.show_names = grid_settings::show_names_get(settings);

  return std::make_unique<AssetGridDataSource>(std::move(config));
}

void AssetGridDataSource::build_window(const bContext &C,
                                       AbstractGridView &view,
                                       const IndexRange window)
{
  this->build_window_and_count(C, view, window);
}

int AssetGridDataSource::build_window_and_count(const bContext &C,
                                                AbstractGridView &view,
                                                const IndexRange window)
{
  const AssetLibraryReference listed_ref = this->listed_library_ref();
  ed::asset::list::storage_fetch(&listed_ref, &C);
  if (!ed::asset::list::is_loaded(&listed_ref)) {
    /* The fetch above only starts the read. Building from a list still being read would sort
     * entries whose catalog data is not filled in yet; see #item_count. */
    return 0;
  }

  int count = 0;
  this->foreach_filtered(
      [&](asset_system::AssetRepresentation &asset, const int filtered_index) {
        if (window.contains(filtered_index)) {
          const StringRef identifier = asset.library_relative_identifier();
          const bool is_active = !config_.active_identifier.empty() &&
                                 identifier == config_.active_identifier;
          view.add_item<AssetGridItem>(
              asset, listed_ref, identifier, asset.get_name(), is_active, config_.item);
        }
        count++;
        return true;
      });
  return count;
}

/** \} */

}  // namespace blender::ui
