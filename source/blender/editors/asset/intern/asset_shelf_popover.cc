/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <optional>

#include "AS_asset_library.hh"

#include "asset_shelf.hh"

#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_userdef_types.h"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "ED_asset_filter.hh"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::asset::shelf {

class StaticPopupShelves {
 public:
  Vector<AssetShelf *> popup_shelves;

  ~StaticPopupShelves()
  {
    for (AssetShelf *shelf : popup_shelves) {
      MEM_delete(shelf);
    }
  }

  static Vector<AssetShelf *> &shelves()
  {
    static StaticPopupShelves storage;
    return storage.popup_shelves;
  }
};

void type_popup_unlink(const AssetShelfType &shelf_type)
{
  for (AssetShelf *shelf : StaticPopupShelves::shelves()) {
    if (shelf->type == &shelf_type) {
      shelf->type = nullptr;
    }
  }
}

static AssetShelf *lookup_shelf_for_popup(const bContext &C, const AssetShelfType &shelf_type)
{
  Vector<AssetShelf *> &popup_shelves = StaticPopupShelves::shelves();

  for (AssetShelf *shelf : popup_shelves) {
    if (STREQ(shelf->idname, shelf_type.idname)) {
      if (type_poll_for_popup(C, ensure_shelf_has_type(*shelf))) {
        return shelf;
      }
      break;
    }
  }

  return nullptr;
}

AssetShelf *popup_shelf_get_or_create(const bContext &C, AssetShelfType &shelf_type)
{
  Vector<AssetShelf *> &popup_shelves = StaticPopupShelves::shelves();

  if (AssetShelf *shelf = lookup_shelf_for_popup(C, shelf_type)) {
    return shelf;
  }

  if (!type_poll_for_popup(C, &shelf_type)) {
    return nullptr;
  }

  AssetShelf *new_shelf = create_shelf_from_type(shelf_type);
  new_shelf->is_popup = 1;
  new_shelf->settings.display_flag |= ASSETSHELF_SHOW_NAMES;
  /* Increased size of previews, to leave more space for the name. */
  new_shelf->settings.preview_size = ASSET_SHELF_PREVIEW_SIZE_DEFAULT;
  /* #create_shelf_from_type zero-initializes the settings, so the DNA member default doesn't
   * apply here. Seed an explicit default before the user preferences may override it. */
  new_shelf->settings.popup_width_units = ASSET_SHELF_POPUP_WIDTH_UNITS_DEFAULT;

  /* Overlay the user's persisted popup view preferences on top of the type defaults. The load is a
   * no-op unless the user has explicitly stored preferences for this shelf type. */
  short display_flag = short(new_shelf->settings.display_flag);
  BKE_preferences_asset_shelf_popup_view_load(&U,
                                              shelf_type.idname,
                                              &new_shelf->settings.preview_size,
                                              &display_flag,
                                              &new_shelf->settings.popup_width_units,
                                              &new_shelf->settings.popup_height_units);
  new_shelf->settings.display_flag = AssetShelfSettings_DisplayFlag(display_flag);

  popup_shelves.append(new_shelf);
  return new_shelf;
}

void ensure_asset_library_fetched(const bContext &C, const AssetShelfType &shelf_type)
{
  if (AssetShelf *shelf = lookup_shelf_for_popup(C, shelf_type)) {
    list::storage_fetch(&shelf->settings.asset_library_reference, &C);
  }
  else {
    AssetLibraryReference library_ref = asset_system::all_library_reference();
    list::storage_fetch(&library_ref, &C);
  }
}

/* Catalog tree item that persists its collapsed state into the shelf settings. */
class AssetShelfCatalogTreeViewItem : public ui::BasicTreeViewItem {
  AssetShelf &shelf_;
  std::string catalog_path_;

 public:
  AssetShelfCatalogTreeViewItem(StringRef name, AssetShelf &shelf, std::string catalog_path)
      : BasicTreeViewItem(name), shelf_(shelf), catalog_path_(std::move(catalog_path))
  {
  }

