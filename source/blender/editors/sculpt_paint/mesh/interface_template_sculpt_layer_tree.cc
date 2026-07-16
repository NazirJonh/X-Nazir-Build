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

#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::layers {

class SculptLayerTreeView : public ui::AbstractTreeView {
 protected:
  Object &object_;

 public:
  explicit SculptLayerTreeView(Object &ob) : object_(ob)
  {
    is_flat_ = true;
  }

  void build_tree() override;
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
    int selected_count = 0;
    for (const SculptLayer &layer : mesh_->sculpt_layers) {
      selected_count += (layer.flag & SCULPT_LAYER_SELECTED) != 0;
    }

    /* Allocate one extra element, to use it as null-delimiter. */
    SculptLayer **selected_layers = MEM_new_array_zeroed<SculptLayer *>(selected_count + 1,
                                                                        "Selected Sculpt Layers");
    int i = 0;
    for (SculptLayer &layer : mesh_->sculpt_layers) {
      if (layer.flag & SCULPT_LAYER_SELECTED) {
        selected_layers[i] = &layer;
        i++;
      }
    }
    BLI_assert_msg(selected_layers[i] == nullptr,
                   "Expected last element to be null (null-delimiter)");
    return selected_layers;
  }
};

class SculptLayerDropTarget : public ui::TreeViewItemDropTarget {
 private:
  SculptLayer &drop_layer_;

 public:
  SculptLayerDropTarget(ui::AbstractTreeViewItem &item,
                       ui::DropBehavior behavior,
                       SculptLayer &drop_layer)
      : TreeViewItemDropTarget(item, behavior), drop_layer_(drop_layer)
  {
  }

  bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    if (drag.type != WM_DRAG_SCULPT_LAYER) {
      return false;
    }
    const SculptLayer **drag_layers = static_cast<const SculptLayer **>(drag.poin);
    return drag_layers && drag_layers[0];
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const StringRef drag_name = TIP_("Selected Layers");
    const StringRef drop_name = drop_layer_.name;

    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        BLI_assert_unreachable();
        break;
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
    RNA_int_set(&props_ptr, "anchor_uid", drop_layer_.uid);
    RNA_boolean_set(&props_ptr, "after", drag_info.drop_location == ui::DropLocation::After);
    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::ExecDefault, &props_ptr, nullptr);
    WM_operator_properties_free(&props_ptr);
    return true;
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
    label_ = layer->name;
    layer_ref_.object = object;
    layer_ref_.layer = layer;
    uid_ = layer->uid;
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

    ui::Layout &vis = row.row(true);
    vis.active_set(values_editable);
    const int vis_icon = (layer.flag & SCULPT_LAYER_ENABLED) ? ICON_HIDE_OFF : ICON_HIDE_ON;
    vis.prop(&layer_ptr, "enabled", ui::ITEM_R_ICON_ONLY, "", vis_icon);

    uiItemL_ex(&row, this->label_, ICON_NONE, false, false);

    ui::Layout &sub = row.row(true);
    sub.alignment_set(ui::LayoutAlign::Right);
    sub.use_property_decorate_set(false);
    if (valid) {
      sub.active_set(values_editable);
      sub.prop(&layer_ptr, "influence", ui::ITEM_R_SLIDER, "", ICON_NONE);
    }
    else {
      sub.red_alert_set(true);
      sub.prop(&layer_ptr, "is_valid", ui::ITEM_R_ICON_ONLY, "", ICON_ERROR);
    }
  }

  std::optional<bool> should_be_active() const override
  {
    const Mesh &mesh = *id_cast<const Mesh *>(layer_ref_.object->data);
    return mesh.sculpt_layers_active_uid == layer_ref_.layer->uid;
  }

  void on_activate(bContext &C) override
  {
    wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_layer_select", false);
    PointerRNA props_ptr = WM_operator_properties_create_ptr(ot);
    RNA_int_set(&props_ptr, "uid", layer_ref_.layer->uid);
    WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::ExecDefault, &props_ptr, nullptr);
    WM_operator_properties_free(&props_ptr);
  }

  std::optional<bool> should_be_selected() const override
  {
    return layer_ref_.layer->flag & SCULPT_LAYER_SELECTED;
  }

  void set_selected(const bool select) override
  {
    AbstractViewItem::set_selected(select);
    SET_FLAG_FROM_TEST(layer_ref_.layer->flag, select, SCULPT_LAYER_SELECTED);
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
      undo::push_sculpt_layer_metadata(object, *layer_ref_.layer);
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
     * stored (by then dangling) pointer, and let only the first surviving item run the operator. */
    if (bke::sculpt_layers::find_by_uid(mesh, uid_) == nullptr) {
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
    return std::make_unique<SculptLayerDropTarget>(
        *this, ui::DropBehavior::Reorder, *layer_ref_.layer);
  }
};

void SculptLayerTreeView::build_tree()
{
  Mesh *mesh = id_cast<Mesh *>(object_.data);
  if (mesh == nullptr) {
    return;
  }
  for (SculptLayer &layer : mesh->sculpt_layers) {
    this->add_tree_item<SculptLayerItem>(&object_, &layer);
  }
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
