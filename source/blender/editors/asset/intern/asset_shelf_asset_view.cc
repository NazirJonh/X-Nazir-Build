/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Grid-view showing all assets according to the giving shelf-type and settings.
 */

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_asset_edit.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_screen.hh"

#include "BLI_fnmatch.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_screen_types.h"

#include "ED_asset.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_asset_shelf.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"

#include "asset_shelf.hh"

namespace blender::ed::asset::shelf {

class AssetView : public ui::AbstractGridView {
  const AssetLibraryReference library_ref_;
  const AssetShelf &shelf_;
  std::optional<AssetWeakReference> active_asset_;
  std::optional<asset_system::AssetCatalogFilter> catalog_filter_ = std::nullopt;

  friend class AssetViewItem;
  friend class AssetDragController;

 public:
  AssetView(const AssetLibraryReference &library_ref,
            const AssetShelf &shelf,
            const std::optional<AssetWeakReference> &active_asset_override);

  void build_items() override;
  bool begin_filtering(const bContext &C) const override;

  void set_catalog_filter(const std::optional<asset_system::AssetCatalogFilter> &catalog_filter);
};

class AssetViewItem : public ui::PreviewGridItem {
  asset_system::AssetRepresentation &asset_;
  bool allow_asset_drag_ = true;
  /* Cached from layout when building image-texture shelf tiles (see #build_grid_tile). */
  mutable uint32_t image_texture_brush_session_uid_ = MAIN_ID_SESSION_UID_UNSET;
  mutable bool image_texture_use_mask_slot_ = false;

 public:
  AssetViewItem(asset_system::AssetRepresentation &asset_, StringRef identifier, StringRef label);

  void disable_asset_drag();
  void build_grid_tile(const bContext &C, ui::Layout &layout) const override;
  void build_context_menu(bContext &C, ui::Layout &column) const override;
  std::optional<bool> should_be_active() const override;
  void on_activate(bContext &C) override;
  bool should_be_filtered_visible(StringRefNull filter_string) const override;

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override;
};

class AssetDragController : public ui::AbstractViewItemDragController {
  asset_system::AssetRepresentation &asset_;

 public:
  AssetDragController(ui::AbstractGridView &view, asset_system::AssetRepresentation &asset);

