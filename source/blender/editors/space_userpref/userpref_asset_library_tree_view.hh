/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 *
 * Tree view for Asset Libraries in Preferences, supporting folder organization
 * and drag & drop reordering.
 */

#pragma once

#include "DNA_userdef_types.h"

#include "UI_tree_view.hh"

namespace blender {

class bContext;

namespace ed::userpref {

class AssetLibraryTreeView : public ui::AbstractTreeView {
  UserDef &userdef_;

  friend class AssetLibraryTreeViewItem;
  friend class AssetLibraryDragController;
  friend class AssetLibraryDropTarget;

 public:
  explicit AssetLibraryTreeView(UserDef &userdef);

  void build_tree() override;

 private:
  void build_items_recursive(ui::TreeViewOrItem &view_parent_item,
                             bUserAssetLibrary *parent_folder);
};

/**
 * Create the asset library tree-view in the given layout.
 * Called from Python via layout.template_asset_library_tree_view().
 */
void userpref_create_asset_library_tree_view_in_layout(const bContext *C,
                                                        ui::Layout &layout,
                                                        UserDef *userdef);

}  // namespace ed::userpref

}  // namespace blender
