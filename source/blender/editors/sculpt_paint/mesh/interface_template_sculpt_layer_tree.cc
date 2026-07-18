/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <fmt/format.h>

#include "BKE_context.hh"
#include "BKE_sculpt_layers.hh"

#include "BLI_listbase.h"
#include "BLI_vector.hh"
#include "BLT_translation.hh"

#include "ED_undo.hh"

#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::layers {

class SculptLayerTreeView : public ui::AbstractTreeView {
 protected:
  Object &object_;

 public:
  explicit SculptLayerTreeView(Object &ob) : object_(ob) {}

  void build_tree() override;

 private:
  void build_tree_recursive(ui::TreeViewOrItem &parent, const SculptLayerGroup &group);
};

struct SculptLayerRef {
  Object *object;
  SculptLayer *layer;
};

class SculptLayerDragController : public ui::AbstractViewItemDragController {
 private:
  Mesh *mesh_;

 public:
  SculptLayerDragController(SculptLayerTreeView &view, Mesh *mesh)
      : AbstractViewItemDragController(view), mesh_(mesh)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_SCULPT_LAYER;
  }

  void *create_drag_data() const override
  {
    /* Held across both loops, which is safe because neither mutates the tree: the span is a view
     * into the root folder's cache and any structural change would rebuild it. */
    const Span<SculptLayer *> layers = bke::sculpt_layers::layers(*mesh_);
    int selected_count = 0;
    for (const SculptLayer *layer : layers) {
      selected_count += (layer->base.flag & SCULPT_LAYER_SELECTED) != 0;
    }

    /* Allocate one extra element, to use it as null-delimiter. */
    SculptLayer **selected_layers = MEM_new_array_zeroed<SculptLayer *>(selected_count + 1,
                                                                        "Selected Sculpt Layers");
    int i = 0;
    for (SculptLayer *layer : layers) {
      if (layer->base.flag & SCULPT_LAYER_SELECTED) {
        selected_layers[i] = layer;
        i++;
      }
    }
    BLI_assert_msg(selected_layers[i] == nullptr,
                   "Expected last element to be null (null-delimiter)");
    return selected_layers;
  }
};

/* True when \a dest is a folder that is itself being dragged, or one nested inside such a folder.
 * Moving a folder there would detach its subtree from the root. A null \a dest — a node with no
 * parent, i.e. the root — is never inside anything. The dragged set is read back from the
 * #SCULPT_LAYER_GROUP_SELECTED flags rather than from #wmDrag::poin, because that is the set
 * #SCULPT_OT_layer_move_to itself acts on — the drag data only carries layers, and serves to mark
 * the drag type. */
static bool group_dest_is_inside_dragged_group(const Mesh &mesh, const SculptLayerGroup *dest)
{
  if (dest == nullptr) {
    return false;
  }
  /* #bke::sculpt_layers::groups never yields the root, which is exactly right here: the root is
   * never drawn as a row, so it can never carry the selection flag. */
  for (const SculptLayerGroup *group : bke::sculpt_layers::groups(mesh)) {
    if (!(group->base.flag & SCULPT_LAYER_GROUP_SELECTED)) {
      continue;
    }
    /* Identity as well as "strictly below": #bke::sculpt_layers::node_is_descendant_of is strict —
     * a node is not its own descendant — and dropping a folder into itself is just as illegal as
     * dropping it into its own subtree. Same pair of tests as in #SCULPT_OT_layer_move_to, which
     * is the operator this target ends up calling. */
    if (dest == group || bke::sculpt_layers::node_is_descendant_of(dest->base, *group)) {
      return true;
    }
  }
  return false;
}

class SculptLayerDropTarget : public ui::TreeViewItemDropTarget {
 private:
  /* The node this row draws, of either kind. One node type carrying one uid space is what removed
   * the former "exactly one of a layer and a group pointer is set" pair: the drop only ever needs
   * the anchor's identity, and #SculptLayerTreeNode::uid now names it whatever kind it is. */
  SculptLayerTreeNode *drop_node_;
  Mesh *mesh_;

