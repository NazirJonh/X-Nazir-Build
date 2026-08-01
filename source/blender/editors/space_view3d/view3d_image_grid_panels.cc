/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

#include "BKE_context.hh"
#include "BKE_name_matching.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "DNA_userdef_types.h"

#include "ED_asset_list.hh"
#include "ED_image_grid.hh"
#include "ED_view3d.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

#include <string>
#include <utility>

namespace blender {

using namespace ed::view3d;

/* -------------------------------------------------------------------- */
/** \name Catalog Selector Popover
 * \{ */

/** Tree view listing catalogs of the current image-grid library. Individual catalogs can be
 * enabled or disabled via checkboxes. An empty selection means "show all". */
class ImageGridCatalogSelectorTree : public ui::AbstractTreeView {
  const bContext &C_;
  ed::view3d::ImageGridUIState &state_;
  /* Full catalog tree shared from the library's catalog service. Using a shared_ptr avoids
   * copying the tree (which has raw parent pointers) and ensures the data stays alive. */
  std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree_;

 public:
  class AllItem;
  class Item;

  ImageGridCatalogSelectorTree(const bContext &C,
                               ed::view3d::ImageGridUIState &state,
                               const asset_system::AssetLibrary &library)
      : C_(C), state_(state)
  {
    /* Use the full catalog tree of the library rather than a filtered tree built from
     * catalog IDs of loaded assets. The filtered approach shows nothing when assets have
     * no catalog assignment (nil UUID), which is the common case after a plain
     * "Mark as Asset" without assigning a catalog path. Showing ALL registered catalogs
     * lets the user navigate to any catalog regardless of current asset assignments. */
    catalog_tree_ = library.catalog_service().catalog_tree();
  }

  void build_tree() override;

  void update_enabled_catalogs_from_items(bContext &C);

  static Item &build_catalog_items_recursive(
      ui::TreeViewOrItem &parent,
      const asset_system::AssetCatalogTreeItem &catalog_item,
      ed::view3d::ImageGridUIState &state);

  /** Activatable item that clears the catalog filter (shows all assets). */
  class AllItem : public ui::BasicTreeViewItem {
    ed::view3d::ImageGridUIState &state_;

