/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_wireframe_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_wireframe)

#include "draw_model_lib.glsl"
#include "draw_object_infos_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

#if !defined(POINTS) && !defined(CURVES)
bool is_edge_sharpness_visible(float wire_data)
{
  return wire_data <= wire_step_param;
}
#endif

#if !defined(POINTS) && !defined(CURVES)
/**
 * Flat reveal offset applied to every subdivision level's visibility
 * threshold (in `wire_level` units). Each unit of offset means a level
 * needs `2^N`× more screen coverage to appear. Tuned so a mesh that
 * fully fits the viewport at a normal viewing distance shows only the
 * base mesh + maybe L1, and deeper levels are revealed progressively as
 * the camera moves closer — with the deepest level appearing only when
 * its cells are large on screen. Main knob for "how conservative" the
 * adaptive wireframe is overall.
 */
const float LEVEL_REVEAL_OFFSET = 2.0f;

/**
 * Extra per-level visibility threshold beyond the linear `level + 0.5` schedule.
 * Without this, the visibility law `wire_level = log2(object_px / MIN_CELL)`
 * grows by exactly one level per 2× zoom — deep subdivisions (L5-L11) appear
 * as quickly as shallow ones, which over-populates the wireframe when the
 * camera is close to a small mesh. The quadratic term keeps L0-L3 on the
 * original schedule and makes each subsequent level require progressively
 * more screen coverage to appear.
 */
float multires_level_extra_threshold(uint level)
{
  float L = float(level);
  return 0.05f * L * L;
}

/**
 * Compute subdivision wireframe visibility level based on screen-space size.
 * Uses logarithmic scale: wire_level = log2(object_screen_px / min_cell_size_px).
 *
 * Returns a value in the normalized level space [0..norm_max_level], where 0 is
 * the coarsest visible edge and norm_max_level is the finest. Raw VBO levels are
 * offset by `wire_level_min` (non-zero in Sculpt Mode), so the caller must
 * normalize raw levels before comparing to the returned value.
 */
float compute_multires_wire_level()
{
  if (gl_Position.w <= 0.0f) {
    return 0.0f;
  }

  float dist_w = abs(gl_Position.w);
  float diameter = multires_wire_buf.object_diameter;

  /* Compute how many pixels the object occupies on screen.
   * pixels_per_world converts world distance to screen pixels.
   * winmat[1][1] encodes orthographic zoom or perspective fov.
   * This formula works identically in perspective and orthographic. */
  float pixels_per_world = drw_view().winmat[1][1] * uniform_buf.size_viewport.y * 0.5f / dist_w;
  float object_screen_px = diameter * pixels_per_world;

  /* Visibility threshold: don't show grid cells smaller than this in pixels.
   * Subdivisions double grid cells each level, so:
   *   cell_size_px = object_screen_px / (2^level)
   * Hide levels where cell_size_px < MIN_CELL_SIZE_PX:
   *   wire_level = log2(object_screen_px / MIN_CELL_SIZE_PX)
   * Higher value = fewer deep subdivisions visible, less visual noise.
   * At 50 px combined with the `level + 0.5` cull offset, the effective
   * hide threshold is ~70 px cells. With this setting a level appears only
   * when its individual edges span more than ~70 px on screen, giving a
   * clean look at any zoom: at close range only 4-5 levels are visible at
   * once instead of the entire subdivision pyramid. */
  const float MIN_CELL_SIZE_PX = 100.0f;

  float wire_level = log2(max(object_screen_px / MIN_CELL_SIZE_PX, 0.0001f));

  /* Normalized depth: number of subdivision levels present in the VBO data.
   * wire_level_max and wire_level_min are both in raw VBO units; their difference
   * gives the 0-based range that `multires_level_fade` operates in. */
  float norm_max_level = multires_wire_buf.wire_level_max - multires_wire_buf.wire_level_min;

  /* Cap so the object's deepest level can still reach full visibility inside
   * its (now shifted) smoothstep band. The fade band for level L is
   * `[L + 0.5 + extra(L) + OFFSET .. L + 1.0 + extra(L) + OFFSET]`, so the
   * cap must include the same extra term and reveal offset for `norm_max_level`,
   * otherwise the deepest level would always sit below its own visibility
   * threshold and never appear. */
  float max_level_top = norm_max_level + 1.0f +
                        multires_level_extra_threshold(uint(norm_max_level)) + LEVEL_REVEAL_OFFSET;
  wire_level = min(wire_level, max_level_top);

  return wire_level;
}

