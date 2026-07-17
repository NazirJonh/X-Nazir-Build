/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <optional>
#include <string>

#include "AS_asset_library.hh"

#include "asset_shelf.hh"
#include "asset_shelf_brush_lists.hh"

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
#include "UI_view2d.hh"

#include "ED_asset_filter.hh"
#include "ED_asset_library.hh"
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

void popup_shelves_foreach_library_ref(FunctionRef<void(AssetLibraryReference &)> fn)
{
  for (AssetShelf *shelf : StaticPopupShelves::shelves()) {
    fn(shelf->settings.asset_library_reference);
  }
}

void ensure_asset_library_fetched(const bContext &C, const AssetShelfType &shelf_type)
{
  if (AssetShelf *shelf = lookup_shelf_for_popup(C, shelf_type)) {
    /* Runs from the popover *button*'s draw, i.e. on every redraw of its host, whereas the popover
     * itself only validates the reference once opened. Without this, removing an asset library in
     * the Preferences leaves the shelf holding a reference #storage_fetch() asserts on. */
    list::storage_fetch(&settings_ensure_valid_library_ref(shelf->settings), &C);
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

/* "Recent" pseudo-catalog tree item; only its context menu differs from a plain #BasicTreeViewItem
 * (offers clearing the list). */
class RecentCatalogTreeViewItem : public ui::BasicTreeViewItem {
 public:
  using BasicTreeViewItem::BasicTreeViewItem;

  void build_context_menu(bContext & /*C*/, ui::Layout &column) const override
  {
    column.op("BRUSH_OT_asset_recent_clear", IFACE_("Clear Recent"), ICON_NONE);
  }
};

/* "Favorites" pseudo-catalog tree item; only its context menu differs from a plain
 * #BasicTreeViewItem (offers clearing the list). */
class FavoritesCatalogTreeViewItem : public ui::BasicTreeViewItem {
 public:
  using BasicTreeViewItem::BasicTreeViewItem;

  void build_context_menu(bContext & /*C*/, ui::Layout &column) const override
  {
    column.op("BRUSH_OT_asset_favorites_clear", IFACE_("Clear Favorites"), ICON_NONE);
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
    /* Recent/Favorites are catalog-independent, so they must be built before the empty-catalog
     * early-out below -- a brush library that defines no catalogs would otherwise never offer
     * them. */
    if (shelf::shelf_idname_is_brush_shelf(shelf_.type->idname)) {
      auto &recent_item = this->add_tree_item<RecentCatalogTreeViewItem>(IFACE_("Recent"),
                                                                         ICON_RECOVER_LAST);
      recent_item.set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem &) {
        settings_set_recent_catalog_active(shelf_.settings);
        send_redraw_notifier(C);
      });
      recent_item.set_is_active_fn(
          [this]() { return settings_is_recent_catalog_active(shelf_.settings); });

      auto &favorites_item = this->add_tree_item<FavoritesCatalogTreeViewItem>(
          IFACE_("Favorites"), ICON_SOLO_ON);
      favorites_item.set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem &) {
        settings_set_favorites_catalog_active(shelf_.settings);
        send_redraw_notifier(C);
      });
      favorites_item.set_is_active_fn(
          [this]() { return settings_is_favorites_catalog_active(shelf_.settings); });
    }

    auto &all_item = this->add_tree_item<ui::BasicTreeViewItem>(IFACE_("All"));
    all_item.set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem &) {
      settings_set_all_catalog_active(shelf_.settings);
      send_redraw_notifier(C);
    });
    all_item.set_is_active_fn(
        [this]() { return settings_is_all_catalog_active(shelf_.settings); });
    all_item.uncollapse_by_default();

    if (catalog_tree_.is_empty()) {
      auto &item = this->add_tree_item<ui::BasicTreeViewItem>(RPT_("No asset catalogs"),
                                                              ICON_INFO);
      item.disable_interaction();
      /* Every item built above is a leaf here (nothing nests under "All" without catalogs), so no
       * space needs to be reserved for collapse chevrons. */
      this->is_flat_ = true;
      return;
    }

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
  /* Match the asset grid beside it: a vertical drag scrolls, it does not reach the catalog rows.
   * Safe here because catalog items are not draggable — no #create_drag_controller override — so
   * awarding vertical gestures to the scroll costs nothing. */
  tree_view->set_drag_scroll(true);

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

