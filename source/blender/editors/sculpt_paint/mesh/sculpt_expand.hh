/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#pragma once

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "DNA_scene_types.h"

#include "sculpt_expand_multi.hh"

namespace blender {

struct Brush;
struct Object;
struct Scene;
namespace bke::pbvh {
class Node;
}

namespace ed::sculpt_paint::expand {

enum class FalloffType {
  Geodesic,
  Topology,
  TopologyNormals,
  Normals,
  Sphere,
  BoundaryTopology,
  BoundaryFaceSet,
  ActiveFaceSet,
};

enum class TargetType {
  Mask,
  FaceSets,
  Colors,
};

enum class RecursionType {
  Topology,
  Geodesic,
};

#define EXPAND_SYMM_AREAS 8

struct ObjectState {
  Object *object = nullptr;

  Array<float> vert_falloff;
  Array<float> face_falloff;

  /* Held by pointer so #ObjectState stays movable: `IndexMaskMemory` (a #LinearAllocator) is
   * NonMovable, which would otherwise delete #ObjectState's move constructor and make
   * `Cache::object_states` (a #Vector) fail to compile. Moving the pointer keeps the allocation
   * (and therefore `node_mask`'s backing storage) in place. Allocated at invoke before `node_mask`
   * is filled. */
  std::unique_ptr<IndexMaskMemory> node_mask_memory;
  IndexMask node_mask;

  /* Multi-object only: the set of this object's connected-island ids that are reachable from the
   * seed component through mesh edges + the cross-mesh bridge. The cross-mesh generalization of
   * `Cache::active_connected_islands` (which cannot represent N objects). Populated per seed
   * change by #find_active_connected_components_from_vert; queried by
   * #is_vert_in_active_component. */
  Set<int> active_islands;

  Array<int> initial_face_sets;
  Array<float> original_mask;
  Array<int> original_face_sets;
  Array<float4> original_colors;

  /* Multi-object Grids only: maps this object's RAW flat CCG vertex index to its canonical
   * representative (see #grids_canonical_map_create) -- built once at invoke, alongside
   * #Cache::world_positions/#Cache::bridge (same static-geometry invariant). Empty for Mesh/BMesh
   * objects. Consumed by #multi_object_graph_propagate to translate seeds/bridge endpoints/results
   * between this object's raw index space (every OTHER consumer of `vert_falloff`) and the
   * duplicate-free canonical index space #detail::propagate_uniform's graph is built over. */
  Array<int> grids_canonical_map;
};

struct Cache;

/* The active object's per-object state (object_states[0]). Valid whenever the cache exists. */
ObjectState &active_object_state(Cache &expand_cache);
const ObjectState &active_object_state(const Cache &expand_cache);

struct Cache {
  /* Target data elements that the expand operation will affect. */
  TargetType target;

  /* Falloff data. */
  FalloffType falloff_type;

  /* Max falloff value across all objects, in `ObjectState::vert_falloff`. */
  float max_vert_falloff;

  /* Max falloff value across all objects, in `ObjectState::face_falloff`. */
  float max_face_falloff;

  /* Falloff value of the active element (vertex or base mesh face) that Expand will expand to. */
  float active_falloff;

  /* When set to true, expand skips all falloff computations and considers all elements as enabled.
   */
  bool all_enabled;

  /* Initial mouse and cursor data from where the current falloff started. This data can be changed
   * during the execution of Expand by moving the origin. */
  float2 initial_mouse_move;
  float2 initial_mouse;
  int initial_active_vert;
  int initial_active_face_set;

  /* Maximum number of vertices allowed in the SculptSession for previewing the falloff using
   * geodesic distances. */
  int max_geodesic_move_preview;

  /* Original falloff type before starting the move operation. */
  FalloffType move_original_falloff_type;
  /* Falloff type using when moving the origin for preview. */
  FalloffType move_preview_falloff_type;

  /* Face set ID that is going to be used when creating a new face set. */
  int next_face_set;

