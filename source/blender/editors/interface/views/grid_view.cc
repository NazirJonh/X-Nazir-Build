/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <cfloat>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

#include "BKE_context.hh"
#include "BKE_icons.hh"

#include "BLI_rect.h"

#include "BLI_index_range.hh"

#include "WM_types.hh"

#include "GPU_state.hh"

#include "RNA_access.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"
#include "interface_intern.hh"

#include "UI_grid_view.hh"

namespace blender::ui {

/* ---------------------------------------------------------------------- */

AbstractGridView::AbstractGridView() : style_(preview_tile_size_x(), preview_tile_size_y()) {}

AbstractGridViewItem &AbstractGridView::add_item(std::unique_ptr<AbstractGridViewItem> item)
{
  items_.append(std::move(item));

  AbstractGridViewItem &added_item = *items_.last();
  item_map_.add(added_item.identifier_, &added_item);
  this->register_item(added_item);

  return added_item;
}

void AbstractGridView::foreach_view_item(FunctionRef<void(AbstractViewItem &)> iter_fn) const
{
  /* Implementation for the base class virtual function. More specialized iterators below. */

  for (const auto &item_ptr : items_) {
    iter_fn(*item_ptr);
  }
}

void AbstractGridView::foreach_item(ItemIterFn iter_fn) const
{
  for (const auto &item_ptr : items_) {
    iter_fn(*item_ptr);
  }
}

void AbstractGridView::foreach_filtered_item(ItemIterFn iter_fn) const
{
  for (const auto &item_ptr : items_) {
    if (item_ptr->is_filtered_visible()) {
      iter_fn(*item_ptr);
    }
  }
}

AbstractGridViewItem *AbstractGridView::find_matching_item(
    const AbstractGridViewItem &item_to_match, const AbstractGridView &view_to_search_in) const
{
  AbstractGridViewItem *const *match = view_to_search_in.item_map_.lookup_ptr(
      item_to_match.identifier_);
  BLI_assert(!match || item_to_match.matches(**match));

  return match ? *match : nullptr;
}

void AbstractGridView::update_children_from_old(const AbstractView &old_view)
{
  const AbstractGridView &old_grid_view = dynamic_cast<const AbstractGridView &>(old_view);

  /* Share the scroll position with the previous view so it survives popover rebuilds (the same
   * mechanism #AbstractTreeView uses). The region #View2D is re-initialized on every refresh and
   * can't be relied on for this. */
  scroll_value_ = old_grid_view.scroll_value_;

  this->foreach_item([this, &old_grid_view](AbstractGridViewItem &new_item) {
    const AbstractGridViewItem *matching_old_item = find_matching_item(new_item, old_grid_view);
    if (!matching_old_item) {
      return;
    }

    new_item.update_from_old(*matching_old_item);
  });
}

const GridViewStyle &AbstractGridView::get_style() const
{
  return style_;
}

int AbstractGridView::get_item_count() const
{
  return items_.size();
}

int AbstractGridView::get_item_count_filtered() const
{
  if (item_count_filtered_) {
    return *item_count_filtered_;
  }

  int i = 0;
  this->foreach_filtered_item([&i](const auto &) { i++; });

  BLI_assert(i <= this->get_item_count());
  item_count_filtered_ = i;
  return i;
}

void AbstractGridView::set_tile_size(int tile_width, int tile_height)
{
  style_.tile_width = tile_width;
  style_.tile_height = tile_height;
}

void AbstractGridView::set_min_viewport_height(const int height_px)
{
  min_viewport_height_ = height_px;
}

std::optional<int> AbstractGridView::min_viewport_height() const
{
  return min_viewport_height_;
}

void AbstractGridView::set_fixed_viewport_layout(const bool fixed_viewport_layout)
{
  fixed_viewport_layout_ = fixed_viewport_layout;
}

bool AbstractGridView::use_fixed_viewport_layout() const
{
  return fixed_viewport_layout_;
}

AbstractGridView::FixedViewportGeometry AbstractGridView::fixed_viewport_geometry() const
{
  const int item_count = this->get_item_count_filtered();
  const int cols = cols_per_row_ > 0 ? cols_per_row_ : 1;
  const int content_rows = (item_count > 0) ? ((item_count - 1) / cols + 1) : 0;
  /* #min_viewport_height_ is always set for fixed-viewport layouts; the fallback only guards
   * misuse. */
  const int viewport_h = min_viewport_height_.value_or(int(UI_UNIT_Y * 12));
  const int visible_rows = std::max(viewport_h / style_.tile_height, 1);
  const int max_first_row = std::max(0, content_rows - visible_rows);
  return {cols, content_rows, visible_rows, max_first_row};
}

int AbstractGridView::fixed_viewport_first_row() const
{
  if (!scroll_value_) {
    return 0;
  }
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  return std::clamp(*scroll_value_, 0, geo.max_first_row);
}

IndexRange AbstractGridView::fixed_viewport_visible_range() const
{
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  const int first_idx = this->fixed_viewport_first_row() * geo.cols;
  const int count = geo.visible_rows * geo.cols;
  return IndexRange(first_idx, count);
}

void AbstractGridView::fixed_viewport_clamp_scroll_value()
{
  if (!scroll_value_) {
    scroll_value_ = std::make_shared<int>(0);
  }
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  *scroll_value_ = std::clamp(*scroll_value_, 0, geo.max_first_row);
}

bool AbstractGridView::supports_scrolling() const
{
  if (!fixed_viewport_layout_) {
    return false;
  }
  return !this->is_fully_visible();
}

bool AbstractGridView::is_fully_visible() const
{
  if (!fixed_viewport_layout_) {
    return false;
  }
  if (this->get_item_count_filtered() == 0) {
    return true;
  }
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  return geo.content_rows <= geo.visible_rows;
}

void AbstractGridView::scroll(const ViewScrollDirection direction)
{
  if (!fixed_viewport_layout_) {
    AbstractView::scroll(direction);
    return;
  }
  if (!scroll_value_) {
    scroll_value_ = std::make_shared<int>(0);
  }
  /* The value is clamped to the valid row range during the next build
   * (#fixed_viewport_clamp_scroll_value). */
  *scroll_value_ += (direction == ViewScrollDirection::UP) ? -1 : 1;
}

std::optional<uiViewState> AbstractGridView::persistent_state() const
{
  if (!scroll_value_) {
    return {};
  }
  uiViewState state{};
  state.scroll_offset = *scroll_value_;
  return state;
}

void AbstractGridView::persistent_state_apply(const uiViewState &state)
{
  if (state.scroll_offset) {
    scroll_value_ = std::make_shared<int>(state.scroll_offset);
  }
}

void AbstractGridView::fixed_viewport_scroll_active_into_view(const bool scroll_active_to_center)
{
  if (!scroll_value_) {
    scroll_value_ = std::make_shared<int>(0);
  }
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  if (geo.cols <= 0) {
    return;
  }

  int index = 0;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (item.is_active()) {
      const int row = index / geo.cols;
      if (scroll_active_to_center) {
        *scroll_value_ = std::clamp(
            row - geo.visible_rows / 2, 0, geo.max_first_row);
      }
      else if (row < *scroll_value_) {
        *scroll_value_ = row;
      }
      else if (row >= *scroll_value_ + geo.visible_rows) {
        *scroll_value_ = std::min(row - geo.visible_rows + 1, geo.max_first_row);
      }
    }
    index++;
  });
}

