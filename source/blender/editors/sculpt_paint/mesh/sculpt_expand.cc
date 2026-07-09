/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#include "sculpt_expand.hh"
#include "sculpt_expand_multi.hh"

#include <cmath>
#include <queue>

#include "MEM_guardedalloc.h"

#include "BLI_array_utils.hh"
#include "BLI_bit_vector.hh"
#include "BLI_linklist_stack.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_ccg.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_multires.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh"
#include "paint_mask.hh"
#include "sculpt_boundary.hh"
#include "sculpt_color.hh"
#include "sculpt_face_set.hh"
#include "sculpt_flood_fill.hh"
#include "sculpt_geodesic.hh"
#include "sculpt_intern.hh"
#include "sculpt_islands.hh"
#include "sculpt_smooth.hh"
#include "sculpt_undo.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "bmesh.hh"

namespace blender {

namespace ed::sculpt_paint::expand {

/* Sculpt Expand. */
/* Operator for creating selections and patterns in Sculpt Mode. Expand can create masks, face sets
 * and fill vertex colors. */
/* The main functionality of the operator
 * - The operator initializes a value per vertex, called "falloff". There are multiple algorithms
 * to generate these falloff values which will create different patterns in the result when using
 * the operator. These falloff values require algorithms that rely on mesh connectivity, so they
 * are only valid on parts of the mesh that are in the same connected component as the given
 * initial vertices. If needed, these falloff values are propagated from vertex or grids into the
 * base mesh faces.
 *
 * - On each modal callback, the operator gets the active vertex and face and gets its falloff
 *   value from its precalculated falloff. This is now the active falloff value.
 * - Using the active falloff value and the settings of the expand operation (which can be modified
 *   during execution using the modal key-map), the operator loops over all elements in the mesh to
 *   check if they are enabled of not.
 * - Based on each element state after evaluating the settings, the desired mesh data (mask, face
 *   sets, colors...) is updated.
 */

/**
 * Used for defining an invalid vertex state (for example, when the cursor is not over the mesh).
 */
#define SCULPT_EXPAND_VERTEX_NONE -1

/** Used for defining an uninitialized active component index for an unused symmetry pass. */
#define EXPAND_ACTIVE_COMPONENT_NONE -1
/**
 * Defines how much each time the texture distortion is increased/decreased
 * when using the modal key-map.
 */
#define SCULPT_EXPAND_TEXTURE_DISTORTION_STEP 0.01f

/**
 * This threshold offsets the required falloff value to start a new loop. This is needed because in
 * some situations, vertices which have the same falloff value as max_falloff will start a new
 * loop, which is undesired.
 */
#define SCULPT_EXPAND_LOOP_THRESHOLD 0.00001f

/**
 * Defines how much changes in curvature in the mesh affect the falloff shape when using normal
 * falloff. This default was found experimentally and it works well in most cases, but can be
 * exposed for tweaking if needed.
 */
#define SCULPT_EXPAND_NORMALS_FALLOFF_EDGE_SENSITIVITY 300

/* Multiplies the min mean world edge length to get the bridge proximity threshold. Conservative
 * first guess; retune after runtime testing on real content (spec §6.4). */
#define SCULPT_EXPAND_BRIDGE_FACTOR 3.0f

/* Expand Modal Key-map. */
enum {
  SCULPT_EXPAND_MODAL_CONFIRM = 1,
  SCULPT_EXPAND_MODAL_CANCEL,
  SCULPT_EXPAND_MODAL_INVERT,
  SCULPT_EXPAND_MODAL_PRESERVE_TOGGLE,
  SCULPT_EXPAND_MODAL_GRADIENT_TOGGLE,
  SCULPT_EXPAND_MODAL_FALLOFF_CYCLE,
  SCULPT_EXPAND_MODAL_RECURSION_STEP_GEODESIC,
  SCULPT_EXPAND_MODAL_RECURSION_STEP_TOPOLOGY,
  SCULPT_EXPAND_MODAL_MOVE_TOGGLE,
  SCULPT_EXPAND_MODAL_FALLOFF_GEODESIC,
  SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY,
  SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY_DIAGONALS,
  SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL,
  SCULPT_EXPAND_MODAL_SNAP_TOGGLE,
  SCULPT_EXPAND_MODAL_SNAP_SEED_OBJECT_ONLY_TOGGLE,
  SCULPT_EXPAND_MODAL_LOOP_COUNT_INCREASE,
  SCULPT_EXPAND_MODAL_LOOP_COUNT_DECREASE,
  SCULPT_EXPAND_MODAL_BRUSH_GRADIENT_TOGGLE,
  SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_INCREASE,
  SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_DECREASE,
};

/* Active object accessors. Stage 1 only reads/writes `object_states[0]`; multi-object fan-out
 * lands in later stages. */

ObjectState &active_object_state(Cache &expand_cache)
{
  return expand_cache.object_states[0];
}
const ObjectState &active_object_state(const Cache &expand_cache)
{
  return expand_cache.object_states[0];
}

/* The per-object state for `object` within the cache (linear scan; object_states is tiny). */
static const ObjectState &object_state_get(const Cache &expand_cache, const Object &object)
{
  for (const ObjectState &state : expand_cache.object_states) {
    if (state.object == &object) {
      return state;
    }
  }
  BLI_assert_unreachable();
  return expand_cache.object_states[0];
}

/* Index of `object` within object_states (0 = active object). */
static int object_index_get(const Cache &expand_cache, const Object &object)
{
  for (const int i : expand_cache.object_states.index_range()) {
    if (expand_cache.object_states[i].object == &object) {
      return i;
    }
  }
  BLI_assert_unreachable();
  return 0;
}

/**
 * Returns true when every object in `states` is backed by a Mesh or Grids (Multires)
 * #bke::pbvh::Tree. Multi-object Expand is restricted to these -- BMesh (Dyntopo) objects are not
 * supported across objects (or wired into the propagation graph at all).
 */
static bool all_topology_supported(Span<ObjectState> states)
{
  for (const ObjectState &state : states) {
    const bke::pbvh::Type type = bke::object::pbvh_get(*state.object)->type();
    if (type != bke::pbvh::Type::Mesh && type != bke::pbvh::Type::Grids) {
      return false;
    }
  }
  return true;
}

/**
 * Returns true when Expand should operate across all objects in `expand_cache.object_states`
 * instead of only the active object: more than one object in the mode and all of them Mesh- or
 * Grids-backed.
 */
static bool expand_multi_object_active(const Cache &expand_cache)
{
  return expand_cache.object_states.size() > 1 &&
        all_topology_supported(expand_cache.object_states);
}

/**
 * Returns true when at least one object in `states` is Grids-backed. Used to decide, once per
 * group, whether Geodesic falloff should route through the multi-object Sphere path instead of
 * true triangle-unfold geodesic -- mirrors the existing single-object Geodesic->Spherical fallback
 * policy (`geodesic_falloff_create`'s `has_topology_info` check), just decided for the whole group
 * at once so a Mesh/Multires seam never has some objects on exact geodesic and others on
 * Euclidean-sphere distances.
 */
static bool any_grids_backed(Span<ObjectState> states)
{
  for (const ObjectState &state : states) {
    if (bke::object::pbvh_get(*state.object)->type() == bke::pbvh::Type::Grids) {
      return true;
    }
  }
  return false;
}

/* Functions for getting the state of mesh elements (vertices and base mesh faces). When the main
 * functions for getting the state of an element return true it means that data associated to that
 * element will be modified by expand. */

/**
 * Returns true if the vertex is in a connected component with correctly initialized falloff
 * values.
 */
static bool is_vert_in_active_component(const SculptSession &ss,
                                        const Cache &expand_cache,
                                        const int vert,
                                        const int object_index)
{
  if (expand_cache.snap_seed_object_only && object_index != expand_cache.seed.object_index) {
    /* Alt: restrict Expand's result to the object Expand started on. This is the single choke
     * point every enable-decision (vertex OR face domain, any target -- Mask/Colors/Face Sets,
     * snap or plain falloff) already goes through via #is_vert_in_active_component/
     * #is_face_in_active_component, so gating here covers all of them uniformly instead of
     * duplicating the check per call site (previously this only existed inside the Face-Sets
     * Snap branches of `face_state_get`/`enabled_state_to_bitmap`, so plain falloff-based Mask
     * Expand ignored Alt entirely). Single-object sessions are unaffected: `object_index` and
     * `expand_cache.seed.object_index` are both always 0 there, so this never fires. */
    return false;
  }

  /* Match BOTH the object and the island id: `active_connected_islands` now keys each symmetry
   * pass by the object it was resolved on, so a vertex is "active" only if some pass landed on its
   * own object AND its own island. Single object: every entry's `object_index` is 0, matching every
   * query's `object_index` of 0, so this reduces to the pre-Task-4.2 `.vert` compare. */
  const int island_id = islands::vert_id_get(ss, vert);
  if (expand_multi_object_active(expand_cache)) {
    /* Cross-mesh: active iff this vertex's island is bridge-reachable from the seed component
     * (computed once per seed change into each ObjectState by
     * #find_active_connected_components_from_vert). */
    return expand_cache.object_states[object_index].active_islands.contains(island_id);
  }
  for (int i = 0; i < EXPAND_SYMM_AREAS; i++) {
    if (expand_cache.active_connected_islands[i].object_index == object_index &&
        expand_cache.active_connected_islands[i].vert == island_id)
    {
      return true;
    }
  }
  return false;
}

/**
 * Returns true if the face is in a connected component with correctly initialized falloff values.
 */
static bool is_face_in_active_component(const Object &object,
                                        const OffsetIndices<int> faces,
                                        const Span<int> corner_verts,
                                        const Cache &expand_cache,
                                        const int f)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const int object_index = object_index_get(expand_cache, object);
  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh:
      return is_vert_in_active_component(
          ss, expand_cache, corner_verts[faces[f].start()], object_index);
    case bke::pbvh::Type::Grids:
      return is_vert_in_active_component(
          ss,
          expand_cache,
          faces[f].start() * BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg).grid_area,
          object_index);
    case bke::pbvh::Type::BMesh:
      return is_vert_in_active_component(
          ss, expand_cache, BM_elem_index_get(ss.bm->ftable[f]->l_first->v), object_index);
  }
  BLI_assert_unreachable();
  return false;
}

/**
 * Returns the falloff value of a vertex. This function includes texture distortion, which is not
 * precomputed into the initial falloff values.
 */
static float falloff_value_vertex_get(const SculptSession &ss,
                                      const ObjectState &state,
                                      const Cache &expand_cache,
                                      const float3 &position,
                                      const int vert)
{
  if (expand_cache.texture_distortion_strength == 0.0f) {
    return state.vert_falloff[vert];
  }
  const Brush *brush = expand_cache.brush;
  const MTex *mtex = BKE_brush_mask_texture_get(brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    return state.vert_falloff[vert];
  }

  float4 rgba;
  const float avg = BKE_brush_sample_tex_3d(
      expand_cache.paint, brush, mtex, position, rgba, 0, ss.tex_pool);

  const float distortion = (avg - 0.5f) * expand_cache.texture_distortion_strength *
                           expand_cache.max_vert_falloff;
  return state.vert_falloff[vert] + distortion;
}

/**
 * Returns the maximum valid falloff value stored in the falloff array, taking the maximum possible
 * texture distortion into account.
 */
static float max_vert_falloff_get(const Cache &expand_cache)
{
  if (expand_cache.texture_distortion_strength == 0.0f) {
    return expand_cache.max_vert_falloff;
  }

  const MTex *mask_tex = BKE_brush_mask_texture_get(expand_cache.brush, OB_MODE_SCULPT);
  if (!mask_tex->tex) {
    return expand_cache.max_vert_falloff;
  }

  return expand_cache.max_vert_falloff +
         (0.5f * expand_cache.texture_distortion_strength * expand_cache.max_vert_falloff);
}

static bool vert_falloff_is_enabled(const SculptSession &ss,
                                    const ObjectState &state,
                                    const Cache &expand_cache,
                                    const float3 &position,
                                    const int vert)
{
  const float max_falloff_factor = max_vert_falloff_get(expand_cache);
  const float loop_len = (max_falloff_factor / expand_cache.loop_count) +
                         SCULPT_EXPAND_LOOP_THRESHOLD;

  const float vertex_falloff_factor = falloff_value_vertex_get(
      ss, state, expand_cache, position, vert);
  const float active_factor = fmod(expand_cache.active_falloff, loop_len);
  const float falloff_factor = fmod(vertex_falloff_factor, loop_len);

  return falloff_factor < active_factor;
}

/**
 * Main function to get the state of a face for the current state and settings of a #Cache.
 * Returns true when the target data should be modified by expand.
 */
static bool face_state_get(const Object &object,
                           const OffsetIndices<int> faces,
                           const Span<int> corner_verts,
                           const Span<bool> hide_poly,
                           const Span<int> face_sets,
                           const Cache &expand_cache,
                           const int face)
{
  if (!hide_poly.is_empty() && hide_poly[face]) {
    return false;
  }

  if (!is_face_in_active_component(object, faces, corner_verts, expand_cache, face)) {
    return false;
  }

  if (expand_cache.all_enabled) {
    if (expand_cache.invert) {
      return false;
    }
    return true;
  }

  if (expand_multi_object_active(expand_cache) && expand_cache.snap_seed_object_only &&
      object_index_get(expand_cache, object) == expand_cache.seed.object_index)
  {
    /* Alt: once restricted to the object Expand started on, treat that WHOLE mesh as enabled
     * immediately -- same reasoning as #enabled_state_to_bitmap's mirrored branch (matches
     * Ctrl/Snap's whole-face-set capture instead of requiring the falloff to reach every face). */
    return !expand_cache.invert;
  }

  bool enabled = false;
  /* Reads `object`'s own state (single object ⇒ `object` is the active object ⇒ identical to
   * today's `active_object_state`). */
  const ObjectState &state = object_state_get(expand_cache, object);

  if (expand_cache.snap_enabled_face_sets) {
    /* Alt (`snap_seed_object_only`) is handled once, upstream, in
     * #is_face_in_active_component/#is_vert_in_active_component -- a face on a non-seed object
     * never reaches this point when it's active, so no separate check is needed here. */
    const int face_set = state.original_face_sets[face];
    enabled = expand_cache.snap_enabled_face_sets->contains(face_set);
  }
  else {
    const float loop_len = (expand_cache.max_face_falloff / expand_cache.loop_count) +
                           SCULPT_EXPAND_LOOP_THRESHOLD;

    const float active_factor = fmod(expand_cache.active_falloff, loop_len);
    const float falloff_factor = fmod(state.face_falloff[face], loop_len);
    enabled = falloff_factor < active_factor;
  }

  if (expand_cache.falloff_type == FalloffType::ActiveFaceSet) {
    if (face_sets[face] == expand_cache.initial_active_face_set) {
      enabled = false;
    }
  }

  if (expand_cache.invert) {
    enabled = !enabled;
  }

  return enabled;
}

/**
 * For target modes that support gradients (such as sculpt masks or colors), this function returns
 * the corresponding gradient value for an enabled vertex.
 */
static float gradient_value_get(const SculptSession &ss,
                                const ObjectState &state,
                                const Cache &expand_cache,
                                const float3 &position,
                                const int vert)
{
  if (!expand_cache.falloff_gradient) {
    return 1.0f;
  }

  const float max_falloff_factor = max_vert_falloff_get(expand_cache);
  const float loop_len = (max_falloff_factor / expand_cache.loop_count) +
                         SCULPT_EXPAND_LOOP_THRESHOLD;

  const float vertex_falloff_factor = falloff_value_vertex_get(
      ss, state, expand_cache, position, vert);
  const float active_factor = fmod(expand_cache.active_falloff, loop_len);
  const float falloff_factor = fmod(vertex_falloff_factor, loop_len);

  float linear_falloff;

  if (expand_cache.invert) {
    /* Active factor is the result of a modulus operation using loop_len, so they will never be
     * equal and loop_len - active_factor should never be 0. */
    BLI_assert((loop_len - active_factor) != 0.0f);
    linear_falloff = (falloff_factor - active_factor) / (loop_len - active_factor);
  }
  else {
    linear_falloff = 1.0f - (falloff_factor / active_factor);
  }

  if (!expand_cache.brush_gradient) {
    return linear_falloff;
  }

  return BKE_brush_curve_strength(expand_cache.brush, linear_falloff, 1.0f);
}

/* Utility functions for getting all vertices state during expand. */

/**
 * Returns a bitmap indexed by vertex index which contains if the vertex was enabled or not for a
 * give expand_cache state.
 */
static BitVector<> enabled_state_to_bitmap(const Depsgraph &depsgraph,
                                           const Object &object,
                                           const Cache &expand_cache)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const int totvert = vertex_count_get(object);
  BitVector<> enabled_verts(totvert);
  if (expand_cache.all_enabled) {
    if (!expand_cache.invert) {
      enabled_verts.fill(true);
    }
    return enabled_verts;
  }
  const ObjectState &state = object_state_get(expand_cache, object);
  /* #is_vert_in_active_component matches on both `object_index` and island id, so secondary
   * objects are filtered by their OWN keyed islands. Single object ⇒ `object_index` is always 0 ⇒
   * identical to the original single-object filter. */
  const int object_index = object_index_get(expand_cache, object);

  if (expand_multi_object_active(expand_cache) && expand_cache.snap_seed_object_only &&
      object_index == expand_cache.seed.object_index)
  {
    /* Alt: once restricted to the object Expand started on, treat that WHOLE mesh as enabled
     * immediately -- matching how Ctrl/Snap captures a whole face set as soon as any of it is
     * reached, instead of requiring the falloff to physically grow across every vertex of a
     * (possibly huge) mesh. Mirrors the `all_enabled` early-return above, just scoped to this one
     * object instead of every object. Gated on multi-object being active: in a single-object
     * session `object_index`/`seed.object_index` are both always 0, so this would otherwise turn
     * Alt into an unrelated "select all" shortcut there. */
    if (!expand_cache.invert) {
      enabled_verts.fill(true);
    }
    return enabled_verts;
  }

  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
      const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
      const VArraySpan face_sets = *attributes.lookup_or_default<int>(
          ".sculpt_face_set", bke::AttrDomain::Face, 0);
      threading::parallel_for_aligned(
          IndexRange(totvert), 1024, bits::BitsPerInt, [&](const IndexRange range) {
            for (const int vert : range) {
              if (!hide_vert.is_empty() && hide_vert[vert]) {
                continue;
              }
              if (!is_vert_in_active_component(ss, expand_cache, vert, object_index)) {
                continue;
              }
              if (expand_cache.snap) {
                /* Alt (`snap_seed_object_only`) is handled once, upstream, in
                 * #is_vert_in_active_component -- the `continue` above already skips a
                 * non-seed-object vertex when it's active. */
                enabled_verts[vert].set(face_set::vert_has_any_face_set(
                    vert_to_face_map, face_sets, vert, *expand_cache.snap_enabled_face_sets));
                continue;
              }
              enabled_verts[vert].set(
                  vert_falloff_is_enabled(ss, state, expand_cache, positions[vert], vert));
            }
          });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Mesh &base_mesh = *id_cast<const Mesh *>(object.data);
      const OffsetIndices<int> faces = base_mesh.faces();
      const Span<int> corner_verts = base_mesh.corner_verts();
      const GroupedSpan<int> vert_to_face_map = base_mesh.vert_to_face_map();
      const bke::AttributeAccessor attributes = base_mesh.attributes();
      const VArraySpan face_sets = *attributes.lookup_or_default<int>(
          ".sculpt_face_set", bke::AttrDomain::Face, 0);

      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const Span<float3> positions = subdiv_ccg.positions;
      BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
      for (const int grid : IndexRange(subdiv_ccg.grids_num)) {
        const int start = grid * key.grid_area;
        BKE_subdiv_ccg_foreach_visible_grid_vert(key, grid_hidden, grid, [&](const int offset) {
          const int vert = start + offset;
          if (!is_vert_in_active_component(ss, expand_cache, vert, object_index)) {
            return;
          }
          if (expand_cache.snap) {
            /* Alt (`snap_seed_object_only`) is handled once, upstream, in
             * #is_vert_in_active_component -- the `return` above already skips a non-seed-object
             * vertex when it's active. */
            const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vert);
            if (face_set::coord_has_any_face_set(faces,
                                                 corner_verts,
                                                 vert_to_face_map,
                                                 face_sets,
                                                 subdiv_ccg,
                                                 coord,
                                                 *expand_cache.snap_enabled_face_sets))
            {
              enabled_verts[vert].set(true);
            }
            return;
          }
          enabled_verts[vert].set(
              vert_falloff_is_enabled(ss, state, expand_cache, positions[vert], vert));
        });
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ss.bm;
      for (const int vert : IndexRange(totvert)) {
        const BMVert *bm_vert = BM_vert_at_index(&bm, vert);
        if (BM_elem_flag_test(bm_vert, BM_ELEM_HIDDEN)) {
          continue;
        }
        if (!is_vert_in_active_component(ss, expand_cache, vert, object_index)) {
          continue;
        }
        if (expand_cache.snap) {
          /* TODO: Support face sets for BMesh. */
          const int face_set = 0;
          enabled_verts[vert].set(expand_cache.snap_enabled_face_sets->contains(face_set));
          continue;
        }
        enabled_verts[vert].set(
            vert_falloff_is_enabled(ss, state, expand_cache, bm_vert->co, vert));
      }
      break;
    }
  }
  if (expand_cache.invert) {
    bits::invert(MutableBoundedBitSpan(enabled_verts));
  }
  return enabled_verts;
}

