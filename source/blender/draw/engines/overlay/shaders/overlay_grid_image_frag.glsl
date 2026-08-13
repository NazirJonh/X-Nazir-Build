/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_grid_image)

#include "overlay_common_lib.glsl"

void main()
{
  frag_color = ucolor;
  line_output = pack_line_data(gl_FragCoord.xy, edge_start, edge_pos);
}
