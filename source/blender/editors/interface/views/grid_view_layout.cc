/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Item layout for #AbstractGridView: visible-row virtualization, tile grid,
 * and #GridViewBuilder. View types live in grid_view.cc; input in
 * grid_view_input.cc.
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "DNA_screen_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_grid_view.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "interface_grid_view.hh"
#include "interface_intern.hh"
#include "views/grid_view_intern.hh"

namespace blender::ui {

/* ---------------------------------------------------------------------- */

/**
 * Helper for only adding layout items for grid items that are actually in view. 3 main functions:
 * - #is_item_visible(): Query if an item of a given index is visible in the view (others should be
 * skipped when building the layout).
 * - #fill_layout_before_visible(): Add empty space to the layout before a visible row is drawn, so
 *   the layout height is the same as if all items were added (important to get the correct scroll
 *   height).
 * - #fill_layout_after_visible(): Same thing, just adds empty space for after the last visible
 *   row.
 *
 * Does two assumptions:
 * - Top-to-bottom flow (ymax = 0 and ymin < 0). If that's not good enough, View2D should
 *   probably provide queries for the scroll offset.
 * - Only vertical scrolling. For horizontal scrolling, spacers would have to be added on the
 *   side(s) as well.
 */
class BuildOnlyVisibleButtonsHelper {
  const AbstractGridView &grid_view_;
  const GridViewStyle &style_;
  const int cols_per_row_ = 0;
  /** Local #View2D from embedded templates (e.g. sculpt image grid). */
  const bool embedded_v2d_ = false;
  /* Indices of items within the view. Calculated by constructor. If this is unset it means all
   * items/buttons should be drawn. */
  std::optional<IndexRange> visible_items_range_;

 public:
  BuildOnlyVisibleButtonsHelper(const View2D &v2d,
                                const AbstractGridView &grid_view,
                                int cols_per_row,
                                const AbstractGridViewItem *force_visible_item,
                                const bool embedded_v2d = false);

  bool is_item_visible(int item_idx) const;
  void fill_layout_before_visible(Block &block) const;
  void fill_layout_after_visible(Block &block) const;

