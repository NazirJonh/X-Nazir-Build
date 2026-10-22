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
#include "ED_undo.hh"

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
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
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

/**
 * The blend-tier timer has its own uuid: it is not a bake and does not debounce one. Were it the
 * same timer, an armed bake debounce would be restarted or cleared by a blend scrub and a demotion
 * could ride on (or be lost to) a render that has nothing to do with it.
 */
char paint_layers_blend_timer_tag = 0;

uintptr_t paint_layers_blend_timer_uuid()
{
  return reinterpret_cast<uintptr_t>(&paint_layers_blend_timer_tag);
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
          wm, &ma, WM_JOB_TYPE_MATERIAL_IMAGES_BAKE, exclude_owner, exclude_job_type) ||
      paint_layers_bake_job_test_excluding(
          wm, &ma, WM_JOB_TYPE_PAINT_LAYERS_ROW_RESULT, exclude_owner, exclude_job_type))
  {
    return true;
  }
  if (!BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  bool row_running = false;
  BKE_paint_layers_foreach_source_row(ma, [&](const MaterialPaintLayer &row) {
    row_running = paint_layers_bake_job_test_excluding(
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

/**
 * The runtime blend tier is not part of a memfile snapshot, so a load or undo puts back a tree and
 * a persistent marker but no tier records. BKE reconciles the two; the editor then has nothing
 * left to demote and drops its timer rather than waking for a deadline that no longer exists.
 */
void paint_layers_blend_load_post(Main *bmain,
                                  PointerRNA ** /*pointers*/,
                                  const int /*pointers_num*/,
                                  void * /*arg*/)
{
  if (bmain == nullptr) {
    return;
  }
  BKE_paint_layers_blend_load_post(*bmain);
  paint_layers_blend_timer_reset();
}

void paint_layers_blend_undo_post(Main *bmain,
                                  PointerRNA ** /*pointers*/,
                                  const int /*pointers_num*/,
                                  void * /*arg*/)
{
  if (bmain == nullptr) {
    return;
  }
  BKE_paint_layers_blend_undo_post(*bmain);
  paint_layers_blend_timer_reset();
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

double paint_layers_blend_timer_tick(uintptr_t /*uuid*/, void * /*user_data*/)
{
  /* Same as the bake debounce tick: a #BLI_timer callback carries no context, so the main database
   * and its window manager are the ones the main loop is running with. */
  if (G_MAIN == nullptr || G_MAIN->wm.first == nullptr) {
    /* Without a main loop to demote with, the deadlines left in the BKE tier would fire against
     * whatever #Main comes next; drop them instead of letting a file inherit another file's
     * scrub. */
    BKE_paint_layers_blend_tier_clear();
    return -1.0;
  }
  const double now = BLI_time_now_seconds();
  /* BKE owns the records and decides what is due: it is the only layer that knows which channels
   * a material's mask really builds, and ED depends on BKE, never the reverse. */
  BKE_paint_layers_blend_tier_demote_due(*G_MAIN, now);
  /* The tick demotes only what has come due; the timer runs again for the nearest record left, or
   * removes itself when nothing is waiting. */
  const double next = BKE_paint_layers_blend_tier_next_deadline();
  return next < 0.0 ? -1.0 : std::max(next - now, 0.0);
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

/* -------------------------------------------------------------------- */
/** \name Use Row Result
 * \{ */

bool paint_layer_row_result_poll(bContext *C)
{
  const Material *owner = static_cast<const Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (owner == nullptr || !BKE_paint_layers_is_layered(*owner) ||
      BLI_uuid_is_nil(BKE_paint_layers_active_get(*owner)))
  {
    CTX_wm_operator_poll_msg_set(C, "No active paint layer");
    return false;
  }
  return true;
}

void paint_layer_row_result_start(void *customdata, wmJobWorkerStatus * /*worker_status*/)
{
  BKE_paint_layers_row_result_job_compute(*static_cast<PaintLayersRowResultJob *>(customdata));
}

void paint_layer_row_result_end(void *customdata)
{
  if (BKE_paint_layers_row_result_job_commit(*static_cast<PaintLayersRowResultJob *>(customdata)))
  {
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
  }
}

void paint_layer_row_result_free(void *customdata)
{
  BKE_paint_layers_row_result_job_free(*static_cast<PaintLayersRowResultJob *>(customdata));
}

/**
 * Render the result of \a source into the channel maps of \a target: on a worker for a heavy row or
 * a map of 2048 pixels and more, inline for a light one so an empty outcome can be reported.
 *
 * \return false when nothing was started or committed, with the reason reported to \a reports.
 */
bool paint_layer_row_result_run(Main &bmain,
                                wmWindowManager &wm,
                                wmWindow *win,
                                Material &owner,
                                const bUUID &source_marker,
                                const bUUID &target_marker,
                                const int size,
                                ReportList *reports,
                                bool *r_committed_inline = nullptr)
{
  PaintLayersRowResultJob *job = BKE_paint_layers_row_result_job_create(
      bmain, owner, source_marker, target_marker, size);
  if (job == nullptr) {
    BKE_report(reports, RPT_ERROR, "No row to take the result from");
    return false;
  }
  /* The threshold the bake planner uses. */
  const MaterialPaintLayer *source = BKE_paint_layers_find(owner, source_marker);
  const bool heavy = (source != nullptr && BKE_paint_layers_bake_is_heavy(owner, *source)) ||
                     size >= PAINT_LAYERS_HEAVY_BAKE_SIZE;
  if (heavy) {
    wmJob *wm_job = WM_jobs_get(&wm,
                                win,
                                &owner,
                                "Rendering row result...",
                                WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                                WM_JOB_TYPE_PAINT_LAYERS_ROW_RESULT);
    WM_jobs_customdata_set(wm_job, job, paint_layer_row_result_free);
    WM_jobs_timer(wm_job, 0.2, NC_MATERIAL, NC_MATERIAL);
    WM_jobs_callbacks(
        wm_job, paint_layer_row_result_start, nullptr, nullptr, paint_layer_row_result_end);
    WM_jobs_start(&wm, wm_job);
    return true;
  }

  BKE_paint_layers_row_result_job_compute(*job);
  const bool any = BKE_paint_layers_row_result_job_commit(*job);
  BKE_paint_layers_row_result_job_free(*job);
  if (!any) {
    BKE_report(reports, RPT_ERROR, "The row contributes to no channel here");
    return false;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &owner.id);
  if (r_committed_inline != nullptr) {
    *r_committed_inline = true;
  }
  return true;
}

/**
 * The Material rows of \a row, its corrections and the rows inside it (a folder's children) whose
 * maps have to be rendered before they are read. The result of a row is that row's own content: the
 * rows beside or below it in the same list are not part of it, so they are not rendered here.
 */
void paint_layer_row_result_forced_collect(const Material &ma,
                                           const MaterialPaintLayer &row,
                                           Vector<bUUID> &r_markers)
{
  if (BKE_paint_layers_material_row_render_forced_wanted(ma, row)) {
    r_markers.append(row.marker);
  }
  /* A Material correction renders its own maps and is read by the row's result as well. */
  for (const MaterialPaintLayer &effect : row.effects) {
    if (BKE_paint_layers_material_row_render_forced_wanted(ma, effect)) {
      r_markers.append(effect.marker);
    }
  }
  /* So does a mask correction that reads a material: its grey shapes the row's coverage. */
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (BKE_paint_layers_material_row_render_forced_wanted(ma, item)) {
      r_markers.append(item.marker);
    }
  }
  for (const MaterialPaintLayer &child : row.children) {
    paint_layer_row_result_forced_collect(ma, child, r_markers);
  }
}

/**
 * A "Use Row Result" waiting for the renders of the source's maps. The timer owns it; the material
 * is found again on every tick, since the file or the stack may change meanwhile.
 */
struct PaintLayerRowResultWait {
  Main *bmain = nullptr;
  Material *material = nullptr;
  uint32_t material_session_uid = 0;
  bUUID source_marker = {};
  bUUID target_marker = {};
  int size = 0;
  double deadline = 0.0;
};

double paint_layer_row_result_wait_tick(uintptr_t /*uuid*/, void *user_data)
{
  const PaintLayerRowResultWait &wait = *static_cast<PaintLayerRowResultWait *>(user_data);
  if (G_MAIN == nullptr || G_MAIN != wait.bmain || G_MAIN->wm.first == nullptr) {
    return -1.0;
  }
  Main &bmain = *G_MAIN;
  Material *ma = nullptr;
  for (Material &candidate : bmain.materials) {
    if (&candidate == wait.material && candidate.id.session_uid == wait.material_session_uid) {
      ma = &candidate;
      break;
    }
  }
  if (ma == nullptr) {
    return -1.0;
  }
  wmWindowManager &wm = *static_cast<wmWindowManager *>(bmain.wm.first);
  if (paint_layers_bake_jobs_in_flight(wm, *ma) && BLI_time_now_seconds() < wait.deadline) {
    return 0.25;
  }
  /* No operator is left to show a failure, so it goes to the status bar. */
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  if (!paint_layer_row_result_run(bmain,
                                  wm,
                                  static_cast<wmWindow *>(wm.windows.first),
                                  *ma,
                                  wait.source_marker,
                                  wait.target_marker,
                                  wait.size,
                                  &reports))
  {
    for (const Report &report : reports.list) {
      WM_global_report(eReportType(report.type), report.message);
    }
  }
  BKE_reports_free(&reports);
  return -1.0;
}

void paint_layer_row_result_wait_free(uintptr_t /*uuid*/, void *user_data)
{
  MEM_delete(static_cast<PaintLayerRowResultWait *>(user_data));
}

wmOperatorStatus paint_layer_row_result_exec(bContext *C, wmOperator *op)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  if (owner == nullptr || bmain == nullptr || wm == nullptr ||
      !BKE_paint_layers_is_layered(*owner))
  {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *target = BKE_paint_layers_find(*owner, BKE_paint_layers_active_get(*owner));
  if (target == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active paint layer");
    return OPERATOR_CANCELLED;
  }
  /* The source is named by its marker, the row's stable identity, so a menu drawn before the stack
   * changed cannot end up picking another row. Without one the target is updated from the row it
   * was last made from. */
  char source_marker_str[UUID_STRING_SIZE];
  RNA_string_get(op->ptr, "source_marker", source_marker_str);
  bUUID source_marker = target->row_result_source;
  if (source_marker_str[0] != '\0' && !BLI_uuid_parse_string(&source_marker, source_marker_str)) {
    source_marker = {};
  }
  const MaterialPaintLayer *source = BLI_uuid_is_nil(source_marker) ?
                                         nullptr :
                                         BKE_paint_layers_find(*owner, source_marker);
  if (source == nullptr) {
    BKE_report(op->reports,
               RPT_ERROR,
               BLI_uuid_is_nil(source_marker) ? "No source row was chosen for this layer yet" :
                                                "The source row no longer exists");
    return OPERATOR_CANCELLED;
  }

  int size = 1024;
  const Scene *scene = CTX_data_scene(C);
  if (scene != nullptr && scene->toolsettings != nullptr) {
    size = scene->toolsettings->paint_mode.new_channel_image_size;
  }
  if (size <= 0) {
    size = 1024;
  }

  /* A Material row the planner keeps live (Auto, or being edited) has rendered nothing but its
   * Normal, and a graph channel can only be read from a map. Render those maps first, then run
   * the job once the renders have landed. */
  Vector<bUUID> forced;
  paint_layer_row_result_forced_collect(*owner, *source, forced);
  if (!forced.is_empty()) {
    material_bake_layered_rows_ensure(*bmain, *owner, /*force_preview=*/true, forced);
    PaintLayerRowResultWait *wait = MEM_new<PaintLayerRowResultWait>(__func__);
    wait->bmain = bmain;
    wait->material = owner;
    wait->material_session_uid = owner->id.session_uid;
    wait->source_marker = source->marker;
    wait->target_marker = target->marker;
    wait->size = size;
    wait->deadline = BLI_time_now_seconds() + 300.0;
    BLI_timer_register(reinterpret_cast<uintptr_t>(wait),
                       paint_layer_row_result_wait_tick,
                       wait,
                       paint_layer_row_result_wait_free,
                       0.25,
                       false);
    BKE_report(op->reports, RPT_INFO, "Rendering the maps of the source, the result follows");
    return OPERATOR_FINISHED;
  }

  bool committed_inline = false;
  if (!paint_layer_row_result_run(*bmain,
                                  *wm,
                                  CTX_wm_window(C),
                                  *owner,
                                  source->marker,
                                  target->marker,
                                  size,
                                  op->reports,
                                  &committed_inline))
  {
    return OPERATOR_CANCELLED;
  }
  if (committed_inline) {
    ED_undo_push(C, "Use Row Result");
  }
  return OPERATOR_FINISHED;
}

/** \} */

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

  /* Registered from here so the blend tier is live wherever the paint-layer bake cleanup already
   * is -- this is the one editor-init entry point that runs after #BKE_callback_global_init. */
  paint_layers_blend_callbacks_init();
}

void paint_layers_blend_callbacks_init()
{
  bCallbackFuncStore *load_store = MEM_new<bCallbackFuncStore>(__func__);
  load_store->alloc = true;
  load_store->arg = nullptr;
  load_store->func = paint_layers_blend_load_post;
  BKE_callback_add(load_store, BKE_CB_EVT_LOAD_POST);

  bCallbackFuncStore *undo_store = MEM_new<bCallbackFuncStore>(__func__);
  undo_store->alloc = true;
  undo_store->arg = nullptr;
  undo_store->func = paint_layers_blend_undo_post;
  BKE_callback_add(undo_store, BKE_CB_EVT_UNDO_POST);
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

void paint_layers_blend_timer_arm()
{
  const double deadline = BKE_paint_layers_blend_tier_next_deadline();
  if (deadline < 0.0) {
    /* Nothing is scrubbing: leave any already-registered timer alone instead of churning it on
     * every material edit that has nothing to demote. */
    return;
  }
  const double delay = std::max(deadline - BLI_time_now_seconds(), 0.0);
  /* Re-registering an active #BLI_timer is not allowed, and arming must push the wake-up out to
   * the nearest deadline BKE now has (every scrub notes a later one), so the old timer goes. */
  if (BLI_timer_is_registered(paint_layers_blend_timer_uuid())) {
    BLI_timer_unregister(paint_layers_blend_timer_uuid());
  }
  /* No window and no #wmEvent: the tier timer belongs to no editor. #BLI_timer runs it from the
   * shared main-loop hook (#BLI_timer_execute), the same mechanism `bpy.app.timers` uses. */
  BLI_timer_register(paint_layers_blend_timer_uuid(),
                     paint_layers_blend_timer_tick,
                     nullptr,
                     nullptr,
                     delay,
                     false);
}

void paint_layers_blend_timer_reset()
{
  /* Process-static tier records tied to a file that is about to stop existing, or to one whose
   * tree a load/undo just rebuilt. A timer left armed would otherwise run against the next #Main,
   * and BKE's records would outlive the nodes they describe. */
  BLI_timer_unregister(paint_layers_blend_timer_uuid());
  BKE_paint_layers_blend_tier_clear();
}

void paint_layers_bake_debounce_reset()
{
  /* The pending set is process-static state tied to a file that is about to stop existing (a new
   * file, closing the file, quitting); a tick left armed would run against the next file's Main.
   */
  BLI_timer_unregister(paint_layers_bake_debounce_timer_uuid());
  paint_layers_bake_debounce_pending().clear();
  /* The blend tier has the same lifetime, and is reset here so every file change that covers the
   * bake debounce covers it too (#ED_editors_exit among them). */
  paint_layers_blend_timer_reset();
}

void MATERIAL_OT_paint_layer_use_row_result(wmOperatorType *ot)
{
  ot->name = "Use Row Result";
  ot->description =
      "Bake another row's result for every channel it takes part in into this row's channel maps";
  ot->idname = "MATERIAL_OT_paint_layer_use_row_result";
  /* No #OPTYPE_UNDO: a heavy row or a source whose maps are still rendering commits later, from a
   * job callback, so a step pushed at the call would not hold the change. The inline path pushes its
   * own step once it has written. */
  ot->flag = OPTYPE_REGISTER;
  ot->exec = paint_layer_row_result_exec;
  ot->poll = paint_layer_row_result_poll;

  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "source_marker",
                                     nullptr,
                                     UUID_STRING_SIZE,
                                     "Source Row",
                                     "Marker of the row whose result is baked");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
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
