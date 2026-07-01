/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

struct Image;

namespace blender::ed::asset {

/** Whether the image can be marked as an asset in the current file. */
bool image_can_be_asset(const Image *image);

/**
 * Mark \a image as an asset in the current-file library and assign catalog path "Images".
 * \return true if the image was newly marked.
 */
bool image_mark_as_asset(Image *image);

}  // namespace blender::ed::asset