   public:
    AllItem(ed::view3d::ImageGridUIState &state)
        : ui::BasicTreeViewItem(IFACE_("All")), state_(state)
    {
      this->set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem & /*item*/) {
        ed::view3d::image_grid_filter_set_show_all(state_);
        if (const std::optional<ed::image_grid::ImageGridOwner> owner =
                ed::image_grid::image_grid_owner_from_context(C))
        {
          ed::image_grid::image_grid_reset_scroll(
              *owner, ed::view3d::image_grid_is_mask_slot_from_context(C));
        }
        ed::view3d::image_grid_notify_change(C);
      });
      this->set_is_active_fn([this]() -> bool {
        return state_.filter.catalog_mode == ed::view3d::ImageGridShelfCatalogMode::All &&
               state_.filter.enabled_catalog_paths.is_empty();
      });
    }
  };

  /** Checkbox item for an individual catalog path. */
  class Item : public ui::BasicTreeViewItem {
    const asset_system::AssetCatalogTreeItem &catalog_item_;
    /* Is the catalog path enabled in this redraw? Set on construction, updated by the UI (which
     * gets a pointer to it). The UI needs it as char. */
    char catalog_path_enabled_ = false;

   public:
    Item(const asset_system::AssetCatalogTreeItem &catalog_item,
         ed::view3d::ImageGridUIState &state)
        : ui::BasicTreeViewItem(catalog_item.get_name()),
          catalog_item_(catalog_item),
          catalog_path_enabled_(
              state.filter.enabled_catalog_paths.contains(catalog_item.catalog_path().str()) ? 1 :
                                                                                               0)
    {
      /* Clicking anywhere in the row toggles the catalog, not just the checkbox — precise checkbox
       * hits are hard with a stylus. The checkbox (#build_row) keeps its own click handling; it and
       * the row's full-width #AbstractTreeViewItem::add_treerow_button are separate buttons, and
       * per-pixel hit-testing resolves to whichever is topmost, so a checkbox click is never also
       * counted as a row click. */
      set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem & /*item*/) {
        ImageGridCatalogSelectorTree &tree = dynamic_cast<ImageGridCatalogSelectorTree &>(
            get_tree_view());
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
          [&has_enabled](const ui::AbstractTreeViewItem &abstract_item) {
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

    void build_row(ui::Layout &row) override
    {
      ImageGridCatalogSelectorTree &tree = dynamic_cast<ImageGridCatalogSelectorTree &>(
          get_tree_view());
      ui::Block *block = row.block();

      row.emboss_set(ui::EmbossType::Emboss);

      ui::Layout &subrow = row.row(false);
      subrow.active_set(catalog_path_enabled_);
      subrow.label(catalog_item_.get_name(), ICON_NONE);
      ui::block_layout_set_current(block, &row);

      ui::Button *toggle_but = uiDefButV(block,
                                         ui::ButtonType::Checkbox,
                                         "",
                                         0,
                                         0,
                                         UI_UNIT_X,
                                         UI_UNIT_Y,
                                         &catalog_path_enabled_,
                                         0,
                                         0,
                                         TIP_("Toggle catalog visibility in the image grid"));
      ui::button_func_set(toggle_but,
                          [&tree](bContext &C) { tree.update_enabled_catalogs_from_items(C); });
      if (!is_catalog_path_enabled() && has_enabled_in_subtree()) {
        ui::button_drawflag_enable(toggle_but, ui::BUT_INDETERMINATE);
      }
      ui::button_flag_disable(toggle_but, ui::BUT_UNDO);
    }
  };
};

void ImageGridCatalogSelectorTree::update_enabled_catalogs_from_items(bContext &C)
{
  const bool was_membership =
      state_.filter.catalog_mode == ed::view3d::ImageGridShelfCatalogMode::Recent ||
      state_.filter.catalog_mode == ed::view3d::ImageGridShelfCatalogMode::Favorites;
  state_.filter.enabled_catalog_paths.clear();
  foreach_item([this](ui::AbstractTreeViewItem &view_item) {
    const Item *item = dynamic_cast<const Item *>(&view_item);
    if (item && item->is_catalog_path_enabled()) {
      state_.filter.enabled_catalog_paths.add(item->catalog_path().str());
    }
  });
  state_.filter.catalog_mode = state_.filter.enabled_catalog_paths.is_empty() ?
                                   ed::view3d::ImageGridShelfCatalogMode::All :
                                   ed::view3d::ImageGridShelfCatalogMode::CatalogPath;
  /* Leaving membership with no catalogs selected is "show all" — do not wipe saved ALL-library
   * filters. Selecting real catalog paths commits normally. */
  if (!(was_membership && state_.filter.enabled_catalog_paths.is_empty())) {
    ed::view3d::image_grid_catalog_commit_active(state_);
  }
  /* Catalog changed — old focus position is stale; dismiss it so the grid does not snap. */
  ed::view3d::image_grid_focus_clear(state_.viewport);
  ed::view3d::image_grid_pending_clear(state_);

  if (const std::optional<ed::image_grid::ImageGridOwner> owner =
          ed::image_grid::image_grid_owner_from_context(C))
  {
    const bool is_mask_slot = ed::view3d::image_grid_is_mask_slot_from_context(C);
    ed::image_grid::image_grid_reset_scroll(*owner, is_mask_slot);
    ed::image_grid::image_grid_state_persist(*owner, state_, is_mask_slot);
  }

  ed::view3d::image_grid_notify_change(C);
}

void ImageGridCatalogSelectorTree::build_tree()
{
  AllItem &all_item = add_tree_item<AllItem>(state_);
  all_item.uncollapse_by_default();

  if (!catalog_tree_ || catalog_tree_->is_empty()) {
    return;
  }

  catalog_tree_->foreach_root_item([this](const asset_system::AssetCatalogTreeItem &cat_item) {
    Item &item = build_catalog_items_recursive(*this, cat_item, state_);
    item.uncollapse_by_default();
  });
}

ImageGridCatalogSelectorTree::Item &ImageGridCatalogSelectorTree::build_catalog_items_recursive(
    ui::TreeViewOrItem &parent,
    const asset_system::AssetCatalogTreeItem &catalog_item,
    ed::view3d::ImageGridUIState &state)
{
  Item &item = parent.add_tree_item<Item>(catalog_item, state);

  catalog_item.foreach_child([&](const asset_system::AssetCatalogTreeItem &child) {
    build_catalog_items_recursive(item, child, state);
  });

  return item;
}

static void image_grid_display_panel_draw(const bContext *C, Panel *panel)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return;
  }

  PointerRNA owner_ptr;
  if (View3D *v3d = owner->as_view3d()) {
    owner_ptr = RNA_pointer_create_discrete(nullptr, RNA_SpaceView3D, v3d);
  }
  else if (SpaceImage *sima = owner->as_space_image()) {
    owner_ptr = RNA_pointer_create_discrete(nullptr, RNA_SpaceImageEditor, sima);
  }
  else {
    return;
  }

  ui::Layout &layout = *panel->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  layout.prop(&owner_ptr, "image_grid_preview_size", UI_ITEM_NONE, IFACE_("Size"), ICON_NONE);
}

