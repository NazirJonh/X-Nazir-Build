/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * The wmJob half of the paint-layer bake planner: a heavy row (see
 * #BKE_paint_layers_bake_is_heavy) is computed on a worker from a localized description and
 * written back on the main thread. The synchronous half stays in `BKE_paint_layers_bake_plan_run`,
 * run at the regeneration point; this only drains what that point leaves pending, from the editor
 * side of the depsgraph update.
 *
 * This file also holds the debounce of the renders an edit starts (a Material row's maps and the
 * heavy rows): #material_changed (`render_update.cc`) arms #paint_layers_bake_debounce_arm instead
 * of letting the next scene update start them, and its #BLI_timer tick, run from the shared
 * main-loop hook the way `bpy.app.timers` works, starts them once 0.3s after the *last* edit of a
 * burst (a trailing-edge debounce: arming restarts the timer, so a continuous drag never lets it
 * reach a tick).
 *
 * #MA_PAINT_LAYERS_BAKE_SCHEDULED stays set for as long as anything the tick started is still
 * running. #paint_layers_bake_scheduled_settle clears it, called from the free callback of every
 * job type the tick can start, success or cancel alike, so a script reading
 * `Material.paint_layers_is_stale` never sees "fresh" while a job it cannot see is still writing.
 */

#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {
struct Main;
struct Material;
struct wmOperatorType;
struct wmWindow;
struct wmWindowManager;
}  // namespace blender

namespace blender::ed::material_bake {

/**
 * Start a job for every layered material with a heavy, not-yet-valid baked row and no job of its
 * own already in flight, and the render of every Material row that is due
 * (#material_bake_layered_rows_ensure). Called on the main thread after the depsgraph update; a
 * material with nothing pending is skipped without touching the job system, and a material inside
 * its debounce window is left to the timer's tick.
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
 * Whether (\a owner, \a job_type) is the exact job named by \a exclude_owner / \a
 * exclude_job_type. Pure -- pointer and integer comparisons only, nothing dereferenced -- since it
 * is the one thing standing between a settle call made from a job's own free callback and
 * `WM_jobs_test` still reporting that very job as running (`wm_jobs_handle_finished` and
 * `wm_jobs_kill_job` in `wm_jobs.cc` both free the job before clearing `running` or removing it).
 */
bool paint_layers_bake_job_is_excluded(const void *owner,
                                       int job_type,
                                       const void *exclude_owner,
                                       int exclude_job_type);

/**
 * Whether \a ma's paint-layer bake is not the one its current stack would produce, counting both
 * what #BKE_paint_layers_is_stale can see on its own (a pending edit, a render nobody started, or
 * the editor's scheduled mark) and a job already running for it through `wmJob`, which
 * #BKE_paint_layers_is_stale cannot see by itself.
 *
 * A consumer of the bake with window-manager access should ask this instead of
 * `Material.paint_layers_is_stale`, since a job running but not yet marked would otherwise be
 * missed.
 */
bool ED_paint_layers_stale_or_pending(const wmWindowManager &wm, const Material &ma);

/**
 * Resolve \a pending materials (by `ID::session_uid`) against \a bmain, appending the ones still
 * there to \a r_found; a session_uid with no match (the material was deleted, or belonged to a
 * file that has since been closed) is silently dropped.
 *
 * Pure with respect to window-manager and job state -- a #Main and a list of numbers are all it
 * needs -- so it is testable without a #wmWindowManager or a timer.
 */
void paint_layers_bake_debounce_resolve(Main &bmain,
                                        Span<uint32_t> pending,
                                        Vector<Material *> &r_found);

/**
 * Whether arming should first remove the debounce #BLI_timer before registering a new one: true
 * whenever one is already running. Named on its own so the trailing-edge intent is checkable
 * directly: arming while a timer runs must restart its countdown, or a continuous drag would still
 * tick, and bake, every 0.3s instead of once at the end.
 */
bool paint_layers_bake_debounce_should_replace_timer();

/**
 * Record that \a ma was just edited and make sure the debounce timer is armed (or, if one is
 * already running, restarted for the nearest due time) to start its due renders once \a ma's time
 * comes: \a delay_seconds after this edit, zero meaning the short default (0.3s). \a ma is marked
 * #MA_PAINT_LAYERS_BAKE_SCHEDULED for as long as it is waiting.
 *
 * A caller with no #wmWindowManager (background mode) has no main loop to run the timer, and no
 * `wmJob` to start either; it never reaches this.
 */
void paint_layers_bake_debounce_arm(Material &ma, double delay_seconds = 0.0);

/**
 * Whether an already-#MA_PAINT_LAYERS_BAKE_SCHEDULED material should have that mark cleared now,
 * from \a jobs_in_flight alone: once nothing is left running, never before. Named on its own since
 * it is the one decision the tick and #paint_layers_bake_scheduled_settle make per material.
 */
bool paint_layers_bake_scheduled_settle_clears(bool jobs_in_flight);

/**
 * Clear #MA_PAINT_LAYERS_BAKE_SCHEDULED for every material of \a bmain that has no bake job in
 * flight and is not waiting in its debounce window (the tick settles that one). Called from the
 * free callback of every job type the debounce can start, so the mark survives exactly as long as
 * something started for the material is running, success or #WM_jobs_stop_all_from_owner cancel
 * alike, and from #paint_layers_bake_jobs_ensure as a backstop.
 *
 * \a exclude_owner / \a exclude_job_type name the caller's own, just-finishing job, which
 * `WM_jobs_test` still reports as running from inside its free callback; pass `nullptr` and
 * `WM_JOB_TYPE_ANY` (from `WM_api.hh`) for a caller with no such job of its own.
 */
void paint_layers_bake_scheduled_settle(Main &bmain,
                                        wmWindowManager &wm,
                                        const void *exclude_owner,
                                        int exclude_job_type);

/**
 * Disarm the debounce timer and forget every material still waiting on it, without starting their
 * renders. Must be called whenever the file is about to stop existing -- a new file, closing,
 * quitting -- or a stale tick would run against whatever comes next. Called from
 * #ED_editors_exit, never for memfile undo (its materials keep their session_uids, so a pending
 * entry stays meaningful across a Ctrl+Z).
 */
void paint_layers_bake_debounce_reset();

/**
 * "Bake Paint Layers Now": collapse \a ma's debounce and start its due renders immediately, the
 * same calls the tick makes, without waiting out the rest of the window. The material is found as
 * the neighboring Layer Material tab operators find it (the `"material"` context pointer).
 *
 * Only starts the bake and returns, `OPERATOR_FINISHED`. A caller that must wait for freshness is
 * a consumer of the *result* instead, and goes through #BKE_paint_layers_bake_gate_decide.
 */
void MATERIAL_OT_paint_layers_bake_now(wmOperatorType *ot);

}  // namespace blender::ed::material_bake
