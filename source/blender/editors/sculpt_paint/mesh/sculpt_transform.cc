/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_array_utils.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_hash.h"
#include "BLI_kdtree.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_kelvinlet.h"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_subdiv_ccg.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh"
#include "mesh_brush_common.hh"
#include "paint_mask.hh"
#include "sculpt_filter.hh"
#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstdlib>
#include <optional>

namespace blender::ed::sculpt_paint {

/**
 * True when Origin Correct should drive a NON-ACTIVE object's own #Object matrix this Transform
 * session, instead of deforming its mesh vertices. Shared by #init_transform_add_object (snapshot
 * capture), #update_modal_transform, and #cancel_modal_transform (Tasks 7/8) so the same
 * four-part condition can't drift out of sync between them. Does not itself check
 * `is_active` -- callers that process both the active and secondary objects must additionally
 * gate on `!is_active` (the active object is never origin-corrected, see the design spec §3).
 */
static bool origin_correct_active_for_secondary(const Sculpt &sd)
{
  return sd.transform_all_objects && sd.transform_origin_correct &&
         sd.transform_mode == SCULPT_TRANSFORM_MODE_ALL_VERTICES;
}

static void sculpt_cursor_store_from_transform_pivot(bContext *C, Object &ob, SculptSession &ss)
{
  Scene *scene = CTX_data_scene(C);
  if (!scene || !cursor::is_enabled(*scene)) {
    return;
  }
  cursor::CursorState state;
  state.location = ss.pivot_pos;
  state.rotation = ss.pivot_rot;
  cursor::state_set(*scene, ob, state);
}

static void init_transform_common(bContext *C, Object &ob, const float mval_fl[2])
{
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  SculptSession &ss = *ob.runtime->sculpt_session;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  /* The transform pivot (and, in multi-object sculpt, the shared cursor position for every
   * target object) is established by #createTransSculpt before this runs, so it is used as-is. */

  ss.init_pivot_pos = ss.pivot_pos;
  ss.init_pivot_rot = ss.pivot_rot;
  ss.init_pivot_scale = ss.pivot_scale;

  ss.prev_pivot_pos = ss.pivot_pos;
  ss.prev_pivot_rot = ss.pivot_rot;
  ss.prev_pivot_scale = ss.pivot_scale;

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  vert_random_access_ensure(ob);

  filter::cache_init(C, ob, sd, undo::NodeDataFlag::Position, mval_fl, 5.0, 1.0f);

  if (sd.transform_mode == SCULPT_TRANSFORM_MODE_RADIUS_ELASTIC) {
    ss.filter_cache->transform_displacement_mode = TransformDisplacementMode::Incremental;
  }
  else {
    ss.filter_cache->transform_displacement_mode = TransformDisplacementMode::Original;
  }
}

void init_transform(bContext *C, Object &ob, const float mval_fl[2], const char *undo_name)
{
  const Scene &scene = *CTX_data_scene(C);
  undo::push_begin_ex(scene, ob, undo_name);
  init_transform_common(C, ob, mval_fl);
}

void init_transform_add_object(bContext *C, Object &ob, const float mval_fl[2])
{
  undo::push_begin_add_object(ob);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  if (origin_correct_active_for_secondary(sd)) {
    undo::set_object_transform_snapshot(ob);
  }
  init_transform_common(C, ob, mval_fl);
}

Vector<Object *> transform_target_objects(bContext *C)
{
  Object &active_ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  if (!sd.transform_all_objects) {
    return {&active_ob};
  }

  const Main &bmain = *CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const View3D *v3d = CTX_wm_view3d(C);
  const ObjectsInModeParams params{OB_MODE_SCULPT, false, nullptr, nullptr};
  return BKE_view_layer_array_from_objects_in_mode_params(bmain, scene, view_layer, v3d, &params);
}

/**
 * Splits \a ob's linear (3x3) world-space basis -- rotation times scale, possibly non-uniform
 * and/or sheared (e.g. inherited from a non-uniformly-scaled, rotated parent) -- into its closest
 * pure rotation \a r_orientation and the corresponding scale/shear part \a r_scale, via polar
 * decomposition (#mat3_polar_decompose). \a r_orientation_inv and \a r_scale_inv are their
 * inverses: #r_orientation is always orthonormal, so its inverse is a plain transpose; #r_scale
 * can be singular for a degenerate object (e.g. a zero #Object.scale axis), in which case its
 * "inverse" falls back to identity rather than propagating NaN/Inf into the mesh.
 *
 * A naive per-column #normalize_m3() is only a valid rotation extraction when the 3x3 basis is
 * ALREADY orthogonal; #mat3_polar_decompose finds the closest orthogonal matrix by SVD instead,
 * so this stays correct under shear too. It also resolves the sign ambiguity for a mirrored
 * (negative-determinant) object the same way #interp_m4_m4m4 does, so #r_orientation is always a
 * proper rotation (never a rotation composed with a reflection) and can be safely round-tripped
 * through a quaternion.
 *
 * Shared by #local_pivot_rot_to_world, #sync_local_pivot_from_world, and
 * #transform_matrices_init -- all three need to conjugate a rotation (or, for the latter, the
 * full transform) by the object's orientation alone or by its full linear basis, never by a
 * matrix that silently mixes the two. Takes the object-to-world matrix explicitly (rather than
 * an `Object &` to read it from) so a rigid-body Origin Correct secondary can conjugate by its
 * FIXED session-start matrix instead of its live, per-step-mutated one -- see
 * #sync_local_pivot_from_matrix.
 */
static void object_orientation_and_scale(const float4x4 &object_to_world,
                                         float r_orientation[3][3],
                                         float r_orientation_inv[3][3],
                                         float r_scale[3][3],
                                         float r_scale_inv[3][3])
{
  float object_lin_mat[3][3];
  copy_m3_m4(object_lin_mat, object_to_world.ptr());

  mat3_polar_decompose(object_lin_mat, r_orientation, r_scale);
  if (is_negative_m3(r_orientation)) {
    /* Quaternions cannot represent a reflection; fold the sign flip into the scale part instead
     * (same fix as #interp_m4_m4m4's for #77154). */
    mul_m3_fl(r_orientation, -1.0f);
    mul_m3_fl(r_scale, -1.0f);
  }
  transpose_m3_m3(r_orientation_inv, r_orientation);

  if (!invert_m3_m3(r_scale_inv, r_scale)) {
    unit_m3(r_scale_inv);
  }
}

/**
 * Conjugates the quaternion \a in_rot by \a mat (i.e. computes
 * `mat * quat_to_mat3(in_rot) * mat_inv`, converting the result back to a quaternion \a r_rot).
 * Used both to convert a LOCAL rotation into WORLD space (\a mat = orientation, \a mat_inv =
 * orientation_inv) and the reverse (\a mat = orientation_inv, \a mat_inv = orientation) -- see
 * #local_pivot_rot_to_world and #sync_local_pivot_from_world.
 */
static void conjugate_quat_m3(const float mat[3][3],
                              const float mat_inv[3][3],
                              const float in_rot[4],
                              float r_rot[4])
{
  float in_mat[3][3];
  quat_to_mat3(in_mat, in_rot);

  float tmp[3][3], out_mat[3][3];
  mul_m3_m3m3(tmp, in_mat, mat_inv);
  mul_m3_m3m3(out_mat, mat, tmp);
  mat3_to_quat(r_rot, out_mat);
}

void local_pivot_rot_to_world(const Object &ob, const float local_rot[4], float r_world_rot[4])
{
  float orientation[3][3], orientation_inv[3][3], scale[3][3], scale_inv[3][3];
  object_orientation_and_scale(
      ob.object_to_world(), orientation, orientation_inv, scale, scale_inv);
  conjugate_quat_m3(orientation, orientation_inv, local_rot, r_world_rot);
}

/**
 * Core of #sync_local_pivot_from_world, parameterized by an explicit object-to-world matrix
 * instead of reading it from an `Object &`. For the ACTIVE object (and for a normal, mesh-deform
 * secondary), that matrix never changes during a Transform session, so calling
 * #sync_local_pivot_from_world(ob) every modal step is self-consistent with #ss.init_pivot_pos/
 * #init_pivot_rot (captured once, from that same unchanging matrix, in #init_transform_common).
 *
 * A rigid-body Origin Correct secondary is different: #update_modal_transform mutates THAT
 * object's own matrix in place every step via #BKE_object_apply_mat4. Deriving the local pivot
 * from the object's live (already-mutated) matrix on the NEXT step would compare it against
 * #init_pivot_pos/#init_pivot_rot, which are still expressed in the ORIGINAL (session-start)
 * frame -- a mismatch that compounds every step (confirmed at runtime via `[ORIGDBG]` printf
 * tracing: the object's world rotation visibly oscillated/jittered while slowly advancing,
 * instead of tracking the gizmo smoothly). The fix is to always conjugate by the FIXED
 * session-start matrix (from the undo snapshot, #undo::get_object_transform_snapshot) for a
 * rigid-body secondary, never by its live matrix -- see #update_modal_transform's rigid-body
 * branch.
 */
static void sync_local_pivot_from_matrix(SculptSession &ss, const float4x4 &object_to_world)
{
  float world_to_object[4][4];
  invert_m4_m4(world_to_object, object_to_world.ptr());
  copy_v3_v3(ss.pivot_pos, ss.transform_pivot_pos_world);
  mul_m4_v3(world_to_object, ss.pivot_pos);

  float orientation[3][3], orientation_inv[3][3], scale[3][3], scale_inv[3][3];
  object_orientation_and_scale(object_to_world, orientation, orientation_inv, scale, scale_inv);
  conjugate_quat_m3(orientation_inv, orientation, ss.transform_pivot_rot_world, ss.pivot_rot);
}

void sync_local_pivot_from_world(Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  sync_local_pivot_from_matrix(ss, ob.object_to_world());
}

/**
 * Per-symmetry-area components of one modal Transform step. Split out of the matrix assembly so
 * the proportional path can interpolate the transform towards identity per vertex, instead of
 * applying the full matrix and scaling the resulting displacement -- which pulls a rotated vertex
 * along the chord and visibly pinches the mesh at the edge of the falloff.
 */
struct TransformComponents {
  /** Translation delta (`pivot_pos - start pivot`), symmetry-flipped. */
  float3 translation;
  /** Local rotation delta quaternion, symmetry-flipped. */
  float rotation[4];
  /** Scale factor (`pivot_scale - start scale + 1`), shared across symmetry areas. */
  float3 scale;
  /** Symmetry-flipped final pivot position. */
  float3 pivot;
};

static std::array<TransformComponents, PAINT_SYMM_AREAS> transform_components_init(
    const SculptSession &ss,
    const ePaintSymmetryFlags symm,
    const TransformDisplacementMode t_mode)
{
  float start_pivot_pos[3], start_pivot_rot[4], start_pivot_scale[3];
  switch (t_mode) {
    case TransformDisplacementMode::Original:
      copy_v3_v3(start_pivot_pos, ss.init_pivot_pos);
      copy_v4_v4(start_pivot_rot, ss.init_pivot_rot);
      copy_v3_v3(start_pivot_scale, ss.init_pivot_scale);
      break;
    case TransformDisplacementMode::Incremental:
      copy_v3_v3(start_pivot_pos, ss.prev_pivot_pos);
      copy_v4_v4(start_pivot_rot, ss.prev_pivot_rot);
      copy_v3_v3(start_pivot_scale, ss.prev_pivot_scale);
      break;
  }

  std::array<TransformComponents, PAINT_SYMM_AREAS> components;
  for (int i = 0; i < PAINT_SYMM_AREAS; i++) {
    ePaintSymmetryAreas v_symm = ePaintSymmetryAreas(i);
    TransformComponents &comp = components[i];

    sub_v3_v3v3(comp.translation, ss.pivot_pos, start_pivot_pos);
    comp.translation = flip_v3_by_symm_area(comp.translation, symm, v_symm, ss.init_pivot_pos);

    sub_qt_qtqt(comp.rotation, ss.pivot_rot, start_pivot_rot);
    normalize_qt(comp.rotation);
    flip_quat_by_symm_area(comp.rotation, symm, v_symm, ss.init_pivot_pos);

    sub_v3_v3v3(comp.scale, ss.pivot_scale, start_pivot_scale);
    add_v3_fl(comp.scale, 1.0f);

    comp.pivot = flip_v3_by_symm_area(ss.pivot_pos, symm, v_symm, start_pivot_pos);
  }

  return components;
}

/**
 * Assemble the full affine transform for one symmetry area from \a components. The rotation is
 * built via full conjugation (object orientation AND scale) instead of a naive local quaternion,
 * so a non-uniformly-scaled object rotates rigidly around the shared world pivot instead of
 * shearing. For a uniformly-scaled or unscaled object this is bit-exact with `quat_to_mat4`
 * (scale cancels out of the conjugation when it is a multiple of the identity).
 */
static void build_symm_area_transform_matrix(const TransformComponents &components,
                                             const float orientation[3][3],
                                             const float orientation_inv[3][3],
                                             const float scale_mat[3][3],
                                             const float scale_inv_mat[3][3],
                                             float r_mat[4][4])
{
  float t_mat[4][4], r_mat_local[4][4], s_mat[4][4], pivot_mat[4][4], pivot_imat[4][4],
      transform_mat[4][4];

  unit_m4(t_mat);
  unit_m4(r_mat_local);
  unit_m4(s_mat);
  unit_m4(pivot_mat);

  translate_m4(
      t_mat, components.translation[0], components.translation[1], components.translation[2]);

  float world_d_r[4];
  conjugate_quat_m3(orientation, orientation_inv, components.rotation, world_d_r);
  float world_rot_mat[3][3];
  quat_to_mat3(world_rot_mat, world_d_r);

  float ortho_scale[3][3], world_ortho_scale[3][3], conjugated[3][3], r_mat3[3][3];
  mul_m3_m3m3(ortho_scale, orientation, scale_mat);            /* O * S. */
  mul_m3_m3m3(world_ortho_scale, world_rot_mat, ortho_scale);  /* Rw * O * S. */
  mul_m3_m3m3(conjugated, orientation_inv, world_ortho_scale); /* O^-1 * Rw * O * S. */
  mul_m3_m3m3(r_mat3, scale_inv_mat, conjugated);              /* S^-1 * O^-1 * Rw * O * S. */
  copy_m4_m3(r_mat_local, r_mat3);

  size_to_mat4(s_mat, components.scale);

  translate_m4(pivot_mat, components.pivot[0], components.pivot[1], components.pivot[2]);
  invert_m4_m4(pivot_imat, pivot_mat);

  mul_m4_m4m4(transform_mat, r_mat_local, t_mat);
  mul_m4_m4m4(transform_mat, transform_mat, s_mat);
  mul_m4_m4m4(r_mat, transform_mat, pivot_imat);
  mul_m4_m4m4(r_mat, pivot_mat, r_mat);
}

static std::array<float4x4, PAINT_SYMM_AREAS> transform_matrices_from_components(
    const std::array<TransformComponents, PAINT_SYMM_AREAS> &components,
    const float orientation[3][3],
    const float orientation_inv[3][3],
    const float scale_mat[3][3],
    const float scale_inv_mat[3][3])
{
  std::array<float4x4, PAINT_SYMM_AREAS> mats;
  for (int i = 0; i < PAINT_SYMM_AREAS; i++) {
    build_symm_area_transform_matrix(
        components[i], orientation, orientation_inv, scale_mat, scale_inv_mat, mats[i].ptr());
  }
  return mats;
}

static std::array<float4x4, 8> transform_matrices_init(const float4x4 &object_to_world,
                                                       const SculptSession &ss,
                                                       const ePaintSymmetryFlags symm,
                                                       const TransformDisplacementMode t_mode)
{
  const std::array<TransformComponents, PAINT_SYMM_AREAS> components =
      transform_components_init(ss, symm, t_mode);

  /* The object's orientation/scale split (used by the rotation matrix) is invariant across all 8
   * symmetry areas -- the object doesn't move mid-step -- so compute it once here. \a
   * object_to_world is passed in explicitly (rather than read from an `Object &`) so a rigid-body
   * Origin Correct secondary can pass its FIXED session-start matrix instead of its live,
   * per-step-mutated one. */
  float orientation[3][3], orientation_inv[3][3], scale_mat[3][3], scale_inv_mat[3][3];
  object_orientation_and_scale(
      object_to_world, orientation, orientation_inv, scale_mat, scale_inv_mat);

  return transform_matrices_from_components(
      components, orientation, orientation_inv, scale_mat, scale_inv_mat);
}

static constexpr float transform_mirror_max_distance_eps = 0.00002f;

struct TransformLocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float3> translations;
  Vector<int> vert_indices;
};

