/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute shader for GPU-based sculpt painting.
 * This shader performs brush painting operations directly on GPU textures,
 * eliminating the CPU->GPU transfer bottleneck.
 */

#include "infos/gpu_paint_infos.hh"

COMPUTE_SHADER_CREATE_INFO(gpu_paint_compute)

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

/**
 * Calculate brush distance based on falloff shape.
 * falloff_shape: 0 = Sphere (3D distance), 1 = Projected (2D distance)
 */
float calc_brush_distance(float3 pixel_pos, float3 brush_pos, int falloff_shape)
{
  float3 diff = pixel_pos - brush_pos;
  if (falloff_shape == 0) {
    /* Sphere falloff: use full 3D distance. */
    return length(diff);
  }
  else {
    /* Projected/Tube falloff: use 2D distance in XY plane. */
    return length(diff.xy);
  }
}

/**
 * Calculate brush falloff factor based on distance, radius and hardness.
 * Matches CPU implementation in sculpt.cc.
 */
float calc_falloff(float distance, float radius, float hardness)
{
  /* Outside brush radius. */
  if (distance >= radius) {
    return 0.0;
  }

  float x = distance / radius;

  /* Inside hardness region: full strength. */
  if (x < hardness) {
    return 1.0;
  }

  /* Falloff region: quadratic curve. */
  float t = (x - hardness) / (1.0 - hardness);
  return 1.0 - t * t;
}

/**
 * Blend colors based on blend mode.
 * Modes match IMB_BlendMode enum.
 */
float4 blend_colors(float4 base, float4 paint, int blend_mode)
{
  float4 result;

  switch (blend_mode) {
    case 0: /* IMB_BLEND_MIX */
      result = mix(base, paint, paint.a);
      break;
    case 1: /* IMB_BLEND_ADD */
      result = base + paint;
      break;
    case 2: /* IMB_BLEND_SUB */
      result = base - paint;
      break;
    case 3: /* IMB_BLEND_MUL */
      result = base * paint;
      break;
    case 4: /* IMB_BLEND_LIGHTEN */
      result = float4(max(base.rgb, paint.rgb * paint.a), base.a);
      break;
    case 5: /* IMB_BLEND_DARKEN */
      result = float4(min(base.rgb, paint.rgb * paint.a), base.a);
      break;
    case 6: /* IMB_BLEND_ERASE_ALPHA */
      result = float4(base.rgb, max(0.0, base.a - paint.a));
      break;
    case 7: /* IMB_BLEND_ADD_ALPHA */
      result = float4(base.rgb, min(1.0, base.a + paint.a));
      break;
    default:
      result = mix(base, paint, paint.a);
      break;
  }

  return result;
}

/**
 * Interpolate 3D position from barycentric coordinates.
 */
float3 calc_pixel_position(uint tri_index, float2 barycentric)
{
  uint3 tri = triangles[tri_index];
  float3 p0 = vertex_positions[tri.x];
  float3 p1 = vertex_positions[tri.y];
  float3 p2 = vertex_positions[tri.z];

  float w0 = barycentric.x;
  float w1 = barycentric.y;
  float w2 = 1.0 - barycentric.x - barycentric.y;

  return p0 * w0 + p1 * w1 + p2 * w2;
}

/**
 * Sample brush texture (if available).
 * Returns white (1.0) if no texture is bound.
 */
float4 sample_brush_texture(float3 pixel_pos, float3 brush_pos)
{
  if (has_brush_texture == 0) {
    return float4(1.0);
  }

  /* Calculate local position relative to brush center. */
  float2 local_pos = pixel_pos.xy - brush_pos.xy;

  /* Apply rotation. */
  float c = cos(brush_tex_rotation);
  float s = sin(brush_tex_rotation);
  float2 rotated = float2(
    local_pos.x * c - local_pos.y * s,
    local_pos.x * s + local_pos.y * c
  );

  /* Normalize to UV space [0, 1]. */
  float2 uv = rotated / brush_radius * 0.5 + 0.5;

  /* Apply texture offset and scale. */
  uv = uv * brush_tex_size + brush_tex_offset;

  return texture(brush_texture, uv);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Compute Function
 * \{ */

void main()
{
  /* Get pixel row index from workgroup ID. */
  uint row_index = gl_WorkGroupID.x;

  /* Bounds check. */
  if (row_index >= num_pixel_rows) {
    return;
  }

  /* Load pixel row data (2 uint4 per row). */
  uint row_data_index = row_index * 2u;
  uint4 row_part_a = pixel_rows[row_data_index];
  uint4 row_part_b = pixel_rows[row_data_index + 1u];

  /* Unpack barycentric coordinates from float bits. */
  float2 start_barycentric = float2(
    uintBitsToFloat(row_part_a.x),
    uintBitsToFloat(row_part_a.y)
  );

  /* Unpack image coordinates. */
  uint2 start_image_coord = row_part_a.zw;

  /* Unpack primitive index and pixel count. */
  uint uv_primitive_index = row_part_b.x;
  uint num_pixels = row_part_b.y;

  /* Calculate pixel position from barycentric coordinates. */
  float3 pixel_pos = calc_pixel_position(uv_primitive_index, start_barycentric);

  /* Calculate brush distance and falloff. */
  float distance = calc_brush_distance(pixel_pos, brush_location, falloff_shape);
  float falloff = calc_falloff(distance, brush_radius, hardness);

  /* Early exit if outside brush radius. */
  if (falloff <= 0.0) {
    return;
  }

  /* Sample brush texture (if available). */
  float4 tex_color = sample_brush_texture(pixel_pos, brush_location);

  /* Apply automasking (if available). */
  if (has_automask != 0) {
    falloff *= automask_factors[row_index];
  }

  /* Calculate final paint factor. */
  float factor = falloff * brush_strength;

  /* Apply brush texture influence. */
  factor *= tex_color.a;

  /* Process each pixel in the row. */
  for (uint i = 0u; i < num_pixels; i++) {
    int2 image_coord = int2(start_image_coord) + int2(int(i), 0);

    /* Bounds check for image. */
    if (any(lessThan(image_coord, int2(0))) ||
        any(greaterThanEqual(image_coord, image_size)))
    {
      continue;
    }

    /* Read current pixel color. */
    float4 current_color = imageLoad(target_image, image_coord);

    /* Calculate paint color. */
    float4 paint_color = brush_color * factor;
    paint_color.a *= brush_alpha;

    /* Apply brush texture color (multiply with brush color). */
    if (has_brush_texture != 0) {
      paint_color.rgb *= tex_color.rgb;
    }

    /* Apply texture sample bias. */
    paint_color = max(paint_color - texture_sample_bias, float4(0.0));

    /* Blend with current color. */
    float4 final_color = blend_colors(current_color, paint_color, blend_mode);

    /* Handle invert flag. */
    if (invert != 0) {
      final_color = mix(current_color, final_color, -1.0);
    }

    /* Write result. */
    imageStore(target_image, image_coord, final_color);
  }
}

/** \} */