std::optional<ViewScrollDirection> AbstractGridView::fixed_viewport_scroll_at_y(
    const Block &block, const float block_space_y) const
{
  if (!fixed_viewport_layout_ || !this->supports_scrolling()) {
    return std::nullopt;
  }

  const std::optional<rcti> bounds = this->get_bounds();
  if (!bounds || BLI_rcti_is_empty(&*bounds)) {
    return std::nullopt;
  }

  /* Extend the active edge band into the ~0.5 #UI_UNIT_Y separator gaps above/below the grid so the
   * persistent scroll arrows drawn there (see #draw_overlays) are themselves hoverable. */
  const float gap = (0.5f * UI_UNIT_Y) / block.aspect;
  if (block_space_y > bounds->ymax + gap || block_space_y < bounds->ymin - gap) {
    return std::nullopt;
  }

  const int first_row = this->fixed_viewport_first_row();
  const float scroll_mouse = UI_MENU_SCROLL_MOUSE / block.aspect;
  if (block_space_y > bounds->ymax - scroll_mouse) {
    if (first_row > 0) {
      return ViewScrollDirection::UP;
    }
  }
  else if (block_space_y < bounds->ymin + scroll_mouse) {
    if (first_row < this->fixed_viewport_geometry().max_first_row) {
      return ViewScrollDirection::DOWN;
    }
  }
  return std::nullopt;
}

