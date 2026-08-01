/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Asset-library source for the ID browser popover (see #interface_template_id_browser.cc).
 *
 * Kept separate from the ID browser itself because none of this is needed for the default
 * blend-data source. The state lives on the #wmWindowManager (not on a space), so the same
 * library/catalog selection applies wherever the popover is opened from.
 */

#include <optional>

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_set.hh"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_asset_types.h"
#include "DNA_ID.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"

#include "interface_grid_view.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Catalog selection state (wmWindowManager)
 * \{ */

Set<std::string> id_browser_catalog_paths_get(const wmWindowManager &wm)
{
  Set<std::string> paths;
  for (const AssetCatalogPathLink &link : wm.id_browser_enabled_catalog_paths) {
    if (link.path != nullptr) {
      paths.add(link.path);
    }
  }
  return paths;
}

void id_browser_catalog_paths_set(wmWindowManager &wm, const Set<std::string> &paths)
{
  BKE_asset_catalog_path_list_free(wm.id_browser_enabled_catalog_paths);
  for (const std::string &path : paths) {
    BKE_asset_catalog_path_list_add_path(wm.id_browser_enabled_catalog_paths, path.c_str());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Library display
 * \{ */

const AssetLibraryReference &id_browser_library_ref_ensure_valid(wmWindowManager &wm)
{
  ed::asset::library_reference_ensure_resolved(wm.id_browser_asset_library_ref);
  return wm.id_browser_asset_library_ref;
}

bool id_browser_library_is_missing(wmWindowManager &wm)
{
  return ed::asset::library_reference_ensure_resolved(wm.id_browser_asset_library_ref) ==
         ed::asset::LibraryRefStatus::Missing;
}

const char *id_browser_library_ui_name(const AssetLibraryReference &lib_ref)
{
  switch (lib_ref.type) {
    case ASSET_LIBRARY_ALL:
      return IFACE_("All Libraries");
    case ASSET_LIBRARY_LOCAL:
      return IFACE_("Current File");
    case ASSET_LIBRARY_ESSENTIALS:
      return IFACE_("Essentials");
    case ASSET_LIBRARY_ONLINE_ESSENTIALS:
      return IFACE_("Online Essentials");
    case ASSET_LIBRARY_CUSTOM: {
      const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(&U,
                                                                                          &lib_ref);
      if (user_library && user_library->name[0]) {
        return user_library->name;
      }
      return IFACE_("Asset Library");
    }
    default:
      return IFACE_("Asset Library");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset iteration
 * \{ */

void id_browser_foreach_asset(const bContext &C,
                              const AssetLibraryReference &lib_ref,
                              const short idcode,
                              const Set<std::string> &enabled_catalog_paths,
                              FunctionRef<bool(asset_system::AssetRepresentation &)> fn)
{
  /* Asynchronous: the first draw may see an empty list. The block's asset listener (see
   * #id_browser_popover_draw) redraws the popover once the library is read. */
  ed::asset::list::storage_fetch(&lib_ref, &C);

  const bool catalog_filtering_enabled = !enabled_catalog_paths.is_empty();

  /* Build catalog filters once from the enabled paths; reused for every asset. */
  Vector<asset_system::AssetCatalogFilter> catalog_filters;
  if (catalog_filtering_enabled) {
    if (const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
            lib_ref))
    {
      for (const std::string &path : enabled_catalog_paths) {
        const asset_system::AssetCatalog *catalog =
            library->catalog_service().find_catalog_by_path(path.c_str());
        if (catalog != nullptr) {
          catalog_filters.append(
              library->catalog_service().create_catalog_filter(catalog->catalog_id));
        }
      }
    }
    /* The library is still loading: showing an unfiltered list would flash items the user has
     * filtered out. Show nothing until the catalogs are known. */
    if (catalog_filters.is_empty()) {
      return;
    }
  }

  const ID_Type id_type = ID_Type(idcode);
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) -> bool {
    if (asset.get_id_type() != id_type) {
      return true;
    }
    if (catalog_filtering_enabled) {
      const AssetMetaData &metadata = asset.get_metadata();
      bool in_catalog = false;
      for (const asset_system::AssetCatalogFilter &filter : catalog_filters) {
        if (filter.contains(metadata.catalog_id)) {
          in_catalog = true;
          break;
        }
      }
      if (!in_catalog) {
        return true;
      }
    }
    return fn(asset);
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Library Operator
 * \{ */

const EnumPropertyItem *id_browser_library_rna_itemf(const bContext *C, bool *r_free)
{
  /* Restrict the library list to libraries explicitly set up via "Add Image Library" when the
   * popover is actually browsing images (its only current use -- see
   * #interface_template_id_browser.cc's docstring). Image indexing itself is opt-in (see
   * #image_library_needs_reindex()), so an untagged library can never surface an image asset here
   * either. Resolved the same way #build_id_grid() resolves its target idcode. Falls back to the
   * permissive default for any other browsed ID type. */
  bool only_image_libraries = false;
  PointerRNA target_ptr = CTX_data_pointer_get(C, "id_browser_ptr");
  const std::optional<StringRefNull> prop_name = CTX_data_string_get(C, "id_browser_prop");
  if (target_ptr.data && prop_name) {
    if (PropertyRNA *target_prop = RNA_struct_find_property(&target_ptr, prop_name->c_str())) {
      if (RNA_property_type(target_prop) == PROP_POINTER) {
        const StructRNA *ptr_type = RNA_property_pointer_type(&target_ptr, target_prop);
        only_image_libraries = ptr_type && RNA_type_to_ID_code(ptr_type) == ID_IM;
      }
    }
  }

  const EnumPropertyItem *items = ed::asset::library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false,
      /*exclude_image_libraries=*/false,
      only_image_libraries);
  *r_free = (items != nullptr);
  return items;
}

bool id_browser_set_asset_library(wmWindowManager &wm, const int library_enum_value)
{
  const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(
      library_enum_value);
  const AssetLibraryReference &old_ref = wm.id_browser_asset_library_ref;
  if (new_ref.type == old_ref.type && new_ref.custom_library_index == old_ref.custom_library_index)
  {
    return false;
  }

  wm.id_browser_asset_library_ref = new_ref;
  /* Catalog paths are library-specific; a path from the previous library would silently filter
   * everything out. Reset to "all catalogs" (see the spec: one list, cleared on library change). */
  id_browser_catalog_paths_set(wm, {});
  grid_view_session_reset_scroll(id_browser_grid_session_key);
  WM_file_tag_modified();
  return true;
}

static const EnumPropertyItem *rna_id_browser_library_itemf(bContext *C,
                                                            PointerRNA * /*ptr*/,
                                                            PropertyRNA * /*prop*/,
                                                            bool *r_free)
{
  return id_browser_library_rna_itemf(C, r_free);
}

static wmOperatorStatus id_browser_set_library_exec(bContext *C, wmOperator *op)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return OPERATOR_CANCELLED;
  }

  if (!id_browser_set_asset_library(*wm, RNA_enum_get(op->ptr, "asset_library_reference"))) {
    return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_ASSET | ND_ASSET_LIST, nullptr);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
  return OPERATOR_FINISHED;
}

void UI_OT_id_browser_set_library(wmOperatorType *ot)
{
  ot->name = "Set Asset Library";
  ot->description = "Set the asset library browsed by the ID browser";
  ot->idname = "UI_OT_id_browser_set_library";

  ot->exec = id_browser_set_library_exec;

  /* UI state only, no undo push. */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "asset_library_reference", rna_enum_dummy_NULL_items, 0, "Asset Library", "");
  RNA_def_enum_funcs(prop, rna_id_browser_library_itemf);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Catalog Selector Popover
 * \{ */

/**
 * Tree view listing catalogs of the browsed asset library. Individual catalogs can be enabled or
 * disabled via checkboxes. An empty selection means "show all" (see #id_browser_foreach_asset).
 */
class IDBrowserCatalogSelectorTree : public AbstractTreeView {
  wmWindowManager &wm_;
  /* Full catalog tree shared from the library's catalog service. Using a shared_ptr avoids
   * copying the tree (which has raw parent pointers) and ensures the data stays alive for the
   * lifetime of this view. */
  std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree_;
  /* Read once per tree build rather than once per item: #build_tree() visits every catalog item,
   * and re-walking the DNA list (and allocating a new #Set) for each of them is wasted work on
   * every redraw. */
  const Set<std::string> enabled_catalog_paths_;

 public:
  class AllItem;
  class Item;

  IDBrowserCatalogSelectorTree(const asset_system::AssetLibrary &library, wmWindowManager &wm)
      : wm_(wm), enabled_catalog_paths_(id_browser_catalog_paths_get(wm))
  {
    /* Use the full catalog tree of the library rather than a filtered tree built from catalog IDs
     * of loaded assets: the filtered approach shows nothing when assets have no catalog
     * assignment (nil UUID), the common case after a plain "Mark as Asset". Showing all
     * registered catalogs lets the user navigate to any catalog regardless of current asset
     * assignments. */
    catalog_tree_ = library.catalog_service().catalog_tree();
  }

  void build_tree() override;

  /**
   * Enable or disable a single catalog path against the stored set (add/remove), rather than
   * rebuilding the whole set from the tree's items. See the doc comment above the definition for
   * why a rebuild is unsafe.
   */
  void update_enabled_catalog_path(bContext &C, const std::string &catalog_path, bool enabled);

  static Item &build_catalog_items_recursive(
      TreeViewOrItem &parent,
      const asset_system::AssetCatalogTreeItem &catalog_item,
      wmWindowManager &wm,
      const Set<std::string> &enabled_catalog_paths);

  /** Activatable item that clears the catalog filter (shows all assets). */
  class AllItem : public BasicTreeViewItem {
    wmWindowManager &wm_;
    /* Derived once from the tree's enabled-paths snapshot; see #enabled_catalog_paths_. */
    bool is_active_;

   public:
    AllItem(wmWindowManager &wm, const Set<std::string> &enabled_catalog_paths)
        : BasicTreeViewItem(IFACE_("All")), wm_(wm), is_active_(enabled_catalog_paths.is_empty())
    {
      set_on_activate_fn([this](bContext &C, BasicTreeViewItem & /*item*/) {
        id_browser_catalog_paths_set(wm_, {});
        grid_view_session_reset_scroll(id_browser_grid_session_key);
        WM_file_tag_modified();
        WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
        if (ARegion *region = CTX_wm_region(&C)) {
          ED_region_tag_redraw(region);
          ED_region_tag_refresh_ui(region);
        }
      });
      set_is_active_fn([this]() -> bool { return is_active_; });
    }
  };

  /** Checkbox item for an individual catalog path. */
  class Item : public BasicTreeViewItem {
    const asset_system::AssetCatalogTreeItem &catalog_item_;
    /* Is the catalog path enabled in this redraw? Set on construction, updated by the UI (which
     * gets a pointer to it). The UI needs it as char. This #Item outlives the redraw via the
     * tree view owned by the block (see #block_add_view), so the button's stored pointer stays
     * valid for as long as the block (and its buttons) are alive. */
    char catalog_path_enabled_ = 0;
    /* A tree component with no catalog of its own (nil #CatalogID) only exists to hold child
     * catalogs (see #AssetCatalogTree's insertion logic: non-leaf path components get a nil ID
     * unless some catalog is registered at that exact path). There is nothing to filter by, so
     * such an item must not get a togglable checkbox. */
    bool has_catalog_ = false;

   public:
    Item(const asset_system::AssetCatalogTreeItem &catalog_item,
         wmWindowManager & /*wm*/,
         const Set<std::string> &enabled_catalog_paths)
        : BasicTreeViewItem(catalog_item.get_name()),
          catalog_item_(catalog_item),
          catalog_path_enabled_(
              enabled_catalog_paths.contains(catalog_item.catalog_path().str()) ? 1 : 0),
          has_catalog_(!BLI_uuid_is_nil(catalog_item.get_catalog_id()))
    {
      if (!has_catalog_) {
        /* Tree-only grouping node: nothing to filter by, so nothing to toggle (matches #build_row,
         * which omits the checkbox for these). */
        disable_activatable();
        return;
      }
      /* Clicking anywhere in the row toggles the catalog, not just the checkbox — precise checkbox
       * hits are hard with a stylus. The checkbox (#build_row) keeps its own click handling; it and
       * the row's full-width #AbstractTreeViewItem::add_treerow_button are separate buttons, and
       * per-pixel hit-testing resolves to whichever is topmost, so a checkbox click is never also
       * counted as a row click. */
      set_on_activate_fn([this](bContext &C, BasicTreeViewItem & /*item*/) {
        IDBrowserCatalogSelectorTree &tree = dynamic_cast<IDBrowserCatalogSelectorTree &>(
            get_tree_view());
        tree.update_enabled_catalog_path(C, catalog_path().str(), !is_catalog_path_enabled());
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
      row.emboss_set(EmbossType::Emboss);

      Layout &subrow = row.row(false);
      /* This item itself has no checkbox to reflect (see #has_catalog_ below); fall back to
       * whether any of its child catalogs are enabled, so the hierarchy still hints at what is
       * filtered. */
      subrow.active_set(has_catalog_ ? bool(catalog_path_enabled_) : has_enabled_in_subtree());
      subrow.label(catalog_item_.get_name(), ICON_NONE);

      if (!has_catalog_) {
        /* Tree-only node: nothing to filter by, so no checkbox (see #has_catalog_). */
        return;
      }

      IDBrowserCatalogSelectorTree &tree = dynamic_cast<IDBrowserCatalogSelectorTree &>(
          get_tree_view());
      Block *block = row.block();
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
                                     TIP_("Toggle catalog visibility in the ID browser"));
      button_func_set(toggle_but, [&tree, this](bContext &C) {
        /* By the time this runs, #catalog_path_enabled_ already reflects the new checkbox
         * state (the button writes to it directly via #uiDefButV). */
        tree.update_enabled_catalog_path(C, catalog_path().str(), is_catalog_path_enabled());
      });
      if (!is_catalog_path_enabled() && has_enabled_in_subtree()) {
        button_drawflag_enable(toggle_but, BUT_INDETERMINATE);
      }
      button_flag_disable(toggle_but, BUT_UNDO);
    }
  };
};

/*
 * The tree is rebuilt from #AssetCatalogTree, which for #ASSET_LIBRARY_ALL is filled in
 * incrementally as sub-libraries finish loading asynchronously (see
 * #id_browser_catalog_selector_draw). That means the tree can legitimately have no item for a
 * catalog the user already enabled from a sub-library that has not loaded yet. Rebuilding the
 * whole enabled-set from "which tree items are checked" would silently drop those paths the
 * moment any other checkbox is toggled. Applying only the one path that changed, on top of the
 * currently stored set, leaves paths with no tree item untouched.
 */
void IDBrowserCatalogSelectorTree::update_enabled_catalog_path(bContext &C,
                                                                const std::string &catalog_path,
                                                                const bool enabled)
{
  Set<std::string> enabled_paths = id_browser_catalog_paths_get(wm_);
  if (enabled) {
    enabled_paths.add(catalog_path);
  }
  else {
    enabled_paths.remove(catalog_path);
  }
  id_browser_catalog_paths_set(wm_, enabled_paths);
  grid_view_session_reset_scroll(id_browser_grid_session_key);

  WM_file_tag_modified();
  WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
  if (ARegion *region = CTX_wm_region(&C)) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
}

void IDBrowserCatalogSelectorTree::build_tree()
{
  add_tree_item<AllItem>(wm_, enabled_catalog_paths_).uncollapse_by_default();

  if (!catalog_tree_ || catalog_tree_->is_empty()) {
    return;
  }

  catalog_tree_->foreach_root_item([this](const asset_system::AssetCatalogTreeItem &cat_item) {
    Item &item = build_catalog_items_recursive(*this, cat_item, wm_, enabled_catalog_paths_);
    item.uncollapse_by_default();
  });
}

IDBrowserCatalogSelectorTree::Item &IDBrowserCatalogSelectorTree::build_catalog_items_recursive(
    TreeViewOrItem &parent,
    const asset_system::AssetCatalogTreeItem &catalog_item,
    wmWindowManager &wm,
    const Set<std::string> &enabled_catalog_paths)
{
  Item &item = parent.add_tree_item<Item>(catalog_item, wm, enabled_catalog_paths);

  catalog_item.foreach_child([&](const asset_system::AssetCatalogTreeItem &child) {
    build_catalog_items_recursive(item, child, wm, enabled_catalog_paths);
  });

  return item;
}

/**
 * Drop catalog paths that no longer exist in \a library (catalog renamed or removed elsewhere);
 * otherwise a stale path would silently filter everything out with no way to see why.
 */
static void id_browser_catalog_selection_sanitize(wmWindowManager &wm,
                                                   const asset_system::AssetLibrary &library)
{
  const Set<std::string> enabled_paths = id_browser_catalog_paths_get(wm);
  if (enabled_paths.is_empty()) {
    return;
  }

  Set<std::string> valid_paths;
  for (const std::string &path : enabled_paths) {
    if (library.catalog_service().find_catalog_by_path(path.c_str())) {
      valid_paths.add(path);
    }
  }

  if (valid_paths == enabled_paths) {
    return;
  }

  id_browser_catalog_paths_set(wm, valid_paths);
  WM_file_tag_modified();
}

static void id_browser_catalog_selector_draw(const bContext *C, Panel *panel)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return;
  }

  Layout &layout = *panel->layout;
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);

  if (id_browser_library_is_missing(*wm)) {
    layout.label(IFACE_("Library not found"), ICON_ERROR);
    return;
  }
  const AssetLibraryReference &lib_ref = id_browser_library_ref_ensure_valid(*wm);
  ed::asset::list::storage_fetch(&lib_ref, C);

  asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  if (library == nullptr) {
    layout.label(IFACE_("Loading\xe2\x80\xa6"), ICON_NONE);
    return;
  }

  /* Drop paths that no longer exist in the library (catalog renamed or removed elsewhere);
   * otherwise a stale path would filter everything out with no way to see why. Gated on the
   * library being fully loaded: for #ASSET_LIBRARY_ALL, catalogs arrive as sub-libraries finish
   * loading, so a non-null library here does not mean its catalog set is complete yet. Sanitizing
   * against an incomplete set would permanently delete paths belonging to a sub-library that
   * simply has not loaded yet (mirrors #id_browser_foreach_asset, which shows nothing rather than
   * act on incomplete catalog data). */
  if (ed::asset::list::is_loaded(&lib_ref)) {
    id_browser_catalog_selection_sanitize(*wm, *library);
  }

  Block *block = layout.block();
  AbstractTreeView *tree_view = block_add_view(
      *block,
      "id_browser_catalog_selector",
      std::make_unique<IDBrowserCatalogSelectorTree>(*library, *wm));
  TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

void id_browser_catalog_selector_register()
{
  if (WM_paneltype_find("UI_PT_id_browser_catalog_selector", true)) {
    return;
  }
  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "UI_PT_id_browser_catalog_selector");
  STRNCPY_UTF8(pt->label, N_("Catalog Selector"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Narrow the asset list down to the selected catalogs");
  pt->draw = id_browser_catalog_selector_draw;
  /* Redraw while the asset library is being read (previews/catalogs arrive asynchronously). */
  pt->listener = ed::asset::list::asset_reading_region_listen_fn;
  WM_paneltype_add(pt);
}

/** \} */

}  // namespace blender::ui
