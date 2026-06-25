/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt Lattice Tool: state machine, operators and lifecycle.
 *
 * ADR-1: Standalone Tool (not a brush type).
 * ADR-2: Variant C (hybrid) — temp OB_LATTICE + direct write to sculpt coords.
 * ADR-11: Variant A mask semantics — mask protects vertices; cage fits unmasked region.
 * ADR-12: Lazy init — state is created on first LMB, not on toolbar selection.
 * ADR-13: Raw pointer in SculptSession (full type lives in editors).
 *
 * See .My_Docs_July_2026/Sculpt-Mode/Lattice-Tool/Plan_1/03_data_model.md
 */

#pragma once

#include <optional>

#include "BLI_array.hh"
#include "BLI_bounds_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {

struct Main;
struct Object;
struct Scene;
struct LatticeDeformData;
struct Depsgraph;
struct bContext;
struct wmOperatorType;

namespace ed::sculpt_paint::lattice {

/**
 * A vertex participates in cage bbox / deformation when (1.0f - mask) > eps.
 * Variant A semantics (ADR-11): mask protects vertices. Default for #LatticeToolData.
 */
constexpr float SCULPT_LATTICE_MASK_EPS_DEFAULT = 1e-4f;

/** Phase of the tool state machine (see 02_ux_state_machine.md). */
enum class Phase {
  Inactive = 0,
  Editing,
};

/**
 * Vertices affected by the current drag: rest coords, vert indices and mask snapshot.
 * Rebuilt at the beginning of each slide (see 05_deform_pipeline.md).
 */
struct AffectedRegion {
  /** Rest positions of affected verts (object-space) captured at slide start. */
  Array<float3> rest_coords;
  /**
   * Last positions this tool wrote for each affected vert, seeded from #rest_coords at slide
   * start. The lattice deform is an absolute rest -> target mapping, but #PositionDeformData::deform
   * is incremental (`+=`). Computing the per-frame translation as `target - current_coords[i]`
   * (instead of `target - eval[v]`) keeps the deform self-consistent: the forced full re-eval
   * stopgap desynchronises #PositionDeformData::eval from the mesh, which otherwise makes the
   * incremental translation diverge (runaway roll-up). Tracking the applied position ourselves
   * removes that dependency.
   */
  Array<float3> current_coords;
  /** Affected vert indices. */
  Vector<int> verts;
  /** Snapshot of mask[v] for each vert (0..1, full-protection = 1). */
  Array<float> mask;
};

/**
 * Per-object tool state, owned via a raw pointer on SculptSession (ADR-13).
 */
struct LatticeToolData {
  /** Temp OB_LATTICE (original-side). Created in ensure_session. */
  Object *lattice_ob = nullptr;

  Phase phase = Phase::Inactive;

  /** Lattice resolution per axis (min 2, ADR-8). */
  int3 resolution = int3(3, 3, 3);

  /** RNA mirror properties. */
  float strength = 1.0f;
  float margin = 0.1f;
  float mask_eps = SCULPT_LATTICE_MASK_EPS_DEFAULT;
  /** KEY_LINEAR / KEY_CARDINAL / KEY_BSPLINE (ADR-5: KEY_LINEAR by default). */
  short interpolation = 0 /* KEY_LINEAR */;
  bool keep_as_modifier = false;

  /** Affected verts / rest / mask for the current drag. */
  AffectedRegion current;

  /** Snapshot of all mesh positions on entry into Editing — for Cancel (Esc). */
  Array<float3> entry_positions;

  /** BKE lattice deform context, rebuilt on every MOUSEMOVE (cage changed). */
  LatticeDeformData *deform_data = nullptr;

  /** Index of the BPoint currently being dragged (screen-space pick result). */
  int pending_drag_index = -1;

  /** True while a modal slide is active (suppresses hover overlay, phase 2). */
  bool drag_active = false;
};

/**
 * Lazy-initialises the tool session on first interaction (ADR-12):
 *  - computes unmasked bbox (variant A)
 *  - creates temp OB_LATTICE fitted to it
 *  - snapshots entry_positions for Cancel
 * Returns false (and reports) if there is nothing to deform.
 */
bool sculpt_lattice_ensure_session(bContext *C);

/**
 * Releases all tool state for \a ob_mesh: destroys temp OB_LATTICE, deform context,
 * snapshots. Idempotent. Used by confirm / cancel / tool-switch / mode-exit.
 */
void sculpt_lattice_session_free(Main *bmain, Scene *scene, Object *ob_mesh);

/**
 * Poll: OB_MODE_SCULPT on OB_MESH, and the active tool is `builtin.sculpt_lattice`.
 */
bool sculpt_lattice_tool_active_poll(bContext *C);

/**
 * One-time editor registration: installs the #BKE_sculpt_lattice_state_free_cb hook so
 * the tool state is released when a sculpt session is freed. Idempotent.
 */
void sculpt_lattice_register();

/** Operators registered in sculpt_ops.cc. */
void SCULPT_OT_lattice_tool(wmOperatorType *ot);
void SCULPT_OT_lattice_pick(wmOperatorType *ot);
void SCULPT_OT_lattice_slide(wmOperatorType *ot);
void SCULPT_OT_lattice_confirm(wmOperatorType *ot);
void SCULPT_OT_lattice_cancel(wmOperatorType *ot);

/* -------------------------------------------------------------------- */
/** \name Deform pipeline (sculpt_lattice_deform.cc)
 *
 * Declared in the public header so operators (sculpt_lattice.cc) can call them.
 * Full bodies live in sculpt_lattice_deform.cc.
 * \{ */

/**
 * Axis-aligned bbox of unmasked verts (variant A). Returns false if everything is masked.
 */
bool sculpt_lattice_compute_deform_bounds(const Depsgraph &depsgraph,
                                          const Object &ob_mesh,
                                          float mask_eps,
                                          std::optional<Bounds<float3>> &r_bounds);

/**
 * Builds the affected vert list / rest / mask snapshot against the original positions.
 * Called ONCE at session start: the cage is the single deformation accumulator, so `rest_coords`
 * must stay the original positions for the whole session (rebuilding it per drag double-applies
 * prior drags).
 */
void sculpt_lattice_build_affected_region(const Depsgraph &depsgraph,
                                          Object &ob_mesh,
                                          LatticeToolData &state);

/**
 * Applies the lattice deformation live (MOUSEMOVE). Writes through PositionDeformData
 * and tags affected PBVH nodes.
 */
void sculpt_lattice_deform_apply(const Depsgraph &depsgraph,
                                 Object &ob_mesh,
                                 LatticeToolData &state);

/** \} */

}  // namespace ed::sculpt_paint::lattice
}  // namespace blender
