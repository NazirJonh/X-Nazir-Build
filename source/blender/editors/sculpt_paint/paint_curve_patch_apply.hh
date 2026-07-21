/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Applying a Curve Patch in one shot, without the modal editor and without a stroke: the path the
 * Python API is built on.
 *
 * Everything the interactive path measures from user input is passed in instead. That is the whole
 * difference between the two -- the session, the sampler, the effects and the undo step are shared
 * verbatim with `SCULPT_OT_curve_patch_edit`, because the Stage 0 measurement found no missing
 * architecture here, only missing values.
 */

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "paint_curve_patch_effect.hh"

namespace blender {
struct Brush;
struct Paint;
struct ReportList;
struct Sculpt;
struct wmOperatorType;
}  // namespace blender

namespace blender::bke {
class CurvesGeometry;
struct CurvePatchParams;
}  // namespace blender::bke

namespace blender::ed::sculpt_paint {

/**
 * Make a brush safe to evaluate outside a stroke.
 *
 * Two things a live `PaintStroke` does that nothing else does:
 * - It builds the brush's `CurveMapping` tables. They are lazy, and only interactive paths
 *   (`paint_cursor.cc`, `wm_operators.cc`, stroke init) ever call `BKE_curvemapping_init()`.
 *   Reading them unbuilt asserts in debug and reads through null in release.
 * - It sets `PaintRuntime::overlap_factor`, which `brush_strength()` multiplies by. Left at its
 *   default 0.0f it produces a zero-amplitude patch with no diagnostic of any kind, in every build
 *   configuration -- which is why this is a separate, named step rather than two lines buried in
 *   #curve_patch_apply.
 */
void curve_patch_prepare_brush_for_headless(Paint &paint, Brush &brush);

/**
 * The stroke measurements a headless apply has to supply, because no stroke took them.
 *
 * Deliberately does NOT carry the patch's radius or its projection plane: both live in
 * `bke::CurvePatchParams` (`radius`, `plane_normal`), which #curve_patch_apply takes as well, and
 * requiring them twice would create two sources of truth for the same geometry. Everything else a
 * `StrokeCache` holds either has a usable default or is derived inside -- `scale` from the object
 * matrix, `overlap_factor` from the brush's spacing, `location`/`radius` from the curve's own
 * bounding sphere once the re-stamp starts.
 */
struct CurvePatchApplyInput {
  /** Object-space center of the patch. Only the tiling origin and the average-stroke-position
   * bookkeeping read it: the re-stamp overwrites `StrokeCache::location` with the control curve's
   * own encompassing sphere before any element is sampled. */
  float3 location = float3(0.0f);
  /** Object-space view direction, read only by the node query of a TUBE-falloff brush. */
  float3 view_normal = float3(0.0f, 0.0f, 1.0f);
  /** No tablet in a script: the strength multiplier a stroke would have read from pressure. */
  float pressure = 1.0f;
};

/**
 * Apply a Curve Patch to an object, without a modal editor and without a stroke.
 *
 * Does NOT open an undo transaction of its own: the caller's operator is responsible for that
 * through `OPTYPE_UNDO`. `ReliefEffect::push_position_step()` parks its step for exactly that
 * caller (see `paint_curve_patch_effect_relief.cc`), and a transaction opened here would collide
 * with it.
 *
 * Each of `control_curves` is copied, in the active object's space, and must already carry its
 * bezier handle positions -- build them with #curve_patch_control_curve_from_points when starting
 * from raw points. `params` runs parallel to it, one entry per curve, because a patch freezes the
 * brush values its own curve was drawn with. Several curves are stamped one after another within
 * each symmetry pass and blend through the same accumulator the passes themselves use, so
 * overlapping patches average rather than stack.
 * The brush the patch is stamped with is the active one of `sd.paint`, because that is the one the
 * symmetry machinery (`do_symmetrical_brush_actions()`) reads regardless of what a caller passes.
 *
 * Returns false, with the reason in `reports` when that is non-null, whenever the object cannot
 * take a patch: no sculpt session, no Paint BVH, Dynamic Topology, a stroke already in flight, a
 * degenerate curve, or a brush the chosen effect cannot be built for.
 */
bool curve_patch_apply(const Scene &scene,
                       const Depsgraph &depsgraph,
                       Object &ob,
                       Sculpt &sd,
                       PaintModeSettings &paint_mode_settings,
                       Span<bke::CurvesGeometry> control_curves,
                       Span<bke::CurvePatchParams> params,
                       CurvePatchEffectType effect_type,
                       const CurvePatchApplyInput &input,
                       ReportList *reports);

/** Defined in `paint_curve_patch_apply.cc`. The thin operator wrapper around #curve_patch_apply:
 * resolves the control curve from a `PaintCurve`, freezes the brush's parameters and fills
 * #CurvePatchApplyInput with defaults derived from the curve itself. */
void SCULPT_OT_curve_patch_apply(wmOperatorType *ot);

}  // namespace blender::ed::sculpt_paint
