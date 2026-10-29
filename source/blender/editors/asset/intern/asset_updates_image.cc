/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * The image half of the project resource scan: which images are snapshotted, when a scanned
 * change may be applied and how the baseline follows a successful reload. See
 * #ED_asset_updates.hh for the general design and #asset_updates_scan.cc for the job frame that
 * drives these functions.
 *
 * The per image baseline (mtime, size, MD5 of the source file contents) lives in the #Image DNA
 * fields and is only touched through the #BKE_image_source_baseline_* API.
 */

#include "ED_asset_updates.hh"

#include <cstdint>
#include <string>
#include <utility>

#include "CLG_log.h"

#include "DNA_ID.h"
#include "DNA_image_types.h"

#include "BKE_image.hh"
#include "BKE_main.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "BLI_fileops.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"

#include "asset_updates_intern.hh"

namespace blender::ed::asset_updates {

static CLG_LogRef LOG = {"asset.updates"};

/* -------------------------------------------------------------------- */
/** \name Resource snapshot
 * \{ */

void image_items_build(Main &bmain, ScanJob &job)
{
  for (Image &image : bmain.images) {
    if (ID_IS_LINKED(&image.id) || ID_IS_OVERRIDE_LIBRARY(&image.id)) {
      /* Library-linked and overridden data-blocks are owned by their source library, reloading
       * them here would clobber the linked data. */
      continue;
    }
    if (image.source != IMA_SRC_FILE) {
      /* Generated, viewer, render result, sequences, movies and UDIM sets are not handled. */
      continue;
    }
    if (image.filepath[0] == '\0') {
      continue;
    }

    ImageUser iuser{};
    BKE_imageuser_default(&iuser);
    char filepath[FILE_MAX];
    BKE_image_user_file_path(&iuser, &image, filepath);
    if (filepath[0] == '\0') {
      continue;
    }

    ScanItem item;
    item.session_uid = image.id.session_uid;
    item.filepath = filepath;
    item.baseline_state = BKE_image_source_baseline_state(image);
    BKE_image_source_baseline_get(
        image, &item.baseline_mtime, &item.baseline_size, item.baseline_hash);
    if (const SkippedVersion *skipped = state().skipped_versions.lookup_ptr(item.session_uid)) {
      item.has_skipped_version = true;
      item.skipped_mtime = skipped->mtime;
      item.skipped_size = skipped->size;
    }
    job.items.append(std::move(item));
  }
}

void image_result_read_new_size(const ScanItem &item, ScanResult &result)
{
  ImBuf *new_ibuf = IMB_load_image_from_filepath(item.filepath.c_str(), ImBufFlags::Test);
  if (new_ibuf != nullptr) {
    result.new_width = new_ibuf->x;
    result.new_height = new_ibuf->y;
    result.has_new_size = true;
  }
  IMB_freeImBuf(new_ibuf);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Applying a scan result
 * \{ */

static void image_reload_from_disk(Main &bmain, Image &image)
{
  BKE_image_clear_autosave(&image);

  ImageUser iuser{};
  BKE_imageuser_default(&iuser);
  BKE_image_signal(&bmain, &image, &iuser, IMA_SIGNAL_RELOAD);

  DEG_id_tag_update(&image.id, ID_RECALC_EDITORS | ID_RECALC_SOURCE);
  /* Same notifier as the image editor's Reload operator: GPU textures and image users (the layer
   * stack included) refresh on this. */
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, &image);
}

bool image_can_reload(Image &image, const ScanResult &result, const bool skip_mismatch)
{
  if (BKE_image_is_dirty(&image)) {
    return false;
  }
  if (!skip_mismatch || !result.has_new_size) {
    return true;
  }
  ImBuf *current_ibuf = BKE_image_get_first_ibuf(&image);
  if (current_ibuf == nullptr) {
    return true;
  }
  const bool match = (current_ibuf->x == result.new_width &&
                      current_ibuf->y == result.new_height);
  BKE_image_release_ibuf(&image, current_ibuf, nullptr);
  return match;
}

bool image_apply_update(Main &bmain, Image &image, const ScanItem &item, const ScanResult &result)
{
  image_reload_from_disk(bmain, image);

  /* TOCTOU: the hash was computed in the background job, the file could have moved on again
   * before the reload read it. Only trust the scan result when the file still matches it. */
  BLI_stat_t file_stat;
  if (BLI_stat(item.filepath.c_str(), &file_stat) == -1 ||
      int64_t(file_stat.st_mtime) != result.mtime || int64_t(file_stat.st_size) != result.size)
  {
    CLOG_WARN(&LOG,
              "Not updating the baseline of \"%s\": its file changed while it was reloaded",
              image.id.name + 2);
    return false;
  }
  /* The content came from the file just reloaded: the baseline is verified now. */
  BKE_image_source_baseline_set(
      image, result.mtime, result.size, result.hash, ImageSourceBaselineState::Full);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Import hook
 * \{ */

void image_import_record_baseline(Image *image)
{
  if (image == nullptr || image->source != IMA_SRC_FILE) {
    return;
  }
  if (BKE_image_source_baseline_state(*image) != ImageSourceBaselineState::None) {
    /* Baseline already recorded (re-drop of an already imported image). */
    return;
  }
  if (image->filepath[0] == '\0') {
    return;
  }

  ImageUser iuser{};
  BKE_imageuser_default(&iuser);
  char filepath[FILE_MAX];
  BKE_image_user_file_path(&iuser, image, filepath);
  if (filepath[0] == '\0') {
    return;
  }

  BLI_stat_t file_stat;
  if (BLI_stat(filepath, &file_stat) == -1) {
    return;
  }
  /* The image content was just loaded from this file, the stat baseline is trustworthy. */
  BKE_image_source_baseline_set(*image,
                                int64_t(file_stat.st_mtime),
                                int64_t(file_stat.st_size),
                                "",
                                ImageSourceBaselineState::StatOnly);
}

/** \} */

}  // namespace blender::ed::asset_updates
