/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#include "BLI_array.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph_query.hh"

#include "multires_reshape.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Sculpt layer composition helpers
 *
 * Grid-domain sculpt layers store tangent displacement in the MDisps layout at the top level and
 * are composed with the base displacement at evaluation time (see
 * `subdiv_displacement_multires.cc`). The reshape machinery converts the sculpted CCG surface
 * (which is the composed surface) back to tangent displacement, so the layers have to be
 * temporarily composed into MDisps for the round-trip to be consistent, and subtracted back
 * afterwards: #CD_MDISPS only ever stores the base surface.
 * \{ */

/* [DEBUG-flush] Diagnostics for the layer-aware CCG -> base MDisps flush. Prints one summary per
 * flush with the subtracted layer set and the distribution of the resulting base change, so a leak
 * of layer data into the base can be attributed to a concrete flush. Tied to the module-wide
 * #SCULPT_LAYERS_DEBUG_LOG switch (see `BKE_sculpt_layers.hh`). */
#define SCULPT_LAYERS_DEBUG_FLUSH SCULPT_LAYERS_DEBUG_LOG
#if SCULPT_LAYERS_DEBUG_FLUSH
#  define SLF_PERF(...) printf(__VA_ARGS__)
#else
#  define SLF_PERF(...) ((void)0)
#endif

Vector<MultiresGridSculptLayer> BKE_multires_grid_sculpt_layers_collect(const Mesh &mesh,
                                                                        const int grid_area)
{
  Vector<MultiresGridSculptLayer> layers;
  const int64_t expected_totelem = int64_t(mesh.corners_num) * grid_area;
  for (const SculptLayer *layer_ptr : bke::sculpt_layers::layers(mesh)) {
    const SculptLayer &layer = *layer_ptr;
    if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr) {
      continue;
    }
    /* #bke::sculpt_layers::effective is the single authority for a layer's contribution weight
     * (visibility gate + influence); routing through it keeps the grid path in sync with the mesh
     * path if the blend/mute semantics ever change. */
    const float eff = bke::sculpt_layers::effective(layer);
    if (eff == 0.0f) {
      continue;
    }
    if (int64_t(layer.totelem) != expected_totelem) {
      /* A layer whose data does not match the top-level MDisps layout (e.g. after a base topology
       * change) is skipped. Both the evaluator and the flush call this same collector, so they can
       * never select a different set — the classic source of a base leak is structurally removed. */
      continue;
    }
    const bke::sculpt_layers::CompositeMask masks = bke::sculpt_layers::grid_masks_for_composite(
        layer, expected_totelem, grid_area);
    layers.append({static_cast<const float3 *>(layer.data), eff, masks});
  }
  return layers;
}

bke::sculpt_layers::MaskBlockWeight BKE_multires_grid_sculpt_layer_weight(
    const MultiresGridSculptLayer &layer, const int grid_index, const float sign)
{
  /* One grid is exactly one mask block, so the grid index is the block index. Guaranteed by the
   * `block_size == grid_area` check in #grid_masks_for_composite. */
  return bke::sculpt_layers::mask_block_weight(layer.masks, grid_index, sign * layer.influence);
}

/* The temporary composition is only correct when every displacement grid is already allocated at
 * the top level: otherwise #multires_reshape_ensure_grids would discard composed values for the
 * reallocated grids and the later subtraction would corrupt the base. */
static bool sculpt_grid_layers_applicable(const MDisps *mdisps,
                                          const int grids_num,
                                          const int grid_area)
{
  for (int i = 0; i < grids_num; i++) {
    if (mdisps[i].disps == nullptr || mdisps[i].totdisp != grid_area) {
      return false;
    }
  }
  return true;
}

static void mdisps_add_sculpt_layers(MDisps *mdisps,
                                     const int grids_num,
                                     const int grid_area,
                                     const Span<MultiresGridSculptLayer> layers,
                                     const float sign,
                                     const Span<bool> grid_enabled = {})
{
  threading::parallel_for(IndexRange(grids_num), 8, [&](const IndexRange range) {
    for (const int grid_index : range) {
      if (!grid_enabled.is_empty() && !grid_enabled[grid_index]) {
        continue;
      }
      float3 *disps = reinterpret_cast<float3 *>(mdisps[grid_index].disps);
      for (const MultiresGridSculptLayer &layer : layers) {
        const float3 *layer_data = layer.data + int64_t(grid_index) * grid_area;
        /* Folded once per grid, not per element: an unmasked layer and a layer whose mask is
         * uniform over this grid both collapse to a single scalar, and a grid the layer is fully
         * masked out of is skipped outright. */
        const bke::sculpt_layers::MaskBlockWeight weight =
            BKE_multires_grid_sculpt_layer_weight(layer, grid_index, sign);
        if (weight.skip) {
          continue;
        }
        if (weight.fold == bke::sculpt_layers::MaskFold::Uniform) {
          for (int i = 0; i < grid_area; i++) {
            disps[i] += layer_data[i] * weight.weight;
          }
          continue;
        }
        for (int i = 0; i < grid_area; i++) {
          disps[i] += layer_data[i] * bke::sculpt_layers::mask_elem_weight(weight, i);
        }
      }
    }
  });
}