 public:
  SculptLayerDropTarget(ui::AbstractTreeViewItem &item,
                        ui::DropBehavior behavior,
                        SculptLayerTreeNode *drop_node,
                        Mesh *mesh)
      : TreeViewItemDropTarget(item, behavior), drop_node_(drop_node), mesh_(mesh)
  {
    BLI_assert(drop_node_ != nullptr);
  }

  bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    if (drag.type != WM_DRAG_SCULPT_LAYER) {
      return false;
    }
    /* Before/After land beside this row, i.e. in the folder that holds it. Reject the row outright
     * when that folder is inside a dragged folder — no drop location this row offers would be
     * legal then. #Into has a different destination and is filtered in #choose_drop_location
     * instead, so that a folder can still be dropped next to (just not inside) itself. */
    return !group_dest_is_inside_dragged_group(*mesh_, drop_node_->parent);
  }

  std::optional<ui::DropLocation> choose_drop_location(const ARegion &region,
                                                       const wmEvent &event) const override
  {
    const std::optional<ui::DropLocation> location = TreeViewItemDropTarget::choose_drop_location(
        region, event);
    /* #can_drop cannot do this: it is not told where in the row the cursor is, and Into is the one
     * location whose destination is this folder itself rather than its parent. Unsetting disables
     * the drop for this band of the row only (see #DropTargetInterface::choose_drop_location).
     * A layer row yields a null group here and therefore never filters, which costs nothing: it
     * offers no Into to begin with (see #create_drop_target). */
    if (location == ui::DropLocation::Into &&
        group_dest_is_inside_dragged_group(*mesh_, bke::sculpt_layers::node_as_group(drop_node_)))
    {
      return std::nullopt;
    }
    return location;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const StringRef drag_name = TIP_("Selected Items");
    const StringRef drop_name = drop_node_->name;

    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        /* Only a folder row offers Into (#DropBehavior::ReorderAndInsert); a layer row stays
         * #DropBehavior::Reorder, which never yields it. */
        BLI_assert(bke::sculpt_layers::node_as_group(drop_node_) != nullptr);
        return fmt::format(fmt::runtime(TIP_("Move {} into {}")), drag_name, drop_name);
      case ui::DropLocation::Before:
        return fmt::format(fmt::runtime(TIP_("Move {} above {}")), drag_name, drop_name);
      case ui::DropLocation::After:
        return fmt::format(fmt::runtime(TIP_("Move {} below {}")), drag_name, drop_name);
      default:
        BLI_assert_unreachable();
        break;
    }
    return "";
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_layer_move_to", false);
    PointerRNA props_ptr = WM_operator_properties_create_ptr(ot);
    /* The uid alone names the anchor row: one counter (#bke::sculpt_layers::node_unique_uid) spans
     * both kinds, so the operator resolves the kind itself and no longer needs to be told it. */
    RNA_int_set(&props_ptr, "anchor_uid", drop_node_->uid);
    /* Set by value rather than by identifier string: #RNA_enum_set needs no #bContext and cannot
     * silently no-op on a typo'd identifier. The operator's enum items are defined from these same
     * #MoveLocation values. */
    MoveLocation location = MoveLocation::After;
    switch (drag_info.drop_location) {
      case ui::DropLocation::Before:
        location = MoveLocation::Before;
        break;
      case ui::DropLocation::After:
        location = MoveLocation::After;
        break;
      case ui::DropLocation::Into:
        location = MoveLocation::Into;
        break;
    }
    RNA_enum_set(&props_ptr, "location", int(location));
    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::ExecDefault, &props_ptr, nullptr);
    WM_operator_properties_free(&props_ptr);
    return true;
  }
};