BLI_NOINLINE static void calc_symm_area_transform_translations(
    const Span<float3> positions,
    const std::array<float4x4, 8> &transform_mats,
    const MutableSpan<float3> translations)
{
  for (const int i : positions.index_range()) {
    const ePaintSymmetryAreas symm_area = get_vertex_symm_area(positions[i]);
    const float3 transformed = math::transform_point(transform_mats[symm_area], positions[i]);
    translations[i] = transformed - positions[i];
  }
}

BLI_NOINLINE static void filter_translations_with_symmetry(const Span<float3> positions,
                                                           const ePaintSymmetryFlags symm,
                                                           const MutableSpan<float3> translations)
{
  if ((symm & (PAINT_SYMM_X | PAINT_SYMM_Y | PAINT_SYMM_Z)) == 0) {
    return;
  }
  for (const int i : positions.index_range()) {
    if ((symm & PAINT_SYMM_X) && (std::abs(positions[i].x) < transform_mirror_max_distance_eps)) {
      translations[i].x = 0.0f;
    }
    if ((symm & PAINT_SYMM_Y) && (std::abs(positions[i].y) < transform_mirror_max_distance_eps)) {
      translations[i].y = 0.0f;
    }
    if ((symm & PAINT_SYMM_Z) && (std::abs(positions[i].z) < transform_mirror_max_distance_eps)) {
      translations[i].z = 0.0f;
    }
  }
}

/**
 * Per-vertex proportional falloff weight. Mirrors the formulas in #calculatePropRatio
 * (`transform_generics.cc`) so the sculpt falloff profiles match Edit Mode. Unlike the original,
 * #PROP_RANDOM is hashed from a stable per-vertex seed instead of a running RNG, so the profile
 * does not flicker between modal steps.
 *
 * \param dist: 1 at the pivot, 0 at the falloff radius.
 */
