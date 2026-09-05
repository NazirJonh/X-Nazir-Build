/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * Turns a #Material into the inputs the Combined preview shades, and hands them to the cache.
 *
 * The evaluator and its cache live in `BKE_paint_material_combined.hh`; only the *gathering* is
 * here. That split is not cosmetic: resolving a channel may reach the source-material bake, which
 * is an editor-side EEVEE job, and BKE must not depend on it. Everything BKE needs -- pixels,
 * constants, dimensions, a structural hash -- crosses the boundary as plain data.
 */

#include <cstdint>

#include "BKE_paint_material_combined.hh"

namespace blender {
struct ImBuf;
struct Main;
struct Material;
struct bContext;
}  // namespace blender

namespace blender::ed::material_combined {

/**
 * What the viewport knows about the preview it is about to draw, and nobody else may claim.
 *
 * Default-constructed means "the whole preview at full resolution", which is what every caller
 * that reads pixels rather than displaying them -- the eyedropper, the scopes, a save -- must
 * pass. The defaults are the safe answer precisely so that a caller who does not know about this
 * struct cannot narrow anything by accident.
 */
struct CombinedPreviewRequest {
  /**
   * The part of the preview the caller is about to read, in **canvas** coordinates. Empty means
   * all of it. See #CombinedCacheRequest.clip for what a caller gives up by narrowing it.
   */
  rcti clip = {0, 0, 0, 0};
  /**
   * The most the caller can display, in pixels. The preview is shaded at the largest power-of-two
   * fraction of the canvas that still covers this, so a canvas fitted into a small editor is not
   * shaded at canvas resolution only to be thrown away by the downscale. Zero means no cap.
   */
  int2 max_output = int2(0);
};

/**
 * Gather every channel of \a ma, shade it, and return the cached preview.
 *
 * Synchronous and non-blocking: a region refresh is sub-millisecond and a full rebuild is tens of
 * milliseconds. Never renders. When a channel needs a bake that has not run, the preview is
 * produced without it -- see #combined_preview_bake_ensure, which the caller invokes separately.
 *
 * The returned buffer is owned by the BKE cache; do not free it.
 *
 * \param r_changed_region: the part of the preview this call re-shaded, in the **returned
 *                          buffer's** texel coordinates, or empty when nothing moved. Not canvas
 *                          coordinates: the buffer covers the whole canvas whatever resolution it
 *                          was shaded at, so a consumer turns this into UV by dividing by the
 *                          dimensions of the buffer it is reading, and canvas texels would be
 *                          wrong by exactly that factor.
 * \return null when nothing resolves, which is the caller's cue to fall back to the plain image.
 */
ImBuf *combined_preview_ensure(Main &bmain,
                               const Material &ma,
                               const CombinedPreviewLighting &lighting,
                               const CombinedPreviewRequest &request = {},
                               uint64_t *r_revision = nullptr,
                               rcti *r_changed_region = nullptr,
                               CombinedEvalStats *r_stats = nullptr);

/**
 * Start an async bake of \a ma if any channel of the Combined preview would need one.
 *
 * Cheap and idempotent, and it exists because nothing else would ever start it: today
 * #material_source_bake_ensure is reached only from the paint cursor and RNA updates, so a user
 * who merely opens the Combined preview on a procedural material would wait for a bake that was
 * never requested and see the channel's default forever.
 *
 * Called from the Image Editor's redraw path, next to the acquire. Never blocks.
 */
void combined_preview_bake_ensure(const bContext &C, Material &ma);

}  // namespace blender::ed::material_combined
