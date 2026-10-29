/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * The background scan job of the automatic asset updates: the frame that stats and hashes the
 * snapshotted resources, turns the results into statuses and reports them. See
 * #ED_asset_updates.hh for the general design; everything that knows about #Image lives in
 * #asset_updates_image.cc, this file only knows #ScanItem and #ScanResult.
 *
 * All hashing happens in this job; a scan only hashes a file when its stat data differs from the
 * baseline (or a manual run forces a content check), so a steady state scan is just one stat()
 * call per image.
 */

#include "ED_asset_updates.hh"

#include <cstdint>
#include <string>
#include <utility>

#include "CLG_log.h"

#include "DNA_image_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "ED_render.hh"

#include "BLI_fileops.h"
#include "BLI_hash_md5.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "WM_api.hh"

#include "BLT_translation.hh"

#include "asset_updates_intern.hh"

namespace blender::ed::asset_updates {

static CLG_LogRef LOG = {"asset.updates"};

/**
 * How many images one automatic scan reloads before it leaves the rest for the next pass.
 * Reloading re-packs and decodes the file on the main thread, so applying every changed image of
 * a scene with dozens of 4K textures would freeze the UI for the whole batch. The deferred images
 * keep their old baseline, so the next automatic scan reports them as changed again and needs no
 * extra bookkeeping.
 */
static constexpr int SCAN_AUTO_RELOAD_LIMIT = 4;

/* -------------------------------------------------------------------- */
/** \name Resource scan job
 * \{ */

static bool file_digest_compute(const char *filepath, char r_hexdigest[33])
{
  FILE *file = BLI_fopen(filepath, "rb");
  if (file == nullptr) {
    return false;
  }
  unsigned char digest[16];
  if (BLI_hash_md5_stream(file, digest) != 0) {
    fclose(file);
    return false;
  }
  fclose(file);
  BLI_hash_md5_to_hexdigest(digest, r_hexdigest);
  return true;
}

static const ScanJob::ScannedFileStat &scan_stat_cached(ScanJob &job, const std::string &filepath)
{
  if (const ScanJob::ScannedFileStat *cached = job.stat_cache.lookup_ptr(filepath)) {
    return *cached;
  }
  ScanJob::ScannedFileStat result;
  BLI_stat_t file_stat;
  if (BLI_stat(filepath.c_str(), &file_stat) != -1) {
    result.exists = true;
    result.mtime = int64_t(file_stat.st_mtime);
    result.size = int64_t(file_stat.st_size);
  }
  job.stat_cache.add(filepath, result);
  return job.stat_cache.lookup(filepath);
}

static const std::string &scan_hash_cached(ScanJob &job, const std::string &filepath)
{
  if (const std::string *cached = job.hash_cache.lookup_ptr(filepath)) {
    return *cached;
  }
  std::string hash;
  char hexdigest[33];
  if (file_digest_compute(filepath.c_str(), hexdigest)) {
    hash = hexdigest;
  }
  job.hash_cache.add(filepath, std::move(hash));
  return job.hash_cache.lookup(filepath);
}

static void scan_startjob(void *customdata, wmJobWorkerStatus *worker_status)
{
  ScanJob &job = *static_cast<ScanJob *>(customdata);

  for (const int item_index : job.items.index_range()) {
    if (worker_status->stop) {
      break;
    }
    const ScanItem &item = job.items[item_index];

    ScanResult result;
    result.item_index = item_index;

    const ScanJob::ScannedFileStat &file_stat = scan_stat_cached(job, item.filepath);
    if (!file_stat.exists) {
      result.status = ScanStatus::Missing;
      job.results.append(result);
      continue;
    }
    result.mtime = file_stat.mtime;
    result.size = file_stat.size;

    const bool stat_matches = (result.mtime == item.baseline_mtime &&
                               result.size == item.baseline_size);
    const bool manual = job.mode == ScanMode::Manual;

    if (item.baseline_state == ImageSourceBaselineState::None) {
      /* Nothing recorded at all: adopt the current stat data without hashing or reloading, so
       * enabling the feature never causes a bulk hash/reload of pre-existing images. The image
       * gets a #ImageSourceBaselineState::Adopted baseline (see #scan_endjob handling), a manual
       * run hashes and reloads it. */
      result.status = ScanStatus::Adopt;
      if (manual) {
        const std::string &hash = scan_hash_cached(job, item.filepath);
        if (hash.empty()) {
          result.status = ScanStatus::Unreadable;
        }
        else {
          STRNCPY(result.hash, hash.c_str());
          /* A manual run reloads images without a baseline (see #scan_endjob). */
          image_result_read_new_size(item, result);
        }
      }
      job.results.append(result);
      continue;
    }

    if (item.baseline_state == ImageSourceBaselineState::StatOnly && stat_matches) {
      /* The import-time record still matches: complete it with the content hash without
       * reloading. A changed stat falls through to the hashing below and reloads. */
      const std::string &hash = scan_hash_cached(job, item.filepath);
      if (hash.empty()) {
        result.status = ScanStatus::Unreadable;
      }
      else {
        STRNCPY(result.hash, hash.c_str());
        result.status = ScanStatus::Adopt;
      }
      job.results.append(result);
      continue;
    }

    if (stat_matches && !manual) {
      result.status = ScanStatus::Unchanged;
      job.results.append(result);
      continue;
    }

    if (!manual && item.has_skipped_version && result.mtime == item.skipped_mtime &&
        result.size == item.skipped_size)
    {
      /* This exact file version was already refused by an earlier scan (see #image_can_reload):
       * don't hash or retry it until the file changes again. */
      result.status = ScanStatus::PreviouslySkipped;
      job.results.append(result);
      continue;
    }

    /* The stat data moved on since the last check (or a manual run forced a content check):
     * hash the file to tell an identical re-save apart from a real content change. */
    const std::string &hash = scan_hash_cached(job, item.filepath);
    if (hash.empty()) {
      result.status = ScanStatus::Unreadable;
      job.results.append(result);
      continue;
    }
    STRNCPY(result.hash, hash.c_str());
    result.status = STREQ(result.hash, item.baseline_hash) ? ScanStatus::SameContent :
                                                             ScanStatus::Changed;
    if (result.status == ScanStatus::Changed) {
      image_result_read_new_size(item, result);
    }
    job.results.append(result);
  }

  worker_status->do_update = true;
}

/** A source file the scan could not use: gone from disk, or present but unreadable. */
struct MissingSource {
  std::string path;
  bool unreadable;
};

static void scan_endjob(void *customdata)
{
  ScanJob &job = *static_cast<ScanJob *>(customdata);
  /* The job is killed on file loads (wm_files.cc); a swapped-out main freed every scanned
   * image, so dropping the results is correct. */
  if (job.bmain != G_MAIN) {
    return;
  }
  Main *bmain = job.bmain;
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);

