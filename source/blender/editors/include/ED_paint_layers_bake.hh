/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * The wmJob half of the paint-layer bake planner: a heavy row (see
 * #BKE_paint_layers_bake_is_heavy) is computed on a worker from a localized description and
 * written back on the main thread. The synchronous half stays in `BKE_paint_layers.cc`, at the
 * K-1 point; this only drains what that point leaves pending, from the editor side of the DEG
 * update.
 *
 * This file also holds the 0.3s debounce for the *light* bakes (Material-row images, Combined
 * preview inputs): #material_changed (`render_update.cc`) no longer runs
 * `material_bake_images_rebake_stale`/`material_bake_layered_rows_ensure` synchronously on every
 * edit; it arms #paint_layers_bake_debounce_arm instead, and its #BLI_timer tick -- run from the
 * shared main-loop hook the same way `bpy.app.timers` works -- runs them once, 0.3s after the
 * *last* edit of a burst (a trailing-edge debounce: #paint_layers_bake_debounce_arm restarts the
 * timer on every call, so a continuous drag never lets it reach a tick).
 *
 * #MA_PAINT_LAYERS_BAKE_SCHEDULED stays set for as long as *anything* the tick started (or is about
 * to start: a heavy bake #paint_layers_bake_jobs_ensure has not queued yet) is still running --
 * #paint_layers_bake_scheduled_settle is what clears it, called from the free callback of every bake
 * job type the tick can start, success or cancel alike, so an add-on reading
 * `Material.paint_layers_is_stale` never sees "fresh" while a job it cannot see is still writing.
 */

#include "BKE_paint_layers.hh"

#include "BLI_span.hh"
#include "BLI_vector.hh"

struct Main;
struct wmOperatorType;
struct wmTimer;
struct wmWindow;
struct wmWindowManager;

namespace blender {
struct Material;
}  // namespace blender

namespace blender::ed::material_bake {

/**
 * Start a job for every layered material with a heavy, not-yet-valid baked row and no job of its
 * own already in flight. Called on the main thread after the depsgraph update; a material with
 * nothing heavy pending is skipped without touching the job system.
 */
void paint_layers_bake_jobs_ensure(wmWindowManager &wm, wmWindow *win, Main &bmain);

/**
 * Register the #BKE_CB_EVT_UNDO_POST handler that clears a stale #MA_PAINT_LAYERS_BAKE_SCHEDULED
 * mark left in a memfile-undo snapshot. Called once from the editor's startup init
 * (#ED_operatortypes_paint), after #BKE_callback_global_init, so it is registered before the first
 * scene update; the callback system owns and frees the store.
 */
void paint_layers_bake_undo_callback_init();

/**
 * Whether (\a owner, \a job_type) is the exact job named by \a exclude_owner / \a exclude_job_type.
 * Pure and tested on its own -- pointer and integer comparisons only, nothing dereferenced -- since
 * it is the one thing standing between a settle call made from a job's own free callback and
 * `WM_jobs_test` still reporting that very job as running (`wm_jobs_handle_finished` and
 * `wm_jobs_kill_job` in `wm_jobs.cc` both free before clearing `running`/removing the job).
 */
bool paint_layers_bake_job_is_excluded(const void *owner,
                                       int job_type,
                                       const void *exclude_owner,
                                       int exclude_job_type);

/**
 * Whether \a ma's paint-layer bake is not the one its current stack would produce, counting both
 * what #BKE_paint_layers_is_stale can see on its own (a stale flag, a heavy bake not yet queued,
 * or the editor's own scheduled mark) and a light bake job already running for it through
 * `wmJob`, which #BKE_paint_layers_is_stale cannot see by itself.
 *
 * This is what a CPU consumer of the bake -- the Image Editor's composite mode, the Combined
 * preview, a bake/export operator -- should ask instead of `Material.paint_layers_is_stale`
 * whenever it has window-manager access, since a job already in flight but not yet stamped
 * #MA_PAINT_LAYERS_BAKE_SCHEDULED would otherwise be missed.
 */
bool ED_paint_layers_stale_or_pending(const wmWindowManager &wm, const Material &ma);

/**
 * Resolve \a pending materials (by `ID::session_uid`) against \a bmain, appending the ones still
 * there to \a r_found; a session_uid with no match (the material was deleted, or belonged to a
 * file that has since been closed) is silently dropped.
 *
 * Pure with respect to window-manager/job state -- a #Main and a list of numbers are everything it
 * needs -- so it is testable without a #wmWindowManager or a `wmTimer`; only what to *do* with a
 * resolved material (#paint_layers_bake_debounce_timer) has real side effects.
 */
void paint_layers_bake_debounce_resolve(Main &bmain,
                                        Span<uint32_t> pending,
                                        Vector<Material *> &r_found);

/**
 * Whether arming should first remove \a existing before creating a new timer -- true whenever one
 * is already running. Named and tested on its own (a raw pointer, no #wmWindowManager needed) so
 * the trailing-edge intent is checkable directly: arming while a timer runs must restart its
 * countdown, or a continuous drag would still tick, and bake, every 0.3s instead of once at the end.
 */
bool paint_layers_bake_debounce_should_replace_timer(const wmTimer *existing);

/**
 * Record that \a ma was just edited and make sure the 0.3s debounce timer is armed (or, if one is
 * already running, restarted from now) to run its due bakes once it fires -- a trailing-edge
 * debounce, so a continuous drag never lets the timer reach a tick. Called from #material_changed
 * (`render_update.cc`) instead of running `material_bake_images_rebake_stale`/
 * `material_bake_layered_rows_ensure` synchronously on every edit; \a ma is marked
 * #MA_PAINT_LAYERS_BAKE_SCHEDULED for as long as it is waiting.
 *
 * \a ma must belong to \a wm's file: a caller with no #wmWindowManager (background mode, a script)
 * has nothing to arm a timer on and must run the bakes immediately instead -- this function is
 * only ever reached once that has already been checked.
 */
void paint_layers_bake_debounce_arm(wmWindowManager &wm, Material &ma);

/**
 * Whether the tick may clear \a ma's #MA_PAINT_LAYERS_BAKE_SCHEDULED mark itself, right after
 * starting its due bakes, from \a jobs_in_flight (a bake job the tick's own calls started is
 * already running) and \a heavy_pending (a heavy bake is queued but #paint_layers_bake_jobs_ensure
 * has not started it yet). True only when neither holds -- everything was already fresh, so nothing
 * will ever reach #paint_layers_bake_scheduled_settle for it. Pure and tested on its own: two
 * booleans, no #Main or `wmJob`.
 */
bool paint_layers_bake_debounce_settles_immediately(bool jobs_in_flight, bool heavy_pending);

/**
 * Look for hidden rows that have aged past #PAINT_LAYERS_COLD_TIER_SECONDS and make them leave the
 * graph: each due material is marked for regeneration, its stored root hash invalidated and a
 * depsgraph update requested. Materials still inside the window are remembered and the shared cold
 * timer is (re)armed for the earliest remaining deadline; with no hidden row at all the timer is
 * disarmed and nothing else happens -- an idle material is never polled. Called from
 * #ED_render_scene_update on every scene update and from the cold timer's own tick.
 */
void paint_layers_cold_tier_scan(Main &bmain, wmWindowManager &wm);

/**
 * Whether an already-#MA_PAINT_LAYERS_BAKE_SCHEDULED material should have that mark cleared now,
 * from \a jobs_in_flight alone. Trivial, but named and tested on its own: it is the one decision
 * #paint_layers_bake_scheduled_settle makes per material, and it is what should be checked without
 * a #Main or a `wmJob` when reviewing that the mark can never clear early.
 */
bool paint_layers_bake_scheduled_settle_clears(bool jobs_in_flight);

/**
 * Clear #MA_PAINT_LAYERS_BAKE_SCHEDULED for every material of \a bmain that no longer has a bake
 * job in flight for it (#ED_paint_layers_stale_or_pending's own job-testing half, shared as
 * `paint_layers_bake_jobs_in_flight`; the per-material decision itself is
 * #paint_layers_bake_scheduled_settle_clears). Called from the free callback of every bake job type
 * the debounce timer can start -- #paint_layers_bake_free and `material_bake_images_free` -- so the
 * mark survives exactly as long as something the tick started (or is still about to start) is
 * running, success or #WM_jobs_stop_all_from_owner cancel alike.
 *
 * \a exclude_owner / \a exclude_job_type name the caller's own, just-finishing job, which
 * `WM_jobs_test` still reports as running from inside its free callback -- pass `nullptr` /
 * `WM_JOB_TYPE_ANY` (from `WM_api.hh`, not repeated here to keep this header free of it) for a
 * caller with no such job of its own, such as #paint_layers_bake_jobs_ensure's backstop sweep.
 */
void paint_layers_bake_scheduled_settle(Main &bmain,
                                        wmWindowManager &wm,
                                        const void *exclude_owner,
                                        int exclude_job_type);

/**
 * Disarm the debounce timer and forget every material still waiting on it, without running their
 * bakes. Must be called before \a wm's own timers are freed (see `wm_close_and_free`) whenever the
 * file \a wm belongs to is about to stop existing -- a new file, closing, quitting -- or
 * #paint_layers_bake_debounce_timer_handle would dangle into whatever comes next. Mirrors
 * `wm_autosave_timer_end`; called from #ED_editors_exit, never for memfile undo (its materials keep
 * the same session_uids, so a pending entry stays meaningful across a Ctrl+Z).
 */
void paint_layers_bake_debounce_reset(wmWindowManager &wm);

/**
 * Size the paint-layer sampler budget from the GPU once, on the ED side of the layer boundary:
 * EEVEE keeps some slots for its own textures (#PAINT_LAYERS_EEVEE_RESERVED_SAMPLERS), the rest
 * bound how many samplers a layered material may use before the fallback pins live rows onto
 * their baked maps. #GPU_max_textures is only valid after GPU init, which is why this runs from
 * the first #ED_render_scene_update rather than startup; until then the budget stays zero, which
 * merely disables the fallback check.
 */
void ED_paint_layers_sampler_budget_ensure();

/**
 * The stale-until-action rule, re-exported for editors: #BKE_paint_layers_bake_gate_decide holds
 * the decision, so the editor callers and the RNA layer that now reaches the same function through
 * BKE can never drift apart. Kept as a named editor alias so the `ed::material_bake` namespace
 * still reads naturally at the call sites and in the tests.
 */
using PaintLayersBakeGateAction = blender::PaintLayersBakeGateAction;

inline PaintLayersBakeGateAction paint_layers_bake_gate_decide(const bool stale,
                                                               const bool is_invoke,
                                                               const bool headless)
{
  return BKE_paint_layers_bake_gate_decide(stale, is_invoke, headless);
}

/**
 * "Bake Paint Layers Now": collapse \a ma's 0.3s debounce (if one is armed for it) and run its due
 * bakes immediately -- the same calls #paint_layers_bake_debounce_timer's own tick makes
 * (`material_bake_images_rebake_stale`, `material_bake_layered_rows_ensure`,
 * #paint_layers_bake_jobs_ensure) -- without waiting the remainder of the debounce window first.
 *
 * \a ma is the layered material found the same way the neighboring Layer Material tab operators
 * find their owner (the `"material"` context pointer, which the tab's own panels point at the
 * stack's owner -- never `CTX_data_active_object`'s active slot directly, unlike
 * `PAINT_OT_material_canvas_cycle`). `invoke` and `exec` both only start the bake and return
 * immediately, `OPERATOR_FINISHED`, matching the "only start, do not wait" decision -- a caller
 * that must wait for freshness is a *result* consumer instead, and goes through
 * #paint_layers_bake_gate_decide, never this operator.
 */
void MATERIAL_OT_paint_layers_bake_now(wmOperatorType *ot);

}  // namespace blender::ed::material_bake