static float proportional_falloff(const int mode, const float dist, const uint32_t seed)
{
  switch (mode) {
    case PROP_SHARP:
      return dist * dist;
    case PROP_SMOOTH:
      return min_ff(1.0f, 3.0f * dist * dist - 2.0f * dist * dist * dist);
    case PROP_ROOT:
      return sqrtf(dist);
    case PROP_LIN:
      return dist;
    case PROP_CONST:
      return 1.0f;
    case PROP_SPHERE:
      return sqrtf(2.0f * dist - dist * dist);
    case PROP_RANDOM:
      return BLI_hash_int_01(seed) * dist;
    case PROP_INVSQUARE:
      return dist * (2.0f - dist);
    default:
      return 1.0f;
  }
}

/**
 * Per-symmetry-area data for the proportional path. The partial rotation uses Rodrigues' formula
 * `R(a) = I + sin(a) K + (1 - cos(a)) K^2`, with `K` and `K^2` conjugated into object space once
 * per step, so each vertex only costs one `sin`/`cos` pair instead of assembling a matrix. Scaling
 * the angle (like #ElementRotation in Edit Mode) instead of slerping also keeps rotations past 180
 * degrees continuous, where a shortest-path slerp would flip direction.
 */
struct ProportionalAreaTransform {
  float3 pivot;
  float3 translation;
  float3 scale;
  float angle;
  /** `S^-1 * O^-1 * K * O * S`, see #build_symm_area_transform_matrix. */
  float rot_k[3][3];
  /** `S^-1 * O^-1 * K^2 * O * S`. */
  float rot_k2[3][3];
};

/**
 * Everything the per-vertex transform math needs for one modal step: the assembled matrices when
 * proportional editing is off, or the interpolatable per-area components when it is on.
 */
struct TransformStepData {
  std::array<float4x4, PAINT_SYMM_AREAS> mats;
  std::array<ProportionalAreaTransform, PAINT_SYMM_AREAS> areas;
};

/** `S^-1 * O^-1 * m * O * S`, the same conjugation #build_symm_area_transform_matrix applies. */
static void conjugate_m3_by_object(const float m[3][3],
                                   const float orientation[3][3],
                                   const float orientation_inv[3][3],
                                   const float scale_mat[3][3],
                                   const float scale_inv_mat[3][3],
                                   float r_m[3][3])
{
  float ortho_scale[3][3], tmp[3][3], tmp2[3][3];
  mul_m3_m3m3(ortho_scale, orientation, scale_mat);
  mul_m3_m3m3(tmp, m, ortho_scale);
  mul_m3_m3m3(tmp2, orientation_inv, tmp);
  mul_m3_m3m3(r_m, scale_inv_mat, tmp2);
}

static ProportionalAreaTransform proportional_area_transform_init(
    const TransformComponents &components,
    const float orientation[3][3],
    const float orientation_inv[3][3],
    const float scale_mat[3][3],
    const float scale_inv_mat[3][3])
{
  ProportionalAreaTransform area;
  area.pivot = components.pivot;
  area.translation = components.translation;
  area.scale = components.scale;
  zero_m3(area.rot_k);
  zero_m3(area.rot_k2);

  float local_rot[4], world_rot[4];
  normalize_qt_qt(local_rot, components.rotation);
  conjugate_quat_m3(orientation, orientation_inv, local_rot, world_rot);
  normalize_qt(world_rot);

  float axis[3];
  quat_to_axis_angle(axis, &area.angle, world_rot);
  if (std::abs(area.angle) < 1e-7f || normalize_v3(axis) == 0.0f) {
    area.angle = 0.0f;
    return area;
  }

  /* Derive `K` and `K^2` from rotation matrices built by the same routine that builds the full
   * rotation, so they follow its axis and matrix conventions exactly:
   * `R(pi) = I + 2 K^2` and `R(pi/2) = I + K + K^2`. */
  float r90[3][3], r180[3][3], k[3][3], k2[3][3];
  axis_angle_normalized_to_mat3(r90, axis, float(M_PI_2));
  axis_angle_normalized_to_mat3(r180, axis, float(M_PI));
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      const float identity = (i == j) ? 1.0f : 0.0f;
      k2[i][j] = (r180[i][j] - identity) * 0.5f;
      k[i][j] = r90[i][j] - identity - k2[i][j];
    }
  }
  conjugate_m3_by_object(k, orientation, orientation_inv, scale_mat, scale_inv_mat, area.rot_k);
  conjugate_m3_by_object(k2, orientation, orientation_inv, scale_mat, scale_inv_mat, area.rot_k2);
  return area;
}

static TransformStepData transform_step_data_init(const float4x4 &object_to_world,
                                                  const SculptSession &ss,
                                                  const ePaintSymmetryFlags symm,
                                                  const TransformDisplacementMode t_mode,
                                                  const bool proportional)
{
  const std::array<TransformComponents, PAINT_SYMM_AREAS> components =
      transform_components_init(ss, symm, t_mode);
  float orientation[3][3], orientation_inv[3][3], scale_mat[3][3], scale_inv_mat[3][3];
  object_orientation_and_scale(
      object_to_world, orientation, orientation_inv, scale_mat, scale_inv_mat);

  TransformStepData step;
  if (proportional) {
    for (int i = 0; i < PAINT_SYMM_AREAS; i++) {
      step.areas[i] = proportional_area_transform_init(
          components[i], orientation, orientation_inv, scale_mat, scale_inv_mat);
    }
  }
  else {
    step.mats = transform_matrices_from_components(
        components, orientation, orientation_inv, scale_mat, scale_inv_mat);
  }
  return step;
}

/**
 * Apply the transform of \a area interpolated from identity by \a factor:
 * `pivot + R(f * angle) * (lerp(1, s, f) * (p - pivot) + f * t)`, which at `factor == 1` matches
 * the full matrix `P * R * T * S * P^-1`.
 */
static float3 proportional_transform_point(const ProportionalAreaTransform &area,
                                           const float3 &position,
                                           const float factor)
{
  const float3 scale = float3(1.0f) + (area.scale - float3(1.0f)) * factor;
  float3 co = (position - area.pivot) * scale + area.translation * factor;
  if (area.angle != 0.0f) {
    const float angle = area.angle * factor;
    float3 k_co, k2_co;
    mul_v3_m3v3(k_co, area.rot_k, co);
    mul_v3_m3v3(k2_co, area.rot_k2, co);
    co += k_co * std::sin(angle) + k2_co * (1.0f - std::cos(angle));
  }
  return area.pivot + co;
}

/**
 * Combine the mask/visibility factor of a vertex with its proportional falloff from the pivot.
 *
 * The radius alone limits the affected region; the mask only protects. Multiplying a hard mask
 * into the falloff would still tear the mesh wherever the radius crosses the mask border, so the
 * mask weight is additionally faded to zero over one radius towards the nearest fully masked or
 * hidden vertex (#TransformProportional::vert_mask_dist). Masked vertices never move.
 */
static float proportional_vert_factor(const filter::TransformProportional &prop,
                                      const float mask_factor,
                                      const int vert)
{
  const float dist = prop.vert_dist[vert];
  if (mask_factor <= 0.0f || dist >= prop.radius) {
    return 0.0f;
  }
  float factor = mask_factor * proportional_falloff(
                                   prop.falloff, 1.0f - dist / prop.radius, uint32_t(vert));
  if (!prop.vert_mask_dist.is_empty()) {
    const float t = std::min(prop.vert_mask_dist[vert] / prop.radius, 1.0f);
    factor *= t * t * (3.0f - 2.0f * t);
  }
  return factor;
}

BLI_NOINLINE static void calc_symm_area_transform_translations_proportional(
    const Span<float3> positions,
    const TransformStepData &step,
    const filter::TransformProportional &prop,
    const Span<int> vert_indices,
    const Span<float> factors,
    const MutableSpan<float3> translations)
{
  for (const int i : positions.index_range()) {
    const int vert = vert_indices[i];
    const float factor = proportional_vert_factor(prop, factors[i], vert);
    if (factor <= 0.0f) {
      translations[i] = float3(0.0f);
      continue;
    }
    const ePaintSymmetryAreas symm_area = get_vertex_symm_area(positions[i]);
    translations[i] = proportional_transform_point(step.areas[symm_area], positions[i], factor) -
                      positions[i];
  }
}

void transform_set_proportional_params(Object &ob,
                                       const bool enabled,
                                       const float radius,
                                       const int falloff,
                                       const bool projected,
                                       const float view_normal[3])
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  BLI_assert(ss.filter_cache != nullptr);
  filter::TransformProportional &prop = ss.filter_cache->proportional;
  prop.enabled = enabled;
  prop.radius = radius;
  prop.falloff = falloff;
  prop.projected = projected;
  prop.view_normal = float3(view_normal);
}

/** Global vertex indices of a grids node, in the order of its gathered data. */
static Span<int> grids_node_vert_indices(const Span<int> grids,
                                         const int grid_area,
                                         Vector<int> &r_indices)
{
  r_indices.resize(grids.size() * grid_area);
  for (const int i : grids.index_range()) {
    array_utils::fill_index_range(r_indices.as_mutable_span().slice(i * grid_area, grid_area),
                                  grids[i] * grid_area);
  }
  return r_indices;
}