  /* The results reference images by session UID. Resolve them once instead of scanning all of
   * Main per result. */
  Map<uint32_t, Image *> images_by_uid;
  for (Image &image : bmain->images) {
    images_by_uid.add(image.id.session_uid, &image);
  }

  struct PendingReload {
    Image *image;
    const ScanItem *item;
    const ScanResult *result;
  };
  Vector<PendingReload> pending_reloads;
  Vector<MissingSource> missing_paths;
  int applied_num = 0;
  int skipped_num = 0;
  int missing_num = 0;

  for (const ScanResult &result : job.results) {
    const ScanItem &item = job.items[result.item_index];
    Image **image_ptr = images_by_uid.lookup_ptr(item.session_uid);
    if (image_ptr == nullptr) {
      /* Deleted while the scan was running. */
      continue;
    }
    Image *image = *image_ptr;

    /* Any outcome other than "still the refused version" means that file version is gone:
     * re-arm the retry (and the warning) for whatever replaced it. */
    if (result.status != ScanStatus::PreviouslySkipped) {
      state().skipped_versions.remove(item.session_uid);
    }

    switch (result.status) {
      case ScanStatus::Unchanged:
        break;
      case ScanStatus::PreviouslySkipped:
        /* Still the file version whose reload was refused earlier: count it as skipped again,
         * but don't retry it (the job didn't even hash it) or log it again. */
        skipped_num++;
        break;
      case ScanStatus::SameContent:
        /* Identical re-save: refresh the stat data so future scans don't keep hashing this
         * file over and over. */
        BKE_image_source_baseline_set(
            *image, result.mtime, result.size, result.hash, ImageSourceBaselineState::Full);
        break;
      case ScanStatus::Missing:
        /* The packed data inside the .blend stays untouched. */
        missing_num++;
        missing_paths.append({item.filepath, false});
        break;
      case ScanStatus::Unreadable:
        /* Counted as missing: the file is there but its content cannot be checked or reloaded. */
        missing_num++;
        missing_paths.append({item.filepath, true});
        break;
      case ScanStatus::Adopt:
        if (wm != nullptr && job.mode == ScanMode::Manual &&
            item.baseline_state == ImageSourceBaselineState::None)
        {
          /* The manual run asked to sync images whose state was entirely unknown. The baseline
           * is only recorded once the reload actually succeeded (see #image_apply_update), so a
           * skipped reload (dirty image, mismatching parameters) is retried on a later run
           * instead of silently declaring the old packed content up-to-date. */
          pending_reloads.append({image, &item, &result});
        }
        else if (item.baseline_state == ImageSourceBaselineState::None) {
          /* An image that had no baseline at all gets its stat data from disk without its
           * content being compared, and stays marked as such so a manual update run can still
           * force a reload. */
          BKE_image_source_baseline_set(*image,
                                        result.mtime,
                                        result.size,
                                        result.hash,
                                        ImageSourceBaselineState::Adopted);
        }
        else {
          /* The stat baseline recorded at import time was completed with the content hash, so
           * it is verified from now on. */
          BKE_image_source_baseline_set(
              *image, result.mtime, result.size, result.hash, ImageSourceBaselineState::Full);
        }
        break;
      case ScanStatus::Changed:
        if (wm != nullptr) {
          pending_reloads.append({image, &item, &result});
        }
        break;
    }
  }

