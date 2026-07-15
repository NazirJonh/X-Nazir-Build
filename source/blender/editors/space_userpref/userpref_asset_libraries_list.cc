/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 *
 * Preferences "Asset Libraries" panel: a tree view of the built-in libraries
 * (All / Essentials) plus the user asset libraries organized into folders, with
 * drag & drop reordering and a per-item context menu.
 */

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_preferences.h"
#include "BKE_report.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "userpref_intern.hh"

#include <fmt/format.h>

namespace blender {

struct AnyAssetLibraryDefinition {
  eAssetLibraryType type;
  bUserAssetLibrary *user_library;
};

constexpr int FIXED_ITEMS_COUNT = 2;

/* Flat, remote-aware list of UI items: the two fixed built-in libraries followed by the user
 * libraries (remote ones skipped when the experimental flag is off). #U.active_asset_library
 * indexes into this vector. */
static Vector<AnyAssetLibraryDefinition> userpref_ui_asset_libraries()
{
  Vector<AnyAssetLibraryDefinition> result;

  result.append(AnyAssetLibraryDefinition{ASSET_LIBRARY_ALL, nullptr});
  result.append(AnyAssetLibraryDefinition{ASSET_LIBRARY_ESSENTIALS, nullptr});

  BLI_assert(result.size() == FIXED_ITEMS_COUNT);

  for (bUserAssetLibrary &user_library : U.asset_libraries) {
    if (!USER_EXPERIMENTAL_TEST(&U, use_remote_asset_libraries) &&
        user_library.flag & ASSET_LIBRARY_USE_REMOTE_URL)
    {
      continue;
    }
    result.append(AnyAssetLibraryDefinition{ASSET_LIBRARY_CUSTOM, &user_library});
  }

  return result;
}

int userpref_ui_asset_libraries_count()
{
  /* Instead of constructing the vector (potentially allocating memory), just count the list items
   * and use the fixed item count. */
  if (USER_EXPERIMENTAL_TEST(&U, use_remote_asset_libraries)) {
    const int count = U.asset_libraries.count() + FIXED_ITEMS_COUNT;
    BLI_assert(count == userpref_ui_asset_libraries().size());
    return count;
  }

  /* In case remote libraries are disabled, just retrieve the count from the available items. */
  return userpref_ui_asset_libraries().size();
}

std::optional<int> userpref_ui_asset_libraries_index_from_user_library(
    const bUserAssetLibrary &user_library)
{
  int i = 0;

  const Vector<AnyAssetLibraryDefinition> libraries = userpref_ui_asset_libraries();
  for (const AnyAssetLibraryDefinition &library : libraries) {
    if (library.user_library && library.user_library == &user_library) {
      return i;
    }
    i++;
  }

  return std::nullopt;
}

/* Returns true only when the item itself and all parent folders are enabled. */
static bool library_is_effectively_enabled(const bUserAssetLibrary &library)
{
  if (!library.is_enabled()) {
    return false;
  }
  if (library.parent) {
    return library_is_effectively_enabled(*library.parent);
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Drag & Drop
 * \{ */

class AssetLibraryDragController : public ui::AbstractViewItemDragController {
  bUserAssetLibrary &library_;

 public:
  AssetLibraryDragController(ui::AbstractTreeView &tree_view, bUserAssetLibrary &library)
      : ui::AbstractViewItemDragController(tree_view), library_(library)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_ASSET_LIBRARY;
  }

  void *create_drag_data() const override
  {
    wmDragAssetLibrary *drag_library = MEM_new<wmDragAssetLibrary>(__func__);
    drag_library->library_index = BKE_preferences_asset_library_get_index(&U, &library_);
    return drag_library;
  }
};

class AssetLibraryDropTarget : public ui::TreeViewItemDropTarget {
  bUserAssetLibrary &library_;

 public:
  AssetLibraryDropTarget(ui::AbstractTreeViewItem &item,
                         bUserAssetLibrary &library,
                         ui::DropBehavior behavior)
      : ui::TreeViewItemDropTarget(item, behavior), library_(library)
  {
  }

  bool can_drop(const wmDrag &drag, const char **r_disabled_hint) const override
  {
    if (drag.type != WM_DRAG_ASSET_LIBRARY) {
      return false;
    }

    const wmDragAssetLibrary *drag_library = WM_drag_get_asset_library_data(&drag);
    bUserAssetLibrary *src_library = BKE_preferences_asset_library_find_index(
        &U, drag_library->library_index);

    if (!src_library) {
      return false;
    }

    if (src_library == &library_) {
      *r_disabled_hint = RPT_("Cannot move item to itself");
      return false;
    }

    /* If dragging a folder, disallow any drop inside its own subtree. For an Into drop the target
     * item, and for a Before/After drop the target's parent, would become a descendant of the
     * folder being moved, creating a cycle. Walking up from the target catches both cases,
     * including leaf targets nested inside the folder. */
    if (src_library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      for (bUserAssetLibrary *current = &library_; current; current = current->parent) {
        if (current == src_library) {
          *r_disabled_hint = RPT_("Cannot move folder into itself");
          return false;
        }
      }
    }

    if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER && src_library->parent == &library_ &&
        this->behavior_ == ui::DropBehavior::Insert)
    {
      *r_disabled_hint = RPT_("Item is already in this folder");
      return false;
    }

    return true;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    BLI_assert(drag_info.drag_data.type == WM_DRAG_ASSET_LIBRARY);

    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
          return fmt::format(fmt::runtime(TIP_("Move into folder {}")), library_.name);
        }
        if (library_.parent) {
          return fmt::format(fmt::runtime(TIP_("Move into folder {}")), library_.parent->name);
        }
        return TIP_("Move to root");
      case ui::DropLocation::Before:
        return fmt::format(fmt::runtime(TIP_("Move above {}")), library_.name);
      case ui::DropLocation::After:
        return fmt::format(fmt::runtime(TIP_("Move below {}")), library_.name);
    }

    BLI_assert_unreachable();
    return "";
  }

