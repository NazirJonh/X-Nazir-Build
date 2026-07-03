/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Generic grid renderer: tiles + smooth scroll + resize grip + overlay scrollbar.
 * Knows nothing about View3D / assets / images. All UI state is accessed via #GridStateAccess
 * and all windowed math via interface_grid_view.hh.
 */

#include "interface_grid_view.hh"
#include "interface_grid_view_settings_utils.hh"
#include "interface_grid_view_sources.hh"
#include "interface_intern.hh"

#include "DNA_view2d_types.h"

#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_sys_types.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "ED_asset_list.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ui {

namespace {

constexpr int GRID_MAX_ITEMS = 512;

/* -------------------------------------------------------------------- */
/** \name Generic grid view + session state (grid_id-keyed, process-lifetime)
 * \{ */

/**
 * Session state for a #GenericGridView, keyed by its `grid_id`. #GenericGridView itself is
 * rebuilt from scratch on every redraw like all views, so grip height / scroll position / item
 * count need to live somewhere that outlives it. #uiViewState (#AbstractView::persistent_state())
 * doesn't work for this: it only round-trips through #block_views_end() /
 * #block_view_persistent_state_restore(), which save/restore at the start/end of building a
 * block's layout — not while a #ButtonType::Grip / #ButtonType::Scroll button is live-dragging
 * the bound pointer. The block being dragged already finished its own save before the drag
 * started, so the drag's writes are never captured, and the very next redraw restores the
 * pre-drag value, making the grip/scrollbar appear unresponsive. Keeping the state in a registry
 * keyed by grid_id sidesteps the save/restore round-trip entirely: the drag and the next
 * redraw's read both go through the same storage.
 *
 * Keyed by `grid_id`: two draw sites passing the *same* grid_id deliberately share one
 * scroll/grip state, so callers wanting independent grids must pass distinct, globally-unique
 * grid_id strings (spelled out in the docstrings of #template_grid_view_asset /
 * #template_grid_view_custom). Entries persist for the process lifetime — there is no natural
 * per-grid teardown signal — so the count of distinct grid_ids used in a session is what bounds
 * the registry's size. See #GenericGridRuntime for the single storage location.
 */
struct GenericGridSessionState {
  int grip_pixel_height = 0;
  int scroll_row = 0;
  int scroll_offset_px = 0;
  int cached_item_count = 0;
  /* Number of live #GenericGridView instances bound to this grid_id. The registry has no owner to
   * free its entries (grid_id keys may be minted dynamically), so a session is reclaimed once no
   * view references it. A displayed grid always keeps a view here, so refcount stays >= 1 and the
   * session is never swept while live. See #generic_grid_sessions_tick_and_sweep. */
  int block_refcount = 0;
  /* #GenericGridRuntime::rebuild_seq captured when #block_refcount last dropped to 0, used to age
   * the entry out only after it stays unreferenced past the sweep grace window. */
  uint64_t orphaned_at_seq = 0;
};

/* Defined together with the rest of the process-lifetime grid runtime (#GenericGridRuntime),
 * once the drag/wheel state types it is grouped with are also declared, below. */
static GenericGridSessionState &generic_grid_session_state_ensure(StringRef grid_id);
static Map<std::string, std::unique_ptr<GenericGridSessionState>> &generic_grid_sessions();
/* Refcount a session by the lifetime of the #GenericGridView instances bound to it (see the
 * #GenericGridSessionState::block_refcount docstring). Defined with the runtime, below. */
static void generic_grid_session_view_acquire(GenericGridSessionState &session);
static void generic_grid_session_view_release(GenericGridSessionState &session);

class GenericGridView : public AbstractGridView {
  const bContext &context_;
  std::unique_ptr<GridDataSource> source_;
  int cols_hint_ = 1;
  GenericGridSessionState &session_;

 public:
  GenericGridView(const bContext &context,
                  std::unique_ptr<GridDataSource> source,
                  const int cols_hint,
                  const StringRef grid_id)
      : context_(context),
        source_(std::move(source)),
        cols_hint_(max_ii(1, cols_hint)),
        session_(generic_grid_session_state_ensure(grid_id))
  {
    generic_grid_session_view_acquire(session_);
  }

  /* Virtual via #AbstractView::~AbstractView; runs when #block_free_views drops the owning
   * #ViewLink, which is the teardown signal the session refcount is keyed on. */
  ~GenericGridView()
  {
    generic_grid_session_view_release(session_);
  }

