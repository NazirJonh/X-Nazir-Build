/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

struct Image;
struct Main;

namespace blender::asset_system {
class AssetRepresentation;
}

namespace blender::ed::asset {

/**
 * Load \a filepath into an #Image, reusing an already loaded image for the same file (see
 * #BKE_image_load_exists), and record the automatic-update source baseline of the external file.
 * When \a unedited is set, an existing image with unsaved edits is left alone and a fresh copy is
 * loaded instead (see #BKE_image_load_exists_unedited).
 *
 * The load hands out a temporary user reference; drop it with #id_us_min() when the caller only
 * used the load as a loan.
 */
Image *image_load_for_import(Main &bmain, const char *filepath, bool *r_exists, bool unedited);

/** Whether the image can be marked as an asset in the current file. */
bool image_can_be_asset(const Image *image);

/**
 * Mark \a image as an asset in the current-file library and assign catalog path "Images".
 * \return true if the image was newly marked.
 */
bool image_mark_as_asset(Image *image);

/** Resolve a local, blend-library, or on-disk image asset to a loaded image. */
Image *resolve_image_from_asset(Main &bmain,
                                const asset_system::AssetRepresentation &asset);

}  // namespace blender::ed::asset