/* True when \a group and every folder above it carry \a flag. A null \a group means "no folder" —
 * the parent of the root — which is inside nothing and therefore passes trivially. The root itself
 * is walked like any other folder and carries both flags this is asked about (see
 * #bke::sculpt_layers::root_group_ensure).
 *
 * The cycle-safe ancestor walk lives in #bke::sculpt_layers::find_ancestor, shared with
 * #bke::sculpt_layers::node_is_descendant_of; here it just looks for the first folder *lacking* the
 * flag, and "all carry it" is "none lacks it". */
static bool ancestors_all_have_flag(const SculptLayerGroup *group, const int flag)
{
  return bke::sculpt_layers::find_ancestor(group, [&](const SculptLayerGroup &ancestor) {
           return !(ancestor.base.flag & flag);
         }) == nullptr;
}

/* True when no ancestor folder currently hides \a group. Only the row's own greying-out depends on
 * it — the layers' actual visibility is the stored #SCULPT_LAYER_GROUP_HIDDEN bit, not this. */
static bool group_visible_by_ancestors(const SculptLayerGroup &group)
{
  return ancestors_all_have_flag(group.base.parent, SCULPT_LAYER_GROUP_ENABLED);
}

/* True when the row for a node held by folder \a parent is actually drawn, i.e. no folder on the
 * way up to the root is collapsed. */
static bool row_is_revealed(const SculptLayerGroup *parent)
{
  return ancestors_all_have_flag(parent, SCULPT_LAYER_GROUP_EXPANDED);
}

class SculptLayerGroupItem : public ui::AbstractTreeViewItem {
 private:
  Object *object_;
  SculptLayerGroup *group_;
  /** Copied rather than read back through #group_: #delete_item runs the remove operator, which
   * frees the group, and the item outlives it (mirrors #SculptLayerItem::uid_). */
  int uid_;

 public:
  SculptLayerGroupItem(Object *object, SculptLayerGroup *group)
      : object_(object), group_(group), uid_(group->base.uid)
  {
    label_ = group->base.name;
    /* A folder must not hold the view's active state, because nothing in the data can keep it
     * there: #Mesh::sculpt_layers_active_uid names a layer, and no field names a folder. Taking the
     * state anyway starts a tug of war - the active layer's #should_be_active still returns true,
     * so the next redraw hands the state straight back through
     * #AbstractTreeViewItem::set_state_active, whose #ensure_parents_uncollapsed then rewrites this
     * folder's #SCULPT_LAYER_GROUP_EXPANDED. That silently undid every collapse, since a chevron
     * click activates the folder whenever it holds the active layer (see
     * #collapse_chevron_click_fn). Refusing the state stops #set_state_active before it deactivates
     * the layer, so nothing is handed back and the collapse stands. The row still answers to a
     * click: a selected but inactive view item draws with the selected background, and
     * #on_activate below keeps doing the selecting that #AbstractViewItem::activate now skips. */
    this->disable_activatable();
    this->always_reactivate_on_click();
  }

