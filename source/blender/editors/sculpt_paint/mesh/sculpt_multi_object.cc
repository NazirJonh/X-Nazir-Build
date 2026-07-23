/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#include "sculpt_multi_object.hh"

#include <cmath>

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"

#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "../paint_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

float4x4 symmetry_space_frame(const ePaintSymmetrySpace symmetry_space,
                              const float4x4 &reference_world_to_object,
                              const float4x4 &cursor_to_world)
{
  float4x4 result;
  switch (symmetry_space) {
    case PAINT_SYMM_SPACE_ACTIVE_OBJECT:
      /* Mirror in the reference object's own local axes (historical behavior). */
      result = reference_world_to_object;
      break;
    case PAINT_SYMM_SPACE_GLOBAL_WORLD:
      result = float4x4::identity();
      break;
    case PAINT_SYMM_SPACE_GLOBAL_CURSOR:
      /* World -> cursor space, so the mirror plane inherits the cursor's full placement (location
       * AND orientation). A translation-only cursor reduces this to `from_location(-location)`. */
      result = math::invert(cursor_to_world);
      break;
    default:
      BLI_assert_unreachable();
      result = float4x4::identity();
      break;
  }
  return result;
}

Vector<MirroredDaub> shared_symmetry_world_daubs(const Object &reference_ob,
                                                 const float3 &world_center,
                                                 const float3 &world_view_direction,
                                                 const ePaintSymmetrySpace symmetry_space,
                                                 const float4x4 &cursor_to_world)
{
  const Mesh &mesh = *id_cast<const Mesh *>(reference_ob.data);
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(reference_ob);

  /* World <-> symmetry-space frame, matching #propagate_shared_sampling_and_symmetry_state: the
   * mirrored daub centers used to decide whether a secondary object is touched must live in the
   * SAME space as the deformation's own mirror plane, or global modes would test against the wrong
   * positions and silently skip secondary objects. ACTIVE_OBJECT keeps the verbatim reference
   * expressions (do NOT route through #symmetry_space_frame/invert) so it stays bit-exact. */
  float4x4 to_symm_space = float4x4::identity();
  float4x4 from_symm_space = float4x4::identity();
  if (symmetry_space == PAINT_SYMM_SPACE_ACTIVE_OBJECT) {
    to_symm_space = reference_ob.world_to_object();
    from_symm_space = reference_ob.object_to_world();
  }
  else {
    to_symm_space = symmetry_space_frame(
        symmetry_space, reference_ob.world_to_object(), cursor_to_world);
    from_symm_space = math::invert(to_symm_space);
  }
  const float3 center_ref = math::transform_point(to_symm_space, world_center);
  /* The view axis travels with the center: a mirrored daub's brush volume is the mirror image of
   * the original, axis included. Carried as a DIRECTION (matching how #view_normal is transformed
   * everywhere else in this module -- see #multi_object_area_sample_active), so the symmetry
   * frame's translation is ignored and any scale it carries cancels across the round trip. */
  const float3 view_ref = math::transform_direction(to_symm_space, world_view_direction);

  Vector<MirroredDaub> daubs;
  for (int i = 0; i <= symm; i++) {
    if (!is_symmetry_iteration_valid(i, symm)) {
      continue;
    }
    const ePaintSymmetryFlags pass = ePaintSymmetryFlags(i);
    const float3 mirrored = symmetry_flip(center_ref, pass);
    const float3 mirrored_view = symmetry_flip(view_ref, pass);
    /* Shared by this pass's daub and all of its radial copies: the CENTER is rotated by the radial
     * matrix below, the VIEW AXIS is not. #cache_calc_brushdata_symm mirrors
     * #StrokeCache.view_normal with `radial_rotate = false`, so #pbvh_gather_generic builds the
     * Projected-falloff cylinder from an unrotated axis. This gate has to test the same volume the
     * brush will, so it must reproduce that asymmetry rather than "fix" it. */
    const float3 mirrored_view_world = math::normalize(
        math::transform_direction(from_symm_space, mirrored_view));
    daubs.append({math::transform_point(from_symm_space, mirrored), mirrored_view_world});

    /* Radial copies around each axis, matching #do_radial_symmetry (flip, then rotate). */
    for (int axis = 0; axis < 3; axis++) {
      const int count = mesh.radial_symmetry[axis];
      for (int r = 1; r < count; r++) {
        const float angle = 2.0f * M_PI * r / count;
        float mat[3][3];
        axis_angle_to_mat3_single(mat, 'X' + axis, angle);
        float3 rotated = mirrored;
        mul_m3_v3(mat, rotated);
        daubs.append({math::transform_point(from_symm_space, rotated), mirrored_view_world});
      }
    }
  }

  return daubs;
}

