/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrender
 *
 * See #ED_paint_layers_bake.hh: the wmJob half of the heavy paint-layer bake planner. It lives
 * beside the material bake because it is a bake job, and the window manager is kept out of
 * paint-layer knowledge.
 */

#include "ED_material_bake.hh"
#include "ED_paint_layers_bake.hh"

#include "BLI_listbase.h"
#include "BLI_set.hh"
#include "BLI_time.h"

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_callbacks.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_node_types.h"
#include "DNA_material_types.h"

#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm_event_types.hh"

namespace blender::ed::material_bake {

namespace {

/** One in-flight heavy bake. The BKE job owns the localized description and the pixel buffers. */
struct PaintLayersBakeWMJob {
  uint32_t material_session_uid = 0;
  PaintLayersBakeJob *bake = nullptr;
};

/**
 * Materials with a heavy bake in flight, so a second poll does not start a second job. The main
 * thread alone touches this: the scheduler, the end callback and the free callback all run there.
 */
Set<uint32_t> &paint_layers_bake_active()
{
  static Set<uint32_t> active;
  return active;
}

/**
 * Materials edited since the debounce timer was last armed, by `ID::session_uid` -- not by
 * pointer, since the material may be deleted, or belong to a file that gets closed, while this is
 * waiting; #paint_layers_bake_debounce_resolve is what turns a surviving uid back into a
 * #Material. A single shared timer drains the whole set on its first tick (see
 * #paint_layers_bake_debounce_timer), matching the batching #material_paint_layers.py's own
 * mesh-map debounce does: the window opens on the *first* edit of a burst, not a rolling "quiet
 * period" reset by each new one.
 */
Set<uint32_t> &paint_layers_bake_debounce_pending()
{
  static Set<uint32_t> pending;
  return pending;
}

/** Null when no debounce timer is currently armed; owned by the window manager once set. */
wmTimer *&paint_layers_bake_debounce_timer_handle()
{
  static wmTimer *handle = nullptr;
  return handle;
}

constexpr double paint_layers_bake_debounce_seconds = 0.3;

void paint_layers_bake_start(void *customdata, wmJobWorkerStatus *worker_status)
{
  PaintLayersBakeWMJob *job = static_cast<PaintLayersBakeWMJob *>(customdata);
  /* The worker only reads the localized copy and shared images; every write waits for the main
   * thread in #paint_layers_bake_end. #WM_JOB_PROGRESS (set on this job in
   * #paint_layers_bake_jobs_ensure) is what makes the window manager read `worker_status->progress`
   * at all -- both for the window's own progress bar and for #WM_jobs_test, which otherwise reports
   * this job type as never running (see #paint_layers_bake_job_test_excluding's doc-comment). */
  BKE_paint_layers_bake_job_compute(
      *job->bake, [worker_status](const float progress) { worker_status->progress = progress; });
}

void paint_layers_bake_end(void *customdata)
{
  PaintLayersBakeWMJob *job = static_cast<PaintLayersBakeWMJob *>(customdata);
  if (BKE_paint_layers_bake_job_commit(*job->bake)) {
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
  }
}

void paint_layers_bake_free(void *customdata)
{
  PaintLayersBakeWMJob *job = static_cast<PaintLayersBakeWMJob *>(customdata);
  paint_layers_bake_active().remove(job->material_session_uid);
  if (job->bake != nullptr) {
    BKE_paint_layers_bake_job_free(*job->bake);
    job->bake = nullptr;
  }
  /* Always runs, success or #WM_jobs_stop_all_from_owner cancel alike: settle whichever material
   * was only waiting on this heavy job, exactly like #material_bake_images_free's own call does for
   * the light job type. #G_MAIN rather than a parameter: `wmJob` free callbacks take only their own
   * `customdata`. This job's own (owner, type) is excluded -- see
   * #paint_layers_bake_job_test_excluding -- or #WM_jobs_test would still see it as running from in
   * here and never let the material it served settle. */
  if (Main *bmain = G_MAIN) {
    if (wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first)) {
      for (Material &ma : bmain->materials) {
        if (ma.id.session_uid == job->material_session_uid) {
          paint_layers_bake_scheduled_settle(*bmain, *wm, &ma, WM_JOB_TYPE_PAINT_LAYERS_BAKE);
          break;
        }
      }
    }
  }
  MEM_delete(job);
}

}  // namespace

