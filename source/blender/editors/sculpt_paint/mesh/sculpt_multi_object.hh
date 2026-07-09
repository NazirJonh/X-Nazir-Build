/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#pragma once

#include <optional>

#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/**
 * Snapshot of the primary object's per-stroke fields that are LAZILY allocated inside the brush
 * action (density_seed in #do_paint_brush_action / #do_blur_brush_action, painted_face_set_id in
 * #do_draw_face_sets_brush) and would otherwise be re-allocated per secondary, producing a
 * divergent value between meshes for what should be one joined stroke.
 *
 * Captured immediately after the primary object's brush action runs and mirrored onto every
 * secondary BEFORE its brush action via #propagate_shared_stroke_state. After this propagation
 * the secondaries' brush actions skip their own lazy allocation.
 *
 * \note This is the only remaining "shared per-stroke state" propagation split between primary
 *       and secondary (needs the primary processed first, see #update_step's swap). The other
 *       per-object-cache shared fields are pure-copy from the framework every step, order
 *       independent -- see #MultiObjectStrokeContext::propagate_shared_state. Two fields are
 *       deliberately NOT unified here: `toggle_settings` (stroke-init only, captured once in
 *       #stroke_cache_init) and #area_texture_frame_to_world / #area_texture_frame_valid (in
 *       #calc_brush_area_texture_mat, kept lazy because the right value depends on the
 *       secondary's own curvature check). See
 *       `.MyTaskAndDoc/.../Refactoring_2/Architecture_Refactoring_Analysis.md` 3.1.
 */
struct SharedStrokeStateSnapshot {
  /* `std::nullopt` => primary had not yet set a seed at capture time (kept as-is; the secondary
   * would then lazy-allocate one, which is acceptable as every secondary needs SOME seed). */
  std::optional<float> density_seed;
  /* `face_set_none_id` => primary had not yet allocated a paint face-set id at capture time.
   * The secondary will allocate one itself; this case should not normally happen during the
   * first stroke step if the brush is engaged, but is allowed for completeness. */
  int painted_face_set_id = face_set_none_id;
};

SharedStrokeStateSnapshot capture_shared_stroke_state(const Object &primary_ob);

void propagate_shared_stroke_state(Object &secondary_ob,
                                   const SharedStrokeStateSnapshot &primary_state);

/**
 * Multi-object ("global") sculpt stroke state that has exactly one value per STROKE, not one per
 * object -- "Container A" of the shared-per-stroke state model
 * (Architecture_Refactoring_Analysis.md 3.1). Bundled into its own type (previously loose fields
 * directly on #SculptPaintStroke) so this state has one discoverable home, and the points in the
 * multi-object stroke lifecycle that touch it -- resolving the primary object, propagating shared
 * state to every object's cache, and deciding whether/where a secondary object is touched this
 * step -- are named methods instead of inline blocks in #SculptPaintStroke::update_step. Pure
 * reorganization: every field keeps its previous semantics and set-once-per-step call site
 * unchanged, just renamed/regrouped -- no behavior change (see this file's revision history for
 * the anisotropic-scale / rake-mirroring / shared-symmetry rounds this code has been through;
 * this refactor deliberately does not touch any of that math).
 *
 * \note #propagate_shared_state only covers Container B
 *       (#propagate_shared_sampling_and_symmetry_state, order-independent across #mode_objects).
 *       Container C (#SharedStrokeStateSnapshot) is NOT covered here: it is captured from the
 *       primary's #StrokeCache only after the primary's brush action has run (the value is lazily
 *       brush-allocated), so it cannot be resolved up front alongside Containers A/B -- it stays a
 *       separate step inside #update_step's Phase 2 loop, gated on the primary having been swapped
 *       to the front of the per-step object list (unrelated to this struct).
 */
struct MultiObjectStrokeContext {
  /* Objects in sculpt mode for the duration of this stroke. The membership is stable while a
   * stroke is modal, so it is captured once in #SculptPaintStroke::test_start instead of
   * re-querying the view layer (which allocates a vector and iterates every object base) on every
   * event. */
  Vector<Object *> mode_objects;

  /* -- Recomputed every #update_step, by #resolve_primary / #propagate_shared_state below -- */

  /* The object under the cursor for tracking brushes, or the pinned #anchored_primary_object for
   * anchored-origin drag brushes. See #resolve_primary. */
  Object *primary_object = nullptr;
  /* The object a #Join would merge everything into -- #mode_objects[0], i.e. the ACTIVE object,
   * NOT #primary_object (the object under the cursor) -- fixed reference frame for the shared
   * symmetry plane so it does not follow the cursor between meshes. Null for single-object
   * strokes. */
  Object *symm_reference_object = nullptr;
  /* True only for multi-object strokes with a resolved #symm_reference_object. */
  bool shared_symmetry_active = false;

  /* Shared world-space brush state, recomputed every #update_step from the primary object in
   * #brush_delta_update.
   *
   * Anchored-origin brushes (Grab, Pose, Boundary, Thumb, Elastic Deform, Cloth-grab) lock a single
   * world-space anchor at stroke start and accumulate a single world-space delta. The primary
   * (under-cursor) object computes these in #brush_delta_update; secondary objects then derive their
   * own local grab state from them, so the whole mode deforms like one joined mesh instead of each
   * object recomputing an inconsistent per-object delta. */
  bool world_grab_state_valid = false;
  float3 world_grab_anchor = float3(0.0f);
  float3 world_grab_delta = float3(0.0f);
  /* World-space rake rotation of the primary object for the current step; mirrored onto secondary
   * objects in #brush_delta_update. Unset when the brush has no rake or none was computed yet. */
  std::optional<math::Quaternion> world_rake_rotation;

  /* Reference object for anchored-origin drag brushes (Grab, Pose, Boundary, Thumb, Elastic Deform,
   * Cloth-grab). These brushes accumulate the grab delta on a single object across the whole stroke,
   * so the primary must stay fixed even when the paint-stroke framework switches
   * #SculptPaintStroke::object to a different mesh under the cursor mid-drag. Captured on the first
   * #update_step; nullptr means not yet captured or the brush is not anchored-origin. */
  Object *anchored_primary_object = nullptr;

  /**
   * #update_step Phase 1: resolve and record the primary object for this step. `cursor_object` is
   * #SculptPaintStroke::object (the object the paint-stroke framework has locked onto for tracking
   * brushes). Returns the resolved primary object (also stored in #primary_object).
   */
  Object *resolve_primary(Object *cursor_object, const Brush &brush);

  /**
   * #update_step Phase 1/2 boundary: resolve #symm_reference_object / #shared_symmetry_active for
   * this step and publish Container B (shared multi-object surface-sampling context + shared
   * symmetry reference-space transforms) onto every object's #StrokeCache. Must be called after
   * #resolve_primary.
   */
  void propagate_shared_state();

  /**
   * #update_step Phase 2, secondary-object branch: resolve whether/where `ob` (a non-primary
   * object) is touched this step from the shared world-space brush center(s), setting up its
   * #StrokeCache location. Returns true if `ob` has a valid location this step (should proceed to
   * #do_symmetrical_brush_actions).
   *
   * Caller must first check `primary_world_center_valid` (whether the primary has established a
   * center yet this step) before calling this -- kept as a separate check at the call site rather
   * than a parameter here so the `continue` (with its RAII-guard-restore semantics) stays visible
   * in #update_step's loop. `primary_world_center` is the primary's world-space brush center for
   * this step. `symm_world_centers` is the set of mirrored daub centers to also test against when
   * #shared_symmetry_active (empty otherwise).
   */
  bool process_secondary(Object &ob,
                         StrokeCache &cache,
                         Paint &paint,
                         const Brush &brush,
                         const float3 &primary_world_center,
                         Span<float3> symm_world_centers) const;
};

}  // namespace blender::ed::sculpt_paint
