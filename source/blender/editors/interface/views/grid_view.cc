/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>

#include "DNA_screen_types.h"

#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_screen.hh"

#include "BLI_rect.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"

#include "ED_fileselect.hh"
#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"
#include "UI_view2d.hh"
#include "interface_grid_view.hh"
#include "interface_intern.hh"

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

/* -------------------------------------------------------------------- */
/** \name Unified grid input (wheel + touch drag + kinetic fling)
 *
 * One pre-button region handler driving scrolling for every session-backed grid, both hosts:
 * fixed-viewport popovers and the embedded core (generic Python grids). Two hit predicates keep
 * the two natural affordances apart: the wheel hits by *area* (#region_view_find_at over the
 * grid's clip-augmented bounds, stable across rebuilds — see #views_bounds_calc), while a press
 * hits by *z-order* (#region_view_item_topmost_idname_at) so a widget drawn over the grid (overlay
 * scrollbar) or below it (resize grip) keeps the press. All scrolling writes the single pixel
 * truth #GridSessionState::scroll_px; whole rows and the sub-row clip offset are derived on demand.
 * \{ */

/* Kinetic (fling) scrolling tuning. Velocities are in content pixels per second. */
/** Timer step; scroll positions are integers, so higher rates would round each step to 0 px. */
static constexpr double GRID_FLING_TIMER_STEP = 1.0 / 60.0;
/** Release velocities below this feel like a positioning drag, not a swipe: no fling. */
static constexpr float GRID_FLING_START_VELOCITY = 200.0f;
/** Settle when the decayed velocity drops below this. */
static constexpr float GRID_FLING_STOP_VELOCITY = 60.0f;
/** Exponential decay rate: the velocity roughly halves every 0.14 seconds. */
static constexpr float GRID_FLING_DECAY = 5.0f;
/** Fraction of the release velocity actually fed into the coast; halves the total glide distance
 * (still v0/#GRID_FLING_DECAY, just with a smaller v0) without changing the decay rate or the
 * swipe-vs-drag threshold below. */
static constexpr float GRID_FLING_STRENGTH = 0.3;
/** Low-pass time constant for the drag velocity estimate feeding the fling: smooths jittery
 * per-event deltas without lagging a real swipe. */
static constexpr float GRID_DRAG_VELOCITY_LOWPASS_TAU = 0.05f;
/** Max idle since the last velocity sample still counted as "released while moving" (else a pause
 * before release means a positioning drag, not a swipe). */
static constexpr double GRID_DRAG_FLING_RELEASE_MAX_IDLE = 0.1;

/**
 * Where a committed drag sends its scroll. A single drag machine serves every LMB drag-scroll in
 * the UI; the sink is decided once at press (see #grid_drag_sink_for_press) and fixes how the
 * gathered delta is applied:
 * - #Session: a grid with a scroll session (fixed-viewport popovers, embedded core) — writes
 *   #GridSessionState::scroll_px, kinetic fling on release.
 * - #RegionV2D: a plain asset region (permanent asset shelf, asset browser window) — pans the
 *   region's View2D, the job the old `asset_view_drag_scroll_handler` did.
 * - #PopupBlock: the non-grid part of an asset-shelf popover — scrolls the whole popup block.
 * - #TreeView: a tree view that opted in via #AbstractTreeView::set_drag_scroll — writes the
 *   view's own row offset. Row-quantized, because a tree builds no buttons for rows outside its
 *   window and so has no sub-row position to hold.
 *
 * TODO: #TreeView reaches across abstractions. A tree has nothing to do with a grid, and this file
 * is its home only because the single pre-button input handler happens to live here; a future tree
 * wanting drag-scroll has no reason to guess that. It belongs on a shared #AbstractView
 * drag-scroll API, and should move there together with the pixel-accurate tree scrolling that
 * replaces the row quantization.
 */
enum class GridDragSink : int8_t {
  None,
  Session,
  RegionV2D,
  PopupBlock,
  TreeView,
};

/**
 * Touch/pen drag-scroll state. Process-lifetime: a single drag is active at a time, keyed to its
 * grid by \a grid_id (re-resolved to the session per event, robust to the per-refresh rebuild) and
 * pinned to \a region so a stray event in another region never drives it.
 */
