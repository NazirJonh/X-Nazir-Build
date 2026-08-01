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
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include <algorithm>

#include <fmt/format.h>

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
#include "WM_types.hh"

#include "asset_shelf.hh"
#include "asset_shelf_asset_lists.hh"

namespace blender::ed::asset::shelf {

class AssetView : public ui::AbstractGridView {
  const AssetLibraryReference library_ref_;
  const AssetShelf &shelf_;
  std::optional<AssetWeakReference> active_asset_;
  std::optional<asset_system::AssetCatalogFilter> catalog_filter_ = std::nullopt;
  /** Members of the active Recent/Favorites pseudo-catalog, in the list's own order (most recently
   * used first, respectively insertion order). #build_items() reproduces that order in the grid,
   * so the container has to keep it -- an unordered #Set would leave the tiles in library order. */
  std::optional<VectorSet<ShelfAssetRef>> pseudo_filter_ = std::nullopt;
  /** Favorites of this shelf, looked up once per rebuild. Drives both the "Only Favorites" header
   * toggle and the per-tile favorite star, so neither has to look up the shelf's lists per asset.
   * Unset for shelves without the favorites feature (only brush shelves have it). */
  std::optional<Set<ShelfAssetRef>> favorites_ = std::nullopt;

  friend class AssetViewItem;
  friend class AssetDragController;
  friend class AssetReorderDragController;

 public:
  AssetView(const AssetLibraryReference &library_ref,
            const AssetShelf &shelf,
            const std::optional<AssetWeakReference> &active_asset_override);

  void build_items() override;
  bool begin_filtering(const bContext &C) const override;

  void set_catalog_filter(const std::optional<asset_system::AssetCatalogFilter> &catalog_filter);
  void set_pseudo_filter(std::optional<VectorSet<ShelfAssetRef>> pseudo_filter);
  void set_favorites(std::optional<Set<ShelfAssetRef>> favorites);

 private:
  void add_asset_item(asset_system::AssetRepresentation &asset, bool is_favorite);
};

class AssetViewItem : public ui::PreviewGridItem {
  asset_system::AssetRepresentation &asset_;
  bool allow_asset_drag_ = true;
  /* Cached from layout when building image-texture shelf tiles (see #build_grid_tile). */
  mutable uint32_t image_texture_brush_session_uid_ = MAIN_ID_SESSION_UID_UNSET;
  mutable bool image_texture_use_mask_slot_ = false;

  /** Whether this asset is in the shelf's Favorites list. Resolved once per rebuild (see
   * #AssetView::build_items()), so drawing a tile doesn't have to derive the asset's identity
   * again. Always false on shelves without the favorites feature. */
  bool is_favorite_ = false;

 public:
  AssetViewItem(asset_system::AssetRepresentation &asset_, StringRef identifier, StringRef label);

  void disable_asset_drag();
  void set_favorite(bool is_favorite);
  void build_grid_tile(const bContext &C, ui::Layout &layout) const override;
  void build_context_menu(bContext &C, ui::Layout &column) const override;
  std::optional<bool> should_be_active() const override;
  void on_activate(bContext &C) override;
  bool should_be_filtered_visible(StringRefNull filter_string) const override;

  bool supports_drag() const override;
  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override;
  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller(
      const wmEvent *event) const override;
  std::unique_ptr<ui::GridViewItemDropTarget> create_drop_target() override;
};

class AssetDragController : public ui::AbstractViewItemDragController {
  asset_system::AssetRepresentation &asset_;

 public:
  AssetDragController(ui::AbstractGridView &view, asset_system::AssetRepresentation &asset);

  std::optional<eWM_DragDataType> get_drag_type() const override;
  void *create_drag_data() const override;
  void on_drag_start(bContext &C, ui::AbstractViewItem &item) override;
};

class AssetReorderDragController : public ui::AbstractViewItemDragController {
  asset_system::AssetRepresentation &asset_;
  std::string shelf_idname_;

