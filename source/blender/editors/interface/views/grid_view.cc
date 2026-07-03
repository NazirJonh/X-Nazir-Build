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
#include "BKE_screen.hh"

#include "BLI_rect.h"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "GPU_state.hh"

#include "RNA_access.hh"

#include "ED_screen.hh"

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
  scroll_offset_px_ = old_grid_view.scroll_offset_px_;

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

void AbstractGridView::set_cols_per_row_hint(const int cols)
{
  cols_per_row_hint_ = std::max(cols, 0);
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
  BLI_assert(style_.tile_height > 0);
  const int tile_height = std::max(style_.tile_height, 1);
  const int item_count = this->get_item_count_filtered();
  const int cols = cols_per_row_ > 0 ? cols_per_row_ : 1;
  const int content_rows = (item_count > 0) ? ((item_count - 1) / cols + 1) : 0;
  /* #min_viewport_height_ is always set for fixed-viewport layouts; the fallback only guards
   * misuse. */
  const int viewport_h = min_viewport_height_.value_or(int(UI_UNIT_Y * 12));
  const int visible_rows = std::max(viewport_h / tile_height, 1);
  /* Pixel-exact scroll range: the whole content minus the raw pixel viewport, not just the whole
   * visible rows. When the viewport is taller than #visible_rows whole rows, the extra pixels show
   * a partial bottom row, so the last scroll position only needs to pull that partial row fully
   * into view (a fraction of a tile) instead of a whole row. */
  const int content_height = content_rows * tile_height;
  const int max_scroll_px = std::max(0, content_height - viewport_h);
  const int max_first_row = max_scroll_px / tile_height;
  return {cols, content_rows, visible_rows, max_first_row, viewport_h, max_scroll_px};
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
  const int tile_height = std::max(style_.tile_height, 1);
  /* Rows the viewport intersects at a whole-row scroll position. #viewport_height may not be an
   * exact multiple of the tile height, so round up: the extra pixels show a partial bottom row
   * that must be built to be drawn clipped (see #GridViewLayoutBuilder::build_from_view). */
  const int rows_touched = std::max(1, (geo.viewport_height + tile_height - 1) / tile_height);
  /* A sub-row #scroll_offset_px() shifts content up, so the window intersects one *more* row at
   * the bottom; without this buffer row it would fall outside this range and vanish instead of
   * being drawn clipped. The overflow is cut away by the scroll-clip window. */
  const int buffer_rows = (this->scroll_offset_px() > 0) ? 1 : 0;
  const int count = (rows_touched + buffer_rows) * geo.cols;
  return IndexRange(first_idx, count);
}

void AbstractGridView::fixed_viewport_clamp_scroll_value()
{
  if (!scroll_value_) {
    scroll_value_ = std::make_shared<int>(0);
  }
  if (!scroll_offset_px_) {
    scroll_offset_px_ = std::make_shared<int>(0);
  }
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  const int tile_height = std::max(style_.tile_height, 1);
  *scroll_value_ = std::clamp(*scroll_value_, 0, geo.max_first_row);
  /* Sub-row offset: on the last row, cap at the pixel remainder of the scroll range so the bottom
   * edge stops exactly at the content end (the partial bottom row pulled fully into view). This
   * remainder is 0 when the viewport height is a whole multiple of the tile height, restoring the
   * old whole-row pin. Otherwise the offset is a full sub-row range. */
  if (*scroll_value_ >= geo.max_first_row) {
    *scroll_offset_px_ = std::clamp(*scroll_offset_px_, 0, geo.max_scroll_px % tile_height);
  }
  else {
    *scroll_offset_px_ = std::clamp(*scroll_offset_px_, 0, tile_height - 1);
  }
}

int AbstractGridView::scroll_value() const
{
  return scroll_value_ ? *scroll_value_ : 0;
}

void AbstractGridView::scroll_value_set(const int value)
{
  if (!scroll_value_) {
    scroll_value_ = std::make_shared<int>(0);
  }
  /* Clamped to the valid row range during the next build (#fixed_viewport_clamp_scroll_value). */
  *scroll_value_ = value;
}

int AbstractGridView::scroll_offset_px() const
{
  if (!scroll_offset_px_) {
    return 0;
  }
  /* Report the effective offset the next build will apply (#fixed_viewport_clamp_scroll_value):
   * capped at the scroll-range pixel remainder on the last page, within one tile otherwise. */
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  const int tile_height = std::max(style_.tile_height, 1);
  if (this->fixed_viewport_first_row() >= geo.max_first_row) {
    return std::clamp(*scroll_offset_px_, 0, geo.max_scroll_px % tile_height);
  }
  return std::clamp(*scroll_offset_px_, 0, tile_height - 1);
}