  int grip_pixel_height() const
  {
    return session_.grip_pixel_height;
  }
  void grip_pixel_height_set(const int value)
  {
    session_.grip_pixel_height = value;
  }
  int &grip_pixel_height_mut()
  {
    return session_.grip_pixel_height;
  }

  int scroll_row() const
  {
    return session_.scroll_row;
  }
  void scroll_row_set(const int value)
  {
    session_.scroll_row = value;
  }
  int &scroll_row_mut()
  {
    return session_.scroll_row;
  }

  int scroll_offset_px() const
  {
    return session_.scroll_offset_px;
  }
  void scroll_offset_px_set(const int value)
  {
    session_.scroll_offset_px = value;
  }

  int cached_item_count() const
  {
    return session_.cached_item_count;
  }

  void build_items() override
  {
    const int cols = cols_hint_;
    const int tile_h = max_ii(1, get_style().tile_height);
    const int item_window = grid_build_window_size(
        session_.grip_pixel_height, tile_h, cols, GRID_MAX_ITEMS);
    const int first_index = session_.scroll_row * cols;
    const IndexRange window(first_index, item_window);

    session_.cached_item_count = source_->item_count(context_);
    source_->build_window(context_, *this, window);
  }

  int get_cached_item_count_for_build() const
  {
    return session_.cached_item_count;
  }
};

/** Forwards #GridStateAccess to a #GenericGridView's session members. */
class ViewGridStateAccess : public GridStateAccess {
  GenericGridView &view_;
  std::string idname_;

 public:
  ViewGridStateAccess(GenericGridView &view, std::string idname)
      : view_(view), idname_(std::move(idname))
  {
  }

  int grip_pixel_height() const override
  {
    return view_.grip_pixel_height();
  }
  void grip_pixel_height_set(const int value) override
  {
    view_.grip_pixel_height_set(value);
  }
  int *grip_pixel_height_ptr() override
  {
    return &view_.grip_pixel_height_mut();
  }

  int scroll_row() const override
  {
    return view_.scroll_row();
  }
  void scroll_row_set(const int value) override
  {
    view_.scroll_row_set(value);
  }
  int *scroll_row_ptr() override
  {
    return &view_.scroll_row_mut();
  }

  int scroll_offset_px() const override
  {
    return view_.scroll_offset_px();
  }
  void scroll_offset_px_set(const int value) override
  {
    view_.scroll_offset_px_set(value);
  }

  int cached_item_count() const override
  {
    return view_.get_cached_item_count_for_build();
  }
  void cached_item_count_set(const int /*value*/) override {}

  int cached_cols() const override
  {
    return view_.cols_per_row();
  }
  void cached_cols_set(const int /*value*/) override {}

  void store_scroll_for_layout(const int /*cols*/, const int /*rows*/) override {}
  void focus_clear() override {}

  int effective_rows_dna_fallback() const override
  {
    const int tile_h = max_ii(1, view_.get_style().tile_height);
    return clamp_i(
        int(divide_ceil_u(uint(max_ii(view_.grip_pixel_height(), tile_h)), uint(tile_h))), 1, 16);
  }