static bool mirror_snap_enabled(const Paint &paint, const Brush &brush)
{
  if ((paint.symmetry_flags & PAINT_SYMMETRY_MIRROR_SNAP_OFF) != 0) {
    return false;
  }
  /* Projected/Tube falloff measures distance to the view ray, so depth already does not matter and
   * the daub center doubles as the cylinder axis origin -- moving it would slide the projected
   * disc sideways instead of correcting depth. */
  return eBrushFalloffShape(brush.falloff_shape) == PAINT_FALLOFF_SHAPE_SPHERE;
}

float mirror_snap_search_multiplier(const Paint &paint, const Brush &brush)
{
  return mirror_snap_enabled(paint, brush) ? BKE_paint_mirror_snap_distance_get(paint) : 1.0f;
}

void mirror_snap_location_to_surface(const Depsgraph &depsgraph,
                                     const Paint &paint,
                                     const Brush &brush,
                                     Object &ob,
                                     StrokeCache &cache)
{
  if (!cache.multi_object_stroke) {
    return;
  }
  /* The primary object is the one the cursor raycast hit, so its main daub is already on its own
   * surface; its mirror pass is the historical single-object behavior and stays untouched. */
  if (&ob == cache.multi_object_sample_reference) {
    return;
  }
  if (cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0) {
    return;
  }
  if (!mirror_snap_enabled(paint, brush)) {
    return;
  }

  /* How far the daub may travel along the mirrored view axis to reach the surface. */
  const float max_distance = cache.radius * BKE_paint_mirror_snap_distance_get(paint);

  /* Snap ALONG THE MIRRORED VIEW AXIS, by ray-casting the surface. This reproduces in the mirrored
   * frame exactly what gives the PRIMARY object its own daub center -- the cursor ray's hit on the
   * surface -- so the mirrored daub lands where a mirrored cursor would have hit this mesh, and
   * only its depth is corrected: its position across the view (the mirror's own lateral answer) is
   * preserved exactly. #StrokeCache.view_normal_symm is the mirrored viewer's direction,
   * recomputed for this pass by #cache_calc_brushdata_symm immediately before this call; it is
   * fixed for the whole pass and does not depend on the surface at all.
   *
   * That independence is the point. Projecting the center onto the tangent plane of the nearest
   * surface VERTEX (what this used to do) makes the snap direction a function of the geometry the
   * brush is itself deforming: the center sits up to \a max_distance off the surface, so the
   * projection amplifies any change in that vertex's normal by that whole distance, and the normal
   * both jumps (the nearest vertex switches from step to step) and swings (the brush is tilting it
   * as it sculpts). The daub center then lurches by a good fraction of a brush radius between
   * steps
   * -- and the depth/lateral budget tests flicker in and out with it -- so the mirrored stroke
   * comes out as a chain of separate craters instead of a line. A ray along a fixed axis has no
   * such feedback loop, and being a true ray-surface intersection it is not quantized to vertex
   * spacing either.
   *
   * The cast uses the ORIGINAL (stroke-start) surface, not the current (deformed) one -- see
   * #raycast_front_facing_surface_offset. Casting against the current surface would close a
   * feedback loop for any accumulating brush (one that does not restore from undo each step:
   * Draw, Clay, Layer, Snake Hook, ...) whenever the brush's own displacement direction is close
   * to the snap's search axis: each step's snap would land slightly further out along the axis as
   * the brush pushes the surface, compounding into a growing offset and a visible tear/seam over
   * the course of the stroke. Grab, Rotate, Thumb and Elastic Deform are unaffected either way:
   * #restore_from_undo_step_if_necessary has already restored them to stroke-start by the time
   * this runs, so their current geometry already IS the original geometry. */
  const std::optional<float> offset = raycast_front_facing_surface_offset(
      depsgraph, ob, cache.location_symm, cache.view_normal_symm, max_distance);
  if (!offset) {
    /* The mirrored cursor ray misses this mesh (the mirror landed past its silhouette) or its
     * surface is out of the budgeted reach. Leaving the daub where the mirror put it is the right
     * answer -- it then finds no nodes there and the object is left alone. */
    return;
  }

  const float3 delta = cache.view_normal_symm * *offset;

  /* #last_location_symm is only ever read as `location_symm - last_location_symm` (stroke
   * direction for Cloth, Smear, Topology Slide, color paint). Shifting it by the SAME delta leaves
   * that difference bit-identical, so the snap changes where the daub sits without changing which
   * way the stroke is going.
   *
   * #initial_location_symm is deliberately NOT shifted: it is a stroke-start anchor, and the snap
   * delta is recomputed every step, so shifting it would make the anchor drift. Its only readers
   * (Boundary, Cloth) belong to the physics family that multi-object sculpt does not support yet
   * anyway -- see #sculpt_multi_object_disable_cloth_family_brushes. */
  cache.location_symm += delta;
  cache.last_location_symm += delta;
}

