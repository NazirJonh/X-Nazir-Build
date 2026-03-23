/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Paint a color made from hash of node pointer. */
// #define DEBUG_PIXEL_NODES

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "CLG_log.h"

#include "ED_paint.hh"

#include "BLI_bit_vector.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_color_blend.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_simd.hh"
#include "BLI_time.h"
#ifdef DEBUG_PIXEL_NODES
#  include "BLI_hash.h"
#endif

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_image_wrappers.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"

#include "../paint_gradient_core.hh"
#include "../paint_image_session_state.hh"
#include "../paint_intern.hh"

#include "ED_view3d.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_intern.hh"

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <type_traits>

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_task.h"
#include "BLI_task.hh"

namespace blender {

namespace ed::sculpt_paint::paint::image {

static CLG_LogRef LOG = {"sculpt.paint_image"};
static constexpr bool kEnableSimpleImageBrushDebugTelemetry = false;
static constexpr int64_t kSimpleImageBrushDebugPeriod = 8;

using namespace blender::bke::pbvh::pixels;
using namespace blender::bke::image;
using paint::image::ImageData;

struct SimpleImageBrushStepSettings {
  bool push_undo_tiles = false;
  bool apply_seam_fix = false;
  bool mark_image_dirty_now = false;
  int brush_size_px = 0;
  int canvas_longest_side_px = 0;
  int64_t tick_version = 1;
};

struct SimpleImageBrushPaintTelemetry {
  std::atomic<int64_t> rows_total = 0;
  std::atomic<int64_t> rows_rejected_by_brush_test = 0;
  std::atomic<int64_t> rows_rejected_by_spherical_cull = 0;
  std::atomic<int64_t> rows_rejected_by_zero_strength = 0;
  std::atomic<int64_t> rows_rejected_by_zero_texture = 0;
  std::atomic<int64_t> rows_painted_attempted = 0;
  std::atomic<int64_t> rows_painted_success = 0;
  std::atomic<int64_t> pre_kernel_time_us = 0;
  std::atomic<int64_t> kernel_time_us = 0;
  std::atomic<int64_t> pre_positions_time_us = 0;
  std::atomic<int64_t> pre_strength_time_us = 0;
  std::atomic<int64_t> pre_texture_time_us = 0;
  std::atomic<int64_t> brush_test_time_us = 0;
  std::atomic<int64_t> spherical_cull_time_us = 0;
  std::atomic<int64_t> ibuf_acquire_release_time_us = 0;

  int64_t rows_total_value() const
  {
    return rows_total.load(std::memory_order_relaxed);
  }

  int64_t rows_rejected_by_brush_test_value() const
  {
    return rows_rejected_by_brush_test.load(std::memory_order_relaxed);
  }

  int64_t rows_rejected_by_spherical_cull_value() const
  {
    return rows_rejected_by_spherical_cull.load(std::memory_order_relaxed);
  }

  int64_t rows_rejected_by_zero_strength_value() const
  {
    return rows_rejected_by_zero_strength.load(std::memory_order_relaxed);
  }

  int64_t rows_rejected_by_zero_texture_value() const
  {
    return rows_rejected_by_zero_texture.load(std::memory_order_relaxed);
  }

  int64_t rows_painted_attempted_value() const
  {
    return rows_painted_attempted.load(std::memory_order_relaxed);
  }

  int64_t rows_painted_success_value() const
  {
    return rows_painted_success.load(std::memory_order_relaxed);
  }

  int64_t pre_kernel_time_us_value() const
  {
    return pre_kernel_time_us.load(std::memory_order_relaxed);
  }

  int64_t kernel_time_us_value() const
  {
    return kernel_time_us.load(std::memory_order_relaxed);
  }

  int64_t pre_positions_time_us_value() const
  {
    return pre_positions_time_us.load(std::memory_order_relaxed);
  }

  int64_t pre_strength_time_us_value() const
  {
    return pre_strength_time_us.load(std::memory_order_relaxed);
  }

  int64_t pre_texture_time_us_value() const
  {
    return pre_texture_time_us.load(std::memory_order_relaxed);
  }

  int64_t brush_test_time_us_value() const
  {
    return brush_test_time_us.load(std::memory_order_relaxed);
  }

  int64_t spherical_cull_time_us_value() const
  {
    return spherical_cull_time_us.load(std::memory_order_relaxed);
  }

  int64_t ibuf_acquire_release_time_us_value() const
  {
    return ibuf_acquire_release_time_us.load(std::memory_order_relaxed);
  }
};

static inline void telemetry_counter_inc(std::atomic<int64_t> &counter)
{
  counter.fetch_add(1, std::memory_order_relaxed);
}

static inline void telemetry_counter_add(std::atomic<int64_t> &counter, const int64_t value)
{
  counter.fetch_add(value, std::memory_order_relaxed);
}

static SimpleImageBrushStepSettings make_simple_image_brush_step_settings_begin(
    const Sculpt &sd,
    const Brush &brush,
    const SculptSession &ss,
    const int canvas_longest_side_px)
{
  SimpleImageBrushStepSettings settings;
  settings.push_undo_tiles =
      ed::sculpt_paint::image::session::should_push_simple_image_brush_undo_step();
  settings.brush_size_px = brush.size;
  settings.canvas_longest_side_px = canvas_longest_side_px;
  settings.tick_version = ss.cache != nullptr ? int64_t(ss.cache->iteration_count) : 1;
  return settings;
}

static void update_simple_image_brush_step_settings_post_paint(
    SimpleImageBrushStepSettings &io_settings, const bool had_updates)
{
  io_settings.apply_seam_fix =
      ed::sculpt_paint::image::session::should_apply_simple_image_brush_seam_fix_step(
          had_updates,
          io_settings.brush_size_px,
          io_settings.tick_version,
          io_settings.canvas_longest_side_px);
  io_settings.mark_image_dirty_now =
      ed::sculpt_paint::image::session::should_mark_simple_image_brush_dirty_step(
          had_updates,
          io_settings.tick_version,
          io_settings.canvas_longest_side_px,
          io_settings.brush_size_px);
}

static void debug_log_simple_image_brush_step(
    const SimpleImageBrushStepSettings &settings,
    const int64_t node_mask_size,
    const int64_t dirty_node_count,
    const bool had_updates,
    const double total_ms,
    const double undo_ms,
    const double paint_ms,
    const double collect_dirty_ms,
    const double seam_ms,
    const double mark_dirty_ms,
    const SimpleImageBrushPaintTelemetry &paint_telemetry)
{
  if constexpr (!kEnableSimpleImageBrushDebugTelemetry) {
    return;
  }

  const int64_t tick = std::max<int64_t>(settings.tick_version, 1);
  if ((tick % kSimpleImageBrushDebugPeriod) != 0) {
    return;
  }

  CLOG_INFO(
      &LOG,
      "simple_image_brush tick=%lld size_px=%d mask=%lld dirty=%lld updates=%d undo=%d seam=%d "
      "mark_dirty=%d rows_total=%lld rej_brush=%lld rej_sphere=%lld rej_strength=%lld "
      "rej_texture=%lld paint_attempt=%lld paint_success=%lld pre_kernel_ms=%.3f "
      "paint_kernel_ms=%.3f pre_pos_ms=%.3f pre_strength_ms=%.3f pre_tex_ms=%.3f "
      "brush_test_ms=%.3f sphere_cull_ms=%.3f ibuf_ms=%.3f total_ms=%.3f "
      "undo_ms=%.3f paint_ms=%.3f "
      "collect_ms=%.3f seam_ms=%.3f mark_ms=%.3f",
      static_cast<long long>(tick),
      settings.brush_size_px,
      static_cast<long long>(node_mask_size),
      static_cast<long long>(dirty_node_count),
      int(had_updates),
      int(settings.push_undo_tiles),
      int(settings.apply_seam_fix),
      int(settings.mark_image_dirty_now),
      static_cast<long long>(paint_telemetry.rows_total_value()),
      static_cast<long long>(paint_telemetry.rows_rejected_by_brush_test_value()),
      static_cast<long long>(paint_telemetry.rows_rejected_by_spherical_cull_value()),
      static_cast<long long>(paint_telemetry.rows_rejected_by_zero_strength_value()),
      static_cast<long long>(paint_telemetry.rows_rejected_by_zero_texture_value()),
      static_cast<long long>(paint_telemetry.rows_painted_attempted_value()),
      static_cast<long long>(paint_telemetry.rows_painted_success_value()),
      double(paint_telemetry.pre_kernel_time_us_value()) / 1000.0,
      double(paint_telemetry.kernel_time_us_value()) / 1000.0,
      double(paint_telemetry.pre_positions_time_us_value()) / 1000.0,
      double(paint_telemetry.pre_strength_time_us_value()) / 1000.0,
      double(paint_telemetry.pre_texture_time_us_value()) / 1000.0,
      double(paint_telemetry.brush_test_time_us_value()) / 1000.0,
      double(paint_telemetry.spherical_cull_time_us_value()) / 1000.0,
      double(paint_telemetry.ibuf_acquire_release_time_us_value()) / 1000.0,
      total_ms,
      undo_ms,
      paint_ms,
      collect_dirty_ms,
      seam_ms,
      mark_dirty_ms);
  std::fprintf(stderr,
               "[sculpt.paint_image] simple_image_brush tick=%lld size_px=%d mask=%lld dirty=%lld "
               "updates=%d undo=%d seam=%d mark_dirty=%d rows_total=%lld rej_brush=%lld "
               "rej_sphere=%lld rej_strength=%lld rej_texture=%lld paint_attempt=%lld "
               "paint_success=%lld pre_kernel_ms=%.3f paint_kernel_ms=%.3f pre_pos_ms=%.3f "
               "pre_strength_ms=%.3f pre_tex_ms=%.3f brush_test_ms=%.3f sphere_cull_ms=%.3f "
               "ibuf_ms=%.3f total_ms=%.3f "
               "undo_ms=%.3f paint_ms=%.3f collect_ms=%.3f seam_ms=%.3f mark_ms=%.3f\n",
               static_cast<long long>(tick),
               settings.brush_size_px,
               static_cast<long long>(node_mask_size),
               static_cast<long long>(dirty_node_count),
               int(had_updates),
               int(settings.push_undo_tiles),
               int(settings.apply_seam_fix),
               int(settings.mark_image_dirty_now),
               static_cast<long long>(paint_telemetry.rows_total_value()),
               static_cast<long long>(paint_telemetry.rows_rejected_by_brush_test_value()),
               static_cast<long long>(paint_telemetry.rows_rejected_by_spherical_cull_value()),
               static_cast<long long>(paint_telemetry.rows_rejected_by_zero_strength_value()),
               static_cast<long long>(paint_telemetry.rows_rejected_by_zero_texture_value()),
               static_cast<long long>(paint_telemetry.rows_painted_attempted_value()),
               static_cast<long long>(paint_telemetry.rows_painted_success_value()),
               double(paint_telemetry.pre_kernel_time_us_value()) / 1000.0,
               double(paint_telemetry.kernel_time_us_value()) / 1000.0,
               double(paint_telemetry.pre_positions_time_us_value()) / 1000.0,
               double(paint_telemetry.pre_strength_time_us_value()) / 1000.0,
               double(paint_telemetry.pre_texture_time_us_value()) / 1000.0,
               double(paint_telemetry.brush_test_time_us_value()) / 1000.0,
               double(paint_telemetry.spherical_cull_time_us_value()) / 1000.0,
               double(paint_telemetry.ibuf_acquire_release_time_us_value()) / 1000.0,
               total_ms,
               undo_ms,
               paint_ms,
               collect_dirty_ms,
               seam_ms,
               mark_dirty_ms);
  std::fflush(stderr);
}

/** Reading and writing to image buffer with 4 float channels. */
class ImageBufferFloat4 {
 private:
  int pixel_offset;

 public:
  void set_image_position(ImBuf *image_buffer, ushort2 image_pixel_position)
  {
    pixel_offset = int(image_pixel_position.y) * image_buffer->x + int(image_pixel_position.x);
  }

  void next_pixel()
  {
    pixel_offset += 1;
  }

  float4 read_pixel(ImBuf *image_buffer) const
  {
    return &image_buffer->float_buffer.data[pixel_offset * 4];
  }

  void write_pixel(ImBuf *image_buffer, const float4 pixel_data) const
  {
    copy_v4_v4(&image_buffer->float_buffer.data[pixel_offset * 4], pixel_data);
  }

  const char *get_colorspace_name(ImBuf *image_buffer)
  {
    return IMB_colormanagement_get_float_colorspace(image_buffer);
  }
};

/** Reading and writing to image buffer with 4 byte channels. */
class ImageBufferByte4 {
 private:
  int pixel_offset;

 public:
  void set_image_position(ImBuf *image_buffer, ushort2 image_pixel_position)
  {
    pixel_offset = int(image_pixel_position.y) * image_buffer->x + int(image_pixel_position.x);
  }

  void next_pixel()
  {
    pixel_offset += 1;
  }

  float4 read_pixel(ImBuf *image_buffer) const
  {
    float4 result;
    rgba_uchar_to_float(result,
                        static_cast<const uchar *>(static_cast<const void *>(
                            &(image_buffer->byte_buffer.data[4 * pixel_offset]))));
    return result;
  }

  void write_pixel(ImBuf *image_buffer, const float4 pixel_data) const
  {
    rgba_float_to_uchar(static_cast<uchar *>(static_cast<void *>(
                            &image_buffer->byte_buffer.data[4 * pixel_offset])),
                        pixel_data);
  }

