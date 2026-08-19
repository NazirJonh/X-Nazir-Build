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
#include "BLI_set.hh"
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
/**
 * Above this per-axis resolution the overlay draws only the cage shell, not every interior
 * control point / edge. Full topology at 64³ is hundreds of thousands of edges per redraw.
 */
constexpr int SCULPT_LATTICE_OVERLAY_FULL_RES_MAX = 12;

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
 * One PBVH leaf that contains vertices inside the cage. Unique node verts only, so a shared
 * vertex is deformed once. Parallel arrays are the rest / current / mask snapshot for this node.
 */
struct AffectedNode {
  int index = -1;
  Vector<int> verts;
  Array<float3> rest_coords;
  Array<float3> current_coords;
  Array<float> mask;
};

/**
 * Vertices affected by the current deform phase, grouped by PBVH leaf.
 * Built at session start and at each #Phase::Placement -> #Phase::Deform transition (see
 * #sculpt_lattice_build_affected_region), never rebuilt between drags within one deform phase.
 */
struct AffectedRegion {
  Vector<AffectedNode> nodes;
  /** Sorted leaf indices, parallel to #nodes, for #IndexMask::from_indices. */
  Vector<int> node_indices;
  /** Identity of the tree #nodes was built against, or null. */
  const void *pbvh = nullptr;
  int pbvh_nodes_num = -1;

  bool is_empty() const
  {
    return node_indices.is_empty();
  }
};

/**
 * Positions of every vertex this tool has ever written, captured the first time the vertex
 * entered an affected region. Session Cancel restores these instead of a full-mesh snapshot.
 */
struct SessionOrigSnapshot {
  Set<int> vert_set;
  Vector<int> verts;
  Vector<float3> positions;
  Set<int> node_set;
  Vector<int> node_indices;
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

  /** Affected PBVH nodes / rest / mask for the current deform phase. */
  AffectedRegion current;

  /** First-seen positions of verts this tool has written, for session Cancel. */
  SessionOrigSnapshot session_orig;

  /** True once this session has written a non-zero deformation to the mesh. */
  bool session_has_mesh_changes = false;

  /** Vertex count at last region build; mismatch means topology changed. */
  int mesh_verts_num = -1;

  /**
   * Cached overlay topology, rebuilt when resolution or shell mode changes.
   */
  Vector<int> overlay_point_indices;
  Vector<int2> overlay_edges;
  int3 overlay_edges_res = int3(0);
  bool overlay_edges_shell = false;

  /**
   * BKE lattice deform context. Rebuilt when the cage transform or resolution changes;
   * during a control-point slide only the moved point is updated in place.
   */
  LatticeDeformData *deform_data = nullptr;

  /** Scratch buffer for #sculpt_lattice_deform_apply, reused across MOUSEMOVE. */
  Array<float3> translations;

  /** Index of the BPoint currently being dragged (screen-space pick result). */
  int pending_drag_index = -1;

  /** True after this drag has opened a position undo step for a real mesh change. */
  bool drag_undo_started = false;

  /** #session_has_mesh_changes at the start of the current drag, used when cancelling it. */
  bool drag_started_with_session_changes = false;

  /**
   * Cage-space position of #pending_drag_index when the drag started, so cancelling a drag can
   * put the control point back. Without it the cage would keep the cancelled displacement and
   * silently re-apply it on the next drag.
   */
  float3 drag_start_point = float3(0.0f);

  /**
   * Cage loc/quat/scale at the start of a Placement G/R/S. Written by
   * #placement_transform_undo_store, consumed on confirm, cleared on Esc.
   */
  float3 placement_xform_orig_loc = float3(0.0f);
  float placement_xform_orig_quat[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float3 placement_xform_orig_scale = float3(1.0f);
  bool placement_xform_orig_valid = false;

  /* NOTE: whether a modal slide is running is *not* tracked here. It has to be visible to
   * #BKE_sculpt_update_object_before_eval, which cannot see this type (ADR-13), so
   * #SculptSession::pbvh_hold is the single source of truth and this struct would only
   * duplicate it. */
};

/**
 * Lazy-initializes the tool session on first interaction (ADR-12):
 *  - computes unmasked bbox (variant A)
 *  - creates temp OB_LATTICE fitted to it
 *  - snapshots first-touched vertex positions for Cancel
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
 * Editor registration: installs the sculpt-session free hook so the tool state is
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
 * Strength 0 (and fully masked verts) still produce a target of \a rest_coords, so a live
 * strength change can roll the mesh back rather than leaving the last deformed position stuck.
 *
 * \param deform_data: cage context from #sculpt_lattice_deform_data_rebuild.
 * \param rest_coords: original (undeformed) positions of the affected verts.
 * \param mask: per-vert mask snapshot, parallel to \a rest_coords.
 * \param current_coords: positions this tool last wrote; advanced to the new targets.
 * \param r_translations: per-vert deltas to hand to #PositionDeformData::deform.
 * \return false when every translation is zero — callers should skip mesh writes.
 */
bool sculpt_lattice_compute_translations(LatticeDeformData &deform_data,
                                         Span<float3> rest_coords,
                                         Span<float> mask,
                                         float strength,
                                         MutableSpan<float3> current_coords,
                                         MutableSpan<float3> r_translations);

/**
 * Returns true when applying the current cage would move any affected vertex, without changing
 * the mesh or the incremental position tracker.
 */
bool sculpt_lattice_deform_would_change(LatticeToolData &state);

/**
 * Applies the lattice deformation live (MOUSEMOVE). Writes through PositionDeformData
 * and tags affected PBVH nodes. Returns false when nothing moved, so callers can skip
 * PBVH tagging, bounds updates and viewport flush.
 */
bool sculpt_lattice_deform_apply(const Depsgraph &depsgraph,
                                 Object &ob_mesh,
                                 LatticeToolData &state);

/**
 * Tags the PBVH nodes in \a ar as having changed positions and refreshes their bounds.
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