 public:
  AssetReorderDragController(ui::AbstractGridView &view,
                             asset_system::AssetRepresentation &asset,
                             const char *shelf_idname);

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

void AssetView::add_asset_item(asset_system::AssetRepresentation &asset, const bool is_favorite)
{
  const bool show_names = (shelf_.settings.display_flag & ASSETSHELF_SHOW_NAMES);
  const StringRef identifier = asset.library_relative_identifier();

  AssetViewItem &item = this->add_item<AssetViewItem>(asset, identifier, asset.get_name());
  item.set_favorite(is_favorite);
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
  if (shelf_.type->flag & ASSET_SHELF_TYPE_FLAG_ACTIVATE_FOR_CONTEXT_MENU &&
      !asset.is_online_only())
  {
    item.activate_for_context_menu_set();
  }
}

void AssetView::build_items()
{
  const asset_system::AssetLibrary *library = list::library_get_once_available(library_ref_);
  if (!library) {
    return;
  }

  /* Pseudo-catalog (Recent/Favorites): the tiles must follow the list's order, not the library's
   * iteration order. Matching assets are parked at their position in the list here and added in a
   * second pass below. The favorite state is parked with them so the second pass doesn't have to
   * derive the asset's identity a second time. */
  struct PseudoCatalogEntry {
    asset_system::AssetRepresentation *asset = nullptr;
    bool is_favorite = false;
  };
  Vector<PseudoCatalogEntry> pseudo_ordered_assets;
  if (pseudo_filter_) {
    pseudo_ordered_assets.resize(pseudo_filter_->size());
  }

  const bool favorites_only = (shelf_.settings.display_flag & ASSETSHELF_FILTER_FAVORITES_ONLY) !=
                              0;

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

    /* Deriving the asset's identity allocates and copies its two identifier strings, so pay for it
     * only once, and only when the shelf has favorites or one of the identity-based filters is
     * active (neither is, for a non-brush shelf). */
    if (pseudo_filter_ || favorites_) {
      const ShelfAssetRef ref = ShelfAssetRef::from_weak_reference(asset.make_weak_reference());
      const bool is_favorite = favorites_ && favorites_->contains(ref);
      if (favorites_only && !is_favorite) {
        /* Skip this asset: header "Only Favorites" toggle is active and it isn't a favorite. */
        return true;
      }
      if (pseudo_filter_) {
        const int64_t index = pseudo_filter_->index_of_try(ref);
        if (index < 0) {
          /* Skip this asset: not in the active Recent/Favorites pseudo-catalog. */
          return true;
        }
        pseudo_ordered_assets[index] = {&asset, is_favorite};
        return true;
      }
      this->add_asset_item(asset, is_favorite);
      return true;
    }

    this->add_asset_item(asset, false);
    return true;
  });