static void restore_ccg_positions_from_sculpt_layers(SubdivCCG &subdiv_ccg,
                                                     const Span<float3> contribution)
{
  if (contribution.is_empty()) {
    return;
  }
  threading::parallel_for(subdiv_ccg.positions.index_range(), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      subdiv_ccg.positions[i] += contribution[i];
    }
  });
}

static bool subtract_sculpt_layers_from_ccg_positions(Mesh &mesh,
                                                      SubdivCCG &subdiv_ccg,
                                                      const int top_level,
                                                      Array<float3> &r_contribution)
{
  r_contribution = {};
  /* Bail out before the O(N) buffer allocations and zero-fill when the mesh carries no grid
   * layers at all, which is the common case for meshes that never use sculpt layers. */
  if (!BKE_multires_mesh_has_grid_sculpt_layers(mesh)) {
    return true;
  }
  /* Select the exact same layer set the displacement evaluator composed onto the CCG (it validates
   * against the top-level MDisps layout, see #BKE_multires_grid_sculpt_layers_collect). A layer at
   * a mismatching size the evaluator skipped must be skipped here too: subtracting a contribution
   * the CCG never contained would permanently dent the base by that layer's displacement. */
  const int top_grid_size = bke::subdiv::grid_size_from_level(top_level);
  const int64_t expected_totelem = int64_t(mesh.corners_num) * top_grid_size * top_grid_size;

  /* Same bail-out as above, extended to the layers that would actually contribute: a mesh whose
   * grid layers are all disabled (zero effective influence) or size-mismatched still has nothing
   * to subtract, so skip the O(N) allocations below for that case too. */
  bool any_contributing_layer = false;
  for (const SculptLayer *layer_ptr : bke::sculpt_layers::layers(mesh)) {
    const SculptLayer &layer = *layer_ptr;
    if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr) {
      continue;
    }
    if (bke::sculpt_layers::effective(layer) == 0.0f) {
      continue;
    }
    if (int64_t(layer.totelem) != expected_totelem) {
      continue;
    }
    any_contributing_layer = true;
    break;
  }
  if (!any_contributing_layer) {
    return true;
  }

  Array<float3> total(subdiv_ccg.positions.size(), float3(0.0f));
  Array<float3> contribution(subdiv_ccg.positions.size());
  bool any_enabled_layer = false;
  for (const SculptLayer *layer_ptr : bke::sculpt_layers::layers(mesh)) {
    const SculptLayer &layer = *layer_ptr;
    if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr) {
      continue;
    }
    /* Same effective-influence authority and top-level predicate as the evaluator's collector, so
     * the flush subtracts exactly the set (and weights) that were composed onto the CCG. */
    const float eff = bke::sculpt_layers::effective(layer);
    if (eff == 0.0f) {
      continue;
    }
    if (int64_t(layer.totelem) != expected_totelem) {
      continue;
    }
    if (!BKE_multires_sculpt_layer_object_contribution(mesh, subdiv_ccg, layer, contribution)) {
      return false;
    }
    any_enabled_layer = true;
    threading::parallel_for(total.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        total[i] += contribution[i] * eff;
      }
    });
  }

  if (!any_enabled_layer) {
    return true;
  }

  threading::parallel_for(subdiv_ccg.positions.index_range(), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      subdiv_ccg.positions[i] -= total[i];
    }
  });
  r_contribution = std::move(total);
  return true;
}

bool BKE_multires_mesh_has_grid_sculpt_layers(const Mesh &mesh)
{
  for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (layer->domain == SCULPT_LAYER_DOMAIN_GRID && layer->data != nullptr) {
      return true;
    }
  }
  return false;
}