  void build_row(ui::Layout &row) override
  {
    /* The color tag replaces the folder icon rather than sitting beside it, as in the Grease
     * Pencil layer tree. The `- 1` offset is the price of #SCULPT_LAYER_COLOR_NONE being zero
     * (see #eSculptLayerColorTag); Grease Pencil's -1 sentinel needs no offset. */
    int folder_icon = ICON_FILE_FOLDER;
    if (group_->base.color_tag != SCULPT_LAYER_COLOR_NONE) {
      folder_icon = ICON_SCULPT_LAYERGROUP_COLOR_01 + int(group_->base.color_tag) - 1;
    }
    uiItemL_ex(&row, this->label_, folder_icon, false, false);

    /* Influence and the eye share one right-aligned container so the eyes line up in a single
     * column at the panel edge whatever the row's name length. Unaligned (`false`) so the slider
     * and the eye do not read as one glued button block, and separate sub-rows because their
     * greying rules differ. */
    ui::Layout &right = row.row(false);
    right.alignment_set(ui::LayoutAlign::Right);

    /* Folder influence slider, mirroring the layer row's: it scales every descendant layer's
     * contribution through the ancestor cascade. */
    Mesh &mesh = *id_cast<Mesh *>(object_->data);
    PointerRNA group_ptr = RNA_pointer_create_discrete(&mesh.id, RNA_SculptLayerGroup, group_);
    ui::Layout &sub = right.row(true);
    sub.use_property_decorate_set(false);
    sub.active_set(object_->mode != OB_MODE_EDIT);
    sub.prop(&group_ptr, "influence", ui::ITEM_R_SLIDER, "", ICON_NONE);

    ui::Layout &vis = right.row(true);
    /* Grey out the eye when an ancestor already hides this folder: the toggle still works and
     * still means "this folder's own state", it just cannot make anything visible right now.
     * Computed on the fly rather than stored — only visible rows pay for it, and it is not undo
     * state. */
    vis.active_set(group_visible_by_ancestors(*group_));
    const int vis_icon = (group_->base.flag & SCULPT_LAYER_GROUP_ENABLED) ? ICON_HIDE_OFF :
                                                                            ICON_HIDE_ON;
    /* An operator button, not the RNA property: #SculptLayerGroup.enabled is deliberately
     * non-editable, because a bare flag write would skip the descendants'
     * #SCULPT_LAYER_GROUP_HIDDEN resync and the sculpt undo push that the cascade needs. */
    PointerRNA op_ptr = vis.op("SCULPT_OT_layer_group_toggle_visibility",
                               "",
                               vis_icon,
                               wm::OpCallContext::ExecDefault,
                               UI_ITEM_NONE);
    RNA_int_set(&op_ptr, "group_uid", uid_);
  }

  std::optional<bool> should_be_collapsed() const override
  {
    return !(group_->base.flag & SCULPT_LAYER_GROUP_EXPANDED);
  }

  bool set_collapsed(const bool collapsed) override
  {
    if (!AbstractTreeViewItem::set_collapsed(collapsed)) {
      return false;
    }
    /* Overridden rather than only #on_collapse_change, because the view calls #set_collapsed
     * directly — from #change_state_delayed and #ensure_parents_uncollapsed — without going
     * through #on_collapse_change, and the DNA flag has to follow those too. No undo and no
     * notifier: the expanded state is pure UI state, like the row selection. */
    SET_FLAG_FROM_TEST(group_->base.flag, !collapsed, SCULPT_LAYER_GROUP_EXPANDED);
    return true;
  }

  void on_activate(bContext & /*C*/) override
  {
    /* #AbstractViewItem::activate selects the row it activates, but only once the row has taken the
     * active state - which this one refuses (see the constructor). Select it here instead, so that
     * clicking a folder still marks it for the operators that read the selection back out of the
     * flags (#SCULPT_OT_layer_move_to, #SCULPT_OT_layer_group_add). Reached on every click thanks
     * to #always_reactivate_on_click, since #set_state_active never reports a change. */
    this->set_selected(true);
  }

  std::optional<bool> should_be_selected() const override
  {
    return group_->base.flag & SCULPT_LAYER_GROUP_SELECTED;
  }

  void set_selected(const bool select) override
  {
    AbstractViewItem::set_selected(select);
    SET_FLAG_FROM_TEST(group_->base.flag, select, SCULPT_LAYER_GROUP_SELECTED);
  }