  std::optional<eWM_DragDataType> get_drag_type() const override;
  void *create_drag_data() const override;
  void on_drag_start(bContext &C, ui::AbstractViewItem &item) override;
};

static std::optional<AssetWeakReference> active_asset_for_shelf(const AssetShelf &shelf,
                                                                const bContext &C)
{
  std::optional<AssetWeakReference> active;
  if (shelf.type->get_active_asset_from_context) {
    if (const AssetWeakReference *weak_ref = shelf.type->get_active_asset_from_context(shelf.type,
                                                                                       &C))
    {
      active = *weak_ref;
    }
  }
  if (!active && shelf.type->get_active_asset) {
    if (const AssetWeakReference *weak_ref = shelf.type->get_active_asset(shelf.type)) {
      active = *weak_ref;
    }
  }
  if (!active) {
    return std::nullopt;
  }

  /* The active datablock may be a local copy made local from an asset (e.g. a brush localized to
   * receive a texture), whose local reference does not match the source library item shown in the
   * shelf. Map it back to the source asset so that item is still highlighted as active. */
  if (active->asset_library_type == ASSET_LIBRARY_LOCAL) {
    if (std::optional<AssetWeakReference> source = bke::asset_edit_local_to_source_weak_reference(
            *CTX_data_main(&C), *active))
    {
      return source;
    }
  }
  return active;
}

AssetView::AssetView(const AssetLibraryReference &library_ref,
                     const AssetShelf &shelf,
                     const std::optional<AssetWeakReference> &active_asset_override)
    : library_ref_(library_ref), shelf_(shelf)
{
  if (active_asset_override) {
    active_asset_ = *active_asset_override;
  }
}

void AssetView::build_items()
{
  const asset_system::AssetLibrary *library = list::library_get_once_available(library_ref_);
  if (!library) {
    return;
  }

  list::iterate(library_ref_, [&](asset_system::AssetRepresentation &asset) {
    if (!shelf::type_asset_poll(*shelf_.type, asset)) {
      /* Skip this asset. */
      return true;
    }

    const AssetMetaData &asset_data = asset.get_metadata();
    if (catalog_filter_ && !catalog_filter_->contains(asset_data.catalog_id)) {
      /* Skip this asset. */
      return true;
    }

    const bool show_names = (shelf_.settings.display_flag & ASSETSHELF_SHOW_NAMES);
    const StringRef identifier = asset.library_relative_identifier();

    AssetViewItem &item = this->add_item<AssetViewItem>(asset, identifier, asset.get_name());
    if (!show_names) {
      item.hide_label();
    }
    if (shelf_.type->flag & ASSET_SHELF_TYPE_FLAG_NO_ASSET_DRAG) {
      item.disable_asset_drag();
    }
    /* Activate on click (release) rather than press so that LMB drag-scroll can intercept the
     * gesture before selection triggers. Without this, popovers activate view items on press via
     * #handle_view_item_event (unconditionally, before drag is detectable). Matches the image-grid
     * template pattern (#select_on_click_set + #always_reactivate_on_click). */
    item.select_on_click_set();
    /* Make sure every click calls the #bl_activate_operator. We might want to add a flag to
     * enable/disable this. Or we only call #bl_activate_operator when an item becomes active, and
     * add a #bl_click_operator for repeated execution on every click. So far it seems like every
     * asset shelf use case works with activating on every click though. */
    item.always_reactivate_on_click();
    if (shelf_.type->flag & ASSET_SHELF_TYPE_FLAG_ACTIVATE_FOR_CONTEXT_MENU) {
      item.activate_for_context_menu_set();
    }

    return true;
  });
}

bool AssetView::begin_filtering(const bContext &C) const
{
  const ScrArea *area = CTX_wm_area(&C);
  for (ARegion &region : area->regionbase) {
    if (ui::textbutton_activate_rna(&C, &region, &shelf_, "search_filter")) {
      return true;
    }
  }

  return false;
}

void AssetView::set_catalog_filter(
    const std::optional<asset_system::AssetCatalogFilter> &catalog_filter)
{
  if (catalog_filter) {
    catalog_filter_.emplace(*catalog_filter);
  }
  else {
    catalog_filter_ = std::nullopt;
  }
}

static std::optional<asset_system::AssetCatalogFilter> catalog_filter_from_shelf_settings(
    const AssetShelfSettings &shelf_settings, const asset_system::AssetLibrary &library)
{
  if (!shelf_settings.active_catalog_path) {
    return {};
  }

  asset_system::AssetCatalog *active_catalog = library.catalog_service().find_catalog_by_path(
      shelf_settings.active_catalog_path);
  if (!active_catalog) {
    return {};
  }

  return library.catalog_service().create_catalog_filter(active_catalog->catalog_id);
}

/* ---------------------------------------------------------------------- */

AssetViewItem::AssetViewItem(asset_system::AssetRepresentation &asset,
                             StringRef identifier,
                             StringRef label)
    : ui::PreviewGridItem(identifier, label, ICON_NONE), asset_(asset)
{
}

void AssetViewItem::disable_asset_drag()
{
  allow_asset_drag_ = false;
}

/**
 * Read brush target from layout context so activation does not depend on a live UI context store
 * (popover refresh can invalidate context inherited from the opening button).
 */
static void image_texture_shelf_brush_target_from_layout(const ui::Layout &layout,
                                                         uint32_t &brush_session_uid,
                                                         bool &use_mask_slot)
{
  brush_session_uid = MAIN_ID_SESSION_UID_UNSET;
  use_mask_slot = false;

  const PointerRNA *target_ptr = layout.context_ptr_get("image_grid_target", nullptr);
  if (!target_ptr || !target_ptr->data || !target_ptr->owner_id) {
    return;
  }
  if (GS(target_ptr->owner_id->name) != ID_BR) {
    return;
  }
  brush_session_uid = target_ptr->owner_id->session_uid;
  use_mask_slot = layout.context_int_get("image_grid_is_mask_slot").value_or(0) != 0;
}

/**
 * Needs freeing with #WM_operator_properties_free() (will be done by button if passed to that) and
 * #MEM_delete().
 */
static std::optional<wmOperatorCallParams> create_asset_operator_params(
    const StringRefNull op_name,
    const asset_system::AssetRepresentation &asset,
    const uint32_t brush_session_uid = MAIN_ID_SESSION_UID_UNSET,
    const bool use_mask_slot = false)
{
  if (op_name.is_empty()) {
    return {};
  }
  wmOperatorType *ot = WM_operatortype_find(op_name.c_str(), true);
  if (!ot) {
    return {};
  }

  PointerRNA *op_props = MEM_new<PointerRNA>(__func__, WM_operator_properties_create_ptr(ot));
  asset::operator_asset_reference_props_set(asset, *op_props);
  if (brush_session_uid != MAIN_ID_SESSION_UID_UNSET) {
    RNA_int_set(op_props, "brush_session_uid", int(brush_session_uid));
    RNA_boolean_set(op_props, "use_mask_slot", use_mask_slot);
  }
  return wmOperatorCallParams{ot, op_props, wm::OpCallContext::InvokeRegionWin};
}

void AssetViewItem::build_grid_tile(const bContext &C, ui::Layout &layout) const
{
  const AssetView &asset_view = reinterpret_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  PointerRNA asset_ptr = RNA_pointer_create_discrete(nullptr, RNA_AssetRepresentation, &asset_);
  button_context_ptr_set(
      layout.block(), reinterpret_cast<ui::Button *>(view_item_but_), "asset", &asset_ptr);

  ui::Button *item_but = reinterpret_cast<ui::Button *>(this->view_item_button());
  if (STREQ(shelf_type.idname, "VIEW3D_AST_image_texture")) {
    image_texture_shelf_brush_target_from_layout(
        layout, image_texture_brush_session_uid_, image_texture_use_mask_slot_);
  }
  if (std::optional<wmOperatorCallParams> activate_op = create_asset_operator_params(
          shelf_type.activate_operator,
          asset_,
          image_texture_brush_session_uid_,
          image_texture_use_mask_slot_))
  {
    /* Attach the operator, but don't call it through the button. We call it using
     * #on_activate(). */
    button_operator_set(item_but, activate_op->optype, activate_op->opcontext, activate_op->opptr);
    button_operator_set_never_call(item_but);

    MEM_delete(activate_op->opptr);
  }
  const ui::GridViewStyle &style = this->get_view().get_style();
  /* Increase background draw size slightly, so highlights are well visible behind previews with an
   * opaque background. */
  button_view_item_draw_size_set(
      item_but, style.tile_width + 2 * U.pixelsize, style.tile_height + 2 * U.pixelsize);

  button_func_tooltip_custom_set(
      item_but,
      [](bContext & /*C*/, ui::TooltipData &tip, ui::Button * /*but*/, void *argN) {
        const asset_system::AssetRepresentation *asset =
            static_cast<const asset_system::AssetRepresentation *>(argN);
        asset_tooltip(*asset, tip);
      },
      (&asset_),
      nullptr);

  /* Request preview when drawing. Grid views have an optimization to only draw items that are
   * actually visible, so only previews scrolled into view will be loaded this way. This reduces
   * total loading time and memory footprint. */
  asset_.ensure_previewable(C);

  const int preview_id = [&]() -> int {
    /* Show loading icon while list is loading still. Previews might get pushed out of view again
     * while the list grows, which can cause a lot of flickering. Note that this also means the
     * actual loading of previews is delayed, because that only happens when a preview icon-ID is
     * attached to a button. */
    if (!list::is_loaded(&asset_view.library_ref_)) {
      return ICON_PREVIEW_LOADING;
    }
    return asset_preview_or_icon(asset_);
  }();

  ui::GridViewStyle grid_style = asset_view.get_style();
  /* Add overlap layout so indicator icons can be displayed on top of the preview. */
  ui::Layout &overlap = layout.overlap();
  overlap.ui_units_x_set(grid_style.tile_width / UI_UNIT_X);
  overlap.ui_units_y_set(grid_style.tile_height / UI_UNIT_Y);

  ui::PreviewGridItem::build_grid_tile_button(overlap.column(true), preview_id);

  ui::Layout &overlay_row = overlap.row(true);
  overlay_row.alignment_set(ui::LayoutAlign::Right);

  const bool is_highlighted = this->is_selected() || this->is_active() || this->is_hovered();
  if (asset_.is_online() && is_highlighted) {
    ui::Button *online_icon = uiItemL_ex(&overlay_row, "", ICON_INTERNET, false, false);
    button_label_alpha_factor_set(online_icon, 0.6f);
    button_label_draw_icon_border_set(online_icon, true);
  }
}

void AssetViewItem::build_context_menu(bContext &C, ui::Layout &column) const
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  bool has_items = false;

