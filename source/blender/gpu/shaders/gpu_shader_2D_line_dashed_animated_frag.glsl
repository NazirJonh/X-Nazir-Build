/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Fragment Shader for animated dashed lines, with uniform multi-color(s),
 * or any single-color, and any thickness.
 *
 * Dashed is performed in screen space.
 * Supports animation of dash position for visual feedback (e.g., selection mask).
 *
 * udash_factor usage:
 * - >= 1.0f: solid line
 * - 0.0 to 1.0: animated dash with offset (creates movement effect)
 */

#include "infos/gpu_shader_line_dashed_uniform_color_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_3D_line_dashed_animated_color)

void main()
{
  float distance_along_line = distance(stipple_pos, stipple_start);
  /* Solid line case, simple. */
  if (udash_factor >= 1.0f) {
    fragColor = color;
  }
  /* Animated dashed line - udash_factor is used as animation offset. */
  else {
    /* Shift dash positions based on udash_factor to create movement animation.
     * This is used for selection feedback to make the selection region more visible. */
    float animated_distance = distance_along_line - (udash_factor * dash_width);
    float normalized_distance = fract(animated_distance / dash_width);
    if (normalized_distance <= 0.5f) {
      fragColor = color;
    }
    else if (colors_len > 0) {
      fragColor = color2;
    }
    else {
      gpu_discard_fragment();
    }
  }
}