struct GridDragState {
  bool active = false;
  bool dragging = false;
  GridDragSink sink = GridDragSink::None;
  /** #GridDragSink::Session only. */
  std::string grid_id;
  const ARegion *region = nullptr;
  int start_x = 0;
  int start_y = 0;
  /** Whether the item under the press supports its own drag (e.g. Favorites reorder); cached at
   * press time so Shift only defers to the item where one is actually present, instead of
   * disabling Shift-drag-to-scroll for every grid. */
  bool pressed_item_supports_drag = false;
  /** #GridDragSink::RegionV2D / #PopupBlock: previous Y for the incremental pan delta. */
  int last_y = 0;
  /** #GridDragSink::Session only: absolute scroll target is measured from this press snapshot. */
  int start_scroll_px = 0;
  /** Content px/sec, low-pass filtered, feeding the kinetic fling on release (Session sink). */
  float velocity = 0.0f;
  double last_time = 0.0;
  int last_px = 0;
  /** #GridDragSink::TreeView only: the view's scroll state, sampled at press because the view
   * object itself does not survive the per-refresh rebuild. */
  TreeViewDragScrollHandle tree;
  /** #GridDragSink::TreeView only: absolute row target is measured from this press snapshot. */
  int start_rows = 0;
};

/** Kinetic fling state; owns the timer directly (no #PopupBlockHandle field) so it works in any
 * region the unified handler runs in. */
struct GridFlingState {
  wmTimer *timer = nullptr;
  wmWindowManager *wm = nullptr;
  wmWindow *window = nullptr;
  /** Non-const: the step tags redraws on it. */
  ARegion *region = nullptr;
  std::string grid_id;
  float velocity = 0.0f;
};

struct GridInputRuntime {
  GridDragState drag;
  GridFlingState fling;
};

static GridInputRuntime &grid_input_runtime()
{
  static GridInputRuntime runtime;
  return runtime;
}

/** Pixel-exact scroll range from the session's per-build geometry snapshot (see
 * #GridStateAccess::geometry_store / #AbstractGridView::store_fixed_viewport_session_geometry). */
static int grid_session_max_scroll_px(const GridSessionState &session)
{
  return grid_max_scroll_px(
      session.cached_item_count, session.cols, session.tile_h, session.viewport_px);
}

/** Block-to-window scale of the region's first block, for the window↔content pixel conversion the
 * drag/trackpad math needs without a concrete button in hand. */
static float grid_region_block_scale(ARegion *region)
{
  Block *block = region ? static_cast<Block *>(region->runtime->uiblocks.first) : nullptr;
  return block ? block_to_window_scale(region, block) : 1.0f;
}

/** Fling teardown; usable without a #bContext (the region may be dying) via the stored
 * window/manager. */
static void grid_fling_stop()
{
  GridFlingState &fling = grid_input_runtime().fling;
  if (fling.timer && fling.wm && fling.window) {
    WM_event_timer_remove(fling.wm, fling.window, fling.timer);
  }
  fling.timer = nullptr;
  fling.wm = nullptr;
  fling.window = nullptr;
  fling.region = nullptr;
  fling.grid_id.clear();
  fling.velocity = 0.0f;
}

static void grid_fling_start(bContext *C,
                             ARegion *region,
                             const std::string &grid_id,
                             const float velocity)
{
  if (fabsf(velocity) < GRID_FLING_START_VELOCITY) {
    return;
  }
  GridFlingState &fling = grid_input_runtime().fling;
  fling.velocity = velocity * GRID_FLING_STRENGTH;
  fling.region = region;
  fling.grid_id = grid_id;
  fling.wm = CTX_wm_manager(C);
  fling.window = CTX_wm_window(C);
  if (fling.timer == nullptr) {
    fling.timer = WM_event_timer_add(fling.wm, fling.window, TIMER, GRID_FLING_TIMER_STEP);
  }
}

/**
 * Which session's overlay scrollbar the cursor is over (drawn outside the grid's own bounds, so
 * #region_view_find_at misses it). Empty when none. Only the embedded host draws a
 * #ButtonType::Scroll widget in this stage; fixed-viewport popovers have no scrollbar widget yet.
 */
