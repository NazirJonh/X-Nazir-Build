/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief VBD (Vertex Block Descent) solver implementation for GPU-accelerated cloth brush.
 */

#include "sculpt_cloth_vbd.hh"
#include "sculpt_cloth.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_utildefines.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_uniform_buffer.hh"

namespace blender::ed::sculpt_paint::cloth::vbd {

/* Constants */
static constexpr int THREADS_PER_GROUP = 16;
static constexpr int DEFAULT_ITERATIONS = 20;
static constexpr float DEFAULT_TIME_STEP = 1.0f / 60.0f;

/* --------------------------------------------------------
 * Initialization
 * -------------------------------------------------------- */

void VBDSolver::init(int vertex_count)
{
  vertices_.vertex_count = vertex_count;

  vertices_.positions.reinitialize(vertex_count);
  vertices_.prev_positions.reinitialize(vertex_count);
  vertices_.inertial_positions.reinitialize(vertex_count);
  vertices_.velocities.reinitialize(vertex_count);
  vertices_.accelerations.reinitialize(vertex_count);
  vertices_.masses.reinitialize(vertex_count);
  vertices_.masses_inv.reinitialize(vertex_count);
  vertices_.colors.reinitialize(vertex_count);
  vertices_.constraint_factors.reinitialize(vertex_count);
  vertices_.constraint_tweak.reinitialize(vertex_count);
  vertices_.deformation_pos.reinitialize(vertex_count);
  vertices_.deformation_strength.reinitialize(vertex_count);
  vertices_.softbody_pos.reinitialize(vertex_count);

  gpu_.is_initialized = false;
  gpu_.needs_upload = true;
}

/* --------------------------------------------------------
 * Data Transfer from SimulationData
 * -------------------------------------------------------- */

void VBDSolver::upload_from_simulation_data(const SimulationData &sim)
{
  const int num_verts = vertices_.vertex_count;

  /* Copy positions */
  for (const int i : IndexRange(num_verts)) {
    if (i < sim.pos.size()) {
      vertices_.positions[i] = sim.pos[i];
      vertices_.prev_positions[i] = sim.prev_pos[i];
      vertices_.softbody_pos[i] = sim.softbody_pos[i];
      vertices_.deformation_pos[i] = sim.deformation_pos[i];
      vertices_.deformation_strength[i] = sim.deformation_strength[i];
      vertices_.constraint_tweak[i] = sim.length_constraint_tweak[i];
    }
  }

  /* Initialize masses */
  const float mass = sim.mass;
  vertices_.masses.fill(mass);
  for (const int i : IndexRange(num_verts)) {
    vertices_.masses_inv[i] = (mass > 0.0f) ? (1.0f / mass) : 1.0f;
  }

  /* Build constraints from length_constraints */
  constraints_.spring_count = sim.length_constraints.size();
  constraints_.spring_v1.reinitialize(constraints_.spring_count);
  constraints_.spring_v2.reinitialize(constraints_.spring_count);
  constraints_.spring_rest_length.reinitialize(constraints_.spring_count);
  constraints_.spring_stiffness.reinitialize(constraints_.spring_count);
  constraints_.spring_node_index.reinitialize(constraints_.spring_count);

  for (const int i : sim.length_constraints.index_range()) {
    const LengthConstraint &lc = sim.length_constraints[i];
    constraints_.spring_v1[i] = lc.elem_index_a;
    constraints_.spring_v2[i] = lc.elem_index_b;
    constraints_.spring_rest_length[i] = lc.length;
    constraints_.spring_stiffness[i] = lc.strength;
    constraints_.spring_node_index[i] = lc.node;
  }

  /* Build adjacency list */
  this->build_adjacency_from_springs();

  /* Compute vertex colors */
  compute_vertex_colors(constraints_, vertices_.vertex_count);

  /* Copy node states */
  constraints_.node_states = sim.node_state;

  /* Initialize constraint factors to 1.0 */
  vertices_.constraint_factors.fill(1.0f);

  gpu_.needs_upload = true;
}

void VBDSolver::build_adjacency_from_springs()
{
  const int N = vertices_.vertex_count;

  /* Count neighbors per vertex */
  Array<int> neighbor_counts(N, 0);
  for (const int i : IndexRange(constraints_.spring_count)) {
    neighbor_counts[constraints_.spring_v1[i]]++;
    if (constraints_.spring_v1[i] != constraints_.spring_v2[i]) {
      neighbor_counts[constraints_.spring_v2[i]]++;
    }
  }

  /* Build offsets */
  constraints_.adj_offsets.reinitialize(N + 1);
  constraints_.adj_offsets[0] = 0;
  for (const int i : IndexRange(N)) {
    constraints_.adj_offsets[i + 1] = constraints_.adj_offsets[i] + neighbor_counts[i];
  }

  /* Build adjacency list */
  const int total_adj = constraints_.adj_offsets.last();
  constraints_.adj_list.reinitialize(total_adj);
  constraints_.adj_spring_idx.reinitialize(total_adj);

  Array<int> current_offset(N, 0);
  for (const int spring_idx : IndexRange(constraints_.spring_count)) {
    const int v1 = constraints_.spring_v1[spring_idx];
    const int v2 = constraints_.spring_v2[spring_idx];

    int adj_idx = constraints_.adj_offsets[v1] + current_offset[v1];
    constraints_.adj_list[adj_idx] = v2;
    constraints_.adj_spring_idx[adj_idx] = spring_idx;
    current_offset[v1]++;

    if (v1 != v2) {
      adj_idx = constraints_.adj_offsets[v2] + current_offset[v2];
      constraints_.adj_list[adj_idx] = v1;
      constraints_.adj_spring_idx[adj_idx] = spring_idx;
      current_offset[v2]++;
    }
  }
}

void VBDSolver::download_to_simulation_data(SimulationData &sim)
{
  const int num_verts = vertices_.vertex_count;
  for (const int i : IndexRange(num_verts)) {
    if (i < sim.pos.size()) {
      sim.pos[i] = vertices_.positions[i];
      sim.prev_pos[i] = vertices_.prev_positions[i];
    }
  }
}

/* --------------------------------------------------------
 * GPU Buffer Management
 * -------------------------------------------------------- */

void VBDSolver::ensure_gpu_buffers()
{
  if (gpu_.is_initialized) {
    return;
  }

  const int N = vertices_.vertex_count;
  const int M = constraints_.spring_count;
  const int adj_size = constraints_.adj_list.size();

  /* Create vertex SSBOs */
  gpu_.positions_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float4), nullptr, GPU_USAGE_DYNAMIC, "VBD Positions");
  gpu_.prev_positions_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float4), nullptr, GPU_USAGE_DYNAMIC, "VBD Prev Positions");
  gpu_.y_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float4), nullptr, GPU_USAGE_DYNAMIC, "VBD Y Positions");
  gpu_.accelerations_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float4), nullptr, GPU_USAGE_DYNAMIC, "VBD Accelerations");
  gpu_.masses_inv_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float), nullptr, GPU_USAGE_STATIC, "VBD Masses Inv");
  gpu_.colors_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(int), nullptr, GPU_USAGE_STATIC, "VBD Colors");
  gpu_.factors_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float), nullptr, GPU_USAGE_DYNAMIC, "VBD Factors");

  /* Create spring/constraint SSBOs */
  gpu_.springs_ssbo = GPU_storagebuf_create_ex(
      M * sizeof(float4), nullptr, GPU_USAGE_STATIC, "VBD Springs");
  gpu_.adj_offsets_ssbo = GPU_storagebuf_create_ex(
      (N + 1) * sizeof(int), nullptr, GPU_USAGE_STATIC, "VBD Adj Offsets");
  gpu_.adj_list_ssbo = GPU_storagebuf_create_ex(
      adj_size * sizeof(int), nullptr, GPU_USAGE_STATIC, "VBD Adj List");
  gpu_.adj_spring_idx_ssbo = GPU_storagebuf_create_ex(
      adj_size * sizeof(int), nullptr, GPU_USAGE_STATIC, "VBD Adj Spring Idx");
  gpu_.node_states_ssbo = GPU_storagebuf_create_ex(
      constraints_.node_states.size() * sizeof(int), nullptr, GPU_USAGE_DYNAMIC, "VBD Node States");

  /* Create auxiliary buffers */
  gpu_.new_positions_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float4), nullptr, GPU_USAGE_DYNAMIC, "VBD New Positions");
  gpu_.deformation_pos_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float4), nullptr, GPU_USAGE_DYNAMIC, "VBD Deformation Pos");
  gpu_.deformation_strength_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float), nullptr, GPU_USAGE_DYNAMIC, "VBD Deformation Strength");
  gpu_.softbody_pos_ssbo = GPU_storagebuf_create_ex(
      N * sizeof(float4), nullptr, GPU_USAGE_DYNAMIC, "VBD Softbody Pos");

  /* Load shaders */
  shader_init_ = GPU_shader_create_from_info_name("sculpt_cloth_vbd_init");
  shader_solve_ = GPU_shader_create_from_info_name("sculpt_cloth_vbd_solve");
  shader_integrate_ = GPU_shader_create_from_info_name("sculpt_cloth_vbd_integrate");

  gpu_.is_initialized = true;
}

