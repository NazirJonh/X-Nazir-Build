/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_sculpt_symmetry_plane)

void main()
{
  /* Semi-transparent plane with a red color */
  float3 plane_color = float3(1.0f, 0.2f, 0.2f); /* Red tint */
  frag_color = float4(plane_color, opacity);
}