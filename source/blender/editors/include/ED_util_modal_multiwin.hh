/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"

namespace blender {

struct ARegion;
struct ScrArea;
struct bContext;
struct wmEvent;

namespace ed {

/**
 * Keeps a modal operator's `bContext` area/region pointed at whichever region is under the
 * cursor for the current event, across split viewports and secondary windows. Needed by modal
 * tools registered on every open window (see #WM_event_add_modal_handler_all_windows):
 * `CTX_wm_area()`/`CTX_wm_region()` stay frozen at the region captured at invoke time and are
 * not otherwise updated when the cursor moves to a different area or window.
 *
 * Relies on the window manager's own guarantee that `CTX_wm_window(C)` is already set to the
 * window whose event queue is currently being processed before any handler (including modal
 * handlers) runs (`wm_event_do_handlers()` in `wm_event_system.cc`) -- so the tracker resolves
 * its own window via `CTX_wm_window(C)` and that window's active screen via
 * `WM_window_get_active_screen()`, not `CTX_wm_screen(C)`. It takes no `wmWindow` parameter of
 * its own; there is no supported case where the window it should track differs from
 * `CTX_wm_window(C)` at construction time.
 *
 * Construct at the top of the scope handling an event (typically the modal callback, or its
 * `MOUSEMOVE` case); the destructor restores the context that was active on entry -- including
 * on an early return (e.g. `OPERATOR_PASS_THROUGH`), since a "not found" construction never
 * touches the context in the first place.
 */
class ModalViewportTracker {
 public:
  ModalViewportTracker(bContext &C, const wmEvent &event, int space_type, int region_type);
  ~ModalViewportTracker();

  ModalViewportTracker(const ModalViewportTracker &) = delete;
  ModalViewportTracker &operator=(const ModalViewportTracker &) = delete;

  /** True when the cursor was over a matching region this event. */
  bool found() const
  {
    return region_ != nullptr;
  }
  ScrArea *area() const
  {
    return area_;
  }
  ARegion *region() const
  {
    return region_;
  }
  /** Mouse position in #region() (or the raw `event.mval` when #found() is false). */
  const int2 &mval() const
  {
    return mval_;
  }

  /**
   * Overrides an unresolved lookup with an explicit region -- e.g. to keep an active drag alive
   * when the cursor strays over an overlapping panel or past the viewport edge. \a fallback_region
   * is validated against the current window's active screen via #ED_screen_area_of_region first;
   * if it is not (any more) part of that screen, this is a no-op and #found() stays false. No-op
   * either way if #found() is already true.
   *
   * \note This validates screen membership, not pointer liveness: a freed #ARegion whose memory
   * has been reused for an unrelated region of the same type would still pass. Closing that gap
   * would need proactive invalidation like `WM_event_modal_handler_region_replace()`; today's
   * cached fallback pointers (e.g. a slide operator's own `ViewContext::region`) have the same
   * latent limitation, so this does not regress anything -- it just does not fix it either.
   */
  void use_fallback_region(ARegion *fallback_region);

 private:
  bContext &C_;
  const wmEvent &event_;
  ScrArea *prev_area_;
  ARegion *prev_region_;
  ScrArea *area_ = nullptr;
  ARegion *region_ = nullptr;
  int2 mval_{0, 0};

  void apply_region(ScrArea *area, ARegion *region);
};

}  // namespace ed
}  // namespace blender
