/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_clone_mesh_source.hh"

#include <cfloat>
#include <climits>

#include <cstdio>
#include <memory>

#include "DNA_brush_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BLI_index_mask.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_view3d.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint::clone_mesh {

/** A dyntopo surface churns its own topology, so a recorded frame could not keep matching it.
 * Same guard as the texture clone's #clone_dyntopo_active. Checked both when a record is
 * validated and when the stroke runtime is built, so a dyntopo source is refused wherever it
 * would be read. */
static bool clone_mesh_dyntopo_active(const Object *ob)
{
  if (ob == nullptr || ob->runtime == nullptr) {
    return false;
  }
  const SculptSession *session = ob->runtime->sculpt_session;
  return session != nullptr && session->bm != nullptr;
}

/* -------------------------------------------------------------------- */
/** \name Source points (runtime-only, not DNA).
 * \{ */

/** Source points of the session, keyed on the TARGET object's #ID.session_uid. Cleared on
 * file load. Keyed per target so every mesh of a global-sculpt stroke can carry its own
 * source, the same policy the texture clone uses for its per-object records. */
static Map<uint32_t, CloneMeshSourcePoint> &clone_mesh_source_points()
{
  static Map<uint32_t, CloneMeshSourcePoint> points;
  return points;
}

bool clone_mesh_source_point_is_valid(const CloneMeshSourcePoint &source)
{
  if (!source.is_valid) {
    return false;
  }
  if (source.source_object == nullptr || source.source_mesh == nullptr) {
    return false;
  }
  if (source.source_object_session_uid != source.source_object->id.session_uid) {
    return false;
  }
  if (source.source_mesh != id_cast<const Mesh *>(source.source_object->data)) {
    return false;
  }
  if (clone_mesh_dyntopo_active(source.source_object)) {
    /* Dyntopo was turned on after the source was picked; the frozen frame can no longer be
     * trusted against a surface that re-tessellates under the brush. */
    return false;
  }
  return true;
}

void clone_mesh_source_point_set(const Object &target_object,
                                 const Object &source_object,
                                 const float3 &co_source_local,
                                 const float3 &normal_source_local,
                                 const float3 frame_axes[3],
                                 std::shared_ptr<CloneMeshStrokeRuntime> surface)
{
  /* The record is stored under the TARGET object's uid and looked up under it too -- every
   * sculpted object carries its own source. Source == target (self-clone) is a legitimate,
   * supported setup: the stroke runtime snapshots the source positions, so the stamp cannot
   * feed on its own output. */
  CloneMeshSourcePoint point;
  point.co_source_local = co_source_local;
  point.normal_source_local = normal_source_local;
  point.frame_axes[0] = frame_axes[0];
  point.frame_axes[1] = frame_axes[1];
  point.frame_axes[2] = frame_axes[2];
  point.is_valid = true;
  point.surface = std::move(surface);
  point.source_object_session_uid = source_object.id.session_uid;
  point.source_object = &source_object;
  point.source_mesh = id_cast<const Mesh *>(source_object.data);
  clone_mesh_source_points().add_overwrite(target_object.id.session_uid, std::move(point));
}

const CloneMeshSourcePoint *clone_mesh_source_point_get(const Object *target_object)
{
  if (target_object == nullptr) {
    return nullptr;
  }
  const CloneMeshSourcePoint *point = clone_mesh_source_points().lookup_ptr(
      target_object->id.session_uid);
  if (point == nullptr || !clone_mesh_source_point_is_valid(*point)) {
    return nullptr;
  }
  return point;
}

CloneMeshSourcePoint *clone_mesh_source_point_get_for_write(const Object *target_object)
{
  if (target_object == nullptr) {
    return nullptr;
  }
  CloneMeshSourcePoint *point = clone_mesh_source_points().lookup_ptr(
      target_object->id.session_uid);
  if (point == nullptr || !clone_mesh_source_point_is_valid(*point)) {
    return nullptr;
  }
  return point;
}

void clone_mesh_source_point_reset(const Object *target_object)
{
  if (target_object != nullptr) {
    clone_mesh_source_points().remove(target_object->id.session_uid);
  }
}

void clone_mesh_source_points_clear_all()
{
  clone_mesh_source_points().clear();
}

static void clone_mesh_source_points_on_file_load(Main * /*bmain*/,
                                                  PointerRNA ** /*pointers*/,
                                                  int /*pointers_num*/,
                                                  void * /*arg*/)
{
  /* Every source names an object of the file being replaced. Session uids are not reused, so
   * the records could never match again -- they would only sit in the map for the rest of the
   * run. */
  clone_mesh_source_points_clear_all();
}

