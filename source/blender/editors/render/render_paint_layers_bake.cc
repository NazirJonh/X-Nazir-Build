/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * See #ED_paint_layers_bake.hh: the wmJob half of the heavy paint-layer bake planner and the
 * debounce of the renders an edit starts. It lives beside the material bake because it is a bake
 * job, and the window manager is kept out of paint-layer knowledge.
 */

#include <algorithm>

#include "ED_material_bake.hh"
#include "ED_paint_layers_bake.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_time.h"
#include "BLI_timer.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_windowmanager_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

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
 * Materials edited since the debounce timer was last armed, with the time each one is due, by `ID::session_uid` -- not by
 * pointer, since the material may be deleted, or belong to a file that gets closed, while this is
 * waiting; #paint_layers_bake_debounce_resolve turns a surviving uid back into a #Material. A
 * single shared timer drains the whole set on its tick.
 */
Map<uint32_t, double> &paint_layers_bake_debounce_pending()
{
  static Map<uint32_t, double> pending;
  return pending;
}

/** The address of this is the uuid of the debounce timer, shared with no other user of #BLI_timer.
 */
char paint_layers_bake_debounce_timer_tag = 0;

uintptr_t paint_layers_bake_debounce_timer_uuid()
{
  return reinterpret_cast<uintptr_t>(&paint_layers_bake_debounce_timer_tag);
}

/** The quiet time after the last edit of a burst before its renders are started. */
constexpr double paint_layers_bake_debounce_seconds = 0.3;

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
 * layer bake uses: its own heavy bake, a "bake to images" job of its own, or the render of one of
 * its Material rows, which is keyed on the row (#material_bake_layered_rows_ensure) and so is
 * never a job whose owner is \a ma itself.
 *
 * \a exclude_owner / \a exclude_job_type name a job to treat as already gone even though
 * `WM_jobs_test` still reports it as running: a settle made from inside that very job's free
 * callback would otherwise always see its own, just-finishing job and never let the material it
 * served settle. Every other caller passes no such job, and nothing is excluded.
 *
 * The one thing #BKE_paint_layers_is_stale cannot answer on its own, since BKE has no visibility
 * into `wmJob` state.
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
  if (!BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  bool row_running = false;
  BKE_paint_layers_foreach(ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer *) {
    row_running = row.source == MA_PAINT_LAYER_SOURCE_MATERIAL &&
                  paint_layers_bake_job_test_excluding(
                      wm, &row, WM_JOB_TYPE_MATERIAL_IMAGES_BAKE, exclude_owner, exclude_job_type);
    return !row_running;
  });
  return row_running;
}

