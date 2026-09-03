/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Code to manage views as part of the regular screen hierarchy. E.g. managing ownership of views
 * inside blocks (#Block.views), looking up items in the region, passing WM notifiers to views,
 * etc.
 *
 * Blocks and their contained views are reconstructed on every redraw. This file also contains
 * functions related to this recreation of views inside blocks. For example to query state
 * information before the view is done reconstructing (#AbstractView.is_reconstructed() returns
 * false), it may be enough to query the previous version of the block/view/view-item. Since such
 * queries rely on the details of the UI reconstruction process, they should remain internal to
 * `interface/` code.
 */

#include <memory>

#include "DNA_screen_types.h"

#include "BKE_screen.hh"

#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_rect.h"

#include "ED_screen.hh"

#include "interface_intern.hh"

#include "UI_abstract_view.hh"
#include "UI_grid_view.hh"
#include "UI_tree_view.hh"
#include "WM_api.hh"

namespace blender::ui {
#define TREE_VIEW_DRAG_SCROLL_SPEED 0.1

/**
 * Wrapper to store views in a #ListBase, addressable via an identifier.
 */
struct ViewLink : public Link {
  std::string idname;
  std::unique_ptr<AbstractView> view;

  static void views_bounds_calc(const Block &block);
};

template<class T>
static T *block_add_view_impl(Block &block, StringRef idname, std::unique_ptr<AbstractView> view)
{
  BLI_assert(idname.size() < int64_t(sizeof(uiViewStateLink::idname)));

  ViewLink *view_link = MEM_new<ViewLink>(__func__);
  BLI_addtail(&block.views, view_link);

  view_link->view = std::move(view);
  view_link->idname = idname;

  return dynamic_cast<T *>(view_link->view.get());
}

AbstractGridView *block_add_view(Block &block,
                                 StringRef idname,
                                 std::unique_ptr<AbstractGridView> grid_view)
{
  return block_add_view_impl<AbstractGridView>(block, idname, std::move(grid_view));
}

AbstractTreeView *block_add_view(Block &block,
                                 StringRef idname,
                                 std::unique_ptr<AbstractTreeView> tree_view)
{
  return block_add_view_impl<AbstractTreeView>(block, idname, std::move(tree_view));
}

void block_free_views(Block *block)
{
  for (ViewLink &link : block->views.items_mutable()) {
    MEM_delete(&link);
  }
}

AbstractGridView *block_view_find_fixed_viewport_grid(Block &block)
{
  for (ViewLink &link : block.views) {
    if (auto *grid_view = dynamic_cast<AbstractGridView *>(link.view.get())) {
      if (grid_view->use_fixed_viewport_layout()) {
        return grid_view;
      }
    }
  }
  return nullptr;
}

Vector<AbstractGridView *> block_view_scroll_clipped_grids(Block &block)
{
  Vector<AbstractGridView *> result;
  for (ViewLink &link : block.views) {
    auto *grid_view = dynamic_cast<AbstractGridView *>(link.view.get());
    if (grid_view && grid_view->scroll_clip_enabled()) {
      result.append(grid_view);
    }
  }
  return result;
}

AbstractGridView *block_view_find_fixed_viewport_grid_at_y(Block &block, const float block_space_y)
{
  AbstractGridView *fallback = nullptr;
  for (ViewLink &link : block.views) {
    auto *grid_view = dynamic_cast<AbstractGridView *>(link.view.get());
    if (!grid_view || !grid_view->use_fixed_viewport_layout()) {
      continue;
    }
    if (!fallback) {
      /* Preserve today's single-grid behavior exactly when nothing below matches (e.g. the
       * pointer is outside every grid's edge-scroll band): pick the first one, as
       * #block_view_find_fixed_viewport_grid always did. */
      fallback = grid_view;
    }
    if (grid_view->fixed_viewport_scroll_at_y(block, block_space_y).has_value()) {
      return grid_view;
    }
  }
  return fallback;
}

void ViewLink::views_bounds_calc(const Block &block)
{
  Map<AbstractView *, rcti> views_bounds;

  rcti minmax;
  BLI_rcti_init_minmax(&minmax);
  for (ViewLink &link : block.views) {
    views_bounds.add(link.view.get(), minmax);
  }

  for (Button &but : block.buttons()) {
    if (but.type != ButtonType::ViewItem) {
      continue;
    }
    auto *view_item_but = static_cast<ButtonViewItem *>(&but);
    if (!view_item_but->view_item) {
      continue;
    }

    /* Get the view from the button. */
    AbstractViewItem &view_item = *view_item_but->view_item;
    AbstractView &view = view_item.get_view();

    /* Clamp grid-scroll-clipped tiles to the visible clip window: the buffer/partially-scrolled
     * rows are masked away when drawn, so they must not extend the view bounds either (these
     * bounds drive the scroll arrows, wheel hover checks and edge auto-scroll zones). */
    rctf but_bounds_rect;
    if (!block_grid_scroll_clip_bounds_rect(view_item_but, &but_bounds_rect)) {
      continue;
    }

    rcti &bounds = views_bounds.lookup(&view);
    rcti but_rcti{};
    BLI_rcti_rctf_copy_round(&but_rcti, &but_bounds_rect);
    BLI_rcti_do_minmax_rcti(&bounds, &but_rcti);
  }

  /* A grid with a scroll-clip window exists independently of the tiles built this frame: during
   * a fast-scroll rebuild few or zero tiles may be built, which would otherwise leave empty (or
   * shrunken) bounds for a frame and leak wheel events to the region's own #View2D pan (the
   * cascade the wheel latches used to paper over). The clip window is the grid's own viewport, so
   * it is the stable bounds source — each grid unions only its own clip rect. */
  for (ViewLink &link : block.views) {
    auto *grid_view = dynamic_cast<AbstractGridView *>(link.view.get());
    if (!grid_view || !grid_view->scroll_clip_enabled()) {
      continue;
    }
    rcti clip_rect{};
    BLI_rcti_rctf_copy_round(&clip_rect, &grid_view->scroll_clip_rect());
    rcti &bounds = views_bounds.lookup(link.view.get());
    BLI_rcti_do_minmax_rcti(&bounds, &clip_rect);
  }

  for (const auto item : views_bounds.items()) {
    const rcti &bounds = item.value;
    if (BLI_rcti_is_empty(&bounds)) {
      continue;
    }

    AbstractView &view = *item.key;
    view.bounds_ = bounds;
  }
}

void block_view_persistent_state_restore(const ARegion &region,
                                         const Block &block,
                                         AbstractView &view)
{
  StringRef idname = [&]() -> StringRef {
    for (ViewLink &link : block.views) {
      if (link.view.get() == &view) {
        return link.idname;
      }
    }
    return "";
  }();

  if (idname.is_empty()) {
    BLI_assert_unreachable();
    return;
  }

  for (uiViewStateLink &stored_state : region.view_states) {
    if (stored_state.idname == idname) {
      view.persistent_state_apply(stored_state.state);
    }
  }
}

static uiViewStateLink *ensure_view_state(ARegion &region, const ViewLink &link)
{
  for (uiViewStateLink &stored_state : region.view_states) {
    if (link.idname == stored_state.idname) {
      return &stored_state;
    }
  }

  uiViewStateLink *new_state = MEM_new<uiViewStateLink>(__func__);
  link.idname.copy(new_state->idname, sizeof(new_state->idname));
  BLI_addhead(&region.view_states, new_state);
  return new_state;
}

void block_views_end(ARegion *region, const Block *block)
{
  ViewLink::views_bounds_calc(*block);

  if (region && region->regiontype != RGN_TYPE_TEMPORARY) {
    for (const ViewLink &link : block->views) {
      /* Ensure persistent view state storage for writing to files if needed. */
      if (std::optional<uiViewState> temp_state = link.view->persistent_state()) {
        uiViewStateLink *state_link = ensure_view_state(*region, link);
        state_link->state = *temp_state;
      }
    }
  }
}

void block_views_listen(const Block *block, const wmRegionListenerParams *listener_params)
{
  ARegion *region = listener_params->region;

  for (ViewLink &view_link : block->views) {
    if (view_link.view->listen(*listener_params->notifier)) {
      ED_region_tag_redraw(region);
    }
  }
}

void block_views_draw_overlays(const ARegion *region, const Block *block)
{
  for (ViewLink &view_link : block->views) {
    view_link.view->draw_overlays(*region, *block);
  }
}

AbstractView *region_view_find_at(const ARegion *region,
                                  const int xy[2],
                                  const int pad,
                                  Block **r_block)
{
  /* NOTE: Similar to #but_find_mouse_over_ex(). */

  if (!region_contains_point_px(region, xy)) {
    return nullptr;
  }
  for (Block &block : region->runtime->uiblocks) {
    float mx = xy[0], my = xy[1];
    window_to_block_fl(region, &block, &mx, &my);

    for (ViewLink &view_link : block.views) {
      std::optional<rcti> bounds = view_link.view->get_bounds();
      if (!bounds) {
        continue;
      }

      rcti padded_bounds = *bounds;
      if (pad) {
        BLI_rcti_pad(&padded_bounds, pad, pad);
      }
      if (BLI_rcti_isect_pt(&padded_bounds, mx, my)) {
        if (r_block != nullptr) {
          *r_block = &block;
        }
        return view_link.view.get();
      }
    }
  }

  return nullptr;
}

static StringRef block_view_find_idname(const Block &block, const AbstractView &view)
{
  /* First get the `idname` of the view we're looking for. */
  for (ViewLink &view_link : block.views) {
    if (view_link.view.get() == &view) {
      return view_link.idname;
    }
  }

  return {};
}

bool region_view_has_idname_at(const ARegion *region,
                               const int xy[2],
                               const int pad,
                               const StringRef idname)
{
  Block *block = nullptr;
  AbstractView *view = region_view_find_at(region, xy, pad, &block);
  if (!view || !block) {
    return false;
  }
  return block_view_find_idname(*block, *view) == idname;
}

bool region_view_item_has_idname_at(const ARegion *region, const int xy[2], const StringRef idname)
{
  auto *item_but = static_cast<ButtonViewItem *>(view_item_find_mouse_over(region, xy));
  if (!item_but || !item_but->view_item) {
    return false;
  }
  return block_view_find_idname(*item_but->block, item_but->view_item->get_view()) == idname;
}

bool region_view_item_topmost_at(const ARegion *region,
                                 const wmEvent *event,
                                 const StringRef idname)
{
  /* #but_find_mouse_over is the same top-most hit test the window manager uses to route a press,
   * so a button drawn over the tile (the overlay scrollbar) or below the grid (the resize grip) is
   * returned instead of the tile behind it. #region_view_item_has_idname_at cannot be used here:
   * it searches view-item buttons only and so reports the tile even when another widget covers it.
   */
  const Button *but = but_find_mouse_over(region, event);
  if (!but || but->type != ButtonType::ViewItem) {
    return false;
  }
  const auto *item_but = static_cast<const ButtonViewItem *>(but);
  if (!item_but->view_item) {
    return false;
  }
  return block_view_find_idname(*item_but->block, item_but->view_item->get_view()) == idname;
}

StringRef region_view_item_topmost_idname_at(const ARegion *region,
                                             const wmEvent *event,
                                             AbstractView **r_view)
{
  const Button *but = but_find_mouse_over(region, event);
  if (!but || but->type != ButtonType::ViewItem) {
    return {};
  }
  const auto *item_but = static_cast<const ButtonViewItem *>(but);
  if (!item_but->view_item) {
    return {};
  }
  AbstractView &view = item_but->view_item->get_view();
  if (r_view != nullptr) {
    *r_view = &view;
  }
  return block_view_find_idname(*item_but->block, view);
}

static void region_view_scroll_at_borders_apply(ARegion *region,
                                              AbstractView &view,
                                              const ViewScrollDirection scroll_dir)
{
  if (auto *grid_view = dynamic_cast<AbstractGridView *>(&view)) {
    if (grid_view->use_fixed_viewport_layout()) {
      /* Fixed-viewport scroll is a row index in the grid view; the refresh rebuilds the visible
       * rows from it. */
      grid_view->scroll(scroll_dir);
      ED_region_tag_refresh_ui(region);
      return;
    }
  }
  /* Non-fixed-viewport grid views (e.g. the sidebar brush texture grid, which scrolls via its own
   * session-based View2D offset instead) do not implement the generic #AbstractView::scroll() -
   * calling it would hit its "Unsupported for this view type" assert. Only views that actually
   * advertise support get the generic scroll fallback. */
  if (!view.supports_scrolling()) {
    return;
  }
  view.scroll(scroll_dir);
  ED_region_tag_redraw(region);
}

void region_view_scroll_at_borders(bContext *C, wmDropBox &dropbox, const wmEvent *event)
{
  Block *block = nullptr;
  ARegion *region = CTX_wm_region_popup(C);
  if (region == nullptr) {
    region = CTX_wm_region(C);
  }
  wmWindow *window = CTX_wm_window(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!ELEM(event->type, MOUSEMOVE, TIMER) || region == nullptr) {
    return;
  }

  AbstractView *view = region_view_find_at(region, event->xy, UI_UNIT_Y, &block);
  if (view == nullptr) {
    /* Fixed-viewport grid views (e.g. the image browser popover) lay out only their visible rows,
     * so #region_view_find_at may miss them when the cursor is in the scroll-edge band. Fall back
     * to the block's fixed-viewport grid so drag-auto-scroll still works there. */
    block = static_cast<Block *>(region->runtime->uiblocks.first);
    if (block) {
      float bx = float(event->xy[0]), by = float(event->xy[1]);
      window_to_block_fl(region, block, &bx, &by);
      view = block_view_find_fixed_viewport_grid_at_y(*block, by);
    }
  }

  if (view == nullptr || block == nullptr) {
    WM_event_timer_remove(wm, window, dropbox.timer);
    dropbox.timer = nullptr;
    return;
  }

  float x = event->xy[0], y = event->xy[1];
  window_to_block_fl(region, block, &x, &y);

  const float margin = UI_UNIT_Y * 1 / 3;
  const std::optional<ViewScrollDirection> scroll_dir =
      [&]() -> std::optional<ViewScrollDirection> {
    if (const auto *grid_view = dynamic_cast<const AbstractGridView *>(view)) {
      if (grid_view->use_fixed_viewport_layout()) {
        return grid_view->fixed_viewport_scroll_at_y(*block, y);
      }
    }

    const std::optional<rcti> bounds = view->get_bounds();
    if (!bounds.has_value()) {
      return std::nullopt;
    }
    if (y > bounds->ymax - margin) {
      return ViewScrollDirection::UP;
    }
    if (y < bounds->ymin + margin) {
      return ViewScrollDirection::DOWN;
    }
    return std::nullopt;
  }();

  if (!scroll_dir.has_value()) {
    WM_event_timer_remove(wm, window, dropbox.timer);
    dropbox.timer = nullptr;
    return;
  }

  if (dropbox.timer) {
    if (event->type == TIMER) {
      region_view_scroll_at_borders_apply(region, *view, *scroll_dir);
    }
  }
  else {
    dropbox.timer = WM_event_timer_add(wm, window, TIMER, TREE_VIEW_DRAG_SCROLL_SPEED);
  }
}

AbstractViewItem *region_views_find_item_at(const ARegion &region, const int xy[2])
{
  auto *item_but = static_cast<ButtonViewItem *>(view_item_find_mouse_over(&region, xy));
  if (!item_but) {
    return nullptr;
  }

  return item_but->view_item;
}

AbstractViewItem *region_views_find_active_item(const ARegion *region, const AbstractView *view)
{
  auto *item_but = static_cast<ButtonViewItem *>(view_item_find_active(region, view));
  if (!item_but) {
    return nullptr;
  }

  return item_but->view_item;
}

Button *region_views_find_active_item_but(const ARegion *region)
{
  return view_item_find_active(region);
}

void region_views_clear_search_highlight(const ARegion *region)
{
  for (Block &block : region->runtime->uiblocks) {
    for (ViewLink &view_link : block.views) {
      view_link.view->clear_search_highlight();
    }
  }
}

std::unique_ptr<DropTargetInterface> region_views_find_drop_target_at(const ARegion *region,
                                                                      const int xy[2])
{
  if (AbstractViewItem *item = region_views_find_item_at(*region, xy)) {
    if (std::unique_ptr<DropTargetInterface> target = item->create_item_drop_target()) {
      return target;
    }
  }

  /* Get style for some sensible padding around the view items. */
  const uiStyle *style = style_get_dpi();
  if (AbstractView *view = region_view_find_at(region, xy, style->buttonspacex)) {
    if (std::unique_ptr<DropTargetInterface> target = view->create_drop_target()) {
      return target;
    }
  }

  /* To continue scroll during drag when mouse is slightly outside the view, find the view with
   * extra padding (UI_UNIT_Y). */
  if (AbstractView *view = region_view_find_at(region, xy, UI_UNIT_Y)) {
    /* If we are above a tree, but not hovering any specific element, dropping something should
     * insert it before first or after last visible item depends on the mouse position. */
    if (AbstractTreeView *tree_view = dynamic_cast<AbstractTreeView *>(view)) {
      /* Find the first or last item which we want to drop below. */
      AbstractTreeViewItem *first_or_last_visible = nullptr;
      tree_view->foreach_root_item([&](AbstractTreeViewItem &item) {
        if (!item.is_interactive()) {
          return;
        }
        std::optional<rctf> rct = item.get_win_rect(*region);
        if (rct.has_value()) {
          if ((!first_or_last_visible && (xy[1] > rct->ymax)) || (xy[1] < rct->ymin)) {
            first_or_last_visible = &item;
          }
        }
      });
      if (first_or_last_visible) {
        return first_or_last_visible->create_item_drop_target();
      }
    }
  }

  return nullptr;
}

template<class T>
static T *block_view_find_matching_in_old_block_impl(const Block &new_block, const T &new_view)
{
  Block *old_block = new_block.oldblock;
  if (!old_block) {
    return nullptr;
  }

  StringRef idname = block_view_find_idname(new_block, new_view);
  if (idname.is_empty()) {
    return nullptr;
  }

  for (ViewLink &old_view_link : old_block->views) {
    if (old_view_link.idname == idname) {
      return dynamic_cast<T *>(old_view_link.view.get());
    }
  }

  return nullptr;
}

AbstractView *block_view_find_matching_in_old_block(const Block &new_block,
                                                    const AbstractView &new_view)
{
  return block_view_find_matching_in_old_block_impl(new_block, new_view);
}

ButtonViewItem *block_view_find_matching_view_item_but_in_old_block(
    const Block &new_block, const AbstractViewItem &new_item)
{
  Block *old_block = new_block.oldblock;
  if (!old_block) {
    return nullptr;
  }

  const AbstractView *old_view = block_view_find_matching_in_old_block_impl(new_block,
                                                                            new_item.get_view());
  if (!old_view) {
    return nullptr;
  }

  for (Button &old_but : old_block->buttons()) {
    if (old_but.type != ButtonType::ViewItem) {
      continue;
    }
    ButtonViewItem *old_item_but = static_cast<ButtonViewItem *>(&old_but);
    if (!old_item_but->view_item) {
      continue;
    }
    AbstractViewItem &old_item = *old_item_but->view_item;
    /* Check if the item is from the expected view. */
    if (&old_item.get_view() != old_view) {
      continue;
    }

    if (view_item_matches(new_item, old_item)) {
      return old_item_but;
    }
  }

  return nullptr;
}

}  // namespace blender::ui
