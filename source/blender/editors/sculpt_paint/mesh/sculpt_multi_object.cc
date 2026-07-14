/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#include "sculpt_multi_object.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"

#include "DNA_mesh_types.h"

#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "../paint_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

float4x4 symmetry_space_frame(const ePaintSymmetrySpace symmetry_space,
                              const float4x4 &reference_world_to_object,
                              const float3 &cursor_world)
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
      result = math::from_location<float4x4>(-cursor_world);
      break;
    default:
      BLI_assert_unreachable();
      result = float4x4::identity();
      break;
  }
  return result;
}

Vector<float3> shared_symmetry_world_centers(const Object &reference_ob,
                                             const float3 &world_center,
                                             const ePaintSymmetrySpace symmetry_space,
                                             const float3 &cursor_world)
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
        symmetry_space, reference_ob.world_to_object(), cursor_world);
    from_symm_space = math::invert(to_symm_space);
  }
  const float3 center_ref = math::transform_point(to_symm_space, world_center);

  Vector<float3> centers;
  for (int i = 0; i <= symm; i++) {
    if (!is_symmetry_iteration_valid(i, symm)) {
      continue;
    }
    const float3 mirrored = symmetry_flip(center_ref, ePaintSymmetryFlags(i));
    centers.append(math::transform_point(from_symm_space, mirrored));

    /* Radial copies around each axis, matching #do_radial_symmetry (flip, then rotate). */
    for (int axis = 0; axis < 3; axis++) {
      const int count = mesh.radial_symmetry[axis];
      for (int r = 1; r < count; r++) {
        const float angle = 2.0f * M_PI * r / count;
        float mat[3][3];
        axis_angle_to_mat3_single(mat, 'X' + axis, angle);
        float3 rotated = mirrored;
        mul_m3_v3(mat, rotated);
        centers.append(math::transform_point(from_symm_space, rotated));
      }
    }
  }

  return centers;
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
                                                         const float3 &cursor_world)
{
  /* Symmetry frame for the whole stroke. In ACTIVE_OBJECT mode we keep the historical per-object
   * expressions verbatim (see below) and never touch S, so the default path stays byte-identical. */
  const bool global_symmetry = shared_symmetry_active &&
                               symmetry_space != PAINT_SYMM_SPACE_ACTIVE_OBJECT;
  float4x4 S = float4x4::identity();
  float4x4 S_inv = float4x4::identity();
  if (global_symmetry) {
    S = symmetry_space_frame(
        symmetry_space, symm_reference_ob->world_to_object(), cursor_world);
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
    ss_iter->cache->symm_reference_object = !sample_objects.is_empty() ? symm_reference_ob :
                                                                         nullptr;
    /* Reference-space transforms for the shared symmetry plane (see #StrokeCache). */
    if (!shared_symmetry_active) {
      /* Single-object stroke: untouched, bit-exact. */
      ss_iter->cache->symm_ref_from_cur = float4x4::identity();
      ss_iter->cache->symm_cur_from_ref = float4x4::identity();
      ss_iter->cache->symm_shared_origin_active = false;
    }
    else if (global_symmetry) {
      /* World-axis symmetry: EVERY object (including the reference) is carried into the shared world
       * frame S, mirrored there, and carried back. */
      ss_iter->cache->symm_ref_from_cur = S * object_ptr->object_to_world();
      ss_iter->cache->symm_cur_from_ref = object_ptr->world_to_object() * S_inv;
      ss_iter->cache->symm_shared_origin_active = true;
    }
    else if (object_ptr == symm_reference_ob) {
      /* ACTIVE_OBJECT mode, reference object: identity -> traditional per-object mirror, bit-exact. */
      ss_iter->cache->symm_ref_from_cur = float4x4::identity();
      ss_iter->cache->symm_cur_from_ref = float4x4::identity();
      ss_iter->cache->symm_shared_origin_active = false;
    }
    else {
      /* ACTIVE_OBJECT mode, secondary object: verbatim historical expressions (do NOT use S/invert,
       * so this stays byte-identical to the pre-feature code). */
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
                                                      const float3 &cursor_world)
{
  const Span<Object *> sample_objects = (this->mode_objects.size() > 1) ?
                                            this->mode_objects.as_span() :
                                            Span<Object *>();
  this->symm_reference_object = this->mode_objects.is_empty() ? nullptr : this->mode_objects[0];
  this->shared_symmetry_active = !sample_objects.is_empty() &&
                                 this->symm_reference_object != nullptr;

  propagate_shared_sampling_and_symmetry_state(this->mode_objects,
                                               sample_objects,
                                               this->primary_object,
                                               this->symm_reference_object,
                                               this->shared_symmetry_active,
                                               symmetry_space,
                                               cursor_world);
}

bool MultiObjectStrokeContext::process_secondary(Object &ob,
                                                 StrokeCache &cache,
                                                 Paint &paint,
                                                 const Brush &brush,
                                                 const float3 &primary_world_center,
                                                 const Span<float3> symm_world_centers) const
{
  bool has_location = stroke_cache_set_location_from_world_sphere(
      ob, cache, paint, brush, primary_world_center);

  /* Shared symmetry: also process this object if its geometry lies under a mirrored daub, even
   * though the main daub misses it. The cache is still set up from the main center -- the mirror
   * passes derive their per-object #location_symm from it -- so the main pass is simply a no-op
   * here while the mirror pass that overlaps this object does the work. */
  if (!has_location && this->shared_symmetry_active) {
    for (const float3 &center : symm_world_centers) {
      if (object_geometry_intersects_world_sphere(ob, cache, paint, brush, center)) {
        stroke_cache_apply_world_center(ob, cache, paint, brush, primary_world_center);
        has_location = true;
        break;
      }
    }
  }

  return has_location;
}

}  // namespace blender::ed::sculpt_paint
