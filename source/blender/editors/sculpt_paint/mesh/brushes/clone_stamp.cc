/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Clone Stamp Mesh brush stamps the surface form of a SOURCE object onto the mesh under
 * the brush, like a physical stamp pressed into clay. The form it carries is the source's
 * height field relative to the tangent plane of a frozen frame picked on the source, sampled
 * per vertex through a stroke-frozen BVH (see #clone_mesh::CloneMeshStrokeRuntime).
 *
 * The destination side has its own reference: every dab measures against the area average of
 * the STROKE-START surface around the dab (#dab_orig_plane_calc, per symmetry pass), and the
 * dab center is the surface hit projected onto that plane. Reading the live surface instead
 * would let every dab re-choose the reference its own previous dabs had displaced (drift);
 * freezing one plane for the whole stroke would press dabs far from the stroke's beginning
 * flat against it (slices on curved targets). Heights on the Relative mode are additionally
 * fixed at a vertex's first touch (see the apply notes below), so overlapping dabs converge
 * instead of re-stamping on top of themselves.
 *
 * Modes (#Paint.clone_mode, shared with the texture Clone Stamp):
 *  - Absolute: every dab reads the same frozen source patch.
 *  - Relative: the source patch travels with the brush, WALKING the source's own surface -- the
 *    brush travel is applied along the patch's tangents, the patch is snapped back onto the
 *    source surface and onto a fresh local average plane, and its frame follows by a minimal
 *    rotation (see #clone_mesh::clone_mesh_dab_source_patch_get). The walk is seeded from the
 *    frozen pick by the first main-pass dab after the source was set (the anchor handshake).
 *
 * Application (#BrushCloneStampMeshSettings.apply_mode):
 *  - Imprint: the dab displaces by (source height - current target height), converging the
 *    region onto the stamped form.
 *  - Additive: the dab displaces by the source height alone, building it on top like an
 *    alpha or VDM brush.
 *
 * In Relative mode both apply with the Layer brush's overlap-free pattern: a vertex's target
 * offset is fixed on its FIRST touch, measured from its stroke-start position, and the applied
 * strength is the largest seen this stroke (#StrokeCache.clone_mesh_factor / #clone_mesh_target).
 * A slow stroke with tight spacing and a fast one reach the same result, and re-covering a
 * vertex never re-stamps it. Absolute keeps the plain converging re-stamp -- repeating one
 * frozen stamp is its meaning. BMesh targets (unstable per-vertex indices under dynamic
 * topology) fall back to the accumulating application.
 *
 * With symmetry (mirror or radial) on, the brush itself is the source: the main pass, under the
 * cursor, only reads -- it refreshes the source point and frame from the surface under the brush
 * (#clone_mesh::clone_mesh_brush_source_update, snapshot frozen at stroke start) and stamps
 * nothing -- and the mirror/radial passes stamp it onto their side. Tangential offsets are
 * measured in the pass-mirrored destination frame while the source frame stays the main pass's,
 * so the content comes out mirrored (`delta' = R * delta(R * d')`) without ever reflecting the
 * source data. The picked source is not used then. In a multi-object stroke the source lives on
 * the object under the cursor and the passes write onto whichever mesh the mirror reaches.
 *
 * Error policy -- ignore, never fill, at the smallest sensible granularity, refusing early
 * where a partial result would smear (see 03_Edge_Cases.md):
 *  - no source, or a source that fails its validity checks: the dab is a no-op;
 *  - a single vertex's probe misses: per #BrushCloneStampMeshSettings.missing_source -- leave
 *    the vertex alone (Ignore), or treat the source height as zero so an Imprint dab pulls
 *    the region onto the stamp plane (Fill).
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "../../paint_clone_mesh_source.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {

namespace clone_stamp_cc {

/* Everything a dab needs, resolved once per symmetry pass before the node loop. */
struct DabData {
  bool valid = false;

  /** The dab center projected onto the dab's reference plane, in the target object's local
   * space. Heights and the tangential sampling coordinates are measured from it; the brush
   * falloff still works on the real surface hit (#StrokeCache.location_symm). */
  float3 location = float3(0.0f);
  /** The reference plane normal for this dab, in WORLD space -- the area average of the
   * stroke-start surface around the dab center (already view-oriented, per symmetry pass), mapped
   * by the inverse transpose of the target's matrix so a non-uniformly scaled target keeps its
   * true normal. */
  float3 n = float3(0.0f, 0.0f, 1.0f);
  /** Pass-mirrored tangents in WORLD space, perpendicular to #n there, so that tangential
   * offsets of corresponding vertices match the main pass's (the symmetry rule documented in the
   * file header). */
  float3 t1 = float3(1.0f, 0.0f, 0.0f);
  float3 t2 = float3(0.0f, 1.0f, 0.0f);

  /** Target-local to world linear part. Every measurement of a dab (heights, tangential
   * offsets) is taken in world units: measuring in the target's local units would stretch the
   * stamp along the axes a non-uniform object scale stretches, and a sphere falloff in world
   * space is an ellipsoid there. */
  float3x3 to_world = float3x3::identity();
  /** The target-local displacement that moves a vertex one world unit along #n, i.e.
   * `inverse(to_world) * n`. Not parallel to the local normal under a non-uniform scale. */
  float3 disp_dir = float3(0.0f, 0.0f, 1.0f);
  /** World length of the frozen source frame's axes: converts world offsets to source-local ones
   * (and source-local heights back) so a scaled source is read at its true size too. */
  float3 source_axis_len = float3(1.0f);

  /** The source frame for this dab, source-local: the frozen pick frame (Absolute) or the
   * walking patch frame (Relative, carried over by minimal rotations). Pass-invariant: mirror
   * and tile passes read the main pass's values. */
  float3 s1 = float3(1.0f, 0.0f, 0.0f);
  float3 s2 = float3(0.0f, 1.0f, 0.0f);
  float3 s3 = float3(0.0f, 0.0f, 1.0f);
  /** The probe plane origin for this dab (Absolute: the frozen pick; Relative: the walking
   * patch, snapped back onto the source surface), source-local. It lies on the stamp plane,
   * not on the surface. */
  float3 s_base = float3(0.0f);

  /** Converts a source-local height along #s3 into world units along #n. */
  float height_scale = 1.0f;
  /** Probe ray length cap, in source-local units: the brush radius expressed on the source
   * side, doubled so a dent or bump up to twice the brush radius deep is still found. */
  float probe_limit = 0.0f;
  /** Ray start height above #s_base along #s3, source-local: above the whole source snapshot, so
   * the ray never starts inside the mesh even where the stamp plane dips into the surface. */
  float probe_top = 0.0f;
};

/** Signed height of the source surface along #DabData::s3 above or below #query_local, or
 * #FLT_MAX when the probe misses within #DabData::probe_limit on either side. ONE ray is cast,
 * from #probe_limit above the query plane downwards, so the surface reads as a height map
 * along #s3: the first hit is the outermost surface over the query point, and the far side of
 * a thin wall -- a self-clone source included -- can never win as a nearer, wrongly-signed
 * height the way it could with a nearest-hit-in-both-directions probe. */
static float clone_stamp_probe_height(const clone_mesh::CloneMeshStrokeRuntime &runtime,
                                      const DabData &dab,
                                      const float3 &query_local)
{
  BVHTreeRayHit hit{};
  hit.dist = dab.probe_top + dab.probe_limit;
  hit.index = -1;
  BLI_bvhtree_ray_cast(runtime.tree_data.tree,
                       query_local + dab.s3 * dab.probe_top,
                       -dab.s3,
                       0.0f,
                       &hit,
                       runtime.tree_data.raycast_callback,
                       const_cast<bke::BVHTreeFromMesh *>(&runtime.tree_data));

  if (hit.index == -1) {
    return FLT_MAX;
  }
  /* The ray starts probe_top above the query plane: a hit lying on the plane itself has
   * travelled probe_top, a surface below it travels further, so the signed height falls out
   * directly and keeps its sign. Surfaces further than probe_limit above the plane belong to a
   * different part of the source (the stamp reads a slab, not the whole mesh) and count as a
   * miss. */
  const float height = dab.probe_top - hit.dist;
  return (height > dab.probe_limit) ? FLT_MAX : height;
}

static bool clone_stamp_is_main_pass(const StrokeCache &cache)
{
  return cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
         cache.tile_pass == 0;
}

/** Reference plane of THIS dab: the area average of the target's STROKE-START surface around
 * the dab center. Positions and normals come from the stroke's undo state (#OrigPositionData,
 * pushed per node before the brush runs, so it holds the pre-stroke surface), never from the
 * live one -- the surface the stamp itself is moving must not tilt its own reference, the
 * feedback that made overlapping dabs drift. Being computed per dab (and per symmetry pass,
 * around that pass's own #StrokeCache.location_symm) is what lets the reference follow the
 * target's curvature along a stroke: a frozen stroke-start plane would press dabs far from the
 * stroke's beginning flat against it (the slices the per-stroke freeze produced).
 *
 * The vertex scheme mirrors #calc_area_normal_and_center's original-data branch (positions and
 * normals within the radius, the normal oriented towards the viewer); a triangle-area variant
 * would need per-node topology walks for every PBVH type for no practical gain. */
static bool dab_orig_plane_calc(const Object &object,
                                const StrokeCache &cache,
                                const IndexMask &node_mask,
                                float3 &r_center,
                                float3 &r_normal)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const float radius_sq = cache.radius * cache.radius;
  const float3 &center_ref = cache.location_symm;
  float3 center_sum(0.0f);
  float3 normal_sum(0.0f);
  int count = 0;

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const VArray<bool> hide_vert = *mesh.attributes().lookup_or_default<bool>(
          ".hide_vert", bke::AttrDomain::Point, false);
      for (const int i : node_mask.index_range()) {
        const bke::pbvh::MeshNode &node = pbvh.nodes<bke::pbvh::MeshNode>()[node_mask[i]];
        const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_mesh(object,
                                                                                        node);
        if (!orig_data) {
          continue;
        }
        const Span<float3> positions = orig_data->positions;
        const Span<float3> normals = orig_data->normals;
        int vi = 0;
        for (const int vert : node.verts()) {
          if (!hide_vert[vert] &&
              math::distance_squared(positions[vi], center_ref) <= radius_sq)
          {
            center_sum += positions[vi];
            normal_sum += normals[vi];
            count++;
          }
          vi++;
        }
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
      for (const int i : node_mask.index_range()) {
        const bke::pbvh::GridsNode &node = pbvh.nodes<bke::pbvh::GridsNode>()[node_mask[i]];
        const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_grids(object,
                                                                                         node);
        if (!orig_data) {
          continue;
        }
        const Span<float3> positions = orig_data->positions;
        const Span<float3> normals = orig_data->normals;
        const Span<int> grids = node.grids();
        for (const int g : grids.index_range()) {
          const IndexRange node_range = bke::ccg::grid_range(key, g);
          const int grid = grids[g];
          for (const int offset : IndexRange(key.grid_area)) {
            if (!grid_hidden.is_empty() && grid_hidden[grid][offset]) {
              continue;
            }
            const int node_vert = node_range[offset];
            if (math::distance_squared(positions[node_vert], center_ref) <= radius_sq) {
              center_sum += positions[node_vert];
              normal_sum += normals[node_vert];
              count++;
            }
          }
        }
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      for (const int i : node_mask.index_range()) {
        const bke::pbvh::BMeshNode &node = pbvh.nodes<bke::pbvh::BMeshNode>()[node_mask[i]];
        const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(
            &const_cast<bke::pbvh::BMeshNode &>(node));
        Array<float3> positions(verts.size());
        Array<float3> normals(verts.size());
        orig_position_data_gather_bmesh(*ss.bm_log, verts, positions, normals);
        int vi = 0;
        for (const BMVert *vert : verts) {
          if (!BM_elem_flag_test(vert, BM_ELEM_HIDDEN) &&
              math::distance_squared(positions[vi], center_ref) <= radius_sq)
          {
            center_sum += positions[vi];
            normal_sum += normals[vi];
            count++;
          }
          vi++;
        }
      }
      break;
    }
  }

  if (count == 0 || math::is_zero(normal_sum)) {
    return false;
  }
  r_center = center_sum / float(count);
  r_normal = math::normalize(normal_sum);
  if (math::dot(r_normal, cache.view_normal_symm) < 0.0f) {
    r_normal = -r_normal;
  }
  return true;
}

static DabData dab_data_calc(const Depsgraph & /*depsgraph*/,
                             const Sculpt &sd,
                             const Brush & /*brush*/,
                             const Object &object,
                             const IndexMask &node_mask,
                             clone_mesh::CloneMeshStrokeRuntime &runtime,
                             const clone_mesh::CloneMeshSourcePoint *brush_source)
{
  DabData dab;
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  /* With symmetry on the source is the surface under the brush (#brush_source, frozen frame, no
   * walk); otherwise the picked point of this object. */
  const clone_mesh::CloneMeshSourcePoint *source = brush_source != nullptr ?
                                                       brush_source :
                                                       clone_mesh::clone_mesh_source_point_get(
                                                           &object);
  if (source == nullptr || !runtime.is_valid || runtime.source_object == nullptr ||
      runtime.tree_data.tree == nullptr)
  {
    printf("[CloneMesh] dab refused: no source or invalid runtime\n");
    return dab;
  }
  if (!(cache.radius > 0.0f)) {
    printf("[CloneMesh] dab refused: radius <= 0\n");
    return dab;
  }
  if (cache.vc == nullptr || cache.vc->rv3d == nullptr) {
    printf("[CloneMesh] dab refused: no view context\n");
    /* The tangential frame is built from the viewport's screen axes; without a view the
     * stamp has no defined orientation. 3D Viewport only, like the texture clone's v1. */
    return dab;
  }

  /* Target side. The reference plane comes from the stroke-start surface under THIS dab (see
   * #dab_orig_plane_calc): stable against the stamp's own deformation, yet following the
   * target's curvature along the stroke. Heights and the tangential sampling coordinates are
   * measured against this plane, never against the live surface. */
  float3 plane_co(0.0f);
  float3 plane_no(0.0f);
  if (!dab_orig_plane_calc(object, cache, node_mask, plane_co, plane_no)) {
    printf("[CloneMesh] dab refused: no stroke-start surface under the dab\n");
    return dab;
  }
  const float4x4 target_to_world = object.object_to_world();
  const float4x4 world_to_target = object.world_to_object();
  dab.to_world = float3x3(target_to_world);
  const float3x3 from_world = float3x3(world_to_target);

  /* Everything below is measured in WORLD units (see #DabData::to_world). The averaged normal
   * is a local one, which maps by the inverse transpose; a plain direction transform would tilt
   * it under a non-uniform scale. */
  dab.n = math::normalize(math::transpose(from_world) * plane_no);
  dab.disp_dir = from_world * dab.n;
  /* The dab center sits on the reference plane's averaged center, lifted along the direction
   * that moves a vertex along the world normal by the surface hit's height above the plane, so
   * the height reference matches the source side (whose pick is projected onto the source's
   * averaged plane as well). The brush falloff keeps working on the real surface hit. */
  dab.location = plane_co +
                 dab.disp_dir * math::dot(dab.to_world * (cache.location_symm - plane_co), dab.n);

  /* The tangents are built from this pass's own frozen plane normal #DabData::n, so they are
   * exactly perpendicular to it and independent of the live (stamp-deformed) surface normal.
   * Symmetry needs no stored main-pass normal: a mirror or rotation is an isometry, so
   * projecting the pass-mirrored screen axis onto this pass's normal equals mirroring the
   * main pass's projection. The mirror itself acts in the target's local space, so the screen
   * axis goes through it and comes back to world. */
  const float4x4 viewinv = float4x4(cache.vc->rv3d->viewinv);
  const auto tangent_from_axis = [&](const float3 &axis_world) {
    const float3 mirrored_local = symm_pass_mirror_direction(
        cache, math::transform_direction(world_to_target, axis_world));
    const float3 axis = math::transform_direction(target_to_world, mirrored_local);
    return axis - math::dot(axis, dab.n) * dab.n;
  };
  float3 t1 = tangent_from_axis(viewinv.x_axis());
  if (math::length(t1) < 1e-6f) {
    /* Looking straight down the dab plane: the screen right has no tangent component. */
    t1 = tangent_from_axis(viewinv.y_axis());
  }
  if (math::length(t1) < 1e-6f) {
    /* View parallel to the plane: a deterministic perpendicular instead of dropping the dab,
     * same ladder as #clone_mesh_source_frame_build on the pick side. */
    t1 = math::cross(dab.n, float3(0.0f, 0.0f, 1.0f));
    if (math::length(t1) < 1e-6f) {
      t1 = float3(1.0f, 0.0f, 0.0f) - dab.n.x * dab.n;
    }
  }
  dab.t1 = math::normalize(t1);
  /* A reflection flips handedness: the mirrored image of `cross(n, t1)` is `-cross(n', t1')`,
   * which is what keeps the stamp content mirrored (file header). Detect it from the images of
   * the unit axes. */
  const float handedness = math::dot(
      math::cross(symm_pass_mirror_direction(cache, float3(1.0f, 0.0f, 0.0f)),
                  symm_pass_mirror_direction(cache, float3(0.0f, 1.0f, 0.0f))),
      symm_pass_mirror_direction(cache, float3(0.0f, 0.0f, 1.0f)));
  dab.t2 = math::normalize(math::cross(dab.n, dab.t1)) * (handedness < 0.0f ? -1.0f : 1.0f);

  /* Scale conversions between the source's local units and world units. They stay anchored to
   * the FROZEN pick frame: the Relative walk only rotates its frame, so rescaling per dab would
   * jitter the copied amplitudes for no benefit. */
  const Object &source_object = *runtime.source_object;
  const float4x4 source_to_world = source_object.object_to_world();
  for (const int axis : IndexRange(3)) {
    dab.source_axis_len[axis] = math::length(
        math::transform_direction(source_to_world, source->frame_axes[axis]));
  }
  if (dab.source_axis_len.x < 1e-12f || dab.source_axis_len.y < 1e-12f ||
      dab.source_axis_len.z < 1e-12f)
  {
    printf("[CloneMesh] dab refused: degenerate object scale\n");
    return dab;
  }
  dab.height_scale = dab.source_axis_len.z;
  /* The brush radius in world units: #StrokeCache.radius is a local one derived with the
   * isotropic scalar of the matrix (see #StrokeCache::position_scale). */
  const float radius_world = cache.radius * mat4_to_scale(target_to_world.ptr());
  dab.probe_limit = 2.0f * radius_world / dab.height_scale;

  /* Where this dab reads from: the frozen pick (Absolute), or the patch walking the source
   * surface (Relative -- #clone_mesh_dab_source_patch_get advances the walk on the main pass,
   * following the source's own tangents and curvature). The brush travel it decomposes is the
   * real surface hit, not the plane-projected dab center, so it measures the stroke's travel
   * alone. */
  if (brush_source != nullptr) {
    /* The brush IS the source: its frame is the one the main pass built for this dab, and the
     * mirror pass reads it unchanged -- the pass-mirrored destination frame above is what
     * mirrors the content. */
    dab.s_base = brush_source->co_source_local;
    dab.s1 = brush_source->frame_axes[0];
    dab.s2 = brush_source->frame_axes[1];
    dab.s3 = brush_source->frame_axes[2];
    dab.probe_top = clone_mesh::clone_mesh_probe_top_get(
        runtime, dab.s_base, dab.s3, dab.probe_limit);
    dab.valid = true;
    return dab;
  }
  clone_mesh::CloneMeshSourcePoint *source_for_write =
      clone_mesh::clone_mesh_source_point_get_for_write(&object);
  if (source_for_write == nullptr) {
    printf("[CloneMesh] dab refused: source record vanished\n");
    return dab;
  }
  clone_mesh::CloneMeshDabSourceParams params;
  params.t1_world = dab.t1;
  params.t2_world = dab.t2;
  params.dab_center_world = math::transform_point(target_to_world, cache.location_symm);
  params.radius_source_local = 0.5f * dab.probe_limit;
  params.probe_limit = dab.probe_limit;
  params.clone_mode = sd.paint.clone_mode;
  params.is_main_pass = clone_stamp_is_main_pass(cache);
  float3 patch_frame[3];
  dab.s_base = clone_mesh::clone_mesh_dab_source_patch_get(
      *source_for_write, runtime, params, patch_frame);
  dab.s1 = patch_frame[0];
  dab.s2 = patch_frame[1];
  dab.s3 = patch_frame[2];
  dab.probe_top = clone_mesh::clone_mesh_probe_top_get(
      runtime, dab.s_base, dab.s3, dab.probe_limit);

  dab.valid = true;
  return dab;
}

static void calc_translations(const Brush &brush,
                              const DabData &dab,
                              const clone_mesh::CloneMeshStrokeRuntime &runtime,
                              const bool no_accum,
                              const float bstrength,
                              const Span<float3> positions,
                              const Span<float3> orig_positions,
                              const Span<float> factors,
                              const MutableSpan<float> factor_cache,
                              const MutableSpan<float4> target_cache,
                              const MutableSpan<float3> r_translations)
{
  const bool imprint =
      brush.clone_stamp_mesh.apply_mode == CLONE_STAMP_MESH_APPLY_IMPRINT;
  const bool fill_missing =
      brush.clone_stamp_mesh.missing_source == CLONE_STAMP_MESH_MISSING_FILL_PLANE;
  /* Imprint converges onto the stamped form, so a strength above 1 would only exaggerate the form
   * (and steepen the falloff edge); Additive keeps the full range. */
  const float strength = imprint ? std::clamp(bstrength, -1.0f, 1.0f) : bstrength;

  /* Height above the dab plane and the source-local sampling point of a target-local position,
   * both measured in world units (see #DabData::to_world). */
  const auto measure = [&](const float3 &position, float &r_height, float3 &r_query) {
    const float3 d = dab.to_world * (position - dab.location);
    r_height = math::dot(d, dab.n);
    const float a = math::dot(d, dab.t1) / dab.source_axis_len.x;
    const float b = math::dot(d, dab.t2) / dab.source_axis_len.y;
    r_query = dab.s_base + a * dab.s1 + b * dab.s2;
  };

  runtime.dbg_verts += int(positions.size());
  for (const int i : positions.index_range()) {
    if (factors[i] == 0.0f) {
      r_translations[i] = float3(0.0f);
      continue;
    }
    runtime.dbg_factor_nonzero++;

    if (no_accum) {
      /* Relative: overlap-free application. The position is always `orig + target * f` from the
       * stroke-start position, so re-covering a vertex never re-applies the stamp on top of
       * itself, and `f` is the largest strength seen this stroke (Layer brush pattern).
       *
       * The target is the strength-weighted AVERAGE of what every covering dab measured
       * (`.xyz` = sum of weight * target, `.w` = sum of weights). Letting a single dab decide
       * -- the first one, or the strongest -- gives each vertex a target from that dab's own
       * plane and source patch, and neighboring vertices that pick different dabs differ by a
       * step along the line where the two dabs are equally strong: a seam in the shape of the
       * brush. The weights are continuous in space, and so is their average. */
      const float f_new = factors[i] * strength;
      const float f_prev = factor_cache[i];
      float height_target_orig;
      float3 query;
      measure(orig_positions[i], height_target_orig, query);
      const float height_local = clone_stamp_probe_height(runtime, dab, query);
      float source_height = 0.0f;
      bool sampled = true;
      if (height_local == FLT_MAX) {
        runtime.dbg_probe_miss++;
        /* Ignore: this dab adds no sample; whatever earlier dabs measured stays in force.
         * Fill: a missing probe counts as source height zero, pulling the vertex onto the dab's
         * reference plane. In Additive the target is zero, so it stays Ignore. */
        sampled = imprint && fill_missing;
      }
      else {
        runtime.dbg_probe_hit++;
        source_height = height_local * dab.height_scale;
      }
      float4 &acc = target_cache[i];
      if (sampled) {
        const float weight = std::abs(f_new);
        const float3 target_dab = dab.disp_dir * (imprint ? (source_height - height_target_orig) :
                                                     source_height);
        acc = float4(float3(acc.x, acc.y, acc.z) + target_dab * weight, acc.w + weight);
        runtime.dbg_max_h_micro = std::max(runtime.dbg_max_h_micro.load(),
                                           int(std::abs(source_height) * 1.0e6f));
        runtime.dbg_max_ht_micro = std::max(runtime.dbg_max_ht_micro.load(),
                                            int(std::abs(height_target_orig) * 1.0e6f));
      }
      if (!(acc.w > 0.0f)) {
        /* Nothing measured for this vertex yet. */
        r_translations[i] = float3(0.0f);
        continue;
      }
      const float3 target = float3(acc.x, acc.y, acc.z) / acc.w;
      /* Largest magnitude wins, the sign kept: an inverted (Ctrl) pass with more strength
       * replaces an earlier non-inverted one, and never the other way around. */
      const float f = std::abs(f_new) > std::abs(f_prev) ? f_new : f_prev;
      factor_cache[i] = f;
      r_translations[i] = orig_positions[i] + target * f - positions[i];
      const int delta_micro = int(math::length(target) * std::abs(f) * 1.0e6f);
      int prev = runtime.dbg_max_delta_micro.load();
      while (delta_micro > prev && !runtime.dbg_max_delta_micro.compare_exchange_weak(prev, delta_micro)) {
      }
      continue;
    }

    float height_target;
    float3 query;
    measure(positions[i], height_target, query);

    const float height_local = clone_stamp_probe_height(runtime, dab, query);
    if (height_local == FLT_MAX) {
      runtime.dbg_probe_miss++;
      if (imprint && fill_missing) {
        /* Fill: a missing probe counts as source height zero, so an Imprint dab pulls the
         * region onto the stamp plane. In Additive this would add zero, so it stays Ignore. */
        r_translations[i] = dab.disp_dir * (-height_target * factors[i]);
      }
      else {
        r_translations[i] = float3(0.0f);
      }
      continue;
    }
    const float height = height_local * dab.height_scale;
    const float delta = imprint ? (height - height_target) : height;
    r_translations[i] = dab.disp_dir * (delta * factors[i]);
    runtime.dbg_probe_hit++;
    runtime.dbg_max_h_micro = std::max(runtime.dbg_max_h_micro.load(),
                                       int(std::abs(height) * 1.0e6f));
    runtime.dbg_max_ht_micro = std::max(runtime.dbg_max_ht_micro.load(),
                                        int(std::abs(height_target) * 1.0e6f));
    const int delta_micro = int(std::abs(delta * factors[i]) * 1.0e6f);
    int prev = runtime.dbg_max_delta_micro.load();
    while (delta_micro > prev && !runtime.dbg_max_delta_micro.compare_exchange_weak(prev, delta_micro)) {
    }
  }
}

struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
  /* Node-local copies of the stroke-wide per-vertex Relative state (gather/scatter around the
   * parallel node loop, like the Layer brush's displacement factors). Empty in Absolute. */
  Vector<float> factor_cache;
  Vector<float4> target_cache;
};

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const DabData &dab,
                       const clone_mesh::CloneMeshStrokeRuntime &runtime,
                       const bool no_accum,
                       const MeshAttributeData &attribute_data,
                       const Span<float3> vert_normals,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const Span<int> verts = node.verts();

  const MutableSpan<float3> positions = gather_data_mesh(position_data.eval, verts, tls.positions);

  MutableSpan<float> factor_cache;
  MutableSpan<float4> target_cache;
  Span<float3> orig_positions;
  if (!no_accum) {
    calc_factors_common_mesh_indexed(depsgraph,
                                     brush,
                                     object,
                                     attribute_data,
                                     position_data.eval,
                                     vert_normals,
                                     node,
                                     tls.factors,
                                     tls.distances);
  }
  else {
    /* The undo node's stored positions are already in #node.verts() order. */
    const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_mesh(object,
                                                                                    node);
    if (!orig_data) {
      /* Should not happen: every node in the mask is pushed to the undo step before the brush
       * runs. Leave the node untouched rather than displace from a wrong base. */
      printf("[CloneMesh] node skipped: no stroke-start position state\n");
      tls.translations.resize(verts.size());
      tls.translations.fill(float3(0.0f));
      return;
    }
    orig_positions = orig_data->positions;
    /* The falloff is measured on the stroke-start surface: the stamp moves vertices along the
     * normal, and with live positions a vertex raised out of a Sphere falloff would drop out of
     * it (a sunken one would join), cutting a ring along every dab's edge. */
    calc_factors_common_from_orig_data_mesh(depsgraph,
                                            brush,
                                            object,
                                            attribute_data,
                                            orig_positions,
                                            orig_data->normals,
                                            node,
                                            tls.factors,
                                            tls.distances);
    factor_cache = gather_data_mesh(ss.cache->clone_mesh_factor.as_span(), verts, tls.factor_cache);
    target_cache = gather_data_mesh(ss.cache->clone_mesh_target.as_span(), verts, tls.target_cache);
  }

  tls.translations.resize(verts.size());
  calc_translations(brush,
                    dab,
                    runtime,
                    no_accum,
                    ss.cache->bstrength,
                    positions,
                    orig_positions,
                    tls.factors,
                    factor_cache,
                    target_cache,
                    tls.translations);
  /* The no-accumulate path folds the strength into the per-vertex factor itself. */
  if (!no_accum) {
    scale_translations(tls.translations, ss.cache->bstrength);
  }

  clip_and_lock_translations(sd, ss, position_data.eval, verts, tls.translations);
  position_data.deform(tls.translations, verts);

  if (no_accum) {
    scatter_data_mesh(
        factor_cache.as_span(), verts, ss.cache->clone_mesh_factor.as_mutable_span());
    scatter_data_mesh(
        target_cache.as_span(), verts, ss.cache->clone_mesh_target.as_mutable_span());
  }
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const DabData &dab,
                       const clone_mesh::CloneMeshStrokeRuntime &runtime,
                       const bool no_accum,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan<float3> positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  MutableSpan<float> factor_cache;
  MutableSpan<float4> target_cache;
  Span<float3> orig_positions;
  if (no_accum) {
    /* The undo node stores its grid points in the node's own grid order, which is the same
     * order #gather_grids_positions produced, so the span indexes the gathered points 1:1. */
    const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_grids(object,
                                                                                     node);
    if (!orig_data) {
      printf("[CloneMesh] node skipped: no stroke-start position state\n");
      tls.translations.resize(positions.size());
      tls.translations.fill(float3(0.0f));
      return;
    }
    orig_positions = orig_data->positions;
    /* Falloff on the stroke-start surface, see #calc_faces. */
    calc_factors_common_from_orig_data_grids(
        depsgraph, brush, object, orig_positions, orig_data->normals, node, tls.factors, tls.distances);
    factor_cache = gather_data_grids(
        subdiv_ccg, ss.cache->clone_mesh_factor.as_span(), grids, tls.factor_cache);
    target_cache = gather_data_grids(
        subdiv_ccg, ss.cache->clone_mesh_target.as_span(), grids, tls.target_cache);
  }
  else {
    calc_factors_common_grids(
        depsgraph, brush, object, positions, node, tls.factors, tls.distances);
  }

  tls.translations.resize(positions.size());
  calc_translations(brush,
                    dab,
                    runtime,
                    no_accum,
                    ss.cache->bstrength,
                    positions,
                    orig_positions,
                    tls.factors,
                    factor_cache,
                    target_cache,
                    tls.translations);
  if (!no_accum) {
    scale_translations(tls.translations, ss.cache->bstrength);
  }

  clip_and_lock_translations(sd, ss, positions, tls.translations);
  apply_translations(tls.translations, grids, subdiv_ccg);

  if (no_accum) {
    scatter_data_grids(
        subdiv_ccg, factor_cache.as_span(), grids, ss.cache->clone_mesh_factor.as_mutable_span());
    scatter_data_grids(
        subdiv_ccg, target_cache.as_span(), grids, ss.cache->clone_mesh_target.as_mutable_span());
  }
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const DabData &dab,
                       const clone_mesh::CloneMeshStrokeRuntime &runtime,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  calc_factors_common_bmesh(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  /* No per-vertex stroke state here: dynamic topology churns the vertex set (and its indices)
   * under the stroke, so the no-accumulate mode is disabled for BMesh targets and dabs apply
   * the accumulating way (see #do_clone_stamp_brush). */

  tls.translations.resize(positions.size());
  calc_translations(brush,
                    dab,
                    runtime,
                    false,
                    ss.cache->bstrength,
                    positions,
                    Span<float3>(),
                    tls.factors,
                    MutableSpan<float>(),
                    MutableSpan<float4>(),
                    tls.translations);
  scale_translations(tls.translations, ss.cache->bstrength);

  clip_and_lock_translations(sd, ss, positions, tls.translations);
  apply_translations(tls.translations, verts);
}

}  // namespace clone_stamp_cc

void do_clone_stamp_brush(const Depsgraph &depsgraph,
                          const Sculpt &sd,
                          Object &object,
                          const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  if (cache.vc == nullptr || cache.vc->rv3d == nullptr) {
    printf("[CloneMesh] dab skipped: no view context\n");
    return;
  }
  /* Symmetry on: the brush is the source and only the mirror/radial passes write. A multi-object
   * stroke mirrors across the reference (active) object's planes, the same object that decides
   * the pass set in #do_symmetrical_brush_actions. */
  const Object &symm_ref_object = cache.symm_reference_object ? *cache.symm_reference_object :
                                                                object;
  const bool brush_is_source = clone_mesh::clone_mesh_symmetry_mode_active(symm_ref_object);
  const clone_mesh::CloneMeshSourcePoint *brush_source = nullptr;
  if (brush_is_source) {
    /* The object under the cursor holds the source; its cache is the primary's. */
    Object &primary = cache.multi_object_sample_reference ?
                          const_cast<Object &>(*cache.multi_object_sample_reference) :
                          object;
    if (cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0) {
      /* Read-only pass: refresh the source on the primary and stamp nothing. */
      if (&object == &primary && cache.tile_pass == 0) {
        if (pbvh.type() == bke::pbvh::Type::BMesh) {
          /* The snapshot cannot follow a surface that re-tessellates under the brush. */
          printf("[CloneMesh] brush source is not supported in dynamic topology mode\n");
        }
        else {
          clone_mesh::clone_mesh_brush_source_update(
              ss.cache->clone_mesh_brush_source,
              &depsgraph,
              primary,
              cache.location,
              cache.radius,
              float4x4(cache.vc->rv3d->viewinv));
        }
      }
      return;
    }
    const SculptSession *primary_ss = primary.runtime->sculpt_session;
    if (primary_ss == nullptr || primary_ss->cache == nullptr ||
        primary_ss->cache->clone_mesh_brush_source == nullptr ||
        !primary_ss->cache->clone_mesh_brush_source->point.is_valid)
    {
      return;
    }
    brush_source = &primary_ss->cache->clone_mesh_brush_source->point;
  }

  clone_mesh::CloneMeshStrokeRuntime *runtime = nullptr;
  if (brush_source != nullptr) {
    runtime = brush_source->surface.get();
    ss.cache->clone_mesh_runtime = runtime;
  }
  else {
    runtime = clone_mesh::clone_mesh_stroke_runtime_ensure(ss.cache->clone_mesh_runtime, object);
  }
  if (runtime == nullptr) {
    /* No source for this object, or an invalid one: the dab is a no-op. */
    printf("[CloneMesh] dab skipped: no runtime (source missing/invalid, source object not found "
           "in view layer, or empty evaluated mesh)\n");
    return;
  }
  runtime->dbg_verts = 0;
  runtime->dbg_factor_nonzero = 0;
  runtime->dbg_probe_hit = 0;
  runtime->dbg_probe_miss = 0;
  runtime->dbg_max_delta_micro = 0;
  runtime->dbg_max_h_micro = 0;
  runtime->dbg_max_ht_micro = 0;

  /* A brush source moves with the brush like a Relative one, so it takes the overlap-free
   * application too. */
  const bool relative = sd.paint.clone_mode == CLONE_MODE_RELATIVE || brush_source != nullptr;
  /* The overlap-free Relative application needs stable per-vertex indices for its stroke-wide
   * state; dynamic topology churns the vertex set under the stroke, so BMesh targets keep the
   * accumulating application. */
  const bool no_accum = relative && pbvh.type() != bke::pbvh::Type::BMesh;
  if (relative && pbvh.type() == bke::pbvh::Type::BMesh) {
    printf("[CloneMesh] BMesh target: Relative applies accumulating (no per-vertex state)\n");
  }
  if (no_accum && ss.cache->clone_mesh_factor.is_empty()) {
    ss.cache->clone_mesh_factor = Array<float>(vertex_count_get(object), 0.0f);
    ss.cache->clone_mesh_target = Array<float4>(vertex_count_get(object), float4(0.0f));
  }

  const clone_stamp_cc::DabData dab = clone_stamp_cc::dab_data_calc(
      depsgraph, sd, brush, object, node_mask, *runtime, brush_source);
  /* Only the main pass reports: a mirrored pass that finds no source surface under its mirror
   * image is a legitimate no-op, not a refused stroke. */
  if (clone_stamp_cc::clone_stamp_is_main_pass(cache)) {
    runtime->last_dab_ok = dab.valid;
  }
  if (!dab.valid) {
    return;
  }

  threading::EnumerableThreadSpecific<clone_stamp_cc::LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const PositionDeformData position_data(depsgraph, object);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      node_mask.foreach_index(
          [&](const int i) {
            clone_stamp_cc::LocalData &tls = all_tls.local();
            clone_stamp_cc::calc_faces(depsgraph,
                                       sd,
                                       brush,
                                       dab,
                                       *runtime,
                                       no_accum,
                                       attribute_data,
                                       vert_normals,
                                       nodes[i],
                                       object,
                                       tls,
                                       position_data);
            bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            clone_stamp_cc::LocalData &tls = all_tls.local();
            clone_stamp_cc::calc_grids(
                depsgraph, sd, object, brush, dab, *runtime, no_accum, nodes[i], tls);
            bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            clone_stamp_cc::LocalData &tls = all_tls.local();
            clone_stamp_cc::calc_bmesh(depsgraph, sd, object, brush, dab, *runtime, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  printf("[CloneMesh] frame: loc=(%g %g %g) n=(%g %g %g) t1=(%g %g %g) t2=(%g %g %g)\n"
         "            s_base=(%g %g %g) s1=(%g %g %g) s2=(%g %g %g) s3=(%g %g %g) probe_limit=%g\n",
         dab.location.x, dab.location.y, dab.location.z, dab.n.x, dab.n.y, dab.n.z,
         dab.t1.x, dab.t1.y, dab.t1.z, dab.t2.x, dab.t2.y, dab.t2.z,
         dab.s_base.x, dab.s_base.y, dab.s_base.z, dab.s1.x, dab.s1.y, dab.s1.z,
         dab.s2.x, dab.s2.y, dab.s2.z, dab.s3.x, dab.s3.y, dab.s3.z, dab.probe_limit);
  printf(
      "[CloneMesh] dab ok: pass(mirror=%d radial=%d tile=%d) nodes=%d verts=%d factor>0=%d "
      "probe_hit=%d probe_miss=%d max|delta|=%g max|h|=%g max|ht|=%g bstrength=%g radius=%g height_scale=%g\n",
      int(cache.mirror_symmetry_pass),
      int(cache.radial_symmetry_pass),
      int(cache.tile_pass),
      int(node_mask.size()),
      runtime->dbg_verts.load(),
      runtime->dbg_factor_nonzero.load(),
      runtime->dbg_probe_hit.load(),
      runtime->dbg_probe_miss.load(),
      double(runtime->dbg_max_delta_micro.load()) * 1.0e-6,
      double(runtime->dbg_max_h_micro.load()) * 1.0e-6,
      double(runtime->dbg_max_ht_micro.load()) * 1.0e-6,
      double(cache.bstrength),
      double(cache.radius),
      double(dab.height_scale));
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

}  // namespace blender::ed::sculpt_paint::brushes