  /* Reloading frees the image buffers, preview jobs may still be using them (same as
   * #IMAGE_OT_reload). Stop them once for the whole batch, not per image. */
  if (!pending_reloads.is_empty()) {
    ED_preview_kill_jobs(wm, bmain);
  }

  int deferred_num = 0;
  for (const int request_index : pending_reloads.index_range()) {
    const PendingReload &request = pending_reloads[request_index];
    if (job.mode == ScanMode::Automatic && applied_num >= SCAN_AUTO_RELOAD_LIMIT) {
      /* Leave the rest to the next automatic scan: their baselines stay untouched, so they are
       * reported as changed again. */
      deferred_num = int(pending_reloads.size()) - request_index;
      break;
    }
    if (!image_can_reload(*request.image, *request.result, job.skip_mismatch)) {
      skipped_num++;
      /* Remember the refused version so the next scans don't hash and log it again until the
       * file changes (see #ScanStatus::PreviouslySkipped). */
      state().skipped_versions.add_overwrite(request.image->id.session_uid,
                                             {request.result->mtime, request.result->size});
      if (BKE_image_is_dirty(request.image)) {
        CLOG_WARN(&LOG, "Skipping \"%s\": it has unsaved edits", request.image->id.name + 2);
      }
      else {
        CLOG_WARN(&LOG,
                  "Skipping \"%s\": its file parameters changed",
                  request.image->id.name + 2);
      }
      continue;
    }
    if (image_apply_update(*bmain, *request.image, *request.item, *request.result)) {
      applied_num++;
    }
    else {
      skipped_num++;
    }
  }

