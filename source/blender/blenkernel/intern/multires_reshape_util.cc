/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "multires_reshape.hh"

#if MULTIRES_TANGENT_DEBUG
#  include <array>
#  include <atomic>
#  include <cmath>
#  include <cstdio>
#  include <mutex>

#  include "BLI_math_base.hh"
#  include "BLI_math_constants.h"
#  include "BLI_math_vector.hh"
#endif

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_task.h"
#include "BLI_task.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_multires.hh"
#include "BKE_multires_grid_resample.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subdiv_eval.hh"

#include "DEG_depsgraph_query.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Construct/destruct reshape context
 * \{ */

bke::subdiv::Subdiv *multires_reshape_create_subdiv(Depsgraph *depsgraph,
                                                    /*const*/ Object *object,
                                                    const MultiresModifierData *mmd)
{
  using namespace blender::bke;
  Mesh *base_mesh;

  if (depsgraph != nullptr) {
    Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
    Object *object_eval = DEG_get_evaluated(depsgraph, object);
    base_mesh = mesh_get_eval_deform(depsgraph, scene_eval, object_eval, &CD_MASK_BAREMESH);
  }
  else {
    base_mesh = id_cast<Mesh *>(object->data);
  }

  subdiv::Settings subdiv_settings;
  BKE_multires_subdiv_settings_init(&subdiv_settings, mmd);
  subdiv::Subdiv *subdiv = subdiv::new_from_mesh(&subdiv_settings, base_mesh);
  if (!subdiv) {
    return nullptr;
  }
  if (!subdiv::eval_begin_from_mesh(subdiv, base_mesh, subdiv::SUBDIV_EVALUATOR_TYPE_CPU)) {
    subdiv::free(subdiv);
    return nullptr;
  }
  return subdiv;
}

static void context_zero(MultiresReshapeContext *reshape_context)
{
  *reshape_context = {};
}

static void context_init_lookup(MultiresReshapeContext *reshape_context)
{
  const OffsetIndices faces = reshape_context->base_faces;

  reshape_context->face_start_grid_index.reinitialize(faces.size());
  int num_grids = 0;
  int num_ptex_faces = 0;
  for (const int face_index : faces.index_range()) {
    const int num_corners = faces[face_index].size();
    reshape_context->face_start_grid_index[face_index] = num_grids;
    num_grids += num_corners;
    num_ptex_faces += (num_corners == 4) ? 1 : num_corners;
  }

  reshape_context->grid_to_face_index.reinitialize(num_grids);
  reshape_context->ptex_start_grid_index.reinitialize(num_ptex_faces);
  for (int face_index = 0, grid_index = 0, ptex_index = 0; face_index < faces.size(); ++face_index)
  {
    const int num_corners = faces[face_index].size();
    const int num_face_ptex_faces = (num_corners == 4) ? 1 : num_corners;
    for (int i = 0; i < num_face_ptex_faces; ++i) {
      reshape_context->ptex_start_grid_index[ptex_index + i] = grid_index + i;
    }
    for (int corner = 0; corner < num_corners; ++corner, ++grid_index) {
      reshape_context->grid_to_face_index[grid_index] = face_index;
    }
    ptex_index += num_face_ptex_faces;
  }

  /* Store number of grids, which will be used for sanity checks. */
  reshape_context->num_grids = num_grids;
}

static void context_init_grid_pointers(MultiresReshapeContext *reshape_context)
{
  Mesh *base_mesh = reshape_context->base_mesh;
  reshape_context->mdisps = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&base_mesh->corner_data, CD_MDISPS, base_mesh->corners_num));
  reshape_context->grid_paint_masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
      &base_mesh->corner_data, CD_GRID_PAINT_MASK, base_mesh->corners_num));
}

static void context_init_common(MultiresReshapeContext *reshape_context)
{
  BLI_assert(reshape_context->subdiv != nullptr);
  BLI_assert(reshape_context->base_mesh != nullptr);

  reshape_context->face_ptex_offset = bke::subdiv::face_ptex_offset_get(reshape_context->subdiv);

  context_init_lookup(reshape_context);
  context_init_grid_pointers(reshape_context);
}

static bool context_is_valid(MultiresReshapeContext *reshape_context)
{
  if (reshape_context->mdisps == nullptr) {
    /* Multi-resolution displacement has been removed before current changes were applies. */
    return false;
  }
  return true;
}

static bool context_verify_or_free(MultiresReshapeContext *reshape_context)
{
  const bool is_valid = context_is_valid(reshape_context);
  if (!is_valid) {
    multires_reshape_context_free(reshape_context);
  }
  return is_valid;
}

bool multires_reshape_context_create_from_base_mesh(MultiresReshapeContext *reshape_context,
                                                    Depsgraph *depsgraph,
                                                    Object *object,
                                                    MultiresModifierData *mmd)
{
  context_zero(reshape_context);

  const bool use_render_params = false;
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Mesh *base_mesh = id_cast<Mesh *>(object->data);

  reshape_context->depsgraph = depsgraph;
  reshape_context->object = object;
  reshape_context->mmd = mmd;

  reshape_context->base_mesh = base_mesh;
  reshape_context->base_positions = base_mesh->vert_positions();
  reshape_context->base_edges = base_mesh->edges();
  reshape_context->base_faces = base_mesh->faces();
  reshape_context->base_corner_verts = base_mesh->corner_verts();
  reshape_context->base_corner_edges = base_mesh->corner_edges();

  reshape_context->subdiv = multires_reshape_create_subdiv(nullptr, object, mmd);
  if (!reshape_context->subdiv) {
    return false;
  }
  reshape_context->need_free_subdiv = true;

  reshape_context->reshape.level = multires_get_level(
      scene_eval, object, mmd, use_render_params, true);
  reshape_context->reshape.grid_size = bke::subdiv::grid_size_from_level(
      reshape_context->reshape.level);

  reshape_context->top.level = mmd->totlvl;
  reshape_context->top.grid_size = bke::subdiv::grid_size_from_level(reshape_context->top.level);

  context_init_common(reshape_context);

  return context_verify_or_free(reshape_context);
}