void paint_layers_bake_start(void *customdata, wmJobWorkerStatus *worker_status)
{
  PaintLayersBakeWMJob *job = static_cast<PaintLayersBakeWMJob *>(customdata);
  /* The worker only reads the localized copy and shared images; every write waits for the main
   * thread in #paint_layers_bake_end. #WM_JOB_PROGRESS (set on the job in
   * #paint_layers_bake_job_start) is what makes the window manager read the progress at all. */
  BKE_paint_layers_bake_job_compute(
      *job->bake,
      [worker_status](const float progress) {
        worker_status->progress = progress;
        worker_status->do_update = true;
      },
      [worker_status]() { return worker_status->stop; });
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
  /* Always runs, success or #WM_jobs_stop_all_from_owner cancel alike: settle the material that
   * was only waiting on this job. #G_MAIN rather than a parameter, since a `wmJob` free callback
   * takes only its own `customdata`. This job itself is excluded (see
   * #paint_layers_bake_jobs_in_flight) or it would still count as running from in here. */
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

/**
 * Start the heavy bake of \a ma when a row of it needs one and none runs already.
 * \return true when a job was started.
 */
bool paint_layers_bake_job_start(wmWindowManager &wm, wmWindow *win, Main &bmain, Material &ma)
{
  /* A job needs a window to own its progress and timer; background mode has none, and nothing there
   * would redraw the result anyway. */
  if (win == nullptr) {
    return false;
  }
  if (paint_layers_bake_active().contains(ma.id.session_uid) ||
      !BKE_paint_layers_bake_heavy_pending(ma))
  {
    return false;
  }
  PaintLayersBakeJob *bake = BKE_paint_layers_bake_job_create(bmain, ma);
  if (bake == nullptr) {
    /* Nothing could actually be queued (no size, no localized copy): leave it to the next poll
     * rather than marking the material active. */
    return false;
  }
  PaintLayersBakeWMJob *job = MEM_new<PaintLayersBakeWMJob>(__func__);
  job->material_session_uid = ma.id.session_uid;
  job->bake = bake;
  /* #WM_JOB_PROGRESS is what makes #WM_jobs_test report this job as running at all -- without it
   * #paint_layers_bake_jobs_in_flight and the running-jobs indicator would never see a heavy bake
   * in flight -- and what feeds the window's progress bar. */
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
   * Material.paint_layers_is_stale never reads fresh while the worker is writing. */
  BKE_paint_layers_bake_scheduled_set(ma, true);
  WM_jobs_start(&wm, wm_job);
  return true;
}

/**
 * Clear a #MA_PAINT_LAYERS_BAKE_SCHEDULED mark that a memfile-undo snapshot restored without the
 * editor state it stood for.
 *
 * The flag lives in #Material::paint_layers_flag, so Ctrl+Z restores it from the snapshot; the
 * debounce timer and the `wmJob`s it may have stood for are not part of the snapshot. A mark that
 * comes back is a lie unless something is still running: it is kept exactly when \a ma has a bake
 * job in flight or sits in a live debounce window, and cleared otherwise, so
 * `Material.paint_layers_is_stale` never reads "busy" for a job that no longer exists.
 */
void paint_layers_bake_undo_post(Main *bmain,
                                 PointerRNA ** /*pointers*/,
                                 const int /*pointers_num*/,
                                 void * /*arg*/)
{
  if (bmain == nullptr) {
    return;
  }
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  const bool debounce_armed = BLI_timer_is_registered(paint_layers_bake_debounce_timer_uuid());
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

double paint_layers_bake_debounce_tick(uintptr_t /*uuid*/, void * /*user_data*/)
{
  /* #BLI_timer callbacks carry no context; the main database and its window manager are the ones
   * the main loop is running with. */
  Map<uint32_t, double> &pending = paint_layers_bake_debounce_pending();
  if (G_MAIN == nullptr || G_MAIN->wm.first == nullptr) {
    pending.clear();
    return -1.0;
  }
  Main &bmain = *G_MAIN;
  wmWindowManager &wm = *static_cast<wmWindowManager *>(bmain.wm.first);
  wmWindow *win = static_cast<wmWindow *>(wm.windows.first);

  /* A material waits as long as its own edit asked for (an edit through the row being edited may
   * wait seconds), so only the ones whose time has come are taken; the timer runs again for the
   * nearest of the rest. */
  const double now = BLI_time_now_seconds();
  Vector<uint32_t> pending_uids;
  double next_wait = -1.0;
  for (const auto item : pending.items()) {
    if (item.value <= now + 1e-3) {
      pending_uids.append(item.key);
    }
    else {
      const double wait = item.value - now;
      next_wait = (next_wait < 0.0) ? wait : std::min(next_wait, wait);
    }
  }
  /* Removed before the work: the jobs started below must not be skipped as "still waiting". */
  for (const uint32_t uid : pending_uids) {
    pending.remove(uid);
  }

  Vector<Material *> found;
  paint_layers_bake_debounce_resolve(bmain, pending_uids, found);
  for (Material *ma : found) {
    /* A material deleted while waiting is simply absent from \a found. The same calls a scene
     * update makes (#paint_layers_bake_jobs_ensure), now for the material that waited. */
    material_bake_layered_rows_ensure(bmain, *ma);
    paint_layers_bake_job_start(wm, win, bmain, *ma);
    /* Nothing started: nothing will ever reach #paint_layers_bake_scheduled_settle for it. A job
     * that did start clears the mark itself when it frees. */
    if (paint_layers_bake_scheduled_settle_clears(paint_layers_bake_jobs_in_flight(wm, *ma))) {
      BKE_paint_layers_bake_scheduled_set(*ma, false);
    }
  }
  /* The timer runs again only for what is still waiting; otherwise it removes itself and the next
   * #paint_layers_bake_debounce_arm registers it again. */
  return next_wait;
}

bool paint_layers_bake_now_poll(bContext *C)
{
  const Material *ma = static_cast<const Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
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
  wmWindowManager *wm = CTX_wm_manager(C);
  if (ma == nullptr || bmain == nullptr || wm == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "No layered material");
    return OPERATOR_CANCELLED;
  }
  /* The same hand-off #paint_layers_bake_debounce_arm makes, so `Material.paint_layers_is_stale`
   * keeps reporting stale for as long as what this starts is still running, whether or not a
   * debounce timer happened to be armed for \a ma already. */
  BKE_paint_layers_bake_scheduled_set(*ma, true);
  /* The exact calls #paint_layers_bake_debounce_tick makes, run now instead of waiting out the
   * rest of the window. \a ma's pending entry is left alone: the timer's own tick still fires and
   * finds nothing left to do, both calls being idempotent against a render already running. */
  material_bake_layered_rows_ensure(*bmain, *ma, /*force_preview=*/true);
  paint_layers_bake_job_start(*wm, CTX_wm_window(C), *bmain, *ma);
  if (!paint_layers_bake_debounce_pending().contains(ma->id.session_uid) &&
      paint_layers_bake_scheduled_settle_clears(paint_layers_bake_jobs_in_flight(*wm, *ma)))
  {
    BKE_paint_layers_bake_scheduled_set(*ma, false);
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  return OPERATOR_FINISHED;
}

}  // namespace

bool paint_layers_bake_job_is_excluded(const void *owner,
                                       const int job_type,
                                       const void *exclude_owner,
                                       const int exclude_job_type)
{
  return owner == exclude_owner && job_type == exclude_job_type;
}

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
  /* Backstop for the settle calls of the job free callbacks: it runs on every scene update, and
   * the per-material flag check inside makes an empty sweep cheap, so a material whose settle was
   * missed for any reason the exclusion in those callbacks does not foresee still settles on the
   * very next update. */
  paint_layers_bake_scheduled_settle(bmain, wm, nullptr, WM_JOB_TYPE_ANY);
  /* A localized or evaluated copy shares its description with the original, so only a material that
   * lives in the main database is baked. */
  const int no_bake_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & no_bake_tags) != 0 || !BKE_paint_layers_is_layered(ma) ||
        !ID_IS_EDITABLE(&ma.id))
    {
      continue;
    }
    /* Leaving the row that was edited ends its wait: what a preview was waiting for is now the
     * render at the row's own size, which is due at once. */
    {
      static Map<uint32_t, bUUID> last_active;
      const bUUID active = BKE_paint_layers_active_get(ma);
      const bUUID *seen = last_active.lookup_ptr(ma.id.session_uid);
      if (seen == nullptr || !BLI_uuid_equal(*seen, active)) {
        last_active.add_overwrite(ma.id.session_uid, active);
        if (seen != nullptr) {
          paint_layers_bake_debounce_pending().remove(ma.id.session_uid);
        }
      }
    }
    /* A material inside its debounce window is left alone: whatever it turns out to need is
     * started by the tick once the window closes. Everything else -- a bake left stale by a file
     * load, or by anything that never goes through #material_changed -- is still caught here on
     * every call; only a just-edited material's own renders are deferred. */
    if (paint_layers_bake_debounce_pending().contains(ma.id.session_uid)) {
      continue;
    }
    /* The rows that read another material are rendered from that material, a job of their own per
     * row and independent of the heavy planner's one job per layered material. */
    material_bake_layered_rows_ensure(bmain, ma);
    paint_layers_bake_job_start(wm, win, bmain, ma);
  }
}