  const char *get_colorspace_name(ImBuf *image_buffer)
  {
    return IMB_colormanagement_get_rect_colorspace(image_buffer);
  }
};

static float3 calc_pixel_position(const Span<float3> vert_positions,
                                  const Span<int3> vert_tris,
                                  const int tri_index,
                                  const float2 &barycentric_weight)
{
  const int3 &verts = vert_tris[tri_index];
  const float3 weights(barycentric_weight.x,
                       barycentric_weight.y,
                       1.0f - barycentric_weight.x - barycentric_weight.y);
  float3 result;
  interp_v3_v3v3v3(result,
                   vert_positions[verts[0]],
                   vert_positions[verts[1]],
                   vert_positions[verts[2]],
                   weights);
  return result;
}

struct PixelRowLinearPosition {
  float3 start;
  float3 delta;
};

static PixelRowLinearPosition calc_pixel_row_linear_position(
    const Span<float3> vert_positions,
    const Span<int3> vert_tris,
    const Span<int> tri_indices,
    const Span<float2> delta_barycentric_coords,
    const PackedPixelRow &pixel_row)
{
  const int tri_index = tri_indices[pixel_row.uv_primitive_index];
  const float2 delta_barycentric = delta_barycentric_coords[pixel_row.uv_primitive_index];
  const float3 start = calc_pixel_position(
      vert_positions, vert_tris, tri_index, pixel_row.start_barycentric_coord);
  const float3 next = calc_pixel_position(vert_positions,
                                          vert_tris,
                                          tri_index,
                                          pixel_row.start_barycentric_coord + delta_barycentric);
  return {start, next - start};
}

static void calc_pixel_row_positions(const Span<float3> vert_positions,
                                     const Span<int3> vert_tris,
                                     const Span<int> tri_indices,
                                     const Span<float2> delta_barycentric_coords,
                                     const PackedPixelRow &pixel_row,
                                     const MutableSpan<float3> positions)
{
  const PixelRowLinearPosition row_linear = calc_pixel_row_linear_position(
      vert_positions, vert_tris, tri_indices, delta_barycentric_coords, pixel_row);
  for (const int i : IndexRange(pixel_row.num_pixels)) {
    positions[i] = row_linear.start + row_linear.delta * i;
  }
}

/**
 * Sample the brush's color texture (brush->mask_mtex for Sculpt mode) for RGBA values.
 * Unlike mask texture, color texture affects the brush color rather than falloff.
 */
static void calc_brush_color_texture(const SculptSession &ss,
                                     const Brush &brush,
                                     const Span<float3> positions,
                                     const MutableSpan<float4> r_colors)
{
  BLI_assert(positions.size() == r_colors.size());

  const int thread_id = BLI_task_parallel_thread_id(nullptr);
  const MTex *mtex = BKE_brush_color_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    r_colors.fill(float4(1.0f, 1.0f, 1.0f, 1.0f));
    return;
  }

  const ed::sculpt_paint::StrokeCache &cache = *ss.cache;

  for (const int i : positions.index_range()) {
    float texture_value;
    float4 texture_rgba;

    float point[3];
    sub_v3_v3v3(point, positions[i], cache.plane_offset);

    if (mtex->brush_map_mode == MTEX_MAP_MODE_3D) {
      texture_value = BKE_brush_sample_tex_3d(
          cache.paint, &brush, mtex, point, texture_rgba, thread_id, ss.tex_pool);
    }
    else {
      if (cache.radial_symmetry_pass) {
        mul_m4_v3(cache.symm_rot_mat_inv.ptr(), point);
      }
      float3 symm_point = ed::sculpt_paint::symmetry_flip(point, cache.mirror_symmetry_pass);

      if (mtex->brush_map_mode == MTEX_MAP_MODE_AREA) {
        mul_m4_v3(cache.brush_local_mat.ptr(), symm_point);

        float x = symm_point[0];
        float y = symm_point[1];

        x *= mtex->size[0];
        y *= mtex->size[1];

        x += mtex->ofs[0];
        y += mtex->ofs[1];

        paint_get_tex_pixel(mtex, x, y, ss.tex_pool, thread_id, &texture_value, texture_rgba);

        add_v3_fl(texture_rgba, brush.texture_sample_bias);
        texture_value -= brush.texture_sample_bias;
      }
      else {
        const float2 point_2d = ED_view3d_project_float_v2_m4(
            cache.vc->region, symm_point, cache.projection_mat);
        const float point_3d[3] = {point_2d[0], point_2d[1], 0.0f};
        texture_value = BKE_brush_sample_tex_3d(
            cache.paint, &brush, mtex, point_3d, texture_rgba, thread_id, ss.tex_pool);
      }
    }

    /* Store texture color. Alpha channel contains the texture alpha for mask modulation. */
    r_colors[i] = texture_rgba;
  }
}

static void apply_hardness_to_squared_distances(const StrokeCache &cache,
                                                MutableSpan<float> distances)
{
  const float radius = cache.radius;
  const float radius_sq = radius * radius;
  const float hardness = cache.hardness;

  if (hardness == 0.0f) {
    for (const int i : distances.index_range()) {
      const float distance_sq = distances[i];
      distances[i] = (distance_sq >= radius_sq) ? radius : std::sqrt(distance_sq);
    }
  }
  else if (hardness == 1.0f) {
    for (const int i : distances.index_range()) {
      distances[i] = (distances[i] < radius_sq) ? 0.0f : radius;
    }
  }
  else {
    const float threshold = hardness * radius;
    const float threshold_sq = threshold * threshold;
    const float radius_inv = math::rcp(radius);
    const float hardness_inv_rcp = math::rcp(1.0f - hardness);

    for (const int i : distances.index_range()) {
      const float distance_sq = distances[i];
      if (distance_sq < threshold_sq) {
        distances[i] = 0.0f;
      }
      else if (distance_sq >= radius_sq) {
        distances[i] = radius;
      }
      else {
        const float distance = std::sqrt(distance_sq);
        const float radius_factor = (distance * radius_inv - hardness) * hardness_inv_rcp;
        distances[i] = radius_factor * radius;
      }
    }
  }
}

static void calc_brush_strength_distances_optimized(const SculptSession &ss,
                                                    const StrokeCache &cache,
                                                    const Brush &brush,
                                                    const eBrushFalloffShape falloff_shape,
                                                    const float bstrength,
                                                    const bool use_brush_texture,
                                                    const Span<float3> pixel_positions,
                                                    MutableSpan<float> distances,
                                                    MutableSpan<float> factors)
{
  BLI_assert(pixel_positions.size() == distances.size());
  BLI_assert(pixel_positions.size() == factors.size());

  /* Small rows are dominated by per-call overhead, so keep the simpler path there.
   * With brush texture enabled, fixed overhead is already higher, so switch earlier to the squared
   * distance path. */
  const int small_row_pixels_threshold = use_brush_texture ? 24 : 48;
  if (pixel_positions.size() <= small_row_pixels_threshold) {
    calc_brush_distances(ss, pixel_positions, falloff_shape, distances);
    apply_hardness_to_distances(cache, distances);
    factors.fill(1.0f);
    calc_brush_strength_factors(cache, brush, distances, factors);
    filter_distances_with_radius(cache.radius, distances, factors);
    scale_factors(factors, bstrength);
    return;
  }

  calc_brush_distances_squared(ss, pixel_positions, falloff_shape, distances);
  apply_hardness_to_squared_distances(cache, distances);

  factors.fill(1.0f);
  calc_brush_strength_factors(cache, brush, distances, factors);
  filter_distances_with_radius(cache.radius, distances, factors);
  scale_factors(factors, bstrength);
}

static void calc_brush_strength_distances_no_texture_row_linear(
    const SculptSession &ss,
    const StrokeCache &cache,
    const Brush &brush,
    const eBrushFalloffShape falloff_shape,
    const float bstrength,
    const PixelRowLinearPosition &row_linear,
    MutableSpan<float> distances,
    MutableSpan<float> factors)
{
  BLI_assert(distances.size() == factors.size());

  const float3 &test_location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  const int small_row_pixels_threshold = 48;
  const float3 delta = row_linear.delta;
  const float3 offset_start = row_linear.start - test_location;
  const float delta_squared = math::length_squared(delta);

  if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE && ss.cache != nullptr) {
    const float3 view_normal = math::normalize(ss.cache->view_normal_symm);
    const float offset_dot_n_delta = math::dot(delta, view_normal);
    float offset_squared = math::length_squared(offset_start);
    float offset_dot_delta = math::dot(offset_start, delta);
    float offset_dot_n = math::dot(offset_start, view_normal);

    if (distances.size() <= small_row_pixels_threshold) {
      for (const int i : distances.index_range()) {
        const float distance_sq = math::max(0.0f, offset_squared - offset_dot_n * offset_dot_n);
        distances[i] = std::sqrt(distance_sq);

        offset_squared += 2.0f * offset_dot_delta + delta_squared;
        offset_dot_delta += delta_squared;
        offset_dot_n += offset_dot_n_delta;
      }
      apply_hardness_to_distances(cache, distances);
    }
    else {
      for (const int i : distances.index_range()) {
        distances[i] = math::max(0.0f, offset_squared - offset_dot_n * offset_dot_n);

        offset_squared += 2.0f * offset_dot_delta + delta_squared;
        offset_dot_delta += delta_squared;
        offset_dot_n += offset_dot_n_delta;
      }
      apply_hardness_to_squared_distances(cache, distances);
    }
  }
  else {
    float offset_squared = math::length_squared(offset_start);
    float offset_dot_delta = math::dot(offset_start, delta);

    if (distances.size() <= small_row_pixels_threshold) {
      for (const int i : distances.index_range()) {
        distances[i] = std::sqrt(std::max(0.0f, offset_squared));

        offset_squared += 2.0f * offset_dot_delta + delta_squared;
        offset_dot_delta += delta_squared;
      }
      apply_hardness_to_distances(cache, distances);
    }
    else {
      for (const int i : distances.index_range()) {
        distances[i] = offset_squared;

        offset_squared += 2.0f * offset_dot_delta + delta_squared;
        offset_dot_delta += delta_squared;
      }
      apply_hardness_to_squared_distances(cache, distances);
    }
  }

  factors.fill(1.0f);
  calc_brush_strength_factors(cache, brush, distances, factors);
  filter_distances_with_radius(cache.radius, distances, factors);
  scale_factors(factors, bstrength);
}