  std::function<void(bContext &)> make_scroll_widget_fn(const int /*store_cols*/,
                                                        const int /*store_rows*/) const override
  {
    return [](bContext &C) {
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  std::function<void(bContext &)> make_grip_change_fn() const override
  {
    return [](bContext &C) {
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  StringRef grid_idname() const override
  {
    return idname_;
  }
};

static void grid_view_block_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  switch (wmn->category) {
    case NC_ASSET:
      if (ELEM(wmn->data,
               int(ND_ASSET_LIST),
               int(ND_ASSET_LIST_READING),
               int(ND_ASSET_LIST_PREVIEW)))
      {
        ED_region_tag_redraw(params->region);
        if (wmn->data != int(ND_ASSET_LIST_PREVIEW)) {
          ED_region_tag_refresh_ui(params->region);
        }
      }
      break;
    default:
      break;
  }
}

/* -------------------------------------------------------------------- */
/** \name Touch/pen drag-scroll + mouse-wheel scroll (grid_id-agnostic)
 * \{ */

/**
 * Pen/tablet drag-to-scroll gesture state. One physical pointer device drags at most one grid at
 * a time, so a single process-lifetime instance (not grid_id-keyed) is enough; #grid_id records
 * which session the active gesture applies to. Mirrors View3D's #ImageGridDragScrollState
 * (`ED_view3d.hh`), generalized to an arbitrary grid_id instead of a `is_mask_slot` bool.
 */
struct GenericGridDragScrollState {
  bool active = false;
  bool dragging = false;
  std::string grid_id;
  /** Press position; used to re-locate the still-live view on every #MOUSEMOVE without depending
   * on the current cursor position, which may drift outside the grid's bounds while dragging. */
  int anchor_xy[2] = {0, 0};
  int start_y = 0;
  int last_y = 0;
};

/**
 * Latches the last wheel hit-test decision so a transient rebuild does not break a scroll burst.
 * A grid view's hit bounds span only the tiles built this frame and momentarily vanish during a
 * fast-scroll rebuild; without this, the wheel event for that frame leaks to the region's own
 * View2D pan, and every following event leaks too (see project memory
 * project-image-grid-wheel-cascade — the same failure mode View3D's #ImageGridWheelLatch guards
 * against). Mirrors that struct, generalized to an arbitrary grid_id.
 */
struct GenericGridWheelLatch {
  bool over = false;
  std::string grid_id;
  /** Region the latch belongs to; a stale latch must never consume a wheel event in an unrelated
   * editor. */
  const ARegion *region = nullptr;
  int xy[2] = {0, 0};
  /** Tile height / column count last seen while #over was true. A vanished-mid-rebuild grid has
   * no live view to re-read these from, and the column count additionally has no equivalent on
   * View3D's struct because View3D persists columns in DNA-backed #ImageGridUIState; the generic
   * session has no such persistent field, so both hints are cached here instead. */
  int tile_h_hint = 0;
  int cols_hint = 1;
};

/**
 * Single home for all process-lifetime state behind the generic grid views. Grouped so future
 * additions extend this struct instead of scattering more file-scope statics (the touch/wheel
 * layer already sits outside the standard #uiViewState view persistence, so keeping its state
 * contained keeps that departure auditable). Holds:
 * - #sessions: per-`grid_id` scroll/grip state outliving the per-redraw #GenericGridView
 *   (see #GenericGridSessionState). A #unique_ptr value keeps each state at a fixed address, so a
 *   #ButtonType::Grip / #ButtonType::Scroll bound to it from an earlier redraw stays valid even
 *   when #Map rehashes on inserting a *different* grid_id.
 * - #drag: the one in-flight touch/pen drag gesture (#GenericGridDragScrollState).
 * - #wheel: the last wheel hit-test decision (#GenericGridWheelLatch).
 * - #rebuild_seq: logical clock advanced once per grid build, used to age unreferenced #sessions
 *   out (see #generic_grid_sessions_tick_and_sweep). It only moves while grids are being built, so
 *   an idle-but-open popup never ages its session.
 */
struct GenericGridRuntime {
  Map<std::string, std::unique_ptr<GenericGridSessionState>> sessions;
  GenericGridDragScrollState drag;
  GenericGridWheelLatch wheel;
  uint64_t rebuild_seq = 0;
};

static GenericGridRuntime &generic_grid_runtime()
{
  static GenericGridRuntime runtime;
  return runtime;
}

static Map<std::string, std::unique_ptr<GenericGridSessionState>> &generic_grid_sessions()
{
  return generic_grid_runtime().sessions;
}

static GenericGridSessionState &generic_grid_session_state_ensure(const StringRef grid_id)
{
  Map<std::string, std::unique_ptr<GenericGridSessionState>> &sessions = generic_grid_sessions();
  std::unique_ptr<GenericGridSessionState> &slot = sessions.lookup_or_add_cb(
      std::string(grid_id), [] { return std::make_unique<GenericGridSessionState>(); });
  return *slot;
}

static void generic_grid_session_view_acquire(GenericGridSessionState &session)
{
  session.block_refcount++;
}

static void generic_grid_session_view_release(GenericGridSessionState &session)
{
  BLI_assert(session.block_refcount > 0);
  session.block_refcount--;
  if (session.block_refcount == 0) {
    /* Not evicted immediately: a redraw frees the old view before (or after) building the new one
     * for the same grid_id, so a momentary refcount == 0 is normal rebuild churn. The sweep waits
     * a grace window to tell that gap apart from a grid that is truly gone. */
    session.orphaned_at_seq = generic_grid_runtime().rebuild_seq;
  }
}

/** Grace window (in #GenericGridRuntime::rebuild_seq ticks) before an unreferenced session is
 * reclaimed. Must exceed the number of distinct grids built within a single redraw pass so a grid
 * whose old view is freed before its new view is built this frame is not evicted in that gap; a
 * handful of simultaneous scrollable grids is the realistic maximum, so this is comfortably
 * generous while still bounding the registry for dynamically-minted grid_ids. */
static constexpr uint64_t GRID_SESSION_SWEEP_GRACE = 8;

/**
 * Advance the registry clock and drop sessions no live #GenericGridView references any more. Called
 * once per grid build. Safe because a displayed grid always holds a view (refcount >= 1) and is
 * never a candidate; only entries orphaned past #GRID_SESSION_SWEEP_GRACE ticks are removed.
 */
static void generic_grid_sessions_tick_and_sweep()
{
  GenericGridRuntime &runtime = generic_grid_runtime();
  const uint64_t seq = ++runtime.rebuild_seq;
  runtime.sessions.remove_if([seq](auto item) {
    const GenericGridSessionState &session = *item.value;
    return session.block_refcount == 0 &&
           (seq - session.orphaned_at_seq) >= GRID_SESSION_SWEEP_GRACE;
  });
}

static GenericGridDragScrollState &generic_grid_drag_scroll_state()
{
  return generic_grid_runtime().drag;
}

static GenericGridWheelLatch &generic_grid_wheel_latch()
{
  return generic_grid_runtime().wheel;
}

/** Effective visible row count for a given grip height and tile height — same clamp #build_grid_view()
 * applies, parameterized so it works whether or not a live #GenericGridView is available. */
static int generic_grid_effective_rows(const int grip_px, const int tile_h)
{
  const int safe_tile_h = max_ii(1, tile_h);
  return clamp_i(int(divide_ceil_u(uint(max_ii(grip_px, safe_tile_h)), uint(safe_tile_h))), 1, 16);
}

/** Same max-scroll-row formula #build_grid_view() already uses, not a re-derived copy that could
 * drift. Takes \a cols / \a tile_h as parameters (rather than reading them off a live view)
 * because the wheel-latch fallback (see below) has no live #GenericGridView to query. */
static int generic_grid_max_scroll_row(const GenericGridSessionState &session,
                                       const int cols,
                                       const int tile_h)
{
  const int effective_rows = generic_grid_effective_rows(session.grip_pixel_height, tile_h);
  const int total_rows = grid_total_rows(
      session.cached_item_count, max_ii(1, cols), effective_rows);
  return max_ii(0, total_rows - effective_rows);
}

static int generic_grid_handle_drag_scroll_event(bContext * /*C*/,
                                                 const wmEvent *event,
                                                 ARegion *region)
{
  GenericGridDragScrollState &drag = generic_grid_drag_scroll_state();

  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    /* Reset on any new LMB press so a missed release never leaves stale state. */
    drag = {};
    /* Start a touch-drag only when the press lands directly on a grid tile. The window manager's
     * top-most hit test means a widget drawn over the grid (the overlay scrollbar) or below it
     * (the resize grip) keeps the press by z-order, so moving the bar or resizing the grid is
     * never mistaken for a drag-scroll. */
    AbstractView *hit_view = nullptr;
    const StringRef idname = region_view_item_topmost_idname_at(region, event, &hit_view);
    if (GenericGridView *grid_view = dynamic_cast<GenericGridView *>(hit_view)) {
      GenericGridSessionState &session = generic_grid_session_state_ensure(idname);
      const int tile_h = max_ii(1, grid_view->get_style().tile_height);
      const int cols = max_ii(1, grid_view->cols_per_row());
      if (generic_grid_max_scroll_row(session, cols, tile_h) > 0) {
        drag.active = true;
        drag.grid_id = idname;
        drag.anchor_xy[0] = event->xy[0];
        drag.anchor_xy[1] = event->xy[1];
        drag.start_y = event->xy[1];
        drag.last_y = event->xy[1];
      }
    }
    /* Always continue so grid items still receive the press for click-selection. */
    return WM_UI_HANDLER_CONTINUE;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    if (drag.active) {
      const bool was_dragging = drag.dragging;
      drag = {};
      /* Consume the release after a drag to prevent item activation. */
      return was_dragging ? WM_UI_HANDLER_BREAK : WM_UI_HANDLER_CONTINUE;
    }
    return WM_UI_HANDLER_CONTINUE;
  }

  if (event->type == MOUSEMOVE && drag.active) {
    const int dy = event->xy[1] - drag.last_y;
    drag.last_y = event->xy[1];

    if (!drag.dragging && abs(event->xy[1] - drag.start_y) >= 8) {
      /* Enter drag mode once the cursor travels more than 8 px from the press origin. */
      drag.dragging = true;
    }

    if (drag.dragging) {
      /* Re-locate the view by the press position rather than the current (possibly-drifted)
       * cursor position: the grid's on-screen bounds do not move while its content scrolls. */
      AbstractView *view = region_view_find_at(region, drag.anchor_xy, 0, nullptr, nullptr);
      GenericGridView *grid_view = dynamic_cast<GenericGridView *>(view);
      if (!grid_view) {
        /* Mid-rebuild this frame; skip, retry on the next MOUSEMOVE. */
        return WM_UI_HANDLER_BREAK;
      }

      GenericGridSessionState &session = generic_grid_session_state_ensure(drag.grid_id);
      const int tile_h = max_ii(1, grid_view->get_style().tile_height);
      const int cols = max_ii(1, grid_view->cols_per_row());
      const int max_scroll_px = generic_grid_max_scroll_row(session, cols, tile_h) * tile_h;

      /* Phone UX: drag up (dy > 0 in Blender Y-up coords) -> content scrolls up -> later rows
       * appear. Scroll by sub-row pixel amounts for smooth motion: accumulate into a combined
       * pixel position and split it back into whole rows plus a sub-row offset. */
      int total_px = session.scroll_row * tile_h + session.scroll_offset_px;
      total_px = clamp_i(total_px + dy, 0, max_scroll_px);
      session.scroll_row = total_px / tile_h;
      session.scroll_offset_px = total_px % tile_h;

      ED_region_tag_redraw(region);
      ED_region_tag_refresh_ui(region);
      return WM_UI_HANDLER_BREAK;
    }
  }

  return WM_UI_HANDLER_CONTINUE;
}

static bool generic_grid_wheel_poll(const ARegion *region,
                                    const wmEvent *event,
                                    std::string *r_grid_id)
{
  if (!ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE) || event->modifier) {
    return false;
  }

  GenericGridWheelLatch &latch = generic_grid_wheel_latch();

  StringRef idname;
  AbstractView *view = region_view_find_at(region, event->xy, 0, nullptr, &idname);
  GenericGridView *grid_view = dynamic_cast<GenericGridView *>(view);

  std::string grid_id;
  int tile_h_hint = 0;
  int cols_hint = 0;
  bool over = false;

  if (grid_view) {
    grid_id = idname;
    tile_h_hint = max_ii(1, grid_view->get_style().tile_height);
    cols_hint = max_ii(1, grid_view->cols_per_row());
    over = true;
  }
  else {
    /* Bounds-based lookup missed (cursor may be over the overlay scrollbar, which is drawn
     * outside the grid's own bounds); iterate live sessions for "over the scrollbar" parity with
     * View3D's #image_grid_scroll_under_mouse. */
    for (auto item : generic_grid_sessions().items()) {
      if (region_scroll_button_under_mouse(region, event->xy, &item.value->scroll_row)) {
        grid_id = item.key;
        over = true;
        /* The scrollbar hit test alone carries no tile/column info. A scrollbar is only ever
         * drawn when its grid already has scrollable content, so borrow the latch's hint for the
         * same grid when available; a fresh hit with no prior hint is the rare case of grabbing
         * the scrollbar before ever hovering a tile. */
        if (latch.grid_id == grid_id) {
          tile_h_hint = latch.tile_h_hint;
          cols_hint = latch.cols_hint;
        }
        break;
      }
    }
  }

  if (over) {
    latch.over = true;
    latch.grid_id = grid_id;
    latch.region = region;
    latch.xy[0] = event->xy[0];
    latch.xy[1] = event->xy[1];
    if (tile_h_hint > 0) {
      latch.tile_h_hint = tile_h_hint;
      latch.cols_hint = max_ii(1, cols_hint);
    }
  }
  else {
    /* Live bounds say "not over". Repeat the last consume decision only while the cursor held
     * still, and only for the region the latch belongs to. */
    bool consume_via_latch = false;
    if (latch.region == region && latch.over) {
      const int tile_h = max_ii(1, latch.tile_h_hint);
      const bool held_still = abs(event->xy[0] - latch.xy[0]) <= tile_h &&
                              abs(event->xy[1] - latch.xy[1]) <= tile_h;
      if (held_still) {
        consume_via_latch = true;
      }
      else {
        latch.over = false;
      }
    }
    if (!consume_via_latch) {
      return false;
    }
    grid_id = latch.grid_id;
    tile_h_hint = latch.tile_h_hint;
    cols_hint = latch.cols_hint;
  }

  if (grid_id.empty()) {
    return false;
  }

  const GenericGridSessionState &session = generic_grid_session_state_ensure(grid_id);
  if (generic_grid_max_scroll_row(session, max_ii(1, cols_hint), max_ii(1, tile_h_hint)) <= 0) {
    return false;
  }

  *r_grid_id = grid_id;
  return true;
}

static int generic_grid_handle_wheel_event(bContext * /*C*/, const wmEvent *event, ARegion *region)
{
  std::string grid_id;
  if (!generic_grid_wheel_poll(region, event, &grid_id)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  GenericGridSessionState &session = generic_grid_session_state_ensure(grid_id);
  const int delta = (event->type == WHEELUPMOUSE) ? -1 : 1;
  session.scroll_row = max_ii(0, session.scroll_row + delta);
  session.scroll_offset_px = 0;

  ED_region_tag_redraw(region);
  ED_region_tag_refresh_ui(region);
  return WM_UI_HANDLER_BREAK;
}

static int generic_grid_view_pre_button_handler(bContext *C, const wmEvent *event, ARegion *region)
{
  const int wheel_retval = generic_grid_handle_wheel_event(C, event, region);
  if (wheel_retval != WM_UI_HANDLER_CONTINUE) {
    return wheel_retval;
  }
  return generic_grid_handle_drag_scroll_event(C, event, region);
}

/** \} */

}  // namespace

void build_grid_view(const bContext &C,
                     Layout &layout,
                     AbstractGridView &view,
                     GridStateAccess &state,
                     const int item_count,
                     const int cols_est,
                     const int panel_width)
{
  /* Advance the session-registry clock and reclaim entries no live grid still references, so the
   * grid_id-keyed registry stays bounded even when grid_ids are minted dynamically. */
  generic_grid_sessions_tick_and_sweep();

  Block *block = layout.block();

  const GridViewStyle &style = view.get_style();
  const int tile_h = style.tile_height;

  /* Reconstruct grip height from the DNA row count on the first frame (grip == 0 when unset) or
   * after file reload. This is the only place that reads the fallback so the core stays neutral
   * about storage. */
  if (state.grip_pixel_height() < tile_h) {
    state.grip_pixel_height_set(state.effective_rows_dna_fallback() * tile_h);
  }

  /* Clamp the raw grip to the 1..16-row range for display; preserve the raw value so a temporary
   * preview-size change does not permanently shrink a height the user set at a smaller tile. */
  const int visible_height = clamp_i(state.grip_pixel_height(), tile_h, 16 * tile_h);
  const int effective_rows = clamp_i(
      int(divide_ceil_u(uint(visible_height), uint(tile_h))), 1, 16);

  /* Compute total rows from the previous-frame item count (updated inside build_items this frame).
   * Falls back to effective_rows when the grid is empty so the grip does not collapse. */
  const int total_rows = grid_total_rows(item_count, cols_est, effective_rows);
  const int max_scroll = grid_clamp_scroll_row(max_ii(0, total_rows - effective_rows),
                                               max_ii(0, total_rows - effective_rows));
  state.scroll_row_set(grid_clamp_scroll_row(state.scroll_row(), max_scroll));

  /* Sub-row offset: pin to a whole-row boundary on the last row (nothing below to reveal). */
  if (state.scroll_row() >= max_scroll) {
    state.scroll_offset_px_set(0);
  }
  else {
    state.scroll_offset_px_set(clamp_i(state.scroll_offset_px(), 0, tile_h - 1));
  }
  const int scroll_offset_px = state.scroll_offset_px();

  /* --- Layout construction --- */

  Layout &outer_row = layout.row(false);
  Layout &grid_layout = outer_row.column(true);
  grid_layout.fixed_size_set(true);
  if (panel_width > 0) {
    grid_layout.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_layout.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

  Layout &grid_stack = grid_layout.overlap();
  grid_stack.fixed_size_set(true);
  if (panel_width > 0) {
    grid_stack.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_stack.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

  const int grid_width = (panel_width > 0) ? panel_width : max_ii(style.tile_width, 1);
  const int total_height = max_ii(visible_height, total_rows * tile_h);

  /* Rows to mark visible (and build) beyond the clipped window. One buffer row covers a window
   * height that is not an exact multiple of #tile_h. A sub-row #scroll_offset_px shifts content
   * up, so the window then intersects one *more* row at the bottom; without a second buffer row
   * that partial bottom row falls outside #BuildOnlyVisibleButtonsHelper's range and vanishes
   * entirely instead of being drawn clipped (most visible during touch/drag scroll). */
  const int buffer_rows = (scroll_offset_px > 0) ? 2 : 1;

  View2D local_v2d{};
  local_v2d.flag |= V2D_IS_INIT;
  local_v2d.tot.xmin = 0.0f;
  local_v2d.tot.xmax = float(grid_width);
  local_v2d.tot.ymin = float(-total_height);
  local_v2d.tot.ymax = 0.0f;
  local_v2d.cur.xmin = 0.0f;
  local_v2d.cur.xmax = float(grid_width);
  /* Keep cur at top of content so BuildOnlyVisibleButtonsHelper starts from item_idx 0.
   * The windowed build_items already offsets by scroll_row*cols; re-offsetting here would
   * cause a double-skip and show the wrong items. */
  const int build_height = visible_height + buffer_rows * tile_h;
  local_v2d.cur.ymin = float(-build_height);
  local_v2d.cur.ymax = 0.0f;
  BLI_rcti_init(&local_v2d.mask, 0, grid_width, -build_height, 0);

  Layout &grid_view_col = grid_stack.column(true);
  grid_view_col.fixed_size_set(true);
  if (panel_width > 0) {
    grid_view_col.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_view_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
  grid_view_col.view_scroll_clip_set(visible_height, scroll_offset_px);

  GridViewBuilder builder(*block);
  builder.build_grid_view(C, view, grid_view_col, "", &local_v2d);

  /* Update cached cols from the actual column count reported by the view after build. */
  const int actual_cols = view.cols_per_row();
  state.cached_cols_set(actual_cols);

  /* #item_count is last frame's count, used above before #build_items() (inside
   * #builder.build_grid_view()) re-affirmed the real, current count. Using the stale parameter
   * here instead of #state.cached_item_count() would under/over-count on any frame where the
   * item count actually changed, most visibly as a scrollbar that never appears because the
   * stale count it starts from (0, for a freshly built view whose GridStateAccess has no prior
   * frame to read from, e.g. #ViewGridStateAccess) never reflects real content. */
  const int total_rows_post = grid_total_rows(state.cached_item_count(), actual_cols, effective_rows);
  const int max_scroll_post = max_ii(0, total_rows_post - effective_rows);
  state.scroll_row_set(grid_clamp_scroll_row(state.scroll_row(), max_scroll_post));
  state.store_scroll_for_layout(actual_cols, effective_rows);

  /* --- Overlay scrollbar (does not steal grid width) --- */

  if (max_scroll_post > 0) {
    Layout &scroll_anchor = grid_stack.row(false);
    scroll_anchor.alignment_set(LayoutAlign::Right);
    Layout &scroll_col = scroll_anchor.column(false);
    scroll_col.fixed_size_set(true);
    scroll_col.ui_units_x_set(float(V2D_SCROLL_WIDTH) / float(UI_UNIT_X));
    scroll_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

    block_layout_set_current(block, &scroll_col);
    Button *but = uiDefButV(block,
                            ButtonType::Scroll,
                            "",
                            0,
                            0,
                            short(V2D_SCROLL_WIDTH),
                            visible_height,
                            state.scroll_row_ptr(),
                            0.0f,
                            float(max_scroll_post),
                            "");
    auto *but_scroll = reinterpret_cast<ButtonScrollBar *>(but);
    but_scroll->visual_height = float(effective_rows);
    uchar scroll_track_bg[4];
    theme::get_color_4ubv(TH_BACK, scroll_track_bg);
    scroll_track_bg[3] = 255;
    button_color_set(but, scroll_track_bg);
    button_flag_disable(but, BUT_UNDO);
    button_func_set(but, state.make_scroll_widget_fn(actual_cols, effective_rows));
    block_layout_set_current(block, &layout);
  }

  /* --- Resize grip --- */

  Layout &grip_row = layout.row(false);
  grip_row.scale_x_set(1.0f);
  block_layout_set_current(block, &grip_row);
  Button *grip_but = uiDefIconButV(block,
                                   ButtonType::Grip,
                                   ICON_GRIP,
                                   0,
                                   0,
                                   short(max_ii(panel_width, int(UI_UNIT_X * 10))),
                                   short(UI_UNIT_Y * 0.5f),
                                   state.grip_pixel_height_ptr(),
                                   0.0f,
                                   0.0f,
                                   "");
  button_flag_disable(grip_but, BUT_UNDO);
  button_func_set(grip_but, state.make_grip_change_fn());
  block_layout_set_current(block, &layout);
}

void template_grid_view_asset(Layout *layout,
                              bContext *C,
                              const char *grid_id,
                              PointerRNA *settings_ptr,
                              const char *activate_operator,
                              const char *drag_operator)
{
  if (!layout || !C || !grid_id || !grid_id[0] || !settings_ptr || !settings_ptr->data) {
    return;
  }

  Block *block = layout->block();

  const AssetLibraryReference lib_ref = grid_settings::library_ref_get(*settings_ptr);
  Set<std::string> catalogs = grid_settings::enabled_catalogs_get(*settings_ptr);
  Set<short> filter_id_types = grid_settings::filter_id_types_get(*settings_ptr);
  const int preview_size = grid_settings::preview_size_get(*settings_ptr);

  const int tile_w = preview_tile_size_x(preview_size);
  const int tile_h = preview_tile_size_y_no_label(preview_size);
  const int panel_width = max_ii(layout->width(), 0);
  const int cols_est = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tile_w, 1)) : 1;

  auto source = std::make_unique<AssetGridDataSource>(lib_ref,
                                                      std::move(catalogs),
                                                      std::move(filter_id_types),
                                                      activate_operator ? activate_operator : "",
                                                      drag_operator ? drag_operator : "");

  auto view_unique = std::make_unique<GenericGridView>(*C, std::move(source), cols_est, grid_id);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tile_w, tile_h);
  view_unique->set_cols_per_row_hint(cols_est);

  AbstractGridView *grid_view = block_add_view(*block, grid_id, std::move(view_unique));

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, grid_view_block_listener);