bool ED_paint_layers_stale_or_pending(const wmWindowManager &wm, const Material &ma)
{
  return BKE_paint_layers_is_stale(ma) || paint_layers_bake_jobs_in_flight(wm, ma);
}

bool paint_layers_bake_scheduled_settle_clears(const bool jobs_in_flight)
{
  return !jobs_in_flight;
}

void paint_layers_bake_scheduled_settle(Main &bmain,
                                        wmWindowManager &wm,
                                        const void *exclude_owner,
                                        const int exclude_job_type)
{
  for (Material &ma : bmain.materials) {
    /* Cheap flag check first: the common case, called from every job's free callback whether or
     * not it has anything to do with the debounce, is that nothing is scheduled. */
    if (!BKE_paint_layers_bake_scheduled_get(ma)) {
      continue;
    }
    /* Still waiting in its debounce window: the tick decides, not a job that just freed. */
    if (paint_layers_bake_debounce_pending().contains(ma.id.session_uid)) {
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
  /* A linear scan: the pending set is a handful of just-edited materials at most, never worth a
   * lookup table built and torn down on every tick. */
  for (Material &ma : bmain.materials) {
    if (pending.contains(ma.id.session_uid)) {
      r_found.append(&ma);
    }
  }
}

bool paint_layers_bake_debounce_should_replace_timer()
{
  /* Arming while a timer already runs must restart its countdown from now, not let the earlier one
   * fire on schedule -- otherwise a continuous drag (a slider held for two seconds) would still
   * start a bake every 0.3s instead of once after the drag ends. */
  return BLI_timer_is_registered(paint_layers_bake_debounce_timer_uuid());
}

void paint_layers_bake_debounce_arm(Material &ma, const double delay_seconds)
{
  BKE_paint_layers_bake_scheduled_set(ma, true);
  const double delay = delay_seconds > 0.0 ? delay_seconds : paint_layers_bake_debounce_seconds;
  Map<uint32_t, double> &pending = paint_layers_bake_debounce_pending();
  /* Overwritten, not kept: each edit pushes the material's own time further out, so only a genuine
   * pause lets it come due. */
  pending.add_overwrite(ma.id.session_uid, BLI_time_now_seconds() + delay);
  /* The timer runs at the nearest time of everything waiting. */
  const double now = BLI_time_now_seconds();
  double nearest = delay;
  for (const double due : pending.values()) {
    nearest = std::min(nearest, std::max(due - now, 0.0));
  }
  if (paint_layers_bake_debounce_should_replace_timer()) {
    BLI_timer_unregister(paint_layers_bake_debounce_timer_uuid());
  }
  /* No window and no #wmEvent: this timer belongs to no editor. #BLI_timer runs it from the shared
   * main-loop hook (#BLI_timer_execute), the same mechanism `bpy.app.timers` uses. */
  BLI_timer_register(paint_layers_bake_debounce_timer_uuid(),
                     paint_layers_bake_debounce_tick,
                     nullptr,
                     nullptr,
                     nearest,
                     false);
}

void paint_layers_bake_debounce_reset()
{
  /* The pending set is process-static state tied to a file that is about to stop existing (a new
   * file, closing the file, quitting); a tick left armed would run against the next file's Main.
   */
  BLI_timer_unregister(paint_layers_bake_debounce_timer_uuid());
  paint_layers_bake_debounce_pending().clear();
}

void MATERIAL_OT_paint_layers_bake_now(wmOperatorType *ot)
{
  ot->name = "Bake Paint Layers Now";
  ot->idname = "MATERIAL_OT_paint_layers_bake_now";
  ot->description =
      "Start the renders this material's paint layers are waiting for now, instead of after the "
      "short pause that follows an edit";
  ot->exec = paint_layers_bake_now_exec;
  ot->poll = paint_layers_bake_now_poll;
  /* No #OPTYPE_UNDO: this starts background computation, it makes no user-visible edit to undo. */
  ot->flag = OPTYPE_REGISTER;
}

}  // namespace blender::ed::material_bake