/**
 * Publishes the shared multi-object surface-sampling context and the shared-symmetry
 * reference-space transforms onto every object's #StrokeCache for this stroke step. Unlike
 * #SharedStrokeStateSnapshot, every field written here is a pure function of
 * `mode_objects`/`sample_objects`/`primary_ob`/`symm_reference_ob` -- none of it is read back from
 * a lazily-populated cache, so the write order across `mode_objects` does not matter (this is
 * called once per #update_step, before the primary-first loop below it that
 * #SharedStrokeStateSnapshot's ordering invariant applies to).
 *
 * Pure extraction of the previous inline loop -- no behavior change.
 */
static void propagate_shared_sampling_and_symmetry_state(const Span<Object *> mode_objects,
                                                         const Span<Object *> sample_objects,
                                                         Object *primary_ob,
                                                         Object *symm_reference_ob,
                                                         const bool shared_symmetry_active,
                                                         const ePaintSymmetrySpace symmetry_space,
                                                         const float4x4 &cursor_to_world)
{
  /* Symmetry frame for the whole stroke. In ACTIVE_OBJECT mode we keep the historical per-object
   * expressions verbatim (see below) and never touch S, so the default path stays byte-identical.
   */
  const bool global_symmetry = shared_symmetry_active &&
                               symmetry_space != PAINT_SYMM_SPACE_ACTIVE_OBJECT;
  float4x4 S = float4x4::identity();
  float4x4 S_inv = float4x4::identity();
  if (global_symmetry) {
    S = symmetry_space_frame(
        symmetry_space, symm_reference_ob->world_to_object(), cursor_to_world);
    S_inv = math::invert(S);
  }

  for (Object *object_ptr : mode_objects) {
    SculptSession *ss_iter = object_ptr->runtime->sculpt_session;
    if (!ss_iter || !ss_iter->cache) {
      continue;
    }

    /* Shared multi-object surface-sampling context, so the area/plane sampling helpers
     * (#calc_area_normal, #calc_area_center, #calc_area_normal_and_center) can pool vertices
     * across all meshes in the reference object's space (joined-mesh parity). Disabled (empty
     * span) for single-object strokes. */
    ss_iter->cache->multi_object_sample_objects = sample_objects;
    ss_iter->cache->multi_object_sample_reference = sample_objects.is_empty() ? nullptr :
                                                                                primary_ob;

    /* 3D brush textures must be sampled in one shared space (the primary object's local space) to
     * match a joined mesh. Identity keeps the single-object path bit-exact. */
    if (sample_objects.is_empty() || object_ptr == primary_ob) {
      ss_iter->cache->texture_sample_from_object = float4x4::identity();
    }
    else {
      ss_iter->cache->texture_sample_from_object = primary_ob->world_to_object() *
                                                   object_ptr->object_to_world();
    }

    /* The reference (active) object defines the shared symmetry plane (position, orientation) and
     * supplies the symmetry AXES for every mesh in a multi-object stroke, so all meshes mirror
     * consistently across the same world-space plane even when they have different, possibly
     * unapplied, transforms and even when a non-active mesh has no #Mesh.symmetry of its own (the
     * header X/Y/Z toggles only set it on the active mesh). Null for single-object strokes, where
     * symmetry collapses to this object's own local plane, staying bit-exact. */
    /* Non-null whenever the shared symmetry frame is engaged (see #propagate_shared_state): for
     * multi-object strokes, and for a single object in World / Cursor space.
     * #do_symmetrical_brush_ actions reads it to take the symmetry axes from the reference mesh
     * and to enable the shared- origin mirror. For a single object the reference IS this object,
     * so the axes are its own. */
    ss_iter->cache->symm_reference_object = shared_symmetry_active ? symm_reference_ob : nullptr;
    /* Reference-space transforms for the shared symmetry plane (see #StrokeCache). */
    if (!shared_symmetry_active) {
      /* Single-object stroke: untouched, bit-exact. */
      ss_iter->cache->symm_ref_from_cur = float4x4::identity();
      ss_iter->cache->symm_cur_from_ref = float4x4::identity();
      ss_iter->cache->symm_shared_origin_active = false;
    }
    else if (global_symmetry) {
      /* World-axis symmetry: EVERY object (including the reference) is carried into the shared
       * world frame S, mirrored there, and carried back. */
      ss_iter->cache->symm_ref_from_cur = S * object_ptr->object_to_world();
      ss_iter->cache->symm_cur_from_ref = object_ptr->world_to_object() * S_inv;
      ss_iter->cache->symm_shared_origin_active = true;
    }
    else if (object_ptr == symm_reference_ob) {
      /* ACTIVE_OBJECT mode, reference object: identity -> traditional per-object mirror,
       * bit-exact. */
      ss_iter->cache->symm_ref_from_cur = float4x4::identity();
      ss_iter->cache->symm_cur_from_ref = float4x4::identity();
      ss_iter->cache->symm_shared_origin_active = false;
    }
    else {
      /* ACTIVE_OBJECT mode, secondary object: verbatim historical expressions (do NOT use
       * S/invert, so this stays byte-identical to the pre-feature code). */
      ss_iter->cache->symm_ref_from_cur = symm_reference_ob->world_to_object() *
                                          object_ptr->object_to_world();
      ss_iter->cache->symm_cur_from_ref = object_ptr->world_to_object() *
                                          symm_reference_ob->object_to_world();
      ss_iter->cache->symm_shared_origin_active = true;
    }
  }
}