/* Catalog tree column width bounds (in #UI_UNIT_X units) for the interactive vertical grip. */
constexpr int CATALOG_COL_WIDTH_MIN_UNITS = 6;
constexpr int CATALOG_COL_WIDTH_MAX_UNITS = 30;
/* Width of the vertical grip strip between the catalog and grid columns (in #UI_UNIT_X units). */
constexpr float CATALOG_GRIP_WIDTH_UNITS = 0.4f;

/**
 * Coarse ceiling for the user's popup grid-viewport height (in #UI_UNIT_Y units), used as the
 * #default_units cap passed to #ui::popup_grid_fixed_viewport_units. The precise window-fit
 * (subtracting the header/tab rows, resize grip, and popover positioning chrome from the space
 * actually available on the spawn side) is done there for every spawn, so this only needs to be a
 * sane upper bound: the
 * lower bound keeps at least a few preview rows; the upper bound is a whole-window figure minus a
 * small screen-edge margin.
 */
static int layout_height_units_clamped(const wmWindow *win, int grid_height_units)
{
  const int min_units_y = 3;
  const int max_units_y = (WM_window_native_pixel_y(win) / UI_UNIT_Y) - 4;
  return std::clamp(grid_height_units, min_units_y, std::max(min_units_y, max_units_y));
}

/* -------------------------------------------------------------------- */
/** \name Pinned library tabs
 * \{ */

/* A tab in the popover's strip, together with the value the shelf's `asset_library_reference` enum
 * uses for it. Both are read from the same itemf the selector menu is built from, so a tab and the
 * matching menu entry can never disagree about which library they switch to. */
struct PinnedLibraryTab {
  /** Null for the built-in libraries (All / Current File / Essentials), which are not
   * #bUserAssetLibrary entries -- their pins are bits in #UserDef.asset_flag instead. */
  bUserAssetLibrary *library;
  int enum_value;
  /** Owned copy. A built-in's label lives in the enum item, and the items are freed before
   * #pinned_tabs_gather returns; a custom library's name can be freed by the Preferences while an
   * open popover still holds this. */
  std::string name;
  /** A built-in tab: fixed at its text width, never reordered, and (except All) carrying the
   * unpin-only context menu. */
  bool is_fixed;
  /** Which wrapped row of the tab strip this tab belongs on. Decided once, by #pinned_tabs_gather,
   * and followed by #pinned_tabs_draw: the row count sizes the popover, so a draw that wrapped by
   * its own arithmetic could disagree with the height already reserved for it. */
  int row;
};

/* Assign each tab a row, opening a new one whenever it would take the current row past \a budget.
 * Returns the number of rows used, or 0 when there are no tabs at all.
 *
 * A tab wider than the budget itself stays on its own row rather than wrapping twice. */
static int pinned_tabs_wrap(const Span<int> tab_widths,
                            const int budget,
                            MutableSpan<int> r_rows)
{
  if (tab_widths.is_empty()) {
    return 0;
  }

  int row_num = 1;
  int row_width = 0;
  for (const int i : tab_widths.index_range()) {
    if (row_width > 0 && row_width + tab_widths[i] > budget) {
      row_num++;
      row_width = 0;
    }
    row_width += tab_widths[i];
    r_rows[i] = row_num - 1;
  }
  return row_num;
}

/* Collect the tabs this shelf can actually show, in tab order, and report how many wrapped rows
 * they need.
 *
 * The list is derived from the shelf's own `asset_library_reference` enum rather than from
 * #UserDef.asset_libraries directly: that enum already has the per-shelf-type filter applied
 * (#rna_asset_library_ui_reference_itemf excludes image libraries on brush shelves, and everything
 * but image libraries on the Texture shelf). Reading it means the tab row cannot drift away from
 * what the selector offers, and no filter logic is duplicated here. The same call passes
 * `include_readonly` and `include_current_file` unconditionally, so All / Current File / Essentials
 * are always among the items to choose from.
 *
 * This includes the "All" tab, which every returned row count accounts for. */