  /* Images deleted while scanning leave their skipped version behind; drop those so the map
   * doesn't grow with stale entries. */
  state().skipped_versions.remove_if(
      [&](const auto item) { return !images_by_uid.contains(item.key); });

  /* Surface newly missing source files as a UI report (banner + Info editor), not only in the
   * console log. The scan runs without an operator context, so #WM_global_reportf is the only
   * way to report them; deduplicate across runs so a file that stays missing is reported once. */
  Set<std::string> missing_now;
  Vector<const MissingSource *> newly_missing;
  for (const MissingSource &missing : missing_paths) {
    missing_now.add(missing.path);
    if (!state().reported_missing.contains(missing.path)) {
      newly_missing.append(&missing);
    }
  }
  if (!newly_missing.is_empty()) {
    if (newly_missing.size() == 1) {
      if (newly_missing[0]->unreadable) {
        WM_global_reportf(
            RPT_WARNING, "Source file cannot be read: \"%s\"", newly_missing[0]->path.c_str());
      }
      else {
        WM_global_reportf(
            RPT_WARNING, "Source file missing: \"%s\"", newly_missing[0]->path.c_str());
      }
    }
    else {
      WM_global_reportf(RPT_WARNING,
                        "%d source files are missing or unreadable (first: \"%s\")",
                        int(newly_missing.size()),
                        newly_missing[0]->path.c_str());
    }
  }
  state().reported_missing = std::move(missing_now);

  ModuleState &module_state = state();
  module_state.last_scan.timestamp = BLI_time_now_seconds();
  module_state.last_scan.checked_num = int(job.results.size());
  module_state.last_scan.reloaded_num = applied_num;
  module_state.last_scan.skipped_num = skipped_num;
  module_state.last_scan.missing_num = missing_num;
  module_state.last_scan.deferred_num = deferred_num;

  CLOG_INFO(&LOG,
            "%d image(s) checked, %d reloaded, %d skipped, %d source file(s) missing, %d deferred",
            module_state.last_scan.checked_num,
            applied_num,
            skipped_num,
            missing_num,
            deferred_num);
  /* Refresh the file/asset spaces so the popover status line picks up the new numbers. The
   * counters change on every completed check, even when nothing was reloaded or missing. */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_FILE_PARAMS, nullptr);
}

bool scan_job_running(wmWindowManager &wm)
{
  return WM_jobs_test(&wm, G_MAIN, WM_JOB_TYPE_ASSET_UPDATES_SCAN);
}

bool scan_job_start(wmWindowManager &wm, wmWindow *window, const ScanMode mode)
{
  if (scan_job_running(wm)) {
    return false;
  }
  if (window == nullptr) {
    window = static_cast<wmWindow *>(wm.windows.first);
  }
  if (window == nullptr) {
    /* Background mode without any window: nothing to attach the job to. */
    return false;
  }

  ScanJob *job = new ScanJob();
  job->bmain = G_MAIN;
  job->mode = mode;
  job->skip_mismatch = settings_from_userdef(U).skip_mismatch;
  image_items_build(*job->bmain, *job);
  if (job->items.is_empty()) {
    delete job;
    return false;
  }

  wmJob *wm_job = WM_jobs_get(&wm,
                              window,
                              G_MAIN,
                              IFACE_("Checking external resources"),
                              eWM_JobFlag(0),
                              WM_JOB_TYPE_ASSET_UPDATES_SCAN);
  WM_jobs_customdata_set(wm_job, job, [](void *data) { delete static_cast<ScanJob *>(data); });
  WM_jobs_callbacks(wm_job, scan_startjob, nullptr, nullptr, scan_endjob);
  WM_jobs_start(&wm, wm_job);
  return true;
}

/** \} */

}  // namespace blender::ed::asset_updates
