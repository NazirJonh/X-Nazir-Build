/* SPDX-FileCopyrightText: 2024 Blender Authors
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

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

struct ImBuf;
struct Image;
struct ImageUser;
struct bContext;

namespace blender {

struct SelectionTileFragment;

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

/** Build sharp-interior GPU preview: straight RGB from \a src, binary alpha from \a binary_mask. */
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
 * Alpha-blend a prepared fragment-sized buffer into a same-sized destination canvas.
 * The blend mask is expected to be a 4-channel float buffer; only channel 0 is sampled.
 */
void image_select_blend_buffer_into_canvas(ImBuf *dst_canvas,
                                           const ImBuf *src_fragment,
                                           const ImBuf *blend_mask);

/**
 * Close an image undo step only if it is still active.
 * Floating selection commits may outlive the undo step opened during lift_source.
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