static Span<int> bmesh_node_vert_indices(const Set<BMVert *, 0> &verts, Vector<int> &r_indices)
{
  r_indices.resize(verts.size());
  int i = 0;
  for (const BMVert *vert : verts) {
    r_indices[i++] = BM_elem_index_get(vert);
  }
  return r_indices;
}

static void transform_node_mesh(const Sculpt &sd,
                                const TransformStepData &step,
                                const filter::TransformProportional &prop,
                                const MeshAttributeData &attribute_data,
                                const bke::pbvh::MeshNode &node,
                                Object &object,
                                TransformLocalData &tls,
                                const PositionDeformData &position_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Span<int> verts = node.verts();
  const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  if (prop.enabled) {
    calc_symm_area_transform_translations_proportional(
        orig_data.positions, step, prop, verts, factors, translations);
  }
  else {
    calc_symm_area_transform_translations(orig_data.positions, step.mats, translations);
    scale_translations(translations, factors);
  }

  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  filter_translations_with_symmetry(orig_data.positions, symm, translations);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void transform_node_grids(const Sculpt &sd,
                                 const TransformStepData &step,
                                 const filter::TransformProportional &prop,
                                 const bke::pbvh::GridsNode &node,
                                 Object &object,
                                 TransformLocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  const Span<int> grids = node.grids();
  const int grid_verts_num = grids.size() * key.grid_area;

  const OrigPositionData orig_data = orig_position_data_get_grids(object, node);

  tls.factors.resize(grid_verts_num);
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);

  tls.translations.resize(grid_verts_num);
  const MutableSpan<float3> translations = tls.translations;
  if (prop.enabled) {
    const Span<int> vert_indices = grids_node_vert_indices(grids, key.grid_area, tls.vert_indices);
    calc_symm_area_transform_translations_proportional(
        orig_data.positions, step, prop, vert_indices, factors, translations);
  }
  else {
    calc_symm_area_transform_translations(orig_data.positions, step.mats, translations);
    scale_translations(translations, factors);
  }

  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  filter_translations_with_symmetry(orig_data.positions, symm, translations);

  clip_and_lock_translations(sd, ss, orig_data.positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void transform_node_bmesh(const Sculpt &sd,
                                 const TransformStepData &step,
                                 const filter::TransformProportional &prop,
                                 bke::pbvh::BMeshNode &node,
                                 Object &object,
                                 TransformLocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  Array<float3> orig_positions(verts.size());
  Array<float3> orig_normals(verts.size());
  orig_position_data_gather_bmesh(*ss.bm_log, verts, orig_positions, orig_normals);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  if (prop.enabled) {
    const Span<int> vert_indices = bmesh_node_vert_indices(verts, tls.vert_indices);
    calc_symm_area_transform_translations_proportional(
        orig_positions, step, prop, vert_indices, factors, translations);
  }
  else {
    calc_symm_area_transform_translations(orig_positions, step.mats, translations);
    scale_translations(translations, factors);
  }

  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  filter_translations_with_symmetry(orig_positions, symm, translations);

  clip_and_lock_translations(sd, ss, orig_positions, translations);
  apply_translations(translations, verts);
}

/**
 * Original positions, global vertex indices and mask/visibility factors of one PBVH node, as
 * used by the proportional distance precomputation.
 */
struct ProportionalNodeData {
  Vector<float3> positions;
  Vector<int> vert_indices;
  Vector<float> factors;
};

static void proportional_node_data_gather(Object &object,
                                          const bke::pbvh::Tree &pbvh,
                                          const MeshAttributeData *attribute_data,
                                          const int node_i,
                                          ProportionalNodeData &r_data)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const bke::pbvh::MeshNode &node = pbvh.nodes<bke::pbvh::MeshNode>()[node_i];
      const Span<int> verts = node.verts();
      r_data.positions.clear();
      r_data.positions.extend(orig_position_data_get_mesh(object, node).positions);
      r_data.vert_indices.clear();
      r_data.vert_indices.extend(verts);
      r_data.factors.resize(verts.size());
      fill_factor_from_hide_and_mask(
          attribute_data->hide_vert, attribute_data->mask, verts, r_data.factors);
      break;
    }
    case bke::pbvh::Type::Grids: {
      const bke::pbvh::GridsNode &node = pbvh.nodes<bke::pbvh::GridsNode>()[node_i];
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const Span<int> grids = node.grids();
      r_data.positions.clear();
      r_data.positions.extend(orig_position_data_get_grids(object, node).positions);
      grids_node_vert_indices(grids, subdiv_ccg.grid_area, r_data.vert_indices);
      r_data.factors.resize(r_data.positions.size());
      fill_factor_from_hide_and_mask(subdiv_ccg, grids, r_data.factors);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      bke::pbvh::BMeshNode &node = const_cast<bke::pbvh::BMeshNode &>(
          pbvh.nodes<bke::pbvh::BMeshNode>()[node_i]);
      const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
      r_data.positions.resize(verts.size());
      Array<float3> orig_normals(verts.size());
      orig_position_data_gather_bmesh(*ss.bm_log, verts, r_data.positions, orig_normals);
      bmesh_node_vert_indices(verts, r_data.vert_indices);
      r_data.factors.resize(verts.size());
      fill_factor_from_hide_and_mask(*ss.bm, verts, r_data.factors);
      break;
    }
  }
}

/**
 * World-space positions of the fixed (fully masked or hidden) vertices that share an edge with a
 * vertex that can move, i.e. the mask border. Only edges between a moving and a fixed vertex can
 * tear, so fading towards this border is enough, and it is usually orders of magnitude smaller than
 * the whole masked region, which keeps the KD-tree cheap to build on the first step of a drag.
 *
 * Reads the current positions: this runs right after the undo restore, when they are original.
 * A fixed vertex shared by several moving neighbors can be added more than once, which only makes
 * the tree slightly bigger.
 */
static Vector<float3> proportional_mask_border_positions(const Depsgraph &depsgraph,
                                                         Object &object,
                                                         const bke::pbvh::Tree &pbvh,
                                                         const IndexMask &node_mask,
                                                         const MeshAttributeData *attribute_data,
                                                         const filter::TransformProportional &prop)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const float4x4 &object_to_world = object.object_to_world();
  const Span<float> vert_dist = prop.vert_dist;
  const auto is_fixed = [&](const int vert) { return vert_dist[vert] == FLT_MAX; };

  struct LocalData {
    ProportionalNodeData node_data;
    Vector<float3> border;
    Vector<int> mesh_neighbors;
    BMeshNeighborVerts bmesh_neighbors;
    SubdivCCGNeighbors grids_neighbors;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;

  const Mesh *mesh = pbvh.type() == bke::pbvh::Type::Mesh ?
                         id_cast<const Mesh *>(object.data) :
                         nullptr;
  const Span<float3> mesh_positions = mesh ? bke::pbvh::vert_positions_eval(depsgraph, object) :
                                             Span<float3>();
  struct MeshTopology {
    OffsetIndices<int> faces;
    Span<int> corner_verts;
    GroupedSpan<int> vert_to_face_map;
  };
  std::optional<MeshTopology> mesh_topology;
  if (mesh) {
    mesh_topology.emplace(
        MeshTopology{mesh->faces(), mesh->corner_verts(), mesh->vert_to_face_map()});
  }

  node_mask.foreach_index(
      [&](const int i) {
        if (prop.node_min_dist[i] == FLT_MAX) {
          /* No vertex of this node can move. */
          return;
        }
        LocalData &tls = all_tls.local();
        ProportionalNodeData &data = tls.node_data;
        proportional_node_data_gather(object, pbvh, attribute_data, i, data);
        for (const int j : data.vert_indices.index_range()) {
          const int vert = data.vert_indices[j];
          if (is_fixed(vert)) {
            continue;
          }
          switch (pbvh.type()) {
            case bke::pbvh::Type::Mesh: {
              for (const int neighbor : vert_neighbors_get_mesh(mesh_topology->faces,
                                                                mesh_topology->corner_verts,
                                                                mesh_topology->vert_to_face_map,
                                                                attribute_data->hide_poly,
                                                                vert,
                                                                tls.mesh_neighbors))
              {
                if (is_fixed(neighbor)) {
                  tls.border.append(
                      math::transform_point(object_to_world, mesh_positions[neighbor]));
                }
              }
              break;
            }
            case bke::pbvh::Type::Grids: {
              const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
              const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
              BKE_subdiv_ccg_neighbor_coords_get(
                  subdiv_ccg, SubdivCCGCoord::from_index(key, vert), false, tls.grids_neighbors);
              for (const SubdivCCGCoord neighbor : tls.grids_neighbors.unique()) {
                const int neighbor_index = neighbor.to_index(key);
                if (is_fixed(neighbor_index)) {
                  tls.border.append(math::transform_point(
                      object_to_world, subdiv_ccg.positions[neighbor_index]));
                }
              }
              break;
            }
            case bke::pbvh::Type::BMesh: {
              BMVert *bm_vert = BM_vert_at_index(ss.bm, vert);
              for (BMVert *neighbor : vert_neighbors_get_bmesh(*bm_vert, tls.bmesh_neighbors)) {
                if (is_fixed(BM_elem_index_get(neighbor))) {
                  tls.border.append(math::transform_point(object_to_world, float3(neighbor->co)));
                }
              }
              break;
            }
          }
        }
      },
      exec_mode::grain_size(1));

  Vector<float3> border;
  for (LocalData &tls : all_tls) {
    border.extend(tls.border);
  }
  return border;
}