  bool matches_single(const ui::AbstractTreeViewItem &other) const override
  {
    /* Row identity is by kind as well as by label: a rename in progress can momentarily give a
     * folder row and a layer row the same text before #node_name_ensure_unique settles it. Without
     * this override, #AbstractTreeViewItem::matches_single's label-only comparison would let
     * #update_from_old carry a folder row's state - a rename in progress, in particular - onto an
     * unrelated layer row of the same name. */
    return dynamic_cast<const SculptLayerGroupItem *>(&other) != nullptr &&
           AbstractTreeViewItem::matches_single(other);
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    bContext &ctx = const_cast<bContext &>(C);
    Object &object = *object_;
    Mesh &mesh = *id_cast<Mesh *>(object.data);

    /* Mirrors #SculptLayerItem::rename exactly: in Sculpt Mode a memfile step would not compose
     * with the delta-based stroke steps, so the name rides in a #Type::SculptLayer metadata step;
     * outside it there are no sculpt steps to interleave with, and no session for
     * #push_begin_ex. */
    const bool use_sculpt_undo = (object.mode & OB_MODE_SCULPT) != 0;
    if (use_sculpt_undo) {
      undo::push_begin_ex(*CTX_data_scene(&ctx), object, "Rename Sculpt Layer Group");
      undo::push_sculpt_layer_metadata(object, group_->base);
    }

    PointerRNA group_ptr = RNA_pointer_create_discrete(&mesh.id, RNA_SculptLayerGroup, group_);
    PropertyRNA *prop = RNA_struct_find_property(&group_ptr, "name");
    RNA_property_string_set(&group_ptr, prop, new_name.c_str());
    RNA_property_update(&ctx, &group_ptr, prop);

    if (use_sculpt_undo) {
      undo::push_end(object);
    }
    else {
      ED_undo_push(&ctx, "Rename Sculpt Layer Group");
    }
    return true;
  }

  StringRef get_rename_string() const override
  {
    return label_;
  }

  void delete_item(bContext *C) override
  {
    Mesh &mesh = *id_cast<Mesh *>(object_->data);
    /* Deliberately no "active only" guard, unlike #SculptLayerItem::delete_item: the group
     * operator takes an explicit uid, so #view_item_delete_invoke firing once per selected row
     * disbands exactly the folders the user selected — each call is addressed at its own row and
     * independent of the others. The lookup guards only against a row whose group a previous call
     * already removed. Each row's removal is its own undo step, which is acceptable because
     * disbanding is cheap and non-destructive (the layers survive). The cast is what makes one uid
     * space safe to look a folder up in: #node_find_by_uid resolves any node, and reading a layer
     * as a folder would run the operator against the wrong row. Uid 0 needs no guard here — it
     * names the root, which is never drawn and so is never a row's uid. */
    if (bke::sculpt_layers::node_as_group(bke::sculpt_layers::node_find_by_uid(mesh, uid_)) ==
        nullptr)
    {
      return;
    }
    wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_layer_group_remove", false);
    PointerRNA props_ptr = WM_operator_properties_create_ptr(ot);
    RNA_int_set(&props_ptr, "group_uid", uid_);
    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::ExecDefault, &props_ptr, nullptr);
    WM_operator_properties_free(&props_ptr);
  }

  void build_context_menu(bContext & /*C*/, ui::Layout &layout) const override
  {
    /* Every entry below is drawn here rather than through a Python menu like the layer rows use:
     * a layer menu can act on the active layer, but folders deliberately have no "active" concept
     * — a folder is only ever addressed by explicit uid, and only this item knows which uid its
     * row is. That is also why the color swatches are one button per tag instead of
     * #operator_enum, which Grease Pencil uses: that helper emits a button per enum value but
     * cannot also set #group_uid. */
    ui::Layout &colors = layout.row(true);
    for (int tag = SCULPT_LAYER_COLOR_NONE; tag <= SCULPT_LAYER_COLOR_08; tag++) {
      const int icon = (tag == SCULPT_LAYER_COLOR_NONE) ?
                           ICON_X :
                           ICON_SCULPT_LAYERGROUP_COLOR_01 + tag - 1;
      PointerRNA color_ptr = colors.op("SCULPT_OT_layer_group_color_tag",
                                       "",
                                       icon,
                                       wm::OpCallContext::ExecDefault,
                                       UI_ITEM_NONE);
      RNA_int_set(&color_ptr, "group_uid", uid_);
      RNA_enum_set(&color_ptr, "color_tag", tag);
    }
    layout.separator();

    PointerRNA merge_ptr = layout.op("SCULPT_OT_layer_group_merge",
                                     IFACE_("Merge Layers"),
                                     ICON_AUTOMERGE_ON,
                                     wm::OpCallContext::ExecDefault,
                                     UI_ITEM_NONE);
    RNA_int_set(&merge_ptr, "group_uid", uid_);

    PointerRNA op_ptr = layout.op("SCULPT_OT_layer_group_remove",
                                  IFACE_("Remove Group"),
                                  ICON_X,
                                  wm::OpCallContext::ExecDefault,
                                  UI_ITEM_NONE);
    RNA_int_set(&op_ptr, "group_uid", uid_);

    /* Invoke (not exec) so the operator's confirmation popup runs: this one deletes the layers,
     * unlike "Remove Group" which keeps them. */
    PointerRNA delete_ptr = layout.op("SCULPT_OT_layer_group_delete",
                                      IFACE_("Delete Group and Layers"),
                                      ICON_TRASH,
                                      wm::OpCallContext::InvokeDefault,
                                      UI_ITEM_NONE);
    RNA_int_set(&delete_ptr, "group_uid", uid_);
  }

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override
  {
    Mesh *mesh = id_cast<Mesh *>(object_->data);
    return std::make_unique<SculptLayerDragController>(
        static_cast<SculptLayerTreeView &>(get_tree_view()), mesh);
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    Mesh *mesh = id_cast<Mesh *>(object_->data);
    /* Only a folder row offers Into; a layer row stays #DropBehavior::Reorder. */
    return std::make_unique<SculptLayerDropTarget>(
        *this, ui::DropBehavior::ReorderAndInsert, &group_->base, mesh);
  }
};

