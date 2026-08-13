/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Process-lifetime #GridSessionState registry keyed by grid_id. Lives in its own translation
 * unit so the view/layout builder and the input handler do not own the map.
 */

#include <limits>
#include <memory>
#include <string>

#include "BLI_assert.h"
#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_sys_types.h"

#include "UI_grid_view.hh"
#include "interface_grid_view.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Grid Session State Registry
 *
 * Process-lifetime scroll/grip state keyed by grid_id, outliving the per-redraw view
 * instances. A #unique_ptr value keeps each state at a fixed address, so widgets bound to
 * its fields from an earlier redraw stay valid even when the #Map rehashes on inserting a
 * different grid_id. Bounded by capacity with LRU eviction of unreferenced entries, so
 * dynamically minted grid_ids cannot grow the registry without limit while scroll/grip
 * state still survives closing and reopening a popover.
 * \{ */

/**
 * Soft cap on live session entries. Eviction only considers entries with #refcount == 0.
 *
 * Between redraws the view destructor calls #grid_session_release, so refcount can hit 0 while
 * grip/scroll widgets still bind `int*` into the session. LRU eviction of that entry would
 * theoretically UAF those widgets. This has not been reproduced; do not "fix" by skipping
 * eviction while #GridSessionState::region is non-null without a repro. Pin-via-refcount is
 * the designed lifetime; #grid_view_session_remove is the explicit drop for dying owners.
 */
static constexpr int64_t GRID_SESSION_CAPACITY = 64;

struct GridSessionRuntime {
  Map<std::string, std::unique_ptr<GridSessionState>> sessions;
  uint64_t use_seq = 0;
};

static GridSessionRuntime &grid_session_runtime()
{
  static GridSessionRuntime runtime;
  return runtime;
}

GridSessionState &grid_session_state_ensure(const StringRef grid_id)
{
  GridSessionRuntime &runtime = grid_session_runtime();
  std::unique_ptr<GridSessionState> &slot = runtime.sessions.lookup_or_add_cb(
      std::string(grid_id), [] { return std::make_unique<GridSessionState>(); });
  slot->last_used_seq = ++runtime.use_seq;

  if (runtime.sessions.size() > GRID_SESSION_CAPACITY) {
    /* Evict the least recently used unreferenced entry. Linear scan is fine at this size. */
    const std::string *oldest_key = nullptr;
    uint64_t oldest_seq = std::numeric_limits<uint64_t>::max();
    GridSessionState *kept = slot.get();
    for (auto item : runtime.sessions.items()) {
      const GridSessionState &session = *item.value;
      if (session.refcount == 0 && item.value.get() != kept &&
          session.last_used_seq < oldest_seq)
      {
        oldest_seq = session.last_used_seq;
        oldest_key = &item.key;
      }
    }
    if (oldest_key) {
      runtime.sessions.remove(*oldest_key);
    }
  }
  return *slot;
}

GridSessionState *grid_session_state_lookup(const StringRef grid_id)
{
  GridSessionRuntime &runtime = grid_session_runtime();
  if (std::unique_ptr<GridSessionState> *slot = runtime.sessions.lookup_ptr_as(grid_id)) {
    return slot->get();
  }
  return nullptr;
}

void grid_session_acquire(GridSessionState &session)
{
  session.refcount++;
}

void grid_session_release(GridSessionState &session)
{
  BLI_assert(session.refcount > 0);
  session.refcount--;
}

void grid_session_state_foreach(
    const FunctionRef<bool(StringRef grid_id, GridSessionState &session)> fn)
{
  for (auto item : grid_session_runtime().sessions.items()) {
    if (!fn(item.key, *item.value)) {
      return;
    }
  }
}

void grid_view_session_remove(const StringRef grid_id)
{
  GridSessionRuntime &runtime = grid_session_runtime();
  /* Only safe when nothing references it (the owning space is being freed). */
  if (std::unique_ptr<GridSessionState> *slot = runtime.sessions.lookup_ptr_as(grid_id)) {
    if ((*slot)->refcount == 0) {
      runtime.sessions.remove_as(grid_id);
    }
  }
}

void grid_view_session_reset_scroll(const StringRef grid_id)
{
  /* Reset an existing session to the top and drop its per-column pins (e.g. a View3D grid whose
   * filter/library changed, so the old position is meaningless). No-op if the session was never
   * created — a fresh one already starts at the top. */
  if (GridSessionState *session = grid_session_state_lookup(grid_id)) {
    session->scroll_px = 0;
    session->scroll_px_by_cols.clear();
  }
}

int grid_view_session_cols(const StringRef grid_id)
{
  /* Column count of an existing session (0 when never drawn). Lets space code that owns its own
   * per-grid bookkeeping (e.g. the View3D focus-applied flags) query a grid's layout without
   * reaching into the interface-internal session registry. */
  if (const GridSessionState *session = grid_session_state_lookup(grid_id)) {
    return session->cols;
  }
  return 0;
}

/** \} */

}  // namespace blender::ui
