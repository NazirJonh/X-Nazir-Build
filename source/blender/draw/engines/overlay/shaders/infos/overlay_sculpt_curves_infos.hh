/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define CURVES_SHADER
#  define DRW_HAIR_INFO
#endif

#include "overlay_common_infos.hh"

GPU_SHADER_INTERFACE_INFO(overlay_sculpt_curves_selection_iface)
SMOOTH(float, mask_weight)
FLAT(float3, tint_color)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_sculpt_curves_selection)
DO_STATIC_COMPILATION()
PUSH_CONSTANT(bool, is_point_domain)
PUSH_CONSTANT(bool, use_object_color)
PUSH_CONSTANT(float, selection_opacity)
SAMPLER(2, samplerBuffer, selection_tx)
VERTEX_OUT(overlay_sculpt_curves_selection_iface)
VERTEX_SOURCE("overlay_sculpt_curves_selection_vert.glsl")
FRAGMENT_SOURCE("overlay_sculpt_curves_selection_frag.glsl")
FRAGMENT_OUT(0, float4, out_color)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_curves)
ADDITIONAL_INFO(draw_curves_infos)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_sculpt_curves_selection_clipped,
                    overlay_sculpt_curves_selection,
                    drw_clipped)

GPU_SHADER_INTERFACE_INFO(overlay_sculpt_curves_cage_iface)
NO_PERSPECTIVE(float2, edge_pos)
FLAT(float2, edge_start)
SMOOTH(float4, final_color)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_sculpt_curves_cage)
DO_STATIC_COMPILATION()
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float, selection)
VERTEX_IN(2, float, curve_hit)
VERTEX_OUT(overlay_sculpt_curves_cage_iface)
FRAGMENT_OUT(0, float4, frag_color)
FRAGMENT_OUT(1, float4, line_output)
PUSH_CONSTANT(float, opacity)
PUSH_CONSTANT(bool, hide_hit_curves)
VERTEX_SOURCE("overlay_sculpt_curves_cage_vert.glsl")
FRAGMENT_SOURCE("overlay_extra_frag.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_sculpt_curves_cage_clipped, overlay_sculpt_curves_cage, drw_clipped)

GPU_SHADER_INTERFACE_INFO(overlay_sculpt_curves_influence_iface)
NO_PERSPECTIVE(float2, edge_pos)
FLAT(float2, edge_start)
SMOOTH(float4, final_color)
GPU_SHADER_INTERFACE_END()

/* Brush influence / hover highlight for sculpt curves mode: line strips through the control
 * points (cage topology), colored per-vertex by the weights published by the brushes or the
 * hover preview (see #sculpt_influence_viz.cc), so the highlight sits exactly on the cage lines.
 * Drawn through the same linear/AA pipeline as the cage. */
GPU_SHADER_CREATE_INFO(overlay_sculpt_curves_influence)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("overlay_shader_shared.hh")
DEFINE("LINE_OUTPUT")
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float, influence)
VERTEX_IN(2, float, curve_hit)
VERTEX_OUT(overlay_sculpt_curves_influence_iface)
PUSH_CONSTANT(float, influence_opacity)
PUSH_CONSTANT(bool, show_influence)
PUSH_CONSTANT(bool, show_hover_hit)
FRAGMENT_OUT(0, float4, frag_color)
FRAGMENT_OUT(1, float4, line_output)
VERTEX_SOURCE("overlay_sculpt_curves_influence_vert.glsl")
FRAGMENT_SOURCE("overlay_extra_frag.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_sculpt_curves_influence_clipped,
                    overlay_sculpt_curves_influence,
                    drw_clipped)
