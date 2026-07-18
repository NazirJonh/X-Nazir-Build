/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Runtime, per-UDIM-tile selection masks for 2D image paint.
 *
 * A selection mask is a single-channel float #ImBuf with the same dimensions as the tile it
 * belongs to. Masks are stored on #bke::ImageRuntime and are never written to .blend files.
 * A cached "blend mask" is derived from each binary mask on demand; it holds the compositing
 * weights described by the image's #PaintSelectionEdgePolicy and is invalidated whenever the
 * binary mask or the policy changes.
 */

#pragma once

#include "BKE_image.hh"

namespace blender {

struct ImBuf;
struct Image;
struct ImageUser;

/** Edge policy with binary (non-feathered) edges. */
inline PaintSelectionEdgePolicy BKE_image_paint_selection_edge_policy_hard()
{
  PaintSelectionEdgePolicy policy;
  policy.use_outward_feather = false;
  policy.blend_radius_px = 0;
  return policy;
}

/** Edge policy with the default outward feather. */
inline PaintSelectionEdgePolicy BKE_image_paint_selection_edge_policy_feathered()
{
  return PaintSelectionEdgePolicy{};
}

/* -------------------------------------------------------------------- */
/** \name Mask Storage
 * \{ */

/**
 * Return the selection mask of the given tile, allocating a zeroed one when missing. An existing
 * mask of a different size is discarded and replaced.
 *
 * \note Like the mutable #BKE_image_paint_selection_mask_lookup overload, this conservatively bumps
 * the mask revision: every caller of this function writes into the buffer it returns.
 *
 * \return The mask owned by the image runtime; never null. Do not free it.
 */
ImBuf *BKE_image_paint_selection_mask_get(Image *image, int tile_number, int width, int height);

/**
 * Return the existing selection mask of the given tile, or null when the tile has none.
 *
 * \note The mutable overload conservatively bumps the mask revision, because callers routinely
 * write straight into the returned buffer instead of going through the edit functions below.
 * Read-only consumers (drawing, sampling) must use the `const Image *` overload so they do not
 * invalidate their own caches.
 *
 * \return The mask owned by the image runtime. Do not free it.
 */
ImBuf *BKE_image_paint_selection_mask_lookup(Image *image, int tile_number);
const ImBuf *BKE_image_paint_selection_mask_lookup(const Image *image, int tile_number);

/**
 * \return A counter that changes whenever any tile's selection mask may have been modified.
 *
 * The value only ever increases and is never reset, so comparing a stored copy against the
 * current value is enough to detect a change. It is conservative: it can advance without the
 * mask contents actually differing (for example when a writable mask pointer was handed out but
 * left untouched), but it never stays put across a real edit.
 */
uint64_t BKE_image_paint_selection_mask_revision_get(const Image *image);

/** Free all selection masks and reset the edge policy to the feathered default. */
void BKE_image_paint_selection_mask_free(Image *image);

/** Free the selection mask of a single tile. */
void BKE_image_paint_selection_mask_tile_free(Image *image, int tile_number);

/** \return True when at least one tile holds a selection mask. */
bool BKE_image_paint_selection_mask_has_any(const Image *image);

/** \return First UDIM tile number that has a non-empty selection, or 0 if none. */
int BKE_image_paint_selection_mask_first_tile_with_selection(const Image *image);

/**
 * Copy the selection mask of the given tile.
 *
 * \return A newly allocated #ImBuf the caller must free with #IMB_freeImBuf, or null when the
 * tile has no mask.
 */
ImBuf *BKE_image_paint_selection_mask_dup_tile(Image *image, int tile_number);

/**
 * Restore the selection mask of a tile from a previously duplicated mask. Passing null for
 * \a src_mask removes the tile's mask.
 */
void BKE_image_paint_selection_mask_restore_tile(Image *image,
                                                 int tile_number,
                                                 const ImBuf *src_mask);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask Queries and Edits
 * \{ */

/**
 * Compute the bounding box of the selected pixels of a tile.
 *
 * \param r_min: Receives the inclusive lower corner [x, y].
 * \param r_max: Receives the exclusive upper corner [x, y].
 * \return False when the tile has no mask or the selection is empty.
 */
bool BKE_image_paint_selection_mask_bounds(const Image *image,
                                           int tile_number,
                                           int r_min[2],
                                           int r_max[2]);

/** \return The raw binary mask value, or 0 when out of bounds or the tile has no mask. */
float BKE_image_paint_selection_mask_sample(const Image *image, int tile_number, int x, int y);

/** Set every pixel of the tile's mask to \a value. Does nothing when the tile has no mask. */
void BKE_image_paint_selection_mask_fill(Image *image, int tile_number, float value);

/** Invert the tile's mask in place. Does nothing when the tile has no mask. */
void BKE_image_paint_selection_mask_invert(Image *image, int tile_number);

/**
 * Replace the region of the tile's mask starting at \a origin with \a fragment_mask. Pixels
 * outside the tile are skipped. Existing values in the region are overwritten, not combined.
 */
void BKE_image_paint_selection_mask_replace(Image *image,
                                            int tile_number,
                                            const ImBuf *fragment_mask,
                                            const int origin[2]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Policy
 * \{ */

/**
 * \return The image's compositing edge policy. Falls back to a static feathered default when
 * \a image has no runtime data; the reference stays valid for the lifetime of the image.
 */
const PaintSelectionEdgePolicy &BKE_image_paint_selection_edge_policy_get(const Image *image);

/** Set the compositing edge policy, invalidating the cached blend masks when it changed. */
void BKE_image_paint_selection_edge_policy_set(Image *image,
                                               const PaintSelectionEdgePolicy &edge_policy);

/** Convenience: true when #PaintSelectionEdgePolicy::use_outward_feather is set. */
bool BKE_image_paint_selection_use_feather(const Image *image);

/** Convenience: sets hard or default feathered edge policy. */
void BKE_image_paint_selection_use_feather_set(Image *image, bool use_feather);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend Masks
 * \{ */

/** Paint/compositing weight: smooth falloff centered on the mask boundary. */
float BKE_image_paint_selection_blend_sample(const Image *image, int tile_number, int x, int y);

/** Bilinearly filtered variant of #BKE_image_paint_selection_blend_sample. */
float BKE_image_paint_selection_blend_sample_bilinear(const Image *image,
                                                      int tile_number,
                                                      float fx,
                                                      float fy);

/**
 * Derive the compositing weights of a binary mask.
 *
 * \return A newly allocated single-channel float #ImBuf the caller must free with
 * #IMB_freeImBuf.
 */
ImBuf *BKE_image_paint_selection_compute_blend_mask(const ImBuf *binary_mask,
                                                    const PaintSelectionEdgePolicy &edge_policy);

/**
 * Variant of #BKE_image_paint_selection_compute_blend_mask writing into a caller-owned 4-channel
 * buffer; only the first channel of each pixel is written.
 *
 * \param binary_stride_channels: Channel count of \a binary, used as the sampling stride.
 */
void BKE_image_paint_selection_compute_blend_mask_to_4ch(
    const float *binary,
    int width,
    int height,
    float *blend_4ch,
    int binary_stride_channels,
    const PaintSelectionEdgePolicy &edge_policy);

/**
 * Grow a selection bounding box so it also covers the outward feather, clamped to the tile.
 * Does nothing for a non-feathered policy.
 */
void BKE_image_paint_selection_bounds_expand_for_blend(
    int r_min[2],
    int r_max[2],
    int tile_width,
    int tile_height,
    const PaintSelectionEdgePolicy &edge_policy);

/** Drop the cached blend masks of every tile; they are rebuilt on the next sample. */
void BKE_image_paint_selection_blend_mask_invalidate(Image *image);

/**
 * Drop the cached blend mask of a single tile.
 *
 * \note Neither this nor #BKE_image_paint_selection_blend_mask_invalidate advances the mask
 * revision: the blend mask is derived data and the binary mask is left untouched.
 */
void BKE_image_paint_selection_blend_mask_invalidate_tile(Image *image, int tile_number);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pixel Transfer
 * \{ */

/**
 * Extract the pixel data from the bounding box of the active paint selection on the given
 * UDIM tile. Returns a newly-allocated #ImBuf on success; caller must free it via
 * #IMB_freeImBuf(). Preserves the source image's color-space on the returned buffer.
 *
 * \param tile_number: UDIM tile number (1001 for a non-tiled image).
 * \param iuser: Optional image user; its `tile` field is overwritten by this function.
 *   Pass null to use a default image user.
 * \param r_origin: Receives the tile-local pixel coordinates of the bounding box origin [x, y].
 * \param r_size: Receives the dimensions of the returned buffer [width, height].
 * \param r_mask_out: If non-null, receives a newly-allocated 1-channel float #ImBuf containing
 *   the mask fragment (same dimensions as the returned buffer). Caller must free it.
 *   Pass null to skip mask extraction.
 * \return nullptr if there is no selection, the tile has no pixel data, or the bbox is empty.
 */
ImBuf *BKE_image_paint_selection_extract_pixels(Image *image,
                                                int tile_number,
                                                ImageUser *iuser,
                                                int r_origin[2],
                                                int r_size[2],
                                                ImBuf **r_mask_out);

/**
 * Write pixel data into the given UDIM tile at the pixel-coordinate region (x, y, width, height).
 * Marks the image dirty and triggers a partial viewport update. Does NOT push an undo step --
 * follow the same convention as #Image.pixels in the Python API.
 *
 * \param pixels: Flat float array, size = `width * height * channels`.
 * \param channels: Number of channels per pixel. Must match the image buffer channel count.
 * \param x, y: Top-left corner of the destination region in tile-local pixel coordinates.
 * \param width, height: Dimensions of the region to write.
 * \param mask: Optional 1-channel float mask (same `width x height`). When non-null, blending
 *   is lerp: `dst = dst_original * (1 - mask[i]) + pixels[i] * mask[i]`. Pass null for a
 *   hard write of all pixels.
 * \return false if the image has no pixel data, region is out of bounds, or channels mismatch.
 */
bool BKE_image_paint_selection_write_region(Image *image,
                                            int tile_number,
                                            ImageUser *iuser,
                                            const float *pixels,
                                            int channels,
                                            int x,
                                            int y,
                                            int width,
                                            int height,
                                            const float *mask);

/** \} */

}  // namespace blender
