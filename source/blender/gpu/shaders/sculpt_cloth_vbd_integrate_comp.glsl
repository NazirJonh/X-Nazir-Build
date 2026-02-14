/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * VBD integration compute shader.
 * For intermediate iterations: just swap buffers (positions = new_positions)
 * For final integration: also update prev_positions and compute velocity
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

  float3 new_pos = new_positions[vertex_id].xyz;

  /* Always update current position from new_positions */
  positions[vertex_id] = float4(new_pos, 0.0);

  /* Also initialize new_positions for next iteration */
  new_positions[vertex_id] = float4(new_pos, 0.0);

  /* Only update prev_positions and velocity if damping > 0 (final integration) */
  if (damping > 0.0) {
    float3 old_pos = prev_positions[vertex_id].xyz;

    /* Compute velocity for next frame */
    float3 velocity = (new_pos - old_pos) * time_step_inv;

    /* Apply damping */
    velocity *= (1.0 - damping);

    /* Store velocity for next frame's inertial position computation */
    accelerations[vertex_id] = float4(velocity, 0.0);
  }
}