void VBDSolver::upload_to_gpu()
{
  if (!gpu_.needs_upload) {
    return;
  }

  ensure_gpu_buffers();

  const int N = vertices_.vertex_count;
  const int M = constraints_.spring_count;

  /* Convert float3 arrays to float4 for GPU */
  Array<float4> positions_4d(N);
  Array<float4> prev_positions_4d(N);
  Array<float4> y_positions_4d(N);
  Array<float4> accelerations_4d(N);
  Array<float4> deformation_pos_4d(N);
  Array<float4> softbody_pos_4d(N);

  for (const int i : IndexRange(N)) {
    positions_4d[i] = float4(vertices_.positions[i], 0.0f);
    prev_positions_4d[i] = float4(vertices_.prev_positions[i], 0.0f);
    y_positions_4d[i] = float4(vertices_.inertial_positions[i], 0.0f);
    accelerations_4d[i] = float4(vertices_.accelerations[i], 0.0f);
    deformation_pos_4d[i] = float4(vertices_.deformation_pos[i], 0.0f);
    softbody_pos_4d[i] = float4(vertices_.softbody_pos[i], 0.0f);
  }

  /* Upload vertex data */
  GPU_storagebuf_update(gpu_.positions_ssbo, positions_4d.data());
  GPU_storagebuf_update(gpu_.prev_positions_ssbo, prev_positions_4d.data());
  GPU_storagebuf_update(gpu_.y_ssbo, y_positions_4d.data());
  GPU_storagebuf_update(gpu_.accelerations_ssbo, accelerations_4d.data());
  GPU_storagebuf_update(gpu_.masses_inv_ssbo, vertices_.masses_inv.data());
  GPU_storagebuf_update(gpu_.colors_ssbo, constraints_.colors.data());
  GPU_storagebuf_update(gpu_.factors_ssbo, vertices_.constraint_factors.data());
  GPU_storagebuf_update(gpu_.deformation_pos_ssbo, deformation_pos_4d.data());
  GPU_storagebuf_update(gpu_.deformation_strength_ssbo, vertices_.deformation_strength.data());
  GPU_storagebuf_update(gpu_.softbody_pos_ssbo, softbody_pos_4d.data());

  /* Pack and upload spring data */
  Array<float4> springs_packed(M);
  for (const int i : IndexRange(M)) {
    springs_packed[i] = float4(float(constraints_.spring_v1[i]),
                                float(constraints_.spring_v2[i]),
                                constraints_.spring_rest_length[i],
                                constraints_.spring_stiffness[i]);
  }
  GPU_storagebuf_update(gpu_.springs_ssbo, springs_packed.data());

  /* Upload adjacency data */
  GPU_storagebuf_update(gpu_.adj_offsets_ssbo, constraints_.adj_offsets.data());
  GPU_storagebuf_update(gpu_.adj_list_ssbo, constraints_.adj_list.data());
  GPU_storagebuf_update(gpu_.adj_spring_idx_ssbo, constraints_.adj_spring_idx.data());

  /* Convert NodeSimState to int for GPU */
  Array<int> node_states_int(constraints_.node_states.size());
  for (const int i : constraints_.node_states.index_range()) {
    node_states_int[i] = static_cast<int>(constraints_.node_states[i]);
  }
  GPU_storagebuf_update(gpu_.node_states_ssbo, node_states_int.data());

  gpu_.needs_upload = false;
}

