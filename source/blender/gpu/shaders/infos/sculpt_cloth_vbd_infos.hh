/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Shader create info for VBD (Vertex Block Descent) cloth simulation.
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "GPU_shader_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

/* Initialize inertial positions shader: y = x + h*v + h²*a_ext */
GPU_SHADER_CREATE_INFO(sculpt_cloth_vbd_init)
LOCAL_GROUP_SIZE(16)
PUSH_CONSTANT(int, total_vertices)
PUSH_CONSTANT(float, time_step)
PUSH_CONSTANT(float, time_step_inv)
PUSH_CONSTANT(float4, gravity)
STORAGE_BUF(0, read_write, float4, positions[])
STORAGE_BUF(1, read, float4, prev_positions[])
STORAGE_BUF(2, read_write, float4, y_positions[])
STORAGE_BUF(3, read_write, float4, accelerations[])
TYPEDEF_SOURCE("GPU_shader_shared.hh")
COMPUTE_SOURCE("sculpt_cloth_vbd_init_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Main VBD solver shader - performs Gauss-Seidel iterations per color */
GPU_SHADER_CREATE_INFO(sculpt_cloth_vbd_solve)
LOCAL_GROUP_SIZE(16)
/* Uniform parameters */
PUSH_CONSTANT(int, total_vertices)
PUSH_CONSTANT(float, time_step_sq_inv)
PUSH_CONSTANT(float, solver_factor)
PUSH_CONSTANT(float, collision_stiffness)
PUSH_CONSTANT(int, current_color)
/* Vertex data */
STORAGE_BUF(0, read_write, float4, positions[])
STORAGE_BUF(1, read, float4, y_positions[])
STORAGE_BUF(2, read_write, float4, accelerations[])
STORAGE_BUF(3, read, float, masses_inv[])
STORAGE_BUF(4, read, int, colors[])
STORAGE_BUF(5, read, float, factors[])
STORAGE_BUF(6, read_write, float4, new_positions[])
/* Constraint/spring data */
STORAGE_BUF(7, read, float4, springs[])
STORAGE_BUF(8, read, int, adj_offsets[])
STORAGE_BUF(9, read, int, adj_list[])
STORAGE_BUF(10, read, int, adj_spring_idx[])
/* Brush data */
STORAGE_BUF(11, read, float4, deformation_pos[])
STORAGE_BUF(12, read, float, deformation_strength[])
STORAGE_BUF(13, read, float4, softbody_pos[])
TYPEDEF_SOURCE("GPU_shader_shared.hh")
COMPUTE_SOURCE("sculpt_cloth_vbd_solve_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Integration shader - compute velocities from position differences */
GPU_SHADER_CREATE_INFO(sculpt_cloth_vbd_integrate)
LOCAL_GROUP_SIZE(16)
PUSH_CONSTANT(int, total_vertices)
PUSH_CONSTANT(float, time_step_inv)
PUSH_CONSTANT(float, damping)
STORAGE_BUF(0, read_write, float4, positions[])
STORAGE_BUF(1, read_write, float4, prev_positions[])
STORAGE_BUF(2, read_write, float4, accelerations[])
STORAGE_BUF(3, read_write, float4, new_positions[])
TYPEDEF_SOURCE("GPU_shader_shared.hh")
COMPUTE_SOURCE("sculpt_cloth_vbd_integrate_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Apply forces shader (brush forces) */
GPU_SHADER_CREATE_INFO(sculpt_cloth_vbd_forces)
LOCAL_GROUP_SIZE(16)
PUSH_CONSTANT(int, total_vertices)
PUSH_CONSTANT(int, brush_type)
PUSH_CONSTANT(float4, brush_location)
PUSH_CONSTANT(float4, brush_delta)
PUSH_CONSTANT(float, brush_radius)
PUSH_CONSTANT(float, brush_strength)
PUSH_CONSTANT(float, time_step)
STORAGE_BUF(0, read, float4, positions[])
STORAGE_BUF(1, read_write, float4, y_positions[])
STORAGE_BUF(2, read, float, factors[])
TYPEDEF_SOURCE("GPU_shader_shared.hh")
COMPUTE_SOURCE("sculpt_cloth_vbd_forces_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
