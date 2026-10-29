/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Shared state and contract of the automatic asset updates module. See #ED_asset_updates.hh for
 * the general design; the implementation is split into:
 * - the module state and the settings snapshot (#asset_updates_state.cc),
 * - the scan job: stat/hash caches, statuses and reporting (#asset_updates_scan.cc),
 * - the image half of the scan: which resources are scanned and how they are updated
 *   (#asset_updates_image.cc),
 * - the periodic timer (#asset_updates_timer.cc),
 * - the operators and the status getters (#asset_updates_ui.cc).
 *
 * The scan job does not care what a resource is: it compares stat data and hashes through
 * #ScanItem/#ScanResult and hands the results back, while the image file owns everything that
 * knows about #Image. A second resource type follows the same contract.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "BKE_image.hh"

namespace blender {

/* Declared here so the signatures below do not depend on which DNA headers a caller happens to
 * include first. #Image and #Main come from #BKE_image.hh. */
struct UserDef;
struct wmWindow;
struct wmWindowManager;

namespace ed::asset_updates {

/* -------------------------------------------------------------------- */
/** \name Settings
 * \{ */

/** Snapshot of the user preferences the module acts on. */
struct UpdatesSettings {
  bool refresh_assets_panel = false;
  bool update_project_resources = false;
  int interval_min = 1;
  bool skip_mismatch = true;
};

UpdatesSettings settings_from_userdef(const UserDef &userdef);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scan job
 * \{ */

/** How a scan was started; selects the comparisons the job performs. */
enum class ScanMode {
  /** Periodic check: a file is only hashed when its stat data moved on, and images without a
   * baseline are only adopted. */
  Automatic,
  /** Update operator: every file is hashed, and images without a baseline are reloaded too. */
  Manual,
};

/** One project resource (image) snapshot, built on the main thread before the scan starts. */
struct ScanItem {
  uint32_t session_uid;
  /** Absolute source file path, resolved from #Image::filepath. */
  std::string filepath;
  ImageSourceBaselineState baseline_state = ImageSourceBaselineState::None;
  int64_t baseline_mtime = 0;
  int64_t baseline_size = 0;
  char baseline_hash[33] = "";
  /** Stat data of the file version whose reload was already refused on an earlier scan. While
   * the file still matches it, the scan neither hashes nor retries (see
   * #ScanStatus::PreviouslySkipped). */
  bool has_skipped_version = false;
  int64_t skipped_mtime = 0;
  int64_t skipped_size = 0;
};

enum class ScanStatus {
  /** Stat data matches the baseline. */
  Unchanged,
  /** Stat data changed but the content hash is still the same (e.g. identical re-save). */
  SameContent,
  /** No comparable baseline existed, the scan records one (whatever the result carries).
   * Never reloads by itself; manual runs only reload items that had no baseline at all. */
  Adopt,
  /** Content hash differs from the baseline, the image should be reloaded. */
  Changed,
  /** The source file is not available on disk. The image content is never touched. */
  Missing,
  /** The source file exists but could not be read, so its content cannot be compared. The image
   * content is never touched, the item is counted as missing. */
  Unreadable,
  /** This exact file version was refused by an earlier scan (see #image_can_reload) and has not
   * changed since, so it is not retried or logged again. Counted as skipped in the status. */
  PreviouslySkipped,
};

struct ScanResult {
  int item_index;
  ScanStatus status;
  int64_t mtime = 0;
  int64_t size = 0;
  char hash[33] = "";
  /** Pixel dimensions of the file on disk, read in the job (header only) whenever the result
   * ends in a reload, so the dimension check runs without touching the disk from the main
   * thread. */
  int new_width = 0;
  int new_height = 0;
  bool has_new_size = false;
};

/** File version whose reload was already refused (unsaved edits or mismatching parameters, see
 * #image_can_reload). Runtime state, not DNA. */
struct SkippedVersion {
  int64_t mtime = 0;
  int64_t size = 0;
};

struct ScanJob {
  Vector<ScanItem> items;
  Vector<ScanResult> results;
  /** Main database the snapshot was built from. Jobs are killed on file loads, but this also
   * guards against any other main swap between #scan_startjob and #scan_endjob. */
  Main *bmain = nullptr;
  ScanMode mode = ScanMode::Automatic;
  bool skip_mismatch = true;

  /* Per-path caches. Several images can share one source file; stat it and hash it once per
   * scan. Only accessed from the (single) job worker thread. */
  struct ScannedFileStat {
    bool exists = false;
    int64_t mtime = 0;
    int64_t size = 0;
  };
  Map<std::string, ScannedFileStat> stat_cache;
  /** MD5 hex digest per path; an empty string means the file could not be read. */
  Map<std::string, std::string> hash_cache;
};

/** \return true when an update scan job is running. */
bool scan_job_running(wmWindowManager &wm);
/** \return true when a scan was started, false when one is already running or there is nothing
 * to check. */
bool scan_job_start(wmWindowManager &wm, wmWindow *window, ScanMode mode);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Timer
 * \{ */

/** Drop the timer registration, if any. Called by #storage_exit(). */
void timer_unregister();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image resources
 *
 * The image specific half of the scan: what to snapshot, when an image may be reloaded and how
 * its baseline follows a successful reload.
 * \{ */

/** Snapshot every image of \a bmain whose content comes from an external file. Main thread only. */
void image_items_build(Main &bmain, ScanJob &job);

/** Read the pixel dimensions of \a item's file (header only) into \a result, for results that
 * end in a reload, so #image_can_reload can compare them without file access on the main
 * thread. */
void image_result_read_new_size(const ScanItem &item, ScanResult &result);

/** \return true when \a image may be reloaded from disk: unsaved edits are never clobbered, and
 * when the user asked for it, files whose parameters (dimensions) changed are skipped too. */
bool image_can_reload(Image &image, const ScanResult &result, bool skip_mismatch);

/**
 * Reload \a image from its source file and record the scanned stat data as its new baseline.
 *
 * \return false when the file changed again (or vanished) between the background scan and the
 * reload. The reload already cleared the baseline (see #BKE_image_signal), so the next scan
 * re-records it from disk instead of retrying the reload.
 */
bool image_apply_update(Main &bmain, Image &image, const ScanItem &item, const ScanResult &result);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Last check status
 * \{ */

/** Counters of the last completed scan, shown in the popover. */
struct LastScanInfo {
  double timestamp = 0.0; /* #BLI_time_now_seconds() of the end of the last scan; 0 = not run. */
  int checked_num = 0;
  int reloaded_num = 0;
  int skipped_num = 0;
  int missing_num = 0;
  /** Images an automatic scan left for the next pass because of its reload limit. */
  int deferred_num = 0;
};

/** Copy a human-readable "how long ago the last update check ran" line (translated) into
 * \a r_text, or "Not checked yet" when a check never ran. */
void last_check_ago_get(char *r_text, size_t text_maxlen);

/** Copy the "N checked, N reloaded, ..." counter line of the last update check into \a r_text;
 * empty when a check never ran. */
void last_check_summary_get(char *r_text, size_t text_maxlen);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Module state
 * \{ */

/** Everything that has to outlive a single scan. Created on first use and freed by
 * #storage_exit(): the containers below use the guarded allocator, so they must not survive
 * #MEM_exit and be reported as leaks. */
struct ModuleState {
  /** Outcome of the last completed scan. Written by #scan_endjob, read by the panel draw and the
   * status getters; main thread only. */
  LastScanInfo last_scan;
  /** Missing source files already reported to the window manager, so a file that stays missing
   * does not raise a report on every scan. Main thread only (see #scan_endjob). */
  Set<std::string> reported_missing;
  /** File versions whose reload was refused, per image session UID (see
   * #ScanStatus::PreviouslySkipped). Main thread only. */
  Map<uint32_t, SkippedVersion> skipped_versions;
  /** Interval the timer is currently armed with, in seconds; 0 when it is not registered. */
  double timer_interval = 0.0;
};

/** The module state, created on first use and freed by #storage_exit(). */
ModuleState &state();

/** \} */

/** Refresh every asset browser currently visible in any window, like #ASSET_OT_library_refresh
 * does for the context's library. */
void refresh_visible_asset_browsers(wmWindowManager &wm);

}  // namespace ed::asset_updates

}  // namespace blender
