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
