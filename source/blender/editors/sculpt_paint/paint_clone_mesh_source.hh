/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Clone Stamp Mesh source points: session storage, picking, operators and the
 * per-stroke frozen sampling runtime.
 *
 * The Clone Stamp Mesh brush (#SCULPT_BRUSH_TYPE_CLONE_MESH) stamps the surface form of a
 * source object onto the mesh being sculpted. This module owns everything the brush needs
 * besides the standard brush pipeline:
 *  - the source point record (which object, which spot, which frozen frame), one per target
 *    object, session-only;
 *  - the one-shot picking operators (Shift+LMB while the brush is active);
 *  - the per-stroke sampling runtime: a snapshot of the source positions taken at stroke
 *    start plus a private BVH built over it.
 */

#pragma once

#include "BKE_bvhutils.hh"

#include "BLI_array.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include <atomic>
#include <cstdint>
#include <memory>

/* NOTE: all Blender DNA/data types live in namespace blender. */
namespace blender {
struct bContext;
struct Depsgraph;
struct Mesh;
struct Object;
struct ViewContext;
struct wmEvent;
struct wmOperator;
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::clone_mesh {

/* -------------------------------------------------------------------- */
/** \name Source surface snapshot (frozen when the source is set).
 *
 * The dab reads a snapshot of the source surface, taken at the moment the source point is set,
 * through a private BVH built over that snapshot, never the live mesh:
 *
 *  1. WHAT WAS PICKED IS WHAT IS STAMPED. Later sculpting of the source (a self-clone stroke
 *     included) cannot change the stamp, so the stamp never feeds on its own output.
 *  2. STALE CACHES. The mesh BVH cache is not invalidated by sculpt edits; a snapshot tree and
 *     its snapshot arrays can never disagree.
 *
 * A source in Sculpt Mode is snapshotted from its ORIGINAL mesh (where the sculpt brushes
 * write); every other source from its evaluated mesh.
 * \{ */

struct CloneMeshStrokeRuntime {
  /** Identity the snapshot was built for. */
  uint32_t source_object_session_uid = 0;

  /** The resolved ORIGINAL source object. Only its world matrix is read at dab time; the
   * surface itself lives in the snapshot arrays below. */
  const Object *source_object = nullptr;

  Array<float3> positions_snapshot;
  /** Corner topology copied with the positions, so the tree never points into a mesh the
   * depsgraph may free and re-evaluate. */
  Array<int> corner_verts_snapshot;
  Array<int3> corner_tris_snapshot;
  /** BVH over the snapshot arrays; owns the tree through #tree_data. */
  bke::BVHTreeFromMesh tree_data{};

  bool is_valid = false;

  /**
   * Outcome of the last dab built against this snapshot: false after a dab was refused (no
   * source data or invalid frames). There is no reports channel reachable from a brush dab, so
   * "the stroke does nothing" is answered visually: the source cursor draws its marker red
   * while this is set.
   */
  bool last_dab_ok = true;

  /* TODO: temporary debug counters, reset per dab and printed by the brush. Remove with the
   * "[CloneMesh]" prints once the brush is verified. */
  mutable std::atomic<int> dbg_verts{0};
  mutable std::atomic<int> dbg_factor_nonzero{0};
  mutable std::atomic<int> dbg_probe_hit{0};
  mutable std::atomic<int> dbg_probe_miss{0};
  /** Largest |displacement| of the dab, in micro-units. */
  mutable std::atomic<int> dbg_max_delta_micro{0};
  /** Largest source height and target height seen in the dab, in micro-units. */
  mutable std::atomic<int> dbg_max_h_micro{0};
  mutable std::atomic<int> dbg_max_ht_micro{0};

  CloneMeshStrokeRuntime() = default;
  ~CloneMeshStrokeRuntime() = default;
  CloneMeshStrokeRuntime(const CloneMeshStrokeRuntime &) = delete;
  CloneMeshStrokeRuntime &operator=(const CloneMeshStrokeRuntime &) = delete;
};

/**
 * The point a Clone Stamp Mesh dab copies from, stored per TARGET object.
 *
 * Everything geometric is kept in the SOURCE object's local space (the space its mesh and
 * its own stroke cache work in), the same choice the texture clone makes when it stores a UV:
 * the record survives the source object being moved, since world transforms are applied
 * fresh at every dab.
 */
struct CloneMeshSourcePoint {
  /** The picked hit, in the source object's local space. */
  float3 co_source_local = float3(0.0f);
  /** The surface normal at the hit, source-local, oriented towards the viewer at pick time. */
  float3 normal_source_local = float3(0.0f, 0.0f, 1.0f);

  /**
   * The stamp frame at the moment the source was picked, FROZEN: screen right and up
   * flattened onto the surface, plus the surface normal, all source-local.
   *
   * Freezing this end is what makes viewport rotation reach the stamp -- the same relation
   * the texture clone's #clone::CloneSurfaceFrame documents: if both ends of the stamp were
   * rebuilt from the current view, rotating the viewport would rotate both frames by the
   * same angle, which cancels out, and the stamp would never turn.
   *
   * It is also pass-invariant BY CONSTRUCTION, which is what makes mirrored symmetry passes
   * read the same source point as the main pass for a corresponding vertex: tangential
   * coordinates measured in the pass-mirrored destination frame equal the main pass's
   * coordinates, and the displacement along the mirrored normal does the reflecting. See
   * 02_Design.md section 4.4.
   */
  float3 frame_axes[3] = {float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
                          float3(0.0f, 0.0f, 1.0f)};

  /**
   * Relative mode: the WALKING patch -- where on the source surface the stamp currently reads.
   *
   * The brush's travel is decomposed in the destination tangents and applied along the patch's
   * own tangents; the moved point is pulled back onto the source surface and onto a fresh local
   * average plane, and the frame follows by a minimal rotation (see
   * #clone_mesh_dab_source_patch_get). Heights read this way carry the source's local detail;
   * a rigidly translated frozen pick would leave the surface on anything curved, and the stamp
   * would pick up the source's large-scale shape instead.
   *
   * The walk cannot be seeded when the source is set -- the pointer is ON the source at that
   * moment and the offset would be zero -- so it starts from the frozen pick on the first
   * main-pass dab painted afterwards, the handshake an aligned clone stamp uses. It is kept on
   * the source point rather than on the stroke, so it survives releasing the mouse and the next
   * stroke continues from where this one left off; setting or resetting the source clears it.
   * Only the main symmetry pass may move it.
   */
  float3 walk_co_source_local = float3(0.0f);
  /** The walking stamp frame, source-local orthonormal, built like #frame_axes. */
  float3 walk_frame[3] = {float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
                          float3(0.0f, 0.0f, 1.0f)};
  /** The previous main-pass dab center in world space: the brush travel the next main-pass dab
   * decomposes is measured against it. */
  float3 walk_prev_target_world = float3(0.0f);
  /** False until the walk was seeded (see above). */
  bool walk_valid = false;

  bool is_valid = false;

  /**
   * #ID.session_uid of the object this record names as the SOURCE.
   *
   * The pointer is kept alongside for the lookup, but validity is decided by the uid: a
   * freed object's address can be handed back out by the allocator, and a pointer
   * comparison alone would then accept a stale record. Session uids are unique for the
   * session and never reused.
   */
  uint32_t source_object_session_uid = 0;
  const Object *source_object = nullptr;
  const Mesh *source_mesh = nullptr;

  /** The source surface as it was when the point was set. See the snapshot section above.
   * Shared between the records of every target object the source was set for in one pick (a
   * multi-object sculpt), so the snapshot is built once. */
  std::shared_ptr<CloneMeshStrokeRuntime> surface;
};

/** Re-checks the identity half of the record: the source object still exists, is still the
 * mesh it was picked on, and is not in dynamic topology (its surface would stop matching the
 * stored frame). */
bool clone_mesh_source_point_is_valid(const CloneMeshSourcePoint &source);

/**
 * Stores \a source_object (with its point and frozen frame) as the source of \a target_object.
 * Keyed on the TARGET: every object being sculpted carries its own source, so a global-sculpt
 * stroke can stamp different sources onto different meshes, and self-clone is just the case
 * where the two are the same object. Setting a source resets the Relative walk.
 */
void clone_mesh_source_point_set(const Object &target_object,
                                 const Object &source_object,
                                 const float3 &co_source_local,
                                 const float3 &normal_source_local,
                                 const float3 frame_axes[3],
                                 std::shared_ptr<CloneMeshStrokeRuntime> surface);
const CloneMeshSourcePoint *clone_mesh_source_point_get(const Object *target_object);
/** Writable counterpart, for the dab path that advances #CloneMeshSourcePoint's Relative walk.
 * Same validity rules as the const version. */
CloneMeshSourcePoint *clone_mesh_source_point_get_for_write(const Object *target_object);
void clone_mesh_source_point_reset(const Object *target_object);
/** Drop every source. Registered on file load; also reachable for a full reset. */
void clone_mesh_source_points_clear_all();

/**
 * Register the file-load handler that calls #clone_mesh_source_points_clear_all.
 * Idempotent; called once at startup.
 */
void clone_mesh_source_points_callbacks_register();

/** What a dab needs to resolve the source patch it reads from. The world-space quantities are
 * this symmetry pass's; the source-side limits are in the source's local units. */
struct CloneMeshDabSourceParams {
  /** The pass's destination tangents in world space, normalized. */
  float3 t1_world = float3(1.0f, 0.0f, 0.0f);
  float3 t2_world = float3(0.0f, 1.0f, 0.0f);
  /** The pass's brush hit, world space. */
  float3 dab_center_world = float3(0.0f);
  /** The brush radius in SOURCE-local units; the walk's average plane is sampled around the
   * moved point at this radius. */
  float radius_source_local = 0.0f;
  /** The probe ray cap in source-local units (twice the radius); also the cap of the walk's
   * snap ray. */
  float probe_limit = 0.0f;
  int8_t clone_mode = 0;
  bool is_main_pass = false;
};

/**
 * The point a dab measures its source patch from, i.e. the source-side center the stamp samples
 * around; the patch's frame (same construction as #CloneMeshSourcePoint.frame_axes) is written
 * to \a r_frame, source-local.
 *
 * Absolute answers with the frozen picked point and frame. Relative walks the patch across the
 * source surface: on every main-pass dab the brush travel since the previous main-pass dab is
 * applied along the patch's own tangents, the moved point is pulled back onto the source
 * surface (one probe ray, nearest-point fallback), re-projected onto a fresh local average
 * plane, and the frame follows by a minimal rotation. Mirror and tile passes answer with the
 * main pass's walk result of this step and never move it.
 */
float3 clone_mesh_dab_source_patch_get(CloneMeshSourcePoint &source,
                                       const CloneMeshStrokeRuntime &runtime,
                                       const CloneMeshDabSourceParams &params,
                                       float3 r_frame[3]);


/**
 * Points  owner (the StrokeCache slot) at the snapshot stored with the source of  target_object.
 * The cache does not own it: the snapshot lives and dies with the source record. Returns null
 * when there is no valid source for this target.
 */
CloneMeshStrokeRuntime *clone_mesh_stroke_runtime_ensure(CloneMeshStrokeRuntime *&owner,
                                                         const Object &target_object);
/**
 * Distance along \a axis (unit) from \a origin to the far side of the snapshot's bounding box,
 * at least \a min_top. A probe ray started this far above the query point begins outside the
 * mesh whatever the stamp plane does: a plane that dips into the surface (concave source,
 * Relative walk over a curve) would otherwise start the ray INSIDE the mesh, where the first
 * "outermost" hit is the inner side of the wall.
 */
float clone_mesh_probe_top_get(const CloneMeshStrokeRuntime &runtime,
                               const float3 &origin,
                               const float3 &axis,
                               float min_top);
/** Detaches the runtime from the cache slot; the snapshot itself belongs to the source record. */
void clone_mesh_stroke_runtime_free(CloneMeshStrokeRuntime *&runtime);

/**
 * The source of a stroke with symmetry on: the surface under the brush itself, not a picked
 * point. The main pass reads it (and writes nothing), the mirror and radial passes stamp it onto
 * their side. Owned by the primary object's #StrokeCache, so its snapshot dies with the stroke.
 */
struct CloneMeshBrushSource {
  /** Record of the current dab's source, in the primary object's local space; its #surface is
   * the stroke-start snapshot taken on the first dab. The Relative walk is not used. */
  CloneMeshSourcePoint point;
};

/** Whether \a symmetry_reference_object has mirror or radial symmetry on, i.e. whether Clone
 * Stamp Mesh reads from the brush instead of a picked source. */
bool clone_mesh_symmetry_mode_active(const Object &symmetry_reference_object);

/**
 * Refreshes \a slot for this dab: takes the stroke-start snapshot of \a primary on the first call,
 * snaps \a co_local (the main pass hit, primary-local) onto it and builds the stamp plane and
 * frame there, like the picker does. Returns the record, or null when the primary has no usable
 * surface or nothing lies within \a radius_local of the point.
 */
const CloneMeshSourcePoint *clone_mesh_brush_source_update(
    std::shared_ptr<CloneMeshBrushSource> &slot,
    const Depsgraph *depsgraph,
    const Object &primary,
    const float3 &co_local,
    float radius_local,
    const float4x4 &view_to_world);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators (Shift+LMB picking).
 * \{ */

void PAINT_OT_clone_mesh_source_set(wmOperatorType *ot);
void PAINT_OT_clone_mesh_source_reset(wmOperatorType *ot);

/** \} */

/* The cursor marker for this brush is declared in paint_cursor.hh
 * (#mesh_cursor_clone_mesh_source_draw), next to the texture clone's marker, because it takes
 * the shared #PaintCursorContext. */

}  // namespace blender::ed::sculpt_paint::clone_mesh
