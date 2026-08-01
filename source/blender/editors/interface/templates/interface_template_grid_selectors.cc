/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Composable header widgets for reusable grid views: asset-library dropdown, catalog filter
 * popover, name-match filter popover, and preview-size control. Each operates on a
 * #GridViewSettings #PointerRNA.
 */

#include "interface_grid_view_settings_utils.hh"

#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_name_matching.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BLT_translation.hh"

#include "ED_asset_list.hh"
#include "ED_screen.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include <string>
#include <utility>

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Panel registration (lazy, global popover panels)
 * \{ */

static void grid_catalog_selector_panel_register();
static void grid_preview_size_panel_register();
static void grid_name_match_operators_register();
static void grid_name_match_filter_panel_register();

static void ensure_grid_panels_registered()
{
  /* Popovers are found via the global WM type registry; no space/region binding needed. */
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  grid_name_match_operators_register();
  grid_catalog_selector_panel_register();
  grid_preview_size_panel_register();
  grid_name_match_filter_panel_register();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Catalog selector tree (backed by #GridViewSettings)
 * \{ */

class GridCatalogSelectorTree : public AbstractTreeView {
  const bContext &C_;
  PointerRNA settings_;
  std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree_;

 public:
  class AllItem;
  class Item;

  GridCatalogSelectorTree(const bContext &C,
                          PointerRNA settings,
                          const asset_system::AssetLibrary &library)
      : C_(C), settings_(settings)
  {
    catalog_tree_ = library.catalog_service().catalog_tree();
  }

  void build_tree() override;

  void update_enabled_catalogs_from_items(bContext &C);

  static Item &build_catalog_items_recursive(
      TreeViewOrItem &parent,
      const asset_system::AssetCatalogTreeItem &catalog_item,
      PointerRNA settings);

  class AllItem : public BasicTreeViewItem {
    PointerRNA settings_;

   public:
    AllItem(PointerRNA settings) : BasicTreeViewItem(IFACE_("All")), settings_(settings)
    {
      set_on_activate_fn([this](bContext &C, BasicTreeViewItem & /*item*/) {
        grid_settings::enabled_catalogs_clear(settings_);
        if (ARegion *region = CTX_wm_region(&C)) {
          ED_region_tag_redraw(region);
          ED_region_tag_refresh_ui(region);
        }
      });
      set_is_active_fn(
          [this]() -> bool { return grid_settings::enabled_catalogs_get(settings_).is_empty(); });
    }
  };

  class Item : public BasicTreeViewItem {
    const asset_system::AssetCatalogTreeItem &catalog_item_;
    char catalog_path_enabled_ = false;

   public:
    Item(const asset_system::AssetCatalogTreeItem &catalog_item, PointerRNA settings)
        : BasicTreeViewItem(catalog_item.get_name()),
          catalog_item_(catalog_item),
          catalog_path_enabled_(
              grid_settings::is_catalog_path_enabled(settings, catalog_item.catalog_path().str()) ?
                  1 :
                  0)
    {
      /* Clicking anywhere in the row toggles the catalog, not just the checkbox — precise checkbox
       * hits are hard with a stylus. The checkbox (#build_row) keeps its own click handling; it and
       * the row's full-width #AbstractTreeViewItem::add_treerow_button are separate buttons, and
       * per-pixel hit-testing resolves to whichever is topmost, so a checkbox click is never also
       * counted as a row click. */
      set_on_activate_fn([this](bContext &C, BasicTreeViewItem & /*item*/) {
        GridCatalogSelectorTree &tree = dynamic_cast<GridCatalogSelectorTree &>(get_tree_view());
        catalog_path_enabled_ = !catalog_path_enabled_;
        tree.update_enabled_catalogs_from_items(C);
      });
    }

    bool is_catalog_path_enabled() const
    {
      return catalog_path_enabled_ != 0;
    }

    bool has_enabled_in_subtree()
    {
      bool has_enabled = false;
      foreach_item_recursive(
          [&has_enabled](const AbstractTreeViewItem &abstract_item) {
            const Item *item = dynamic_cast<const Item *>(&abstract_item);
            if (item && item->is_catalog_path_enabled()) {
              has_enabled = true;
            }
          },
          IterOptions::SkipFiltered);
      return has_enabled;
    }

    asset_system::AssetCatalogPath catalog_path() const
    {
      return catalog_item_.catalog_path();
    }

    void build_row(Layout &row) override
    {
      GridCatalogSelectorTree &tree = dynamic_cast<GridCatalogSelectorTree &>(get_tree_view());
      Block *block = row.block();

      row.emboss_set(EmbossType::Emboss);

      Layout &subrow = row.row(false);
      subrow.active_set(catalog_path_enabled_);
      subrow.label(catalog_item_.get_name(), ICON_NONE);
      block_layout_set_current(block, &row);

      Button *toggle_but = uiDefButV(block,
                                     ButtonType::Checkbox,
                                     "",
                                     0,
                                     0,
                                     short(UI_UNIT_X),
                                     short(UI_UNIT_Y),
                                     &catalog_path_enabled_,
                                     0,
                                     0,
                                     TIP_("Toggle catalog visibility in the grid"));
      button_func_set(toggle_but,
                      [&tree](bContext &C) { tree.update_enabled_catalogs_from_items(C); });
      if (!is_catalog_path_enabled() && has_enabled_in_subtree()) {
        button_drawflag_enable(toggle_but, BUT_INDETERMINATE);
      }
      button_flag_disable(toggle_but, BUT_UNDO);
    }
  };
};

void GridCatalogSelectorTree::update_enabled_catalogs_from_items(bContext &C)
{
  Set<std::string> enabled;
  foreach_item([&](AbstractTreeViewItem &view_item) {
    const Item *item = dynamic_cast<Item *>(&view_item);
    if (item && item->is_catalog_path_enabled()) {
      enabled.add_new(item->catalog_path().str());
    }
  });
  grid_settings::enabled_catalogs_set(settings_, enabled);

  if (ARegion *region = CTX_wm_region(&C)) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
}

void GridCatalogSelectorTree::build_tree()
{
  add_tree_item<AllItem>(settings_).uncollapse_by_default();

  if (!catalog_tree_ || catalog_tree_->is_empty()) {
    return;
  }

  catalog_tree_->foreach_root_item([this](const asset_system::AssetCatalogTreeItem &cat_item) {
    Item &item = build_catalog_items_recursive(*this, cat_item, settings_);
    item.uncollapse_by_default();
  });
}

GridCatalogSelectorTree::Item &GridCatalogSelectorTree::build_catalog_items_recursive(
    TreeViewOrItem &parent,
    const asset_system::AssetCatalogTreeItem &catalog_item,
    PointerRNA settings)
{
  Item &item = parent.add_tree_item<Item>(catalog_item, settings);

  catalog_item.foreach_child([&](const asset_system::AssetCatalogTreeItem &child) {
    build_catalog_items_recursive(item, child, settings);
  });

  return item;
}

static void grid_catalog_selector_panel_draw(const bContext *C, Panel *panel)
{
  PointerRNA settings_ptr = CTX_data_pointer_get(C, "grid_view_settings");
  if (!settings_ptr.data) {
    return;
  }

  Layout &layout = *panel->layout;
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);

  const AssetLibraryReference lib_ref = grid_settings::library_ref_get(settings_ptr);
  ed::asset::list::storage_fetch(&lib_ref, C);

  Layout &row = layout.row(true);
  template_asset_library_column_selector(row, C, &settings_ptr, "asset_library_reference", ICON_NONE);
  if (lib_ref.type != ASSET_LIBRARY_LOCAL) {
    row.op("ASSET_OT_library_refresh", "", ICON_FILE_REFRESH);
  }

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  if (!library) {
    layout.label(IFACE_("Loading\xe2\x80\xa6"), ICON_NONE);
    return;
  }

  Block *block = layout.block();
  AbstractTreeView *tree_view = block_add_view(
      *block,
      "grid_catalog_selector",
      std::make_unique<GridCatalogSelectorTree>(*C, settings_ptr, *library));
  TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

static void grid_catalog_selector_panel_register()
{
  if (WM_paneltype_find("GRIDVIEW_PT_catalog_selector", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "GRIDVIEW_PT_catalog_selector");
  STRNCPY_UTF8(pt->label, N_("Catalog Selector"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select asset catalogs to display in the grid");
  pt->draw = grid_catalog_selector_panel_draw;
  pt->listener = ed::asset::list::asset_reading_region_listen_fn;
  /* Not bound to any region type list — popovers are located via the global type registry. */
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview-size popover panel
 * \{ */

static void grid_preview_size_panel_draw(const bContext *C, Panel *panel)
{
  PointerRNA settings_ptr = CTX_data_pointer_get(C, "grid_view_settings");
  if (!settings_ptr.data) {
    return;
  }

  Layout &layout = *panel->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(&settings_ptr, "preview_size", UI_ITEM_NONE, IFACE_("Size"), ICON_NONE);
}

static void grid_preview_size_panel_register()
{
  if (WM_paneltype_find("GRIDVIEW_PT_preview_size", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "GRIDVIEW_PT_preview_size");
  STRNCPY_UTF8(pt->label, N_("Display Settings"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Adjust preview tile size for the grid");
  pt->draw = grid_preview_size_panel_draw;
  /* Not bound to any region type list — popovers are located via the global type registry. */
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Name-match filter (toggle + popover; map types only)
 * \{ */

static PointerRNA grid_view_settings_from_context(const bContext *C)
{
  return CTX_data_pointer_get(C, "grid_view_settings");
}

static wmOperatorStatus name_match_map_type_toggle_exec(bContext *C, wmOperator *op)
{
  PointerRNA settings = grid_view_settings_from_context(C);
  if (settings.data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  char identifier[64];
  RNA_string_get(op->ptr, "identifier", identifier);
  grid_settings::name_match_settings_toggle_map_type(settings, identifier);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
  return OPERATOR_FINISHED;
}

static void GRIDVIEW_OT_name_match_map_type_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Name Match Map Type";
  ot->idname = "GRIDVIEW_OT_name_match_map_type_toggle";
  ot->description = "Toggle a map type in the grid view name matching filter";
  ot->exec = name_match_map_type_toggle_exec;
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_string(ot->srna, "identifier", nullptr, 64, "Identifier", "");
}

static wmOperatorStatus name_match_clear_exec(bContext *C, wmOperator * /*op*/)
{
  PointerRNA settings = grid_view_settings_from_context(C);
  if (settings.data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  grid_settings::name_match_settings_clear_selection(settings);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
  return OPERATOR_FINISHED;
}

static void GRIDVIEW_OT_name_match_clear(wmOperatorType *ot)
{
  ot->name = "Clear Name Match Filter";
  ot->idname = "GRIDVIEW_OT_name_match_clear";
  ot->description = "Clear active name matching map-type selections on the grid view";
  ot->exec = name_match_clear_exec;
  ot->flag = OPTYPE_INTERNAL;
}

static void grid_name_match_operators_register()
{
  if (WM_operatortype_find("GRIDVIEW_OT_name_match_map_type_toggle", false)) {
    return;
  }
  WM_operatortype_append(GRIDVIEW_OT_name_match_map_type_toggle);
  WM_operatortype_append(GRIDVIEW_OT_name_match_clear);
}

static void grid_name_match_panel_draw(const bContext *C, Panel *panel)
{
  PointerRNA settings = CTX_data_pointer_get(C, "grid_view_settings");
  if (settings.data == nullptr) {
    return;
  }

  Layout &layout = *panel->layout;
  /* Builtins are seeded once at defaults/versioning/homefile-read time
   * (#BKE_name_matching_userdef_ensure_defaults) and, since built-in rows can't be removed (see
   * #BKE_name_matching_map_type_remove), the list is guaranteed well-formed here without
   * re-checking on every redraw. */
  Vector<std::pair<std::string, std::string>> map_type_rows;
  for (const bUserNameMatchMapType &map_type : U.name_match_map_types) {
    if (map_type.identifier[0] != '\0') {
      map_type_rows.append(
          {map_type.identifier, map_type.name[0] != '\0' ? map_type.name : map_type.identifier});
    }
  }

  const NameMatchFilterState state = grid_settings::name_match_filter_get(settings);
  layout.enabled_set(state.enabled);
  layout.label(IFACE_("Map Types"), ICON_NONE);
  for (const auto &[identifier, name] : map_type_rows) {
    const bool active = BKE_name_match_filter_map_type_is_active(state, identifier);
    Layout &row = layout.row(false);
    PointerRNA props = row.op("GRIDVIEW_OT_name_match_map_type_toggle",
                              name,
                              active ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT);
    if (props.type != nullptr) {
      RNA_string_set(&props, "identifier", identifier.c_str());
    }
  }
  layout.separator();
  layout.op("GRIDVIEW_OT_name_match_clear", IFACE_("Clear Filter"), ICON_X);
}

static void grid_name_match_filter_panel_register()
{
  if (WM_paneltype_find("GRIDVIEW_PT_name_match_filter", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "GRIDVIEW_PT_name_match_filter");
  STRNCPY_UTF8(pt->label, N_("Name Match Filter"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select map types for name matching in the grid");
  pt->draw = grid_name_match_panel_draw;
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared library menu item
 * \{ */

/* Seed the menu's return value with the library that is already active.
 *
 * These menus are drawn for an RNA enum button (#ButtonType::Menu), and when the menu closes that
 * parent button unconditionally applies #PopupBlockHandle.retvalue to the property
 * (#apply_but_BLOCK). Only a library choice writes that field, so any other button that closes the
 * menu -- the pin toggle -- would leave it at its initial 0, which is not a valid asset-library
 * enum value and trips the assert in #ed::asset::library_reference_from_enum_value.
 *
 * Seeding it with the current value makes such a close apply the library that is already set, i.e.
 * a no-op. A real choice overwrites the seed before the menu closes.
 *
 * \note "No-op" is about the stored value only. The apply still runs: RNA does not short-circuit a
 * write of an equal value, so closing the menu by clicking a pin re-sets
 * `asset_library_reference`, fires its update callback and pushes undo on the parent button
 * (#BUT_UNDO is cleared on the pin itself, not on the button the menu belongs to). That is
 * invisible but not free. It is the price of the pin closing the menu, which is a deliberate
 * choice -- the alternatives were traced and are worse -- so fixing it means revisiting that
 * choice, not this seed. */
static void library_selector_seed_retvalue(PopupBlockHandle *handle, const int current_value)
{
  if (handle) {
    handle->retvalue = current_value;
  }
}

/* Whether any selectable item in the enum has an icon (used to align icon-less items). */
static bool library_enum_has_item_with_icon(const EnumPropertyItem *items)
{
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (item->identifier[0] && item->icon) {
      return true;
    }
  }
  return false;
}

/* Add one asset-library choice as a menu entry: a #ButtonType::ButMenu that returns the item value
 * through the popup handle, highlighted with the active color (#UI_SELECT_DRAW) when it is the
 * current library. Modeled on the built-in enum dropdown (#def_but_rna__menu): the parent RNA enum
 * button applies the returned value to the property (with undo) when the menu closes, so this only
 * has to draw the row -- no radio/checkbox as #Layout::prop_enum would produce. */
static void library_selector_menu_item(Block *block,
                                       Layout &column,
                                       PopupBlockHandle *handle,
                                       const EnumPropertyItem &item,
                                       const int current_value,
                                       const bool has_item_with_icon,
                                       const bool show_pin)
{
  int icon = item.icon;
  if (icon == ICON_NONE && has_item_with_icon) {
    /* Keep labels aligned with items that do have an icon. */
    icon = ICON_BLANK1;
  }

  /* One row per entry: the choice button fills it, the pin sits at its right edge. Must be
   * aligned (`row(true)`): #block_bounds_calc_text lays the surrounding menu out in columns by
   * comparing each button's `xmin` against the next, and would otherwise mistake the boundary
   * between the choice and pin buttons for a real column break (splitting every pinnable entry
   * into its own column and starving #Block.menu_first_col_minwidth after the first). Aligning
   * gives both buttons a shared #Button.alignnr, which #but_is_row_alignment_group() (interface.cc)
   * uses to skip over the pair as one unit instead of splitting it. */
  Layout &item_row = column.row(true);
  block_layout_set_current(block, &item_row);

  Button *but;
  if (icon) {
    but = uiDefIconTextBut(block,
                           ButtonType::ButMenu,
                           icon,
                           item.name,
                           0,
                           0,
                           short(UI_UNIT_X * 5),
                           short(UI_UNIT_Y),
                           &handle->retvalue,
                           std::nullopt);
  }
  else {
    /* Height is #UI_UNIT_Y (row height), not #UI_UNIT_X as the built-in #def_but_rna__menu passes
     * in its icon-less branch (that looks like a long-standing typo there). */
    but = uiDefButV(block,
                    ButtonType::ButMenu,
                    item.name,
                    0,
                    0,
                    short(UI_UNIT_X * 5),
                    short(UI_UNIT_Y),
                    &handle->retvalue,
                    0.0,
                    0.0,
                    std::nullopt);
  }

  button_enum_prop_value_set(but, item.value);

  /* Restore the tooltip the built-in enum dropdown (#def_but_rna__menu) shows: for asset libraries
   * this is the library path/URL, a useful hint. The enum items are freed after this draw
   * (#RNA_property_enum_items_gettexted with `free`), so store a copy rather than referencing the
   * item string directly. */
  if (item.description && item.description[0]) {
    char *description_copy = BLI_strdup(item.description);
    button_func_tooltip_set(
        but,
        [](bContext * /*C*/, void *argN, const StringRef /*tip*/) -> std::string {
          return static_cast<const char *>(argN);
        },
        description_copy,
        MEM_delete_void);
  }

  /* Pin toggle. Only drawn for hosts that show the pinned libraries (the asset shelf popover's tab
   * row); everywhere else this selector is reused the toggle would change state with nothing on
   * screen to show for it.
   *
   * "All" gets no toggle: it is always the first tab and cannot be unpinned. Folders never reach
   * here at all -- they are not selectable leaves.
   *
   * This fires the operator rather than writing the flag or the bit directly: the operator owns
   * marking the Preferences dirty and notifying open popovers, and this keeps that in one place. */
  if (show_pin) {
    const bUserAssetLibrary *user_library = nullptr;
    eAssetLibraryType builtin_type = ASSET_LIBRARY_ALL;
    bool has_pin = false;
    bool is_pinned = false;

    if (item.value >= ASSET_LIBRARY_CUSTOM) {
      const AssetLibraryReference library_ref = ed::asset::library_reference_from_enum_value(
          item.value);
      user_library = BKE_preferences_asset_library_find_from_ref(&U, &library_ref);
      if (user_library) {
        has_pin = true;
        is_pinned = (user_library->flag & ASSET_LIBRARY_IS_PINNED) != 0;
      }
    }
    else {
      /* BKE owns which built-ins have a pin at all (see #asset_builtin_pin_flag_from_type); asking
       * it means the rule is not restated here. */
      builtin_type = eAssetLibraryType(item.value);
      if (BKE_preferences_asset_builtin_pin_supported(builtin_type)) {
        has_pin = true;
        is_pinned = BKE_preferences_asset_builtin_pin_get(&U, builtin_type);
      }
    }

    if (has_pin) {
      Button *pin_but = uiDefIconButO(
          block,
          ButtonType::But,
          "PREFERENCES_OT_asset_library_pin_set",
          wm::OpCallContext::ExecDefault,
          is_pinned ? ICON_PINNED : ICON_UNPINNED,
          0,
          0,
          short(UI_UNIT_X),
          short(UI_UNIT_Y),
          is_pinned ? TIP_("Unpin this library from the asset shelf popover") :
                      TIP_("Pin this library as a tab at the top of the asset shelf popover"));
      PointerRNA *pin_opptr = button_operator_ptr_ensure(pin_but);
      if (user_library) {
        RNA_enum_set(pin_opptr, "library_type", ASSET_LIBRARY_CUSTOM);
        RNA_string_set(pin_opptr, "library_name", user_library->name);
      }
      else {
        RNA_enum_set(pin_opptr, "library_type", int(builtin_type));
      }
      /* The state to apply is decided here, where the icon showing it is also decided, so the two
       * can never disagree. */
      RNA_boolean_set(pin_opptr, "pinned", !is_pinned);
      button_flag_disable(pin_but, BUT_UNDO);
    }
  }

  if (item.value == current_value) {
    button_flag_enable(but, UI_SELECT_DRAW);
  }
}

/* Draw the standard RNA enum dropdown button (#ButtonType::Menu) for `prop_name` on `ptr`, then
 * replace its built-in #def_but_rna__menu layout with `draw_fn`. Reusing the real enum button keeps
 * the reliable open-in-any-region behavior and the native return-value apply (with undo); the
 * drawer only customizes how the choices are laid out. */
static void library_selector_menu_button(Layout &row,
                                          PointerRNA *ptr,
                                          const StringRefNull prop_name,
                                          const int icon,
                                          MenuCreateFunc draw_fn)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, prop_name.c_str());
  if (!prop) {
    return;
  }

  Block *block = row.block();
  const int64_t first_new = block->buttons_ptrs.size();
  row.prop(ptr, prop_name, UI_ITEM_NONE, "", icon);

  for (int64_t i = first_new; i < block->buttons_ptrs.size(); i++) {
    Button *but = block->buttons_ptrs[i].get();
    if (but->rnaprop == prop && but->type == ButtonType::Menu) {
      but->menu_create_func = draw_fn;
      break;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Library selector menu (vertical, backed by #GridViewSettings)
 * \{ */

/* Draw the asset-library choices as a plain vertical menu instead of the RNA enum dropdown. The
 * enum dropdown lays folder headings out as side-by-side columns (#def_but_rna__menu); a menu reads
 * top to bottom. Folder headings become labels, and each library is a row. Called with the calling
 * RNA enum button (#Button::poin), so the source pointer and property are read live from it (no
 * owned copy that could dangle across popover redraws). */
static void grid_library_selector_menu_draw(bContext *C, Layout *layout, void *but_p)
{
  Button *but = static_cast<Button *>(but_p);
  Block *block = layout->block();
  PopupBlockHandle *handle = block->handle;

  PointerRNA ptr = but->rnapoin;
  PropertyRNA *prop = but->rnaprop;

  const EnumPropertyItem *items = nullptr;
  bool free = false;
  RNA_property_enum_items_gettexted(C, &ptr, prop, &items, nullptr, &free);
  if (!items) {
    return;
  }

  block_flag_enable(block, BLOCK_MOVEMOUSE_QUIT);
  block_layout_set_current(block, layout);

  const int current_value = RNA_property_enum_get(&ptr, prop);
  library_selector_seed_retvalue(handle, current_value);
  const bool has_item_with_icon = library_enum_has_item_with_icon(items);

  Layout &col = layout->column(false);
  /* Start "separated" so a leading folder heading gets no divider above it. */
  bool prev_was_separator = true;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    /* Empty identifier: a folder heading (has a name) or a plain separator (no name). */
    if (!item->identifier[0]) {
      if (item->name && item->name[0]) {
        /* Divider before each folder name, unless one was just drawn (avoids doubling the
         * built-in separator before the custom section). */
        if (!prev_was_separator) {
          col.separator();
        }
        col.label(item->name, item->icon);
        prev_was_separator = false;
      }
      else {
        col.separator();
        prev_was_separator = true;
      }
      continue;
    }
    /* No pin: this vertical selector has no host that shows the pinned libraries. */
    library_selector_menu_item(
        block, col, handle, *item, current_value, has_item_with_icon, /*show_pin=*/false);
    prev_was_separator = false;
  }

  if (free) {
    MEM_delete(items);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Columnar library selector (folders as side-by-side columns)
 * \{ */

/* Draw the asset-library choices as side-by-side columns (one per folder), like the RNA enum
 * dropdown but with the first (folder-less) column pinned to the calling button's width. The
 * built-in dropdown (#def_but_rna__menu) splits the width evenly and re-sizes columns to their
 * content, so it can't hold the first column to the button; a plain #Layout::row of #Layout::column
 * items can, because #LayoutRow honors a fixed-size column. Called with the calling RNA enum button
 * (#Button::poin); the source pointer and property are read live from it.
 *
 * \param show_pins: see #template_asset_library_column_selector. A #MenuCreateFunc takes no extra
 * arguments, hence the two thin wrappers below rather than a parameter on the drawer itself. */
static void asset_library_column_menu_draw_impl(bContext *C,
                                                Layout *layout,
                                                void *but_p,
                                                const bool show_pins)
{
  Button *but = static_cast<Button *>(but_p);
  Block *block = layout->block();
  PopupBlockHandle *handle = block->handle;

  PointerRNA ptr = but->rnapoin;
  PropertyRNA *prop = but->rnaprop;

  /* The enum's itemf may read the calling button's context store (e.g. the ID browser's library
   * itemf narrows the list to image libraries via `id_browser_ptr`/`id_browser_prop`). This menu
   * runs from #block_func_POPUP, which copies that store onto the menu layout but not onto \a C, and
   * only applies it to \a C later in #block_layout_resolve -- after the itemf below has already run.
   * Present the button's store to the itemf directly, matching #button_context_poll_operator_ex. */
  const bContextStore *previous_store = CTX_store_get(C);
  if (but->context) {
    CTX_store_set(C, but->context);
  }
  const EnumPropertyItem *items = nullptr;
  bool free = false;
  RNA_property_enum_items_gettexted(C, &ptr, prop, &items, nullptr, &free);
  if (but->context) {
    CTX_store_set(C, previous_store);
  }
  if (!items) {
    return;
  }

  block_flag_enable(block, BLOCK_MOVEMOUSE_QUIT);
  block_layout_set_current(block, layout);

  /* Width of the button that opened this menu (the dropdown), read live from the popup handle. */
  const Button *calling_but = handle ? handle->popup_create_vars.but : nullptr;
  const float but_width = calling_but ? BLI_rctf_size_x(&calling_but->rect) : 0.0f;

  /* Pin the first (folder-less) column to the dropdown's width. A popup menu (#BLOCK_BOUNDS_POPUP_MENU)
   * is re-sized to its widest text per column in #block_bounds_calc_text, ignoring any ui_units_x /
   * fixed_size set on a column. The only lever that pass honors is #Block.menu_first_col_minwidth,
   * so hand the button width to it, capped so a very wide dropdown doesn't oversize the column. */
  if (but_width > 0.0f) {
    /* Upper bound so an unusually wide dropdown button doesn't stretch the first column across the
     * whole menu; ten units comfortably fits a typical library name while staying compact. */
    const int max_first_col_width = 10 * UI_UNIT_X;
    int pinned_width = int(but_width);
    if (pinned_width > max_first_col_width) {
      pinned_width = max_first_col_width;
    }
    block->menu_first_col_minwidth = pinned_width;
  }

  const int current_value = RNA_property_enum_get(&ptr, prop);
  library_selector_seed_retvalue(handle, current_value);
  const bool has_item_with_icon = library_enum_has_item_with_icon(items);

  /* What each column needs beyond its widest text. #block_bounds_calc_text measures the text alone,
   * so this padding has to carry everything the widget puts around it, or the longest name in each
   * column gets ellipsized. The figures come from the draw code, not from the layout's estimator
   * (whose `text_pad_none` is about a different question and is short of what is needed here):
   *
   * - 0.125 each side: the menu item's own box padding. #widget_menu_itembut shrinks the rect in
   *   place and the text is then drawn into that same, smaller rect.
   * - #UI_TEXT_MARGIN_X: the left text margin, applied via #button_text_padding.
   * - 0.25: the margin #text_clip_middle keeps free before it starts ellipsizing
   *   (#UI_TEXT_CLIP_MARGIN). #ButtonType::ButMenu is not one of the types exempt from it.
   * - The icon column, when the items show icons (#widget_draw_text_icon: 0.2 for a menu item plus
   *   the icon and its padding).
   *
   * Cross-check on the derivation: with an icon these sum to 2.0, so with the gap below they land
   * exactly on the 2.5-unit blanket #block_bounds_calc_popup hands the pass -- that blanket is the
   * icon case's requirement, which an ordinary single-column enum menu pays once. This menu draws a
   * column per folder and would pay it in every one, so it says what its own items need instead.
   *
   * WARNING: this is a silent coupling to the draw code, in both directions. Nothing here breaks at
   * compile time if #widget_menu_itembut or #UI_TEXT_CLIP_MARGIN changes its padding -- the figures
   * simply drift out of agreement with what is drawn, and the longest name in a column starts
   * clipping (or gains slack) again. Re-check this block against the draw code after any upstream
   * merge that touches `interface_widgets.cc`. The one cheap test: open the selector with a folder
   * whose longest library name nearly fills its column and confirm it is not ellipsized. */
  {
    const float box_padding_units = 2.0f * 0.125f;
    const float clip_margin_units = 0.25f;
    const float icon_units = has_item_with_icon ? 1.1f : 0.0f;
    const float col_gap_units = 0.5f;
    block->menu_col_padding = int((box_padding_units + UI_TEXT_MARGIN_X + clip_margin_units +
                                   icon_units + col_gap_units) *
                                  UI_UNIT_X);
    /* Where a row's pin stops: exactly the inset #widget_draw gives a menu's separator line
     * (`BLI_rcti_pad(rect, -7 * UI_SCALE_FAC, 0)`), so the pin lines up with the end of the rule
     * that underlines the column's heading instead of sitting on the column boundary. Written the
     * same way the draw writes it: #UI_UNIT_X is not a whole 20 * #UI_SCALE_FAC
     * (#WM_window_dpi_set_userdef rounds it), so a fraction of a unit would not land on the same
     * pixel. Smaller than `col_gap_units` above, so the row's text keeps its slack and stays
     * unclipped. */
    block->menu_col_row_inset = int(7.0f * UI_SCALE_FAC);
  }

  Layout &columns = layout->row(false);
  Layout *column = nullptr;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    /* Empty identifier: a folder heading (has a name) opens a new column; a plain separator is
     * ignored, since the columns themselves separate the groups. */
    if (!item->identifier[0]) {
      if (item->name && item->name[0]) {
        column = &columns.column(false);
        column->label(item->name, item->icon);
        /* Rule line under the heading, matching the built-in enum dropdown (#def_but_rna__menu). */
        column->separator();
      }
      continue;
    }
    if (!column) {
      /* First column: the folder-less root libraries. An empty label aligns this column's rows with
       * the folder columns, which start one row lower under their heading; its width is set via
       * #Block.menu_first_col_minwidth above, not via this placeholder. A separator under the empty
       * label mirrors the heading rule of the folder columns (and #def_but_rna__menu's first column). */
      column = &columns.column(false);
      column->label("", ICON_NONE);
      column->separator();
    }
    library_selector_menu_item(
        block, *column, handle, *item, current_value, has_item_with_icon, show_pins);
  }

  if (free) {
    MEM_delete(items);
  }
}

static void asset_library_column_menu_draw(bContext *C, Layout *layout, void *but_p)
{
  asset_library_column_menu_draw_impl(C, layout, but_p, /*show_pins=*/false);
}

static void asset_library_column_menu_draw_pins(bContext *C, Layout *layout, void *but_p)
{
  asset_library_column_menu_draw_impl(C, layout, but_p, /*show_pins=*/true);
}

void template_asset_library_column_selector(Layout &row,
                                            const bContext * /*C*/,
                                            PointerRNA *ptr,
                                            StringRefNull prop_name,
                                            int icon,
                                            const bool show_pins)
{
  library_selector_menu_button(
      row,
      ptr,
      prop_name,
      icon,
      show_pins ? asset_library_column_menu_draw_pins : asset_library_column_menu_draw);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public template entry points
 * \{ */

void template_grid_library_selector(Layout *layout, bContext *C, PointerRNA *settings_ptr)
{
  if (!layout || !C || !settings_ptr || !settings_ptr->data) {
    return;
  }

  const AssetLibraryReference lib_ref = grid_settings::library_ref_get(*settings_ptr);

  Layout &row = layout->row(true);
  library_selector_menu_button(
      row, settings_ptr, "asset_library_reference", ICON_ASSET_MANAGER, grid_library_selector_menu_draw);

  if (lib_ref.type != ASSET_LIBRARY_LOCAL) {
    row.op("ASSET_OT_library_refresh", "", ICON_FILE_REFRESH);
  }
}

void template_grid_catalog_selector(Layout *layout, bContext *C, PointerRNA *settings_ptr)
{
  if (!layout || !C || !settings_ptr || !settings_ptr->data) {
    return;
  }

  ensure_grid_panels_registered();

  Layout &row = layout->row(false);
  row.emboss_set(EmbossType::Emboss);
  row.ui_units_x_set(1.6f);
  row.context_ptr_set("grid_view_settings", settings_ptr);
  row.popover(C, "GRIDVIEW_PT_catalog_selector", "", ICON_COLLAPSEMENU);

  Block *block = row.block();
  Button *but = block->buttons_ptrs.last().get();
  but->rect.xmax = but->rect.xmin + short(1.6f * UI_UNIT_X);
}

void template_grid_preview_size(Layout *layout, bContext *C, PointerRNA *settings_ptr)
{
  if (!layout || !C || !settings_ptr || !settings_ptr->data) {
    return;
  }

  ensure_grid_panels_registered();

  Layout &row = layout->row(false);
  row.emboss_set(EmbossType::Emboss);
  row.ui_units_x_set(1.6f);
  row.context_ptr_set("grid_view_settings", settings_ptr);
  row.popover(C, "GRIDVIEW_PT_preview_size", "", ICON_IMGDISPLAY);

  Block *block = row.block();
  Button *but = block->buttons_ptrs.last().get();
  but->rect.xmax = but->rect.xmin + short(1.6f * UI_UNIT_X);
}

void template_grid_name_match_filter(Layout *layout, bContext *C, PointerRNA *settings_ptr)
{
  if (!layout || !C || !settings_ptr || !settings_ptr->data) {
    return;
  }

  ensure_grid_panels_registered();

  const NameMatchFilterState state = grid_settings::name_match_filter_get(*settings_ptr);
  Layout &row = layout->row(true);
  row.context_ptr_set("grid_view_settings", settings_ptr);
  row.prop(settings_ptr,
           "filter_name_match_enabled",
           ITEM_R_TOGGLE | ITEM_R_ICON_ONLY,
           "",
           state.enabled ? ICON_FILTER_FILLED : ICON_FILTER);
  /* Outliner-style disclosure on a square icon button; see Host B name-match popover note. */
  row.popover(C, "GRIDVIEW_PT_name_match_filter", "", ICON_DOWNARROW_HLT);
}

/** \} */

}  // namespace blender::ui