bool paint_layers_bake_job_is_excluded(const void *owner,
                                       const int job_type,
                                       const void *exclude_owner,
                                       const int exclude_job_type)
{
  return owner == exclude_owner && job_type == exclude_job_type;
}

namespace {

/**
 * Like #WM_jobs_test, but always false for the exact (\a exclude_owner, \a exclude_job_type) pair.
 *
 * #WM_jobs_test still reports a job as running (or suspended) from *inside* its own free callback:
 * `wm_jobs_handle_finished` (`wm_jobs.cc`) calls `job.run_free`/the `customdata` free before setting
 * `job.running = false`, and `wm_jobs_kill_job`'s cancel path frees before removing the job from
 * `wm->runtime->jobs` at all, never touching `running`. A settle call made from inside
 * #paint_layers_bake_free or `material_bake_images_free` -- both free callbacks -- would otherwise
 * always see its own, just-finishing job as still in flight and never clear the last waiting
 * material's #MA_PAINT_LAYERS_BAKE_SCHEDULED mark, success or cancel alike.
 */
bool paint_layers_bake_job_test_excluding(const wmWindowManager &wm,
                                          const void *owner,
                                          const int job_type,
                                          const void *exclude_owner,
                                          const int exclude_job_type)
{
  if (paint_layers_bake_job_is_excluded(owner, job_type, exclude_owner, exclude_job_type)) {
    return false;
  }
  return WM_jobs_test(&wm, owner, job_type);
}

/**
 * Whether \a ma has a bake job in flight right now, through any of the `wmJob` types the paint-
 * layer bake can use: its own heavy bake, its own light "bake to images" job, or -- one level
 * removed -- a Material row's job, which is keyed on *its source material*, not on \a ma (see
 * #material_bake_layered_rows_ensure, which sets `params.material = row->material` before starting
 * it). \a ma would otherwise show as settled while one of its rows is still mid-bake, because the
 * job's owner is never \a ma itself.
 *
 * \a exclude_owner / \a exclude_job_type name a job to treat as already gone even though
 * #WM_jobs_test still reports it as running -- see #paint_layers_bake_job_test_excluding for why a
 * settle call made from inside that very job's free callback needs this. Left at their defaults
 * (`nullptr`, #WM_JOB_TYPE_ANY) for every other caller: no real job is ever registered with that
 * owner/type pair, so nothing is excluded.
 *
 * The one thing #BKE_paint_layers_is_stale cannot answer on its own, since BKE has no visibility
 * into `wmJob` state; shared between #ED_paint_layers_stale_or_pending and
 * #paint_layers_bake_scheduled_settle, which both need exactly this question answered.
 */
bool paint_layers_bake_jobs_in_flight(const wmWindowManager &wm,
                                      const Material &ma,
                                      const void *exclude_owner = nullptr,
                                      const int exclude_job_type = WM_JOB_TYPE_ANY)
{
  if (paint_layers_bake_job_test_excluding(
          wm, &ma, WM_JOB_TYPE_PAINT_LAYERS_BAKE, exclude_owner, exclude_job_type) ||
      paint_layers_bake_job_test_excluding(
          wm, &ma, WM_JOB_TYPE_MATERIAL_IMAGES_BAKE, exclude_owner, exclude_job_type))
  {
    return true;
  }
  /* A Custom row's job is keyed on a throw-away host material, so neither test above can see it;
   * only the editor-static active set can. */
  if (material_bake_custom_in_flight(ma)) {
    return true;
  }
  if (!paint_layers_is_layered(ma)) {
    return false;
  }
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten_all(ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer->material != nullptr &&
        paint_layers_bake_job_test_excluding(wm,
                                             layer->material,
                                             WM_JOB_TYPE_MATERIAL_IMAGES_BAKE,
                                             exclude_owner,
                                             exclude_job_type))
    {
      return true;
    }
  }
  return false;
}

