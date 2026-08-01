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

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_asset_catalog_memory.hh"
#include "BKE_context.hh"
#include "BKE_name_matching.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BLT_translation.hh"

#include "ED_asset_list.hh"
#include "ED_screen.hh"

#include "WM_api.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include <optional>
#include <string>
#include <utility>

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_templates_intern.hh"

#include "interface_intern.hh"

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
/** \name Catalog selector tree (backed by #GridViewSettings / #UserDef catalog memory)
 * \{ */

static bool id_browser_catalog_id_enabled(const AssetLibraryReference &library_ref,
                                          const bUUID catalog_id)
{
  /* #ASSET_CATALOG_MEMORY_SET is an explicit include-list (see its use as a filter in
   * #id_browser_asset_visible, interface_template_id_browser_asset.cc): with no entry (or the
   * default #ASSET_CATALOG_MEMORY_ALL mode) every catalog is shown, so no checkbox should read as
   * checked -- checking one is what opts it into the include-list. */
  if (BKE_asset_catalog_memory_get_mode(
          &U, library_ref, grid_settings::id_browser_catalog_memory_domain) !=
      ASSET_CATALOG_MEMORY_SET)
  {
    return false;
  }
  const Vector<bUUID> enabled = BKE_asset_catalog_memory_get_set(
      &U, library_ref, grid_settings::id_browser_catalog_memory_domain);
  for (const bUUID &id : enabled) {
    if (BLI_uuid_equal(id, catalog_id)) {
      return true;
    }
  }
  return false;
}

