/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_curves_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_sculpt_curves_points)

void main()
{
  float2 coord = gl_PointCoord - float2(0.5f);
  float dist = length(coord);
  if (dist > 0.5f) {
    discard;
  }
  outColor = float4(finalColor, 1.0f);
}