static Vector<PinnedLibraryTab> pinned_tabs_gather(const bContext &C,
                                                   PointerRNA &shelf_ptr,
                                                   const int avail_width_px,
                                                   int *r_row_num)
{
  Vector<PinnedLibraryTab> builtins;
  Vector<PinnedLibraryTab> customs;

  PropertyRNA *prop = RNA_struct_find_property(&shelf_ptr, "asset_library_reference");
  if (prop) {
    const EnumPropertyItem *items = nullptr;
    bool free = false;
    /* #RNA_property_enum_items_gettexted takes a non-const context; the itemf backing this enum
     * ignores it entirely (see #rna_asset_library_ui_reference_itemf). */
    RNA_property_enum_items_gettexted(
        const_cast<bContext *>(&C), &shelf_ptr, prop, &items, nullptr, &free);

    for (const EnumPropertyItem *item = items; item && item->identifier; item++) {
      /* An empty identifier is a folder heading or a separator, not a choice. */
      if (!item->identifier[0]) {
        continue;
      }

      if (item->value >= ASSET_LIBRARY_CUSTOM) {
        const AssetLibraryReference library_ref = library_reference_from_enum_value(item->value);
        bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(&U,
                                                                                      &library_ref);
        if (user_library && (user_library->flag & ASSET_LIBRARY_IS_PINNED)) {
          customs.append({user_library, item->value, user_library->name, false, 0});
        }
        continue;
      }

      /* A built-in. "All" leads the strip unconditionally -- it has no pin of its own, and being
       * always present is what replaces one. The rest appear only while their bit is set. */
      const eAssetLibraryType type = eAssetLibraryType(item->value);
      if (type != ASSET_LIBRARY_ALL && !BKE_preferences_asset_builtin_pin_get(&U, type)) {
        continue;
      }
      builtins.append({nullptr, item->value, item->name, true, 0});
    }

    if (free) {
      MEM_delete(items);
    }
  }

  /* Only the custom tabs carry #bUserAssetLibrary.pin_order. The built-ins have none: they keep the
   * order the enum lists them in (All, Current File, Essentials), which is exactly the fixed order
   * they are specified to appear in. Sorting the two together would read `library->pin_order`
   * through a null pointer. */
  std::sort(
      customs.begin(), customs.end(), [](const PinnedLibraryTab &a, const PinnedLibraryTab &b) {
        return a.library->pin_order < b.library->pin_order;
      });

  Vector<PinnedLibraryTab> pinned = std::move(builtins);
  pinned.extend(customs);

  Vector<int> tab_widths(pinned.size());
  for (const int i : pinned.index_range()) {
    tab_widths[i] = tab_button_width(pinned[i].name);
  }
  Vector<int> rows(pinned.size());

  /* Wrap the strip. This pass decides the row count, which the caller reserves height for. */
  const int row_num = pinned_tabs_wrap(tab_widths, avail_width_px, rows);

  /* Even the rows out. The custom tabs in a row are stretched to fill whatever the fixed built-in
   * tabs leave (a layout row's default #LayoutAlign::Expand hands the last button what is left
   * over), so packing the first row full and leaving the remainder alone on the last row blows it
   * up across the whole strip. Re-wrapping with the smallest budget that still needs the same
   * number of rows minimizes the widest row, which spreads the load -- and with it the stretch --
   * over the rows.
   *
   * The row count cannot change: it is already the minimum (a wider budget never needs more rows),
   * and only budgets that keep it are accepted. That is what makes this safe to do after the height
   * has been derived. Fewer rows is likewise impossible, so the search lands on exactly `row_num`.
   * The row count is monotonic in the budget, so a binary search is exact here rather than a
   * heuristic. Budgets never exceed `avail_width_px`, so a row cannot be made to overflow the strip
   * (which would put the squeeze back and re-clip the names). */
  if (!pinned.is_empty()) {
    int lo = 1;
    int hi = std::max(1, avail_width_px);
    while (lo < hi) {
      const int mid = lo + (hi - lo) / 2;
      if (pinned_tabs_wrap(tab_widths, mid, rows) <= row_num) {
        hi = mid;
      }
      else {
        lo = mid + 1;
      }
    }
    pinned_tabs_wrap(tab_widths, lo, rows);
  }

  for (const int i : pinned.index_range()) {
    pinned[i].row = rows[i];
  }
  *r_row_num = row_num;

  return pinned;
}