template<typename ImageBuffer> class PaintingKernel {
  ImageBuffer image_accessor_;

  float4 brush_color_;
  float4 brush_color_linear_; /* Unconverted brush color for texture multiplication. */

  const char *last_used_color_space_ = nullptr;

 public:
  explicit PaintingKernel() = default;

  static bool find_positive_factor_bounds(const Span<float> factors,
                                          int &r_first_positive,
                                          int &r_last_positive)
  {
    r_first_positive = -1;
    r_last_positive = -1;
    for (const int i : factors.index_range()) {
      if (factors[i] > 0.0f) {
        if (r_first_positive == -1) {
          r_first_positive = i;
        }
        r_last_positive = i;
      }
    }
    return r_first_positive != -1;
  }

  /* For gradient tools, negative factors are valid (pixels "before" gradient start).
   * This method finds the bounds of all non-zero factors. */
  static bool find_nonzero_factor_bounds(const Span<float> factors,
                                         int &r_first_nonzero,
                                         int &r_last_nonzero)
  {
    r_first_nonzero = -1;
    r_last_nonzero = -1;
    for (const int i : factors.index_range()) {
      if (factors[i] != 0.0f) {
        if (r_first_nonzero == -1) {
          r_first_nonzero = i;
        }
        r_last_nonzero = i;
      }
    }
    return r_first_nonzero != -1;
  }

#if BLI_HAVE_SSE2
  bool paint_mix_float4_simd(const PackedPixelRow &pixel_row,
                             const Span<float> factors,
                             ImBuf *image_buffer,
                             const float brush_alpha,
                             const int first_positive,
                             const int last_positive,
                             const bool is_gradient_tool = false)
  {
    const int pixel_start_offset = int(pixel_row.start_image_coordinate.y) * image_buffer->x +
                                   int(pixel_row.start_image_coordinate.x) + first_positive;
    float *pixel_data = &image_buffer->float_buffer.data[pixel_start_offset * 4];

    __m128 brush_color = _mm_loadu_ps(&brush_color_[0]);
#  ifdef DEBUG_PIXEL_NODES
    if ((pixel_row.start_image_coordinate.y >> 3) & 1) {
      const __m128 rgb_debug_scale = _mm_setr_ps(0.5f, 0.5f, 0.5f, 1.0f);
      brush_color = _mm_mul_ps(brush_color, rgb_debug_scale);
    }
#  endif

    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 alpha = _mm_set1_ps(brush_alpha);

    bool pixels_painted = false;
    for (int x = first_positive; x <= last_positive; x++) {
      const float factor = factors[x];
      /* For gradient tools, negative factors are valid (pixels "before" gradient start).
       * Only skip exactly zero factors for gradient tools. For regular brushes, skip <= 0. */
      if (is_gradient_tool ? (factor == 0.0f) : (factor <= 0.0f)) {
        pixel_data += 4;
        continue;
      }

      const __m128 color = _mm_loadu_ps(pixel_data);
      /* For gradient tools, use absolute value since negative factors indicate position
       * before gradient start, not negative color intensity. */
      const float effective_factor = is_gradient_tool ? std::abs(factor) : factor;
      const __m128 factor_vec = _mm_set1_ps(effective_factor);
      const __m128 paint_color = _mm_mul_ps(brush_color, factor_vec);

      const __m128 paint_alpha = _mm_shuffle_ps(paint_color, paint_color, _MM_SHUFFLE(3, 3, 3, 3));
      const __m128 paint_alpha_inv = _mm_sub_ps(one, paint_alpha);
      __m128 buffer_color = _mm_add_ps(_mm_mul_ps(color, paint_alpha_inv), paint_color);

      buffer_color = _mm_mul_ps(buffer_color, alpha);

      const __m128 buffer_alpha = _mm_shuffle_ps(
          buffer_color, buffer_color, _MM_SHUFFLE(3, 3, 3, 3));
      const __m128 buffer_alpha_inv = _mm_sub_ps(one, buffer_alpha);
      const __m128 out_color = _mm_add_ps(_mm_mul_ps(color, buffer_alpha_inv), buffer_color);

      _mm_storeu_ps(pixel_data, out_color);
      pixels_painted = true;
      pixel_data += 4;
    }

    return pixels_painted;
  }
#endif

  bool paint(const Brush &brush,
             const PackedPixelRow &pixel_row,
             const Span<float> factors,
             ImBuf *image_buffer,
             const int first_positive,
             const int last_positive,
             const bool is_gradient_tool = false)
  {
    if (first_positive == -1) {
      return false;
    }

    const float brush_alpha = brush.alpha;
    const IMB_BlendMode blend_mode = static_cast<IMB_BlendMode>(brush.blend);

#if BLI_HAVE_SSE2
    if constexpr (std::is_same_v<ImageBuffer, ImageBufferFloat4>) {
      if (blend_mode == IMB_BLEND_MIX) {
        return paint_mix_float4_simd(
            pixel_row, factors, image_buffer, brush_alpha, first_positive, last_positive, is_gradient_tool);
      }
    }
#endif

    ushort2 image_start = pixel_row.start_image_coordinate;
    image_start.x = ushort(int(image_start.x) + first_positive);

    image_accessor_.set_image_position(image_buffer, image_start);
    bool pixels_painted = false;
    for (int x = first_positive; x <= last_positive; x++) {
      const float factor = factors[x];
      /* For gradient tools, negative factors are valid (pixels "before" gradient start).
       * Only skip exactly zero factors for gradient tools. For regular brushes, skip <= 0. */
      if (is_gradient_tool ? (factor == 0.0f) : (factor <= 0.0f)) {
        image_accessor_.next_pixel();
        continue;
      }

      float4 color = image_accessor_.read_pixel(image_buffer);
      /* For gradient tools, use absolute value since negative factors indicate position
       * before gradient start, not negative color intensity. */
      const float effective_factor = is_gradient_tool ? std::abs(factor) : factor;
      float4 paint_color = brush_color_ * effective_factor;
      float4 buffer_color;

#ifdef DEBUG_PIXEL_NODES
      if ((pixel_row.start_image_coordinate.y >> 3) & 1) {
        paint_color[0] *= 0.5f;
        paint_color[1] *= 0.5f;
        paint_color[2] *= 0.5f;
      }
#endif

      blend_color_mix_float(buffer_color, color, paint_color);
      buffer_color *= brush_alpha;
      IMB_blend_color_float(color, color, buffer_color, blend_mode);
      image_accessor_.write_pixel(image_buffer, color);
      pixels_painted = true;

      image_accessor_.next_pixel();
    }
    return pixels_painted;
  }

  bool paint(const Brush &brush,
             const PackedPixelRow &pixel_row,
             const Span<float> factors,
             ImBuf *image_buffer,
             const bool is_gradient_tool = false)
  {
    int first_positive;
    int last_positive;
    /* For gradient tools, find non-zero factors (including negative values for "before" pixels).
     * For regular brushes, only find positive factors. */
    if (is_gradient_tool) {
      if (!find_nonzero_factor_bounds(factors, first_positive, last_positive)) {
        return false;
      }
    }
    else {
      if (!find_positive_factor_bounds(factors, first_positive, last_positive)) {
        return false;
      }
    }
    return paint(brush, pixel_row, factors, image_buffer, first_positive, last_positive, is_gradient_tool);
  }

  /**
   * Paint with color texture support.
   * Matches the behavior of the old paint_image_proj.cc system.
   * The texture RGB multiplies brush color, texture alpha modulates the mask.
   */
  bool paint_with_texture_colors(const Brush &brush,
                                 const PackedPixelRow &pixel_row,
                                 const Span<float> factors,
                                 const Span<float4> texture_colors,
                                 ImBuf *image_buffer,
                                 const int first_positive,
                                 const int last_positive,
                                 const bool is_gradient_tool = false)
  {
    if (first_positive == -1) {
      return false;
    }

    const IMB_BlendMode blend_mode = static_cast<IMB_BlendMode>(brush.blend);

    /* Prepare color space converter if needed (for byte buffers). */
    ColormanageProcessor *cm_processor = nullptr;
    if (last_used_color_space_ != nullptr) {
      const char *from_colorspace = IMB_colormanagement_role_colorspace_name_get(
          COLOR_ROLE_SCENE_LINEAR);
      cm_processor = IMB_colormanagement_colorspace_processor_new(from_colorspace,
                                                                  last_used_color_space_);
    }

    ushort2 image_start = pixel_row.start_image_coordinate;
    image_start.x = ushort(int(image_start.x) + first_positive);

    image_accessor_.set_image_position(image_buffer, image_start);
    bool pixels_painted = false;
    for (int x = first_positive; x <= last_positive; x++) {
      const float factor = factors[x];
      /* For gradient tools, negative factors are valid (pixels "before" gradient start).
       * Only skip exactly zero factors for gradient tools. For regular brushes, skip <= 0. */
      if (is_gradient_tool ? (factor == 0.0f) : (factor <= 0.0f)) {
        image_accessor_.next_pixel();
        continue;
      }

      const float4 &tex_color = texture_colors[x];
      float4 dest_color = image_accessor_.read_pixel(image_buffer);

      /* Match old system (do_projectpaint_draw_f):
       * 1. Multiply brush color (linear) by texture RGB (linear) = result in linear
       * 2. Apply mask (factor includes texture alpha)
       * 3. Set alpha = mask
       * 4. Convert to image color space (for byte buffers)
       * 5. Blend with IMB_blend_color_float
       */
      float4 paint_color;

      /* Multiply brush color (linear) by texture RGB (linear). */
      paint_color[0] = brush_color_linear_[0] * tex_color[0];
      paint_color[1] = brush_color_linear_[1] * tex_color[1];
      paint_color[2] = brush_color_linear_[2] * tex_color[2];

      /* Apply mask (factor already includes brush strength, falloff, etc).
       * Texture alpha modulates the mask.
       * For gradient tools, use absolute value since negative factors indicate position
       * before gradient start, not negative color intensity. */
      const float effective_factor = is_gradient_tool ? std::abs(factor) : factor;
      const float mask = effective_factor * tex_color[3];
      paint_color[0] *= mask;
      paint_color[1] *= mask;
      paint_color[2] *= mask;
      paint_color[3] = mask;

      /* Convert from scene linear to image color space (for byte buffers). */
      if (cm_processor != nullptr) {
        IMB_colormanagement_processor_apply_v4(cm_processor, paint_color);
      }

#ifdef DEBUG_PIXEL_NODES
      if ((pixel_row.start_image_coordinate.y >> 3) & 1) {
        paint_color[0] *= 0.5f;
        paint_color[1] *= 0.5f;
        paint_color[2] *= 0.5f;
      }
#endif

      /* Blend directly - this matches the old system. */
      IMB_blend_color_float(dest_color, dest_color, paint_color, blend_mode);
      image_accessor_.write_pixel(image_buffer, dest_color);
      pixels_painted = true;

      image_accessor_.next_pixel();
    }

    if (cm_processor != nullptr) {
      IMB_colormanagement_processor_free(cm_processor);
    }

    return pixels_painted;
  }

  void init_brush_color(ImBuf *image_buffer, float in_brush_color[3])
  {
    const char *to_colorspace = image_accessor_.get_colorspace_name(image_buffer);
    if (last_used_color_space_ == to_colorspace) {
      return;
    }

    /* Store linear brush color for texture multiplication. */
    copy_v3_v3(brush_color_linear_, in_brush_color);
    brush_color_linear_[3] = 1.0f;

    /* Store converted brush color for direct blending. */
    copy_v3_v3(brush_color_, in_brush_color);
    brush_color_[3] = 1.0f;

    const char *from_colorspace = IMB_colormanagement_role_colorspace_name_get(
        COLOR_ROLE_SCENE_LINEAR);
    ColormanageProcessor *cm_processor = IMB_colormanagement_colorspace_processor_new(
        from_colorspace, to_colorspace);
    IMB_colormanagement_processor_apply_v4(cm_processor, brush_color_);
    IMB_colormanagement_processor_free(cm_processor);
    last_used_color_space_ = to_colorspace;
  }
};

static BitVector<> init_uv_primitives_brush_test(SculptSession &ss,
                                                 const Span<int3> vert_tris,
                                                 const Span<int> tri_indices,
                                                 const Span<float3> positions,
                                                 bool &r_has_any_true)
{
  const float3 location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  const float radius = ss.cache ? ss.cache->radius : ss.cursor_radius;
  const Bounds<float3> brush_bounds(location - radius, location + radius);

  BitVector<> brush_test(tri_indices.size());
  r_has_any_true = false;
  for (const int i : tri_indices.index_range()) {
    const int tri_index = tri_indices[i];
    const int3 verts = vert_tris[tri_index];

    Bounds<float3> tri_bounds(positions[verts[0]]);
    math::min_max(positions[verts[1]], tri_bounds.min, tri_bounds.max);
    math::min_max(positions[verts[2]], tri_bounds.min, tri_bounds.max);

    const bool intersects = isect_aabb_aabb_v3(
        brush_bounds.min, brush_bounds.max, tri_bounds.min, tri_bounds.max);
    brush_test[i].set(intersects);
    r_has_any_true |= intersects;
  }
  return brush_test;
}

static bool primitive_might_intersect_spherical_brush(const Span<float3> positions,
                                                      const Span<int3> vert_tris,
                                                      const int tri_index,
                                                      const float3 &brush_location,
                                                      const float brush_radius_squared)
{
  const int3 tri = vert_tris[tri_index];
  float3 closest;
  closest_on_tri_to_point_v3(
      closest, brush_location, positions[tri[0]], positions[tri[1]], positions[tri[2]]);
  return math::distance_squared(closest, brush_location) <= brush_radius_squared;
}

static bool pixel_row_might_intersect_spherical_brush(const PixelRowLinearPosition &row_linear,
                                                      const int pixel_count,
                                                      const float3 &brush_location,
                                                      const float brush_radius_squared)
{
  if (pixel_count == 0) {
    return false;
  }

  const float3 row_start_position = row_linear.start;
  const float3 row_end_position = row_linear.start + row_linear.delta * float(pixel_count - 1);

  return dist_squared_to_line_segment_v3(brush_location, row_start_position, row_end_position) <=
         brush_radius_squared;
}

static void do_paint_pixels(const Depsgraph &depsgraph,
                            Object &object,
                            const Paint &paint,
                            const Brush &brush,
                            const ImageData *image_data,
                            bke::pbvh::Node &node,
                            SimpleImageBrushPaintTelemetry *r_paint_telemetry)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  PBVHData &pbvh_data = bke::pbvh::pixels::data_get(pbvh);
  NodeData &node_data = bke::pbvh::pixels::node_data_get(node);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);

  const double brush_test_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() : 0.0;
  bool brush_test_has_true = false;
  BitVector<> brush_test = init_uv_primitives_brush_test(
      ss, pbvh_data.vert_tris, node_data.uv_primitives.tri_indices, positions, brush_test_has_true);
  if (r_paint_telemetry != nullptr) {
    telemetry_counter_add(r_paint_telemetry->brush_test_time_us,
                          int64_t((BLI_time_now_seconds() - brush_test_start) * 1.0e6));
  }
  if (!brush_test_has_true) {
    return;
  }

  PaintingKernel<ImageBufferFloat4> kernel_float4;
  PaintingKernel<ImageBufferByte4> kernel_byte4;

  if (brush.alpha <= 0.0f || cache.bstrength <= 0.0f) {
    return;
  }
  const float bstrength = cache.bstrength;

  Vector<float3> pixel_positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float4> texture_colors;
  const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush.falloff_shape);
  const bool use_spherical_row_cull = (brush.falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE);
  const bool use_brush_texture = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT)->tex !=
                                 nullptr;
  const bool use_color_texture = BKE_brush_color_texture_get(&brush, OB_MODE_SCULPT)->tex !=
                                 nullptr;
  Vector<int8_t> primitive_sphere_cull_state;
  if (use_spherical_row_cull) {
    primitive_sphere_cull_state.resize(node_data.uv_primitives.tri_indices.size());
    primitive_sphere_cull_state.fill(-1);
  }

  ImageUser image_user = *image_data->image_user;
  bool pixels_updated = false;
  bool brush_color_ready = false;
  float4 brush_color;
  for (UDIMTilePixels &tile_data : node_data.tiles) {
    if (tile_data.pixel_rows.is_empty()) {
      continue;
    }

    image_user.tile = tile_data.tile_number;

    ImBuf *image_buffer = nullptr;

    for (const PackedPixelRow &pixel_row : tile_data.pixel_rows) {
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_inc(r_paint_telemetry->rows_total);
      }

      if (!brush_test[pixel_row.uv_primitive_index]) {
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_inc(r_paint_telemetry->rows_rejected_by_brush_test);
        }
        continue;
      }

      PixelRowLinearPosition row_linear;
      bool row_linear_is_valid = false;

      const double sphere_cull_start = (r_paint_telemetry != nullptr && use_spherical_row_cull) ?
                                           BLI_time_now_seconds() :
                                           0.0;
      const bool rejected_by_spherical_cull =
          use_spherical_row_cull && ([&]() {
            const int uv_primitive_index = pixel_row.uv_primitive_index;
            int8_t &primitive_state = primitive_sphere_cull_state[uv_primitive_index];
            if (primitive_state == -1) {
              primitive_state = primitive_might_intersect_spherical_brush(
                                    positions,
                                    pbvh_data.vert_tris,
                                    node_data.uv_primitives.tri_indices[uv_primitive_index],
                                    cache.location_symm,
                                    cache.radius_squared) ?
                                    int8_t(1) :
                                    int8_t(0);
            }
            if (primitive_state == 0) {
              return true;
            }
            if (!row_linear_is_valid) {
              row_linear = calc_pixel_row_linear_position(
                  positions,
                  pbvh_data.vert_tris,
                  node_data.uv_primitives.tri_indices,
                  node_data.uv_primitives.delta_barycentric_coords,
                  pixel_row);
              row_linear_is_valid = true;
            }
            return !pixel_row_might_intersect_spherical_brush(
                row_linear, int(pixel_row.num_pixels), cache.location_symm, cache.radius_squared);
          })();
      if (r_paint_telemetry != nullptr && use_spherical_row_cull) {
        telemetry_counter_add(r_paint_telemetry->spherical_cull_time_us,
                              int64_t((BLI_time_now_seconds() - sphere_cull_start) * 1.0e6));
      }
      if (rejected_by_spherical_cull) {
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_inc(r_paint_telemetry->rows_rejected_by_spherical_cull);
        }
        continue;
      }

      const double row_pre_kernel_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                           0.0;

      const int row_pixels_num = int(pixel_row.num_pixels);
      constexpr int kTinyRowPixelsMax = 24;
      const bool use_tiny_row_path = !use_brush_texture && row_pixels_num <= kTinyRowPixelsMax;
      std::array<float, kTinyRowPixelsMax> tiny_factors_storage;
      std::array<float, kTinyRowPixelsMax> tiny_distances_storage;
      MutableSpan<float> row_factors;
      MutableSpan<float> row_distances;

      if (use_tiny_row_path) {
        row_factors = MutableSpan<float>(tiny_factors_storage.data(), row_pixels_num);
        row_distances = MutableSpan<float>(tiny_distances_storage.data(), row_pixels_num);
      }
      else {
        if (factors.capacity() < row_pixels_num) {
          factors.reserve(row_pixels_num);
        }
        if (distances.capacity() < row_pixels_num) {
          distances.reserve(row_pixels_num);
        }
      }

      if (use_brush_texture && pixel_positions.capacity() < row_pixels_num) {
        pixel_positions.reserve(row_pixels_num);
      }
      if (use_color_texture && texture_colors.capacity() < row_pixels_num) {
        texture_colors.reserve(row_pixels_num);
      }

      const double row_strength_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                         0.0;

      if (!use_tiny_row_path) {
        factors.resize(row_pixels_num);
        distances.resize(row_pixels_num);
        row_factors = factors;
        row_distances = distances;
      }

      if (use_brush_texture) {
        const double row_positions_start = (r_paint_telemetry != nullptr) ?
                                               BLI_time_now_seconds() :
                                               0.0;
        pixel_positions.resize(pixel_row.num_pixels);
        calc_pixel_row_positions(positions,
                                 pbvh_data.vert_tris,
                                 node_data.uv_primitives.tri_indices,
                                 node_data.uv_primitives.delta_barycentric_coords,
                                 pixel_row,
                                 pixel_positions);
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_add(r_paint_telemetry->pre_positions_time_us,
                                int64_t((BLI_time_now_seconds() - row_positions_start) * 1.0e6));
        }

        calc_brush_strength_distances_optimized(ss,
                                                cache,
                                                brush,
                                                falloff_shape,
                                                bstrength,
                                                use_brush_texture,
                                                pixel_positions,
                                                row_distances,
                                                row_factors);

        const double row_texture_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                          0.0;
        calc_brush_texture_factors(ss, brush, pixel_positions, row_factors);
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_add(r_paint_telemetry->pre_texture_time_us,
                                int64_t((BLI_time_now_seconds() - row_texture_start) * 1.0e6));
        }

        /* Sample color texture if enabled. */
        if (use_color_texture) {
          texture_colors.resize(pixel_row.num_pixels);
          calc_brush_color_texture(ss, brush, pixel_positions, texture_colors);
        }
      }
      else {
        if (!row_linear_is_valid) {
          row_linear = calc_pixel_row_linear_position(
              positions, pbvh_data.vert_tris, node_data.uv_primitives.tri_indices,
              node_data.uv_primitives.delta_barycentric_coords, pixel_row);
          row_linear_is_valid = true;
        }
        calc_brush_strength_distances_no_texture_row_linear(
            ss, cache, brush, falloff_shape, bstrength, row_linear, row_distances, row_factors);

        /* Sample color texture if enabled (no mask texture, but color texture present). */
        if (use_color_texture) {
          pixel_positions.resize(pixel_row.num_pixels);
          calc_pixel_row_positions(
              positions, pbvh_data.vert_tris, node_data.uv_primitives.tri_indices,
              node_data.uv_primitives.delta_barycentric_coords, pixel_row, pixel_positions);
          texture_colors.resize(pixel_row.num_pixels);
          calc_brush_color_texture(ss, brush, pixel_positions, texture_colors);
        }
      }
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_add(r_paint_telemetry->pre_strength_time_us,
                              int64_t((BLI_time_now_seconds() - row_strength_start) * 1.0e6));
      }

      int first_positive;
      int last_positive;
      if (!PaintingKernel<ImageBufferFloat4>::find_positive_factor_bounds(
              row_factors, first_positive, last_positive))
      {
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_inc(use_brush_texture ?
                                    r_paint_telemetry->rows_rejected_by_zero_texture :
                                    r_paint_telemetry->rows_rejected_by_zero_strength);
          telemetry_counter_add(r_paint_telemetry->pre_kernel_time_us,
                                int64_t((BLI_time_now_seconds() - row_pre_kernel_start) * 1.0e6));
        }
        continue;
      }

      if (image_buffer == nullptr) {
        const double ibuf_acquire_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                           0.0;
        image_buffer = BKE_image_acquire_ibuf(image_data->image, &image_user, nullptr);
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_add(r_paint_telemetry->ibuf_acquire_release_time_us,
                                int64_t((BLI_time_now_seconds() - ibuf_acquire_start) * 1.0e6));
        }
        if (image_buffer == nullptr) {
          break;
        }

        if (!brush_color_ready) {
#ifdef DEBUG_PIXEL_NODES
          uint hash = BLI_hash_int(POINTER_AS_UINT(&node));

          brush_color[0] = float(hash & 255) / 255.0f;
          brush_color[1] = float((hash >> 8) & 255) / 255.0f;
          brush_color[2] = float((hash >> 16) & 255) / 255.0f;
#else
          float step_brush_color[3];
          paint_brush_color_get(&paint,
                                &brush,
                                ss.cache->initial_hsv_jitter,
                                ss.cache->invert,
                                cache.stroke_distance,
                                cache.pressure,
                                step_brush_color);
          copy_v3_v3(brush_color, step_brush_color);
#endif
          brush_color[3] = 1.0f;
          brush_color_ready = true;
        }

        if (image_buffer->float_buffer.data != nullptr) {
          kernel_float4.init_brush_color(image_buffer, brush_color);
        }
        else {
          kernel_byte4.init_brush_color(image_buffer, brush_color);
        }
      }

      if (r_paint_telemetry != nullptr) {
        telemetry_counter_add(r_paint_telemetry->pre_kernel_time_us,
                              int64_t((BLI_time_now_seconds() - row_pre_kernel_start) * 1.0e6));
      }

      bool pixels_painted = false;
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_inc(r_paint_telemetry->rows_painted_attempted);
      }
      const double row_kernel_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                       0.0;
      if (image_buffer->float_buffer.data != nullptr) {
        if (use_color_texture) {
          pixels_painted = kernel_float4.paint_with_texture_colors(brush,
                                                                   pixel_row,
                                                                   row_factors,
                                                                   texture_colors,
                                                                   image_buffer,
                                                                   first_positive,
                                                                   last_positive);
        }
        else {
          pixels_painted = kernel_float4.paint(
              brush, pixel_row, row_factors, image_buffer, first_positive, last_positive);
        }
      }
      else {
        if (use_color_texture) {
          pixels_painted = kernel_byte4.paint_with_texture_colors(brush,
                                                                  pixel_row,
                                                                  row_factors,
                                                                  texture_colors,
                                                                  image_buffer,
                                                                  first_positive,
                                                                  last_positive);
        }
        else {
          pixels_painted = kernel_byte4.paint(
              brush, pixel_row, row_factors, image_buffer, first_positive, last_positive);
        }
      }
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_add(r_paint_telemetry->kernel_time_us,
                              int64_t((BLI_time_now_seconds() - row_kernel_start) * 1.0e6));
      }

      if (pixels_painted) {
        tile_data.mark_dirty(pixel_row);
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_inc(r_paint_telemetry->rows_painted_success);
        }
      }
    }

    if (image_buffer != nullptr) {
      const double ibuf_release_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                         0.0;
      BKE_image_release_ibuf(image_data->image, image_buffer, nullptr);
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_add(r_paint_telemetry->ibuf_acquire_release_time_us,
                              int64_t((BLI_time_now_seconds() - ibuf_release_start) * 1.0e6));
      }
    }
    pixels_updated |= tile_data.flags.dirty;
  }

  node_data.flags.dirty |= pixels_updated;
}