  /* Face set ID of the Face set selected for editing. */
  int update_face_set;

  /* Mouse position since the last time the origin was moved. Used for reference when moving the
   * initial position of Expand. */
  float2 original_mouse_move;

  /* Active island checks. */
  /* Indexed by symmetry pass. `.object_index` = which object, `.vert` = the connected island id
   * for that pass. A mirrored seed can land on a different object than the main pass (spec §8). */
  MultiVertRef active_connected_islands[EXPAND_SYMM_AREAS];

  /* Snapping. */
  /* Set containing all face set IDs that Expand will use to snap the new data. */
  std::unique_ptr<Set<int>> snap_enabled_face_sets;

  /* Texture distortion data. */
  const Brush *brush;
  const Paint *paint;
  Scene *scene;
  // struct MTex *mtex;

  /* Controls how much texture distortion will be applied to the current falloff */
  float texture_distortion_strength;

  /* Expand state options. */

  /* Number of loops (times that the falloff is going to be repeated). */
  int loop_count;

  /* Invert the falloff result. */
  bool invert;

  /* When set to true, preserves the previous state of the data and adds the new one on top. */
  bool preserve;

  /* When set to true, the mask or colors will be applied as a gradient. */
  bool falloff_gradient;

  /* When set to true, Expand will use the Brush falloff curve data to shape the gradient. */
  bool brush_gradient;

  /* When set to true, Expand will move the origin (initial active vertex and cursor position)
   * instead of updating the active vertex and active falloff. */
  bool move;

  /* When set to true, Expand will snap the new data to the face set IDs found in
   * `ObjectState::original_face_sets`. */
  bool snap;

  /* When set to true together with `snap`, restricts the snapped result to the object that holds
   * `seed` (where the operator started), instead of every object sharing a matching face set ID.
   * Face set IDs are per-object and not globally unique, so without this a fresh object's single
   * default face set (ID 1, covering the whole mesh) matches the same ID on every other object. */
  bool snap_seed_object_only;

  /* When set to true, Expand will use the current face set ID to modify an existing face set
   * instead of creating a new one. */
  bool modify_active_face_set;

  /* When set to true, Expand will reposition the sculpt pivot to the boundary of the expand result
   * after finishing the operation. */
  bool reposition_pivot;

  /* If nothing is masked set mask of every vertex to 0. */
  bool auto_mask;

  /* Color target data type related data. */
  float fill_color[4];
  short blend_mode;

  bool check_islands;
  int normal_falloff_blur_steps;

  /* Per-object state, active object at index 0 (matches sculpt_mode_objects order). */
  Vector<ObjectState> object_states;

  /* The seed vertex under the cursor, possibly on a non-active object (spec §4.2/§4.3). The active
   * object is always object_states[0]; `initial_active_vert` mirrors this only when the seed is on
   * the active object (see set_initial_components_for_mouse). */
  MultiVertRef seed;

  /* World-space positions per object, index-aligned to object_states. Built ONCE at invoke and
   * valid for the whole modal op (Expand never mutates geometry — static-geometry invariant).
   * Empty in the single-object path. */
  Array<Array<float3>> world_positions;

  /* Cross-mesh proximity bridge, built ONCE at invoke (static-geometry invariant — Expand never
   * mutates geometry). Empty in the single-object path. */
  MultiObjectBridge bridge;

  /* Concatenated Geodesic-mode topology + adjacency maps, lazily built on the first Geodesic
   * #multi_object_graph_propagate call and reused for the rest of the modal op (same
   * static-geometry invariant as #world_positions / #bridge above) -- avoids rebuilding
   * #bke::mesh::build_edge_to_face_map / #build_vert_to_edge_map on every mouse-move while moving
   * the propagation origin (Architecture_Refactoring_Analysis.md 5.2). Null when Geodesic falloff
   * has not been used yet, or in the single-object path. */
  std::unique_ptr<detail::GlobalGeodesicTopology> geodesic_topology_cache;
};

}  // namespace ed::sculpt_paint::expand

}  // namespace blender
