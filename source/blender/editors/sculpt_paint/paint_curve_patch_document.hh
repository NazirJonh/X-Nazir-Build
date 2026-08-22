/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The state a live Curve Patch edit consists of, with no target attached: the control curves the
 * user edits, the texture slots one build resolves, the session-local undo stack, and the live
 * brush inputs the last stamp was made with.
 *
 * Both editing sessions hold one of these by value -- 3D Sculpt Mode's #CurvePatchSession and the
 * Image Editor's #ImageCurvePatchSession -- so that the shared modal editor can act on a patch
 * without knowing which of the two owns it. What stays on each session instead is everything that
 * describes ITS target: the effect and #CurvePatchApplyState for the mesh, the image undo
 * transaction and the reference tile for the canvas.
 *
 * The core geometry and parameters live one layer further down, in `BKE_curve_patch.hh`.
 */

#include <array>

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_texture_types.h"

#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

#include "paint_curve_patch_live.hh"

namespace blender::ed::sculpt_paint {

/* Curve Patch core geometry and parameters live in blenkernel. These aliases keep call sites in
 * this module unqualified; the free `curve_patch_*` functions are not covered and carry an
 * explicit `bke::` at their call sites. */
using bke::CURVE_PATCH_MAX_FRAMES;
using bke::CurvePatchEndFalloff;
using bke::CurvePatchFrame;
using bke::CurvePatchFrameRange;
using bke::CurvePatchFrameSet;
using bke::CurvePatchFramesParams;
using bke::CurvePatchGeometry;
using bke::CurvePatchLengthMode;
using bke::CurvePatchParams;
using bke::CurvePatchPointShape;
using bke::CurvePatchRibbonLut;
using bke::CurvePatchSpline;
using bke::CurvePatchStamp;
using bke::CurvePatchStampMode;
using bke::CurvePatchStampProjection;
using bke::CurvePatchSurfaceSnapshot;
using bke::CurvePatchTextureZone;
using bke::CurvePatchTextureZoneSample;

/** One patch's contribution to an undo snapshot. */
struct CurvePatchEditStepItem {
  bke::CurvesGeometry curve;
  /* The full frozen-parameter set (radius, radius_per_size, plane_normal, swap_axis, stamp_seed,
   * final_quality), not just `swap_axis` / `stamp_seed`: a step can hold MORE items than the
   * session currently does (redo past a patch delete, once that exists), and restoring such a
   * newly-grown item needs its frozen fields from somewhere -- there is no live brush value to
   * fall back on for a patch that already finished its stroke. Live fields get overwritten by the
   * next poll's #curve_patch_params_live_overlay regardless, so storing the whole struct costs
   * nothing on the common path. */
  bke::CurvePatchParams params;
};

/** One snapshot of the session-local undo stack: everything the user edits inside a live Curve
 * Patch that the relief is derived from. The control curve carries positions, handles, handle
 * types, radii and `cyclic` internally, so nothing else about it needs storing.
 *
 * Every patch is snapshotted, not just the active one: `swap_axis` and `stamp_seed` are per-patch
 * (they are `CurvePatchParams` fields), and an undo that restored one curve while leaving its
 * neighbors at their current state would be unpredictable the moment curves can be switched. */
struct CurvePatchEditStep {
  Vector<CurvePatchEditStepItem> items;
  int active_patch = 0;
};

/** Resolved texture slots for one build. Stays in the editor layer because `MTex` is DNA and the
 * core must not depend on it. */
struct CurvePatchTextureBinding {
  /** Resolved texture variants for this restamp: copies of `Brush::mtex` with only `tex` swapped,
   * so every mapping setting (Size / Offset / Angle / Swap Axis) stays shared. Rebuilt on every
   * restamp because the texture source toggles are live-synced like
   * `CurvePatchParams::stamp_mode`. Empty in SINGLE mode, where the relief reads `brush.mtex`
   * directly.
   *
   * The `tex` inside a variant is a raw pointer, not a registered ID reference. Safe within
   * Blender's event model: this array is rebuilt at the top of every restamp from the brush's own
   * (registered) fields, and a restamp is not interrupted, so there is no window in which a
   * deleted texture could be read from here.
   *
   * #Array rather than #Vector: `MTex` carries #DNA_DEFINE_CXX_METHODS, which DELETES its copy and
   * move constructors so a DNA struct cannot be duplicated except through the explicit
   * shallow-copy path. `Vector` relocates its elements when it grows, which needs a move
   * constructor; `Array` only ever default-constructs in place, so it is the container this type
   * can actually live in. Sized once per restamp and filled via `dna::shallow_copy()`. */
  Array<MTex> stamp_texture_variants;
  /** Cumulative weight table over `stamp_texture_variants`, one entry per variant, non-decreasing.
   * Empty unless STAMPS + LIST is active with at least one positive weight. */
  Vector<float> stamp_texture_weights_cdf;
  /** Ribbon CAPS variants indexed by #CurvePatchTextureZone. An entry with a null `tex` marks a
   * zone the user left empty; the relief returns no displacement there. Left default-constructed
   * rather than `= {}`-initialized: `MTex`'s deleted copy assignment (see #DNA_DEFINE_CXX_METHODS)
   * makes `std::array::fill()` and any copy-based reset ill-formed, so entries are reset
   * element-wise through `dna::shallow_zero_initialize()` instead. */
  std::array<MTex, 3> ribbon_zone_variants;
  /** Whether the Ribbon texture source is MULTI this restamp -- NOT "the caps are being drawn". It
   * is set purely from `BrushCurvePatchSettings::ribbon_texture_source`, deliberately without also
   * testing
   * `stamp_mode`, so it is true in Stamps mode too even though Stamps has no caps. Adding that
   * test would create a second place obliged to stay in sync with the relief's branch selector,
   * which is exactly the drift `CurvePatchGeometry::stamp_search_reach` was burned by. Instead the
   * invariant is one-way: ONLY the Ribbon branch of `branch_relief()` reads this field (and
   * `ribbon_zone_variants` / `world_cap_*` with it), and that branch runs only when `stamp_mode`
   * is not STAMPS.
   *
   * The two cap lengths are already resolved from brush diameters into world units. */
  bool caps_enabled = false;
  float world_cap_start = 0.0f;
  float world_cap_end = 0.0f;
};

/**
 * One patch: a curve the user drew, the brush values frozen when it started, and everything built
 * from the two.
 *
 * `params` is per-item rather than per-session because a curve started later can have been drawn
 * with a different brush size: `radius`, `plane_normal`, `stamp_seed` and `swap_axis` are frozen
 * at the moment that curve begins. What is NOT per-item is anything describing the TARGET -- the
 * effect, the texture binding and `CurvePatchApplyState` all describe the mesh, which is shared.
 */
struct CurvePatchItem {
  /** Index of the source PaintCurve spline. -1 for procedurally-created curves. */
  int source_curve_index = -1;

