/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_task.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_workspace_types.h"

#include "BKE_brush.hh"
#include "BKE_colorband.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_library.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_undo_system.hh"

#include "DEG_depsgraph.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "../paint_intern.hh"
#include "paint_image_select_gradient.hh"
#include "paint_image_select_intern.hh"

namespace blender {

static uint64_t image_paint_gradient_settings_revision = 0;

void ED_image_paint_select_gradient_settings_revision_bump()
{
  image_paint_gradient_settings_revision++;
}

/* -------------------------------------------------------------------- */
/** \name Gradient math
 * \{ */

static float image_paint_gradient_sample_t(const float t_raw,
                                           const ImagePaintGradientRepeat repeat)
{
  switch (repeat) {
    case ImagePaintGradientRepeat::Repeat:
      return t_raw - floorf(t_raw);
    case ImagePaintGradientRepeat::Reflect: {
      const float wrapped = fmodf(t_raw, 2.0f);
      return (wrapped > 1.0f) ? (2.0f - wrapped) : wrapped;
    }
    case ImagePaintGradientRepeat::None:
    default:
      return clamp_f(t_raw, 0.0f, 1.0f);
  }
}

static float image_paint_gradient_remap_midpoint(const float t, const float midpoint)
{
  if (t <= 0.0f) {
    return 0.0f;
  }
  if (t >= 1.0f) {
    return 1.0f;
  }

  const float mid = clamp_f(midpoint, 0.001f, 0.999f);

  /* Linear pass-through when the handle is centered. */
  if (fabsf(mid - 0.5f) < 1e-4f) {
    return t;
  }

  /* Two cubic Hermite segments meeting at (mid, 0.5) with C1 continuity.
   *
   * Tangents are assigned by the Catmull-Rom rule over the three knots
   * (0,0), (mid,0.5), (1,1):
   *   - start  : one-sided slope of segment 1  -> m_start = 0.5 / mid
   *   - junction: global slope (1-0)/(1-0)      -> m_mid   = 1.0
   *   - end    : one-sided slope of segment 2  -> m_end   = 0.5 / (1-mid)
   *
   * Because all three tangents are strictly positive and the unique interior
   * critical point of each Hermite segment lies outside [0,1] for every
   * mid in (0,1), both segments are guaranteed monotone -- no flat zones,
   * no overshoot, no derivative discontinuity at the junction.
   *
   * At mid=0.5 the scaled tangents of both segments equal 0.5, so the
   * cubic Hermite degenerates to the identity, matching the linear pass-through.
   *
   * Scaled tangents (slope * interval width):
   *   segment 1: m_start * mid = 0.5,  m_mid * mid = mid
   *   segment 2: m_mid * (1-mid) = (1-mid),  m_end * (1-mid) = 0.5
   */
  if (t <= mid) {
    const float u = t / mid;
    const float u2 = u * u;
    const float u3 = u2 * u;
    /* h10 * 0.5 + h01 * 0.5 + h11 * mid */
    return (u3 - 2.0f * u2 + u) * 0.5f + (-2.0f * u3 + 3.0f * u2) * 0.5f + (u3 - u2) * mid;
  }

  const float im = 1.0f - mid;
  const float u = (t - mid) / im;
  const float u2 = u * u;
  const float u3 = u2 * u;
  /* h00 * 0.5 + h10 * im + h01 * 1.0 + h11 * 0.5 */
  return (2.0f * u3 - 3.0f * u2 + 1.0f) * 0.5f + (u3 - 2.0f * u2 + u) * im +
         (-2.0f * u3 + 3.0f * u2) + (u3 - u2) * 0.5f;
}

/**
 * Project \a sample into the gradient's axis-aligned frame: \a r_u runs along start->end, \a r_w
 * is perpendicular (both in the input space). Returns the axis length, or 0 when the endpoints are
 * closer than \a min_len (degenerate gradient). Used by the conical / diamond / square shapes.
 */
static float image_paint_gradient_axis_frame(const float2 &p0,
                                             const float2 &p1,
                                             const float2 &sample,
                                             const float min_len,
                                             float &r_u,
                                             float &r_w)
{
  const float dx = p1.x - p0.x;
  const float dy = p1.y - p0.y;
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < min_len) {
    r_u = 0.0f;
    r_w = 0.0f;
    return 0.0f;
  }
  const float ux = dx / len;
  const float uy = dy / len;
  const float vx = sample.x - p0.x;
  const float vy = sample.y - p0.y;
  r_u = vx * ux + vy * uy;
  r_w = -vx * uy + vy * ux;
  return len;
}

/**
 * Core gradient parameter evaluation in a generic 2D space. \a p0 and \a p1 are the gradient
 * endpoints and \a sample is the evaluated point, all expressed in the same space. \a min_len is
 * the smallest endpoint separation treated as non-degenerate: 1 pixel for tile-local pixel space,
 * a sub-texel value for global UV space. New gradient shapes are added here, in the single switch.
 *
 * \a axis_scale rescales the input coordinates before evaluation so the radial / conical /
 * diamond / square iso-lines stay isotropic in pixels. In normalized UV space a non-square
 * texture stretches them along the longer axis; passing (width/height, 1) cancels that. The
 * pixel-space path is already isotropic and passes (1, 1).
 */
static float image_paint_gradient_eval_t_generic(const ImagePaintGradientParams &params,
                                                 const float2 &p0_in,
                                                 const float2 &p1_in,
                                                 const float midpoint,
                                                 const float2 &sample_in,
                                                 const float min_len,
                                                 const float2 &axis_scale)
{
  const float2 p0 = p0_in * axis_scale;
  const float2 p1 = p1_in * axis_scale;
  const float2 sample = sample_in * axis_scale;

  float t_raw = 0.0f;

  switch (params.type) {
    case ImagePaintGradientType::Linear: {
      const float dx = p1.x - p0.x;
      const float dy = p1.y - p0.y;
      const float dist_sq = dx * dx + dy * dy;
      if (dist_sq >= min_len * min_len) {
        t_raw = ((sample.x - p0.x) * dx + (sample.y - p0.y) * dy) / dist_sq;
      }
      break;
    }
    case ImagePaintGradientType::Radial: {
      const float radius = len_v2v2(p1, p0);
      if (radius >= min_len) {
        const float offset[2] = {sample.x - p0.x, sample.y - p0.y};
        t_raw = len_v2(offset) / radius;
      }
      break;
    }
    case ImagePaintGradientType::Conical: {
      float u, w;
      if (image_paint_gradient_axis_frame(p0, p1, sample, min_len, u, w) > 0.0f) {
        /* Angle around start, zero along the start->end axis, wrapped to [0, 1). */
        const float a = atan2f(w, u) * float(0.5 / M_PI);
        t_raw = a - floorf(a);
      }
      break;
    }
    case ImagePaintGradientType::Diamond: {
      float u, w;
      const float len = image_paint_gradient_axis_frame(p0, p1, sample, min_len, u, w);
      if (len > 0.0f) {
        /* L1 (Manhattan) distance in the axis frame -> rhombus iso-lines. */
        t_raw = (fabsf(u) + fabsf(w)) / len;
      }
      break;
    }
    case ImagePaintGradientType::Square: {
      float u, w;
      const float len = image_paint_gradient_axis_frame(p0, p1, sample, min_len, u, w);
      if (len > 0.0f) {
        /* L-infinity (Chebyshev) distance in the axis frame -> square iso-lines. */
        t_raw = std::max(fabsf(u), fabsf(w)) / len;
      }
      break;
    }
  }

  const float t = image_paint_gradient_sample_t(t_raw, params.repeat);
  return image_paint_gradient_remap_midpoint(t, midpoint);
}

float image_paint_gradient_eval_t(const ImagePaintGradientParams &params,
                                  const float2 &start_px,
                                  const float2 &end_px,
                                  const float midpoint,
                                  const float px_x,
                                  const float px_y)
{
  /* Tile-local pixel space: gradients shorter than one pixel are treated as degenerate. Pixel
   * coordinates are already isotropic, so no aspect correction is applied. */
  return image_paint_gradient_eval_t_generic(
      params, start_px, end_px, midpoint, float2(px_x, px_y), 1.0f, float2(1.0f, 1.0f));
}

/**
 * Same as #image_paint_gradient_eval_t but endpoints and sample are in global image UV space.
 * \a axis_scale carries the per-tile pixel aspect (width/height, 1) so the shape stays isotropic.
 */
static float image_paint_gradient_eval_t_uv(const ImagePaintGradientParams &params,
                                            const float2 &start_uv,
                                            const float2 &end_uv,
                                            const float midpoint,
                                            const float2 &pixel_uv,
                                            const float2 &axis_scale)
{
  /* Global UV space: endpoints are within [0, tiles]; use a sub-texel degenerate threshold. */
  return image_paint_gradient_eval_t_generic(
      params, start_uv, end_uv, midpoint, pixel_uv, 1e-6f, axis_scale);
}

