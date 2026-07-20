/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * What a Curve Patch session writes, and how it takes it back.
 *
 * The geometry half of the re-stamp lives in `paint_curve_patch_sampler.hh` and is shared. An
 * effect owns both application phases rather than merely consuming samples, because phase 1 has
 * to capture the pre-patch value of the TARGET (a position for relief, a color for paint), whose
 * type differs per effect -- see the derived contract in the Stage 2 plan.
 */

#pragma once

#include <cstdint>
#include <memory>

namespace blender {
struct Brush;
struct Depsgraph;
struct Object;
struct PaintModeSettings;
struct bContext;
}  // namespace blender

namespace blender::ed::sculpt_paint {

struct CurvePatchCache;

class CurvePatchEffect {
 public:
  virtual ~CurvePatchEffect() = default;

  /**
   * Open whatever undo transaction this effect needs for the whole session. No-op by default;
   * only a target with its own undo system (the image canvas) overrides it.
   *
   * The timing is load-bearing and is why this is a separate hook rather than constructor work.
   * The stroke that spawns a session leaves its own transaction open (`stroke_undo_begin()`,
   * `mesh/sculpt.cc:5696-5712`), and the handoff discards that transaction with
   * `BKE_undosys_step_push_init_abort()` only AFTER the session has been published and its initial
   * preview stamped (`mesh/sculpt.cc:6130-6140`, and the roll bridge at `:6172-6183`). A
   * transaction opened any earlier -- in the constructor, or lazily on the first `apply_pass()`,
   * which still runs inside that handoff -- is therefore destroyed by that abort before the modal
   * ever starts. Callers must invoke this immediately after the abort; do NOT move the work back
   * into the constructor.
   */
  virtual void session_undo_begin() {}

  /** Count the snapshot keys index into, compared against `CurvePatchCache::element_num` every
   * restamp to detect a mesh that changed underneath the session. */
  virtual int64_t element_num(Object &ob) const = 0;

  /**
   * Write the snapshot back. Runs FIRST in every restamp, before the spline rebuild, and
   * consumes the previous restamp's `CurvePatchCache::last_restamp_nodes`.
   */
  virtual void restore(Object &ob, const CurvePatchCache &patch) = 0;

  /**
   * Once per restamp, after the ribbon is built and before the first symmetry pass: refresh
   * whatever input the sampler and the write step will read. Relief recomputes vertex normals
   * here; an effect that neither reads nor invalidates them does nothing.
   */
  virtual void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchCache &patch) = 0;

  /**
   * Both phases for one symmetry pass: gather in parallel, then apply serially. Called by
   * `do_symmetrical_brush_actions()` once per enabled pass; the serial half is the sole writer of
   * `CurvePatchCache::pass_weight_accum` and of the target data.
   */
  virtual void apply_pass(const Depsgraph &depsgraph,
                          Object &ob,
                          const Brush &brush,
                          CurvePatchCache &patch) = 0;

  /** After all passes: final-quality smoothing and the viewport flush this target needs. */
  virtual void end_restamp(bContext &C, Object &ob, CurvePatchCache &patch) = 0;

  /** Fold the session into the real undo stack. */
  virtual void commit(bContext &C, Object &ob, const CurvePatchCache &patch) = 0;

  /** Elements the snapshot currently holds. Exists only for the `CURVE_PATCH_PROFILING` line in
   * `curve_patch_restore_and_restamp()`, which reported it before the snapshot moved off the
   * cache; no non-debug caller may rely on it. */
  virtual int64_t snapshot_size() const = 0;
};

/** Null when no effect handles this brush; the caller then refuses to start a session. */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_create(
    const Brush &brush,
    Object &ob,
    PaintModeSettings &paint_mode_settings);

/** Builds the vertex-color effect. Called only by #curve_patch_effect_create; returns null when the
 * mesh has no usable active color attribute. */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_color_create(const Object &ob);

/** Builds the image-canvas effect. Called only by #curve_patch_effect_create; returns null when the
 * brush's Paint Mode canvas cannot be resolved (`SCULPT_use_image_paint_brush()` already gates the
 * call, so this is a defensive re-check, not the primary guard). */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_image_create(
    Object &ob,
    PaintModeSettings &paint_mode_settings);

}  // namespace blender::ed::sculpt_paint
