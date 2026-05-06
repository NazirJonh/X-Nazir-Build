/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_extra_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_extra_wire_base)

#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

#ifdef CONTOUR_USE_WIDTH
#  define LINE_WIDTH_PACK_SCALE 255.0f
#  define CONTOUR_WIDTH_VALUE max(contour_width, 1.0f)
#else
#  define CONTOUR_WIDTH_VALUE 1.0f
#endif

void main()
{
#ifdef CONTOUR_NO_STIPPLE
  frag_color = final_color;
  line_output = pack_line_data(gl_FragCoord.xy, stipple_start, stipple_coord);
  line_output.w = CONTOUR_WIDTH_VALUE / LINE_WIDTH_PACK_SCALE;
  select_id_output(select_id);
  return;
#endif

  frag_color = final_color;

  /* Stipple */
  /* GLSL не поддерживает constexpr, используем обычные константы. */
  const float dash_width = 6.0f;
  const float dash_factor = 0.5f;

  line_output = pack_line_data(gl_FragCoord.xy, stipple_start, stipple_coord);

  float dist = distance(stipple_start, stipple_coord);

  if (frag_color.a == 0.0f) {
    /* Disable stippling. */
    dist = 0.0f;
  }

  frag_color.a = 1.0f;

#ifndef SELECT_ENABLE
  /* Discarding inside the selection will create some undefined behavior.
   * This is because we force the early depth test to only output the front most fragment.
   * Discarding would expose us to race condition depending on rasterization order. */
  if (fract(dist / dash_width) > dash_factor) {
    gpu_discard_fragment();
  }
#endif

  select_id_output(select_id);
}