static bool do_paint_pixels_for_node_tile(const SculptSession &ss,
                                          const StrokeCache &cache,
                                          const PBVHData &pbvh_data,
                                          const Span<float3> positions,
                                          const Paint &paint,
                                          const Brush &brush,
                                          bke::pbvh::Node &node,
                                          const short tile_number,
                                          ImBuf &image_buffer,
                                          const BitVector<> &brush_test,
                                          SimpleImageBrushPaintTelemetry *r_paint_telemetry)
{
  NodeData &node_data = bke::pbvh::pixels::node_data_get(node);

  UDIMTilePixels *tile_data_ptr = nullptr;
  for (UDIMTilePixels &tile_data : node_data.tiles) {
    if (tile_data.tile_number == tile_number) {
      tile_data_ptr = &tile_data;
      break;
    }
  }
  if (tile_data_ptr == nullptr || tile_data_ptr->pixel_rows.is_empty()) {
    return false;
  }

  PaintingKernel<ImageBufferFloat4> kernel_float4;
  PaintingKernel<ImageBufferByte4> kernel_byte4;

  if (brush.alpha <= 0.0f || cache.bstrength <= 0.0f) {
    return false;
  }
  const float bstrength = cache.bstrength;

  Vector<float3> pixel_positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float4> texture_colors;
  const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush.falloff_shape);
  const bool use_spherical_row_cull = (brush.falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE);
  const bool use_brush_texture = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT)->tex !=
                                 nullptr;
  const bool use_color_texture = BKE_brush_color_texture_get(&brush, OB_MODE_SCULPT)->tex !=
                                 nullptr;
  bool kernel_initialized = false;
  float4 brush_color;
  Vector<int8_t> primitive_sphere_cull_state;
  if (use_spherical_row_cull) {
    primitive_sphere_cull_state.resize(node_data.uv_primitives.tri_indices.size());
    primitive_sphere_cull_state.fill(-1);
  }

  bool pixels_updated = false;
  UDIMTilePixels &tile_data = *tile_data_ptr;
  for (const PackedPixelRow &pixel_row : tile_data.pixel_rows) {
    if (r_paint_telemetry != nullptr) {
      telemetry_counter_inc(r_paint_telemetry->rows_total);
    }

    if (!brush_test[pixel_row.uv_primitive_index]) {
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_inc(r_paint_telemetry->rows_rejected_by_brush_test);
      }
      continue;
    }

    PixelRowLinearPosition row_linear;
    bool row_linear_is_valid = false;

    const double sphere_cull_start = (r_paint_telemetry != nullptr && use_spherical_row_cull) ?
                                         BLI_time_now_seconds() :
                                         0.0;
    const bool rejected_by_spherical_cull =
        use_spherical_row_cull && ([&]() {
          const int uv_primitive_index = pixel_row.uv_primitive_index;
          int8_t &primitive_state = primitive_sphere_cull_state[uv_primitive_index];
          if (primitive_state == -1) {
            primitive_state = primitive_might_intersect_spherical_brush(
                                  positions,
                                  pbvh_data.vert_tris,
                                  node_data.uv_primitives.tri_indices[uv_primitive_index],
                                  cache.location_symm,
                                  cache.radius_squared) ?
                                  int8_t(1) :
                                  int8_t(0);
          }
          if (primitive_state == 0) {
            return true;
          }
          if (!row_linear_is_valid) {
            row_linear = calc_pixel_row_linear_position(
                positions, pbvh_data.vert_tris, node_data.uv_primitives.tri_indices,
                node_data.uv_primitives.delta_barycentric_coords, pixel_row);
            row_linear_is_valid = true;
          }
          return !pixel_row_might_intersect_spherical_brush(
              row_linear, int(pixel_row.num_pixels), cache.location_symm, cache.radius_squared);
        })();
    if (r_paint_telemetry != nullptr && use_spherical_row_cull) {
      telemetry_counter_add(r_paint_telemetry->spherical_cull_time_us,
                            int64_t((BLI_time_now_seconds() - sphere_cull_start) * 1.0e6));
    }
    if (rejected_by_spherical_cull) {
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_inc(r_paint_telemetry->rows_rejected_by_spherical_cull);
      }
      continue;
    }

    const double row_pre_kernel_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                         0.0;

    const int row_pixels_num = int(pixel_row.num_pixels);
    constexpr int kTinyRowPixelsMax = 24;
    const bool use_tiny_row_path = !use_brush_texture && row_pixels_num <= kTinyRowPixelsMax;
    std::array<float, kTinyRowPixelsMax> tiny_factors_storage;
    std::array<float, kTinyRowPixelsMax> tiny_distances_storage;
    MutableSpan<float> row_factors;
    MutableSpan<float> row_distances;

    if (use_tiny_row_path) {
      row_factors = MutableSpan<float>(tiny_factors_storage.data(), row_pixels_num);
      row_distances = MutableSpan<float>(tiny_distances_storage.data(), row_pixels_num);
    }
    else {
      if (factors.capacity() < row_pixels_num) {
        factors.reserve(row_pixels_num);
      }
      if (distances.capacity() < row_pixels_num) {
        distances.reserve(row_pixels_num);
      }
    }

    if (use_brush_texture && pixel_positions.capacity() < row_pixels_num) {
      pixel_positions.reserve(row_pixels_num);
    }
    if (use_color_texture && texture_colors.capacity() < row_pixels_num) {
      texture_colors.reserve(row_pixels_num);
    }

    const double row_strength_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                       0.0;
    if (!use_tiny_row_path) {
      factors.resize(row_pixels_num);
      distances.resize(row_pixels_num);
      row_factors = factors;
      row_distances = distances;
    }

      if (use_brush_texture) {
        const double row_positions_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                           0.0;
        pixel_positions.resize(pixel_row.num_pixels);
        calc_pixel_row_positions(
            positions, pbvh_data.vert_tris, node_data.uv_primitives.tri_indices,
            node_data.uv_primitives.delta_barycentric_coords, pixel_row, pixel_positions);
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_add(r_paint_telemetry->pre_positions_time_us,
                                int64_t((BLI_time_now_seconds() - row_positions_start) * 1.0e6));
        }

        calc_brush_strength_distances_optimized(ss,
                                                cache,
                                                brush,
                                                falloff_shape,
                                                bstrength,
                                                use_brush_texture,
                                                pixel_positions,
                                                row_distances,
                                                row_factors);

        const double row_texture_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                          0.0;
        calc_brush_texture_factors(ss, brush, pixel_positions, row_factors);
        if (r_paint_telemetry != nullptr) {
          telemetry_counter_add(r_paint_telemetry->pre_texture_time_us,
                                int64_t((BLI_time_now_seconds() - row_texture_start) * 1.0e6));
        }

        /* Sample color texture if enabled. */
        if (use_color_texture) {
          texture_colors.resize(pixel_row.num_pixels);
          calc_brush_color_texture(ss, brush, pixel_positions, texture_colors);
        }
      }
      else {
        if (!row_linear_is_valid) {
          row_linear = calc_pixel_row_linear_position(
              positions, pbvh_data.vert_tris, node_data.uv_primitives.tri_indices,
              node_data.uv_primitives.delta_barycentric_coords, pixel_row);
          row_linear_is_valid = true;
        }
        calc_brush_strength_distances_no_texture_row_linear(
            ss, cache, brush, falloff_shape, bstrength, row_linear, row_distances, row_factors);

        /* Sample color texture if enabled (no mask texture, but color texture present). */
        if (use_color_texture) {
          pixel_positions.resize(pixel_row.num_pixels);
          calc_pixel_row_positions(positions,
                                   pbvh_data.vert_tris,
                                   node_data.uv_primitives.tri_indices,
                                   node_data.uv_primitives.delta_barycentric_coords,
                                   pixel_row,
                                   pixel_positions);
          texture_colors.resize(pixel_row.num_pixels);
          calc_brush_color_texture(ss, brush, pixel_positions, texture_colors);
        }
      }
    if (r_paint_telemetry != nullptr) {
      telemetry_counter_add(r_paint_telemetry->pre_strength_time_us,
                            int64_t((BLI_time_now_seconds() - row_strength_start) * 1.0e6));
    }

    int first_positive;
    int last_positive;
    if (!PaintingKernel<ImageBufferFloat4>::find_positive_factor_bounds(
            row_factors, first_positive, last_positive))
    {
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_inc(use_brush_texture ?
                                  r_paint_telemetry->rows_rejected_by_zero_texture :
                                  r_paint_telemetry->rows_rejected_by_zero_strength);
        telemetry_counter_add(r_paint_telemetry->pre_kernel_time_us,
                              int64_t((BLI_time_now_seconds() - row_pre_kernel_start) * 1.0e6));
      }
      continue;
    }

    if (!kernel_initialized) {
#ifdef DEBUG_PIXEL_NODES
      uint hash = BLI_hash_int(POINTER_AS_UINT(&node));

      brush_color[0] = float(hash & 255) / 255.0f;
      brush_color[1] = float((hash >> 8) & 255) / 255.0f;
      brush_color[2] = float((hash >> 16) & 255) / 255.0f;
#else
      float step_brush_color[3];
      paint_brush_color_get(&paint,
                            &brush,
                            ss.cache->initial_hsv_jitter,
                            ss.cache->invert,
                            cache.stroke_distance,
                            cache.pressure,
                            step_brush_color);
      copy_v3_v3(brush_color, step_brush_color);
#endif

      brush_color[3] = 1.0f;
      if (image_buffer.float_buffer.data != nullptr) {
        kernel_float4.init_brush_color(&image_buffer, brush_color);
      }
      else {
        kernel_byte4.init_brush_color(&image_buffer, brush_color);
      }
      kernel_initialized = true;
    }

    if (r_paint_telemetry != nullptr) {
      telemetry_counter_add(r_paint_telemetry->pre_kernel_time_us,
                            int64_t((BLI_time_now_seconds() - row_pre_kernel_start) * 1.0e6));
    }

    bool pixels_painted = false;
    if (r_paint_telemetry != nullptr) {
      telemetry_counter_inc(r_paint_telemetry->rows_painted_attempted);
    }
    const double row_kernel_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() : 0.0;
    if (image_buffer.float_buffer.data != nullptr) {
      if (use_color_texture) {
        pixels_painted = kernel_float4.paint_with_texture_colors(brush,
                                                                 pixel_row,
                                                                 row_factors,
                                                                 texture_colors,
                                                                 &image_buffer,
                                                                 first_positive,
                                                                 last_positive);
      }
      else {
        pixels_painted = kernel_float4.paint(
            brush, pixel_row, row_factors, &image_buffer, first_positive, last_positive);
      }
    }
    else {
      if (use_color_texture) {
        pixels_painted = kernel_byte4.paint_with_texture_colors(brush,
                                                                pixel_row,
                                                                row_factors,
                                                                texture_colors,
                                                                &image_buffer,
                                                                first_positive,
                                                                last_positive);
      }
      else {
        pixels_painted = kernel_byte4.paint(
            brush, pixel_row, row_factors, &image_buffer, first_positive, last_positive);
      }
    }
    if (r_paint_telemetry != nullptr) {
      telemetry_counter_add(r_paint_telemetry->kernel_time_us,
                            int64_t((BLI_time_now_seconds() - row_kernel_start) * 1.0e6));
    }

    if (pixels_painted) {
      tile_data.mark_dirty(pixel_row);
      if (r_paint_telemetry != nullptr) {
        telemetry_counter_inc(r_paint_telemetry->rows_painted_success);
      }
    }
  }

  pixels_updated |= tile_data.flags.dirty;
  node_data.flags.dirty |= pixels_updated;
  return pixels_updated;
}