/**
 * Compute #TransformProportional::vert_dist, #vert_mask_dist and #node_min_dist from the original
 * positions.
 *
 * The distances only depend on data that is fixed for the whole drag, so this runs once instead
 * of on every modal step; a step then only reads per-vertex floats, and the per-node minimum gives
 * an exact node culling test without refreshing any bounds.
 */
static void proportional_distances_build(const Depsgraph &depsgraph,
                                         Object &object,
                                         const ePaintSymmetryFlags symm,
                                         filter::TransformProportional &prop)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const IndexMask &node_mask = ss.filter_cache->node_mask;
  const float4x4 &object_to_world = object.object_to_world();

  std::optional<MeshAttributeData> attribute_data;
  if (pbvh.type() == bke::pbvh::Type::Mesh) {
    attribute_data.emplace(*id_cast<const Mesh *>(object.data));
  }
  const MeshAttributeData *attribute_data_ptr = attribute_data ? &*attribute_data : nullptr;

  const bool projected = prop.projected;
  const float3 view_normal = prop.view_normal;

  float3 world_pivots[PAINT_SYMM_AREAS];
  for (int i = 0; i < PAINT_SYMM_AREAS; i++) {
    /* Mirror the initial pivot the same way the per-area transform does, so a vertex in a mirrored
     * symmetry area measures its falloff from the mirrored pivot. */
    world_pivots[i] = math::transform_point(
        object_to_world,
        flip_v3_by_symm_area(ss.init_pivot_pos, symm, ePaintSymmetryAreas(i), ss.init_pivot_pos));
  }

  const int verts_num = vertex_count_get(object);
  prop.vert_dist.reinitialize(verts_num);
  prop.vert_dist.fill(FLT_MAX);
  prop.node_min_dist.reinitialize(pbvh.nodes_num());
  prop.node_min_dist.fill(FLT_MAX);

  /* First pass: falloff distance from the pivot and the per-node culling distance. */
  struct BuildLocalData {
    ProportionalNodeData node_data;
    bool has_fixed = false;
    bool has_moving = false;
  };
  threading::EnumerableThreadSpecific<BuildLocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        BuildLocalData &tls = all_tls.local();
        ProportionalNodeData &data = tls.node_data;
        proportional_node_data_gather(object, pbvh, attribute_data_ptr, i, data);
        float node_min_dist = FLT_MAX;
        for (const int j : data.positions.index_range()) {
          if (data.factors[j] <= 0.0f) {
            tls.has_fixed = true;
            continue;
          }
          const float3 world_co = math::transform_point(object_to_world, data.positions[j]);
          const ePaintSymmetryAreas area = get_vertex_symm_area(data.positions[j]);
          float3 delta = world_co - world_pivots[area];
          if (projected) {
            delta -= view_normal * math::dot(delta, view_normal);
          }
          const float dist = math::length(delta);
          prop.vert_dist[data.vert_indices[j]] = dist;
          node_min_dist = std::min(node_min_dist, dist);
          tls.has_moving = true;
        }
        prop.node_min_dist[i] = node_min_dist;
      },
      exec_mode::grain_size(1));

  bool has_fixed = false;
  bool has_moving = false;
  for (const BuildLocalData &tls : all_tls) {
    has_fixed |= tls.has_fixed;
    has_moving |= tls.has_moving;
  }

  prop.vert_mask_dist = Array<float>();
  const Vector<float3> border = (has_fixed && has_moving) ?
                                    proportional_mask_border_positions(depsgraph,
                                                                       object,
                                                                       pbvh,
                                                                       node_mask,
                                                                       attribute_data_ptr,
                                                                       prop) :
                                    Vector<float3>();
  if (!border.is_empty()) {
    KDTree<float3> *tree = kdtree_new<float3>(uint(border.size()));
    for (const int i : border.index_range()) {
      kdtree_insert(tree, i, border[i]);
    }
    kdtree_balance(tree);

    /* Second pass, only for vertices that can move: the world-space distance to the nearest fixed
     * vertex. Not projected, since the fade follows the mask on the surface rather than the view. */
    prop.vert_mask_dist.reinitialize(verts_num);
    threading::EnumerableThreadSpecific<ProportionalNodeData> all_node_tls;
    node_mask.foreach_index(
        [&](const int i) {
          if (prop.node_min_dist[i] == FLT_MAX) {
            return;
          }
          ProportionalNodeData &data = all_node_tls.local();
          proportional_node_data_gather(object, pbvh, attribute_data_ptr, i, data);
          for (const int j : data.positions.index_range()) {
            float &mask_dist = prop.vert_mask_dist[data.vert_indices[j]];
            if (data.factors[j] <= 0.0f) {
              mask_dist = 0.0f;
              continue;
            }
            KDTreeNearest<float3> nearest;
            const float3 world_co = math::transform_point(object_to_world, data.positions[j]);
            mask_dist = (kdtree_find_nearest(tree, world_co, &nearest) != -1) ? nearest.dist :
                                                                                  FLT_MAX;
          }
        },
        exec_mode::grain_size(1));
    kdtree_free(tree);
  }

  prop.distances_valid = true;
  prop.distances_projected = projected;
  prop.distances_view_normal = view_normal;
}

static void proportional_distances_ensure(const Depsgraph &depsgraph,
                                          Object &object,
                                          const ePaintSymmetryFlags symm,
                                          filter::TransformProportional &prop)
{
  if (prop.distances_valid && prop.distances_projected == prop.projected &&
      (!prop.projected || prop.distances_view_normal == prop.view_normal))
  {
    return;
  }
  proportional_distances_build(depsgraph, object, symm, prop);
}

/**
 * Nodes to deform this step: those with a vertex inside the radius, plus the ones deformed on the
 * previous step, which were moved back by the undo restore and still need their bounds and draw
 * data refreshed.
 */
static IndexMask proportional_deform_nodes(const IndexMask &node_mask,
                                           filter::TransformProportional &prop,
                                           IndexMaskMemory &memory)
{
  const Span<float> node_min_dist = prop.node_min_dist;
  const Span<bool> prev_nodes = prop.prev_nodes;
  const IndexMask deform_mask = IndexMask::from_predicate(
      node_mask,
      memory,
      [&](const int i) {
        return node_min_dist[i] < prop.radius || prev_nodes.is_empty() || prev_nodes[i];
      },
      exec_mode::grain_size(1024));

  Array<bool> in_radius(node_min_dist.size(), false);
  deform_mask.foreach_index([&](const int i) { in_radius[i] = node_min_dist[i] < prop.radius; });
  prop.prev_nodes = std::move(in_radius);
  return deform_mask;
}

