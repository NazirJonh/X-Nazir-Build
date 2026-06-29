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

struct wmTimer;

namespace blender::ed::image {

/* Runtime data owned by SpaceImage, allocated on space create, freed on space free. */
struct SpaceImage_Runtime {
  /* Floating selection operation state (move / transform) for Image Paint mode.
   * Null when no selection operation is in progress for this editor instance. */
  blender::PaintSelectSession paint_select;

  /* Notifier timer driving the selection-mask "marching ants" animation, or null when inactive.
   * The wmTimer is owned by the window manager; this is only a reference. Created/removed by
   * #image_selection_mask_timer_update and torn down in #image_exit. */
  wmTimer *selection_mask_timer = nullptr;
};

} /* namespace blender::ed::image */
