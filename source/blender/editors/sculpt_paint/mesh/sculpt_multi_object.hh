/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#pragma once

#include <optional>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_scene_enums.h"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/**
 * Build the symmetry-frame matrix S (world -> symmetry space) for a multi-object stroke. The brush
 * symmetry plane is the set of `symmetry_flip` axis planes expressed in this space:
 * - #PAINT_SYMM_SPACE_ACTIVE_OBJECT: reference object's local space (world_to_object). Historical.
 * - #PAINT_SYMM_SPACE_GLOBAL_WORLD: identity (world axes through the scene origin).
 * - #PAINT_SYMM_SPACE_GLOBAL_CURSOR: the 3D cursor's frame (its location AND orientation), so the
 *   mirror plane follows a rotated cursor - this is `invert(cursor_to_world)`.
 */
float4x4 symmetry_space_frame(ePaintSymmetrySpace symmetry_space,
                              const float4x4 &reference_world_to_object,
                              const float4x4 &cursor_to_world);

/**
 * One symmetry pass's brush daub, in world space.
 *
 * #view_direction is only meaningful for #PAINT_FALLOFF_SHAPE_TUBE (Projected), where the brush is
 * a cylinder along the view axis rather than a sphere. Mirroring a daub reflects that axis just as
 * it reflects the center, so the axis has to travel with the center rather than be recomputed from
 * the un-mirrored view.
 */
struct MirroredDaub {
  float3 center;
  float3 view_direction;
};

/**
 * Every symmetry pass (mirror reflections and radial copies) of the daub at \a world_center facing
 * \a world_view_direction, taken across \a reference_ob's symmetry planes, expressed in
 * \a symmetry_space. Used so a multi-object shared-symmetry stroke also processes objects whose
 * geometry only lies under a mirrored daub -- a joined mesh would deform that geometry, so the
 * separate object must not be skipped -- and so paint-cursor overlays can preview the same
 * mirrored positions the stroke itself will use. The first entry is the un-mirrored daub itself.
 */
Vector<MirroredDaub> shared_symmetry_world_daubs(const Object &reference_ob,
                                                 const float3 &world_center,
                                                 const float3 &world_view_direction,
                                                 ePaintSymmetrySpace symmetry_space,
                                                 const float4x4 &cursor_to_world);

/**
 * Multi-object mirror surface snap: pull the mirrored (or radially rotated) brush daub center onto
 * the surface of a secondary object.
 *
 * #StrokeCache.location_symm is the geometric reflection of the primary object's hit point. When
 * the secondary mesh is not an exact mirror twin of the primary, that reflection floats off its
 * surface; with #PAINT_FALLOFF_SHAPE_SPHERE the brush measures a true 3D distance, so no vertex
 * falls inside the radius and the mirror pass silently does nothing. Ray-casting the surface along
 * the MIRRORED VIEW AXIS (#StrokeCache.view_normal_symm) corrects the depth while leaving the
 * daub's position across the view exactly where the mirror put it -- the mirrored daub lands where
 * a mirrored cursor would have hit this mesh, which is how the primary object gets its own center.
 * The snap axis is therefore fixed for the pass and independent of the geometry the brush is
 * deforming, which is what keeps the mirrored stroke a continuous line.
 *
 * No-op unless the stroke is multi-object, \a ob is not the primary object, the current pass is a
 * mirror or radial pass, the brush falloff is Sphere, and the feature is enabled on \a paint. Must
 * be called after #cache_calc_brushdata_symm and BEFORE #do_tiled, which takes the (possibly
 * snapped) #StrokeCache.location_symm as the origin it offsets each tile from.
 */
void mirror_snap_location_to_surface(const Depsgraph &depsgraph,
                                     const Paint &paint,
                                     const Brush &brush,
                                     Object &ob,
                                     StrokeCache &cache);

