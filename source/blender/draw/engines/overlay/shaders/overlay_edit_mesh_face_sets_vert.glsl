/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_mesh_face_sets)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "overlay_common_lib.glsl"

/* Hash helpers reused from paint overlay color generation. */
float hash11(uint n)
{
  n = (n << 13u) ^ n;
  n = n * (n * n * 15731u + 789221u) + 1376312589u;
  return float(n & 0x00FFFFFFu) / float(0x01000000u);
}

float3 face_set_color_from_id(int face_set_id, int seed)
{
  /* Mirror of BKE_paint_face_set_overlay_color_get. */
  const float golden_ratio_conjugate = 0.618033988749895f;
  float random_mod_hue = golden_ratio_conjugate * float(face_set_id + (seed % 10));
  random_mod_hue = fract(random_mod_hue);
  const float random_mod_sat = hash11(uint(face_set_id + seed + 1));
  const float random_mod_val = hash11(uint(face_set_id + seed + 2));

  /* Local HSV->RGB to avoid extra includes. */
  float h = random_mod_hue;
  float s = 0.6f + (random_mod_sat * 0.25f);
  float v = 1.0f - (random_mod_val * 0.35f);

  float r = v;
  float g = v;
  float b = v;

  if (s != 0.0) {
    h = fract(h) * 6.0;
    int i = int(h);
    float f = h - float(i);
    float p = v * (1.0 - s);
    float q = v * (1.0 - s * f);
    float t = v * (1.0 - s * (1.0 - f));

    if (i == 0) {
      r = v;
      g = t;
      b = p;
    }
    else if (i == 1) {
      r = q;
      g = v;
      b = p;
    }
    else if (i == 2) {
      r = p;
      g = v;
      b = t;
    }
    else if (i == 3) {
      r = p;
      g = q;
      b = v;
    }
    else if (i == 4) {
      r = t;
      g = p;
      b = v;
    }
    else {
      r = v;
      g = p;
      b = q;
    }
  }

  float3 rgb = float3(r, g, b);
  return rgb;
}

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  float3 view_pos = drw_point_world_to_view(world_pos);
  gl_Position = drw_point_view_to_homogenous(view_pos);

  /* Use proper Z-offset like retopology shader */
  gl_Position.z += get_homogenous_z_offset(
      drw_view().winmat, view_pos.z, gl_Position.w, retopology_offset);

  /* Compute face set color from id. Default is transparent unless retopology is enabled,
   * where we reuse the retopology theme color to keep parity with sculpt retopo view. */
  bool is_default = (face_set_id == face_set_default);
  float3 rgb = is_default ? (retopology_enabled ? theme.colors.face_retopology.rgb : float3(0.0)) :
                            face_set_color_from_id(face_set_id, face_set_seed);
  float alpha = is_default ? (retopology_enabled ? face_sets_opacity : 0.0) : face_sets_opacity;

  /* When retopology is enabled, we use BLEND_ALPHA instead of BLEND_MUL because the base mesh
   * is hidden, so render_fb contains black/background. With BLEND_ALPHA, we output the color
   * directly with alpha (controlled by face_sets_opacity), so it blends properly on top of the
   * black background.
   *
   * In normal mode (BLEND_MUL), we mix from white (1.0) to face set color, like Sculpt Mode.
   * This gives the correct multiplicative blending result: dst.rgb = src.rgb * dst.rgb */
  face_set_color = retopology_enabled ?
                       /* For BLEND_ALPHA, output color directly with alpha. */
                       float4(rgb, alpha) :
                       /* For BLEND_MUL, mix from white to color, like Sculpt Mode. */
                       float4(mix(float3(1.0f), rgb, alpha), 1.0f);

  color_fac = 1.0f;
  view_clipping_distances(world_pos);
}
