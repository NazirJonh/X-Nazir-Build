/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * VBD initialization compute shader.
 * Computes inertial position: y = x + h*v + h²*a_ext
 */

#include "gpu_shader_math_vector_lib.glsl"
#include "infos/sculpt_cloth_vbd_infos.hh"

COMPUTE_SHADER_CREATE_INFO(sculpt_cloth_vbd_init)

void main()
{
  int vertex_id = int(gl_GlobalInvocationID.x);
  if (vertex_id >= total_vertices) {
    return;
  }

  /* Compute velocity from position difference */
  float3 x = positions[vertex_id].xyz;
  float3 x_prev = prev_positions[vertex_id].xyz;
  float3 velocity = (x - x_prev) * time_step_inv;

  /* Apply gravity as external acceleration */
  float3 a_ext = gravity.xyz;

  /* Compute inertial position: y = x + h*v + h²*a_ext */
  float h = time_step;
  float3 y = x + h * velocity + h * h * a_ext;

  /* Store inertial position */
  y_positions[vertex_id] = float4(y, 0.0);

  /* Initialize acceleration to gravity */
  accelerations[vertex_id] = float4(a_ext, 0.0);
}