/**
 * Returns a bitmap indexed by vertex index which contains if the vertex is in the boundary of the
 * enabled vertices. This is defined as vertices that are enabled and at least have one connected
 * vertex that is not enabled.
 */
static IndexMask boundary_from_enabled(Object &object,
                                       const BitSpan enabled_verts,
                                       const bool use_mesh_boundary,
                                       IndexMaskMemory &memory)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  /* `enabled_verts` must be indexed by this object's own vertex-count convention (raw CCG count for
   * a Grids object, not the base-mesh count); a mismatch would index the wrong buffer. */
  BLI_assert(enabled_verts.size() == vertex_count_get(object));

  /* Defensive (real check, not assert-only): `object_states_init` populates every group member's
   * `boundary_info_cache` once at invoke, but this function is only ever exercised for a
   * secondary Grids object once, at Confirm time (via #reposition_pivot), never during the live
   * per-mouse-move drag -- unlike #enabled_state_to_bitmap, which runs on every drag step and has
   * therefore already proven `ss`/`subdiv_ccg` valid for this object by the time this runs. Making
   * this call idempotently self-healing here (cheap: `ensure_boundary_info` no-ops if the cache
   * already exists) removes any chance of dereferencing a null cache, whatever the reason it could
   * be unset, instead of crashing. */
  boundary::ensure_boundary_info(object);

  const IndexMask enabled_mask = IndexMask::from_bits(enabled_verts, memory);

  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const OffsetIndices faces = mesh.faces();
      const Span<int> corner_verts = mesh.corner_verts();
      const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      return IndexMask::from_predicate(enabled_mask, memory, [&](const int vert) {
        Vector<int> neighbors;
        for (const int neighbor : vert_neighbors_get_mesh(
                 faces, corner_verts, vert_to_face_map, hide_poly, vert, neighbors))
        {
          if (!enabled_verts[neighbor]) {
            return true;
          }
        }

        if (use_mesh_boundary &&
            boundary::vert_is_boundary(
                vert_to_face_map, hide_poly, ss.boundary_info_cache->verts, vert))
        {
          return true;
        }

        return false;
      });
    }
    case bke::pbvh::Type::Grids: {
      const Mesh &base_mesh = *id_cast<const Mesh *>(object.data);
      const OffsetIndices faces = base_mesh.faces();
      const Span<int> corner_verts = base_mesh.corner_verts();

      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      return IndexMask::from_predicate(enabled_mask, memory, [&](const int vert) {
        const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vert);
        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, false, neighbors);
        for (const SubdivCCGCoord neighbor : neighbors.coords) {
          if (!enabled_verts[neighbor.to_index(key)]) {
            return true;
          }
        }

        if (use_mesh_boundary && boundary::vert_is_boundary(faces,
                                                            corner_verts,
                                                            ss.boundary_info_cache->verts,
                                                            ss.boundary_info_cache->edges,
                                                            subdiv_ccg,
                                                            coord))
        {
          return true;
        }

        return false;
      });
    }
    case bke::pbvh::Type::BMesh: {
      return IndexMask::from_predicate(enabled_mask, memory, [&](const int vert) {
        BMVert *bm_vert = BM_vert_at_index(ss.bm, vert);
        BMeshNeighborVerts neighbors;
        for (const BMVert *neighbor : vert_neighbors_get_bmesh(*bm_vert, neighbors)) {
          if (!enabled_verts[BM_elem_index_get(neighbor)]) {
            return true;
          }
        }

        if (use_mesh_boundary && BM_vert_is_boundary(bm_vert)) {
          return true;
        }

        return false;
      });
    }
  }
  BLI_assert_unreachable();
  return {};
}

static void check_topology_islands(Object &ob, FalloffType falloff_type)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  Cache &expand_cache = *ss.expand_cache;

  expand_cache.check_islands = ELEM(falloff_type,
                                    FalloffType::Geodesic,
                                    FalloffType::Topology,
                                    FalloffType::TopologyNormals,
                                    FalloffType::BoundaryTopology,
                                    FalloffType::Normals);

  if (falloff_type == FalloffType::Geodesic && any_grids_backed(expand_cache.object_states)) {
    /* Mirrors #calc_falloff_from_vert_and_symmetry's Geodesic->Sphere group fallback: when the
     * group has a Grids object, Geodesic is actually computed as a plain Euclidean distance
     * across the whole group (#spherical_falloff_multi), exactly like the user-chosen Sphere
     * falloff -- which never sets check_islands (it is absent from the ELEM list above, and the
     * SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL toggle explicitly clears it). Leaving check_islands
     * true here would still gate the actual Mask/FaceSet WRITE by cross-object island/bridge
     * connectivity even though the distance computation no longer respects topology at all -- the
     * proximity bridge (#build_multi_object_bridge) is only a best-effort stitch, so a secondary
     * object not close enough to bridge-connect would have its write silently suppressed while
     * the falloff-based preview (which never checked islands) kept showing it as reachable. */
    expand_cache.check_islands = false;
  }

  if (expand_cache.check_islands) {
    islands::ensure_cache(ob);
  }
}

}  // namespace ed::sculpt_paint::expand

namespace ed::sculpt_paint {

/* Functions implementing different algorithms for initializing falloff values. */

Vector<int> find_symm_verts_mesh(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const int original_vert,
                                 const float max_distance)
{
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const bool use_original = false;

  Vector<int> symm_verts;
  symm_verts.append(original_vert);

  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

  const float3 location = positions[original_vert];
  for (int symm_it = 1; symm_it <= symm; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    const float3 symm_location = symmetry_flip(location, ePaintSymmetryFlags(symm_it));
    const std::optional<int> nearest = nearest_vert_calc_mesh(
        pbvh, positions, hide_vert, symm_location, max_distance, use_original);
    if (!nearest) {
      continue;
    }
    symm_verts.append(*nearest);
  }

  std::ranges::sort(symm_verts);
  return symm_verts;
}

Vector<int> find_symm_verts_grids(const Object &object,
                                  const int original_vert,
                                  const float max_distance)
{
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const bool use_original = false;

  Vector<int> symm_verts;
  symm_verts.append(original_vert);

  const SculptSession &ss = *object.runtime->sculpt_session;
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float3> positions = subdiv_ccg.positions;
  const float3 location = positions[original_vert];
  for (int symm_it = 1; symm_it <= symm; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    const float3 symm_location = symmetry_flip(location, ePaintSymmetryFlags(symm_it));
    const std::optional<SubdivCCGCoord> nearest = nearest_vert_calc_grids(
        pbvh, subdiv_ccg, symm_location, max_distance, use_original);
    if (!nearest) {
      continue;
    }
    symm_verts.append(nearest->to_index(key));
  }

  std::ranges::sort(symm_verts);
  return symm_verts;
}

Vector<int> find_symm_verts_bmesh(const Object &object,
                                  const int original_vert,
                                  const float max_distance)
{
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const bool use_original = false;

  Vector<int> symm_verts;
  symm_verts.append(original_vert);

  const SculptSession &ss = *object.runtime->sculpt_session;
  BMesh &bm = *ss.bm;
  const BMVert *original_bm_vert = BM_vert_at_index(&bm, original_vert);
  const float3 location = original_bm_vert->co;
  for (int symm_it = 1; symm_it <= symm; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    const float3 symm_location = symmetry_flip(location, ePaintSymmetryFlags(symm_it));
    const std::optional<BMVert *> nearest = nearest_vert_calc_bmesh(
        pbvh, symm_location, max_distance, use_original);
    if (!nearest) {
      continue;
    }
    symm_verts.append(BM_elem_index_get(*nearest));
  }

  std::ranges::sort(symm_verts);
  return symm_verts;
}

Vector<int> find_symm_verts(const Depsgraph &depsgraph,
                            const Object &object,
                            const int original_vert,
                            const float max_distance)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      return find_symm_verts_mesh(depsgraph, object, original_vert, max_distance);
    case bke::pbvh::Type::Grids:
      return find_symm_verts_grids(object, original_vert, max_distance);
    case bke::pbvh::Type::BMesh:
      return find_symm_verts_bmesh(object, original_vert, max_distance);
  }
  BLI_assert_unreachable();
  return {};
}

std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts(const Depsgraph &depsgraph,
                                                      const Object &object,
                                                      const int original_vert,
                                                      const float max_distance)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      return find_all_symm_verts_mesh(depsgraph, object, original_vert, max_distance);
    case bke::pbvh::Type::Grids:
      return find_all_symm_verts_grids(object, original_vert, max_distance);
    case bke::pbvh::Type::BMesh:
      return find_all_symm_verts_bmesh(object, original_vert, max_distance);
  }
  BLI_assert_unreachable();
  return {};
}

std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts_mesh(const Depsgraph &depsgraph,
                                                           const Object &object,
                                                           const int original_vert,
                                                           const float max_distance)
{
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const bool use_original = false;

  std::array<int, PAINT_SYMM_AREAS> symm_verts;
  symm_verts.fill(-1);
  symm_verts[0] = original_vert;

  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

  const float3 location = positions[original_vert];
  for (int symm_it = 1; symm_it <= PAINT_SYMM_AREAS; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    const float3 symm_location = symmetry_flip(location, ePaintSymmetryFlags(symm_it));
    const std::optional<int> nearest = nearest_vert_calc_mesh(
        pbvh, positions, hide_vert, symm_location, max_distance, use_original);
    if (!nearest) {
      continue;
    }
    symm_verts[symm_it] = *nearest;
  }

  return symm_verts;
}

std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts_grids(const Object &object,
                                                            const int original_vert,
                                                            const float max_distance)
{
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const bool use_original = false;

  std::array<int, PAINT_SYMM_AREAS> symm_verts;
  symm_verts.fill(-1);
  symm_verts[0] = original_vert;

  const SculptSession &ss = *object.runtime->sculpt_session;
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float3> positions = subdiv_ccg.positions;
  const float3 location = positions[original_vert];
  for (int symm_it = 1; symm_it <= PAINT_SYMM_AREAS; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    const float3 symm_location = symmetry_flip(location, ePaintSymmetryFlags(symm_it));
    const std::optional<SubdivCCGCoord> nearest = nearest_vert_calc_grids(
        pbvh, subdiv_ccg, symm_location, max_distance, use_original);
    if (!nearest) {
      continue;
    }
    symm_verts[symm_it] = nearest->to_index(key);
  }

  return symm_verts;
}

std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts_bmesh(const Object &object,
                                                            const int original_vert,
                                                            const float max_distance)
{
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(object);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const bool use_original = false;

  std::array<int, PAINT_SYMM_AREAS> symm_verts;
  symm_verts.fill(-1);
  symm_verts[0] = original_vert;

  const SculptSession &ss = *object.runtime->sculpt_session;
  BMesh &bm = *ss.bm;
  const BMVert *original_bm_vert = BM_vert_at_index(&bm, original_vert);
  const float3 location = original_bm_vert->co;
  for (int symm_it = 1; symm_it <= PAINT_SYMM_AREAS; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    const float3 symm_location = symmetry_flip(location, ePaintSymmetryFlags(symm_it));
    const std::optional<BMVert *> nearest = nearest_vert_calc_bmesh(
        pbvh, symm_location, max_distance, use_original);
    if (!nearest) {
      continue;
    }
    symm_verts[symm_it] = BM_elem_index_get(*nearest);
  }

  return symm_verts;
}

}  // namespace ed::sculpt_paint

namespace ed::sculpt_paint::expand {

/**
 * Geodesic: Initializes the falloff with geodesic distances from the given active vertex, taking
 * symmetry into account.
 */
static Array<float> geodesic_falloff_create(const Depsgraph &depsgraph,
                                            Object &ob,
                                            const IndexMask &initial_verts)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<int2> edges = mesh.edges();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);

  SculptSession &ss = *ob.runtime->sculpt_session;
  if (ss.edge_to_face_map.is_empty()) {
    ss.edge_to_face_map = bke::mesh::build_edge_to_face_map(
        faces, corner_edges, edges.size(), ss.edge_to_face_offsets, ss.edge_to_face_indices);
  }
  if (ss.vert_to_edge_map.is_empty()) {
    ss.vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
        edges, mesh.verts_num, ss.vert_to_edge_offsets, ss.vert_to_edge_indices);
  }

  Set<int> verts;
  initial_verts.foreach_index([&](const int vert) { verts.add(vert); });

  return geodesic::distances_create(vert_positions,
                                    edges,
                                    faces,
                                    corner_verts,
                                    ss.vert_to_edge_map,
                                    ss.edge_to_face_map,
                                    hide_poly,
                                    verts,
                                    FLT_MAX);
}
static Array<float> geodesic_falloff_create(const Depsgraph &depsgraph,
                                            Object &ob,
                                            const int initial_vert)
{
  const Vector<int> symm_verts = find_symm_verts(depsgraph, ob, initial_vert);

  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(symm_verts.as_span(), memory);

  return geodesic_falloff_create(depsgraph, ob, mask);
}

/**
 * Topology: Initializes the falloff using a flood-fill operation,
 * increasing the falloff value by 1 when visiting a new vertex.
 */
static void calc_topology_falloff_from_verts(Object &ob,
                                             const IndexMask &initial_verts,
                                             MutableSpan<float> distances)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const int totvert = vertex_count_get(ob);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      flood_fill::FillDataMesh flood(totvert);
      initial_verts.foreach_index([&](const int vert) { flood.add_and_skip_initial(vert); });
      flood.execute(ob, vert_to_face_map, [&](const int from_vert, const int to_vert) {
        distances[to_vert] = distances[from_vert] + 1.0f;
        return true;
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

      flood_fill::FillDataGrids flood(totvert);
      initial_verts.foreach_index([&](const int vert) {
        const SubdivCCGCoord orig_coord = SubdivCCGCoord::from_index(key, vert);
        flood.add_and_skip_initial(orig_coord, vert);
      });
      flood.execute(
          ob,
          subdiv_ccg,
          [&](const SubdivCCGCoord from, const SubdivCCGCoord to, const bool is_duplicate) {
            const int from_vert = from.to_index(key);
            const int to_vert = to.to_index(key);
            if (is_duplicate) {
              distances[to_vert] = distances[from_vert];
            }
            else {
              distances[to_vert] = distances[from_vert] + 1.0f;
            }
            return true;
          });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ss.bm;
      flood_fill::FillDataBMesh flood(totvert);
      initial_verts.foreach_index(
          [&](const int vert) { flood.add_and_skip_initial(BM_vert_at_index(&bm, vert), vert); });
      flood.execute(ob, [&](BMVert *from_bm_vert, BMVert *to_bm_vert) {
        const int from_vert = BM_elem_index_get(from_bm_vert);
        const int to_vert = BM_elem_index_get(to_bm_vert);
        distances[to_vert] = distances[from_vert] + 1.0f;
        return true;
      });
      break;
    }
  }
}

static Array<float> topology_falloff_create(const Depsgraph &depsgraph,
                                            Object &ob,
                                            const int initial_vert)
{
  const Vector<int> symm_verts = find_symm_verts(depsgraph, ob, initial_vert);

  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(symm_verts.as_span(), memory);

  Array<float> dists(vertex_count_get(ob), 0.0f);
  calc_topology_falloff_from_verts(ob, mask, dists);
  return dists;
}

/**
 * Normals: Flood-fills the mesh and reduces the falloff depending on the normal difference between
 * each vertex and the previous one.
 * This creates falloff patterns that follow and snap to the hard edges of the object.
 */
static Array<float> normals_falloff_create(const Depsgraph &depsgraph,
                                           Object &ob,
                                           const int vert,
                                           const float edge_sensitivity,
                                           const int blur_steps)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const int totvert = vertex_count_get(ob);
  Array<float> dists(totvert, 0.0f);
  Array<float> edge_factors(totvert, 1.0f);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);

      const float3 orig_normal = vert_normals[vert];
      flood_fill::FillDataMesh flood(totvert);
      flood.add_initial(find_symm_verts(depsgraph, ob, vert));
      flood.execute(ob, vert_to_face_map, [&](const int from_vert, const int to_vert) {
        const float3 &from_normal = vert_normals[from_vert];
        const float3 &to_normal = vert_normals[to_vert];
        const float from_edge_factor = edge_factors[from_vert];
        const float dist = math::dot(orig_normal, to_normal) *
                           powf(from_edge_factor, edge_sensitivity);
        edge_factors[to_vert] = math::dot(to_normal, from_normal) * from_edge_factor;
        dists[to_vert] = std::clamp(dist, 0.0f, 1.0f);
        return true;
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const Span<float3> normals = subdiv_ccg.normals;
      const float3 orig_normal = normals[vert];
      flood_fill::FillDataGrids flood(totvert);
      flood.add_initial(key, find_symm_verts(depsgraph, ob, vert));
      flood.execute(
          ob,
          subdiv_ccg,
          [&](const SubdivCCGCoord from, const SubdivCCGCoord to, const bool is_duplicate) {
            const int from_vert = from.to_index(key);
            const int to_vert = to.to_index(key);
            if (is_duplicate) {
              edge_factors[to_vert] = edge_factors[from_vert];
              dists[to_vert] = dists[from_vert];
            }
            else {
              const float3 &from_normal = normals[from_vert];
              const float3 &to_normal = normals[to_vert];
              const float from_edge_factor = edge_factors[from_vert];
              const float dist = math::dot(orig_normal, to_normal) *
                                 powf(from_edge_factor, edge_sensitivity);
              edge_factors[to_vert] = math::dot(to_normal, from_normal) * from_edge_factor;
              dists[to_vert] = std::clamp(dist, 0.0f, 1.0f);
            }
            return true;
          });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      flood_fill::FillDataBMesh flood(totvert);
      BMVert *orig_vert = BM_vert_at_index(ss.bm, vert);
      const float3 orig_normal = orig_vert->no;
      flood.add_initial(*ss.bm, find_symm_verts(depsgraph, ob, vert));
      flood.execute(ob, [&](BMVert *from_bm_vert, BMVert *to_bm_vert) {
        const float3 from_normal = from_bm_vert->no;
        const float3 to_normal = to_bm_vert->no;
        const int from_vert = BM_elem_index_get(from_bm_vert);
        const int to_vert = BM_elem_index_get(to_bm_vert);
        const float from_edge_factor = edge_factors[from_vert];
        const float dist = math::dot(orig_normal, to_normal) *
                           powf(from_edge_factor, edge_sensitivity);
        edge_factors[to_vert] = math::dot(to_normal, from_normal) * from_edge_factor;
        dists[to_vert] = std::clamp(dist, 0.0f, 1.0f);
        return true;
      });
      break;
    }
  }

  smooth::blur_geometry_data_array(ob, blur_steps, dists);

  for (int i = 0; i < totvert; i++) {
    dists[i] = 1.0 - dists[i];
  }

  return dists;
}

/* Nearest vertex to `world_target` across all objects, within `max_world_distance`.
 * Returns {-1,-1} if none. Reuses each object's existing pbvh nearest-vert query. */
static MultiVertRef nearest_multi_vert(const Depsgraph &depsgraph,
                                       Span<ObjectState> states,
                                       Span<Array<float3>> world_positions,
                                       const float3 &world_target,
                                       const float max_world_distance)
{
  MultiVertRef best;
  float best_dist_sq = max_world_distance * max_world_distance;
  for (const int i : states.index_range()) {
    Object &object = *states[i].object;
    const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
    const float3 local_target = math::transform_point(object.world_to_object(), world_target);
    /* Search radius MUST be a conservative LOCAL radius covering the world sphere, not the raw
     * world distance — otherwise a scaled-down object under-searches and drops valid
     * candidates. */
    const float local_radius = world_radius_to_local_search_radius(object.world_to_object(),
                                                                   max_world_distance);

    std::optional<int> nearest;
    if (pbvh.type() == bke::pbvh::Type::Grids) {
      const SculptSession &ss = *object.runtime->sculpt_session;
      const std::optional<SubdivCCGCoord> nearest_coord = nearest_vert_calc_grids(
          pbvh, *ss.subdiv_ccg, local_target, local_radius, false);
      if (nearest_coord) {
        const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
        nearest = nearest_coord->to_index(key);
      }
    }
    else {
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
      nearest = nearest_vert_calc_mesh(
          pbvh, positions, hide_vert, local_target, local_radius, false);
    }
    if (!nearest) {
      continue;
    }
    /* Exact world-space gap decides acceptance and the winner. */
    const float dist_sq = math::distance_squared(world_positions[i][*nearest], world_target);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = {i, *nearest};
    }
  }
  return best;
}

