/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * VBD integration compute shader.
 * Computes velocities from position differences: v = (x_new - x_prev) / h
 */

#include "gpu_shader_math_vector_lib.glsl"
#include "infos/sculpt_cloth_vbd_infos.hh"

COMPUTE_SHADER_CREATE_INFO(sculpt_cloth_vbd_integrate)

void main()
{
  int vertex_id = int(gl_GlobalInvocationID.x);
  if (vertex_id >= total_vertices) {
    return;
  }

  /* Swap buffers: prev <- current <- new */
  float3 new_pos = new_positions[vertex_id].xyz;
  float3 old_pos = positions[vertex_id].xyz;

  /* Update previous position */
  prev_positions[vertex_id] = float4(old_pos, 0.0);

  /* Update current position */
  positions[vertex_id] = float4(new_pos, 0.0);

  /* Compute velocity (stored implicitly in position difference) */
  /* Velocity = (new - prev) / (2 * dt) for central difference */
  float3 prev = prev_positions[vertex_id].xyz;
  float3 velocity = (new_pos - prev) * time_step_inv * 0.5;

  /* Apply damping */
  velocity *= (1.0 - damping);

  /* Store velocity in acceleration buffer for debugging */
  accelerations[vertex_id] = float4(velocity, 0.0);
}
