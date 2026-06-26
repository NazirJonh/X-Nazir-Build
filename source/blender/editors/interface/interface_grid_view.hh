/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edinterface
 *
 * Public API for the reusable grid view:
 *
 * - #GridDataSource — data provider interface consumed by #build_grid_view.
 * - Pure windowing/scroll math — #grid_build_window_size, #grid_total_rows,
 *   #grid_clamp_scroll_row. Isolated so they can be unit-tested without a GPU/window context.
 * - #GridStateAccess — session UI state interface (grip height, scroll, layout buckets).
 * - #build_grid_view — generic core renderer: tiles + smooth scroll + resize grip +
 *   overlay scrollbar. Knows nothing about View3D / assets / images.
 */

#include <functional>

#include "BLI_index_range.hh"
#include "BLI_string_ref.hh"

namespace blender {
struct bContext;

namespace ui {

class AbstractGridView;
class Layout;
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

/** Clamp \a scroll_row into [0, max_scroll_row]. */
int grid_clamp_scroll_row(int scroll_row, int max_scroll_row);

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

  /* --- Scroll row --- */

  virtual int scroll_row() const = 0;
  virtual void scroll_row_set(int value) = 0;
  /** Raw pointer required by #uiDefButV ButtonType::Scroll binding. */
  virtual int *scroll_row_ptr() = 0;

  /* --- Sub-row pixel offset --- */

  virtual int scroll_offset_px() const = 0;
  virtual void scroll_offset_px_set(int value) = 0;

  /* --- Per-frame cached values (shared between thin caller and core) --- */

  virtual int cached_item_count() const = 0;
  virtual void cached_item_count_set(int value) = 0;
  virtual int cached_cols() const = 0;
  virtual void cached_cols_set(int value) = 0;

  /* --- Layout bucket scroll persistence --- */

  /** Persist #scroll_row / #scroll_offset_px after building with (cols, rows). */
  virtual void store_scroll_for_layout(int cols, int rows) = 0;
  /** Clear any pending focus-to-asset request (user manually scrolled). */
  virtual void focus_clear() = 0;

  /* --- DNA fallback for first-frame grip initialization --- */

  /**
   * Returns the whole-row count stored in DNA (or a sensible default) for use only when
   * #grip_pixel_height has not been set yet (first frame / after file reload). Stage 2b
   * derives this from #uiViewState::custom_height instead.
   */
  virtual int effective_rows_dna_fallback() const = 0;

  /* --- Widget callbacks (factories: lambda fires long after scope exits) --- */

  /**
   * Returns a callback stored on the scrollbar widget. Fires when the user moves the scrollbar;
   * must zero sub-row offset, clear focus, and persist the new scroll position to the (cols,
   * rows) layout bucket. The implementation re-derives state from #bContext so no dangling refs.
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

}  /* namespace ui */
}  /* namespace blender */
