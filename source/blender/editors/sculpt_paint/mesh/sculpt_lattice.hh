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
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_key_types.h" /* For #KeyInterpolationType. */

namespace blender {
struct Main;
struct Object;
struct Scene;
struct LatticeDeformData;
struct Depsgraph;
struct bContext;
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::lattice {

/**
 * A vertex participates in cage bbox / deformation when (1.0f - mask) > eps.
 * Variant A semantics (ADR-11): mask protects vertices. Default for #LatticeToolData.
 */
constexpr float SCULPT_LATTICE_MASK_EPS_DEFAULT = 1e-4f;

/**
 * Defaults shared by #LatticeToolData and the #SCULPT_OT_lattice_tool RNA properties, so the
 * struct initializers and the tool settings cannot drift apart.
 */
constexpr float SCULPT_LATTICE_STRENGTH_DEFAULT = 1.0f;
constexpr float SCULPT_LATTICE_MARGIN_DEFAULT = 0.1f;
constexpr int SCULPT_LATTICE_RESOLUTION_DEFAULT = 3;
/** Maximum control points per axis, matching the RNA property range. */
constexpr int SCULPT_LATTICE_MAX_RESOLUTION = 64;

/** Values of the #SCULPT_OT_lattice_transform `mode` property. */
enum {
  SCULPT_LATTICE_XFORM_TRANSLATE = 0,
  SCULPT_LATTICE_XFORM_ROTATE = 1,
  SCULPT_LATTICE_XFORM_RESIZE = 2,
};

/**
 * Phase of the tool state machine.
 *
 * In #Placement the cage control points are always at the neutral grid, which makes the lattice
 * deformation the identity for every vertex. Moving, rotating or scaling the cage in that phase
 * therefore cannot change the mesh, which is what lets the transform integration skip all deform
 * and PBVH work (see the design doc, section 3).
 */
enum class Phase {
  Inactive = 0,
  Placement,
  Deform,
};

/**
 * Vertices affected by the current deform phase: rest coords, vert indices and mask snapshot.
 * Built at session start and at each #Phase::Placement -> #Phase::Deform transition (see
 * #sculpt_lattice_build_affected_region), never rebuilt between drags within one deform phase.
 */
struct AffectedRegion {
  /**
   * Rest positions of affected verts (object-space), captured at session start and re-captured
   * at each #Phase::Placement -> #Phase::Deform transition; never rebuilt between drags.
   */
  Array<float3> rest_coords;
  /**
   * Last positions this tool wrote for each affected vert, seeded from #rest_coords at session
   * start. The lattice deform is an absolute rest -> target mapping, but #PositionDeformData::deform
   * is incremental (`+=`), so the per-frame translation is `target - current_coords[i]`.
   *
   * Tracking the applied position here rather than reading #PositionDeformData::eval keeps the
   * incremental math independent of when the depsgraph last re-evaluated the mesh. It is re-seeded
   * from the live positions at the start of every drag (see #SCULPT_OT_lattice_slide invoke) so
   * that edits made between drags — a global undo, another operator — cannot make it diverge.
   */
  Array<float3> current_coords;
  /** Affected vert indices. */
  Vector<int> verts;
  /** Snapshot of mask[v] for each vert (0..1, full-protection = 1). */
  Array<float> mask;

  /**
   * PBVH leaf nodes that contain at least one of #verts. Built with the region, reused by
   * #sculpt_lattice_tag_affected_nodes and per-drag undo. Invalidated when #affected_pbvh
   * no longer matches the live tree (PBVH rebuilt between drags).
   */
  Vector<int> affected_pbvh_nodes;
  /** Identity of the tree #affected_pbvh_nodes was built against, or null. */
  const void *affected_pbvh = nullptr;
  int affected_pbvh_nodes_num = -1;
};

/**
 * Per-object tool state, owned via a raw pointer on SculptSession (ADR-13).
 */
struct LatticeToolData {
  /** Temp OB_LATTICE (original-side). Created in ensure_session. */
  Object *lattice_ob = nullptr;

  Phase phase = Phase::Inactive;

  /** Lattice resolution per axis (min 2, ADR-8). */
  int3 resolution = int3(SCULPT_LATTICE_RESOLUTION_DEFAULT);

  /** RNA mirror properties. */
  float strength = SCULPT_LATTICE_STRENGTH_DEFAULT;
  float margin = SCULPT_LATTICE_MARGIN_DEFAULT;
  float mask_eps = SCULPT_LATTICE_MASK_EPS_DEFAULT;
  /** Cage interpolation (ADR-5: #KEY_LINEAR by default). */
  KeyInterpolationType interpolation = KEY_LINEAR;

  /** Affected verts / rest / mask for the current drag. */
  AffectedRegion current;

  /** Snapshot of all mesh positions on session start, for Cancel (Esc). */
  Array<float3> entry_positions;

  /**
   * BKE lattice deform context. Rebuilt when the cage transform or resolution changes;
   * during a control-point slide only the moved point is updated in place.
   */
  LatticeDeformData *deform_data = nullptr;

  /** Scratch buffer for #sculpt_lattice_deform_apply, reused across MOUSEMOVE. */
  Array<float3> translations;

  /** Index of the BPoint currently being dragged (screen-space pick result). */
  int pending_drag_index = -1;

  /**
   * Cage-space position of #pending_drag_index when the drag started, so cancelling a drag can
   * put the control point back. Without it the cage would keep the cancelled displacement and
   * silently re-apply it on the next drag.
   */
  float3 drag_start_point = float3(0.0f);

  /* NOTE: whether a modal slide is running is *not* tracked here. It has to be visible to
   * #BKE_sculpt_update_object_before_eval, which cannot see this type (ADR-13), so
   * #SculptSession::lattice_slide_active is the single source of truth and this struct would only
   * duplicate it. */
};

/**
 * Lazy-initializes the tool session on first interaction (ADR-12):
 *  - computes unmasked bbox (variant A)
 *  - creates temp OB_LATTICE fitted to it
 *  - snapshots entry_positions for Cancel
 * Returns false (and reports) if there is nothing to deform.
 */
bool sculpt_lattice_ensure_session(bContext *C);

/**
 * Releases all tool state for \a ob_mesh: destroys the no-main temp OB_LATTICE, deform context,
 * snapshots. Idempotent. Used by confirm / cancel / tool-switch / mode-exit.
 *
 * The temp object is a standalone no-main ID (ADR-15): it is freed directly with #BKE_id_free and
 * never lived in a scene / view-layer, so no #Main / #Scene context is needed here.
 */
void sculpt_lattice_session_free(Object *ob_mesh);

/**
 * Poll: OB_MODE_SCULPT on OB_MESH, and the active tool is `builtin.sculpt_lattice`.
 */
bool sculpt_lattice_tool_active_poll(bContext *C);

/**
 * Editor registration: installs the #BKE_sculpt_lattice_state_free_cb hook so the tool state is
 * released when a sculpt session is freed. Called from #operatortypes_sculpt.
 */
void sculpt_lattice_register();

/** Operators registered in sculpt_ops.cc. */
void SCULPT_OT_lattice_tool(wmOperatorType *ot);
void SCULPT_OT_lattice_pick(wmOperatorType *ot);
void SCULPT_OT_lattice_slide(wmOperatorType *ot);
void SCULPT_OT_lattice_confirm(wmOperatorType *ot);
void SCULPT_OT_lattice_cancel(wmOperatorType *ot);
void SCULPT_OT_lattice_phase_toggle(wmOperatorType *ot);
void SCULPT_OT_lattice_transform(wmOperatorType *ot);

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
 * Builds the affected vert list / rest / mask snapshot against the current mesh positions.
 *
 * Called at session start and on every #Phase::Placement -> #Phase::Deform transition, never
 * between drags inside #Phase::Deform. Within one deform phase the cage accumulates the whole
 * deformation, so re-snapshotting `rest_coords` per drag would double-apply the prior drags.
 * At a phase transition the cage has just been reset to neutral, which makes re-snapshotting
 * against the deformed mesh correct.
 */
void sculpt_lattice_build_affected_region(const Depsgraph &depsgraph,
                                          Object &ob_mesh,
                                          LatticeToolData &state);

/**
 * Computes the incremental translation for each affected vertex and advances the applied-position
 * tracker. Pure with respect to the sculpt session: it touches no #Object, #Depsgraph or PBVH, so
 * it can be exercised on its own.
 *
 * The mapping is absolute (`target = lattice(rest)`) while #PositionDeformData::deform is
 * incremental, hence `translation = target - current`:
 *   `target = mix(rest, lattice(rest), strength * (1 - mask))`
 *
 * \param deform_data: cage context from #sculpt_lattice_deform_data_rebuild.
 * \param rest_coords: original (undeformed) positions of the affected verts.
 * \param mask: per-vert mask snapshot, parallel to \a rest_coords.
 * \param current_coords: positions this tool last wrote; advanced to the new targets.
 * \param r_translations: per-vert deltas to hand to #PositionDeformData::deform.
 */
void sculpt_lattice_compute_translations(LatticeDeformData &deform_data,
                                         Span<float3> rest_coords,
                                         Span<float> mask,
                                         float strength,
                                         MutableSpan<float3> current_coords,
                                         MutableSpan<float3> r_translations);

/**
 * Applies the lattice deformation live (MOUSEMOVE). Writes through PositionDeformData
 * and tags affected PBVH nodes.
 */
void sculpt_lattice_deform_apply(const Depsgraph &depsgraph,
                                 Object &ob_mesh,
                                 LatticeToolData &state);

/**
 * Tags the PBVH nodes containing \a ar.verts as having changed positions and refreshes their
 * bounds.
 *
 * Required after every direct write through #PositionDeformData: with PBVH drawing active
 * #flush_update_step only tags #ID_RECALC_SHADING, so nothing else invalidates the draw buffers
 * and the viewport keeps showing the positions from before the write.
 */
void sculpt_lattice_tag_affected_nodes(const Depsgraph &depsgraph,
                                       Object &ob_mesh,
                                       AffectedRegion &ar);

/**
 * Re-seeds #AffectedRegion::current_coords from the live mesh positions.
 *
 * The incremental tracker assumes it is the only writer of the affected verts, which stops being
 * true between drags (global undo, another sculpt operator). Re-seeding before each drag makes the
 * next translation resolve against the mesh as it actually is; the cage stays the absolute source
 * of truth for the deformation. Returns false when the vertex count no longer matches the session
 * snapshot, i.e. topology changed and the session must be dropped.
 */
bool sculpt_lattice_sync_tracker_to_mesh(const Depsgraph &depsgraph,
                                         const Object &ob_mesh,
                                         LatticeToolData &state);

/**
 * Places the temp cage so its unit cell encloses \a bounds (given in object-space of \a ob_mesh)
 * plus \a margin, object-aligned with the mesh (ADR-4). Refreshes the cage's runtime matrix.
 */
void sculpt_lattice_fit_temp_to_bounds(Object &lat_ob,
                                       const Object &ob_mesh,
                                       const Bounds<float3> &bounds,
                                       float margin);

/* -------------------------------------------------------------------- */
/** \name Phase transitions (sculpt_lattice_place.cc)
 * \{ */

/**
 * Switches to #Phase::Deform, re-snapshotting the affected region against the current mesh
 * positions: they become the new rest. The cage is neutral at this point, so the snapshot cannot
 * double-apply the deformation accumulated before the last placement.
 *
 * Returns false and reports when the cage volume contains no deformable vertex; the caller must
 * then stay in #Phase::Placement. A failed call still clears #LatticeToolData::current, since the
 * rebuild runs unconditionally; this is harmless, as the next successful transition rebuilds it.
 */
bool sculpt_lattice_enter_deform(bContext *C, Object &ob_mesh, LatticeToolData &state);

/**
 * Switches to #Phase::Placement, baking the accumulated deformation: the positions this tool last
 * wrote become the new rest, and the cage control points go back to the neutral grid. The mesh does
 * not move, so the cage can then be placed freely without disturbing the result.
 */
void sculpt_lattice_enter_placement(Object &ob_mesh, LatticeToolData &state);

void SCULPT_OT_lattice_fit(wmOperatorType *ot);

void SCULPT_OT_lattice_box_define(wmOperatorType *ot);

/** \} */

/** \} */

}  // namespace blender::ed::sculpt_paint::lattice