class SculptLayerItem : public ui::AbstractTreeViewItem {
 private:
  SculptLayerRef layer_ref_;
  /** Copied rather than read back through #layer_ref_, which #delete_item may outlive. */
  int uid_;

 public:
  SculptLayerItem(Object *object, SculptLayer *layer)
  {
    label_ = layer->base.name;
    layer_ref_.object = object;
    layer_ref_.layer = layer;
    uid_ = layer->base.uid;
  }

  void build_row(ui::Layout &row) override
  {
    Mesh &mesh = *id_cast<Mesh *>(layer_ref_.object->data);
    const SculptLayer &layer = *layer_ref_.layer;
    const bool valid = !bke::sculpt_layers::is_stale(mesh, layer);
    const bool values_editable = (layer_ref_.object->mode != OB_MODE_EDIT) && valid;
    row.red_alert_set(!valid);

    PointerRNA layer_ptr = RNA_pointer_create_discrete(
        &mesh.id, RNA_SculptLayer, layer_ref_.layer);

    uiItemL_ex(&row, this->label_, ICON_NONE, false, false);

    /* Influence and the eye share one right-aligned container so the eyes line up in a single
     * column at the panel edge whatever the row's name length. See the folder row for why the
     * container is unaligned and the two widgets stay in separate sub-rows. */
    ui::Layout &right = row.row(false);
    right.alignment_set(ui::LayoutAlign::Right);

    ui::Layout &sub = right.row(true);
    sub.use_property_decorate_set(false);
    if (valid) {
      sub.active_set(values_editable);
      sub.prop(&layer_ptr, "influence", ui::ITEM_R_SLIDER, "", ICON_NONE);
    }
    else {
      sub.red_alert_set(true);
      sub.prop(&layer_ptr, "is_valid", ui::ITEM_R_ICON_ONLY, "", ICON_ERROR);
    }

    ui::Layout &vis = right.row(true);
    /* Grey out the eye when a disabled folder already hides this layer: its own
     * #SCULPT_LAYER_ENABLED bit — which the icon shows — still says "visible", but #effective is
     * 0 and the layer shapes nothing. Mirrors how the Grease Pencil tree greys a layer's controls
     * by its parent group's visibility. The toggle keeps working: it edits the layer's own state,
     * which is what is restored when the folder is re-enabled. */
    vis.active_set(values_editable && !(layer.base.flag & SCULPT_LAYER_GROUP_HIDDEN));
    const int vis_icon = (layer.base.flag & SCULPT_LAYER_ENABLED) ? ICON_HIDE_OFF : ICON_HIDE_ON;
    vis.prop(&layer_ptr, "enabled", ui::ITEM_R_ICON_ONLY, "", vis_icon);
  }