static void sculpt_transform_all_vertices(const Depsgraph &depsgraph, const Sculpt &sd, Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(ob);
  filter::TransformProportional &prop = ss.filter_cache->proportional;

  if (prop.enabled && !prop.prev_nodes.is_empty()) {
    /* Only the nodes moved on the previous step differ from the undo positions. Restoring (and so
     * tagging) every node would recompute normals and draw data for the whole mesh each step. */
    IndexMaskMemory restore_memory;
    undo::restore_position_from_undo_step(
        depsgraph, ob, IndexMask::from_bools(prop.prev_nodes, restore_memory));
  }
  else {
    undo::restore_position_from_undo_step(depsgraph, ob);
  }

  const TransformStepData step = transform_step_data_init(
      ob.object_to_world(), ss, symm, ss.filter_cache->transform_displacement_mode, prop.enabled);

  /* Regular transform applies all symmetry passes at once as it is split by symmetry areas
   * (each vertex can only be transformed once by the transform matrix of its area). */
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const IndexMask &node_mask = ss.filter_cache->node_mask;

  IndexMaskMemory mask_memory;
  IndexMask deform_mask = node_mask;
  if (prop.enabled) {
    proportional_distances_ensure(depsgraph, ob, symm, prop);
    deform_mask = proportional_deform_nodes(node_mask, prop, mask_memory);
  }
  else {
    /* Every node gets deformed, so a later proportional step has to refresh all of them. */
    prop.prev_nodes = Array<bool>();
  }

  threading::EnumerableThreadSpecific<TransformLocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(ob.data);
      const MeshAttributeData attribute_data(mesh);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const PositionDeformData position_data(depsgraph, ob);
      deform_mask.foreach_index(
          [&](const int i) {
            TransformLocalData &tls = all_tls.local();
            transform_node_mesh(sd, step, prop, attribute_data, nodes[i], ob, tls, position_data);
            bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *ob.runtime->sculpt_session->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      deform_mask.foreach_index(
          [&](const int i) {
            TransformLocalData &tls = all_tls.local();
            transform_node_grids(sd, step, prop, nodes[i], ob, tls);
            bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      deform_mask.foreach_index(
          [&](const int i) {
            TransformLocalData &tls = all_tls.local();
            transform_node_bmesh(sd, step, prop, nodes[i], ob, tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  pbvh.tag_positions_changed(deform_mask);
  pbvh.flush_bounds_to_parents();
}

BLI_NOINLINE static void calc_transform_translations(const float4x4 &elastic_transform_mat,
                                                     const Span<float3> positions,
                                                     const MutableSpan<float3> r_translations)
{
  for (const int i : positions.index_range()) {
    const float3 transformed = math::transform_point(elastic_transform_mat, positions[i]);
    r_translations[i] = transformed - positions[i];
  }
}

BLI_NOINLINE static void apply_kelvinet_to_translations(const Object &object,
                                                        const KelvinletParams &params,
                                                        const float3 &elastic_transform_pivot,
                                                        const Span<float3> positions,
                                                        const MutableSpan<float3> translations)
{
  if (!object_has_non_uniform_scale(object)) {
    for (const int i : positions.index_range()) {
      BKE_kelvinlet_grab_triscale(
          translations[i], &params, positions[i], elastic_transform_pivot, translations[i]);
    }
    return;
  }

  /* Non-uniform-scale path -- see #KelvinletWorldTransform. No #StrokeCache on this path (the
   * Transform tool's Elastic mode runs via the mesh-filter machinery, not a normal brush
   * stroke), so the gate above is a freshly-computed check rather than
   * `cache.non_uniform_scale_active`. `translations[i]` going in is the per-vertex linear-
   * transform displacement from `calc_transform_translations`; it is the `brush_delta` argument
   * to kelvinlet, transformed as a DIRECTION, and is OVERWRITTEN by the kelvinlet output (matches
   * the pre-existing overwrite semantics of the branch above). */
  const KelvinletWorldTransform transform = kelvinlet_world_transform_init(object);
  const float3 world_pivot = kelvinlet_position_to_world(transform, elastic_transform_pivot);
  for (const int i : positions.index_range()) {
    const float3 world_co = kelvinlet_position_to_world(transform, positions[i]);
    const float3 world_delta = kelvinlet_direction_to_world(transform, translations[i]);
    float3 world_disp;
    BKE_kelvinlet_grab_triscale(world_disp, &params, world_co, world_pivot, world_delta);
    translations[i] = kelvinlet_direction_to_local(transform, world_disp);
  }
}

static void elastic_transform_node_mesh(const Sculpt &sd,
                                        const KelvinletParams &params,
                                        const float4x4 &elastic_transform_mat,
                                        const float3 &elastic_transform_pivot,
                                        const MeshAttributeData &attribute_data,
                                        const bke::pbvh::MeshNode &node,
                                        Object &object,
                                        TransformLocalData &tls,
                                        const PositionDeformData &position_data)
{
  const SculptSession &ss = *object.runtime->sculpt_session;

  const Span<int> verts = node.verts();
  const MutableSpan positions = gather_data_mesh(position_data.eval, verts, tls.positions);

  /* TODO: Using the factors array is unnecessary when there are no hidden vertices and no mask. */
  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  scale_factors(factors, 20.0f);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_transform_translations(elastic_transform_mat, positions, translations);
  apply_kelvinet_to_translations(object, params, elastic_transform_pivot, positions, translations);

  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void elastic_transform_node_grids(const Sculpt &sd,
                                         const KelvinletParams &params,
                                         const float4x4 &elastic_transform_mat,
                                         const float3 &elastic_transform_pivot,
                                         const bke::pbvh::GridsNode &node,
                                         Object &object,
                                         TransformLocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  /* TODO: Using the factors array is unnecessary when there are no hidden vertices and no mask. */
  tls.factors.resize(positions.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  scale_factors(factors, 20.0f);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_transform_translations(elastic_transform_mat, positions, translations);
  apply_kelvinet_to_translations(object, params, elastic_transform_pivot, positions, translations);

  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void elastic_transform_node_bmesh(const Sculpt &sd,
                                         const KelvinletParams &params,
                                         const float4x4 &elastic_transform_mat,
                                         const float3 &elastic_transform_pivot,
                                         bke::pbvh::BMeshNode &node,
                                         Object &object,
                                         TransformLocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  scale_factors(factors, 20.0f);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_transform_translations(elastic_transform_mat, positions, translations);
  apply_kelvinet_to_translations(object, params, elastic_transform_pivot, positions, translations);

  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

static void transform_radius_elastic(const Depsgraph &depsgraph,
                                     const Sculpt &sd,
                                     Object &ob,
                                     const float transform_radius)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  BLI_assert(ss.filter_cache->transform_displacement_mode ==
             TransformDisplacementMode::Incremental);

  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(ob);

  std::array<float4x4, 8> transform_mats = transform_matrices_init(
      ob.object_to_world(), ss, symm, ss.filter_cache->transform_displacement_mode);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const IndexMask &node_mask = ss.filter_cache->node_mask;

  KelvinletParams params;
  /* TODO(pablodp606): These parameters can be exposed if needed as transform strength and volume
   * preservation like in the elastic deform brushes. Setting them to the same default as elastic
   * deform triscale grab because they work well in most cases. */
  const float force = 1.0f;
  const float shear_modulus = 1.0f;
  const float poisson_ratio = 0.4f;
  BKE_kelvinlet_init_params(&params, transform_radius, force, shear_modulus, poisson_ratio);

  threading::EnumerableThreadSpecific<TransformLocalData> all_tls;
  for (ePaintSymmetryFlags symmpass = PAINT_SYMM_NONE; symmpass <= symm; symmpass++) {
    if (!is_symmetry_iteration_valid(symmpass, symm)) {
      continue;
    }

    const float3 elastic_transform_pivot = symmetry_flip(ss.pivot_pos, symmpass);

    const int symm_area = get_vertex_symm_area(elastic_transform_pivot);
    float4x4 elastic_transform_mat = transform_mats[symm_area];
    switch (pbvh.type()) {
      case bke::pbvh::Type::Mesh: {
        Mesh &mesh = *id_cast<Mesh *>(ob.data);
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const PositionDeformData position_data(depsgraph, ob);
        const MeshAttributeData attribute_data(mesh);
        node_mask.foreach_index(
            [&](const int i) {
              TransformLocalData &tls = all_tls.local();
              elastic_transform_node_mesh(sd,
                                          params,
                                          elastic_transform_mat,
                                          elastic_transform_pivot,
                                          attribute_data,
                                          nodes[i],
                                          ob,
                                          tls,
                                          position_data);
              bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
            },
            exec_mode::grain_size(1));
        break;
      }
      case bke::pbvh::Type::Grids: {
        SubdivCCG &subdiv_ccg = *ob.runtime->sculpt_session->subdiv_ccg;
        MutableSpan<float3> positions = subdiv_ccg.positions;
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        node_mask.foreach_index(
            [&](const int i) {
              TransformLocalData &tls = all_tls.local();
              elastic_transform_node_grids(
                  sd, params, elastic_transform_mat, elastic_transform_pivot, nodes[i], ob, tls);
              bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
            },
            exec_mode::grain_size(1));
        break;
      }
      case bke::pbvh::Type::BMesh: {
        MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
        node_mask.foreach_index(
            [&](const int i) {
              TransformLocalData &tls = all_tls.local();
              elastic_transform_node_bmesh(
                  sd, params, elastic_transform_mat, elastic_transform_pivot, nodes[i], ob, tls);
              bke::pbvh::update_node_bounds_bmesh(nodes[i]);
            },
            exec_mode::grain_size(1));
        break;
      }
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

void update_modal_transform(bContext *C, Object &ob, bool is_active)
{
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  SculptSession &ss = *ob.runtime->sculpt_session;
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

  /* Origin Correct: a non-active object moves as a rigid body -- its own matrix follows the
   * shared pivot delta, mesh vertices are never touched -- instead of the normal mesh-deform
   * path below. See design spec §5 for why #transform_mats[0] (the unflipped/main symmetry area)
   * is the correct delta for the object's own origin. */
  if (!is_active && origin_correct_active_for_secondary(sd)) {
    /* Conjugate by the FIXED session-start matrix (the undo snapshot from
     * #undo::set_object_transform_snapshot), NEVER by `ob`'s live matrix -- this block mutates
     * `ob`'s own matrix every step via #BKE_object_apply_mat4 below, so reading it back for the
     * pivot/orientation math (#sync_local_pivot_from_matrix, #transform_matrices_init) would
     * compound a growing error each step: confirmed at runtime via `[ORIGDBG]` printf tracing
     * (since removed) that the object's rotation visibly oscillated/jittered while only slowly
     * advancing toward the gizmo's target, instead of tracking it smoothly. Falls back to the
     * live matrix only if no snapshot exists (should not normally happen --
     * #init_transform_add_object always captures one when this toggle is already on at session
     * start; defensive only, e.g. the toggle was flipped on mid-drag). */
    float4x4 orig_mat = ob.object_to_world();
    undo::get_object_transform_snapshot(ob, orig_mat);

    sync_local_pivot_from_matrix(ss, orig_mat);

    const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(ob);
    const std::array<float4x4, 8> transform_mats = transform_matrices_init(
        orig_mat, ss, symm, ss.filter_cache->transform_displacement_mode);
    const float4x4 new_world_mat = orig_mat * transform_mats[0];
    BKE_object_apply_mat4(&ob, new_world_mat.ptr(), false, true);
    DEG_id_tag_update(&ob.id, ID_RECALC_TRANSFORM);
    WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, &ob);
  }
  else {
    /* Blender's generic Transform system (Move/Rotate/Scale) drives #ss.transform_pivot_pos_world/
     * #transform_pivot_rot_world directly (see #createTransSculpt) -- refresh this object's LOCAL
     * pivot from that shared world value before doing any vertex math below, which reads
     * #ss.pivot_pos/#pivot_rot. Safe to read `ob`'s live matrix here: this branch never mutates
     * it, unlike the rigid-body branch above. */
    sync_local_pivot_from_world(ob);

    vert_random_access_ensure(ob);
    BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

    switch (sd.transform_mode) {
      case SCULPT_TRANSFORM_MODE_ALL_VERTICES: {
        sculpt_transform_all_vertices(*depsgraph, sd, ob);
        break;
      }
      case SCULPT_TRANSFORM_MODE_RADIUS_ELASTIC: {
        const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
        float transform_radius;

        if (BKE_brush_use_locked_size(&sd.paint, &brush)) {
          transform_radius = BKE_brush_unprojected_radius_get(&sd.paint, &brush);
        }
        else {
          ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

          transform_radius = paint_calc_object_space_radius(
              vc, ss.init_pivot_pos, BKE_brush_radius_get(&sd.paint, &brush));
        }

        transform_radius_elastic(*depsgraph, sd, ob, transform_radius);
        break;
      }
    }

    /* Per-object overload -- bit-exact with the single-object #flush_update_step(C, ...) wrapper
     * when `ob == CTX_data_active_object(C)`, but that wrapper always resolves to the ACTIVE
     * object, so calling it here for a non-active `ob` would silently flush the WRONG object.
     * For a Grids/multires object this is not just a redraw nicety: #multires_mark_as_modified
     * (called from the per-object overload below) is what extracts the interactively-edited
     * #SubdivCCG::positions runtime cache back into the persistent MDisps displacement -- without
     * it for THIS object, the next depsgraph evaluation recomputes multires displacement from the
     * unchanged MDisps data and the edit visually reverts (Mesh objects are unaffected because
     * their position edit already lives in the persistent Mesh vertex array itself). */
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    flush_update_step(vc, ob, UpdateType::Position);
  }

  copy_v3_v3(ss.prev_pivot_pos, ss.pivot_pos);
  copy_v4_v4(ss.prev_pivot_rot, ss.pivot_rot);
  copy_v3_v3(ss.prev_pivot_scale, ss.pivot_scale);
}

void cancel_modal_transform(bContext *C, Object &ob, bool is_active)
{
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  if (!is_active && origin_correct_active_for_secondary(sd)) {
    /* Rigid-body secondary: revert its matrix, not its mesh -- it was never touched. */
    undo::restore_object_transform_from_undo_step(ob);
    return;
  }

  /* Canceling "Elastic" transforms (due to its #TransformDisplacementMode::Incremental nature),
   * requires restoring positions from undo. For "All Vertices" there is no benefit in using the
   * transform system to update to original positions either. */
  Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  undo::restore_position_from_undo_step(depsgraph, ob);

  copy_v3_v3(ss.pivot_pos, ss.init_pivot_pos);
  copy_qt_qt(ss.pivot_rot, ss.init_pivot_rot);
  copy_v3_v3(ss.pivot_scale, ss.init_pivot_scale);
  sculpt_cursor_store_from_transform_pivot(C, ob, ss);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  bke::pbvh::update_normals(depsgraph, ob, pbvh);
  pbvh.update_bounds(depsgraph, ob);
}

void end_transform(bContext *C, Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  /* `pin_cursor` keeps the sculpt cursor fixed; otherwise it follows the transform pivot, the way
   * the regular sculpt pivot does. */
  const Scene *scene = CTX_data_scene(C);
  if (scene && !cursor::pin_get(*scene)) {
    sculpt_cursor_store_from_transform_pivot(C, ob, ss);
  }
  MEM_delete(ss.filter_cache);
  ss.filter_cache = nullptr;
  undo::push_end(ob);
  flush_update_done(C, ob, UpdateType::Position);
}

void end_transform(bContext *C, Span<Object *> objects)
{
  for (Object *ob : objects) {
    SculptSession &ss = *ob->runtime->sculpt_session;
    MEM_delete(ss.filter_cache);
    ss.filter_cache = nullptr;
  }
  undo::finish_multi_object(C, objects, UpdateType::Position);
}

enum class PivotPositionMode {
  Origin = 0,
  Unmasked = 1,
  MaskBorder = 2,
  ActiveVert = 3,
  CursorSurface = 4,
};

static EnumPropertyItem prop_sculpt_pivot_position_types[] = {
    {int(PivotPositionMode::Origin),
     "ORIGIN",
     0,
     "Origin",
     "Sets the pivot to the origin of the sculpt"},
    {int(PivotPositionMode::Unmasked),
     "UNMASKED",
     0,
     "Unmasked",
     "Sets the pivot position to the average position of the unmasked vertices"},
    {int(PivotPositionMode::MaskBorder),
     "BORDER",
     0,
     "Mask Border",
     "Sets the pivot position to the center of the border of the mask"},
    {int(PivotPositionMode::ActiveVert),
     "ACTIVE",
     0,
     "Active Vertex",
     "Sets the pivot position to the active vertex position"},
    {int(PivotPositionMode::CursorSurface),
     "SURFACE",
     0,
     "Surface",
     "Sets the pivot position to the surface under the cursor"},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool set_pivot_depends_on_cursor(bContext & /*C*/, wmOperatorType & /*ot*/, PointerRNA *ptr)
{
  if (!ptr) {
    return true;
  }
  const PivotPositionMode mode = PivotPositionMode(RNA_enum_get(ptr, "mode"));
  return ELEM(mode, PivotPositionMode::CursorSurface, PivotPositionMode::ActiveVert);
}

struct AveragePositionAccumulation {
  double3 position;
  double weight_total;
};

static AveragePositionAccumulation combine_average_position_accumulation(
    const AveragePositionAccumulation &a, const AveragePositionAccumulation &b)
{
  return AveragePositionAccumulation{a.position + b.position, a.weight_total + b.weight_total};
}

BLI_NOINLINE static void accumulate_weighted_average_position(const Span<float3> positions,
                                                              const Span<float> factors,
                                                              AveragePositionAccumulation &total)
{
  BLI_assert(positions.size() == factors.size());

  for (const int i : positions.index_range()) {
    total.position += double3(positions[i] * factors[i]);
    total.weight_total += factors[i];
  }
}

static float3 average_unmasked_position(const Depsgraph &depsgraph,
                                        const Object &object,
                                        const float3 &pivot,
                                        const ePaintSymmetryFlags symm)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::search_nodes(
      pbvh, memory, [&](const bke::pbvh::Node &node) {
        return !node_fully_masked_or_hidden(node);
      });

  struct LocalData {
    Vector<float> factors;
    Vector<float3> positions;
  };

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, object);
      const AveragePositionAccumulation total = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AveragePositionAccumulation{},
          [&](const IndexRange range, AveragePositionAccumulation sum) {
            LocalData &tls = all_tls.local();
            threading::isolate_task([&]() {
              node_mask.slice(range).foreach_index([&](const int i) {
                const Span<int> verts = nodes[i].verts();

                tls.positions.resize(verts.size());
                const MutableSpan<float3> positions = tls.positions;
                array_utils::gather(vert_positions, verts, positions);

                tls.factors.resize(verts.size());
                const MutableSpan<float> factors = tls.factors;
                fill_factor_from_hide_and_mask(
                    attribute_data.hide_vert, attribute_data.mask, verts, factors);
                filter_verts_outside_symmetry_area(positions, pivot, symm, factors);

                accumulate_weighted_average_position(positions, factors, sum);
              });
            });
            return sum;
          },
          combine_average_position_accumulation);
      return float3(math::safe_divide(total.position, total.weight_total));
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const AveragePositionAccumulation total = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AveragePositionAccumulation{},
          [&](const IndexRange range, AveragePositionAccumulation sum) {
            LocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              const Span<int> grids = nodes[i].grids();
              const MutableSpan positions = gather_grids_positions(
                  subdiv_ccg, grids, tls.positions);

              tls.factors.resize(positions.size());
              const MutableSpan<float> factors = tls.factors;
              fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
              filter_verts_outside_symmetry_area(positions, pivot, symm, factors);

              accumulate_weighted_average_position(positions, factors, sum);
            });
            return sum;
          },
          combine_average_position_accumulation);
      return float3(math::safe_divide(total.position, total.weight_total));
    }
    case bke::pbvh::Type::BMesh: {
      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      const AveragePositionAccumulation total = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AveragePositionAccumulation{},
          [&](const IndexRange range, AveragePositionAccumulation sum) {
            LocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(
                  &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
              const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

              tls.factors.resize(verts.size());
              const MutableSpan<float> factors = tls.factors;
              fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
              filter_verts_outside_symmetry_area(positions, pivot, symm, factors);

              accumulate_weighted_average_position(positions, factors, sum);
            });
            return sum;
          },
          combine_average_position_accumulation);
      return float3(math::safe_divide(total.position, total.weight_total));
    }
  }
  BLI_assert_unreachable();
  return float3(0);
}

BLI_NOINLINE static void mask_border_weight_calc(const Span<float> masks,
                                                 const MutableSpan<float> factors)
{
  constexpr float threshold = 0.2f;

  for (const int i : masks.index_range()) {
    if (std::abs(masks[i] - 0.5f) > threshold) {
      factors[i] = 0.0f;
    }
  }
};

static float3 average_mask_border_position(const Depsgraph &depsgraph,
                                           const Object &object,
                                           const float3 &pivot,
                                           const ePaintSymmetryFlags symm)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::search_nodes(
      pbvh, memory, [&](const bke::pbvh::Node &node) {
        return !node_fully_masked_or_hidden(node);
      });

  struct LocalData {
    Vector<float> factors;
    Vector<float> masks;
    Vector<float3> positions;
  };

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, object);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan mask_attr = *attributes.lookup_or_default<float>(
          ".sculpt_mask", bke::AttrDomain::Point, 0.0f);
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
      const AveragePositionAccumulation total = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AveragePositionAccumulation{},
          [&](const IndexRange range, AveragePositionAccumulation sum) {
            LocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              const Span<int> verts = nodes[i].verts();
              MutableSpan positions = gather_data_mesh(vert_positions, verts, tls.positions);
              MutableSpan masks = gather_data_mesh(mask_attr, verts, tls.masks);

              tls.factors.resize(verts.size());
              const MutableSpan<float> factors = tls.factors;
              fill_factor_from_hide(hide_vert, verts, factors);

              mask_border_weight_calc(masks, factors);
              filter_verts_outside_symmetry_area(positions, pivot, symm, factors);

              accumulate_weighted_average_position(positions, factors, sum);
            });
            return sum;
          },
          combine_average_position_accumulation);
      return float3(math::safe_divide(total.position, total.weight_total));
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const AveragePositionAccumulation total = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AveragePositionAccumulation{},
          [&](const IndexRange range, AveragePositionAccumulation sum) {
            LocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              const Span<int> grids = nodes[i].grids();
              const MutableSpan positions = gather_grids_positions(
                  subdiv_ccg, grids, tls.positions);

              tls.masks.resize(positions.size());
              const MutableSpan<float> masks = tls.masks;
              mask::gather_mask_grids(subdiv_ccg, grids, masks);

              tls.factors.resize(positions.size());
              const MutableSpan<float> factors = tls.factors;
              fill_factor_from_hide(subdiv_ccg, grids, factors);
              mask_border_weight_calc(masks, factors);
              filter_verts_outside_symmetry_area(positions, pivot, symm, factors);

              accumulate_weighted_average_position(positions, factors, sum);
            });
            return sum;
          },
          combine_average_position_accumulation);
      return float3(math::safe_divide(total.position, total.weight_total));
    }
    case bke::pbvh::Type::BMesh: {
      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      const AveragePositionAccumulation total = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AveragePositionAccumulation{},
          [&](const IndexRange range, AveragePositionAccumulation sum) {
            LocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(
                  &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
              const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

              tls.masks.resize(verts.size());
              const MutableSpan<float> masks = tls.masks;
              mask::gather_mask_bmesh(*ss.bm, verts, masks);

              tls.factors.resize(verts.size());
              const MutableSpan<float> factors = tls.factors;
              fill_factor_from_hide(verts, factors);
              mask_border_weight_calc(masks, factors);
              filter_verts_outside_symmetry_area(positions, pivot, symm, factors);

              accumulate_weighted_average_position(positions, factors, sum);
            });
            return sum;
          },
          combine_average_position_accumulation);
      return float3(math::safe_divide(total.position, total.weight_total));
    }
  }
  BLI_assert_unreachable();
  return float3(0);
}