/**
 * Multiplier applied to the brush radius when testing whether a secondary object lies under a
 * MIRRORED daub (see #MultiObjectStrokeContext::process_secondary). It matches the distance
 * #mirror_snap_location_to_surface is allowed to travel, so an object whose surface the snap could
 * still reach is not rejected by the object gate first. Returns 1.0 when the snap is inactive,
 * keeping the gate byte-identical in that case.
 */
float mirror_snap_search_multiplier(const Paint &paint, const Brush &brush);

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
 *       secondary's own curvature check).
 */
struct SharedStrokeStateSnapshot {
  /* `std::nullopt` => primary had not yet set a seed at capture time (kept as-is; the secondary
   * would then lazy-allocate one, which is acceptable as every secondary needs SOME seed). */
  std::optional<float> density_seed;
  /* `face_set_none_id` => primary had not yet allocated a paint face-set id at capture time.
   * The secondary will allocate one itself; this case should not normally happen during the
   * first stroke step if the brush is engaged, but is allowed for completeness. */
  int painted_face_set_id = face_set_none_id;
  int painted_face_set_secondary_id = face_set_none_id;
  /**
   * Custom overlay colors registered for the two IDs above in the PRIMARY object's mesh.
   *
   * The IDs are shared across the whole stroke, but #Mesh.face_set_colors is per-mesh: a secondary
   * that only receives the ID writes faces for a Face Set its own table knows nothing about, and
   * the overlay falls back to the pseudo-random hue derived from the ID. That is the "same brush
   * color on the object under the cursor, similar-but-wrong color on the other one" symptom in
   * Custom mode. Carrying the color lets #propagate_shared_stroke_state install it on every
   * secondary mesh before that mesh's brush action writes any face, mirroring what
   * #FaceSetColorStrokeCache::ensure_face_set_id_for_quant_color already does for the
   * Color-from-Texture path.
   *
   * `std::nullopt` => the ID has no custom color on the primary (Random mode, or an ID sampled
   * from geometry that never had one); the secondary's table is then left untouched.
   */
  std::optional<float3> painted_face_set_color;
  std::optional<float3> painted_face_set_secondary_color;
  /**
   * The primary object's #StrokeCache.sculpt_normal for this step, in WORLD space.
   *
   * #update_sculpt_normal only computes #StrokeCache.sculpt_normal on the MAIN symmetry pass, and
   * that pass can produce nothing for a secondary object: the main daub may miss it entirely (the
   * whole point of shared symmetry, e.g. a mirrored limb kept as a separate mesh) or cover only
   * masked/hidden geometry. Its mirror pass then mirrors the zero-initialized vector, and the
   * brush runs over the right nodes with the right strength while displacing every vertex by
   * nothing. Seeding the primary's normal is also what a joined mesh does: one area normal, taken
   * under the main daub, mirrored onto the other side. When the secondary's own main pass DOES
   * produce a normal it overwrites this seed -- and for a Mesh PBVH the two are the same vector
   * anyway, since #calc_area_normal pools across every mesh in the stroke.
   *
   * `std::nullopt` => the primary had no normal at capture time (its own brush action bailed, or
   * the brush does not use one); the secondary is then left untouched.
   */
  std::optional<float3> sculpt_normal_world;
};

SharedStrokeStateSnapshot capture_shared_stroke_state(const Object &primary_ob);

void propagate_shared_stroke_state(Object &secondary_ob,
                                   const SharedStrokeStateSnapshot &primary_state);

