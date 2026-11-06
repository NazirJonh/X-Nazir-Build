/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * What ONE TARGET of a Curve Patch session writes inside a sculpt object, and how it takes it
 * back.
 *
 * One effect instance writes one attribute of one target object: every method here operates on a
 * #CurvePatchTargetState (an `Object &` plus that object's apply state, items and cache -- one
 * element of `CurvePatchSession::targets`), `element_num()` is measured against `Mesh::verts_num`,
 * `restore()` consumes a PBVH node mask, and `apply_pass()` is driven by
 * `do_symmetrical_brush_actions()`. What varies between these effects is which ATTRIBUTE of that
 * one sculpt object is written -- positions, mask, color, texture pixels. Multi-object sessions
 * are not an effect concern: a session holds one effect instance per target, and the session
 * (`paint_curve_patch_session.hh`) is what fans a re-stamp, a restore and a commit out over them.
 *
 * A new target that is not a sculpt object (the flat canvas is the first) implements
 * #CurvePatchHost instead and writes in whatever way suits it; the seam between the two is one
 * whole re-stamp, not one element. Trying to synthesize an `Object` and a PBVH mask to reach this
 * interface would be the wrong end of the abstraction.
 *
 * The geometry half of the re-stamp lives in `paint_curve_patch_sampler.hh` and is shared by every
 * target, sculpt or not. An effect owns both application phases rather than merely consuming
 * samples, because phase 1 has to capture the pre-patch value of the attribute (a position for
 * relief, a color for paint), whose type differs per effect.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "BLI_assert.h"
#include "BLI_index_mask.hh"

#include "paint_curve_patch_profile.hh"

namespace blender {
struct Brush;
struct Paint;
struct Depsgraph;
struct Object;
struct PaintModeSettings;
struct Scene;
}  // namespace blender

namespace blender::ed::sculpt_paint {

struct CurvePatchItem;
struct CurvePatchSession;
/** Per-target view the effects operate on; defined in `paint_curve_patch_session.hh` next to the
 * target state it views. */
struct CurvePatchTargetState;
/* Opaque: defined in `mesh/sculpt_intern.hh`, which this header deliberately does not pull in --
 * only the enumeration's identity is needed here. */
enum class UpdateType;

/** Which target a Curve Patch session writes to.
 *
 * Split out of the factory so the choice can be stated rather than inferred: a scripted caller
 * selects an effect explicitly, whereas the editor infers it from the active brush. */
enum class CurvePatchEffectType : int8_t {
  Relief = 0,
  Color = 1,
  Image = 2,
  Mask = 3,
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

  /** Count the snapshot keys index into, compared against `CurvePatchApplyState::element_num`
   * every restamp to detect a mesh that changed underneath the session. */
  virtual int64_t element_num(Object &ob) const = 0;

  /**
   * Write the snapshot back. Runs FIRST in every restamp, before the spline rebuild, and
   * consumes the previous restamp's `CurvePatchApplyState::last_restamp_nodes`.
   */
  virtual void restore(CurvePatchTargetState &target) = 0;

  /**
   * Immediately after #restore, before anything is rebuilt or written: adopt whatever the
   * session's OUTPUT configuration has become since the last restamp.
   *
   * A live session outlives panel edits -- that is the whole point of the live-sync poll -- and
   * some of those edits change WHERE the patch writes, not just what it writes. Toggling a PBR
   * channel is the case that forced this to exist: it adds or removes a destination image, and an
   * effect that froze its destinations at session start would either keep painting a channel the
   * user just switched off or silently paint nothing at all.
   *
   * Ordering is the contract: #restore has already put the pristine content back through the OLD
   * destinations, so re-targeting here cannot strand painted pixels on a canvas that is about to
   * stop being addressed. Nothing may be written from this call.
   */
  virtual void sync_targets(Object & /*ob*/) {}

  /**
   * Once per restamp, after the ribbon is built and before the first symmetry pass: refresh
   * whatever input the sampler and the write step will read. Relief recomputes vertex normals
   * here; an effect that neither reads nor invalidates them does nothing.
   */
  virtual void begin_restamp(const Depsgraph &depsgraph, CurvePatchTargetState &target) = 0;