void VBDSolver::download_from_gpu()
{
  const int N = vertices_.vertex_count;
  Array<float4> positions_4d(N);

  GPU_storagebuf_sync_to_host(gpu_.positions_ssbo);
  GPU_storagebuf_read(gpu_.positions_ssbo, positions_4d.data());

  for (const int i : IndexRange(N)) {
    vertices_.positions[i] = positions_4d[i].xyz();
  }
}

/* --------------------------------------------------------
 * Simulation Step
 * -------------------------------------------------------- */

void VBDSolver::step(const VBDParams &params)
{
  ensure_gpu_buffers();
  upload_to_gpu();

  const int N = params.total_vertices;
  const int groups_x = (N + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP;

  /* Stage 1: Initialize inertial positions */
  GPU_shader_bind(shader_init_);

  /* Set push constants */
  GPU_shader_uniform_1i(shader_init_, "total_vertices", N);
  GPU_shader_uniform_1f(shader_init_, "time_step", params.time_step);
  GPU_shader_uniform_1f(shader_init_, "time_step_inv", params.time_step_inv);
  GPU_shader_uniform_4fv(shader_init_, "gravity", &params.gravity.x);

  /* Bind SSBOs for init shader */
  GPU_storagebuf_bind(gpu_.positions_ssbo, 0);
  GPU_storagebuf_bind(gpu_.prev_positions_ssbo, 1);
  GPU_storagebuf_bind(gpu_.y_ssbo, 2);
  GPU_storagebuf_bind(gpu_.accelerations_ssbo, 3);

  GPU_compute_dispatch(shader_init_, groups_x, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Stage 2: VBD iterations */
  for (int iter = 0; iter < params.num_iterations; iter++) {
    /* Process each color */
    for (int c = 0; c <= params.max_color; c++) {
      GPU_shader_bind(shader_solve_);

      /* Set push constants */
      GPU_shader_uniform_1i(shader_solve_, "total_vertices", N);
      GPU_shader_uniform_1f(shader_solve_, "time_step_sq_inv", params.time_step_sq_inv);
      GPU_shader_uniform_1f(shader_solve_, "solver_factor", params.solver_factor);
      GPU_shader_uniform_1f(shader_solve_, "collision_stiffness", params.collision_stiffness);
      GPU_shader_uniform_1i(shader_solve_, "current_color", c);

      /* Bind SSBOs for solve shader */
      GPU_storagebuf_bind(gpu_.positions_ssbo, 0);
      GPU_storagebuf_bind(gpu_.y_ssbo, 1);
      GPU_storagebuf_bind(gpu_.accelerations_ssbo, 2);
      GPU_storagebuf_bind(gpu_.masses_inv_ssbo, 3);
      GPU_storagebuf_bind(gpu_.colors_ssbo, 4);
      GPU_storagebuf_bind(gpu_.factors_ssbo, 5);
      GPU_storagebuf_bind(gpu_.new_positions_ssbo, 6);
      GPU_storagebuf_bind(gpu_.springs_ssbo, 7);
      GPU_storagebuf_bind(gpu_.adj_offsets_ssbo, 8);
      GPU_storagebuf_bind(gpu_.adj_list_ssbo, 9);
      GPU_storagebuf_bind(gpu_.adj_spring_idx_ssbo, 10);
      GPU_storagebuf_bind(gpu_.deformation_pos_ssbo, 11);
      GPU_storagebuf_bind(gpu_.deformation_strength_ssbo, 12);
      GPU_storagebuf_bind(gpu_.softbody_pos_ssbo, 13);

      GPU_compute_dispatch(shader_solve_, groups_x, 1, 1);
      GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

      /* Copy new_positions to positions (swap buffers) */
      /* For now, we do this by binding new_positions as read and positions as write */
      /* This is handled in the integrate shader */
    }
  }

  /* Stage 3: Integration (compute velocities, swap buffers) */
  GPU_shader_bind(shader_integrate_);

  /* Set push constants */
  GPU_shader_uniform_1i(shader_integrate_, "total_vertices", N);
  GPU_shader_uniform_1f(shader_integrate_, "time_step_inv", params.time_step_inv);
  GPU_shader_uniform_1f(shader_integrate_, "damping", params.damping);

  /* Bind SSBOs for integrate shader */
  GPU_storagebuf_bind(gpu_.positions_ssbo, 0);
  GPU_storagebuf_bind(gpu_.prev_positions_ssbo, 1);
  GPU_storagebuf_bind(gpu_.accelerations_ssbo, 2);
  GPU_storagebuf_bind(gpu_.new_positions_ssbo, 3);

  GPU_compute_dispatch(shader_integrate_, groups_x, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  GPU_shader_unbind();

  /* Download results */
  download_from_gpu();
}

/* --------------------------------------------------------
 * Helper Functions
 * -------------------------------------------------------- */

void compute_vertex_colors(VBDConstraints &constraints, int vertex_count)
{
  constraints.colors.reinitialize(vertex_count);
  constraints.colors.fill(-1);

  int max_color = 0;

  for (int v = 0; v < vertex_count; v++) {
    bool used_colors[64] = {false};

    /* Check neighbors */
    const int adj_start = constraints.adj_offsets[v];
    const int adj_end = constraints.adj_offsets[v + 1];

    for (int i = adj_start; i < adj_end; i++) {
      const int neighbor = constraints.adj_list[i];
      if (neighbor >= 0 && neighbor < vertex_count) {
        const int neighbor_color = constraints.colors[neighbor];
        if (neighbor_color >= 0 && neighbor_color < 64) {
          used_colors[neighbor_color] = true;
        }
      }
    }

    /* Find first unused color */
    for (int c = 0; c < 64; c++) {
      if (!used_colors[c]) {
        constraints.colors[v] = c;
        max_color = math::max(max_color, c);
        break;
      }
    }
  }

  constraints.max_color = max_color;
}

int get_spring_count_for_mesh(const Mesh & /*mesh*/)
{
  /* TODO: Implement based on mesh topology */
  return 0;
}

}  // namespace blender::ed::sculpt_paint::cloth::vbd