  for (const int i : pseudo_ordered_assets.index_range()) {
    const PseudoCatalogEntry &entry = pseudo_ordered_assets[i];
    /* Null for list entries the library doesn't contain (any more): removed, renamed, or from a
     * library that isn't loaded in this session. */
    if (entry.asset) {
      this->add_asset_item(*entry.asset, entry.is_favorite);
    }
  }
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

void AssetView::set_pseudo_filter(std::optional<VectorSet<ShelfAssetRef>> pseudo_filter)
{
  pseudo_filter_ = std::move(pseudo_filter);
}

void AssetView::set_favorites(std::optional<Set<ShelfAssetRef>> favorites)
{
  favorites_ = std::move(favorites);
}

static std::optional<asset_system::AssetCatalogFilter> catalog_filter_from_shelf_settings(
    const AssetShelfSettings &shelf_settings, const asset_system::AssetLibrary &library)
{
  if (!shelf_settings.active_catalog_path) {
    return {};
  }
  /* Sentinel values for pseudo-catalogs (Recent/Favorites) must not be forwarded to the real
   * catalog filter; the pseudo_filter handles them instead. */
  if (settings_is_recent_catalog_active(shelf_settings) ||
      settings_is_favorites_catalog_active(shelf_settings))
  {
    return {};
  }

  asset_system::AssetCatalog *active_catalog = library.catalog_service().find_catalog_by_path(
      shelf_settings.active_catalog_path);
  if (!active_catalog) {
    return {};
  }

  return library.catalog_service().create_catalog_filter(active_catalog->catalog_id);
}

static std::optional<VectorSet<ShelfAssetRef>> pseudo_filter_from_shelf_settings(
    const AssetShelfSettings &shelf_settings, const StringRef shelf_idname)
{
  Span<ShelfAssetRef> refs;
  if (shelf::settings_is_recent_catalog_active(shelf_settings)) {
    refs = shelf::shelf_asset_lists_recent(shelf_idname);
  }
  else if (shelf::settings_is_favorites_catalog_active(shelf_settings)) {
    refs = shelf::shelf_asset_lists_favorites(shelf_idname);
  }
  else {
    return std::nullopt;
  }

  /* A #VectorSet keeps the list's order (which the grid reproduces, see #AssetView::build_items())
   * alongside the constant-time membership lookup the per-asset filtering needs. */
  VectorSet<ShelfAssetRef> filter;
  filter.add_multiple(refs);
  return filter;
}

/**
 * The favorites of \a shelf_idname, or nothing for a shelf that doesn't support asset lists.
 * Built once per grid rebuild, for both the "Only Favorites" filter and the per-tile star;
 * #shelf_asset_lists_is_favorite() would look up the shelf's lists again for every single asset.
 */
static std::optional<Set<ShelfAssetRef>> favorites_from_shelf(const StringRef shelf_idname)
{
  if (!shelf::shelf_supports_asset_lists(shelf_idname)) {
    return std::nullopt;
  }

  Set<ShelfAssetRef> favorites;
  favorites.add_multiple(shelf::shelf_asset_lists_favorites(shelf_idname));
  return favorites;
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

void AssetViewItem::set_favorite(const bool is_favorite)
{
  is_favorite_ = is_favorite;
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

/**
 * Size multiplier for the tile's favorite star, relative to a default-sized icon.
 *
 * Scales with preview size: half size at the minimum preview (24 px), default at the default
 * preview size (48 px), and up to 1.5× for larger previews. Past that the star would compete with
 * the preview itself for attention.
 */
static float favorite_icon_scale(const int preview_size)
{
  return std::clamp(preview_size / float(ASSET_SHELF_PREVIEW_SIZE_DEFAULT), 0.5f, 1.5f);
}

void AssetViewItem::build_grid_tile(const bContext &C, ui::Layout &layout) const
{
  const AssetView &asset_view = reinterpret_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  PointerRNA asset_ptr = RNA_pointer_create_discrete(nullptr, RNA_AssetRepresentation, &asset_);
  button_context_ptr_set(
      layout.block(), reinterpret_cast<ui::Button *>(view_item_but_), "asset", &asset_ptr);

  ui::Button *item_but = reinterpret_cast<ui::Button *>(this->view_item_button());
  if (shelf_type.grid_tile_activate_extra_params) {
    shelf_type.grid_tile_activate_extra_params(
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

  /* #LayoutOverlap anchors its children to the top and the overlay row right-aligns them, which
   * leaves the overlays flush against two edges of the tile. Inset them from the corner by a
   * fraction of their own size, so the gap reads the same at every preview size.
   *
   * Both the padding and the icon scale are computed here rather than next to the star, because the
   * star is only built while the tile is hovered (see below) -- deriving the row's padding from it
   * would shift the indicator icons on every hover. */
  const float overlay_icon_scale = favorite_icon_scale(asset_view.shelf_.settings.preview_size);
  const short overlay_pad = short(ICON_DEFAULT_WIDTH_SCALE * overlay_icon_scale * 0.2f);

  ui::Layout &overlay_column = overlap.column(false);
  ui::Block *overlay_block = overlay_column.block();

  /* Empty label acting as a spacer; #Layout::separator() is unusable here, as it draws a line
   * rather than a gap in menu blocks (the popover is one), and its step differs between menu and
   * non-menu blocks, which would desync the popover from the shelf region. */
  ui::block_layout_set_current(overlay_block, &overlay_column);
  ui::uiDefBut(
      overlay_block, ui::ButtonType::Label, "", 0, 0, 1, overlay_pad, nullptr, 0, 0, std::nullopt);

  ui::Layout &overlay_row = overlay_column.row(true);
  overlay_row.alignment_set(ui::LayoutAlign::Right);

  if (asset_.is_online_only()) {
    ui::Button *online_icon = uiItemL_ex(&overlay_row, "", ICON_INTERNET, false, false);
    button_label_alpha_factor_set(online_icon, 0.6f);
    button_label_draw_icon_border_set(online_icon, true);
  }
  else if (asset_.needs_download()) {
    ui::Button *needs_download_icon = uiItemL_ex(&overlay_row, "", ICON_ERROR, false, false);
    button_label_alpha_factor_set(needs_download_icon, 0.6f);
    button_label_draw_icon_border_set(needs_download_icon, true);
  }

  /* Favorite toggle. Only shelves with a favorites list get one (#AssetView::favorites_ is unset
   * for the others). Unlike the indicator icons above this is a real button, not a label, so that
   * it is clickable at all (#button_is_interactive_ex() ignores non-draggable labels). It is
   * defined after the tile's view-item button, which is what makes the two coexist: hit-testing
   * walks the block back to front (#button_find_mouse_over_ex), so a press inside the star goes to
   * the star and everywhere else to the tile. For the same reason the grid's touch drag-scroll
   * doesn't arm on it, as that only claims presses landing on a view item (#grid_hit_press()).
   *
   * A favorite always shows its (filled) star so the state is visible at a glance; a non-favorite
   * only reveals the (hollow) star while its tile is hovered, matching the download button below.
   * This keeps the grid uncluttered but still lets the user favorite a brush on demand. */
  const bool show_favorite_icon = (asset_view.shelf_.settings.display_flag &
                                   ASSETSHELF_HIDE_FAVORITE_ICON) == 0;
  if (asset_view.favorites_ && show_favorite_icon && (is_favorite_ || is_hovered())) {
    /* Scaling the button rect alone would only center a default-sized icon in a bigger button, so
     * the icon is scaled separately below. The rect grows along with it to keep the icon's padding
     * proportional, and to give the star a hit area that matches what is drawn.
     *
     * #uiDefIconButO appends to the block's *current* layout; make that the overlay row. */
    ui::block_layout_set_current(overlay_block, &overlay_row);
    ui::Button *favorite_but = uiDefIconButO(
        overlay_block,
        ui::ButtonType::But,
        "ASSETSHELF_OT_asset_favorite_toggle",
        wm::OpCallContext::ExecDefault,
        is_favorite_ ? ICON_SOLO_ON : ICON_SOLO_OFF,
        0,
        0,
        short(ICON_DEFAULT_WIDTH_SCALE * overlay_icon_scale),
        short(ICON_DEFAULT_HEIGHT_SCALE * overlay_icon_scale),
        std::nullopt);
    /* Act on this tile's asset, which is not necessarily the active brush (without these
     * properties the operator falls back to the active brush). */
    PointerRNA *favorite_opptr = ui::button_operator_ptr_ensure(favorite_but);
    ed::asset::operator_asset_reference_props_set(asset_, *favorite_opptr);
    /* The star sits on top of the preview image, which can be any color: draw it like the download
     * button below, as a white icon over a dark circle. */
    ui::button_pushbutton_draw_as_overlay_set(favorite_but, true);
    ui::button_pushbutton_overlay_alpha_factor_set(favorite_but, is_hovered() ? 1.0f : 0.8f);
    /* #widget_draw_icon() derives the drawn icon size from here, not from the button's rect. */
    ui::button_icon_scale_set(favorite_but, overlay_icon_scale);
  }

  /* Trailing spacer, insetting the row's contents from the tile's right edge (the row is
   * right-aligned, so this is what pushes them inwards). */
  ui::block_layout_set_current(overlay_block, &overlay_row);
  ui::uiDefBut(
      overlay_block, ui::ButtonType::Label, "", 0, 0, overlay_pad, 1, nullptr, 0, 0, std::nullopt);

  /* Download overlay button for online assets. */
  if (is_hovered() && asset_.needs_download()) {
    ui::Block *block = overlap.block();

    ui::Layout &center_row = overlap.row(true);
    center_row.alignment_set(ui::LayoutAlign::Center);
    center_row.ui_units_x_set(overlap.ui_units_x());

    center_row.column(true);

    const int overlay_width = ICON_DEFAULT_WIDTH_SCALE * 2.0f;
    const int overlay_height = ICON_DEFAULT_HEIGHT_SCALE * 2.0f;
    const int preview_height = tile_height(asset_view.shelf_.settings) -
                               ((asset_view.shelf_.settings.display_flag & ASSETSHELF_SHOW_NAMES) ?
                                    UI_UNIT_Y :
                                    0.0f);

    /* Insert padding above the overlay to center it vertically. */
    ui::uiDefBut(block,
                 ui::ButtonType::Label,
                 "",
                 0,
                 0,
                 1,
                 std::max(0.0f, (preview_height - overlay_height + U.pixelsize) * 0.5f),
                 nullptr,
                 0,
                 0,
                 std::nullopt);

    ui::Button *but = uiDefIconButO(block,
                                    ui::ButtonType::But,
                                    "ASSET_OT_asset_download",
                                    wm::OpCallContext::ExecDefault,
                                    ICON_DOWNLOAD,
                                    0,
                                    0,
                                    overlay_width,
                                    overlay_height,
                                    std::nullopt);
    PointerRNA *opptr = ui::button_operator_ptr_ensure(but);
    ed::asset::operator_asset_reference_props_set(asset_, *opptr);
    ui::button_icon_scale_set(but, 1.5f);
    ui::button_pushbutton_draw_as_overlay_set(but, true);
  }
}

void AssetViewItem::build_context_menu(bContext &C, ui::Layout &column) const
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelf &shelf = asset_view.shelf_;
  const AssetShelfType &shelf_type = *shelf.type;

  const bool show_reorder = !shelf_type.reorder_direction_operator.empty() &&
                            settings_is_favorites_catalog_active(shelf.settings);

  if (show_reorder) {
    /* #reorder_direction_operator's calling convention is currently only implemented against the
     * brush shelf's disk-persisted favorites list; a non-brush consumer would need its own asset
     * lookup here too. That's fine for now: for any shelf type whose idname isn't in the brush
     * lists cache, #shelf_asset_lists_favorites() safely returns an empty span, so `index` stays -1 and
     * this silently offers no reorder entries instead of misbehaving. */
    const ShelfAssetRef ref = ShelfAssetRef::from_weak_reference(asset_.make_weak_reference());
    const Span<ShelfAssetRef> favorites = shelf_asset_lists_favorites(shelf.type->idname);

    int index = -1;
    for (const int i : favorites.index_range()) {
      if (favorites[i] == ref) {
        index = i;
        break;
      }
    }

    if (index >= 0) {
      const int count = int(favorites.size());
      const StringRefNull reorder_op = shelf_type.reorder_direction_operator;

      column.operator_context_set(wm::OpCallContext::ExecDefault);

      {
        ui::Layout &sub = column.row(false);
        sub.enabled_set(index > 0);
        PointerRNA ptr = sub.op(reorder_op, IFACE_("Move Left"), ICON_TRIA_LEFT_BAR);
        ed::asset::operator_asset_reference_props_set(asset_, ptr);
        RNA_enum_set_identifier(&C, &ptr, "direction", "LEFT");
      }
      {
        ui::Layout &sub = column.row(false);
        sub.enabled_set(index < count - 1);
        PointerRNA ptr = sub.op(reorder_op, IFACE_("Move Right"), ICON_TRIA_RIGHT_BAR);
        ed::asset::operator_asset_reference_props_set(asset_, ptr);
        RNA_enum_set_identifier(&C, &ptr, "direction", "RIGHT");
      }

      column.separator();

      {
        PointerRNA ptr = column.op(reorder_op, IFACE_("Reorder to Front"), ICON_TRIA_LEFT_BAR);
        ed::asset::operator_asset_reference_props_set(asset_, ptr);
        RNA_enum_set_identifier(&C, &ptr, "direction", "FRONT");
      }
      {
        PointerRNA ptr = column.op(reorder_op, IFACE_("Reorder to Back"), ICON_TRIA_RIGHT_BAR);
        ed::asset::operator_asset_reference_props_set(asset_, ptr);
        RNA_enum_set_identifier(&C, &ptr, "direction", "BACK");
      }

      column.separator();
    }
  }

  if (shelf_type.draw_context_menu) {
    shelf_type.draw_context_menu(&C, &shelf_type, &asset_, column);
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
  /* Shelf types whose #get_active_asset_from_context uses a different addressing scheme than
   * #AssetRepresentation::make_weak_reference() can opt into a name-based fallback match. */
  if (shelf_type.active_asset_name_fallback_matches) {
    if (const ID *local_id = asset_.local_id()) {
      return shelf_type.active_asset_name_fallback_matches(
          &shelf_type, local_id, *asset_view.active_asset_);
    }
  }
  return false;
}

void AssetViewItem::on_activate(bContext &C)
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  /* Don't allow activating the asset when it requires downloading. */
  if (asset_.is_online_only()) {
    return;
  }

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

bool AssetViewItem::supports_drag() const
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;
  /* Entering BUTTON_STATE_WAIT_DRAG on every press (not just Shift-held ones) is required so the
   * drag-threshold state machine can recognize a *later* Shift-held move as the start of a
   * reorder drag -- reorder validity itself is decided by the configured operator's poll(), not
   * here (see GridItemReorderDropTarget::can_drop()). */
  if (!shelf_type.reorder_operator.empty()) {
    return true;
  }
  return create_drag_controller() != nullptr;
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

std::unique_ptr<ui::AbstractViewItemDragController> AssetViewItem::create_drag_controller(
    const wmEvent *event) const
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;

  const bool shift = event && (event->modifier & KM_SHIFT);
  if (shift && !shelf_type.reorder_operator.empty()) {
    return std::make_unique<AssetReorderDragController>(
        this->get_view(), asset_, shelf_type.idname);
  }

  return create_drag_controller();
}

std::unique_ptr<ui::GridViewItemDropTarget> AssetViewItem::create_drop_target()
{
  const AssetView &asset_view = dynamic_cast<const AssetView &>(this->get_view());
  const AssetShelfType &shelf_type = *asset_view.shelf_.type;
  if (shelf_type.reorder_operator.empty()) {
    return nullptr;
  }
  return std::make_unique<ui::GridItemReorderDropTarget>(
      this->get_view(), *this, shelf_type.reorder_operator, shelf_type.activate_operator,
      shelf_type.idname);
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
                      const bContext &C,
                      std::optional<int> popup_grid_viewport_height_px,
                      std::optional<int> cols_hint)
{
  /* Recent/Favorites are identity-keyed lists, not catalog-keyed, and their members can come from
   * any library. Show them regardless of which library happens to be selected in the header, rather
   * than silently hiding entries that don't belong to it -- scoping favorites to one library is
   * still possible via "Only Favorites" combined with picking that library and its "All" catalog. */
  const bool show_across_all_libraries = settings_is_recent_catalog_active(shelf.settings) ||
                                         settings_is_favorites_catalog_active(shelf.settings);
  const AssetLibraryReference effective_library_ref =
      show_across_all_libraries ? asset_system::all_library_reference() : library_ref;

  /* Always fetch the actually-selected library, even when it isn't what populates the grid below:
   * #catalog_tree_draw() and the library selector both key off \a library_ref directly, independent
   * of which pseudo-catalog happens to be active here, and rely on this fetch to make it available. */
  list::storage_fetch(&library_ref, &C);
  if (show_across_all_libraries) {
    list::storage_fetch(&effective_library_ref, &C);
  }

  const asset_system::AssetLibrary *library = list::library_get_once_available(
      effective_library_ref);
  if (!library) {
    return;
  }

  const float tile_width = shelf::tile_width(shelf.settings);
  const float tile_height = shelf::tile_height(shelf.settings);
  BLI_assert(tile_width != 0);
  BLI_assert(tile_height != 0);

  const std::optional<AssetWeakReference> active_asset = active_asset_for_shelf(shelf, C);

  std::unique_ptr asset_view = std::make_unique<AssetView>(
      effective_library_ref, shelf, active_asset);
  asset_view->set_catalog_filter(catalog_filter_from_shelf_settings(shelf.settings, *library));
  asset_view->set_pseudo_filter(
      pseudo_filter_from_shelf_settings(shelf.settings, shelf.type->idname));
  asset_view->set_favorites(favorites_from_shelf(shelf.type->idname));
  asset_view->set_tile_size(tile_width, tile_height);
  if (cols_hint) {
    /* Popover snaps its width to whole columns; forcing the column count here keeps the grid from
     * computing one fewer column due to float rounding at the boundary (which would leave a gap). */
    asset_view->set_cols_per_row_hint(*cols_hint);
  }
  if (popup_grid_viewport_height_px) {
    /* Popover: bound the grid to a fixed viewport with internal row scrolling so the popover header
     * (search / preview size / settings) stays put instead of scrolling away with the block. */
    asset_view->set_fixed_viewport_layout(true);
    asset_view->set_min_viewport_height(*popup_grid_viewport_height_px);
  }

  ui::Block *block = layout.block();
  ui::AbstractGridView *grid_view = block_add_view(
      *block, "asset shelf asset view", std::move(asset_view));
  if (popup_grid_viewport_height_px) {
    /* Fixed-viewport popover: the scroll position must survive the per-refresh view rebuild and
     * popover reopen; the session registry provides both. Keyed per shelf type so different
     * shelves keep independent positions (the view idname is a shared constant). */
    grid_view->use_session_scroll(fmt::format("asset_shelf_grid:{}", shelf.type->idname));
  }
  grid_view->set_context_menu_title("Asset Shelf");
  if (shelf.type->flag & ASSET_SHELF_TYPE_FLAG_CENTER_ACTIVE_ASSET_ON_OPEN) {
    if (popup_grid_viewport_height_px) {
      /* Fixed-viewport popover: centre via the row-index scroll model, only on first open (a refresh
       * fires on every scroll step; re-centring each time would fight the user's scrolling). */
      if (ui::block_is_first_open(block)) {
        grid_view->scroll_active_into_view(const_cast<bContext *>(&C),
                                           /*scroll_active_to_center=*/true);
      }
    }
    else {
      grid_view->scroll_active_into_center_on_draw_ = true;
    }
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

/* ---------------------------------------------------------------------- */
/* Shift+drag reorder via #AssetShelfType::reorder_operator. */

AssetReorderDragController::AssetReorderDragController(ui::AbstractGridView &view,
                                                       asset_system::AssetRepresentation &asset,
                                                       const char *shelf_idname)
    : ui::AbstractViewItemDragController(view), asset_(asset), shelf_idname_(shelf_idname)
{
}

std::optional<eWM_DragDataType> AssetReorderDragController::get_drag_type() const
{
  return WM_DRAG_GRID_ITEM_REORDER_ASSET;
}

void *AssetReorderDragController::create_drag_data() const
{
  wmDragGridItemReorderAsset *drag_data = MEM_new<wmDragGridItemReorderAsset>(__func__);
  drag_data->shelf_idname = shelf_idname_;
  drag_data->source = asset_.make_weak_reference();
  drag_data->display_name = asset_.get_name();
  return drag_data;
}

void AssetReorderDragController::on_drag_start(bContext &C, ui::AbstractViewItem &item)
{
  wmWindowManager *wm = CTX_wm_manager(&C);
  if (wm && !wm->runtime->drags.is_empty()) {
    wmDrag &drag = *static_cast<wmDrag *>(wm->runtime->drags.last);
    if (drag.type == WM_DRAG_GRID_ITEM_REORDER_ASSET) {
      const BIFIconID icon_id = asset_preview_icon_id(asset_);
      if (icon_id != ICON_NONE) {
        /* Scale relative to #wm_drag_preview_icon_size_get()'s base size (96px * UI_SCALE_FAC);
         * this is the value that actually takes effect -- #wmDrag::preview_icon_scale's own
         * default is always overwritten by this call, so tune the icon size here, not there. */
        WM_event_drag_preview_icon(&drag, icon_id, 0.5f);
      }
    }
  }
  item.set_state_active();
}

}  // namespace blender::ed::asset::shelf