  /** The user-editable control curve. Not attached to any `Brush`/datablock -- a standalone
   * runtime `CurvesGeometry`, built fresh via `paintcurve_geometry_init_bezier()` (see
   * `paint_curve_geometry.cc:546`) at Curve Patch start. */
  bke::CurvesGeometry control_curve;

  /** Brush/texture parameter values frozen at the moment this patch's stroke started. Re-stamp
   * reads these instead of the live `Brush`/`MTex`, so a mid-edit change to the brush panel's
   * sliders cannot alter an already-started Curve Patch.
   *
   * NOTE: brush *strength* is deliberately NOT frozen here -- it is read live from the brush on
   * every re-stamp (see `curve_patch_restore_and_restamp()`, which passes `std::nullopt` as
   * `do_symmetrical_brush_actions()`'s `forced_bstrength` so it recomputes `brush_strength()` from
   * the current UI slider). Frozen vs live fields are classified by
   * #curve_patch_params_live_overlay; the modal poll calls that helper rather than picking fields
   * by hand. */
  bke::CurvePatchParams params;

  bke::CurvePatchGeometry geometry;
};

/**
 * Everything one Curve Patch edit consists of, independent of what it is stamped onto.
 *
 * Held by value by both sessions. Nothing here knows about a mesh, a canvas, an undo system or a
 * region -- that is what makes it the state the shared editor can act on.
 */
struct CurvePatchDocument {
  /** Every patch this document applies, in creation order. Applied one after another within each
   * symmetry pass; see `curve_patch_apply_effect_action()`.
   *
   * The interactive editor currently publishes exactly one. The plural exists because the headless
   * apply path can be asked for every spline of a paint curve at once, and because switching
   * between curves in the modal is the next step. */
  Vector<CurvePatchItem> patches;

