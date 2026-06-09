/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "DNA_brush_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_vector.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_assert.h"

#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "mask_canvas.hh"

#include <algorithm>
#include <cmath>

namespace blender::ed::sculpt_paint::mask {

/* -------------------------------------------------------------------- */
/** \name Session Management
 * \{ */

MaskCanvas &canvas_ensure(bContext &C, Object &ob)
{
  BLI_assert(ob.runtime != nullptr);
  BLI_assert(ob.runtime->sculpt_session != nullptr);
  
  SculptSession &ss = *ob.runtime->sculpt_session;
  if (!ss.mask_canvas) {
    ss.mask_canvas = std::make_unique<MaskCanvas>();
  }

  MaskCanvas &canvas = *ss.mask_canvas;

  if (!canvas.active) {
    ARegion *region = CTX_wm_region(&C);
    RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

    canvas.width = region->winx;
    canvas.height = region->winy;

    canvas.projviewobjmat = ED_view3d_ob_project_mat_get(rv3d, &ob);

    const float3x3 view_inv(float4x4(rv3d->viewinv));
    const float3 view_dir = math::normalize(math::transform_direction(view_inv, float3(0, 0, 1)));
    canvas.true_view_normal = math::normalize(
        math::transform_direction(ob.world_to_object(), view_dir));

    canvas.pixels.clear();
    canvas.pixels.resize(canvas.width * canvas.height, 0.0f);

    Paint *paint = BKE_paint_get_active_from_context(&C);
    Brush *brush = BKE_paint_brush(paint);
    canvas.use_front_faces_only = (brush->flag & BRUSH_FRONTFACE) != 0;

    canvas.active = true;
    canvas.texture_dirty = true;
  }

  return canvas;
}

void canvas_clear(MaskCanvas &canvas)
{
  if (canvas.active) {
    std::fill(canvas.pixels.begin(), canvas.pixels.end(), 0.0f);
    canvas.texture_dirty = true;
  }
}

void canvas_end(MaskCanvas &canvas)
{
  canvas.pixels.clear();
  canvas.active = false;
  if (canvas.texture) {
    GPU_texture_free(canvas.texture);
    canvas.texture = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Canvas Drawing Primitives
 * \{ */

/* Helper for scanline polygon rasterization.
 * Processes each scanline and calls the provided callback for pixel ranges. */
template<typename PixelFunc>
static void scanline_rasterize_polygon(const MaskCanvas &canvas,
                                       blender::Span<int2> points,
                                       PixelFunc pixel_func)
{
  if (points.size() < 3) {
    return;
  }

  int min_y = canvas.height;
  int max_y = -1;
  for (const int2 &p : points) {
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }

  min_y = std::clamp(min_y, 0, canvas.height - 1);
  max_y = std::clamp(max_y, 0, canvas.height - 1);

  for (int y = min_y; y <= max_y; y++) {
    Vector<int> intersections;
    for (int i = 0; i < points.size(); i++) {
      const int2 &p1 = points[i];
      const int2 &p2 = points[(i + 1) % points.size()];

      if ((p1.y <= y && p2.y > y) || (p2.y <= y && p1.y > y)) {
        const float x = p1.x + (float(y - p1.y) / (p2.y - p1.y)) * (p2.x - p1.x);
        intersections.append(int(x));
      }
    }

    std::sort(intersections.begin(), intersections.end());

    for (int i = 0; i < intersections.size(); i += 2) {
      if (i + 1 < intersections.size()) {
        const int x_start = std::max(0, intersections[i]);
        const int x_end = std::min(canvas.width - 1, intersections[i + 1]);
        for (int x = x_start; x <= x_end; x++) {
          pixel_func(x, y);
        }
      }
    }
  }
}

void canvas_draw_circle(MaskCanvas &canvas,
                        float px,
                        float py,
                        float radius_px,
                        float value,
                        float hardness)
{
  if (!canvas.active || radius_px <= 0.0f || value == 0.0f) {
    return;
  }

  /* Bounding box of the circle in pixels. */
  const int xmin = std::max(0, int(std::floor(px - radius_px)));
  const int ymin = std::max(0, int(std::floor(py - radius_px)));
  const int xmax = std::min(canvas.width - 1, int(std::ceil(px + radius_px)));
  const int ymax = std::min(canvas.height - 1, int(std::ceil(py + radius_px)));

  const float radius_sq = radius_px * radius_px;
  const float hard_factor = std::max(hardness * 2.0f, 1.0f);

  for (int y = ymin; y <= ymax; y++) {
    for (int x = xmin; x <= xmax; x++) {
      const float dx = float(x) - px;
      const float dy = float(y) - py;
      const float dist_sq = dx * dx + dy * dy;

      if (dist_sq <= radius_sq) {
        /* Smooth radial falloff */
        const float falloff = std::pow(1.0f - (dist_sq / radius_sq), hard_factor);
        const float draw_val = value * falloff;

        const int index = y * canvas.width + x;
        float &pixel = canvas.pixels[index];

        if (value > 0.0f) {
          pixel = std::max(pixel, draw_val);
        }
        else {
          pixel = std::clamp(pixel + draw_val, 0.0f, 1.0f);
        }
      }
    }
  }
  canvas.texture_dirty = true;
}

void canvas_fill_polygon(MaskCanvas &canvas,
                         blender::Span<int2> points,
                         float value)
{
  if (!canvas.active || points.size() < 3 || value == 0.0f) {
    return;
  }

  scanline_rasterize_polygon(canvas, points, [&](int x, int y) {
    const int index = y * canvas.width + x;
    float &pixel = canvas.pixels[index];
    pixel = std::clamp(pixel + value, 0.0f, 1.0f);
  });

  canvas.texture_dirty = true;
}

void canvas_fill_rect(MaskCanvas &canvas, int xmin, int ymin, int xmax, int ymax, float value)
{
  if (!canvas.active || value == 0.0f) {
    return;
  }

  xmin = std::clamp(xmin, 0, canvas.width - 1);
  ymin = std::clamp(ymin, 0, canvas.height - 1);
  xmax = std::clamp(xmax, 0, canvas.width - 1);
  ymax = std::clamp(ymax, 0, canvas.height - 1);

  for (int y = ymin; y <= ymax; y++) {
    for (int x = xmin; x <= xmax; x++) {
      const int index = y * canvas.width + x;
      float &pixel = canvas.pixels[index];
      pixel = std::clamp(pixel + value, 0.0f, 1.0f);
    }
  }
  canvas.texture_dirty = true;
}

static void canvas_set_pixels_in_range(MaskCanvas &canvas,
                                       int xmin,
                                       int ymin,
                                       int xmax,
                                       int ymax,
                                       const float value)
{
  xmin = std::clamp(xmin, 0, canvas.width - 1);
  ymin = std::clamp(ymin, 0, canvas.height - 1);
  xmax = std::clamp(xmax, 0, canvas.width - 1);
  ymax = std::clamp(ymax, 0, canvas.height - 1);

  const float clamped_value = std::clamp(value, 0.0f, 1.0f);
  for (int y = ymin; y <= ymax; y++) {
    for (int x = xmin; x <= xmax; x++) {
      canvas.pixels[y * canvas.width + x] = clamped_value;
    }
  }
}

void canvas_set_rect(MaskCanvas &canvas, int xmin, int ymin, int xmax, int ymax, float value)
{
  if (!canvas.active) {
    return;
  }
  canvas_set_pixels_in_range(canvas, xmin, ymin, xmax, ymax, value);
  canvas.texture_dirty = true;
}

void canvas_set_polygon(MaskCanvas &canvas, blender::Span<int2> points, float value)
{
  if (!canvas.active || points.size() < 3) {
    return;
  }

  const float clamped_value = std::clamp(value, 0.0f, 1.0f);

  scanline_rasterize_polygon(canvas, points, [&](int x, int y) {
    canvas.pixels[y * canvas.width + x] = clamped_value;
  });

  canvas.texture_dirty = true;
}

void canvas_set_line_halfplane(MaskCanvas &canvas,
                               const float2 &p0,
                               const float2 &p1,
                               const bool flip,
                               const bool limit_to_segment,
                               const float value)
{
  if (!canvas.active) {
    return;
  }

  const float2 dir = p1 - p0;
  const float len_sq = math::dot(dir, dir);
  if (len_sq < 1e-6f) {
    return;
  }

  const float clamped_value = std::clamp(value, 0.0f, 1.0f);

  for (int y = 0; y < canvas.height; y++) {
    for (int x = 0; x < canvas.width; x++) {
      const float2 p(float(x) + 0.5f, float(y) + 0.5f);
      const float2 vp = p - p0;
      const float cross = dir.x * vp.y - dir.y * vp.x;
      bool fill = (cross > 0.0f) ^ flip;

      if (limit_to_segment) {
        const float t = math::dot(vp, dir) / len_sq;
        if (t < 0.0f || t > 1.0f) {
          fill = false;
        }
      }

      if (fill) {
        canvas.pixels[y * canvas.width + x] = clamped_value;
      }
    }
  }
  canvas.texture_dirty = true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Canvas Sampling
 * \{ */

std::optional<float2> canvas_project_co(const MaskCanvas &canvas, const float3 &co)
{
  if (!canvas.active) {
    return std::nullopt;
  }

  float4 vec4(co.x, co.y, co.z, 1.0f);
  vec4 = canvas.projviewobjmat * vec4;

  if (vec4.w <= FLT_EPSILON) {
    return std::nullopt;
  }

  return float2(float(canvas.width) / 2.0f + (float(canvas.width) / 2.0f) * vec4.x / vec4.w,
                float(canvas.height) / 2.0f + (float(canvas.height) / 2.0f) * vec4.y / vec4.w);
}

float canvas_sample(const MaskCanvas &canvas, float px, float py)
{
  if (!canvas.active) {
    return 0.0f;
  }

  /* Basic bilinear interpolation */
  const int x0 = int(std::floor(px));
  const int y0 = int(std::floor(py));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  /* Clamp coordinates to valid range */
  if (x0 < 0 || x0 >= canvas.width || y0 < 0 || y0 >= canvas.height) {
    return 0.0f; /* Out of bounds */
  }

  /* Handle edge cases by clamping to last valid pixel */
  const int x1_clamped = std::min(x1, canvas.width - 1);
  const int y1_clamped = std::min(y1, canvas.height - 1);

  const float fx = px - x0;
  const float fy = py - y0;

  const float v00 = canvas.pixels[y0 * canvas.width + x0];
  const float v10 = canvas.pixels[y0 * canvas.width + x1_clamped];
  const float v01 = canvas.pixels[y1_clamped * canvas.width + x0];
  const float v11 = canvas.pixels[y1_clamped * canvas.width + x1_clamped];

  const float v0 = v00 * (1.0f - fx) + v10 * fx;
  const float v1 = v01 * (1.0f - fx) + v11 * fx;

  return v0 * (1.0f - fy) + v1 * fy;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Canvas Overlay
 * \{ */

static void canvas_overlay_update_texture(MaskCanvas &canvas)
{
  if (canvas.texture) {
    GPU_texture_free(canvas.texture);
    canvas.texture = nullptr;
  }

  const int num_pixels = canvas.width * canvas.height;
  Array<uchar> uchar_buf(num_pixels);
  for (const int i : IndexRange(num_pixels)) {
    uchar_buf[i] = uchar(canvas.pixels[i] * 255.0f);
  }

  canvas.texture = GPU_texture_create_2d("mask_canvas",
                                       canvas.width,
                                       canvas.height,
                                       1,
                                       gpu::TextureFormat::UNORM_8,
                                       GPU_TEXTURE_USAGE_SHADER_READ,
                                       nullptr);
  if (!canvas.texture) {
    return;
  }

  GPU_texture_update(canvas.texture, GPU_DATA_UBYTE, uchar_buf.data());
  GPU_texture_swizzle_set(canvas.texture, "rrrr");
  canvas.texture_dirty = false;
}

void canvas_draw_overlay(MaskCanvas &canvas, ARegion *region)
{
  if (!canvas.active || canvas.width <= 0 || canvas.height <= 0) {
    return;
  }

  if (canvas.texture_dirty || !canvas.texture) {
    canvas_overlay_update_texture(canvas);
  }

  if (!canvas.texture) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA_PREMULT);
  GPU_depth_test(GPU_DEPTH_NONE);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  const uint tex_coord = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immUniformColor4f(1.0f, 0.0f, 0.0f, 0.5f);

  immBindTextureSampler("image",
                        canvas.texture,
                        {GPU_SAMPLER_FILTERING_LINEAR,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});

  const float xmax = float(region->winx);
  const float ymax = float(region->winy);

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(tex_coord, 0.0f, 0.0f);
  immVertex2f(pos, 0.0f, 0.0f);

  immAttr2f(tex_coord, 1.0f, 0.0f);
  immVertex2f(pos, xmax, 0.0f);

  immAttr2f(tex_coord, 1.0f, 1.0f);
  immVertex2f(pos, xmax, ymax);

  immAttr2f(tex_coord, 0.0f, 1.0f);
  immVertex2f(pos, 0.0f, ymax);
  immEnd();

  immUnbindProgram();
  GPU_texture_unbind(canvas.texture);
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::mask