static std::string grid_scrollbar_hit(ARegion *region, const int xy[2])
{
  std::string hit_id;
  grid_session_state_foreach([&](const StringRef grid_id, GridSessionState &session) {
    if (session.region != region) {
      return true;
    }
    /* The overlay scrollbar binds directly to #GridSessionState::scroll_px (pixel-scale). */
    if (region_scroll_button_under_mouse(region, xy, &session.scroll_px) &&
        grid_session_max_scroll_px(session) > 0)
    {
      hit_id = grid_id;
      return false;
    }
    return true;
  });
  return hit_id;
}

/**
 * The grid a press lands on, by top-most view item (z-order): a grip/scrollbar drawn over the grid
 * is not a view item, so it keeps the press and is never mistaken for a drag-scroll. Returns the
 * live view and its session key (which may differ from the #ViewLink idname).
 */
static AbstractGridView *grid_hit_press(const ARegion *region,
                                        const wmEvent *event,
                                        std::string *r_grid_id)
{
  AbstractView *hit_view = nullptr;
  region_view_item_topmost_idname_at(region, event, &hit_view);
  AbstractGridView *grid_view = dynamic_cast<AbstractGridView *>(hit_view);
  if (grid_view == nullptr) {
    return nullptr;
  }
  const StringRef grid_id = grid_view->session_grid_id();
  if (grid_id.is_empty()) {
    return nullptr;
  }
  *r_grid_id = grid_id;
  return grid_view;
}

static int grid_handle_fling_timer_event(bContext * /*C*/, const wmEvent *event, ARegion *region)
{
  GridFlingState &fling = grid_input_runtime().fling;
  if (fling.timer == nullptr || event->type != TIMER || event->customdata != fling.timer) {
    return WM_UI_HANDLER_CONTINUE;
  }
  /* Only the region that owns the fling drives it; a TIMER seen in another region is ignored. */
  if (fling.region != region) {
    return WM_UI_HANDLER_CONTINUE;
  }
  GridSessionState &session = grid_session_state_ensure(fling.grid_id);
  const int max_scroll = grid_session_max_scroll_px(session);
  if (session.region != region || max_scroll <= 0) {
    grid_fling_stop();
    return WM_UI_HANDLER_CONTINUE;
  }

  const double dt = std::max(fling.timer->time_delta, 1.0e-4);
  const int total_px = session.scroll_px;
  const int target_px = grid_clamp_scroll_px(total_px + int(round(fling.velocity * dt)), max_scroll);
  fling.velocity *= expf(-float(dt) * GRID_FLING_DECAY);

  if (target_px == total_px) {
    /* Hit a list boundary, or too slow to move a whole pixel: settle. */
    grid_fling_stop();
    return WM_UI_HANDLER_CONTINUE;
  }

  session.scroll_px = target_px;
  ED_region_tag_redraw(fling.region);
  ED_region_tag_refresh_ui(fling.region);

  if (fabsf(fling.velocity) < GRID_FLING_STOP_VELOCITY) {
    grid_fling_stop();
  }
  return WM_UI_HANDLER_BREAK;
}

