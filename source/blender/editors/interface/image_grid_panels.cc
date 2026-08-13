/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

#include "BKE_asset_catalog_memory.hh"
#include "BKE_context.hh"
#include "BKE_name_matching.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "DNA_userdef_types.h"

#include "ED_asset_list.hh"
#include "ED_image_grid.hh"

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

using namespace ed::image_grid;

static bool image_grid_catalog_id_in_span(const Span<bUUID> ids, const bUUID &catalog_id)
{
  for (const bUUID &id : ids) {
    if (BLI_uuid_equal(id, catalog_id)) {
      return true;
    }
  }
  return false;
}

static bool image_grid_library_has_catalog_filter(const AssetLibraryReference &lib_ref)
{
  return BKE_asset_catalog_memory_get_mode(&U, lib_ref, image_grid_catalog_memory_domain) ==
         ASSET_CATALOG_MEMORY_SET;
}

static bool image_grid_catalog_id_enabled(const AssetLibraryReference &lib_ref,
                                          const bUUID catalog_id)
{
  if (!image_grid_library_has_catalog_filter(lib_ref)) {
    return false;
  }
  return image_grid_catalog_id_in_span(
      BKE_asset_catalog_memory_get_set(&U, lib_ref, image_grid_catalog_memory_domain), catalog_id);
}

static void image_grid_catalog_id_set_enabled(const AssetLibraryReference &lib_ref,
                                              const bUUID catalog_id,
                                              const bool enabled)
{
  blender::Vector<bUUID> ids;
  if (image_grid_library_has_catalog_filter(lib_ref)) {
    ids = BKE_asset_catalog_memory_get_set(&U, lib_ref, image_grid_catalog_memory_domain);
  }
  if (enabled) {
    if (!image_grid_catalog_id_in_span(ids, catalog_id)) {
      ids.append(catalog_id);
    }
  }
  else {
    for (int64_t i = ids.size() - 1; i >= 0; i--) {
      if (BLI_uuid_equal(ids[i], catalog_id)) {
        ids.remove_and_reorder(i);
      }
    }
  }
  if (ids.is_empty()) {
    BKE_asset_catalog_memory_set_all(&U, lib_ref, image_grid_catalog_memory_domain);
  }
  else {
    BKE_asset_catalog_memory_set_set(
        &U, lib_ref, image_grid_catalog_memory_domain, ids.as_span());
  }
}

static void image_grid_apply_catalog_item_toggle(bContext &C,
                                                 ed::image_grid::ImageGridUIState &state,
                                                 const AssetLibraryReference &library_ref,
                                                 const std::string &library_key,
                                                 const bUUID catalog_id,
                                                 const std::string &catalog_path)
{
  if (!library_key.empty() || state.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
    image_grid_catalog_id_set_enabled(
        library_ref, catalog_id, !image_grid_catalog_id_enabled(library_ref, catalog_id));
  }
  else {
    const bool was_membership =
        state.filter.catalog_mode == ed::image_grid::ImageGridCatalogMode::Recent ||
        state.filter.catalog_mode == ed::image_grid::ImageGridCatalogMode::Favorites;
    if (!state.filter.enabled_catalog_paths.remove(catalog_path)) {
      state.filter.enabled_catalog_paths.add(catalog_path);
    }
    state.filter.catalog_mode = state.filter.enabled_catalog_paths.is_empty() ?
                                    ed::image_grid::ImageGridCatalogMode::All :
                                    ed::image_grid::ImageGridCatalogMode::CatalogPath;
    if (!(was_membership && state.filter.enabled_catalog_paths.is_empty())) {
      ed::image_grid::image_grid_catalog_commit_active(state);
    }
  }

  ed::image_grid::image_grid_focus_clear(state.viewport);
  ed::image_grid::image_grid_pending_clear(state);

  if (const std::optional<ed::image_grid::ImageGridOwner> owner =
          ed::image_grid::image_grid_owner_from_context(C))
  {
    const bool is_mask_slot = ed::image_grid::image_grid_is_mask_slot_from_context(C);
    ed::image_grid::image_grid_reset_scroll(*owner, is_mask_slot);
    ed::image_grid::image_grid_state_persist(*owner, state, is_mask_slot);
  }

  ed::image_grid::image_grid_notify_change(C);
}

/* -------------------------------------------------------------------- */
/** \name Catalog Selector Popover
 * \{ */

/** Tree view listing catalogs of the current image-grid library. Individual catalogs can be
 * enabled or disabled via checkboxes. An empty selection means "show all". */
