/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 *
 * Tree view for Asset Libraries in Preferences, supporting folder organization
 * and drag & drop reordering.
 */

#include "userpref_asset_library_tree_view.hh"

#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_preferences.h"
#include "BKE_report.hh"

#include "ED_userpref.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_interface_c.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include <fmt/format.h>

namespace blender {

namespace ed::userpref {

/* Forward declarations. */
class AssetLibraryTreeView;
class AssetLibraryTreeViewItem;
class AssetLibraryDragController;
class AssetLibraryDropTarget;

/* ---------------------------------------------------------------------- */
/** \name AssetLibraryTreeViewItem
 * \{ */

class AssetLibraryTreeViewItem : public ui::BasicTreeViewItem {
  bUserAssetLibrary &library_;

 public:
  AssetLibraryTreeViewItem(bUserAssetLibrary &library)
      : BasicTreeViewItem(library.name, get_icon(library)), library_(library)
  {
  }

  static BIFIconID get_icon(bUserAssetLibrary &library)
  {
    if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      return ICON_FILE_FOLDER;
    }
    if (library.flag & ASSET_LIBRARY_USE_REMOTE_URL) {
      return ICON_INTERNET;
    }
    return ICON_DISK_DRIVE;
  }

  void on_activate(bContext & /*C*/) override;

  void build_row(ui::Layout &row) override;

  void build_context_menu(bContext & /*C*/, ui::Layout &column) const override
  {
    if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      column.op("PREFERENCES_OT_asset_library_add", IFACE_("Add Asset Library"), ICON_NONE);
      column.op("PREFERENCES_OT_asset_library_folder_add", IFACE_("Add Subfolder"), ICON_FILE_FOLDER);
      column.separator();
    }

    column.op("UI_OT_view_item_rename", IFACE_("Rename"), ICON_NONE);

