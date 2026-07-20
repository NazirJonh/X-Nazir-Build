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
struct bContext;
}  // namespace blender

namespace blender::ed::sculpt_paint {

struct CurvePatchCache;

class CurvePatchEffect {
 public:
  virtual ~CurvePatchEffect() = default;

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
std::unique_ptr<CurvePatchEffect> curve_patch_effect_create(const Brush &brush, const Object &ob);

}  // namespace blender::ed::sculpt_paint
