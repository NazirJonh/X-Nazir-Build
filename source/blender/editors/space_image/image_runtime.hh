/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 *
 * Runtime-only data for the Image Editor space (not written to .blend).
 */

#pragma once

#include <cstdint>

struct Image;
struct wmTimer;

namespace blender {

namespace gpu {
class Batch;
}

struct ImageSelectMoveState;
struct ImageSelectTransformState;
struct ImageSelectGradientState;
struct ImageSelectWarpState;

/* Floating selection operation state for Image Paint mode. The individual states are opaque
 * here: they are owned and manipulated by the sculpt_paint module, which is what keeps this
 * header (and therefore every SpaceImage user) free of a dependency on that module. */
struct PaintSelectSession {
  ImageSelectMoveState *move = nullptr;
  ImageSelectTransformState *transform = nullptr;
  ImageSelectGradientState *gradient = nullptr;
  ImageSelectWarpState *warp = nullptr;
};

namespace ed::image {

/* Runtime data owned by SpaceImage, allocated on space create, freed on space free. */
struct SpaceImage_Runtime {
  /* Floating selection operation state (move / transform) for Image Paint mode.
   * All members are null when no selection operation is in progress for this editor instance. */
  PaintSelectSession paint_select;

  /* Notifier timer driving the selection-mask "marching ants" animation, or null when inactive.
   * The wmTimer is owned by the window manager; this is only a reference. Created/removed by
   * #image_selection_mask_timer_update and torn down in #image_exit. */
  wmTimer *selection_mask_timer = nullptr;

  /* Cached outline of the paint selection mask, in UV space, as a GPU_PRIM_LINES batch covering
   * every UDIM tile of #selection_outline_image. Extracting the outline is O(width * height), so
   * it is rebuilt only when the mask actually changes; the marching-ants animation is driven by a
   * shader uniform instead. Owned here and discarded in #image_free. */
  gpu::Batch *selection_outline_batch = nullptr;
  /* Image the cached batch was built from. Compared by address only, never dereferenced. */
  const Image *selection_outline_image = nullptr;
  /* Value of #BKE_image_paint_selection_mask_revision_get when the batch was built. */
  uint64_t selection_outline_revision = 0;
};

} /* namespace ed::image */

} /* namespace blender */