  bool on_drop(bContext * /*C*/, const ui::DragInfo &drag_info) const override
  {
    BLI_assert(drag_info.drag_data.type == WM_DRAG_ASSET_LIBRARY);
    const wmDragAssetLibrary *drag_library = WM_drag_get_asset_library_data(&drag_info.drag_data);
    bUserAssetLibrary *src_library = BKE_preferences_asset_library_find_index(
        &U, drag_library->library_index);

    if (!src_library) {
      return false;
    }

    eBKE_AssetLibraryMoveLocation location = ASSET_LIBRARY_MOVE_INTO;
    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        location = ASSET_LIBRARY_MOVE_INTO;
        break;
      case ui::DropLocation::Before:
        location = ASSET_LIBRARY_MOVE_BEFORE;
        break;
      case ui::DropLocation::After:
        location = ASSET_LIBRARY_MOVE_AFTER;
        break;
    }

    /* Remember the currently active library by pointer: the reorder physically moves nodes in the
     * listbase, which shifts raw indices, and #U.active_asset_library is an index. Without this
     * the active selection would silently jump to a different item. Fixed items (index < 2) are
     * left untouched. */
    bUserAssetLibrary *active_lib = nullptr;
    {
      const Vector<AnyAssetLibraryDefinition> libs = userpref_ui_asset_libraries();
      if (U.active_asset_library >= 0 && U.active_asset_library < libs.size()) {
        active_lib = libs[U.active_asset_library].user_library;
      }
    }

    if (!BKE_preferences_asset_library_reorder(&U, src_library, &library_, location)) {
      return false;
    }

    if (active_lib) {
      if (const std::optional<int> idx = userpref_ui_asset_libraries_index_from_user_library(
              *active_lib))
      {
        U.active_asset_library = *idx;
      }
    }

    U.runtime.is_dirty = true;
    WM_main_add_notifier(NC_WINDOW, nullptr);

