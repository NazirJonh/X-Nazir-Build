/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Grid view types: #AbstractGridView, #AbstractGridViewItem, #PreviewGridItem, and overlay
 * drawing. Session registry: grid_view_session.cc. Input: grid_view_input.cc. Layout:
 * grid_view_layout.cc.
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

#include "DNA_screen_types.h"

#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_screen.hh"

#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"
#include "interface_grid_view.hh"
#include "interface_intern.hh"
#include "views/grid_view_intern.hh"

#include "UI_grid_view.hh"

namespace blender::ui {

/* ---------------------------------------------------------------------- */

AbstractGridView::AbstractGridView() : style_(preview_tile_size_x(), preview_tile_size_y()) {}

AbstractGridView::~AbstractGridView()
{
  if (session_) {
    grid_session_release(*session_);
  }
}

void AbstractGridView::use_session_scroll(const StringRef grid_id)
{
  BLI_assert(session_ == nullptr);
  session_ = &grid_session_state_ensure(grid_id);
  session_grid_id_ = grid_id;
  grid_session_acquire(*session_);
}

void AbstractGridView::store_fixed_viewport_session_geometry(const ARegion *region)
{
  if (session_ == nullptr) {
    return;
  }
  /* Mirror the embedded host's #GridStateAccess::geometry_store, but without the per-column scroll
   * pin (fixed-viewport grips only clip, they never reflow): just publish the stable geometry the
   * unified input handler reads to hit-test and clamp scrolling. */
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  session_->tile_h = std::max(style_.tile_height, 1);
  session_->cols = geo.cols;
  session_->cached_item_count = this->get_item_count_filtered();
  session_->viewport_px = geo.viewport_height;
  session_->region = region;
}

int AbstractGridView::scroll_px() const
{
  return session_ ? session_->scroll_px : 0;
}

void AbstractGridView::scroll_px_set(const int px)
{
  if (session_) {
    session_->scroll_px = std::clamp(px, 0, this->max_scroll_px());
  }
}

int *AbstractGridView::session_scroll_px_ptr()
{
  return session_ ? &session_->scroll_px : nullptr;
}

int AbstractGridView::max_scroll_px() const
{
  return fixed_viewport_layout_ ? this->fixed_viewport_geometry().max_scroll_px : 0;
}

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

  /* The scroll position needs no handoff here: it lives in the grid_id-keyed session registry
   * (see #use_session_scroll) and the rebuilt view re-attaches to the same entry. The region
   * #View2D is re-initialized on every refresh and can't be relied on for this. */