  /** Index into #patches the modal editor is acting on. Consumers must validate it against
   * `patches.size()`: the document outlives the modal. */
  int active_patch = 0;

  /** Index into `patches[active_patch].control_curve` of the point the user last interacted with,
   * or -1 for none. The patch it belongs to is #active_patch: the pair is what identifies a point,
   * because two patches index their points independently.
   *
   * Owned conceptually by the live-edit modal, but stored here rather than in its `op->customdata`
   * so the small operators the modal's context menu invokes -- which cannot reach a running
   * modal's customdata -- can act on the clicked point. Because this document outlives the modal,
   * the index is reset to -1 whenever an edit session ends or a new control curve is built;
   * consumers must still validate it against `control_curve.points_num()`. */
  int active_point = -1;

  CurvePatchTextureBinding texture;

  /** Live brush state as of the last stamp. Compared against a fresh capture on every modal event
   * so a change made from the UI (which pushes nothing at the session -- RNA only sends
   * `NC_BRUSH | NA_EDITED`) is noticed and re-stamped. */
  CurvePatchLiveInputs last_synced;

  /** Resolved geometry-build parameters as of that same stamp, compared alongside #last_synced.
   * Kept separately rather than inside #last_synced because it is derived state, not a live brush
   * input -- see the note on #CurvePatchLiveInputs.
   *
   * Written by the flat-canvas session only. 3D Sculpt Mode compares the freshly resolved
   * parameters against `active_item().params` instead, which it overwrites on the same event; the
   * canvas cannot do that because its rebuild resolves the parameters again, downstream of the
   * compare. */
  bke::CurvePatchParams last_synced_params;

  /** Session-local undo stack, owned by the live-edit modal.
   *
   * Blender's own undo systems cannot cover this: a sculpt undo step stores mesh attributes only
   * (see #undo::Type) and has no slot for the control curve, so an official step would restore
   * vertex positions and leave the curve untouched; and the paint-curve undo system refuses Sculpt
   * Mode outright (`paintcurve_undosys_poll`) and wants a real `PaintCurve` ID, which this runtime
   * curve is not. A modal that owns runtime state no undo type describes keeps its own stack --
   * exactly what the knife tool does with `KnifeUndoFrame` / `kcd->undostack`
   * (`editmesh_knife.cc`).
   *
   * Holds STATES, not deltas, with #undo_step_current as the cursor: entry 0 is the state the
   * anchor stroke produced, and a new snapshot truncates any redo branch above the cursor. */
  Vector<CurvePatchEditStep> undo_steps;
  int undo_step_current = -1;
  /** True once #CURVE_PATCH_UNDO_STEPS_MAX has forced the oldest step out. Index 0 is then no
   * longer the anchor stroke's state, so #curve_patch_undo_step_back must not read "back at 0" as
   * "nothing left to undo, cancel the patch" -- there IS a state at 0, it is just not the start.
   */
  bool undo_anchor_trimmed = false;

  /** Whether #active_item may be dereferenced. False only on a half-built document: every publish
   * path appends a patch before publishing. */
  bool has_active_item() const
  {
    return this->patches.index_range().contains(this->active_patch);
  }

  /** The patch the modal editor acts on. */
  CurvePatchItem &active_item()
  {
    BLI_assert(this->has_active_item());
    return this->patches[this->active_patch];
  }
  const CurvePatchItem &active_item() const
  {
    BLI_assert(this->has_active_item());
    return this->patches[this->active_patch];
  }
};

/** Return the patch corresponding to a PaintCurve spline, or -1 when no exact match exists. */
int curve_patch_index_for_source_curve(const CurvePatchDocument &document, int source_curve_index);

/**
 * Set the build-quality switch on every patch.
 *
 * `final_quality` describes the BUILD, not one curve -- every patch of a document is always
 * rebuilt at the same quality -- but it lives in the per-patch #bke::CurvePatchParams because that
 * is what the core build takes. Readers take it off #CurvePatchDocument::active_item.
 */
inline void curve_patch_set_final_quality(CurvePatchDocument &document, const bool value)
{
  for (CurvePatchItem &item : document.patches) {
    item.params.final_quality = value;
  }
}

}  // namespace blender::ed::sculpt_paint