  if (asset_.is_online()) {
    column.op("asset.assets_download", IFACE_("Download Asset"), ICON_NONE);
    has_items = true;
  }

  if (shelf_type.draw_context_menu) {
    if (has_items) {
      column.separator();
    }
    shelf_type.draw_context_menu(&C, &shelf_type, &asset_, column);
    has_items = true;
  }
}

std::optional<bool> AssetViewItem::should_be_active() const
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;
  /* Return no preference when neither the Python callback nor the context-aware C++ callback
   * has provided an active asset. Shelf types that set #get_active_asset_from_context
   * (e.g. VIEW3D_AST_image_texture) store the result in #AssetView::active_asset_ via
   * #active_asset_for_shelf(); honour that here instead of treating it as "no active asset". */
  if (!shelf_type.get_active_asset && !asset_view.active_asset_) {
    return {};
  }
  if (!asset_view.active_asset_) {
    return false;
  }
  const AssetWeakReference item_weak_ref = asset_.make_weak_reference();
  if (*asset_view.active_asset_ == item_weak_ref) {
    return true;
  }
  /* #bke::asset_edit_weak_reference_from_id() uses "Image/<id-name>" while shelf assets use the
   * library-relative path from #AssetRepresentation::make_weak_reference(). Match by local ID
   * name for the image-texture browse popover. */
  if (STREQ(shelf_type.idname, "VIEW3D_AST_image_texture")) {
    if (const ID *local_id = asset_.local_id()) {
      const char *active_identifier = asset_view.active_asset_->relative_asset_identifier;
      if (active_identifier && STRPREFIX(active_identifier, "Image/")) {
        return STREQ(local_id->name + 2, active_identifier + 6);
      }
    }
  }
  return false;
}