  std::optional<bool> should_be_active() const override
  {
    const Mesh &mesh = *id_cast<const Mesh *>(layer_ref_.object->data);
    if (mesh.sculpt_layers_active_uid != layer_ref_.layer->base.uid) {
      return false;
    }
    /* Claim the active state only once the row is actually drawn. Claiming it while a collapsed
     * folder hides the row buys nothing — there is no row to highlight — and costs that folder its
     * collapse: #AbstractTreeViewItem::set_state_active reveals whatever becomes active through
     * #ensure_parents_uncollapsed, and #SculptLayerGroupItem::set_collapsed persists that into
     * #SCULPT_LAYER_GROUP_EXPANDED. A block built from scratch (a Properties tab switch, a file
     * load) starts every row inactive, so this fired on every rebuild and silently re-expanded the
     * folder holding the active layer.
     *
     * Unset rather than false: false states "the data says this row is not the active layer", which
     * #change_state_delayed answers by deselecting the row. The data says the opposite — this is the
     * active layer, it just cannot show it yet. It takes the state back when the folder opens, and
     * the uncollapse is a no-op by then, so the flag survives. */
    if (!row_is_revealed(layer_ref_.layer->base.parent)) {
      return std::nullopt;
    }
    return true;
  }

  void on_activate(bContext &C) override
  {
    wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_layer_select", false);
    PointerRNA props_ptr = WM_operator_properties_create_ptr(ot);
    RNA_int_set(&props_ptr, "uid", layer_ref_.layer->base.uid);
    WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::ExecDefault, &props_ptr, nullptr);
    WM_operator_properties_free(&props_ptr);
  }

  std::optional<bool> should_be_selected() const override
  {
    return layer_ref_.layer->base.flag & SCULPT_LAYER_SELECTED;
  }

  void set_selected(const bool select) override
  {
    AbstractViewItem::set_selected(select);
    SET_FLAG_FROM_TEST(layer_ref_.layer->base.flag, select, SCULPT_LAYER_SELECTED);
  }

  bool matches_single(const ui::AbstractTreeViewItem &other) const override
  {
    /* Mirrors #SculptLayerGroupItem::matches_single: row identity is by kind as well as by label, so
     * a layer row must not be matched against a folder row that momentarily shares its label during
     * a rename. */
    return dynamic_cast<const SculptLayerItem *>(&other) != nullptr &&
           AbstractTreeViewItem::matches_single(other);
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    bContext &ctx = const_cast<bContext &>(C);
    Object &object = *layer_ref_.object;
    Mesh &mesh = *id_cast<Mesh *>(object.data);

    /* Which undo system owns the rename depends on the mode. In Sculpt Mode a memfile step would
     * not compose with the delta-based stroke steps (see the #STRUCT_UNDO note on RNA_SculptLayer),
     * so the name rides along in a #Type::SculptLayer metadata step, captured before the change.
     * Outside Sculpt Mode there are no sculpt steps to interleave with — and no sculpt session for
     * #undo::push_begin_ex to read — so a global push is both safe and the only option. */
    const bool use_sculpt_undo = (object.mode & OB_MODE_SCULPT) != 0;
    if (use_sculpt_undo) {
      undo::push_begin_ex(*CTX_data_scene(&ctx), object, "Rename Sculpt Layer");
      undo::push_sculpt_layer_metadata(object, layer_ref_.layer->base);
    }

    PointerRNA layer_ptr = RNA_pointer_create_discrete(&mesh.id, RNA_SculptLayer, layer_ref_.layer);
    PropertyRNA *prop = RNA_struct_find_property(&layer_ptr, "name");
    RNA_property_string_set(&layer_ptr, prop, new_name.c_str());
    RNA_property_update(&ctx, &layer_ptr, prop);

    if (use_sculpt_undo) {
      undo::push_end(object);
    }
    else {
      ED_undo_push(&ctx, "Rename Sculpt Layer");
    }
    return true;
  }

  StringRef get_rename_string() const override
  {
    return label_;
  }

  void delete_item(bContext *C) override
  {
    Mesh &mesh = *id_cast<Mesh *>(layer_ref_.object->data);
    /* #X deletion fires this once per active-or-selected item (#view_item_delete_invoke), whereas
     * #SCULPT_OT_layer_remove removes that same set in one go. The first call therefore already
     * frees the layers behind the remaining items, so resolve by uid instead of dereferencing the
     * stored (by then dangling) pointer, and let only the first surviving item run the operator.
     * Kind-checked, because one uid space now spans folders too and reading a folder as a layer
     * would send #SculptLayer::data past the end of the smaller allocation. */
    if (bke::sculpt_layers::node_as_layer(bke::sculpt_layers::node_find_by_uid(mesh, uid_)) ==
        nullptr)
    {
      return;
    }
    WM_operator_name_call(
        C, "SCULPT_OT_layer_remove", wm::OpCallContext::ExecDefault, nullptr, nullptr);
  }

  void build_context_menu(bContext &C, ui::Layout &layout) const override
  {
    MenuType *mt = WM_menutype_find("MESH_MT_sculpt_layer_context_menu", true);
    if (!mt) {
      return;
    }
    ui::menutype_draw(&C, mt, &layout);
  }

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override
  {
    Mesh *mesh = id_cast<Mesh *>(layer_ref_.object->data);
    return std::make_unique<SculptLayerDragController>(
        static_cast<SculptLayerTreeView &>(get_tree_view()), mesh);
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    Mesh *mesh = id_cast<Mesh *>(layer_ref_.object->data);
    /* #DropBehavior::Reorder, not ReorderAndInsert: nothing can go inside a layer. */
    return std::make_unique<SculptLayerDropTarget>(
        *this, ui::DropBehavior::Reorder, &layer_ref_.layer->base, mesh);
  }
};