class ImageGridCatalogSelectorTree : public ui::AbstractTreeView {
  /** One catalog tree per library, populated only in All-Libraries mode. Empty in
   * single-library mode, where #catalog_tree_ is used instead. */
  struct LibrarySection {
    std::string key;
    std::string name;
    AssetLibraryReference library_ref{};
    std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree;
  };

  const bContext &C_;
  ed::image_grid::ImageGridUIState &state_;
  /* Full catalog tree shared from the library's catalog service. Using a shared_ptr avoids
   * copying the tree (which has raw parent pointers) and ensures the data stays alive. Only
   * populated in single-library mode; see #library_sections_ for All-Libraries mode. */
  std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree_;
  /* One entry per real library behind #ASSET_LIBRARY_ALL, name-sorted. Only populated in
   * All-Libraries mode; see #catalog_tree_ for single-library mode. Storing #name directly
   * (rather than re-deriving it from #key during #build_tree(), or keeping a raw
   * #asset_system::AssetLibrary pointer with no lifetime guarantee past the constructor) keeps
   * the two lookups this class needs -- the tree data and the display name -- both trivially
   * available and both lifetime-safe. */
  blender::Vector<LibrarySection> library_sections_;

 public:
  class AllItem;
  class LibrarySectionItem;
  class Item;