static void do_paint_pixels_batched_by_tile(const Depsgraph &depsgraph,
                                            Object &object,
                                            const Paint &paint,
                                            const Brush &brush,
                                            const ImageData *image_data,
                                            MutableSpan<bke::pbvh::MeshNode> nodes,
                                            const IndexMask &node_mask,
                                            SimpleImageBrushPaintTelemetry *r_paint_telemetry)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  PBVHData &pbvh_data = bke::pbvh::pixels::data_get(pbvh);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);

  Map<short, Vector<int>> node_indices_by_tile;
  Map<int, BitVector<>> brush_test_by_node;
  node_mask.foreach_index([&](const int i) {
    const NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[i]);
    const double brush_test_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() : 0.0;
    bool brush_test_has_true = false;
    BitVector<> brush_test = init_uv_primitives_brush_test(
        ss, pbvh_data.vert_tris, node_data.uv_primitives.tri_indices, positions, brush_test_has_true);
    if (r_paint_telemetry != nullptr) {
      telemetry_counter_add(r_paint_telemetry->brush_test_time_us,
                            int64_t((BLI_time_now_seconds() - brush_test_start) * 1.0e6));
    }
    if (!brush_test_has_true) {
      return;
    }
    brush_test_by_node.add(i, std::move(brush_test));

    for (const UDIMTilePixels &tile_data : node_data.tiles) {
      if (tile_data.pixel_rows.is_empty()) {
        continue;
      }

      if (Vector<int> *tile_node_indices = node_indices_by_tile.lookup_ptr(tile_data.tile_number))
      {
        tile_node_indices->append(i);
      }
      else {
        Vector<int> new_tile_node_indices;
        new_tile_node_indices.append(i);
        node_indices_by_tile.add(tile_data.tile_number, std::move(new_tile_node_indices));
      }
    }
  });

  if (node_indices_by_tile.is_empty()) {
    return;
  }

  ImageUser image_user = *image_data->image_user;
  for (const auto item : node_indices_by_tile.items()) {
    image_user.tile = item.key;

    const double ibuf_acquire_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                       0.0;
    ImBuf *image_buffer = BKE_image_acquire_ibuf(image_data->image, &image_user, nullptr);
    if (r_paint_telemetry != nullptr) {
      telemetry_counter_add(r_paint_telemetry->ibuf_acquire_release_time_us,
                            int64_t((BLI_time_now_seconds() - ibuf_acquire_start) * 1.0e6));
    }
    if (image_buffer == nullptr) {
      continue;
    }

    for (const int node_index : item.value) {
      const BitVector<> *brush_test = brush_test_by_node.lookup_ptr(node_index);
      if (brush_test == nullptr) {
        continue;
      }
      do_paint_pixels_for_node_tile(ss,
                                    cache,
                                    pbvh_data,
                                    positions,
                                    paint,
                                    brush,
                                    nodes[node_index],
                                    item.key,
                                    *image_buffer,
                                    *brush_test,
                                    r_paint_telemetry);
    }

    const double ibuf_release_start = (r_paint_telemetry != nullptr) ? BLI_time_now_seconds() :
                                                                       0.0;
    BKE_image_release_ibuf(image_data->image, image_buffer, nullptr);
    if (r_paint_telemetry != nullptr) {
      telemetry_counter_add(r_paint_telemetry->ibuf_acquire_release_time_us,
                            int64_t((BLI_time_now_seconds() - ibuf_release_start) * 1.0e6));
    }
  }
}

static constexpr bool kEnableGradientPaintDebugTelemetry = true;
static constexpr int64_t kGradientPaintDebugPeriod = 1;
static constexpr int kMaxDebugRejectedRows = 5;  /* Only log first N rejected rows in detail */

struct GradientPaintTelemetry {
  std::atomic<int64_t> rows_total = 0;
  std::atomic<int64_t> rows_rejected_no_influence = 0;
  std::atomic<int64_t> rows_painted_attempted = 0;
  std::atomic<int64_t> rows_painted_success = 0;
  std::atomic<int64_t> pre_positions_time_us = 0;
  std::atomic<int64_t> gradient_calc_time_us = 0;
  std::atomic<int64_t> kernel_time_us = 0;
  std::atomic<int64_t> ibuf_acquire_release_time_us = 0;
  std::atomic<int64_t> cache_hits = 0;
  std::atomic<int64_t> cache_misses = 0;
  
  /* Extended debugging for missing pixels */
  std::atomic<int64_t> pixels_total = 0;
  std::atomic<int64_t> pixels_rejected_early = 0;
  std::atomic<int64_t> pixels_rejected_projection = 0;
  std::atomic<int64_t> pixels_with_factor_zero = 0;
  std::atomic<int64_t> pixels_with_factor_nonzero = 0;
  std::atomic<int64_t> pixels_kernel_skipped = 0;
  std::atomic<int64_t> pixels_kernel_painted = 0;
};

static GradientPaintTelemetry g_gradient_telemetry;

static void do_paint_pixels_gradient(const Depsgraph &depsgraph,
                                     Object &object,
                                     const Paint &paint,
                                     const Brush &brush,
                                     const ImageData *image_data,
                                     bke::pbvh::Node &node,
                                     const ARegion &region,
                                     const ed::sculpt_paint::gradient::Calculator &calculator,
                                     const bool clamp_to_range,
                                     const int symmetry,
                                     const int8_t radial_symmetry[3])
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache *cache = ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  PBVHData &pbvh_data = bke::pbvh::pixels::data_get(pbvh);
  NodeData &node_data = bke::pbvh::pixels::node_data_get(node);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);

  /* Note: We don't use brush test filter here because gradient can affect any face on the
   * screen, not just those near the brush location. The gradient calculation already handles
   * pixels with no influence (returns 0.0f). */

  /* Early exit if brush has no strength */
  const float bstrength = cache ? cache->bstrength : 1.0f;
  if (bstrength <= 0.0f) {
    return;
  }

  float4 brush_color;

#ifdef DEBUG_PIXEL_NODES
  uint hash = BLI_hash_int(POINTER_AS_UINT(&node));

  brush_color[0] = float(hash & 255) / 255.0f;
  brush_color[1] = float((hash >> 8) & 255) / 255.0f;
  brush_color[2] = float((hash >> 16) & 255) / 255.0f;
#else
  float step_brush_color[3];
  if (cache) {
    paint_brush_color_get(&paint,
                          &brush,
                          cache->initial_hsv_jitter,
                          cache->invert,
                          cache->stroke_distance,
                          cache->pressure,
                          step_brush_color);
  }
  else {
    paint_brush_color_get(&paint, &brush, std::nullopt, false, 0.0f, 1.0f, step_brush_color);
  }
  copy_v3_v3(brush_color, step_brush_color);