  std::optional<bool> should_be_collapsed() const override
  {
    return settings_get_catalog_path_collapsed(shelf_.settings,
                                               asset_system::AssetCatalogPath(catalog_path_));
  }

  bool set_collapsed(bool collapsed) override
  {
    const bool result = BasicTreeViewItem::set_collapsed(collapsed);
    settings_set_catalog_path_collapsed(
        shelf_.settings, asset_system::AssetCatalogPath(catalog_path_), collapsed);
    return result;
  }
};

class AssetCatalogTreeView : public ui::AbstractTreeView {
  AssetShelf &shelf_;
  asset_system::AssetCatalogTree catalog_tree_;

 public:
  AssetCatalogTreeView(const asset_system::AssetLibrary &library, AssetShelf &shelf)
      : shelf_(shelf)
  {
    catalog_tree_ = build_filtered_catalog_tree(
        library,
        shelf_.settings.asset_library_reference,
        [this](const asset_system::AssetRepresentation &asset) {
          return type_asset_poll(*shelf_.type, asset);
        });

    /* Keep the popup open when clicking to activate a catalog. */
    this->set_popup_keep_open();
  }

  void build_tree() override
  {
    if (catalog_tree_.is_empty()) {
      auto &item = this->add_tree_item<ui::BasicTreeViewItem>(RPT_("No asset catalogs"),
                                                              ICON_INFO);
      item.disable_interaction();
      this->is_flat_ = true;
      return;
    }

    auto &all_item = this->add_tree_item<ui::BasicTreeViewItem>(IFACE_("All"));
    all_item.set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem &) {
      settings_set_all_catalog_active(shelf_.settings);
      send_redraw_notifier(C);
    });
    all_item.set_is_active_fn(
        [this]() { return settings_is_all_catalog_active(shelf_.settings); });
    all_item.uncollapse_by_default();

    catalog_tree_.foreach_root_item([&, this](
                                        const asset_system::AssetCatalogTreeItem &catalog_item) {
      ui::BasicTreeViewItem &item = this->build_catalog_items_recursive(all_item, catalog_item);
      item.uncollapse_by_default();
    });
  }

  AssetShelfCatalogTreeViewItem &build_catalog_items_recursive(
      ui::TreeViewOrItem &parent_view_item,
      const asset_system::AssetCatalogTreeItem &catalog_item) const
  {
    std::string catalog_path = catalog_item.catalog_path().str();
    AssetShelfCatalogTreeViewItem &view_item =
        parent_view_item.add_tree_item<AssetShelfCatalogTreeViewItem>(
            catalog_item.get_name(), shelf_, catalog_path);

    view_item.set_on_activate_fn([this, catalog_path](bContext &C, ui::BasicTreeViewItem &) {
      settings_set_active_catalog(shelf_.settings, catalog_path);
      send_redraw_notifier(C);
    });
    view_item.set_is_active_fn([this, catalog_path]() {
      return settings_is_active_catalog(shelf_.settings, catalog_path);
    });

    const int parent_count = view_item.count_parents() + 1;

    catalog_item.foreach_child([&, this](const asset_system::AssetCatalogTreeItem &child) {
      ui::BasicTreeViewItem &child_item = build_catalog_items_recursive(view_item, child);

      /* Uncollapse to some level (gives quick access, but don't let the tree get too big). */
      if (parent_count < 3) {
        child_item.uncollapse_by_default();
      }
    });

    return view_item;
  }

  /* Redraw the catalog tree when catalogs (and thus their collapsed state) change. */
  bool listen(const wmNotifier &notifier) const override
  {
    return notifier.category == NC_ASSET && notifier.data == ND_ASSET_CATALOGS;
  }
};

