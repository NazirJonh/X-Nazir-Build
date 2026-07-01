/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Composable header widgets for reusable grid views: asset-library dropdown, catalog filter
 * popover, and preview-size control. Each operates on a #GridViewSettings #PointerRNA.
 */

#include "interface_grid_view_settings_utils.hh"

#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "DNA_screen_types.h"

#include "BLT_translation.hh"

#include "ED_asset_list.hh"
#include "ED_screen.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Panel registration (lazy, global popover panels)
 * \{ */

static void grid_catalog_selector_panel_register();
static void grid_preview_size_panel_register();

static void ensure_grid_panels_registered()
{
  /* Popovers are found via the global WM type registry; no space/region binding needed. */
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  grid_catalog_selector_panel_register();
  grid_preview_size_panel_register();
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
      disable_activatable();
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
  row.prop(&settings_ptr, "asset_library_reference", UI_ITEM_NONE, "", ICON_NONE);
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
/** \name Public template entry points
 * \{ */

void template_grid_library_selector(Layout *layout, bContext *C, PointerRNA *settings_ptr)
{
  if (!layout || !settings_ptr || !settings_ptr->data) {
    return;
  }

  Layout &row = layout->row(true);
  row.prop(settings_ptr, "asset_library_reference", UI_ITEM_NONE, "", ICON_ASSET_MANAGER);

  const AssetLibraryReference lib_ref = grid_settings::library_ref_get(*settings_ptr);
  if (lib_ref.type != ASSET_LIBRARY_LOCAL) {
    row.op("ASSET_OT_library_refresh", "", ICON_FILE_REFRESH);
  }
  UNUSED_VARS(C);
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

/** \} */

}  // namespace blender::ui
