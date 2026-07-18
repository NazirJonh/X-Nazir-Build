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

struct PaintSelectFloatingSession;

/* Floating selection operation state for Image Paint mode.
 *
 * There is exactly one slot. The move / transform / warp tools lift pixels off the canvas and hold
 * an image undo step open while they float; two sessions being live at once is what used to leave
 * the older one pointing at a step the newer one's #ED_image_undo_push_begin had already freed. One
 * slot makes that unrepresentable -- a tool taking over must end whatever is here first. The
 * gradient holds no step of its own but shares the slot for the same reason: only one floating edit
 * can own the editor at a time.
 *
 * Which tool the session belongs to is carried by #PaintSelectFloatingSession::tool, so the state
 * stays opaque here and this header (and therefore every SpaceImage user) keeps its independence
 * from the sculpt_paint module. */
struct PaintSelectSession {
  PaintSelectFloatingSession *active = nullptr;
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