#endif

  brush_color[3] = 1.0f;

  /* Check if color texture is enabled for gradient painting */
  const bool use_color_texture = BKE_brush_color_texture_get(&brush, OB_MODE_SCULPT)->tex !=
                                 nullptr;

  /* Note: pixel_positions and factors are now thread-local inside parallel_for loop */

  /* Debug: log batch optimization status once per function call */
  const bool radial_symmetry_enabled = (radial_symmetry[0] != 1 || radial_symmetry[1] != 1 ||
                                        radial_symmetry[2] != 1);
  const bool use_batch_projection = (symmetry == 0 && !radial_symmetry_enabled);
  std::fprintf(stderr,
               "[gradient_paint] symmetry=%d radial_symmetry=[%d,%d,%d] use_batch=%d\n",
               symmetry,
               radial_symmetry[0],
               radial_symmetry[1],
               radial_symmetry[2],
               use_batch_projection);

  ImageUser image_user = *image_data->image_user;
  Vector<UDIMTilePixels *> tiles_to_process;
  int64_t total_rows = 0;
  int64_t large_tiles_num = 0;
  constexpr int64_t row_threading_min_rows = 4096;
  constexpr int64_t row_threading_grain_size = 256;
  constexpr int64_t tile_parallel_min_rows = 8192;
  constexpr int64_t tile_parallel_min_tiles = 2;
  for (UDIMTilePixels &tile_data : node_data.tiles) {
    if (!tile_data.pixel_rows.is_empty()) {
      tiles_to_process.append(&tile_data);
      const int64_t tile_rows = tile_data.pixel_rows.size();
      total_rows += tile_rows;
      if (tile_rows >= tile_parallel_min_rows) {
        large_tiles_num++;
      }
    }
  }

  const bool use_tile_parallel = (tiles_to_process.size() >= tile_parallel_min_tiles &&
                                  large_tiles_num >= tile_parallel_min_tiles &&
                                  total_rows >= tile_parallel_min_rows * tile_parallel_min_tiles);
  std::fprintf(stderr,
               "[gradient_paint] tile_mode use_tile_parallel=%d tiles=%lld large_tiles=%lld "
               "total_rows=%lld\n",
               use_tile_parallel,
               static_cast<long long>(tiles_to_process.size()),
               static_cast<long long>(large_tiles_num),
               static_cast<long long>(total_rows));

  auto process_tile = [&](UDIMTilePixels &tile_data, const bool allow_row_parallel) {
    ImageUser local_image_user = image_user;
    local_image_user.tile = tile_data.tile_number;

    const double ibuf_acquire_start = BLI_time_now_seconds();
    ImBuf *image_buffer = BKE_image_acquire_ibuf(image_data->image, &local_image_user, nullptr);
    if (kEnableGradientPaintDebugTelemetry) {
      telemetry_counter_add(g_gradient_telemetry.ibuf_acquire_release_time_us,
                            int64_t((BLI_time_now_seconds() - ibuf_acquire_start) * 1.0e6));
    }
    if (image_buffer == nullptr) {
      return;
    }

    PaintingKernel<ImageBufferFloat4> local_kernel_float4;
    PaintingKernel<ImageBufferByte4> local_kernel_byte4;
    const bool is_float_image = image_buffer->float_buffer.data != nullptr;
    if (is_float_image) {
      local_kernel_float4.init_brush_color(image_buffer, brush_color);
    }
    else {
      local_kernel_byte4.init_brush_color(image_buffer, brush_color);
    }

    int64_t local_rows_total = 0;
    int64_t local_rows_rejected_no_influence = 0;
    int64_t local_rows_painted_attempted = 0;
    int64_t local_rows_painted_success = 0;
    int64_t local_pre_positions_time_us = 0;
    int64_t local_gradient_calc_time_us = 0;
    int64_t local_kernel_time_us = 0;

    const Span<PackedPixelRow> pixel_rows = tile_data.pixel_rows;
    if (allow_row_parallel && pixel_rows.size() >= row_threading_min_rows) {
      struct ThreadDirtyRegion {
        bool has_dirty = false;
        rcti region;

        ThreadDirtyRegion()
        {
          BLI_rcti_init_minmax(&region);
        }
      };

      threading::EnumerableThreadSpecific<Vector<float3>> tls_pixel_positions;
      threading::EnumerableThreadSpecific<Vector<float>> tls_factors;
      threading::EnumerableThreadSpecific<Vector<float2>> tls_screen_coords;
      threading::EnumerableThreadSpecific<Vector<float4>> tls_texture_colors;
      threading::EnumerableThreadSpecific<ThreadDirtyRegion> tls_dirty_regions;

      std::atomic<int64_t> local_rows_total_atomic{0};
      std::atomic<int64_t> local_rows_rejected_no_influence_atomic{0};
      std::atomic<int64_t> local_rows_painted_attempted_atomic{0};
      std::atomic<int64_t> local_rows_painted_success_atomic{0};
      std::atomic<int64_t> local_pre_positions_time_us_atomic{0};
      std::atomic<int64_t> local_gradient_calc_time_us_atomic{0};
      std::atomic<int64_t> local_kernel_time_us_atomic{0};
      std::atomic<int> debug_rejected_rows_count{0};  /* For limiting detailed debug output */

      threading::parallel_for(
          pixel_rows.index_range(), row_threading_grain_size, [&](const IndexRange range) {
            Vector<float3> &pixel_positions = tls_pixel_positions.local();
            Vector<float> &factors = tls_factors.local();
            Vector<float2> &screen_coords = tls_screen_coords.local();
            Vector<float4> &texture_colors = tls_texture_colors.local();

            for (const int row_index : range) {
              const PackedPixelRow &pixel_row = pixel_rows[row_index];
              if (kEnableGradientPaintDebugTelemetry) {
                local_rows_total_atomic.fetch_add(1, std::memory_order_relaxed);
              }

              const int row_pixels_num = int(pixel_row.num_pixels);
              if (pixel_positions.capacity() < row_pixels_num) {
                pixel_positions.reserve(row_pixels_num);
              }
              if (factors.capacity() < row_pixels_num) {
                factors.reserve(row_pixels_num);
              }

              const double positions_start = kEnableGradientPaintDebugTelemetry ?
                                                 BLI_time_now_seconds() :
                                                 0.0;
              pixel_positions.resize(pixel_row.num_pixels);
              calc_pixel_row_positions(positions,
                                       pbvh_data.vert_tris,
                                       node_data.uv_primitives.tri_indices,
                                       node_data.uv_primitives.delta_barycentric_coords,
                                       pixel_row,
                                       pixel_positions);
              if (kEnableGradientPaintDebugTelemetry) {
                local_pre_positions_time_us_atomic.fetch_add(
                    int64_t((BLI_time_now_seconds() - positions_start) * 1.0e6),
                    std::memory_order_relaxed);
              }

              const double gradient_calc_start = kEnableGradientPaintDebugTelemetry ?
                                                     BLI_time_now_seconds() :
                                                     0.0;
              factors.resize(pixel_positions.size());
              bool has_influence = false;

              /* For debugging: store sample factors from this row */
              float first_factor_raw = 0.0f, first_factor_final = 0.0f;
              float middle_factor_raw = 0.0f, middle_factor_final = 0.0f;
              float last_factor_raw = 0.0f, last_factor_final = 0.0f;
              int pixels_processed = 0;

              if (use_batch_projection) {
                if (screen_coords.capacity() < pixel_positions.size()) {
                  screen_coords.reserve(pixel_positions.size());
                }
                screen_coords.resize(pixel_positions.size());

                /* Allocate projection status array */
                Vector<eV3DProjStatus> proj_statuses;
                if (proj_statuses.capacity() < pixel_positions.size()) {
                  proj_statuses.reserve(pixel_positions.size());
                }
                proj_statuses.resize(pixel_positions.size());

                ED_view3d_project_float_object_array_with_status(
                    &region,
                    pixel_positions.as_span(),
                    screen_coords.as_mutable_span(),
                    proj_statuses.as_mutable_span(),
                    V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_NEAR);

                for (const int i : factors.index_range()) {
                  const float2 &screen_co = screen_coords[i];
                  const bool projection_failed = (proj_statuses[i] != V3D_PROJ_RET_OK);

                  if (kEnableGradientPaintDebugTelemetry) {
                    g_gradient_telemetry.pixels_total.fetch_add(1, std::memory_order_relaxed);
                  }

                  /* Early rejection optimization disabled for gradient tool - the gradient calculator
                   * will correctly return 0.0f for pixels outside the gradient range. Early rejection
                   * was using screen-space coordinates which are incorrect for World/UV space gradients. */
                  if (projection_failed && kEnableGradientPaintDebugTelemetry) {
                    g_gradient_telemetry.pixels_rejected_projection.fetch_add(1, std::memory_order_relaxed);
                  }

                  float factor_raw = paint_projected_gradient_factor_with_preprojected(
                      calculator, screen_co, symmetry, radial_symmetry);
                  float factor = paint_gradient_finalize_factor(
                      brush, factor_raw, clamp_to_range, bstrength, true);
                  factors[i] = factor;

                  /* Store sample factors for debugging */
                  if (kEnableGradientPaintDebugTelemetry) {
                    if (pixels_processed == 0) {
                      first_factor_raw = factor_raw;
                      first_factor_final = factor;
                    }
                    if (pixels_processed == pixel_positions.size() / 2) {
                      middle_factor_raw = factor_raw;
                      middle_factor_final = factor;
                    }
                    last_factor_raw = factor_raw;
                    last_factor_final = factor;
                    pixels_processed++;

                    if (factor == 0.0f) {
                      g_gradient_telemetry.pixels_with_factor_zero.fetch_add(1, std::memory_order_relaxed);
                    }
                    else {
                      g_gradient_telemetry.pixels_with_factor_nonzero.fetch_add(1, std::memory_order_relaxed);
                    }
                  }

                  /* For Gradient Tools, negative factors are valid (pixels "before" gradient start) */
                  has_influence |= (factor != 0.0f);
                }

                /* Log details for first few rejected rows */
                if (kEnableGradientPaintDebugTelemetry && !has_influence) {
                  int rejected_count = debug_rejected_rows_count.fetch_add(1, std::memory_order_relaxed);
                  if (rejected_count < kMaxDebugRejectedRows) {
                    fprintf(stderr,
                            "GRADIENT ROW REJECT #%d: row=%d pixels=%d processed=%d\n"
                            "  first: raw=%.4f final=%.4f\n"
                            "  middle: raw=%.4f final=%.4f\n"
                            "  last: raw=%.4f final=%.4f\n"
                            "  bstrength=%.4f clamp=%d symmetry=%d\n",
                            rejected_count + 1,
                            row_index,
                            int(pixel_row.num_pixels),
                            pixels_processed,
                            first_factor_raw,
                            first_factor_final,
                            middle_factor_raw,
                            middle_factor_final,
                            last_factor_raw,
                            last_factor_final,
                            bstrength,
                            int(clamp_to_range),
                            symmetry);
                    fflush(stderr);
                  }
                }
              }
              else {
                /* Non-batch projection path */
                float first_factor_raw = 0.0f, first_factor_final = 0.0f;
                float middle_factor_raw = 0.0f, middle_factor_final = 0.0f;
                float last_factor_raw = 0.0f, last_factor_final = 0.0f;
                int pixels_processed = 0;

                for (const int i : factors.index_range()) {
                  float factor_raw = paint_projected_gradient_factor_with_symmetry(
                      &region, calculator, pixel_positions[i], symmetry, radial_symmetry);
                  float factor = paint_gradient_finalize_factor(
                      brush, factor_raw, clamp_to_range, bstrength, true);
                  factors[i] = factor;

                  if (kEnableGradientPaintDebugTelemetry) {
                    if (pixels_processed == 0) {
                      first_factor_raw = factor_raw;
                      first_factor_final = factor;
                    }
                    if (pixels_processed == pixel_positions.size() / 2) {
                      middle_factor_raw = factor_raw;
                      middle_factor_final = factor;
                    }
                    last_factor_raw = factor_raw;
                    last_factor_final = factor;
                    pixels_processed++;
                  }

                  /* For Gradient Tools, negative factors are valid (pixels "before" gradient start) */
                  has_influence |= (factor != 0.0f);
                }

                /* Log details for first few rejected rows */
                if (kEnableGradientPaintDebugTelemetry && !has_influence) {
                  int rejected_count = debug_rejected_rows_count.fetch_add(1, std::memory_order_relaxed);
                  if (rejected_count < kMaxDebugRejectedRows) {
                    fprintf(stderr,
                            "GRADIENT ROW REJECT #%d (symmetry): row=%d pixels=%d processed=%d\n"
                            "  first: raw=%.4f final=%.4f\n"
                            "  middle: raw=%.4f final=%.4f\n"
                            "  last: raw=%.4f final=%.4f\n"
                            "  bstrength=%.4f clamp=%d symmetry=%d\n",
                            rejected_count + 1,
                            row_index,
                            int(pixel_row.num_pixels),
                            pixels_processed,
                            first_factor_raw,
                            first_factor_final,
                            middle_factor_raw,
                            middle_factor_final,
                            last_factor_raw,
                            last_factor_final,
                            bstrength,
                            int(clamp_to_range),
                            symmetry);
                    fflush(stderr);
                  }
                }
              }

              if (kEnableGradientPaintDebugTelemetry) {
                local_gradient_calc_time_us_atomic.fetch_add(
                    int64_t((BLI_time_now_seconds() - gradient_calc_start) * 1.0e6),
                    std::memory_order_relaxed);
              }

              if (!has_influence) {
                if (kEnableGradientPaintDebugTelemetry) {
                  local_rows_rejected_no_influence_atomic.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
              }

              if (kEnableGradientPaintDebugTelemetry) {
                local_rows_painted_attempted_atomic.fetch_add(1, std::memory_order_relaxed);
              }

              const double kernel_start = kEnableGradientPaintDebugTelemetry ?
                                              BLI_time_now_seconds() :
                                              0.0;
              bool pixels_painted = false;
              
              /* Count pixels with non-zero factors before kernel */
              int64_t nonzero_factors_in_row = 0;
              if (kEnableGradientPaintDebugTelemetry) {
                for (const int i : factors.index_range()) {
                  if (factors[i] != 0.0f) {
                    nonzero_factors_in_row++;
                  }
                }
              }
              
              if (is_float_image) {
                pixels_painted = local_kernel_float4.paint(
                    brush, pixel_row, factors, image_buffer, true);
              }
              else {
                pixels_painted = local_kernel_byte4.paint(brush, pixel_row, factors, image_buffer, true);
              }
              if (kEnableGradientPaintDebugTelemetry) {
                local_kernel_time_us_atomic.fetch_add(
                    int64_t((BLI_time_now_seconds() - kernel_start) * 1.0e6),
                    std::memory_order_relaxed);
                
                if (pixels_painted) {
                  g_gradient_telemetry.pixels_kernel_painted.fetch_add(nonzero_factors_in_row, std::memory_order_relaxed);
                }
                else {
                  g_gradient_telemetry.pixels_kernel_skipped.fetch_add(nonzero_factors_in_row, std::memory_order_relaxed);
                }
              }

              if (pixels_painted) {
                ThreadDirtyRegion &dirty_region = tls_dirty_regions.local();
                const int2 start_image_coord(pixel_row.start_image_coordinate.x,
                                             pixel_row.start_image_coordinate.y);
                BLI_rcti_do_minmax_v(&dirty_region.region, start_image_coord);
                BLI_rcti_do_minmax_v(&dirty_region.region,
                                     start_image_coord + int2(pixel_row.num_pixels + 1, 0));
                dirty_region.has_dirty = true;
                if (kEnableGradientPaintDebugTelemetry) {
                  local_rows_painted_success_atomic.fetch_add(1, std::memory_order_relaxed);
                }
              }
            }
          });

      bool has_any_dirty = false;
      rcti merged_dirty_region;
      BLI_rcti_init_minmax(&merged_dirty_region);
      for (const ThreadDirtyRegion &dirty_region : tls_dirty_regions) {
        if (!dirty_region.has_dirty) {
          continue;
        }
        BLI_rcti_do_minmax_rcti(&merged_dirty_region, &dirty_region.region);
        has_any_dirty = true;
      }
      if (has_any_dirty) {
        if (tile_data.flags.dirty) {
          BLI_rcti_do_minmax_rcti(&tile_data.dirty_region, &merged_dirty_region);
        }
        else {
          tile_data.dirty_region = merged_dirty_region;
          tile_data.flags.dirty = true;
        }
      }

      local_rows_total = local_rows_total_atomic.load(std::memory_order_relaxed);
      local_rows_rejected_no_influence = local_rows_rejected_no_influence_atomic.load(
          std::memory_order_relaxed);
      local_rows_painted_attempted = local_rows_painted_attempted_atomic.load(
          std::memory_order_relaxed);
      local_rows_painted_success = local_rows_painted_success_atomic.load(
          std::memory_order_relaxed);
      local_pre_positions_time_us = local_pre_positions_time_us_atomic.load(
          std::memory_order_relaxed);
      local_gradient_calc_time_us = local_gradient_calc_time_us_atomic.load(
          std::memory_order_relaxed);
      local_kernel_time_us = local_kernel_time_us_atomic.load(std::memory_order_relaxed);
    }
    else {
      Vector<float3> pixel_positions;
      Vector<float> factors;
      Vector<float2> screen_coords;

      for (const PackedPixelRow &pixel_row : pixel_rows) {
        if (kEnableGradientPaintDebugTelemetry) {
          local_rows_total++;
        }

        const int row_pixels_num = int(pixel_row.num_pixels);
        if (pixel_positions.capacity() < row_pixels_num) {
          pixel_positions.reserve(row_pixels_num);
        }
        if (factors.capacity() < row_pixels_num) {
          factors.reserve(row_pixels_num);
        }

        const double positions_start = kEnableGradientPaintDebugTelemetry ?
                                           BLI_time_now_seconds() :
                                           0.0;
        pixel_positions.resize(pixel_row.num_pixels);
        calc_pixel_row_positions(positions,
                                 pbvh_data.vert_tris,
                                 node_data.uv_primitives.tri_indices,
                                 node_data.uv_primitives.delta_barycentric_coords,
                                 pixel_row,
                                 pixel_positions);
        if (kEnableGradientPaintDebugTelemetry) {
          local_pre_positions_time_us += int64_t((BLI_time_now_seconds() - positions_start) *
                                                 1.0e6);
        }

        const double gradient_calc_start = kEnableGradientPaintDebugTelemetry ?
                                               BLI_time_now_seconds() :
                                               0.0;
        factors.resize(pixel_positions.size());
        bool has_influence = false;

        /* For debugging: store sample factors from this row */
        float first_factor_raw = 0.0f, first_factor_final = 0.0f;
        float middle_factor_raw = 0.0f, middle_factor_final = 0.0f;
        float last_factor_raw = 0.0f, last_factor_final = 0.0f;
        int pixels_processed = 0;
        static int debug_serial_rejected_count = 0;  /* Static to limit output across all rows */

        if (use_batch_projection) {
          if (screen_coords.capacity() < pixel_positions.size()) {
            screen_coords.reserve(pixel_positions.size());
          }
          screen_coords.resize(pixel_positions.size());

          /* Allocate projection status array */
          Vector<eV3DProjStatus> proj_statuses;
          if (proj_statuses.capacity() < pixel_positions.size()) {
            proj_statuses.reserve(pixel_positions.size());
          }
          proj_statuses.resize(pixel_positions.size());

          ED_view3d_project_float_object_array_with_status(
              &region,
              pixel_positions.as_span(),
              screen_coords.as_mutable_span(),
              proj_statuses.as_mutable_span(),
              V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_NEAR);

          for (const int i : factors.index_range()) {
            const float2 &screen_co = screen_coords[i];
            const bool projection_failed = (proj_statuses[i] != V3D_PROJ_RET_OK);

            /* Early rejection optimization disabled for gradient tool - the gradient calculator
             * will correctly return 0.0f for pixels outside the gradient range. */
            if (projection_failed) {
              factors[i] = 0.0f;
              continue;
            }

            float factor_raw = paint_projected_gradient_factor_with_preprojected(
                calculator, screen_co, symmetry, radial_symmetry);
            float factor = paint_gradient_finalize_factor(brush, factor_raw, clamp_to_range, bstrength, true);
            factors[i] = factor;

            /* Store sample factors for debugging */
            if (pixels_processed == 0) {
              first_factor_raw = factor_raw;
              first_factor_final = factor;
            }
            if (pixels_processed == pixel_positions.size() / 2) {
              middle_factor_raw = factor_raw;
              middle_factor_final = factor;
            }
            last_factor_raw = factor_raw;
            last_factor_final = factor;
            pixels_processed++;

            /* For Gradient Tools, negative factors are valid (pixels "before" gradient start) */
            has_influence |= (factor != 0.0f);
          }
        }
        else {
          /* Non-batch projection path (serial) */
          for (const int i : factors.index_range()) {
            float factor_raw = paint_projected_gradient_factor_with_symmetry(
                &region, calculator, pixel_positions[i], symmetry, radial_symmetry);
            float factor = paint_gradient_finalize_factor(brush, factor_raw, clamp_to_range, bstrength, true);
            factors[i] = factor;

            /* Store sample factors for debugging */
            if (pixels_processed == 0) {
              first_factor_raw = factor_raw;
              first_factor_final = factor;
            }
            if (pixels_processed == pixel_positions.size() / 2) {
              middle_factor_raw = factor_raw;
              middle_factor_final = factor;
            }
            last_factor_raw = factor_raw;
            last_factor_final = factor;
            pixels_processed++;

            /* For Gradient Tools, negative factors are valid (pixels "before" gradient start) */
            has_influence |= (factor != 0.0f);
          }
        }

        /* Log details for first few rejected rows (serial path) */
        if (kEnableGradientPaintDebugTelemetry && !has_influence) {
          if (debug_serial_rejected_count < kMaxDebugRejectedRows) {
            fprintf(stderr,
                    "GRADIENT ROW REJECT #%d (serial): row_idx=%d pixels=%d processed=%d\n"
                    "  first: raw=%.4f final=%.4f\n"
                    "  middle: raw=%.4f final=%.4f\n"
                    "  last: raw=%.4f final=%.4f\n"
                    "  bstrength=%.4f clamp=%d symmetry=%d\n",
                    debug_serial_rejected_count + 1,
                    local_rows_total,
                    int(pixel_row.num_pixels),
                    pixels_processed,
                    first_factor_raw,
                    first_factor_final,
                    middle_factor_raw,
                    middle_factor_final,
                    last_factor_raw,
                    last_factor_final,
                    bstrength,
                    int(clamp_to_range),
                    symmetry);
            fflush(stderr);
            debug_serial_rejected_count++;
          }
        }

        if (kEnableGradientPaintDebugTelemetry) {
          local_gradient_calc_time_us += int64_t((BLI_time_now_seconds() - gradient_calc_start) *
                                                 1.0e6);
        }

        if (!has_influence) {
          if (kEnableGradientPaintDebugTelemetry) {
            local_rows_rejected_no_influence++;
          }
          continue;
        }

        if (kEnableGradientPaintDebugTelemetry) {
          local_rows_painted_attempted++;
        }

        const double kernel_start = kEnableGradientPaintDebugTelemetry ? BLI_time_now_seconds() :
                                                                         0.0;
        bool pixels_painted = false;
        if (is_float_image) {
          pixels_painted = local_kernel_float4.paint(brush, pixel_row, factors, image_buffer, true);
        }
        else {
          pixels_painted = local_kernel_byte4.paint(brush, pixel_row, factors, image_buffer, true);
        }
        if (kEnableGradientPaintDebugTelemetry) {
          local_kernel_time_us += int64_t((BLI_time_now_seconds() - kernel_start) * 1.0e6);
        }

        if (pixels_painted) {
          tile_data.mark_dirty(pixel_row);
          if (kEnableGradientPaintDebugTelemetry) {
            local_rows_painted_success++;
          }
        }
      }
    }

    if (kEnableGradientPaintDebugTelemetry) {
      telemetry_counter_add(g_gradient_telemetry.rows_total, local_rows_total);
      telemetry_counter_add(g_gradient_telemetry.rows_rejected_no_influence,
                            local_rows_rejected_no_influence);
      telemetry_counter_add(g_gradient_telemetry.rows_painted_attempted,
                            local_rows_painted_attempted);
      telemetry_counter_add(g_gradient_telemetry.rows_painted_success, local_rows_painted_success);
      telemetry_counter_add(g_gradient_telemetry.pre_positions_time_us,
                            local_pre_positions_time_us);
      telemetry_counter_add(g_gradient_telemetry.gradient_calc_time_us,
                            local_gradient_calc_time_us);
      telemetry_counter_add(g_gradient_telemetry.kernel_time_us, local_kernel_time_us);
    }

    const double ibuf_release_start = BLI_time_now_seconds();
    BKE_image_release_ibuf(image_data->image, image_buffer, nullptr);
    if (kEnableGradientPaintDebugTelemetry) {
      telemetry_counter_add(g_gradient_telemetry.ibuf_acquire_release_time_us,
                            int64_t((BLI_time_now_seconds() - ibuf_release_start) * 1.0e6));
    }
  };

  if (use_tile_parallel) {
    threading::parallel_for(tiles_to_process.index_range(), 1, [&](const IndexRange range) {
      for (const int tile_index : range) {
        process_tile(*tiles_to_process[tile_index], false);
      }
    });
  }
  else {
    for (UDIMTilePixels *tile_data : tiles_to_process) {
      process_tile(*tile_data, true);
    }
  }

  bool pixels_updated = false;
  for (const UDIMTilePixels &tile_data : node_data.tiles) {
    pixels_updated |= tile_data.flags.dirty;
  }

  node_data.flags.dirty |= pixels_updated;

  /* Debug output every call */
  if (kEnableGradientPaintDebugTelemetry) {
    const int64_t tick = cache ? cache->iteration_count : 0;
    const int64_t pixels_total = g_gradient_telemetry.pixels_total.load(std::memory_order_relaxed);
    const int64_t pixels_rejected_early = g_gradient_telemetry.pixels_rejected_early.load(std::memory_order_relaxed);
    const int64_t pixels_rejected_projection = g_gradient_telemetry.pixels_rejected_projection.load(std::memory_order_relaxed);
    const int64_t pixels_factor_zero = g_gradient_telemetry.pixels_with_factor_zero.load(std::memory_order_relaxed);
    const int64_t pixels_factor_nonzero = g_gradient_telemetry.pixels_with_factor_nonzero.load(std::memory_order_relaxed);
    const int64_t pixels_kernel_painted = g_gradient_telemetry.pixels_kernel_painted.load(std::memory_order_relaxed);
    const int64_t pixels_kernel_skipped = g_gradient_telemetry.pixels_kernel_skipped.load(std::memory_order_relaxed);
    
    std::fprintf(
        stderr,
        "[gradient_paint] tick=%lld rows_total=%lld rej_no_influence=%lld "
        "paint_attempt=%lld paint_success=%lld pre_pos_ms=%.3f "
        "gradient_calc_ms=%.3f kernel_ms=%.3f ibuf_ms=%.3f\n",
        static_cast<long long>(tick),
        static_cast<long long>(g_gradient_telemetry.rows_total.load(std::memory_order_relaxed)),
        static_cast<long long>(
            g_gradient_telemetry.rows_rejected_no_influence.load(std::memory_order_relaxed)),
        static_cast<long long>(
            g_gradient_telemetry.rows_painted_attempted.load(std::memory_order_relaxed)),
        static_cast<long long>(
            g_gradient_telemetry.rows_painted_success.load(std::memory_order_relaxed)),
        double(g_gradient_telemetry.pre_positions_time_us.load(std::memory_order_relaxed)) /
            1000.0,
        double(g_gradient_telemetry.gradient_calc_time_us.load(std::memory_order_relaxed)) /
            1000.0,
        double(g_gradient_telemetry.kernel_time_us.load(std::memory_order_relaxed)) / 1000.0,
        double(g_gradient_telemetry.ibuf_acquire_release_time_us.load(std::memory_order_relaxed)) /
            1000.0);
    
    std::fprintf(
        stderr,
        "[gradient_paint] PIXELS total=%lld rej_early=%lld rej_proj=%lld factor_zero=%lld "
        "factor_nonzero=%lld kernel_painted=%lld kernel_skipped=%lld\n",
        static_cast<long long>(pixels_total),
        static_cast<long long>(pixels_rejected_early),
        static_cast<long long>(pixels_rejected_projection),
        static_cast<long long>(pixels_factor_zero),
        static_cast<long long>(pixels_factor_nonzero),
        static_cast<long long>(pixels_kernel_painted),
        static_cast<long long>(pixels_kernel_skipped));
    std::fflush(stderr);

    /* Reset counters for next call */
    g_gradient_telemetry.rows_total.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.rows_rejected_no_influence.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.rows_painted_attempted.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.rows_painted_success.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pre_positions_time_us.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.gradient_calc_time_us.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.kernel_time_us.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.ibuf_acquire_release_time_us.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pixels_total.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pixels_rejected_early.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pixels_rejected_projection.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pixels_with_factor_zero.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pixels_with_factor_nonzero.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pixels_kernel_painted.store(0, std::memory_order_relaxed);
    g_gradient_telemetry.pixels_kernel_skipped.store(0, std::memory_order_relaxed);
  }
}