void AbstractGridView::draw_overlays(const ARegion &region, const Block &block) const
{
  if (!fixed_viewport_layout_ || !this->supports_scrolling()) {
    return;
  }

  /* Persistent scroll-arrow affordances, drawn whenever there is more content in that direction
   * (mirrors #draw_clip_tri for #BLOCK_CLIPTOP / #BLOCK_CLIPBOTTOM menus). The current scroll row
   * comes from #scroll_value_. */
  const int first_row = this->fixed_viewport_first_row();
  const bool can_scroll_up = first_row > 0;
  const bool can_scroll_down = first_row < this->fixed_viewport_geometry().max_first_row;
  if (!can_scroll_up && !can_scroll_down) {
    return;
  }

  /* Union visible tile rects in region pixel space (same path as #draw_button). */
  rcti pixel_bounds;
  BLI_rcti_init_minmax(&pixel_bounds);
  bool has_visible_tile = false;
  int tile_pixel_width = 0;
  for (const Button &but : block.buttons()) {
    if (but.type != ButtonType::ViewItem) {
      continue;
    }
    const auto *view_item_but = static_cast<const ButtonViewItem *>(&but);
    if (view_item_but->view_item == nullptr ||
        &view_item_but->view_item->get_view() != static_cast<const AbstractView *>(this))
    {
      continue;
    }
    if (but.flag & (UI_HIDDEN | UI_SCROLLED)) {
      continue;
    }
    rcti but_pixel{};
    button_to_pixelrect(&but_pixel, &region, &block, &but);
    BLI_rcti_do_minmax_rcti(&pixel_bounds, &but_pixel);
    tile_pixel_width = BLI_rcti_size_x(&but_pixel);
    has_visible_tile = true;
  }
  if (!has_visible_tile || BLI_rcti_is_empty(&pixel_bounds)) {
    return;
  }

  /* Center the arrows on the full grid width, not the union of visible tiles. The bottom row may be
   * partial (fewer than #cols_per_row_ tiles), and at high zoom the viewport can show only that one
   * row, so the union would be narrower than the grid and pull the arrow off-center to the left.
   * Tiles are left-aligned, so the grid's left edge is stable at #pixel_bounds xmin; extend it by
   * the full column count. With a full row visible this equals #BLI_rcti_cent_x(&pixel_bounds). */
  const int cols = std::max(cols_per_row_, 1);
  const int center_x = pixel_bounds.xmin + (cols * tile_pixel_width) / 2;

  float draw_color[4];
  theme::get_color_4fv(TH_TEXT, draw_color);

  /* Layout gap above/below the grid is ~0.5 #UI_UNIT_Y in block space; map through the block
   * matrix so arrows stay centered in those gaps when the parent UI is scaled (e.g. zoomed node
   * editor sets #Block::aspect on the popover via the browse button). */
  const float gap_px = 0.5f * UI_UNIT_Y * block_to_window_scale(&region, &block);
  const float aspect = block.aspect;
  GPU_blend(GPU_BLEND_ALPHA);
  if (can_scroll_up) {
    /* Lift the up-arrow an extra half #UI_UNIT_Y (one #gap_px) off the top grid row so it doesn't
     * crowd the tiles. */
    draw_icon_tri(float(center_x),
                  pixel_bounds.ymax + gap_px * 1.5f,
                  't',
                  draw_color,
                  aspect);
  }
  if (can_scroll_down) {
    draw_icon_tri(float(center_x),
                  pixel_bounds.ymin - gap_px * 0.5f,
                  'v',
                  draw_color,
                  aspect);
  }
  GPU_blend(GPU_BLEND_NONE);
}