static void library_tab_context_menu_draw(const bContext *C, Menu *menu)
{
  /* The menu is opened from a tab, and #interface_context_menu.cc draws it while that tab is still
   * the active button -- which is how the menu learns which library it is acting on. */
  const bUserAssetLibrary *user_library = static_cast<const bUserAssetLibrary *>(
      ui::context_active_but_tab_custom_data_get(C));
  if (!user_library) {
    return;
  }
  /* Belt and braces. #asset_shelf_popover_listen rebuilds the tab row when the library list
   * changes, so a tab should never outlive its library -- but the rebuild only happens at the next
   * draw, while notifiers and handlers run before that, leaving a window in which a click could
   * still reach a tab whose library was just freed. Checking list membership compares pointers
   * without dereferencing, so it is safe even on a freed one. */
  if (BLI_findindex(&U.asset_libraries, user_library) == -1) {
    return;
  }

  ui::Layout &layout = *menu->layout;
  layout.operator_context_set(wm::OpCallContext::ExecDefault);

  const int index = user_library->pin_order;
  const int count = BKE_preferences_asset_library_pinned_count(&U);

  {
    ui::Layout &sub = layout.row(false);
    sub.enabled_set(index > 0);
    PointerRNA ptr = sub.op(
        "PREFERENCES_OT_asset_library_pin_reorder", IFACE_("Move Left"), ICON_TRIA_LEFT_BAR);
    RNA_string_set(&ptr, "library_name", user_library->name);
    RNA_enum_set_identifier(const_cast<bContext *>(C), &ptr, "direction", "LEFT");
  }
  {
    ui::Layout &sub = layout.row(false);
    sub.enabled_set(index < count - 1);
    PointerRNA ptr = sub.op(
        "PREFERENCES_OT_asset_library_pin_reorder", IFACE_("Move Right"), ICON_TRIA_RIGHT_BAR);
    RNA_string_set(&ptr, "library_name", user_library->name);
    RNA_enum_set_identifier(const_cast<bContext *>(C), &ptr, "direction", "RIGHT");
  }

  layout.separator();

  {
    PointerRNA ptr = layout.op(
        "PREFERENCES_OT_asset_library_pin_reorder", IFACE_("Reorder to Front"), ICON_TRIA_LEFT_BAR);
    RNA_string_set(&ptr, "library_name", user_library->name);
    RNA_enum_set_identifier(const_cast<bContext *>(C), &ptr, "direction", "FRONT");
  }
  {
    PointerRNA ptr = layout.op(
        "PREFERENCES_OT_asset_library_pin_reorder", IFACE_("Reorder to Back"), ICON_TRIA_RIGHT_BAR);
    RNA_string_set(&ptr, "library_name", user_library->name);
    RNA_enum_set_identifier(const_cast<bContext *>(C), &ptr, "direction", "BACK");
  }

  layout.separator();

  {
    PointerRNA ptr = layout.op(
        "PREFERENCES_OT_asset_library_pin_set", IFACE_("Unpin Library"), ICON_UNPINNED);
    RNA_string_set(&ptr, "library_name", user_library->name);
    RNA_boolean_set(&ptr, "pinned", false);
  }
}

/* The built-in tabs' context-menu payload. A pointer into this table is stable for the whole
 * process, so unlike the custom tabs' #bUserAssetLibrary * it can never dangle and the menu needs
 * no membership guard.
 *
 * Only the built-ins that can actually be unpinned are listed; "All" has nothing to offer a menu.
 * Keep in step with #asset_builtin_pin_flag_from_type in `preferences.cc` -- the #BLI_assert in
 * #pinned_tabs_draw is what catches them drifting apart. */
static const eAssetLibraryType builtin_tab_menu_types[] = {ASSET_LIBRARY_LOCAL,
                                                           ASSET_LIBRARY_ESSENTIALS};

static const eAssetLibraryType *builtin_tab_menu_type_ptr(const eAssetLibraryType type)
{
  for (const eAssetLibraryType &entry : builtin_tab_menu_types) {
    if (entry == type) {
      return &entry;
    }
  }
  return nullptr;
}