/**
 * Clear a #MA_PAINT_LAYERS_BAKE_SCHEDULED mark that a memfile-undo snapshot restored without the
 * editor state it stood for.
 *
 * The flag lives in #Material::paint_layers_flag, so Ctrl+Z restores it from the snapshot; the
 * process-static debounce timer and the `wmJob`s it may have stood for are *not* part of the
 * snapshot. A mark that comes back is a lie unless something is still running: keep it exactly when
 * \a ma has a bake job in flight, or it sits in a live debounce window (an armed timer and a pending
 * uid). Everything else is cleared here, on the main thread, so `Material.paint_layers_is_stale`
 * never reads "busy" for a job that no longer exists.
 *
 * This is deliberately the inverse of the arm/settle pair: arming sets the flag before any job
 * exists, so on a normal update the pending set proves the wait is still real; only an undo can
 * resurrect the flag with no pending entry and no job, which is what this callback catches.
 */
void paint_layers_bake_undo_post(Main *bmain,
                                 PointerRNA ** /*pointers*/,
                                 const int /*pointers_num*/,
                                 void * /*arg*/)
{
  if (bmain == nullptr) {
    return;
  }
  /* The snapshot restores the generated trees and their stored root hash, but the regeneration
   * flag it carries was already cleared, and the runtime that would notice the mismatch is not part
   * of it. Without this the restored graph stays out of step with the restored description until
   * the next unrelated edit. */
  for (Material &ma : bmain->materials) {
    if (paint_layers_is_layered(ma)) {
      BKE_paint_layers_tag_edited(ma);
    }
  }

  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  const bool debounce_armed = paint_layers_bake_debounce_timer_handle() != nullptr;
  for (Material &ma : bmain->materials) {
    if (!BKE_paint_layers_bake_scheduled_get(ma)) {
      continue;
    }
    if (wm != nullptr && paint_layers_bake_jobs_in_flight(*wm, ma)) {
      continue;
    }
    if (debounce_armed && paint_layers_bake_debounce_pending().contains(ma.id.session_uid)) {
      continue;
    }
    BKE_paint_layers_bake_scheduled_set(ma, false);
  }
}

}  // namespace

/**
 * Register the memfile-undo cleanup once, from the editor's startup init
 * (#ED_operatortypes_paint, after #BKE_callback_global_init), so the callback exists before the
 * first scene update and for the whole lifetime of the callback system. The store is heap-owned by
 * the callback system (`alloc = true`), so it is freed by #BKE_callback_global_finalize and no
 * static object needs to outlive it.
 */
void paint_layers_bake_undo_callback_init()
{
  bCallbackFuncStore *store = MEM_new<bCallbackFuncStore>(__func__);
  store->alloc = true;
  store->arg = nullptr;
  store->func = paint_layers_bake_undo_post;
  BKE_callback_add(store, BKE_CB_EVT_UNDO_POST);
}