void SculptLayerTreeView::build_tree_recursive(ui::TreeViewOrItem &parent,
                                               const SculptLayerGroup &group)
{
  /* One walk over the folder's own children, in list order. That list holds both kinds interleaved
   * and *is* the sibling order, so the rows come out exactly as the tree stores them — no filtering
   * by a parent tag, and no ordering rule of this view's own invention. */
  for (SculptLayerTreeNode &node : group.children) {
    switch (node.type) {
      case SCULPT_LAYER_TREE_NODE_TYPE_GROUP: {
        SculptLayerGroup &child = *bke::sculpt_layers::node_as_group(&node);
        SculptLayerGroupItem &item = parent.add_tree_item<SculptLayerGroupItem>(&object_, &child);
        this->build_tree_recursive(item, child);
        break;
      }
      case SCULPT_LAYER_TREE_NODE_TYPE_LAYER: {
        parent.add_tree_item<SculptLayerItem>(&object_, bke::sculpt_layers::node_as_layer(&node));
        break;
      }
    }
  }
}

void SculptLayerTreeView::build_tree()
{
  Mesh *mesh = id_cast<Mesh *>(object_.data);
  if (mesh == nullptr) {
    return;
  }
  /* From the root folder's children: the root always exists but is never drawn as a row, so the
   * top-level rows are its children rather than the tree's own. */
  this->build_tree_recursive(*this, *bke::sculpt_layers::root_group(*mesh));
}

void template_layer_tree(ui::Layout *layout, bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return;
  }

  ui::Block *block = layout->block();

  ui::AbstractTreeView *tree_view = block_add_view(
      *block, "Sculpt Layer Tree View", std::make_unique<SculptLayerTreeView>(*ob));
  tree_view->set_context_menu_title("Sculpt Layer");
  tree_view->set_default_rows(5);
  tree_view->allow_multiselect_items();

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}

}  // namespace blender::ed::sculpt_paint::layers