void AssetViewItem::on_activate(bContext &C)
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  if (std::optional<wmOperatorCallParams> activate_op = create_asset_operator_params(
          shelf_type.activate_operator,
          asset_,
          image_texture_brush_session_uid_,
          image_texture_use_mask_slot_))
  {
    WM_operator_name_call_ptr(
        &C, activate_op->optype, activate_op->opcontext, activate_op->opptr, nullptr);
    WM_operator_properties_free(activate_op->opptr);
    MEM_delete(activate_op->opptr);
  }
}

bool AssetViewItem::should_be_filtered_visible(const StringRefNull filter_string) const
{
  const StringRefNull asset_name = asset_.get_name();
  return fnmatch(filter_string.c_str(), asset_name.c_str(), FNM_CASEFOLD) == 0;
}

std::unique_ptr<ui::AbstractViewItemDragController> AssetViewItem::create_drag_controller() const
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  if (!allow_asset_drag_ && shelf_type.drag_operator.empty()) {
    return nullptr;
  }
  return std::make_unique<AssetDragController>(this->get_view(), asset_);
}

/* ---------------------------------------------------------------------- */

static std::string filter_string_get(const AssetShelf &shelf)
{
  /* Copy of the filter string from #AssetShelfSettings, with extra '*' added to the beginning and
   * end of the string, for `fnmatch()` to work. */
  char search_string[sizeof(AssetShelfSettings::search_string) + 2];
  BLI_strncpy_ensure_pad(search_string, shelf.settings.search_string, '*', sizeof(search_string));
  return search_string;
}