static void catalog_tree_draw(const bContext &C,
                              ui::Layout &layout,
                              AssetShelf &shelf,
                              int fixed_height_px)
{
  const asset_system::AssetLibrary *library = list::library_get_once_available(
      shelf.settings.asset_library_reference);
  if (!library) {
    return;
  }

  ui::Block *block = layout.block();
  ui::AbstractTreeView *tree_view = block_add_view(
      *block,
      "asset shelf catalog tree view",
      std::make_unique<AssetCatalogTreeView>(*library, shelf));

  /* Bound the catalog tree to the exact pixel height of the asset grid viewport so the popover keeps
   * a stable height with no dead space below either column, and the header stays put even with many
   * catalogs. Grip-less: the popover height is fixed externally. */
  tree_view->set_fixed_height_px(fixed_height_px, /*allow_resize=*/false);

  ui::TreeViewBuilder::build_tree_view(C, *tree_view, layout);
}

static AssetShelfType *lookup_type_from_idname_in_context(const bContext *C)
{
  const std::optional<StringRefNull> idname = CTX_data_string_get(C, "asset_shelf_idname");
  if (!idname) {
    return nullptr;
  }
  return type_find_from_idname(*idname);
}

constexpr int LEFT_COL_WIDTH_UNITS = 10;

/* Default asset grid viewport height (in #UI_UNIT_Y) for the popover when it is not zoomed; the
 * grid scrolls internally beyond this. Shrunk to fit the window by
 * #ui::popup_grid_fixed_viewport_units. */
constexpr float ASSET_SHELF_POPUP_GRID_DEFAULT_UNITS_Y = 18.0f;

/**
 * Ensure the popover width fits into the window: clamp the total width to the horizontal budget
 * (in #UI_UNIT_X units) available to the right of the popover's pinned left edge. Once the catalog
 * is wide enough to consume the budget, the right (grid) column gives up space instead of the
 * popover growing off screen (the left edge stays fixed, see #ui::BLOCK_POPUP_ANCHOR_LEFT).
 */
static int layout_width_units_clamped(int left_col_width, int right_col_width, int max_total_units)
{
  /* Ensure a reasonable minimum width for the right column. */
  const int effective_right_width = std::max(right_col_width, 20);
  return std::min(left_col_width + effective_right_width, max_total_units);
}

/* Catalog tree column width bounds (in #UI_UNIT_X units) for the interactive vertical grip. */
constexpr int CATALOG_COL_WIDTH_MIN_UNITS = 6;
constexpr int CATALOG_COL_WIDTH_MAX_UNITS = 30;
/* Width of the vertical grip strip between the catalog and grid columns (in #UI_UNIT_X units). */
constexpr float CATALOG_GRIP_WIDTH_UNITS = 0.4f;

/**
 * Clamp the user's popup grid-viewport height (in #UI_UNIT_Y units) so the whole popover stays on
 * screen. Mirrors #layout_width_units_clamped for the vertical axis: #popup_grid_fixed_viewport_units
 * only window-fits a zoomed (aspect < 1) spawn, so the general case needs this explicit clamp.
 * The lower bound keeps at least a few preview rows; the upper bound leaves room for the header row
 * above the grid plus a small screen-edge margin.
 */
static int layout_height_units_clamped(const wmWindow *win, int grid_height_units)
{
  const int min_units_y = 3;
  const int max_units_y = (WM_window_native_pixel_y(win) / UI_UNIT_Y) - 4;
  return std::clamp(grid_height_units, min_units_y, std::max(min_units_y, max_units_y));
}

