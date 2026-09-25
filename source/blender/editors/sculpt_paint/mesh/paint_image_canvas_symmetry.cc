/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Canvas-space symmetry of 2D texture painting, see #ED_image_paint_symmetry.hh.
 */

#include <algorithm>
#include <cmath>

#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "DNA_image_types.h"


#include "ED_image_paint_symmetry.hh"

namespace blender::ed::image_paint_symmetry {

/** Below this distance from the inversion center a point has no finite image. */
static constexpr float CIRCLE_CENTER_EPSILON = 1e-6f;
/** Longest polygon edge mapped through the inversion without subdivision, in UV. */
static constexpr float CIRCLE_SUBDIVIDE_LENGTH = 1.0f / 256.0f;
/** Cap on the subdivided polygon, so a huge lasso cannot explode the vertex count. */
static constexpr int CIRCLE_SUBDIVIDE_MAX_POINTS = 8192;
/** Brush dab magnification range of the circle inversion, see #CanvasSymmetry::dab_scale_at. */
static constexpr float DAB_SCALE_MIN = 0.05f;
static constexpr float DAB_SCALE_MAX = 20.0f;

float2 CanvasSymmetry::direction() const
{
  return float2(cosf(this->angle), sinf(this->angle));
}

float2 CanvasSymmetry::normal() const
{
  return float2(-sinf(this->angle), cosf(this->angle));
}

int CanvasSymmetry::copies_num() const
{
  switch (this->type) {
    case IMAGE_PAINT_SYMMETRY_TYPE_LINE:
    case IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE:
      return 1;
    case IMAGE_PAINT_SYMMETRY_TYPE_PARALLEL:
      return 2 * std::max(this->parallel_count, 1);
  }
  return 0;
}

float2 CanvasSymmetry::apply(const int copy, const float2 &uv) const
{
  const float2 rel = uv - this->pivot;
  switch (this->type) {
    case IMAGE_PAINT_SYMMETRY_TYPE_LINE: {
      /* Householder reflection across the line through the pivot. */
      const float2 dir = this->direction();
      return this->pivot + dir * (2.0f * math::dot(rel, dir)) - rel;
    }
    case IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE: {
      const float len_sq = math::length_squared(rel);
      if (len_sq < CIRCLE_CENTER_EPSILON * CIRCLE_CENTER_EPSILON) {
        return uv;
      }
      return this->pivot + rel * (this->radius * this->radius / len_sq);
    }
    case IMAGE_PAINT_SYMMETRY_TYPE_PARALLEL: {
      /* Copies alternate sides: +1, -1, +2, -2, ... */
      const int step = copy / 2 + 1;
      const float side = (copy % 2 == 0) ? 1.0f : -1.0f;
      return uv + this->normal() * (side * float(step) * this->width);
    }
  }
  return uv;
}

float CanvasSymmetry::scale_at(const float2 &uv) const
{
  if (this->type != IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE) {
    return 1.0f;
  }
  /* The inversion is conformal with local scale `r^2 / d^2`. */
  const float len_sq = std::max(math::length_squared(uv - this->pivot),
                                CIRCLE_CENTER_EPSILON * CIRCLE_CENTER_EPSILON);
  return this->radius * this->radius / len_sq;
}

float CanvasSymmetry::dab_scale_at(const float2 &uv) const
{
  return std::clamp(this->scale_at(uv), DAB_SCALE_MIN, DAB_SCALE_MAX);
}

/** Reflection across the line through the origin with unit direction \a d. */
static float2x2 reflection_matrix(const float2 &d)
{
  /* `2 * d * d^T - I`, symmetric so the column order does not matter. */
  return float2x2(float2(2.0f * d.x * d.x - 1.0f, 2.0f * d.x * d.y),
                  float2(2.0f * d.x * d.y, 2.0f * d.y * d.y - 1.0f));
}

float2x2 CanvasSymmetry::jacobian(const float2 &uv) const
{
  switch (this->type) {
    case IMAGE_PAINT_SYMMETRY_TYPE_LINE:
      return reflection_matrix(this->direction());
    case IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE: {
      /* The inversion reflects across the tangent of the circle through `uv` (the radial
       * direction flips) and scales by `r^2 / d^2`. */
      const float2 rel = uv - this->pivot;
      const float len = math::length(rel);
      if (len < CIRCLE_CENTER_EPSILON) {
        return float2x2::identity();
      }
      const float2 tangent(-rel.y / len, rel.x / len);
      return reflection_matrix(tangent) * this->scale_at(uv);
    }
    case IMAGE_PAINT_SYMMETRY_TYPE_PARALLEL:
      return float2x2::identity();
  }
  return float2x2::identity();
}

bool CanvasSymmetry::is_reflection() const
{
  return this->type != IMAGE_PAINT_SYMMETRY_TYPE_PARALLEL;
}

CanvasSymmetry from_settings_unconditional(const ImagePaintSettings &settings)
{
  CanvasSymmetry symmetry;
  symmetry.type = eImagePaint_SymmetryType(settings.symmetry_type);
  symmetry.pivot = float2(settings.symmetry_line_pivot[0], settings.symmetry_line_pivot[1]);
  symmetry.angle = settings.symmetry_line_angle;
  symmetry.radius = std::max(settings.symmetry_circle_radius, 1e-4f);
  symmetry.width = std::max(settings.symmetry_parallel_width, 1e-4f);
  symmetry.parallel_count = std::max(int(settings.symmetry_parallel_count), 1);
  return symmetry;
}

bool canvas_mode_active(const ToolSettings &tool_settings)
{
  return tool_settings.imapaint.symmetry_line_flag & IMAGE_PAINT_SYMMETRY_MODE_CANVAS;
}

std::optional<CanvasSymmetry> from_settings(const ToolSettings &tool_settings,
                                            const eImagePaint_SymmetryLineFlag affect)
{
  const ImagePaintSettings &settings = tool_settings.imapaint;
  if (!canvas_mode_active(tool_settings) ||
      !(settings.symmetry_line_flag & IMAGE_PAINT_SYMMETRY_LINE_ENABLED) ||
      !(settings.symmetry_line_flag & affect))
  {
    return std::nullopt;
  }
  return from_settings_unconditional(settings);
}

/** Even-odd point in polygon test, used to reject polygons around the inversion center. */
static bool polygon_contains(const Span<float2> polygon, const float2 &p)
{
  bool inside = false;
  for (const int i : polygon.index_range()) {
    const float2 &a = polygon[i];
    const float2 &b = polygon[(i + 1) % polygon.size()];
    if ((a.y > p.y) != (b.y > p.y)) {
      const float x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
      if (p.x < x) {
        inside = !inside;
      }
    }
  }
  return inside;
}

Vector<float2> apply_polygon(const CanvasSymmetry &symmetry,
                             const int copy,
                             const Span<float2> uv_polygon)
{
  Vector<float2> result;
  if (symmetry.type != IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE) {
    result.reserve(uv_polygon.size());
    for (const float2 &uv : uv_polygon) {
      result.append(symmetry.apply(copy, uv));
    }
    return result;
  }

  /* The image of a region around the center is the unbounded outside of a curve. */
  if (polygon_contains(uv_polygon, symmetry.pivot)) {
    return result;
  }
  float perimeter = 0.0f;
  for (const int i : uv_polygon.index_range()) {
    perimeter += math::distance(uv_polygon[i], uv_polygon[(i + 1) % uv_polygon.size()]);
  }
  const float step = std::max(CIRCLE_SUBDIVIDE_LENGTH,
                              perimeter / float(CIRCLE_SUBDIVIDE_MAX_POINTS));
  for (const int i : uv_polygon.index_range()) {
    const float2 &a = uv_polygon[i];
    const float2 &b = uv_polygon[(i + 1) % uv_polygon.size()];
    const int segments = std::max(1, int(std::ceil(math::distance(a, b) / step)));
    for (const int s : IndexRange(segments)) {
      result.append(symmetry.apply(copy, math::interpolate(a, b, float(s) / float(segments))));
    }
  }
  return result;
}

float2 active_tile_origin(const Image *ima)
{
  /* Non-tiled images always resolve to the base tile; `sima->iuser.tile` is not reliable there. */
  int tile_number = 1001;
  if (ima && ima->source == IMA_SRC_TILED) {
    const ImageTile *tile = static_cast<const ImageTile *>(
        BLI_findlink(&ima->tiles, ima->active_tile_index));
    if (tile) {
      tile_number = tile->tile_number;
    }
  }
  return float2(float((tile_number - 1001) % 10), float((tile_number - 1001) / 10));
}

bool line_clip_to_tile(const float2 &pivot,
                       const float2 &direction,
                       const float2 &tile_origin,
                       float &r_t0,
                       float &r_t1)
{
  const float2 tile_max = tile_origin + float2(1.0f);
  float t0 = -1e9f, t1 = 1e9f;
  for (const int axis : IndexRange(2)) {
    if (std::abs(direction[axis]) > 1e-12f) {
      float ta = (tile_origin[axis] - pivot[axis]) / direction[axis];
      float tb = (tile_max[axis] - pivot[axis]) / direction[axis];
      if (ta > tb) {
        std::swap(ta, tb);
      }
      t0 = std::max(t0, ta);
      t1 = std::min(t1, tb);
    }
    else if (pivot[axis] < tile_origin[axis] || pivot[axis] > tile_max[axis]) {
      return false;
    }
  }
  if (t0 >= t1) {
    return false;
  }
  r_t0 = t0;
  r_t1 = t1;
  return true;
}

EditHandles edit_handles_calc(const ImagePaintSettings &settings, const float2 &tile_origin)
{
  const CanvasSymmetry symmetry = from_settings_unconditional(settings);
  EditHandles handles;
  handles.pivot = symmetry.pivot;

  if (symmetry.type == IMAGE_PAINT_SYMMETRY_TYPE_CIRCLE) {
    handles.extent = symmetry.pivot + symmetry.direction() * symmetry.radius;
    handles.has_extent = true;
    return handles;
  }

  const float2 dir = symmetry.direction();
  float t0, t1;
  if (line_clip_to_tile(symmetry.pivot, dir, tile_origin, t0, t1)) {
    const float length = std::clamp(settings.symmetry_line_length, 0.05f, 1.0f);
    handles.end_a = symmetry.pivot + dir * (t0 * length);
    handles.end_b = symmetry.pivot + dir * (t1 * length);
    handles.has_ends = true;
  }
  if (symmetry.type == IMAGE_PAINT_SYMMETRY_TYPE_PARALLEL) {
    handles.extent = symmetry.pivot + symmetry.normal() * symmetry.width;
    handles.has_extent = true;
  }
  return handles;
}

}  // namespace blender::ed::image_paint_symmetry