SharedStrokeStateSnapshot capture_shared_stroke_state(const Object &primary_ob)
{
  SharedStrokeStateSnapshot result;
  const SculptSession *ss = primary_ob.runtime->sculpt_session;
  if (ss && ss->cache) {
    result.density_seed = ss->cache->paint_brush.density_seed;
    result.painted_face_set_id = ss->cache->paint_face_set;

    /* #StrokeCache.sculpt_normal is written only by the MAIN symmetry pass, and the mirror passes
     * leave it alone, so it still holds the main-pass value here (after all of the primary's
     * passes have run). Carry it in world space: the secondaries it is handed to have their own,
     * possibly unapplied, transforms. Normals transform by the inverse transpose. Only ever read
     * by a secondary object, so skip the matrix work entirely for single-object strokes. */
    const float3 &normal_local = ss->cache->sculpt_normal;
    if (ss->cache->multi_object_stroke && !math::is_zero(normal_local)) {
      const float3x3 local_to_world_normal = math::transpose(
          math::invert(float3x3(primary_ob.object_to_world())));
      result.sculpt_normal_world = math::normalize(local_to_world_normal * normal_local);
    }
  }
  return result;
}

void propagate_shared_stroke_state(Object &secondary_ob,
                                   const SharedStrokeStateSnapshot &primary_state)
{
  SculptSession *ss = secondary_ob.runtime->sculpt_session;
  if (!ss || !ss->cache) {
    return;
  }
  StrokeCache *cache = ss->cache;
  if (primary_state.density_seed && !cache->paint_brush.density_seed) {
    cache->paint_brush.density_seed = *primary_state.density_seed;
  }
  if (primary_state.painted_face_set_id != face_set_none_id &&
      cache->paint_face_set == face_set_none_id)
  {
    cache->paint_face_set = primary_state.painted_face_set_id;
  }
  if (primary_state.sculpt_normal_world) {
    /* Inverse transpose of #Object::world_to_object, which is the transpose of the object-to-world
     * basis. Seeded unconditionally and every step: this runs BEFORE the object's own passes, so
     * if its main pass does produce a normal that computation still wins, and if it does not (main
     * daub missing, or covering only masked/hidden geometry) the mirror pass has a usable one
     * instead of zero. Re-seeding each step also keeps the mirrored daub tracking the surface as
     * the stroke moves, exactly as it would on a joined mesh. */
    const float3x3 world_to_local_normal = math::transpose(
        float3x3(secondary_ob.object_to_world()));
    cache->sculpt_normal = math::normalize(world_to_local_normal *
                                           *primary_state.sculpt_normal_world);
  }
}

