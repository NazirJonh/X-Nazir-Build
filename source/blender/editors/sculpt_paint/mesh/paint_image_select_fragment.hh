/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared pixel/compositing helpers for floating selection fragments (move + transform).
 * BKE owns mask storage and weight functions; this module owns fragment-local buffers
 * and lift/restore/preview rebuild used by editor operators.
 */

#pragma once

#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

struct ImBuf;
struct Image;
struct ImageUser;
struct ImageUndoStep;
struct bContext;

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Fragment types
 *
 * A fragment is one tile's worth of lifted selection: the extracted pixels, the mask that
 * defines its shape, and the derived buffers used to preview it while it floats. These live
 * here rather than in #paint_image_select_intern.hh because #Vector<SelectionTileFragment>
 * appears in this header's own API, and #Vector needs the complete type to size its inline
 * buffer.
 * \{ */

struct SelectionTileFragmentPixelData {
  ImBuf *fragment_ibuf = nullptr;
  ImBuf *fragment_mask_ibuf = nullptr;
};

struct SelectionTileFragmentPreviewBuffers {
  ImBuf *fragment_blend_mask_ibuf = nullptr;
  ImBuf *fragment_display_ibuf = nullptr;
  ImBuf *fragment_feather_display_ibuf = nullptr;
};

struct SelectionTileFragmentGeometry {
  int2 origin_px = {0, 0};
  int2 size_px = {0, 0};
  int2 selection_origin_px = {0, 0};
  int2 selection_size_px = {0, 0};
  int2 tile_size_px = {1, 1};
  int tile_number = 1001;
};

struct SelectionTileFragment {
  SelectionTileFragmentPixelData pixels;
  SelectionTileFragmentPreviewBuffers preview;
  SelectionTileFragmentGeometry geom;
  PaintSelectionEdgePolicy edge_policy = BKE_image_paint_selection_edge_policy_feathered();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image undo session
 *
 * Single point of contact with #UndoStack::step_init for the floating-selection tools
 * (move / transform / warp / gradient / paste). Nothing else in this module family should
 * touch undo internals directly.
 *
 * ARCHITECTURAL NOTE: undo steps are operator-scoped -- the window manager opens one when an
 * operator starts and finalizes it when the operator finishes. Floating selection state,
 * however, deliberately outlives its operator: a fragment lifted by `select_move` stays live
 * across subsequent operators until the user confirms or cancels. The step opened at lift time
 * can therefore be finalized (or replaced) by an unrelated operator before the commit runs, at
 * which point #image_select_undo_session_is_open reports false and the commit silently skips
 * closing a step it no longer owns.
 *
 * That is a known desync, not a fix: the pixel edits made after the step was lost end up
 * outside any undo record. The correct solution is to stop letting floating state outlive an
 * operator (e.g. keep the modal operator running until confirm/cancel, or record the edits into
 * a step owned by the confirm operator itself). Until then, all of the fragile knowledge is
 * concentrated in these functions so the fix lands in one place.
 * \{ */

/** True when an image-type undo step is currently open on the global undo stack. */
bool image_select_undo_session_is_open();

/**
 * The currently open image undo step, or null when no image step is open.
 * Used to register additional tiles into the step opened by the caller.
 */
ImageUndoStep *image_select_undo_session_step_get();

/** Close the open image undo step. No-op when the step was already finalized (see note above). */
void image_select_undo_session_end();

/** \} */

/** Free preview buffers and pixel data owned by \a frag. */
void image_select_fragment_free(SelectionTileFragment &frag);

/**
 * Rebuild derived preview buffers from #SelectionTileFragment::pixels.fragment_mask_ibuf.
 * Frees and nulls prior preview buffers first.
 *
 * Contract:
 * - #fragment_blend_mask_ibuf: 1ch float, from BKE compute_blend_mask + edge_policy.
 * - #fragment_display_ibuf: 4ch straight RGB, binary alpha (GPU pass 1, nearest, ALPHA).
 * - #fragment_feather_display_ibuf: 4ch premultiplied RGB, alpha only outside binary mask
 *   (GPU pass 2, linear, ALPHA_PREMULT); omitted when edge_policy is hard.
 */
void image_select_fragment_update_preview_buffers(SelectionTileFragment &frag);

/**
 * Build sharp-interior GPU preview: straight RGB from \a src, binary alpha from \a binary_mask.
 */
ImBuf *image_select_make_display_ibuf_binary(const ImBuf *src, const ImBuf *binary_mask);

/** Build outward-feather GPU preview: premultiplied RGB, alpha only outside \a binary_mask. */
ImBuf *image_select_make_display_ibuf_feather(const ImBuf *src,
                                              const ImBuf *binary_mask,
                                              const ImBuf *blend_mask);

/**
 * Bilinear sample of a 1-channel float mask at fragment-local coordinates.
 * Fragment masks live outside BKE tile maps, so this stays in the editor layer.
 */
float image_select_sample_mask_bilinear(const ImBuf *mask, float fx, float fy);

/**
 * Alpha-blend a prepared fragment buffer into a destination canvas, placing the fragment's
 * top-left corner at \a origin in canvas pixel coordinates. The fragment and the blend mask must
 * agree on size; the region is clipped to the canvas. The blend mask is expected to be a
 * 4-channel float buffer; only channel 0 is sampled.
 */
void image_select_blend_buffer_into_canvas_at(ImBuf *dst_canvas,
                                              const ImBuf *src_fragment,
                                              const ImBuf *blend_mask,
                                              const int origin[2]);

/**
 * Close the image undo step opened for a floating fragment, if it is still open, and clear
 * \a r_undo_begun. Thin wrapper over #image_select_undo_session_end.
 */
void image_select_fragment_undo_push_end_if_open(bool &r_undo_begun);

/**
 * Clear source canvas pixels and runtime mask for each fragment (lift).
 * Opens an image undo step on first tile; caller must push_end on commit/cancel.
 */
void image_select_fragment_lift_source(bContext *C,
                                       Image *ima,
                                       const ImageUser &base_iuser,
                                       const Vector<SelectionTileFragment> &fragments,
                                       const char *undo_label,
                                       bool &r_undo_begun);

/**
 * Restore source pixels and mask from fragments (cancel).
 * Closes undo when \a undo_begun; clears the active undo step afterward.
 */
void image_select_fragment_restore_source(bContext *C,
                                          Image *ima,
                                          const ImageUser &base_iuser,
                                          const Vector<SelectionTileFragment> &fragments,
                                          bool undo_begun);

} /* namespace blender */