static void builtin_library_tab_context_menu_draw(const bContext *C, Menu *menu)
{
  /* The menu is opened from a tab, and #interface_context_menu.cc draws it while that tab is still
   * the active button -- which is how the menu learns which library it is acting on. */
  const eAssetLibraryType *type = static_cast<const eAssetLibraryType *>(
      ui::context_active_but_tab_custom_data_get(C));
  if (!type) {
    return;
  }

  ui::Layout &layout = *menu->layout;
  layout.operator_context_set(wm::OpCallContext::ExecDefault);

  /* Unpin only: a built-in tab is fixed in place, so there is nothing to reorder. */
  PointerRNA ptr = layout.op(
      "PREFERENCES_OT_asset_library_pin_set", IFACE_("Unpin Library"), ICON_UNPINNED);
  RNA_enum_set(&ptr, "library_type", int(*type));
  RNA_boolean_set(&ptr, "pinned", false);
}

/* Registered from #popover_panel_register, which runs exactly once (it early-returns when its panel
 * types already exist). Kept next to the draw callback it registers rather than inlined there, so
 * the menu's id, label and draw function stay in one place. */
static void library_tab_context_menu_register()
{
  if (!WM_menutype_find("ASSETSHELF_MT_library_tab_context", true)) {
    MenuType *mt = MEM_new_zeroed<MenuType>(__func__);
    STRNCPY_UTF8(mt->idname, "ASSETSHELF_MT_library_tab_context");
    STRNCPY_UTF8(mt->label, N_("Pinned Library"));
    STRNCPY_UTF8(mt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
    mt->draw = library_tab_context_menu_draw;
    /* Depends on the active button rather than on the RNA context, which is close enough: the
     * flag's real job here is to keep the menu out of the menu search, where there would be no tab
     * to act on and the draw would produce an empty menu. */
    mt->flag = MenuTypeFlag::ContextDependent;
    WM_menutype_add(mt);
  }

  if (!WM_menutype_find("ASSETSHELF_MT_builtin_library_tab_context", true)) {
    MenuType *builtin_mt = MEM_new_zeroed<MenuType>(__func__);
    STRNCPY_UTF8(builtin_mt->idname, "ASSETSHELF_MT_builtin_library_tab_context");
    STRNCPY_UTF8(builtin_mt->label, N_("Pinned Library"));
    STRNCPY_UTF8(builtin_mt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
    builtin_mt->draw = builtin_library_tab_context_menu_draw;
    /* Same reasoning as the menu above: depends on the active button, and the flag's real job is to
     * keep it out of the menu search where there would be no tab to act on. */
    builtin_mt->flag = MenuTypeFlag::ContextDependent;
    WM_menutype_add(builtin_mt);
  }
}

static void pinned_tabs_draw(ui::Layout &layout,
                             AssetShelf &shelf,
                             PointerRNA &shelf_ptr,
                             const Span<PinnedLibraryTab> pinned)
{
  PropertyRNA *library_prop = RNA_struct_find_property(&shelf_ptr, "asset_library_reference");
  if (!library_prop) {
    BLI_assert_unreachable();
    return;
  }

  if (pinned.is_empty()) {
    return;
  }

  ui::Block *block = layout.block();
  AssetShelfSettings &shelf_settings = shelf.settings;

  /* The rows come from #pinned_tabs_gather (#PinnedLibraryTab.row); this pass only follows them, so
   * it cannot end up with a different number of rows than the popover reserved height for. */
  ui::Layout *row = &layout.row(true);

  auto add_tab = [&](const StringRefNull name,
                     const int enum_value,
                     const bool is_fixed) -> ui::Button * {
    const int tab_width = tab_button_width(name);

    if (is_fixed) {
      /* All / Current File / Essentials are shown at exactly their text width and never stretched:
       * they are always in the same place, and a constant size is what lets the eye find them
       * without reading. #LayoutRow honors a fixed-size sub-layout -- the library selector holds its
       * first column to the dropdown's width the same way -- so the custom tabs beside them still
       * absorb the row's remaining width, as they did before. */
      ui::Layout &fixed = row->row(true);
      /* Both flags together: #ui_units_x_set gives the sub-row its natural width, and
       * #fixed_size_set is what stops the parent row's #LayoutAlign::Expand from stretching it to an
       * even share (see #ui_litem_estimate_row -- a non-button item is pinned to its own size only
       * when #Item::fixed_size is set). Setting the units alone leaves it a "free size" item that
       * still expands, which is why the tabs came out stretched. */
      fixed.fixed_size_set(true);
      fixed.ui_units_x_set(float(tab_width) / float(UI_UNIT_X));
      ui::block_layout_set_current(block, &fixed);
    }
    else {
      ui::block_layout_set_current(block, row);
    }
    /* The tab drives the shelf's own `asset_library_reference` property instead of a callback: a
     * callback-driven tab is dead inside a popup, because #do_but_TAB only applies one on a
     * #KM_CLICK event and a popup never sees one (#popup_handler consumes every press, so
     * #wm_handlers_do never synthesizes the click). The shelf's permanent catalog tabs get away
     * with callbacks only because they live in a regular region. Driving the property also makes a
     * tab press the exact same action as picking that library in the selector menu, update notifier
     * included. */
    ui::Button *but = uiDefButR_prop(block,
                                     ui::ButtonType::Tab,
                                     name,
                                     0,
                                     0,
                                     tab_width,
                                     UI_UNIT_Y,
                                     &shelf_ptr,
                                     library_prop,
                                     -1,
                                     0,
                                     enum_value,
                                     TIP_("Show this asset library in the shelf"));
    ui::button_flag_disable(but, ui::BUT_UNDO);
    return but;
  };

  int current_row = 0;
  for (const PinnedLibraryTab &tab : pinned) {
    if (tab.row != current_row) {
      row = &layout.row(true);
      current_row = tab.row;
    }

    ui::Button *but = add_tab(tab.name, tab.enum_value, tab.is_fixed);

    /* The pushed state is stated explicitly rather than left to the default enum comparison:
     * #button_is_pushed_ex reads a tab that has both an RNA property and #custom_data (set below,
     * for the context menu) as a workspace-style pointer tab, which never matches an enum
     * property. */
    if (tab.is_fixed) {
      const eAssetLibraryType type = eAssetLibraryType(tab.enum_value);
      ui::button_func_pushed_state_set(but, [&shelf_settings, type](const ui::Button &) -> bool {
        return shelf_settings.asset_library_reference.type == type;
      });
      /* "All" gets no menu: it can be neither moved nor unpinned, so there is nothing to put in
       * one. Every other built-in tab is here because it is pinned, so it must have a payload --
       * if it does not, the table above has drifted from BKE's list of pinnable built-ins. */
      if (BKE_preferences_asset_builtin_pin_supported(type)) {
        const eAssetLibraryType *menu_payload = builtin_tab_menu_type_ptr(type);
        BLI_assert(menu_payload != nullptr);
        if (menu_payload) {
          ui::button_tab_menu_set(
              but,
              WM_menutype_find("ASSETSHELF_MT_builtin_library_tab_context", true),
              const_cast<eAssetLibraryType *>(menu_payload));
        }
      }
      continue;
    }

    /* A custom tab's lambda may be evaluated long after this draw: the popover is #BLOCK_KEEP_OPEN
     * and Preferences opens in a separate window, so a library can be deleted from underneath an
     * open popover. #asset_shelf_popover_listen rebuilds the row when that happens, but only at the
     * next draw -- this can run before it. Resolve by name, not by the pointer, matching
     * #library_from_op_props in `userpref_ops.cc`. */
    const std::string library_name = tab.name;
    ui::button_func_pushed_state_set(
        but, [&shelf_settings, library_name](const ui::Button &) -> bool {
          bUserAssetLibrary *library = BKE_preferences_asset_library_find_by_name(
              &U, library_name.c_str());
          /* Not pushed once the library is gone, rather than reading a dangling pointer. */
          if (!library) {
            return false;
          }
          /* Resolve by identity, not by the stored index, which may be stale. */
          return BKE_preferences_asset_library_find_from_ref(
                     &U, &shelf_settings.asset_library_reference) == library;
        });
    /* Right-click opens the reorder / unpin menu, acting on this tab's library. The library can be
     * freed while this button survives (see above); the menu draw itself checks list membership
     * before it is dereferenced. */
    ui::button_tab_menu_set(
        but, WM_menutype_find("ASSETSHELF_MT_library_tab_context", true), tab.library);
  }

  ui::block_layout_set_current(block, &layout);
}

/** \} */

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

  const int tile_w_px = std::max(1, tile_width(shelf->settings));
  const float tile_w_units = float(tile_w_px) / float(UI_UNIT_X);
  const float scroll_gutter_units = float(V2D_SCROLL_WIDTH) / float(UI_UNIT_X);

  /* Right (grid) column budget in pixels: the user's width setting, capped so the whole popover
   * (catalog column + vertical grip + grid column) fits within the width available from the pinned
   * left edge to the right window margin. Working in pixels/floats and reserving the grip exactly
   * (rather than the earlier whole-unit slack) keeps the column count idempotent across the
   * position-dependent refresh: on first open the budget is the full usable window width, but once
   * the block is positioned the budget becomes its own settled width, and recomputing #grid_cols
   * from that must reproduce the same value instead of dropping one -- which shrank the popover off
   * the window edge one frame after it opened. */
  const float avail_total_px = float(ui::popup_block_left_anchored_budget_px(win, popover_block));
  const float catalog_px = float(catalog_units) * float(UI_UNIT_X);
  const float grip_px = CATALOG_GRIP_WIDTH_UNITS * float(UI_UNIT_X);
  const float user_right_px = float(std::max(int(shelf->settings.popup_width_units), 20)) *
                              float(UI_UNIT_X);
  const float right_budget_px = std::min(user_right_px, avail_total_px - catalog_px - grip_px);

  /* Ensure the assertion doesn't fail if the window is extremely small. */
  if (right_budget_px < float(tile_w_px)) {
    return;
  }

  /* Snap the right column to a whole number of grid tile columns so the previews fill it exactly,
   * with no gap between the last column and the popover edge (most visible while resizing width
   * with the grip). #grid_cols is forwarded to the grid as a hint so a float rounding at the column
   * boundary cannot drop it to one fewer column and reopen the gap.
   *
   * A #V2D_SCROLL_WIDTH gutter is reserved on top of the whole columns: when the grid overflows, its
   * overlay scrollbar (right-aligned over the grid column) lands in this gutter beside the tiles
   * instead of covering the last column, and the popover edge still sits right after the scrollbar
   * (no gap). When it does not overflow the gutter is just a thin empty margin, as for any reserved
   * scrollbar. */
  const int grid_cols = std::max(
      1, int((right_budget_px - float(V2D_SCROLL_WIDTH)) / float(tile_w_px)));
  const float right_col_width_units = float(grid_cols) * tile_w_units + scroll_gutter_units;

  bScreen *screen = CTX_wm_screen(C);
  /* Make the shelf accessible to nested popovers (e.g. display settings panel). Built here, ahead
   * of the layout below, because #pinned_tabs_gather (immediately below) needs it to read the
   * shelf's filtered library enum. */
  PointerRNA shelf_ptr = RNA_pointer_create_discrete(&screen->id, RNA_AssetShelf, shelf);

  /* The tab row spans the whole popover, both columns. */
  const int tabs_avail_width_px = int(
      (float(catalog_units) + CATALOG_GRIP_WIDTH_UNITS + right_col_width_units) * UI_UNIT_X);
  int tab_row_num = 0;
  const Vector<PinnedLibraryTab> pinned_libraries = pinned_tabs_gather(
      *C, shelf_ptr, tabs_avail_width_px, &tab_row_num);

  ui::Layout &layout = *panel->layout;
  layout.ui_units_x_set(float(catalog_units) + CATALOG_GRIP_WIDTH_UNITS + right_col_width_units);

  PointerRNA library_ref_ptr = RNA_pointer_create_discrete(
      &screen->id, RNA_AssetLibraryReference, &shelf->settings.asset_library_reference);
  layout.context_ptr_set("asset_library_reference", &library_ref_ptr);

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
  /* Header + gap consumed above the grid: the search / preset row (~1 unit), the ~1-unit gap below
   * it where the persistent scroll-up arrow is drawn, and the pinned tab rows. Counting the tab
   * rows here is what keeps a zoomed popover inside the window -- this figure is what
   * #popup_grid_fixed_viewport_units shrinks the grid against. */
  const float non_grid_units = 2.0f + float(tab_row_num);
  /* User-set popover height (grid-viewport units) overrides the default, clamped to the window.
   * 0 means "not set" — use the type default. */
  const float default_grid_units = (shelf->settings.popup_height_units > 0) ?
                                       float(layout_height_units_clamped(
                                           win, shelf->settings.popup_height_units)) :
                                       ASSET_SHELF_POPUP_GRID_DEFAULT_UNITS_Y;
  const float grid_units = ui::popup_grid_fixed_viewport_units(
      C, layout.block(), non_grid_units, tile_units, default_grid_units);
  const int grid_viewport_px = std::max(tile_h_px, int(grid_units * UI_UNIT_Y));

  pinned_tabs_draw(layout, *shelf, shelf_ptr, pinned_libraries);

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
                                                 ICON_GRIP_V,
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
  /* Only meaningful on brush shelves (only they have favorites lists), and greyed out while the
   * "Favorites" pseudo-catalog is itself active, since that view is already favorites-only. */
  if (shelf_idname_is_brush_shelf(shelf->type->idname)) {
    controls.label(IFACE_("Filter:"), ICON_NONE);
    controls.separator();
    ui::Layout &favorites_only_row = controls.row(true);
    /* #fixed_size_set() / #alignment_set() don't propagate from #controls to this nested row (it
     * exists only to scope #enabled_set() to this one button, see below), so without repeating them
     * here this button would size differently from the preset buttons that follow. */
    favorites_only_row.fixed_size_set(true);
    favorites_only_row.alignment_set(ui::LayoutAlign::Right);
    favorites_only_row.enabled_set(!settings_is_favorites_catalog_active(shelf->settings));
    favorites_only_row.prop(&shelf_ptr,
                            "filter_favorites_only",
                            ui::ITEM_R_TOGGLE,
                            IFACE_("Only Favorite"),
                            ICON_SOLO_ON);
    /* Detach the favorites filter (a view toggle) from the "Size:" group that follows, which is a
     * different kind of control -- otherwise they read as one merged button group. */
    controls.separator();
  }
  controls.label(IFACE_("Size:"), ICON_NONE);
  /* Detach the label from the preset buttons it introduces, matching the "Filter:" label above. */
  controls.separator();
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
      /* #ND_ASSET_CATALOGS also covers the pseudo-catalogs (Recent/Favorites): an open popover has
       * to rebuild for a favorite toggled from outside of it (brush context menu, Python, another
       * window). A popup only rebuilds on #RGN_REFRESH_UI, so tagging a redraw is not enough. */
      if (ELEM(wmn->data, ND_ASSET_LIST_READING, ND_ASSET_LIST_PREVIEW, ND_ASSET_CATALOGS)) {
        ED_region_tag_refresh_ui(region);
      }
      /* A brush activated from outside the popover (hotkey, Python, brush context menu, another
       * window) changes the Recent pseudo-catalog; rebuild so an open popover reflects it. */
      else if (wmn->action == NA_ACTIVATED) {
        ED_region_tag_refresh_ui(region);
      }
      break;
    case NC_SPACE:
      /* #ND_REGIONS_ASSET_SHELF: redraw to apply changes to preview size or show names instantly.
       *
       * #ND_SPACE_ASSET_PARAMS: the Preferences' asset library list itself changed (see
       * #PREFERENCES_OT_asset_library_remove). The pinned tab row is built from that list, and the
       * `asset_library_reference` enum values the tabs drive are positional
       * (#library_reference_to_enum_value counts an index into #UserDef.asset_libraries), so any
       * add/remove/reorder shifts them. A tab left over from the old list would not just dangle --
       * it would silently switch the shelf to a *different* library. Rebuild instead. */
      if (ELEM(wmn->data, ND_REGIONS_ASSET_SHELF, ND_SPACE_ASSET_PARAMS)) {
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
    /* Offset so mouse is below search button, over the first row of assets. The pinned library tab
     * row sits above the search row and pushes the grid down by its height; one row is the common
     * case, so that is what this compensates for. When many pins wrap onto further rows the open
     * position drifts, which is accepted -- this is static and the wrap count is not. */
    pt->offset_units_xy.y = 3.5f;
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

  /* Context menu for the pinned library tabs. Registered here with the panels because it shares
   * their lifetime; #WM_menutype_find is what the tab looks it up with. */
  library_tab_context_menu_register();
}

}  // namespace blender::ed::asset::shelf
