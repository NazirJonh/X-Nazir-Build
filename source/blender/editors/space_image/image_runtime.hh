/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 *
 * Runtime-only data for the Image Editor space (not written to .blend).
 */

#pragma once

#include <cstdint>

#include "BLI_math_vector_types.hh"

#include "DNA_vec_types.h"

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
 * the older one pointing at a step the newer one's #ED_image_undo_push_begin had already freed.
 * One slot makes that unrepresentable -- a tool taking over must end whatever is here first. The
 * gradient holds no step of its own but shares the slot for the same reason: only one floating
 * edit can own the editor at a time.
 *
 * Which tool the session belongs to is carried by #PaintSelectFloatingSession::tool, so the state
 * stays opaque here and this header (and therefore every SpaceImage user) keeps its independence
 * from the sculpt_paint module. */
struct PaintSelectSession {
  PaintSelectFloatingSession *active = nullptr;
};

namespace ed::image {

/* What a Combined preview may be narrowed to while the main region is drawing it.
 *
 * Both fields are the viewport's alone. Everything else that acquires the preview -- the
 * eyedropper, the scopes, image saving -- reads pixels this state says nothing about, so it must
 * see the cleared value and get the whole preview at full resolution. See
 * #SpaceImage_Runtime::combined_preview_draw for how that is enforced. */
struct CombinedPreviewDrawState {
  /* The part of the canvas about to be drawn, in canvas pixels. Empty when nothing is drawing. */
  rcti clip = {0, 0, 0, 0};
  /* Region pixels the canvas is drawn into, so the preview is not shaded finer than it can be
   * shown. Zero when nothing is drawing. */
  int2 display_size = int2(0);

  /* Whether a draw is in flight. Keyed on the display size rather than the clip, because a canvas
   * scrolled off screen legitimately arms a degenerate clip. */
  bool is_armed() const
  {
    return this->display_size.x > 0 && this->display_size.y > 0;
  }
};

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

  /* What the main region is drawing this instant, or nothing at all when no draw is in flight.
   *
   * Armed by #image_main_region_draw for the duration of #DRW_draw_view and cleared again the
   * moment that returns, because #ED_space_image_acquire_composite_buffer is reached both from
   * the image engine -- which only ever looks at the pixels named here -- and from the eyedropper,
   * the scopes and image saving, which look at all of them. Narrowing the Combined preview for the
   * second group would hand them stale pixels, so the arming window is what tells the two apart
   * and it must not outlive the draw. #ScopedCombinedPreviewDraw is what guarantees that. */
  CombinedPreviewDrawState combined_preview_draw;

  /* The canvas the Combined preview last resolved to, and the #Image it was resolved for.
   *
   * #ED_space_image_get_size is reached several times per redraw and its composite branch would
   * otherwise walk the node tree of a material on each one, so the answer is remembered here as
   * the preview is produced. A UID of zero means nothing is remembered, which is also how a
   * material that stops resolving clears it.
   *
   * At most one frame stale, which is what the cache it stands in for already was: the size query
   * happens before the preview is produced within a frame either way. */
  int2 combined_preview_canvas = int2(0);
  uint32_t combined_preview_canvas_image_uid = 0;
};

} /* namespace ed::image */

} /* namespace blender */
