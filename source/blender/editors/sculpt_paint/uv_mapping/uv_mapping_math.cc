/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "uv_mapping_math.hh"

#include <algorithm>
#include <cmath>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "BKE_paint_layers.hh"

namespace blender::ed::sculpt_paint::uv_mapping {

namespace {

/** Largest scale magnitude a drag may reach; keeps a handle dragged onto the pivot finite. */
constexpr float SCALE_MAX = 1000.0f;

/** Smallest magnitude of a drag factor `k`: `s' = s / k` must not blow up at the pivot. */
constexpr float DRAG_FACTOR_MIN = 1.0f / SCALE_MAX;

float scale_axis_limit(const float value)
{
  if (!std::isfinite(value) || value == 0.0f) {
    return 1.0f;
  }
  return std::copysign(std::clamp(std::fabs(value), PAINT_LAYER_MAPPING_SCALE_MIN, SCALE_MAX),
                       value);
}

/** \a k with its magnitude kept above #DRAG_FACTOR_MIN; the sign (a mirror) is preserved. */
float drag_factor_limit(const float k)
{
  if (!std::isfinite(k)) {
    return 1.0f;
  }
  return std::fabs(k) < DRAG_FACTOR_MIN ? std::copysign(DRAG_FACTOR_MIN, k) : k;
}

/** Offset that keeps \a pivot_uv fixed while the linear part changes from \a before to
 * \a after. */
float2 offset_keeping_pivot(const UvMappingXform &start,
                            const float3x3 &before,
                            const float3x3 &after,
                            const float2 &pivot_uv)
{
  return start.offset + math::transform_point(before, pivot_uv) -
         math::transform_point(after, pivot_uv);
}

}  // namespace

float3x3 uv_mapping_sample_matrix(const UvMappingXform &xform)
{
  const float c = std::cos(xform.rotation);
  const float s = std::sin(xform.rotation);
  /* `scale * R`: rotate the canvas UV, then scale along the canvas axes. This is the order the
   * generated Mapping chain and the CPU resample use, and it keeps the footprint rectangular. */
  return float3x3(float3(c * xform.scale.x, s * xform.scale.y, 0.0f),
                  float3(-s * xform.scale.x, c * xform.scale.y, 0.0f),
                  float3(0.0f, 0.0f, 1.0f));
}

float3x3 uv_mapping_footprint_matrix(const UvMappingXform &xform)
{
  /* Element names use row/column indices: a11 is row 0, column 0. */
  const float a11 = xform.scale.x * std::cos(xform.rotation);
  const float a21 = xform.scale.y * std::sin(xform.rotation);
  const float a12 = -xform.scale.x * std::sin(xform.rotation);
  const float a22 = xform.scale.y * std::cos(xform.rotation);
  const float det = a11 * a22 - a12 * a21;
  if (!(std::fabs(det) > 1e-12f)) {
    return float3x3::identity();
  }
  /* A^-1 = [[a22, -a12], [-a21, a11]] / det, stored by columns. */
  return float3x3(float3(a22 / det, -a21 / det, 0.0f),
                  float3(-a12 / det, a11 / det, 0.0f),
                  float3(0.0f, 0.0f, 1.0f));
}

float2 uv_mapping_map_point(const UvMappingXform &xform, const float2 &canvas_uv)
{
  return math::transform_point(uv_mapping_sample_matrix(xform), canvas_uv) + xform.offset;
}

float2 uv_mapping_footprint_point(const UvMappingXform &xform, const float2 &map_uv)
{
  return math::transform_point(uv_mapping_footprint_matrix(xform), map_uv - xform.offset);
}

float2 uv_mapping_footprint_center(const UvMappingXform &xform)
{
  return uv_mapping_footprint_point(xform, float2(0.5f));
}

UvMappingXform uv_mapping_drag_move(const UvMappingXform &start, const float2 &delta_uv)
{
  UvMappingXform result = start;
  /* The offset is added after the transform, so moving the footprint by delta needs the
   * offset to move by -A * delta. */
  result.offset = start.offset - math::transform_point(uv_mapping_sample_matrix(start), delta_uv);
  return result;
}

UvMappingXform uv_mapping_drag_rotate(const UvMappingXform &start,
                                      const float2 &pivot_uv,
                                      const float delta_angle)
{
  UvMappingXform result = start;
  result.rotation = start.rotation - delta_angle;
  result.offset = offset_keeping_pivot(start,
                                       uv_mapping_sample_matrix(start),
                                       uv_mapping_sample_matrix(result),
                                       pivot_uv);
  return result;
}

UvMappingXform uv_mapping_drag_scale(const UvMappingXform &start,
                                     const float2 &pivot_uv,
                                     const float2 &mouse_start_uv,
                                     const float2 &mouse_uv,
                                     const UvMappingScaleAxes &axes,
                                     const bool uniform)
{
  UvMappingXform result = start;
  const float3x3 before = uv_mapping_sample_matrix(start);
  /* Measure the drag in the cage's own axes: the cage is drawn as a rectangle rotated by
   * -rotation, so projecting onto the rotation axes makes a corner follow the mouse and keeps a
   * side handle on its own axis, whatever the current scale. */
  const float c = std::cos(start.rotation);
  const float s = std::sin(start.rotation);
  const float2 start_delta = mouse_start_uv - pivot_uv;
  const float2 now_delta = mouse_uv - pivot_uv;
  const float2 q0(c * start_delta.x - s * start_delta.y,
                  s * start_delta.x + c * start_delta.y);
  const float2 q(c * now_delta.x - s * now_delta.y, s * now_delta.x + c * now_delta.y);

  /* The footprint grows as the mapping scale shrinks: s' = s / k. A zero reference axis
   * (an edge handle on its other axis, or a press exactly on the pivot) keeps its scale. */
  constexpr float q_epsilon = 1e-6f;
  auto axis_k = [](const float q0_axis, const float q_axis) {
    return (std::fabs(q0_axis) > q_epsilon) ? q_axis / q0_axis : 1.0f;
  };

  const float kx = axis_k(q0.x, q.x);
  const float ky = axis_k(q0.y, q.y);

  if (uniform) {
    /* One factor for both axes, from how far the mouse moved along the start direction. This
     * keeps the dragged handle on the pivot-to-handle line instead of letting one axis lag. */
    const float2 q0_axis = float2(axes.x ? q0.x : 0.0f, axes.y ? q0.y : 0.0f);
    const float2 q_axis = float2(axes.x ? q.x : 0.0f, axes.y ? q.y : 0.0f);
    const float q0_len_sq = math::length_squared(q0_axis);
    const float k = (q0_len_sq > 1e-12f) ? math::dot(q_axis, q0_axis) / q0_len_sq : 1.0f;
    const float lead_scale = axes.x ? start.scale.x : start.scale.y;
    result.scale = float2(scale_axis_limit(lead_scale / drag_factor_limit(k)));
  }
  else {
    if (axes.x) {
      result.scale.x = scale_axis_limit(start.scale.x / drag_factor_limit(kx));
    }
    if (axes.y) {
      result.scale.y = scale_axis_limit(start.scale.y / drag_factor_limit(ky));
    }
  }

  result.offset = offset_keeping_pivot(
      start, before, uv_mapping_sample_matrix(result), pivot_uv);
  return result;
}

}  // namespace blender::ed::sculpt_paint::uv_mapping
