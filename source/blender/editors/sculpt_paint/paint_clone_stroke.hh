/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Clone Stamp stroke runtime: target ownership and dab hooks.
 */

#pragma once

#include "paint_clone_source.hh"

/* NOTE: all Blender DNA/data types live in namespace blender. */
namespace blender {
struct Brush;
struct Depsgraph;
struct Main;
struct Material;
struct Object;
struct PaintModeSettings;
struct Sculpt;
struct bContext;
}  // namespace blender

namespace blender::ed::sculpt_paint::clone {

struct CloneStrokeRuntime {
  CloneStrokeTargets targets;
  CloneSourcePoint source;
  /** Brush whose falloff curve shades the dab. Not owned; valid for the stroke duration. */
  const Brush *brush = nullptr;
  /** Material the current targets were built from; rebuilt per dab when it changes. */
  const Material *material = nullptr;
  /**
   * Destination UV of this dab's main symmetry pass, for the mirrored passes that follow it.
   *
   * A mirrored pass paints the far side of the mesh, which is a different surface but not
   * necessarily different texels: a UV layout that mirrors its islands to reuse space maps both
   * halves onto the same patch of the map. Stamping there twice per dab would blend the source in
   * at roughly double the requested strength, so a mirrored dab that lands on top of the main one
   * is dropped -- see #clone_dab_is_symmetry_duplicate.
   */
  float2 main_pass_dest_uv = float2(0.0f);
  bool main_pass_dest_uv_valid = false;
  /**
   * Transform and destination UV Jacobian the main symmetry pass stamped with, for the mirrored
   * passes that follow it: their sampling map is this transform composed with the mirror Jacobian
   * (#clone_symmetry_transform_apply), which is what mirrors the stamped detail. Building from the
   * mirrored triangle instead would lose the mirror -- the mirrored location's own parametrization
   * maps UV to screen the same way the main one does on a symmetric mesh, so the stamp would carry
   * the source's direction into both halves unchanged.
   */
  CloneDabTransform main_pass_transform;
  float3 main_pass_dp_du = float3(0.0f);
  float3 main_pass_dp_dv = float3(0.0f);
  bool main_pass_transform_valid = false;
};

/**
 * Whether a mirrored dab would repaint the texels the main pass of the same dab just painted.
 *
 * True when the two footprints overlap at all: within one radius they cover the same detail, and
 * the second pass would only deepen it.
 */
bool clone_dab_is_symmetry_duplicate(const CloneStrokeRuntime &runtime,
                                     const float2 &dest_uv,
                                     float uv_radius);

/**
 * Get (or lazily create) the runtime for the in-progress stroke, owned by \a owner.
 *
 * \param owner: the caller's own storage slot for the stroke -- #StrokeCache::clone_runtime for a
 * sculpt stroke, #ImagePaintState::clone_runtime for a 2D one, #ProjStrokeHandle::clone_runtime
 * for projection paint. Passing the slot rather than keeping one global is what lets two strokes
 * exist at once, and what makes each owner responsible for its own release.
 *
 * Keyed on the material: when the active object's material differs from the one the targets were
 * built for, the target map is rebuilt so each layer/channel keeps its exact pairing. Undo is
 * owned by the outer stroke in every path, so the runtime never opens one of its own.
 *
 * Returns null (leaving \a owner untouched) when no writable target could be built.
 */
CloneStrokeRuntime *clone_stroke_runtime_ensure(CloneStrokeRuntime *&owner,
                                                Main *bmain,
                                                const CloneSourcePoint &source,
                                                const Material &ma,
                                                const Brush *brush,
                                                int visible_material_channels);

/** Release a stroke runtime and its ImBuf locks, clearing \a owner. No-op when already null. */
void clone_stroke_runtime_free(CloneStrokeRuntime *&owner);

/* -------------------------------------------------------------------- */
/** \name Projection-paint hooks (guarded no-op when no clone source is set).
 *
 * Each spaced dab stamps the full brush footprint (UV-space disk) into every visible
 * PBR channel/layer target. The legacy single-image clone projection still runs for the
 * active slot canvas; suppressing it entirely is the follow-up once stamp parity is verified.
 * True-clone offset is preserved: source_uv = source_center + (dest - dab_center).
 * \{ */

void clone_pbr_proj_dab(const bContext *C,
                        CloneStrokeRuntime *&owner,
                        const float mouse[2],
                        float pressure,
                        bool eraser);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt-stroke hooks (guarded no-op when no clone source is set).
 *
 * Each sculpt dab stamps the full brush footprint (UV-space disk) resolved from the stroke
 * cache dab location (object space) via an object-space BVH raycast and the hit triangle's
 * surface-to-UV scale. Mirror-symmetry passes stamp the mirror image of the main pass's stamp
 * (the main transform composed with the pass's UV Jacobian, #symmetry_uv_jacobian), and a
 * mirrored dab that lands on the main pass's texels is dropped.
 * Undo is owned by the outer sculpt stroke (image-undo when
 * sculpt_brush_uses_image_canvas(); CLONE is registered there). Targets are owned by the
 * stroke's #StrokeCache slot and released with it.
 * \{ */

/**
 * Apply one sculpt dab to all PBR targets. No-op when: not a mesh, dyntopo,
 * inverted stroke, non-image canvas, no source, raycast miss, or no targets.
 */
void sculpt_clone_dab_apply(const Depsgraph &depsgraph,
                            const Sculpt &sd,
                            Object &ob,
                            const Brush &brush,
                            PaintModeSettings &settings,
                            CloneStrokeRuntime *&owner);

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
