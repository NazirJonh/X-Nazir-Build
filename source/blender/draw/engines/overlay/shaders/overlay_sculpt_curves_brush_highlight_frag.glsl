/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_curves_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_sculpt_curves_brush_highlight)

void main()
{
  /* Red highlight color with intensity-based alpha */
  float3 highlight_color = float3(1.0f, 0.2f, 0.1f);
  out_color = float4(highlight_color, highlight_intensity);
}