static wmOperatorStatus set_pivot_position_exec(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  ARegion *region = CTX_wm_region(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(ob);

  const PivotPositionMode mode = PivotPositionMode(RNA_enum_get(op->ptr, "mode"));

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  switch (mode) {
    case PivotPositionMode::Origin:
      ss.pivot_pos = float3(0.0f);
      break;
    case PivotPositionMode::Unmasked:
      ss.pivot_pos = average_unmasked_position(*depsgraph, ob, ss.pivot_pos, symm);
      break;
    case PivotPositionMode::MaskBorder:
      ss.pivot_pos = average_mask_border_position(*depsgraph, ob, ss.pivot_pos, symm);
      break;
    case PivotPositionMode::ActiveVert: {
      const float2 mval(RNA_float_get(op->ptr, "mouse_x"), RNA_float_get(op->ptr, "mouse_y"));
      Object *hit_ob = nullptr;
      if (cursor_geometry_info_update(C, mval, false, &hit_ob)) {
        /* The cursor may be over a DIFFERENT sculpt-mode object than the active one --
         * #cursor_geometry_info_update already searches every selected object. Whichever
         * object was hit owns the active-vertex state read below, so its position must be
         * looked up (and converted) from THAT object, not silently reused as if it were the
         * active object's own local-space position (see #ED_sculpt.hh's #pivot_pos doc: the
         * shared pivot is always stored in the ACTIVE object's local space). */
        Object &vert_ob = hit_ob ? *hit_ob : ob;
        const SculptSession &vert_ss = *vert_ob.runtime->sculpt_session;
        const float3 world_location = math::transform_point(
            vert_ob.object_to_world(), vert_ss.active_vert_position(*depsgraph, vert_ob));
        ss.pivot_pos = math::transform_point(ob.world_to_object(), world_location);
      }
      break;
    }
    case PivotPositionMode::CursorSurface: {
      const float2 mval(RNA_float_get(op->ptr, "mouse_x"), RNA_float_get(op->ptr, "mouse_y"));
      ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
      const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
      const Brush *brush = BKE_paint_brush(BKE_paint_get_active_from_context(C));
      Object *hit_ob = nullptr;
      float3 stroke_location;
      if (stroke_get_location_bvh(
              *depsgraph, vc, &sd, brush, stroke_location, mval, false, &hit_ob))
      {
        /* #stroke_get_location_bvh searches every sculpt-mode object and returns the surface
         * point of WHICHEVER one is actually closest to the viewer under the cursor, in THAT
         * object's own local space -- it does not have to be the active object. Storing it
         * straight into `ss.pivot_pos` (the active object's local pivot) without converting
         * first silently misinterpreted a foreign object's local coordinates as the active
         * object's own, placing the pivot at the wrong point whenever the cursor was over a
         * non-active mesh. Route it through world space instead. */
        Object &hit_object = hit_ob ? *hit_ob : ob;
        const float3 world_location = math::transform_point(hit_object.object_to_world(),
                                                            stroke_location);
        ss.pivot_pos = math::transform_point(ob.world_to_object(), world_location);
      }
      break;
    }
  }

  /* Update the viewport navigation rotation origin. */
  Paint *paint = BKE_paint_get_active_from_context(C);
  bke::PaintRuntime *paint_runtime = paint->runtime;
  paint_runtime->average_stroke_accum = ss.pivot_pos;
  paint_runtime->average_stroke_counter = 1;
  paint_runtime->last_stroke_valid = true;

  ED_region_tag_redraw(region);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob.data);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus set_pivot_position_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  RNA_float_set(op->ptr, "mouse_x", event->mval[0]);
  RNA_float_set(op->ptr, "mouse_y", event->mval[1]);
  return set_pivot_position_exec(C, op);
}

