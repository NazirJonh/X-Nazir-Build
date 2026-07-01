/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edinterface
 *
 * Public API for the reusable grid view:
 *
 * - #GridDataSource — data provider interface consumed by #build_grid_view.
 * - Pure windowing/scroll math — #grid_build_window_size, #grid_total_rows,
 *   #grid_max_scroll_px, #grid_clamp_scroll_px, #grid_rows_to_build. Isolated so they can be
 *   unit-tested without a GPU/window context.
 * - #GridSessionState — grid_id-keyed session state registry (scroll/grip state outliving the
 *   per-redraw views), implemented in `views/grid_view.cc`.
 * - #GridStateAccess — session UI state interface (grip height, scroll, layout buckets).
 * - #build_grid_view — generic core renderer: tiles + smooth scroll + resize grip +
 *   overlay scrollbar. Knows nothing about View3D / assets / images.
 */

#include <cstdint>
#include <functional>

#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_string_ref.hh"

namespace blender {
struct ARegion;
struct bContext;

namespace ui {

class AbstractGridView;
struct Layout;
struct TooltipData;

/* -------------------------------------------------------------------- */
/** \name GridDataSource — data provider interface
 * \{ */

/**
 * Data-source abstraction for the reusable grid view. The generic core (#build_grid_view) renders,
 * scrolls, and resizes the grid and knows only this interface; concrete sources (asset library,
 * image+texture, Python callback) supply items.
 */
class GridDataSource {
 public:
  virtual ~GridDataSource() = default;

  /** Total filtered item count; drives scrollbar bounds and total grid height. */
  virtual int item_count(const bContext &C) const = 0;

  /**
   * Build #PreviewGridItem objects for the visible window only, by adding them to \a view via
   * #AbstractGridView::add_item(). The window is [window.first(), window.one_after_last()).
   * Building only the visible window keeps per-redraw cost bounded.
   */
  virtual void build_window(const bContext &C, AbstractGridView &view, IndexRange window) = 0;