Object *MultiObjectStrokeContext::resolve_primary(Object *cursor_object, const Brush &brush)
{
  Object *primary_ob;
  if (need_delta_from_anchored_origin(brush)) {
    if (this->anchored_primary_object == nullptr) {
      this->anchored_primary_object = cursor_object;
    }
    primary_ob = this->anchored_primary_object;
  }
  else {
    primary_ob = cursor_object;
  }
  this->primary_object = primary_ob;
  return primary_ob;
}

void MultiObjectStrokeContext::propagate_shared_state(const ePaintSymmetrySpace symmetry_space,
                                                      const float4x4 &cursor_to_world)
{
  const Span<Object *> sample_objects = (this->mode_objects.size() > 1) ?
                                            this->mode_objects.as_span() :
                                            Span<Object *>();
  this->symm_reference_object = this->mode_objects.is_empty() ? nullptr : this->mode_objects[0];
  /* The shared symmetry frame is engaged whenever the mirror plane is NOT this object's own local
   * plane: for every multi-object stroke (all meshes share the reference plane), and also for a
   * single object in World / 3D-Cursor space, where the plane is world- or cursor-aligned rather
   * than the object's local axes. ACTIVE_OBJECT with a single object keeps the historical local
   * mirror (frame disengaged), staying bit-exact. This is independent of the cross-object surface
   * sampling (#sample_objects), which stays empty for a single object. */
  const bool non_local_space = symmetry_space != PAINT_SYMM_SPACE_ACTIVE_OBJECT;
  this->shared_symmetry_active = this->symm_reference_object != nullptr &&
                                 (!sample_objects.is_empty() || non_local_space);

  propagate_shared_sampling_and_symmetry_state(this->mode_objects,
                                               sample_objects,
                                               this->primary_object,
                                               this->symm_reference_object,
                                               this->shared_symmetry_active,
                                               symmetry_space,
                                               cursor_to_world);
}

bool MultiObjectStrokeContext::process_secondary(Object &ob,
                                                 StrokeCache &cache,
                                                 Paint &paint,
                                                 const Brush &brush,
                                                 const float3 &primary_world_center,
                                                 const float3 &primary_world_view_direction,
                                                 const Span<MirroredDaub> symm_daubs) const
{
  bool has_location = stroke_cache_set_location_from_world_sphere(
      ob, cache, paint, brush, primary_world_center, primary_world_view_direction);

  /* Shared symmetry: also process this object if its geometry lies under a mirrored daub, even
   * though the main daub misses it. The cache is still set up from the main center -- the mirror
   * passes derive their per-object #location_symm from it -- so the main pass is simply a no-op
   * here while the mirror pass that overlaps this object does the work. */
  if (!has_location && this->shared_symmetry_active) {
    /* The mirror surface snap may pull a mirrored daub up to N brush radii to reach this object's
     * surface, so widen the test by the same amount -- otherwise the object is rejected here
     * before the snap ever runs. The multiplier is 1.0 when the snap is inactive, and multiplying
     * the radius by 1.0 is exact, so the gate then tests what it always did.
     *
     * Index 0 is the un-mirrored main center, which #stroke_cache_set_location_from_world_sphere
     * already tested above at the plain radius; it is never snapped, so it must not be widened.
     * (It is the same center, round-tripped through the reference-space transforms, so re-testing
     * it here was redundant even before the snap.) */
    const float snap_multiplier = mirror_snap_search_multiplier(paint, brush);
    for (const MirroredDaub &daub : symm_daubs.drop_front(1)) {
      if (object_geometry_intersects_world_sphere(
              ob, cache, paint, brush, daub.center, daub.view_direction, snap_multiplier))
      {
        stroke_cache_apply_world_center(ob, cache, paint, brush, primary_world_center);
        has_location = true;
        break;
      }
    }
  }

  return has_location;
}

}  // namespace blender::ed::sculpt_paint