static bool set_pivot_position_poll_property(const bContext * /*C*/,
                                             wmOperator *op,
                                             const PropertyRNA *prop)
{
  if (STRPREFIX(RNA_property_identifier(prop), "mouse_")) {
    const PivotPositionMode mode = PivotPositionMode(RNA_enum_get(op->ptr, "mode"));
    return ELEM(mode, PivotPositionMode::CursorSurface, PivotPositionMode::ActiveVert);
  }
  return true;
}

void SCULPT_OT_set_pivot_position(wmOperatorType *ot)
{
  ot->name = "Set Pivot Position";
  ot->idname = "SCULPT_OT_set_pivot_position";
  ot->description = "Sets the sculpt transform pivot position";

  ot->invoke = set_pivot_position_invoke;
  ot->exec = set_pivot_position_exec;
  ot->poll = sculpt_mode_poll_view3d;
  ot->depends_on_cursor = set_pivot_depends_on_cursor;
  ot->poll_property = set_pivot_position_poll_property;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_enum(ot->srna,
               "mode",
               prop_sculpt_pivot_position_types,
               int(PivotPositionMode::Unmasked),
               "Mode",
               "");

  RNA_def_float(ot->srna,
                "mouse_x",
                0.0f,
                0.0f,
                FLT_MAX,
                "Mouse Position X",
                "Position of the mouse used for \"Surface\" and \"Active Vertex\" mode",
                0.0f,
                10000.0f);
  RNA_def_float(ot->srna,
                "mouse_y",
                0.0f,
                0.0f,
                FLT_MAX,
                "Mouse Position Y",
                "Position of the mouse used for \"Surface\" and \"Active Vertex\" mode",
                0.0f,
                10000.0f);
}

}  // namespace blender::ed::sculpt_paint
