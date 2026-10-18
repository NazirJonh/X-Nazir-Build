/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * See #ED_paint_layers_bake.hh: the wmJob half of the heavy paint-layer bake planner. It lives
 * beside the material bake because it is a bake job, and the window manager is kept out of
 * paint-layer knowledge.
 */

#include "ED_paint_layers_bake.hh"

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "MEM_guardedalloc.h"

#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint_layers.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"

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
 * thread alone touches this: the scheduler and the free callback both run there.
 */
Set<uint32_t> &paint_layers_bake_active()
{
  static Set<uint32_t> active;
  return active;
}

void paint_layers_bake_start(void *customdata, wmJobWorkerStatus *worker_status)
{
  PaintLayersBakeWMJob *job = static_cast<PaintLayersBakeWMJob *>(customdata);
  /* The worker only reads the localized copy and shared images; every write waits for the main
   * thread in #paint_layers_bake_end. #WM_JOB_PROGRESS (set on the job in
   * #paint_layers_bake_jobs_ensure) is what makes the window manager read the progress at all. */
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
  MEM_delete(job);
}

}  // namespace

void paint_layers_bake_jobs_ensure(wmWindowManager &wm, wmWindow *win, Main &bmain)
{
  /* A job needs a window to own its progress and timer; background mode has none, and nothing there
   * would redraw the result anyway. */
  if (win == nullptr) {
    return;
  }
  /* A localized or evaluated copy shares its description with the original, so only a material that
   * lives in the main database is baked. */
  const int no_bake_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & no_bake_tags) != 0 || !BKE_paint_layers_is_layered(ma) ||
        !ID_IS_EDITABLE(&ma.id) || paint_layers_bake_active().contains(ma.id.session_uid))
    {
      continue;
    }
    if (!BKE_paint_layers_bake_heavy_pending(ma)) {
      continue;
    }
    PaintLayersBakeJob *bake = BKE_paint_layers_bake_job_create(bmain, ma);
    if (bake == nullptr) {
      /* Nothing could actually be queued (no size, no localized copy): leave it to the next poll
       * rather than marking the material active. */
      continue;
    }
    PaintLayersBakeWMJob *job = MEM_new<PaintLayersBakeWMJob>(__func__);
    job->material_session_uid = ma.id.session_uid;
    job->bake = bake;
    /* #WM_JOB_PROGRESS is what makes #WM_jobs_test report this job as running at all, and what
     * feeds the window's progress bar and the running-jobs indicator. */
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
    WM_jobs_start(&wm, wm_job);
  }
}

}  // namespace blender::ed::material_bake