void image_grid_display_panel_register(ARegionType *region_type)
{
  if (WM_paneltype_find("VIEW3D_PT_image_grid_display", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "VIEW3D_PT_image_grid_display");
  STRNCPY_UTF8(pt->label, N_("Display Settings"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Adjust display settings for the image grid");
  pt->draw = image_grid_display_panel_draw;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

static void image_grid_catalog_selector_draw(const bContext *C, Panel *panel)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return;
  }

  ed::view3d::ImageGridUIState &state = ed::image_grid::image_grid_state_get_from_context(*C);

  ui::Layout &layout = *panel->layout;
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);

  if (ed::image_grid::image_grid_library_is_missing(
          *owner, ed::view3d::image_grid_is_mask_slot_from_context(*C)))
  {
    layout.label(IFACE_("Library not found"), ICON_ERROR);
    return;
  }

  /* Catalog tree. */
  ed::asset::list::storage_fetch(&state.filter.lib_ref, C);

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      state.filter.lib_ref);
  if (!library) {
    layout.label(IFACE_("Loading\xe2\x80\xa6"), ICON_NONE);
    return;
  }

  ed::view3d::image_grid_catalog_sanitize_selection(state);

  ui::Block *block = layout.block();
  ui::AbstractTreeView *tree_view = ui::block_add_view(
      *block,
      "image_grid_catalog_selector",
      std::make_unique<ImageGridCatalogSelectorTree>(*C, state, *library));
  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

void image_grid_catalog_selector_panel_register(ARegionType *region_type)
{
  if (WM_paneltype_find("VIEW3D_PT_image_grid_catalog_selector", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "VIEW3D_PT_image_grid_catalog_selector");
  STRNCPY_UTF8(pt->label, N_("Catalog Selector"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select the asset library and catalog to display in the image grid");
  pt->draw = image_grid_catalog_selector_draw;
  pt->listener = ed::asset::list::asset_reading_region_listen_fn;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Name-match filter (Map Types only)
 * \{ */

static wmOperatorStatus image_grid_name_match_enabled_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return OPERATOR_CANCELLED;
  }

  const bool is_mask_slot = ed::view3d::image_grid_is_mask_slot_from_context(*C);
  ed::view3d::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             is_mask_slot);
  state.filter.name_match.enabled = !state.filter.name_match.enabled;
  ed::image_grid::image_grid_state_persist(*owner, state, is_mask_slot);
  ed::view3d::image_grid_notify_change(*C, is_mask_slot);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_name_match_enabled_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Image Grid Name Match Filter";
  ot->idname = "VIEW3D_OT_image_grid_name_match_enabled_toggle";
  ot->description = "Enable or disable name matching for the image grid";
  ot->exec = image_grid_name_match_enabled_toggle_exec;
  ot->flag = OPTYPE_INTERNAL;
}

static wmOperatorStatus image_grid_name_match_map_type_toggle_exec(bContext *C, wmOperator *op)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return OPERATOR_CANCELLED;
  }

  char identifier[64];
  RNA_string_get(op->ptr, "identifier", identifier);

  const bool is_mask_slot = ed::view3d::image_grid_is_mask_slot_from_context(*C);
  ed::view3d::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             is_mask_slot);
  BKE_name_match_filter_toggle_map_type(state.filter.name_match, identifier);
  ed::image_grid::image_grid_state_persist(*owner, state, is_mask_slot);
  ed::view3d::image_grid_notify_change(*C, is_mask_slot);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_name_match_map_type_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Image Grid Name Match Map Type";
  ot->idname = "VIEW3D_OT_image_grid_name_match_map_type_toggle";
  ot->description = "Toggle a map type in the image grid name matching filter";
  ot->exec = image_grid_name_match_map_type_toggle_exec;
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_string(ot->srna, "identifier", nullptr, 64, "Identifier", "");
}