void paint_layers_bake_jobs_ensure(wmWindowManager &wm, wmWindow *win, Main &bmain)
{
  /* Backstop for #paint_layers_bake_free's and `material_bake_images_free`'s own settle calls:
   * called on every scene update regardless of whether anything is scheduled (the per-material
   * flag check inside makes an empty sweep cheap), so a material whose settle was missed for any
   * reason not foreseen by the exclusion those callbacks use -- #paint_layers_bake_job_test_excluding
   * only ever excludes one (owner, job_type) pair per call -- still settles on the very next update. */
  paint_layers_bake_scheduled_settle(bmain, wm, nullptr, WM_JOB_TYPE_ANY);
  for (Material &ma : bmain.materials) {
    if (!paint_layers_is_layered(ma)) {
      continue;
    }
    if (paint_layers_bake_active().contains(ma.id.session_uid)) {
      continue;
    }
    /* A material inside its 0.3s debounce window is left alone: its heavy bake, if it turns out
     * to need one, is queued by the very next call of this function, once
     * #paint_layers_bake_debounce_timer's tick has cleared it from the pending set. Everything
     * else -- a bake left stale by a file load, or by anything that never goes through
     * #material_changed -- is still caught here on every call, exactly as before; only a just-
     * edited material's *own* heavy job is deferred. */
    if (paint_layers_bake_debounce_pending().contains(ma.id.session_uid)) {
      continue;
    }
    if (!BKE_paint_layers_bake_heavy_pending(ma)) {
      continue;
    }
    PaintLayersBakeJob *bake = BKE_paint_layers_bake_job_create(bmain, ma);
    if (bake == nullptr) {
      /* Nothing could actually be queued (no size, no localized copy): leave it to the next
       * poll rather than marking it active. */
      continue;
    }
    PaintLayersBakeWMJob *job = MEM_new<PaintLayersBakeWMJob>(__func__);
    job->material_session_uid = ma.id.session_uid;
    job->bake = bake;

    /* #WM_JOB_PROGRESS is what makes #WM_jobs_test ever report this job as running at all --
     * without it, `wm_job.flag & WM_JOB_PROGRESS` in `WM_jobs_test` (`wm_jobs.cc`) is false and the
     * whole `running || suspended` check is skipped, so #paint_layers_bake_jobs_in_flight and the
     * topbar's running-jobs indicator (`interface_template_running_jobs.cc`) would never see a
     * heavy bake in flight, however long it actually runs. #paint_layers_bake_start feeds the
     * window manager's progress bar the fraction of row/channel pairs computed so far, or the flag
     * alone would leave that bar frozen at zero. Orthogonal to #WM_JOB_EXCL_RENDER's own exclusivity
     * bookkeeping and to #WM_JOB_PRIORITY (`wm_jobs.cc:465-489`): neither reads
     * `WM_JOB_PROGRESS`, so adding it here changes nothing about which jobs suspend which. */
    wmJob *wm_job = WM_jobs_get(&wm,
                                win,
                                &ma,
                                "Baking paint layers...",
                                WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                                WM_JOB_TYPE_PAINT_LAYERS_BAKE);
    WM_jobs_customdata_set(wm_job, job, paint_layers_bake_free);
    WM_jobs_timer(wm_job, 0.2, NC_MATERIAL, NC_MATERIAL);
    WM_jobs_callbacks(wm_job, paint_layers_bake_start, nullptr, nullptr, paint_layers_bake_end);
    paint_layers_bake_active().add(ma.id.session_uid);
    /* The editor now has an outstanding bake for \a ma: stamp it before the job can run, so
     * Material.paint_layers_is_stale never reads fresh while the worker is writing. The debounce and
     * "bake now" callers already stamped it; this covers the backstop sweep after a file load, which
     * starts a heavy job that no arm preceded. */
    BKE_paint_layers_bake_scheduled_set(ma, true);
  }
}

bool ED_paint_layers_stale_or_pending(const wmWindowManager &wm, const Material &ma)
{
  return BKE_paint_layers_is_stale(ma) || paint_layers_bake_jobs_in_flight(wm, ma);
}

bool paint_layers_bake_scheduled_settle_clears(const bool jobs_in_flight)
{
  /* Named and tested on its own, trivial as it is: this is the one decision
   * #paint_layers_bake_scheduled_settle makes per already-scheduled material, and it is what a
   * reviewer (or a future change) should be able to check without a #Main or a `wmJob` at all --
   * clear once nothing is left running, never before. */
  return !jobs_in_flight;
}

void paint_layers_bake_scheduled_settle(Main &bmain,
                                        wmWindowManager &wm,
                                        const void *exclude_owner,
                                        const int exclude_job_type)
{
  for (Material &ma : bmain.materials) {
    /* Cheap flag check first: the common case, called from every bake job's free callback whether
     * or not it has anything to do with the paint-layer debounce, is that nothing is scheduled. */
    if (!BKE_paint_layers_bake_scheduled_get(ma)) {
      continue;
    }
    if (paint_layers_bake_scheduled_settle_clears(
            paint_layers_bake_jobs_in_flight(wm, ma, exclude_owner, exclude_job_type)))
    {
      BKE_paint_layers_bake_scheduled_set(ma, false);
    }
  }
}

void paint_layers_bake_debounce_resolve(Main &bmain,
                                        const Span<uint32_t> pending,
                                        Vector<Material *> &r_found)
{
  if (pending.is_empty()) {
    return;
  }
  /* A linear scan per pending uid: the pending set is a handful of just-edited materials at most,
   * never worth a lookup table built and torn down on every timer tick. */
  for (Material &ma : bmain.materials) {
    if (pending.contains(ma.id.session_uid)) {
      r_found.append(&ma);
    }
  }
}