static void id_browser_catalog_id_set_enabled(const AssetLibraryReference &library_ref,
                                              const bUUID catalog_id,
                                              const bool enabled)
{
  Vector<bUUID> ids;
  if (BKE_asset_catalog_memory_get_mode(
          &U, library_ref, grid_settings::id_browser_catalog_memory_domain) ==
      ASSET_CATALOG_MEMORY_SET)
  {
    ids = BKE_asset_catalog_memory_get_set(
        &U, library_ref, grid_settings::id_browser_catalog_memory_domain);
  }
  if (enabled) {
    bool found = false;
    for (const bUUID &id : ids) {
      if (BLI_uuid_equal(id, catalog_id)) {
        found = true;
        break;
      }
    }
    if (!found) {
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
    BKE_asset_catalog_memory_set_all(
        &U, library_ref, grid_settings::id_browser_catalog_memory_domain);
  }
  else {
    BKE_asset_catalog_memory_set_set(
        &U, library_ref, grid_settings::id_browser_catalog_memory_domain, ids.as_span());
  }
}

/* #AssetLibrary::name() is empty for built-ins (Current File / Essentials / Online Essentials);
 * fall back to the same UI labels the library selector shows. Mirrors
 * #id_browser_library_ui_name (interface_template_id_browser_asset.cc), duplicated rather than
 * shared because that function lives in a translation unit this file does not otherwise depend
 * on and the mapping is three lines. */
static std::string grid_catalog_library_section_name(const asset_system::AssetLibrary &library)
{
  const std::string &name = library.name();
  if (!name.empty()) {
    return name;
  }
  if (!library.library_reference().has_value()) {
    return IFACE_("Asset Library");
  }
  switch (library.library_reference()->type) {
    case ASSET_LIBRARY_LOCAL:
      return IFACE_("Current File");
    case ASSET_LIBRARY_ESSENTIALS:
      return IFACE_("Essentials");
    case ASSET_LIBRARY_ONLINE_ESSENTIALS:
      return IFACE_("Online Essentials");
    default:
      return IFACE_("Asset Library");
  }
}

class GridCatalogSelectorTree : public AbstractTreeView {
  const bContext &C_;
  PointerRNA settings_;
  /* Single-library mode only; see #library_sections_ for All-Libraries mode. */
  std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree_;

  struct LibrarySection {
    std::string key;
    std::string name;
    AssetLibraryReference library_ref{};
    std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree;
  };
  /* One entry per real library behind #ASSET_LIBRARY_ALL. Empty in single-library mode. */
  Vector<LibrarySection> library_sections_;
  bool all_libraries_mode_ = false;

 public:
  class AllItem;
  class LibrarySectionItem;
  class Item;

  bool all_libraries_catalog_filters_empty() const
  {
    for (const LibrarySection &section : library_sections_) {
      if (BKE_asset_catalog_memory_get_mode(
              &U, section.library_ref, grid_settings::id_browser_catalog_memory_domain) ==
          ASSET_CATALOG_MEMORY_SET)
      {
        return false;
      }
    }
    return true;
  }

  void clear_all_libraries_catalog_filters()
  {
    for (const LibrarySection &section : library_sections_) {
      BKE_asset_catalog_memory_set_all(
          &U, section.library_ref, grid_settings::id_browser_catalog_memory_domain);
    }
    /* Exit Recent/Favorites membership stored under #ASSET_LIBRARY_ALL as well. */
    BKE_asset_catalog_memory_set_all(&U,
                                     asset_system::all_library_reference(),
                                     grid_settings::id_browser_catalog_memory_domain);
  }

  /** Single-library mode constructor (unchanged behavior). */
  GridCatalogSelectorTree(const bContext &C,
                          PointerRNA settings,
                          const asset_system::AssetLibrary &library)
      : C_(C), settings_(settings)
  {
    catalog_tree_ = library.catalog_service().catalog_tree();
  }

  /** All-Libraries mode constructor: one section per real library. */
  struct AllLibrariesTag {
  };
  GridCatalogSelectorTree(const bContext &C, PointerRNA settings, AllLibrariesTag /*all_libraries*/)
      : C_(C), settings_(settings), all_libraries_mode_(true)
  {
    for (asset_system::AssetLibrary *library :
         ed::asset::all_mode_libraries(/*exclude_image_libraries=*/false,
                                       /*only_image_libraries=*/false))
    {
      const std::optional<AssetLibraryReference> lib_ref = library->library_reference();
      if (!lib_ref.has_value()) {
        continue;
      }
      LibrarySection section;
      section.key = BKE_preferences_asset_library_identifier_from_ref(&U, &*lib_ref);
      section.name = grid_catalog_library_section_name(*library);
      section.library_ref = *lib_ref;
      section.catalog_tree = library->catalog_service().catalog_tree();
      library_sections_.append(std::move(section));
    }
  }

  void build_tree() override;

  void apply_catalog_toggle(bContext &C, const Item &toggled_item);

  static Item &build_catalog_items_recursive(
      TreeViewOrItem &parent,
      const asset_system::AssetCatalogTreeItem &catalog_item,
      PointerRNA settings,
      StringRef library_key,
      const AssetLibraryReference &library_ref);

  class AllItem : public BasicTreeViewItem {
    PointerRNA settings_;

   public:
    AllItem(PointerRNA settings) : BasicTreeViewItem(IFACE_("All")), settings_(settings)
    {
      set_on_activate_fn([this](bContext &C, BasicTreeViewItem & /*item*/) {
        GridCatalogSelectorTree &tree = dynamic_cast<GridCatalogSelectorTree &>(get_tree_view());
        if (tree.all_libraries_mode_) {
          tree.clear_all_libraries_catalog_filters();
        }
        else {
          BKE_asset_catalog_memory_set_all(&U,
                                           grid_settings::library_ref_get(settings_),
                                           grid_settings::id_browser_catalog_memory_domain);
        }
        WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
        if (ARegion *region = CTX_wm_region(&C)) {
          ED_region_tag_redraw(region);
          ED_region_tag_refresh_ui(region);
        }
      });
      set_is_active_fn([this]() -> bool {
        GridCatalogSelectorTree &tree = dynamic_cast<GridCatalogSelectorTree &>(get_tree_view());
        if (tree.all_libraries_mode_) {
          return tree.all_libraries_catalog_filters_empty();
        }
        return BKE_asset_catalog_memory_get_mode(&U,
                                                 grid_settings::library_ref_get(settings_),
                                                 grid_settings::id_browser_catalog_memory_domain) !=
               ASSET_CATALOG_MEMORY_SET;
      });
    }
  };

  /** Parent row for one real library's catalogs in All-Libraries mode. Its row also
   * carries the per-library "All" checkbox, clearing only this library's saved filter -- distinct
   * from #AllItem, which clears every library at once. Mirrors
   * #ImageGridCatalogSelectorTree::LibrarySectionItem (view3d_image_grid_panels.cc), which uses the
   * same generic #AbstractTreeViewItem collapse-chevron mechanism as #Item below and is proven to
   * work in a nested popover.
   *
   * An earlier version of this item disabled the generic chevron (#supports_collapsing() ->
   * `false`) and only built its children while expanded, driving expansion through its own
   * explicit button instead. That combination is what crashed at the time: a *collapsed* section
   * had no children built, so #AbstractTreeViewItem::is_collapsible() (which treats a childless
   * item as non-collapsible) hid the *sibling* #Item catalog rows' generic chevron inconsistently
   * between redraws, and #Item still used the generic mechanism. That was switched to the generic
   * chevron for both classes as a fix (children always built, one consistent mechanism for the
   * whole tree) -- but CLOG instrumentation across three separate reproductions in the same nested
   * All-Libraries popover showed the crash persisting there too, always at the same site
   * (#AbstractTreeViewItem::collapse_chevron_click_fn / #toggle_collapsed_from_view,
   * tree_view.cc), and #on_collapse_change never once logged as reached -- the crash happens
   * before the toggle is applied. #collapse_chevron_click_fn resolves the clicked item by mouse
   * coordinate against whatever #uiBlock the region currently holds
   * (#region_views_find_item_at); this popover's panel rebuilds on every region redraw (routine
   * mouse-move redraws included, not just asset-list notifiers), so a press-to-release click on
   * this specific chevron has a real chance of resolving against a #uiBlock that has already been
   * replaced by the time it is applied.
   *
   * The one interactive control in this tree that has never crashed in any of these
   * reproductions, across dozens of clicks, is the "show all" checkbox below: a plain button whose
   * callback argument is heap-owned via #button_funcN_set, independent of both this item's
   * lifetime and the generic view-item chevron machinery. Expansion now uses the same pattern: an
   * explicit chevron-style icon button instead of #AbstractTreeViewItem's built-in one. This means
   * #supports_collapsing() must stay `false` (never let the generic mechanism draw a second,
   * competing chevron) and children are built only while expanded (see #build_tree()) -- there is
   * no generic #is_collapsed() gate to hide them otherwise. That children-built-conditionally
   * shape is the same one flagged elsewhere in project history as capable of confusing
   * #AbstractTreeView::update_children_from_old_recursive's state migration between redraws; it is
   * accepted here as the smaller, already-known risk against a reproducible crash. */
  class LibrarySectionItem : public BasicTreeViewItem {
    PointerRNA settings_;
    std::string library_key_;
    AssetLibraryReference library_ref_{};
    char showing_all_ = false;
    bool expanded_ = false;

    /* Owned copy of what the callback needs, independent of this item's lifetime. A button that is
     * mid-click when the popover redraws is kept alive across the rebuild by
     * #but_update_old_active_from_new (interface.cc), which refreshes its bound data pointer
     * (#poin) but leaves any *captured* state -- like a `[this]` lambda closure stored in
     * #Button.apply_func -- pointing at whatever `this` was captured, which may already be freed
     * by the time the click is finally applied. Heap-owning the callback's data via
     * #button_funcN_set sidesteps that: the argument's lifetime is tied to the button, not to this
     * item.
     *
     * Deliberately POD (fixed buffer, not `std::string`; raw `PointerRNA` fields, not a
     * #PointerRNA member): #button_funcN_set's defaults free the argument with #MEM_delete_void
     * and duplicate it with #MEM_dupalloc_void, neither of which runs a C++ destructor or
     * copy-constructor -- only trivial, memcpy-safe data is safe with them (a non-trivial
     * #MEM_new allocation here previously crashed in #but_free on popover refresh; see project
     * notes on this exact pitfall). #PointerRNA itself is *not* trivial -- it owns an `ancestors`
     * #blender::Vector -- so its three raw fields are copied out individually instead; none of
     * the accessors this callback needs (#RNA_string_get/set) touch the ancestors chain.
     * `library_key` is always a short, ASCII library identifier (see
     * #BKE_preferences_asset_library_identifier_from_ref), well within #MAX_NAME. */
    struct ToggleArg {
      ID *settings_owner_id;
      StructRNA *settings_type;
      void *settings_data;
      char library_key[MAX_NAME];
      AssetLibraryReference library_ref;
    };
    using ExpandArg = ToggleArg;

   public:
    LibrarySectionItem(PointerRNA settings,
                       std::string library_key,
                       const AssetLibraryReference &library_ref,
                       std::string name)
        : BasicTreeViewItem(std::move(name)), settings_(settings), library_key_(std::move(library_key)),
          library_ref_(library_ref),
          showing_all_((BKE_asset_catalog_memory_get_mode(
                            &U,
                            library_ref_,
                            grid_settings::id_browser_catalog_memory_domain) !=
                        ASSET_CATALOG_MEMORY_SET) ?
                           1 :
                           0),
          expanded_(grid_settings::is_library_section_expanded(settings_, library_key_))
    {
      BLI_assert(!library_key_.empty());
    }

    const std::string &library_key() const
    {
      return library_key_;
    }

    bool is_expanded() const
    {
      return expanded_;
    }

    bool supports_collapsing() const override
    {
      /* Expansion is driven entirely by the explicit chevron below, never the generic mechanism.
       * See the class comment for why. */
      return false;
    }

    void build_row(Layout &row) override
    {
      Block *block = row.block();

      Layout &subrow = row.row(false);
      Button *expand_but = uiDefIconBut(block,
                                        ButtonType::ButToggle,
                                        expanded_ ? ICON_DOWNARROW_HLT : ICON_RIGHTARROW,
                                        0,
                                        0,
                                        short(UI_UNIT_X),
                                        short(UI_UNIT_Y),
                                        nullptr,
                                        0,
                                        0,
                                        TIP_("Expand or collapse library catalogs"));
      /* #ExpandArg (= #ToggleArg) now carries an #AssetLibraryReference member, whose default
       * member initializers (`= -1`, etc.) make it non-trivially-constructible -- MSVC's stricter
       * #MEM_new_zeroed check rejects that even though the struct stays trivially copyable and
       * destructible (still safe with #button_funcN_set's default free/copy, see the class
       * comment above). #MEM_new value-initializes it correctly instead. */
      ExpandArg *expand_arg = MEM_new<ExpandArg>(__func__);
      expand_arg->settings_owner_id = settings_.owner_id;
      expand_arg->settings_type = settings_.type;
      expand_arg->settings_data = settings_.data;
      BLI_strncpy(expand_arg->library_key, library_key_.c_str(), sizeof(expand_arg->library_key));
      button_funcN_set(
          expand_but,
          [](bContext *C, void *argN, void * /*arg2*/) {
            auto *arg = static_cast<ExpandArg *>(argN);
            PointerRNA settings{arg->settings_owner_id, arg->settings_type, arg->settings_data};
            const bool expanded = grid_settings::is_library_section_expanded(settings,
                                                                                arg->library_key);
            grid_settings::library_section_set_expanded(settings, arg->library_key, !expanded);
            if (ARegion *region = CTX_wm_region(C)) {
              ED_region_tag_redraw(region);
              ED_region_tag_refresh_ui(region);
            }
          },
          expand_arg,
          nullptr);
      button_flag_disable(expand_but, BUT_UNDO);
      subrow.label(this->label(), ICON_NONE);
      block_layout_set_current(block, &row);

      Button *toggle_but = uiDefButV(block,
                                     ButtonType::Checkbox,
                                     "",
                                     0,
                                     0,
                                     short(UI_UNIT_X),
                                     short(UI_UNIT_Y),
                                     &showing_all_,
                                     0,
                                     0,
                                     TIP_("Show all catalogs from this library"));
      /* See the #MEM_new comment on #expand_but's #ExpandArg above -- same struct, same reason. */
      ToggleArg *toggle_arg = MEM_new<ToggleArg>(__func__);
      toggle_arg->settings_owner_id = settings_.owner_id;
      toggle_arg->settings_type = settings_.type;
      toggle_arg->settings_data = settings_.data;
      BLI_strncpy(toggle_arg->library_key, library_key_.c_str(), sizeof(toggle_arg->library_key));
      toggle_arg->library_ref = library_ref_;
      button_funcN_set(
          toggle_but,
          [](bContext *C, void *argN, void * /*arg2*/) {
            auto *arg = static_cast<ToggleArg *>(argN);
            BKE_asset_catalog_memory_set_all(&U,
                                             arg->library_ref,
                                             grid_settings::id_browser_catalog_memory_domain);
            WM_event_add_notifier(C, NC_ASSET | ND_ASSET_LIST, nullptr);
            if (ARegion *region = CTX_wm_region(C)) {
              ED_region_tag_redraw(region);
              ED_region_tag_refresh_ui(region);
            }
          },
          toggle_arg,
          nullptr);
      button_flag_disable(toggle_but, BUT_UNDO);
    }
  };

  class Item : public BasicTreeViewItem {
    const asset_system::AssetCatalogTreeItem &catalog_item_;
    PointerRNA settings_;
    /* Empty in single-library mode. In All-Libraries mode, the key of the library this catalog
     * belongs to. */
    std::string library_key_;
    AssetLibraryReference library_ref_{};
    char catalog_path_enabled_ = false;
    bool expanded_ = false;

    /* Same heap-owned-argument pattern and rationale as #LibrarySectionItem::ToggleArg -- see that
     * class's comment. Not POD here: a catalog path (unlike a library key) has no length bound
     * short enough for a fixed buffer, so this carries `std::string` members and pairs
     * #button_funcN_set with custom free/copy functions (#expand_arg_free / #expand_arg_copy)
     * instead of its POD-only defaults (#MEM_delete_void / #MEM_dupalloc_void), which do not run
     * constructors/destructors and would leak or corrupt a non-trivial member. */
    struct ExpandArg {
      ID *settings_owner_id;
      StructRNA *settings_type;
      void *settings_data;
      std::string library_key;
      std::string catalog_path;
    };

    static void expand_arg_free(void *argN)
    {
      MEM_delete(static_cast<ExpandArg *>(argN));
    }

    static void *expand_arg_copy(const void *argN)
    {
      return MEM_new<ExpandArg>(__func__, *static_cast<const ExpandArg *>(argN));
    }

   public:
    Item(const asset_system::AssetCatalogTreeItem &catalog_item,
         PointerRNA settings,
         std::string library_key,
         const AssetLibraryReference &library_ref)
        : BasicTreeViewItem(catalog_item.get_name()),
          catalog_item_(catalog_item),
          settings_(settings),
          library_key_(std::move(library_key)),
          library_ref_(library_ref),
          catalog_path_enabled_(id_browser_catalog_id_enabled(library_ref_,
                                                               catalog_item.get_catalog_id()) ?
                                    1 :
                                    0),
          expanded_(catalog_item.has_children() &&
                    grid_settings::is_catalog_item_expanded(settings, expansion_key(
                        library_key_, catalog_item.catalog_path().str())))
    {
      /* The checkbox is the only way to toggle this item. A row-wide activation handler can race
       * the checkbox click: updating the filter requests a redraw while the current event is still
       * being applied, so the release may target a freshly rebuilt button. This matches the working
       * Image Grid catalog selector and the upstream asset catalog selector. */
      disable_activatable();
    }

    bool is_catalog_path_enabled() const
    {
      return catalog_path_enabled_ != 0;
    }

    const std::string &library_key() const
    {
      return library_key_;
    }

    const AssetLibraryReference &library_ref() const
    {
      return library_ref_;
    }

    bool is_expanded() const
    {
      return expanded_;
    }

    std::string expansion_key() const
    {
      return expansion_key(library_key_, catalog_item_.catalog_path().str());
    }

    static std::string expansion_key(StringRef library_key, StringRef catalog_path)
    {
      std::string key(library_key);
      key += '\x1f';
      key += catalog_path;
      return key;
    }

    /* See the class comment on #LibrarySectionItem for why this tree never uses the generic
     * chevron: this class draws its own instead (see #build_row()). */
    bool supports_collapsing() const override
    {
      return false;
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

    bUUID catalog_id() const
    {
      return catalog_item_.get_catalog_id();
    }

    void build_row(Layout &row) override
    {
      GridCatalogSelectorTree &tree = dynamic_cast<GridCatalogSelectorTree &>(get_tree_view());
      Block *block = row.block();

      /* Chevron first, under the ambient (flat, boxless) tree emboss -- matches the generic
       * #AbstractTreeViewItem::add_collapse_chevron(), which is always drawn before
       * #build_row() runs and therefore before the #EmbossType::Emboss switch below. Creating the
       * chevron after that switch was what gave it a filled-box look unlike every other chevron in
       * the tree (library sections, and every other tree view in Blender). */
      if (catalog_item_.has_children()) {
        Button *expand_but = uiDefIconBut(block,
                                          ButtonType::ButToggle,
                                          expanded_ ? ICON_DOWNARROW_HLT : ICON_RIGHTARROW,
                                          0,
                                          0,
                                          short(UI_UNIT_X),
                                          short(UI_UNIT_Y),
                                          nullptr,
                                          0,
                                          0,
                                          TIP_("Expand or collapse nested catalogs"));
        ExpandArg *expand_arg = MEM_new<ExpandArg>(__func__);
        expand_arg->settings_owner_id = settings_.owner_id;
        expand_arg->settings_type = settings_.type;
        expand_arg->settings_data = settings_.data;
        expand_arg->library_key = library_key_;
        expand_arg->catalog_path = catalog_item_.catalog_path().str();
        button_funcN_set(
            expand_but,
            [](bContext *C, void *argN, void * /*arg2*/) {
              auto *arg = static_cast<ExpandArg *>(argN);
              PointerRNA settings{arg->settings_owner_id, arg->settings_type, arg->settings_data};
              const std::string key = expansion_key(arg->library_key, arg->catalog_path);
              const bool expanded = grid_settings::is_catalog_item_expanded(settings, key);
              grid_settings::catalog_item_set_expanded(settings, key, !expanded);
              if (ARegion *region = CTX_wm_region(C)) {
                ED_region_tag_redraw(region);
                ED_region_tag_refresh_ui(region);
              }
            },
            expand_arg,
            nullptr,
            expand_arg_free,
            expand_arg_copy);
        button_flag_disable(expand_but, BUT_UNDO);
      }

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
                      [this, &tree](bContext &C) { tree.apply_catalog_toggle(C, *this); });
      if (!is_catalog_path_enabled() && has_enabled_in_subtree()) {
        button_drawflag_enable(toggle_but, BUT_INDETERMINATE);
      }
      button_flag_disable(toggle_but, BUT_UNDO);
    }
  };
};

void GridCatalogSelectorTree::apply_catalog_toggle(bContext &C, const Item &toggled_item)
{
  /* Apply only the one item that changed on top of the currently-stored setting, instead of
   * rebuilding it from every #Item currently present in the tree: #build_tree() only builds a
   * section's or item's children while it is expanded (see the class comment on
   * #LibrarySectionItem), so a rebuild-from-visible-items approach would silently drop the saved
   * state of any catalog under a collapsed section or item. */
  const AssetLibraryReference library_ref = all_libraries_mode_ ?
                                                toggled_item.library_ref() :
                                                grid_settings::library_ref_get(settings_);
  id_browser_catalog_id_set_enabled(
      library_ref, toggled_item.catalog_id(), toggled_item.is_catalog_path_enabled());
  WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);

  if (ARegion *region = CTX_wm_region(&C)) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
}

