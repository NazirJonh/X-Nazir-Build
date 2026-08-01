/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Generic Shift+drag reorder drop target, shared by the asset shelf's Favorites list and any
 * template_grid_view_custom / UIGrid grid that configures a reorder operator.
 */

#include "AS_asset_representation.hh"

#include "BKE_context.hh"

#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_asset_menu_utils.hh"

#include <fmt/format.h>

namespace blender::ui {

GridItemReorderDropTarget::GridItemReorderDropTarget(AbstractGridView &view,
                                                     AbstractGridViewItem &drop_item,
                                                     const StringRef reorder_operator,
                                                     const StringRef activate_operator,
                                                     const StringRef isolation_key)
    : GridViewItemDropTarget(view),
      drop_item_(drop_item),
      reorder_operator_(reorder_operator),
      activate_operator_(activate_operator),
      isolation_key_(isolation_key)
{
}

static bool isolation_key_matches(const wmDrag &drag, const StringRef expected_key)
{
  if (drag.type == WM_DRAG_GRID_ITEM_REORDER_ASSET) {
    const wmDragGridItemReorderAsset *data = WM_drag_get_grid_item_reorder_asset_data(&drag);
    return data && data->shelf_idname == expected_key;
  }
  if (drag.type == WM_DRAG_GRID_ITEM_REORDER_PY) {
    const wmDragGridItemReorderPy *data = WM_drag_get_grid_item_reorder_py_data(&drag);
    return data && data->grid_id == expected_key;
  }
  return false;
}

/** True if \a drag's source is this exact item (a no-op drop). */
static bool is_self_drop(const wmDrag &drag, const AbstractGridViewItem &drop_item)
{
  if (drag.type == WM_DRAG_GRID_ITEM_REORDER_PY) {
    const wmDragGridItemReorderPy *data = WM_drag_get_grid_item_reorder_py_data(&drag);
    return data && data->source_identifier == drop_item.identifier();
  }
  if (drag.type == WM_DRAG_GRID_ITEM_REORDER_ASSET) {
    /* Favorites tiles use library_relative_identifier() as the item identifier — the same string
     * stored on AssetWeakReference::relative_asset_identifier. Compare those (null-safe) so
     * can_drop rejects self-hover instead of only no-opping in the operator. */
    const wmDragGridItemReorderAsset *data = WM_drag_get_grid_item_reorder_asset_data(&drag);
    if (!data || !data->source.relative_asset_identifier) {
      return false;
    }
    return drop_item.identifier() == data->source.relative_asset_identifier;
  }
  return false;
}

bool GridItemReorderDropTarget::can_drop(bContext &C,
                                         const wmDrag &drag,
                                         const char **r_disabled_hint) const
{
  if (!ELEM(drag.type, WM_DRAG_GRID_ITEM_REORDER_ASSET, WM_DRAG_GRID_ITEM_REORDER_PY)) {
    return false;
  }
  if (!isolation_key_matches(drag, isolation_key_)) {
    return false;
  }
  if (is_self_drop(drag, drop_item_)) {
    *r_disabled_hint = RPT_("Cannot move item to itself");
    return false;
  }

  wmOperatorType *ot = WM_operatortype_find(reorder_operator_.c_str(), true);
  if (!ot) {
    return false;
  }
  if (WM_operator_poll(&C, ot)) {
    return true;
  }

  bool free_msg = false;
  const char *msg = CTX_wm_operator_poll_msg_get(&C, &free_msg);
  if (msg) {
    disabled_hint_cache_ = msg;
    if (free_msg) {
      MEM_SAFE_DELETE(msg);
    }
    *r_disabled_hint = disabled_hint_cache_.c_str();
  }
  return false;
}

std::optional<DropLocation> GridItemReorderDropTarget::choose_drop_location(
    const ARegion &region, const wmEvent &event) const
{
  const std::optional<rctf> win_rect = drop_item_.win_rect_in_region(region);
  if (!win_rect) {
    return std::nullopt;
  }

  /* Grid tiles flow left-to-right within a row (wrapping to multiple columns), unlike a
   * vertically stacked list, so the natural before/after split is horizontal: which half of the
   * hovered tile the cursor is over. Same geometry FavoriteAssetDropTarget used. */
  if (event.xy[0] < win_rect->xmin) {
    return DropLocation::Before;
  }
  const float item_width = BLI_rctf_size_x(&*win_rect);
  if (event.xy[0] - win_rect->xmin > item_width / 2.0f) {
    return DropLocation::After;
  }
  return DropLocation::Before;
}

std::string GridItemReorderDropTarget::drop_tooltip(const DragInfo &drag_info) const
{
  /* Prefer the drawn label (PreviewGridItem::label — AssetViewItem / PyGridItem both inherit it)
   * so Favorites tooltips show the asset name, not the library-relative identifier path. Fall
   * back to identifier() for any non-preview grid item that wires this drop target. */
  StringRef item_name = drop_item_.identifier();
  if (const PreviewGridItem *preview = dynamic_cast<const PreviewGridItem *>(&drop_item_)) {
    item_name = preview->label;
  }

  switch (drag_info.drop_location) {
    case DropLocation::Before:
      return fmt::format(fmt::runtime(TIP_("Move before {}")), item_name);
    case DropLocation::After:
      return fmt::format(fmt::runtime(TIP_("Move after {}")), item_name);
    case DropLocation::Into:
      BLI_assert_unreachable();
      break;
  }
  return "";
}

void GridItemReorderDropTarget::drop_linehint(ARegion &region, const DragInfo &drag_info) const
{
  view_.set_drop_linehint(region, drop_item_, drag_info.drop_location);
}

bool GridItemReorderDropTarget::on_drop(bContext *C, const DragInfo &drag_info) const
{
  view_.clear_drop_linehint();

  wmOperatorType *ot = WM_operatortype_find(reorder_operator_.c_str(), true);
  if (!ot) {
    return false;
  }

  const wmDrag &drag = drag_info.drag_data;
  const wmDragGridItemReorderAsset *asset_data = nullptr;
  const wmDragGridItemReorderPy *py_data = nullptr;
  const asset_system::AssetRepresentation *asset = nullptr;

  if (drag.type == WM_DRAG_GRID_ITEM_REORDER_ASSET) {
    asset_data = WM_drag_get_grid_item_reorder_asset_data(&drag);
    if (!asset_data) {
      return false;
    }
    asset = ed::asset::find_asset_from_weak_ref(*C, asset_data->source, nullptr);
    if (!asset) {
      return false;
    }
  }
  else if (drag.type == WM_DRAG_GRID_ITEM_REORDER_PY) {
    py_data = WM_drag_get_grid_item_reorder_py_data(&drag);
    if (!py_data) {
      return false;
    }
  }
  else {
    return false;
  }

  PointerRNA *op_props = MEM_new<PointerRNA>(__func__, WM_operator_properties_create_ptr(ot));
  RNA_string_set(op_props, "target_identifier", drop_item_.identifier().data());
  RNA_enum_set_identifier(C,
                          op_props,
                          "drop_location",
                          drag_info.drop_location == DropLocation::Before ? "BEFORE" : "AFTER");
  if (asset) {
    ed::asset::operator_asset_reference_props_set(*asset, *op_props);
  }
  else {
    RNA_string_set(op_props, "source_identifier", py_data->source_identifier.c_str());
  }

  /* #wm::OpCallContext::InvokeDefault, not InvokeRegionWin: RegionWin forces the call's region to
   * RGN_TYPE_WINDOW when the current region isn't already that type (see
   * #wm_operator_call_internal), which re-polls the operator with #CTX_wm_region() pointed at the
   * viewport window region instead of this drop target's own region. The permanent asset shelf's
   * "asset_shelf" context member only resolves through the asset-shelf region's own #context()
   * callback (#asset::shelf::context) -- there is no region-independent fallback like the popover's
   * context_ptr_set() store -- so the redirect silently breaks #asset_favorite_reorder_to_poll()
   * there, and the drop is rejected without ever reaching exec(). Keeping the region already set by
   * the drop dispatch (which is this drop target's own region) keeps that poll working everywhere. */
  const wmOperatorStatus reorder_status = WM_operator_name_call_ptr(
      C, ot, wm::OpCallContext::InvokeDefault, op_props, nullptr);
  const bool ok = reorder_status == OPERATOR_FINISHED;

  /* Activate uses a separate property bag with only the domain's source props (design decision 7 /
   * FavoriteAssetDropTarget precedent) -- never reuse reorder's target_identifier / drop_location. */
  if (ok && !activate_operator_.empty()) {
    if (wmOperatorType *activate_ot = WM_operatortype_find(activate_operator_.c_str(), true)) {
      PointerRNA *activate_props = MEM_new<PointerRNA>(
          __func__, WM_operator_properties_create_ptr(activate_ot));
      if (asset) {
        ed::asset::operator_asset_reference_props_set(*asset, *activate_props);
      }
      else {
        RNA_string_set(activate_props, "identifier", py_data->source_identifier.c_str());
      }
      /* InvokeDefault for the same reason as the reorder call above. */
      WM_operator_name_call_ptr(
          C, activate_ot, wm::OpCallContext::InvokeDefault, activate_props, nullptr);
      WM_operator_properties_free(activate_props);
      MEM_delete(activate_props);
    }
  }

  WM_operator_properties_free(op_props);
  MEM_delete(op_props);
  return ok;
}

}  // namespace blender::ui
