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
#include <memory>

#include "BKE_paint_material_combined.hh"

#include "BLI_math_matrix_types.hh"

namespace blender {
struct ImBuf;
struct Main;
struct Material;
struct PaintMaterialCompositeSurface;
struct bContext;
namespace gpu {
class Texture;
}
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
 * \param surface: the active object's UV surface for exact Object/Generated Gradient rows, or
 *                 null. Its key is part of the stack hash, so an exact and an approximate preview
 *                 never share a cache entry. The GPU path does not read it (see
 *                 #combined_preview_gpu_update).
 * \return null when nothing resolves, which is the caller's cue to fall back to the plain image.
 */
ImBuf *combined_preview_ensure(Main &bmain,
                               const Material &ma,
                               const CombinedPreviewLighting &lighting,
                               const CombinedPreviewRequest &request = {},
                               uint64_t *r_revision = nullptr,
                               rcti *r_changed_region = nullptr,
                               CombinedEvalStats *r_stats = nullptr,
                               std::shared_ptr<const PaintMaterialCompositeSurface> surface =
                                   nullptr);

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

/**
 * Whether the Image Editor preview of \a ma is to be shaded on the GPU: the material's
 * #Material::paint_layers_composite_device (the GPU unless set otherwise). The environment variable
 * `BLENDER_PBR_GPU_COMBINED=0` is kept as a developer override that forces the CPU for every
 * material, to compare against the GPU without touching files. Refusals and activation are reported
 * through the perf log (`BLENDER_PBR_PERF`).
 */
bool combined_preview_gpu_enabled(const Material *ma);

/** Free the compute shaders of the GPU path. Needs the GPU context current; called at draw exit. */
void combined_preview_gpu_free();

/**
 * Shade the Combined preview of the layered material \a ma straight into \a output on
 * the GPU, one compute dispatch per row of every channel stack plus one for the lighting.
 *
 * \param pass: the Combined preview (#PAINT_LAYER_PASS_COMBINED) or the channel pass to show.
 * \param output: a display texture created with `GPU_TEXTURE_USAGE_SHADER_WRITE`; it is filled at
 *                its own resolution.
 * \param texel_to_uv: maps an \a output texel (plus the half-texel centre) to the canvas UV in
 *                     [0, 1]; a texel that lands outside the canvas is written transparent.
 * \return false, with \a output untouched or partly written, when the material or one of its rows
 *         is something the GPU path does not lay (an adjustment inside a folder, folders nested
 *         deeper than the accumulators it keeps), when a layer map has no GPU texture, or when
 *         there is no GPU context. The caller then falls back to the CPU preview.
 */
bool combined_preview_gpu_update(const Material &ma,
                                 const CombinedPreviewLighting &lighting,
                                 int pass,
                                 gpu::Texture *output,
                                 const float3x3 &texel_to_uv);

/**
 * Whether the GPU path lays \a pass: the Combined preview, or the channel pass of one of
 * the channels it shades. The Mask pass is not one.
 */
bool combined_preview_gpu_pass_supported(int pass);

/**
 * Compare \a output, just shaded by #combined_preview_gpu_update, with the CPU preview
 * of the same material and print the difference through the perf log. Off unless the environment
 * variable `BLENDER_PBR_GPU_VERIFY` is 1. It runs the CPU shading only every 120th call: it is a
 * check for a developer, not a mode.
 */
void combined_preview_gpu_verify(Main &bmain,
                                 const Material &ma,
                                 const CombinedPreviewLighting &lighting,
                                 gpu::Texture *output,
                                 const float3x3 &texel_to_uv);

}  // namespace blender::ed::material_combined