  /** Single-library mode constructor (unchanged behavior). */
  ImageGridCatalogSelectorTree(const bContext &C,
                               ed::image_grid::ImageGridUIState &state,
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

  /** All-Libraries mode constructor: one section per real library behind #ASSET_LIBRARY_ALL. */
  class AllLibrariesTag {
  };
  ImageGridCatalogSelectorTree(const bContext &C,
                               ed::image_grid::ImageGridUIState &state,
                               AllLibrariesTag /*all_libraries*/)
      : C_(C), state_(state)
  {
    for (asset_system::AssetLibrary *library : ed::image_grid::image_grid_all_mode_libraries()) {
      const AssetLibraryReference &lib_ref = *library->library_reference();
      /* #AssetLibrary::name() is empty for built-ins (Current File / Essentials); use the same
       * UI labels as the library selector. */
      library_sections_.append({ed::image_grid::image_grid_library_key(lib_ref),
                                std::string(ed::image_grid::image_grid_library_ui_name(lib_ref)),
                                lib_ref,
                                library->catalog_service().catalog_tree()});
    }
  }

  void build_tree() override;

  static Item &build_catalog_items_recursive(
      ui::TreeViewOrItem &parent,
      const asset_system::AssetCatalogTreeItem &catalog_item,
      ed::image_grid::ImageGridUIState &state,
      const std::string &library_key,
      const AssetLibraryReference &library_ref);

  /** Activatable item that clears the catalog filter (shows all assets). */
  class AllItem : public ui::BasicTreeViewItem {
    ed::image_grid::ImageGridUIState &state_;

   public:
    AllItem(ed::image_grid::ImageGridUIState &state)
        : ui::BasicTreeViewItem(IFACE_("All")), state_(state)
    {
      this->set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem & /*item*/) {
        if (state_.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
          ed::image_grid::image_grid_filter_set_show_all_for_all_libraries(state_);
        }
        else {
          ed::image_grid::image_grid_filter_set_show_all(state_);
        }
        if (const std::optional<ed::image_grid::ImageGridOwner> owner =
                ed::image_grid::image_grid_owner_from_context(C))
        {
          const bool is_mask_slot = ed::image_grid::image_grid_is_mask_slot_from_context(C);
          ed::image_grid::image_grid_reset_scroll(*owner, is_mask_slot);
          ed::image_grid::image_grid_state_persist(*owner, state_, is_mask_slot);
        }
        ed::image_grid::image_grid_notify_change(C);
      });
      this->set_is_active_fn([this]() -> bool {
        if (state_.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
          for (asset_system::AssetLibrary *library :
               ed::image_grid::image_grid_all_mode_libraries())
          {
            if (const std::optional<AssetLibraryReference> lib_ref = library->library_reference())
            {
              if (image_grid_library_has_catalog_filter(*lib_ref)) {
                return false;
              }
            }
          }
          return true;
        }
        return state_.filter.catalog_mode == ed::image_grid::ImageGridCatalogMode::All &&
               state_.filter.enabled_catalog_paths.is_empty();
      });
    }
  };

  /** Collapsible parent for one real library's catalogs in All-Libraries mode. Its row also
   * carries the per-library "All" checkbox, clearing only this library's saved catalog filter
   * (leaving every other library's filter untouched) -- distinct from #AllItem, which clears
   * every library at once.
   *
   * Expand state is session-only via #ImageGridFilter::expanded_library_section_keys (absence
   * = collapsed). #library_key_ is never empty (only constructed from
   * #image_grid_all_mode_libraries() entries).
   *
   * The checkbox is a plain button (#uiDefButV), not tree-item activation: activating a tree
   * item calls #AbstractTreeViewItem::ensure_parents_uncollapsed(), so an always-active
   * per-library "All" (the common "no filter" case) would force its own section open on every
   * redraw and make collapsing impossible. */
  class LibrarySectionItem : public ui::BasicTreeViewItem {
    ed::image_grid::ImageGridUIState &state_;
    std::string library_key_;
    AssetLibraryReference library_ref_{};
    /* "Showing all" state for this library, i.e. whether domain #"image_grid" has no SET mode
     * for #library_ref_. Set on construction; the checkbox always clears the library's filter on
     * click regardless of the resulting value, so this is read but never written back from
     * #build_row's toggle callback -- the next redraw (triggered by that same callback)
     * reconstructs it from UserDef. */
    char showing_all_ = false;

    struct ToggleArg {
      ed::image_grid::ImageGridUIState *state;
      AssetLibraryReference library_ref;
    };

    static void toggle_arg_free(void *argN)
    {
      MEM_delete(static_cast<ToggleArg *>(argN));
    }

    static void *toggle_arg_copy(const void *argN)
    {
      return MEM_new<ToggleArg>(__func__, *static_cast<const ToggleArg *>(argN));
    }

   public:
    LibrarySectionItem(ed::image_grid::ImageGridUIState &state,
                       std::string library_key,
                       AssetLibraryReference library_ref,
                       std::string name)
        : ui::BasicTreeViewItem(std::move(name)),
          state_(state),
          library_key_(std::move(library_key)),
          library_ref_(library_ref),
          showing_all_(image_grid_library_has_catalog_filter(library_ref_) ? 0 : 1)
    {
      BLI_assert(!library_key_.empty());
    }

    std::optional<bool> should_be_collapsed() const override
    {
      return !state_.filter.expanded_library_section_keys.contains(library_key_);
    }

    void on_collapse_change(bContext & /*C*/, const bool is_collapsed) override
    {
      /* Record session intent only. Do not call #image_grid_notify_change (would rebuild the
       * image grid for a UI-only section open/close) or #image_grid_state_persist (not DNA). */
      if (is_collapsed) {
        state_.filter.expanded_library_section_keys.remove(library_key_);
      }
      else {
        state_.filter.expanded_library_section_keys.add(library_key_);
      }
    }

    void build_row(ui::Layout &row) override
    {
      ui::Block *block = row.block();

      ui::Layout &subrow = row.row(false);
      subrow.label(this->label(), ICON_NONE);
      ui::block_layout_set_current(block, &row);

      ui::Button *toggle_but = uiDefButV(block,
                                         ui::ButtonType::Checkbox,
                                         "",
                                         0,
                                         0,
                                         UI_UNIT_X,
                                         UI_UNIT_Y,
                                         &showing_all_,
                                         0,
                                         0,
                                         TIP_("Show all catalogs from this library"));
      ToggleArg *toggle_arg = MEM_new<ToggleArg>(__func__);
      toggle_arg->state = &state_;
      toggle_arg->library_ref = library_ref_;
      ui::button_funcN_set(
          toggle_but,
          [](bContext *C, void *argN, void * /*arg2*/) {
            auto *arg = static_cast<ToggleArg *>(argN);
            BKE_asset_catalog_memory_set_all(&U, arg->library_ref, image_grid_catalog_memory_domain);
            if (const std::optional<ed::image_grid::ImageGridOwner> owner =
                    ed::image_grid::image_grid_owner_from_context(*C))
            {
              const bool is_mask_slot = ed::image_grid::image_grid_is_mask_slot_from_context(*C);
              ed::image_grid::image_grid_reset_scroll(*owner, is_mask_slot);
              ed::image_grid::image_grid_state_persist(*owner, *arg->state, is_mask_slot);
            }
            ed::image_grid::image_grid_notify_change(*C);
          },
          toggle_arg,
          nullptr,
          toggle_arg_free,
          toggle_arg_copy);
      ui::button_flag_disable(toggle_but, ui::BUT_UNDO);
    }
  };

  /** Checkbox item for an individual catalog path. */
  class Item : public ui::BasicTreeViewItem {
    ed::image_grid::ImageGridUIState &state_;
    const asset_system::AssetCatalogTreeItem &catalog_item_;
    /* Empty in single-library mode. In All-Libraries mode, the key of the library this catalog
     * belongs to -- read/written against #BKE_asset_catalog_memory_* (domain #"image_grid")
     * instead of the flat #ImageGridFilter::enabled_catalog_paths. */
    std::string library_key_;
    AssetLibraryReference library_ref_{};
    /* Is the catalog path enabled in this redraw? Set on construction, updated by the UI (which
     * gets a pointer to it). The UI needs it as char. */
    char catalog_path_enabled_ = false;

    struct ToggleArg {
      ed::image_grid::ImageGridUIState *state;
      AssetLibraryReference library_ref;
      bUUID catalog_id;
      std::string library_key;
      std::string catalog_path;
    };

    static void toggle_arg_free(void *argN)
    {
      MEM_delete(static_cast<ToggleArg *>(argN));
    }

    static void *toggle_arg_copy(const void *argN)
    {
      return MEM_new<ToggleArg>(__func__, *static_cast<const ToggleArg *>(argN));
    }

   public:
    Item(const asset_system::AssetCatalogTreeItem &catalog_item,
         ed::image_grid::ImageGridUIState &state,
         std::string library_key,
         AssetLibraryReference library_ref)
        : ui::BasicTreeViewItem(catalog_item.get_name()),
          state_(state),
          catalog_item_(catalog_item),
          library_key_(std::move(library_key)),
          library_ref_(library_ref),
          catalog_path_enabled_(
              Item::initial_enabled(catalog_item, state, library_key_, library_ref_) ? 1 : 0)
    {
      /* The checkbox (#build_row) is the only way to toggle this item. A row-wide activate
       * handler that also toggled on click raced the checkbox's own click: toggling rebuilds the
       * popover tree mid-click, so the checkbox's own release would land on a freshly rebuilt
       * button. Matches upstream's #AssetCatalogSelectorTree::Item. */
      disable_activatable();
    }

    const std::string &library_key() const
    {
      return library_key_;
    }

    const AssetLibraryReference &library_ref() const
    {
      return library_ref_;
    }

   private:
    static bool initial_enabled(const asset_system::AssetCatalogTreeItem &catalog_item,
                                const ed::image_grid::ImageGridUIState &state,
                                const std::string &library_key,
                                const AssetLibraryReference &library_ref)
    {
      if (!library_key.empty()) {
        return image_grid_catalog_id_enabled(library_ref, catalog_item.get_catalog_id());
      }
      return state.filter.enabled_catalog_paths.contains(catalog_item.catalog_path().str());
    }

   public:
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

    bUUID catalog_id() const
    {
      return catalog_item_.get_catalog_id();
    }

    void build_row(ui::Layout &row) override
    {
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
      ToggleArg *toggle_arg = MEM_new<ToggleArg>(__func__);
      toggle_arg->state = &state_;
      toggle_arg->library_ref = library_ref_;
      toggle_arg->catalog_id = catalog_item_.get_catalog_id();
      toggle_arg->library_key = library_key_;
      toggle_arg->catalog_path = catalog_item_.catalog_path().str();
      ui::button_funcN_set(
          toggle_but,
          [](bContext *C, void *argN, void * /*arg2*/) {
            auto *arg = static_cast<ToggleArg *>(argN);
            image_grid_apply_catalog_item_toggle(*C,
                                                 *arg->state,
                                                 arg->library_ref,
                                                 arg->library_key,
                                                 arg->catalog_id,
                                                 arg->catalog_path);
          },
          toggle_arg,
          nullptr,
          toggle_arg_free,
          toggle_arg_copy);
      if (!is_catalog_path_enabled() && has_enabled_in_subtree()) {
        ui::button_drawflag_enable(toggle_but, ui::BUT_INDETERMINATE);
      }
      ui::button_flag_disable(toggle_but, ui::BUT_UNDO);
    }
  };
};