static wmOperatorStatus image_grid_name_match_clear_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return OPERATOR_CANCELLED;
  }

  const bool is_mask_slot = ed::view3d::image_grid_is_mask_slot_from_context(*C);
  ed::view3d::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             is_mask_slot);
  BKE_name_match_filter_clear_selection(state.filter.name_match);
  ed::image_grid::image_grid_state_persist(*owner, state, is_mask_slot);
  ed::view3d::image_grid_notify_change(*C, is_mask_slot);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_name_match_clear(wmOperatorType *ot)
{
  ot->name = "Clear Image Grid Name Match Filter";
  ot->idname = "VIEW3D_OT_image_grid_name_match_clear";
  ot->description = "Clear active name matching map-type selections on the image grid";
  ot->exec = image_grid_name_match_clear_exec;
  ot->flag = OPTYPE_INTERNAL;
}

static void image_grid_name_match_filter_panel_draw(const bContext *C, Panel *panel)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return;
  }

  const bool is_mask_slot = ed::view3d::image_grid_is_mask_slot_from_context(*C);
  ed::view3d::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             is_mask_slot);
  ui::Layout &layout = *panel->layout;

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

  layout.enabled_set(state.filter.name_match.enabled);
  layout.label(IFACE_("Map Types"), ICON_NONE);
  for (const auto &[identifier, name] : map_type_rows) {
    const bool active = BKE_name_match_filter_map_type_is_active(state.filter.name_match,
                                                                  identifier);
    ui::Layout &row = layout.row(false);
    PointerRNA props = row.op("VIEW3D_OT_image_grid_name_match_map_type_toggle",
                              name,
                              active ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT);
    if (props.type != nullptr) {
      RNA_string_set(&props, "identifier", identifier.c_str());
    }
  }
  layout.separator();
  layout.op("VIEW3D_OT_image_grid_name_match_clear", IFACE_("Clear Filter"), ICON_X);
}

void image_grid_name_match_filter_panel_register(ARegionType *region_type)
{
  if (WM_paneltype_find("VIEW3D_PT_image_grid_name_match_filter", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "VIEW3D_PT_image_grid_name_match_filter");
  STRNCPY_UTF8(pt->label, N_("Name Match Filter"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select map types for name matching in the image grid");
  pt->draw = image_grid_name_match_filter_panel_draw;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

/** \} */

}  // namespace blender
