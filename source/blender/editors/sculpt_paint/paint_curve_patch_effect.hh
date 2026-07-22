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
#include <optional>

namespace blender {
struct Brush;
struct Depsgraph;
struct Object;
struct PaintModeSettings;
struct Scene;
}  // namespace blender

namespace blender::ed::sculpt_paint {

struct CurvePatchItem;
struct CurvePatchSession;
/* Opaque: defined in `mesh/sculpt_intern.hh`, which this header deliberately does not pull in --
 * only the enumeration's identity is needed here. */
enum class UpdateType;

/** Which target a Curve Patch session writes to.
 *
 * Split out of the factory so the choice can be stated rather than inferred: the Python API of
 * Stage 5+ selects an effect explicitly, whereas the editor infers it from the active brush. */
enum class CurvePatchEffectType : int8_t {
  Relief = 0,
  Color = 1,
  Image = 2,
};

class CurvePatchEffect {
 public:
  virtual ~CurvePatchEffect() = default;

  /** What the viewport (and, for the image canvas, any open Image editor) must refresh after this
   * effect writes. The session issues the flushes; the effect only names the kind, because the
   * flush helpers take a `bContext`/`ViewContext` this interface has no business holding. */
  virtual UpdateType update_type() const = 0;

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

  /** Count the snapshot keys index into, compared against `CurvePatchApplyState::element_num` every
   * restamp to detect a mesh that changed underneath the session. */
  virtual int64_t element_num(Object &ob) const = 0;

  /**
   * Write the snapshot back. Runs FIRST in every restamp, before the spline rebuild, and
   * consumes the previous restamp's `CurvePatchApplyState::last_restamp_nodes`.
   */
  virtual void restore(Object &ob, const CurvePatchSession &patch) = 0;

  /**
   * Once per restamp, after the ribbon is built and before the first symmetry pass: refresh
   * whatever input the sampler and the write step will read. Relief recomputes vertex normals
   * here; an effect that neither reads nor invalidates them does nothing.
   */
  virtual void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchSession &patch) = 0;

  /**
   * Both phases for one symmetry pass of ONE patch: gather in parallel, then apply serially.
   * Called by `do_symmetrical_brush_actions()` once per enabled pass, and by the session once per
   * patch within that pass; the serial half is the sole writer of
   * `CurvePatchApplyState::pass_weight_accum` and of the target data.
   *
   * `patch` carries what is shared with the whole session (texture binding, apply state); `item`
   * is the one patch this call writes. Two overlapping items reach the same vertex through
   * `pass_weight_accum`, exactly as two symmetry passes do -- so Relief and Image AVERAGE their
   * contributions rather than stacking them, while Color, which does not use the accumulator,
   * lets the last item win.
   */
  virtual void apply_pass(const Depsgraph &depsgraph,
                          Object &ob,
                          const Brush &brush,
                          CurvePatchSession &patch,
                          const CurvePatchItem &item) = 0;

  /** After all passes: whatever finishing work this target needs (relief smooths its profile).
   * The viewport flush that used to live here is issued by the session instead -- see
   * #update_type. */
  virtual void end_restamp(Object &ob, CurvePatchSession &patch) = 0;

  /**
   * Fold the session into the real undo stack.
   *
   * `scene` and `depsgraph` are passed explicitly rather than fetched from a `bContext`: an effect
   * must be usable from a caller that has no window manager at all, which is what the headless
   * apply path of Stage 4 needs. Not every implementation reads them.
   */
  virtual void commit(const Scene &scene,
                      const Depsgraph &depsgraph,
                      Object &ob,
                      const CurvePatchSession &patch) = 0;

  /** Elements the snapshot currently holds. Exists only for the `CURVE_PATCH_PROFILING` line in
   * `curve_patch_restore_and_restamp()`, which reported it before the snapshot moved off the
   * cache; no non-debug caller may rely on it. */
  virtual int64_t snapshot_size() const = 0;
};

/**
 * Which effect the brush implies on this object, or nullopt when none does -- the brush is not a
 * Curve Patch brush, or the object is Dynamic Topology, which has no stable per-element index for
 * any snapshot to key into.
 *
 * Split from #curve_patch_effect_create so a caller can override the choice instead of inferring
 * it: the Python API of Stage 5+ names the effect outright.
 */
std::optional<CurvePatchEffectType> curve_patch_effect_type_for_brush(
    const Brush &brush,
    Object &ob,
    PaintModeSettings &paint_mode_settings);

/** Build one effect of the named type. Null when the object cannot actually carry it -- no usable
 * color attribute, or an unresolvable canvas -- which the type alone cannot rule out. */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_create(
    CurvePatchEffectType type,
    Object &ob,
    PaintModeSettings &paint_mode_settings);

/** #curve_patch_effect_type_for_brush followed by #curve_patch_effect_create. Null when no effect
 * handles this brush; the caller then refuses to start a session. */
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