/**
 * Multi-object ("global") sculpt stroke state that has exactly one value per STROKE, not one per
 * object. Bundled into its own type (previously loose fields directly on #SculptPaintStroke) so
 * this state has one discoverable home, and the points in the multi-object stroke lifecycle that
 * touch it -- resolving the primary object, propagating shared state to every object's cache, and
 * deciding whether/where a secondary object is touched this step -- are named methods instead of
 * inline blocks in #SculptPaintStroke::update_step. Pure reorganization: every field keeps its
 * previous semantics and set-once-per-step call site unchanged, just renamed/regrouped -- no
 * behavior change (see this file's revision history for the anisotropic-scale / rake-mirroring /
 * shared-symmetry rounds this code has been through; this refactor deliberately does not touch
 * any of that math).
 *
 * \note #propagate_shared_state only covers the fields written by
 *       #propagate_shared_sampling_and_symmetry_state, which are pure functions of #mode_objects
 *       / #primary_object / #symm_reference_object and so can be written in any order across
 *       #mode_objects. #SharedStrokeStateSnapshot is NOT covered here: it is captured from the
 *       primary's #StrokeCache only after the primary's brush action has run (the value is lazily
 *       brush-allocated), so it cannot be resolved up front alongside this struct's own fields --
 *       it stays a separate step inside #update_step's Phase 2 loop, gated on the primary having
 *       been swapped to the front of the per-step object list (unrelated to this struct).
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
   * symmetry plane so it does not follow the cursor between meshes. For a single object this is
   * that object itself. Null only when no object is in the mode. */
  Object *symm_reference_object = nullptr;
  /* True when the shared symmetry frame is engaged: every multi-object stroke, and also a single
   * object in World / Cursor space (where the mirror plane is not the object's own local plane).
   * False for a single object in ACTIVE_OBJECT space, keeping that path a bit-exact local mirror.
   */
  bool shared_symmetry_active = false;

  /* Shared world-space brush state, recomputed every #update_step from the primary object in
   * #brush_delta_update.
   *
   * Anchored-origin brushes (Grab, Pose, Boundary, Thumb, Elastic Deform, Cloth-grab) lock a
   * single world-space anchor at stroke start and accumulate a single world-space delta. The
   * primary (under-cursor) object computes these in #brush_delta_update; secondary objects then
   * derive their own local grab state from them, so the whole mode deforms like one joined mesh
   * instead of each object recomputing an inconsistent per-object delta. */
  bool world_grab_state_valid = false;
  float3 world_grab_anchor = float3(0.0f);
  float3 world_grab_delta = float3(0.0f);
  /* World-space rake rotation of the primary object for the current step; mirrored onto secondary
   * objects in #brush_delta_update. Unset when the brush has no rake or none was computed yet. */
  std::optional<math::Quaternion> world_rake_rotation;

  /* Reference object for anchored-origin drag brushes (Grab, Pose, Boundary, Thumb, Elastic
   * Deform, Cloth-grab). These brushes accumulate the grab delta on a single object across the
   * whole stroke, so the primary must stay fixed even when the paint-stroke framework switches
   * #SculptPaintStroke::object to a different mesh under the cursor mid-drag. Captured on the
   * first #update_step; nullptr means not yet captured or the brush is not anchored-origin. */
  Object *anchored_primary_object = nullptr;

  /**
   * #update_step Phase 1: resolve and record the primary object for this step. `cursor_object` is
   * #SculptPaintStroke::object (the object the paint-stroke framework has locked onto for tracking
   * brushes). Returns the resolved primary object (also stored in #primary_object).
   */
  Object *resolve_primary(Object *cursor_object, const Brush &brush);

  /**
   * #update_step Phase 1/2 boundary: resolve #symm_reference_object / #shared_symmetry_active for
   * this step and publish the shared multi-object surface-sampling context and shared symmetry
   * reference-space transforms onto every object's #StrokeCache. Must be called after
   * #resolve_primary.
   */
  void propagate_shared_state(ePaintSymmetrySpace symmetry_space, const float4x4 &cursor_to_world);

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
   * this step, and `primary_world_view_direction` its brush volume's axis (only used by Projected
   * falloff). `symm_daubs` is the set of mirrored daubs to also test against when
   * #shared_symmetry_active (empty otherwise); its first entry is the un-mirrored daub.
   */
  bool process_secondary(Object &ob,
                         StrokeCache &cache,
                         Paint &paint,
                         const Brush &brush,
                         const float3 &primary_world_center,
                         const float3 &primary_world_view_direction,
                         Span<MirroredDaub> symm_daubs) const;
};

}  // namespace blender::ed::sculpt_paint