bool paint_layers_bake_debounce_should_replace_timer(const wmTimer *existing)
{
  /* A named, testable predicate rather than an inlined null check, so the trailing-edge intent is
   * visible on its own: arming while a timer already runs must restart its countdown from now, not
   * let the earlier one fire on schedule -- otherwise a continuous drag (a slider held for two
   * seconds) would still start a bake every 0.3s instead of once after the drag ends. */
  return existing != nullptr;
}

void paint_layers_bake_debounce_arm(wmWindowManager &wm, Material &ma)
{
  BKE_paint_layers_bake_scheduled_set(ma, true);
  paint_layers_bake_debounce_pending().add(ma.id.session_uid);
  if (paint_layers_bake_debounce_should_replace_timer(paint_layers_bake_debounce_timer_handle())) {
    /* Removing and re-adding, rather than leaving the running timer alone, is what makes this a
     * *trailing*-edge debounce: each edit pushes the fire time 0.3s further out, so only a genuine
     * pause of that length ever lets the timer reach its tick. #WM_event_timer_remove only tags the
     * old timer for removal -- it will not fire in the meantime (the dispatch loop skips a tagged
     * timer), so there is no risk of it running between this call and the new one below. */
    WM_event_timer_remove(&wm, nullptr, paint_layers_bake_debounce_timer_handle());
  }
  /* `win = nullptr`: this timer belongs to no window, exactly like `wm->autosavetimer` -- it is
   * dispatched straight from the window-manager's own timer loop (see `TIMERPAINTLAYERSBAKE` in
   * `wm_window.cc`), never queued as a `wmEvent` to any area's handlers. */
  /* A correction visibility toggle keeps the longer window open (see
   * #PAINT_LAYERS_BAKE_EDIT_QUIET_SECONDS); every re-arm in it, including the one the graph
   * rebuild's own shading tag causes, restarts the countdown with the rest of that window. */
  const double seconds = BKE_paint_layers_bake_debounce_seconds(
      ma, paint_layers_bake_debounce_seconds);
  paint_layers_bake_debounce_timer_handle() = WM_event_timer_add(
      &wm, nullptr, TIMERPAINTLAYERSBAKE, seconds);
}

bool paint_layers_bake_debounce_settles_immediately(const bool jobs_in_flight,
                                                    const bool heavy_pending)
{
  /* Named and testable with two booleans, no #Main or `wmJob` needed: the tick may clear
   * #MA_PAINT_LAYERS_BAKE_SCHEDULED itself only when nothing was started by it and nothing is about
   * to be -- a heavy bake that #paint_layers_bake_jobs_ensure has not queued yet still counts as
   * outstanding, or the mark would flicker false for the brief window before that queuing happens.
   * Everything else is left for #paint_layers_bake_scheduled_settle, once a job it actually started
   * frees. */
  return !jobs_in_flight && !heavy_pending;
}

void paint_layers_bake_debounce_timer(Main &bmain, wmWindowManager &wm, wmTimer &wt)
{
  Set<uint32_t> &pending = paint_layers_bake_debounce_pending();
  Vector<uint32_t> pending_uids;
  pending_uids.reserve(pending.size());
  for (const uint32_t uid : pending) {
    pending_uids.append(uid);
  }
  Vector<Material *> found;
  paint_layers_bake_debounce_resolve(bmain, pending_uids, found);
  for (Material *ma : found) {
    /* The same two calls #material_changed used to make synchronously on every edit; a material
     * deleted while waiting is simply absent from \a found and never reaches them. */
    material_bake_images_rebake_stale(bmain, *ma);
    material_bake_layered_rows_ensure(bmain, *ma);
    /* The idle moment after an edit: put back the warm slot the edit used, so the next add of the
     * same kind is instant again. The recompile this causes happens here, off the user's action. */
    BKE_paint_layers_warm_replenish(*ma);
    if (paint_layers_bake_debounce_settles_immediately(paint_layers_bake_jobs_in_flight(wm, *ma),
                                                        BKE_paint_layers_bake_heavy_pending(*ma)))
    {
      BKE_paint_layers_bake_scheduled_set(*ma, false);
    }
    /* Otherwise left set: #paint_layers_bake_scheduled_settle clears it once every job this
     * started -- or #paint_layers_bake_jobs_ensure's very next call is about to start -- actually
     * frees, success or cancel alike. */
  }
  /* One-shot in effect: the timer removes itself on its first tick and is re-added by the next
   * #paint_layers_bake_debounce_arm, exactly like `material_paint_layers.py`'s own mesh-map
   * debounce re-registers its `bpy.app.timers` callback only when a new edit needs it. */
  pending.clear();
  WM_event_timer_remove(&wm, nullptr, &wt);
  paint_layers_bake_debounce_timer_handle() = nullptr;
}

