/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * VBD solver compute shader.
 * Performs one Gauss-Seidel iteration for vertices of the current color.
 * Solves local 3x3 system: H * Δx = f
 */

#include "gpu_shader_math_matrix_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "infos/sculpt_cloth_vbd_infos.hh"

COMPUTE_SHADER_CREATE_INFO(sculpt_cloth_vbd_solve)

/* Compute spring force and Hessian contribution */
void compute_spring_contribution(float3 xi,
                                  float3 xj,
                                  float rest_length,
                                  float stiffness,
                                  float scale,
                                  out float3 force,
                                  out float3x3 hessian)
{
  float3 diff = xi - xj;
  float len = length(diff);

  if (len < 1e-6) {
    force = float3(0.0);
    hessian = float3x3(0.0);
    return;
  }

  float3 n = diff / len;
  float stretch = len - rest_length;

  /* Force: f = -k * stretch * n
   * Scale by time_step_sq_inv to balance with inertial term
   */
  float scaled_stiffness = stiffness * scale;
  force = -scaled_stiffness * stretch * n;

  /* Hessian: H = k * (n*n^T + (1 - rest/len) * (I - n*n^T)) */
  float3x3 nnt = outerProduct(n, n);
  float factor = 1.0 - rest_length / len;
  float3x3 I_minus_nnt = float3x3(1.0) - nnt;
  hessian = scaled_stiffness * (nnt + factor * I_minus_nnt);
}

void main()
{
  int vertex_id = int(gl_GlobalInvocationID.x);
  if (vertex_id >= total_vertices) {
    return;
  }

  /* Only process vertices of the current color */
  if (colors[vertex_id] != current_color) {
    /* Don't write to new_positions - keep the value from previous color or init */
    return;
  }

  /* Skip if factor is zero (pinned) - keep existing new_positions value */
  float factor = factors[vertex_id];
  if (factor < 1e-6) {
    return;
  }

  float3 xi = positions[vertex_id].xyz;
  float3 yi = y_positions[vertex_id].xyz;
  float mass_inv = masses_inv[vertex_id];

  /* Initialize with inertial term */
  float3 force = -time_step_sq_inv * (xi - yi) / mass_inv;
  float3x3 hessian = float3x3(time_step_sq_inv / mass_inv);

  /* Sum spring contributions */
  int adj_start = adj_offsets[vertex_id];
  int adj_end = adj_offsets[vertex_id + 1];

  /* Use sqrt of time_step_sq_inv for more moderate scaling (~62.5 instead of 3906) */
  float spring_scale = sqrt(time_step_sq_inv);

  for (int i = adj_start; i < adj_end; i++) {
    int neighbor = adj_list[i];
    int spring_idx = adj_spring_idx[i];

    float4 spring = springs[spring_idx];
    float rest_length = spring.z;
    float stiffness = spring.w;

    /* Read neighbor position from the correct buffer */
    float3 xj;
    int neighbor_color = colors[neighbor];
    if (neighbor_color < current_color) {
      xj = new_positions[neighbor].xyz;
    }
    else {
      xj = positions[neighbor].xyz;
    }

    float3 spring_force;
    float3x3 spring_hessian;
    /* Use moderate scaling to balance springs with inertial term */
    compute_spring_contribution(xi, xj, rest_length, stiffness, spring_scale, spring_force, spring_hessian);

    force += spring_force;
    hessian += spring_hessian;
  }

  /* Apply brush deformation constraint */
  float deform_strength = deformation_strength[vertex_id];
  if (deform_strength > 1e-6) {
    float3 target = deformation_pos[vertex_id].xyz;
    float k_deform = collision_stiffness * deform_strength;
    force += k_deform * (target - xi);
    hessian += float3x3(k_deform);
  }

  /* Apply softbody constraint */
  float3 softbody = softbody_pos[vertex_id].xyz;
  if (length(softbody) > 1e-6) {
    float k_soft = collision_stiffness * 0.1;
    force += k_soft * (softbody - xi);
    hessian += float3x3(k_soft);
  }

  /* Solve 3x3 system: H * Δx = f */
  float3 delta = inverse(hessian) * force;

  /* Apply update with factor */
  float3 new_pos = xi + delta * factor * solver_factor;
  new_positions[vertex_id] = float4(new_pos, 0.0);
}