 private:
  IndexRange get_visible_range(const View2D &v2d,
                               const AbstractGridViewItem *force_visible_item) const;
  void add_spacer_button(Block &block, int row_count) const;
};

BuildOnlyVisibleButtonsHelper::BuildOnlyVisibleButtonsHelper(
    const View2D &v2d,
    const AbstractGridView &grid_view,
    const int cols_per_row,
    const AbstractGridViewItem *force_visible_item,
    const bool embedded_v2d)
    : grid_view_(grid_view),
      style_(grid_view.get_style()),
      cols_per_row_(cols_per_row),
      embedded_v2d_(embedded_v2d)
{
  if (grid_view.use_fixed_viewport_layout()) {
    /* Fixed-viewport layouts derive the visible rows from #scroll_px, not the region #View2D
     * (which the popup pipeline re-initializes on every refresh). */
    if (grid_view.get_item_count_filtered()) {
      visible_items_range_ = grid_view.fixed_viewport_visible_range();
    }
    return;
  }
  if (v2d.flag & V2D_IS_INIT && grid_view.get_item_count_filtered()) {
    visible_items_range_ = this->get_visible_range(v2d, force_visible_item);
  }
}

IndexRange BuildOnlyVisibleButtonsHelper::get_visible_range(
    const View2D &v2d, const AbstractGridViewItem *force_visible_item) const
{
  BLI_assert(v2d.flag & V2D_IS_INIT);

  int first_idx_in_view = 0;

  if (embedded_v2d_) {
    /* Fixed viewport: map #View2D::cur.ymax (0 at top, negative when scrolled) to row index. */
    const float scrolled_past_top = v2d.tot.ymax - v2d.cur.ymax;
    if (scrolled_past_top > 0.0f) {
      const int scrolled_away_rows = int(scrolled_past_top) / style_.tile_height;
      first_idx_in_view = scrolled_away_rows * cols_per_row_;
    }
  }
  else {
    const float scroll_ofs_y = std::abs(v2d.cur.ymax - v2d.tot.ymax);
    float scrolled_past_grid_top = scroll_ofs_y;

    /* Grids are often embedded below other UI (panel headers, catalog pills, etc.). The view
     * bounds from the previous redraw are used to only virtualize once the scroll offset is past
     * that content, not from the top of the entire region. */
    if (const std::optional<rcti> bounds = grid_view_.get_bounds()) {
      if (!BLI_rcti_is_empty(&*bounds)) {
        const float grid_top = float(bounds->ymax);
        const float content_top = v2d.tot.ymax;
        scrolled_past_grid_top = scroll_ofs_y - (content_top - grid_top);
      }
    }

    if (scrolled_past_grid_top > 0.0f) {
      const int scrolled_away_rows = int(scrolled_past_grid_top) / style_.tile_height;
      first_idx_in_view = scrolled_away_rows * cols_per_row_;
    }
  }

  const int item_count = grid_view_.get_item_count_filtered();
  if (item_count > 0 && first_idx_in_view >= item_count) {
    first_idx_in_view = max_ii(0, item_count - cols_per_row_);
  }

  const int view_height = BLI_rcti_size_y(&v2d.mask);
  const int count_rows_in_view = std::max(view_height / style_.tile_height, 1);
  /* Embedded grids use a fixed viewport height; do not add an extra buffer row. */
  const int max_items_in_view = embedded_v2d_ ? count_rows_in_view * cols_per_row_ :
                                                (count_rows_in_view + 1) * cols_per_row_;
  BLI_assert(max_items_in_view > 0);

  IndexRange visible_items(first_idx_in_view, max_items_in_view);

  /* Ensure #visible_items contains #force_visible_item, adjust if necessary. */
  if (force_visible_item && force_visible_item->is_filtered_visible()) {
    if (std::optional<int> item_idx = grid_view_find_filtered_item_index(*force_visible_item)) {
      if (!visible_items.contains(*item_idx)) {
        /* Move range so the first row contains #force_visible_item. */
        const int aligned_start = *item_idx - (*item_idx % cols_per_row_);
        return IndexRange(aligned_start, max_items_in_view);
      }
    }
  }

  return visible_items;
}

bool BuildOnlyVisibleButtonsHelper::is_item_visible(const int item_idx) const
{
  return !visible_items_range_ || visible_items_range_->contains(item_idx);
}

void BuildOnlyVisibleButtonsHelper::fill_layout_before_visible(Block &block) const
{
  if (grid_view_.use_fixed_viewport_layout()) {
    return;
  }
  if (!visible_items_range_ || visible_items_range_->is_empty()) {
    return;
  }
  const int first_idx_in_view = visible_items_range_->first();
  if (first_idx_in_view < 1) {
    return;
  }
  const int tot_tiles_before_visible = first_idx_in_view;
  const int scrolled_away_rows = tot_tiles_before_visible / cols_per_row_;
  this->add_spacer_button(block, scrolled_away_rows);
}

void BuildOnlyVisibleButtonsHelper::fill_layout_after_visible(Block &block) const
{
  if (grid_view_.use_fixed_viewport_layout()) {
    return;
  }
  if (!visible_items_range_ || visible_items_range_->is_empty()) {
    return;
  }
  const int last_item_idx = grid_view_.get_item_count_filtered() - 1;
  const int last_visible_idx = visible_items_range_->last();

  if (last_item_idx > last_visible_idx) {
    const int remaining_rows = (cols_per_row_ > 0) ? ceilf((last_item_idx - last_visible_idx) /
                                                           float(cols_per_row_)) :
                                                     0;
    BuildOnlyVisibleButtonsHelper::add_spacer_button(block, remaining_rows);
  }
}

void BuildOnlyVisibleButtonsHelper::add_spacer_button(Block &block, const int row_count) const
{
  /* UI code only supports button dimensions of `signed short` size, the layout height we want to
   * fill may be bigger than that. So add multiple labels of the maximum size if necessary. */
  for (int remaining_rows = row_count; remaining_rows > 0;) {
    const short row_count_this_iter = std::min(
        std::numeric_limits<short>::max() / style_.tile_height, remaining_rows);

    uiDefBut(&block,
             ButtonType::Label,
             "",
             0,
             0,
             UI_UNIT_X,
             row_count_this_iter * style_.tile_height,
             nullptr,
             0,
             0,
             "");
    remaining_rows -= row_count_this_iter;
  }
}

/* ---------------------------------------------------------------------- */

class GridViewLayoutBuilder {
  Block &block_;

  friend class GridViewBuilder;

 public:
  GridViewLayoutBuilder(Layout &layout);

  void build_from_view(const bContext &C,
                       AbstractGridView &grid_view,
                       const View2D &v2d,
                       const bool embedded_v2d) const;

 private:
  void build_grid_tile(const bContext &C, Layout &grid_layout, AbstractGridViewItem &item) const;