static Vector<MultiVertRef> find_symm_multi_verts(const Depsgraph &depsgraph,
                                                  Span<ObjectState> states,
                                                  Span<Array<float3>> world_positions,
                                                  const MultiVertRef seed,
                                                  const float max_world_distance)
{
  Object &reference = *states[0].object;
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(reference);

  Vector<MultiVertRef> result;
  result.append(seed);

  const float3 seed_world = world_positions[seed.object_index][seed.vert];
  /* Round 4B chain: world -> reference local -> symmetry_flip -> world. A plain orthogonal world
   * reflection is only correct under uniform scale; this chain stays correct under non-uniform
   * scale / rotation of the reference object (spec §5). */
  const float3 seed_ref_local = math::transform_point(reference.world_to_object(), seed_world);
  for (int symm_it = 1; symm_it <= symm; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    const float3 mirror_ref_local = symmetry_flip(seed_ref_local, ePaintSymmetryFlags(symm_it));
    const float3 mirror_world = math::transform_point(reference.object_to_world(),
                                                       mirror_ref_local);
    const MultiVertRef nearest = nearest_multi_vert(
        depsgraph, states, world_positions, mirror_world, max_world_distance);
    if (nearest.object_index == -1) {
      continue;
    }
    result.append(nearest);
  }

  /* Dedup by `(object_index, vert)`: mirrored symmetry passes can resolve to the same vertex (e.g.
   * a seed already lying on a mirror plane), which would otherwise produce redundant island entries
   * AND redundant propagation seeds (spec §11). Keeps the FIRST occurrence, so `seed` (always the
   * first entry) always survives and pass order is otherwise preserved -- safe for every caller:
   * min-over-seeds falloffs (Sphere) and multi-source BFS/graph propagation (Geodesic, Topology,
   * boundary types, islands) are unaffected by a duplicate 0-distance source. */
  Vector<MultiVertRef> deduped;
  Set<int64_t> seen;
  for (const MultiVertRef &vert_ref : result) {
    const int64_t key = (int64_t(vert_ref.object_index) << 32) | uint32_t(vert_ref.vert);
    if (seen.add(key)) {
      deduped.append(vert_ref);
    }
  }
  return deduped;
}

/**
 * Spherical: Initializes the falloff based on the distance from a vertex, taking symmetry into
 * account.
 */
static Array<float> spherical_falloff_create(const Depsgraph &depsgraph,
                                             const Object &object,
                                             const int vert)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  Array<float> dists(vertex_count_get(object));

  const Vector<int> symm_verts = find_symm_verts(depsgraph, object, vert);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);

      Array<float3> locations(symm_verts.size());
      array_utils::gather(positions, symm_verts.as_span(), locations.as_mutable_span());

      threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
        for (const int vert : range) {
          float dist = std::numeric_limits<float>::max();
          for (const float3 &location : locations) {
            dist = std::min(dist, math::distance(positions[vert], location));
          }
          dists[vert] = dist;
        }
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const Span<float3> positions = subdiv_ccg.positions;

      Array<float3> locations(symm_verts.size());
      for (const int i : symm_verts.index_range()) {
        locations[i] = positions[symm_verts[i]];
      }

      threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
        for (const int vert : range) {
          float dist = std::numeric_limits<float>::max();
          for (const float3 &location : locations) {
            dist = std::min(dist, math::distance(positions[vert], location));
          }
          dists[vert] = dist;
        }
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ss.bm;

      Array<float3> locations(symm_verts.size());
      for (const int i : symm_verts.index_range()) {
        locations[i] = BM_vert_at_index(&bm, symm_verts[i])->co;
      }

      threading::parallel_for(IndexRange(bm.totvert), 1024, [&](const IndexRange range) {
        for (const int vert : range) {
          float dist = std::numeric_limits<float>::max();
          for (const float3 &location : locations) {
            dist = std::min(dist,
                            math::distance(float3(BM_vert_at_index(&bm, vert)->co), location));
          }
          dists[vert] = dist;
        }
      });
      break;
    }
  }

  return dists;
}

/**
 * Multi-object Spherical falloff: mirrors #spherical_falloff_create's min-over-seeds distance,
 * but in WORLD space and across every object in `expand_cache.object_states`. `dists[v]` is never
 * FLT_MAX here -- every vert gets a finite distance -- which matches the single-object Sphere
 * path where all verts are reachable.
 */
static void spherical_falloff_multi(const Depsgraph &depsgraph, Cache &expand_cache)
{
  Span<ObjectState> states = expand_cache.object_states;
  Span<Array<float3>> world = expand_cache.world_positions;
  const Vector<MultiVertRef> seeds = find_symm_multi_verts(
      depsgraph, states, world, expand_cache.seed, FLT_MAX);

  Vector<float3> seed_world;
  for (const MultiVertRef &s : seeds) {
    seed_world.append(world[s.object_index][s.vert]);
  }

  for (const int i : states.index_range()) {
    const Span<float3> positions = world[i];
    Array<float> dists(positions.size());
    threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
      for (const int v : range) {
        float dist = std::numeric_limits<float>::max();
        for (const float3 &s : seed_world) {
          dist = std::min(dist, math::distance(positions[v], s));
        }
        dists[v] = dist;
      }
    });
    expand_cache.object_states[i].vert_falloff = std::move(dists);
  }
}

/**
 * Boundary: This falloff mode uses the code from sculpt_boundary to initialize the closest mesh
 * boundary to a falloff value of 0. Then, it propagates that falloff to the rest of the mesh so it
 * stays parallel to the boundary, increasing the falloff value by 1 on each step.
 */
static Array<float> boundary_topology_falloff_create(const Depsgraph &depsgraph,
                                                     Object &ob,
                                                     const int initial_vert)
{
  const Vector<int> symm_verts = find_symm_verts(depsgraph, ob, initial_vert);

  BitVector<> boundary_verts(vertex_count_get(ob));
  for (const int vert : symm_verts) {
    if (std::unique_ptr<boundary::SculptBoundary> boundary = boundary::data_init(
            depsgraph, ob, nullptr, vert, FLT_MAX))
    {
      for (const int vert : boundary->verts) {
        boundary_verts[vert].set();
      }
    }
  }

  IndexMaskMemory memory;
  const IndexMask boundary_mask = IndexMask::from_bits(boundary_verts, memory);

  Array<float> dists(vertex_count_get(ob), 0.0f);
  calc_topology_falloff_from_verts(ob, boundary_mask, dists);
  return dists;
}

/**
 * Topology diagonals. This falloff is similar to topology, but it also considers the diagonals of
 * the base mesh faces when checking a vertex neighbor. For this reason, this is not implement
 * using the general flood-fill and sculpt neighbors accessors.
 */
static Array<float> diagonals_falloff_create(const Depsgraph &depsgraph,
                                             Object &ob,
                                             const int vert)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const int totvert = vertex_count_get(ob);
  Array<float> dists(totvert, 0.0f);

  /* This algorithm uses mesh data (faces and loops), so this falloff type can't be initialized for
   * Multires. It also does not make sense to implement it for dyntopo as the result will be the
   * same as Topology falloff. */
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return dists;
  }

  const Vector<int> symm_verts = find_symm_verts(depsgraph, ob, vert);

  /* Search and mask as visited the initial vertices using the enabled symmetry passes. */
  BitVector<> visited_verts(totvert);
  std::queue<int> queue;
  for (const int vert : symm_verts) {
    queue.push(vert);
    visited_verts[vert].set();
  }

  if (queue.empty()) {
    return dists;
  }

  /* Propagate the falloff increasing the value by 1 each time a new vertex is visited. */
  while (!queue.empty()) {
    const int next_vert = queue.front();
    queue.pop();

    for (const int face : vert_to_face_map[next_vert]) {
      for (const int vert : corner_verts.slice(faces[face])) {
        if (visited_verts[vert]) {
          continue;
        }
        dists[vert] = dists[next_vert] + 1.0f;
        visited_verts[vert].set();
        queue.push(vert);
      }
    }
  }

  return dists;
}

/**
 * Updates the max_falloff value for vertices in a #Cache based on the current values of
 * the falloff, skipping any invalid values initialized to FLT_MAX and not initialized components.
 */
static void update_max_vert_falloff_value(const Object &object, Cache &expand_cache)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const ObjectState &state = active_object_state(expand_cache);
  expand_cache.max_vert_falloff = threading::parallel_reduce(
      IndexRange(vertex_count_get(object)),
      4096,
      std::numeric_limits<float>::lowest(),
      [&](const IndexRange range, float max) {
        for (const int vert : range) {
          if (state.vert_falloff[vert] == FLT_MAX) {
            continue;
          }
          /* This helper is only ever called with the active object (0), single-object switch
           * below the multi gate; hardcode the index rather than resolving it. */
          if (!is_vert_in_active_component(ss, expand_cache, vert, 0)) {
            continue;
          }
          max = std::max(max, state.vert_falloff[vert]);
        }
        return max;
      },
      [](const float a, const float b) { return std::max(a, b); });
}

/* Global max over EVERY object's vert_falloff (spec §4.1: the max stays a single scalar for the
 * whole operation). The active-component island filter applies per object -- each object's verts
 * are checked against its OWN keyed islands via `is_vert_in_active_component(..., i)`. */
static void update_max_vert_falloff_value_multi(Cache &expand_cache)
{
  Span<ObjectState> states = expand_cache.object_states;
  float global_max = std::numeric_limits<float>::lowest();
  for (const int i : states.index_range()) {
    const ObjectState &state = states[i];
    const Object &object = *state.object;
    SculptSession &ss = *object.runtime->sculpt_session;
    const float object_max = threading::parallel_reduce(
        IndexRange(vertex_count_get(object)),
        4096,
        std::numeric_limits<float>::lowest(),
        [&](const IndexRange range, float max) {
          for (const int vert : range) {
            if (state.vert_falloff[vert] == FLT_MAX) {
              continue;
            }
            if (!is_vert_in_active_component(ss, expand_cache, vert, i)) {
              continue;
            }
            max = std::max(max, state.vert_falloff[vert]);
          }
          return max;
        },
        [](const float a, const float b) { return std::max(a, b); });
    global_max = std::max(global_max, object_max);
  }
  expand_cache.max_vert_falloff = global_max;
}

/**
 * Updates the max_falloff value for faces in a Cache based on the current values of the
 * falloff, skipping any invalid values initialized to FLT_MAX and not initialized components.
 */
static void update_max_face_falloff_factor(const Object &object, Mesh &mesh, Cache &expand_cache)
{
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const ObjectState &state = active_object_state(expand_cache);
  expand_cache.max_face_falloff = threading::parallel_reduce(
      faces.index_range(),
      4096,
      std::numeric_limits<float>::lowest(),
      [&](const IndexRange range, float max) {
        for (const int face : range) {
          if (state.face_falloff[face] == FLT_MAX) {
            continue;
          }
          if (!is_face_in_active_component(object, faces, corner_verts, expand_cache, face)) {
            continue;
          }
          max = std::max(max, state.face_falloff[face]);
        }
        return max;
      },
      [](const float a, const float b) { return std::max(a, b); });
}

/* Global max over EVERY object's face_falloff (mirrors #update_max_vert_falloff_value_multi for
 * verts -- the max stays a single scalar for the whole operation). The active-component island
 * filter applies per object; a face whose island is not reachable (or that averages an unreached
 * FLT_MAX vertex) must not corrupt the global max. */
static void update_max_face_falloff_factor_multi(Cache &expand_cache)
{
  Span<ObjectState> states = expand_cache.object_states;
  float global_max = std::numeric_limits<float>::lowest();
  for (const ObjectState &state : states) {
    Object &object = *state.object;
    const Mesh &mesh = *id_cast<const Mesh *>(object.data);
    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();
    const float object_max = threading::parallel_reduce(
        faces.index_range(),
        4096,
        std::numeric_limits<float>::lowest(),
        [&](const IndexRange range, float max) {
          for (const int face : range) {
            if (state.face_falloff[face] == FLT_MAX) {
              continue;
            }
            if (!is_face_in_active_component(object, faces, corner_verts, expand_cache, face)) {
              continue;
            }
            max = std::max(max, state.face_falloff[face]);
          }
          return max;
        },
        [](const float a, const float b) { return std::max(a, b); });
    global_max = std::max(global_max, object_max);
  }
  expand_cache.max_face_falloff = global_max;
}

/**
 * Functions to get falloff values for faces from the values from the vertices. This is used for
 * expanding face sets. Depending on the data type of the #SculptSession, this needs to get the per
 * face falloff value from the connected vertices of each face or from the grids stored per loops
 * for each face.
 */
static void vert_to_face_falloff_grids(SculptSession &ss, Mesh *mesh, Cache &expand_cache)
{
  const OffsetIndices faces = mesh->faces();
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
  ObjectState &state = active_object_state(expand_cache);

  threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
    for (const int face : range) {
      float accum = 0.0f;
      for (const int corner : faces[face]) {
        const int grid_loop_index = corner * key.grid_area;
        for (int g = 0; g < key.grid_area; g++) {
          accum += state.vert_falloff[grid_loop_index + g];
        }
      }
      state.face_falloff[face] = accum / (faces[face].size() * key.grid_area);
    }
  });
}

static void vert_to_face_falloff_mesh(Mesh *mesh, Cache &expand_cache)
{
  const OffsetIndices faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();
  ObjectState &state = active_object_state(expand_cache);

  threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
    for (const int face : range) {
      const Span<int> face_verts = corner_verts.slice(faces[face]);
      float accum = 0.0f;
      for (const int vert : face_verts) {
        accum += state.vert_falloff[vert];
      }
      state.face_falloff[face] = accum / face_verts.size();
    }
  });
}

/**
 * Main function to update the faces falloff from a already calculated vertex falloff.
 */
static void vert_to_face_falloff(Object &object, Mesh *mesh, Cache &expand_cache)
{
  ObjectState &state = active_object_state(expand_cache);
  BLI_assert(!state.vert_falloff.is_empty());

  state.face_falloff.reinitialize(mesh->faces_num);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  if (pbvh.type() == bke::pbvh::Type::Mesh) {
    vert_to_face_falloff_mesh(mesh, expand_cache);
  }
  else if (pbvh.type() == bke::pbvh::Type::Grids) {
    vert_to_face_falloff_grids(*object.runtime->sculpt_session, mesh, expand_cache);
  }
  else {
    BLI_assert(false);
  }
}

/* Multi-object #vert_to_face_falloff: mirrors #vert_to_face_falloff_mesh/#vert_to_face_falloff_grids,
 * per object. Needed so the FaceSets target has a `face_falloff` to read from on every object, not
 * only the active one. Per-object pbvh-type branch added so a Grids object's `state.vert_falloff`
 * (raw CCG-indexed, per #world_positions_create/Task 5) is read through the matching grid-loop
 * convention instead of through `mesh.corner_verts()`'s base-mesh vertex indices -- the two index
 * spaces are unrelated sizes, so reading the Mesh convention for a Grids object silently pulled
 * wrong (but in-bounds, since raw CCG count exceeds base-mesh count) falloff values into
 * `face_falloff`, rather than crashing. BMesh is unreachable here (multi-object gate excludes it). */
static void vert_to_face_falloff_multi(Cache &expand_cache)
{
  for (ObjectState &state : expand_cache.object_states) {
    Object &object = *state.object;
    Mesh &mesh = *id_cast<Mesh *>(object.data);
    const OffsetIndices faces = mesh.faces();
    state.face_falloff.reinitialize(mesh.faces_num);

    if (bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Grids) {
      const SculptSession &ss = *object.runtime->sculpt_session;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
        for (const int face : range) {
          float accum = 0.0f;
          for (const int corner : faces[face]) {
            const int grid_loop_index = corner * key.grid_area;
            for (int g = 0; g < key.grid_area; g++) {
              accum += state.vert_falloff[grid_loop_index + g];
            }
          }
          state.face_falloff[face] = accum / (faces[face].size() * key.grid_area);
        }
      });
      continue;
    }

    const Span<int> corner_verts = mesh.corner_verts();
    threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
      for (const int face : range) {
        const Span<int> face_verts = corner_verts.slice(faces[face]);
        float accum = 0.0f;
        for (const int vert : face_verts) {
          accum += state.vert_falloff[vert];
        }
        state.face_falloff[face] = accum / face_verts.size();
      }
    });
  }
}

/* Recursions. These functions will generate new falloff values based on the state of the vertices
 * from the current Cache options and falloff values. */

/**
 * Geodesic recursion: Initializes falloff values using geodesic distances from the boundary of the
 * current vertices state.
 */
static void geodesics_from_state_boundary(const Depsgraph &depsgraph,
                                          Object &ob,
                                          Cache &expand_cache,
                                          const BitSpan enabled_verts)
{
  BLI_assert(bke::object::pbvh_get(ob)->type() == bke::pbvh::Type::Mesh);

  IndexMaskMemory memory;
  const IndexMask boundary_verts = boundary_from_enabled(ob, enabled_verts, false, memory);

  ObjectState &state = active_object_state(expand_cache);
  state.face_falloff = {};

  state.vert_falloff = geodesic_falloff_create(depsgraph, ob, boundary_verts);
}

/**
 * Topology recursion: Initializes falloff values using topology steps from the boundary of the
 * current vertices state, increasing the value by 1 each time a new vertex is visited.
 */
static void topology_from_state_boundary(Object &ob,
                                         Cache &expand_cache,
                                         const BitSpan enabled_verts)
{
  ObjectState &state = active_object_state(expand_cache);
  state.face_falloff = {};

  state.vert_falloff.reinitialize(vertex_count_get(ob));
  state.vert_falloff.fill(0);

  IndexMaskMemory memory;
  const IndexMask boundary_verts = boundary_from_enabled(ob, enabled_verts, false, memory);

  calc_topology_falloff_from_verts(ob, boundary_verts, state.vert_falloff);
}

/**
 * Main function to create a recursion step from the current #Cache state.
 */
static void resursion_step_add(const Depsgraph &depsgraph,
                               Object &ob,
                               Cache &expand_cache,
                               const RecursionType recursion_type)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return;
  }

  const BitVector<> enabled_verts = enabled_state_to_bitmap(depsgraph, ob, expand_cache);

  /* Each time a new recursion step is created, reset the distortion strength. This is the expected
   * result from the recursion, as otherwise the new falloff will render with undesired distortion
   * from the beginning. */
  expand_cache.texture_distortion_strength = 0.0f;

  switch (recursion_type) {
    case RecursionType::Geodesic:
      geodesics_from_state_boundary(depsgraph, ob, expand_cache, enabled_verts);
      break;
    case RecursionType::Topology:
      topology_from_state_boundary(ob, expand_cache, enabled_verts);
      break;
  }

  update_max_vert_falloff_value(ob, expand_cache);
  if (expand_cache.target == TargetType::FaceSets) {
    Mesh &mesh = *id_cast<Mesh *>(ob.data);
    vert_to_face_falloff(ob, &mesh, expand_cache);
    update_max_face_falloff_factor(ob, mesh, expand_cache);
  }
}

/* Face Set Boundary falloff. */

/**
 * When internal falloff is set to true, the falloff will fill the active face set with a gradient,
 * otherwise the active face set will be filled with a constant falloff of 0.0f.
 */