namespace {

/**
 * Materials with a hidden row still inside the cold-tier window, by `ID::session_uid` -- not by
 * pointer, since the material may be deleted while this is waiting. Process-static editor state,
 * exactly like #paint_layers_bake_debounce_pending.
 */
Set<uint32_t> &paint_layers_cold_pending()
{
  static Set<uint32_t> pending;
  return pending;
}

/** Null when no cold-tier timer is currently armed; owned by the window manager once set. */
wmTimer *&paint_layers_cold_timer_handle()
{
  static wmTimer *handle = nullptr;
  return handle;
}

/** The absolute time the armed cold timer targets; only re-armed when it moves perceptibly. */
double &paint_layers_cold_deadline()
{
  static double deadline = 0.0;
  return deadline;
}

}  // namespace

void paint_layers_bake_debounce_reset(wmWindowManager &wm)
{
  /* The debounce timer handle and pending set are process-static ED state, keyed on a #wmWindowManager
   * that is about to stop existing (a new file, closing the file, quitting) -- #wm_close_and_free
   * frees every registered `wmTimer`, including this one if it is still armed, which would leave
   * #paint_layers_bake_debounce_timer_handle dangling for the next file's first edit to dereference.
   * Mirrors `wm_autosave_timer_end`, the same cleanup for `wm->autosavetimer`. Called from
   * #ED_editors_exit, in the branch that runs for a real file replace, never for memfile undo. */
  if (wmTimer *timer = paint_layers_bake_debounce_timer_handle()) {
    WM_event_timer_remove(&wm, nullptr, timer);
    paint_layers_bake_debounce_timer_handle() = nullptr;
  }
  paint_layers_bake_debounce_pending().clear();
  /* The cold timer is keyed on the same `wm`, so it must be disarmed here too or its handle would
   * dangle into the next file. */
  if (wmTimer *timer = paint_layers_cold_timer_handle()) {
    WM_event_timer_remove(&wm, nullptr, timer);
    paint_layers_cold_timer_handle() = nullptr;
  }
  paint_layers_cold_pending().clear();
}

void paint_layers_cold_tier_scan(Main &bmain, wmWindowManager &wm)
{
  const double now = BLI_time_now_seconds();
  Set<uint32_t> &pending = paint_layers_cold_pending();
  const int no_regen_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  double next = -1.0;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & no_regen_tags) != 0 || !paint_layers_is_layered(ma)) {
      continue;
    }
    /* Rows hidden behind set_enabled's back (undo) get their clock before the deadline is read. */
    BKE_paint_layers_cold_tier_stamp(ma, now);
    const double remaining = BKE_paint_layers_cold_tier_seconds(ma, now);
    if (remaining < 0.0) {
      /* No hidden row: nothing to watch and nothing to do. */
      pending.remove(ma.id.session_uid);
      continue;
    }
    if (remaining <= 0.0) {
      /* Checked before the poll, which sets the flag itself: a regeneration already requested and
       * not yet consumed must not be announced a second time. */
      const bool regen_pending = (ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0;
      const bool cold_due = BKE_paint_layers_cold_tier_poll(ma, now);
      if (cold_due && !regen_pending) {
        /* The flag alone is consumed by the next regenerate; the tag is what makes that happen
         * without a user action. */
        WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma.id);
        DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
      }
      pending.remove(ma.id.session_uid);
      continue;
    }
    pending.add(ma.id.session_uid);
    if (next < 0.0 || remaining < next) {
      next = remaining;
    }
  }
  const double deadline = now + next;
  const bool armed = paint_layers_cold_timer_handle() != nullptr;
  const double deadband = deadline - paint_layers_cold_deadline();
  const bool moved = deadband > 0.5 || deadband < -0.5;
  if (next >= 0.0 && (!armed || moved)) {
    /* Only re-arm when the deadline actually moved: a scan on every scene update must not keep
     * resetting the timer. A hair past the deadline so the poll's `>=` age test cannot miss. */
    if (armed) {
      WM_event_timer_remove(&wm, nullptr, paint_layers_cold_timer_handle());
    }
    paint_layers_cold_timer_handle() = WM_event_timer_add(
        &wm, nullptr, TIMERPAINTLAYERSCOLD, next + 0.05);
    paint_layers_cold_deadline() = deadline;
  }
  else if (next < 0.0 && armed) {
    WM_event_timer_remove(&wm, nullptr, paint_layers_cold_timer_handle());
    paint_layers_cold_timer_handle() = nullptr;
    paint_layers_cold_deadline() = 0.0;
  }
}

