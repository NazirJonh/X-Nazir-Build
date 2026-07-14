/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_sculpt_mask)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);

  faceset_color = mix(float3(1.0f), fset, face_sets_opacity);
  /* The ordinary sculpt mask keeps its original black darkening: `mix(1, 0, a)` is `1 - a`,
   * which is the formula this overlay used before the tint existed. */
  mask_color = mix(float3(1.0f), float3(0.0f), msk * mask_opacity);
  /* Inverted against the ordinary mask on purpose: what the layer mask hides is what the user
   * needs to see, so tint follows `1 - weight` rather than the weight itself. */
  layer_color = mix(float3(1.0f), layer_mask_tint, (1.0f - layer_weight) * layer_mask_opacity);
  /* The attribute carries the recorded displacement as a fraction of the bounding-box diagonal; the
   * threshold that turns it into a tint is a user setting and is applied here, so dragging that
   * slider costs a redraw rather than a refill of every preview buffer on the mesh. Straight, not
   * inverted the way `layer_color` is: a larger value means more change, which is what the tint
   * should follow. The floor keeps a scripted or hand-edited zero from dividing by zero. */
  float preview_factor = clamp(layer_preview / max(layer_preview_threshold, 1e-6f), 0.0f, 1.0f);
  preview_color = mix(float3(1.0f), layer_preview_tint, preview_factor * layer_preview_opacity);

  view_clipping_distances(world_pos);
}
