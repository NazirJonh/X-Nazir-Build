/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Automatic updates for external resources used in the project (Asset Browser popover).
 *
 * Textures imported into the project by drag & drop (layer stack fill layer slots) are packed
 * into the .blend, so changes made to their external source files are not picked up. This module
 * keeps a baseline of every scanned image's source file in the #Image DNA and periodically
 * checks the files for changes in a background job. What the recorded baseline states are and
 * what each of them makes a scan do is described in #ImageSourceBaselineState (BKE_image.hh);
 * the DNA only stores the state of an image explicitly.
 *
 * Changed files are reloaded into the existing #Image through #BKE_image_signal() with
 * #IMA_SIGNAL_RELOAD, which re-reads the file from disk and replaces the packed data while
 * keeping the previous packed data when the file is no longer available.
 *
 * Known limitations: only #IMA_SRC_FILE images are handled (generated, viewer, render result,
 * sequences, movies and UDIM sets are skipped; supporting UDIM would need a baseline covering a
 * whole set of files, not a single #Image), and mtime has seconds granularity on most file
 * systems, so a same-size re-save within the same second is only caught once the file gets
 * hashed (a stat mismatch, or a manual update run which always hashes).
 *
 * The checks are scheduled by a global #BLI_timer callback that fires from
 * #wm_event_do_notifiers() on every main-loop iteration, independent of open windows or spaces.
 * It is registered as persistent, so file loads keep it alive, and it reads the current database
 * and settings on every fire. The settings live in the user preferences (Asset Browser →
 * Automatic Updates) and are exposed by a popover drawn in Python (#FILE_PT_asset_updates in
 * bl_ui/space_filebrowser.py); changing them through RNA re-arms the timer (#timer_sync()).
 */

#pragma once

#include <cstddef>

namespace blender {

struct Image;

namespace ed::asset_updates {

void operatortypes();

/**
 * Record the external source file stat baseline (mtime & size, cheap stat only -- hashing here
 * would block the drop handler) of a freshly imported image. Does nothing when the image
 * already has a baseline or doesn't reference an external file. Called from the common image
 * import helper #blender::ed::asset::image_load_for_import(); the first update scan completes
 * the baseline with the content hash.
 */
void image_import_record_baseline(Image *image);

/**
 * Make the periodic timer match the user preferences: register it when the feature is enabled,
 * drop it when not, and re-arm it when the interval changed. Called from #WM_init (after the
 * preferences were read), from the RNA update callback of the preferences properties and after
 * loading/reverting preferences (which replaces #U without running those callbacks).
 */
void timer_sync();

/** Copy a human-readable "how long ago the last update check ran" line (translated) into
 * \a r_text, or "Not checked yet" when a check never ran. Backs the read-only
 * `AssetUpdatesStatus.last_check_ago` RNA property the #FILE_PT_asset_updates popover shows. */
void last_check_ago_get(char *r_text, size_t text_maxlen);

/** Copy the "N checked, N reloaded, ..." counter line of the last update check into \a r_text;
 * empty when a check never ran. */
void last_check_summary_get(char *r_text, size_t text_maxlen);

/** Drop the module state (the timer, the last-scan status and the remembered skipped file
 * versions). */
void storage_exit();

}  // namespace ed::asset_updates

}  // namespace blender