static void mark_image_dirty_batched_by_tile(MutableSpan<bke::pbvh::MeshNode> nodes,
                                             const Span<int> dirty_node_indices,
                                             Image &image,
                                             ImageUser &image_user)
{
  Map<short, rcti> dirty_regions_by_tile;

  for (const int node_index : dirty_node_indices) {
    NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[node_index]);
    if (!node_data.flags.dirty) {
      continue;
    }

    for (const UDIMTilePixels &tile_data : node_data.tiles) {
      if (!tile_data.flags.dirty) {
        continue;
      }

      if (rcti *merged_region = dirty_regions_by_tile.lookup_ptr(tile_data.tile_number)) {
        merged_region->xmin = std::min(merged_region->xmin, tile_data.dirty_region.xmin);
        merged_region->xmax = std::max(merged_region->xmax, tile_data.dirty_region.xmax);
        merged_region->ymin = std::min(merged_region->ymin, tile_data.dirty_region.ymin);
        merged_region->ymax = std::max(merged_region->ymax, tile_data.dirty_region.ymax);
      }
      else {
        dirty_regions_by_tile.add(tile_data.tile_number, tile_data.dirty_region);
      }
    }
  }

  bool require_full_update = false;
  ImageUser local_image_user = image_user;
  for (const auto item : dirty_regions_by_tile.items()) {
    ImageTile *image_tile = BKE_image_get_tile(&image, item.key);
    if (image_tile == nullptr) {
      continue;
    }

    local_image_user.tile = item.key;
    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    if (image_buffer->planes == 8) {
      image_buffer->planes = 32;
      require_full_update = true;
    }
    else {
      BKE_image_partial_update_mark_region(&image, image_tile, image_buffer, &item.value);
    }

    BKE_image_release_ibuf(&image, image_buffer, nullptr);
  }

  if (require_full_update) {
    BKE_image_partial_update_mark_full_update(&image);
  }

  for (const int node_index : dirty_node_indices) {
    NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[node_index]);
    if (!node_data.flags.dirty) {
      continue;
    }

    for (UDIMTilePixels &tile_data : node_data.tiles) {
      if (tile_data.flags.dirty) {
        tile_data.clear_dirty();
      }
    }
    node_data.flags.dirty = false;
  }
}