/**
 * Per-edge fade for a normalized subdivision `level` (0 = coarsest visible edge).
 * Level 0 is always fully bright. Deeper levels use a sharp 0.5-wide fade band
 * that starts at the hide threshold (`level + 0.5`, matching the cull check in
 * `main`) and reaches full visibility at `level + 1.0`. The hierarchy dimmer
 * linearly interpolates each level's brightness between 1.0 and a floor based on
 * its position inside the currently visible range `[0 .. wire_level]`, so the
 * readable brightness band stretches across however many levels are exposed at
 * the current zoom — preventing deep levels from collapsing onto the same
 * intensity when several of them are simultaneously visible.
 *
 * `level` must already be normalized: `raw_vbo_level - wire_level_min`.
 */
float multires_level_fade(uint level)
{
  if (level == 0u) {
    return 1.0f;
  }
  float wire_level = compute_multires_wire_level();
  float extra = multires_level_extra_threshold(level);
  float hide = float(level) + 0.5f + extra + LEVEL_REVEAL_OFFSET;
  float fade = smoothstep(hide, hide + 0.5f, wire_level);

  /* Adaptive hierarchy dimmer: normalize the level's position within the
   * range `[0 .. min(wire_level, norm_max_level)]`. The `min` with `norm_max_level`
   * is essential for objects with few subdivisions (e.g. a cube with
   * `totlvl = 3`): without it, when `wire_level` exceeds `norm_max_level`,
   * the deepest existing level never reaches `t = 1` and ends up at the same
   * brightness as shallower levels. Anchoring on `norm_max_level` guarantees
   * L0 is always brightest and the deepest existing level always lands at
   * `HIERARCHY_FLOOR`, while during the reveal phase (`wire_level < norm_max_level`)
   * the band stretches over only the currently exposed levels. */
  const float HIERARCHY_FLOOR = 0.15f;
  float norm_max_level = multires_wire_buf.wire_level_max - multires_wire_buf.wire_level_min;
  /* Subtract `LEVEL_REVEAL_OFFSET` so the denominator tracks the levels
   * actually exposed at the current zoom (rather than the zoom value
   * itself). Without this, the conservative reveal offset would leave
   * the deepest visible level mid-brightness instead of near FLOOR. */
  float visible_depth = max(min(wire_level - LEVEL_REVEAL_OFFSET, norm_max_level), 1.0f);
  float t = clamp(float(level) / visible_depth, 0.0f, 1.0f);
  /* Quadratic curve: shallow levels stay near full brightness longer,
   * with the falloff concentrated toward the deepest visible levels —
   * makes the outer-most subdivisions read as the dominant structure. */
  float hierarchy_dimmer = mix(1.0f, HIERARCHY_FLOOR, t * t);

  return fade * hierarchy_dimmer;
}
#endif

void wire_color_get(float3 &rim_col, float3 &wire_col)
{
  eObjectInfoFlag ob_flag = drw_object_infos().flag;
  bool is_selected = flag_test(ob_flag, OBJECT_SELECTED);
  bool is_from_set = flag_test(ob_flag, OBJECT_FROM_SET);
  bool is_active = flag_test(ob_flag, OBJECT_ACTIVE);

  if (is_from_set) {
    rim_col = theme.colors.wire.rgb;
    wire_col = theme.colors.wire.rgb;
  }
  else if (is_selected && use_coloring) {
    if (is_transform) {
      rim_col = theme.colors.transform.rgb;
    }
    else if (is_active) {
      rim_col = theme.colors.active_object.rgb;
    }
    else {
      rim_col = theme.colors.object_select.rgb;
    }
    wire_col = theme.colors.wire.rgb;
  }
  else {
    rim_col = theme.colors.wire.rgb;
    wire_col = theme.colors.background.rgb;
  }
}