static void init_from_face_set_boundary(const Depsgraph &depsgraph,
                                        Object &ob,
                                        Cache &expand_cache,
                                        const int active_face_set,
                                        const bool internal_falloff)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const int totvert = vertex_count_get(ob);

  Array<bool> vert_has_face_set(totvert);
  Array<bool> vert_has_unique_face_set(totvert);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                           bke::AttrDomain::Face);
      threading::parallel_for(IndexRange(totvert), 1024, [&](const IndexRange range) {
        for (const int vert : range) {
          vert_has_face_set[vert] = face_set::vert_has_face_set(
              vert_to_face_map, face_sets, vert, active_face_set);
          vert_has_unique_face_set[vert] = face_set::vert_has_unique_face_set(
              vert_to_face_map, face_sets, vert);
        }
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Mesh &base_mesh = *id_cast<const Mesh *>(ob.data);
      const OffsetIndices<int> faces = base_mesh.faces();
      const Span<int> corner_verts = base_mesh.corner_verts();
      const GroupedSpan<int> vert_to_face_map = base_mesh.vert_to_face_map();
      const bke::AttributeAccessor attributes = base_mesh.attributes();
      const VArraySpan face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                           bke::AttrDomain::Face);
      const SubdivCCG &subdiv_ccg = *ob.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      threading::parallel_for(IndexRange(totvert), 1024, [&](const IndexRange range) {
        for (const int vert : range) {
          const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vert);
          vert_has_face_set[vert] = face_set::coord_has_face_set(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 face_sets,
                                                                 subdiv_ccg,
                                                                 coord,
                                                                 active_face_set);
          vert_has_unique_face_set[vert] = face_set::vert_has_unique_face_set(
              faces, corner_verts, vert_to_face_map, face_sets, subdiv_ccg, coord);
        }
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ob.runtime->sculpt_session->bm;
      const int offset = CustomData_get_offset_named(&bm.pdata, CD_PROP_INT32, ".sculpt_face_set");
      BM_mesh_elem_table_ensure(&bm, BM_FACE);
      threading::parallel_for(IndexRange(totvert), 1024, [&](const IndexRange range) {
        for (const int vert : range) {
          const BMVert *bm_vert = BM_vert_at_index(&bm, vert);
          vert_has_face_set[vert] = face_set::vert_has_face_set(offset, *bm_vert, active_face_set);
          vert_has_unique_face_set[vert] = face_set::vert_has_unique_face_set(offset, *bm_vert);
        }
      });
      break;
    }
  }

  BitVector<> enabled_verts(totvert);
  for (int i = 0; i < totvert; i++) {
    if (!vert_has_unique_face_set[i]) {
      continue;
    }
    if (!vert_has_face_set[i]) {
      continue;
    }
    enabled_verts[i].set();
  }

  if (pbvh.type() == bke::pbvh::Type::Mesh) {
    geodesics_from_state_boundary(depsgraph, ob, expand_cache, enabled_verts);
  }
  else {
    topology_from_state_boundary(ob, expand_cache, enabled_verts);
  }

  ObjectState &state = active_object_state(expand_cache);
  if (internal_falloff) {
    for (int i = 0; i < totvert; i++) {
      if (!(vert_has_face_set[i] && vert_has_unique_face_set[i])) {
        continue;
      }
      state.vert_falloff[i] *= -1.0f;
    }

    float min_factor = FLT_MAX;
    for (int i = 0; i < totvert; i++) {
      min_factor = min_ff(state.vert_falloff[i], min_factor);
    }

    const float additional_falloff = fabsf(min_factor);
    for (int i = 0; i < totvert; i++) {
      state.vert_falloff[i] += additional_falloff;
    }
  }
  else {
    for (int i = 0; i < totvert; i++) {
      if (!vert_has_face_set[i]) {
        continue;
      }
      state.vert_falloff[i] = 0.0f;
    }
  }
}

/**
 * Multi-object Face-Set-boundary falloff: mirrors #init_from_face_set_boundary's Mesh arm (multi
 * is Mesh-only), per object, but with ONE shared Geodesic propagation across the whole cross-mesh
 * graph so the falloff stays continuous over the bridge. Each object seeds from its own boundary
 * of `expand_cache.initial_active_face_set`; objects that do not contain that face-set id
 * contribute no seeds (empty subset -- no crash). The `internal_falloff` post-process
 * additional-offset uses the GLOBAL minimum across every object, not per object, so the metric
 * stays consistent across the bridge.
 */
static void face_set_boundary_falloff_multi(const Depsgraph &depsgraph,
                                             Cache &expand_cache,
                                             const bool internal_falloff)
{
  Span<ObjectState> states = expand_cache.object_states;
  Vector<Object *> objects;
  for (const ObjectState &state : states) {
    objects.append(state.object);
  }

  Vector<Array<bool>> vert_has_face_set(states.size());
  Vector<Array<bool>> vert_has_unique_face_set(states.size());
  Vector<MultiVertRef> seeds;

  for (const int i : states.index_range()) {
    Object &object = *states[i].object;
    const Mesh &mesh = *id_cast<const Mesh *>(object.data);
    const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
    const bke::AttributeAccessor attributes = mesh.attributes();
    const VArraySpan face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                         bke::AttrDomain::Face);

    const int totvert = vertex_count_get(object);
    Array<bool> has_face_set(totvert);
    Array<bool> has_unique_face_set(totvert);
    threading::parallel_for(IndexRange(totvert), 1024, [&](const IndexRange range) {
      for (const int vert : range) {
        has_face_set[vert] = face_set::vert_has_face_set(
            vert_to_face_map, face_sets, vert, expand_cache.initial_active_face_set);
        has_unique_face_set[vert] = face_set::vert_has_unique_face_set(
            vert_to_face_map, face_sets, vert);
      }
    });

    /* Seeds are the BOUNDARY of the enabled region (enabled verts with at least one
     * non-enabled neighbor), matching the single-object ground truth in
     * #init_from_face_set_boundary -- not every vert that merely has the face set. */
    BitVector<> enabled_verts(totvert);
    for (int vert = 0; vert < totvert; vert++) {
      if (has_face_set[vert] && has_unique_face_set[vert]) {
        enabled_verts[vert].set();
      }
    }

    IndexMaskMemory boundary_memory;
    const IndexMask boundary_verts = boundary_from_enabled(
        object, enabled_verts, false, boundary_memory);
    boundary_verts.foreach_index([&](const int vert) { seeds.append({i, vert}); });

    vert_has_face_set[i] = std::move(has_face_set);
    vert_has_unique_face_set[i] = std::move(has_unique_face_set);
  }

  Array<Array<int>> canonical_maps(states.size());
  for (const int i : states.index_range()) {
    canonical_maps[i] = states[i].grids_canonical_map;
  }
  Array<Array<float>> falloffs(states.size());
  multi_object_graph_propagate(depsgraph,
                               objects,
                               expand_cache.world_positions,
                               canonical_maps,
                               seeds,
                               expand_cache.bridge,
                               PropagationMode::Geodesic,
                               expand_cache.geodesic_topology_cache,
                               falloffs);
  for (const int i : states.index_range()) {
    expand_cache.object_states[i].vert_falloff = std::move(falloffs[i]);
  }

  if (internal_falloff) {
    for (const int i : states.index_range()) {
      ObjectState &state = expand_cache.object_states[i];
      for (const int vert : state.vert_falloff.index_range()) {
        if (!(vert_has_face_set[i][vert] && vert_has_unique_face_set[i][vert])) {
          continue;
        }
        state.vert_falloff[vert] *= -1.0f;
      }
    }

    /* GLOBAL minimum across every object (not per object) so the additional-offset keeps the
     * falloff metric consistent across the bridge. */
    float min_factor = FLT_MAX;
    for (const ObjectState &state : expand_cache.object_states) {
      for (const float value : state.vert_falloff) {
        min_factor = min_ff(value, min_factor);
      }
    }
    const float additional_falloff = fabsf(min_factor);
    for (ObjectState &state : expand_cache.object_states) {
      for (float &value : state.vert_falloff) {
        value += additional_falloff;
      }
    }
  }
  else {
    for (const int i : states.index_range()) {
      ObjectState &state = expand_cache.object_states[i];
      for (const int vert : state.vert_falloff.index_range()) {
        if (!vert_has_face_set[i][vert]) {
          continue;
        }
        state.vert_falloff[vert] = 0.0f;
      }
    }
  }
}

/**
 * Multi-object Normals falloff: mirrors #normals_falloff_create's accumulation formula (Mesh arm)
 * exactly, but flooded across a single combined graph = every object's edge adjacency PLUS the
 * cross-mesh `bridge` (both directions), so the falloff stays continuous over the seam. Crossing
 * the bridge is inherently HEURISTIC -- the two surfaces' normals are unrelated at the seam -- and
 * that is ACCEPTABLE per spec §6.3. Mesh-only (multi is Mesh-only throughout, matching
 * #face_set_boundary_falloff_multi). The traversal order does NOT need to match the single-object
 * `FillDataMesh` flood -- single-object stays on the old, untouched code path.
 */
static void normals_falloff_multi(const Depsgraph &depsgraph,
                                  Cache &expand_cache,
                                  const float edge_sensitivity,
                                  const int blur_steps)
{
  Span<ObjectState> states = expand_cache.object_states;
  const int object_num = states.size();

  /* Step 1: WORLD-space vertex normals per object. The transpose of `world_to_object`'s 3x3 linear
   * part is the correct normal matrix under non-uniform scale (same pattern as the single-object
   * world-normal transforms elsewhere in sculpt.cc). */
  Array<Array<float3>> world_normals(object_num);
  for (const int i : states.index_range()) {
    Object &object = *states[i].object;
    const Span<float3> local_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
    const float3x3 normal_matrix = math::transpose(float3x3(object.world_to_object()));
    Array<float3> normals(local_normals.size());
    threading::parallel_for(local_normals.index_range(), 1024, [&](const IndexRange range) {
      for (const int v : range) {
        normals[v] = math::normalize(normal_matrix * local_normals[v]);
      }
    });
    world_normals[i] = std::move(normals);
  }

  /* Step 2: per-object working state, matching #normals_falloff_create's initialization. */
  Array<Array<float>> dists(object_num);
  Array<Array<float>> edge_factors(object_num);
  Array<BitVector<>> visited(object_num);
  for (const int i : states.index_range()) {
    const int totvert = vertex_count_get(*states[i].object);
    dists[i] = Array<float>(totvert, 0.0f);
    edge_factors[i] = Array<float>(totvert, 1.0f);
    visited[i] = BitVector<>(totvert);
  }

  /* Step 3: per-object, per-vertex edge-adjacency list, built once up front. A plain nested Vector
   * (rather than the shared BFS core's #GroupedSpan machinery) is enough here -- object counts are
   * small and this flood is heuristic across the bridge regardless. */
  Vector<Vector<Vector<int>>> neighbors(object_num);
  for (const int i : states.index_range()) {
    const Mesh &mesh = *id_cast<const Mesh *>(states[i].object->data);
    Vector<Vector<int>> vert_neighbors(mesh.verts_num);
    for (const int2 &edge : mesh.edges()) {
      vert_neighbors[edge[0]].append(edge[1]);
      vert_neighbors[edge[1]].append(edge[0]);
    }
    neighbors[i] = std::move(vert_neighbors);
  }

  /* Step 4: per-vertex cross-object bridge partner list, same pattern as
   * #detail::propagate_uniform (bridge edges are undirected, recorded on both endpoints). */
  Vector<Vector<Vector<MultiVertRef>>> partners(object_num);
  for (const int i : states.index_range()) {
    partners[i] = Vector<Vector<MultiVertRef>>(vertex_count_get(*states[i].object));
  }
  for (const BridgeEdge &edge : expand_cache.bridge.edges) {
    partners[edge.a.object_index][edge.a.vert].append(edge.b);
    partners[edge.b.object_index][edge.b.vert].append(edge.a);
  }

  /* Step 5: symmetry-resolved seeds, same as the other multi falloffs. The primary seed
   * (`expand_cache.seed`) supplies `orig_normal`, matching the single-object function where
   * `orig_normal` always comes from the requested `vert`, not from a mirrored symmetry pass. */
  const Vector<MultiVertRef> seeds = find_symm_multi_verts(
      depsgraph, states, expand_cache.world_positions, expand_cache.seed, FLT_MAX);
  const float3 orig_normal =
      world_normals[expand_cache.seed.object_index][expand_cache.seed.vert];

  std::queue<MultiVertRef> queue;
  for (const MultiVertRef &seed : seeds) {
    /* Mark visited without touching `dists` -- a seed's falloff stays at the 0.0f initial value,
     * matching the single-object flood where only `to_vert` (never the seed itself) is written. */
    if (visited[seed.object_index][seed.vert]) {
      continue;
    }
    visited[seed.object_index][seed.vert].set();
    queue.push(seed);
  }

  /* Step 6: combined flood. Relaxing a `to` vertex reproduces #normals_falloff_create's
   * accumulation formula exactly (from_edge_factor powf, edge_factors update, clamp); the bridge
   * partners let the accumulation cross the seam. */
  auto relax = [&](const float from_edge_factor, const float3 &from_normal,
                   const MultiVertRef &to) {
    if (visited[to.object_index][to.vert]) {
      return;
    }
    const float3 &to_normal = world_normals[to.object_index][to.vert];
    const float dist = math::dot(orig_normal, to_normal) *
                       powf(from_edge_factor, edge_sensitivity);
    edge_factors[to.object_index][to.vert] = math::dot(to_normal, from_normal) *
                                             from_edge_factor;
    dists[to.object_index][to.vert] = std::clamp(dist, 0.0f, 1.0f);
    visited[to.object_index][to.vert].set();
    queue.push(to);
  };

  while (!queue.empty()) {
    const MultiVertRef from = queue.front();
    queue.pop();
    const float from_edge_factor = edge_factors[from.object_index][from.vert];
    const float3 &from_normal = world_normals[from.object_index][from.vert];

    for (const int neighbor_vert : neighbors[from.object_index][from.vert]) {
      relax(from_edge_factor, from_normal, {from.object_index, neighbor_vert});
    }
    for (const MultiVertRef &partner : partners[from.object_index][from.vert]) {
      relax(from_edge_factor, from_normal, partner);
    }
  }

  /* Step 7: per-object blur + final inversion, matching #normals_falloff_create's tail exactly. */
  for (const int i : states.index_range()) {
    smooth::blur_geometry_data_array(*states[i].object, blur_steps, dists[i]);
    for (float &value : dists[i]) {
      value = 1.0f - value;
    }
    expand_cache.object_states[i].vert_falloff = std::move(dists[i]);
  }
}

/* Falloff types whose multi-object cross-mesh falloff is implemented so far. Types not listed
 * fall back to the single-object (active-object-only) path even in multi-object mode (no crash).
 *
 * No `default:` case: every #FalloffType value is listed explicitly (all currently return true --
 * multi-object falloff is implemented for all of them). A new #FalloffType value that lands here
 * must be triaged into `true`/`false` explicitly; a `default:` would have silently routed it to
 * the single-object fallback with no compiler diagnostic (Architecture_Refactoring_Analysis.md
 * 5.5). */
static bool falloff_type_supports_multi(const FalloffType falloff_type)
{
  switch (falloff_type) {
    case FalloffType::Sphere:
    case FalloffType::Geodesic:
    case FalloffType::Topology:
    case FalloffType::TopologyNormals:
    case FalloffType::BoundaryTopology:
    case FalloffType::ActiveFaceSet:
    case FalloffType::BoundaryFaceSet:
    case FalloffType::Normals:
      return true;
  }
  BLI_assert_unreachable();
  return false;
}

/**
 * Main function to initialize new falloff values in a #Cache given an initial vertex and a
 * falloff type.
 */
static void calc_falloff_from_vert_and_symmetry(const Depsgraph &depsgraph,
                                                Cache &expand_cache,
                                                Object &ob,
                                                const int vert,
                                                FalloffType falloff_type)
{
  if (expand_multi_object_active(expand_cache) && falloff_type_supports_multi(falloff_type)) {
    expand_cache.falloff_type = falloff_type;
    if (falloff_type == FalloffType::Sphere ||
        (falloff_type == FalloffType::Geodesic && any_grids_backed(expand_cache.object_states)))
    {
      spherical_falloff_multi(depsgraph, expand_cache);
    }
    else if (falloff_type == FalloffType::ActiveFaceSet ||
             falloff_type == FalloffType::BoundaryFaceSet)
    {
      /* `internal_falloff` must match the single-object switch below EXACTLY: BoundaryFaceSet
       * passes `true`, ActiveFaceSet passes `false` (counter-intuitive given the type names --
       * verified against the single-object `case` arms, not guessed). */
      const bool internal_falloff = falloff_type == FalloffType::BoundaryFaceSet;
      face_set_boundary_falloff_multi(depsgraph, expand_cache, internal_falloff);
    }
    else if (falloff_type == FalloffType::Normals) {
      /* Normals is a normal-weighted accumulation, not a distance propagation -- it does not go
       * through #multi_object_graph_propagate at all (see #normals_falloff_multi). */
      normals_falloff_multi(depsgraph,
                            expand_cache,
                            SCULPT_EXPAND_NORMALS_FALLOFF_EDGE_SENSITIVITY,
                            expand_cache.normal_falloff_blur_steps);
    }
    else {
      Span<ObjectState> states = expand_cache.object_states;
      Vector<Object *> objects;
      for (const ObjectState &state : states) {
        objects.append(state.object);
      }
      /* Symmetry-resolved seeds across all objects. */
      Vector<MultiVertRef> seeds = find_symm_multi_verts(
          depsgraph, states, expand_cache.world_positions, expand_cache.seed, FLT_MAX);
      if (falloff_type == FalloffType::BoundaryTopology) {
        /* Expand each seed to its object's boundary vertex set (mirrors
         * boundary_topology_falloff_create, per object). */
        Vector<MultiVertRef> boundary_seeds;
        for (const MultiVertRef &s : seeds) {
          Object &object = *states[s.object_index].object;
          if (std::unique_ptr<boundary::SculptBoundary> boundary = boundary::data_init(
                  depsgraph, object, nullptr, s.vert, FLT_MAX))
          {
            for (const int bvert : boundary->verts) {
              boundary_seeds.append({s.object_index, bvert});
            }
          }
        }
        seeds = std::move(boundary_seeds);
      }
      /* TopologyNormals ("Topology Diagonals") is a BFS through face corners, not a
       * normal-weighted falloff -- it reuses the Uniform (unit-weight) BFS core with diagonal
       * (shared-face) adjacency instead of edge adjacency; see #PropagationMode. No boundary
       * expansion, same as Topology. */
      const PropagationMode mode = falloff_type == FalloffType::Geodesic ?
                                       PropagationMode::Geodesic :
                                       (falloff_type == FalloffType::TopologyNormals ?
                                            PropagationMode::UniformDiagonals :
                                            PropagationMode::Uniform);
      Array<Array<int>> canonical_maps(states.size());
      for (const int i : states.index_range()) {
        canonical_maps[i] = states[i].grids_canonical_map;
      }
      Array<Array<float>> falloffs(states.size());
      multi_object_graph_propagate(depsgraph,
                                   objects,
                                   expand_cache.world_positions,
                                   canonical_maps,
                                   seeds,
                                   expand_cache.bridge,
                                   mode,
                                   expand_cache.geodesic_topology_cache,
                                   falloffs);
      for (const int i : states.index_range()) {
        expand_cache.object_states[i].vert_falloff = std::move(falloffs[i]);
      }
    }
    update_max_vert_falloff_value_multi(expand_cache);
    if (expand_cache.target == TargetType::FaceSets) {
      vert_to_face_falloff_multi(expand_cache);
      update_max_face_falloff_factor_multi(expand_cache);
    }
    return;
  }

  expand_cache.falloff_type = falloff_type;

  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const bool has_topology_info = pbvh.type() == bke::pbvh::Type::Mesh;
  ObjectState &state = active_object_state(expand_cache);

  switch (falloff_type) {
    case FalloffType::Geodesic:
      state.vert_falloff = has_topology_info ?
                                geodesic_falloff_create(depsgraph, ob, vert) :
                                spherical_falloff_create(depsgraph, ob, vert);
      break;
    case FalloffType::Topology:
      state.vert_falloff = topology_falloff_create(depsgraph, ob, vert);
      break;
    case FalloffType::TopologyNormals:
      state.vert_falloff = has_topology_info ?
                                diagonals_falloff_create(depsgraph, ob, vert) :
                                topology_falloff_create(depsgraph, ob, vert);
      break;
    case FalloffType::Normals:
      state.vert_falloff = normals_falloff_create(
          depsgraph,
          ob,
          vert,
          SCULPT_EXPAND_NORMALS_FALLOFF_EDGE_SENSITIVITY,
          expand_cache.normal_falloff_blur_steps);
      break;
    case FalloffType::Sphere:
      state.vert_falloff = spherical_falloff_create(depsgraph, ob, vert);
      break;
    case FalloffType::BoundaryTopology:
      state.vert_falloff = boundary_topology_falloff_create(depsgraph, ob, vert);
      break;
    case FalloffType::BoundaryFaceSet:
      init_from_face_set_boundary(
          depsgraph, ob, expand_cache, expand_cache.initial_active_face_set, true);
      break;
    case FalloffType::ActiveFaceSet:
      init_from_face_set_boundary(
          depsgraph, ob, expand_cache, expand_cache.initial_active_face_set, false);
      break;
  }

  /* Update max falloff values and propagate to base mesh faces if needed. */
  update_max_vert_falloff_value(ob, expand_cache);
  if (expand_cache.target == TargetType::FaceSets) {
    Mesh &mesh = *id_cast<Mesh *>(ob.data);
    vert_to_face_falloff(ob, &mesh, expand_cache);
    update_max_face_falloff_factor(ob, mesh, expand_cache);
  }
}

/**
 * Adds to the snapping face set `gset` all face sets which contain all enabled vertices for the
 * current #Cache state. This improves the usability of snapping, as already enabled
 * elements won't switch their state when toggling snapping with the modal key-map.
 */
