/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * VBD forces compute shader.
 * Applies brush forces to vertices.
 */

#include "gpu_shader_math_vector_lib.glsl"
#include "infos/sculpt_cloth_vbd_infos.hh"

COMPUTE_SHADER_CREATE_INFO(sculpt_cloth_vbd_forces)

void main()
{
  int vertex_id = int(gl_GlobalInvocationID.x);
  if (vertex_id >= total_vertices) {
    return;
  }

  /* Skip if factor is zero */
  float factor = factors[vertex_id];
  if (factor < 1e-6) {
    return;
  }

  float3 pos = positions[vertex_id].xyz;
  float3 brush_loc = brush_location.xyz;
  float3 brush_d = brush_delta.xyz;

  /* Compute distance to brush */
  float dist = distance(pos, brush_loc);
  if (dist > brush_radius) {
    return;
  }

  /* Falloff */
  float falloff = 1.0 - (dist / brush_radius);
  falloff = falloff * falloff;  /* Quadratic falloff */

  /* Apply force based on brush type */
  float3 force = float3(0.0);

  /* Brush type 0: Grab/Pull */
  if (brush_type == 0) {
    force = brush_d * brush_strength * falloff * factor;
  }
  /* Brush type 1: Push */
  else if (brush_type == 1) {
    float3 dir = normalize(pos - brush_loc);
    force = dir * brush_strength * falloff * factor;
  }
  /* Brush type 2: Pinch */
  else if (brush_type == 2) {
    float3 dir = normalize(brush_loc - pos);
    force = dir * brush_strength * falloff * factor;
  }

  /* Add force to inertial position */
  float3 y = y_positions[vertex_id].xyz;
  float h2 = time_step * time_step;
  y_positions[vertex_id] = float4(y + h2 * force, 0.0);
}