void AbstractGridView::scroll_offset_px_set(const int offset_px)
{
  if (!scroll_offset_px_) {
    scroll_offset_px_ = std::make_shared<int>(0);
  }
  *scroll_offset_px_ = offset_px;
}

int AbstractGridView::fixed_viewport_max_scroll_px() const
{
  return this->fixed_viewport_geometry().max_scroll_px;
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
  /* Pixel-exact: fully visible when the whole content fits within the raw viewport height. When
   * the viewport is taller than the whole visible rows, a partial bottom row of the *last* content
   * row can still make the content overflow, so compare heights rather than row counts. */
  return this->fixed_viewport_geometry().max_scroll_px == 0;
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
  /* Row-stepped scrolling (wheel, edge auto-scroll) snaps to whole rows: drop any sub-row offset
   * left over from a drag so rows land exactly on the window edges. */
  if (scroll_offset_px_) {
    *scroll_offset_px_ = 0;
  }
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
      bool scrolled = false;
      if (scroll_active_to_center) {
        *scroll_value_ = std::clamp(
            row - geo.visible_rows / 2, 0, geo.max_first_row);
        scrolled = true;
      }
      else if (row < *scroll_value_) {
        *scroll_value_ = row;
        scrolled = true;
      }
      else if (row >= *scroll_value_ + geo.visible_rows) {
        *scroll_value_ = std::min(row - geo.visible_rows + 1, geo.max_first_row);
        scrolled = true;
      }
      /* Jumping to the active item snaps to whole rows, otherwise the target row would still be
       * cut by a sub-row offset left over from a drag. */
      if (scrolled && scroll_offset_px_) {
        *scroll_offset_px_ = 0;
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

  /* The buffer/partially scrolled rows are cut to the scroll-clip window when drawn; clamp the
   * arrow anchor bounds the same way so the arrows stay attached to the visible window edges. */
  if (block.view_scroll_clip_enabled) {
    const rcti clip_pixel_rect = rect_to_pixelrect(&region, &block, &block.view_scroll_clip_rect);
    if (!BLI_rcti_isect(&pixel_bounds, &clip_pixel_rect, &pixel_bounds)) {
      return;
    }
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
  const float scale_factor = block_to_window_scale(&region, &block);
  const float gap_px = 0.5f * UI_UNIT_Y * scale_factor;
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

  /* Thin scroll indicator along the right window edge. Its position includes the sub-row
   * #scroll_offset_px so it glides smoothly during drag/trackpad scrolling instead of jumping row
   * by row. */
  {
    const FixedViewportGeometry geo = this->fixed_viewport_geometry();
    const int max_scroll_px = this->fixed_viewport_max_scroll_px();
    if (max_scroll_px > 0) {
      const int tile_h = std::max(style_.tile_height, 1);
      const float scroll_px = float(first_row * tile_h + this->scroll_offset_px());
      const float track_h = float(BLI_rcti_size_y(&pixel_bounds));
      const float thumb_w = 4.0f * scale_factor;
      const float min_thumb_h = std::min(float(UI_UNIT_Y) * scale_factor, track_h);
      /* Pixel-exact thumb: viewport-to-content height ratio, so a partial bottom row sizes the
       * thumb proportionally instead of rounding to whole rows. */
      const float content_height = float(std::max(geo.content_rows * tile_h, 1));
      const float thumb_h = std::max(track_h * float(geo.viewport_height) / content_height,
                                     min_thumb_h);
      const float travel = std::max(track_h - thumb_h, 0.0f);
      const float frac = std::clamp(scroll_px / float(max_scroll_px), 0.0f, 1.0f);

      rctf thumb_rect;
      thumb_rect.xmax = float(pixel_bounds.xmax) - 2.0f * scale_factor;
      thumb_rect.xmin = thumb_rect.xmax - thumb_w;
      thumb_rect.ymax = float(pixel_bounds.ymax) - frac * travel;
      thumb_rect.ymin = thumb_rect.ymax - thumb_h;

      float thumb_color[4];
      theme::get_color_4fv(TH_TEXT, thumb_color);
      thumb_color[3] *= 0.35f;
      draw_roundbox_corner_set(CNR_ALL);
      draw_roundbox_aa(&thumb_rect, true, thumb_w * 0.5f, thumb_color);
    }
  }
  GPU_blend(GPU_BLEND_NONE);
}

/* -------------------------------------------------------------------- */
/** \name Fixed-viewport grid scrolling in popups
 *
 * Generic popup/menu event glue driving the fixed-viewport scroll of whichever grid view in the
 * block opted into it (#AbstractGridView::set_fixed_viewport_layout). Found via
 * #block_view_find_fixed_viewport_grid, so this knows nothing about the concrete view (e.g. the
 * image browser) it scrolls.
 * \{ */

bool popup_block_fixed_grid_autoscroll_at_pointer(Block *block, const int my)
{
  if (block == nullptr) {
    return false;
  }
  const AbstractGridView *grid_view = block_view_find_fixed_viewport_grid(*block);
  if (grid_view == nullptr) {
    return false;
  }
  return grid_view->fixed_viewport_scroll_at_y(*block, float(my)).has_value();
}

void popup_block_fixed_grid_redraw_for_scroll_overlay(ARegion *region, Block *block)
{
  if (region == nullptr || block == nullptr) {
    return;
  }
  const AbstractGridView *grid_view = block_view_find_fixed_viewport_grid(*block);
  if (grid_view == nullptr || grid_view->is_fully_visible()) {
    return;
  }
  ED_region_tag_redraw(region);
}

bool popup_block_fixed_grid_scrolltimer_step(bContext *C,
                                             PopupBlockHandle *menu,
                                             Block *block,
                                             const int my)
{
  if (block == nullptr || menu == nullptr || menu->region == nullptr) {
    return false;
  }
  AbstractGridView *grid_view = block_view_find_fixed_viewport_grid(*block);
  if (grid_view == nullptr) {
    return false;
  }

  const std::optional<ViewScrollDirection> scroll_dir = grid_view->fixed_viewport_scroll_at_y(
      *block, float(my));
  if (!scroll_dir) {
    return false;
  }

  /* Edge auto-scroll takes over from any running fling, otherwise both timers would fight over
   * the same scroll position. */
  popup_block_fixed_grid_fling_stop(C, menu);

  /* The scroll position is a row index stored in the grid view; the rebuild triggered below reads
   * it to pick the visible rows. */
  grid_view->scroll(*scroll_dir);
  ED_region_tag_refresh_ui(menu->region);
  return true;
}

bool popup_block_fixed_grid_wheel_scroll(bContext *C, ARegion *region, const wmEvent *event)
{
  if (region == nullptr) {
    return false;
  }

  Block *block = static_cast<Block *>(region->runtime->uiblocks.first);
  if (block == nullptr || (block->flag & BLOCK_POPOVER) == 0) {
    return false;
  }

  /* Route the wheel to whichever view is under the cursor. Over a non-grid scrollable view (e.g. the
   * catalog tree beside the asset grid in the asset shelf popover) scroll that view by whole steps
   * instead of the grid, and never let the grid steal the event while the cursor is over it. */
  if (AbstractView *hover_view = region_view_find_at(region, event->xy, 0, nullptr)) {
    if (dynamic_cast<AbstractGridView *>(hover_view) == nullptr) {
      if (hover_view->supports_scrolling() && !hover_view->is_fully_visible()) {
        std::optional<ViewScrollDirection> dir;
        if (event->type == WHEELUPMOUSE) {
          dir = ViewScrollDirection::UP;
        }
        else if (event->type == WHEELDOWNMOUSE) {
          dir = ViewScrollDirection::DOWN;
        }
        if (dir) {
          hover_view->scroll(*dir);
          ED_region_tag_refresh_ui(region);
          return true;
        }
      }
      return false;
    }
  }

  AbstractGridView *grid_view = block_view_find_fixed_viewport_grid(*block);
  if (grid_view == nullptr || grid_view->is_fully_visible()) {
    return false;
  }

  float mx = float(event->xy[0]);
  float my = float(event->xy[1]);
  window_to_block_fl(region, block, &mx, &my);

  /* Don't steal wheel events while hovering the fixed header above the grid; only the grid
   * scrolls. The grid's own bounds delimit the scrollable area (extended into the ~0.5 #UI_UNIT_Y
   * separator gap above it where the scroll-up arrow is drawn), so this needs no knowledge of the
   * popover's header layout. */
  if (const std::optional<rcti> bounds = grid_view->get_bounds()) {
    const float gap = (0.5f * UI_UNIT_Y) / block->aspect;
    if (my > bounds->ymax + gap) {
      return false;
    }
  }

  /* Manual scrolling takes over from any running fling. */
  popup_block_fixed_grid_fling_stop(C, block->handle);

  /* Trackpad pan: scroll pixel-exactly through the same clamped accumulator as touch dragging
   * (partially scrolled rows are drawn clipped, see #Layout::view_scroll_clip_set) instead of
   * discretizing into whole wheel steps. #WM_event_absolute_delta_y already applies the
   * natural-scroll preference; its sign follows wheel semantics (> 0 scrolls towards earlier
   * rows, like #WHEELUPMOUSE). */
  if (event->type == MOUSEPAN) {
    const int dy_window = WM_event_absolute_delta_y(event);
    if (dy_window != 0) {
      const float scale = block_to_window_scale(region, block);
      const int dy_content = int(roundf(float(dy_window) / std::max(scale, FLT_EPSILON)));
      const int tile_h = std::max(grid_view->get_style().tile_height, 1);
      const int total_px = std::clamp(grid_view->scroll_value() * tile_h +
                                          grid_view->scroll_offset_px() - dy_content,
                                      0,
                                      grid_view->fixed_viewport_max_scroll_px());
      if (total_px / tile_h != grid_view->scroll_value() ||
          total_px % tile_h != grid_view->scroll_offset_px())
      {
        grid_view->scroll_value_set(total_px / tile_h);
        grid_view->scroll_offset_px_set(total_px % tile_h);
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    }
    return true;
  }

  std::optional<ViewScrollDirection> direction;
  if (event->type == WHEELUPMOUSE) {
    direction = ViewScrollDirection::UP;
  }
  else if (event->type == WHEELDOWNMOUSE) {
    direction = ViewScrollDirection::DOWN;
  }
  else {
    return false;
  }

  /* The scroll position is a row index stored in the grid view (#scroll_value_), not the region
   * #View2D. The popup pipeline re-initializes the region #View2D on every refresh, so it can't
   * hold a stable scroll position. The rebuild triggered below reads the row index to pick the
   * visible rows. */
  grid_view->scroll(*direction);
  ED_region_tag_refresh_ui(region);
  return true;
}

bool popup_block_fixed_grid_drag_scroll_dy(ARegion *region, const int dy)
{
  if (region == nullptr) {
    return false;
  }
  Block *block = static_cast<Block *>(region->runtime->uiblocks.first);
  if (block == nullptr || (block->flag & BLOCK_POPOVER) == 0) {
    return false;
  }
  AbstractGridView *grid_view = block_view_find_fixed_viewport_grid(*block);
  if (grid_view == nullptr) {
    return false;
  }
  if (grid_view->is_fully_visible()) {
    /* A fixed-viewport grid is present but there is nothing to scroll; still report it so the caller
     * doesn't fall back to whole-popover scrolling. */
    return true;
  }

  /* Blender Y-up: drag up (dy > 0) makes content follow the cursor up, revealing later rows (same
   * sign as the touch/pen drag in #ui_do_but_VIEW_ITEM). Scroll pixel-exactly through the same
   * clamped accumulator as the wheel trackpad pan; the partially scrolled rows are drawn clipped. */
  const float scale = block_to_window_scale(region, block);
  const int dy_content = int(roundf(float(dy) / std::max(scale, FLT_EPSILON)));
  const int tile_h = std::max(grid_view->get_style().tile_height, 1);
  const int total_px = std::clamp(grid_view->scroll_value() * tile_h +
                                      grid_view->scroll_offset_px() + dy_content,
                                  0,
                                  grid_view->fixed_viewport_max_scroll_px());
  if (total_px / tile_h != grid_view->scroll_value() ||
      total_px % tile_h != grid_view->scroll_offset_px())
  {
    grid_view->scroll_value_set(total_px / tile_h);
    grid_view->scroll_offset_px_set(total_px % tile_h);
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
  return true;
}

bool popup_region_point_over_fixed_grid(ARegion *region, const int xy[2])
{
  if (region == nullptr) {
    return false;
  }
  AbstractView *view = region_view_find_at(region, xy, 0, nullptr);
  if (view == nullptr) {
    return false;
  }
  AbstractGridView *grid_view = dynamic_cast<AbstractGridView *>(view);
  return grid_view != nullptr && grid_view->use_fixed_viewport_layout();
}

/* Kinetic (fling) scrolling tuning. Velocities are in content pixels per second. */
/** Timer step; scroll positions are integers, so higher rates would round each step to 0 px. */
static constexpr double GRID_FLING_TIMER_STEP = 1.0 / 60.0;
/** Release velocities below this feel like a positioning drag, not a swipe: no fling. */
static constexpr float GRID_FLING_START_VELOCITY = 200.0f;
/** Settle when the decayed velocity drops below this. */
static constexpr float GRID_FLING_STOP_VELOCITY = 60.0f;
/** Exponential decay rate: the velocity roughly halves every 0.14 seconds. */
static constexpr float GRID_FLING_DECAY = 5.0f;

void popup_block_fixed_grid_fling_start(bContext *C,
                                        PopupBlockHandle *menu,
                                        const float velocity_px_per_sec)
{
  if (menu == nullptr || menu->region == nullptr) {
    return;
  }
  if (fabsf(velocity_px_per_sec) < GRID_FLING_START_VELOCITY) {
    return;
  }
  menu->grid_fling_velocity = velocity_px_per_sec;
  if (menu->grid_fling_timer == nullptr) {
    menu->grid_fling_timer = WM_event_timer_add(
        CTX_wm_manager(C), CTX_wm_window(C), TIMER, GRID_FLING_TIMER_STEP);
  }
}

void popup_block_fixed_grid_fling_stop(bContext *C, PopupBlockHandle *menu)
{
  if (menu == nullptr) {
    return;
  }
  menu->grid_fling_velocity = 0.0f;
  if (menu->grid_fling_timer) {
    WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), menu->grid_fling_timer);
    menu->grid_fling_timer = nullptr;
  }
}

bool popup_block_fixed_grid_fling_step(bContext *C, PopupBlockHandle *menu, Block *block)
{
  if (menu == nullptr || menu->grid_fling_timer == nullptr) {
    return false;
  }
  if (menu->region == nullptr) {
    /* No region left to scroll or redraw: settle the fling so the timer stops firing idly. */
    popup_block_fixed_grid_fling_stop(C, menu);
    return true;
  }
  AbstractGridView *grid_view = block ? block_view_find_fixed_viewport_grid(*block) : nullptr;
  if (grid_view == nullptr || grid_view->is_fully_visible()) {
    popup_block_fixed_grid_fling_stop(C, menu);
    return true;
  }

  const double dt = std::max(menu->grid_fling_timer->time_delta, 1.0e-4);
  const int tile_h = std::max(grid_view->get_style().tile_height, 1);
  const int total_px = grid_view->scroll_value() * tile_h + grid_view->scroll_offset_px();
  const int target_px = std::clamp(total_px + int(round(menu->grid_fling_velocity * dt)),
                                   0,
                                   grid_view->fixed_viewport_max_scroll_px());

  menu->grid_fling_velocity *= expf(-float(dt) * GRID_FLING_DECAY);

  if (target_px == total_px) {
    /* Hit a list boundary, or too slow to move a whole pixel: settle. */
    popup_block_fixed_grid_fling_stop(C, menu);
    return true;
  }

  grid_view->scroll_value_set(target_px / tile_h);
  grid_view->scroll_offset_px_set(target_px % tile_h);
  ED_region_tag_redraw(menu->region);
  ED_region_tag_refresh_ui(menu->region);

  if (fabsf(menu->grid_fling_velocity) < GRID_FLING_STOP_VELOCITY) {
    popup_block_fixed_grid_fling_stop(C, menu);
  }
  return true;
}

/** \} */

float popup_grid_fixed_viewport_units(const bContext *C,
                                      const Block *block,
                                      const float non_grid_units,
                                      const float tile_units,
                                      const float default_units)
{
  if (block == nullptr || block->handle == nullptr) {
    return default_units;
  }
  const Button *but = block->handle->popup_create_vars.but;
  ARegion *butregion = block->handle->popup_create_vars.butregion;
  if (but == nullptr || but->block == nullptr || butregion == nullptr) {
    return default_units;
  }
  const float aspect = but->block->aspect;
  if (aspect >= 1.0f) {
    /* Not zoomed in; the full-height popover fits. */
    return default_units;
  }

  /* Vertical space on the roomier side, measured from the button *edge* the popover stacks away
   * from, not its center: the popover grows away from the button, so the center over-counts the
   * available space by half the button height. At high zoom the button is drawn large (scaling with
   * 1/aspect), making that a 1-2 unit error - exactly what overflows the tightly-packed grid. */
  float bx = BLI_rctf_cent_x(&but->rect);
  float by_bottom = but->rect.ymin;
  float bx_top = bx;
  float by_top = but->rect.ymax;
  block_to_window_fl(butregion, but->block, &bx, &by_bottom);
  block_to_window_fl(butregion, but->block, &bx_top, &by_top);
  const auto win_size = WM_window_native_pixel_size(CTX_wm_window(C));
  float avail_px = std::max(by_bottom, float(win_size[1]) - by_top);

  /* Chrome the popover wraps around the content when positioned, all scaling with 1/aspect like the
   * layout: the arrow hint (~0.5 #widget_unit) plus the block bounds padding on both sides
   * (#block_margin = widget_unit/2 each) ≈ 1.5 widget_unit, plus a fixed screen-edge margin.
   * Subtracting it keeps the final block inside the window so it never turns menu-scrollable, which
   * would otherwise drag the fixed header off screen while the grid scrolls. Keep this
   * conservative. */
  avail_px -= 1.5f * float(UI_UNIT_Y) / aspect + float(UI_UNIT_Y) * 1.5f;

  /* popover_pixels = units * UI_UNIT_Y / aspect  →  units = pixels * aspect / UI_UNIT_Y. */
  const float budget_units = avail_px * aspect / float(UI_UNIT_Y);
  const float tile = std::max(tile_units, 0.001f);
  /* Whole tile rows that fit after the fixed header/gaps; never drop below a single row. */
  const int rows = std::max(1, int((budget_units - non_grid_units) / tile));
  return std::clamp(float(rows) * tile, tile, default_units);
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
      scroll_active_center_on_build_ = scroll_active_to_center;
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

void AbstractGridView::scroll_active_into_center(bContext *C)
{
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (item.is_active()) {
      Button *but = reinterpret_cast<Button *>(item.view_item_button());
      ARegion *region = CTX_wm_region(C);
      if (but && region && but->block) {
        View2D *v2d = &region->v2d;
        if ((v2d->flag & V2D_IS_INIT) == 0) {
          return;
        }

        rctf region_rect;
        block_to_region_rctf(region, but->block, &region_rect, &but->rect);

        rctf view_rect;
        view2d_region_to_view_rctf(v2d, &region_rect, &view_rect);

        const float target_center_y = BLI_rctf_cent_y(&view_rect);
        const float old_center_y = BLI_rctf_cent_y(&v2d->cur);

        if (fabsf(old_center_y - target_center_y) > 1.0f) {
          view2d_center_set(v2d, BLI_rctf_cent_x(&v2d->cur), target_center_y);
          view2d_curRect_changed(C, v2d);
          ED_region_tag_redraw_no_rebuild(region);
        }
      }
    }
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
    const AbstractGridViewItem *force_visible_item,
    const bool embedded_v2d)
    : grid_view_(grid_view),
      style_(grid_view.get_style()),
      cols_per_row_(cols_per_row),
      embedded_v2d_(embedded_v2d)
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
    if (std::optional<int> item_idx = find_filtered_item_index(*force_visible_item)) {
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

  Layout &layout = parent_layout.column(true);
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
    if (grid_view.scroll_active_into_view_on_build_) {
      grid_view.fixed_viewport_scroll_active_into_view(grid_view.scroll_active_center_on_build_);
      grid_view.scroll_active_into_view_on_build_ = false;
      grid_view.scroll_active_center_on_build_ = false;
    }

    /* Turn the grid column into a fixed-height scroll-clip window (see
     * #Layout::view_scroll_clip_set): the buffer/partially scrolled rows built below overflow the
     * window and are cut at its edges instead of growing the popup. #ui_units_y_set fixes the
     * estimated height to the window too, so siblings below the grid are laid out unaffected by
     * the overflow. Skipped while everything fits: no scrolling, no overflow to clip.
     *
     * The clip window is the raw pixel viewport height, not a whole-row multiple: when it is
     * taller than the fully visible rows, the extra pixels show a partial bottom row cut at the
     * window edge (matching the reference image grid), so no dead space is left below the grid when
     * the tile size changes without the popover resizing. */
    if (!grid_view.is_fully_visible()) {
      const int visible_height = grid_view.fixed_viewport_geometry().viewport_height;
      layout.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
      layout.view_scroll_clip_set(visible_height, grid_view.scroll_offset_px());
    }
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

  block_layout_set_current(&block_, &parent_layout);

  if (!embedded_v2d) {
    build_visible_helper.fill_layout_after_visible(block_);
  }
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