bool multires_reshape_context_create_from_object(MultiresReshapeContext *reshape_context,
                                                 Depsgraph *depsgraph,
                                                 Object *object,
                                                 MultiresModifierData *mmd)
{
  using namespace blender::bke;
  context_zero(reshape_context);

  const bool use_render_params = false;
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Mesh *base_mesh = id_cast<Mesh *>(object->data);

  reshape_context->depsgraph = depsgraph;
  reshape_context->object = object;
  reshape_context->mmd = mmd;

  reshape_context->base_mesh = base_mesh;
  reshape_context->base_positions = base_mesh->vert_positions();
  /* TODO: The following check can be replaced by ShapeKeyData struct member `basis_key_active`
   * found in `sculpt_intern.hh`.*/
  if (base_mesh->key && object->shapenr > 0) {
    KeyBlock *kb = base_mesh->key->refkey;
    reshape_context->basis_shape_key = kb;
  }
  reshape_context->base_edges = base_mesh->edges();
  reshape_context->base_faces = base_mesh->faces();
  reshape_context->base_corner_verts = base_mesh->corner_verts();
  reshape_context->base_corner_edges = base_mesh->corner_edges();

  reshape_context->subdiv = multires_reshape_create_subdiv(depsgraph, object, mmd);
  if (!reshape_context->subdiv) {
    return false;
  }
  reshape_context->need_free_subdiv = true;

  reshape_context->reshape.level = multires_get_level(
      scene_eval, object, mmd, use_render_params, true);
  reshape_context->reshape.grid_size = subdiv::grid_size_from_level(
      reshape_context->reshape.level);

  reshape_context->top.level = mmd->totlvl;
  reshape_context->top.grid_size = subdiv::grid_size_from_level(reshape_context->top.level);

  const bke::AttributeAccessor attributes = base_mesh->attributes();
  reshape_context->cd_vert_crease = *attributes.lookup<float>("crease_vert", AttrDomain::Point);
  reshape_context->cd_edge_crease = *attributes.lookup<float>("crease_edge", AttrDomain::Edge);

  context_init_common(reshape_context);

  return context_verify_or_free(reshape_context);
}

bool multires_reshape_context_create_from_ccg(MultiresReshapeContext *reshape_context,
                                              SubdivCCG *subdiv_ccg,
                                              Mesh *base_mesh,
                                              int top_level)
{
  context_zero(reshape_context);

  reshape_context->base_mesh = base_mesh;
  reshape_context->base_positions = base_mesh->vert_positions();
  reshape_context->base_edges = base_mesh->edges();
  reshape_context->base_faces = base_mesh->faces();
  reshape_context->base_corner_verts = base_mesh->corner_verts();
  reshape_context->base_corner_edges = base_mesh->corner_edges();

  reshape_context->subdiv = subdiv_ccg->subdiv;
  reshape_context->need_free_subdiv = false;

  reshape_context->reshape.level = subdiv_ccg->level;
  reshape_context->reshape.grid_size = bke::subdiv::grid_size_from_level(
      reshape_context->reshape.level);

  reshape_context->top.level = top_level;
  reshape_context->top.grid_size = bke::subdiv::grid_size_from_level(reshape_context->top.level);

  context_init_common(reshape_context);

  return context_verify_or_free(reshape_context);
}

bool multires_reshape_context_create_from_modifier(MultiresReshapeContext *reshape_context,
                                                   Object *object,
                                                   MultiresModifierData *mmd,
                                                   int top_level)
{
  bke::subdiv::Subdiv *subdiv = multires_reshape_create_subdiv(nullptr, object, mmd);

  const bool result = multires_reshape_context_create_from_subdiv(
      reshape_context, object, mmd, subdiv, top_level);

  reshape_context->need_free_subdiv = true;

  return result;
}

bool multires_reshape_context_create_from_subdiv(MultiresReshapeContext *reshape_context,
                                                 Object *object,
                                                 MultiresModifierData *mmd,
                                                 bke::subdiv::Subdiv *subdiv,
                                                 int top_level)
{
  using namespace blender::bke;
  context_zero(reshape_context);

  Mesh *base_mesh = id_cast<Mesh *>(object->data);

  reshape_context->mmd = mmd;
  reshape_context->base_mesh = base_mesh;
  reshape_context->base_positions = base_mesh->vert_positions();
  reshape_context->base_edges = base_mesh->edges();
  reshape_context->base_faces = base_mesh->faces();
  reshape_context->base_corner_verts = base_mesh->corner_verts();
  reshape_context->base_corner_edges = base_mesh->corner_edges();

  const bke::AttributeAccessor attributes = base_mesh->attributes();
  reshape_context->cd_vert_crease = *attributes.lookup<float>("crease_vert", AttrDomain::Point);

  reshape_context->subdiv = subdiv;
  reshape_context->need_free_subdiv = false;

  reshape_context->reshape.level = mmd->totlvl;
  reshape_context->reshape.grid_size = subdiv::grid_size_from_level(
      reshape_context->reshape.level);

  reshape_context->top.level = top_level;
  reshape_context->top.grid_size = subdiv::grid_size_from_level(reshape_context->top.level);

  context_init_common(reshape_context);

  return context_verify_or_free(reshape_context);
}

void multires_reshape_free_original_grids(MultiresReshapeContext *reshape_context)
{
  MDisps *orig_mdisps = reshape_context->orig.mdisps;
  GridPaintMask *orig_grid_paint_masks = reshape_context->orig.grid_paint_masks;

  if (orig_mdisps == nullptr && orig_grid_paint_masks == nullptr) {
    return;
  }

  const int num_grids = reshape_context->num_grids;
  for (int grid_index = 0; grid_index < num_grids; grid_index++) {
    if (orig_mdisps != nullptr) {
      MDisps *orig_grid = &orig_mdisps[grid_index];
      MEM_SAFE_DELETE(orig_grid->disps);
    }
    if (orig_grid_paint_masks != nullptr) {
      GridPaintMask *orig_paint_mask_grid = &orig_grid_paint_masks[grid_index];
      MEM_SAFE_DELETE(orig_paint_mask_grid->data);
    }
  }

  MEM_SAFE_DELETE(orig_mdisps);
  MEM_SAFE_DELETE(orig_grid_paint_masks);

  reshape_context->orig.mdisps = nullptr;
  reshape_context->orig.grid_paint_masks = nullptr;
}

