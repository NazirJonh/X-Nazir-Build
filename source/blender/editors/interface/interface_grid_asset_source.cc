/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "interface_grid_view_sources.hh"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_math_base.h"
#include "BLI_vector.hh"

#include "ED_asset.hh"
#include "ED_asset_list.hh"
#include "ED_asset_menu_utils.hh"

#include "RNA_access.hh"

#include "UI_grid_view.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name AssetGridItem — grid tile for a single asset
 * \{ */

class AssetGridItem : public PreviewGridItem {
  asset_system::AssetRepresentation *asset_; /* Not owned; valid for asset-list lifetime. */
  AssetLibraryReference library_ref_;        /* Copied for is_loaded check during draw. */
  std::string activate_operator_;

 public:
  AssetGridItem(asset_system::AssetRepresentation &asset,
                const AssetLibraryReference &library_ref,
                StringRef identifier,
                StringRef label,
                StringRef activate_operator)
      : PreviewGridItem(identifier, label, ICON_NONE),
        asset_(&asset),
        library_ref_(library_ref),
        activate_operator_(activate_operator)
  {
  }

  void build_grid_tile(const bContext &C, Layout &layout) const override;
  void on_activate(bContext &C) override;
};

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
      [](bContext & /*C*/, TooltipData &tip, Button * /*but*/, void *argN) {
        const auto *asset = static_cast<const asset_system::AssetRepresentation *>(argN);
        ed::asset::asset_tooltip(*asset, tip);
      },
      asset_,
      nullptr);
}

void AssetGridItem::on_activate(bContext &C)
{
  if (activate_operator_.empty()) {
    return;
  }
  wmOperatorType *ot = WM_operatortype_find(activate_operator_.c_str(), true);
  if (!ot) {
    return;
  }
  PointerRNA *op_props = MEM_new<PointerRNA>(__func__, WM_operator_properties_create_ptr(ot));
  ed::asset::operator_asset_reference_props_set(*asset_, *op_props);
  WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::InvokeRegionWin, op_props, nullptr);
  WM_operator_properties_free(op_props);
  MEM_delete(op_props);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Catalog-filter helpers
 * \{ */

static Vector<asset_system::AssetCatalogFilter> build_catalog_filters(
    const Set<std::string> &enabled_catalogs, const asset_system::AssetLibrary &library)
{
  Vector<asset_system::AssetCatalogFilter> filters;
  filters.reserve(enabled_catalogs.size());
  for (const std::string &path : enabled_catalogs) {
    asset_system::AssetCatalog *catalog = library.catalog_service().find_catalog_by_path(
        path.c_str());
    if (catalog) {
      filters.append(library.catalog_service().create_catalog_filter(catalog->catalog_id));
    }
  }
  return filters;
}

static bool asset_passes_catalog_filter(
    const asset_system::AssetRepresentation &asset,
    const Vector<asset_system::AssetCatalogFilter> &filters)
{
  for (const asset_system::AssetCatalogFilter &f : filters) {
    if (f.contains(asset.get_metadata().catalog_id)) {
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name AssetGridDataSource
 * \{ */

AssetGridDataSource::AssetGridDataSource(const AssetLibraryReference &library_ref,
                                         Set<std::string> enabled_catalogs,
                                         std::string activate_operator)
    : library_ref_(library_ref),
      enabled_catalogs_(std::move(enabled_catalogs)),
      activate_operator_(std::move(activate_operator))
{
}

int AssetGridDataSource::item_count(const bContext &C) const
{
  ed::asset::list::storage_fetch(&library_ref_, &C);

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      library_ref_);
  if (!library) {
    return 0;
  }

  const bool filter_enabled = !enabled_catalogs_.is_empty();
  const Vector<asset_system::AssetCatalogFilter> filters = filter_enabled ?
                                                               build_catalog_filters(
                                                                   enabled_catalogs_, *library) :
                                                               Vector<asset_system::AssetCatalogFilter>{};

  int count = 0;
  ed::asset::list::iterate(library_ref_, [&](asset_system::AssetRepresentation &asset) -> bool {
    if (filter_enabled) {
      if (filters.is_empty() || !asset_passes_catalog_filter(asset, filters)) {
        return true;
      }
    }
    count++;
    return true;
  });
  return count;
}

void AssetGridDataSource::build_window(const bContext &C,
                                       AbstractGridView &view,
                                       const IndexRange window)
{
  ed::asset::list::storage_fetch(&library_ref_, &C);

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      library_ref_);
  if (!library) {
    return;
  }

  const bool filter_enabled = !enabled_catalogs_.is_empty();
  const Vector<asset_system::AssetCatalogFilter> filters = filter_enabled ?
                                                               build_catalog_filters(
                                                                   enabled_catalogs_, *library) :
                                                               Vector<asset_system::AssetCatalogFilter>{};

  int filtered_index = 0;
  ed::asset::list::iterate(library_ref_, [&](asset_system::AssetRepresentation &asset) -> bool {
    if (filter_enabled) {
      if (filters.is_empty() || !asset_passes_catalog_filter(asset, filters)) {
        return true;
      }
    }

    if (window.contains(filtered_index)) {
      view.add_item<AssetGridItem>(asset,
                                   library_ref_,
                                   asset.library_relative_identifier(),
                                   asset.get_name(),
                                   activate_operator_);
    }

    filtered_index++;
    /* Stop iterating once the window is fully built. */
    return filtered_index < int(window.one_after_last());
  });
}

void AssetGridDataSource::item_activate(bContext & /*C*/, const StringRef /*identifier*/)
{
  /* Activation is handled per-item in AssetGridItem::on_activate. */
}

void AssetGridDataSource::item_build_tooltip(const bContext & /*C*/,
                                              StringRef /*identifier*/,
                                              TooltipData & /*tip*/) const
{
  /* Tooltip is built per-item in AssetGridItem::build_grid_tile. */
}

/** \} */

}  // namespace blender::ui