void BKE_multires_sculpt_layer_apply_to_mdisps(Mesh &mesh, const SculptLayer &layer, float factor)
{
  if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr || factor == 0.0f) {
    return;
  }
  MDisps *mdisps = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh.corner_data, CD_MDISPS, mesh.corners_num));
  if (mdisps == nullptr || mesh.corners_num == 0) {
    return;
  }
  const int grid_area = mdisps[0].totdisp;
  if (grid_area <= 0 || int64_t(layer.totelem) != int64_t(mesh.corners_num) * grid_area) {
    return;
  }
  if (!sculpt_grid_layers_applicable(mdisps, mesh.corners_num, grid_area)) {
    return;
  }
  /* The bake writes the layer's *masked* contribution into the base, and the matching undo passes a
   * negated \a factor through this same path, so both directions weigh every element identically.
   * The recovery is not bit-exact — see the note on #accumulate_layer — but its error is bounded by
   * one rounding step rather than growing with each bake / undo pair. */
  const MultiresGridSculptLayer scaled = {
      static_cast<const float3 *>(layer.data),
      factor,
      bke::sculpt_layers::grid_masks_for_composite(layer, layer.totelem, grid_area)};
  mdisps_add_sculpt_layers(mdisps, mesh.corners_num, grid_area, Span(&scaled, 1), 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from object
 * \{ */

static bool multiresModifier_reshapeFromVertcos(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                Span<float3> positions)
{
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return false;
  }
  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(id_cast<Mesh *>(object->data), reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_vertcos(&reshape_context, positions)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

bool multiresModifier_reshapeFromObject(Depsgraph *depsgraph,
                                        MultiresModifierData *mmd,
                                        Object *dst,
                                        Object *src)
{
  const Object *ob_eval = DEG_get_evaluated(depsgraph, src);
  if (!ob_eval) {
    return false;
  }
  const Mesh *src_mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (!src_mesh_eval) {
    return false;
  }

  return multiresModifier_reshapeFromVertcos(depsgraph, dst, mmd, src_mesh_eval->vert_positions());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from modifier
 * \{ */

bool multiresModifier_reshapeFromDeformModifier(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                ModifierData *deform_md)
{
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  /* Create mesh for the multires, ignoring any further modifiers (leading
   * deformation modifiers will be applied though). */
  Mesh *multires_mesh = BKE_multires_create_mesh(depsgraph, object, &highest_mmd);
  Array<float3> deformed_verts(multires_mesh->vert_positions());

  /* Apply deformation modifier on the multires, */
  ModifierEvalContext modifier_ctx{};
  modifier_ctx.depsgraph = depsgraph;
  modifier_ctx.object = object;
  modifier_ctx.flag = MOD_APPLY_USECACHE | MOD_APPLY_IGNORE_SIMPLIFY;

  const bool deform_success = BKE_modifier_deform_verts(
      deform_md, &modifier_ctx, multires_mesh, deformed_verts);
  BKE_id_free(nullptr, multires_mesh);
  if (!deform_success) {
    return false;
  }

  /* Reshaping */
  bool result = multiresModifier_reshapeFromVertcos(
      depsgraph, object, &highest_mmd, deformed_verts);

  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from grids
 * \{ */

bool multiresModifier_reshapeFromCCG(const int tot_level,
                                     Mesh *coarse_mesh,
                                     SubdivCCG *subdiv_ccg,
                                     const MultiresReshapeFromCCGMode mode)
{
  Array<float3> source_layer_contribution;
  if (mode == MultiresReshapeFromCCGMode::Base &&
      !subtract_sculpt_layers_from_ccg_positions(
          *coarse_mesh, *subdiv_ccg, tot_level, source_layer_contribution))
  {
    SLF_PERF("[sculpt-layers][flush] WARNING: reshapeFromCCG base mode skipped; could not "
             "compute sculpt layer contribution without baking layers\n");
    return false;
  }

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, subdiv_ccg, coarse_mesh, tot_level))
  {
    restore_ccg_positions_from_sculpt_layers(*subdiv_ccg, source_layer_contribution);
    return false;
  }

  multires_ensure_external_read(coarse_mesh, reshape_context.top.level);

  /* The reshape source is explicit:
   * - Composed mode: CCG positions are `base + enabled layers`; temporarily compose the same
   *   layers into MDisps so the reshape round-trip is symmetric, then subtract them back.
   * - Base mode: CCG positions were converted to base-view before context creation; do not compose
   *   layers into MDisps, otherwise layer detail would be written into #CD_MDISPS. */
  const int grid_area = reshape_context.top.grid_size * reshape_context.top.grid_size;
  Vector<MultiresGridSculptLayer> layers;
  if (mode == MultiresReshapeFromCCGMode::Composed) {
    layers = BKE_multires_grid_sculpt_layers_collect(*coarse_mesh, grid_area);
  }
  if (!layers.is_empty() &&
      !sculpt_grid_layers_applicable(
          reshape_context.mdisps, reshape_context.num_grids, grid_area))
  {
    SLF_PERF(
        "[sculpt-layers][flush] WARNING: MDisps not fully allocated at top level, flushing "
        "WITHOUT layer subtraction (composed surface bakes into the base!)\n");
    layers.clear();
  }
#if SCULPT_LAYERS_DEBUG_FLUSH
  /* Snapshot the pre-flush base displacement so the net base change of this flush can be
   * reported. Taken before the temporary layer composition so it also covers flushes with no
   * enabled layers (e.g. Solo Base); requires all grids at the top level to stay comparable. */
  Array<float3> debug_base;
  if (sculpt_grid_layers_applicable(reshape_context.mdisps, reshape_context.num_grids, grid_area))
  {
    debug_base.reinitialize(int64_t(reshape_context.num_grids) * grid_area);
    threading::parallel_for(IndexRange(reshape_context.num_grids), 32, [&](const IndexRange range) {
      for (const int grid_index : range) {
        const float3 *disps = reinterpret_cast<const float3 *>(
            reshape_context.mdisps[grid_index].disps);
        std::copy_n(disps, grid_area, debug_base.data() + int64_t(grid_index) * grid_area);
      }
    });
  }
#endif

  if (!layers.is_empty()) {
    mdisps_add_sculpt_layers(
        reshape_context.mdisps, reshape_context.num_grids, grid_area, layers, 1.0f);
  }

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_ccg(&reshape_context, subdiv_ccg)) {
    if (!layers.is_empty()) {
      mdisps_add_sculpt_layers(
          reshape_context.mdisps, reshape_context.num_grids, grid_area, layers, -1.0f);
    }
    multires_reshape_context_free(&reshape_context);
    restore_ccg_positions_from_sculpt_layers(*subdiv_ccg, source_layer_contribution);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  if (!layers.is_empty()) {
    mdisps_add_sculpt_layers(
        reshape_context.mdisps, reshape_context.num_grids, grid_area, layers, -1.0f);
  }

#if SCULPT_LAYERS_DEBUG_FLUSH
  {
    /* Report the base-change distribution: a correct flush changes the base only under the
     * stroke; a leak shows up as large deltas spread over many (or all) grids. */
    int changed_grids = 0;
    float max_delta = 0.0f;
    if (!debug_base.is_empty()) {
      for (int grid_index = 0; grid_index < reshape_context.num_grids; grid_index++) {
        const float3 *now = reinterpret_cast<const float3 *>(
            reshape_context.mdisps[grid_index].disps);
        const float3 *before = debug_base.data() + int64_t(grid_index) * grid_area;
        float grid_max = 0.0f;
        for (int i = 0; i < grid_area; i++) {
          const float d = math::length(now[i] - before[i]);
          grid_max = std::max(grid_max, d);
        }
        if (grid_max > 1e-5f) {
          changed_grids++;
        }
        max_delta = std::max(max_delta, grid_max);
      }
    }
    SLF_PERF(
        "[sculpt-layers][flush] reshapeFromCCG: mode=%s reshape_level=%d top_level=%d "
        "layers=%d snapshot=%d grids_changed=%d/%d max_base_delta=%.6f influences=[",
        mode == MultiresReshapeFromCCGMode::Base ? "base" : "composed",
        reshape_context.reshape.level,
        reshape_context.top.level,
        int(layers.size()),
        int(!debug_base.is_empty()),
        changed_grids,
        reshape_context.num_grids,
        double(max_delta));
    for (const MultiresGridSculptLayer &layer : layers) {
      SLF_PERF("%.3f ", double(layer.influence));
    }
    SLF_PERF("]\n");
  }
#endif

  multires_reshape_context_free(&reshape_context);
  restore_ccg_positions_from_sculpt_layers(*subdiv_ccg, source_layer_contribution);
  return true;
}

/* FNV-1a hash of the coarse control mesh, used to detect when the base-mesh limit surface (and
 * thus the cached tangent frames) changed. Covers vertex positions AND the topology that the limit
 * surface depends on: edge/face/corner counts plus the corner-vertex/corner-edge connectivity.
 * Topology-only edits that leave vertex positions unchanged (edge flips, connectivity changes with
 * equal vertex counts) would otherwise keep the position-only hash and return stale tangent frames.
 * Cheap: the coarse control mesh is small relative to the subdivided grid mesh. */
static uint64_t coarse_mesh_positions_hash(const Mesh &mesh)
{
  auto fold = [](uint64_t hash, uint64_t bits) {
    return (hash ^ bits) * 1099511628211ull;
  };
  uint64_t hash = 14695981039346656037ull;
  /* Topology sizes: a change in any of these is a structural change. */
  hash = fold(hash, uint64_t(mesh.verts_num));
  hash = fold(hash, uint64_t(mesh.edges_num));
  hash = fold(hash, uint64_t(mesh.faces_num));
  hash = fold(hash, uint64_t(mesh.corners_num));
  /* Corner connectivity: which vertex/edge each corner references. This catches connectivity-only
   * edits (edge flips, face winding changes) that preserve vertex positions and counts. */
  for (const int vert : mesh.corner_verts()) {
    hash = fold(hash, uint64_t(vert));
  }
  for (const int edge : mesh.corner_edges()) {
    hash = fold(hash, uint64_t(edge));
  }
  /* Vertex positions: the primary driver of the limit surface shape. */
  for (const float3 &p : mesh.vert_positions()) {
    for (int i = 0; i < 3; i++) {
      uint32_t bits;
      std::memcpy(&bits, &p[i], sizeof(bits));
      hash = fold(hash, uint64_t(bits));
    }
  }
  return hash;
}

bool multiresModifier_reshapeFromCCG_into_sculpt_layer(const int tot_level,
                                                       Mesh *coarse_mesh,
                                                       SubdivCCG *subdiv_ccg,
                                                       const Span<int> touched_grids,
                                                       SculptLayer &layer,
                                                       Vector<float3> &r_undo_delta)
{
#if SCULPT_LAYERS_DEBUG_FLUSH
  const auto t_start = std::chrono::high_resolution_clock::now();
#endif
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, subdiv_ccg, coarse_mesh, tot_level))
  {
    return false;
  }

  multires_ensure_external_read(coarse_mesh, reshape_context.top.level);

  const int grid_area = reshape_context.top.grid_size * reshape_context.top.grid_size;
  const int num_grids = reshape_context.num_grids;
  const int64_t total_elems = int64_t(num_grids) * grid_area;

  if (!sculpt_grid_layers_applicable(reshape_context.mdisps, num_grids, grid_area)) {
    /* Displacement grids are not fully allocated at the top level; recording is not possible
     * without corrupting the base. Callers ensure this cannot happen for a live sculpt session. */
    multires_reshape_context_free(&reshape_context);
    return false;
  }

  /* Make sure the target layer buffer exists at the top level (zero-filled on first use). Passed
   * as the 64-bit count it was computed as: narrowing here wrapped on dense meshes (a 200k-corner
   * base at level 7 reaches 3.3e9) and sized the buffer from the wrapped value while the loops
   * below kept indexing with `grid_index * grid_area` derived from the true one. */
  bke::sculpt_layers::data_ensure(layer, total_elems);
  layer.level = short(tot_level);
  MutableSpan<float3> layer_data = bke::sculpt_layers::data_get(layer);

  /* All enabled layers, including the recording one (its influence is normalized to 1.0 for the
   * duration of the recording), form the pre-stroke composed surface `T_old`. */
  const Vector<MultiresGridSculptLayer> layers = BKE_multires_grid_sculpt_layers_collect(
      *coarse_mesh, grid_area);

  /* The store / assign steps normally run over ALL grids because the detail-smoothing pass
   * (Catmull-Clark detail transport from the reshape level up to the top level) reads neighbouring
   * original grids and refines a subdivision over the whole mesh; a grid left in a different state
   * (null original, or tangent-space displacement while its neighbours hold object-space
   * coordinates) would corrupt the result even for touched grids next to it.
   *
   * That constraint only bites when there IS a smoothing pass, i.e. when recording at
   * `sculptlvl < totlvl`. When recording at the top level (`reshape.level == top.level`) the
   * smoothing pass is a no-op, no grid ever reads its neighbours, and every phase can be restricted
   * to the grids the stroke actually touched — which on a dense mesh with a small brush footprint
   * is the dominant per-stroke cost. `grid_enabled` carries that restriction: a non-empty mask
   * limits compose / store / assign / save / restore to the touched grids; an empty mask is the
   * full-mesh reshape and is byte-for-byte the original behavior. The tangent encode below is always
   * touched-only regardless. */
  const bool restrict_to_touched = reshape_context.reshape.level == reshape_context.top.level;
  Array<bool> grid_enabled;
  if (restrict_to_touched) {
    grid_enabled = Array<bool>(num_grids, false);
    for (const int grid_index : touched_grids) {
      if (grid_index >= 0 && grid_index < num_grids) {
        grid_enabled[grid_index] = true;
      }
    }
  }
  const auto grid_active = [&](const int grid_index) {
    return grid_enabled.is_empty() || grid_enabled[grid_index];
  };

  /* Save the base displacement of every grid the reshape below will overwrite (object-space assign
   * plus, at a lower sculpt level, detail smoothing), so it can be restored afterwards — MDisps only
   * ever stores the base surface (I1). With a restricted reshape only the touched grids are modified,
   * so only those are saved and restored; the untouched entries of #saved_base are never read. */
  Array<float3> saved_base(int64_t(num_grids) * grid_area);
  threading::parallel_for(IndexRange(num_grids), 32, [&](const IndexRange range) {
    for (const int grid_index : range) {
      if (!grid_active(grid_index)) {
        continue;
      }
      const float3 *disps = reinterpret_cast<const float3 *>(
          reshape_context.mdisps[grid_index].disps);
      std::copy_n(disps, grid_area, saved_base.data() + int64_t(grid_index) * grid_area);
    }
  });

  /* Compose the layers into MDisps only for the full-mesh path, where #store_original_grids below
   * snapshots the composed surface as `T_old`. The restricted (top-level) path skips both the
   * compose and the snapshot: it reconstructs `T_old` per element in the delta step from #saved_base
   * plus the same layer sum, which avoids composing into — and dup-allocating a snapshot of — every
   * touched grid (the dominant part of the store/compose cost). */
  if (!restrict_to_touched && !layers.is_empty()) {
    mdisps_add_sculpt_layers(
        reshape_context.mdisps, num_grids, grid_area, layers, 1.0f, grid_enabled);
  }
#if SCULPT_LAYERS_DEBUG_FLUSH
  const auto t_saved = std::chrono::high_resolution_clock::now();
#endif

  /* Standard reshape: after this MDisps holds `T_new`, the tangent displacement of the sculpted
   * surface at the top level. The full-mesh path also snapshots `T_old` into the original grids for
   * the detail-smoothing step (which honours #grid_enabled — empty mask, every grid). The restricted
   * path has no smoothing pass and reconstructs `T_old` itself, so it skips the snapshot entirely. */
  if (!restrict_to_touched) {
    multires_reshape_store_original_grids(&reshape_context, grid_enabled);
  }
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_ccg(&reshape_context, subdiv_ccg, grid_enabled)) {
    threading::parallel_for(IndexRange(num_grids), 32, [&](const IndexRange range) {
      for (const int grid_index : range) {
        if (!grid_active(grid_index)) {
          continue;
        }
        float3 *disps = reinterpret_cast<float3 *>(reshape_context.mdisps[grid_index].disps);
        std::copy_n(saved_base.data() + int64_t(grid_index) * grid_area, grid_area, disps);
      }
    });
    multires_reshape_context_free(&reshape_context);
    return false;
  }
#if SCULPT_LAYERS_DEBUG_FLUSH
  const auto t_assign = std::chrono::high_resolution_clock::now();
#endif
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
#if SCULPT_LAYERS_DEBUG_FLUSH
  const auto t_smooth = std::chrono::high_resolution_clock::now();
#endif
  /* Reuse cached base-mesh limit frames across strokes: the encode is dominated by evaluating the
   * base-mesh limit surface, which is stable while the coarse control mesh is unchanged. A change
   * of the coarse mesh (control-cage edit) drops the cache via the hash; a CCG rebuild drops it by
   * freeing the array. */
  {
    const uint64_t coarse_hash = coarse_mesh_positions_hash(*coarse_mesh);
    if (subdiv_ccg->multires_layer_frames_coarse_hash != coarse_hash ||
        subdiv_ccg->multires_layer_frames.size() != num_grids)
    {
      subdiv_ccg->multires_layer_frames.reinitialize(num_grids);
      subdiv_ccg->multires_layer_frames_coarse_hash = coarse_hash;
      subdiv_ccg->multires_layer_frames_bytes = 0;
      subdiv_ccg->multires_layer_frames_clock = 0;
    }

    /* Cache the whole mesh's frames when they fit under the memory ceiling (then no stroke ever
     * re-evaluates the limit surface); above the ceiling keep it and evict the least-recently
     * touched grids, so the region the brush is working stays warm instead of the first grids
     * getting frozen in forever. */
    const int64_t per_grid_bytes = int64_t(grid_area) *
                                   int64_t(sizeof(float3) + sizeof(float3x3));
    constexpr int64_t max_frame_cache_bytes = int64_t(4) * 1024 * 1024 * 1024;
    const int64_t whole_mesh_bytes = int64_t(num_grids) * per_grid_bytes;
    const int64_t cap_bytes = std::min(max_frame_cache_bytes, whole_mesh_bytes);

    MutableSpan<SubdivCCGMultiresLayerFrames> frames = subdiv_ccg->multires_layer_frames;

    /* Stamp every touched grid as most-recently-used before choosing victims, so a grid this stroke
     * still needs can never be evicted (its stamp is newer than #stroke_clock_base). */
    const int64_t stroke_clock_base = subdiv_ccg->multires_layer_frames_clock;
    for (const int grid_index : touched_grids) {
      if (grid_index >= 0 && grid_index < num_grids) {
        frames[grid_index].last_used = ++subdiv_ccg->multires_layer_frames_clock;
      }
    }

    Array<bool> populate_grids(num_grids, false);
    for (const int grid_index : touched_grids) {
      if (grid_index < 0 || grid_index >= num_grids || !frames[grid_index].positions.is_empty()) {
        continue;
      }
      /* Free least-recently-used cached grids (never one touched this stroke) until this grid fits. */
      while (subdiv_ccg->multires_layer_frames_bytes + per_grid_bytes > cap_bytes) {
        int victim = -1;
        int64_t oldest = subdiv_ccg->multires_layer_frames_clock + 1;
        for (const int64_t g : IndexRange(num_grids)) {
          if (frames[g].positions.is_empty() || frames[g].last_used > stroke_clock_base) {
            continue;
          }
          if (frames[g].last_used < oldest) {
            oldest = frames[g].last_used;
            victim = int(g);
          }
        }
        if (victim < 0) {
          /* The cap cannot hold even this stroke's touched set; encode the rest without caching. */
          break;
        }
        frames[victim].positions = {};
        frames[victim].inv_tangent_matrices = {};
        subdiv_ccg->multires_layer_frames_bytes -= per_grid_bytes;
      }
      if (subdiv_ccg->multires_layer_frames_bytes + per_grid_bytes <= cap_bytes) {
        populate_grids[grid_index] = true;
        subdiv_ccg->multires_layer_frames_bytes += per_grid_bytes;
      }
    }

    multires_reshape_object_grids_to_tangent_displacement_for_grids(
        &reshape_context, touched_grids, frames, populate_grids);
  }
#if SCULPT_LAYERS_DEBUG_FLUSH
  const auto t_reshape = std::chrono::high_resolution_clock::now();
#endif

  /* Accumulate the stroke delta `T_new - T_old` into the layer for the touched grids only, and
   * collect the explicit per-element undo delta. `T_new` is the freshly encoded tangent surface in
   * MDisps; `T_old` is the pre-stroke composed surface. The full-mesh path reads it from the
   * original-grid snapshot; the restricted path never took that snapshot and rebuilds it in place
   * from the saved base plus the same layer sum #mdisps_add_sculpt_layers would have composed —
   * arithmetically identical, `base + sum(layer.data * influence)`. */
  r_undo_delta.resize(touched_grids.size() * int64_t(grid_area));
  threading::parallel_for(touched_grids.index_range(), 8, [&](const IndexRange range) {
    for (const int64_t t : range) {
      const int grid_index = touched_grids[t];
      const int64_t grid_off = int64_t(grid_index) * grid_area;
      const float3 *new_disps = reinterpret_cast<const float3 *>(
          reshape_context.mdisps[grid_index].disps);
      float3 *layer_grid = layer_data.data() + grid_off;
      float3 *undo_grid = r_undo_delta.data() + t * grid_area;
      if (restrict_to_touched) {
        const float3 *base_grid = saved_base.data() + grid_off;
        /* Folded once per grid, ahead of the element loop, through the same helper
         * #mdisps_add_sculpt_layers uses — that is what keeps this reconstruction of `T_old`
         * arithmetically identical to the composition it stands in for. */
        Vector<bke::sculpt_layers::MaskBlockWeight, 8> weights;
        weights.reserve(layers.size());
        for (const MultiresGridSculptLayer &layer : layers) {
          weights.append(BKE_multires_grid_sculpt_layer_weight(layer, grid_index));
        }
        for (int i = 0; i < grid_area; i++) {
          float3 old_val = base_grid[i];
          for (const int li : layers.index_range()) {
            if (weights[li].skip) {
              continue;
            }
            old_val += layers[li].data[grid_off + i] *
                       bke::sculpt_layers::mask_elem_weight(weights[li], i);
          }
          const float3 delta = new_disps[i] - old_val;
          layer_grid[i] += delta;
          undo_grid[i] = delta;
        }
      }
      else {
        const float3 *old_disps = reinterpret_cast<const float3 *>(
            reshape_context.orig.mdisps[grid_index].disps);
        for (int i = 0; i < grid_area; i++) {
          const float3 delta = new_disps[i] - old_disps[i];
          layer_grid[i] += delta;
          undo_grid[i] = delta;
        }
      }
    }
  });

#if SCULPT_LAYERS_DEBUG_FLUSH
  const auto t_delta = std::chrono::high_resolution_clock::now();
#endif

  /* Restore the base into MDisps (I1: layers are never baked into the base). The compose + assign
   * above overwrote every modified grid's MDisps with the composed/top-level result, so each must be
   * restored to its pre-stroke base. This mirrors #grid_enabled exactly: only the grids that were
   * saved and modified above are restored — with an empty mask that is every grid, with a restricted
   * reshape only the touched ones (the rest were never touched). */
  threading::parallel_for(IndexRange(num_grids), 32, [&](const IndexRange range) {
    for (const int grid_index : range) {
      if (!grid_active(grid_index)) {
        continue;
      }
      float3 *disps = reinterpret_cast<float3 *>(reshape_context.mdisps[grid_index].disps);
      std::copy_n(saved_base.data() + int64_t(grid_index) * grid_area, grid_area, disps);
    }
  });

#if SCULPT_LAYERS_DEBUG_FLUSH
  {
    const auto t_restore = std::chrono::high_resolution_clock::now();
    const auto us = [](const auto a, const auto b) {
      return static_cast<long long>(
          std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    float max_delta = 0.0f;
    for (const float3 &d : r_undo_delta) {
      max_delta = std::max(max_delta, math::length(d));
    }
    SLF_PERF(
        "[sculpt-layers][flush] reshape_into_layer: reshape_level=%d top_level=%d touched=%d/%d "
        "layers_composed=%d max_layer_delta=%.6f | save+compose=%lldus store+assign=%lldus "
        "smooth=%lldus encode=%lldus delta=%lldus restore=%lldus TOTAL=%lldus\n",
        reshape_context.reshape.level,
        reshape_context.top.level,
        int(touched_grids.size()),
        num_grids,
        int(layers.size()),
        double(max_delta),
        us(t_start, t_saved),
        us(t_saved, t_assign),
        us(t_assign, t_smooth),
        us(t_smooth, t_reshape),
        us(t_reshape, t_delta),
        us(t_delta, t_restore),
        us(t_start, t_restore));
  }
#endif

  multires_reshape_context_free(&reshape_context);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Subdivision
 * \{ */

void multiresModifier_subdivide(Object *object,
                                MultiresModifierData *mmd,
                                const MultiresSubdivideModeType mode)
{
  const int top_level = mmd->totlvl + 1;
  multiresModifier_subdivide_to_level(object, mmd, top_level, mode);
}

void multiresModifier_subdivide_to_level(Object *object,
                                         MultiresModifierData *mmd,
                                         const int top_level,
                                         const MultiresSubdivideModeType mode)
{
  if (top_level <= mmd->totlvl) {
    return;
  }

  Mesh *coarse_mesh = id_cast<Mesh *>(object->data);
  if (coarse_mesh->corners_num == 0) {
    /* If there are no loops in the mesh implies there is no CD_MDISPS as well. So can early output
     * from here as there is nothing to subdivide. */
    return;
  }

  MultiresReshapeContext reshape_context;

  /* There was no multires at all, all displacement is at 0. Can simply make sure all mdisps grids
   * are allocated at a proper level and return. */
  const bool has_mdisps = CustomData_has_layer(&coarse_mesh->corner_data, CD_MDISPS);
  if (!has_mdisps) {
    CustomData_add_layer(
        &coarse_mesh->corner_data, CD_MDISPS, CD_SET_DEFAULT, coarse_mesh->corners_num);
  }

  /* NOTE: Subdivision happens from the top level of the existing multires modifier. If it is set
   * to 0 and there is mdisps layer it would mean that the modifier went out of sync with the data.
   * This happens when, for example, linking modifiers from one object to another.
   *
   * In such cases simply ensure grids to be the proper level.
   *
   * If something smarter is needed it is up to the operators which does data synchronization, so
   * that the mdisps layer is also synchronized. */
  if (!has_mdisps || top_level == 1 || mmd->totlvl == 0) {
    multires_reshape_ensure_grids(coarse_mesh, top_level);
    if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
      multires_subdivide_create_tangent_displacement_linear_grids(object, mmd);
    }
    else {
      multires_set_tot_level(object, mmd, top_level);
    }
    return;
  }

  multires_flush_sculpt_updates(object);

  if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, top_level)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  multires_reshape_assign_final_elements_from_orig_mdisps(&reshape_context);

  /* Free original grids which makes it so smoothing with details thinks all the details were
   * added against base mesh's limit surface. This is similar behavior to as if we've done all
   * displacement in sculpt mode at the old top level and then propagated to the new top level. */
  multires_reshape_free_original_grids(&reshape_context);

  if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
    multires_reshape_smooth_object_grids(&reshape_context, mode);
  }
  else {
    multires_reshape_smooth_object_grids_with_details(&reshape_context);
  }

  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);

  multires_set_tot_level(object, mmd, top_level);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply base
 * \{ */

void multiresModifier_base_apply(Depsgraph *depsgraph,
                                 Object *object,
                                 MultiresModifierData *mmd,
                                 const ApplyBaseMode mode)
{
  multires_force_sculpt_rebuild(object);

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);

  /* At this point base_mesh is object's mesh, the subdiv is initialized to the deformed state of
   * the base mesh.
   * Store coordinates of top level grids in object space which will define true shape we would
   * want to reshape to after modifying the base mesh. */
  multires_reshape_assign_final_coords_from_mdisps(&reshape_context);

  /* For modifying base mesh we only want to consider deformation caused by multires displacement
   * and ignore all deformation which might be caused by deformation modifiers leading the multires
   * one.
   * So refine the subdiv to the original mesh vertices positions, which will also need to make
   * it so object space displacement is re-evaluated for them (as in, can not re-use any knowledge
   * from the final coordinates in the object space ). */
  multires_reshape_apply_base_refine_from_base(&reshape_context);

  /* Modify original mesh coordinates. This happens in two steps:
   * - Coordinates are set to their final location, where they are intended to be in the final
   *   result.
   * - Heuristic moves them a bit, kind of canceling out the effect of subsurf (so then when
   *   multires modifier applies subsurf vertices are placed at the desired location). */
  multires_reshape_apply_base_update_mesh_coords(&reshape_context);
  if (mode == ApplyBaseMode::ForSubdivision) {
    multires_reshape_apply_base_refit_base_mesh(&reshape_context);
  }
  multires_reshape_apply_base_update_shape_key(&reshape_context);

  /* Reshape to the stored final state.
   * Not that the base changed, so the subdiv is to be refined to the new positions. Unfortunately,
   * this can not be done foe entirely cheap: if there were deformation modifiers prior to the
   * multires they need to be re-evaluated for the new base mesh. */
  multires_reshape_apply_base_refine_from_deform(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);

  multires_reshape_context_free(&reshape_context);
}

/** \} */

}  // namespace blender