float3 hsv_to_rgb(float3 hsv)
{
  float3 nrgb = abs(hsv.x * 6.0f - float3(3.0f, 2.0f, 4.0f)) * float3(1, -1, -1) +
                float3(-1, 2, 2);
  nrgb = clamp(nrgb, 0.0f, 1.0f);
  return ((nrgb - 1.0f) * hsv.y + 1.0f) * hsv.z;
}

void wire_object_color_get(float3 &rim_col, float3 &wire_col)
{
  ObjectInfos info = drw_object_infos();
  bool is_selected = flag_test(info.flag, OBJECT_SELECTED);

  if (color_type == V3D_SHADING_OBJECT_COLOR) {
    rim_col = wire_col = drw_object_infos().ob_color.rgb * 0.5f;
  }
  else {
    float hue = info.random;
    float3 hsv = float3(hue, 0.75f, 0.8f);
    rim_col = wire_col = hsv_to_rgb(hsv);
  }

  if (is_selected && use_coloring) {
    /* "Normalize" color. */
    wire_col += 1e-4f; /* Avoid division by 0. */
    float brightness = max(wire_col.x, max(wire_col.y, wire_col.z));
    wire_col *= 0.5f / brightness;
    rim_col += 0.75f;
  }
  else {
    rim_col *= 0.5f;
    wire_col += 0.5f;
  }
}

void main()
{
  select_id_set(drw_custom_id());

  /* If no attribute is available, use a fixed facing value depending on the coloring mode.
   * This allow to keep most of the contrast between unselected and selected color
   * while keeping object coloring mode working (see #134011). */
  float no_nor_facing = (color_type == V3D_SHADING_SINGLE_COLOR) ? 0.0f : 0.5f;

#ifdef WITH_RADIUS
  float3 wpos = drw_point_object_to_world(pos_rad.xyz);
  wpos += drw_world_incident_vector(wpos) * pos_rad.w;
#else
  float3 wpos = drw_point_object_to_world(pos);
#endif

#if defined(POINTS)
  gl_PointSize = theme.sizes.vert * 2.0f;
#elif defined(CURVES)
  float facing = no_nor_facing;
#else
  float3 wnor = safe_normalize(drw_normal_object_to_world(nor));

  if (is_hair) {
    float4x4 obmat = hair_dupli_matrix;
    wpos = (obmat * float4(pos, 1.0f)).xyz;
    wnor = -normalize(to_float3x3(obmat) * nor);
  }

  bool is_persp = (drw_view().winmat[3][3] == 0.0f);
  float3 V = (is_persp) ? normalize(drw_view().viewinv[3].xyz - wpos) : drw_view().viewinv[2].xyz;

  bool no_attr = all(equal(nor, float3(0)));
  float facing = no_attr ? no_nor_facing : dot(wnor, V);
#endif

  gl_Position = drw_point_world_to_homogenous(wpos);

#if !defined(POINTS) && !defined(CURVES)
  /* Integer varyings cannot be smoothly interpolated by the pipeline — GLSL requires
   * `flat` for integer types. `FIRST_VERTEX_CONVENTION` ties the provoking vertex to
   * the first corner of each edge, matching the per-corner VBO layout. */
  subdiv_level_iface = subdiv_level;
  multires_fade_iface = 1.0f;

  if (!use_custom_depth_bias) {
    float facing_ratio = clamp(1.0f - facing * facing, 0.0f, 1.0f);
    float flip = sign(facing); /* Flip when not facing the normal (i.e.: back-facing). */
    float curvature = (1.0f - wd * 0.75f); /* Avoid making things worse for curvy areas. */
    float3 wofs = wnor * (facing_ratio * curvature * flip);
    wofs = drw_normal_world_to_view(wofs);

    /* Push vertex half a pixel (maximum) in normal direction. */
    gl_Position.xy += wofs.xy * uniform_buf.size_viewport_inv * gl_Position.w;

    /* Push the vertex towards the camera. Helps a bit. */
    gl_Position.z -= facing_ratio * curvature * 1.0e-6f * gl_Position.w;
  }
#elif defined(CURVES)
  /* CURVES uses the same interface (overlay_wireframe_iface) but has no subdiv_level
   * vertex input. Default to level 0 so it is always fully visible. */
  subdiv_level_iface = 0u;
  multires_fade_iface = 1.0f;
  /* POINTS has a different interface entirely — no subdiv_level_iface field at all. */
#endif

  /* Curves do not need the offset since they *are* the curve geometry. */
#if !defined(CURVES)
  gl_Position.z -= ndc_offset_factor * gl_Position.w;
#endif

  float3 rim_col, wire_col;
  if (color_type == V3D_SHADING_OBJECT_COLOR || color_type == V3D_SHADING_RANDOM_COLOR) {
    wire_object_color_get(rim_col, wire_col);
  }
  else {
    wire_color_get(rim_col, wire_col);
  }

#if defined(POINTS)
  final_color = float4(wire_col * wire_opacity, wire_opacity);
  final_color_inner = float4(rim_col * wire_opacity, wire_opacity);

#else
  /* Convert to screen position [0..sizeVp]. */
  edge_start = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) * uniform_buf.size_viewport;
  edge_pos = edge_start;