  /**
   * Both phases for one symmetry pass of ONE patch: gather in parallel, then apply serially.
   * Called by `do_symmetrical_brush_actions()` once per enabled pass, and by the session once per
   * patch within that pass; the serial half is the sole writer of
   * `CurvePatchApplyState::pass_weight_accum` and of the target data.
   *
   * `target` carries what the whole target shares (its apply state, its items, the document's
   * texture binding); `item` is the one patch this call writes. The passes of one patch run back
   * to back and average (symmetry); a patch stamped later, `CurvePatchApplyState::patch_rank`
   * higher, is composited over the earlier ones -- see #curve_patch_blend_over. Relief and Mask
   * composite a scalar, Color and Image composite the pre-multiplied color and its coverage, so a
   * lower curve shows through wherever an upper one is partially transparent.
   */
  virtual void apply_pass(const Depsgraph &depsgraph,
                          const Brush &brush,
                          CurvePatchTargetState &target,
                          const CurvePatchItem &item) = 0;

  /** After all passes: whatever finishing work this target needs (relief smooths its profile).
   * The viewport flush that used to live here is issued by the session instead -- see
   * #update_type. */
  virtual void end_restamp(CurvePatchTargetState &target) = 0;

  /**
   * Whether committing this target has anything to store into the shared sculpt undo step (see
   * #CurvePatchCommitStep). Drives whether the commit orchestrator opens / extends the step for
   * this target at all. The image canvas keeps its own undo system and stores nothing here.
   */
  virtual bool commit_has_content(const CurvePatchTargetState &target) const = 0;

  /**
   * Whether this target's commit wants a face-set write, computed BEFORE the shared step opens --
   * the answer (over every target) decides whether the shared step is forced into the stack or
   * left parked (see #CurvePatchCommitStep::face_set_follows). Relief only; the returned masks
   * must stay alive through #commit_face_set via \a memory.
   */
  virtual bool commit_wants_face_set(CurvePatchTargetState & /*target*/,
                                     IndexMaskMemory & /*memory*/,
                                     IndexMask & /*r_face_mask*/,
                                     IndexMask & /*r_node_mask*/)
  {
    return false;
  }

  /**
   * Store this target's undo data into the OPEN shared sculpt undo step of its session (see
   * #CurvePatchCommitStep). The step is opened / extended by the session's commit orchestrator
   * (`undo::push_begin_ex` / `undo::push_begin_add_object`) and parked once for the whole session
   * by `undo::push_end_all_ex(false, false)` -- effects must NOT open or close it themselves.
   *
   * \a depsgraph is passed explicitly rather than fetched from a `bContext`: an effect must be
   * usable from a caller that has no window manager at all, which is what the headless apply path
   * needs.
   */
  virtual void commit_push_nodes(const Depsgraph &depsgraph, CurvePatchTargetState &target) = 0;

  /**
   * Write this target's raised faces into the OPEN shared face-set undo step, under the
   * session-shared face set id (every target of the commit writes the SAME id, so the multi-object
   * commit stays one Ctrl+Z). Relief only; the face-set step itself is opened / parked by the
   * commit orchestrator, exactly like the position step above.
   */
  virtual void commit_face_set(CurvePatchTargetState & /*target*/,
                               int /*face_set_id*/,
                               const IndexMask & /*face_mask*/,
                               const IndexMask & /*node_mask*/)
  {
    BLI_assert_unreachable();
  }

  /**
   * Exchange the live preview with the pre-patch snapshot for every snapshotted element of the
   * original #Mesh. Calling it twice is the identity. Exists for the memfile undo encoder: the
   * preview is not committed, so a global undo snapshot taken mid-session (a texture assignment, any
   * `OPTYPE_UNDO` operator) must capture the surface as it was before the patch, or undoing to that
   * step brings the uncommitted preview back -- and with sculpt layers, a surface that no longer
   * matches `base + layers`.
   *
   * Nothing is tagged: two calls leave the data exactly as it was. Only data the memfile writes
   * needs this; multires grids and image pixels are not part of it, so no-op by default.
   */
  virtual void preview_swap(Object & /*ob*/) {}