static std::optional<int> find_filtered_item_index(const AbstractGridViewItem &item)
{
  BLI_assert(item.is_filtered_visible());

  const AbstractGridView &view = item.get_view();
  std::optional<int> index;

  int i = 0;
  view.foreach_filtered_item([&](AbstractGridViewItem &iter_item) {
    if (&item == &iter_item) {
      index = i;
    }
    i++;
  });

  return index;
}

AbstractViewItem *AbstractGridView::find_active_or_visible_item() const
{
  AbstractViewItem *active_item = nullptr;
  AbstractViewItem *first_visible_item = nullptr;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (item.is_active()) {
      active_item = &item;
    }
    if (!first_visible_item) {
      first_visible_item = &item;
    }
  });
  return active_item ? active_item : first_visible_item;
}

AbstractViewItem *AbstractGridView::navigate_left(AbstractViewItem *from)
{
  AbstractViewItem *next_item = nullptr;
  bool found_active = false;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    found_active |= (&item == from);
    if (!found_active) {
      next_item = &item;
    }
  });

  return found_active ? next_item : from;
}

AbstractViewItem *AbstractGridView::navigate_right(AbstractViewItem *from)
{
  AbstractViewItem *next_item = nullptr;
  bool found_active = false;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (found_active) {
      /* Store the element next to the active. */
      next_item = &item;
      found_active = false;
    }
    found_active = (&item == from);
  });

  return next_item ? next_item : from;
}

AbstractViewItem *AbstractGridView::navigate_up(AbstractViewItem *from)
{
  const std::optional<int> from_index = find_filtered_item_index(
      dynamic_cast<const AbstractGridViewItem &>(*from));

  const int next_item_index = std::clamp(
      *from_index - cols_per_row_, 0, get_item_count_filtered() - 1);

  int i = 0;
  AbstractViewItem *next_item = nullptr;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (i == next_item_index) {
      next_item = &item;
    }
    i++;
  });
  return next_item ? next_item : from;
}

AbstractViewItem *AbstractGridView::navigate_down(AbstractViewItem *from)
{
  const std::optional<int> from_index = find_filtered_item_index(
      dynamic_cast<const AbstractGridViewItem &>(*from));

  const int next_item_index = std::clamp(
      *from_index + cols_per_row_, 0, get_item_count_filtered() - 1);

  int i = 0;
  AbstractViewItem *next_item = nullptr;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (i == next_item_index) {
      next_item = &item;
    }
    i++;
  });

  return next_item ? next_item : from;
}

IndexRange AbstractGridView::get_visible_range(
    const View2D &v2d, const AbstractGridViewItem *force_visible_item) const
{
  BLI_assert(v2d.flag & V2D_IS_INIT);

  int first_idx_in_view = 0;

  const float scroll_ofs_y = std::abs(v2d.cur.ymax - v2d.tot.ymax);
  if (!IS_EQF(scroll_ofs_y, 0)) {
    const int scrolled_away_rows = int(scroll_ofs_y) / style_.tile_height;

    first_idx_in_view = scrolled_away_rows * cols_per_row_;
  }

  const int view_height = BLI_rcti_size_y(&v2d.mask);
  const int count_rows_in_view = std::max(view_height / style_.tile_height, 1);
  const int max_items_in_view = (count_rows_in_view + 1) * cols_per_row_;
  BLI_assert(max_items_in_view > 0);

  IndexRange visible_items(first_idx_in_view, max_items_in_view);

  /* Ensure #visible_items contains #force_visible_item, adjust if necessary. */
  if (force_visible_item && force_visible_item->is_filtered_visible()) {
    if (std::optional<int> item_idx = find_filtered_item_index(*force_visible_item)) {
      if (!visible_items.contains(*item_idx)) {
        /* Move range so the first row contains #force_visible_item. */
        return IndexRange((item_idx == 0) ? 0 : *item_idx % cols_per_row_, max_items_in_view);
      }
    }
  }

  return visible_items;
}