#  if !defined(SELECT_ENABLE)
  facing = clamp(abs(facing), 0.0f, 1.0f);
  /* Do interpolation in a non-linear space to have a better visual result. */
  rim_col = pow(rim_col, float3(1.0f / 2.2f));
  wire_col = pow(wire_col, float3(1.0f / 2.2f));
  float3 final_front_col = mix(rim_col, wire_col, 0.35f);
  final_color.rgb = mix(rim_col, final_front_col, facing);
  final_color.rgb = pow(final_color.rgb, float3(2.2f));
#  endif

  final_color.a = wire_opacity;
  final_color.rgb *= wire_opacity;

#  if !defined(CURVES)
  if (use_multires_wireframe) {
    /* Normalize raw VBO level to 0-based range: 0 = coarsest visible edge.
     * In Sculpt Mode (PBVH grids) the VBO stores `level_offset + local_level`
     * where `level_offset = max(0, total_level - grid_depth)` may be > 0.
     * Subtracting `wire_level_min` (= level_offset) makes the shader operate
     * uniformly regardless of PBVH grid depth. */
    uint raw_level = subdiv_level_iface;
    uint level_min_u = uint(multires_wire_buf.wire_level_min);
    uint level = (raw_level >= level_min_u) ? (raw_level - level_min_u) : 0u;

    float fade = multires_level_fade(level);
    if (fade <= 0.0f) {
      edge_start = float2(-1.0f);
    }
    else {
      /* Keep `final_color` smooth; the fragment shader applies this flat factor so the facing
       * gradient along the edge is preserved while the edge fades uniformly. */
      multires_fade_iface = fade;
    }
  }
#  endif

#  if !defined(CURVES)
  /* Cull flat edges below threshold. */
  if (!no_attr && !is_edge_sharpness_visible(wd)) {
    edge_start = float2(-1.0f);
  }
#  endif

#  if defined(SELECT_ENABLE)
  /* HACK: to avoid losing sub-pixel object in selections, we add a bit of randomness to the
   * wire to at least create one fragment that will pass the occlusion query. */
  gl_Position.xy += uniform_buf.size_viewport_inv * gl_Position.w *
                    ((gl_VertexID % 2 == 0) ? -1.0f : 1.0f);
#  endif
#endif

  view_clipping_distances(wpos);
}
