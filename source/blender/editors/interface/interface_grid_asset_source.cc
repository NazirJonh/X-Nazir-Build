/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "interface_grid_view_sources.hh"

#include "BKE_name_matching.hh"

#include "DNA_userdef_types.h"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_vector.hh"

#include "DNA_asset_types.h"
#include "DNA_ID.h"

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
  std::string drag_operator_;

 public:
  AssetGridItem(asset_system::AssetRepresentation &asset,
                const AssetLibraryReference &library_ref,
                StringRef identifier,
                StringRef label,
                StringRef activate_operator,
                StringRef drag_operator,
                const bool is_active)
      : PreviewGridItem(identifier, label, ICON_NONE),
        asset_(&asset),
        library_ref_(library_ref),
        activate_operator_(activate_operator),
        drag_operator_(drag_operator)
  {
    if (is_active) {
      this->set_is_active_fn([]() { return true; });
    }
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
  return std::make_unique<AssetGridItemDragController>(
      this->get_view(), *asset_, drag_operator_);
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
/** \name Name-match filter helpers
 * \{ */

static bool asset_passes_name_match(const NameMatchResolvedFilter &resolved,
                                    const asset_system::AssetRepresentation &asset)
{
  Vector<StringRef> metadata_tags;
  for (const AssetTag &tag : asset.get_metadata().tags) {
    metadata_tags.append(tag.name);
  }
  return BKE_name_match_resolved_asset_passes(resolved, asset.get_name(), metadata_tags);
}

static bool asset_grid_extra_poll(const asset_system::AssetRepresentation &asset,
                                  const Set<short> &filter_id_types,
                                  const NameMatchResolvedFilter &name_match_resolved)
{
  if (!filter_id_types.is_empty() && !filter_id_types.contains(asset.get_id_type())) {
    return false;
  }
  return asset_passes_name_match(name_match_resolved, asset);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name AssetGridDataSource
 * \{ */

AssetGridDataSource::AssetGridDataSource(const AssetLibraryReference &library_ref,
                                         Set<std::string> enabled_catalogs,
                                         Set<short> filter_id_types,
                                         NameMatchFilterState name_match,
                                         std::string activate_operator,
                                         std::string drag_operator,
                                         std::string active_identifier)
    : library_ref_(library_ref),
      enabled_catalogs_(std::move(enabled_catalogs)),
      filter_id_types_(std::move(filter_id_types)),
      name_match_(std::move(name_match)),
      activate_operator_(std::move(activate_operator)),
      drag_operator_(std::move(drag_operator)),
      active_identifier_(std::move(active_identifier))
{
}

int AssetGridDataSource::item_count(const bContext & /*C*/) const
{
  const NameMatchResolvedFilter name_match_resolved = BKE_name_match_filter_resolve(name_match_,
                                                                                    U);
  int count = 0;
  ed::asset::foreach_filtered_asset(
      library_ref_,
      enabled_catalogs_.is_empty() ? nullptr : &enabled_catalogs_,
      ed::asset::CatalogContainment::IncludeChildren,
      [&](const asset_system::AssetRepresentation &asset) {
        return asset_grid_extra_poll(asset, filter_id_types_, name_match_resolved);
      },
      [&](asset_system::AssetRepresentation & /*asset*/, int /*filtered_index*/) {
        count++;
        return true;
      });
  return count;
}

bool AssetGridDataSource::item_count_ready(const bContext & /*C*/) const
{
  return ed::asset::list::is_loaded(&library_ref_);
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
  ed::asset::list::storage_fetch(&library_ref_, &C);

  const NameMatchResolvedFilter name_match_resolved = BKE_name_match_filter_resolve(name_match_,
                                                                                    U);
  int count = 0;
  ed::asset::foreach_filtered_asset(
      library_ref_,
      enabled_catalogs_.is_empty() ? nullptr : &enabled_catalogs_,
      ed::asset::CatalogContainment::IncludeChildren,
      [&](const asset_system::AssetRepresentation &asset) {
        return asset_grid_extra_poll(asset, filter_id_types_, name_match_resolved);
      },
      [&](asset_system::AssetRepresentation &asset, const int filtered_index) {
        if (window.contains(filtered_index)) {
          const StringRef identifier = asset.library_relative_identifier();
          const bool is_active = !active_identifier_.empty() && identifier == active_identifier_;
          view.add_item<AssetGridItem>(asset,
                                       library_ref_,
                                       identifier,
                                       asset.get_name(),
                                       activate_operator_,
                                       drag_operator_,
                                       is_active);
        }
        count++;
        return true;
      });
  return count;
}

/** \} */

}  // namespace blender::ui