    if (BKE_preferences_asset_library_can_delete(&U, &library_)) {
      column.separator();
      column.op("UI_OT_view_item_delete", IFACE_("Delete"), ICON_NONE);
    }
    else if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      column.separator();
      column.label(IFACE_("Folder must be empty to delete"), ICON_ERROR);
    }
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    BasicTreeViewItem::rename(C, new_name);
    BKE_preferences_asset_library_name_set(&U, &library_, new_name.c_str());
    return true;
  }

  void delete_item(bContext *C) override
  {
    if (!BKE_preferences_asset_library_can_delete(&U, &library_)) {
      if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
        BKE_report(CTX_wm_reports(C), RPT_ERROR, "Cannot delete non-empty folder");
      }
      return;
    }

    BKE_preferences_asset_library_remove(&U, &library_);
    U.runtime.is_dirty = true;
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override;

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override;

  bool is_folder() const
  {
    return library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER;
  }

  bUserAssetLibrary &get_library() const
  {
    return library_;
  }

  bool should_be_filtered_visible(StringRefNull filter_string) const override
  {
    if (filter_string.is_empty()) {
      return true;
    }
    /* Check if the library name contains the filter string (case-insensitive). */
    const char *haystack = label_.c_str();
    const char *needle = filter_string.c_str();

    if (!haystack[0] || !needle[0]) {
      return true;
    }

    /* Simple case-insensitive substring search. */
    const int haystack_len = strlen(haystack);
    const int needle_len = strlen(needle);

    if (needle_len > haystack_len) {
      return false;
    }

    for (int i = 0; i <= haystack_len - needle_len; i++) {
      bool match = true;
      for (int j = 0; j < needle_len; j++) {
        if (tolower(haystack[i + j]) != tolower(needle[j])) {
          match = false;
          break;
        }
      }
      if (match) {
        return true;
      }
    }

    return false;
  }
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name AssetLibraryDragController
 * \{ */

class AssetLibraryDragController : public ui::AbstractViewItemDragController {
  bUserAssetLibrary &library_;

 public:
  explicit AssetLibraryDragController(bUserAssetLibrary &library)
      : ui::AbstractViewItemDragController(*static_cast<ui::AbstractTreeView *>(nullptr)),
        library_(library)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_ASSET_LIBRARY;
  }

  void *create_drag_data() const override
  {
    wmDragAssetLibrary *drag_library = MEM_new<wmDragAssetLibrary>(__func__);
    STRNCPY(drag_library->library_name, library_.name);
    drag_library->library_index = BKE_preferences_asset_library_get_index(&U, &library_);
    return drag_library;
  }
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name AssetLibraryDropTarget
 * \{ */

class AssetLibraryDropTarget : public ui::TreeViewItemDropTarget {
  bUserAssetLibrary &library_;

 public:
  AssetLibraryDropTarget(AssetLibraryTreeViewItem &item,
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

    if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      bUserAssetLibrary *current = &library_;
      while (current) {
        if (current == src_library) {
          *r_disabled_hint = RPT_("Cannot move folder into itself");
          return false;
        }
        current = current->parent;
      }

      if (src_library->parent == &library_ && this->behavior_ == ui::DropBehavior::Insert) {
        *r_disabled_hint = RPT_("Item is already in this folder");
        return false;
      }
    }

    return true;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    BLI_assert(drag_info.drag_data.type == WM_DRAG_ASSET_LIBRARY);
    const wmDragAssetLibrary *drag_library = WM_drag_get_asset_library_data(&drag_info.drag_data);
    bUserAssetLibrary *src_library = BKE_preferences_asset_library_find_index(
        &U, drag_library->library_index);

    const char *item_type = src_library->type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER ?
                                "folder" :
                                "library";

    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
          return fmt::format(
              fmt::runtime(TIP_("Move {} into folder {}")), item_type, library_.name);
        }
        else {
          if (library_.parent) {
            return fmt::format(
                fmt::runtime(TIP_("Move {} into folder {}")), item_type, library_.parent->name);
          }
          else {
            return fmt::format(fmt::runtime(TIP_("Move {} to root")), item_type);
          }
        }
      case ui::DropLocation::Before:
        return fmt::format(
            fmt::runtime(TIP_("Move {} above {}")), item_type, library_.name);
      case ui::DropLocation::After:
        return fmt::format(
            fmt::runtime(TIP_("Move {} below {}")), item_type, library_.name);
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

    /* Map DropLocation to MoveLocation: Into=0, Before=1, After=2 */
    int location = 0;
    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        location = 0;
        break;
      case ui::DropLocation::Before:
        location = 1;
        break;
      case ui::DropLocation::After:
        location = 2;
        break;
    }

    BKE_preferences_asset_library_reorder(&U, src_library, &library_, location);
    U.runtime.is_dirty = true;
    WM_main_add_notifier(NC_WINDOW, nullptr);

    return true;
  }
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name AssetLibraryTreeViewItem Implementation
 * \{ */

void AssetLibraryTreeViewItem::on_activate(bContext & /*C*/)
{
  if (library_.type != USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    const int index = BKE_preferences_asset_library_get_index(&U, &library_);
    U.active_asset_library = index;
    U.runtime.is_dirty = true;
  }
}

void AssetLibraryTreeViewItem::build_row(ui::Layout &row)
{
  this->add_label(row, label_);

  if (!is_hovered()) {
    return;
  }

  ui::ButtonViewItem *view_item_but = view_item_button();

  if (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    PointerRNA *props = button_extra_operator_icon_add(
        reinterpret_cast<ui::Button *>(view_item_but),
        "PREFERENCES_OT_asset_library_add",
        wm::OpCallContext::InvokeDefault,
        ICON_ADD);
    RNA_string_set(props, "parent_folder_name", library_.name);

    props = button_extra_operator_icon_add(
        reinterpret_cast<ui::Button *>(view_item_but),
        "PREFERENCES_OT_asset_library_folder_add",
        wm::OpCallContext::InvokeDefault,
        ICON_NEWFOLDER);
    RNA_string_set(props, "parent_folder_name", library_.name);
  }
  else {
    PointerRNA *props = button_extra_operator_icon_add(
        reinterpret_cast<ui::Button *>(view_item_but),
        "PREFERENCES_OT_asset_library_remove",
        wm::OpCallContext::InvokeDefault,
        ICON_X);
    const int index = BKE_preferences_asset_library_get_index(&U, &library_);
    RNA_int_set(props, "index", index);
  }
}

std::unique_ptr<ui::TreeViewItemDropTarget> AssetLibraryTreeViewItem::create_drop_target()
{
  /* Folders support Into, Before, After. Libraries support Before, After only. */
  ui::DropBehavior behavior = (library_.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) ?
                                  ui::DropBehavior::ReorderAndInsert :
                                  ui::DropBehavior::Reorder;
  return std::make_unique<AssetLibraryDropTarget>(*this, library_, behavior);
}

std::unique_ptr<ui::AbstractViewItemDragController> AssetLibraryTreeViewItem::
    create_drag_controller() const
{
  return std::make_unique<AssetLibraryDragController>(library_);
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name AssetLibraryTreeView
 * \{ */

AssetLibraryTreeView::AssetLibraryTreeView(UserDef &userdef) : userdef_(userdef) {}

void AssetLibraryTreeView::build_tree()
{
  build_items_recursive(*this, nullptr);
}

void AssetLibraryTreeView::build_items_recursive(ui::TreeViewOrItem &view_parent_item,
                                                  bUserAssetLibrary *parent_folder)
{
  for (bUserAssetLibrary &library : userdef_.asset_libraries) {
    if (library.parent != parent_folder) {
      continue;
    }

    AssetLibraryTreeViewItem &view_item =
        view_parent_item.add_tree_item<AssetLibraryTreeViewItem>(library);

    view_item.set_is_active_fn([this, &library]() {
      const int index = BKE_preferences_asset_library_get_index(&userdef_, &library);
      return userdef_.active_asset_library == index;
    });

    if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      build_items_recursive(view_item, &library);
    }
  }
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Public API
 * \{ */

void userpref_create_asset_library_tree_view_in_layout(const bContext *C,
                                                        ui::Layout &layout,
                                                        UserDef *userdef)
{
  ui::Block *block = layout.block();
  ui::block_layout_set_current(block, &layout);

  ui::AbstractTreeView *tree_view = block_add_view(
      *block,
      "asset library tree view",
      std::make_unique<ed::userpref::AssetLibraryTreeView>(*userdef));
  tree_view->set_context_menu_title("Asset Library");
  tree_view->set_default_rows(6);
  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

/** \} */

}  // namespace ed::userpref

namespace ui {

void template_asset_library_tree_view(Layout *layout, bContext *C)
{
  ed::userpref::userpref_create_asset_library_tree_view_in_layout(C, *layout, &U);
}

}  // namespace ui

}  // namespace blender