  /** Elements the snapshot currently holds. Exists only for the `CURVE_PATCH_PROFILING` line in
   * `curve_patch_restore_and_restamp()`, which reported it before the snapshot moved off the
   * cache; no non-debug caller may rely on it. */
  virtual int64_t snapshot_size() const = 0;

  /** Elements THIS restamp wrote, for the same `CURVE_PATCH_PROFILING` line's `displaced` column.
   *
   * `nullopt` means "count me the standard way", i.e. from the shared cross-pass accumulator,
   * which holds one entry per written element for every effect that blends through
   * #curve_patch_blend_across_passes. Only an effect that accumulates elsewhere returns a value.
   *
   * Deliberately takes no `CurvePatchApplyState`: that type lives in `paint_curve_patch_session.hh`,
   * which includes THIS header, so naming it here would close a cycle. The caller already holds
   * the state and applies the fallback itself.
   *
   * Debug-only, exactly as #snapshot_size is; no non-debug caller may rely on it. */
  virtual std::optional<int64_t> displaced_size() const
  {
    return std::nullopt;
  }
};

/**
 * Which effect the brush implies on this object, or nullopt when none does -- the brush is not a
 * Curve Patch brush, or the object is Dynamic Topology, which has no stable per-element index for
 * any snapshot to key into.
 *
 * Split from #curve_patch_effect_create so a caller can override the choice instead of inferring
 * it: a scripted caller names the effect outright.
 */
std::optional<CurvePatchEffectType> curve_patch_effect_type_for_brush(
    const Brush &brush, const Paint &paint, Object &ob, PaintModeSettings &paint_mode_settings);

/** Why #curve_patch_effect_create refused, for the caller to report. */
enum class CurvePatchRefusal : int8_t {
  None = 0,
  /** The brush type has no Curve Patch meaning (#bke::brush::supports_curve_patch). */
  BrushType,
  /** Multires or Dynamic Topology: no stable per-element index to snapshot. */
  Geometry,
  /** Paint Mode canvas is an image/material map, but no target resolved. */
  NoImageCanvas,
  /** Color Attribute or Poly Paint canvas with no attribute to write. */
  NoAttribute,
};

/** Human-readable reason for \a refusal, or null when there is none. */
const char *curve_patch_refusal_message(CurvePatchRefusal refusal);

/** Build one effect of the named type. Null when the object cannot actually carry it -- no usable
 * color attribute, or an unresolvable canvas -- which the type alone cannot rule out. */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_create(
    CurvePatchEffectType type, Object &ob, PaintModeSettings &paint_mode_settings);

/** #curve_patch_effect_type_for_brush followed by #curve_patch_effect_create. Null when no effect
 * handles this brush; the caller then refuses to start a session. */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_create(
    const Brush &brush, const Paint &paint, Object &ob, PaintModeSettings &paint_mode_settings);

/** Builds the vertex-color effect. Called only by #curve_patch_effect_create; returns null when
 * the mesh has no usable color attribute to write.
 *
 * Two canvases land here. #PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE paints the mesh's ACTIVE color
 * attribute with the brush color. #PAINT_CANVAS_SOURCE_MATERIAL_PAINT (Poly Paint) paints the
 * NAMED attribute the material paint settings assign to Base Color, with that channel's own
 * color and blend mode, and undoes through a #undo::Type::Material step. The effect class is
 * the same either way -- only the target differs -- so no separate #CurvePatchEffectType is
 * needed. */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_color_create(
    const Object &ob, PaintModeSettings &paint_mode_settings);

/** Builds the image-canvas effect. Called only by #curve_patch_effect_create; returns null when
 * the brush's Paint Mode canvas cannot be resolved (`SCULPT_use_image_paint_brush()` already gates
 * the call, so this is a defensive re-check, not the primary guard). */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_image_create(
    Object &ob, PaintModeSettings &paint_mode_settings);

/** Builds the sculpt-mask effect. */
std::unique_ptr<CurvePatchEffect> curve_patch_effect_mask_create();

}  // namespace blender::ed::sculpt_paint