void clone_mesh_source_points_callbacks_register()
{
  /* The store is a list node: adding it twice would splice the callback list into a cycle.
   * Guard here rather than at the call site, so the function stays safe for any caller. */
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;

  static bCallbackFuncStore load_pre_cb{};
  load_pre_cb.func = clone_mesh_source_points_on_file_load;
  load_pre_cb.alloc = false;
  BKE_callback_add(&load_pre_cb, BKE_CB_EVT_LOAD_PRE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-stroke sampling runtime.
 * \{ */

CloneMeshStrokeRuntime *clone_mesh_stroke_runtime_ensure(CloneMeshStrokeRuntime *&owner,
                                                         const Object &target_object)
{
  /* The snapshot was taken when the source was set and belongs to the record; the stroke only
   * borrows it. */
  const CloneMeshSourcePoint *source = clone_mesh_source_point_get(&target_object);
  if (source == nullptr || source->surface == nullptr || !source->surface->is_valid) {
    owner = nullptr;
    return nullptr;
  }
  owner = source->surface.get();
  return owner;
}

void clone_mesh_stroke_runtime_free(CloneMeshStrokeRuntime *&runtime)
{
  runtime = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source surface snapshot.
 * \{ */

/** Copies the surface of \a source_object and builds the private BVH over the copy. A source in
 * Sculpt Mode is read from its original mesh, because that is where the sculpt brushes write
 * and the evaluated mesh only catches up later; any other source is read evaluated. */
/** Snapshot of a multires surface in Sculpt Mode: the displaced grids live in the session's
 * #SubdivCCG, the object's own mesh only holds the coarse cage. Every grid cell becomes two
 * triangles (the winding is irrelevant: the probes are two-sided and normals are oriented
 * towards the viewer where they matter); a cell with a hidden corner is left out, like the
 * hidden faces the picker refuses. */
static std::unique_ptr<CloneMeshStrokeRuntime> clone_mesh_surface_build_grids(
    const SubdivCCG &subdiv_ccg, const Object &source_object)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  const int cells_per_grid = (key.grid_size - 1) * (key.grid_size - 1);
  if (cells_per_grid <= 0 || subdiv_ccg.grids_num == 0) {
    return nullptr;
  }

  Vector<int> corner_verts;
  corner_verts.reserve(size_t(subdiv_ccg.grids_num) * size_t(cells_per_grid) * 6);
  for (const int grid : IndexRange(subdiv_ccg.grids_num)) {
    const int base = grid * key.grid_area;
    for (int y = 0; y < key.grid_size - 1; y++) {
      for (int x = 0; x < key.grid_size - 1; x++) {
        const int i0 = base + y * key.grid_size + x;
        const int i1 = i0 + 1;
        const int i2 = i0 + key.grid_size + 1;
        const int i3 = i0 + key.grid_size;
        if (!grid_hidden.is_empty()) {
          const int offset = i0 - base;
          if (grid_hidden[grid][offset] || grid_hidden[grid][offset + 1] ||
              grid_hidden[grid][offset + key.grid_size] ||
              grid_hidden[grid][offset + key.grid_size + 1])
          {
            continue;
          }
        }
        for (const int vert : {i0, i1, i2, i0, i2, i3}) {
          corner_verts.append(vert);
        }
      }
    }
  }
  const int tris_num = int(corner_verts.size() / 3);
  if (tris_num == 0) {
    return nullptr;
  }

  std::unique_ptr<CloneMeshStrokeRuntime> surface = std::make_unique<CloneMeshStrokeRuntime>();
  surface->source_object = &source_object;
  surface->source_object_session_uid = source_object.id.session_uid;
  surface->positions_snapshot = Array<float3>(subdiv_ccg.positions.as_span());
  surface->corner_verts_snapshot = Array<int>(corner_verts.as_span());
  surface->corner_tris_snapshot = Array<int3>(tris_num);
  Array<int> face_offsets(tris_num + 1);
  for (const int tri : IndexRange(tris_num)) {
    surface->corner_tris_snapshot[tri] = int3(3 * tri, 3 * tri + 1, 3 * tri + 2);
    face_offsets[tri] = tri;
  }
  face_offsets[tris_num] = tris_num;
  /* One "face" per triangle, so the full mask below takes the builder's path that never reads
   * the offsets. */
  surface->tree_data = bke::bvhtree_from_mesh_corner_tris_ex(
      surface->positions_snapshot.as_span(),
      OffsetIndices<int>(face_offsets.as_span()),
      surface->corner_verts_snapshot.as_span(),
      surface->corner_tris_snapshot.as_span(),
      IndexMask(tris_num));
  surface->is_valid = surface->tree_data.tree != nullptr;
  if (!surface->is_valid) {
    return nullptr;
  }
  return surface;
}

static std::unique_ptr<CloneMeshStrokeRuntime> clone_mesh_surface_build(
    const Depsgraph *depsgraph, const Object &source_object)
{
  if ((source_object.mode & OB_MODE_SCULPT) != 0 && source_object.runtime != nullptr &&
      source_object.runtime->sculpt_session != nullptr &&
      source_object.runtime->sculpt_session->subdiv_ccg != nullptr)
  {
    return clone_mesh_surface_build_grids(*source_object.runtime->sculpt_session->subdiv_ccg,
                                          source_object);
  }
  const Mesh *mesh = nullptr;
  if ((source_object.mode & OB_MODE_SCULPT) != 0) {
    mesh = id_cast<const Mesh *>(source_object.data);
  }
  else {
    const Object *object_eval = DEG_get_evaluated(depsgraph, &source_object);
    mesh = (object_eval != nullptr) ? BKE_object_get_evaluated_mesh(object_eval) : nullptr;
  }
  if (mesh == nullptr || mesh->corner_tris().is_empty() || mesh->verts_num == 0) {
    return nullptr;
  }

  std::unique_ptr<CloneMeshStrokeRuntime> surface = std::make_unique<CloneMeshStrokeRuntime>();
  surface->source_object = &source_object;
  surface->source_object_session_uid = source_object.id.session_uid;
  surface->positions_snapshot = Array<float3>(mesh->vert_positions());
  surface->corner_verts_snapshot = Array<int>(mesh->corner_verts());
  surface->corner_tris_snapshot = Array<int3>(mesh->corner_tris());
  /* The mask is over faces, not triangles. A full mask takes the builder's path that never
   * reads face offsets, so only the face count matters here. */
  surface->tree_data = bke::bvhtree_from_mesh_corner_tris_ex(
      surface->positions_snapshot.as_span(),
      mesh->faces(),
      surface->corner_verts_snapshot.as_span(),
      surface->corner_tris_snapshot.as_span(),
      IndexMask(mesh->faces_num));
  surface->is_valid = surface->tree_data.tree != nullptr;
  if (!surface->is_valid) {
    return nullptr;
  }
  return surface;
}

/** Area-weighted average plane of the snapshot triangles whose centroid lies within \a radius of
 * \a co. The stamp measures the source's form as heights above this plane, so a dent or a bump
 * reads as below or above it, instead of vanishing when the picked point happens to sit on the
 * dent's floor. The normal is oriented along \a view_normal.
 *
 * Candidates come from a BVH range query over the snapshot tree rather than a scan of every
 * triangle: the picker calls this once, but the Relative walk calls it on every dab. The query
 * reports a triangle as soon as its bounding box reaches into the radius, so the centroid
 * distance is still checked here. */
static bool clone_mesh_area_plane_calc(const CloneMeshStrokeRuntime &surface,
                                       const float3 &co,
                                       const float radius,
                                       const float3 &view_normal,
                                       float3 &r_center,
                                       float3 &r_normal)
{
  const float radius_sq = radius * radius;
  float3 normal_sum(0.0f);
  float3 center_sum(0.0f);
  float area_sum = 0.0f;
  BLI_bvhtree_range_query_cpp(*surface.tree_data.tree,
                              co,
                              radius,
                              [&](const int index, const float3 & /*leaf_co*/, float dist_sq) {
                                if (dist_sq > radius_sq) {
                                  return;
                                }
                                const int3 &tri = surface.corner_tris_snapshot[index];
                                const float3 &v0 = surface.positions_snapshot
                                                       [surface.corner_verts_snapshot[tri[0]]];
                                const float3 &v1 = surface.positions_snapshot
                                                       [surface.corner_verts_snapshot[tri[1]]];
                                const float3 &v2 = surface.positions_snapshot
                                                       [surface.corner_verts_snapshot[tri[2]]];
                                const float3 centroid = (v0 + v1 + v2) * (1.0f / 3.0f);
                                if (math::distance_squared(centroid, co) > radius_sq) {
                                  return;
                                }
                                /* Cross product length is twice the area: it weights both sums
                                 * without a square root. */
                                const float3 area_normal = math::cross(v1 - v0, v2 - v0);
                                const float area = math::length(area_normal);
                                normal_sum += area_normal;
                                center_sum += centroid * area;
                                area_sum += area;
                              });
  if (!(area_sum > 0.0f) || math::is_zero(normal_sum)) {
    return false;
  }
  r_center = center_sum / area_sum;
  r_normal = math::normalize(normal_sum);
  if (math::dot(r_normal, view_normal) < 0.0f) {
    r_normal = -r_normal;
  }
  return true;
}

/** \} */

float clone_mesh_probe_top_get(const CloneMeshStrokeRuntime &runtime,
                               const float3 &origin,
                               const float3 &axis,
                               const float min_top)
{
  float bb_min[3];
  float bb_max[3];
  BLI_bvhtree_get_bounding_box(runtime.tree_data.tree, bb_min, bb_max);
  float top = min_top;
  for (int corner = 0; corner < 8; corner++) {
    const float3 co((corner & 1) ? bb_max[0] : bb_min[0],
                    (corner & 2) ? bb_max[1] : bb_min[1],
                    (corner & 4) ? bb_max[2] : bb_min[2]);
    top = std::max(top, math::dot(co - origin, axis));
  }
  /* A small margin keeps the ray origin strictly outside the box. */
  return top + 1e-4f * std::max(top, 1.0f);
}


/* -------------------------------------------------------------------- */
/** \name Relative walk on the source surface.
 * \{ */

float3 clone_mesh_dab_source_patch_get(CloneMeshSourcePoint &source,
                                       const CloneMeshStrokeRuntime &runtime,
                                       const CloneMeshDabSourceParams &params,
                                       float3 r_frame[3])
{
  const auto answer_frozen = [&]() {
    r_frame[0] = source.frame_axes[0];
    r_frame[1] = source.frame_axes[1];
    r_frame[2] = source.frame_axes[2];
    return source.co_source_local;
  };

  if (params.clone_mode != CLONE_MODE_RELATIVE) {
    return answer_frozen();
  }
  if (!source.walk_valid) {
    if (!params.is_main_pass) {
      /* A mirrored pass must never seed the walk: the offset would be fixed in the mirrored
       * half's terms (the texture clone has the same rule). Answer with the frozen pick until
       * the main pass comes along -- symmetry pass 0 always runs first, so this is at most one
       * dab. */
      return answer_frozen();
    }
    /* Handshake: seed the walk from the frozen pick at the first main-pass dab painted after
     * the source was set. */
    source.walk_co_source_local = source.co_source_local;
    source.walk_frame[0] = source.frame_axes[0];
    source.walk_frame[1] = source.frame_axes[1];
    source.walk_frame[2] = source.frame_axes[2];
    source.walk_prev_target_world = params.dab_center_world;
    source.walk_valid = true;
    printf("[CloneMesh] walk: seeded at pick (%g %g %g)\n",
           source.walk_co_source_local.x,
           source.walk_co_source_local.y,
           source.walk_co_source_local.z);
    r_frame[0] = source.walk_frame[0];
    r_frame[1] = source.walk_frame[1];
    r_frame[2] = source.walk_frame[2];
    return source.walk_co_source_local;
  }
  if (!params.is_main_pass) {
    /* Mirror and tile passes read the main pass's walk result of this step and never move it
     * (the same rule the anchor handshake follows). */
    r_frame[0] = source.walk_frame[0];
    r_frame[1] = source.walk_frame[1];
    r_frame[2] = source.walk_frame[2];
    return source.walk_co_source_local;
  }

  /* Brush travel since the previous main-pass dab, decomposed in the destination tangents
   * (world units) and converted to source-local units along the patch's own tangent axes. */
  const float3 step_world = params.dab_center_world - source.walk_prev_target_world;
  const float a_world = math::dot(step_world, params.t1_world);
  const float b_world = math::dot(step_world, params.t2_world);
  const float4x4 source_to_world = runtime.source_object->object_to_world();
  const float len_s1 = math::length(
      math::transform_direction(source_to_world, source.walk_frame[0]));
  const float len_s2 = math::length(
      math::transform_direction(source_to_world, source.walk_frame[1]));
  float3 moved = source.walk_co_source_local;
  if (len_s1 > 1e-12f && len_s2 > 1e-12f) {
    moved += (a_world / len_s1) * source.walk_frame[0] + (b_world / len_s2) * source.walk_frame[1];
  }

  /* Pull the moved point back onto the source surface: one probe ray from above the moved
   * point along the patch normal (the same top-down scheme the dabs sample heights with), and
   * the nearest surface point when the ray misses -- the patch may have walked past an edge or
   * a hole, and the nearest point keeps it glued to the surface instead of adrift. */
  float3 snap_co = moved;
  const char *snap_kind = "nearest";
  BVHTreeRayHit hit{};
  /* The ray starts above the whole source, so a walk plane that dips into the surface cannot
   * start it inside the mesh. */
  const float walk_top = clone_mesh_probe_top_get(
      runtime, moved, source.walk_frame[2], params.probe_limit);
  hit.dist = walk_top + params.probe_limit;
  hit.index = -1;
  BLI_bvhtree_ray_cast(runtime.tree_data.tree,
                       moved + source.walk_frame[2] * walk_top,
                       -source.walk_frame[2],
                       0.0f,
                       &hit,
                       runtime.tree_data.raycast_callback,
                       const_cast<bke::BVHTreeFromMesh *>(&runtime.tree_data));
  if (hit.index != -1) {
    snap_co = float3(hit.co);
    snap_kind = "ray";
  }
  else {
    BVHTreeNearest nearest{};
    nearest.dist_sq = FLT_MAX;
    nearest.index = -1;
    BLI_bvhtree_find_nearest(runtime.tree_data.tree,
                             moved,
                             &nearest,
                             runtime.tree_data.nearest_callback,
                             const_cast<bke::BVHTreeFromMesh *>(&runtime.tree_data));
    if (nearest.index == -1) {
      snap_kind = "miss";
    }
    else {
      snap_co = float3(nearest.co);
    }
  }

  /* Fresh local average NORMAL around the snapped point. The walk point stays ON the surface
   * (the snap hit): projecting it onto the average plane would sink the stamp plane into the
   * mesh under convex spots and make the stamp read large-scale shape as relief. */
  float3 plane_center;
  float3 plane_normal;
  float3 new_co = snap_co;
  if (clone_mesh_area_plane_calc(runtime,
                                 snap_co,
                                 params.radius_source_local,
                                 source.walk_frame[2],
                                 plane_center,
                                 plane_normal))
  {
    /* Carry the frame over by the minimal rotation onto the new plane normal: the stamp's
     * orientation tracks the surface without jumps. */
    float quat[4];
    rotation_between_vecs_to_quat(quat, source.walk_frame[2], plane_normal);
    const math::Quaternion rotation(quat[0], quat[1], quat[2], quat[3]);
    float3 axis_z = math::normalize(math::transform_point(rotation, source.walk_frame[2]));
    float3 axis_x = math::transform_point(rotation, source.walk_frame[0]);
    axis_x = math::normalize(axis_x - math::dot(axis_x, axis_z) * axis_z);
    source.walk_frame[0] = axis_x;
    source.walk_frame[1] = math::normalize(math::cross(axis_z, axis_x));
    source.walk_frame[2] = axis_z;
  }
  else {
    /* No source triangles within the radius (the patch walked onto a hole): keep the previous
     * frame; the point stays on the snapped surface. */
    printf("[CloneMesh] walk: no plane within the radius, previous plane kept\n");
  }

  source.walk_co_source_local = new_co;
  source.walk_prev_target_world = params.dab_center_world;
  printf("[CloneMesh] walk: step=(%g %g) snap=%s co=(%g %g %g) n=(%g %g %g)\n",
         a_world,
         b_world,
         snap_kind,
         new_co.x,
         new_co.y,
         new_co.z,
         source.walk_frame[2].x,
         source.walk_frame[2].y,
         source.walk_frame[2].z);

  r_frame[0] = source.walk_frame[0];
  r_frame[1] = source.walk_frame[1];
  r_frame[2] = source.walk_frame[2];
  return source.walk_co_source_local;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source picking (3D Viewport).
 * \{ */

/** Whether the picked face of this mesh is hidden -- the same guard #clone_pick_face applies,
 * since the corner-tris BVH contains hidden faces too. */
static bool clone_mesh_pick_face_hidden(const Mesh &mesh_eval, const int face_index)
{
  const VArray<bool> hide_poly = *mesh_eval.attributes().lookup_or_default<bool>(
      ".hide_poly", bke::AttrDomain::Face, false);
  return hide_poly[face_index];
}

/** Raycasts one visible mesh object with the view segment, in world space. Returns the hit
 * nearest along the ray, or false. */
static bool clone_mesh_pick_object(ViewContext &vc,
                                   const int mval[2],
                                   const Object &object,
                                   const Mesh &mesh_eval,
                                   float3 &r_co_world,
                                   float3 &r_no_world,
                                   float &r_depth,
                                   int &r_face_index)
{
  if (mesh_eval.faces_num == 0) {
    return false;
  }
  float3 start_world;
  float3 end_world;
  ED_view3d_win_to_segment_clipped(
      vc.depsgraph, vc.region, vc.v3d, float2(mval[0], mval[1]), start_world, end_world, true);
  const float3 ray_direction = math::normalize(end_world - start_world);

  const float4x4 &world_to_object = object.world_to_object();
  const float3 start_local = math::transform_point(world_to_object, start_world);
  const float3 direction_local = math::transform_direction(world_to_object, ray_direction);

  bke::BVHTreeFromMesh mesh_bvh = mesh_eval.bvh_corner_tris();
  if (mesh_bvh.tree == nullptr) {
    return false;
  }
  BVHTreeRayHit ray_hit{};
  ray_hit.dist = FLT_MAX;
  ray_hit.index = -1;
  BLI_bvhtree_ray_cast(mesh_bvh.tree,
                       start_local,
                       math::normalize(direction_local),
                       0.0f,
                       &ray_hit,
                       mesh_bvh.raycast_callback,
                       &mesh_bvh);
  if (ray_hit.index == -1) {
    return false;
  }
  r_face_index = mesh_eval.corner_tri_faces()[ray_hit.index];
  if (clone_mesh_pick_face_hidden(mesh_eval, r_face_index)) {
    return false;
  }
  r_co_world = math::transform_point(object.object_to_world(), float3(ray_hit.co));
  r_no_world = math::transform_direction(object.object_to_world(), float3(ray_hit.no));
  /* World-space depth: the parametric hit distance is local to each object, and several
   * objects' hits are compared against each other. */
  r_depth = math::distance(start_world, r_co_world);
  return true;
}

/** Builds the frozen stamp frame at a picked point: screen right and up flattened onto the
 * surface, all in the SOURCE object's local space. See #CloneMeshSourcePoint.frame_axes. */
static void clone_mesh_source_frame_build(const Object &source_object,
                                          const float3 &normal_local,
                                          const float4x4 &view_to_world,
                                          float3 r_frame_axes[3])
{
  const float4x4 &world_to_source = source_object.world_to_object();

  float3 axis_z = math::normalize(normal_local);

  const float4x4 view_to_source = world_to_source * view_to_world;
  float3 axis_x = math::transform_direction(view_to_source, float3(1.0f, 0.0f, 0.0f));
  axis_x -= math::dot(axis_x, axis_z) * axis_z;
  if (math::length(axis_x) < 1e-6f) {
    /* Looking straight down the surface normal: the screen right carries no tangent
     * component. The screen up does (or the pick is meaningless either way). */
    axis_x = math::transform_direction(view_to_source, float3(0.0f, 1.0f, 0.0f));
    axis_x -= math::dot(axis_x, axis_z) * axis_z;
  }
  if (math::length(axis_x) < 1e-6f) {
    axis_x = math::normalize(math::cross(axis_z, float3(0.0f, 0.0f, 1.0f)));
    if (math::length(axis_x) < 1e-6f) {
      axis_x = float3(1.0f, 0.0f, 0.0f);
    }
  }
  axis_x = math::normalize(axis_x);
  const float3 axis_y = math::normalize(math::cross(axis_z, axis_x));

  r_frame_axes[0] = axis_x;
  r_frame_axes[1] = axis_y;
  r_frame_axes[2] = axis_z;
}

bool clone_mesh_symmetry_mode_active(const Object &symmetry_reference_object)
{
  if (symmetry_reference_object.type != OB_MESH || symmetry_reference_object.data == nullptr) {
    return false;
  }
  if (mesh_symmetry_xyz_get(symmetry_reference_object) != 0) {
    return true;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(symmetry_reference_object.data);
  return mesh.radial_symmetry[0] > 1 || mesh.radial_symmetry[1] > 1 || mesh.radial_symmetry[2] > 1;
}

const CloneMeshSourcePoint *clone_mesh_brush_source_update(
    std::shared_ptr<CloneMeshBrushSource> &slot,
    const Depsgraph *depsgraph,
    const Object &primary,
    const float3 &co_local,
    const float radius_local,
    const float4x4 &view_to_world)
{
  if (slot == nullptr) {
    /* First dab of the stroke: freeze the surface now, so the mirror passes (which can overlap
     * the read area near the symmetry plane) never feed on their own output. */
    slot = std::make_shared<CloneMeshBrushSource>();
    slot->point.surface = clone_mesh_surface_build(depsgraph, primary);
  }
  CloneMeshSourcePoint &point = slot->point;
  if (point.surface == nullptr || !point.surface->is_valid) {
    return nullptr;
  }
  const CloneMeshStrokeRuntime &surface = *point.surface;

  /* The brush hit lies on the live surface; the snapshot may differ by this stroke's own
   * displacement, so stand on the snapshot. */
  BVHTreeNearest nearest{};
  nearest.dist_sq = FLT_MAX;
  nearest.index = -1;
  BLI_bvhtree_find_nearest(surface.tree_data.tree,
                           co_local,
                           &nearest,
                           surface.tree_data.nearest_callback,
                           const_cast<bke::BVHTreeFromMesh *>(&surface.tree_data));
  if (nearest.index == -1) {
    return nullptr;
  }
  const float3 co_snapped = float3(nearest.co);

  /* Normal oriented towards the viewer, averaged over half the brush radius like the picker. */
  const float3 to_viewer = math::normalize(
      math::transform_direction(primary.world_to_object(), view_to_world.z_axis()));
  float3 plane_center;
  float3 normal = float3(nearest.no);
  if (math::dot(normal, to_viewer) < 0.0f) {
    normal = -normal;
  }
  float3 plane_normal;
  if (clone_mesh_area_plane_calc(surface, co_snapped, radius_local * 0.5f, to_viewer, plane_center,
                                 plane_normal))
  {
    normal = plane_normal;
  }

  float3 frame_axes[3];
  clone_mesh_source_frame_build(primary, normal, view_to_world, frame_axes);

  point.co_source_local = co_snapped;
  point.normal_source_local = normal;
  point.frame_axes[0] = frame_axes[0];
  point.frame_axes[1] = frame_axes[1];
  point.frame_axes[2] = frame_axes[2];
  point.is_valid = true;
  point.source_object_session_uid = primary.id.session_uid;
  point.source_object = &primary;
  point.source_mesh = id_cast<const Mesh *>(primary.data);
  return &point;
}

/** One-shot picker: raycasts every visible mesh object (the active one included -- cloning
 * from a mesh onto itself is a legitimate setup) and freezes the nearest hit as the source
 * of the ACTIVE object. */
static wmOperatorStatus clone_mesh_source_set_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || ob == nullptr) {
    return OPERATOR_CANCELLED;
  }

  int mval[2];
  RNA_int_get_array(op->ptr, "location", mval);

  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  if (vc.v3d == nullptr || vc.region == nullptr || vc.view_layer == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Clone Stamp Mesh source requires the 3D Viewport");
    return OPERATOR_CANCELLED;
  }

  ListBaseT<Base> *bases = BKE_view_layer_object_bases_get(vc.view_layer);
  if (bases == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bool hit_found = false;
  const Object *hit_object = nullptr;
  const Mesh *hit_mesh_eval = nullptr;
  float3 hit_co_world(0.0f);
  float3 hit_no_world(0.0f);
  float hit_depth = FLT_MAX;
  for (Base &base : *bases) {
    Object *object = base.object;
    if (object == nullptr || object->type != OB_MESH || object->data == nullptr) {
      continue;
    }
    if (!BKE_base_is_visible(vc.v3d, &base)) {
      continue;
    }
    Object *object_eval = DEG_get_evaluated(depsgraph, object);
    if (object_eval == nullptr) {
      continue;
    }
    const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(object_eval);
    if (mesh_eval == nullptr) {
      continue;
    }
    float3 co_world;
    float3 no_world;
    float depth;
    int face_index;
    if (!clone_mesh_pick_object(
            vc, mval, *object_eval, *mesh_eval, co_world, no_world, depth, face_index))
    {
      continue;
    }
    if (depth < hit_depth) {
      hit_found = true;
      hit_object = object;
      hit_mesh_eval = mesh_eval;
      hit_co_world = co_world;
      hit_no_world = no_world;
      hit_depth = depth;
    }
  }

  if (!hit_found || hit_object == nullptr || hit_mesh_eval == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Clone Stamp Mesh: no surface under the cursor");
    return OPERATOR_CANCELLED;
  }
  if (clone_mesh_dyntopo_active(hit_object)) {
    /* A dyntopo source re-tessellates under the brush, so neither the frozen frame nor any
     * snapshot could keep matching it -- refuse the pick instead of recording something the
     * stroke would have to reject later. */
    BKE_report(op->reports,
               RPT_WARNING,
               "Clone Stamp Mesh: source is not supported in dynamic topology mode");
    return OPERATOR_CANCELLED;
  }

  /* Orient the stored normal towards the viewer, so the probe axis leaves the surface the
   * user is looking at and not the far side of a thin wall. */
  float3 start_world;
  float3 end_world;
  ED_view3d_win_to_segment_clipped(
      vc.depsgraph, vc.region, vc.v3d, float2(mval[0], mval[1]), start_world, end_world, true);
  const float3 ray_direction = math::normalize(end_world - start_world);
  if (math::dot(hit_no_world, ray_direction) > 0.0f) {
    hit_no_world = -hit_no_world;
  }

  /* Freeze the source surface now: the snapshot is what every later dab reads. */
  std::unique_ptr<CloneMeshStrokeRuntime> surface = clone_mesh_surface_build(vc.depsgraph,
                                                                             *hit_object);
  if (surface == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Clone Stamp Mesh: source mesh has no usable surface");
    return OPERATOR_CANCELLED;
  }

  const float4x4 &world_to_source = hit_object->world_to_object();
  float3 co_source_local = math::transform_point(world_to_source, hit_co_world);
  float3 normal_source_local = math::normalize(
      math::transform_direction(world_to_source, hit_no_world));

  /* Re-pick on the snapshot itself: the picker raycasts the evaluated mesh, which may differ
   * from the sculpted original the snapshot was taken from. */
  {
    const float3 start_local = math::transform_point(world_to_source, start_world);
    const float3 direction_local = math::normalize(
        math::transform_direction(world_to_source, ray_direction));
    BVHTreeRayHit snapshot_hit{};
    snapshot_hit.dist = FLT_MAX;
    snapshot_hit.index = -1;
    BLI_bvhtree_ray_cast(surface->tree_data.tree,
                         start_local,
                         direction_local,
                         0.0f,
                         &snapshot_hit,
                         surface->tree_data.raycast_callback,
                         &surface->tree_data);
    if (snapshot_hit.index != -1) {
      co_source_local = float3(snapshot_hit.co);
      normal_source_local = math::normalize(float3(snapshot_hit.no));
      if (math::dot(normal_source_local, direction_local) > 0.0f) {
        normal_source_local = -normal_source_local;
      }
    }
  }

  /* The stamp plane: tangent to the source surface AT the picked point, with the normal
   * averaged over a small neighborhood so a single noisy face does not tilt it. The plane
   * keeps passing through the real hit: shifting it to the area's average height (as an average
   * plane would) sinks it into the mesh under a convex spot -- the stamp would then read the
   * surface's large-scale shape as relief, and re-picking the same point would give a different
   * plane for every brush size and view distance. */
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = (paint != nullptr) ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush != nullptr) {
    const float pixel_radius = float(BKE_brush_size_get(paint, brush));
    const float radius_world = ED_view3d_pixel_size(vc.rv3d, hit_co_world) * pixel_radius;
    const float scale = math::length(math::transform_direction(world_to_source, ray_direction));
    /* Half the brush radius: wide enough to average out face noise, narrow enough that the
     * large-scale curvature of the source does not bend the normal away from the hit. */
    constexpr float normal_radius_factor = 0.5f;
    float3 plane_center;
    float3 plane_normal;
    if (clone_mesh_area_plane_calc(*surface,
                                   co_source_local,
                                   radius_world * scale * normal_radius_factor,
                                   normal_source_local,
                                   plane_center,
                                   plane_normal))
    {
      normal_source_local = plane_normal;
    }
  }

  float3 frame_axes[3];
  clone_mesh_source_frame_build(
      *hit_object, normal_source_local, float4x4(vc.rv3d->viewinv), frame_axes);

  printf("[CloneMesh] pick: source_is_target=%d co=(%g %g %g) n=(%g %g %g) s1=(%g %g %g) "
         "s2=(%g %g %g) s3=(%g %g %g)\n",
         int(hit_object == ob), co_source_local.x, co_source_local.y, co_source_local.z,
         normal_source_local.x, normal_source_local.y, normal_source_local.z,
         frame_axes[0].x, frame_axes[0].y, frame_axes[0].z,
         frame_axes[1].x, frame_axes[1].y, frame_axes[1].z,
         frame_axes[2].x, frame_axes[2].y, frame_axes[2].z);
  /* Every object of a multi-object sculpt carries its own record: the stroke runs the brush on
   * each of them, and one without a record would be skipped. They share the snapshot, but keep
   * their own Relative walk. */
  const std::shared_ptr<CloneMeshStrokeRuntime> shared_surface = std::move(surface);
  clone_mesh_source_point_set(
      *ob, *hit_object, co_source_local, normal_source_local, frame_axes, shared_surface);
  for (Object *target : sculpt_mode_objects(vc)) {
    if (target != ob && target->type == OB_MESH) {
      clone_mesh_source_point_set(
          *target, *hit_object, co_source_local, normal_source_local, frame_axes, shared_surface);
    }
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  BKE_report(op->reports, RPT_INFO, "Clone Stamp Mesh source set");
  return OPERATOR_FINISHED;
}

static wmOperatorStatus clone_mesh_source_set_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  if (region != nullptr && event != nullptr) {
    RNA_int_set_array(op->ptr, "location", event->mval);
  }
  return clone_mesh_source_set_exec(C, op);
}

static bool clone_mesh_source_poll_common(bContext *C)
{
  /* NOTE: no dyntopo guard on the TARGET here, on purpose -- unlike the texture clone, this
   * brush has a BMesh PBVH branch and holds no target topology of its own (only the SOURCE's
   * snapshot), so a dyntopo target is stampable. A dyntopo SOURCE is what is refused, in
   * #clone_mesh_source_point_is_valid and in the picker. */
  if (G.background) {
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  if (scene == nullptr || ob == nullptr || ob->data == nullptr) {
    return false;
  }
  if (ob->type != OB_MESH) {
    return false;
  }
  if (ob->mode != OB_MODE_SCULPT) {
    CTX_wm_operator_poll_msg_set(C, "Not in Sculpt Mode");
    return false;
  }
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = (paint != nullptr) ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush == nullptr || brush->sculpt_brush_type != SCULPT_BRUSH_TYPE_CLONE_MESH) {
    CTX_wm_operator_poll_msg_set(C, "Active brush is not a Clone Stamp Mesh brush");
    return false;
  }
  return true;
}

static bool clone_mesh_source_set_poll(bContext *C)
{
  if (!clone_mesh_source_poll_common(C)) {
    return false;
  }
  return true;
}

static wmOperatorStatus clone_mesh_source_reset_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  clone_mesh_source_point_reset(ob);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  if (vc.scene != nullptr && vc.view_layer != nullptr) {
    for (const Object *target : sculpt_mode_objects(vc)) {
      clone_mesh_source_point_reset(target);
    }
  }
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  return OPERATOR_FINISHED;
}

static bool clone_mesh_source_reset_poll(bContext *C)
{
  return clone_mesh_source_poll_common(C);
}

void PAINT_OT_clone_mesh_source_set(wmOperatorType *ot)
{
  ot->name = "Set Clone Stamp Mesh Source";
  ot->idname = "PAINT_OT_clone_mesh_source_set";
  ot->description =
      "Set the object and point the Clone Stamp Mesh brush copies the surface form from "
      "(Shift+LMB in the 3D Viewport)";

  ot->exec = clone_mesh_source_set_exec;
  ot->invoke = clone_mesh_source_set_invoke;
  ot->poll = clone_mesh_source_set_poll;

  /* One-shot picker, no modal loop. No OPTYPE_UNDO, matching #PAINT_OT_clone_source_set:
   * picking is setting a tool up, not editing the file, and the source is session state, so
   * there is nothing to undo there. */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_int_vector(
      ot->srna, "location", 2, nullptr, 0, INT_MAX, "Location", "", 0, 16384);
  RNA_def_property_flag(prop, (PROP_SKIP_SAVE | PROP_HIDDEN));
}

void PAINT_OT_clone_mesh_source_reset(wmOperatorType *ot)
{
  ot->name = "Reset Clone Stamp Mesh Source";
  ot->idname = "PAINT_OT_clone_mesh_source_reset";
  ot->description = "Clear the Clone Stamp Mesh source point and its Relative offset";

  ot->exec = clone_mesh_source_reset_exec;
  ot->poll = clone_mesh_source_reset_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::clone_mesh
