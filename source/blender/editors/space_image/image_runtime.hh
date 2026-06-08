/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 *
 * Runtime-only data for the Image Editor space (not written to .blend).
 */

#pragma once

#include "../sculpt_paint/mesh/paint_image_select_intern.hh"

namespace blender::ed::image {

/* Runtime data owned by SpaceImage, allocated on space create, freed on space free. */
struct SpaceImage_Runtime {
  /* Floating selection operation state (move / transform) for Image Paint mode.
   * Null when no selection operation is in progress for this editor instance. */
  blender::PaintSelectSession paint_select;
};

} /* namespace blender::ed::image */