static int grid_handle_wheel_event(bContext * /*C*/, const wmEvent *event, ARegion *region)
{
  if (!(event->type == WHEELUPMOUSE || event->type == WHEELDOWNMOUSE ||
        event->type == MOUSEPAN) ||
      event->modifier)
  {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Wheel hits by area. Over a non-grid scrollable view (e.g. the catalog tree beside the asset
   * grid) scroll that view by whole steps; over any non-grid view the grid never steals the
   * wheel. */
  std::string grid_id;
  if (AbstractView *hover_view = region_view_find_at(region, event->xy, 0, nullptr)) {
    AbstractGridView *hover_grid = dynamic_cast<AbstractGridView *>(hover_view);
    if (hover_grid == nullptr) {
      if (event->type != MOUSEPAN && hover_view->supports_scrolling() &&
          !hover_view->is_fully_visible())
      {
        hover_view->scroll((event->type == WHEELUPMOUSE) ? ViewScrollDirection::UP :
                                                           ViewScrollDirection::DOWN);
        ED_region_tag_refresh_ui(region);
        return WM_UI_HANDLER_BREAK;
      }
      return WM_UI_HANDLER_CONTINUE;
    }
    grid_id = hover_grid->session_grid_id();
  }
  else {
    grid_id = grid_scrollbar_hit(region, event->xy);
  }

  if (grid_id.empty()) {
    return WM_UI_HANDLER_CONTINUE;
  }
  GridSessionState &session = grid_session_state_ensure(grid_id);
  const int max_scroll = grid_session_max_scroll_px(session);
  if (session.region != region || max_scroll <= 0) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* Manual scrolling takes over from any running fling. */
  grid_fling_stop();

  const int tile_h = std::max(session.tile_h, 1);
  if (event->type == MOUSEPAN) {
    /* Trackpad pan: pixel-exact through the same clamped accumulator as touch dragging.
     * #WM_event_absolute_delta_y already applies the natural-scroll preference; its sign follows
     * wheel semantics (> 0 scrolls towards earlier rows, like #WHEELUPMOUSE). */
    const int dy_window = WM_event_absolute_delta_y(event);
    if (dy_window != 0) {
      const float scale = grid_region_block_scale(region);
      const int dy_content = int(roundf(float(dy_window) / std::max(scale, FLT_EPSILON)));
      const int total_px = grid_clamp_scroll_px(session.scroll_px - dy_content, max_scroll);
      if (total_px != session.scroll_px) {
        session.scroll_px = total_px;
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    }
    return WM_UI_HANDLER_BREAK;
  }

  /* Wheel notch: one whole row; any sub-row offset snaps to the row boundary. */
  const int delta = (event->type == WHEELUPMOUSE) ? -1 : 1;
  const int row = session.scroll_px / tile_h + delta;
  const int total_px = grid_clamp_scroll_px(std::max(row, 0) * tile_h, max_scroll);
  if (total_px != session.scroll_px) {
    session.scroll_px = total_px;
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
  return WM_UI_HANDLER_BREAK;
}

/**
 * Which sink a fresh LMB press should feed, and (for #GridDragSink::Session) the grid it lands on.
 * Ported from the old `asset_view_drag_scroll_handler`'s `region_wants_drag_scroll`: a press over a
 * session-backed grid tile scrolls that grid; otherwise the asset shelf / browser regions and the
 * asset-shelf popover opt into a whole-region / whole-popover scroll. Any other region declines.
 */
static GridDragSink grid_drag_sink_for_press(const bContext *C,
                                             ARegion *region,
                                             const wmEvent *event,
                                             std::string *r_grid_id,
                                             std::optional<TreeViewDragScrollHandle> *r_tree)
{
  if (region == nullptr) {
    return GridDragSink::None;
  }
  /* A press on the overlay scrollbar belongs to the scrollbar widget (thumb drag), not the
   * drag-scroll machine. #grid_hit_press already ignores it for the session sink (the scrollbar is
   * not a view item, so #region_view_item_topmost_idname_at returns nothing), but the region /
   * whole-popover sinks below arm on any press in their region, so decline here first — otherwise
   * grabbing the scrollbar would start a dead touch-scroll instead of dragging the thumb. */
  if (!grid_scrollbar_hit(region, event->xy).empty()) {
    return GridDragSink::None;
  }
  /* A press on a resize/divider grip (popup corner grip, catalog-column divider) or on a
   * non-session scroll bar (e.g. the asset shelf popover's catalog tree, whose bar binds to the
   * view's own scroll value rather than a #GridSessionState, so #grid_scrollbar_hit above misses
   * it) belongs to that widget's own drag, not the drag-scroll machine. Every sink below arms
   * unconditionally on any press in its region/grid, so without this check the
   * whole-popover/whole-region scroll would win the same MOUSEMOVE threshold race as the widget's
   * own drag-start and free its active button first, leaving the widget dead. */
  const Button *but_over = but_find_mouse_over(region, event);
  if (but_over && ELEM(but_over->type, ButtonType::Grip, ButtonType::Scroll)) {
    return GridDragSink::None;
  }
  /* A press landing on a session-backed grid tile scrolls that grid (a grip/scrollbar over the
   * grid is not a view item, so it keeps the press — see #grid_hit_press). */
  if (grid_hit_press(region, event, r_grid_id) != nullptr) {
    return GridDragSink::Session;
  }
  /* A press on a tree view that opted into drag-scrolling (e.g. the asset shelf popover's catalog
   * tree) scrolls that tree rather than the region or popover around it. Probed after
   * #grid_hit_press so a grid tile still wins, and before the region sinks below so that a
   * drag-scrollable tree hosted in an asset shelf or asset browser region is not swallowed by
   * #GridDragSink::RegionV2D before it is ever considered. Hit by area like the wheel path, not by
   * z-order: the decline checkpoints above already handed the widgets their presses. */
  if (AbstractTreeView *tree_view = dynamic_cast<AbstractTreeView *>(
          region_view_find_at(region, event->xy, 0, nullptr)))
  {
    if (std::optional<TreeViewDragScrollHandle> handle = tree_view->drag_scroll_handle()) {
      *r_tree = std::move(handle);
      return GridDragSink::TreeView;
    }
  }
  /* Permanent asset shelf region (e.g. bottom of the 3D viewport): pan its View2D. */
  if (region->regiontype == RGN_TYPE_ASSET_SHELF) {
    return GridDragSink::RegionV2D;
  }
  /* Asset browser main window region: pan its View2D (horizontal drags stay with the button for
   * asset DnD / box select — decided by the arbitration below). */
  if (region->regiontype == RGN_TYPE_WINDOW) {
    const SpaceFile *sfile = CTX_wm_space_file(C);
    if (sfile && ED_fileselect_is_asset_browser(sfile)) {
      return GridDragSink::RegionV2D;
    }
  }
  /* Asset shelf popover: the grid itself was already claimed as #GridDragSink::Session above; a
   * drag starting on the non-grid part (search/preview-size header) scrolls the whole popup block.
   * The panel is drawn inline and never stored in `region->panels`, so match it by block panel
   * name via #region_popup_has_panel (see the old handler for the same lookup rationale). */
  if (region->regiontype == RGN_TYPE_TEMPORARY) {
    if (region_popup_has_panel(region, "ASSETSHELF_PT_popover_panel")) {
      return GridDragSink::PopupBlock;
    }
  }
  return GridDragSink::None;
}

/** Pan \a region's View2D by a window-space \a dy (natural scroll). Ported from the old
 * `asset_view_drag_scroll_handler`. Always returns #WM_UI_HANDLER_BREAK: the active button was
 * already freed at the arbitration commit, so the caller must never dereference its dangling
 * `but`, even on an event that cannot scroll. */
static int grid_drag_apply_region_v2d(ARegion *region, const int dy)
{
  View2D *v2d = &region->v2d;
  if (!(v2d->flag & V2D_IS_INIT)) {
    return WM_UI_HANDLER_BREAK;
  }
  const float region_height = float(BLI_rcti_size_y(&region->winrct) + 1);
  if (region_height < 1.0f) {
    return WM_UI_HANDLER_BREAK;
  }
  const float facy = BLI_rctf_size_y(&v2d->cur) / region_height;
  /* Natural scroll: drag up (dy > 0) shifts cur downward → reveals content below. */
  v2d->cur.ymin -= float(dy) * facy;
  v2d->cur.ymax -= float(dy) * facy;
  view2d_curRect_validate(v2d);
  ED_region_tag_redraw(region);
  ED_region_tag_refresh_ui(region);
  return WM_UI_HANDLER_BREAK;
}

static int grid_handle_drag_scroll_event(bContext *C, const wmEvent *event, ARegion *region)
{
  GridDragState &drag = grid_input_runtime().drag;

  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    /* Reset on any new press so a missed release never leaves stale state. */
    drag = {};
    /* Shift+drag defers to the pressed item's own drag (e.g. Favorites reorder) when it has one;
     * cache that here so the mid-drag arbitration below doesn't need to re-resolve the item, and
     * so grids without such an item keep plain Shift-drag-to-scroll working. */
    if (event->modifier & KM_SHIFT) {
      const AbstractViewItem *hovered_item = region_views_find_item_at(*region, event->xy);
      drag.pressed_item_supports_drag = hovered_item && view_item_supports_drag(*hovered_item);
      if (drag.pressed_item_supports_drag) {
        return WM_UI_HANDLER_CONTINUE;
      }
    }
    std::string grid_id;
    std::optional<TreeViewDragScrollHandle> tree_handle;
    const GridDragSink sink = grid_drag_sink_for_press(C, region, event, &grid_id, &tree_handle);
    if (sink == GridDragSink::Session) {
      GridSessionState &session = grid_session_state_ensure(grid_id);
      if (session.region == region && grid_session_max_scroll_px(session) > 0) {
        /* Catch a flinging list: pressing stops the kinetic scroll (phone UX). */
        grid_fling_stop();
        drag.active = true;
        drag.sink = sink;
        drag.grid_id = grid_id;
        drag.region = region;
        drag.start_x = event->xy[0];
        drag.start_y = event->xy[1];
        drag.start_scroll_px = session.scroll_px;
        drag.velocity = 0.0f;
        drag.last_time = BLI_time_now_seconds();
        drag.last_px = session.scroll_px;
      }
    }
    else if (sink == GridDragSink::RegionV2D || sink == GridDragSink::PopupBlock) {
      drag.active = true;
      drag.sink = sink;
      drag.region = region;
      drag.start_x = event->xy[0];
      drag.start_y = event->xy[1];
      drag.last_y = event->xy[1];
    }
    else if (sink == GridDragSink::TreeView) {
      /* No `last_y` (this sink is absolute, not incremental) and no velocity/fling fields. */
      drag.active = true;
      drag.sink = sink;
      drag.region = region;
      drag.start_x = event->xy[0];
      drag.start_y = event->xy[1];
      drag.tree = std::move(*tree_handle);
      drag.start_rows = *drag.tree.scroll_value;
    }
    /* Always continue so the grid tile / region button still receives the press for
     * click-selection. */
    return WM_UI_HANDLER_CONTINUE;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    if (!drag.active) {
      return WM_UI_HANDLER_CONTINUE;
    }
    const bool was_dragging = drag.dragging;
    const GridDragSink sink = drag.sink;
    const std::string grid_id = drag.grid_id;
    const float velocity = drag.velocity;
    const bool released_moving = (BLI_time_now_seconds() - drag.last_time) <
                                 GRID_DRAG_FLING_RELEASE_MAX_IDLE;
    drag = {};
    if (sink == GridDragSink::Session && was_dragging && released_moving) {
      grid_fling_start(C, region, grid_id, velocity);
    }
    /* Consume the release only after a real drag, to prevent item activation; a tap continues so
     * the button handles selection (crash rule: never BREAK a released-button event we did not
     * take over, see memory touch-scroll-asset-browser). */
    return was_dragging ? WM_UI_HANDLER_BREAK : WM_UI_HANDLER_CONTINUE;
  }

  if (event->type == MOUSEMOVE && drag.active) {
    if (drag.region != region) {
      drag = {};
      return WM_UI_HANDLER_CONTINUE;
    }

    GridSessionState *session = nullptr;
    if (drag.sink == GridDragSink::Session) {
      session = &grid_session_state_ensure(drag.grid_id);
      if (session->refcount == 0 || session->region != region) {
        /* The grid closed or moved regions under us; abandon the drag. */
        drag = {};
        return WM_UI_HANDLER_CONTINUE;
      }
    }

    const float scale = grid_region_block_scale(region);
    /* The exact click-vs-drag threshold #but_drag_init uses (same expression), so the verdict here
     * is reached no later than the button starts a WM drag. */
    const int drag_threshold = min_ii(WM_event_drag_threshold(event),
                                      int((UI_UNIT_Y / 2) * scale));

    if (!drag.dragging) {
      /* Shift+drag: let the pressed item start its own WM drag (e.g. Favorites reorder), same
       * verdict as at press time -- only when that item actually has one. */
      if ((event->modifier & KM_SHIFT) && drag.pressed_item_supports_drag) {
        drag = {};
        return WM_UI_HANDLER_CONTINUE;
      }
      /* Direction-aware arbitration, decided once when the gesture crosses the threshold. The
       * Manhattan metric matches #but_drag_init (`abs(dx) + abs(dy) > threshold`); firing at `>=`
       * makes this handler win the tie, and every dispatch point runs pre-button handlers before
       * #handle_button_event, so a horizontal DnD or box-select never starts before the verdict
       * (no post-hoc #WM_drag_free_list cancel needed). */
      const int total_dx = std::abs(event->xy[0] - drag.start_x);
      const int total_dy = std::abs(event->xy[1] - drag.start_y);
      if (total_dx + total_dy < drag_threshold) {
        return WM_UI_HANDLER_CONTINUE; /* Below threshold: neither scroll nor DnD yet. */
      }
      bool horizontal_gesture = total_dx > total_dy;
      if (region->regiontype == RGN_TYPE_WINDOW) {
        const SpaceFile *sfile = CTX_wm_space_file(C);
        if (sfile && ED_fileselect_is_asset_browser(sfile)) {
          /* Asset Browser DnD commonly starts with a slightly diagonal movement. Give the
           * horizontal intent a small margin over vertical touch scrolling. */
          horizontal_gesture = float(total_dx) >= float(total_dy) * 0.8f;
        }
      }
      if (horizontal_gesture) {
        /* Horizontal-dominant: the gesture belongs to the button (item DnD / box select). */
        drag = {};
        return WM_UI_HANDLER_CONTINUE;
      }
      /* Vertical-dominant: commit to scrolling. Free the active button so no tooltip/DnD starts;
       * from here on every event until release is consumed (BREAK discipline). */
      drag.dragging = true;
      /* Re-anchor the incremental sinks at the commit point so the first applied delta is the next
       * event's small step, not the whole threshold distance in one jump (Session scrolls from an
       * absolute press snapshot and ignores this field). */
      drag.last_y = event->xy[1];
      UI_region_free_active_but_all(C, region);
    }

    if (drag.sink == GridDragSink::Session) {
      /* Phone UX: drag up (dy_total > 0 in Blender Y-up) reveals later rows. Absolute target from
       * the press snapshot; the partially scrolled rows are drawn clipped. */
      const int dy_total = event->xy[1] - drag.start_y;
      const int max_scroll = grid_session_max_scroll_px(*session);
      const int dy_content = int(roundf(float(dy_total) / std::max(scale, FLT_EPSILON)));
      const int total_px = grid_clamp_scroll_px(drag.start_scroll_px + dy_content, max_scroll);
      if (total_px != session->scroll_px) {
        session->scroll_px = total_px;
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }

      /* Velocity sample for the kinetic fling on release, low-pass filtered (~50 ms). */
      const double now = BLI_time_now_seconds();
      const double time_delta = now - drag.last_time;
      if (time_delta > 1.0e-4) {
        const float velocity_sample = float(double(total_px - drag.last_px) / time_delta);
        const float mix = 1.0f - expf(float(-time_delta) / GRID_DRAG_VELOCITY_LOWPASS_TAU);
        drag.velocity += (velocity_sample - drag.velocity) * mix;
        drag.last_time = now;
        drag.last_px = total_px;
      }
      return WM_UI_HANDLER_BREAK;
    }

    if (drag.sink == GridDragSink::TreeView) {
      /* Same phone UX and same absolute-from-snapshot model as the session sink above, but the
       * unit is a whole row: a tree builds no buttons outside its row window, so there is no
       * partial row to draw. */
      const int dy_total = event->xy[1] - drag.start_y;
      const int dy_content = int(roundf(float(dy_total) / std::max(scale, FLT_EPSILON)));
      /* Clamp here rather than leaning on the clamp in #TreeViewLayoutBuilder::build_from_tree: an
       * unclamped write would be corrected in place on every rebuild, so each move while parked at
       * an end would re-tag a refresh for no visible change. Overshoot still costs the reverse
       * stroke the same distance, exactly as it does for the session sink above. */
      const int total_rows = std::clamp(
          drag.start_rows + dy_content / drag.tree.row_height, 0, drag.tree.max_rows);
      if (total_rows != *drag.tree.scroll_value) {
        *drag.tree.scroll_value = total_rows;
        /* Which rows exist as buttons is decided at layout time, so a rebuild is needed, not just a
         * redraw. Same notification the wheel path uses for these views. */
        ED_region_tag_refresh_ui(region);
      }
      return WM_UI_HANDLER_BREAK;
    }

    /* Region / whole-popover sinks: incremental window-space delta since the previous event. */
    const int dy = event->xy[1] - drag.last_y;
    drag.last_y = event->xy[1];
    if (drag.sink == GridDragSink::RegionV2D) {
      return grid_drag_apply_region_v2d(region, dy);
    }
    /* #GridDragSink::PopupBlock: whole-popover scroll (moves the button rects like MMB panning,
     * only effective when the popup overflows the screen). */
    popup_region_scroll_apply_dy(region, float(dy));
    return WM_UI_HANDLER_BREAK;
  }

  return WM_UI_HANDLER_CONTINUE;
}

static int grid_view_pre_button_handler(bContext *C, const wmEvent *event, ARegion *region)
{
  int retval = grid_handle_fling_timer_event(C, event, region);
  if (retval != WM_UI_HANDLER_CONTINUE) {
    return retval;
  }
  retval = grid_handle_wheel_event(C, event, region);
  if (retval != WM_UI_HANDLER_CONTINUE) {
    return retval;
  }
  return grid_handle_drag_scroll_event(C, event, region);
}

void grid_view_register_pre_button_handler()
{
  region_pre_button_handler_add(grid_view_pre_button_handler);
}

void grid_view_input_region_freed(const ARegion *region)
{
  GridInputRuntime &runtime = grid_input_runtime();
  if (runtime.fling.region == region) {
    grid_fling_stop();
  }
  if (runtime.drag.region == region) {
    runtime.drag = {};
  }
}

void UI_grid_view_input_region_freed(const ARegion *region)
{
  grid_view_input_region_freed(region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Fixed-viewport grid scrolling in popups
 *
 * Generic popup/menu event glue driving the fixed-viewport scroll of whichever grid view in the
 * block opted into it (#AbstractGridView::set_fixed_viewport_layout). Found via
 * #block_view_find_fixed_viewport_grid_at_y, so this knows nothing about the concrete view (e.g.
 * the image browser) it scrolls.
 * \{ */

/**
 * Returns false while the mouse is still sitting at (or hasn't yet been recorded away from) the
 * position it had when this popup first opened. #PanelType::offset_units_xy places that initial
 * position over what is assumed to be row 0; #ASSET_SHELF_TYPE_FLAG_CENTER_ACTIVE_ASSET_ON_OPEN
 * can move the first row away from 0 before the user has touched the mouse, which otherwise makes
 * that still, placement-driven cursor look like an edge-hover scroll request and immediately
 * undoes the centering (see #PopupBlockHandle::fixed_grid_autoscroll_gate_released).
 */
static bool fixed_grid_autoscroll_user_moved(PopupBlockHandle &menu, const int my)
{
  if (menu.fixed_grid_autoscroll_gate_released) {
    return true;
  }
  if (!menu.fixed_grid_autoscroll_baseline_set) {
    menu.fixed_grid_autoscroll_baseline_set = true;
    menu.fixed_grid_autoscroll_baseline_my = my;
    return false;
  }
  if (my == menu.fixed_grid_autoscroll_baseline_my) {
    return false;
  }
  menu.fixed_grid_autoscroll_gate_released = true;
  return true;
}

bool popup_block_fixed_grid_autoscroll_at_pointer(PopupBlockHandle *menu,
                                                   Block *block,
                                                   const int my)
{
  if (block == nullptr || menu == nullptr) {
    return false;
  }
  const AbstractGridView *grid_view = block_view_find_fixed_viewport_grid_at_y(*block, float(my));
  if (grid_view == nullptr) {
    return false;
  }
  if (!grid_view->fixed_viewport_scroll_at_y(*block, float(my)).has_value()) {
    return false;
  }
  return fixed_grid_autoscroll_user_moved(*menu, my);
}

bool popup_block_fixed_grid_scrolltimer_step(bContext * /*C*/,
                                             PopupBlockHandle *menu,
                                             Block *block,
                                             const int my)
{
  if (block == nullptr || menu == nullptr || menu->region == nullptr) {
    return false;
  }
  AbstractGridView *grid_view = block_view_find_fixed_viewport_grid_at_y(*block, float(my));
  if (grid_view == nullptr) {
    return false;
  }

  const std::optional<ViewScrollDirection> scroll_dir = grid_view->fixed_viewport_scroll_at_y(
      *block, float(my));
  if (!scroll_dir) {
    return false;
  }
  if (!fixed_grid_autoscroll_user_moved(*menu, my)) {
    return false;
  }

  /* Edge auto-scroll takes over from any running fling, otherwise both timers would fight over
   * the same scroll position. */
  grid_fling_stop();

  /* The scroll position is a row index stored in the grid view; the rebuild triggered below reads
   * it to pick the visible rows. */
  grid_view->scroll(*scroll_dir);
  ED_region_tag_refresh_ui(menu->region);
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
    if (grid_view.scroll_active_into_view_on_build_) {
      grid_view.fixed_viewport_scroll_active_into_view(grid_view.scroll_active_center_on_build_);
      grid_view.scroll_active_into_view_on_build_ = false;
      grid_view.scroll_active_center_on_build_ = false;
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