  ViewGridStateAccess state_access(*view_ptr, grid_id);
  build_grid_view(*C,
                  *layout,
                  *grid_view,
                  state_access,
                  view_ptr->get_cached_item_count_for_build(),
                  cols_est,
                  panel_width);
}

void template_grid_view_custom(Layout *layout,
                               bContext *C,
                               const char *grid_id,
                               const char *gridtype_name,
                               PointerRNA *dataptr,
                               const char *propname,
                               PointerRNA *settings_ptr)
{
  if (!layout || !C || !grid_id || !grid_id[0] || !gridtype_name || !gridtype_name[0] ||
      !dataptr || !dataptr->data || !propname || !propname[0])
  {
    return;
  }

  uiGridType *grid_type = WM_uigridtype_find(gridtype_name, false);
  if (!grid_type) {
    RNA_warning("Grid type %s not found", gridtype_name);
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(dataptr, propname);
  if (!prop || RNA_property_type(prop) != PROP_COLLECTION) {
    RNA_warning(
        "Expected a collection property: %s.%s", RNA_struct_identifier(dataptr->type), propname);
    return;
  }

  Block *block = layout->block();

  const int preview_size = (settings_ptr && settings_ptr->data) ?
                               grid_settings::preview_size_get(*settings_ptr) :
                               96;
  const int tile_w = preview_tile_size_x(preview_size);
  const int tile_h = preview_tile_size_y_no_label(preview_size);
  const int panel_width = max_ii(layout->width(), 0);
  const int cols_est = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tile_w, 1)) : 1;

  auto source = std::make_unique<PyCallbackGridDataSource>(grid_type, *dataptr, propname);

  auto view_unique = std::make_unique<GenericGridView>(*C, std::move(source), cols_est, grid_id);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tile_w, tile_h);
  view_unique->set_cols_per_row_hint(cols_est);

  AbstractGridView *grid_view = block_add_view(*block, grid_id, std::move(view_unique));

  ViewGridStateAccess state_access(*view_ptr, grid_id);
  build_grid_view(*C,
                  *layout,
                  *grid_view,
                  state_access,
                  view_ptr->get_cached_item_count_for_build(),
                  cols_est,
                  panel_width);
}

} /* namespace blender::ui */

void blender::ui::grid_view_register_pre_button_handler()
{
  region_pre_button_handler_add(generic_grid_view_pre_button_handler);
}