    return true;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tree View Item
 * \{ */

class AssetLibraryListItem : public ui::AbstractTreeViewItem {
  AnyAssetLibraryDefinition library_;
  int index_in_list_ = 0;

 public:
  AssetLibraryListItem(const AnyAssetLibraryDefinition &library, const int index_in_list)
      : library_(library), index_in_list_(index_in_list)
  {
    if (library_.user_library) {
      label_ = library_.user_library->name;
    }
    else {
      const char *name_cstr;
      RNA_enum_name_gettexted(
          rna_enum_asset_library_type_items, library_.type, BLT_I18NCONTEXT_DEFAULT, &name_cstr);
      label_ = name_cstr;
    }
  }

  bool is_folder() const
  {
    return library_.user_library &&
           library_.user_library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER;
  }

  static BIFIconID get_icon(const bUserAssetLibrary &library)
  {
    if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      return ICON_FILE_FOLDER;
    }
    if (library.flag & ASSET_LIBRARY_USE_REMOTE_URL) {
      return ICON_INTERNET;
    }
    return ICON_DISK_DRIVE;
  }

  void build_row(ui::Layout &row) override
  {
    /* Fixed built-in items (All / Essentials). */
    if (!library_.user_library) {
      row.label(label_, ICON_NONE);

      ui::Layout &sub = row.row(true);
      /* Draw text grayed out. */
      sub.active_set(false);
      sub.alignment_set(ui::LayoutAlign::Right);
      sub.label(IFACE_("Built-In"), ICON_NONE);
      return;
    }

    bUserAssetLibrary &library = *library_.user_library;
    const bool is_remote_library = library.flag & ASSET_LIBRARY_USE_REMOTE_URL;

    row.label(label_, get_icon(library));

    if (is_hovered()) {
      if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
        PointerRNA props = row.op("PREFERENCES_OT_asset_library_add",
                                  std::nullopt,
                                  ICON_ADD,
                                  wm::OpCallContext::InvokeDefault,
                                  ui::ITEM_R_ICON_ONLY);
        RNA_string_set(&props, "parent_folder_name", library.name);
      }
    }

    if (library.is_enabled() && is_remote_library && !library.remote_url[0]) {
      row.label("", ICON_ERROR);
    }

    {
      PointerRNA library_ptr = RNA_pointer_create_discrete(
          nullptr, RNA_UserAssetLibrary, &library);
      const BIFIconID icon = library_is_effectively_enabled(library) ? ICON_CHECKBOX_HLT :
                                                                       ICON_CHECKBOX_DEHLT;
      row.prop(&library_ptr, "enabled", ui::ITEM_R_ICON_ONLY, std::nullopt, int(icon));
    }
  }

  void on_activate(bContext & /*C*/) override
  {
    U.active_asset_library = index_in_list_;
    U.runtime.is_dirty = true;
  }

  std::optional<bool> should_be_active() const override
  {
    return U.active_asset_library == index_in_list_;
  }

  bool supports_renaming() const override
  {
    return library_.user_library != nullptr;
  }

  bool rename(const bContext & /*C*/, StringRefNull new_name) override
  {
    if (!library_.user_library) {
      return false;
    }
    /* Must go through the BKE setter: it also propagates a renamed folder's new name to its
     * children's #parent_name, keeping the hierarchy consistent across save/load. */
    BKE_preferences_asset_library_name_set(&U, library_.user_library, new_name.c_str());
    label_ = library_.user_library->name;
    return true;
  }