void image_paint_gradient_eval_color(const ImagePaintGradientParams &params,
                                     const float t,
                                     float r_color[4])
{
  /* Both param builders always supply a color ramp (the tool's embedded ramp or the brush
   * gradient), matching the long-standing 2D gradient-fill invariant. */
  BLI_assert(params.colorband != nullptr);
  BKE_colorband_evaluate(params.colorband, t, r_color);
  /* Respect the per-stop alpha of the ramp, scaled by the global gradient opacity (and the
   * selection mask weight at composite time). This lets a stop fade the gradient to
   * transparent. */
  r_color[3] *= params.opacity;
}

void image_paint_gradient_ensure_colorband(ImagePaintSettings &imapaint)
{
  /* The embedded ramp is zero-initialized in DNA (and in files saved before it existed). Build the
   * default two-stop black -> white ramp on first use so the widget and evaluation always have a
   * valid color band. */
  if (imapaint.gradient_colorband.tot == 0) {
    BKE_colorband_init(&imapaint.gradient_colorband, true);
  }
}

ImagePaintGradientParams image_paint_gradient_params_from_imapaint(
    const ImagePaintSettings &imapaint)
{
  ImagePaintGradientParams params;
  params.type = ImagePaintGradientType(imapaint.gradient_type);
  params.repeat = ImagePaintGradientRepeat(imapaint.gradient_repeat);
  params.blend_mode = IMB_BlendMode(imapaint.gradient_blend_mode);
  params.opacity = imapaint.gradient_opacity;
  /* The tool always uses its own embedded color ramp. The const_cast is safe: the ramp is only
   * read (via #BKE_colorband_evaluate, whose legacy signature takes a non-const pointer). */
  params.colorband = const_cast<ColorBand *>(&imapaint.gradient_colorband);
  return params;
}