static void snap_init_from_enabled(const Depsgraph &depsgraph,
                                   const Object &object,
                                   Cache &expand_cache)
{
  if (expand_multi_object_active(expand_cache)) {
    /* Global two-phase across all objects: face-set ids are shared, so an id must be dropped from
     * the snap set if it has any disabled vertex on ANY object, regardless of iteration order. */
    const bool prev_snap_state = expand_cache.snap;
    const bool prev_invert_state = expand_cache.invert;
    expand_cache.snap = false;
    expand_cache.invert = false;

    /* Phase 1: add every face-set id present on every object. Grids objects are skipped here (and
     * in Phase 2 below) to match the single-object path a few lines down, which returns early
     * ("no snap support") for any non-Mesh pbvh -- Snap face-set init has never been implemented
     * for Multires. Contributing a Grids object's ids in Phase 1 without Phase 2 being able to
     * correctly evaluate "any disabled vertex" for it (its `corner_verts()` are base-mesh indices,
     * but `enabled_verts` from #enabled_state_to_bitmap is raw-CCG-indexed for Grids -- reading one
     * through the other is in-bounds but semantically wrong, not a crash) would leave some ids
     * that should have been removed incorrectly stuck in the snap set. */
    for (const ObjectState &state : expand_cache.object_states) {
      if (bke::object::pbvh_get(*state.object)->type() != bke::pbvh::Type::Mesh) {
        continue;
      }
      const Mesh &mesh = *id_cast<const Mesh *>(state.object->data);
      const OffsetIndices<int> faces = mesh.faces();
      for (const int i : faces.index_range()) {
        expand_cache.snap_enabled_face_sets->add(state.original_face_sets[i]);
      }
    }
    /* Phase 2: remove any id that has a disabled vertex on any object. */
    for (const ObjectState &state : expand_cache.object_states) {
      if (bke::object::pbvh_get(*state.object)->type() != bke::pbvh::Type::Mesh) {
        continue;
      }
      const Mesh &mesh = *id_cast<const Mesh *>(state.object->data);
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int> corner_verts = mesh.corner_verts();
      const BitVector<> enabled_verts = enabled_state_to_bitmap(
          depsgraph, *state.object, expand_cache);
      for (const int i : faces.index_range()) {
        const Span<int> face_verts = corner_verts.slice(faces[i]);
        const bool any_disabled = std::any_of(
            face_verts.begin(), face_verts.end(), [&](const int vert) {
              return !enabled_verts[vert];
            });
        if (any_disabled) {
          expand_cache.snap_enabled_face_sets->remove(state.original_face_sets[i]);
        }
      }
    }

    expand_cache.snap = prev_snap_state;
    expand_cache.invert = prev_invert_state;
    return;
  }

  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  /* Make sure this code runs with snapping and invert disabled. This simplifies the code and
   * prevents using this function with snapping already enabled. */
  const bool prev_snap_state = expand_cache.snap;
  const bool prev_invert_state = expand_cache.invert;
  expand_cache.snap = false;
  expand_cache.invert = false;

  const BitVector<> enabled_verts = enabled_state_to_bitmap(depsgraph, object, expand_cache);
  const ObjectState &state = active_object_state(expand_cache);

  for (const int i : faces.index_range()) {
    const int face_set = state.original_face_sets[i];
    expand_cache.snap_enabled_face_sets->add(face_set);
  }

  for (const int i : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[i]);
    const bool any_disabled = std::any_of(face_verts.begin(),
                                          face_verts.end(),
                                          [&](const int vert) { return !enabled_verts[vert]; });
    if (any_disabled) {
      const int face_set = state.original_face_sets[i];
      expand_cache.snap_enabled_face_sets->remove(face_set);
    }
  }

  expand_cache.snap = prev_snap_state;
  expand_cache.invert = prev_invert_state;
}

static void expand_cache_free(SculptSession &ss)
{
  MEM_delete<Cache>(ss.expand_cache);
  /* Needs to be set to nullptr as the paint cursor relies on checking this pointer detecting if an
   * expand operation is running. */
  ss.expand_cache = nullptr;
}

/**
 * Functions to restore the original state from the #Cache when canceling the operator.
 */
static void restore_face_set_data(Object &object, Cache &expand_cache)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(
      *id_cast<Mesh *>(object.data));
  face_sets.span.copy_from(object_state_get(expand_cache, object).original_face_sets);
  face_sets.finish();

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  pbvh.tag_face_sets_changed(node_mask);
}

static void restore_color_data(Object &ob, Cache &expand_cache)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  bke::GSpanAttributeWriter color_attribute = color::active_color_attribute_for_write(mesh);
  const ObjectState &state = object_state_get(expand_cache, ob);
  node_mask.foreach_index([&](const int i) {
    for (const int vert : nodes[i].verts()) {
      color::color_vert_set(faces,
                            corner_verts,
                            vert_to_face_map,
                            color_attribute.domain,
                            vert,
                            state.original_colors[vert],
                            color_attribute.span);
    }
  });
  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

static void write_mask_data(Depsgraph *depsgraph, Object &object, const Span<float> mask)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      attributes.remove(".sculpt_mask");
      attributes.add<float>(".sculpt_mask",
                            bke::AttrDomain::Point,
                            bke::AttributeInitVArray(VArray<float>::from_span(mask)));
      bke::pbvh::update_mask_mesh(mesh, node_mask, pbvh);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ss.bm;
      const int offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
      vert_random_access_ensure(object);
      for (const int i : mask.index_range()) {
        BM_ELEM_CD_SET_FLOAT(BM_vert_at_index(&bm, i), offset, mask[i]);
      }
      bke::pbvh::update_mask_bmesh(bm, node_mask, pbvh);
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      subdiv_ccg.masks.as_mutable_span().copy_from(mask);
      bke::pbvh::update_mask_grids(subdiv_ccg, node_mask, pbvh);
      /* Writing #SubdivCCG::masks alone does not survive the next depsgraph re-evaluation: the
       * change is flushed back to the persistent base-cage only when the object is marked with
       * #MULTIRES_COORDS_MODIFIED (see the matching apply-path comment in #update_for_vert). Both
       * the Cancel/Esc restore path and the invoke-time auto-mask reach this branch for a Grids
       * object, so without the mark a secondary Multires object's restored mask is silently
       * discarded on the depsgraph re-eval triggered by the following #flush_update. */
      multires_mark_as_modified(depsgraph, &object, MULTIRES_COORDS_MODIFIED);
      break;
    }
  }
  pbvh.tag_masks_changed(node_mask);
}

/* Main function to restore the original state of the data to how it was before starting the expand
 * operation. */
static void restore_original_state(bContext *C, Object &ob, Cache &expand_cache)
{
  if (expand_multi_object_active(expand_cache)) {
    /* Restore every object's own original target data, then flush the viewport once (step) and
     * finalize per object (done), mirroring the single-object order below. */
    for (const ObjectState &state : expand_cache.object_states) {
      Object &object = *state.object;
      switch (expand_cache.target) {
        case TargetType::Mask:
          write_mask_data(CTX_data_depsgraph_pointer(C), object, state.original_mask);
          break;
        case TargetType::FaceSets:
          restore_face_set_data(object, expand_cache);
          break;
        case TargetType::Colors:
          /* Colors is Mesh-only (see #original_state_store_for_object's matching guard) --
           * #restore_color_data unconditionally does `pbvh.nodes<bke::pbvh::MeshNode>()`, which
           * throws `std::bad_variant_access` (not a plain null-deref, but just as fatal) for a
           * Grids object's pbvh. Skip it here the same way the apply path already does. */
          if (bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Mesh) {
            restore_color_data(object, expand_cache);
          }
          break;
      }
    }
    switch (expand_cache.target) {
      case TargetType::Mask:
        flush_update_step(C, UpdateType::Mask);
        break;
      case TargetType::FaceSets:
        flush_update_step(C, UpdateType::FaceSet);
        break;
      case TargetType::Colors:
        flush_update_step(C, UpdateType::Color);
        break;
    }
    for (const ObjectState &state : expand_cache.object_states) {
      switch (expand_cache.target) {
        case TargetType::Mask:
          flush_update_done(C, *state.object, UpdateType::Mask);
          break;
        case TargetType::FaceSets:
          flush_update_done(C, *state.object, UpdateType::FaceSet);
          break;
        case TargetType::Colors:
          flush_update_done(C, *state.object, UpdateType::Color);
          break;
      }
    }
    if (expand_cache.target != TargetType::Colors) {
      /* The single-object Colors case does not tag overlays; match it. */
      tag_update_overlays(C);
    }
    return;
  }

  switch (expand_cache.target) {
    case TargetType::Mask:
      write_mask_data(CTX_data_depsgraph_pointer(C), ob, active_object_state(expand_cache).original_mask);
      flush_update_step(C, UpdateType::Mask);
      flush_update_done(C, ob, UpdateType::Mask);
      tag_update_overlays(C);
      break;
    case TargetType::FaceSets:
      restore_face_set_data(ob, expand_cache);
      flush_update_step(C, UpdateType::FaceSet);
      flush_update_done(C, ob, UpdateType::FaceSet);
      tag_update_overlays(C);
      break;
    case TargetType::Colors:
      restore_color_data(ob, expand_cache);
      flush_update_step(C, UpdateType::Color);
      flush_update_done(C, ob, UpdateType::Color);
      break;
  }
}

/**
 * Cancel operator callback.
 */
static void sculpt_expand_cancel(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;

  restore_original_state(C, ob, *ss.expand_cache);
  if (expand_multi_object_active(*ss.expand_cache)) {
    undo::push_end_all_ex(false, true);
  }
  else {
    undo::push_end(ob);
  }
  expand_cache_free(ss);

  ED_workspace_status_text(C, nullptr);
}

/* Functions to update the sculpt mesh data. */

static void calc_new_mask_mesh(const SculptSession &ss,
                               const Cache &expand_cache,
                               const ObjectState &state,
                               const Span<float3> positions,
                               const BitSpan enabled_verts,
                               const Span<int> verts,
                               const MutableSpan<float> mask,
                               const int object_index)
{
  /* `expand_cache` is passed in explicitly (not `*ss.expand_cache`): in the multi-object apply loop
   * `ss` is a secondary object's session, whose `expand_cache` pointer is null (only the active
   * object owns the #Cache). */
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    const bool enabled = enabled_verts[vert];

    if (expand_cache.check_islands &&
        !is_vert_in_active_component(ss, expand_cache, vert, object_index))
    {
      continue;
    }

    if (enabled) {
      mask[i] = gradient_value_get(ss, state, expand_cache, positions[vert], vert);
    }
    else {
      mask[i] = 0.0f;
    }

    if (expand_cache.preserve) {
      if (expand_cache.invert) {
        mask[i] = min_ff(mask[i], state.original_mask[vert]);
      }
      else {
        mask[i] = max_ff(mask[i], state.original_mask[vert]);
      }
    }
  }

  mask::clamp_mask(mask);
}

static bool update_mask_grids(const SculptSession &ss,
                              const Cache &expand_cache,
                              const ObjectState &state,
                              const int object_index,
                              const BitSpan enabled_verts,
                              bke::pbvh::GridsNode &node,
                              SubdivCCG &subdiv_ccg)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float3> positions = subdiv_ccg.positions;
  MutableSpan<float> masks = subdiv_ccg.masks;

  bool any_changed = false;
  for (const int grid : node.grids()) {
    for (const int vert : bke::ccg::grid_range(key, grid)) {
      const float initial_mask = masks[vert];

      /* `object_index` is passed in explicitly (not hardcoded to 0) so this also works for a
       * SECONDARY Grids object in a multi-object Expand group -- see the multi-object apply loop's
       * Grids case, which passes each object's own index. */
      if (expand_cache.check_islands &&
          !is_vert_in_active_component(ss, expand_cache, vert, object_index))
      {
        continue;
      }

      float new_mask;

      if (enabled_verts[vert]) {
        new_mask = gradient_value_get(ss, state, expand_cache, positions[vert], vert);
      }
      else {
        new_mask = 0.0f;
      }

      if (expand_cache.preserve) {
        if (expand_cache.invert) {
          new_mask = min_ff(new_mask, state.original_mask[vert]);
        }
        else {
          new_mask = max_ff(new_mask, state.original_mask[vert]);
        }
      }

      if (new_mask == initial_mask) {
        continue;
      }

      masks[vert] = clamp_f(new_mask, 0.0f, 1.0f);
      any_changed = true;
    }
  }
  if (any_changed) {
    bke::pbvh::node_update_mask_grids(key, masks, node);
  }
  return any_changed;
}

static float calc_new_mask_bmesh(const SculptSession &ss,
                                 const Cache &expand_cache,
                                 const float old_mask,
                                 const BitSpan enabled_verts,
                                 const BMVert *vert)
{
  const int vert_index = BM_elem_index_get(vert);
  const ObjectState &state = active_object_state(expand_cache);
  /* BMesh is never multi-object (#expand_multi_object_active is Mesh-only), so this always runs
   * on the active object (index 0). */
  if (expand_cache.check_islands &&
      !is_vert_in_active_component(ss, expand_cache, vert_index, 0))
  {
    return old_mask;
  }
  float new_mask;
  if (enabled_verts[vert_index]) {
    new_mask = gradient_value_get(ss, state, expand_cache, vert->co, vert_index);
  }
  else {
    new_mask = 0.0f;
  }
  if (expand_cache.preserve) {
    if (expand_cache.invert) {
      new_mask = min_ff(new_mask, state.original_mask[vert_index]);
    }
    else {
      new_mask = max_ff(new_mask, state.original_mask[vert_index]);
    }
  }
  return clamp_f(new_mask, 0.0f, 1.0f);
}

static bool update_mask_bmesh(SculptSession &ss,
                              const BitSpan enabled_verts,
                              const int mask_offset,
                              const Span<float> old_mask,
                              bke::pbvh::BMeshNode *node)
{
  const Cache &expand_cache = *ss.expand_cache;

  bool any_changed = false;
  for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node)) {
    const float initial_mask = BM_ELEM_CD_GET_FLOAT(vert, mask_offset);
    float new_mask = calc_new_mask_bmesh(ss, expand_cache, initial_mask, enabled_verts, vert);
    if (new_mask != initial_mask) {
      any_changed = true;
    }
    BM_ELEM_CD_SET_FLOAT(vert, mask_offset, new_mask);
  }
  if (!any_changed) {
    int i = 0;
    for (BMVert *vert : BKE_pbvh_bmesh_node_other_verts(node)) {
      if (calc_new_mask_bmesh(ss, expand_cache, old_mask[i], enabled_verts, vert) != old_mask[i]) {
        any_changed = true;
        break;
      }
      i++;
    }
  }
  if (!any_changed) {
    return false;
  }
  bke::pbvh::node_update_mask_bmesh(mask_offset, *node);
  return true;
}

/**
 * Update Face Set data. Not multi-threaded per node as nodes don't contain face arrays.
 */
static void face_sets_update(Object &object, Cache &expand_cache)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(mesh);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  for (const int f : face_sets.span.index_range()) {
    const bool enabled = face_state_get(
        object, faces, corner_verts, hide_poly, face_sets.span, expand_cache, f);
    if (!enabled) {
      continue;
    }
    if (expand_cache.preserve) {
      face_sets.span[f] += expand_cache.next_face_set;
    }
    else {
      face_sets.span[f] = expand_cache.next_face_set;
    }
  }

  face_sets.finish();

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  /* Tags `object`'s own nodes (single object ⇒ `object` is the active object ⇒ identical to
   * today's `active_object_state`). */
  pbvh.tag_face_sets_changed(object_state_get(expand_cache, object).node_mask);
}

/**
 * Callback to update vertex colors per bke::pbvh::Tree node.
 */
static bool colors_update_task(const Depsgraph & /*depsgraph*/,
                               Object &object,
                               const Cache &expand_cache,
                               const Span<float3> vert_positions,
                               const OffsetIndices<int> faces,
                               const Span<int> corner_verts,
                               const GroupedSpan<int> vert_to_face_map,
                               const Span<bool> hide_vert,
                               const Span<float> mask,
                               bke::pbvh::MeshNode *node,
                               bke::GSpanAttributeWriter &color_attribute,
                               const BitSpan enabled_verts)
{
  /* `expand_cache` is passed in explicitly (not `object`'s session `expand_cache`): in the
   * multi-object apply loop `object` may be a secondary object whose session's `expand_cache`
   * pointer is null (only the active object owns the #Cache). */
  const SculptSession &ss = *object.runtime->sculpt_session;
  /* Resolving via `object` (rather than always the active object) makes Color read the correct
   * per-object state as a free side effect of threading `state` through the falloff readers.
   * Single object ⇒ `object` == active ⇒ identical to today's `active_object_state`. */
  const ObjectState &state = object_state_get(expand_cache, object);

  /* Hoisted to once per object by the caller (Task 4.1, spec §9 perf) instead of once per node. */

  bool any_changed = false;
  const Span<int> verts = node->verts();
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    if (!hide_vert.is_empty() && hide_vert[vert]) {
      continue;
    }

    float4 initial_color = color::color_vert_get(
        faces, corner_verts, vert_to_face_map, color_attribute.span, color_attribute.domain, vert);

    float fade;

    if (enabled_verts[vert]) {
      fade = gradient_value_get(ss, state, expand_cache, vert_positions[vert], vert);
    }
    else {
      fade = 0.0f;
    }

    if (!mask.is_empty()) {
      fade *= 1.0f - mask[vert];
    }
    fade = clamp_f(fade, 0.0f, 1.0f);

    float4 final_color;
    float4 final_fill_color;
    mul_v4_v4fl(final_fill_color, expand_cache.fill_color, fade);
    IMB_blend_color_float(final_color,
                          state.original_colors[vert],
                          final_fill_color,
                          IMB_BlendMode(expand_cache.blend_mode));

    if (initial_color == final_color) {
      continue;
    }

    color::color_vert_set(faces,
                          corner_verts,
                          vert_to_face_map,
                          color_attribute.domain,
                          vert,
                          final_color,
                          color_attribute.span);

    any_changed = true;
  }
  return any_changed;
}

/* Store the original mesh data state for one object into its own #ObjectState. Shared by
 * #original_state_store for every object in #Cache::object_states; a one-element loop reproduces
 * today's single-object body exactly, so this needs no multi-object gate. */
static void original_state_store_for_object(Cache &expand_cache, ObjectState &state)
{
  Object &ob = *state.object;
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  const int totvert = vertex_count_get(ob);

  face_set::create_face_sets_mesh(ob);

  /* Face sets are always stored as they are needed for snapping. */
  state.initial_face_sets = face_set::duplicate_face_sets(mesh);
  state.original_face_sets = face_set::duplicate_face_sets(mesh);

  if (expand_cache.target == TargetType::Mask) {
    state.original_mask = mask::duplicate_mask(ob);
  }

  if (expand_cache.target == TargetType::Colors &&
      bke::object::pbvh_get(ob)->type() == bke::pbvh::Type::Mesh)
  {
    /* Colors is Mesh-only even in single-object Expand (no per-pbvh-type branch exists for it
     * anywhere in this file) -- vertex-color painting on a Multires object isn't supported by this
     * codebase today. Guarding here keeps the multi-object path no worse than single-object
     * (skips a Grids object instead of indexing its much larger flattened CCG vertex range through
     * arrays sized to the BASE mesh's vertex count). Fixing Colors-on-Multires itself is a
     * separate, pre-existing gap, out of scope here. */
    const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();
    const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
    const bke::GAttributeReader color_attribute = color::active_color_attribute(mesh);
    const GVArraySpan colors = *color_attribute;

    state.original_colors = Array<float4>(totvert);
    for (int i = 0; i < totvert; i++) {
      state.original_colors[i] = color::color_vert_get(
          faces, corner_verts, vert_to_face_map, colors, color_attribute.domain, i);
    }
  }
}

/* Store the original mesh data state in the expand cache. */
static void original_state_store(Object & /*ob*/, Cache &expand_cache)
{
  for (ObjectState &state : expand_cache.object_states) {
    original_state_store_for_object(expand_cache, state);
  }
}

/**
 * Restore the state of the face sets before a new update.
 */
static void face_sets_restore(Object &object, Cache &expand_cache)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(mesh);
  /* Restores `object`'s own original/initial face sets (single object ⇒ `object` is the active
   * object ⇒ identical to today's `active_object_state`). */
  const ObjectState &state = object_state_get(expand_cache, object);
  for (const int i : faces.index_range()) {
    if (state.original_face_sets[i] <= 0) {
      /* Do not modify hidden face sets, even when restoring the IDs state. */
      continue;
    }
    if (!is_face_in_active_component(object, faces, corner_verts, expand_cache, i)) {
      continue;
    }
    face_sets.span[i] = state.initial_face_sets[i];
  }
  face_sets.finish();
}