void GridCatalogSelectorTree::build_tree()
{
  add_tree_item<AllItem>(settings_).uncollapse_by_default();

  if (all_libraries_mode_) {
    for (const LibrarySection &section : library_sections_) {
      LibrarySectionItem &section_item = add_tree_item<LibrarySectionItem>(
          settings_, section.key, section.library_ref, section.name);
      /* Children are built only while expanded: this tree draws its own explicit chevrons (see
       * the class comment on #LibrarySectionItem) instead of using the generic collapse
       * mechanism, so there is no framework-level gate to hide built children of a collapsed
       * section -- omitting them from the tree is the only way. */
      if (!section_item.is_expanded() || !section.catalog_tree || section.catalog_tree->is_empty())
      {
        continue;
      }
      section.catalog_tree->foreach_root_item([&](const asset_system::AssetCatalogTreeItem &cat_item) {
        build_catalog_items_recursive(
            section_item, cat_item, settings_, section.key, section.library_ref);
      });
    }
    return;
  }

  if (!catalog_tree_ || catalog_tree_->is_empty()) {
    return;
  }

  const AssetLibraryReference library_ref = grid_settings::library_ref_get(settings_);
  catalog_tree_->foreach_root_item([this, &library_ref](const asset_system::AssetCatalogTreeItem &cat_item) {
    build_catalog_items_recursive(*this, cat_item, settings_, "", library_ref);
  });
}