void multires_reshape_context_free(MultiresReshapeContext *reshape_context)
{
  if (reshape_context->need_free_subdiv) {
    bke::subdiv::free(reshape_context->subdiv);
  }

  multires_reshape_free_original_grids(reshape_context);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper accessors
 * \{ */

int multires_reshape_grid_to_face_index(const MultiresReshapeContext *reshape_context,
                                        int grid_index)
{
  BLI_assert(grid_index >= 0);
  BLI_assert(grid_index < reshape_context->num_grids);

  /* TODO(sergey): Optimization: when SubdivCCG is known we can calculate face index using
   * SubdivCCG::grid_faces and SubdivCCG::faces, saving memory used by grid_to_face_index. */

  return reshape_context->grid_to_face_index[grid_index];
}

int multires_reshape_grid_to_corner(const MultiresReshapeContext *reshape_context, int grid_index)
{
  BLI_assert(grid_index >= 0);
  BLI_assert(grid_index < reshape_context->num_grids);

  /* TODO(sergey): Optimization: when SubdivCCG is known we can calculate face index using
   * SubdivCCG::grid_faces and SubdivCCG::faces, saving memory used by grid_to_face_index. */

  const int face_index = multires_reshape_grid_to_face_index(reshape_context, grid_index);
  return grid_index - reshape_context->face_start_grid_index[face_index];
}

bool multires_reshape_is_quad_face(const MultiresReshapeContext *reshape_context, int face_index)
{
  return reshape_context->base_faces[face_index].size() == 4;
}

int multires_reshape_grid_to_ptex_index(const MultiresReshapeContext *reshape_context,
                                        int grid_index)
{
  const int face_index = multires_reshape_grid_to_face_index(reshape_context, grid_index);
  const int corner = multires_reshape_grid_to_corner(reshape_context, grid_index);
  const bool is_quad = multires_reshape_is_quad_face(reshape_context, face_index);
  return reshape_context->face_ptex_offset[face_index] + (is_quad ? 0 : corner);
}

PTexCoord multires_reshape_grid_coord_to_ptex(const MultiresReshapeContext *reshape_context,
                                              const GridCoord *grid_coord)
{
  PTexCoord ptex_coord;

  ptex_coord.ptex_face_index = multires_reshape_grid_to_ptex_index(reshape_context,
                                                                   grid_coord->grid_index);

  float corner_u, corner_v;
  bke::subdiv::grid_uv_to_ptex_face_uv(grid_coord->u, grid_coord->v, &corner_u, &corner_v);

  const int face_index = multires_reshape_grid_to_face_index(reshape_context,
                                                             grid_coord->grid_index);
  const int corner = multires_reshape_grid_to_corner(reshape_context, grid_coord->grid_index);
  if (multires_reshape_is_quad_face(reshape_context, face_index)) {
    float grid_u, grid_v;
    bke::subdiv::ptex_face_uv_to_grid_uv(corner_u, corner_v, &grid_u, &grid_v);
    bke::subdiv::rotate_grid_to_quad(corner, grid_u, grid_v, &ptex_coord.u, &ptex_coord.v);
  }
  else {
    ptex_coord.u = corner_u;
    ptex_coord.v = corner_v;
  }

  return ptex_coord;
}

GridCoord multires_reshape_ptex_coord_to_grid(const MultiresReshapeContext *reshape_context,
                                              const PTexCoord *ptex_coord)
{
  GridCoord grid_coord;

  const int start_grid_index = reshape_context->ptex_start_grid_index[ptex_coord->ptex_face_index];
  const int face_index = reshape_context->grid_to_face_index[start_grid_index];

  int corner_delta;
  if (multires_reshape_is_quad_face(reshape_context, face_index)) {
    corner_delta = bke::subdiv::rotate_quad_to_corner(
        ptex_coord->u, ptex_coord->v, &grid_coord.u, &grid_coord.v);
  }
  else {
    corner_delta = 0;
    grid_coord.u = ptex_coord->u;
    grid_coord.v = ptex_coord->v;
  }
  grid_coord.grid_index = start_grid_index + corner_delta;

  bke::subdiv::ptex_face_uv_to_grid_uv(grid_coord.u, grid_coord.v, &grid_coord.u, &grid_coord.v);

  return grid_coord;
}

void multires_reshape_tangent_matrix_for_corner(const MultiresReshapeContext *reshape_context,
                                                const int face_index,
                                                const int corner,
                                                const float3 &dPdu,
                                                const float3 &dPdv,
                                                float3x3 &r_tangent_matrix)
{
  /* For a quad faces we would need to flip the tangent, since they will use
   * use different coordinates within displacement grid compared to the ptex face. */
  const bool is_quad = multires_reshape_is_quad_face(reshape_context, face_index);
  const int tangent_corner = is_quad ? corner : 0;
  BKE_multires_construct_tangent_matrix(r_tangent_matrix, dPdu, dPdv, tangent_corner);
}

void multires_reshape_tangent_matrix_for_corner_for_versioning(
    const MultiresReshapeContext *reshape_context,
    const int face_index,
    const int corner,
    const float3 &dPdu,
    const float3 &dPdv,
    float3x3 &r_tangent_matrix)
{
  /* For a quad faces we would need to flip the tangent, since they will use
   * use different coordinates within displacement grid compared to the ptex face. */
  const bool is_quad = multires_reshape_is_quad_face(reshape_context, face_index);
  const int tangent_corner = is_quad ? corner : 0;
  BKE_multires_construct_tangent_matrix_for_versioning(
      r_tangent_matrix, dPdu, dPdv, tangent_corner);
}

#if MULTIRES_TANGENT_DEBUG

/* One metric aggregated over a reshape pass. The fast path per sample is a couple of relaxed
 * atomic loads, so the diagnostics do not serialize the parallel reshape loops; the mutex is
 * only taken when a sample beats the current maximum. */
struct TangentDebugMetric {
  std::atomic<const char *> name{nullptr};
  std::atomic<float> max_gain{0.0f};
  std::atomic<int> count_over_2{0};
  std::atomic<int> count_over_10{0};
  std::atomic<int> count_over_100{0};
  std::atomic<int> count_non_finite{0};

  /* Snapshot of the worst sample, guarded by `snapshot_mutex`. */
  std::mutex snapshot_mutex;
  GridCoord coord = {0, 0.0f, 0.0f};
  float input_len = 0.0f;
  float len_x = 0.0f;
  float len_y = 0.0f;
  float angle = 0.0f;
  float det = 0.0f;
};

struct TangentDebugState {
  const char *label = nullptr;
  int reshape_level = 0;
  int top_level = 0;
  std::array<TangentDebugMetric, 4> metrics;
};

static TangentDebugState tangent_debug_state;

static TangentDebugMetric *tangent_debug_metric_get(const char *metric)
{
  for (TangentDebugMetric &slot : tangent_debug_state.metrics) {
    const char *slot_name = slot.name.load(std::memory_order_acquire);
    if (slot_name == metric) {
      return &slot;
    }
    if (slot_name == nullptr) {
      const char *expected = nullptr;
      if (slot.name.compare_exchange_strong(expected, metric, std::memory_order_acq_rel)) {
        return &slot;
      }
      if (expected == metric) {
        return &slot;
      }
    }
  }
  return nullptr;
}

void multires_reshape_tangent_debug_begin(const MultiresReshapeContext *reshape_context,
                                          const char *label)
{
  tangent_debug_state.label = label;
  tangent_debug_state.reshape_level = reshape_context->reshape.level;
  tangent_debug_state.top_level = reshape_context->top.level;
  for (TangentDebugMetric &slot : tangent_debug_state.metrics) {
    slot.name.store(nullptr, std::memory_order_relaxed);
    slot.max_gain.store(0.0f, std::memory_order_relaxed);
    slot.count_over_2.store(0, std::memory_order_relaxed);
    slot.count_over_10.store(0, std::memory_order_relaxed);
    slot.count_over_100.store(0, std::memory_order_relaxed);
    slot.count_non_finite.store(0, std::memory_order_relaxed);
  }
}

void multires_reshape_tangent_debug_gain(const char *metric,
                                         const GridCoord *grid_coord,
                                         const float3x3 &tangent_matrix,
                                         const float3 &input,
                                         const float3 &output)
{
  const bool output_is_finite = std::isfinite(output[0]) && std::isfinite(output[1]) &&
                                std::isfinite(output[2]);
  const float input_len = math::length(input);
  /* Gain of a near-zero input is dominated by float noise, not by the matrix conditioning. */
  if (output_is_finite && input_len <= 1e-6f) {
    return;
  }
  TangentDebugMetric *slot = tangent_debug_metric_get(metric);
  if (slot == nullptr) {
    return;
  }
  if (!output_is_finite) {
    slot->count_non_finite.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const float gain = math::length(output) / input_len;
  if (gain > 2.0f) {
    slot->count_over_2.fetch_add(1, std::memory_order_relaxed);
    if (gain > 10.0f) {
      slot->count_over_10.fetch_add(1, std::memory_order_relaxed);
    }
    if (gain > 100.0f) {
      slot->count_over_100.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (gain <= slot->max_gain.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard lock(slot->snapshot_mutex);
  if (gain <= slot->max_gain.load(std::memory_order_relaxed)) {
    return;
  }
  slot->max_gain.store(gain, std::memory_order_relaxed);
  slot->coord = *grid_coord;
  slot->input_len = input_len;
  slot->len_x = math::length(tangent_matrix.x_axis());
  slot->len_y = math::length(tangent_matrix.y_axis());
  const float len_prod = slot->len_x * slot->len_y;
  slot->angle = (len_prod > 0.0f) ?
                    RAD2DEGF(math::safe_acos(
                        math::dot(tangent_matrix.x_axis(), tangent_matrix.y_axis()) / len_prod)) :
                    0.0f;
  slot->det = math::determinant(tangent_matrix);
}

void multires_reshape_tangent_debug_end()
{
  for (TangentDebugMetric &slot : tangent_debug_state.metrics) {
    const char *name = slot.name.load(std::memory_order_acquire);
    if (name == nullptr) {
      continue;
    }
    printf(
        "[multires-tangent][%s L%d->%d] %s: max=%.4g at grid=%d uv=(%.4f, %.4f) |in|=%.4g "
        "|x|=%.4g |y|=%.4g angle=%.2f det=%.4g; >2:%d >10:%d >100:%d nonfinite:%d\n",
        tangent_debug_state.label,
        tangent_debug_state.reshape_level,
        tangent_debug_state.top_level,
        name,
        double(slot.max_gain.load(std::memory_order_relaxed)),
        slot.coord.grid_index,
        double(slot.coord.u),
        double(slot.coord.v),
        double(slot.input_len),
        double(slot.len_x),
        double(slot.len_y),
        double(slot.angle),
        double(slot.det),
        slot.count_over_2.load(std::memory_order_relaxed),
        slot.count_over_10.load(std::memory_order_relaxed),
        slot.count_over_100.load(std::memory_order_relaxed),
        slot.count_non_finite.load(std::memory_order_relaxed));
  }
}

#endif

ReshapeGridElement multires_reshape_grid_element_for_grid_coord(
    const MultiresReshapeContext *reshape_context, const GridCoord *grid_coord)
{
  ReshapeGridElement grid_element = {nullptr, nullptr};

  const int grid_size = reshape_context->top.grid_size;
  const int grid_x = lround(grid_coord->u * (grid_size - 1));
  const int grid_y = lround(grid_coord->v * (grid_size - 1));
  const int grid_element_index = grid_y * grid_size + grid_x;

  if (reshape_context->mdisps != nullptr) {
    MDisps *displacement_grid = &reshape_context->mdisps[grid_coord->grid_index];
    grid_element.displacement = reinterpret_cast<float3 *>(
        displacement_grid->disps[grid_element_index]);
  }

  if (reshape_context->grid_paint_masks != nullptr) {
    GridPaintMask *grid_paint_mask = &reshape_context->grid_paint_masks[grid_coord->grid_index];
    grid_element.mask = &grid_paint_mask->data[grid_element_index];
  }

  return grid_element;
}

ReshapeGridElement multires_reshape_grid_element_for_ptex_coord(
    const MultiresReshapeContext *reshape_context, const PTexCoord *ptex_coord)
{
  GridCoord grid_coord = multires_reshape_ptex_coord_to_grid(reshape_context, ptex_coord);
  return multires_reshape_grid_element_for_grid_coord(reshape_context, &grid_coord);
}

ReshapeConstGridElement multires_reshape_orig_grid_element_for_grid_coord(
    const MultiresReshapeContext *reshape_context, const GridCoord *grid_coord)
{
  ReshapeConstGridElement grid_element = {{0.0f, 0.0f, 0.0f}, 0.0f};

  const MDisps *mdisps = reshape_context->orig.mdisps;
  if (mdisps != nullptr) {
    const MDisps *displacement_grid = &mdisps[grid_coord->grid_index];
    if (displacement_grid->disps != nullptr) {
      const int grid_size = bke::subdiv::grid_size_from_level(displacement_grid->level);
      const int grid_x = lround(grid_coord->u * (grid_size - 1));
      const int grid_y = lround(grid_coord->v * (grid_size - 1));
      const int grid_element_index = grid_y * grid_size + grid_x;
      grid_element.displacement = displacement_grid->disps[grid_element_index];
    }
  }

  const GridPaintMask *grid_paint_masks = reshape_context->orig.grid_paint_masks;
  if (grid_paint_masks != nullptr) {
    const GridPaintMask *paint_mask_grid = &grid_paint_masks[grid_coord->grid_index];
    if (paint_mask_grid->data != nullptr) {
      const int grid_size = bke::subdiv::grid_size_from_level(paint_mask_grid->level);
      const int grid_x = lround(grid_coord->u * (grid_size - 1));
      const int grid_y = lround(grid_coord->v * (grid_size - 1));
      const int grid_element_index = grid_y * grid_size + grid_x;
      grid_element.mask = paint_mask_grid->data[grid_element_index];
    }
  }

  return grid_element;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sample limit surface of the base mesh
 * \{ */

void multires_reshape_evaluate_base_mesh_limit_at_grid(
    const MultiresReshapeContext *reshape_context,
    const GridCoord *grid_coord,
    float3 &r_P,
    float3x3 &r_tangent_matrix)
{
  float3 dPdu;
  float3 dPdv;
  const PTexCoord ptex_coord = multires_reshape_grid_coord_to_ptex(reshape_context, grid_coord);
  bke::subdiv::Subdiv *subdiv = reshape_context->subdiv;
  bke::subdiv::eval_limit_point_and_derivatives(
      subdiv, ptex_coord.ptex_face_index, ptex_coord.u, ptex_coord.v, r_P, dPdu, dPdv);

  const int face_index = multires_reshape_grid_to_face_index(reshape_context,
                                                             grid_coord->grid_index);
  const int corner = multires_reshape_grid_to_corner(reshape_context, grid_coord->grid_index);
  multires_reshape_tangent_matrix_for_corner(
      reshape_context, face_index, corner, dPdu, dPdv, r_tangent_matrix);
}

void multires_reshape_evaluate_base_mesh_limit_at_grid_for_versioning(
    const MultiresReshapeContext *reshape_context,
    const GridCoord *grid_coord,
    float3 &r_P,
    float3x3 &r_tangent_matrix)
{
  float3 dPdu;
  float3 dPdv;
  const PTexCoord ptex_coord = multires_reshape_grid_coord_to_ptex(reshape_context, grid_coord);
  bke::subdiv::Subdiv *subdiv = reshape_context->subdiv;
  bke::subdiv::eval_limit_point_and_derivatives(
      subdiv, ptex_coord.ptex_face_index, ptex_coord.u, ptex_coord.v, r_P, dPdu, dPdv);

  const int face_index = multires_reshape_grid_to_face_index(reshape_context,
                                                             grid_coord->grid_index);
  const int corner = multires_reshape_grid_to_corner(reshape_context, grid_coord->grid_index);
  multires_reshape_tangent_matrix_for_corner_for_versioning(
      reshape_context, face_index, corner, dPdu, dPdv, r_tangent_matrix);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Custom data preparation
 * \{ */

static void allocate_displacement_grid(MDisps *displacement_grid, const int level)
{
  const int grid_size = bke::subdiv::grid_size_from_level(level);
  const int grid_area = grid_size * grid_size;
  float (*disps)[3] = MEM_new_array_zeroed<float[3]>(grid_area, "multires disps");
  if (displacement_grid->disps != nullptr) {
    MEM_delete(displacement_grid->disps);
  }
  /* TODO(sergey): Preserve data on the old level. */
  displacement_grid->disps = disps;
  displacement_grid->totdisp = grid_area;
  displacement_grid->level = level;
}

static void ensure_displacement_grid(MDisps *displacement_grid, const int level)
{
  if (displacement_grid->disps != nullptr && displacement_grid->level >= level) {
    return;
  }
  allocate_displacement_grid(displacement_grid, level);
}

static void ensure_displacement_grids(Mesh *mesh, const int grid_level)
{
  const int num_grids = mesh->corners_num;
  MDisps *mdisps = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
  for (int grid_index = 0; grid_index < num_grids; grid_index++) {
    ensure_displacement_grid(&mdisps[grid_index], grid_level);
  }
}

static void ensure_mask_grids(Mesh *mesh, const int level)
{
  GridPaintMask *grid_paint_masks = static_cast<GridPaintMask *>(
      CustomData_get_layer_for_write(&mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
  if (grid_paint_masks == nullptr) {
    return;
  }
  const int num_grids = mesh->corners_num;
  const int grid_size = bke::subdiv::grid_size_from_level(level);
  const int grid_area = grid_size * grid_size;
  for (int grid_index = 0; grid_index < num_grids; grid_index++) {
    GridPaintMask *grid_paint_mask = &grid_paint_masks[grid_index];
    if (grid_paint_mask->level >= level) {
      continue;
    }
    grid_paint_mask->level = level;
    if (grid_paint_mask->data) {
      MEM_delete(grid_paint_mask->data);
    }
    /* TODO(sergey): Preserve data on the old level. */
    grid_paint_mask->data = MEM_new_array_zeroed<float>(grid_area, "gpm.data");
  }
}

void multires_reshape_ensure_grids(Mesh *mesh, const int level)
{
  ensure_displacement_grids(mesh, level);
  ensure_mask_grids(mesh, level);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Displacement, space conversion
 * \{ */

void multires_reshape_store_original_grids(MultiresReshapeContext *reshape_context,
                                           const Span<bool> grid_enabled)
{
  const MDisps *mdisps = reshape_context->mdisps;
  const GridPaintMask *grid_paint_masks = reshape_context->grid_paint_masks;

  MDisps *orig_mdisps = MEM_dupalloc(mdisps);
  GridPaintMask *orig_grid_paint_masks = nullptr;
  if (grid_paint_masks != nullptr) {
    orig_grid_paint_masks = MEM_dupalloc(grid_paint_masks);
  }

  const int num_grids = reshape_context->num_grids;
  for (int grid_index = 0; grid_index < num_grids; grid_index++) {
    /* When a per-grid mask is given (a restricted, touched-grid reshape), only the enabled grids
     * are read back. The array copy above aliased every grid's #disps pointer to the live data, so
     * for the disabled grids drop that pointer to nullptr: this skips the expensive per-grid copy
     * and stops the free path from double-freeing the live grid (#MEM_SAFE_DELETE ignores null). */
    const bool enabled = grid_enabled.is_empty() || grid_enabled[grid_index];
    MDisps *orig_grid = &orig_mdisps[grid_index];
    /* Ignore possibly invalid/non-allocated original grids. They will be replaced with 0 original
     * data when accessed during reshape process.
     * Reshape process will ensure all grids are on top level, but that happens on separate set of
     * grids which eventually replaces original one. */
    orig_grid->disps = (enabled && orig_grid->disps != nullptr) ? MEM_dupalloc(orig_grid->disps) :
                                                                  nullptr;
    if (orig_grid_paint_masks != nullptr) {
      GridPaintMask *orig_paint_mask_grid = &orig_grid_paint_masks[grid_index];
      orig_paint_mask_grid->data = (enabled && orig_paint_mask_grid->data != nullptr) ?
                                       MEM_dupalloc(orig_paint_mask_grid->data) :
                                       nullptr;
    }
  }

  reshape_context->orig.mdisps = orig_mdisps;
  reshape_context->orig.grid_paint_masks = orig_grid_paint_masks;
}

using ForeachGridCoordinateCallback = void (*)(const MultiresReshapeContext *reshape_context,
                                               const GridCoord *grid_coord,
                                               void *userdata_v);

struct ForeachGridCoordinateTaskData {
  const MultiresReshapeContext *reshape_context;

  int grid_size;
  float grid_size_1_inv;

  /* When non-null, a per-grid enable mask (indexed by grid index): grids whose entry is false are
   * skipped entirely. Used to restrict a reshape to the grids a sculpt stroke actually touched.
   * Null means every grid is processed (the default for full-mesh reshapes). */
  const bool *grid_enabled;

  ForeachGridCoordinateCallback callback;
  void *callback_userdata_v;
};

static void foreach_grid_face_coordinate_task(void *__restrict userdata_v,
                                              const int face_index,
                                              const TaskParallelTLS *__restrict /*tls*/)
{
  ForeachGridCoordinateTaskData *data = static_cast<ForeachGridCoordinateTaskData *>(userdata_v);

  const MultiresReshapeContext *reshape_context = data->reshape_context;

  const OffsetIndices faces = reshape_context->base_faces;
  const int grid_size = data->grid_size;
  const float grid_size_1_inv = 1.0f / (float(grid_size) - 1.0f);

  const int num_corners = faces[face_index].size();
  int grid_index = reshape_context->face_start_grid_index[face_index];
  for (int corner = 0; corner < num_corners; ++corner, ++grid_index) {
    if (data->grid_enabled != nullptr && !data->grid_enabled[grid_index]) {
      continue;
    }
    for (int y = 0; y < grid_size; ++y) {
      const float v = float(y) * grid_size_1_inv;
      for (int x = 0; x < grid_size; ++x) {
        const float u = float(x) * grid_size_1_inv;

        GridCoord grid_coord;
        grid_coord.grid_index = grid_index;
        grid_coord.u = u;
        grid_coord.v = v;

        data->callback(data->reshape_context, &grid_coord, data->callback_userdata_v);
      }
    }
  }
}

/* Run given callback for every grid coordinate at a given level. */
static void foreach_grid_coordinate(const MultiresReshapeContext *reshape_context,
                                    const int level,
                                    ForeachGridCoordinateCallback callback,
                                    void *userdata_v,
                                    const Span<bool> grid_enabled = {})
{
  ForeachGridCoordinateTaskData data;
  data.reshape_context = reshape_context;
  data.grid_size = bke::subdiv::grid_size_from_level(level);
  data.grid_size_1_inv = 1.0f / (float(data.grid_size) - 1.0f);
  data.grid_enabled = grid_enabled.is_empty() ? nullptr : grid_enabled.data();
  data.callback = callback;
  data.callback_userdata_v = userdata_v;

  TaskParallelSettings parallel_range_settings;
  BLI_parallel_range_settings_defaults(&parallel_range_settings);
  parallel_range_settings.min_iter_per_thread = 1;

  const Mesh *base_mesh = reshape_context->base_mesh;
  const int num_faces = base_mesh->faces_num;
  BLI_task_parallel_range(
      0, num_faces, &data, foreach_grid_face_coordinate_task, &parallel_range_settings);
}

static void object_grid_element_to_tangent_displacement(
    const MultiresReshapeContext *reshape_context,
    const GridCoord *grid_coord,
    void * /*userdata_v*/)
{
  float3 P;
  float3x3 tangent_matrix;
  multires_reshape_evaluate_base_mesh_limit_at_grid(
      reshape_context, grid_coord, P, tangent_matrix);

  const float3x3 inv_tangent_matrix = math::invert(tangent_matrix);

  ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(reshape_context,
                                                                                 grid_coord);

  float3 D = *grid_element.displacement - P;

  float3 tangent_D = math::transform_direction(inv_tangent_matrix, D);

#if MULTIRES_TANGENT_DEBUG
  multires_reshape_tangent_debug_gain("gain", grid_coord, tangent_matrix, D, tangent_D);
  /* Decoding the just-encoded displacement must reproduce the object-space delta; divergence
   * beyond float noise means the frame is too ill-conditioned to store data through it. */
  const float3 D_roundtrip = math::transform_direction(tangent_matrix, tangent_D);
  multires_reshape_tangent_debug_gain(
      "roundtrip-rel", grid_coord, tangent_matrix, D, D_roundtrip - D);
#endif

  *grid_element.displacement = tangent_D;
}

void multires_reshape_object_grids_to_tangent_displacement(
    const MultiresReshapeContext *reshape_context, const Span<bool> grid_enabled)
{
#if MULTIRES_TANGENT_DEBUG
  multires_reshape_tangent_debug_begin(reshape_context, "encode");
#endif
  foreach_grid_coordinate(reshape_context,
                          reshape_context->top.level,
                          object_grid_element_to_tangent_displacement,
                          nullptr,
                          grid_enabled);
#if MULTIRES_TANGENT_DEBUG
  multires_reshape_tangent_debug_end();
#endif
}

void multires_reshape_object_grids_to_tangent_displacement_for_grids(
    const MultiresReshapeContext *reshape_context,
    const Span<int> grid_indices,
    const MutableSpan<SubdivCCGMultiresLayerFrames> frame_cache,
    const Span<bool> populate_grids)
{
  /* Use the same top-level grid size the rest of the reshape uses for #grid_area, so the element
   * iteration matches the stored MDisps layout exactly. */
  const int grid_size = reshape_context->top.grid_size;
  const float grid_size_1_inv = 1.0f / (float(grid_size) - 1.0f);
  const int64_t grid_area = int64_t(grid_size) * grid_size;
  const bool use_cache = !frame_cache.is_empty();
  /* Encode only the listed grids. The tangent encode of a grid element depends on the base-mesh
   * limit surface at that element alone (no cross-grid dependency), so restricting the set is
   * exact. Used by sculpt-layer recording to encode just the grids a stroke touched. When a frame
   * cache is provided, reuse (or lazily populate) the per-element limit position and inverse
   * tangent matrix so repeated strokes skip the expensive limit-surface evaluation. */
  threading::parallel_for(grid_indices.index_range(), 1, [&](const IndexRange range) {
    for (const int64_t i : range) {
      const int grid_index = grid_indices[i];
      SubdivCCGMultiresLayerFrames *frames = use_cache ? &frame_cache[grid_index] : nullptr;
      const bool cached = frames != nullptr && !frames->positions.is_empty();
      const bool populate = frames != nullptr && !cached && !populate_grids.is_empty() &&
                            populate_grids[grid_index];
      if (populate) {
        frames->positions.reinitialize(grid_area);
        frames->inv_tangent_matrices.reinitialize(grid_area);
      }
      for (int y = 0; y < grid_size; ++y) {
        const float v = float(y) * grid_size_1_inv;
        for (int x = 0; x < grid_size; ++x) {
          const int64_t elem = int64_t(y) * grid_size + x;
          GridCoord grid_coord;
          grid_coord.grid_index = grid_index;
          grid_coord.u = float(x) * grid_size_1_inv;
          grid_coord.v = v;

          float3 P;
          float3x3 inv_tangent_matrix;
          if (cached) {
            P = frames->positions[elem];
            inv_tangent_matrix = frames->inv_tangent_matrices[elem];
          }
          else {
            float3x3 tangent_matrix;
            multires_reshape_evaluate_base_mesh_limit_at_grid(
                reshape_context, &grid_coord, P, tangent_matrix);
            inv_tangent_matrix = math::invert(tangent_matrix);
            if (populate) {
              frames->positions[elem] = P;
              frames->inv_tangent_matrices[elem] = inv_tangent_matrix;
            }
          }

          ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(
              reshape_context, &grid_coord);
          const float3 D = *grid_element.displacement - P;
          *grid_element.displacement = math::transform_direction(inv_tangent_matrix, D);
        }
      }
    }
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name MDISPS
 * \{ */

/* TODO(sergey): Make foreach_grid_coordinate more accessible and move this functionality to
 * its own file. */

static void assign_final_coords_from_mdisps(const MultiresReshapeContext *reshape_context,
                                            const GridCoord *grid_coord,
                                            void * /*userdata_v*/)
{
  float3 P;
  float3x3 tangent_matrix;
  multires_reshape_evaluate_base_mesh_limit_at_grid(
      reshape_context, grid_coord, P, tangent_matrix);

  ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(reshape_context,
                                                                                 grid_coord);
  const float3 D = math::transform_direction(tangent_matrix, *grid_element.displacement);

  *grid_element.displacement = P + D;
}

void multires_reshape_assign_final_coords_from_mdisps(
    const MultiresReshapeContext *reshape_context)
{
  foreach_grid_coordinate(
      reshape_context, reshape_context->top.level, assign_final_coords_from_mdisps, nullptr);
}

static void assign_final_coords_from_mdisps_for_versioning(
    const MultiresReshapeContext *reshape_context,
    const GridCoord *grid_coord,
    void * /*userdata_v*/)
{
  float3 P;
  float3x3 tangent_matrix;
  multires_reshape_evaluate_base_mesh_limit_at_grid_for_versioning(
      reshape_context, grid_coord, P, tangent_matrix);

  ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(reshape_context,
                                                                                 grid_coord);
  const float3 D = math::transform_direction(tangent_matrix, *grid_element.displacement);

  *grid_element.displacement = P + D;
}

void multires_reshape_assign_final_coords_from_mdisps_for_versioning(
    const MultiresReshapeContext *reshape_context)
{
  foreach_grid_coordinate(reshape_context,
                          reshape_context->top.level,
                          assign_final_coords_from_mdisps_for_versioning,
                          nullptr);
}

static void assign_final_elements_from_orig_mdisps(const MultiresReshapeContext *reshape_context,
                                                   const GridCoord *grid_coord,
                                                   void * /*userdata_v*/)
{
  float3 P;
  float3x3 tangent_matrix;
  multires_reshape_evaluate_base_mesh_limit_at_grid(
      reshape_context, grid_coord, P, tangent_matrix);

  const ReshapeConstGridElement orig_grid_element =
      multires_reshape_orig_grid_element_for_grid_coord(reshape_context, grid_coord);

  float3 D = math::transform_direction(tangent_matrix, orig_grid_element.displacement);

  ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(reshape_context,
                                                                                 grid_coord);
  *grid_element.displacement = P + D;

  if (grid_element.mask != nullptr) {
    *grid_element.mask = orig_grid_element.mask;
  }
}

void multires_reshape_assign_final_elements_from_orig_mdisps(
    const MultiresReshapeContext *reshape_context)
{
  foreach_grid_coordinate(reshape_context,
                          reshape_context->top.level,
                          assign_final_elements_from_orig_mdisps,
                          nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt layer object-space contribution
 * \{ */

bool BKE_multires_sculpt_layer_object_contribution(Mesh &base_mesh,
                                                   SubdivCCG &subdiv_ccg,
                                                   const SculptLayer &layer,
                                                   MutableSpan<float3> r_contrib)
{
  if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr || layer.level <= 0) {
    return false;
  }
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, &subdiv_ccg, &base_mesh, layer.level))
  {
    return false;
  }

  const int grids_num = reshape_context.num_grids;
  const int top_grid_size = bke::subdiv::grid_size_from_level(layer.level);
  const int cur_level = subdiv_ccg.level;
  const int cur_grid_size = bke::subdiv::grid_size_from_level(cur_level);
  const int cur_grid_area = cur_grid_size * cur_grid_size;
  if (int64_t(layer.totelem) != int64_t(grids_num) * top_grid_size * top_grid_size ||
      r_contrib.size() != int64_t(grids_num) * cur_grid_area)
  {
    multires_reshape_context_free(&reshape_context);
    return false;
  }

  /* A CCG finer than the layer's storage level cannot be served: #grid_subsample hands back the
   * source unchanged when asked to *up*sample, which is shorter than the buffer indexed below. */
  if (cur_level > layer.level) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }

  /* Subsample the tangent coefficients from the storage level down to the current CCG level
   * (exact at the coarse grid points), then transform each coefficient into object space with
   * the limit-surface tangent matrix — the same frames the displacement evaluator uses. */
  const Span<float3> layer_data(static_cast<const float3 *>(layer.data), layer.totelem);
  const Array<float3> subsampled = bke::grid_subsample(
      layer_data, layer.level, cur_level, grids_num);

  /* The mask lives on the layer's own (top-level) grid domain, while this loop walks the CCG's
   * current level. #grid_subsample maps a current-level point `(x, y)` to the top-level point
   * `(x * step, y * step)` — an exact index stride, because CCG grids are nested. Sampling the
   * mask through that *same* stride is what makes this direction symmetric with the forward one:
   * the weight applied to a coefficient here is read from the very element the coefficient itself
   * was read from, so the contribution subtracted on flush is exactly the contribution the
   * displacement evaluator composed. At the top level `step` is 1 and this degenerates to the
   * forward path's own `y * grid_size + x`. Any other sampling (interpolating the mask, or
   * re-cutting it at the CCG level) would leave a residual the base absorbs on every flush. */
  const int step = (top_grid_size - 1) / (cur_grid_size - 1);
  const bke::sculpt_layers::CompositeMask masks = bke::sculpt_layers::grid_masks_for_composite(
      layer, layer.totelem, top_grid_size * top_grid_size);

  threading::parallel_for(IndexRange(grids_num), 8, [&](const IndexRange range) {
    for (const int grid_index : range) {
      const int64_t grid_start = int64_t(grid_index) * cur_grid_area;
      /* One grid is one mask block, so this folds once per grid. The contribution is defined at
       * influence 1.0 — callers scale it — so only the mask is folded in here.
       *
       * That fixed 1.0 makes this the one direction whose association order differs from the
       * forward one: #subtract_sculpt_layers_from_ccg_positions multiplies the result by the
       * layer's effective influence afterwards, giving `(m * 1) * eff`, where the evaluator's
       * collector folds the influence into the same expression as `m * eff`. The difference is a
       * single rounding step (~1 ulp) and is forced: the influence-drag caller needs this buffer at
       * influence 1.0 so it can rescale it per tick without recomputing it. It is also dominated by
       * the residual the surrounding transform already carries — the tangent frames are evaluated
       * from the limit surface and subsampled per level, so the coefficient this weight multiplies
       * is itself not bit-identical to the one the displacement evaluator saw. Any exact-cancellation
       * claim belongs to the mask fold alone, not to this whole product. */
      const bke::sculpt_layers::MaskBlockWeight weight = bke::sculpt_layers::mask_block_weight(
          masks, grid_index, 1.0f);
      /* Invariant over the grid, so it is answered once here rather than per element. */
      if (weight.skip) {
        r_contrib.slice(grid_start, cur_grid_area).fill(float3(0.0f));
        continue;
      }
      for (int y = 0; y < cur_grid_size; y++) {
        for (int x = 0; x < cur_grid_size; x++) {
          const int64_t elem = grid_start + int64_t(y) * cur_grid_size + x;
          GridCoord grid_coord;
          grid_coord.grid_index = grid_index;
          grid_coord.u = float(x) / float(cur_grid_size - 1);
          grid_coord.v = float(y) / float(cur_grid_size - 1);
          float3 P;
          float3x3 tangent_matrix;
          multires_reshape_evaluate_base_mesh_limit_at_grid(
              &reshape_context, &grid_coord, P, tangent_matrix);
          const int mask_local = (y * step) * top_grid_size + x * step;
          r_contrib[elem] = math::transform_direction(tangent_matrix, subsampled[elem]) *
                            bke::sculpt_layers::mask_elem_weight(weight, mask_local);
        }
      }
    }
  });

  multires_reshape_context_free(&reshape_context);
  return true;
}

/** \} */

}  // namespace blender