static void undo_region_tiles(
    ImBuf *ibuf, int x, int y, int w, int h, int *tx, int *ty, int *tw, int *th)
{
  int srcx = 0, srcy = 0;
  IMB_rectclip(ibuf, nullptr, &x, &y, &srcx, &srcy, &w, &h);
  *tw = ((x + w - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *th = ((y + h - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *tx = (x >> ED_IMAGE_UNDO_TILE_BITS);
  *ty = (y >> ED_IMAGE_UNDO_TILE_BITS);
}

static void push_undo(const UDIMTileUndo &tile_undo,
                      Image &image,
                      ImageUser &image_user,
                      ImBuf &image_buffer,
                      ImBuf **tmpibuf)
{
  int tilex, tiley, tilew, tileh;
  PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();
  undo_region_tiles(&image_buffer,
                    tile_undo.region.xmin,
                    tile_undo.region.ymin,
                    BLI_rcti_size_x(&tile_undo.region),
                    BLI_rcti_size_y(&tile_undo.region),
                    &tilex,
                    &tiley,
                    &tilew,
                    &tileh);
  for (int ty = tiley; ty <= tileh; ty++) {
    for (int tx = tilex; tx <= tilew; tx++) {
      ED_image_paint_tile_push(undo_tiles,
                               &image,
                               &image_buffer,
                               tmpibuf,
                               &image_user,
                               tx,
                               ty,
                               nullptr,
                               nullptr,
                               true,
                               true);
    }
  }
}

static void do_push_undo_tile(Image &image, ImageUser &image_user, bke::pbvh::Node &node)
{
  NodeData &node_data = bke::pbvh::pixels::node_data_get(node);
  if (node_data.undo_regions.is_empty()) {
    return;
  }

  ImBuf *tmpibuf = nullptr;
  ImageUser local_image_user = image_user;
  for (const UDIMTileUndo &tile_undo : node_data.undo_regions) {
    local_image_user.tile = tile_undo.tile_number;
    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    push_undo(tile_undo, image, image_user, *image_buffer, &tmpibuf);
    BKE_image_release_ibuf(&image, image_buffer, nullptr);
  }
  if (tmpibuf) {
    IMB_freeImBuf(tmpibuf);
  }
}

static void do_push_undo_tiles_batched(Image &image,
                                       ImageUser &image_user,
                                       SculptSession &ss,
                                       MutableSpan<bke::pbvh::MeshNode> nodes,
                                       const IndexMask &node_mask)
{
  Map<short, Vector<rcti>> regions_by_tile;
  node_mask.foreach_index([&](const int i) {
    if (!ss.cache->paint_brush.image_undo_pushed_node_indices.add(i)) {
      return;
    }

    NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[i]);
    for (const UDIMTileUndo &tile_undo : node_data.undo_regions) {
      Vector<rcti> *regions = regions_by_tile.lookup_ptr(tile_undo.tile_number);
      if (regions == nullptr) {
        Vector<rcti> regions;
        regions.append(tile_undo.region);
        regions_by_tile.add(tile_undo.tile_number, std::move(regions));
      }
      else {
        regions->append(tile_undo.region);
      }
    }
  });

  if (regions_by_tile.is_empty()) {
    return;
  }

  ImBuf *tmpibuf = nullptr;
  ImageUser local_image_user = image_user;
  for (const auto item : regions_by_tile.items()) {
    local_image_user.tile = item.key;
    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    Set<uint64_t> undo_tile_coords;
    for (const rcti &region : item.value) {
      int tilex, tiley, tilew, tileh;
      undo_region_tiles(image_buffer,
                        region.xmin,
                        region.ymin,
                        BLI_rcti_size_x(&region),
                        BLI_rcti_size_y(&region),
                        &tilex,
                        &tiley,
                        &tilew,
                        &tileh);
      for (int ty = tiley; ty <= tileh; ty++) {
        for (int tx = tilex; tx <= tilew; tx++) {
          const uint64_t packed_coord = (uint64_t(uint32_t(ty)) << 32) | uint32_t(tx);
          undo_tile_coords.add(packed_coord);
        }
      }
    }

    PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();
    for (const uint64_t packed_coord : undo_tile_coords) {
      const int tx = int(packed_coord & uint64_t(0xFFFFFFFF));
      const int ty = int(packed_coord >> 32);
      ED_image_paint_tile_push(undo_tiles,
                               &image,
                               image_buffer,
                               &tmpibuf,
                               &image_user,
                               tx,
                               ty,
                               nullptr,
                               nullptr,
                               true,
                               true);
    }

    BKE_image_release_ibuf(&image, image_buffer, nullptr);
  }

  if (tmpibuf) {
    IMB_freeImBuf(tmpibuf);
  }
}

/* -------------------------------------------------------------------- */

/** \name Fix non-manifold edge bleeding.
 * \{ */

static Vector<image::TileNumber> collect_dirty_tiles(MutableSpan<bke::pbvh::MeshNode> nodes,
                                                     const IndexMask &node_mask)
{
  Vector<image::TileNumber> dirty_tiles;
  node_mask.foreach_index(
      [&](const int i) { bke::pbvh::pixels::collect_dirty_tiles(nodes[i], dirty_tiles); });
  return dirty_tiles;
}
static void fix_non_manifold_seam_bleeding(bke::pbvh::Tree &pbvh,
                                           Image &image,
                                           ImageUser &image_user,
                                           Span<TileNumber> tile_numbers_to_fix)
{
  if (tile_numbers_to_fix.is_empty()) {
    return;
  }

  Map<image::TileNumber, ImBuf *> buffers;
  ImageUser local_image_user = image_user;
  for (image::TileNumber tile_number : tile_numbers_to_fix) {
    local_image_user.tile = tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &local_image_user, nullptr);
    if (ibuf != nullptr) {
      buffers.add(tile_number, ibuf);
    }
  }

  for (image::TileNumber tile_number : tile_numbers_to_fix) {
    if (buffers.contains(tile_number)) {
      bke::pbvh::pixels::copy_pixels(pbvh, buffers, tile_number);
    }
  }

  for (const auto item : buffers.items()) {
    BKE_image_release_ibuf(&image, item.value, nullptr);
  }
}

static void fix_non_manifold_seam_bleeding(Object &ob,
                                           Image &image,
                                           ImageUser &image_user,
                                           MutableSpan<bke::pbvh::MeshNode> nodes,
                                           const IndexMask &node_mask)
{
  Vector<image::TileNumber> dirty_tiles = collect_dirty_tiles(nodes, node_mask);
  fix_non_manifold_seam_bleeding(*bke::object::pbvh_get(ob), image, image_user, dirty_tiles);
}

/** \} */

ImageData::~ImageData() = default;

std::unique_ptr<ImageData> ImageData::init_active_image(Object &ob,
                                                        PaintModeSettings &paint_mode_settings)
{
  Image *image = nullptr;
  ImageUser *image_user = nullptr;
  if (!BKE_paint_canvas_image_get(&paint_mode_settings, &ob, &image, &image_user)) {
    return nullptr;
  }
  if (image == nullptr || image_user == nullptr) {
    return nullptr;
  }
  
  auto result = std::make_unique<ImageData>();
  result->image = image;
  result->image_user = image_user;
  return result;
}

}  // namespace ed::sculpt_paint::paint::image

using namespace blender::ed::sculpt_paint::paint::image;

bool SCULPT_paint_image_canvas_get(PaintModeSettings &paint_mode_settings,
                                   Object &ob,
                                   Image **r_image,
                                   ImageUser **r_image_user)
{
  *r_image = nullptr;
  *r_image_user = nullptr;

  std::unique_ptr<ImageData> image_data = ImageData::init_active_image(ob, paint_mode_settings);
  if (!image_data) {
    return false;
  }

  *r_image = image_data->image;
  *r_image_user = image_data->image_user;
  return true;
}

bool SCULPT_use_image_paint_brush(PaintModeSettings &settings, Object &ob)
{
  if (!USER_EXPERIMENTAL_TEST(&U, use_sculpt_texture_paint)) {
    return false;
  }
  if (ob.type != OB_MESH) {
    return false;
  }
  Image *image;
  ImageUser *image_user;
  return BKE_paint_canvas_image_get(&settings, &ob, &image, &image_user);
}

bool SCULPT_do_paint_brush_image(const Depsgraph &depsgraph,
                                 PaintModeSettings &paint_mode_settings,
                                 const Sculpt &sd,
                                 Object &ob,
                                 const IndexMask &node_mask)
{
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.runtime->sculpt_session;

  /* Match the regular sculpt color path: the first step initializes symmetry-pass state and
   * should not paint to avoid first-dab artifacts/double-apply. */
  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    return false;
  }

  if (node_mask.is_empty()) {
    return false;
  }

  std::unique_ptr<ImageData> image_data = ImageData::init_active_image(ob, paint_mode_settings);
  if (!image_data) {
    return false;
  }
  int image_width = 0;
  int image_height = 0;
  BKE_image_get_size(image_data->image, image_data->image_user, &image_width, &image_height);
  const int canvas_longest_side_px = std::max(image_width, image_height);
  if (ss.cache != nullptr) {
    ss.cache->paint_brush.image_brush_canvas_longest_side = canvas_longest_side_px;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  const double step_time_start = BLI_time_now_seconds();
  double undo_ms = 0.0;
  double paint_ms = 0.0;
  double collect_dirty_ms = 0.0;
  double seam_ms = 0.0;
  double mark_dirty_ms = 0.0;
  SimpleImageBrushPaintTelemetry paint_telemetry;

  SimpleImageBrushStepSettings step_settings = make_simple_image_brush_step_settings_begin(
      sd, *brush, ss, canvas_longest_side_px);
  double stage_time = BLI_time_now_seconds();
  if (step_settings.push_undo_tiles) {
    do_push_undo_tiles_batched(*image_data->image, *image_data->image_user, ss, nodes, node_mask);
  }
  undo_ms = (BLI_time_now_seconds() - stage_time) * 1000.0;

  stage_time = BLI_time_now_seconds();
  const bool use_heavy_profile_tile_batch =
      ed::sculpt_paint::image::session::should_use_simple_image_brush_heavy_profile(
          canvas_longest_side_px, brush->size);
  const bool use_extended_tile_batch = use_heavy_profile_tile_batch ||
                                       (canvas_longest_side_px >= 4096 && brush->size >= 64) ||
                                       (canvas_longest_side_px >= 2048 && brush->size >= 96);
  if (use_extended_tile_batch) {
    do_paint_pixels_batched_by_tile(
        depsgraph, ob, sd.paint, *brush, image_data.get(), nodes, node_mask, &paint_telemetry);
  }
  else {
    node_mask.foreach_index(
        [&](const int i) {
          do_paint_pixels(depsgraph, ob, sd.paint, *brush, image_data.get(), nodes[i], &paint_telemetry);
        },
        exec_mode::grain_size(1));
  }
  paint_ms = (BLI_time_now_seconds() - stage_time) * 1000.0;

  stage_time = BLI_time_now_seconds();
  bool had_updates = false;
  Vector<int> dirty_node_indices;
  dirty_node_indices.reserve(node_mask.size());
  node_mask.foreach_index([&](const int i) {
    const NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[i]);
    if (!node_data.flags.dirty) {
      return;
    }
    had_updates = true;
    dirty_node_indices.append(i);
  });
  collect_dirty_ms = (BLI_time_now_seconds() - stage_time) * 1000.0;

  if (!had_updates) {
    const double total_ms = (BLI_time_now_seconds() - step_time_start) * 1000.0;
    update_simple_image_brush_step_settings_post_paint(step_settings, had_updates);
    debug_log_simple_image_brush_step(step_settings,
                                      int64_t(node_mask.size()),
                                      int64_t(dirty_node_indices.size()),
                                      had_updates,
                                      total_ms,
                                      undo_ms,
                                      paint_ms,
                                      collect_dirty_ms,
                                      seam_ms,
                                      mark_dirty_ms,
                                      paint_telemetry);
    return false;
  }

  IndexMaskMemory dirty_node_mask_memory;
  const IndexMask dirty_node_mask = IndexMask::from_indices(dirty_node_indices.as_span(),
                                                            dirty_node_mask_memory);

  update_simple_image_brush_step_settings_post_paint(step_settings, had_updates);

  stage_time = BLI_time_now_seconds();
  if (step_settings.apply_seam_fix) {
    fix_non_manifold_seam_bleeding(
        ob, *image_data->image, *image_data->image_user, nodes, dirty_node_mask);
  }
  seam_ms = (BLI_time_now_seconds() - stage_time) * 1000.0;

  stage_time = BLI_time_now_seconds();
  if (step_settings.mark_image_dirty_now) {
    if (use_extended_tile_batch) {
      mark_image_dirty_batched_by_tile(
          nodes, dirty_node_indices.as_span(), *image_data->image, *image_data->image_user);
    }
    else {
      for (const int i : dirty_node_indices) {
        bke::pbvh::pixels::mark_image_dirty(nodes[i], *image_data->image, *image_data->image_user);
      }
    }
  }
  mark_dirty_ms = (BLI_time_now_seconds() - stage_time) * 1000.0;

  const double total_ms = (BLI_time_now_seconds() - step_time_start) * 1000.0;
  debug_log_simple_image_brush_step(step_settings,
                                    int64_t(node_mask.size()),
                                    int64_t(dirty_node_indices.size()),
                                    had_updates,
                                    total_ms,
                                    undo_ms,
                                    paint_ms,
                                    collect_dirty_ms,
                                    seam_ms,
                                    mark_dirty_ms,
                                    paint_telemetry);

  return had_updates;
}

bool SCULPT_do_paint_brush_image_gradient(const Depsgraph &depsgraph,
                                          PaintModeSettings &paint_mode_settings,
                                          const Sculpt &sd,
                                          Object &ob,
                                          const IndexMask &node_mask,
                                          const ARegion *region,
                                          const int gradient_type,
                                          const float2 &start_ss,
                                          const float2 &end_ss,
                                          const float hardness,
                                          const bool clamp_to_range,
                                          const bool push_undo_tiles,
                                          const bool ensure_pixels,
                                          const bool apply_seam_fix,
                                          const bool mark_image_dirty_now,
                                          const bool clip_before_start)
{
  if (region == nullptr) {
    return false;
  }

  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  std::unique_ptr<ImageData> image_data = ImageData::init_active_image(ob, paint_mode_settings);
  if (!image_data) {
    return false;
  }
  /* Ensure PBVH pixel data exists before accessing per-node pixel payload. */
  if (ensure_pixels) {
    bke::pbvh::build_pixels(depsgraph, ob, *image_data->image, *image_data->image_user);
  }

  ed::sculpt_paint::gradient::Params gradient_params;
  gradient_params.type = (gradient_type == WPAINT_GRADIENT_TYPE_LINEAR) ?
                             ed::sculpt_paint::gradient::Type::Linear :
                             ed::sculpt_paint::gradient::Type::Radial;
  gradient_params.space = ed::sculpt_paint::gradient::Space::Screen;
  gradient_params.start_ss = start_ss;
  gradient_params.end_ss = end_ss;
  gradient_params.hardness = hardness;
  gradient_params.clamp_to_range = clamp_to_range;
  gradient_params.curve = nullptr;
  gradient_params.clip_before_start = clip_before_start;
  const std::unique_ptr<ed::sculpt_paint::gradient::Calculator> calculator =
      ed::sculpt_paint::gradient::create(gradient_params);

  const int symmetry = int(SCULPT_mesh_symmetry_xyz_get(ob));
  const Mesh &mesh = *id_cast<Mesh *>(ob.data);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  IndexMaskMemory pixels_node_mask_memory;
  const IndexMask pixels_node_mask = IndexMask::from_predicate(
      node_mask, pixels_node_mask_memory, [&](const int64_t i) {
        return nodes[i].pixels_ != nullptr;
      });
  if (pixels_node_mask.is_empty()) {
    return false;
  }

  if (push_undo_tiles) {
    pixels_node_mask.foreach_index(
        [&](const int i) {
          do_push_undo_tile(*image_data->image, *image_data->image_user, nodes[i]);
        },
        exec_mode::grain_size(1));
  }
  pixels_node_mask.foreach_index(
      [&](const int i) {
        do_paint_pixels_gradient(depsgraph,
                                 ob,
                                 sd.paint,
                                 *brush,
                                 image_data.get(),
                                 nodes[i],
                                 *region,
                                 *calculator,
                                 clamp_to_range,
                                 symmetry,
                                 mesh.radial_symmetry);
      },
      exec_mode::grain_size(1));

  if (apply_seam_fix) {
    fix_non_manifold_seam_bleeding(
        ob, *image_data->image, *image_data->image_user, nodes, pixels_node_mask);
  }

  bool had_updates = false;
  pixels_node_mask.foreach_index([&](const int i) {
    const NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[i]);
    had_updates |= node_data.flags.dirty;
  });

  if (mark_image_dirty_now) {
    pixels_node_mask.foreach_index([&](const int i) {
      bke::pbvh::pixels::mark_image_dirty(nodes[i], *image_data->image, *image_data->image_user);
    });
  }

  return had_updates;
}

void SCULPT_image_paint_push_undo_tiles(const Depsgraph &depsgraph,
                                        PaintModeSettings &paint_mode_settings,
                                        Object &ob,
                                        const IndexMask &node_mask)
{
  std::unique_ptr<ImageData> image_data = ImageData::init_active_image(ob, paint_mode_settings);
  if (!image_data) {
    return;
  }
  bke::pbvh::build_pixels(depsgraph, ob, *image_data->image, *image_data->image_user);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  IndexMaskMemory pixels_node_mask_memory;
  const IndexMask pixels_node_mask = IndexMask::from_predicate(
      node_mask, pixels_node_mask_memory, [&](const int64_t i) {
        return nodes[i].pixels_ != nullptr;
      });
  if (pixels_node_mask.is_empty()) {
    return;
  }

  pixels_node_mask.foreach_index(
      [&](const int i) { do_push_undo_tile(*image_data->image, *image_data->image_user, nodes[i]); },
      exec_mode::grain_size(1));
}

}  // namespace blender
