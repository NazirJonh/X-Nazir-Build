/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Pixel-writer for the 2D Image Editor Curve Patch session. Replaces the dab-replay path
 * (`paint_2d_stroke()`) that `paint_image_curve_patch.cc` used before this module existed --
 * see `.MyTaskAndDoc/Paint-Curve-in-Sculpt-Mode/2026-08-20-image-curve-patch-rasterizer-spec.md`
 * for the full design and the reasoning behind every non-obvious choice below.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "BKE_curve_patch.hh"

struct ColorSpace;

namespace blender {

struct bContext;
struct Brush;
struct Paint;

namespace ed::sculpt_paint {

struct ImageCurvePatchSession;

/* -------------------------------------------------------------------- */
/** \name Public entry points
 * \{ */

/**
 * Rebuild `session.doc.active_item().geometry` (and its `control_curve` / `params`) from the
 * session's
 * canonical UV curve. Call after every edit to the curve and on every live-parameter change
 * (spec §6.4).
 */
void image_curve_patch_geometry_rebuild(bContext *C, ImageCurvePatchSession &session);

/**
 * Resolve the patch parameters for the current brush state: refresh the session-frozen radius
 * from the live Size slider, then overlay every brush-driven field on top of the frozen ones.
 *
 * Shared by #image_curve_patch_geometry_rebuild() and the live-brush watchdog in
 * `paint_image_curve_patch.hh`, which compares the result against the last stamped one. The two
 * MUST resolve identically -- a second copy of this expression would make the watchdog either
 * miss changes or re-stamp forever.
 */
bke::CurvePatchParams image_curve_patch_params_resolve(ImageCurvePatchSession &session,
                                                       const Paint &paint,
                                                       const Brush &brush);

/**
 * Rasterize `session.doc.active_item().geometry` into the canvas. Assumes the caller already
 * restored tiles
 * to the anchor state (spec §7.3) -- this function reads FROM the tiles' original pixels, it does
 * not restore them itself.
 */
void image_curve_patch_raster_draw(bContext *C, ImageCurvePatchSession &session);

/**
 * Commit-time draw: rebuild with `final_quality = true`, then draw. The caller is responsible for
 * restoring tiles to the anchor state before calling this and for closing the undo step after
 * (spec §10).
 */
void image_curve_patch_raster_draw_final(bContext *C, ImageCurvePatchSession &session);

/* \} */

/* -------------------------------------------------------------------- */
/** \name Pure helpers (exposed for unit testing -- spec §14.2, §14.3)
 * \{ */

/** UV -> pixel space of the reference tile (spec §4.1, §4.2). */
float2 image_curve_patch_uv_to_ref_px(const float2 &uv,
                                      const float2 &ref_tile_uv_origin,
                                      const int2 &ref_tile_resolution);

/** Inverse of #image_curve_patch_uv_to_ref_px. */
float2 image_curve_patch_ref_px_to_uv(const float2 &ref_px,
                                      const float2 &ref_tile_uv_origin,
                                      const int2 &ref_tile_resolution);

/**
 * Float-buffer `src` for `IMB_blend_color_float()`: premultiplied, scene-linear (spec §7.5).
 * `alpha` must already be `|sample.value| * sample.tex_color.a`, clamped to [0, 1].
 */
float4 image_curve_patch_blend_src_float(const float3 &brush_color_linear, float alpha);

/**
 * Byte-buffer `src` for `IMB_blend_color_byte()`: straight alpha, converted to
 * `byte_colorspace` (spec §7.5). `alpha` must already be `|sample.value| * sample.tex_color.a`,
 * clamped to [0, 1]. `byte_colorspace` may be null (no conversion).
 */
void image_curve_patch_blend_src_byte(const float3 &brush_color_linear,
                                      float alpha,
                                      const ColorSpace *byte_colorspace,
                                      uchar r_src[4]);

/* \} */

}  // namespace ed::sculpt_paint
}  // namespace blender