  void delete_item(bContext *C) override
  {
    if (!library_.user_library) {
      return;
    }
    bUserAssetLibrary &library = *library_.user_library;
    if (!BKE_preferences_asset_library_can_delete(&U, &library)) {
      if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
        BKE_report(CTX_wm_reports(C), RPT_ERROR, "Cannot delete non-empty folder");
      }
      return;
    }
    BKE_preferences_asset_library_remove(&U, &library);
    U.runtime.is_dirty = true;
  }

  void build_context_menu(bContext & /*C*/, ui::Layout &column) const override
  {
    if (!library_.user_library) {
      return;
    }
    bUserAssetLibrary &library = *library_.user_library;

    if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      PointerRNA props = column.op(
          "PREFERENCES_OT_asset_library_add", IFACE_("Add Asset Library"), ICON_NONE);
      RNA_string_set(&props, "parent_folder_name", library.name);

      props = column.op(
          "PREFERENCES_OT_asset_library_folder_add", IFACE_("Add Subfolder"), ICON_FILE_FOLDER);
      RNA_string_set(&props, "parent_folder_name", library.name);

      column.separator();
    }

    column.op("UI_OT_view_item_rename", IFACE_("Rename"), ICON_NONE);

    if (BKE_preferences_asset_library_can_delete(&U, &library)) {
      column.separator();
      column.op("UI_OT_view_item_delete", IFACE_("Delete"), ICON_NONE);
    }
    else if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      column.separator();
      column.label(IFACE_("Folder must be empty to delete"), ICON_ERROR);
    }
  }

  bool should_be_filtered_visible(StringRefNull filter_string) const override
  {
    if (filter_string.is_empty()) {
      return true;
    }
    return BLI_strcasestr(label_.c_str(), filter_string.c_str()) != nullptr;
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    if (!library_.user_library) {
      return nullptr;
    }
    /* Folders support Into, Before, After. Libraries support Before, After only. */
    const ui::DropBehavior behavior = is_folder() ? ui::DropBehavior::ReorderAndInsert :
                                                    ui::DropBehavior::Reorder;
    return std::make_unique<AssetLibraryDropTarget>(*this, *library_.user_library, behavior);
  }

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override
  {
    if (!library_.user_library) {
      return nullptr;
    }
    return std::make_unique<AssetLibraryDragController>(get_tree_view(), *library_.user_library);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tree View
 * \{ */

struct AssetLibraryList : public ui::AbstractTreeView {
  void build_tree() override
  {
    /* Hierarchical: fixed items at root, then the user-library folder tree. */
    this->is_flat_ = false;

    add_tree_item<AssetLibraryListItem>(AnyAssetLibraryDefinition{ASSET_LIBRARY_ALL, nullptr}, 0);
    add_tree_item<AssetLibraryListItem>(
        AnyAssetLibraryDefinition{ASSET_LIBRARY_ESSENTIALS, nullptr}, 1);

    build_user_items_recursive(*this, nullptr);
  }

 private:
  void build_user_items_recursive(ui::TreeViewOrItem &view_parent,
                                  bUserAssetLibrary *parent_folder)
  {
    const bool use_remote = USER_EXPERIMENTAL_TEST(&U, use_remote_asset_libraries);

    for (bUserAssetLibrary &library : U.asset_libraries) {
      if (library.parent != parent_folder) {
        continue;
      }
      /* Same remote filter as #userpref_ui_asset_libraries(), so the displayed set matches the
       * set the index/count helpers assume. Folders never carry the remote flag. */
      if (!use_remote && (library.flag & ASSET_LIBRARY_USE_REMOTE_URL)) {
        continue;
      }
      const std::optional<int> index = userpref_ui_asset_libraries_index_from_user_library(
          library);
      if (!index) {
        continue;
      }

      AssetLibraryListItem &view_item = view_parent.add_tree_item<AssetLibraryListItem>(
          AnyAssetLibraryDefinition{ASSET_LIBRARY_CUSTOM, &library}, *index);

      if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
        build_user_items_recursive(view_item, &library);
      }
    }
  }
};

static void draw_library_list(const bContext &C, ui::Layout &layout)
{
  ui::Block *block = layout.block();

  ui::AbstractTreeView *tree_view = block_add_view(
      *block, "Asset Libraries Preferences", std::make_unique<AssetLibraryList>());
  tree_view->set_context_menu_title("Asset Library");
  tree_view->set_default_rows(5);

  ui::TreeViewBuilder::build_tree_view(C, *tree_view, layout);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panel
 * \{ */

static void draw_active_library_settings(ui::Layout &layout,
                                         const AnyAssetLibraryDefinition &library)
{
  if (library.type == ASSET_LIBRARY_ESSENTIALS) {
    PointerRNA prefs_ptr = RNA_pointer_create_discrete(nullptr, RNA_PreferencesAssetLibraries, &U);

    ui::Layout &row = layout.row(false);
    row.active_set((G.f & G_FLAG_INTERNET_ALLOW) != 0);
    row.prop(&prefs_ptr,
             "use_online_essentials",
             UI_ITEM_NONE,
             IFACE_("Include Online Essentials"),
             ICON_NONE);
  }

  if (library.user_library) {
    /* Folders have no path/import settings. */
    if (library.user_library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      return;
    }

    PointerRNA library_ptr = RNA_pointer_create_discrete(
        nullptr, RNA_UserAssetLibrary, library.user_library);

    if (library.user_library->flag & ASSET_LIBRARY_USE_REMOTE_URL) {
      if (USER_EXPERIMENTAL_TEST(&U, use_remote_asset_libraries)) {
        ui::Layout &row = layout.row(false);
        row.red_alert_set(!library.user_library->remote_url[0]);
        row.prop(&library_ptr,
                 RNA_struct_find_property(&library_ptr, "remote_url"),
                 RNA_NO_INDEX,
                 0,
                 UI_ITEM_NONE,
                 "",
                 ICON_INTERNET,
                 IFACE_("Repository URL"));
      }
      layout.prop(&library_ptr, "import_method", UI_ITEM_NONE, IFACE_("Import Method"), ICON_NONE);
    }
    else {
      layout.prop(&library_ptr, "path", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(&library_ptr, "import_method", UI_ITEM_NONE, IFACE_("Import Method"), ICON_NONE);
      layout.prop(&library_ptr, "use_relative_path", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
}

void userpref_asset_libraries_panel_draw(const bContext *C, Panel *panel)
{
  Vector<AnyAssetLibraryDefinition> libraries = userpref_ui_asset_libraries();

  ui::Layout &layout = *panel->layout;

  ui::Layout &row = layout.row(false);

  draw_library_list(*C, row);

  ui::Layout &col = row.column(true);
  if (USER_EXPERIMENTAL_TEST(&U, use_remote_asset_libraries)) {
    col.op_menu_enum(C, "preferences.asset_library_add", "type", "", ICON_ADD);
  }
  else {
    PointerRNA props = col.op("preferences.asset_library_add", "", ICON_ADD);
    RNA_enum_set(&props, "type", ASSET_LIBRARY_LOCAL);
  }

  /* Add a folder at the root level. */
  col.op("preferences.asset_library_folder_add", "", ICON_NEWFOLDER);

  ui::Layout &sub = col.row(true);
  const bool active_idx_in_range = U.active_asset_library >= 0 &&
                                   U.active_asset_library < libraries.size();
  /* Removable only when the active item is a user library/folder that can be deleted (folders must
   * be empty; #BKE_preferences_asset_library_remove asserts on non-empty folders). */
  bool can_remove = false;
  if (active_idx_in_range) {
    const AnyAssetLibraryDefinition &active = libraries[U.active_asset_library];
    can_remove = active.type == ASSET_LIBRARY_CUSTOM && active.user_library &&
                 BKE_preferences_asset_library_can_delete(&U, active.user_library);
  }
  sub.enabled_set(can_remove);
  PointerRNA props = sub.op("preferences.asset_library_remove", "", ICON_REMOVE);
  /* Convert from UI-items list index to #U.asset_libraries index. */
  RNA_int_set(&props, "index", U.active_asset_library - FIXED_ITEMS_COUNT);

  if (!active_idx_in_range) {
    return;
  }

  layout.separator();

  draw_active_library_settings(layout, libraries[U.active_asset_library]);
}

/** \} */

}  // namespace blender
