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
 * Gather every channel of \a ma, shade it, and return the cached preview.
 *
 * Synchronous and non-blocking: a region refresh is sub-millisecond and a full rebuild is tens of
 * milliseconds. Never renders. When a channel needs a bake that has not run, the preview is
 * produced without it -- see #combined_preview_bake_ensure, which the caller invokes separately.
 *
 * The returned buffer is owned by the BKE cache; do not free it.
 *
 * \return null when nothing resolves, which is the caller's cue to fall back to the plain image.
 */
ImBuf *combined_preview_ensure(Main &bmain,
                               const Material &ma,
                               const CombinedPreviewLighting &lighting,
                               uint64_t *r_revision = nullptr,
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