void AbstractGridView::scroll_active_into_view(bContext *C, bool scroll_active_to_center)
{
  if (fixed_viewport_layout_) {
    /* Fixed-viewport scrolling is a row index in #scroll_value_, applied by the next build. When
     * #cols_per_row_ isn't known yet (no build has run), defer to the build instead. */
    if (cols_per_row_ > 0) {
      this->fixed_viewport_scroll_active_into_view(scroll_active_to_center);
    }
    else {
      scroll_active_into_view_on_build_ = true;
    }
    return;
  }

  int index = 0;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (item.is_active()) {
      Button *but = reinterpret_cast<Button *>(item.view_item_button());
      /* Prefer the popup region so we don't accidentally scroll the host editor's View2D
       * when this is called from inside a popover draw callback. */
      ARegion *region = CTX_wm_region_popup(C);
      if (!region) {
        region = CTX_wm_region(C);
      }

      if (but) {
        but_ensure_in_view(C, region, but);
        return;
      }

      View2D &v2d = region->v2d;

      const IndexRange &visible_range = this->get_visible_range(v2d, nullptr);
      const int first_idx_in_view = visible_range.first();
      const int last_idx_in_view = visible_range.last();

      const int view_height = BLI_rcti_size_y(&v2d.mask);
      const int count_rows_in_view = std::max(view_height / style_.tile_height, 1);

      if (index < first_idx_in_view) {
        int target_row = index / cols_per_row_;
        target_row -= scroll_active_to_center ? count_rows_in_view / 2 : 0;
        const int cur_height = BLI_rctf_size_y(&v2d.cur);
        v2d.cur.ymax = v2d.tot.ymax - target_row * style_.tile_height;
        v2d.cur.ymin = v2d.cur.ymax - cur_height;
      }
      else if (index >= last_idx_in_view) {
        int target_row = (index / cols_per_row_) + 1;
        target_row += scroll_active_to_center ? count_rows_in_view / 2 : 0;
        const int cur_height = BLI_rctf_size_y(&v2d.cur);
        v2d.cur.ymin = v2d.tot.ymax - target_row * style_.tile_height;
        v2d.cur.ymax = v2d.cur.ymin + cur_height;
      }
    }
    index++;
  });
}

GridViewStyle::GridViewStyle(int width, int height) : tile_width(width), tile_height(height) {}

/* ---------------------------------------------------------------------- */

AbstractGridViewItem::AbstractGridViewItem(StringRef identifier) : identifier_(identifier) {}

bool AbstractGridViewItem::matches(const AbstractViewItem &other) const
{
  const AbstractGridViewItem &other_grid_item = dynamic_cast<const AbstractGridViewItem &>(other);
  return identifier_ == other_grid_item.identifier_;
}

void AbstractGridViewItem::add_grid_tile_button(Block &block)
{
  const GridViewStyle &style = this->get_view().get_style();
  view_item_but_ = static_cast<ButtonViewItem *>(uiDefBut(&block,
                                                          ButtonType::ViewItem,
                                                          "",
                                                          0,
                                                          0,
                                                          style.tile_width,
                                                          style.tile_height,
                                                          nullptr,
                                                          0,
                                                          0,
                                                          ""));

  view_item_but_->view_item = this;
}

std::optional<std::string> AbstractGridViewItem::debug_name() const
{
  return identifier_;
}

AbstractGridView &AbstractGridViewItem::get_view() const
{
  if (UNLIKELY(!view_)) {
    throw std::runtime_error(
        "Invalid state, item must be added through AbstractGridView::add_item()");
  }
  return dynamic_cast<AbstractGridView &>(*view_);
}

/* ---------------------------------------------------------------------- */

std::unique_ptr<DropTargetInterface> AbstractGridViewItem::create_item_drop_target()
{
  return create_drop_target();
}