  Layout &current_layout() const;
};

GridViewLayoutBuilder::GridViewLayoutBuilder(Layout &layout) : block_(*layout.block()) {}

void GridViewLayoutBuilder::build_grid_tile(const bContext &C,
                                            Layout &grid_layout,
                                            AbstractGridViewItem &item) const
{
  Layout &overlap = grid_layout.overlap();
  overlap.fixed_size_set(true);

  item.add_grid_tile_button(block_);
  item.build_grid_tile(C, overlap.row(false));
}

void GridViewLayoutBuilder::build_from_view(const bContext &C,
                                            AbstractGridView &grid_view,
                                            const View2D &v2d,
                                            const bool embedded_v2d) const
{
  Layout &parent_layout = this->current_layout();

  /* Fixed-viewport grids (popovers) place the tiles and the overflow scrollbar side by side in a
   * row: the scrollbar is a real column to the *right* of the grid (like a #View2D scrollbar), so it
   * never covers the last tile. The host reserves the scrollbar's width inside the grid column (see
   * #asset_shelf_popover). Region-scrolled grids keep the plain column. */
  const bool fixed_viewport = grid_view.use_fixed_viewport_layout();
  Layout &grid_host = parent_layout.column(true);
  Layout *grid_row = fixed_viewport ? &grid_host.row(false) : nullptr;
  Layout &layout = grid_row ? grid_row->column(true) : grid_host;
  const GridViewStyle &style = grid_view.get_style();

  /* We might not actually know the width available for the grid view. Let's just assume that
   * either there is a fixed width defined via #uiLayoutSetUnitsX() or that the layout is close to
   * the root level and inherits its width. Might need a more reliable method. */
  const int cols_per_row = [&]() {
    if (grid_view.cols_per_row_hint_ > 0) {
      return grid_view.cols_per_row_hint_;
    }
    const int guessed_layout_width = (parent_layout.ui_units_x() > 0) ?
                                         parent_layout.ui_units_x() * UI_UNIT_X :
                                         parent_layout.width();
    return std::max(guessed_layout_width / style.tile_width, 1);
  }();
  grid_view.cols_per_row_ = cols_per_row;

  if (grid_view.use_fixed_viewport_layout()) {
    /* Now that the column count and filtered items are known, snap the stored scroll row into the
     * valid range, then apply any deferred "scroll active into view" request (e.g. on first open of
     * the popover) before the visible rows are selected below. */
    grid_view.fixed_viewport_clamp_scroll_value();
    const bool session_focus_pending = grid_view.session_ &&
                                       grid_view.session_->scroll_active_into_view_pending;
    if (grid_view.scroll_active_into_view_on_build_ || session_focus_pending) {
      const bool center = session_focus_pending ?
                              grid_view.session_->scroll_active_to_center_pending :
                              grid_view.scroll_active_center_on_build_;
      const bool found_active = grid_view.fixed_viewport_scroll_active_into_view(center);
      grid_view.scroll_active_into_view_on_build_ = false;
      grid_view.scroll_active_center_on_build_ = false;
      if (found_active && grid_view.session_) {
        grid_view.session_->scroll_active_into_view_pending = false;
        grid_view.session_->scroll_active_to_center_pending = false;
      }
    }

    /* Pin the grid row and tile column to the pixel-exact viewport height the host asked for (e.g.
     * the popover's resize grip), whether or not the content overflows it. Sizing must not depend
     * on the content: padding the layout out with whole tile rows instead would quantize the height
     * to the tile size, so a grid that happens to fit would follow the preview size rather than the
     * requested height. The sibling scrollbar and the clipped grid share this window, and siblings
     * below the grid are laid out unaffected by the buffer-row overflow.
     *
     * On overflow the column additionally becomes a scroll-clip window (see
     * #Layout::view_scroll_clip_set): the buffer/partially scrolled rows built below overflow the
     * window and are cut at its edges instead of growing the popup. The clip window is the raw
     * pixel viewport height, not a whole-row multiple: when it is taller than the fully visible
     * rows, the extra pixels show a partial bottom row cut at the window edge (matching the
     * reference image grid), so no dead space is left below the grid when the tile size changes
     * without the popover resizing. */
    const int visible_height = grid_view.fixed_viewport_geometry().viewport_height;
    grid_host.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
    if (grid_row != nullptr) {
      grid_row->ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
    }
    layout.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
    if (!grid_view.is_fully_visible()) {
      layout.view_scroll_clip_set(visible_height, grid_view.scroll_offset_px(), &grid_view);
    }

    /* Publish geometry to the session so the unified input handler hit-tests and clamps scrolling
     * from stable state, independent of the tiles built this frame (mirrors the embedded host's
     * #GridStateAccess::geometry_store). The popup pipeline hands the grid its
     * #uiPopupBlockHandle::region. */
    grid_view.store_fixed_viewport_session_geometry(block_.handle ? block_.handle->region :
                                                                    nullptr);
  }

  const AbstractGridViewItem *force_visible_item = dynamic_cast<const AbstractGridViewItem *>(
      grid_view.search_highlight_item());
  if (!force_visible_item && embedded_v2d) {
    grid_view.foreach_filtered_item([&](AbstractGridViewItem &item) {
      if (item.is_active()) {
        force_visible_item = &item;
      }
    });
  }

  BuildOnlyVisibleButtonsHelper build_visible_helper(
      v2d, grid_view, cols_per_row, force_visible_item, embedded_v2d);

  /* Spacers simulate full scroll height in region #View2D grids; embedded fixed viewports only
   * swap visible tiles inside a clipped layout (see sculpt image grid). */
  if (!embedded_v2d) {
    build_visible_helper.fill_layout_before_visible(block_);
  }

  int item_idx = 0;
  Layout *row = nullptr;
  grid_view.foreach_filtered_item([&](AbstractGridViewItem &item) {
    /* Skip if item isn't visible. */
    if (!build_visible_helper.is_item_visible(item_idx)) {
      item_idx++;
      return;
    }

    /* Start a new row for every first item in the row. Also when the first *visible* item
     * starts mid-row (virtualization), #row may still be null. */
    if (row == nullptr || (item_idx % cols_per_row) == 0) {
      row = &layout.row(true);
    }

    this->build_grid_tile(C, *row, item);
    item_idx++;
  });

  /* Overflow scrollbar (pixel-scale, mouse-draggable) as a column to the *right* of the tile column
   * (a later sibling in #grid_row), so it sits beside the grid instead of over it. Only when the
   * fixed viewport overflows and the grid has a session to bind the pixel scroll position to.
   * Replaces the thin draw-only thumb. */
  if (grid_row != nullptr && !grid_view.is_fully_visible()) {
    if (int *scroll_px = grid_view.session_scroll_px_ptr()) {
      const int visible_height = grid_view.fixed_viewport_geometry().viewport_height;
      const int max_scroll_px = grid_view.fixed_viewport_max_scroll_px();
      Layout &scroll_col = grid_row->column(false);
      scroll_col.fixed_size_set(true);
      scroll_col.ui_units_x_set(float(V2D_SCROLL_WIDTH) / float(UI_UNIT_X));
      scroll_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
      block_layout_set_current(&block_, &scroll_col);
      Button *but = uiDefButV(&block_,
                              ButtonType::Scroll,
                              "",
                              0,
                              0,
                              short(V2D_SCROLL_WIDTH),
                              visible_height,
                              scroll_px,
                              0.0f,
                              float(max_scroll_px),
                              "");
      auto *but_scroll = reinterpret_cast<ButtonScrollBar *>(but);
      but_scroll->visual_height = float(visible_height);
      uchar scroll_track_bg[4];
      theme::get_color_4ubv(TH_BACK, scroll_track_bg);
      scroll_track_bg[3] = 255;
      button_color_set(but, scroll_track_bg);
      button_flag_disable(but, BUT_UNDO);
      button_func_set(but, [](bContext &C) {
        ARegion *region = CTX_wm_region_popup(&C) ? CTX_wm_region_popup(&C) : CTX_wm_region(&C);
        if (region) {
          ED_region_tag_redraw(region);
          ED_region_tag_refresh_ui(region);
        }
      });
    }
  }

  block_layout_set_current(&block_, &parent_layout);

  if (!embedded_v2d) {
    build_visible_helper.fill_layout_after_visible(block_);
  }
}

Layout &GridViewLayoutBuilder::current_layout() const
{
  return *block_.curlayout;
}

/* ---------------------------------------------------------------------- */

GridViewBuilder::GridViewBuilder(Block & /*block*/) {}

void GridViewBuilder::build_grid_view(const bContext &C,
                                      AbstractGridView &grid_view,
                                      Layout &layout,
                                      std::optional<StringRef> search_string,
                                      const View2D *v2d_override)
{
  Block &block = *layout.block();

  const ARegion *region = CTX_wm_region_popup(&C) ? CTX_wm_region_popup(&C) : CTX_wm_region(&C);
  if (block.handle != nullptr && block.handle->region != nullptr && block_is_popup_any(&block)) {
    region = block.handle->region;
  }
  if (!v2d_override) {
    block_view_persistent_state_restore(*region, block, grid_view);
  }

  grid_view.build_items();
  grid_view.update_from_old(block);

  /* When a drag didn't result in a drop, an 'orphaned' drop line hint could otherwise be left
   * behind -- there's no on-drag-end callback to clear it from, so check for active drags here. */
  const wmWindowManager *wm = CTX_wm_manager(&C);
  if (!wm || wm->runtime->drags.is_empty()) {
    grid_view.clear_drop_linehint();
  }

  grid_view.change_state_delayed();
  grid_view.filter(search_string);

  block_layout_set_current(&block, &layout);

  GridViewLayoutBuilder builder(layout);
  const View2D &v2d = v2d_override ? *v2d_override : region->v2d;
  builder.build_from_view(C, grid_view, v2d, v2d_override != nullptr);

  if (grid_view.scroll_active_into_center_on_draw_) {
    grid_view.scroll_active_into_center(const_cast<bContext *>(&C));
    grid_view.scroll_active_into_center_on_draw_ = false;
  }
}

}  // namespace blender::ui
