/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Unified grid input: wheel, touch/pen drag, kinetic fling, and fixed-viewport
 * popup edge auto-scroll. Session registry lives in grid_view_session.cc;
 * item layout in grid_view_layout.cc.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>
#include <string>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_fileselect.hh"
#include "ED_screen.hh"

#include "UI_grid_view.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"
#include "UI_view2d.hh"

#include "interface_grid_view.hh"
#include "interface_intern.hh"

namespace blender::ui {

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
  /**
   * A scroll drag just ended, so the click the window manager synthesizes after the release must
   * not reach the tile under the cursor. Outside popups a view item activates on that synthesized
   * #KM_CLICK (see #handle_view_item_event), which arrives after the release this handler
   * consumed -- by then the drag state is already reset, so the intent has to survive one event.
   */
  bool swallow_next_click = false;
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
  GridInputRuntime &runtime = grid_input_runtime();
  GridDragState &drag = runtime.drag;

  if (event->type == LEFTMOUSE && event->val == KM_CLICK) {
    /* The click synthesized after a scroll drag must not activate the tile the gesture happened to
     * start on. Consumed once: a later genuine tap synthesizes its own click. */
    if (runtime.swallow_next_click) {
      runtime.swallow_next_click = false;
      return WM_UI_HANDLER_BREAK;
    }
    return WM_UI_HANDLER_CONTINUE;
  }

  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    /* Reset on any new press so a missed release never leaves stale state. */
    drag = {};
    runtime.swallow_next_click = false;
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
    runtime.swallow_next_click = was_dragging;
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

bool grid_view_item_defers_activation_to_click(const AbstractViewItem &item)
{
  const auto *grid_view = dynamic_cast<const AbstractGridView *>(&item.get_view());
  if (grid_view == nullptr) {
    return false;
  }
  const StringRef grid_id = grid_view->session_grid_id();
  if (grid_id.is_empty()) {
    return false;
  }
  bool scrollable = false;
  grid_session_state_foreach([&](const StringRef id, GridSessionState &session) {
    if (id != grid_id) {
      return true;
    }
    scrollable = grid_session_max_scroll_px(session) > 0;
    return false;
  });
  return scrollable;
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

}  // namespace blender::ui