  /** Item activation, tooltips, and context menus are owned by concrete #PreviewGridItem types. */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pure windowing/scroll math
 * \{ */

/**
 * Number of items to build for the current scroll window: visible rows plus one buffer row, times
 * \a cols, capped at \a max_items. Visible rows are derived from \a grip_px / \a tile_h, clamped
 * to 1..16.
 */
int grid_build_window_size(int grip_px, int tile_h, int cols, int max_items);

/** ceil(item_count / cols); returns \a fallback_rows when there are no items. */
int grid_total_rows(int item_count, int cols, int fallback_rows = 1);

/**
 * Pixel-exact total scroll range: whole content height minus the raw pixel \a viewport_px, so
 * scrolling stops exactly at the content end (revealing a partial bottom row fully) with no
 * over- or under-scroll. 0 when everything fits or the grid is empty.
 */
int grid_max_scroll_px(int item_count, int cols, int tile_h, int viewport_px);

/** Clamp \a scroll_px into [0, max_scroll_px]; a negative \a max_scroll_px behaves like 0. */
int grid_clamp_scroll_px(int scroll_px, int max_scroll_px);

/**
 * Rows a clip window of \a viewport_px height intersects at sub-row offset \a offset_px:
 * `ceil((viewport_px + offset_px) / tile_h)`, at least 1. The single formula behind the
 * "buffer rows" of both grid layout hosts, so the two can not drift apart.
 */
int grid_rows_to_build(int viewport_px, int tile_h, int offset_px);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid session state registry
 * \{ */

struct GridSessionState {
  /* Visible grid height in pixels for the embedded-host resize grip (#ButtonType::Grip).
   * Popover 2D grips persist to DNA instead and do not use this. */
  int grip_pixel_height = 0;
  /* Single source of truth for the scroll position, in content pixels. Whole rows and the
   * sub-row clip offset are derived on demand (`scroll_px / tile_h`, `scroll_px % tile_h`).
   * The #ButtonType::Scroll overlay widget binds directly to this pixel value (pixel-scale). */
  int scroll_px = 0;
  int cached_item_count = 0;
  /* Geometry snapshot, written once per build: stable across rebuilds, so drags and hit-tests
   * never depend on the tiles built this frame. Zero tile_h/cols means the grid was not built
   * yet. */
  int tile_h = 0;
  int cols = 0;
  int viewport_px = 0;
  const ARegion *region = nullptr;
  /* Scroll position remembered per column count, so a grip resize (which only clips, never
   * reflows) or a preview-size change restores the position the user had at that layout. */
  Map<int, int> scroll_px_by_cols;
  /* Number of live views bound to this state; a referenced state is never evicted. */
  int refcount = 0;
  /* LRU stamp, refreshed by #grid_session_state_ensure. */
  uint64_t last_used_seq = 0;
};

/**
 * Return the process-lifetime session state for \a grid_id, creating it on first use. The
 * returned reference has a stable address for the state's whole lifetime; it stays valid at
 * least as long as a caller holds a reference via #grid_session_acquire.
 */
GridSessionState &grid_session_state_ensure(StringRef grid_id);
void grid_session_acquire(GridSessionState &session);
void grid_session_release(GridSessionState &session);

/**
 * Visit every registered session until \a fn returns false. Transitional: only needed while
 * the wheel-latch scrollbar scan identifies the overlay scrollbar widget by the address of a
 * bound #GridSessionState field.
 */
void grid_session_state_foreach(
    FunctionRef<bool(StringRef grid_id, GridSessionState &session)> fn);

/** \} */

/* -------------------------------------------------------------------- */
/** \name GridStateAccess — session UI state interface
 * \{ */

/**
 * Abstracts the per-grid session UI state (grip height, scroll row, sub-row pixel offset, focus,
 * layout-bucket persistence) so the generic core (#build_grid_view) does not depend on #View3D.
 *
 * Stage 2a backs this with the existing View3D state for byte-for-byte identical behavior.
 * Stage 2b swaps the backend to #uiViewState without touching the core.
 */
class GridStateAccess {
 public:
  virtual ~GridStateAccess() = default;

  /* --- Grip (visible grid height in pixels) --- */

  virtual int grip_pixel_height() const = 0;
  virtual void grip_pixel_height_set(int value) = 0;
  /** Raw pointer required by #uiDefIconButV ButtonType::Grip binding. */
  virtual int *grip_pixel_height_ptr() = 0;

  /* --- Scroll position (content pixels; the single source of truth) --- */

  virtual int scroll_px() const = 0;
  virtual void scroll_px_set(int value) = 0;
  /**
   * Stable raw pointer to the pixel scroll position for the #uiDefButV #ButtonType::Scroll binding.
   * The widget is pixel-scale: it writes #scroll_px directly (range [0, max_scroll_px]).
   */
  virtual int *scroll_px_ptr() = 0;

  /* --- Per-frame cached values (shared between thin caller and core) --- */

  virtual int cached_item_count() const = 0;
  virtual int cached_cols() const = 0;
  virtual void cached_cols_set(int value) = 0;

  /* --- Per-column-count scroll persistence --- */

  /**
   * Persist #scroll_px for \a cols after building. Keyed by column count only: a grip resize
   * changes only the visible row count and must keep the current scroll (it re-clips, it does
   * not reflow), while a width/preview-size change genuinely relayouts.
   */
  virtual void store_scroll_for_cols(int cols) = 0;

  /**
   * Store the per-build geometry snapshot the input layer reads. Session-backed states persist
   * it (stable across rebuilds, so drags and hit-tests never depend on the tiles built this
   * frame); states without a session may ignore it.
   */
  virtual void geometry_store(const ARegion * /*region*/,
                              int /*tile_h*/,
                              int /*cols*/,
                              int /*viewport_px*/)
  {
  }

  /* --- DNA fallback for first-frame grip initialization --- */

  /**
   * Returns the whole-row count stored in DNA (or a sensible default) for use only when
   * #grip_pixel_height has not been set yet (first frame / after file reload). Stage 2b
   * derives this from #uiViewState::custom_height instead.
   */
  virtual int effective_rows_dna_fallback() const = 0;

  /* --- Widget callbacks (factories: lambda fires long after scope exits) --- */

  /**
   * Returns a callback stored on the scrollbar widget. Fires when the user drags the scrollbar (the
   * widget already wrote the new #scroll_px directly, pixel-scale); the callback clears any pending
   * focus request and redraws. Implementations re-derive state from #bContext or capture only
   * stable addresses, so no dangling refs.
   */
  virtual std::function<void(bContext &)> make_scroll_widget_fn(int store_cols,
                                                                int store_rows) const = 0;

  /**
   * Returns a callback stored on the grip widget. Fires when the user drags the grip; at minimum
   * redraws the region. The View3D-backed implementation also sends #ND_SPACE_VIEW3D so RNA
   * display properties (e.g. preview-size panel) update.
   */
  virtual std::function<void(bContext &)> make_grip_change_fn() const = 0;

  /* --- Idname --- */

  /** Stable key used as the #AbstractGridView idname and for #uiViewState persistence. */
  virtual StringRef grid_idname() const = 0;
};

/**
 * Generic grid renderer: tiles + smooth scroll + resize grip + overlay scrollbar.
 * Knows nothing about View3D / assets / images.
 *
 * \param view must already be registered via #block_add_view before calling this.
 * \param state provides/persists session UI state (grip height, scroll, layout buckets).
 * \param item_count total filtered item count from the *previous* frame (cached); reaffirmed
 *        inside the view's build_items() during this frame.
 * \param cols_est column estimate derived from the panel width by the caller.
 * \param panel_width layout width in pixels, 0 when unknown.
 */
void build_grid_view(const bContext &C,
                     Layout &layout,
                     AbstractGridView &view,
                     GridStateAccess &state,
                     int item_count,
                     int cols_est,
                     int panel_width);

/** \} */

} /* namespace ui */
} /* namespace blender */