void ImageGridCatalogSelectorTree::build_tree()
{
  AllItem &all_item = add_tree_item<AllItem>(state_);
  all_item.uncollapse_by_default();

  if (!library_sections_.is_empty()) {
    for (const LibrarySection &section : library_sections_) {
      /* No #uncollapse_by_default(): default is collapsed via empty
       * #expanded_library_section_keys and #LibrarySectionItem::should_be_collapsed(). */
      LibrarySectionItem &section_item = add_tree_item<LibrarySectionItem>(
          state_, section.key, section.library_ref, section.name);

      if (!section.catalog_tree || section.catalog_tree->is_empty()) {
        continue;
      }
      section.catalog_tree->foreach_root_item(
          [&](const asset_system::AssetCatalogTreeItem &cat_item) {
            Item &item = build_catalog_items_recursive(
                section_item, cat_item, state_, section.key, section.library_ref);
            item.uncollapse_by_default();
          });
    }
    return;
  }

  if (!catalog_tree_ || catalog_tree_->is_empty()) {
    return;
  }

  catalog_tree_->foreach_root_item([this](const asset_system::AssetCatalogTreeItem &cat_item) {
    Item &item = build_catalog_items_recursive(
        *this, cat_item, state_, /*library_key=*/"", state_.filter.lib_ref);
    item.uncollapse_by_default();
  });
}