static void update_for_vert(bContext *C, Object &ob, const MultiVertRef vertex)
{
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  Cache &expand_cache = *ss.expand_cache;
  const ObjectState &state = active_object_state(expand_cache);

  /* Update the active factor in the cache. The cursor/seed vertex may live on a non-active
   * object (cross-mesh Expand), so its falloff must be read from its OWN object's state. Single ⇒
   * `vertex` is always `{0, v}` ⇒ this reads `active_object_state(...).vert_falloff[v]`,
   * identical to before. */
  if (vertex.object_index == -1) {
    /* This means that the cursor is not over the mesh, so a valid active falloff can't be
     * determined. In this situations, don't evaluate enabled states and default all vertices in
     * connected components to enabled. */
    expand_cache.active_falloff = expand_cache.max_vert_falloff;
    expand_cache.all_enabled = true;
  }
  else {
    expand_cache.active_falloff =
        expand_cache.object_states[vertex.object_index].vert_falloff[vertex.vert];
    expand_cache.all_enabled = false;
  }

  if (expand_multi_object_active(expand_cache) &&
      falloff_type_supports_multi(expand_cache.falloff_type))
  {
    /* Unified per-object apply covering all three targets (Mesh-only, matching
     * #expand_multi_object_active's gate). FaceSets restores each object's own original state
     * right before applying to it (mirrors the single-object restore-then-apply order below). */
    for (const int i : expand_cache.object_states.index_range()) {
      Object &object = *expand_cache.object_states[i].object;
      const ObjectState &object_state = expand_cache.object_states[i];
      switch (expand_cache.target) {
        case TargetType::Mask: {
          SculptSession &object_ss = *object.runtime->sculpt_session;
          const IndexMask &object_node_mask = object_state.node_mask;
          const BitVector<> enabled_verts = enabled_state_to_bitmap(
              depsgraph, object, expand_cache);
          const bke::pbvh::Tree &object_pbvh_type_check = *bke::object::pbvh_get(object);
          switch (object_pbvh_type_check.type()) {
            case bke::pbvh::Type::Mesh: {
              const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
              mask::update_mask_mesh(
                  depsgraph,
                  object,
                  object_node_mask,
                  [&](const MutableSpan<float> mask, const Span<int> verts) {
                    calc_new_mask_mesh(object_ss,
                                       expand_cache,
                                       object_state,
                                       positions,
                                       enabled_verts,
                                       verts,
                                       mask,
                                       i);
                  });
              break;
            }
            case bke::pbvh::Type::Grids: {
              SubdivCCG &object_subdiv_ccg = *object_ss.subdiv_ccg;
              bke::pbvh::Tree &object_pbvh = *bke::object::pbvh_get(object);
              Array<bool> node_changed(object_node_mask.min_array_size(), false);
              MutableSpan<bke::pbvh::GridsNode> nodes = object_pbvh.nodes<bke::pbvh::GridsNode>();
              object_node_mask.foreach_index(
                  [&](const int node_i) {
                    node_changed[node_i] = update_mask_grids(object_ss,
                                                             expand_cache,
                                                             object_state,
                                                             i,
                                                             enabled_verts,
                                                             nodes[node_i],
                                                             object_subdiv_ccg);
                  },
                  exec_mode::grain_size(1));
              IndexMaskMemory memory;
              object_pbvh.tag_masks_changed(IndexMask::from_bools(node_changed, memory));
              BKE_subdiv_ccg_average_grids(object_subdiv_ccg);
              /* `update_mask_grids` writes ONLY to the evaluated `subdiv_ccg.masks` span. That change
               * is flushed back to the persistent base-cage `GridPaintMask` layer only when the
               * object's `subdiv_ccg->dirty.coords` is set (there is no separate mask flag -- see the
               * `MultiresModifiedFlags` enum, which only has COORDS/HIDDEN; the reshape write-back
               * path `multires_reshape_assign_final_coords_from_ccg` copies masks as a side effect of
               * the coords reshape). `flush_update_step(C, ...)` below marks dirty.coords for the
               * ACTIVE object only, so without this per-object mark a SECONDARY Grids object's mask
               * survives in evaluated data while dragging (preview is correct, the viewport reads
               * eval) but is discarded on the depsgraph re-eval triggered by Confirm's
               * `flush_update_done` -- appearing as an empty/reverted mask on the secondary object,
               * with the undo step (snapshot taken from eval at invoke) showing the same wrong state.
               * This is the identical call the Mesh mask tools make (`paint_mask.cc`: every Grids
               * mask op) and that `flush_update_step` makes for the active object. `multires_mark_as_
               * modified` takes a non-const `Depsgraph *`, so resolve it directly from the context
               * here instead of the function's const `depsgraph` binding. */
              multires_mark_as_modified(
                  CTX_data_depsgraph_pointer(C), &object, MULTIRES_COORDS_MODIFIED);
              break;
            }
            case bke::pbvh::Type::BMesh: {
              /* Unreachable: the multi-object gate (#all_topology_supported) never admits a BMesh
               * object into a multi-object group. */
              BLI_assert_unreachable();
              break;
            }
          }
          break;
        }
        case TargetType::FaceSets: {
          face_sets_restore(object, expand_cache);
          face_sets_update(object, expand_cache);
          break;
        }
        case TargetType::Colors: {
          if (bke::object::pbvh_get(object)->type() != bke::pbvh::Type::Mesh) {
            /* Colors is Mesh-only (see #original_state_store_for_object's matching guard) -- skip
             * this object rather than reading its color data through arrays sized to the wrong
             * (base-mesh) vertex count. Reported once per object per apply call is too noisy for a
             * modal drag operator; the invoke-time report (Step 3) covers the user-facing warning
             * instead. */
            break;
          }
          Mesh &mesh = *id_cast<Mesh *>(object.data);
          const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, object);
          const OffsetIndices<int> faces = mesh.faces();
          const Span<int> corner_verts = mesh.corner_verts();
          const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
          const bke::AttributeAccessor attributes = mesh.attributes();
          const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert",
                                                                bke::AttrDomain::Point);
          const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask",
                                                             bke::AttrDomain::Point);
          bke::GSpanAttributeWriter color_attribute = color::active_color_attribute_for_write(
              mesh);
          /* Hoisted once per object instead of once per node (spec §9 perf). */
          const BitVector<> enabled_verts = enabled_state_to_bitmap(
              depsgraph, object, expand_cache);

          bke::pbvh::Tree &object_pbvh = *bke::object::pbvh_get(object);
          const IndexMask &object_node_mask = object_state.node_mask;
          Array<bool> node_changed(object_node_mask.min_array_size(), false);

          MutableSpan<bke::pbvh::MeshNode> nodes = object_pbvh.nodes<bke::pbvh::MeshNode>();
          object_node_mask.foreach_index(
              [&](const int node_i) {
                node_changed[node_i] = colors_update_task(depsgraph,
                                                          object,
                                                          expand_cache,
                                                          vert_positions,
                                                          faces,
                                                          corner_verts,
                                                          vert_to_face_map,
                                                          hide_vert,
                                                          mask,
                                                          &nodes[node_i],
                                                          color_attribute,
                                                          enabled_verts);
              },
              exec_mode::grain_size(1));

          IndexMaskMemory memory;
          object_pbvh.tag_attribute_changed(IndexMask::from_bools(node_changed, memory),
                                            mesh.active_color_attribute);
          color_attribute.finish();
          break;
        }
      }
    }
    switch (expand_cache.target) {
      case TargetType::Mask:
        flush_update_step(C, UpdateType::Mask);
        break;
      case TargetType::FaceSets:
        flush_update_step(C, UpdateType::FaceSet);
        break;
      case TargetType::Colors:
        flush_update_step(C, UpdateType::Color);
        break;
    }
    return;
  }

  if (expand_cache.target == TargetType::FaceSets) {
    /* Face sets needs to be restored their initial state on each iteration as the overwrite
     * existing data. */
    face_sets_restore(ob, expand_cache);
  }

  const IndexMask &node_mask = state.node_mask;

  const BitVector<> enabled_verts = enabled_state_to_bitmap(depsgraph, ob, expand_cache);

  switch (expand_cache.target) {
    case TargetType::Mask: {
      switch (pbvh.type()) {
        case bke::pbvh::Type::Mesh: {
          const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
          mask::update_mask_mesh(
              depsgraph, ob, node_mask, [&](const MutableSpan<float> mask, const Span<int> verts) {
                calc_new_mask_mesh(ss, expand_cache, state, positions, enabled_verts, verts, mask, 0);
              });
          break;
        }
        case bke::pbvh::Type::Grids: {
          Array<bool> node_changed(node_mask.min_array_size(), false);

          MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
          node_mask.foreach_index(
              [&](const int i) {
                node_changed[i] = update_mask_grids(
                    ss, expand_cache, state, 0, enabled_verts, nodes[i], *ss.subdiv_ccg);
              },
              exec_mode::grain_size(1));

          IndexMaskMemory memory;
          pbvh.tag_masks_changed(IndexMask::from_bools(node_changed, memory));

          BKE_subdiv_ccg_average_grids(*ss.subdiv_ccg);
          break;
        }
        case bke::pbvh::Type::BMesh: {
          const int mask_offset = CustomData_get_offset_named(
              &ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
          MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();

          Array<Vector<float>> old_masks(node_mask.min_array_size());
          node_mask.foreach_index(
              [&](const int i) {
                const Set<BMVert *, 0> &other = BKE_pbvh_bmesh_node_other_verts(&nodes[i]);
                old_masks[i].resize(other.size());
                int j = 0;
                for (BMVert *vert : other) {
                  old_masks[i][j] = BM_ELEM_CD_GET_FLOAT(vert, mask_offset);
                  j++;
                }
              },
              exec_mode::grain_size(1));
          Array<bool> node_changed(node_mask.min_array_size(), false);
          node_mask.foreach_index(
              [&](const int i) {
                node_changed[i] = update_mask_bmesh(
                    ss, enabled_verts, mask_offset, old_masks[i].as_span(), &nodes[i]);
              },
              exec_mode::grain_size(1));

          IndexMaskMemory memory;
          pbvh.tag_masks_changed(IndexMask::from_bools(node_changed, memory));
          break;
        }
      }
      flush_update_step(C, UpdateType::Mask);
      break;
    }
    case TargetType::FaceSets:
      face_sets_update(ob, expand_cache);
      flush_update_step(C, UpdateType::FaceSet);
      break;
    case TargetType::Colors: {
      Mesh &mesh = *id_cast<Mesh *>(ob.data);
      const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int> corner_verts = mesh.corner_verts();
      const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
      const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
      bke::GSpanAttributeWriter color_attribute = color::active_color_attribute_for_write(mesh);

      Array<bool> node_changed(node_mask.min_array_size(), false);

      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            node_changed[i] = colors_update_task(depsgraph,
                                                 ob,
                                                 expand_cache,
                                                 vert_positions,
                                                 faces,
                                                 corner_verts,
                                                 vert_to_face_map,
                                                 hide_vert,
                                                 mask,
                                                 &nodes[i],
                                                 color_attribute,
                                                 enabled_verts);
          },
          exec_mode::grain_size(1));

      IndexMaskMemory memory;
      pbvh.tag_attribute_changed(IndexMask::from_bools(node_changed, memory),
                                 mesh.active_color_attribute);

      color_attribute.finish();
      flush_update_step(C, UpdateType::Color);
      break;
    }
  }
}

/**
 * Updates the #SculptSession cursor data and resolves the seed vertex under the cursor across
 * ALL sculpt-mode objects. A hit on any of `expand_cache.object_states` is now valid (this is the
 * cross-mesh Expand feature — no longer the previous single-object rejection). Returns
 * `{-1, -1}` when the cursor is over no mesh, or over a mesh that is not in sculpt mode.
 * `r_hit_ob` receives the hit object, or nullptr on a miss.
 */
static MultiVertRef target_vert_update_and_get(bContext *C,
                                               Cache &expand_cache,
                                               const float mval[2],
                                               Object **r_hit_ob)
{
  CursorGeometryInfo cgi;
  Object *hit_ob = nullptr;
  const bool hit = cursor_geometry_info_update(C, &cgi, mval, false, &hit_ob);
  *r_hit_ob = hit ? hit_ob : nullptr;
  if (!hit || hit_ob == nullptr) {
    return {-1, -1};
  }
  int object_index = -1;
  for (const int i : expand_cache.object_states.index_range()) {
    if (expand_cache.object_states[i].object == hit_ob) {
      object_index = i;
      break;
    }
  }
  if (object_index == -1) {
    /* Hit a mesh that is not among the sculpt-mode objects: report a clean miss so callers never
     * see a non-null hit object paired with an invalid {-1, -1} reference. */
    *r_hit_ob = nullptr;
    return {-1, -1};
  }
  SculptSession &ss = *hit_ob->runtime->sculpt_session;
  const int vert = ss.active_vert_index();
  if (vert == -1) {
    /* No valid active vertex on the hit object's session: treat the same as a miss (previously
     * crashed indexing with a negative index in #find_symm_verts_mesh). */
    *r_hit_ob = nullptr;
    return {-1, -1};
  }
  return {object_index, vert};
}

/**
 * Moves the sculpt pivot to the average point of the boundary enabled vertices of the current
 * expand state. Take symmetry and active components into account.
 */
static void reposition_pivot(bContext *C, Object &ob, Cache &expand_cache)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const char symm = mesh_symmetry_xyz_get(ob);
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);

  if (expand_multi_object_active(expand_cache)) {
    /* Multi-object: average the enabled boundary of EVERY object in the active object's local
     * frame (via each object's precomputed world positions), so meshes with different transforms
     * combine correctly and the result lands in `ss.pivot_pos`'s space (active-object local),
     * matching the single-object path below. */
    const bool initial_invert_state = expand_cache.invert;
    expand_cache.invert = false;

    const float use_mesh_boundary = expand_cache.falloff_type != FalloffType::BoundaryTopology;
    const float4x4 world_to_active = ob.world_to_object();

    const MultiVertRef seed = expand_cache.seed;
    const float3 world_seed = (seed.object_index >= 0 &&
                               seed.object_index < expand_cache.world_positions.size()) ?
                                  expand_cache.world_positions[seed.object_index][seed.vert] :
                                  expand_cache.world_positions[0][expand_cache.initial_active_vert];
    const float3 expand_init_co = math::transform_point(world_to_active, world_seed);

    double3 average(0);
    int total = 0;
    for (const int i : expand_cache.object_states.index_range()) {
      Object &object = *expand_cache.object_states[i].object;
      const SculptSession &object_ss = *object.runtime->sculpt_session;
      const BitVector<> enabled_verts = enabled_state_to_bitmap(depsgraph, object, expand_cache);
      IndexMaskMemory memory;
      const IndexMask boundary_verts = boundary_from_enabled(
          object, enabled_verts, use_mesh_boundary, memory);
      const Span<float3> world_positions = expand_cache.world_positions[i];
      boundary_verts.foreach_index([&](const int vert) {
        if (!is_vert_in_active_component(object_ss, expand_cache, vert, i)) {
          return;
        }
        const float3 position = math::transform_point(world_to_active, world_positions[vert]);
        if (!check_vertex_pivot_symmetry(position, expand_init_co, symm)) {
          return;
        }
        average += double3(position);
        total++;
      });
    }

    expand_cache.invert = initial_invert_state;

    if (total > 0) {
      ss.pivot_pos = float3(average / total);
    }
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob.data);
    return;
  }

  const bool initial_invert_state = expand_cache.invert;
  expand_cache.invert = false;
  const BitVector<> enabled_verts = enabled_state_to_bitmap(depsgraph, ob, expand_cache);

  /* For boundary topology, position the pivot using only the boundary of the enabled vertices,
   * without taking mesh boundary into account. This allows to create deformations like bending the
   * mesh from the boundary of the mask that was just created. */
  const float use_mesh_boundary = expand_cache.falloff_type != FalloffType::BoundaryTopology;

  IndexMaskMemory memory;
  const IndexMask boundary_verts = boundary_from_enabled(
      ob, enabled_verts, use_mesh_boundary, memory);

  /* Ignore invert state, as this is the expected behavior in most cases and mask are created in
   * inverted state by default. */
  expand_cache.invert = initial_invert_state;

  double3 average(0);
  int total = 0;
  /* `ob` here is always the active object (index 0): the pivot is repositioned relative to the
   * active mesh's own boundary, never a secondary object's. */
  switch (bke::object::pbvh_get(ob)->type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
      const float3 expand_init_co = positions[expand_cache.initial_active_vert];
      boundary_verts.foreach_index([&](const int vert) {
        if (!is_vert_in_active_component(ss, expand_cache, vert, 0)) {
          return;
        }
        const float3 &position = positions[vert];
        if (!check_vertex_pivot_symmetry(position, expand_init_co, symm)) {
          return;
        }
        average += double3(position);
        total++;
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const Span<float3> positions = subdiv_ccg.positions;
      const float3 expand_init_co = positions[expand_cache.initial_active_vert];
      boundary_verts.foreach_index([&](const int vert) {
        if (!is_vert_in_active_component(ss, expand_cache, vert, 0)) {
          return;
        }
        const float3 position = positions[vert];
        if (!check_vertex_pivot_symmetry(position, expand_init_co, symm)) {
          return;
        }
        average += double3(position);
        total++;
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ss.bm;
      const float3 expand_init_co = BM_vert_at_index(&bm, expand_cache.initial_active_vert)->co;
      boundary_verts.foreach_index([&](const int vert) {
        if (!is_vert_in_active_component(ss, expand_cache, vert, 0)) {
          return;
        }
        const float3 position = BM_vert_at_index(&bm, vert)->co;
        if (!check_vertex_pivot_symmetry(position, expand_init_co, symm)) {
          return;
        }
        average += double3(position);
        total++;
      });
      break;
    }
  }

  if (total > 0) {
    ss.pivot_pos = float3(average / total);
  }

  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob.data);
}

static void finish(bContext *C)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  Cache &expand_cache = *ss.expand_cache;

  if (expand_multi_object_active(expand_cache)) {
    undo::push_end_all_ex(false, true);
    for (const ObjectState &state : expand_cache.object_states) {
      Object &object = *state.object;
      switch (expand_cache.target) {
        case TargetType::Mask:
          flush_update_done(C, object, UpdateType::Mask);
          break;
        case TargetType::FaceSets:
          flush_update_done(C, object, UpdateType::FaceSet);
          break;
        case TargetType::Colors:
          flush_update_done(C, object, UpdateType::Color);
          break;
      }
    }
    expand_cache_free(ss);
    ED_workspace_status_text(C, nullptr);
    return;
  }

  undo::push_end(ob);

  switch (ss.expand_cache->target) {
    case TargetType::Mask:
      flush_update_done(C, ob, UpdateType::Mask);
      break;
    case TargetType::FaceSets:
      flush_update_done(C, ob, UpdateType::FaceSet);
      break;
    case TargetType::Colors:
      flush_update_done(C, ob, UpdateType::Color);
      break;
  }

  expand_cache_free(ss);
  ED_workspace_status_text(C, nullptr);
}

/**
 * Finds and stores in the #Cache the sculpt connected component index for each symmetry
 * pass needed for expand.
 */
static void find_active_connected_components_from_vert(const Depsgraph &depsgraph,
                                                       Object &ob,
                                                       Cache &expand_cache,
                                                       const int initial_vertex)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  for (int i = 0; i < EXPAND_SYMM_AREAS; i++) {
    expand_cache.active_connected_islands[i] = {0, EXPAND_ACTIVE_COMPONENT_NONE};
  }

  if (expand_multi_object_active(expand_cache)) {
    /* Cross-mesh: the active component spans every island reachable from the seed's island through
     * mesh edges and the proximity bridge. BFS over the graph whose nodes are (object, island) pairs
     * and whose cross-object edges are the bridge stitches; store each object's reachable island ids
     * in its ObjectState (spec §8, cross-mesh generalization). */
    Span<ObjectState> states = expand_cache.object_states;
    for (ObjectState &state : expand_cache.object_states) {
      state.active_islands.clear();
    }

    /* Resolve each bridge stitch to an edge between two (object, island) nodes, once. */
    struct IslandEdge {
      int object_a;
      int island_a;
      int object_b;
      int island_b;
    };
    Vector<IslandEdge> island_edges;
    island_edges.reserve(expand_cache.bridge.edges.size());
    for (const BridgeEdge &edge : expand_cache.bridge.edges) {
      const SculptSession &ss_a = *states[edge.a.object_index].object->runtime->sculpt_session;
      const SculptSession &ss_b = *states[edge.b.object_index].object->runtime->sculpt_session;
      island_edges.append({edge.a.object_index,
                           islands::vert_id_get(ss_a, edge.a.vert),
                           edge.b.object_index,
                           islands::vert_id_get(ss_b, edge.b.vert)});
    }

    /* Seed the BFS with the symmetry-resolved seeds' islands. `frontier` reuses #MultiVertRef as an
     * (object_index, island id) pair, matching how `active_connected_islands` stores islands. */
    const Vector<MultiVertRef> seeds = find_symm_multi_verts(
        depsgraph, states, expand_cache.world_positions, expand_cache.seed, FLT_MAX);
    Vector<MultiVertRef> frontier;
    for (const MultiVertRef &seed : seeds) {
      const SculptSession &seed_ss = *states[seed.object_index].object->runtime->sculpt_session;
      const int island = islands::vert_id_get(seed_ss, seed.vert);
      if (expand_cache.object_states[seed.object_index].active_islands.add(island)) {
        frontier.append({seed.object_index, island});
      }
    }

    /* Flood across bridge edges: reaching either endpoint's island makes the other reachable. */
    while (!frontier.is_empty()) {
      const MultiVertRef node = frontier.pop_last();
      for (const IslandEdge &edge : island_edges) {
        int next_object = -1;
        int next_island = -1;
        if (edge.object_a == node.object_index && edge.island_a == node.vert) {
          next_object = edge.object_b;
          next_island = edge.island_b;
        }
        else if (edge.object_b == node.object_index && edge.island_b == node.vert) {
          next_object = edge.object_a;
          next_island = edge.island_a;
        }
        else {
          continue;
        }
        if (expand_cache.object_states[next_object].active_islands.add(next_island)) {
          frontier.append({next_object, next_island});
        }
      }
    }
    return;
  }

  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(ob);

  const Vector<int> symm_verts = find_symm_verts(depsgraph, ob, initial_vertex);

  int valid_index = 0;
  for (int symm_it = 0; symm_it <= symm; symm_it++) {
    if (!is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    /* Single-object path (reached only when #expand_multi_object_active is false): the connected
     * island lives on the active object (index 0). */
    expand_cache.active_connected_islands[symm_it] = {
        0, islands::vert_id_get(ss, symm_verts[valid_index])};
    valid_index++;
  }
}