void build_asset_view(ui::Layout &layout,
                      const AssetLibraryReference &library_ref,
                      const AssetShelf &shelf,
                      const bContext &C)
{
  list::storage_fetch(&library_ref, &C);

  const asset_system::AssetLibrary *library = list::library_get_once_available(library_ref);
  if (!library) {
    return;
  }

  const float tile_width = shelf::tile_width(shelf.settings);
  const float tile_height = shelf::tile_height(shelf.settings);
  BLI_assert(tile_width != 0);
  BLI_assert(tile_height != 0);

  const std::optional<AssetWeakReference> active_asset = active_asset_for_shelf(shelf, C);

  std::unique_ptr asset_view = std::make_unique<AssetView>(library_ref, shelf, active_asset);
  asset_view->set_catalog_filter(catalog_filter_from_shelf_settings(shelf.settings, *library));
  asset_view->set_tile_size(tile_width, tile_height);

  ui::Block *block = layout.block();
  ui::AbstractGridView *grid_view = block_add_view(
      *block, "asset shelf asset view", std::move(asset_view));
  grid_view->set_context_menu_title("Asset Shelf");
  if (STREQ(shelf.type->idname, "VIEW3D_AST_image_texture")) {
    grid_view->scroll_active_into_center_on_draw_ = true;
  }

  ui::GridViewBuilder builder(*block);
  builder.build_grid_view(C, *grid_view, layout, filter_string_get(shelf));
}

/* ---------------------------------------------------------------------- */
/* Dragging. */

AssetDragController::AssetDragController(ui::AbstractGridView &view,
                                         asset_system::AssetRepresentation &asset)
    : ui::AbstractViewItemDragController(view), asset_(asset)
{
}

std::optional<eWM_DragDataType> AssetDragController::get_drag_type() const
{
  const AssetView &asset_view = this->get_view<AssetView>();
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  /* Disable asset dragging, only call #AssetShelfType::drag_operator in #on_drag_start(). */
  if (!shelf_type.drag_operator.empty()) {
    return std::nullopt;
  }
  return asset_.is_local_id() ? WM_DRAG_ID : WM_DRAG_ASSET;
}

void AssetDragController::on_drag_start(bContext &C, ui::AbstractViewItem &item)
{
  const AssetView &asset_view = this->get_view<AssetView>();
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  if (std::optional<wmOperatorCallParams> drag_op = create_asset_operator_params(
          shelf_type.drag_operator, asset_))
  {
    WM_operator_name_call_ptr(&C, drag_op->optype, drag_op->opcontext, drag_op->opptr, nullptr);
    WM_operator_properties_free(drag_op->opptr);
    MEM_delete(drag_op->opptr);

    /* Display as active so it's clear which item is being operated on. #activate() would trigger
     * the activation operator. We really don't want this for poses, since dragging shouldn't fully
     * apply a pose, but trigger interactive pose blending instead.
     *
     * Messing with the active state could cause problems, in that case a separate highlighting
     * feature might make sense (so e.g. dragged from assets get an outline). */
    item.set_state_active();
  }
}

void *AssetDragController::create_drag_data() const
{
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

}  // namespace blender::ed::asset::shelf