static void popover_panel_draw(const bContext *C, Panel *panel)
{
  const wmWindow *win = CTX_wm_window(C);
  AssetShelfType *shelf_type = lookup_type_from_idname_in_context(C);
  BLI_assert_msg(shelf_type != nullptr, "couldn't find asset shelf type from context");

  AssetShelf *shelf = popup_shelf_get_or_create(*C, *shelf_type);
  if (!shelf) {
    BLI_assert_unreachable();
    return;
  }

  settings_ensure_valid_library_ref(shelf->settings);

  /* The popup shelf is a process-global singleton, not tied to the current file
   * (#StaticPopupShelves), and #popup_shelf_get_or_create returns an existing instance without
   * re-seeding. Re-apply the size from the current file's per-`.blend` override (falling back to the
   * Preferences default) whenever the popover opens, so switching files shows each file's own
   * remembered size. Gated on first-open, not every refresh, so a live grip resize during an open
   * popover is not clobbered. The width/height are clamped to the window at draw time below. */
  if (ui::block_is_first_open(panel->layout->block())) {
    short width_units = shelf->settings.popup_width_units;
    short height_units = shelf->settings.popup_height_units;
    short catalog_width_units = shelf->settings.popup_catalog_width_units;
    BKE_preferences_asset_shelf_popup_view_load(
        &U, shelf->idname, nullptr, nullptr, &width_units, &height_units);
    if (const wmWindowManager *wm = CTX_wm_manager(C)) {
      popup_size_load(*wm, shelf->idname, &width_units, &height_units, &catalog_width_units);
    }
    /* Materialize the "0 = use default" values into the concrete defaults so the resize grips drag
     * from the size actually shown (their origin is the stored value), instead of jumping from 0. */
    if (height_units <= 0) {
      height_units = short(ASSET_SHELF_POPUP_GRID_DEFAULT_UNITS_Y);
    }
    if (catalog_width_units <= 0) {
      catalog_width_units = short(LEFT_COL_WIDTH_UNITS);
    }
    shelf->settings.popup_width_units = width_units;
    shelf->settings.popup_height_units = height_units;
    shelf->settings.popup_catalog_width_units = catalog_width_units;
  }

  /* Catalog tree column width, resizable via the vertical grip between the columns. */
  const int catalog_units = std::clamp(int(shelf->settings.popup_catalog_width_units) > 0 ?
                                           int(shelf->settings.popup_catalog_width_units) :
                                           LEFT_COL_WIDTH_UNITS,
                                       CATALOG_COL_WIDTH_MIN_UNITS,
                                       CATALOG_COL_WIDTH_MAX_UNITS);

  /* Pin the popover's left edge so the catalog-width grip (and the width control) only grow/shrink
   * it to the right; the left edge stays fixed even when the block is right-aligned to the window
   * edge. The total width is clamped to the horizontal budget from that pinned left edge, so once
   * the catalog fills the budget the grid gives up columns rather than the popover going off screen. */
  ui::Block *popover_block = panel->layout->block();
  ui::block_flag_enable(popover_block, ui::BLOCK_POPUP_ANCHOR_LEFT);
  const int width_budget_units = std::max(
      catalog_units + 1, ui::popup_block_left_anchored_budget_px(win, popover_block) / UI_UNIT_X - 1);

  const int layout_width_units = layout_width_units_clamped(
      catalog_units, shelf->settings.popup_width_units, width_budget_units);

  /* Ensure the assertion doesn't fail if the window is extremely small. */
  if (layout_width_units <= catalog_units) {
    return;
  }

  /* Snap the right column to a whole number of grid tile columns so the previews fill it exactly,
   * with no gap between the last column and the popover edge (most visible while resizing width
   * with the grip). #grid_cols is forwarded to the grid as a hint so a float rounding at the column
   * boundary cannot drop it to one fewer column and reopen the gap. */
  const int tile_w_px = std::max(1, tile_width(shelf->settings));
  const float tile_w_units = float(tile_w_px) / float(UI_UNIT_X);
  const int raw_right_units = layout_width_units - catalog_units;
  const int grid_cols = std::max(1, int(float(raw_right_units) / tile_w_units));
  const float right_col_width_units = float(grid_cols) * tile_w_units;

  ui::Layout &layout = *panel->layout;
  layout.ui_units_x_set(float(catalog_units) + CATALOG_GRIP_WIDTH_UNITS + right_col_width_units);

  bScreen *screen = CTX_wm_screen(C);
  PointerRNA library_ref_ptr = RNA_pointer_create_discrete(
      &screen->id, RNA_AssetLibraryReference, &shelf->settings.asset_library_reference);
  layout.context_ptr_set("asset_library_reference", &library_ref_ptr);

  /* Make the shelf accessible to nested popovers (e.g. display settings panel). */
  PointerRNA shelf_ptr = RNA_pointer_create_discrete(&screen->id, RNA_AssetShelf, shelf);
  layout.context_ptr_set("asset_shelf", &shelf_ptr);

  /* Asset grid viewport height, derived from the window so the popover fits on screen (see
   * #ui::popup_grid_fixed_viewport_units). The raw pixel height is used (not snapped to whole tile
   * rows): the grid clips a partial bottom row at the viewport edge, so when the preview size
   * changes without the popover resizing, the grid still fills the whole height instead of leaving
   * dead space below the last whole row. The catalog tree is given the same pixel height so the
   * taller column never leaves dead space below the shorter one. Reused to keep both columns within
   * the window so the sticky header holds. */
  const int tile_h_px = std::max(1, tile_height(shelf->settings));
  const float tile_units = float(tile_h_px) / float(UI_UNIT_Y);
  /* Header + gap consumed above the grid: the search / preset row (~1 unit) plus the ~1-unit gap
   * below it where the persistent scroll-up arrow is drawn. Only used to shrink the grid when a
   * zoomed popover would otherwise overflow the window. */
  const float non_grid_units = 2.0f;
  /* User-set popover height (grid-viewport units) overrides the default, clamped to the window.
   * 0 means "not set" — use the type default. */
  const float default_grid_units = (shelf->settings.popup_height_units > 0) ?
                                       float(layout_height_units_clamped(
                                           win, shelf->settings.popup_height_units)) :
                                       ASSET_SHELF_POPUP_GRID_DEFAULT_UNITS_Y;
  const float grid_units = ui::popup_grid_fixed_viewport_units(
      C, layout.block(), non_grid_units, tile_units, default_grid_units);
  const int grid_viewport_px = std::max(tile_h_px, int(grid_units * UI_UNIT_Y));

  ui::Layout &row = layout.row(false);
  ui::Layout &catalogs_col = row.column(false);
  catalogs_col.ui_units_x_set(float(catalog_units));
  catalogs_col.fixed_size_set(true);
  library_selector_draw(C, catalogs_col, *shelf);
  catalog_tree_draw(*C, catalogs_col, *shelf, grid_viewport_px);

  /* Vertical grip between the columns to resize the catalog tree width (useful when category names
   * are long). X-only (no second value pointer); the hard min/max bound it inside #do_but_GRIP. */
  {
    ui::Layout &grip_col = row.column(false);
    grip_col.fixed_size_set(true);
    ui::Block *block = layout.block();
    ui::block_layout_set_current(block, &grip_col);
    ui::Button *catalog_grip = ui::uiDefIconButV(block,
                                                 ui::ButtonType::Grip,
                                                 ICON_GRIP,
                                                 0,
                                                 0,
                                                 short(CATALOG_GRIP_WIDTH_UNITS * UI_UNIT_X),
                                                 short(grid_viewport_px),
                                                 &shelf->settings.popup_catalog_width_units,
                                                 float(CATALOG_COL_WIDTH_MIN_UNITS),
                                                 float(CATALOG_COL_WIDTH_MAX_UNITS),
                                                 std::nullopt);
    ui::button_grip_2d_set(catalog_grip, nullptr);
    ui::button_flag_disable(catalog_grip, ui::BUT_UNDO);
    AssetShelf *shelf_capture = shelf;
    ui::button_func_set(catalog_grip, [shelf_capture](bContext &C) {
      if (wmWindowManager *wm = CTX_wm_manager(&C)) {
        popup_size_store(*wm,
                         shelf_capture->idname,
                         shelf_capture->settings.popup_width_units,
                         shelf_capture->settings.popup_height_units,
                         shelf_capture->settings.popup_catalog_width_units);
        WM_file_tag_modified();
      }
    });
    ui::block_layout_set_current(block, &layout);
  }

  ui::Layout &right_col = row.column(false);
  right_col.ui_units_x_set(right_col_width_units);
  right_col.fixed_size_set(true);

  ui::Layout &header_row = right_col.row(false);
  header_row.ui_units_x_set(right_col_width_units);

  /* Wrapper row expands to fill the header up to the fixed-width control buttons. */
  ui::Layout &search_row = header_row.row(false);
  search_row.prop(&shelf_ptr,
                  "search_filter",
                  /* Force the button to be active in a semi-modal state. */
                  ui::ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE,
                  "",
                  ICON_VIEWZOOM);

  /* Fixed-width preset buttons merged with the display settings popover.
   * Non-default alignment is required in popover panels so button width is based on
   * label content instead of the default 10 UI-unit placeholder width. */
  ui::Layout &controls = header_row.row(true);
  controls.fixed_size_set(true);
  controls.alignment_set(ui::LayoutAlign::Right);
  controls.prop_enum(
      &shelf_ptr, "preview_size_preset", "SMALL", IFACE_("Small"), ICON_SHORTDISPLAY);
  controls.prop_enum(
      &shelf_ptr, "preview_size_preset", "MEDIUM", IFACE_("Medium"), ICON_IMGDISPLAY);
  controls.prop_enum(
      &shelf_ptr, "preview_size_preset", "LARGE", IFACE_("Large"), ICON_LONGDISPLAY);
  {
    PropertyRNA *preset_prop = RNA_struct_find_property(&shelf_ptr, "preview_size_preset");
    controls.prop_with_popover(&shelf_ptr,
                               preset_prop,
                               -1,
                               0,
                               ui::ITEM_R_ICON_ONLY,
                               std::nullopt,
                               ICON_NONE,
                               "ASSETSHELF_PT_popover_display");
  }

  /* Gap between the header and the grid so the persistent scroll-up arrow
   * (#ui::AbstractGridView::draw_overlays), drawn ~0.75 #UI_UNIT_Y above the top tile row, clears the
   * search field instead of overlapping it. #Layout::separator uses 6px*UI_SCALE_FAC steps. */
  const float unit_gap_factor = float(UI_UNIT_Y) / (6.0f * UI_SCALE_FAC);
  right_col.separator(unit_gap_factor);

  ui::Layout &asset_view_col = right_col.column(false);
  asset_view_col.ui_units_x_set(right_col_width_units);
  asset_view_col.fixed_size_set(true);

  build_asset_view(asset_view_col,
                   shelf->settings.asset_library_reference,
                   *shelf,
                   *C,
                   grid_viewport_px,
                   grid_cols);

  /* Interactive 2D resize grip in the bottom-right corner. Placed in a full-width row beneath both
   * columns so the strip it adds is under the catalog tree and the grid equally — never lengthening
   * only the right column (which would leave dead space below the shorter catalog column). The grip
   * drives #popup_width_units (X) and #popup_height_units (Y); both are clamped to the window at the
   * next draw. */
  {
    ui::Layout &grip_row = layout.row(false);
    grip_row.alignment_set(ui::LayoutAlign::Right);
    ui::Block *block = layout.block();
    ui::block_layout_set_current(block, &grip_row);
    ui::Button *grip = ui::uiDefIconButV(block,
                                         ui::ButtonType::Grip,
                                         ICON_GRIP,
                                         0,
                                         0,
                                         short(UI_UNIT_X),
                                         short(UI_UNIT_Y * 0.7f),
                                         &shelf->settings.popup_width_units,
                                         0.0f,
                                         0.0f,
                                         std::nullopt);
    ui::button_grip_2d_set(grip, &shelf->settings.popup_height_units);
    ui::button_flag_disable(grip, ui::BUT_UNDO);
    /* Persist the live size into this file's per-`.blend` override on each grip apply. #shelf is a
     * process-global static (#StaticPopupShelves), so capturing it is safe across popover rebuilds;
     * the window manager is re-fetched from context each call. */
    AssetShelf *shelf_capture = shelf;
    ui::button_func_set(grip, [shelf_capture](bContext &C) {
      if (wmWindowManager *wm = CTX_wm_manager(&C)) {
        popup_size_store(*wm,
                         shelf_capture->idname,
                         shelf_capture->settings.popup_width_units,
                         shelf_capture->settings.popup_height_units,
                         shelf_capture->settings.popup_catalog_width_units);
        WM_file_tag_modified();
      }
    });
    ui::block_layout_set_current(block, &layout);
  }
}