/**
 * Stores the seed, active vertex, face set and mouse coordinates in the #Cache based on the
 * current cursor position. The cursor may now hit ANY sculpt-mode object (see
 * #target_vert_update_and_get); the resulting seed is stored in `expand_cache.seed` and mirrored
 * into `expand_cache.initial_active_vert` only when it lands on the active object.
 */
static bool set_initial_components_for_mouse(bContext *C,
                                             Object &ob,
                                             Cache &expand_cache,
                                             const float mval[2])
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);

  Object *hit_ob = nullptr;
  MultiVertRef seed = target_vert_update_and_get(C, expand_cache, mval, &hit_ob);
  if (seed.object_index == -1) {
    /* Cursor not over the mesh, for creating valid initial falloffs, fall back to the last active
     * vertex in the sculpt session. This is a seed on the ACTIVE object (index 0), matching
     * today's single-object fallback.
     * It still may be the case that there is no last active vert in rare circumstances for
     * everyday usage.
     * (i.e. if the cursor has never been over the mesh at all. A solution to both this problem
     * and needing to store this data is to figure out which is the nearest vertex to the current
     * cursor position */
    const int last_active_vert_index = ss.last_active_vert_index();
    if (last_active_vert_index == -1) {
      return false;
    }
    seed = {0, last_active_vert_index};
    hit_ob = nullptr; /* Fell back to the active object; there is no cross-object hit. */
  }
  expand_cache.seed = seed;
  copy_v2_v2(ss.expand_cache->initial_mouse, mval);

  /* CRASH-SAFETY + single-object exactness: `initial_active_vert` is read by many single-object
   * paths that index the ACTIVE mesh with `ob` (e.g. #reposition_pivot, falloff calculations). It
   * must therefore ALWAYS be a valid index into the active object. It equals the real seed vert
   * only when the seed is on the active object; when the seed is on another object, mirror a safe
   * active-object vert instead (the true cross-object seed lives in `expand_cache.seed`, which is
   * what the multi-object Sphere path reads). Non-Sphere falloffs seeded from a non-active object
   * are not yet fully correct until Stage 3 — that is a known visual gap, NOT a crash. */
  if (seed.object_index == 0) {
    expand_cache.initial_active_vert = seed.vert;
  }
  else {
    const int active_fallback = ss.last_active_vert_index();
    expand_cache.initial_active_vert = active_fallback == -1 ? 0 : active_fallback;
  }

  /* Read the active face set from the HIT object, falling back to the active object when the
   * cursor missed (matches the seed fallback above). */
  Object &face_set_ob = hit_ob ? *hit_ob : ob;
  expand_cache.initial_active_face_set = face_set::active_face_set_get(face_set_ob);

  if (expand_cache.next_face_set == face_set_none_id) {
    /* Only set the next face set once, otherwise this ID will constantly update to a new one each
     * time this function is called for using a new initial vertex from a different cursor
     * position. */
    if (expand_cache.modify_active_face_set) {
      expand_cache.next_face_set = face_set::active_face_set_get(face_set_ob);
    }
    else {
      if (expand_multi_object_active(expand_cache)) {
        /* One shared id across all objects for the whole stroke (Round 13 parity). */
        Vector<Object *> objects;
        for (const ObjectState &state : expand_cache.object_states) {
          objects.append(state.object);
        }
        expand_cache.next_face_set = face_set::find_shared_next_available_id(objects.as_span());
      }
      else {
        expand_cache.next_face_set = face_set::find_next_available_id(ob);
      }
    }
  }

  /* The new mouse position can be over a different connected component, so this needs to be
   * updated. Passes the ACTIVE object + the guaranteed-valid active-object `initial_active_vert`
   * (crash-safe); when multi-object Expand is active, the callee ignores both and re-derives
   * cross-object seeds from `expand_cache.seed` instead. */
  find_active_connected_components_from_vert(
      depsgraph, ob, expand_cache, expand_cache.initial_active_vert);
  return true;
}

/**
 * Displaces the initial mouse coordinates using the new mouse position to get a new active vertex.
 * After that, initializes a new falloff of the same type with the new active vertex.
 */
static void move_propagation_origin(bContext *C,
                                    Object &ob,
                                    const wmEvent *event,
                                    Cache &expand_cache)
{
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  float move_disp[2];
  sub_v2_v2v2(move_disp, mval_fl, expand_cache.initial_mouse_move);

  float new_mval[2];
  add_v2_v2v2(new_mval, move_disp, expand_cache.original_mouse_move);

  /* NOTE: no multi-object specific code is needed here. `set_initial_components_for_mouse`
   * updates `expand_cache.seed` in place, which may now point to a different object; the multi
   * branch of `calc_falloff_from_vert_and_symmetry` re-propagates from that updated seed across
   * the bridge, and islands follow via `find_active_connected_components_from_vert`. The
   * `initial_active_vert` argument below is only consumed by the single-object falloff path. */
  set_initial_components_for_mouse(C, ob, expand_cache, new_mval);
  calc_falloff_from_vert_and_symmetry(depsgraph,
                                      expand_cache,
                                      ob,
                                      expand_cache.initial_active_vert,
                                      expand_cache.move_preview_falloff_type);
}

/**
 * Ensures that the #SculptSession contains the required data needed for Expand.
 */
static void ensure_sculptsession_data(Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  islands::ensure_cache(ob);
  vert_random_access_ensure(ob);
  boundary::ensure_boundary_info(ob);
  if (!ss.tex_pool) {
    ss.tex_pool = BKE_image_pool_new();
  }
}

/**
 * Returns the active face set ID from the enabled face or grid in the #SculptSession.
 */
static int active_face_set_id_get(Object &object, Cache &expand_cache)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const ObjectState &state = object_state_get(expand_cache, object);
  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh:
      if (!ss.active_face_index) {
        return face_set_none_id;
      }
      return state.original_face_sets[*ss.active_face_index];
    case bke::pbvh::Type::Grids: {
      if (!ss.active_grid_index) {
        return face_set_none_id;
      }
      const int face_index = BKE_subdiv_ccg_grid_to_face_index(*ss.subdiv_ccg,
                                                               *ss.active_grid_index);
      return state.original_face_sets[face_index];
    }
    case bke::pbvh::Type::BMesh: {
      /* Dyntopo does not support face set functionality. */
      BLI_assert(false);
    }
  }
  return face_set_none_id;
}

static void sculpt_expand_status(bContext *C, wmOperator *op, Cache *expand_cache)
{
  WorkspaceStatus status(C);

  status.opmodal(IFACE_("Confirm"), op->type, SCULPT_EXPAND_MODAL_CONFIRM);
  status.opmodal(IFACE_("Cancel"), op->type, SCULPT_EXPAND_MODAL_CANCEL);
  status.opmodal(IFACE_("Invert"), op->type, SCULPT_EXPAND_MODAL_INVERT, expand_cache->invert);
  status.opmodal(IFACE_("Snap"), op->type, SCULPT_EXPAND_MODAL_SNAP_TOGGLE, expand_cache->snap);
  status.opmodal(IFACE_("Snap to Origin Object"),
                 op->type,
                 SCULPT_EXPAND_MODAL_SNAP_SEED_OBJECT_ONLY_TOGGLE,
                 expand_cache->snap_seed_object_only);
  status.opmodal(IFACE_("Move"), op->type, SCULPT_EXPAND_MODAL_MOVE_TOGGLE, expand_cache->move);
  status.opmodal(
      IFACE_("Preserve"), op->type, SCULPT_EXPAND_MODAL_PRESERVE_TOGGLE, expand_cache->preserve);

  if (expand_cache->target != TargetType::FaceSets) {
    status.opmodal(IFACE_("Falloff Gradient"),
                   op->type,
                   SCULPT_EXPAND_MODAL_GRADIENT_TOGGLE,
                   expand_cache->falloff_gradient);
    status.opmodal(IFACE_("Brush Gradient"),
                   op->type,
                   SCULPT_EXPAND_MODAL_BRUSH_GRADIENT_TOGGLE,
                   expand_cache->brush_gradient);
  }

  if (ELEM(expand_cache->falloff_type,
           FalloffType::Geodesic,
           FalloffType::Topology,
           FalloffType::TopologyNormals,
           FalloffType::Sphere))
  {
    status.item(IFACE_("Falloff:"), 0);
    status.opmodal(IFACE_("Geodesic"),
                   op->type,
                   SCULPT_EXPAND_MODAL_FALLOFF_GEODESIC,
                   expand_cache->falloff_type == FalloffType::Geodesic);
    status.opmodal(IFACE_("Topology"),
                   op->type,
                   SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY,
                   expand_cache->falloff_type == FalloffType::Topology);
    status.opmodal(IFACE_("Diagonals"),
                   op->type,
                   SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY_DIAGONALS,
                   expand_cache->falloff_type == FalloffType::TopologyNormals);
    status.opmodal(IFACE_("Spherical"),
                   op->type,
                   SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL,
                   expand_cache->falloff_type == FalloffType::Sphere);
  }

  status.opmodal({}, op->type, SCULPT_EXPAND_MODAL_LOOP_COUNT_INCREASE);
  status.item("/", 0);
  status.separator(-1.2f);
  status.opmodal(IFACE_("Loop Count"), op->type, SCULPT_EXPAND_MODAL_LOOP_COUNT_DECREASE);

  status.opmodal(IFACE_("Geodesic Step"), op->type, SCULPT_EXPAND_MODAL_RECURSION_STEP_GEODESIC);
  status.opmodal(IFACE_("Topology Step"), op->type, SCULPT_EXPAND_MODAL_RECURSION_STEP_TOPOLOGY);

  const MTex *mask_tex = BKE_brush_mask_texture_get(expand_cache->brush, OB_MODE_SCULPT);
  if (mask_tex->tex) {
    status.opmodal({}, op->type, SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_INCREASE);
    status.opmodal(
        IFACE_("Texture Distortion"), op->type, SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_DECREASE);
  }
}

static wmOperatorStatus sculpt_expand_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;

  /* Skips INBETWEEN_MOUSEMOVE events and other events that may cause unnecessary updates. */
  if (!ELEM(event->type, MOUSEMOVE, EVT_MODAL_MAP)) {
    return OPERATOR_RUNNING_MODAL;
  }

  /* Update SculptSession data. */
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);
  ensure_sculptsession_data(ob);

  /* Update and get the active vertex (and face) from the cursor. Any sculpt-mode object may now
   * be hit (see #target_vert_update_and_get); `update_for_vert` reads the falloff from the hit
   * object's own state via the #MultiVertRef. */
  const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  Object *hit_ob = nullptr;
  const MultiVertRef target_expand_vertex = target_vert_update_and_get(
      C, *ss.expand_cache, mval_fl, &hit_ob);

  /* Handle the modal keymap state changes. */
  Cache &expand_cache = *ss.expand_cache;
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case SCULPT_EXPAND_MODAL_CANCEL: {
        sculpt_expand_cancel(C, op);
        return OPERATOR_FINISHED;
      }
      case SCULPT_EXPAND_MODAL_INVERT: {
        expand_cache.invert = !expand_cache.invert;
        break;
      }
      case SCULPT_EXPAND_MODAL_PRESERVE_TOGGLE: {
        expand_cache.preserve = !expand_cache.preserve;
        break;
      }
      case SCULPT_EXPAND_MODAL_GRADIENT_TOGGLE: {
        expand_cache.falloff_gradient = !expand_cache.falloff_gradient;
        break;
      }
      case SCULPT_EXPAND_MODAL_BRUSH_GRADIENT_TOGGLE: {
        expand_cache.brush_gradient = !expand_cache.brush_gradient;
        if (expand_cache.brush_gradient) {
          expand_cache.falloff_gradient = true;
        }
        break;
      }
      case SCULPT_EXPAND_MODAL_SNAP_TOGGLE: {
        if (expand_cache.snap) {
          expand_cache.snap = false;
          if (expand_cache.snap_enabled_face_sets) {
            expand_cache.snap_enabled_face_sets.reset();
          }
        }
        else {
          expand_cache.snap = true;
          expand_cache.snap_enabled_face_sets = std::make_unique<Set<int>>();
          snap_init_from_enabled(*depsgraph, ob, expand_cache);
        }
        break;
      }
      case SCULPT_EXPAND_MODAL_SNAP_SEED_OBJECT_ONLY_TOGGLE: {
        expand_cache.snap_seed_object_only = !expand_cache.snap_seed_object_only;
        break;
      }
      case SCULPT_EXPAND_MODAL_MOVE_TOGGLE: {
        if (expand_cache.move) {
          expand_cache.move = false;
          calc_falloff_from_vert_and_symmetry(*depsgraph,
                                              expand_cache,
                                              ob,
                                              expand_cache.initial_active_vert,
                                              expand_cache.move_original_falloff_type);
          break;
        }
        expand_cache.move = true;
        expand_cache.move_original_falloff_type = expand_cache.falloff_type;
        copy_v2_v2(expand_cache.initial_mouse_move, mval_fl);
        copy_v2_v2(expand_cache.original_mouse_move, expand_cache.initial_mouse);
        if (expand_cache.falloff_type == FalloffType::Geodesic &&
            vertex_count_get(ob) > expand_cache.max_geodesic_move_preview)
        {
          /* Set to spherical falloff for preview in high poly meshes as it is the fastest one.
           * In most cases it should match closely the preview from geodesic. */
          expand_cache.move_preview_falloff_type = FalloffType::Sphere;
        }
        else {
          expand_cache.move_preview_falloff_type = expand_cache.falloff_type;
        }
        break;
      }
      case SCULPT_EXPAND_MODAL_RECURSION_STEP_GEODESIC: {
        resursion_step_add(*depsgraph, ob, expand_cache, RecursionType::Geodesic);
        break;
      }
      case SCULPT_EXPAND_MODAL_RECURSION_STEP_TOPOLOGY: {
        resursion_step_add(*depsgraph, ob, expand_cache, RecursionType::Topology);
        break;
      }
      case SCULPT_EXPAND_MODAL_CONFIRM: {
        update_for_vert(C, ob, target_expand_vertex);

        if (expand_cache.reposition_pivot) {
          reposition_pivot(C, ob, expand_cache);
        }

        finish(C);
        return OPERATOR_FINISHED;
      }
      case SCULPT_EXPAND_MODAL_FALLOFF_GEODESIC: {
        check_topology_islands(ob, FalloffType::Geodesic);

        calc_falloff_from_vert_and_symmetry(
            *depsgraph, expand_cache, ob, expand_cache.initial_active_vert, FalloffType::Geodesic);
        break;
      }
      case SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY: {
        check_topology_islands(ob, FalloffType::Topology);

        calc_falloff_from_vert_and_symmetry(
            *depsgraph, expand_cache, ob, expand_cache.initial_active_vert, FalloffType::Topology);
        break;
      }
      case SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY_DIAGONALS: {
        check_topology_islands(ob, FalloffType::TopologyNormals);

        calc_falloff_from_vert_and_symmetry(*depsgraph,
                                            expand_cache,
                                            ob,
                                            expand_cache.initial_active_vert,
                                            FalloffType::TopologyNormals);
        break;
      }
      case SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL: {
        expand_cache.check_islands = false;
        calc_falloff_from_vert_and_symmetry(
            *depsgraph, expand_cache, ob, expand_cache.initial_active_vert, FalloffType::Sphere);
        break;
      }
      case SCULPT_EXPAND_MODAL_LOOP_COUNT_INCREASE: {
        expand_cache.loop_count += 1;
        break;
      }
      case SCULPT_EXPAND_MODAL_LOOP_COUNT_DECREASE: {
        expand_cache.loop_count -= 1;
        expand_cache.loop_count = max_ii(expand_cache.loop_count, 1);
        break;
      }
      case SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_INCREASE: {
        if (expand_cache.texture_distortion_strength == 0.0f) {
          const MTex *mask_tex = BKE_brush_mask_texture_get(expand_cache.brush, OB_MODE_SCULPT);
          if (mask_tex->tex == nullptr) {
            BKE_report(op->reports,
                       RPT_WARNING,
                       "Active brush does not contain any texture to distort the expand boundary");
            break;
          }
          if (mask_tex->brush_map_mode != MTEX_MAP_MODE_3D) {
            BKE_report(op->reports,
                       RPT_WARNING,
                       "Texture mapping not set to 3D, results may be unpredictable");
          }
        }
        expand_cache.texture_distortion_strength += SCULPT_EXPAND_TEXTURE_DISTORTION_STEP;
        break;
      }
      case SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_DECREASE: {
        expand_cache.texture_distortion_strength -= SCULPT_EXPAND_TEXTURE_DISTORTION_STEP;
        expand_cache.texture_distortion_strength = max_ff(expand_cache.texture_distortion_strength,
                                                          0.0f);
        break;
      }
    }
  }

  /* Handle expand origin movement if enabled. */
  if (expand_cache.move) {
    move_propagation_origin(C, ob, event, expand_cache);
  }

  /* Add new face set IDs to the snapping set if enabled. Read from the HIT object, falling back
   * to the active object when the cursor missed (matches the seed fallback above). */
  if (expand_cache.snap) {
    Object &snap_ob = hit_ob ? *hit_ob : ob;
    const int active_face_set_id = active_face_set_id_get(snap_ob, expand_cache);
    /* The key may exist, in that case this does nothing. */
    expand_cache.snap_enabled_face_sets->add(active_face_set_id);
  }

  /* Update the sculpt data with the current state of the #Cache. */
  update_for_vert(C, ob, target_expand_vertex);

  sculpt_expand_status(C, op, &expand_cache);

  return OPERATOR_RUNNING_MODAL;
}

/**
 * Deletes the `delete_id` face set from the mesh face sets
 * and stores the result in `r_face_set`.
 * The faces that were using the `delete_id` face set are filled
 * using the content from their neighbors.
 */
static void delete_face_set_id(
    int *r_face_sets, Object &object, Cache &expand_cache, Mesh *mesh, const int delete_id)
{
  const GroupedSpan<int> vert_to_face_map = mesh->vert_to_face_map();
  const OffsetIndices faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();

  /* Check that all the face set IDs in the mesh are not equal to `delete_id`
   * before attempting to delete it. */
  bool all_same_id = true;
  for (const int i : faces.index_range()) {
    if (!is_face_in_active_component(object, faces, corner_verts, expand_cache, i)) {
      continue;
    }
    if (r_face_sets[i] != delete_id) {
      all_same_id = false;
      break;
    }
  }
  if (all_same_id) {
    return;
  }

  BLI_LINKSTACK_DECLARE(queue, void *);
  BLI_LINKSTACK_DECLARE(queue_next, void *);

  BLI_LINKSTACK_INIT(queue);
  BLI_LINKSTACK_INIT(queue_next);

  for (const int i : faces.index_range()) {
    if (r_face_sets[i] == delete_id) {
      BLI_LINKSTACK_PUSH(queue, POINTER_FROM_INT(i));
    }
  }

  const ObjectState &state = object_state_get(expand_cache, object);
  while (BLI_LINKSTACK_SIZE(queue)) {
    bool any_updated = false;
    while (BLI_LINKSTACK_SIZE(queue)) {
      const int f_index = POINTER_AS_INT(BLI_LINKSTACK_POP(queue));
      int other_id = delete_id;
      for (const int vert : corner_verts.slice(faces[f_index])) {
        for (const int neighbor_face_index : vert_to_face_map[vert]) {
          if (state.original_face_sets[neighbor_face_index] <= 0) {
            /* Skip picking IDs from hidden face sets. */
            continue;
          }
          if (r_face_sets[neighbor_face_index] != delete_id) {
            other_id = r_face_sets[neighbor_face_index];
          }
        }
      }

      if (other_id != delete_id) {
        any_updated = true;
        r_face_sets[f_index] = other_id;
      }
      else {
        BLI_LINKSTACK_PUSH(queue_next, POINTER_FROM_INT(f_index));
      }
    }
    if (!any_updated) {
      /* No face sets were updated in this iteration, which means that no more content to keep
       * filling the faces of the deleted face set was found. Break to avoid entering an infinite
       * loop trying to search for those faces again. */
      break;
    }

    BLI_LINKSTACK_SWAP(queue, queue_next);
  }

  BLI_LINKSTACK_FREE(queue);
  BLI_LINKSTACK_FREE(queue_next);
}