ImagePaintGradientParams image_paint_gradient_params_from_brush(const Paint *paint,
                                                                const Brush *brush)
{
  ImagePaintGradientParams params;
  params.type = (brush->gradient_fill_mode == BRUSH_GRADIENT_LINEAR) ?
                    ImagePaintGradientType::Linear :
                    ImagePaintGradientType::Radial;
  params.repeat = ImagePaintGradientRepeat::None;
  params.blend_mode = IMB_BlendMode(brush->blend);
  params.opacity = BKE_brush_alpha_get(paint, brush);
  params.colorband = brush->gradient;
  return params;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Regions
 * \{ */

static void image_paint_gradient_sanitize_region(rcti &region, const int tile_w, const int tile_h)
{
  region.xmin = std::max(region.xmin, 0);
  region.ymin = std::max(region.ymin, 0);
  region.xmax = std::min(region.xmax, tile_w);
  region.ymax = std::min(region.ymax, tile_h);
  BLI_rcti_sanitize(&region);
}

/**
 * Tile-local pixel bounds of the active selection mask, expanded for blend feathering. Returns
 * false and sets \a r_region empty when the tile has no selection. Shared by the backup and
 * work-region computations so the masked-bounds logic lives in one place.
 */
static bool image_paint_gradient_selection_bounds_region(const Image *image,
                                                         const int tile_number,
                                                         const int tile_w,
                                                         const int tile_h,
                                                         rcti &r_region)
{
  int sel_min[2], sel_max[2];
  if (!BKE_image_paint_selection_mask_bounds(image, tile_number, sel_min, sel_max)) {
    BLI_rcti_init(&r_region, 0, 0, 0, 0);
    return false;
  }
  const PaintSelectionEdgePolicy &edge_policy = BKE_image_paint_selection_edge_policy_get(image);
  BKE_image_paint_selection_bounds_expand_for_blend(sel_min, sel_max, tile_w, tile_h, edge_policy);
  BLI_rcti_init(&r_region, sel_min[0], sel_max[0] + 1, sel_min[1], sel_max[1] + 1);
  return true;
}

void image_paint_gradient_calc_work_region(const Scene * /*scene*/,
                                           const Image *image,
                                           const int tile_number,
                                           const int tile_w,
                                           const int tile_h,
                                           const rcti *region_override,
                                           rcti &r_region)
{
  if (BKE_image_paint_selection_mask_has_any(image)) {
    /* Masked gradient: paint every pixel inside the selection bounds. Per-pixel mask
     * weights discard pixels outside the mask; t is still evaluated from global coords. */
    image_paint_gradient_selection_bounds_region(image, tile_number, tile_w, tile_h, r_region);
  }
  else {
    /* Without a mask the gradient spans the entire tile; t is evaluated per pixel globally. */
    BLI_rcti_init(&r_region, 0, tile_w, 0, tile_h);
  }

  if (region_override) {
    BLI_rcti_isect(&r_region, region_override, &r_region);
  }
}

/**
 * Work region of \a tile_number: the selection bounds when a mask is active, the whole tile
 * otherwise.
 *
 * \note There is deliberately no bounding-box culling against the gradient vector. That vector
 * only positions the ramp, it does not bound the painted area. With
 * #ImagePaintGradientRepeat::None the parameter is *clamped* to [0, 1] (see
 * #image_paint_gradient_sample_t), so pixels past the drag still receive the ramp's end color
 * rather than being skipped, and Repeat/Reflect wrap it indefinitely. On top of that, Conical
 * parameterizes by the angle around the start point and so sweeps the whole plane, Linear is
 * unbounded perpendicular to its axis, and Diamond/Square reach up to sqrt(2) past the Euclidean
 * radius because they use the L1 and L-infinity metrics. The bounding box previously used here
 * dropped tiles in every one of those cases, leaving them silently unpainted. Culling only
 * becomes sound once the ramp is known not to contribute (a fully transparent end stop, say),
 * which cannot be decided from the gradient geometry alone.
 */
static void image_paint_gradient_calc_work_region_uv(const Scene * /*scene*/,
                                                    const Image *image,
                                                    const int tile_number,
                                                    const int tile_w,
                                                    const int tile_h,
                                                    const ImagePaintGradientParams & /*params*/,
                                                    const float2 & /*start_uv*/,
                                                    const float2 & /*end_uv*/,
                                                    rcti &r_region)
{
  BLI_rcti_init(&r_region, 0, 0, 0, 0);

  if (BKE_image_paint_selection_mask_has_any(image)) {
    if (image_paint_gradient_selection_bounds_region(
            image, tile_number, tile_w, tile_h, r_region))
    {
      image_paint_gradient_sanitize_region(r_region, tile_w, tile_h);
    }
    return;
  }

  BLI_rcti_init(&r_region, 0, tile_w, 0, tile_h);
  image_paint_gradient_sanitize_region(r_region, tile_w, tile_h);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Backup / compositing
 * \{ */

static void image_paint_gradient_restore_full_backup(ImBuf *canvas, const ImBuf *backup)
{
  if (!canvas || !backup || canvas->x != backup->x || canvas->y != backup->y) {
    return;
  }
  if (canvas->float_buffer.data && backup->float_buffer.data) {
    IMB_copy_rect(canvas, backup, int2(0, 0), int2(0, 0), int2(canvas->x, canvas->y));
  }
  else if (canvas->byte_buffer.data && backup->byte_buffer.data) {
    IMB_copy_rect(canvas, backup, int2(0, 0), int2(0, 0), int2(canvas->x, canvas->y));
  }
}

struct GradientRowTaskData {
  const Scene *scene;
  const Image *image;
  int tile_number;
  bool use_selection_mask;
  bool use_global_uv = false;
  ImagePaintGradientParams params;
  float2 start_px;
  float2 end_px;
  float2 start_uv;
  float2 end_uv;
  float midpoint;
  ImBuf *canvas_ibuf;
  /** Writable pixel buffers; acquired once before parallel work (not thread-safe in ImBuf). */
  float *canvas_float_data = nullptr;
  uchar *canvas_byte_data = nullptr;
  const ColorSpace *byte_colorspace = nullptr;
  /** Precomputed t -> color table (RGBA, straight alpha). For byte canvases the RGB is already
   * converted to the buffer color space; for float canvases it is scene-linear. This hoists the
   * expensive per-pixel colorband / sRGB / color-space evaluation out of the rasterization
   * loop. */
  Array<float4> color_lut;
  rcti work_region;
};

static bool image_paint_gradient_prepare_canvas_write(GradientRowTaskData &task_data)
{
  ImBuf *ibuf = task_data.canvas_ibuf;
  if (ibuf->float_buffer.data) {
    task_data.canvas_float_data = ibuf->float_data_for_write();
    return task_data.canvas_float_data != nullptr;
  }
  if (ibuf->byte_buffer.data) {
    task_data.canvas_byte_data = ibuf->byte_data_for_write();
    task_data.byte_colorspace = ibuf->byte_buffer.colorspace;
    return task_data.canvas_byte_data != nullptr;
  }
  return false;
}

/**
 * Number of entries in the gradient color lookup table. The gradient color is a 1D function of
 * the parameter t, so it is evaluated once per table entry instead of once per pixel. 1024
 * entries keep the quantization error below the perceptible threshold even for color ramps with
 * sharp stops.
 */
static constexpr int GRADIENT_COLOR_LUT_SIZE = 1024;

/**
 * Fill \a lut with the gradient color for evenly spaced t in [0, 1]. When \a byte_colorspace is
 * non-null the RGB is converted into that color space (byte canvas); otherwise it stays
 * scene-linear (float canvas).
 */
static void image_paint_gradient_build_color_lut(const ImagePaintGradientParams &params,
                                                 const ColorSpace *byte_colorspace,
                                                 Array<float4> &lut)
{
  const int n = int(lut.size());
  const float inv = (n > 1) ? 1.0f / float(n - 1) : 0.0f;
  for (int i = 0; i < n; i++) {
    const float t = float(i) * inv;
    float col[4];
    image_paint_gradient_eval_color(params, t, col);
    if (byte_colorspace) {
      IMB_colormanagement_scene_linear_to_colorspace_v3(col, byte_colorspace);
    }
    lut[i] = float4(col);
  }
}

static const float4 &image_paint_gradient_lut_sample(const GradientRowTaskData &data,
                                                     const float t)
{
  const int n = int(data.color_lut.size());
  /* t is already clamped/wrapped to [0, 1] by #image_paint_gradient_eval_t. */
  int idx = int(t * float(n - 1) + 0.5f);
  idx = std::clamp(idx, 0, n - 1);
  return data.color_lut[idx];
}

static float image_paint_gradient_row_eval_t(const GradientRowTaskData &data,
                                             const int px,
                                             const int py)
{
  if (data.use_global_uv) {
    const float2 origin = image_select_udim_tile_uv_origin(data.tile_number);
    const float2 pixel_uv(origin.x + (float(px) + 0.5f) / float(data.canvas_ibuf->x),
                          origin.y + (float(py) + 0.5f) / float(data.canvas_ibuf->y));
    /* UV is normalized per axis, so a non-square tile makes the metric anisotropic. Correct it by
     * the pixel aspect (width/height, 1) so radial / conical / diamond / square stay round. */
    const float aspect = (data.canvas_ibuf->y > 0) ?
                             float(data.canvas_ibuf->x) / float(data.canvas_ibuf->y) :
                             1.0f;
    return image_paint_gradient_eval_t_uv(
        data.params, data.start_uv, data.end_uv, data.midpoint, pixel_uv, float2(aspect, 1.0f));
  }
  return image_paint_gradient_eval_t(
      data.params, data.start_px, data.end_px, data.midpoint, float(px), float(py));
}

static void image_paint_gradient_composite_pixel_float(const GradientRowTaskData &data,
                                                     const int px,
                                                     const int py,
                                                     float *dst_px)
{
  const float t = image_paint_gradient_row_eval_t(data, px, py);
  const float4 &grad_col = image_paint_gradient_lut_sample(data, t);

  float sel_w = 1.0f;
  if (data.use_selection_mask) {
    sel_w = BKE_image_paint_selection_blend_sample_bilinear(
        data.image, data.tile_number, float(px) + 0.5f, float(py) + 0.5f);
    if (sel_w <= 0.0f) {
      return;
    }
  }

  float brush_col[4] = {grad_col[0], grad_col[1], grad_col[2], grad_col[3]};
  brush_col[3] *= sel_w;
  mul_v3_fl(brush_col, brush_col[3]);
  IMB_blend_color_float(dst_px, dst_px, brush_col, data.params.blend_mode);
}

static void image_paint_gradient_composite_pixel_byte(const GradientRowTaskData &data,
                                                      const int px,
                                                      const int py,
                                                      uchar *dst_px)
{
  const float t = image_paint_gradient_row_eval_t(data, px, py);
  /* LUT RGB is already converted to the canvas color space (see build_color_lut). */
  const float4 &grad_col = image_paint_gradient_lut_sample(data, t);

  float sel_w = 1.0f;
  if (data.use_selection_mask) {
    sel_w = BKE_image_paint_selection_blend_sample_bilinear(
        data.image, data.tile_number, float(px) + 0.5f, float(py) + 0.5f);
    if (sel_w <= 0.0f) {
      return;
    }
  }

  float brush_col[4] = {grad_col[0], grad_col[1], grad_col[2], grad_col[3]};
  brush_col[3] *= sel_w;
  uchar brush_byte[4];
  rgba_float_to_uchar(brush_byte, brush_col);
  IMB_blend_color_byte(dst_px, dst_px, brush_byte, data.params.blend_mode);
}

static void image_paint_gradient_row_task(void *__restrict userdata,
                                          const int y,
                                          const TaskParallelTLS *__restrict /*tls*/)
{
  GradientRowTaskData *data = static_cast<GradientRowTaskData *>(userdata);
  if (y < data->work_region.ymin || y >= data->work_region.ymax) {
    return;
  }

  ImBuf *ibuf = data->canvas_ibuf;
  const int ibuf_x = ibuf->x;
  if (data->canvas_float_data) {
    float *row = data->canvas_float_data + 4 * (size_t(y) * ibuf_x);
    for (int px = data->work_region.xmin; px < data->work_region.xmax; px++) {
      image_paint_gradient_composite_pixel_float(*data, px, y, row + 4 * px);
    }
  }
  else if (data->canvas_byte_data) {
    uchar *row = data->canvas_byte_data + 4 * (size_t(y) * ibuf_x);
    for (int px = data->work_region.xmin; px < data->work_region.xmax; px++) {
      image_paint_gradient_composite_pixel_byte(*data, px, y, row + 4 * px);
    }
  }
}

/**
 * Shared rasterization core. Acquires write access to the canvas, builds the t -> color table
 * once, and runs the threaded per-row composite over \a task_data.work_region. The caller is
 * responsible for filling the coordinate fields (px or uv) and setting
 * #GradientRowTaskData::use_global_uv.
 */
static void image_paint_gradient_rasterize(GradientRowTaskData &task_data)
{
  if (BLI_rcti_is_empty(&task_data.work_region)) {
    return;
  }
  if (!image_paint_gradient_prepare_canvas_write(task_data)) {
    return;
  }

  /* Build the t -> color table once; the rasterization loop only does a table lookup per pixel. */
  task_data.color_lut.reinitialize(GRADIENT_COLOR_LUT_SIZE);
  image_paint_gradient_build_color_lut(
      task_data.params, task_data.byte_colorspace, task_data.color_lut);

  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.min_iter_per_thread = 8;
  BLI_task_parallel_range(task_data.work_region.ymin,
                          task_data.work_region.ymax,
                          &task_data,
                          image_paint_gradient_row_task,
                          &settings);
}

void image_paint_gradient_apply_region(const Scene *scene,
                                       Image *image,
                                       const int tile_number,
                                       ImBuf *canvas_ibuf,
                                       const ImagePaintGradientParams &params,
                                       const float2 &start_px,
                                       const float2 &end_px,
                                       const float midpoint,
                                       const rcti &work_region)
{
  GradientRowTaskData task_data{};
  task_data.scene = scene;
  task_data.image = image;
  task_data.tile_number = tile_number;
  task_data.use_selection_mask = BKE_image_paint_selection_mask_has_any(image);
  task_data.use_global_uv = false;
  task_data.params = params;
  task_data.start_px = start_px;
  task_data.end_px = end_px;
  task_data.midpoint = midpoint;
  task_data.canvas_ibuf = canvas_ibuf;
  task_data.work_region = work_region;

  image_paint_gradient_rasterize(task_data);
}

static void image_paint_gradient_apply_region_uv(const Scene *scene,
                                                 Image *image,
                                                 const int tile_number,
                                                 ImBuf *canvas_ibuf,
                                                 const ImagePaintGradientParams &params,
                                                 const float2 &start_uv,
                                                 const float2 &end_uv,
                                                 const float midpoint,
                                                 const rcti &work_region)
{
  GradientRowTaskData task_data{};
  task_data.scene = scene;
  task_data.image = image;
  task_data.tile_number = tile_number;
  task_data.use_selection_mask = BKE_image_paint_selection_mask_has_any(image);
  task_data.use_global_uv = true;
  task_data.params = params;
  task_data.start_uv = start_uv;
  task_data.end_uv = end_uv;
  task_data.midpoint = midpoint;
  task_data.canvas_ibuf = canvas_ibuf;
  task_data.work_region = work_region;

  image_paint_gradient_rasterize(task_data);
}

/**
 * Compute the tile-local pixel bounding box that is currently visible in the viewport.
 * Used to limit preview updates to the visible area during interactive dragging,
 * avoiding full-tile processing on every MOUSEMOVE event.
 *
 * A 1-pixel border is added on each side to prevent sub-pixel cracks at the edges.
 */
static rcti image_paint_gradient_viewport_clip_px(const ARegion *region,
                                                  const int tile_number,
                                                  const int tile_w,
                                                  const int tile_h)
{
  const float2 origin = image_select_udim_tile_uv_origin(tile_number);
  const rctf &cur = region->v2d.cur;
  const float fw = float(tile_w);
  const float fh = float(tile_h);

  rcti clip;
  BLI_rcti_init(&clip,
                int(std::floor((cur.xmin - origin.x) * fw)) - 1,
                int(std::ceil((cur.xmax - origin.x) * fw)) + 1,
                int(std::floor((cur.ymin - origin.y) * fh)) - 1,
                int(std::ceil((cur.ymax - origin.y) * fh)) + 1);
  clip.xmin = std::max(clip.xmin, 0);
  clip.ymin = std::max(clip.ymin, 0);
  clip.xmax = std::min(clip.xmax, tile_w);
  clip.ymax = std::min(clip.ymax, tile_h);
  BLI_rcti_sanitize(&clip);
  return clip;
}

/**
 * Apply a gradient preview to \a canvas_ibuf for one tile.
 *
 * \param viewport_clip  Optional pixel rectangle to limit processing to the viewport-visible
 *                       region.  Pass nullptr for a full-tile update (e.g. on mouse release).
 * \param r_work_region  Output: the actual pixel region that was restored and painted.
 *                       Empty when the gradient does not intersect the tile.
 *
 * \a backup_ibuf is the full-tile, pixel-identical copy of the canvas captured at session start
 * (coordinates are 1:1 with \a canvas_ibuf). The function restores only the pixels it is about to
 * repaint, then applies the gradient. Restoring from the original every frame keeps each blend a
 * fresh composite over the unmodified texture, so non-`MIX` blend modes cannot accumulate.
 */
static void image_paint_gradient_apply_preview_uv(const Scene *scene,
                                                  Image *image,
                                                  const int tile_number,
                                                  ImBuf *canvas_ibuf,
                                                  const ImBuf *backup_ibuf,
                                                  const ImagePaintGradientParams &params,
                                                  const float2 &start_uv,
                                                  const float2 &end_uv,
                                                  const float midpoint,
                                                  const rcti *viewport_clip,
                                                  rcti &r_work_region)
{
  rcti work_region;
  image_paint_gradient_calc_work_region_uv(scene,
                                           image,
                                           tile_number,
                                           canvas_ibuf->x,
                                           canvas_ibuf->y,
                                           params,
                                           start_uv,
                                           end_uv,
                                           work_region);

  if (viewport_clip && !BLI_rcti_is_empty(viewport_clip)) {
    BLI_rcti_isect(&work_region, viewport_clip, &work_region);
  }

  r_work_region = work_region;

  if (BLI_rcti_is_empty(&work_region)) {
    return;
  }

  /* Restore only the sub-region we are about to re-paint so that backup pixels outside the
   * work_region are never needlessly copied. The backup is the full tile, so coordinates are
   * 1:1 with the canvas. */
  if (backup_ibuf) {
    IMB_copy_rect(canvas_ibuf,
                  backup_ibuf,
                  int2(work_region.xmin, work_region.ymin),
                  int2(work_region.xmin, work_region.ymin),
                  int2(BLI_rcti_size_x(&work_region), BLI_rcti_size_y(&work_region)));
  }

  image_paint_gradient_apply_region_uv(scene,
                                       image,
                                       tile_number,
                                       canvas_ibuf,
                                       params,
                                       start_uv,
                                       end_uv,
                                       midpoint,
                                       work_region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session state
 * \{ */

struct ImageSelectGradientTileData {
  int tile_number = 1001;
  ImageUser iuser = {};
  /** Full-tile, pixel-identical copy of the canvas at session start. Doubles as the preview
   * restore source (every frame composites over the original) and the undo snapshot on commit. */
  ImBuf *undo_ibuf = nullptr;
};

/**
 * Runtime state of a floating gradient preview.
 *
 * Derives from the shared base like the three lifted-fragment tools, even though it uses only part
 * of it: #owner_sima, #owner_region_type and #draw_handle used to be duplicated here verbatim.
 * #iuser is unused (each backed tile carries its own #ImageSelectGradientTileData::iuser),
 * #is_dragging is unused (the drag lives in the operator's #GradientDragData), and #undo_begun
 * stays false for the whole session -- see the note on that member.
 */
struct ImageSelectGradientState : public PaintSelectFloatingSession {
  static constexpr PaintSelectTool tool_type = PaintSelectTool::Gradient;
  ImageSelectGradientState() : PaintSelectFloatingSession(tool_type) {}

  Vector<ImageSelectGradientTileData> tiles;

  float2 start_uv = {0.0f, 0.0f};
  float2 end_uv = {0.0f, 0.0f};
  float midpoint = 0.5f;

  double last_preview_time = 0.0;
  bool preview_pending = false;
  uint64_t applied_settings_revision = 0;

  /** #bToolRef.idname of the tool that owned the session at start (empty if it could not be
   * resolved). The modal commits and tears down once the active tool no longer matches this, so
   * switching to any other tool bakes the preview instead of leaving the handles floating. */
  char owner_tool_idname[64] = {};
};

static ImageSelectGradientTileData *image_select_gradient_find_tile(
    ImageSelectGradientState *state, const int tile_number)
{
  for (ImageSelectGradientTileData &tile : state->tiles) {
    if (tile.tile_number == tile_number) {
      return &tile;
    }
  }
  return nullptr;
}

static void image_select_gradient_free_tile_data(ImageSelectGradientTileData &tile)
{
  if (tile.undo_ibuf) {
    IMB_freeImBuf(tile.undo_ibuf);
    tile.undo_ibuf = nullptr;
  }
}

void image_select_gradient_state_free(ImageSelectGradientState *state)
{
  if (!state) {
    return;
  }
  image_select_floating_draw_handle_clear(*state);
  for (ImageSelectGradientTileData &tile : state->tiles) {
    image_select_gradient_free_tile_data(tile);
  }
  state->tiles.clear();
  MEM_delete(state);
}

void image_select_gradient_session_free(PaintSelectFloatingSession *session)
{
  /* Only reached through #image_select_floating_session_free, which dispatches on the tag, so the
   * downcast is the one the tag guarantees. */
  BLI_assert(session->tool == ImageSelectGradientState::tool_type);
  image_select_gradient_state_free(static_cast<ImageSelectGradientState *>(session));
}

bool image_select_gradient_is_floating_in_space(const SpaceImage *sima)
{
  return image_select_session_get<ImageSelectGradientState>(sima) != nullptr;
}

bool image_select_gradient_is_floating(bContext *C)
{
  return image_select_gradient_is_floating_in_space(CTX_wm_space_image(C));
}

static ImageSelectGradientState *image_select_gradient_state_get(SpaceImage *sima)
{
  return image_select_session_get<ImageSelectGradientState>(sima);
}

static float2 image_select_gradient_mid_uv(const ImageSelectGradientState *state)
{
  return state->start_uv + state->midpoint * (state->end_uv - state->start_uv);
}

static ImagePaintGradientParams image_select_gradient_current_params(const Scene *scene)
{
  return image_paint_gradient_params_from_imapaint(scene->toolsettings->imapaint);
}

/* One-way sync: imapaint -> operator props (all marked PROP_SKIP_SAVE).
 * Runtime params are always read back from imapaint via image_select_gradient_current_params(),
 * so op->ptr values are never used during the session. This sync keeps the operator redo
 * panel consistent and allows future F9 / last-operator repeat to reflect panel state. */
static void image_select_gradient_sync_op_from_imapaint(wmOperator *op, const Scene *scene)
{
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  RNA_enum_set(op->ptr, "gradient_type", imapaint.gradient_type);
  RNA_enum_set(op->ptr, "repeat", imapaint.gradient_repeat);
  RNA_enum_set(op->ptr, "blend_mode", imapaint.gradient_blend_mode);
  RNA_float_set(op->ptr, "opacity", imapaint.gradient_opacity);
}

static bool image_select_gradient_mouse_to_global_uv(const ARegion *region,
                                                     Image *ima,
                                                     const wmEvent *event,
                                                     int &r_tile_number,
                                                     float2 &r_uv)
{
  /* Derive region-relative coordinates from the window-absolute #wmEvent.xy. While the modal runs
   * the context region (and therefore #wmEvent.mval) may belong to the header or N-panel when the
   * pointer leaves the image; #wmEvent.xy stays valid and keeps the gradient anchored to the
   * image region passed in here. */
  const float mx = float(event->xy[0] - region->winrct.xmin);
  const float my = float(event->xy[1] - region->winrct.ymin);

  float uv[2];
  ui::view2d_region_to_view(&region->v2d, mx, my, &uv[0], &uv[1]);

  float uv_origin[2];
  int tile_number = BKE_image_get_tile_from_pos(ima, uv, uv, uv_origin);
  if (tile_number == 0) {
    const ImageTile *first_tile = static_cast<const ImageTile *>(ima->tiles.first);
    if (first_tile) {
      tile_number = first_tile->tile_number;
    }
  }

  r_uv = float2(uv[0], uv[1]);
  r_tile_number = tile_number;
  return tile_number != 0;
}

/**
 * Tiles the gradient can touch. Without a selection mask that is every tile of the image; see
 * #image_paint_gradient_calc_work_region_uv for why the gradient geometry cannot cull any.
 */
static void image_select_gradient_collect_affected_tiles(
    Image *ima,
    Scene *scene,
    const ImagePaintGradientParams & /*params*/,
    const float2 & /*start_uv*/,
    const float2 & /*end_uv*/,
    Vector<int> &r_tile_numbers)
{
  r_tile_numbers.clear();
  if (!ima) {
    return;
  }

  const bool use_selection_mask = BKE_image_paint_selection_mask_has_any(ima);
  /* Without the multi-UDIM option the gradient is confined to the active tile: the vector may still
   * extend past it, but only that tile's pixels are painted. The active tile is tracked by
   * #Image.active_tile_index, matching the move / transform / mask selection tools. */
  const bool multi_udim = scene && scene->toolsettings->imapaint.gradient_multi_udim;
  int active_tile_number = 1001;
  if (!multi_udim) {
    const ImageTile *active_tile = static_cast<const ImageTile *>(
        BLI_findlink(&ima->tiles, ima->active_tile_index));
    if (active_tile) {
      active_tile_number = active_tile->tile_number;
    }
  }

  for (const ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
    if (!multi_udim && tile->tile_number != active_tile_number) {
      continue;
    }
    if (use_selection_mask) {
      int sel_min[2], sel_max[2];
      if (!BKE_image_paint_selection_mask_bounds(ima, tile->tile_number, sel_min, sel_max)) {
        continue;
      }
    }
    /* Unmasked: the tile is affected as a whole, see #image_paint_gradient_calc_work_region_uv. */
    r_tile_numbers.append(tile->tile_number);
  }
}

static bool image_select_gradient_init_tile_backup(bContext * /*C*/,
                                                   Image *ima,
                                                   SpaceImage *sima,
                                                   const int tile_number,
                                                   ImageSelectGradientTileData &tile)
{
  tile.tile_number = tile_number;
  tile.iuser = sima->iuser;
  tile.iuser.tile = tile_number;

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile.iuser, &lock);
  if (!ibuf) {
    return false;
  }

  /* Full-tile snapshot: used both as the preview restore source and the undo image. */
  tile.undo_ibuf = IMB_dupImBuf(ibuf);
  BKE_image_release_ibuf(ima, ibuf, lock);
  return tile.undo_ibuf != nullptr;
}

static void image_select_gradient_restore_tile_backup(bContext *C,
                                                      Image *ima,
                                                      const ImageSelectGradientTileData &tile)
{
  if (!ima || !tile.undo_ibuf) {
    return;
  }
  void *lock = nullptr;
  ImageUser iuser = tile.iuser;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
  if (!ibuf) {
    return;
  }
  image_paint_gradient_restore_full_backup(ibuf, tile.undo_ibuf);
  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  /* Mark the whole tile as changed so the partial-update GPU texture cache re-uploads the restored
   * pixels. #BKE_image_mark_dirty only flags the ImBuf; it does not refresh the GPU texture, which
   * tracks changes solely through #BKE_image_partial_update_mark_region. Without this, dropping a
   * tile mid-session (for example disabling "All UDIM Tiles") reverts the ImBuf but leaves the
   * stale preview on screen. */
  rcti tile_region;
  BLI_rcti_init(&tile_region, 0, ibuf->x, 0, ibuf->y);
  const ImageTile *itile = BKE_image_get_tile(ima, tile.tile_number);
  BKE_image_partial_update_mark_region(ima, itile, ibuf, &tile_region);
  BKE_image_mark_dirty(ima, ibuf);
  BKE_image_release_ibuf(ima, ibuf, lock);
  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

static void image_select_gradient_sync_tile_backups(bContext *C,
                                                    ImageSelectGradientState *state,
                                                    const ImagePaintGradientParams &params)
{
  Image *ima = state->owner_sima->image;
  Scene *scene = CTX_data_scene(C);
  SpaceImage *sima = state->owner_sima;

  Vector<int> needed_tiles;
  image_select_gradient_collect_affected_tiles(
      ima, scene, params, state->start_uv, state->end_uv, needed_tiles);

  for (int i = state->tiles.size() - 1; i >= 0; i--) {
    ImageSelectGradientTileData &tile = state->tiles[i];
    if (needed_tiles.contains(tile.tile_number)) {
      continue;
    }
    image_select_gradient_restore_tile_backup(C, ima, tile);
    image_select_gradient_free_tile_data(tile);
    state->tiles.remove_and_reorder(i);
  }

  for (const int tile_number : needed_tiles) {
    if (image_select_gradient_find_tile(state, tile_number)) {
      continue;
    }
    ImageSelectGradientTileData tile;
    if (image_select_gradient_init_tile_backup(C, ima, sima, tile_number, tile)) {
      state->tiles.append(tile);
    }
  }
}

static void image_select_gradient_commit_floating_ops(bContext *C)
{
  /* Every other tool's session: each of them holds an open image undo step, and
   * #image_select_gradient_apply_session opens one of its own, which would free theirs. A gradient
   * session already floating here is left alone -- #image_select_gradient_begin_session restores
   * and replaces it, which is not the same as the takeover teardown. */
  image_select_floating_sessions_end(C, CTX_wm_space_image(C), PaintSelectTool::Gradient);
}

/**
 * Core preview dispatch: restore backup pixels and apply gradient for every affected tile.
 *
 * \param use_viewport_clip  When true the update is limited to the pixels currently visible
 *   in the active viewport region.  This makes interactive drag (MOUSEMOVE) fast even on
 *   large textures because only the visible sub-region is restored and repainted each frame.
 *   On mouse release (force=true path) this flag is false so the full tile is updated,
 *   keeping off-screen pixels coherent with the final handle positions.
 */
static void image_select_gradient_run_preview(bContext *C,
                                              ImageSelectGradientState *state,
                                              const ImagePaintGradientParams &params,
                                              const bool commit_to_image,
                                              const bool use_viewport_clip)
{
  Image *ima = state->owner_sima->image;
  Scene *scene = CTX_data_scene(C);

  image_select_gradient_sync_tile_backups(C, state, params);

  /* Obtain the viewport region once; nullptr disables clipping (full-tile update). */
  const ARegion *vp_region = (use_viewport_clip && !commit_to_image) ? CTX_wm_region(C) :
                                                                        nullptr;

  for (ImageSelectGradientTileData &tile : state->tiles) {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile.iuser, &lock);
    if (!ibuf) {
      continue;
    }

    /* Compute viewport clip for this tile (pixels currently visible on screen). */
    rcti viewport_clip;
    const rcti *viewport_clip_ptr = nullptr;
    if (vp_region) {
      viewport_clip = image_paint_gradient_viewport_clip_px(
          vp_region, tile.tile_number, ibuf->x, ibuf->y);
      if (!BLI_rcti_is_empty(&viewport_clip)) {
        viewport_clip_ptr = &viewport_clip;
      }
    }

    /* Apply preview; work_region is the actual region that was restored + painted.
     * This also eliminates the redundant second call to image_paint_gradient_calc_work_region_uv
     * that previously followed this block. */
    rcti work_region;
    image_paint_gradient_apply_preview_uv(scene,
                                          ima,
                                          tile.tile_number,
                                          ibuf,
                                          tile.undo_ibuf,
                                          params,
                                          state->start_uv,
                                          state->end_uv,
                                          state->midpoint,
                                          viewport_clip_ptr,
                                          work_region);

    if (commit_to_image) {
      BKE_image_mark_dirty(ima, ibuf);
      if (!BLI_rcti_is_empty(&work_region)) {
        const ImageTile *itile = BKE_image_get_tile(ima, tile.tile_number);
        BKE_image_partial_update_mark_region(ima, itile, ibuf, &work_region);
      }
    }
    else {
      /* Preview path: invalidate display buffer and mark partial update so the
       * GPU texture cache picks up the changed pixels, but do NOT mark_dirty. */
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      if (!BLI_rcti_is_empty(&work_region)) {
        const ImageTile *itile = BKE_image_get_tile(ima, tile.tile_number);
        BKE_image_partial_update_mark_region(ima, itile, ibuf, &work_region);
      }
    }

    BKE_image_release_ibuf(ima, ibuf, lock);
  }

  if (commit_to_image) {
    DEG_id_tag_update(&ima->id, 0);
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
  }
  else {
    /* Preview: use NC_IMAGE | NA_EDITED to trigger GPU texture cache refresh.
     * BKE_image_mark_dirty is NOT called, so the image will not be flagged as modified.
     * ID_RECALC_EDITORS is sufficient (lighter than full DEG recalc). */
    DEG_id_tag_update(&ima->id, ID_RECALC_EDITORS);
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
  }
  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
}

static void image_select_gradient_update_preview(bContext *C,
                                                 ImageSelectGradientState *state,
                                                 const bool force)
{
  if (!state || !state->owner_sima) {
    return;
  }

  const bool settings_changed = state->applied_settings_revision !=
                                image_paint_gradient_settings_revision;
  if (!force && !settings_changed) {
    const double now = BLI_time_now_seconds();
    if (now - state->last_preview_time < (1.0 / 60.0)) {
      state->preview_pending = true;
      return;
    }
    state->last_preview_time = now;
  }
  else if (force) {
    state->last_preview_time = BLI_time_now_seconds();
  }

  state->preview_pending = false;
  state->applied_settings_revision = image_paint_gradient_settings_revision;

  const Scene *scene = CTX_data_scene(C);
  /* Interactive (non-forced) updates clip to the viewport for performance.
   * Forced updates (LMB release, settings change) do a full-tile refresh. */
  image_select_gradient_run_preview(
      C, state, image_select_gradient_current_params(scene), false, /*use_viewport_clip=*/!force);
}

void image_select_gradient_refresh_preview_from_settings(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ImageSelectGradientState *state = image_select_gradient_state_get(sima);
  if (!state) {
    return;
  }
  image_select_gradient_update_preview(C, state, true);
}

static void image_select_gradient_restore_session_backup(bContext *C,
                                                         ImageSelectGradientState *state);

static void image_select_gradient_apply_session(bContext *C, ImageSelectGradientState *state);

static void draw_gradient_polyline_circle(const uint pos,
                                          const float center[2],
                                          const float radius,
                                          const int segments)
{
  immBegin(GPU_PRIM_LINE_LOOP, segments);
  for (int i = 0; i < segments; i++) {
    const float angle = (float(i) / float(segments)) * float(2.0 * M_PI);
    immVertex2f(pos, center[0] + cosf(angle) * radius, center[1] + sinf(angle) * radius);
  }
  immEnd();
}

static void draw_gradient_diamond(const uint pos, const float center[2], const float half_size)
{
  immBegin(GPU_PRIM_LINE_LOOP, 4);
  immVertex2f(pos, center[0], center[1] + half_size);
  immVertex2f(pos, center[0] + half_size, center[1]);
  immVertex2f(pos, center[0], center[1] - half_size);
  immVertex2f(pos, center[0] - half_size, center[1]);
  immEnd();
}

static void draw_gradient_vector_overlay(const bContext * /*C*/, ARegion *region, void *arg)
{
  ImageSelectGradientState *state = static_cast<ImageSelectGradientState *>(arg);
  if (!state) {
    return;
  }
  /* Draw handles even when tiles is empty (degenerate/zero-length gradient at drag start). */

  const float2 mid_uv = image_select_gradient_mid_uv(state);

  float p0[2], p1[2], pm[2];
  ui::view2d_view_to_region_fl(
      &region->v2d, state->start_uv.x, state->start_uv.y, &p0[0], &p0[1]);
  ui::view2d_view_to_region_fl(&region->v2d, state->end_uv.x, state->end_uv.y, &p1[0], &p1[1]);
  ui::view2d_view_to_region_fl(&region->v2d, mid_uv.x, mid_uv.y, &pm[0], &pm[1]);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  GPU_line_width(3.0f);
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.55f);
  immBegin(GPU_PRIM_LINES, 2);
  immVertex2f(pos, p0[0], p0[1]);
  immVertex2f(pos, p1[0], p1[1]);
  immEnd();

  GPU_line_width(1.5f);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
  immBegin(GPU_PRIM_LINES, 2);
  immVertex2f(pos, p0[0], p0[1]);
  immVertex2f(pos, p1[0], p1[1]);
  immEnd();

  GPU_line_width(3.0f);
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
  draw_gradient_polyline_circle(pos, p0, 7.0f, 24);
  draw_gradient_polyline_circle(pos, p1, 9.0f, 24);

  GPU_line_width(1.5f);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.95f);
  draw_gradient_polyline_circle(pos, p0, 7.0f, 24);
  immUniformColor4f(1.0f, 0.85f, 0.0f, 0.95f);
  draw_gradient_polyline_circle(pos, p1, 9.0f, 24);

  GPU_line_width(3.0f);
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
  draw_gradient_diamond(pos, pm, 6.0f);
  GPU_line_width(1.5f);
  immUniformColor4f(0.2f, 0.9f, 1.0f, 0.95f);
  draw_gradient_diamond(pos, pm, 6.0f);

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void image_select_gradient_begin_session(bContext *C,
                                                wmOperator * /*op*/,
                                                Image *ima,
                                                SpaceImage *sima,
                                                const int tile_number,
                                                const float2 &start_uv)
{
  if (ImageSelectGradientState *previous = image_select_gradient_state_get(sima)) {
    image_select_session_clear(sima);
    image_select_gradient_restore_session_backup(C, previous);
    image_select_gradient_state_free(previous);
  }

  ImageSelectGradientState *state = MEM_new<ImageSelectGradientState>(__func__);
  state->owner_sima = sima;
  state->start_uv = start_uv;
  state->end_uv = start_uv;
  state->midpoint = 0.5f;
  state->applied_settings_revision = image_paint_gradient_settings_revision;
  /* Remember the owning tool so the modal can commit when the user switches to another one. */
  if (const bToolRef *tref = WM_toolsystem_ref_from_context(C)) {
    STRNCPY(state->owner_tool_idname, tref->idname);
  }

  Scene *scene = CTX_data_scene(C);
  const ImagePaintGradientParams params = image_select_gradient_current_params(scene);
  image_select_gradient_sync_tile_backups(C, state, params);

  if (state->tiles.is_empty()) {
    ImageSelectGradientTileData tile;
    if (image_select_gradient_init_tile_backup(C, ima, sima, tile_number, tile)) {
      state->tiles.append(tile);
    }
  }

  if (state->tiles.is_empty()) {
    image_select_gradient_state_free(state);
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (region && region->runtime->type) {
    state->owner_region_type = region->runtime->type;
    state->draw_handle = ED_region_draw_cb_activate(state->owner_region_type,
                                                    draw_gradient_vector_overlay,
                                                    state,
                                                    REGION_DRAW_POST_PIXEL);
  }

  image_select_session_set(sima, state);
}

static void image_select_gradient_restore_session_backup(bContext *C,
                                                         ImageSelectGradientState *state)
{
  Image *ima = state->owner_sima ? state->owner_sima->image : nullptr;
  if (!ima) {
    return;
  }
  for (const ImageSelectGradientTileData &tile : state->tiles) {
    image_select_gradient_restore_tile_backup(C, ima, tile);
  }
}

void image_select_gradient_session_end_for_takeover(bContext *C, SpaceImage *sima)
{
  ImageSelectGradientState *state = image_select_gradient_state_get(sima);
  if (!state) {
    return;
  }
  /* Committed rather than discarded: switching to another tool with an unconfirmed gradient bakes
   * the preview into the image and pushes a complete undo step, so the visible result is kept. Like
   * the confirm path, this applies before the session slot is cleared. Explicit cancel (ESC) still
   * restores the per-tile backups. */
  image_select_gradient_apply_session(C, state);
  image_select_session_clear(sima);
  image_select_gradient_state_free(state);
}

static void image_select_gradient_apply_session(bContext *C, ImageSelectGradientState *state)
{
  Image *ima = state->owner_sima->image;
  Scene *scene = CTX_data_scene(C);
  const ImagePaintGradientParams params = image_select_gradient_current_params(scene);

  image_select_gradient_run_preview(C, state, params, true, /*use_viewport_clip=*/false);

  if (state->tiles.is_empty()) {
    return;
  }

  ImageUndoStep *us_open = nullptr;
  bool first_undo = true;

  for (const ImageSelectGradientTileData &tile : state->tiles) {
    ImageUser iuser = tile.iuser;
    if (first_undo) {
      ED_image_undo_push_begin_with_image("Gradient", ima, tile.undo_ibuf, &iuser);
      us_open = image_select_undo_session_step_get();
      first_undo = false;
    }
    else if (us_open) {
      ED_image_undo_push(ima, tile.undo_ibuf, &iuser, us_open);
    }
    ED_image_undo_capture_selection_mask(ima, tile.tile_number);
  }
  ED_image_undo_push_end();

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Handle hit-testing
 * \{ */

enum class GradientDragMode {
  Idle = -1,
  New = 0,
  Start,
  End,
  Mid,
};

struct GradientDragData {
  GradientDragMode drag_mode;
  wmTimer *timer = nullptr;
};

static constexpr float GRADIENT_HANDLE_HIT_RADIUS_PX = 15.0f;

static bool image_select_gradient_hit_test_handle(const ARegion *region,
                                                  const wmEvent *event,
                                                  const float2 &handle_uv)
{
  if (!region) {
    return false;
  }

  float rx, ry;
  ui::view2d_view_to_region_fl(&region->v2d, handle_uv.x, handle_uv.y, &rx, &ry);
  /* Window-absolute coordinates: #wmEvent.mval may be relative to another region (e.g. the header)
   * while the modal runs, so it cannot be trusted for hit-testing image-space handles. */
  const float mx = float(event->xy[0] - region->winrct.xmin);
  const float my = float(event->xy[1] - region->winrct.ymin);
  const float dist_sq = (mx - rx) * (mx - rx) + (my - ry) * (my - ry);
  return dist_sq <= GRADIENT_HANDLE_HIT_RADIUS_PX * GRADIENT_HANDLE_HIT_RADIUS_PX;
}

static float image_select_gradient_project_midpoint(const float2 &uv,
                                                    const float2 &start_uv,
                                                    const float2 &end_uv)
{
  const float dx = end_uv.x - start_uv.x;
  const float dy = end_uv.y - start_uv.y;
  const float dist_sq = dx * dx + dy * dy;
  if (dist_sq < 1e-12f) {
    return 0.5f;
  }
  const float t = ((uv.x - start_uv.x) * dx + (uv.y - start_uv.y) * dy) / dist_sq;
  return clamp_f(t, 0.05f, 0.95f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static bool image_select_gradient_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima) {
    return false;
  }
  /* Re-invoke over the gradient already floating in this space: re-anchoring the vector needs no
   * canvas mask, and #image_select_gradient_begin_session restores and replaces the old session.
   * Tested first because #image_paint_selection_poll rejects a floating gradient. */
  if (image_select_gradient_state_get(sima)) {
    return true;
  }
  /* Mode, editability, region *and* "another tool is floating in this editor" all come from the
   * one shared poll, the same way move / transform / warp reach them. This used to be an inline
   * copy that checked transform and warp but not gradient. */
  if (!image_paint_selection_poll(C)) {
    return false;
  }
  return sima->image != nullptr;
}

static bool image_select_gradient_active_poll(bContext *C)
{
  return image_select_gradient_is_floating(C);
}

static wmOperatorStatus image_select_gradient_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  if (!sima || !region) {
    return OPERATOR_CANCELLED;
  }
  Image *ima = sima->image;
  if (!ima) {
    return OPERATOR_CANCELLED;
  }

  image_select_gradient_commit_floating_ops(C);
  image_paint_gradient_ensure_colorband(CTX_data_scene(C)->toolsettings->imapaint);
  image_select_gradient_sync_op_from_imapaint(op, CTX_data_scene(C));

  int tile_number = 0;
  float2 click_uv;
  if (!image_select_gradient_mouse_to_global_uv(region, ima, event, tile_number, click_uv)) {
    return OPERATOR_CANCELLED;
  }

  GradientDragMode drag_mode = GradientDragMode::New;
  float2 start_uv = click_uv;
  float2 end_uv = click_uv;
  float midpoint = 0.5f;

  ImageSelectGradientState *existing = image_select_gradient_state_get(sima);
  if (existing) {
    start_uv = existing->start_uv;
    end_uv = existing->end_uv;
    midpoint = existing->midpoint;
    if (image_select_gradient_hit_test_handle(region, event, existing->start_uv)) {
      drag_mode = GradientDragMode::Start;
    }
    else if (image_select_gradient_hit_test_handle(region, event, existing->end_uv)) {
      drag_mode = GradientDragMode::End;
    }
    else if (image_select_gradient_hit_test_handle(
                 region, event, image_select_gradient_mid_uv(existing)))
    {
      drag_mode = GradientDragMode::Mid;
    }
    else {
      drag_mode = GradientDragMode::New;
      start_uv = click_uv;
      end_uv = click_uv;
      midpoint = 0.5f;
    }
  }

  if (drag_mode == GradientDragMode::New && !existing) {
    image_select_gradient_begin_session(C, op, ima, sima, tile_number, start_uv);
    existing = image_select_gradient_state_get(sima);
    if (!existing) {
      return OPERATOR_CANCELLED;
    }
  }
  else if (existing) {
    existing->start_uv = start_uv;
    existing->end_uv = end_uv;
    existing->midpoint = midpoint;
  }

  existing = image_select_gradient_state_get(sima);
  if (!existing) {
    return OPERATOR_CANCELLED;
  }

  GradientDragData *data = MEM_new<GradientDragData>(__func__);
  data->drag_mode = drag_mode;
  /* When the modal is first entered via invoke with LMB already pressed (New mode),
   * the drag starts immediately. Otherwise (Start/End/Mid) wait for LMB release
   * to transition to Idle, then a subsequent press to begin dragging. */
  op->customdata = data;

  image_select_gradient_update_preview(C, existing, true);
  /* Timer so that panel setting changes (opacity, colors, type) update the preview
   * even when the mouse is stationary. The modal only runs on events, so without a
   * timer the settings_revision check would never fire after a slider change. */
  data->timer = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 1.0 / 30.0);
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  return OPERATOR_RUNNING_MODAL;
}

/**
 * Whether the tool that started the floating gradient is still the active tool. Returns true when
 * the owning tool could not be resolved at session start, so a change we cannot detect never
 * triggers an auto-commit.
 */
static bool image_select_gradient_owner_tool_active(bContext *C,
                                                    const ImageSelectGradientState *state)
{
  if (state->owner_tool_idname[0] == '\0') {
    return true;
  }
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  return tref && STREQ(tref->idname, state->owner_tool_idname);
}

static wmOperatorStatus image_select_gradient_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ImageSelectGradientState *state = image_select_gradient_state_get(sima);
  GradientDragData *data = static_cast<GradientDragData *>(op->customdata);
  if (!state || !data) {
    /* The session can be torn down from under a still-registered modal handler, for instance by
     * #PAINT_OT_image_select_gradient_apply / `_cancel`. Returning #OPERATOR_CANCELLED here does
     * not run `ot->cancel` -- that is only dispatched from the window manager's teardown paths --
     * so the drag data, the 30 FPS timer and the modal cursor must be released explicitly. */
    if (data) {
      if (data->timer) {
        WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
        data->timer = nullptr;
      }
      MEM_delete(data);
      op->customdata = nullptr;
    }
    WM_cursor_modal_restore(CTX_wm_window(C));
    return OPERATOR_CANCELLED;
  }

  /* The modal handler is dispatched before the per-region UI handlers, but the window manager
   * forces the context region to this operator's home region (the image #RGN_TYPE_WINDOW) for the
   * whole callback, so the context cannot reveal where the pointer actually is. Resolve both
   * explicitly: `region` is the image region used for all coordinate math, while
   * `region_under_cursor` -- looked up from the window-absolute event position, honoring
   * overlapping panels -- tells us when the pointer is over the header / tool-settings / N-panel
   * so those clicks can be handed back to the UI instead of being hijacked by the gradient. */
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = area ? BKE_area_find_region_type(area, RGN_TYPE_WINDOW) : nullptr;
  const ARegion *region_under_cursor = area ?
                                           ED_area_find_region_xy_visual(
                                               area, RGN_TYPE_ANY, event->xy) :
                                           nullptr;
  const bool pointer_over_image = region_under_cursor &&
                                  region_under_cursor->regiontype == RGN_TYPE_WINDOW;

  Image *ima = sima ? sima->image : nullptr;
  /* The floating gradient session can outlive the image it was started on (image swapped,
   * closed, or freed). Bail out gracefully instead of dereferencing a null Image in
   * BKE_image_get_tile_from_pos / hit-testing. */
  if (!ima) {
    if (data->timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
      data->timer = nullptr;
    }
    image_select_session_clear(sima);
    image_select_gradient_state_free(state);
    MEM_delete(data);
    op->customdata = nullptr;
    WM_cursor_modal_restore(CTX_wm_window(C));
    return OPERATOR_CANCELLED;
  }

  /* Commit and tear down when the user switches to another tool. Switching the active tool does not
   * cancel a running modal, so without this the gradient's modal -- and its handle overlay -- would
   * keep floating over the newly selected tool. This also runs on the 30 FPS timer, so it fires
   * shortly after a switch even when the pointer is stationary. */
  if (!image_select_gradient_owner_tool_active(C, state)) {
    if (data->timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
      data->timer = nullptr;
    }
    image_select_gradient_apply_session(C, state);
    image_select_session_clear(sima);
    image_select_gradient_state_free(state);
    MEM_delete(data);
    op->customdata = nullptr;
    WM_cursor_modal_restore(CTX_wm_window(C));
    return OPERATOR_FINISHED;
  }

  /* Show the gradient cross-hair only over the image (and while a handle is being dragged), and
   * the standard arrow over the surrounding UI -- header, tool-settings, N-panel -- so hovering a
   * widget looks and feels like normal UI, matching other Blender tools. WM_cursor_set
   * short-circuits when the shape is unchanged, so refreshing it every event is cheap. */
  {
    const bool show_crosshair = (data->drag_mode != GradientDragMode::Idle) || pointer_over_image;
    WM_cursor_modal_set(CTX_wm_window(C),
                        show_crosshair ? WM_CURSOR_CROSS : WM_CURSOR_DEFAULT);
  }

  /* Timer tick: refresh preview if settings changed in the panel without mouse movement. */
  if (event->type == TIMER) {
    if (state->applied_settings_revision != image_paint_gradient_settings_revision ||
        state->preview_pending)
    {
      image_select_gradient_update_preview(C, state, true);
    }
    return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }

  if (state->preview_pending ||
      state->applied_settings_revision != image_paint_gradient_settings_revision)
  {
    image_select_gradient_update_preview(C, state, false);
  }

  /* Confirm / cancel go through the shared floating modal keymap, so the bindings are
   * user-configurable like those of move / transform / warp. Gradient has no undo-step notion, so
   * that item is simply passed on. */
  if (event->type == EVT_MODAL_MAP) {
    if (ELEM(event->val,
             IMAGE_SELECT_FLOATING_MODAL_CANCEL,
             IMAGE_SELECT_FLOATING_MODAL_CONFIRM))
    {
      const bool confirm = event->val == IMAGE_SELECT_FLOATING_MODAL_CONFIRM;
      if (data->timer) {
        WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
        data->timer = nullptr;
      }
      if (confirm) {
        image_select_gradient_apply_session(C, state);
      }
      else {
        image_select_gradient_restore_session_backup(C, state);
      }
      image_select_session_clear(sima);
      image_select_gradient_state_free(state);
      MEM_delete(data);
      op->customdata = nullptr;
      WM_cursor_modal_restore(CTX_wm_window(C));
      return confirm ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
    }
    return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }

  /* While no handle is being dragged, a mouse event over the header / tool-settings / N-panel
   * belongs to the UI. Pass it through (without breaking the event) so its widgets receive the
   * click instead of the gradient hijacking it to start a spurious new vector. An in-progress drag
   * keeps control until its button is released, even if the pointer leaves the image region. */
  if (data->drag_mode == GradientDragMode::Idle && !pointer_over_image &&
      ELEM(event->type, LEFTMOUSE, MOUSEMOVE))
  {
    return OPERATOR_PASS_THROUGH;
  }

  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    if (image_select_gradient_hit_test_handle(region, event, state->start_uv)) {
      data->drag_mode = GradientDragMode::Start;
    }
    else if (image_select_gradient_hit_test_handle(region, event, state->end_uv)) {
      data->drag_mode = GradientDragMode::End;
    }
    else if (image_select_gradient_hit_test_handle(
                 region, event, image_select_gradient_mid_uv(state)))
    {
      data->drag_mode = GradientDragMode::Mid;
    }
    else {
      /* Click on empty space: start a brand-new gradient from this point. */
      data->drag_mode = GradientDragMode::New;
      int tile_number = 0;
      float2 uv;
      if (region &&
          image_select_gradient_mouse_to_global_uv(region, ima, event, tile_number, uv))
      {
        state->start_uv = uv;
        state->end_uv = uv;
        state->midpoint = 0.5f;
      }
    }
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == MOUSEMOVE || (event->type == LEFTMOUSE && event->val == KM_RELEASE)) {
    /* Only update UVs while a drag is actually in progress (not Idle). */
    if (data->drag_mode != GradientDragMode::Idle) {
      int tile_number = 0;
      float2 uv;
      if (region && image_select_gradient_mouse_to_global_uv(region, ima, event, tile_number, uv))
      {
        switch (data->drag_mode) {
          case GradientDragMode::Start:
            state->start_uv = uv;
            break;
          case GradientDragMode::Mid:
            state->midpoint = image_select_gradient_project_midpoint(
                uv, state->start_uv, state->end_uv);
            break;
          case GradientDragMode::End:
          case GradientDragMode::New:
          case GradientDragMode::Idle:
          default:
            state->end_uv = uv;
            break;
        }
        image_select_gradient_update_preview(C, state, event->type == LEFTMOUSE);
      }
    }

    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
      /* End the active drag; transition to Idle so MOUSEMOVE no longer moves handles. */
      data->drag_mode = GradientDragMode::Idle;
      if (state->preview_pending) {
        image_select_gradient_update_preview(C, state, true);
      }
      return OPERATOR_RUNNING_MODAL;
    }

    if (event->type == MOUSEMOVE && data->drag_mode == GradientDragMode::Idle) {
      /* Idle mouse-move must pass through so middle-mouse / trackpad view-pan reaches the
       * area/region keymap handlers. Returning RUNNING_MODAL would set WM_HANDLER_BREAK
       * and swallow the pan gesture. */
      return OPERATOR_PASS_THROUGH;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  /* All other events (wheel zoom, middle-mouse pan, trackpad gestures, modifier keys, etc.).
   * Must be plain PASS_THROUGH -- combining with RUNNING_MODAL sets WM_HANDLER_BREAK which
   * prevents area/region keymap handlers (image.view_zoom, image.view_pan) from receiving
   * the event. This is what unblocked zoom/pan for the move/transform selection operators. */
  return OPERATOR_PASS_THROUGH;
}

static void image_select_gradient_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata) {
    GradientDragData *data = static_cast<GradientDragData *>(op->customdata);
    if (data->timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
      data->timer = nullptr;
    }
    MEM_delete(data);
    op->customdata = nullptr;
  }
  SpaceImage *sima = CTX_wm_space_image(C);
  if (ImageSelectGradientState *state = image_select_gradient_state_get(sima)) {
    image_select_session_clear(sima);
    image_select_gradient_restore_session_backup(C, state);
    image_select_gradient_state_free(state);
  }
  WM_cursor_modal_restore(CTX_wm_window(C));
}

static wmOperatorStatus image_select_gradient_apply_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectGradientState *state = image_select_gradient_state_get(sima);
  if (!state) {
    return OPERATOR_CANCELLED;
  }
  image_select_session_clear(sima);
  image_select_gradient_apply_session(C, state);
  image_select_gradient_state_free(state);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_gradient_cancel_exec(bContext *C, wmOperator *op)
{
  image_select_gradient_cancel(C, op);
  return OPERATOR_CANCELLED;
}

void PAINT_OT_image_select_gradient(wmOperatorType *ot)
{
  static const EnumPropertyItem gradient_type_items[] = {
      {IMAGE_PAINT_GRADIENT_LINEAR, "LINEAR", 0, "Linear", ""},
      {IMAGE_PAINT_GRADIENT_RADIAL, "RADIAL", 0, "Radial", ""},
      {IMAGE_PAINT_GRADIENT_CONICAL, "CONICAL", 0, "Conical", ""},
      {IMAGE_PAINT_GRADIENT_DIAMOND, "DIAMOND", 0, "Diamond", ""},
      {IMAGE_PAINT_GRADIENT_SQUARE, "SQUARE", 0, "Square", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem repeat_items[] = {
      {IMAGE_PAINT_GRADIENT_REPEAT_NONE, "NONE", 0, "None", "Clamp colors at the ends"},
      {IMAGE_PAINT_GRADIENT_REPEAT_REPEAT, "REPEAT", 0, "Repeat", "Tile the gradient"},
      {IMAGE_PAINT_GRADIENT_REPEAT_REFLECT, "REFLECT", 0, "Reflect", "Mirror the gradient"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem blend_items[] = {
      {IMB_BLEND_MIX, "MIX", 0, "Mix", ""},
      {IMB_BLEND_MUL, "MUL", 0, "Multiply", ""},
      {IMB_BLEND_ADD, "ADD", 0, "Add", ""},
      {IMB_BLEND_SUB, "SUB", 0, "Subtract", ""},
      {IMB_BLEND_OVERLAY, "OVERLAY", 0, "Overlay", ""},
      {IMB_BLEND_SCREEN, "SCREEN", 0, "Screen", ""},
      {IMB_BLEND_DARKEN, "DARKEN", 0, "Darken", ""},
      {IMB_BLEND_LIGHTEN, "LIGHTEN", 0, "Lighten", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Gradient Selection";
  ot->idname = "PAINT_OT_image_select_gradient";
  ot->description = "Paint a gradient inside the selection mask";

  ot->invoke = image_select_gradient_invoke;
  ot->modal = image_select_gradient_modal;
  ot->cancel = image_select_gradient_cancel;
  ot->poll = image_select_gradient_poll;

  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop;
  prop = RNA_def_enum(ot->srna, "gradient_type", gradient_type_items, 0, "Type", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_enum(ot->srna, "repeat", repeat_items, 0, "Repeat", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_enum(ot->srna, "blend_mode", blend_items, 0, "Blend", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_float(ot->srna, "opacity", 1.0f, 0.0f, 1.0f, "Opacity", "", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

void PAINT_OT_image_select_gradient_apply(wmOperatorType *ot)
{
  ot->name = "Apply Gradient";
  ot->idname = "PAINT_OT_image_select_gradient_apply";
  ot->description = "Commit the gradient preview to the image";
  ot->exec = image_select_gradient_apply_exec;
  ot->poll = image_select_gradient_active_poll;
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_gradient_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Gradient";
  ot->idname = "PAINT_OT_image_select_gradient_cancel";
  ot->description = "Discard the gradient preview";
  ot->exec = image_select_gradient_cancel_exec;
  ot->cancel = image_select_gradient_cancel;
  ot->poll = image_select_gradient_active_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

} /* namespace blender */