static bool popover_panel_poll(const bContext *C, PanelType * /*panel_type*/)
{
  const AssetShelfType *shelf_type = lookup_type_from_idname_in_context(C);
  if (!shelf_type) {
    return false;
  }

  return type_poll_for_popup(*C, shelf_type);
}

/* ---------------------------------------------------------------------- */
/** \name Display settings panel for the popover
 * \{ */

static void popover_display_panel_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  /* Retrieve shelf from the popover context (set by popover_panel_draw). */
  PointerRNA shelf_ptr = CTX_data_pointer_get_type(C, "asset_shelf", RNA_AssetShelf);
  if (!shelf_ptr.data) {
    return;
  }

  ui::Layout &col = layout.column(false);
  col.prop(&shelf_ptr, "preview_size", UI_ITEM_NONE, IFACE_("Preview Size"), ICON_NONE);
  col.prop(&shelf_ptr, "show_names", UI_ITEM_NONE, IFACE_("Show Names"), ICON_NONE);
  col.prop(&shelf_ptr, "popup_width_units", UI_ITEM_NONE, IFACE_("Popup Width"), ICON_NONE);
  col.prop(&shelf_ptr, "popup_height_units", UI_ITEM_NONE, IFACE_("Popup Height"), ICON_NONE);

  /* Store the current per-file size as the global default for new files (the interactive grip and
   * the fields above only change this file's remembered size). */
  layout.op("ASSET_OT_shelf_popup_set_default_size", IFACE_("Set as Default"), ICON_NONE);
}

