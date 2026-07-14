/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

/* Compute shader for the interactive sculpt-layer influence drag: refreshes a node's position draw
 * buffer in place on the GPU (`position += delta * scale`) instead of re-extracting and re-uploading
 * it from the CPU on every tick. See #pbvh_layer_drag_comp.glsl and the dispatch in #draw_pbvh.cc. */
GPU_SHADER_CREATE_INFO(pbvh_layer_drag)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(64)
PUSH_CONSTANT(float, scale)
PUSH_CONSTANT(int, verts_num)
STORAGE_BUF(0, read, float, delta_buf[])
STORAGE_BUF(1, read_write, float, position_buf[])
COMPUTE_SOURCE("pbvh_layer_drag_comp.glsl")
GPU_SHADER_CREATE_END()
