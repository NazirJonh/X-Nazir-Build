/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief VBD (Vertex Block Descent) solver data structures for GPU-accelerated cloth brush.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "GPU_storage_buffer.hh"
#include "GPU_uniform_buffer.hh"

struct Mesh;

/* Forward declaration - SimulationData is in parent cloth namespace */
namespace blender::ed::sculpt_paint::cloth {
struct SimulationData;
struct LengthConstraint;
enum NodeSimState;
}

namespace blender::ed::sculpt_paint::cloth::vbd {

/* Import parent namespace types */
using cloth::SimulationData;
using cloth::LengthConstraint;
using cloth::NodeSimState;

/* VBD vertex data - SoA layout for GPU efficiency */
struct VBDVertexData {
  /* CPU-side data (mirrored from SimulationData) */
  Array<float3> positions;           /* Current positions (x) */
  Array<float3> prev_positions;      /* Previous timestep positions (x_prev) */
  Array<float3> inertial_positions;  /* y = x_t + h*v_t + h²*a_ext */
  Array<float3> velocities;          /* Velocities (derived from position diff) */
  Array<float3> accelerations;       /* Acceleration buffer */
  Array<float> masses;               /* Mass per vertex */
  Array<float> masses_inv;           /* 1/mass (precomputed for GPU) */
  Array<int> colors;                 /* Vertex colors for parallelization */

  /* Constraint factors from existing system */
  Array<float> constraint_factors;   /* Per-vertex simulation factor */
  Array<float> constraint_tweak;     /* Length expansion factor */

  /* For brush deformation */
  Array<float3> deformation_pos;     /* Brush deformation targets */
  Array<float> deformation_strength; /* Brush influence strength */

  /* For softbody */
  Array<float3> softbody_pos;        /* Softbody anchor positions */

  int vertex_count = 0;
};

/* VBD spring/constraint data */
struct VBDConstraints {
  /* Per-spring data */
  Array<int> spring_v1;              /* First vertex index */
  Array<int> spring_v2;              /* Second vertex index */
  Array<float> spring_rest_length;   /* Rest length */
  Array<float> spring_stiffness;     /* Spring constant k */

  /* Adjacency for fast lookup */
  Array<int> adj_offsets;            /* [N+1] offset into adj_list */
  Array<int> adj_list;               /* Neighbor vertex indices */
  Array<int> adj_spring_idx;         /* Spring index for each adjacency */

  /* Node association */
  Array<int> spring_node_index;      /* PBVH node index per spring */
  Array<NodeSimState> node_states;   /* ACTIVE/INACTIVE per node */

  /* Vertex colors for parallelization */
  Array<int> colors;                 /* Color assigned to each vertex */

  int spring_count = 0;
  int max_color = 0;
};

/* GPU buffers for VBD */
struct VBDGPUBuffers {
  /* Vertex SSBOs */
  gpu::StorageBuf *positions_ssbo = nullptr;
  gpu::StorageBuf *prev_positions_ssbo = nullptr;
  gpu::StorageBuf *y_ssbo = nullptr;            /* Inertial positions */
  gpu::StorageBuf *accelerations_ssbo = nullptr;
  gpu::StorageBuf *masses_inv_ssbo = nullptr;
  gpu::StorageBuf *colors_ssbo = nullptr;
  gpu::StorageBuf *factors_ssbo = nullptr;

  /* Constraint SSBOs */
  gpu::StorageBuf *springs_ssbo = nullptr;      /* (v1, v2, rest, k) packed */
  gpu::StorageBuf *adj_offsets_ssbo = nullptr;
  gpu::StorageBuf *adj_list_ssbo = nullptr;
  gpu::StorageBuf *adj_spring_idx_ssbo = nullptr;
  gpu::StorageBuf *node_states_ssbo = nullptr;

  /* Auxiliary buffers */
  gpu::StorageBuf *new_positions_ssbo = nullptr;  /* Double-buffered positions */
  gpu::StorageBuf *deformation_pos_ssbo = nullptr;
  gpu::StorageBuf *deformation_strength_ssbo = nullptr;
  gpu::StorageBuf *softbody_pos_ssbo = nullptr;

  /* Uniform buffer for parameters */
  gpu::UniformBuf *params_ubo = nullptr;

  bool is_initialized = false;
  bool needs_upload = true;
};

/* VBD simulation parameters (matches GPU struct in shader) */
struct VBDParams {
  float4 gravity;                 /* (gx, gy, gz, 0) */
  float time_step;                /* dt */
  float time_step_inv;            /* 1/dt */
  float time_step_sq_inv;         /* 1/(dt²) */
  float damping;                  /* Rayleigh damping coefficient */

  int total_vertices;
  int total_springs;
  int num_iterations;             /* VBD iterations per step */
  int current_color;              /* For color-pass dispatch */
  int max_color;                  /* Number of colors */

  float solver_factor;            /* Position update factor */
  float collision_stiffness;      /* For collision springs */

  /* Brush parameters */
  float4 brush_location;          /* Brush center */
  float4 brush_delta;             /* Movement direction */
  float brush_radius;
  float brush_strength;
  int brush_type;
  int padding;
};

/* Main VBD solver class */
class VBDSolver {
 public:
  /* Initialization */
  void init(int vertex_count);
  void upload_from_simulation_data(const SimulationData &sim);
  void download_to_simulation_data(SimulationData &sim);

  /* Simulation */
  void step(const VBDParams &params);

  /* GPU buffer management */
  void ensure_gpu_buffers();
  void upload_to_gpu();
  void download_from_gpu();

  /* Internal helpers */
  void build_adjacency_from_springs();

  /* Getters */
  int get_vertex_count() const { return vertices_.vertex_count; }
  int get_spring_count() const { return constraints_.spring_count; }

 private:
  VBDVertexData vertices_;
  VBDConstraints constraints_;
  VBDGPUBuffers gpu_;

  /* Shaders */
  gpu::Shader *shader_init_ = nullptr;       /* Initialize y from x, v */
  gpu::Shader *shader_solve_ = nullptr;      /* Main VBD solver */
  gpu::Shader *shader_integrate_ = nullptr;  /* Velocity computation */
};

/* Helper functions */
void compute_vertex_colors(VBDConstraints &constraints, int vertex_count);
int get_spring_count_for_mesh(const Mesh &mesh);

}  // namespace blender::ed::sculpt_paint::cloth::vbd