static bool popover_display_panel_poll(const bContext *C, PanelType * /*panel_type*/)
{
  PointerRNA shelf_ptr = CTX_data_pointer_get_type(C, "asset_shelf", RNA_AssetShelf);
  return shelf_ptr.data != nullptr;
}

static void asset_shelf_popover_listen(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  ARegion *region = params->region;

  switch (wmn->category) {
    case NC_ASSET:
      if (ELEM(wmn->data, ND_ASSET_LIST_READING, ND_ASSET_LIST_PREVIEW)) {
        ED_region_tag_refresh_ui(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_REGIONS_ASSET_SHELF) {
        /* Redraw to apply changes to preview size or show names instantly. */
        ED_region_tag_refresh_ui(region);
      }
      break;
  }
}

/** \} */

void popover_panel_register(ARegionType *region_type)
{
  /* Uses global paneltype registry to allow usage as popover. So only register this once (may be
   * called from multiple spaces). */
  if (WM_paneltype_find("ASSETSHELF_PT_popover_panel", true)) {
    return;
  }

  /* Register the main asset shelf popover panel. */
  {
    PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
    STRNCPY_UTF8(pt->idname, "ASSETSHELF_PT_popover_panel");
    STRNCPY_UTF8(pt->label, N_("Asset Shelf Panel"));
    STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
    pt->description = N_("Display an asset shelf in a popover panel");
    pt->draw = popover_panel_draw;
    pt->poll = popover_panel_poll;
    pt->listener = asset_shelf_popover_listen;
    /* Move to have first asset item under cursor. */
    pt->offset_units_xy.x = -(LEFT_COL_WIDTH_UNITS + 1.5f);
    /* Offset so mouse is below search button, over the first row of assets. */
    pt->offset_units_xy.y = 2.5f;
    BLI_addtail(&region_type->paneltypes, pt);
    WM_paneltype_add(pt);
  }

  /* Register the display settings panel (opened from the gear icon). */
  {
    PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
    STRNCPY_UTF8(pt->idname, "ASSETSHELF_PT_popover_display");
    STRNCPY_UTF8(pt->label, N_("Display Settings"));
    STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
    pt->description = N_("Adjust preview size and display options for the brush asset popup");
    pt->draw = popover_display_panel_draw;
    pt->poll = popover_display_panel_poll;
    /* No listener needed — the parent popover handles redraws. */
    BLI_addtail(&region_type->paneltypes, pt);
    WM_paneltype_add(pt);
  }
}

}  // namespace blender::ed::asset::shelf