ImageGridCatalogSelectorTree::Item &ImageGridCatalogSelectorTree::build_catalog_items_recursive(
    ui::TreeViewOrItem &parent,
    const asset_system::AssetCatalogTreeItem &catalog_item,
    ed::image_grid::ImageGridUIState &state,
    const std::string &library_key,
    const AssetLibraryReference &library_ref)
{
  Item &item = parent.add_tree_item<Item>(catalog_item, state, library_key, library_ref);

  catalog_item.foreach_child([&](const asset_system::AssetCatalogTreeItem &child) {
    build_catalog_items_recursive(item, child, state, library_key, library_ref);
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

  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(
      *owner, ed::image_grid::image_grid_is_mask_slot_from_context(*C));

  ui::Layout &layout = *panel->layout;
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);

  if (ed::image_grid::image_grid_library_is_missing(
          *owner, ed::image_grid::image_grid_is_mask_slot_from_context(*C)))
  {
    layout.label(IFACE_("Library not found"), ICON_ERROR);
    return;
  }

  /* Catalog tree. */
  ed::asset::list::storage_fetch(&state.filter.lib_ref, C);

  ui::Block *block = layout.block();
  std::unique_ptr<ImageGridCatalogSelectorTree> tree;

  if (state.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
    /* No single #storage_fetch/#library_get_once_available gate here: the All-mode tree only
     * shows sections for libraries #image_grid_all_mode_libraries() already reports as loaded,
     * so a still-loading library simply has no section yet (see the spec's Phase 3/"Enumerating
     * the real libraries" note) rather than blocking the whole popover behind a single
     * "Loading…" label. */
    ed::image_grid::image_grid_catalog_sanitize_selection(state);
    tree = std::make_unique<ImageGridCatalogSelectorTree>(
        *C, state, ImageGridCatalogSelectorTree::AllLibrariesTag{});
  }
  else {
    const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
        state.filter.lib_ref);
    if (!library) {
      layout.label(IFACE_("Loading\xe2\x80\xa6"), ICON_NONE);
      return;
    }
    ed::image_grid::image_grid_catalog_sanitize_selection(state);
    tree = std::make_unique<ImageGridCatalogSelectorTree>(*C, state, *library);
  }

  ui::AbstractTreeView *tree_view = ui::block_add_view(
      *block, "image_grid_catalog_selector", std::move(tree));
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

  const bool is_mask_slot = ed::image_grid::image_grid_is_mask_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             is_mask_slot);
  state.filter.name_match.enabled = !state.filter.name_match.enabled;
  ed::image_grid::image_grid_state_persist(*owner, state, is_mask_slot);
  ed::image_grid::image_grid_notify_change(*C, is_mask_slot);
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

  const bool is_mask_slot = ed::image_grid::image_grid_is_mask_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             is_mask_slot);
  BKE_name_match_filter_toggle_map_type(state.filter.name_match, identifier);
  ed::image_grid::image_grid_state_persist(*owner, state, is_mask_slot);
  ed::image_grid::image_grid_notify_change(*C, is_mask_slot);
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

  const bool is_mask_slot = ed::image_grid::image_grid_is_mask_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             is_mask_slot);
  BKE_name_match_filter_clear_selection(state.filter.name_match);
  ed::image_grid::image_grid_state_persist(*owner, state, is_mask_slot);
  ed::image_grid::image_grid_notify_change(*C, is_mask_slot);
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

  const bool is_mask_slot = ed::image_grid::image_grid_is_mask_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
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
  layout.separator();
  PointerRNA preferences_props = layout.op(
      "SCREEN_OT_userpref_show", IFACE_("Open Preferences..."), ICON_PREFERENCES);
  if (preferences_props.type != nullptr) {
    RNA_enum_set(&preferences_props, "section", USER_SECTION_ASSETS);
  }
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