void paint_layers_cold_tier_timer(Main &bmain, wmWindowManager &wm, wmTimer &wt)
{
  if (paint_layers_cold_timer_handle() == &wt) {
    paint_layers_cold_timer_handle() = nullptr;
  }
  WM_event_timer_remove(&wm, nullptr, &wt);
  /* Rescan, which drops whatever is now due and re-arms for the rest. */
  paint_layers_cold_tier_scan(bmain, wm);
}

namespace {

bool paint_layers_bake_now_poll(bContext *C)
{
  const Material *ma = static_cast<const Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (ma == nullptr || !paint_layers_is_layered(*ma)) {
    CTX_wm_operator_poll_msg_set(C, "No layered material");
    return false;
  }
  return true;
}

wmOperatorStatus paint_layers_bake_now_exec(bContext *C, wmOperator *op)
{
  Material *ma = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  Main *bmain = CTX_data_main(C);
  if (ma == nullptr || bmain == nullptr || !paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "No layered material");
    return OPERATOR_CANCELLED;
  }

  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    /* Headless: nothing to snap a debounce timer out of, and #material_changed's own headless
     * branch already runs these two synchronously on every edit -- reached only through a script
     * that skipped that, if at all. */
    material_bake_images_rebake_stale(*bmain, *ma);
    material_bake_layered_rows_ensure(*bmain, *ma);
    return OPERATOR_FINISHED;
  }

  /* The same hand-off #paint_layers_bake_debounce_arm makes, so `Material.paint_layers_is_stale`
   * keeps reporting stale for as long as what this starts is still running, whether or not a
   * debounce timer happened to be armed for \a ma already. */
  BKE_paint_layers_bake_scheduled_set(*ma, true);
  /* The exact two calls #paint_layers_bake_debounce_timer's own tick makes, run now instead of
   * waiting out the remainder of the 0.3s window -- \a ma's pending debounce entry, if it has one,
   * is left alone rather than resolved here: the timer's own tick still fires later and simply
   * finds nothing left to do for it (#material_bake_images_rebake_stale and
   * #material_bake_layered_rows_ensure are idempotent against an already-fresh result). */
  material_bake_images_rebake_stale(*bmain, *ma);
  material_bake_layered_rows_ensure(*bmain, *ma);
  BKE_paint_layers_warm_replenish(*ma);
  paint_layers_bake_jobs_ensure(*wm, CTX_wm_window(C), *bmain);
  if (paint_layers_bake_debounce_settles_immediately(paint_layers_bake_jobs_in_flight(*wm, *ma),
                                                      BKE_paint_layers_bake_heavy_pending(*ma)))
  {
    BKE_paint_layers_bake_scheduled_set(*ma, false);
  }
  return OPERATOR_FINISHED;
}

}  // namespace

void MATERIAL_OT_paint_layers_bake_now(wmOperatorType *ot)
{
  ot->name = "Bake Paint Layers Now";
  ot->idname = "MATERIAL_OT_paint_layers_bake_now";
  ot->description =
      "Collapse the paint layers bake debounce and bake this material's paint layers "
      "immediately, without waiting for the result to be read";
  ot->exec = paint_layers_bake_now_exec;
  ot->poll = paint_layers_bake_now_poll;
  /* No #OPTYPE_UNDO: this starts background computation (a debounce collapse, `wmJob`s), it does
   * not itself make a user-visible edit to undo back out of -- exactly like the other job-starting
   * operators in this file have none either. */
  ot->flag = OPTYPE_REGISTER;
}

}  // namespace blender::ed::material_bake