std::unique_ptr<GridViewItemDropTarget> AbstractGridViewItem::create_drop_target()
{
  return nullptr;
}

GridViewItemDropTarget::GridViewItemDropTarget(AbstractGridView &view) : view_(view) {}

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
  /* Indices of items within the view. Calculated by constructor. If this is unset it means all
   * items/buttons should be drawn. */
  std::optional<IndexRange> visible_items_range_;

 public:
  BuildOnlyVisibleButtonsHelper(const View2D &v2d,
                                const AbstractGridView &grid_view,
                                int cols_per_row,
                                const AbstractGridViewItem *force_visible_item);

  bool is_item_visible(int item_idx) const;
  void fill_layout_before_visible(Block &block) const;
  void fill_layout_after_visible(Block &block) const;
  void fill_min_viewport_height(Block &block,
                                const AbstractGridView &grid_view,
                                int cols_per_row) const;

 private:
  IndexRange get_visible_range(const View2D &v2d,
                               const AbstractGridViewItem *force_visible_item) const;
  void add_spacer_button(Block &block, int row_count) const;
};

BuildOnlyVisibleButtonsHelper::BuildOnlyVisibleButtonsHelper(
    const View2D &v2d,
    const AbstractGridView &grid_view,
    const int cols_per_row,
    const AbstractGridViewItem *force_visible_item)
    : grid_view_(grid_view), style_(grid_view.get_style()), cols_per_row_(cols_per_row)
{
  if (grid_view.use_fixed_viewport_layout()) {
    /* Fixed-viewport layouts derive the visible rows from #scroll_value_, not the region #View2D
     * (which the popup pipeline re-initializes on every refresh). */
    if (grid_view.get_item_count_filtered()) {
      visible_items_range_ = grid_view.fixed_viewport_visible_range();
    }
    return;
  }
  if (v2d.flag & V2D_IS_INIT && grid_view.get_item_count_filtered()) {
    visible_items_range_ = this->grid_view_.get_visible_range(v2d, force_visible_item);
  }
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

void BuildOnlyVisibleButtonsHelper::fill_min_viewport_height(Block &block,
                                                             const AbstractGridView &grid_view,
                                                             const int cols_per_row) const
{
  const std::optional<int> min_height_opt = grid_view.min_viewport_height();
  if (!min_height_opt) {
    return;
  }

  const int item_count = grid_view.get_item_count_filtered();
  const int content_rows = (item_count > 0 && cols_per_row > 0) ?
                               ((item_count - 1) / cols_per_row + 1) :
                               0;
  const int content_height = content_rows * style_.tile_height;
  const int min_height = *min_height_opt;

  if (content_height >= min_height) {
    return;
  }

  const int pad_rows = (min_height - content_height + style_.tile_height - 1) / style_.tile_height;
  this->add_spacer_button(block, pad_rows);
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

  void build_from_view(const bContext &C, AbstractGridView &grid_view, const View2D &v2d) const;

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
                                            const View2D &v2d) const
{
  Layout &parent_layout = this->current_layout();

  Layout &layout = parent_layout.column(true);
  const GridViewStyle &style = grid_view.get_style();

  /* We might not actually know the width available for the grid view. Let's just assume that
   * either there is a fixed width defined via #uiLayoutSetUnitsX() or that the layout is close to
   * the root level and inherits its width. Might need a more reliable method. */
  const int guessed_layout_width = (parent_layout.ui_units_x() > 0) ?
                                       parent_layout.ui_units_x() * UI_UNIT_X :
                                       parent_layout.width();
  const int cols_per_row = std::max(guessed_layout_width / style.tile_width, 1);
  grid_view.cols_per_row_ = cols_per_row;

  if (grid_view.use_fixed_viewport_layout()) {
    /* Now that the column count and filtered items are known, snap the stored scroll row into the
     * valid range, then apply any deferred "scroll active into view" request (e.g. on first open of
     * the popover) before the visible rows are selected below. */
    grid_view.fixed_viewport_clamp_scroll_value();
    if (grid_view.scroll_active_into_view_on_build_) {
      grid_view.fixed_viewport_scroll_active_into_view(false);
      grid_view.scroll_active_into_view_on_build_ = false;
    }
  }

  const AbstractGridViewItem *search_highlight_item = dynamic_cast<const AbstractGridViewItem *>(
      grid_view.search_highlight_item());

  BuildOnlyVisibleButtonsHelper build_visible_helper(
      v2d, grid_view, cols_per_row, search_highlight_item);

  build_visible_helper.fill_layout_before_visible(block_);

  int item_idx = 0;
  Layout *row = nullptr;
  grid_view.foreach_filtered_item([&](AbstractGridViewItem &item) {
    /* Skip if item isn't visible. */
    if (!build_visible_helper.is_item_visible(item_idx)) {
      item_idx++;
      return;
    }

    /* Start a new row for every first item in the row. */
    if ((item_idx % cols_per_row) == 0) {
      row = &layout.row(true);
    }

    this->build_grid_tile(C, *row, item);
    item_idx++;
  });

  block_layout_set_current(&block_, &parent_layout);

  build_visible_helper.fill_layout_after_visible(block_);
  build_visible_helper.fill_min_viewport_height(block_, grid_view, cols_per_row);
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
                                      std::optional<StringRef> search_string)
{
  Block &block = *layout.block();

  const ARegion *region = CTX_wm_region_popup(&C) ? CTX_wm_region_popup(&C) : CTX_wm_region(&C);
  if (block.handle != nullptr && block.handle->region != nullptr && block_is_popup_any(&block)) {
    region = block.handle->region;
  }
  block_view_persistent_state_restore(*region, block, grid_view);

  grid_view.build_items();
  grid_view.update_from_old(block);
  grid_view.change_state_delayed();
  grid_view.filter(search_string);

  /* Ensure the given layout is actually active. */
  block_layout_set_current(&block, &layout);

  GridViewLayoutBuilder builder(layout);
  builder.build_from_view(C, grid_view, region->v2d);
}

/* ---------------------------------------------------------------------- */

PreviewGridItem::PreviewGridItem(StringRef identifier, StringRef label, int preview_icon_id)
    : AbstractGridViewItem(identifier), label(label), preview_icon_id(preview_icon_id)
{
}

void PreviewGridItem::build_grid_tile_button(Layout &layout,
                                             BIFIconID override_preview_icon_id) const
{
  const GridViewStyle &style = this->get_view().get_style();
  Block *block = layout.block();

  button_func_quick_tooltip_set(this->view_item_button(),
                                [this](const Button * /*but*/) { return label; });

  Button *but = uiDefBut(block,
                         ButtonType::PreviewTile,
                         hide_label_ ? "" : label,
                         0,
                         0,
                         style.tile_width,
                         style.tile_height,
                         nullptr,
                         0,
                         0,
                         "");

  const BIFIconID icon_id = override_preview_icon_id ? override_preview_icon_id : preview_icon_id;

  def_but_icon(but,
               icon_id,
               /* NOLINTNEXTLINE: bugprone-suspicious-enum-usage */
               UI_HAS_ICON | BUT_ICON_PREVIEW);
  but->emboss = EmbossType::None;
}

void PreviewGridItem::build_grid_tile(const bContext & /*C*/, Layout &layout) const
{
  this->build_grid_tile_button(layout);
}

void PreviewGridItem::set_on_activate_fn(ActivateFn fn)
{
  activate_fn_ = fn;
}

void PreviewGridItem::set_is_active_fn(IsActiveFn fn)
{
  is_active_fn_ = fn;
}

void PreviewGridItem::hide_label()
{
  hide_label_ = true;
}

void PreviewGridItem::on_activate(bContext &C)
{
  if (activate_fn_) {
    activate_fn_(C, *this);
  }
}

std::optional<bool> PreviewGridItem::should_be_active() const
{
  if (is_active_fn_) {
    return is_active_fn_();
  }
  return std::nullopt;
}

}  // namespace blender::ui