GridCatalogSelectorTree::Item &GridCatalogSelectorTree::build_catalog_items_recursive(
    TreeViewOrItem &parent,
    const asset_system::AssetCatalogTreeItem &catalog_item,
    PointerRNA settings,
    StringRef library_key,
    const AssetLibraryReference &library_ref)
{
  Item &item = parent.add_tree_item<Item>(
      catalog_item, settings, std::string(library_key), library_ref);

  /* Grandchildren are built only while this item is expanded -- see the class comment on
   * #LibrarySectionItem for why (explicit chevron, no framework-level hide-when-collapsed gate).
   * The item itself is always added regardless of its own expanded state; only its descendants
   * are conditional. */
  if (item.is_expanded()) {
    catalog_item.foreach_child([&](const asset_system::AssetCatalogTreeItem &child) {
      build_catalog_items_recursive(item, child, settings, library_key, library_ref);
    });
  }

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

  const std::optional<int64_t> show_library_row = CTX_data_int_get(
      C, "grid_catalog_selector_show_library_row");
  if (!show_library_row || *show_library_row) {
    Layout &row = layout.row(true);
    template_asset_library_column_selector(
        row, C, &settings_ptr, "asset_library_reference", ICON_NONE);
    if (lib_ref.type != ASSET_LIBRARY_LOCAL) {
      row.op("ASSET_OT_library_refresh", "", ICON_FILE_REFRESH);
    }
  }

  Block *block = layout.block();

  if (lib_ref.type == ASSET_LIBRARY_ALL) {
    /* Warm every real library behind "All" -- #storage_fetch above only requested the merged
     * pseudo-library itself. Without this, a library nobody separately selected yet stays
     * unloaded for the lifetime of this popover: #GridCatalogSelectorTree's All-Libraries
     * constructor (#ed::asset::all_mode_libraries) only reports already-loaded libraries, so its
     * section would silently never appear, and worse, would keep re-triggering a background load
     * (and the redraw that follows it) on every redraw for as long as the popover stays open --
     * including while a click on this same popover is being handled. */
    ed::asset::fetch_all_mode_libraries(
        *C, /*exclude_image_libraries=*/false, /*only_image_libraries=*/false);

    /* Distinct view idname from the single-library tree below: this mode adds a
     * #GridCatalogSelectorTree::LibrarySectionItem level the single-library tree never has, so a
     * cross-frame identity match (#AbstractTreeViewItem::matches_single -- label text only, no
     * type check) between the two shapes must never be attempted. Sharing one idname risks
     * #AbstractTreeView::update_children_from_old_recursive migrating open/active state from a
     * catalog #Item in one tree onto a same-labeled #LibrarySectionItem in the other. */
    AbstractTreeView *tree_view = block_add_view(
        *block,
        "grid_catalog_selector_all_libraries",
        std::make_unique<GridCatalogSelectorTree>(
            *C, settings_ptr, GridCatalogSelectorTree::AllLibrariesTag{}));
    TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
    return;
  }

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  if (!library) {
    layout.label(IFACE_("Loading\xe2\x80\xa6"), ICON_NONE);
    return;
  }

  AbstractTreeView *tree_view = block_add_view(
      *block,
      "grid_catalog_selector",
      std::make_unique<GridCatalogSelectorTree>(*C, settings_ptr, *library));
  TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

/* #ed::asset::list::asset_reading_region_listen_fn (the listener every other consumer of the
 * asset-list system uses) also redraws on #ND_ASSET_LIST_READING and #ND_ASSET_LIST_PREVIEW --
 * notifiers that fire continuously while a library is still being scanned (assets discovered,
 * previews streaming in one by one). None of that changes which catalogs exist, so it does not
 * need to rebuild this tree. In All-Libraries mode this panel can have up to one such stream
 * running per real library at once (see #library_sections_), which measured in practice as tens
 * of rebuilds per second for as long as any library kept loading -- confirmed by CLOG
 * instrumentation: #build_tree ran repeatedly, several times a second, with the popover open and
 * idle. That is a wide window for a chevron click's coordinate-based hit test
 * (#region_views_find_item_at, tree_view.cc) to resolve against a tree the popover has already
 * rebuilt out from under it by the time the click is applied -- the confirmed crash site
 * (#AbstractTreeViewItem::collapse_chevron_click_fn). Only #ND_ASSET_CATALOGS -- the catalog set
 * itself changing -- actually needs a rebuild here. */
static void grid_catalog_selector_region_listen(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  if (wmn->category == NC_ASSET && wmn->data == ND_ASSET_CATALOGS) {
    ED_region_tag_redraw(params->region);
    ED_region_tag_refresh_ui(params->region);
  }
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
  pt->listener = grid_catalog_selector_region_listen;
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
  /* Auto-activate the name match filter when the user picks a map type while it is off.
   * Selecting a type implies intent to filter, so enabling it automatically is more convenient
   * than forcing a separate click on the toggle button. */
  if (!RNA_boolean_get(&settings, "filter_name_match_enabled")) {
    RNA_boolean_set(&settings, "filter_name_match_enabled", true);
  }
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
  /* The popover is a separate interactive surface. Its panel layout must not inherit a disabled
   * state from the button/layout that opened it, otherwise Layout::resolve() disables every item
   * recursively and child enabled_set(true) calls cannot undo that. */
  layout.enabled_set(true);
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
  /* Map Types are always interactive: clicking one auto-enables the filter (see
   * #name_match_map_type_toggle_exec). The label is dimmed to indicate the filter is off,
   * but the buttons remain clickable. */
  {
    Layout &label_row = layout.row(false);
    label_row.enabled_set(state.enabled);
    label_row.label(IFACE_("Map Types"), ICON_NONE);
  }

  /* Create map type buttons in a separate layout that is never disabled. */
  Layout &types_layout = layout.column(false);
  types_layout.enabled_set(true);
  types_layout.active_set(true);

  for (const auto &[identifier, name] : map_type_rows) {
    const bool active = BKE_name_match_filter_map_type_is_active(state, identifier);
    Layout &row = types_layout.row(false);
    PointerRNA props = row.op("GRIDVIEW_OT_name_match_map_type_toggle",
                              name,
                              active ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT);
    if (props.type != nullptr) {
      RNA_string_set(&props, "identifier", identifier.c_str());
    }
  }
  layout.separator();
  /* Always interactive: clearing the map-type selection should not require the filter to be
   * enabled first, matching the "always clickable" map type buttons above. */
  layout.op("GRIDVIEW_OT_name_match_clear", IFACE_("Clear Filter"), ICON_X);
  layout.separator();
  PointerRNA preferences_props = layout.op(
      "SCREEN_OT_userpref_show", IFACE_("Open Preferences..."), ICON_PREFERENCES);
  if (preferences_props.type != nullptr) {
    RNA_enum_set(&preferences_props, "section", USER_SECTION_ASSETS);
  }
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
static void library_selector_seed_retvalue(PopupBlockHandle *handle,
                                           const EnumPropertyItem *items,
                                           const int current_value)
{
  if (!handle) {
    return;
  }

  /* The current value has to be validated against the enum before it is seeded: a value that is
   * not one of the items (a never-initialized or otherwise stale #AssetLibraryReference.type of 0,
   * say) would be applied to the property on close just like a real choice, turning a display
   * glitch into stored garbage. Seed the first selectable item instead in that case. */
  const EnumPropertyItem *fallback = nullptr;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (item->identifier[0] == '\0') {
      /* Separator or heading, not a value that can be applied. */
      continue;
    }
    if (item->value == current_value) {
      handle->retvalue = current_value;
      return;
    }
    if (!fallback) {
      fallback = item;
    }
  }

  if (fallback) {
    handle->retvalue = fallback->value;
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

/* Menu heading label without the blank icon #Layout::label inserts in menu layouts
 * (`ICON_BLANK1`), which would indent the text past icon-less #ButtonType::ButMenu rows.
 * Matches #def_but_rna__menu's heading path. */
static void library_selector_menu_heading(Block *block, Layout &column, const StringRef name)
{
  if (name.is_empty()) {
    return;
  }
  block_layout_set_current(block, &column);
  uiDefBut(block,
           ButtonType::Label,
           name,
           0,
           0,
           short(UI_UNIT_X * 5),
           short(UI_UNIT_Y),
           nullptr,
           0.0,
           0.0,
           "");
}

/* True when the enum entry is a custom library nested under a Preferences folder. Root-level
 * libraries (and all built-ins) return false -- they belong in the selector's first column even
 * when they appear after a folder in Preferences list order. */
static bool library_enum_item_is_in_folder(const EnumPropertyItem &item)
{
  if (item.value < ASSET_LIBRARY_CUSTOM) {
    return false;
  }
  const AssetLibraryReference library_ref = ed::asset::library_reference_from_enum_value(
      item.value);
  const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(&U,
                                                                                      &library_ref);
  return user_library && user_library->parent != nullptr;
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

  /* The enum item callback may depend on the button's context store in the same way as the
   * columnar selector below. Present it directly while building the item list. */
  const bContextStore *previous_store = CTX_store_get(C);
  if (but->context) {
    CTX_store_set(C, but->context);
  }
  const EnumPropertyItem *items = nullptr;
  bool free = false;
  RNA_property_enum_items_gettexted(C, &ptr, prop, &items, nullptr, &free);
  /* Set only by the ID Browser header (`interface_template_id_browser.cc`) -- no other
   * #template_grid_library_selector caller draws Recent/Favorites, so this stays off by default. */
  bool show_recent_favorites = false;
  if (but->context) {
    if (const std::optional<int64_t> flag = CTX_data_int_get(
            C, "grid_library_selector_show_recent_favorites"))
    {
      show_recent_favorites = *flag != 0;
    }
    CTX_store_set(C, previous_store);
  }
  if (!items) {
    return;
  }

  block_flag_enable(block, BLOCK_MOVEMOUSE_QUIT);
  block_layout_set_current(block, layout);

  const int current_value = RNA_property_enum_get(&ptr, prop);
  library_selector_seed_retvalue(handle, items, current_value);
  const bool has_item_with_icon = library_enum_has_item_with_icon(items);

  Layout &col = layout->column(false);
  /* Property UI name as the menu heading (e.g. "Asset Library"), matching the built-in enum
   * dropdown (#def_but_rna__menu) when it shows a title above the choices. */
  const char *title = RNA_property_ui_name(
      prop, RNA_pointer_is_null(&ptr) ? nullptr : &ptr);
  if (title && title[0]) {
    library_selector_menu_heading(block, col, title);
    col.separator();
  }

  /* Recent / Favorites first, same order as the Image Grid's own library menu
   * (#image_grid_library_selector_menu_draw). Not real libraries; they switch the ID Browser's
   * catalog-memory mode via #id_browser_set_membership (#BKE_asset_catalog_memory_set_mode under
   * #ASSET_LIBRARY_ALL, domain #"id_browser"), called directly from a #button_func_set callback
   * rather than through an operator.
   *
   * These must still write #ASSET_LIBRARY_ALL into `&handle->retvalue` like a normal library item
   * (#library_selector_menu_item) does: this popup was opened from a real RNA enum property button
   * (`id_browser_asset_library_reference`), and closing *any* popup spawned that way re-applies
   * `handle->retvalue` to that property (#rna_WindowManager_id_browser_asset_library_set ->
   * #id_browser_set_asset_library) regardless of which item was clicked. Leaving retvalue at its
   * pre-seeded `current_value` would make that re-apply see a library change (old != #ALL). Matching
   * #ASSET_LIBRARY_ALL here (already set by #id_browser_set_membership) makes that re-apply
   * compare equal and early-return instead. */
  if (show_recent_favorites) {
    auto add_membership_item = [&](const StringRefNull label,
                                   const int icon,
                                   const grid_settings::CatalogMode mode) {
      Layout &item_row = col.row(true);
      block_layout_set_current(block, &item_row);
      Button *item_but = uiDefIconTextBut(block,
                                          ButtonType::ButMenu,
                                          icon,
                                          label,
                                          0,
                                          0,
                                          short(UI_UNIT_X * 5),
                                          short(UI_UNIT_Y),
                                          &handle->retvalue,
                                          std::nullopt);
      button_enum_prop_value_set(item_but, ASSET_LIBRARY_ALL);
      button_func_set(item_but, [mode](bContext &C) {
        wmWindowManager *wm = CTX_wm_manager(&C);
        if (wm == nullptr) {
          return;
        }
        id_browser_set_membership(*wm, mode);
        /* Explicit, not relied-upon-implicit: #id_browser_asset_library_reference's own
         * #RNA_def_property_update fires from the popup-close re-apply above, but that only
         * covers the enum property itself -- it does not know this click also rewrote the
         * catalog-memory mode, which is what the grid actually reads. */
        WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
        if (ARegion *region = CTX_wm_region(&C)) {
          ED_region_tag_redraw(region);
          ED_region_tag_refresh_ui(region);
        }
      });
    };

    add_membership_item(IFACE_("Recent"), ICON_RECOVER_LAST, grid_settings::CatalogMode::Recent);
    add_membership_item(IFACE_("Favorites"), ICON_SOLO_ON, grid_settings::CatalogMode::Favorites);
    col.separator();
  }

  bool root_items_drawn = false;
  bool folder_section_started = false;

  /* Pass 1: root libraries and separators. Mirrors the columnar selector's first column so a
   * root library that sits after a folder in Preferences is not drawn under that folder's
   * heading. */
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (!item->identifier[0]) {
      if (item->name && item->name[0]) {
        continue; /* Folder heading: second pass. */
      }
      col.separator();
      continue;
    }
    if (library_enum_item_is_in_folder(*item)) {
      continue;
    }
    library_selector_menu_item(
        block, col, handle, *item, current_value, has_item_with_icon, /*show_pin=*/false);
    root_items_drawn = true;
  }

  /* Pass 2: folder headings and their nested libraries, in Preferences order. */
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (!item->identifier[0]) {
      if (item->name && item->name[0]) {
        if (root_items_drawn && !folder_section_started) {
          col.separator();
        }
        if (item->icon) {
          col.label(item->name, item->icon);
        }
        else {
          library_selector_menu_heading(block, col, item->name);
        }
        folder_section_started = true;
      }
      continue;
    }
    if (!library_enum_item_is_in_folder(*item)) {
      continue;
    }
    library_selector_menu_item(
        block, col, handle, *item, current_value, has_item_with_icon, /*show_pin=*/false);
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
  library_selector_seed_retvalue(handle, items, current_value);
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

  /* Two passes: all root libraries into the first column, then one column per folder with its
   * children. Appending a root item after a folder column has already been created breaks
   * #block_bounds_calc_text -- that pass splits columns by button order and xmin, so a late
   * first-column button is absorbed into the last folder column (and keeps its lower Y, which
   * shows up as a gap). Draw roots first so button order matches column order. */

  /* Pass 1: first column -- property title, root-level separators, root libraries. */
  Layout &first_column = columns.column(false);
  {
    const char *title = RNA_property_ui_name(
        prop, RNA_pointer_is_null(&ptr) ? nullptr : &ptr);
    if (title && title[0]) {
      library_selector_menu_heading(block, first_column, title);
    }
    first_column.separator();
  }
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (!item->identifier[0]) {
      if (item->name && item->name[0]) {
        continue; /* Folder heading: second pass. */
      }
      first_column.separator();
      continue;
    }
    if (library_enum_item_is_in_folder(*item)) {
      continue;
    }
    library_selector_menu_item(
        block, first_column, handle, *item, current_value, has_item_with_icon, show_pins);
  }

  /* Pass 2: one column per folder heading, with only that folder's nested libraries. */
  Layout *folder_column = nullptr;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (!item->identifier[0]) {
      if (item->name && item->name[0]) {
        folder_column = &columns.column(false);
        if (item->icon) {
          folder_column->label(item->name, item->icon);
        }
        else {
          library_selector_menu_heading(block, *folder_column, item->name);
        }
        folder_column->separator();
      }
      continue;
    }
    if (!library_enum_item_is_in_folder(*item) || !folder_column) {
      continue;
    }
    library_selector_menu_item(
        block, *folder_column, handle, *item, current_value, has_item_with_icon, show_pins);
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

void template_grid_library_selector(Layout *layout,
                                    bContext *C,
                                    PointerRNA *ptr,
                                    const StringRefNull prop_name,
                                    const bool embed_in_parent_row,
                                    const bool show_refresh)
{
  if (!layout || !C || !ptr || !ptr->data) {
    return;
  }
  if (!RNA_struct_find_property(ptr, prop_name.c_str())) {
    return;
  }

  Layout &row = embed_in_parent_row ? *layout : layout->row(true);
  library_selector_menu_button(
      row, ptr, prop_name, ICON_ASSET_MANAGER, grid_library_selector_menu_draw);

  if (show_refresh) {
    const int enum_value = RNA_enum_get(ptr, prop_name.c_str());
    const AssetLibraryReference lib_ref =
        ed::asset::library_reference_from_enum_value(enum_value);
    if (lib_ref.type != ASSET_LIBRARY_LOCAL) {
      row.op("ASSET_OT_library_refresh", "", ICON_FILE_REFRESH);
    }
  }
}

void template_grid_catalog_selector(Layout *layout,
                                    bContext *C,
                                    PointerRNA *settings_ptr,
                                    const bool embed_in_parent_row)
{
  if (!layout || !C || !settings_ptr || !settings_ptr->data) {
    return;
  }

  ensure_grid_panels_registered();

  Layout &row = embed_in_parent_row ? *layout : layout->row(false);
  if (!embed_in_parent_row) {
    row.emboss_set(EmbossType::Emboss);
    row.ui_units_x_set(1.6f);
  }
  row.context_ptr_set("grid_view_settings", settings_ptr);
  row.popover(C, "GRIDVIEW_PT_catalog_selector", "", ICON_COLLAPSEMENU);

  if (!embed_in_parent_row) {
    Block *block = row.block();
    Button *but = block->buttons_ptrs.last().get();
    but->rect.xmax = but->rect.xmin + short(1.6f * UI_UNIT_X);
  }
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
  /* Popover is always accessible, even when the filter is off, so users can make selections
   * that auto-enable the filter. */
  Layout &popover_layout = row.row(true);
  popover_layout.enabled_set(true);  /* Ensure popover button is always clickable. */
  popover_layout.popover(C, "GRIDVIEW_PT_name_match_filter", "", ICON_DOWNARROW_HLT);
}

/** \} */

}  // namespace blender::ui