  has_drop_linehint_ = old_grid_view.has_drop_linehint_;
  drop_linehint_x_ = old_grid_view.drop_linehint_x_;
  drop_linehint_ymin_ = old_grid_view.drop_linehint_ymin_;
  drop_linehint_ymax_ = old_grid_view.drop_linehint_ymax_;

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

void AbstractGridView::set_preview_size_px(const int preview_size_px)
{
  style_.preview_size_px = preview_size_px;
}

void AbstractGridView::set_cols_per_row_hint(const int cols)
{
  cols_per_row_hint_ = std::max(cols, 0);
}

void AbstractGridView::set_min_viewport_height(const int height_px)
{
  min_viewport_height_ = height_px;
}

void AbstractGridView::set_fixed_viewport_layout(const bool fixed_viewport_layout)
{
  fixed_viewport_layout_ = fixed_viewport_layout;
}

bool AbstractGridView::use_fixed_viewport_layout() const
{
  return fixed_viewport_layout_;
}

void AbstractGridView::scroll_clip_set(const rctf &rect)
{
  scroll_clip_enabled_ = true;
  scroll_clip_rect_ = rect;
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
  /* Pixel-exact scroll range (see #grid_max_scroll_px): the last scroll position pulls a partial
   * bottom row fully into view instead of quantizing to whole rows. */
  const int max_scroll_px = grid_max_scroll_px(item_count, cols, tile_height, viewport_h);
  const int max_first_row = max_scroll_px / tile_height;
  return {cols, content_rows, visible_rows, max_first_row, viewport_h, max_scroll_px};
}

int AbstractGridView::fixed_viewport_first_row() const
{
  if (!session_) {
    return 0;
  }
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  const int tile_height = std::max(style_.tile_height, 1);
  return std::clamp(session_->scroll_px / tile_height, 0, geo.max_first_row);
}

IndexRange AbstractGridView::fixed_viewport_visible_range() const
{
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  const int first_idx = this->fixed_viewport_first_row() * geo.cols;
  const int tile_height = std::max(style_.tile_height, 1);
  /* Every row the clip window intersects at the current sub-row offset (see #grid_rows_to_build
   * — the single formula shared with the embedded host). Rows beyond the window edge are drawn
   * clipped by the scroll-clip window instead of vanishing. */
  const int count = grid_rows_to_build(geo.viewport_height, tile_height, this->scroll_offset_px()) *
                    geo.cols;
  return IndexRange(first_idx, count);
}

void AbstractGridView::fixed_viewport_clamp_scroll_value()
{
  /* Re-clamp against the freshly known geometry (item count / column count may have changed
   * since the position was written). The sub-row remainder on the last page emerges naturally
   * from the pixel-exact `[0, max_scroll_px]` range: when the viewport height is a whole
   * multiple of the tile height the remainder is 0, restoring the old whole-row pin. */
  this->scroll_px_set(this->scroll_px());
}

int AbstractGridView::scroll_value() const
{
  const int tile_height = std::max(style_.tile_height, 1);
  return this->scroll_px() / tile_height;
}

int AbstractGridView::scroll_offset_px() const
{
  const int tile_height = std::max(style_.tile_height, 1);
  return this->scroll_px() % tile_height;
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
  const int tile_height = std::max(style_.tile_height, 1);
  /* Row-stepped scrolling (wheel, edge auto-scroll) snaps to whole rows: drop any sub-row offset
   * left over from a drag so rows land exactly on the window edges. */
  const int row = this->scroll_px() / tile_height +
                  ((direction == ViewScrollDirection::UP) ? -1 : 1);
  this->scroll_px_set(std::max(row, 0) * tile_height);
}

void AbstractGridView::fixed_viewport_scroll_active_into_view(const bool scroll_active_to_center)
{
  const FixedViewportGeometry geo = this->fixed_viewport_geometry();
  if (geo.cols <= 0) {
    return;
  }
  const int tile_height = std::max(style_.tile_height, 1);
  const int current_row = this->scroll_px() / tile_height;

  int index = 0;
  this->foreach_filtered_item([&](AbstractViewItem &item) {
    if (item.is_active()) {
      const int row = index / geo.cols;
      /* Jumping to the active item snaps to whole rows, otherwise the target row would still be
       * cut by a sub-row offset left over from a drag. */
      if (scroll_active_to_center) {
        const int target = std::clamp(row - geo.visible_rows / 2, 0, geo.max_first_row);
        this->scroll_px_set(target * tile_height);
      }
      else if (row < current_row) {
        this->scroll_px_set(row * tile_height);
      }
      else if (row >= current_row + geo.visible_rows) {
        const int target = std::min(row - geo.visible_rows + 1, geo.max_first_row);
        this->scroll_px_set(target * tile_height);
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
  /* Independent of the fixed-viewport scroll-arrow affordances below: drawn whenever a drag is
   * hovering a drop target that opted in via #set_drop_linehint(). */
  this->draw_drop_linehint();

  if (!fixed_viewport_layout_ || !this->supports_scrolling()) {
    return;
  }

  /* Persistent scroll-arrow affordances, drawn whenever there is more content in that direction
   * (mirrors #draw_clip_tri for #BLOCK_CLIPTOP / #BLOCK_CLIPBOTTOM menus). The current scroll row
   * derives from #scroll_px(). */
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
   * arrow anchor bounds the same way so the arrows stay attached to the visible window edges.
   * This is this grid's own window (not a block-global one): a block may host more than one
   * clip-scrolled grid, each with its own. */
  if (this->scroll_clip_enabled()) {
    const rcti clip_pixel_rect = rect_to_pixelrect(&region, &block, &this->scroll_clip_rect());
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

  /* The scroll position indicator is the real #ButtonType::Scroll overlay widget built in
   * #GridViewLayoutBuilder::build_from_view (pixel-scale, mouse-draggable); only the persistent
   * scroll arrows are drawn here. */
  GPU_blend(GPU_BLEND_NONE);
}

void AbstractGridView::draw_drop_linehint() const
{
  if (!has_drop_linehint_) {
    return;
  }

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformThemeColorAlpha(TH_TEXT, 0.5f);

  GPU_line_width(2.0f);
  GPU_blend(GPU_BLEND_ALPHA);
  immBegin(GPU_PRIM_LINES, 2);
  immVertex2f(pos, float(drop_linehint_x_), float(drop_linehint_ymin_));
  immVertex2f(pos, float(drop_linehint_x_), float(drop_linehint_ymax_));
  immEnd();
  GPU_blend(GPU_BLEND_NONE);

  immUnbindProgram();
}

void AbstractGridView::set_drop_linehint(ARegion &region,
                                         const AbstractGridViewItem &item,
                                         const DropLocation location)
{
  /* Use the button size and position of the grid item for calculating the vertical line. */
  ButtonViewItem *but = item.view_item_button();
  if (but == nullptr) {
    has_drop_linehint_ = false;
    return;
  }

  rcti but_rect;
  button_to_pixelrect(&but_rect, &region, but->block, but);

  /* Grids have no nesting, so only #DropLocation::Before/After are ever used here (see
   * #FavoriteAssetDropTarget::choose_drop_location): hint the tile's left or right edge. */
  const int start_x = (location == DropLocation::Before) ? but_rect.xmin : but_rect.xmax;

  const bool changed = !has_drop_linehint_ || drop_linehint_x_ != start_x ||
                       drop_linehint_ymin_ != but_rect.ymin ||
                       drop_linehint_ymax_ != but_rect.ymax;
  has_drop_linehint_ = true;
  drop_linehint_x_ = start_x;
  drop_linehint_ymin_ = but_rect.ymin;
  drop_linehint_ymax_ = but_rect.ymax;

  if (changed) {
    ED_region_tag_redraw_no_rebuild(&region);
  }
}

void AbstractGridView::clear_drop_linehint()
{
  has_drop_linehint_ = false;
}

bool grid_view_session_scroll_button_under_mouse(const ARegion *region,
                                                 const int xy[2],
                                                 const StringRef grid_id)
{
  /* Hit-test a session grid's overlay scrollbar without exposing the session's transitional
   * scroll-widget field to callers outside the interface layer (View3D numpad-focus hotkey). */
  if (GridSessionState *session = grid_session_state_lookup(grid_id)) {
    return region_scroll_button_under_mouse(region, xy, &session->scroll_px);
  }
  return false;
}

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
  const auto win_size = WM_window_native_pixel_size(CTX_wm_window(C));

  /* Window-fit budget: the vertical pixel space the assembled block may occupy, from which the grid
   * viewport is sized so the block never overflows the window and turns menu-scrollable (which would
   * drag the fixed header and the bottom resize grip off screen). Run for every spawn, not just
   * zoomed (aspect < 1) ones: a user-enlarged popover overflows at aspect >= 1 too, and
   * #default_units alone does not window-fit the *assembled* block (grid + header/tab rows + grip +
   * positioning chrome). A default-sized popover is unaffected: its budget exceeds #default_units,
   * so the final #std::clamp returns #default_units unchanged. */
  float aspect;
  float avail_px;
  if (but == nullptr || but->block == nullptr || butregion == nullptr) {
    /* No source button (e.g. a panel popover opened without one, where
     * #PopupBlockHandle::popup_create_vars.but is null): the popover is not anchored to a button
     * edge, so there is no "roomier side" to measure. Fall back to the authoritative on-screen clip
     * range the block is bounded to in #popup_block_clip -- it may occupy the whole usable window
     * height minus the screen-edge reserves. */
    aspect = 1.0f;
    avail_px = float(win_size[1]) - UI_POPUP_MENU_TOP - UI_SCREEN_MARGIN;
  }
  else {
    aspect = but->block->aspect;
    /* Vertical space on the roomier side, measured from the button *edge* the popover stacks away
     * from, not its center: the popover grows away from the button, so the center over-counts the
     * available space by half the button height. At high zoom the button is drawn large (scaling
     * with 1/aspect), making that a 1-2 unit error - exactly what overflows the tightly-packed
     * grid. */
    float bx = BLI_rctf_cent_x(&but->rect);
    float by_bottom = but->rect.ymin;
    float bx_top = bx;
    float by_top = but->rect.ymax;
    block_to_window_fl(butregion, but->block, &bx, &by_bottom);
    block_to_window_fl(butregion, but->block, &bx_top, &by_top);
    avail_px = std::max(by_bottom, float(win_size[1]) - by_top);
  }

  /* Chrome the popover wraps around the content when positioned (see
   * #popover_vertical_chrome_px, defined next to the positioning code). Subtracting it keeps
   * the final block inside the window so it never turns menu-scrollable, which would drag the
   * fixed header off screen while the grid scrolls. */
  avail_px -= popover_vertical_chrome_px(aspect);

  /* popover_pixels = units * UI_UNIT_Y / aspect  →  units = pixels * aspect / UI_UNIT_Y. */
  const float budget_units = avail_px * aspect / float(UI_UNIT_Y);
  const float tile = std::max(tile_units, 0.001f);
  /* Whole tile rows that fit after the fixed header/gaps; never drop below a single row. */
  const int rows = std::max(1, int((budget_units - non_grid_units) / tile));
  return std::clamp(float(rows) * tile, tile, default_units);
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
  const std::optional<int> from_index = grid_view_find_filtered_item_index(
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
  const std::optional<int> from_index = grid_view_find_filtered_item_index(
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
    if (std::optional<int> item_idx = grid_view_find_filtered_item_index(*force_visible_item)) {
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
    /* Fixed-viewport scrolling is a pixel position in the session (#scroll_px), applied by the
     * next build. When #cols_per_row_ isn't known yet (no build has run), defer to the build
     * instead. */
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
      rctf rect;
      View2D &v2d = region->v2d;

      if (but) {
        rctf region_rect;
        block_to_region_rctf(region, but->block, &region_rect, &but->rect);

        view2d_region_to_view_rctf(&v2d, &region_rect, &rect);
      }

      const IndexRange &visible_range = this->get_visible_range(v2d, nullptr);
      int first_idx_in_view = visible_range.first();
      int last_idx_in_view = visible_range.last();

      /* When button is slightly outside the view, clamp region to button's height, see: !159566 */
      first_idx_in_view += rect.ymax > v2d.cur.ymax ? cols_per_row_ : 0;
      last_idx_in_view -= rect.ymin < v2d.cur.ymin ? cols_per_row_ : 0;

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
        v2d.cur.ymin = v2d.tot.ymax - target_row * style_.tile_height - 2 * U.pixelsize;
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

StringRef AbstractGridViewItem::identifier() const
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

PreviewGridItem::PreviewGridItem(StringRef identifier, StringRef label, int preview_icon_id)
    : AbstractGridViewItem(identifier), label(label), preview_icon_id(preview_icon_id)
{
}

void PreviewGridItem::build_grid_tile_button(Layout &layout,
                                             BIFIconID override_preview_icon_id,
                                             int preview_size_px) const
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
  /* Forward the unscaled preview size to the widget code so that small previews (< 56 px) can
   * scale the label font down based on the icon footprint, not the full tile (which includes
   * the label rect in tiles like the Asset Shelf that always show a name). The explicit argument
   * wins; otherwise fall back to the view style (set by the Asset Shelf). */
  but->preview_size_px = preview_size_px != 0 ? preview_size_px : style.preview_size_px;
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