static void cache_initial_config_set(bContext *C, wmOperator *op, Cache &expand_cache)
{
  expand_cache.normal_falloff_blur_steps = RNA_int_get(op->ptr, "normal_falloff_smooth");
  expand_cache.invert = RNA_boolean_get(op->ptr, "invert");
  expand_cache.preserve = RNA_boolean_get(op->ptr, "use_mask_preserve");
  expand_cache.auto_mask = RNA_boolean_get(op->ptr, "use_auto_mask");
  expand_cache.falloff_gradient = RNA_boolean_get(op->ptr, "use_falloff_gradient");
  expand_cache.target = TargetType(RNA_enum_get(op->ptr, "target"));
  expand_cache.modify_active_face_set = RNA_boolean_get(op->ptr, "use_modify_active");
  expand_cache.reposition_pivot = RNA_boolean_get(op->ptr, "use_reposition_pivot");
  expand_cache.max_geodesic_move_preview = RNA_int_get(op->ptr, "max_geodesic_move_preview");

  /* These can be exposed in RNA if needed. */
  expand_cache.loop_count = 1;
  expand_cache.brush_gradient = false;

  /* Texture and color data from the active Brush. */
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  expand_cache.paint = paint;
  expand_cache.brush = BKE_paint_brush_for_read(&sd.paint);
  BKE_curvemapping_init(expand_cache.brush->curve_distance_falloff);
  copy_v4_fl(expand_cache.fill_color, 1.0f);
  copy_v3_v3(expand_cache.fill_color, BKE_brush_color_get(paint, expand_cache.brush));

  expand_cache.scene = CTX_data_scene(C);
  expand_cache.texture_distortion_strength = 0.0f;
  expand_cache.blend_mode = expand_cache.brush->blend;
}

/**
 * Populates `expand_cache.object_states`, active object first, matching #sculpt_mode_objects
 * order. Must run before any code that touches `active_object_state()` or the per-object arrays.
 */
static void object_states_init(bContext *C, Cache &expand_cache)
{
  Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, &depsgraph);
  expand_cache.object_states.clear();
  for (Object *object : sculpt_mode_objects(vc)) {
    /* Secondary objects' PBVH / evaluated data may not be current; every multi-object sculpt
     * operator prepares each object before touching its PBVH (e.g. sculpt_face_set.cc). */
    BKE_sculpt_update_object_for_edit(&depsgraph, object, false);
    /* Every object's connected-island ids must be available, not just the active object's: both
     * seed resolution (#find_active_connected_components_from_vert) and
     * #is_vert_in_active_component on secondary objects call #islands::vert_id_get. Idempotent --
     * guards on emptiness -- so this is a no-op for objects whose islands are already cached. */
    islands::ensure_cache(*object);
    /* Secondary objects also need their boundary info: the multi-object apply/pivot paths call
     * #boundary_from_enabled (and BoundaryTopology seeds) on every object, which dereferences
     * `SculptSession::boundary_info_cache`. `ensure_sculptsession_data` only prepares the active
     * object, so ensure it here for the rest. Idempotent (guards on the cache pointer). */
    boundary::ensure_boundary_info(*object);
    ObjectState state;
    state.object = object;
    if (bke::object::pbvh_get(*object)->type() == bke::pbvh::Type::Grids) {
      state.grids_canonical_map = grids_canonical_map_create(*object);
    }
    expand_cache.object_states.append(std::move(state));
  }
}

/**
 * Does the undo sculpt push for the affected target data of the #Cache.
 */
static void undo_push(const Depsgraph &depsgraph, Cache &expand_cache)
{
  for (const ObjectState &state : expand_cache.object_states) {
    Object &ob = *state.object;
    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*bke::object::pbvh_get(ob), memory);
    switch (expand_cache.target) {
      case TargetType::Mask:
        undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Mask);
        break;
      case TargetType::FaceSets:
        undo::push_nodes(depsgraph, ob, node_mask, undo::Type::FaceSet);
        break;
      case TargetType::Colors: {
        undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Color);
        break;
      }
    }
  }
}

static bool any_nonzero_mask(const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask");
      if (mask.is_empty()) {
        return false;
      }
      return std::any_of(
          mask.begin(), mask.end(), [&](const float value) { return value > 0.0f; });
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const Span<float> mask = subdiv_ccg.masks;
      if (mask.is_empty()) {
        return false;
      }
      return std::any_of(
          mask.begin(), mask.end(), [&](const float value) { return value > 0.0f; });
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ss.bm;
      const int offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
      if (offset == -1) {
        return false;
      }
      BMIter iter;
      BMVert *vert;
      BM_ITER_MESH (vert, &iter, &bm, BM_VERTS_OF_MESH) {
        if (BM_ELEM_CD_GET_FLOAT(vert, offset) > 0.0f) {
          return true;
        }
      }
      return false;
    }
  }
  return false;
}

static wmOperatorStatus sculpt_expand_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const Scene &scene = *CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  Mesh *mesh = id_cast<Mesh *>(ob.data);

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  /* Create and configure the Expand Cache. */
  ss.expand_cache = MEM_new<Cache>(__func__);
  cache_initial_config_set(C, op, *ss.expand_cache);
  object_states_init(C, *ss.expand_cache);

  if (ss.expand_cache->target == TargetType::Colors && ss.expand_cache->object_states.size() > 1) {
    for (const ObjectState &state : ss.expand_cache->object_states) {
      if (bke::object::pbvh_get(*state.object)->type() != bke::pbvh::Type::Mesh) {
        BKE_reportf(op->reports,
                   RPT_WARNING,
                   "Expand Color target does not support Multires: skipping \"%s\"",
                   state.object->id.name + 2);
      }
    }
  }

  if (expand_multi_object_active(*ss.expand_cache)) {
    /* World-space positions are only needed for cross-object falloff (Task 2.4+); building them
     * once here keeps them valid for the whole modal op (static-geometry invariant). */
    Vector<Object *> objects;
    for (const ObjectState &state : ss.expand_cache->object_states) {
      objects.append(state.object);
    }
    ss.expand_cache->world_positions = world_positions_create(*depsgraph, objects);
    ss.expand_cache->bridge = build_multi_object_bridge(
        *depsgraph, objects, ss.expand_cache->world_positions, SCULPT_EXPAND_BRIDGE_FACTOR);
  }

  /* Update object. */
  const bool needs_colors = ss.expand_cache->target == TargetType::Colors;

  if (needs_colors) {
    /* CTX_data_ensure_evaluated_depsgraph should be used at the end to include the updates of
     * earlier steps modifying the data. Expand itself stays single-object, but it writes into the
     * shared multi-object color channel. */
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    color::ensure_shared_color_attributes(ob, sculpt_mode_objects(vc));
    depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  }

  if (ss.expand_cache->target == TargetType::Mask) {
    Scene &scene = *CTX_data_scene(C);
    Vector<Object *> mask_layer_objects;
    for (const ObjectState &state : ss.expand_cache->object_states) {
      mask_layer_objects.append(state.object);
    }
    /* Ensure the grid paint mask layer exists on EVERY object in the group, not just the active
     * one -- the multi-object Mask apply path (#update_mask_grids) indexes SubdivCCG::masks
     * unconditionally, which is left empty for a Multires object that has never had a mask layer
     * created. Same class of bug already fixed for the Mask gesture tools (see
     * #ensure_mask_layers's use in paint_mask.cc's init_operation); Expand's invoke only ever
     * ensured the layer for the active object, which was harmless while multi-object Expand was
     * Mesh-only and became a null-read crash once Grids objects were admitted into the group. */
    ensure_mask_layers(depsgraph, CTX_data_main(C), &scene, mask_layer_objects);

    if (RNA_boolean_get(op->ptr, "use_auto_mask")) {
      if (any_nonzero_mask(ob)) {
        write_mask_data(
            CTX_data_depsgraph_pointer(C), ob, Array<float>(vertex_count_get(ob), 1.0f));
      }
    }
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, needs_colors);

  if (ss.expand_cache->target == TargetType::Mask) {
    ed::sculpt_paint::mask_overlay_check(*C, *op);
  }
  else if (ss.expand_cache->target == TargetType::FaceSets) {
    ed::sculpt_paint::face_set_overlay_check(*C, *op);
  }

  /* Do nothing when the mesh has 0 vertices. */
  const int totvert = vertex_count_get(ob);
  if (totvert == 0) {
    expand_cache_free(ss);
    return OPERATOR_CANCELLED;
  }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  /* Face set operations are not supported in dyntopo. */
  if (ss.expand_cache->target == TargetType::FaceSets && pbvh.type() == bke::pbvh::Type::BMesh) {
    expand_cache_free(ss);
    return OPERATOR_CANCELLED;
  }

  ensure_sculptsession_data(ob);

  /* Set the initial element for expand from the event position. */
  const float mouse[2] = {float(event->mval[0]), float(event->mval[1])};

  /* When getting the initial active vert, in cases where the cursor is not over the mesh and
   * the mesh type has changed, we cannot proceed with the expand operator, as there is no
   * sensible last active vertex when switching between backing implementations. */
  if (!set_initial_components_for_mouse(C, ob, *ss.expand_cache, mouse)) {
    expand_cache_free(ss);
    return OPERATOR_CANCELLED;
  }

  /* Initialize undo.
   *
   * If multi-object is active we use the unified #push_begin_multi_object helper (it pushes the
   * active object first, then iterates the secondaries); the expand cache's `object_states`
   * starts with `ob = `active object` (see #state_check), so a simple Span over its pointers
   * (after the active-first dedup) is exactly what the helper expects. For the single-object
   * path we keep the simple `push_begin` call -- `push_begin_multi_object` is a no-op on an
   * empty span but adds an indirection that isn't worth it on this hot path. */
  if (expand_multi_object_active(*ss.expand_cache)) {
    Vector<Object *> all_objects;
    all_objects.append(&ob);
    for (const ObjectState &state : ss.expand_cache->object_states) {
      if (state.object != &ob) {
        all_objects.append(state.object);
      }
    }
    undo::push_begin_multi_object(scene, op, all_objects.as_span());
  }
  else {
    undo::push_begin(scene, ob, op);
  }
  undo_push(*depsgraph, *ss.expand_cache);

  /* Cache bke::pbvh::Tree nodes for every object. The multi-object apply loop reads each object's own
   * `node_mask`; without this, secondary objects would apply Mask/Colors to zero nodes. */
  for (ObjectState &state : ss.expand_cache->object_states) {
    state.node_mask_memory = std::make_unique<IndexMaskMemory>();
    state.node_mask = bke::pbvh::all_leaf_nodes(*bke::object::pbvh_get(*state.object),
                                                *state.node_mask_memory);
  }

  /* Store initial state. */
  original_state_store(ob, *ss.expand_cache);

  if (ss.expand_cache->modify_active_face_set) {
    if (expand_multi_object_active(*ss.expand_cache)) {
      /* Delete the shared next-face-set id on every object; each uses its own initial face sets
       * and mesh, populated per-object by #original_state_store above. */
      for (ObjectState &state : ss.expand_cache->object_states) {
        Mesh &state_mesh = *id_cast<Mesh *>(state.object->data);
        delete_face_set_id(state.initial_face_sets.data(),
                           *state.object,
                           *ss.expand_cache,
                           &state_mesh,
                           ss.expand_cache->next_face_set);
      }
    }
    else {
      delete_face_set_id(active_object_state(*ss.expand_cache).initial_face_sets.data(),
                         ob,
                         *ss.expand_cache,
                         mesh,
                         ss.expand_cache->next_face_set);
    }
  }

  const int initial_vert = ss.expand_cache->initial_active_vert;

  /* Initialize the falloff. */
  FalloffType falloff_type = FalloffType(RNA_enum_get(op->ptr, "falloff_type"));

  /* When starting from a boundary vertex, set the initial falloff to boundary. */
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      if (boundary::vert_is_boundary(
              vert_to_face_map, hide_poly, ss.boundary_info_cache->verts, initial_vert))
      {
        falloff_type = FalloffType::BoundaryTopology;
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Mesh &base_mesh = *id_cast<const Mesh *>(ob.data);
      const OffsetIndices<int> faces = base_mesh.faces();
      const Span<int> corner_verts = base_mesh.corner_verts();
      const bke::AttributeAccessor attributes = base_mesh.attributes();
      const VArraySpan face_sets = *attributes.lookup_or_default<int>(
          ".sculpt_face_set", bke::AttrDomain::Face, 0);
      const SubdivCCG &subdiv_ccg = *ob.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      if (boundary::vert_is_boundary(faces,
                                     corner_verts,
                                     ss.boundary_info_cache->verts,
                                     ss.boundary_info_cache->edges,
                                     subdiv_ccg,
                                     SubdivCCGCoord::from_index(key, initial_vert)))
      {
        falloff_type = FalloffType::BoundaryTopology;
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ob.runtime->sculpt_session->bm;
      vert_random_access_ensure(ob);
      if (boundary::vert_is_boundary(BM_vert_at_index(&bm, initial_vert))) {
        falloff_type = FalloffType::BoundaryTopology;
      }
      break;
    }
  }

  calc_falloff_from_vert_and_symmetry(
      *depsgraph, *ss.expand_cache, ob, initial_vert, falloff_type);

  check_topology_islands(ob, falloff_type);

  /* Initial mesh data update, resets all target data in the sculpt mesh. Uses `expand_cache.seed`
   * (not the plain `initial_vert`) so a cross-mesh seed reads its falloff from its own object.
   * Single object ⇒ `seed` == `{0, initial_vert}` (see #set_initial_components_for_mouse) ⇒
   * identical to before. */
  update_for_vert(C, ob, ss.expand_cache->seed);

  sculpt_expand_status(C, op, ss.expand_cache);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

void modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {SCULPT_EXPAND_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {SCULPT_EXPAND_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {SCULPT_EXPAND_MODAL_INVERT, "INVERT", 0, "Invert", ""},
      {SCULPT_EXPAND_MODAL_PRESERVE_TOGGLE, "PRESERVE", 0, "Toggle Preserve State", ""},
      {SCULPT_EXPAND_MODAL_GRADIENT_TOGGLE, "GRADIENT", 0, "Toggle Gradient", ""},
      {SCULPT_EXPAND_MODAL_RECURSION_STEP_GEODESIC,
       "RECURSION_STEP_GEODESIC",
       0,
       "Geodesic recursion step",
       ""},
      {SCULPT_EXPAND_MODAL_RECURSION_STEP_TOPOLOGY,
       "RECURSION_STEP_TOPOLOGY",
       0,
       "Topology recursion Step",
       ""},
      {SCULPT_EXPAND_MODAL_MOVE_TOGGLE, "MOVE_TOGGLE", 0, "Move Origin", ""},
      {SCULPT_EXPAND_MODAL_FALLOFF_GEODESIC, "FALLOFF_GEODESICS", 0, "Geodesic Falloff", ""},
      {SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY, "FALLOFF_TOPOLOGY", 0, "Topology Falloff", ""},
      {SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY_DIAGONALS,
       "FALLOFF_TOPOLOGY_DIAGONALS",
       0,
       "Diagonals Falloff",
       ""},
      {SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL, "FALLOFF_SPHERICAL", 0, "Spherical Falloff", ""},
      {SCULPT_EXPAND_MODAL_SNAP_TOGGLE, "SNAP_TOGGLE", 0, "Snap expand to Face Sets", ""},
      {SCULPT_EXPAND_MODAL_SNAP_SEED_OBJECT_ONLY_TOGGLE,
       "SNAP_SEED_OBJECT_ONLY_TOGGLE",
       0,
       "Restrict Snap to the Object Expand Started On",
       ""},
      {SCULPT_EXPAND_MODAL_LOOP_COUNT_INCREASE,
       "LOOP_COUNT_INCREASE",
       0,
       "Loop Count Increase",
       ""},
      {SCULPT_EXPAND_MODAL_LOOP_COUNT_DECREASE,
       "LOOP_COUNT_DECREASE",
       0,
       "Loop Count Decrease",
       ""},
      {SCULPT_EXPAND_MODAL_BRUSH_GRADIENT_TOGGLE,
       "BRUSH_GRADIENT_TOGGLE",
       0,
       "Toggle Brush Gradient",
       ""},
      {SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_INCREASE,
       "TEXTURE_DISTORTION_INCREASE",
       0,
       "Texture Distortion Increase",
       ""},
      {SCULPT_EXPAND_MODAL_TEXTURE_DISTORTION_DECREASE,
       "TEXTURE_DISTORTION_DECREASE",
       0,
       "Texture Distortion Decrease",
       ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const char *name = "Sculpt Expand Modal";
  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);
  WM_modalkeymap_assign(keymap, "SCULPT_OT_expand");
}

void SCULPT_OT_expand(wmOperatorType *ot)
{
  ot->name = "Expand";
  ot->idname = "SCULPT_OT_expand";
  ot->description = "Generic sculpt expand operator";

  ot->invoke = sculpt_expand_invoke;
  ot->modal = sculpt_expand_modal;
  ot->cancel = sculpt_expand_cancel;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  static EnumPropertyItem prop_sculpt_expand_falloff_type_items[] = {
      {int(FalloffType::Geodesic), "GEODESIC", 0, "Geodesic", ""},
      {int(FalloffType::Topology), "TOPOLOGY", 0, "Topology", ""},
      {int(FalloffType::TopologyNormals), "TOPOLOGY_DIAGONALS", 0, "Topology Diagonals", ""},
      {int(FalloffType::Normals), "NORMALS", 0, "Normals", ""},
      {int(FalloffType::Sphere), "SPHERICAL", 0, "Spherical", ""},
      {int(FalloffType::BoundaryTopology), "BOUNDARY_TOPOLOGY", 0, "Boundary Topology", ""},
      {int(FalloffType::BoundaryFaceSet), "BOUNDARY_FACE_SET", 0, "Boundary Face Set", ""},
      {int(FalloffType::ActiveFaceSet), "ACTIVE_FACE_SET", 0, "Active Face Set", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static EnumPropertyItem prop_sculpt_expand_target_type_items[] = {
      {int(TargetType::Mask), "MASK", 0, "Mask", ""},
      {int(TargetType::FaceSets), "FACE_SETS", 0, "Face Sets", ""},
      {int(TargetType::Colors), "COLOR", 0, "Color", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_enum(ot->srna,
               "target",
               prop_sculpt_expand_target_type_items,
               int(TargetType::Mask),
               "Data Target",
               "Data that is going to be modified in the expand operation");

  RNA_def_enum(ot->srna,
               "falloff_type",
               prop_sculpt_expand_falloff_type_items,
               int(FalloffType::Geodesic),
               "Falloff Type",
               "Initial falloff of the expand operation");

  ot->prop = RNA_def_boolean(
      ot->srna, "invert", false, "Invert", "Invert the expand active elements");
  ot->prop = RNA_def_boolean(ot->srna,
                             "use_mask_preserve",
                             false,
                             "Preserve Previous",
                             "Preserve the previous state of the target data");
  ot->prop = RNA_def_boolean(ot->srna,
                             "use_falloff_gradient",
                             false,
                             "Falloff Gradient",
                             "Expand Using a linear falloff");

  ot->prop = RNA_def_boolean(ot->srna,
                             "use_modify_active",
                             false,
                             "Modify Active",
                             "Modify the active face set instead of creating a new one");

  ot->prop = RNA_def_boolean(
      ot->srna,
      "use_reposition_pivot",
      true,
      "Reposition Pivot",
      "Reposition the sculpt transform pivot to the boundary of the expand active area");

  ot->prop = RNA_def_int(ot->srna,
                         "max_geodesic_move_preview",
                         10000,
                         0,
                         INT_MAX,
                         "Max Vertex Count for Geodesic Move Preview",
                         "Maximum number of vertices in the mesh for using geodesic falloff when "
                         "moving the origin of expand. If the total number of vertices is greater "
                         "than this value, the falloff will be set to spherical when moving",
                         0,
                         1000000);
  ot->prop = RNA_def_boolean(ot->srna,
                             "use_auto_mask",
                             false,
                             "Auto Create",
                             "Fill in mask if nothing is already masked");
  ot->prop = RNA_def_int(ot->srna,
                         "normal_falloff_smooth",
                         2,
                         0,
                         10,
                         "Normal Smooth",
                         "Blurring steps for normal falloff",
                         0,
                         10);
}

}  // namespace ed::sculpt_paint::expand

}  // namespace blender
