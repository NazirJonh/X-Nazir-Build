/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#include "sculpt_multi_object.hh"

#include "BLI_math_matrix.hh"

#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/**
 * Publishes the shared multi-object surface-sampling context and the shared-symmetry
 * reference-space transforms onto every object's #StrokeCache for this stroke step (Container B
 * of the shared-per-stroke state; see #SharedStrokeStateSnapshot's doc-comment for how this
 * relates to Container C). Unlike Container C, every field written here is a pure function of
 * `mode_objects`/`sample_objects`/`primary_ob`/`symm_reference_ob` -- none of it is read back from
 * a lazily-populated cache, so the write order across `mode_objects` does not matter (this is
 * called once per #update_step, before the primary-first loop below it that Container C's ordering
 * invariant applies to).
 *
 * Pure extraction of the previous inline loop -- no behavior change. See
 * `.MyTaskAndDoc/.../Refactoring_2/Architecture_Refactoring_Analysis.md` 3.1.
 */
static void propagate_shared_sampling_and_symmetry_state(const Span<Object *> mode_objects,
                                                         const Span<Object *> sample_objects,
                                                         Object *primary_ob,
                                                         Object *symm_reference_ob,
                                                         const bool shared_symmetry_active)
{
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
    /* Reference-space transforms for the shared symmetry plane (see #StrokeCache). Identity for
     * the reference (active) object itself and for single-object strokes. */
    if (!shared_symmetry_active || object_ptr == symm_reference_ob) {
      ss_iter->cache->symm_ref_from_cur = float4x4::identity();
      ss_iter->cache->symm_cur_from_ref = float4x4::identity();
      ss_iter->cache->symm_shared_origin_active = false;
    }
    else {
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

void MultiObjectStrokeContext::propagate_shared_state()
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
                                               this->shared_symmetry_active);
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
